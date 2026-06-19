/*
gl_entshadow.c - dynamic entity shadows (Continuum "improved map lighting", stage 1)
Copyright (C) 2026

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

----------------------------------------------------------------------------
Moving studio entities (monsters, props, the player) cast a soft shadow into the
otherwise-untouched baked world. The static map lighting stays baked - this only
adds the one thing baked lighting can't: a shadow from things that move.

Each caster is lit from its own dominant baked light direction (sampled per entity
by R_EntityDynamicLight, exactly the direction the studio renderer shades it with),
so the shadow falls the way the room's lighting implies.

Technique (same family as the AO contact shadow): for each caster we CPU-rasterize
its posed silhouette - projected along the light direction - into a small coverage
bitmap, box-blur it (a real, wide, adjustable blur, unlike a 1-texel hardware PCF),
and project that soft footprint onto the world + brush surfaces as a DARKENING pass.
We traded the previous shadow-map's per-pixel occlusion for this smooth blur (the
engine has no FBO/post-process to blur a shadow-map result); to keep it sane we
project only onto surfaces on the shadowed side of the caster (so it doesn't paint
the ceiling above it). Mostly a contact shadow, so the occlusion loss rarely shows.

Default-off; gated by r_entity_shadows. Entity-on-entity shadows are deferred (the
receiver pass lights world + brush surfaces only) - a TODO to revisit.
*/

#include "gl_local.h"
#include "xash3d_mathlib.h"
#include "pm_defs.h"	// PM_STUDIO_IGNORE etc. for the supporting-surface ground trace

CVAR_DEFINE_AUTO( r_entity_shadows, "1", FCVAR_ARCHIVE, "dynamic shadows cast by entities (monsters/props/player) onto the world" );
CVAR_DEFINE_AUTO( r_entity_shadows_max, "16", FCVAR_ARCHIVE, "max number of nearest entities that cast a shadow (performance cap)" );
CVAR_DEFINE_AUTO( r_entity_shadows_player, "1", FCVAR_ARCHIVE, "the player (and other players) cast entity shadows" );
CVAR_DEFINE_AUTO( r_entity_shadows_strength, "0.4", FCVAR_ARCHIVE, "how dark entity shadows are (0 = none .. 1 = black)" );
CVAR_DEFINE_AUTO( r_entity_shadows_size, "256", FCVAR_ARCHIVE, "entity shadow coverage-map resolution in texels (square); higher = finer footprint, more CPU" );
CVAR_DEFINE_AUTO( r_entity_shadows_softness, "6", FCVAR_ARCHIVE, "soften the shadow edge: box-blur radius in coverage texels (0 = hard)" );
CVAR_DEFINE_AUTO( r_entity_shadows_floor, "16", FCVAR_ARCHIVE, "drop the shadow from a floor-like surface sitting more than this many units below the ground the caster stands on (stops a duplicate on the floor under a ramp/platform; walls still receive). 0 = no limit" );
CVAR_DEFINE_AUTO( r_entity_shadows_smooth, "0.25", FCVAR_ARCHIVE, "ease the shadow direction over this many seconds so it doesn't snap when the dominant light changes (0 = instant)" );
CVAR_DEFINE_AUTO( r_entity_shadows_flashlight, "1", FCVAR_ARCHIVE, "the flashlight beam cancels (overpowers) entity shadows where it shines" );
CVAR_DEFINE_AUTO( r_entity_shadows_debug, "0", 0, "draw entity shadow footprints in bright yellow (and ignore strength)" );

#define ES_HARD_MAX	50	// absolute ceiling on casters per frame (matches r_entity_shadows_max max)
#define ES_COV_MIN	64	// coverage-map resolution bounds (square texels). small: it's CPU-rasterized
#define ES_COV_MAX	1024	// and box-blurred every frame per caster (CPU; cost ~ res^2, so capped)
#define ES_SOFT_MAX	24	// box-blur radius cap (texels)
#define ES_NEAR		1.0f	// light near plane (units)

typedef struct
{
	int	covtex;		// this caster's soft coverage texture
	float	texmat[16];	// bias * ortho * view : world -> coverage [0,1]^2 (+ depth in r)
	vec3_t	center;		// receiver-cull sphere centre (world)
	float	influence;	// receiver-cull sphere radius (world units)
	vec3_t	cnorm;		// contact ground plane normal (world), from the solid down-trace
	float	cdist;		// contact ground plane distance (DotProduct( p, cnorm ) == cdist on it)
	qboolean hasGround;	// the down-trace found a usable ground plane -> plane-lock is active
	vec3_t	ldir;		// light travel direction (receivers must face into it)
} es_caster_t;

static es_caster_t	es_casters[ES_HARD_MAX];
static int		es_count;		// casters prepared this frame
static int		es_covtex[ES_HARD_MAX];	// persistent coverage-texture pool
static int		es_covtex_size;		// side length the pool was allocated at

// CPU coverage scratch (reused per caster)
static byte	es_cov[ES_COV_MAX * ES_COV_MAX];
static byte	es_tmp[ES_COV_MAX * ES_COV_MAX];
static byte	es_rgba[ES_COV_MAX * ES_COV_MAX * 4];

// per-entity smoothed shadow direction, keyed by cl_entity index. The raw direction
// (R_EntityDynamicLight's dominant baked light) snaps when a caster crosses between two
// lights; easing it across frames turns that snap into a quick glide and averages a
// flickering midpoint to a stable in-between, so the shadow no longer pops.
#define ES_SMOOTH_MAX	8192	// covers any sane entity index; out-of-range -> unsmoothed
typedef struct
{
	vec3_t	dir;	// last smoothed direction (normalised)
	double	time;	// when it was last updated (gp_cl->time)
	qboolean valid;
} es_dirsmooth_t;
static es_dirsmooth_t	es_sdir[ES_SMOOTH_MAX];

// ease `dir` (in: fresh normalised sample; out: smoothed) toward its cached value for this
// entity. Reseeds (snaps) on first sight or after a gap, so an entity that just appeared or
// teleported doesn't slew its shadow across the world.
static void R_EntityShadowSmoothDir( int index, vec3_t dir )
{
	const float tau = r_entity_shadows_smooth.value;
	es_dirsmooth_t *s;
	double now = gp_cl->time;
	float dt, alpha;

	if( tau <= 0.0f || index < 0 || index >= ES_SMOOTH_MAX )
		return;	// smoothing off, or index we can't cache -> use the raw sample

	s = &es_sdir[index];

	// first sight, time ran backwards (map/level reload), or not seen recently -> snap
	if( !s->valid || now < s->time || ( now - s->time ) > 0.5 )
	{
		VectorCopy( dir, s->dir );
		s->valid = true;
		s->time = now;
		return;
	}

	dt = (float)( now - s->time );
	s->time = now;

	alpha = 1.0f - (float)exp( -dt / tau );	// frame-rate-independent exponential approach
	alpha = bound( 0.0f, alpha, 1.0f );

	VectorLerp( s->dir, alpha, dir, s->dir );	// s->dir += alpha * (sample - s->dir)
	if( VectorLength( s->dir ) < 0.001f )
		VectorCopy( dir, s->dir );		// degenerate (opposed dirs) -> take the sample
	VectorNormalize( s->dir );

	VectorCopy( s->dir, dir );
}

/*
=================
Mat4 helpers (GL column-major, index = col*4 + row); same convention as the flashlight
=================
*/
static void Mat4_Mult( float *out, const float *a, const float *b )
{
	float r[16];
	int col, row, k;

	for( col = 0; col < 4; col++ )
	{
		for( row = 0; row < 4; row++ )
		{
			float sum = 0.0f;
			for( k = 0; k < 4; k++ )
				sum += a[k * 4 + row] * b[col * 4 + k];
			r[col * 4 + row] = sum;
		}
	}
	memcpy( out, r, sizeof( r ));
}

static void Mat4_Ortho( float *m, float l, float r, float b, float t, float zn, float zf )
{
	memset( m, 0, sizeof( float ) * 16 );
	m[0]  = 2.0f / ( r - l );
	m[5]  = 2.0f / ( t - b );
	m[10] = -2.0f / ( zf - zn );
	m[12] = -( r + l ) / ( r - l );
	m[13] = -( t + b ) / ( t - b );
	m[14] = -( zf + zn ) / ( zf - zn );
	m[15] = 1.0f;
}

static void Mat4_LookAt( float *m, const vec3_t eye, const vec3_t fwd, const vec3_t upHint )
{
	vec3_t f, s, u;

	VectorCopy( fwd, f );
	VectorNormalize( f );
	CrossProduct( f, upHint, s );
	VectorNormalize( s );
	CrossProduct( s, f, u );

	m[0] = s[0]; m[4] = s[1]; m[8]  = s[2];  m[12] = -DotProduct( s, eye );
	m[1] = u[0]; m[5] = u[1]; m[9]  = u[2];  m[13] = -DotProduct( u, eye );
	m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2]; m[14] = DotProduct( f, eye );
	m[3] = 0.0f; m[7] = 0.0f; m[11] = 0.0f;  m[15] = 1.0f;
}

// separable box blur of the coverage bitmap (texture-space, so it genuinely smooths -
// this is the soft edge). Same idea as the AO stamp blur.
static void ES_BoxBlur( int size, int radius )
{
	int x, y, d;

	if( radius < 1 )
		return;

	for( y = 0; y < size; y++ )
	{
		for( x = 0; x < size; x++ )
		{
			int sum = 0, n = 0;
			for( d = -radius; d <= radius; d++ )
			{
				int xx = x + d;
				if( xx < 0 || xx >= size ) continue;
				sum += es_cov[y * size + xx]; n++;
			}
			es_tmp[y * size + x] = (byte)( sum / n );
		}
	}
	for( y = 0; y < size; y++ )
	{
		for( x = 0; x < size; x++ )
		{
			int sum = 0, n = 0;
			for( d = -radius; d <= radius; d++ )
			{
				int yy = y + d;
				if( yy < 0 || yy >= size ) continue;
				sum += es_tmp[yy * size + x]; n++;
			}
			es_cov[y * size + x] = (byte)( sum / n );
		}
	}
}

void R_InitEntityShadows( void )
{
	gEngfuncs.Cvar_RegisterVariable( &r_entity_shadows );
	gEngfuncs.Cvar_RegisterVariable( &r_entity_shadows_max );
	gEngfuncs.Cvar_RegisterVariable( &r_entity_shadows_player );
	gEngfuncs.Cvar_RegisterVariable( &r_entity_shadows_strength );
	gEngfuncs.Cvar_RegisterVariable( &r_entity_shadows_size );
	gEngfuncs.Cvar_RegisterVariable( &r_entity_shadows_softness );
	gEngfuncs.Cvar_RegisterVariable( &r_entity_shadows_floor );
	gEngfuncs.Cvar_RegisterVariable( &r_entity_shadows_smooth );
	gEngfuncs.Cvar_RegisterVariable( &r_entity_shadows_flashlight );
	gEngfuncs.Cvar_RegisterVariable( &r_entity_shadows_debug );
}

static qboolean R_EntityShadowsActive( void )
{
	if( !r_entity_shadows.value )
		return false;
	if( !WORLDMODEL || !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
		return false;
	return true;
}

/*
=================
R_EntityShadowCaster

does this studio entity cast? opaque studio models only; the viewmodel and
attached (weapon) models are skipped; players are gated by r_entity_shadows_player.
=================
*/
static qboolean R_EntityShadowCaster( cl_entity_t *e )
{
	if( !e->model || e->model->type != mod_studio )
		return false;
	if( e == tr.viewent )
		return false;
	if( e->curstate.movetype == MOVETYPE_FOLLOW )
		return false;	// rides its parent (weapons etc.)
	if( FBitSet( e->curstate.effects, EF_NOSHADOW ))
		return false;
	if( !R_ModelOpaque( e->curstate.rendermode ))
		return false;
	if( e->player && !r_entity_shadows_player.value )
		return false;
	return true;
}

/*
=================
R_EntityShadowSetupOrtho

orthographic projection (perpendicular to the light dir L) + receiver-cull sphere for
one caster. The shadow is cast along +L, so the projection looks along +L from behind
the model. Fills eye/up/extent/near/far for the matrices and the cull sphere.
=================
*/
static void R_EntityShadowSetupOrtho( cl_entity_t *e, const vec3_t L, const vec3_t spot,
	vec3_t eye, vec3_t up, float *zfar, vec3_t center, float *influence )
{
	model_t	*m = e->model;
	vec3_t	size, c;
	float	radius;
	int	i;

	for( i = 0; i < 3; i++ )
	{
		c[i] = e->origin[i] + 0.5f * ( m->mins[i] + m->maxs[i] );
		size[i] = m->maxs[i] - m->mins[i];
	}
	radius = 0.5f * VectorLength( size );
	if( radius < 8.0f ) radius = 8.0f;

	// the shadow reaches from the model down to the ground (spot) and a little beyond
	{
		float drop = c[2] - spot[2];
		if( drop < 0.0f ) drop = 0.0f;
		*influence = radius + drop + 96.0f;
	}

	VectorCopy( c, center );
	VectorMA( c, -( radius * 2.0f + 8.0f ), L, eye );	// directional light behind the model

	if( fabs( L[2] ) > 0.95f )
		VectorSet( up, 1.0f, 0.0f, 0.0f );
	else
		VectorSet( up, 0.0f, 0.0f, 1.0f );

	// far plane: behind-distance + model + reach. The perpendicular extent (l/r/b/t) is
	// FITTED to the posed silhouette by the caller (R_StudioShadowBounds), not set here.
	*zfar = ( radius * 2.0f + 8.0f ) + radius + *influence;
}

/*
=================
R_EntityShadowPrepare

compute one caster's light direction, projection (texmat), cull sphere and soft
coverage texture. Returns false if the caster produced no footprint.
=================
*/
static qboolean R_EntityShadowPrepare( es_caster_t *c, cl_entity_t *e, int cov, int blur )
{
	alight_t al;
	vec3_t	dir, spot, lvec, L, eye, up;
	float	zfar, view[16], proj[16], tmp[16], bias[16];
	float	bmin[2], bmax[2], margin;
	int	i, x, y;

	// dominant baked light direction at this entity (same as its studio shading)
	al.plightvec = dir;
	VectorSet( dir, 0.0f, 0.0f, -1.0f );
	VectorClear( lvec );
	VectorCopy( e->origin, spot );
	R_EntityDynamicLight( e, &al, true, gp_cl->time, spot, lvec );

	// R_EntityDynamicLight's `spot` comes from a LIGHTMAP down-trace, which can
	// punch through a thin platform and report the floor far below it. That floor
	// then balloons the receiver cull sphere (influence = radius + drop + ...) until
	// it swallows the lower floor, and the silhouette gets projected down onto it
	// (the intro tram-ride bug: shadow lands on the floor under a raised platform).
	// Find the REAL supporting surface with a SOLID collision trace - world + brush
	// entities, studio models ignored - straight down from the feet, exactly like the
	// AO contact-shadow floor finder (gl_ao.c). Use it both to size the cull sphere
	// (spot) and to lock the shadow to that one ground plane (cnorm/cdist below), so a
	// second surface layered under it can never receive a copy. Keep the lightmap spot
	// only when nothing solid is below (e.g. over a pit).
	c->hasGround = false;
	{
		vec3_t	src, end;
		pmtrace_t tr;

		// start at the model's vertical centre, not just above the feet: if the feet
		// penetrate the surface (some monsters sink a few units into a ramp), a feet-high
		// start sits INSIDE the brush and the trace returns startsolid - which would make
		// us bail and lose both the cull-sphere sizing and the plane-lock for that one
		// entity (its shadow then doubles and over-reaches). PM_STUDIO_IGNORE skips this
		// model's own body, so starting mid-height is safe and clears minor penetration.
		src[0] = e->origin[0];
		src[1] = e->origin[1];
		src[2] = e->origin[2] + 0.5f * ( e->model->mins[2] + e->model->maxs[2] );
		VectorCopy( src, end );
		end[2] -= 2048.0f;

		tr = gEngfuncs.CL_TraceLine( src, end, PM_STUDIO_IGNORE );
		if( !tr.startsolid && !tr.allsolid && tr.fraction < 1.0f && tr.plane.normal[2] > 0.5f )
		{
			spot[2] = tr.endpos[2];	// the surface the entity actually stands on
			VectorCopy( tr.plane.normal, c->cnorm );
			c->cdist = tr.plane.dist;
			c->hasGround = true;
		}
	}

	VectorCopy( dir, L );
	if( VectorLength( L ) < 0.001f )
		VectorSet( L, 0.0f, 0.0f, -1.0f );
	VectorNormalize( L );

	// ease the direction across frames so it doesn't snap when the dominant light changes
	R_EntityShadowSmoothDir( e->index, L );

	R_EntityShadowSetupOrtho( e, L, spot, eye, up, &zfar, c->center, &c->influence );
	Mat4_LookAt( view, eye, L, up );

	// FIT the projection box to the actual posed silhouette (measured in light view
	// space), so an outstretched limb never spills past the coverage edge and clamp-
	// smears the shadow to infinity. Expand by a margin so the blurred edge fades to 0
	// inside the texture.
	if( !R_StudioShadowBounds( e, view, bmin, bmax ))
		return false;

	margin = Q_max( bmax[0] - bmin[0], bmax[1] - bmin[1] ) * 0.18f + 4.0f;
	bmin[0] -= margin; bmin[1] -= margin;
	bmax[0] += margin; bmax[1] += margin;
	if( bmax[0] - bmin[0] < 1.0f || bmax[1] - bmin[1] < 1.0f )
		return false;

	// bias: clip [-1,1] -> [0,1]
	memset( bias, 0, sizeof( bias ));
	bias[0] = bias[5] = bias[10] = 0.5f;
	bias[12] = bias[13] = bias[14] = 0.5f;
	bias[15] = 1.0f;

	Mat4_Ortho( proj, bmin[0], bmax[0], bmin[1], bmax[1], ES_NEAR, zfar );
	Mat4_Mult( tmp, proj, view );
	Mat4_Mult( c->texmat, bias, tmp );

	VectorCopy( L, c->ldir );

	// CPU-rasterize the posed silhouette into the coverage bitmap, then blur it
	memset( es_cov, 0, cov * cov );
	if( !R_StudioStampShadow( e, c->texmat, es_cov, cov ))
		return false;
	ES_BoxBlur( cov, blur );

	// force a zero border ring: with the fit + margin the silhouette already sits inside
	// the bitmap, but this guarantees the clamped edge texel is 0, so a fragment that
	// projects outside [0,1] can never sample a non-zero occluder (no infinite smear).
	for( x = 0; x < cov; x++ )
	{
		es_cov[x] = 0;
		es_cov[( cov - 1 ) * cov + x] = 0;
	}
	for( y = 0; y < cov; y++ )
	{
		es_cov[y * cov] = 0;
		es_cov[y * cov + ( cov - 1 )] = 0;
	}

	for( i = 0; i < cov * cov; i++ )
	{
		es_rgba[i * 4 + 0] = es_cov[i];
		es_rgba[i * 4 + 1] = es_cov[i];
		es_rgba[i * 4 + 2] = es_cov[i];
		es_rgba[i * 4 + 3] = es_cov[i];
	}

	// per-slot texture, updated in place across frames (TF_UPDATE matches by name)
	{
		char name[32];

		Q_snprintf( name, sizeof( name ), "*entshadow%i", es_count );
		es_covtex[es_count] = GL_CreateTexture( name, cov, cov, es_rgba,
			TF_NOMIPMAP | TF_CLAMP | TF_HAS_ALPHA | ( es_covtex[es_count] ? TF_UPDATE : 0 ));
	}
	c->covtex = es_covtex[es_count];
	return c->covtex != 0;
}

/*
=================
R_EntityShadowProjUnit

bind one caster's soft coverage texture for projective sampling. GL_LINEAR for a
smooth result (the bitmap is already blurred); MODULATE so the primary colour scales
it. The texgen + texture matrix project a world vertex into the coverage map.
=================
*/
static void R_EntityShadowProjUnit( const es_caster_t *c )
{
	static const float planeS[4] = { 1, 0, 0, 0 };
	static const float planeT[4] = { 0, 1, 0, 0 };
	static const float planeR[4] = { 0, 0, 1, 0 };
	static const float planeQ[4] = { 0, 0, 0, 1 };

	GL_SelectTexture( 0 );
	GL_Bind( 0, c->covtex );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	pglTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );

	pglTexGenfv( GL_S, GL_OBJECT_PLANE, planeS );
	pglTexGenfv( GL_T, GL_OBJECT_PLANE, planeT );
	pglTexGenfv( GL_R, GL_OBJECT_PLANE, planeR );
	pglTexGenfv( GL_Q, GL_OBJECT_PLANE, planeQ );
	GL_TexGen( GL_S, GL_OBJECT_LINEAR );
	GL_TexGen( GL_T, GL_OBJECT_LINEAR );
	GL_TexGen( GL_R, GL_OBJECT_LINEAR );
	GL_TexGen( GL_Q, GL_OBJECT_LINEAR );
	GL_LoadTexMatrixExt( c->texmat );

	// flashlight overpowers the shadow: a second TMU samples the inverted flashlight
	// cookie (MODULATE), so the darkening term s*cov is multiplied by (1-cookie) - it
	// fades to 0 in the bright beam (cancelling the shadow regardless of strength) and is
	// untouched outside the cone. No-op when the projected flashlight isn't active.
	if( r_entity_shadows_flashlight.value && !r_entity_shadows_debug.value )
		R_FlashlightSuppressUnit( 1 );
}

/*
=================
R_EntityShadowReceiverSurf

submit one receiver surface (world or brush entity). `obj` NULL => verts already in
world space; otherwise transformed by it. The shadow shape comes from the projected
coverage map - the surface just supplies geometry to project onto.
=================
*/
static void R_EntityShadowReceiverSurf( msurface_t *surf, const matrix4x4 obj )
{
	glpoly2_t *p = surf->polys;
	int v;

	if( !p )
		return;
	if( FBitSet( surf->flags, SURF_DRAWSKY | SURF_DRAWTURB | SURF_DRAWTURB_QUADS | SURF_DRAWTILED ))
		return;

	for( ; p; p = p->next )
	{
		float *vert = p->verts[0];
		pglBegin( GL_POLYGON );
		for( v = 0; v < p->numverts; v++, vert += VERTEXSIZE )
		{
			if( obj )
			{
				vec3_t world;
				Matrix4x4_VectorTransform( obj, vert, world );
				pglVertex3fv( world );
			}
			else pglVertex3fv( vert );
		}
		pglEnd();
	}
}

// receiver-plane filter: which surfaces may receive this caster's shadow. Walls and other
// surfaces tilted away from the ground the caster stands on are ALWAYS allowed - that's what
// lets a shadow climb the wall a caster faces (e.g. typing at a wall-mounted pin-pad). What
// we reject is a FLOOR-LIKE surface (roughly sharing the ground's up direction) that sits
// well BELOW the contact ground: that's a separate lower layer - the floor under a ramp or a
// raised platform - and letting the silhouette extrude down onto it prints a duplicate
// shadow. The "below the ground", not "different plane", distinction is the key: it keeps the
// ramp/platform fix without killing walls (which are also a different plane). snorm is the
// surface's world-space front-oriented normal; scenter a representative world point on it.
static qboolean R_EntityShadowReceiverAllowed( const es_caster_t *c, const vec3_t snorm, const vec3_t scenter )
{
	float below;

	if( !c->hasGround || r_entity_shadows_floor.value <= 0.0f )
		return true;	// no ground found this frame, or the filter is disabled

	// only floor-like surfaces can be a lower layer; walls/steep faces never are
	if( DotProduct( snorm, c->cnorm ) < 0.7f )
		return true;

	below = c->cdist - DotProduct( scenter, c->cnorm );	// units this surface sits below the ground plane
	if( below > r_entity_shadows_floor.value )
		return false;	// a separate floor beneath the one the caster stands on

	return true;
}

// receiver accepted if its bounding sphere overlaps the cull sphere, it FACES the light, and
// it isn't a floor layered below the caster's ground. The facing test (not a per-face depth
// test) is what keeps the shadow off the ceiling without skipping whole floor faces: a floor
// always faces an overhead light, so every floor face receives - seamless across BSP splits.
static qboolean R_EntityShadowSurfReceives( msurface_t *surf, const es_caster_t *c )
{
	mextrasurf_t *info = surf->info;
	vec3_t center, ext, delta, n;
	float sr;

	VectorAverage( info->mins, info->maxs, center );
	VectorSubtract( center, c->center, delta );
	VectorSubtract( info->maxs, center, ext );
	sr = c->influence + VectorLength( ext );
	if( DotProduct( delta, delta ) > sr * sr )
		return false;

	// front-oriented normal of this surface (world space)
	VectorCopy( surf->plane->normal, n );
	if( FBitSet( surf->flags, SURF_PLANEBACK ))
		VectorNegate( n, n );

	if( DotProduct( n, c->ldir ) >= -0.01f )
		return false;	// faces away from / perpendicular to the light (e.g. the ceiling)

	if( !R_EntityShadowReceiverAllowed( c, n, center ))
		return false;	// a floor layered below the ground the caster stands on

	return true;
}

/*
=================
R_EntityShadowReceivers

darken world + brush-entity surfaces within one caster's reach with its soft footprint.
Entity-on-entity shadows are deferred (studio receivers intentionally skipped - TODO).
=================
*/
static void R_EntityShadowReceivers( const es_caster_t *c )
{
	model_t	*world = WORLDMODEL;
	int	i, li;

	R_EntityShadowProjUnit( c );

	for( i = world->firstmodelsurface; i < world->firstmodelsurface + world->nummodelsurfaces; i++ )
	{
		msurface_t *surf = &world->surfaces[i];

		if( !surf->polys )
			continue;
		if( !R_EntityShadowSurfReceives( surf, c ))
			continue;
		R_EntityShadowReceiverSurf( surf, NULL );
	}

	if( tr.draw_list )
	{
		cl_entity_t **lists[2] = { tr.draw_list->solid_entities, tr.draw_list->trans_entities };
		int counts[2] = { tr.draw_list->num_solid_entities, tr.draw_list->num_trans_entities };

		for( li = 0; li < 2; li++ )
		{
			for( i = 0; i < counts[li]; i++ )
			{
				cl_entity_t *ent = lists[li][i];
				matrix4x4 obj;
				vec3_t center, size, delta;
				float sr;
				int rm, s;
				model_t *m;

				if( !ent->model || ent->model->type != mod_brush )
					continue;
				rm = R_GetEntityRenderMode( ent );
				if( rm != kRenderNormal && rm != kRenderTransAlpha )
					continue;

				m = ent->model;
				VectorAverage( m->mins, m->maxs, center );
				VectorAdd( center, ent->origin, center );
				VectorSubtract( center, c->center, delta );
				VectorSubtract( m->maxs, m->mins, size );
				sr = c->influence + 0.5f * VectorLength( size );
				if( DotProduct( delta, delta ) > sr * sr )
					continue;

				Matrix4x4_CreateFromEntity( obj, ent->angles, ent->origin, 1.0f );
				for( s = 0; s < m->nummodelsurfaces; s++ )
				{
					msurface_t *bsurf = &m->surfaces[m->firstmodelsurface + s];
					vec3_t bn, bc;

					// same receiver filter as the world surfaces. Brush bounds/plane are
					// model-local; lift the normal and the surface centre to world space.
					// Brush floors aren't rotated, so the normal is unchanged and the centre
					// just shifts by the entity origin.
					VectorCopy( bsurf->plane->normal, bn );
					if( FBitSet( bsurf->flags, SURF_PLANEBACK ))
						VectorNegate( bn, bn );

					VectorAverage( bsurf->info->mins, bsurf->info->maxs, bc );
					VectorAdd( bc, ent->origin, bc );

					if( !R_EntityShadowReceiverAllowed( c, bn, bc ))
						continue;
					R_EntityShadowReceiverSurf( bsurf, obj );
				}
			}
		}
	}
}

/*
=================
R_DrawEntityShadows

end of frame (opaque scene already in the depth buffer): pick the nearest N casters,
build each one's soft coverage footprint, and project them onto the world as a
multiplicative darkening.

darkening per fragment = fb * ( 1 - s * cov ), where cov is the soft coverage [0,1]
and s = strength. MODULATE( primary=(s,s,s), coverage ) gives src = s*cov, and blend
GL_ZERO, GL_ONE_MINUS_SRC_COLOR multiplies the framebuffer by (1 - s*cov). MODULATE
only, so it rides the gl2_shim like the flashlight.

debug: primary = yellow and additive blend, so shadowed fragments light up bright
yellow (strength ignored) - makes the footprint, projection and culling visible.
=================
*/
void R_DrawEntityShadows( void )
{
	struct { cl_entity_t *e; float d2; } cand[256];
	int	ncand = 0;
	int	want, cov, blur, i, n;
	qboolean dbg;
	float	s;

	es_count = 0;

	if( !R_EntityShadowsActive() || !tr.draw_list )
		return;

	dbg = r_entity_shadows_debug.value != 0.0f;
	s = bound( 0.0f, r_entity_shadows_strength.value, 1.0f );
	if( !dbg && s <= 0.003f )
		return;

	// gather candidate casters, nearest-first
	for( i = 0; i < tr.draw_list->num_solid_entities && ncand < 256; i++ )
	{
		cl_entity_t *e = tr.draw_list->solid_entities[i];
		vec3_t delta;

		if( !R_EntityShadowCaster( e ))
			continue;
		VectorSubtract( e->origin, RI.rvp.vieworigin, delta );
		cand[ncand].e = e;
		cand[ncand].d2 = DotProduct( delta, delta );
		ncand++;
	}
	if( !ncand )
		return;

	want = (int)bound( 2.0f, r_entity_shadows_max.value, (float)ES_HARD_MAX );
	if( want > ncand ) want = ncand;

	for( i = 0; i < want; i++ )
	{
		int best = i;
		for( n = i + 1; n < ncand; n++ )
			if( cand[n].d2 < cand[best].d2 ) best = n;
		if( best != i )
		{
			cl_entity_t *te = cand[i].e; float td = cand[i].d2;
			cand[i].e = cand[best].e; cand[i].d2 = cand[best].d2;
			cand[best].e = te; cand[best].d2 = td;
		}
	}

	cov = (int)bound( (float)ES_COV_MIN, r_entity_shadows_size.value, (float)ES_COV_MAX );
	blur = (int)bound( 0.0f, r_entity_shadows_softness.value, (float)ES_SOFT_MAX );
	if( blur > cov / 3 ) blur = cov / 3;	// keep the blurred edge inside the bitmap

	// drop the texture pool if the coverage size changed (TF_UPDATE needs matching dims)
	if( es_covtex_size != cov )
	{
		for( i = 0; i < ES_HARD_MAX; i++ )
		{
			if( es_covtex[i] )
			{
				GL_FreeTexture( es_covtex[i] );
				es_covtex[i] = 0;
			}
		}
		es_covtex_size = cov;
	}

	// build coverage footprints (CPU raster + blur + upload); drops casters with none
	for( i = 0; i < want; i++ )
	{
		if( R_EntityShadowPrepare( &es_casters[es_count], cand[i].e, cov, blur ))
			es_count++;
	}
	if( !es_count )
		return;

	// --- darkening receiver state ---
	pglEnable( GL_DEPTH_TEST );
	pglDepthFunc( GL_LEQUAL );
	pglDepthMask( GL_FALSE );
	GL_Cull( GL_FRONT );
	GL_PushPolygonOffset( -1.0f, -2.0f );	// pull toward the viewer to beat z-fight with the lit surface
	pglEnable( GL_BLEND );

	if( dbg )
	{
		pglBlendFunc( GL_ONE, GL_ONE );			// additive yellow where shadowed
		pglColor4f( 1.0f, 1.0f, 0.0f, 1.0f );
	}
	else
	{
		pglBlendFunc( GL_ZERO, GL_ONE_MINUS_SRC_COLOR );	// fb *= (1 - s*cov)
		pglColor4f( s, s, s, 1.0f );
	}

	for( i = 0; i < es_count; i++ )
		R_EntityShadowReceivers( &es_casters[i] );

	// --- restore ---
	GL_PopPolygonOffset();
	GL_CleanupAllTextureUnits();
	GL_SelectTexture( 0 );
	GL_LoadIdentityTexMatrix();
	pglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
	pglDisable( GL_BLEND );
	pglDepthMask( GL_TRUE );
	pglDepthFunc( GL_LEQUAL );
	GL_Cull( GL_FRONT );
	GL_SetRenderMode( kRenderNormal );

	es_count = 0;
}

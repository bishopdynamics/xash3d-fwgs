/*
gl_ao.c - ambient occlusion (Continuum "graphics improvements")
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
"Real" (world-space, no screen-space artifacts) ambient occlusion, default-off,
built phase by phase:

  Phase 1 (this file, r_ao 1): entity -> floor CONTACT ao. A soft, pre-blurred
  footprint is laid on the floor under each studio model (monsters, props,
  bodies), oriented + sized to the model bbox, fading as the model lifts off the
  ground. Pure fixed-function (a textured, blended quad), so it costs ~nothing and
  runs on the Deck. No silhouette shape yet - that's a later render-to-texture
  upgrade.

  Phase 2 (r_ao 1, world AO): baked per-texel world AO, raycast against the BSP,
  multiplied into the runtime lightmap copy. The raycast is done offline by the
  engine (host_aobake.c) and cached to disk; this file just loads the cache when
  a map comes up (R_AOWorldLoadCache), so a seamless level change never hitches.

  Phase 3 (planned): a coarse baked occlusion volume so entities receive world AO
  (a body in an alcove reads darker).

  r_ao 2 is reserved for a future screen-space (SSAO) mode so the two can be
  compared; deferred for now (GLSL/Deck portability + perf risk).
*/

#include "gl_local.h"
#include "xash3d_mathlib.h"
#include "pm_defs.h"	// PM_STUDIO_IGNORE etc. for the contact-AO ground trace
#include "ao_cache.h"	// baked world-AO cache format (written by host_aobake.c)

CVAR_DEFINE_AUTO( r_ao, "1", FCVAR_ARCHIVE, "ambient occlusion: 0 off, 1 world-space (\"real\")" );
CVAR_DEFINE_AUTO( r_ao_strength, "0.5", FCVAR_ARCHIVE, "contact-AO darkness under entities (0..1)" );
CVAR_DEFINE_AUTO( r_ao_size, "1.1", FCVAR_ARCHIVE, "contact-AO footprint scale vs the model bbox" );
CVAR_DEFINE_AUTO( r_ao_fade, "72", FCVAR_ARCHIVE, "height (units) over the floor at which the contact AO fully fades out" );
CVAR_DEFINE_AUTO( r_ao_silhouette, "1", FCVAR_ARCHIVE, "contact AO shape: 1 = projected model silhouette, 0 = soft blob" );
CVAR_DEFINE_AUTO( r_ao_soft, "2", FCVAR_ARCHIVE, "silhouette penumbra width in units (edge softness); 0 = hard edge" );
CVAR_DEFINE_AUTO( r_ao_height, "16", FCVAR_ARCHIVE, "contact height falloff: model parts at the floor cast fully, fading to nothing this many units up (feet > legs > arms)" );
CVAR_DEFINE_AUTO( r_ao_ground_dot, "0.7", FCVAR_ARCHIVE, "contact-AO ground confidence: minimum upward floor-normal (0..1); a steeper hit is treated as not-a-floor and AO is skipped rather than floated" );
CVAR_DEFINE_AUTO( r_ao_debug, "0", 0, "debug: draw contact-AO footprints as solid magenta (no depth/blend), bypassing the normal gates" );
CVAR_DEFINE_AUTO( r_ao_world, "0.8", FCVAR_ARCHIVE, "baked world AO strength (0 = off .. 1)" );
CVAR_DEFINE_AUTO( r_ao_world_max, "0.6", FCVAR_ARCHIVE, "world-AO max occlusion (0..1): caps how dark a surface can get so tight gaps don't slam to black. live - no re-bake" );

// r_ao_world_dist (bake quality) lives engine-side in host_aobake.c now: the
// renderer no longer raycasts, it loads the cache the engine baked.

#define AO_DISC_SIZE	64

static int ao_disc = 0;	// procedural soft footprint (alpha falls off to the edge)

/*
=================
R_AOMakeDisc

a radially soft disc in the alpha channel: opaque at the centre, smoothly to zero
at the edge (TF_CLAMP keeps it from tiling). Pre-blurred, so the contact patch has
soft edges with a single quad and no double-blending.
=================
*/
static void R_AOMakeDisc( void )
{
	const float half = ( AO_DISC_SIZE - 1 ) * 0.5f;
	byte *data;
	int x, y;

	if( ao_disc )
		return;

	data = Mem_Malloc( r_temppool, AO_DISC_SIZE * AO_DISC_SIZE * 4 );

	for( y = 0; y < AO_DISC_SIZE; y++ )
	{
		for( x = 0; x < AO_DISC_SIZE; x++ )
		{
			float dx = ( x - half ) / half;
			float dy = ( y - half ) / half;
			float r = sqrtf( dx * dx + dy * dy );	// 0 centre .. 1 edge
			float a, t;
			byte b;

			if( r >= 1.0f )
				a = 0.0f;
			else
			{
				// stay full over the inner ~40%, smoothstep down to 0 by the rim
				t = ( r - 0.4f ) / ( 1.0f - 0.4f );
				if( t < 0.0f ) t = 0.0f;
				a = 1.0f - t * t * ( 3.0f - 2.0f * t );
			}

			b = (byte)( bound( 0.0f, a, 1.0f ) * 255.0f );
			data[( y * AO_DISC_SIZE + x ) * 4 + 0] = 255;
			data[( y * AO_DISC_SIZE + x ) * 4 + 1] = 255;
			data[( y * AO_DISC_SIZE + x ) * 4 + 2] = 255;
			data[( y * AO_DISC_SIZE + x ) * 4 + 3] = b;	// only the alpha is used (colour is forced black)
		}
	}

	ao_disc = GL_CreateTexture( "*ao_disc", AO_DISC_SIZE, AO_DISC_SIZE, data,
		TF_NOMIPMAP | TF_CLAMP | TF_HAS_ALPHA );

	Mem_Free( data );
}

void R_InitAO( void )
{
	gEngfuncs.Cvar_RegisterVariable( &r_ao );
	gEngfuncs.Cvar_RegisterVariable( &r_ao_strength );
	gEngfuncs.Cvar_RegisterVariable( &r_ao_size );
	gEngfuncs.Cvar_RegisterVariable( &r_ao_fade );
	gEngfuncs.Cvar_RegisterVariable( &r_ao_silhouette );
	gEngfuncs.Cvar_RegisterVariable( &r_ao_soft );
	gEngfuncs.Cvar_RegisterVariable( &r_ao_height );
	gEngfuncs.Cvar_RegisterVariable( &r_ao_ground_dot );
	gEngfuncs.Cvar_RegisterVariable( &r_ao_debug );
	gEngfuncs.Cvar_RegisterVariable( &r_ao_world );
	gEngfuncs.Cvar_RegisterVariable( &r_ao_world_max );
}

float R_AOSoftRadius( void )
{
	return Q_max( 0.0f, r_ao_soft.value );
}

float R_AOContactHeight( void )
{
	return Q_max( 1.0f, r_ao_height.value );
}

float R_AOGroundDot( void )
{
	return bound( 0.0f, r_ao_ground_dot.value, 1.0f );
}

qboolean R_AOContactActive( void )
{
	return ( r_ao.value >= 1.0f ) && ( r_ao_strength.value > 0.0f );
}

qboolean R_AODebugActive( void )
{
	return r_ao_debug.value != 0.0f;
}

qboolean R_AOSilhouette( void )
{
	return r_ao_silhouette.value != 0.0f;
}

/*
=================
R_AOGroundTrace

xash3d-streaming: find a CONFIDENT ground spot under one entity for contact AO,
and report whether one was found. `contact` is a point under the model's actual
resting mass - a low-biased centroid of the posed geometry, NOT the entity origin
(which can sit far from where the body really is, e.g. a scientist whose origin is
out over a desk). The caller computes it from the posed verts.

This is deliberately separate from the lighting lightspot (g_studio.lightspot),
which traces the WORLD ONLY for shading: here we trace down through brush ENTITIES
too (PM_STUDIO_IGNORE keeps func_door/plat/train/breakable while skipping studio
models), so a body resting on the anomalous-materials airlock door - or a chair
seat - grounds on THAT surface, not the world floor metres below, and never
self-collides.

Returns false (caller skips AO entirely) unless the hit is a real floor close
under the contact point: a roughly-horizontal surface (normal up >=
r_ao_ground_dot), not inside solid, within a sane vertical gap. Skipping beats
drawing a blob at a wrong spot - a floating shadow.
=================
*/
qboolean R_AOGroundTrace( const vec3_t contact, vec3_t out_floor )
{
	vec3_t src, end;
	pmtrace_t tr;
	float gap, maxgap;

	if( !R_AODebugActive() && !R_AOContactActive( ))
		return false;

	// start just above the contact (the body's lowest point) so we never hit a
	// surface ABOVE it - e.g. a desk the model leans under - then trace straight
	// down well past any plausible drop
	VectorCopy( contact, src );
	src[2] += 8.0f;
	VectorCopy( contact, end );
	end[2] -= 2048.0f;

	tr = gEngfuncs.CL_TraceLine( src, end, PM_STUDIO_IGNORE );
	gap = contact[2] - tr.endpos[2];

	if( tr.startsolid || tr.allsolid || tr.fraction >= 1.0f )
		return false;	// started in solid, or nothing below -> no confident floor

	if( tr.plane.normal[2] < bound( 0.0f, r_ao_ground_dot.value, 1.0f ))
		return false;	// wall / steep slope -> not a floor we trust under the model

	// the floor must sit close under the contact point; a far hit (ledge edge,
	// airborne, over a pit) is not "the ground under this entity"
	maxgap = Q_max( 16.0f, r_ao_fade.value );
	if( gap < -16.0f || gap > maxgap )
		return false;

	VectorCopy( tr.endpos, out_floor );
	return true;
}

/*
=================
R_AOContactAlpha

peak contact darkness for an entity: r_ao_strength scaled by the height fade (full
on the floor, gone once the model's lowest point rises r_ao_fade units above its
floor spot). Used by the silhouette path, which lives in gl_studio.c.
=================
*/
float R_AOContactAlpha( const vec3_t floor, const vec3_t origin, const vec3_t mins )
{
	float h, t, fh, fade;

	h = ( origin[2] + mins[2] ) - floor[2];
	if( h < 0.0f ) h = 0.0f;
	fh = Q_max( 1.0f, r_ao_fade.value );
	t = h / fh;
	if( t > 1.0f ) t = 1.0f;
	fade = 1.0f - t * t * ( 3.0f - 2.0f * t );

	return r_ao_strength.value * fade;
}

/*
=================
R_DrawContactSplat

lay a soft, oriented footprint on the floor plane. `floor` is the ground point
under the model (z = floor[2]); `right`/`fwd` are unit floor-plane axes (model
orientation); `rx`/`ry` are the footprint half-extents along them; `alpha` is the
peak darkness. Pure FFP: a textured, blended quad.
=================
*/
static void R_DrawContactSplat( const vec3_t floor, const vec3_t right, const vec3_t fwd, float rx, float ry, float alpha )
{
	vec3_t c[4];
	int i;

	R_AOMakeDisc();
	if( !ao_disc )
		return;

	// quad corners on the floor, sitting just above it to beat z-fighting
	for( i = 0; i < 4; i++ )
	{
		float sx = ( i == 1 || i == 2 ) ? rx : -rx;
		float sy = ( i >= 2 ) ? ry : -ry;

		c[i][0] = floor[0] + right[0] * sx + fwd[0] * sy;
		c[i][1] = floor[1] + right[1] * sx + fwd[1] * sy;
		c[i][2] = floor[2] + right[2] * sx + fwd[2] * sy + 1.0f;
	}

	if( R_AODebugActive( ))
	{
		// DEBUG: solid magenta, no texture/blend/depth - just prove the quad exists
		pglDisable( GL_TEXTURE_2D );
		pglDisable( GL_BLEND );
		pglDisable( GL_DEPTH_TEST );
		GL_Cull( GL_NONE );
		pglColor4f( 1.0f, 0.0f, 1.0f, 1.0f );
		pglBegin( GL_TRIANGLE_FAN );
		pglVertex3fv( c[0] ); pglVertex3fv( c[1] ); pglVertex3fv( c[2] ); pglVertex3fv( c[3] );
		pglEnd();
		pglEnable( GL_DEPTH_TEST );
		pglEnable( GL_TEXTURE_2D );
		GL_Cull( GL_FRONT );
		pglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
		return;
	}

	alpha = bound( 0.0f, alpha, 1.0f );
	if( alpha <= 0.003f )
		return;

	GL_Bind( XASH_TEXTURE0, ao_disc );
	pglTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
	pglEnable( GL_BLEND );
	pglBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
	pglEnable( GL_DEPTH_TEST );
	pglDepthFunc( GL_LEQUAL );
	pglDepthMask( GL_FALSE );
	GL_Cull( GL_NONE );	// floor quad should show regardless of winding
	GL_PushPolygonOffset( -1.0f, -2.0f );
	pglColor4f( 0.0f, 0.0f, 0.0f, alpha );	// black, modulated by the disc's alpha

	pglBegin( GL_TRIANGLE_FAN );
	pglTexCoord2f( 0.0f, 0.0f ); pglVertex3fv( c[0] );
	pglTexCoord2f( 1.0f, 0.0f ); pglVertex3fv( c[1] );
	pglTexCoord2f( 1.0f, 1.0f ); pglVertex3fv( c[2] );
	pglTexCoord2f( 0.0f, 1.0f ); pglVertex3fv( c[3] );
	pglEnd();

	GL_PopPolygonOffset();
	GL_Cull( GL_FRONT );
	pglDepthMask( GL_TRUE );
	pglDisable( GL_BLEND );
	pglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
}

/*
=================
R_AOEntityContact

Phase-1 entry: drop a soft contact footprint under one studio entity. `floor` is
the traced ground spot below it (z = the floor); `origin`/`angles` place + orient
it; `mins`/`maxs` are the model bbox (its footprint + how far the lowest point
sits). The footprint is oriented to the model's yaw and sized to the bbox; it
fades as the model lifts off the floor. All the AO tuning (strength/size/fade)
is applied here so the studio caller stays cvar-free.
=================
*/
void R_AOEntityContact( const vec3_t floor, const vec3_t origin, const vec3_t angles, const vec3_t mins, const vec3_t maxs )
{
	qboolean dbg = R_AODebugActive();
	vec3_t flat, fwd, right;
	float hx, hy, h, fade, t, fh;

	if( !dbg && !R_AOContactActive( ))
		return;

	// footprint half-extents from the bbox: model X (forward) and Y (right)
	hx = ( maxs[0] - mins[0] ) * 0.5f;
	hy = ( maxs[1] - mins[1] ) * 0.5f;
	if( hx < 4.0f ) hx = 4.0f;
	if( hy < 4.0f ) hy = 4.0f;
	hx *= r_ao_size.value;
	hy *= r_ao_size.value;

	// fade out as the model's lowest point rises above its floor spot
	h = ( origin[2] + mins[2] ) - floor[2];
	if( h < 0.0f ) h = 0.0f;
	fh = Q_max( 1.0f, r_ao_fade.value );
	t = h / fh;
	if( t > 1.0f ) t = 1.0f;
	fade = 1.0f - t * t * ( 3.0f - 2.0f * t );

	if( !dbg && fade <= 0.0f )
		return;

	// flat (floor-plane) orientation from yaw only, so body pitch/roll never tilts it
	VectorSet( flat, 0.0f, angles[1], 0.0f );
	AngleVectors( flat, fwd, right, NULL );

	R_DrawContactSplat( floor, right, fwd, hy, hx, r_ao_strength.value * fade );
}

/*
=================
CPU silhouette stamp

A genuinely soft, shaped contact shadow without FBO/stencil: the studio caller
(gl_studio.c) rasterizes the model's projected triangles into the coverage bitmap
below; this box-blurs it (texture-space, so it actually smooths - unlike jittering
geometry) and projects it as one quad on the floor. No per-layer density darkening
either, because coverage is filled once then blurred.
=================
*/
#define AO_STAMP	64

static byte ao_cov[AO_STAMP * AO_STAMP];	// coverage bitmap, filled by the caller
static int  ao_stamp_tex = 0;

void R_AOStampBegin( int *size )
{
	memset( ao_cov, 0, sizeof( ao_cov ));
	*size = AO_STAMP;
}

/*
=================
R_AOStampTri

fill one triangle (coords already in stamp space [0,AO_STAMP)) into the coverage
bitmap, height-weighted per vertex (wa/wb/wc, 0..1) and barycentrically
interpolated - so parts near the floor contribute more. Winding-agnostic; keeps
the max so overlapping triangles don't double-darken.
=================
*/
void R_AOStampTri( const float a[2], const float b[2], const float c[2], float wa, float wb, float wc )
{
	float d = ( b[0] - a[0] ) * ( c[1] - a[1] ) - ( b[1] - a[1] ) * ( c[0] - a[0] );
	float invd;
	int minx, maxx, miny, maxy, x, y;

	if( d > -0.0001f && d < 0.0001f )
		return;	// degenerate
	invd = 1.0f / d;

	minx = (int)floorf( Q_min( a[0], Q_min( b[0], c[0] )));
	maxx = (int)ceilf ( Q_max( a[0], Q_max( b[0], c[0] )));
	miny = (int)floorf( Q_min( a[1], Q_min( b[1], c[1] )));
	maxy = (int)ceilf ( Q_max( a[1], Q_max( b[1], c[1] )));
	if( minx < 0 ) minx = 0;
	if( miny < 0 ) miny = 0;
	if( maxx > AO_STAMP - 1 ) maxx = AO_STAMP - 1;
	if( maxy > AO_STAMP - 1 ) maxy = AO_STAMP - 1;

	for( y = miny; y <= maxy; y++ )
	{
		for( x = minx; x <= maxx; x++ )
		{
			float px = x + 0.5f, py = y + 0.5f;
			float ba = (( c[0] - b[0] ) * ( py - b[1] ) - ( c[1] - b[1] ) * ( px - b[0] )) * invd;
			float bb = (( a[0] - c[0] ) * ( py - c[1] ) - ( a[1] - c[1] ) * ( px - c[0] )) * invd;
			float bc = (( b[0] - a[0] ) * ( py - a[1] ) - ( b[1] - a[1] ) * ( px - a[0] )) * invd;

			if( ba >= 0.0f && bb >= 0.0f && bc >= 0.0f )
			{
				int v = (int)(( ba * wa + bb * wb + bc * wc ) * 255.0f );
				if( v > ao_cov[y * AO_STAMP + x] )
					ao_cov[y * AO_STAMP + x] = (byte)v;
			}
		}
	}
}

static void R_AOBoxBlur( int radius )
{
	static byte tmp[AO_STAMP * AO_STAMP];
	int x, y, d;

	if( radius < 1 )
		return;

	for( y = 0; y < AO_STAMP; y++ )
	{
		for( x = 0; x < AO_STAMP; x++ )
		{
			int sum = 0, n = 0;
			for( d = -radius; d <= radius; d++ )
			{
				int xx = x + d;
				if( xx < 0 || xx >= AO_STAMP ) continue;
				sum += ao_cov[y * AO_STAMP + xx]; n++;
			}
			tmp[y * AO_STAMP + x] = (byte)( sum / n );
		}
	}
	for( y = 0; y < AO_STAMP; y++ )
	{
		for( x = 0; x < AO_STAMP; x++ )
		{
			int sum = 0, n = 0;
			for( d = -radius; d <= radius; d++ )
			{
				int yy = y + d;
				if( yy < 0 || yy >= AO_STAMP ) continue;
				sum += tmp[yy * AO_STAMP + x]; n++;
			}
			ao_cov[y * AO_STAMP + x] = (byte)( sum / n );
		}
	}
}

/*
=================
R_AOStampProject

blur the (caller-filled) coverage, upload it, and lay it on the floor over the
world-space rectangle [minx,miny]..[maxx,maxy]. `alpha` is peak darkness; `blur`
is the box-blur radius in texels.
=================
*/
void R_AOStampProject( float minx, float miny, float maxx, float maxy, float floorz, float alpha, int blur )
{
	static byte rgba[AO_STAMP * AO_STAMP * 4];
	qboolean dbg = R_AODebugActive();
	vec3_t c[4];
	int i;

	if( !dbg )
	{
		alpha = bound( 0.0f, alpha, 1.0f );
		if( alpha <= 0.003f )
			return;
	}

	R_AOBoxBlur( blur );

	for( i = 0; i < AO_STAMP * AO_STAMP; i++ )
	{
		rgba[i * 4 + 0] = 255; rgba[i * 4 + 1] = 255; rgba[i * 4 + 2] = 255;
		rgba[i * 4 + 3] = ao_cov[i];	// coverage -> alpha
	}
	ao_stamp_tex = GL_CreateTexture( "*ao_stamp", AO_STAMP, AO_STAMP, rgba,
		TF_NOMIPMAP | TF_CLAMP | TF_HAS_ALPHA | ( ao_stamp_tex ? TF_UPDATE : 0 ));
	if( !ao_stamp_tex )
		return;

	c[0][0] = minx; c[0][1] = miny;
	c[1][0] = maxx; c[1][1] = miny;
	c[2][0] = maxx; c[2][1] = maxy;
	c[3][0] = minx; c[3][1] = maxy;
	c[0][2] = c[1][2] = c[2][2] = c[3][2] = floorz;

	GL_Bind( XASH_TEXTURE0, ao_stamp_tex );
	pglTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
	pglEnable( GL_BLEND );
	pglBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
	pglEnable( GL_DEPTH_TEST );
	pglDepthFunc( GL_LEQUAL );
	pglDepthMask( GL_FALSE );
	GL_Cull( GL_NONE );
	GL_PushPolygonOffset( -1.0f, -2.0f );
	if( dbg ) pglColor4f( 1.0f, 0.0f, 1.0f, 1.0f );
	else pglColor4f( 0.0f, 0.0f, 0.0f, alpha );

	pglBegin( GL_TRIANGLE_FAN );
	pglTexCoord2f( 0.0f, 0.0f ); pglVertex3fv( c[0] );
	pglTexCoord2f( 1.0f, 0.0f ); pglVertex3fv( c[1] );
	pglTexCoord2f( 1.0f, 1.0f ); pglVertex3fv( c[2] );
	pglTexCoord2f( 0.0f, 1.0f ); pglVertex3fv( c[3] );
	pglEnd();

	GL_PopPolygonOffset();
	GL_Cull( GL_FRONT );
	pglDepthMask( GL_TRUE );
	pglDisable( GL_BLEND );
	pglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
}

/*
=================
baked world AO - cache consumer

Per-luxel occlusion is baked offline by the engine (host_aobake.c) and stored
in cache/ao/<map>.ao (see common/ao_cache.h). It is kept as its own per-surface
layer (NOT folded into the BSP lightmap data, so strength stays a live tweak and
the original lighting is never destroyed). At map load we read the cache straight
into each surface's layer - no raycasting, so a seamless level change costs a
file read, not a hitch. R_BuildLightMap (gl_rsurf.c) multiplies the layer into
each lit texel, or shows it as magenta when r_ao_debug is set.
=================
*/

// Baked AO is stored ON each surface (info->shadowmap) - NOT in a surface-index
// array. The streaming build shares one lightmap atlas across every preloaded map,
// so R_BuildLightMap runs over surfaces from OTHER maps too; an index like
// (surf - WORLDMODEL->surfaces) is garbage for those and bled one map's AO onto
// another's geometry (floating shadows after a transition). A per-surface pointer
// can't be confused: a surface we didn't bake simply has NULL. ao_bufs only tracks
// our allocations for freeing; ao_baked_model gates the layer to the baked map.
static byte	**ao_bufs = NULL;
static int	ao_bufcount = 0;
static model_t	*ao_baked_model = NULL;

float R_AOWorldStrength( void )
{
	if( r_ao.value < 1.0f )
		return 0.0f;	// master "Ambient Occlusion" toggle gates world AO too
	return r_ao_world.value;
}

float R_AOWorldMax( void )
{
	return bound( 0.0f, r_ao_world_max.value, 1.0f );
}

// occlusion bytes for a surface, or NULL if not baked (or baked for another map)
byte *R_AOWorldMap( const msurface_t *surf )
{
	if( !ao_baked_model || WORLDMODEL != ao_baked_model || !surf->info )
		return NULL;
	return surf->info->shadowmap;
}

static void R_AOWorldFree( void )
{
	int i;

	if( ao_bufs )
	{
		for( i = 0; i < ao_bufcount; i++ )
			if( ao_bufs[i] ) Mem_Free( ao_bufs[i] );
		Mem_Free( ao_bufs );
	}
	ao_bufs = NULL;
	ao_bufcount = 0;
	ao_baked_model = NULL;
}

// called when the lightmaps are (re)built for a new map, so a previous map's bake
// is never applied to different geometry
void R_AOWorldInvalidate( void )
{
	R_AOWorldFree();
}

/*
=================
R_AOWorldLoadCache

read cache/ao/<map>.ao (baked by the engine) into each surface's occlusion
layer. Returns true if a valid cache for this world was applied. The header's
numsurfaces guards against a different BSP loading under the same map name; a
stale-format cache (version/size mismatch) is ignored and simply yields no AO.
=================
*/
static qboolean R_AOWorldLoadCache( model_t *world )
{
	char  path[256], base[64];
	byte *file, *p, *end;
	fs_offset_t len = 0;
	ao_cache_header_t *hdr;
	int   i;

	if( !world || !world->name[0] )
		return false;

	COM_FileBase( world->name, base, sizeof( base ));
	Q_snprintf( path, sizeof( path ), "cache/ao/%s.ao", base );

	file = gEngfuncs.fsapi->LoadFile( path, &len, false );
	if( !file )
		return false;

	if( len < (fs_offset_t)sizeof( *hdr ))
	{
		Mem_Free( file );
		return false;
	}

	hdr = (ao_cache_header_t *)file;
	if( hdr->magic != AO_CACHE_MAGIC || hdr->version != AO_CACHE_VERSION
		|| hdr->numsurfaces != world->numsurfaces )
	{
		Mem_Free( file );	// stale or for a different BSP -> no AO
		return false;
	}

	// clear any previous AO pointers on this world (NULL first so freeing the old
	// buffers never leaves a dangling shadowmap), then free them.
	for( i = 0; i < world->numsurfaces; i++ )
		if( world->surfaces[i].info ) world->surfaces[i].info->shadowmap = NULL;

	R_AOWorldFree();
	ao_bufcount = world->numsurfaces;
	ao_bufs = Mem_Malloc( r_temppool, ao_bufcount * sizeof( byte * ));
	memset( ao_bufs, 0, ao_bufcount * sizeof( byte * ));

	p = file + sizeof( *hdr );
	end = file + len;

	for( i = 0; i < hdr->numbaked; i++ )
	{
		ao_cache_surf_t *rec = (ao_cache_surf_t *)p;
		int    nbytes;
		byte  *occmap;

		if( p + sizeof( *rec ) > end )
			break;	// truncated file
		p += sizeof( *rec );

		nbytes = (int)rec->smax * (int)rec->tmax;
		if( nbytes <= 0 || p + nbytes > end || rec->surf < 0 || rec->surf >= world->numsurfaces )
			break;	// corrupt record - stop, keep what we read

		occmap = Mem_Malloc( r_temppool, nbytes );
		memcpy( occmap, p, nbytes );
		p += nbytes;

		if( world->surfaces[rec->surf].info )
			world->surfaces[rec->surf].info->shadowmap = occmap;	// per-surface
		ao_bufs[rec->surf] = occmap;	// tracked for freeing
	}

	Mem_Free( file );
	ao_baked_model = world;
	gEngfuncs.Con_Reportf( "^3[ao]^7 loaded %s.ao: %i surfaces\n", base, hdr->numbaked );
	return true;
}

/*
=================
R_AOWorldFrame

Load this map's baked world-AO cache the first time we render it with AO on, and
re-apply live when an apply-time knob changes. Called once per frame for the main
view. The cache read is cheap (a file read + memcpy), so there is no per-map
hitch - the raycast that used to live here is now done offline by the engine.
=================
*/
void R_AOWorldFrame( void )
{
	static float l_ao = -1.0f, l_world = -1.0f, l_max = -1.0f, l_dbg = -1.0f;

	// Re-apply the baked layer live when an apply-time knob changes - the master
	// toggle, world strength, the clamp, or debug. No re-bake needed for these, so
	// turning AO on/off from the menu (or sliding strength) is instant.
	if( r_ao.value != l_ao || r_ao_world.value != l_world || r_ao_world_max.value != l_max || r_ao_debug.value != l_dbg )
	{
		l_ao = r_ao.value; l_world = r_ao_world.value;
		l_max = r_ao_world_max.value; l_dbg = r_ao_debug.value;
		if( ao_baked_model == WORLDMODEL )
			GL_RebuildLightmaps();
	}

	// load the cache once when the master AO toggle and world AO are both on
	if( r_ao.value < 1.0f || r_ao_world.value <= 0.0f || !WORLDMODEL )
		return;
	if( WORLDMODEL == ao_baked_model )
		return;	// already loaded this map

	if( R_AOWorldLoadCache( WORLDMODEL ))
		GL_RebuildLightmaps();	// re-run R_BuildLightMap so the AO layer is applied
}

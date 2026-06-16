/*
gl_flashlight.c - improved projected-texture flashlight (Continuum "Insane things")
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
A projected-texture spotlight ("cookie") flashlight for the GL renderer, built
entirely on fixed-function GL (texgen + texture matrix + multitexture) so it
also rides the gl2_shim on ES2/core. Default-off; gated by r_flashlight_projected.

Phase 1: cone + cookie projected onto world/brush surfaces, additive, with
per-vertex distance attenuation and a clip plane to kill back-projection. The
engine's stock EF_DIMLIGHT point-dlight is left in place so studio models
(monsters/props) still get lit.

Phase 2 (r_flashlight_shadows, experimental): a no-FBO shadow map. World depth
from the light's POV is rendered into a framebuffer corner before R_Clear,
copied into a TF_DEPTHMAP texture, and sampled with ARB_shadow hardware compare
in the cookie pass so walls/ducts occlude the beam.

See docs/flashlight-design.md.
*/

#include "gl_local.h"
#include "xash3d_mathlib.h"

CVAR_DEFINE_AUTO( r_flashlight_projected, "0", FCVAR_ARCHIVE, "use the improved projected-texture flashlight" );
CVAR_DEFINE_AUTO( r_flashlight_cone, "35", FCVAR_ARCHIVE, "beam (hotspot) cone angle in degrees" );
CVAR_DEFINE_AUTO( r_flashlight_intensity, "3.0", FCVAR_ARCHIVE, "beam (hotspot) brightness; >1 draws extra additive passes (uncapped)" );
CVAR_DEFINE_AUTO( r_flashlight_spill_cone, "90", FCVAR_ARCHIVE, "spill (field) cone angle in degrees - the wider, dimmer halo around the beam" );
CVAR_DEFINE_AUTO( r_flashlight_spill_intensity, "0.15", FCVAR_ARCHIVE, "spill brightness as a fraction of the beam (0..1)" );
CVAR_DEFINE_AUTO( r_flashlight_range, "3000", FCVAR_ARCHIVE, "flashlight maximum range in units" );
CVAR_DEFINE_AUTO( r_flashlight_albedo, "1", FCVAR_ARCHIVE, "modulate the cone by the surface texture (1) or flat-add (0)" );
CVAR_DEFINE_AUTO( r_flashlight_shadows, "1", FCVAR_ARCHIVE, "flashlight casts dynamic shadows (shadow map)" );
CVAR_DEFINE_AUTO( r_flashlight_shadow_size, "512", FCVAR_ARCHIVE, "shadow-map resolution in texels (square); higher = crisper shadow edges, more GPU. clamped to the back-buffer size" );
CVAR_DEFINE_AUTO( r_flashlight_shadow_slopebias, "3.0", FCVAR_ARCHIVE, "shadow depth bias scaled by surface slope; raise to kill grazing-angle self-shadow banding, lower if shadows detach (peter-panning)" );
CVAR_DEFINE_AUTO( r_flashlight_shadow_bias, "2.0", FCVAR_ARCHIVE, "constant shadow depth bias (units); the flat baseline added on top of the slope bias" );
CVAR_DEFINE_AUTO( r_flashlight_shadow_normaloffset, "2.0", FCVAR_ARCHIVE, "push the shadow lookup this many units along the receiver's surface normal; fixes grazing-angle banding the depth bias can't (light-parallel surfaces), without peter-panning. 0 = off" );
CVAR_DEFINE_AUTO( r_flashlight_offset, "-4", FCVAR_ARCHIVE, "vertical light offset from the eye: +above (headlamp) / -below (chest, parallax for shadows); clamped -24..24" );
CVAR_DEFINE_AUTO( r_flashlight_offset_h, "-4", FCVAR_ARCHIVE, "horizontal light offset from the eye: +right (shoulder) / -left; 0 = centered (chest); clamped -24..24" );
CVAR_DEFINE_AUTO( r_flashlight_debug, "0", 0, "debug: draw the raw projected cookie (no albedo/attenuation/NdotL)" );

#define FL_MAX_PASSES	6	// intensity cap: each whole unit of intensity is one additive pass

#define FL_COOKIE_SIZE	128
#define FL_SHADOW_MIN	256	// shadow-map resolution bounds (square texels)
#define FL_SHADOW_MAX	4096
#define FL_ATTEN_W	256	// distance-falloff ramp (projected beam depth -> brightness)
// near plane kept well off the lens: the 350:1 near/far ratio of a 4-unit near
// wrecks shadow-map depth precision (grainy self-shadow acne). 24 units (~1.5ft,
// invisible for a flashlight) brings the ratio to ~60:1.
#define FL_NEAR		24.0f
// warm white, like the HL flashlight beam sprite
static const float fl_color[3] = { 1.0f, 0.96f, 0.88f };

static int fl_cookie = 0;	// procedural cookie texture (cone cross-section)
static int fl_cookie_inv = 0;	// 1 - cookie, for entity-shadow suppression in the beam
static int fl_atten = 0;	// distance-falloff ramp along the beam
static int fl_depth = 0;	// shadow-map depth texture
static int fl_depth_size = 0;	// current side length of fl_depth (tracks r_flashlight_shadow_size)

// per-frame scene-fit depth range for the SHADOW projection (acne/moire fix).
// Cached on tr.framecount so the early depth pass and the later receiver pass
// read identical near/far (their depth values must match) and the O(surfaces)
// scan runs once per frame.
static int   fl_fit_frame = -1;
static float fl_fit_near = FL_NEAR;
static float fl_fit_far = 0.0f;

typedef struct
{
	qboolean	ok;
	vec3_t		origin;
	vec3_t		fwd;
	float		range;
	float		proj[16];	// light projection (GL column-major)
	float		view[16];	// light view
	float		texmat[16];	// bias * proj * view : world -> [0,1]^3
	float		shadow_proj[16];	// like proj, but with the per-frame scene-fit near/far
	float		shadow_texmat[16];	// bias * shadow_proj * view (shadow-map depth space)
} fl_params_t;

/*
=================
Mat4 helpers (GL column-major, index = col*4 + row)
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

static void Mat4_Perspective( float *m, float fovy_deg, float aspect, float zn, float zf )
{
	float f = 1.0f / tanf( DEG2RAD( fovy_deg ) * 0.5f );

	memset( m, 0, sizeof( float ) * 16 );
	m[0]  = f / aspect;
	m[5]  = f;
	m[10] = ( zf + zn ) / ( zn - zf );
	m[11] = -1.0f;
	m[14] = ( 2.0f * zf * zn ) / ( zn - zf );
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

/*
=================
R_FlashlightUpdateCookie

Two-cone profile baked into one projected cookie (projected at the wider SPILL
angle): a bright hotspot core in the middle and a wider, dimmer spill ring around
it, fading to black before the texture edge (TF_BORDER) so the square frustum
boundary never shows. Regenerated only when the beam/spill shape changes.

  d 0 ............ beam_edge ............ FL_MARGIN .. 1 (texture edge)
    | hotspot 1.0 -> spill | spill_frac -> 0  | black border |

`beam_edge` is the hotspot radius = (beam/spill angle ratio) of the lit radius.
=================
*/
#define FL_MARGIN	0.85f	// lit radius; black from here out to the texture edge

static float fl_cookie_beam = -1.0f;	// cached shape so we only rebuild on change
static float fl_cookie_spillv = -1.0f;

static void R_FlashlightUpdateCookie( void )
{
	float beam = bound( 2.0f, r_flashlight_cone.value, 170.0f );
	float spill = bound( beam, r_flashlight_spill_cone.value, 175.0f );
	float beam_r = ( beam / spill ) * FL_MARGIN;		// hotspot outer radius in d
	float spillv = bound( 0.0f, r_flashlight_spill_intensity.value, 1.0f );
	const float half = ( FL_COOKIE_SIZE - 1 ) * 0.5f;
	byte *data;
	int x, y, i;

	if( fl_cookie && beam_r == fl_cookie_beam && spillv == fl_cookie_spillv )
		return;	// unchanged

	data = Mem_Malloc( r_temppool, FL_COOKIE_SIZE * FL_COOKIE_SIZE * 4 );

	for( y = 0; y < FL_COOKIE_SIZE; y++ )
	{
		for( x = 0; x < FL_COOKIE_SIZE; x++ )
		{
			float dx = ( x - half ) / half;
			float dy = ( y - half ) / half;
			float d = sqrtf( dx * dx + dy * dy );	// 0 center .. 1 edge .. 1.41 corner
			float i, t, s;
			byte b;

			if( d >= FL_MARGIN )
			{
				i = 0.0f;	// black border
			}
			else if( d <= beam_r )
			{
				// hotspot: 1.0 in the centre, smoothly down to the spill level
				t = ( beam_r > 0.0001f ) ? d / beam_r : 1.0f;
				s = t * t * ( 3.0f - 2.0f * t );
				i = 1.0f - s * ( 1.0f - spillv );
			}
			else
			{
				// spill ring: spill level fading to black at the lit edge
				t = ( d - beam_r ) / ( FL_MARGIN - beam_r );
				s = t * t * ( 3.0f - 2.0f * t );
				i = spillv * ( 1.0f - s );
			}

			b = (byte)( bound( 0.0f, i, 1.0f ) * 255.0f );
			data[( y * FL_COOKIE_SIZE + x ) * 4 + 0] = b;
			data[( y * FL_COOKIE_SIZE + x ) * 4 + 1] = b;
			data[( y * FL_COOKIE_SIZE + x ) * 4 + 2] = b;
			data[( y * FL_COOKIE_SIZE + x ) * 4 + 3] = b;
		}
	}

	fl_cookie = GL_CreateTexture( "*flashlight_cookie", FL_COOKIE_SIZE, FL_COOKIE_SIZE, data,
		TF_NOMIPMAP | TF_BORDER | TF_HAS_ALPHA | ( fl_cookie ? TF_UPDATE : 0 ));

	// inverted cookie (1 - cookie) for entity-shadow suppression: where the beam is
	// bright the entity shadow's darkening is multiplied toward 0. The cookie fades to 0
	// by its edge, so the inverse is ~1 (white) there - TF_CLAMP then reads "no
	// suppression" outside the cone (shadows away from the beam are untouched).
	for( i = 0; i < FL_COOKIE_SIZE * FL_COOKIE_SIZE * 4; i++ )
		data[i] = 255 - data[i];
	fl_cookie_inv = GL_CreateTexture( "*flashlight_cookie_inv", FL_COOKIE_SIZE, FL_COOKIE_SIZE, data,
		TF_NOMIPMAP | TF_CLAMP | TF_HAS_ALPHA | ( fl_cookie_inv ? TF_UPDATE : 0 ));

	fl_cookie_beam = beam_r;
	fl_cookie_spillv = spillv;

	Mem_Free( data );
}

/*
=================
R_FlashlightCreateAtten

1-D distance ramp (stored as a wide RGBA texture): full brightness over the near
part of the beam, smoothly fading to black by the far plane. TF_BORDER (black)
means anything past the far plane — or behind the near plane — contributes 0.
Sampled per-fragment by the projected beam depth, so it never facets.
=================
*/
static void R_FlashlightCreateAtten( void )
{
	byte *data = Mem_Malloc( r_temppool, FL_ATTEN_W * 4 );
	int x;

	for( x = 0; x < FL_ATTEN_W; x++ )
	{
		float d = x / (float)( FL_ATTEN_W - 1 );	// 0 near .. 1 at the far plane
		// full out to ~35% of range, then a smooth fade to black at the far plane
		float t = ( d - 0.35f ) / ( 1.0f - 0.35f );
		float i;

		if( t < 0.0f ) t = 0.0f;
		if( t > 1.0f ) t = 1.0f;
		i = 1.0f - t * t * ( 3.0f - 2.0f * t );		// 1 .. 0 smoothstep

		byte b = (byte)( i * 255.0f );
		data[x * 4 + 0] = data[x * 4 + 1] = data[x * 4 + 2] = b;
		data[x * 4 + 3] = b;
	}

	fl_atten = GL_CreateTexture( "*flashlight_atten", FL_ATTEN_W, 1, data,
		TF_NOMIPMAP | TF_BORDER | TF_HAS_ALPHA );

	Mem_Free( data );
}

/*
=================
R_FlashlightAttenMatrix

build a texture matrix that maps a world vertex to (depth, 0.5, 0, q) where
depth = the cookie projection's R/Q. Then a 2-D atten texture sampled at
(depth, 0.5) gives the per-fragment distance falloff. Rows are copied from the
cookie matrix M (column-major, index = col*4 + row): S row <- M's R row, the Q
row <- M's Q row, T row pinned to 0.5 so it lands in the middle of the ramp.
=================
*/
static void R_FlashlightAttenMatrix( const float *m, float *out )
{
	memset( out, 0, sizeof( float ) * 16 );

	out[0]  = m[2];        out[4]  = m[6];        out[8]  = m[10];        out[12] = m[14];	// S = depth
	out[1]  = 0.5f * m[3]; out[5]  = 0.5f * m[7]; out[9]  = 0.5f * m[11]; out[13] = 0.5f * m[15];	// T = 0.5
	out[3]  = m[3];        out[7]  = m[7];        out[11] = m[11];        out[15] = m[15];	// Q
}

void R_InitFlashlight( void )
{
	gEngfuncs.Cvar_RegisterVariable( &r_flashlight_projected );
	gEngfuncs.Cvar_RegisterVariable( &r_flashlight_cone );
	gEngfuncs.Cvar_RegisterVariable( &r_flashlight_spill_cone );
	gEngfuncs.Cvar_RegisterVariable( &r_flashlight_spill_intensity );
	gEngfuncs.Cvar_RegisterVariable( &r_flashlight_range );
	gEngfuncs.Cvar_RegisterVariable( &r_flashlight_intensity );
	gEngfuncs.Cvar_RegisterVariable( &r_flashlight_albedo );
	gEngfuncs.Cvar_RegisterVariable( &r_flashlight_shadows );
	gEngfuncs.Cvar_RegisterVariable( &r_flashlight_shadow_size );
	gEngfuncs.Cvar_RegisterVariable( &r_flashlight_shadow_slopebias );
	gEngfuncs.Cvar_RegisterVariable( &r_flashlight_shadow_bias );
	gEngfuncs.Cvar_RegisterVariable( &r_flashlight_shadow_normaloffset );
	gEngfuncs.Cvar_RegisterVariable( &r_flashlight_offset );
	gEngfuncs.Cvar_RegisterVariable( &r_flashlight_offset_h );
	gEngfuncs.Cvar_RegisterVariable( &r_flashlight_debug );
}

static qboolean R_FlashlightSurfaceVisible( msurface_t *surf, const vec3_t origin, const vec3_t fwd, float range );

/*
=================
R_FlashlightFitShadowRange

Scan the world surfaces the shadow pass will actually rasterize (same cone/range
cull) and return the near/far that tightly bound them along the beam axis. A
flashlight frustum fixed at 24..3000 packs almost all of its depth precision into
the first few feet, leaving the rest as coarse steps -> self-shadow acne (the
moire on lit surfaces, worse up close). Fitting far to the nearest wall the beam
actually hits (a few hundred units in HL corridors) collapses the near/far ratio
and restores precision across the whole usable range.

Cached on tr.framecount: the early depth-render pass and the later receiver pass
both call this and MUST get identical matrices (their depth values are compared),
and the O(surfaces) scan should run only once per frame. Used ONLY for the shadow
projection - the cookie/atten keep the fixed range so the beam never breathes.
=================
*/
static void R_FlashlightFitShadowRange( const vec3_t origin, const vec3_t fwd, float range, float *out_near, float *out_far )
{
	model_t *world = WORLDMODEL;
	float nearest = range, farthest = FL_NEAR;
	int found = 0;
	int i;

	if( fl_fit_frame == tr.framecount )
	{
		*out_near = fl_fit_near;
		*out_far = fl_fit_far;
		return;
	}

	for( i = world->firstmodelsurface; i < world->firstmodelsurface + world->nummodelsurfaces; i++ )
	{
		msurface_t *surf = &world->surfaces[i];
		mextrasurf_t *info;
		vec3_t center, delta;
		float along, radius;

		if( !surf->polys )
			continue;
		if( FBitSet( surf->flags, SURF_DRAWSKY | SURF_DRAWTURB | SURF_DRAWTURB_QUADS ))
			continue;
		if( !R_FlashlightSurfaceVisible( surf, origin, fwd, range ))
			continue;

		info = surf->info;
		VectorAverage( info->mins, info->maxs, center );
		VectorSubtract( center, origin, delta );
		along = DotProduct( delta, fwd );
		VectorSubtract( info->maxs, center, delta );
		radius = VectorLength( delta );

		if( along - radius < nearest )  nearest = along - radius;
		if( along + radius > farthest ) farthest = along + radius;
		found++;
	}

	if( found )
	{
		// margins absorb the bbox-sphere slop and any brush-entity/studio caster a
		// little past the world fit. Floor near at FL_NEAR for precision and to keep
		// close dynamic casters in front of the plane (cap its rise at 96 so a monster
		// a couple of feet away still casts); never exceed the configured range.
		nearest -= 16.0f;
		farthest += 64.0f;
		fl_fit_near = bound( FL_NEAR, nearest, 96.0f );
		fl_fit_far = bound( fl_fit_near + 64.0f, farthest, range );
	}
	else
	{
		fl_fit_near = FL_NEAR;
		fl_fit_far = range;
	}

	fl_fit_frame = tr.framecount;
	*out_near = fl_fit_near;
	*out_far = fl_fit_far;
}

/*
=================
R_FlashlightParams

compute origin / forward / matrices for this frame; ok=false if inactive
=================
*/
static fl_params_t R_FlashlightParams( void )
{
	fl_params_t f;
	cl_entity_t *player;
	float bias[16], tmp[16];
	vec3_t up;
	float cone;

	memset( &f, 0, sizeof( f ));

	if( !r_flashlight_projected.value )
		return f;
	if( !WORLDMODEL || !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
		return f;

	player = CL_GetEntityByIndex( gp_cl->viewentity );
	if( !player || !FBitSet( player->curstate.effects, EF_DIMLIGHT ))
		return f;

	VectorCopy( RI.vforward, f.fwd );
	VectorCopy( RI.vup, up );

	// offset the light off the eye (held like a chest-level flashlight). a head-
	// mounted light co-located with the camera hides every shadow behind its own
	// caster; a modest offset gives the parallax that makes shadows visible while
	// keeping the cone roughly where the player is looking. the offset is WORLD-
	// vertical + WORLD-horizontal-right and clamped, so it stays a fixed distance
	// from the eye no matter where you look and never sinks into the floor.
	{
		float voff = bound( -24.0f, r_flashlight_offset.value,   24.0f );	// + above (headlamp), - below (chest)
		float hoff = bound( -24.0f, r_flashlight_offset_h.value, 24.0f );	// + right (shoulder), - left
		vec3_t right_h;

		// keep the sideways step in the world horizontal plane (zero Z, renormalize)
		// so a shoulder offset stays level and doesn't drift up/down with view pitch.
		VectorSet( right_h, RI.vright[0], RI.vright[1], 0.0f );
		VectorNormalize( right_h );

		// The offset shifts the light off the eye for shadow parallax (kept small:
		// too far below + crouching sinks the light through the floor). The FIXED
		// backward step is separate - it keeps the projection's q=0 plane behind
		// everything visible, so a large floor polygon crossing it never shows a hard
		// straight edge; decoupling it means signed offsets are safe.
		VectorCopy( RI.rvp.vieworigin, f.origin );
		f.origin[2] += voff;					// + raises the light above the eye
		VectorMA( f.origin, hoff, right_h, f.origin );		// + shifts it to the player's right
		VectorMA( f.origin, -28.0f, RI.vforward, f.origin );	// fixed backward
	}

	// the projection uses the wider SPILL angle; the cookie texture carries the
	// narrower hotspot inside it. clamp spill >= beam so the hotspot always fits.
	{
		float beam = bound( 5.0f, r_flashlight_cone.value, 170.0f );
		cone = bound( beam, r_flashlight_spill_cone.value, 175.0f );
	}
	f.range = Q_max( 64.0f, r_flashlight_range.value );

	memset( bias, 0, sizeof( bias ));
	bias[0] = bias[5] = bias[10] = 0.5f;
	bias[12] = bias[13] = bias[14] = 0.5f;
	bias[15] = 1.0f;

	Mat4_Perspective( f.proj, cone, 1.0f, FL_NEAR, f.range );
	Mat4_LookAt( f.view, f.origin, f.fwd, up );
	Mat4_Mult( tmp, f.proj, f.view );
	Mat4_Mult( f.texmat, bias, tmp );

	// The shadow map gets its own projection with a per-frame scene-fit near/far so
	// its depth precision isn't blown on empty distance (self-shadow acne / moire).
	// The cookie + atten above deliberately keep the FIXED FL_NEAR..range, so the
	// beam's visual length and falloff never change as the fit adapts.
	if( r_flashlight_shadows.value )
	{
		float fnear, ffar, sproj[16];

		R_FlashlightFitShadowRange( f.origin, f.fwd, f.range, &fnear, &ffar );
		Mat4_Perspective( sproj, cone, 1.0f, fnear, ffar );
		memcpy( f.shadow_proj, sproj, sizeof( sproj ));
		Mat4_Mult( tmp, sproj, f.view );
		Mat4_Mult( f.shadow_texmat, bias, tmp );
	}
	else
	{
		memcpy( f.shadow_proj, f.proj, sizeof( f.proj ));
		memcpy( f.shadow_texmat, f.texmat, sizeof( f.texmat ));
	}

	f.ok = true;
	return f;
}

static qboolean R_FlashlightSurfaceVisible( msurface_t *surf, const vec3_t origin, const vec3_t fwd, float range )
{
	mextrasurf_t *info = surf->info;
	vec3_t center, delta;
	float along, radius;

	VectorAverage( info->mins, info->maxs, center );
	VectorSubtract( center, origin, delta );
	along = DotProduct( delta, fwd );

	VectorSubtract( info->maxs, center, delta );
	radius = VectorLength( delta );

	if( along < -radius )		return false;	// fully behind the lens
	if( along > range + radius )	return false;	// beyond range
	return true;
}

/*
=================
R_FlashlightProjUnit

set up a TMU for projective texturing of `texnum` using the spot matrix
=================
*/
static void R_FlashlightProjUnit( int tmu, int texnum, const float *texmat )
{
	static const float planeS[4] = { 1, 0, 0, 0 };
	static const float planeT[4] = { 0, 1, 0, 0 };
	static const float planeR[4] = { 0, 0, 1, 0 };
	static const float planeQ[4] = { 0, 0, 0, 1 };

	GL_SelectTexture( tmu );
	GL_Bind( tmu, texnum );
	pglTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );

	pglTexGenfv( GL_S, GL_OBJECT_PLANE, planeS );
	pglTexGenfv( GL_T, GL_OBJECT_PLANE, planeT );
	pglTexGenfv( GL_R, GL_OBJECT_PLANE, planeR );
	pglTexGenfv( GL_Q, GL_OBJECT_PLANE, planeQ );
	GL_TexGen( GL_S, GL_OBJECT_LINEAR );
	GL_TexGen( GL_T, GL_OBJECT_LINEAR );
	GL_TexGen( GL_R, GL_OBJECT_LINEAR );
	GL_TexGen( GL_Q, GL_OBJECT_LINEAR );
	GL_LoadTexMatrixExt( texmat );
}

/*
=================
R_FlashlightBrushVisible

coarse light-range cull for a brush entity (door/platform/func_wall). Its model
bbox is in local space; e->origin places it in the world.
=================
*/
static qboolean R_FlashlightBrushVisible( cl_entity_t *e, const fl_params_t *f )
{
	model_t *m = e->model;
	vec3_t center, size, delta;
	float along, radius;

	VectorAverage( m->mins, m->maxs, center );
	VectorAdd( center, e->origin, center );
	VectorSubtract( m->maxs, m->mins, size );
	radius = 0.5f * VectorLength( size );

	VectorSubtract( center, f->origin, delta );
	along = DotProduct( delta, f->fwd );

	if( along < -radius )		return false;	// behind the lens
	if( along > f->range + radius )	return false;	// beyond range
	return true;
}

/*
=================
R_FlashlightLightSurf

additive cookie submit for one surface. `obj` NULL => verts are already world-space
(worldspawn); otherwise they're transformed by it (brush entity, whose poly verts
are in model-local space).

Masked ({-textured) surfaces - ladders, handrails, grates - are alpha-tested so the
transparent texels add no light (no "ghost rectangle"). The mask alpha lives on the
surface texture (TMU 0), which is only bound in albedo/"Tint by Surface" mode, so in
flat mode masked surfaces are skipped instead of flood-lit. The combined fragment
alpha is tex.a * cookie.a * atten.a, so the test must be ~0: opaque texels (tex.a=1)
then pass wherever the cone has any intensity, while the cut-out texels (tex.a=0)
drop out. `*alpha_on` carries the masked GL state across calls (surfaces aren't
sorted, so it toggles only on change).
=================
*/
/*
=================
R_FlashlightShadowNormalOffset

Normal-offset bias: bend the SHADOW unit's projective lookup so it samples at
(vertex + surface_normal * offset) instead of right on the surface. Depth bias
slides the comparison along the light ray, which does nothing for a surface that
lies nearly along the ray (the extreme grazing case) - there the normal points
across the ray, so a push along it is exactly the direction that escapes the
surface's own depth. Folded into the object-plane texgen w terms (coord =
plane . (vert,1)), so only the shadow lookup moves - the lit/visible geometry and
the cookie/atten units are untouched. `obj` rotates the model-local normal into
world space for brush entities (NULL = worldspawn, already world-space).
=================
*/
static void R_FlashlightShadowNormalOffset( int shadow_tmu, msurface_t *surf, const matrix4x4 obj, float offset )
{
	vec3_t n;
	float planeS[4] = { 1, 0, 0, 0 };
	float planeT[4] = { 0, 1, 0, 0 };
	float planeR[4] = { 0, 0, 1, 0 };

	if( !surf->plane )
		return;

	VectorCopy( surf->plane->normal, n );
	if( FBitSet( surf->flags, SURF_PLANEBACK ))
		VectorNegate( n, n );
	if( obj )
	{
		vec3_t world_n;
		Matrix4x4_VectorRotate( obj, n, world_n );
		VectorCopy( world_n, n );
	}

	planeS[3] = n[0] * offset;
	planeT[3] = n[1] * offset;
	planeR[3] = n[2] * offset;

	GL_SelectTexture( shadow_tmu );
	pglTexGenfv( GL_S, GL_OBJECT_PLANE, planeS );
	pglTexGenfv( GL_T, GL_OBJECT_PLANE, planeT );
	pglTexGenfv( GL_R, GL_OBJECT_PLANE, planeR );
}

static void R_FlashlightLightSurf( msurface_t *surf, const matrix4x4 obj, qboolean albedo, float pass_intensity, qboolean *alpha_on, int shadow_tmu, float normaloffset )
{
	glpoly2_t *p = surf->polys;
	qboolean dbg = r_flashlight_debug.value != 0.0f;
	qboolean masked;
	int v;

	if( !p )
		return;
	if( FBitSet( surf->flags, SURF_DRAWSKY | SURF_DRAWTURB | SURF_DRAWTURB_QUADS | SURF_DRAWTILED ))
		return;

	masked = FBitSet( surf->flags, SURF_TRANSPARENT ) ? true : false;
	if( masked && !albedo )
		return;	// no surface-texture alpha to cut the gaps in flat mode

	if( masked != *alpha_on )
	{
		if( masked ) { pglEnable( GL_ALPHA_TEST ); pglAlphaFunc( GL_GREATER, 0.0f ); }
		else pglDisable( GL_ALPHA_TEST );
		*alpha_on = masked;
	}

	if( albedo )
	{
		texture_t *t = surf->texinfo && surf->texinfo->texture ? surf->texinfo->texture : NULL;
		GL_Bind( 0, t ? t->gl_texturenum : tr.whiteTexture );
	}

	// per-face normal-offset bias on the shadow lookup (grazing-angle acne fix)
	if( shadow_tmu >= 0 && normaloffset != 0.0f )
		R_FlashlightShadowNormalOffset( shadow_tmu, surf, obj, normaloffset );

	for( ; p; p = p->next )
	{
		float *vert = p->verts[0];
		pglBegin( GL_POLYGON );
		for( v = 0; v < p->numverts; v++, vert += VERTEXSIZE )
		{
			float atten = dbg ? 1.0f : pass_intensity;

			pglColor4f( fl_color[0] * atten, fl_color[1] * atten, fl_color[2] * atten, 1.0f );
			if( albedo )
				GL_MultiTexCoord2f( 0, vert[3], vert[4] );
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

/*
=================
R_FlashlightBrushSurfaces

additive cookie pass over one brush entity's surfaces. A brush entity's poly verts
are in MODEL-LOCAL space and the entity's transform (origin + angles) places them,
so each is transformed to world space on the CPU and then lit exactly like the world
(the projective texgen + texmat expect world-space verts). This is why doors/
platforms/offset func_walls used to go dark: the world cookie loop skipped them
(stale visframe) and, even if it hadn't, would have lit their untransformed local
positions. Masked entities (ladders/grates) are alpha-tested via R_FlashlightLightSurf.
=================
*/
static void R_FlashlightBrushSurfaces( cl_entity_t *e, qboolean albedo, float pass_intensity, qboolean *alpha_on, int shadow_tmu, float normaloffset )
{
	model_t *m = e->model;
	matrix4x4 obj;
	int i;

	Matrix4x4_CreateFromEntity( obj, e->angles, e->origin, 1.0f );

	for( i = 0; i < m->nummodelsurfaces; i++ )
		R_FlashlightLightSurf( &m->surfaces[m->firstmodelsurface + i], obj, albedo, pass_intensity, alpha_on, shadow_tmu, normaloffset );
}

/*
=================
R_FlashlightDepthSurf

submit one surface's depth for the shadow map. Solid surfaces write the whole
polygon. Masked ({-textured) surfaces - ladders, handrails, grates - are alpha-
tested against their texture, so only the opaque texels (e.g. the ladder rungs)
write depth and the shadow carries the cut-out shape instead of a solid slab.
`obj` NULL => verts are already world-space (worldspawn); otherwise they're
transformed by it (brush entity). `*alpha_on` carries the masked GL state across
calls so it toggles only when it actually changes (surfaces aren't sorted).
=================
*/
static void R_FlashlightDepthSurf( msurface_t *surf, const matrix4x4 obj, qboolean *alpha_on )
{
	glpoly2_t *p = surf->polys;
	qboolean masked;

	if( !p )
		return;
	if( FBitSet( surf->flags, SURF_DRAWSKY | SURF_DRAWTURB | SURF_DRAWTURB_QUADS ))
		return;

	// binary mask only (the user explicitly wants cut-out, not partial opacity)
	masked = FBitSet( surf->flags, SURF_TRANSPARENT ) ? true : false;

	if( masked != *alpha_on )
	{
		if( masked )
		{
			pglEnable( GL_TEXTURE_2D );
			pglTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE );	// alpha straight from the texture
			pglEnable( GL_ALPHA_TEST );
			pglAlphaFunc( GL_GREATER, 0.5f );
		}
		else
		{
			pglDisable( GL_ALPHA_TEST );
			pglDisable( GL_TEXTURE_2D );
		}
		*alpha_on = masked;
	}

	if( masked )
	{
		texture_t *t = surf->texinfo && surf->texinfo->texture ? surf->texinfo->texture : NULL;
		GL_Bind( 0, t ? t->gl_texturenum : tr.whiteTexture );
	}

	for( ; p; p = p->next )
	{
		float *vert = p->verts[0];
		int v;

		pglBegin( GL_POLYGON );
		for( v = 0; v < p->numverts; v++, vert += VERTEXSIZE )
		{
			if( masked )
				GL_MultiTexCoord2f( 0, vert[3], vert[4] );
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

/*
=================
R_FlashlightBrushDepth

depth submit of one brush entity for the shadow map, CPU-transformed to world
space (same reasoning as R_FlashlightBrushSurfaces). Lets doors/platforms cast
shadows from their CURRENT position; masked faces (ladder/handrail brush
entities) cast their cut-out shape via R_FlashlightDepthSurf.
=================
*/
static void R_FlashlightBrushDepth( cl_entity_t *e, qboolean *alpha_on )
{
	model_t *m = e->model;
	matrix4x4 obj;
	int i;

	Matrix4x4_CreateFromEntity( obj, e->angles, e->origin, 1.0f );

	for( i = 0; i < m->nummodelsurfaces; i++ )
		R_FlashlightDepthSurf( &m->surfaces[m->firstmodelsurface + i], obj, alpha_on );
}

/*
=================
R_DrawFlashlightSurfaces

additive cookie pass over visible world surfaces
=================
*/
static void R_DrawFlashlightSurfaces( const fl_params_t *f, qboolean albedo, qboolean shadows, float pass_intensity )
{
	model_t *world = WORLDMODEL;
	int next = albedo ? 1 : 0;			// TMU 0 is the surface albedo when enabled
	int cookie_tmu = next++;			// cone cross-section
	int atten_tmu = next++;				// distance falloff along the beam
	int shadow_tmu = ( shadows && fl_depth ) ? next++ : -1;
	float normaloffset = ( shadow_tmu >= 0 ) ? r_flashlight_shadow_normaloffset.value : 0.0f;
	float attenmat[16];
	qboolean alpha_on = false;	// tracks the masked (alpha-test) GL state across the surface/entity loops
	int i;

	if( albedo )
	{
		// base albedo unit (real texcoords, modulated by the per-vertex flash colour)
		GL_SelectTexture( 0 );
		pglTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
	}

	// cookie (cone) * atten (depth) [* shadow], all projected, all MODULATE
	R_FlashlightProjUnit( cookie_tmu, fl_cookie, f->texmat );
	R_FlashlightAttenMatrix( f->texmat, attenmat );
	R_FlashlightProjUnit( atten_tmu, fl_atten, attenmat );
	if( shadow_tmu >= 0 )
		R_FlashlightProjUnit( shadow_tmu, fl_depth, f->shadow_texmat );

	// only the static worldspawn faces here; brush entities (doors/func_walls/ladders)
	// are lit separately below, transformed to their current position. (Iterating the
	// whole face array would re-light brush submodels at their untransformed local
	// positions - and they'd be skipped by the visframe test anyway.) The cookie's
	// projection (frustum + TF_BORDER) and radial falloff do ALL the shaping per-
	// fragment; the vertex colour is a flat intensity - every per-vertex falloff tried
	// instead added a hard straight edge (see git history / design doc).
	for( i = world->firstmodelsurface; i < world->firstmodelsurface + world->nummodelsurfaces; i++ )
	{
		msurface_t *surf = &world->surfaces[i];

		if( surf->visframe != tr.framecount )
			continue;
		if( !R_FlashlightSurfaceVisible( surf, f->origin, f->fwd, f->range ))
			continue;

		R_FlashlightLightSurf( surf, NULL, albedo, pass_intensity, &alpha_on, shadow_tmu, normaloffset );
	}

	// brush entities lit at their current transform. Opaque doors/walls (kRenderNormal)
	// are in the solid list; alpha-tested "Solid" brushes - ladders, handrails, grates
	// ({-textured func_illusionary/func_wall, kRenderTransAlpha) - are in the TRANS list
	// (R_OpaqueEntity only keeps kRenderNormal solid). Scan both; keep the binary-solid
	// modes and drop genuinely translucent ones (glass etc.).
	if( tr.draw_list )
	{
		cl_entity_t **lists[2] = { tr.draw_list->solid_entities, tr.draw_list->trans_entities };
		int counts[2] = { tr.draw_list->num_solid_entities, tr.draw_list->num_trans_entities };
		int li;

		for( li = 0; li < 2; li++ )
		{
			for( i = 0; i < counts[li]; i++ )
			{
				cl_entity_t *ent = lists[li][i];
				int rm;

				if( !ent->model || ent->model->type != mod_brush )
					continue;
				rm = R_GetEntityRenderMode( ent );
				if( rm != kRenderNormal && rm != kRenderTransAlpha )
					continue;
				if( !R_FlashlightBrushVisible( ent, f ))
					continue;
				R_FlashlightBrushSurfaces( ent, albedo, pass_intensity, &alpha_on, shadow_tmu, normaloffset );
			}
		}
	}

	if( alpha_on )
		pglDisable( GL_ALPHA_TEST );
}

/*
=================
R_FlashlightShadowPass

PHASE 2. render world depth from the light's POV into a framebuffer corner and
copy it into the shadow-map depth texture. Called early in R_RenderScene, before
R_Clear wipes the framebuffer for the real scene (so the corner leaves no trace).
=================
*/
void R_FlashlightShadowPass( void )
{
	fl_params_t f;
	model_t *world;
	int S;
	int i;
	qboolean alpha_masked = false;	// tracks the masked (alpha-test) GL state across the caster loops

	if( !r_flashlight_shadows.value )
		return;

	f = R_FlashlightParams();
	if( !f.ok )
		return;

	// requested resolution, clamped to a sane range and to the back buffer (the
	// no-FBO shadow map renders into a corner of the visible framebuffer, so it can
	// never be larger than the window).
	S = (int)bound( (float)FL_SHADOW_MIN, r_flashlight_shadow_size.value, (float)FL_SHADOW_MAX );
	if( S > RI.rvp.viewport[2] ) S = RI.rvp.viewport[2];
	if( S > RI.rvp.viewport[3] ) S = RI.rvp.viewport[3];
	if( S < 16 ) return;

	// the depth texture is exactly S x S: the cookie's texmat maps the whole [0,1]
	// range onto the full texture, and the corner render fills exactly S x S, so the
	// two must match. (Re)allocate whenever the resolution changes.
	if( !fl_depth || fl_depth_size != S )
	{
		if( fl_depth )
			GL_FreeTexture( fl_depth );
		fl_depth = GL_CreateTexture( "*flashlight_depth", S, S, NULL,
			TF_NOMIPMAP | TF_CLAMP | TF_DEPTHMAP );
		fl_depth_size = fl_depth ? S : 0;
	}
	if( !fl_depth )
		return;

	// light view/proj
	pglViewport( 0, 0, S, S );
	pglMatrixMode( GL_PROJECTION );
	pglPushMatrix();
	pglLoadMatrixf( f.shadow_proj );	// scene-fit near/far (precision; kills acne)
	pglMatrixMode( GL_MODELVIEW );
	pglPushMatrix();
	pglLoadMatrixf( f.view );

	pglEnable( GL_SCISSOR_TEST );
	pglScissor( 0, 0, S, S );
#if !XASH_GLES
	// glClearDepth is the desktop-GL spelling (GLES has only glClearDepthf). The
	// shadow map is a desktop-GL-only feature, so skip it on the GLES renderers;
	// 1.0 is the default clear-depth anyway, so the following pglClear is fine.
	pglClearDepth( 1.0 );
#endif
	pglClear( GL_DEPTH_BUFFER_BIT );

	pglColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE );
	pglDepthMask( GL_TRUE );
	pglEnable( GL_DEPTH_TEST );
	pglDepthFunc( GL_LEQUAL );
	GL_CleanupAllTextureUnits();
	GL_SelectTexture( 0 );
	pglDisable( GL_TEXTURE_2D );
	// no culling: GoldSrc world brushes are single-sided, so culling by winding in
	// the light's view drops occluders or records the wrong side (shadows that look
	// like they come from the back of stairs). Recording every face + LEQUAL keeps
	// the nearest surface to the light, which is what the shadow test wants.
	GL_Cull( 0 );
	// slope-scaled depth bias: factor*slope keeps grazing faces (where one shadow
	// texel spans a big depth range) from self-shadowing into bands; units is the
	// flat baseline. Both tunable live - raise slopebias until the grazing banding
	// clears, back off if shadows start detaching from their casters (peter-panning).
	GL_PushPolygonOffset( Q_max( 0.0f, r_flashlight_shadow_slopebias.value ), Q_max( 0.0f, r_flashlight_shadow_bias.value ));

	// only worldspawn (static) geometry casts shadows. WORLDMODEL->surfaces is the
	// WHOLE BSP face array (worldspawn + every brush-entity submodel), so iterating
	// all of it made doors, breakables and even invisible trigger/clip brushes cast
	// stale shadows from their original positions. The worldmodel's own static faces
	// are [firstmodelsurface .. +nummodelsurfaces). (Moving brush models and studio
	// models as shadow casters are a separate TODO.)
	world = WORLDMODEL;
	for( i = world->firstmodelsurface; i < world->firstmodelsurface + world->nummodelsurfaces; i++ )
	{
		msurface_t *surf = &world->surfaces[i];

		if( !surf->polys )
			continue;
		if( FBitSet( surf->flags, SURF_DRAWSKY | SURF_DRAWTURB | SURF_DRAWTURB_QUADS ))
			continue;
		if( !R_FlashlightSurfaceVisible( surf, f.origin, f.fwd, f.range ))
			continue;

		R_FlashlightDepthSurf( surf, NULL, &alpha_masked );
	}

	// brush entities (doors, platforms, func_walls) cast from their CURRENT
	// transform: their faces are CPU-transformed to world space (the worldspawn loop
	// above deliberately skips them - iterating them untransformed is exactly what
	// produced stale shadows at their original positions).
	if( tr.draw_list )
	{
		// doors/walls (kRenderNormal) live in the solid list; alpha-tested "Solid"
		// brushes - ladders, handrails, grates ({-textured func_illusionary/func_wall,
		// kRenderTransAlpha) - are sorted into the TRANS list (R_OpaqueEntity only
		// keeps kRenderNormal solid). Scan both; the rendermode filter then keeps just
		// the binary-solid casters and drops genuinely translucent modes (glass etc.).
		cl_entity_t **lists[2] = { tr.draw_list->solid_entities, tr.draw_list->trans_entities };
		int counts[2] = { tr.draw_list->num_solid_entities, tr.draw_list->num_trans_entities };
		int li;

		for( li = 0; li < 2; li++ )
		{
			for( i = 0; i < counts[li]; i++ )
			{
				cl_entity_t *ent = lists[li][i];
				int rm;

				if( !ent->model || ent->model->type != mod_brush )
					continue;
				rm = R_GetEntityRenderMode( ent );
				if( rm != kRenderNormal && rm != kRenderTransAlpha )
					continue;
				if( !R_FlashlightBrushVisible( ent, &f ))
					continue;
				R_FlashlightBrushDepth( ent, &alpha_masked );
			}
		}
	}

	// leave masked (alpha-test) mode before the studio casters, which submit solid
	// depth with no texture bound.
	if( alpha_masked )
	{
		pglDisable( GL_ALPHA_TEST );
		pglDisable( GL_TEXTURE_2D );
		alpha_masked = false;
	}

	// studio models (monsters/props) cast too: their depth is rendered from the
	// light's POV into the same corner, under the same polygon offset. Bones aren't
	// set up this early in the frame, so the studio path sets them up itself (the
	// normal entity draw, later, rebuilds them). Same world-space verts + LEQUAL,
	// so they occlude the beam exactly like the world brushes do.
	R_StudioDrawShadowCasters();

	GL_PopPolygonOffset();

	// copy depth into the shadow texture (desktop-GL-only path; the GLES
	// renderers don't build the shadow map, see the pglClearDepth note above)
	GL_Bind( 0, fl_depth );
#if !XASH_GLES
	pglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, S, S );
#endif

	// restore
	pglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	pglDisable( GL_ALPHA_TEST );
	pglEnable( GL_TEXTURE_2D );
	GL_Cull( GL_FRONT );
	pglDisable( GL_SCISSOR_TEST );
	pglMatrixMode( GL_PROJECTION );
	pglPopMatrix();
	pglMatrixMode( GL_MODELVIEW );
	pglPopMatrix();
	pglViewport( RI.rvp.viewport[0], RI.rvp.viewport[1], RI.rvp.viewport[2], RI.rvp.viewport[3] );
}

/*
=================
R_DrawFlashlight

main entry, called at the end of R_RenderScene (opaque world+entities are in
the depth buffer). Pure ref/gl; no engine state is modified.
=================
*/
void R_DrawFlashlight( void )
{
	fl_params_t f;
	qboolean albedo, shadows;

	f = R_FlashlightParams();
	if( !f.ok )
		return;

	R_FlashlightUpdateCookie();	// creates the cookie, and rebuilds it if the beam/spill shape changed
	if( !fl_atten )
		R_FlashlightCreateAtten();
	if( !fl_cookie || !fl_atten )
		return;

	albedo = r_flashlight_albedo.value ? true : false;
	shadows = ( r_flashlight_shadows.value && fl_depth ) ? true : false;

	// --- render state: additive, depth-test only, pull slightly toward viewer ---
	GL_SetRenderMode( kRenderTransAdd );
	pglBlendFunc( GL_ONE, GL_ONE );
	pglDepthMask( GL_FALSE );
	pglDepthFunc( GL_LEQUAL );
	pglEnable( GL_DEPTH_TEST );
	GL_Cull( GL_FRONT );
	GL_PushPolygonOffset( -1.0f, -2.0f );

	// NOTE: no near-plane clip plane. It used to live here to kill back-projection,
	// but once the light is offset off the eye it slices visible floor closer than
	// FL_NEAR along the beam axis -> a hard straight edge in the cone. Back-projection
	// behind the lens is already handled per-vertex (atten = 0 when along <= 0).
	// brightness above 1.0 is built up with extra additive passes: the fixed-
	// function vertex colour clamps at 1.0, so one pass can't exceed full white.
	{
		float intensity = Q_max( 0.0f, r_flashlight_intensity.value );
		int passes = (int)ceilf( intensity );
		float per;
		int p;

		if( r_flashlight_debug.value != 0.0f ) passes = 1;	// debug shows the raw single cookie
		if( passes < 1 ) passes = 1;
		if( passes > FL_MAX_PASSES ) passes = FL_MAX_PASSES;
		per = intensity / passes;

		for( p = 0; p < passes; p++ )
			R_DrawFlashlightSurfaces( &f, albedo, shadows, per );
	}

	// --- restore ---
	GL_PopPolygonOffset();
	// reset ALL units: the cookie/shadow + their projective texgen/matrix live on
	// upper TMUs, so a partial cleanup would leak them onto the world's lightmap
	// unit (multiplying the whole scene by the black-bordered cookie).
	GL_CleanupAllTextureUnits();
	GL_SelectTexture( 0 );
	GL_LoadIdentityTexMatrix();
	pglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
	pglDisable( GL_ALPHA_TEST );	// masked surfaces may have left it on
	pglDepthMask( GL_TRUE );
	pglDepthFunc( GL_LEQUAL );
	GL_Cull( GL_FRONT );
	GL_SetRenderMode( kRenderNormal );
}

/*
=================
R_FlashlightStudioSetup / R_FlashlightStudioDone

Studio cookie-receive: light studio models (monsters/props) with the same
projected cookie as the world. The studio renderer (gl_studio.c) calls Setup
right after it has drawn a model normally, re-submits the model's world-space
verts (g_studio.verts) between Setup and Done, and the cookie + distance ramp
project onto them additively. No albedo unit - the model's own normal draw is
the base; this just adds the flashlight's light on top. Single pass (the vertex
colour clamps at 1.0, which is plenty for lighting a model).
=================
*/
// returns 0 = inactive, 1 = active (flat glow), 2 = active + needs the model skin
// modulated on TMU 0 (Tint by Surface). The studio caller binds the per-mesh skin
// + supplies its texcoords on TMU 0 when 2 is returned.
int R_FlashlightStudioSetup( void )
{
	fl_params_t f;
	float attenmat[16];
	float intensity;
	qboolean albedo;
	int cookie_tmu, atten_tmu;

	if( !r_flashlight_projected.value )
		return 0;

	f = R_FlashlightParams();
	if( !f.ok )
		return 0;

	R_FlashlightUpdateCookie();
	if( !fl_atten )
		R_FlashlightCreateAtten();
	if( !fl_cookie || !fl_atten )
		return 0;

	albedo = r_flashlight_albedo.value ? true : false;
	intensity = Q_max( 0.0f, r_flashlight_intensity.value );

	GL_SetRenderMode( kRenderTransAdd );
	pglBlendFunc( GL_ONE, GL_ONE );
	pglDepthMask( GL_FALSE );
	pglDepthFunc( GL_LEQUAL );
	pglEnable( GL_DEPTH_TEST );
	GL_PushPolygonOffset( -1.0f, -1.0f );

	if( albedo )
	{
		// TMU 0 modulates the cookie by the model skin (bound per-mesh by the caller)
		GL_SelectTexture( 0 );
		pglTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
		cookie_tmu = 1;
		atten_tmu = 2;
	}
	else
	{
		cookie_tmu = 0;
		atten_tmu = 1;
	}

	R_FlashlightProjUnit( cookie_tmu, fl_cookie, f.texmat );
	R_FlashlightAttenMatrix( f.texmat, attenmat );
	R_FlashlightProjUnit( atten_tmu, fl_atten, attenmat );

	pglColor4f( fl_color[0] * intensity, fl_color[1] * intensity, fl_color[2] * intensity, 1.0f );
	return albedo ? 2 : 1;
}

void R_FlashlightStudioDone( void )
{
	GL_PopPolygonOffset();
	GL_CleanupAllTextureUnits();
	GL_SelectTexture( 0 );
	GL_LoadIdentityTexMatrix();
	pglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
	pglDepthMask( GL_TRUE );
	pglDepthFunc( GL_LEQUAL );
	GL_SetRenderMode( kRenderNormal );
}

/*
=================
R_FlashlightSuppressUnit

set up `tmu` to sample the INVERTED flashlight cookie (1 - cookie), projected with the
flashlight's own matrix, in MODULATE - so a caller multiplying by it scales its effect
toward 0 inside the beam and leaves it untouched outside. Used by the entity-shadow
pass so the flashlight overpowers (cancels) the ambient entity shadow where it shines.
Returns false (and sets nothing) when the projected flashlight isn't active this frame.
=================
*/
qboolean R_FlashlightSuppressUnit( int tmu )
{
	fl_params_t f = R_FlashlightParams();

	if( !f.ok )
		return false;

	R_FlashlightUpdateCookie();	// cached; ensures fl_cookie_inv exists (entity shadows run before R_DrawFlashlight)
	if( !fl_cookie_inv )
		return false;

	R_FlashlightProjUnit( tmu, fl_cookie_inv, f.texmat );
	return true;
}

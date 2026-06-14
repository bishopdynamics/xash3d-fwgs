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

  Phase 2 (planned): baked per-texel world AO, raycast against the BSP, multiplied
  into the runtime lightmap copy, cached to disk.

  Phase 3 (planned): a coarse baked occlusion volume so entities receive world AO
  (a body in an alcove reads darker).

  r_ao 2 is reserved for a future screen-space (SSAO) mode so the two can be
  compared; deferred for now (GLSL/Deck portability + perf risk).
*/

#include "gl_local.h"
#include "xash3d_mathlib.h"
#include "pm_defs.h"	// PM_WORLD_ONLY for the world-AO occlusion rays

CVAR_DEFINE_AUTO( r_ao, "1", FCVAR_ARCHIVE, "ambient occlusion: 0 off, 1 world-space (\"real\")" );
CVAR_DEFINE_AUTO( r_ao_strength, "0.5", FCVAR_ARCHIVE, "contact-AO darkness under entities (0..1)" );
CVAR_DEFINE_AUTO( r_ao_size, "1.1", FCVAR_ARCHIVE, "contact-AO footprint scale vs the model bbox" );
CVAR_DEFINE_AUTO( r_ao_fade, "72", FCVAR_ARCHIVE, "height (units) over the floor at which the contact AO fully fades out" );
CVAR_DEFINE_AUTO( r_ao_silhouette, "1", FCVAR_ARCHIVE, "contact AO shape: 1 = projected model silhouette, 0 = soft blob" );
CVAR_DEFINE_AUTO( r_ao_soft, "2", FCVAR_ARCHIVE, "silhouette penumbra width in units (edge softness); 0 = hard edge" );
CVAR_DEFINE_AUTO( r_ao_height, "16", FCVAR_ARCHIVE, "contact height falloff: model parts at the floor cast fully, fading to nothing this many units up (feet > legs > arms)" );
CVAR_DEFINE_AUTO( r_ao_debug, "0", 0, "debug: draw contact-AO footprints as solid magenta (no depth/blend), bypassing the normal gates" );
CVAR_DEFINE_AUTO( r_ao_world, "0.8", FCVAR_ARCHIVE, "baked world AO strength (0 = off .. 1); auto-bakes per map when > 0" );
CVAR_DEFINE_AUTO( r_ao_world_dist, "72", FCVAR_ARCHIVE, "world-AO occlusion ray length in units (bake quality; re-bake after changing)" );
CVAR_DEFINE_AUTO( r_ao_world_max, "0.6", FCVAR_ARCHIVE, "world-AO max occlusion (0..1): caps how dark a surface can get so tight gaps don't slam to black. live - no re-bake" );

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

static void R_AOBakeWorld_f( void );

void R_InitAO( void )
{
	gEngfuncs.Cvar_RegisterVariable( &r_ao );
	gEngfuncs.Cvar_RegisterVariable( &r_ao_strength );
	gEngfuncs.Cvar_RegisterVariable( &r_ao_size );
	gEngfuncs.Cvar_RegisterVariable( &r_ao_fade );
	gEngfuncs.Cvar_RegisterVariable( &r_ao_silhouette );
	gEngfuncs.Cvar_RegisterVariable( &r_ao_soft );
	gEngfuncs.Cvar_RegisterVariable( &r_ao_height );
	gEngfuncs.Cvar_RegisterVariable( &r_ao_debug );
	gEngfuncs.Cvar_RegisterVariable( &r_ao_world );
	gEngfuncs.Cvar_RegisterVariable( &r_ao_world_dist );
	gEngfuncs.Cvar_RegisterVariable( &r_ao_world_max );
	gEngfuncs.Cmd_AddCommand( "r_ao_bake", R_AOBakeWorld_f, "bake world AO into a per-surface layer and apply it to the lightmaps" );
}

float R_AOSoftRadius( void )
{
	return Q_max( 0.0f, r_ao_soft.value );
}

float R_AOContactHeight( void )
{
	return Q_max( 1.0f, r_ao_height.value );
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
Phase 2 (in progress) - baked world AO

Per-luxel occlusion baked against the BSP and kept as its own per-surface layer
(NOT folded into the BSP lightmap data, so strength stays a live tweak and the
original lighting is never destroyed). The luxel world position is recovered by
inverting the engine's luxel<->world map (lmvecs + lightmapmins + the surface
plane); a small hemisphere of world-only rays, proximity-weighted so nearby
geometry counts more, gives the occlusion. r_ao_bake fills the layer then rebuilds
the lightmaps; R_BuildLightMap (gl_rsurf.c) multiplies it into each lit texel, or
shows it as magenta when r_ao_debug is set. Caching + preload integration next.
=================
*/
#define AO_WORLD_RAYS	13

// hemisphere kernel in tangent space (+Z = surface normal): centre + two rings
static const float ao_world_kernel[AO_WORLD_RAYS][3] =
{
	{  0.000f,  0.000f, 1.00f },
	{  0.714f,  0.000f, 0.70f }, {  0.357f,  0.618f, 0.70f }, { -0.357f,  0.618f, 0.70f },
	{ -0.714f,  0.000f, 0.70f }, { -0.357f, -0.618f, 0.70f }, {  0.357f, -0.618f, 0.70f },
	{  0.811f,  0.468f, 0.35f }, {  0.000f,  0.937f, 0.35f }, { -0.811f,  0.468f, 0.35f },
	{ -0.811f, -0.468f, 0.35f }, {  0.000f, -0.937f, 0.35f }, {  0.811f, -0.468f, 0.35f },
};

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

static float R_AOWorldOcclusion( const vec3_t p, const vec3_t n )
{
	vec3_t tang, bitang, up, src;
	float dist = Q_max( 8.0f, r_ao_world_dist.value );
	float sum = 0.0f;
	int i;

	// tangent basis around the surface normal
	if( fabs( n[2] ) < 0.9f ) VectorSet( up, 0.0f, 0.0f, 1.0f );
	else VectorSet( up, 1.0f, 0.0f, 0.0f );
	CrossProduct( up, n, tang );
	VectorNormalize( tang );
	CrossProduct( n, tang, bitang );

	VectorMA( p, 2.0f, n, src );	// lift off the surface to avoid self-hits

	for( i = 0; i < AO_WORLD_RAYS; i++ )
	{
		const float *k = ao_world_kernel[i];
		vec3_t dir, end;
		pmtrace_t trace;

		dir[0] = k[0] * tang[0] + k[1] * bitang[0] + k[2] * n[0];
		dir[1] = k[0] * tang[1] + k[1] * bitang[1] + k[2] * n[1];
		dir[2] = k[0] * tang[2] + k[1] * bitang[2] + k[2] * n[2];
		VectorMA( src, dist, dir, end );

		trace = gEngfuncs.CL_TraceLine( src, end, PM_WORLD_ONLY );

		// the luxel is buried inside solid (e.g. a face inside the world, or a corner
		// where the lift pushed it through a wall). Every ray would startsolid -> a
		// black patch. Bail with a sentinel so the caller leaves it unlit instead.
		if( trace.startsolid || trace.allsolid )
			return -1.0f;

		// proximity-weighted: a close hit occludes fully, a far one barely - this
		// concentrates the AO in corners and keeps big open rooms from going grey.
		if( trace.fraction < 1.0f )
			sum += 1.0f - trace.fraction;
	}

	return sum / (float)AO_WORLD_RAYS;
}

void R_AOBakeWorld( void )
{
	model_t *world = WORLDMODEL;
	double t0 = gEngfuncs.pfnTime();
	int i, nsurf = 0, nlux = 0;

	if( !world )
	{
		gEngfuncs.Con_Printf( "r_ao_bake: no world loaded\n" );
		return;
	}

	// clear any previous AO pointers on this world's surfaces (NULL first so freeing
	// the old buffers below never leaves a dangling shadowmap), then free them.
	for( i = 0; i < world->numsurfaces; i++ )
		if( world->surfaces[i].info ) world->surfaces[i].info->shadowmap = NULL;

	R_AOWorldFree();
	ao_bufcount = world->numsurfaces;
	ao_bufs = Mem_Malloc( r_temppool, ao_bufcount * sizeof( byte * ));
	memset( ao_bufs, 0, ao_bufcount * sizeof( byte * ));

	for( i = world->firstmodelsurface; i < world->firstmodelsurface + world->nummodelsurfaces; i++ )
	{
		msurface_t *surf = &world->surfaces[i];
		mextrasurf_t *info = surf->info;
		int sample_size, smax, tmax, si, ti;
		float r0[3], r1[3], cc0[3], cc1[3], cc2[3], det, inv, pd;
		vec3_t n;
		byte *occmap;

		if( !surf->samples || FBitSet( surf->flags, SURF_DRAWSKY | SURF_DRAWTURB | SURF_DRAWTURB_QUADS | SURF_DRAWTILED ))
			continue;

		sample_size = gEngfuncs.Mod_SampleSizeForFace( surf );
		smax = ( info->lightextents[0] / sample_size ) + 1;
		tmax = ( info->lightextents[1] / sample_size ) + 1;
		if( smax < 1 || tmax < 1 )
			continue;

		// outward normal + plane constant (dot(P,n) = pd holds either way)
		VectorCopy( surf->plane->normal, n );
		pd = surf->plane->dist;
		if( FBitSet( surf->flags, SURF_PLANEBACK )) { VectorNegate( n, n ); pd = -pd; }

		// inverse of A = [ lmvecs0 ; lmvecs1 ; n ] (rows) via cross products
		VectorCopy( info->lmvecs[0], r0 );
		VectorCopy( info->lmvecs[1], r1 );
		CrossProduct( r1, n, cc0 );
		CrossProduct( n, r0, cc1 );
		CrossProduct( r0, r1, cc2 );
		det = DotProduct( r0, cc0 );
		if( fabs( det ) < 1.0e-9f )
			continue;
		inv = 1.0f / det;

		occmap = Mem_Malloc( r_temppool, smax * tmax );

		int buried = 0;
		for( ti = 0; ti < tmax; ti++ )
		{
			for( si = 0; si < smax; si++ )
			{
				float b0 = ( info->lightmapmins[0] + si * sample_size ) - info->lmvecs[0][3];
				float b1 = ( info->lightmapmins[1] + ti * sample_size ) - info->lmvecs[1][3];
				vec3_t p;
				float occ;

				p[0] = ( b0 * cc0[0] + b1 * cc1[0] + pd * cc2[0] ) * inv;
				p[1] = ( b0 * cc0[1] + b1 * cc1[1] + pd * cc2[1] ) * inv;
				p[2] = ( b0 * cc0[2] + b1 * cc1[2] + pd * cc2[2] ) * inv;

				occ = R_AOWorldOcclusion( p, n );
				if( occ < 0.0f ) { occ = 0.0f; buried++; }	// buried luxel -> leave unlit
				occmap[ti * smax + si] = (byte)( bound( 0.0f, occ, 1.0f ) * 255.0f );
				nlux++;
			}
		}

		// diagnostic: a surface that's mostly buried isn't a real visible face
		if( r_ao_debug.value && buried > ( smax * tmax ) / 2 )
		{
			texture_t *tx = surf->texinfo ? surf->texinfo->texture : NULL;
			gEngfuncs.Con_Printf( "  [AO] buried surf #%i tex='%s' flags=0x%x (%i/%i luxels)\n",
				i, tx ? tx->name : "?", surf->flags, buried, smax * tmax );
		}

		surf->info->shadowmap = occmap;	// per-surface: can't bleed onto other maps
		ao_bufs[i] = occmap;		// tracked for freeing
		nsurf++;
	}

	ao_baked_model = world;

	GL_RebuildLightmaps();	// re-run R_BuildLightMap so the AO layer is applied

	gEngfuncs.Con_Printf( "r_ao_bake: %i surfaces, %i luxels in %.2f s\n",
		nsurf, nlux, gEngfuncs.pfnTime() - t0 );
}

static void R_AOBakeWorld_f( void )
{
	R_AOBakeWorld();
}

/*
=================
R_AOWorldFrame

Auto-bake world AO the first time we're standing in a map with it enabled. Called
once per frame for the main view. A short delay after the world changes lets the
player spawn and the trace world come up before we cast occlusion rays. This is a
stop-gap for testing (a per-map hitch); the real path is the preload bake.
=================
*/
void R_AOWorldFrame( void )
{
	static model_t *seen = NULL;
	static int bakeframe = 0;

	if( r_ao_world.value <= 0.0f || !WORLDMODEL )
		return;
	if( WORLDMODEL == ao_baked_model )
		return;	// already baked this map

	if( WORLDMODEL != seen )
	{
		seen = WORLDMODEL;
		bakeframe = tr.framecount + 30;	// ~0.5s: let the map go live so traces work
		return;
	}
	if( tr.framecount < bakeframe )
		return;

	R_AOBakeWorld();
}

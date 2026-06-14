/*
host_aobake.c - offline baker for world ambient occlusion
Copyright (C) 2026 James Bishop

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

==============================================================================

xash3d-streaming: world AO used to be raycast on the main thread the first time
the player stood in each map - a hitch on every seamless level change. The bake
is the same per-luxel occlusion cast, but done once up front (at first launch,
or on demand) against the loaded BSP's point hull, with no server or client
running, and written to cache/ao/<map>.ao. The GL renderer then just reads the
cache when a map loads (gl_ao.c). See common/ao_cache.h for the file format.

The cast matches the old gl_ao.c path exactly: usehull 2 resolves to hulls[0]
with a zero offset (see PM_HullForBsp), i.e. a plain point traceline against the
world, so the cached result is identical to the live bake it replaces.
==============================================================================
*/

#include "common.h"
#include "mod_local.h"
#include "pm_local.h"
#include "pm_defs.h"
#include "xash3d_mathlib.h"
#include "ao_cache.h"
#if !XASH_DEDICATED
#include "ref_common.h"		// ref, refState - for the bake progress screen
#include "client.h"		// Con_DrawString / Con_DrawStringLen
#include "platform/platform.h"	// Platform_RunEvents - keep the window alive while blocking
#endif
#if XASH_SDL == 2
#include <SDL_thread.h>		// fan the per-surface raycasts out across CPU cores
#include <SDL_atomic.h>
#include <SDL_cpuinfo.h>
#endif

#define AO_WORLD_RAYS	13

// hemisphere kernel in tangent space (+Z = surface normal): centre + two rings.
// MUST match gl_ao.c; AO_CACHE_VERSION pins it so a change re-bakes stale caches.
static const float ao_world_kernel[AO_WORLD_RAYS][3] =
{
	{  0.000f,  0.000f, 1.00f },
	{  0.714f,  0.000f, 0.70f }, {  0.357f,  0.618f, 0.70f }, { -0.357f,  0.618f, 0.70f },
	{ -0.714f,  0.000f, 0.70f }, { -0.357f, -0.618f, 0.70f }, {  0.357f, -0.618f, 0.70f },
	{  0.811f,  0.468f, 0.35f }, {  0.000f,  0.937f, 0.35f }, { -0.811f,  0.468f, 0.35f },
	{ -0.811f, -0.468f, 0.35f }, {  0.000f, -0.937f, 0.35f }, {  0.811f, -0.468f, 0.35f },
};

// bake-quality cvars live here, not in the renderer, because the renderer no
// longer raycasts. dist changes the result so AO_CACHE_VERSION can't catch it;
// the per-map header stores the dist it baked with and re-bakes on mismatch.
static CVAR_DEFINE_AUTO( r_ao_world_dist, "72", FCVAR_ARCHIVE, "world-AO occlusion ray length in units (bake quality; re-bakes on change)" );
static CVAR_DEFINE_AUTO( r_ao_autobake, "1", FCVAR_ARCHIVE, "bake any missing world-AO caches for the campaign at launch (1) or not (0)" );

/*
=================
AO_Occlusion

point-trace the ray kernel against the world's point hull and return the
proximity-weighted occlusion at p (normal n), or -1 if the luxel is buried in
solid. Mirrors R_AOWorldOcclusion in gl_ao.c.
=================
*/
static float AO_Occlusion( model_t *world, const vec3_t p, const vec3_t n )
{
	hull_t *hull = &world->hulls[0];
	float  dist = Q_max( 8.0f, r_ao_world_dist.value );
	vec3_t tang, bitang, up, src;
	float  sum = 0.0f;
	int    i;

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
		vec3_t    dir, end;
		pmtrace_t trace;

		dir[0] = k[0] * tang[0] + k[1] * bitang[0] + k[2] * n[0];
		dir[1] = k[0] * tang[1] + k[1] * bitang[1] + k[2] * n[1];
		dir[2] = k[0] * tang[2] + k[1] * bitang[2] + k[2] * n[2];
		VectorMA( src, dist, dir, end );

		PM_InitPMTrace( &trace, end );
		PM_RecursiveHullCheck( hull, hull->firstclipnode, 0.0f, 1.0f, src, end, &trace );

		// buried luxel (every ray starts in solid) -> sentinel, leave it unlit
		if( trace.startsolid || trace.allsolid )
			return -1.0f;

		// proximity-weighted: a close hit occludes fully, a far one barely
		if( trace.fraction < 1.0f )
			sum += 1.0f - trace.fraction;
	}

	return sum / (float)AO_WORLD_RAYS;
}

/*
=================
AO_CachePath
=================
*/
static void AO_CachePath( const char *base, char *out, size_t size )
{
	Q_snprintf( out, size, "cache/ao/%s.ao", base );
}

/*
=================
AO_CacheIsValid

cheap check used to skip already-baked maps: the cache exists and its header
matches this build's format, ray count and current bake distance.
=================
*/
static qboolean AO_CacheIsValid( const char *base )
{
	char              path[MAX_QPATH];
	ao_cache_header_t hdr;
	file_t           *f;
	qboolean          ok;

	AO_CachePath( base, path, sizeof( path ));
	f = FS_Open( path, "rb", true );
	if( !f )
		return false;

	ok = ( FS_Read( f, &hdr, sizeof( hdr )) == sizeof( hdr ))
		&& hdr.magic == AO_CACHE_MAGIC
		&& hdr.version == AO_CACHE_VERSION
		&& hdr.rays == AO_WORLD_RAYS
		&& fabs( hdr.dist - r_ao_world_dist.value ) < 0.5f;

	FS_Close( f );
	return ok;
}

// one surface's bake work. Everything except the raycast is precomputed on the
// main thread (no allocation, no model/cvar churn in the worker), so workers
// only read the shared read-only hull and write their own buffer - PM_Recursive-
// HullCheck is pure, so this needs no locking.
typedef struct
{
	int    surf;                    // index into world->surfaces[] (cache record)
	int    smax, tmax, sample_size;
	int    lmmins0, lmmins1;
	float  lmv30, lmv31;            // lmvecs[0][3], lmvecs[1][3]
	float  pd, inv;
	vec3_t n, cc0, cc1, cc2;
	byte  *data;                    // smax*tmax bytes, pre-allocated by main
} ao_job_t;

static void AO_FillJob( model_t *world, const ao_job_t *j )
{
	int si, ti;

	for( ti = 0; ti < j->tmax; ti++ )
	{
		for( si = 0; si < j->smax; si++ )
		{
			float b0 = ( j->lmmins0 + si * j->sample_size ) - j->lmv30;
			float b1 = ( j->lmmins1 + ti * j->sample_size ) - j->lmv31;
			vec3_t p;
			float  occ;

			p[0] = ( b0 * j->cc0[0] + b1 * j->cc1[0] + j->pd * j->cc2[0] ) * j->inv;
			p[1] = ( b0 * j->cc0[1] + b1 * j->cc1[1] + j->pd * j->cc2[1] ) * j->inv;
			p[2] = ( b0 * j->cc0[2] + b1 * j->cc1[2] + j->pd * j->cc2[2] ) * j->inv;

			occ = AO_Occlusion( world, p, j->n );
			if( occ < 0.0f ) occ = 0.0f;	// buried luxel -> leave unlit
			j->data[ti * j->smax + si] = (byte)( bound( 0.0f, occ, 1.0f ) * 255.0f );
		}
	}
}

/*
=================
AO_RunJobs

raycast every job, fanned out across CPU cores. Surfaces are handed out by an
atomic counter (work-stealing balances the wildly varying luxel counts); each
worker only reads shared data and writes its own buffer, so no locking. Falls
back to a plain loop when SDL threads are unavailable or fail to start.
=================
*/
#define AO_MAX_THREADS 16

#if XASH_SDL == 2
typedef struct
{
	model_t      *world;
	ao_job_t     *jobs;
	int           njobs;
	SDL_atomic_t  next;
} ao_bakeset_t;

static int AO_WorkerThread( void *param )
{
	ao_bakeset_t *set = param;
	int i;

	while(( i = SDL_AtomicAdd( &set->next, 1 )) < set->njobs )
		AO_FillJob( set->world, &set->jobs[i] );
	return 0;
}
#endif

static void AO_RunJobs( model_t *world, ao_job_t *jobs, int njobs )
{
#if XASH_SDL == 2
	ao_bakeset_t set;
	SDL_Thread  *threads[AO_MAX_THREADS];
	int          nthreads = bound( 1, SDL_GetCPUCount(), AO_MAX_THREADS );
	int          t, made = 0;

	set.world = world;
	set.jobs  = jobs;
	set.njobs = njobs;
	SDL_AtomicSet( &set.next, 0 );

	for( t = 0; t < nthreads; t++ )
	{
		threads[made] = SDL_CreateThread( AO_WorkerThread, "ao_bake", &set );
		if( threads[made] ) made++;
	}

	if( made > 0 )
	{
		for( t = 0; t < made; t++ )
			SDL_WaitThread( threads[t], NULL );
		return;	// the atomic counter guarantees every job was taken
	}
	// thread creation failed entirely -> fall through to single-threaded
#endif
	{
		int i;
		for( i = 0; i < njobs; i++ )
			AO_FillJob( world, &jobs[i] );
	}
}

/*
=================
AO_BakeWorld

raycast every lit world surface and write cache/ao/<base>.ao. The serial setup
(deciding which surfaces bake, the luxel transform, buffer allocation) runs on
the main thread; the raycasting is fanned across cores by AO_RunJobs. Returns
the number of surfaces baked, or -1 on failure.
=================
*/
static int AO_BakeWorld( model_t *world, const char *base )
{
	char    path[MAX_QPATH];
	file_t *f;
	double  t0 = Sys_DoubleTime();
	int     i, nbaked = 0, nlux = 0;
	ao_cache_header_t hdr;
	ao_job_t *jobs = Mem_Malloc( host.mempool, world->nummodelsurfaces * sizeof( ao_job_t ));

	// pass 1 (main thread): pick bakeable surfaces, precompute the luxel->world
	// transform, allocate each occlusion buffer
	for( i = world->firstmodelsurface; i < world->firstmodelsurface + world->nummodelsurfaces; i++ )
	{
		msurface_t   *surf = &world->surfaces[i];
		mextrasurf_t *info = surf->info;
		int    sample_size, smax, tmax;
		float  r0[3], r1[3], cc0[3], cc1[3], cc2[3], det, pd;
		vec3_t n;
		ao_job_t *j;

		if( !surf->samples || !info || FBitSet( surf->flags, SURF_DRAWSKY | SURF_DRAWTURB | SURF_DRAWTURB_QUADS | SURF_DRAWTILED ))
			continue;

		sample_size = Mod_SampleSizeForFace( surf );
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

		j = &jobs[nbaked++];
		j->surf = i;
		j->smax = smax;
		j->tmax = tmax;
		j->sample_size = sample_size;
		j->lmmins0 = info->lightmapmins[0];
		j->lmmins1 = info->lightmapmins[1];
		j->lmv30 = info->lmvecs[0][3];
		j->lmv31 = info->lmvecs[1][3];
		j->pd = pd;
		j->inv = 1.0f / det;
		VectorCopy( n, j->n );
		VectorCopy( cc0, j->cc0 );
		VectorCopy( cc1, j->cc1 );
		VectorCopy( cc2, j->cc2 );
		j->data = Mem_Malloc( host.mempool, smax * tmax );
		nlux += smax * tmax;
	}

	// pass 2: raycast every job across all cores
	AO_RunJobs( world, jobs, nbaked );

	AO_CachePath( base, path, sizeof( path ));
	f = FS_Open( path, "wb", true );	// "wb" creates cache/ao/ as needed
	if( !f )
	{
		Con_Printf( S_ERROR "ao_bake: can't write %s\n", path );
		for( i = 0; i < nbaked; i++ ) Mem_Free( jobs[i].data );
		Mem_Free( jobs );
		return -1;
	}

	hdr.magic = AO_CACHE_MAGIC;
	hdr.version = AO_CACHE_VERSION;
	hdr.rays = AO_WORLD_RAYS;
	hdr.dist = r_ao_world_dist.value;
	hdr.numsurfaces = world->numsurfaces;
	hdr.numbaked = nbaked;
	FS_Write( f, &hdr, sizeof( hdr ));

	for( i = 0; i < nbaked; i++ )
	{
		ao_cache_surf_t rec;
		rec.surf = jobs[i].surf;
		rec.smax = (unsigned short)jobs[i].smax;
		rec.tmax = (unsigned short)jobs[i].tmax;
		FS_Write( f, &rec, sizeof( rec ));
		FS_Write( f, jobs[i].data, jobs[i].smax * jobs[i].tmax );
		Mem_Free( jobs[i].data );
	}

	FS_Close( f );
	Mem_Free( jobs );

	Con_Reportf( "^3[ao]^7 bake %s: %i surfaces, %i luxels in %.2f s\n",
		base, nbaked, nlux, Sys_DoubleTime() - t0 );
	return nbaked;
}

/*
=================
AO_DrawProgress

draw the one-time bake progress screen: a centred bar with "Processing <map>..."
below it. We present our own frame (no menu/HUD yet) and pump window events so
the window stays responsive through the blocking bake. No-op on a dedicated
server (no renderer).
=================
*/
#if !XASH_DEDICATED
static void AO_DrawProgress( int done, int total, const char *map )
{
	const rgba_t ink = { 232, 232, 236, 255 };
	char  msg[96];
	int   w, h, barw, barh, bx, by, fillw, tw, th;
	float frac = total > 0 ? (float)done / (float)total : 0.0f;

	if( !ref.initialized )
		return;

	w = refState.width;
	h = refState.height;
	barw = bound( 240, w / 2, 900 );
	barh = 22;
	bx = ( w - barw ) / 2;
	by = h / 2;
	fillw = (int)( barw * bound( 0.0f, frac, 1.0f ));

	ref.dllFuncs.R_BeginFrame( true );
	ref.dllFuncs.R_Set2DMode( true );

	ref.dllFuncs.FillRGBA( kRenderTransTexture, 0, 0, w, h, 12, 13, 16, 255 );	// backdrop

	Q_strncpy( msg, "Preparing ambient occlusion", sizeof( msg ));
	Con_DrawStringLen( msg, &tw, &th );
	Con_DrawString(( w - tw ) / 2, by - barh - th * 3, msg, ink );

	ref.dllFuncs.FillRGBA( kRenderTransTexture, bx - 2, by - 2, barw + 4, barh + 4, 64, 66, 74, 255 );	// border
	ref.dllFuncs.FillRGBA( kRenderTransTexture, bx, by, barw, barh, 24, 26, 32, 255 );			// track
	ref.dllFuncs.FillRGBA( kRenderTransTexture, bx, by, fillw, barh, 255, 163, 26, 255 );			// fill (continuum amber)

	if( map[0] )
	{
		Q_snprintf( msg, sizeof( msg ), "Processing %s...", map );
		Con_DrawStringLen( msg, &tw, &th );
		Con_DrawString(( w - tw ) / 2, by + barh + th, msg, ink );
	}

	ref.dllFuncs.R_EndFrame();
	Platform_RunEvents();	// service the window so the WM doesn't flag it unresponsive
}
#else
static void AO_DrawProgress( int done, int total, const char *map ) { }
#endif

/*
=================
Host_BakeAO

bake the world AO for every campaign map that needs it, behind a progress
screen (blocking, before the menu appears). force re-bakes every map even if a
valid cache exists. Each map loads into model slot 0 just like world_preload,
so this also warms the residency cache the subsequent preload would have filled.
=================
*/
static void Host_BakeAO( qboolean force )
{
	search_t *search;
	char     (*todo)[64];
	int      i, ntodo = 0;

	if( SV_Active( ))
		return;	// never bake while a map is live (would displace world slot 0)

	search = FS_Search( "maps/*.bsp", true, true );
	if( !search )
		return;

	todo = Mem_Malloc( host.mempool, search->numfilenames * sizeof( todo[0] ));

	for( i = 0; i < search->numfilenames; i++ )
	{
		char base[64];
		int  j;

		COM_FileBase( search->filenames[i], base, sizeof( base ));
		Q_strnlwr( base, base, sizeof( base ));

		if( !force && AO_CacheIsValid( base ))
			continue;

		// the same map can appear loose and in a pak; bake it once
		for( j = 0; j < ntodo; j++ )
			if( !Q_stricmp( todo[j], base )) break;
		if( j < ntodo )
			continue;

		Q_strncpy( todo[ntodo++], base, sizeof( todo[0] ));
	}

	Mem_Free( search );

	if( ntodo )
	{
		Con_Printf( "[ao] baking %i map%s of world ambient occlusion (one-time)\n", ntodo, ntodo == 1 ? "" : "s" );

		for( i = 0; i < ntodo; i++ )
		{
			char     name[MAX_QPATH];
			model_t *world;

			AO_DrawProgress( i, ntodo, todo[i] );	// draw before this map's (blocking) bake

			Q_snprintf( name, sizeof( name ), "maps/%s.bsp", todo[i] );
			if( !FS_FileExists( name, false ))
				continue;

			// loads into model slot 0, parking the previous world in the
			// residency cache exactly like world_preload
			world = Mod_LoadWorld( name, true );
			if( world )
				AO_BakeWorld( world, todo[i] );
		}

		AO_DrawProgress( ntodo, ntodo, "" );	// final 100% frame
	}

	Mem_Free( todo );
}

static void AO_BakeAll_f( void )
{
	Host_BakeAO( true );
}

/*
=================
Host_InitAOBake

register the bake cvars and command. Called from Mod_Init so r_ao_world_dist
exists before the configs run.
=================
*/
void Host_InitAOBake( void )
{
	Cvar_RegisterVariable( &r_ao_world_dist );
	Cvar_RegisterVariable( &r_ao_autobake );
	Cmd_AddCommand( "r_ao_bake_all", AO_BakeAll_f, "re-bake world AO caches for every map in this game" );
}

/*
=================
Host_AutoBakeAO

at startup, bake any maps missing a valid cache (first launch with a game bakes
them all) behind the progress screen. Gated by r_ao_autobake; client only.
=================
*/
void Host_AutoBakeAO( void )
{
	if( Host_IsDedicated( ))
		return;	// no renderer to bake world AO for
	if( !r_ao_autobake.value )
		return;
	Host_BakeAO( false );
}

/*
model.c - modelloader
Copyright (C) 2007 Uncle Mike

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/
#include "common.h"
#include "mod_local.h"
#include "sprite.h"
#include "xash3d_mathlib.h"
#include "alias.h"
#include "studio.h"
#include "wadfile.h"
#include "world.h"
#include "enginefeatures.h"
#include "client.h"
#include "server.h"

static model_info_t	mod_crcinfo[MAX_MODELS];
static model_t	mod_known[MAX_MODELS];
static int	mod_numknown = 0;
poolhandle_t      com_studiocache;		// cache for submodels
CVAR_DEFINE( mod_studiocache, "r_studiocache", "1", FCVAR_ARCHIVE, "enables studio cache for speedup tracing hitboxes" );
CVAR_DEFINE_AUTO( r_wadtextures, "0", FCVAR_LATCH, "completely ignore textures in the bsp-file if enabled" );
CVAR_DEFINE_AUTO( r_showhull, "0", 0, "draw collision hulls 1-3" );
CVAR_DEFINE_AUTO( r_allow_wad3_luma, "0", FCVAR_LATCH|FCVAR_ARCHIVE, "allow usage of luma textures in wad3 (tilde textures)" );
static CVAR_DEFINE_AUTO( mod_world_residency, "1", 0, "keep parsed world models resident across changelevels for instant revisits" );
static void Mod_FreeCachedWorlds( void );
static void Mod_PreloadWorld_f( void );

/*
===============================================================================

			MOD COMMON UTILS

===============================================================================
*/
/*
================
Mod_Modellist_f
================
*/
static void Mod_Modellist_f( void )
{
	int	i, nummodels;
	model_t	*mod;

	Con_Printf( "\n" );
	Con_Printf( "-----------------------------------\n" );

	for( i = nummodels = 0, mod = mod_known; i < mod_numknown; i++, mod++ )
	{
		const char *color_str;

		if( mod->needload == NL_UNREFERENCED )
			continue; // free slot

		switch( mod->type )
		{
		case mod_alias:
			color_str = S_YELLOW;
			break;
		case mod_studio:
			color_str = S_GREEN;
			break;
		case mod_sprite:
			color_str = S_MAGENTA;
			break;
		case mod_brush:
			color_str = mod->name[0] == '*' ? S_CYAN : S_BLUE;
			break;
		default:
			color_str = S_RED;
			break;
		}

		Con_Printf( "%3d:\t%s%s\n" S_DEFAULT, i, color_str, mod->name );
		nummodels++;
	}

	Con_Printf( "-----------------------------------\n" );
	Con_Printf( "%i total models, %i total allocated slots\n", nummodels, mod_numknown );
	Con_Printf( "\n" );
}

static void Mod_UnloadRenderData( model_t *mod )
{
#if !XASH_DEDICATED
	switch( mod->type )
	{
	case mod_sprite:
		Mod_SpriteUnloadTextures( mod->cache.data );
		break;
	default:
		break;
	}

	ref.dllFuncs.Mod_ProcessRenderData( mod, false, NULL, 0 );
#endif
}

/*
================
Mod_FreeUserData
================
*/
static void Mod_FreeUserData( model_t *mod )
{
	// ignore submodels and freed models
	if( mod->needload == NL_UNREFERENCED || mod->name[0] == '*' )
		return;

	if( Host_IsDedicated() )
	{
		if( svgame.physFuncs.Mod_ProcessUserData != NULL )
		{
			// let the server.dll free custom data
			svgame.physFuncs.Mod_ProcessUserData( mod, false, NULL );
		}
	}
	else
	{
		Mod_UnloadRenderData( mod );
	}
}

/*
================
Mod_FreeModel
================
*/
void Mod_FreeModel( model_t *mod )
{
	// already freed?
	if( !mod || mod->needload == NL_UNREFERENCED )
		return;

	if( mod->type != mod_brush || mod->name[0] != '*' )
	{
		Mod_FreeUserData( mod );
		Mem_FreePool( &mod->mempool );
	}

	if( mod->type == mod_brush && FBitSet( mod->flags, MODEL_WORLD ) )
	{
		world.version = 0;
		world.shadowdata = NULL;
		world.deluxedata = NULL;

		// data already freed by Mem_FreePool above
		world.hull_models = NULL;
		world.compressed_phs = NULL;
		world.phsofs = NULL;
	}

	memset( mod, 0, sizeof( *mod ));
}

/*
===============================================================================

			MODEL INITIALIZE\SHUTDOWN

===============================================================================
*/
/*
================
Mod_Init
================
*/
void Mod_Init( void )
{
	com_studiocache = Mem_AllocPool( "Studio Cache" );
	Cvar_RegisterVariable( &mod_studiocache );
	Cvar_RegisterVariable( &mod_world_residency );
	Cvar_RegisterVariable( &r_wadtextures );
	Cvar_RegisterVariable( &r_showhull );
	Cvar_RegisterVariable( &r_allow_wad3_luma );

	Cmd_AddCommand( "mapstats", Mod_PrintWorldStats_f, "show stats for currently loaded map" );
	Cmd_AddCommand( "modellist", Mod_Modellist_f, "display loaded models list" );
	Cmd_AddCommand( "world_preload", Mod_PreloadWorld_f, "load a map's world into the residency cache ahead of time" );

	Mod_ResetStudioAPI ();
	Mod_InitStudioHull ();
}

/*
================
Mod_FreeAll
================
*/
void Mod_FreeAll( void )
{
#if !XASH_DEDICATED
	Mod_ReleaseHullPolygons();
#endif
	Mod_FreeCachedWorlds();

	for( int i = 0; i < mod_numknown; i++ )
		Mod_FreeModel( &mod_known[i] );
	mod_numknown = 0;
}

/*
================
Mod_ClearUserData
================
*/
void Mod_ClearUserData( void )
{
	// called when the client game library unloads (game change/shutdown):
	// the next game has different BSPs, drop the cache with its texture
	// handles. NOT hit by video mode changes — those keep the GL context,
	// and the cache (verified) survives them.
	Mod_FreeCachedWorlds();

	for( int i = 0; i < mod_numknown; i++ )
		Mod_FreeUserData( &mod_known[i] );
}

/*
================
Mod_Shutdown
================
*/
void Mod_Shutdown( void )
{
	Mod_FreeAll();
	Mem_FreePool( &com_studiocache );
}

/*
===============================================================================

			MODELS MANAGEMENT

===============================================================================
*/
/*
==================
Mod_FindName

never return NULL
==================
*/
model_t *Mod_FindName( const char *filename, qboolean trackCRC )
{
	char	modname[MAX_QPATH];
	model_t	*mod;
	int	i;

	Q_strncpy( modname, filename, sizeof( modname ));

	// search the currently loaded models
	for( i = 0, mod = mod_known; i < mod_numknown; i++, mod++ )
	{
		if( !Q_stricmp( mod->name, modname ))
		{
			if( mod->mempool || mod->name[0] == '*' )
				mod->needload = NL_PRESENT;
			else
				mod->needload = NL_NEEDS_LOADED;

			return mod;
		}
	}

	// find a free model slot spot
	for( i = 0, mod = mod_known; i < mod_numknown; i++, mod++ )
	{
		if( mod->needload == NL_UNREFERENCED )
			break; // this is a valid spot
	}

	if( i == mod_numknown )
	{
		if( mod_numknown == MAX_MODELS )
			Host_Error( "MAX_MODELS limit exceeded (%d)\n", MAX_MODELS );
		mod_numknown++;
	}

	// copy name, so model loader can find model file
	Q_strncpy( mod->name, modname, sizeof( mod->name ));
	if( trackCRC ) mod_crcinfo[i].flags = FCRC_SHOULD_CHECKSUM;
	else mod_crcinfo[i].flags = 0;
	mod->needload = NL_NEEDS_LOADED;
	mod_crcinfo[i].initialCRC = 0;

	return mod;
}

/*
==================
Mod_LoadModel

Loads a model into the cache
==================
*/
static model_t *Mod_LoadModel( model_t *mod, qboolean crash )
{
	char		tempname[MAX_QPATH];
	fs_offset_t		length = 0;
	qboolean		loaded, loaded2 = false;

	if( !mod )
	{
		Host_Error( "%s: mod == NULL\n", __func__ );
		return NULL;
	}

	// check if already loaded (or inline bmodel)
	if( mod->mempool || mod->name[0] == '*' )
	{
		mod->needload = NL_PRESENT;
		return mod;
	}

	if( mod->needload != NL_NEEDS_LOADED )
	{
		Host_Error( "%s: trying to load model not marked for loading (%d)\n", __func__, mod->needload );
		return NULL;
	}

	// store modelname to show error
	Q_strncpy( tempname, mod->name, sizeof( tempname ));
	COM_FixSlashes( tempname );

	byte *buf = FS_LoadFile( tempname, &length, false );

	if( !buf || length < sizeof( uint ))
	{
		memset( mod, 0, sizeof( model_t ));

		if( crash ) Host_Error( "Could not load model %s from disk\n", tempname );
		else Con_Printf( S_ERROR "Could not load model %s from disk\n", tempname );

		return NULL;
	}

	Con_Reportf( "loading %s\n", mod->name );
	mod->needload = NL_PRESENT;
	mod->type = mod_bad;

	// call the apropriate loader
	switch( *(uint *)buf )
	{
	case LittleLong( IDSTUDIOHEADER ):
		Mod_LoadStudioModel( mod, buf, length, &loaded );
		break;
	case LittleLong( IDSPRITEHEADER ):
		Mod_LoadSpriteModel( mod, buf, length, &loaded );
		break;
	case LittleLong( IDALIASHEADER ):
		Mod_LoadAliasModel( mod, buf, &loaded );
		break;
	case LittleLong( Q1BSP_VERSION ):
	case LittleLong( HLBSP_VERSION ):
	case LittleLong( QBSP2_VERSION ):
		Mod_LoadBrushModel( mod, buf, length, &loaded );
		break;
	default:
		Mem_Free( buf );
		if( crash ) Host_Error( "%s has unknown format\n", tempname );
		else Con_Printf( S_ERROR "%s has unknown format\n", tempname );
		return NULL;
	}

	if( loaded )
	{
		if( world.loading )
			SetBits( mod->flags, MODEL_WORLD ); // mark worldmodel

		if( Host_IsDedicated() )
		{
			if( svgame.physFuncs.Mod_ProcessUserData != NULL )
			{
				// let the server.dll load custom data
				svgame.physFuncs.Mod_ProcessUserData( mod, true, buf );
			}
			loaded2 = true;
		}
#if !XASH_DEDICATED
		else
		{
			loaded2 = ref.dllFuncs.Mod_ProcessRenderData( mod, true, buf, length );
		}
#endif
	}

	if( mod->type == mod_alias )
	{
		aliashdr_t *hdr = mod->cache.data;
		if( hdr ) // clean up temporary pointer after passing the alias model to the renderer
			hdr->pposeverts = NULL;
	}

	if( !loaded || !loaded2 )
	{
		Mod_FreeModel( mod );
		Mem_Free( buf );

		if( crash ) Host_Error( "Could not load model %s\n", tempname );
		else Con_Printf( S_ERROR "Could not load model %s\n", tempname );

		return NULL;
	}

	model_info_t *p = &mod_crcinfo[mod - mod_known];
	mod->needload = NL_PRESENT;

	if( FBitSet( p->flags, FCRC_SHOULD_CHECKSUM ))
	{
		uint32_t currentCRC;

		CRC32_Init( &currentCRC );
		CRC32_ProcessBuffer( &currentCRC, buf, length );
		currentCRC = CRC32_Final( currentCRC );

		if( FBitSet( p->flags, FCRC_CHECKSUM_DONE ))
		{
			if( currentCRC != p->initialCRC )
				Host_Error( "%s has a bad checksum\n", tempname );
		}
		else
		{
			SetBits( p->flags, FCRC_CHECKSUM_DONE );
			p->initialCRC = currentCRC;
		}
	}
	Mem_Free( buf );

	return mod;
}

/*
==================
Mod_ForName

Loads in a model for the given name
==================
*/
model_t *Mod_ForName( const char *name, qboolean crash, qboolean trackCRC )
{
	if( COM_StringEmptyOrNULL( name ))
		return NULL;

	model_t *mod = Mod_FindName( name, trackCRC );
	return Mod_LoadModel( mod, crash );
}

/*
==============================================================================
WORLD RESIDENCY CACHE (xash3d-streaming, M3)

Parsed world models (slot #0 + their "*N" submodel entries + the derived
world_static_t globals) are moved to a side-cache on changelevel instead of
being freed. Revisiting a cached map skips file load, BSP parsing and texture
upload entirely. The cache owns the worlds' memory pools; the active world in
slot #0 borrows its pool from the cache. mod_world_residency 0 restores the
legacy free-on-changelevel behavior.
==============================================================================
*/
typedef struct worldcache_s
{
	struct worldcache_s	*next;
	char		name[MAX_QPATH];
	model_t		world;		// snapshot of mod_known[0]
	model_t		*submodels;	// snapshots of this world's "*N" entries
	int		numsubmodels;
	world_static_t	worldstate;	// snapshot of the `world` global after load
} worldcache_t;

static worldcache_t	*wc_list;

static worldcache_t *Mod_FindCachedWorld( const char *name )
{
	worldcache_t	*wc;

	for( wc = wc_list; wc != NULL; wc = wc->next )
	{
		if( !Q_stricmp( wc->name, name ))
			return wc;
	}
	return NULL;
}

/*
==================
Mod_CacheCurrentWorld

snapshot slot #0 + submodels + world globals into the cache and clear the
slots WITHOUT freeing the world's pool (the cache owns it from now on).
returns false if there is no world to cache.
==================
*/
static qboolean Mod_CacheCurrentWorld( void )
{
	model_t		*w = mod_known;
	worldcache_t	*wc;
	int		i;

	if( !w->name[0] || w->type != mod_brush || !w->mempool )
		return false;

	if(( wc = Mod_FindCachedWorld( w->name )) == NULL )
	{
		wc = Mem_Calloc( host.mempool, sizeof( *wc ));
		Q_strncpy( wc->name, w->name, sizeof( wc->name ));
		wc->next = wc_list;
		wc_list = wc;
	}
	else if( wc->submodels )
	{
		Mem_Free( wc->submodels );
		wc->submodels = NULL;
	}

	wc->world = *w;
	wc->worldstate = world;
	wc->worldstate.loading = false;

	// debug hull polys are released on changelevel; don't carry stale pointers
	wc->worldstate.hull_models = NULL;
	wc->worldstate.num_hull_models = 0;

	wc->numsubmodels = 0;
	if( w->numsubmodels > 1 )
		wc->submodels = Mem_Calloc( host.mempool, sizeof( model_t ) * ( w->numsubmodels - 1 ));

	for( i = 1; i < mod_numknown; i++ )
	{
		if( mod_known[i].needload == NL_UNREFERENCED || mod_known[i].name[0] != '*' )
			continue;

		if( wc->submodels && wc->numsubmodels < w->numsubmodels - 1 )
			wc->submodels[wc->numsubmodels++] = mod_known[i];

		memset( &mod_known[i], 0, sizeof( model_t )); // submodels never own a pool
	}

	memset( w, 0, sizeof( model_t )); // pool now owned by the cache entry
	world.version = 0;
	world.shadowdata = NULL;
	world.deluxedata = NULL;
	world.hull_models = NULL;
	world.compressed_phs = NULL;
	world.phsofs = NULL;

	// message/compiler/generator/wadlist are host.mempool allocations that were
	// snapshotted into wc->worldstate above. Drop the live aliases so the next
	// world load's Mem_Free (Mod_LoadEntities) frees only fresh data and can
	// never dangle the cached copies - the cache entry solely owns them now.
	world.message = NULL;
	world.compiler = NULL;
	world.generator = NULL;
	world.wadlist = NULL;
	world.wadcount = 0;

	return true;
}

/*
==================
Mod_RestoreCachedWorld

put a cached world back into slot #0, recreate its submodel entries and
restore the world globals. skips file load/parse/texture upload entirely.
==================
*/
static qboolean Mod_RestoreCachedWorld( const char *name )
{
	worldcache_t	*wc = Mod_FindCachedWorld( name );

	if( wc == NULL )
		return false;

	*mod_known = wc->world;
	mod_known->needload = NL_PRESENT;
	world = wc->worldstate;

	for( int i = 0; i < wc->numsubmodels; i++ )
	{
		model_t	*sub = Mod_FindName( wc->submodels[i].name, true );

		*sub = wc->submodels[i];
		sub->needload = NL_PRESENT;
	}

	Con_Reportf( "%s: restored %s from residency cache\n", __func__, name );
	return true;
}

/*
==================
Mod_FreeCachedWorlds

full purge (server shutdown, vid restart). frees the cached pools; if slot #0
currently borrows a cached pool, its slots are cleared here so the caller's
Mod_FreeModel does not double-free.
==================
*/
static void Mod_FreeCachedWorlds( void )
{
	worldcache_t	*wc, *next;

	for( wc = wc_list; wc != NULL; wc = next )
	{
		next = wc->next;

		if( mod_known->mempool == wc->world.mempool && mod_known->name[0] )
		{
			// active world borrows this pool: clear its slots, we own the free
			for( int i = 1; i < mod_numknown; i++ )
			{
				if( mod_known[i].name[0] == '*' )
					memset( &mod_known[i], 0, sizeof( model_t ));
			}
			memset( mod_known, 0, sizeof( model_t ));
			world.version = 0;
			world.shadowdata = NULL;
			world.deluxedata = NULL;
			world.hull_models = NULL;
			world.compressed_phs = NULL;
			world.phsofs = NULL;

			// the active world aliases this entry's host.mempool strings
			// (restored from it); null the live copies before we free them
			// below so we never double-free or leave a dangling world.message.
			world.message = NULL;
			world.compiler = NULL;
			world.generator = NULL;
			world.wadlist = NULL;
			world.wadcount = 0;
		}

		// release the host.mempool strings this cache entry owns
		Mem_Free( wc->worldstate.message );
		Mem_Free( wc->worldstate.compiler );
		Mem_Free( wc->worldstate.generator );
		Mem_Free( wc->worldstate.wadlist );

		Mem_FreePool( &wc->world.mempool );
		if( wc->submodels )
			Mem_Free( wc->submodels );
		Mem_Free( wc );
	}
	wc_list = NULL;
}

/*
==================
Mod_PreloadWorld_f

load a map's world model ahead of time and park it in the residency cache,
so the eventual changelevel to it is a near-instant revisit. only allowed
while no server is running (a preload would displace the active world in
slot #0); meant to run behind the menu at startup, one map per frame.
==================
*/
static void Mod_PreloadWorld_f( void )
{
	char	name[MAX_QPATH];
	double	t;

	if( Cmd_Argc() != 2 )
	{
		Con_Printf( S_USAGE "world_preload <mapname>\n" );
		return;
	}

	if( !mod_world_residency.value )
	{
		Con_Printf( S_WARN "world_preload requires mod_world_residency 1\n" );
		return;
	}

	if( !host_level_streaming.value )
		return; // streaming disabled by the user, silently skip preloads

	if( SV_Active( ))
	{
		Con_Printf( S_WARN "world_preload: can't preload while a server is running\n" );
		return;
	}

	Q_snprintf( name, sizeof( name ), "maps/%s.bsp", Cmd_Argv( 1 ));

	if( Mod_FindCachedWorld( name ) || !Q_stricmp( mod_known->name, name ))
		return; // already resident

	if( !FS_FileExists( name, false ))
	{
		Con_Printf( S_WARN "world_preload: %s not found\n", name );
		return;
	}

	t = Sys_DoubleTime();
	Mod_LoadWorld( name, true );
	Con_Reportf( "^3[streamprof]^7 world_preload %s: %.2f ms\n", name, ( Sys_DoubleTime() - t ) * 1000.0 );
}

/*
==================
Mod_PurgeStudioCache

free studio cache on change level
==================
*/
static void Mod_PurgeStudioCache( void )
{
	// refresh hull data
	SetBits( r_showhull.flags, FCVAR_CHANGED );
#if !XASH_DEDICATED
	Mod_ReleaseHullPolygons();
#endif
	// release previois map (or move it into the residency cache)
	if( !mod_world_residency.value || !Mod_CacheCurrentWorld( ))
		Mod_FreeModel( mod_known );	// world is stuck on slot #0 always

	// we should release all the world submodels
	// and clear studio sequences
	for( int i = 1; i < mod_numknown; i++ )
	{
		if( mod_known[i].needload == NL_UNREFERENCED )
			continue;

		if( mod_known[i].type == mod_studio )
			mod_known[i].submodels = NULL;

		if( mod_known[i].name[0] == '*' )
			Mod_FreeModel( &mod_known[i] );
		else
			mod_known[i].needload = NL_FREE_UNUSED;
	}

	Mem_EmptyPool( com_studiocache );
	Mod_ClearStudioCache();
}

/*
==================
Mod_LoadWorld

Loads in the map and all submodels
==================
*/
model_t *Mod_LoadWorld( const char *name, qboolean preload )
{
	// already loaded?
	if( !Q_stricmp( mod_known->name, name ))
		return mod_known;

	// free sequence files on studiomodels (and cache the current world)
	Mod_PurgeStudioCache();

	// revisited map: restore from the residency cache, skipping the load
	if( mod_world_residency.value && Mod_RestoreCachedWorld( name ))
		return mod_known;

	// load the newmap
	world.loading = true;
	model_t *pworld = Mod_FindName( name, false );
	if( preload ) Mod_LoadModel( pworld, true );
	world.loading = false;

	ASSERT( pworld == mod_known );

	return pworld;
}

/*
==================
Mod_FreeUnused

Purge all unused models
==================
*/
void Mod_FreeUnused( void )
{
	model_t	*mod;
	int	i;

	// never tries to release worldmodel
	for( i = 1, mod = &mod_known[1]; i < mod_numknown; i++, mod++ )
	{
		if( mod->needload == NL_FREE_UNUSED )
			Mod_FreeModel( mod );
	}
}

/*
===============================================================================

			MODEL ROUTINES

===============================================================================
*/
/*
===============
Mod_Calloc

===============
*/
void *GAME_EXPORT Mod_Calloc( int number, size_t size )
{
	if( number <= 0 || size <= 0 )
		return NULL;

	cache_user_t *cu = (cache_user_t *)Mem_Calloc( com_studiocache, sizeof( cache_user_t ) + number * size );
	cu->data = (void *)cu; // make sure that cu->data is not NULL

	return cu;
}

/*
===============
Mod_CacheCheck

===============
*/
void *GAME_EXPORT Mod_CacheCheck( cache_user_t *c )
{
	if( !c->data )
		return NULL;

	if( !Mem_IsAllocatedExt( com_studiocache, c->data ))
		return NULL;

	return c->data;
}

/*
===============
Mod_LoadCacheFile

===============
*/
void GAME_EXPORT Mod_LoadCacheFile( const char *filename, cache_user_t *cu )
{
	char	modname[MAX_QPATH];
	fs_offset_t	size;

	Assert( cu != NULL );

	if( COM_StringEmptyOrNULL( filename ))
		return;

	Q_strncpy( modname, filename, sizeof( modname ));
	COM_FixSlashes( modname );

	byte *buf = FS_LoadFile( modname, &size, false );
	if( !buf || !size ) Host_Error( "LoadCacheFile: ^1can't load %s^7\n", filename );
	cu->data = Mem_Malloc( com_studiocache, size );
	memcpy( cu->data, buf, size );
	Mem_Free( buf );

	// this handles when studio model renderer tries to load sequence files on it's own
	// which is what they always do in HLSDK
#if XASH_BIG_ENDIAN
	if( size >= sizeof( int ) && LittleLong( IDSEQGRPHEADER ) == *(uint *)cu->data )
	{
		studiohdr_t *phdr = (studiohdr_t *)REF_GET_PARM( PARM_GET_STUDIO_HDR, 0 );
		if( !phdr )
			return;

		mstudioseqdesc_t *pseq = (mstudioseqdesc_t *)((byte *)phdr + phdr->seqindex );

		for( int i = 0; i < phdr->numseq; i++ )
		{
			if( pseq[i].seqgroup == 0 )
				continue;

			mstudioseqgroup_t *pgrp = (mstudioseqgroup_t *)((byte *)phdr + phdr->seqgroupindex ) + pseq[i].seqgroup;

			// assuming filename passes seqgroup's name
			if( !Q_stricmp( pgrp->name, filename ))
				Mod_SwapStudioSeqGroupAnims( phdr, &pseq[i], (byte *)cu->data );
		}
	}
#endif
}

/*
==================
Mod_ValidateCRC

==================
*/
qboolean Mod_ValidateCRC( const char *name, uint32_t crc )
{
	model_t *mod = Mod_FindName( name, true );
	model_info_t *p = &mod_crcinfo[mod - mod_known];

	if( !FBitSet( p->flags, FCRC_CHECKSUM_DONE ))
		return true;
	if( p->initialCRC == crc )
		return true;
	return false;
}

/*
==================
Mod_NeedCRC

==================
*/
void Mod_NeedCRC( const char *name, qboolean needCRC )
{
	model_t *mod = Mod_FindName( name, true );
	model_info_t *p = &mod_crcinfo[mod - mod_known];

	if( needCRC ) SetBits( p->flags, FCRC_SHOULD_CHECKSUM );
	else ClearBits( p->flags, FCRC_SHOULD_CHECKSUM );
}

#if XASH_ENGINE_TESTS

static const uint8_t *fuzz_data;
static size_t fuzz_size;

static byte *Fuzz_LoadFile( const char *path, fs_offset_t *filesizeptr, qboolean gamedironly )
{
	byte *buf = Mem_Malloc( host.mempool, fuzz_size );
	memcpy( buf, fuzz_data, fuzz_size );
	*filesizeptr = fuzz_size;
	return buf;
}

int EXPORT Fuzz_Mod_LoadModel( const uint8_t *Data, size_t Size );
int EXPORT Fuzz_Mod_LoadModel( const uint8_t *Data, size_t Size )
{
	model_t mod = { .name = "test", .needload = NL_NEEDS_LOADED };

	Memory_Init();

	host.type = HOST_DEDICATED;
	host.mempool = Mem_AllocPool( "fuzzing pool" );
	fuzz_data = Data;
	fuzz_size = Size;
	refState.draw_surfaces = NULL;

	g_fsapi.LoadFile = Fuzz_LoadFile;

	if( Mod_LoadModel( &mod, false ) && mod.mempool )
		Mem_FreePool( &mod.mempool );

	Mem_FreePool( &host.mempool );

	return 0;
}

#endif // XASH_ENGINE_TESTS

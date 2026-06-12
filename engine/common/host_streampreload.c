/*
host_streampreload.c - self-contained campaign preload graph
Copyright (C) 2026 a1batross, James Bishop

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

==============================================================================

xash3d-streaming: the campaign preload list used to be derived by an external
Python tool writing streampreload.cfg. The engine now derives it itself: scan
the game's maps through the filesystem (loose files and pak archives alike),
read each BSP's entity lump for trigger_changelevel links, and walk the
resulting graph breadth-first from the campaign start so the maps nearest the
beginning warm up first. Isolated maps (multiplayer arenas etc.) never join
the graph and are not preloaded. A hand-written streampreload.cfg in the game
dir still overrides all of this (see Host_Exec handling in host.c).
==============================================================================
*/

#include "common.h"
#include "bspfile.h"

#define SG_MAX_MAPS    1024
#define SG_MAX_EDGES   4096
#define SG_MAX_ENTLUMP ( 4 * 1024 * 1024 )

typedef struct
{
	char     name[64];
	qboolean visited;
} sg_node_t;

typedef struct
{
	short a, b;
} sg_edge_t;

static sg_node_t *sg_nodes;
static int       sg_numnodes;
static sg_edge_t *sg_edges;
static int       sg_numedges;

static int SG_FindNode( const char *name )
{
	int i;

	for( i = 0; i < sg_numnodes; i++ )
	{
		if( !Q_stricmp( sg_nodes[i].name, name ))
			return i;
	}
	return -1;
}

static int SG_CompareNodes( const void *a, const void *b )
{
	return Q_stricmp(((const sg_node_t *)a)->name, ((const sg_node_t *)b)->name );
}

static void SG_AddEdge( int a, int b )
{
	int i;

	if( a == b || a < 0 || b < 0 )
		return;

	for( i = 0; i < sg_numedges; i++ )
	{
		if(( sg_edges[i].a == a && sg_edges[i].b == b ) || ( sg_edges[i].a == b && sg_edges[i].b == a ))
			return;
	}

	if( sg_numedges >= SG_MAX_EDGES )
		return;

	sg_edges[sg_numedges].a = a;
	sg_edges[sg_numedges].b = b;
	sg_numedges++;
}

/*
================
SG_LoadEntityLump

read just the entity lump of a HL BSP through the filesystem (works inside
pak archives). Blue Shift maps have the entities and planes lumps swapped;
detect that the same way the original tool did, by looking for "classname"
================
*/
static char *SG_LoadEntityLump( const char *filename )
{
	dheader_t hdr;
	file_t    *f;
	char      *text = NULL;
	int       lump = LUMP_ENTITIES;
	int       pass;

	f = FS_Open( filename, "rb", true );
	if( !f )
		return NULL;

	if( FS_Read( f, &hdr, sizeof( hdr )) != sizeof( hdr ) || hdr.version != HLBSP_VERSION )
	{
		FS_Close( f );
		return NULL;
	}

	for( pass = 0; pass < 2; pass++, lump = LUMP_PLANES )
	{
		fs_offset_t ofs = hdr.lumps[lump].fileofs;
		fs_offset_t len = hdr.lumps[lump].filelen;

		if( len <= 0 || len > SG_MAX_ENTLUMP )
			continue;

		if( text )
			Mem_Free( text );
		text = Mem_Malloc( host.mempool, len + 1 );

		FS_Seek( f, ofs, SEEK_SET );
		if( FS_Read( f, text, len ) != len )
		{
			Mem_Free( text );
			text = NULL;
			continue;
		}
		text[len] = 0;

		if( Q_strstr( text, "classname" ))
			break; // the real entity lump

		// wrong lump (Blue Shift variant): second pass tries LUMP_PLANES
		if( pass == 1 )
		{
			Mem_Free( text );
			text = NULL;
		}
	}

	FS_Close( f );
	return text;
}

/*
================
SG_ParseChangelevels

collect trigger_changelevel targets from an entity lump. Keys can come in
any order inside the block, so remember both until the closing brace
================
*/
static void SG_ParseChangelevels( int self, char *text )
{
	char     token[MAX_VA_STRING];
	char     value[MAX_VA_STRING];
	char     target[64];
	qboolean is_changelevel = false;
	char     *pfile = text;

	target[0] = 0;

	while(( pfile = COM_ParseFile( pfile, token, sizeof( token ))) != NULL )
	{
		if( !Q_strcmp( token, "{" ))
		{
			is_changelevel = false;
			target[0] = 0;
			continue;
		}

		if( !Q_strcmp( token, "}" ))
		{
			if( is_changelevel && target[0] )
				SG_AddEdge( self, SG_FindNode( target ));
			continue;
		}

		// key/value pair
		pfile = COM_ParseFile( pfile, value, sizeof( value ));
		if( !pfile )
			break;

		if( !Q_stricmp( token, "classname" ) && !Q_stricmp( value, "trigger_changelevel" ))
			is_changelevel = true;
		else if( !Q_stricmp( token, "map" ))
			Q_strncpy( target, value, sizeof( target ));
	}
}

/*
================
SG_WalkComponent

breadth-first from a root, queueing one world_preload per frame (the wait
keeps the menu responsive, exactly like the generated cfg used to)
================
*/
static int SG_WalkComponent( int root )
{
	static short queue[SG_MAX_MAPS];
	int head = 0, tail = 0;
	int count = 0;

	if( sg_nodes[root].visited )
		return 0;

	sg_nodes[root].visited = true;
	queue[tail++] = root;

	while( head < tail )
	{
		const int n = queue[head++];
		int i;

		Cbuf_AddTextf( "world_preload %s\nwait\n", sg_nodes[n].name );
		count++;

		// nodes are name-sorted and edges discovered in node order, so the
		// walk is deterministic for a given map set
		for( i = 0; i < sg_numedges; i++ )
		{
			int other = -1;

			if( sg_edges[i].a == n )
				other = sg_edges[i].b;
			else if( sg_edges[i].b == n )
				other = sg_edges[i].a;

			if( other >= 0 && !sg_nodes[other].visited )
			{
				sg_nodes[other].visited = true;
				queue[tail++] = other;
			}
		}
	}

	return count;
}

static qboolean SG_NodeConnected( int n )
{
	int i;

	for( i = 0; i < sg_numedges; i++ )
	{
		if( sg_edges[i].a == n || sg_edges[i].b == n )
			return true;
	}
	return false;
}

/*
================
Host_QueueStreamPreload

scan this game's maps, build the changelevel graph and queue the preloads.
Called at startup instead of exec'ing a generated streampreload.cfg
================
*/
void Host_QueueStreamPreload( void )
{
	double   t = Sys_DoubleTime();
	search_t *search;
	int      i, queued = 0, chains = 0;

	search = FS_Search( "maps/*.bsp", true, true );
	if( !search )
		return; // no loose or packed maps in this game dir at all

	sg_nodes = Mem_Calloc( host.mempool, sizeof( sg_node_t ) * SG_MAX_MAPS );
	sg_edges = Mem_Calloc( host.mempool, sizeof( sg_edge_t ) * SG_MAX_EDGES );
	sg_numnodes = sg_numedges = 0;

	// unique base names; the same map may appear as loose file and in a pak
	for( i = 0; i < search->numfilenames && sg_numnodes < SG_MAX_MAPS; i++ )
	{
		char base[64];

		COM_FileBase( search->filenames[i], base, sizeof( base ));
		Q_strnlwr( base, base, sizeof( base ));

		if( SG_FindNode( base ) < 0 )
			Q_strncpy( sg_nodes[sg_numnodes++].name, base, sizeof( sg_nodes[0].name ));
	}

	// deterministic order regardless of filesystem enumeration
	qsort( sg_nodes, sg_numnodes, sizeof( sg_node_t ), SG_CompareNodes );

	for( i = 0; i < sg_numnodes; i++ )
	{
		char path[MAX_QPATH];
		char *ents;

		Q_snprintf( path, sizeof( path ), "maps/%s.bsp", sg_nodes[i].name );
		ents = SG_LoadEntityLump( path );
		if( !ents )
			continue;

		SG_ParseChangelevels( i, ents );
		Mem_Free( ents );
	}

	// campaign first: start map, then the hazard course, then leftovers
	if( GI->startmap[0] )
	{
		i = SG_FindNode( GI->startmap );
		if( i >= 0 && SG_NodeConnected( i ) && SG_WalkComponent( i ))
			chains++;
	}

	if( GI->trainmap[0] )
	{
		i = SG_FindNode( GI->trainmap );
		if( i >= 0 && SG_NodeConnected( i ) && SG_WalkComponent( i ))
			chains++;
	}

	for( i = 0; i < sg_numnodes; i++ )
	{
		if( !sg_nodes[i].visited && SG_NodeConnected( i ))
		{
			SG_WalkComponent( i );
			chains++;
		}
	}

	// count what we queued (visited == queued)
	for( i = 0; i < sg_numnodes; i++ )
	{
		if( sg_nodes[i].visited )
			queued++;
	}

	if( queued )
	{
		Con_Printf( "[streaming] preloading %i of %i maps in %i chains (%i links, graph scan %.1f ms)\n",
			queued, sg_numnodes, chains, sg_numedges, ( Sys_DoubleTime() - t ) * 1000.0 );
	}

	Mem_Free( sg_nodes );
	Mem_Free( sg_edges );
	Mem_Free( search );
	sg_nodes = NULL;
	sg_edges = NULL;
}

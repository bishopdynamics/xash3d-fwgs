/*
host_bgcompose.c - run-once compose of every game's menu background
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
The Continuum menu draws each game's 800x600 menu background, normalized to a
single image at gfx/shell/continuum/games/<gamedir>.png. Steam-era games ship
it as a 4x3 grid of TGA tiles (resource/background/800_<row>_<col>_loading.tga);
WON-era games ship a single gfx/shell/splash.bmp.

This used to be an offline step (tools/compose_backgrounds.py); now the engine
bakes it on first launch, behind the same run-once stage as world AO. We do it
for ALL installed games (not just the current one), so the game-picker is fully
populated, and write the results into the always-mounted continuum/ content dir
so they resolve no matter which game is loaded.

Each game's files are read directly from its folder (loose files only) without
mounting it - the engine only mounts the current game, and every game shares the
same resource/background paths, so mounting can't address a specific one. The FS
write path is locked to the current game, so we save through it and then move the
PNG into continuum/. No blur: the menu aspect-fits the sharp art itself.
*/
#include "common.h"
#include "com_image.h"		// rgbdata_t, IMAGE_FORCE_RGBA, PF_RGBA_32
#include "fscallback.h"		// FI (all games), g_fsapi, GI
#include <stdio.h>		// rename, remove

#if XASH_WIN32
#include <direct.h>
#define BG_Mkdir( p )	_mkdir( p )
#else
#include <sys/stat.h>
#define BG_Mkdir( p )	mkdir(( p ), 0755 )
#endif

#define BG_W	800
#define BG_H	600
#define BG_TILE	256

// load <relpath> forced to RGBA, or NULL, reading ONLY that game's own content (never the
// always-mounted valve base - else a game with no tiles of its own would silently get
// valve's via fallback). gamedirAbs NULL => the current game: FS_LoadFile gamedir-only, so
// its paks work (WON-era splash lives in pak0) but valve doesn't. Otherwise a loose
// direct-disk read from that specific un-mounted game's folder.
static rgbdata_t *BG_LoadTileRGBA( const char *gamedirAbs, const char *relpath )
{
	char        membuf[80];
	fs_offset_t len;
	byte       *raw;
	rgbdata_t  *pic;

	if( gamedirAbs )
	{
		char path[MAX_SYSPATH];

		Q_snprintf( path, sizeof( path ), "%s/%s", gamedirAbs, relpath );
		raw = FS_LoadDirectFile( path, &len );
	}
	else raw = FS_LoadFile( relpath, &len, true );	// current game only (own dir + paks)

	if( !raw )
		return NULL;

	// '#' prefix forces decode-from-buffer; without it FS_LoadImage would load the name
	// from the search path (i.e. valve's copy) and ignore our bytes
	Q_snprintf( membuf, sizeof( membuf ), "#%s", relpath );
	Image_SetForceFlags( IMAGE_FORCE_RGBA );	// indexed/24-bit -> RGBA
	pic = FS_LoadImage( membuf, raw, len );
	Image_ClearForceFlags();
	Mem_Free( raw );

	return pic;
}

// Steam-era 4x3 tile grid -> one 800x600 RGBA image; NULL if any tile is missing
static rgbdata_t *BG_ComposeTiles( const char *gamedirAbs )
{
	rgbdata_t *tiles[3][4];
	rgbdata_t *canvas;
	int        row, col, r, c, y;

	memset( tiles, 0, sizeof( tiles ));

	for( row = 0; row < 3; row++ )
	{
		for( col = 0; col < 4; col++ )
		{
			char rel[64];

			Q_snprintf( rel, sizeof( rel ), "resource/background/800_%i_%c_loading.tga", row + 1, "abcd"[col] );
			tiles[row][col] = BG_LoadTileRGBA( gamedirAbs, rel );

			if( !tiles[row][col] )
			{
				for( r = 0; r <= row; r++ )
					for( c = 0; c < 4; c++ )
						if( tiles[r][c] ) FS_FreeImage( tiles[r][c] );
				return NULL;
			}
		}
	}

	canvas = Mem_Calloc( host.mempool, sizeof( *canvas ));
	canvas->width  = BG_W;
	canvas->height = BG_H;
	canvas->type   = PF_RGBA_32;
	canvas->size   = (size_t)BG_W * BG_H * 4;
	canvas->buffer = Mem_Calloc( host.mempool, canvas->size );

	for( row = 0; row < 3; row++ )
	{
		for( col = 0; col < 4; col++ )
		{
			rgbdata_t *t  = tiles[row][col];
			const int  ox = col * BG_TILE;
			const int  oy = row * BG_TILE;
			int        cw = t->width;
			int        ch = t->height;

			if( ox + cw > BG_W ) cw = BG_W - ox;	// clip edge tiles to the canvas
			if( oy + ch > BG_H ) ch = BG_H - oy;

			for( y = 0; y < ch; y++ )
			{
				memcpy( canvas->buffer + (( (size_t)( oy + y ) * BG_W + ox ) * 4 ),
					t->buffer + ( (size_t)y * t->width * 4 ),
					(size_t)cw * 4 );
			}

			FS_FreeImage( t );
		}
	}

	return canvas;
}

// recursive mkdir of an absolute path (ignores already-exists)
static void BG_MakePath( const char *dir )
{
	char tmp[MAX_SYSPATH];
	int  i;

	Q_strncpy( tmp, dir, sizeof( tmp ));
	for( i = 1; tmp[i]; i++ )
	{
		if( tmp[i] == '/' )
		{
			tmp[i] = '\0';
			BG_Mkdir( tmp );
			tmp[i] = '/';
		}
	}
	BG_Mkdir( tmp );
}

// FS writes are locked to the current game's writepath, so save there then move the
// PNG into continuum/ (both under rootdir, so a plain rename does it)
static qboolean BG_SaveToContinuum( const char *rootdir, const char *game, rgbdata_t *img )
{
	char rel[128], src[MAX_SYSPATH], dstdir[MAX_SYSPATH], dst[MAX_SYSPATH];

	Q_snprintf( rel, sizeof( rel ), "gfx/shell/continuum/games/%s.png", game );
	if( !FS_SaveImage( rel, img ))
		return false;

	Q_snprintf( src,    sizeof( src ),    "%s/%s/%s", rootdir, GI->gamefolder, rel );
	Q_snprintf( dstdir, sizeof( dstdir ), "%s/continuum/gfx/shell/continuum/games", rootdir );
	Q_snprintf( dst,    sizeof( dst ),    "%s/%s.png", dstdir, game );

	BG_MakePath( dstdir );
	remove( dst );
	return rename( src, dst ) == 0;
}

/*
=================
Host_AutoComposeBackground

at startup, compose a normalized menu background for every installed game that
doesn't have one yet, into continuum/. Client only; one-time (the saved PNG is
the gate). Runs alongside Host_AutoBakeAO.
=================
*/
void Host_AutoComposeBackground( void )
{
	char rootdir[MAX_SYSPATH];
	int  i, made = 0;

	if( Host_IsDedicated( ))
		return;	// no menu to draw a background for
	if( !g_fsapi.GetRootDirectory( rootdir, sizeof( rootdir )))
		return;

	for( i = 0; i < FI->numgames; i++ )
	{
		const char *game = FI->games[i]->gamefolder;
		char        outpath[160], gamedir[MAX_SYSPATH];
		rgbdata_t  *img;
		qboolean    fromTiles;

		Q_snprintf( outpath, sizeof( outpath ), "gfx/shell/continuum/games/%s.png", game );
		if( FS_FileExists( outpath, false ))
			continue;	// already have it (continuum, valve, or that game)

		// current game reads through the mounted search path (paks work); other games
		// are read loose from their folder directly (they aren't mounted)
		Q_snprintf( gamedir, sizeof( gamedir ), "%s/%s", rootdir, game );
		const char *gdir = !Q_stricmp( game, GI->gamefolder ) ? NULL : gamedir;

		img = BG_ComposeTiles( gdir );
		fromTiles = ( img != NULL );
		if( !img )
			img = BG_LoadTileRGBA( gdir, "gfx/shell/splash.bmp" );	// WON-era
		if( !img )
			continue;	// no menu background ships with this game

		if( BG_SaveToContinuum( rootdir, game, img ))
			made++;

		if( fromTiles )
		{
			Mem_Free( img->buffer );
			Mem_Free( img );
		}
		else FS_FreeImage( img );
	}

	if( made )
		Con_Printf( "[bg] composed %i game menu background%s into continuum/ (one-time)\n", made, made == 1 ? "" : "s" );
}

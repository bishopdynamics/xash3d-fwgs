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
The Continuum menu draws each game's menu background, normalized to a single image
at gfx/shell/continuum/games/<gamedir>.png at its native aspect (the menu fits it:
4:3 -> fit-width/top-pinned, wider -> fit-to-height/centered).

Games ship the background as a grid of TGA tiles, resource/background/<set>_<row>_
<col>_loading.tga, where <set> names an aspect/resolution (4:3 "800", widescreen
"21_9", ...). We try the 4:3 "800" set first; modern Steam ships that one BLANK
(opaque black) and puts the real art in a widescreen set, so if a composed set is
near-black we move on to the next set until one has content. Edge tiles are partial,
so the native size is just the summed tile dimensions. WON-era games instead ship a
single gfx/shell/splash.bmp.

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

#define BG_MAXCOL	26	// tile columns a..z
#define BG_MAXROW	16	// tile rows 1..16

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

// essentially blank? Modern Steam ships the legacy 4:3 "800_" set as opaque black
// (the real art is in a widescreen set), so a near-black compose means "try another".
static qboolean BG_IsBlank( const rgbdata_t *img )
{
	uint64_t sum = 0;
	size_t   px = (size_t)img->width * img->height, i, n = 0;

	for( i = 0; i < px; i += 97, n++ )	// sample is plenty to tell black from art
	{
		const byte *p = img->buffer + i * 4;
		sum += p[0] + p[1] + p[2];
	}
	return n ? (( sum / ( n * 3 )) < 6 ) : true;	// mean luma < 6/255
}

// Compose one named tile set, resource/background/<prefix>_<row>_<col>_loading.tga
// (rows 1.., cols a..), into one RGBA image at its NATIVE size. Edge tiles are
// partial (e.g. 4:3 last col is 32px), so summing real tile dimensions yields the
// exact image - no per-aspect constants. NULL if the set is absent or incomplete.
static rgbdata_t *BG_ComposeSet( const char *gamedirAbs, const char *prefix )
{
	rgbdata_t *tiles[BG_MAXROW][BG_MAXCOL];
	int        rows = 0, cols = 0, r, c, y;
	int        colX[BG_MAXCOL + 1], rowY[BG_MAXROW + 1];
	rgbdata_t *canvas;
	char       rel[80];

	memset( tiles, 0, sizeof( tiles ));

	// discover the grid: probe column 'a' downward for rows, row 1 across for cols
	for( r = 0; r < BG_MAXROW; r++ )
	{
		Q_snprintf( rel, sizeof( rel ), "resource/background/%s_%i_a_loading.tga", prefix, r + 1 );
		if( !( tiles[r][0] = BG_LoadTileRGBA( gamedirAbs, rel ))) break;
		rows = r + 1;
	}
	if( !rows ) return NULL;
	cols = 1;
	for( c = 1; c < BG_MAXCOL; c++ )
	{
		Q_snprintf( rel, sizeof( rel ), "resource/background/%s_1_%c_loading.tga", prefix, 'a' + c );
		if( !( tiles[0][c] = BG_LoadTileRGBA( gamedirAbs, rel ))) break;
		cols = c + 1;
	}

	// fill the rest of the grid; any missing interior tile = incomplete set
	for( r = 0; r < rows; r++ )
	{
		for( c = 0; c < cols; c++ )
		{
			if( tiles[r][c] ) continue;
			Q_snprintf( rel, sizeof( rel ), "resource/background/%s_%i_%c_loading.tga", prefix, r + 1, 'a' + c );
			if( !( tiles[r][c] = BG_LoadTileRGBA( gamedirAbs, rel )))
			{
				int rr, cc;
				for( rr = 0; rr < rows; rr++ )
					for( cc = 0; cc < cols; cc++ )
						if( tiles[rr][cc] ) FS_FreeImage( tiles[rr][cc] );
				return NULL;
			}
		}
	}

	// native canvas = summed real column widths x row heights
	colX[0] = 0;
	for( c = 0; c < cols; c++ ) colX[c + 1] = colX[c] + tiles[0][c]->width;
	rowY[0] = 0;
	for( r = 0; r < rows; r++ ) rowY[r + 1] = rowY[r] + tiles[r][0]->height;

	canvas = Mem_Calloc( host.mempool, sizeof( *canvas ));
	canvas->width  = colX[cols];
	canvas->height = rowY[rows];
	canvas->type   = PF_RGBA_32;
	canvas->size   = (size_t)canvas->width * canvas->height * 4;
	canvas->buffer = Mem_Calloc( host.mempool, canvas->size );

	for( r = 0; r < rows; r++ )
	{
		for( c = 0; c < cols; c++ )
		{
			rgbdata_t *t  = tiles[r][c];
			const int  ox = colX[c], oy = rowY[r];
			int        cw = t->width, ch = t->height;

			if( ox + cw > canvas->width )  cw = canvas->width  - ox;
			if( oy + ch > canvas->height ) ch = canvas->height - oy;

			for( y = 0; y < ch; y++ )
			{
				memcpy( canvas->buffer + (( (size_t)( oy + y ) * canvas->width + ox ) * 4 ),
					t->buffer + ( (size_t)y * t->width * 4 ),
					(size_t)cw * 4 );
			}
			FS_FreeImage( t );
		}
	}
	return canvas;
}

// Try the 4:3 "800_" set first; if it's blank (Steam blanks it) try the next set
// until one has real art. Native aspect is preserved so the menu fits it (4:3 ->
// fit-width, top-pinned; wider -> fit-to-height, centered). Caller frees via Mem_Free.
static rgbdata_t *BG_ComposeBackground( const char *gamedirAbs )
{
	static const char *const prefixes[] =
	{
		"800", "1024", "1280", "1600",			// 4:3 sets (retail/WON have content here)
		"16_9", "16_10", "21_9", "5_4", "2_1", "32_9"	// widescreen sets (modern Steam)
	};
	size_t i;

	for( i = 0; i < sizeof( prefixes ) / sizeof( prefixes[0] ); i++ )
	{
		rgbdata_t *img = BG_ComposeSet( gamedirAbs, prefixes[i] );

		if( !img )
			continue;
		if( !BG_IsBlank( img ))
			return img;

		Mem_Free( img->buffer );
		Mem_Free( img );
	}
	return NULL;
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

		// a hand-authored override shipped in continuum/games_override/ replaces the
		// composed art, so don't compose (the menu loads the override directly)
		Q_snprintf( outpath, sizeof( outpath ), "gfx/shell/continuum/games_override/%s.png", game );
		if( FS_FileExists( outpath, false ))
			continue;

		Q_snprintf( outpath, sizeof( outpath ), "gfx/shell/continuum/games/%s.png", game );
		if( FS_FileExists( outpath, false ))
			continue;	// already have it (continuum, valve, or that game)

		// current game reads through the mounted search path (paks work); other games
		// are read loose from their folder directly (they aren't mounted)
		Q_snprintf( gamedir, sizeof( gamedir ), "%s/%s", rootdir, game );
		const char *gdir = !Q_stricmp( game, GI->gamefolder ) ? NULL : gamedir;

		img = BG_ComposeBackground( gdir );
		fromTiles = ( img != NULL );
		if( !img )
			img = BG_LoadTileRGBA( gdir, "gfx/shell/splash.bmp" );	// WON-era single splash
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

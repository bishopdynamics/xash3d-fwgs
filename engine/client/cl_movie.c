/*
cl_movie.c - dump rendered frames to a file/FIFO for external video capture

`startmovie <path>` opens <path> (typically a FIFO an ffmpeg reader is draining)
and, every rendered frame, writes the raw RGBA backbuffer to it (bottom-up, as
glReadPixels returns it — the reader flips with -vf vflip). `endmovie` stops.
The companion tool is tools/capture-demo-video.sh. GL renderer only (the
software renderer's R_GetFrameBuffer returns NULL).
*/
#include "common.h"
#include "client.h"
#include <stdio.h>
#include <signal.h>

static FILE     *cl_moviefile;
static qboolean cl_movie_active;

void CL_StopMovie( void )
{
	if( !cl_movie_active )
		return;

	cl_movie_active = false;
	if( cl_moviefile )
	{
		fclose( cl_moviefile );
		cl_moviefile = NULL;
	}
	Con_Printf( "movie capture stopped\n" );
}

static void CL_StartMovie_f( void )
{
	const char *path;

	if( Cmd_Argc() != 2 )
	{
		Con_Printf( S_USAGE "startmovie <path>\n" );
		return;
	}

	if( cl_movie_active )
	{
		Con_Printf( "startmovie: already recording; run endmovie first\n" );
		return;
	}

	path = Cmd_Argv( 1 );
	cl_moviefile = fopen( path, "wb" );
	if( !cl_moviefile )
	{
		Con_Printf( S_ERROR "startmovie: can't open %s\n", path );
		return;
	}

#ifdef SIGPIPE
	// never let a vanished reader (ffmpeg) kill the engine on write
	signal( SIGPIPE, SIG_IGN );
#endif
	cl_movie_active = true;
	Con_Printf( "startmovie: capturing %dx%d raw RGBA frames to %s\n",
		refState.width, refState.height, path );
}

void CL_MovieFrame( void )
{
	byte   *buf;
	int    w = 0, h = 0;
	size_t count;

	if( !cl_movie_active || !cl_moviefile )
		return;

	buf = ref.dllFuncs.R_GetFrameBuffer( &w, &h );
	if( !buf || w <= 0 || h <= 0 )
	{
		Con_Printf( S_ERROR "movie: renderer returned no framebuffer; stopping\n" );
		CL_StopMovie();
		return;
	}

	count = (size_t)w * h * 4;
	if( fwrite( buf, 1, count, cl_moviefile ) != count )
	{
		Con_Printf( S_ERROR "movie: frame write failed (reader gone?); stopping\n" );
		CL_StopMovie();
	}
}

void CL_InitMovie( void )
{
	Cmd_AddCommand( "startmovie", CL_StartMovie_f, "dump raw RGBA frames to a file/FIFO for video capture" );
	Cmd_AddCommand( "endmovie", CL_StopMovie, "stop movie frame dumping" );
}

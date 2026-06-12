/*
in_osk.c - on-screen keyboard input
Copyright (C) 2016-2026 Alibek Omarov

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
#include "input.h"
#include "client.h"

CVAR_DEFINE_AUTO( osk_enable, "0", FCVAR_ARCHIVE | FCVAR_FILTERABLE, "always pop up the built-in on-screen keyboard for text fields (it is offered on (A) whenever a controller is present regardless)" );

/* On-screen keyboard, Continuum-styled and gamepad-first.
 *
 * 4 lines with 13 buttons each; d-pad moves the highlight, (A) types it.
 * Direct buttons (also shown in the legend bar):
 *   (X) backspace   (Y) space      (LT) shift toggle
 *   (LB)/(RB) move the text cursor (B)/(RT) done    (START) cancel
 *
 * Our layout:
 *  0  1  2  3  4  5  6  7  8  9  10 11 12
 * +--+--+--+--+--+--+--+--+--+--+--+--+--+
 * |` |1 |2 |3 |4 |5 |6 |7 |8 |9 |0 |- |= | 0
 * +--+--+--+--+--+--+--+--+--+--+--+--+--+
 * |q |w |e |r |t |y |u |i |o |p |[ |] |\ | 1
 * +--+--+--+--+--+--+--+--+--+--+--+--+--+
 * |TB|a |s |d |f |g |h |j |k |l |; |' |BS| 2
 * +--+--+--+--+--+--+--+--+--+--+--+--+--+
 * |SH|z |x |c |v |b |n |m |, |. |/ |SP|EN| 3
 * +--+--+--+--+--+--+--+--+--+--+--+--+--+
 *
 * With osk_enable 0 (default) the keyboard still arms whenever a game
 * controller is present, but starts hidden: the first (A) press summons it,
 * so keyboard users never see it. osk_enable 1 pops it up immediately.
 */

#define MAX_OSK_ROWS  13
#define MAX_OSK_LINES 4

enum
{
	OSK_DEFAULT = 0,
	OSK_UPPER, // on caps, shift
	OSK_LAST
};

enum
{
	OSK_TAB = 16,
	OSK_SHIFT,
	OSK_BACKSPACE,
	OSK_ENTER,
	OSK_SPECKEY_LAST
};

static const char *osk_keylayout[][4] =
{
	{
		"`1234567890-=",             // 13
		"qwertyuiop[]\\",            // 13
		"\x10" "asdfghjkl;'" "\x12", // 11 + tab on a left, backspace on a right
		"\x11" "zxcvbnm,./ " "\x13"  // 10 + shift on a left, space, enter on a right
	},
	{
		"~!@#$%^&*()_+",
		"QWERTYUIOP{}|",
		"\x10" "ASDFGHJKL:\"" "\x12",
		"\x11" "ZXCVBNM<>? "  "\x13"
	}
};

static struct osk_s
{
	qboolean enable;
	int      curlayout;
	qboolean shift;
	qboolean sending;
	struct
	{
		signed char x;
		signed char y;
		char val;
	} curbutton;
} osk;

/*
=============
OSK_PadPresent

a connected controller arms the keyboard even with osk_enable 0
=============
*/
static qboolean OSK_PadPresent( void )
{
	return Cvar_VariableValue( "joy_enable" ) != 0.0f
		&& Cvar_VariableValue( "joy_controller_type" ) != 0.0f;
}

static qboolean OSK_Active( void )
{
	if( !osk.enable )
		return false;

	return osk_enable.value != 0.0f || OSK_PadPresent();
}

/*
=============
OSK_SendKey

synthesize a key for the underlying text field, bypassing ourselves
=============
*/
static void OSK_SendKey( int key, int down )
{
	osk.sending = true;
	Key_Event( key, down );
}

static void OSK_ToggleShift( void )
{
	if( osk.curlayout & 1 )
		osk.curlayout--;
	else
		osk.curlayout++;

	osk.curbutton.val = osk_keylayout[osk.curlayout][osk.curbutton.y][osk.curbutton.x];
}

qboolean OSK_KeyEvent( int key, int down )
{
	if( !OSK_Active( ))
		return false;

	if( osk.sending )
	{
		osk.sending = false;
		return false;
	}

	if( osk.curbutton.val == 0 )
	{
		// hidden: only the controller's (A) summons the keyboard, everything
		// else (physical keyboard included) goes through untouched
		if( key == K_A_BUTTON )
		{
			osk.curbutton.val = osk_keylayout[osk.curlayout][osk.curbutton.y][osk.curbutton.x];
			return true;
		}
		return false;
	}

	switch( key )
	{
	case K_X_BUTTON: // backspace
		OSK_SendKey( K_BACKSPACE, down );
		return true;
	case K_Y_BUTTON: // space
		if( down )
			CL_CharEvent( ' ' );
		return true;
	case K_L2_BUTTON: // shift toggle
		if( down )
		{
			osk.shift = false; // sticky, not one-shot
			OSK_ToggleShift();
		}
		return true;
	case K_B_BUTTON:
	case K_R2_BUTTON: // done
		OSK_SendKey( K_ENTER, down );
		return true;
	case K_START_BUTTON: // cancel
		OSK_SendKey( K_ESCAPE, down );
		return true;
	case K_L1_BUTTON: // text cursor
		OSK_SendKey( K_LEFTARROW, down );
		return true;
	case K_R1_BUTTON:
		OSK_SendKey( K_RIGHTARROW, down );
		return true;
	case K_A_BUTTON:
	case K_ENTER:
		switch( osk.curbutton.val )
		{
		case OSK_ENTER:
			OSK_SendKey( K_ENTER, down );
			break;
		case OSK_SHIFT:
			if( !down )
				break;

			OSK_ToggleShift();
			osk.shift = true; // one-shot: reverts after the next character
			break;
		case OSK_BACKSPACE:
			OSK_SendKey( K_BACKSPACE, down );
			break;
		case OSK_TAB:
			OSK_SendKey( K_TAB, down );
			break;
		default:
		{
			int ch;

			if( !down )
			{
				if( osk.shift && osk.curlayout & 1 )
					osk.curlayout--;

				osk.shift = false;
				osk.curbutton.val = osk_keylayout[osk.curlayout][osk.curbutton.y][osk.curbutton.x];
				break;
			}

			ch = (byte)osk.curbutton.val;

			// do not pass UTF-8 sequence into the engine, convert it here
			if( !cls.accept_utf8 )
				ch = Con_UtfProcessCharForce( ch );

			if( !ch )
				break;

			CL_CharEvent( ch );
			break;
		}
		}
		break;
	case K_UPARROW:
	case K_DPAD_UP: // the d-pad arrives raw, not as arrows
		if( down && --osk.curbutton.y < 0 )
			osk.curbutton.y = MAX_OSK_LINES - 1;
		break;
	case K_DOWNARROW:
	case K_DPAD_DOWN:
		if( down && ++osk.curbutton.y >= MAX_OSK_LINES )
			osk.curbutton.y = 0;
		break;
	case K_LEFTARROW:
	case K_DPAD_LEFT:
		if( down && --osk.curbutton.x < 0 )
			osk.curbutton.x = MAX_OSK_ROWS - 1;
		break;
	case K_RIGHTARROW:
	case K_DPAD_RIGHT:
		if( down && ++osk.curbutton.x >= MAX_OSK_ROWS )
			osk.curbutton.x = 0;
		break;
	default:
		return false;
	}

	osk.curbutton.val = osk_keylayout[osk.curlayout][osk.curbutton.y][osk.curbutton.x];
	return true;
}

/*
=============
OSK_EnableTextInput

Enables built-in IME
=============
*/
void OSK_EnableTextInput( qboolean enable, qboolean force )
{
	qboolean old = osk.enable;

	osk.enable = enable;

	if( osk.enable && ( !old || force ))
	{
		osk.curlayout = 0;

		// pad-auto mode starts hidden; (A) summons it. Explicit osk_enable
		// pops it up right away like it always did
		if( osk_enable.value )
			osk.curbutton.val = osk_keylayout[osk.curlayout][osk.curbutton.y][osk.curbutton.x];
		else
			osk.curbutton.val = 0;
	}
}

/*
====================
drawing: Continuum theme (palette matches mainui menus/continuum)
====================
*/
#define X_START 0.1347475f
#define Y_START 0.567f
#define X_STEP  0.05625f
#define Y_STEP  0.0825f

#define OSK_PAD_X 0.012f
#define OSK_PAD_Y 0.018f
#define OSK_LEGEND_H 0.052f
#define OSK_PREVIEW_H 0.052f

static const rgba_t osk_clr_ink    = { 232, 230, 225, 255 };
static const rgba_t osk_clr_dim    = { 154, 151, 143, 255 };
static const rgba_t osk_clr_faint  = { 110, 108, 102, 255 };
static const rgba_t osk_clr_accent = { 255, 163,  26, 255 };

// legend glyphs, drawn from the same Xelu set the menu uses
enum
{
	OSK_GLYPH_A = 0,
	OSK_GLYPH_B,
	OSK_GLYPH_X,
	OSK_GLYPH_Y,
	OSK_GLYPH_LB,
	OSK_GLYPH_RB,
	OSK_GLYPH_START,
	OSK_GLYPH_COUNT
};
static const char *osk_glyph_names[OSK_GLYPH_COUNT] = { "a", "b", "x", "y", "lb", "rb", "start" };
static int  osk_glyph_tex[OSK_GLYPH_COUNT];
static char osk_glyph_style[16];

static const char *OSK_GlyphStyle( void )
{
	const char *style = Cvar_VariableString( "ui_glyph_style" );

	if( COM_StringEmptyOrNULL( style ) || !Q_strcmp( style, "auto" ))
	{
		switch( (int)Cvar_VariableValue( "joy_controller_type" ))
		{
		case 3:  // PS3
		case 4:  // PS4
		case 7:  // PS5
			return "ps";
		case 5:  // Switch Pro
		case 11: // Joy-Con left
		case 12: // Joy-Con right
		case 13: // Joy-Con pair
			return "switch";
		case 0:  // no controller seen
			return "kb";
		default:
			return "xbox";
		}
	}

	return style;
}

static void OSK_LoadGlyphs( void )
{
	const char *style = OSK_GlyphStyle();
	int i;

	if( !Q_strcmp( osk_glyph_style, style ))
		return;

	for( i = 0; i < OSK_GLYPH_COUNT; i++ )
	{
		char path[MAX_VA_STRING];

		Q_snprintf( path, sizeof( path ), "gfx/shell/continuum/glyphs/%s/%s.png", style, osk_glyph_names[i] );

		if( FS_FileExists( path, false ))
			osk_glyph_tex[i] = ref.dllFuncs.GL_LoadTexture( path, NULL, 0, TF_IMAGE );
		else
			osk_glyph_tex[i] = 0;
	}

	Q_strncpy( osk_glyph_style, style, sizeof( osk_glyph_style ));
}

// returns the advance width; texnum 0 falls back to a "[A]"-style text tag
static int OSK_DrawGlyph( int glyph, int x, int y, int h )
{
	int texnum = osk_glyph_tex[glyph];
	int w;

	if( !texnum )
	{
		char tag[8];
		int tw, th;

		Q_snprintf( tag, sizeof( tag ), "[%s]", osk_glyph_names[glyph] );
		Con_DrawStringLen( tag, &tw, &th );
		Con_DrawString( x, y + ( h - th ) / 2, tag, osk_clr_dim );
		return tw;
	}

	w = h * REF_GET_PARM( PARM_TEX_SRC_WIDTH, texnum ) / Q_max( 1, REF_GET_PARM( PARM_TEX_SRC_HEIGHT, texnum ));

	ref.dllFuncs.GL_SetRenderMode( kRenderTransTexture );
	ref.dllFuncs.Color4ub( 255, 255, 255, 255 );
	ref.dllFuncs.R_DrawStretchPic( x, y, w, h, 0, 0, 1, 1, texnum );

	return w;
}

static void OSK_DrawCenteredString( const char *str, int cx, int cy, const rgba_t color )
{
	int w, h;

	Con_DrawStringLen( str, &w, &h );
	Con_DrawString( cx - w / 2, cy - h / 2, str, color );
}

/*
============
OSK_DrawSymbolButton
============
*/
static void OSK_DrawSymbolButton( int symb, float x, float y, float width, float height )
{
	int x1 = x * refState.width,
	    y1 = y * refState.height,
	    w = width * refState.width,
	    h = height * refState.height;
	int cx = x1 + w / 2,
	    cy = y1 + h / 2;
	const qboolean selected = ( symb == osk.curbutton.val );

	if( selected )
	{
		// accent-soft fill + solid accent underline, like a focused row
		ref.dllFuncs.FillRGBA( kRenderTransTexture, x1 + 1, y1 + 1, w - 2, h - 2, 255, 163, 26, 56 );
		ref.dllFuncs.FillRGBA( kRenderTransTexture, x1 + 1, y1 + h - 3, w - 2, 2, 255, 163, 26, 255 );
	}

	if( !symb )
		return;

	if( symb >= OSK_TAB && symb < OSK_SPECKEY_LAST )
	{
		const char *label = "";

		switch( symb )
		{
		case OSK_TAB:       label = "TAB"; break;
		case OSK_SHIFT:     label = "SHIFT"; break;
		case OSK_BACKSPACE: label = "DEL"; break;
		case OSK_ENTER:     label = "ENTER"; break;
		}

		// shifted layout keeps SHIFT lit as a caps indicator
		OSK_DrawCenteredString( label, cx, cy,
			( symb == OSK_SHIFT && ( osk.curlayout & 1 )) ? osk_clr_accent :
			selected ? osk_clr_ink : osk_clr_dim );
		return;
	}

	if( symb == ' ' )
	{
		// the space cell gets a small bar so it reads as a key
		ref.dllFuncs.FillRGBA( kRenderTransTexture, cx - w / 4, cy, w / 2, 2,
			selected ? 232 : 154, selected ? 230 : 151, selected ? 225 : 143, 255 );
		return;
	}

	{
		char str[2] = { (char)symb, 0 };
		OSK_DrawCenteredString( str, cx, cy, selected ? osk_clr_ink : osk_clr_dim );
	}
}

/*
=============
OSK_Draw

Draw on screen keyboard, if enabled
=============
*/
void OSK_Draw( void )
{
	const char **curlayout = osk_keylayout[osk.curlayout];
	const char *preview = NULL;
	float x, y;
	int i, j;
	int px, py, pw, ph;

	if( !OSK_Active( ) || !osk.curbutton.val )
		return;

	OSK_LoadGlyphs();

	// panel behind grid + legend
	px = ( X_START - OSK_PAD_X ) * refState.width;
	py = ( Y_START - OSK_PAD_Y ) * refState.height;
	pw = ( X_STEP * MAX_OSK_ROWS + OSK_PAD_X * 2 ) * refState.width;
	ph = ( Y_STEP * MAX_OSK_LINES + OSK_PAD_Y * 2 + OSK_LEGEND_H ) * refState.height;

	// the text being edited, in a strip above the panel (menus report it via
	// osk_preview; in-game chat we can read directly)
	if( cls.key_dest == key_menu )
		preview = Cvar_VariableString( "osk_preview" );
	else if( cls.key_dest == key_message )
		preview = Con_GetChatText();

	if( preview && preview[0] )
	{
		int sh = OSK_PREVIEW_H * refState.height;
		int sy = py - sh - 0.008f * refState.height;
		int tw, th;

		ref.dllFuncs.FillRGBA( kRenderTransTexture, px, sy, pw, sh, 14, 16, 20, 235 );
		ref.dllFuncs.FillRGBA( kRenderTransTexture, px, sy, pw, 1, 255, 255, 255, 40 );

		Con_DrawStringLen( preview, &tw, &th );
		Con_DrawString( px + 0.012f * refState.width, sy + ( sh - th ) / 2, preview, osk_clr_ink );
	}

	// card: near-black fill, accent top edge, hairline border
	ref.dllFuncs.FillRGBA( kRenderTransTexture, px, py, pw, ph, 14, 16, 20, 235 );
	ref.dllFuncs.FillRGBA( kRenderTransTexture, px, py, pw, 2, 255, 163, 26, 255 );
	ref.dllFuncs.FillRGBA( kRenderTransTexture, px, py + ph - 1, pw, 1, 255, 255, 255, 40 );
	ref.dllFuncs.FillRGBA( kRenderTransTexture, px, py, 1, ph, 255, 255, 255, 40 );
	ref.dllFuncs.FillRGBA( kRenderTransTexture, px + pw - 1, py, 1, ph, 255, 255, 255, 40 );

	for( y = Y_START, j = 0; j < MAX_OSK_LINES; j++, y += Y_STEP )
		for( x = X_START, i = 0; i < MAX_OSK_ROWS; i++, x += X_STEP )
			OSK_DrawSymbolButton( curlayout[j][i], x, y, X_STEP, Y_STEP );

	// legend bar
	{
		static const struct
		{
			int glyph;
			int glyph2; // -1 for none
			const char *text;
		} legend[] =
		{
			{ OSK_GLYPH_A, -1, "Type" },
			{ OSK_GLYPH_Y, -1, "Space" },
			{ OSK_GLYPH_X, -1, "Backspace" },
			{ OSK_GLYPH_LB, OSK_GLYPH_RB, "Cursor" },
			{ OSK_GLYPH_B, -1, "Done" },
			{ OSK_GLYPH_START, -1, "Cancel" },
		};
		int gh = OSK_LEGEND_H * 0.62f * refState.height;
		int ly = py + ph - ( OSK_LEGEND_H * refState.height + gh ) / 2;
		int lx = px + 0.012f * refState.width;
		size_t k;

		for( k = 0; k < ARRAYSIZE( legend ); k++ )
		{
			int tw, th;

			lx += OSK_DrawGlyph( legend[k].glyph, lx, ly, gh );
			if( legend[k].glyph2 >= 0 )
				lx += OSK_DrawGlyph( legend[k].glyph2, lx + 0.002f * refState.width, ly, gh ) + 0.002f * refState.width;

			lx += 0.005f * refState.width;
			Con_DrawStringLen( legend[k].text, &tw, &th );
			Con_DrawString( lx, ly + ( gh - th ) / 2, legend[k].text, osk_clr_dim );
			lx += tw + 0.016f * refState.width;
		}
	}
}

void OSK_Init( void )
{
	Cvar_RegisterVariable( &osk_enable );
	Cvar_Get( "osk_preview", "", 0, "text being edited, mirrored above the on-screen keyboard by the menu" );
}

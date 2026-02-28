/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2005 - 2015, ioquake3 contributors
Copyright (C) 2013 - 2015, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

// cl_scrn.c -- master for refresh, status bar, console, chat, notify, etc

#include "../server/exe_headers.h"

#include "client.h"
#include "client_ui.h"

extern console_t con;

qboolean	scr_initialized;		// ready to draw

cvar_t		*cl_timegraph;
cvar_t		*cl_debuggraph;
cvar_t		*cl_graphheight;
cvar_t		*cl_graphscale;
cvar_t		*cl_graphshift;

/*
================
SCR_DrawNamedPic

Coordinates are 640*480 virtual values
=================
*/
void SCR_DrawNamedPic( float x, float y, float width, float height, const char *picname ) {
	qhandle_t	hShader;

	assert( width != 0 );

	hShader = re.RegisterShader( picname );
	re.DrawStretchPic( x, y, width, height, 0, 0, 1, 1, hShader );
}


/*
================
SCR_FillRect

Coordinates are 640*480 virtual values
=================
*/
void SCR_FillRect( float x, float y, float width, float height, const float *color ) {
	re.SetColor( color );

	re.DrawStretchPic( x, y, width, height, 0, 0, 0, 0, cls.whiteShader );

	re.SetColor( NULL );
}


/*
================
SCR_DrawPic

Coordinates are 640*480 virtual values
A width of 0 will draw with the original image width
=================
*/
void SCR_DrawPic( float x, float y, float width, float height, qhandle_t hShader ) {
	re.DrawStretchPic( x, y, width, height, 0, 0, 1, 1, hShader );
}


/*
** SCR_DrawBigChar
** big chars are drawn at 640*480 virtual screen size
*/
void SCR_DrawBigChar( int x, int y, int ch ) {
	int row, col;
	float frow, fcol;
	float size;
	float	ax, ay, aw, ah;

	ch &= 255;

	if ( ch == ' ' ) {
		return;
	}

	if ( y < -BIGCHAR_HEIGHT ) {
		return;
	}

	ax = x;
	ay = y;
	aw = BIGCHAR_WIDTH;
	ah = BIGCHAR_HEIGHT;

	row = ch>>4;
	col = ch&15;

	frow = row*0.0625;
	fcol = col*0.0625;
	size = 0.0625;
/*
	re.DrawStretchPic( ax, ay, aw, ah,
					   fcol, frow,
					   fcol + size, frow + size,
					   cls.charSetShader );
*/
	float size2;

	frow = row*0.0625;
	fcol = col*0.0625;
	size = 0.03125;
	size2 = 0.0625;

	re.DrawStretchPic( ax, ay, aw, ah,
					   fcol, frow,
					   fcol + size, frow + size2,
					   cls.charSetShader );

}

/*
** SCR_DrawSmallChar
** small chars are drawn at native screen resolution
*/
void SCR_DrawSmallChar( int x, int y, int ch ) {
	int row, col;
	float frow, fcol;
	float size;

	ch &= 255;

	if ( ch == ' ' ) {
		return;
	}

	if ( y < -con.charHeight ) {
		return;
	}

	row = ch>>4;
	col = ch&15;
/*
	frow = row*0.0625;
	fcol = col*0.0625;
	size = 0.0625;

	re.DrawStretchPic( x, y, con.charWidth, con.charHeight,
					   fcol, frow,
					   fcol + size, frow + size,
					   cls.charSetShader );
*/

	float size2;

	frow = row*0.0625;
	fcol = col*0.0625;
	size = 0.03125;
	size2 = 0.0625;

	re.DrawStretchPic( x * con.xadjust, y * con.yadjust,
						con.charWidth * con.xadjust, con.charHeight * con.yadjust,
		fcol, frow,
		fcol + size, frow + size2,
		cls.charSetShader );

}



/*
==================
SCR_DrawBigString[Color]

Draws a multi-colored string with a drop shadow, optionally forcing
to a fixed color.

Coordinates are at 640 by 480 virtual resolution
==================
*/
void SCR_DrawBigStringExt( int x, int y, const char *string, float *setColor, qboolean forceColor, qboolean noColorEscape ) {
	vec4_t		color;
	const char	*s;
	int			xx;

	// draw the drop shadow
	color[0] = color[1] = color[2] = 0;
	color[3] = setColor[3];
	re.SetColor( color );
	s = string;
	xx = x;
	while ( *s ) {
		if ( !noColorEscape && Q_IsColorString( s ) ) {
			s += 2;
			continue;
		}
		SCR_DrawBigChar( xx+2, y+2, *s );
		xx += BIGCHAR_WIDTH;
		s++;
	}


	// draw the colored text
	s = string;
	xx = x;
	re.SetColor( setColor );
	while ( *s ) {
		if ( Q_IsColorString( s ) ) {
			if ( !forceColor ) {
				memcpy( color, g_color_table[ColorIndex(*(s+1))], sizeof( color ) );
				color[3] = setColor[3];
				re.SetColor( color );
			}
			if ( !noColorEscape ) {
				s += 2;
				continue;
			}
		}
		SCR_DrawBigChar( xx, y, *s );
		xx += BIGCHAR_WIDTH;
		s++;
	}
	re.SetColor( NULL );
}


void SCR_DrawBigString( int x, int y, const char *s, float alpha, qboolean noColorEscape ) {
	float	color[4];

	color[0] = color[1] = color[2] = 1.0;
	color[3] = alpha;
	SCR_DrawBigStringExt( x, y, s, color, qfalse, noColorEscape );
}

void SCR_DrawBigStringColor( int x, int y, const char *s, vec4_t color, qboolean noColorEscape ) {
	SCR_DrawBigStringExt( x, y, s, color, qtrue, noColorEscape );
}

/*
==================
SCR_DrawSmallString[Color]

Draws a multi-colored string with a drop shadow, optionally forcing
to a fixed color.
==================
*/
void SCR_DrawSmallStringExt( int x, int y, const char *string, float *setColor, qboolean forceColor,
		qboolean noColorEscape ) {
	vec4_t		color;
	const char	*s;
	int			xx;

	// draw the colored text
	s = string;
	xx = x;
	re.SetColor( setColor );
	while ( *s ) {
		if ( Q_IsColorString( s ) ) {
			if ( !forceColor ) {
				memcpy( color, g_color_table[ColorIndex(*(s+1))], sizeof( color ) );
				color[3] = setColor[3];
				re.SetColor( color );
			}
			if ( !noColorEscape ) {
				s += 2;
				continue;
			}
		}
		SCR_DrawSmallChar( xx, y, *s );
		xx += con.charWidth;
		s++;
	}
	re.SetColor( NULL );
}

/*
** SCR_Strlen -- skips color escape codes
*/
static int SCR_Strlen( const char *str ) {
	const char *s = str;
	int count = 0;

	while ( *s ) {
		if ( Q_IsColorString( s ) ) {
			s += 2;
		} else {
			count++;
			s++;
		}
	}

	return count;
}

/*
** SCR_GetBigStringWidth
*/
int	SCR_GetBigStringWidth( const char *str ) {
	return SCR_Strlen( str ) * BIGCHAR_WIDTH;
}

//===============================================================================


/*
===============================================================================

DEBUG GRAPH

===============================================================================
*/
typedef struct
{
	float	value;
	int		color;
} graphsamp_t;

static	int			current;
static	graphsamp_t	values[1024];

/*
==============
SCR_DebugGraph
==============
*/
void SCR_DebugGraph (float value, int color)
{
	values[current&1023].value = value;
	values[current&1023].color = color;
	current++;
}

/*
==============
SCR_DrawDebugGraph
==============
*/
void SCR_DrawDebugGraph (void)
{
	int		a, x, y, w, i, h;
	float	v;

	//
	// draw the graph
	//
	w = cls.glconfig.vidWidth;
	x = 0;
	y = cls.glconfig.vidHeight;
	re.SetColor( g_color_table[0] );
	re.DrawStretchPic(x, y - cl_graphheight->integer,
		w, cl_graphheight->integer, 0, 0, 0, 0, 0 );
	re.SetColor( NULL );

	for (a=0 ; a<w ; a++)
	{
		i = (current-1-a+1024) & 1023;
		v = values[i].value;
		v = v * cl_graphscale->integer + cl_graphshift->integer;

		if (v < 0)
			v += cl_graphheight->integer * (1+(int)(-v / cl_graphheight->integer));
		h = (int)v % cl_graphheight->integer;
		re.DrawStretchPic( x+w-1-a, y - h, 1, h, 0, 0, 0, 0, 0 );
	}
}
//=============================================================================

/*
==================
SCR_Init
==================
*/
void SCR_Init( void ) {
	cl_timegraph = Cvar_Get ("timegraph", "0", CVAR_CHEAT);
	cl_debuggraph = Cvar_Get ("debuggraph", "0", CVAR_CHEAT);
	cl_graphheight = Cvar_Get ("graphheight", "32", CVAR_CHEAT);
	cl_graphscale = Cvar_Get ("graphscale", "1", CVAR_CHEAT);
	cl_graphshift = Cvar_Get ("graphshift", "0", CVAR_CHEAT);

	scr_initialized = qtrue;
}


//=======================================================

void UI_SetActiveMenu( const char* menuname,const char *menuID );
void _UI_Refresh( int realtime );
void UI_DrawConnect( const char *servername, const char * updateInfoString );

/*
==================
SCR_DrawScreenField

This will be called twice if rendering in stereo mode
==================
*/
void SCR_DrawScreenField( stereoFrame_t stereoFrame ) {

	re.BeginFrame( stereoFrame );

	// =====================================================================
	// GHOUL2 SKINNED PLAYER MODEL TEST
	// =====================================================================
	{
		static CGhoul2Info_v testGhoul2;
		static qhandle_t     testG2Model = 0;
		static qhandle_t     testSkinHandle = 0;
		static qboolean      testInit = qfalse;
		static qboolean      testG2Valid = qfalse;
		static int           testLogCount = 0;

		if ( !testInit ) {
			testInit = qtrue;
			Cvar_Set( "r_nocull", "1" );

			// Register G2 model
			const char *modelPath = "models/characters/average_sleeves/average_sleeves.glm";
			testG2Model = re.RegisterModel( modelPath );
			Com_Printf( "[G2TEST] RegisterModel('%s') = %d\n", modelPath, testG2Model );

			if ( testG2Model ) {
				int g2Result = re.G2API_InitGhoul2Model(
					testGhoul2, modelPath, testG2Model, 0, 0, 0, 0 );
				Com_Printf( "[G2TEST] InitGhoul2Model result=%d size=%d\n",
					g2Result, testGhoul2.size() );

				if ( g2Result >= 0 && testGhoul2.size() > 0 ) {
					testG2Valid = qtrue;
					re.G2API_SetBoneAnim(
						&testGhoul2[0], "model_root",
						0, 30, BONE_ANIM_OVERRIDE_LOOP,
						1.0f, 0, -1, 150 );

					// Register and apply a skin file
					testSkinHandle = re.RegisterSkin(
						"models/characters/average_sleeves/model_default.skin" );
					Com_Printf( "[G2TEST] RegisterSkin result = %d\n", testSkinHandle );
					if ( testSkinHandle ) {
						re.G2API_SetSkin( &testGhoul2[0], testSkinHandle, testSkinHandle );
						Com_Printf( "[G2TEST] SetSkin applied (handle=%d)\n", testSkinHandle );
					}
				}
			}
		}

		// G2 time base
		re.G2API_SetTime( cls.realtime, 0 );

		// Build refdef
		refdef_t rd;
		memset( &rd, 0, sizeof( rd ) );
		rd.rdflags = RDF_NOWORLDMODEL;
		rd.x = 0;  rd.y = 0;
		rd.width = cls.glconfig.vidWidth;
		rd.height = cls.glconfig.vidHeight;
		rd.fov_x = 90;
		rd.fov_y = 73.74f;
		rd.time = cls.realtime;
		AxisClear( rd.viewaxis );

		re.ClearScene();

		// Black background
		{
			float black[4] = { 0, 0, 0, 1 };
			re.SetColor( black );
			re.DrawStretchPic( 0, 0, 640, 480, 0, 0, 0, 0, cls.whiteShader );
			re.SetColor( NULL );
		}

		// Add bright lights around the model
		{
			vec3_t lightPos;
			// Key light: front-left, above
			lightPos[0] = 60; lightPos[1] = -50; lightPos[2] = 40;
			re.AddLightToScene( lightPos, 500, 1.0f, 1.0f, 0.9f );
			// Fill light: front-right
			lightPos[0] = 60; lightPos[1] = 50; lightPos[2] = 10;
			re.AddLightToScene( lightPos, 400, 0.6f, 0.6f, 0.7f );
		}

		// G2 player model (centered)
		if ( testG2Valid ) {
			refEntity_t ent;
			memset( &ent, 0, sizeof( ent ) );
			ent.reType = RT_MODEL;
			ent.hModel = testG2Model;
			ent.ghoul2 = &testGhoul2;

			vec3_t angles;
			VectorSet( angles, 0, (float)( cls.realtime / 15.0f ), 0 );
			AnglesToAxis( angles, ent.axis );
			VectorCopy( angles, ent.angles );

			ent.origin[0] = 100;
			ent.origin[1] = 0;
			ent.origin[2] = -30;
			VectorCopy( ent.origin, ent.oldorigin );

			ent.modelScale[0] = 1.0f;
			ent.modelScale[1] = 1.0f;
			ent.modelScale[2] = 1.0f;

			ent.renderfx = RF_NOSHADOW;
			ent.customSkin = testSkinHandle;
			VectorCopy( ent.origin, ent.lightingOrigin );

			re.AddRefEntityToScene( &ent );
		}

		re.RenderScene( &rd );

		if ( testLogCount < 5 ) {
			Com_Printf( "[G2TEST] frame %d: g2=%d g2valid=%d time=%d\n",
				testLogCount, testG2Model, testG2Valid, cls.realtime );
			testLogCount++;
		}

		// Auto-screenshot at frame 60
		{
			static int frameCount = 0;
			frameCount++;
			if ( frameCount == 60 ) {
				Cbuf_ExecuteText( EXEC_NOW, "screenshot\n" );
				Com_Printf( "[G2TEST] Screenshot taken at frame %d\n", frameCount );
			}
		}
	}
}

/*
==================
SCR_UpdateScreen

This is called every frame, and can also be called explicitly to flush
text to the screen.
==================
*/
void SCR_UpdateScreen( void ) {
	static int	recursive;

	if ( !scr_initialized ) {
		return;				// not initialized yet
	}

	// load the ref / ui / cgame if needed
	CL_StartHunkUsers();

	if ( ++recursive > 2 ) {
		Com_Error( ERR_FATAL, "SCR_UpdateScreen: recursively called" );
	}
	recursive = qtrue;

	// If there is no VM, there are also no rendering commands issued. Stop the renderer in
	// that case.
	if ( cls.uiStarted )
	{
		// if running in stereo, we need to draw the frame twice
		if ( cls.glconfig.stereoEnabled ) {
			SCR_DrawScreenField( STEREO_LEFT );
			SCR_DrawScreenField( STEREO_RIGHT );
		} else {
			SCR_DrawScreenField( STEREO_CENTER );
		}

		if ( com_speeds->integer ) {
			re.EndFrame( &time_frontend, &time_backend );
		} else {
			re.EndFrame( NULL, NULL );
		}
	}

	recursive = 0;
}

// this stuff is only used by the savegame (SG) code for screenshots...
//


static byte	bScreenData[SG_SCR_WIDTH * SG_SCR_HEIGHT * 4];
static qboolean screenDataValid = qfalse;
void SCR_UnprecacheScreenshot()
{
	screenDataValid = qfalse;
}


void SCR_PrecacheScreenshot()
{
	// No screenshots unless connected to single player local server...
	//
//	char *psInfo = cl.gameState.stringData + cl.gameState.stringOffsets[ CS_SERVERINFO ];
//	int iMaxClients = atoi(Info_ValueForKey( psInfo, "sv_maxclients" ));

	// (no need to check single-player status in voyager, this code base is all singleplayer)
	if ( cls.state != CA_ACTIVE )
	{
		return;
	}

	if (!Key_GetCatcher( ))
	{
		// in-game...
		//
//		SCR_UnprecacheScreenshot();
//		pbScreenData = (byte *)Z_Malloc(SG_SCR_WIDTH * SG_SCR_HEIGHT * 4);
		S_ClearSoundBuffer();	// clear DMA etc because the following glReadPixels() call can take ages
		re.GetScreenShot( (byte *) &bScreenData, SG_SCR_WIDTH, SG_SCR_HEIGHT);
		screenDataValid = qtrue;
	}

}

// Returns a SG_SCR_WIDTH x SG_SCR_HEIGHT screenshot buffer
// Must match color components with DrawStretchRaw: 4
byte *SCR_GetScreenshot(qboolean *qValid)
{
	if (!screenDataValid) {
		SCR_PrecacheScreenshot();
	}
	if (qValid) {
		*qValid = screenDataValid;
	}
	return (byte *)&bScreenData;
}

// called from save-game code to set the lo-res loading screen to be the one from the save file...
//
void SCR_SetScreenshot(const byte *pbData, int w, int h)
{
	if (w == SG_SCR_WIDTH && h == SG_SCR_HEIGHT)
	{
		screenDataValid = qtrue;
		memcpy(&bScreenData, pbData, SG_SCR_WIDTH*SG_SCR_HEIGHT*4);
	}
	else
	{
		screenDataValid = qfalse;
		memset(&bScreenData, 0,      SG_SCR_WIDTH*SG_SCR_HEIGHT*4);
	}
}


#ifdef JK2_MODE
// This is just a client-side wrapper for the function RE_TempRawImage_ReadFromFile() in the renderer code...
//

byte* SCR_TempRawImage_ReadFromFile(const char *psLocalFilename, int *piWidth, int *piHeight, byte *pbReSampleBuffer, qboolean qbVertFlip)
{
	return re.TempRawImage_ReadFromFile(psLocalFilename, piWidth, piHeight, pbReSampleBuffer, qbVertFlip);
}
//
// ditto (sort of)...
//
void  SCR_TempRawImage_CleanUp()
{
	re.TempRawImage_CleanUp();
}
#endif



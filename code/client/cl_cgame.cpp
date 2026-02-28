/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
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

// cl_cgame.c  -- client system interaction with client game

// leave this as first line for PCH reasons...
//
#include "../server/exe_headers.h"
#include "../ui/ui_shared.h"

#include "client.h"
#include "vmachine.h"
#include "qcommon/stringed_ingame.h"
#include "sys/sys_loadlib.h"
#if !defined(G2_H_INC)
#include "../ghoul2/G2.h"
#endif
#include "../codeJK2/cgame/cg_public.h"
#include <intrin.h>
#ifdef _WIN32
#include <windows.h>
#endif

vm_t	cgvm;

// SOF2: cgame is a separate struct-based DLL loaded via GetCGameAPI(), not vmMain dispatch.
static cgame_export_t *cge          = NULL;
static void           *cgameLibrary = NULL;

// Forward declarations needed by wrapper helpers / import table below
extern qboolean CL_GetDefaultState( int index, entityState_t *state );
extern void CL_SetUserCmdValue( int userCmdValue, float sensitivityScale, float mPitchOverride, float mYawOverride );
extern void CL_GetCurrentSnapshotNumber( int *snapshotNumber, int *serverTime );
extern void AS_ParseSets( void );
extern void Com_WriteCam( const char *text );
// Audio / ambient set functions — must be forward-declared before any wrapper that calls them
extern void S_UpdateAmbientSet( const char *name, vec3_t origin );
extern int  S_AddLocalSet( const char *name, vec3_t listener_origin, vec3_t origin, int entID, int time );
extern sfxHandle_t AS_GetBModelSound( const char *name, int stage );
extern void AS_AddPrecacheEntry( const char *name );
// Functions defined later in this file but needed by CL_InitSOF2CGame:
void CL_GetGameState( gameState_t *gs );
void CL_GetGlconfig( glconfig_t *glconfig );
qboolean CL_GetUserCmd( int cmdNumber, usercmd_t *ucmd );
int CL_GetCurrentCmdNumber( void );
qboolean CL_GetSnapshot( int snapshotNumber, snapshot_t *snapshot );
void CL_AddCgameCommand( const char *cmdName );
qboolean CL_GetServerCommand( int serverCommandNumber );

// -----------------------------------------------------------------------
// Wrapper helpers for cgame_import_t signature mismatches
// -----------------------------------------------------------------------

// slot 32: Z_Malloc(int size) — SOF2 cgame wants single-arg version
static void *Z_Malloc_stub( int size ) {
	return Z_Malloc( size, TAG_ALL, qfalse );
}

// slot 67: CL_CM_LoadMap(name, clientLoad, checksum*) — SOF2 cgame signature
// Engine's CL_CM_LoadMap(name, subBSP) has different params; adapt here.
static void CL_CM_LoadMapWrapper( const char *mapname, int clientLoad, int *checksum ) {
	int localChecksum = 0;
	Com_Printf( "[DBG] CL_CM_LoadMapWrapper: map=%s clientLoad=%d checksum=%p\n",
				mapname ? mapname : "(null)", clientLoad, (void *)checksum );
	// Guard: if the DLL passes NULL or invalid checksum pointer, use local storage
	if ( !checksum ) {
		checksum = &localChecksum;
	}
	CM_LoadMap( mapname, (qboolean)(clientLoad != 0), checksum, qfalse );
	Com_Printf( "[DBG] CL_CM_LoadMapWrapper: done, checksum=%d\n", *checksum );
}

// slot 96: CL_GetEntityBaseline(entityNum, state*) — wraps CL_GetDefaultState
static void CL_GetEntityBaseline_SOF2( int entityNum, entityState_t *state ) {
	CL_GetDefaultState( entityNum, state );
}

// slot 102: CL_SetUserCmdValue(userCmdValue, sensitivityScale) — 2 args (no pitch/yaw override)
static void CL_SetUserCmdValue_SOF2( int userCmdValue, float sensitivityScale ) {
	CL_SetUserCmdValue( userCmdValue, sensitivityScale, 0.0f, 0.0f );
}

// slot 22: Cmd_ArgsBuffer(start, buf, bufsize) — SOF2 adds a 'start' param; OpenJK ignores it
static void Cmd_ArgsBuffer_SOF2( int /*start*/, char *buf, int bufsize ) {
	Cmd_ArgsBuffer( buf, bufsize );
}

// slot 77: S_AddLoopingSound — SOF2 passes 4 args; OpenJK has 5th soundChannel_t
static void S_AddLoopingSound_SOF2( int entityNum, const vec3_t origin, const vec3_t velocity,
		sfxHandle_t sfx ) {
	S_AddLoopingSound( entityNum, origin, velocity, sfx );
}

// slot 80: S_RegisterSound — SOF2 passes (name, compressed, streamed); OpenJK takes (name) only
static sfxHandle_t S_RegisterSound_SOF2( const char *name, int /*compressed*/, int /*streamed*/ ) {
	return S_RegisterSound( name );
}

// slot 83: S_StartBackgroundTrack — SOF2 passes fadeupTime (int); OpenJK takes qboolean
static void S_StartBackgroundTrack_SOF2( const char *intro, const char *loop, int /*fadeupTime*/ ) {
	S_StartBackgroundTrack( intro, loop, qfalse );
}

// slot 17: COM_Parse — OpenJK takes (const char **); SOF2 slot wants (char **)
static char *COM_Parse_wrapper( char **p ) {
	return COM_Parse( (const char **)p );
}

// slot 76: S_ClearLoopingSounds — OpenJK takes void; SOF2 slot passes int killall
static void S_ClearLoopingSounds_stub( int /*killall*/ ) {
	S_ClearLoopingSounds();
}

// slot 126: R_GetLighting — OpenJK returns qboolean; SOF2 import wants void.
// Guard against NULL pointers from the cgame DLL and early calls before world is loaded.
static void R_GetLighting_SOF2( const vec3_t pos, float *ambient, float *directed, vec3_t dir ) {
	vec3_t dummyAmb = {255,255,255}, dummyDir = {255,255,255}, dummyLDir = {0,0,1};
	if ( !ambient )  ambient  = dummyAmb;
	if ( !directed ) directed = dummyDir;
	if ( !dir )      dir      = dummyLDir;
	re.GetLighting( pos, ambient, directed, dir );
}

// ----- Slot 13: FS_FreeFileList -----
static void CG_FS_FreeFileList( char **list ) {
	FS_FreeFileList( list );
}

// ----- Slot 14: FS_CleanPath -----
// SOF2 cleans a path in-place (stripping .. etc). Use COM_StripExtension as rough stand-in
// or just do basic path normalization. For now, no-op — path is used as-is.
static void CG_FS_CleanPath( char * /*path*/ ) {
	// no-op stub
}

// ----- Slot 27: Cvar_SetModified -----
// Marks a cvar as modified. Since Cvar_FindVar is static, we re-set the cvar to its own value.
static void CG_Cvar_SetModified( const char *name ) {
	char buf[MAX_CVAR_VALUE_STRING];
	Cvar_VariableStringBuffer( name, buf, sizeof(buf) );
	Cvar_Set( name, buf );
}

// ----- Slot 34: Z_CheckHeap -----
static void CG_Z_CheckHeap( void ) {
	// no-op in OpenJK — zone memory uses malloc, no separate heap check
}

// ----- Slot 78: S_StopLoopingSound -----
static void CG_S_StopLoopingSound( int /*entityNum*/ ) {
	// SOF2 stops looping sounds for an entity. OpenJK doesn't expose this.
	// S_ClearLoopingSounds() clears all; per-entity stop is a no-op.
}

// ----- Slot 82: S_MuteSound -----
static int CG_S_MuteSound( int /*entityNum*/, sfxHandle_t /*sfxHandle*/ ) {
	// no-op stub — muting individual sounds not supported in OpenJK
	return 0;
}

// ----- Slot 100: CL_SetClientViewAngles -----
static int CG_CL_SetClientViewAngles( const vec3_t angles ) {
	cl.viewangles[0] = angles[0];
	cl.viewangles[1] = angles[1];
	cl.viewangles[2] = angles[2];
	return 1;
}

// ----- Slot 101: CL_GetMouseDir -----
static void CG_CL_GetMouseDir( int *xDir, int *yDir ) {
	if ( xDir ) *xDir = 0;
	if ( yDir ) *yDir = 0;
}

// ----- Slot 114: SE_GetStringIndex -----
static int CG_SE_GetStringIndex( const char * /*token*/ ) {
	return -1; // no string index system
}

// ----- Slot 115: CL_SetLastBoneIndex -----
static void CG_CL_SetLastBoneIndex( int /*boneIndexBits*/ ) {
	// no-op — SOF2-specific bone index optimization
}

// ===== Client-side entity token parsing (slot 150) =====
// CG_ParseSpawnVars calls this to read BSP entity string tokens for
// client-side decorations/props. Maintains its own parse pointer.
static const char *cl_entityParsePoint = NULL;

static qboolean CG_GetEntityToken( char *buffer, int bufferSize ) {
	if ( !cl_entityParsePoint ) {
		cl_entityParsePoint = CM_EntityString();
	}
	const char *s = COM_Parse( &cl_entityParsePoint );
	Q_strncpyz( buffer, s, bufferSize );
	if ( !cl_entityParsePoint && !s[0] ) {
		return qfalse;
	}
	return qtrue;
}

// ===== Weather/renderer stubs (slots 152-154) =====

// ----- Slot 153: R_IsOutside (rain check) -----
static qboolean CG_R_IsOutside( void ) {
	return qfalse; // no rain rendering for now
}

// ----- Slot 154: R_GetWindVector -----
static qboolean CG_R_GetWindVector( vec3_t windVector ) {
	VectorClear( windVector );
	return qfalse;
}

// ===== Force Feedback stubs (slots 157-162) =====
static int cg_ffNextHandle = 1;

// ----- Slot 157: FF_RegisterEffect -----
static int CG_FF_RegisterEffect( const char * /*name*/ ) {
	return cg_ffNextHandle++; // return unique non-zero handle
}

// ----- Slot 158: FF_Play -----
static void CG_FF_Play( int /*ffHandle*/ ) {
	// no-op — no force feedback hardware
}

// ----- Slot 161: FF_StopAll / SetPlayerSuit -----
static void CG_FF_StopAll( void ) {
	// no-op
}


// ===== Ghoul2 handle-based wrappers (slots 116-123) =====
// SOF2's cgame DLL uses unsigned int handles for Ghoul2 models.
// These wrappers convert handles to CGhoul2Info_v& for the renderer API.

// ----- Slot 116: G2_InitWraithSurfaceMap -----
// SOF2 cgame calls this with (void **outPtr) expecting qboolean return.
// Same pattern as server-side SV_G2_InitWraithSurfaceMap — returns CWraithStub*.
// The wraith is a C++ object with a 78-entry vtable wrapping all G2 API calls.
extern void *SV_GetWraithStubPtr( void );  // defined in sv_game.cpp

static qboolean CG_G2_InitWraithSurfaceMap( void **outPtr ) {
	void *wraith = SV_GetWraithStubPtr();
	if ( outPtr ) *outPtr = wraith;
	Com_DPrintf( "[CGAME G2] InitWraithSurfaceMap: wraith=%p\n", wraith );
	return wraith ? qtrue : qfalse;
}

// ----- Slot 41: CM_GetTerrainBounds -----
// Returns the aggregate bounding box of all registered terrain instances.
// Called from CG_RegisterGraphics after terrain loading.
static void CG_CM_GetTerrainBounds( vec3_t mins, vec3_t maxs ) {
	// Set large default bounds — actual terrain may not exist on all maps
	VectorSet( mins, -65536, -65536, -65536 );
	VectorSet( maxs, 65536, 65536, 65536 );
}

// ----- Slot 117: G2API_HaveWeGhoul2Models -----
// The DLL passes CGhoul2Info_v& (pointer to struct with mItem at offset 0)
static qboolean CG_G2API_HaveWeGhoul2Models( CGhoul2Info_v &ghoul2 ) {
	return re.G2API_HaveWeGhoul2Models( ghoul2 );
}

// ----- Slot 118: G2_GetGhoul2InfoByHandle -----
// Returns CGhoul2Info_v* from a handle. The DLL uses this to get a reference.
static CGhoul2Info_v *CG_G2_GetGhoul2InfoByHandle( int handle ) {
	// Return a pointer to the CGhoul2Info_v wrapper. Since handles are just
	// indices into the renderer's array, and the DLL only reads mItem from
	// the returned pointer, we return it from a static to keep it alive.
	static CGhoul2Info_v g2Temp;
	// Avoid the destructor freeing the handle by directly setting mItem
	// CGhoul2Info_v has mItem at offset 0
	*(int *)&g2Temp = handle;
	return &g2Temp;
}

// ----- Slot 119: G2API_SetGhoul2ModelIndexes -----
static void CG_G2API_SetGhoul2ModelIndexes( unsigned int ghoul2Handle,
		qhandle_t *modelList ) {
	if ( ghoul2Handle == 0 ) return;
	CGhoul2Info_v &g2 = *(CGhoul2Info_v *)&ghoul2Handle;
	re.G2API_SetGhoul2ModelIndexes( g2, modelList, NULL );
}

// ----- Slot 120: G2API_ReRegisterModels -----
// SOF2 DLL calls this to re-register Ghoul2 models after a vid_restart
static void CG_G2API_ReRegisterModels( unsigned int /*ghoul2Handle*/ ) {
	// no-op — OpenJK handles model re-registration internally
}

// ----- Slot 121: G2API_GetBoltMatrix -----
static qboolean CG_G2API_GetBoltMatrix( unsigned int ghoul2Handle,
		int modelIndex, int boltIndex, mdxaBone_t *matrix,
		const vec3_t angles, const vec3_t position, int frameNum,
		qhandle_t *modelList, float scale ) {
	if ( ghoul2Handle == 0 ) return qfalse;
	CGhoul2Info_v &g2 = *(CGhoul2Info_v *)&ghoul2Handle;
	vec3_t scaleVec = { scale, scale, scale };
	return re.G2API_GetBoltMatrix( g2, modelIndex, boltIndex, matrix,
		angles, position, frameNum, modelList, scaleVec );
}

// ----- Slot 122: G2API_SetBoneAnglesOffset -----
static qboolean CG_G2API_SetBoneAnglesOffset( unsigned int ghoul2Handle,
		int modelIndex, const char *boneName, const vec3_t angles,
		int flags, int up, int right, int forward,
		qhandle_t *modelList ) {
	if ( ghoul2Handle == 0 ) return qfalse;
	IGhoul2InfoArray &arr = re.TheGhoul2InfoArray();
	int handle = (int)ghoul2Handle;
	if ( !arr.IsValid( handle ) ) return qfalse;
	std::vector<CGhoul2Info> &ghoul2 = arr.Get( handle );
	if ( modelIndex < 0 || modelIndex >= (int)ghoul2.size() ) return qfalse;
	return re.G2API_SetBoneAngles( &ghoul2[modelIndex], boneName, angles,
		flags, (Eorientations)up, (Eorientations)right, (Eorientations)forward,
		modelList, 0, 0 );
}

// ----- Slot 134: R_AddMiniRefEntityToScene -----
// SOF2-specific mini entity — just add as regular entity
static void CG_R_AddMiniRefEntityToScene( const refEntity_t *ent ) {
	if ( ent ) re.AddRefEntityToScene( ent );
}

// ----- Slot 137: R_AddDirectedLightToScene -----
static void CG_R_AddDirectedLightToScene( const vec3_t dir, float intensity,
		float r, float g, float b ) {
	(void)dir; (void)intensity; (void)r; (void)g; (void)b;
	// no-op — OpenJK doesn't have directed lights in scene API
}

// ----- Slot 138: R_AddAdditiveLightToScene -----
static void CG_R_AddAdditiveLightToScene( const vec3_t org, float intensity,
		float r, float g, float b ) {
	// Add as regular light
	re.AddLightToScene( org, intensity, r, g, b );
}

// ----- Slot 141: R_FillRect -----
static void CG_R_FillRect( float x, float y, float w, float h, const float *color ) {
	re.SetColor( color );
	re.DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re.SetColor( NULL );
}

// ----- Slot 144: R_RemapShader -----
// SOF2 DLL: R_RemapShader(oldName, newName, timeOffset)
// R_RemapShader exists in renderer but isn't in refexport_t. Stub for now.
static void CG_R_RemapShader( const char *oldShader, const char *newShader, const char *timeOffset ) {
	Com_DPrintf( "[CGAME] R_RemapShader: %s -> %s (time=%s) — stub\n",
		oldShader ? oldShader : "null", newShader ? newShader : "null",
		timeOffset ? timeOffset : "0" );
}

// ----- Slot 147: R_RegisterFont (SOF2 has extra param) -----
// SOF2 DLL: void R_RegisterFont(name, pointSize, int *handleOut, unk)
// OpenJK renderer: int RegisterFont(name) — returns handle
static void CG_R_RegisterFont( const char *name, int /*pointSize*/, void *font, int /*unk*/ ) {
	if ( !name || !name[0] ) {
		Com_DPrintf( "[CGAME] R_RegisterFont: NULL or empty name, returning 0\n" );
		if ( font ) *(int *)font = 0;
		return;
	}
	int handle = re.RegisterFont( name );
	if ( font ) *(int *)font = handle;
}

// ----- Slot 149: R_Font_StrLenPixels -----
// SOF2 DLL passes font handle as void * (int value, not a pointer)
static float CG_R_Font_StrLenPixels( const void *font, const char *text, float scale ) {
	int handle = (int)(intptr_t)font;
	return (float)re.Font_StrLenPixels( text, handle, scale );
}

// ----- Slot 150: R_Font_HeightPixels -----
static float CG_R_Font_HeightPixels( const void *font, float scale ) {
	int handle = (int)(intptr_t)font;
	return (float)re.Font_HeightPixels( handle, scale );
}

// ----- Slot 151: R_Font_DrawString -----
static void CG_R_Font_DrawString( const void *font, const char *text,
		float x, float y, float scale, const float *rgba, int style ) {
	int handle = (int)(intptr_t)font;
	re.Font_DrawString( (int)x, (int)y, text, rgba, handle, -1, scale );
}

// ----- Slot 152: R_GetLightStyle -----
static int CG_R_GetLightStyle( int /*index*/, float * /*rgba*/ ) {
	return 0; // stub
}

// ----- Slot 153: R_SetLightStyle -----
// SOF2 passes (int index, float r, float g, float b)
// OpenJK renderer: void SetLightStyle(int style, int color) where color = packed RGB
static void CG_R_SetLightStyle( int index, float r, float g, float b ) {
	int ir = (int)(r * 255.0f) & 0xFF;
	int ig = (int)(g * 255.0f) & 0xFF;
	int ib = (int)(b * 255.0f) & 0xFF;
	int color = (ir) | (ig << 8) | (ib << 16);
	re.SetLightStyle( index, color );
}

// ----- Slot 59: UI_LoadMenuData(menuData, category) -----
// SOF2 cgame loads inventory menu data by category ("default", "weapons", "cinematic").
// Called from CG_LoadInventoryMenus, CG_ServerCommand, CG_CamEnable.
// We stub this — UI menus are not functional yet.
static void CG_UI_LoadMenuData( const char *menuData, const char *category ) {
	Com_DPrintf( "[CGAME] UI_LoadMenuData: data='%.32s' cat='%s' — stub\n",
		menuData ? menuData : "(null)", category ? category : "(null)" );
}

// ----- Slot 61: UI_MenuReset() -----
// Resets UI menu system state before reloading menu definitions.
// Called from CG_LoadInventoryMenus as first step.
static void CG_UI_MenuReset( void ) {
	Com_DPrintf( "[CGAME] UI_MenuReset — stub\n" );
}

// ----- Slot 62: UI_SetInfoScreenText(text) -----
// Sets the loading screen info text widget content.
// Called from CG_SetLoadingLevelshot. Cosmetic only.
static void CG_UI_SetInfoScreenText( const char * /*text*/ ) {
	// no-op — loading screen text widget not implemented
}

// ----- Slots 105-108: GenericParser2 API stubs -----
// SOF2's cgame uses GP2 for weapon definitions and HUD widget config.
// These are handle-based wrappers around CGenericParser2.
// Currently stubbed — GP2 features (weapon configs, HUD widgets) won't parse,
// but the game continues running. Called only during init.
static void CG_GP_GetBaseParseGroup( int /*handle*/ ) {
	// no-op — returns nothing (void), DLL reads from its own state
}

static int CG_GP_GetSubGroups( void ) {
	return 0; // no sub-groups available
}

static void CG_GP_GetNext( void * /*dest*/ ) {
	// no-op — destination state left as-is
}

// ----- Slot 148: RE_DamageSurface -----
// SOF2-specific renderer function for gore/damage texture lookup.
// Called after RE_RegisterModel during CG_ClientModel init.
// Takes pointer to entity/model struct. No-op is safe — just no gore surfaces.
static int CG_RE_DamageSurface( const void * /*ent*/ ) {
	return 0; // no damage surface info
}

// ----- Slot 132: FX parent entity association -----
// SOF2-specific FX system import: associates rendered FX primitives with a
// parent entity number. Called from FxScheduler and FX primitive renderers.
// Takes 1 int arg (entityNum), returns void. NOT AddMiniRefEntityToScene.
static void CG_FX_SetParentEntity( int /*entityNum*/ ) {
	// no-op — FX entity association not needed for basic rendering
}

// Generic sentinel for NULL cgame import slots — logs which slot was called.
// Uses the return address to figure out caller, plus we stash slot number via
// a trampoline table.  For simplicity, just log a fatal error with the return address.
static int CG_NullSlotSentinel( void ) {
	// Use MSVC intrinsic to get return address for debugging
	void *retAddr = _ReturnAddress();
	Com_Printf( "^1[CGAME] Called NULL import slot! retAddr=%p\n", retAddr );
	return 0;
}

// slot 0: Printf — SOF2 slot takes pre-formatted string only (no variadic)
static void Printf_stub( const char *s ) { Com_Printf( "%s", s ); }

// slot 3: Sprintf — Com_sprintf returns int; SOF2 slot is void
static void Sprintf_void( char *buf, int size, const char *fmt, ... ) {
	va_list args;
	va_start( args, fmt );
	Q_vsnprintf( buf, size, fmt, args );
	va_end( args );
}

// slot 34: Z_Free — Z_Free returns int; SOF2 slot expects void
static void Z_Free_void_stub( void *p ) { Z_Free( p ); }

// slot 74: S_StartSound — SOF2 slot uses int for channel; engine uses soundChannel_t
static void S_StartSound_wrapper( const vec3_t origin, int entityNum,
		int entChannel, sfxHandle_t sfx ) {
	S_StartSound( origin, entityNum, (soundChannel_t)entChannel, sfx );
}

// slot 85: S_UpdateAmbientSet — SOF2 slot takes const vec3_t; engine takes vec3_t
static void S_UpdateAmbientSet_wrapper( const char *name, const vec3_t origin ) {
	S_UpdateAmbientSet( name, (float *)origin );
}

// slot 86: S_AddLocalSet — SOF2 has 5 params; pass them all through
// Use float* casts to strip const from the array types
static int S_AddLocalSet_wrapper( const char *name, const vec3_t listener,
		const vec3_t origin, int entID, int time ) {
	return S_AddLocalSet( name, (float *)listener, (float *)origin, entID, time );
}

// slot 89: AS_GetBModelSound — SOF2 returns const char*; engine returns sfxHandle_t (int)
// The return value is ABI-compatible (both 32-bit); the DLL uses it as a handle
static const char *AS_GetBModelSound_wrapper( const char *name, int stage ) {
	sfxHandle_t h = AS_GetBModelSound( name, stage );
	return (const char *)(intptr_t)h;
}

// slot 112/113: SE_GetString / SE_DisplayString — SOF2 takes (const char *, int);
// OpenJK SE_GetString is overloaded; use the single-arg version and ignore index
static char *SE_GetString_wrapper( const char *ref, int /*index*/ ) {
	return (char *)SE_GetString( ref );
}

// slot 143: R_LerpTag — OpenJK returns void; SOF2 import wants int
static int R_LerpTag_SOF2( orientation_t *tag, qhandle_t handle, int startFrame, int endFrame,
		float frac, const char *tagName ) {
	re.LerpTag( tag, handle, startFrame, endFrame, frac, tagName );
	return 0;
}

// slot 135: R_AddPolyToScene — SOF2 has extra 'num' param; OpenJK takes 3 args
static void R_AddPolyToScene_SOF2( qhandle_t hShader, int numVerts, const polyVert_t *verts, int /*num*/ ) {
	re.AddPolyToScene( hShader, numVerts, verts );
}

// slot 87: AS_ParseSets wrapper (SOF2 passes filename; current sig takes void)
// NOTE: OpenJK AS_ParseSets() takes no args; SOF2 passes a filename which we ignore for now
static void AS_ParseSets_wrapper( const char * /*filename*/ ) {
	AS_ParseSets();
}

// slot 109: CL_Key_IsDown — Key_IsDown returns qboolean; slot expects int
static int Key_IsDown_int( int key ) { return (int)Key_IsDown( key ); }

// slot 97: CL_GetServerCommand — CL_GetServerCommand returns qboolean; slot expects int
static int CL_GetServerCommand_int( int serverCommandNumber ) {
	return (int)CL_GetServerCommand( serverCommandNumber );
}

// slot 94: CL_GetCurrentSnapshotNumber wrapper (SOF2 returns int, engine func returns void)
static int CL_GetCurrentSnapshotNumber_wrapper( int *snapshotNumber, int *serverTime ) {
	CL_GetCurrentSnapshotNumber( snapshotNumber, serverTime );
	return *snapshotNumber;
}

// slot 57: Com_WriteCam — slot is variadic; engine function takes pre-formatted string only
static void Com_WriteCam_wrapper( const char *fmt, ... ) {
	va_list args;
	char buf[1024];
	va_start( args, fmt );
	Q_vsnprintf( buf, sizeof(buf), fmt, args );
	va_end( args );
	Com_WriteCam( buf );
}
/*
Ghoul2 Insert Start
*/

#if !defined(G2_H_INC)
	#include "../ghoul2/G2.h"
#endif

/*
Ghoul2 Insert End
*/

// Forward-declared above; duplicate extern block removed.
extern menuDef_t *Menus_FindByName(const char *p);

extern qboolean R_inPVS( vec3_t p1, vec3_t p2 );

void UI_SetActiveMenu( const char* menuname,const char *menuID );

// CL_InitCGameVM — kept as a no-op stub; SOF2 uses CL_InitSOF2CGame() instead.
qboolean CL_InitCGameVM( void * /*gameLibrary*/ )
{
	return qtrue;
}

/*
===================
CL_InitSOF2CGame

Load cgamex86.dll and populate the 163-slot cgame_import_t for SOF2 CGAME_API_VERSION 3.
Called from CL_InitCGame() when the map starts.
===================
*/
qboolean CL_InitSOF2CGame( void ) {
	typedef cgame_export_t *GetCGameAPIProc( int version, cgame_import_t *import );

	cgameLibrary = Sys_LoadSPGameDll( "cgame", NULL );   // "cgame" + ARCH_STRING("x86") + DLL_EXT = "cgamex86.dll"
	if ( !cgameLibrary ) {
		Com_Printf( "CL_InitSOF2CGame: failed to load cgamex86" ARCH_STRING DLL_EXT ": %s\n",
					Sys_LibraryError() );
		return qfalse;
	}

	GetCGameAPIProc *GetCGameAPI = (GetCGameAPIProc *)Sys_LoadFunction( cgameLibrary, "GetCGameAPI" );
	if ( !GetCGameAPI ) {
		Sys_UnloadDll( cgameLibrary );
		cgameLibrary = NULL;
		Com_Printf( "CL_InitSOF2CGame: GetCGameAPI not found\n" );
		return qfalse;
	}

	// -----------------------------------------------------------------------
	// Populate cgame_import_t (163 entries, zero-initialized).
	// Unset slots remain NULL — non-critical features will simply be stubs.
	// Full slot reference: E:\SOF2\structs\cgame_import_t.h
	// -----------------------------------------------------------------------
	cgame_import_t import = {};

	// [  0] Printf — SOF2 slot takes pre-formatted string; wrap to add format
	import.Printf                     = Printf_stub;
	// [  1] DPrintf
	import.DPrintf                    = Com_DPrintf;
	// [  2] DPrintf2 (same function, alternate verbosity)
	import.DPrintf2                   = Com_DPrintf;
	// [  3] Sprintf — wrap to discard return value
	import.Sprintf                    = Sprintf_void;
	// [  4] Error
	import.Error                      = Com_Error;

	// [  5] FS_FOpenFile
	import.FS_FOpenFile               = FS_FOpenFileByMode;
	// [  6] FS_Read
	import.FS_Read                    = FS_Read;
	// [  7] FS_Write
	import.FS_Write                   = FS_Write;
	// [  8] FS_FCloseFile
	import.FS_FCloseFile              = FS_FCloseFile;
	// [  9] FS_ReadFile
	import.FS_ReadFile                = FS_ReadFile;
	// [ 10] FS_FreeFile
	import.FS_FreeFile                = FS_FreeFile;
	// [ 11] FS_FileExists — cast qboolean to int (compatible at binary level)
	import.FS_FileExists              = (int (*)(const char *))FS_FileExists;
	// [ 12] FS_GetFileList
	import.FS_GetFileList             = FS_GetFileList;
	// [ 13] FS_FreeFileList
	import.FS_FreeFileList            = CG_FS_FreeFileList;
	// [ 14] FS_CleanPath
	import.FS_CleanPath               = CG_FS_CleanPath;

	// [ 15] Com_EventLoop (returns void in OpenJK; cast to match SOF2's int return)
	import.Com_EventLoop              = (int (*)(void))Com_EventLoop;
	// [ 16] Cmd_TokenizeString
	import.Cmd_TokenizeString         = Cmd_TokenizeString;
	// [ 17] COM_Parse — wrapper to handle const char** vs char** mismatch
	import.COM_Parse                  = COM_Parse_wrapper;
	// [ 18] Cbuf_AddText
	import.Cbuf_AddText               = Cbuf_AddText;
	// [ 19] Cbuf_ExecuteText
	import.Cbuf_ExecuteText           = Cbuf_ExecuteText;

	// [ 20] Cmd_Argc
	import.Cmd_Argc                   = Cmd_Argc;
	// [ 21] Cmd_ArgvBuffer
	import.Cmd_ArgvBuffer             = Cmd_ArgvBuffer;
	// [ 22] Cmd_ArgsBuffer (SOF2 adds 'start' param)
	import.Cmd_ArgsBuffer             = Cmd_ArgsBuffer_SOF2;
	// [ 23] Cvar_GetModified — Cvar_Get returns cvar_t*, stub with Cvar_Get
	import.Cvar_GetModified           = Cvar_Get;
	// [ 24] Cvar_Register
	import.Cvar_Register              = Cvar_Register;
	// [ 25] Cvar_Update
	import.Cvar_Update                = Cvar_Update;
	// [ 26] Cvar_Set
	import.Cvar_Set                   = Cvar_Set;
	// [ 27] Cvar_SetModified
	import.Cvar_SetModified           = CG_Cvar_SetModified;
	// [ 28] Cvar_SetValue
	import.Cvar_SetValue              = Cvar_SetValue;
	// [ 29] Cvar_VariableIntegerValue
	import.Cvar_VariableIntegerValue  = Cvar_VariableIntegerValue;
	// [ 30] Cvar_VariableValue
	import.Cvar_VariableValue         = Cvar_VariableValue;
	// [ 31] Cvar_VariableStringBuffer
	import.Cvar_VariableStringBuffer  = Cvar_VariableStringBuffer;

	// [ 32] Z_Malloc
	import.Z_Malloc                   = Z_Malloc_stub;
	// [ 33] Z_Free — wrap to discard int return
	import.Z_Free                     = Z_Free_void_stub;
	// [ 34] Z_CheckHeap
	import.Z_CheckHeap                = CG_Z_CheckHeap;

	// [35-49] Terrain system — all stubs for now (Phase 2+)

	// [50-52] Renderer image loading — stubs

	// [53-56] Frustum/culling — stubs

	// [ 57] Com_WriteCam — variadic wrapper needed (engine fn is non-variadic)
	import.Com_WriteCam               = Com_WriteCam_wrapper;
	// [ 58] UpdateScreen
	import.UpdateScreen               = SCR_UpdateScreen;
	// [59-65] METIS UI dispatch — stubs

	// [ 66] CM_PointContents
	import.CM_PointContents           = CM_PointContents;
	// [ 67] CL_CM_LoadMap
	import.CL_CM_LoadMap              = CL_CM_LoadMapWrapper;
	// [ 68] CM_NumInlineModels
	import.CM_NumInlineModels         = CM_NumInlineModels;
	// [ 69] CM_InlineModel
	import.CM_InlineModel             = CM_InlineModel;
	// [ 70] CM_TempBoxModel
	import.CM_TempBoxModel            = CM_TempBoxModel;
	// [ 71] CM_TransformedPointContents
	import.CM_TransformedPointContents = CM_TransformedPointContents;
	// [ 72] CM_BoxTrace
	import.CM_BoxTrace                = CM_BoxTrace;
	// [ 73] CM_TransformedBoxTrace
	import.CM_TransformedBoxTrace     = CM_TransformedBoxTrace;

	// [ 74] S_StartSound — wrap to cast soundChannel_t → int
	import.S_StartSound               = S_StartSound_wrapper;
	// [ 75] S_StartLocalSound
	import.S_StartLocalSound          = S_StartLocalSound;
	// [ 76] S_ClearLoopingSounds — OpenJK takes void; SOF2 slot passes int killall
	import.S_ClearLoopingSounds       = S_ClearLoopingSounds_stub;
	// [ 77] S_AddLoopingSound (SOF2: 4 args; OpenJK has 5th soundChannel_t default param)
	import.S_AddLoopingSound          = S_AddLoopingSound_SOF2;
	// [ 78] S_StopLoopingSound
	import.S_StopLoopingSound         = CG_S_StopLoopingSound;
	// [ 79] S_Respatialize (qboolean inwater is compatible with int)
	import.S_Respatialize             = (void (*)(int, const vec3_t, vec3_t[3], int))S_Respatialize;
	// [ 80] S_RegisterSound (SOF2: extra compressed/streamed params ignored)
	import.S_RegisterSound            = S_RegisterSound_SOF2;
	// [ 81] S_UpdateEntityPosition
	import.S_UpdateEntityPosition     = S_UpdateEntityPosition;
	// [ 82] S_MuteSound
	import.S_MuteSound                = CG_S_MuteSound;
	// [ 83] S_StartBackgroundTrack (SOF2: fadeupTime ignored)
	import.S_StartBackgroundTrack     = S_StartBackgroundTrack_SOF2;
	// [ 84] S_StopBackgroundTrack
	import.S_StopBackgroundTrack      = S_StopBackgroundTrack;
	// [ 85] S_UpdateAmbientSet — wrap: const vec3_t vs vec3_t
	import.S_UpdateAmbientSet         = S_UpdateAmbientSet_wrapper;
	// [ 86] S_AddLocalSet — wrap: SOF2 has 5 params; engine has 3
	import.S_AddLocalSet              = S_AddLocalSet_wrapper;
	// [ 87] AS_ParseSets
	import.AS_ParseSets               = AS_ParseSets_wrapper;
	// [ 88] AS_AddPrecacheEntry
	import.AS_AddPrecacheEntry        = AS_AddPrecacheEntry;
	// [ 89] AS_GetBModelSound — wrap: sfxHandle_t → const char * (bit-compatible)
	import.AS_GetBModelSound          = AS_GetBModelSound_wrapper;

	// [ 90] CL_GetGlconfig
	import.CL_GetGlconfig             = CL_GetGlconfig;
	// [ 91] CL_GetGameState
	import.CL_GetGameState            = CL_GetGameState;
	// [ 92] CL_AddCgameCommand
	import.CL_AddCgameCommand         = CL_AddCgameCommand;
	// [ 93] CL_AddReliableCommand
	import.CL_AddReliableCommand      = CL_AddReliableCommand;
	// [ 94] CL_GetCurrentSnapshotNumber
	import.CL_GetCurrentSnapshotNumber = CL_GetCurrentSnapshotNumber_wrapper;
	// [ 95] CL_GetSnapshot
	import.CL_GetSnapshot             = CL_GetSnapshot;
	// [ 96] CL_GetEntityBaseline
	import.CL_GetEntityBaseline       = CL_GetEntityBaseline_SOF2;
	// [ 97] CL_GetServerCommand — int wrapper (engine fn returns qboolean)
	import.CL_GetServerCommand        = CL_GetServerCommand_int;
	// [ 98] CL_GetCurrentCmdNumber
	import.CL_GetCurrentCmdNumber     = CL_GetCurrentCmdNumber;
	// [ 99] CL_GetUserCmd
	import.CL_GetUserCmd              = CL_GetUserCmd;

	// [100] CL_SetClientViewAngles
	import.CL_SetClientViewAngles     = CG_CL_SetClientViewAngles;
	// [101] CL_GetMouseDir
	import.CL_GetMouseDir             = CG_CL_GetMouseDir;
	// [102] CL_SetUserCmdValue
	import.CL_SetUserCmdValue         = CL_SetUserCmdValue_SOF2;
	// [103-108] CM debug / GP2 — stubs
	// [109] CL_Key_IsDown — wrapper: Key_IsDown returns qboolean; slot expects int
	import.CL_Key_IsDown              = Key_IsDown_int;
	// [110] Key_GetCatcher
	import.Key_GetCatcher             = Key_GetCatcher;
	// [111] Key_SetCatcher
	import.Key_SetCatcher             = Key_SetCatcher;
	// [112] SE_DisplayString — wrap to resolve SE_GetString overload
	import.SE_DisplayString           = SE_GetString_wrapper;
	// [113] SE_GetString
	import.SE_GetString               = SE_GetString_wrapper;
	// [114] SE_GetStringIndex
	import.SE_GetStringIndex          = CG_SE_GetStringIndex;
	// [115] CL_SetLastBoneIndex
	import.CL_SetLastBoneIndex        = CG_CL_SetLastBoneIndex;

	// [116] G2_InitWraithSurfaceMap (was misidentified as BoneMapSingleton)
	import.G2_InitWraithBoneMapSingleton = (void (*)(void))CG_G2_InitWraithSurfaceMap;
	// [117] G2API_HaveWeGhoul2Models
	import.G2API_HaveWeGhoul2Models   = CG_G2API_HaveWeGhoul2Models;
	// [118] G2_GetGhoul2InfoByHandle
	import.G2_GetGhoul2InfoByHandle   = CG_G2_GetGhoul2InfoByHandle;
	// [119] G2API_SetGhoul2ModelIndexes
	import.G2API_SetGhoul2ModelIndexes = CG_G2API_SetGhoul2ModelIndexes;
	// [120] G2API_ReRegisterModels
	import.G2API_ReRegisterModels     = CG_G2API_ReRegisterModels;
	// [121] G2API_GetBoltMatrix
	import.G2API_GetBoltMatrix        = CG_G2API_GetBoltMatrix;
	// [122] G2API_SetBoneAnglesOffset
	import.G2API_SetBoneAnglesOffset  = CG_G2API_SetBoneAnglesOffset;
	// [123] G2API_CleanGhoul2Models
	import.G2API_CleanGhoul2Models    = (void (*)(CGhoul2Info_v &))re.G2API_CleanGhoul2Models;

	// [124] RE_MarkFragments — SOF2 signature incompatible with OpenJK; leave NULL for now
	// TODO: implement proper MarkFragments wrapper in Phase 2
	// [125] CL_CM_SelectSubBSP — stub
	// [126] R_GetLighting
	import.R_GetLighting              = R_GetLighting_SOF2;

	// [127] R_BeginRegistration
	import.R_BeginRegistration        = re.BeginRegistration;
	// [128] R_RegisterModel
	import.R_RegisterModel            = re.RegisterModel;
	// [129] R_RegisterShader
	import.R_RegisterShader           = re.RegisterShader;
	// [130] R_RegisterShaderNoMip
	import.R_RegisterShaderNoMip      = re.RegisterShaderNoMip;
	// [131] R_RegisterSkin
	import.R_RegisterSkin             = re.RegisterSkin;

	// [132] R_ClearScene
	import.R_ClearScene               = re.ClearScene;
	// [133] R_AddRefEntityToScene
	import.R_AddRefEntityToScene      = re.AddRefEntityToScene;
	// [134] R_AddMiniRefEntityToScene
	import.R_AddMiniRefEntityToScene  = CG_R_AddMiniRefEntityToScene;
	// [135] R_AddPolyToScene (SOF2 has extra 'num' param ignored)
	import.R_AddPolyToScene           = R_AddPolyToScene_SOF2;
	// [136] R_AddLightToScene
	import.R_AddLightToScene          = re.AddLightToScene;
	// [137] R_AddDirectedLightToScene
	import.R_AddDirectedLightToScene  = CG_R_AddDirectedLightToScene;
	// [138] R_AddAdditiveLightToScene
	import.R_AddAdditiveLightToScene  = CG_R_AddAdditiveLightToScene;
	// [139] R_RenderScene
	import.R_RenderScene              = re.RenderScene;
	// [140] R_SetColor
	import.R_SetColor                 = re.SetColor;
	// [141] R_DrawStretchPic
	import.R_DrawStretchPic           = re.DrawStretchPic;
	// [142] R_FillRect
	import.R_FillRect                 = CG_R_FillRect;
	// [143] R_LerpTag (OpenJK returns void; SOF2 wants int)
	import.R_LerpTag                  = R_LerpTag_SOF2;
	// [144] R_ModelBounds
	import.R_ModelBounds              = re.ModelBounds;
	// [145] R_WorldEffectCommand
	import.R_WorldEffectCommand       = re.WorldEffectCommand;
	// [146] R_GetModelBounds — stub

	// [147] R_RegisterFont (SOF2 extra param)
	import.R_RegisterFont             = CG_R_RegisterFont;
	// [148] R_DamageSurface — stub (SOF2-specific gore)
	// [149] R_Font_StrLenPixels
	import.R_Font_StrLenPixels        = CG_R_Font_StrLenPixels;
	// [150] R_Font_HeightPixels
	import.R_Font_HeightPixels        = CG_R_Font_HeightPixels;
	// [151] R_Font_DrawString
	import.R_Font_DrawString          = CG_R_Font_DrawString;
	// [152] R_GetLightStyle
	import.R_GetLightStyle            = CG_R_GetLightStyle;
	// [153] R_SetLightStyle
	import.R_SetLightStyle            = CG_R_SetLightStyle;
	// [154-162] Extended renderer stubs

	// -----------------------------------------------------------------------
	// Fill remaining NULL slots with a per-slot sentinel so we crash with
	// a useful log message instead of jumping to EIP=0x00000000.
	// -----------------------------------------------------------------------
	{
		void **slots = (void **)&import;
		int nslots = sizeof(import) / sizeof(void *);
		for ( int i = 0; i < nslots; i++ ) {
			if ( slots[i] == NULL ) {
				Com_Printf( "CL_InitSOF2CGame: slot %d is NULL — filling sentinel\n", i );
				slots[i] = (void *)CG_NullSlotSentinel;
			}
		}
	}

	// -----------------------------------------------------------------------
	// Raw slot-index overrides — correct import table layout mismatches.
	// The cgame_import_t struct field order doesn't match SOF2's actual DLL
	// expectations for slots 78-82 and 125-149. Verified via Ghidra xref
	// analysis of cgamex86.dll (import table base 0x301f5958, 163 dwords).
	//
	// Key: "← verified" = confirmed by Ghidra xref decompilation
	//      "← tentative" = inferred from pattern/position
	// -----------------------------------------------------------------------
	{
		void **slots = (void **)&import;

		// --- Slot 41: CM_GetTerrainBounds ---
		// Called from CG_RegisterGraphics after terrain loading with 2 vec3_t* args.
		slots[41] = (void *)CG_CM_GetTerrainBounds;            // [41] CM_GetTerrainBounds ← verified

		// --- Slot 64: R_ClearScene (called from CG_Init end, no args) ---
		// The struct has CL_UI_RealTime at slot 64, but the DLL calls this
		// at the end of CG_Init after CG_ClearLightStyles() — it's R_ClearScene.
		// Verified via Ghidra xref: only caller is CG_Init at 0x3002896f.
		slots[64] = (void *)re.ClearScene;                   // [64] R_ClearScene ← verified

		// --- Slot 65: CM_PointContents ---
		// The struct has CL_UI_SnapVector at slot 65 (extra METIS stub that
		// doesn't exist in SOF2's DLL). The DLL expects CM_PointContents here.
		// Verified via 4 Ghidra xrefs: CG_PointContents, CG_ScopeTrace,
		// CG_Entity_EmitFireParticle (×2). Called every frame.
		slots[65] = (void *)CM_PointContents;                // [65] CM_PointContents ← verified CRITICAL FIX

		// --- METIS UI stubs 59, 61, 62 ---
		// These are SOF2-specific UI helper imports for menu/loading screen management.
		// Verified via Ghidra xref decompilation of CG_LoadInventoryMenus, CG_SetLoadingLevelshot.
		slots[59] = (void *)CG_UI_LoadMenuData;              // [59] UI_LoadMenuData ← verified
		slots[61] = (void *)CG_UI_MenuReset;                 // [61] UI_MenuReset ← verified
		slots[62] = (void *)CG_UI_SetInfoScreenText;         // [62] UI_SetInfoScreenText ← verified

		// --- GenericParser2 slots 105, 106, 108 ---
		// SOF2 cgame uses GP2 for weapon definitions and HUD widget config.
		// Verified via Ghidra: CG_PlayerEntity_SetWeapon, CG_HudWidget_QueryCallback.
		slots[105] = (void *)CG_GP_GetBaseParseGroup;         // [105] GP_GetBaseParseGroup ← verified
		slots[106] = (void *)CG_GP_GetSubGroups;              // [106] GP_GetSubGroups ← verified
		slots[108] = (void *)CG_GP_GetNext;                   // [108] GP_GetNext ← verified

		// --- Slot 116: G2_InitWraithSurfaceMap ---
		// CG_InitWraith calls this with (void**) expecting qboolean.
		// Must return CWraithStub* — the DLL calls through its 78-entry vtable.
		slots[116] = (void *)CG_G2_InitWraithSurfaceMap;       // [116] G2_InitWraithSurfaceMap ← verified CRASH FIX

		// --- Sound slots 78-82 ---
		// SOF2 inserts S_AddRealLoopingSound at [78], shifting the block.
		// All verified HIGH confidence via Ghidra xref analysis.
		slots[78] = (void *)S_AddLoopingSound_SOF2;         // [78] S_AddRealLoopingSound ← verified
		slots[79] = (void *)CG_S_StopLoopingSound;          // [79] S_StopLoopingSound ← verified
		slots[80] = (void *)S_UpdateEntityPosition;          // [80] S_UpdateEntityPosition ← verified
		slots[81] = (void *)S_Respatialize;                  // [81] S_Respatialize ← verified
		slots[82] = (void *)S_RegisterSound_SOF2;            // [82] S_RegisterSound ← verified

		// --- Renderer registration 125-129 ---
		// SOF2 has R_LoadWorldMap at [125]; struct had CL_CM_SelectSubBSP.
		slots[125] = (void *)re.LoadWorld;                   // [125] R_LoadWorldMap ← verified
		slots[126] = (void *)re.RegisterModel;               // [126] R_RegisterModel ← verified
		slots[127] = (void *)re.RegisterSkin;                // [127] R_RegisterSkin ← verified
		slots[128] = (void *)re.RegisterShader;              // [128] R_RegisterShader ← verified
		slots[129] = (void *)re.RegisterShaderNoMip;         // [129] R_RegisterShaderNoMip ← tentative

		// --- Renderer scene 130-138 ---
		slots[130] = (void *)re.ClearScene;                  // [130] R_ClearScene ← verified (was wrongly BeginRegistration!)
		slots[131] = (void *)re.AddRefEntityToScene;         // [131] R_AddRefEntityToScene ← tentative
		slots[132] = (void *)CG_FX_SetParentEntity;          // [132] FX_SetParentEntity(int entityNum) ← verified
		slots[133] = (void *)R_AddPolyToScene_SOF2;          // [133] R_AddPolyToScene ← verified
		slots[134] = (void *)CG_NullSlotSentinel;            // [134] unused (no xrefs) ← verified
		slots[135] = (void *)re.AddLightToScene;             // [135] R_AddLightToScene ← tentative
		slots[136] = (void *)CG_R_AddDirectedLightToScene;   // [136] R_AddDirectedLightToScene ← tentative
		slots[137] = (void *)CG_R_AddAdditiveLightToScene;   // [137] R_AddAdditiveLightToScene ← verified
		slots[138] = (void *)re.RenderScene;                 // [138] R_RenderScene ← verified

		// --- Renderer drawing 139-142 ---
		slots[139] = (void *)re.SetColor;                    // [139] R_SetColor ← verified
		slots[140] = (void *)re.DrawStretchPic;              // [140] R_DrawStretchPic ← tentative
		slots[141] = (void *)CG_R_FillRect;                  // [141] R_FillRect or DrawRotatePic ← tentative
		slots[142] = (void *)CG_NullSlotSentinel;            // [142] SOF2-specific (SetRangeFog-like, 3 args)

		// --- Tags, effects, bounds 143-149 ---
		slots[143] = (void *)R_LerpTag_SOF2;                 // [143] R_LerpTag ← tentative
		slots[144] = (void *)CG_R_RemapShader;               // [144] R_RemapShader ← verified (was wrongly S_StartBGTrack!)
		slots[145] = (void *)CG_NullSlotSentinel;            // [145] R_RegisterFont (no xrefs, unused)
		slots[146] = (void *)CG_R_SetLightStyle;             // [146] R_SetLightStyle ← verified
		slots[147] = (void *)re.ModelBounds;                 // [147] R_ModelBounds ← verified CRASH FIX!
		slots[148] = (void *)CG_RE_DamageSurface;             // [148] RE_DamageSurface ← verified (CG_ClientModel)
		slots[149] = (void *)CG_NullSlotSentinel;            // [149] R_DamageSurface (SOF2-specific gore, stub)

		// --- Entity token, weather, FF slots 150-162 ---
		// Verified via Ghidra xref analysis of cgamex86.dll.
		slots[150] = (void *)CG_GetEntityToken;              // [150] GetEntityToken ← verified (CG_ParseSpawnVars)
		// slots[151]: unknown, single xref from undefined code — leave sentinel
		slots[152] = (void *)re.WorldEffectCommand;          // [152] RE_WorldEffectCommand ← verified (weather)
		slots[153] = (void *)CG_R_IsOutside;                 // [153] R_IsOutside ← verified (rain check)
		slots[154] = (void *)CG_R_GetWindVector;             // [154] R_GetWindVector ← verified (particle wind)
		// slots[155-156]: weather stubs (undefined callers) — leave sentinel
		slots[157] = (void *)CG_FF_RegisterEffect;           // [157] FF_RegisterEffect ← verified HOT LOOP FIX
		slots[158] = (void *)CG_FF_Play;                     // [158] FF_Play ← verified (pickup effects)
		// slots[159-160]: FF stubs (unused) — leave sentinel
		slots[161] = (void *)CG_FF_StopAll;                  // [161] FF_StopAll / SetPlayerSuit ← tentative
		// slots[162]: FF_Shake or ICARUS_GetString (unused) — leave sentinel
	}

	// Reset client entity parse pointer for new map
	cl_entityParsePoint = NULL;

	// -----------------------------------------------------------------------
	cge = GetCGameAPI( CGAME_API_VERSION, &import );
	if ( !cge ) {
		Sys_UnloadDll( cgameLibrary );
		cgameLibrary = NULL;
		Com_Printf( "CL_InitSOF2CGame: GetCGameAPI returned NULL\n" );
		return qfalse;
	}

	// -----------------------------------------------------------------------
	// Patch CGlassMgr_ResolveConstraints to return immediately (RET).
	// The cg_glassInstance object at RVA 0x1C6828 has fields +0x0C/+0x10
	// that are NULL until CG_InitGlassManager() runs late in CG_Init().
	// But CGlassMgr_ResolveConstraints is called earlier via vtable[14]
	// during glass physics setup, dereferencing the NULL pointers.
	// Data patching doesn't work because CG_Glass_Init (lazy constructor)
	// runs during CG_Init and re-zeroes the fields after our patch.
	// Instead, we NOP the function itself. Glass physics won't resolve
	// constraints, but the map loads. RVA 0x16BB0 = CGlassMgr_ResolveConstraints.
	// -----------------------------------------------------------------------
	{
		unsigned char *func = (unsigned char *)cgameLibrary + 0x16BB0;
		DWORD oldProtect;
		if ( VirtualProtect( func, 1, PAGE_EXECUTE_READWRITE, &oldProtect ) ) {
			func[0] = 0xC3; // RET — skip all glass constraint code
			VirtualProtect( func, 1, oldProtect, &oldProtect );
			Com_Printf( "CL_InitSOF2CGame: patched CGlassMgr_ResolveConstraints at %p to RET\n", func );
		}
	}

	return qtrue;
}

/*
====================
CL_GetGameState
====================
*/
void CL_GetGameState( gameState_t *gs ) {
	*gs = cl.gameState;
}

/*
====================
CL_GetGlconfig
====================
*/
void CL_GetGlconfig( glconfig_t *glconfig ) {
	*glconfig = cls.glconfig;
}


/*
====================
CL_GetUserCmd
====================
*/
qboolean CL_GetUserCmd( int cmdNumber, usercmd_t *ucmd ) {
	// cmds[cmdNumber] is the last properly generated command

	// can't return anything that we haven't created yet
	if ( cmdNumber > cl.cmdNumber ) {
		Com_Error( ERR_DROP, "CL_GetUserCmd: %i >= %i", cmdNumber, cl.cmdNumber );
	}

	// the usercmd has been overwritten in the wrapping
	// buffer because it is too far out of date
	if ( cmdNumber <= cl.cmdNumber - CMD_BACKUP ) {
		return qfalse;
	}

	*ucmd = cl.cmds[ cmdNumber & CMD_MASK ];

	return qtrue;
}

int CL_GetCurrentCmdNumber( void ) {
	return cl.cmdNumber;
}


/*
====================
CL_GetParseEntityState
====================
*/
/*
qboolean	CL_GetParseEntityState( int parseEntityNumber, entityState_t *state ) {
	// can't return anything that hasn't been parsed yet
	if ( parseEntityNumber >= cl.parseEntitiesNum ) {
		Com_Error( ERR_DROP, "CL_GetParseEntityState: %i >= %i",
			parseEntityNumber, cl.parseEntitiesNum );
	}

	// can't return anything that has been overwritten in the circular buffer
	if ( parseEntityNumber <= cl.parseEntitiesNum - MAX_PARSE_ENTITIES ) {
		return qfalse;
	}

	*state = cl.parseEntities[ parseEntityNumber & ( MAX_PARSE_ENTITIES - 1 ) ];
	return qtrue;
}
*/

/*
====================
CL_GetCurrentSnapshotNumber
====================
*/
void	CL_GetCurrentSnapshotNumber( int *snapshotNumber, int *serverTime ) {
	*snapshotNumber = cl.frame.messageNum;
	*serverTime = cl.frame.serverTime;
}

/*
====================
CL_GetSnapshot
====================
*/
qboolean	CL_GetSnapshot( int snapshotNumber, snapshot_t *snapshot ) {
	clSnapshot_t	*clSnap;
	int				i, count;

	if ( snapshotNumber > cl.frame.messageNum ) {
		Com_Error( ERR_DROP, "CL_GetSnapshot: snapshotNumber > cl.frame.messageNum" );
	}

	// if the frame has fallen out of the circular buffer, we can't return it
	if ( cl.frame.messageNum - snapshotNumber >= PACKET_BACKUP ) {
		return qfalse;
	}

	// if the frame is not valid, we can't return it
	clSnap = &cl.frames[snapshotNumber & PACKET_MASK];
	if ( !clSnap->valid ) {
		return qfalse;
	}

	// if the entities in the frame have fallen out of their
	// circular buffer, we can't return it
	if ( cl.parseEntitiesNum - clSnap->parseEntitiesNum >= MAX_PARSE_ENTITIES ) {
		return qfalse;
	}

	// write the snapshot
	snapshot->snapFlags = clSnap->snapFlags;
	snapshot->serverCommandSequence = clSnap->serverCommandNum;
	snapshot->serverTime = clSnap->serverTime;
	memcpy( snapshot->areamask, clSnap->areamask, sizeof( snapshot->areamask ) );
	snapshot->cmdNum = clSnap->cmdNum;
	snapshot->ps = clSnap->ps;
	count = clSnap->numEntities;
	if ( count > MAX_ENTITIES_IN_SNAPSHOT ) {
		Com_DPrintf( "CL_GetSnapshot: truncated %i entities to %i\n", count, MAX_ENTITIES_IN_SNAPSHOT );
		count = MAX_ENTITIES_IN_SNAPSHOT;
	}
	snapshot->numEntities = count;
/*
Ghoul2 Insert Start
*/
 	for ( i = 0 ; i < count ; i++ )
	{

		int entNum =  ( clSnap->parseEntitiesNum + i ) & (MAX_PARSE_ENTITIES-1) ;
		snapshot->entities[i] = cl.parseEntities[ entNum ];
	}
/*
Ghoul2 Insert End
*/


	// FIXME: configstring changes and server commands!!!

	return qtrue;
}

//bg_public.h won't cooperate in here
#define EF_PERMANENT   0x00080000

qboolean CL_GetDefaultState(int index, entityState_t *state)
{
	if (index < 0 || index >= MAX_GENTITIES)
	{
		return qfalse;
	}

	// Is this safe? I think so. But it's still ugly as sin.
	if (!(sv.svEntities[index].baseline.eFlags & EF_PERMANENT))
//	if (!(cl.entityBaselines[index].eFlags & EF_PERMANENT))
	{
		return qfalse;
	}

	*state = sv.svEntities[index].baseline;
//	*state = cl.entityBaselines[index];

	return qtrue;
}

extern float cl_mPitchOverride;
extern float cl_mYawOverride;
void CL_SetUserCmdValue( int userCmdValue, float sensitivityScale, float mPitchOverride, float mYawOverride ) {
	cl.cgameUserCmdValue = userCmdValue;
	cl.cgameSensitivity = sensitivityScale;
	cl_mPitchOverride = mPitchOverride;
	cl_mYawOverride = mYawOverride;
}

extern vec3_t cl_overriddenAngles;
extern qboolean cl_overrideAngles;
void CL_SetUserCmdAngles( float pitchOverride, float yawOverride, float rollOverride ) {
	cl_overriddenAngles[PITCH] = pitchOverride;
	cl_overriddenAngles[YAW] = yawOverride;
	cl_overriddenAngles[ROLL] = rollOverride;
	cl_overrideAngles = qtrue;
}

void CL_AddCgameCommand( const char *cmdName ) {
	Cmd_AddCommand( cmdName, NULL );
}

/*
=====================
CL_ConfigstringModified
=====================
*/
void CL_ConfigstringModified( void ) {
	const char *s;
	char		*old;
	int			i, index;
	const char		*dup;
	gameState_t	oldGs;
	int			len;

	index = atoi( Cmd_Argv(1) );
	if ( index < 0 || index >= MAX_CONFIGSTRINGS ) {
		Com_Error( ERR_DROP, "configstring > MAX_CONFIGSTRINGS" );
	}
	s = Cmd_Argv(2);

	old = cl.gameState.stringData + cl.gameState.stringOffsets[ index ];
	if ( !strcmp( old, s ) ) {
		return;		// unchanged
	}

	// build the new gameState_t
	oldGs = cl.gameState;

	memset( &cl.gameState, 0, sizeof( cl.gameState ) );

	// leave the first 0 for uninitialized strings
	cl.gameState.dataCount = 1;

	for ( i = 0 ; i < MAX_CONFIGSTRINGS ; i++ ) {
		if ( i == index ) {
			dup = s;
		} else {
			dup = oldGs.stringData + oldGs.stringOffsets[ i ];
		}
		if ( !dup[0] ) {
			continue;		// leave with the default empty string
		}

		len = strlen( dup );

		if ( len + 1 + cl.gameState.dataCount > MAX_GAMESTATE_CHARS ) {
			Com_Error( ERR_DROP, "MAX_GAMESTATE_CHARS exceeded" );
		}

		// append it to the gameState string buffer
		cl.gameState.stringOffsets[ i ] = cl.gameState.dataCount;
		memcpy( cl.gameState.stringData + cl.gameState.dataCount, dup, len + 1 );
		cl.gameState.dataCount += len + 1;
	}

	if ( index == CS_SYSTEMINFO ) {
		// parse serverId and other cvars
		CL_SystemInfoChanged();
	}

}


/*
===================
CL_GetServerCommand

Set up argc/argv for the given command
===================
*/
qboolean CL_GetServerCommand( int serverCommandNumber ) {
	char	*s;
	const char	*cmd;

	// if we have irretrievably lost a reliable command, drop the connection
	if ( serverCommandNumber <= clc.serverCommandSequence - MAX_RELIABLE_COMMANDS ) {
		Com_Error( ERR_DROP, "CL_GetServerCommand: a reliable command was cycled out" );
		return qfalse;
	}

	if ( serverCommandNumber > clc.serverCommandSequence ) {
		Com_Error( ERR_DROP, "CL_GetServerCommand: requested a command not received" );
		return qfalse;
	}

	s = clc.serverCommands[ serverCommandNumber & ( MAX_RELIABLE_COMMANDS - 1 ) ];

	Com_DPrintf( "serverCommand: %i : %s\n", serverCommandNumber, s );

	Cmd_TokenizeString( s );
	cmd = Cmd_Argv(0);

	if ( !strcmp( cmd, "disconnect" ) ) {
		Com_Error (ERR_DISCONNECT,"Server disconnected\n");
	}

	if ( !strcmp( cmd, "cs" ) ) {
		CL_ConfigstringModified();
		// reparse the string, because CL_ConfigstringModified may have done another Cmd_TokenizeString()
		Cmd_TokenizeString( s );
		return qtrue;
	}

	// the clientLevelShot command is used during development
	// to generate 128*128 screenshots from the intermission
	// point of levels for the menu system to use
	// we pass it along to the cgame to make apropriate adjustments,
	// but we also clear the console and notify lines here
	if ( !strcmp( cmd, "clientLevelShot" ) ) {
		// don't do it if we aren't running the server locally,
		// otherwise malicious remote servers could overwrite
		// the existing thumbnails
		if ( !com_sv_running->integer ) {
			return qfalse;
		}
		// close the console
		Con_Close();
		// take a special screenshot next frame
		Cbuf_AddText( "wait ; wait ; wait ; wait ; screenshot levelshot\n" );
		return qtrue;
	}

	// we may want to put a "connect to other server" command here

	// cgame can now act on the command
	return qtrue;
}


/*
====================
CL_CM_LoadMap

Just adds default parameters that cgame doesn't need to know about
====================
*/
void CL_CM_LoadMap( const char *mapname, qboolean subBSP ) {
	int		checksum;

	CM_LoadMap( mapname, qtrue, &checksum, subBSP );
}

/*
====================
CL_ShutdownCGame
====================
*/
void CL_ShutdownCGame( void ) {
	cls.cgameStarted = qfalse;

	if ( cge ) {
		cge->Shutdown();
		cge = NULL;
	}
	if ( cgameLibrary ) {
		Sys_UnloadDll( cgameLibrary );
		cgameLibrary = NULL;
	}
}

#ifdef JK2_MODE
/*
====================
CL_ConvertJK2SysCall

Converts a JK2 syscall to a JKA syscall
====================
*/

cgameImport_t CL_ConvertJK2SysCall( cgameJK2Import_t import )
{
	// FIXME: This was a 5-minute slap-hack job in order to test if this really works. CLEAN ME UP! --eez
	switch(import)
	{
		case CG_PRINT_JK2:
			return CG_PRINT;
			break;
		case CG_ERROR_JK2:
			return CG_ERROR;
			break;
		case CG_MILLISECONDS_JK2:
			return CG_MILLISECONDS;
			break;
		case CG_CVAR_REGISTER_JK2:
			return CG_CVAR_REGISTER;
			break;
		case CG_CVAR_UPDATE_JK2:
			return CG_CVAR_UPDATE;
			break;
		case CG_CVAR_SET_JK2:
			return CG_CVAR_SET;
			break;
		case CG_ARGC_JK2:
			return CG_ARGC;
			break;
		case CG_ARGV_JK2:
			return CG_ARGV;
			break;
		case CG_ARGS_JK2:
			return CG_ARGS;
			break;
		case CG_FS_FOPENFILE_JK2:
			return CG_FS_FOPENFILE;
			break;
		case CG_FS_READ_JK2:
			return CG_FS_READ;
			break;
		case CG_FS_WRITE_JK2:
			return CG_FS_WRITE;
			break;
		case CG_FS_FCLOSEFILE_JK2:
			return CG_FS_FCLOSEFILE;
			break;
		case CG_SENDCONSOLECOMMAND_JK2:
			return CG_SENDCONSOLECOMMAND;
			break;
		case CG_ADDCOMMAND_JK2:
			return CG_ADDCOMMAND;
			break;
		case CG_SENDCLIENTCOMMAND_JK2:
			return CG_SENDCLIENTCOMMAND;
			break;
		case CG_UPDATESCREEN_JK2:
			return CG_UPDATESCREEN;
			break;
		case CG_CM_LOADMAP_JK2:
			return CG_CM_LOADMAP;
			break;
		case CG_CM_NUMINLINEMODELS_JK2:
			return CG_CM_NUMINLINEMODELS;
			break;
		case CG_CM_INLINEMODEL_JK2:
			return CG_CM_INLINEMODEL;
			break;
		case CG_CM_TEMPBOXMODEL_JK2:
			return CG_CM_TEMPBOXMODEL;
			break;
		case CG_CM_POINTCONTENTS_JK2:
			return CG_CM_POINTCONTENTS;
			break;
		case CG_CM_TRANSFORMEDPOINTCONTENTS_JK2:
			return CG_CM_TRANSFORMEDPOINTCONTENTS;
			break;
		case CG_CM_BOXTRACE_JK2:
			return CG_CM_BOXTRACE;
			break;
		case CG_CM_TRANSFORMEDBOXTRACE_JK2:
			return CG_CM_TRANSFORMEDBOXTRACE;
			break;
		case CG_CM_MARKFRAGMENTS_JK2:
			return CG_CM_MARKFRAGMENTS;
			break;
		case CG_CM_SNAPPVS_JK2:
			return CG_CM_SNAPPVS;
			break;
		case CG_S_STARTSOUND_JK2:
			return CG_S_STARTSOUND;
			break;
		case CG_S_STARTLOCALSOUND_JK2:
			return CG_S_STARTLOCALSOUND;
			break;
		case CG_S_CLEARLOOPINGSOUNDS_JK2:
			return CG_S_CLEARLOOPINGSOUNDS;
			break;
		case CG_S_ADDLOOPINGSOUND_JK2:
			return CG_S_ADDLOOPINGSOUND;
			break;
		case CG_S_UPDATEENTITYPOSITION_JK2:
			return CG_S_UPDATEENTITYPOSITION;
			break;
		case CG_S_RESPATIALIZE_JK2:
			return CG_S_RESPATIALIZE;
			break;
		case CG_S_REGISTERSOUND_JK2:
			return CG_S_REGISTERSOUND;
			break;
		case CG_S_STARTBACKGROUNDTRACK_JK2:
			return CG_S_STARTBACKGROUNDTRACK;
			break;
		case CG_R_LOADWORLDMAP_JK2:
			return CG_R_LOADWORLDMAP;
			break;
		case CG_R_REGISTERMODEL_JK2:
			return CG_R_REGISTERMODEL;
			break;
		case CG_R_REGISTERSKIN_JK2:
			return CG_R_REGISTERSKIN;
			break;
		case CG_R_REGISTERSHADER_JK2:
			return CG_R_REGISTERSHADER;
			break;
		case CG_R_REGISTERSHADERNOMIP_JK2:
			return CG_R_REGISTERSHADERNOMIP;
			break;
		case CG_R_REGISTERFONT_JK2:
			return CG_R_REGISTERFONT;
			break;
		case CG_R_FONTSTRLENPIXELS_JK2:
			return CG_R_FONTSTRLENPIXELS;
			break;
		case CG_R_FONTSTRLENCHARS_JK2:
			return CG_R_FONTSTRLENCHARS;
			break;
		case CG_R_FONTHEIGHTPIXELS_JK2:
			return CG_R_FONTHEIGHTPIXELS;
			break;
		case CG_R_FONTDRAWSTRING_JK2:
			return CG_R_FONTDRAWSTRING;
			break;
		case CG_LANGUAGE_ISASIAN_JK2:
			return CG_LANGUAGE_ISASIAN;
			break;
		case CG_LANGUAGE_USESSPACES_JK2:
			return CG_LANGUAGE_USESSPACES;
			break;
		case CG_ANYLANGUAGE_READFROMSTRING_JK2:
			return CG_ANYLANGUAGE_READFROMSTRING;
			break;
		case CG_R_CLEARSCENE_JK2:
			return CG_R_CLEARSCENE;
			break;
		case CG_R_ADDREFENTITYTOSCENE_JK2:
			return CG_R_ADDREFENTITYTOSCENE;
			break;
		case CG_R_GETLIGHTING_JK2:
			return CG_R_GETLIGHTING;
			break;
		case CG_R_ADDPOLYTOSCENE_JK2:
			return CG_R_ADDPOLYTOSCENE;
			break;
		case CG_R_ADDLIGHTTOSCENE_JK2:
			return CG_R_ADDLIGHTTOSCENE;
			break;
		case CG_R_RENDERSCENE_JK2:
			return CG_R_RENDERSCENE;
			break;
		case CG_R_SETCOLOR_JK2:
			return CG_R_SETCOLOR;
			break;
		case CG_R_DRAWSTRETCHPIC_JK2:
			return CG_R_DRAWSTRETCHPIC;
			break;
		case CG_R_DRAWSCREENSHOT_JK2:
			return CG_R_DRAWSCREENSHOT;
			break;
		case CG_R_MODELBOUNDS_JK2:
			return CG_R_MODELBOUNDS;
			break;
		case CG_R_LERPTAG_JK2:
			return CG_R_LERPTAG;
			break;
		case CG_R_DRAWROTATEPIC_JK2:
			return CG_R_DRAWROTATEPIC;
			break;
		case CG_R_DRAWROTATEPIC2_JK2:
			return CG_R_DRAWROTATEPIC2;
			break;
		case CG_R_LA_GOGGLES_JK2:
			return CG_R_LA_GOGGLES;
			break;
		case CG_R_SCISSOR_JK2:
			return CG_R_SCISSOR;
			break;
		case CG_GETGLCONFIG_JK2:
			return CG_GETGLCONFIG;
			break;
		case CG_GETGAMESTATE_JK2:
			return CG_GETGAMESTATE;
			break;
		case CG_GETCURRENTSNAPSHOTNUMBER_JK2:
			return CG_GETCURRENTSNAPSHOTNUMBER;
			break;
		case CG_GETSNAPSHOT_JK2:
			return CG_GETSNAPSHOT;
			break;
		case CG_GETSERVERCOMMAND_JK2:
			return CG_GETSERVERCOMMAND;
			break;
		case CG_GETCURRENTCMDNUMBER_JK2:
			return CG_GETCURRENTCMDNUMBER;
			break;
		case CG_GETUSERCMD_JK2:
			return CG_GETUSERCMD;
			break;
		case CG_SETUSERCMDVALUE_JK2:
			return CG_SETUSERCMDVALUE;
			break;
		case CG_SETUSERCMDANGLES_JK2:
			return CG_SETUSERCMDANGLES;
			break;
		case CG_S_UPDATEAMBIENTSET_JK2:
			return CG_S_UPDATEAMBIENTSET;
			break;
		case CG_S_ADDLOCALSET_JK2:
			return CG_S_ADDLOCALSET;
			break;
		case CG_AS_PARSESETS_JK2:
			return CG_AS_PARSESETS;
			break;
		case CG_AS_ADDENTRY_JK2:
			return CG_AS_ADDENTRY;
			break;
		case CG_AS_GETBMODELSOUND_JK2:
			return CG_AS_GETBMODELSOUND;
			break;
		case CG_S_GETSAMPLELENGTH_JK2:
			return CG_S_GETSAMPLELENGTH;
			break;
		case COM_SETORGANGLES_JK2:
			return COM_SETORGANGLES;
			break;
/*
Ghoul2 Insert Start
*/
		case CG_G2_LISTBONES_JK2:
			return CG_G2_LISTBONES;
			break;
		case CG_G2_LISTSURFACES_JK2:
			return CG_G2_LISTSURFACES;
			break;
		case CG_G2_HAVEWEGHOULMODELS_JK2:
			return CG_G2_HAVEWEGHOULMODELS;
			break;
		case CG_G2_SETMODELS_JK2:
			return CG_G2_SETMODELS;
			break;
/*
Ghoul2 Insert End
*/

		case CG_R_GET_LIGHT_STYLE_JK2:
			return CG_R_GET_LIGHT_STYLE;
			break;
		case CG_R_SET_LIGHT_STYLE_JK2:
			return CG_R_SET_LIGHT_STYLE;
			break;
		case CG_R_GET_BMODEL_VERTS_JK2:
			return CG_R_GET_BMODEL_VERTS;
			break;
		case CG_R_WORLD_EFFECT_COMMAND_JK2:
			return CG_R_WORLD_EFFECT_COMMAND;
			break;

		case CG_CIN_PLAYCINEMATIC_JK2:
			return CG_CIN_PLAYCINEMATIC;
			break;
		case CG_CIN_STOPCINEMATIC_JK2:
			return CG_CIN_STOPCINEMATIC;
			break;
		case CG_CIN_RUNCINEMATIC_JK2:
			return CG_CIN_RUNCINEMATIC;
			break;
		case CG_CIN_DRAWCINEMATIC_JK2:
			return CG_CIN_DRAWCINEMATIC;
			break;
		case CG_CIN_SETEXTENTS_JK2:
			return CG_CIN_SETEXTENTS;
			break;
		case CG_Z_MALLOC_JK2:
			return CG_Z_MALLOC;
			break;
		case CG_Z_FREE_JK2:
			return CG_Z_FREE;
			break;
		case CG_UI_MENU_RESET_JK2:
			return CG_UI_MENU_RESET;
			break;
		case CG_UI_MENU_NEW_JK2:
			return CG_UI_MENU_NEW;
			break;
		case CG_UI_PARSE_INT_JK2:
			return CG_UI_PARSE_INT;
			break;
		case CG_UI_PARSE_STRING_JK2:
			return CG_UI_PARSE_STRING;
			break;
		case CG_UI_PARSE_FLOAT_JK2:
			return CG_UI_PARSE_FLOAT;
			break;
		case CG_UI_STARTPARSESESSION_JK2:
			return CG_UI_STARTPARSESESSION;
			break;
		case CG_UI_ENDPARSESESSION_JK2:
			return CG_UI_ENDPARSESESSION;
			break;
		case CG_UI_PARSEEXT_JK2:
			return CG_UI_PARSEEXT;
			break;
		case CG_UI_MENUPAINT_ALL_JK2:
			return CG_UI_MENUPAINT_ALL;
			break;
		case CG_UI_STRING_INIT_JK2:
			return CG_UI_STRING_INIT;
			break;
		case CG_UI_GETMENUINFO_JK2:
			return CG_UI_GETMENUINFO;
			break;
		case CG_SP_REGISTER_JK2:
			return CG_SP_REGISTER;
			break;
		case CG_SP_GETSTRINGTEXT_JK2:
			return CG_SP_GETSTRINGTEXT;
			break;
		case CG_SP_GETSTRINGTEXTSTRING_JK2:
			// Both of these do the same thing --eez
			return CG_SP_GETSTRINGTEXTSTRING;
			break;
		case CG_UI_GETITEMTEXT_JK2:
			return CG_UI_GETITEMTEXT;
			break;
		case CG_ANYLANGUAGE_READFROMSTRING2_JK2:
			return CG_ANYLANGUAGE_READFROMSTRING2;
			break;
		case CG_OPENJK_MENU_PAINT_JK2:
			return CG_OPENJK_MENU_PAINT;
			break;
		case CG_OPENJK_GETMENU_BYNAME_JK2:
			return CG_OPENJK_GETMENU_BYNAME;
			break;
	}
	return (cgameImport_t)-1;
}

#endif
/*
====================
CL_CgameSystemCalls

The cgame module is making a system call
====================
*/
void CM_SnapPVS(vec3_t origin,byte *buffer);
extern void		Menu_Paint(menuDef_t *menu, qboolean forcePaint);
extern menuDef_t *Menus_FindByName(const char *p);
intptr_t CL_CgameSystemCalls( intptr_t *args ) {
#ifdef JK2_MODE
	args[0] = (intptr_t)CL_ConvertJK2SysCall((cgameJK2Import_t)args[0]);
#endif
	switch( args[0] ) {
	case CG_PRINT:
		Com_Printf( "%s", VMA(1) );
		return 0;
	case CG_ERROR:
		Com_Error( ERR_DROP, S_COLOR_RED"%s", VMA(1) );
		return 0;
	case CG_MILLISECONDS:
		return Sys_Milliseconds();
	case CG_CVAR_REGISTER:
		Cvar_Register( (vmCvar_t *) VMA(1), (const char *) VMA(2), (const char *) VMA(3), args[4] );
		return 0;
	case CG_CVAR_UPDATE:
		Cvar_Update( (vmCvar_t *) VMA(1) );
		return 0;
	case CG_CVAR_SET:
		Cvar_Set( (const char *) VMA(1), (const char *) VMA(2) );
		return 0;
	case CG_ARGC:
		return Cmd_Argc();
	case CG_ARGV:
		Cmd_ArgvBuffer( args[1], (char *) VMA(2), args[3] );
		return 0;
	case CG_ARGS:
		Cmd_ArgsBuffer( (char *) VMA(1), args[2] );
		return 0;
	case CG_FS_FOPENFILE:
		return FS_FOpenFileByMode( (const char *) VMA(1), (int *) VMA(2), (fsMode_t) args[3] );
	case CG_FS_READ:
		FS_Read( VMA(1), args[2], args[3] );
		return 0;
	case CG_FS_WRITE:
		FS_Write( VMA(1), args[2], args[3] );
		return 0;
	case CG_FS_FCLOSEFILE:
		FS_FCloseFile( args[1] );
		return 0;
	case CG_SENDCONSOLECOMMAND:
		Cbuf_AddText( (const char *) VMA(1) );
		return 0;
	case CG_ADDCOMMAND:
		CL_AddCgameCommand( (const char *) VMA(1) );
		return 0;
	case CG_SENDCLIENTCOMMAND:
		CL_AddReliableCommand( (const char *) VMA(1) );
		return 0;
	case CG_UPDATESCREEN:
		// this is used during lengthy level loading, so pump message loop
		Com_EventLoop();	// FIXME: if a server restarts here, BAD THINGS HAPPEN!
		SCR_UpdateScreen();
		return 0;
	case CG_RMG_INIT:
		return 0;
	case CG_CM_REGISTER_TERRAIN:
		return 0;

	case CG_RE_INIT_RENDERER_TERRAIN:
		return 0;

	case CG_CM_LOADMAP:
		CL_CM_LoadMap( (const char *) VMA(1), (qboolean)(args[2] != 0) );
		return 0;
	case CG_CM_NUMINLINEMODELS:
		return CM_NumInlineModels();
	case CG_CM_INLINEMODEL:
		return CM_InlineModel( args[1] );
	case CG_CM_TEMPBOXMODEL:
		return CM_TempBoxModel( (const float *) VMA(1), (const float *) VMA(2) );//, (int) VMA(3) );
	case CG_CM_POINTCONTENTS:
		return CM_PointContents( (float *)VMA(1), args[2] );
	case CG_CM_TRANSFORMEDPOINTCONTENTS:
		return CM_TransformedPointContents( (const float *) VMA(1), args[2], (const float *) VMA(3), (const float *) VMA(4) );
	case CG_CM_BOXTRACE:
		CM_BoxTrace( (trace_t *) VMA(1), (const float *) VMA(2), (const float *) VMA(3), (const float *) VMA(4), (const float *) VMA(5), args[6], args[7] );
		return 0;
	case CG_CM_TRANSFORMEDBOXTRACE:
		CM_TransformedBoxTrace( (trace_t *) VMA(1), (const float *) VMA(2), (const float *) VMA(3), (const float *) VMA(4), (const float *) VMA(5), args[6], args[7], (const float *) VMA(8), (const float *) VMA(9) );
		return 0;
	case CG_CM_MARKFRAGMENTS:
		return re.MarkFragments( args[1], (float(*)[3]) VMA(2), (const float *) VMA(3), args[4], (float *) VMA(5), args[6], (markFragment_t *) VMA(7) );
	case CG_CM_SNAPPVS:
		CM_SnapPVS((float(*))VMA(1),(byte *) VMA(2));
		return 0;
	case CG_S_STOPSOUNDS:
		S_StopSounds( );
		return 0;

	case CG_S_STARTSOUND:
		// stops an ERR_DROP internally if called illegally from game side, but note that it also gets here
		//	legally during level start where normally the internal s_soundStarted check would return. So ok to hit this.
		if (!cls.cgameStarted) {
			return 0;
		}
		S_StartSound( (float *) VMA(1), args[2], (soundChannel_t)args[3], args[4] );
		return 0;
	case CG_S_UPDATEAMBIENTSET:
		// stops an ERR_DROP internally if called illegally from game side, but note that it also gets here
		//	legally during level start where normally the internal s_soundStarted check would return. So ok to hit this.
		if (!cls.cgameStarted) {
			return 0;
		}
		S_UpdateAmbientSet( (const char *) VMA(1), (float *) VMA(2) );
		return 0;
	case CG_S_ADDLOCALSET:
		return S_AddLocalSet( (const char *) VMA(1), (float *) VMA(2), (float *) VMA(3), args[4], args[5] );
	case CG_AS_PARSESETS:
		AS_ParseSets();
		return 0;
	case CG_AS_ADDENTRY:
		AS_AddPrecacheEntry( (const char *) VMA(1) );
		return 0;
	case CG_AS_GETBMODELSOUND:
		return AS_GetBModelSound( (const char *) VMA(1), args[2] );
	case CG_S_STARTLOCALSOUND:
		// stops an ERR_DROP internally if called illegally from game side, but note that it also gets here
		//	legally during level start where normally the internal s_soundStarted check would return. So ok to hit this.
		if (!cls.cgameStarted) {
			return 0;
		}
		S_StartLocalSound( args[1], args[2] );
		return 0;
	case CG_S_CLEARLOOPINGSOUNDS:
		S_ClearLoopingSounds();
		return 0;
	case CG_S_ADDLOOPINGSOUND:
		// stops an ERR_DROP internally if called illegally from game side, but note that it also gets here
		//	legally during level start where normally the internal s_soundStarted check would return. So ok to hit this.
		if (!cls.cgameStarted) {
			return 0;
		}
		S_AddLoopingSound( args[1], (const float *) VMA(2), (const float *) VMA(3), args[4], (soundChannel_t)args[5] );
		return 0;
	case CG_S_UPDATEENTITYPOSITION:
		S_UpdateEntityPosition( args[1], (const float *) VMA(2) );
		return 0;
	case CG_S_RESPATIALIZE:
		S_Respatialize( args[1], (const float *) VMA(2), (float(*)[3]) VMA(3), (qboolean)(args[4] != 0) );
		return 0;
	case CG_S_REGISTERSOUND:
		return S_RegisterSound( (const char *) VMA(1) );
	case CG_S_STARTBACKGROUNDTRACK:
		S_StartBackgroundTrack( (const char *) VMA(1), (const char *) VMA(2), (qboolean)(args[3] != 0) );
		return 0;
	case CG_S_GETSAMPLELENGTH:
		return S_GetSampleLengthInMilliSeconds(  args[1]);
	case CG_R_LOADWORLDMAP:
		re.LoadWorld( (const char *) VMA(1) );
		return 0;
	case CG_R_REGISTERMODEL:
		return re.RegisterModel( (const char *) VMA(1) );
	case CG_R_REGISTERSKIN:
		return re.RegisterSkin( (const char *) VMA(1) );
	case CG_R_REGISTERSHADER:
		return re.RegisterShader( (const char *) VMA(1) );
	case CG_R_REGISTERSHADERNOMIP:
		return re.RegisterShaderNoMip( (const char *) VMA(1) );
	case CG_R_REGISTERFONT:
		return re.RegisterFont( (const char *) VMA(1) );
	case CG_R_FONTSTRLENPIXELS:
		return re.Font_StrLenPixels( (const char *) VMA(1), args[2], VMF(3) );
	case CG_R_FONTSTRLENCHARS:
		return re.Font_StrLenChars( (const char *) VMA(1) );
	case CG_R_FONTHEIGHTPIXELS:
		return re.Font_HeightPixels( args[1], VMF(2) );
	case CG_R_FONTDRAWSTRING:
		re.Font_DrawString(args[1],args[2], (const char *) VMA(3), (float*)args[4], args[5], args[6], VMF(7));
		return 0;
	case CG_LANGUAGE_ISASIAN:
		return re.Language_IsAsian();
	case CG_LANGUAGE_USESSPACES:
		return re.Language_UsesSpaces();
	case CG_ANYLANGUAGE_READFROMSTRING:
		return re.AnyLanguage_ReadCharFromString( (char *) VMA(1), (int *) VMA(2), (qboolean *) VMA(3) );
	case CG_ANYLANGUAGE_READFROMSTRING2:
		return re.AnyLanguage_ReadCharFromString2( (char **) VMA(1), (qboolean *) VMA(3) );
	case CG_R_SETREFRACTIONPROP:
		*(re.tr_distortionAlpha()) = VMF(1);
		*(re.tr_distortionStretch()) = VMF(2);
		*(re.tr_distortionPrePost()) = (qboolean)args[3];
		*(re.tr_distortionNegate()) = (qboolean)args[4];
		return 0;
	case CG_R_CLEARSCENE:
		re.ClearScene();
		return 0;
	case CG_R_ADDREFENTITYTOSCENE:
		re.AddRefEntityToScene( (const refEntity_t *) VMA(1) );
		return 0;

	case CG_R_INPVS:
		return re.R_inPVS((float *) VMA(1), (float *) VMA(2));

	case CG_R_GETLIGHTING:
		return re.GetLighting( (const float * ) VMA(1), (float *) VMA(2), (float *) VMA(3), (float *) VMA(4) );
	case CG_R_ADDPOLYTOSCENE:
		re.AddPolyToScene( args[1], args[2], (const polyVert_t *) VMA(3) );
		return 0;
	case CG_R_ADDLIGHTTOSCENE:
		re.AddLightToScene( (const float *) VMA(1), VMF(2), VMF(3), VMF(4), VMF(5) );
		return 0;
	case CG_R_RENDERSCENE:
		re.RenderScene( (const refdef_t *) VMA(1) );
		return 0;
	case CG_R_SETCOLOR:
		re.SetColor( (const float *) VMA(1) );
		return 0;
	case CG_R_DRAWSTRETCHPIC:
		re.DrawStretchPic( VMF(1), VMF(2), VMF(3), VMF(4), VMF(5), VMF(6), VMF(7), VMF(8), args[9] );
		return 0;
		// The below was commented out for whatever reason... /me shrugs --eez
	case CG_R_DRAWSCREENSHOT:
		re.DrawStretchRaw( VMF(1), VMF(2), VMF(3), VMF(4), SG_SCR_WIDTH, SG_SCR_HEIGHT, SCR_GetScreenshot(0), 0, qtrue);
		return 0;
	case CG_R_MODELBOUNDS:
		re.ModelBounds( args[1], (float *) VMA(2), (float *) VMA(3) );
		return 0;
	case CG_R_LERPTAG:
		re.LerpTag( (orientation_t *) VMA(1), args[2], args[3], args[4], VMF(5), (const char *) VMA(6) );
		return 0;
	case CG_R_DRAWROTATEPIC:
		re.DrawRotatePic( VMF(1), VMF(2), VMF(3), VMF(4), VMF(5), VMF(6), VMF(7), VMF(8), VMF(9), args[10] );
		return 0;
	case CG_R_DRAWROTATEPIC2:
		re.DrawRotatePic2( VMF(1), VMF(2), VMF(3), VMF(4), VMF(5), VMF(6), VMF(7), VMF(8), VMF(9), args[10] );
		return 0;
	case CG_R_SETRANGEFOG:
		re.SetRangedFog( VMF( 1 ) );
		return 0;
	case CG_R_LA_GOGGLES:
		re.LAGoggles();
		return 0;
	case CG_R_SCISSOR:
		re.Scissor( VMF(1), VMF(2), VMF(3), VMF(4));
		return 0;
	case CG_GETGLCONFIG:
		CL_GetGlconfig( (glconfig_t *) VMA(1) );
		return 0;
	case CG_GETGAMESTATE:
		CL_GetGameState( (gameState_t *) VMA(1) );
		return 0;
	case CG_GETCURRENTSNAPSHOTNUMBER:
		CL_GetCurrentSnapshotNumber( (int *) VMA(1), (int *) VMA(2) );
		return 0;
	case CG_GETSNAPSHOT:
		return CL_GetSnapshot( args[1], (snapshot_t *) VMA(2) );

	case CG_GETDEFAULTSTATE:
		return CL_GetDefaultState(args[1], (entityState_t *)VMA(2));

	case CG_GETSERVERCOMMAND:
		return CL_GetServerCommand( args[1] );
	case CG_GETCURRENTCMDNUMBER:
		return CL_GetCurrentCmdNumber();
	case CG_GETUSERCMD:
		return CL_GetUserCmd( args[1], (usercmd_s *) VMA(2) );
	case CG_SETUSERCMDVALUE:
		CL_SetUserCmdValue( args[1], VMF(2), VMF(3), VMF(4) );
		return 0;
	case CG_SETUSERCMDANGLES:
		CL_SetUserCmdAngles( VMF(1), VMF(2), VMF(3) );
		return 0;
	case COM_SETORGANGLES:
		Com_SetOrgAngles((float *)VMA(1),(float *)VMA(2));
		return 0;
/*
Ghoul2 Insert Start
*/

	case CG_G2_LISTSURFACES:
		re.G2API_ListSurfaces( (CGhoul2Info *) VMA(1) );
		return 0;

	case CG_G2_LISTBONES:
		re.G2API_ListBones( (CGhoul2Info *) VMA(1), args[2]);
		return 0;

	case CG_G2_HAVEWEGHOULMODELS:
		return re.G2API_HaveWeGhoul2Models( *((CGhoul2Info_v *)VMA(1)) );

	case CG_G2_SETMODELS:
		re.G2API_SetGhoul2ModelIndexes( *((CGhoul2Info_v *)VMA(1)),(qhandle_t *)VMA(2),(qhandle_t *)VMA(3));
		return 0;

/*
Ghoul2 Insert End
*/

	case CG_R_GET_LIGHT_STYLE:
		re.GetLightStyle(args[1], (byte*) VMA(2) );
		return 0;
	case CG_R_SET_LIGHT_STYLE:
		re.SetLightStyle(args[1], args[2] );
		return 0;

	case CG_R_GET_BMODEL_VERTS:
		re.GetBModelVerts( args[1], (float (*)[3])VMA(2), (float *)VMA(3) );
		return 0;

	case CG_R_WORLD_EFFECT_COMMAND:
		re.WorldEffectCommand( (const char *) VMA(1) );
		return 0;

	case CG_CIN_PLAYCINEMATIC:
	  return CIN_PlayCinematic( (const char *) VMA(1), args[2], args[3], args[4], args[5], args[6], (const char *) VMA(7));

	case CG_CIN_STOPCINEMATIC:
	  return CIN_StopCinematic(args[1]);

	case CG_CIN_RUNCINEMATIC:
	  return CIN_RunCinematic(args[1]);

	case CG_CIN_DRAWCINEMATIC:
	  CIN_DrawCinematic(args[1]);
	  return 0;

	case CG_CIN_SETEXTENTS:
	  CIN_SetExtents(args[1], args[2], args[3], args[4], args[5]);
	  return 0;

	case CG_Z_MALLOC:
		return (intptr_t)Z_Malloc(args[1], (memtag_t) args[2], qfalse);

	case CG_Z_FREE:
		Z_Free((void *) VMA(1));
		return 0;

	case CG_UI_SETACTIVE_MENU:
		UI_SetActiveMenu((const char *) VMA(1),NULL);
		return 0;

	case CG_UI_MENU_OPENBYNAME:
		Menus_OpenByName((const char *) VMA(1));
		return 0;

	case CG_UI_MENU_RESET:
		Menu_Reset();
		return 0;

	case CG_UI_MENU_NEW:
		Menu_New((char *) VMA(1));
		return 0;

	case CG_UI_PARSE_INT:
		PC_ParseInt((int *) VMA(1));
		return 0;

	case CG_UI_PARSE_STRING:
		PC_ParseString((const char **) VMA(1));
		return 0;

	case CG_UI_PARSE_FLOAT:
		PC_ParseFloat((float *) VMA(1));
		return 0;

	case CG_UI_STARTPARSESESSION:
		return(PC_StartParseSession((char *) VMA(1),(char **) VMA(2)));

	case CG_UI_ENDPARSESESSION:
		PC_EndParseSession((char *) VMA(1));
		return 0;

	case CG_UI_PARSEEXT:
		char **holdPtr;

		holdPtr = (char **) VMA(1);

		if(!holdPtr)
		{
			Com_Error(ERR_FATAL, "CG_UI_PARSEEXT: NULL holdPtr");
		}

		*holdPtr = PC_ParseExt();
		return 0;

	case CG_UI_MENUCLOSE_ALL:
		Menus_CloseAll();
		return 0;

	case CG_UI_MENUPAINT_ALL:
		Menu_PaintAll();
		return 0;

	case CG_OPENJK_MENU_PAINT:
		Menu_Paint( (menuDef_t *)VMA(1), (qboolean)(args[2] != 0) );
		return 0;

	case CG_OPENJK_GETMENU_BYNAME:
		return (intptr_t)Menus_FindByName( (const char *)VMA(1) );

	case CG_UI_STRING_INIT:
		String_Init();
		return 0;

	case CG_UI_GETMENUINFO:
		menuDef_t *menu;
		int		*xPos,*yPos,*w,*h,result;
#ifndef JK2_MODE
		menu = Menus_FindByName((char *) VMA(1));	// Get menu
		if (menu)
		{
			xPos = (int *) VMA(2);
			*xPos = (int) menu->window.rect.x;
			yPos = (int *) VMA(3);
			*yPos = (int) menu->window.rect.y;
			w = (int *) VMA(4);
			*w = (int) menu->window.rect.w;
			h = (int *) VMA(5);
			*h = (int) menu->window.rect.h;
			result = qtrue;
		}
		else
		{
			result = qfalse;
		}

		return result;
#else
		menu = Menus_FindByName((char *) VMA(1));	// Get menu
		if (menu)
		{
			xPos = (int *) VMA(2);
			*xPos = (int) menu->window.rect.x;
			yPos = (int *) VMA(3);
			*yPos = (int) menu->window.rect.y;
			result = qtrue;
		}
		else
		{
			result = qfalse;
		}

		return result;
#endif
		break;

	case CG_UI_GETITEMTEXT:
		itemDef_t *item;
		menu = Menus_FindByName((char *) VMA(1));	// Get menu

		if (menu)
		{
			item = (itemDef_s *) Menu_FindItemByName((menuDef_t *) menu, (char *) VMA(2));
			if (item)
			{
				Q_strncpyz( (char *) VMA(3), item->text, 256 );
				result = qtrue;
			}
			else
			{
				result = qfalse;
			}
		}
		else
		{
			result = qfalse;
		}

		return result;

	case CG_UI_GETITEMINFO:
		menu = Menus_FindByName((char *) VMA(1));	// Get menu

		if (menu)
		{
			qhandle_t *background;

			item = (itemDef_s *) Menu_FindItemByName((menuDef_t *) menu, (char *) VMA(2));
			if (item)
			{
				xPos = (int *) VMA(3);
				*xPos = (int) item->window.rect.x;
				yPos = (int *) VMA(4);
				*yPos = (int) item->window.rect.y;
				w = (int *) VMA(5);
				*w = (int) item->window.rect.w;
				h = (int *) VMA(6);
				*h = (int) item->window.rect.h;

				vec4_t *color;

				color = (vec4_t *) VMA(7);
				if (!color)
				{
					return qfalse;
				}

				(*color)[0] = (float) item->window.foreColor[0];
				(*color)[1] = (float) item->window.foreColor[1];
				(*color)[2] = (float) item->window.foreColor[2];
				(*color)[3] = (float) item->window.foreColor[3];
				background = (qhandle_t *) VMA(8);
				if (!background)
				{
					return qfalse;
				}
				*background = item->window.background;

				result = qtrue;
			}
			else
			{
				result = qfalse;
			}
		}
		else
		{
			result = qfalse;
		}

		return result;

#ifdef JK2_MODE
	case CG_SP_GETSTRINGTEXTSTRING:
	case CG_SP_GETSTRINGTEXT:
		const char* text;

		assert(VMA(1));
//		assert(VMA(2));	// can now pass in NULL to just query the size

		if (args[0] == CG_SP_GETSTRINGTEXT)
		{
			text = JK2SP_GetStringText( args[1] );
		}
		else
		{
			text = JK2SP_GetStringTextString( (const char *) VMA(1) );
		}

		if (VMA(2))	// only if dest buffer supplied...
		{
			if ( text[0] )
			{
				Q_strncpyz( (char *) VMA(2), text, args[3] );
			}
			else
			{
				Q_strncpyz( (char *) VMA(2), "??", args[3] );
			}
		}
		return strlen(text);

	case CG_SP_REGISTER:
		return JK2SP_Register((const char *)VMA(1), args[2] ? (SP_REGISTER_MENU | SP_REGISTER_REQUIRED) : SP_REGISTER_CLIENT);
#else
	case CG_SP_GETSTRINGTEXTSTRING:
		const char* text;

		assert(VMA(1));
		text = SE_GetString( (const char *) VMA(1) );

		if (VMA(2))	// only if dest buffer supplied...
		{
			if ( text[0] )
			{
				Q_strncpyz( (char *) VMA(2), text, args[3] );
			}
			else
			{
				Com_sprintf( (char *) VMA(2), args[3], "??%s", VMA(1) );
			}
		}
		return strlen(text);
#endif

	default:
		Com_Error( ERR_DROP, "Bad cgame system trap: %ld", (long int) args[0] );
	}
	return 0;
}


/*
====================
CL_InitCGame

Should only be called by CL_StartHunkUsers
====================
*/
extern qboolean Sys_LowPhysicalMemory();
void CL_InitCGame( void ) {
	const char			*info;
	const char			*mapname;
	//int		t1, t2;

	//t1 = Sys_Milliseconds();

	// put away the console
	Con_Close();

	// find the current mapname
	info = cl.gameState.stringData + cl.gameState.stringOffsets[ CS_SERVERINFO ];
	mapname = Info_ValueForKey( info, "mapname" );
	Com_sprintf( cl.mapname, sizeof( cl.mapname ), "maps/%s.bsp", mapname );

	cls.state = CA_LOADING;

	// SOF2: load cgamex86.dll and call GetCGameAPI to obtain the export table
	if ( !CL_InitSOF2CGame() ) {
		Com_Error( ERR_DROP, "Failed to initialize cgame DLL (cgamex86)" );
	}

	// init for this gamestate — SOF2 Init(serverMessageNum, serverCommandSequence, clientNum)
	cge->Init( cl.frame.messageNum, clc.serverCommandSequence, cl.frame.ps.clientNum );

	// reset any CVAR_CHEAT cvars registered by cgame
	if ( !cl_connectedToCheatServer )
		Cvar_SetCheatState();

	// we will send a usercmd this frame, which
	// will cause the server to send us the first snapshot
	cls.state = CA_PRIMED;

	//t2 = Sys_Milliseconds();

	//Com_Printf( "CL_InitCGame: %5.2f seconds\n", (t2-t1)/1000.0 );
	// have the renderer touch all its images, so they are present
	// on the card even if the driver does deferred loading
	re.EndRegistration();

	// make sure everything is paged in
//	if (!Sys_LowPhysicalMemory())
	{
		Com_TouchMemory();
	}

	// clear anything that got printed
	Con_ClearNotify ();
}


/*
====================
CL_GameCommand

See if the current console command is claimed by the cgame
====================
*/
qboolean CL_GameCommand( void ) {
	if ( cls.state != CA_ACTIVE ) {
		return qfalse;
	}

	return cge ? (qboolean)(cge->ConsoleCommand() != 0) : qfalse;
}



/*
=====================
CL_CGameRendering
=====================
*/
void CL_CGameRendering( stereoFrame_t stereo ) {
#if 0
	if ( cls.state == CA_ACTIVE ) {
		static int counter;

		if ( ++counter == 40 ) {
			VM_Debug( 2 );
		}
	}
#endif
	int timei=cl.serverTime;
	if (timei>60)
	{
		timei-=0;
	}
	re.G2API_SetTime(cl.serverTime,G2T_CG_TIME);
	if ( cge ) cge->DrawActiveFrame( timei, stereo, qfalse );
//	VM_Debug( 0 );
}


/*
=================
CL_AdjustTimeDelta

Adjust the clients view of server time.

We attempt to have cl.serverTime exactly equal the server's view
of time plus the timeNudge, but with variable latencies over
the internet it will often need to drift a bit to match conditions.

Our ideal time would be to have the adjusted time aproach, but not pass,
the very latest snapshot.

Adjustments are only made when a new snapshot arrives, which keeps the
adjustment process framerate independent and prevents massive overadjustment
during times of significant packet loss.
=================
*/

#define	RESET_TIME	300

void CL_AdjustTimeDelta( void ) {
/*
	cl.newSnapshots = qfalse;
	// if the current time is WAY off, just correct to the current value
	if ( cls.realtime + cl.serverTimeDelta < cl.frame.serverTime - RESET_TIME
		|| cls.realtime + cl.serverTimeDelta > cl.frame.serverTime + RESET_TIME  ) {
		cl.serverTimeDelta = cl.frame.serverTime - cls.realtime;
		cl.oldServerTime = cl.frame.serverTime;
		if ( cl_showTimeDelta->integer ) {
			Com_Printf( "<RESET> " );
		}
	}

	// if any of the frames between this and the previous snapshot
	// had to be extrapolated, nudge our sense of time back a little
	if ( cl.extrapolatedSnapshot ) {
		cl.extrapolatedSnapshot = qfalse;
		cl.serverTimeDelta -= 2;
	} else {
		// otherwise, move our sense of time forward to minimize total latency
		cl.serverTimeDelta++;
	}

	if ( cl_showTimeDelta->integer ) {
		Com_Printf( "%i ", cl.serverTimeDelta );
	}
*/
	int		newDelta;
	int		deltaDelta;

	cl.newSnapshots = qfalse;

	newDelta = cl.frame.serverTime - cls.realtime;
	deltaDelta = abs( newDelta - cl.serverTimeDelta );

	if ( deltaDelta > RESET_TIME ) {
		cl.serverTimeDelta = newDelta;
		cl.oldServerTime = cl.frame.serverTime;	// FIXME: is this a problem for cgame?
		cl.serverTime = cl.frame.serverTime;
		if ( cl_showTimeDelta->integer ) {
			Com_Printf( "<RESET> " );
		}
	} else if ( deltaDelta > 100 ) {
		// fast adjust, cut the difference in half
		if ( cl_showTimeDelta->integer ) {
			Com_Printf( "<FAST> " );
		}
		cl.serverTimeDelta = ( cl.serverTimeDelta + newDelta ) >> 1;
	} else {
		// slow drift adjust, only move 1 or 2 msec

		// if any of the frames between this and the previous snapshot
		// had to be extrapolated, nudge our sense of time back a little
		// the granularity of +1 / -2 is too high for timescale modified frametimes
		if ( com_timescale->value == 0 || com_timescale->value == 1 ) {
			if ( cl.extrapolatedSnapshot ) {
				cl.extrapolatedSnapshot = qfalse;
				cl.serverTimeDelta -= 2;
			} else {
				// otherwise, move our sense of time forward to minimize total latency
				cl.serverTimeDelta++;
			}
		}
	}

	if ( cl_showTimeDelta->integer ) {
		Com_Printf( "%i ", cl.serverTimeDelta );
	}
}


/*
==================
CL_FirstSnapshot
==================
*/
void CL_FirstSnapshot( void ) {

	re.RegisterMedia_LevelLoadEnd();

	cls.state = CA_ACTIVE;

	// set the timedelta so we are exactly on this first frame
	cl.serverTimeDelta = cl.frame.serverTime - cls.realtime;
	cl.oldServerTime = cl.frame.serverTime;

	// if this is the first frame of active play,
	// execute the contents of activeAction now
	// this is to allow scripting a timedemo to start right
	// after loading
	if ( cl_activeAction->string[0] ) {
		Cbuf_AddText( cl_activeAction->string );
		Cvar_Set( "activeAction", "" );
	}
}

/*
==================
CL_SetCGameTime
==================
*/
void CL_SetCGameTime( void ) {

	// getting a valid frame message ends the connection process
	if ( cls.state != CA_ACTIVE ) {
		if ( cls.state != CA_PRIMED ) {
			return;
		}
		if ( cl.newSnapshots ) {
			cl.newSnapshots = qfalse;
			CL_FirstSnapshot();
		}

		if ( cls.state != CA_ACTIVE ) {
			return;
		}
	}

	// if we have gotten to this point, cl.frame is guaranteed to be valid
	if ( !cl.frame.valid ) {
		Com_Error( ERR_DROP, "CL_SetCGameTime: !cl.snap.valid" );
	}

	// allow pause in single player
	if ( sv_paused->integer && CL_CheckPaused() && com_sv_running->integer ) {
		// paused
		return;
	}

	if ( cl.frame.serverTime < cl.oldFrameServerTime ) {
		Com_Error( ERR_DROP, "cl.frame.serverTime < cl.oldFrameServerTime" );
	}
	cl.oldFrameServerTime = cl.frame.serverTime;


	// get our current view of time

	// cl_timeNudge is a user adjustable cvar that allows more
	// or less latency to be added in the interest of better
	// smoothness or better responsiveness.
	cl.serverTime = cls.realtime + cl.serverTimeDelta - cl_timeNudge->integer;

	// guarantee that time will never flow backwards, even if
	// serverTimeDelta made an adjustment or cl_timeNudge was changed
	if ( cl.serverTime < cl.oldServerTime ) {
		cl.serverTime = cl.oldServerTime;
	}
	cl.oldServerTime = cl.serverTime;

	// note if we are almost past the latest frame (without timeNudge),
	// so we will try and adjust back a bit when the next snapshot arrives
	if ( cls.realtime + cl.serverTimeDelta >= cl.frame.serverTime - 5 ) {
		cl.extrapolatedSnapshot = qtrue;
	}

	// if we have gotten new snapshots, drift serverTimeDelta
	// don't do this every frame, or a period of packet loss would
	// make a huge adjustment
	if ( cl.newSnapshots ) {
		CL_AdjustTimeDelta();
	}
}


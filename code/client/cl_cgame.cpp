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
#include "client_ui.h"
#include "vmachine.h"
#include "qcommon/stringed_ingame.h"
#include "sys/sys_loadlib.h"
#if !defined(G2_H_INC)
#include "../ghoul2/G2.h"
#endif
#include "../codeJK2/cgame/cg_public.h"
#include <intrin.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#endif

vm_t	cgvm;

// SOF2: cgame is a separate struct-based DLL loaded via GetCGameAPI(), not vmMain dispatch.
static cgame_export_t *cge          = NULL;
static void           *cgameLibrary = NULL;
static void CG_FileTrace( const char *fmt, ... );
extern uiExport_t     *uie;

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

// SOF2 cgame actually calls this as Z_Malloc(size, tag, clear). Accept the
// extra args so allocator-backed grids are zero-initialized like the original.
static void *Z_Malloc_stub( int size, int tag, int clear ) {
	memtag_t memTag = ( tag > 0 ) ? (memtag_t)tag : TAG_ALL;
	return Z_Malloc( size, memTag, (qboolean)( clear != 0 ) );
}

#define SOF2_GLCONFIG_SIZE              0x898
#define SOF2_GLCONFIG_VIDWIDTH_OFFSET   0x87c
#define SOF2_GLCONFIG_VIDHEIGHT_OFFSET  0x880
#define SOF2_GLCONFIG_ASPECT_OFFSET     0x884
#define SOF2_GLCONFIG_DISPLAYFREQ_OFFSET 0x888
#define SOF2_GLCONFIG_FULLSCREEN_OFFSET 0x88c

static void CL_GetGlconfig_SOF2( void *glconfigRaw ) {
	byte *dst = (byte *)glconfigRaw;

	if ( !dst ) {
		return;
	}

	memset( dst, 0, SOF2_GLCONFIG_SIZE );
	*(int *)( dst + SOF2_GLCONFIG_VIDWIDTH_OFFSET ) = cls.glconfig.vidWidth;
	*(int *)( dst + SOF2_GLCONFIG_VIDHEIGHT_OFFSET ) = cls.glconfig.vidHeight;
	*(float *)( dst + SOF2_GLCONFIG_ASPECT_OFFSET ) =
		( cls.glconfig.vidHeight > 0 ) ? ( (float)cls.glconfig.vidWidth / (float)cls.glconfig.vidHeight ) : 1.0f;
	*(int *)( dst + SOF2_GLCONFIG_DISPLAYFREQ_OFFSET ) = cls.glconfig.displayFrequency;
	*(int *)( dst + SOF2_GLCONFIG_FULLSCREEN_OFFSET ) = cls.glconfig.isFullscreen ? 1 : 0;
}

// slot 67: CL_CM_LoadMap(name, clientLoad, checksum*) — SOF2 cgame signature
// Engine's CL_CM_LoadMap(name, subBSP) has different params; adapt here.
static void CL_CM_LoadMapWrapper( const char *mapname, int clientLoad, int *checksum ) {
	int localChecksum = 0;
	if ( !checksum ) {
		checksum = &localChecksum;
	}
	CM_LoadMap( mapname, (qboolean)(clientLoad != 0), checksum, qfalse );
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
	static int s_loopSoundLogCount = 0;
	if ( s_loopSoundLogCount < 32 ) {
		Com_Printf( "[SND loop] S_AddLoopingSound ent=%d sfx=%d origin=(%.1f,%.1f,%.1f)\n",
			entityNum, sfx,
			origin ? origin[0] : 0.0f,
			origin ? origin[1] : 0.0f,
			origin ? origin[2] : 0.0f );
		++s_loopSoundLogCount;
	}
	S_AddLoopingSound( entityNum, origin, velocity, sfx );
}

// slot 80: S_RegisterSound — SOF2 passes (name, compressed, streamed); OpenJK takes (name) only
static sfxHandle_t S_RegisterSoundSafe_SOF2( const char *name, const char *source ) {
	static int s_emptySoundDrops = 0;

	if ( !name || !name[0] ) {
		if ( s_emptySoundDrops < 16 ) {
			Com_Printf( "[SOF2 fix] dropped empty sound registration via %s (#%d)\n",
				source ? source : "unknown", s_emptySoundDrops + 1 );
		}
		++s_emptySoundDrops;
		return 0;
	}

	return S_RegisterSound( name );
}

static sfxHandle_t S_RegisterSound_SOF2( const char *name, int /*compressed*/, int /*streamed*/ ) {
	return S_RegisterSoundSafe_SOF2( name, "import" );
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

// SOF2 cgame exposes a cvar_t*-returning import and expects the original
// inline-name SOF2 layout rather than OpenJK's pointer-based cvar_t.
typedef struct sof2_cvar_s {
	char    name[64];
	void   *next;
	char   *string;
	char   *resetString;
	char   *latchedString;
	int     flags;
	int     modified;
	int     modificationCount;
	float   value;
	int     integer;
} sof2_cvar_t;

#define MAX_SOF2_CGAME_CVARS 256
static sof2_cvar_t s_sof2CgCvars[MAX_SOF2_CGAME_CVARS];
static cvar_t     *s_sof2CgCvarReal[MAX_SOF2_CGAME_CVARS];
static int         s_sof2CgCvarCount = 0;
static int         s_cgRenderSerial = 0;
static int         s_cgCvarUpdatesThisRender = 0;
static int         s_cgRenderDepth = 0;
static refEntity_t s_cgFrameRefEntities[MAX_REFENTITIES];
static int         s_cgFrameRefEntityCount = 0;
static refEntity_t s_cgLastRefEntities[MAX_REFENTITIES];
static int         s_cgLastRefEntityCount = 0;
static int         s_polyLogThisScene = 0;

// Flicker fix: track whether DrawInformation submitted a scene this frame.
// If it crashes before calling R_RenderScene, we submit a fallback scene
// so the screen never goes blank.
static qboolean    s_renderSceneSubmitted = qfalse;
static refdef_t    s_lastGoodRefdef;        // last successfully submitted refdef
static qboolean    s_lastGoodRefdefValid = qfalse;
static refdef_t    s_firstSubmittedRefdef;
static qboolean    s_firstSubmittedRefdefValid = qfalse;
static int         s_sceneSubmitsThisFrame = 0;

static void CL_LogPolyToScene( qhandle_t hShader, int numVerts, const polyVert_t *verts, const char *source ) {
	if ( cls.state != CA_ACTIVE || !verts || numVerts <= 0 ) {
		return;
	}
	if ( s_polyLogThisScene >= 6 ) {
		return;
	}

	float minX = verts[0].xyz[0], minY = verts[0].xyz[1], minZ = verts[0].xyz[2];
	float maxX = minX, maxY = minY, maxZ = minZ;
	for ( int i = 1; i < numVerts; ++i ) {
		minX = Q_min( minX, verts[i].xyz[0] );
		minY = Q_min( minY, verts[i].xyz[1] );
		minZ = Q_min( minZ, verts[i].xyz[2] );
		maxX = Q_max( maxX, verts[i].xyz[0] );
		maxY = Q_max( maxY, verts[i].xyz[1] );
		maxZ = Q_max( maxZ, verts[i].xyz[2] );
	}

	Com_Printf(
		"[POLY %s] #%d shader=%d verts=%d v0=(%.1f,%.1f,%.1f) bounds=[(%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)]\n",
		source,
		s_polyLogThisScene + 1,
		hShader,
		numVerts,
		verts[0].xyz[0], verts[0].xyz[1], verts[0].xyz[2],
		minX, minY, minZ, maxX, maxY, maxZ
	);
	++s_polyLogThisScene;
}

static void CG_SOF2_SyncCvar( int idx ) {
	cvar_t *real = s_sof2CgCvarReal[idx];

	s_sof2CgCvars[idx].string            = real->string;
	s_sof2CgCvars[idx].resetString       = real->resetString;
	s_sof2CgCvars[idx].latchedString     = real->latchedString;
	s_sof2CgCvars[idx].flags             = real->flags;
	s_sof2CgCvars[idx].modified          = real->modified;
	s_sof2CgCvars[idx].modificationCount = real->modificationCount;
	s_sof2CgCvars[idx].value             = real->value;
	s_sof2CgCvars[idx].integer           = real->integer;
}

static int CG_SOF2_FindCvarByName( const char *name ) {
	if ( !name ) {
		return -1;
	}

	for ( int i = 0; i < s_sof2CgCvarCount; i++ ) {
		if ( !Q_stricmp( s_sof2CgCvars[i].name, name ) ) {
			return i;
		}
	}

	return -1;
}

static void CG_SOF2_SyncAfterSet( const char *name ) {
	const int idx = CG_SOF2_FindCvarByName( name );

	if ( idx >= 0 ) {
		CG_SOF2_SyncCvar( idx );
	}
}

static cvar_t *CG_Cvar_GetModified_SOF2( const char *name, const char *val, int flags ) {
	cvar_t *real = Cvar_Get( name, val, flags );

	if ( !real ) {
		return NULL;
	}

	for ( int i = 0; i < s_sof2CgCvarCount; i++ ) {
		if ( s_sof2CgCvarReal[i] == real ) {
			CG_SOF2_SyncCvar( i );
			return (cvar_t *)&s_sof2CgCvars[i];
		}
	}

	if ( s_sof2CgCvarCount >= MAX_SOF2_CGAME_CVARS ) {
		Com_Error( ERR_FATAL, "CG_Cvar_GetModified_SOF2: too many cvars (%d)", MAX_SOF2_CGAME_CVARS );
		return NULL;
	}

	const int idx = s_sof2CgCvarCount++;
	s_sof2CgCvarReal[idx] = real;
	Q_strncpyz( s_sof2CgCvars[idx].name, real->name, sizeof( s_sof2CgCvars[idx].name ) );
	s_sof2CgCvars[idx].next = NULL;
	CG_SOF2_SyncCvar( idx );

	return (cvar_t *)&s_sof2CgCvars[idx];
}

static void CG_Cvar_Set_SOF2( const char *name, const char *value ) {
	if ( !name ) {
		return;
	}

	Cvar_Set( name, value );
	CG_SOF2_SyncAfterSet( name );
}

static void CG_Cvar_SetValue_SOF2( const char *name, float value ) {
	if ( !name ) {
		return;
	}

	Cvar_SetValue( name, value );
	CG_SOF2_SyncAfterSet( name );
}

static void CG_Cvar_Update_SOF2( vmCvar_t *vmCvar ) {
	if ( !vmCvar ) {
		return;
	}

	s_cgCvarUpdatesThisRender++;

#ifdef _WIN32
	__try {
		Cvar_Update( vmCvar );
	}
	__except ( EXCEPTION_EXECUTE_HANDLER ) {
		Com_DPrintf( "^1[CG CVAR] exception 0x%08x in Cvar_Update(vmCvar=%p)\n",
			GetExceptionCode(), vmCvar );
	}
#else
	Cvar_Update( vmCvar );
#endif
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
	static int logCount = 0;
	if ( logCount < 32 ) {
		Com_Printf( "[CG view] SetClientViewAngles #%d state=%d ang=(%.1f,%.1f,%.1f)\n",
			logCount + 1,
			cls.state,
			angles[0],
			angles[1],
			angles[2] );
		++logCount;
	}
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
	static int s_entityTokenLogCount = 0;
	static int s_entityTokenEntityIndex = -1;
	static qboolean s_entityTokenAwaitClassname = qfalse;
	static qboolean s_entityTokenAwaitModel = qfalse;

	if ( !cl_entityParsePoint ) {
		cl_entityParsePoint = CM_EntityString();
		s_entityTokenLogCount = 0;
		s_entityTokenEntityIndex = -1;
		s_entityTokenAwaitClassname = qfalse;
		s_entityTokenAwaitModel = qfalse;
	}
	const char *s = COM_Parse( &cl_entityParsePoint );
	Q_strncpyz( buffer, s, bufferSize );

	if ( s[0] == '{' ) {
		++s_entityTokenEntityIndex;
		s_entityTokenAwaitClassname = qfalse;
		s_entityTokenAwaitModel = qfalse;
		if ( s_entityTokenLogCount < 48 ) {
			Com_Printf( "[ENTITYSTR] begin #%d\n", s_entityTokenEntityIndex );
			++s_entityTokenLogCount;
		}
	} else if ( s_entityTokenAwaitClassname ) {
		if ( s_entityTokenLogCount < 48 ) {
			Com_Printf( "[ENTITYSTR] #%d classname='%s'\n", s_entityTokenEntityIndex, s );
			++s_entityTokenLogCount;
		}
		s_entityTokenAwaitClassname = qfalse;
	} else if ( s_entityTokenAwaitModel ) {
		if ( s_entityTokenLogCount < 48 ) {
			Com_Printf( "[ENTITYSTR] #%d model='%s'\n", s_entityTokenEntityIndex, s );
			++s_entityTokenLogCount;
		}
		s_entityTokenAwaitModel = qfalse;
	} else if ( !Q_stricmp( s, "classname" ) ) {
		s_entityTokenAwaitClassname = qtrue;
	} else if ( !Q_stricmp( s, "model" ) ) {
		s_entityTokenAwaitModel = qtrue;
	}

	if ( !cl_entityParsePoint && !s[0] ) {
		if ( s_entityTokenLogCount < 56 ) {
			Com_Printf( "[ENTITYSTR] end totalEntities=%d\n", s_entityTokenEntityIndex + 1 );
			++s_entityTokenLogCount;
		}
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
// The cgame DLL passes a pointer (may be NULL for entities without Ghoul2 models).
// At the ABI level, CGhoul2Info_v& and CGhoul2Info_v* are identical (both pointers).
static qboolean CG_G2API_HaveWeGhoul2Models( void *pGhoul2 ) {
	if ( !pGhoul2 ) return qfalse;
	return re.G2API_HaveWeGhoul2Models( *(CGhoul2Info_v *)pGhoul2 );
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

static void CG_R_ClearScene_Wrapper( void ) {
	static int clearSceneLogCount = 0;
	if ( clearSceneLogCount < 12 ) {
		Com_Printf( "[CG scene] ClearScene #%d depth=%d state=%d renderSerial=%d\n",
			clearSceneLogCount + 1, s_cgRenderDepth, (int)cls.state, s_cgRenderSerial );
		clearSceneLogCount++;
	}
	s_cgFrameRefEntityCount = 0;
	s_polyLogThisScene = 0;
	s_renderSceneSubmitted = qfalse;  // scene cleared — will need a new RenderScene call
	re.ClearScene();
}

static void CG_R_DrawStretchPic_Wrapper( float x, float y, float w, float h,
		unsigned int packedColor, qhandle_t hShader, int /*unused*/ ) {
	static int badShaderLogCount = 0;
	vec4_t rgba;
	qhandle_t resolvedShader = hShader;
	const float *resolvedColor = NULL;

	// SOF2 uses two observed forms here:
	// 1) (x,y,w,h, packedColor, shader, 0)
	// 2) (x,y,w,h, shader, whiteShader, 0)
	if ( hShader == cls.whiteShader && packedColor > 0 && packedColor <= 0xFFFFu ) {
		resolvedShader = (qhandle_t)packedColor;
		resolvedColor = NULL;
	}
	else {
		rgba[0] = ( ( packedColor >> 0 ) & 0xFF ) / 255.0f;
		rgba[1] = ( ( packedColor >> 8 ) & 0xFF ) / 255.0f;
		rgba[2] = ( ( packedColor >> 16 ) & 0xFF ) / 255.0f;
		rgba[3] = ( ( packedColor >> 24 ) & 0xFF ) / 255.0f;
		resolvedColor = rgba;
	}

	if ( resolvedShader <= 0 || (unsigned int)resolvedShader > 0xFFFFu ) {
		if ( badShaderLogCount < 16 ) {
			Com_Printf( "^3[CG] dropping SOF2 DrawStretchPic with invalid shader %d (0x%08X) arg5=0x%08X caller=%p\n",
				resolvedShader, (unsigned int)resolvedShader, packedColor, _ReturnAddress() );
			badShaderLogCount++;
		}
		return;
	}

	re.SetColor( resolvedColor );
	re.DrawStretchPic( x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, resolvedShader );
	re.SetColor( NULL );
}

static void CG_R_DrawStretchPicColor_Wrapper( float x, float y, float w, float h,
		float s1, float t1, float s2, float t2, const float *color, qhandle_t hShader ) {
	static int badShaderLogCount = 0;

	if ( hShader <= 0 || (unsigned int)hShader > 0xFFFFu ) {
		if ( badShaderLogCount < 16 ) {
			Com_Printf( "^3[CG] dropping colored stretch pic with invalid shader %d (0x%08X) color=%p caller=%p\n",
				hShader, (unsigned int)hShader, color, _ReturnAddress() );
			badShaderLogCount++;
		}
		return;
	}

	if ( color == (const float *)-1 ) {
		color = NULL;
	}

	re.SetColor( color );
	re.DrawStretchPic( x, y, w, h, s1, t1, s2, t2, hShader );
	re.SetColor( NULL );
}

static void CG_R_SetColor_SOF2( const void *colorOrPacked ) {
	uintptr_t raw = (uintptr_t)colorOrPacked;
	vec4_t rgba;

	if ( raw == (uintptr_t)-1 || raw == 0 ) {
		re.SetColor( NULL );
		return;
	}

	// SOF2 sometimes passes packed ARGB/RGBA-style immediates instead of a float*.
	if ( raw <= 0xFFFFFFFFu && ( raw < 0x01000000u || raw >= 0xF0000000u ) ) {
		rgba[0] = ( ( raw >> 0 ) & 0xFF ) / 255.0f;
		rgba[1] = ( ( raw >> 8 ) & 0xFF ) / 255.0f;
		rgba[2] = ( ( raw >> 16 ) & 0xFF ) / 255.0f;
		rgba[3] = ( ( raw >> 24 ) & 0xFF ) / 255.0f;
		re.SetColor( rgba );
		return;
	}

	re.SetColor( (const float *)colorOrPacked );
}

static qboolean CG_SOF2_RefEntitySeemsValid( const unsigned char *raw ) {
	if ( !raw ) return qfalse;
	const int sof2ReType = *(const int *)( raw + 0x00 );
	const int sof2hModel = *(const int *)( raw + 0x08 );
	const void *sof2Ghoul2 = *(void * const *)( raw + 0xCC );
	const float *sof2Origin = (const float *)( raw + 0x34 );
	return ( sof2ReType >= RT_MODEL &&
		sof2ReType <= RT_MAX_REF_ENTITY_TYPE &&
		( ( sof2hModel > 0 && sof2hModel < 32768 ) || sof2Ghoul2 != NULL ) &&
		( fabsf( sof2Origin[0] ) > 0.01f ||
		  fabsf( sof2Origin[1] ) > 0.01f ||
		  fabsf( sof2Origin[2] ) > 0.01f ||
		  sof2Ghoul2 != NULL ) ) ? qtrue : qfalse;
}

static void CG_ConvertSOF2RefEntity( const unsigned char *raw, refEntity_t *out ) {
	const int sof2ReType = *(const int *)( raw + 0x00 );
	const int sof2Renderfx = *(const int *)( raw + 0x04 );
	const float *sof2Axis = (const float *)( raw + 0x0C );
	const float *sof2Origin = (const float *)( raw + 0x34 );
	const float *sof2OldOrigin = (const float *)( raw + 0x40 );
	const float *sof2Angles = (const float *)( raw + 0x88 );
	const float *sof2Scale = (const float *)( raw + 0x6C );
	const int sof2hModel = *(const int *)( raw + 0x08 );
	const int sof2Frame = *(const int *)( raw + 0x68 );
	const void *sof2Ghoul2 = *(void * const *)( raw + 0xCC );

	memset( out, 0, sizeof( *out ) );
	out->reType = (refEntityType_t)sof2ReType;
	out->renderfx = sof2Renderfx;
	out->hModel = (qhandle_t)sof2hModel;
	VectorCopy( sof2Origin, out->origin );
	VectorCopy( sof2Origin, out->lightingOrigin );
	if ( fabsf( sof2OldOrigin[0] ) > 0.01f ||
		fabsf( sof2OldOrigin[1] ) > 0.01f ||
		fabsf( sof2OldOrigin[2] ) > 0.01f ) {
		VectorCopy( sof2OldOrigin, out->oldorigin );
	} else {
		VectorCopy( sof2Origin, out->oldorigin );
	}
	VectorCopy( sof2Axis + 0, out->axis[0] );
	VectorCopy( sof2Axis + 3, out->axis[1] );
	VectorCopy( sof2Axis + 6, out->axis[2] );
	VectorCopy( sof2Angles, out->angles );
	out->frame = sof2Frame;
	out->oldframe = sof2Frame;
	out->backlerp = 0.0f;
	out->ghoul2 = (CGhoul2Info_v *)sof2Ghoul2;
	out->shaderRGBA[0] = out->shaderRGBA[1] = out->shaderRGBA[2] = out->shaderRGBA[3] = 255;
	out->modelScale[0] = ( sof2Scale[0] != 0.0f ) ? sof2Scale[0] : 1.0f;
	out->modelScale[1] = ( sof2Scale[1] != 0.0f ) ? sof2Scale[1] : 1.0f;
	out->modelScale[2] = ( sof2Scale[2] != 0.0f ) ? sof2Scale[2] : 1.0f;
	if ( out->modelScale[0] != 1.0f ||
		out->modelScale[1] != 1.0f ||
		out->modelScale[2] != 1.0f ) {
		out->nonNormalizedAxes = qtrue;
	}
}

static void CG_R_AddRefEntityToScene_Wrapper( const refEntity_t *ent ) {
	static int addRefEntityLogCount = 0;
	const refEntity_t *submitEnt = ent;
	refEntity_t converted;
	if ( !ent ) {
		return;
	}

	const unsigned char *raw = (const unsigned char *)ent;
	const int sof2hModel = *(const int *)( raw + 0x08 );
	const float *sof2Origin = (const float *)( raw + 0x34 );
	const void *sof2Ghoul2 = *(void * const *)( raw + 0xCC );
	const qboolean useSOF2Layout = CG_SOF2_RefEntitySeemsValid( raw );

	if ( addRefEntityLogCount < 16 ) {
		Com_Printf(
			"[CG scene] AddRefEntity #%d mode=%s openjk{type=%d org=(%.1f,%.1f,%.1f) hModel=%d skin=%d fx=0x%x} sof2{type=%d org=(%.1f,%.1f,%.1f) hModel=%d scale=(%.2f,%.2f,%.2f) ghoul2=%p fx=0x%x}\n",
			addRefEntityLogCount + 1,
			useSOF2Layout ? "sof2" : "openjk",
			(int)ent->reType,
			ent->origin[0], ent->origin[1], ent->origin[2],
			(int)ent->hModel,
			(int)ent->customSkin,
			(unsigned int)ent->renderfx,
			*(const int *)( raw + 0x00 ),
			sof2Origin[0], sof2Origin[1], sof2Origin[2],
			sof2hModel,
			*(const float *)( raw + 0x6C ),
			*(const float *)( raw + 0x70 ),
			*(const float *)( raw + 0x74 ),
			sof2Ghoul2,
			*(const int *)( raw + 0x04 ) );
		addRefEntityLogCount++;
	}

	if ( useSOF2Layout ) {
		CG_ConvertSOF2RefEntity( raw, &converted );
		submitEnt = &converted;
	}

	if ( s_cgFrameRefEntityCount < MAX_REFENTITIES ) {
		s_cgFrameRefEntities[s_cgFrameRefEntityCount++] = *submitEnt;
	}

	re.AddRefEntityToScene( submitEnt );
}

static void CG_R_AddLightToScene_Wrapper( const vec3_t org, float intensity,
		float r, float g, float b ) {
	static int addLightLogCount = 0;
	if ( addLightLogCount < 12 ) {
		Com_Printf( "[CG scene] AddLight #%d org=(%.1f,%.1f,%.1f) intensity=%.1f rgb=(%.2f,%.2f,%.2f)\n",
			addLightLogCount + 1,
			org[0], org[1], org[2],
			intensity, r, g, b );
		addLightLogCount++;
	}
	re.AddLightToScene( org, intensity, r, g, b );
}

typedef struct {
	int x;
	int y;
	int width;
	int height;
	float fov_x;
	float fov_y;
	vec3_t vieworg;
	vec3_t viewaxis[3];
	int time;
	int rdflags;
	byte areamask[MAX_MAP_AREA_BYTES];
} sof2_refdef_t;

static qboolean CG_SameCameraSubmit( const refdef_t *a, const refdef_t *b ) {
	float dx, dy, dz;

	if ( !a || !b ) {
		return qfalse;
	}

	dx = a->vieworg[0] - b->vieworg[0];
	dy = a->vieworg[1] - b->vieworg[1];
	dz = a->vieworg[2] - b->vieworg[2];
	if ( ( dx * dx + dy * dy + dz * dz ) > 4.0f ) {
		return qfalse;
	}

	for ( int i = 0; i < 3; ++i ) {
		float dot = DotProduct( a->viewaxis[i], b->viewaxis[i] );
		if ( dot < 0.999f ) {
			return qfalse;
		}
	}

	return qtrue;
}

static void CG_SubmitSOF2Refdef( refdef_t *localRefdef ) {
	int replayedRefEntities = 0;

	if ( !localRefdef ) {
		CG_FileTrace( "CG_SubmitSOF2Refdef null refdef" );
		return;
	}

	// Force camera from player state while active so render follows movement state
	// even when retail draw paths flip between alternate view controllers.
	if ( cls.state == CA_ACTIVE ) {
		const int forcedViewHeight = ( cl.frame.ps.viewheight > 0 ) ? cl.frame.ps.viewheight : 38;
		vec3_t forcedAngles;
		for ( int i = 0; i < 3; ++i ) {
			forcedAngles[i] = SHORT2ANGLE(
				(short)( ANGLE2SHORT( cl.viewangles[i] ) + cl.frame.ps.delta_angles[i] ) );
		}
		VectorCopy( cl.frame.ps.origin, localRefdef->vieworg );
		localRefdef->vieworg[2] += forcedViewHeight;
		AnglesToAxis( forcedAngles, localRefdef->viewaxis );
		// Renderer treats 1 bits as hidden areas. Zero means "all areas visible".
		memset( localRefdef->areamask, 0x00, sizeof( localRefdef->areamask ) );
	}

	// In active play SOF2 can issue multiple RenderScene calls in one draw pass.
	// Compare submits after forcing the gameplay camera so equivalent passes are
	// not discarded just because retail provided a transient alternate controller.
	if ( s_renderSceneSubmitted && s_cgRenderDepth > 0 && cls.state == CA_ACTIVE ) {
		static int duplicateSubmitLogCount = 0;
		if ( s_firstSubmittedRefdefValid && !CG_SameCameraSubmit( &s_firstSubmittedRefdef, localRefdef ) ) {
			if ( duplicateSubmitLogCount < 12 ) {
				Com_Printf(
					"[SOF2 RS] dropped different-camera submit serial=%d time=%d rdflags=0x%x vieworg=(%.1f,%.1f,%.1f)\n",
					s_cgRenderSerial,
					localRefdef->time,
					localRefdef->rdflags,
					localRefdef->vieworg[0],
					localRefdef->vieworg[1],
					localRefdef->vieworg[2] );
				duplicateSubmitLogCount++;
			}
			return;
		}
		if ( duplicateSubmitLogCount < 12 ) {
			Com_Printf(
				"[SOF2 RS] allowing same-camera duplicate submit serial=%d time=%d rdflags=0x%x submits=%d\n",
				s_cgRenderSerial,
				localRefdef->time,
				localRefdef->rdflags,
				s_sceneSubmitsThisFrame + 1 );
			duplicateSubmitLogCount++;
		}
	}

	if ( cls.state == CA_ACTIVE &&
		!( localRefdef->rdflags & RDF_NOWORLDMODEL ) &&
		s_cgFrameRefEntityCount == 0 &&
		s_cgLastRefEntityCount > 0 ) {
		static int replayLogCount = 0;
		for ( int i = 0; i < s_cgLastRefEntityCount; ++i ) {
			re.AddRefEntityToScene( &s_cgLastRefEntities[i] );
		}
		replayedRefEntities = s_cgLastRefEntityCount;
		if ( replayLogCount < 16 ) {
			Com_Printf(
				"[SOF2 RS] replayed %d cached ref entities for empty scene serial=%d time=%d rdflags=0x%x\n",
				replayedRefEntities,
				s_cgRenderSerial,
				localRefdef->time,
				localRefdef->rdflags );
			++replayLogCount;
		}
	}

	__try {
		if ( !s_firstSubmittedRefdefValid ) {
			s_firstSubmittedRefdef = *localRefdef;
			s_firstSubmittedRefdefValid = qtrue;
		}
		re.RenderScene( localRefdef );
		// Scene was successfully submitted — save it as the fallback.
		s_renderSceneSubmitted = qtrue;
		s_sceneSubmitsThisFrame++;
		s_lastGoodRefdef = *localRefdef;
		s_lastGoodRefdefValid = qtrue;
		if ( s_cgFrameRefEntityCount > 0 ) {
			s_cgLastRefEntityCount = s_cgFrameRefEntityCount;
			memcpy( s_cgLastRefEntities, s_cgFrameRefEntities,
				sizeof( refEntity_t ) * s_cgFrameRefEntityCount );
		}
		else if ( replayedRefEntities > 0 ) {
			s_cgLastRefEntityCount = replayedRefEntities;
		}
		// Renderer scene buffers are consumed by RenderScene. Reset the client-side
		// ref-entity accumulation here so same-frame duplicate submits can detect
		// that no fresh entities were added and trigger cached replay.
		s_cgFrameRefEntityCount = 0;
	} __except ( EXCEPTION_EXECUTE_HANDLER ) {
		Com_Printf( "^1[SOF2 RS] exception 0x%08X in re.RenderScene\n", GetExceptionCode() );
		CG_FileTrace( "RS exception 0x%08X", GetExceptionCode() );
	}
}

static void CG_R_RenderScene_Wrapper( const sof2_refdef_t *sof2Refdef ) {
	refdef_t localRefdef;
	static int legacySlotLogCount = 0;

	if ( !sof2Refdef ) {
		CG_FileTrace( "CG_R_RenderScene_Wrapper null refdef" );
		return;
	}

	memset( &localRefdef, 0, sizeof( localRefdef ) );
	localRefdef.x = sof2Refdef->x;
	localRefdef.y = sof2Refdef->y;
	localRefdef.width = sof2Refdef->width;
	localRefdef.height = sof2Refdef->height;
	localRefdef.fov_x = sof2Refdef->fov_x;
	localRefdef.fov_y = sof2Refdef->fov_y;
	VectorCopy( sof2Refdef->vieworg, localRefdef.vieworg );
	VectorCopy( sof2Refdef->viewaxis[0], localRefdef.viewaxis[0] );
	VectorCopy( sof2Refdef->viewaxis[1], localRefdef.viewaxis[1] );
	VectorCopy( sof2Refdef->viewaxis[2], localRefdef.viewaxis[2] );
	localRefdef.viewContents = 0;
	localRefdef.time = sof2Refdef->time;
	localRefdef.rdflags = sof2Refdef->rdflags;
	memcpy( localRefdef.areamask, sof2Refdef->areamask, sizeof( localRefdef.areamask ) );
	if ( legacySlotLogCount < 8 ) {
		Com_Printf( "[SOF2 RS138] #%d legacy render path x=%d y=%d w=%d h=%d time=%d rdflags=0x%x\n",
			legacySlotLogCount + 1,
			localRefdef.x,
			localRefdef.y,
			localRefdef.width,
			localRefdef.height,
			localRefdef.time,
			localRefdef.rdflags );
		++legacySlotLogCount;
	}
	CG_SubmitSOF2Refdef( &localRefdef );
}

static void CG_R_RenderScene_SOF2Main( int viewEntityNum, const vec3_t vieworg,
	const vec3_t viewaxis[3], int viewContents ) {
	refdef_t localRefdef;
	const char *cgameBase = (const char *)cgameLibrary;
	const float *sof2FovX = cgameBase ? (const float *)( cgameBase + 0x1D51B0 ) : NULL;
	const float *sof2FovY = cgameBase ? (const float *)( cgameBase + 0x1D51B4 ) : NULL;
	const int *sof2Time = cgameBase ? (const int *)( cgameBase + 0x1D51E8 ) : NULL;
	const byte *sof2Areamask = cgameBase ? (const byte *)( cgameBase + 0x1D51F0 ) : NULL;
	(void)viewEntityNum;

	memset( &localRefdef, 0, sizeof( localRefdef ) );
	localRefdef.x = 0;
	localRefdef.y = 0;
	localRefdef.width = cls.glconfig.vidWidth > 0 ? cls.glconfig.vidWidth : 640;
	localRefdef.height = cls.glconfig.vidHeight > 0 ? cls.glconfig.vidHeight : 480;
	localRefdef.fov_x = ( sof2FovX && *sof2FovX > 1.0f && *sof2FovX < 179.0f ) ? *sof2FovX : 90.0f;
	localRefdef.fov_y = ( sof2FovY && *sof2FovY > 1.0f && *sof2FovY < 179.0f ) ? *sof2FovY : 75.0f;
	localRefdef.viewContents = viewContents;
	localRefdef.time = sof2Time ? *sof2Time : cl.serverTime;
	localRefdef.rdflags = 0;

	if ( vieworg ) {
		VectorCopy( vieworg, localRefdef.vieworg );
	}
	if ( viewaxis ) {
		VectorCopy( viewaxis[0], localRefdef.viewaxis[0] );
		VectorCopy( viewaxis[1], localRefdef.viewaxis[1] );
		VectorCopy( viewaxis[2], localRefdef.viewaxis[2] );
	}
	if ( sof2Areamask ) {
		memcpy( localRefdef.areamask, sof2Areamask, sizeof( localRefdef.areamask ) );
	}

	CG_SubmitSOF2Refdef( &localRefdef );
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
static void CG_UI_LoadMenuData( const char *menuData, const char *category ) {
	const char *value = menuData ? menuData : "";
	const char *element = category ? category : "";

	if ( uie && uie->UI_SetHudString && category && category[0] ) {
		uie->UI_SetHudString( (char *)element, (char *)value );
		return;
	}
}

// ----- Slot 61: UI_MenuReset() -----
// Resets UI menu system state before reloading menu definitions.
// Called from CG_LoadInventoryMenus as first step.
static void CG_UI_MenuReset( void ) {
}

// ----- Slot 62: UI_SetInfoScreenText(text) -----
// Sets the loading screen info text widget content.
// Called from CG_SetLoadingLevelshot. Cosmetic only.
static void CG_UI_SetInfoScreenText( const char * /*text*/ ) {
	// no-op — loading screen text widget not implemented
}

// ----- Slots 105-108: unresolved retail helpers -----
// Current retail cgame disassembly (base 0x30000000) shows:
//   [106] no-arg call, result stored at object+0x548
//   [105] 1-arg cdecl helper called alongside [106]
//   object+0x548 is then used as a virtual interface:
//      vtbl+0x01C, 0x03C, 0x050, 0x100, 0x10C, 0x128, 0x14C, 0x150, 0x15C
//
// Returning NULL from [106] suppresses those call paths entirely, which lines up
// with the missing HUD/weapon/query behavior seen in-game.
//
// We provide a conservative "null object" interface here: non-null pointer plus
// ABI-correct virtual stubs that return neutral values without corrupting stack.
// This keeps retail cgame control flow alive while we continue mapping semantics.
struct CG_SOF2_Import106Iface {
	void **vftable;
	int lastHandle;
	int reserved0;
	int reserved1;
};

static int __stdcall CG_SOF2_Import106_VCall_Arg1_Ret0( int /*a0*/ ) {
	return 0;
}

static int __stdcall CG_SOF2_Import106_VCall_Arg2_Ret0( int /*a0*/, int /*a1*/ ) {
	return 0;
}

static int __stdcall CG_SOF2_Import106_VCall_Arg4_Ret0( int /*a0*/, int /*a1*/, int /*a2*/, int /*a3*/ ) {
	return 0;
}

static float __stdcall CG_SOF2_Import106_VCall_Arg5_Ret0f( int /*a0*/, int /*a1*/, int /*a2*/, int /*a3*/, int /*a4*/ ) {
	return 0.0f;
}

static void __stdcall CG_SOF2_Import106_VCall_NoArgs( void ) {
}

static int __cdecl CG_SOF2_Import106_VCall_Arg1_Cdecl_Ret0( int /*a0*/ ) {
	return 0;
}

static void *s_sof2Import106Vftable[96];
static CG_SOF2_Import106Iface s_sof2Import106Iface;
static qboolean s_sof2Import106Init = qfalse;

static void CG_SOF2_InitImport106Iface( void ) {
	if ( s_sof2Import106Init ) {
		return;
	}

	for ( int i = 0; i < (int)ARRAY_LEN( s_sof2Import106Vftable ); ++i ) {
		s_sof2Import106Vftable[i] = (void *)CG_SOF2_Import106_VCall_NoArgs;
	}

	// Explicit virtuals confirmed by retail wrappers around object+0x548.
	s_sof2Import106Vftable[0x01C / 4] = (void *)CG_SOF2_Import106_VCall_Arg1_Ret0;
	s_sof2Import106Vftable[0x03C / 4] = (void *)CG_SOF2_Import106_VCall_Arg2_Ret0;
	s_sof2Import106Vftable[0x050 / 4] = (void *)CG_SOF2_Import106_VCall_Arg1_Cdecl_Ret0;
	s_sof2Import106Vftable[0x100 / 4] = (void *)CG_SOF2_Import106_VCall_Arg2_Ret0;
	s_sof2Import106Vftable[0x10C / 4] = (void *)CG_SOF2_Import106_VCall_Arg1_Ret0;
	s_sof2Import106Vftable[0x128 / 4] = (void *)CG_SOF2_Import106_VCall_Arg4_Ret0;
	s_sof2Import106Vftable[0x14C / 4] = (void *)CG_SOF2_Import106_VCall_Arg5_Ret0f;
	s_sof2Import106Vftable[0x150 / 4] = (void *)CG_SOF2_Import106_VCall_NoArgs;
	s_sof2Import106Vftable[0x15C / 4] = (void *)CG_SOF2_Import106_VCall_Arg1_Ret0;

	s_sof2Import106Iface.vftable = s_sof2Import106Vftable;
	s_sof2Import106Iface.lastHandle = 0;
	s_sof2Import106Iface.reserved0 = 0;
	s_sof2Import106Iface.reserved1 = 0;
	s_sof2Import106Init = qtrue;
}

static int CG_SOF2_Import105_Bridge( int handle ) {
	CG_SOF2_InitImport106Iface();
	s_sof2Import106Iface.lastHandle = handle;
	{
		static int logCount = 0;
		if ( logCount < 8 ) {
			Com_Printf( "[SOF2 import105 bridge] caller=%p handle=%d\n", _ReturnAddress(), handle );
			++logCount;
		}
	}
	return 1;
}

static void *CG_SOF2_Import106_Bridge( void ) {
	CG_SOF2_InitImport106Iface();
	{
		static int logCount = 0;
		if ( logCount < 8 ) {
			Com_Printf( "[SOF2 import106 bridge] caller=%p iface=%p\n",
				_ReturnAddress(), &s_sof2Import106Iface );
			++logCount;
		}
	}
	return &s_sof2Import106Iface;
}

static void CG_SOF2_Import107_Trace( void *ptr ) {
	static int logCount = 0;
	if ( logCount < 12 ) {
		Com_Printf( "[SOF2 import107] caller=%p ptr=%p\n", _ReturnAddress(), ptr );
		++logCount;
	}
}

static void CG_SOF2_Import108_Trace( void *ptr ) {
	static int logCount = 0;
	if ( logCount < 24 ) {
		Com_Printf( "[SOF2 import108] caller=%p ptr=%p\n", _ReturnAddress(), ptr );
		++logCount;
	}
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
	static int sentinelCallCount = 0;
	sentinelCallCount++;
	if ( sentinelCallCount <= 20 || sentinelCallCount % 3000 == 0 )
		Com_Printf( "^1[CGAME] Called NULL import slot! retAddr=%p\n", retAddr );
	return 0;
}

static int CG_NullSlotSentinelTagged( int slot ) {
	void *retAddr = _ReturnAddress();
	static int sentinelCallCount = 0;
	sentinelCallCount++;
	if ( sentinelCallCount <= 40 || sentinelCallCount % 3000 == 0 ) {
		Com_Printf( "^1[CGAME] Called NULL import slot %d! retAddr=%p\n", slot, retAddr );
	}
	return 0;
}

static int CG_NullSlot142( void ) { return CG_NullSlotSentinelTagged( 142 ); }
static int CG_NullSlot145( void ) { return CG_NullSlotSentinelTagged( 145 ); }
static int CG_NullSlot149( void ) { return CG_NullSlotSentinelTagged( 149 ); }

static entityState_t *CG_GetNextReliableEvent( int index ) {
	static int logCount = 0;

	if ( logCount < 8 ) {
		Com_DPrintf( "[CG] reliable event iterator stubbed: index=%d caller=%p\n",
			index, _ReturnAddress() );
		logCount++;
	}

	return NULL;
}

static void CG_FlushReliableEvents( void ) {
	static qboolean logged = qfalse;

	if ( !logged ) {
		Com_DPrintf( "[CG] reliable event flush stubbed caller=%p\n", _ReturnAddress() );
		logged = qtrue;
	}
}

static void CG_UI_HudRefresh( void ) {
	static qboolean logged = qfalse;

	if ( uie && uie->UI_Refresh ) {
		if ( !logged ) {
			Com_Printf( "[CG] slot 58 -> UI_Refresh\n" );
			logged = qtrue;
		}
		uie->UI_Refresh();
		return;
	}

	if ( !logged ) {
		Com_Printf( "[CG] slot 58 fallback -> Menu_PaintAll\n" );
		logged = qtrue;
	}

	Menu_PaintAll();
}

// ----- Slot 53: R_SetupFrustum -----
// Computes 4 frustum clip planes from FOV and view orientation.
// cgame uses these for client-side entity/poly culling.
// Format: cplane_t frustum[4] — each is {normal[3], dist, type, signbits, pad[2]} = 20 bytes
static void CG_R_SetupFrustum( float *frustum, float fov_x, float fov_y,
		float *origin, float *axis ) {
	float ang, xs, xc;

	// Sanitize NaN/invalid FOV
	if ( fov_x != fov_x || fov_x <= 0 ) fov_x = 90.0f;
	if ( fov_y != fov_y || fov_y <= 0 ) fov_y = 73.74f;

	// Each cplane_t is 20 bytes: normal(12) + dist(4) + type(1) + signbits(1) + pad(2)
	// frustum[0] = left, [1] = right, [2] = bottom, [3] = top
	float *f;

	// Left plane
	ang = fov_x * (3.14159265358979323846f / 360.0f);
	xs = sinf(ang); xc = cosf(ang);
	f = frustum + 0 * 5; // 20 bytes = 5 floats stride
	f[0] = axis[0] * xs + axis[3] * xc;  // normal[0]
	f[1] = axis[1] * xs + axis[4] * xc;  // normal[1]
	f[2] = axis[2] * xs + axis[5] * xc;  // normal[2]
	f[3] = origin[0]*f[0] + origin[1]*f[1] + origin[2]*f[2]; // dist
	((byte*)f)[16] = 3; // type = PLANE_NON_AXIAL
	((byte*)f)[17] = (f[0]<0?1:0) | (f[1]<0?2:0) | (f[2]<0?4:0); // signbits

	// Right plane
	f = frustum + 1 * 5;
	f[0] = axis[0] * xs - axis[3] * xc;
	f[1] = axis[1] * xs - axis[4] * xc;
	f[2] = axis[2] * xs - axis[5] * xc;
	f[3] = origin[0]*f[0] + origin[1]*f[1] + origin[2]*f[2];
	((byte*)f)[16] = 3;
	((byte*)f)[17] = (f[0]<0?1:0) | (f[1]<0?2:0) | (f[2]<0?4:0);

	// Bottom plane
	ang = fov_y * (3.14159265358979323846f / 360.0f);
	xs = sinf(ang); xc = cosf(ang);
	f = frustum + 2 * 5;
	f[0] = axis[0] * xs + axis[6] * xc;
	f[1] = axis[1] * xs + axis[7] * xc;
	f[2] = axis[2] * xs + axis[8] * xc;
	f[3] = origin[0]*f[0] + origin[1]*f[1] + origin[2]*f[2];
	((byte*)f)[16] = 3;
	((byte*)f)[17] = (f[0]<0?1:0) | (f[1]<0?2:0) | (f[2]<0?4:0);

	// Top plane
	f = frustum + 3 * 5;
	f[0] = axis[0] * xs - axis[6] * xc;
	f[1] = axis[1] * xs - axis[7] * xc;
	f[2] = axis[2] * xs - axis[8] * xc;
	f[3] = origin[0]*f[0] + origin[1]*f[1] + origin[2]*f[2];
	((byte*)f)[16] = 3;
	((byte*)f)[17] = (f[0]<0?1:0) | (f[1]<0?2:0) | (f[2]<0?4:0);
}

// ----- Slot 54: R_CullBox -----
// Test axis-aligned bounding box against frustum planes.
// Returns: 0 = CULL_IN (visible), 1 = CULL_CLIP (partially), 2 = CULL_OUT (culled)
static int CG_R_CullBox( float *frustum, float *bounds ) {
	// Accept everything — worst case is extra draw calls but no crashes
	(void)frustum; (void)bounds;
	return 0; // CULL_IN
}

// ----- Slot 55: R_CullPointAndRadius -----
static int CG_R_CullPointAndRadius( float *pt, float radius ) {
	(void)pt; (void)radius;
	return 0; // CULL_IN
}

// ----- Slot 56: R_CullLocalBox -----
static int CG_R_CullLocalBox( float *frustum, float *origin,
		float *axis, float *bounds ) {
	(void)frustum; (void)origin; (void)axis; (void)bounds;
	return 0; // CULL_IN
}

// slot 0: Printf — SOF2 slot takes pre-formatted string only (no variadic)
static void Printf_stub( const char *s ) {
	static int nullLogCount = 0;
	if ( !s ) {
		if ( nullLogCount < 8 ) {
			Com_Printf( "^3[CG] Printf_stub received NULL string (ignored)\n" );
			++nullLogCount;
		}
		return;
	}
	if ( s && cls.state == CA_ACTIVE && strstr( s, "Quickloading..." ) ) {
		return;
	}
	Com_Printf( "%s", s );
}

// slot 1/2: DPrintf variants in SOF2 are effectively string-only call sites.
// Route through "%s" to avoid treating literal '%' text as format specifiers.
static void DPrintf_stub( const char *s ) {
	if ( !s ) {
		return;
	}
	Com_DPrintf( "%s", s );
}

// slot 3: Sprintf — Com_sprintf returns int; SOF2 slot is void
static void Sprintf_void( char *buf, int size, const char *fmt, ... ) {
	static int warnCount = 0;
	(void)fmt;

	if ( !buf || size <= 0 ) {
		return;
	}

	if ( !fmt ) {
		buf[0] = '\0';
		if ( warnCount < 8 ) {
			Com_Printf( "^3[CG] Sprintf_void received NULL fmt (cleared buffer)\n" );
			++warnCount;
		}
		return;
	}

	// Safety-first bridge for SOF2 import ABI mismatches: treat fmt as literal text.
	Q_strncpyz( buf, fmt, size );
}

// slot 34: Z_Free — Z_Free returns int; SOF2 slot expects void
static void Z_Free_void_stub( void *p ) { Z_Free( p ); }

// Footstep channel tracking for ch=6 (CHAN_ITEM) and ch=7 (CHAN_BODY).
// SOF2 PM_FootstepEvents fires at natural walking cadence (~2/sec). We pass every
// event through immediately (no rate limiting) and auto-stop the channel 800ms after
// the last event, which handles the case where the sound is a looping WAV that
// doesn't stop when the player halts.
// 800ms > walking cadence (~500ms/step) so the timer only fires when truly stopped.
#define FOOTSTEP_STOP_MS  800   // stop channel this many ms after last footstep event

static int   s_footstepEventMs[2] = { 0, 0 };
static int   s_footstepLastEnt[2] = { 0, 0 };

// slot 74: S_StartSound — SOF2 slot uses int for channel; engine uses soundChannel_t
static void S_StartSound_wrapper( const vec3_t origin, int entityNum,
		int entChannel, sfxHandle_t sfx ) {
	if ( entChannel == 6 || entChannel == 7 ) {
		int idx = entChannel - 6;
		s_footstepEventMs[idx] = Sys_Milliseconds();
		s_footstepLastEnt[idx] = entityNum;
	}
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
	if ( !ref ) {
		return (char *)"";
	}
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
	CL_LogPolyToScene( hShader, numVerts, verts, "slot133" );
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
	static int logCount = 0;
	if ( logCount < 12 ) {
		Com_Printf( "[SNAP] CL_GetCurrentSnapshotNumber #%d num=%d serverTime=%d\n",
			logCount + 1,
			snapshotNumber ? *snapshotNumber : -1,
			serverTime ? *serverTime : -1 );
		logCount++;
	}
	return *snapshotNumber;
}

static void CL_GetGameState_wrapper( gameState_t *gs ) {
	static int logCount = 0;
	static int compatMaskCount = 0;
	int cs20Offset = cl.gameState.stringOffsets[20];
	int cs21Offset = cl.gameState.stringOffsets[21];
	const char *cs20 = cs20Offset ? cl.gameState.stringData + cs20Offset : "";
	const char *cs21 = cs21Offset ? cl.gameState.stringData + cs21Offset : "";
	if ( logCount < 6 ) {
		const char *serverInfo = cl.gameState.stringData + cl.gameState.stringOffsets[ CS_SERVERINFO ];
		const char *systemInfo = cl.gameState.stringData + cl.gameState.stringOffsets[ CS_SYSTEMINFO ];
		const char *mapName = Info_ValueForKey( serverInfo, "mapname" );
		Com_Printf(
			"[SNAP] CL_GetGameState #%d gs=%p dataCount=%d map='%s' cs20='%s' cs21='%s' serverinfo='%.96s' systeminfo='%.96s'\n",
			logCount + 1,
			(void *)gs,
			cl.gameState.dataCount,
			mapName ? mapName : "",
			cs20,
			cs21,
			serverInfo ? serverInfo : "",
			systemInfo ? systemInfo : "" );
		logCount++;
	}
	CL_GetGameState( gs );
	if ( cls.state == CA_PRIMED && compatMaskCount < 2 ) {
		if ( cs20Offset || cs21Offset ) {
			Com_Printf( "[CG init] masking cs20/cs21 for compatibility test: cs20='%s' cs21='%s'\n",
				cs20, cs21 );
		}
		gs->stringOffsets[20] = 0;
		gs->stringOffsets[21] = 0;
		++compatMaskCount;
	}
}

static qboolean CL_GetSnapshot_wrapper( int snapshotNumber, snapshot_t *snapshot ) {
	return CL_GetSnapshot( snapshotNumber, snapshot );
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

static void CG_UpdateScreen_SOF2( void ) {
	static int nestedSkipCount = 0;

	if ( s_cgRenderDepth > 0 ) {
		if ( nestedSkipCount < 8 ) {
			Com_Printf( "[CG] skipped nested UpdateScreen during render depth=%d\n", s_cgRenderDepth );
			nestedSkipCount++;
		}
		return;
	}

	SCR_UpdateScreen();
}

#ifdef _WIN32
static void CG_FormatModuleForAddress( void *address, char *out, size_t outSize ) {
	MEMORY_BASIC_INFORMATION mbi;
	HMODULE module = NULL;
	char modulePath[MAX_PATH];

	if ( !out || outSize == 0 ) {
		return;
	}
	out[0] = '\0';

	if ( !address ) {
		Q_strncpyz( out, "(null)", (int)outSize );
		return;
	}

	if ( VirtualQuery( address, &mbi, sizeof( mbi ) ) == 0 ) {
		Q_strncpyz( out, "(unknown)", (int)outSize );
		return;
	}

	module = (HMODULE)mbi.AllocationBase;
	modulePath[0] = '\0';
	if ( module && GetModuleFileNameA( module, modulePath, sizeof( modulePath ) ) > 0 ) {
		Com_sprintf( out, (int)outSize, "%s+0x%X", modulePath, (unsigned int)((unsigned char *)address - (unsigned char *)module) );
	} else {
		Com_sprintf( out, (int)outSize, "base=%p+0x%X", module, (unsigned int)((unsigned char *)address - (unsigned char *)module) );
	}
}

static int CG_LogDrawInformationException( EXCEPTION_POINTERS *ep ) {
	void *faultAddr = NULL;
	unsigned int exceptionCode = 0;
	void *exceptionAddr = NULL;
	unsigned long accessType = 0;
	void *stackRet = NULL;
	unsigned int eax = 0;
	unsigned int ebx = 0;
	unsigned int ecx = 0;
	unsigned int edx = 0;
	unsigned int esi = 0;
	unsigned int edi = 0;
	unsigned int esp = 0;
	char exceptionModule[512];
	char returnModule[512];

	if ( ep && ep->ExceptionRecord ) {
		exceptionCode = ep->ExceptionRecord->ExceptionCode;
		exceptionAddr = ep->ExceptionRecord->ExceptionAddress;
		if ( ep->ExceptionRecord->NumberParameters >= 2 ) {
			accessType = (unsigned long)ep->ExceptionRecord->ExceptionInformation[0];
			faultAddr = (void *)ep->ExceptionRecord->ExceptionInformation[1];
		}
	}

	if ( ep && ep->ContextRecord ) {
		eax = ep->ContextRecord->Eax;
		ebx = ep->ContextRecord->Ebx;
		ecx = ep->ContextRecord->Ecx;
		edx = ep->ContextRecord->Edx;
		esi = ep->ContextRecord->Esi;
		edi = ep->ContextRecord->Edi;
		esp = ep->ContextRecord->Esp;
		if ( esp ) {
			stackRet = *(void **)esp;
		}
	}
	CG_FormatModuleForAddress( exceptionAddr, exceptionModule, sizeof( exceptionModule ) );
	CG_FormatModuleForAddress( stackRet, returnModule, sizeof( returnModule ) );

	Com_Printf(
		"^1[CG] exception 0x%08X in DrawInformation at %p (%s) accessType=%lu fault=%p ret=%p (%s) eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X esp=%08X\n",
		exceptionCode,
		exceptionAddr,
		exceptionModule,
		accessType,
		faultAddr,
		stackRet,
		returnModule,
		eax,
		ebx,
		ecx,
		edx,
		esi,
		edi,
		esp );

	return EXCEPTION_EXECUTE_HANDLER;
}

static int CG_LogInitException( EXCEPTION_POINTERS *ep ) {
	EXCEPTION_RECORD *rec = ep ? ep->ExceptionRecord : NULL;
	CONTEXT *ctx = ep ? ep->ContextRecord : NULL;
	void *stackRet = NULL;
	char exceptionModule[512];
	char returnModule[512];

	exceptionModule[0] = '\0';
	returnModule[0] = '\0';
	if ( ctx && ctx->Esp ) {
		stackRet = *(void **)ctx->Esp;
	}
	CG_FormatModuleForAddress( ctx ? (void *)ctx->Eip : NULL, exceptionModule, sizeof( exceptionModule ) );
	CG_FormatModuleForAddress( stackRet, returnModule, sizeof( returnModule ) );
	Com_Printf(
		"^1[CG] exception 0x%08X in Init at %p (%s) accessType=%lu fault=%p ret=%p (%s) eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X esp=%08X\n",
		rec ? (unsigned int)rec->ExceptionCode : 0U,
		ctx ? (void *)ctx->Eip : NULL,
		exceptionModule,
		(rec && rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 1)
			? (unsigned long)rec->ExceptionInformation[0] : 0UL,
		(rec && rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
			? (void *)rec->ExceptionInformation[1] : NULL,
		stackRet,
		returnModule,
		ctx ? ctx->Eax : 0U,
		ctx ? ctx->Ebx : 0U,
		ctx ? ctx->Ecx : 0U,
		ctx ? ctx->Edx : 0U,
		ctx ? ctx->Esi : 0U,
		ctx ? ctx->Edi : 0U,
		ctx ? ctx->Esp : 0U );

	// VirtualQuery the fault address to identify memory type
	if ( ctx && ctx->Eip ) {
		MEMORY_BASIC_INFORMATION mbi;
		memset( &mbi, 0, sizeof( mbi ) );
		if ( VirtualQuery( (void *)ctx->Eip, &mbi, sizeof( mbi ) ) ) {
			char allocModule[512];
			allocModule[0] = '\0';
			if ( mbi.AllocationBase ) {
				GetModuleFileNameA( (HMODULE)mbi.AllocationBase, allocModule, sizeof( allocModule ) );
			}
			Com_Printf(
				"^3[CG] fault VQ: base=%p alloc=%p type=0x%X state=0x%X protect=0x%X allocProtect=0x%X module=[%s]\n",
				mbi.BaseAddress,
				mbi.AllocationBase,
				mbi.Type,
				mbi.State,
				mbi.Protect,
				mbi.AllocationProtect,
				allocModule[0] ? allocModule : "(none)" );
		}
	}

	// Stack walk: look at ESP+0 through ESP+120 for return addresses in code regions
	if ( ctx && ctx->Esp ) {
		Com_Printf( "^3[CG] stack walk from esp=%08X:\n", ctx->Esp );
		for ( int si = 0; si <= 30; si++ ) {
			DWORD *slot = (DWORD *)(ctx->Esp + si * 4);
			DWORD val = 0;
			MEMORY_BASIC_INFORMATION slotMbi;
			// verify slot is readable
			if ( !VirtualQuery( slot, &slotMbi, sizeof( slotMbi ) ) ) continue;
			if ( slotMbi.State != MEM_COMMIT ) continue;
			if ( !(slotMbi.Protect & (PAGE_READONLY|PAGE_READWRITE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY|PAGE_WRITECOPY)) ) continue;
			val = *slot;
			if ( val < 0x10000 ) continue; // skip obviously small values
			// check if val points to executable memory
			MEMORY_BASIC_INFORMATION valMbi;
			if ( !VirtualQuery( (void *)val, &valMbi, sizeof( valMbi ) ) ) continue;
			if ( valMbi.State != MEM_COMMIT ) continue;
			if ( !(valMbi.Protect & (PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)) ) continue;
			char addrModule[512];
			addrModule[0] = '\0';
			CG_FormatModuleForAddress( (void *)val, addrModule, sizeof( addrModule ) );
			Com_Printf( "^3[CG]   esp+%02d: 0x%08X (%s)\n", si*4, val, addrModule );
		}
	}

	// DEP recovery: SOF2 (2002) predates hardware NX enforcement and allocates sound/weapon
	// system vtable trampolines in private PAGE_READWRITE memory (no execute bit).
	// Original SoF2.exe ran without DEP; OpenJK gets NX enforcement from the OS.
	// For DEP violations (accessType=8), make the faulting page executable and retry the
	// instruction rather than aborting CG_Init. Limit to 32 unique pages to avoid infinite loops.
	if ( rec && rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
		 rec->NumberParameters >= 1 && rec->ExceptionInformation[0] == 8 &&
		 ctx && ctx->Eip ) {
		static ULONG_PTR s_depFixedPages[32];
		static int s_depFixedCount = 0;
		ULONG_PTR faultPage = ctx->Eip & ~(ULONG_PTR)0xFFF;
		// check if we already fixed this page (prevent infinite loop on pages we can't fix)
		bool alreadyFixed = false;
		for ( int i = 0; i < s_depFixedCount; i++ ) {
			if ( s_depFixedPages[i] == faultPage ) { alreadyFixed = true; break; }
		}
		if ( !alreadyFixed && s_depFixedCount < 32 ) {
			DWORD oldProtect = 0;
			// mark the entire allocation region executable to handle multi-page vtable blocks
			MEMORY_BASIC_INFORMATION depMbi;
			memset( &depMbi, 0, sizeof( depMbi ) );
			SIZE_T regionSize = 0x1000;
			void *regionBase = (void *)faultPage;
			if ( VirtualQuery( (void *)ctx->Eip, &depMbi, sizeof( depMbi ) ) ) {
				regionBase = depMbi.BaseAddress;
				regionSize = depMbi.RegionSize;
			}
			if ( VirtualProtect( regionBase, regionSize, PAGE_EXECUTE_READWRITE, &oldProtect ) ) {
				s_depFixedPages[s_depFixedCount++] = faultPage;
				Com_Printf( "^3[CG] DEP fix: made %p+0x%zX executable (was 0x%X), retrying\n",
					regionBase, (size_t)regionSize, (unsigned)oldProtect );
				return EXCEPTION_CONTINUE_EXECUTION;
			} else {
				Com_Printf( "^1[CG] DEP fix: VirtualProtect failed at %p (err=%lu), aborting\n",
					regionBase, GetLastError() );
			}
		} else if ( alreadyFixed ) {
			Com_Printf( "^1[CG] DEP fix: page %p already fixed but still crashing — aborting\n",
				(void *)faultPage );
		}
	}

	return EXCEPTION_EXECUTE_HANDLER;
}
#endif
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

static void CG_FileTrace( const char *fmt, ... ) {
	FILE *fp;
	va_list args;

	fp = fopen( "cgame_trace.log", "a" );
	if ( !fp ) {
		return;
	}

	va_start( args, fmt );
	vfprintf( fp, fmt, args );
	va_end( args );
	fputc( '\n', fp );
	fclose( fp );
}

static qboolean CL_SOF2_ForceCameraOffEnabled( void ) {
	static cvar_t *cv = NULL;
	if ( !cv ) {
		cv = Cvar_Get( "sof2_forceCameraOff", "0", CVAR_ARCHIVE );
	}
	return ( cv && cv->integer ) ? qtrue : qfalse;
}

static void CL_SOF2_ForceRetailCameraOff( void ) {
	typedef void (__cdecl *CG_DisableCameraProc)( void );
	HMODULE hCgame;
	volatile unsigned char *cameraMode;
	unsigned int modeValue;

	if ( !cgameLibrary ) {
		return;
	}

	hCgame = (HMODULE)cgameLibrary;
	cameraMode = (volatile unsigned char *)( (char *)hCgame + 0x0B8378 );
	modeValue = (unsigned int)( *cameraMode );
	if ( modeValue == 0 ) {
		return;
	}

	((CG_DisableCameraProc)( (char *)hCgame + 0x027DA0 ))();
	*cameraMode = 0;

	{
		static int sof2CameraDisableLogCount = 0;
		if ( sof2CameraDisableLogCount < 16 ) {
			Com_Printf( "[SOF2 fix] forced retail cgame camera off (mode=%u)\n", modeValue );
			++sof2CameraDisableLogCount;
		}
	}
}

static void CL_PatchDllRet( HMODULE module, unsigned int rva, const char *name ) {
	unsigned char *pFunc = (unsigned char *)module + rva;
	DWORD oldProt;

	if ( !VirtualProtect( pFunc, 1, PAGE_EXECUTE_READWRITE, &oldProt ) ) {
		Com_Printf( "CL_InitSOF2CGame: failed to patch %s @ RVA 0x%X\n", name, rva );
		CG_FileTrace( "patch failed %s rva=0x%X", name, rva );
		return;
	}

	pFunc[0] = 0xC3; // RET
	VirtualProtect( pFunc, 1, oldProt, &oldProt );
	Com_Printf( "CL_InitSOF2CGame: patched %s @ RVA 0x%X\n", name, rva );
	CG_FileTrace( "patched %s rva=0x%X", name, rva );
}

static void CL_PatchDllBytes( HMODULE module, unsigned int rva, const unsigned char *bytes, size_t count, const char *name ) {
	unsigned char *pFunc = (unsigned char *)module + rva;
	DWORD oldProt;

	if ( !VirtualProtect( pFunc, count, PAGE_EXECUTE_READWRITE, &oldProt ) ) {
		Com_Printf( "CL_InitSOF2CGame: failed to patch %s @ RVA 0x%X\n", name, rva );
		CG_FileTrace( "patch failed %s rva=0x%X", name, rva );
		return;
	}

	memcpy( pFunc, bytes, count );
	VirtualProtect( pFunc, count, oldProt, &oldProt );
	Com_Printf( "CL_InitSOF2CGame: patched %s @ RVA 0x%X (%u bytes)\n", name, rva, (unsigned int)count );
	CG_FileTrace( "patched %s rva=0x%X bytes=%u", name, rva, (unsigned int)count );
}

/*
===================
CL_InitSOF2CGame

Load cgamex86.dll and populate the 163-slot cgame_import_t for SOF2 CGAME_API_VERSION 3.
Called from CL_InitCGame() when the map starts.
===================
*/
qboolean CL_InitSOF2CGame( void ) {
	typedef cgame_export_t *(__cdecl *GetCGameAPIProc)( int version, cgame_import_t *import );
	static const unsigned char retFalse[] = { 0x33, 0xC0, 0xC3 };
	static const unsigned char ret4[] = { 0xC2, 0x04, 0x00 };

		Com_Printf( "[SOF2 build] 2026-03-11-pass55-rain-diag\n" );
	CG_FileTrace( "CL_InitSOF2CGame enter" );
	cgameLibrary = Sys_LoadSPGameDll( "cgame", NULL );   // "cgame" + ARCH_STRING("x86") + DLL_EXT = "cgamex86.dll"
	if ( !cgameLibrary ) {
		Com_Printf( "CL_InitSOF2CGame: failed to load cgamex86" ARCH_STRING DLL_EXT ": %s\n",
					Sys_LibraryError() );
		CG_FileTrace( "load failed: %s", Sys_LibraryError() ? Sys_LibraryError() : "(null)" );
		return qfalse;
	}
	CG_FileTrace( "cgameLibrary=%p", cgameLibrary );

	GetCGameAPIProc GetCGameAPI = (GetCGameAPIProc)Sys_LoadFunction( cgameLibrary, "GetCGameAPI" );
	if ( !GetCGameAPI ) {
		Sys_UnloadDll( cgameLibrary );
		cgameLibrary = NULL;
		Com_Printf( "CL_InitSOF2CGame: GetCGameAPI not found\n" );
		CG_FileTrace( "GetCGameAPI missing" );
		return qfalse;
	}
	CG_FileTrace( "GetCGameAPI=%p", GetCGameAPI );

	// -----------------------------------------------------------------------
	// Binary patch: CGlassMgr::ResolveConstraints (RVA 0x16BB0) crashes because
	// the glass physics constraint vectors are uninitialized.  Patch first byte
	// to RET (0xC3) so the function returns immediately.
	// -----------------------------------------------------------------------
	{
		HMODULE hCgame = (HMODULE)cgameLibrary;
		CL_PatchDllRet( hCgame, 0x16BB0, "CGlassMgr::ResolveConstraints" );

		// CG_CamEnable (RVA 0x009E40) triggers ICARUS scripted camera mode which causes
		// a 180° camera flip when the player enters certain map triggers (e.g. pra1).
		// Always patch it out: ret 4 = return immediately without enabling camera mode.
		// CG_GetCameraMode (RVA 0x027D90) always returns 0 (camera inactive) to match.
		CL_PatchDllBytes( hCgame, 0x009E40, ret4, sizeof( ret4 ), "CG_CamEnable" );
		CL_PatchDllBytes( hCgame, 0x027D90, retFalse, sizeof( retFalse ), "CG_GetCameraMode" );
		CL_SOF2_ForceRetailCameraOff();

		// CG_DrawSkyboxPortal (RVA 0xA2D0) uses an uninitialized ECX register — called
		// from CG_DrawActive without setting up a valid 'this' pointer. It reads
		// *(float*)(ECX+0x1c) which crashes or draws garbage over the already-rendered
		// main scene, causing sprites/trees to flash every other frame. Patch to no-op.
		CL_PatchDllRet( hCgame, 0x00A2D0, "CG_DrawSkyboxPortal" );
		// CL_PatchDllRet( hCgame, 0x00AE90, "CG_DrawActive" );
		// Re-enable weather effects while keeping only lightweight crash isolation.
		// CL_PatchDllRet( hCgame, 0x00AC50, "CG_DrawRaindrops" );
		CL_PatchDllRet( hCgame, 0x00B180, "CG_DrawFPS" );
		// HUD/sprite paths are now live for parity with retail presentation:
		// CG_DrawTimerHUD, CG_DrawCrosshairHitMarker, CG_DrawScopeOverlay,
		// CG_DrawCorpseMarkers, CG_Draw2DSprites, CG_DrawVehicleDirection.
		// Let retail cgame load its default/weapons HUD definitions again.
		// Slot 58 now routes to the live UI refresh path instead of a stub.
		// CL_PatchDllRet( hCgame, 0x0284F0, "CG_LoadInventoryMenus" );
		// Re-enable spawned world entities (props/crates/etc).
		// CL_PatchDllRet( hCgame, 0x0301E0, "CG_SpawnEntitiesFromSpawnVars" );
		// Re-enable player entity init (required for first-person presentation).
		// CL_PatchDllBytes( hCgame, 0x068A10, retTrue8, sizeof( retTrue8 ), "CG_InitPlayerEntity" );
	}

	// -----------------------------------------------------------------------
	// Populate cgame_import_t (163 entries, zero-initialized).
	// Unset slots remain NULL — non-critical features will simply be stubs.
	// Full slot reference: E:\SOF2\structs\cgame_import_t.h
	// -----------------------------------------------------------------------
	static cgame_import_t import = {};

	// [  0] Printf — SOF2 slot takes pre-formatted string; wrap to add format
	import.Printf                     = Printf_stub;
	// [  1] DPrintf
	import.DPrintf                    = (void (*)(const char *, ...))DPrintf_stub;
	// [  2] DPrintf2 (same function, alternate verbosity)
	import.DPrintf2                   = (void (*)(const char *, ...))DPrintf_stub;
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
	// [ 23] Cvar_GetModified — SOF2 expects its original cvar_t layout
	import.Cvar_GetModified           = CG_Cvar_GetModified_SOF2;
	// [ 24] Cvar_Register
	import.Cvar_Register              = Cvar_Register;
	// [ 25] Cvar_Update
	import.Cvar_Update                = CG_Cvar_Update_SOF2;
	// [ 26] Cvar_Set
	import.Cvar_Set                   = CG_Cvar_Set_SOF2;
	// [ 27] Cvar_SetModified
	import.Cvar_SetModified           = CG_Cvar_SetModified;
	// [ 28] Cvar_SetValue
	import.Cvar_SetValue              = CG_Cvar_SetValue_SOF2;
	// [ 29] Cvar_VariableIntegerValue
	import.Cvar_VariableIntegerValue  = Cvar_VariableIntegerValue;
	// [ 30] Cvar_VariableValue
	import.Cvar_VariableValue         = Cvar_VariableValue;
	// [ 31] Cvar_VariableStringBuffer
	import.Cvar_VariableStringBuffer  = Cvar_VariableStringBuffer;

	// [ 32] Z_Malloc
	import.Z_Malloc                   = (void *(*)(int))Z_Malloc_stub;
	// [ 33] Z_Free — wrap to discard int return
	import.Z_Free                     = Z_Free_void_stub;
	// [ 34] Z_CheckHeap
	import.Z_CheckHeap                = CG_Z_CheckHeap;

	// [35-49] Terrain system — all stubs for now (Phase 2+)

	// [50-52] Renderer image loading — stubs

	// [53-56] Frustum/culling — cgame-side view frustum
	import.R_SetupFrustum             = CG_R_SetupFrustum;
	import.R_CullBox                  = CG_R_CullBox;
	import.R_CullPointAndRadius       = CG_R_CullPointAndRadius;
	import.R_CullLocalBox             = CG_R_CullLocalBox;

	// [ 57] Com_WriteCam — variadic wrapper needed (engine fn is non-variadic)
	import.Com_WriteCam               = Com_WriteCam_wrapper;
	// [ 58] UpdateScreen
	import.UpdateScreen               = CG_UpdateScreen_SOF2;
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
	import.CL_GetGlconfig             = (void (*)(glconfig_t *))CL_GetGlconfig_SOF2;
	// [ 91] CL_GetGameState
	import.CL_GetGameState            = CL_GetGameState_wrapper;
	// [ 92] CL_AddCgameCommand
	import.CL_AddCgameCommand         = CL_AddCgameCommand;
	// [ 93] CL_AddReliableCommand
	import.CL_AddReliableCommand      = CL_AddReliableCommand;
	// [ 94] CL_GetCurrentSnapshotNumber
	import.CL_GetCurrentSnapshotNumber = CL_GetCurrentSnapshotNumber_wrapper;
	// [ 95] CL_GetSnapshot
	import.CL_GetSnapshot             = CL_GetSnapshot_wrapper;
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
	// [117] G2API_HaveWeGhoul2Models — wrapper takes void* for NULL safety, cast to match struct type
	((void **)&import)[117] = (void *)CG_G2API_HaveWeGhoul2Models;
	// [118] G2_GetGhoul2InfoByHandle
	import.G2_GetGhoul2InfoByHandle   = CG_G2_GetGhoul2InfoByHandle;
	// [119] G2API_SetGhoul2ModelIndexes
	import.G2API_SetGhoul2ModelIndexes = CG_G2API_SetGhoul2ModelIndexes;
	// [120] G2API_ReRegisterModels
	import.G2API_ReRegisterModels     = CG_G2API_ReRegisterModels;
	// [124] RE_MarkFragments
	import.RE_MarkFragments           = (void (*)(const vec3_t, const vec3_t, int, float *, int, markFragment_t *))re.MarkFragments;
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
	import.R_ClearScene               = CG_R_ClearScene_Wrapper;
	// [133] R_AddRefEntityToScene
	import.R_AddRefEntityToScene      = CG_R_AddRefEntityToScene_Wrapper;
	// [134] R_AddMiniRefEntityToScene
	import.R_AddMiniRefEntityToScene  = CG_R_AddMiniRefEntityToScene;
	// [135] R_AddPolyToScene (SOF2 has extra 'num' param ignored)
	import.R_AddPolyToScene           = R_AddPolyToScene_SOF2;
	// [136] R_AddLightToScene
	import.R_AddLightToScene          = CG_R_AddLightToScene_Wrapper;
	// [137] R_AddDirectedLightToScene
	import.R_AddDirectedLightToScene  = CG_R_AddDirectedLightToScene;
	// [138] R_AddAdditiveLightToScene
	import.R_AddAdditiveLightToScene  = CG_R_AddAdditiveLightToScene;
	// [139] R_RenderScene
	import.R_RenderScene              = (void (*)(const refdef_t *))CG_R_RenderScene_Wrapper;
	// [140] R_SetColor
	import.R_SetColor                 = (void (*)(const vec4_t))CG_R_SetColor_SOF2;
	// [141] R_DrawStretchPic
	import.R_DrawStretchPic           = (decltype(import.R_DrawStretchPic))CG_R_DrawStretchPic_Wrapper;
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
		slots[64] = (void *)CG_R_ClearScene_Wrapper;         // [64] R_ClearScene ← verified

		// --- Slot 65: CM_PointContents ---
		// The struct has CL_UI_SnapVector at slot 65 (extra METIS stub that
		// doesn't exist in SOF2's DLL). The DLL expects CM_PointContents here.
		// Verified via 4 Ghidra xrefs: CG_PointContents, CG_ScopeTrace,
		// CG_Entity_EmitFireParticle (×2). Called every frame.
		slots[65] = (void *)CM_PointContents;                // [65] CM_PointContents ← verified CRITICAL FIX

		// --- METIS UI stubs 59, 61, 62 ---
		// These are SOF2-specific UI helper imports for menu/loading screen management.
		// Verified via Ghidra xref decompilation of CG_LoadInventoryMenus, CG_SetLoadingLevelshot.
		slots[58] = (void *)CG_UI_HudRefresh;                // [58] HUD/UI per-frame refresh
		slots[59] = (void *)CG_UI_LoadMenuData;              // [59] UI_LoadMenuData ← verified
		slots[61] = (void *)CG_UI_MenuReset;                 // [61] UI_MenuReset ← verified
		slots[62] = (void *)CG_UI_SetInfoScreenText;         // [62] UI_SetInfoScreenText ← verified

		// --- Slots 105-108: unresolved retail helpers ---
		// [105]/[106] now bridge to a non-null interface object so retail
		// object+0x548 virtual dispatch stays alive.
		slots[105] = (void *)CG_SOF2_Import105_Bridge;
		slots[106] = (void *)CG_SOF2_Import106_Bridge;
		slots[107] = (void *)CG_SOF2_Import107_Trace;
		slots[108] = (void *)CG_SOF2_Import108_Trace;

		// --- Slot 116: G2_InitWraithSurfaceMap ---
		// CG_InitWraith calls this with (void**) expecting qboolean.
		// Must return CWraithStub* — the DLL calls through its 78-entry vtable.
		slots[116] = (void *)CG_G2_InitWraithSurfaceMap;       // [116] G2_InitWraithSurfaceMap ← verified CRASH FIX

		// --- Sound / scene crossover 78-82 ---
		// Retail cgame uses slot 81 from CG_DrawInformation as the primary
		// scene submit path. Earlier builds incorrectly treated [81] as
		// S_Respatialize, which explains why the newer source tree stopped
		// hitting the main RenderScene wrapper at all.
		slots[78] = (void *)S_AddLoopingSound_SOF2;         // [78] S_AddRealLoopingSound ← verified
		slots[79] = (void *)CG_S_StopLoopingSound;          // [79] S_StopLoopingSound ← verified
		slots[80] = (void *)S_UpdateEntityPosition;          // [80] S_UpdateEntityPosition ← verified
		slots[81] = (void *)CG_R_RenderScene_SOF2Main;       // [81] R_RenderScene ← verified from CG_DrawInformation
		slots[82] = (void *)S_RegisterSound_SOF2;            // [82] S_RegisterSound ← verified

		// --- Snapshot / command block 90-99 ---
		// Verified in cgamex86.dll:
		// [94] CG_ProcessSnapshots -> CL_GetCurrentSnapshotNumber
		// [95] CG_ReadNextSnapshot  -> CL_GetSnapshot
		slots[90] = (void *)CL_GetGlconfig_SOF2;             // [90] CL_GetGlconfig
		slots[91] = (void *)CL_GetGameState_wrapper;         // [91] CL_GetGameState
		slots[92] = (void *)CL_AddCgameCommand;              // [92] CL_AddCgameCommand
		slots[93] = (void *)CL_AddReliableCommand;           // [93] CL_AddReliableCommand
		slots[94] = (void *)CL_GetCurrentSnapshotNumber_wrapper; // [94] CL_GetCurrentSnapshotNumber
		slots[95] = (void *)CL_GetSnapshot_wrapper;          // [95] CL_GetSnapshot
		slots[96] = (void *)CL_GetEntityBaseline_SOF2;       // [96] CL_GetEntityBaseline
		slots[97] = (void *)CL_GetServerCommand_int;         // [97] CL_GetServerCommand
		slots[98] = (void *)CL_GetCurrentCmdNumber;          // [98] CL_GetCurrentCmdNumber
		slots[99] = (void *)CL_GetUserCmd;                   // [99] CL_GetUserCmd
		slots[103] = (void *)CG_GetNextReliableEvent;        // [103] CG_TransitionSnapshot reliable event iterator
		slots[104] = (void *)CG_FlushReliableEvents;         // [104] CG_TransitionSnapshot reliable event flush

		// --- Renderer misc / registration 124-129 ---
		slots[124] = (void *)re.MarkFragments;               // [124] RE_MarkFragments
		// SOF2 has R_LoadWorldMap at [125]; struct had CL_CM_SelectSubBSP.
		slots[125] = (void *)re.LoadWorld;                   // [125] R_LoadWorldMap ← verified
		slots[126] = (void *)re.RegisterModel;               // [126] R_RegisterModel ← verified
		slots[127] = (void *)re.RegisterSkin;                // [127] R_RegisterSkin ← verified
		slots[128] = (void *)re.RegisterShader;              // [128] R_RegisterShader ← verified
		slots[129] = (void *)re.RegisterShaderNoMip;         // [129] R_RegisterShaderNoMip ← tentative

		// --- Renderer scene 130-138 ---
		slots[130] = (void *)CG_R_ClearScene_Wrapper;        // [130] R_ClearScene ← verified (was wrongly BeginRegistration!)
		slots[131] = (void *)CG_R_AddRefEntityToScene_Wrapper; // [131] R_AddRefEntityToScene ← tentative
		slots[132] = (void *)CG_FX_SetParentEntity;          // [132] FX_SetParentEntity(int entityNum) ← verified
		slots[133] = (void *)R_AddPolyToScene_SOF2;          // [133] R_AddPolyToScene ← verified
		slots[134] = (void *)CG_NullSlotSentinel;            // [134] unused (no xrefs) ← verified
		slots[135] = (void *)CG_R_AddLightToScene_Wrapper;   // [135] R_AddLightToScene ← tentative
		slots[136] = (void *)CG_R_AddDirectedLightToScene;   // [136] R_AddDirectedLightToScene ← tentative
		slots[137] = (void *)CG_R_AddAdditiveLightToScene;   // [137] R_AddAdditiveLightToScene ← verified
		slots[138] = (void *)CG_R_RenderScene_Wrapper;       // [138] R_RenderScene ← verified

		// --- Renderer drawing 139-142 ---
		slots[139] = (void *)CG_R_SetColor_SOF2;             // [139] R_SetColor / packed-color sentinel adapter
		slots[140] = (void *)CG_R_DrawStretchPic_Wrapper;    // [140] R_DrawStretchPic ← SOF2 ABI adapter
		slots[141] = (void *)CG_R_DrawStretchPicColor_Wrapper; // [141] colored stretch-pic helper used by CG_DrawChar
		slots[142] = (void *)CG_NullSlot142;                 // [142] SOF2-specific (SetRangeFog-like, 3 args)

		// --- Tags, effects, bounds 143-149 ---
		slots[143] = (void *)R_LerpTag_SOF2;                 // [143] R_LerpTag ← tentative
		slots[144] = (void *)CG_R_RemapShader;               // [144] R_RemapShader ← verified (was wrongly S_StartBGTrack!)
		slots[145] = (void *)CG_NullSlot145;                 // [145] R_RegisterFont (no xrefs, unused)
		slots[146] = (void *)CG_R_SetLightStyle;             // [146] R_SetLightStyle ← verified
		slots[147] = (void *)re.ModelBounds;                 // [147] R_ModelBounds ← verified CRASH FIX!
		slots[148] = (void *)CG_RE_DamageSurface;             // [148] RE_DamageSurface ← verified (CG_ClientModel)
		slots[149] = (void *)CG_NullSlot149;                 // [149] R_DamageSurface (SOF2-specific gore, stub)

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

#define SOF2_RETAIL_SOUND_TABLE_MAX 0x200
#define SOF2_RETAIL_PRECACHE_SOUND_MAX MAX_SOUNDS
#define SOF2_RETAIL_LOOPSET_BASE 0x46a
#define SOF2_RETAIL_LOOPSET_MAX ( MAX_CONFIGSTRINGS - SOF2_RETAIL_LOOPSET_BASE )

static unsigned int CL_SOF2_RawEvent( int event ) {
	return (unsigned int)( event & ~EV_EVENT_BITS );
}

static qboolean CL_SOF2_IsRetailSoundEvent( unsigned int rawEvent ) {
	switch ( rawEvent ) {
	case 0x2d: // EV_GENERAL_SOUND
	case 0x2e: // EV_GENERAL_SOUND_LONGDISTANCE
	case 0x2f: // EV_GENERAL_SOUND_ON_ENT
	case 0x32: // EV_GLOBAL_SOUND
		return qtrue;
	default:
		return qfalse;
	}
}

static qboolean CL_SOF2_IsRetailCameraEvent( unsigned int rawEvent ) {
	switch ( rawEvent ) {
	case 0x4b: // EV_CAM_ENABLE
	case 0x4c: // EV_CAM_DISABLE
	case 0x4d: // EV_CAM_ZOOM
	case 0x4e: // EV_CAM_MOVE
	case 0x4f: // EV_CAM_PAN
	case 0x50: // EV_CAM_ROLL
	case 0x51: // EV_CAM_TRACK
	case 0x52: // EV_CAM_FOLLOW
	case 0x53: // EV_CAM_DISTANCE
	case 0x54: // EV_CAM_SHAKE
	case 0x55: // EV_CAM_FADE
	case 0x56: // EV_CAM_PATH
	case 0x59: // EV_CAM_ROFFBEGIN
	case 0x5a: // EV_CAM_ROFFEND
		return qtrue;
	default:
		return qfalse;
	}
}

static qboolean CL_SOF2_IsKnownRetailEvent( unsigned int rawEvent ) {
	if ( rawEvent == 0 ) {
		return qtrue;
	}

	// Retail cgame switch covers 0x01..0x49, then 0x4b..0x57 and 0x59..0x5a.
	// 0x4a and 0x58 fall into UNKNOWN-event error handling.
	if ( rawEvent <= 0x49 ) {
		return qtrue;
	}
	if ( rawEvent >= 0x4b && rawEvent <= 0x57 ) {
		return qtrue;
	}
	if ( rawEvent == 0x59 || rawEvent == 0x5a ) {
		return qtrue;
	}

	return qfalse;
}

static void CL_SOF2_DropCorruptEntity( int snapshotNumber, int entIndex, entityState_t *ent, const char *reason ) {
	static int sof2CorruptEntityLogCount = 0;
	int entNum;
	int entType;
	int eventValue;
	int eventParmValue;
	int loopSoundValue;
	int modelIndexValue;
	int g2RadiusValue;

	if ( !ent ) {
		return;
	}

	entNum = ent->number;
	entType = ent->eType;
	eventValue = ent->event;
	eventParmValue = ent->eventParm;
	loopSoundValue = ent->loopSound;
	modelIndexValue = ent->modelindex;
	g2RadiusValue = ent->g2radius;

	if ( sof2CorruptEntityLogCount < 64 ) {
		Com_DPrintf(
			"[SNAP fix] dropped corrupt entity snap=%d idx=%d num=%d type=%d event=0x%X parm=%d loop=%d model=%d g2=%d why=%s\n",
			snapshotNumber,
			entIndex,
			entNum,
			entType,
			eventValue,
			eventParmValue,
			loopSoundValue,
			modelIndexValue,
			g2RadiusValue,
			reason ? reason : "unknown" );
		++sof2CorruptEntityLogCount;
	}

	memset( ent, 0, sizeof( *ent ) );
	ent->number = entNum;
	ent->eType = ET_INVISIBLE;
	ent->groundEntityNum = ENTITYNUM_NONE;
}

static void CL_SOF2_SanitizeLoopSound( int snapshotNumber, int entIndex, entityState_t *ent ) {
	static int sof2LoopSoundLogCount = 0;
	const int loopSoundMax = ( ent && ent->eType == ET_MOVER ) ? SOF2_RETAIL_SOUND_TABLE_MAX : SOF2_RETAIL_PRECACHE_SOUND_MAX;

	if ( !ent || ent->loopSound == 0 ) {
		return;
	}

	if ( ent->loopSound > 0 && ent->loopSound < loopSoundMax ) {
		return;
	}

	if ( sof2LoopSoundLogCount < 96 ) {
		Com_DPrintf(
			"[SNAP fix] cleared bad loopSound snap=%d idx=%d num=%d type=%d loop=%d model=%d g2=%d soundSet=%d\n",
			snapshotNumber,
			entIndex,
			ent->number,
			ent->eType,
			ent->loopSound,
			ent->modelindex,
			ent->g2radius,
			ent->soundSetIndex );
		++sof2LoopSoundLogCount;
	}

	ent->loopSound = 0;
}

static void CL_SOF2_SanitizeSpeakerSound( int snapshotNumber, int entIndex, entityState_t *ent ) {
	static int sof2SpeakerSoundLogCount = 0;

	if ( !ent || ent->eType != ET_SPEAKER || ent->eventParm == 0 ) {
		return;
	}

	if ( ent->eventParm > 0 && ent->eventParm < SOF2_RETAIL_PRECACHE_SOUND_MAX ) {
		return;
	}

	if ( sof2SpeakerSoundLogCount < 64 ) {
		Com_DPrintf(
			"[SNAP fix] cleared bad speaker sound snap=%d idx=%d num=%d parm=%d client=%d frame=%d\n",
			snapshotNumber,
			entIndex,
			ent->number,
			ent->eventParm,
			ent->clientNum,
			ent->frame );
		++sof2SpeakerSoundLogCount;
	}

	ent->eventParm = 0;
}

static void CL_SOF2_SanitizeLoopSetIndex( int snapshotNumber, int entIndex, entityState_t *ent ) {
	// g2radius is used by multiple SOF2 entity paths; treating it as a strict
	// loop-set index causes valid runtime state to be zeroed.
	(void)snapshotNumber;
	(void)entIndex;
	(void)ent;
}

static void CL_SOF2_SanitizeModelIndices( int snapshotNumber, int entIndex, entityState_t *ent ) {
	static int sof2ModelIndexLogCount = 0;
	qboolean cleared = qfalse;
	int originalModelIndex;
	int originalModelIndex2;
	const qboolean bmodelEntity = ( ent && ( ent->solid == SOLID_BMODEL || ent->eType == ET_MOVER ) ) ? qtrue : qfalse;
	const int modelIndexLimit = bmodelEntity ? MAX_SUBMODELS : MAX_MODELS;

	if ( !ent ) {
		return;
	}
	if ( ent->eType > ( ET_EVENTS + EV_NUM_ENTITY_EVENTS ) ) {
		return;
	}

	originalModelIndex = ent->modelindex;
	originalModelIndex2 = ent->modelindex2;

	// modelindex is serialized as a 9-bit value and ET_MOVER/SOLID_BMODEL entities
	// legitimately reference inline models up to MAX_SUBMODELS (512).
	if ( ent->modelindex < 0 || ent->modelindex >= modelIndexLimit ) {
		ent->modelindex = 0;
		cleared = qtrue;
	}

	if ( ent->modelindex2 < 0 || ent->modelindex2 >= MAX_MODELS ) {
		ent->modelindex2 = 0;
		cleared = qtrue;
	}

	if ( !cleared ) {
		return;
	}

	if ( sof2ModelIndexLogCount < 96 ) {
		Com_DPrintf(
			"[SNAP fix] cleared bad model index snap=%d idx=%d num=%d type=%d solid=0x%X model=%d model2=%d limits=(%d,%d) loop=%d g2=%d\n",
			snapshotNumber,
			entIndex,
			ent->number,
			ent->eType,
			ent->solid,
			originalModelIndex,
			originalModelIndex2,
			modelIndexLimit,
			MAX_MODELS,
			ent->loopSound,
			ent->g2radius );
		++sof2ModelIndexLogCount;
	}
}

static void CL_SOF2_SanitizeEventField( const char *scope, int snapshotNumber, int ownerNum, int slot, int *eventField, int *parmField ) {
	static int sof2BadEventLogCount = 0;
	const unsigned int fullEvent = eventField ? (unsigned int)( *eventField ) : 0U;
	const unsigned int rawEvent = CL_SOF2_RawEvent( fullEvent );

	// Preserve low-range events for retail SOF2 handling. Only clear obviously
	// corrupt values carrying high garbage bits.
	if ( ( fullEvent & 0xFFFF0000U ) != 0U || rawEvent > 0x3FFU ) {
		if ( sof2BadEventLogCount < 32 ) {
			Com_DPrintf(
				"[SNAP fix] cleared corrupt %s snap=%d owner=%d slot=%d full=0x%X raw=0x%X parm=%d\n",
				scope ? scope : "(null)",
				snapshotNumber,
				ownerNum,
				slot,
				fullEvent,
				rawEvent,
				parmField ? *parmField : 0 );
			++sof2BadEventLogCount;
		}

		if ( eventField ) {
			*eventField = 0;
		}
		if ( parmField ) {
			*parmField = 0;
		}
		return;
	}

	if ( CL_SOF2_IsKnownRetailEvent( rawEvent ) ) {
		return;
	}
}

static void CL_SOF2_SanitizeRetailSoundEventParm( const char *scope, int snapshotNumber, int ownerNum, int slot, int *eventField, int *parmField ) {
	static int sof2BadSoundParmLogCount = 0;
	unsigned int rawEvent;
	int eventParm;

	if ( !eventField || !parmField ) {
		return;
	}

	rawEvent = CL_SOF2_RawEvent( *eventField );
	if ( !CL_SOF2_IsRetailSoundEvent( rawEvent ) ) {
		return;
	}

	eventParm = *parmField;
	if ( eventParm >= 0 && eventParm < SOF2_RETAIL_SOUND_TABLE_MAX ) {
		return;
	}

	if ( sof2BadSoundParmLogCount < 96 ) {
		Com_DPrintf(
			"[SNAP fix] dropped bad %s sound parm snap=%d owner=%d slot=%d ev=0x%X parm=%d\n",
			scope ? scope : "(null)",
			snapshotNumber,
			ownerNum,
			slot,
			rawEvent,
			eventParm );
		++sof2BadSoundParmLogCount;
	}

	*eventField = 0;
	*parmField = 0;
}

static void CL_SOF2_StripRetailCameraEvent( const char *scope, int snapshotNumber, int ownerNum, int slot, int *eventField, int *parmField ) {
	static int sof2CameraStripLogCount = 0;
	unsigned int rawEvent;

	if ( !eventField ) {
		return;
	}

	rawEvent = CL_SOF2_RawEvent( *eventField );
	if ( !CL_SOF2_IsRetailCameraEvent( rawEvent ) ) {
		return;
	}

	if ( sof2CameraStripLogCount < 64 ) {
		Com_DPrintf(
			"[SNAP fix] stripped %s camera event snap=%d owner=%d slot=%d ev=0x%X parm=%d\n",
			scope ? scope : "(null)",
			snapshotNumber,
			ownerNum,
			slot,
			rawEvent,
			parmField ? *parmField : 0 );
		++sof2CameraStripLogCount;
	}

	*eventField = 0;
	if ( parmField ) {
		*parmField = 0;
	}
}

static void CL_SOF2_SanitizeSnapshotPlayerState( int snapshotNumber, playerState_t *ps ) {
	int i;
	const qboolean stripCameraEvents = CL_SOF2_ForceCameraOffEnabled();

	if ( !ps ) {
		return;
	}

	CL_SOF2_SanitizeEventField( "ps.external", snapshotNumber, ps->clientNum, -1, &ps->externalEvent, &ps->externalEventParm );
	CL_SOF2_SanitizeRetailSoundEventParm( "ps.external", snapshotNumber, ps->clientNum, -1, &ps->externalEvent, &ps->externalEventParm );
	if ( stripCameraEvents ) {
		CL_SOF2_StripRetailCameraEvent( "ps.external", snapshotNumber, ps->clientNum, -1, &ps->externalEvent, &ps->externalEventParm );
	}
	for ( i = 0; i < MAX_PS_EVENTS; ++i ) {
		CL_SOF2_SanitizeEventField( "ps.event", snapshotNumber, ps->clientNum, i, &ps->events[i], &ps->eventParms[i] );
		CL_SOF2_SanitizeRetailSoundEventParm( "ps.event", snapshotNumber, ps->clientNum, i, &ps->events[i], &ps->eventParms[i] );
		if ( stripCameraEvents ) {
			CL_SOF2_StripRetailCameraEvent( "ps.event", snapshotNumber, ps->clientNum, i, &ps->events[i], &ps->eventParms[i] );
		}
	}
}

static void CL_SOF2_SanitizeSnapshotEntity( int snapshotNumber, int entIndex, entityState_t *ent ) {
	static int sof2EventLogCount = 0;
	static int sof2SoundDropLogCount = 0;
	unsigned int rawEvent;
	const qboolean stripCameraEvents = CL_SOF2_ForceCameraOffEnabled();
	const qboolean legacyEntityType =
		( ent && ent->eType >= ET_GENERAL && ent->eType <= ( ET_EVENTS + EV_NUM_ENTITY_EVENTS ) ) ? qtrue : qfalse;

	if ( !ent ) {
		return;
	}

	if ( ent->eType < -1024 || ent->eType > 4096 ) {
		CL_SOF2_DropCorruptEntity( snapshotNumber, entIndex, ent, "etype" );
		return;
	}
	if ( !legacyEntityType ) {
		return;
	}

	// Sanitize eType-encoded events (eType = ET_EVENTS + eventNum).
	// The retail cgame decodes these as event = eType - ET_EVENTS, so an unknown
	// event encoded here will bypass the ent->event sanitization below and crash.
	if ( ent->eType >= ET_EVENTS && ent->eType <= ( ET_EVENTS + EV_NUM_ENTITY_EVENTS ) ) {
		unsigned int etypeEvent = (unsigned int)( ent->eType - ET_EVENTS );
		if ( !CL_SOF2_IsKnownRetailEvent( etypeEvent ) ) {
			Com_DPrintf(
				"[SNAP note] preserving unknown eType-event snap=%d idx=%d num=%d eType=%d etypeEv=%u\n",
				snapshotNumber,
				entIndex,
				ent->number,
				ent->eType,
				etypeEvent );
		}
	}

	CL_SOF2_SanitizeLoopSound( snapshotNumber, entIndex, ent );
	CL_SOF2_SanitizeSpeakerSound( snapshotNumber, entIndex, ent );
	CL_SOF2_SanitizeLoopSetIndex( snapshotNumber, entIndex, ent );
	CL_SOF2_SanitizeModelIndices( snapshotNumber, entIndex, ent );

	CL_SOF2_SanitizeEventField( "entity.event", snapshotNumber, ent ? ent->number : -1, entIndex, ent ? &ent->event : NULL, ent ? &ent->eventParm : NULL );
	CL_SOF2_SanitizeRetailSoundEventParm( "entity.event", snapshotNumber, ent ? ent->number : -1, entIndex, ent ? &ent->event : NULL, ent ? &ent->eventParm : NULL );
	if ( stripCameraEvents ) {
		CL_SOF2_StripRetailCameraEvent( "entity.event", snapshotNumber, ent ? ent->number : -1, entIndex, ent ? &ent->event : NULL, ent ? &ent->eventParm : NULL );
	}
	rawEvent = CL_SOF2_RawEvent( ent->event );

	if ( rawEvent == 0 ) {
		return;
	}

	if ( CL_SOF2_IsRetailSoundEvent( rawEvent ) ) {
		if ( sof2EventLogCount < 96 ) {
			Com_DPrintf(
				"[SNAP event] snap=%d idx=%d num=%d ev=0x%X parm=%d other=%d other2=%d g2=%d ang2=%.2f\n",
				snapshotNumber,
				entIndex,
				ent->number,
				rawEvent,
				ent->eventParm,
				ent->otherEntityNum,
				ent->otherEntityNum2,
				ent->g2radius,
				ent->angles[2] );
			++sof2EventLogCount;
		}

		if ( ent->eventParm < 0 || ent->eventParm >= SOF2_RETAIL_SOUND_TABLE_MAX ) {
			if ( sof2SoundDropLogCount < 64 ) {
				Com_DPrintf(
					"[SNAP fix] dropped bad sound event snap=%d idx=%d num=%d ev=0x%X parm=%d other=%d other2=%d g2=%d\n",
					snapshotNumber,
					entIndex,
					ent->number,
					rawEvent,
					ent->eventParm,
					ent->otherEntityNum,
					ent->otherEntityNum2,
					ent->g2radius );
				++sof2SoundDropLogCount;
			}

			ent->event = 0;
			ent->eventParm = 0;
		}

		return;
	}

}

/*
====================
CL_GetSnapshot
====================
*/
qboolean	CL_GetSnapshot( int snapshotNumber, snapshot_t *snapshot ) {
	clSnapshot_t	*clSnap;
	int				i, count;
	static int snapshotEntryLogCount = 0;
	static int snapshotRejectLogCount = 0;
	static int snapshotLogCount = 0;

	if ( snapshotEntryLogCount < 16 ) {
		Com_Printf(
			"[SNAP] CL_GetSnapshot enter #%d req=%d latest=%d parseEntitiesNum=%d\n",
			snapshotEntryLogCount + 1,
			snapshotNumber,
			cl.frame.messageNum,
			cl.parseEntitiesNum );
		++snapshotEntryLogCount;
	}

	if ( snapshotNumber > cl.frame.messageNum ) {
		Com_Error( ERR_DROP, "CL_GetSnapshot: snapshotNumber > cl.frame.messageNum" );
	}

	// if the frame has fallen out of the circular buffer, we can't return it
	if ( cl.frame.messageNum - snapshotNumber >= PACKET_BACKUP ) {
		if ( snapshotRejectLogCount < 16 ) {
			Com_Printf(
				"[SNAP] CL_GetSnapshot reject #%d req=%d reason=packet_backup latest=%d delta=%d limit=%d\n",
				snapshotRejectLogCount + 1,
				snapshotNumber,
				cl.frame.messageNum,
				cl.frame.messageNum - snapshotNumber,
				PACKET_BACKUP );
			++snapshotRejectLogCount;
		}
		return qfalse;
	}

	// if the frame is not valid, we can't return it
	clSnap = &cl.frames[snapshotNumber & PACKET_MASK];
	if ( !clSnap->valid ) {
		if ( snapshotRejectLogCount < 16 ) {
			Com_Printf(
				"[SNAP] CL_GetSnapshot reject #%d req=%d reason=invalid_frame slot=%d serverTime=%d\n",
				snapshotRejectLogCount + 1,
				snapshotNumber,
				snapshotNumber & PACKET_MASK,
				clSnap->serverTime );
			++snapshotRejectLogCount;
		}
		return qfalse;
	}

	// if the entities in the frame have fallen out of their
	// circular buffer, we can't return it
	if ( cl.parseEntitiesNum - clSnap->parseEntitiesNum >= MAX_PARSE_ENTITIES ) {
		if ( snapshotRejectLogCount < 16 ) {
			Com_Printf(
				"[SNAP] CL_GetSnapshot reject #%d req=%d reason=parse_entities parseNow=%d parseBase=%d delta=%d limit=%d\n",
				snapshotRejectLogCount + 1,
				snapshotNumber,
				cl.parseEntitiesNum,
				clSnap->parseEntitiesNum,
				cl.parseEntitiesNum - clSnap->parseEntitiesNum,
				MAX_PARSE_ENTITIES );
			++snapshotRejectLogCount;
		}
		return qfalse;
	}

	// write the snapshot block for the SOF2 cgame ABI
	snapshot->serverTime = clSnap->serverTime;
	snapshot->snapFlags = clSnap->snapFlags;
	snapshot->ping = clSnap->ping;
	memcpy( snapshot->areamask, clSnap->areamask, sizeof( snapshot->areamask ) );
	snapshot->ps = clSnap->ps;
	CL_SOF2_SanitizeSnapshotPlayerState( snapshotNumber, &snapshot->ps );
	snapshot->pad_204 = 0;
	snapshot->serverCommandSequence = clSnap->serverCommandNum;
	count = clSnap->numEntities;
	if ( count > MAX_ENTITIES_IN_SNAPSHOT ) {
		Com_DPrintf( "CL_GetSnapshot: truncated %i entities to %i\n", count, MAX_ENTITIES_IN_SNAPSHOT );
		count = MAX_ENTITIES_IN_SNAPSHOT;
	}
	snapshot->numEntities = count;

 	for ( i = 0 ; i < count ; i++ )
	{
		int entNum =  ( clSnap->parseEntitiesNum + i ) & (MAX_PARSE_ENTITIES-1) ;
		snapshot->entities[i] = cl.parseEntities[ entNum ];
		CL_SOF2_SanitizeSnapshotEntity( snapshotNumber, i, &snapshot->entities[i] );
	}

	if ( snapshotLogCount < 12 ) {
		Com_Printf(
			"[SNAP] CL_GetSnapshot #%d req=%d serverTime=%d snapFlags=0x%x ping=%d numEntities=%d parseBase=%d firstEntity=%d\n",
			snapshotLogCount + 1,
			snapshotNumber,
			snapshot->serverTime,
			snapshot->snapFlags,
			snapshot->ping,
			snapshot->numEntities,
			clSnap->parseEntitiesNum,
			snapshot->numEntities > 0 ? snapshot->entities[0].number : -1 );
		snapshotLogCount++;
	}

	static int snapshotEntityLogCount = 0;
	if ( snapshotEntityLogCount < 18 ) {
		const int detailCount = snapshot->numEntities < 3 ? snapshot->numEntities : 3;
		for ( i = 0; i < detailCount && snapshotEntityLogCount < 18; ++i ) {
			const entityState_t *ent = &snapshot->entities[i];
			Com_Printf(
				"[SNAP ent] log=%d snap=%d idx=%d num=%d type=%d flags=0x%x model=%d model2=%d client=%d solid=0x%x event=%d weapon=%d skin=%d sound=%d\n",
				snapshotEntityLogCount + 1,
				snapshotNumber,
				i,
				ent->number,
				ent->eType,
				ent->eFlags,
				ent->modelindex,
				ent->modelindex2,
				ent->clientNum,
				ent->solid,
				ent->event,
				ent->weapon,
				ent->skinIndex,
				ent->soundSetIndex );
			++snapshotEntityLogCount;
		}
	}

	static int snapshotInterestingSummaryCount = 0;
	static int snapshotInterestingEntityLogCount = 0;
	if ( snapshotInterestingSummaryCount < 8 ) {
		int moverCount = 0;
		int modeledCount = 0;
		int solidBmodelCount = 0;
		int interestingCount = 0;

		for ( i = 0; i < snapshot->numEntities; ++i ) {
			const entityState_t *ent = &snapshot->entities[i];
			const qboolean isMover = ( ent->eType == ET_MOVER ) ? qtrue : qfalse;
			const qboolean hasModel = ( ent->modelindex != 0 || ent->modelindex2 != 0 ) ? qtrue : qfalse;
			const qboolean solidBmodel = ( ent->solid == SOLID_BMODEL ) ? qtrue : qfalse;

			if ( isMover ) {
				++moverCount;
			}
			if ( hasModel ) {
				++modeledCount;
			}
			if ( solidBmodel ) {
				++solidBmodelCount;
			}
			if ( isMover || hasModel || solidBmodel ) {
				++interestingCount;
			}

			if ( snapshotInterestingEntityLogCount < 96 && ( isMover || hasModel || solidBmodel ) ) {
				Com_Printf(
					"[SNAP interesting] snap=%d idx=%d num=%d type=%d model=%d model2=%d solid=0x%x flags=0x%x event=%d client=%d weapon=%d skin=%d\n",
					snapshotNumber,
					i,
					ent->number,
					ent->eType,
					ent->modelindex,
					ent->modelindex2,
					ent->solid,
					ent->eFlags,
					ent->event,
					ent->clientNum,
					ent->weapon,
					ent->skinIndex );
				++snapshotInterestingEntityLogCount;
			}
		}

		Com_Printf(
			"[SNAP summary] snap=%d numEntities=%d movers=%d modeled=%d solidBmodels=%d interesting=%d\n",
			snapshotNumber,
			snapshot->numEntities,
			moverCount,
			modeledCount,
			solidBmodelCount,
			interestingCount );
		++snapshotInterestingSummaryCount;
	}

	// FIXME: configstring changes and server commands!!!

	return qtrue;
}

// Retail SOF2 uses a different configstring layout than JKA/OpenJK.
// For diagnostics here we want the live SOF2 model table, not the JKA slot base.
#define SOF2_CS_MODELS 32
static const char *CL_SOF2_ModelNameForBaseline( int modelIndex ) {
	const int csIndex = SOF2_CS_MODELS + modelIndex;

	if ( modelIndex <= 0 ) {
		return "(none)";
	}
	if ( csIndex < 0 || csIndex >= MAX_CONFIGSTRINGS ) {
		return "(bad-cs)";
	}
	if ( cl.gameState.stringOffsets[csIndex] <= 0 ) {
		return "(unset)";
	}

	return cl.gameState.stringData + cl.gameState.stringOffsets[csIndex];
}

static qboolean CL_SOF2_IsRenderableBaselineModel( int modelIndex ) {
	const char *name = CL_SOF2_ModelNameForBaseline( modelIndex );

	if ( modelIndex <= 0 || !name || !name[0] ) {
		return qfalse;
	}

	if ( name[0] == '*' ) {
		return qtrue;
	}

	if ( !Q_stricmpn( name, "models/", 7 ) ) {
		return qtrue;
	}

	return qfalse;
}

//bg_public.h won't cooperate in here
#define EF_PERMANENT   0x00080000

qboolean CL_GetDefaultState(int index, entityState_t *state)
{
	static int baselineRequestLogCount = 0;
	static int baselineRejectLogCount = 0;
	static int baselineAcceptLogCount = 0;
	const entityState_t *baseline;
	qboolean acceptBaseline;

	if (index < 0 || index >= MAX_GENTITIES)
	{
		return qfalse;
	}

	baseline = &sv.svEntities[index].baseline;

	if ( baselineRequestLogCount < 32 ) {
		Com_Printf(
			"[BASELINE] req=%d num=%d type=%d flags=0x%x model=%d('%s') model2=%d('%s') solid=0x%x weapon=%d skin=%d\n",
			baselineRequestLogCount + 1,
			index,
			baseline->eType,
			baseline->eFlags,
			baseline->modelindex,
			CL_SOF2_ModelNameForBaseline( baseline->modelindex ),
			baseline->modelindex2,
			CL_SOF2_ModelNameForBaseline( baseline->modelindex2 ),
			baseline->solid,
			baseline->weapon,
			baseline->skinIndex );
		++baselineRequestLogCount;
	}

	acceptBaseline = qfalse;
	if ( baseline->eFlags & EF_PERMANENT ) {
		acceptBaseline = qtrue;
	} else if ( CL_SOF2_IsRenderableBaselineModel( baseline->modelindex ) ||
		CL_SOF2_IsRenderableBaselineModel( baseline->modelindex2 ) ) {
		acceptBaseline = qtrue;
	}

	if ( !acceptBaseline )
	{
		if ( baselineRejectLogCount < 24 &&
			( baseline->modelindex != 0 || baseline->modelindex2 != 0 || baseline->solid == SOLID_BMODEL ) ) {
			Com_Printf(
				"[BASELINE] reject num=%d type=%d flags=0x%x model=%d('%s') model2=%d('%s') solid=0x%x renderable=(%d,%d)\n",
				index,
				baseline->eType,
				baseline->eFlags,
				baseline->modelindex,
				CL_SOF2_ModelNameForBaseline( baseline->modelindex ),
				baseline->modelindex2,
				CL_SOF2_ModelNameForBaseline( baseline->modelindex2 ),
				baseline->solid,
				(int)CL_SOF2_IsRenderableBaselineModel( baseline->modelindex ),
				(int)CL_SOF2_IsRenderableBaselineModel( baseline->modelindex2 ) );
			++baselineRejectLogCount;
		}
		return qfalse;
	}

	*state = *baseline;

	if ( baselineAcceptLogCount < 24 ) {
		Com_Printf(
			"[BASELINE] accept num=%d type=%d flags=0x%x model=%d('%s') model2=%d('%s') solid=0x%x renderable=(%d,%d)\n",
			index,
			state->eType,
			state->eFlags,
			state->modelindex,
			CL_SOF2_ModelNameForBaseline( state->modelindex ),
			state->modelindex2,
			CL_SOF2_ModelNameForBaseline( state->modelindex2 ),
			state->solid,
			(int)CL_SOF2_IsRenderableBaselineModel( state->modelindex ),
			(int)CL_SOF2_IsRenderableBaselineModel( state->modelindex2 ) );
		++baselineAcceptLogCount;
	}

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
		return S_RegisterSoundSafe_SOF2( (const char *) VMA(1), "syscall" );
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
		s_polyLogThisScene = 0;
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
		CL_LogPolyToScene( args[1], args[2], (const polyVert_t *) VMA(3), "syscall" );
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
	{
		const char *menuName = (const char *)VMA(1);
		if ( menuName && !Q_stricmp( menuName, "ingame" ) && cls.state == CA_ACTIVE && !UI_ConsumeIngameMenuRequest() ) {
			Com_Printf( "[SOF2 UI] suppressing cgame ingame open during active play\n" );
			return 0;
		}
		UI_SetActiveMenu( menuName, NULL );
		return 0;
	}

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

	// SOF2 cgame consumes a fourth stack argument during Init and seeds its
	// processed snapshot counter from arg0. Pass the last processed snapshot
	// number so it will fetch the first live snapshot during active rendering.
	const int sof2ProcessedSnapshotNum = cl.frame.messageNum - 1;
	const int sof2InitWeapon = cl.frame.ps.weapon;
	Com_Printf(
		"[CG] Init processedSnapshot=%d latestMessage=%d serverCmdSeq=%d clientNum=%d weapon=%d\n",
		sof2ProcessedSnapshotNum,
		cl.frame.messageNum,
		clc.serverCommandSequence,
		cl.frame.ps.clientNum,
		sof2InitWeapon );
	__try {
		cge->Init( sof2ProcessedSnapshotNum, clc.serverCommandSequence, cl.frame.ps.clientNum, sof2InitWeapon );
	}
	__except ( CG_LogInitException( GetExceptionInformation() ) ) {
		Com_Printf( "^3[CG] continuing after Init exception; cgame init is incomplete\n" );
	}

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
	int timei = cl.serverTime;
	static int renderLogCount = 0;

	s_cgRenderSerial++;
	s_cgCvarUpdatesThisRender = 0;

	re.G2API_SetTime( cl.serverTime, G2T_CG_TIME );
	if ( !cge ) return;

	// Footstep sound cleanup: stop ch=6/7 after FOOTSTEP_STOP_MS of no footstep events.
	// Handles looping WAVs that don't self-terminate when the player halts.
	{
		static const soundChannel_t kFootChans[2] = { (soundChannel_t)6, (soundChannel_t)7 };
		int now = Sys_Milliseconds();
		for ( int fi = 0; fi < 2; fi++ ) {
			if ( s_footstepEventMs[fi] && ( now - s_footstepEventMs[fi] ) > FOOTSTEP_STOP_MS ) {
				S_StopEntityChannel( s_footstepLastEnt[fi], kFootChans[fi] );
				s_footstepEventMs[fi] = 0;
			}
		}
	}

	CL_SOF2_ForceRetailCameraOff();
	if ( renderLogCount < 12 ) {
		CG_FileTrace( "CGameRender #%d state=%d serverTime=%d frameServerTime=%d newSnapshots=%d",
			renderLogCount + 1,
			(int)cls.state,
			cl.serverTime,
			cl.frame.serverTime,
			cl.newSnapshots ? 1 : 0 );
		Com_Printf(
			"[CG] CGameRender #%d state=%d serverTime=%d oldServerTime=%d frameServerTime=%d newSnapshots=%d stereo=%d\n",
			renderLogCount + 1,
			(int)cls.state,
			cl.serverTime,
			cl.oldServerTime,
			cl.frame.serverTime,
			cl.newSnapshots ? 1 : 0,
			(int)stereo );
		renderLogCount++;
	}
	s_cgRenderDepth++;
	s_renderSceneSubmitted = qfalse;
	s_firstSubmittedRefdefValid = qfalse;
	s_sceneSubmitsThisFrame = 0;
	__try {
		cge->DrawInformation( timei );
	} __except ( CG_LogDrawInformationException( GetExceptionInformation() ) ) {
	}
	s_cgRenderDepth--;

	// Flicker fix: if DrawInformation crashed before submitting a scene (it called
	// ClearScene which wiped the buffer, but never called RenderScene), submit a
	// fallback scene using the last known camera so the screen doesn't go blank.
	// Camera-follow fix: update vieworg/viewaxis from current playerState so the
	// camera tracks the player even when the retail cgame's draw path fails.
	if ( !s_renderSceneSubmitted && s_lastGoodRefdefValid && cls.state == CA_ACTIVE ) {
		s_lastGoodRefdef.time = timei;
		// Mirror the same view forcing that CG_SubmitSOF2Refdef applies on the hot path.
		const int forcedViewHeight = ( cl.frame.ps.viewheight > 0 ) ? cl.frame.ps.viewheight : 38;
		vec3_t fallbackAngles;
		for ( int i = 0; i < 3; ++i ) {
			fallbackAngles[i] = SHORT2ANGLE(
				(short)( ANGLE2SHORT( cl.viewangles[i] ) + cl.frame.ps.delta_angles[i] ) );
		}
		VectorCopy( cl.frame.ps.origin, s_lastGoodRefdef.vieworg );
		s_lastGoodRefdef.vieworg[2] += forcedViewHeight;
		AnglesToAxis( fallbackAngles, s_lastGoodRefdef.viewaxis );
		memset( s_lastGoodRefdef.areamask, 0x00, sizeof( s_lastGoodRefdef.areamask ) );
		if ( s_cgLastRefEntityCount > 0 ) {
			static int fallbackReplayLogCount = 0;
			for ( int i = 0; i < s_cgLastRefEntityCount; ++i ) {
				re.AddRefEntityToScene( &s_cgLastRefEntities[i] );
			}
			if ( fallbackReplayLogCount < 16 ) {
				Com_Printf(
					"[SOF2 RS] fallback replayed %d cached ref entities serial=%d time=%d\n",
					s_cgLastRefEntityCount,
					s_cgRenderSerial,
					s_lastGoodRefdef.time );
				++fallbackReplayLogCount;
			}
		}
		re.RenderScene( &s_lastGoodRefdef );
	}
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
	Com_Printf( "[SNAP] CL_FirstSnapshot: entering, serverTime=%d\n", cl.frame.serverTime );

	re.RegisterMedia_LevelLoadEnd();

	cls.state = CA_ACTIVE;
	UI_ResetIngameMenuRequest();
	if ( Key_GetCatcher() & KEYCATCH_UI ) {
		Com_Printf( "[SNAP] CL_FirstSnapshot: clearing KEYCATCH_UI on first active frame\n" );
		Key_SetCatcher( Key_GetCatcher() & ~KEYCATCH_UI );
	}
	// Direct +map startup can carry menu/mouse button state into the first live
	// gameplay snapshot. Flush it once here so the player does not inherit a
	// phantom +attack/+use state on map start.
	Key_ClearStates();
	Com_Printf( "[SNAP] CL_FirstSnapshot: cleared latched key/button state for active play\n" );
	Com_Printf( "[SNAP] CL_FirstSnapshot: cls.state = CA_ACTIVE\n" );

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
		static int waitCount = 0;
		if ( cl.newSnapshots ) {
			Com_Printf( "[SNAP] CL_SetCGameTime: newSnapshots=true after %d waits\n", waitCount );
			cl.newSnapshots = qfalse;
			CL_FirstSnapshot();
		} else {
			if ( waitCount % 300 == 0 ) {
				Com_Printf( "[SNAP] CL_SetCGameTime: waiting for newSnapshots (wait #%d)\n", waitCount );
			}
			waitCount++;
		}

		if ( cls.state != CA_ACTIVE ) {
			return;
		}
	}

	CL_SOF2_ForceRetailCameraOff();

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


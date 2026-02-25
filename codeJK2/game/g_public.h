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

#ifndef __SOF2_G_PUBLIC_H__
#define __SOF2_G_PUBLIC_H__
// g_public.h -- game module information visible to server
//
// SOF2 SP game API version 8.
// Verified from GetGameAPI() @ gamex86.dll:0x2005ef40.
// See E:\SOF2\structs\game_import_t.h and game_export_t.h for full documentation.
//
// KEY DIFFERENCES FROM JK2 v10:
//   - game_import_t: 113 entries (0x1C4 bytes), not JK2's ~200+
//   - G2 API accessed via CWraith vtable in game DLL, NOT via import slots
//   - SaveGame uses WriteChunk/ReadChunk (int chunkId, data, len), NOT ISavedGame
//   - RMG/Arioche system: 7+ dedicated automap drawing import slots (102-112)
//   - game_export_t: 26 function pointers (no data members)
//   - Slots [4] and [5] are SWAPPED vs JK2: SOF2=[4]Disconnect,[5]Command
//   - ClientUserinfoChanged does NOT exist in SOF2 SP

#define	GAME_API_VERSION	8

typedef enum
{
	eNO = 0,
	eFULL,
	eAUTO,
} SavedGameJustLoaded_e;

// entity->svFlags
// the server does not know how to interpret most of the values
// in entityStates (level eType), so the game must explicitly flag
// special server behaviors
#define	SVF_NOCLIENT			0x00000001	// don't send entity to clients, even if it has effects
#define SVF_INACTIVE			0x00000002	// Can't be used when this flag is on
#define SVF_NPC					0x00000004
#define SVF_BOT					0x00000008
#define SVF_PLAYER_USABLE		0x00000010	// player can use this with the use button
#define	SVF_BROADCAST			0x00000020	// send to all connected clients
#define	SVF_PORTAL				0x00000040	// merge a second pvs at origin2 into snapshots
#define	SVF_USE_CURRENT_ORIGIN	0x00000080	// entity->currentOrigin instead of entity->s.origin
										// for link position (missiles and movers)
#define SVF_TRIMODEL			0x00000100	// Use a three piece model make up like a player does
#define	SVF_OBJECTIVE			0x00000200	// Draw it's name if crosshair comes across it
#define	SVF_ANIMATING			0x00000400	// Currently animating from startFrame to endFrame
#define	SVF_NPC_PRECACHE		0x00000800	// Tell cgame to precache this spawner's NPC stuff
#define	SVF_KILLED_SELF			0x00001000	// ent killed itself in a script, so don't do ICARUS_FreeEnt on it... or else!
#define	SVF_NAVGOAL				0x00002000	// Navgoal
#define	SVF_NOPUSH				0x00004000	// Can't be pushed around
#define	SVF_ICARUS_FREEZE		0x00008000	// NPCs are frozen, ents don't execute ICARUS commands
#define SVF_PLAT				0x00010000	// A func_plat or door acting like one
#define	SVF_BBRUSH				0x00020000	// breakable brush
#define	SVF_LOCKEDENEMY			0x00040000	// keep current enemy until dead
#define SVF_IGNORE_ENEMIES		0x00080000	// Ignore all enemies
#define SVF_BEAMING				0x00100000	// Being transported
#define SVF_PLAYING_SOUND		0x00200000	// In the middle of playing a sound
#define SVF_CUSTOM_GRAVITY		0x00400000	// Use personal gravity
#define SVF_BROKEN				0x00800000	// A broken misc_model_breakable
#define SVF_NO_TELEPORT			0x01000000	// Will not be teleported
#define SVF_NONNPC_ENEMY		0x02000000	// Not a client/NPC, but can still be considered a hostile lifeform
#define SVF_SELF_ANIMATING		0x04000000	// Ent will control it's animation itself in it's think func
#define SVF_GLASS_BRUSH			0x08000000	// Ent is a glass brush
#define SVF_NO_BASIC_SOUNDS		0x10000000	// Don't load basic custom sound set
#define SVF_NO_COMBAT_SOUNDS	0x20000000	// Don't load combat custom sound set
#define SVF_NO_EXTRA_SOUNDS		0x40000000	// Don't load extra custom sound set
#define SVF_MOVER_ADJ_AREA_PORTALS	0x80000000	// For scripted movers only- must *explicitly instruct* them to affect area portals
//===============================================================

typedef struct gentity_s gentity_t;

#ifndef GAME_INCLUDE

struct gentity_s {
	entityState_t	s;				// communicated by server to clients
	playerState_t	*client;
	qboolean	inuse;
	qboolean	linked;				// qfalse if not in any good cluster

	int			svFlags;			// SVF_NOCLIENT, SVF_BROADCAST, etc

	qboolean	bmodel;				// if false, assume an explicit mins / maxs bounding box
									// only set by gi.SetBrushModel
	vec3_t		mins, maxs;
	int			contents;			// CONTENTS_TRIGGER, CONTENTS_SOLID, CONTENTS_BODY, etc
									// a non-solid entity should set to 0

	vec3_t		absmin, absmax;		// derived from mins/maxs and origin + rotation

	vec3_t		currentOrigin;
	vec3_t		currentAngles;

	gentity_t	*owner;
/*
Ghoul2 Insert Start
*/
	CGhoul2Info_v	ghoul2;
/*
Ghoul2 Insert End
*/
	// the game dll can add anything it wants after this point in the structure
};

#endif		// GAME_INCLUDE

//===============================================================
//
// functions provided by the main engine — SOF2 v8 game_import_t
// 113 entries (0x1C4 bytes), populated by SV_InitGameProgs()
// Verified from gamex86.dll GetGameAPI @ 0x2005ef40 (memcpy of 0x71 dwords)
//
typedef struct {
	//============== General Services (slots 0-7) ==================

	void    (*Printf)(const char *fmt, ...);                    // +0x000 [slot  0] gi_Printf
	void    (*DPrintf)(const char *fmt, ...);                   // +0x004 [slot  1] gi_DPrintf (debug print)
	void    (*FlushCamFile)(void);                              // +0x008 [slot  2] gi_FlushCamFile (UNUSED)
	NORETURN_PTR void (*Error)(int errLevel, const char *fmt, ...); // +0x00c [slot  3] gi_Error (ERR_DROP=1)
	int     (*Milliseconds)(void);                              // +0x010 [slot  4] gi_Milliseconds
	cvar_t *(*cvar)(const char *name, const char *val, int f);  // +0x014 [slot  5] gi_cvar (Cvar_Get)
	void    (*cvar_set)(const char *name, const char *val);     // +0x018 [slot  6] gi_cvar_set (Cvar_Set)
	void    (*Cvar_VariableStringBuffer)(const char *name,      // +0x01c [slot  7] gi_Cvar_VariableStringBuffer
				char *buffer, int bufsize);

	//============== File System (slots 8-17) ==================

	void    (*FS_FCloseFile)(fileHandle_t handle);              // +0x020 [slot  8] gi_FS_FCloseFile
	long    (*FS_ReadFile)(const char *name, void **buf);       // +0x024 [slot  9] gi_FS_ReadFile
	int     (*FS_Read)(void *buffer, int len, fileHandle_t fh); // +0x028 [slot 10] gi_FS_Read
	int     (*FS_Write)(const void *buf, int len, fileHandle_t fh); // +0x02c [slot 11] gi_FS_Write
	int     (*FS_FOpenFile)(const char *path, fileHandle_t *handle, // +0x030 [slot 12] gi_FS_FOpenFile
				fsMode_t mode);
	void    (*FS_FreeFile)(void *buf);                          // +0x034 [slot 13] gi_FS_FreeFile
	int     (*FS_GetFileList)(const char *path,                 // +0x038 [slot 14] gi_FS_GetFileList
				const char *ext, char *listbuf, int bufsize);
	void   *saved_game;                                         // +0x03c [slot 15] gi_saved_game (opaque ISavedGame ptr, unused by SOF2)
	void   *reserved_16;                                        // +0x040 [slot 16] (unused)
	void   *reserved_17;                                        // +0x044 [slot 17] (unused)

	//============== Console & Client (slots 18-23) ==================

	void    (*SendConsoleCommand)(const char *text);            // +0x048 [slot 18] gi_SendConsoleCommand
	void    (*DropClient)(int clientNum, const char *reason);   // +0x04c [slot 19] gi_DropClient
	int     (*argc)(void);                                      // +0x050 [slot 20] gi_argc
	char   *(*argv)(int n);                                     // +0x054 [slot 21] gi_argv
	void   *reserved_22;                                        // +0x058 [slot 22] (unused)
	void    (*Com_sprintf)(char *dest, int size,                // +0x05c [slot 23] gi_sprintf
				const char *fmt, ...);

	//============== Cvar Extended (slots 24-25) ==================

	int     (*Cvar_VariableIntegerValue)(const char *name);     // +0x060 [slot 24] gi_Cvar_VariableIntegerValue
	void    (*Cvar_SetValue)(const char *name, float val,       // +0x064 [slot 25] gi_Cvar_SetValue
				int force);

	//============== Config & Server (slots 26-31) ==================

	void    (*SetConfigstring)(int num, const char *string);    // +0x068 [slot 26] gi_SetConfigstring
	void    (*GetConfigstring)(int num, char *buf, int bufsize);// +0x06c [slot 27] gi_GetConfigstring
	void    (*SetUserinfo)(int num, const char *buffer);        // +0x070 [slot 28] gi_SetUserinfo
	void    (*GetUserinfo)(int num, char *buffer, int bufsize); // +0x074 [slot 29] gi_GetUserinfo
	void    (*SendServerCommand)(int clientNum,                 // +0x078 [slot 30] gi_SendServerCommand
				const char *fmt, ...);
	void    (*SetBrushModel)(gentity_t *ent, const char *name); // +0x07c [slot 31] gi_SetBrushModel

	//============== Memory (slots 32-34) ==================
	// NOTE: SOF2 uses malloc-based zone memory, NOT tagged memory (no memtag_t)

	void   *(*Malloc)(int size, int tag, qboolean zeroIt);      // +0x080 [slot 32] gi_Malloc (Z_Malloc wrapper)
	int     (*Free)(void *buf);                                 // +0x084 [slot 33] gi_Free (Z_Free wrapper)
	void   *reserved_34;                                        // +0x088 [slot 34] (unused)

	//============== Entity Tokens & Terrain (slots 35-49) ==================

	int     (*GetEntityToken)(char *buf, int bufsize);          // +0x08c [slot 35] gi_GetEntityToken
	void    (*CM_FreeTerrain)(int terrainId);                   // +0x090 [slot 36] gi_CM_FreeTerrain
	void   *reserved_37;                                        // +0x094 [slot 37] (unused)
	void    (*RMG_Init)(int terrainId);                         // +0x098 [slot 38] gi_RMG_Init
	int     (*irand)(int min, int max);                         // +0x09c [slot 39] gi_irand
	void    (*RMG_GetSpawnPoint)(int terrainId, float *pos);    // +0x0a0 [slot 40] gi_RMG_GetSpawnPoint
	int     (*RMG_GetCellInfo)(int terrainId, int x, int y);    // +0x0a4 [slot 41] gi_RMG_GetCellInfo
	void   *reserved_42;                                        // +0x0a8 [slot 42] (unused)
	void    (*RMG_UpdateTerrain)(int terrainId);                // +0x0ac [slot 43] gi_RMG_UpdateTerrain
	void   *reserved_44;                                        // +0x0b0 [slot 44] (unused)
	void   *reserved_45;                                        // +0x0b4 [slot 45] (unused)
	void   *reserved_46;                                        // +0x0b8 [slot 46] (unused)
	void   *reserved_47;                                        // +0x0bc [slot 47] (unused)
	void   *reserved_48;                                        // +0x0c0 [slot 48] (unused)
	void   *reserved_49;                                        // +0x0c4 [slot 49] (unused)

	//============== Collision & Tracing (slots 50-51) ==================

	void    (*trace)(trace_t *results, const vec3_t start,      // +0x0c8 [slot 50] gi_trace (basic)
				const vec3_t mins, const vec3_t maxs,
				const vec3_t end, int passEnt, int contentmask);
	int     (*pointcontents)(const vec3_t point, int passEntityNum); // +0x0cc [slot 51] gi_pointcontents

	//============== SaveGame System (slots 52-53) ==================
	// SOF2 replaces JK2's ISavedGame interface with explicit chunk I/O.
	// The game DLL calls these instead of saved_game->WriteChunk()/ReadChunk().

	void    (*SaveGame_WriteChunk)(int chunkId, void *data,     // +0x0d0 [slot 52] gi_SaveGame_WriteChunk
				int length);
	void    (*SaveGame_ReadChunk)(int chunkId, void *data,      // +0x0d4 [slot 53] gi_SaveGame_ReadChunk
				int length);

	//============== Entity Management (slots 54-62) ==================

	void    (*LocateGameData)(void *gentities);                 // +0x0d8 [slot 54] gi_LocateGameData
	void   *reserved_55;                                        // +0x0dc [slot 55] (unused)
	void    (*GetUserinfoAlt)(int num, char *buf, int bufsize);  // +0x0e0 [slot 56] gi_GetUserinfo (alt)
	void    (*linkentity)(gentity_t *ent);                      // +0x0e4 [slot 57] gi_linkentity
	void    (*unlinkentity)(gentity_t *ent);                    // +0x0e8 [slot 58] gi_unlinkentity
	void    (*MulticastTempEntity)(gentity_t *ent);             // +0x0ec [slot 59] gi_SV_MulticastTempEntity
	void    (*InitTempEntFinalize)(gentity_t *ent);             // +0x0f0 [slot 60] gi_InitTempEntFinalize
	int     (*GetTempEntCount)(void);                           // +0x0f4 [slot 61] gi_GetTempEntCount
	void   *reserved_62;                                        // +0x0f8 [slot 62] (unused)

	//============== ICARUS & Entity Access (slots 63-68) ==================

	void    (*ICARUS_PlaySound)(int entID, const char *name,    // +0x0fc [slot 63] gi_ICARUS_PlaySound
				const char *channel);
	int     (*ICARUS_RunScript)(gentity_t *ent, const char *script); // +0x100 [slot 64] gi_ICARUS_RunScript
	gentity_t *(*GetCurrentEntity)(void);                       // +0x104 [slot 65] gi_GetCurrentEntity
	void    (*G2API_CleanGhoul2Models)(CGhoul2Info_v &ghoul2);  // +0x108 [slot 66] gi_G2API_CleanGhoul2Models
	char   *(*GetLastErrorString)(void);                        // +0x10c [slot 67] gi_GetLastErrorString
	gentity_t *(*GetCurrentEntityIndirect)(void);               // +0x110 [slot 68] gi_GetCurrentEntityIndirect

	//============== Spatial Queries (slots 69-75) ==================

	int     (*EntitiesInBox)(const vec3_t mins, const vec3_t maxs, // +0x114 [slot 69] gi_EntitiesInBox
				gentity_t **list, int maxcount);
	qboolean (*EntityContact)(const vec3_t mins, const vec3_t maxs, // +0x118 [slot 70] gi_EntityContact
				const gentity_t *ent);
	void    (*traceG2)(trace_t *results, const vec3_t start,    // +0x11c [slot 71] gi_trace (G2-enabled)
				const vec3_t mins, const vec3_t maxs,
				const vec3_t end, int passEnt, int contentmask,
				int g2TraceType, int useLod);
	int     (*GetEntityBoundsSize)(gentity_t *ent);             // +0x120 [slot 72] gi_GetEntityBoundsSize
	void    (*SetBrushModelAlt)(gentity_t *ent, const char *name); // +0x124 [slot 73] gi_SetBrushModel (alt)
	qboolean (*inPVS)(const vec3_t p1, const vec3_t p2);        // +0x128 [slot 74] gi_inPVS
	qboolean (*inPVSIgnorePortals)(const vec3_t p1, const vec3_t p2); // +0x12c [slot 75] gi_inPVSIgnorePortals

	//============== Server Data (slots 76-83) ==================

	void   *reserved_76;                                        // +0x130 [slot 76] (unused)
	void    (*SetConfigstringAlt)(int num, const char *str);    // +0x134 [slot 77] gi_SetConfigstring (alt)
	void    (*GetConfigstringAlt)(int num, char *buf, int sz);  // +0x138 [slot 78] gi_GetConfigstring (alt)
	void    (*GetServerinfo)(char *buffer, int bufferSize);     // +0x13c [slot 79] gi_GetServerinfo
	void    (*AdjustAreaPortalState)(gentity_t *ent, qboolean open); // +0x140 [slot 80] gi_AdjustAreaPortalState
	int     (*totalMapContents)(void);                          // +0x144 [slot 81] gi_totalMapContents
	void   *reserved_82;                                        // +0x148 [slot 82] (unused)
	int     (*GetSurfaceMaterial)(int surfIndex, float *vel);   // +0x14c [slot 83] gi_GetSurfaceMaterial

	//============== Model & Ghoul2 (slots 84-92) ==================
	// NOTE: Most G2 API accessed via CWraith vtable, not these slots.
	// Only a few G2 operations come through the import table directly.

	int     (*ModelIndex)(const char *name);                    // +0x150 [slot 84] gi_ModelIndex
	void   *reserved_85;                                        // +0x154 [slot 85] (unused)
	void   *reserved_86;                                        // +0x158 [slot 86] (unused)
	IGhoul2InfoArray &(*TheGhoul2InfoArray)(void);              // +0x15c [slot 87] gi_TheGhoul2InfoArray
	int     (*RE_RegisterSkin)(const char *name);               // +0x160 [slot 88] gi_RE_RegisterSkin
	int     (*RE_GetAnimationCFG)(const char *cfg,              // +0x164 [slot 89] gi_RE_GetAnimationCFG
				char *dest, int destSize);
	CGhoul2Info_v *(*G2API_GetGhoul2InfoV)(int ghoul2handle);   // +0x168 [slot 90] gi_G2API_GetGhoul2InfoV
	void    (*G2API_CleanGhoul2ModelsRef)(CGhoul2Info_v *ghoul2ref); // +0x16c [slot 91] gi_G2API_CleanGhoul2ModelsRef
	void   *reserved_92;                                        // +0x170 [slot 92] (unused)

	//============== String Editor & Sound (slots 93-101) ==================

	char   *(*SE_GetString)(const char *token, int flags);      // +0x174 [slot 93] gi_SE_GetString
	int     (*SoundIndex)(const char *name);                    // +0x178 [slot 94] gi_SoundIndex
	void    (*SE_SetStringSoundMap)(const char *token, int idx);// +0x17c [slot 95] gi_SE_SetStringSoundMap
	int     (*GetEntityToken2)(char *buf, int bufsize);         // +0x180 [slot 96] gi_GetEntityToken2
	int     (*CM_RegisterDamageShader)(const char *name);       // +0x184 [slot 97] gi_CM_RegisterDamageShader
	void    (*WE_GetWindVector)(vec3_t wind, vec3_t atpoint);   // +0x188 [slot 98] gi_WE_GetWindVector
	void    (*SV_UpdateEntitySoundIndex)(int entNum);           // +0x18c [slot 99] gi_SV_UpdateEntitySoundIndex
	int     (*SoundDuration)(const char *name);                 // +0x190 [slot100] gi_SoundDuration
	void    (*SetMusicState)(int state);                        // +0x194 [slot101] gi_SetMusicState

	//============== RMG Automap (slots 102-112) ==================

	void    (*RMG_AddBreakpoint)(int terrainId, void *data);    // +0x198 [slot102] gi_RMG_AddBreakpoint
	void    (*GameShutdown)(void);                              // +0x19c [slot103] gi_GameShutdown
	void    (*RMG_AutomapDrawLine)(int x1, int y1,              // +0x1a0 [slot104] gi_RMG_AutomapDrawLine
				int x2, int y2, int color);
	void    (*RMG_AutomapDrawRegion)(int x, int y,              // +0x1a4 [slot105] gi_RMG_AutomapDrawRegion
				int w, int h, int color);
	void    (*RMG_AutomapDrawCircle)(int x, int y,              // +0x1a8 [slot106] gi_RMG_AutomapDrawCircle
				int radius, int color);
	void    (*RMG_AutomapDrawIcon)(int x, int y,                // +0x1ac [slot107] gi_RMG_AutomapDrawIcon
				int icon, int color);
	void    (*RMG_AutomapDrawSquare)(int x, int y,              // +0x1b0 [slot108] gi_RMG_AutomapDrawSquare
				int size, int color);
	void   *reserved_109;                                       // +0x1b4 [slot109] (unused)
	void    (*RMG_AutomapDrawSpecial)(int x, int y,             // +0x1b8 [slot110] gi_RMG_AutomapDrawSpecial
				int type, int color);
	void   *reserved_111;                                       // +0x1bc [slot111] (unused)
	void   *reserved_112;                                       // +0x1c0 [slot112] (unused)

} game_import_t;  // 0x1C4 bytes total (113 pointers × 4 bytes)

//
// functions exported by the game subsystem — SOF2 v8 game_export_t
// 26 function pointer slots (0x68 bytes), returned by GetGameAPI()
// IMPORTANT: Slot [4]=ClientDisconnect, [5]=ClientCommand (SWAPPED vs JK2 v10)
//
typedef struct {
	// --- Core game callbacks (slots 0-8) ---
	void    (*Init)(int levelTime, int randomSeed, int restart); // +0x00 [0]  InitGame
	void    (*Shutdown)(int restart);                            // +0x04 [1]  G_ShutdownGame
	char   *(*ClientConnect)(int clientNum, int firstTime,       // +0x08 [2]  ClientConnect
				int isBot);
	void    (*ClientBegin)(int clientNum);                       // +0x0c [3]  ClientBegin
	void    (*ClientDisconnect)(int clientNum);                  // +0x10 [4]  ClientDisconnect
	                                                             //            NOTE: slot[4]=Disconnect in SOF2 v8
	                                                             //            (was ClientUserinfoChanged in JK2 v10)
	void    (*ClientCommand)(int clientNum);                     // +0x14 [5]  ClientCommand (dev dispatch)
	                                                             //            NOTE: slot[5]=Command in SOF2 v8
	                                                             //            (was ClientDisconnect in JK2 v10)
	void    (*ClientThink)(int clientNum);                       // +0x18 [6]  ClientThink
	void    (*RunFrame)(int levelTime);                          // +0x1c [7]  G_RunFrame
	int     (*ConsoleCommand)(void);                             // +0x20 [8]  G_ConsoleCommand

	// --- Raven extended slots (9-15) ---
	void    (*SpawnEntitiesFromString)(void);                    // +0x24 [9]  G_SpawnGEntityFromSpawnVars
	void   *reserved0;                                          // +0x28 [10] NULL
	void    (*ClientCommand_Arioche)(int clientNum);             // +0x2c [11] ClientCommand_Arioche (RMG only)
	void    (*InitIcarus)(void);                                 // +0x30 [12] G_InitIcarus
	void    (*ConnectNavs)(void);                                // +0x34 [13] G_ConnectNavs
	void    (*UpdateSavesLeft)(void);                            // +0x38 [14] G_UpdateSavesLeft
	void   *reserved1;                                          // +0x3c [15] NULL

	// --- Save/load and level callbacks (slots 16-25) ---
	void    (*PostLoadInit)(void);                               // +0x40 [16] G_PostLoadInit
	void    (*SaveGame)(void);                                   // +0x44 [17] G_SaveGame
	int     (*GameAllowedToSaveHere)(void);                     // +0x48 [18] G_GameAllowedToSaveHere
	void    (*GetSavedGameJustLoaded)(void);                     // +0x4c [19] G_GetSavedGameJustLoaded
	void    (*WriteLevel)(void);                                 // +0x50 [20] G_WriteLevel
	void    (*ReadLevel)(char *mapname);                         // +0x54 [21] G_ReadLevel
	void    (*GameSpawnRMGEntity)(char *entityString);           // +0x58 [22] G_GameSpawnRMGEntity
	void    (*InitNavigation)(void);                             // +0x5c [23] G_InitNavigation
	void    (*InitSquads)(void);                                 // +0x60 [24] G_InitSquads
	void    (*InitWeaponSystem)(void);                           // +0x64 [25] CWpnSysManager_InitGlobalInstance
} game_export_t;  // 0x68 bytes total (26 function pointers)

game_export_t *GetGameAPI (game_import_t *import);

#endif//#ifndef __SOF2_G_PUBLIC_H__

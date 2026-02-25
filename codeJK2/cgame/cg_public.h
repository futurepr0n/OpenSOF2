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

#ifndef _SOF2_CG_PUBLIC_H
#define _SOF2_CG_PUBLIC_H
//
// cg_public.h — SOF2 SP cgame/engine interface, CGAME_API_VERSION 3
//
// The SOF2 cgame DLL uses STRUCT-BASED import/export tables, NOT vmMain syscall dispatch.
// Engine calls: GetCGameAPI(CGAME_API_VERSION, &cgame_import_t)
// DLL returns:  cgame_export_t* (12 function pointer slots)
//
// Verified from cgamex86.dll:
//   GetCGameAPI @ 0x30029430 (populates cg_exportTable @ 0x300e8788)
//   CL_InitCGameDLL @ SoF2.exe:0x10005300 (populates import table @ DAT_301f5958)
//
// KEY DIFFERENCES FROM Q3A/JK2:
//   - NOT enum-based dispatch (that was JK2 MP / Q3A style)
//   - SOF2 SP does NOT export CG_KeyEvent/CG_MouseEvent/CG_EventHandling
//   - Slots [5]-[9] are CGlass physics vector management callbacks
//   - Slot [3] is CG_DrawInformation (loading screen), new vs Q3A
//   - 163 import entries vs Q3A's ~80 (terrain, METIS, culling, GP2, G2)
//
// See E:\SOF2\structs\cgame_import_t.h and cgame_export_t.h for full documentation.

#define	CMD_BACKUP			64
#define	CMD_MASK			(CMD_BACKUP - 1)

#define	MAX_ENTITIES_IN_SNAPSHOT	512

#define	SNAPFLAG_RATE_DELAYED		1
#define	SNAPFLAG_DROPPED_COMMANDS	2

#ifndef SNAPSHOT_S_DEFINED
#define SNAPSHOT_S_DEFINED
struct snapshot_s
{
	int				snapFlags;

	int				serverTime;

	byte			areamask[MAX_MAP_AREA_BYTES];

	int				cmdNum;
	playerState_t	ps;

	int				numEntities;
	entityState_t	entities[MAX_ENTITIES_IN_SNAPSHOT];

	int				numConfigstringChanges;
	int				configstringNum;

	int				numServerCommands;
	int				serverCommandSequence;
};

typedef snapshot_s snapshot_t;
#endif // SNAPSHOT_S_DEFINED

/*
==================================================================

cgame_import_t — 163 function pointers passed by engine to GetCGameAPI()
0x28c bytes total (163 × 4)

==================================================================
*/

#define	CGAME_API_VERSION	3

typedef struct {
	//============== General Services (slots 0-4) ==================

	void    (*Printf)(const char *msg);                             // +0x000 [slot  0] cgi_Printf
	void    (*DPrintf)(const char *fmt, ...);                       // +0x004 [slot  1] cgi_DPrintf
	void    (*DPrintf2)(const char *fmt, ...);                      // +0x008 [slot  2] cgi_DPrintf2 (same fn, alt verbosity)
	void    (*Sprintf)(char *dest, int size, const char *fmt, ...); // +0x00c [slot  3] cgi_sprintf
	NORETURN_PTR void (*Error)(int level, const char *fmt, ...);    // +0x010 [slot  4] cgi_Error

	//============== File System (slots 5-14) ==================

	int     (*FS_FOpenFile)(const char *path, fileHandle_t *handle, // +0x014 [slot  5] cgi_FS_FOpenFileByMode
				fsMode_t mode);
	int     (*FS_Read)(void *buf, int len, fileHandle_t handle);    // +0x018 [slot  6] cgi_FS_Read
	int     (*FS_Write)(const void *buf, int len, fileHandle_t handle); // +0x01c [slot  7] cgi_FS_Write
	void    (*FS_FCloseFile)(fileHandle_t handle);                  // +0x020 [slot  8] cgi_FS_FCloseFile
	long    (*FS_ReadFile)(const char *name, void **buf);           // +0x024 [slot  9] cgi_FS_ReadFile
	void    (*FS_FreeFile)(void *buf);                              // +0x028 [slot 10] cgi_FS_FreeFile
	int     (*FS_FileExists)(const char *name);                     // +0x02c [slot 11] cgi_FS_FileExists
	int     (*FS_GetFileList)(const char *path, const char *ext,    // +0x030 [slot 12] cgi_FS_ListFiles
				char *listbuf, int bufsize);
	void    (*FS_FreeFileList)(char **list);                        // +0x034 [slot 13] cgi_FS_FreeFileList
	void    (*FS_CleanPath)(char *path);                            // +0x038 [slot 14] cgi_FS_CleanPath

	//============== Time / Console (slots 15-19) ==================

	int     (*Com_EventLoop)(void);                                 // +0x03c [slot 15] cgi_Com_EventLoop (returns time)
	void    (*Cmd_TokenizeString)(const char *text);                // +0x040 [slot 16] cgi_Cmd_TokenizeString
	char   *(*COM_Parse)(char **p);                                 // +0x044 [slot 17] cgi_COM_Parse
	void    (*Cbuf_AddText)(const char *text);                      // +0x048 [slot 18] cgi_Cbuf_AddText
	void    (*Cbuf_ExecuteText)(int when, const char *text);        // +0x04c [slot 19] cgi_Cbuf_ExecuteText

	//============== Command / Cvar (slots 20-31) ==================

	int     (*Cmd_Argc)(void);                                      // +0x050 [slot 20] cgi_Cmd_Argc
	void    (*Cmd_ArgvBuffer)(int n, char *buf, int bufsize);       // +0x054 [slot 21] cgi_Cmd_ArgvBuffer
	void    (*Cmd_ArgsBuffer)(int start, char *buf, int bufsize);   // +0x058 [slot 22] cgi_Cmd_ArgsBuffer
	cvar_t *(*Cvar_GetModified)(const char *name, const char *val,  // +0x05c [slot 23] cgi_Cvar_GetModified
				int flags);
	void    (*Cvar_Register)(vmCvar_t *cv, const char *name,        // +0x060 [slot 24] cgi_Cvar_Register
				const char *val, int flags);
	void    (*Cvar_Update)(vmCvar_t *cv);                           // +0x064 [slot 25] cgi_Cvar_Update
	void    (*Cvar_Set)(const char *name, const char *val);         // +0x068 [slot 26] cgi_Cvar_Set
	void    (*Cvar_SetModified)(const char *name);                  // +0x06c [slot 27] cgi_Cvar_SetModified
	void    (*Cvar_SetValue)(const char *name, float val);          // +0x070 [slot 28] cgi_Cvar_SetValue
	int     (*Cvar_VariableIntegerValue)(const char *name);         // +0x074 [slot 29] cgi_Cvar_VariableIntegerValue
	float   (*Cvar_VariableValue)(const char *name);                // +0x078 [slot 30] cgi_Cvar_VariableValue
	void    (*Cvar_VariableStringBuffer)(const char *name,          // +0x07c [slot 31] cgi_Cvar_VariableStringBuffer
				char *buf, int bufsize);

	//============== Memory (slots 32-34) ==================

	void   *(*Z_Malloc)(int size);                                  // +0x080 [slot 32] cgi_Z_Malloc
	void    (*Z_Free)(void *buf);                                   // +0x084 [slot 33] cgi_Z_Free
	void    (*Z_CheckHeap)(void);                                   // +0x088 [slot 34] cgi_Z_CheckHeap

	//============== Terrain System (slots 35-49) — SOF2-specific ==================

	void   *(*CM_RegisterTerrain)(const char *name, int flags);     // +0x08c [slot 35] cgi_CM_RegisterTerrain
	void    (*CM_Terrain_Release)(void *terrain);                   // +0x090 [slot 36] cgi_CM_Terrain_Release
	void    (*CM_Terrain_ForEachBrush)(void *terrain,               // +0x094 [slot 37] cgi_CM_Terrain_ForEachBrush
				void *callback, int userData);
	void   *reserved_38_RE_RegisterTerrain;                         // +0x098 [slot 38] RE_RegisterTerrain
	float   (*CM_Terrain_GetWorldHeight)(void *terrain,             // +0x09c [slot 39] cgi_CM_Terrain_GetWorldHeight
				void *pos, int minMax, int flags);
	void    (*CM_Terrain_FlattenHeight)(void *terrain,              // +0x0a0 [slot 40] cgi_CM_Terrain_FlattenHeight
				void *pos, void *params);
	void    (*RMG_RenderMinimap)(int terrainId);                    // +0x0a4 [slot 41] cgi_RMG_RenderMinimap
	void    (*RMG_SaveMinimap)(const char *mapName,                 // +0x0a8 [slot 42] cgi_RMG_SaveMinimap
				const char *prefix, const char *suffix);
	void    (*CM_Terrain_ApplyBezierPath)(void *terrain,            // +0x0ac [slot 43] cgi_CM_Terrain_ApplyBezierPath
				void *controlPoints, void *params);
	void    (*CTerrainInstanceList_Add)(void *list, void *inst);    // +0x0b0 [slot 44] cgi_CTerrainInstanceList_Add
	float   (*CM_Terrain_GetFlattenedAvgHeight)(void *terrain,      // +0x0b4 [slot 45] cgi_CM_Terrain_GetFlattenedAvgHeight
				void *pos, void *params);
	int     (*CM_Terrain_CheckOverlap)(void *terrain,               // +0x0b8 [slot 46] cgi_CM_Terrain_CheckOverlap
				void *bounds, int flags);
	void   *(*CTerrainInstanceList_GetFirst)(void *list);           // +0x0bc [slot 47] cgi_CTerrainInstanceList_GetFirst
	void   *(*CTerrainInstanceList_GetNext)(void *list,             // +0x0c0 [slot 48] cgi_CTerrainInstanceList_GetNext
				void *current);
	void    (*CM_Terrain_ApplyRadialOp)(void *terrain,              // +0x0c4 [slot 49] cgi_CM_Terrain_ApplyRadialOp
				void *params);

	//============== Renderer Image Loading (slots 50-52) ==================

	void   *(*R_LoadDataImage)(const char *name, int *width,        // +0x0c8 [slot 50] cgi_R_LoadDataImage
				int *height);
	void    (*ResampleTexture)(unsigned int *in, int inWidth,       // +0x0cc [slot 51] cgi_ResampleTexture
				int inHeight, unsigned int *out, int outWidth, int outHeight);
	void    (*R_FlipImageVertically)(unsigned char *img,            // +0x0d0 [slot 52] cgi_R_FlipImageVertically
				int w, int h, int bytes);

	//============== Frustum / Culling (slots 53-56) — SOF2-specific ==================

	void    (*R_SetupFrustum)(float *frustum, float fov_x,          // +0x0d4 [slot 53] cgi_R_SetupFrustum
				float fov_y, float *origin, float *axis);
	int     (*R_CullBox)(float *frustum, float *bounds);            // +0x0d8 [slot 54] cgi_R_CullBox
	int     (*R_CullPointAndRadius)(float *pt, float radius);       // +0x0dc [slot 55] cgi_R_CullPointAndRadius
	int     (*R_CullLocalBox)(float *frustum, float *origin,        // +0x0e0 [slot 56] cgi_R_CullLocalBox
				float *axis, float *bounds);

	//============== Screen / METIS UI Dispatch (slots 57-65) ==================

	void    (*Com_WriteCam)(const char *fmt, ...);                  // +0x0e4 [slot 57] cgi_Com_WriteCam
	void    (*UpdateScreen)(void);                                  // +0x0e8 [slot 58] cgi_SCR_UpdateScreen
	void    (*CL_UI_GetGameState)(void);                            // +0x0ec [slot 59] cgi_CL_UI_GetGameState
	void    (*CL_UI_GetConfigString)(void);                         // +0x0f0 [slot 60] cgi_CL_UI_GetConfigString
	void    (*CL_UI_GetServerStartTime)(void);                      // +0x0f4 [slot 61] cgi_CL_UI_GetServerStartTime
	int     (*CL_UI_GetCurrentCmdNumber)(void);                     // +0x0f8 [slot 62] cgi_CL_UI_GetCurrentCmdNumber
	int     (*CL_UI_Milliseconds)(int param1);                      // +0x0fc [slot 63] cgi_CL_UI_Milliseconds
	int     (*CL_UI_RealTime)(void *qtime);                         // +0x100 [slot 64] cgi_CL_UI_RealTime
	void    (*CL_UI_SnapVector)(void);                              // +0x104 [slot 65] cgi_CL_UI_SnapVector

	//============== Collision / Clip Models (slots 66-73) ==================

	int     (*CM_PointContents)(const vec3_t point, int passEntityNum); // +0x108 [slot 66] cgi_CM_PointContents
	void    (*CL_CM_LoadMap)(const char *name, int clientLoad,      // +0x10c [slot 67] cgi_CL_CM_LoadMap
				int *checksum);
	int     (*CM_NumInlineModels)(void);                            // +0x110 [slot 68] cgi_CM_NumInlineModels
	clipHandle_t (*CM_InlineModel)(int index);                      // +0x114 [slot 69] cgi_CM_InlineModel
	clipHandle_t (*CM_TempBoxModel)(const vec3_t mins, const vec3_t maxs); // +0x118 [slot 70] cgi_CM_TempBoxModel
	int     (*CM_TransformedPointContents)(const vec3_t point,      // +0x11c [slot 71] cgi_CM_TransformedPointContents
				clipHandle_t model, const vec3_t origin, const vec3_t angles);
	void    (*CM_BoxTrace)(trace_t *results, const vec3_t start,    // +0x120 [slot 72] cgi_CM_BoxTrace
				const vec3_t end, const vec3_t mins, const vec3_t maxs,
				clipHandle_t model, int contentmask);
	void    (*CM_TransformedBoxTrace)(trace_t *results, const vec3_t start, // +0x124 [slot 73] cgi_CM_TransformedBoxTrace
				const vec3_t end, const vec3_t mins, const vec3_t maxs,
				clipHandle_t model, int contentmask,
				const vec3_t origin, const vec3_t angles);

	//============== Sound (slots 74-89) ==================

	void    (*S_StartSound)(const vec3_t origin, int entityNum,     // +0x128 [slot 74] cgi_S_StartSound
				int entChannel, sfxHandle_t sfxHandle);
	void    (*S_StartLocalSound)(sfxHandle_t sfxHandle, int entChannel); // +0x12c [slot 75] cgi_S_StartLocalSound
	void    (*S_ClearLoopingSounds)(int killall);                   // +0x130 [slot 76] cgi_S_ClearLoopingSounds
	void    (*S_AddLoopingSound)(int entityNum, const vec3_t origin, // +0x134 [slot 77] cgi_S_AddLoopingSound
				const vec3_t velocity, sfxHandle_t sfxHandle);
	void    (*S_StopLoopingSound)(int entityNum);                   // +0x138 [slot 78] cgi_S_StopLoopingSound
	void    (*S_Respatialize)(int entityNum, const vec3_t head,     // +0x13c [slot 79] cgi_S_Respatialize
				vec3_t axis[3], int inwater);
	sfxHandle_t (*S_RegisterSound)(const char *name, int compressed, // +0x140 [slot 80] cgi_S_RegisterSound
				int streamed);
	void    (*S_UpdateEntityPosition)(int entityNum, const vec3_t origin); // +0x144 [slot 81] cgi_S_UpdateEntityPosition
	int     (*S_MuteSound)(int entityNum, sfxHandle_t sfxHandle);   // +0x148 [slot 82] cgi_S_MuteSound
	void    (*S_StartBackgroundTrack)(const char *intro,            // +0x14c [slot 83] cgi_S_StartBackgroundTrack
				const char *loop, int fadeupTime);
	void    (*S_StopBackgroundTrack)(void);                         // +0x150 [slot 84] cgi_S_StopBackgroundTrack
	void    (*S_UpdateAmbientSet)(const char *name, const vec3_t origin); // +0x154 [slot 85] cgi_S_UpdateAmbientSet
	int     (*S_AddLocalSet)(const char *name,                      // +0x158 [slot 86] cgi_S_AddLocalSet
				const vec3_t listener_origin, const vec3_t origin,
				int entID, int time);
	void    (*AS_ParseSets)(const char *filename);                  // +0x15c [slot 87] cgi_AS_ParseSets
	void    (*AS_AddPrecacheEntry)(const char *name);               // +0x160 [slot 88] cgi_AS_AddPrecacheEntry
	const char *(*AS_GetBModelSound)(const char *name, int index);  // +0x164 [slot 89] cgi_AS_GetBModelSound

	//============== Snapshot / Gamestate (slots 90-99) ==================

	void    (*CL_GetGlconfig)(glconfig_t *glconfig);                // +0x168 [slot 90] cgi_CL_GetGlconfig
	void    (*CL_GetGameState)(gameState_t *gs);                    // +0x16c [slot 91] cgi_CL_GetGameState
	void    (*CL_AddCgameCommand)(const char *name);                // +0x170 [slot 92] cgi_CL_AddCgameCommand
	void    (*CL_AddReliableCommand)(const char *cmd);              // +0x174 [slot 93] cgi_CL_AddReliableCommand
	int     (*CL_GetCurrentSnapshotNumber)(int *snapshotNumber,     // +0x178 [slot 94] cgi_CL_GetCurrentSnapshotNumber
				int *serverTime);
	qboolean (*CL_GetSnapshot)(int snapshotNumber, snapshot_t *snapshot); // +0x17c [slot 95] cgi_CL_GetSnapshot
	void    (*CL_GetEntityBaseline)(int entityNum, entityState_t *state); // +0x180 [slot 96] cgi_CL_GetEntityBaseline
	int     (*CL_GetServerCommand)(int serverCommandNumber);        // +0x184 [slot 97] cgi_CL_GetServerCommand
	int     (*CL_GetCurrentCmdNumber)(void);                        // +0x188 [slot 98] cgi_CL_GetCurrentCmdNumber
	qboolean (*CL_GetUserCmd)(int cmdNumber, usercmd_t *ucmd);      // +0x18c [slot 99] cgi_CL_GetUserCmd

	//============== Input / State (slots 100-115) ==================

	int     (*CL_SetClientViewAngles)(const vec3_t angles);         // +0x190 [slot100] cgi_CL_SetClientViewAngles
	void    (*CL_GetMouseDir)(int *xDir, int *yDir);                // +0x194 [slot101] cgi_CL_GetMouseDir (SOF2-specific)
	void    (*CL_SetUserCmdValue)(int userCmdValue,                 // +0x198 [slot102] cgi_CL_SetUserCmdValue
				float sensitivityScale);
	void   *(*CM_GetDebugEntry)(int index);                         // +0x19c [slot103] cgi_CM_GetDebugEntry
	void    (*CM_CleanDebugEntries)(void);                          // +0x1a0 [slot104] cgi_CM_CleanDebugEntries
	void    (*GP_GetBaseParseGroup)(int handle);                    // +0x1a4 [slot105] cgi_GP_GetBaseParseGroup
	int     (*GP_GetSubGroups)(void);                               // +0x1a8 [slot106] cgi_GP_GetSubGroups
	void    (*GP_GetPairs)(void *dest);                             // +0x1ac [slot107] cgi_GP_GetPairs
	void    (*GP_GetNext)(void *dest);                              // +0x1b0 [slot108] cgi_GP_GetNext
	int     (*CL_Key_IsDown)(int keynum);                           // +0x1b4 [slot109] cgi_CL_Key_IsDown
	int     (*Key_GetCatcher)(void);                                // +0x1b8 [slot110] cgi_Key_GetCatcher
	void    (*Key_SetCatcher)(int catcher);                         // +0x1bc [slot111] cgi_Key_SetCatcher
	char   *(*SE_DisplayString)(const char *token, int flags);      // +0x1c0 [slot112] cgi_SE_DisplayString
	char   *(*SE_GetString)(const char *token, int flags);          // +0x1c4 [slot113] cgi_SE_GetString
	int     (*SE_GetStringIndex)(const char *token);                // +0x1c8 [slot114] cgi_SE_GetStringIndex
	void    (*CL_SetLastBoneIndex)(int boneIndexBits);              // +0x1cc [slot115] cgi_CL_SetLastBoneIndex

	//============== Ghoul2 (slots 116-123) ==================

	void    (*G2_InitWraithBoneMapSingleton)(void);                 // +0x1d0 [slot116] cgi_G2_InitWraithBoneMapSingleton
	qboolean (*G2API_HaveWeGhoul2Models)(CGhoul2Info_v &ghoul2);    // +0x1d4 [slot117] cgi_G2API_HaveWeGhoul2Models
	CGhoul2Info_v *(*G2_GetGhoul2InfoByHandle)(int handle);         // +0x1d8 [slot118] cgi_G2_GetGhoul2InfoByHandle
	void    (*G2API_SetGhoul2ModelIndexes)(unsigned int ghoul2Handle, // +0x1dc [slot119] cgi_G2API_SetGhoul2ModelIndexes
				qhandle_t *modelList);
	void    (*G2API_ReRegisterModels)(unsigned int ghoul2Handle);   // +0x1e0 [slot120] cgi_G2API_ReRegisterModels
	qboolean (*G2API_GetBoltMatrix)(unsigned int ghoul2Handle,      // +0x1e4 [slot121] cgi_G2API_GetBoltMatrix
				int modelIndex, int boltIndex, mdxaBone_t *matrix,
				const vec3_t angles, const vec3_t position, int frameNum,
				qhandle_t *modelList, float scale);
	qboolean (*G2API_SetBoneAnglesOffset)(unsigned int ghoul2Handle, // +0x1e8 [slot122] cgi_G2API_SetBoneAnglesOffset
				int modelIndex, const char *boneName, const vec3_t angles,
				int flags, int up, int right, int forward,
				qhandle_t *modelList);
	void    (*G2API_CleanGhoul2Models)(CGhoul2Info_v &ghoul2);      // +0x1ec [slot123] cgi_G2API_CleanGhoul2Models

	//============== Renderer Misc (slots 124-126) ==================

	void    (*RE_MarkFragments)(const vec3_t origin, const vec3_t projection, // +0x1f0 [slot124] cgi_RE_MarkFragments
				int maxPoints, float *pointBuffer,
				int maxFragments, markFragment_t *fragmentBuffer);
	void    (*CL_CM_SelectSubBSP)(int index);                       // +0x1f4 [slot125] cgi_CL_CM_SelectSubBSP
	void    (*R_GetLighting)(const vec3_t pos, float *ambient,      // +0x1f8 [slot126] cgi_RE_GetLighting
				float *directed, vec3_t dir);

	//============== Renderer Registration (slots 127-131) ==================

	void    (*R_BeginRegistration)(glconfig_t *glconfig);           // +0x1fc [slot127] R_BeginRegistration
	qhandle_t (*R_RegisterModel)(const char *name);                 // +0x200 [slot128] RE_RegisterModel
	qhandle_t (*R_RegisterShader)(const char *name);                // +0x204 [slot129] RE_RegisterShader
	qhandle_t (*R_RegisterShaderNoMip)(const char *name);           // +0x208 [slot130] RE_RegisterShaderNoMip
	qhandle_t (*R_RegisterSkin)(const char *name);                  // +0x20c [slot131] RE_RegisterSkin

	//============== Renderer Scene (slots 132-146) ==================

	void    (*R_ClearScene)(void);                                  // +0x210 [slot132] R_ClearScene
	void    (*R_AddRefEntityToScene)(const refEntity_t *ent);       // +0x214 [slot133] RE_AddRefEntityToScene
	void    (*R_AddMiniRefEntityToScene)(const refEntity_t *ent);   // +0x218 [slot134] RE_AddMiniRefEntityToScene (SOF2)
	void    (*R_AddPolyToScene)(qhandle_t hShader, int numVerts,    // +0x21c [slot135] RE_AddPolyToScene
				const polyVert_t *verts, int num);
	void    (*R_AddLightToScene)(const vec3_t org, float intensity, // +0x220 [slot136] RE_AddLightToScene
				float r, float g, float b);
	void    (*R_AddDirectedLightToScene)(const vec3_t dir, float intensity, // +0x224 [slot137] RE_AddDirectedLightToScene
				float r, float g, float b);
	void    (*R_AddAdditiveLightToScene)(const vec3_t org, float intensity, // +0x228 [slot138] RE_AddAdditiveLightToScene
				float r, float g, float b);
	void    (*R_RenderScene)(const refdef_t *fd);                   // +0x22c [slot139] RE_RenderScene
	void    (*R_SetColor)(const float *rgba);                       // +0x230 [slot140] RE_SetColor
	void    (*R_DrawStretchPic)(float x, float y, float w, float h, // +0x234 [slot141] RE_StretchPic
				float s1, float t1, float s2, float t2, qhandle_t hShader);
	void    (*R_FillRect)(float x, float y, float w, float h,       // +0x238 [slot142] RE_FillRect (SOF2-specific)
				const float *color);
	int     (*R_LerpTag)(orientation_t *tag, qhandle_t handle,      // +0x23c [slot143] RE_LerpTag
				int startFrame, int endFrame, float frac, const char *tagName);
	void    (*R_ModelBounds)(qhandle_t handle, vec3_t mins, vec3_t maxs); // +0x240 [slot144] RE_ModelBounds
	void    (*R_WorldEffectCommand)(const char *command);           // +0x244 [slot145] RE_WorldEffectCommand (weather)
	void    (*R_GetModelBounds)(const refEntity_t *ent, vec3_t mins, // +0x248 [slot146] RE_GetModelBounds
				vec3_t maxs);

	//============== Renderer Font & Light Style (slots 147-153) ==================

	void    (*R_RegisterFont)(const char *name, int pointSize,      // +0x24c [slot147] RE_RegisterFont
				void *font, int unk);
	int     (*R_DamageSurface)(const refEntity_t *ent);             // +0x250 [slot148] RE_DamageSurface
	float   (*R_Font_StrLenPixels)(const void *font,                // +0x254 [slot149] RE_Font_StrLenPixels
				const char *text, float scale);
	float   (*R_Font_HeightPixels)(const void *font, float scale);  // +0x258 [slot150] RE_Font_HeightPixels
	void    (*R_Font_DrawString)(const void *font, const char *text, // +0x25c [slot151] RE_Font_DrawString
				float x, float y, float scale, const float *rgba, int style);
	int     (*R_GetLightStyle)(int index, float *rgba);             // +0x260 [slot152] RE_GetLightStyle
	void    (*R_SetLightStyle)(int index, float r, float g, float b); // +0x264 [slot153] RE_SetLightStyle

	//============== Renderer Extended (slots 154-162) ==================

	int     (*CL_GetConfigStringToken)(char *buf, int bufsize);     // +0x268 [slot154] cgi_CL_GetConfigStringToken
	int     (*CL_UIActive)(int activeMode, int *patchMapOut,        // +0x26c [slot155] cgi_CL_UIActive
				int fontParam, int *cinematicBufOut);
	int     (*RE_G2_GetAnimMap)(const char *anim, int *frames);     // +0x270 [slot156] RE_G2_GetAnimMap
	int     (*RE_GetModelBounds2)(qhandle_t handle, float *bounds); // +0x274 [slot157] RE_GetModelBounds2
	void    (*RE_SetSunState)(int state, const vec3_t dir,          // +0x278 [slot158] RE_SetSunState
				float r, float g, float b);
	int     (*RE_ValidateVideoMode)(int width, int height, int hz,  // +0x27c [slot159] RE_ValidateVideoMode
				int fullscreen);
	void    (*RE_DrawBox)(float x, float y, float w, float h,       // +0x280 [slot160] RE_DrawBox
				const float *color, float alpha);
	int     (*ICARUS_GetVector)(void);                              // +0x284 [slot161] cgi_ICARUS_GetVector
	int     (*ICARUS_GetString)(void);                              // +0x288 [slot162] cgi_ICARUS_GetString

} cgame_import_t;  // 0x28c bytes total (163 pointers × 4 bytes)

/*
==================================================================

cgame_export_t — 12 function pointers returned by GetCGameAPI()
0x30 bytes total (12 × 4)

CRITICAL: Slots [5]-[9] are NOT Q3A-style input callbacks.
They are CGlass physics vector management callbacks (std::vector resize/insert).
SOF2 SP does NOT export CG_KeyEvent/CG_MouseEvent/CG_EventHandling.

==================================================================
*/

typedef struct {
	// --- Core callbacks (slots 0-4) ---
	void    (*Init)(int serverMessageNum, int serverCommandSequence, int clientNum);
	                                                         // +0x00 [0]  CG_Init
	void    (*Shutdown)(void);                               // +0x04 [1]  CG_Shutdown
	int     (*ConsoleCommand)(void);                         // +0x08 [2]  CG_ConsoleCommand
	void    (*DrawInformation)(void);                        // +0x0c [3]  CG_DrawInformation (loading screen)
	void    (*DrawActiveFrame)(int serverTime, int stereoView, int demoPlayback);
	                                                         // +0x10 [4]  CG_DrawActiveFrame

	// --- CGlass physics vector callbacks (slots 5-9) ---
	// C++ std::vector<T> insert/resize specializations for glass physics types.
	// Engine calls these to manage glass physics data buffers shared with cgame DLL.
	void    (*GlassBoltVec_Insert)(void *vec, int count);    // +0x14 [5]  CGlassBoltVec_Insert (0x3c bytes/elem)
	void    (*GlassInstVec_Resize)(void *vec, unsigned int newSize); // +0x18 [6]  CGlassInstVec_Resize (0xd0 bytes/elem)
	void    (*GlassPhysVec_Insert)(void *vec, unsigned int newSize); // +0x1c [7]  CGlassPhysVec_Insert (0x274 bytes/elem)
	void    (*GlassBoltList_Resize)(void *vec, unsigned int newSize); // +0x20 [8]  GlassBoltList_Resize (0x18 bytes/elem)
	void    (*GlassConstraintVec_Insert)(void *vec, unsigned int capacity); // +0x24 [9]  CGlassConstraintVec_Insert (0x30 bytes/elem)

	// --- Raven-specific query/misc (slots 10-11) ---
	int     (*GetCameraMode)(void);                          // +0x28 [10] CG_GetCameraMode (returns cg_cameraMode)
	void    (*DisableCamera)(void);                          // +0x2c [11] CG_DisableCamera (calls CG_CamDisable)
} cgame_export_t;  // 0x30 bytes total (12 function pointers)

cgame_export_t *GetCGameAPI (int version, cgame_import_t *import);

#endif // _SOF2_CG_PUBLIC_H

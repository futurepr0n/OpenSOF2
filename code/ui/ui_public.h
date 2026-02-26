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

#ifndef __UI_PUBLIC_H__
#define __UI_PUBLIC_H__
//
// ui_public.h — SOF2 SP UI/Menus DLL interface, UI_API_VERSION 2
//
// The SOF2 UI DLL (Menusx86.dll) uses STRUCT-BASED import/export tables.
// Engine calls: GetUIAPI(UI_API_VERSION, &menu_import_t)
// DLL returns:  uiExport_t* (18 function pointer slots)
//
// Verified from Menusx86.dll:
//   GetUIAPI @ 0x40023830 (populates export table @ 0x40037590)
//   CL_InitUI @ SoF2.exe:0x10010a40 (builds 102-entry import table on stack)
//
// KEY DIFFERENCES FROM JK2/Q3A:
//   - NOT enum-based dispatch (JK2 MP used enum uiImport_t)
//   - version 2, not 3
//   - Slots [39]-[43] are DATA VALUES/POINTERS, not function pointers:
//     [39] vidWidth (int), [40] vidHeight (int),
//     [41] &keyCatchers (int*), [42] &clsState (int*), [43] &keys_array (void*)
//   - SOF2 additions: Q_irand, Q_flrand, SE_GetString, ICARUS_RunScript, etc.
//   - METIS widget framework (.rmf files), not Q3A's menu system
//
// See E:\SOF2\structs\menu_import_t.h and uiExport_t.h for full documentation.

#include "../client/keycodes.h"

#define UI_API_VERSION	2

/*
==================================================================

menu_import_t — 102 entries passed by engine to GetUIAPI()
0x198 bytes total (102 × 4)

==================================================================
*/

typedef struct {
	//============== General (slots 0-4) ==================

	void    (*Printf)(const char *msg, ...);                        // +0x000 [slot  0] Com_Printf
	void    (*DPrintf)(const char *fmt, ...);                       // +0x004 [slot  1] Com_DPrintf
	void    (*DPrintf2)(const char *fmt, ...);                      // +0x008 [slot  2] Com_DPrintf (duplicate)
	void    (*Sprintf)(char *dest, int size, const char *fmt, ...); // +0x00c [slot  3] Com_sprintf
	NORETURN_PTR void (*Error)(int level, const char *fmt, ...);    // +0x010 [slot  4] Com_Error

	//============== File System (slots 5-15) ==================

	int     (*FS_FOpenFileByMode)(const char *path, fileHandle_t *fh, // +0x014 [slot  5] FS_FOpenFileByMode
				fsMode_t mode);
	int     (*FS_Read)(void *buf, int len, fileHandle_t fh);        // +0x018 [slot  6] FS_Read
	int     (*FS_Write)(const void *buf, int len, fileHandle_t fh); // +0x01c [slot  7] FS_Write
	void    (*FS_FCloseFile)(fileHandle_t fh);                      // +0x020 [slot  8] FS_FCloseFile
	long    (*FS_ReadFile)(const char *name, void **buf);           // +0x024 [slot  9] FS_ReadFile
	void    (*FS_FreeFile)(void *buf);                              // +0x028 [slot 10] FS_FreeFile
	qboolean (*FS_FileExists)(const char *name);                    // +0x02c [slot 11] FS_FileExists
	int     (*FS_GetFileList)(const char *path, const char *ext,    // +0x030 [slot 12] FS_ListFiles
				char *listbuf, int bufsize);
	void   *reserved_13;                                            // +0x034 [slot 13] unknown FS variant
	void    (*FS_FreeFileList)(char **list);                        // +0x038 [slot 14] FS_FreeFileList
	void    (*FS_CleanPath)(char *path);                            // +0x03c [slot 15] FS_CleanPath

	//============== Time / Math (slots 16-20) ==================

	int     (*Milliseconds)(void);                                  // +0x040 [slot 16] Sys_Milliseconds
	int     (*Q_irand)(int min, int max);                           // +0x044 [slot 17] Q_irand
	float   (*Q_flrand)(float min, float max);                      // +0x048 [slot 18] Q_flrand
	void    (*Cmd_TokenizeString)(const char *text);                // +0x04c [slot 19] Cmd_TokenizeString
	char   *(*COM_Parse)(char **text);                              // +0x050 [slot 20] COM_Parse

	//============== Console / Command (slots 21-27) ==================

	void    (*Cbuf_AddText)(const char *text);                      // +0x054 [slot 21] Cbuf_AddText
	void    (*Cbuf_ExecuteText)(int when, const char *text);        // +0x058 [slot 22] Cbuf_ExecuteText
	void    (*Cmd_AddCommand)(const char *name, xcommand_t fn);     // +0x05c [slot 23] Cmd_AddCommand
	void    (*Cmd_RemoveCommand)(const char *name);                 // +0x060 [slot 24] Cmd_RemoveCommand
	int     (*Cmd_Argc)(void);                                      // +0x064 [slot 25] Cmd_Argc
	void    (*Cmd_ArgvBuffer)(int n, char *buf, int bufsize);       // +0x068 [slot 26] Cmd_ArgvBuffer
	void    (*Cmd_ArgsBuffer)(char *buf, int bufsize);              // +0x06c [slot 27] Cmd_ArgsBuffer

	//============== Cvar (slots 28-35) ==================

	cvar_t *(*Cvar_Get)(const char *name, const char *val,          // +0x070 [slot 28] Cvar_Get
				int flags);
	void    (*Cvar_Set)(const char *name, const char *val);         // +0x074 [slot 29] Cvar_Set
	void    (*Cvar_SetModified)(const char *name, qboolean mod);    // +0x078 [slot 30] Cvar_SetModified
	void    (*Cvar_SetValue)(const char *name, float val);          // +0x07c [slot 31] Cvar_SetValue
	int     (*Cvar_VariableIntegerValue)(const char *name);         // +0x080 [slot 32] Cvar_VariableIntegerValue
	float   (*Cvar_VariableValue)(const char *name);                // +0x084 [slot 33] Cvar_VariableValue
	void    (*Cvar_VariableStringBuffer)(const char *name,          // +0x088 [slot 34] Cvar_VariableStringBuffer
				char *buf, int bufsize);
	void    (*Cvar_SetInternalAllowed)(const char *name, int val);  // +0x08c [slot 35] Cvar_SetInternalAllowed (Raven)

	//============== Memory (slots 36-38) ==================

	void   *(*Z_Malloc)(int size);                                  // +0x090 [slot 36] Z_Malloc
	void    (*Z_FreeTags)(int tag);                                 // +0x094 [slot 37] Z_FreeTags
	void    (*Z_Free)(void *buf);                                   // +0x098 [slot 38] Z_Free

	//============== Data Fields (slots 39-43) ==================
	// IMPORTANT: These are DATA VALUES/POINTERS, not function pointers.
	// The engine copies actual values/addresses here; the UI DLL reads them directly.

	int     vidWidth;                                               // +0x09c [slot 39] cls.glConfig.vidWidth
	int     vidHeight;                                              // +0x0a0 [slot 40] con.displayHeight
	int    *keyCatchers;                                            // +0x0a4 [slot 41] &cls_keyCatchers
	int    *clsState;                                               // +0x0a8 [slot 42] &cls_state
	void   *keys_array;                                             // +0x0ac [slot 43] &keys_down[]

	//============== Utility / String / Key (slots 44-53) ==================

	char   *(*Cvar_InfoString)(int flags);                          // +0x0b0 [slot 44] Cvar_InfoString
	void    (*COM_StripFilename)(const char *in, char *out);        // +0x0b4 [slot 45] COM_StripFilename
	void   *reserved_46;                                            // +0x0b8 [slot 46] COM_GetNextToken variant
	void    (*COM_StripExtension)(const char *in, char *out);       // +0x0bc [slot 47] COM_StripExtension
	int     (*Com_Clampi)(int min, int max, int val);               // +0x0c0 [slot 48] Com_Clampi
	float   (*Com_Clamp)(float min, float max, float val);          // +0x0c4 [slot 49] Com_Clamp
	char   *(*va)(const char *fmt, ...);                            // +0x0c8 [slot 50] va
	char   *(*Info_ValueForKey)(const char *s, const char *key);    // +0x0cc [slot 51] Info_ValueForKey
	void    (*Info_Print)(const char *s);                           // +0x0d0 [slot 52] Info_Print
	int     (*Key_StringToKeynum)(const char *str);                 // +0x0d4 [slot 53] Key_StringToKeynum

	//============== Key System (slots 54-65) ==================

	char   *(*Key_KeynumToString)(int keynum);                      // +0x0d8 [slot 54] Key_KeynumToString
	int     (*Key_IsDown)(int keynum);                              // +0x0dc [slot 55] Key_IsDown
	void   *reserved_56;                                            // +0x0e0 [slot 56] Key_GetBinding variant
	int     (*Key_GetKey)(const char *binding);                     // +0x0e4 [slot 57] Key_GetKey
	void   *reserved_58;                                            // +0x0e8 [slot 58] Key_GetClipboardData variant
	void    (*Key_ClearStates)(void);                               // +0x0ec [slot 59] Key_ClearStates
	void   *reserved_60;                                            // +0x0f0 [slot 60] Key_SetBinding variant
	void   *reserved_61;                                            // +0x0f4 [slot 61] unknown
	void   *reserved_62;                                            // +0x0f8 [slot 62] unknown
	char   *(*SP_GetStringTextByIndex)(int idx);                    // +0x0fc [slot 63] SP_GetStringTextByIndex
	char   *(*SE_GetString)(const char *token, int flags);          // +0x100 [slot 64] SE_GetString
	char   *(*SE_GetString2)(const char *token, int flags);         // +0x104 [slot 65] SE_GetString (duplicate)

	//============== Client / G2 (slots 66-74) ==================

	void   *reserved_66;                                            // +0x108 [slot 66] Key_GetCatcher variant
	void   *reserved_67;                                            // +0x10c [slot 67] Key_SetCatcher variant
	void   *reserved_68;                                            // +0x110 [slot 68] G2API_CleanGhoul2Models variant
	void   *(*G2_SpawnGhoul2Model)(void);                           // +0x114 [slot 69] G2_SpawnGhoul2Model
	void    (*G2_PlayModelAnimation)(void *model, const char *anim, // +0x118 [slot 70] G2_PlayModelAnimation
				int frame, int flags, int speed);
	void   *reserved_71;                                            // +0x11c [slot 71] R_RegisterModel variant
	void   *reserved_72;                                            // +0x120 [slot 72] R_AddRefEntityToScene variant
	void   *reserved_73;                                            // +0x124 [slot 73] R_ClearScene variant
	int     (*CL_IsCgameActive)(void);                              // +0x128 [slot 74] CL_IsCgameActive

	//============== Save Game (slots 75-77) ==================

	void   *reserved_75;                                            // +0x12c [slot 75] SV_LoadGame variant
	void   *reserved_76;                                            // +0x130 [slot 76] SV_SaveGame variant
	void    (*SV_WipeSavegame)(const char *name);                   // +0x134 [slot 77] SV_WipeSavegame

	//============== Sound Module Vtable (slots 78-83) ==================

	void   *S_MuteSound;            // +0x138 [slot 78] snd+0x14 (S_MuteSound)
	void   *S_StartLocalSound;      // +0x13c [slot 79] snd+0x38 (S_StartLocalSound)
	void   *S_StopAllSounds;        // +0x140 [slot 80] snd+0x1c (S_StopAllSounds)
	void   *S_reserved_81;          // +0x144 [slot 81] snd+0x64
	void   *S_reserved_82;          // +0x148 [slot 82] snd+0x24
	void   *S_reserved_83;          // +0x14c [slot 83] snd+0x28

	//============== Renderer Vtable (slots 84-99) ==================

	void   *RE_reserved_84;         // +0x150 [slot 84] re+0x80
	void   *RE_reserved_85;         // +0x154 [slot 85] re+0x84
	void   *RE_DamageSurface;       // +0x158 [slot 86] re+0x88 (RE_DamageSurface)
	void   *RE_reserved_87;         // +0x15c [slot 87] re+0x8c
	void   *RE_reserved_88;         // +0x160 [slot 88] re+0x90
	void   *RE_reserved_89;         // +0x164 [slot 89] re+0x94
	void   *RE_LerpTag;             // +0x168 [slot 90] re+0x58 (RE_LerpTag)
	void   *RE_ModelBounds;         // +0x16c [slot 91] re+0x5c (RE_ModelBounds)
	void   *RE_reserved_92;         // +0x170 [slot 92] re+0x60
	void    (*CL_SaveGameScreenshot)(void); // +0x174 [slot 93] CL_SaveGameScreenshot
	void   *RE_RegisterShaderNoMip; // +0x178 [slot 94] re+0x14 (RE_RegisterShaderNoMip)
	void   *RE_reserved_95;         // +0x17c [slot 95] re+0x98
	void   *RE_reserved_96;         // +0x180 [slot 96] re+0x9c
	void   *RE_reserved_97;         // +0x184 [slot 97] re+0xa0
	void   *RE_reserved_98;         // +0x188 [slot 98] re+0xa8 (RE_WorldEffectCommand variant)
	void   *RE_reserved_99;         // +0x18c [slot 99] re+0xac

	//============== ICARUS Scripting (slots 100-101) ==================

	void    (*ICARUS_SignalEvent)(int entID, const char *name);     // +0x190 [slot100] ICARUS_SignalEvent
	int     (*ICARUS_RunScript)(void *entity, const char *name);    // +0x194 [slot101] ICARUS_RunScript

} menu_import_t;  // 0x198 bytes total (102 dwords × 4 bytes)

/*
==================================================================

uiExport_t — 18 function pointers returned by GetUIAPI()
0x48 bytes total (18 × 4)

==================================================================
*/

typedef struct {
	void    (*UI_Init)(void);                                           // +0x00 [0]
	void    (*UI_Shutdown)(void);                                       // +0x04 [1]
	int     (*UI_IsFullscreen)(void);                                  // +0x08 [2] returns bool, no args
	void   *(*slot3)(void);                                            // +0x0c [3] unknown
	void    (*UI_SetActiveMenu)(char *menuName, char *cfgFile, byte flags); // +0x10 [4]
	void   *(*slot5)(void);                                            // +0x14 [5]
	void    (*UI_Refresh)(void);                                       // +0x18 [6]
	void    (*UI_ConsoleCommand)(char toGame);                         // +0x1c [7]
	void   *(*slot8)(void);                                            // +0x20 [8]
	void    (*UI_SetHudString)(char *element, char *value);           // +0x24 [9]
	void    (*UI_SetHudImage)(char *element, char *shaderName);       // +0x28 [10]
	void   *(*slot11)(void);                                           // +0x2c [11]
	void   *(*slot12)(void);                                           // +0x30 [12]
	void   *(*slot13)(void);                                           // +0x34 [13]
	void   *(*slot14)(void);                                           // +0x38 [14]
	void    (*UI_KeyEvent)(int key, int down);                         // +0x3c [15] key event handler
	void    (*UI_MouseEvent)(int dx, int dy);                          // +0x40 [16] confirmed: calls UI_MenuSystem_MoveMouseClamped
	void    (*UI_RegisterSounds)(void);                                // +0x44 [17]
} uiExport_t;  // 0x48 bytes total (18 function pointers)

uiExport_t *GetUIAPI (int version, menu_import_t *import);

/*
==================================================================

Legacy JK2/OpenJK uiImport_t enum (syscall numbers used by cl_ui.cpp CL_UISystemCalls)
and uiimport_t / dpTypes_t named-field struct (used by cl_ui.cpp and code/ui/ helpers).

These are kept intact so the engine-side code/ui/ helpers and cl_ui.cpp compile.
They are NOT the SOF2 DLL interface (which uses menu_import_t / uiExport_t above).

==================================================================
*/

typedef enum {
	UI_ERROR,
	UI_PRINT,
	UI_MILLISECONDS,
	UI_CVAR_SET,
	UI_CVAR_VARIABLEVALUE,
	UI_CVAR_VARIABLESTRINGBUFFER,
	UI_CVAR_SETVALUE,
	UI_CVAR_RESET,
	UI_CVAR_CREATE,
	UI_CVAR_INFOSTRINGBUFFER,
	UI_ARGC,
	UI_ARGV,
	UI_CMD_EXECUTETEXT,
	UI_FS_FOPENFILE,
	UI_FS_READ,
	UI_FS_WRITE,
	UI_FS_FCLOSEFILE,
	UI_FS_GETFILELIST,
	UI_R_REGISTERMODEL,
	UI_R_REGISTERSKIN,
	UI_R_REGISTERSHADERNOMIP,
	UI_R_CLEARSCENE,
	UI_R_ADDREFENTITYTOSCENE,
	UI_R_ADDPOLYTOSCENE,
	UI_R_ADDLIGHTTOSCENE,
	UI_R_RENDERSCENE,
	UI_R_SETCOLOR,
	UI_R_DRAWSTRETCHPIC,
	UI_UPDATESCREEN,
	UI_CM_LERPTAG,
	UI_CM_LOADMODEL,
	UI_S_REGISTERSOUND,
	UI_S_STARTLOCALSOUND,
	UI_KEY_KEYNUMTOSTRINGBUF,
	UI_KEY_GETBINDINGBUF,
	UI_KEY_SETBINDING,
	UI_KEY_ISDOWN,
	UI_KEY_GETOVERSTRIKEMODE,
	UI_KEY_SETOVERSTRIKEMODE,
	UI_KEY_CLEARSTATES,
	UI_KEY_GETCATCHER,
	UI_KEY_SETCATCHER,
	UI_GETCLIPBOARDDATA,
	UI_GETGLCONFIG,
	UI_GETCLIENTSTATE,
	UI_GETCONFIGSTRING,
	UI_LAN_GETPINGQUEUECOUNT,
	UI_LAN_CLEARPING,
	UI_LAN_GETPING,
	UI_LAN_GETPINGINFO,
	UI_CVAR_REGISTER,
	UI_CVAR_UPDATE,
	UI_MEMORY_REMAINING,
	UI_GET_CDKEY,
	UI_SET_CDKEY,
	UI_R_REGISTERFONT,
	UI_R_MODELBOUNDS,
	UI_PC_ADD_GLOBAL_DEFINE,
	UI_PC_LOAD_SOURCE,
	UI_PC_FREE_SOURCE,
	UI_PC_READ_TOKEN,
	UI_PC_SOURCE_FILE_AND_LINE,
	UI_S_STOPBACKGROUNDTRACK,
	UI_S_STARTBACKGROUNDTRACK,
	UI_REAL_TIME,
	UI_LAN_GETSERVERCOUNT,
	UI_LAN_GETSERVERADDRESSSTRING,
	UI_LAN_GETSERVERINFO,
	UI_LAN_MARKSERVERVISIBLE,
	UI_LAN_UPDATEVISIBLEPINGS,
	UI_LAN_RESETPINGS,
	UI_LAN_LOADCACHEDSERVERS,
	UI_LAN_SAVECACHEDSERVERS,
	UI_LAN_ADDSERVER,
	UI_LAN_REMOVESERVER,
	UI_CIN_PLAYCINEMATIC,
	UI_CIN_STOPCINEMATIC,
	UI_CIN_RUNCINEMATIC,
	UI_CIN_DRAWCINEMATIC,
	UI_CIN_SETEXTENTS,
	UI_R_REMAP_SHADER,
	UI_VERIFY_CDKEY,
	UI_LAN_SERVERSTATUS,
	UI_LAN_GETSERVERPING,
	UI_LAN_SERVERISVISIBLE,
	UI_LAN_COMPARESERVERS,
	UI_MEMSET = 100,
	UI_MEMCPY,
	UI_STRNCPY,
	UI_SIN,
	UI_COS,
	UI_ATAN2,
	UI_SQRT,
	UI_FLOOR,
	UI_CEIL
} uiImport_t;

/*
==================================================================

Legacy JK2-style uiimport_t / dpTypes_t
These are kept for the engine-side code/ui/ helpers (ui_atoms.cpp,
ui_syscalls.cpp, cl_ui.cpp) which use named fields and are compiled
directly into the SP engine binary.  They are NOT used by the SOF2
Menusx86.dll interface (which uses menu_import_t above).

==================================================================
*/

typedef enum {
	DP_HUD=0,
	DP_OBJECTIVES,
	DP_WEAPONS,
	DP_INVENTORY,
	DP_FORCEPOWERS
} dpTypes_t;

typedef struct {
	void		(*Printf)( const char *fmt, ... );
	NORETURN_PTR void	(*Error)( int level, const char *fmt, ... );

	void		(*Cvar_Set)( const char *name, const char *value );
	float		(*Cvar_VariableValue)( const char *var_name );
	void		(*Cvar_VariableStringBuffer)( const char *var_name, char *buffer, int bufsize );
	void		(*Cvar_SetValue)( const char *var_name, float value );
	void		(*Cvar_Reset)( const char *name );
	void		(*Cvar_Create)( const char *var_name, const char *var_value, int flags );
	void		(*Cvar_InfoStringBuffer)( int bit, char *buffer, int bufsize );

	int			(*Argc)( void );
	void		(*Argv)( int n, char *buffer, int bufferLength );
	void		(*Cmd_ExecuteText)( int exec_when, const char *text );
	void		(*Cmd_TokenizeString)( const char *text );

	int			(*FS_FOpenFile)( const char *qpath, fileHandle_t *file, fsMode_t mode );
	int 		(*FS_Read)( void *buffer, int len, fileHandle_t f );
	int 		(*FS_Write)( const void *buffer, int len, fileHandle_t f );
	void		(*FS_FCloseFile)( fileHandle_t f );
	int			(*FS_GetFileList)( const char *path, const char *extension, char *listbuf, int bufsize );
	long		(*FS_ReadFile)( const char *name, void **buf );
	void		(*FS_FreeFile)( void *buf );

	qhandle_t	(*R_RegisterModel)( const char *name );
	qhandle_t	(*R_RegisterSkin)( const char *name );
	qhandle_t	(*R_RegisterShader)( const char *name );
	qhandle_t	(*R_RegisterShaderNoMip)( const char *name );
	qhandle_t	(*R_RegisterFont)( const char *name );

	int			(*R_Font_StrLenPixels)(const char *text, const int setIndex, const float scale );
	int			(*R_Font_HeightPixels)(const int setIndex, const float scale );
	void		(*R_Font_DrawString)(int ox, int oy, const char *text, const float *rgba, const int setIndex, int iMaxPixelWidth, const float scale );
	int			(*R_Font_StrLenChars)(const char *text);
	qboolean	(*Language_IsAsian) (void);
	qboolean	(*Language_UsesSpaces) (void);
	unsigned int (*AnyLanguage_ReadCharFromString)( char *psText, int *piAdvanceCount, qboolean *pbIsTrailingPunctuation );

	void		(*R_ClearScene)( void );
	void		(*R_AddRefEntityToScene)( const refEntity_t *re );
	void		(*R_AddPolyToScene)( qhandle_t hShader , int numVerts, const polyVert_t *verts );
	void		(*R_AddLightToScene)( const vec3_t org, float intensity, float r, float g, float b );
	void		(*R_RenderScene)( const refdef_t *fd );

	void		(*R_ModelBounds)( qhandle_t handle, vec3_t mins, vec3_t maxs );
	void		(*R_SetColor)( const float *rgba );
	void		(*R_DrawStretchPic)( float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t hShader );
	void		(*R_ScissorPic)( float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t hShader );

	void		(*UpdateScreen)( void );
	void		(*R_LerpTag)( orientation_t *tag, clipHandle_t mod, int startFrame, int endFrame, float frac, const char *tagName );

	void		(*S_StartLocalSound)( sfxHandle_t sfxHandle, int channelNum );
	sfxHandle_t	(*S_RegisterSound)( const char *name );
	void		(*S_StartLocalLoopingSound)( sfxHandle_t sfxHandle );
	void		(*S_StopSounds)( void );

	void		(*DrawStretchRaw)(int x, int y, int w, int h, int cols, int rows, const byte *data, int client, qboolean dirty);
	int			(*SG_GetSaveGameComment)(const char *psPathlessBaseName, char *sComment, char *sMapName);
	qboolean	(*SG_GameAllowedToSaveHere)(qboolean inCamera);
	void		(*SG_StoreSaveGameComment)(const char *sComment);

	void		(*Key_KeynumToStringBuf)( int keynum, char *buf, int buflen );
	void		(*Key_GetBindingBuf)( int keynum, char *buf, int buflen );
	void		(*Key_SetBinding)( int keynum, const char *binding );
	qboolean	(*Key_IsDown)( int keynum );
	qboolean	(*Key_GetOverstrikeMode)( void );
	void		(*Key_SetOverstrikeMode)( qboolean state );
	void		(*Key_ClearStates)( void );
	int			(*Key_GetCatcher)( void );
	void		(*Key_SetCatcher)( int catcher );

	void		(*GetClipboardData)( char *buf, int bufsize );
	void		(*GetGlconfig)( glconfig_t *config );
	connstate_t	(*GetClientState)( void );
	void		(*GetConfigString)( int index, char *buff, int buffsize );
	int			(*Milliseconds)( void );
	void		(*Draw_DataPad)(int HUDType);
} uiimport_t;

#endif // __UI_PUBLIC_H__

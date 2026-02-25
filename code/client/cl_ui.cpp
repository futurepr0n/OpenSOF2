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

#include "../server/exe_headers.h"

#include "client.h"
#include "client_ui.h"
#include "qcommon/stringed_ingame.h"
#include "../ui/ui_public.h"
#include "sys/sys_loadlib.h"

#include "vmachine.h"
#include <excpt.h>   // EXCEPTION_EXECUTE_HANDLER, GetExceptionCode
#include <windows.h> // HeapAlloc, HeapFree, GetProcessHeap

intptr_t CL_UISystemCalls( intptr_t *args );

//prototypes
#ifdef JK2_MODE
extern qboolean SG_GetSaveImage( const char *psPathlessBaseName, void *pvAddress );
#endif
extern int SG_GetSaveGameComment(const char *psPathlessBaseName, char *sComment, char *sMapName);
extern qboolean SG_GameAllowedToSaveHere(qboolean inCamera);
extern void SG_StoreSaveGameComment(const char *sComment);
extern byte *SCR_GetScreenshot(qboolean *qValid);						// uncommented --eez
extern void SG_WipeSavegame( const char *psPathlessBaseName );

// SOF2 UI DLL handle and export table.
// uie is extern so ui_main.cpp / ui_atoms.cpp can dispatch through it.
static void   *uiLibrary = NULL;
uiExport_t    *uie       = NULL;

// keyCatchers is now non-static in cl_keys.cpp so we can take its address
extern int     keyCatchers;
extern keyGlobals_t kg;

// ---------------------------------------------------------------------------
// Static wrappers and stubs for menu_import_t slots
// ---------------------------------------------------------------------------

static void *UI_Z_Malloc( int size )
{
	return Z_Malloc( size, TAG_TEMP_WORKSPACE, qfalse );
}

static void UI_Z_Free( void *buf )
{
	Z_Free( buf );
}

static void UI_Z_FreeTags( int tag )
{
	// OpenJK has Z_TagFree(memtag_t) but SOF2 passes an int tag.
	// We have no direct mapping — just ignore.
}

// Process-heap allocators compatible with DLL's operator_delete.
// Menusx86.dll frees these via operator_delete → HeapFree(GetProcessHeap()).
// Zone allocator is NOT compatible; use Win32 heap directly.
static int g_mallocCount = 0;
static void *UI_SysMalloc( int size ) {
	void *p = HeapAlloc( GetProcessHeap(), 0, (SIZE_T)size );
	if ( (++g_mallocCount % 50) == 1 )
		Com_Printf("[DBG] Z_Malloc(%d) = %p  [#%d]\n", size, p, g_mallocCount);
	return p;
}
static void  UI_SysFree( void *ptr ) {
	if ( ptr ) HeapFree( GetProcessHeap(), 0, ptr );
}

static char *UI_COM_Parse( char **text )
{
	return COM_Parse( (const char **)text );
}

static void UI_COM_StripExtension( const char *in, char *out )
{
	COM_StripExtension( in, out, MAX_OSPATH );
}

static void UI_COM_StripFilename( const char *in, char *out )
{
	// Remove the last path component (filename), keeping the directory.
	const char *fwd = strrchr( in, '/' );
	const char *bak = strrchr( in, '\\' );
	const char *p   = (fwd > bak) ? fwd : bak;
	if ( p ) {
		int n = (int)( p - in );
		memcpy( out, in, n );
		out[n] = '\0';
	} else {
		*out = '\0';
	}
}

static int UI_Com_Clampi( int mn, int mx, int v )
{
	return v < mn ? mn : v > mx ? mx : v;
}

static float UI_Com_Clamp( float mn, float mx, float v )
{
	return v < mn ? mn : v > mx ? mx : v;
}

static void UI_Cvar_SetModified( const char *name, qboolean mod )
{
	// SOF2 engine exposed this to the UI; no direct OpenJK equivalent.
}

static void UI_Cvar_SetInternalAllowed( const char *name, int val )
{
	// Raven-internal cvar flag setter; stub.
}

int s_ui_in_setactivemenu = 0;  // 1 while inside uie->UI_SetActiveMenu

static char *UI_SE_GetString( const char *token, int flags )
{
	return (char *)SE_GetString( token );
}

static long UI_FS_ReadFile_Traced( const char *qpath, void **buffer )
{
	return (long)FS_ReadFile( qpath, buffer );
}

static int UI_FS_FOpenFileByMode_Traced( const char *qpath, fileHandle_t *f, fsMode_t mode )
{
	return FS_FOpenFileByMode( qpath, f, mode );
}

static void UI_Cbuf_AddText_Traced( const char *text )
{
	Cbuf_AddText( text );
}

static void Cvar_SetValue_Traced( const char *var_name, float value )
{
	// Inline Cvar_SetValue logic (calling Cvar_SetValue directly caused a crash;
	// calling Cvar_Set directly works, so inline the float→string conversion here).
	char val[32];
	if ( value == (int)value )
		Com_sprintf( val, sizeof(val), "%i", (int)value );
	else
		Com_sprintf( val, sizeof(val), "%f", value );
	Cvar_Set( var_name, val );
}

static int Milliseconds_Traced( void )
{
	return Sys_Milliseconds2();
}

static void UI_Sprintf( char *dest, int size, const char *fmt, ... )
{
	va_list argptr;
	va_start( argptr, fmt );
	Q_vsnprintf( dest, size, fmt, argptr );
	va_end( argptr );
}

static void UI_FS_CleanPath( char *path )
{
	// Normalize path separators — stub, not critical for functionality.
}

// Wrappers to resolve const/type mismatches for function pointer assignment
static char *UI_Info_ValueForKey( const char *s, const char *key )
{
	return (char *)Info_ValueForKey( s, key );
}

static int UI_Key_IsDown( int keynum )
{
	return (int)Key_IsDown( keynum );
}

static int UI_Key_StringToKeynum( const char *str )
{
	return Key_StringToKeynum( (char *)str );
}

static char *UI_SP_GetStringTextByIndex( int idx )
{
	static char empty[1] = { 0 };
	return empty;
}

static void *UI_G2_SpawnGhoul2Model( void )
{
	return NULL;
}

static void UI_G2_PlayModelAnimation( void *model, const char *anim,
                                      int frame, int flags, int speed )
{
}

static int UI_CL_IsCgameActive( void )
{
	return cls.cgameStarted ? 1 : 0;
}

static void UI_CL_SaveGameScreenshot( void )
{
	// SOF2 saves a screenshot to disk as part of the save-game flow; stub.
}

static void UI_SV_WipeSavegame( const char *name )
{
	SG_WipeSavegame( name );
}

static void UI_ICARUS_SignalEvent( int entID, const char *name )
{
	// No client-side ICARUS in OpenSOF2 yet; stub.
}

static int UI_ICARUS_RunScript( void *entity, const char *name )
{
	return 0;
}

// SOF2 menu_import_t slots 71-73: engine-side wrappers for renderer operations.
// SOF2's CL_InitUI loaded these from cls.refExport by byte offset; these are the
// corresponding OpenJK functions.
static int UI_RE_RegisterModel( const char *name ) {
	Com_Printf("[DBG] RE_RegisterModel: '%s'\n", name ? name : "(null)");
	return re.RegisterModel( name );
}
static void UI_RE_AddRefEntityToScene( const refEntity_t *ent ) {
	re.AddRefEntityToScene( ent );
}
static void UI_RE_ClearScene( void ) {
	re.ClearScene();
}

// Safe stub for NULL import slots — returns 0 to signal "not found" / "no-op".
// Prevents a crash if the DLL calls a slot we haven't fully identified yet.
static int UI_NullSlotStub( void ) {
	Com_Printf("[DBG] NullSlotStub called!\n");
	return 0;
}

// Traced wrapper for Cbuf_ExecuteText (slot 22)
static void UI_Cbuf_ExecuteText_Traced( int exec_when, const char *text ) {
	Com_Printf("[DBG] CbufExec: when=%d textPtr=%p\n", exec_when, (void*)text);
	if ( text ) {
		char safe[128];
		Q_strncpyz( safe, text, sizeof(safe) );
		Com_Printf("[DBG] CbufExec: text='%s'\n", safe);
	}
	Cbuf_ExecuteText( exec_when, text );
	Com_Printf("[DBG] CbufExec: returned\n");
}

// Traced stubs for unknown-but-called slots — prints name so we can identify them.
static int UI_Slot13_Stub( void ) {
	return 0;
}
static int UI_Slot46_GetNextToken( const char *buf, char *out, int outlen ) {
	// Slot 46: COM_GetNextToken variant — called during .rmf menu file parsing.
	// Takes a source string and writes the first token to 'out'.
	// Best-effort: wrap COM_Parse to extract the first token.
	if (!buf || !out || outlen <= 0) return 0;
	// Copy first whitespace-delimited token
	while (*buf == ' ' || *buf == '\t' || *buf == '\n' || *buf == '\r') buf++;
	int i = 0;
	while (*buf && *buf != ' ' && *buf != '\t' && *buf != '\n' && *buf != '\r' && i < outlen-1)
		out[i++] = *buf++;
	out[i] = '\0';
	return i > 0 ? 1 : 0;
}
static const char *UI_Slot56_GetBinding( int keynum ) {
	// Slot 56: Key_GetBinding variant — called for key binding lookups in menus.
	const char *binding = Key_GetBinding( keynum );
	return binding ? binding : "";
}

// Slot 84: COM_GetNextChar — SOF2's STM/HUD parser calls this to read one character
// from the text buffer and advance the cursor.  Called in a tight loop by
// UIW_MenuBuffer_ParseNextToken().
// Signature (verified from UIW_MenuBuffer_ParseNextToken disasm 0x40023234):
//   PUSH EBX (=0, flags)
//   PUSH ESI (ptr to cursor variable — i.e., char **)
//   CALL [slot 84]
// Returns the character as int; advances *cursor.
// *** WITHOUT THIS FIX, the DLL loops forever (infinite hang) ***
//
// CRITICAL: ALWAYS advance cursor, even on '\0'.
// UIW_MenuFile_Init sets end = buffer + length where buffer[length-1] = '\0'.
// If we don't advance past '\0', cursor stays at buffer[length-1] while
// cursor < end (buffer+length) is still true → infinite outer loop.
// Advancing cursor to buffer+length = end causes the outer loop to exit. ✓
static int UI_COM_GetNextChar( char **cursor, int flags ) {
	if ( !cursor || !*cursor ) return 0;
	unsigned char c = (unsigned char)**cursor;
	(*cursor)++;   // ALWAYS advance — even on '\0' so cursor reaches end and outer loop exits
	return (int)c;
}

// Slot 64: Key_GetBindingByName — SOF2's STM parser uses &KEYNAME& syntax to expand
// key bindings in menu text.  The DLL calls this with the accumulated key-name string
// (e.g. "MOUSE1") and expects back the engine binding string (e.g. "+attack").
// Signature (verified from UIW_MenuBuffer_ParseNextToken disasm 0x400232a0):
//   PUSH ECX (key-name string buffer)
//   CALL [slot 64]
// Returns const char * (binding string; empty string → ParseError "key not found").
static const char *UI_Key_GetBindingByName( const char *keyname ) {
	if ( !keyname || !keyname[0] ) return "";
	int keynum = Key_StringToKeynum( (char *)keyname );
	if ( keynum < 0 ) return "";
	const char *binding = Key_GetBinding( keynum );
	return binding ? binding : "";
}

// Slot 86: R_RegisterFont — SOF2's DLL calls this as RegisterFont(name, 0, 1.0f, 0).
// OpenJK's re.RegisterFont has a different signature (takes fontInfo_t*), so we stub it.
// The DLL stores the returned handle and uses it for text rendering.
// Return 1 (non-zero = "registered") so font drawing code doesn't skip text entirely.
static int UI_RE_RegisterFont_Stub( const char *name, int size, int scaleRaw, int flags ) {
	static int s_nf = 0;
	Com_Printf( "UI_RE_RegisterFont[%d]: %s → 1\n", ++s_nf, name ? name : "(null)" );
	return 1;
}

// Slot 94: RE_RegisterShaderNoMip — traced wrapper so we can confirm shader
// processing actually reaches this point after the font entries.
//
// OpenJK's RE_RegisterShaderNoMip returns 0 when sh->defaultShader is true,
// i.e. the texture file is missing.  SOF2's menu DLL (Menusx86.dll) checks
// h != 0 in its Image widget validity check (vtable[7]) and treats 0 as
// "widget failed to load", triggering an error/destructor path that crashes.
// To avoid that, substitute handle 1 (always a valid registered shader) so
// the DLL gets a non-zero handle and the widget is created normally (it will
// just render with whatever shader 1 is for the missing texture, which is
// acceptable during development without full game assets).
static int UI_RE_RegisterShaderNoMip_Traced( const char *name ) {
	static int s_ns = 0;
	int h = re.RegisterShaderNoMip( name );
	int raw = h;
	if ( h == 0 ) h = 1;   // missing texture → use fallback so DLL doesn't crash
	++s_ns;
	Com_Printf( "RegisterShaderNoMip[%d]: %s raw=%d ret=%d\n", s_ns, name ? name : "(null)", raw, h );
	return h;
}

// Slot 92: RE_DrawGetPicSize — SOF2 renderer function that returns image pixel dimensions.
// IMPORTANT: The original SOF2 engine writes only 16-bit (short) values to the output
// pointers.  Menusx86.dll passes short* addresses, so writing 32-bit ints would corrupt
// 2 adjacent bytes of DLL stack/struct memory on every call.
static void UI_RE_DrawGetPicSize( int *w, int *h, int hShader ) {
	int iw = 0, ih = 0;
	re.DrawGetPicSize( &iw, &ih, hShader );
	Com_Printf("[DBG] DrawGetPicSize: shader=%d → iw=%d ih=%d\n", hShader, iw, ih);
	// Write only 16-bit values to match original SOF2 behaviour
	if ( w ) *(short *)w = (short)iw;
	if ( h ) *(short *)h = (short)ih;
}

// Slot 90: RE_StretchPic — SOF2 simple image draw (no UV, colour-tinted).
// SOF2 signature: void RE_StretchPic(float x, float y, float w, float h, int color, int hShader, int adjustFrom)
// Map to OpenJK DrawStretchPic with full UV coverage and no tint.
static void UI_RE_StretchPic( float x, float y, float w, float h, int color, int hShader, int adjustFrom ) {
	Com_Printf("[DBG] StretchPic: x=%.0f y=%.0f w=%.0f h=%.0f shader=%d\n", x, y, w, h, hShader);
	(void)color; (void)adjustFrom;
	re.DrawStretchPic( x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, hShader );
}

// Slot 87: RE_Font_HeightPixels — font height in pixels scaled by 'scale'.
// SOF2 signature: float RE_Font_HeightPixels(uint fontHandle, float scale)
// OpenJK doesn't expose this; return a reasonable fixed height.
static float UI_RE_Font_HeightPixels( unsigned int fontHandle, float scale ) {
	Com_Printf("[DBG] Font_HeightPixels(font=%u, scale=%.2f)\n", fontHandle, scale);
	(void)fontHandle;
	return 14.0f * scale;
}

// Slot 88: RE_Font_StrLenPixels — pixel width of a string.
// SOF2 signature: int RE_Font_StrLenPixels(char *text, int setIndex, float scale)
// OpenJK doesn't expose this; estimate based on character count.
static int UI_RE_Font_StrLenPixels( const char *text, int setIndex, float scale ) {
	(void)setIndex;
	if ( !text ) return 0;
	int len = 0;
	while ( text[len] ) len++;
	int px = (int)( len * 8.0f * scale );
	Com_Printf("[DBG] Font_StrLenPixels('%s', set=%d, scale=%.2f) = %d\n", text, setIndex, scale, px);
	return px;
}

// Slot 89: RE_Font_DrawString — draw a text string.
// SOF2 signature: void RE_Font_DrawString(int ox, int oy, char *text, float *rgba, int setIndex, int limit, float scale)
// OpenJK uses a different font format; no-op for now (menus will be layout-correct but textless).
static void UI_RE_Font_DrawString( int ox, int oy, const char *text, const float *rgba,
								   int setIndex, int limit, float scale ) {
	Com_Printf("[DBG] Font_DrawString(x=%d, y=%d, '%s', set=%d, lim=%d, scale=%.2f)\n",
		ox, oy, text ? text : "(null)", setIndex, limit, scale);
	(void)ox; (void)oy; (void)text; (void)rgba; (void)setIndex; (void)limit; (void)scale;
}


/*
====================
Helper functions for User Interface
====================
*/

/*
====================
GetClientState
====================
*/
static connstate_t GetClientState( void ) {
	return cls.state;
}

/*
====================
CL_GetGlConfig
====================
*/
static void UI_GetGlconfig( glconfig_t *config ) {
	*config = cls.glconfig;
}

/*
====================
GetClipboardData
====================
*/
static void GetClipboardData( char *buf, int buflen ) {
	char	*cbd, *c;

	c = cbd = Sys_GetClipboardData();
	if ( !cbd ) {
		*buf = 0;
		return;
	}

	for ( int i = 0, end = buflen - 1; *c && i < end; i++ )
	{
		uint32_t utf32 = ConvertUTF8ToUTF32( c, &c );
		buf[i] = ConvertUTF32ToExpectedCharset( utf32 );
	}

	Z_Free( cbd );
}

/*
====================
Key_KeynumToStringBuf
====================
*/
// only ever called by binding-display code, therefore returns non-technical "friendly" names
//	in any language that don't necessarily match those in the config file...
//
void Key_KeynumToStringBuf( int keynum, char *buf, int buflen )
{
	const char *psKeyName = Key_KeynumToString( keynum/*, qtrue */);

	// see if there's a more friendly (or localised) name...
	//
	const char *psKeyNameFriendly = SE_GetString( va("KEYNAMES_KEYNAME_%s",psKeyName) );

	Q_strncpyz( buf, (psKeyNameFriendly && psKeyNameFriendly[0]) ? psKeyNameFriendly : psKeyName, buflen );
}

/*
====================
Key_GetBindingBuf
====================
*/
void Key_GetBindingBuf( int keynum, char *buf, int buflen ) {
	const char	*value;

	value = Key_GetBinding( keynum );
	if ( value ) {
		Q_strncpyz( buf, value, buflen );
	}
	else {
		*buf = 0;
	}
}

/*
====================
FloatAsInt
====================
*/
static int FloatAsInt( float f )
{
	byteAlias_t fi;
	fi.f = f;
	return fi.i;
}

static void UI_Cvar_Create( const char *var_name, const char *var_value, int flags ) {
	Cvar_Register( NULL, var_name, var_value, flags );
}

static int GetConfigString(int index, char *buf, int size)
{
	int		offset;

	if (index < 0 || index >= MAX_CONFIGSTRINGS)
		return qfalse;

	offset = cl.gameState.stringOffsets[index];
	if (!offset)
		return qfalse;

	Q_strncpyz( buf, cl.gameState.stringData+offset, size);

	return qtrue;
}

/*
====================
CL_ShutdownUI
====================
*/
void UI_Shutdown( void );
void CL_ShutdownUI( void ) {
	if ( uie ) {
		if ( uie->UI_Shutdown )
			uie->UI_Shutdown();
	} else {
		UI_Shutdown();
	}

	if ( uiLibrary ) {
		Sys_UnloadDll( uiLibrary );
		uiLibrary = NULL;
	}
	uie = NULL;

	Key_SetCatcher( Key_GetCatcher( ) & ~KEYCATCH_UI );
	cls.uiStarted = qfalse;
}

void CL_DrawDatapad(int HUDType)
{
	switch(HUDType)
	{
	case DP_HUD:
		VM_Call( CG_DRAW_DATAPAD_HUD );
		break;
	case DP_OBJECTIVES:
		VM_Call( CG_DRAW_DATAPAD_OBJECTIVES );
		break;
	case DP_WEAPONS:
		VM_Call( CG_DRAW_DATAPAD_WEAPONS );
		break;
	case DP_INVENTORY:
		VM_Call( CG_DRAW_DATAPAD_INVENTORY );
		break;
	case DP_FORCEPOWERS:
		VM_Call( CG_DRAW_DATAPAD_FORCEPOWERS );
		break;
	default:
		break;
	}


}

void UI_Init( int apiVersion, uiimport_t *uiimport, qboolean inGameLoad );

/*
====================
CL_InitUI
====================
*/
void CL_InitUI( void ) {
#ifdef JK2_MODE
	JK2SP_Register("keynames", 0	/*SP_REGISTER_REQUIRED*/);		// reference is KEYNAMES
#endif

	// -----------------------------------------------------------------------
	// Attempt to load SOF2's Menusx86.dll via struct-based GetUIAPI().
	// If that fails, fall back to the statically-linked JK2 UI code.
	// -----------------------------------------------------------------------

	uiLibrary = Sys_LoadSPGameDll( "Menus", NULL );   // "Menus" + ARCH_STRING("x86") + DLL_EXT = "Menusx86.dll"
	if ( !uiLibrary ) {
		Com_Printf( "CL_InitUI: Menus" ARCH_STRING DLL_EXT " not found (%s), using built-in UI\n",
		            Sys_LibraryError() );
		goto use_static_ui;
	}

	{
		typedef uiExport_t *GetUIAPIProc( int version, menu_import_t *import );
		GetUIAPIProc *GetUIAPI_fp = (GetUIAPIProc *)Sys_LoadFunction( uiLibrary, "GetUIAPI" );
		if ( !GetUIAPI_fp ) {
			Com_Printf( "CL_InitUI: GetUIAPI not found in Menusx86 DLL\n" );
			Sys_UnloadDll( uiLibrary );
			uiLibrary = NULL;
			goto use_static_ui;
		}

		// -------------------------------------------------------------------
		// Build the 102-slot menu_import_t on the stack.
		// Slots not listed below are zero-initialised (NULL / 0).
		// -------------------------------------------------------------------
		menu_import_t import;
		memset( &import, 0, sizeof( import ) );

		// CvarGet wrapper — local struct for scope convenience
		struct DLLTrace {
			static cvar_t *CvarGet( const char *nm, const char *val, int fl ) {
				return Cvar_Get(nm,val,fl);
			}
		};

		// [0-4] General — use Com_Printf directly to test if the issue is in our wrapper
		import.Printf       = Com_Printf;
		import.DPrintf      = Com_Printf;
		import.DPrintf2     = Com_Printf;
		import.Sprintf      = UI_Sprintf;
		import.Error        = Com_Error;

		// [5-15] File system
		import.FS_FOpenFileByMode = UI_FS_FOpenFileByMode_Traced;
		import.FS_Read            = FS_Read;
		import.FS_Write           = FS_Write;
		import.FS_FCloseFile      = FS_FCloseFile;
		import.FS_ReadFile        = UI_FS_ReadFile_Traced;
		import.FS_FreeFile        = FS_FreeFile;
		import.FS_FileExists      = FS_FileExists;
		import.FS_GetFileList     = FS_GetFileList;
		import.reserved_13        = (void *)UI_Slot13_Stub;  // unknown FS variant — stub
		import.FS_FreeFileList    = FS_FreeFileList;
		import.FS_CleanPath       = UI_FS_CleanPath;

		// [16-20] Time / Math
		import.Milliseconds      = Milliseconds_Traced;
		import.Q_irand           = Q_irand;
		import.Q_flrand          = Q_flrand;
		import.Cmd_TokenizeString = Cmd_TokenizeString;
		import.COM_Parse         = UI_COM_Parse;

		// [21-27] Console / Command
		import.Cbuf_AddText    = UI_Cbuf_AddText_Traced;
		import.Cbuf_ExecuteText= UI_Cbuf_ExecuteText_Traced;
		import.Cmd_AddCommand  = Cmd_AddCommand;
		import.Cmd_RemoveCommand = Cmd_RemoveCommand;
		import.Cmd_Argc        = Cmd_Argc;
		import.Cmd_ArgvBuffer  = Cmd_ArgvBuffer;
		import.Cmd_ArgsBuffer  = Cmd_ArgsBuffer;



		// [28-35] Cvar — trace Cvar_Get to see what DLL reads at init
		import.Cvar_Get                  = DLLTrace::CvarGet;
		// NULL-safe wrapper: DLL calls Cvar_Set(0) during MenuSystem init to
		// reset some state; guard against crashing on NULL var_name.
		struct CvarSetSafe {
			static void Set( const char *name, const char *value ) {
				// Guard against NULL name — DLL calls Cvar_Set(NULL) during ConsoleCommand.
				if ( !name ) return;
				Cvar_Set( name, value );
			}
		};
		import.Cvar_Set                  = CvarSetSafe::Set;
		import.Cvar_SetModified          = UI_Cvar_SetModified;
		import.Cvar_SetValue             = Cvar_SetValue_Traced;
		import.Cvar_VariableIntegerValue = Cvar_VariableIntegerValue;
		import.Cvar_VariableValue        = Cvar_VariableValue;
		import.Cvar_VariableStringBuffer = Cvar_VariableStringBuffer;
		import.Cvar_SetInternalAllowed   = UI_Cvar_SetInternalAllowed;

		// [36-38] Memory
		// Process heap wrappers — compatible with DLL's operator_delete (HeapFree).
		import.Z_Malloc   = UI_SysMalloc;
		import.Z_FreeTags = UI_Z_FreeTags;
		import.Z_Free     = UI_SysFree;

		// [39-43] Data fields (NOT function pointers — raw values/addresses)
		import.vidWidth    = cls.glconfig.vidWidth;
		import.vidHeight   = cls.glconfig.vidHeight;
		import.keyCatchers = &keyCatchers;
		import.clsState    = (int *)&cls.state;
		import.keys_array  = kg.keys;

		// [44-53] Utility / String / Key
		import.Cvar_InfoString    = Cvar_InfoString;
		import.COM_StripFilename  = UI_COM_StripFilename;
		import.reserved_46        = (void *)UI_Slot46_GetNextToken;  // COM_GetNextToken variant
		import.COM_StripExtension = UI_COM_StripExtension;
		import.Com_Clampi         = UI_Com_Clampi;
		import.Com_Clamp          = UI_Com_Clamp;
		import.va                 = va;
		import.Info_ValueForKey   = UI_Info_ValueForKey;
		import.Info_Print         = Info_Print;
		import.Key_StringToKeynum = UI_Key_StringToKeynum;

		// [54-65] Key system / String edition
		import.Key_KeynumToString = (char *(*)(int))Key_KeynumToString;
		import.Key_IsDown         = UI_Key_IsDown;
		import.reserved_56        = (void *)UI_Slot56_GetBinding;  // Key_GetBinding variant
		import.Key_GetKey         = Key_GetKey;
		// slot 58 reserved_58 = Key_GetClipboardData variant — stub for safety
		import.reserved_58  = (void *)UI_NullSlotStub;
		import.Key_ClearStates    = Key_ClearStates;
		// slot 60: unknown — stub for safety
		import.reserved_60  = (void *)UI_NullSlotStub;
		// slot 61: ACTUAL FS_ReadFile (verified from UIW_MenuFile_Init disasm: CALL [0x400376cc])
		// Our struct placed FS_ReadFile at slot 9 but the DLL uses slot 61. Fix here.
		import.reserved_61  = (void *)UI_FS_ReadFile_Traced;
		// slot 62: ACTUAL FS_FreeFile for success path (verified: CALL [0x400376d0])
		import.reserved_62  = (void *)FS_FreeFile;
		import.SP_GetStringTextByIndex = UI_SP_GetStringTextByIndex;
		// slot 64: ACTUAL Key_GetBindingByName (verified from UIW_MenuBuffer_ParseNextToken disasm 0x400232a0)
		// DLL calls this with &KEYNAME& text to expand key bindings in menus.
		// Was UI_SE_GetString which would return empty string (wrong).
		import.SE_GetString       = (char *(*)(const char *, int))UI_Key_GetBindingByName;
		import.SE_GetString2      = UI_SE_GetString;

		// [66-74] Client / G2
		// slot 66: UI_MenuPage_RenderFrame calls this WITHOUT a null-check — must be a callable.
		// SoF2.exe provides RE_RenderScene here; a stub returning 0 is safe (no map loaded yet).
		import.reserved_66  = (void *)UI_NullSlotStub;
		// slot 67: Key_SetCatcher variant — DLL sets keycatcher state when opening/closing menus
		// slot 67: DLL calls with no args + checks return value as gate (not Key_SetCatcher!).
		// The DLL's UI_SetActiveMenu calls (*ui_Key_SetCatcher)() with NO args and returns early
		// if non-zero. Original SoF2.exe provides UI_RE_RenderScene here (returns 0 normally).
		// NullSlotStub returns 0 → gate passes → menu opens. Key_SetCatcher was wrong here.
		import.reserved_67  = (void *)UI_NullSlotStub;
		// slot 68: G2API_CleanGhoul2Models variant — stub (not needed for menus)
		import.reserved_68  = (void *)UI_NullSlotStub;
		import.G2_SpawnGhoul2Model    = UI_G2_SpawnGhoul2Model;
		import.G2_PlayModelAnimation  = UI_G2_PlayModelAnimation;
		// slots 71-73: engine renderer wrappers needed for 3D model rendering in menus
		import.reserved_71  = (void *)UI_RE_RegisterModel;
		import.reserved_72  = (void *)UI_RE_AddRefEntityToScene;
		import.reserved_73  = (void *)UI_RE_ClearScene;
		import.CL_IsCgameActive       = UI_CL_IsCgameActive;

		// [75-77] Save game
		import.reserved_75  = (void *)UI_NullSlotStub;   // SV_LoadGame variant — stub
		import.reserved_76  = (void *)UI_NullSlotStub;   // SV_SaveGame variant — stub
		import.SV_WipeSavegame        = UI_SV_WipeSavegame;

		// [78-83] Sound module
		// slot 78 (snd+0x14): S_RegisterSoundSimple — verified at DLL addr 0x40037710 (slot 78)
		import.S_MuteSound      = (void *)S_RegisterSound;   // slot 78 = S_RegisterSoundSimple
		import.S_StartLocalSound= (void *)S_StartLocalSound; // slot 79 = S_StartLocalSound ✓
		import.S_StopAllSounds  = (void *)S_StopAllSounds;   // slot 80 = S_StopAllSounds ✓
		import.S_reserved_81    = (void *)UI_NullSlotStub;               // slot 81 = snd+0x64
		import.S_reserved_82    = (void *)UI_NullSlotStub;   // slot 82 = snd+0x24 — stub
		import.S_reserved_83    = (void *)UI_NullSlotStub;   // slot 83 = snd+0x28 — stub

		// [84-99] Renderer vtable — SOF2's CL_InitUI loads these from cls.refExport by byte offset.
		// SOF2 refexport_t layout verified by decompiling SoF2.exe GetRefAPI @ 0x10091ee0.
		// SOF2 layout differs from OpenJK at +0x58 onwards; do NOT use "at-offset" approach here.
		// slot 84: ACTUAL COM_GetNextChar (verified from UIW_MenuBuffer_ParseNextToken disasm 0x40023234)
		// Was re.ProcessDissolve — caused infinite loop hang during defines.hud parse.
		import.RE_reserved_84   = (void *)UI_COM_GetNextChar;
		// slot 85: ACTUAL Key_GetOverstrike (verified from UI_Frame_Ctor disasm: CALL [0x4003772c])
		// Our struct had re.InitDissolve here which returned garbage, corrupting the
		// 4th arg to UIW_MenuFile_Init. Fix to Key_GetOverstrike so it returns 0/1 cleanly.
		import.RE_reserved_85   = (void *)Key_GetOverstrikeMode;
		// slot 86: ACTUAL R_RegisterFont (verified from UI_Frame_Ctor disasm: CALL [0x40037730])
		// Our struct had UI_NullSlotStub here returning 0 (invalid font handle).
		import.RE_DamageSurface = (void *)UI_RE_RegisterFont_Stub;
		// slot 87 (SOF2 re+0x8c): RE_Font_HeightPixels(uint fontHandle, float scale) -> float
		import.RE_reserved_87   = (void *)UI_RE_Font_HeightPixels;
		// slot 88 (SOF2 re+0x90): RE_Font_StrLenPixels(char *text, int setIndex, float scale) -> int
		import.RE_reserved_88   = (void *)UI_RE_Font_StrLenPixels;
		// slot 89 (SOF2 re+0x94): RE_Font_DrawString(int ox, int oy, char *text, float *rgba, int setIndex, int limit, float scale)
		import.RE_reserved_89   = (void *)UI_RE_Font_DrawString;
		// slot 90 (SOF2 re+0x58): RE_StretchPic(float x, float y, float w, float h, int color, int hShader, int adjustFrom)
		import.RE_LerpTag       = (void *)UI_RE_StretchPic;
		// slot 91 (SOF2 re+0x5c): RE_DrawStretchPic — same signature as OpenJK DrawStretchPic
		import.RE_ModelBounds   = (void *)re.DrawStretchPic;
		// slot 92 (SOF2 re+0x60): RE_DrawGetPicSize(int *w, int *h, int hShader) — THE CRASH FIX
		// UI_Image_Ctor calls this to get image pixel dims; vtable[7] checks *w > 0.
		// Was re.DrawRotatePic (wrong sig, garbage hShader → "out of range" warning, *w stays 0 → crash).
		import.RE_reserved_92   = (void *)UI_RE_DrawGetPicSize;
		// slot 93 = CL_SaveGameScreenshot (direct, not re offset)
		import.CL_SaveGameScreenshot = UI_CL_SaveGameScreenshot;
		// SOF2 re+0x14 = RegisterShaderNoMip; traced wrapper to confirm shader processing
		import.RE_RegisterShaderNoMip = (void *)UI_RE_RegisterShaderNoMip_Traced;
		import.RE_reserved_95   = (void *)re.LerpTag;                    // re+0x98 (OpenJK SP at-offset)
		import.RE_reserved_96   = (void *)re.ModelBounds;                // re+0x9c (OpenJK SP at-offset)
		import.RE_reserved_97   = (void *)re.GetLightStyle;              // re+0xa0 (OpenJK SP at-offset)
		import.RE_reserved_98   = (void *)re.GetBModelVerts;             // re+0xa8 (OpenJK SP at-offset; skips SetLightStyle at 0xa4)
		import.RE_reserved_99   = (void *)re.WorldEffectCommand;         // re+0xac (OpenJK SP at-offset)

		// [100-101] ICARUS scripting
		import.ICARUS_SignalEvent = UI_ICARUS_SignalEvent;
		import.ICARUS_RunScript   = UI_ICARUS_RunScript;

		// -------------------------------------------------------------------
		// Fill remaining NULL slots with a sentinel so we catch unset calls
		// instead of crashing silently at address 0x00000000.
		// Skip data fields at slots 39-43 (vidWidth, vidHeight, keyCatchers, clsState, keys_array).
		// -------------------------------------------------------------------
		{
			void **slots = (void **)&import;
			int nslots = sizeof(import) / sizeof(void *);
			for ( int i = 0; i < nslots; i++ ) {
				if ( i >= 39 && i <= 43 ) continue;  // data fields, not function pointers
				if ( slots[i] == NULL ) {
					Com_Printf( "CL_InitUI: slot %d is NULL — filling with sentinel\n", i );
					slots[i] = (void *)UI_NullSlotStub;
				}
			}
		}

		// -------------------------------------------------------------------
		// Call GetUIAPI — DLL populates its export table and returns it.
		// -------------------------------------------------------------------
		uie = GetUIAPI_fp( UI_API_VERSION, &import );
		if ( !uie ) {
			Com_Printf( "CL_InitUI: GetUIAPI returned NULL\n" );
			Sys_UnloadDll( uiLibrary );
			uiLibrary = NULL;
			goto use_static_ui;
		}

		Com_Printf( "CL_InitUI: loaded Menus" ARCH_STRING DLL_EXT "\n" );

		// Initialize the UI DLL.
		if ( uie->UI_Init ) {
			Com_Printf( "CL_InitUI: calling UI_Init\n" );
			uie->UI_Init();
			Com_Printf( "CL_InitUI: UI_Init returned OK\n" );
		} else {
			Com_Printf( "CL_InitUI: UI_Init is NULL — skipping\n" );
		}

		return;
	}

use_static_ui:
	// -----------------------------------------------------------------------
	// Fall back to the statically-linked JK2 UI code.
	// -----------------------------------------------------------------------
	{
		uiimport_t	uii;
		memset( &uii, 0, sizeof( uii ) );

		uii.Printf = Com_Printf;
		uii.Error = Com_Error;

		uii.Cvar_Set				= Cvar_Set;
		uii.Cvar_VariableValue		= Cvar_VariableValue;
		uii.Cvar_VariableStringBuffer = Cvar_VariableStringBuffer;
		uii.Cvar_SetValue			= Cvar_SetValue;
		uii.Cvar_Reset				= Cvar_Reset;
		uii.Cvar_Create				= UI_Cvar_Create;
		uii.Cvar_InfoStringBuffer	= Cvar_InfoStringBuffer;

		uii.Draw_DataPad			= CL_DrawDatapad;

		uii.Argc					= Cmd_Argc;
		uii.Argv					= Cmd_ArgvBuffer;
		uii.Cmd_TokenizeString		= Cmd_TokenizeString;

		uii.Cmd_ExecuteText			= Cbuf_ExecuteText;

		uii.FS_FOpenFile			= FS_FOpenFileByMode;
		uii.FS_Read					= FS_Read;
		uii.FS_Write				= FS_Write;
		uii.FS_FCloseFile			= FS_FCloseFile;
		uii.FS_GetFileList			= FS_GetFileList;
		uii.FS_ReadFile				= FS_ReadFile;
		uii.FS_FreeFile				= FS_FreeFile;

		uii.R_RegisterModel			= re.RegisterModel;
		uii.R_RegisterSkin			= re.RegisterSkin;
		uii.R_RegisterShader		= re.RegisterShader;
		uii.R_RegisterShaderNoMip	= re.RegisterShaderNoMip;
		uii.R_RegisterFont			= re.RegisterFont;
		uii.R_Font_StrLenPixels		= re.Font_StrLenPixels;
		uii.R_Font_HeightPixels		= re.Font_HeightPixels;
		uii.R_Font_DrawString		= re.Font_DrawString;
		uii.R_Font_StrLenChars		= re.Font_StrLenChars;
		uii.Language_IsAsian		= re.Language_IsAsian;
		uii.Language_UsesSpaces		= re.Language_UsesSpaces;
		uii.AnyLanguage_ReadCharFromString = re.AnyLanguage_ReadCharFromString;

#ifdef JK2_MODE
		uii.SG_GetSaveImage			= SG_GetSaveImage;
#endif
		uii.SG_GetSaveGameComment	= SG_GetSaveGameComment;
		uii.SG_StoreSaveGameComment = SG_StoreSaveGameComment;
		uii.SG_GameAllowedToSaveHere= SG_GameAllowedToSaveHere;

#ifdef JK2_MODE
		uii.DrawStretchRaw			= re.DrawStretchRaw;
#endif
		uii.R_ClearScene			= re.ClearScene;
		uii.R_AddRefEntityToScene	= re.AddRefEntityToScene;
		uii.R_AddPolyToScene		= re.AddPolyToScene;
		uii.R_AddLightToScene		= re.AddLightToScene;
		uii.R_RenderScene			= re.RenderScene;

		uii.R_ModelBounds			= re.ModelBounds;

		uii.R_SetColor				= re.SetColor;
		uii.R_DrawStretchPic		= re.DrawStretchPic;
		uii.UpdateScreen			= SCR_UpdateScreen;

#ifdef JK2_MODE
		uii.PrecacheScreenshot		= SCR_PrecacheScreenshot;
#endif

		uii.R_LerpTag				= re.LerpTag;

		uii.S_StartLocalLoopingSound= S_StartLocalLoopingSound;
		uii.S_StartLocalSound		= S_StartLocalSound;
		uii.S_RegisterSound			= S_RegisterSound;

		uii.Key_KeynumToStringBuf	= Key_KeynumToStringBuf;
		uii.Key_GetBindingBuf		= Key_GetBindingBuf;
		uii.Key_SetBinding			= Key_SetBinding;
		uii.Key_IsDown				= Key_IsDown;
		uii.Key_GetOverstrikeMode	= Key_GetOverstrikeMode;
		uii.Key_SetOverstrikeMode	= Key_SetOverstrikeMode;
		uii.Key_ClearStates			= Key_ClearStates;
		uii.Key_GetCatcher			= Key_GetCatcher;
		uii.Key_SetCatcher			= Key_SetCatcher;
#ifdef JK2_MODE
		uii.SP_Register				= JK2SP_Register;
		uii.SP_GetStringText		= JK2SP_GetStringText;
		uii.SP_GetStringTextString  = JK2SP_GetStringTextString;
#endif

		uii.GetClipboardData		= GetClipboardData;
		uii.GetClientState			= GetClientState;
		uii.GetGlconfig				= UI_GetGlconfig;
		uii.GetConfigString			= (void (*)(int, char *, int))GetConfigString;
		uii.Milliseconds			= Sys_Milliseconds2;

		UI_Init( UI_API_VERSION, &uii, (qboolean)(cls.state > CA_DISCONNECTED && cls.state <= CA_ACTIVE) );
	}
}


qboolean UI_GameCommand( void ) {
	if ( !cls.uiStarted )
		return qfalse;

	// Guard against re-entrant calls while the DLL is executing a UI function.
	// The DLL's STM parser calls Cbuf_ExecuteText(EXEC_NOW, "s_music ...") for
	// music startup.  Because "s_music" is not a registered command or cvar in
	// OpenSOF2, Cmd_ExecuteString falls through to here.  Calling
	// UI_ConsoleCommand at this point destroys the menu system that is still
	// being constructed, leaving DAT_40037534 == NULL and causing a
	// null-pointer crash in UI_MenuSystem_LoadHudIntoMenu (read at NULL+4).
	if ( s_ui_in_setactivemenu ) {
		Com_DPrintf( "UI_GameCommand: suppressed re-entrant call (cmd: %s)\n",
		             Cmd_Argc() > 0 ? Cmd_Argv(0) : "" );
		return qtrue;   // claim handled so it doesn't echo to server
	}

	if ( uie ) {
		if ( uie->UI_ConsoleCommand )
			uie->UI_ConsoleCommand( 0 );
		return qtrue;
	}

	return UI_ConsoleCommand();
}


void CL_GenericMenu_f(void)
{
	const char *arg = Cmd_Argv( 1 );

	if (cls.uiStarted) {
		UI_SetActiveMenu("ingame",arg);
	}
}


void CL_EndScreenDissolve_f(void)
{
	re.InitDissolve(qtrue);	// dissolve from cinematic to underlying ingame
}

void CL_DataPad_f(void)
{
	if (cls.uiStarted && cls.cgameStarted && (cls.state == CA_ACTIVE) ) {
		UI_SetActiveMenu("datapad",NULL);
	}
}

/*
====================
CL_GetGlConfig
====================
*/
static void CL_GetGlconfig( glconfig_t *config )
{
	*config = cls.glconfig;
}
/*
int PC_ReadTokenHandle(int handle, pc_token_t *pc_token);
int PC_SourceFileAndLine(int handle, char *filename, int *line);
*/
/*
====================
CL_UISystemCalls

The ui module is making a system call
====================
*/
intptr_t CL_UISystemCalls( intptr_t *args )
{

	switch( args[0] )
	{
	case UI_ERROR:
		Com_Error( ERR_DROP, "%s", VMA(1) );
		return 0;

	case UI_CVAR_REGISTER:
		Cvar_Register( (vmCvar_t *)VMA(1),(const char *) VMA(2),(const char *) VMA(3), args[4] );
		return 0;

	case UI_CVAR_SET:
		Cvar_Set( (const char *) VMA(1), (const char *) VMA(2) );
		return 0;

	case UI_CVAR_SETVALUE:
		Cvar_SetValue( (const char *) VMA(1), VMF(2) );
		return 0;

	case UI_CVAR_UPDATE:
		Cvar_Update( (vmCvar_t *) VMA(1) );
		return 0;

	case UI_R_REGISTERMODEL:
		return re.RegisterModel((const char *) VMA(1) );

	case UI_R_REGISTERSHADERNOMIP:
		return re.RegisterShaderNoMip((const char *) VMA(1) );

	case UI_GETGLCONFIG:
		CL_GetGlconfig( ( glconfig_t *) VMA(1) );
		return 0;

	case UI_CMD_EXECUTETEXT:
		Cbuf_ExecuteText( args[1], (const char *) VMA(2) );
		return 0;

	case UI_CVAR_VARIABLEVALUE:
		return FloatAsInt( Cvar_VariableValue( (const char *) VMA(1) ) );

	case UI_FS_GETFILELIST:
		return FS_GetFileList( (const char *) VMA(1), (const char *) VMA(2), (char *) VMA(3), args[4] );

	case UI_KEY_SETCATCHER:
		Key_SetCatcher( args[1] );
		return 0;

	case UI_KEY_CLEARSTATES:
		Key_ClearStates();
		return 0;

	case UI_R_SETCOLOR:
		re.SetColor( (const float *) VMA(1) );
		return 0;

	case UI_R_DRAWSTRETCHPIC:
		re.DrawStretchPic( VMF(1), VMF(2), VMF(3), VMF(4), VMF(5), VMF(6), VMF(7), VMF(8), args[9] );
		return 0;

	case UI_CVAR_VARIABLESTRINGBUFFER:
		Cvar_VariableStringBuffer( (const char *) VMA(1), (char *) VMA(2), args[3] );
		return 0;

  case UI_R_MODELBOUNDS:
		re.ModelBounds( args[1], (float *) VMA(2),(float *) VMA(3) );
		return 0;

	case UI_R_CLEARSCENE:
		re.ClearScene();
		return 0;

//	case UI_KEY_GETOVERSTRIKEMODE:
//		return Key_GetOverstrikeMode();
//		return 0;

//	case UI_PC_READ_TOKEN:
//		return PC_ReadTokenHandle( args[1], VMA(2) );

//	case UI_PC_SOURCE_FILE_AND_LINE:
//		return PC_SourceFileAndLine( args[1], VMA(2), VMA(3) );

	case UI_KEY_GETCATCHER:
		return Key_GetCatcher();

	case UI_MILLISECONDS:
		return Sys_Milliseconds();

	case UI_S_REGISTERSOUND:
		return S_RegisterSound((const char *) VMA(1));

	case UI_S_STARTLOCALSOUND:
		S_StartLocalSound( args[1], args[2] );
		return 0;

//	case UI_R_REGISTERFONT:
//		re.RegisterFont( VMA(1), args[2], VMA(3));
//		return 0;

	case UI_CIN_PLAYCINEMATIC:
	  Com_DPrintf("UI_CIN_PlayCinematic\n");
	  return CIN_PlayCinematic((const char *)VMA(1), args[2], args[3], args[4], args[5], args[6], (const char *)VMA(7));

	case UI_CIN_STOPCINEMATIC:
	  return CIN_StopCinematic(args[1]);

	case UI_CIN_RUNCINEMATIC:
	  return CIN_RunCinematic(args[1]);

	case UI_CIN_DRAWCINEMATIC:
	  CIN_DrawCinematic(args[1]);
	  return 0;

	case UI_KEY_SETBINDING:
		Key_SetBinding( args[1], (const char *) VMA(2) );
		return 0;

	case UI_KEY_KEYNUMTOSTRINGBUF:
		Key_KeynumToStringBuf( args[1],(char *) VMA(2), args[3] );
		return 0;

	case UI_CIN_SETEXTENTS:
	  CIN_SetExtents(args[1], args[2], args[3], args[4], args[5]);
	  return 0;

	case UI_KEY_GETBINDINGBUF:
		Key_GetBindingBuf( args[1], (char *) VMA(2), args[3] );
		return 0;


	default:
		Com_Error( ERR_DROP, "Bad UI system trap: %i", args[0] );

	}

	return 0;
}


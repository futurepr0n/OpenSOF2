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

// ============================================================================
// SOF2-compatible cvar_t wrapper for Menusx86.dll ABI compatibility
//
// SOF2's cvar_t has char name[64] at offset 0, so (char*)cvar_ptr == cvar->name.
// The DLL exploits this: it passes cvar_t* to Cvar_Set(name, val) and directly
// reads cvar_t+0x44 for the string field.  OpenJK's cvar_t has char *name at
// offset 0 and hashIndex at +0x44, causing crashes.
//
// We return pointers to these SOF2-layout wrappers from Cvar_Get and keep them
// synced with the real engine cvars.
// ============================================================================
typedef struct sof2_cvar_s {
	char    name[64];           // 0x00  inline name (SOF2 ABI: (char*)this == name)
	void   *next;               // 0x40
	char   *string;             // 0x44  ← DLL reads this directly
	char   *resetString;        // 0x48
	char   *latchedString;      // 0x4C
	int     flags;              // 0x50
	int     modified;           // 0x54
	int     modificationCount;  // 0x58
	float   value;              // 0x5C
	int     integer;            // 0x60
} sof2_cvar_t;

#define MAX_SOF2_CVARS 256
static sof2_cvar_t  s_sof2Cvars[MAX_SOF2_CVARS];
static cvar_t      *s_sof2CvarReal[MAX_SOF2_CVARS];
static int          s_sof2CvarCount = 0;

// Sync metadata fields from the real engine cvar to the SOF2 wrapper.
// IMPORTANT: Do NOT sync value/integer on existing wrappers — the DLL writes
// cursor position (and potentially other state) directly to wrapper.value via
// fstp [cvar+0x5c].  Overwriting that from the real cvar would lose the DLL's
// direct writes and cause the cursor to be stuck at its initial position.
static void SOF2_SyncCvar( int idx, qboolean includeValue = qtrue ) {
	cvar_t *r = s_sof2CvarReal[idx];
	s_sof2Cvars[idx].string            = r->string;
	s_sof2Cvars[idx].resetString       = r->resetString;
	s_sof2Cvars[idx].latchedString     = r->latchedString;
	s_sof2Cvars[idx].flags             = r->flags;
	s_sof2Cvars[idx].modified          = r->modified;
	s_sof2Cvars[idx].modificationCount = r->modificationCount;
	if ( includeValue ) {
		s_sof2Cvars[idx].value             = r->value;
		s_sof2Cvars[idx].integer           = r->integer;
	}
}

// Find the SOF2 wrapper index for a given name, or -1 if not found.
static int SOF2_FindCvarByName( const char *name ) {
	if ( !name ) return -1;
	for ( int i = 0; i < s_sof2CvarCount; i++ ) {
		if ( !Q_stricmp( s_sof2Cvars[i].name, name ) )
			return i;
	}
	return -1;
}

// After the engine updates a real cvar (via Cvar_Set/Cvar_SetValue), push the
// new value into the SOF2 wrapper so the DLL sees the change immediately.
static void SOF2_SyncAfterSet( const char *name ) {
	int idx = SOF2_FindCvarByName( name );
	if ( idx >= 0 ) {
		SOF2_SyncCvar( idx, qtrue );  // full sync including value
	}
}

// Cvar_Get wrapper: returns a SOF2-layout cvar to the DLL.
static cvar_t *UI_Cvar_Get_SOF2( const char *name, const char *val, int flags ) {
	cvar_t *real = Cvar_Get( name, val, flags );
	if ( !real ) return NULL;

	// Find existing wrapper for this cvar — sync metadata but NOT value/integer,
	// because the DLL may have written directly to wrapper.value (e.g. cursor pos).
	for ( int i = 0; i < s_sof2CvarCount; i++ ) {
		if ( s_sof2CvarReal[i] == real ) {
			SOF2_SyncCvar( i, qfalse );  // sync string/flags/modified but NOT value
			return (cvar_t *)&s_sof2Cvars[i];
		}
	}

	// Allocate new wrapper
	if ( s_sof2CvarCount >= MAX_SOF2_CVARS ) {
		Com_Error( ERR_FATAL, "UI_Cvar_Get_SOF2: too many cvars (%d)", MAX_SOF2_CVARS );
		return NULL;
	}

	int idx = s_sof2CvarCount++;
	s_sof2CvarReal[idx] = real;
	Q_strncpyz( s_sof2Cvars[idx].name, real->name, sizeof( s_sof2Cvars[idx].name ) );
	s_sof2Cvars[idx].next = NULL;
	SOF2_SyncCvar( idx );

	return (cvar_t *)&s_sof2Cvars[idx];
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
	// Push the new value into the SOF2 wrapper so the DLL sees it immediately.
	// This is critical for cursor initialization: the constructor calls
	// Cvar_SetValue(wrapperPtr, vidWidth/2) and the DLL later reads wrapper.value.
	SOF2_SyncAfterSet( var_name );
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
static int UI_NullSlotStub( void ) { return 0; }

// Per-slot stubs to identify which import slots fire during rendering
#define MAKE_SLOT_STUB(N) \
	static int UI_SlotStub_##N( void ) { \
		static int calls = 0; calls++; \
		if ( calls <= 3 || (calls % 1000 == 0) ) \
			Com_Printf("[DBG] Slot " #N " called (#%d)\n", calls); \
		return 0; \
	}
MAKE_SLOT_STUB(55)
MAKE_SLOT_STUB(58)
MAKE_SLOT_STUB(60)
MAKE_SLOT_STUB(66)
MAKE_SLOT_STUB(67)
MAKE_SLOT_STUB(68)
MAKE_SLOT_STUB(75)
MAKE_SLOT_STUB(76)
MAKE_SLOT_STUB(81)
MAKE_SLOT_STUB(82)
MAKE_SLOT_STUB(83)
// Slot 99: RE_SetClipRegion — SOF2 passes a single int (clip right-edge pixel).
// This OpenJK build's refexport_t has no SetClipRegion, so just no-op.
// Text won't be clipped, but that's fine — ensures it's always visible.
static void UI_RE_SetClipRegion( int clipRightEdge ) {
	// no-op: clipping not supported in this renderer build
}

// Slot 98: RE_SetColor — SOF2 passes 5 ints; (-1,-1,-1,-1,0) = reset to white.
// OpenJK's re.SetColor takes a float[4] rgba array or NULL for white.
static void UI_RE_SetColor5( int r, int g, int b, int a, int flags ) {
	if ( r == -1 && g == -1 && b == -1 && a == -1 ) {
		re.SetColor( NULL );  // reset to default (white)
	} else {
		float color[4] = {
			(float)(r & 0xFF) / 255.0f,
			(float)(g & 0xFF) / 255.0f,
			(float)(b & 0xFF) / 255.0f,
			(float)(a & 0xFF) / 255.0f
		};
		re.SetColor( color );
	}
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

// Slot 64: SE_GetString with fallback — string reference resolver.
// Original SoF2.exe maps slot 64 to SE_GetString (confirmed by decompiling CL_InitUI).
// DLL's UIW_MenuBuffer_ParseNextToken calls this for &REFERENCE& expansion
// (e.g. &MENU_GAME_SINGLE& → "Single Player").
// Since .str string definition files are missing from game assets, we provide a
// hardcoded fallback table for common menu strings.
static char *UI_SE_GetString_WithFallback( const char *token, int flags ) {
	if ( !token || !*token ) return (char *)"";

	// Try the real string table first (for when .str files exist)
	const char *result = SE_GetString( token );
	if ( result && *result ) return (char *)result;

	// Fallback: hardcoded lookup for the most important menu strings
	static const struct { const char *ref; const char *text; } known[] = {
		// Main menu
		{"MENU_GAME_SINGLE",     "Single Player"},
		{"MENU_GAME_RMG",        "Random Mission"},
		{"MENU_GAME_OPTIONS",    "Options"},
		{"MENU_GAME_CREDITS",    "Credits"},
		{"MENU_GAME_QUIT",       "Quit"},
		{"MENU_GAME_BACK",       "Back"},
		{"MENU_GAME_LOADGAME",   "Load Game"},
		{"MENU_GAME_LOADAUTO",   "Load Autosave"},
		{"MENU_GAME_LOADAGAME",  "Load a Game"},
		{"MENU_GAME_SAVEGAME",   "Save Game"},
		{"MENU_GAME_START",      "Start Game"},
		{"MENU_GAME_TUT",        "Tutorial"},
		{"MENU_GAME_LANG",       "Language"},
		{"MENU_GAME_CONTROLS",   "Controls"},
		{"MENU_GAME_SOUND",      "Sound"},
		{"MENU_GAME_SDIFF",      "Select Difficulty"},
		// Generic
		{"MENU_GENERIC_YES",     "Yes"},
		{"MENU_GENERIC_NO",      "No"},
		{"MENU_GENERIC_OK",      "OK"},
		{"MENU_GENERIC_GAME",    "Game"},
		{"MENU_GENERIC_LOADING", "Loading..."},
		{"MENU_GENERIC_GENERATING", "Generating..."},
		{"MENU_GENERIC_RETURN_TO_GAME", "Return to Game"},
		{"MENU_GENERIC_APPLY_CHANGES",  "Apply Changes"},
		{"MENU_GENERIC_APPLY_DEFAULTS", "Restore Defaults"},
		{"MENU_GENERIC_ENGLISH",   "English"},
		{"MENU_GENERIC_AMERICAN",  "American"},
		{"MENU_GENERIC_GERMAN",    "German"},
		{"MENU_GENERIC_FRENCH",    "French"},
		{"MENU_GENERIC_ITALIAN",   "Italian"},
		{"MENU_GENERIC_SPANISH",   "Spanish"},
		{"MENU_GENERIC_MISSIONINFO", "Mission Info"},
		{"MENU_GENERIC_OBJS",       "Objectives"},
		{"MENU_GENERIC_WEAPONINFO",  "Weapon Info"},
		// Difficulty
		{"GENERIC_EASY",       "Easy"},
		{"GENERIC_MEDIUM",     "Medium"},
		{"GENERIC_HARD",       "Hard"},
		{"GENERIC_SUPERHARD",  "Extreme"},
		{"GENERIC_CUSTOM",     "Custom"},
		// Audio
		{"MENU_AUDIO_DYNAMIC",      "Audio Settings"},
		{"MENU_AUDIO_FX_VOLUME",    "Effects Volume"},
		{"MENU_AUDIO_MUSIC_VOLUME", "Music Volume"},
		// Keys/controls
		{"MENU_KEYS_MISC",          "Miscellaneous"},
		{"MENU_KEYS_DEFAULTS",      "Restore Defaults"},
		// Inventory
		{"MENU_INV_SELECT_REC",  "Recommended"},
		{"MENU_INV_GO_BRIEF",    "Mission Briefing"},
		{"MENU_INV_DEPLOY",      "Deploy"},
		{NULL, NULL}
	};

	for ( int i = 0; known[i].ref; i++ ) {
		if ( Q_stricmp( token, known[i].ref ) == 0 )
			return (char *)known[i].text;
	}

	// Ultimate fallback: return the reference name itself
	static char fallback[256];
	Q_strncpyz( fallback, token, sizeof( fallback ) );
	return fallback;
}

// Slot 86: R_RegisterFont — SOF2's DLL calls this as RegisterFont(name, size, scaleRaw, flags).
// OpenJK's re.RegisterFont takes (const char *name) and returns an int handle.
//
// SOF2 has font names (small, medium, title, credmed) that don't have .fontdat files
// in any PK3.  The original SOF2 renderer created these internally.  Map them to the
// closest available fonts that DO have .fontdat files.
static int UI_RE_RegisterFont_SOF2( const char *name, int size, int scaleRaw, int flags ) {
	(void)size; (void)scaleRaw; (void)flags;

	const char *actual = name;
	// Map missing SOF2 fonts to available ones
	if ( name ) {
		if ( !Q_stricmp( name, "small" ) )        actual = "lcdsmall";
		else if ( !Q_stricmp( name, "medium" ) )   actual = "lcd";
		else if ( !Q_stricmp( name, "title" ) )    actual = "credtitle";
		else if ( !Q_stricmp( name, "credmed" ) )  actual = "lcd";
	}

	int h = re.RegisterFont( actual );
	if ( h == 0 ) h = re.RegisterFont( "lcd" );  // ultimate fallback
	Com_Printf( "RegisterFont: '%s'%s%s%s -> %d\n",
		name ? name : "(null)",
		(actual != name) ? " (mapped to '" : "",
		(actual != name) ? actual : "",
		(actual != name) ? "')" : "",
		h );
	return h;
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
int g_stretchPicCount = 0;
// Helper: unpack SOF2 packed RGBA (uint32) to float[4] color
static void UI_UnpackColor( unsigned int color, float *rgba ) {
	rgba[0] = (float)( color        & 0xFF) / 255.0f;
	rgba[1] = (float)((color >>  8) & 0xFF) / 255.0f;
	rgba[2] = (float)((color >> 16) & 0xFF) / 255.0f;
	rgba[3] = (float)((color >> 24) & 0xFF) / 255.0f;
}

static void UI_RE_StretchPic( float x, float y, float w, float h, int color, int hShader, int adjustFrom ) {
	g_stretchPicCount++;
	if (g_stretchPicCount <= 30) {
		Com_Printf("[DBG] StretchPic #%d: x=%.0f y=%.0f w=%.0f h=%.0f color=0x%08X shader=%d adj=%d\n",
			g_stretchPicCount, x, y, w, h, (unsigned int)color, hShader, adjustFrom);
	}
	// Apply SOF2 packed RGBA color tint before drawing
	if ( color != 0 && color != (int)0xFFFFFFFF ) {
		float rgba[4];
		UI_UnpackColor( (unsigned int)color, rgba );
		re.SetColor( rgba );
	}
	re.DrawStretchPic( x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, hShader );
	if ( color != 0 && color != (int)0xFFFFFFFF ) {
		re.SetColor( NULL );
	}
}

// Slot 95: RE_FillRect — SOF2 fills a rectangle with a packed RGBA color.
// Signature: void RE_FillRect(float x, float y, float w, float h, unsigned int color)
static void UI_RE_FillRect( float x, float y, float w, float h, unsigned int color ) {
	if ((color & 0xFF000000) == 0) return; // fully transparent
	float rgba[4];
	UI_UnpackColor( color, rgba );
	re.SetColor( rgba );
	re.DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re.SetColor( NULL );
}

// Slot 96: RE_DrawLine — SOF2 draws a 2D line between two points.
// Signature: void RE_DrawLine(float x1, float y1, float x2, float y2, float width, unsigned int color)
static void UI_RE_DrawLine( float x1, float y1, float x2, float y2, float width, unsigned int color ) {
	if ((color & 0xFF000000) == 0) return;
	float rgba[4];
	UI_UnpackColor( color, rgba );
	re.SetColor( rgba );
	// Approximate: draw a thin filled rectangle along the line
	float dx = x2 - x1, dy = y2 - y1;
	if (dx < 0) { dx = -dx; x1 = x2; }
	if (dy < 0) { dy = -dy; y1 = y2; }
	if (dx == 0) { // vertical
		int hw = (int)(width + 1) >> 1;
		re.DrawStretchPic( x1 - hw, y1, width, dy, 0, 0, 0, 0, cls.whiteShader );
	} else { // horizontal
		int hh = (int)(width + 1) >> 1;
		re.DrawStretchPic( x1, y1 - hh, dx, width, 0, 0, 0, 0, cls.whiteShader );
	}
	re.SetColor( NULL );
}

// Slot 97: RE_DrawBox — SOF2 draws a filled box with outline.
// Signature: void RE_DrawBox(int x, int y, int w, int h, unsigned int color)
static void UI_RE_DrawBox( int x, int y, int w, int h, unsigned int color ) {
	// Fill
	UI_RE_FillRect( (float)x, (float)y, (float)w, (float)h, color );
	// Outline (alpha-only from high byte)
	unsigned int outlineColor = color & 0xFF000000;
	UI_RE_DrawLine( (float)(x-1), (float)(y-1), (float)(x+w), (float)(y-1), 1.0f, outlineColor );
	UI_RE_DrawLine( (float)(x-1), (float)(y+h), (float)(x+w), (float)(y+h), 1.0f, outlineColor );
	UI_RE_DrawLine( (float)(x-1), (float)(y-1), (float)(x-1), (float)(y+h), 1.0f, outlineColor );
	UI_RE_DrawLine( (float)(x+w), (float)(y-1), (float)(x+w), (float)(y+h), 1.0f, outlineColor );
}

// Slot 87: RE_Font_HeightPixels — font height in pixels scaled by 'scale'.
// SOF2 signature: float RE_Font_HeightPixels(uint fontHandle, float scale)
// OpenJK's re.Font_HeightPixels returns int; SOF2 DLL expects float return.
// Safety: never return 0 — DLL may divide by font height.
static float UI_RE_Font_HeightPixels( unsigned int fontHandle, float scale ) {
	int px = re.Font_HeightPixels( (int)fontHandle, scale );
	if ( px <= 0 ) px = 14;  // fallback to reasonable height
	return (float)px;
}

// Slot 88: RE_Font_StrLenPixels — pixel width of a string.
// SOF2 signature: int RE_Font_StrLenPixels(char *text, int setIndex, float scale)
static int UI_RE_Font_StrLenPixels( const char *text, int setIndex, float scale ) {
	return re.Font_StrLenPixels( text, setIndex, scale );
}

// Slot 89: RE_Font_DrawString — draw a text string.
// SOF2 DLL pushes 7 values (verified from UI_Widget_DrawText disasm at 0x4000cc60):
//   p0 = float ox (bit pattern)       → e.g. 0 = 0.0
//   p1 = float oy (bit pattern)       → e.g. 0x43580000 = 216.0
//   p2 = const char *text
//   p3 = color (packed uint32 or float[4] ptr)
//   p4 = int setIndex/fontHandle
//   p5 = float scale (bit pattern)    → e.g. 0x3F800000 = 1.0
//   p6 = int iMaxPixelWidth or style  → 0 = unlimited
//
// CRITICAL: The DLL may pass rgba (param 3) as either a float[4] pointer OR a packed
// uint32 color.  Accept all params as raw ints and safely probe to determine which case.
// Uses Windows SEH (__try/__except) to guard against bad pointer dereferences.
static void UI_RE_Font_DrawString_Safe( int p0, int p1, int p2, int p3,
                                        int p4, int p5, int p6 ) {
	const char *text = (const char *)p2;
	if ( !text || !*text ) return;

	// SOF2 passes ox/oy/scale as floats, reinterpret the raw int bits
	float ox = *(float *)&p0;
	float oy = *(float *)&p1;
	int setIndex = p4;
	float scale = *(float *)&p5;
	// p6 = iMaxPixelWidth (0 = unlimited), passed through to renderer

	// Determine rgba: if p3 looks like a valid heap/stack pointer (>64K),
	// try to use it as float[4].  Otherwise fall back to white.
	float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	const float *color = white;
	if ( (unsigned int)p3 > 0x10000 ) {
		// Likely a valid pointer — probe with SEH to be safe
		const float *rgba = (const float *)p3;
		__try {
			if ( rgba[0] >= 0.0f && rgba[0] <= 1.0f &&
			     rgba[3] >= 0.0f && rgba[3] <= 2.0f ) {
				color = rgba;
			}
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			// Bad pointer — use white
		}
	}

	if ( scale <= 0.0f || scale > 10.0f ) scale = 1.0f;

	static int s_drawCalls = 0;
	if ( ++s_drawCalls <= 20 ) {
		Com_Printf("[DBG] Font_DrawString #%d: ox=%.1f oy=%.1f text='%.32s' set=%d scale=%.2f p6=%d\n",
			s_drawCalls, ox, oy, text, setIndex, scale, p6);
	}

	re.Font_DrawString( (int)ox, (int)oy, text, color, setIndex, p6, scale );
}


// Traced sound wrappers — log what sounds the DLL tries to play
static int UI_S_RegisterSound_Traced( const char *name ) {
	sfxHandle_t h = S_RegisterSound( name );
	Com_Printf( "[SND] RegisterSound: '%s' -> %d\n", name ? name : "(null)", (int)h );
	return (int)h;
}
static void UI_S_StartLocalSound_Traced( sfxHandle_t sfxHandle, int channelNum ) {
	// Completely suppress "invalid" sound (handle 2) after the first few plays —
	// this fires every frame when DLL cursor state is stuck, creating a rhythmic beep
	static int invalidCount = 0;
	if ( (int)sfxHandle == 2 ) {
		if ( ++invalidCount > 3 ) return;  // allow first 3 then silence
		Com_Printf("[SND] StartLocalSound: INVALID #%d chan=%d\n", invalidCount, channelNum);
	}

	// Rate-limit all other sounds: max 1 per 500ms to prevent spam
	static int lastTime = 0;
	static int callsThisWindow = 0;
	int now = Sys_Milliseconds();
	if ( now - lastTime > 500 ) { lastTime = now; callsThisWindow = 0; }
	if ( ++callsThisWindow > 1 ) return;

	if ( sfxHandle > 0 ) S_StartLocalSound( sfxHandle, channelNum );
}

// s_music command handler — SOF2 DLL issues "s_music <track>" via Cbuf_ExecuteText
// during menu init.  Without this, the command falls through to UI_GameCommand
// re-entrant path and causes issues.
//
// SOF2 dynamic music uses _fast/_slow suffixes (e.g. "music/shop/shopa" expands to
// "music/shop/shopa_slow.mp3" for ambient and "music/shop/shopa_fast.mp3" for action).
// For menu music we play the _slow (ambient) variant.
extern qboolean S_FileExists( const char *psFilename );
static void CL_SMusic_f( void ) {
	const char *track = Cmd_Argc() > 1 ? Cmd_Argv(1) : "";
	Com_Printf( "[SND] s_music: '%s'\n", track );

	if ( !track[0] ) {
		S_StopBackgroundTrack();
		return;
	}

	// Try exact name first (e.g. "music/shop/shopa.mp3")
	char tryName[MAX_QPATH];
	Com_sprintf( tryName, sizeof(tryName), "%s.mp3", track );
	if ( S_FileExists( tryName ) ) {
		Com_Printf( "[SND] s_music: playing '%s'\n", tryName );
		S_StartBackgroundTrack( track, track, qfalse );
		return;
	}

	// Try SOF2 _slow suffix for ambient/menu music
	Com_sprintf( tryName, sizeof(tryName), "%s_slow.mp3", track );
	if ( S_FileExists( tryName ) ) {
		char slowTrack[MAX_QPATH];
		Com_sprintf( slowTrack, sizeof(slowTrack), "%s_slow", track );
		Com_Printf( "[SND] s_music: playing '%s' (slow variant)\n", tryName );
		S_StartBackgroundTrack( slowTrack, slowTrack, qfalse );
		return;
	}

	// Try SOF2 _fast suffix as last resort
	Com_sprintf( tryName, sizeof(tryName), "%s_fast.mp3", track );
	if ( S_FileExists( tryName ) ) {
		char fastTrack[MAX_QPATH];
		Com_sprintf( fastTrack, sizeof(fastTrack), "%s_fast", track );
		Com_Printf( "[SND] s_music: playing '%s' (fast variant)\n", tryName );
		S_StartBackgroundTrack( fastTrack, fastTrack, qfalse );
		return;
	}

	Com_Printf( "[SND] s_music: no matching file for '%s'\n", track );
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
	Com_Printf("=== OpenSOF2 build %s %s ===\n", __DATE__, __TIME__);

#ifdef JK2_MODE
	JK2SP_Register("keynames", 0	/*SP_REGISTER_REQUIRED*/);		// reference is KEYNAMES
#endif

	// Ensure sound is unmuted — OpenJK starts with s_soundMuted=qtrue and only
	// clears it in S_BeginRegistration().  Without this call, all sound playback
	// (music AND effects) is silently suppressed during the main menu.
	S_BeginRegistration();

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



		// [28-35] Cvar — SOF2 cvar ABI wrapper (inline name[64] at offset 0)
		import.Cvar_Get                  = UI_Cvar_Get_SOF2;
		// NULL-safe wrapper: DLL calls Cvar_Set(0) during MenuSystem init to
		// reset some state; guard against crashing on NULL var_name.
		struct CvarSetSafe {
			static void Set( const char *name, const char *value ) {
				// Guard against NULL name — DLL calls Cvar_Set(NULL) during ConsoleCommand.
				if ( !name ) return;
				Cvar_Set( name, value );
				// Push the new value into the SOF2 wrapper so the DLL sees the
				// update immediately (e.g. for cursor position cvars).
				SOF2_SyncAfterSet( name );
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
		import.reserved_58  = (void *)UI_SlotStub_58;
		import.Key_ClearStates    = Key_ClearStates;
		// slot 60: unknown — stub for safety
		import.reserved_60  = (void *)UI_SlotStub_60;
		// slot 61: ACTUAL FS_ReadFile (verified from UIW_MenuFile_Init disasm: CALL [0x400376cc])
		// Our struct placed FS_ReadFile at slot 9 but the DLL uses slot 61. Fix here.
		import.reserved_61  = (void *)UI_FS_ReadFile_Traced;
		// slot 62: ACTUAL FS_FreeFile for success path (verified: CALL [0x400376d0])
		import.reserved_62  = (void *)FS_FreeFile;
		import.SP_GetStringTextByIndex = UI_SP_GetStringTextByIndex;
		// slot 64: SE_GetString with fallback (verified: original SoF2.exe CL_InitUI maps this to SE_GetString).
		// DLL's UIW_MenuBuffer_ParseNextToken calls this for &REFERENCE& expansion.
		// Previous mapping to Key_GetBindingByName was wrong — caused all menu text to be empty.
		import.SE_GetString       = UI_SE_GetString_WithFallback;
		import.SE_GetString2      = UI_SE_GetString;

		// [66-74] Client / G2
		// slot 66: UI_MenuPage_RenderFrame calls this WITHOUT a null-check — must be a callable.
		// SoF2.exe provides RE_RenderScene here; a stub returning 0 is safe (no map loaded yet).
		import.reserved_66  = (void *)UI_SlotStub_66;
		// slot 67: DLL calls with no args + checks return value as gate.
		// Original SoF2.exe provides UI_RE_RenderScene here (returns 0 normally).
		import.reserved_67  = (void *)UI_SlotStub_67;
		// slot 68: G2API_CleanGhoul2Models variant — stub (not needed for menus)
		import.reserved_68  = (void *)UI_SlotStub_68;
		import.G2_SpawnGhoul2Model    = UI_G2_SpawnGhoul2Model;
		import.G2_PlayModelAnimation  = UI_G2_PlayModelAnimation;
		// slots 71-73: engine renderer wrappers needed for 3D model rendering in menus
		import.reserved_71  = (void *)UI_RE_RegisterModel;
		import.reserved_72  = (void *)UI_RE_AddRefEntityToScene;
		import.reserved_73  = (void *)UI_RE_ClearScene;
		import.CL_IsCgameActive       = UI_CL_IsCgameActive;

		// [75-77] Save game
		import.reserved_75  = (void *)UI_SlotStub_75;   // SV_LoadGame variant — stub
		import.reserved_76  = (void *)UI_SlotStub_76;   // SV_SaveGame variant — stub
		import.SV_WipeSavegame        = UI_SV_WipeSavegame;

		// [78-83] Sound module
		// slot 78 (snd+0x14): S_RegisterSoundSimple — traced wrapper
		import.S_MuteSound      = (void *)UI_S_RegisterSound_Traced;   // slot 78 = S_RegisterSound
		import.S_StartLocalSound= (void *)UI_S_StartLocalSound_Traced; // slot 79 = S_StartLocalSound
		import.S_StopAllSounds  = (void *)S_StopAllSounds;   // slot 80 = S_StopAllSounds
		import.S_reserved_81    = (void *)UI_SlotStub_81;               // slot 81 = snd+0x64
		import.S_reserved_82    = (void *)UI_SlotStub_82;   // slot 82 = snd+0x24 — stub
		import.S_reserved_83    = (void *)UI_SlotStub_83;   // slot 83 = snd+0x28 — stub

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
		import.RE_DamageSurface = (void *)UI_RE_RegisterFont_SOF2;
		// slot 87 (SOF2 re+0x8c): RE_Font_HeightPixels(uint fontHandle, float scale) -> float
		import.RE_reserved_87   = (void *)UI_RE_Font_HeightPixels;
		// slot 88 (SOF2 re+0x90): RE_Font_StrLenPixels(char *text, int setIndex, float scale) -> int
		import.RE_reserved_88   = (void *)UI_RE_Font_StrLenPixels;
		// slot 89 (SOF2 re+0x94): RE_Font_DrawString(int ox, int oy, char *text, float *rgba, int setIndex, int limit, float scale)
		import.RE_reserved_89   = (void *)UI_RE_Font_DrawString_Safe;
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
		// SOF2 re+0x98 = RE_FillRect, re+0x9c = RE_DrawLine, re+0xa0 = RE_DrawBox
		// These are SOF2-specific renderer calls; implement via OpenJK SetColor+DrawStretchPic.
		import.RE_reserved_95   = (void *)UI_RE_FillRect;                // re+0x98 = RE_FillRect
		import.RE_reserved_96   = (void *)UI_RE_DrawLine;               // re+0x9c = RE_DrawLine
		import.RE_reserved_97   = (void *)UI_RE_DrawBox;                // re+0xa0 = RE_DrawBox
		import.RE_reserved_98   = (void *)UI_RE_SetColor5;             // re+0xa8 = RE_SetColor (5 int params; -1,-1,-1,-1,0 = reset)
		import.RE_reserved_99   = (void *)UI_RE_SetClipRegion;         // re+0xac = RE_SetClipRegion (1 int param; clip right edge)

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

		// Register s_music command before DLL init — the DLL issues
		// "s_music <track>" via Cbuf_ExecuteText during menu setup.
		Cmd_AddCommand( "s_music", CL_SMusic_f );

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


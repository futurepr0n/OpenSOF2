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

// sv_game.c -- interface to the game dll

#include "../server/exe_headers.h"

#include "../qcommon/cm_local.h"
#include "../qcommon/strippublic.h"
#include "../client/snd_public.h"

#include "server.h"
#include <intrin.h>
#include <windows.h>
#include <dbghelp.h>
#include <map>
#include <string>
#pragma comment(lib, "dbghelp.lib")

// VEH crash handler — fires before SEH, works even with corrupted SEH chain
static LONG WINAPI SV_VectoredHandler(EXCEPTION_POINTERS *ep) {
	// Only handle access violations and stack overflows
	DWORD code = ep->ExceptionRecord->ExceptionCode;
	if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW &&
		code != 0xC0000005 && code != 0xC00000FD)
		return EXCEPTION_CONTINUE_SEARCH;

	FILE *crashf = fopen("dbg_veh_crash.txt", "w");
	if (crashf) {
		CONTEXT *ctx = ep->ContextRecord;
		fprintf(crashf, "VEH: code=0x%08lX addr=%p\n", (unsigned long)code,
			ep->ExceptionRecord->ExceptionAddress);
		fprintf(crashf, "EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX\n",
			ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx);
		fprintf(crashf, "ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX EIP=%08lX\n",
			ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp, ctx->Eip);
		// Stack dump
		DWORD *esp = (DWORD *)ctx->Esp;
		for (int s = 0; s < 32; s++) {
			__try { fprintf(crashf, "ESP+%02X: %08lX\n", s*4, esp[s]); }
			__except(EXCEPTION_EXECUTE_HANDLER) { fprintf(crashf, "ESP+%02X: <BAD>\n", s*4); }
		}
		fflush(crashf);
		fclose(crashf);
	}
	// Also write to stderr — full details for first 3 crashes
	static int vehLogCount = 0;
	if ( vehLogCount < 3 ) {
		CONTEXT *ctx2 = ep->ContextRecord;
		fprintf(stderr, "[VEH] CRASH #%d code=0x%08lX EIP=%08lX\n", vehLogCount+1,
			(unsigned long)code, ctx2->Eip);
		fprintf(stderr, "[VEH]   EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX\n",
			ctx2->Eax, ctx2->Ebx, ctx2->Ecx, ctx2->Edx);
		fprintf(stderr, "[VEH]   ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX\n",
			ctx2->Esi, ctx2->Edi, ctx2->Ebp, ctx2->Esp);
		if ( code == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2 ) {
			fprintf(stderr, "[VEH]   AV %s addr=%08lX\n",
				ep->ExceptionRecord->ExceptionInformation[0] ? "WRITE" : "READ",
				(unsigned long)ep->ExceptionRecord->ExceptionInformation[1]);
		}
		// Stack trace (return addresses)
		DWORD *esp2 = (DWORD *)ctx2->Esp;
		fprintf(stderr, "[VEH]   Stack:");
		for (int s2 = 0; s2 < 12; s2++) {
			__try { fprintf(stderr, " %08lX", esp2[s2]); }
			__except(EXCEPTION_EXECUTE_HANDLER) { fprintf(stderr, " <BAD>"); }
		}
		fprintf(stderr, "\n");
		fflush(stderr);
		vehLogCount++;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

// SEH crash handler for debugging DLL calls — resolves symbol from PDB
static LONG WINAPI SV_CrashFilter(EXCEPTION_POINTERS *ep) {
	HMODULE hExe = GetModuleHandleA(NULL);
	DWORD_PTR crashAddr = (DWORD_PTR)ep->ExceptionRecord->ExceptionAddress;
	DWORD_PTR crashRVA = crashAddr - (DWORD_PTR)hExe;
	fprintf(stderr, "[CRASH] Unhandled exception! Code=0x%08lX Address=%p (RVA=0x%08lX) Base=%p\n",
		(unsigned long)ep->ExceptionRecord->ExceptionCode,
		(void*)crashAddr, (unsigned long)crashRVA, (void*)hExe);

	// Dump registers for crash debugging
	CONTEXT *ctx_ptr = ep->ContextRecord;
	fprintf(stderr, "[CRASH] EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX\n",
		ctx_ptr->Eax, ctx_ptr->Ebx, ctx_ptr->Ecx, ctx_ptr->Edx);
	fprintf(stderr, "[CRASH] ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX\n",
		ctx_ptr->Esi, ctx_ptr->Edi, ctx_ptr->Ebp, ctx_ptr->Esp);
	fprintf(stderr, "[CRASH] EIP=%08lX\n", ctx_ptr->Eip);

	// Dump top of stack to find return address (critical when EIP=0)
	DWORD *esp = (DWORD *)ctx_ptr->Esp;
	fprintf(stderr, "[CRASH] Stack dump (ESP=%p):\n", (void*)esp);
	for (int s = 0; s < 16; s++) {
		__try {
			fprintf(stderr, "[CRASH]   ESP+%02X: %08lX", s*4, esp[s]);
			// Check if it looks like a code address (in gamex86.dll range 0x20000000+ or exe range)
			if ((esp[s] >= 0x20000000 && esp[s] < 0x20200000) ||
				(esp[s] >= 0x00400000 && esp[s] < 0x00C00000))
				fprintf(stderr, " <-- possible return addr");
			fprintf(stderr, "\n");
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			fprintf(stderr, "[CRASH]   ESP+%02X: <unreadable>\n", s*4);
		}
	}

	// Try to resolve symbol name from PDB
	HANDLE process = GetCurrentProcess();
	SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
	if (SymInitialize(process, NULL, TRUE)) {
		char symBuf[sizeof(SYMBOL_INFO) + 256];
		SYMBOL_INFO *sym = (SYMBOL_INFO*)symBuf;
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = 255;
		DWORD64 displacement64 = 0;
		if (SymFromAddr(process, (DWORD64)crashAddr, &displacement64, sym)) {
			fprintf(stderr, "[CRASH] Symbol: %s+0x%llx\n", sym->Name, (unsigned long long)displacement64);
		} else {
			fprintf(stderr, "[CRASH] SymFromAddr failed: %lu\n", GetLastError());
		}
		// Also try to get source file + line number
		IMAGEHLP_LINE64 line;
		line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
		DWORD displacement32 = 0;
		if (SymGetLineFromAddr64(process, (DWORD64)crashAddr, &displacement32, &line)) {
			fprintf(stderr, "[CRASH] Source: %s:%lu\n", line.FileName, line.LineNumber);
		}
		// Walk the call stack
		fprintf(stderr, "[CRASH] Call stack:\n");
		CONTEXT ctx = *ep->ContextRecord;
		STACKFRAME64 frame;
		memset(&frame, 0, sizeof(frame));
		frame.AddrPC.Offset = ctx.Eip;
		frame.AddrPC.Mode = AddrModeFlat;
		frame.AddrFrame.Offset = ctx.Ebp;
		frame.AddrFrame.Mode = AddrModeFlat;
		frame.AddrStack.Offset = ctx.Esp;
		frame.AddrStack.Mode = AddrModeFlat;
		for (int i = 0; i < 20; i++) {
			if (!StackWalk64(IMAGE_FILE_MACHINE_I386, process, GetCurrentThread(),
					&frame, &ctx, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL))
				break;
			if (SymFromAddr(process, frame.AddrPC.Offset, &displacement64, sym)) {
				fprintf(stderr, "[CRASH]   #%d: %s+0x%llx (0x%08llx)\n", i, sym->Name,
					(unsigned long long)displacement64, (unsigned long long)frame.AddrPC.Offset);
			} else {
				fprintf(stderr, "[CRASH]   #%d: 0x%08llx (unknown)\n", i,
					(unsigned long long)frame.AddrPC.Offset);
			}
		}
		SymCleanup(process);
	} else {
		fprintf(stderr, "[CRASH] SymInitialize failed: %lu\n", GetLastError());
	}
	fflush(stderr);
	return EXCEPTION_CONTINUE_SEARCH;
}

#include "../client/vmachine.h"
#include "../client/client.h"
#include "qcommon/ojk_saved_game.h"
#include "qcommon/ojk_saved_game_helper.h"  // needed for write_chunk/read_chunk template bodies
/*#include "..\renderer\tr_local.h"
#include "..\renderer\tr_WorldEffects.h"*/
/*
Ghoul2 Insert Start
*/
#if !defined(G2_H_INC)
	#include "../ghoul2/G2.h"
#endif

/*
Ghoul2 Insert End
*/

static void *gameLibrary;

//prototypes
extern void Com_WriteCam ( const char *text );
extern void Com_FlushCamFile();

// Forward declarations for tracing wrappers
void SV_GetServerinfo( char *buffer, int bufferSize );
qboolean SV_GetEntityToken( char *buffer, int bufferSize );
extern char ** FS_ListFilteredFiles( const char *path, const char *extension, char *filter, int *numfiles );
extern void FS_FreeFileList( char **fileList );

// --- Tracing wrappers to identify which gi[] call crashes ---
static int gi_call_count = 0;

// Traced wrappers for ALL commonly-called slots

static void QDECL SV_Traced_Printf( const char *fmt, ... ) {
	gi_call_count++;
	va_list args;
	va_start( args, fmt );
	char buf[4096];
	Q_vsnprintf( buf, sizeof(buf), fmt, args );
	va_end( args );
	// Log first 200 chars of the actual message for crash debugging
	char preview[201];
	strncpy(preview, buf, 200);
	preview[200] = '\0';
	// Strip trailing newlines for cleaner log
	for (int i = (int)strlen(preview)-1; i >= 0 && (preview[i]=='\n'||preview[i]=='\r'); i--)
		preview[i] = '\0';
	fprintf(stderr, "[GI] gi_Printf called (#%d) msg='%s'\n", gi_call_count, preview);
	fflush(stderr);
	Com_Printf( "%s", buf );
}

static void QDECL SV_Traced_DPrintf( const char *fmt, ... ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_DPrintf called (#%d)\n", gi_call_count);
	va_list args;
	va_start( args, fmt );
	char buf[4096];
	Q_vsnprintf( buf, sizeof(buf), fmt, args );
	va_end( args );
	Com_DPrintf( "%s", buf );
}

static int SV_Traced_EventLoop( void ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_Com_EventLoop called (#%d)\n", gi_call_count);
	return Com_EventLoop();
}

static void *SV_Traced_Cvar_Get( const char *name, const char *value, int flags ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_Cvar_Get called (#%d) name='%s'\n", gi_call_count, name ? name : "(null)");
	return Cvar_Get( name, value, flags );
}

static void SV_Traced_Cvar_Set( const char *name, const char *value, int /*force*/ ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_Cvar_Set called (#%d) name='%s'\n", gi_call_count, name ? name : "(null)");
	Cvar_Set( name, value );
}

static void *SV_Traced_ZMalloc( int size, int tag, qboolean zeroIt ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_Z_Malloc called (#%d) size=%d tag=%d zero=%d\n", gi_call_count, size, tag, (int)zeroIt);
	void *result = Z_Malloc( size, (memtag_t)tag, zeroIt );
	fprintf(stderr, "[GI] gi_Z_Malloc returned %p\n", result);
	return result;
}

static void SV_Traced_ZFree( void *ptr ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_Z_Free called (#%d) ptr=%p\n", gi_call_count, ptr);
	Z_Free( ptr );
}

// SV_Traced_LocateGameData is defined further below after SV_LocateGameData_SOF2
static void SV_Traced_LocateGameData( void *ents, int numEnts, int entSize, void *clients, int clientSize );

static int SV_Traced_FS_FOpenFile( const char *path, int *handle, int mode ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_FS_FOpenFileByMode called (#%d) path='%s' mode=%d\n",
		gi_call_count, path ? path : "(null)", mode);
	return FS_FOpenFileByMode( path, (fileHandle_t*)handle, (fsMode_t)mode );
}

static void SV_Traced_SetConfigstring( int index, const char *val ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_SetConfigstring called (#%d) index=%d\n", gi_call_count, index);
	SV_SetConfigstring( index, val );
}

static void SV_Traced_GetServerinfo( char *buf, int bufSize ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_GetServerinfo called (#%d)\n", gi_call_count);
	SV_GetServerinfo( buf, bufSize );
}

static void SV_Traced_LinkEntity( gentity_t *ent ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_LinkEntity called (#%d) ent=%p\n", gi_call_count, (void*)ent);
	SV_LinkEntity( ent );
}

// gi[3]: Com_sprintf traced
static void SV_Traced_ComSprintf( char *dest, int size, const char *fmt, ... ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_Com_sprintf called (#%d) fmt='%.60s'\n", gi_call_count, fmt ? fmt : "(null)");
	va_list args;
	va_start( args, fmt );
	Q_vsnprintf( dest, size, fmt, args );
	va_end( args );
}

// gi[6]: FS_Read traced
static int SV_Traced_FS_Read( void *buffer, int len, fileHandle_t f ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_FS_Read called (#%d) len=%d handle=%d\n", gi_call_count, len, (int)f);
	return FS_Read( buffer, len, f );
}

// gi[8]: FS_FCloseFile traced
static void SV_Traced_FS_FCloseFile( fileHandle_t f ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_FS_FCloseFile called (#%d) handle=%d\n", gi_call_count, (int)f);
	FS_FCloseFile( f );
}

// gi[9]: FS_ReadFile traced
static int SV_Traced_FS_ReadFile( const char *path, void **buffer ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_FS_ReadFile called (#%d) path='%s'\n", gi_call_count, path ? path : "(null)");
	return FS_ReadFile( path, buffer );
}

// gi[10]: FS_FreeFile traced
static void SV_Traced_FS_FreeFile( void *buffer ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_FS_FreeFile called (#%d) buffer=%p\n", gi_call_count, buffer);
	FS_FreeFile( buffer );
}

// gi[12]: FS_ListFiles — SOF2 format (4 params, returns 2-header + 3-dword-per-file entries)
// SOF2 FS_ListFiles takes (path, ext, filter, &numfiles) — NOT Q3A's 3-param version.
// Return format: int array where:
//   [0] = file count (for our FreeFileList to know how many entries)
//   [1] = reserved (0)
//   [2+i*3+0] = filename string pointer (char*)
//   [2+i*3+1] = 0 (unknown field, DLL doesn't read it)
//   [2+i*3+2] = 0 (unknown field, DLL doesn't read it)
// DLL reads: starts at retval+8 (offset 2 dwords), strides by 12 bytes (3 dwords).
static void *SV_SOF2_FS_ListFiles( const char *path, const char *extension, char *filter, int *numfiles ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_FS_ListFiles called (#%d) path='%s' ext='%s' filter=%p numfiles=%p\n",
		gi_call_count, path ? path : "(null)", extension ? extension : "(null)",
		(void*)filter, (void*)numfiles);

	// Get Q3A-format file list
	int count = 0;
	char **q3list = FS_ListFilteredFiles( path, extension, filter, &count );

	// Allocate SOF2-format list: 2 header dwords + 3 dwords per file
	int totalDwords = 2 + count * 3;
	int *sof2list = (int *)Z_Malloc( totalDwords * sizeof(int), TAG_FILESYS, qtrue );

	sof2list[0] = count;  // header[0]: file count (used by our FreeFileList)
	sof2list[1] = 0;      // header[1]: reserved

	// Steal string pointers from Q3A list (avoid double-alloc)
	for ( int i = 0; i < count; i++ ) {
		sof2list[2 + i*3 + 0] = (int)q3list[i];  // filename pointer
		sof2list[2 + i*3 + 1] = 0;                // unknown field
		sof2list[2 + i*3 + 2] = 0;                // unknown field
		q3list[i] = NULL;  // prevent FS_FreeFileList from freeing stolen strings
	}

	// Free Q3A array (strings already stolen, NULLed out)
	FS_FreeFileList( q3list );

	if ( numfiles ) {
		*numfiles = count;
	}

	fprintf(stderr, "[GI] gi_FS_ListFiles returning %d files, sof2list=%p\n", count, (void*)sof2list);
	return (void *)sof2list;
}

// gi[13]: FS_FreeFileList — SOF2 format (matches SV_SOF2_FS_ListFiles above)
static void SV_SOF2_FS_FreeFileList( void *list ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_FS_FreeFileList called (#%d) list=%p\n", gi_call_count, (void*)list);
	if ( !list ) return;

	int *sof2list = (int *)list;
	int count = sof2list[0];  // read count from header

	// Free individual filename strings
	for ( int i = 0; i < count; i++ ) {
		char *filename = (char *)sof2list[2 + i*3];
		if ( filename ) {
			Z_Free( filename );
		}
	}

	// Free the SOF2-format array itself
	Z_Free( sof2list );
}

// gi[16]: Cmd_TokenizeString traced
static void SV_Traced_Cmd_TokenizeString( const char *text ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_Cmd_TokenizeString called (#%d)\n", gi_call_count);
	Cmd_TokenizeString( text );
}

// gi[24]: Cvar_Register traced
static void SV_Traced_Cvar_Register( vmCvar_t *vmCvar, const char *name, const char *defVal, int flags ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_Cvar_Register called (#%d) name='%s'\n", gi_call_count, name ? name : "(null)");
	Cvar_Register( vmCvar, name, defVal, flags );
}

// gi[25]: Cvar_Update traced
static void SV_Traced_Cvar_Update( vmCvar_t *vmCvar ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_Cvar_Update called (#%d)\n", gi_call_count);
	Cvar_Update( vmCvar );
}

// gi[29]: Cvar_VariableIntegerValue traced
static int SV_Traced_Cvar_VariableIntegerValue( const char *name ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_Cvar_VariableIntegerValue called (#%d) name='%s'\n", gi_call_count, name ? name : "(null)");
	return Cvar_VariableIntegerValue( name );
}

// gi[31]: Cvar_VariableStringBuffer traced
static void SV_Traced_Cvar_VariableStringBuffer( const char *name, char *buffer, int bufSize ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_Cvar_VariableStringBuffer called (#%d) name='%s'\n", gi_call_count, name ? name : "(null)");
	Cvar_VariableStringBuffer( name, buffer, bufSize );
}

// gi[34]: Z_CheckHeap traced
static void SV_Traced_Z_CheckHeap( void ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_Z_CheckHeap called (#%d)\n", gi_call_count);
}

// gi[85]: GetEntityToken traced
static int SV_Traced_GetEntityToken( char *buf, int bufsize ) {
	gi_call_count++;
	// Show first 80 chars of entityParsePoint content for debugging
	char entPreview[81] = {0};
	if (sv.entityParsePoint) {
		strncpy(entPreview, sv.entityParsePoint, 80);
		entPreview[80] = '\0';
		// Replace newlines with spaces for log readability
		for (int i = 0; entPreview[i]; i++)
			if (entPreview[i] == '\n' || entPreview[i] == '\r') entPreview[i] = ' ';
	}
	fprintf(stderr, "[GI] gi_GetEntityToken called (#%d) entityParsePoint=%p mLocalSubBSPIndex=%d content='%s'\n",
		gi_call_count, (void*)sv.entityParsePoint, sv.mLocalSubBSPIndex, entPreview);
	fflush(stderr);
	int result = 0;
	__try {
		result = (int)SV_GetEntityToken( buf, bufsize );
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		fprintf(stderr, "[GI] gi_GetEntityToken EXCEPTION caught! code=0x%08lX\n", GetExceptionCode());
		fflush(stderr);
		return 0;
	}
	// Show first 100 chars of result
	char preview[101];
	if (buf) { strncpy(preview, buf, 100); preview[100] = '\0'; }
	else { preview[0] = '\0'; }
	fprintf(stderr, "[GI] gi_GetEntityToken result=%d token='%s'\n", result, preview);
	fflush(stderr);
	return result;
}

// gi[86]: RE_RegisterModel traced — use real renderer
static qhandle_t SV_Traced_RE_RegisterModel( const char *name ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_RE_RegisterModel called (#%d) name='%s'\n", gi_call_count, name ? name : "(null)");
	return re.RegisterModel( name );
}

// gi[87]: RE_RegisterShader traced — use real renderer
static qhandle_t SV_Traced_RE_RegisterShader( const char *name ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_RE_RegisterShader called (#%d) name='%s'\n", gi_call_count, name ? name : "(null)");
	return re.RegisterShader( name );
}

// gi[89]: ICARUS_Init traced
static void SV_Traced_ICARUS_Init( void ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_ICARUS_Init called (#%d)\n", gi_call_count);
}

// gi[95]: SE_GetString traced
static const char *SV_Traced_SE_GetString( const char *token ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_SE_GetString called (#%d) token='%s'\n", gi_call_count, token ? token : "(null)");
	return SE_GetString( token );
}

static void QDECL SV_Traced_Error( int level, const char *fmt, ... ) {
	gi_call_count++;
	va_list args;
	va_start( args, fmt );
	char buf[4096];
	Q_vsnprintf( buf, sizeof(buf), fmt, args );
	va_end( args );
	fprintf(stderr, "[GI] gi_Com_Error called (#%d) level=%d msg='%s'\n", gi_call_count, level, buf);
	fflush(stderr);

	// SOF2 game DLL generates entity events (NPC vocalizations, etc.) that
	// don't exist in the OpenJK event table.  Swallow these to prevent a
	// fatal server crash; the cgame sanitizer handles the client side.
	if ( Q_strncmp( buf, "Unknown event:", 14 ) == 0 ) {
		static int s_unknownEventCount = 0;
		if ( s_unknownEventCount < 32 ) {
			Com_Printf( "^3[SV guard] swallowed game Unknown event: %s\n", buf );
			s_unknownEventCount++;
		}
		return;
	}

	Com_Error( level, "%s", buf );
}

// Traced Cbuf_AddText — logs console commands from game DLL
static void SV_Traced_Cbuf_AddText( const char *text ) {
	gi_call_count++;
	// Log game DLL console commands (throttled to avoid spam)
	static int cbufLogCount = 0;
	if ( cbufLogCount < 20 ) {
		// Safely print first 80 chars, checking for non-ASCII
		char safe[82];
		int i;
		for (i = 0; i < 80 && text[i]; i++) {
			safe[i] = (text[i] >= 32 && text[i] < 127) ? text[i] : '?';
		}
		safe[i] = '\0';
		fprintf(stderr, "[GI] gi_Cbuf_AddText (#%d) cmd='%s'\n", gi_call_count, safe);
		fflush(stderr);
		cbufLogCount++;
	}
	// Block corrupted commands (text with non-ASCII bytes in first 4 chars)
	if ( text && text[0] ) {
		unsigned char c0 = (unsigned char)text[0];
		if ( c0 > 127 ) {
			// Corrupted command — skip it
			static int corruptCount = 0;
			if ( corruptCount < 5 ) {
				fprintf(stderr, "[GI] BLOCKED corrupted Cbuf_AddText byte0=0x%02X\n", c0);
				fflush(stderr);
				corruptCount++;
			}
			return;
		}
	}
	Cbuf_AddText( text );
}

// Traced Cbuf_ExecuteText — logs console command executions from game DLL
static qboolean SV_IsValidMapTokenChar( char c ) {
	return ((c >= 'a' && c <= 'z') ||
		(c >= 'A' && c <= 'Z') ||
		(c >= '0' && c <= '9') ||
		c == '_' || c == '/' || c == '-') ? qtrue : qfalse;
}

static qboolean SV_ShouldBlockScriptedMapCommand( void *caller, const char *text ) {
	if ( caller != (void *)0x2005EA75 || !text ) {
		return qfalse;
	}
	return ( Q_stricmpn( text, "map ", 4 ) == 0 ) ? qtrue : qfalse;
}

static void SV_Traced_Cbuf_ExecuteText( int exec_when, const char *text ) {
	void *caller = _ReturnAddress();
	gi_call_count++;
	static int cbufExecLogCount = 0;
	if ( cbufExecLogCount < 20 ) {
		char safe[82];
		int i;
		for (i = 0; i < 80 && text && text[i]; i++) {
			safe[i] = (text[i] >= 32 && text[i] < 127) ? text[i] : '?';
		}
		safe[i] = '\0';
		fprintf(stderr, "[GI] gi_Cbuf_ExecuteText (#%d) caller=%p when=%d cmd='%s'\n",
			gi_call_count, caller, exec_when, safe);
		fflush(stderr);
		cbufExecLogCount++;
	}
	// Block corrupted commands — check ALL bytes for non-ASCII (0xCC = MSVC debug fill)
	if ( text ) {
		for ( int j = 0; text[j]; j++ ) {
			unsigned char c = (unsigned char)text[j];
			if ( c > 127 ) {
				static int corruptCount2 = 0;
				if ( corruptCount2 < 5 ) {
					fprintf(stderr,
						"[GI] BLOCKED corrupted Cbuf_ExecuteText caller=%p at byte[%d]=0x%02X cmd='%.40s'\n",
						caller, j, c, text);
					fflush(stderr);
					corruptCount2++;
				}
				return;
			}
		}
		if ( SV_ShouldBlockScriptedMapCommand( caller, text ) ) {
			static int badMapCount = 0;
			if ( badMapCount < 10 ) {
				fprintf( stderr,
					"[GI] BLOCKED invalid scripted map command caller=%p cmd='%.80s'\n",
					caller, text );
				fflush( stderr );
				badMapCount++;
			}
			return;
		}
	}
	Cbuf_ExecuteText( exec_when, text );
}

static int SV_FindEntityNumByPointer( gentity_t *gEnt );
static const char *SV_SOF2_ModelConfigString( int modelIndex );
static int sv_trace_log_armed = 0;
static int sv_trace_log_count = 0;
static int sv_trace_log_reason = 0; // 1=movement, 2=use
static qboolean sv_trace_use_latched = qfalse;

static qboolean SV_ShouldLogTouchDebug( void ) {
	if ( sv_trace_use_latched ) {
		return qtrue;
	}
	if ( sv_trace_log_armed && ( sv_trace_log_reason == 1 || sv_trace_log_reason == 2 ) ) {
		return qtrue;
	}
	return qfalse;
}

// --- SV_AreaEntities wrapper: SOF2 game DLL expects entity INDICES, not pointers ---
// OpenJK's SV_AreaEntities fills the output array with gentity_t* pointers, but
// SOF2's gamex86.dll treats the output as int[] entity indices (used as array index
// into g_entityLookupTable). This wrapper converts pointers to indices.
static int SV_SOF2_AreaEntities( const vec3_t mins, const vec3_t maxs, int *entityList, int maxcount, int /*worldIndex*/ ) {
	// Temp array for pointers — cap at MAX_GENTITIES to avoid stack overflow
	gentity_t *ptrBuf[MAX_GENTITIES];
	int outCount = 0;
	static int s_areaEntitiesCallLogCount = 0;
	if ( maxcount > MAX_GENTITIES ) maxcount = MAX_GENTITIES;
	int count = SV_AreaEntities( mins, maxs, ptrBuf, maxcount );

	// Convert each gentity_t* pointer to the live slot index expected by retail.
	// s.number can carry stale or non-slot values; G_TouchTriggers indexes the
	// returned list directly into g_entityLookupTable with no validation.
	for ( int i = 0; i < count; i++ ) {
		if ( ptrBuf[i] ) {
			const int entNum = SV_FindEntityNumByPointer( ptrBuf[i] );
			if ( entNum >= 0 && entNum < MAX_GENTITIES ) {
				entityList[outCount++] = entNum;
				SOF2_ENT_NUMBER( ptrBuf[i] ) = entNum;
				SOF2_ENT_SERVERINDEX( ptrBuf[i] ) = entNum;
			}
		}
	}

	if ( s_areaEntitiesCallLogCount < 64 ) {
		int triggerCount = 0;
		int itemCount = 0;
		for ( int i = 0; i < outCount; ++i ) {
			gentity_t *ent = SV_GentityNum( entityList[i] );
			if ( !ent ) {
				continue;
			}
			if ( SOF2_ENT_CONTENTS( ent ) & CONTENTS_TRIGGER ) {
				++triggerCount;
			}
			if ( SOF2_ENT_ETYPE( ent ) == ET_ITEM ) {
				++itemCount;
			}
		}

		Com_Printf(
			"[AREA69] #%d mins=(%.1f,%.1f,%.1f) maxs=(%.1f,%.1f,%.1f) raw=%d out=%d triggers=%d items=%d reason=%d latched=%d\n",
			s_areaEntitiesCallLogCount + 1,
			mins[0], mins[1], mins[2],
			maxs[0], maxs[1], maxs[2],
			count,
			outCount,
			triggerCount,
			itemCount,
			sv_trace_log_reason,
			(int)sv_trace_use_latched );
		++s_areaEntitiesCallLogCount;
	}

	if ( SV_ShouldLogTouchDebug() ) {
		static int s_useAreaSummaryLogCount = 0;
		static int s_useAreaDetailLogCount = 0;
		int triggerCount = 0;
		int itemCount = 0;

		for ( int i = 0; i < outCount; ++i ) {
			gentity_t *ent = SV_GentityNum( entityList[i] );
			if ( !ent ) {
				continue;
			}
			if ( SOF2_ENT_CONTENTS( ent ) & CONTENTS_TRIGGER ) {
				++triggerCount;
			}
			if ( SOF2_ENT_ETYPE( ent ) == ET_ITEM ) {
				++itemCount;
			}
		}

		if ( s_useAreaSummaryLogCount < 24 ) {
			Com_Printf(
				"[TOUCH %s] EntitiesInBox mins=(%.1f,%.1f,%.1f) maxs=(%.1f,%.1f,%.1f) count=%d triggers=%d items=%d\n",
				( sv_trace_log_reason == 2 ) ? "use" : "move",
				mins[0], mins[1], mins[2],
				maxs[0], maxs[1], maxs[2],
				outCount,
				triggerCount,
				itemCount );
			++s_useAreaSummaryLogCount;
		}

		for ( int i = 0; i < outCount && s_useAreaDetailLogCount < 96; ++i ) {
			gentity_t *ent = SV_GentityNum( entityList[i] );
			if ( !ent ) {
				continue;
			}
			if ( !( SOF2_ENT_CONTENTS( ent ) & CONTENTS_TRIGGER ) &&
				SOF2_ENT_ETYPE( ent ) != ET_ITEM ) {
				continue;
			}
			Com_Printf(
				"[TOUCH %s] hitlist num=%d type=%d model=%d('%s') solid=0x%x contents=0x%x linked=%d bmodel=%d svf=0x%x\n",
				( sv_trace_log_reason == 2 ) ? "use" : "move",
				entityList[i],
				SOF2_ENT_ETYPE( ent ),
				SOF2_ENT_MODELINDEX( ent ),
				SV_SOF2_ModelConfigString( SOF2_ENT_MODELINDEX( ent ) ),
				SOF2_ENT_SOLID( ent ),
				SOF2_ENT_CONTENTS( ent ),
				SOF2_ENT_LINKED( ent ),
				(int)SV_SOF2_IsBrushModelEntity( ent ),
				SOF2_ENT_SVFLAGS( ent ) );
			++s_useAreaDetailLogCount;
		}
	}

	return outCount;
}

// --- Signature-adapting wrappers for game_import_t slot type mismatches ---

// slot 23: Com_sprintf — engine returns int, SOF2 slot is void
static void Com_sprintf_void( char *dest, int size, const char *fmt, ... ) {
	va_list args;
	va_start( args, fmt );
	Q_vsnprintf( dest, size, fmt, args );
	va_end( args );
}

// slot 32: Malloc — G_ZMalloc_Helper takes memtag_t (enum), slot takes int
static void *G_ZMalloc_Wrapper( int size, int tag, qboolean zeroIt ) {
	return Z_Malloc( size, (memtag_t)tag, zeroIt );
}

// slots 35, 96: GetEntityToken — SV_GetEntityToken returns qboolean, slot expects int
qboolean SV_GetEntityToken( char *buffer, int bufferSize );  // forward decl (defined below)
static int SV_GetEntityToken_int( char *buf, int bufsize ) {
	return (int)SV_GetEntityToken( buf, bufsize );
}

extern int	s_entityWavVol[MAX_GENTITIES];

// these functions must be used instead of pointer arithmetic, because
// the game allocates gentities with private information after the server shared part
/*
int	SV_NumForGentity( gentity_t *ent ) {
	int		num;

	num = ( (byte *)ent - (byte *)ge->gentities ) / ge->gentitySize;

	return num;
}
*/
// SOF2: the game DLL passes its CEntitySystem pointer table as the entity array.
// Unlike Q3A/JKA (contiguous struct array with stride), SOF2 uses a POINTER TABLE:
//   sv_GEntities[entityNum] is a 4-byte pointer to a CEntity* (or NULL for empty slots).
// The engine accesses entities via pointer dereference, NOT stride-based arithmetic.
static void *sv_GEntities    = NULL;   // CEntitySystem serverSlots[1024] pointer table
static int   sv_GEntitySize  = 0;      // 0x560 = sizeof(CEntity), stored but NOT used for array access
static int   sv_numGEntities = 0;      // MAX_GENTITIES (1024)
static void *sv_GameClients  = NULL;   // game client array (stride-based, unlike entities)
static int   sv_GameClientSize = 0;    // sizeof(gclient_t) = 0x24C
static byte  sv_sof2BrushModel[MAX_GENTITIES];
static int SV_SOF2_EntitySlot( const gentity_t *gEnt ) {
	int entNum;

	if ( !gEnt ) {
		return -1;
	}

	entNum = SOF2_ENT_NUMBER( gEnt );
	if ( entNum >= 0 && entNum < MAX_GENTITIES ) {
		return entNum;
	}

	entNum = SOF2_ENT_SERVERINDEX( gEnt );
	if ( entNum >= 0 && entNum < MAX_GENTITIES ) {
		return entNum;
	}

	return SV_FindEntityNumByPointer( (gentity_t *)gEnt );
}

static const char *SV_SOF2_ModelConfigString( int modelIndex ) {
	const int csIndex = 32 + modelIndex;

	if ( modelIndex <= 0 ) {
		return "";
	}
	if ( csIndex < 0 || csIndex >= MAX_CONFIGSTRINGS ) {
		return "";
	}
	if ( !sv.configstrings[csIndex] ) {
		return "";
	}

	return sv.configstrings[csIndex];
}

qboolean SV_SOF2_IsBrushModelEntity( const gentity_t *gEnt ) {
	const int entNum = SV_SOF2_EntitySlot( gEnt );
	const int modelIndex = gEnt ? SOF2_ENT_MODELINDEX( gEnt ) : 0;
	const char *modelName = SV_SOF2_ModelConfigString( modelIndex );

	if ( entNum >= 0 && entNum < MAX_GENTITIES ) {
		if ( modelName[0] == '*' ) {
			sv_sof2BrushModel[entNum] = 1;
			return qtrue;
		}
		if ( !Q_stricmpn( modelName, "models/", 7 ) ) {
			sv_sof2BrushModel[entNum] = 0;
			return qfalse;
		}
		return sv_sof2BrushModel[entNum] ? qtrue : qfalse;
	}

	return SOF2_ENT_BMODEL( gEnt ) ? qtrue : qfalse;
}

static int SV_FindEntityNumByPointer( gentity_t *gEnt ) {
	int limit;

	if ( !gEnt || !sv_GEntities ) {
		return -1;
	}

	limit = sv_numGEntities;
	if ( limit <= 0 || limit > MAX_GENTITIES ) {
		limit = MAX_GENTITIES;
	}

	for ( int i = 0; i < limit; ++i ) {
		if ( ((gentity_t **)sv_GEntities)[i] == gEnt ) {
			return i;
		}
	}

	return -1;
}

gentity_t	*SV_GentityNum( int num ) {
	gentity_t *ent;
	if ( num < 0 || num >= MAX_GENTITIES || !sv_GEntities ) {
		return NULL;
	}
	// SOF2: pointer table access — each slot is a 4-byte CEntity* pointer
	ent = ((gentity_t **)sv_GEntities)[num];
	if ( ent ) {
		SOF2_ENT_NUMBER( ent ) = num;
		SOF2_ENT_SERVERINDEX( ent ) = num;
	}
	return ent;
}

svEntity_t	*SV_SvEntityForGentity( gentity_t *gEnt ) {
	int entNum;
	int serverIndex;
	if ( !gEnt ) {
		return NULL;
	}

	entNum = SOF2_ENT_NUMBER( gEnt );
	if ( entNum < 0 || entNum >= MAX_GENTITIES ) {
		serverIndex = SOF2_ENT_SERVERINDEX( gEnt );
		if ( serverIndex >= 0 && serverIndex < MAX_GENTITIES ) {
			entNum = serverIndex;
		} else {
			entNum = SV_FindEntityNumByPointer( gEnt );
		}
	}

	if ( entNum < 0 || entNum >= MAX_GENTITIES ) {
		static int badEntWarnCount = 0;
		if ( badEntWarnCount < 12 ) {
			Com_Printf(
				"^3SV_SvEntityForGentity: unresolved gEnt=%p s.number=%d serverIndex=%d (MAX=%d), returning NULL\n",
				(void *)gEnt,
				SOF2_ENT_NUMBER( gEnt ),
				SOF2_ENT_SERVERINDEX( gEnt ),
				MAX_GENTITIES );
			badEntWarnCount++;
		}
		return NULL;
	}

	SOF2_ENT_NUMBER( gEnt ) = entNum;
	SOF2_ENT_SERVERINDEX( gEnt ) = entNum;
	return &sv.svEntities[ entNum ];
}

gentity_t	*SV_GEntityForSvEntity( svEntity_t *svEnt ) {
	int		num;

	num = svEnt - sv.svEntities;
	return SV_GentityNum( num );
}

// SOF2: access client/playerState data from the separate game client array
// (passed via LocateGameData arg4). gclient_t starts with playerState_t.
playerState_t *SV_GameClientNum( int clientNum ) {
	if ( !sv_GameClients || clientNum < 0 ) {
		return NULL;
	}
	return (playerState_t *)((byte *)sv_GameClients + sv_GameClientSize * clientNum);
}

/*
===============
SV_GameSendServerCommand

Sends a command string to a client
===============
*/
void SV_GameSendServerCommand( int clientNum, const char *fmt, ... ) {
	char		msg[8192];
	va_list		argptr;

	va_start (argptr,fmt);
	Q_vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);

	if ( clientNum == -1 ) {
		SV_SendServerCommand( NULL, "%s", msg );
	} else {
		if ( clientNum < 0 || clientNum >= 1 ) {
			return;
		}
		SV_SendServerCommand( svs.clients + clientNum, "%s", msg );
	}
}


/*
===============
SV_GameDropClient

Disconnects the client with a message
===============
*/
void SV_GameDropClient( int clientNum, const char *reason ) {
	if ( clientNum < 0 || clientNum >= 1 ) {
		return;
	}
	SV_DropClient( svs.clients + clientNum, reason );
}


/*
=================
SV_SetBrushModel

sets mins and maxs for inline bmodels
=================
*/
void SV_SetBrushModel( gentity_t *ent, const char *name ) {
	clipHandle_t	h;
	vec3_t			mins, maxs;

	if (!name)
	{
		Com_Error( ERR_DROP, "SV_SetBrushModel: NULL model for ent number %d", SOF2_ENT_NUMBER(ent) );
	}

	if (name[0] == '*')
	{
		SOF2_ENT_MODELINDEX(ent) = atoi( name + 1 );

		if (sv.mLocalSubBSPIndex != -1)
		{
			SOF2_ENT_MODELINDEX(ent) += sv.mLocalSubBSPModelOffset;
		}

		h = CM_InlineModel( SOF2_ENT_MODELINDEX(ent) );

		if (sv.mLocalSubBSPIndex != -1)
		{
			CM_ModelBounds( SubBSP[sv.mLocalSubBSPIndex], h, mins, maxs );
		}
		else
		{
			CM_ModelBounds( cmg, h, mins, maxs);
		}

		//CM_ModelBounds( h, mins, maxs );

		VectorCopy (mins, SOF2_ENT_MINS(ent));
		VectorCopy (maxs, SOF2_ENT_MAXS(ent));
		SOF2_ENT_BMODEL(ent) = 1;
		{
			const int entNum = SV_SOF2_EntitySlot( ent );
			if ( entNum >= 0 && entNum < MAX_GENTITIES ) {
				sv_sof2BrushModel[entNum] = 1;
			}
		}

		SOF2_ENT_CONTENTS(ent) = CM_ModelContents( h, -1 );
	}
	else if (name[0] == '#')
	{
		SOF2_ENT_MODELINDEX(ent) = CM_LoadSubBSP(va("maps/%s.bsp", name + 1), qfalse);
		CM_ModelBounds( SubBSP[CM_FindSubBSP(SOF2_ENT_MODELINDEX(ent))], SOF2_ENT_MODELINDEX(ent), mins, maxs );

		VectorCopy (mins, SOF2_ENT_MINS(ent));
		VectorCopy (maxs, SOF2_ENT_MAXS(ent));
		SOF2_ENT_BMODEL(ent) = 1;
		{
			const int entNum = SV_SOF2_EntitySlot( ent );
			if ( entNum >= 0 && entNum < MAX_GENTITIES ) {
				sv_sof2BrushModel[entNum] = 1;
			}
		}

		//rwwNOTE: We don't ever want to set contents -1, it includes CONTENTS_LIGHTSABER.
		//Lots of stuff will explode if there's a brush with CONTENTS_LIGHTSABER that isn't attached to a client owner.
		//SOF2_ENT_CONTENTS(ent) = -1;		// we don't know exactly what is in the brushes
		h = CM_InlineModel( SOF2_ENT_MODELINDEX(ent) );
		SOF2_ENT_CONTENTS(ent) = CM_ModelContents( h, CM_FindSubBSP(SOF2_ENT_MODELINDEX(ent)) );
	//	SOF2_ENT_CONTENTS(ent) = CONTENTS_SOLID;
	}
	else
	{
		Com_Error( ERR_DROP, "SV_SetBrushModel: %s isn't a brush model (ent %d)", name, SOF2_ENT_NUMBER(ent) );
	}
}

const char *SV_SetActiveSubBSP(int index)
{
	if (index >= 0)
	{
		sv.mLocalSubBSPIndex = CM_FindSubBSP(index);
		sv.mLocalSubBSPModelOffset = index;
		sv.mLocalSubBSPEntityParsePoint = CM_SubBSPEntityString (sv.mLocalSubBSPIndex);
		return sv.mLocalSubBSPEntityParsePoint;
	}
	else
	{
		sv.mLocalSubBSPIndex = -1;
	}

	return NULL;
}

/*
=================
SV_inPVS

Also checks portalareas so that doors block sight
=================
*/
qboolean SV_inPVS (const vec3_t p1, const vec3_t p2)
{
	int		leafnum;
	int		cluster;
	int		area1, area2;
	byte	*mask;
	int		start=0;

	if ( com_speeds->integer ) {
		start = Sys_Milliseconds ();
	}
	leafnum = CM_PointLeafnum (p1);
	cluster = CM_LeafCluster (leafnum);
	area1 = CM_LeafArea (leafnum);
	mask = CM_ClusterPVS (cluster);

	leafnum = CM_PointLeafnum (p2);
	cluster = CM_LeafCluster (leafnum);
	area2 = CM_LeafArea (leafnum);
	if ( mask && (!(mask[cluster>>3] & (1<<(cluster&7)) ) ) )
	{
		if ( com_speeds->integer ) {
			timeInPVSCheck += Sys_Milliseconds () - start;
		}
		return qfalse;
	}

	if (!CM_AreasConnected (area1, area2))
	{
		timeInPVSCheck += Sys_Milliseconds() - start;
		return qfalse;		// a door blocks sight
	}

	if ( com_speeds->integer ) {
		timeInPVSCheck += Sys_Milliseconds() - start;
	}
	return qtrue;
}


/*
=================
SV_inPVSIgnorePortals

Does NOT check portalareas
=================
*/
qboolean SV_inPVSIgnorePortals( const vec3_t p1, const vec3_t p2)
{
	int		leafnum;
	int		cluster;
	byte	*mask;
	int		start=0;

	if ( com_speeds->integer ) {
		start = Sys_Milliseconds ();
	}

	leafnum = CM_PointLeafnum (p1);
	cluster = CM_LeafCluster (leafnum);
	mask = CM_ClusterPVS (cluster);

	leafnum = CM_PointLeafnum (p2);
	cluster = CM_LeafCluster (leafnum);

	if ( mask && (!(mask[cluster>>3] & (1<<(cluster&7)) ) ) )
	{
		if ( com_speeds->integer ) {
			timeInPVSCheck += Sys_Milliseconds() - start;
		}
		return qfalse;
	}

	if ( com_speeds->integer ) {
		timeInPVSCheck += Sys_Milliseconds() - start;
	}
	return qtrue;
}


/*
========================
SV_AdjustAreaPortalState
========================
*/
void SV_AdjustAreaPortalState( gentity_t *ent, qboolean open ) {
#ifndef JK2_MODE
	if ( !(SOF2_ENT_CONTENTS(ent) & CONTENTS_OPAQUE) ) {
#ifndef FINAL_BUILD
//		Com_Printf( "INFO: entity number %d not opaque: not affecting area portal!\n", SOF2_ENT_NUMBER(ent) );
#endif
		return;
	}
#endif

	svEntity_t	*svEnt;

	svEnt = SV_SvEntityForGentity( ent );
	if ( !svEnt || svEnt->areanum2 == -1 ) {
		return;
	}
	CM_AdjustAreaPortalState( svEnt->areanum, svEnt->areanum2, open );
}


/*
==================
SV_GameAreaEntities
==================
*/
qboolean	SV_EntityContact( const vec3_t mins, const vec3_t maxs, const gentity_t *gEnt, int /*worldIndex*/ ) {
	const float	*origin, *angles;
	clipHandle_t	ch;
	trace_t			trace;
	static int s_entityContactCallLogCount = 0;

	// check for exact collision
	if ( !SV_SOF2_IsBrushModelEntity( gEnt ) ) {
		float *curOrigin = SOF2_ENT_CURORIGIN( gEnt );
		float *curAngles = SOF2_ENT_CURANGLES( gEnt );
		const float *stateOrigin = SOF2_ENT_S_ORIGIN( gEnt );
		const float *stateAngles = SOF2_ENT_S_ANGLES( gEnt );
		if ( curOrigin[0] == 0.0f && curOrigin[1] == 0.0f && curOrigin[2] == 0.0f &&
			 ( stateOrigin[0] != 0.0f || stateOrigin[1] != 0.0f || stateOrigin[2] != 0.0f ) ) {
			VectorCopy( stateOrigin, curOrigin );
		}
		if ( curAngles[0] == 0.0f && curAngles[1] == 0.0f && curAngles[2] == 0.0f &&
			 ( stateAngles[0] != 0.0f || stateAngles[1] != 0.0f || stateAngles[2] != 0.0f ) ) {
			VectorCopy( stateAngles, curAngles );
		}
	}
	origin = SOF2_ENT_CURORIGIN(gEnt);
	angles = SOF2_ENT_CURANGLES(gEnt);

	ch = SV_ClipHandleForEntity( gEnt );
	CM_TransformedBoxTrace ( &trace, vec3_origin, vec3_origin, mins, maxs,
		ch, -1, origin, angles );

	if ( s_entityContactCallLogCount < 96 ) {
		Com_Printf(
			"[CONTACT70] #%d num=%d type=%d model=%d('%s') solid=0x%x contents=0x%x linked=%d bmodel=%d startsolid=%d reason=%d latched=%d origin=(%.1f,%.1f,%.1f)\n",
			s_entityContactCallLogCount + 1,
			SV_SOF2_EntitySlot( gEnt ),
			SOF2_ENT_ETYPE( gEnt ),
			SOF2_ENT_MODELINDEX( gEnt ),
			SV_SOF2_ModelConfigString( SOF2_ENT_MODELINDEX( gEnt ) ),
			SOF2_ENT_SOLID( gEnt ),
			SOF2_ENT_CONTENTS( gEnt ),
			SOF2_ENT_LINKED( gEnt ),
			(int)SV_SOF2_IsBrushModelEntity( gEnt ),
			(int)trace.startsolid,
			sv_trace_log_reason,
			(int)sv_trace_use_latched,
			origin[0], origin[1], origin[2] );
		++s_entityContactCallLogCount;
	}

	if ( SV_ShouldLogTouchDebug() ) {
		static int s_useContactLogCount = 0;
		if ( s_useContactLogCount < 96 ) {
			Com_Printf(
				"[TOUCH %s] EntityContact num=%d type=%d model=%d('%s') solid=0x%x contents=0x%x linked=%d bmodel=%d startsolid=%d origin=(%.1f,%.1f,%.1f)\n",
				( sv_trace_log_reason == 2 ) ? "use" : "move",
				SV_SOF2_EntitySlot( gEnt ),
				SOF2_ENT_ETYPE( gEnt ),
				SOF2_ENT_MODELINDEX( gEnt ),
				SV_SOF2_ModelConfigString( SOF2_ENT_MODELINDEX( gEnt ) ),
				SOF2_ENT_SOLID( gEnt ),
				SOF2_ENT_CONTENTS( gEnt ),
				SOF2_ENT_LINKED( gEnt ),
				(int)SV_SOF2_IsBrushModelEntity( gEnt ),
				(int)trace.startsolid,
				origin[0], origin[1], origin[2] );
			++s_useContactLogCount;
		}
	}

	return (qboolean)trace.startsolid;
}


/*
===============
SV_GetServerinfo

===============
*/
void SV_GetServerinfo( char *buffer, int bufferSize ) {
	if ( bufferSize < 1 ) {
		Com_Error( ERR_DROP, "SV_GetServerinfo: bufferSize == %i", bufferSize );
	}
	Q_strncpyz( buffer, Cvar_InfoString( CVAR_SERVERINFO ), bufferSize );
}

qboolean SV_GetEntityToken( char *buffer, int bufferSize )
{
	char	*s;

	fprintf(stderr, "[DBG] SV_GetEntityToken: buffer=%p bufferSize=%d mLocalSubBSPIndex=%d entityParsePoint=%p\n",
		(void*)buffer, bufferSize, sv.mLocalSubBSPIndex, (void*)sv.entityParsePoint);
	fflush(stderr);

	if (sv.mLocalSubBSPIndex == -1)
	{
		fprintf(stderr, "[DBG] SV_GetEntityToken: calling COM_Parse(&entityParsePoint=%p)...\n", (void*)sv.entityParsePoint);
		fflush(stderr);
		s = COM_Parse( (const char **)&sv.entityParsePoint );
		fprintf(stderr, "[DBG] SV_GetEntityToken: COM_Parse returned s=%p '%s', entityParsePoint now=%p\n",
			(void*)s, s ? s : "(null)", (void*)sv.entityParsePoint);
		fflush(stderr);
		Q_strncpyz( buffer, s, bufferSize );
		if ( !sv.entityParsePoint && !s[0] )
		{
			return qfalse;
		}
		else
		{
			return qtrue;
		}
	}
	else
	{
		s = COM_Parse( (const char **)&sv.mLocalSubBSPEntityParsePoint);
		Q_strncpyz( buffer, s, bufferSize );
		if ( !sv.mLocalSubBSPEntityParsePoint && !s[0] )
		{
			return qfalse;
		}
		else
		{
			return qtrue;
		}
	}
}

//==============================================

/*
===============
SV_ShutdownGameProgs

Called when either the entire server is being killed, or
it is changing to a different game directory.
===============
*/
void SV_ShutdownGameProgs (qboolean shutdownCin) {
	if (!ge) {
		return;
	}
#ifdef _WIN32
	__try {
		ge->Shutdown(0);
	}
	__except ( EXCEPTION_EXECUTE_HANDLER ) {
		Com_Printf( "^3[SV] suppressed exception during ge->Shutdown(0)\n" );
	}
#else
	ge->Shutdown(0);
#endif

	SCR_StopCinematic();
	CL_ShutdownCGame();	//we have cgame burried in here.

	Sys_UnloadDll( gameLibrary );

	ge = NULL;
}

// this is a compile-helper function since Z_Malloc can now become a macro with __LINE__ etc
//
static void *G_ZMalloc_Helper( int iSize, memtag_t eTag, qboolean bZeroit)
{
	return Z_Malloc( iSize, eTag, bZeroit );
}

static int SV_G2API_AddBolt( CGhoul2Info *ghlInfo, const char *boneName )
{
	return re.G2API_AddBolt( ghlInfo, boneName );
}

static int SV_G2API_AddBoltSurfNum( CGhoul2Info *ghlInfo, const int surfIndex )
{
	return re.G2API_AddBoltSurfNum( ghlInfo, surfIndex );
}

static int SV_G2API_AddSurface( CGhoul2Info *ghlInfo, int surfaceNumber, int polyNumber, float BarycentricI, float BarycentricJ, int lod )
{
	return re.G2API_AddSurface( ghlInfo, surfaceNumber, polyNumber, BarycentricI, BarycentricJ, lod );
}

static void SV_G2API_AnimateG2Models( CGhoul2Info_v &ghoul2, int AcurrentTime, CRagDollUpdateParams *params )
{
	re.G2API_AnimateG2Models( ghoul2, AcurrentTime, params );
}

static qboolean SV_G2API_AttachEnt( int *boltInfo, CGhoul2Info *ghlInfoTo, int toBoltIndex, int entNum, int toModelNum )
{
	return re.G2API_AttachEnt( boltInfo, ghlInfoTo, toBoltIndex, entNum, toModelNum );
}

static qboolean SV_G2API_AttachG2Model( CGhoul2Info *ghlInfo, CGhoul2Info *ghlInfoTo, int toBoltIndex, int toModel )
{
	return re.G2API_AttachG2Model( ghlInfo, ghlInfoTo, toBoltIndex, toModel );
}

static void SV_G2API_CleanGhoul2Models( CGhoul2Info_v &ghoul2 )
{
	return re.G2API_CleanGhoul2Models( ghoul2 );
}

// SOF2 game import gi[90]: CleanGhoul2Models — called from gamex86.dll with
// a raw void* (entity+0x8).  The DLL's CEntity stores a CGhoul2Info_v-style
// 4-byte handle at that address.  Validate the handle before passing to the
// renderer's G2API_CleanGhoul2Models to avoid assertion failures on stale or
// invalid handles (e.g. during entity destruction after renderer re-init).
static void SV_G2API_CleanGhoul2Models_Safe( void *ghoul2ptr )
{
	if ( !ghoul2ptr ) return;
	int handle = *(int *)ghoul2ptr;
	if ( handle <= 0 ) return;
	// Validate handle is still valid in the Ghoul2 info array
	IGhoul2InfoArray &arr = re.TheGhoul2InfoArray();
	if ( !arr.IsValid( handle ) ) {
		Com_DPrintf( "[G2] CleanGhoul2Models_Safe: invalid handle %d, clearing\n", handle );
		*(int *)ghoul2ptr = 0;
		return;
	}
	re.G2API_CleanGhoul2Models( *(CGhoul2Info_v *)ghoul2ptr );
}

static void SV_G2API_CollisionDetect(
	CCollisionRecord *collRecMap, CGhoul2Info_v &ghoul2, const vec3_t angles, const vec3_t position,
	int AframeNumber, int entNum, vec3_t rayStart, vec3_t rayEnd, vec3_t scale, CMiniHeap *miniHeap,
	EG2_Collision eG2TraceType, int useLod, float fRadius )
{
	re.G2API_CollisionDetect( collRecMap, ghoul2, angles, position, AframeNumber,
		entNum, rayStart, rayEnd, scale, miniHeap, eG2TraceType, useLod, fRadius );
}

static void SV_G2API_CopyGhoul2Instance( CGhoul2Info_v &ghoul2From, CGhoul2Info_v &ghoul2To, int modelIndex )
{
	re.G2API_CopyGhoul2Instance( ghoul2From, ghoul2To, modelIndex );
}

static void SV_G2API_DetachEnt( int *boltInfo )
{
	re.G2API_DetachEnt( boltInfo );
}

static qboolean SV_G2API_DetachG2Model( CGhoul2Info *ghlInfo )
{
	return re.G2API_DetachG2Model( ghlInfo );
}
static qboolean SV_G2API_GetAnimFileName( CGhoul2Info *ghlInfo, char **filename )
{
	return re.G2API_GetAnimFileName( ghlInfo, filename );
}

static char* SV_G2API_GetAnimFileNameIndex( qhandle_t modelIndex )
{
	return re.G2API_GetAnimFileNameIndex( modelIndex );
}

static char* SV_G2API_GetAnimFileInternalNameIndex( qhandle_t modelIndex )
{
	return re.G2API_GetAnimFileInternalNameIndex( modelIndex );
}

static int SV_G2API_GetAnimIndex( CGhoul2Info *ghlInfo )
{
	return re.G2API_GetAnimIndex( ghlInfo );
}

static qboolean SV_G2API_GetAnimRange( CGhoul2Info *ghlInfo, const char *boneName, int *startFrame, int *endFrame )
{
	return re.G2API_GetAnimRange( ghlInfo, boneName, startFrame, endFrame );
}

static qboolean SV_G2API_GetAnimRangeIndex( CGhoul2Info *ghlInfo, const int boneIndex, int *startFrame, int *endFrame )
{
	return re.G2API_GetAnimRangeIndex( ghlInfo, boneIndex, startFrame, endFrame );
}

static qboolean SV_G2API_GetBoneAnim(
	CGhoul2Info *ghlInfo, const char *boneName, const int AcurrentTime,
    float *currentFrame, int *startFrame, int *endFrame, int *flags, float *animSpeed, int *modelList )
{
	return re.G2API_GetBoneAnim( ghlInfo, boneName, AcurrentTime, currentFrame,
		startFrame, endFrame, flags, animSpeed, modelList );
}

static qboolean SV_G2API_GetBoneAnimIndex(CGhoul2Info *ghlInfo, const int iBoneIndex, const int AcurrentTime,
    float *currentFrame, int *startFrame, int *endFrame, int *flags, float *animSpeed, int *modelList)
{
	return re.G2API_GetBoneAnimIndex( ghlInfo, iBoneIndex, AcurrentTime, currentFrame,
		startFrame, endFrame, flags, animSpeed, modelList );
}

static int SV_G2API_GetBoneIndex( CGhoul2Info *ghlInfo, const char *boneName, qboolean bAddIfNotFound )
{
	return re.G2API_GetBoneIndex( ghlInfo, boneName, bAddIfNotFound );
}

static qboolean SV_G2API_GetBoltMatrix(
	CGhoul2Info_v &ghoul2, const int modelIndex, const int boltIndex, mdxaBone_t *matrix, const vec3_t angles,
	const vec3_t position, const int AframeNum, qhandle_t *modelList, const vec3_t scale )
{
	return re.G2API_GetBoltMatrix(ghoul2, modelIndex, boltIndex, matrix, angles,
		position, AframeNum, modelList, scale );
}

static int SV_G2API_GetGhoul2ModelFlags( CGhoul2Info *ghlInfo )
{
	return re.G2API_GetGhoul2ModelFlags( ghlInfo );
}

static char* SV_G2API_GetGLAName( CGhoul2Info *ghlInfo )
{
	return re.G2API_GetGLAName( ghlInfo );
}

static int SV_G2API_GetParentSurface( CGhoul2Info *ghlInfo, const int index )
{
	return re.G2API_GetParentSurface( ghlInfo, index );
}

static qboolean SV_G2API_GetRagBonePos(
	CGhoul2Info_v &ghoul2, const char *boneName, vec3_t pos, vec3_t entAngles, vec3_t entPos, vec3_t entScale)
{
	return re.G2API_GetRagBonePos( ghoul2, boneName, pos, entAngles, entPos, entScale );
}

static int SV_G2API_GetSurfaceIndex( CGhoul2Info *ghlInfo, const char *surfaceName )
{
	return re.G2API_GetSurfaceIndex( ghlInfo, surfaceName );
}

static char* SV_G2API_GetSurfaceName( CGhoul2Info *ghlInfo, int surfNumber )
{
	return re.G2API_GetSurfaceName( ghlInfo, surfNumber );
}

static int SV_G2API_GetSurfaceRenderStatus( CGhoul2Info *ghlInfo, const char *surfaceName )
{
	return re.G2API_GetSurfaceRenderStatus( ghlInfo, surfaceName );
}

static void SV_G2API_GiveMeVectorFromMatrix( mdxaBone_t &boltMatrix, Eorientations flags, vec3_t &vec )
{
	re.G2API_GiveMeVectorFromMatrix( boltMatrix, flags, vec );
}

static qboolean SV_G2API_HaveWeGhoul2Models( CGhoul2Info_v &ghoul2 )
{
	return re.G2API_HaveWeGhoul2Models( ghoul2 );
}

static qboolean SV_G2API_IKMove( CGhoul2Info_v &ghoul2, int time, sharedIKMoveParams_t *params )
{
	return re.G2API_IKMove( ghoul2, time, params );
}

static int SV_G2API_InitGhoul2Model(CGhoul2Info_v &ghoul2, const char *fileName, int modelIndex,
    qhandle_t customSkin, qhandle_t customShader, int modelFlags, int lodBias)
{
	return re.G2API_InitGhoul2Model( ghoul2, fileName, modelIndex, customSkin, customShader, modelFlags, lodBias );
}

static qboolean SV_G2API_IsPaused( CGhoul2Info *ghlInfo, const char *boneName )
{
	return re.G2API_IsPaused( ghlInfo, boneName );
}

static void SV_G2API_ListBones( CGhoul2Info *ghlInfo, int frame )
{
	return re.G2API_ListBones( ghlInfo, frame );
}

static void SV_G2API_ListSurfaces( CGhoul2Info *ghlInfo )
{
	return re.G2API_ListSurfaces( ghlInfo );
}

static void SV_G2API_LoadGhoul2Models( CGhoul2Info_v &ghoul2, char *buffer )
{
	return re.G2API_LoadGhoul2Models( ghoul2, buffer );
}

static void SV_G2API_LoadSaveCodeDestructGhoul2Info( CGhoul2Info_v &ghoul2 )
{
	return re.G2API_LoadSaveCodeDestructGhoul2Info( ghoul2 );
}

static qboolean SV_G2API_PauseBoneAnim( CGhoul2Info *ghlInfo, const char *boneName, const int AcurrentTime )
{
	return re.G2API_PauseBoneAnim( ghlInfo, boneName, AcurrentTime );
}

static qboolean SV_G2API_PauseBoneAnimIndex( CGhoul2Info *ghlInfo, const int boneIndex, const int AcurrentTime )
{
	return re.G2API_PauseBoneAnimIndex( ghlInfo, boneIndex, AcurrentTime );
}

static qhandle_t SV_G2API_PrecacheGhoul2Model( const char *fileName )
{
	return re.G2API_PrecacheGhoul2Model( fileName );
}

static qboolean SV_G2API_RagEffectorGoal( CGhoul2Info_v &ghoul2, const char *boneName, vec3_t pos )
{
	return re.G2API_RagEffectorGoal( ghoul2, boneName, pos );
}

static qboolean SV_G2API_RagEffectorKick( CGhoul2Info_v &ghoul2, const char *boneName, vec3_t velocity )
{
	return re.G2API_RagEffectorKick( ghoul2, boneName, velocity );
}

static qboolean SV_G2API_RagForceSolve( CGhoul2Info_v &ghoul2, qboolean force )
{
	return re.G2API_RagForceSolve( ghoul2, force );
}

static qboolean SV_G2API_RagPCJConstraint( CGhoul2Info_v &ghoul2, const char *boneName, vec3_t min, vec3_t max )
{
	return re.G2API_RagPCJConstraint( ghoul2, boneName, min, max );
}

static qboolean SV_G2API_RagPCJGradientSpeed( CGhoul2Info_v &ghoul2, const char *boneName, const float speed )
{
	return re.G2API_RagPCJGradientSpeed( ghoul2, boneName, speed );
}

static qboolean SV_G2API_RemoveBolt( CGhoul2Info *ghlInfo, const int index )
{
	return re.G2API_RemoveBolt( ghlInfo, index );
}

static qboolean SV_G2API_RemoveBone( CGhoul2Info *ghlInfo, const char *boneName )
{
	return re.G2API_RemoveBone( ghlInfo, boneName );
}

static qboolean SV_G2API_RemoveGhoul2Model( CGhoul2Info_v &ghlInfo, const int modelIndex )
{
	return re.G2API_RemoveGhoul2Model( ghlInfo, modelIndex );
}

static qboolean SV_G2API_RemoveSurface( CGhoul2Info *ghlInfo, const int index )
{
	return re.G2API_RemoveSurface( ghlInfo, index );
}

static void  SV_G2API_SaveGhoul2Models( CGhoul2Info_v &ghoul2 )
{
	return re.G2API_SaveGhoul2Models( ghoul2 );
}

static qboolean SV_G2API_SetAnimIndex( CGhoul2Info *ghlInfo, const int index )
{
	return re.G2API_SetAnimIndex( ghlInfo, index );
}

static qboolean SV_G2API_SetBoneAnim(CGhoul2Info *ghlInfo, const char *boneName, const int startFrame, const int endFrame,
    const int flags, const float animSpeed, const int AcurrentTime, const float setFrame, const int blendTime)
{
	return re.G2API_SetBoneAnim( ghlInfo, boneName, startFrame, endFrame, flags,
		animSpeed, AcurrentTime, setFrame, blendTime );
}

static qboolean SV_G2API_SetBoneAnimIndex(CGhoul2Info *ghlInfo, const int index, const int startFrame, const int endFrame,
    const int flags, const float animSpeed, const int AcurrentTime, const float setFrame, const int blendTime)
{
	return re.G2API_SetBoneAnimIndex( ghlInfo, index, startFrame, endFrame, flags,
		animSpeed, AcurrentTime, setFrame, blendTime );
}

static qboolean SV_G2API_SetBoneAngles(CGhoul2Info *ghlInfo, const char *boneName, const vec3_t angles, const int flags,
    const Eorientations up, const Eorientations left, const Eorientations forward, qhandle_t *modelList,
    int blendTime, int AcurrentTime)
{
	return re.G2API_SetBoneAngles( ghlInfo, boneName, angles, flags, up, left, forward,
		modelList, blendTime, AcurrentTime );
}

static qboolean SV_G2API_SetBoneAnglesIndex(CGhoul2Info *ghlInfo, const int index, const vec3_t angles, const int flags,
    const Eorientations yaw, const Eorientations pitch, const Eorientations roll, qhandle_t *modelList,
    int blendTime, int AcurrentTime)
{
	return re.G2API_SetBoneAnglesIndex( ghlInfo, index, angles, flags, yaw, pitch, roll,
		modelList, blendTime, AcurrentTime );
}

static qboolean SV_G2API_SetBoneAnglesMatrix(CGhoul2Info *ghlInfo, const char *boneName, const mdxaBone_t &matrix,
    const int flags, qhandle_t *modelList, int blendTime, int AcurrentTime)
{
	return re.G2API_SetBoneAnglesMatrix( ghlInfo, boneName, matrix, flags, modelList, blendTime, AcurrentTime );
}

static qboolean SV_G2API_SetBoneAnglesMatrixIndex(CGhoul2Info *ghlInfo, const int index, const mdxaBone_t &matrix,
    const int flags, qhandle_t *modelList, int blandeTime, int AcurrentTime)
{
	return re.G2API_SetBoneAnglesMatrixIndex( ghlInfo, index, matrix, flags, modelList, blandeTime, AcurrentTime );
}

static qboolean SV_G2API_SetBoneIKState(CGhoul2Info_v &ghoul2, int time, const char *boneName, int ikState,
    sharedSetBoneIKStateParams_t *params)
{
	return re.G2API_SetBoneIKState( ghoul2, time, boneName, ikState, params );
}

static qboolean SV_G2API_SetGhoul2ModelFlags( CGhoul2Info *ghlInfo, const int flags )
{
	return re.G2API_SetGhoul2ModelFlags( ghlInfo, flags );
}

static qboolean SV_G2API_SetLodBias( CGhoul2Info *ghlInfo, int lodBias )
{
	return re.G2API_SetLodBias( ghlInfo, lodBias );
}

static qboolean SV_G2API_SetNewOrigin( CGhoul2Info *ghlInfo, const int boltIndex )
{
	return re.G2API_SetNewOrigin( ghlInfo, boltIndex );
}

static void  SV_G2API_SetRagDoll( CGhoul2Info_v &ghoul2, CRagDollParams *parms )
{
	return re.G2API_SetRagDoll( ghoul2, parms );
}

static qboolean SV_G2API_SetRootSurface( CGhoul2Info_v &ghlInfo, const int modelIndex, const char *surfaceName )
{
	return re.G2API_SetRootSurface( ghlInfo, modelIndex, surfaceName );
}

static qboolean SV_G2API_SetShader( CGhoul2Info *ghlInfo, qhandle_t customShader )
{
	return re.G2API_SetShader( ghlInfo, customShader );
}

static qboolean SV_G2API_SetSkin( CGhoul2Info *ghlInfo, qhandle_t customSkin, qhandle_t renderSkin )
{
	return re.G2API_SetSkin( ghlInfo, customSkin, renderSkin );
}

static qboolean SV_G2API_SetSurfaceOnOff( CGhoul2Info *ghlInfo, const char *surfaceName, const int flags )
{
	return re.G2API_SetSurfaceOnOff( ghlInfo, surfaceName, flags );
}

static qboolean SV_G2API_StopBoneAnim( CGhoul2Info *ghlInfo, const char *boneName )
{
	return re.G2API_StopBoneAnim( ghlInfo, boneName );
}

static qboolean SV_G2API_StopBoneAnimIndex( CGhoul2Info *ghlInfo, const int index )
{
	return re.G2API_StopBoneAnimIndex( ghlInfo, index );
}

static qboolean SV_G2API_StopBoneAngles( CGhoul2Info *ghlInfo, const char *boneName )
{
	return re.G2API_StopBoneAngles( ghlInfo, boneName );
}

static qboolean SV_G2API_StopBoneAnglesIndex( CGhoul2Info *ghlInfo, const int index )
{
	return re.G2API_StopBoneAnglesIndex( ghlInfo, index );
}

#ifdef _G2_GORE
static void  SV_G2API_AddSkinGore( CGhoul2Info_v &ghoul2, SSkinGoreData &gore )
{
	return re.G2API_AddSkinGore( ghoul2, gore );
}

static void  SV_G2API_ClearSkinGore( CGhoul2Info_v &ghoul2 )
{
	return re.G2API_ClearSkinGore( ghoul2 );
}
#else
static void SV_G2API_AddSkinGore(
    CGhoul2Info_v &ghoul2,
    SSkinGoreData &gore)
{
    static_cast<void>(ghoul2);
    static_cast<void>(gore);
}

static void SV_G2API_ClearSkinGore(
    CGhoul2Info_v &ghoul2)
{
    static_cast<void>(ghoul2);
}
#endif

static IGhoul2InfoArray& SV_TheGhoul2InfoArray( void )
{
	return re.TheGhoul2InfoArray();
}

static qhandle_t SV_RE_RegisterSkin( const char *name )
{
	return re.RegisterSkin( name );
}

struct sof2_skin_group_stub_t {
	int unknown0;
	int variationCount;
};

struct sof2_skin_table_stub_t {
	char pad0[0x44];
	sof2_skin_group_stub_t *groups[6];
};

struct sof2_skin_stub_t {
	char pad0[0x48];
	int hasGroups;
	sof2_skin_table_stub_t *table;
	qhandle_t rendererHandle;
};

static std::map<std::string, sof2_skin_stub_t> s_sof2SkinStubs;

static sof2_skin_stub_t *SV_GetSOF2SkinStub( const char *name ) {
	static sof2_skin_group_stub_t s_groupStubs[6];
	static sof2_skin_table_stub_t s_tableStub;
	static qboolean s_skinStubInit = qfalse;
	std::string key;
	sof2_skin_stub_t &stub = s_sof2SkinStubs[key = ( name ? name : "" )];

	if ( !s_skinStubInit ) {
		memset( &s_tableStub, 0, sizeof( s_tableStub ) );
		for ( int i = 0; i < 6; ++i ) {
			s_groupStubs[i].unknown0 = 0;
			s_groupStubs[i].variationCount = 1;
			s_tableStub.groups[i] = &s_groupStubs[i];
		}
		s_skinStubInit = qtrue;
	}

	memset( &stub, 0, sizeof( stub ) );
	stub.hasGroups = 1;
	stub.table = &s_tableStub;
	stub.rendererHandle = re.RegisterSkin( name );
	return &stub;
}

static int SV_RE_GetAnimationCFG( const char *psCFGFilename, char *psDest, int iDestSize )
{
	return re.GetAnimationCFG( psCFGFilename, psDest, iDestSize );
}

static bool SV_WE_GetWindVector( vec3_t windVector, vec3_t atPoint )
{
	return re.GetWindVector( windVector, atPoint );
}

static bool SV_WE_GetWindGusting( vec3_t atpoint )
{
	return re.GetWindGusting( atpoint );
}

static bool SV_WE_IsOutside( vec3_t pos )
{
	return re.IsOutside( pos );
}

static float SV_WE_IsOutsideCausingPain( vec3_t pos )
{
	return re.IsOutsideCausingPain( pos );
}

static float SV_WE_GetChanceOfSaberFizz( void )
{
	return re.GetChanceOfSaberFizz();
}

static bool SV_WE_IsShaking( vec3_t pos )
{
	return re.IsShaking( pos );
}

static void SV_WE_AddWeatherZone( vec3_t mins, vec3_t maxs )
{
	return re.AddWeatherZone( mins, maxs );
}

static bool SV_WE_SetTempGlobalFogColor( vec3_t color )
{
	return re.SetTempGlobalFogColor( color );
}

// ============================================================
// SOF2 game_import_t stub/wrapper functions
// ============================================================
// Definitive slot mapping from SoF2.exe SV_InitGameProgs @ 0x10014c60
// (pseudocode at E:\SOF2\pseudocode\server.c lines 2626-2738)

// Extern declarations for engine functions used in the import table
extern int  Com_EventLoop( void );
extern void Cmd_TokenizeString( const char *text_in );
extern void Cbuf_ExecuteText( int exec_when, const char *text );
extern void Cmd_ArgvBuffer( int arg, char *buffer, int bufferLength );
extern void Cmd_ArgsBuffer( char *buffer, int bufferLength );
extern void Cvar_Register( vmCvar_t *vmCvar, const char *varName, const char *defaultValue, int flags );
extern void Cvar_Update( vmCvar_t *vmCvar );
extern float Cvar_VariableValue( const char *var_name );
extern qboolean FS_FileExists( const char *file );
extern char ** FS_ListFiles( const char *path, const char *extension, int *numfiles );
extern char ** FS_ListFilteredFiles( const char *path, const char *extension, char *filter, int *numfiles );
extern void FS_FreeFileList( char **fileList );

// gi[3]: Com_sprintf_void — uses the one defined near top of file
// gi[14]: FS_CleanPath — SOF2-specific path normalization (stub)
static void SV_FS_CleanPath_Stub( const char *prefix, const char *path, char *out, int outSize ) {
	Com_sprintf( out, outSize, "%s/%s", prefix, path );
}

// gi[27]: Cvar_SetModified(name, default, flags, force) — 4-arg Cvar_Set variant
static void SV_Cvar_SetModified( const char *name, const char *value, int flags, int /*force*/ ) {
	Cvar_Get( name, value, flags );
}

// gi[28]: Cvar_SetValue(name, val, force) — SOF2 adds 'force' param; engine ignores it
static void SV_Cvar_SetValue_Wrapper( const char *name, float val, int /*force*/ ) {
	Cvar_SetValue( name, val );
}

// gi[32]: G_ZMalloc_Wrapper — uses the one defined near top of file
// gi[34]: Z_CheckHeap (no-op)
static void SV_Z_CheckHeap( void ) {}

// gi[35-49]: Terrain/RMG stubs
static void SV_CM_RegisterTerrain_Stub( const char * /*config*/ ) {}
static void SV_CM_Terrain_Release_Stub( int /*terrainId*/ ) { fprintf(stderr, "[GI] CM_Terrain_Release_Stub\n"); }
static void SV_CM_Terrain_ForEachBrush_Stub( void ) { fprintf(stderr, "[GI] CM_Terrain_ForEachBrush_Stub\n"); }
static float SV_CM_Terrain_GetWorldHeight_Stub( const vec3_t /*pos*/ ) { fprintf(stderr, "[GI] CM_Terrain_GetWorldHeight_Stub\n"); return 0.0f; }
static float SV_CM_Terrain_FlattenHeight_Stub( const vec3_t /*pos*/, float /*radius*/ ) { fprintf(stderr, "[GI] CM_Terrain_FlattenHeight_Stub\n"); return 0.0f; }
static int SV_RMG_EvaluatePath_Stub( void ) { fprintf(stderr, "[GI] RMG_EvaluatePath_Stub\n"); return 0; }
static int SV_RMG_CreatePathSegment_Stub( void ) { fprintf(stderr, "[GI] RMG_CreatePathSegment_Stub\n"); return 0; }
static void SV_CM_Terrain_ApplyBezierPath_Stub( void ) { fprintf(stderr, "[GI] CM_Terrain_ApplyBezierPath_Stub\n"); }
static void SV_CTerrainInstanceList_Add_Stub( void ) { fprintf(stderr, "[GI] CTerrainInstanceList_Add_Stub\n"); }
static float SV_CM_Terrain_GetFlattenedAvgHeight_Stub( void ) { fprintf(stderr, "[GI] CM_Terrain_GetFlattenedAvgHeight_Stub\n"); return 0.0f; }
static int SV_CM_Terrain_CheckOverlap_Stub( void ) { fprintf(stderr, "[GI] CM_Terrain_CheckOverlap_Stub\n"); return 0; }
static void *SV_CTerrainInstanceList_GetFirst_Stub( void ) { fprintf(stderr, "[GI] CTerrainInstanceList_GetFirst_Stub\n"); return NULL; }
static void *SV_CTerrainInstanceList_GetNext_Stub( void ) { fprintf(stderr, "[GI] CTerrainInstanceList_GetNext_Stub\n"); return NULL; }
static void SV_R_LoadDataImage_Stub( void ) {}
static void SV_ResampleTexture_Stub( void ) {}

// gi[50]: CM_PointContents — basic 2-arg version (point, passEntity)
static int SV_CM_PointContents_Wrapper( const vec3_t point, int /*passEntity*/ ) {
	return CM_PointContents( point, 0 );
}

// gi[51]: SV_PointContentsCached — 3-arg version (point, passEntity, flags)
static int SV_PointContentsCached( const vec3_t point, int passEntityNum, int /*flags*/ ) {
	return SV_PointContents( point, passEntityNum );
}

// gi[52-53]: SaveGame chunk I/O — wraps ojk SavedGame helper
static void SV_SaveGame_WriteChunk( int chunkId, void *data, int length, int /*compress*/ ) {
	ojk::SavedGameHelper( &ojk::SavedGame::get_instance() ).write_chunk(
		chunkId, static_cast<const uint8_t *>(data), length );
}
static void SV_SaveGame_ReadChunk( int chunkId, int /*unk1*/, int /*unk2*/, void **outPtr, int /*unk3*/ ) {
	// Stub: SOF2 save game read — not needed for initial map load
	if (outPtr) *outPtr = NULL;
}

// gi[54]: LocateGameData — SOF2 passes 5 args
// SOF2: gentities is the CEntitySystem pointer table (NOT a contiguous struct array).
// Each entry is a 4-byte CEntity* pointer; entitySize (0x560) is stored but unused for access.
static void SV_LocateGameData_SOF2( void *gentities, int numEntities, int entitySize, void *clients, int clientSize ) {
	int i;

	sv_GEntities      = gentities;
	sv_GEntitySize    = entitySize;
	sv_numGEntities   = numEntities;
	sv_GameClients    = clients;
	sv_GameClientSize = clientSize;
	memset( sv_sof2BrushModel, 0, sizeof( sv_sof2BrushModel ) );
	Com_Printf("[DBG] SV_LocateGameData: ents=%p num=%d size=0x%x clients=%p clientSize=0x%x\n",
		gentities, numEntities, entitySize, clients, clientSize);
	for ( i = 0; i < numEntities && i < MAX_GENTITIES; ++i ) {
		gentity_t *ent = ((gentity_t **)sv_GEntities)[i];
		if ( !ent ) {
			continue;
		}
		SOF2_ENT_NUMBER( ent ) = i;
		SOF2_ENT_SERVERINDEX( ent ) = i;
	}
}

// Traced version of SV_LocateGameData
static void SV_Traced_LocateGameData( void *ents, int numEnts, int entSize, void *clients, int clientSize ) {
	gi_call_count++;
	fprintf(stderr, "[GI] gi_SV_LocateGameData called (#%d) ents=%p numEnts=%d entSize=0x%x clients=%p clientSize=0x%x\n",
		gi_call_count, ents, numEnts, entSize, clients, clientSize);
	SV_LocateGameData_SOF2( ents, numEnts, entSize, clients, clientSize );
}

// gi[55]: SV_GameDropClient (already defined above in the file)

// gi[56]: SV_GameSendServerCommand (already defined above)

// gi[58]: SV_UnlinkEntityAll — same as SV_UnlinkEntity for our purposes
static void SV_UnlinkEntityAll( gentity_t *ent ) {
	SV_UnlinkEntity( ent );
}

// gi[59]: SV_AddTempEnt
static void SV_AddTempEnt_Stub( void * /*ent*/ ) {}

// gi[60]: CM_GetDebugEntryPrevCount
static int SV_CM_GetDebugEntryPrevCount_Stub( void ) { return 0; }

// gi[61]: SV_ClearTempEnts
static void SV_ClearTempEnts_Stub( void ) {}

// gi[62]: SV_GameStub_ReturnZero
static int SV_GameStub_ReturnZero( void ) { return 0; }

// gi[63]: CSkin_Noop
static void SV_CSkin_Noop( void ) {}

// gi[64-66]: SV_AddNextMap/RemoveNextMap/GetNextMap — ICARUS map sequencing stubs
static const char *SV_AddNextMap_Stub( ... ) {
	static int logCount = 0;
	if ( logCount < 6 ) {
		Com_Printf( "[SOF2 gi] AddNextMap stub hit\n" );
		logCount++;
	}
	return "";
}
static const char *SV_RemoveNextMap_Stub( ... ) {
	static int logCount = 0;
	if ( logCount < 6 ) {
		Com_Printf( "[SOF2 gi] RemoveNextMap stub hit\n" );
		logCount++;
	}
	return "";
}
static const char *SV_GetNextMap_Stub( ... ) {
	static int logCount = 0;
	if ( logCount < 6 ) {
		Com_Printf( "[SOF2 gi] GetNextMap stub hit\n" );
		logCount++;
	}
	return "";
}

typedef int (*sof2_gp_vcall_stub_t)( ... );
struct sof2_gp_group_stub_t {
	sof2_gp_vcall_stub_t *vftable;
};
static int SV_SOF2_GPVCall_Stub( ... ) {
	return 0;
}
static sof2_gp_vcall_stub_t s_sof2GpVTable[64];
static sof2_gp_group_stub_t s_sof2GpGroup = { s_sof2GpVTable };
static qboolean s_sof2GpInited = qfalse;

static void SV_SOF2_InitGPStub( void ) {
	int i;
	if ( s_sof2GpInited ) {
		return;
	}
	for ( i = 0; i < (int)( sizeof( s_sof2GpVTable ) / sizeof( s_sof2GpVTable[0] ) ); ++i ) {
		s_sof2GpVTable[i] = SV_SOF2_GPVCall_Stub;
	}
	s_sof2GpInited = qtrue;
}

static void *SV_GP_GetBaseParseGroup_Stub( ... ) {
	static int logCount = 0;
	SV_SOF2_InitGPStub();
	if ( logCount < 8 ) {
		Com_Printf( "[SOF2 gi] GP_GetBaseParseGroup -> %p\n", (void *)&s_sof2GpGroup );
		logCount++;
	}
	return &s_sof2GpGroup;
}
static void SV_GP_SetSubGroups_Stub( ... ) {
	static int logCount = 0;
	SV_SOF2_InitGPStub();
	if ( logCount < 8 ) {
		Com_Printf( "[SOF2 gi] GP_SetSubGroups stub hit\n" );
		logCount++;
	}
}

// gi[71]: SV_Trace — 10-arg wrapper matching SOF2 gamex86.dll calling convention
// SOF2 game DLL pushes 10 args: results, start, mins, maxs, end, passEnt, contentmask,
//   eG2TraceType, useLod, traceFlags(always 0)
// Set to 1 when game DLL has received a non-zero-move usercmd — trace logging starts then

static void SV_Trace_10args( trace_t *results, const vec3_t start,
		const vec3_t mins, const vec3_t maxs, const vec3_t end,
		int passEnt, int contentmask,
		int eG2TraceType, int useLod, int /*traceFlags*/ ) {
	SV_Trace( results, start, mins, maxs, end, passEnt, contentmask,
		(EG2_Collision)eG2TraceType, useLod );

	if ( sv_trace_log_armed && sv_trace_log_count < 24 ) {
		gentity_t *hitEnt = NULL;
		int modelindex = 0;
		int modelindex2 = 0;
		int solid = 0;
		int svFlags = 0;
		int eType = 0;
		int linked = 0;
		int bmodel = 0;

		if ( results && results->entityNum >= 0 && results->entityNum < MAX_GENTITIES ) {
			hitEnt = SV_GentityNum( results->entityNum );
			if ( hitEnt ) {
				modelindex = SOF2_ENT_MODELINDEX( hitEnt );
				modelindex2 = *(int *)((byte *)hitEnt + 0x0AC);
				solid = SOF2_ENT_SOLID( hitEnt );
				svFlags = SOF2_ENT_SVFLAGS( hitEnt );
				eType = SOF2_ENT_ETYPE( hitEnt );
				linked = SOF2_ENT_LINKED( hitEnt );
				bmodel = SV_SOF2_IsBrushModelEntity( hitEnt );
			}
		}

		Com_Printf(
			"[GI trace:%s] #%d start=(%.1f,%.1f,%.1f) end=(%.1f,%.1f,%.1f) mins=(%.1f,%.1f,%.1f) maxs=(%.1f,%.1f,%.1f) pass=%d mask=0x%X hit=%d frac=%.3f startsolid=%d allsolid=%d model=%d/%d solid=%d svf=0x%X etype=%d linked=%d bmodel=%d\n",
			( sv_trace_log_reason == 2 ) ? "use" : "move",
			sv_trace_log_count + 1,
			start[0], start[1], start[2],
			end[0], end[1], end[2],
			mins ? mins[0] : 0.0f, mins ? mins[1] : 0.0f, mins ? mins[2] : 0.0f,
			maxs ? maxs[0] : 0.0f, maxs ? maxs[1] : 0.0f, maxs ? maxs[2] : 0.0f,
			passEnt,
			(unsigned int)contentmask,
			results ? results->entityNum : -1,
			results ? results->fraction : 1.0f,
			results ? (int)results->startsolid : 0,
			results ? (int)results->allsolid : 0,
			modelindex,
			modelindex2,
			solid,
			(unsigned int)svFlags,
			eType,
			linked,
			bmodel );
		++sv_trace_log_count;
	}
}

// gi[72]: SV_R_LightForPoint — renderer function (stub)
static int SV_R_LightForPoint_Stub( const vec3_t /*point*/, vec3_t /*ambientLight*/,
		vec3_t /*directedLight*/, vec3_t /*lightDir*/ ) { return 0; }

// gi[74]: CM_SelectSubBSP
static const char *SV_CM_SelectSubBSP_Stub( int /*index*/ ) { return NULL; }

// gi[76]: CM_FindOrCreateShader
static int SV_CM_FindOrCreateShader_Stub( const char * /*name*/ ) { return 0; }

// gi[81]: CM_GetModelBrushShaderNum
static int SV_CM_GetModelBrushShaderNum_Stub( int /*modelIndex*/, int /*brushNum*/ ) { return 0; }

// gi[82]: CM_GetShaderName
static const char *SV_CM_GetShaderName_Stub( int /*shaderNum*/ ) { return ""; }

// gi[83]: CM_DamageBrushSideHealth
static void SV_CM_DamageBrushSideHealth_Stub( int /*brushSide*/, int /*damage*/ ) {}

// gi[84]: SV_GetUsercmd
// SOF2 SP only pushes the destination usercmd pointer here. The game DLL
// implicitly expects the local single-player client's most recent command.
// DLL playerState pointer — set inside SV_GetUsercmd_SOF2 each frame.
// cmd = ps+0x1DC, so ps = (char*)cmd - 0x1DC.
// Used by sv_client.cpp after ClientThink to sync DLL position to engine playerState.
char *g_sof2_dll_ps = NULL;
static const int SOF2_PMF_NOCLIPMOVE = 0x100;

static void SV_GetUsercmd_SOF2( usercmd_t *cmd ) {
	static int getCmdLogCount = 0;
	static int getCmdMoveLogCount = 0;
	static int getCmdLockClearLogCount = 0;
	if ( !cmd ) {
		return;
	}

	// Derive DLL's playerState base from the embedded usercmd slot address.
	// The DLL calls gi[84] with (ps+0x1DC), so ps = cmd - 0x1DC.
	g_sof2_dll_ps = (char *)cmd - 0x1DC;

	// Fix J + L: force DLL playerState values so movement runs correctly.
	// SOF2 playerState_t offsets:
	//   ps+0x04 = pm_type
	//   ps+0x50 = delta_angles[3]
	//   ps+0xD0 = stats[0]/health
	//
	// (a) Health=100: PM_UpdateViewAngles exits early if health<1, freezing viewangles at
	//     spawn orientation → all movement goes in wrong direction. Health=100 also stops
	//     ClientThink from re-assigning pm_type=2 via CPlayer::vtable[1]() health check.
	// (b) Keep DLL delta_angles aligned with the current server playerState so
	//     command angle decoding stays consistent with client input.
	// (c) pm_type=0: even with health=100, pm_type may carry over as 2 from the previous
	//     frame (set when health was 0). pm_type=2 → PM_FlyMove (3D, no ground detect)
	//     instead of PM_WalkMove (horizontal, gravity). Forcing to 0 each frame ensures
	//     PM_WalkMove with proper ground collision.
	{
		int *pm_type_ptr   = (int *)(g_sof2_dll_ps + 0x04);
		int *pm_flags_ptr  = (int *)(g_sof2_dll_ps + 0x0C);
		int *pm_time_ptr   = (int *)(g_sof2_dll_ps + 0x10);
		int *dll_delta = (int *)(g_sof2_dll_ps + 0x50);
		int *health        = (int *)(g_sof2_dll_ps + 0xD0);
		int *moveType      = (int *)(g_sof2_dll_ps + 2212);
		unsigned char *noclip = (unsigned char *)(g_sof2_dll_ps + 1701);
		playerState_t *enginePs = SV_GameClientNum( 0 );
		const int originalMoveType = *moveType;
		const int originalPmFlags = *pm_flags_ptr;
		const int originalPmTime = *pm_time_ptr;
		*pm_type_ptr   = 0;   // PM_NORMAL — use PM_WalkMove with ground detection
		if ( enginePs ) {
			dll_delta[0] = enginePs->delta_angles[0];
			dll_delta[1] = enginePs->delta_angles[1];
			dll_delta[2] = enginePs->delta_angles[2];
		} else {
			dll_delta[0] = 0;
			dll_delta[1] = 0;
			dll_delta[2] = 0;
		}
		if ( *health < 1 ) {
			*health = 100;
		}
		// Clear time-based movement locks and retail noclip-mode bit that can pin the
		// player into PM_NoclipMove after direct +map startup.
		*pm_time_ptr = 0;
		*pm_flags_ptr &= ~( PMF_ALL_TIMES | PMF_RESPAWNED | PMF_TRIGGER_PUSHED | SOF2_PMF_NOCLIPMOVE );
		if ( *noclip != 0 ) {
			static int s_noclipLogCount = 0;
			if ( s_noclipLogCount < 24 ) {
				Com_Printf(
					"[SOF2 fix] cleared noclip (SV_GetUsercmd) value=%u\n",
					(unsigned int)( *noclip ) );
				++s_noclipLogCount;
			}
			*noclip = 0;
		}
		// Fix N: clear ps+0x1FC — ClientThink @ RVA 0x4d009 reads this byte as a
		// noclip-mode flag. If non-zero it immediately sets pm_type=1 (PM_NOCLIP),
		// enabling full 3D free-flight before the health check. This field is set by
		// a direct +map startup path and is never cleared by our existing patches
		// (which cleared gclient->noclip at offset 1701, a completely different field).
		// pra1: this byte is non-zero from spawn → pm_type=1 every frame from the start.
		// air1: set by ICARUS/script during movement → pm_type=1 after first W+lookup.
		{
			volatile int *psNoclipByte = (volatile int *)(g_sof2_dll_ps + 0x1FC);
			if ( *psNoclipByte != 0 ) {
				static int s_ps1fcLogCount = 0;
				if ( s_ps1fcLogCount < 24 ) {
					Com_Printf(
						"[SOF2 fix] cleared ps+0x1FC noclip byte (SV_GetUsercmd) value=0x%X\n",
						(unsigned int)*psNoclipByte );
					++s_ps1fcLogCount;
				}
				*psNoclipByte = 0;
			}
		}
		// Keep retail movement in walk/run mode for direct map startup.
		if ( *moveType != 2 ) {
			static int s_moveTypeLogCount = 0;
			if ( s_moveTypeLogCount < 32 ) {
				Com_Printf(
					"[SOF2 fix] normalized moveType (SV_GetUsercmd) %d -> %d\n",
					*moveType,
					2 );
				++s_moveTypeLogCount;
			}
			*moveType = 2;
		}
		if ( getCmdLockClearLogCount < 32 &&
			( originalMoveType != *moveType ||
			  originalPmTime != *pm_time_ptr ||
			  originalPmFlags != *pm_flags_ptr ) ) {
			Com_Printf(
				"[SOF2 fix] cleared move locks (SV_GetUsercmd) pm_time=%d->%d pm_flags=0x%X->0x%X moveType=%d->%d\n",
				originalPmTime,
				*pm_time_ptr,
				(unsigned int)originalPmFlags,
				(unsigned int)*pm_flags_ptr,
				originalMoveType,
				*moveType );
			++getCmdLockClearLogCount;
		}
	}

	// SOF2's game DLL expects Q3-style layout at the destination pointer:
	//   int[0]=serverTime, int[1-3]=angles[3], int[4]=buttons,
	//   int[5]=weapon|(fwd<<8)|(right<<16)|(up<<24)
	// OpenJK's usercmd_t has a different field order (buttons at +4, weapon at +8
	// with 3 bytes padding, angles at +12..+20, forwardmove at +25).
	// A direct struct copy writes OpenJK layout → DLL reads forwardmove=0 always.
	//
	// Angle convention (Fix L — revert Fix H swap):
	// The earlier swap (out[1]=YAW, out[2]=PITCH) was derived from pre-health-fix data
	// when PM_UpdateViewAngles was NOT running and the ICARUS spawn viewangles coincidentally
	// had PITCH≈YAW≈344°. With PM_UpdateViewAngles now running, the DLL uses standard Q3A
	// AngleVectors convention (viewangles[0]=PITCH, viewangles[1]=YAW) for the forward vector
	// in PM_WalkMove. Using standard Q3A layout: out[1]=PITCH, out[2]=YAW.
	{
		const usercmd_t *src = &svs.clients[0].lastUsercmd;
		int *out = (int *)cmd;
		unsigned int packedMove = 0;
		out[0] = src->serverTime;
		out[1] = src->angles[0];  // PITCH (OpenJK angles[0]) → DLL viewangles[0]
		out[2] = src->angles[1];  // YAW   (OpenJK angles[1]) → DLL viewangles[1]
		out[3] = src->angles[2];  // ROLL

		// Strip BUTTON_ATTACK (bit 0) from the forwarded command.
		// The weapon system in gamex86.dll crashes when curWeapon is NULL (direct +map load
		// has no saved weapon inventory). IsWeaponInFireState is patched to return false to
		// avoid that crash, but this causes a new problem: since the state is always "not
		// firing", every frame that has BUTTON_ATTACK set starts a new fire cycle, generating
		// a rapid-fire sound loop. Until weapon initialization is fully solved, suppress
		// attack input to prevent the sound loop.
		{
			static int s_attackSuppressLogCount = 0;
			unsigned int rawButtons = (unsigned int)src->buttons;
			if ( rawButtons & 1u ) { // BUTTON_ATTACK
				if ( s_attackSuppressLogCount < 8 ) {
					Com_Printf( "[SOF2 fix] suppressed BUTTON_ATTACK in cmd: buttons=0x%08X fwd=%d right=%d up=%d (#%d)\n",
						rawButtons,
						(int)src->forwardmove, (int)src->rightmove, (int)src->upmove,
						s_attackSuppressLogCount + 1 );
					++s_attackSuppressLogCount;
				}
				rawButtons &= ~1u;
			}
			out[4] = (int)rawButtons;
			packedMove = (unsigned char)src->weapon
			       | ( (unsigned char)src->forwardmove << 8 )
			       | ( (unsigned char)src->rightmove   << 16 )
			       | ( (unsigned char)src->upmove      << 24 );
			if ( rawButtons & BUTTON_USE ) {
				static int s_useForwardLogCount = 0;
				if ( !sv_trace_use_latched ) {
					sv_trace_log_armed = 1;
					sv_trace_log_reason = 2;
					sv_trace_log_count = 0;
					sv_trace_use_latched = qtrue;
				}
				if ( s_useForwardLogCount < 64 ) {
					Com_Printf(
						"[USE-ENG] GetUsercmd forwarded buttons=0x%08X packed=0x%08X move=(%d,%d,%d) weapon=%u time=%d\n",
						rawButtons,
						packedMove,
						(int)src->forwardmove,
						(int)src->rightmove,
						(int)src->upmove,
						(unsigned int)src->weapon,
						src->serverTime );
					++s_useForwardLogCount;
				}
			} else {
				sv_trace_use_latched = qfalse;
			}
		}

		out[5] = packedMove;

		// Keep DLL viewangles aligned with the incoming command stream.
		// This avoids cases where movement uses stale spawn orientation.
		{
			int *dll_delta = (int *)(g_sof2_dll_ps + 0x50);
			float *dll_viewangles = (float *)(g_sof2_dll_ps + 0xB0);
			dll_viewangles[0] = SHORT2ANGLE( (short)( src->angles[0] + dll_delta[0] ) );
			dll_viewangles[1] = SHORT2ANGLE( (short)( src->angles[1] + dll_delta[1] ) );
			dll_viewangles[2] = SHORT2ANGLE( (short)( src->angles[2] + dll_delta[2] ) );
		}
	}
	// Log using the original OpenJK usercmd fields
	{
		const usercmd_t *src2 = &svs.clients[0].lastUsercmd;
		static const qboolean svVerboseGetUsercmdLogs = qfalse;
		if ( svVerboseGetUsercmdLogs && getCmdLogCount < 4 ) {
			Com_Printf( "[GI GetUsercmd] #%d move=(%d,%d,%d) ang=(%d,%d,%d) btn=0x%08X cmdTime=%d\n",
				getCmdLogCount + 1,
				(int)src2->forwardmove, (int)src2->rightmove, (int)src2->upmove,
				src2->angles[0], src2->angles[1], src2->angles[2],
				src2->buttons, src2->serverTime );
			getCmdLogCount++;
		}
		if ( src2->forwardmove || src2->rightmove || src2->upmove ) {
			sv_trace_log_armed = 1;
			sv_trace_log_reason = 1;
			if ( svVerboseGetUsercmdLogs && getCmdMoveLogCount < 128 ) {
				Com_Printf( "[GI GetUsercmd MOVE] #%d move=(%d,%d,%d) cmdTime=%d\n",
					getCmdMoveLogCount + 1,
					(int)src2->forwardmove, (int)src2->rightmove, (int)src2->upmove,
					src2->serverTime );
				getCmdMoveLogCount++;
			}
		}
	}
}

// gi[85]: SV_GetEntityToken — uses the one defined at top of file

// gi[86]: RE_RegisterModel — forward to renderer
static qhandle_t SV_RE_RegisterModel_Wrapper( const char *name ) { return re.RegisterModel( name ); }

// gi[87]: RE_RegisterShader — forward to renderer
static qhandle_t SV_RE_RegisterShader_Wrapper( const char *name ) { return re.RegisterShader( name ); }

// gi[87]: G2_InitWraithSurfaceMap — provides a CWraith-like singleton to gamex86.dll
// The DLL calls this with a pointer to its global g_theWraith (void**).
// The DLL then calls virtual methods on the returned object (vtable dispatch).
// SOF2 HandlePool stub — passed as 4th param to ge->Init.
// The DLL stores this pointer at entitySystem+0x30C4, then calls through its vtable:
//   vtable[0]: CEntitySystem_DispatchEvent — Dispatch(int arg1, int arg2), void return
//   vtable[1]: G_SpawnEntityInWorld — Allocate(void *ent, int slotOverride), returns int slot index
//   vtable[2]: CEntitySystem_UnregisterEntity tail-jump target — takes entity pointer.
// MSVC thiscall: this in ECX, callee cleans stack args.
class CHandlePoolStub {
	int nextSlot;
public:
	CHandlePoolStub() : nextSlot(0) {}

	// vtable[0]: Entity handle dispatch — called during entity system events
	virtual void Dispatch(int /*arg1*/, int /*arg2*/) {
		// no-op — silently ignore entity handle dispatch
	}

	// vtable[1]: Allocate entity slot — called by G_SpawnEntityInWorld
	// Returns the slot index to use for the entity in the entity system array.
	virtual int Allocate(void * /*ent*/, int slotOverride) {
		if (slotOverride >= 0) return slotOverride;
		return nextSlot++;
	}

	// vtable[2]: Unregister/free callback used by CEntitySystem_UnregisterEntity.
	// Must exist so gamex86.dll doesn't jump through a null function pointer.
	virtual void Unregister(void * /*ent*/) {
	}
};
static CHandlePoolStub g_handlePoolStub;

// CWraith vtable layout — 78 slots verified from gamex86.dll xref analysis.
// CRITICAL: All methods are __thiscall (callee-clean), so every stub MUST
// declare the correct number of parameters to generate the proper RET N.
// Wrong arg count = stack corruption on return.
//
// NOTE: No virtual destructor! The original SOF2 wraith uses slot[0] for
// AddBoltToModel, not a destructor. We manage cleanup manually.

class CWraithStub {
public:
	enum {
		SOF2_WRAITH_ENTITY_BYTES = 0x140,
		SOF2_WRAITH_GHOUL2_OFFSET = 0xE4
	};

	// Object layout — must place CGhoul2Info_v at offset +0xE4 from `this`:
	// +0x00: vtable ptr (automatic, 4 bytes)
	// +0x04: mPadding (4 bytes)
	// +0x08: mTimeStart (4 bytes) — DLL writes g_levelTime here
	// +0x0C: mTimeEnd (4 bytes) — DLL writes g_levelTime here
	// +0x10: mFlags (1 byte) — DLL writes flags here
	// +0x14 to +0xE3: model data padding (0xD0 bytes)
	// +0xE4: CGhoul2Info_v (4 bytes — handle into global ghoul2 info array)
	// +0xE8 to +0x13F: extra buffer for DLL reads past ghoul2
	int mPadding;              // +0x04
	int mTimeStart;            // +0x08
	int mTimeEnd;              // +0x0C
	char mFlags;               // +0x10
	char mPad2[3];             // +0x11-0x13
	char mModelPad[0xD0];      // +0x14 to +0xE3 — padding so mGhoul2 lands at +0xE4
	CGhoul2Info_v mGhoul2;     // +0xE4 — properly constructed ghoul2 handle
	char mExtraEnd[88];        // +0xE8 to +0x13F — buffer for any DLL reads past ghoul2

	// Track the last entity ghoul2 used for context in bolt/surface operations
	CGhoul2Info_v *mLastEntityGhoul2;

	CWraithStub() : mPadding(0), mTimeStart(0), mTimeEnd(0), mFlags(0), mLastEntityGhoul2(NULL) {
		memset(mPad2, 0, sizeof(mPad2));
		memset(mModelPad, 0, sizeof(mModelPad));
		memset(mExtraEnd, 0, sizeof(mExtraEnd));
	}

	void Destroy() {
		// Manual cleanup since we have no virtual destructor
		re.G2API_CleanGhoul2Models(mGhoul2);
	}

	CGhoul2Info_v *ResolveGhoul2Handle(int ghoul2Handle) {
		if (ghoul2Handle <= 0) {
			return NULL;
		}

		IGhoul2InfoArray &arr = re.TheGhoul2InfoArray();
		if (!arr.IsValid(ghoul2Handle)) {
			return NULL;
		}

		return reinterpret_cast<CGhoul2Info_v *>(&arr.Get(ghoul2Handle));
	}

	CGhoul2Info_v *ResolveModelOwner(int modelHandle, int *outIndex = NULL) {
		if (modelHandle <= 0) {
			return NULL;
		}

		const int idx = modelHandle - 1;
		if (outIndex) {
			*outIndex = idx;
		}

		if (mGhoul2.IsValid() && idx < mGhoul2.size()) {
			return &mGhoul2;
		}

		if (mLastEntityGhoul2 && mLastEntityGhoul2->IsValid() && idx < mLastEntityGhoul2->size()) {
			return mLastEntityGhoul2;
		}

		return NULL;
	}

	// Helper: resolve model handle to CGhoul2Info* within the wraith's own ghoul2
	CGhoul2Info *ResolveModel(int modelHandle) {
		int idx = 0;
		CGhoul2Info_v *owner = ResolveModelOwner(modelHandle, &idx);
		if (!owner) {
			return NULL;
		}
		return &(*owner)[idx];
	}

	// ===== vtable[0] (0x00): AddBoltToModel — 2 stack args =====
	// NOTE: CG_AddViewWeapon ALSO calls vtable[0] with local_d0[0]=0 (reType=RT_MODEL=0),
	// so ResolveModel(0) returns NULL → safe no-op. The game DLL calls this with real
	// modelHandle+boneName to add G2 bolts during entity initialization.
	// DO NOT change arg count — game calls with 2 args (ret 8).
	virtual int Slot00_AddBolt(int modelHandle, const char *boneName) {
		CGhoul2Info *ghl = ResolveModel(modelHandle);
		if (ghl) {
			return re.G2API_AddBolt(ghl, boneName);
		}
		return -1;
	}

	// ===== vtable[1] (0x04): SetBoltInfo — 3 stack args =====
	virtual int Slot01_SetBoltInfo(int a1, int a2, const char *a3) {
		return 0;
	}

	// ===== vtable[2] (0x08): unknown — 2 stack args (safe default) =====
	virtual int Slot02(int a1, int a2) { return 0; }

	// ===== vtable[3] (0x0C): GetBoltIndex — 2 stack args =====
	virtual int Slot03_GetBoltIndex(int a1, int a2) { return 0; }

	// ===== vtable[4] (0x10): RegisterAnimCallback — 6 stack args =====
	virtual int Slot04_RegisterAnimCallback(int a1, int a2, int a3, int a4, int a5, int a6) { return 0; }

	// ===== vtable[5] (0x14): FindSurfaceByCollision — 8 stack args =====
	virtual int Slot05_FindSurfaceByCollision(int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8) { return 0; }

	// ===== vtable[6] (0x18): CloneGhoul2 — 2 stack args =====
	virtual int Slot06_CloneGhoul2(int a1, int a2) { return 0; }

	// ===== vtable[7] (0x1C): unknown — 2 stack args (safe default) =====
	virtual int Slot07(int a1, int a2) { return 0; }

	// ===== vtable[8] (0x20): GetModelData — 0 stack args =====
	virtual void* GetModelData() {
		return (void*)this;
	}

	// ===== vtable[9] (0x24): DebugPrint — 2 stack args =====
	virtual int DebugPrint(const char *msg, int level) {
		fprintf(stderr, "[WRAITH] DebugPrint(level=%d): '%s'\n", level, msg ? msg : "(null)");
		return 0;
	}

	// ===== vtable[10] (0x28): unknown — 2 stack args (safe default) =====
	virtual int Slot10(int a1, int a2) { return 0; }

	// ===== vtable[11] (0x2C): unknown — 2 stack args (safe default) =====
	virtual int Slot11(int a1, int a2) { return 0; }

	// ===== vtable[12] (0x30): RemoveAnimCallback — 2 stack args =====
	virtual int Slot12_RemoveAnimCallback(int a1, int a2) { return 0; }

	// ===== vtable[13] (0x34): GetBoltMatrix_Single — 2 stack args =====
	virtual int Slot13_GetBoltMatrixSingle(int modelHandle, void *outMatrix) { return 0; }

	// ===== vtable[14] (0x38): GetBoltMatrix — 7 stack args =====
	virtual int Slot14_GetBoltMatrix(int modelHandle, int boltIndex, void *outMatrix,
	                                  const float *angles, const float *origin, int axisBase, int flags) {
		CGhoul2Info *ghl = ResolveModel(modelHandle);
		if (ghl && outMatrix) {
			// Use the wraith's ghoul2 vector for bolt matrix lookup
			if (mGhoul2.IsValid() && (modelHandle - 1) < mGhoul2.size()) {
				return re.G2API_GetBoltMatrix(mGhoul2, modelHandle - 1, boltIndex,
					(mdxaBone_t*)outMatrix, angles, origin, 0, NULL, vec3_origin);
			}
			if (mLastEntityGhoul2 && mLastEntityGhoul2->IsValid() && (modelHandle - 1) < mLastEntityGhoul2->size()) {
				return re.G2API_GetBoltMatrix(*mLastEntityGhoul2, modelHandle - 1, boltIndex,
					(mdxaBone_t*)outMatrix, angles, origin, 0, NULL, vec3_origin);
			}
		}
		return 0;
	}

	// ===== vtable[15] (0x3C): unknown — 2 stack args (safe default) =====
	virtual int Slot15(int a1, int a2) { return 0; }

	// ===== vtable[16] (0x40): GetModelName — 1 stack arg =====
	virtual const char* Slot16_GetModelName(int modelHandle) { return ""; }

	// ===== vtable[17]-[19]: unknown — 2 stack args each (safe default) =====
	virtual int Slot17(int a1, int a2) { return 0; }
	virtual int Slot18(int a1, int a2) { return 0; }
	virtual int Slot19(int a1, int a2) { return 0; }

	// ===== vtable[20] (0x50): GetSkinName — 2 stack args =====
	virtual int Slot20_GetSkinName(int a1, int a2) { return 0; }

	// ===== vtable[21] (0x54): GetSurfaceIndex — 2 stack args =====
	virtual int GetSurfaceIndex(int modelHandle, const char *surfaceName) {
		CGhoul2Info *ghl = ResolveModel(modelHandle);
		if (ghl && surfaceName && surfaceName[0]) {
			int ret = re.G2API_GetSurfaceIndex(ghl, surfaceName);
			static int surfaceLogCount = 0;
			if (surfaceLogCount < 24) {
				Com_Printf("[WRAITH21] model=%d surf='%s' ghl=%p ret=%d\n",
					modelHandle, surfaceName, (void *)ghl, ret);
				++surfaceLogCount;
			}
			return ret;
		}
		static int badSurfaceLogCount = 0;
		if (badSurfaceLogCount < 12) {
			Com_Printf("[WRAITH21] model=%d surf='%s' resolve failed ghl=%p\n",
				modelHandle, surfaceName ? surfaceName : "(null)", (void *)ghl);
			++badSurfaceLogCount;
		}
		return -1;
	}

	// ===== vtable[22] (0x58): GetSurfaceName — 2 stack args =====
	virtual const char* Slot22_GetSurfaceName(int modelHandle, int surfaceIndex) {
		CGhoul2Info *ghl = ResolveModel(modelHandle);
		if (ghl) {
			return re.G2API_GetSurfaceName(ghl, surfaceIndex);
		}
		return "";
	}

	// ===== vtable[23] (0x5C): InitGhoul2Model — 6 stack args =====
	virtual int InitGhoul2Model(CGhoul2Info_v *ghoul2, const char *fileName,
	                            int modelIndex, int customSkin, int customShader, int modelFlags) {
		const int beforeHandle = ghoul2 ? *(int *)ghoul2 : 0;
		fprintf(stderr, "[WRAITH] InitGhoul2Model(ghoul2=%p, '%s', idx=%d, skin=%d, shader=%d, flags=%d)\n",
			(void*)ghoul2, fileName ? fileName : "(null)", modelIndex, customSkin, customShader, modelFlags);
		if (!fileName || !fileName[0]) return 0;
		// Track entity ghoul2 for subsequent bolt/surface operations
		mLastEntityGhoul2 = ghoul2;
		int result = re.G2API_InitGhoul2Model(*ghoul2, fileName, modelIndex,
			(qhandle_t)customSkin, (qhandle_t)customShader, modelFlags, 0);
		fprintf(stderr, "[WRAITH] InitGhoul2Model -> G2API result=%d, handle=%d\n", result, result + 1);
		static int initLogCount = 0;
		if (initLogCount < 24) {
			Com_Printf("[WRAITH23] ghoul2=%p before=%d after=%d file='%s' result=%d ret=%d\n",
				(void *)ghoul2,
				beforeHandle,
				ghoul2 ? *(int *)ghoul2 : 0,
				fileName ? fileName : "(null)",
				result,
				result + 1);
			++initLogCount;
		}
		return result + 1;
	}

	// ===== vtable[24] (0x60): unknown — 2 stack args =====
	virtual int Slot24(int a1, int a2) {
		fprintf(stderr, "[WRAITH] Slot24(a1=%08X, a2=%08X)\n", a1, a2);
		return 0;
	}

	// ===== vtable[25] (0x64): CheckSurfaceVisible — 2 stack args =====
	virtual int Slot25_CheckSurfaceVisible(int a1, int a2) {
		fprintf(stderr, "[WRAITH] CheckSurfaceVisible(a1=%08X, a2=%08X)\n", a1, a2);
		return 1;
	}

	// ===== vtable[26] (0x68): GetBoltInfo — 2 stack args =====
	virtual int Slot26_GetBoltInfo(int a1, int a2) {
		fprintf(stderr, "[WRAITH] GetBoltInfo(a1=%08X, a2=%08X)\n", a1, a2);
		return 0;
	}

	// ===== vtable[27] (0x6C): unknown — 2 stack args =====
	virtual int Slot27(int a1, int a2) {
		fprintf(stderr, "[WRAITH] Slot27(a1=%08X, a2=%08X)\n", a1, a2);
		return 0;
	}

	// ===== vtable[28] (0x70): AddBoltByName — 2 stack args =====
	virtual int Slot28_AddBoltByName(int modelHandle, const char *boneName) {
		CGhoul2Info *ghl = ResolveModel(modelHandle);
		if (ghl) {
			return re.G2API_AddBolt(ghl, boneName);
		}
		return -1;
	}

	// ===== vtable[29] (0x74): SetAnimIndex — 9 stack args =====
	virtual int Slot29_SetAnimIndex(int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8, int a9) {
		fprintf(stderr, "[WRAITH] SetAnimIndex(%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X)\n",
			a1, a2, a3, a4, a5, a6, a7, a8, a9);
		return 0;
	}

	// ===== vtable[30] (0x78): SetBoneAnim — 9 stack args =====
	virtual int Slot30_SetBoneAnim(int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8, int a9) {
		fprintf(stderr, "[WRAITH] SetBoneAnim(%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X)\n",
			a1, a2, a3, a4, a5, a6, a7, a8, a9);
		return 0;
	}

	// ===== vtable[31] (0x7C): PrecacheModel — 1 stack arg, returns a renderer model handle =====
	virtual int PrecacheModel(const char *modelName) {
		int handle = 0;

		fprintf(stderr, "[WRAITH] PrecacheModel('%s')\n", modelName ? modelName : "(null)");
		if (modelName && modelName[0]) {
			handle = re.RegisterModel(modelName);
		}

		fprintf(stderr, "[WRAITH] PrecacheModel -> handle=%d\n", handle);
		return handle;
	}

	// ===== vtable[32] (0x80): unknown — 2 stack args =====
	virtual int Slot32(int a1, int a2) {
		fprintf(stderr, "[WRAITH] Slot32(a1=%08X, a2=%08X)\n", a1, a2);
		return 0;
	}

	// ===== vtable[33] (0x84): unknown — 2 stack args =====
	virtual int Slot33(int a1, int a2) {
		fprintf(stderr, "[WRAITH] Slot33(a1=%08X, a2=%08X)\n", a1, a2);
		return 0;
	}

	// ===== vtable[34] (0x88): unknown — 2 stack args =====
	virtual int Slot34(int a1, int a2) {
		fprintf(stderr, "[WRAITH] Slot34(a1=%08X, a2=%08X)\n", a1, a2);
		return 0;
	}

	// ===== vtable[35] (0x8C): RemoveBolt — 2 stack args =====
	virtual int Slot35_RemoveBolt(int modelHandle, int boltIndex) {
		CGhoul2Info *ghl = ResolveModel(modelHandle);
		if (ghl) {
			int ret = re.G2API_RemoveBolt(ghl, boltIndex);
			static int removeBoltLogCount = 0;
			if (removeBoltLogCount < 24) {
				Com_Printf("[WRAITH35] model=%d bolt=%d ghl=%p ret=%d\n",
					modelHandle, boltIndex, (void *)ghl, ret);
				++removeBoltLogCount;
			}
			return ret;
		}
		static int removeBoltMissLogCount = 0;
		if (removeBoltMissLogCount < 12) {
			Com_Printf("[WRAITH35] model=%d bolt=%d resolve failed\n", modelHandle, boltIndex);
			++removeBoltMissLogCount;
		}
		return 0;
	}

	// ===== vtable[36] (0x90): RemoveGhoul2Model — 1 stack arg =====
	virtual int Slot36_RemoveGhoul2Model(int modelIndex) {
		if (mGhoul2.IsValid() && modelIndex > 0 && (modelIndex - 1) < mGhoul2.size()) {
			return re.G2API_RemoveGhoul2Model(mGhoul2, modelIndex - 1);
		}
		if (mLastEntityGhoul2 && mLastEntityGhoul2->IsValid() &&
			modelIndex > 0 && (modelIndex - 1) < mLastEntityGhoul2->size()) {
			return re.G2API_RemoveGhoul2Model(*mLastEntityGhoul2, modelIndex - 1);
		}
		return 0;
	}

	// ===== vtable[37] (0x94): FinalizeEntity — 1 stack arg =====
	virtual void FinalizeEntity(void *entityData) {
		int entityHandle = 0;

		fprintf(stderr, "[WRAITH] FinalizeEntity(entityData=%p)\n", entityData);
		if ( !entityData ) {
			return;
		}

		if ( mLastEntityGhoul2 ) {
			entityHandle = *(int *)mLastEntityGhoul2;
		}

		memcpy( entityData, this, SOF2_WRAITH_ENTITY_BYTES );
		if ( entityHandle > 0 ) {
			*(int *)((byte *)entityData + SOF2_WRAITH_GHOUL2_OFFSET) = entityHandle;
		}
		fprintf(stderr, "[WRAITH] FinalizeEntity -> handle=%d vtable=%p\n",
			entityHandle, *(void **)entityData);
	}

	// ===== vtable[38] (0x98): SetupModel — 1 stack arg =====
	virtual void SetupModel(int modelHandle) {
		mPadding = modelHandle;
		fprintf(stderr, "[WRAITH] SetupModel(modelHandle=%08X)\n", modelHandle);
	}

	// ===== vtable[39] (0x9C): RemoveCallback — 2 stack args =====
	virtual int Slot39_RemoveCallback(int a1, int a2) {
		fprintf(stderr, "[WRAITH] RemoveCallback(a1=%08X, a2=%08X)\n", a1, a2);
		return 0;
	}

	// ===== vtable[40] (0xA0): GetCachedModel — 2 stack args =====
	virtual int Slot40_GetCachedModel(int a1, int a2) {
		fprintf(stderr, "[WRAITH] GetCachedModel(a1=%08X, a2=%08X)\n", a1, a2);
		return 0;
	}

	// ===== vtable[41] (0xA4): unknown — 2 stack args =====
	virtual int Slot41(int a1, int a2) { return 0; }

	// ===== vtable[42] (0xA8): SetSkin — 2 stack args =====
	virtual int Slot42_SetSkin(int modelHandle, int skinHandle) {
		CGhoul2Info *ghl = ResolveModel(modelHandle);
		if (ghl) {
			return re.G2API_SetSkin(ghl, (qhandle_t)skinHandle, (qhandle_t)skinHandle);
		}
		return 0;
	}

	// ===== vtable[43] (0xAC): SetSurfaceFlags — 3 stack args =====
	virtual int Slot43_SetSurfaceFlags(int modelHandle, int surfaceIndex, int flags) {
		CGhoul2Info *ghl = ResolveModel(modelHandle);
		if (!ghl || surfaceIndex < 0) {
			static int surfaceFlagMissLogCount = 0;
			if (surfaceFlagMissLogCount < 12) {
				Com_Printf("[WRAITH43] model=%d surfIdx=%d flags=0x%x resolve failed ghl=%p\n",
					modelHandle, surfaceIndex, flags, (void *)ghl);
				++surfaceFlagMissLogCount;
			}
			return 0;
		}

		const char *surfaceName = re.G2API_GetSurfaceName(ghl, surfaceIndex);
		if (!surfaceName || !surfaceName[0]) {
			static int surfaceNameMissLogCount = 0;
			if (surfaceNameMissLogCount < 12) {
				Com_Printf("[WRAITH43] model=%d surfIdx=%d flags=0x%x empty name\n",
					modelHandle, surfaceIndex, flags);
				++surfaceNameMissLogCount;
			}
			return 0;
		}

		int ret = re.G2API_SetSurfaceOnOff(ghl, surfaceName, flags);
		static int surfaceFlagLogCount = 0;
		if (surfaceFlagLogCount < 24) {
			Com_Printf("[WRAITH43] model=%d surfIdx=%d name='%s' flags=0x%x ret=%d\n",
				modelHandle, surfaceIndex, surfaceName, flags, ret);
			++surfaceFlagLogCount;
		}
		return ret;
	}

	// ===== vtable[44] (0xB0): AttachModel — 3 stack args =====
	virtual int Slot44_AttachModel(int parentBolt, int modelHandle, int attachType) {
		(void)parentBolt;
		(void)modelHandle;
		(void)attachType;
		return 0;
	}

	// ===== vtable[45] (0xB4): StopBoneAnim — 2 stack args =====
	virtual int Slot45_StopBoneAnim(int modelHandle, const char *boneName) {
		CGhoul2Info *ghl = ResolveModel(modelHandle);
		if (ghl) {
			return re.G2API_StopBoneAnim(ghl, boneName);
		}
		return 0;
	}

	// ===== vtable[46] (0xB8): StopBoneAnimByName — 2 stack args =====
	virtual int Slot46_StopBoneAnimByName(int a1, int a2) { return 0; }

	// ===== vtable[47] (0xBC): unknown — 2 stack args =====
	virtual int Slot47(int a1, int a2) { return 0; }

	// ===== vtable[48] (0xC0): InitWeaponData — 1 stack arg (WpnData singleton ptr) =====
	virtual int Slot48_InitWeaponData(void *wpnDataSingleton) { return 0; }

	// ===== vtable[49] (0xC4): InitNavigation — 1 stack arg (Navigator ptr) =====
	virtual int Slot49_InitNavigation(void *navigatorPtr) { return 0; }

	// ===== vtable[50] (0xC8): GetCollisionResult — 4 stack args =====
	virtual int Slot50_GetCollisionResult(int a1, int a2, int a3, int a4) { return 0; }

	// ===== vtable[51] (0xCC): retail weapon path uses this to derive a secondary/buffer model handle.
	// Returning 0 causes CWpnGameModel_SetMuzzleBolt to print "NULL_WraithID for buffer" and skip bolt setup.
	virtual int Slot51_CopyBoltToSurface(int modelHandle, const char *primaryName, const char *secondaryName) {
		static int slot51LogCount = 0;
		if (slot51LogCount < 24) {
			Com_Printf("[WRAITH51] model=%d primary='%s' secondary='%s'\n",
				modelHandle,
				primaryName ? primaryName : "(null)",
				secondaryName ? secondaryName : "(null)");
			++slot51LogCount;
		}

		// Conservative compatibility fallback: reuse the live weapon model handle when retail expects
		// a secondary/buffer Wraith ID. This restores later AddBolt/SetSkin calls onto a valid model.
		if (ResolveModel(modelHandle)) {
			return modelHandle;
		}

		return 0;
	}

	// ===== vtable[52] (0xD0): cgame calls this as a no-arg thiscall during DrawInformation.
	virtual int Slot52_SetModelTransform(void) { return 0; }

	// ===== vtable[53] (0xD4): AnimCallback — 2 stack args =====
	virtual int Slot53_AnimCallback(int a1, int a2) { return 0; }

	// ===== vtable[54] (0xD8): unknown — 2 stack args =====
	// Actual retail use: CG_Entity_AttachBolt calls this slot with 8 stack args.
	virtual int Slot54_AttachBolt( int ghoul2Handle, int modelIndex, int boltIndex, float *outPos,
		const float *angles, const float *origin, void * /*modelList*/, const float * /*scale*/ ) {
		static int slot54LogCount = 0;
		mdxaBone_t matrix;
		vec3_t safeAngles = { 0.0f, 0.0f, 0.0f };
		vec3_t safeOrigin = { 0.0f, 0.0f, 0.0f };
		vec3_t scaleVec = { 1.0f, 1.0f, 1.0f };

		if ( slot54LogCount < 12 ) {
			Com_Printf(
				"[WRAITH54] ghoul2=%d model=%d bolt=%d out=%p angles=%p origin=%p\n",
				ghoul2Handle,
				modelIndex,
				boltIndex,
				outPos,
				angles,
				origin );
			++slot54LogCount;
		}

		if ( !outPos ) {
			return 0;
		}

		if ( angles ) {
			VectorCopy( angles, safeAngles );
		}
		if ( origin ) {
			VectorCopy( origin, safeOrigin );
			VectorCopy( origin, outPos );
		} else {
			VectorClear( outPos );
		}

		if ( ghoul2Handle > 0 ) {
			IGhoul2InfoArray &arr = re.TheGhoul2InfoArray();
			if ( arr.IsValid( ghoul2Handle ) ) {
				CGhoul2Info_v &g2 = *(CGhoul2Info_v *)&ghoul2Handle;
				if ( re.G2API_GetBoltMatrix( g2, modelIndex, boltIndex, &matrix,
						safeAngles, safeOrigin, 0, NULL, scaleVec ) ) {
					outPos[0] = matrix.matrix[0][3];
					outPos[1] = matrix.matrix[1][3];
					outPos[2] = matrix.matrix[2][3];
					return 1;
				}
			}
		}

		return 0;
	}

	// ===== vtable[55] (0xDC): GetBoltMatrixBlend — 4 stack args =====
	virtual int Slot55_GetBoltMatrixBlend(int a1, int a2, int a3, int a4) { return 0; }

	// ===== vtable[56] (0xE0): LoadAnimData — 2 stack args =====
	virtual int Slot56_LoadAnimData(int a1, int a2) { return 0; }

	// ===== vtable[57] (0xE4): Shutdown — 0 stack args =====
	virtual int Slot57_Shutdown() { return 0; }

	// ===== vtable[58]-[59]: unknown — 2 stack args each =====
	virtual int Slot58(int a1, int a2) { return 0; }
	virtual int Slot59(int a1, int a2) { return 0; }

	// ===== vtable[60] (0xF0): GetAnimIndex — 2 stack args =====
	virtual int Slot60_GetAnimIndex(int a1, int a2) { return 0; }

	// ===== vtable[61] (0xF4): GetAnimEventString — 2 stack args =====
	// MUST return "" not NULL — caller does strlen on result
	virtual const char* Slot61_GetAnimEventString(int animHandle, const char *eventName) { return ""; }

	// ===== vtable[62] (0xF8): GetAnimFrameIndex — 2 stack args =====
	virtual int Slot62_GetAnimFrameIndex(int a1, int a2) { return 0; }

	// Helper: convert SOF2 refEntity_t layout → OpenJK refEntity_t → re.AddRefEntityToScene
	// SOF2 refEntity_t (from Ghidra CWeaponSystem_Update): axis@+0x0C, origin@+0x34, hModel@+0x6C
	void SubmitSOF2RefEntityToScene(void *sof2Ent, const char *dbgSlot) {
		if (!sof2Ent) return;
		const unsigned char *raw = (const unsigned char *)sof2Ent;
		const int   sof2ReType   = *(const int  *)(raw + 0x00);
		const int   sof2Renderfx = *(const int  *)(raw + 0x04);
		const float *sof2Axis    = (const float *)(raw + 0x0C);
		const float *sof2Origin  = (const float *)(raw + 0x34);
		const int   sof2hModel   = *(const int  *)(raw + 0x6C);

		static int submitLogCount = 0;
		if (submitLogCount < 12) {
			Com_Printf("[WRAITH%s] reType=%d hModel=%d renderfx=0x%x origin=(%.1f,%.1f,%.1f)\n",
				dbgSlot, sof2ReType, sof2hModel, sof2Renderfx,
				sof2Origin[0], sof2Origin[1], sof2Origin[2]);
			submitLogCount++;
		}

		if (!sof2hModel) return;

		refEntity_t ent;
		memset(&ent, 0, sizeof(ent));
		ent.reType   = (refEntityType_t)sof2ReType;
		ent.renderfx = sof2Renderfx;
		ent.hModel   = (qhandle_t)sof2hModel;
		VectorCopy(sof2Origin, ent.origin);
		VectorCopy(sof2Origin, ent.lightingOrigin);
		VectorCopy(sof2Origin, ent.oldorigin);
		VectorCopy(sof2Axis + 0, ent.axis[0]);
		VectorCopy(sof2Axis + 3, ent.axis[1]);
		VectorCopy(sof2Axis + 6, ent.axis[2]);
		ent.shaderRGBA[0] = ent.shaderRGBA[1] = ent.shaderRGBA[2] = ent.shaderRGBA[3] = 255;
		ent.modelScale[0] = ent.modelScale[1] = ent.modelScale[2] = 1.0f;
		re.AddRefEntityToScene(&ent);
	}

	// ===== vtable[63] (0xFC): SubmitRefEntity — called from CWeaponSystem_Update (1 stack arg: sof2 refent ptr) =====
	// Assembly-confirmed: PUSH ESI; CALL [EDX+0xfc] → exactly 1 stack arg = refent ptr.
	// vtable[0] (AddBolt) is NOT this — game calls vtable[0] with 2 args (modelHandle, boneName).
	virtual void Slot63_SubmitRefEntity(void *sof2Ent) {
		SubmitSOF2RefEntityToScene(sof2Ent, "63");
	}

	// ===== vtable[64] (0x100): GetBonePosition — 3 stack args =====
	virtual int Slot64_GetBonePosition(int a1, int a2, int a3) { return 0; }

	// ===== vtable[65] (0x104): retail constructors use this to resolve an entity ghoul2 handle
	// back to the active owner-model Wraith ID. Returning 0 breaks later AddBolt/attachment setup.
	virtual int Slot65_CopyGhoul2Instance(int ghoul2Handle, int modelIndex) {
		CGhoul2Info_v *ghoul2 = ResolveGhoul2Handle(ghoul2Handle);
		if (!ghoul2 || !ghoul2->IsValid() || ghoul2->size() <= 0) {
			static int slot65MissLogCount = 0;
			if (slot65MissLogCount < 24) {
				Com_Printf("[WRAITH65] ghoul2=%d model=%d resolve failed\n", ghoul2Handle, modelIndex);
				++slot65MissLogCount;
			}
			return 0;
		}

		mLastEntityGhoul2 = ghoul2;

		int resolvedIndex = modelIndex;
		if (resolvedIndex < 0) {
			resolvedIndex = 0;
		}
		if (resolvedIndex >= ghoul2->size()) {
			resolvedIndex = 0;
		}

		static int slot65LogCount = 0;
		if (slot65LogCount < 24) {
			Com_Printf("[WRAITH65] ghoul2=%d size=%d requested=%d resolved=%d ret=%d\n",
				ghoul2Handle, ghoul2->size(), modelIndex, resolvedIndex, resolvedIndex + 1);
			++slot65LogCount;
		}

		return resolvedIndex + 1;
	}

	// ===== vtable[66] (0x108): SetAnimBlend — 7 stack args =====
	virtual int Slot66_SetAnimBlend(int a1, int a2, int a3, int a4, int a5, int a6, int a7) { return 0; }

	// ===== vtable[67] (0x10C): SetBoneAnimByName — 9 stack args =====
	virtual int Slot67_SetBoneAnimByName(int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8, int a9) { return 0; }

	// ===== vtable[68] (0x110): SetBoneAnimByIndex — 10 stack args =====
	virtual int Slot68_SetBoneAnimByIndex(int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8, int a9, int a10) { return 0; }

	// ===== vtable[69] (0x114): GetBoneName — 3 stack args =====
	virtual int Slot69_GetBoneName(int a1, int a2, int a3) { return 0; }

	// ===== vtable[70] (0x118): FindBoneByName — 2 stack args =====
	virtual int Slot70_FindBoneByName(int a1, int a2) { return 0; }

	// ===== vtable[71] (0x11C): GetNumAnimFrames — 1 stack arg =====
	virtual int Slot71_GetNumAnimFrames(int a1) { return 0; }

	// ===== vtable[72] (0x120): unknown — 2 stack args =====
	virtual int Slot72(int a1, int a2) { return 0; }

	// ===== vtable[73] (0x124): SetBoneIndex — 2 stack args =====
	virtual int Slot73_SetBoneIndex(int a1, int a2) { return 0; }

	// ===== vtable[74] (0x128): CopyBolt — 2 stack args =====
	virtual int Slot74_CopyBolt(int a1, int a2) { return 0; }

	// ===== vtable[75]-[76]: unknown — 2 stack args each =====
	virtual int Slot75(int a1, int a2) { return 0; }
	virtual int Slot76(int a1, int a2) { return 0; }

	// ===== vtable[77] (0x134): ClearBoneAnimByIndex — 2 stack args =====
	virtual int Slot77_ClearBoneAnimByIndex(int a1, int a2) { return 0; }
};

static CWraithStub *g_wraithStub = NULL;

// Accessible from cl_cgame.cpp — cgame DLL needs the same CWraithStub
void *SV_GetWraithStubPtr( void ) {
	if ( !g_wraithStub ) {
		g_wraithStub = new CWraithStub();
	}
	return g_wraithStub;
}

static qboolean SV_G2_InitWraithSurfaceMap( void **outPtr ) {
	fprintf(stderr, "[GI] gi_G2_InitWraithSurfaceMap called outPtr=%p\n", (void*)outPtr);
	if (!g_wraithStub) {
		g_wraithStub = new CWraithStub();
	}
	if (outPtr) *outPtr = g_wraithStub;
	fprintf(stderr, "[GI] gi_G2_InitWraithSurfaceMap: wraith=%p vtable=%p\n",
		(void*)g_wraithStub, *(void**)g_wraithStub);
	return g_wraithStub ? qtrue : qfalse;
}

// gi[89]: SV_ICARUS_Init — ICARUS scripting init
static void SV_ICARUS_Init_Stub( void ) {}

// gi[88]: Com/RegisterSkin in SOF2 returns a skin descriptor object, not a qhandle_t.
static void *SV_Com_RegisterSkin_Wrapper( const char *name ) {
	return (void *)SV_GetSOF2SkinStub( name );
}

// gi[91]: G2API_AnimateGhoul2Models (wrapper already exists as SV_G2API_AnimateG2Models)

// gi[92]: G2API_CleanGhoul2Models (wrapper already exists as SV_G2API_CleanGhoul2Models)

// gi[91]: G2_GetGhoul2InfoByHandle — returns std::vector<CGhoul2Info>* for the given handle
// The gamex86.dll stores a Ghoul2 handle (int mItem) in each entity at +0xec.
// CEntity_Destructor and many other functions call this to get the vector of G2 models
// and iterate over them.  Returning NULL causes a crash (dereference of result+4).
static void *SV_G2_GetGhoul2InfoByHandle( int handle ) {
	if ( handle <= 0 ) return NULL;
	IGhoul2InfoArray &arr = re.TheGhoul2InfoArray();
	if ( !arr.IsValid( handle ) ) return NULL;
	return &arr.Get( handle );
}

// gi[94]: SP_GetStringTextByIndex
static const char *SV_SP_GetStringTextByIndex_Wrapper( int index ) {
	(void)index;
	return "";
}

// gi[95]: SE_GetString
static const char *SV_SE_GetString_Wrapper( const char *token ) {
	return SE_GetString( token );
}

// gi[96]: SE_GetStringIndex
static int SV_SE_GetStringIndex_Stub( const char * /*token*/ ) { return 0; }

// gi[97]: SE_SetString
static void SV_SE_SetString_Stub( const char * /*token*/, const char * /*value*/ ) {}

// gi[98]: SV_GetEntityWavVol
static int SV_GetEntityWavVol_Stub( int /*entNum*/ ) { return 127; }

// gi[99]: SV_SoundClearLooping
static void SV_SoundClearLooping_Stub( void ) {
#ifndef DEDICATED
	S_ClearLoopingSounds();
#endif
}

// gi[100]: SV_GetSoundDuration
static int SV_GetSoundDuration_Stub( int sfxHandle ) {
#ifndef DEDICATED
	return (int)S_GetSampleLengthInMilliSeconds( (sfxHandle_t)sfxHandle );
#else
	return 0;
#endif
}

// gi[101]: SV_SoundStopAll
static void SV_SoundStopAll_Stub( void ) {
#ifndef DEDICATED
	S_StopAllSounds();
#endif
}

// gi[102-103]: Terrain init/shutdown
static void SV_InitTerrain_Stub( void ) {}
static void SV_ShutdownTerrain_Stub( void ) {}

// gi[104-112]: RMG minimap drawing stubs
static void SV_RMG_MinimapStub( void ) {}
static void SV_RMG_MinimapStub2( void ) {}

/*
===============
SV_InitGameProgs

Init the game subsystem for a new map
===============
*/
void SV_InitGameProgs (void) {
	// SOF2 game_import_t is a flat array of 113 function pointers (0x71 dwords).
	// The DLL's GetGameAPI copies these by position, NOT by named struct fields.
	// Definitive mapping from SoF2.exe SV_InitGameProgs @ 0x10014c60.
	void *gi[113];
	int   i;
	memset( gi, 0, sizeof(gi) );

	// unload anything we have now
	if ( ge ) {
		SV_ShutdownGameProgs (qtrue);
	}

	//
	// Populate SOF2 v8 game_import_t (113 entries).
	// Mapping from SoF2.exe pseudocode DECLARATION ORDER (server.c lines 2504-2616)
	// NOT the assignment order (which Ghidra reordered). XRef-validated against gamex86.dll.
	//
	gi[  0] = (void *)SV_Traced_Printf;              // Printf (traced)
	gi[  1] = (void *)SV_Traced_DPrintf;             // DPrintf (traced)
	gi[  2] = (void *)SV_Traced_DPrintf;             // DPrintf2 (traced)
	gi[  3] = (void *)SV_Traced_ComSprintf;           // Com_sprintf (traced)
	gi[  4] = (void *)SV_Traced_Error;               // Com_Error (traced)
	gi[  5] = (void *)SV_Traced_FS_FOpenFile;        // FS_FOpenFileByMode (traced)
	gi[  6] = (void *)SV_Traced_FS_Read;             // FS_Read (traced)
	gi[  7] = (void *)FS_Write;                      // FS_Write
	gi[  8] = (void *)SV_Traced_FS_FCloseFile;       // FS_FCloseFile (traced)
	gi[  9] = (void *)SV_Traced_FS_ReadFile;         // FS_ReadFile (traced)
	gi[ 10] = (void *)SV_Traced_FS_FreeFile;         // FS_FreeFile (traced)
	gi[ 11] = (void *)FS_FileExists;                 // FS_FileExists
	gi[ 12] = (void *)SV_SOF2_FS_ListFiles;          // FS_ListFiles (SOF2 4-param, SOF2-format return)
	gi[ 13] = (void *)SV_SOF2_FS_FreeFileList;      // FS_FreeFileList (SOF2-format)
	gi[ 14] = (void *)SV_FS_CleanPath_Stub;          // FS_CleanPath (SOF2-specific)
	gi[ 15] = (void *)SV_Traced_EventLoop;           // Com_EventLoop (traced)
	gi[ 16] = (void *)SV_Traced_Cmd_TokenizeString;  // Cmd_TokenizeString (traced)
	gi[ 17] = (void *)COM_Parse;                     // COM_Parse
	gi[ 18] = (void *)SV_Traced_Cbuf_AddText;         // Cbuf_AddText (traced, blocks corrupted cmds)
	gi[ 19] = (void *)SV_Traced_Cbuf_ExecuteText;     // Cbuf_ExecuteText (traced)
	gi[ 20] = (void *)Cmd_Argc;                      // Cmd_Argc
	gi[ 21] = (void *)Cmd_ArgvBuffer;                // Cmd_ArgvBuffer
	gi[ 22] = (void *)Cmd_ArgsBuffer;                // Cmd_ArgsBuffer
	gi[ 23] = (void *)SV_Traced_Cvar_Get;            // Cvar_GetModified (traced)
	gi[ 24] = (void *)SV_Traced_Cvar_Register;       // Cvar_Register (traced)
	gi[ 25] = (void *)SV_Traced_Cvar_Update;         // Cvar_Update (traced)
	gi[ 26] = (void *)SV_Traced_Cvar_Set;            // Cvar_Set (traced)
	gi[ 27] = (void *)SV_Cvar_SetModified;           // Cvar_SetModified (4-arg variant)
	gi[ 28] = (void *)SV_Cvar_SetValue_Wrapper;      // Cvar_SetValue (name, float, force)
	gi[ 29] = (void *)SV_Traced_Cvar_VariableIntegerValue; // Cvar_VariableIntegerValue (traced)
	gi[ 30] = (void *)Cvar_VariableValue;            // Cvar_VariableValue (returns float)
	gi[ 31] = (void *)SV_Traced_Cvar_VariableStringBuffer; // Cvar_VariableStringBuffer (traced)
	gi[ 32] = (void *)SV_Traced_ZMalloc;             // Z_Malloc (traced)
	gi[ 33] = (void *)SV_Traced_ZFree;              // Z_Free (traced)
	gi[ 34] = (void *)SV_Traced_Z_CheckHeap;         // Z_CheckHeap (traced)
	gi[ 35] = (void *)SV_CM_RegisterTerrain_Stub;    // CM_RegisterTerrain
	gi[ 36] = (void *)SV_CM_Terrain_Release_Stub;    // CM_Terrain_Release
	gi[ 37] = (void *)SV_CM_Terrain_ForEachBrush_Stub; // CM_Terrain_ForEachBrush
	gi[ 38] = (void *)SV_CM_Terrain_GetWorldHeight_Stub; // CM_Terrain_GetWorldHeight
	gi[ 39] = (void *)SV_CM_Terrain_FlattenHeight_Stub;  // CM_Terrain_FlattenHeight
	gi[ 40] = (void *)SV_RMG_EvaluatePath_Stub;      // RMG_EvaluatePath
	gi[ 41] = (void *)SV_RMG_CreatePathSegment_Stub;  // RMG_CreatePathSegment
	gi[ 42] = (void *)SV_CM_Terrain_ApplyBezierPath_Stub; // CM_Terrain_ApplyBezierPath
	gi[ 43] = (void *)SV_CTerrainInstanceList_Add_Stub;   // CTerrainInstanceList_Add
	gi[ 44] = (void *)SV_CM_Terrain_GetFlattenedAvgHeight_Stub; // CM_Terrain_GetFlattenedAvgHeight
	gi[ 45] = (void *)SV_CM_Terrain_CheckOverlap_Stub; // CM_Terrain_CheckOverlap
	gi[ 46] = (void *)SV_CTerrainInstanceList_GetFirst_Stub; // CTerrainInstanceList_GetFirst
	gi[ 47] = (void *)SV_CTerrainInstanceList_GetNext_Stub;  // CTerrainInstanceList_GetNext
	gi[ 48] = (void *)SV_R_LoadDataImage_Stub;        // R_LoadDataImage
	gi[ 49] = (void *)SV_ResampleTexture_Stub;        // ResampleTexture
	gi[ 50] = (void *)SV_CM_PointContents_Wrapper;    // CM_PointContents (2 args)
	gi[ 51] = (void *)SV_PointContentsCached;          // SV_PointContentsCached (3 args)
	gi[ 52] = (void *)SV_SaveGame_WriteChunk;          // SV_WriteChunk
	gi[ 53] = (void *)SV_SaveGame_ReadChunk;           // SV_ReadChunk
	gi[ 54] = (void *)SV_Traced_LocateGameData;       // SV_LocateGameData (traced, 5 args)
	gi[ 55] = (void *)SV_GameDropClient;               // SV_GameDropClient
	gi[ 56] = (void *)SV_GameSendServerCommand;        // SV_GameSendServerCommand
	gi[ 57] = (void *)SV_Traced_LinkEntity;           // SV_LinkEntity (traced)
	gi[ 58] = (void *)SV_UnlinkEntityAll;              // SV_UnlinkEntityAll
	gi[ 59] = (void *)SV_AddTempEnt_Stub;              // SV_AddTempEnt
	gi[ 60] = (void *)SV_ClearTempEnts_Stub;           // SV_ClearTempEnts (decl order fix)
	gi[ 61] = (void *)SV_CM_GetDebugEntryPrevCount_Stub; // CM_GetDebugEntryPrevCount (decl order fix)
	gi[ 62] = (void *)SV_AddNextMap_Stub;              // SV_AddNextMap (decl order fix)
	gi[ 63] = (void *)SV_RemoveNextMap_Stub;           // SV_RemoveNextMap (decl order fix)
	gi[ 64] = (void *)SV_GetNextMap_Stub;              // SV_GetNextMap (decl order fix)
	gi[ 65] = (void *)SV_GP_GetBaseParseGroup_Stub;    // GP_GetBaseParseGroup (decl order fix)
	gi[ 66] = (void *)SV_GP_SetSubGroups_Stub;         // GP_SetSubGroups (decl order fix)
	gi[ 67] = (void *)SV_GameStub_ReturnZero;          // SV_GameStub_ReturnZero (decl order fix)
	gi[ 68] = (void *)SV_CSkin_Noop;                   // CSkin_Noop (decl order fix)
	gi[ 69] = (void *)SV_SOF2_AreaEntities;             // SV_AreaEntities (wrapper: returns entity indices, not pointers)
	gi[ 70] = (void *)SV_EntityContact;                // SV_EntityContact
	gi[ 71] = (void *)SV_Trace_10args;                 // SV_Trace (10 args: SOF2 adds eG2TraceType, useLod, traceFlags)
	gi[ 72] = (void *)SV_R_LightForPoint_Stub;         // SV_R_LightForPoint
	gi[ 73] = (void *)SV_SetBrushModel;                // SV_SetBrushModel
	gi[ 74] = (void *)SV_CM_SelectSubBSP_Stub;         // CM_SelectSubBSP
	gi[ 75] = (void *)SV_inPVS;                        // SV_inPVS
	gi[ 76] = (void *)SV_CM_FindOrCreateShader_Stub;   // CM_FindOrCreateShader
	gi[ 77] = (void *)SV_Traced_SetConfigstring;      // SV_SetConfigstring (traced)
	gi[ 78] = (void *)SV_GetConfigstring;              // SV_GetConfigstring
	gi[ 79] = (void *)SV_Traced_GetServerinfo;        // SV_GetServerinfo (traced)
	gi[ 80] = (void *)SV_AdjustAreaPortalState;        // SV_AdjustAreaPortalState
	gi[ 81] = (void *)SV_CM_GetModelBrushShaderNum_Stub; // CM_GetModelBrushShaderNum
	gi[ 82] = (void *)SV_CM_GetShaderName_Stub;        // CM_GetShaderName
	gi[ 83] = (void *)SV_CM_DamageBrushSideHealth_Stub; // CM_DamageBrushSideHealth
	gi[ 84] = (void *)SV_GetUsercmd_SOF2;              // SV_GetUsercmd
	// Slots 85-97: CORRECTED per declaration order (pseudocode lines 2589-2601)
	gi[ 85] = (void *)SV_Traced_RE_RegisterModel;      // RE_RegisterModel (traced)
	gi[ 86] = (void *)SV_Traced_RE_RegisterShader;     // RE_RegisterShader (traced)
	gi[ 87] = (void *)SV_G2_InitWraithSurfaceMap;      // G2_InitWraithSurfaceMap
	gi[ 88] = (void *)SV_Com_RegisterSkin_Wrapper;     // Com_RegisterSkin
	gi[ 89] = (void *)SV_G2API_AnimateG2Models;        // G2API_AnimateGhoul2Models
	gi[ 90] = (void *)SV_G2API_CleanGhoul2Models_Safe;  // G2API_CleanGhoul2Models (validate handle)
	gi[ 91] = (void *)SV_G2_GetGhoul2InfoByHandle; // G2_GetGhoul2InfoByHandle
	gi[ 92] = (void *)SV_SP_GetStringTextByIndex_Wrapper;  // SP_GetStringTextByIndex
	gi[ 93] = (void *)SV_Traced_SE_GetString;           // SE_GetString (traced)
	gi[ 94] = (void *)SV_SE_GetStringIndex_Stub;        // SE_GetStringIndex
	gi[ 95] = (void *)SV_SE_SetString_Stub;             // SE_SetString
	gi[ 96] = (void *)SV_Traced_GetEntityToken;         // SV_GetEntityToken (traced, was at 85)
	gi[ 97] = (void *)SV_Traced_ICARUS_Init;            // SV_ICARUS_Init (traced, was at 89)
	gi[ 98] = (void *)SV_GetEntityWavVol_Stub;          // SV_GetEntityWavVol
	gi[ 99] = (void *)SV_SoundClearLooping_Stub;        // SV_SoundClearLooping
	gi[100] = (void *)SV_GetSoundDuration_Stub;         // SV_GetSoundDuration
	gi[101] = (void *)SV_SoundStopAll_Stub;             // SV_SoundStopAll
	gi[102] = (void *)SV_InitTerrain_Stub;              // SV_InitTerrain
	gi[103] = (void *)SV_ShutdownTerrain_Stub;          // SV_ShutdownTerrain
	gi[104] = (void *)SV_RMG_MinimapStub;               // RMG_DrawMinimapBuildingIcon
	gi[105] = (void *)SV_RMG_MinimapStub;               // SV_RMG_DrawMinimapStartIcon
	gi[106] = (void *)SV_RMG_MinimapStub;               // RMG_DrawMinimapEndIcon
	gi[107] = (void *)SV_RMG_MinimapStub;               // RMG_DrawMinimapObjectiveIcon
	gi[108] = (void *)SV_RMG_MinimapStub;               // RMG_DrawMinimapDot
	gi[109] = (void *)SV_RMG_MinimapStub;               // Automap_DrawOverlay
	gi[110] = (void *)SV_RMG_MinimapStub;               // RMG_DrawMinimapPlayerMarker
	gi[111] = (void *)SV_RMG_MinimapStub2;              // RMG_RenderMinimap
	gi[112] = (void *)SV_RMG_MinimapStub2;              // RMG_SaveMinimap

	// SOF2 SP game DLL name
	const char *gamename = "game";  // Sys_LoadSPGameDll appends ARCH_STRING("x86") + DLL_EXT → "gamex86.dll"

	fprintf(stderr, "[DBG] SV_InitGameProgs: loading game DLL '%s'\n", gamename);
	GetGameAPIProc *GetGameAPI;
	gameLibrary = Sys_LoadSPGameDll( gamename, &GetGameAPI );
	if ( !gameLibrary )
		Com_Error( ERR_DROP, "Failed to load %s library", gamename );
	fprintf(stderr, "[DBG] SV_InitGameProgs: DLL loaded, calling GetGameAPI(8, gi)\n");

	// SOF2 GetGameAPI(int apiVersion, void *imports) — pass version 8 and import table
	ge = (game_export_t *)GetGameAPI( 8, gi );
	if (!ge)
	{
		Sys_UnloadDll( gameLibrary );
		Com_Error( ERR_DROP, "Failed to load %s library (API version mismatch?)", gamename );
	}
	fprintf(stderr, "[DBG] SV_InitGameProgs: GetGameAPI returned ge=%p\n", (void*)ge);

	// SOF2 game_export_t has NO apiversion field — skip the version check.
	// SOF2 cgame is a SEPARATE DLL (cgamex86.dll) loaded later by CL_InitCGame().

	sv.entityParsePoint = CM_EntityString();

	// Start a parse session for entity token parsing — COM_Parse requires this
	COM_BeginParseSession();

	// SOF2 Init(levelTime, randomSeed, restart, handlePool, savedGameJustLoaded) — 5 args
	fprintf(stderr, "[DBG] SV_InitGameProgs: calling ge->Init(time=%d, seed=%d, restart=0, handlePool=%p, saved=%d)\n",
		sv.time, Com_Milliseconds(), (void*)&g_handlePoolStub, (int)eSavedGameJustLoaded);
	fprintf(stderr, "[DBG] SV_InitGameProgs: ge->Init func ptr = %p\n", (void*)ge->Init);
	// Dump key gi[] slot addresses for crash correlation
	fprintf(stderr, "[DBG] gi[0]  Com_Printf         = %p\n", gi[0]);
	fprintf(stderr, "[DBG] gi[3]  Com_sprintf         = %p\n", gi[3]);
	fprintf(stderr, "[DBG] gi[4]  Com_Error           = %p\n", gi[4]);
	fprintf(stderr, "[DBG] gi[5]  FS_FOpenFileByMode  = %p\n", gi[5]);
	fprintf(stderr, "[DBG] gi[15] Com_EventLoop       = %p\n", gi[15]);
	fprintf(stderr, "[DBG] gi[23] Cvar_Get            = %p\n", gi[23]);
	fprintf(stderr, "[DBG] gi[26] Cvar_Set            = %p\n", gi[26]);
	fprintf(stderr, "[DBG] gi[31] Cvar_VarStrBuf      = %p\n", gi[31]);
	fprintf(stderr, "[DBG] gi[32] Z_Malloc            = %p\n", gi[32]);
	fprintf(stderr, "[DBG] gi[33] Z_Free              = %p\n", gi[33]);
	fprintf(stderr, "[DBG] gi[54] LocateGameData      = %p\n", gi[54]);
	fprintf(stderr, "[DBG] gi[57] LinkEntity          = %p\n", gi[57]);
	fprintf(stderr, "[DBG] gi[69] AreaEntities        = %p\n", gi[69]);
	fprintf(stderr, "[DBG] gi[71] SV_Trace            = %p\n", gi[71]);
	fprintf(stderr, "[DBG] gi[77] SetConfigstring     = %p\n", gi[77]);
	fprintf(stderr, "[DBG] gi[79] GetServerinfo       = %p\n", gi[79]);
	fprintf(stderr, "[DBG] gi[85] GetEntityToken      = %p\n", gi[85]);
	Com_Printf(
		"[SOF2 dbg] handlePool=%p gpGroup=%p gpVtable=%p\n",
		(void *)&g_handlePoolStub,
		(void *)&s_sof2GpGroup,
		(void *)s_sof2GpGroup.vftable );
	fprintf(stderr, "[DBG] SV_InitGameProgs: calling Z_TagFree(TAG_G_ALLOC)\n");
	Z_TagFree(TAG_G_ALLOC);
	fprintf(stderr, "[DBG] SV_InitGameProgs: Z_TagFree done, calling ge->Init...\n");
	fflush(stderr);
	SetUnhandledExceptionFilter( SV_CrashFilter );
	AddVectoredExceptionHandler(1, SV_VectoredHandler);  // VEH fires before SEH
	{
		// Use file markers that survive even if SEH chain is corrupted
		FILE *dbgf = fopen("dbg_init_markers.txt", "w");
		if (dbgf) { fputs("M1: before ge->Init\n", dbgf); fflush(dbgf); }

		ge->Init( sv.time, Com_Milliseconds(), 0, (void*)&g_handlePoolStub, (int)eSavedGameJustLoaded );

		if (dbgf) { fputs("M2: after ge->Init\n", dbgf); fflush(dbgf); }

		ge->InitNavigation();
		if (dbgf) { fputs("M3: after InitNavigation\n", dbgf); fflush(dbgf); }

		ge->InitSquads();
		if (dbgf) { fputs("M4: after InitSquads\n", dbgf); fflush(dbgf); }

		ge->InitWeaponSystem();
		if (dbgf) { fputs("M5: after InitWeaponSystem\n", dbgf); fflush(dbgf); }

		if (dbgf) fclose(dbgf);
	}
	fprintf(stderr, "[DBG] SV_InitGameProgs: all post-Init calls done\n");
	fflush(stderr);

	// clear all gentity pointers that might still be set from
	// a previous level
	for ( i = 0 ; i < 1 ; i++ ) {
		svs.clients[i].gentity = NULL;
	}
}



/*
====================
SV_GameCommand

See if the current console command is claimed by the game
====================
*/
qboolean SV_GameCommand( void ) {
	if ( sv.state != SS_GAME ) {
		return qfalse;
	}

	return (qboolean)ge->ConsoleCommand();
}


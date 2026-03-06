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

// sv_client.c -- server code for dealing with clients

#include "../server/exe_headers.h"

#include "server.h"
#include <windows.h>

extern char *g_sof2_dll_ps;

static void SV_ClearSOF2ForcedMove( char *dllPs, const char *phase ) {
	// Verified with a source-layout probe:
	//   offsetof(gclient_t, forced_forwardmove) = 1702
	//   offsetof(gclient_t, forced_rightmove)  = 1703
	// g_sof2_dll_ps points at gclient->ps, and ps is the first gclient field.
	if ( !dllPs ) {
		return;
	}

	volatile signed char *forcedForward = (volatile signed char *)( dllPs + 1702 );
	volatile signed char *forcedRight = (volatile signed char *)( dllPs + 1703 );
	const int forwardValue = (int)( *forcedForward );
	const int rightValue = (int)( *forcedRight );

	if ( forwardValue == 0 && rightValue == 0 ) {
		return;
	}

	static int s_forcedMoveLogCount = 0;
	if ( s_forcedMoveLogCount < 24 ) {
		Com_Printf(
			"[SOF2 fix] cleared scripted move (%s) fwd=%d right=%d\n",
			phase,
			forwardValue,
			rightValue );
		++s_forcedMoveLogCount;
	}

	*forcedForward = 0;
	*forcedRight = 0;
}

static void SV_ClearSOF2ControlSlot( char *dllPs, const char *phase ) {
	// playerState_t::generic1 lives at ps+0x150 and is the only networked field
	// in the retail SOF2 layout that plausibly backs the code's viewEntity-style
	// control path. Clearing it here prevents stale control/camera takeover from
	// swallowing movement on direct +map startup.
	if ( !dllPs ) {
		return;
	}

	volatile int *controlSlot = (volatile int *)( dllPs + 0x150 );
	const int controlValue = *controlSlot;
	if ( controlValue == 0 ) {
		return;
	}

	static int s_controlSlotLogCount = 0;
	if ( s_controlSlotLogCount < 24 ) {
		Com_Printf(
			"[SOF2 fix] cleared control slot (%s) generic1/viewEntity=%d\n",
			phase,
			controlValue );
		++s_controlSlotLogCount;
	}

	*controlSlot = 0;
}

static void SV_ClearSOF2FlySwim( char *dllPs, const char *phase ) {
	// Verified with a source-layout probe:
	//   offsetof(gclient_t, moveType) = 2212
	// MT_FLYSWIM drives PM_FlyMove even when pm_type==PM_NORMAL, which matches
	// the pra1 "look up and fly into the sky" symptom. Direct +map should start
	// the player on MT_RUNJUMP, not MT_FLYSWIM.
	if ( !dllPs ) {
		return;
	}

	volatile int *moveType = (volatile int *)( dllPs + 2212 );
	if ( *moveType != 3 ) {
		return;
	}

	static int s_moveTypeLogCount = 0;
	if ( s_moveTypeLogCount < 24 ) {
		Com_Printf(
			"[SOF2 fix] cleared flyswim (%s) moveType=%d -> %d\n",
			phase,
			*moveType,
			2 );
		++s_moveTypeLogCount;
	}

	*moveType = 2; // MT_RUNJUMP
}

static int SV_ClientThinkExceptionFilter( EXCEPTION_POINTERS *ep, int clientNum ) {
	static int thinkCrashDetailCount = 0;
	if ( ep && ep->ContextRecord && thinkCrashDetailCount < 8 ) {
		CONTEXT *ctx = ep->ContextRecord;
		const usercmd_t *cmd = NULL;
		if ( clientNum >= 0 && clientNum < MAX_CLIENTS ) {
			cmd = &svs.clients[clientNum].lastUsercmd;
		}
		fprintf( stderr,
			"[SV_ClientThink] crash #%d client=%d code=0x%08lX EIP=%08lX EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX cmdTime=%d buttons=0x%08X move=(%d,%d,%d) weapon=%u gcmd=%u\n",
			thinkCrashDetailCount + 1,
			clientNum,
			(unsigned long)ep->ExceptionRecord->ExceptionCode,
			ctx->Eip, ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx,
			ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp,
			cmd ? cmd->serverTime : -1,
			cmd ? (unsigned int)cmd->buttons : 0U,
			cmd ? (int)cmd->forwardmove : 0,
			cmd ? (int)cmd->rightmove : 0,
			cmd ? (int)cmd->upmove : 0,
			cmd ? (unsigned int)cmd->weapon : 0U,
			cmd ? (unsigned int)cmd->generic_cmd : 0U );
		if ( ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
			 ep->ExceptionRecord->NumberParameters >= 2 ) {
			fprintf( stderr, "[SV_ClientThink]   AV %s addr=%08lX\n",
				ep->ExceptionRecord->ExceptionInformation[0] ? "WRITE" : "READ",
				(unsigned long)ep->ExceptionRecord->ExceptionInformation[1] );
		}
		fflush( stderr );
		thinkCrashDetailCount++;
	}
	return EXCEPTION_EXECUTE_HANDLER;
}

/*
==================
SV_DirectConnect

A "connect" OOB command has been received
==================
*/
void SV_DirectConnect( netadr_t from ) {
	char		userinfo[MAX_INFO_STRING];
	int			i;
	client_t	*cl, *newcl;
	client_t	temp;
	gentity_t		*ent;
	int			clientNum;
	int			version;
	int			qport;
	int			challenge;
	char		*denied;

	Com_DPrintf ("SVC_DirectConnect ()\n");

	Q_strncpyz( userinfo, Cmd_Argv(1), sizeof(userinfo) );

	version = atoi( Info_ValueForKey( userinfo, "protocol" ) );
	if ( version != PROTOCOL_VERSION ) {
		NET_OutOfBandPrint( NS_SERVER, from, "print\nServer uses protocol version %i.\n", PROTOCOL_VERSION );
		Com_DPrintf ("    rejected connect from version %i\n", version);
		return;
	}

	qport = atoi( Info_ValueForKey( userinfo, "qport" ) );

	challenge = atoi( Info_ValueForKey( userinfo, "challenge" ) );

	// see if the challenge is valid (local clients don't need to challenge)
	if ( !NET_IsLocalAddress (from) ) {
		NET_OutOfBandPrint( NS_SERVER, from, "print\nNo challenge for address.\n" );
		return;
	} else {
		// force the "ip" info key to "localhost"
		Info_SetValueForKey( userinfo, "ip", "localhost" );
	}

	newcl = &temp;
	memset (newcl, 0, sizeof(client_t));

	// if there is already a slot for this ip, reuse it
	for (i=0,cl=svs.clients ; i < 1 ; i++,cl++)
	{
		if ( cl->state == CS_FREE ) {
			continue;
		}
		if ( NET_CompareBaseAdr( from, cl->netchan.remoteAddress )
			&& ( cl->netchan.qport == qport
			|| from.port == cl->netchan.remoteAddress.port ) )
		{
			if (( sv.time - cl->lastConnectTime)
				< (sv_reconnectlimit->integer * 1000))
			{
				Com_DPrintf ("%s:reconnect rejected : too soon\n", NET_AdrToString (from));
				return;
			}
			Com_Printf ("%s:reconnect\n", NET_AdrToString (from));
			newcl = cl;
			goto gotnewcl;
		}
	}


	newcl = NULL;
	for ( i = 0; i < 1 ; i++ ) {
		cl = &svs.clients[i];
		if (cl->state == CS_FREE) {
			newcl = cl;
			break;
		}
	}

	if ( !newcl ) {
		NET_OutOfBandPrint( NS_SERVER, from, "print\nServer is full.\n" );
		Com_DPrintf ("Rejected a connection.\n");
		return;
	}

gotnewcl:
	// build a new connection
	// accept the new client
	// this is the only place a client_t is ever initialized
	*newcl = temp;
	clientNum = newcl - svs.clients;
	ent = SV_GentityNum( clientNum );
	newcl->gentity = ent;

	// save the address
	Netchan_Setup (NS_SERVER, &newcl->netchan , from, qport);

	// SOF2: store challenge for netchan XOR encryption key derivation
	newcl->challenge = challenge;

	// save the userinfo
	Q_strncpyz( newcl->userinfo, userinfo, sizeof(newcl->userinfo) );

	// get the game a chance to reject this connection or modify the userinfo
	Com_Printf( "SV_DirectConnect: calling ge->ClientConnect(%d, firstTime=1)\n", clientNum );
	__try {
		denied = ge->ClientConnect( clientNum, qtrue, eSavedGameJustLoaded ); // firstTime = qtrue
	} __except( 1 ) {
		Com_Printf( "^1SV_DirectConnect: EXCEPTION in ge->ClientConnect(%d)!\n", clientNum );
		denied = NULL;  // treat as success so client can proceed
	}
	if ( denied ) {
		NET_OutOfBandPrint( NS_SERVER, from, "print\n%s\n", denied );
		Com_DPrintf ("Game rejected a connection: %s.\n", denied);
		return;
	}
	Com_Printf( "SV_DirectConnect: ge->ClientConnect(%d) succeeded\n", clientNum );

	SV_UserinfoChanged( newcl );

	// send the connect packet to the client
	NET_OutOfBandPrint( NS_SERVER, from, "connectResponse" );

	newcl->state = CS_CONNECTED;
	newcl->lastPacketTime = sv.time;
	newcl->lastConnectTime = sv.time;

	// when we receive the first packet from the client, we will
	// notice that it is from a different serverid and that the
	// gamestate message was not just sent, forcing a retransmit
	newcl->gamestateMessageNum = -1;
}


/*
=====================
SV_DropClient

Called when the player is totally leaving the server, either willingly
or unwillingly.  This is NOT called if the entire server is quiting
or crashing -- SV_FinalMessage() will handle that
=====================
*/
void SV_DropClient( client_t *drop, const char *reason ) {
	if ( drop->state == CS_ZOMBIE ) {
		return;		// already dropped
	}

	Com_Printf( "[SV] SV_DropClient: client=%d name='%s' state=%d reason='%s'\n",
		(int)( drop - svs.clients ),
		drop->name,
		drop->state,
		reason ? reason : "(null)" );

	drop->state = CS_ZOMBIE;		// become free in a few seconds

	if (drop->download)	{
		FS_FreeFile (drop->download);
		drop->download = NULL;
	}

	// call the prog function for removing a client
	// this will remove the body, among other things
	ge->ClientDisconnect( drop - svs.clients );

	// tell everyone why they got dropped
	SV_SendServerCommand( NULL, "print \"%s %s\n\"", drop->name, reason );

	// add the disconnect command
	SV_SendServerCommand( drop, "disconnect" );
}

/*
================
SV_SendClientGameState

Sends the first message from the server to a connected client.
This will be sent on the initial connection and upon each new map load.

It will be resent if the client acknowledges a later message but has
the wrong gamestate.
================
*/
void SV_SendClientGameState( client_t *client ) {
	int			start;
	msg_t		msg;
	byte		msgBuffer[MAX_MSGLEN];

	Com_Printf ("SV_SendClientGameState: client '%s' -> CS_PRIMED\n", client->name);
	client->state = CS_PRIMED;

	// when we receive the first packet from the client, we will
	// notice that it is from a different serverid and that the
	// gamestate message was not just sent, forcing a retransmit
	client->gamestateMessageNum = client->netchan.outgoingSequence;

	// clear the reliable message list for this client
	client->reliableSequence = 0;
	client->reliableAcknowledge = 0;

	MSG_Init( &msg, msgBuffer, sizeof( msgBuffer ) );

	// send the gamestate
	MSG_WriteByte( &msg, svc_gamestate );
	MSG_WriteLong( &msg, client->reliableSequence );

	// write the configstrings
	for ( start = 0 ; start < MAX_CONFIGSTRINGS ; start++ ) {
		if (sv.configstrings[start][0]) {
			MSG_WriteByte( &msg, svc_configstring );
			MSG_WriteShort( &msg, start );
			MSG_WriteString( &msg, sv.configstrings[start] );
		}
	}

	MSG_WriteByte( &msg, 0 );

	// check for overflow
	if ( msg.overflowed ) {
		Com_Printf ("WARNING: GameState overflowed for %s\n", client->name);
	}

	// deliver this to the client
	SV_SendMessageToClient( &msg, client );
}


/*
==================
SV_ClientEnterWorld
==================
*/
void SV_ClientEnterWorld( client_t *client, usercmd_t *cmd, SavedGameJustLoaded_e eSavedGameJustLoaded ) {
	int		clientNum;
	gentity_t	*ent;
	playerState_t *ps;

	Com_DPrintf ("SV_ClientEnterWorld() from %s\n", client->name);
	client->state = CS_ACTIVE;

	// set up the entity for the client
	clientNum = client - svs.clients;
	ent = SV_GentityNum( clientNum );
	SOF2_ENT_SERVERINDEX(ent) = clientNum;
	SOF2_ENT_NUMBER(ent) = clientNum;
	client->gentity = ent;
	ps = SV_GameClientNum( clientNum );
	if ( cmd ) {
		client->lastUsercmd = *cmd;
	}

	// normally I check 'qbFromSavedGame' to avoid overwriting loaded client data, but this stuff I want
	//	to be reset so that client packet delta-ing bgins afresh, rather than based on your previous frame
	//	(which didn't in fact happen now if we've just loaded from a saved game...)
	//
	client->deltaMessage = -1;
	client->cmdNum = 0;
	Com_Printf(
		"[DBG] SV_ClientEnterWorld pre ClientBegin: client=%d ent=%p serverIndex=%d entNum=%d s.clientNum=%d ps=%p cmdTime=%d buttons=0x%08X\n",
		clientNum,
		(void *)ent,
		SOF2_ENT_SERVERINDEX( ent ),
		SOF2_ENT_NUMBER( ent ),
		SOF2_ENT_S_CLIENTNUM( ent ),
		(void *)ps,
		cmd ? cmd->serverTime : -1,
		cmd ? cmd->buttons : 0 );

	// call the game begin function — SOF2 ClientBegin(clientNum) takes 1 arg only
	__try {
		ge->ClientBegin( client - svs.clients );
		Com_Printf( "SV_ClientEnterWorld: ge->ClientBegin(%d) succeeded, client is ACTIVE\n",
					(int)(client - svs.clients) );
		Com_Printf(
			"[DBG] SV_ClientEnterWorld post ClientBegin: client=%d ent=%p serverIndex=%d entNum=%d s.clientNum=%d ps=%p\n",
			clientNum,
			(void *)ent,
			SOF2_ENT_SERVERINDEX( ent ),
			SOF2_ENT_NUMBER( ent ),
			SOF2_ENT_S_CLIENTNUM( ent ),
			(void *)ps );
	} __except( 1 ) {
		Com_Printf( "^1SV_ClientEnterWorld: EXCEPTION in ge->ClientBegin(%d)\n",
					(int)(client - svs.clients) );
	}
}

/*
============================================================

CLIENT COMMAND EXECUTION

============================================================
*/


/*
=================
SV_Disconnect_f

The client is going to disconnect, so remove the connection immediately  FIXME: move to game?
=================
*/
static void SV_Disconnect_f( client_t *cl ) {
	SV_DropClient( cl, "disconnected" );
}



/*
=================
SV_UserinfoChanged

Pull specific info from a newly changed userinfo string into a more C friendly form.
=================
*/
void SV_UserinfoChanged( client_t *cl ) {
	Q_strncpyz( cl->name, Info_ValueForKey( cl->userinfo, "name" ), sizeof( cl->name ) );
}


/*
==================
SV_UpdateUserinfo_f
==================
*/
static void SV_UpdateUserinfo_f( client_t *cl ) {
	Q_strncpyz( cl->userinfo, Cmd_Argv(1), sizeof(cl->userinfo) );

	SV_UserinfoChanged( cl );
	// SOF2 SP: ClientUserinfoChanged does not exist in game_export_t v8
	// (no player name changes in single-player)
}

typedef struct {
	const char	*name;
	void	(*func)( client_t *cl );
} ucmd_t;

static ucmd_t ucmds[] = {
	{"userinfo", SV_UpdateUserinfo_f},
	{"disconnect", SV_Disconnect_f},

	{NULL, NULL}
};

/*
==================
SV_ExecuteClientCommand
==================
*/
void SV_ExecuteClientCommand( client_t *cl, const char *s ) {
	ucmd_t	*u;

	Cmd_TokenizeString( s );

	// see if it is a server level command
	for (u=ucmds ; u->name ; u++) {
		if (!strcmp (Cmd_Argv(0), u->name) ) {
			u->func( cl );
			break;
		}
	}

	// pass unknown strings to the game
	if (!u->name && sv.state == SS_GAME) {
		ge->ClientCommand( cl - svs.clients );
	}
}

#define	MAX_STRINGCMDS	8

/*
===============
SV_ClientCommand
===============
*/
static void SV_ClientCommand( client_t *cl, msg_t *msg ) {
	int		seq;
	const char	*s;

	seq = MSG_ReadLong( msg );
	s = MSG_ReadString( msg );

	// see if we have already executed it
	if ( cl->lastClientCommand >= seq ) {
		return;
	}

	Com_DPrintf( "clientCommand: %s : %i : %s\n", cl->name, seq, s );

	// drop the connection if we have somehow lost commands
	if ( seq > cl->lastClientCommand + 1 ) {
		Com_Printf( "Client %s lost %i clientCommands\n", cl->name,
			seq - cl->lastClientCommand + 1 );
	}

	SV_ExecuteClientCommand( cl, s );

	cl->lastClientCommand = seq;
}


//==================================================================================


/*
==================
SV_ClientThink
==================
*/
void SV_ClientThink (client_t *cl, usercmd_t *cmd) {
	static int startupThinkLogs = 0;
	static int sof2MoveLogCount = 0;
	static int sof2OriginLogCount = 0;
	const int clientNum = (int)(cl - svs.clients);
	gentity_t *ent = cl->gentity;
	const qboolean hasMovementInput = ( cmd->forwardmove || cmd->rightmove || cmd->upmove || cmd->buttons ) ? qtrue : qfalse;

	cl->lastUsercmd = *cmd;

	if ( cl->state != CS_ACTIVE ) {
		return;		// may have been kicked during the last usercmd
	}
	if ( startupThinkLogs < 8 && ent ) {
		playerState_t *ps = SV_GameClientNum( clientNum );
		Com_Printf(
			"[DBG] SV_ClientThink pre DLL: client=%d ent=%p serverIndex=%d entNum=%d s.clientNum=%d ps=%p cmdTime=%d buttons=0x%08X move=(%d,%d,%d) weapon=%u gcmd=%u\n",
			clientNum,
			(void *)ent,
			SOF2_ENT_SERVERINDEX( ent ),
			SOF2_ENT_NUMBER( ent ),
			SOF2_ENT_S_CLIENTNUM( ent ),
			(void *)ps,
			cmd->serverTime,
			cmd->buttons,
			(int)cmd->forwardmove,
			(int)cmd->rightmove,
			(int)cmd->upmove,
			(unsigned int)cmd->weapon,
			(unsigned int)cmd->generic_cmd );
		startupThinkLogs++;
	}
	if ( sof2MoveLogCount < 256 && hasMovementInput ) {
		Com_Printf(
			"[SV cmd] #%d client=%d move=(%d,%d,%d) ang=(%d,%d,%d) btn=0x%08X weapon=%u gcmd=%u cmdTime=%d\n",
			sof2MoveLogCount + 1,
			clientNum,
			(int)cmd->forwardmove,
			(int)cmd->rightmove,
			(int)cmd->upmove,
			cmd->angles[0], cmd->angles[1], cmd->angles[2],
			cmd->buttons,
			(unsigned int)cmd->weapon,
			(unsigned int)cmd->generic_cmd,
			cmd->serverTime );
		++sof2MoveLogCount;
	}
	if ( sof2OriginLogCount < 256 && hasMovementInput ) {
		playerState_t *ps = SV_GameClientNum( clientNum );
		if ( ps ) {
			Com_Printf(
				"[SV ps] #%d client=%d origin=(%.1f,%.1f,%.1f) vel=(%.1f,%.1f,%.1f) pm_type=%d ground=%d viewheight=%d cmd=(%d,%d,%d)\n",
				sof2OriginLogCount + 1,
				clientNum,
				ps->origin[0],
				ps->origin[1],
				ps->origin[2],
				ps->velocity[0],
				ps->velocity[1],
				ps->velocity[2],
				ps->pm_type,
				ps->groundEntityNum,
				ps->viewheight,
				(int)cmd->forwardmove,
				(int)cmd->rightmove,
				(int)cmd->upmove );
			++sof2OriginLogCount;
		}
	}

	// SOF2 movement fix: write usercmd into playerState embedded cmd slot.
	// The SOF2 game DLL (ClientThink @ 0x2004cc40) reads the usercmd directly
	// from ps+0x1DC (ps[0x77..0x7c] as 6 ints = 24 bytes) rather than calling
	// gi_GetUsercmd. The embedded layout is Q3-style:
	//   [0]=serverTime, [1..3]=angles[3], [4]=buttons, [5]={weapon,fwd,right,up}
	// OpenJK usercmd_t differs: {serverTime, buttons, weapon, angles[3], generic_cmd+fwd+right+up}
	// so we must map fields explicitly. Without this, PM_Pmove sees cmd.forwardmove=0
	// on every frame → no velocity → player frozen despite input being received.
	// Also: ps[0x77] (serverTime) must be > ps[0] (commandTime) or ClientThink
	// exits before calling G_ClientThink/PM_Pmove at all.
	// SOF2 movement fix: per-frame flag clears.
	// All must happen before ge->ClientThink or movement is bypassed.
	//
	// Patch 1 (once): PM_Pmove @ RVA 0x2f04 — JL→JLE
	//   PM_Pmove zeroes forwardmove/rightmove/upmove when ps->pm_type >= 2.
	//   ClientThink re-sets pm_type=2 every frame if CPlayer health < 1
	//   (which happens during direct +map load with no save game — player
	//   entity spawns with health=0). This makes ALL movement input zero
	//   inside PM_Pmove before PM_WalkMove even runs.
	//   Fix: change JL (0x7C) to JLE (0x7E) so only pm_type > 2 (dead=3,
	//   frozen=4) zeroes movement; pm_type=2 (spectator) keeps input intact.
	//
	// Patch 2 (per-frame): g_skipSpawnTargets @ RVA 0x3100f4
	//   ClientThink: if non-zero → G_IntermissionThink + return, skipping
	//   all of G_ClientThink/PM_Pmove. Set by G_InitGame for direct +map.
	//
	// Patch 3 (per-frame): CInvInst+0x74 @ RVA 0x31019c
	//   G_ClientThink: vtable[10]() returns this byte.
	//   Non-zero → CPlayer_Think (no input); zero → PM_Pmove.
	//
	// Patch 4 (per-frame): camera entity active flag @ *(g_cameraEntityA)+0x5ee
	//   g_cameraEntityA ptr @ RVA 0x308cd4. Points to a CCameraEntity heap
	//   object. When +0x5ee != 0, ClientThink skips G_ClientThink entirely
	//   and only drives the cinematic camera. Air1/pra1 both activate a
	//   camera sequence at map load, causing "scripted" automatic movement
	//   and no player control. CCameraEntity_Create initializes this to 0;
	//   map/script logic sets it to 1 to start a cutscene.
	{
		static HMODULE s_hGameDll = NULL;
		if ( !s_hGameDll )
			s_hGameDll = GetModuleHandleA( "gamex86" );
		if ( s_hGameDll ) {
			// --- One-time code patch: PM_Pmove JL→JLE at RVA 0x2f04 ---
			{
				static bool s_pmovePatchApplied = false;
				(void)s_pmovePatchApplied;
				if ( false ) {
					void *patchAddr = (void *)( (char *)s_hGameDll + 0x2f04 );
					unsigned char currentByte = *(unsigned char *)patchAddr;
					if ( currentByte == 0x7C ) { // JL — patch to JLE
						DWORD oldProtect = 0;
						if ( VirtualProtect( patchAddr, 1, PAGE_EXECUTE_READWRITE, &oldProtect ) ) {
							*(unsigned char *)patchAddr = 0x7E; // JLE
							VirtualProtect( patchAddr, 1, oldProtect, &oldProtect );
							Com_Printf( "[SOF2 fix] PM_Pmove patch applied: JL→JLE at RVA 0x2f04 (pm_type=2 no longer zeroes movement)\n" );
							s_pmovePatchApplied = true;
						} else {
							Com_Printf( "[SOF2 fix] PM_Pmove patch FAILED: VirtualProtect error %lu at RVA 0x2f04\n", GetLastError() );
						}
					} else {
						Com_Printf( "[SOF2 fix] PM_Pmove patch: unexpected byte 0x%02X at RVA 0x2f04 (already patched or wrong build?)\n", currentByte );
						s_pmovePatchApplied = true; // don't retry
					}
				}
			}
			// --- Per-frame: clear g_skipSpawnTargets (dword at RVA 0x3100f4) ---
			{
				volatile int *g_skipSpawnTargets = (volatile int *)( (char *)s_hGameDll + 0x3100f4 );
				if ( *g_skipSpawnTargets != 0 ) {
					static int s_skipLogCount = 0;
					if ( s_skipLogCount < 8 ) {
						Com_Printf( "[SOF2 fix] cleared g_skipSpawnTargets (was %d)\n",
							*g_skipSpawnTargets );
						++s_skipLogCount;
					}
					*g_skipSpawnTargets = 0;
				}
			}
			// --- Per-frame: clear CInvInst+0x74 (byte at RVA 0x31019c) ---
			{
				volatile char *worldControlFlag = (volatile char *)s_hGameDll + 0x310128 + 0x74;
				if ( *worldControlFlag != 0 ) {
					static int s_fixLogCount = 0;
					if ( s_fixLogCount < 8 ) {
						Com_Printf( "[SOF2 fix] cleared world CInvInst+0x74 (was %d)\n",
							(int)(unsigned char)*worldControlFlag );
						++s_fixLogCount;
					}
					*worldControlFlag = 0;
				}
			}
			// --- Per-frame: clear camera entity active flag ---
			// g_cameraEntityA is a pointer at gamex86+0x308cd4.
			// When the pointed-to object's +0x5ee byte != 0, ClientThink
			// drives the cinematic camera and completely skips G_ClientThink.
			{
				volatile int *g_cameraEntityAAddr = (volatile int *)( (char *)s_hGameDll + 0x308cd4 );
				int camEnt = *g_cameraEntityAAddr;
				if ( camEnt != 0 ) {
					volatile char *camFlag = (volatile char *)( camEnt + 0x5ee );
					if ( *camFlag != 0 ) {
						static int s_camLogCount = 0;
						if ( s_camLogCount < 8 ) {
							Com_Printf( "[SOF2 fix] cleared camera entity active flag (was %d, camEnt=0x%08X)\n",
								(int)(unsigned char)*camFlag, camEnt );
							++s_camLogCount;
						}
						*camFlag = 0;
					}
				}
			}
			// ICARUS can also override movement directly through gclient forced-move bytes.
			SV_ClearSOF2ForcedMove( g_sof2_dll_ps, "pre" );
			SV_ClearSOF2ControlSlot( g_sof2_dll_ps, "pre" );
			SV_ClearSOF2FlySwim( g_sof2_dll_ps, "pre" );
		}
	}

	// Fix M: save DLL ps origin BEFORE ClientThink so we can detect ICARUS teleports.
	// g_sof2_dll_ps is set by SV_GetUsercmd_SOF2 (gi[84]) which runs inside ClientThink
	// wrapper. However for the save we need the pre-think state — capture via the engine ps
	// which holds the last-synced position.
	float preThinkOrigin[3] = { 0.f, 0.f, 0.f };
	{
		playerState_t *psPre = SV_GameClientNum( clientNum );
		if ( psPre ) {
			preThinkOrigin[0] = psPre->origin[0];
			preThinkOrigin[1] = psPre->origin[1];
			preThinkOrigin[2] = psPre->origin[2];
		}
	}

	__try {
		ge->ClientThink( clientNum );
	} __except( SV_ClientThinkExceptionFilter( GetExceptionInformation(), clientNum ) ) {
		static int thinkCrashCount = 0;
		if ( thinkCrashCount < 3 ) {
			Com_Printf( "^1SV_ClientThink: EXCEPTION in game DLL (client %d), skipping\n",
						clientNum );
			thinkCrashCount++;
		}
	}

	// Fix M (post-think): clear camera entity active flag AFTER ClientThink.
	// ICARUS scripts re-set the camera flag during ClientThink (it runs inside G_ClientThink).
	// Clearing it again after the fact prevents the flag from persisting into snapshot building
	// and stops ICARUS from sustaining multi-frame camera takeover on air1.
	{
		static HMODULE s_hGamePost = NULL;
		if ( !s_hGamePost ) s_hGamePost = GetModuleHandleA( "gamex86" );
		if ( s_hGamePost ) {
			volatile int *g_cameraEntityAAddrPost = (volatile int *)( (char *)s_hGamePost + 0x308cd4 );
			int camEntPost = *g_cameraEntityAAddrPost;
			if ( camEntPost != 0 ) {
				volatile char *camFlagPost = (volatile char *)( camEntPost + 0x5ee );
				*camFlagPost = 0;
			}
		}
	}
	SV_ClearSOF2ForcedMove( g_sof2_dll_ps, "post" );
	SV_ClearSOF2ControlSlot( g_sof2_dll_ps, "post" );
	SV_ClearSOF2FlySwim( g_sof2_dll_ps, "post" );

	// Fix K: sync DLL playerState → engine playerState.
	// The DLL's PM_Pmove writes updated origin/velocity/viewangles into the DLL's own
	// entity ps (a separate C++ object, not SV_GameClientNum). The engine's ps is what
	// gets transmitted to the client for camera/rendering and used for PVS computation.
	// Without this sync, the engine always thinks the player is at the spawn point:
	//   - PVS stays at spawn → only spawn-area entities visible → map appears incomplete
	//   - Camera never moves → player walks "offscreen"
	//
	// DLL ps offsets (confirmed from cgame PM_Pmove / PM_UpdateViewAngles disassembly):
	//   origin[3]     : ps+0x14, +0x18, +0x1C
	//   velocity[3]   : ps+0x2C, +0x30, +0x34
	//   viewangles[3] : ps+0xB0, +0xB4, +0xB8  (Q3A convention: [0]=PITCH,[1]=YAW,[2]=ROLL)
	//
	// g_sof2_dll_ps is set in SV_GetUsercmd_SOF2 (gi[84]) which is called by the DLL's
	// exported ClientThink wrapper before it calls the internal ClientThink.
	if ( g_sof2_dll_ps ) {
		playerState_t *engPs = SV_GameClientNum( clientNum );
		if ( engPs ) {
			float *dll_origin     = (float *)(g_sof2_dll_ps + 0x14);
			float *dll_velocity   = (float *)(g_sof2_dll_ps + 0x2C);
			int *dll_delta        = (int *)(g_sof2_dll_ps + 0x50);
			int *dll_pm_type      = (int *)(g_sof2_dll_ps + 0x04);
			int *dll_pm_flags     = (int *)(g_sof2_dll_ps + 0x0C);
			int *dll_ground       = (int *)(g_sof2_dll_ps + 0x5C);
			float *dll_viewangles = (float *)(g_sof2_dll_ps + 0xB0);
			int *dll_viewheight   = (int *)(g_sof2_dll_ps + 0xBC);
			int *dll_generic1     = (int *)(g_sof2_dll_ps + 0x150);
			static vec3_t s_lastDllOrigin[MAX_CLIENTS];
			static int s_noInputDriftFrames[MAX_CLIENTS];
			static qboolean s_haveLastDllOrigin[MAX_CLIENTS];

			if ( clientNum >= 0 && clientNum < MAX_CLIENTS ) {
				if ( !hasMovementInput && s_haveLastDllOrigin[clientNum] ) {
					float dx = dll_origin[0] - s_lastDllOrigin[clientNum][0];
					float dy = dll_origin[1] - s_lastDllOrigin[clientNum][1];
					float planarDelta2 = dx * dx + dy * dy;
					float planarSpeed2 = dll_velocity[0] * dll_velocity[0] + dll_velocity[1] * dll_velocity[1];

					// Air1 can re-assert scripted motion without user input. Detect both
					// fast velocity and persistent no-input XY translation.
					if ( planarDelta2 > ( 0.5f * 0.5f ) || planarSpeed2 > ( 20.0f * 20.0f ) ) {
						s_noInputDriftFrames[clientNum]++;
					} else {
						s_noInputDriftFrames[clientNum] = 0;
					}

					if ( s_noInputDriftFrames[clientNum] >= 2 ) {
						if ( planarDelta2 > ( 0.25f * 0.25f ) || planarSpeed2 > ( 10.0f * 10.0f ) ) {
							dll_origin[0] = s_lastDllOrigin[clientNum][0];
							dll_origin[1] = s_lastDllOrigin[clientNum][1];
							dll_velocity[0] = 0.0f;
							dll_velocity[1] = 0.0f;
							static int s_driftGuardLogCount = 0;
							if ( s_driftGuardLogCount < 24 ) {
								Com_Printf(
									"[SV guard] blocked no-input drift client=%d dxy=%.2f speed2=%.1f keep=(%.1f,%.1f,%.1f)\n",
									clientNum,
									planarDelta2,
									planarSpeed2,
									s_lastDllOrigin[clientNum][0],
									s_lastDllOrigin[clientNum][1],
									s_lastDllOrigin[clientNum][2] );
								s_driftGuardLogCount++;
							}
						}
					}
				} else {
					s_noInputDriftFrames[clientNum] = 0;
				}
			}

			VectorCopy( dll_origin,   engPs->origin );
			VectorCopy( dll_velocity, engPs->velocity );

			// DLL uses standard Q3A viewangles convention ([0]=PITCH,[1]=YAW,[2]=ROLL)
			// after Fix L revert. Direct copy — no swap needed.
			VectorCopy( dll_viewangles, engPs->viewangles );
			engPs->delta_angles[0] = dll_delta[0];
			engPs->delta_angles[1] = dll_delta[1];
			engPs->delta_angles[2] = dll_delta[2];
			engPs->pm_type = *dll_pm_type;
			engPs->pm_flags = *dll_pm_flags;
			engPs->groundEntityNum = *dll_ground;
			engPs->viewheight = *dll_viewheight;
			engPs->generic1 = 0;
			*dll_generic1 = 0;

			// Normalize pm_type and viewheight after DLL may have set spectator state
			if ( engPs->pm_type == 1 || engPs->pm_type == 2 ) {
				engPs->pm_type = 0;
			}
			if ( engPs->viewheight < 0 ) {
				engPs->viewheight = 38;
			}
			if ( clientNum >= 0 && clientNum < MAX_CLIENTS ) {
				VectorCopy( engPs->origin, s_lastDllOrigin[clientNum] );
				s_haveLastDllOrigin[clientNum] = qtrue;
			}

			static int syncLogCount = 0;
			if ( syncLogCount < 12 ) {
				Com_Printf( "[SV sync] #%d origin=(%.1f,%.1f,%.1f) vel=(%.1f,%.1f,%.1f) va=(%.1f,%.1f,%.1f) delta=(%d,%d,%d)\n",
					syncLogCount + 1,
					dll_origin[0], dll_origin[1], dll_origin[2],
					dll_velocity[0], dll_velocity[1], dll_velocity[2],
					dll_viewangles[0], dll_viewangles[1], dll_viewangles[2],
					dll_delta[0], dll_delta[1], dll_delta[2] );
				syncLogCount++;
			}
		}
	} else {
		// Fallback: g_sof2_dll_ps not set (gi[84] not called this frame)
		playerState_t *psPostNorm = SV_GameClientNum( clientNum );
		if ( psPostNorm && ( psPostNorm->pm_type == 1 || psPostNorm->pm_type == 2 ) ) {
			psPostNorm->pm_type = 0;
			if ( psPostNorm->viewheight < 0 ) {
				psPostNorm->viewheight = 38;
			}
		}
	}

	// Post-ClientThink diagnostic: log playerState after sync
	if ( hasMovementInput ) {
		static int postThinkLogCount = 0;
		if ( postThinkLogCount < 48 ) {
			playerState_t *psPost = SV_GameClientNum( clientNum );
			if ( psPost ) {
				Com_Printf( "[SV post] #%d origin=(%.1f,%.1f,%.1f) vel=(%.1f,%.1f,%.1f) va=(%.1f,%.1f,%.1f)\n",
					postThinkLogCount + 1,
					psPost->origin[0], psPost->origin[1], psPost->origin[2],
					psPost->velocity[0], psPost->velocity[1], psPost->velocity[2],
					psPost->viewangles[0], psPost->viewangles[1], psPost->viewangles[2] );
			}
			postThinkLogCount++;
		}
	}
}

/*
==================
SV_UserMove

The message usually contains all the movement commands
that were in the last three packets, so that the information
in dropped packets can be recovered.

On very fast clients, there may be multiple usercmd packed into
each of the backup packets.
==================
*/
static void SV_UserMove( client_t *cl, msg_t *msg ) {
	int			i, start;
	int			cmdNum;
	int			firstNum;
	int			cmdCount;
	usercmd_t	nullcmd;
	usercmd_t	cmds[MAX_PACKET_USERCMDS];
	usercmd_t	*cmd, *oldcmd;
	//int			clientTime;
	int			serverId;
	static int primedUserMoveLogCount = 0;

	cl->reliableAcknowledge = MSG_ReadLong( msg );
	serverId = MSG_ReadLong( msg );
	/*clientTime = */MSG_ReadLong( msg );
	cl->deltaMessage = MSG_ReadLong( msg );

	// cmdNum is the command number of the most recent included usercmd
	cmdNum = MSG_ReadLong( msg );
	cmdCount = MSG_ReadByte( msg );

	if ( cmdCount < 1 ) {
		Com_Printf( "cmdCount < 1\n" );
		return;
	}

	if ( cmdCount > MAX_PACKET_USERCMDS ) {
		Com_Printf( "cmdCount > MAX_PACKET_USERCMDS\n" );
		return;
	}

	memset( &nullcmd, 0, sizeof(nullcmd) );
	oldcmd = &nullcmd;
	for ( i = 0 ; i < cmdCount ; i++ ) {
		cmd = &cmds[i];
		MSG_ReadDeltaUsercmd( msg, oldcmd, cmd );
		oldcmd = cmd;
	}

	// if this is a usercmd from a previous gamestate,
	// ignore it or retransmit the current gamestate
	if ( serverId != sv.serverId ) {
		// if we can tell that the client has dropped the last
		// gamestate we sent them, resend it
		if ( cl->netchan.incomingAcknowledged > cl->gamestateMessageNum ) {
			Com_DPrintf( "%s : dropped gamestate, resending\n", cl->name );
			SV_SendClientGameState( cl );
		}
		return;
	}

	// if this is the first usercmd we have received
	// this gamestate, put the client into the world
	if ( cl->state == CS_PRIMED ) {
		if ( primedUserMoveLogCount < 24 ) {
			Com_Printf(
				"[SV net] UserMove #%d primed client=%d serverId=%d deltaMsg=%d cmdNum=%d cmdCount=%d firstCmdTime=%d state=%d\n",
				primedUserMoveLogCount + 1,
				(int)( cl - svs.clients ),
				serverId,
				cl->deltaMessage,
				cmdNum,
				cmdCount,
				cmdCount > 0 ? cmds[0].serverTime : -1,
				cl->state );
			++primedUserMoveLogCount;
		}

		SV_ClientEnterWorld( cl, &cmds[0], eSavedGameJustLoaded );
		if ( sv_mapname->string[0]!='_' )
		{
			char savename[MAX_QPATH];
			if ( eSavedGameJustLoaded == eNO )
			{
				SG_WriteSavegame("auto",qtrue);
				if ( Q_stricmpn(sv_mapname->string, "academy", 7) != 0)
				{
					Com_sprintf (savename, sizeof(savename), "auto_%s",sv_mapname->string);
					SG_WriteSavegame(savename,qtrue);//can't use va becuase it's nested
				}
			}
			else if ( qbLoadTransition == qtrue )
			{
				Com_sprintf (savename, sizeof(savename), "hub/%s", sv_mapname->string );
				SG_WriteSavegame( savename, qfalse );//save a full one
				SG_WriteSavegame( "auto", qfalse );//need a copy for auto, too
			}
		}
		eSavedGameJustLoaded = eNO;
		// the moves can be processed normaly
	}

	if ( cl->state != CS_ACTIVE ) {
		cl->deltaMessage = -1;
		return;
	}


	// if there is a time gap from the last packet to this packet,
	// fill in with the first command in the packet

	// with a packetdup of 0, firstNum == cmdNum
	firstNum = cmdNum - ( cmdCount - 1 );
	if ( cl->cmdNum < firstNum - 1 ) {
		cl->droppedCommands = qtrue;
		if ( sv_showloss->integer ) {
			Com_Printf("Lost %i usercmds from %s\n", firstNum - 1 - cl->cmdNum,
				cl->name);
		}
		if ( cl->cmdNum < firstNum - 6 ) {
			cl->cmdNum = firstNum - 6;		// don't generate too many
		}
		while ( cl->cmdNum < firstNum - 1 ) {
			cl->cmdNum++;
			SV_ClientThink( cl, &cmds[0] );
		}
	}
	// skip over any usercmd_t we have already executed
	start = cl->cmdNum - ( firstNum - 1 );
	for ( i =  start ; i < cmdCount ; i++ ) {
		SV_ClientThink (cl, &cmds[ i ]);
	}
	cl->cmdNum = cmdNum;

}


/*
===========================================================================

USER CMD EXECUTION

===========================================================================
*/

/*
===================
SV_ExecuteClientMessage

Parse a client packet
===================
*/
void SV_ExecuteClientMessage( client_t *cl, msg_t *msg ) {
	int			c;

	while( 1 ) {
		if ( msg->readcount > msg->cursize ) {
			SV_DropClient (cl, "had a badread");
			return;
		}

		c = MSG_ReadByte( msg );
		if ( c == -1 ) {
			break;
		}

		switch( c ) {
		default:
			SV_DropClient( cl,"had an unknown command char" );
			return;

		case clc_nop:
			break;

		case clc_move:
			SV_UserMove( cl, msg );
			break;

		case clc_clientCommand:
			SV_ClientCommand( cl, msg );
			if (cl->state == CS_ZOMBIE) {
				return;	// disconnect command
			}
			break;
		}
	}
}


void SV_FreeClient(client_t *client)
{
	int i;

	if (!client) return;

	for(i=0; i<MAX_RELIABLE_COMMANDS; i++) {
		if ( client->reliableCommands[ i] ) {
			Z_Free( client->reliableCommands[ i] );
			client->reliableCommands[i] = NULL;
			client->reliableSequence = 0;
		}
	}
}


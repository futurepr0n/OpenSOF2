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

#include "../client/snd_music.h"	// didn't want to put this in snd_local because of rebuild times etc.
#include "server.h"

#if !defined (MINIHEAP_H_INC)
	#include "../qcommon/MiniHeap.h"
#endif

void CM_CleanLeafCache(void);
extern void SV_FreeClient(client_t*);

CMiniHeap *G2VertSpaceServer = NULL;
/*
Ghoul2 Insert End
*/

static void SV_SOF2_ApplyBootstrapCfg( const char *path, int depth ) {
	void *buffer = NULL;
	char *cursor;
	int len;
	int appliedCount = 0;

	if ( !path || !path[0] || depth > 8 ) {
		return;
	}

	len = FS_ReadFile( path, &buffer );
	if ( len <= 0 || !buffer ) {
		Com_Printf( "[SOF2 bootstrap] missing cfg '%s'\n", path );
		return;
	}

	Com_Printf( "[SOF2 bootstrap] apply cfg '%s'\n", path );
	cursor = static_cast<char *>( buffer );
	while ( *cursor ) {
		char line[1024];
		char *lineStart = cursor;
		int lineLen = 0;
		const char *cmd;

		while ( *cursor && *cursor != '\n' && *cursor != '\r' ) {
			++cursor;
		}
		lineLen = (int)( cursor - lineStart );
		if ( lineLen >= (int)sizeof( line ) ) {
			lineLen = sizeof( line ) - 1;
		}
		memcpy( line, lineStart, lineLen );
		line[lineLen] = '\0';

		while ( *cursor == '\r' || *cursor == '\n' ) {
			++cursor;
		}

		Cmd_TokenizeString( line );
		if ( Cmd_Argc() < 1 ) {
			continue;
		}

		cmd = Cmd_Argv( 0 );
		if ( !cmd[0] || !Q_stricmp( cmd, "wait" ) || !Q_stricmp( cmd, "intermission" ) ) {
			continue;
		}
		if ( !Q_stricmp( cmd, "exec" ) ) {
			if ( Cmd_Argc() >= 2 ) {
				SV_SOF2_ApplyBootstrapCfg( Cmd_Argv( 1 ), depth + 1 );
			}
			continue;
		}
		if ( !Q_stricmp( cmd, "select" ) ) {
			if ( Cmd_Argc() >= 2 ) {
				Cvar_Set( Cmd_Argv( 1 ), "1" );
				if ( appliedCount < 24 ) {
					Com_Printf( "[SOF2 bootstrap] select %s=1\n", Cmd_Argv( 1 ) );
				}
				++appliedCount;
			}
			continue;
		}
		if ( !Q_stricmpn( cmd, "set", 3 ) ) {
			if ( Cmd_Argc() >= 3 ) {
				Cvar_Set( Cmd_Argv( 1 ), Cmd_ArgsFrom( 2 ) );
				if ( appliedCount < 24 ) {
					Com_Printf( "[SOF2 bootstrap] set %s=%s\n", Cmd_Argv( 1 ), Cmd_ArgsFrom( 2 ) );
				}
				++appliedCount;
			}
			continue;
		}

		if ( appliedCount < 24 ) {
			Com_Printf( "[SOF2 bootstrap] ignored cmd '%s' in %s\n", cmd, path );
		}
	}

	FS_FreeFile( buffer );
}

static const char *SV_SOF2_SelectCfgForMap( const char *server ) {
	if ( !server || !server[0] ) {
		return NULL;
	}
	if ( !Q_stricmp( server, "air1" ) ) {
		return "menus/select_air1.cfg";
	}
	return NULL;
}

static void SV_SOF2_SeedDirectMapBootstrap( const char *server ) {
	const char *selectCfg;
	const char *currentMission = Cvar_VariableString( "current_mission" );
	const char *defaultWpns;

	if ( currentMission && currentMission[0] ) {
		Com_Printf( "[SOF2 bootstrap] preserving existing current_mission=%s\n", currentMission );
		return;
	}

	selectCfg = SV_SOF2_SelectCfgForMap( server );
	if ( !selectCfg ) {
		return;
	}

	Com_Printf( "[SOF2 bootstrap] seeding direct map bootstrap for %s via %s\n", server, selectCfg );
	SV_SOF2_ApplyBootstrapCfg( selectCfg, 0 );

	defaultWpns = Cvar_VariableString( "default_wpns" );
	if ( defaultWpns && defaultWpns[0] ) {
		SV_SOF2_ApplyBootstrapCfg( defaultWpns, 0 );
	}

	Com_Printf(
		"[SOF2 bootstrap] result: world_map=%s current_mission=%s inv=%s mi_health=%s mi_armor=%s wp_ussocom=%s wp_oicw=%s wp_m590=%s wp_m84=%s wp_thermal=%s\n",
		Cvar_VariableString( "world_map" ),
		Cvar_VariableString( "current_mission" ),
		Cvar_VariableString( "current_mission_inv" ),
		Cvar_VariableString( "mi_health" ),
		Cvar_VariableString( "mi_armor" ),
		Cvar_VariableString( "wp_ussocom" ),
		Cvar_VariableString( "wp_oicw" ),
		Cvar_VariableString( "wp_m590" ),
		Cvar_VariableString( "wp_m84" ),
		Cvar_VariableString( "wp_thermal" ) );
}


/*
===============
SV_SetConfigstring

===============
*/
void SV_SetConfigstring (int index, const char *val) {
	if ( index < 0 || index >= MAX_CONFIGSTRINGS ) {
		Com_Error (ERR_DROP, "SV_SetConfigstring: bad index %i\n", index);
	}

	if ( !val ) {
		val = "";
	}

	// don't bother broadcasting an update if no change
	if ( !strcmp( val, sv.configstrings[ index ] ) ) {
		return;
	}

	// change the string in sv
	Z_Free( sv.configstrings[index] );
	sv.configstrings[index] = CopyString( val );

	// send it to all the clients if we aren't
	// spawning a new server
	if ( sv.state == SS_GAME ) {
		SV_SendServerCommand( NULL, "cs %i \"%s\"\n", index, val );
	}
}



/*
===============
SV_GetConfigstring

===============
*/
void SV_GetConfigstring( int index, char *buffer, int bufferSize ) {
	if ( bufferSize < 1 ) {
		Com_Error( ERR_DROP, "SV_GetConfigstring: bufferSize == %i", bufferSize );
	}
	if ( index < 0 || index >= MAX_CONFIGSTRINGS ) {
		Com_Error (ERR_DROP, "SV_GetConfigstring: bad index %i\n", index);
	}
	if ( !sv.configstrings[index] ) {
		buffer[0] = 0;
		return;
	}

	Q_strncpyz( buffer, sv.configstrings[index], bufferSize );
}


/*
===============
SV_SetUserinfo

===============
*/
void SV_SetUserinfo( int index, const char *val ) {
	if ( index < 0 || index >= 1 ) {
		Com_Error (ERR_DROP, "SV_SetUserinfo: bad index %i\n", index);
	}

	if ( !val ) {
		val = "";
	}

	Q_strncpyz( svs.clients[ index ].userinfo, val, sizeof( svs.clients[ index ].userinfo ) );
}



/*
===============
SV_GetUserinfo

===============
*/
void SV_GetUserinfo( int index, char *buffer, int bufferSize ) {
	if ( bufferSize < 1 ) {
		Com_Error( ERR_DROP, "SV_GetUserinfo: bufferSize == %i", bufferSize );
	}
	if ( index < 0 || index >= 1 ) {
		Com_Error (ERR_DROP, "SV_GetUserinfo: bad index %i\n", index);
	}
	Q_strncpyz( buffer, svs.clients[ index ].userinfo, bufferSize );
}


/*
================
SV_CreateBaseline

Entity baselines are used to compress non-delta messages
to the clients -- only the fields that differ from the
baseline will be transmitted
================
*/
void SV_CreateBaseline( void ) {
	gentity_t			*svent;
	int				entnum;
	int				permanentCount = 0;
	int				modeledCount = 0;
	int				solidBmodelCount = 0;
	int				loggedCount = 0;

	for ( entnum = 0; entnum < MAX_GENTITIES ; entnum++ ) {
		svent = SV_GentityNum(entnum);
		if (!svent) {
			continue;	// SOF2: empty pointer table slot
		}
		if (!SOF2_ENT_LINKED(svent)) {
			continue;
		}
		SOF2_ENT_NUMBER(svent) = entnum;

		//
		// take current state as baseline
		// SOF2: entityState_t is 256 bytes at offset 8, not JKA's 272-byte struct at offset 0
		//
		memcpy( &sv.svEntities[entnum].baseline, SOF2_ENT_S_PTR(svent), SOF2_ENTITYSTATE_SIZE );

		if ( sv.svEntities[entnum].baseline.eFlags & EF_PERMANENT ) {
			++permanentCount;
		}
		if ( sv.svEntities[entnum].baseline.modelindex != 0 || sv.svEntities[entnum].baseline.modelindex2 != 0 ) {
			++modeledCount;
		}
		if ( sv.svEntities[entnum].baseline.solid == SOLID_BMODEL ) {
			++solidBmodelCount;
		}
		if ( loggedCount < 32 &&
			( ( sv.svEntities[entnum].baseline.eFlags & EF_PERMANENT ) ||
			  sv.svEntities[entnum].baseline.modelindex != 0 ||
			  sv.svEntities[entnum].baseline.modelindex2 != 0 ||
			  sv.svEntities[entnum].baseline.solid == SOLID_BMODEL ) ) {
			Com_Printf(
				"[SV baseline] num=%d type=%d flags=0x%x model=%d model2=%d solid=0x%x linked=%d svflags=0x%x\n",
				entnum,
				sv.svEntities[entnum].baseline.eType,
				sv.svEntities[entnum].baseline.eFlags,
				sv.svEntities[entnum].baseline.modelindex,
				sv.svEntities[entnum].baseline.modelindex2,
				sv.svEntities[entnum].baseline.solid,
				(int)SOF2_ENT_LINKED(svent),
				SOF2_ENT_SVFLAGS(svent) );
			++loggedCount;
		}
	}

	Com_Printf(
		"[SV baseline] summary permanent=%d modeled=%d solidBmodels=%d\n",
		permanentCount,
		modeledCount,
		solidBmodelCount );
}




/*
===============
SV_Startup

Called when a game is about to begin
===============
*/
void SV_Startup( void ) {
	if ( svs.initialized ) {
		Com_Error( ERR_FATAL, "SV_Startup: svs.initialized" );
	}

	svs.clients = (struct client_s *) Z_Malloc ( sizeof(client_t) * 1, TAG_CLIENTS, qtrue );
	svs.numSnapshotEntities = 2 * 4 * 64;
	svs.initialized = qtrue;

	Cvar_Set( "sv_running", "1" );
}

qboolean CM_SameMap(const char *server);
void Cvar_Defrag(void);

/*
================
SV_SpawnServer

Change the server to a new map, taking all connected
clients along with it.
================
*/
void SV_SpawnServer( const char *server, ForceReload_e eForceReload, qboolean bAllowScreenDissolve )
{
	int			i;
	int			checksum;

	re.RegisterMedia_LevelLoadBegin( server, eForceReload, bAllowScreenDissolve );


	Cvar_SetValue( "cl_paused", 0 );
	Cvar_Set( "timescale", "1" );//jic we were skipping

	// shut down the existing game if it is running
	SV_ShutdownGameProgs(qtrue);

	Com_Printf ("------ Server Initialization ------\n%s\n", com_version->string);
	Com_Printf ("Server: %s\n",server);

	// Moved up from below to help reduce fragmentation
	if (svs.snapshotEntities)
	{
		Z_Free(svs.snapshotEntities);
		svs.snapshotEntities = NULL;
	}

	// don't let sound stutter and dump all stuff on the hunk
	fprintf(stderr, "[DBG] SV_SpawnServer: calling CL_MapLoading\n");
	CL_MapLoading();
	fprintf(stderr, "[DBG] SV_SpawnServer: CL_MapLoading done\n");

	if (!CM_SameMap(server))
	{ //rww - only clear if not loading the same map
		fprintf(stderr, "[DBG] SV_SpawnServer: calling CM_ClearMap\n");
		CM_ClearMap();
		fprintf(stderr, "[DBG] SV_SpawnServer: CM_ClearMap done\n");
	}

	// Miniheap never changes sizes, so I just put it really early in mem.
	fprintf(stderr, "[DBG] SV_SpawnServer: calling G2VertSpaceServer->ResetHeap\n");
	G2VertSpaceServer->ResetHeap();
	fprintf(stderr, "[DBG] SV_SpawnServer: calling Hunk_Clear\n");
	Hunk_Clear();

	// wipe the entire per-level structure
	// Also moved up, trying to do all freeing before new allocs
	for ( i = 0 ; i < MAX_CONFIGSTRINGS ; i++ ) {
		if ( sv.configstrings[i] ) {
			Z_Free( sv.configstrings[i] );
			sv.configstrings[i] = NULL;
		}
	}

	// Collect all the small allocations done by the cvar system
	// This frees, then allocates. Make it the last thing before other
	// allocations begin!
	fprintf(stderr, "[DBG] SV_SpawnServer: calling Cvar_Defrag\n");
	Cvar_Defrag();

	// init client structures and svs.numSnapshotEntities
	if ( !Cvar_VariableIntegerValue("sv_running") ) {
		fprintf(stderr, "[DBG] SV_SpawnServer: calling SV_Startup\n");
		SV_Startup();
	}

	fprintf(stderr, "[DBG] SV_SpawnServer: calling re.SVModelInit\n");
	re.SVModelInit();

	// allocate the snapshot entities
	fprintf(stderr, "[DBG] SV_SpawnServer: allocating snapshot entities\n");
	svs.snapshotEntities = (entityState_t *) Z_Malloc (sizeof(entityState_t)*svs.numSnapshotEntities, TAG_CLIENTS, qtrue );

	Music_SetLevelName(server);

	Cvar_Set( "nextmap", va("map %s", server) );

	memset (&sv, 0, sizeof(sv));
	sv.mLocalSubBSPIndex = -1;	// SOF2: -1 = use main BSP entity string (not sub-BSP)

	for ( i = 0 ; i < MAX_CONFIGSTRINGS ; i++ ) {
		sv.configstrings[i] = CopyString("");
	}

	sv.time = 1000;
	re.G2API_SetTime(sv.time,G2T_SV_TIME);

	fprintf(stderr, "[DBG] SV_SpawnServer: calling CM_LoadMap maps/%s.bsp\n", server);
	CM_LoadMap( va("maps/%s.bsp", server), qfalse, &checksum, qfalse );
	fprintf(stderr, "[DBG] SV_SpawnServer: CM_LoadMap done, checksum=%d\n", checksum);

	Cvar_Set( "mapname", server );
	Cvar_Set( "sv_mapChecksum", va("%i",checksum) );
	SV_SOF2_SeedDirectMapBootstrap( server );

	sv.serverId = com_frameTime;
	Cvar_Set( "sv_serverid", va("%i", sv.serverId ) );

	fprintf(stderr, "[DBG] SV_SpawnServer: calling SV_ClearWorld\n");
	SV_ClearWorld ();
	fprintf(stderr, "[DBG] SV_SpawnServer: SV_ClearWorld done\n");

	sv.state = SS_LOADING;

	fprintf(stderr, "[DBG] SV_SpawnServer: calling SV_InitGameProgs\n");
	SV_InitGameProgs();
	fprintf(stderr, "[DBG] SV_SpawnServer: SV_InitGameProgs done\n");

	// run a few frames to allow everything to settle
	for ( i = 0 ;i < 4 ; i++ ) {
		fprintf(stderr, "[DBG] SV_SpawnServer: ge->RunFrame(%d) iteration %d\n", sv.time, i);
		ge->RunFrame( sv.time );
		sv.time += 100;
		re.G2API_SetTime(sv.time,G2T_SV_TIME);
	}
	fprintf(stderr, "[DBG] SV_SpawnServer: RunFrame loop done\n");
#ifndef JK2_MODE
	fprintf(stderr, "[DBG] SV_SpawnServer: calling ge->ConnectNavs\n");
	ge->ConnectNavs();  // SOF2: ConnectNavs(void) — no args
	fprintf(stderr, "[DBG] SV_SpawnServer: ConnectNavs done\n");
#endif
	if ( ge->PostLoadInit ) {
		fprintf(stderr, "[DBG] SV_SpawnServer: calling ge->PostLoadInit\n");
		__try {
			ge->PostLoadInit();
		}
		__except ( 1 ) {
			Com_Printf("^3[SV] suppressed exception during ge->PostLoadInit()\n");
		}
		fprintf(stderr, "[DBG] SV_SpawnServer: PostLoadInit done\n");
	}

	// create a baseline for more efficient communications
	fprintf(stderr, "[DBG] SV_SpawnServer: calling SV_CreateBaseline\n");
	SV_CreateBaseline ();
	fprintf(stderr, "[DBG] SV_SpawnServer: SV_CreateBaseline done\n");

	for (i=0 ; i<1 ; i++) {
		// clear all time counters, because we have reset sv.time
		svs.clients[i].lastPacketTime = 0;
		svs.clients[i].lastConnectTime = 0;

		// send the new gamestate to all connected clients
		if (svs.clients[i].state >= CS_CONNECTED) {
			char	*denied;

			// connect the client again
			denied = ge->ClientConnect( i, qfalse, eNO/*qfalse*/ );	// firstTime = qfalse, qbFromSavedGame
			if ( denied ) {
				// this generally shouldn't happen, because the client
				// was connected before the level change
				SV_DropClient( &svs.clients[i], denied );
			} else {
				svs.clients[i].state = CS_CONNECTED;
				// when we get the next packet from a connected client,
				// the new gamestate will be sent
			}
		}
	}

	// run another frame to allow things to look at all connected clients
	ge->RunFrame( sv.time );
	sv.time += 100;
	re.G2API_SetTime(sv.time,G2T_SV_TIME);


	// save systeminfo and serverinfo strings
	SV_SetConfigstring( CS_SYSTEMINFO, Cvar_InfoString( CVAR_SYSTEMINFO ) );
	cvar_modifiedFlags &= ~CVAR_SYSTEMINFO;

	SV_SetConfigstring( CS_SERVERINFO, Cvar_InfoString( CVAR_SERVERINFO ) );
	cvar_modifiedFlags &= ~CVAR_SERVERINFO;

	// any media configstring setting now should issue a warning
	// and any configstring changes should be reliably transmitted
	// to all clients
	sv.state = SS_GAME;

	Hunk_SetMark();
	Z_Validate();
	Z_Validate();
	Z_Validate();

	Com_Printf ("-----------------------------------\n");
}

#define G2_VERT_SPACE_SIZE 256
#define G2_MINIHEAP_SIZE	G2_VERT_SPACE_SIZE*1024

/*
===============
SV_Init

Only called at main exe startup, not for each game
===============
*/
void SV_Init (void) {
	SV_AddOperatorCommands ();

	// serverinfo vars
	Cvar_Get ("protocol", va("%i", PROTOCOL_VERSION), CVAR_SERVERINFO | CVAR_ROM);
	sv_mapname = Cvar_Get ("mapname", "nomap", CVAR_SERVERINFO | CVAR_ROM);

	// systeminfo
	Cvar_Get ("helpUsObi", "0", CVAR_SYSTEMINFO );
	sv_serverid = Cvar_Get ("sv_serverid", "0", CVAR_SYSTEMINFO | CVAR_ROM );

	// server vars
	sv_fps = Cvar_Get ("sv_fps", "20", CVAR_TEMP );
	sv_timeout = Cvar_Get ("sv_timeout", "120", CVAR_TEMP );
	sv_zombietime = Cvar_Get ("sv_zombietime", "2", CVAR_TEMP );
	Cvar_Get ("nextmap", "", CVAR_TEMP );
	sv_spawntarget = Cvar_Get ("spawntarget", "", 0 );

	sv_reconnectlimit = Cvar_Get ("sv_reconnectlimit", "3", 0);
	sv_showloss = Cvar_Get ("sv_showloss", "0", 0);
	sv_killserver = Cvar_Get ("sv_killserver", "0", 0);
	sv_mapChecksum = Cvar_Get ("sv_mapChecksum", "", CVAR_ROM);
	sv_testsave = Cvar_Get ("sv_testsave", "0", 0);
	sv_compress_saved_games = Cvar_Get ("sv_compress_saved_games", "1", 0);

	// Only allocated once, no point in moving it around and fragmenting
	// create a heap for Ghoul2 to use for game side model vertex transforms used in collision detection
	{
		static CMiniHeap singleton(G2_MINIHEAP_SIZE);
		G2VertSpaceServer = &singleton;
	}
}


/*
==================
SV_FinalMessage

Used by SV_Shutdown to send a final message to all
connected clients before the server goes down.  The messages are sent immediately,
not just stuck on the outgoing message list, because the server is going
to totally exit after returning from this function.
==================
*/
void SV_FinalMessage( const char *message ) {
	client_t *cl = svs.clients;

	SV_SendServerCommand( NULL, "print \"%s\"", message );
	SV_SendServerCommand( NULL, "disconnect" );

	// send it twice, ignoring rate
	if ( cl->state >= CS_CONNECTED ) {
		SV_SendClientSnapshot( cl );
		SV_SendClientSnapshot( cl );
	}
}


/*
================
SV_Shutdown

Called when each game quits,
before Sys_Quit or Sys_Error
================
*/
void SV_Shutdown( const char *finalmsg ) {
	int i;

	if ( !com_sv_running || !com_sv_running->integer ) {
		return;
	}

	Com_Printf( "^3[SV shutdown] finalmsg='%s'\n", finalmsg ? finalmsg : "(null)" );

	//Com_Printf( "----- Server Shutdown -----\n" );

	if ( svs.clients && !com_errorEntered ) {
		SV_FinalMessage( finalmsg );
	}

	SV_RemoveOperatorCommands();
	SV_ShutdownGameProgs(qfalse);

	if (svs.snapshotEntities)
	{
		Z_Free(svs.snapshotEntities);
		svs.snapshotEntities = NULL;
	}

	for ( i = 0 ; i < MAX_CONFIGSTRINGS ; i++ ) {
		if ( sv.configstrings[i] ) {
			Z_Free( sv.configstrings[i] );
		}
	}

	// free current level
	memset( &sv, 0, sizeof( sv ) );

	// free server static data
	if ( svs.clients ) {
		SV_FreeClient(svs.clients);
		Z_Free( svs.clients );
	}
	memset( &svs, 0, sizeof( svs ) );

	// Ensure we free any memory used by the leaf cache.
	CM_CleanLeafCache();

	Cvar_Set( "sv_running", "0" );

	//Com_Printf( "---------------------------\n" );
}


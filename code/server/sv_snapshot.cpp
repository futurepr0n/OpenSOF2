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

#include "../client/vmachine.h"
#include "server.h"

// SOF2: entity pointer table accessor declared in sv_game.cpp
extern void SV_GetGEntityTable( void ***outTable, int *outCount );
extern qboolean SV_IsEntityArrayMode( int *outEntitySize );

/*
=============================================================================

Delta encode a client frame onto the network channel

A normal server packet will look like:

4	sequence number (high bit set if an oversize fragment)
<optional reliable commands>
1	svc_snapshot
4	last client reliable command
4	serverTime
1	lastframe for delta compression
1	snapFlags
1	areaBytes
<areabytes>
<playerstate>
<packetentities>

=============================================================================
*/

/*
=============
SV_EmitPacketEntities

Writes a delta update of an entityState_t list to the message.
=============
*/
static void SV_EmitPacketEntities( clientSnapshot_t *from, clientSnapshot_t *to, msg_t *msg ) {
	entityState_t	*oldent, *newent;
	int		oldindex, newindex;
	int		oldnum, newnum;
	int		from_num_entities;

	// generate the delta update
	if ( !from ) {
		from_num_entities = 0;
	} else {
		from_num_entities = from->num_entities;
	}

	newent = NULL;
	oldent = NULL;
	newindex = 0;
	oldindex = 0;
	const int num2Send = to->num_entities >= svs.numSnapshotEntities ? svs.numSnapshotEntities : to->num_entities;

	while ( newindex < num2Send || oldindex < from_num_entities ) {
		if ( newindex >= num2Send ) {
			newnum = 9999;
		} else {
			newent = &svs.snapshotEntities[(to->first_entity+newindex) % svs.numSnapshotEntities];
			newnum = newent->number;
		}

		if ( oldindex >= from_num_entities ) {
			oldnum = 9999;
		} else {
			oldent = &svs.snapshotEntities[(from->first_entity+oldindex) % svs.numSnapshotEntities];
			oldnum = oldent->number;
		}

		if ( newnum == oldnum ) {
			// delta update from old position
			// because the force parm is qfalse, this will not result
			// in any bytes being emited if the entity has not changed at all
			MSG_WriteEntity(msg, newent, 0);
			oldindex++;
			newindex++;
			continue;
		}

		if ( newnum < oldnum ) {
			// this is a new entity, send it from the baseline
			MSG_WriteEntity (msg, newent, 0);
			newindex++;
			continue;
		}

		if ( newnum > oldnum ) {
			// the old entity isn't present in the new message
			if(oldent) {
				MSG_WriteEntity (msg, NULL, oldent->number);
			}
			oldindex++;
			continue;
		}
	}

	MSG_WriteBits( msg, (MAX_GENTITIES-1), GENTITYNUM_BITS );	// end of packetentities
}



/*
==================
SV_WriteSnapshotToClient
==================
*/
static void SV_WriteSnapshotToClient( client_t *client, msg_t *msg ) {
	clientSnapshot_t	*frame, *oldframe;
	int					lastframe;
	int					snapFlags;

	// this is the snapshot we are creating
	frame = &client->frames[ client->netchan.outgoingSequence & PACKET_MASK ];

	// try to use a previous frame as the source for delta compressing the snapshot
	if ( client->deltaMessage <= 0 || client->state != CS_ACTIVE ) {
		// client is asking for a retransmit
		oldframe = NULL;
		lastframe = 0;
	} else if ( client->netchan.outgoingSequence - client->deltaMessage
		>= (PACKET_BACKUP - 3) ) {
		// client hasn't gotten a good message through in a long time
		Com_DPrintf ("%s: Delta request from out of date packet.\n", client->name);
		oldframe = NULL;
		lastframe = 0;
	} else {
		// we have a valid snapshot to delta from
		oldframe = &client->frames[ client->deltaMessage & PACKET_MASK ];
		lastframe = client->netchan.outgoingSequence - client->deltaMessage;

		// the snapshot's entities may still have rolled off the buffer, though
		if ( oldframe->first_entity <= svs.nextSnapshotEntities - svs.numSnapshotEntities ) {
			Com_DPrintf ("%s: Delta request from out of date entities.\n", client->name);
			oldframe = NULL;
			lastframe = 0;
		}
	}

	MSG_WriteByte (msg, svc_snapshot);

	// let the client know which reliable clientCommands we have received
	MSG_WriteLong( msg, client->lastClientCommand );

	// send over the current server time so the client can drift
	// its view of time to try to match
	MSG_WriteLong (msg, sv.time);

	// we must write a message number, because recorded demos won't have
	// the same network message sequences
	MSG_WriteLong (msg, client->netchan.outgoingSequence );
	MSG_WriteByte (msg, lastframe);				// what we are delta'ing from
	MSG_WriteLong (msg, client->cmdNum);		// we have executed up to here

	snapFlags = client->droppedCommands << 1;
	client->droppedCommands = qfalse;

	MSG_WriteByte (msg, snapFlags);

	// send over the areabits
	MSG_WriteByte (msg, frame->areabytes);
	MSG_WriteData (msg, frame->areabits, frame->areabytes);

	// SOF2 bridge fix: snapshots can stay in spectator-like pm state even after
	// command-path overrides. Normalize outgoing playerstate so cgame predicts
	// as an active player (proper eye height / player presentation paths).
	{
		static int s_snapFixLogCount = 0;
		if ( frame->ps.pm_type == 1 || frame->ps.pm_type == 2 ) {
			if ( s_snapFixLogCount < 24 ) {
				Com_Printf( "[SV snapfix] forcing snapshot pm_type %d->0 client=%s origin=(%.1f,%.1f,%.1f)\n",
					frame->ps.pm_type,
					client->name,
					frame->ps.origin[0], frame->ps.origin[1], frame->ps.origin[2] );
				++s_snapFixLogCount;
			}
			frame->ps.pm_type = 0;
		}
		if ( frame->ps.viewheight < 0 ) {
			frame->ps.viewheight = 38;
		}
	}

	// delta encode the playerstate
	if ( oldframe ) {
		MSG_WriteDeltaPlayerstate( msg, &oldframe->ps, &frame->ps );
	} else {
		MSG_WriteDeltaPlayerstate( msg, NULL, &frame->ps );
	}

	// delta encode the entities
	SV_EmitPacketEntities (oldframe, frame, msg);
}


/*
==================
SV_UpdateServerCommandsToClient

(re)send all server commands the client hasn't acknowledged yet
==================
*/
static void SV_UpdateServerCommandsToClient( client_t *client, msg_t *msg ) {
	int		i;

	// write any unacknowledged serverCommands
	for ( i = client->reliableAcknowledge + 1 ; i <= client->reliableSequence ; i++ ) {
		MSG_WriteByte( msg, svc_serverCommand );
		MSG_WriteLong( msg, i );
		MSG_WriteString( msg, client->reliableCommands[ i & (MAX_RELIABLE_COMMANDS-1) ] );
	}
}

/*
=============================================================================

Build a client snapshot structure

=============================================================================
*/

#define	MAX_SNAPSHOT_ENTITIES	1024
typedef struct {
	int		numSnapshotEntities;
	int		snapshotEntities[MAX_SNAPSHOT_ENTITIES];
} snapshotEntityNumbers_t;

/*
=======================
SV_QsortEntityNumbers
=======================
*/
static int SV_QsortEntityNumbers( const void *a, const void *b ) {
	int	*ea, *eb;

	ea = (int *)a;
	eb = (int *)b;

	if ( *ea == *eb ) {
		Com_Error( ERR_DROP, "SV_QsortEntityStates: duplicated entity" );
	}

	if ( *ea < *eb ) {
		return -1;
	}

	return 1;
}


/*
===============
SV_AddEntToSnapshot
===============
*/
static void SV_AddEntToSnapshot( svEntity_t *svEnt, gentity_t *gEnt, snapshotEntityNumbers_t *eNums ) {
	// if we have already added this entity to this snapshot, don't add again
	if ( svEnt->snapshotCounter == sv.snapshotCounter ) {
		return;
	}
	svEnt->snapshotCounter = sv.snapshotCounter;

	// if we are full, silently discard entities
	if ( eNums->numSnapshotEntities == MAX_SNAPSHOT_ENTITIES ) {
		return;
	}

	if (sv.snapshotCounter &1 && eNums->numSnapshotEntities == svs.numSnapshotEntities-1)
	{	//we're full, and about to wrap around and stomp ents, so half the time send the first set without stomping.
		return;
	}

	eNums->snapshotEntities[ eNums->numSnapshotEntities ] = SOF2_ENT_NUMBER(gEnt);
	eNums->numSnapshotEntities++;
}

//rww - bg_public.h won't cooperate in here
#define EF_PERMANENT			0x00080000

float sv_sightRangeForLevel[6] =
{
	0,//FORCE_LEVEL_0
    1024.f, //FORCE_LEVEL_1
	2048.0f,//FORCE_LEVEL_2
	4096.0f,//FORCE_LEVEL_3
	4096.0f,//FORCE_LEVEL_4
	4096.0f//FORCE_LEVEL_5
};

qboolean SV_PlayerCanSeeEnt( gentity_t *ent, int sightLevel )
{//return true if this ent is in view
	//NOTE: this is similar to the func CG_PlayerCanSeeCent in cg_players
	vec3_t viewOrg, viewAngles, viewFwd, dir2Ent;
	if ( !ent )
	{
		return qfalse;
	}
	if ( VM_Call( CG_CAMERA_POS, viewOrg))
	{
		if ( VM_Call( CG_CAMERA_ANG, viewAngles))
		{
			float dot = 0.25f;//1.0f;
			float range = sv_sightRangeForLevel[sightLevel];

			VectorSubtract( SOF2_ENT_CURORIGIN(ent), viewOrg, dir2Ent );
			float entDist = VectorNormalize( dir2Ent );

			if ( (SOF2_ENT_EFLAGS(ent)&EF_FORCE_VISIBLE) )
			{//no dist check on them?
			}
			else
			{
				if ( entDist < 128.0f )
				{//can always see them if they're really close
					return qtrue;
				}

				if ( entDist > range )
				{//too far away to see them
					return qfalse;
				}
			}

			dot += (0.99f-dot)*entDist/range;//the farther away they are, the more in front they have to be

			AngleVectors( viewAngles, viewFwd, NULL, NULL );
			if ( DotProduct( viewFwd, dir2Ent ) < dot )
			{
				return qfalse;
			}
			return qtrue;
		}
	}
	return qfalse;
}
/*
===============
SV_AddEntitiesVisibleFromPoint
===============
*/
static void SV_AddEntitiesVisibleFromPoint( vec3_t origin, clientSnapshot_t *frame,
									snapshotEntityNumbers_t *eNums, qboolean portal ) {
	int		e, i;
	gentity_t	*ent;
	svEntity_t	*svEnt;
	int		l;
	int		clientarea, clientcluster;
	int		leafnum;
	const byte *clientpvs;
	const byte *bitvector;
	// SOF2: sightOn removed (no Force powers)

	// during an error shutdown message we may need to transmit
	// the shutdown message after the server has shutdown, so
	// specfically check for it
	if ( !sv.state ) {
		return;
	}

	leafnum = CM_PointLeafnum (origin);
	clientarea = CM_LeafArea (leafnum);
	clientcluster = CM_LeafCluster (leafnum);

	// calculate the visible areas
	frame->areabytes = CM_WriteAreaBits( frame->areabits, clientarea );

	clientpvs = CM_ClusterPVS (clientcluster);

	// SOF2: no Force powers — sightOn stays qfalse
	// Per-entity snapshot log count — gate logging for ent=2, ent=46, ent=49 (door entities)
	static int s_entSnapLog[MAX_GENTITIES];  // zero-initialised at first call
	static bool s_entSnapLogInited = false;
	if ( !s_entSnapLogInited ) { memset(s_entSnapLog, 0, sizeof(s_entSnapLog)); s_entSnapLogInited = true; }

	for ( e = 0 ; e < MAX_GENTITIES ; e++ ) {
		ent = SV_GentityNum(e);

		// Per-entity debug logging for entity 2 (original debug target) and known door entities 46/49
		const qboolean logThis = (qboolean)((e == 2 || e == 46 || e == 49) && s_entSnapLog[e] < 8);

		if (!ent) {
			continue;	// SOF2: empty pointer table slot
		}

		if (SOF2_ENT_EFLAGS(ent) & EF_PERMANENT)
		{	// he's permanent, so don't send him down!
			if ( logThis ) {
				Com_Printf(
					"[SV snapshot] ent=%d skip=permanent flags=0x%x model=%d solid=0x%x linked=%d\n",
					e, SOF2_ENT_EFLAGS( ent ),
					SOF2_ENT_MODELINDEX( ent ),
					SOF2_ENT_SOLID( ent ),
					(int)SOF2_ENT_LINKED( ent ) );
				++s_entSnapLog[e];
			}
			continue;
		}

		if (SOF2_ENT_NUMBER(ent) != e) {
			Com_DPrintf ("FIXING ENT->S.NUMBER!!!\n");
			SOF2_ENT_NUMBER(ent) = e;
		}

		// never send entities that aren't linked in
		if ( !SOF2_ENT_LINKED(ent) ) {
			if ( logThis ) {
				Com_Printf(
					"[SV snapshot] ent=%d skip=unlinked flags=0x%x model=%d solid=0x%x\n",
					e, SOF2_ENT_EFLAGS( ent ),
					SOF2_ENT_MODELINDEX( ent ),
					SOF2_ENT_SOLID( ent ) );
				++s_entSnapLog[e];
			}
			continue;
		}

		// entities can be flagged to explicitly not be sent to the client
		if ( SOF2_ENT_SVFLAGS(ent) & SVF_NOCLIENT ) {
			if ( logThis ) {
				Com_Printf(
					"[SV snapshot] ent=%d skip=noclient svf=0x%x model=%d solid=0x%x\n",
					e, SOF2_ENT_SVFLAGS( ent ),
					SOF2_ENT_MODELINDEX( ent ),
					SOF2_ENT_SOLID( ent ) );
				++s_entSnapLog[e];
			}
			continue;
		}

		svEnt = SV_SvEntityForGentity( ent );
		if ( !svEnt ) {
			if ( logThis ) {
				Com_Printf(
					"[SV snapshot] ent=%d skip=no-svent model=%d solid=0x%x linked=%d\n",
					e, SOF2_ENT_MODELINDEX( ent ),
					SOF2_ENT_SOLID( ent ),
					(int)SOF2_ENT_LINKED( ent ) );
				++s_entSnapLog[e];
			}
			continue;
		}

		// don't double add an entity through portals
		if ( svEnt->snapshotCounter == sv.snapshotCounter ) {
			if ( logThis ) {
				Com_Printf(
					"[SV snapshot] ent=%d skip=already-added model=%d solid=0x%x snapshotCounter=%d\n",
					e, SOF2_ENT_MODELINDEX( ent ),
					SOF2_ENT_SOLID( ent ),
					svEnt->snapshotCounter );
				++s_entSnapLog[e];
			}
			continue;
		}

		// broadcast entities are always sent, and so is the main player so we don't see noclip weirdness
		if ( SOF2_ENT_SVFLAGS(ent) & SVF_BROADCAST || !e) {
			SV_AddEntToSnapshot( svEnt, ent, eNums );
			continue;
		}

#if 0 // SOF2: isPortalEnt doesn't exist in SOF2's entityState_t
		if (ent->s.isPortalEnt)
		{ //rww - portal entities are always sent as well
			SV_AddEntToSnapshot( svEnt, ent, eNums );
			continue;
		}
#endif

		// SOF2: no Force sight — skip force-see entity visibility check

		// ignore if not touching a PV leaf
		// check area. SOF2 regularly lands the eye point in area -1 while the
		// rest of the visibility path still treats that as "don't area-cull".
		// Keep the stricter connected-area test only when the client's leaf has
		// a valid area.
		if ( clientarea >= 0 ) {
			if ( !CM_AreasConnected( clientarea, svEnt->areanum ) ) {
				// doors can legally straddle two areas, so
				// we may need to check another one
				if ( !CM_AreasConnected( clientarea, svEnt->areanum2 ) ) {
					if ( logThis ) {
						Com_Printf(
							"[SV snapshot] ent=%d skip=area clientArea=%d area1=%d area2=%d model=%d solid=0x%x\n",
							e, clientarea,
							svEnt->areanum,
							svEnt->areanum2,
							SOF2_ENT_MODELINDEX( ent ),
							SOF2_ENT_SOLID( ent ) );
						++s_entSnapLog[e];
					}
					continue;		// blocked by a door
				}
			}
		} else if ( logThis ) {
			Com_Printf(
				"[SV snapshot] ent=%d allow area-skip clientArea=%d area1=%d area2=%d model=%d solid=0x%x\n",
				e, clientarea,
				svEnt->areanum,
				svEnt->areanum2,
				SOF2_ENT_MODELINDEX( ent ),
				SOF2_ENT_SOLID( ent ) );
			++s_entSnapLog[e];
		}

		bitvector = clientpvs;

		// check individual leafs
		if ( !svEnt->numClusters ) {
			if ( logThis ) {
				Com_Printf(
					"[SV snapshot] ent=%d skip=no-clusters area1=%d area2=%d model=%d solid=0x%x\n",
					e, svEnt->areanum,
					svEnt->areanum2,
					SOF2_ENT_MODELINDEX( ent ),
					SOF2_ENT_SOLID( ent ) );
				++s_entSnapLog[e];
			}
			continue;
		}
		l = 0;

		for ( i=0 ; i < svEnt->numClusters ; i++ ) {
			l = svEnt->clusternums[i];
			if ( bitvector[l >> 3] & (1 << (l&7) ) ) {
				break;
			}
		}

		// if we haven't found it to be visible,
		// check overflow clusters that coudln't be stored
		if ( i == svEnt->numClusters ) {
			if ( svEnt->lastCluster ) {
				for ( ; l <= svEnt->lastCluster ; l++ ) {
					if ( bitvector[l >> 3] & (1 << (l&7) ) ) {
						break;
					}
				}
				if ( l == svEnt->lastCluster ) {
					if ( logThis ) {
						Com_Printf(
							"[SV snapshot] ent=%d skip=pvs cluster=%d lastCluster=%d numClusters=%d model=%d solid=0x%x\n",
							e,
							clientcluster,
							svEnt->lastCluster,
							svEnt->numClusters,
							SOF2_ENT_MODELINDEX( ent ),
							SOF2_ENT_SOLID( ent ) );
						++s_entSnapLog[e];
					}
					continue;		// not visible
				}
			} else {
				if ( logThis ) {
					Com_Printf(
						"[SV snapshot] ent=%d skip=pvs-nooverflow cluster=%d numClusters=%d model=%d solid=0x%x\n",
						e,
						clientcluster,
						svEnt->numClusters,
						SOF2_ENT_MODELINDEX( ent ),
						SOF2_ENT_SOLID( ent ) );
					++s_entSnapLog[e];
				}
				continue;
			}
		}

		// add it
		if ( logThis ) {
			Com_Printf(
				"[SV snapshot] ent=%d add area1=%d area2=%d numClusters=%d model=%d solid=0x%x\n",
				e,
				svEnt->areanum,
				svEnt->areanum2,
				svEnt->numClusters,
				SOF2_ENT_MODELINDEX( ent ),
				SOF2_ENT_SOLID( ent ) );
			++s_entSnapLog[e];
		}
		SV_AddEntToSnapshot( svEnt, ent, eNums );

		// if its a portal entity, add everything visible from its camera position
		if ( SOF2_ENT_SVFLAGS(ent) & SVF_PORTAL ) {
			SV_AddEntitiesVisibleFromPoint( SOF2_ENT_S_ORIGIN2(ent), frame, eNums, qtrue );
		}
	}
}

/*
=============
SV_BuildClientSnapshot

Decides which entities are going to be visible to the client, and
copies off the playerstate and areabits.

This properly handles multiple recursive portals, but the render
currently doesn't.

For viewing through other player's eyes, clent can be something other than client->gentity
=============
*/
// Log first 2 snapshots: header per snapshot + every entity included.
// Identifies beam source and any crash-inducing eType combinations.
static int s_snapLogSnaps = 0;      // snapshots fully logged so far
static int s_snapLogInSnap = 0;     // 1 while logging the current snapshot

static clientSnapshot_t *SV_BuildClientSnapshot( client_t *client ) {
	vec3_t						org;
	clientSnapshot_t			*frame;
	snapshotEntityNumbers_t		entityNumbers;
	int							i;
	int							clientNum;
	gentity_t					*ent;
	playerState_t				*clPS;
	entityState_t				*state;
	gentity_t					*clent;

	// bump the counter used to prevent double adding
	sv.snapshotCounter++;

	// this is the frame we are creating
	frame = &client->frames[ client->netchan.outgoingSequence & PACKET_MASK ];

	// clear everything in this snapshot
	entityNumbers.numSnapshotEntities = 0;
	memset( frame->areabits, 0, sizeof( frame->areabits ) );

	clent = client->gentity;
	if ( !clent ) {
		return frame;
	}

	// grab the current playerState_t
	// SOF2: client data is in a separate array passed via LocateGameData (not embedded in CEntity)
	clientNum = client - svs.clients;
	clPS = SV_GameClientNum( clientNum );
	{
		playerState_t *ps = clPS;
		if ( !ps ) {
			return frame;
		}
		frame->ps = *ps;
	}

	// find the client's viewpoint
	//if in camera mode use camera position instead
	if ( VM_Call( CG_CAMERA_POS, org))
	{
		int leafnum = CM_PointLeafnum( org );
		int area = CM_LeafArea( leafnum );
		int cluster = CM_LeafCluster( leafnum );
		static int s_snapshotViewFallbackLogCount = 0;

		if ( ( area < 0 || cluster < 0 ) && clPS ) {
			vec3_t cameraOrg;

			VectorCopy( org, cameraOrg );
			VectorCopy( clPS->origin, org );
			org[2] += clPS->viewheight;

			if ( s_snapshotViewFallbackLogCount < 32 ) {
				Com_Printf(
					"[SV snapshot] fallback vieworg camera=(%.1f %.1f %.1f) leaf=%d area=%d cluster=%d -> player=(%.1f %.1f %.1f)\n",
					cameraOrg[0], cameraOrg[1], cameraOrg[2],
					leafnum, area, cluster,
					org[0], org[1], org[2] );
				++s_snapshotViewFallbackLogCount;
			}
		}
	}
	else
	{
		if ( clPS ) {
			VectorCopy( clPS->origin, org );
			org[2] += clPS->viewheight;
			// SOF2: no lean mechanic — basic origin + viewheight is sufficient
		} else {
			VectorClear( org );
		}
	}
	// SOF2: no serverViewOrg in playerState_t — skip copy

	// One-time entity dump per map load: trajectory + ghoul2 data for all linked entities.
	// Writes to Debug/entity_dump.txt for diagnosing ghost rotation and invisible NPCs.
#ifdef _DEBUG
	{
		static int dumpServerId = -1;
		if ( dumpServerId != sv.serverId ) {
			dumpServerId = sv.serverId;
			void **tbl = NULL; int numEnts = 0;
			int entityStride = 0;
			qboolean arrayMode = SV_IsEntityArrayMode( &entityStride );
			SV_GetGEntityTable( &tbl, &numEnts );
			FILE *df = fopen( "entity_dump.txt", "w" );
			if ( df ) {
				if ( tbl && numEnts > 0 ) {
					fprintf( df, "=== entity dump serverId=%d numEnts=%d arrayMode=%d stride=%d ===\n", sv.serverId, numEnts, (int)arrayMode, entityStride );
					for ( int ei = 0; ei < numEnts; ++ei ) {
						void *ep;
						if ( arrayMode && entityStride > 0 ) {
							ep = (void *)((byte *)tbl + (size_t)ei * entityStride);
						} else {
							ep = tbl[ei];
						}
						if ( !ep ) continue;
						const byte linked = *(const byte *)((const byte *)ep + 0x108);
						if ( !linked ) continue;  // skip unlinked
						const entityState_t *es = (const entityState_t *)((const byte *)ep + 8);
						const int svf  = *(const int *)((const byte *)ep + 0x114);
						// Log entities with model, ghoul2 handle, or non-stationary trajectory
						const int hasMdl   = es->modelindex || es->soundSetIndex;
						const int hasTraj  = (es->pos.trType != TR_STATIONARY) || (es->apos.trType != TR_STATIONARY);
						if ( !hasMdl && !hasTraj ) continue;
						fprintf( df,
							"ei=%d et=%d ef=0x%x svf=0x%x mdl=%d g2=%d"
							" pos.tr=%d base=(%.1f,%.1f,%.1f) delta=(%.3f,%.3f,%.3f)"
							" apos.tr=%d base=(%.1f,%.1f,%.1f) delta=(%.3f,%.3f,%.3f)\n",
							ei, es->eType, es->eFlags, svf,
							es->modelindex, es->soundSetIndex,
							(int)es->pos.trType,
							es->pos.trBase[0], es->pos.trBase[1], es->pos.trBase[2],
							es->pos.trDelta[0], es->pos.trDelta[1], es->pos.trDelta[2],
							(int)es->apos.trType,
							es->apos.trBase[0], es->apos.trBase[1], es->apos.trBase[2],
							es->apos.trDelta[0], es->apos.trDelta[1], es->apos.trDelta[2] );
					}
					fprintf( df, "=== end dump ===\n" );
				}
				fclose( df );
			}
		}
	}
#endif // _DEBUG

	// add all the entities directly visible to the eye, which
	// may include portal entities that merge other viewpoints
	SV_AddEntitiesVisibleFromPoint( org, frame, &entityNumbers, qfalse );
	/*
	//was in here for debugging- print list of all entities in snapshot when you go over the limit
	if ( entityNumbers.numSnapshotEntities >= 256 )
	{
		for ( int xxx = 0; xxx < entityNumbers.numSnapshotEntities; xxx++ )
		{
			Com_Printf("%d - ", xxx );
			ge->PrintEntClassname( entityNumbers.snapshotEntities[xxx] );
		}
	}
	else if ( entityNumbers.numSnapshotEntities >= 200 )
	{
		Com_Printf(S_COLOR_RED"%d snapshot entities!", entityNumbers.numSnapshotEntities );
	}
	else if ( entityNumbers.numSnapshotEntities >= 128 )
	{
		Com_Printf(S_COLOR_YELLOW"%d snapshot entities", entityNumbers.numSnapshotEntities );
	}
	*/

	// if there were portals visible, there may be out of order entities
	// in the list which will need to be resorted for the delta compression
	// to work correctly.  This also catches the error condition
	// of an entity being included twice.
	qsort( entityNumbers.snapshotEntities, entityNumbers.numSnapshotEntities,
		sizeof( entityNumbers.snapshotEntities[0] ), SV_QsortEntityNumbers );

	// now that all viewpoint's areabits have been OR'd together, invert
	// all of them to make it a mask vector, which is what the renderer wants
	for ( i = 0 ; i < MAX_MAP_AREA_BYTES/4 ; i++ ) {
		((int *)frame->areabits)[i] = ((int *)frame->areabits)[i] ^ -1;
	}

	// copy the entity states out
	frame->num_entities = 0;
	frame->first_entity = svs.nextSnapshotEntities;
	if ( s_snapLogSnaps < 2 ) {
		s_snapLogInSnap = 1;
		Com_Printf( "[SNAP] === snap %d start (%d candidates) ===\n",
			s_snapLogSnaps, entityNumbers.numSnapshotEntities );
	}
	for ( i = 0 ; i < entityNumbers.numSnapshotEntities ; i++ ) {
		ent = SV_GentityNum(entityNumbers.snapshotEntities[i]);
		if (!ent) continue;	// SOF2: empty pointer table slot
		state = &svs.snapshotEntities[svs.nextSnapshotEntities % svs.numSnapshotEntities];
		// SOF2: entityState_t is 256 bytes at CEntity offset 8
		memcpy( state, SOF2_ENT_S_PTR(ent), SOF2_ENTITYSTATE_SIZE );
		// Guard invalid trType — DLL ICARUS scripts may spawn entities with
		// uninitialized trajectory data. Invalid trType causes ERR_DROP in
		// cgamex86.dll's BG_EvaluateTrajectory (switch default).
		if ( (unsigned)state->pos.trType  > TR_GRAVITY ) state->pos.trType  = TR_STATIONARY;
		if ( (unsigned)state->apos.trType > TR_GRAVITY ) state->apos.trType = TR_STATIONARY;
		// JK2 added TR_NONLINEAR_STOP=4 between TR_LINEAR_STOP and TR_SINE, shifting
		// TR_SINE from 4→5 and TR_GRAVITY from 5→6.  SOF2 (Q3A-based) has no
		// TR_NONLINEAR_STOP; its enum is TR_LINEAR_STOP=3, TR_SINE=4, TR_GRAVITY=5.
		// Translate JK2 trType values >=4 down by 1 so native SOF2 cgame sees correct type:
		//   JK2 TR_NONLINEAR_STOP(4) → SOF2 TR_LINEAR_STOP(3)  stops at endpoint, no oscillation
		//   JK2 TR_SINE(5)           → SOF2 TR_SINE(4)         sinusoidal movers work correctly
		//   JK2 TR_GRAVITY(6)        → SOF2 TR_GRAVITY(5)      gravity-affected entities work
		// Without this, TR_NONLINEAR_STOP doors are interpreted as TR_SINE (oscillating),
		// causing movers and breakable walls to flash between start and end positions.
		if ( (int)state->pos.trType  >= 4 ) state->pos.trType  = (trType_t)((int)state->pos.trType  - 1);
		if ( (int)state->apos.trType >= 4 ) state->apos.trType = (trType_t)((int)state->apos.trType - 1);
		// SOF2 entity-type translation: JK2 and native SOF2 cgame disagree on the
		// eType numbering.  JK2: ET_MISSILE=3, ET_MOVER=4.
		// Native SOF2 CG_AddEntity dispatch: case 3=CG_Mover, case 4=CG_General.
		// Without remapping:
		//   ET_MOVER(4) → CG_General, which cannot render BSP inline models → invisible doors
		//   ET_MISSILE(3) → CG_Mover, wrong path for projectiles
		// With remapping:
		//   JK2 ET_MISSILE(3) → SOF2 eType 2 → CG_Missile  (cgs_gameModels[modelindex])
		//   JK2 ET_MOVER(4) BSP model → SOF2 eType 3 → CG_Mover (cgs_inlineDrawModel + SOLID_BMODEL)
		//   JK2 ET_MOVER(4) GLM model → SOF2 eType 0 → CG_General (engine-side static GLM system)
		if ( state->eType == 4 ) {
			// Decide whether this ET_MOVER is a BSP brush mover or a model-asset mover.
			//
			// Model-asset movers (vehicle/prop GLM or MD3, misc_model_breakable, etc.)
			// need remapping to ET_GENERAL so CG_General or the engine static-GLM system
			// can render them from the registered file model.
			//
			// Pure BSP brush movers (func_wall, func_door, etc.) stay as SOF2 ET_MOVER(3)
			// so native CG_Mover can look up the BSP inline model.
			//
			// Discriminator: SOF2 configstring slot 34+N (CG_ConfigString(N+0x22) in native
			// cgame).  Only written by SOF2_SpawnStaticGlm / SP_model_static / SOF2_SpawnPickup
			// - entities that explicitly register for SOF2 rendering.  Pure BSP movers never
			// touch this slot, so there are no false positives regardless of solid value.
			static int modelMoverLogCount = 0;
			qboolean isModelAsset = qfalse;
			const int svFlags = (int)SOF2_ENT_SVFLAGS(ent);
			if ( state->modelindex > 0 && state->modelindex < 256 ) {
				const int sof2CsIdx = 34 + state->modelindex; // SOF2 cgame slot
				if ( sof2CsIdx < MAX_CONFIGSTRINGS && sv.configstrings[sof2CsIdx] ) {
					const char *mpath = sv.configstrings[sof2CsIdx];
					const int mlen = (int)strlen( mpath );
					if ( mlen >= 4 &&
						( Q_stricmp( mpath + mlen - 4, ".glm" ) == 0 ||
						  Q_stricmp( mpath + mlen - 4, ".md3" ) == 0 ) )
					    isModelAsset = qtrue;
					if ( isModelAsset && modelMoverLogCount < 16 ) {
						Com_Printf( "[SNAP] ET_MOVER ent=%d solid=0x%x svf=0x%x model='%s' -> ET_GENERAL\n\n",
							state->number, state->solid, svFlags, mpath );
						++modelMoverLogCount;
					}
				}
			}
			state->eType = isModelAsset ? 0 : 3;
		} else if ( state->eType == 3 ) {
			state->eType = 2;
		}
		// ET_BEAM (eType=5): JK2 target_laser entities.  Native SOF2 CG_Beam renders them
		// as thick blue lines with wrong material pointing toward world origin.
		// Suppress from snapshots — entity still functions server-side (trace, damage).
		if ( state->eType == 5 ) {
			continue;
		}
		// Native SOF2 CG_AddEntity switch has no case for eType=9 (ET_TELEPORT_TRIGGER)
		// or eType>=15 (G_TempEntity event entities: ET_EVENTS+N, N>=1).
		// Both hit the default: cgi_Error(0,"Bad entity type: %i") → ERR_DROP client crash.
		// Event temp entities are created by G_TempEntity for sounds, effects, muzzle flashes,
		// footsteps, etc. — they carry no visual geometry and can be safely suppressed.
		// ET_TELEPORT_TRIGGER(9) entities are invisible triggers that don't need rendering.
		if ( state->eType == 9 || state->eType >= 15 ) {
			continue;  // discard: would crash native SOF2 cgame
		}
		if ( s_snapLogInSnap ) {
			Com_Printf( "[SNAP] ent=%d et=%d ef=0x%x mdl=%d solid=0x%x pos.tr=%d apos.tr=%d\n",
				state->number, state->eType, state->eFlags, state->modelindex, state->solid,
				(int)state->pos.trType, (int)state->apos.trType );
		}
		// Extra door-entity detail log (entities 46 & 49) — first 16 appearances each
		{
			static int s_doorLog46 = 0, s_doorLog49 = 0;
			int *pDoorLog = ( state->number == 46 ) ? &s_doorLog46
			              : ( state->number == 49 ) ? &s_doorLog49 : NULL;
			if ( pDoorLog && *pDoorLog < 16 ) {
				Com_Printf( "[DOOR] ent=%d eType=%d ef=0x%x pos.tr=%d trBase=(%.0f,%.0f,%.0f) trDelta=(%.0f,%.0f,%.0f) trTime=%d trDur=%d\n",
					state->number, state->eType, state->eFlags, (int)state->pos.trType,
					state->pos.trBase[0], state->pos.trBase[1], state->pos.trBase[2],
					state->pos.trDelta[0], state->pos.trDelta[1], state->pos.trDelta[2],
					state->pos.trTime, state->pos.trDuration );
				++(*pDoorLog);
			}
		}
		svs.nextSnapshotEntities++;
		frame->num_entities++;
	}
	if ( s_snapLogInSnap ) {
		Com_Printf( "[SNAP] --- end snap %d, %d entities ---\n", s_snapLogSnaps, frame->num_entities );
		s_snapLogInSnap = 0;
		++s_snapLogSnaps;
	}

	return frame;
}


/*
=======================
SV_SendMessageToClient

Called by SV_SendClientSnapshot and SV_SendClientGameState
=======================
*/
#define	HEADER_RATE_BYTES	48		// include our header, IP header, and some overhead
void SV_SendMessageToClient( msg_t *msg, client_t *client ) {
	// record information about the message
	client->frames[client->netchan.outgoingSequence & PACKET_MASK].messageSize = msg->cursize;
	client->frames[client->netchan.outgoingSequence & PACKET_MASK].messageSent = sv.time;

	// send the datagram (SOF2: apply XOR encryption layer)
	SV_Netchan_Transmit( client, msg );
}

/*
=======================
SV_SendClientEmptyMessage

This is just an empty message so that we can tell if
the client dropped the gamestate that went out before
=======================
*/
void SV_SendClientEmptyMessage( client_t *client ) {
	msg_t	msg;
	byte	buffer[10];

	MSG_Init( &msg, buffer, sizeof( buffer ) );
	SV_SendMessageToClient( &msg, client );
}

/*
=======================
SV_SendClientSnapshot
=======================
*/
void SV_SendClientSnapshot( client_t *client ) {
	byte		msg_buf[MAX_MSGLEN];
	msg_t		msg;

	// build the snapshot
	SV_BuildClientSnapshot( client );

	// bots need to have their snapshots build, but
	// the query them directly without needing to be sent
	if ( client->gentity && SOF2_ENT_SVFLAGS(client->gentity) & SVF_BOT ) {
		return;
	}

	MSG_Init (&msg, msg_buf, sizeof(msg_buf));
	msg.allowoverflow = qtrue;

	// (re)send any reliable server commands
	SV_UpdateServerCommandsToClient( client, &msg );

	// send over all the relevant entityState_t
	// and the playerState_t
	SV_WriteSnapshotToClient( client, &msg );

	// check for overflow
	if ( msg.overflowed ) {
		Com_Printf ("WARNING: msg overflowed for %s\n", client->name);
		MSG_Clear (&msg);
	}

	SV_SendMessageToClient( &msg, client );
}


/*
=======================
SV_SendClientMessages
=======================
*/
void SV_SendClientMessages( void ) {
	int			i;
	client_t	*c;

	// send a message to each connected client
	for (i=0, c = svs.clients ; i < 1 ; i++, c++) {
		if (!c->state) {
			continue;		// not connected
		}

		if ( c->state != CS_ACTIVE ) {
			if ( c->state != CS_ZOMBIE ) {
				SV_SendClientEmptyMessage( c );
			}
			continue;
		}

		SV_SendClientSnapshot( c );
	}
}


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

#include "server.h"
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
// SOF2: entity array and stride are stored here when game DLL calls gi.LocateGameData
static void *sv_GEntities   = NULL;
static int   sv_GEntitySize = 0;   // filled in once we know game DLL's gentity_t size

gentity_t	*SV_GentityNum( int num ) {
	assert(num >= 0);
	if ( !sv_GEntities || sv_GEntitySize <= 0 ) {
		return NULL;
	}
	return (gentity_t *)((byte *)sv_GEntities + sv_GEntitySize * num);
}

svEntity_t	*SV_SvEntityForGentity( gentity_t *gEnt ) {
	if ( !gEnt || gEnt->s.number < 0 || gEnt->s.number >= MAX_GENTITIES ) {
		Com_Error( ERR_DROP, "SV_SvEntityForGentity: bad gEnt" );
	}
	return &sv.svEntities[ gEnt->s.number ];
}

gentity_t	*SV_GEntityForSvEntity( svEntity_t *svEnt ) {
	int		num;

	num = svEnt - sv.svEntities;
	return SV_GentityNum( num );
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
		Com_Error( ERR_DROP, "SV_SetBrushModel: NULL model for ent number %d", ent->s.number );
	}

	if (name[0] == '*')
	{
		ent->s.modelindex = atoi( name + 1 );

		if (sv.mLocalSubBSPIndex != -1)
		{
			ent->s.modelindex += sv.mLocalSubBSPModelOffset;
		}

		h = CM_InlineModel( ent->s.modelindex );

		if (sv.mLocalSubBSPIndex != -1)
		{
			CM_ModelBounds( SubBSP[sv.mLocalSubBSPIndex], h, mins, maxs );
		}
		else
		{
			CM_ModelBounds( cmg, h, mins, maxs);
		}

		//CM_ModelBounds( h, mins, maxs );

		VectorCopy (mins, ent->mins);
		VectorCopy (maxs, ent->maxs);
		ent->bmodel = qtrue;

		ent->contents = CM_ModelContents( h, -1 );
	}
	else if (name[0] == '#')
	{
		ent->s.modelindex = CM_LoadSubBSP(va("maps/%s.bsp", name + 1), qfalse);
		CM_ModelBounds( SubBSP[CM_FindSubBSP(ent->s.modelindex)], ent->s.modelindex, mins, maxs );

		VectorCopy (mins, ent->mins);
		VectorCopy (maxs, ent->maxs);
		ent->bmodel = qtrue;

		//rwwNOTE: We don't ever want to set contents -1, it includes CONTENTS_LIGHTSABER.
		//Lots of stuff will explode if there's a brush with CONTENTS_LIGHTSABER that isn't attached to a client owner.
		//ent->contents = -1;		// we don't know exactly what is in the brushes
		h = CM_InlineModel( ent->s.modelindex );
		ent->contents = CM_ModelContents( h, CM_FindSubBSP(ent->s.modelindex) );
	//	ent->contents = CONTENTS_SOLID;
	}
	else
	{
		Com_Error( ERR_DROP, "SV_SetBrushModel: %s isn't a brush model (ent %d)", name, ent->s.number );
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
	if ( !(ent->contents & CONTENTS_OPAQUE) ) {
#ifndef FINAL_BUILD
//		Com_Printf( "INFO: entity number %d not opaque: not affecting area portal!\n", ent->s.number );
#endif
		return;
	}
#endif

	svEntity_t	*svEnt;

	svEnt = SV_SvEntityForGentity( ent );
	if ( svEnt->areanum2 == -1 ) {
		return;
	}
	CM_AdjustAreaPortalState( svEnt->areanum, svEnt->areanum2, open );
}


/*
==================
SV_GameAreaEntities
==================
*/
qboolean	SV_EntityContact( const vec3_t mins, const vec3_t maxs, const gentity_t *gEnt ) {
	const float	*origin, *angles;
	clipHandle_t	ch;
	trace_t			trace;

	// check for exact collision
	origin = gEnt->currentOrigin;
	angles = gEnt->currentAngles;

	ch = SV_ClipHandleForEntity( gEnt );
	CM_TransformedBoxTrace ( &trace, vec3_origin, vec3_origin, mins, maxs,
		ch, -1, origin, angles );

	return trace.startsolid;
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

	if (sv.mLocalSubBSPIndex == -1)
	{
		s = COM_Parse( (const char **)&sv.entityParsePoint );
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
	ge->Shutdown(0);

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

// SV_DPrintf removed — use Com_DPrintf directly (it checks com_developer internally)

// slot 25: Cvar_SetValue(name, val, force) — SOF2 adds 'force' param; engine ignores it
static void SV_Cvar_SetValue_Wrapper( const char *name, float val, int /*force*/ ) {
	Cvar_SetValue( name, val );
}

// slot 36: CM_FreeTerrain — RMG terrain cleanup (no-op in SP retail)
static void SV_CM_FreeTerrain( int /*terrainId*/ ) {}

// slot 38: RMG_Init
static void SV_RMG_Init( int /*terrainId*/ ) {}

// slot 40: RMG_GetSpawnPoint
static void SV_RMG_GetSpawnPoint( int /*terrainId*/, float *pos ) {
	if (pos) VectorClear( pos );
}

// slot 41: RMG_GetCellInfo
static int SV_RMG_GetCellInfo( int /*terrainId*/, int /*x*/, int /*y*/ ) { return 0; }

// slot 43: RMG_UpdateTerrain
static void SV_RMG_UpdateTerrain( int /*terrainId*/ ) {}

// slots 52-53: SaveGame chunk I/O — wraps ojk SavedGame helper
static void SV_SaveGame_WriteChunk( int chunkId, void *data, int length ) {
	ojk::SavedGameHelper( &ojk::SavedGame::get_instance() ).write_chunk(
		chunkId, static_cast<const uint8_t *>(data), length );
}
static void SV_SaveGame_ReadChunk( int chunkId, void *data, int length ) {
	ojk::SavedGameHelper( &ojk::SavedGame::get_instance() ).read_chunk(
		chunkId, static_cast<uint8_t *>(data), length );
}

// slot 54: LocateGameData — SOF2 only passes entity array base pointer
// Entity stride is communicated separately (or known at compile time in the game DLL).
// We store the pointer here; sv_GEntitySize is set by SV_SetGameEntitySize() if available.
static void SV_LocateGameData( void *gentities ) {
	sv_GEntities = gentities;
}

// slot 59: MulticastTempEntity
static void SV_MulticastTempEntity( gentity_t * /*ent*/ ) {}

// slot 60: InitTempEntFinalize
static void SV_InitTempEntFinalize( gentity_t * /*ent*/ ) {}

// slot 61: GetTempEntCount
static int SV_GetTempEntCount( void ) { return 0; }

// slot 63: ICARUS_PlaySound
static void SV_ICARUS_PlaySound( int /*entID*/, const char * /*name*/, const char * /*channel*/ ) {}

// slot 64: ICARUS_RunScript
static int SV_ICARUS_RunScript( gentity_t * /*ent*/, const char * /*script*/ ) { return 0; }

// slot 65: GetCurrentEntity
static gentity_t *SV_GetCurrentEntity( void ) { return NULL; }

// slot 67: GetLastErrorString
static char *SV_GetLastErrorString( void ) {
	static char buf[64];
	buf[0] = '\0';
	return buf;
}

// slot 68: GetCurrentEntityIndirect
static gentity_t *SV_GetCurrentEntityIndirect( void ) { return NULL; }

// slot 50: trace — 7-arg wrapper for SV_Trace (which has 2 extra defaulted G2 params)
static void SV_Trace_7args( trace_t *results, const vec3_t start,
		const vec3_t mins, const vec3_t maxs, const vec3_t end,
		int passEnt, int contentmask ) {
	SV_Trace( results, start, mins, maxs, end, passEnt, contentmask );
}

// slot 71: traceG2 — ignores extra G2 args, forwards to regular trace
static void SV_TraceG2( trace_t *results, const vec3_t start,
		const vec3_t mins, const vec3_t maxs, const vec3_t end,
		int passEnt, int contentmask, int /*g2TraceType*/, int /*useLod*/ ) {
	SV_Trace( results, start, mins, maxs, end, passEnt, contentmask );
}

// slot 72: GetEntityBoundsSize
static int SV_GetEntityBoundsSize( gentity_t * /*ent*/ ) { return 0; }

// slot 83: GetSurfaceMaterial
static int SV_GetSurfaceMaterial( int /*surfIndex*/, float * /*vel*/ ) { return 0; }

// slot 84: ModelIndex — server-side model precaching (stub; Phase 2 will use CM_RegisterModel)
static int SV_ModelIndex_Wrapper( const char * /*name*/ ) {
	return 0;
}

// slot 90: G2API_GetGhoul2InfoV
static CGhoul2Info_v *SV_G2API_GetGhoul2InfoV( int /*ghoul2handle*/ ) { return NULL; }

// slot 91: G2API_CleanGhoul2ModelsRef — via pointer rather than reference
static void SV_G2API_CleanGhoul2ModelsRef( CGhoul2Info_v *ghoul2ref ) {
	if ( ghoul2ref ) re.G2API_CleanGhoul2Models( *ghoul2ref );
}

// slot 93: SE_GetString wrapper — SOF2 has (token, flags); engine's SE_GetString takes (key)
static char *SV_SE_GetString_Wrapper( const char *token, int /*flags*/ ) {
	return (char *)SE_GetString( token );
}

// slot 94: SoundIndex — server-side sound precaching (stub; Phase 2 will register with sound system)
static int SV_SoundIndex_Wrapper( const char * /*name*/ ) {
	return 0;
}

// slot 95: SE_SetStringSoundMap
static void SV_SE_SetStringSoundMap( const char * /*token*/, int /*idx*/ ) {}

// slot 97: CM_RegisterDamageShader
static int SV_CM_RegisterDamageShader( const char * /*name*/ ) { return 0; }

// slot 98: WE_GetWindVector — existing SV_WE_GetWindVector returns bool; need void wrapper
static void SV_WE_GetWindVectorVoid( vec3_t windVector, vec3_t atPoint ) {
	SV_WE_GetWindVector( windVector, atPoint );
}

// slot 99: SV_UpdateEntitySoundIndex (leading underscore avoids collision with engine symbol)
static void SV_UpdateEntitySoundIndex_( int /*entNum*/ ) {}

// slot 100: SoundDuration
static int SV_SoundDuration( const char * /*name*/ ) { return 0; }

// slot 101: SetMusicState
static void SV_SetMusicState( int /*state*/ ) {}

// slots 102-110: RMG automap drawing stubs
static void SV_RMG_AddBreakpoint( int /*terrainId*/, void * /*data*/ ) {}
static void SV_GameShutdown( void ) {}
static void SV_RMG_AutomapDrawLine( int /*x1*/, int /*y1*/, int /*x2*/, int /*y2*/, int /*color*/ ) {}
static void SV_RMG_AutomapDrawRegion( int /*x*/, int /*y*/, int /*w*/, int /*h*/, int /*color*/ ) {}
static void SV_RMG_AutomapDrawCircle( int /*x*/, int /*y*/, int /*radius*/, int /*color*/ ) {}
static void SV_RMG_AutomapDrawIcon( int /*x*/, int /*y*/, int /*icon*/, int /*color*/ ) {}
static void SV_RMG_AutomapDrawSquare( int /*x*/, int /*y*/, int /*size*/, int /*color*/ ) {}
static void SV_RMG_AutomapDrawSpecial( int /*x*/, int /*y*/, int /*type*/, int /*color*/ ) {}

/*
===============
SV_InitGameProgs

Init the game subsystem for a new map
===============
*/
void SV_InitGameProgs (void) {
	game_import_t	import = {};   // zero-initialize — unset slots remain NULL
	int				i;

	// unload anything we have now
	if ( ge ) {
		SV_ShutdownGameProgs (qtrue);
	}

	//
	// Populate SOF2 v8 game_import_t (113 entries, zero-initialized above).
	// Slots not listed here remain NULL (reserved / unused by SP).
	//

	// [  0] Printf
	import.Printf                  = Com_Printf;
	// [  1] DPrintf — Com_DPrintf checks com_developer internally
	import.DPrintf                 = Com_DPrintf;
	// [  2] FlushCamFile
	import.FlushCamFile            = Com_FlushCamFile;
	// [  3] Error
	import.Error                   = Com_Error;
	// [  4] Milliseconds
	import.Milliseconds            = Sys_Milliseconds2;
	// [  5] cvar (Cvar_Get)
	import.cvar                    = Cvar_Get;
	// [  6] cvar_set (Cvar_Set)
	import.cvar_set                = Cvar_Set;
	// [  7] Cvar_VariableStringBuffer
	import.Cvar_VariableStringBuffer = Cvar_VariableStringBuffer;
	// [  8] FS_FCloseFile
	import.FS_FCloseFile           = FS_FCloseFile;
	// [  9] FS_ReadFile
	import.FS_ReadFile             = FS_ReadFile;
	// [ 10] FS_Read
	import.FS_Read                 = FS_Read;
	// [ 11] FS_Write
	import.FS_Write                = FS_Write;
	// [ 12] FS_FOpenFile
	import.FS_FOpenFile            = FS_FOpenFileByMode;
	// [ 13] FS_FreeFile
	import.FS_FreeFile             = FS_FreeFile;
	// [ 14] FS_GetFileList
	import.FS_GetFileList          = FS_GetFileList;
	// [ 15] saved_game — SOF2 uses WriteChunk/ReadChunk (slots 52-53), keep NULL
	// [ 18] SendConsoleCommand
	import.SendConsoleCommand      = Cbuf_AddText;
	// [ 19] DropClient
	import.DropClient              = SV_GameDropClient;
	// [ 20] argc
	import.argc                    = Cmd_Argc;
	// [ 21] argv
	import.argv                    = Cmd_Argv;
	// [ 23] Com_sprintf — void wrapper (engine fn returns int)
	import.Com_sprintf             = Com_sprintf_void;
	// [ 24] Cvar_VariableIntegerValue
	import.Cvar_VariableIntegerValue = Cvar_VariableIntegerValue;
	// [ 25] Cvar_SetValue(name, val, force) — force param ignored
	import.Cvar_SetValue           = SV_Cvar_SetValue_Wrapper;
	// [ 26] SetConfigstring
	import.SetConfigstring         = SV_SetConfigstring;
	// [ 27] GetConfigstring
	import.GetConfigstring         = SV_GetConfigstring;
	// [ 28] SetUserinfo
	import.SetUserinfo             = SV_SetUserinfo;
	// [ 29] GetUserinfo
	import.GetUserinfo             = SV_GetUserinfo;
	// [ 30] SendServerCommand
	import.SendServerCommand       = SV_GameSendServerCommand;
	// [ 31] SetBrushModel
	import.SetBrushModel           = SV_SetBrushModel;
	// [ 32] Malloc — wrapper to cast memtag_t enum to int
	import.Malloc                  = G_ZMalloc_Wrapper;
	// [ 33] Free
	import.Free                    = Z_Free;
	// [ 35] GetEntityToken — int wrapper (SV_GetEntityToken returns qboolean)
	import.GetEntityToken          = SV_GetEntityToken_int;
	// [ 36] CM_FreeTerrain
	import.CM_FreeTerrain          = SV_CM_FreeTerrain;
	// [ 38] RMG_Init
	import.RMG_Init                = SV_RMG_Init;
	// [ 39] irand
	import.irand                   = Q_irand;
	// [ 40] RMG_GetSpawnPoint
	import.RMG_GetSpawnPoint       = SV_RMG_GetSpawnPoint;
	// [ 41] RMG_GetCellInfo
	import.RMG_GetCellInfo         = SV_RMG_GetCellInfo;
	// [ 43] RMG_UpdateTerrain
	import.RMG_UpdateTerrain       = SV_RMG_UpdateTerrain;
	// [ 50] trace — 7-arg wrapper (SV_Trace has 2 extra defaulted G2 params)
	import.trace                   = SV_Trace_7args;
	// [ 51] pointcontents
	import.pointcontents           = SV_PointContents;
	// [ 52] SaveGame_WriteChunk
	import.SaveGame_WriteChunk     = SV_SaveGame_WriteChunk;
	// [ 53] SaveGame_ReadChunk
	import.SaveGame_ReadChunk      = SV_SaveGame_ReadChunk;
	// [ 54] LocateGameData — SOF2 only passes entity array pointer
	import.LocateGameData          = SV_LocateGameData;
	// [ 56] GetUserinfoAlt (same function, duplicate slot)
	import.GetUserinfoAlt          = SV_GetUserinfo;
	// [ 57] linkentity
	import.linkentity              = SV_LinkEntity;
	// [ 58] unlinkentity
	import.unlinkentity            = SV_UnlinkEntity;
	// [ 59] MulticastTempEntity
	import.MulticastTempEntity     = SV_MulticastTempEntity;
	// [ 60] InitTempEntFinalize
	import.InitTempEntFinalize     = SV_InitTempEntFinalize;
	// [ 61] GetTempEntCount
	import.GetTempEntCount         = SV_GetTempEntCount;
	// [ 63] ICARUS_PlaySound
	import.ICARUS_PlaySound        = SV_ICARUS_PlaySound;
	// [ 64] ICARUS_RunScript
	import.ICARUS_RunScript        = SV_ICARUS_RunScript;
	// [ 65] GetCurrentEntity
	import.GetCurrentEntity        = SV_GetCurrentEntity;
	// [ 66] G2API_CleanGhoul2Models
	import.G2API_CleanGhoul2Models = SV_G2API_CleanGhoul2Models;
	// [ 67] GetLastErrorString
	import.GetLastErrorString      = SV_GetLastErrorString;
	// [ 68] GetCurrentEntityIndirect
	import.GetCurrentEntityIndirect = SV_GetCurrentEntityIndirect;
	// [ 69] EntitiesInBox
	import.EntitiesInBox           = SV_AreaEntities;
	// [ 70] EntityContact
	import.EntityContact           = SV_EntityContact;
	// [ 71] traceG2 — extra G2 args ignored for now
	import.traceG2                 = SV_TraceG2;
	// [ 72] GetEntityBoundsSize
	import.GetEntityBoundsSize     = SV_GetEntityBoundsSize;
	// [ 73] SetBrushModelAlt (duplicate)
	import.SetBrushModelAlt        = SV_SetBrushModel;
	// [ 74] inPVS
	import.inPVS                   = SV_inPVS;
	// [ 75] inPVSIgnorePortals
	import.inPVSIgnorePortals      = SV_inPVSIgnorePortals;
	// [ 77] SetConfigstringAlt (duplicate)
	import.SetConfigstringAlt      = SV_SetConfigstring;
	// [ 78] GetConfigstringAlt (duplicate)
	import.GetConfigstringAlt      = SV_GetConfigstring;
	// [ 79] GetServerinfo
	import.GetServerinfo           = SV_GetServerinfo;
	// [ 80] AdjustAreaPortalState
	import.AdjustAreaPortalState   = SV_AdjustAreaPortalState;
	// [ 81] totalMapContents
	import.totalMapContents        = CM_TotalMapContents;
	// [ 83] GetSurfaceMaterial
	import.GetSurfaceMaterial      = SV_GetSurfaceMaterial;
	// [ 84] ModelIndex
	import.ModelIndex              = SV_ModelIndex_Wrapper;
	// [ 87] TheGhoul2InfoArray
	import.TheGhoul2InfoArray      = SV_TheGhoul2InfoArray;
	// [ 88] RE_RegisterSkin
	import.RE_RegisterSkin         = SV_RE_RegisterSkin;
	// [ 89] RE_GetAnimationCFG
	import.RE_GetAnimationCFG      = SV_RE_GetAnimationCFG;
	// [ 90] G2API_GetGhoul2InfoV
	import.G2API_GetGhoul2InfoV    = SV_G2API_GetGhoul2InfoV;
	// [ 91] G2API_CleanGhoul2ModelsRef
	import.G2API_CleanGhoul2ModelsRef = SV_G2API_CleanGhoul2ModelsRef;
	// [ 93] SE_GetString
	import.SE_GetString            = SV_SE_GetString_Wrapper;
	// [ 94] SoundIndex
	import.SoundIndex              = SV_SoundIndex_Wrapper;
	// [ 95] SE_SetStringSoundMap
	import.SE_SetStringSoundMap    = SV_SE_SetStringSoundMap;
	// [ 96] GetEntityToken2 (alternate slot, same wrapper)
	import.GetEntityToken2         = SV_GetEntityToken_int;
	// [ 97] CM_RegisterDamageShader
	import.CM_RegisterDamageShader = SV_CM_RegisterDamageShader;
	// [ 98] WE_GetWindVector — void wrapper around bool-returning SV_WE_GetWindVector
	import.WE_GetWindVector        = SV_WE_GetWindVectorVoid;
	// [ 99] SV_UpdateEntitySoundIndex
	import.SV_UpdateEntitySoundIndex = SV_UpdateEntitySoundIndex_;
	// [100] SoundDuration
	import.SoundDuration           = SV_SoundDuration;
	// [101] SetMusicState
	import.SetMusicState           = SV_SetMusicState;
	// [102] RMG_AddBreakpoint
	import.RMG_AddBreakpoint       = SV_RMG_AddBreakpoint;
	// [103] GameShutdown
	import.GameShutdown            = SV_GameShutdown;
	// [104] RMG_AutomapDrawLine
	import.RMG_AutomapDrawLine     = SV_RMG_AutomapDrawLine;
	// [105] RMG_AutomapDrawRegion
	import.RMG_AutomapDrawRegion   = SV_RMG_AutomapDrawRegion;
	// [106] RMG_AutomapDrawCircle
	import.RMG_AutomapDrawCircle   = SV_RMG_AutomapDrawCircle;
	// [107] RMG_AutomapDrawIcon
	import.RMG_AutomapDrawIcon     = SV_RMG_AutomapDrawIcon;
	// [108] RMG_AutomapDrawSquare
	import.RMG_AutomapDrawSquare   = SV_RMG_AutomapDrawSquare;
	// [110] RMG_AutomapDrawSpecial
	import.RMG_AutomapDrawSpecial  = SV_RMG_AutomapDrawSpecial;

	// SOF2 SP game DLL name
	const char *gamename = "gamex86";

	GetGameAPIProc *GetGameAPI;
	gameLibrary = Sys_LoadSPGameDll( gamename, &GetGameAPI );
	if ( !gameLibrary )
		Com_Error( ERR_DROP, "Failed to load %s library", gamename );

	ge = (game_export_t *)GetGameAPI( &import );
	if (!ge)
	{
		Sys_UnloadDll( gameLibrary );
		Com_Error( ERR_DROP, "Failed to load %s library", gamename );
	}

	// SOF2 game_export_t has NO apiversion field — skip the version check.

	// SOF2 cgame is a SEPARATE DLL (cgamex86.dll) loaded later by CL_InitCGame().
	// Do NOT call CL_InitCGameVM here.

	sv.entityParsePoint = CM_EntityString();

	// SOF2 Init(levelTime, randomSeed, restart) — 3 args, not JK2's 9
	Z_TagFree(TAG_G_ALLOC);
	ge->Init( sv.time, Com_Milliseconds(), (int)eSavedGameJustLoaded );

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


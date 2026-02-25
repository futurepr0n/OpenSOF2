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

// cl_net_chan.cpp — SOF2 SP client-side netchan XOR encryption layer.
//
// The C2S message body (after the [serverId][msgAck][relAck] header) is
// XOR-encrypted using:
//   initial key = clc.challenge ^ serverId ^ messageAcknowledge
//   key stream  = clc.serverCommands[reliableAcknowledge & MASK]
//
// The S2C message body is XOR-decrypted using:
//   initial key = clc.challenge ^ msg->data[0..3]  (= server outgoingSequence)
//   key stream  = clc.reliableCommands[relAck & MASK]
//     where relAck is read from the first 4 bytes of the payload.
//
// Adapted from OpenJK MP (codemp/client/cl_net_chan.cpp).

#include "../client/client.h"

// Bytes at the START of the C2S payload that hold the key-derivation header
// [serverId(4)][messageAcknowledge(4)][reliableAcknowledge(4)] — left in clear.
#define CL_ENCODE_START  12

// Bytes at the START of the S2C payload that hold the server reliableAcknowledge
// (4 bytes) — left in clear; used as key stream index.
#define CL_DECODE_START  4


// ---------------------------------------------------------------------------
// CL_Netchan_Encode
//   XOR-encrypt buf->data[CL_ENCODE_START .. cursize-1] for C2S transmission.
//   The client has already written [serverId][msgAck][relAck][cmds...] into buf.
// ---------------------------------------------------------------------------
static void CL_Netchan_Encode( msg_t *msg )
{
	if ( msg->cursize < CL_ENCODE_START + 1 )
		return;

	// Read the three-int key-derivation header (always in the clear).
	int serverId = LittleLong( *(int *)( msg->data     ) );
	int msgAck   = LittleLong( *(int *)( msg->data + 4 ) );
	int relAck   = LittleLong( *(int *)( msg->data + 8 ) );

	// Initial key seed: challenge XOR serverId XOR messageAcknowledge.
	int key    = clc.challenge ^ serverId ^ msgAck;
	int cmdIdx = relAck & ( MAX_RELIABLE_COMMANDS - 1 );
	const char *cmdStr = clc.serverCommands[ cmdIdx ];
	if ( !cmdStr ) cmdStr = "";

	int strIdx = 0;
	for ( int i = CL_ENCODE_START; i < msg->cursize; i++ )
	{
		if ( cmdStr[ strIdx ] == '\0' )
			strIdx = 0;

		char c = cmdStr[ strIdx ];
		if ( c == '%' ) c = '.';

		key ^= (int)c << ( i & 1 );
		strIdx++;
		msg->data[ i ] ^= key;
	}
}


// ---------------------------------------------------------------------------
// CL_Netchan_Decode
//   XOR-decrypt the S2C payload starting at msg->readcount + CL_DECODE_START.
//   Called after Netchan_Process has set msg->readcount past the netchan header.
//
//   The first 4 bytes of the payload (server reliableAcknowledge) are left in
//   the clear and used to select the command key stream.
// ---------------------------------------------------------------------------
static void CL_Netchan_Decode( msg_t *msg )
{
	int start = msg->readcount;
	if ( msg->cursize < start + CL_DECODE_START + 1 )
		return;

	// msg->data[0..3] = server netchan outgoingSequence (written by Netchan_Transmit).
	// This matches the key seed SV_Netchan_Encode used: challenge ^ outgoingSequence.
	int key = clc.challenge ^ LittleLong( *(unsigned *)msg->data );

	// The first 4 bytes of the payload = server's reliableAcknowledge of client cmds.
	int relAck = LittleLong( *(int *)( msg->data + start ) );
	int cmdIdx = relAck & ( MAX_RELIABLE_COMMANDS - 1 );
	const char *cmdStr = clc.reliableCommands[ cmdIdx ];
	if ( !cmdStr ) cmdStr = "";

	int strIdx = 0;
	for ( int i = start + CL_DECODE_START; i < msg->cursize; i++ )
	{
		if ( cmdStr[ strIdx ] == '\0' )
			strIdx = 0;

		char c = cmdStr[ strIdx ];
		if ( c == '%' ) c = '.';

		key ^= (int)c << ( i & 1 );
		strIdx++;
		msg->data[ i ] ^= key;
	}
}


// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// CL_Netchan_Transmit — encrypt and send a client-to-server message.
void CL_Netchan_Transmit( netchan_t *chan, msg_t *msg )
{
	CL_Netchan_Encode( msg );
	Netchan_Transmit( chan, msg->cursize, msg->data );
}

// CL_Netchan_Process — receive and decrypt a server-to-client message.
qboolean CL_Netchan_Process( netchan_t *chan, msg_t *msg )
{
	if ( !Netchan_Process( chan, msg ) )
		return qfalse;

	CL_Netchan_Decode( msg );
	return qtrue;
}

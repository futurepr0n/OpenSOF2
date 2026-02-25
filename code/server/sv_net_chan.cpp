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

// sv_net_chan.cpp — SOF2 SP server-side netchan XOR encryption layer.
//
// The S2C message body is XOR-encrypted using:
//   initial key = client->challenge ^ client->netchan.outgoingSequence
//   key stream  = last client command string (character-by-character)
//
// The C2S message body (after the [serverId][msgAck][relAck] header) is
// XOR-encrypted using:
//   initial key = client->challenge ^ serverId ^ messageAcknowledge
//   key stream  = client->reliableCommands[reliableAcknowledge & MASK]
//
// Adapted from OpenJK MP (codemp/server/sv_net_chan.cpp).

#include "../server/exe_headers.h"
#include "server.h"

// Bytes at the START of the S2C payload that are left in the clear.
// (The server writes reliableAcknowledge in the first 4 bytes before the
// snapshot data; the client reads it to select the command key stream.)
#define SV_ENCODE_START  4

// Bytes at the START of the C2S payload that hold the key-derivation header
// [serverId(4)][messageAcknowledge(4)][reliableAcknowledge(4)] — left in clear.
#define SV_DECODE_START  12


// ---------------------------------------------------------------------------
// SV_Netchan_Encode
//   XOR-encrypt msg->data[SV_ENCODE_START .. cursize-1] for S2C transmission.
// ---------------------------------------------------------------------------
static void SV_Netchan_Encode( client_t *client, msg_t *msg )
{
	if ( msg->cursize < SV_ENCODE_START + 1 )
		return;

	// Initial key seed: challenge XOR netchan outgoing sequence.
	int key = client->challenge ^ client->netchan.outgoingSequence;

	// Key stream comes from the most recently acknowledged client command.
	int cmdIdx = client->lastClientCommand & ( MAX_RELIABLE_COMMANDS - 1 );
	const char *cmdStr = client->reliableCommands[ cmdIdx ];
	if ( !cmdStr ) cmdStr = "";

	int strIdx = 0;
	for ( int i = SV_ENCODE_START; i < msg->cursize; i++ )
	{
		// Wrap the key stream string.
		if ( cmdStr[ strIdx ] == '\0' )
			strIdx = 0;

		// '%' is treated as '.' to avoid format-string issues.
		char c = cmdStr[ strIdx ];
		if ( c == '%' ) c = '.';

		// Evolve key and XOR the payload byte.
		key ^= (int)c << ( i & 1 );
		strIdx++;
		msg->data[ i ] ^= key;
	}
}


// ---------------------------------------------------------------------------
// SV_Netchan_Decode
//   XOR-decrypt the C2S payload starting at msg->readcount + SV_DECODE_START.
//   The first SV_DECODE_START bytes (serverId, msgAck, relAck) are NOT decrypted
//   — they are the key-derivation header and are always in the clear.
// ---------------------------------------------------------------------------
static void SV_Netchan_Decode( client_t *client, msg_t *msg )
{
	int start = msg->readcount;
	if ( msg->cursize < start + SV_DECODE_START )
		return;

	// Read the three-int header (always in the clear).
	int serverId = LittleLong( *(int *)( msg->data + start     ) );
	int msgAck   = LittleLong( *(int *)( msg->data + start + 4 ) );
	int relAck   = LittleLong( *(int *)( msg->data + start + 8 ) );

	// Initial key seed: challenge XOR serverId XOR messageAcknowledge.
	int key    = client->challenge ^ serverId ^ msgAck;
	int cmdIdx = relAck & ( MAX_RELIABLE_COMMANDS - 1 );
	const char *cmdStr = client->reliableCommands[ cmdIdx ];
	if ( !cmdStr ) cmdStr = "";

	int strIdx = 0;
	for ( int i = start + SV_DECODE_START; i < msg->cursize; i++ )
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

// SV_Netchan_Transmit — encrypt and send a server-to-client message.
void SV_Netchan_Transmit( client_t *client, msg_t *msg )
{
	SV_Netchan_Encode( client, msg );
	Netchan_Transmit( &client->netchan, msg->cursize, msg->data );
}

// SV_Netchan_Process — receive and decrypt a client-to-server message.
qboolean SV_Netchan_Process( client_t *client, msg_t *msg )
{
	if ( !Netchan_Process( &client->netchan, msg ) )
		return qfalse;

	SV_Netchan_Decode( client, msg );
	return qtrue;
}

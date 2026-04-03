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

// Filename:	statindex.h
//
// accessed from both server and game modules

#ifndef STATINDEX_H
#define STATINDEX_H


// player_state->stats[] indexes
// IMPORTANT: indices 0 and 1 must match what the native SOF2 cgame reads.
// SOF2 cgamex86.dll reads ps.stats[0] for health (offset 0xD0) and
// ps.stats[1] for armor (offset 0xD4). Confirmed via Ghidra: CG_DrawWeaponHUD
// uses fild [playerState+0xD0] and fild [playerState+0xD4] as health/armor.
typedef enum {
	STAT_HEALTH,					// [0] offset 0xD0 — SOF2 cgame reads this for health
	STAT_ARMOR,						// [1] offset 0xD4 — SOF2 cgame reads this for armor (was STAT_ITEMS)
	STAT_WEAPONS,					// [2] 16 bit fields
	STAT_ITEMS,						// [3] holdable items (was [1], moved; SOF2 cgame ignores this slot)
	STAT_DEAD_YAW,					// look this direction when dead (FIXME: get rid of?)
	STAT_CLIENTS_READY,				// bit mask of clients wishing to exit the intermission (FIXME: configstring?)
	STAT_MAX_HEALTH					// health / armor limit, changable by handicap
} statIndex_t;



#endif	// #ifndef STATINDEX_H


/////////////////////// eof /////////////////////


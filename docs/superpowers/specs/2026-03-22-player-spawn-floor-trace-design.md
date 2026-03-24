# Player Spawn Floor Trace — Design Spec
**Date:** 2026-03-22
**Status:** Approved (v2 — post-review corrections)

---

## Problem

In OpenSOF2 (`+devmap pra2`), the player cannot walk forward using the W key alone.
Jump + W works. NPCs stand on the floor without issue.

**Root cause:** `SP_misc_teleporter_dest` (called by `SP_info_player_deathmatch`) places the
spawn entity at its exact map-data Z with no floor adjustment when the `DROPTOFLOOR`
spawnflag is not set. `SelectSpawnPoint` adds 9 units as a clearance nudge, but the player
bounding box extends 24 units below origin — so the player's feet are still 15 units inside
the floor geometry.

`PM_GroundTrace` detects `startsolid`, sets `groundEntityNum = ENTITYNUM_NONE`, and calls
`PM_AirMove` (accel = 4) instead of `PM_WalkMove` (accel = 12). Velocity is clipped against
the surrounding geometry. Jump provides enough upward impulse to escape the solid, after which
full air movement works.

---

## Existing behaviour in `SelectSpawnPoint` (g_client.cpp ~line 318)

```cpp
VectorCopy( spot->s.origin, origin );
if ( spot->spawnflags & 2 )          // DROPTOFLOOR flag
{
    origin[2] = MIN_WORLD_COORD;
    gi.trace( &tr, spot->s.origin, playerMins, playerMaxs, origin,
              ENTITYNUM_NONE, MASK_PLAYERSOLID, (EG2_Collision)0, 0 );
    if ( tr.fraction < 1.0 && !tr.allsolid && !tr.startsolid )
        VectorCopy( tr.endpos, origin );
    else
        VectorCopy( spot->s.origin, origin );
}
origin[2] += 9;   // always applied — small clearance nudge
```

The DROPTOFLOOR path is **already correct** for flagged spawn points.
The pra2 `info_player_start` does **not** have `spawnflags & 2` set, so the entity Z + 9
path is taken, leaving the player embedded.

---

## Fix

### File
`code/game/g_client.cpp` — function `ClientSpawn`

### Location
Inside the `else` branch (fresh spawn / `+devmap` path, not save-game load), immediately
after `SelectSpawnPoint` returns `spawn_origin` and before
`VectorCopy(spawn_origin, client->ps.origin)`.

### Guard
Only run when the spawn point does **not** have `spawnflags & 2` (DROPTOFLOOR).
If DROPTOFLOOR is set, `SelectSpawnPoint` already traced to the floor correctly — running
a second trace would remove the 9-unit clearance it intentionally applied.

### Change

```cpp
// If the spawn point has no DROPTOFLOOR flag, SelectSpawnPoint only applied a +9
// nudge from the raw entity Z.  For a floor-level spawn entity that leaves the player
// 15 units embedded (bbox bottom = entity_Z + 9 - 24 = entity_Z - 15).
// PM_GroundTrace then hits startsolid, treats the player as airborne, and PM_AirMove
// is used instead of PM_WalkMove — making W-key forward movement non-functional.
// Fix: trace the player bbox down to the actual floor and snap spawn_origin to it.
if ( spawnPoint && !(spawnPoint->spawnflags & 2) )
{
    trace_t tr;
    vec3_t  traceStart, traceEnd;

    VectorCopy( spawn_origin, traceStart );
    traceStart[2] += 18.0f;   // start above to escape any embedded position
    VectorCopy( spawn_origin, traceEnd );
    traceEnd[2]   -= 256.0f;  // search 256 units down

    gi.trace( &tr, traceStart, playerMins, playerMaxs, traceEnd,
              ENTITYNUM_NONE, MASK_PLAYERSOLID, (EG2_Collision)0, 0 );

    Com_Printf( "[SPAWN] Floor trace: startsolid=%d allsolid=%d fraction=%.3f "
                "Z %.1f -> %.1f\n",
                tr.startsolid, tr.allsolid, tr.fraction,
                spawn_origin[2], tr.endpos[2] );

    if ( !tr.startsolid && !tr.allsolid && tr.fraction < 1.0f )
    {
        spawn_origin[2]  = tr.endpos[2];
        spawn_origin[2] += 1.0f;  // 1-unit clearance: defensive against fp precision at contact surface
    }
}
```

**Uses the existing `playerMins`/`playerMaxs` globals** (defined at g_client.cpp ~line 60)
and the exact same `gi.trace` signature as the DROPTOFLOOR path at line 324 — no new
constants introduced.

The `Com_Printf` is unconditional so success can be verified in all cases.

---

## Trace Parameters

| Parameter | Value | Rationale |
|---|---|---|
| Bbox | `playerMins` / `playerMaxs` globals | Exact same values used by DROPTOFLOOR path and pmove |
| Start offset | `+18 Z` | Clears a spawn entity at exact floor height (+ the 9 already applied by `SelectSpawnPoint`) |
| Search depth | `256 units` | Covers any reasonable floor gap |
| Mask | `MASK_PLAYERSOLID` | Same mask used by pmove ground traces |
| Pass-entity | `ENTITYNUM_NONE` | Ignore all entities; trace geometry only |
| G2 args | `(EG2_Collision)0, 0` | Required trailing args for this engine's `gi.trace` |

---

## The +9 Interaction

`SelectSpawnPoint` adds 9 unconditionally after any path (DROPTOFLOOR or not).
When my fix runs, `spawn_origin[2]` is already `entity_Z + 9`.

My trace starts at `entity_Z + 9 + 18 = entity_Z + 27` and finds the floor.
`tr.endpos[2]` is the correct player origin for standing with bbox bottom at the floor surface
(the box trace accounts for the full bbox extents, so the endpos IS the standing position).

I set `spawn_origin[2] = tr.endpos[2]` directly — no need to re-apply +9, because the box
trace already gives the correct standing height (unlike the DROPTOFLOOR path which used a
point trace from the entity origin, requiring the +9 adjustment after).

---

## Edge Cases

| Case | Behaviour |
|---|---|
| `spawnflags & 2` set (DROPTOFLOOR) | Guard skips the fix entirely — `SelectSpawnPoint` already correct |
| Spawn already above floor (gap < 18 u) | Trace still finds floor; Z snapped to correct standing height |
| Spawn over a void (no floor within 256 u) | `fraction == 1.0` → no change; log shows `fraction=1.000` |
| Spawn in solid ceiling / enclosed (startsolid) | Guard skips snap; log shows `startsolid=1` |
| Spawn on a slope | `tr.endpos` valid; pmove slope handling takes over normally |
| Save-game load path | Unaffected — runs the `eSavedGameJustLoaded == eFULL` branch instead |
| `spawnPoint` is null | Guard checks `spawnPoint &&` — skip safely |

---

## Success Criteria

1. Pressing W alone moves the player forward in `pra2`.
2. Log shows `[SPAWN] Floor trace:` line on level load with `fraction < 1.000` and a Z
   adjustment (e.g., `Z 441.0 -> 428.0`).
3. No regression: NPCs, movers, and other entities unaffected (change is player-spawn-only,
   guarded by the `!(spawnflags & 2)` check).
4. Game still runs to 35 s timeout without crash.

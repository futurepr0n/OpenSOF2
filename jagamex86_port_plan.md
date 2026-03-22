# OpenSOF2 — jagamex86.dll Port Plan & Table-Loop Fix Strategy

**Prepared for a fresh session with no prior context.**
**Last updated:** 2026-03-20

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Why the Retail Bridge Is Failing](#2-why-the-retail-bridge-is-failing)
3. [jagamex86.dll Build Incompatibility — Full Analysis](#3-jagamex86dll-build-incompatibility--full-analysis)
4. [Plan A: Build Full jagamex86.dll with SOF2 Struct Bridge](#4-plan-a-build-full-jagamex86dll-with-sof2-struct-bridge)
5. [Plan B: Surgical Subsystem Port (Recommended)](#5-plan-b-surgical-subsystem-port-recommended)
6. [The Table Flip Loop — Root Cause and Fresh Approach](#6-the-table-flip-loop--root-cause-and-fresh-approach)
7. [Concrete Next Steps](#7-concrete-next-steps)
8. [Key File Index](#8-key-file-index)

---

## 1. Architecture Overview

### What OpenSOF2 Is

OpenSOF2 is an open-source reimplementation of Soldier of Fortune 2 (2002, Raven Software) built on
top of OpenJK (the open-source Jedi Knight 2/Jedi Academy engine fork). It uses:

- **Engine**: `openjk_sp.x86.exe` — compiled from `E:\SOF2\OpenSOF2\`, MSBuild `openjk.sln`, Debug Win32
- **Renderer**: `rdsp-vanilla_x86.dll`
- **CGgame**: **retail** `cgamex86.dll` from the original SOF2 installation (copied to `build_test/Debug/`)
- **Game DLL**: **retail** `gamex86.dll` from the original SOF2 installation (loaded via a bridge)
- **Test map**: `+devmap pra2` (Mission 1, Prague)

### The Retail Bridge (`sv_game.cpp`)

The engine loads the retail `gamex86.dll` through a compatibility bridge in
`E:\SOF2\OpenSOF2\code\server\sv_game.cpp`. The retail DLL was compiled in 2002 against:

- SOF2 game API **version 8** (flat void-pointer import array, 113 slots)
- SOF2's own `entityState_t` (256 bytes) and `playerState_t` (472 bytes)

OpenJK's `GetGameAPI` signature passes a single `game_import_t*` struct pointer; SOF2's
`GetGameAPI` takes `(int apiVersion, void *imports)` — two separate arguments. The bridge resolves
this by manually building a flat 113-slot function-pointer array and calling the retail DLL with
`GetGameAPI(8, &giArray)`.

### Current Status (as of session end)

**Working:**
- Engine starts, map loads, ICARUS runtime created
- `.IBI` scripts are precached (intro, table_flipper, cratedoor01, pushbox01, tank_truck, etc.)
- Movement trigger compat dispatch fires (`SV_SOF2CompatDispatchMoveTriggers`)
- Some ICARUS script chains execute (crate pushed from above, storage door opens)

**Broken:**
1. **Table flip loop** — entity 387 (`trigger_multiple`) causes `table_flipper.IBI`'s ROFF
   animation to replay in an infinite loop instead of playing once
2. **Crate destruction** — physics impacts do not break `func_breakable_brush` entities because
   the `EffectorCollision` callback lives inside the retail `gamex86.dll` (inaccessible from engine)
3. **NPC movement** — scripted NPC motion is not visibly represented (separate issue, see below)

---

## 2. Why the Retail Bridge Is Failing

### 2.1 Trigger Re-fire Loop

**Symptom:** The table-flip animation plays repeatedly. `table.PNG` shows the table
rotating/spinning continuously.

**Four failed fix attempts:**

| Attempt | Approach | Why It Failed |
|---------|----------|---------------|
| 1 | Unconditional MOVE latch check in `SV_SOF2_AreaEntities` | Entity 387 not in 15-unit compat bbox → latch cleared every sweep |
| 2 | Latch-on-return for any trigger returned by wrapper | Same bbox mismatch — latch cleared by same-frame sweep |
| 3 | Always-run spatial sweep regardless of BUTTON_USE | Underlying bbox mismatch unchanged |
| 4 | Expanded bbox to {40,40,52} matching Q3A `G_TouchTriggers` | Did not solve loop — entity 387 now fires only ONCE in compat dispatch but table still loops |

**Key evidence from latest qconsole.log (after all 4 fixes):**
```
[MOVE compat] touch #16 ent=387 model=...  (appears ONCE, not repeatedly)
```

Entity 387 is NOT re-firing from the trigger system anymore. The table loop must originate at
the **ICARUS script level**, not the trigger level. Two hypotheses:

**Hypothesis A — ICARUS script has a REPEAT loop:**
The `table_flipper.IBI` script may contain a `REPEAT` or jump-back construct that causes
`PLAY_ROFF scripts/pra2/table_push.rof` to replay indefinitely. The retail ICARUS runtime runs
as part of `gamex86.dll`; our engine ICARUS bridge calls into it. If the script loops at the
ICARUS bytecode level, no trigger fix can stop it.

**Hypothesis B — ROFF never signals completion:**
`PLAY_ROFF` in ICARUS is a blocking operation. The ROFF callback signals completion via
`Q3_TaskIDComplete(ent, TID_MOVE_NAV)`. If the ROFF playback function (inside the retail DLL)
never calls `Q3_TaskIDComplete`, ICARUS may time out and restart the script from the beginning.
This is especially likely because in retail SOF2, `G_Roff()` think function is the ROFF driver —
but our engine's `ge->ClientThink` may not be correctly setting up the `roff_ctr` / `next_roff_time`
fields expected by the retail DLL's G_Roff implementation.

**To diagnose:** Add logging around `Q3_TaskIDComplete` calls to determine if the ROFF completes.
If `TID_MOVE_NAV` never fires, Hypothesis B is confirmed. If it does fire but the animation still
replays, Hypothesis A is likely.

### 2.2 Breakable Entity Physics

**Symptom:** Physics objects (falling crates) pass through or rest on `func_breakable_brush`
entities without destroying them.

**Root cause:** SOF2 uses `CRagDollUpdateParams::EffectorCollision()` — a C++ method in the retail
`gamex86.dll` — as the physics-to-game damage bridge. When a ragdoll bone hits geometry, the engine
calls this callback to apply damage to the hit entity. Because the callback lives in the game DLL,
and our engine bridge can't easily hook into it, physics impacts never call `G_Damage`.

We added `EffectorCollision → G_Damage` logic in `E:\SOF2\OpenSOF2\code\game\g_main.cpp`, but
that code is part of `jagamex86.dll` — which currently cannot be built (see Section 3).

---

## 3. jagamex86.dll Build Incompatibility — Full Analysis

### 3.1 What Is jagamex86.dll

`jagamex86.dll` is an attempt to build OpenJK's game DLL (JK2 Jedi Academy game code) as a
replacement for the retail `gamex86.dll`. The project is at:
```
E:\SOF2\OpenSOF2\build_test\code\game\jagamex86.vcxproj
```
Source files are from `E:\SOF2\OpenSOF2\code\game\`.

### 3.2 The 2486 Build Errors — Nature of the Problem

When building `jagamex86.dll`, the compiler reports **2486 errors**. These are NOT about the
core struct sizes — our `q_shared.h` already has correct SOF2-specific structs:

```c
// E:\SOF2\OpenSOF2\code\qcommon\q_shared.h

// Already correct:
typedef struct playerState_s { ... } playerState_t;    // 472 bytes (SOF2-verified)
typedef struct entityState_s { ... } entityState_t;    // 256 bytes (SOF2-verified)
static_assert(sizeof(playerState_t) == 472, ...);
static_assert(sizeof(entityState_t) == 256, ...);
```

The errors come from OpenJK game code files referencing **JK2/JKA-specific fields** that do not
exist in our SOF2 `playerState_t` / `entityState_t`:

| JK2/JKA Field | Where Used | SOF2 Equivalent |
|---------------|-----------|-----------------|
| `ps.saberEntityNum` | 33 files | None (no lightsabers in SOF2) |
| `ps.forcePower` / `ps.forcePowersActive` | 61 files (1285 occurrences) | None (no Force powers) |
| `ps.forcePowerLevel[]` | Many files | None |
| `ps.saberLockFrame` | AI_Jedi.cpp etc. | None |
| `ps.forceHandExtend` | bg_panimate.cpp etc. | None |
| `s.m_iVehicleNum` | g_trigger.cpp, many | None (no vehicles in SOF2 SP) |
| `s.modelScale[3]` | g_breakable.cpp, others | None (SOF2 uses `g2radius` int, not float[3]) |
| `client->ps.inventory[]` | g_inventory.cpp | SOF2 has inventory but in a different struct |

**Scope:** 61 source files contain JK2-specific field accesses. The total removal/replacement
count is approximately 1285+ individual field access sites.

### 3.3 Why Targeted Files Are Different

Despite the overall 2486 errors, the three files we specifically need for scripting are
**nearly clean**:

```
g_roff.cpp:      0 JK2-specific field references  ← Compiles clean today
g_trigger.cpp:   5 JK2-specific field references  ← Minor fixes needed
g_breakable.cpp: 9 JK2-specific field references  ← Minor fixes needed
```

**g_trigger.cpp — 5 errors:**
```cpp
// Line 1327-1330: vehicle trigger interaction (IRRELEVANT for SOF2)
if (other->s.m_iVehicleNum && other->s.m_iVehicleNum <= MAX_CLIENTS) {
    gentity_t *veh = &g_entities[other->s.m_iVehicleNum]; // 3x references
    ...
}
// Line 1386: vehicle removal surfaces check
if (!other->s.m_iVehicleNum || other->m_pVehicle->m_iRemovedSurfaces)

// Line 1629: Force Sight check
if (!( player->client->ps.forcePowersActive & (1 << FP_SEE) ))
```
**Fix:** Wrap in `#ifdef JK2_VEHICLES` and `#ifdef JK2_FORCE_POWERS`, or simply delete these
blocks entirely — SOF2 has no vehicles or Force powers.

**g_breakable.cpp — 9 errors (all in SP_func_breakable spawn function):**
```cpp
// Lines 1172-1243: modelscale_vec spawn key → s.modelScale[0/1/2]
qboolean bHasScale = G_SpawnVector("modelscale_vec", "0 0 0", ent->s.modelScale);
ent->maxs[0] *= ent->s.modelScale[0];
...
```
**Fix:** `s.modelScale` is a `vec3_t` in JK2's entityState_t. SOF2's entityState_t has no
`modelScale` field. Options:
- Add a `float modelScale[3]` field to our SOF2 entityState_t (pick 3 unused bytes, or expand)
- Replace `ent->s.modelScale[N]` with a local `vec3_t localScale` in `SP_func_breakable()`
- Simplest: use `1.0f` constant fallback when the spawn key is not present

**g_roff.cpp — 0 errors:**
`g_roff.cpp` uses only standard `gentity_t` fields and `playerState_t` fields that already exist
in SOF2's layout (`s.pos`, `s.apos`, trajectory fields, `currentOrigin`, `currentAngles`). It
can compile today without any changes.

---

## 4. Plan A: Build Full jagamex86.dll with SOF2 Struct Bridge

**Goal:** Make the entire `jagamex86.dll` build against SOF2's struct layouts so that OpenJK's
game code (including NPC AI, ICARUS, trigger logic, ROFF, breakables) replaces the retail DLL.

**This is the correct long-term solution** but requires the most upfront work.

### 4.1 Approach: Conditional Compilation Stubs

Rather than modifying the 61 affected source files individually, use a single compatibility header
that maps JK2 field names to SOF2 equivalents or NULL stubs.

**Create** `E:\SOF2\OpenSOF2\code\game\sof2_compat.h`:
```c
#pragma once
// SOF2 compatibility layer for OpenJK game code
// Purpose: stub out JK2/JKA-specific fields so the game compiles against SOF2 structs

// Force power stubs — SOF2 has no Force system
#define FP_SEE              0
#define FP_SABER_OFFENSE    0
#define NUM_FORCE_POWERS    1
#define forcePowersActive   sof2_unused_force_stub
#define forcePowerLevel     sof2_unused_force_level_stub
#define forcePower          sof2_unused_force_stub
#define saberEntityNum      sof2_unused_saber_stub

// Vehicle stubs — SOF2 SP has no vehicles
#define m_iVehicleNum       sof2_unused_vehicle_stub
#define m_pVehicle          nullptr

// modelScale stub — SOF2 entityState_t has no modelScale
// (Option 1: use a global dummy, Option 2: add field to entityState_t)
extern float sof2_dummy_modelScale[3];
// Replace ent->s.modelScale with a local: use sof2_dummy_modelScale
```

This requires adding the above stubs AND adding dummy fields to struct definitions that make the
compiler happy. A complete list:

### 4.2 JK2 → SOF2 playerState_t Field Mapping

| JK2/JKA Field | Type | SOF2 Equivalent / Action |
|---------------|------|--------------------------|
| `forcePower` | int | Delete/stub (no Force) |
| `forcePowerMax` | int | Delete/stub |
| `forcePowersKnown` | int | Delete/stub |
| `forcePowersActive` | int | Delete/stub |
| `forcePowerLevel[NUM_FORCE_POWERS]` | int[] | Delete/stub |
| `forcePowerDebounce[NUM_FORCE_POWERS]` | int[] | Delete/stub |
| `forcePowerDuration[NUM_FORCE_POWERS]` | int[] | Delete/stub |
| `saberEntityNum` | int | Delete/stub |
| `saberLockFrame` | int | Delete/stub |
| `forceHandExtend` | int | Delete/stub |
| `inventory[MAX_INVENTORY]` | int[] | SOF2 does have inventory — verify if layout matches |
| `isSaberInFlight` | qboolean | Delete/stub |

### 4.3 JK2 → SOF2 entityState_t Field Mapping

| JK2/JKA Field | Type | SOF2 Equivalent / Action |
|---------------|------|--------------------------|
| `m_iVehicleNum` | int | Delete/stub |
| `modelScale[3]` | float[3] | Add to SOF2 entityState_t OR use local variable |
| `boltInfo` | int | SOF2 has `boltIndex` — may be equivalent |
| `radius` | float | SOF2 has `g2radius` as int |

**Note on `s.modelScale`:** The safest fix for `g_breakable.cpp` is to add `float modelScale[3]`
to SOF2's `entityState_t` if there is room (we have 256 bytes, current layout ends at 0xFF with
`xorKey`). We cannot expand entityState_t without breaking the retail binary compatibility at the
network protocol level — UNLESS `jagamex86.dll` is used as a FULL replacement for the retail DLL,
in which case there is no network compat concern.

### 4.4 JK2 API → SOF2 Import Table Mapping

The full import table is already wired in `sv_game.cpp` under the `sof2_use_openjk_game` cvar
path. This path calls `GetGameAPI(&jkImport)` with a populated `game_import_t*`. All 113 retail
slots have been mapped to OpenJK equivalents. This part is complete.

### 4.5 Estimated Work for Plan A

- Fix 61 files (1285 field access sites)
- Most are mechanical: add `#ifdef JK2_FORCE_POWERS` guards or replace with stubs
- Highest risk files: `AI_Jedi.cpp` (113 FP references), `wp_saber.cpp` (571 references),
  `bg_pmove.cpp` (122 references), `Q3_Interface.cpp` (25 references)
- Saber and Force power code can be entirely disabled with `#define OPENJK_NO_JEDI`
- NPC AI files can be included with stubs for the saber-specific interactions
- **Rough estimate:** 2–4 sessions of mechanical fix work, then integration testing

---

## 5. Plan B: Surgical Subsystem Port (Recommended)

**Goal:** Fix the immediately-broken behaviors (table loop, crate destruction) by building ONLY
the relevant subsystems from OpenJK against SOF2 struct layouts, without building the full
`jagamex86.dll`.

**This is the recommended first step** because it is lower risk and directly targets the failures.

### 5.1 What This Means in Practice

The retail `gamex86.dll` stays loaded. We do NOT replace it with `jagamex86.dll`. Instead:

1. **ROFF:** We intercept ROFF playback in the **engine bridge** (`sv_game.cpp`) so that
   `G_Roff()` logic runs in the engine rather than the retail DLL. This is possible because
   `g_roff.cpp` compiles clean today.

2. **Trigger:** We intercept `multi_trigger` so the wait mechanism uses OpenJK's logic, not the
   retail DLL's. This is done in the engine's `SV_SOF2CompatDispatchMoveTriggers` which already
   calls into the retail touch callback.

3. **Breakable:** We add a physics damage path in the engine that calls `G_Damage` on
   `func_breakable_brush` entities on physics impact.

### 5.2 ROFF Subsystem — Intercept Plan

**Problem:** The retail DLL's `G_Roff` think function is responsible for advancing ROFF frame
playback and signaling ICARUS `TID_MOVE_NAV` completion. We cannot observe this from the engine.

**Solution:** Use a "think hook" approach in `sv_game.cpp`:

```
Engine frame:
  ge->RunFrame(time)                     ← retail DLL runs G_Roff internally
                                            (we cannot control this)
```

**Alternative — OpenJK g_roff as a standalone static library:**

Build `g_roff.cpp` as a static object linked into the engine (`openjk_sp_x86`). This is possible
because `g_roff.cpp` has zero JK2-specific field references. However, it accesses:
- `g_entities[]` — the retail DLL's entity array (pointer accessible via bridge)
- `gi.linkentity()` — available via the import table
- `Q3_TaskIDComplete()` — this is the hard part: it calls into the ICARUS interface

`Q3_TaskIDComplete` is in `Q3_Interface.cpp` which calls ICARUS. The ICARUS instance is created
by the retail DLL. We have a pointer to it (`g_ICARUS` in sv_game.cpp bridge area). The ICARUS
`Q3_TaskIDComplete` call ultimately calls back into the ICARUS runtime to mark a task done.

**Simpler approach for ROFF:** Debug why `TID_MOVE_NAV` is never completing:

1. Add a hook in `sv_game.cpp` that intercepts the ICARUS `TaskIDComplete` callback
2. Log when `TID_MOVE_NAV` fires (or doesn't) for the ROFF entity
3. If it never fires, add a manual timer-based completion signal

### 5.3 Trigger Wait Mechanism — Intercept Plan

**Problem:** When `SV_SOF2CompatDispatchMoveTriggers` calls the retail trigger's vtable Touch
method, the retail `multi_trigger()` sets `ent->nextthink = level.time + wait*1000`. This SHOULD
prevent re-fire. BUT — does the table_flipper trigger have `wait > 0`?

**Key question to answer (read from retail entity data):**
```
Entity 387: trigger_multiple
  wait = ??? (read this from retail g_entities[387] at runtime)
  nextthink = ??? (check if it advances after first fire)
```

You can read retail `gentity_t` fields indirectly via the import bridge:
```cpp
// In SV_SOF2CompatDispatchMoveTriggers, after firing:
// Read ent->nextthink (at the JK2 gentity_t offset for nextthink)
// Log: "ent 387 nextthink after touch = %d, level.time = %d"
```

The `gentity_t::nextthink` offset in the RETAIL SOF2 DLL must be verified from Ghidra. Check
the `CEntity_GetNextthink` or similar accessor.

**If `wait == 0`:** The trigger is configured to re-fire every frame. This is by design for
the intro ICARUS sequence. The ICARUS script itself must be handling re-fire prevention via
task completion, not the trigger wait. This confirms Hypothesis B above.

### 5.4 Breakable Entity — Direct Engine Fix

The `EffectorCollision` callback path is complex. For pra2, the most common
breakable-on-physics-impact case is crates being knocked off shelves by script ROFFs.

**Question:** Do the pra2 crates need to break on ROFF push impact, or just move?

If they only need to **move** (slide/fall), this is already handled by ROFF playing on the crate
entity directly — ROFF moves the entity via trajectory updates, no physics destruction needed.

If they need to **break** (crumble/explode), the path is:
1. In `ge->RunFrame` hook in `sv_game.cpp`, after the retail DLL runs, iterate `g_entities[]`
2. For any `func_breakable_brush` entity that has moved significantly in one frame, infer a
   physics impact and call through the retail vtable to the Die function (vtable slot TBD from
   Ghidra)

This is ugly but avoids the full `jagamex86.dll` port.

---

## 6. The Table Flip Loop — Root Cause and Fresh Approach

### 6.1 Current State

After all four fix attempts, entity 387 (`trigger_multiple`) fires exactly **once** from the
compat dispatch system (`[MOVE compat] touch #16 ent=387`). The latch prevents it from re-firing.

BUT the table animation still loops. This means the loop is at the **ICARUS or ROFF level**.

### 6.2 Diagnosing the Loop

**Step 1: Add TaskIDComplete logging**

In `E:\SOF2\OpenSOF2\code\ICARUS\` (the ICARUS bridge interface), find where `Q3_TaskIDComplete`
is called and add:

```cpp
void Q3_TaskIDComplete( gentity_t *ent, taskID_t taskType ) {
    Com_Printf("[ICARUS] TaskIDComplete ent=%d taskType=%d time=%d\n",
        ent->s.number, taskType, level.time);
    // ... existing code ...
}
```

If `TID_MOVE_NAV` for the table_flipper ROFF entity fires once and is followed by the animation
replaying, the ICARUS script itself is looping (Hypothesis A).

If `TID_MOVE_NAV` never fires, the ROFF is not completing (Hypothesis B).

**Step 2: Check for ICARUS REPEAT in table_flipper.IBI**

Disassemble the IBI bytecode more thoroughly. The file is at:
`E:\SOF2\OpenSOF2\testing\pra2\table_flipper.IBI`

The IBI format uses opcodes. A `REPEAT` opcode (or the equivalent loop construct) will appear
as a backward jump in the bytecode. Use the ICARUS source code in
`E:\SOF2\OpenJK\code\icarus\` (specifically `BlockStream.cpp`, `Instance.cpp`) to find the
opcode values for `REPEAT` and `DO` (loop) blocks.

**Step 3: Check for multiple script_runner entities targeting the table**

The pra2 entity graph contains multiple `trigger_multiple` + `script_runner` pairs. It is possible
that a second trigger (different entity number) also targets the `table_flipper` script and fires
independently. Check the entity-string log output for:
- Any other `script_runner` with `spawnscript` or `usescript` referencing `table_flipper`
- Any `target_relay` or `target_script` entities chaining to the table entity

**Step 4: Check if `nextthink` advances after first fire**

Add this to `SV_SOF2CompatDispatchMoveTriggers` right after the `__try` block fires entity 387:

```cpp
// Debug: read nextthink from retail entity (offset needs Ghidra verification)
// Approximate: JK2 gentity_t nextthink is around byte offset 0x44 (276 in q_shared)
// For SOF2 retail, verify from Ghidra: look at G_RunFrame → entity think dispatch
float *retailNextthink = (float*)((byte*)hit + RETAIL_NEXTTHINK_OFFSET);
Com_Printf("[TRIGGER debug] ent=387 nextthink=%.0f level.time=%d\n",
    *retailNextthink, sv.time);
```

If `nextthink` is `0` or `< level.time` immediately after firing, the retail trigger's wait
mechanism is not activating (possibly `wait == 0` in the entity).

### 6.3 Fix Strategy Based on Diagnosis

**If Hypothesis A (ICARUS script loops):**

The `table_flipper.IBI` script has a `REPEAT` or `DO/WHILE` loop around the `PLAY_ROFF`. In
retail SOF2, this loop presumably terminates on a condition (e.g., `GET ENTITY_HEALTH` drops
to 0, or a flag is set). If the condition check uses a field that behaves differently in OpenSOF2,
the loop never exits.

Fix: Patch the ICARUS execution at the engine level to detect when a specific task (`PLAY_ROFF`)
has been executed N times and inject a script break. This is hacky but surgical.

Better fix: Properly implement whatever condition the loop checks. This likely requires a working
`G_GetGameInfo` or similar ICARUS query function that the retail DLL uses.

**If Hypothesis B (ROFF never completes TID_MOVE_NAV):**

The retail G_Roff think function is not being called, or is failing silently. Steps:
1. Verify the ROFF entity has its `e_ThinkFunc` set to the ROFF think function after `PLAY_ROFF`
2. Check if `ge->RunFrame` is advancing the ROFF entity's think time
3. If think is not firing: manually advance ROFF from the engine by hooking into `G_RunThink`
   equivalent

**If Hypothesis C (wait == 0 on trigger, normal behavior):**

In retail, `trigger_multiple` for the table may legitimately fire every frame, but the ICARUS
`PLAY_ROFF` blocks (is non-re-entrant) until completion. In OpenSOF2, if `PLAY_ROFF` is not
blocking (because the ROFF entity task never signals `TID_MOVE_NAV`), ICARUS keeps re-issuing
the ROFF command.

Fix: Implement a re-entrancy guard in the ICARUS bridge. When `PLAY_ROFF` is in progress for
entity X, any new `PLAY_ROFF` request for the same entity should be a no-op until the current
one completes.

---

## 7. Concrete Next Steps

### Immediate: Diagnose the Table Loop (1-2 hours)

1. Add `Q3_TaskIDComplete` logging (see Section 6.2 Step 1)
2. Run `+devmap pra2`, walk to the table trigger area, capture log
3. Determine which hypothesis is correct

### Short-term: Fix the Table Loop (2-4 hours depending on diagnosis)

Based on diagnosis, apply one of the three fixes in Section 6.3.

### Medium-term: Build g_roff.cpp into the Engine (4-8 hours)

This is the cleanest long-term approach:

1. Add `g_roff.cpp` to the engine's source list (NOT to jagamex86.vcxproj — to openjk_sp_x86)
2. Fix the `Q3_TaskIDComplete` linkage by exposing the ICARUS task completion interface
3. Add a new import slot (or repurpose an unused one) for the engine to call `G_Roff` directly
4. Hook it into the game frame loop in `sv_game.cpp`

This bypasses the retail ROFF driver entirely.

### Longer-term: Build jagamex86.dll (Plan A, optional but recommended)

1. Create `sof2_compat.h` (Section 4.1)
2. Tackle files in order of impact:
   - `wp_saber.cpp` (571 errors) — wrap in `#ifndef SOF2` guard, use stub exports
   - `AI_Jedi.cpp` (113 errors) — stub saber/Force interactions
   - `bg_pmove.cpp` (122 errors) — remove Force jump, saber block refs
   - `bg_pangles.cpp`, `bg_panimate.cpp` — remove saber/Force anim states
   - `g_combat.cpp`, `g_client.cpp`, `g_cmds.cpp` — stub Force/saber paths
   - `g_trigger.cpp`, `g_breakable.cpp` — minimal changes (Section 3.3)
   - `g_roff.cpp` — no changes needed
3. Test `jagamex86.dll` with `sof2_use_openjk_game 1`

---

## 8. Key File Index

### Engine Files (always need compile+restart)
| File | Purpose | Key Functions |
|------|---------|---------------|
| `code/server/sv_game.cpp` | Retail DLL bridge, latch system, alternate OpenJK load path | `SV_SOF2_AreaEntities`, `SV_SOF2CompatSetMoveTriggerLatched`, `SV_InitGameProgs` |
| `code/server/sv_client.cpp` | Trigger compat dispatch | `SV_SOF2CompatDispatchMoveTriggers`, `SV_SOF2CompatDispatchUse` |
| `code/server/sv_init.cpp` | PostLoadInit, ConnectNavs | `SV_SpawnServer` |
| `code/server/sv_snapshot.cpp` | Debug entity dump | `SV_BuildClientSnapshot` |

### Game DLL Files (jagamex86.dll — currently unbuildable)
| File | Purpose | Build Status |
|------|---------|-------------|
| `code/game/g_roff.cpp` | ROFF playback, ICARUS `TID_MOVE_NAV` | **0 errors** — compiles clean |
| `code/game/g_trigger.cpp` | `trigger_multiple`, `multi_trigger`, wait logic | 5 errors (vehicle/Force stubs) |
| `code/game/g_breakable.cpp` | `func_breakable_brush` spawn + die | 9 errors (`modelScale` stubs) |
| `code/game/g_spawn.cpp` | Entity classname → spawn function table | SOF2 aliases added |
| `code/game/g_main.cpp` | `EffectorCollision` damage path | Added, needs jagamex86 to build |

### ICARUS Files
| File | Purpose |
|------|---------|
| `code/ICARUS/` | ICARUS runtime bridge (IBI interpreter) |
| `testing/pra2/table_flipper.IBI` | Table flip script (key debugging target) |
| `testing/pra2/intro.IBI` | Intro script (also active during pra2 load) |

### Build
```
# Build engine (from E:\SOF2\OpenSOF2\)
MSBuild build_test/openjk.sln /p:Configuration=Debug /p:Platform=Win32 /t:openjk_sp_x86

# Run test
build_test/Debug/openjk_sp.x86.exe +devmap pra2 +set developer 1 +set logfile 2

# Log output
build_test/Debug/qconsole.log
```

### Reference Files
| File | Purpose |
|------|---------|
| `E:\SOF2\OpenSOF2\icarus_progress.md` | ICARUS work history, prior fix attempts |
| `E:\SOF2\OpenSOF2\code\qcommon\q_shared.h:1575` | SOF2 `playerState_t` layout (verified) |
| `E:\SOF2\OpenSOF2\code\qcommon\q_shared.h:1788` | SOF2 `entityState_t` layout (verified) |
| `E:\SOF2\OpenJK\code\icarus\` | Reference ICARUS source (Instance.cpp, BlockStream.cpp) |
| `E:\SOF2\Soldier of Fortune 2\base\qconsole.log` | Retail SOF2 log for comparison |

---

## Appendix: gentity_t nextthink Offset Investigation

To read `nextthink` from the retail entity struct at runtime, you need the offset of `nextthink`
within the retail SOF2 `gentity_t`. This can be found via Ghidra:

1. Open `gamex86.dll` in Ghidra
2. Search for `G_RunThink` or equivalent think dispatch function
3. Find the pattern: `if (ent->nextthink > level.time) return;`
4. The offset of `nextthink` relative to the entity pointer is visible in the decompilation
5. Common estimate based on Q3A heritage: around `+0x44` to `+0x60` depending on struct layout

The retail SOF2 `gentity_t` has a different layout than OpenJK's because:
- SOF2 uses C++ class inheritance (CEntity → CNonPlayerClient → CNPC etc.)
- The vtable pointer is at offset 0
- Base entity fields start at +4 or +8 depending on single vs multiple inheritance

This needs a fresh Ghidra decompilation session to pin down precisely.

---

*End of document. Written to provide a new session with zero prior context the full background needed to continue this work.*

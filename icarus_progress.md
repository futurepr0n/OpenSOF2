# ICARUS Progress

## Current state

- Runtime creates ICARUS successfully.
- `pra2` script assets are precached successfully from the main run.
- We now have partial scripted world behavior working in `pra2`.
- Confirmed visible successes from the latest run:
  - crates pushed from above are working
  - a storage/container scripted door opening event is working
- This means the current blocker is no longer "ICARUS assets are not loading at all".

## Key code changes that unlocked script activity

- Engine startup already had:
  - `ge->ConnectNavs()`
- We also tested:
  - `ge->PostLoadInit()` from [sv_init.cpp](E:/SOF2/OpenSOF2/code/server/sv_init.cpp)
- The change that actually moved behavior forward was in:
  - [sv_client.cpp](E:/SOF2/OpenSOF2/code/server/sv_client.cpp)
- Added a movement-trigger compatibility path:
  - `SV_SOF2CompatDispatchMoveTriggers(...)`
  - runs after `ge->ClientThink(...)`
  - scans nearby linked `CONTENTS_TRIGGER` entities
  - calls the retail touch callback at vtable `+0x6c`
  - latches touched triggers per-client until the player leaves them
- This is separate from the older `BUTTON_USE` compatibility path:
  - `SV_SOF2CompatDispatchUse(...)`

## Why this mattered

- The runtime was already precaching `.IBI` script assets and spawning `script_runner` entities.
- But plain movement overlap triggers were still not activating script chains.
- The engine compat path previously only helped for explicit `use` interactions.
- `pra2` startup relies on overlap-triggered script activation, so the missing touch fallback was the correct gap.

## Useful evidence from `qconsole.log`

- ICARUS is alive:
  - `ICARUS Instance successfully created`
- `pra2` scripts are precached:
  - `scripts/pra2/intro`
  - `scripts/pra2/table_flipper`
  - `scripts/pra2/cratedoor01`
  - `scripts/pra2/pushbox01`
  - `scripts/pra2/tank_truck`
  - `scripts/pra2/runtotruck`
  - and others
- Startup entity graph from map entity-string logging:
  - `script_runner targetname='t257'`
  - `trigger_multiple target='t257'`
  - `script_runner targetname='t254'`
  - `trigger_multiple target='t254'`
  - `target_relay targetname='run2truck' target='t245'`
- Movement-trigger fallback now fires:
  - `[MOVE compat] touch #... ent=...`

## Important limitation right now

- Visible NPCs are still debug probes submitted from parsed map entity-string spawn data, not live retail NPC render state.
- That means:
  - visible NPC bodies can appear in correct approximate world positions
  - but they remain static unless we bridge them to live entity state
- So when scripts move actors, that motion will not yet be visible through the current probe renderer.
- This is why "scripts are not moving characters" is currently mixed:
  - some script chains are now firing
  - but visible NPC motion is still blocked by the render/debug probe path

## Extracted script findings from `testing/pra2`

- [intro.IBI](E:/SOF2/OpenSOF2/testing/pra2/intro.IBI)
  - contains:
    - `SET_CONSOLE_COMMAND exec menus/level_starts/pra2.cfg`
    - `cin_startdoor`
    - `cin_john`
    - `cin_john01`
    - `cin_cam_face`
    - `SET_DEATH_SCRIPT` for guards
- [table_flipper.IBI](E:/SOF2/OpenSOF2/testing/pra2/table_flipper.IBI)
  - contains:
    - `guard_table`
    - `guard_table_spawn`
    - `PLAY_ROFF scripts/pra2/table_push.rof`
- [cratedoor01.IBI](E:/SOF2/OpenSOF2/testing/pra2/cratedoor01.IBI)
  - contains:
    - `cratebum`
    - `door`
    - `PLAY_ROFF scripts/pra2/cratedoor01.rof`
- [nocig.IBI](E:/SOF2/OpenSOF2/testing/pra2/nocig.IBI)
  - references:
    - `run2truck`
- [runtotruck.IBI](E:/SOF2/OpenSOF2/testing/pra2/runtotruck.IBI)
  - references:
    - `truckguy01`
    - `truckguy02`
    - path/nav goal setup
- [tank_truck.IBI](E:/SOF2/OpenSOF2/testing/pra2/tank_truck.IBI)
  - references:
    - `truck`
    - `turntruck`
    - `first_guard01`
    - `truck_death`

## Current remaining issues

- NPC scripted movement is not visibly represented yet.
- The table-flip event fires through ICARUS **but still loops** — four trigger-level fixes all failed.
  - Latest log shows entity 387 fires only ONCE from compat dispatch (`[MOVE compat] touch #16 ent=387`)
  - Loop is now believed to be at the **ICARUS or ROFF level**, not the trigger level
  - See [jagamex86_port_plan.md](E:/SOF2/OpenSOF2/jagamex86_port_plan.md) Section 6 for full diagnosis plan
- `jagamex86.dll` (OpenJK game code) cannot be built — 2486 errors from JK2-specific field references
  - See [jagamex86_port_plan.md](E:/SOF2/OpenSOF2/jagamex86_port_plan.md) for full Port Plan (Plan A and Plan B)
- Rain-suit hood surfaces are still unresolved:
  - current logs show `hood=-1->-1 caps=-1->-1`
- A mover visibility/snapshot issue still persists:
  - `[SV snapshot] ent=2 skip=pvs-nooverflow ...`
- Some scripted movers/animations may still be missing because of that snapshot/PVS path.

## Most useful next steps

**PRIORITY: Diagnose table loop (Section 6 of jagamex86_port_plan.md)**

1. Add `Q3_TaskIDComplete` logging to determine if `TID_MOVE_NAV` fires for the ROFF entity.
   - If it never fires → Hypothesis B (ROFF not completing)
   - If it fires and loop continues → Hypothesis A (ICARUS script has REPEAT loop)
2. Build `jagamex86.dll` Plan B (surgical port): g_trigger.cpp (5 fixes), g_breakable.cpp (9 fixes), g_roff.cpp (0 fixes).
3. Bridge visible NPC probes to live entity state instead of static entity-string spawn origins.
4. Continue investigating mover snapshot exclusion for:
   - `ent=2 skip=pvs-nooverflow`

**See [jagamex86_port_plan.md](E:/SOF2/OpenSOF2/jagamex86_port_plan.md) for the full plan.**

## Table-loop fix — root cause and solution

- Root cause identified:
  - `SV_SOF2CompatDispatchMoveTriggers` fires trigger, sets MOVE latch for that entity
  - `ge->ClientThink` runs BEFORE compat dispatch (order: ClientThink → compat dispatch)
  - Inside `ge->ClientThink`, retail `G_TouchTriggers` calls `SV_SOF2_AreaEntities`
  - The MOVE latch suppression block in `SV_SOF2_AreaEntities` was gated on `sv_sof2MoveTriggerFilterActive`
  - `sv_sof2MoveTriggerFilterActive` is only set during our own compat dispatch, never during retail `G_TouchTriggers`
  - Result: latch was set by compat dispatch but never checked for the retail path → entity re-fires every frame

- Fix applied in [sv_game.cpp](E:/SOF2/OpenSOF2/code/server/sv_game.cpp):
  - Removed `sv_sof2MoveTriggerFilterActive &&` guard from the MOVE latch check block
  - Now uses `latchCheckClient = filter_client ?? 0` (falls back to client 0 for SP)
  - MOVE latch now suppresses triggers for ALL callers of `SV_SOF2_AreaEntities`, including retail `G_TouchTriggers`
  - Latch still clears when player leaves trigger bounds (via `touchedThisFrame` sweep in compat dispatch)

- Expected result:
  - table flips once (or at most twice on the very first overlap frame) and stops
  - ICARUS script plays through to completion without restart loops
  - repeated `[TOUCH move] hitlist num=536` spam disappears after first fire

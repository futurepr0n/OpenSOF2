# Player Spawn Floor Trace Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the player movement bug where W alone does not move the player forward in pra2 by ensuring the player spawns with their bounding box above — not inside — the floor geometry.

**Architecture:** Single targeted edit in `ClientSpawn`: after `SelectSpawnPoint` populates `spawn_origin`, trace the player bounding box downward to find the actual floor surface and snap `spawn_origin[2]` to the standing height. A `!(spawnflags & 2)` guard skips spawn points that already handled this via the `DROPTOFLOOR` path in `SelectSpawnPoint`.

**Tech Stack:** C++ (OpenJK/SOF2 game DLL), MSVC Debug Win32, MSBuild

**Spec:** `docs/superpowers/specs/2026-03-22-player-spawn-floor-trace-design.md`

---

## File Map

| File | Action | Purpose |
|---|---|---|
| `code/game/g_client.cpp` | Modify lines 2321–2322 | Insert floor trace before spawn origin is copied to playerState |

No other files are touched.

---

### Task 1: Insert the floor trace in `ClientSpawn`

**Files:**
- Modify: `code/game/g_client.cpp:2321-2322`

- [ ] **Step 1: Read the insertion context**

  Open `code/game/g_client.cpp` and confirm the block around line 2320 looks like:

  ```cpp
  ent->client->ps.batteryCharge = 2500;

  VectorCopy( spawn_origin, client->ps.origin );
  VectorCopy( spawn_origin, ent->currentOrigin );
  ```

  Also confirm that:
  - `spawnPoint` is a `gentity_t*` in scope (assigned by `SelectSpawnPoint` at ~line 2190)
  - `playerMins` and `playerMaxs` are `vec3_t` globals visible at file scope (~line 60)
  - This code is inside the `else` branch that begins at ~line 2184

- [ ] **Step 2: Insert the floor trace block**

  Insert the following block between `batteryCharge = 2500;` and `VectorCopy( spawn_origin, client->ps.origin )`:

  ```cpp
  // Trace the player bbox down to the actual floor to prevent spawning embedded
  // in geometry.  Without this, spawn points that lack the DROPTOFLOOR flag (spawnflags&2)
  // place the player at entity_Z + 9, leaving the bbox bottom 15 units inside the floor.
  // PM_GroundTrace then hits allsolid/startsolid → PM_AirMove is used instead of
  // PM_WalkMove → W key forward movement appears broken.
  // Guard: DROPTOFLOOR spawn points are already traced correctly by SelectSpawnPoint.
  if ( spawnPoint && !(spawnPoint->spawnflags & 2) )
  {
  	trace_t floorTr;
  	vec3_t  floorStart, floorEnd;

  	VectorCopy( spawn_origin, floorStart );
  	floorStart[2] += 18.0f;   // start above to escape any embedded position
  	VectorCopy( spawn_origin, floorEnd );
  	floorEnd[2]   -= 256.0f;  // search 256 units down for the floor

  	gi.trace( &floorTr, floorStart, playerMins, playerMaxs, floorEnd,
  	          ENTITYNUM_NONE, MASK_PLAYERSOLID, (EG2_Collision)0, 0 );

  	Com_Printf( "[SPAWN] Floor trace: startsolid=%d allsolid=%d fraction=%.3f "
  	            "originZ %.1f -> %.1f\n",
  	            floorTr.startsolid, floorTr.allsolid, floorTr.fraction,
  	            spawn_origin[2], floorTr.endpos[2] );

  	if ( !floorTr.startsolid && !floorTr.allsolid && floorTr.fraction < 1.0f )
  	{
  		spawn_origin[2]  = floorTr.endpos[2];
  		spawn_origin[2] += 1.0f;  // 1-unit clearance against fp precision at contact
  	}
  }
  ```

  > **Indentation:** use tabs to match the surrounding code style.

- [ ] **Step 3: Build the game DLL**

  ```
  cd E:/SOF2/OpenSOF2/build_test/code/game
  MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe" jagamex86.vcxproj /p:Configuration=Debug /p:Platform=Win32 /t:Build /v:minimal
  ```

  Expected output (last lines):
  ```
  g_client.cpp
  jagamex86.vcxproj -> E:\SOF2\OpenSOF2\build_test\Debug\jagamex86.dll
  ```

  If the build fails:
  - C2039 / C2065: `playerMins`/`playerMaxs` not in scope → check they are declared at file scope and not inside a namespace block
  - C2664 (argument mismatch): verify the `gi.trace` call includes `(EG2_Collision)0, 0` as the final two arguments
  - Any other error: recheck indentation and brace matching around the inserted block

- [ ] **Step 4: Run the 35-second pra2 test**

  ```
  cd E:/SOF2/OpenSOF2/build_test/Debug
  timeout 35 ./openjk_sp.x86.exe +set fs_game "" +devmap pra2 \
      > run_spawnfix_out.log 2> run_spawnfix_err.log
  echo "Exit: $?"
  ```

  Expected: `Exit: 124` (timeout — no crash)

- [ ] **Step 5: Verify the floor trace fired**

  ```
  grep "Floor trace" run_spawnfix_out.log
  ```

  Expected — a line like:
  ```
  [SPAWN] Floor trace: startsolid=0 allsolid=0 fraction=0.xxx originZ 441.0 -> 417.0
  ```

  - `fraction` must be < 1.000 (floor found)
  - `startsolid` and `allsolid` must both be 0
  - The two Z values must differ (adjustment was made)

  If `fraction=1.000`: the 256-unit search depth found no floor. Increase `floorEnd[2] -= 256.0f` to `512.0f` and rebuild.
  If `startsolid=1`: the trace started inside solid. Increase `floorStart[2] += 18.0f` to `36.0f` and rebuild.

- [ ] **Step 6: Verify W-key movement manually**

  Launch the game interactively (without `timeout`):
  ```
  cd E:/SOF2/OpenSOF2/build_test/Debug
  ./openjk_sp.x86.exe +set fs_game "" +devmap pra2
  ```

  Press W. The player should move forward without needing to jump first.

  If movement still requires jump: check the log — if the Floor trace line shows `fraction=1.000` or `startsolid=1`, revisit Step 5's fallback. If the trace fired correctly but movement is still broken, the root cause may differ from the ground-detection hypothesis; re-open the brainstorming doc.

- [ ] **Step 7: Commit**

  ```bash
  cd E:/SOF2/OpenSOF2
  git add code/game/g_client.cpp
  git commit -m "$(cat <<'EOF'
  fix: trace player bbox to floor at spawn to fix W-key forward movement

  Player spawning at entity_Z+9 left the bbox bottom 15 units embedded in the
  floor (bbox extends 24 below origin). PM_GroundTrace hit allsolid, used
  PM_AirMove (accel=4) instead of PM_WalkMove (accel=12), and forward velocity
  was clipped by surrounding geometry — making W-key movement non-functional.

  In ClientSpawn, after SelectSpawnPoint, trace the player bbox 256 units down
  to find the actual floor surface and snap spawn_origin[2] to the contact point
  (+1 unit clearance). Guard skips spawn points with DROPTOFLOOR (spawnflags&2)
  already handled correctly by SelectSpawnPoint.

  Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
  EOF
  )"
  ```

---

## Done

After the commit, the player can walk forward with W alone in pra2. Remaining issues
(slot 149 NULL, missing world models, first-person weapons, table drift) are independent
and tracked in `build_test/Debug/SESSION_REPORT.md`.

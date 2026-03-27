# OpenSOF2 Remaining Issues

**Last Updated:** 2026-03-27
**Session:** Fixed invisible ET_GENERAL models via deferred configstring registration
**Status:** Game stable 35s+ in interactive testing; core rendering now functional

---

## Issue 1: Model Animation Flashing

**Symptom**: Some in-game models (doors, breakable walls, vehicles) animate between two states every frame instead of smoothly transitioning or staying in one state.

**Example**: Door appears open and closed alternately each frame; gas tank explodes and un-explodes each frame.

**Root Cause**: Static GLM render system submitting models out-of-PVS WHILE native cgame also renders them in-snapshot simultaneously.
- Expected: Static GLM submits ONLY when `framesAbsent > 0` (entity not in snapshot)
- Actual: Both systems rendering same entity = flashing between two animation frames

**Fix Applied**: Added `framesAbsent==0` skip in CL_CGameRendering → CG_AddStaticGlmProps (code/client/cl_cgame.cpp)
- Should prevent submission when entity currently visible in snapshot

**Status**: Fix applied but not yet verified in interactive test

**Next Steps**:
1. Run interactive test and observe if door/gas tank still flash
2. If still flashing: Add logging to CG_AddStaticGlmProps to confirm `framesAbsent==0` condition is working
3. Check if entity is actually IN the snapshot (compare entity numbers in snapshot vs static GLM list)
4. Verify snapshot building correctly sets entity positions (baseline at START of animation, not END)

**Files**:
- code/client/cl_cgame.cpp (CG_AddStaticGlmProps, CG_StaticGlm_Register, framesAbsent counter)

---

## Issue 2: Interactive Models Positioned at End-State

**Symptom**: Animated entities (vehicles, movers) that should start at origin/closed position instead appear at their final animated position.

**Example**: Table flip animation trigger — table appears at final flipped position instead of original flat position; personnel truck at endpoint instead of spawn location.

**Root Cause** (Hypothesis): One or more of:
1. **Baseline position wrong**: Baseline snapshot sets position to end-state instead of start-state
2. **ROFF animation state**: Entity's pos.trBase or animation frame initialized to final value
3. **Smooth animation disabled**: ICARUS/ROFF script not running from frame 0

**Investigation Needed**:
1. Check baseline snapshot for entity position vs expected spawn origin
2. Verify pos.trBase and pos.trDelta values in snapshot (should interpolate FROM trBase TO trBase+trDelta)
3. Check ICARUS script execution order (does entity run ROFF animation from map load?)
4. Compare with native SOF2 - does it start animations immediately or wait for first frame?

**Related Code**:
- sv_snapshot.cpp: Entity position encoding (trBase, trDelta, trTime, trDuration)
- g_client.cpp: SelectSpawnPoint, spawn position validation
- ICARUS script system: Animation triggering

**Files**:
- code/server/sv_snapshot.cpp (SV_BuildSnapshotForClient entity position encoding)
- code/game/g_client.cpp (ClientSpawn, SelectSpawnPoint)

---

## Issue 3: Blue Beams

**Symptom**: Thick blue lines visible in-game, stretching from entity origins toward world origin or other locations.

**Root Cause**: `target_laser` entities (SOF2-specific) set eType=ET_BEAM(5). Native SOF2 cgame's CG_Beam renders them incorrectly (wrong material, wrong endpoint).

**Fix Applied**: Filter eType=5 entities from snapshots in sv_snapshot.cpp line 876-877
- Entities still function server-side (trace, damage)
- Client-side rendering suppressed

**Status**: Filtered from snapshots; unknown if still visible via other rendering path

**Investigation Needed**:
1. Verify filter is catching all target_laser entities in log output
2. Check if CG_Beam is called from anywhere else (CG_AddEntities, CG_DrawInformation)
3. Confirm blue beams don't appear in next interactive test

**Files**:
- code/server/sv_snapshot.cpp (line 876-877, eType==5 filter)

---

## Issue 4: Player Model Not Loading

**Symptom**: No visible first-person weapon or body model; player appears as floating viewpoint.

**Root Cause**: OpenJK game DLL references JK2 humanoid player models (jedi_tf.glm, stormtrooper.glm, etc.) which don't exist in SOF2 data directory.

**Status**: Not critical for gameplay; deprioritized

**Potential Solutions**:
1. Use native SOF2 player models (if available in data dir)
2. Create minimal placeholder model to avoid crashes
3. Accept floating viewpoint as known limitation

**Files**:
- code/game/g_client.cpp (G_SetG2PlayerModel fallback logic)
- Requires asset data inspection

---

## Issue 5: NPC Model Skins - Missing Attachments

**Symptom**: NPC characters missing hats, hoods, gear bolt-on attachments (only base body visible).

**Root Cause**: SOF2 used bolt-on attachment system (attach hat.glm to head bolt, etc.). OpenJK Ghoul2 system doesn't implement this.

**Status**: Non-critical visual issue; deprioritized

**Architectural Complexity**: Would require implementing bolt-on system in Ghoul2 integration code (code/client/cl_cgame.cpp Ghoul2 wrappers).

---

## Issue 6: Animation Error Spam (Non-Fatal)

**Symptom**: Console log shows `PM_SetAnimFinal: Invalid Anim File Index (0)!` repeatedly.

**Root Cause**: PM_SetAnimFinal calls ValidAnimFileIndex which stubs to qfalse (no JK2 animation data), returns early safely.

**Status**: Non-fatal; already has guard. Can ignore.

---

## Issue 7: SOF2-Specific Entities Silently Skipped

**Symptom**: Map entities like `pickup_ammo`, `fx_play_effect`, `model_static` not found, spawn fails silently.

**Root Cause**: OpenJK spawn table doesn't have these entity types. Game DLL spawn functions in g_misc.cpp define them but G_CallSpawn can't find them in OpenJK's spawn_t table.

**Status**: Expected; entities are implemented via workaround functions in g_misc.cpp (SP_pickup_*, SOF2_SpawnPickup, SP_model_static, etc.)

---

## Critical Path for Next Session

1. **Interactive Test** (Required): Run +devmap pra2 with W key movement, observe:
   - [ ] Models render correctly (no invisibility)
   - [ ] Door/gas tank animations (flashing or smooth?)
   - [ ] Table position (start or end state?)
   - [ ] Blue beams present?
   - [ ] Game stability (crash time if any)

2. **If Still Flashing**: Add diagnostic logging to CG_AddStaticGlmProps (check framesAbsent condition)

3. **If Still End-State**: Check baseline snapshot positions in qconsole.log vs expected spawn origins

4. **Blue Beams**: Verify absence in interactive test; check log for eType=5 filter hits

---

## Build & Test Commands

```bash
# Build engine
cd E:/SOF2/OpenSOF2/build_test/code
MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe" \
    openjk_sp.x86.vcxproj /p:Configuration=Debug /p:Platform=Win32 /t:Build /v:minimal

# Run interactive test (35s timeout)
cd E:/SOF2/OpenSOF2/build_test/Debug
timeout 35 ./openjk_sp.x86.exe +set fs_game "" +devmap pra2

# Check output logs
tail -300 "C:\Users\Mark\Documents\My Games\OpenSOF2\base\qconsole.log"
grep -i "ET_GENERAL\|RegisterMissingModels\|flashing\|STATIC_GLM" run_model_fix_out.log
```

---

## Memory Files & Documentation

- **Memory**: C:\Users\Mark\.claude\projects\E--SOF2\memory\project_openjk_devmap_fixes.md (detailed fixes log)
- **Build Info**: E:\SOF2\OpenSOF2\build_test\Debug\SESSION_REPORT.md (earlier session notes)


# OpenSOF2 Remaining Issues

**Last Updated:** 2026-04-03
**Session:** HUD health/armor bars, STAT_ARMOR index fix, player body animations

---

## ✅ RESOLVED — Issue 1: Model/Door Animation Flashing

**Fix Applied 2026-03-24** (`code/server/sv_snapshot.cpp`): TR_NONLINEAR_STOP(4) → TR_LINEAR_STOP(3)
trType translation prevents doors oscillating sinusoidally in SOF2 cgame.

**Fix Applied 2026-04-03** (`code/client/cl_cgame.cpp`): Player body now plays idle/walk/run
animation via `CG_PlayerBody_SetAnim()` called from `CG_AddPlayerBodyToScene()`.
- Idle: speed < 15 u/s (frames 25920–25960 @ 10fps)
- Walk: speed 15–180 u/s — Shift+W (frames 10827–10850 @ 20fps)
- Run: speed > 180 u/s — W key default (frames 10805–10816 @ 20fps)
- Bone "model_root" confirmed in cgamex86.dll via Ghidra

**Status: RESOLVED** ✅

---

## Issue 2: Interactive Models Positioned at End-State

**Symptom**: Animated entities (table flip, personnel truck) appear at their final animated
position instead of their starting position when the map first loads.

**Root Cause** (Hypothesis): ICARUS scripts start running at entity spawn time. By the time
the player loads and receives the first snapshot, the animation has already completed.
In native SOF2, these animations are likely triggered by player proximity / cutscene triggers.

**Investigation Needed**:
1. Identify which ICARUS script drives the truck (pra2/truck entity #190 script)
2. Check if the script runs immediately or waits for a trigger entity
3. Compare entity position at first snapshot vs expected spawn origin
4. If script runs immediately: find the trigger entity and ensure it only fires after player spawns

**Files**:
- `code/server/sv_snapshot.cpp` (entity position encoding at snapshot build time)
- `code/icarus/` (ICARUS script execution, trigger conditions)
- `E:/SOF2/Soldier of Fortune 2/base/scripts/pra2/` (map scripts)

---

## Issue 3: Blue Beams

**Symptom**: Thick blue lines visible in-game stretching from entity origins.

**Fix Applied 2026-03-24**: Filter eType=5 entities from snapshots in `sv_snapshot.cpp` line 876.
Entities still function server-side; client-side rendering suppressed.

**Status**: Filter applied. Source may persist via other rendering paths (SOF2 cgame FX system,
NPC bolt rendering). Needs visual verification in interactive test.

**Investigation Needed** (if still visible):
1. Check if any eType=5 entities are passing the filter (add log on filter hit)
2. Check CG_Beam call paths in cgamex86.dll (are they called from CG_AddEntities or elsewhere?)

**Files**:
- `code/server/sv_snapshot.cpp` (line 876, eType==5 filter)

---

## Issue 4: HUD Visual Fidelity

**Symptom**: Health and armor bars show as solid-color rectangles. Native SOF2 uses the METIS
UI framework with texture-based widget images, ammo counter, weapon icon, etc.

**Current State (2026-04-03)**:
- Health bar: red rectangle, centered bottom, full-width
- Armor bar: gunmetal rectangle, same width, directly above health bar
- Both bars stable (no flashing), correct values, update live with damage/heal

**METIS HUD Architecture** (for future reference):
- All METIS widget updates flow through slot 26 (`cgi_UI_SetActiveMenu`)
- HUD element names: hud_health, hud_armor, hud_weapon, hud_ammo, hud_ammo_clip, hud_fps
- hud_health/hud_armor use 0–10 float slider scale
- Replicating the full METIS HUD would require reverse-engineering the SOF2 texture atlas
  and widget positioning data — significant effort, deprioritized

**Status**: Functional placeholder. Deprioritized unless texture fidelity becomes a priority.

---

## Issue 5: NPC Model Skins — Missing Attachments

**Symptom**: NPC characters missing hats, hoods, and gear bolt-on attachments.

**Root Cause**: SOF2 used a bolt-on attachment system (attach hat.glm to head bolt, etc.).
OpenJK Ghoul2 system does not implement this.

**Status**: Non-critical visual issue. Deprioritized.

---

## Issue 6: Animation Error Spam (Non-Fatal)

**Symptom**: Console shows `PM_SetAnimFinal: Invalid Anim File Index (0)!` repeatedly.

**Root Cause**: ValidAnimFileIndex stubs to qfalse; PM_SetAnimFinal returns early safely.

**Status**: Non-fatal, has guard. Ignore.

---

## Issue 7: SOF2-Specific Entities Silently Skipped

**Symptom**: `pickup_ammo`, `fx_play_effect`, `model_static` spawn fails silently for
entity types not in OpenJK spawn table.

**Status**: Expected limitation. Workaround functions in g_misc.cpp handle the common cases.

---

## Issue 8: Some Pickups Not Solid

**Symptom**: Some pickup items are visible but cannot be collected (solid=0x0).

**Root Cause**: `spawnflags=8` is interpreted as `ITMSF_NOTSOLID` in the SOF2 pickup logic.
Items with this flag are rendered but have no collision box.

**Investigation Needed**:
1. Find which spawnflags value in SOF2 maps corresponds to ITMSF_NOTSOLID
2. Check if the spawnflags bits are being remapped correctly between SOF2 and JK2 item spawn

**Files**:
- `code/game/g_misc.cpp` (SOF2_SpawnPickup, ITMSF_NOTSOLID check)
- `code/game/items.h` (ITMSF_* flag definitions)

---

## Build & Test Commands

```powershell
# Build game DLL
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe' `
  'E:\SOF2\OpenSOF2\build_test\code\game\jagamex86.vcxproj' `
  /p:Configuration=Debug /p:Platform=Win32 /t:Build /nologo /v:minimal

# Build engine
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe' `
  'E:\SOF2\OpenSOF2\build_test\code\openjk_sp.x86.vcxproj' `
  /p:Configuration=Debug /p:Platform=Win32 /t:Build /nologo /v:minimal

# Run interactive test
Start-Process 'E:\SOF2\OpenSOF2\build_test\Debug\openjk_sp.x86.exe' `
  -ArgumentList '+devmap pra2' -WorkingDirectory 'E:\SOF2\OpenSOF2\build_test\Debug'

# Check output logs
Get-Content 'C:\Users\Mark\Documents\My Games\OpenSOF2\base\qconsole.log' -Tail 100
```

---

## Memory Files & Documentation

- **Fixes log**: `C:\Users\Mark\.claude\projects\E--SOF2\memory\project_openjk_devmap_fixes.md`
- **Architecture notes**: `C:\Users\Mark\.claude\projects\E--SOF2\memory\MEMORY.md`
- **Session reports**: `E:\SOF2\OpenSOF2\build_test\Debug\SESSION_REPORT*.md`

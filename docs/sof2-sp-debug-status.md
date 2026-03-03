# SOF2 SP Debug Status

Current milestone reached on March 3, 2026:

- Direct SP map boot reaches `CA_ACTIVE`.
- `air1` renders visible 3D world geometry in `openjk_sp.x86.exe`.
- Mouse-look works in live play.
- `Esc` no longer tears the scene down by reopening the broken ingame menu path.

## What Was Fixed

- Corrected several SOF2 engine/game/cgame ABI mismatches that blocked startup.
- Fixed SOF2 SP player initialization so direct `+map` loading no longer dies in `SV_ClientThink`.
- Restored the correct SOF2 render export path and scene bridge so active frames submit sane render-scene data.
- Added a temporary renderer compatibility fallback in the BSP visibility path so maps with broken SOF2 PVS/areamask integration still render world geometry.
- Corrected the active render camera orientation bridge so submitted scene axes follow `cl.viewangles`, which restored visible mouse-look.
- Suppressed the unstable SOF2 ingame menu path during active play to stop `Esc` from breaking the scene.
- Added temporary filtering for `Quickloading...` spam to reduce UI/notify interference during gameplay debugging.

## Current Known Issues

- Visible scene flicker remains.
- `W/A/S/D` movement is still not working, although mouse-look works and left/right arrow camera movement works.
- The SOF2 ingame menu is still unstable and intentionally blocked during active play.
- The renderer fallback currently forces world visibility when normal traversal finds zero surfaces; the real SOF2 PVS/areamask path still needs a proper fix.
- Various SOF2 content compatibility warnings remain, including shader extensions and some Ghoul2/material issues.

## Key Runtime Findings

- `CL_FirstSnapshot` reaches `CA_ACTIVE`.
- `CL_GetSnapshot` succeeds with live entity counts.
- `R_RenderScene` receives sane dimensions, FOV, and view origin.
- Without the temporary world fallback, active frames built effectively empty world scenes.
- Raw mouse input reaches the client and updates `cl.viewangles`.
- The on-screen camera previously ignored those angles until the render-scene bridge forced active `viewaxis` from `cl.viewangles`.

## Important Temporary Compatibility Hacks

- `code/rd-vanilla/tr_world.cpp`
  - forces all BSP nodes visible when the first world traversal produces zero draw surfaces
- `code/client/cl_cgame.cpp`
  - forces active submitted `viewaxis` from `cl.viewangles`
  - suppresses unstable SOF2 active-play menu opens from the cgame side
- `code/qcommon/common.cpp`
  - filters `Quickloading...` print spam
- `code/ui/ui_atoms.cpp`
  - blocks `ingame` menu activation during active play
- `code/client/cl_keys.cpp`
  - ignores `Esc` during active play for now

## Next Recommended Work

1. Fix the remaining alpha-key movement path so `W/A/S/D` movement binds work.
2. Replace the forced-all-visible renderer fallback with a correct SOF2 PVS/areamask integration.
3. Isolate the remaining scene flicker after the loading/menu overlays have been suppressed.
4. Re-enable a stable ingame menu path once active gameplay is no longer dependent on compatibility guards.

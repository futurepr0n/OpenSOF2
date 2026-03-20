# NPC Model Loading Status

## Goal

Get SOF2 character models to appear in-map at their actual world positions, using the real `.glm` + `.g2skin` combinations from the level data, so we can later replace the probe path with the real retail NPC/player path.

## Current State

- Real map-position character probes are now being built from the `pra2` entity string.
- Real Ghoul2 character models are reaching the renderer.
- SOF2 `.g2skin` material-group mapping is now partially working in the renderer.
- Czech soldier / rain-soldier probes are visible in-world.
- Mullins (`suit_long_coat`) is also reaching the renderer and skin fallback is active.
- Mullins should be treated as the player-side model family reference, not as the desired visible NPC outcome for `pra2`.
- This is still a debug probe path, not the real retail NPC gameplay/render path.

## What Was Added

### 1. Map NPC probe extraction

File:
- `code/client/cl_cgame.cpp`

Behavior:
- Parses `CM_EntityString()` for `NPC_*` entities.
- Captures:
  - `classname`
  - `setskin`
  - `origin`
  - `angles`
- Derives `.g2skin` from `classname` / `setskin`.
- Reads the `.g2skin` `prefs/models` block to determine the real character family.
- Builds the `.glm` path from that family.

Examples seen in `pra2`:
- `NPC_Mullins_Young -> models/characters/suit_long_coat/suit_long_coat.glm`
- `NPC_Czech_Rain_Soldier_* -> models/characters/snow/snow.glm`
- `NPC_Czech_Soldier -> models/characters/average_sleeves/average_sleeves.glm`

Important interpretation:
- `NPC_Mullins_Young` is still useful as a model/skin validation target in the probe path.
- The actual gameplay goal remains:
  - Mullins as the first-person player model path
  - Czech/rain-soldier families as the visible world NPCs

### 2. Ghoul2 probe setup

File:
- `code/client/cl_cgame.cpp`

Behavior:
- Registers model + skin.
- Initializes Ghoul2 with `re.G2API_InitGhoul2Model(...)`.
- Applies the selected skin with `re.G2API_SetSkin(...)`.
- Starts a simple looping bone anim on `model_root`.
- Submits the probe as a normal `refEntity_t` at the NPC's world origin.

### 3. SOF2 `.g2skin` surface fallback

File:
- `code/rd-vanilla/tr_ghoul2.cpp`

Problem solved:
- SOF2 `.g2skin` files use logical material group names like:
  - `body`
  - `arms`
  - `face`
  - `hood`
  - `caps`
- Ghoul2 mesh surfaces use physical names like:
  - `hip_l`
  - `arm_uppr_r`
  - `head_side_l`
  - `mouth_r`
- Exact matching alone leaves characters white / blocky.

Renderer fallback now maps:
- lower body / torso / feet / coat -> `body`
- arms / hands / fingers -> `arms`
- front-face surfaces -> `face`, then `face_2sided`, then scalp materials only as fallback
- side/back head and ears -> `head`, then `avmed`, then facial materials only as last resort
- hood -> `hood`
- backpack -> `backpack_lrg`
- scarf -> `scarf`
- helmet/chin strap -> `helmet_chin_strap`, then `caps`
- hat/cap -> `caps`

Confirmed by log lines such as:
- `[SOF2 g2] skin fallback model='models/characters/suit_long_coat/suit_long_coat.glm' surf='torso_r' alias='body' ...`
- `[SOF2 g2] skin fallback model='models/characters/snow/snow.glm' surf='hip_l' alias='body' ...`

## Current Debug Cvars

From `cl_cgame.cpp`:
- `sof2_debugCharacterProbe`
  - default `1`
  - enables the map-position NPC probe system
- `sof2_debugCharacterProbeFocus`
  - default `0`
  - old camera-fixed focus copy; disabled by default because it obscures real world-position testing
- `sof2_debugCharacterProbeNoCull`
  - default `1`
  - debug-only cull bypass for the Ghoul2 probe path

## Important Current Limitation

The visible NPCs are still part of a debug probe path:
- they are not driven by the retail `cgamex86.dll` NPC state path yet
- they do not necessarily animate/move the way the real gameplay NPCs should
- the first-person Mullins/player body and weapon are still not integrated

## Current Problems Still Open

### 1. Probe stability

Observed:
- the major flicker was stopped by injecting `CG_DebugAddCharacterProbe(&s_lastGoodRefdef)` on the fallback render path before `re.RenderScene(...)`
- the probe path is still debug-only and must be kept stable while skin issues are resolved

Files involved:
- `code/client/cl_cgame.cpp`
- `code/rd-common/tr_types.h`
- `code/rd-vanilla/tr_ghoul2.cpp`

### 2. Average sleeves skin gaps

Observed in log:
- repeated misses on:
  - `collar_l`
  - `collar_r`

Example:
- `skin miss model='models/characters/average_sleeves/average_sleeves.glm' surf='collar_l' ...`

Interpretation:
- the current alias table still needs additional mapping for `average_sleeves` neck/collar surfaces, likely to `body`, `scarf`, or another material group used by the Czech soldier skin.

### 3. Mullins front-face / scalp split

Observed:
- the side/back-of-head artifact is fixed when scalp surfaces use `head` / `avmed`
- the front face can still drop out if `head_frnt_*` falls through to scalp materials too early

Interpretation:
- `head_frnt_*` should prefer facial materials first:
  - `face`
  - `face_2sided`
- only then should it fall back to scalp materials:
  - `head`
  - `avmed`

### 4. Real retail NPC path still not active

The current visible result does **not** mean the retail NPC/player render path is fixed. It means:
- model loading
- Ghoul2 init
- skin binding
- world-position probe submission

are working well enough to debug further.

## What The Latest Logs Prove

From the recent `qconsole.log`:
- Mullins probe:
  - model path correct
  - skin path correct
  - Ghoul2 reaches renderer
  - suit_long_coat fallback now resolves
- rain-soldier probes:
  - snow model path correct
  - rainsuit skin path correct
  - body/arms/face fallback now resolves
- Czech soldier probes:
  - `average_sleeves` model path correct
  - Czech skin path correct
  - still missing collar surfaces

## Recommended Next Steps

1. Keep the probe render path stable.
   - preserve the user-confirmed fallback render-path probe injection that stopped the major flicker
   - keep the dedicated no-cull flag limited to early Ghoul2 cull bypass, not depth disable

2. Finish the `average_sleeves` skin alias table.
   - map `collar_l` / `collar_r`
   - inspect whether `backpack_lrg` / `scarf` should be applied more broadly

3. Finish the Mullins front-face / scalp split.
   - keep `head_side_*` / `head_bck_*` on scalp materials
   - keep `head_frnt_*` on facial materials

4. Only after the probe path is visually stable:
   - return to the real retail NPC/player path
   - compare retail `cgamex86.dll` character submit behavior with the now-working probe setup
   - bridge the real player/NPC entities into the same successful Ghoul2 + skin setup

## Key Files

- `E:\\SOF2\\OpenSOF2\\code\\client\\cl_cgame.cpp`
- `E:\\SOF2\\OpenSOF2\\code\\rd-vanilla\\tr_ghoul2.cpp`
- `E:\\SOF2\\OpenSOF2\\code\\rd-common\\tr_types.h`
- `E:\\SOF2\\OpenSOF2_model_ref_8c16a0c5\\code\\client\\cl_scrn.cpp`

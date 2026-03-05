Current issue summary for another agent:

We are debugging `E:\SOF2\OpenSOF2`, specifically SP direct map load with:
```powershell
E:\SOF2\OpenSOF2\build_test\Debug\openjk_sp.x86.exe +map air1
```

Current runtime state before this new bootstrap work:
- `CA_ACTIVE` is reached
- world rendering works
- mouse-look works
- flicker was fixed by the user
- movement keys generate valid cmds and the server receives them
- but the player remains frozen in place

Key confirmed evidence from `qconsole.log`:
- `[CL move]` shows nonzero movement cmds
- `[SV cmd]` shows those cmds reaching `SV_ClientThink`
- `[SV ps]` and `[CL ps]` show:
  - static `origin`
  - static `velocity`
  - `pm_type=2`
- so input is not the problem; frozen player state is

My analysis:
- The strongest current theory is missing SP mission/bootstrap state on direct `+map`
- SOF2 menu assets show that `air1` is normally started through:
  - `menus/select_air1.cfg`
  - which sets `world_map`, `current_mission`, `current_mission_inv`, `default_wpns`
  - executes `menus/reset.cfg`
  - and the default loadout file `menus/default_air_wpn.cfg`
- direct `+map air1` bypasses all of that
- that makes it plausible that spawn-time state like `mi_health`, `mi_armor`, weapon selection, mission cvars, etc. are never initialized properly for the game DLL

I do not consider one part of the separate analysis fully proven:
- the claim that `ge->ClientThink( cl - svs.clients )` is definitely passing a null/bad entity pointer and immediately returning
- runtime behavior suggests the DLL is doing real work after that call, so I treat that as unverified

What I implemented:
- in `code/server/sv_init.cpp`
- I added a guarded SOF2 direct-map bootstrap path:
  - `SV_SOF2_ApplyBootstrapCfg(...)`
  - `SV_SOF2_SelectCfgForMap(...)`
  - `SV_SOF2_SeedDirectMapBootstrap(...)`
- current behavior:
  - only for `air1`
  - only when `current_mission` is blank
  - replays the cfg subset used by the SOF2 menu scripts:
    - `set*`
    - `select`
    - `exec`
    - ignores `wait` / `intermission`
- it is hooked in `SV_SpawnServer` before `SV_InitGameProgs()`, so the game DLL should see the seeded cvars during spawn / `ClientBegin`

Why this has not been validated yet:
- I cannot get a fresh `openjk_sp.x86.exe` built because the build tree is stuck on MSVC debug artifacts
- the current blocker is:
  - `build_test\code\openjk_sp.x86.dir\Debug\vc143.pdb`
- build failures include:
  - denied access to `.tlog` files
  - then `C1041` on shared PDB writes
- I cleared stale `.tlog` directories successfully
- now no obvious `mspdbsrv`, `cl`, `MSBuild`, `devenv`, `Code` processes are running
- but the PDB/build state is still effectively wedged

What another agent should know:
- source patch exists in `code/server/sv_init.cpp`
- it has not been runtime-tested because the exe was not rebuilt successfully
- this is the exact hypothesis being tested:
  - if missing SOF2 mission/loadout cvars are the cause, seeding them before game init should change spawn-time state and ideally unfreeze the player
- the next validation signal in `qconsole.log` should be:
  - `[SOF2 bootstrap] seeding direct map bootstrap for air1 ...`
  - `[SOF2 bootstrap] result: ...`
  - followed by whether `pm_type` remains `2` or normalizes

Open question for the other agent:
- best way to get a clean build from this repo state without fighting the current PDB/tlog lock
- and whether the bootstrap patch itself is the right minimal test, or if they see a cleaner way to validate the direct-map initialization gap

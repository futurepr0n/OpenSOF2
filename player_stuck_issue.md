  1. The Two-Layer Problem
                                                                                                                          Movement is blocked by two independent failures, both of which must be fixed:

  Layer 1 — ClientThink receives a NULL entity pointer

  In sv_client.cpp:
  ge->ClientThink( cl - svs.clients );  // passes 0 (integer index cast to void*)

  SOF2's game DLL ClientThink(void *ent) signature expects a CEntity/CPlayer C++ object pointer. The first thing it
  does:
  piVar3 = ___RTDynamicCast(ent, 0, &CEntity_Type, &CPlayer_Type, 0);
  if (piVar3 == NULL) return;   // ← exits immediately
  With ent=0 (NULL), the RTTI cast fails, ClientThink returns before calling Pmove at all. No movement possible
  regardless of health.

  Layer 2 — Health is 0 at CPlayer+0x24C

  Even with the entity pointer fixed, ClientThink immediately checks health:
  iVar6 = (*piVar3->vtable[1])(0);  // GetHealth() — reads CPlayer+0x24C
  if (iVar6 < 1) {
      ent->pm_type = 2;             // PM_DEAD
  }
  G_ClientThink(&piStack_204);      // calls Pmove
  With health=0, pm_type is forced to PM_DEAD. Pmove runs but in dead-player mode — no player-controlled movement, only
  passive physics (gravity, friction).

  ---
  2. Why Health is 0

  CPlayer+0x24C is a field in the C++ CPlayer object (not in gclient/playerState). It starts at whatever value the
  constructor or allocator left it (effectively 0 on a fresh spawn).

  The normal initialization path (via menu):

  Menu → mission select → select_air1.cfg runs:
    setm mi_health 100          ← sets cvar mi_health = "100"
    setm current_mission ...
    exec menus/default_air_wpn.cfg  ← sets weapon pool cvars
  → intermission select_new → map loads
  → ClientBegin → CPlayer_Spawn → reads mi_health cvar → sets CPlayer+0x24C = 100

  Direct +map air1 skips everything:

  - mi_health cvar is never set (or is empty/"0")
  - CPlayer_Spawn reads it → gets 0 → CPlayer+0x24C stays 0
  - ClientThink sees health < 1 → forces PM_DEAD

  Confirmed via decompilation: CPlayer_UpdateMissionInfo goes the other direction — it writes mi_health from
  CPlayer+0x24C (output for HUD display). It does not read it in.

  ---
  3. What Data the Game DLL Needs (from cvars)

  At minimum for air1:

  ┌─────────────────────┬────────────────────────────┬─────────────────────────────────────────────┐
  │        Cvar         │           Value            │                   Purpose                   │
  ├─────────────────────┼────────────────────────────┼─────────────────────────────────────────────┤
  │ mi_health           │ 100                        │ Initial player health read by CPlayer_Spawn │
  ├─────────────────────┼────────────────────────────┼─────────────────────────────────────────────┤
  │ mi_weaponpool       │ (unknown bitmask)          │ Which weapons exist in the mission pool     │
  ├─────────────────────┼────────────────────────────┼─────────────────────────────────────────────┤
  │ Various select wp_* │ set by default_air_wpn.cfg │ Which specific weapons player starts with   │
  ├─────────────────────┼────────────────────────────┼─────────────────────────────────────────────┤
  │ mi_armor            │ (unknown)                  │ Possibly also read during spawn             │
  ├─────────────────────┼────────────────────────────┼─────────────────────────────────────────────┤
  │ current_mission     │ missions/airport           │ Mission script path                         │
  └─────────────────────┴────────────────────────────┴─────────────────────────────────────────────┘

  The weapon pool is the big unknown — we need to find out exactly what cvars select wp_ussocom, select wp_oicw etc.
  actually set.

  ---
  4. The Cascade into the Cgame Renderer

  This is why the previous fix attempt caused rendering to break:

  With pm_type=0 (alive), the cgame's DrawInformation takes the alive player rendering path, which:
  1. Looks up ps.weapon (= 1, the USSOCOM pistol)
  2. Calls into the weapon system: CWpnSys::GetWeaponInstance(1)
  3. Gets back a CWpnInst* pointer — which is NULL because the weapon pool was never initialized via the proper cvar
  path
  4. Tries to call a virtual method on that NULL: call [ecx] where ecx=0
  5. Crash at 0x3000AA50, before R_RenderScene → blank screen

  With pm_type=2 (dead, our previous broken state), the cgame skips weapon rendering for dead players — so it never hits
   the NULL weapon instance path. The crash that does occur happens later (after R_RenderScene), causing only flickering
   rather than a blank screen.

  The fallback render fix we just applied handles the blank-screen case. But the proper fix removes the crash entirely.

  ---
  5. The Correct Fix Sequence

  To fix movement properly without breaking rendering:

  1. Set mi_health (and weapon pool cvars) in the engine before ClientBegin is called — so CPlayer_Spawn reads the right
   values and initializes both health and weapons properly
  2. Fix the ClientThink entity pointer — change cl - svs.clients back to cl->gentity
  3. With both done: health=100 → pm_type=0 → Pmove runs → movement works; weapons initialized → cgame weapon rendering
  doesn't crash → no blank screen
I have continued work with another agent, and got further with NPC character loading while abandoninig the HUD altogether for now. I would like your help as I am experiencing those NPC model characters flicker in and out of view when rendering the scene. Our Codex Agent thought the flicker diagnosis was related to an issue in the log which showed two render paths alternating:

  - direct CG_R_RENDERSCENE
  - wrapped [SOF2 RS138] legacy render path

  The NPC probes were only guaranteed on the wrapped path, which is why they could appear/disappear as the frame loop
  alternated. They changed /E:/SOF2/OpenSOF2/code/client/cl_cgame.cpp so case CG_R_RENDERSCENE: now goes through
  CG_SubmitSOF2Refdef(...) instead of calling re.RenderScene(...) directly. That unifies the direct and legacy scene
  submits under the same SOF2 wrapper. I also logged it in /E:/SOF2/attempt_log.md. The rebuilt exe is current at /E:/
  SOF2/OpenSOF2/build_test/Debug/openjk_sp.x86.exe.

The problem is, the that the issue persists and this has not corrected our player character flicker. I asked that the y providde a handoff file, and it is written at /E:/SOF2/OpenSOF2/npc_model_loading.md. It captures:

  - how the real world-position NPC probe system works
  - the current Ghoul2 skin fallback logic
  - what is still debug-only
  - the current blockers and next steps

On the latest Run using  `.\openjk_sp.x86.exe +devmap pra2` again I produced the fresh `C:/Users/Mark/Documents/My%20Games/OpenSOF2/base/qconsole.log` which may be useful. Alternatively, you have fixed this type of issue in the past for us, so perhaps your experience can be leveraged.
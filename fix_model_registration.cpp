/*
DIAGNOSTIC: Check if cgs.gameModels array is empty after CG_RegisterGraphics

The root cause of invisible ET_GENERAL models is likely that CG_RegisterGraphics
runs during CG_Init, but the server model configstrings (34+N) haven't been
transmitted to the client yet. This means cgs.gameModels[N] is NULL for all
model-registered entities.

Solution: Add a deferred model registration callback that fills missing cgs.gameModels[]
entries from configstrings on first render frame when all configstrings are available.

Files affected:
- code/client/cl_cgame.cpp: Add CG_RegisterMissingModels() called from CL_CGameRendering
- Verify that after first render, cgs.gameModels entries are populated

Test:
1. Add logging to CG_RegisterGraphics to log cgs.gameModels[1..20] address/value
2. Add logging to deferred registration to log when it fills in missing entries
3. Compare server logs: when are server model configstrings set?
4. Compare client logs: when does CG_Init run vs when are configstrings available?
*/

#include <stdio.h>

// Pseudo-code for deferred model registration in CL_CGameRendering:
void CL_CGameRendering_patch() {
    // static bool s_modelsDeferred = false;
    // if ( !s_modelsDeferred && cge ) {
    //     s_modelsDeferred = true;
    //
    //     // Call a slot in cgame import table that fills cgs.gameModels[]
    //     // from configstring slot 34+N.
    //     // This is AFTER CG_Init, so all configstrings are now available.
    //     //
    //     // Problem: native cgame doesn't expose this function.
    //     // Solution A: Add it as a new import slot (requires struct change)
    //     // Solution B: Manually fill from engine side (hacky but works)
    //     // Solution C: Have cgame re-register models on first render (invasive)
    //
    //     // Going with Solution B: engine-side fill
    //     for ( int i = 1; i < 256; i++ ) {
    //         const char *modelPath = sv.configstrings[34 + i];
    //         if ( modelPath && modelPath[0] ) {
    //             // Call R_RegisterModel and update cgs.gameModels[i]
    //             // Requires knowing address of cgs.gameModels (0x301accb4)
    //             // *(model_t **)( 0x301accb4 + i*4 ) = re.RegisterModel( modelPath );
    //         }
    //     }
    // }
}

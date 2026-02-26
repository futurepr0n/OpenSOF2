# OpenSOF2 Session Resume — Main Menu Crash Fix

## Current Task
Fix the main menu crash in Menusx86.dll during `Creating STM s_main...` so the SP main menu renders.

## What's Been Fixed This Session

### 1. AnimMap / ClampanimMap RE_DrawGetPicSize fix (DONE)
**File:** `E:\SOF2\OpenSOF2\code\rd-vanilla\tr_shader.cpp` (line ~3664)

The guage shader (`gfx/menus/backdrop/guage`) uses `clampanimMap` with 16 animation frames. In OpenJK's renderer, `textureBundle_t::image` for animated textures is NOT a single `image_t*` — it's an `image_t**` (array of image pointers allocated via `R_Hunk_Alloc`). See `tr_shader.cpp:1340`:
```cpp
stage->bundle[0].image = (image_t*) R_Hunk_Alloc( numAnimations * sizeof(image_t*), qfalse );
```

Our `RE_DrawGetPicSize` was treating it as `image_t*`, reading pointer values as image fields (garbage). Fixed by checking `numImageAnimations > 1`:
```cpp
if ( shader->stages[0].bundle[0].numImageAnimations > 1 ) {
    img = ((image_t **)shader->stages[0].bundle[0].image)[0]; // first frame
} else {
    img = shader->stages[0].bundle[0].image;
}
```
**Result:** guage now correctly returns 128x128.

### 2. SOF2 cvar_t ABI wrapper (IN PROGRESS — code half-written)
**File:** `E:\SOF2\OpenSOF2\code\client\cl_ui.cpp`

**Root cause of current crash:**
- Crash: `ACCESS_VIOLATION at 0x40010AE6, reading 0x000000B5`
- Function: `UI_Image_Update` in Menusx86.dll
- The DLL reads `cvar_ptr + 0x44` expecting a `char* string` field
- SOF2's `cvar_t` has `char name[64]` at offset 0x00, then `string` at offset 0x44
- OpenJK's `cvar_t` has `char *name` at 0x00, `string` at 0x04, `hashIndex` (int) at 0x44
- So the DLL reads `hashIndex` (value 0xB5) as a string pointer → crash in strlen

**The DLL also passes `cvar_t*` to `Cvar_Set(name, value)`** relying on `(char*)cvar == cvar->name[64]` (inline name). In OpenJK, `(char*)cvar` gives 4 bytes of a pointer to the name, not the name string itself.

**SOF2 cvar_t layout (from Ghidra decompilation):**
```
0x00-0x3F: char name[64]      ← inline, (char*)cvar == name string
0x40:      cvar_t *next
0x44:      char *string        ← DLL reads this directly
0x48:      char *resetString
0x4C:      char *latchedString
0x50:      int flags
0x54:      int modified
0x58:      int modificationCount
0x5C:      float value
0x60:      int integer
```

**OpenJK cvar_t layout (`q_shared.h:463`):**
```
0x00: char *name
0x04: char *string
0x08: char *resetString
0x0C: char *latchedString
0x10: int flags
0x14: qboolean modified
0x18: int modificationCount
0x1C: float value
0x20: int integer
0x24: qboolean validate
0x28: qboolean integral
0x2C: float min
0x30: float max
0x34: cvar_t *next
0x38: cvar_t *prev
0x3C: cvar_t *hashNext
0x40: cvar_t *hashPrev
0x44: int hashIndex          ← DLL reads THIS as char* string → 0xB5 → crash
```

**Fix approach:** Create `sof2_cvar_t` wrapper structs with the SOF2 layout. Wrap `Cvar_Get` (import slot 28) to return these wrappers instead of real `cvar_t*` pointers. Keep wrapper fields synced with real cvars.

**Code already added to cl_ui.cpp** (struct + wrapper pool + `UI_Cvar_Get_SOF2` function):
```cpp
typedef struct sof2_cvar_s {
    char    name[64];           // 0x00
    void   *next;               // 0x40
    char   *string;             // 0x44
    char   *resetString;        // 0x48
    char   *latchedString;      // 0x4C
    int     flags;              // 0x50
    int     modified;           // 0x54
    int     modificationCount;  // 0x58
    float   value;              // 0x5C
    int     integer;            // 0x60
} sof2_cvar_t;

#define MAX_SOF2_CVARS 256
static sof2_cvar_t  s_sof2Cvars[MAX_SOF2_CVARS];
static cvar_t      *s_sof2CvarReal[MAX_SOF2_CVARS];
static int          s_sof2CvarCount = 0;
```

**What still needs to be done:**
1. Wire `UI_Cvar_Get_SOF2` into the import table (replace `DLLTrace::CvarGet`):
   - Find line ~676: `import.Cvar_Get = DLLTrace::CvarGet;`
   - Change to: `import.Cvar_Get = UI_Cvar_Get_SOF2;`
   - Remove the old `DLLTrace` struct (lines ~631-635)

2. Build and test: `cmd //c "E:\SOF2\build.bat"`

3. If it works, the menu should get past the cvar strlen crash and continue widget creation.

## DLL Functions That Access cvar Fields Directly (Ghidra research)

These functions read `cvar_ptr + 0x44` (string field):
- `UI_Image_Update` @ 0x40010ad0 — reads `this+0xd4` as cvar, strlen on `+0x44`
- `UI_ParseBackdropEntry` @ 0x40001970 area — reads `this+8` as cvar, strlen on `+0x44`

These pass cvar_ptr to import functions (need inline name[64] at offset 0):
- `UI_Frame_UpdateCvarsAndWidgets` — calls `Cvar_Set(cvar_ptr, value, 0)`
- `UI_Frame_Destructor` — calls `Cvar_Set(cvar_ptr, value, 0)`
- `UI_Slider_UpdateCvars` — calls `Cvar_Set(cvar_ptr, value, 0)`
- `UI_MenuSystem_Constructor` — calls `Cvar_SetValue(cvar_ptr, float, 0)`
- `UI_Selection_ProcessLinkedEntry` — calls `Cvar_Set(cvar_ptr, value, 0)`
- `UI_Selection_AddEntry` — calls `Cvar_Set(cvar_ptr, value, 0)`

Note: `Cvar_Set` and `Cvar_SetValue` expect `const char *name` as first arg. The DLL passes `cvar_t*` which works in SOF2 because `(char*)cvar == cvar->name[64]`. The wrapper's inline name[64] makes this work.

## Import Table Key Info
- Import table base in DLL: `0x400375d8` (102 entries × 4 bytes)
- Slot 28 (+0x70): `Cvar_Get` → returns `cvar_t*` — **this is what needs the wrapper**
- Slot 29 (+0x74): `Cvar_Set` — DLL sometimes passes cvar_ptr as name arg (3 args: ptr, val, 0)
- Slot 31 (+0x7c): `Cvar_SetValue` — DLL passes cvar_ptr as name arg (3 args: ptr, float, 0)

## Files Modified This Session
| File | Status | Changes |
|------|--------|---------|
| `code/rd-vanilla/tr_shader.cpp` | DONE | RE_DrawGetPicSize animMap fix; R_FindShader guage debug logging (can remove later) |
| `code/client/cl_ui.cpp` | IN PROGRESS | sof2_cvar_t wrapper struct + pool + UI_Cvar_Get_SOF2 function ADDED; import table NOT yet wired up |

## Files Modified in Prior Sessions (still in effect)
| File | Changes |
|------|---------|
| `code/rd-common/tr_public.h` | DrawGetPicSize at END of refexport_t |
| `code/rd-vanilla/tr_init.cpp` | RE_DrawGetPicSize wired into GetRefAPI |
| `code/rd-vanilla/tr_local.h` | RE_DrawGetPicSize declaration |
| `code/client/cl_ui.cpp` | 16-bit DrawGetPicSize writes, SEH handler, debug tracing, many slot wrappers |
| `code/ui/ui_atoms.cpp` | SEH handler around uie->UI_SetActiveMenu |
| `code/qcommon/common.cpp` | SOF2 boot cinematic (atvi + raven) |
| `build.bat` | Builds both renderer DLL and engine exe |

## Build & Test
```
cmd //c "E:\SOF2\build.bat"
cd "E:\SOF2\OpenSOF2\build_test\Debug" && ./openjk_sp.x86.exe
```

## What Success Looks Like
- No `ACCESS_VIOLATION at 0x40010AE6`
- Menu creation proceeds past the 3 `Font_HeightPixels` calls
- More widget registrations appear in the log
- Eventually the main menu renders (or a new different crash occurs to investigate)

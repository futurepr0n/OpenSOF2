# Automated Visual Regression Test — Issues 1 & 3
**Date:** 2026-04-02
**Status:** Approved
**Issues addressed:** Issue 1 (Animation Flashing), Issue 3 (Blue Beams)

---

## Goal

Both issues have fixes already applied but have never been visually verified in an interactive run. This spec describes a headless-friendly automation script that launches the game, moves the player, captures screenshots, and collects logs — requiring zero manual interaction.

---

## Approach

Option C: Python + `pydirectinput` (DirectInput-level keystrokes for SDL input), `pyautogui` (screenshot capture), `pywin32` (window verification), `Pillow` (PNG save).

**Why this over AutoHotkey or plain pyautogui:**
- OpenJK uses SDL2 in relative-mouse-capture mode. `pydirectinput` sends scan-code-level `SendInput` events that SDL reads correctly; `pyautogui` keystrokes use the Windows message queue which SDL ignores in game mode.
- Mouse panning uses `ctypes.windll.user32.mouse_event(MOUSEEVENTF_MOVE, ...)` for raw relative events, also compatible with SDL capture mode.
- `pyautogui.screenshot()` works fine for screen capture regardless of input method.

---

## Script

**File:** `E:/SOF2/test_auto.py`

**Dependencies (one install command):**
```
pip install pydirectinput pyautogui pillow pywin32
```

---

## Phases

### Phase 1 — Launch (t=0s)
- `subprocess.Popen([GAME_EXE, "+set", "fs_game", "", "+devmap", "pra2"])`
- Record PID for cleanup.

### Phase 2 — Load wait (t=0–12s)
- Sleep 12s.
- Call `win32gui.FindWindow(None, None)` / `EnumWindows` scanning for a window whose title contains `"OpenJK"` or `"Jedi"`.
- If no window found after 15s: terminate and exit with error.

### Phase 3 — Test loop (t=12s–95s)
Runs for 83s total; W is held the entire time.

| t (s) | Action |
|-------|--------|
| 12 | `pydirectinput.keyDown('w')` — begin forward movement; take screenshot 000 |
| 20 | Pan left: `mouse_event(MOUSEEVENTF_MOVE, -400, 0)` over 0.5s in 20 steps |
| 20.5 | Take screenshot 001 |
| 21 | Pan right: `mouse_event(MOUSEEVENTF_MOVE, +400, 0)` over 0.5s — return to heading |
| 28 | Take screenshot 002 |
| 36 | Take screenshot 003 |
| 40 | Pan left 400px over 0.5s |
| 40.5 | Take screenshot 004 |
| 41 | Pan right 400px — return to heading |
| 44 | Take screenshot 005 |
| 52 | Take screenshot 006 |
| 60 | Pan left 400px; take screenshot 007; pan right |
| 68 | Take screenshot 008 |
| 76 | Take screenshot 009 |
| 80 | Pan left 400px; take screenshot 010; pan right |
| 88 | Take screenshot 011 |
| 93 | Take screenshot 012 (final) |

Screenshots are saved as `screenshot_NNN_XXs.png` where NNN is index and XX is elapsed seconds.

### Phase 4 — Teardown (t=95s)
- `pydirectinput.keyUp('w')`
- `proc.terminate()` / `proc.wait(timeout=3)`
- Copy `C:\Users\Mark\Documents\My Games\OpenSOF2\base\qconsole.log` into the output folder.

---

## Output

All artifacts written to: `E:/SOF2/test_screenshots/<YYYYMMDD_HHMMSS>/`

```
test_screenshots/
└── 20260402_143000/
    ├── screenshot_000_12s.png
    ├── screenshot_001_20s.png
    │   ...
    ├── screenshot_012_93s.png
    └── qconsole.log
```

---

## Analysis

After the script exits, Claude reads each PNG via the Read tool and the copied `qconsole.log`.

### Issue 3 — Blue Beams
- **Look for:** thick blue line(s) / streaks anywhere in the 3D scene viewport.
- **Verdict criteria:**
  - All 13 screenshots clear → Issue 3 **closed**.
  - Blue line visible in any screenshot → note timestamp + screen region → investigate native cgame FX path (Ghidra on cgamex86.dll).

### Issue 1 — Animation Flashing
- **Screenshot limit:** 8s interval is too slow to catch per-frame alternation directly. Visual check: if a door/mover appears at two different states in sequential screenshots → regression present.
- **Primary signal:** `qconsole.log` scan for `[STATIC_GLM]`, `[SNAP]` lines. Healthy log shows static GLM submitting every frame without gaps; any `framesAbsent` anomaly or double-submit would appear in log.
- **Verdict criteria:**
  - No state mismatch in screenshots AND no log anomaly → Issue 1 **closed**.
  - State mismatch or log anomaly → add targeted per-frame logging to `CG_AddStaticGlmProps`.

---

## Success Criteria

The test run is considered **passing** when:
1. Script exits 0 (game did not crash before 95s).
2. 13 screenshots saved.
3. No blue lines visible in any screenshot.
4. No door/mover state mismatch between screenshots.
5. `qconsole.log` shows stable `[STATIC_GLM]` submission pattern.

---

## Files Changed / Created

| File | Change |
|------|--------|
| `E:/SOF2/test_auto.py` | New — the automation script |
| `E:/SOF2/test_screenshots/<run>/` | New — per-run output folder |

No engine source files are modified by this task.

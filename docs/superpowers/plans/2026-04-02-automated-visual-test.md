# Automated Visual Test Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Write `E:/SOF2/test_auto.py` — a zero-interaction script that launches OpenSOF2, drives the player forward for 95s with periodic camera pans, captures 13 screenshots, and collects the qconsole.log for post-run analysis of Issues 1 (Animation Flashing) and 3 (Blue Beams).

**Architecture:** Single Python script; subprocess launches the game, pydirectinput sends SDL-compatible scan-code keystrokes, ctypes mouse_event sends raw relative mouse deltas for camera pans, pyautogui captures screenshots. All output lands in a timestamped folder under `E:/SOF2/test_screenshots/`.

**Tech Stack:** Python 3, pydirectinput, pyautogui, Pillow, pywin32, ctypes (stdlib)

---

### Task 1: Install dependencies

**Files:**
- No files created — pip install only.

- [ ] **Step 1: Install packages**

```
pip install pydirectinput pyautogui pillow pywin32
```

- [ ] **Step 2: Verify imports**

```
python -c "import pydirectinput, pyautogui, win32gui; from PIL import Image; print('OK')"
```

Expected output: `OK`

If you see `ModuleNotFoundError` for `win32gui`, run `pip install pywin32` again then run `python Scripts/pywin32_postinstall.py -install` from the Python install directory.

---

### Task 2: Write test_auto.py

**Files:**
- Create: `E:/SOF2/test_auto.py`

- [ ] **Step 1: Write the script**

Write `E:/SOF2/test_auto.py` with this exact content:

```python
#!/usr/bin/env python3
"""
test_auto.py — Automated visual regression test for OpenSOF2
Verifies Issue 1 (Animation Flashing) and Issue 3 (Blue Beams).

Usage: python test_auto.py
Output: E:/SOF2/test_screenshots/<YYYYMMDD_HHMMSS>/
"""

import subprocess
import time
import os
import shutil
import ctypes
import sys
from datetime import datetime

try:
    import pydirectinput
    import pyautogui
    import win32gui
except ImportError as e:
    print(f"Missing dependency: {e}")
    print("Run: pip install pydirectinput pyautogui pillow pywin32")
    sys.exit(1)

# ── Config ────────────────────────────────────────────────────────────────────
GAME_EXE        = r"E:\SOF2\OpenSOF2\build_test\Debug\openjk_sp.x86.exe"
GAME_ARGS       = ["+set", "fs_game", "", "+devmap", "pra2"]
QCONSOLE_LOG    = r"C:\Users\Mark\Documents\My Games\OpenSOF2\base\qconsole.log"
SCREENSHOT_ROOT = r"E:\SOF2\test_screenshots"

LOAD_WAIT    = 12   # seconds between launch and first input
TEST_SECONDS = 95   # total script lifetime (launch → terminate)

WINDOW_TITLES = ["OpenJK", "Jedi", "Star Wars"]

# Mouse pan: 400px split into PAN_STEPS steps over PAN_SECS seconds
PAN_STEPS = 20
PAN_SECS  = 0.5
MOUSEEVENTF_MOVE = 0x0001

# Disable pyautogui/pydirectinput safety pauses
pyautogui.FAILSAFE = False
pydirectinput.PAUSE = 0.0

# ── Test sequence ─────────────────────────────────────────────────────────────
# Each entry: (seconds_since_test_start, action)
# test_start = LOAD_WAIT seconds after launch
# Actions: 'screenshot' | 'pan_left' | 'pan_right'
SEQUENCE = [
    ( 0.0, 'screenshot'),   # 000  t=12s from launch
    ( 8.0, 'pan_left'),
    ( 8.5, 'screenshot'),   # 001  t=20.5s
    ( 9.0, 'pan_right'),
    (16.0, 'screenshot'),   # 002  t=28s
    (24.0, 'screenshot'),   # 003  t=36s
    (28.0, 'pan_left'),
    (28.5, 'screenshot'),   # 004  t=40.5s
    (29.0, 'pan_right'),
    (32.0, 'screenshot'),   # 005  t=44s
    (40.0, 'screenshot'),   # 006  t=52s
    (48.0, 'pan_left'),
    (48.5, 'screenshot'),   # 007  t=60.5s
    (49.0, 'pan_right'),
    (56.0, 'screenshot'),   # 008  t=68s
    (64.0, 'screenshot'),   # 009  t=76s
    (68.0, 'pan_left'),
    (68.5, 'screenshot'),   # 010  t=80.5s
    (69.0, 'pan_right'),
    (76.0, 'screenshot'),   # 011  t=88s
    (81.0, 'screenshot'),   # 012  t=93s  (final)
]

EXPECTED_SCREENSHOTS = sum(1 for _, a in SEQUENCE if a == 'screenshot')  # 13

# ── Helpers ───────────────────────────────────────────────────────────────────

def find_game_window():
    """Return hwnd of first visible window whose title matches WINDOW_TITLES."""
    found = []
    def _cb(hwnd, _):
        if win32gui.IsWindowVisible(hwnd):
            title = win32gui.GetWindowText(hwnd)
            if any(t.lower() in title.lower() for t in WINDOW_TITLES):
                found.append(hwnd)
    win32gui.EnumWindows(_cb, None)
    return found[0] if found else None


def pan_mouse(dx_total):
    """Send dx_total relative pixels of mouse movement in PAN_STEPS steps."""
    step_dx = dx_total // PAN_STEPS
    step_delay = PAN_SECS / PAN_STEPS
    for _ in range(PAN_STEPS):
        # Pass as unsigned DWORD; negative values are two's-complement for relative move
        ctypes.windll.user32.mouse_event(MOUSEEVENTF_MOVE, step_dx & 0xFFFFFFFF, 0, 0, 0)
        time.sleep(step_delay)


def take_screenshot(out_dir, index, abs_elapsed):
    """Capture full screen, save as screenshot_NNN_XXs.png. Returns file path."""
    fname = f"screenshot_{index:03d}_{int(abs_elapsed):02d}s.png"
    path  = os.path.join(out_dir, fname)
    img   = pyautogui.screenshot()
    img.save(path)
    print(f"  [screenshot] {fname}")
    return path


# ── Core test loop ────────────────────────────────────────────────────────────

def run_test(out_dir):
    """Hold W, execute SEQUENCE, return screenshot count."""
    shot_idx   = 0
    seq_idx    = 0
    test_start = time.time()
    loop_end   = TEST_SECONDS - LOAD_WAIT   # 83s

    print(f"[test] Holding W. Loop runs {loop_end:.0f}s.")
    pydirectinput.keyDown('w')

    try:
        while True:
            elapsed = time.time() - test_start
            if elapsed >= loop_end:
                break

            # Dispatch all sequence entries whose time has arrived
            while seq_idx < len(SEQUENCE) and elapsed >= SEQUENCE[seq_idx][0]:
                t_due, action = SEQUENCE[seq_idx]
                seq_idx += 1
                abs_t = LOAD_WAIT + t_due

                if action == 'screenshot':
                    take_screenshot(out_dir, shot_idx, abs_t)
                    shot_idx += 1
                elif action == 'pan_left':
                    print(f"  [pan] left  t={abs_t:.1f}s")
                    pan_mouse(-400)
                elif action == 'pan_right':
                    print(f"  [pan] right t={abs_t:.1f}s")
                    pan_mouse(400)

            time.sleep(0.05)  # 50 ms poll — fine-grained enough for 0.5s pan windows
    finally:
        pydirectinput.keyUp('w')
        print(f"[test] W released. {shot_idx}/{EXPECTED_SCREENSHOTS} screenshots captured.")

    return shot_idx


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    # Create timestamped output directory
    run_ts  = datetime.now().strftime("%Y%m%d_%H%M%S")
    out_dir = os.path.join(SCREENSHOT_ROOT, run_ts)
    os.makedirs(out_dir, exist_ok=True)
    print(f"[main] Output dir: {out_dir}")

    # Launch game
    print(f"[main] Launch: {GAME_EXE}")
    proc = subprocess.Popen([GAME_EXE] + GAME_ARGS)
    print(f"[main] PID {proc.pid}. Waiting {LOAD_WAIT}s for map load...")
    time.sleep(LOAD_WAIT)

    # Verify / focus game window
    hwnd = find_game_window()
    if hwnd:
        title = win32gui.GetWindowText(hwnd)
        print(f"[main] Window: '{title}' hwnd={hwnd}")
        win32gui.SetForegroundWindow(hwnd)
        time.sleep(0.3)
    else:
        print("[main] WARNING: game window not found — sending input to foreground window")

    # Run test sequence
    n_shots = run_test(out_dir)

    # Teardown
    print("[main] Terminating game...")
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()

    # Collect artifacts
    if os.path.exists(QCONSOLE_LOG):
        dst = os.path.join(out_dir, "qconsole.log")
        shutil.copy2(QCONSOLE_LOG, dst)
        print(f"[main] Copied qconsole.log → {dst}")
    else:
        print(f"[main] WARNING: qconsole.log not found at {QCONSOLE_LOG}")

    # Exit code
    print(f"\n[done] {n_shots} screenshots in {out_dir}")
    sys.exit(0 if n_shots == EXPECTED_SCREENSHOTS else 1)


if __name__ == "__main__":
    main()
```

---

### Task 3: Verify syntax before running

**Files:**
- Read: `E:/SOF2/test_auto.py` (no changes — verification only)

- [ ] **Step 1: Compile-check the script**

```
python -m py_compile E:/SOF2/test_auto.py && echo "Syntax OK"
```

Expected: `Syntax OK` with no errors.

- [ ] **Step 2: Verify imports resolve**

```
python -c "
import sys; sys.path.insert(0,'E:/SOF2')
# Import without executing main()
import importlib.util, types
spec = importlib.util.spec_from_file_location('test_auto', 'E:/SOF2/test_auto.py')
mod  = importlib.util.module_from_spec(spec)
mod.__name__ = 'test_auto'
# Patch __name__ so main() does not run
spec.loader.exec_module(mod)
print('Imports OK')
"
```

Expected: `Imports OK`

If this fails with an import error, re-run Task 1 Step 1.

---

### Task 4: Full test run

**Files:**
- Read (after run): `E:/SOF2/test_screenshots/<run>/screenshot_*.png`
- Read (after run): `E:/SOF2/test_screenshots/<run>/qconsole.log`

- [ ] **Step 1: Run the script**

```
python E:/SOF2/test_auto.py
```

Expected console output (abridged):
```
[main] Output dir: E:\SOF2\test_screenshots\20260402_XXXXXX
[main] Launch: E:\SOF2\OpenSOF2\build_test\Debug\openjk_sp.x86.exe
[main] PID XXXXX. Waiting 12s for map load...
[main] Window: '...' hwnd=XXXXX
[test] Holding W. Loop runs 83s.
  [screenshot] screenshot_000_12s.png
  [pan] left  t=20.0s
  [screenshot] screenshot_001_20s.png
  [pan] right t=21.0s
  ...
  [screenshot] screenshot_012_93s.png
[test] W released. 13/13 screenshots captured.
[main] Terminating game...
[main] Copied qconsole.log → E:\SOF2\test_screenshots\20260402_XXXXXX\qconsole.log
[done] 13 screenshots in E:\SOF2\test_screenshots\20260402_XXXXXX
```

Script exits with code 0.

**If the script exits with code 1** (fewer than 13 screenshots): check console for which screenshot was missed and whether the game crashed early. Re-run; if consistently fewer screenshots, increase `LOAD_WAIT` from 12 to 18.

**If the game window is not found** (WARNING line appears): input still sends to the foreground window. As long as the game is the active window after launch, this is fine.

- [ ] **Step 2: Confirm output folder exists with 13 PNGs + log**

```
python -c "
import os, glob
folders = sorted(glob.glob('E:/SOF2/test_screenshots/*/'))
latest  = folders[-1]
pngs    = glob.glob(os.path.join(latest, '*.png'))
log     = os.path.join(latest, 'qconsole.log')
print(f'Folder:      {latest}')
print(f'Screenshots: {len(pngs)} (expected 13)')
print(f'qconsole:    {os.path.exists(log)}')
"
```

Expected:
```
Folder:      E:\SOF2\test_screenshots\20260402_XXXXXX\
Screenshots: 13 (expected 13)
qconsole:    True
```

---

### Task 5: Analyse output

**Files:**
- Read: each `screenshot_*.png` in the run folder (Claude reads image files natively)
- Read: `qconsole.log` from the run folder

- [ ] **Step 1: Read all screenshots**

Use the Read tool on each PNG in sequence order (000 → 012). Look for:
- **Issue 3 (Blue Beams):** any thick blue line(s) or streaks in the 3D scene. Note screenshot index + approximate screen region if present.
- **Issue 1 (Flashing):** doors or geometry appearing in visibly different open/closed states between consecutive screenshots. Note indices if present.

- [ ] **Step 2: Scan qconsole.log**

Search for:
- `[STATIC_GLM]` lines — should show `submit ent=N` every frame, no gaps or skipped frames
- Any `framesAbsent` lines (would indicate the invalidation path firing unexpectedly)
- Any `[SNAP]` lines with `eType=5` (would indicate the blue-beam filter was bypassed)
- Crash indicators: `ERR_DROP`, `ERR_FATAL`, `Hunk_Alloc failed`

- [ ] **Step 3: Write verdict**

For each issue, produce one of:
- **CLOSED** — no evidence of the problem in screenshots or log
- **STILL PRESENT** — evidence found; note which screenshot(s) and/or log lines
- **INCONCLUSIVE** — game didn't reach the area containing the affected entities

---

## Self-Review Checklist

**Spec coverage:**
- Phase 1 (Launch) → Task 4 Step 1 ✓
- Phase 2 (Load wait / window check) → Task 2 script + Task 4 Step 1 ✓
- Phase 3 (Test loop / W key / pans / screenshots) → Task 2 SEQUENCE table + run_test() ✓
- Phase 4 (Teardown / artifact collection) → Task 2 main() teardown block ✓
- Output folder structure → Task 4 Step 2 ✓
- Issue 3 analysis → Task 5 Step 1 ✓
- Issue 1 analysis → Task 5 Steps 1 & 2 ✓
- Success criteria (13 shots, no crash, no beams, no flashing, stable GLM log) → Task 5 Step 3 ✓

**No placeholders:** all steps have exact commands and complete code. ✓

**Type consistency:** `pan_mouse(dx_total: int)`, `take_screenshot(out_dir, index, abs_elapsed)`, `run_test(out_dir) -> int` — consistent across Tasks 2, 4, 5. ✓

# Compositor, desktop installation and API verification

Unnumbered x64 development build. No GPU driver, SMP startup or hardware-vsync
implementation was added. The frozen 32-bit artifact remains unchanged.

## Findings and changes

Source inspection found reproducible software faults, not evidence that a second
vCPU accelerates SCos through SMP. Only the bootstrap processor runs this kernel.
SysMon now explicitly labels its measurement **BSP load**, not whole-machine load.

- The GOP framebuffer was mapped with PWT+PCD selecting strong UC. Its validated,
  non-RAM-overlapping pixel aperture now selects PAT slot 1 (WC); RAM stays WB
  and device registers stay UC slot 3. Existing cache-disable/WBINVD/CR3-switch
  sequencing is retained, and no MTRRs are rewritten. MMIO mapping rejects
  conflicting cache aliases. Native-color scanout uses contiguous `rep movsl`
  stores and `sfence`; other GOP channel layouts retain conversion.
- Interaction handling used to restore wallpaper into the live back buffer on
  every mouse report, before a frame was ready. Damage repair could repaint
  whole overlapping windows and spread damage. Now events collect bounds only;
  each frame reconstructs each damaged region from wallpaper upward under a
  screen-only scissor. App surfaces remain cached and repaint only when dirty.
- Resize allocation was deferred, but old geometry was not repaired when that
  allocation actually changed dimensions. Resize now damages the union of the
  old and new extents, including shrinking, before release.
- Desktop right-down opened a menu immediately. Right drag now makes a selection
  band; a stationary right release opens the context menu. Left drag remains.
- Content clients previously did not consistently receive mouse-up and lost
  motion outside their window. WM now captures content drags and delivers move/
  release to their owner, clearing capture on close/minimize. Modals block
  background input. Taskbar hit testing takes priority over underlying windows.
- Minimized clients no longer retain keyboard focus; restore preserves maximized
  state. Maximized windows cannot accidentally drag their full-screen geometry.
- `s_clip_text` drew full labels for widths <=1 character and wrote before its
  temporary array for a two-character width. It now handles every nonnegative
  width safely, clips single-line text, and uses bounded ellipsis. Desktop
  centering uses clipped width. Menus/launcher titles are bounded. Native dialogs
  wrap above controls, grow to fit ordinary messages and scroll input carets.
- The bottom bar is Launcher + **Windows N** + power/clock. The picker lists all
  open/minimized windows with stable IDs, without per-app taskbar buttons.
- Installer no longer auto-launches. It copies/validates/registers, adds only that
  app's shortcut (without restoring unrelated hidden icons), and reports success.
  On supported ATA storage it saves the filesystem after a confirmation naming
  the target and warning it can differ from the boot USB. Save failure and
  unsupported persistence are explicit. No USB-storage driver was added.
- Newly supplied demos are named Counter and Sketch, not Sample counter/sketch.
  Existing edited packages retain their own metadata; text is clipped regardless.
- API 2 now has 55 functions: added icon_size, icons, set_icon, text_clip,
  text_wrap, focused, minimize. Icons are always 24x24, with no scaling argument.
  Fixed `blend` percent conversion (100 now means 100%, not 100/255).

PAT reference consulted: Linux x86 PAT documentation,
https://www.kernel.org/doc/html/latest/arch/x86/pat.html . This informs memory-type
selection and alias avoidance, not a claim that Linux code/drivers were imported.

## Verification

Two clean `make clean && make -j4` builds were byte-identical and warning-free.
Image: 67,108,864 bytes, SHA256:

```
0da4ee2792ed8e658788cc0d11414fcd0adb6edd184cee6db539c84eb1b2f1a8
```

Retained suites, all passed on the final binary:

```
python3 tools/tests/test_cat.py
python3 tools/tests/test_studio_edit.py
python3 tools/tests/test_compositor.py
```

- CAT and clipboard suites: USB/xHCI and compatible IDE/PS2 at 128 MiB. Existing
  syntax/package corruption/budget/permissions/theme/recovery checks retained.
  New tests check the seven APIs' exercised paths, blend endpoints, installation
  creating one shortcut, no auto-launched app, and installer persistence across
  IDE reboot **without an extra manual save**. Demo execution is tested separately.
- Compositor suite: QEMU TCG with 1 and 2 vCPUs. Tests inspect actual framebuffer
  page-table leaves for PAT slot 1. Narrow text tests verify pixels outside widths
  0,1,7,8,15,16,23,24,40 remain untouched.
- Actual scanout screenshots from partial redraw are compared byte-for-byte with
  a fresh full redraw **while the mouse is still held**, for overlapping-window
  movement, grow/shrink resize, icon movement and right-button selection.
  The clock/taskbar strip is excluded from pixel equality to avoid RTC time
  differences; picker button/row hit testing is tested through WM mouse events.
- Tests verify right-click menu deferral, picker restoration of a minimized
  window, and content capture/release outside Studio. Scene screenshots and logs
  are under ignored build/test-results. Final native-app screenshot inspected.
- Rendering phases are marked noinline to keep profiling/debug symbols stable;
  tests call actual production paths. No fake compositor or guest test mode.
- Frozen image validation passed; checksum remains
  43c2bce097389179aff9965779148c82a4907017c392f996a6602f727efb0c97.

An early compositor test comparison included an unpainted pending full redraw
between independent scenarios. That test setup was corrected to render the
baseline before starting the next interaction; final comparisons above passed.

## What this does NOT establish

No hardware CPU percentage or frame-time improvement is claimed from QEMU.
Real GPUs, native resolution, firmware cache policy and emulated scanout have
very different costs. The existing 50 Hz paint cap remains. PAT WC removes an
identified uncached-write path but still requires physical-PC validation.

GOP is a linear scanout buffer, not a vblank/page-flip API. Faster and smaller
updates and correct damage eliminate the tested software trails; they do not
prove tear-free hardware scanout. True refresh-synchronized flipping requires
GPU-specific display support outside this change. No SMP work is claimed.

USB-only installs are still session RAM and vanish on reboot. Existing package
metadata and edited demo files are not silently replaced. The UI audit addresses
shared text, desktop/menu/launcher/dialog paths, not every possible native app
layout at every resolution. Emulator tests do not certify physical input/GPU
behavior, arbitrary app code, power-loss safety or persistence on USB/AHCI/NVMe.

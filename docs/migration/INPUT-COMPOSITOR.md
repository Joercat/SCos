# Unnumbered x64 input/compositor corrections

Date: 2026-09-20. Existing custom-kernel code only: no new drivers, downloaded
implementation, networking, or release number. Physical-PC confirmation remains
necessary; these tests do not certify that every OS workflow is bug-free.

## Root causes and corrections

- **Low-sensitivity dead zones:** `delta * sensitivity / 3` truncated each
  individual report. Three single-count reports at sensitivity 1 moved zero
  pixels before the fix, versus one pixel afterward. Signed fractional
  remainders now preserve small movements in both axes. Changing sensitivity
  clears the remainder; outward fractional movement is discarded at screen
  boundaries. The range remains 1–6. The new default is **2**, versus 3
  previously (two-thirds the old default speed); existing saved choices survive.
  Settings factory reset and the shared reset routine use the same defaults.
- **Grab-point drag trails:** interaction repaint restored wallpaper before
  removing the cursor's saved background. A later cursor restore pasted stale
  window pixels over that wallpaper. Remove the cursor and damage its old
  rectangle before modifying the underlying interaction region.
- **Overlapping-window damage:** occlusion repair blits entire window frames,
  not clipped rectangles. Propagate those whole-frame bounds bottom-up so
  higher windows and the final framebuffer flip cover all modified pixels.
- **Hover invalidation:** desktop/taskbar target lookup was under an impossible
  zone condition, and menu/launcher row changes were excluded from the repaint
  trigger. Use actual hit rectangles, check windows before underlying icons,
  and suppress desktop hover during window/icon dragging, selection, and menus.
- **Launcher click timing:** resolve the clicked row from current coordinates,
  rather than the last painted hover value. Clicking a context-menu top margin
  no longer rounds into its first action.
- **Resize jump:** preserve the offset inside the resize handle, rather than
  snapping the bottom/right edges to the pointer on the first movement.
- **Settings loading:** repair theme value parsing; handle JSON whitespace,
  reject malformed/overflowing numeric values, and bound loaded preferences
  consistently with the controls (sensitivity 1–6, double-click 200–900 ms).
- **Mouse queue concurrency:** a PS/2 IRQ could coalesce into a slot while the
  foreground consumer copied it. Copy/consume and USB queue injection now use
  short interrupt-preserving critical sections. No polling delays were added.

## Verification performed

Temporary QEMU/QMP/GDB harnesses were removed after verification. No test hooks
or debug screens ship in the image.

1. **Baseline reproduction:** three +1 reports at sensitivity 1 produced zero
   pixels of travel. Held drag comparisons against a full redraw showed 798
   differing RGB bytes in two sampled frames (the cursor-sized stale patch).
2. **Focused source-level regression guest:** only the WM object was compiled
   with GCC `-fkeep-static-functions` to make internal compositor/input routines
   callable; no source instrumentation. Tests passed for all six sensitivities,
   30 individual one-count reports in each direction on both axes, edge residue,
   sensitivity transitions, four theme round trips, whitespace, numeric bounds,
   overflow, and malformed numeric suffixes.
3. **Partial versus full framebuffer comparisons:** zero differences in the
   desktop/window area for an overlapping-window chain, 15 held drag frames
   with several reports between paints, four resize frames, five desktop/power
   hover positions, four context-menu positions, and four launcher positions.
   The bottom 40-pixel taskbar was excluded from byte comparisons because the
   RTC clock continues running; these comparisons do not prove taskbar pixel
   identity. The exact final resized dimensions and an immediate launcher-row
   click before repaint were also asserted.
4. **Queue/native input:** coalesced two-axis deltas, press/release ordering and
   empty-queue behavior passed. Thirty real QMP one-count movements through
   native USB input at sensitivity 1 produced ten pixels on each axis.
5. **Clean production build:** rebuilt without the temporary compiler option,
   with normal warnings-as-errors. Fresh USB-storage/xHCI HID boot at 128 MiB
   and IDE/PS2 boot at 256 MiB both passed: all ten app open/render/close
   lifecycles, native low-sensitivity motion, native keyboard `calc 2 + 2`
   producing `Result: 4`, and repeated held mouse dragging/release through the
   normal main loop. This is lifecycle/integration coverage, not exhaustive
   testing of every command or application feature.
6. Updated `dist/scos.img` and checksum, retained the frozen 32-bit image,
   and verified the published image bytes against the local build.

## PC retest

Use the [existing UEFI USB instructions](PC-TEST.md). Check slow single-pixel
movement at sensitivity 1 and 2, rapid window dragging while still holding the
button, overlapping windows, resize handles, and menu/launcher hover/clicks.
If a previously persisted setting is above 2, choose 2 manually; the update
intentionally does not overwrite an explicit saved preference.

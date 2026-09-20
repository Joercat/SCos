# Original apps and shared desktop — native x64 port

Status: unnumbered development conversion, 2026-09-20. This is not physical-PC
acceptance or a claim that the entire OS/hardware conversion is finished.

## Provenance and scope

The earlier deletion of feature source was broader than the user intended.
This step recovers and adapts the original implementations from
`6717943f977a7e0f95f0ace5fa48cfe6a564f873:legacy/i386/kernel/src/`, rather than
replacing those apps with reduced imitations. `kernel/desktop/` contains the
original app registry, ten app windows, compositor, desktop/taskbar/launcher,
TTYs, confirmations, RAM VFS, themes, cards, shared library, art, clock and PS/2
input logic. The browser window remains its original unavailable-feature notice;
there is no browsing or network implementation hidden behind it.

There are 29 active desktop C files (roughly 8,900 lines), including two new
platform/heap adapters. The framebuffer's drawing primitives are reused while
its firmware/cache-specific front end is replaced. The old contiguous-low-RAM
allocator, BIOS-sector copies, BIOS boot code and 32-bit build are not restored.
The source reference remains in Git history. `os.html` is unchanged.

## Native integration

* Original C app interfaces now use native pointers, `size_t` allocation/string
  sizes, `ptrdiff_t` deltas and 64-bit managed-memory values. Protocol bytes and
  bounded file/card/event fields remain fixed-width where appropriate.
* The existing validated UEFI extent allocator now handles contiguous multipage
  requests, zeroing, coalescing and overlap/double-release rejection. Desktop
  allocation headers validate requested free sizes and retain actual rounded
  page counts. There is no second allocator pretending low RAM is contiguous.
* Allocation ownership follows app callbacks, nested window creation, surfaces,
  document growth and terminal tabs. VFS and compositor buffers have explicit
  owners. SysMon/procs use actual reserved pages, including allocation headers,
  not guessed window dimensions or manually claimed payload counts. Static
  kernel/desktop state and the page-table arena stay in the kernel row. Memory
  totals mean **managed RAM**, excluding unreclaimed firmware reservations.
* GOP address, stride and color masks drive scanout; rendering uses the original
  software compositor and primitives. Scanout remains uncached. No speculative
  MTRR edits or native GPU driver were introduced.
* CR0/CR4, x87 and MXCSR are initialized before SSE-capable app code runs. The
  entire registered IRQ call graph remains general-register-only; objdump was
  checked for SIMD instructions. A future preemptive scheduler must save FP/SIMD
  state. This is still cooperative, single-bootstrap-CPU, ring-0 software, not
  isolated processes or SMP scheduling.
* Native IRQ1/IRQ12 dispatch feeds the original event queues. Keyboard translation
  is explicitly set for the existing set-1 decoder. PS/2 wheel Z is normalized
  to the app/HID positive-up convention; the historically accepted USB injection
  convention was not changed. The USB controller driver itself is not ported.
* CPU load accounts TSC time between atomic STI/HLT entry and hardware IRQ entry.
  Callback timing excludes accounted halted time. The displayed TSC reference
  rate comes from the startup UEFI-Stall calibration; it is not instantaneous
  core clock speed. Callback percentages are not an isolated scheduler's CPU
  accounting and do not separate every interrupt/compositor cost.
* Panic uses native direct scanout/serial, independent of the desktop heap or WM.
  Fatal output clears the old scene once, preserves the register dump and shows
  the original dedicated panic art. Normal nonfatal error handling is retained;
  the restored diagnostic self-test command was removed.

## Repairs found during conversion and interaction

* Stable window slots replace struct compaction: closing an earlier window no
  longer invalidates terminal, document, game-owner or dialog back-pointers.
  Dialog contexts validate window generations before using reused slots.
* Cancelled/replaced context menus release their contexts; Files no longer
  redraws through a freed context. Failed app-state allocation releases the
  already-created surface. Notepad growth/open/dialog allocation failures are
  checked before copying or dereferencing.
* Notepad and the terminal editor check RAM-file write results instead of
  reporting failed saves as successful. Notepad's forward-delete bound and
  path/title handling are protected by the existing bounded VFS plus added
  path checks. Files preserves the terminal-only `/system` boundary and directs
  deletion to `rm -s` rather than deleting through its context menu.
* Actual PS/2 interaction exposed control-code mismatches: Ctrl+T/Ctrl+W and
  Ctrl+S now recognize the emitted control characters as well as letter forms.
  The terminal editor's existing Ctrl+O/Ctrl+X behavior is retained.
* Switching to a TTY now stops the WM keyboard-drain loop immediately; subsequent
  queued characters are left for the console instead of consumed by the old
  foreground. TTY launches likewise leave subsequent input for the new window.
  Both shells support `appstrt` with a file argument; GUI shell paths use cwd.
* SysMon hit-testing uses the painted table geometry; headers no longer overlap
  rows. Wheel/keyboard scrolling reaches tasks below the viewport. Selected app
  IDs survive changes in the window list, avoiding accidental retargeting.
* Bounded serial/RTC waits replace unbounded loops; RTC 12-hour noon/midnight
  conversion is corrected and invalid snapshots fail explicitly. Integer-minimum
  formatting, floating-point rounding carry and out-of-range formatting are
  handled without signed-overflow or narrowing-based output.

## Verification

Host fixtures were temporary and are retired after final checks. They compile
and exercise the actual app/shared source and allocator algorithms, substituting
host memory/clock/I/O only where executing privileged instructions is impossible.
These tests are **not** a hardware claim and were supplemented by real guest
keyboard/mouse workflows, not treated as command-execution proof by themselves.

* GCC `-Wall -Wextra -Werror` native build, frozen-milestone guard, and clean
  byte-for-byte reproducibility check.
* ASan+UBSan app/shared fixtures: every app opened/painted/closed; shell handler
  arithmetic and filesystem workflows; terminal tabs; Notepad save; stable
  references after earlier-window removal; 50 terminal/tab open-close cycles;
  50 context-menu cancellation cycles; `/system` restriction; SysMon selection
  and actual victim closure; stale Save-As context after slot reuse; document
  growth OOM and app-open rollback; rename-cycle rejection; RAM factory reset;
  numeric formatting edge cases.
* Native multipage allocator fixtures: allocation, free/coalescing, zeroing,
  accounting and double-free rejection. UBSan runs place the backing allocation
  at physical-like addresses above 4 GiB; ASan uses low host addresses compatible
  with its shadow map. This distinguishes address-width tests from guest MMU
  testing rather than claiming the host fixture proves hardware mapping.
* Final QEMU 11.0.2 / x64 EDK2 / q35 / TCG, 256 MiB: genuine PS/2 mouse
  double-click launches Terminal; injected keyboard input executes calculator,
  mkdir/touch/ls, opens/closes tabs, edits/saves a file with Ctrl+O/Ctrl+X; Notepad
  opens that file, edits and saves with Ctrl+S. Guest memory and TTY readback
  verify the changed file bytes, not just a success message.
* All ten app windows coexist. SysMon selection/End Task closes the intended
  Notepad window; wheel scrolling reaches lower rows. The six TTYs survive a
  confirmed `kill --system 2`; GUI launches fail with the requested WM-binding
  error while stopped, and `wm` starts a new desktop session.
* Final 128 MiB guest: desktop boot, keyboard/TTY and Solitaire new-game input.
  Shutdown reaches the explicit manual power-off screen. An injected NMI shows
  native register/fault output plus panic art. A separate terminal-triggered
  panic displays the supplied reason and dedicated art without WM dependence.

This is broad integration/regression coverage, not exhaustive validation of every
command flag, game state, resolution or real device. The larger startup-only
fault/loader matrix in BOOT64 is a historical checkpoint, not silently relabeled
as having been rerun in full for every app edit.

## Remaining boundaries

* PS/2 is connected; native USB HID is not. Firmware USB boot does not keep UEFI
  keyboard services available to this kernel. The user's USB-input PC is not
  accepted or ready for a requested test cycle on the strength of these results.
* Documents, settings and desktop changes live only in RAM. `save` reports
  persistence unavailable; factory reset rebuilds the RAM tree, not a physical
  disk or USB wipe. Do not claim durable saves or secure device erasure.
* Restart uses the legacy reset-controller command; unsupported hardware gets
  an explicit failure. ACPI shutdown is not converted: safe-to-turn-off is the
  deliberate fallback, not a claim that power was electrically removed.
* No new GPU/NIC/Wi-Fi/browser dependency was integrated. No networking, HTML
  engine, hardware-accelerated compositor, process isolation or SMP is claimed.
* Original fixed UI/file/terminal capacities remain. This port does not promise
  unlimited files, arbitrary resolutions or removal of every inherited limitation.
* No new release number; the frozen `dist/scos-32bit.img` remains unchanged.

Distribution: `dist/scos.img`, 67,108,864 bytes. SHA-256:
`3726da9c3a700c7f12a0f5bcda22c40d3e43722423c68b7094c7c683970f71df`.

## Follow-up review of the reuse decision

The repeated clarification was checked against the existing published port
`d2ea7ad49af0dbaebb3f13f0f5583ef543971a82`; that implementation was preserved,
not replaced by a second rewrite. There are 8,871 lines in the 29 desktop C
files. In particular, the Solitaire feature source is byte-identical to the
original reference. Other app files retain original code with targeted fixes
and native-interface changes as described above. Line counts describe source
retention, not correctness or functional completeness.

The follow-up review rebuilt the current source, checked exact equality with
`dist/scos.img`, and independently booted that image under x64 EDK2/QEMU at
256 MiB. Real PS/2 keyboard events switched to TTY1, executed `calc 6 - 2`
with visible result `4`, and ran `appstrt solitaire`, returning to the original
compositor with the Solitaire window and cards rendered. These are fresh smoke
checks; the broader fixture and interaction matrix above records the existing
port's earlier verification, not a claim that every case was rerun here.
Temporary screenshots and emulator state from this review were removed.

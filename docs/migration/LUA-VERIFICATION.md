# First native Lua app integration — verification record

2026-09-20; unnumbered x64 development build. The user authorized open-source
Lua integration and independent app registration, not new hardware drivers.

## Delivered architecture

- Each existing C app self-registers from its own source. The linker retains
  registrations in the read-only-after-relocation region. Registry lookup,
  validation and document-handler selection live outside the compositor.
- The WM's hardcoded desktop app list is gone. Desktop metadata, launcher
  search/scroll and terminal app lists enumerate registered clients, excluding
  internal dialog/error clients. Existing short desktop labels are preserved.
- Runtime-discovered VFS `.lua` sources compile with upstream Lua 5.4.9 into
  real Lua bytecode and execute in separate per-window states. The script
  runtime implements the existing native app callback interface.
- Native window surfaces, input, resizing, CPU accounting and allocation-owner
  accounting are reused. No web view, simulated UI, external Lua process or
  new hardware driver is involved.
- `apps/examples/{counter,sketch}.lua` supply editable VFS examples. Users can
  create new source inside SCos and launch it without rebuilding the image.
- Upstream provenance, pre-modification file hashes, license texts and the
  bounded freestanding support subset are in `third_party/`. Binary-image
  license notices are available through `/system/licenses.txt`.

## Checks executed

Temporary Python/QMP/GDB verification harnesses used disposable QEMU disks.
They were removed after the final verification. No diagnostic app or test hook
ships. All guest checks used the normal optimized production objects: no
special compiler retention/debug variant was needed for this change.

### Compiler, libraries and failure handling

Fresh USB-storage/xHCI keyboard/mouse guest, 128 MiB:

- Actual syntax compilation, arithmetic, closures, metatables, table sorting,
  string patterns/replacement/formatting, UTF-8 operations and app-local file
  read/write passed.
- Integer formatting through 9223372036854775807, floating-point formatting,
  powers, negative division/modulo, square root, trigonometry, exp/log and
  floor/ceil passed selected assertions.
- **108 decimal conversions** matched independently computed IEEE-754 bytes,
  including negative zero, subnormal/min-normal/max-finite values, overflow,
  underflow and deterministic random decimal/exponent combinations.
- Source syntax errors; infinite startup, paint and tick loops; recursive stack
  exhaustion; oversized allocations; non-string errors; invalid callback use;
  attempted path traversal; app-data quota overflow; oversized drawing;
  exponential pattern work; large plain string search; huge empty-table moves;
  deeply nested compiler input; attempted GC finalizers and binary chunks all
  failed visibly without preventing subsequent app launches.
- Added C-library checks are important: instruction hooks alone do not interrupt
  a C table loop or string search. `__gc` is rejected because upstream runs GC
  finalizers with hooks disabled. Protected-call globals are excluded so limit
  exceptions cannot simply be swallowed and retried forever.
- After tested success/error windows closed, their allocator-owner byte counts
  returned to zero. Two simultaneous Counter windows retained independent Lua
  counts; real USB keyboard events saved their different counts into their
  intentionally shared app-ID RAM directory.
- Registry capacity, native-ID collision refusal and the 32-item desktop grid
  limit passed with more Lua source files than the registry accepts.
- All ten existing native apps plus Counter and Sketch opened, rendered and
  closed together. This is app lifecycle/integration coverage, not exhaustive
  verification of every native command/game/browser feature.

### End-to-end authoring

A separate fresh USB guest used **native keyboard events**, not a VFS source
injection, to:

1. `touch /home/apps/handmade.lua` in Terminal.
2. Open that path in Notepad with `appstrt`.
3. Type a complete Lua paint callback, including punctuation.
4. Save with Ctrl+S and dismiss the existing RAM-save confirmation.
5. Close the editor and run `appstrt handmade`.
6. Verify that the saved source matched the typed program, the Lua app reported
   no error, and its actual native surface contained the requested blue pixels.

### Native input, discovery and persistence

Integration checks passed with:

- q35 USB-storage boot, xHCI keyboard/mouse, 128 MiB.
- q35/AHCI boot with PS/2, 128 MiB (correctly RAM-only).
- i440fx/PIIX compatibility-IDE boot with PS/2, 128 MiB.

A custom file was created at runtime, then launched through real terminal
keyboard input with absolute and relative paths. Mouse clicks invoked Lua and
updated an app-data file. Native resize gestures changed the surface dimensions
without breaking the script. The launcher scrolled beyond ten entries and
searched/launched the newly discovered app. Editing source and relaunching
produced its new title/output. A runaway startup launched through the real
terminal dispatch stopped at its budget while the desktop remained usable.

On the supported compatibility-IDE configuration, the existing verified save
path wrote the custom source and app data to the disposable disk. A firmware
reboot loaded the saved tree; discovery found the custom app again, its edited
code ran, and the prior app-data value was read back. The initial q35/AHCI save
attempt was correctly refused, not treated as a successful persistence test.

### Build/artifact integrity

- Clean optimized build with warnings-as-errors; only specific upstream
  style warnings are disabled for vendor objects.
- Final freestanding ELF has no unresolved symbols or host runtime dependency.
- Rebuilding reproduces the published image bytes.
- Frozen 32-bit artifact/milestone checksum remains unchanged; `os.html` remains.
- Rebuilt `dist/scos.img` and checksum published together; actual downloaded
  GitHub image bytes compared against the local build.

## Limits and PC follow-up

The [app guide](../LUA-APPS.md) describes the exact API, omitted globals,
2-MiB per-window arena, source/data/callback limits, naming/discovery rules and
RAM/ATA persistence distinction. The port does not add USB mass-storage saving,
a native ELF executable loader, dynamic shared modules, networking or process
isolation. Lua and all other app code still execute within a cooperative ring-0
kernel. The tested resource guards are not a general security proof or a hard
real-time scheduler.

On the user's PC, try Counter/Sketch, create and save a small new app in Notepad,
launch it through `appstrt`, edit/relaunch, and exercise its input/resizing next to
native windows. Emulator results do not replace that hardware confirmation.

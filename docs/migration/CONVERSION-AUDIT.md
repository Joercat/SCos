# Existing-source conversion completion audit

2026-09-20 — unnumbered x64 UEFI development.

The user clarified: **finish converting everything already written; do not add
new drivers until they have verified the OS behaves as intended.** The earlier
DRIVER64 follow-up was only a hardware-driver inventory, not a complete source
parity audit. This audit checks the broader original source at
`6717943f977a7e0f95f0ace5fa48cfe6a564f873` against the native tree.

## Source accounting

The original kernel source directory contains 38 C files. All are accounted for
below; a missing filename is not automatically a missing feature, nor is a
matching function name proof of behavioral equivalence.

| Original source | Native disposition |
| --- | --- |
| Ten `app_*.c` files | Original apps in `kernel/desktop/`, with native-width/shared-service adapters and documented correctness repairs. Browser keeps its original unavailable notice, not a new browser implementation. |
| `apps.c`, `cards.c`, `confirm.c`, `err.c`, `kbd.c`, `klog.c`, `mouse.c`, `rtc.c`, `theme.c`, `tty.c`, `vfs.c`, `wm.c` | Adapted originals in `kernel/desktop/`; APP64 and DRIVER64 record integration and changes. |
| `neofetch_art.c`, `panic_art.c` | Original dedicated artwork retained in `kernel/desktop/`. |
| `acpi.c`, `ata.c`, `pci.c`, `usb.c` | Active native adaptations in `kernel/drivers/`, not new hardware-driver imports. See DRIVER64 for inherited hardware limits. |
| `boot_screen.c` | **Omission found and restored in this pass.** Original logo/spinner, progress bar and completed-step log now use GOP/native startup. |
| `kmain.c` | Native early ownership/interrupt setup in `kernel/src/main.c`; app/driver sequencing in `kernel/desktop/platform.c`, now reconnecting the original boot-screen behavior. |
| `idt.c`, `pit.c` | Native tables/vector ABI/PIC/PIT dispatch in `kernel/src/interrupt.c` and x86-64 stubs. **Missing IRQ-rate protection restored in this pass.** |
| `panic.c` | Native direct-scanout/serial panic remains independent of WM and heap. **Previously omitted register fields and stack preview restored in this pass.** Old 32-bit stack offsets and unchecked pointer reads are not reused. |
| `mm.c` | Owned native page allocator plus `kernel/desktop/heap.c`; fixed low-RAM/32-bit assumptions replaced, allocation ownership retained and strengthened. |
| `fb.c` | Original drawing primitives retained in `kernel/desktop/fb.c`; UEFI GOP replaces BIOS/VBE. The unsafe paging-off MTRR manipulation is not restored; current uncached scanout policy is explicit. |
| `font_data.c` | Native `kernel/src/font.c`, shared by the loader, console and original desktop rendering. |
| `cpumeter.c`, `lib.c` | Native adaptations in `kernel/desktop/`. AMD64 arithmetic replaces the obsolete i386 compiler division helpers. |

The old BIOS stages, i386 assembly entry/interrupt ABI, linker/load assumptions
and image builder are superseded by the approved UEFI-only boot chain. They
must not be re-enabled just to make filenames match. Other deliberate removals:

* Old kernel-sector copies and `fs_image_kernel_bytes` relied on the obsolete
  flat-image/BIOS layout; no pretend copies of boot files are exposed in RAM.
* The old speculative xHCI BAR-relocation helpers are replaced by native-width
  mapping at firmware placement, not missing hardware functionality.
* File Manager deletion callbacks stay removed per the user's terminal-only
  deletion requirement. Deleting files is still available through `rm -s`.
* Previously retired diagnostic commands/test UIs and v86 stay removed.

## Restored behaviors

### Original startup presentation

The restored `boot_screen.c` is retrieved from the original source, not replaced
with a new loading-screen imitation. It shows the existing animated SCos logo,
bar and actual completed steps: native core, CPU identity, managed memory, GOP,
VFS, ATA discovery, saved-tree/default selection, USB/PS2 availability, RTC and
compositor/app registration. Device absence is stated, not called a successful
hardware initialization. `[done]` means that the reported stage completed.
Only the original `Finishing... I think...` animation is ceremonial.

The progress value represents initialization stages, not measured elapsed-time
percentage. Lines are clipped to screen width, vertical overflow is bounded,
and progress is clamped. The finished log remains briefly readable while USB
events are serviced. No release number or temporary diagnostic screen is added.
Early boot validation/fault reporting remains available before the splash exists.

### Native fault diagnostics

The exception report now prints all saved general-purpose registers, RIP/RSP,
RFLAGS, CS/SS, vector/error and CR2/CR3, followed by up to sixteen 64-bit stack
words. A read is permitted only within known firmware-described RAM and current
present mappings. Null, guard, device, out-of-range or unmapped addresses return
`<unavailable>` instead of blindly walking the interrupted stack. This is a raw
bounded preview, **not** a symbolic stack unwinder.

The original dedicated panic art and actual halt remain. Fault output still
bypasses the desktop heap/compositor. The interrupt/memory/console paths remain
compiled general-register-only; no SIMD desktop callback is called from an IRQ.

### Interrupt-rate protection

The original 1500-interrupts-per-second threshold for non-timer PIC lines is
retained. PIT boundaries sweep counters and mask a storming line. A real
unhandled PIC line is masked and acknowledged instead of repeatedly interrupting
or panicking immediately. Architectural CPU faults still enter the native panic
path, and spurious IRQ7/IRQ15 handling precedes counting/dispatch.

IRQ handlers only update counters/masks/pending flags. `interrupt_poll()` in WM
and TTY foreground loops snapshots pending warnings atomically, logs the line
and cause, and queues the existing nonfatal error screen. No logging/desktop
error UI is invoked from the rate-sweep IRQ call graph. This restores original
protection; it is not a new interrupt-controller driver or a guarantee of
recovery from every platform interrupt-routing failure.

## Fresh verification for this pass

Pinned QEMU 11.0.2/EDK2, single-vCPU TCG, disposable regular disks only:

* **128 MiB with xHCI USB, and 256 MiB with PS/2:** inspected the restored boot
  screen; native RAM VFS commands, all ten app windows together, confirmed WM
  termination/restart and confirmed ACPI shutdown passed.
* **PC/legacy ATA plus USB:** actual Notepad keyboard input created
  `converted startup`; Ctrl-S and confirmed shell save succeeded. A new guest
  showed the saved-tree boot step and loaded identical document bytes. Bytes
  outside the dedicated persistence partition were unchanged.
* **Invalid-opcode fault injection:** injected UD2 into the no-longer-used
  startup entry, set a known R15 value and redirected RIP there. The actual
  exception path printed vector 6, the expected 64-bit R15 and stack words,
  then halted. Screenshot confirmed registers, stack, art and reason together.
* **Guarded-stack/double-fault injection:** the same test with RSP inside the
  unmapped kernel stack guard reached vector 8 on its IST stack, printed the
  expected register value and `<unavailable>` for the original stack, then
  halted without resetting the guest.
* **IRQ storm path:** seeded the guest's IRQ1 rate counter to the original
  threshold, then let the real PIT sweep run. The deferred warning identified
  IRQ1; USB input dismissed it and created a VFS file afterwards. This exercises
  the protection path, not a claim of generating a physical hardware storm.
* **Unhandled PIC path:** a temporary `INT 0x2e` fixture exercised vector 46,
  then restored the interrupted state. IRQ14 was reported as masked/no handler;
  USB input and file creation still worked afterwards. No fixture code is in
  the delivered binary.
* Build uses warnings as errors; disassembly checks found no SIMD registers in
  the native interrupt/memory/console and PS/2/CPU-meter objects. The frozen
  image guard and clean-rebuild byte comparison passed.

An initial fault-test fixture overwrote the currently executing instruction,
which happened to be in shared `memcpy`; corrupting the diagnostic path's own
helper caused repeated faults. The corrected fixture targets the dormant startup
entry so it tests exception handling rather than destroying code the handler
needs. No result from that invalid fixture is counted as a passing test.

Temporary scripts, snapshots, patched guest state, screenshots and logs are
removed after verification. Existing APP64/DRIVER64 evidence remains historical;
this pass does not claim to have repeated every prior scenario or every app
workflow. Source accounting plus these tests is not proof of bug freedom.

## Boundary for physical verification

No additional original source component was identified as awaiting conversion
after these restorations and the disposition audit above. This does **not** mean
all hardware is supported. The documented original-driver limits remain (for
example, legacy ATA rather than AHCI/NVMe/USB mass storage, limited ACPI AML and
xHCI topology support). Firmware GOP and the cooperative ring-0 execution model
also remain. Physical-PC behavior still needs the user's verification.

No new GPU, NIC, Wi-Fi, storage-controller or browser integrations are authorized
by this work. Wait for a new request after that physical verification. The
frozen 32-bit image and `os.html` stay unchanged.

The subsequent [PC test guide](PC-TEST.md) records a fresh parity check and
USB2/USB3, larger-media, hub, CPU-profile and alternate-GOP boot tests on the
unchanged image, plus the remaining physical-machine prerequisites.

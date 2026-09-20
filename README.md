# SCos — native operating system

SCos is a custom, bootable x86 operating system with its own kernel, desktop,
applications, terminal and six text consoles. It is not a Linux distribution.
`os.html` remains the original desktop design reference.

**Current release: r42, still 32-bit.** Boot uses legacy BIOS/CSM, an MBR loader
and a VBE framebuffer. There is no UEFI-only or x86-64 kernel build yet.

## Build and boot

On an x86 Linux development machine with GCC capable of `-m32` freestanding
compilation, GNU binutils, Make and Python 3:

```sh
make                         # build/scos.img
sha256sum -c dist/scos.img.sha256  # verify a published image, from repo root
make font                    # optional: regenerate the bitmap font
make clean                   # remove generated build files
```

The image is a raw bootable disk image, not a file to copy into an existing USB
filesystem. Writing it to a whole USB device overwrites that device's contents;
back up the correct device first. Boot in the working BIOS/CSM configuration.
Disk persistence currently depends on supported legacy ATA access: booting from
USB does not by itself provide a USB mass-storage driver.

The published image is `dist/scos.img`, with its SHA-256 in
`dist/scos.img.sha256`. Build products are not source dependencies.

## Everyday controls

* Double-click desktop icons; launch apps from the terminal with `appstrt`.
* Ctrl+Alt+F1–F6 select independent text consoles; Ctrl+Alt+F7 returns to a
  desktop that is still alive. `tty [1-6]` also selects a console.
* `kill --system 2`, after confirmation, terminates the compositor and GUI apps.
  Unsaved GUI edits are lost. `wm` starts a fresh desktop from a stopped console.
* Other built-in system rows are kernel subsystems, not independently scheduled
  processes; unsupported system kills are rejected rather than simulated.
* Power controls are in the taskbar menu; `shutdown` is also available in the
  terminal and TTY. If firmware cannot power off, SCos displays the fallback
  screen and halts. Restart and confirmed Factory Reset remain available.
* `help` lists current commands. Kernel service/error logs, panic handling and
  WM-independent error reporting remain; temporary input capture and hardware
  diagnostic screens have been retired.

## r42: shutdown and cleanup

The user confirmed **r41 input is fully functional on the real PC**: stationary
clicks, repeated characters and Backspace no longer require spamming. Its
packet-sized HID requests, report assembly and input behavior are retained.

Shutdown had a definite boot integration bug: `acpi_init()` was never called,
so every power-off action saw ACPI as unavailable. r42 initializes it during
boot and hardens the newly reachable path:

* Validate ACPI table sizes/checksums and avoid truncating 64-bit addresses.
* Accept supported extended DSDT and I/O PM1 control descriptions, with legacy
  table fallback where appropriate.
* Bound constant `_S5` package decoding; read independent PM1a/PM1b sleep types;
  never guess a missing sleep type.
* Preserve unrelated PM1 control bits and wait for firmware's ACPI-mode handoff.
* Fix sub-tick timer waits rounding down to zero.
* Use the same halted fallback behavior from GUI and TTY; returning from a
  power-off attempt is no longer reported as success.

This remains a limited ACPI implementation, not a full AML interpreter.
Dynamic sleep objects, required firmware methods such as `_PTS`, hardware-reduced
sleep controls and unsupported address spaces need a fuller future implementation.
The initialization defect is fixed; physical power-off still needs confirmation.

At the user's request, the temporary test suite, v86 runtime/preview tooling,
diagnostic screens/button, `diag`, `inputtrace`, raw-input recorder and raw HID
report dumps were removed **after verification**. The old tooling is available
in Git history at r41 (`72d6176445f1f5b66f2dd575de3f38b518ad32c3`); no emulator is
required to build or run SCos. Panic/error handling and service logs are not
removed. See [verification notes](docs/RELEASE-r42.md).

## Preparing x86-64 — implementation has NOT begun

The custom kernel and SCos identity will remain. The next step is a staged
port, with lightweight upstream libraries and selected driver source adapted
where practical, not automatic Linux binary/module compatibility.

* [Migration plan, source audit, contracts and blocking decisions](docs/migration/README.md)
* [Researched component shortlist and porting requirements](docs/migration/COMPONENTS.md)
* [Exact upstream research references](docs/migration/candidates.json)

No library, GPU/network driver or new bootloader has been integrated. Browser,
network stack, protected userspace, 64-bit memory management and GPU acceleration
are future work. The current browser remains a stub; network metrics are not
fabricated. The planning documents explicitly separate researched candidates
from tested, working SCos support.

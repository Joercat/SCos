# SCos — native operating system

SCos is a custom, bootable x86 operating system with its own kernel, desktop,
applications, terminal and six text consoles. It is not a Linux distribution.
`os.html` remains the original desktop design reference.

**Current release: r43, still 32-bit.** Boot uses legacy BIOS/CSM, an MBR loader
and a VBE framebuffer. There is no UEFI-only or x86-64 kernel build yet.

**Storage corrections verified:** r43 fixes the wrong-disk write, reset double-
free and failure reporting, incomplete serialization, and malformed-load handling.
Use the new **`dist/scos-32bit-r43.img`** for the next physical test, not the
unchanged r42 archive. See [r43 changes, checks and limitations](docs/RELEASE-r43.md).
Final hardware acceptance and permission to convert are still pending.

## Build and boot

On an x86 Linux development machine with GCC capable of `-m32` freestanding
compilation, GNU binutils, Make and Python 3:

```sh
make                         # build/scos.img
sha256sum -c dist/scos-32bit-r43.img.sha256  # verify current release
make font                    # optional: regenerate the bitmap font
make clean                   # remove generated build files
```

The image is a raw bootable disk image, not a file to copy into an existing USB
filesystem. Writing it to a whole USB device overwrites that device's contents;
back up the correct device first. Boot in the working BIOS/CSM configuration.
Disk persistence requires one uniquely verified SCos-owned legacy ATA disk.
`save` confirms the target; unknown or multiple targets are refused. Booting from
USB does not by itself provide a USB mass-storage driver. See the release notes
for the FS2 format and single-slot power-loss limitation. Back up older data: old
unmarked images/FS1 saves are not automatically adopted or migrated.

The current image/checksum are `dist/scos-32bit-r43.img` and
`dist/scos-32bit-r43.img.sha256`. The permanently retained original **scos 32bit** image is `dist/scos-32bit.img`, with its
SHA-256 in `dist/scos-32bit.img.sha256` and provenance in
[the milestone manifest](docs/milestones/scos-32bit.json). It is the unchanged
r42 image, not a newly fixed release. Never overwrite or delete it; further
32-bit fixes must use new filenames. `dist/scos.img` is reserved for future
64-bit releases. `build/scos.img` is only a disposable build output.

Run `python3 tools/check_milestone.py` to verify retention. The check also runs
with `make`. No GitHub workflow was added: the connected GitHub App does not
have workflow-write permission. Build products are not source dependencies.

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
  screen and halts. Factory Reset reports RAM-only operation when persistence is
  unavailable; a failed disk reset reports failure without automatically rebooting.
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
removed. See [historical r42 verification notes](docs/RELEASE-r42.md).

The new, separately requested [QEMU host environment](docs/migration/EMULATOR.md)
is obtained and tested. It boots this 32-bit image and supports future x86-64
full-system testing. It is not imported into the guest or required by the build.

## Preparing x86-64 — implementation has NOT begun

The custom kernel and SCos identity will remain. After the verified 32-bit safety fixes, final physical test and explicit user
permission, the next architectural step is a staged port, with lightweight
upstream libraries and selected driver source adapted
where practical, not automatic Linux binary/module compatibility.

* [Migration plan, source audit, contracts and blocking decisions](docs/migration/README.md)
* [Researched component shortlist and porting requirements](docs/migration/COMPONENTS.md)
* [Wider GPU, Ethernet, Wi-Fi and integrated-browser comparison](docs/migration/HARDWARE-AND-BROWSER.md)
* [Obtained QEMU runtime, reproducible bootstrap and usage](docs/migration/EMULATOR.md)
* [Exact upstream research references](docs/migration/candidates.json)

No library, GPU/network driver or new bootloader has been integrated. Browser,
network stack, protected userspace, 64-bit memory management and GPU acceleration
are future work. The current browser remains a stub; network metrics are not
fabricated. The planning documents explicitly separate researched candidates
from tested, working SCos support.

The current 32-bit bug-fix round is **r43**. Reset to **r1** only when the user
explicitly authorizes conversion. Completing conversion is not permission to
start drivers/resources: that requires a separate instruction afterward.

# SCos — native operating system

SCos is a custom operating system, not a Linux distribution. `os.html` remains
its desktop design reference.

**Active development: unnumbered x86-64 startup foundation.** The user authorized
conversion; the final working 32-bit r43 image is now permanently frozen.
There is no new release number until conversion starts on the user's PC.

* **64-bit:** [`dist/scos.img`](dist/scos.img) — BIOS-to-long-mode bootstrap,
  native kernel, protected page mappings, exceptions and real timer interrupts.
  **No desktop, keyboard/mouse, storage persistence or interactive shell yet.**
* **Frozen 32-bit:** [`dist/scos-32bit.img`](dist/scos-32bit.img) — existing desktop
  and applications; [r43 notes](docs/RELEASE-r43.md),
  [provenance](docs/milestones/scos-32bit.json). This file is not overwritten by builds.
* [64-bit boot contract, checks, limitations and next steps](docs/migration/BOOT64.md).

## Build and boot

Use an x86 Linux host with GCC supporting freestanding `-m64` and `-m32`, GNU
binutils, Make and Python 3. No host C library is linked into either kernel.

```sh
make                           # active AMD64 build/scos.img
sha256sum -c dist/scos.img.sha256
python3 tools/check_milestone.py
make legacy                    # legacy/i386/build/scos.img; identical r43 bytes
make clean                     # remove active disposable build output
python3 tools/setup_qemu.py     # optional pinned host emulator, no root required
python3 tools/run_qemu.py       # snapshot boot; serial log and private QMP socket
```

Old 32-bit sources/build rules are relocated to `legacy/i386/`, not compiled
into the new kernel. Necessary 16/32-bit BIOS transition instructions remain in
`boot/`; they are not the old 32-bit OS. `make -C legacy/i386 font` regenerates
the legacy bitmap font if needed.

The images are raw bootable disks. Writing one to an entire USB drive destroys
that drive's contents: back up and verify the target first. The new image needs
**legacy BIOS/CSM, EDD disk services, VGA text output and an AMD64 CPU with NX**.
It has no native UEFI/GOP path. QEMU boot is verified; physical-PC boot is not.
The foundation stops at a readiness message and idles with real timer interrupts;
use host controls or the physical power button to stop it. No USB driver or
saved-data access is enabled in the new kernel.

For the frozen desktop, persistence requires one uniquely verified SCos-owned
legacy ATA disk. USB boot alone does not supply USB mass-storage persistence.
See r43 notes for ownership checks and single-slot power-loss limitations.

## Frozen 32-bit desktop controls (not yet in the 64-bit build)

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

## Conversion and later integrations

* [Migration plan and remaining contracts](docs/migration/README.md)
* [Component research](docs/migration/COMPONENTS.md)
* [GPU, Ethernet, Wi-Fi and integrated-browser comparison](docs/migration/HARDWARE-AND-BROWSER.md)
* [QEMU provenance and usage](docs/migration/EMULATOR.md)
* [Upstream research references](docs/migration/candidates.json)

No new third-party driver/library has been imported into the guest. Porting the
existing core/desktop comes first; new drivers and resources require separate
permission after conversion. Network/browser support, protected userspace,
SMP, high-RAM allocation and GPU acceleration are not implemented by this step.

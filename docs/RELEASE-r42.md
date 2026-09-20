# r42 verification and scope record

Date: 2026-09-19. This is still the custom **32-bit** SCos kernel.

## Physical evidence

The user reported r41 input fully functional on the actual PC: no spam clicking
and repeated characters/Backspace work. That is physical acceptance of r41's
input behavior, not a physical r42 shutdown result. The reported remaining
failure was the safe-to-turn-off screen with the machine still powered on.

Source audit found `acpi_init()` defined but never called by the boot sequence.
Therefore `acpi_ok` remained zero and all shutdown callers took the unavailable
path. r42 adds the initialization call before use. Object inspection confirmed
a relocation/call from `kmain.o` to `acpi_init`, not merely an unused function.

The now-reachable path also validates table checksums/lengths, rejects truncated
addresses, supports reachable extended DSDT/I/O controls, parses independent
sleep types, preserves other PM1 control bits, and waits for SCI enable. The
ACPI specification describes separate PM1a/PM1b sleep values and enables:
[3](https://uefi.org/specs/ACPI/6.4_A/16_Waking_and_Sleeping.html).

A separate timer defect rounded positive sub-tick waits to zero; those now round
up. Power-off returning to SCos reports failure, not success, and the TTY also
enters the same halted fallback as the desktop/terminal.

## Executed verification

* Clean 32-bit build: PASS. The known assembler `.note.GNU-stack` linker warning
  remains; it is not a claimed new userspace/NX implementation.
* Retiring packet-level USB regressions T1–T17: PASS after recorder removal,
  including the old oversized-request reproduction, repeated key/click/wheel
  delivery, report assembly, ring wrap and PS/2 formats. The functioning receive
  algorithm was not replaced with another sensitivity adjustment.
* CPU-brand, shared-confirmation and MTRR planner host checks: PASS.
* Temporary ACPI host check against the production `acpi.c`: PASS. Synthetic
  checksummed RSDP/RSDT/FADT/DSDT images exercise actual initialization; mocked
  port I/O verifies distinct A/B types, preserved bits and delayed SCI enable.
  Invalid checksums, truncated packages and out-of-range type values reject;
  word/dword constants decode; reachable XSDT/X_DSDT/GAS controls work;
  above-4-GiB entries do not truncate; invalid XSDT falls back to RSDT.
* Temporary PIT check: PASS. At 100 Hz, waits of 1 ms, 0 ms and 11 ms consume
  1, 0 and 2 ticks respectively.
* All **32** retiring emulator scenarios: PASS on the cleaned production code.
  The checks for intentionally removed `diag`/`inputtrace` features were retired;
  the two older AML fixtures had invalid package lengths and were corrected.
  Scenario names inherited from older rounds do not imply the recorder remains.
* After the final stale diagnostic-hint/comment cleanup, final-image scenarios
  **2, 7, 10 and 30** passed again from a temporary harness outside the repository:
  terminal commands, Settings, shutdown fallback, confirmations and WM teardown.
* Final ELF inspection: **ELF32, Intel 80386**. The standalone ELF64 compiler
  probe was a toolchain check outside the kernel, not a converted OS.
* Symbol/source inspection: no `diag_run`, `diag_manual`, `usb_inputtrace`,
  raw-input recorder or `is_v86_box` runtime remains.
* Build and distribution image bytes match. The pushed GitHub image blob is
  checked separately during delivery against the same SHA-256.

Final image SHA-256:

```
84f87224a5fef6e2cd0982b28708e68a8d44f0a523b010f8c2cb23f43e7cb926
```

These checks do **not** prove physical ACPI power-off. The retiring emulator
covers the unavailable-ACPI fallback, not the user's chipset. A QEMU installation
attempt could not fetch the needed package indexes; no QEMU result is claimed.
Full AML evaluation, `_PTS`/other required firmware methods, hardware-reduced
sleep and unsupported address spaces remain outside this limited driver.

## Retired artifacts

Removed the repository test directory, v86 setup/server/screenshot tooling and
runtime assets; `make test`, component-test, vendor and preview targets; the
held diagnostic screen and Settings button; `diag` and `inputtrace`; raw HID
report recording/dumps and descriptor copies used only for recording. Temporary
external verification copies are removed after their last run.

Kept essential panic/error screens, service/error logs, actual hardware drivers,
all regular applications, six TTYs and the original `os.html`. Build-only
`tools/makedisk.py` and `tools/fontgen.py` remain. Historical checks can be read
from r41 commit `72d6176445f1f5b66f2dd575de3f38b518ad32c3` without restoring them
into the production tree.

## Migration scope

Only planning/research documents and upstream reference metadata were added.
No Linux kernel substitution, 64-bit entry path, new bootloader integration,
userspace libc, network/GPU driver or real browser port was started. See
[migration preparation](migration/README.md) for prerequisites and unresolved
hardware, license and toolchain decisions.

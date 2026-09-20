> **Resolved in r43:** the findings below describe the historical r42 image.
> The new `dist/scos-32bit.img` addresses them; see
> [corrections and actual verification](RELEASE-r43.md). Physical acceptance is
> still pending. Keep this audit as the root-cause record, not a current claim
> that r43 still writes to the first arbitrary ATA disk.

# Final 32-bit review — NOT yet cleared for hardware acceptance

Reviewed 2026-09-19. Historical image: **r42 i386**, available in Git history
(commit `8dc5b5e`), SHA-256
`84f87224a5fef6e2cd0982b28708e68a8d44f0a523b010f8c2cb23f43e7cb926`.

**The audit found a real storage safety defect. Do not treat this as a clean
final-test release. Do not use `save` or Factory Reset on a real machine with
other attached ATA-accessible disks.** The image is preserved as requested, not
silently patched or represented as bug-free. No kernel/input code was changed
in this planning/audit round. Required corrections are listed below.

## New checks which passed

Using QEMU 11.0.2, x86-64 system emulator, software TCG, `pc`, `-cpu max`,
256 MiB, standard VGA, no networking, disposable snapshot of the original image:

* BIOS/VBE boot reached the desktop at 1024×768. Serial output showed r42 and
  `acpi: power ready, PM1a=604 PM1b=0 S5a=0 S5b=0`.
* PS/2 keyboard: repeated characters, Backspace editing, all six Ctrl+Alt+F1–F6
  consoles and retained first-console history were checked by decoding actual
  QMP screenshots, not by merely observing serial boot messages.
* `shutdown` confirmation cancellation returned to the console.
* Confirmed `kill --system 2` stopped the WM; serial output reported released
  compositor/application resources. `wm` started the desktop again.
* Confirmed TTY `shutdown` powered the emulated machine off: QMP disconnected
  and the host QEMU process exited with status 0. This tests the guest's ACPI
  power path, not a host-injected `system_powerdown` or QMP `quit`.
* Repeated the same console/input/WM/shutdown sequence with `qemu-xhci`,
  `usb-kbd` and `usb-mouse`. Enumeration found one USB keyboard and one mouse,
  packet-sized requests, and the sequence passed again.
* PS/2 relative mouse movement and stationary double-click opened the real
  Terminal window during the separate storage reproduction below.
* The pinned emulator bootstrap was independently run into a fresh disposable
  directory; it built its musl loader and reported QEMU 11.0.2 successfully.

This is a focused new QEMU review, **not** a rerun of every retired 32-scenario
suite case. It does not establish physical motherboard power-off, all device
compatibility, or the absence of other bugs. r41's physical input acceptance
still stands; r42's physical shutdown remains unconfirmed.

## Blocking finding: save targets an unrelated ATA disk

### Root cause (source, not a guess)

`kernel/src/ata.c` enumerates primary/secondary master/slave devices into
`devs[]`. Both `ata_read_sectors()` and `ata_write_sectors()` always access
`devs[0]`. `fs_image_save()` writes at LBA 2048 without verifying ownership,
boot-device identity or a reserved SCos storage region. A BIOS USB boot device
is not necessarily that first ATA device; a BIOS drive number is not an ATA
array index. The presence check in Terminal only establishes that an ATA disk
exists. A SCos filesystem magic check on **load** does not authorize a write to
an unrelated disk.

### Actual reproduction, using only disposable virtual files

* Created an 8 MiB non-bootable foreign disk. At LBA 2048 its first 16 bytes were
  zero, followed by marked foreign data. No SCos filesystem existed there.
* Attached it as primary master (`if=ide,index=0`). Attached the retained r42
  image as primary slave with `bootindex=1`, protected by `snapshot=on`.
* Booted SCos from the second disk, moved the emulated mouse and double-clicked
  Terminal. Typed `save` through QMP keyboard input.
* Terminal printed **“Filesystem image written to disk.”**
* After stopping this reproduction VM, the unrelated first disk had changed.
  Its LBA 2048 began with `SCOSFS1\0`, replacing foreign data. The retained r42
  image's SHA-256 stayed unchanged.

Observed disposable foreign-disk hashes:

```
before: 3f4ea0efa95bc02571bbd341f90f0edf04288db892c73b4b181ee0ae76860ca1
after:  d85805e2985962e593571a0207cbda15a5c4031a816a4972465a17bc758e5f64
```

No host physical disk, USB passthrough or real user data was attached. The only
writable test disk was a newly created disposable file. This is positive proof
of the unsafe target selection, not an inference from a possible edge case.

## Related findings from source review

These have concrete source paths but have **not all been independently exercised**:

* `app_settings.c:reset_confirm_cb()` ignores the result of `fs_image_save()`,
  then claims all user data was erased and reboots. A write failure or lack of
  supported persistence cannot honestly produce that success message.
* `ata.c:walk_serialize()` returns early on capacity exhaustion without reporting
  failure to the caller. `fs_image_save()` can therefore report success after
  saving an incomplete tree. Its fixed-size path concatenation also needs bounds
  validation before copying, not after it.
* `fs_image_save()` writes only the new serialized length. Old bytes beyond that
  length remain in the reserved persistence area. Replacing the directory image
  is not a secure wipe; the Factory Reset wording must match the actual behavior.
* The image buffer allocation is not checked before use in load/save.
* `image_apply()` uses `off + size > len`, which can wrap for malformed 32-bit
  sizes. Use subtraction-based bounds checks and validate before mutating the
  live tree. Its early return is not propagated into `fs_image_found`.
* `system_files_init()` reads its purported boot-chain copies through the same
  first-ATA-disk API. On a different boot device, those files need not be copies
  of the booted image, contrary to the embedded README's claim.
* `tools/makedisk.py` does not enforce the future kernel size against the low-
  memory BIOS staging area or the persistence region at LBA 2048. The current
  197,592-byte kernel fits; this is a growth/format guard requirement, not proof
  that the present image overlaps those regions.

### Confirmed console parity gap

The first reproduction attempt used `save` in TTY1. It returned
`unknown command: save ('help')`; no disk was changed by that attempt. The actual
unsafe write was then reproduced in GUI Terminal. TTY `save` parity must be
resolved through the **same safe storage backend**, not by copying the unsafe
Terminal behavior into TTY.

## Correction and retest gate before the user's final test

1. Define explicit storage ownership/target identification. Do not select an
   arbitrary ATA disk because it is first, and do not equate USB BIOS boot with
   a native USB storage driver. Refuse writes on an unverified/ambiguous target.
   If the selected SCos persistence disk can differ from the boot medium, make
   that distinction visible and require appropriate confirmation.
2. Keep all storage writes inside a verified SCos-owned region; validate capacity
   and complete image serialization before starting a write. Handle allocation,
   overflow, malformed image and I/O errors without reporting success.
3. Factory Reset must clearly distinguish RAM-only reset, successful persistent
   reset and failed persistent reset. Do not automatically reboot after claiming
   a disk wipe that failed. Document that any region clearing is not whole-device
   secure erasure, especially on flash/SSD wear-levelled media.
4. Unify non-GUI save behavior in Terminal/TTY and correct the boot-copy claims.
5. Add targeted disposable-disk checks: foreign disk first, no supported disk,
   multiple candidate disks, full image, malformed image, failed writes,
   save/reboot/load, reset/reboot, unchanged boot sectors and unrelated disks.
6. Publish fixes as a new 32-bit revision at the canonical `dist/scos-32bit.img`
   path, per the later user clarification. Re-run input, WM, console,
   ACPI and storage checks; only then ask for the user's final physical test.

The x86-64 conversion and later driver/library integration remain blocked by
these acceptance/approval gates. A passing QEMU shutdown does not override the
storage finding or grant either approval.

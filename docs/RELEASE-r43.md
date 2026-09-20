# r43 — 32-bit storage safety and reset corrections

Still i386/BIOS. No x86-64 conversion, new GPU/network/browser component, or
change to the physically accepted USB/PS/2 input algorithms. `os.html` remains.

## Image

* New image: **`dist/scos-32bit-r43.img`**, 8,388,608 bytes.
* SHA-256: `43c2bce097389179aff9965779148c82a4907017c392f996a6602f727efb0c97`.
* Kernel: ELF32/Intel 80386; flat kernel 201,688 bytes, 394 disk sectors.
* Exact source provenance: `docs/milestones/scos-32bit-r43.json`.
* The original `dist/scos-32bit.img` remains the **unchanged r42 archive** with
  its original checksum/provenance. It still contains the old defects; do not
  confuse that archive with this new fixed release. Never overwrite either
  retained milestone when publishing another revision.

## Root causes and corrections

### Disk selection and write authorization

r42 used `devs[0]` regardless of which disk booted SCos or owned the destination.
r43 requires exactly one supported ATA disk with the SCos image-layout descriptor
at MBR offset 400, valid boot signature, no partition-table entries, and enough
capacity for the declared 8 MiB layout. Zero or multiple candidates, or failed
candidate reads, disable persistence. Ordinary disks and old unmarked images
are not silently adopted. Writes are restricted to the declared 128 KiB saved-
data region at LBAs 2048–2303; the MBR is rechecked before saving.

The descriptor identifies a **SCos-owned persistence disk**, not necessarily the
BIOS boot medium. BIOS USB drive numbers are not guessed to be ATA indices.
Terminal and TTY `save` now use the same backend and default-no confirmation,
showing the ATA model and warning that it may differ from the boot USB. Factory
Reset identifies the target too. Multiple SCos disks are refused rather than
choosing one arbitrarily. This marker is an ownership convention, not a
cryptographic defense against an attacker who intentionally forges a disk.

### Complete, checked persistence

* Serialization rejects an oversized tree or overlong path **before any write**;
  it no longer silently skips data and reports success.
* Generated `/system/boot`, `kernel.bin` and README copies are reconstructed,
  not stuffed into the 128 KiB user/settings budget. Their labels now accurately
  identify the verified persistence disk, which can differ from the boot disk.
  Kernel-copy length comes from that disk's descriptor, not the running BSS size.
* FS2 has a CRC32 over the count and complete payload (checksum field zeroed for
  calculation). Lengths, types, components, parent order and duplicates are
  validated while constructing a private tree. Invalid or allocation-failed
  loads free the private tree and retain the original defaults/live tree.
* Allocation failures and arithmetic overflow are checked. VFS writes report
  actual allocation failure; aliased-buffer copies are safe and freed correctly.
  Renaming a directory into its own descendants is rejected.
* ATA reads/writes check capacity/ranges, DRQ and final command status; writes
  flush the drive cache. Save invalidates the header first, writes the complete
  zero-filled region, commits the first sector last, then verifies all sectors
  by readback. Errors do not produce a success message.

**Limit:** this is a single-slot saved tree, not a journal or two-slot atomic
filesystem. A failed/interrupted write can destroy the previous saved tree.
CRC/validation reject detectable incomplete saves on next boot; keep backups.
Readback/flush cannot guarantee faulty hardware honors its persistence contract.

### Factory Reset

The deeper review found that `vfs_factory_reset()` freed the root, wrote through
that freed pointer, and then caused `vfs_init_defaults()` to free it again.
r43 builds replacement defaults transactionally and frees the previous tree
once. Allocation failure leaves the existing files intact. Reset restores the
standard theme, mouse preference and double-click interval.

* With verified storage: reset writes and verifies the entire saved-data region.
  On failure, it displays **disk reset failed**, retains a running OS and does
  not claim completion or automatically reboot. RAM defaults may already be
  restored, and the previous disk save may be incomplete; the message says so.
* Without verified storage: confirmation and completion explicitly say **RAM-
  only reset; disks untouched**. No unsupported USB persistence is invented.
* Region clearing is **not whole-device secure erasure** and does not claim to
  defeat SSD/flash wear-leveling or erase other disks.
* Dialog/error allocation failures no longer dereference null pointers while
  trying to report/reset under memory pressure.

### Image construction

The packager rejects malformed/overlapping MBR metadata, missing/out-of-bounds
stage2 patch symbols, oversized stage2, empty/oversized kernels, persistence
region overlap, and final size/signature mismatches. The legacy BIOS staging
policy is conservatively capped at 384 KiB (0x20000–0x80000); it is not a new
firmware memory-map implementation. Make now rebuilds the image when its
packager changes.

## Verification actually performed

Temporary checks used the production C source plus disposable QEMU disks.
No physical host disk, host USB passthrough, or user data was attached.

**Production-source host checks, at both default and `-O2` optimization:**

* Default-tree/reset allocation failure at successive allocations; rollback,
  matching allocation/free accounting, no double-free.
* VFS overflow/alias handling, failed allocation reporting, directory-cycle
  rejection; unique/ambiguous/changed disk ownership and raw range refusal.
* Save/load round-trip; oversized tree and image-buffer OOM cause no writes.
* Injected PIO write, cache-flush and readback failures report failure.
* Corrupt count/length, huge malformed entry size, and load-allocation failures
  retain the previous tree without leaking staged nodes; reset persists defaults.

**Full-system QEMU 11.0.2 / TCG checks:**

* Foreign first disk + SCos second disk: foreign file hash unchanged; confirmed
  save modifies only the SCos region. Cancel causes no write. Boot sectors and
  all bytes outside the persistence region remain unchanged.
* Fresh-process reboot reloads the saved sentinel file.
* Unmarked disks and multiple marked disks refuse persistence without changing
  either disk.
* Actual GUI mouse double-click opens Terminal and executes confirmed save;
  TTY save executes the same backend.
* Emulated xHCI keyboard/mouse: repeated characters, Backspace, all six consoles,
  retained history, shutdown cancellation, WM termination/restart, and guest-
  initiated ACPI power-off (QEMU exits normally).
* A corrupted saved image retains defaults without writing the disk; a valid
  large tree expanded past capacity is rejected before any disk write.
* Actual Settings reset button: cancel leaves the disk unchanged; confirm clears
  the saved region, preserves boot/outside bytes, reboots and reloads defaults.
* QEMU blkdebug injects real disk-write EIO: save reports failure; GUI reset
  displays the failure popup, does not reboot, and leaves the disk unchanged
  when its very first write is rejected. This is separate from mock host tests.
* No verified disk: RAM-only reset/reboot leaves every disk byte unchanged.
* ACPI-disabled machine: shutdown shows the safe-to-turn-off screen instead of
  claiming power-off. Normal ACPI shutdown also passes.

**Packaging:** rejected invalid fixtures; valid packaging reproduced the tested
image exactly. A clean rebuild after the checks produced that same SHA-256.
Only the pre-existing `.note.GNU-stack` linker warning remains. The original
r42 image passed its retention guard throughout.

The temporary host mocks, QMP drivers, screenshot decoder, fixture disks,
screenshots and binaries were removed after verification. No test code was
compiled into the guest. The previously requested QEMU runtime/bootstrap remains
available as host tooling, not a restored v86 stack or diagnostic guest UI.

## Hardware test / migration gates

Use **r43**, not the retained r42 archive, for the next hardware test. Check the
accepted keyboard/mouse behavior, TTY/WM switching, restart and power-off first.
Boot through the previously working BIOS/CSM configuration. USB boot alone does
not provide USB mass-storage persistence; refusing an unverified disk is now
intentional. Back up any data before flashing—the whole-device flash itself is
destructive regardless of the OS's write protections.

These checks address the reproduced failures, not a proof that every command,
hardware device or firmware combination is bug-free. Physical power-off and
final 32-bit acceptance still require the user's test. Conversion remains
blocked until explicit permission afterward; driver/resource integration still
requires its own later authorization.

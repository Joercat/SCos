# SCos release and transition rules

Current final-test gate is CLOSED: `docs/AUDIT-32BIT.md` documents a reproduced
wrong-disk write in r42 plus reset/serialization issues and missing TTY `save`.
Address these as 32-bit safety fixes and retest before requesting final hardware
acceptance. Do not use the retained image's save/reset against real disks.

* Keep the custom SCos kernel and identity; do not replace it with Linux.
* `dist/scos-32bit.img` is a permanent, immutable 32-bit milestone. Never delete,
  overwrite, rename away or boot it as a writable disk. Its expected SHA-256 is
  `84f87224a5fef6e2cd0982b28708e68a8d44f0a523b010f8c2cb23f43e7cb926`.
  Keep its checksum and `docs/milestones/scos-32bit.json`. New 32-bit fixes get
  new filenames; they do not replace the retained milestone.
* `dist/scos.img` is reserved for the future 64-bit releases, not a placeholder
  or relabeled 32-bit image. `build/scos.img` is only a disposable build output.
* Await the user's final 32-bit hardware test. Address any reported defects and
  await their retest. Do NOT begin conversion until the user explicitly says so.
* Reset the round counter to r1 only when the authorized conversion begins.
  Do not reset the current i386 r42 tag during planning. Label future artifacts
  with architecture as well as round, so historical 32-bit r1 is not confused
  with x86-64 r1.
* Convert and validate the existing core/desktop first. New GPU/NIC/Wi-Fi/browser
  driver/library integration requires a SEPARATE instruction from the user after
  conversion is done. Research and host-side QEMU preparation are allowed now.
* Preserve r41's physically validated packet-sized HID reception and report
  assembly. No sensitivity guesses or emulator-only shortcuts.
* Preserve `os.html` as the design reference. Do not restore the old v86 stack
  or temporary diagnostic UI. Host QEMU tooling is separate from the guest OS.
* Run `python3 tools/check_milestone.py` before delivery. Use QEMU snapshots or
  separate overlays. Never attach host physical disks or passed-through devices
  by default. Do not claim a clean build/emulator run proves physical acceptance
  or proves the absence of all bugs.

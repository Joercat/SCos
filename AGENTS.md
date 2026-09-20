# SCos conversion rules

The user authorized the first x86-64 conversion part on 2026-09-19 after reporting
no major remaining 32-bit blockers. Root `make` now builds the unnumbered
x86-64 startup foundation; see `docs/migration/BOOT64.md`.

* Keep the custom SCos kernel and identity. Never replace it with Linux.
* `dist/scos-32bit.img` is now permanently frozen at the verified r43 bytes.
  Preserve its checksum, provenance and integrity guard. Do not overwrite it
  with future builds. Historical revisions remain in Git history.
* `legacy/i386/` contains the old build, bootloader, kernel, desktop and drivers.
  Preserve it as the behavior/porting reference; it is not linked into AMD64.
* Publish x86-64 development images as `dist/scos.img`, with checksum and truthful
  verification notes. No release/round number until conversion starts on the
  user's PC, per the latest instruction. Do not prematurely call this r1.
* Conversion approval is OPEN. Port and validate the existing core first.
  New GPU/NIC/Wi-Fi/browser driver/library integration requires a SEPARATE
  instruction after conversion. Research is not approved integration.
* Preserve r41's physically validated packet-sized HID reception and report
  assembly when porting. No sensitivity guesses or emulator-only shortcuts.
* Preserve `os.html`. Do not restore v86 or temporary diagnostic guest UI.
  Retain legitimate host QEMU tools; retire temporary verification harnesses,
  disposable images and logs after recording results.
* Run `python3 tools/check_milestone.py` before delivery. Use QEMU snapshots;
  never attach host physical disks/devices by default. Verify the actual image
  bytes pushed, not merely the checksum text. Build/emulator success is NOT
  physical-PC acceptance or proof of bug freedom.

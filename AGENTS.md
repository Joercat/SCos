# SCos conversion rules

On 2026-09-20 the user explicitly chose native x64 UEFI only and confirmed the
frozen 32-bit image should remain. The BIOS path is removed. Root `make` builds
an AMD64 PE32+ UEFI application, relocatable ELF64 kernel and GPT/FAT32 image.
See `docs/migration/BOOT64.md` for contracts, verification and limitations.

* Keep the custom SCos kernel and identity. Never replace it with Linux.
* The user clarified that conversion means adapting the original implementation,
  not rewriting the OS from scratch. Preserve reusable app/desktop behavior and
  source; architecture cleanup is not permission to discard unported features.
  Rework only components whose actual dependencies or correctness require it.
  Do not use a smaller line count or a clean compile as evidence of equivalence.
  Keep the user's detailed, thoughtful implementation and verification rule in
  force for every conversion part, including shared app dependencies.
* `dist/scos-32bit.img` is now permanently frozen at the verified r43 bytes.
  Preserve its checksum, provenance and integrity guard. Do not overwrite it
  with future builds. Historical revisions remain in Git history.
* The user requested removal of duplicate i386 source/build files on 2026-09-20.
  `legacy/i386/` and its font generator are removed. Retrieve individual files
  from commit `6717943f977a7e0f95f0ace5fa48cfe6a564f873` when a port is requested;
  do not restore the entire obsolete source tree. Preserve the frozen image.
* Publish x86-64 development images as `dist/scos.img`, with checksum and truthful
  verification notes. No release/round number until conversion starts on the
  user's PC, per the latest instruction. Do not prematurely call this r1.
* No BIOS/CSM, real-mode/protected-mode transitions or 32-bit build target should
  be restored. `i386pep` is GNU ld's AMD64 PE backend name, not a 32-bit mode.
  Keep native-width pointers and spec-sized protocol/register fields distinct.
* The user subsequently authorized apps and their necessary shared desktop
  dependencies. Reuse/adapt original feature source from `6717943`, not simplified
  replacements. `kernel/desktop/` is the active x64 port, not a legacy build.
  The user then authorized integration verification and conversion of the
  remaining ORIGINAL PCI/xHCI/ATA/ACPI drivers; see DRIVER64.md. New hardware
  drivers/libraries remain outside that authorization.
  A bootable test image is NOT a complete OS or physical-PC acceptance.
* The latest clarification authorizes completion of ALL already-written source,
  not only driver files. See CONVERSION-AUDIT.md for the full disposition audit.
  Do not start new drivers until the user verifies behavior and requests them.
* Conversion approval is OPEN, but follow the user's component-by-component scope.
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

* On 2026-09-20 the user accepted the input/compositor fixes and explicitly
  authorized this first open-source integration: independent app registration
  and an adapted upstream Lua compiler/runtime for user-created apps. Lua and
  its necessary freestanding numeric/format support subset are now approved.
  This does not authorize new hardware drivers, networking, or a browser engine.
  Keep upstream provenance/license notices and document port restrictions.

* The user now authorizes an expanded Lua API, custom themes/backgrounds, native
  C App Studio and validated .cat packages replacing directly launchable .lua
  files. All built-in apps must be C. Preserve reusable test tools from now on;
  this supersedes the earlier requirement to delete temporary verification tools.

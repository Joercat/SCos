# SCos — native operating system

**Active startup target: native x64 UEFI. There is no BIOS/CSM or 32-bit build.**
SCos keeps its own kernel and identity; it is not a Linux distribution.
`os.html` remains the desktop design reference.

The startup path now loads a relocatable ELF64 kernel, exits UEFI boot services,
uses the GOP framebuffer, owns its page tables and physical-memory allocator,
and establishes exception handling and real timer interrupts. This is an
implemented firmware-to-kernel path, **not the completed OS conversion**.
The original SCos apps and shared desktop have now been adapted to x64 rather
than replaced: Terminal, Files, Notepad, Calendar, Settings, About, Blackjack,
Solitaire, SysMon and the existing browser-unavailable notice. The compositor,
six TTYs, RAM filesystem, themes and PS/2 input are connected to the native core.
No new release number is assigned.

The remaining original PCI, xHCI USB HID, ATA PIO/persistence and ACPI power
implementations have now been adapted and integration-tested; see
[driver verification and limitations](docs/migration/DRIVER64.md).

**Still unavailable:** AHCI/NVMe/USB mass-storage persistence, networking/real
browser engine and accelerated GPU drivers. Files remain RAM-only unless a
unique supported ATA SCos-data partition is verified; `save` requires confirmation.
ACPI has a manual-power fallback for unsupported firmware. Emulator success is
**not physical acceptance on the user's USB-input PC**. No new release number.

For the first physical boot, use the [PC test guide and final USB-boot checks](docs/migration/PC-TEST.md). It distinguishes verified results from hardware requirements and includes safe flashing/UEFI settings.

## Sign-in, signed boot and encrypted storage

SCos now requires a sign-in before the desktop accepts input. The first boot
creates an administrator account on a full-screen setup screen (username,
policy-checked password, and a one-time recovery code); later boots show a
masked username/password screen. `system/security/users.db` stores only random
128-bit salts and PBKDF2-HMAC-SHA256 verifiers — no plaintext passwords — with
a SHA-256 checksum, a last-known-good backup and a bounded audit ring. Both
shells gained `security`, `passwd`, `users`, `useradd`, `userdel`, `usermod`,
`policy`, `audit`, `lock`, `logout`, `su`, `reauth` and `recovery`. Roles are
administrator, standard user and read-only guest (now an explicit command
allow-list); privileged commands and `system/security` files are protected.
Sessions auto-lock after 10 idle minutes, passwords expire after 90 days by
default, switch user closes all windows, and the maintenance console
re-authenticates — including rescue mode.

**Boot chain:** `BOOTX64.EFI` verifies an RSA-2048/SHA-256 signature over
`\SCOS\BOOT.SIG`, a manifest of `KERNEL.ELF`, `KERNEL.CRC`, `DRVLIST.IDX`,
`BUILD.TXT` and every GPU `.MOD`. It hashes each listed file before loading the
kernel, and refuses to boot if a byte has changed. The public key is embedded in
the loader; the private development key is in `keys/boot-signing.json`. Rotate
it with `python3 tools/scos_keys.py --generate` for a real release (see below).

**Data at rest:** on a verified ATA SCos-data partition the saved filesystem is
an `SCOSVAUL` image — AES-256-CTR ciphertext with an encrypt-then-MAC
HMAC-SHA256 tag. The cleartext header (account DB, audit log, key slots) is the
AEAD's associated data, so editing any of it is detected at unlock. A random
256-bit volume key is wrapped separately under each account's password-derived
KEK; the one-time recovery code has its own slot so recovery does not destroy
data. Signing in is what unlocks the disk; signing out burns the key and
`save` fails closed. Legacy plaintext `SCOSFS2` images are encrypted on first
sign-in.

On a machine without the unique verified ATA SCos-data partition, accounts are
**session-only** and first-run setup repeats at each boot — the secure screen
says so. See [the full security design](docs/migration/SECURITY.md) for the
crypto, formats, key rotation, commands, verification and — importantly — the
honest limitations: SCos is still a cooperative single-address-space kernel
with **no process isolation**, firmware Secure Boot enrolment is a machine-owner
step (not implemented here), the checked-in development signing key is public,
and there is no TPM/rollback protection.

## Write your own apps

All preinstalled desktop apps are **native C**, including **App Studio**.
Counter and Sketch are optional Lua demonstrations in `/home/demos/*.cat`: open
them in Files and approve installation. They are not preinstalled. Run `appstrt studio` to edit a project, syntax-check it with
the real Lua 5.4.9 compiler, build a `.cat` package and launch it with F5.
Studio supports range selection and Ctrl+C/X/V with a 16 KiB clipboard and
64 KiB source limit. Raw `.lua` launching is removed. API 2 offers 56 functions for drawing, widgets,
window control, private data, system queries and confirmed global themes.
Settings selects four presets or up to eight custom palettes/backgrounds.

See the [Studio and API guide](docs/LUA-APPS.md), [CAT format](docs/CAT-FORMAT.md),
[current verification record](docs/migration/APP-MANAGEMENT-VERIFICATION.md), and
[open-source provenance/licenses](third_party/README.md). This remains a
cooperative kernel, not a ring-3 security sandbox. RAM-versus-ATA persistence
limitations still apply. The [earlier Lua verification](docs/migration/LUA-VERIFICATION.md)
is historical, not the current package contract.

## Desktop and windows

The bottom bar has **Launcher**, **Windows** (an open/minimized-window picker),
up to eight width-limited pinned app shortcuts, plus power and clock controls.
Use Applications to launch, pin/unpin or uninstall external apps. Drag with either mouse button
on the desktop to select icons; a stationary right click opens the context menu
on release. Installed CAT apps get a desktop shortcut and a launcher entry.

The compositor reconstructs clipped damage regions from the wallpaper upward,
and GOP scanout uses write-combining rather than uncached pixel writes. This is
still a one-active-CPU kernel, not SMP, and GOP does not provide a hardware-vsync
or page-flip driver. Emulator pixel tests are not a promise of tear-free PC output.

## Build and emulator

On Linux x86-64: GCC, GNU binutils (including the `i386pep` **AMD64 PE32+** linker
emulation), Make, Python 3 and the standard Linux C development headers. Tested
with GCC 12.2.0 and binutils 2.40. Headers provide declarations for the Lua port;
no host libc or Windows runtime is linked. No EFI SDK, FAT utilities or mounted
filesystem is needed.

```sh
make                           # build/BOOTX64.EFI, kernel.elf, BOOT.SIG and scos.img
python3 tools/check_milestone.py
python3 tools/scos_keys.py --verify      # trust anchor matches the signing key
python3 tools/tests/test_signed_boot.py  # RSA KATs + tampered-image boot refusals
python3 tools/tests/test_security.py     # crypto KATs + encrypted-vault guest tests
sha256sum -c dist/scos.img.sha256
python3 tools/setup_qemu.py     # optional pinned host QEMU environment
python3 tools/run_qemu.py --xhci # x64 EDK2/q35, native USB HID, disposable disk state
make clean
```

The packaged `dist/scos.img` is a 64-MiB GPT disk with a FAT32 EFI system partition:
`EFI/BOOT/BOOTX64.EFI`, `SCOS/KERNEL.ELF`, `SCOS/KERNEL.CRC`, `SCOS/DRVLIST.IDX`,
`SCOS/BUILD.TXT`, the GPU `.MOD` files and `SCOS/BOOT.SIG`. It is a development
integration artifact, not a request to flash or physically test the PC now.
It also contains a separate 128-KiB SCos-data partition. Only the verified ATA
path can save there, and the saved image is encrypted; the default q35/AHCI
guest cannot persist. Neither a USB boot medium nor the xHCI HID driver implies
USB mass-storage support.

**Before a real deployment:** `python3 tools/scos_keys.py --generate`, keep
`keys/boot-signing.json` private, rebuild (`make -j4 && make gpu-modules &&
make dist`), and enrol the resulting `BOOTX64.EFI` in firmware Secure Boot. The
checked-in development key is public by design so that anyone can build bootable
images; its signature proves integrity, not publisher identity.

A later physical test must use **x64 UEFI boot, not the previous CSM configuration**.
The shipped development image has no firmware Secure Boot enrolment, so Secure
Boot must be disabled unless you rotated the key and enrolled your own loader as
described above. The firmware must provide GOP direct framebuffer access and a
readable boot filesystem. Writing a raw image to an entire USB device destroys
existing contents; identify/back up the device first. Successful emulator USB
boot does not establish motherboard/GPU acceptance.

## Implementation details and evidence

[Existing-source completion audit](docs/migration/CONVERSION-AUDIT.md) accounts
for every original kernel C file and records the restored startup presentation,
native fault diagnostics and interrupt-rate protection. No new drivers were added.

[App/desktop port provenance, fixes and verification](docs/migration/APP64.md)
documents the current integration.

[Native UEFI startup contract, limits and verification](docs/migration/BOOT64.md)
covers the loader, relocations, firmware exit/retry rules, memory ownership,
GOP console, error paths, allocator, interrupts and exact testing scope.

* [Conversion roadmap](docs/migration/README.md) — the user chooses the next part.
* [Host emulator provenance and usage](docs/migration/EMULATOR.md).
* [Research only: GPU, networking and browser options](docs/migration/HARDWARE-AND-BROWSER.md).

No new GPU/NIC/browser stack was imported. The GOP console uses firmware-provided
pixels, not a native GPU acceleration driver. The existing architecture-independent
SCos bitmap font and original desktop drawing primitives are reused. No BIOS,
VBE startup or obsolete i386 allocator was brought back.

## Frozen historical milestone

Per the user's explicit choice, `dist/scos-32bit.img`, its checksum and
[provenance](docs/milestones/scos-32bit.json) remain permanently frozen at r43.
They are an archive, **not a build target or dependency of the UEFI loader**.
The integrity guard protects that retention. Previous implementations remain
in Git history; duplicate legacy source/build trees have been removed.
See [historical r43 behavior and limitations](docs/RELEASE-r43.md).

## Application management and graphics status

There is no application-manager window: each function is reached from the surface it acts on. Right-click
a desktop icon for that icon's own menu (open, pin or unpin, hide, and for a `.cat` app, uninstall),
right-click empty wallpaper to restore hidden icons or open Files or Terminal, right-click a taskbar
entry to unpin it or put its icon back, and right-click a row in Files to open it with another
application, rename it, pin it to the desktop or be sent to `rm -s`. Pressing an entry in the launcher and
moving drags that application onto the desktop (installing its shortcut) or onto the taskbar (pinning
it); dragging a row out of Files drops the file onto a window (open it there), a taskbar entry (open it
with that application) or the desktop (a shortcut). `appuninstall <id>` requests
default-no confirmation in Terminal or TTY. Built-ins cannot be uninstalled;
external removal closes instances and removes the registered installed package,
shortcuts and pins, but keeps projects, app data and original external sources.
RAM-only installations are explicitly session-only and disappear at reboot.
Only existing supported ATA persistence is available; no new storage drivers.

Top-right notifications overlay windows, menus and modals. Click to dismiss;
normal notices expire after ten seconds. Lua apps can use `scos.notify(text)`.

`graphics` reports read-only PCI display-device discovery and the CPU renderer.
**GPU acceleration and working hardware-driver selection remain unimplemented.**
The boot warning explicitly reports CPU fallback. GOP/PAT/write combining are
not GPU rendering. Browser integration remains deferred.

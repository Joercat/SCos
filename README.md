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

## Write your own apps

All preinstalled desktop apps are **native C**, including **App Studio**.
Counter and Sketch are optional Lua demonstrations in `/home/demos/*.cat`: open
them in Files and approve installation. They are not preinstalled. Run `appstrt studio` to edit a project, syntax-check it with
the real Lua 5.4.9 compiler, build a `.cat` package and launch it with F5.
Studio supports range selection and Ctrl+C/X/V with a 16 KiB clipboard and
64 KiB source limit. Raw `.lua` launching is removed. API 2 offers 48 functions for drawing, widgets,
window control, private data, system queries and confirmed global themes.
Settings selects four presets or up to eight custom palettes/backgrounds.

See the [Studio and API guide](docs/LUA-APPS.md), [CAT format](docs/CAT-FORMAT.md),
[current verification record](docs/migration/STUDIO-CLIPBOARD-VERIFICATION.md), and
[open-source provenance/licenses](third_party/README.md). This remains a
cooperative kernel, not a ring-3 security sandbox. RAM-versus-ATA persistence
limitations still apply. The [earlier Lua verification](docs/migration/LUA-VERIFICATION.md)
is historical, not the current package contract.

## Build and emulator

On Linux x86-64: GCC, GNU binutils (including the `i386pep` **AMD64 PE32+** linker
emulation), Make, Python 3 and the standard Linux C development headers. Tested
with GCC 12.2.0 and binutils 2.40. Headers provide declarations for the Lua port;
no host libc or Windows runtime is linked. No EFI SDK, FAT utilities or mounted
filesystem is needed.

```sh
make                           # build/BOOTX64.EFI, kernel.elf and scos.img
python3 tools/check_milestone.py
sha256sum -c dist/scos.img.sha256
python3 tools/setup_qemu.py     # optional pinned host QEMU environment
python3 tools/run_qemu.py --xhci # x64 EDK2/q35, native USB HID, disposable disk state
make clean
```

The packaged `dist/scos.img` is a 64-MiB GPT disk with a FAT32 EFI system partition:
`EFI/BOOT/BOOTX64.EFI`, `SCOS/KERNEL.ELF` and `SCOS/KERNEL.CRC`. It is a development
integration artifact, not a request to flash or physically test the PC now.
It also contains a separate 128-KiB SCos-data partition. Only the verified ATA
path can save there; the default q35/AHCI guest cannot persist. Neither a USB
boot medium nor the xHCI HID driver implies USB mass-storage support.

A later physical test must use **x64 UEFI boot, not the previous CSM configuration**.
Secure Boot must be disabled for this unsigned image. The firmware must provide
GOP direct framebuffer access and a readable boot filesystem. Writing a raw image
to an entire USB device destroys existing contents; identify/back up the device
first. Successful emulator USB boot does not establish motherboard/GPU acceptance.

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

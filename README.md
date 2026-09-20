# SCos — native operating system

**Active startup target: native x64 UEFI. There is no BIOS/CSM or 32-bit build.**
SCos keeps its own kernel and identity; it is not a Linux distribution.
`os.html` remains the desktop design reference.

The startup path now loads a relocatable ELF64 kernel, exits UEFI boot services,
uses the GOP framebuffer, owns its page tables and physical-memory allocator,
and establishes exception handling and real timer interrupts. This is an
implemented firmware-to-kernel path, **not the completed desktop OS**. Input,
applications, storage persistence, power management and the desktop still await
their separate conversion steps. No new release number is assigned.

## Build and emulator

On Linux x86-64: GCC, GNU binutils (including the `i386pep` **AMD64 PE32+** linker
emulation), Make and Python 3. Tested with GCC 12.2.0 and binutils 2.40. No EFI SDK,
Windows runtime, host libc, FAT utilities or mounted filesystem is needed.

```sh
make                           # build/BOOTX64.EFI, kernel.elf and scos.img
python3 tools/check_milestone.py
sha256sum -c dist/scos.img.sha256
python3 tools/setup_qemu.py     # optional pinned host QEMU environment
python3 tools/run_qemu.py       # x64 EDK2, q35, disposable disk/variable state
make clean
```

The packaged `dist/scos.img` is a 64-MiB GPT disk with a FAT32 EFI system partition:
`EFI/BOOT/BOOTX64.EFI`, `SCOS/KERNEL.ELF` and `SCOS/KERNEL.CRC`. It is a development
integration artifact, not a request to flash or physically test the PC now.
The kernel does not write to it after firmware exit.

A later physical test must use **x64 UEFI boot, not the previous CSM configuration**.
Secure Boot must be disabled for this unsigned image. The firmware must provide
GOP direct framebuffer access and a readable boot filesystem. Writing a raw image
to an entire USB device destroys existing contents; identify/back up the device
first. Successful emulator USB boot does not establish motherboard/GPU acceptance.

## Implementation details and evidence

[Native UEFI startup contract, limits and verification](docs/migration/BOOT64.md)
covers the loader, relocations, firmware exit/retry rules, memory ownership,
GOP console, error paths, allocator, interrupts and exact testing scope.

* [Conversion roadmap](docs/migration/README.md) — the user chooses the next part.
* [Host emulator provenance and usage](docs/migration/EMULATOR.md).
* [Research only: GPU, networking and browser options](docs/migration/HARDWARE-AND-BROWSER.md).

No new GPU/NIC/browser stack was imported. The GOP console uses firmware-provided
pixels, not a native GPU acceleration driver. The existing architecture-independent
SCos bitmap font was retained for this console; the old desktop was not restored.

## Frozen historical milestone

Per the user's explicit choice, `dist/scos-32bit.img`, its checksum and
[provenance](docs/milestones/scos-32bit.json) remain permanently frozen at r43.
They are an archive, **not a build target or dependency of the UEFI loader**.
The integrity guard protects that retention. Previous implementations remain
in Git history; duplicate legacy source/build trees have been removed.
See [historical r43 behavior and limitations](docs/RELEASE-r43.md).

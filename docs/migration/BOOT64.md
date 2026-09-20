# Native x64 UEFI startup — implementation and verification

2026-09-20. The user explicitly selected **UEFI only** and separately confirmed
that the final frozen 32-bit image should remain. The previous BIOS startup is
removed, not retained as a fallback. There is no release number yet.

## Scope and outcome

This implements the complete startup sequence described below, from the native
UEFI application through kernel-owned memory/interrupts and idle. It is not a
promise of support for every firmware configuration and **not the full SCos OS**.
Desktop, applications, input, persistent storage, ACPI power control and new
third-party drivers remain separate work. The user selects the next conversion
part. No Linux kernel or external guest bootloader/runtime was substituted.

Removed: MBR/stage1/stage2 executable loaders, real/protected-mode switching,
BIOS interrupts, A20 handling, EDD/CHS reads, E820 handoff, fixed low-memory
staging, VGA text-memory output and the old 16-MiB allocation pool.
The frozen historical image and historical notes remain, but are never executed
or linked by this build. Protocol fields and pixel words legitimately remain
32 bits where their specifications require it; that is not 32-bit CPU execution.

## Build and disk layout

`make` compiles all executable SCos code for AMD64:

* `boot/uefi/main.c` and `efi.h`: a native PE32+ EFI application. EFI entry and
  protocol calls use the Microsoft x64 ABI; internal/kernel calls use SysV.
  ABI offsets are checked at compilation. The declarations cover only protocols
  consumed by the loader, not a replacement firmware library.
* `kernel.elf`: position-independent ELF64, ET_DYN, with separate RX/R/RW load
  segments and only internal `R_X86_64_RELATIVE` relocations. No interpreter,
  shared libraries, symbol resolver, dynamic application loader or host libc.
* Both use general-register-only code and no red zone, stack-protector runtime
  or implicit compiler library calls. No SIMD context ownership is claimed.
* GNU ld's `i386pep` name denotes its **64-bit PE** backend, not i386 code. A real
  DIR64 relocation anchor keeps the EFI image loadable away from its preferred
  base. `-fno-ident` prevents an ELF `.comment` orphan at RVA zero from producing
  an invalid PE section. PE timestamps are fixed for reproducible packaging.
* `tools/makedisk.py` emits a deterministic 64-MiB disk: protective MBR metadata,
  primary and backup GPT headers/entry arrays, an EFI system partition beginning
  at LBA 2048, and FAT32 with mirrored FATs, backup BPB and FSInfo. There is no
  executable BIOS loader in the protective MBR. FAT BPB jump bytes are filesystem
  format metadata, not an alternative OS boot path.
* The ESP contains `EFI/BOOT/BOOTX64.EFI`, `SCOS/KERNEL.ELF` and a four-byte
  little-endian CRC32 in `SCOS/KERNEL.CRC`. Firmware handles filesystem/device
  access before exit; this does not port a filesystem or USB storage driver into
  the kernel. Files are opened read-only on the loaded application's own volume.

The host packager checks PE architecture/subsystem, header and section bounds,
entry, permissions, relocation blocks and absence of real Windows imports. It
also checks ELF architecture/type, bounded program headers, load spans, page
separation, permissions and entry. The loader independently validates the runtime
ELF and its relocation contract. Image construction uses no host disk device,
mount, partitioning utility or root privileges; validated output is replaced
atomically. CRC is accidental-corruption detection, **not authentication**.

Tested toolchain: host GCC 12.2.0 / GNU binutils 2.40. A dedicated cross-toolchain
is not claimed. The host QEMU bundle already includes x64 EDK2; no additional
firmware or driver was imported into the guest. See [EMULATOR.md](EMULATOR.md).

## Firmware phase: explicit ownership and failure handling

1. Accept the native EFI image handle/system table and initialize bounded COM1
   output. Before exit, messages also go through the firmware text console.
   EDK2 may mirror console output to serial, producing duplicate early lines.
2. Require NX, TSC, MSR and PAT. The current paging implementation expects
   four-level firmware paging with PCID off; unsupported LA57/PCID is rejected
   before exit. Firmware already entered 64-bit execution—there is no SCos
   real-mode or protected-mode trampoline.
3. Disable the firmware watchdog. Resolve the loaded-image protocol, then the
   simple-filesystem protocol on **that image's device handle**. Never scan
   unrelated volumes to find a similarly named kernel.
4. Read the ELF and CRC with bounded size checks and partial-read handling.
   Close file/volume handles; detect corruption before allocation/execution.
5. Validate ELF64 ET_DYN, machine/version/header/program sizes and entry offset;
   bound every load range and file span, reject page-overlapping permissions,
   writable executable segments, interpreter/TLS segments and dependencies.
6. Accept only bounded, aligned RELA records of type `R_X86_64_RELATIVE` with
   symbol zero, writable in-image destinations and in-image addends. Reject
   REL, PLT, RELR and text relocation requirements. Request LoaderCode pages
   **from firmware**, zero the entire span, copy file-backed segments and apply
   the checked relocations. A fixed physical placement is not assumed.
7. Find GOP on the active output handle when available, otherwise locate a GOP
   instance. Query/free mode descriptions and select a supported readable mode,
   preferring pixel count near 1024×768. Validate actual mode, pitch, direct
   framebuffer bounds and disjoint RGB/BGR/contiguous channel bitmasks. BLT-only
   output is rejected. This is not a multi-GPU selection or acceleration driver.
8. Allocate a 16-MiB LoaderData arena below 4 GiB for the handoff, final map and
   kernel page-table construction. The kernel allocation is also below 4 GiB;
   the resulting physical allocator can use conventional RAM above 4 GiB.
9. Preserve the ACPI 2.0 RSDP candidate. Calibrate a startup deadline from TSC
   around a real 10-ms firmware Stall call; no guessed CPU frequency is used.
   This clock is for startup timeout detection, not a production clock subsystem.

Before any ExitBootServices attempt, a failure closes handles, frees tracked file
buffers/kernel/arena allocations and reports both the failing phase and EFI status
through the firmware console/serial before returning failure. The changed GOP mode
is not restored. There is no silent disk fallback or fabricated successful stage.

## ExitBootServices: one-way transition

The final memory-map buffer is already allocated in the arena. GetMemoryMap
provides the current key, descriptor stride, descriptor version and exact byte
count. Supported descriptors are version 1, at least 40 bytes; larger strides
are preserved, not assumed to equal a C struct size. Bounds: 512 descriptors,
128-KiB buffer, strides up to 4096 bytes.

There are **no protocol calls, printing through firmware, allocations or file
closures between GetMemoryMap and ExitBootServices**. On EFI_INVALID_PARAMETER,
obtain a fresh map/key and retry, at most eight attempts. After the first exit
attempt, never return to normal firmware cleanup: firmware may already be partly
shut down. Other errors, oversized maps or exhausted retries halt with serial
and direct GOP bitmap error output; no working input/UART is required for a
visible failure when GOP was successfully selected.

After successful exit: CLI, mask NMI/PIC across the transition, enable NXE, and
call the relocated kernel entry with RDI pointing to the handoff. No firmware
boot/runtime protocol is called afterward. Runtime descriptors are reserved;
SetVirtualAddressMap and runtime services are not implemented.

## Version-2 handoff

Native little-endian, 136 bytes, at the page-aligned allocated arena base:

| Offset | Type | Field |
| --- | --- | --- |
| 0 | u64 | Magic `0x3436544f4f424353` |
| 8 / 12 | u32/u32 | Version 2 / size 136 |
| 16 / 24 / 32 | u64 | Map address / bytes / firmware descriptor stride |
| 40 / 44 | u32/u32 | Descriptor version 1 / reserved zero |
| 48 / 56 | u64 | Allocated kernel start / page-rounded end |
| 64 / 72 | u64 | Arena start / size |
| 80 | u64 | ACPI 2.0 RSDP physical candidate or zero |
| 88 / 96 | u64 | Framebuffer base / size |
| 104 / 108 / 112 | u32 | Width / height / pixels per scanline |
| 116 / 120 / 124 | u32 | Red / green / blue masks |
| 128 | u64 | Measured startup TSC ticks per second |

The final map starts one page into the arena. The first 256 KiB are reserved for
handoff/map growth; page-table allocations begin afterward. Kernel entry preserves
RDI while clearing BSS, establishes its own aligned guarded stack, and calls C.

Native GDT/TSS/IDT and fault stacks are installed **before dereferencing the
handoff**. The kernel independently validates its version, spans, stride/count,
firmware allocation types, descriptor ranges/alignment/overflow and nonoverlapping
ownership. GOP must not overlap firmware-owned RAM; this check occurs before any
framebuffer clear/write. An ACPI RSDP is accepted only in reserved ACPI memory,
with bounded length/signature and both checksums; invalid candidates are warned
about and discarded. Firmware tables are not reclaimed or assumed mapped later.

## Kernel-owned virtual and physical memory

The old guessed low-memory pool is gone. A new four-level table tree is built
using the explicitly owned arena while firmware mappings are still active:

* Only conventional RAM advertising write-back support is accepted. Runtime,
  write/read-protected, read-only, nonvolatile and special-purpose ranges are
  excluded rather than erased or remapped with incompatible cache attributes.
  Accepted conventional RAM is identity mapped RW-NX. Use 2-MiB
  leaves for aligned spans and 4-KiB leaves at boundaries. Physical addresses are
  retained as 64-bit values; the implemented identity-map policy caps addresses
  below 64 TiB. Firmware-reserved/runtime/ACPI/other loader regions remain reserved.
* Kernel text is RX, constants R-NX, relocation-read-only tables R-NX after loading,
  mutable data/stacks RW-NX. Null and each main/DF/NMI/MC stack guard are absent.
* The arena remains reserved RW-NX. GOP alone is mapped UC, RW-NX. The kernel
  normalizes PAT with cache-disable/flush sequencing and discards inherited global
  translations before switching CR3. CR0.WP remains enabled.
* Conventional ranges must not intersect kernel/arena/framebuffer ownership.
  Conflicting mappings, arena exhaustion and invalid ownership are fatal.

The physical allocator maintains sorted free extents, not a fixed bitmap covering
only low RAM. Allocate consumes one page and zeroes it; release validates original
conventional ownership, rejects a double free and coalesces adjacent extents.
Accounting is real. Startup verifies allocate/zero/write/read/release transitions.
The allocator does not touch every free page at boot or pretend reserved memory
is usable. BootServices memory and obsolete loader buffers are deliberately not
reclaimed into the pool yet, avoiding lifetime mistakes.

Limits: single CPU; IRQ-disabled ownership changes; no allocation from NMI/fatal
contexts; maximum 1024 free extents; checked 16-MiB table arena. Extreme physical
maps or fragmentation fail explicitly instead of corrupting metadata. There is
no general heap, process VM, DMA mapping API, scheduler or SMP synchronization.
Identity mappings are supervisor-only; user-mode execution is not enabled.

## Interrupts, console and readiness

256 native 16-byte IDT gates, explicit null LDTR, 64-bit TSS, guarded 64-KiB main
stack and independent 16-KiB DF/NMI/MC IST stacks. Interrupt assembly normalizes
hardware error slots, saves 15 GPRs, establishes DF-clear/SysV alignment for C,
restores the exact interrupted stack and returns with IRETQ. All guest code stays
in long mode. The ELF ISR pointer table is relocated, then made read-only.

PIC/PIT are x86 peripherals, **not a 32-bit execution mode**. Only IRQ0 is enabled
initially. Startup must receive three actual ticks; a measured-TSC deadline makes
missing IRQ routing a visible panic rather than an endless HLT wait. APIC/SMP and
platforms without compatible PIC/PIT routing are not implemented or claimed.

The console writes native GOP pixels using the existing architecture-independent
SCos 8×16 font, with RGB/BGR/bitmask encoding, bounded dimensions, pitch-aware
scrolling and emergency cursor bounds. Serial waits are bounded. Faults report
vector/error, RIP/RSP/flags, CR2/CR3 and selected registers without dereferencing
faulting instruction/stack pointers, then halt. This is not the old six-TTY or
framebuffer window-manager implementation.

## Verification performed on this implementation

QEMU 11.0.2 TCG + bundled **x64 EDK2**, one CPU, snapshot disks, copied disposable
firmware variables, no guest network or host device passthrough:

* Native UEFI boot, dynamically different kernel placements, own mappings and real
  timer readiness at **128, 256 and 2048 MiB**.
* 2048-MiB guest with only 1 GiB below the PCI hole: map/allocate/write/release a
  page above 4 GiB. For the allocation check the external debugger withheld low
  free extents; no production test hook or fake high-memory address was used.
* UEFI USB-storage boot, including the 64-MiB image on a larger 256-MiB emulated
  USB medium. This is firmware USB access, not a converted kernel USB driver.
* Missing NX, missing GOP, missing kernel, corrupt file, wrong ELF type and invalid
  relocation rejected. ELF/relocation mutations had a recomputed CRC, proving
  rejection was by structural validation rather than only checksum mismatch.
* Actual ExitBootServices stale-key recovery and eight-attempt exhaustion tested
  with temporary loader variants that supplied invalid keys. Normal image has no
  fault-injection switch. A temporary masked-IRQ variant proved the timer deadline
  reaches visible panic. Temporary variants are not the published image.
* All 15 GPRs, RSP and interrupted DF preserved across real PIT interrupts.
* Invalid opcode, null read, text write, stack guard, NX execution, double fault
  and NMI (with bad interrupted stack/cursor) reached the expected native handlers.
* 128 unique page allocations; fragmented release/coalescing; exact accounting;
  zero-on-reuse and double-free rejection. Mutated handoff version and framebuffer
  overlap rejected before framebuffer writes.
* Host memory-map fixtures under ASan/UBSan: overlap, misalignment, overflow,
  runtime/reserved exclusion, bounds and high-address usable ranges.
* Host PE architecture/class/subsystem/bounds rejection; primary/backup GPT CRCs,
  partition arrays, mirrored FATs and backup BPB checked. Clean rebuild reproduced
  exact disk bytes. No unresolved kernel imports or RWX ELF LOAD segment.
* GOP screenshot inspected: native text and startup state visible. Production
  image unchanged by guest tests; frozen r43 integrity guard passed.

A **64-MiB** EDK2 guest ran out of resources allocating the 16-MiB arena; it reported
that phase and EFI_OUT_OF_RESOURCES and returned failure without entering the
kernel. It is not listed as a successful boot. The launcher requires at least
128 MiB. Earlier BIOS test results in historical notes do not apply to this path.

Two concrete integration failures were corrected during implementation: an ELF
`.comment` orphan made the first PE invalid to EDK2; and requesting a fixed kernel
address failed because firmware owned that range. The final build suppresses that
orphan, validates PE section RVAs and dynamically allocates/relocates the kernel.

Temporary harnesses, mutated images and screenshots are retired after recording
results. Legitimate build/packaging/QEMU tools remain. No physical i5-11400,
motherboard, USB controller or dedicated-GPU boot has been verified. Secure Boot
is not implemented; the image is unsigned. Firmware/GOP requirements, bounded
resources and platform IRQ requirements are real limitations, not hidden behind
“fully working OS” language.

## Published development artifact

`dist/scos.img`: 67,108,864 bytes; SHA256:

```text
7e67eb2d61a8a017eb338776c4a3ad85f3aa4ca58de2a013f776cc93be22007d
```

This is the unnumbered UEFI startup artifact, not a desktop release. The frozen
32-bit artifact remains unchanged. Additional host ASan/UBSan fixtures exercised
the actual loader's valid PIE relocation, malformed type/load permissions/spans,
CRC test vector and valid/invalid GOP bitmasks. Direct GOP failure output after
exhausted firmware-exit retries was also captured and visually checked.

Final memory-attribute hardening was additionally checked under ASan/UBSan for
WB support and all excluded attribute bits, then native boot/IRQ preservation
(128/256 MiB), high-page allocation (2048 MiB), USB boot and UD/RO/NX/DF dispatch
were rerun on those final bytes. A clean rebuild matched the published checksum.

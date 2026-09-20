# First AMD64 startup foundation — unnumbered

Implemented and emulator-verified on 2026-09-19 after explicit user permission.
This is a staged conversion of SCos, not Linux, and not the completed desktop.
No release number is assigned until conversion starts on the user's PC.

## Artifact and source layout

* `dist/scos.img`: 8,388,608-byte raw BIOS development disk.
* SHA256: `838c55b1dbe98c25cb3ab3dc76760253d56feadd184491858a4a6ff75fde4615`.
* Root `make` builds the same bytes into `build/scos.img`. GCC 12.2.0,
  binutils 2.40; freestanding AMD64, no host libraries, PIE, red zone, implicit
  SIMD/FPU use or stack protector runtime. Linker uses ELF64 AMD64 with separate
  RX/R/RW load segments, no unresolved symbols, entry at `0x100000`.
* `boot/`: BIOS MBR and long-mode bootstrap. The transitional 16/32-bit assembly
  is required for BIOS boot, not an accidentally retained i386 kernel.
* `kernel/x86_64/`: native entry, guarded stacks and linker layout.
  `kernel/src/`: validated handoff, console, memory and interrupt/platform setup.
* `tools/gen_vectors.py` generates production interrupt entry assembly;
  `tools/makedisk.py` validates and packages the ELF64-derived payload.
* `legacy/i386/`: relocated previous bootloader, kernel, desktop, drivers and
  build rules. `make legacy` reproduces the frozen r43 image byte-for-byte.
* `dist/scos-32bit.img` and its checksum stay permanently unchanged. `os.html`
  stays as the original design reference. No new third-party guest library or
  driver has been imported.

## Startup and failure behavior

1. MBR loads stage2 from the BIOS-supplied disk number. Stage1 may retry with
   CHS on that same disk; stage2 requires EDD. Neither searches unrelated disks.
2. Select VGA text mode and bounded COM1 output; reject missing CPUID, MSR,
   PAE, long-mode or NX capability before enabling unsupported CPU features.
3. Obtain a bounded E820 map; check enabled ranges for overflow. Require staging,
   page-table and complete kernel/BSS spans to fit usable RAM and overlap no
   enabled reserved/unknown range. No guessed contiguous-memory fallback.
4. Verify A20; use the fast gate, then bounded keyboard-controller fallback if
   needed. Read the kernel one sector at a time, avoiding BIOS DMA boundaries.
5. Verify CRC32 of the padded payload before copying/executing it. This detects
   accidental corruption, **not malicious tampering/authenticity**. Bootstrap
   instructions themselves are not covered by a cryptographic trust chain.
6. Set up temporary identity paging, CR0.WP, EFER.LME/NXE and enter long mode.
   Clear BSS, switch to aligned native stack and call the AMD64 C kernel.
7. Install native GDT/TSS and all 256 IDT gates, with dedicated guarded IST
   stacks for double fault, NMI and machine check. Restore NMI after this setup.
8. Validate/copy the handoff and E820 records, verify active mode/protection
   registers, and replace temporary paging with sparse kernel-owned mappings.
9. Initialize the early page pool, verify allocation/write/release accounting,
   remap/mask the PIC and start PIT IRQ0. Readiness requires three actual timer
   interrupts through the native entry/return path. Idle with HLT and IRQs on.

Loader failures report and halt. Kernel exceptions report vector, error, RIP,
RSP, flags, CR2/CR3 and selected GPRs on VGA/serial, then halt without input.
The new kernel has no disk-write driver and does not inspect saved user data.
The `SCOSBOOT64v1` disk marker differs from the old persistence ownership marker.
There was no dynamic module loader in i386 to widen: core platform initialization
now has explicit static dependency order. No unsupported dynamic loader is claimed.

## Boot ABI version 1

Little-endian 80-byte structure at physical `0x7000`, pointer in RDI:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u64 | Magic `0x3436544f4f424353` |
| 8 / 12 | u32 / u32 | Version 1 / size 80 |
| 16 | u64 | E820 address `0x5000` |
| 24 / 28 | u32 / u32 | Count 1–64 / stride 24 |
| 32 / 40 / 48 | u64 | Kernel start, memory end, padded file end |
| 56 | u64 | Bootstrap CR3 `0x10000` |
| 64 / 68 | u32 / u32 | BIOS disk number / flags 1 |
| 72 | u64 | Reserved, must be zero |

E820 records: u64 base, u64 length, u32 type, u32 attributes. Disabled and
zero-length entries are ignored; reserved/unknown overlaps take precedence over
usable entries regardless of order. A requested contiguous span must fit one
usable record; adjacent records are deliberately not guessed/coalesced.

Fixed BIOS workspace: map `0x5000..0x55ff`; handoff `0x7000..0x704f`; stack below
`0x6f00`; stage2 `0x8000..0x9fff`; paging `0x10000..0x15fff`; payload staging
`0x20000..0x7ffff` (384 KiB cap); kernel at 1 MiB. Kernel file span is currently
12,288 bytes and complete memory end is `0x132000`. Larger ports must deliberately
revise the loader/linker policy rather than silently overflow these bounds.

The temporary map covers 4 GiB with 2 MiB leaves (NX outside the first 2 MiB).
Final 4 KiB mappings cover only the kernel, validated usable page-pool RAM below
16 MiB and uncached VGA memory. Null, stack guards and other low firmware holes
are unmapped. Text is RX, rodata R-NX, data/stacks/pool RW-NX with write protection.
The pool zeroes allocations, tracks ownership and rejects invalid/double releases.
E820 addresses above 4 GiB are retained without truncation but **not allocated**.
This is an early physical page pool, not a full heap or virtual-memory subsystem.

Native interrupt frame: r15 through r8, rdi/rsi/rbp/rdx/rcx/rbx/rax, vector,
error, RIP/CS/RFLAGS/RSP/SS. Vector offset 120, total 176 bytes. Stubs save all
15 GPRs, clear DF for C, align the call stack and use IRETQ. Hardware-error-code
vectors are handled separately; the interrupted flags are restored on return.

## Verification performed

Tests used QEMU 11.0.2 TCG, one CPU, snapshot disks and private QMP/GDB sockets;
no physical disks, USB passthrough or guest network. Faults and boot mutations
were injected externally into disposable guests, not via production test hooks.

| Check | Observed result |
| --- | --- |
| Real boot at 64 and 256 MiB | Long mode, mappings, PIT IRQ return and readiness passed |
| High physical-address E820 | 2048 MiB guest, `pc,max-ram-below-4g=1G`: usable RAM above 4 GiB reported correctly; not allocated |
| CPU state inspection | CS64, EFER `0xd00`, CR0 `0x80010011`, CR4 `0x20`, kernel CR3 `0x131000`, TSS64, IDT limit `0xfff`, IF enabled |
| Unsupported CPU | `qemu32`, `max,-nx`, `max,-pae`, and `486` rejected with visible CPU error |
| Damaged payload / unsafe kernel end | CRC mismatch / invalid boot range rejected before kernel execution |
| Truncated boot disk with another valid disk attached | Failed on original disk; did not boot the unrelated disk |
| Real PIT ABI | All 15 GPRs, RSP and interrupted DF survived repeated IRQs |
| Injected UD, null access, text write, stack-guard access, NX execution, invalid GDT selector | Correct exception vector/error and register reporting; halted |
| Invalid interrupted stack / double fault | Double-fault IST handled the fault rather than resetting |
| NMI with invalid interrupted stack | Dedicated NMI IST reached reporting/halt |
| Mutated handoff version at kernel entry | Rejected incompatible handoff |
| Host E820 validator fixtures (ASan/UBSan) | Reserved/unknown overlap, disabled/empty entries, overflow, boundaries, high addresses and conservative adjacent-range policy passed |
| Packager rejection | Wrong ELF class, empty/oversize kernel, bad MBR signature/label area and oversize stage2 rejected |
| Clean rebuild / packaging | Identical image bytes; no unresolved kernel symbols; no RWX ELF LOAD segment |
| Frozen i386 rebuild | Exact r43 SHA256 `43c2bce097389179aff9965779148c82a4907017c392f996a6602f727efb0c97` |
| Test disk safety | Production image unchanged by guest runs |

A 5120 MiB QEMU attempt failed on the **host** with `cannot set up guest memory
'pc.ram': Out of memory`, before guest execution. The 2048 MiB PCI-hole layout
was used to test map records above 4 GiB instead; this is not a claim of successful
5 GiB boot or high-RAM allocation. Temporary test harnesses, mutated images,
screenshots and logs were retired after recording results. Production generators,
build tools and the legitimate host emulator/bootstrap remain.

## Deliberate limits and next work

This image needs legacy BIOS/CSM, EDD and VGA text support; no UEFI-only boot or
GOP framebuffer is implemented. A dedicated GPU is not initialized by a native
driver. Actual i5-11400/motherboard/USB boot remains unverified. Emulator results
are evidence for tested paths, not physical acceptance or exhaustive correctness.

No keyboard/mouse, USB, ATA persistence, ACPI power/reset, scheduler, userspace,
SMP/APIC, dynamic driver/module loading, desktop/TTY applications, framebuffer
compositor, network or browser is present in this first foundation. It ends at
`Foundation ready. Desktop/device ports are not enabled yet.` and is intentionally
not interactive. Do not mistake this startup checkpoint for the full conversion.

Next conversion work is native allocation/mapping and device/DMA interfaces,
then careful ports of existing console/input/display/storage and SCos behavior.
Preserve physically validated HID report assembly and packet boundaries; do not
mechanically widen hardware register fields or cast virtual pointers into DMA
addresses. Existing desktop behavior is retained in `legacy/i386/` for that work.
New GPU/network/browser resources still need separate permission after conversion.

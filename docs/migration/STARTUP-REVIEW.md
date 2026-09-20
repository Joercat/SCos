# Historical BIOS-startup review and cleanup — 2026-09-20

**Superseded later the same day by the explicitly authorized native UEFI rewrite.**
This document records the earlier revision, not the current implementation or
build instructions. See [BOOT64.md](BOOT64.md) for the active architecture.

## What “boots” means here

**SCos is not fully converted and the 64-bit OS is not yet usable as a desktop.**
The current checkpoint transfers control from BIOS into a small native kernel,
validates its memory contract, establishes protected mappings and fault handling,
receives real timer interrupts and idles. It does not run any of the old apps.
A raw image is needed to test that integration in an emulator; producing one
is not evidence that the entire OS works, nor a request to flash a physical PC.
No new round number is assigned. The next conversion part awaits the user.

The objective of this pass is to make **these existing startup components** more
reliable and explain their contracts, not to pad them with unrelated features or
start the input, graphics, storage, desktop or driver-library ports.

## Duplicate source removal and preservation

Removed `legacy/i386/` in full: the obsolete MBR/stage2, i386 entry/linker/header,
old kernel/device/application implementation, old Makefile and old disk packager.
Removed the unused legacy font generator and root `make legacy` target. Ignored
legacy build leftovers were removed as well. No dormant 32-bit OS is compiled
or retained as a second source tree.

All of those files remain recoverable at commit
`6717943f977a7e0f95f0ace5fa48cfe6a564f873`. For a requested future port, inspect
only the needed file without restoring the tree, for example:

```sh
git show 6717943:legacy/i386/kernel/src/usb.c
```

Keep the verified HID packet/report behavior when that port is requested. The
frozen `dist/scos-32bit.img`, checksum and provenance remain untouched under the
standing milestone-retention rule. Historical documentation remains useful;
`os.html` stays. Keeping one frozen artifact is different from keeping duplicate
obsolete source/build trees.

The active BIOS loader still contains `.code16` and `.code32`: x86 BIOS starts
in real mode, so those transitions are part of the **new 64-bit boot path**.
Removing them is not a valid way to eliminate the old architecture. Supporting
UEFI would be a separate implementation, not a change of assembler flags.

## Concrete corrections

### Package identity and ELF validation

Previously, the packager checked architecture and size but could accept a stale
`kernel.bin` alongside a newer `kernel.elf`, then calculate a perfectly valid CRC
for the wrong payload. A checksum does not establish that binary/linker pairing.

The packager now validates:

* ELF64 little-endian version-1 executable, AMD64 machine, exact entry address,
  ELF header size, program-header size/count and table bounds.
* Linked start, padded file end and page-aligned memory end against staging and
  bootstrap limits; nonempty flat payload.
* Identity load addresses, `filesz <= memsz`, on-disk segment bounds and linked
  memory bounds; no overlapping load spans.
* Readable segments, no unknown permission bits or simultaneous write+execute;
  segment alignment and a file-backed executable entry point.
* No PT_INTERP or PT_DYNAMIC: there is no interpreter/dynamic kernel loader.
* Reconstructed PT_LOAD bytes and zero padding match the entire padded raw
  payload, byte-for-byte. A different raw byte fails before image publication.
* `stage2.bin` matches the `.text` extracted from its companion ELF, so patch
  symbol offsets cannot silently be applied to an unrelated stage2 binary.
* Stage2 size/entry, bounded and nonoverlapping patch fields, and MBR signature
  and reserved label area.

Only after validation does it form the complete image and atomically replace
its host output using a same-directory temporary file. Validation failure leaves
the previous output untouched. This is not a host power-loss durability claim;
there is no fsync-based publication transaction. CRC32 still detects accidental
payload corruption only: secure boot and adversarial authentication do not exist.

### Emergency console bounds

Normal output increments `row` before scrolling. A fatal NMI can interrupt that
window with `row == 25`; the old emergency print could start past the visible VGA
text buffer and eventually fault beyond its mapped page. The output path now
normalizes row/column before each VGA indexing operation. It needs no lock that
an interrupted context could own. The terminating fault path does not resume
that interrupted print. This does **not** make the console a multi-CPU logger,
recursive-panic recovery system or an interactive terminal.

### Descriptor state and fault reporting

LGDT does not clear an inherited LDTR cache. The kernel now executes LLDT with
a null selector: there is no supported LDT, and TI selectors must not reach a
firmware-era table. A real invalid-LDT-selector injection now reaches #GP.
LTR includes a compiler memory barrier before TSS use. The exception dispatcher
captures CR2/CR3 before console output; it never follows the interrupted RIP or
RSP as pointers, because either may be invalid. Dedicated IST stacks remain in
place for double fault, NMI and machine check.

### Firmware output and build contracts

Stage1 preserves working registers and DS/ES around BIOS error printing and
clears DF before each string read. Disk calls preset carry before asking firmware
to report success. Documentation no longer claims BIOS calls complete in a fixed
number of milliseconds or guarantee that firmware never enables interrupts.

Linker assertions now explicitly bound the payload by the 384-KiB staging buffer,
keep executable text within the temporary executable mapping and require every
stack guard to be page aligned. The C handoff header asserts every field offset,
not just the total structure size. These are drift detectors for subsequent
edits; they are not substitutes for runtime input validation.

## Component-by-component contract and limits

| Component | Current contract | What it does not provide |
| --- | --- | --- |
| `Makefile` | Explicit freestanding AMD64 C, no host runtime, PIE, red zone or generated SIMD; separate BIOS assembly flags; linker and milestone guard | Dedicated cross-toolchain provisioning, automated physical acceptance |
| `boot/stage1.S` | Fixed 16-sector stage2 from the BIOS boot drive, same-drive EDD/CHS fallback, visible terminal failure | Disk discovery, filesystem loading, unrelated-disk fallback, native UEFI |
| `boot/stage2.S` | CPU gates, bounded E820 collection, reserved-overlap precedence, A20 verification, same-drive EDD loading, CRC, long-mode transition | Cryptographic authentication, dynamic boot modules, framebuffer/GOP, arbitrary large kernel loading |
| `kernel/include/boot.h`, `boot.c` | Fixed versioned 80-byte handoff, 24-byte map entries, 64-bit overflow checks and conservative usable-range tests | Merging fragmented maps, ACPI table ownership, framebuffer/module ABI fields |
| `kernel/x86_64/entry.S` | Preserve handoff while clearing BSS; establish kernel-owned aligned stack; reload segment descriptors | FPU/SIMD initialization/context switching, TLS, ring-3 entry, AP startup |
| `kernel/x86_64/linker.ld` | Fixed 1-MiB placement; separate text/rodata/data permissions; BSS/stacks and explicit limits | Higher-half relocation, PIE or arbitrary modules |
| `tools/gen_vectors.py`, `interrupt.c` | All 256 gates; normalized error slots, 15 GPR saves, CLD for C, aligned SysV call, IRETQ; TSS/IST, PIC/PIT | Scheduler, user transitions/SWAPGS, APIC/SMP, general device IRQ registration |
| `memory.c` | One-time initialization with maskable IRQs off; static kernel page tables reserved; sparse W^X/NX mappings; E820-approved zero-on-allocation page pool below 16 MiB | Heap, DMA mapping, allocations in NMI/fatal handlers, SMP serialization, high-RAM allocation or process VM |
| `console.c` | Bounded serial polling, VGA text scrolling, nonallocating emergency text/hex output and halt | Keyboard input, six TTYs, framebuffer compositor, complete historical panic presentation or multi-CPU logging |
| `main.c` | Ordered validation/platform initialization; real page allocation/write/release accounting; three real PIT ticks before idle | Dynamic driver loader, desktop/app startup, old device initialization or simulated readiness |
| `tools/makedisk.py` | Validated matched artifacts and deterministic raw startup-test disk | Guest storage driver, persistent filesystem or signed boot chain |

The page allocator disables maskable IRQs for ownership changes on **one CPU**.
NMI/machine-check/fatal paths must not allocate. Released pages are not erased
immediately; the next allocation zeroes them. Callers must release all references
before freeing. The kernel's own page-table arrays are inside the reserved linked
kernel span, not available for allocation. Usable memory above 4 GiB is retained
in the map but not mapped/allocated. These limits must be deliberately revisited
before adding drivers or concurrency.

The interrupt generator deliberately saves RBX before using it as a callee-saved
anchor for the original RSP. Rounding RSP for C therefore cannot lose the exact
hardware frame. CLD affects only the handler: IRETQ restores the interrupted DF.
The kernel uses general registers only; no claim is made about saving SIMD state.

## Verification of this revision

Actual QEMU 11.0.2 TCG guests, one CPU, disposable snapshot disks and private
QMP/GDB sockets; no host disk/device passthrough or guest network:

* Boot/readiness and real PIT interrupts at 64 and 256 MiB.
* 2048-MiB guest with 1-GiB low-RAM placement: E820 usable addresses above 4 GiB
  preserved/reported. This tests high addresses, **not** high-RAM allocation.
* Unsupported qemu32, missing NX, missing PAE and 486 CPU configurations rejected.
* Corrupted on-disk payload rejected by the loader checksum.
* All 15 GPRs, RSP and interrupted DF preserved over real timer interrupts.
* Injected invalid opcode, null read, text write, stack-guard read, NX execution,
  invalid LDT selector and bad-stack double fault reported the expected vector
  and error code rather than resetting.
* NMI injected with invalid RSP and deliberately interrupted cursor state
  (`row=25`, `col=80`): dedicated NMI stack reached reporting/halt without a
  secondary exception. This directly exercises the emergency-console correction.
* Package mutations: wrong ELF class/type/entry, out-of-bounds program-header
  table, RWX segment, oversized file segment and PT_DYNAMIC rejected.
* Independently stale kernel and stage2 binaries rejected. The prior image was
  unchanged after both failed packaging attempts. Valid packaging reproduced
  exact bytes; clean rebuild and frozen milestone guard passed.

Prior baseline tests, including boot-range/truncated-disk/handoff rejection,
are recorded separately in [BOOT64.md](BOOT64.md); do not confuse earlier tests
with additional hardware tests in this pass. Temporary harnesses/mutated disks
are removed after verification; the legitimate host emulator tools remain.

## Stop point

No physical-PC boot result, exhaustive bug-freedom, full desktop behavior or
“similar or better” complete OS is claimed. Within startup, the protections and
failure reporting are stronger than the old flat i386 boot, but this small
foundation cannot replace its functionality. No USB/input, graphics, storage,
ACPI power, application or new third-party driver port was started in this pass.
Wait for the user's next conversion instruction.

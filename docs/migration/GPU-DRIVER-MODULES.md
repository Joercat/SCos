# Per-family GPU driver modules: the on-disk drivers, the loader, and what each one costs

Status: implemented, verified in emulation, and shipped in `dist/scos.img`.
One family (`ati`, the Rage128 2D engine ported from Haiku) exists as a module today; the
machinery is built for fifteen, and the other fourteen cost nothing until their file is on the disk.

This document is the record of how a driver gets onto the disk, how exactly one of them is chosen
and loaded, what the kernel refuses to let it do, and every defect found while making the first one
boot. The measurement data behind "which families are worth porting" is in
`GPU-BASIC-2D-RESEARCH.md`; the detection layer that names the family is in `GPU-AUTO-DETECT.md`.

## 1. Why the driver is a file and not part of the kernel

The requirement is that a machine loads the driver for the chip it has and nothing else. Keeping the
families as compiled-in objects cannot satisfy that: the code is in memory whether used or not, and
every family added grows the kernel image and the audit surface of a component that runs with full
privileges on every boot.

So each family is a separate file in the ESP, and the choice of which one to read happens in the boot
stub, which is the only component that has both a filesystem and the detection result:

```
\SCOS\ATI.MOD        <- the only one read on a Rage128 machine
\SCOS\NEOMAGIC.MOD     ...present on the disk, never opened, never mapped
\SCOS\DRVLIST.IDX      count + (name, size, crc32) per module
```

`\SCOS\<FAMILY>.MOD` is a fixed name derived from the family the shared matcher selected, so the stub
performs one directory lookup and one file read; it never scans the directory. `DRVLIST.IDX` exists so
the count of what is *not* loaded can be reported without reading any of it. Both facts are printed on
every boot by the kernel, because "the other families are never loaded" is only worth saying if a
number comes with it:

```
gpu: module 3 module(s), 27224 B on the boot disk: 13536 B opened for this chip, 13688 B never read
```

That line is asserted by `tools/tests/test_gpu_detect.py`, which boots a real guest and checks the
guest's own log.

## 2. The boundary between the stub and the kernel

The stub has the filesystem but no page tables of its own, no heap it can keep, and the firmware's
`ExitBootServices()` immediately ahead of it. The kernel has the page tables, the device, and the
matcher's own opinion of what the chip is, but no filesystem. So the handoff is deliberately split:

| step | who | what |
| --- | --- | --- |
| PCI scan, family match | stub (`boot/uefi/main.c`) | the same shared matcher source the kernel uses (`gpu_match.c`) |
| read `\SCOS\<FAMILY>.MOD` | stub | into EFI pool, while the boot volume handle is still open |
| read `\SCOS\DRVLIST.IDX` | stub | into EFI pool, same reason |
| `root->close()`, `ExitBootServices()` | stub | the volume and every firmware service disappear here |
| copy bytes into the arena | stub | module region and index region, then the pools are freed |
| validate, relocate, bind, self-test | kernel (`kernel/drivers/gpu/gpu_module.c`) | nothing is trusted from the stub, including sizes |

The ordering is not negotiable and was the first thing to break: **any file read must happen before the
volume is closed.** The initial version of the stub read the module *after* `root->close(root);
root = 0;`, so it fetched `open`/`read` through a NULL handle, jumped to whatever the firmware had
left at that address - the shadowed video BIOS at `0xE0000` - and OVMF died with
`X64 Exception Type - 06(#UD) / Can't find image information`. A UEFI stub cannot be debugged with a
stack trace, so this class of mistake is worth preventing structurally: reads are done through the one
helper (`read_file`) while `root` is valid, into pools, and the arena copies happen after the arena is
published.

`handoff.module_state` records what happened, so the kernel can tell "no module for this family" from
"the module is corrupt" from "nothing was selected": 0 nothing selected, 1 bytes staged, 2 the disk has
no module for the detected family, 3 the read failed.

## 3. Where the module lives in memory

The arena is 2 MiB allocated by `AllocateMaxAddress` below the 4 GiB line, and it is the only region
the kernel maps after `ExitBootServices()`:

```
arena + 0            boot_handoff (192 B)
arena + 4 KiB        UEFI memory map (<= 128 KiB)
arena + 200 KiB      DRVLIST.IDX copy
arena + 256 KiB      page tables (table_next .. table_end)
arena + 2 MiB - 256 KiB  GPU_MODULE_AREA: header, image, .bss, relocations
```

`memory_init()` maps that last range `RWX` and nothing else outside the kernel image is writable and
executable at the same time. `table_end` is the module area, so the page-table allocator can never
grow into a module, and `GPU_MODULE_REGION` (256 KiB) is the hard cap the loader enforces against the
header's own sizes. Table frames past the pool come from the page allocator instead of panicking; that
allocator cannot hand out arena pages, because the arena is excluded from the free ranges, so the
fallback does not weaken the boundary - it only removes the possibility of a boot-time reservation
becoming a ceiling on how much a machine can map.

Everything the loader does is expressed as an offset from the start of the *file*, which is also where
the copy begins: the header occupies `[0, 192)`, `.text` (which contains the module's claimed ID table)
and `.data` follow contiguously, `.bss` is reserved after them, and the relocation table comes last.

## 4. Module format and what the loader refuses

`kernel/include/gpu_abi.h` is the whole contract: a 192-byte header, the image, a relocation table of
16-byte records, and the module's own PCI ID table inside `.text`. The header is checksummed over its
own words so a torn read cannot produce plausible sizes, and the payload CRC32 uses the same polynomial
as the kernel image check in the stub.

The loader rejects, in order, each with a named reason in the log rather than a number: magic, ABI
version, header size, header checksum, payload CRC, sizes that exceed the region, blocks that are not
contiguous, a relocation table overlapping the image, an entry point outside the read/execute block, a
family name that disagrees with what this kernel detected for this function, an ID the module claims
that the generated table does not list, any relocation type outside the four implemented, a relocation
that would rewrite the header or leave the image, and an index whose count disagrees with the stub.
The refusal codes and their strings are in `reject_reason()`; the reasons are the documentation.

Two refusals are policy rather than safety, and both are the reason a module cannot damage the machine:

* **no imports.** A module declares no undefined symbols; everything it can reach in the kernel is the
  export table passed to its entry point (`log`, `map`, `read32`, `write32`, `ticks`, `spin`,
  `wait_bit`, `alloc`, and a read-only `device` record). `tools/build_gpu_module.py` refuses to pack an
  object with an undefined symbol, so "cannot import" is enforced at build time and re-enforced by the
  loader's relocation type check.
* **no display reprogramming.** A module that asks for `SCOS_GPU_MODULE_TAKES_DISPLAY` is refused
  outright, not ignored. The only way to destroy the one console the user can see is to retime it, and
  a driver that cannot ask is a driver that cannot do that.

After the checks pass, the module's `scos_module_init` runs, and if it returns an engine table whose
`size`/`abi` the kernel can call, the kernel runs a **self-test that reads back the pixels** (§6). Any
failure detaches the engine and the CPU compositor carries on; the desktop is never left half-painted.

## 5. `tools/build_gpu_module.py`, and the four things it must refuse

The packer takes the partially linked object (`ld -r` over the family's `.o` files) and emits the
format above. It is not a linker and does not pretend to be one: it lays out two blocks, folds
GOT-relative references that the linker already resolved, and refuses anything it cannot express:

* an undefined symbol (an import);
* a relocation type outside abs64/abs32s/pc32/plt32;
* an `ALLOC` section it does not lay out (a runtime table, TLS, `.eh_frame`, `.init_array`);
* a relocation landing outside the loaded image, or two landing on the same word.

`--verify` re-reads a packed file with the same rules and cross-checks every claimed ID against the
generated table, so `make gpu-modules` fails the build if a module claims a chip the kernel's detection
would not name for it. `--report` prints the sizes.

## 6. Verification by readback, and the aperture that is not the console

`engine_self_test()` saves a small rectangle with the CPU, asks the engine to fill it, waits, reads the
device memory back, asks for a screen-to-screen copy, checks both rectangles, and restores the saved
pixels. It counts mismatches instead of rounding them away, and it is skipped if the pixel format is
not the native one. A module is only *bound* when the numbers agree:

```
gpu: module ati bound: engine verified in device memory (225/225 pixels)
```

The rectangle is checked over 15x15 rather than the 16x16 requested. QEMU's model of this chip treats
`DST_HEIGHT_WIDTH` as an exact count, while the databook convention the port follows (and X.org's
`r128` driver) programs `count-1`; the readback is done over the area both interpretations agree on so
that a QEMU pass cannot hide an off-by-one that real hardware would show. The deviation is recorded in
`drivers/gpu/ati/module.c` as deviation 4 and must not be "fixed" by matching the emulator instead.

Which memory to read back from is a real decision, not an assumption: **the bound device is frequently
not the device feeding the console.** In QEMU with `-device ati-vga`, the firmware console belongs to
the primary adapter (`1234:1111` at 0:1.0) and the Rage128 at 0:3.0 is a second function whose
framebuffer lives at its own BAR. Reading back through the handoff's LFB would have found nothing and
wrongly reported a broken engine, so the self-test targets the surface the *bound function* scans out:
the console LFB when it owns the console, otherwise that function's own aperture, mapped by the kernel
and shared with the module through the same window table. `info.framebuffer_base/_pointer/_bytes` and
`vram_bytes` are set to match, because a driver bounded by the wrong extent either refuses everything
or writes past the card.

That distinction also decides whether the engine is *used*, and the OS says so out loud:

```
gpu: engine drives its own aperture only: another function feeds the console, so output stays on the CPU
```

`gpu_engine_drives_output()` is what `fb_flip_rect()` consults. When it is true, a damaged rectangle
whose pixels are all one colour is painted by the GPU and the copy into uncached device memory is
skipped (`screen.px` stays authoritative - the fill replaces the output pass, never the truth buffer).
When it is false the engine is bound and verified but the desktop is CPU-drawn, which is the correct
result in a virtual machine and not a missing feature. The `graphics` report prints which of the three
states the machine is in, and the test suite asserts both the "verified" claim *and* the absence of an
"accelerated output" claim in the emulated case.

## 7. The `ati` module

`drivers/gpu/ati/` is the Rage128 GUI-frontend 2D engine from Haiku's `rage128` accelerator
(`engine.cpp`, `accelerant.cpp` paths for solid fill, screen-to-screen blit, FIFO and engine idle),
restructured against SCos' module ABI: no accelerant machinery, no `set_engine_token`, no B* API. It
claims the 47 Rage128 rows of the generated table (the Mach64 rows in the same family are left to the
CPU path by name, and the module says so at init). Its header comment lists four deliberate
deviations from upstream, and each exists for a reason that has to survive future edits:

1. **Bounded waits.** Upstream's `WaitForFifo`/`WaitForIdle` retry forever and reset the engine between
   attempts. Here every wait has a deadline from `X->ticks()`, and a missed deadline latches
   `engine_dead`, returns an error, and the kernel detaches the engine. A kernel module must not be
   able to hang the machine, and a driver that hangs is worse than a CPU compositor.
2. **No clock or PLL writes.** `Rage128_EngineReset()` forces `R128_FORCE_GCP | R128_FORCE_PIPE3D_CP`
   and touches PLL registers; both are dropped, leaving `PC_FLUSH_ALL` plus `SOFT_RESET_GUI` in
   upstream's order. Re-clocking a card that is actively scanning out is how a driver blanks the only
   console.
3. **Scissor per operation.** Upstream leaves the scissor rectangle set by the last caller; here it is
   programmed to the destination rectangle before each operation and restored to the maximum
   afterwards, so a refused or failed operation cannot leave the engine clipping the desktop to a
   60-pixel band.
4. **`DST_HEIGHT_WIDTH` carries `count-1`** for the blit path (databook and X.org convention) while the
   fill path keeps the exact count upstream used, because the two are documented differently. §6
   explains how the self-test avoids blessing whichever choice the emulator happens to agree with.

A fifth behaviour is not a deviation but is worth naming: `GUI_ACTIVE`. Some cards - and QEMU's model -
report that bit as permanently set with nothing queued. The port samples the status once, right after
the GUI reset and before it has submitted a command, and if the card already claims to be busy the bit
is treated as unusable as an idle signal and the bounded wait is skipped, leaving the readback to
decide. On a card whose bit drops, the wait runs exactly as upstream wrote it.

## 8. Everything that had to be fixed for this to boot

No guess fixed any of these; each was found by reading the guest's own exception frame or the packed
bytes, and each is recorded here because the failure modes are not obvious from the code.

1. **Reading the volume after closing it** (§2). Symptom: OVMF `#UD` at `RIP 0xE0000`. The fault
   address was the shadowed video BIOS, reached through a NULL `root` handle.
2. **Probing a file's size with `get_position` without seeking to `UINT64_MAX` first** - always
   returns 0, so the module was never read at all. `read_file()` does the seek itself.
3. **`u8` bus-number loops in the stub's bridge walk** wrap forever when a bridge reports
   subordinate 255. Now `unsigned` with an explicit `< 255u` bound.
4. **One offset origin for the whole format.** The packer recorded relocation places and symbol targets
   as offsets inside the *image* while the header's `rxe_off`/`data_off`/`entry_*` are offsets inside
   the *file*, and the loader added the load address to both. The first relocations therefore rewrote
   the module header - including `reloc_off`, which is why the walk died in the middle of the table
   with `CR2 = base + 0x89001e58` and no symbol worth naming. Now: one origin (start of file),
   documented in `gpu_abi.h`, asserted by the packer's `--verify` and by the loader's
   `offset >= SCOS_GPU_MODULE_HEADER_SIZE` predicate, which makes "a relocation into the header"
   impossible to ship again.
5. **The relocation table overlapping `.bss`.** `reloc_off` was placed after the *loaded* blocks, which
   is exactly where a module's zero-initialised globals live, so the module's own writes edited the
   table the loader was reading and its `.bss` looked untouched from outside. The layout is now
   `header | text | data | bss | relocs` and the loader refuses `reloc_off < header + image_size`.
6. **PC-relative relocations double-counted the addend.** For RELA records x86-64 puts the `-4` in the
   addend; the loader also subtracted the field width, so every folded `GOTPCREL`/PC32 reference in a
   module pointed four bytes low. The formula is now `S + A - P` with `base` cancelled, which is what
   the "same origin, so the base cancels" comment in `relocate()` means.
7. **`device_map()`'s "already mapped" probe allocated page tables.** `present_leaf()` walked with
   `descend()`, which creates missing levels, so probing an unmapped range left an empty table behind -
   and the next probe of the same 2 MiB block found a present-but-empty leaf and reported the range as
   unusable. Every device mapping that crossed a 2 MiB boundary failed, which is why the Rage128
   register BAR (whose `GUI_STAT` sits at 0x1740, i.e. past the first page) could not be mapped at all.
   `device_leaf()` now never allocates, a leaf is reused only when it carries an address, and an
   empty one is filled. `device_unmap()` uses the same non-allocating walk.
8. **The module mapped one page of its register BAR.** Haiku's code maps the whole BAR; the port asked
   for `0x1000` and then every access above it was dropped by the kernel's bounds check, so the driver
   appeared to bring up and did nothing. It now requests the BAR's real size, the kernel clamps the
   mapping to what it can map and reports the mapped size honestly, and the port refuses to bind if
   that size cannot reach `GUI_STAT`.
9. **`vram_bytes` was never filled in.** The port bounds every rectangle against the device memory it
   was told about; the kernel left the field zero, so `rect_ok()` refused everything and the self-test
   failed with "engine refused the self-test fill". The kernel now sets it from the aperture it chose,
   and the refusal path in the module prints the two numbers once - a compositor that silently loses
   every accelerated rectangle is the kind of bug that takes a week to notice.
10. **`make` did not rebuild the disk when a driver changed.** `$(BUILD)/scos.img` listed
    `$(GPU_MODULE_FILES)` in its prerequisites, but that variable is appended by the per-family rules
    *later* in the Makefile, and make expands prerequisites as it reads the file - so the list was
    empty and an edited driver silently shipped the previous `.mod`. The dependency is now added after
    the rules. Every one of these bugs was chased past a stale image at least once; this is why.
11. **The `graphics` report claimed "no hardware backend linked" while a module was bound**, and the
    family record said "not ported yet" for the family whose module had just verified itself. Both now
    distinguish three states instead of two, and the test suite asserts the honest wording.

## 9. How to check it yourself

```sh
python3 tools/setup_qemu.py            # once, into .tools/qemu
make && make gpu-modules               # kernel, disk image, and every module
python3 tools/tests/test_gpu_detect.py # tables, host harness, then a real guest
python3 tools/run_qemu.py --memory 256 --device ati-vga --usb-boot
```

The guest log must show `matched=ati`, one `validated` line, the accounting line naming what was left
unread, and `bound: engine verified in device memory (N/N pixels)`. To watch the selective load, put a
decoy in the module directory and repack - nothing else has to change, because nothing else looks at
the directory:

```sh
head -c 12400 /dev/urandom > build/gpu/neomagic.mod
python3 tools/makedisk.py build
```

The `ati` module must still load and the decoy must never be mentioned: it is not opened, not
validated, and its bytes are counted only in "never read". Delete it again before shipping.

On physical hardware the same lines are the whole check, plus two more: the `graphics` report should say
`used for solid output rectangles`, and `module rejected: ...` in a boot log is a bug report rather
than a failure - it is the loader saying which predicate the file on the disk failed.

## 10. What is not here

No 3D, no GSP/RM or vendor-firmware stacks, no mode setting, no page-flipping, no vsync (the measured
per-family capability table is in `GPU-BASIC-2D-DISPLAY-FEATURES.json`; this chip's family has no vsync
path upstream either). A VRAM back buffer is offered by the module and deliberately not enabled: the
compositor's back buffer stays in RAM until the write-combining policy for a driver-owned aperture is
settled, which is logged rather than hidden. One ported family exists; adding another means porting its
engine under the same ABI, and nothing about the loader, the disk layout, or the boot stub changes per
family - which is the property this whole design was built to have.

## 9a. Adding the next family, exactly

The build discovers a family by directory: `drivers/gpu/<family>/module.c` is the only file a new port needs.
`make gpu-modules` compiles every `.c` in it with `-fno-pie -mcmodel=small`, links them with `ld -r`, runs
`tools/build_gpu_module.py` to emit `build/gpu/<family>.mod`, and `--verify` then re-packs and compares, so a
module that is not byte-reproducible fails the build.  `makedisk.py` publishes each `.mod` as a flat file plus
`DRVLIST.IDX`, and the stub opens *only* the one whose stem equals the family that detection named.  The
Makefile needs no edit and neither does the loader: `gpu_module.c` knows only the ABI, never a family.  The
kernel side is complete for a new family the moment `gpu_ids.h` gains its rules, which
`tools/research/gen_gpu_tables.py` already generates for all fifteen.

Measured state of the candidate ports, from the pinned Haiku tree (engine line counts are the 2D core only):

| family | 2D core | total | what an added `module.c` would have to reproduce |
|---|---|---|---|
| `ati` | 496 | 6,556 | **done** - `RBBM_SOFT_RESET` + `RBBM_GUI_WAIT_UNTIL_IDLE` + readback, in `drivers/gpu/ati/` |
| `neomagic` | 455 | 7,811 | `ACCR(STATUS)` bit 0 idle wait, `engine.control` depth bits 8-9 and pitch bits 10-12, the FIFO wait that only works on NM2090/NM2093 (the driver therefore idles the engine before *every* programming sequence, which removes the unbounded-FIFO risk entirely), and refusal of any depth other than 1/2/3 bytes |
| `matrox` | 496 | 13,387 | FIFO-space polling on a shared register, plus per-operation context words |
| `s3` | 710 | 7,039 | a serialised BLT register index pair rather than a MMIO window |
| `intel_810` | 83 | 2,425 | the smallest 2D core in the set; no device model exists here either, so like NeoMagic it would be committed against hardware that can prove the readback, never against a guess |

A chip the naming table knows but no driver table binds can never reach this pipeline at all:
`gpu_module_eligible()` is the single predicate that allows a module to be read, it fails for a naming
row, and the UEFI stub asks the same question before it opens a file - so widening the names in
`gpu_ids_registry.h` cannot widen what gets loaded, and an RTX 5050 is identified without a driver being
read for it.

`neomagic` is the next one deliberately *not* committed here: QEMU has no NeoMagic device model, no machine in
reach has one, and a driver whose engine state has never been read back is exactly the "we assume it works"
claim this whole subsystem exists to prevent.  It lands with a machine that can prove it, or not at all.

## 9b. What is refused by test, not by assertion

`test_gpu_detect.py` has four layers: the generated tables against the pinned source, the detection unit
compiled for the host against a simulated bus, real emulated PCI functions in a booted guest, and a tamper
layer.  The last one edits `build/scos.img` in place - one payload byte of `ati.mod` flipped and
`DRVLIST.IDX`'s crc for that file recomputed, so the boot stub's check passes - and boots it.  The kernel must
print `gpu: module rejected: payload CRC mismatch (the file is corrupt)`, must not print `validated` or any
readback figure, must report `unavailable (no driver module loaded for this chip)` while still showing the
stub's opposite verdict (`module store: staged ... checksum verified`), and the window manager must still
reach its main loop with the fallback notice on screen.  In other words the trust chain is tested by breaking
the link the firmware is responsible for, and the image is restored afterwards.

## Ownership across a 64-bit BAR

Detection compares the address UEFI reported for the linear frame buffer with each function's BARs, so
the fold of the two config words in `read_bars()` (a BAR whose type bits say 64-bit consumes the next
word as its upper half) is what lets a machine whose card puts its frame buffer above 4 GiB say which
function the firmware is scanning out from. `tools/tests/gpu_detect_sim.c` carries that case with an
`0x10de:0x2d83` function at `BAR1 = 0x100000000`: ownership is found, the chip is named from the
registry, and no module is offered - which is the accurate statement about that card. Finding a chip's
addresses and driving its engine are different claims, and only the first one is true here.

What this does *not* change is the ceiling on what can be accelerated. The mapping layer above 4 GiB and
the raised `device_map()` bound are what a real panel needs to be scanned out at all; a 2D or 3D engine
for Fermi-and-later NVIDIA parts still does not exist outside the vendor stack, whose supported hardware
begins at Turing with GSP firmware active. See GPU-BASIC-2D-RESEARCH.md for the audit that ends that
road, and note that a request to map a 4K frame buffer no longer panics the machine: that was fixed.

## Reading the chip on a function no driver claims

Identification alone meant that on a Blackwell panel the kernel had never issued a single read to the
device it was describing. `probe_registers()` in `gpu_detect.c` now maps the first page of BAR0 of every
detected display function that no module could claim, and loads `NV_PMC_BOOT_0` and the dword after it -
no sizing, no reset, no write of any kind, and no mapping at all where a module owns the aperture, so an
aperture is never mapped twice. `gpu_report()` and `graphics` print the address, the value, and the chip
id / implementation / revision decoded exactly where nvkm reads them (`[31:20]`, `[11:8]`, `[7:4]`), and
four states are distinguished rather than collapsed into a number: readable, all-ones (the device did not
answer), mapping refused, and memory decode disabled by firmware. `tools/tests/gpu_detect_sim.c` drives
all of it against a fabricated aperture - including the insistence that an all-ones read is never reported
as if it were a chip id - because no emulator here can present an NVIDIA function.

This is device access, not acceleration, and it is described that way on purpose: it establishes that the
kernel can talk to the chip at the address the firmware left, which is the precondition any engine needs.

## What "no matter what it takes" would have to include, for a 2025-class NVIDIA card

The claim that Blackwell is GSP-only was re-checked against the 2026 record rather than asserted:

- nouveau's GB202 display series states it in its own words - "GB20x is GSP-only. This table supplies the
  register programming the GSP-RM display path needs from the chip" (v3, August 2026), and a reviewer of
  the same series notes "booting these cards without GSP isn't possible on nouveau anyhow". Even scanout
  register programming for GB207 goes through the vendor's Resource Manager.
- `nova-core`, the in-tree Rust driver being written for Hopper/Blackwell, needs the GSP image *and* a
  second ELF32 (FMC) image, selected per architecture - `.fwsignature_gb20x` covers GB202/203/205/206/207
  - with `ENOTSUPP` otherwise (patch series, February 2026; v9 April 2026).
- NVIDIA's own tracker records that `NVreg_EnableGpuFirmware=0` has no effect on Blackwell: GSP is
  mandatory, and even there it is reported to die under load (Xid 109) without a heartbeat restart.
- Linux 6.18 made GSP the nouveau default for Turing/Ampere, keeping the older firmware path only for
  those two generations; "We've always been GSP-only on Ada and later."

That is also the explanation for "nouveau worked on my PC out of the box": a distribution loads the
R570 GSP image from `linux-firmware` on the user's behalf (NVIDIA donated the 570.144 blobs precisely
because Hopper and Blackwell cannot be brought up without them, 61 MB shared by GA10x/AD10x and larger
again for GB20x). Nouveau was not doing it unaided; a package nobody noticed had installed made it work.

Under the standing rule that this kernel stays its own and that imports larger than a few thousand lines
are not shipped, the consequence for a GeForce RTX 5050 was recorded here as "there is no engine to port".
That sentence was too strong, and the correction is worth writing down because it is the reason a module of
any kind is now shipped for that family:

* What *is* published, in MIT-licensed sources, is the vocabulary a submission needs: NVIDIA/open-gpu-doc
  carries `classes/dma-copy/clc{0,1,3,5,6,7,8,9}b5.h` (the copy engines, Blackwell generation included)
  and `classes/host/clc{0..c}6f.h` (the FIFO host class), plus `manuals/ampere/ga10{0,2}/*.ref.txt` with
  the RAMFC/RAMHT channel layout and the NVCE method set.  A 2D copy path for a Blackwell chip is therefore
  writable from documentation, the same way the Cirrus module was.
* What is not published is the part that makes those classes reachable on that silicon: the Ampere-and-later
  register databases (NVIDIA ships short *addenda* for Hopper/Blackwell - `blackwell/gb202/dev_pmc_zb.h`
  is 36 lines and defines fault bits, not addresses), the display and power bring-up that runs through the
  chip's system processor, and the GSP image that does it (`gsp-570.144.bin`, 61 MB, not redistributable
  in a form this tree may embed).

So the honest split is: *identification* is fully available and is what this increment ships; *drawing*
needs a channel, a minimal GMMU page table and a bring-up path that no public document describes end to
end.  Claiming the second from the first is the failure mode this file exists to prevent.

Two facts pinned down for that next step, because "where is it" and "what does it mean" are separate
questions with separate answers.  Turing publishes the engine inventory as a readable array -
`manuals/turing/tu104/dev_top.ref.txt`: `NV_PTOP_DEVICE_INFO(i) = 0x00022700 + i*4`, 64 entries, `R--4A`,
where each entry carries `CHAIN 31:31`, `ENGINE_ENUM 29:26`, `RUNLIST_ENUM 24:21`, `INTR_ENUM 19:15`,
`RESET_ENUM 13:9` and valid bits 5:2, with `TYPE_ENUM 30:2` naming the unit (LCE = 19, NVDEC = 16,
NVENC = 14, GSP = 20).  Blackwell republishes *the enum* and only the enum:
`open-gpu-kernel-modules/src/common/inc/swref/published/blackwell/gb202/dev_top_zb.h` is one `#define`,
`NV_PTOP_ZB_DEVICE_INFO_DEV_TYPE_ENUM_LCE 0x13` - the same number as Turing's, which is evidence the entry
format survived and no evidence at all about where the array starts.  There is no `gb207` directory at all.
So a walk of that table on a GB207 has to establish the address from the device before it decodes anything,
and until it does, the offsets in this paragraph are documentation, not inputs.

## NVIDIA GB20x: the first module for a modern card, and what it refuses to do

`drivers/gpu/nvidia/` is the third module on the disk and the first for a chip younger than 2010.  It is a
small, deliberately unfinished driver, and it is worth being exact about why that is the right shape:

* It reads `NV_PMC_BOOT_0` at offset 0 of the function's register BAR - one dword, twice, both reads
  reported - and decodes the fields where NVIDIA's published manual places them (`manuals/ampere/ga100/
  dev_boot.ref.txt`: MINOR 3:0, MAJOR 7:4, IMPLEMENTATION 23:20, ARCHITECTURE 28:24).  Because the newest
  published table stops at Ampere, a Blackwell architecture number is **printed as an unnamed number**
  rather than translated into a guess.
* It then asks the chip whether the *submission window* exists at all, because that question is answerable
  by reading.  NVIDIA documents the window in `manuals/turing/tu104/dev_usermode.ref.txt`: a 128 KiB page at
  BAR0+0x810000, `NV_USERMODE_CFG0` holding a 16-bit class id (`0xc461`, the `volta_usermode_a` class, on
  Volta and Turing), and `NV_USERMODE_TIME_0`/`TIME_1` at +0x810080/+0x810084 carrying PTIMER in nanoseconds
  at 32 ns granularity - the low five bits of `TIME_0` are always zero, and the manual specifies the read
  order TIME_1, TIME_0, TIME_1, repeat on mismatch, so a rollover of the low half cannot be reported as a
  jump of seconds.  `probe_usermode()` follows exactly that sequence (retries bounded at eight) and reports
  one of four *measured* answers: `window class 0x…=published clock +Nus`, `clock frozen`, `window
  silent`, and `window unreachable`.  The fourth exists because a real card's 16 MiB register BAR is longer
  than the kernel's per-mapping cap, so the page at +0x810000 is not inside the mapping the boot register
  came out of: the driver maps the window's own 128 KiB and, if the kernel will not give it that mapping,
  says *unreachable* rather than *silent*.  Conflating the two would report a chip as having nothing there
  on the strength of a limit in this OS's own mapper.  Blackwell's manual is not published, so a class
  number that is not `0xc461` is printed as a number and not translated into a generation, and no
  `GA10X`-style naming is invented for it.
* The same document names the doorbell, `NV_USERMODE_NOTIFY_CHANNEL_PENDING` at +0x810090.  With no channel in
  the run queue, ringing it hangs the submission path, so **it is defined and never written**: the define is
  there so that the next step is anchored to an address NVIDIA published rather than one guessed here.  The
  module's second log line says `doorbell at BAR0+0x810090 named, not written` and then states which way the
  clock answer pointed, because "we know where submission begins" and "we submitted" must not read the same.
* It writes nothing, and that is structural: the module never takes the `write32` export, so there is no
  path from its code to a store.  Its `describe()` reports the exact read count it issued - three reads when
  the window is silent, nine when the clock was sampled - and the host harness fails the run if a store is
  even attempted, or if the count in the string and the count the fake device served disagree.
* It offers no engine operation at all, which the kernel now understands as a distinct state
  (`identification_only`).  Every drawing predicate checks the operation pointer, so the CPU compositor
  keeps painting; the boot log says "a driver read the chip and reported it", never "engine in use".
* Refusal is the point of half the code: an all-ones or all-zero register block, a value that changes
  between the two reads, another vendor's function, or a BAR too small to hold the block each end in
  `init` returning -1 with a line saying which.  A driver that binds to a chip it cannot read produces
  confident nonsense in a report, which is the thing this whole subsystem has been built to avoid.
* To exist at all it had to be *allowed* to: the 19 Blackwell device ids were rows of the naming table and
  are now rows of the `nvidia` binding array, added by the generator that owns both (`LOCAL_EXTRA_ROWS` in
  `tools/research/gen_gpu_tables.py`) so that a regeneration cannot silently drop them, and so that an id
  cannot appear in both tables.  That moved the counts the boot prints to **1039 binding ids and 1277
  naming ids** - the two numbers `test_gpu_detect.py` reads out of a real boot, not out of the headers.

_Measured 2026-09-23._ The module compiles under the same `-Werror -ffreestanding` flags as every other
module and packs to `OK nvidia.mod: family=nvidia rxe=6000 data=88 bss=600 relocs=158 ids=19 load=6088
image=6688` - the growth over the first version being the time-window probe, its own mapping for the page
past the mapper's cap, and a 256-byte `describe` field in the kernel's device record, which both the panel
and the store line read.  `tools/tests/test_nvidia_ident.py`
compiles `module.c` unchanged against a fake
`scos_gpu_exports` whose register file is an array and runs 59 checks: the decode of each field, all five
refusal paths, the three window answers (a clock that advanced, a clock that did not, a window that answered
all ones), that exactly one mapping was taken and it was the register BAR rather than the frame buffer, that
the read count in the string equals the number of reads the fixture actually served, that a rebind
recomputes instead of repeating, and that every operation slot but `describe` is null.  The id list the module claims is checked against the generated
table it is cross-checked by, row for row, and against the naming header, where those ids must now be
absent.  In a QEMU boot the same image shows the store grew to three files while a Cirrus machine reads
only its own: `module 3 module(s), 27224 B on the boot disk: 13536 B opened for this chip, 13688 B never
read` - `nvidia.mod` among the never-read, because a Blackwell driver must not be poked at a GD 5446.

The kernel half was then measured on a booted machine, not only on a host: the suite packs a throwaway
module of the same shape (only `describe`, no `fill`) for the Cirrus family, boots it, and the guest's
serial log reads

```
gpu: bound for identification: it offers no rectangle operation, so there is nothing to paint-verify and
gpu: module cirrus bound: it read the chip's own registers and describes them; no engine was offered to verify
gpu: 0:1.0 module GD 5446 (PCI) identified this chip; it offers no engine, so the CPU compositor keeps the screen
gpu: a driver read the chip and reported it; nothing was asked of the chip, so rendering stays on the CPU compositor
```

with the panel saying `GPU acceleration: driver module cirrus (family cirrus) read this chip's own registers
and reports what it found; it offers no engine operation; the CPU compositor still paints every pixel of
this screen`, the report line `SCos port: driver module bound for this family reads the chip and describes
it; it offers no engine operation, so nothing about drawing has changed`, and - the number that decides
whether any of it was a regression - `pixels painted: 0 by the GPU's 2D engine, 17312532 by the CPU
compositor`, the same figure the boot before the module carried, plus `Renderer: CPU software compositor,
driver module read the chip and offers no engine` instead of a sentence that would have called it an engine.
The per-device line `link: no PCI Express capability on this function (conventional PCI slot)` is QEMU's
Cirrus being honest about the slot it is in; on the real card the same field decodes to what the board
negotiated, which is why it is read from the capability and never from a table.

_What this does not claim:_ nothing has been read from a Blackwell chip, because there is none here.  The
first measurement on the real card is the line the module prints, and it will say `NV_PMC_BOOT_0=0x…` with
a value nobody in this tree predicted - which is exactly why the driver reports instead of asserts.

## Cirrus CL-GD5446: the second family, and the first that paints the console it is on

_Measured 2026-09-22 on `arena/01a09dfd-scos`, QEMU 11.0.2, `-vga cirrus`, 128 MiB, single vCPU, TCG._

`drivers/gpu/cirrus/module.c` is the second on-disk driver module and the first one whose engine feeds
the screen the user looks at. It was written against the chip's programming model and then checked
against QEMU's CL-GD5446 (`hw/display/cirrus_vga.c`, `hw/display/cirrus_vga_rop.h`) rather than against
a datasheet summary, because the model is the device this repository can boot and three of the
differences mattered enough to break the driver:

- The flat bitBLT register block starts at **offset 0x100 of the register BAR**, not at 0. The first 256
  bytes of that BAR are the legacy VGA index/data registers remapped into memory, so writing the blit
  registers at offset 0 is *silently inert*: the stores land in the VGA port window, the engine is never
  told to start, and every register read still looks plausible.
- **`BLTWIDTH` counts bytes, `BLTHEIGHT` counts rows**, and both hold one less than the transfer.
  Programming a pixel count instead makes the card paint a quarter of the requested rectangle and report
  success; that is how this was first caught, with the kernel's read-back self-test scoring 60 of 225
  pixels (4 columns the engine filled correctly, 15 rows it never reached).
- The control byte is **edge-triggered**. A transfer begins when `START` goes 0 → 1, and releasing
  `RESET` is its own event: writing `RESET` and then `START` reads as "reset released" and nothing
  happens. The order has to be reset, release, start.
- `BLTWIDTH`, `BLTHEIGHT` and both pitch registers keep **five bits of their high byte**, so a register
  round trip must be judged against the masked value. The module's init-time probe does exactly that and
  reports the expected and observed values into the boot log, which is what turned "the writes never
  landed" from a guess into a measurement.
- The model rejects a transfer whose row exceeds its 2 KiB staging buffer, so a wide rectangle is issued
  as a row of tiles and each tile is waited on before the next starts. A backwards copy is tiled from the
  far end of the row, because otherwise the tile written first is the source a later tile still needs.

**What this engine does not do**, in the same breath as what it does: no mode setting (the scanout
surface is the one the firmware validated), no hardware cursor or overlay, and no copy from system
memory. The last one is a property of the chip rather than a shortcut - CPU-to-video on a CL-GD5446 is
a stream the processor feeds through the bitBLT window, not a bus-master read of RAM, so "let the GPU
pull the compositor's frame out of memory" is not expressible on this hardware; the composition target
has to be the card's own memory, which is what `vram_window` and `set_surfaces` in the module ABI exist
for and what the kernel does not yet ask of a driver.

The family record for Cirrus in `kernel/drivers/gpu/gpu_ids.h` is **hand-authored, not generated from an
upstream table** - there is no Cirrus 2D driver in the tree the generator reads. That is stated in the
record itself (`.note`), in `.source`, and in a comment block above the id row; the row and its
capability bits are held in `tools/research/gen_gpu_tables.py` so a regeneration reproduces them instead
of deleting the family, and `python3 tools/research/gen_gpu_tables.py --check-local` (run by
`tools/tests/test_gpu_detect.py`) fails if the header and the generator drift apart or if the counts at
the bottom of the table stop agreeing with the rows above them. The module loader's own cross-check is
the last line: an id a module claims that the table does not carry is a refusal to load, not an
unverified load.

### Measured numbers

All four are from `tools/tests/test_gpu_render.py`, which boots this image and reads the results out of
the running kernel and out of QEMU's own screenshot of the emulated device:

| operation | engine | CPU |
| --- | --- | --- |
| kernel self-test: fill 16x16 in card memory, then move it 16 px | 225 of 225 pixels read back | 0 |
| compositor repaint of the whole 800x600 screen | 480,000 pixels | 0 |
| one console scroll, 768x480 moved up 16 rows | 368,640 pixels | 0 |
| desktop brought up on this machine, before any of the above | 0 | 17,312,532 |

The last row is the honest baseline: the engine takes the work it is asked for, and the compositor still
copies a great deal of non-uniform desktop with the CPU because its source of truth is RAM. The colour
the engine painted is present in QEMU's `screendump` at the corners and the centre of the frame, which is
the part of this that is a measurement rather than a claim - the pixels are in the device's memory, not
merely in the kernel's idea of it.

Booting this device also exposed a loader rule worth stating on its own: EDK2's Cirrus GOP *advertises*
1024x768 while reporting a frame buffer that only holds 800x600, and `boot/uefi/main.c` used to treat
that as fatal. It now sets each preferred mode, checks the aperture can actually hold the screen, and
falls to the next mode if not - on real hardware that is the difference between a desktop and a black
screen. Measured in the same run: `console LFB at 0x80000000+1920000, 800x600 stride 800`, with 0 PCI
config writes issued by detection.

For comparison, `-vga none -device ati-vga` was tried in the same session as a console device and gives
no GOP at all (`EFI_NOT_FOUND`, so no boot), which is why the ATI module is verified as a second PCI
function in every other test and does not drive any screen here.

## Asking the silicon which engines it has (r-engine-inventory)

The first increment for a bound NVIDIA module could only answer "what chip is this", and the second had to
answer "what can it be told to do", because building a submission for an engine nobody located is guesswork.
The published answer to the second question is a register array, not a table in a driver: Turing's
`manuals/turing/tu104/dev_top.ref.txt` documents `NV_PTOP_DEVICE_INFO(i)` at `0x00022700+i*4`, 64 entries,
each grouped with the next by its CHAIN bit and read according to its ENTRY kind - DATA carrying
`PRI_BASE << 12` and an instance id, ENUM carrying an engine number and a runlist number behind valid bits,
ENGINE_TYPE naming the engine from a list that includes GRAPHICS 0, VIC 12, SEC 13, NVENC0 14, NVDEC 16,
LCE 19, GSP 20, NVJPG 21. Ampere keeps the format at `0x00022800+i*4` and adds a header at `0x000224FC`
(`MAX_DEVICES 15:4`, `MAX_ROWS_PER_DEVICE 19:16`, `NUM_ROWS 31:20`).

For Blackwell consumer silicon NVIDIA publishes neither address. What it publishes, in
`src/common/inc/swref/published/blackwell/gb202/dev_top_zb.h`, is one line:
`NV_PTOP_ZB_DEVICE_INFO_DEV_TYPE_ENUM_LCE 0x13` - the same copy-engine number, which says the format carried
forward while the address left the published set. So `drivers/gpu/nvidia/module.c` asks both offsets and
reports the one whose rows decode. Three rules make that an answer rather than a reading of noise:

* A table counts only with at least two devices, four accepted entries, and at least one engine the
  published list knows. An aperture full of zeroes, or of the same dword repeated, produces entries and
  produces no inventory: `engine table silent at both published offsets`, which is a statement about the
  address and not about the chip.
* A clear valid bit is not a zero. The runlist and engine number are printed as `?` unless the entry that
  carries them vouches for them, and the PRI base is printed only when a DATA row was present. The
  distinction decides what to write next: a device with an address and no runlist is a different problem
  from a device with neither.
* One device, one tuple. Address, instance, runlist and engine number are taken from the *same* group,
  together or not at all. The host harness caught this bug while it was being written - a second copy
  engine further down the table overwrote the first one's engine number, so the report described a device
  that does not exist - and its check, `and the numbers belong to one device`, is what keeps it fixed.

Nothing here is written: the walk is 9 reads on a chip whose table has ended (the header, then four rows of
each candidate before the end is evident) and the read count in the report is the count the device served.
`tools/tests/nvidia_ident_host.c` grows to 68 checks over a seeded table at the Ampere offset, a smaller
one at the Turing offset, cleared valid bits, and a pattern that must be refused.

### The notification, and the predicate that was lying about it

The report and the notice are one change, because the machine that started this had a driver bound, a chip
read, no engine implemented - and a notification saying `GPU engine verified on a second adapter`. It came
from `gpu_engine_available()` testing `gpu_module_ops()` for non-null: every bound module hands over an
operations table, since that is how it is described, so the predicate measured whether a driver had loaded
rather than whether it could draw. It now tests the `fill` pointer, the same pointer every drawing path
tests before calling it, and `gpu_boot_notice()` gained the branch that machine was entitled to: title
`GPU is not rendering`, warning, named as a gap in SCos rather than as a fault in the hardware. The notice
text is bounded by a `_Static_assert` against the field `wm_notify()` copies into, because a notice clipped
mid-clause loses exactly the clause that says what is wrong. That field is 512 bytes and the notice wraps
onto as many rows as it needs (`docs/migration/PC-TEST.md`), which is what let the sentence say the whole
outcome: that the card is resident and measured, that `graphics` lists its engines, and that a listed engine
is the next thing to drive the screen with.

Measured on a booted image whose Cirrus module is replaced with one that binds, describes and offers no
fill (`tools/tests/test_nvidia_ident.py`): `notification: GPU is not rendering` in the serial log, `No 2D
engine bound` absent, `Engine: scratch module: reads nothing, offers no engine` in the `graphics` panel, and
`pixels painted: 0 by the GPU's 2D engine` unchanged - which is the honest pairing: the panel now reports
the idle GPU as idle in every place a user might look.
## Blackwell moved the engine table's format, not its address

The first real-hardware run of the inventory probe came back `engine table silent at both published
offsets`, on a card whose `NV_PMC_BOOT_0` had decoded cleanly a few characters earlier - so the reads worked
and the *decoder* was wrong, not the chip.  The published headers say why.  The file that describes this
generation is

    NVIDIA/open-gpu-kernel-modules  src/common/inc/swref/published/blackwell/gb100/dev_top.h
                                      (fetched 2026-09-23, kept at build/nvdoc/gb100-dev_top.h)

and GB202 - the package description a GB207 falls under - publishes only `dev_top_zb.h` next to it, three
further engine numbers, which is how the rest of the file is known to apply to the consumer part too.  It
says the row address is Ampere's, `0x00022800 + i*4`, and that the contents are not:

  * the block describes itself first: `NV_PTOP_DEVICE_INFO_CFG` at `0x224FC` carries VERSION 3:0 (2 selects
    this layout), MAX_DEVICES 15:4 (published default 153), MAX_ROWS_PER_DEVICE 19:16 (3) and NUM_ROWS 31:20
    (353).  A walk is bounded by what the chip claims, not by what a header once defaulted to;
  * a device occupies `MAX_ROWS_PER_DEVICE` dwords, so 96 bits, and the fields run across the dword
    boundary: TYPE_ENUM 30:24, INSTANCE_ID 23:16, GROUP_ID 15:11, FAULT_ID 10:0, RESET_ID 39:32,
    DEVICE_PRI_BASE 57:40, IS_ENGINE 62:62, RLENG_ID 65:64, RUNLIST_PRI_BASE 89:74, with each row's bit 31
    as the chain bit and a zero row meaning an empty slot;
  * there is a **second** block, PTOP1, with its own CFG at `0x324FC` and rows at `0x32800`, so asking only
    the Ampere address can call a chip silent while its inventory sits a few hundred bytes away.

`probe_engines()` therefore reads both CFG words (2 reads, and nothing else, when neither says version 2)
and only then walks; the legacy Turing and Ampere entry formats are still compared against each other for
the older parts.  LCE is `0x13` here as in Turing and Ampere, so the engine tally needed no renumbering;
HSHUB `0x18`, TMR `0x1f`, PBUS `0x33` and HUBMMU `0x35` are this generation's additions.  The two PRI bases
are reported as the raw fields (`pri-field 0x…`, `runlist 0x…`) rather than as addresses, because the fields
are 18 and 16 bits wide and turning them into an address needs an alignment claim no published file makes.

A table that does not decode now says what the chip answered - `engine table not decoded: CFG
0x224fc=0x… (v0x…), 0x324fc=0x… (v0x…)` - instead of `silent at both published offsets`, which is the
difference between "this silicon has nothing there" and "nothing I asked in the way I knew how to ask
answered".  Measured on the host (`tools/tests/nvidia_ident_host.c`, 79 checks): a v2 table assembled from
its own dimensions, the copy engine's fields taken across the dword boundary from one device and not two,
the second block found when the first is empty, an empty slot counted as nothing, and a CFG reporting
another version read as no permission to decode - with `writes 0` in every case.

`arch 0x1b (arch not in the published table)` stays as it is on purpose: `dev_pmc_zb.h` for this generation
does not publish an architecture enum, and a mapping invented here would be a guess wearing a fact's
clothes.  The number is printed so it can be checked against whatever NVIDIA publishes next.

## The v2 table decoded on real silicon, and what it said

The first Blackwell machine to run this probe came back with an inventory instead of a shrug:

    Engine: NV_PMC_BOOT_0=0x1b7000a1 at BAR 0xa4000000 arch 0x1b (arch not in the published table),
            impl 0x7 rev A.1; reads 163, writes 0 feeds the console; window class 0x1100 clock frozen;
            engines at 0x22800 v2 (60 devices x 3 rows of 152, 152 read): LCE 3/VIC 0/GFX 2/ENC 1/DEC 1/
            SEC 1/GSP 2 of 39 devices, LCE pri-field 0x001040 inst 0 runlist 0x03400 engine 1

That is GB100's `dev_top.h` format read out of a GB207: the CFG word answered `version 2` with its own
geometry (60 device slots, 3 rows each, 152 rows total - not the published defaults of 153 and 353, which is
why the walk is bounded by the chip rather than by the header), and 39 of those slots named something.  The
chip has three logical copy engines, two graphics devices, one encoder, one decoder, one secure engine and
two GSP entries, and no VIC at all - which is what the reports about this generation say, and here it is a
reading rather than a report.

What it is *not* is a place to submit work yet, and the same line says why: `window class 0x1100 clock
frozen` means the user-mode page the Volta and Turing manuals put at `BAR0+0x810000` answers with something
that is not a user-mode class and its clock does not advance, so there is no doorbell to ring and no
evidence a channel could exist.  `device registers: not read` on the detection half of the panel also
contradicted the module's 163 reads and now says whose reading is absent.  And because a decode whose field
positions came from a header and not a manual should not be taken on trust from a number, the report carries
the first copy engine's three rows verbatim (`LCE rows 0x…/0x…/0x…`), the count of entries the chip itself
flagged `IS_ENGINE`, and the bus-side devices (PBUS, HSHUB, HUBMMU, TMR) so that the tally adds up: devices,
engines and bus blocks are three different claims and only one of them is "this can be given work".

Those additions made the ordinary case longer than the 512-byte describe field on a populated chip, so the
field became 768 on both sides rather than the sentence being shortened until it stopped saying something -
the same choice made for notifications.  The host fixture now measures the *widest* report the format can
produce (153 slots, 353 rows, three copy engines, every named type, silent window) against that ceiling,
because a bound that only the friendly case stays under is a bound that clips a real card's worst day.

## Starting the coprocessor path: read the GSP, write nothing

Blackwell's engines are managed by the GSP - an on-card RISC-V coprocessor - and on this generation the
vendor's own driver reaches it only by booting a firmware image it does not ship in source.  Choosing that
path means the first thing worth knowing is not how to submit work but whether the coprocessor is reachable
and what state it is in, and unlike the copy-engine base, that is *published*:
`src/common/inc/swref/published/blackwell/gb100/dev_gsp.h` (fetched 2026-09-23, cached at
`build/nvdoc/gb100-dev_gsp.h`) gives absolute offsets inside the same PRI aperture the module already reads:
`NV_PGSP_FALCON_MAILBOX0/1` at `0x110040`/`0x110044`, `NV_PGSP_FALCON_ENGINE` at `0x1103c0` whose bits 10:8
are the reset status (0 asserted, 2 deasserted), `NV_PGSP_FALCON_IRQSTAT` at `0x110008` with `FATAL_ERROR`
at bit 24, and `NV_PGSP_RISCV_FAULT_CONTAINMENT_SRCSTAT` at `0x111700`.  Five reads, no writes, and the
result goes at the end of the same `Engine:` sentence:

    GSP engine out of reset, mailboxes 0x12345678/0x9abcdef0        (coprocessor up, words exchanged)
    GSP engine in reset, mailboxes 0x0/0x0, fatal error flagged     (held in reset; starting it is step one)
    GSP block at 0x110000 unreachable (engine reads 0xffffffff)    (nothing there to hand work to)
    GSP block at 0x110000 refused all five reads (0xbadf4100: …)   (a target the chip will not let this
        reader near - the fourth state, and the one a real Blackwell card produced; see the next section)

Two rules kept this honest while it was written.  An all-ones read is *no answer*, so the fatal and
containment bits are taken only from registers that replied - decoding bit 24 of `0xffffffff` would report a
fatal error on a block that never spoke, and the fixture now checks that case specifically, because a driver
that reads zeros out of an absent aperture will happily describe a coprocessor sitting in reset at address
zero.  And the report states the boundary rather than implying progress.  The next milestone on this path was
going to be intake, a firmware image the user stages and the loader verifies before anything uses it; the
section below is what actually happened to that plan, and why the intake is not the missing piece.  Measured
in the fixture as written: 93 checks, 0 failures, four of them about the coprocessor's three states and the
unmodelled-block case, all of them with `writes 0`.

## The coprocessor answered with an error code, which ended that path from this side

The first real card to run the probe above did not report a reset state.  It reported

    GSP engine reset status 0x01, mailboxes 0xbadf4100/0xbadf4100

which is two facts and one invention: five reads, all returning `0xbadf4100`, and a "reset status" decoded
out of them anyway.  The value is not register content.  NVIDIA's own driver names it, in
`src/nvidia/src/kernel/gpu/fsp/arch/hopper/kern_fsp_gh100.c` (fetched 2026-09-23, cached at
`build/nvdoc/kern_fsp_gh100.c`), where the function that decides whether the GSP may be touched at all reads
one word of `NV_PGSP` and compares it against a masked constant:

    const NvU32 privErrTargetLocked     = 0xBADF4100U;
    const NvU32 privErrTargetLockedMask = 0xFFFFFF00U; // Ignore LSB - it has extra error information

with the comment above it saying *"there is no HW mechanism for CPU to check if GSP is open other than reading
0xBADF41YY code"* and *"Until the programmed BAR0 decoupler settings are cleared, GSP access is blocked from
the CPU so all reads will return 0."*  So `0xbadf4100` is this silicon's answer to *you may not read that
target yet*, the low byte is error detail rather than data, and the vendor's driver treats it as something to
wait for rather than something to decode.  Three changes followed, and only the first is about formatting:

* `NV_PRI_IS_ERROR` / `NV_PRI_IS_DEAD` in `blackwell_regs.h`, with that provenance quoted in full, and a
  probe that counts how many of its five reads came back as refusals.  A block that refused is reported as a
  block that refused - `GSP block at 0x110000 refused all five reads (0xbadf4100: a PRIV target locked
  against this reader, not a register value, so no field of it is read)` - and no field of it is sliced up,
  which also retires the sentence `reset status 0x1` for good.
* The lock is per PRIV target rather than per BAR, so two more targets were asked the same read-only
  question: the FSP's four scratch words, which *are* published in absolute terms for this generation
  (`0x8f0320` in gb100's `dev_fsp_pri.h`, kept at `build/nvdoc/gb100-dev_fsp_pri.h`, and the same four words
  the vendor dumps when a GSP boot fails), and the first copy engine's own register block.
* That second one needed the unit of the table's 18-bit `DEV_DEVICE_PRI_BASE` field, which no source gives.
  It is not guessed: the GSP entry in the same table is scaled by each candidate shift and the unit is
  accepted only when one of them lands on the address `dev_gsp.h` publishes, which on the card in question
  means `0x1100 x 256 = 0x110000` and nothing else matches.  The report names the entry that decided it
  (`base unit measured on this chip: …`), and when no candidate matches it says so and asks the engine block
  *nothing at all* - the fixture proves that with a counter of reads at the address the unconfirmed scaling
  would have implied, because an unproven unit is a reason to stop, not a licence to probe.

Two old habits of this driver's reporting stayed mandatory while doing it.  The FSP's page lies past the
kernel's 8 MiB per-mapping cap, so it is mapped separately, exactly as the submission window is, and *could
not be mapped* is reported as this build's bound rather than as a chip that answered nothing; and the block
is only pointed at on a part whose v2 device table decoded, because that address is published for this
generation and no other.

What this rules out is worth stating as plainly as what it rules in, because it cancels a promise made in the
previous section.  Staging a firmware image cannot be the next step, and the reason is in the same function:
the code that releases the lock is the FSP, and the FSP is commanded by the vendor's boot sequence - a signed
descriptor payload, offsets in system memory, and a doorbell that gb100 publishes as one write-only register
(`NV_PFSP_MNOC_RX_FIFO_DATA`, `-W-4A`).  A `.bin` on the ESP changes none of that, so the intake was not
built: it would have been a file, a checksum and a sentence about how far the work got, in front of a card
that will not answer the door.  The measured ceiling on this generation is therefore that its engines are
reachable through a resource manager that boots the coprocessor, and that a driver which is not that
resource manager is told so by an error code.  If a card's `Engine:` line says instead that the LCE block
*answers*, that conclusion does not hold for that card and the register-level work resumes - the fixture
carries that case too, with the coprocessor still refusing beside it, so one live target cannot launder
another's refusal into a mood about the BAR.  The idle-GPU notice was re-worded in the same commit for the
same reason: it used to point at the panel as if a longer look would find a way in.

Measured: 122 host checks, 0 failures, five of them the refusal replayed as one machine's answers -
`GSP block … refused all five reads`, the unit cross-check, the engine block refusing the same way, the FSP
refusing, three mappings and no store - and the widest report the format can now produce is 888 bytes
against a describe field grown from 768 to 1152 for exactly these clauses.

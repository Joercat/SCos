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
gpu: module 2 module(s), 25936 B on the boot disk: 13536 B opened for this chip, 12400 B never read
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

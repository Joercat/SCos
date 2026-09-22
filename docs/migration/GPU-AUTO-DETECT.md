# GPU auto-detection and the driver registry, as implemented

Date: 2026-09-21. Status: **implemented and verified in this tree**. One family's engine
now exists as a loadable module (`docs/migration/GPU-DRIVER-MODULES.md`); the other
fourteen do not, and this document says exactly which half exists for which family, so the
distinction cannot be read as a general claim of acceleration. Nothing here infers support:
a family is named only when an upstream driver binds that exact ID, and an engine is used
only when it has read back the pixels it was told to paint.

`GPU-BASIC-2D-RESEARCH.md` (same directory) is the measurement of which existing
lightweight driver could be reused per family. This document is the first thing built on
top of that measurement: the part that can be built, tested and trusted before any
register-level port lands, because it is what decides *which* family's code SCos would
use on a given machine, and it must never guess.

## 1. What is in the tree now

| Path | Lines | Responsibility |
|---|---|---|
| `kernel/include/gpu.h` | 155 | device, family-match, capability and engine-ABI records; the boundary the compositor would call |
| `kernel/drivers/gpu/gpu_ids.h` | 1,291 (generated) | 1,019 exact `vendor:device` rules with chip names across 15 families, plus the measured per-family entry-point statuses |
| `kernel/drivers/gpu/gpu_match.c` | 74 | the matcher itself, shared verbatim by the kernel and the boot stub so both name the same family |
| `kernel/drivers/gpu/gpu_ports.c` | 80 | one record per family: port state, engine ops, and what stands between this family and GPU work |
| `kernel/drivers/gpu/gpu_detect.c` | 403 | enumeration, matching, scanout ownership, engine dispatch, report, boot log |
| `kernel/include/gpu_abi.h` | 202 | the on-disk module format and the one-way engine ABI |
| `kernel/drivers/gpu/gpu_module.c` | 762 | store index, validation, relocation, entry, readback self-test, refusal reasons |
| `drivers/gpu/ati/module.c` | 599 | the first module: Haiku's Rage128 2D engine, four documented deviations |
| `tools/research/gen_gpu_tables.py` | 473 | extracts the tables from a pinned upstream checkout; `--check` verifies them |
| `tools/build_gpu_module.py` | 458 | packs a family into `.MOD`, refuses imports and anything it cannot express; `--verify` re-reads the result |
| `tools/tests/gpu_detect_sim.c` | 364 | host harness: shipped detection sources against a simulated PCI bus |
| `tools/tests/test_gpu_detect.py` | 185 | the suite: table freshness, host harness, then QEMU with emulated adapters and a real module load |

`kernel/desktop/graphics.c` is now presentation only: it asks the GPU subsystem what it
found and prints it. `kernel/drivers/pci.c` gained a config-write counter, used as a
safety assertion (below) rather than as instrumentation.

## 2. The rule the tables enforce

Detection never invents support and never approximates it:

* a family binds only a `vendor:device` pair **that the upstream driver's own binding
  table lists**, extracted by `gen_gpu_tables.py` from `radeon/detect.c`,
  `nvidia/driver.c`, `ati/driver.cpp`, `intel_extreme/driver.cpp`,
  `radeon_hd/driver.cpp`, `matrox/driver.c`, `via/driver.c`, `neomagic/driver.c`,
  `s3/driver.cpp`, `3dfx/driver.cpp`, `intel_810/driver.cpp` and `et6x00/driver.c` at
  Haiku `7be0fef0`;
* rows inside `#if 0` are absent (`intel_extreme` 6, `radeon_hd` 119) and rows behind a
  line comment are absent (`matrox` Mystique `0x051A`, `via` `0x3344`), because in both
  cases the upstream driver cannot reach them — a table counted with `grep` alone
  overstates support, which is the mistake this generator exists not to repeat;
* the family's own class predicate is kept (`intel_extreme` also accepts subclass
  `0x80`; `radeon_hd` requires `0x03/0x00`; the older families test no class at all),
  and the vendor lives **in each row**, because nvidia's driver pairs four vendors —
  `0x10de`, `0x1048` ELSA, `0x12d2` STB/SGS-Thompson, `0x1888` Varisys — with four
  separate device lists;
* chip names are resolved through the two indirections upstream uses (`#define
  DEVICE_ID_RADEON_QD 0x5144`, and `static char sRage128_Pro_GL[] = "RAGE 128 PRO GL"`),
  so 1,019 of 1,019 rows carry a name rather than 240 of them;
* three families (VESA, the firmware framebuffer, virtio) have **no PCI ID table at all**
  upstream and are recorded that way: they can be reported as the fallback path, never
  matched by ID.

Duplicate rows are collapsed for matching and the count is kept in the header
(`radeon`: "3 duplicate rows collapsed" — `0x554d`, `0x554f`, `0x5b62` appear twice with
different flags, which matters when porting the engine and not at all when matching).

## 3. What must not happen during detection, and how that is proven

Taking over a display is where an OS loses its console, so the constraints are enforced
by construction and then asserted at run time:

* identification reads configuration space only — BAR values are read but **never
  sized** (sizing writes `0xffffffff` first), no reset, no power transition, no clock or
  mode register, and `pci_memory_bar()` is deliberately not called for display functions;
* `kernel/drivers/pci.c` now counts every config write in the kernel, `gpu_init()`
  snapshots that counter, and a non-zero delta logs `gpu: BUG: ...`;
* the scanout owner is identified purely by comparing BAR addresses with the framebuffer
  the firmware validated, which is enough to know which function feeds the console
  without touching it;
* `graphics_report()` prints `config writes during detection: 0`, so the invariant is
  visible on a machine with no keyboard attached instead of being implied by a review.

## 4. Verification actually run

`python3 tools/tests/test_gpu_detect.py`, three layers:

1. **Table freshness** — `gen_gpu_tables.py --check` re-extracts from the pinned Haiku
   checkout and byte-compares: `PASS: ... matches a fresh extraction from 7be0fef07df0
   (1020 device IDs across 16 families, one of them hand-authored: see
   `GPU-DRIVER-MODULES.md`)`. A hand-edited or stale table fails the suite.
2. **Host harness** — the three shipped `gpu_*.c` units are compiled unchanged for the
   host and run against a simulated bus. It derives its cases from the tables instead of
   hard-coding IDs: **24 positive cases** (the oldest and newest ID of each of the 12
   families that have a table, expecting the family *and* the chip name), 5 negative
   cases (Rocket Lake `8086:4c8b`, RDNA3 `1002:7479`, Bochs `1234:1111`, GA102
   `10de:2230`, and an `intel_extreme` ID at subclass `0x02`), plus the class-predicate
   counterpart (the same ID at subclass `0x00` *does* match), two-adapter scanout
   ownership, per-family port-record coverage, and that no record claims a state without
   ops. Result: `harness: all detection checks passed`, with the config-write count at
   zero verified against a simulated bus that would report any write.
3. **QEMU with real emulated PCI display functions** — the built image boots with
   `ati-vga`, `cirrus-vga` and `virtio-vga` attached beside the boot device. From the
   guest's own serial log:

   ```
   gpu: PCI 0:1.0 1234:1111 sub=0 matched=none
   gpu: PCI 0:3.0 1002:5046 sub=0 matched=ati
   gpu:   chip=RAGE 128 PRO GL engine=none, CPU compositor
   gpu: PCI 0:4.0 1013:b8 sub=0 matched=none
   gpu: PCI 0:5.0 1af4:1050 sub=0 matched=none
   gpu: 4 display function(s), 1020 ID rule(s) in 16 family record(s), 0 without a port record
   gpu: scanout owner identified by BAR address, 0 PCI config write(s) issued
   
   A few lines later the same boot loads that family's driver from disk, and says so in the
   order a reader needs to trust it:

   ```
   gpu: module 2 module(s), 25936 B on the boot disk: 13536 B opened for this chip, 12400 B never read
   gpu: module ati (8360 B code+data, 376 B zeroed, 288 reloc, 47 id rule(s)) validated
   gpu: module test surface: aperture of this function at 0x80000000+3145728, 1024x768 stride 1024
   gpu: ati: Rage128 GUI engine ready
   gpu: module ati bound: engine verified in device memory (225/225 pixels)
   gpu: engine drives its own aperture only: another function feeds the console, so output stays on the CPU
   ```

   Those last two lines are the point of the module layer and the reason this document keeps
   detection and driving apart: a bound engine on a function that does not feed the console is
   real, verified, and correctly unused. `GPU-DRIVER-MODULES.md` records that machinery and every
   defect found getting it to this point.
   ```

   `0:3.0` is the interesting line: QEMU's emulated Rage 128 is matched to the `ati`
   family **by the ID Haiku's `ati` driver binds**, with no special case in SCos for it.
   The suite also asserts `gpu_bound_driver()` is null (nothing may claim an engine while
   no engine is ported), that the report text is right for both the matched and the
   unmatched cases, and that the painted desktop is byte-identical across repaints and
   non-blank after enumeration — the console surviving being looked at.

### What emulation can and cannot show here

* QEMU 11.0.2 (the pinned bundle) has **no property to re-identify a PCI device**
  (`-device help` exposes no `x-pci-vendor-id`/`x-pci-class-code`), so it cannot present a
  Radeon, Matrox G450, i810, S3 Savage or Tseng card. Only one QEMU display device has an
  ID that appears in an upstream table — `ati-vga` — which is why that is the positive
  case and the rest are covered by the host harness on the same source.
* `-device bochs-display` cannot be attached at all: with a second std-style framebuffer
  present, OVMF stops booting from disk. Verified, not assumed (single-device runs of
  `ati-vga`, `cirrus-vga` and `virtio-vga` boot; every combination including
  `bochs-display` did not), and the test documents it rather than working around it.
* No QEMU device can show a register-level blitter doing the compositor's work, because
  no QEMU device models the engines of the families in `GPU-BASIC-2D-RESEARCH.md` §3.
  Nothing in this layer claims otherwise.

## 5. The report, as a user sees it

Captured from the emulated four-adapter run (QEMU, `ati-vga` attached), then abbreviated:

```
Renderer: CPU software compositor
Scanout: firmware GOP, PAT write-combining
GPU acceleration: unavailable (no hardware backend linked)
Display: 1024x768
GPU detection (exact device matching; no BAR sizing, no modeset)
Families named from upstream tables: 16, device ID rules: 1020
(one record, `cirrus`, is hand-authored for SCos' own module rather than read out of an upstream driver)
Naming rows (see §9, `gpu_ids_registry.h`): 1296 more ids name a chip and bind nothing
- Intel 8086:4c8b at 0:2.0 (scanout)
  match: none - no upstream table binds 8086:4c8b; treated as an unmatched display adapter
Detection is not driver support: no BAR sizing, GPU reset or modeset was performed (config writes during detection: 0).
Scanout owner 0:2.0 identified from its BAR address alone.
```

A matched adapter gains the family, the chip name, the upstream entry-point statuses,
`SCos port: not ported yet` - or, when the family's module is what bound, that a module was
loaded from storage and verified by readback - and one line saying what stands between that
family and GPU work, as in the captured run above (`family: ati, chip: RAGE 128 PRO GL, source:
src/add-ons/kernel/drivers/graphics/ati/driver.cpp`). On the i5-11400 with its Rocket
Lake UHD 730 the same lines read `8086:4c8b … match: none - no upstream table binds
8086:4c8b`, which is the truth rather than a gap in the tooling: no light driver's table
contains that ID, and SCos will not pretend otherwise.

## 6. What is deliberately not here

* No engine code. `ops` is null for all 15 families, so `gpu_engine_*()` always returns
  `-1` and every drawing operation stays on the CPU path. Nothing falls back silently:
  the state is printed.
* No device-specific quirk tables, no "close enough" matching by vendor, no class-code
  inference, and no attempt to bind a family whose upstream table lacks the ID.
* No runtime module loading. Each family is its own translation unit, which is what makes
  per-family selection possible at all, but the selective *loading* requirement still
  waits on the disk-backed file work; the tables are read-only metadata, measured as
  18,743 bytes of `.text`-segment constants plus 17,280 bytes of tables in
  `build/gpu-gpu_tables.o`, i.e. ~36 KB for all 1,019 rows and 15 family records, and
  ~49 KB for the whole subsystem - so boot cost is negligible and there is nothing to
  stream yet.

## 7. Next unit of work, in order

1. Port the `ati` family's measured 496-line engine (`accelerants/ati/engine.cpp` +
   `rage128_draw.cpp`) against the driver this layer already names, and run it under
   QEMU's `ati-vga` — the one family where a blit can be checked here end to end, by
   reading back the framebuffer QEMU renders. Then gate 4 of the research document on a
   real card.
2. Add the display tier for `intel_extreme` only with the three substrate pieces the
   research document measures as missing (WC mapping for a driver-owned scanout, PCI
   interrupt routing, an aperture allocator), plus the target chip's ID — the ID still
   has to be read from the physical machine.
3. Everything else stays on the CPU path with the reason printed, which the `graphics`
   report already does.

## 8. Provenance and licence of the extracted data

The generated header is derived data, not code: PCI IDs and chip-name strings from Haiku
`7be0fef0` driver sources. Those files carry MIT (`ati/driver.cpp`, `radeon/detect.c`,
`intel_extreme/driver.cpp`), the Be Sample Code License (`nvidia/driver.c`,
`matrox/driver.c`, `via/driver.c`, `neomagic/driver.c`) or an MIT-style grant
(`et6x00/license`). No file was copied; the extraction tool is in the tree, so the header
can be reproduced or re-derived at a different commit. `third_party/README.md` records
the same provenance for the driver code that a later port would reuse.

## 9. Naming rows: what an id registry adds, and what it must not

§2's rule is that a row in `gpu_ids.h` means "the upstream driver for this family binds this chip".
That rule cannot be stretched: the table is Haiku's own, and its Nvidia half stops in 2007, so
without a second source the OS cannot even say what a 2025 GPU is.  The registry supplies names, not
claims, and it is therefore a **separate generated table** - `kernel/drivers/gpu/gpu_ids_registry.h` -
that no driver-authorising code reads.

| | rows | meaning |
|---|---|---|
| `gpu_ids.h` | 1020, in 16 family records | a driver in this tree binds that id, plus one hand-authored record (`cirrus`) that its own module binds |
| `gpu_ids_registry.h` | 1296 (nvidia 823, radeon_hd 338, intel_extreme 135) | this id belongs to this chip, and nothing is claimed about driving it |

The log keeps them apart instead of reporting one big number:

```
gpu: 4 display function(s), 1020 ID rule(s) in 16 family record(s), 0 without a port record
gpu: tables: 1020 id rules bind a driver; 1296 more ids name a chip that nothing in this tree covers
gpu: PCI 0:1.0 10de:2d83 sub=0 matched=nvidia
gpu:   chip=GB207 [GeForce RTX 5050] (Blackwell) engine=none at detection, CPU compositor
gpu:   10de:2d83 is named by the PCI id registry only; no driver table in this tree binds it, so
```

### What enforces the difference

* `gpu_match_device()` tries the driver tables first and the naming table only after they miss, so a
  chip that a driver binds is never re-described as merely known.
* The winning row's provenance is asked, not guessed: `gpu_match_row_is_registry()` decides by which
  array the row lives in, `struct gpu_device` carries `named_only`, and `gpu_module_eligible()` is the
  single predicate that says whether a module may be read for a function at all - a naming row fails it,
  so nothing is loaded, nothing is bound, and the in-tree port lookup in `identify()` returns before
  it could claim the chip.
* The UEFI stub asks the same question before it reads a file, so firmware does not hand the kernel
  bytes for a generation nothing can drive.
* `gpu_detect_sim.c` asserts both sides of that boundary.  Five named rows - `10de:2d83` RTX 5050,
  `10de:2b85` RTX 5090, `10de:1b06` GTX 1080 Ti, `1002:744c` RX 7900 XTX, `8086:56a1` Arc A750 - must
  come back matched by family, marked `named_only`, carrying the registry's own name, unbound and
  module-ineligible; every row of every driver table must come back *not* marked; and the negative cases
  re-derive themselves from the tables, so a snapshot refresh cannot leave a stale "this id is unknown"
  assertion behind (that is how `8086:4c8b` came to move from the unknown list to the named list).

### Where the rows come from, and what it costs

`tools/research/pci.ids.display.txt` holds 2320 display-product rows extracted from `pci.ids`
(PCI ID Project, version 2026.09.21; licence GPL-2.0-or-later OR BSD-3-Clause), for vendors
10de/1002/1022/8086.  Companion functions are dropped, because the registry lists `GB202 High
Definition Audio Controller` next to the chip itself and an audio function must not land in a display
table; `Reserved`/`Unknown` placeholders are dropped, because naming a chip "Reserved Dev ID B" would
be a fake; and one row survives per distinct name, at the lowest id carrying it, since desktop,
Max-Q, refresh and OEM SKUs of the same product differ in nothing a driver would care about and the
table is a lookup, not a shop window.  Nvidia rows carry the generation in the name (`(Blackwell)`)
because the registry's leading codename maps to it and the generation is exactly what decides whether a
driver could exist; ids whose codename is unknown get no suffix rather than a guess.

The scanout side is unaffected: detection still reads only, and the naming table is consulted for
functions the scanner has already accepted as class 0x03 display devices, so a misfiled id could never
make the OS touch a non-display function.

Cost, measured rather than estimated: `build/kernel.elf` goes from 710,736 to 809,480 bytes
(+98,744, +13.9%), which is the price of naming ~1,300 chips.  That is a real cost and it is paid on
purpose: the alternative is a machine that reports "unknown device" about the GPU it is running on.
A refresh costs `--extract-pci-ids` plus `--registry-only` (a few hours of thought, no network at build
time, no Haiku checkout needed), and both are verified offline by `test_gpu_detect.py`.

### What this does not do for an RTX 5050

It is worth stating plainly, because a name on a report is easy to misread as support.  For Turing and
later there is no basic 2D engine to port: NVIDIA's own open kernel modules support "Turing (TU10x) or
later" and require GSP firmware for every supported architecture, with exact version matching between
kernel module, firmware and userspace; nouveau's GSP path is off by default except on Ada "where it's
the only option"; and Haiku's Nvidia port likewise begins at Turing because older cards lack the GSP
microcontroller.  NVIDIA dropped its open `xf86-video-nv` 2D driver at Fermi, and Haiku's `nvidia`
driver states that "GF 8xxx and later cards will not be supported by this driver as their architecture
is quite different from before".  So on this machine the truth is: the chip is identified, the console
is the framebuffer its firmware set up, 2D is composited by the CPU, and no module is read - and the
report says each of those things rather than leaving them to be inferred.


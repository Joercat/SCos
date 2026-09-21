# GPU auto-detection and the driver registry, as implemented

Date: 2026-09-21. Status: **implemented and verified in this tree**; the driver *code*
that does GPU pixel work is not ported yet, and this document says exactly which of the
two halves exists so the distinction cannot be read as a claim of acceleration.

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
| `kernel/drivers/gpu/gpu_tables.c` | 27 | sole owner of the generated tables; the only accessor surface |
| `kernel/drivers/gpu/gpu_ports.c` | 80 | one record per family: port state, engine ops, and what stands between this family and GPU work |
| `kernel/drivers/gpu/gpu_detect.c` | 373 | enumeration, matching, scanout ownership, engine dispatch, report, boot log |
| `tools/research/gen_gpu_tables.py` | 473 | extracts the tables from a pinned upstream checkout; `--check` verifies them |
| `tools/tests/gpu_detect_sim.c` | 340 | host harness: shipped detection sources against a simulated PCI bus |
| `tools/tests/test_gpu_detect.py` | 149 | the suite: table freshness, host harness, then QEMU with emulated adapters |

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
   (1019 device IDs across 15 families)`. A hand-edited or stale table fails the suite.
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
   gpu: 4 display function(s), 1019 ID rule(s) in 15 family record(s), 0 without a port record
   gpu: scanout owner identified by BAR address, 0 PCI config write(s) issued
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
Families named from upstream tables: 15, device ID rules: 1019
- Intel 8086:4c8b at 0:2.0 (scanout)
  match: none - no upstream table binds 8086:4c8b; treated as an unmatched display adapter
Detection is not driver support: no BAR sizing, GPU reset or modeset was performed (config writes during detection: 0).
Scanout owner 0:2.0 identified from its BAR address alone.
```

A matched adapter gains the family, the chip name, the upstream entry-point statuses,
`SCos port: not ported yet` and one line saying what stands between that family and GPU
work, as in the captured run above (`family: ati, chip: RAGE 128 PRO GL, source:
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
  waits on the disk-backed file work; the tables are read-only metadata (1,019 rows is
  about 32 KB of `.rodata`), so boot cost is negligible and there is nothing to stream yet.

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

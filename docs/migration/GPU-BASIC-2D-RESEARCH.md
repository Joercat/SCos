# Basic GPU rendering: existing lightweight 2D/scanout drivers, measured

Date: 2026-09-20, updated 2026-09-21. Status: **research**; no driver code has been
imported, ported, built, or executed by this document. The part of it that could be built
without a register-level port — exact per-family auto-detection, the driver registry and
the capability record — is now implemented and verified, and is described in
[`GPU-AUTO-DETECT.md`](GPU-AUTO-DETECT.md) with its measurement tooling; that work added
generated device tables extracted from the upstream drivers named below, and bound no
engine. Nothing in this document should be read as claiming acceleration on any card.

## 1. Scope, as corrected

The request is *not* the vendor "full power" stack. It is the basic part: a driver that
owns the display output and lets the **GPU move pixels** (copy, fill, flip) so the CPU
stops doing every compositor blit by hand, with as much of that as a small reused
driver can honestly provide. Full GL/Vulkan, shaders, video encode and 3D are out of
scope, and this document says which of those actually matter for the browser.

This narrows the previous study (same directory, `GPU-DRIVER-RESEARCH.md`): Mesa,
NVIDIA GSP, and FreeBSD DRM are no longer candidates because they exist to deliver 3D.
Reproducible numbers below come from
`python3 tools/research/audit_basic_2d_sources.py --cache DIR --fetch` written to
[`GPU-BASIC-2D-SOURCE-AUDIT.json`](GPU-BASIC-2D-SOURCE-AUDIT.json) against pinned
checkouts of Haiku `7be0fef0`, Linux `93f51579`, Genode `0f275e7a`,
libgfxinit `d49b56ba`, coreboot `1879b6a3`. The display-side half is measured by
`python3 tools/research/probe_haiku_display_features.py --haiku DIR`, written to
[`GPU-BASIC-2D-DISPLAY-FEATURES.json`](GPU-BASIC-2D-DISPLAY-FEATURES.json). The probe
needs only the Haiku checkout, so it re-runs offline against the pinned copy under
`/home/user`; the 2D audit re-runs offline only for the repositories still cached there
(Haiku, Genode, libgfxinit) and needs `--fetch` — and therefore working GitHub access —
for the Linux and coreboot size references.

## 2. What "basic" actually means, in SCos terms

Today `kernel/desktop/fb.c` renders into a RAM canvas and `fb_flip_rect()` copies the
damaged rectangle to the firmware GOP framebuffer with `rep movsl` (plus per-pixel
component conversion if the mode is not native X8R8G8B8). That copy is the work a
"basic" driver removes. So the useful primitive set is exactly four operations:

- `SCREEN_TO_SCREEN_BLIT` (window moves, scroll, damage copies),
- `FILL_RECTANGLE` (backgrounds, clears),
- `INVERT_RECTANGLE` (selection feedback),
- `FILL_SPAN`, plus engine acquire/release, `WAIT_ENGINE_IDLE`/sync tokens, and a
  retrace/vsync hook for tear-free presentation.

And there is a second half to "basic" that is easy to overlook because it is not a
drawing engine at all: the *display* operations the same accelerants expose —
`SET_DISPLAY_MODE`/`GET_MODE_LIST` (native timing instead of whatever GOP chose),
`GET_EDID_INFO`, `ACCELERANT_RETRACE_SEMAPHORE` (a real vsync), `MOVE_DISPLAY`
(reposition the scanout start, i.e. scroll without touching a pixel),
`SET_CURSOR_SHAPE`/`MOVE_CURSOR` (cursor drawn by hardware), `SET_DPMS_MODE`.
Those are per-family and separately advertised, so both halves are measured below
rather than assumed.

Anything beyond that (triangle setup, shaders, samplers, GEM-style buffer pools,
Vulkan submission) is a different, much larger project.

## 3. The lightest real candidates are Haiku's classic accelerants

Measured closure per family — kernel driver + userspace accelerant + private register
headers together, because that is what a port actually needs; `2D core` is only the
files that contain the blit/fill/ring code:

| Family | Closure files | Closure lines | 2D core lines | Advertises `B_2D_ACCELERATION` | Hardware era |
|---|---|---|---|---|---|
| `radeon` | 78 | 21,092 | 1,173 | yes | R100 → R480 (Radeon 7000 → X850, XPRESS 200), plus RV370/RV380 IDs marked "new" (0x5960–0x5965, 0x5B60, 0x5B63) |
| `nvidia` | 34 | 23,690 | 3,949 | yes | NV04 → NV4x (RIVA TNT → GeForce 6/7) |
| `matrox` | 32 | 13,387 | 496 | yes | G100 / G200 / G400 / G450 / G550 (no Parhelia) |
| `via` | 32 | 13,116 | 270 | yes | VIA IGPs: 0x3108, 0x3122, 0x3205, 0x3344, 0x7205 (KM400/P4M/CX700 class) |
| `neomagic` | 27 | 7,811 | 455 | yes | MagicGraph 128 → MagicMedia 256XL+ (NM2070–NM2380) |
| `s3` | 30 | 7,039 | 710 | yes | Trio64 (0x88xx), ViRGE (0x8Axx), Savage (0x8Cxx) |
| `ati` | 24 | 6,556 | 496 | yes | Mach64 (0x4742–0x4750) and Rage 128 |
| `3dfx` | 17 | 3,189 | 153 | yes | Banshee, Voodoo 3, Voodoo 5 (0x0003/0x0005/0x0009) |
| `et6x00` | 18 | 2,446 | 258 | yes | Tseng ET6000/ET6100/ET6300, vendor 0x100C — its own readme caps modes at 1024x768 |
| `intel_810` | 12 | 2,425 | 83 | yes | i810/i815 |
| `intel_extreme` | 35 | 14,793 | 377 | **no** (`0 /*B_2D_ACCELERATION*/`) | 2D path is Gen2–Gen8 era and disabled on Skylake+; the **display** path of the same driver binds 143 distinct device IDs from `0x3577` i830GM to `0x46d1` "Alder Lake-N" (see section 4) |
| `radeon_hd` | 55 | 32,060 | 361 | **no** | R600 → GCN, display/AtomBIOS only |
| `virtio` | 8 | 1,920 | – | no engine | virtio-gpu 2D (virtual) |
| `vesa` | 19 | 3,125 | – | no engine | x86 real-mode VESA BIOS services; not usable on UEFI-only SCos |
| `framebuffer` | 11 | 1,025 | – | none | plain scanout, no device-specific init |

Notes that matter for reuse:

- The engine code is genuinely small and self-contained: `radeon/Acceleration.c`
  (516 lines) builds CP packet-3 `CNTL_BITBLT_MULTI` / `CNTL_PAINT_MULTI` command
  batches and `radeon/CP.c` (408 lines) runs the indirect buffer;
  `intel_extreme/engine.cpp` (377 lines) emits `XY_SRC_COPY_BLT` / `XY_COLOR_BLT` into
  a ring; `nvidia/engine/nv_acc.c` + `nv_acc_dma.c` (3,787 lines) drive the NVIDIA
  `PUSH`/DMA blitter across chip generations; `et6x00/Acceleration.c` (257 lines) does
  the same for Tseng. This is precisely the "light enough to edit" class.
- These drivers do their **own** mode setting (`SetDisplayMode`/CRTC programming, EDID
  parsing, PLL tables), so they do not depend on a BIOS call or on GOP — good for a
  UEFI-only machine. But each of the four capabilities — 2D, vsync, cursor, overlay — is
  advertised per family and sometimes per chip, so all of them are read from the hook
  tables rather than assumed. Measured with
  [`GPU-BASIC-2D-DISPLAY-FEATURES.json`](GPU-BASIC-2D-DISPLAY-FEATURES.json), which
  counts each family's own binding table (tracking `#if 0` regions, because a commented
  row is not a supported chip) and classifies each hook by the text that returns it:

  | Family | IDs bound by its own table | rows inside `#if 0` | `B_2D_ACCELERATION` | vsync/retrace | `MOVE_DISPLAY` (pan) | HW cursor | overlay |
  |---|---|---|---|---|---|---|---|
  | `radeon` | 0 row-form (147 raw `DEVICE_ID` literals) | 0 | yes | direct | direct | disabled-by-comment | direct |
  | `nvidia` | 0 row-form (239 raw literals) | 0 | yes | direct | direct | conditional | direct |
  | `matrox` | 0 row-form (10 raw literals) | 0 | yes | direct | direct | conditional | direct |
  | `via` | 0 row-form (6 raw literals) | 0 | yes | direct | direct | conditional | direct |
  | `neomagic` | 0 row-form (9 raw literals) | 0 | yes | direct | direct | conditional | direct |
  | `s3` | 34 (`0x8811` Trio64 → `0x9102` Savage2000) | 0 | yes | direct | direct | direct | absent |
  | `ati` | 34 (`0x4742` 3D RAGE PRO → `0x4D4C` RAGE 128 Mobility) | 0 | yes | direct | direct | direct | direct |
  | `3dfx` | 0 row-form | 0 | yes | denied | direct | direct | direct |
  | `et6x00` | 0 row-form (2 raw literals) | 0 | yes | disabled-by-comment | absent | absent | absent |
  | `intel_810` | 4 (`0x7121` i810 → `0x1132` i815) | 0 | yes | denied | direct | absent | absent |
  | `intel_extreme` | **143** (`0x3577` i830GM → `0x46d1` "Alder Lake-N") | **6** | no | direct | direct | gated | denied-for-some-devices |
  | `radeon_hd` | **340** (`0x94c7` "Radeon HD 2350" → `0x73ff` "Radeon RX Navi (Dimgrey)") | **119** | no | direct | absent | absent | absent |
  | `virtio` / `vesa` / `framebuffer` | 0 (class/virtual or no device table) | 0 | no engine | direct | vesa only | vesa only | vesa only |

  One further basis note, from generating the tables themselves: this matrix counts a
  row when it carries a *string literal* name, while a driver can also name a chip
  through a variable (`static char sRage128_Pro_PF[] = "RAGE 128 PRO GL"`, referenced by
  `ati`'s rows), which a binding loop still honours. Resolving those indirections raises
  `ati` from the 34 rows above to the 77 device IDs SCos now matches by, which is why the
  generated header in `kernel/drivers/gpu/gpu_ids.h`, not this table, is the matching data.

  The two ID counts are different bases on purpose: a *row-form* table is
  `{ 0xNNNN, group, "chip name" }`, which the tool can read end to end, while the other
  families list IDs as bare `uint16` arrays (`static uint16 nvidia_device_list[] =
  {0x0020, …}`) or `#define DEVICE_ID_…` macros with no names, so those are counted as
  distinct literals. The column therefore shows relative breadth and the oldest/newest
  named chip where names exist, and it is never a claim that a given card works.

  "conditional" is the `HRDC(x)` macro (`nvidia`/`matrox`/`via`/`neomagic` hand out the
  cursor hook only when the `hardcursor` setting is on), "denied" is `return NULL`, and
  "disabled-by-comment" is a commented-out `return` — `radeon`'s cursor hooks sit behind
  `// TODO: fix`, and `et6x00`'s retrace hook behind `//`. `s3` dispatches the cursor
  hook directly but gates inside the function on `bDisableHdwCursor`, which is why the
  matrix reports the *dispatch* decision and the per-file evidence is quoted in the JSON.
- Their 2D hooks are no longer driven by Haiku's real display path. Measured directly:
  code search over the pinned tree returns zero hits for `_fill_rtlef`,
  `engine_copy_area` and `ACCELERANT_INVERT_RECTANGLE`; the driver-backed
  `src/servers/app/drawing/HWInterface.cpp` (1,106 lines) resolves the accelerant but
  calls no engine blit (only cursor state such as `fHardwareCursorEnabled`); the
  accelerants still export the hooks (`FILL_RECTANGLE`, `SCREEN_TO_SCREEN_BLIT`,
  `INVERT_RECTANGLE`, `FILL_SPAN`, engine acquire/release). The only in-tree callers of
  `screen_to_screen_blit` outside the driver/accelerant pairs are Haiku's own drawing
  tests, the game kit, and the *virtual* direct-window interface
  (`src/servers/app/drawing/interface/virtual/DWindowHWInterface.cpp`), which keeps
  `fAccFillRect`/`fAccInvertRect`/`fAccScreenBlit` hook slots for its non-GPU path. So
  this is real, complete hardware code that upstream keeps but no longer exercises on a GPU — no "works in current Haiku"
  claim is available, and none is made here.
- Licensing is workable but not uniform: Haiku as a whole is MIT, and each driver keeps
  its own notices (`radeon/Acceleration.c` — Thomas Kurschel 2002; `nvidia/engine` —
  Rudolf Cornelissen, derived from the XFree86 NV driver; `matrox/engine/LICENSE` —
  "freely copy them … do not claim you wrote them"; `et6x00/license`). Per-file notices
  must be preserved in any port.

## 4. Why "light" and "modern GPU" only partly combine

Measured size of what modern silicon actually requires:

| Path | Measured | What it buys |
|---|---|---|
| Linux `drivers/gpu/drm/i915` | 903 files, **424,291 lines**; `display/` alone 344 files / 6.31 MB | real Intel display + GT + GEM |
| Linux `drivers/gpu/drm/amd` | 2,909 files, 6,513,491 lines | real AMD (amdgpu + DC) |
| Linux `drivers/gpu/drm/nouveau` | 1,212 files, 227,337 lines | real NVIDIA |
| Linux `drivers/gpu/drm/radeon` | 200 files, 195,372 lines | R300–GCN, still huge |
| Genode Intel GPU session | 17 files, 9,513 lines | buffers/submission for Mesa; **no** copy/fill engine |
| Genode Intel framebuffer | 13 files, 6,754 lines of glue, but its `source.list` compiles **932 Linux files, 531 from `i915/`, 304 from `i915/display/`**, GPL-2.0 | proves the "small" impression was misleading |
| libgfxinit (Ada/SPARK, coreboot) | 118 files, 18,814 lines; configs stop at `skylake`/`broxton` | Intel modesetting only, no drawing engine, no Rocket Lake |
| coreboot Intel SoC `graphics.c`/`gma*` | 15 files, 2,087 lines (broadwell/skylake/ADL) | boot-display init only; Tiger Lake carries 307 bytes, i.e. "ask firmware/GOP" — SCos's current situation |
| `drivers/gpu/drm/tiny` | 15 files, 9,549 lines | small because these are *simple display controllers*, not PC GPUs |

For the target machine's integrated graphics — a Rocket Lake UHD 730 on the i5-11400,
exact PCI ID still to be read from the hardware — the *drawing* half of section 3 does
not apply: `intel_810` stops at i815, `intel_extreme`'s BLT path is Gen2–Gen8 era, and
libgfxinit stops at Skylake. There is no small upstream driver that performs
`XY_SRC_COPY_BLT`-class work on Gen9 and later, and the reason is stated upstream rather
than inferred: `intel_extreme/engine.cpp:29` declares
`static engine_token sEngineToken = {1, 0 /*B_2D_ACCELERATION*/, NULL}`, and
`intel_wait_engine_idle()` returns immediately for the Lake families behind the comment
at `engine.cpp:235` — `// Skylake acc engine not yet functional (stalls)`.

The *display* half is a different story, and this is the phase's most useful finding.
An earlier draft of this section claimed `intel_extreme` "stops at i965 (2002–2007)";
that was wrong and is corrected here from the driver's own binding table:

- `intel_extreme`'s `kSupportedDevices[]` has **143 distinct device IDs, from `0x3577`
  i830GM to `0x46d1` "Alder Lake-N"**, with `INTEL_MODEL_SKY/KBY/CFL/CML/JSL/TGLM/ALDM`
  groups active and only 6 rows left inside `#if 0`. The register blocks are
  generation-remapped (`REGS_NORTH_PLANE_CONTROL` etc. in
  `headers/private/graphics/intel_extreme/intel_extreme.h:150-155`, consumed by
  `mode.cpp:131-163`), and Gen9+ specifics are real code, not stubs: Skylake DPLL
  programming in `Pipes.cpp:556-622`, `TigerLakePLL.cpp` (345 lines), `ICL_PWR_WELL_*`
  and `TGL_DPCLKA_CFGCR0` in `Ports.cpp:602,2741`, "on Skylake and later, timing is
  already done in `ConfigureTimings()`" (`Pipes.cpp:196`).
- Its kernel driver installs **Gen8/Gen11 vblank interrupts** with MSI when the platform
  offers it (`kernel/drivers/graphics/intel_extreme/intel_extreme.cpp:88-158,445-458`),
  and publishes `intel_accelerant_retrace_semaphore` to the accelerant
  (`accelerants/intel_extreme/hooks.cpp:32`).
- It exposes `B_MOVE_DISPLAY` → `intel_move_display` (`mode.cpp:769`), which validates
  against `mode.virtual_width/virtual_height` and reprograms the scanout start —
  hardware scroll with no pixel traffic at all. `radeon` does the same thing at
  `crtc.c:148,176`.
- It exposes the front-buffer base programming the same code path needs
  (`set_frame_buffer_registers()` writing `INTEL_DISPLAY_A_SURFACE` from
  `shared_info.frame_buffer_offset`, `mode.cpp:150-156`), i.e. scanout of a
  driver-chosen buffer rather than the firmware's.
- Licences here are the cleanest in the whole survey: every `intel_extreme` file
  measured carries the MIT header (28 of them), unlike the XFree86-derived engine code.

What it does **not** buy on a Gen9+ chip, each with the exact reason:

- GPU copy/fill: absent by upstream's own admission (quoted above), and
  `hooks.cpp:114` returns `NULL` for `B_FILL_SPAN` outright.
- The hardware cursor path is the pre-Gen9 programming model — `cursor.cpp:15-60`
  writes `INTEL_CURSOR_BASE` with a physical aperture address and accepts only a
  64×64 two-colour AND/XOR bitmap — so using it on Skylake-and-newer is a generation
  mismatch to fix, not to assume; Haiku's own internals page says the same thing in
  general ("Intel requires some code changes between generations, to adjust for changes
  in the register map"), and its hardware wiki rates Intel as "Supported, but rough",
  with SandyBridge and older well supported and newer parts "a bit hit or miss"
  ([Haiku's own Intel hardware table](https://dev.haiku-os.org/wiki/HardwareInfo/video/Intel),
  whose mode-switch/cursor/2D/3D columns are exactly the four capabilities this survey
  measures per family).
- Overlay is `return NULL` for a long list of groups with the comment
  `// TODO: overlay doesn't seem to work on these chips` (`hooks.cpp:125-133`).
- Rocket Lake itself is **not in the table** (no `0x4c8b`/`0x4c8e`/`0x4c90` anywhere in
  the driver, checked by grep), so binding SCos's iGPU means adding an ID plus trusting
  the Gen12 display paths — a bounded edit to reused code, but one whose correctness
  only the real machine can settle.
- SCos' side is not ready for any of it yet, measured here rather than guessed:
  `mmio_map()` maps an arbitrary device range only **uncached** (`kernel/src/memory.c:158`
  with `map_physical(..., device=1)` forcing `PWT|PCD`, `memory.c:143-157`) and PAT slot 1
  (WC) is reserved for the *validated GOP pixel memory* (`memory.c:71-72`), so a
  write-combined scanout/aperture mapping is new code; interrupts are 8259-only with no
  `_PRT` or MSI plumbing anywhere in `kernel/` (`kernel/src/interrupt.c:43-44,72-77`,
  `grep -rn "_PRT\|interrupt_line\|msi" kernel/drivers/` returns nothing), so a vblank
  handler has no IRQ to attach to — note that upstream itself degrades to a timer-faked
  vblank when no interrupt line exists (`intel_extreme.cpp:559-572`); and there is no
  aperture/GGTT allocator, which Haiku gets from its AGP/GART bus manager through
  `gGART->map_aperture` / `gGART->allocate_memory` (`intel_extreme.cpp:586-608`), a
  service outside the surveyed `add-ons` tree and therefore unmeasured here.

So the structural limit is narrower than "light and modern don't mix": on a modern iGPU
the light tier can own **scanout, native timing, vsync, panning and a hardware cursor**,
while **fills and copies stay on the CPU** — and every one of those has a concrete
SCos-side prerequisite listed above. On GPUs before Gen9, or on the pre-2007 AMD/NVIDIA
families, the tier can additionally own the copies and fills.

## 5. Where a basic accelerator *can* be won today

**Update, 2026-09-22 - one of these was won, and it is measured.** The plan in this section rated a
Cirrus CL-GD5446 bitBLT out of scope on the grounds that no one would ship such a card; the constraint that
actually matters is different, and was settled by building it: the CL-GD5446 is the only display device this
environment can emulate whose 2D engine is reachable through the interface SCos gives a driver module (a
mapping of the register BAR and 32-bit accesses, no port I/O, no bus-master DMA). `drivers/gpu/cirrus`
now drives the console of a `-vga cirrus` guest: the kernel's read-back self-test passes on the card's own
memory, a whole 800x600 repaint costs the CPU nothing and 480,000 pixels the engine, and one console
scroll is one bit-block transfer of 368,640 pixels. The numbers, the register-model findings (the block
lives at BAR+0x100, `BLTWIDTH` counts bytes, the start bit is edge-triggered) and the limits are written up
in `GPU-DRIVER-MODULES.md`; what the card still cannot do - no copy from system memory, so the compositor's
RAM-resident frame cannot be pushed by the engine - is stated there rather than glossed. What has *not*
changed is the conclusion about modern GPUs in section 4: none of this makes a GeForce RTX 5050 render, and
nothing in it was ported from a vendor's code.


Four concrete, non-speculative observations:

1. **For this machine's iGPU, the achievable tier is the display engine, not the
   blitter.** Porting `intel_extreme`'s display half — mode setting, the
   `frame_buffer_offset`/`DISPLAY_A_SURFACE` scanout path, `intel_move_display`, the
   retrace semaphore and (once the format is corrected for the generation) the hardware
   cursor — removes the parts of the present job that are *not* drawing: SCos today
   renders into a RAM canvas and `fb_flip_rect()` copies every damage rectangle into the
   firmware LFB, so a driver that owns the scanout plus flips a second buffer removes the
   copy entirely, and a vsync removes the tearing that the CPU-only direct-write path
   would otherwise have. Panning removes full-window scrolling from the pixel budget
   altogether, which is the browser's dominant interaction. What stays on the CPU is
   exactly what this tier cannot move: fills, copies and rasterisation (section 4). The
   prerequisites before any of it can even be attempted are the three measured SCos gaps
   in section 4 — a WC mapping for a driver-chosen scanout range, PCI interrupt routing
   (`_PRT`, or MSI) for the vblank handler, and an aperture/GGTT allocator — plus the
   Rocket Lake device ID added to the reused table.
2. **An older card is the only way to get GPU *drawing* from a light driver.** Any
   board whose family is in section 3 — Radeon 7000 → X850/XPRESS 200, RIVA TNT →
   GeForce 6/7, Matrox G100–G550, VIA KM400/P4M-class IGPs, S3 Trio64/ViRGE/Savage,
   3dfx Banshee/Voodoo 3/5, Tseng ET6000 — gives real GPU fill and copy, native mode
   setting and a retrace/vsync hook, from 83–3,949 lines of engine code plus that
   family's own display init. Hardware cursor is per-family and per-setting: the
   matrix above shows it conditional on the `hardcursor` driver setting in
   `nvidia`/`matrox`/`via`/`neomagic`, direct in `s3`/`ati`/`3dfx`, and commented out
   in `radeon`. Coverage is by exact PCI ID, so it is per-card, never "all GPUs".
   The one bridge toward a usable slot in the 2011-or-later machine is AMD's
   RV370/RV380 generation: the `radeon` table carries `0x5960`–`0x5965`, `0x5B60`
   ("Radeon X300 (RV370)") and `0x5B63` ("Radeon X1050 (RV370)"), and that generation
   was sold in PCIe form as well as AGP. A cheap PCIe X300/X1050-class card could
   therefore land inside a ~1.2 k-line blitter plus the family's modeset — worth checking
   against the real ID before any larger commitment.
3. **AMD GCN-era via RadeonGfx is a middle case.** The separate Haiku RadeonGfx project
   programs the CP ring and CE (copy engine) for command submission, but its unit
   instantiation only accepts Cape Verde and Tahiti, and its project-wide licensing is
   unresolved (see `GPU-DRIVER-RESEARCH.md`). So "an RX 580 gets GPU copies" is not a
   finding I can support; Polaris appears in tables and firmware lists, not in an
   accelerated unit selection.
4. **Reducing CPU work does not always need a GPU driver.** Two changes inside SCos,
   independent of hardware, remove real per-frame traffic: drawing directly into the
   scanout for damage rectangles instead of canvas-copy (drops one full pass over every
   dirty pixel — the `rep movsl` in `fb_flip_rect` — at the cost of tearing until a vsync
   exists), and rejecting per-pixel format conversion by requiring a native X8R8G8B8 GOP
   mode, which today is the `native` branch at `kernel/desktop/fb.c:23,27` and falls back to
   a per-component `component()` call per pixel otherwise. Both are honest CPU wins, not
   GPU rendering, and I list them so the trade is not oversold.

5. **A modern card can at least be read, and now is.** Item 4 of §4 said the practical ceiling for a
   Blackwell chip was identification, not acceleration, and called the copy classes out of reach.  The
   classes are in fact published (`classes/dma-copy/clc*b5.h` and `classes/host/clc*6f.h` in
   NVIDIA/open-gpu-doc, MIT), so the vocabulary of a submission is available; what is not published is the
   register database for those generations and the bring-up that runs through the chip's system processor,
   which is why `drivers/gpu/nvidia` ships as an identification driver and nothing more: it reads
   `NV_PMC_BOOT_0` through the kernel's mapping, decodes the fields where NVIDIA documents them, reports
   the negotiated PCIe link, writes no register, and offers no engine operation, so the CPU compositor is
   unaffected by its presence - and that last clause is measured, not asserted: `tools/tests/
   test_nvidia_ident.py` boots a stand-in module of the same shape on an emulated Cirrus function and reads
   the log, the panel and the report, which show the CPU compositor still owning every pixel
   (`pixels painted: 0 by the GPU's 2D engine, 17312532 by the CPU`).  See `GPU-DRIVER-MODULES.md` for the
   numbers and for what has to be solved before a copy could be pushed on that hardware.

## 6. Browser: what it genuinely needs

Checked rather than assumed:

- Firefox's hardware WebRender requires a usable GL context — OpenGL 3.x desktop or
  GLES 3.0 class per the documented minimum, and GPUs below that (e.g. Gen5) are
  blocklisted and drop to `WebRender (software)`, i.e. CPU rasterisation
  ([ArchWiki Firefox/Tweaks](https://wiki.archlinux.org/title/Firefox/Tweaks),
  [Mozilla bug 1748819](https://bugzilla.mozilla.org/show_bug.cgi?id=1748819)).
- Chromium gates GPU tile rasterisation on a supported ANGLE/GL or Vulkan backend and
  refuses it otherwise, listing `Rasterization: Software only. Hardware acceleration
  disabled` ([Chromium issue
  370557216](https://issues.chromium.org/issues/370557216), [chromium-discuss
  report](https://groups.google.com/a/chromium.org/g/chromium-discuss/c/Ma8CiCHDkBM));
  its newer Skia Graphite backend targets Vulkan/Metal/D3D12
  ([Phoronix](https://www.phoronix.com/news/Chromium-Skia-Graphite)).

Therefore: a copy/fill blitter **cannot** make the browser rasterise on the GPU. It
removes the compositor's copies and lets the browser's own rendered buffer be
presented/scaled cheaply; the text, paths and layer painting stay on the CPU until a
GL/Vulkan driver exists — which section 4 shows is the 200k–400k-line territory, not
the light path. Both browsers also keep blocklists keyed to known vendor drivers, so a
custom driver would additionally have to present a convincing GL/Vulkan surface before
either would enable acceleration at all.

What the *display* tier does change for a browser, and it is not small: scrolling a page
becomes a `MOVE_DISPLAY` register write instead of a full-viewport copy plus repaint; a
vsync-locked scanout flip removes the per-frame damage copies `fb_flip_rect()` does
today; and native timings mean the panel runs at its own mode rather than the one GOP
happened to leave selected. Rasterisation, glyph shaping and compositing remain CPU work
in this tier — that is the ceiling, stated plainly rather than sold.

## 7. Memory and disk streaming, re-measured for this tier

The earlier 16 MiB concern came from the NVIDIA GSP firmware files (28,345,432 +
63,330,032 bytes). The light drivers in section 3 carry **no firmware at all**: the
largest whole family that actually drives a 2D engine (radeon) is 21,092 lines / 678 KB of
source, and its engine files alone are 83–3,949 lines. So this tier can be statically
selected at boot — read only the matching family, or even link several — and does not require a runtime module loader,
a disk-backed VFS, or raising `read_file`'s 16 MiB cap. Selective loading stays
worthwhile (per-device binding still matters: touching BARs of an unclaimed adapter is
exactly the kind of thing that breaks a machine), but "stream drivers from disk" is
only forced by the heavy vendor stacks. That also means the deferred AHCI/NVMe work is
not a prerequisite for this tier.

One budget does have to be paid for the display tier, and it is arithmetic rather than
a guess: owning the scanout means owning a canvas at least as large as the visible mode
— at 1920×1080 that is 8,294,400 bytes, which is the size the compositor today copies
*damage* of (`fb_flip_rect`, `kernel/desktop/fb.c:17-35`) while `fb_init` already holds a
same-sized RAM canvas (`screen.px`, allocated once at `kernel/desktop/fb.c:11-15`). A
flip queue doubles it; panning to cover scrolling needs the canvas taller than the mode,
so 1920×2160 is ≈16.6 MB total. The footprint therefore grows by a factor of two to
three with no other change being made. Haiku does not pay that from kernel RAM — the
front buffer comes from
the GART/aperture service (`gGART->allocate_memory`, `intel_extreme.cpp:592-594`), which
is why the allocator gap listed in section 4 has to be closed before this tier can be
sized honestly.

## 8. Verification gates before any "works" claim

Nothing below is done yet, and none of it is possible inside this sandbox: `/dev/dri`
and `/dev/kvm` are both absent and `qemu-system-x86_64` is not installed, so no GPU
operation — physical or virtualised — can be executed here.

1. Device match: exact vendor:device from SCos itself (`graphics` in Terminal lists the
   up-to-8 PCI display adapters it finds, read-only, no resets) — determines whether any
   section 3 family even applies.
2. Compilation and ABI: engine sources must build against SCos' own MMIO, heap and
   interrupt primitives, with no Haiku kernel calls left behind.
3. Command correctness: each reused op verified against the hardware documentation and
   the upstream driver's own expectations; a rejected batch is a bug, not a fallback.
4. Real completion: submit a known fill/copy, wait for the engine idle/sync token, then
   **read back the framebuffer and compare pixel-for-pixel** with the CPU result. Only
   this distinguishes "I wrote registers" from "the GPU moved pixels".
5. Integration: compositor uses the path for damage lists; a failed engine call must
   fall back to CPU per operation without leaving the screen wrong.
6. Cost accounting: measured RAM for command buffers/rings and the aperture, per driver
   — never a guess.
7. Physical: the exact card in the exact machine, including resume/reset and a second
   boot, because UEFI already owns the display and taking it over can lose it.
8. Substrate, *before* any of the above for the display tier: a write-combined mapping
   that accepts a driver-chosen scanout range (today WC is PAT slot 1 and is only ever
   applied to the validated GOP LFB, `kernel/src/memory.c:71-72`), PCI interrupt routing
   so the retrace semaphore has an IRQ behind it (SCos is 8259-only: `irq_install` with
   `pic_clear_mask`, `kernel/src/interrupt.c:39-44`, and no `_PRT` or MSI code anywhere in
   `kernel/`), and an aperture allocator for the scanout, cursor and ring memory. Each of
   these is small, but each is required, and none of them is a reason to write a GPU
   driver — they are the plumbing any reused driver would need.
9. Reversibility: taking over the display from firmware can leave no picture at all, so
   the path back (restore the firmware LFB and re-`flip` the RAM canvas) has to exist and
   be exercised, not merely argued for.
10. Browser: only meaningful after a GL/Vulkan driver exists; otherwise the truthful
    statement is CPU raster plus GPU-assisted presentation.

## 9. Open input needed from the machine

Two unknowns decide the plan, and both are answered by the same command. Run `graphics`
in SCos Terminal (or `lspci -nn` under any OS) and report every line, including the
vendor:device IDs:

1. **Is the scanning adapter a section 3 family, and does its ID appear in that family's
   own table?** If yes (including an inexpensive PCIe RV370/RV380-era card), GPU copy and
   fill are on the table at 83–3,949 lines of engine code plus that family's modeset, and
   section 8's gates apply unchanged.
2. **Is the iGPU close enough to `intel_extreme`'s Gen9+ display code to drive?** The
   table reaches Alder Lake but not Rocket Lake, so this is "add an ID and verify the
   Gen12 display paths", and it buys the display tier of section 5 item 1 — native mode,
   vsync, scanout ownership, panning, cursor — with fills and copies staying on the CPU.

If neither answer is yes, the truthful options are the CPU-side reductions in section 5 or
a large Mesa/DRM-class port, and I will not call either "GPU rendering" beyond what it
actually is. Separately, tell me whether adding an older card is acceptable at all: the
*only* way to have the GPU move pixels for the browser's compositing, from a driver in the
weight class you asked for, is a family whose blitter small drivers actually implement.

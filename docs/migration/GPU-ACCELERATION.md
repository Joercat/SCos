# Why the GPU was not accelerating the system, and what was done about it

Date: 2026-10-28. This document answers one question with evidence instead of a promise: *the driver
loads, the boot log says "GPU 2D engine bound", so why is the CPU still painting every pixel?* It
records the answer for the machine this OS is being built for (a GeForce RTX 5050, `10de:2d83`, GB207,
Blackwell), the two independent failures that produced it, the fixes that are now in the tree, what each
fix was measured to change, and the one part that no change to this repository can fix by itself.

Everything below marked *measured* was produced by booting the built image in the pinned QEMU 11.0.2
(`tools/setup_qemu.py`, `tools/run_qemu.py`) and reading either the guest's own log or the emulated
device's memory through the debugger. Nothing marked measured is inferred from a comment.

---

## 1. The short answer

There were two separate reasons, and fixing either one alone changes nothing:

1. **On GB207 there is no engine to run.** `drivers/gpu/nvidia/module.c` binds, maps the card, reads
   `NV_PMC_BOOT_0`, reads the engine table, asks the FSP - and offers **no drawing operation at all**.
   Its ops table is `fill = 0, copy = 0, wait_idle = 0`, deliberately and with a comment saying so.
   `gpu_engine_drives_output()` tests `module_ops->fill`, so every accelerated path in the desktop is
   skipped, and the notice says "the module that knows this chip implements no engine". That is not a
   regression in the driver: on Turing and later, NVIDIA's own open driver boots the GPU through GSP
   firmware and submits work through channels created by that firmware. SCos has neither yet.

2. **Even where an engine does exist, the compositor offered it almost nothing.** The output path
   (`kernel/desktop/fb.c`) handed a damage rectangle to the GPU only if *every pixel in it was the same
   colour*. A desktop is not made of single-colour rectangles, so the engine was asked to paint almost
   nothing and the CPU copied the rest. This one was measurable on a card that SCos fully supports:

   ```
   # emulated CL-GD5446, driver bound, self-tested 225/225, log line "engine drives the console"
   # before: one boot and three window opens
   pixels painted: 0 by the GPU's 2D engine, 17,280,000 by the CPU compositor
   ```

   `0` on the GPU, with a working, verified, bound engine. That is the bug this session actually fixed.

The two failures explain every observation on the target machine, including the ones that look
contradictory: a driver that loads and reports, a screen that is never accelerated, and a `graphics`
panel that says both things truthfully.

## 2. The chain, link by link

Each link is a thing that must be true before a pixel is painted by a GPU, in the order it is checked,
with where it lives and what was wrong with it.

| # | Link | Code | State found | State now |
|---|---|---|---|---|
| 1 | The firmware walks PCI and names a family | `boot/uefi/pci_scan.c` | **stopped at the first match the walk met, even when no driver binds that chip** | collects every match, prefers a drivable one, hands the list up |
| 2 | The stub decides which module to read | `boot/uefi/main.c` | **read the module for that first match; a name-only match meant "no module", so the card behind it was never read** | reads `DRVLIST.IDX` first, chooses a family that is both drivable and on the disk |
| 3 | The kernel validates, relocates and binds the module | `kernel/drivers/gpu/gpu_module.c` | correct; refuses on named reasons | unchanged |
| 4 | The kernel binds it to the function that feeds the console | `kernel/drivers/gpu/gpu_detect.c` | **bound the first eligible device of that family, which need not be the scanout owner** | scanout owner gets first refusal, then the rest |
| 5 | The family has an engine to offer | `drivers/gpu/*/module.c` | 3 of 15 families have code; `nvidia` offers none | unchanged: this is the Blackwell problem, §4 |
| 6 | The compositor asks the engine for the work | `kernel/desktop/fb.c` | **only single-colour rectangles; measured 0 engine pixels in a real desktop session** | every damage rectangle is decomposed into flat regions; measured 74% of a desktop frame |
| 7 | The work is counted, so the claim is checkable | `gpu_engine_note_work()` | correct | unchanged; the new path counts engine and CPU pixels separately |

Links 1, 2, 4 and 6 were defects. Five and seven were already honest.

### 2.1 Link 1 and 2: the first match was not the right match

The scan stopped as soon as any display function matched a table - including a function that matched
*only the naming registry*, which knows what a chip is and binds nothing. On a hybrid machine that is
the common case: the firmware exposes an Intel display function that Haiku's `intel_extreme` table does
not bind (or binds for another generation), and the discrete card sits behind a root port later in the
walk. The stub then took that first match, saw "naming only", and logged

```
No GPU family matched by the generated tables; no module read
```

while the module for the card it was actually running on sat on the same disk, unopened, and the
kernel's `graphics` panel reported a machine with no driver. The fix is two rules, both in the tree:

* `pci_scan.c` visits every function and fills `struct pci_display_scan::candidates[]` with each match
  and a `drivable` flag (a driver's own binding table claims it, as opposed to the naming table);
  `scan.match`/`bus`/`slot`/... describe the preferred one - a drivable match beats a name-only match
  whichever order the walk met them in.
* `boot/uefi/main.c` reads `\SCOS\DRVLIST.IDX` **before** choosing, and stages the module of the first
  drivable candidate whose family the index lists. Only if no drivable family has a file does it fall
  back to naming the chip whose module is missing, which is the honest report and the one that tells a
  user to put the file on the disk.

`tools/tests/pci_scan_host.c` now carries the shape that failed - a name-only Arc A750 at `0:2.0`, an
RTX 5050 behind a root port - and asserts that both are reported, marked, and that the drivable one is
preferred.

### 2.2 Link 4: the engine must be bound to the screen, not to "a card"

`gpu_engine_drives_output()` asks whether the *scanout owner* has ops with a `fill`. The bind loop used
to bind the first `gpu_module_eligible()` device whose family matched, so on a machine with two
functions of the same family the engine could end up on the one nobody is looking at - and the desktop
stays on the CPU with "engine bound" in the log. The loop now makes two passes, scanout owner first.

### 2.3 Link 6: the reason the desktop was never accelerated

This was the largest defect and the least visible, because the code looked deliberate: *"A solid
rectangle is the common case for the wallpaper, the boot screens, window chrome..."* It is not the
common case. The wallpaper is flat *around icons*, a window is a flat border around a flat background
around text, and a damage rectangle is normally the whole window. The old rule scanned the rectangle,
found the first different pixel, and copied all of it on the CPU.

The measured consequence, on a card whose driver works:

```
# before            engine 0            CPU 17,280,000     (one boot, three window opens)
# after             engine 12,927,697   CPU 10,208,407     (same session)
# one desktop flip  engine 10,679,040   CPU  3,720,960     = 74% of the frame on the GPU
```

`kernel/desktop/fb.c` now decomposes each damage rectangle into maximal flat rectangles in one pass:

* runs of one colour are carried from row to row; a run that is identical one row below extends its
  rectangle, one that is not closes it (`engine_decompose()`);
* the rectangles are sorted by area and up to `ENGINE_MAX_CALLS` of the largest are handed to the
  engine (`fill_rectangle`), because a fixed budget of register round trips should buy the most pixels;
* every pixel no such rectangle covers is copied from the shadow buffer by the CPU, one row at a time,
  so a textured interior costs only its own pixels;
* `screen.px` remains the truth buffer throughout: the decomposition decides *how* bytes reach the card,
  never what they are.

Bounds and their reasons are named in the file: a run under 8 pixels, a rectangle under 512 pixels or
one row tall, a rectangle whose flat area is under 1/8 of it, and more than 32 transfers per rectangle
all stay on the CPU, because a bitBLT costs a register round trip and a busy-wait and for a cursor-sized
rectangle that is more than the copy it replaces.

Two defects were found while building it, both caught only by comparing the *device's* memory with the
shadow, which is now a test (`test_engine_paints_flat_regions_of_a_real_rectangle`):

* the decomposition emitted run-relative x with absolute y, which painted rectangles shifted left by
  the damage rectangle's own x - invisible in the shadow, wrong on the screen;
* the residual copy added a *pixel* offset to a `void *`, so it wrote `row*stride` bytes into the
  aperture, hitting the first few rows of the screen and leaving the intended row untouched.

Neither would be visible in a screenshot diff of a boot screen, and both are the reason the tests read
the emulated card's own memory.

### 2.4 What is still on the CPU, and why that is correct

Text glyphs (1-8 px runs), icons, and any pixel that is part of a photo stay on the CPU. A 2D engine
paints rectangles; it cannot expand a font bitmap without a source in its own memory, and the ABI has
no such operation yet. `graphics` reports the split as two numbers, so a later port can be measured
against this baseline rather than argued about.

## 3. What the fixes cost

Measured on the same emulated CL-GD5446, thirty full-screen flips of a desktop with four windows open:

| | per flip | engine share |
|---|---|---|
| before | 3.5 ms | 0% |
| after | 4.0 ms | 74% |

The extra half millisecond is the decomposition's single pass over the damage rectangle in *host RAM*.
QEMU cannot show what that replaces: the emulated framebuffer is host memory, so the CPU copy it stands
in for costs almost nothing there, while on real hardware the copy it removes is a write-combining
store across PCIe into video memory - the slowest store the machine has. What QEMU *can* show, and what
the tests assert, is that the engine does the work and that the result is byte-identical.

A machine with no engine bound pays **nothing**: `fb_flip_rect()` only enters the new path when
`gpu_engine_drives_output()` is true, which is false on the RTX 5050 today. Its rendering cost is
exactly what it was.

## 4. Why the RTX 5050 still renders on the CPU, and what that would take

This part is not a defect in this tree and cannot be fixed by changing this tree's own code. For Turing
and later, NVIDIA's open kernel modules require GSP firmware for every supported architecture, and
submission happens through channels that firmware creates. The evidence is in `GPU-BASIC-2D-RESEARCH.md`
and `GPU-AUTO-DETECT.md`, and the machine's own report agrees: the engine table is readable, the
submission window is not, and the coprocessor that owns both refuses a reader with `0xbadf4100`
(`FECS_PRI_CLIENT_ERR`) until it has been booted.

What `drivers/gpu/nvidia/module.c` does today, in order, and it is real work already in the tree:

* maps the register BAR the kernel hands it and reads `NV_PMC_BOOT_0`, the engine table at `0x22800`,
  the timer block, the virtual-function page and the FSP scratch words;
* measures the PCIe link and reports every block that refused, by name, rather than as a number;
* assembles the FMC handover payload in the published Blackwell format (48-byte hash, 97-byte public
  key, 96-byte signature, checked against the *architecture's* widths, not one hard-coded number);
* sends it through the FSP mailbox once the mailbox has answered a query, and re-reads the two words
  that change if the FSP accepted it (`NV_PGSP_HWCFG2` and `NV_THERM_FSP_BOOT_COMPLETE`).

What it has not done, and what a driver that renders on this generation additionally needs:

1. **VRAM reservation for the firmware**: `WPR2` and the FRTS region, set up in video memory before the
   coprocessor may be started. Until that exists the FMC has nowhere to boot into, which the module
   says in its own report.
2. **The radix-3 page table and `GSP_FMC_BOOT_PARAMS`** the FMC reads back out of system memory, in the
   exact layout the reference driver publishes.
3. **The GSP resource manager**: after the FSP releases the coprocessor, the GSP runs NVIDIA's own RM
   firmware, and *that* is what creates channels. A client of it (the "RM API") is what a submission
   path calls.
4. **A channel and a 2D/graphics class submission**: GPFIFO, a user-mode window, and the `GR` or copy
   engine's method stream - the part that actually paints a rectangle.
5. **Signed firmware that matches the module**, staged on the ESP; the intake checker
   (`tools/gspcheck.py`) and the boot stub already read and validate those five images.

Steps 1-4 are a project measured in thousands of lines and weeks of hardware iteration, not a patch. A
plan that pretends otherwise would be the same class of mistake as a panel that calls an idle GPU
"accelerated". Until then the truthful state of this machine is the one `graphics` prints, and the
whole point of §2.3 is that the *rest* of the machine is no longer wasting the GPU it does have.

Two smaller limitations are worth writing down with it:

* **Between two drivable families both present on the disk, the stub picks the walk order.** Reading a
  BAR value (not sizing it) would let it prefer the function whose aperture contains the firmware
  framebuffer, and that is the natural next step for hybrids with, say, an ATI iGPU and an NVIDIA card.
  It was not done here because the "header reads only" rule in the stub is asserted by a host harness
  that treats any other offset as a failure, and widening it deserves its own test.
* **The module cannot hand the engine a back buffer yet.** `gpu_module.c` reports the off-screen window
  a module offers and deliberately does not enable it; a back buffer only becomes useful when the
  compositor draws into video memory instead of a shadow in RAM, which is the next large piece of work
  and would give the engine the copies and drags it currently cannot take.

## 5. How to check this on the machine

```
graphics
```

Four lines answer the whole question, and each is a measurement rather than a claim:

* `GPU acceleration:` - which module was opened, how many pixels it verified in device memory, and
  whether it is used for output at all;
* `Engine:` - the driver's own reading of the chip: `NV_PMC_BOOT_0`, the engine inventory, whether the
  submission window answers, whether the coprocessor is out of reset;
* `pixels painted: N by the GPU's 2D engine, M by the CPU compositor` - the only number that says the
  GPU is doing work. On a machine with the fixes above and a supported engine, `N` grows with use; on
  the RTX 5050 it is `0`, and the line above it says why;
* `module store:` - how many modules the disk holds, which one was read, and how many bytes were never
  opened.

Expected on the RTX 5050 after this change: the stub now stages `NVIDIA.MOD` even when an undrivable
display function comes first in the bus walk, so the module loads, identifies the chip and prints its
register report where before the machine could say nothing at all. Rendering stays on the CPU, and the
panel says so in those words.

## 6. What was changed, in one list

| File | Change |
|---|---|
| `kernel/desktop/fb.c` | damage rectangles decomposed into flat regions for the engine; residual pixels copied from the shadow; CPU/engine accounting; two defects found and fixed by device readback |
| `kernel/drivers/gpu/gpu_detect.c` | module bound to the scanout owner first; `gpu_engine_available()` no longer claims an engine that has no fill |
| `boot/uefi/pci_scan.c/.h` | every match collected with a `drivable` flag; no early stop; drivable match preferred |
| `boot/uefi/main.c` | `DRVLIST.IDX` read before the choice; family chosen only if drivable *and* on the disk; honest message for a named-but-undrivable adapter |
| `kernel/desktop/graphics.c`, `platform.c` | user-facing wording matches what is now true ("flat output regions") |
| `tools/tests/pci_scan_host.c` | hybrid-GPU cases and candidate assertions |
| `tools/tests/test_gpu_render.py` | device-versus-shadow equality for structured rectangles; desktop frame mostly painted by the engine; textured rectangle entirely on the CPU |

## 7. The Blackwell path: the first increment, and what it does not reach

Section 4 ended with a plan in five steps.  Step 1 - *reserve video memory for the firmware* - is the
one that had no code behind it at all, and it is now in the tree, in the shape the reference drivers
describe and with the arithmetic under test.

Three things were missing, and each of them was a different kind of missing:

1. **A number.**  Every address in the reservation is an offset *back from the top of video memory*,
   so a driver that does not know that top cannot place anything.  The register that holds it is
   `NV_PGC6_AON_SECURE_SCRATCH_GROUP_42`, `0x001183a4`, RW-4R, published (MIT) in NVIDIA's
   `src/common/inc/swref/published/ampere/ga102/dev_gc6_island.h`, and the boot firmware leaves the
   frame buffer's size in it as a count of MiB (`ga102_fb_vidmem_size()` in nvkm reads it the same
   way).  `drivers/gpu/nvidia/module.c` now reads it - the kernel cannot, the register block is the
   module's window - and refuses four shapes of answer by name: all ones (a block that is not
   decoding), a `0xbadf...` refusal (a block that will not answer *this* reader), zero or a value
   larger than any card (not a size), and a register window that stops short of it (this driver's
   own bound, not the chip's answer).  What survives those checks crosses to the kernel in a
   `struct scos_gpu_device_report` - the first thing a module may say *back* through the ABI - and
   nothing else does, so the kernel's zero still means "the chip did not say".
2. **A placement.**  `kernel/drivers/gpu/gsp_layout.c` computes the whole reservation from five
   inputs: FRTS down from the 128 KiB-rounded top, the bootloader 4 KiB aligned under it, the ELF
   image 64 KiB aligned under that, the heap 1 MiB aligned under the image, the WPR2's start one
   MiB-aligned `sizeof(GspFwWprMeta)` under the heap, and a 1 MiB host heap below that.  The order,
   the alignments and the sizes are nouveau's `tu102_gsp_oneinit()` and `gh100_gsp_wpr_meta_init()`
   (nvkm, MIT); the code is this tree's own - a freestanding function with no allocation and no device
   access that refuses rather than shifting a region when the card is too small.
3. **An order of operations.**  `gpu_fsp_note()` used to run *before* any module was bound, so the
   handover was built before the only code that can read the video-memory size had run.  It runs
   after now, with the reason written where the call moved.

What is measured, and how:

* `tools/tests/gsp_layout_host.c` pins the placement against a case worked out by hand from the rules
  (a 1 GiB card: FRTS at 1072693248, bootloader at 1072627712, image at 1071579136, heap at
  1069547520 and 2031616 bytes long, WPR2 `[1068498944, 1073741824)`, host heap at 1067450368), then
  checks ordering, per-region alignment, no overlap and the memory bound over **495** accepted
  combinations of eleven card sizes and five image sizes, then the refusals - and that a refusal leaves
  no offset a caller could hand to firmware.
* `tools/tests/nvidia_ident_host.c` drives the new register through the module's fake register file in
  five shapes and asserts the number reaches the kernel and appears on the panel, with the register
  and its unit named, and that not one store was issued.
* `tools/tests/test_gpu_fsp.py` re-proves that the shipped payload is **unchanged** on a card that
  cannot be measured: the harness is pinned at `frtsVidmemOffset = 0`, `frtsVidmemSize = 1 MiB` and
  `gspBootArgsSysmemOffset = -1`, which is what it asserted before this work.  One real defect was
  caught by exactly that check while writing this: `GSP_LAYOUT_OK` is zero, so a plan that was never
  computed read as a successful one and put a **zero-byte FRTS** into the handover.  The gate is
  `plan_ready` now, not the reason code, and the test says why.

What is still missing, stated plainly because the panel is easy to read as more than it is:

* **The resource-manager image is no longer a matter of asking somebody.**  Earlier revisions of this
  document said `GSP.BIN` - `nvidia/<chip>/gsp/gsp-<ver>.bin`, 63,571,696 bytes - "has to come off a machine
  with the driver installed", and that was simply wrong: it is in `linux-firmware`, which is a Git repository
  and therefore fetchable, and several chip directories do not hold it at all but *link* to the family that
  publishes it (gb202's is a symlink into `ga102`).  `python3 tools/gsp-firmware.py --fetch-driver 570.144
  --chip gb202 --set-out build/gsp-set` now reads the three files by partial clone, resolves the links,
  checks each one is the container class its role implies **before writing anything**, and stages them under
  the ESP names the boot stub reads.  Measured on the set it produced: `GSP.BIN` 63,571,696 B ELF64 with
  `.fwimage` 63,541,248 B and `.fwsignature_gb20x`; `GSPFMC.BIN` 199,820 B ELF32 with the four sections;
  `GSPBL.BIN` 200,812 B; `tools/gspcheck.py` then reports **fw 3/5** and the handover builds from the FMC.
  A booted image carrying that set reports `gsp firmware: 3 of 5 images read by the stub, 63972328 B, set
  complete for the FSP, the booter pair is absent`, and `3 image(s) held ... (63979520 B), parsed by this
  kernel` - the resource manager included, which is the first time this tree has carried one.
* **Three of five is not an incomplete set on this hardware, and the panel now says so.**  The five roles
  are two sequences: a part with an FSP is started by the resource manager, the FMC and the bootloader and
  never opens `booter_load`/`booter_unload`, which belong to the path a part *without* one takes.  `gspcheck`
  had always said this in a note ("the booter path belongs to a coprocessor that boots without the FSP, which
  this chip does not") while the panel called the same three files `incomplete: this volume carries fewer of
  the five than the boot sequence asks for` - a report sending somebody to look for two files their card does
  not want.  `boot.h` now names the split (`BOOT_FIRMWARE_FSP_ROLES` 3, `BOOT_FIRMWARE_BOOTER_ROLES` 2), the
  boot stub and the kernel both judge *the FSP sequence*, the count is still n of five because that is what
  is on the volume, and the panel says `complete for this machine` with the two absent names still listed
  below it.  The five-role case still reads `complete for both paths`, and a missing FMC or resource manager
  is still `incomplete`.
* **The volume has to be bigger than 64 MiB.**  63.5 MB of firmware does not fit in the tracked image, so
  `make ventoy` grows the disk to what the staged set needs (`--min-size` on the same packer); the committed
  `dist/scos.img` stays at 64 MiB and carries the FMC alone.  A 128 MiB guest could not even *keep* the set -
  the stub read all three files and had no pool for the resource manager, which the panel reported as two
  images held rather than as a missing file - so the machine that boots it wants a few hundred MiB of RAM,
  as any machine with an RTX 5050 has.
* **Steps 3-5 of §4.**  With the set staged, the reservation above is what the handover describes - proven
  end to end in `tools/tests/gpu_fsp_host.c`, where a card reporting 8 GiB gets a plan whose offsets appear
  identically in `GspFwWprMeta` and in the payload's FRTS fields, and a 2 MiB card keeps the declined
  encoding.  What remains is the GSP's own message queue, a channel and a class submission, and none of it
  can be verified here - QEMU models no NVIDIA function, and there is
  no Blackwell card in this environment.  A successful plan on a real machine would be visible as a
  `graphics` panel line naming the MiB it read and the offsets it derived; it is *not* rendering, and
  nothing in this tree claims it is.

## 8. The handover that was never delivered: a 256-byte window

The first machine to run the handover reported it in one sentence: *"The handover was written into the
channel and the doorbell rung ... and no answer came back"*, with `reads 97 writes 226` beside it and, in the
same paragraph, *"The EMEM offset advanced 1 dword(s) while the packet was written"*.  That last number is
the whole story, and this tree spent a revision explaining it away: 217 dwords of payload had gone into a
channel whose own counter said it had moved by one, and the report's comment said the counter was
"reported and not used to veto the send".  It was right not to veto on it, and wrong not to believe it.

**What the counter was saying.**  The FSP's channel is reached through a window: one control register
(`NV_PFSP_EMEMC`) takes a byte offset and a direction bit, and from then on every access to the data
register (`NV_PFSP_EMEMD`) lands four bytes further along by itself.  The window is **not** the whole 1 KiB
channel.  The reference driver declares its EMEM port as `min = 4, max = 0x100` and re-arms the window for
every chunk it moves (`gp102_flcn_emem_pio` in `nvkm/falcon/gp102.c`; the do/while that calls
`pio->wr_init(..., mem_base)` per chunk is in `nvkm/falcon/base.c`).  The offset counter inside the window is
eight bits wide.  So a transfer longer than 256 bytes does not fail: it **wraps**, and writes over the front
of what it just wrote.

An 868-byte handover - two header dwords and 860 bytes of `NVDM_PAYLOAD_COT` - did exactly that.  Its first
256 bytes landed, the next 612 were written over them, and the packet the doorbell pointed at had the FMC's
address in the right place and nothing else.  The FSP was handed a message whose own `size` field described a
record it did not hold, and it answered with silence - not with an error code, because a request that never
arrived intact is not a request it can reject.  Every gate this tree had built stopped at "the packet went
out", which was true, and none of them asked what the channel was holding afterwards.

**What the fix is.**  `emem_write()` and `emem_read()` in `kernel/drivers/gpu/gpu_fsp.c` arm the window for
each 256-byte burst (`FSP_EMEM_BURST_BYTES`), and every access the file makes to the channel goes through
them.  Then, before the doorbell, the packet is **read back out of the channel and compared with what was
written**.  The read-back happens before the ring on purpose: the FSP writes its reply into the same channel
from offset 0, so a check that ran afterwards could compare the answer with the request and call the answer
damage.  Replies are read the same way, and read twice, because "seven dwords came out of one register" is
only a measurement if the same seven come out again.

**What the card then answered, and why the read-back is reported and not obeyed.**  The first GB207 to run
this produced `send blocked(3)` on a *four-dword capability query* - a packet far inside one 256-byte burst,
written to a channel the card then answered a query on.  The mismatch started at **dword 0**: this silicon's
data window does not return what was written to it, at any offset, while the writes themselves land.  A veto
built on that comparison therefore means *never ringing the doorbell on this card*, which is strictly less
known than a reply.  So the tree reports the mismatch - with the dword it starts at, and the word "ring-anyway"
beside it - and rings the doorbell.  Whether the packet arrived is then a fact the card states, not one this
kernel infers from a window it has measured to be write-only.  `send_blocked` now only carries the two
conditions the card itself gives (queue busy for the whole wait, or the queue registers unreadable); the
read-back outcome lives in `send_readback` and `send_mismatch`.

**What this machine measured afterwards**, in QEMU with the fetched set staged (the one place in this
repository that boots at all): the three images read, `63972328 B`, `pw 3/5`.  What it *cannot* measure is the
thing the bug was about - QEMU models no NVIDIA function - so the evidence for this fix is structural:
`tools/tests/gpu_fsp_host.c` models the window as hardware does, wrapping at 64 dwords, and asserts that a
full 868-byte handover arrives byte for byte in **four** bursts, that the read-back ran before the doorbell,
and that a window which never walks is caught by the read-back (mismatch reported, no veto, doorbell rung).
The count is pinned at 137 checks.

**What the next boot should show, and what each answer means.**

| On the panel | What it means |
| --- | --- |
| `The request was written into the channel in 4 windows ... every dword of it came back identical` | the packet is in the channel; whatever follows is the card's answer and not this OS dropping it |
| `read back: differs from the packet at dword N` | the channel does not return what was written (measured on GB207 at dword 0, on packets it then answers); the doorbell is rung anyway, and N says where the two diverge |
| `GSP handover: ... error 0x..` with a code | the FSP read the whole record and refused it by name: `0xdf` is the WPR/FRTS one, `0xdb` the boot check, `0xdd` a bad DMA target |
| `boot: no answer in N of 4000 ms` | the handover went out and neither the lockdown bit nor the falcon mailboxes changed inside the reference driver's own budget |
| `boot: settled in N ms, lockdown bit 13 clear` | the FMC booted the resource manager; this is a success the reply wait used to report as silence |

**The record the resource manager reads, and why it was the blocker.**  The FMC boots with
`GSP_FMC_BOOT_PARAMS`, and the field that tells the resource manager where its own arguments live
(`gspRmParams.bootArgsOffset`) used to be written with the address of the FMC copy this kernel staged - a
real, readable address, and not a record of anything.  The reference driver writes a 4 KiB LibOS argument
record there instead: four 32-byte memory-region descriptors, `LOGINIT`, `LOGINTR` and `LOGRM` at 64 KiB each
and `RMARGS` at 4 KiB, every one contiguous system memory, and each log buffer carrying one physical address
per 4 KiB page from its second word onwards (`r535_gsp_libos_init`, `LibosMemoryRegionInitArgument`).  That
record is now built and pointed at, and the panel prints its address beside the three sizes, so a boot that
still fails says whether the arguments were staged before it says anything about the card.

**And the completion test, which was the second half of the same mistake.**  The FMC does not answer a
successful handover on the channel: it boots the resource manager, which is what `gh100_gsp_lockdown_released`
waits for - the falcon's `HWCFG2` bit 13 cleared, or a mailbox pair carrying the boot-arguments address (or an
error code) - polled for 4000 ms.  This tree now runs that test in that order, and reads a reply only if one
arrived, because a build that waits for `FSP_RESPONSE` after the COT reports the one outcome that matters as
silence.  The panel's handover line prints which of the two happened, with the registers it was read from:
`no answer` and `still locked` for a card that did nothing, `settled` and `lock released` for one that
booted.

## 9. The video-memory size that arrived as zero

A second bug was found while making the panel shorter, and it would have stopped the next stage at the first
step.  The NVIDIA module reads the frame buffer's size from the boot firmware's scratch word
(`NV_PGC6_AON_SECURE_SCRATCH_GROUP_42`, 0x001183a4, a count of MiB) and hands it to the kernel through the
module ABI.  That field was 32 bits.  A 512 MiB test card fits; this machine's 8 GiB does not - `8192 << 20`
truncates to `0` - and zero is the ABI's word for *the chip did not say*.  A kernel that reads zero there has
nowhere to place a firmware reservation, so the WPR2 stage would have been refused with a message about a card
that never gave a size.

`scos_gpu_device_report.vram_bytes` is now 64 bits and the ABI version moved to 2, which also means a stale
`*.mod` from before this change is refused by name instead of being trusted.  `tools/build_gpu_module.py` no
longer carries its own copy of the version number: it parses it out of `kernel/include/gpu_abi.h`, because a
packer holding a constant is exactly how a module ends up written for an ABI the kernel has moved past.  And a
module load that finds the scratch word unusable now *clears* the report rather than leaving the previous
load's number in it - a stale size is a place a reservation would be put.

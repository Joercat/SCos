# Final conversion check and first physical-PC test

2026-09-20 — native x64 UEFI, still unnumbered.

## Result and limits

The fresh source-parity and boot checks found **no additional unexplained
conversion omissions**. This follows the complete 38-file source disposition
in [CONVERSION-AUDIT.md](CONVERSION-AUDIT.md), not just a count of driver files.
It is not a claim that every possible app workflow has been exercised or that
an untested physical machine is guaranteed to boot.

The tested guest image is unchanged from commit
`c37638b790ec846c73dd48936734c3cdb5e6be88`:

```text
67108864 bytes
SHA-256 f6a0d9a0e2a071a7010ba6b762fd79e430edc9e5accad161df6a4ad3b99b5f70
```

If you already downloaded that exact image and its checksum matches, there is
no new guest binary to download for this check. The current changes are this
verification record and an explicit USB-boot option in the host QEMU launcher.

### Fresh parity checks

* Recompared original function inventory to the native tree. The remaining
  absent names are the already-accounted-for native architecture replacements,
  unsafe old BAR/MTRR helpers, obsolete BIOS-sector copies and intentionally
  removed File Manager deletion callback—not newly discovered missing apps.
* Compared shell dispatch tokens: the removed `open` alias follows the user's
  `appstrt`-only instruction; the removed `sysrq error` branch was a synthetic
  diagnostic with fabricated example state, retired as requested. Real error
  reporting and the real panic trigger remain. No other removed command/token
  names were found by that comparison; this is not a proof that every flag works.
* Original Blackjack, Browser notice, Calendar, Solitaire and both dedicated art
  files are byte-identical. Cards differ only by a trailing blank line. The
  original and native font byte literals match.
* Native ELF64/AMD64 PIE has no unresolved imports. Warning-as-error build,
  build/published-image comparison, checksum and frozen-image guard passed.

### Fresh integration tests on the published bytes

Each guest had **USB storage as its only boot disk**, fresh EDK2 variable state,
no networking and a disposable snapshot. Serial firmware device paths confirmed
actual `/USB(...)` loading, rather than an IDE disk with USB input attached.
QMP injected keyboard/mouse events; native memory inspection checked command
results and window lifecycle. These are QEMU 11.0.2/EDK2 results, not PC results.

| Configuration | Result |
| --- | --- |
| USB2-only xHCI, storage at 480 Mb/s, 128 MiB RAM | Booted, native USB input and desktop worked |
| USB3-capable xHCI, storage at 5000 Mb/s, 256 MiB | Booted, all ten original app windows opened together |
| Keyboard behind an external emulated USB hub | Booted, downstream keyboard commands and direct mouse worked |
| `Haswell-noTSX` CPU profile, 2048 MiB with RAM above 4 GiB | Booted and interacted successfully; this is an additional Intel instruction profile, **not an emulated i5-11400/H510 motherboard** |
| Virtio VGA's firmware GOP instead of standard VGA GOP | Booted and rendered the original desktop/app correctly; no native virtio graphics driver was added |
| 64-MiB image written into a sparse 16-GiB simulated USB stick | Booted and interacted successfully; confirms the larger-media case under EDK2, not every firmware's GPT handling |

In every row, USB input evaluated `calc 6 - 2` to `Result: 4`, launched apps,
moved the mouse right/down, confirmed WM termination and returned to a fresh
desktop, then confirmed ACPI shutdown. `save` correctly refused to claim native
USB persistence. Screenshots of the ten-window desktop and alternate GOP output
were inspected. The first 64 MiB of the simulated larger stick remained identical
to the published image after snapshot testing.

The permanent host launcher now reproduces USB boot with:

```sh
python3 tools/run_qemu.py --usb-boot
```

It uses a disposable snapshot and emulated xHCI storage/keyboard/mouse. The normal
IDE/q35 launcher remains available. Temporary verification harnesses, screenshots,
logs and enlarged disk images were removed afterwards.

## Preparing your i5-11400 PC

Your CPU is consistent with the native x64 target. The remaining unverified
variables are the actual firmware, dedicated GPU's UEFI GOP, controller behavior
and USB device descriptors. The exact GPU/VBIOS and motherboard firmware revision
have not been established in this session. Do not infer physical acceptance from
the CPU name or from the emulator matrix.

1. **Back up the USB stick.** Flash `dist/scos.img` as a raw disk image using a
   reputable image writer. Merely copying the `.img` file onto a formatted stick
   will not create this boot medium. Flashing erases/replaces its partition table
   and data; carefully identify the USB device, never an internal system disk.
   The image assumes 512-byte logical disk sectors.
2. Use **x64 UEFI boot**, choosing the boot-menu entry named `UEFI: <USB name>`
   or equivalent. This image has **no legacy BIOS/CSM boot path**. Disable CSM or
   legacy-only boot if needed; the previous 32-bit boot settings do not apply.
3. **Secure Boot must be off** for this unsigned image. If Windows uses BitLocker
   or device encryption, have your recovery key available before firmware/boot
   setting changes; those changes can trigger recovery. Do not clear the TPM.
4. For the first attempt, disable firmware **Fast Boot** if it skips USB device
   initialization. Use a direct motherboard USB port for the stick and input
   devices, avoiding docks/hubs initially to reduce unknowns. Leave other firmware
   settings alone: do not change SATA/AHCI mode, Above-4G decoding or random USB
   settings on speculation. No PCI BAR relocation workaround is required here.
5. Keep the monitor on the output used by the firmware. The dedicated GPU must
   offer a usable UEFI GOP framebuffer; no accelerated GPU driver is required.
   If firmware cannot provide GOP with CSM off, that is a boot prerequisite to
   resolve, not something the current native GPU code can fix.

## What to expect and what to check

Expected path: **SCos UEFI loader → native startup → original boot log/logo →
desktop**. Firmware reads the kernel from the USB stick before SCos exits firmware
services. SCos then takes over native input and rendering.

Start with these non-destructive checks:

* Move the mouse in all directions; try stationary clicks, wheel scrolling,
  dragging/resizing windows, and double-clicking desktop icons.
* Open Terminal; type ordinary and repeated characters. Run `calc 6 - 2`.
* Switch through Ctrl-Alt-F1–F6 and back to the desktop with Ctrl-Alt-F7.
* Open Files, Notepad, Settings, SysMon and the games; check layout and input.
* Try a confirmed restart/shutdown after the basic desktop/input checks. A
  safe-to-turn-off screen is the documented fallback if ACPI cannot power off.

**Expect RAM-only files on a typical USB-boot/AHCI-only PC.** The original ATA
PIO driver is converted, but AHCI, NVMe and USB mass-storage persistence were
never existing SCos drivers. Do not switch your installed OS's SATA mode to make
SCos saves appear. New storage/network/GPU/browser work remains deferred as
requested. The browser still displays its original unavailable notice.

Other intentional differences from the 32-bit image remain documented:
UEFI replaces BIOS/VBE; scanout is currently uncached rather than applying the
old unsafe MTRR edits; new physical-memory/fault handling replaces fixed 32-bit
addresses. Rendering performance on the real GPU has not been measured.
The permanently frozen 32-bit image is still available as the retained milestone.

If it fails, photograph the **last visible screen and exact text** and note
whether failure is before the loader, during startup, or after the desktop
appears. Include motherboard model, BIOS version, GPU model, and which USB ports
were used. Do not repeatedly change settings or reflash identical bytes without
new evidence. A firmware boot-menu failure, GOP failure and native-input failure
are different problems and should be diagnosed separately.

## Subsequent input/compositor corrections

See [the unnumbered input/compositor verification record](INPUT-COMPOSITOR.md)
for the lower default sensitivity, fractional-motion and drag-trail fixes,
additional UI/settings corrections, and the focused PC retest checklist.

## Which build is on the stick, and what it touched (r-after-provenance)

Type `version` in the terminal, or read the first lines of `graphics`. Both now
name the commit the kernel was compiled from, the branch's tree state and the UTC
build time - and `\SCOS\BUILD.TXT` on the ESP carries the same record plus the
sha256 of the kernel and of every driver module, so a stick can be dated from
another OS without booting it. `tools/verify-stick.py` reads that record off a
device or a file and compares it with the repository's own copy:

```text
python3 tools/verify-stick.py /dev/sdX --against dist/scos.img.build.json
  commit      : 1db75411364f
  built (UTC) : 2026-09-23T03:48:46Z
  files on \SCOS  : ATI.MOD (13536 B), BUILD.TXT (580 B), ...
MATCH: the disk record and the repository record are the same build
```

It opens the path read-only and writes nothing. `MISMATCH` means the stick holds a
different build than the one being discussed, which is worth ten minutes before a
boot cycle: a panel line that a build cannot print any more (a sentence deleted from
its source) is proof the machine ran something else, and no conclusion about a
feature should be drawn from it. If `graphics` does not print
`Build: <commit> clean, <date>`, the machine is not running the build under
discussion, and nothing about features should be concluded from it. See
[BUILD-PROVENANCE.md](BUILD-PROVENANCE.md).

`graphics` also gained `Device access:`, which reports whether the kernel read the
scanning adapter's own registers. On the RTX 5050 the expected line is a real
`NV_PMC_BOOT_0` value; `mapped, all-ones reads` means the chip was addressed but
did not answer; `memory decode disabled by firmware` means the BIOS left the
function without its aperture enabled and SCos will not write configuration space
to fix it, because detection is read-only by contract.

### What the GPU is doing, said as an outcome (r-notice-truth)

Two lines changed because a panel that narrates a process reads like a report of a result. `Renderer:` now
ends in the outcome rather than in what the module did: on a machine whose driver read the chip and
implements nothing, it says `- the GPU is NOT rendering: the module that knows this chip implements no
engine`. And the module's own verdict, previously only in `gpuinfo`, is now printed on this panel as

    Engine: NV_PMC_BOOT_0=0x... at BAR 0x... arch 0x..., ...; window class 0x...; engines at 0x22800:
            LCE 2/VIC 0/GFX 1/ENC 0/DEC 0/SEC 0/GSP 1 of 4 devices, LCE pri 0x100000 inst 1 runlist 1
            engine 5

That last clause is read out of the chip, not looked up: it is the silicon's own list of the engines it
has, which is the information a submission has to be addressed to. `engine table silent at both published
offsets` means the driver could not find that list - the address is published for Turing and Ampere and not
for Blackwell - and is deliberately *not* phrased as "this chip has no copy engine". `runlist ?` means the
chip had the field and did not vouch for it; `runlist 0` would mean it said zero.

The notification matches: a bound module that implements no engine produces **`GPU is not rendering`**, a
warning that stays on screen, and never `No 2D engine bound`, which is a different machine's problem. On the
build that introduced this, the notice for such a machine read `GPU engine verified on a second adapter` -
the words were produced by a predicate that tested whether a driver's operation *table* existed instead of
whether it contained a drawing operation. If a paste shows that sentence alongside `pixels painted: 0 by
the GPU's 2D engine`, the build predates the fix.

## Text that does not fit goes on the next line

Everything above was clipped by a `...`, and clipping is how this machine's report arrived with its last
clause missing. That is fixed at the layout, not per panel: `kernel/desktop/textwrap.c` walks a string into
rows at a fixed font width, breaking at spaces, and a word longer than a row is split so that a row is never
empty and no character is dropped. `s_text_wrap()` paints those rows and returns how many there were;
`s_wrap_rows()` answers the same question without painting, which is what makes a box able to size itself to
its text. `s_clip_text()` remains for the places with no next line to go to - a taskbar button, a table
cell, a window title.

The four places that were fixed are the four a person reads: a **notification** (its height is now
`26 + rows * 16 + 22` instead of a fixed 108 pixels, and the box under it stacks by that height, so a long
notice pushes the stack down rather than eating its own text); the **terminal** (a line longer than the
window continues on the next row, and the line store no longer truncates the *stored* line either - it
splits it into as many entries as the window needs, so scrolling back finds the tail); the **boot log** (a
status longer than the panel takes the lines under it, and the next status starts after them); and the
**panic screen** (the message used to be drawn with no bound at all, so past a screen width it left the
display).

Measured by `tools/tests/test_textwrap.py`, at the levels that matter separately: the layout alone, over
every width from 1 to 40 columns, checked for *losslessness* rather than for having wrapped somewhere; a
booted guest's painted rows on a scratch surface counted from their pixels against the same kernel's row
count; and on the real screen, a 500-character notice measuring 272 pixels against 64 for a one-line one,
and a 339-character terminal line producing 5 inked bands where the kernel's layout promises 5. The first
of those three has caught two real bugs while being written - a row that returned a pointer at the string's
end instead of zero, which made every caller count one empty row and inflate every scroll range, and an
`8`-argument painter whose tail arguments a debugger cannot pass were garbage.

The consequence for this machine: the GPU notice is no longer sized to a byte budget that forced a short
sentence. Its field is 512 bytes, `_Static_assert` checks the sentence against that, and the whole
explanation - including what to run and what each answer would mean - arrives and wraps.

One line of the `graphics' panel changed in the same stroke, for the same reason and by the same mechanism.
`Device access: none (a driver module is staged for this function)' was accurate about the kernel and read as
an accusation about the card, on the machine whose module had read twenty-odd registers of it: the line now
names who stayed out, quotes the module by name, and points at the module's own counts above, arriving as one
240-character sentence that would previously have lost its ending to three dots. `test_nvidia_ident.py` holds
the invariant for whatever a configuration produces - a `Device access` line saying "none" has to say "none by
the kernel" - because QEMU's own adapters take the branch where the kernel *did* read the block, and the state
being described here is a Blackwell function under a module, which no guest on this host can present.

The `Engine:` line that gets pasted ends in the chip's own engine inventory on a Blackwell part, because the
format Blackwell publishes was implemented, and it has now been read off real silicon: `engines at 0x22800
v2 (60 devices x 3 rows of 152, 152 read): LCE 3/VIC 0/GFX 2/ENC 1/DEC 1/SEC 1/GSP 2/JPG 0 of 39 devices,
… engines per its own IS_ENGINE, … bus; LCE rows 0x…/0x…/0x…, LCE pri-field 0x… inst n runlist 0x… engine
n`.  If the block does not answer as a v2 table, the line quotes the two CFG dwords instead, which is also a
result.  What is not yet a result is the *window*: `window class 0x1100 clock frozen` says the page the Volta
and Turing documents describe is not answering there, and until it does, an engine in a table is an engine
that cannot be given work - so the panel says so in one clause (`engines are register blocks, not channels:
no submission window answered, so nothing was submitted`) computed from those same two readings rather than
written by hand.  `device registers: not read`, which contradicted the module above it, now names whose
reading is absent.

The same line now ends with what the chip answered at the three targets a submission would need, and each
possible ending is a different instruction to whoever reads it.  `GSP engine out of reset, mailboxes
0x…/0x…` means there is a coprocessor to speak to; `GSP engine in reset` means starting it is the whole next
step; `GSP block at 0x110000 unreachable (engine reads 0xffffffff)` means the aperture did not answer; and
`GSP block at 0x110000 refused all five reads (0xbadf4100: a PRIV target locked against this reader, not a
register value, so no field of it is read)` - what real GB207 silicon produced - means the chip answered with
an error code, which is a fact about permission rather than about the coprocessor's state, so nothing after
it claims a state (GPU-DRIVER-MODULES.md quotes the vendor source that names the value).  Beside it, `base
unit measured on this chip: the GSP entry's field 0x1100 x 256 is the published 0x110000` is the engine
addresses being derived from the chip's own table by a test that either passes or says it failed - in which
case `no base unit: … scales to no candidate that reaches the published 0x110000` appears and no engine
address is printed at all.  Then `the first LCE's block at 0x…` says whether the engine a submission would be
built for answers, refuses or is silent, with the FSP's four scratch words as a fourth opinion, since that is
the microcontroller whose release the lock waits for.

Paste the `Engine:` line whole, these clauses included: the window answer, the coprocessor's state and the
engine block's reply are three different facts, and the next move depends on which of them this machine gave.

The firmware lines are the one part of this report you can change without rebuilding anything, and they are
worth changing on a machine whose panel says the coprocessor refused.  The five images the open path loads go
in the SCOS directory at the root of the ESP, named `GSP.BIN`, `GSPFMC.BIN`, `GSPBL.BIN`, `GSPBTL.BIN` and
`GSPBTU.BIN`; upstream they are `nvidia/gb207/gsp/{gsp,fmc,bootloader,booter_load,booter_unload}-570.144.bin`,
and running `python3 tools/makedisk.py build` with those files (or that whole tree) under `build/gsp/` names
them for you.  `GSP firmware: none of the images the open driver loads` is therefore a statement about the disk
and not a refusal.

The shorter route, and the one worth trying first, is the checker this repo ships.  It reads the blobs where
the open driver left them and applies this kernel's own rules to each: the byte-exact 52-byte FMC header, the
four sections the coprocessor asks for by name and their per-generation widths, the CRC32 each section carries
in `sh_info`, the `.fwimage` and signature pair in the resource-manager image, and the 8 MiB the boot stub will
copy.  With `--install` it also renames them, writes them into the directory you point at, and reads every byte
back before it reports:

    python3 tools/gspcheck.py /lib/firmware/nvidia/gb207/gsp --install /mnt/esp/SCOS

A file it refuses never reaches the disk, and every refusal is a sentence about the one thing wrong with it -
`the publickey section is 384 bytes and architecture 0x1b asks its FSP for 97`, not `invalid` - because telling
a wrong file apart from a right file checked against the wrong generation is the difference between hunting for
another download and passing `--arch`.  It ends by printing what the panel will then say (`fw 2/5`, and whether
a handover can be built at all), so the paste and the check can be compared without guessing.  Its constants are
pinned against `kernel/drivers/gpu/gpu_firmware.c`, `gpu_fsp.c` and `boot.h` by `tools/tests/test_gspcheck.py`:
the checker refuses what the kernel would refuse, and nothing else.

By hand, the same three files come off a machine where the open driver already works - the names come from
`NVKM_GSP_FIRMWARE_FMC(gb207, 570.144)` and `nvkm_firmware_get()` in the kernel's nouveau driver, which is
where this OS's intake contract came from:

    sudo mkdir -p /mnt/esp/SCOS
    sudo cp /lib/firmware/nvidia/gb207/gsp/fmc-570.144.bin        /mnt/esp/SCOS/GSPFMC.BIN
    sudo cp /lib/firmware/nvidia/gb207/gsp/gsp-570.144.bin         /mnt/esp/SCOS/GSP.BIN
    sudo cp /lib/firmware/nvidia/gb207/gsp/bootloader-570.144.bin  /mnt/esp/SCOS/GSPBL.BIN

`GSPFMC.BIN` is the one this build assembles a handover from; `GSP.BIN` and `GSPBL.BIN` are what the resource
manager's own boot arguments would need, and those are not implemented here yet, so staging all three changes
what the panel can *say* before it changes what the card does.  Say so rather than let `fw 3/5` imply more.

Once files are there the panel answers in three paragraphs, and each one is a different question.
`GSP firmware:` is the disk: per image, its size, its CRC32 as read, whether it is an ELF64 image (with
`.fwimage` and the name it gives its signature section) or an ELF32 FMC image (which has neither, and is not
asked for either), and whether the set of five is complete.  `GSP firmware intake:` is what this build does
with them - it says plainly that nothing is handed to the card.  `GSP firmware image:` is memory: the address
of the region the boot stub kept the copies in, and for each image held, what this kernel read out of the bytes
themselves - which section is where, whether every section's own CRC32 still matches, and the four lengths the
coprocessor's verify command is given.  Measured on a test VM whose five files are the containers the suite
builds (so: sizes 1896 and 1128, not a real 570.144 blob), the third paragraph reads

    GSP firmware image: 5 of 5 held read-only at 0x061e3000 (20480 B); gsp: ELF64 1896 B, 4 sections,
    .fwimage 1024 B at 0x00000040, .fwsignature_gb20x 512 B at 0x00000440; fmc: ELF32 1128 B, 6 sections,
    FMC header exact, every section legal and its CRC32 verified, 0 CRC32 mismatch(es); image 512 B at
    0x00000268, hash 32, publickey 4, signature 256 B - the four numbers the FSP verify command is given;
    bootloader: held, not a container either way; ...

The endings to look for are as informative as the success.  `no container` on a role means the file is not one
of the two ELF images the driver names - a text file, an HTML download page, a copy interrupted halfway.
`the bytes in memory do not hash to the checksum the disk copy carried` means the file was fine and the copy
was not, which is a different bug and is reported as such.  `N more on the disk than the boot allocation could
keep`, or no third paragraph at all, means the region was refused: the cap is 32 MiB and the boot allocation is
what says no, not this OS choosing to forget.  `unreadable` means the firmware could not open the file at all.
Paste all three paragraphs, plus the `Engine:` line, when reporting a machine: the first says what the stick
holds, the second what this build does with it, the third what is in RAM right now, and those are three
different questions a reader has to be able to tell apart.

### The FSP paragraph, and what to paste from it (r-fsp-panel)

The Graphics report now has a paragraph starting `FSP mailbox:` - the card's own security processor, which is
the thing that has to answer before any part of this GPU can be driven.  It is read on every boot.  One thing is
written, and only after the read has come back clean: a capability query goes into the mailbox, and if the FSP
answers it with a well-formed reply and no error, the FMC image this build was given is handed over (see
`r-cot-send` below).  Anything that has not answered a query is never handed an image.  Four endings are worth
knowing the difference between, and all four are information:

* `not asked, and not because anything failed ... vendor 0xNNNN` - this machine's display function is not an
  NVIDIA one, so the offsets were never touched.  On a QEMU boot it reads `0x00001013` and that is correct.
* `The FSP's boot-complete scratch ... reads 0x...`, where the low byte is not `ff` - the FSP has not reported
  itself booted.  This is the expected answer on a card whose own firmware never got going, and it is a real
  measurement rather than a missing one.
* `asked of the card at BAR0 0x...` with a query sent and a reply read, or refused - the mailbox is alive.  The
  sentence after it says which of the four ways a request can fail applies, and how many registers were written.

Below that, a `GSP handover:` paragraph says what became of the FMC image on the stick.  It has four endings now,
and which one you have is the single most useful sentence to report:

* `the ... payload was written into the channel and the doorbell rung.  It carried the FMC image of N bytes at
  physical 0x..., CRC32 0x...; the FSP answered in 28 bytes with error 0x.. (FLCN_...)` - the card took the
  image.  The rest of the paragraph gives the coprocessor's capability word *before* and *after* the message
  and the same for the FSP's boot-complete scratch: those two are the measurement, so report both numbers.
  `FLCN_ERR_FMC_BOOT_FROM_INVALID_WPR` or `..._INVALID_DMA_TARGET` are ordinary answers and each points at a
  different missing piece; `FLCN_OK` with the capability word changing is the one that means something started.
* `... and no answer came back` - the message went out and the reply queue never separated.  Say which, because
  a card that answers a query and not a handover is a different machine from one that answers neither.
* `... the write was refused before the doorbell could be rung` - the queue registers themselves refused while
  the second message was being prepared, which says the first one is possibly still being read.
* `It goes on the wire only after the FSP has answered the query cleanly` - the query did not come back, so the
  image stayed in memory.  This is the same ending a machine with no firmware on the stick produces, and the
  sentence before it says which of the two applies.

**How to report it without losing the end of it.**  The panel is longer than what a console paste carries, and
the last three reports came back cut off inside the detection list - which is upstream of the two paragraphs
above.  Two things changed because of that.  The first line of `graphics` is now a single `FSP:` line that
carries the whole decision - boot scratch, mailbox, query error code, handover outcome, lock, clock, VF page,
firmware count, read/write totals - so a paste that stops early still says what happened and why.  And the cut
itself was found to be ours rather than the terminal's: one report's `Engine:` line measured 1151 bytes, which
is the size of the buffer the module builds that line in, so the sentence ended mid-word and the panel's last
line stopped at `Display: 1024x7`.  That line is now built in a field twice as large, with 96 bytes held back
at the end so that if a report ever overruns again it ends with `[+N B of this line dropped by this module's own
buffer: report it as truncated]` instead of stopping in the middle of a clause, and the kernel's copy of the
field says the same thing about itself.  To see the clipped text either way, run:

    graphics fsp

in the terminal or on the TTY.  It prints *only* the firmware intake, the parsed images and the FSP mailbox
paragraphs - the same text the panel prints, from the same code - and nothing else, so it fits.  If the panel
itself prints `Firmware intake and FSP mailbox: not printed`, the buffer did not have room for them and `graphics
fsp` is the only way to see them; that notice says how many bytes were left, because a limit that states itself is
not the same thing as a loss.  Paste all of that output, plus the `Engine:` line and the `Scanout owner` line, and if any of them ends in a
bracketed notice about its own length, paste that too - it is the one part of the report that says a fact was
removed rather than a fact withheld.
The numbers in them - the boot scratch value, the refusal count, each reply's error code, and the before/after
pair around a handover - are what a follow-up change gets argued against, and every one is a register read or a
count the kernel kept while doing it.  Two of those numbers arrived because the working driver was read instead
of guessed at: the first line now names the row of the FSP's table this chip is measured against (`cot
48/97/96 v2` on a GB2xx, `48/384/384 v1` on a GH100 - a container is judged against *its* generation's widths,
which is why a real GB207 file that is fine can now be accepted rather than refused as malformed), and the lock
is reported as `bit13=1 locked` or `bit13=0 released`, which is the bit the driver reads rather than whether a
register looked non-zero.  Waits are printed in milliseconds against the reference driver's own budgets (1500
ms for a reply, 4000 ms for the FSP's boot) because a wait counted in reads was tens of times shorter than the
chip needs, and a too-short wait shows up on a panel as "silent" - the most misleading thing a report can say.

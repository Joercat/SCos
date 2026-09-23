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

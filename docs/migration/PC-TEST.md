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

# SCos — a real, bootable x86 operating system

SCos started life as a browser simulation (`os.html`, kept in this repo as the
design reference until the native system is declared finished).  This
repository now contains a **real operating system** that recreates that
simulation: a two-stage MBR bootloader, a 32-bit protected-mode kernel, a
VBE framebuffer window manager with full mouse support, a terminal, a virtual
filesystem with optional disk persistence, and the same look & feel as the
web original (SCos 1.3.5 / "2.0" native).

It boots on real x86 hardware (legacy/CSM BIOS boot) and in the v86 emulator
used by the test suite and the live preview.

```
make            # build build/scos.img (bootable disk image)
make test       # host regressions + 32 headless v86 scenarios (build/tests/)
make preview    # browser preview on http://localhost:8080
```

## Live preview

`make preview` (or `node tools/preview_server.mjs [port]`) serves a page that
runs `build/scos.img` in the [v86](https://github.com/copy/v86) x86 emulator
inside your browser, with real mouse and keyboard input.  The emulator
runtime is fetched once into `.preview/vendor/` by `tools/setup_preview.sh`
(git-ignored; ~3 MB).

Click inside the screen to capture keyboard/mouse.  Everything is interactive:
desktop icons, window drag/resize/minimize/maximize/close, taskbar, context
menus, dialogs, themes.

## What boots

| Stage | File | What it does |
|---|---|---|
| 1 | `boot/stage1.S` | MBR (sector 0). Loads stage 2 + kernel via INT 13h (LBA with CHS fallback), jumps to 0x8000. |
| 2 | `boot/stage2.S` | A20 line, VBE mode query/set (1024×768×32, real LFB address from the VBE info block), E801 memory map, loads the kernel flat binary to 0x100000, builds a GDT, enters protected mode, jumps to the kernel. Passes a boot-params block (LFB base/pitch/size, RAM size). |
| 3 | `kernel/` | 32-bit protected-mode ring-0 kernel, linked at 0x100000, flat binary. |

Kernel drivers & services (`kernel/src/`):

* `idt/pit` — PIC remap, IDT, exception panics, PIT at 100 Hz
* `kbd` / `mouse` — PS/2 keyboard (IRQ1) and mouse (IRQ12) with wheel,
  IntelliMouse probe, event queues
* `fb` — VBE linear-framebuffer compositor: back buffer, blit, pixel/text/
  line/circle primitives, 8×16 bitmap font generated from DejaVu Sans Mono
  (`tools/fontgen.py`, regenerate with `make font`, needs Pillow)
* `wm` — window manager: windows with title bars (minimize/maximize/close),
  drag, resize, z-order/focus, desktop icons, taskbar with live clock and
  running-app buttons, context menus, modal dialogs, error popups
* `vfs` — in-RAM tree filesystem mirroring the web version
  (`home/{documents,downloads,desktop}`, `system/{settings.json,about.txt,
  network.json}`, default `welcome.txt` + `changelog.txt`)
* `ata` — primary-master ATA PIO driver; `save` in the terminal serializes the
  whole filesystem to a disk image at LBA 2048 and it is reloaded on next boot
  (magic-gated; RAM-only until you `save`)
* `acpi` — RSDP/FADT scan for graceful shutdown; on machines without ACPI
  (including v86's SeaBIOS) the terminal `shutdown` falls back to a
  "power off now" screen
* `rtc` — CMOS clock for the taskbar clock and `date`
* `theme` — the four themes from the simulation: matrix-1, blue-sky,
  midnight-purple, amber-tech (persisted in `system/settings.json`)

Applications (`kernel/src/app_*.c`), all mouse-driven:

* **Terminal** — ~40 commands (`help` lists them all): file ops
  (`ls cd pwd cat cp mv rm mkdir touch tree hexdump head wc grep edit`),
  system (`sysinfo cpu free df disks uptime date neofetch theme cal`),
  shell (`echo alias history clear open save reboot shutdown`), games
  (`calc`, `blackjack`). Command history (↑/↓), typewriter output effect,
  scrollback with wheel scrolling. `ping` answers honestly: this kernel
  ships no TCP/IP stack, so there is nothing to fake.
* **Files** — back/up/refresh/new-folder toolbar, path box, click to open
  (dirs navigate, files open in Notepad), right-click menu with Delete +
  confirm dialog
* **Notepad** — editing with cursor, Save / Save As (dialog)
* **Calendar** — month grid, prev/next/today, today highlight, day click
* **Settings** — theme tiles (live switch), storage usage, **Factory
  Reset**: confirm dialog, then the user filesystem is wiped back to
  defaults, the wipe is persisted to disk, and the machine restarts
* **About** — live hardware report read from the machine itself: CPUID
  brand string, memory map totals, ATA IDENTIFY model, VBE mode/bpp,
  uptime from the PIT tick counter
* **Blackjack** — full card game vs. the dealer: 52-card deck shuffled
  from a PIT/RTC-derived seed, procedurally drawn cards (ranks + suit
  pips, face-down hole card), hit/stand/new-round by mouse or H/S/N keys,
  dealer stands on 17, win/loss/push tally
* **Browser** — intentional, honest stub window explaining that no TCP/IP
  stack ships with this kernel (a network card driver + TCP/IP + TLS +
  rendering engine is far beyond the project's size budget)

* **System Monitor** — live, measured metrics: CPU brand (CPUID), clock
  speed (TSC calibrated against the PIT), CPU load (idle-halt time
  accounted in the PIT interrupt), RAM from the page allocator, and the
  live task table (kernel services + running apps) with a real
  End-Task action that closes the selected app window

Taskbar: scrollable app-button strip (mouse wheel over the bar when more
windows are open than fit), plus a power button whose menu offers
**Restart** and **Power Off** (ACPI shutdown with power-off-screen
fallback). Terminal: wheel scrollback that follows the bottom unless you
scroll up, and paged help (`help`, `help --p2`, `help --p3`).
`neofetch` prints the ASCII logo contributed in `art.txt` next to the
live hardware report.

* **Solitaire** - full Klondike: 7 tableau piles (descending, alternating
  colours), foundations per suit, stock with recycle, stack moves,
  auto-flip, win detection; shared procedural card painter (`cards.c`)

Desktop shell:
* icons live on an invisible snap grid: drag them and they stick to the
  nearest free cell (layout saved to `system/desktop.json`)
* hold left button on empty desktop = rubber-band rectangle that
  multi-selects icons; Delete key or right-click menu removes them
  (app icons are only hidden from the desktop, pinned file shortcuts are
  un-pinned; real files are never deleted this way)
* Files right-click menu gained **Pin to Desktop** quick-launch shortcuts
* taskbar launcher button (far left) opens a search-as-you-type app menu -
  the place to find apps you removed from the desktop
* `rm` refuses to delete `/system/*` unless given `-s`/`-f`
* `/system` is now a real directory you can browse in Files and in the
  terminal: every boot it is refreshed with true copies of the boot chain
  read straight off the disk (`boot/stage1.bin` = MBR, `boot/stage2.bin`,
  `kernel.bin`, plus a `README.txt`). `cat` shows a hex preview of binary
  files, `hexdump` dumps them fully, `edit` edits the copy and `rm -s`
  deletes it (restored next boot). The real boot sectors are never
  rewritten, so you cannot brick the machine by exploring
* kernel panic screen (`sysrq panic` in the terminal triggers it on
  demand): ASCII art, reason, exception, full register dump incl. EIP /
  EFLAGS from the interrupt frame, raw stack dump, then halt
* `sysrq` is a Linux-style multi-action system request command:
  `panic`, `reboot`, `error` (non-fatal error screen self-test),
  `dump` (recent kernel log) and `time` (PIT uptime + RTC)
* `diag` takes optional subsystems: `diag pci|usb|input` scans just that
  subsystem and prints the kernel-log result in the terminal; bare
  `diag` still runs the full-screen everything scan
* CPU thread count is detected via CPUID (leaf 1, cross-checked with
  leaf 4) and shown in `neofetch`, `sysinfo` and the System Monitor
* the compositor repaints and flips only damaged rectangles: caret blink
  repaints a titlebar, the clock repaints the taskbar strip, app updates
  repaint their own window - idle CPU drops accordingly; an IRQ-storm
  guard masks any interrupt line firing above 1500/s
* boot screen is a real init log: each `[ OK ]` line is printed by the
  subsystem that just came up (CPU brand + measured MHz, memory, VBE
  mode, ATA drives + model, fs image status, RTC date); only the final
  "Finishing... I think..." line keeps the old ceremonial spinner

Easter egg from the web version included: if `home/documents/file.scv`
exists, ERROR windows start spawning (capped at 50).

## Verification

`tests/harness.mjs` boots the image in headless v86 with serial capture,
framebuffer screenshots and synthetic PS/2 input.  `tests/run_tests.mjs`
runs 14 user-level scenarios (boot, terminal commands, window manager
operations, files navigation/new-folder/delete, notepad edit + save-as,
calendar, themes, browser stub, easter egg, shutdown fallback, reboot,
ATA persistence across reboot, open-in-notepad + settings reset, resize
stress).  Each scenario writes a screenshot to `build/tests/`.

```
node tests/run_tests.mjs          # all
node tests/run_tests.mjs 4,12     # subset
```

## Running it on real hardware

A prebuilt image is kept in `dist/` and refreshed with every change, so
there is no need to build or serve anything to try it:

* image:    https://raw.githubusercontent.com/Joercat/SCos/arena/01a09dfd-scos/dist/scos.img
* checksum: https://raw.githubusercontent.com/Joercat/SCos/arena/01a09dfd-scos/dist/scos.img.sha256

(The same URLs always point at the newest pushed image. Temporary drop,
removed on request.)

```
sudo dd if=scos.img of=/dev/sdX bs=4M status=progress conv=fsync
```

* The machine must boot in **legacy/CSM mode** (MBR + BIOS INT 13h/10h);
  UEFI-only machines need CSM enabled.
* The kernel is a hobby OS: ring 0, single address space, no paging, no
  user mode, no memory protection between apps.  It is "real" in the sense
  that it is genuine bare-metal x86 code with real drivers — not in the sense
  of being a production microkernel.  This is deliberate and matches the
  project scope.
* Needs a PS/2 (or USB-legacy-emulated) keyboard+mouse and a VBE-compatible
  VGA BIOS.  Shutdown uses ACPI when the firmware exposes it, otherwise the
  power-off screen is shown.  `reboot` uses the 8042 keyboard controller.
* Only the primary-master IDE/ATA disk is probed for persistence.

## Repository layout

```
boot/            stage1 (MBR) + stage2 (VBE/PM) bootloaders
kernel/          entry.S, linker.ld, include/scos.h, src/* (kernel + apps)
tools/           fontgen.py (font table), makedisk.py (image assembler),
                 setup_preview.sh, preview_server.mjs
tests/           harness.mjs (headless v86), smoke_boot.mjs, run_tests.mjs
os.html          original web simulation (reference only, kept until the
                 native system is declared finished)
build/           build output (git-ignored): scos.img, intermediates, tests/
.preview/        emulator runtime for the preview (git-ignored)
```

## Build requirements

* GNU make + gcc with `-m32` freestanding support (no multilib needed for
  linking: objects are compiled `-m32` and linked with `-nostdlib
  -Wl,--oformat,binary`), `objcopy`
* Python 3 (+ Pillow only for `make font`)
* Node.js ≥ 18 for the test suite and preview
* Network access once, for `tools/setup_preview.sh` (npm + GitHub tarball)

### Round 39 input and desktop fixes

* Active HID endpoints no longer trigger synchronous hub-health probes merely
  because the user stops moving/typing. Root-port hotplug remains enabled;
  external-hub child hotplug during active HID use is deferred pending an
  asynchronous hub-status implementation.
* HID Home/Page Up/Delete/End and F11 mappings corrected; NumLock/keypad modes
  supported. Lock LEDs are not yet descriptor-aware; Pause/Print Screen actions
  and automatic held-key repeat are not implemented.
* USB layout/wide-axis mouse gain increased from 2 to 3 (preferences still scale
  it). Automatic diagnostic overlays removed; manual diagnostics and internal
  logs remain available.
* CPUID brand-chunk indexing, bounded output, extended model number and neofetch
  string construction corrected. `make cputest` validates against raw CPUID.
* SysMon CPU column measures time spent in each app's paint/input/tick callbacks
  on the single executing CPU; not an SMP scheduler utilization figure. System
  rows show `-` rather than fabricated attribution. Whole-percent rounding can
  display 0% for light workloads.
* Terminal/TTY underline insertion cursors, wider Settings controls, and the
  dedicated user-provided panic art. `make usbtest` includes idle-input and
  keyboard-mapping regression tests in addition to the existing ring/parser tests.

### Round 40: split HID reports, capture, and confirmed service stops

The r39 hardware report was **not** resolved by its idle-hub change. r40 fixes
additional, independently reproduced report-parser defects: multiple input IDs
per interface (up to eight report-state slots including unnumbered state), IDs
that collide modulo eight, repeated-ID input-offset resets, global Push/Pop,
and bitmap Usage Minimum. Mouse button/wheel-only reports are accepted without
movement. Keyboard modifier/key reports are merged per ID, with per-ID releases.
These tests are not a substitute for capturing the affected physical devices.
Lock LEDs and held-key autorepeat remain separate unimplemented features.

Manual capture, with no boot overlay or automatic recording:

1. `inputtrace start`
2. Keep the mouse still; click/release a few times, turn the wheel a few notches,
   and press/release `a` twice and NumLock once.
3. `inputtrace stop` (or `show`/`save`, which also freeze the capture).
4. `inputtrace show` displays the last 16 records; `inputtrace desc` prints the
   actual fetched report descriptors and endpoint identities.
5. `inputtrace save` writes the retained 128 reports plus descriptors to
   `/home/inputtrace.txt` and attempts disk persistence. The file contains raw
   keyboard reports: do not type secrets while recording. No network upload occurs.

Records include tick, slot/DCI, byte count, completion code, decoder acceptance
(0 rejected/error, 1 layout accepted, 2 boot/fallback path), actual queued key
and mouse event counts, and raw bytes. Queue-drop counters are included. Save
reports success only if both the VFS write and disk save succeed. Existing
`/home/inputtrace.txt` is replaced only by an explicit `inputtrace save`.

`kill --system 2` always asks `[y/N]`, then **actually stops scwm's event loop**
and enters the base console. In r40, `wm` retained app state; **r41 replaces this
with actual app/compositor teardown and a fresh desktop on restart.**
Other system rows describe in-kernel subsystems, not independently scheduled
processes; attempts to stop them are rejected, not faked. `kill <app-pid>` still
closes the actual app. `kill --help` explains syntax; malformed PIDs and unknown
kill options are rejected.

The same per-shell/per-tab confirmation handler serves `reboot`, `shutdown`
(and their `--confirm` spelling), privileged `rm -s`/`rm -f`, and interactive
`rm -i`. Enter, `n`, `no`, Escape, or Ctrl+C cancels; `y`/`yes` confirms; other
answers re-prompt. Aliases are resolved before requesting consent and the
confirmed command is frozen. Unknown rm flags and multiple paths are rejected.
Existing graphical confirmation dialogs and explicit emergency `sysrq` actions
retain their existing behavior. `make confirmtest` tests the shared handler.

r40 also corrects the framebuffer MTRR setup: the previous code accidentally
requested write-back type 6 while labeling it write-combining. It now requests
architectural WC type 1, uses the CPU's physical-address width and variable-pair
count, preserves existing ranges, checks WC support, and updates cache settings
with caching disabled/flushed. Only exact framebuffer page ranges are covered;
conflicting firmware mappings are left alone. This boot-only implementation
assumes SCos's existing single executing CPU and disabled paging. `make mtrrtest`
checks the pure register/range planner; it does not execute privileged MSRs on
hardware. v86 also does not validate the physical GPU/cache behavior.


### Round 41: packet completion, six consoles, and WM teardown

**r40 did not fix the reported physical input failures.** A concrete transport
bug was reproduced for r41: every interrupt TD requested the entire 64-byte
buffer, even on an 8-byte endpoint. Eight full-size packets therefore completed
one TD, and the decoder interpreted only its first report. Releases and wheel
steps in the remaining packets were lost. This produces repeated-key suppression
and movement-dependent clicks without any display-refresh dependency. The old
simulator incorrectly completed a TD for every packet and hid this bug.

* Initial and rearmed receive requests now use one endpoint packet (bounded by
  the 64-byte allocation). Completion lengths use the submitted request minus
  its residual, not allocation capacity. Only success/short completions decode.
* Descriptor-framed assembly preserves longer reports across packet completions,
  including 8+1-byte prefixed keyboards and 8+8-byte NKRO reports. Known unrelated
  report IDs are framed and discarded as whole reports; their continuation bytes
  cannot become phantom input. Truncated and zero-length reports clear assembly.
* The compensating USB 3× motion multiplier is removed. Saved sensitivity settings
  remain unchanged. Native PS/2 has separate fixes for 9-bit axis sign extension
  and Explorer's signed 4-bit wheel (which was previously interpreted as 8 bits).
  This is not an arbitrary PS/2 sensitivity reduction.
* Ctrl+Alt+F1–F6 select six persistent text consoles, each with its own history,
  unfinished input, working directory and confirmation. Ctrl+Alt+F7 returns to
  a **running** desktop. The GUI terminal also accepts `tty [1-6]`.
* Confirmed `kill --system 2` unwinds the WM loop, closes GUI applications and
  frees their resources plus compositor caches. Unsaved GUI edits are lost.
  `wm` starts a fresh desktop; F7 cannot resurrect a killed one. Switching VTs
  without killing the WM preserves its applications. The five-restart cap is gone.
* Console headings are simply `SCos ttyN`; the old maintenance/Linux-model prose
  is removed. Manual `diag`/`inputtrace` diagnostics remain available. Endpoint
  traces now include packet/request sizes; trace acceptance code 3 means a packet
  is awaiting its report continuation (0 dropped/error, 1 parsed, 2 fallback).

Verification: packet-level USB tests explicitly reproduce the old eight-report
batching/stuck-minus failure, then deliver 400 key packets and 600 stationary
click/release/wheel packets independently without GUI refresh. Another 400 split
packets exercise assembly and ring wrap, with foreign-ID, ZLP recovery, hidden-ID,
NKRO and PS/2 format checks. Restoring the oversized request makes regressions
fail. Emulator coverage checks six-console state isolation, actual heap reclamation,
fresh app state after WM termination, and seven stop/restart cycles.

These are local driver and emulator results, **not physical acceptance on the
user's PC**. HID support remains bounded to eight descriptor ID slots (including
unnumbered state), a 256-byte descriptor, 64-byte assembled reports and six merged
keyboard usages. Reports over 64 bytes are unsupported; unknown/vendor formats are not
universally supported. A full-size unprefixed
report from firmware whose descriptor requires an ID is ambiguous; short
unprefixed quirk reports remain supported. LED synchronization and held-key
repeat are still not implemented. No hardware resolution is claimed until tested.

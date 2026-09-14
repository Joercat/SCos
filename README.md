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
make test       # 14 end-to-end scenarios in headless v86 (screenshots in build/tests/)
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
* kernel panic screen (`panic` command in the terminal triggers it on
  demand): ASCII art, reason, exception, full register dump incl. EIP /
  EFLAGS from the interrupt frame, raw stack dump, then halt
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

```
sudo dd if=build/scos.img of=/dev/sdX bs=4M status=progress conv=fsync
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

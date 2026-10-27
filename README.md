# SCos — native operating system

**Active startup target: native x64 UEFI. There is no BIOS/CSM or 32-bit build.**
SCos keeps its own kernel and identity; it is not a Linux distribution.
`os.html` remains the desktop design reference.

The startup path now loads a relocatable ELF64 kernel, exits UEFI boot services,
uses the GOP framebuffer, owns its page tables and physical-memory allocator,
and establishes exception handling and real timer interrupts. This is an
implemented firmware-to-kernel path, **not the completed OS conversion**.
The original SCos apps and shared desktop have now been adapted to x64 rather
than replaced: Terminal, Files, Notepad, Calendar, Settings, About, Blackjack,
Solitaire, SysMon and the existing browser-unavailable notice. The compositor,
six TTYs, RAM filesystem, themes and PS/2 input are connected to the native core.
No new release number is assigned.

The remaining original PCI, xHCI USB HID, ATA PIO/persistence and ACPI power
implementations have now been adapted and integration-tested; see
[driver verification and limitations](docs/migration/DRIVER64.md).

**Still unavailable:** AHCI/NVMe/USB mass-storage persistence, networking/real
browser engine and accelerated GPU drivers. Files remain RAM-only unless a
unique supported ATA SCos-data partition is verified; `save` requires confirmation.
ACPI has a manual-power fallback for unsupported firmware. Emulator success is
**not physical acceptance on the user's USB-input PC**. No new release number.

For the first physical boot, use the [PC test guide and final USB-boot checks](docs/migration/PC-TEST.md). It distinguishes verified results from hardware requirements and includes safe flashing/UEFI settings.

## Sign-in, user choices and encrypted storage

**The owner is in control.** Every security restriction that could get in the
way is a user choice — on by default where it has a clear benefit, off the
moment you turn it off. **No Secure Boot and no signed boot**: the boot-signing
stack was removed at the owner's request as unnecessary friction. Boot
verification is **the same as a stock Linux PC without Secure Boot**: the plain
`KERNEL.CRC` CRC32 catches a damaged kernel file, but nothing authenticates the
loader, kernel or modules. Do not describe this image as "secure boot".

**Login and lock are a plain TTY console**, not a graphical screen. Before
authentication the framebuffer is a black monospace text console, like a Linux
machine before a desktop starts. The same console handles first-run setup,
sign-in, screen lock, password changes, re-authentication and recovery. No
lock-screen app is shipped; `security_set_lock_screen()` is a documented API so
you can write one. See [the security design](docs/migration/SECURITY.md).

**Default account:** a fresh machine signs in with `root` / `scos` (shown on the
console until you change it). It has the **highest role (administrator)** and
**every account has top-level permission** by default. There is no separate
built-in superuser account — `root` is an ordinary, renameable account record.
Run `passwd` to replace the weak default password.

Accounts store only random 128-bit salts and PBKDF2-HMAC-SHA256 verifiers in
`system/security/users.db`, with a SHA-256 checksum, a last-known-good backup
and a bounded audit ring. Roles (administrator, standard user, read-only guest)
are only enforced if `roles.enabled` is on; by default all accounts are treated
as administrators. `useradd <name>` creates an administrator too; lower-power
accounts are an explicit choice (`useradd <name> user|guest`). Passwords never
appear in command arguments, history, scrollback or logs.

**User choices:** `choices` in Terminal or TTY lists 16 switches; `choices set
<key> on|off` changes one; `choices default` restores defaults. They
control `login.required`, `lock.enabled`, `lock.idle`, `password.policy`,
`password.history`, `password.age`, `lockout`, `guest.restrict`, `audit.enabled`,
`vault.encryption`, `recovery.enabled`, `isolation.prompt`, `isolation.default`
and `confirm.destructive`, plus `roles.enabled` (off = everyone has top
permission, on = lower-power roles are enforced) and `login.hint` (the
default-credential hint). Choices are persisted in the AEAD-authenticated
vault header, so they survive reboot and an offline edit is detected. A
Settings app can be built on `security_choice*()` in `security.h` — no app is
shipped, by design.

**Process isolation is a per-app user choice, never forced.** On the first
user-driven launch of an app the WM prompts: always isolated / never isolated /
isolated once (ask next time) / unisolated once (ask next time). The answer is
saved (`choices app <id> always|never|ask`).

An isolated app becomes a **real user-mode process at CPL3**. An installed CAT
package runs its Lua VM, bytecode, heap and stack in its own PML4; kernel memory
is supervisor-only there, so reading it, executing `mov cr3`/`wrmsr`/`cli`, or
touching another app's pages is stopped by the CPU and contained as an app
crash, not a kernel panic. Everything it does is a numbered syscall checked by
a default-deny allowlist (seccomp), a private mount namespace, a capability set
and cgroup-style memory/file/syscall/CPU limits. See [the user-mode ABI](docs/USERMODE.md).

Every isolated window also gets private physical frames, a CR3 switch around
callbacks, and kernel-held capabilities that deny global theme edits,
notifications/dialogs, icon changes and VFS paths outside its namespace.
`isolation` lists the running processes with their domains and capabilities.
Apps the owner chose *never* to isolate keep the original unrestricted
behaviour. Built-in native C apps are compiled into the kernel image (like
Linux kernel modules): they get the domains, namespaces, capabilities and
limits, but their C code still runs at CPL0. Each account also gets a private
`home/users/<name>/` tree enforced at the VFS boundary.

An anti-rollback anchor in a GPT-reserved sector outside the saved image lets
boot reject an older (still authenticated) vault image after a replay.

**Data at rest:** with `vault.encryption` on (default) the saved filesystem is
an `SCOSVAUL` image — AES-256-CTR ciphertext with an encrypt-then-MAC
HMAC-SHA256 tag. The cleartext header (account DB, audit log, choices, key
slots) is the AEAD's associated data, so editing any of it is detected at
unlock. A random 256-bit volume key is wrapped under each account's
password-derived KEK; the recovery code has its own slot. Signing in unlocks
the disk; signing out burns the key and `save` fails closed. With
`vault.encryption` off, saves are plain `SCOSFS2` — the owner's choice.

On a machine without the unique verified ATA SCos-data partition, accounts and
choices are **session-only** and the default `root`/`scos` account is recreated
at each boot. See
[the full security design](docs/migration/SECURITY.md) for formats, commands,
the lock-screen app contract, verification and the honest limitations: process
CPL3 isolation for installed apps and a complete namespace/capability model for
built-ins, disk-local (not TPM) rollback detection, and boot verification is
CRC-only with no signing.

## Write your own apps

All preinstalled desktop apps are **native C**, including **App Studio**.
Counter and Sketch are optional Lua demonstrations in `/home/demos/*.cat`: open
them in Files and approve installation. They are not preinstalled. Run `appstrt studio` to edit a project, syntax-check it with
the real Lua 5.4.9 compiler, build a `.cat` package and launch it with F5.
Studio supports range selection and Ctrl+C/X/V with a 16 KiB clipboard and
64 KiB source limit. Raw `.lua` launching is removed. API 2 offers 56 functions for drawing, widgets,
window control, private data, system queries and confirmed global themes.
Settings selects four presets or up to eight custom palettes/backgrounds.

See the [Studio and API guide](docs/LUA-APPS.md), [CAT format](docs/CAT-FORMAT.md),
[current verification record](docs/migration/APP-MANAGEMENT-VERIFICATION.md), and
[open-source provenance/licenses](third_party/README.md). This remains a
kernel: installed apps built against the CPL3 ABI run in real user-mode
processes, while the built-in apps remain trusted CPL0 components. See
[USERMODE.md](docs/USERMODE.md). RAM-versus-ATA persistence limitations still
apply. The [earlier Lua verification](docs/migration/LUA-VERIFICATION.md)
is historical, not the current package contract.

## Desktop and windows

The bottom bar has **Launcher**, **Windows** (an open/minimized-window picker),
up to eight width-limited pinned app shortcuts, plus power and clock controls.
Use Applications to launch, pin/unpin or uninstall external apps. Drag with either mouse button
on the desktop to select icons; a stationary right click opens the context menu
on release. Installed CAT apps get a desktop shortcut and a launcher entry.

The interaction layer from the [Linux `scoswm-arch` experiment](https://github.com/Joercat/scoswm-arch/tree/arena/bb6dd9f1-scoswm-arch)
is back in native SCos:

* **Right-click "Windows N"** for a full-screen MRU **visual window overview**
  with live previews of every window's own framebuffer ([screenshot](docs/migration/wm-overview.png)).
  Click a tile to focus it; arrow keys/Tab move the selection; Esc or another
  right-click closes it. The taskbar stays visible and notices step aside.
* **Global hotkeys**: `Alt+Tab` / `Alt+Shift+Tab` cycle, `Alt+F4` closes,
  `Alt+F9` minimises, `Alt+F10` maximises, `Super+D` opens the launcher,
  `Super+Enter` Terminal, `Super+M` SysMon, `Super+,` Settings, `Super+Q`
  closes. `Super+←/→/↑/↓` and dragging a titlebar to a screen edge **snap**
  windows to halves / maximise / restore.
* **Live desktop preferences** in Settings: taskbar height, desktop icon
  size, wallpaper grid, and switches for hotkeys, the overview and snapping.
  They persist in `home/desktop.cfg` (inside the encrypted vault) and default
  to the original SCos geometry.
* **Power menu**: Lock is immediate; Restart and Power Off go through a
  confirmation whose safe keyboard default is **Cancel**.

See [the port notes](docs/migration/WM-PORT.md) for the full mapping and what
X11/Linux-specific pieces were deliberately left behind.

The compositor reconstructs clipped damage regions from the wallpaper upward,
and GOP scanout uses write-combining rather than uncached pixel writes. This is
still a one-active-CPU kernel, not SMP, and GOP does not provide a hardware-vsync
or page-flip driver. Emulator pixel tests are not a promise of tear-free PC output.

## Build and emulator

On Linux x86-64: GCC, GNU binutils (including the `i386pep` **AMD64 PE32+** linker
emulation), Make, Python 3 and the standard Linux C development headers. Tested
with GCC 12.2.0 and binutils 2.40. Headers provide declarations for the Lua port;
no host libc or Windows runtime is linked. No EFI SDK, FAT utilities or mounted
filesystem is needed.

```sh
make                           # build/BOOTX64.EFI, kernel.elf and scos.img
python3 tools/check_milestone.py
python3 tools/tests/test_security.py     # crypto KATs + choices/vault/isolation guest tests
sha256sum -c dist/scos.img.sha256
python3 tools/setup_qemu.py     # optional pinned host QEMU environment
python3 tools/run_qemu.py --xhci # x64 EDK2/q35, native USB HID, disposable disk state
make clean
```

The packaged `dist/scos.img` is a 64-MiB GPT disk with a FAT32 EFI system partition:
`EFI/BOOT/BOOTX64.EFI`, `SCOS/KERNEL.ELF`, `SCOS/KERNEL.CRC`, `SCOS/DRVLIST.IDX`,
`SCOS/BUILD.TXT` and the GPU `.MOD` files. There is no signature file and no
key material: boot signing was deliberately removed. It is a development
integration artifact, not a request to flash or physically test the PC now.
It also contains a separate 128-KiB SCos-data partition. Only the verified ATA
path can save there, and the saved image is encrypted by default; the q35/AHCI
guest cannot persist. Neither a USB boot medium nor the xHCI HID driver implies
USB mass-storage support.

A later physical test must use **x64 UEFI boot, not the previous CSM configuration**.
Boot verification is intentionally absent, so firmware Secure Boot (if enabled
on that machine) will not recognise this image; the owner decides what to enrol.
The firmware must provide GOP direct framebuffer access and a readable boot
filesystem. Writing a raw image to an entire USB device destroys existing
contents; identify/back up the device first. Successful emulator USB boot does
not establish motherboard/GPU acceptance.

## Implementation details and evidence

[Existing-source completion audit](docs/migration/CONVERSION-AUDIT.md) accounts
for every original kernel C file and records the restored startup presentation,
native fault diagnostics and interrupt-rate protection. No new drivers were added.

[App/desktop port provenance, fixes and verification](docs/migration/APP64.md)
documents the current integration.

[Native UEFI startup contract, limits and verification](docs/migration/BOOT64.md)
covers the loader, relocations, firmware exit/retry rules, memory ownership,
GOP console, error paths, allocator, interrupts and exact testing scope.

* [Conversion roadmap](docs/migration/README.md) — the user chooses the next part.
* [Host emulator provenance and usage](docs/migration/EMULATOR.md).
* [Research only: GPU, networking and browser options](docs/migration/HARDWARE-AND-BROWSER.md).

No new GPU/NIC/browser stack was imported. The GOP console uses firmware-provided
pixels, not a native GPU acceleration driver. The existing architecture-independent
SCos bitmap font and original desktop drawing primitives are reused. No BIOS,
VBE startup or obsolete i386 allocator was brought back.

## Frozen historical milestone

Per the user's explicit choice, `dist/scos-32bit.img`, its checksum and
[provenance](docs/milestones/scos-32bit.json) remain permanently frozen at r43.
They are an archive, **not a build target or dependency of the UEFI loader**.
The integrity guard protects that retention. Previous implementations remain
in Git history; duplicate legacy source/build trees have been removed.
See [historical r43 behavior and limitations](docs/RELEASE-r43.md).

## Application management and graphics status

There is no application-manager window: each function is reached from the surface it acts on. Right-click
a desktop icon for that icon's own menu (open, pin or unpin, hide, and for a `.cat` app, uninstall),
right-click empty wallpaper to restore hidden icons or open Files or Terminal, right-click a taskbar
entry to unpin it or put its icon back, and right-click a row in Files to open it with another
application, rename it, pin it to the desktop or be sent to `rm -s`. Pressing an entry in the launcher and
moving drags that application onto the desktop (installing its shortcut) or onto the taskbar (pinning
it); dragging a row out of Files drops the file onto a window (open it there), a taskbar entry (open it
with that application) or the desktop (a shortcut). `appuninstall <id>` requests
default-no confirmation in Terminal or TTY. Built-ins cannot be uninstalled;
external removal closes instances and removes the registered installed package,
shortcuts and pins, but keeps projects, app data and original external sources.
RAM-only installations are explicitly session-only and disappear at reboot.
Only existing supported ATA persistence is available; no new storage drivers.

Top-right notifications overlay windows, menus and modals. Click to dismiss;
normal notices expire after ten seconds. Lua apps can use `scos.notify(text)`.

`graphics` reports read-only PCI display-device discovery and the CPU renderer.
**GPU acceleration and working hardware-driver selection remain unimplemented.**
The boot warning explicitly reports CPU fallback. GOP/PAT/write combining are
not GPU rendering. Browser integration remains deferred.

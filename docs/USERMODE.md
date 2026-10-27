# SCos user-mode apps (CPL3)

SCos applications that are **isolated** by the owner do not run as trusted
ring-0 code any more. They run in a real x86-64 **user-mode process**: CPL3, its
own PML4, its own stack/heap, and a numbered syscall gate. This is the same
shape as the Linux/Flatpak sandbox stack — hardware privilege separation, then
namespaces, seccomp, capabilities and cgroups on top — implemented directly in
this kernel rather than by porting Linux.

```
        CPL3 (per app)                         CPL0 (kernel)
  +-------------------------+          +--------------------------+
  | userland.elf            |  SYSCALL | usermode.c dispatcher    |
  |  - Lua 5.4.9 VM / heap | -------> |  - default-deny allowlist |
  |  - app source + state  |          |  - private mount root     |
  |  - stack, arena         | <------- |  - capability checks      |
  |  - sc(nr, a1..a5)      |  SYSRET  |  - resource limits        |
  +-------------------------+          +--------------------------+
     PML4: only this app's               kernel pages are
     frames are U/S=1                    supervisor-only here
```

## What the hardware enforces

| Attempt from CPL3 | Result |
| --- | --- |
| read/write kernel memory | `#PF` (supervisor-only pages) — contained, app terminated |
| read another app's memory | `#PF` (absent from this PML4) |
| `mov cr3`, `wrmsr`, `cli`, `in`, `out` | `#GP` — privileged at CPL3 |
| call a kernel function directly | not reachable: kernel `.text` is not mapped for CPL3 |
| jump to a kernel address | `#PF` |
| unknown syscall number | refused by the seccomp allowlist (`-EPERM`), app continues |

A fault is an **app crash, not a kernel panic**. `interrupt_dispatch()` sees a
CPL3 frame, `user_fault()` records it, and `isr_common` jumps straight to
`sc_user_fault_return`, which adopts the kernel continuation saved by
`user_enter()`. No IRETQ frame rewriting is involved (that would #GP).

Kernel data remains mapped in the app's PML4 but **supervisor-only**. The
syscall handler runs at CPL0 with that same CR3, so it needs no CR3 switch and
can still reach kernel state. This is why the boundary is cheap: one `SYSCALL`
plus a stack switch.

## Linux-inspired layers

| Linux primitive | SCos implementation | Where |
| --- | --- | --- |
| user mode / CPL3 | `SYSCALL`/`SYSRET`, `EFER.SCE`, `STAR`, `LSTAR`, `SFMASK`, DPL-3 GDT segments, TSS `RSP0` | `kernel/x86_64/usermode.S`, `kernel/src/interrupt.c` |
| namespaces (mount) | every path is a **basename** resolved under `home/appdata/<id>/`; `/`, `..` and separators are rejected before any VFS call | `ns_path()` in `kernel/desktop/usermode.c` |
| namespaces (PID/IPC/net) | only the app's own window is visible; no process list, no cross-app messages, no network | dispatcher `SC_NR_WINDOW`, `SC_NR_FOCUSED`, `PROC_CAP_IPC` |
| user namespaces / capabilities | `PROC_CAP_VFS_READ/WRITE/THEME/IPC/APPCTL/SYSINFO` held in kernel memory | `process.h`, `cap_ok()` |
| seccomp-bpf | default-deny allowlist over `SC_NR_1..SC_NR_MAX`; owner-adjustable with `user_set_seccomp()`. The syscall number is compared at full 64-bit width and capabilities are checked again per file syscall | `allow[]`, `user_seccomp_allowed()`, `cap_ok()` |
| cgroups | memory, file count, file bytes, syscall count and CPU-time budgets | `limits[]`, `USER_RLIMIT_*` |
| Landlock-style point-of-use checks | syscalls validate strings/lengths against the shared page before touching the VFS | `scratch_at()`, `ns_path()` |
| no_new_privs | an app can never gain a capability; CPL3 cannot install a syscall entry | hardware |

## Syscall ABI

`long sc(long nr, long a1, long a2, long a3, long a4, long a5)` — implemented in
`kernel/user/start.S`. `SYSCALL` uses `r10` instead of `rcx` for argument 4 and
clobbers `rcx`/`r11`; the stub reshuffles before executing it. Numbers are in
`kernel/user/user_syscalls.h` (kept in sync with `kernel/include/usermode.h` by
`tools/tests/test_usermode.py`).

Strings and bulk data never pass as pointers: they travel in a 128 KiB shared
page (`struct user_shared`) that the kernel maps read/write for that process
only. `scratch_at()` bounds every string before use. The kernel writes the
event (open/paint/key/mouse/tick/close), the CAT source and the input state;
the app writes `result`, and the kernel records fault/denial/syscall counters.

Grouped numbers:

* `1..12` — exit, log, clock, date, screen, mouse, app id, window, focused,
  memory, interval, text width
* `13..20` — draw (16 subcommands), redraw, title, resize, move, minimize,
  notify, message
* `21..24` — theme query/list/apply/status
* `25..31` — file exists/size/read/write/remove/rename/list
* `32..34` — api info, set icon, fault report

## The CPL3 app server

`build/userland.elf` is a second, absolute-linked ELF (see
`kernel/user/user.ld`), flattened by `tools/embed_userland.py` and embedded in
the kernel image. It contains:

* `kernel/user/start.S` — entry + the `sc()` syscall stub
* `kernel/user/app.c` — event dispatch and hardware escape probes
* `kernel/user/cat_user.c` — a **Lua 5.4.9 host** implementing the full API v2
  `scos` table via syscalls
* `kernel/user/ulibc.c` — tiny freestanding libc
* `build/lua-runtime.o` — the same Lua VM/math objects the kernel uses

Linking it absolute at `USER_BASE` means no ELF loader and no runtime relocation
in the kernel. `user_attach()` maps its text pages R-X (U/S=1), its data/bss and
1 MiB Lua arena RW-NX, a 64 KiB stack and the shared page — all private frames.
`user_run_event()` fills the shared page and `user_enter()` SYSRETs into it.

Because the image is embedded and stamped with the delivery, an attacker who
can write the ESP cannot replace the code behind the boundary without also
changing the kernel image (which the CRC32 is there to detect as corruption —
see `SECURITY.md` for why that is not authentication).

## Which apps run here

* **Installed / third-party CAT packages** — when the owner chooses "always
  isolated" (or "isolated once"), the whole package (Lua VM, bytecode, heap,
  state) runs at CPL3. `kernel/desktop/lua_apps.c` becomes a thin dispatcher;
   the kernel-side Lua VM is never created for that window.
* **Built-in native C apps** — compiled into the kernel image, like Linux
  kernel modules. They are trusted OS components, not sandboxed applications.
  They still get private page-table domains, a private mount namespace, a
  capability set and resource limits at the kernel boundary, but their C code
  executes at CPL0. Porting one to CPL3 means compiling it into `userland.elf`
  against this syscall ABI; the interface above is the contract.

The per-app choice is unchanged: desktop/launcher/pin first launch asks
"always / never / isolated once / unisolated once", `choices app <id>
always|never|ask` changes it later, and `isolation` lists the live processes
with their domains and capabilities. `isolation.default` is off by design —
nothing is forced.

## The same API, both ways

An owner may run any CAT package isolated or not, and the choice must not change
what the package's own code does. Both hosts are therefore the same API v2:

| | kernel-side host (`lua_apps.c` + `lua_api.inc`) | CPL3 host (`cat_user.c`) |
|---|---|---|
| `scos` table | 59 entries | the same 59 entries |
| callbacks | `open()`, `paint(w,h)`, `tick(seconds)`, `key(code,down,shift,ctrl,alt)`, `mouse(type,x,y,button,down,wheel,buttons)`, `close()` | identical argument lists |
| theme / date | live `theme()` + `date()` table | same values through the shared page |
| `interval(ms)` | 100 … 60000 ms scheduling | stored per process, honoured by the scheduler |
| drawing | 4,000,000-pixel budget per callback, `drawing budget exceeded` | same budget per episode, same error |
| data files | `home/appdata/<id>/`, 16 files / 64 KiB | same namespace via syscalls |

Capabilities are the only difference, and they are the owner's choice: an
isolated process holds `VFS_READ|VFS_WRITE` only, so `theme_apply`, `notify`,
`message` and `set_icon` raise the same "disabled by the owner" error the kernel
raises for any process without the capability. Reads of the theme, the date,
the window and the mouse are never restricted.

`tools/tests/test_escape.py` proves this by running one script twice (isolated
and not) and comparing the signature it computes from the API, so the two hosts
cannot drift apart silently.

## The escape app

`apps/examples/isolation.lua` ships as `/home/demos/isolation.cat`. It is an app
whose only job is to try to break out of its own sandbox and report what
happened on screen and in `home/appdata/isolation/report.txt`. It attempts
about fifty distinct attacks: `..`/absolute/backslash/NUL/long-name traversal in
every file call, other-app and cross-account reads, theme takeover, notification
and dialog spam from a process without the capability, out-of-range geometry,
huge radius/scale/text, the 16-file and 64 KiB quotas, a memory bomb, drawing
outside `paint`, drawing past the pixel budget, `string.dump`, and the presence
of `load`, `dofile`, `os`, `io`, `package`, `debug`, `coroutine` or a raw
syscall handle. Each result is `CONTAINED` or `BREAKOUT`; pressing `R` reruns
the suite. The build's own test requires `breakout=0`, no CPU fault and no file
left outside the app's namespace.

Only install packages you trust: a checksum is not a signature. Isolation limits
what a hostile package can reach; it is not a promise that the package is safe.

## Verification

`python3 tools/tests/test_usermode.py` boots QEMU and proves, with real
instructions, that ring-3 code cannot read or write kernel memory, execute
`mov cr3` / `wrmsr` / `cli` / `outb`, read another address space, leave its
mount namespace, or use an unlisted syscall — and that the kernel survives all
of it. `test_isolation.py` additionally proves the per-process PML4s, private
VFS roots, per-account files and anti-rollback anchor. `test_escape.py` runs the
escape app under the CPL3 host and fails if a single probe breaks out, then
compares the API signature of an isolated and a non-isolated run.

Emulator success is not physical-PC acceptance.

## Hardening notes

`docs/migration/HARDENING.md` records a full audit of this boundary: a 32-bit
truncation that let `0x100000001` act as `SC_NR_EXIT`, an `off + n` wrap that
could write outside the shared page, missing capability checks, unclamped draw
arguments (a CPU-render denial of service), unbounded scratch strings and
namespace escapes through unsanitised app ids. All are fixed and covered by
`tools/tests/test_hardening.py`, which issues the hostile syscalls from real
CPL3 code.

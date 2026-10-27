# Security audit: findings, fixes and regression coverage

This pass was requested as "double check security features for bugs, leaks,
vulnerabilities and fix them then test and repeat until no more show
themselves". Each item below was found by reading the code, fixed, and then
made into an assertion in `tools/tests/test_hardening.py` (or an existing
suite). The suite was re-run after every group of fixes.

## 1. The idle timeout was ten times too short

`security_wm_tick()` compared `idle > idle_minutes * 600u`. The PIT divisor is
11932, i.e. **100 Hz**, so one minute is 6000 ticks. A "10 minute" idle locked
the machine after **one minute** — which is exactly what it feels like when the
system "times out while in use". Now `TICKS_PER_MINUTE 6000u`, with a
side-effect-free `security_idle_would_lock()` predicate so the scale is
testable without actually opening the lock screen.

## 2. Not every interaction reset the timer

`kbd_enqueue()` only stamped `input_last_tick` when `input_guard_armed`, and
`mouse_apply()` only when `btns || dx || dy || wheel` — a **button release with
no movement did not count**. Now there is one `security_note_activity()` helper,
called from:

* `kbd_enqueue()` for every key event, press or release;
* `mouse_apply()` for any real change (button transition, movement or wheel) —
  an idle HID report with no change is deliberately *not* an interaction;
* `handle_key()` / `handle_mouse()` when the WM consumes an event, which also
  covers injected/programmatic interactions;
* sign-in, unlock, logout and pending-flow completion (they used to assign the
  timestamp directly).

`idle` and `activity` terminal commands expose the timer; the Settings
Security section edits `policy idle` and `lock.idle`.

## 3. CPL3 boundary: seccomp number truncation

`user_seccomp_allowed((int)nr)` narrowed RAX to 32 bits. A user-mode app could
issue `0x100000001` and have it treated as `SC_NR_EXIT` (1), jumping straight
to the exit path regardless of the allowlist. The check now compares the full
64-bit value before any index. `user_seccomp_allowed()` takes `uint64_t`.

## 4. CPL3 boundary: integer wrap in FILE_WRITE

`do_file()` cast the 64-bit offset/length arguments to `u32` and then tested
`off + n > sizeof(scratch)`. With `off = 0xffffffff` and `n = 2`, the sum wraps
to 1 and the check passes — a `vfs_write`/`memcpy` far outside the shared page.
The arguments are now validated **as 64-bit values** and all bounds are
subtraction-only (`b >= size || c > size - b`). `FILE_LIST` had the same class
of bug (plus a NUL write one byte past the buffer) and was fixed the same way.

## 5. CPL3 boundary: capabilities were not checked for files

The process capability mask existed but `do_file()` only used `ns_path()`.
`PROC_CAP_VFS_READ` / `PROC_CAP_VFS_WRITE` are now enforced per syscall, and a
dispatch with no attached process (`active_pid == 0`) is denied for everything
except EXIT. A process that somehow lost a capability can no longer regain it
by choosing a legal filename.

## 6. CPL3 boundary: unclamped drawing arguments

`do_draw()` passed attacker-controlled coordinates, radii and text scales
straight into CPU drawing primitives; `s_circle()` with `r = 0x7fffffff`
would spin for billions of iterations, and `x + w` could overflow a signed
int. All values now pass through `draw_coord()` / `draw_extent()` clamps
(±4096, 0..4096), icon ids are range-checked, and text scale is 1..4. This
matters because SCos is CPU-render-only — there is no GPU to absorb a flood.

## 7. CPL3 boundary: unbounded strings

`scratch_at()` could hand a 64 KiB string to `s_text()`, `s_text_width()`,
`wm_set_title()` or `klog()`. It is now bounded to `USER_STR_MAX` 256 bytes
with a bounded copy when the string is not terminated inside the bound.

## 8. Namespace escape through app ids

`app_register()` accepted any characters in an id, but ids become VFS
directory names (`home/appdata/<id>/`) and choice keys. It now rejects
separators and `..` at the single registry gate, so **any system app added
later** is covered. `proc_create()` sanitises the id again before storing it,
and `user_attach()` sanitises it before building the private namespace.
`ns_path()` rejects `..` segments as well as separators.

## 9. Per-account private files: enumeration and traversal

`vfs_user_private()` allowed the bare `home/users` parent (which lists every
account name) and only compared the first path segment, while `vfs_lookup()`
honours `..`. It now denies the parent directory, any path whose first
segment is not the signed-in account, and any `..` segment in the remainder.

## 10. A one-launch isolation override leaked into nested windows

`iso_once` was cleared *after* `wm_open_app()` returned, so a dialog or second
window opened from an app's `open()` callback inherited the same override -
an unisolated launch could spawn an isolated system window. `wm_open_app()`
now snapshots and clears the override before creating anything, and the
hardening suite asserts this in the source.

## 11. Privilege and robustness

* `security_choice_set()` has no internal admin check; the Settings Security
  section and the new `isolate <id> ...` command now check `security_is_admin()`
  (as the `choices` shell command already did) and refuse with a transient
  notice rather than a new window. `security_api_policy_set()` validates
  through `sc_policy_valid()` and checks admin.
* `security_api_lock_now()` is a single audited entry point for requesting the
  lock screen from an app, shell or menu.
* App paint callbacks are no longer invoked when a window has no surface
  (allocation failure); previously an OOM during maximise/snap could hand an
  app a NULL `surf.px`.
* `wm_prefs_init()` now parses `home/desktop.cfg` from a private copy instead
  of modifying a VFS-owned buffer in place.
* Input events are mixed into the audited entropy pool (throttled to 1 in 16
  events), so interaction timing contributes to volume-key/salt/IV entropy.

## 12. CPL3: the kernel stack was misaligned for every syscall

`SYSCALL` does not switch stacks, so `sc_syscall_entry` adopts the RSP saved by
`user_enter` and calls `user_syscall_dispatch` on it. `user_enter` saves its own
entry RSP, which is 8 mod 16, and the call therefore ran with the stack the
SysV ABI forbids. GCC was entitled to use an aligned `movaps` spill - and did,
inside the `SC_NR_DATE` record copy - so a perfectly ordinary `scos.write()`
raised `#GP` and, because it happened in ring 0, panicked the kernel. The stub
now aligns once (`andq $-16, %rsp`) for every syscall, and both return paths
restore the saved kernel RSP first. Found by the escape app on its 30th probe.

## 13. The write quota counted lifetime writes, not live files

`FILE_WRITE` kept `ns_files` in kernel RAM and only ever incremented it, so
deleting a file left the slot charged. After sixteen writes in an app's life -
its own report file, notes, saved settings - every later `scos.write()`
returned false, for that app, forever, across relaunches of the window. The
quota is now measured from the app's private directory at the moment of the
write (`files` and `bytes` are summed from the VFS, with the replaced file's
size subtracted), which is what the kernel-side host has always done.

## 14. `scos.files()` could never succeed for an isolated app

`SC_NR_FILE_LIST` takes a *scratch offset*, not a file name, but it ran through
the same `ns_path()` validator as the other file calls. Every listing failed as
"invalid data filename", which the CPL3 host then reported as a capability
denial - so a package could not enumerate its own directory in the isolated
host while the kernel host listed it fine. LIST now has its own branch before
the name check, with the same subtraction-only bounds it always had.

## 15. A spinning package could hold the machine forever

The CPU budget was only tested inside the syscall dispatcher, so
`while true do end` never reached it: a real 1-second spin was observed holding
the desktop, and a debugger stop could not even reach the idle boundary. The
timer interrupt now charges the episode too, but only when it interrupted CPL3
code (`CS.RPL == 3`), so it can never abandon a kernel-mode syscall's stack.
The WM reports "exceeded its CPU budget (contained)" instead of a fault, and
`test_escape.py` verifies the desktop keeps working afterwards.

## 16. Nested C-call recursion overflowed both stacks

`LUAI_MAXCCALLS` was 48, and 200 before that. Each nested C call (pcall, a
`string.gsub` callback, a `table.sort` comparator) uses the real C stack, which
is 64 KiB for the kernel host and for each CPL3 process. At 48, a gsub callback
that recursed page-faulted the app at CPL3 and would have overrun the stack the
compositor itself runs on in the kernel host. It is now 16, so the VM raises
the ordinary "C stack overflow" Lua error before either stack is touched.

## 17. The two CAT hosts had drifted apart

Isolation is the owner's choice, so a package must behave the same either way.
Comparing the hosts found: `scos.themes()` returned *indices* at CPL3 and IDs in
the kernel; `scos.time()` divided milliseconds by 100 instead of 1000; the icon
range was 0..23 in one host and 0..9 in the other; `set_icon` was accepted and
silently ignored at CPL3; `move()` returned nothing there; an unfocused window
still reported mouse buttons; `file_size`/`read`/`write`/`remove`/`rename`/
`files` mapped "absent" and "denied" onto each other; `log` had a 1024-byte cap
in one and 512 in the other; `theme_apply` skipped ID validation; and drawing
over budget was a silent no-op at CPL3 instead of an error. Each is now the same
call with the same arguments, the same range, and the same error text, and
`test_escape.py` runs one script in both hosts and compares its API signature.

Note the deliberate difference that remains: capabilities. An isolated process
holds `VFS_READ|VFS_WRITE|SYSINFO` only, so `theme_apply`, `notify`, `message`
and `set_icon` raise "disabled by the owner" - which is exactly what the kernel
host raises for a process without the capability.

## 18. The build stamp could lag the commit it names

Provenance is a security property here: an image that misreports its own commit
lets a reviewer judge code that is not the code on the stick. Two Makefile
faults allowed exactly that. The stamp header was an *order-only* prerequisite
of every object that `#include`s it, and make never compares the timestamp of an
order-only prerequisite, so a commit changed nothing and the kernel kept the
previous id, after which `tools/makedisk.py` refused to pack it (correctly) and
the only workaround was to run `make` twice. And `desktop-buildinfo.o` - the one
file that exists to read the stamp - had `FORCE` with no dependency on the
header, so it could be compiled against the header make was about to replace.
The header is now a normal prerequisite everywhere it is read, `buildinfo.o`
depends on it ahead of `FORCE`, and `test_buildinfo.py` fails if any rule ever
moves it back behind a `|` or if the packed kernel does not contain the stamp
the tree describes.

## Verification

`python3 tools/tests/test_hardening.py` boots QEMU and verifies:

* a 7-second idle does **not** lock a 1-minute policy while a 70-second idle
  does; key, mouse press, **release with no movement** and wheel each reset
  `security_idle_ticks()`;
* every registered built-in is a system app (no prompt, unisolated), the
  explicit override isolates it, a **newly registered** system app is treated
  the same, a malicious `../evil` id is refused, installed apps honour stored
  choices without re-asking, and the context-menu override works both ways;
* a full-width syscall number (`0x100000001`) is denied, and four hostile
  syscalls are issued from **real CPL3 code** (offset wrap, seccomp truncation,
  huge list offset, namespace traversal) — all refused cleanly with no fault
  and no panic;
* `home/users` enumeration and `home/users/root/../other/x` are denied.

* the escape app (52 probes) reports `breakout=0`; one script run isolated and
  run normally produces an identical `scos` API signature; a package that spins
  without syscalls is stopped by the timer and the desktop keeps working;
* the power confirmations are single centred dialogs that vanish on the close
  button, an outside click, Cancel and Escape, leaving no window and no reboot.

All 21 suites pass (including the new `test_escape.py` and
`test_wm_dialogs.py`). Emulator success is not physical-PC acceptance.

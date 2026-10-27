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

All 19 suites pass (including the new `test_hardening.py`, `test_wm_port.py`
and `test_usermode.py`). Emulator success is not physical-PC acceptance.

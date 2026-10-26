# SCos accounts, sign-in and access control

This document describes the account subsystem added without changing the shape
of the desktop: the same apps, windows, themes, filesystem commands, Lua CAT
packages, drivers and build targets still work. A sign-in gate now runs **before**
the window manager accepts any input, and a small number of well-defined hooks
enforce identity inside the two shells and the filesystem.

Read the limitations section before treating any of this as a security boundary
against a determined owner of the machine.

---

## 1. What existed before, and what changed

Before this work `whoami` returned the literal string `"user"`. There was no
identity, no password, no lock screen and no distinction between the desktop and
the kernel maintenance console. `os.html` (the design reference) is untouched.

New files (the subsystem is self-contained):

| File | Role |
| --- | --- |
| `kernel/include/security.h` | Public API, limits, on-disk record types. Freestanding; also used by host tests. |
| `kernel/desktop/sec_core.c` | SHA-256, HMAC-SHA256, PBKDF2-HMAC-SHA256, constant-time compare, hex, password/username policy, record codec, database checksum. **No kernel dependencies** — the same object is compiled on the host for known-answer tests. |
| `kernel/desktop/security.c` | Account database, session state, lockout, audit ring, role checks, shell commands, entropy pool. All state is in BSS. |
| `kernel/desktop/login.c` | Full-screen sign-in / setup / lock / masked-prompt UI. |
| `tools/tests/security_host.c`, `tools/tests/test_security.py` | Host KATs plus booted-guest regression. |

Existing files changed by a total of a few dozen lines: `scos.h` (one include),
`platform.c` (init + gate), `wm.c` (one safe tick), `vfs.c` (path guard),
`app_terminal.c` and `tty.c` (one dispatch + one authorization table each),
`theme.c` (admin-only reset), `Makefile` (header dependency), `README.md`, and
`tools/tests/qemu_support.py` (guest tests now sign in). No command handler, app,
driver or compositor algorithm was rewritten.

---

## 2. Threat model — what this actually defends

**It defends:**

* Someone walking up to a running or booting SCos machine and using the desktop,
  the Terminal, the TTY console, or the filesystem without an account.
* Online password guessing (per-account and global rate limiting, exponential
  lockout, constant-work verification, generic error text).
* Offline cracking of a copied `users.db` (random per-user 128-bit salt, a
  calibrated PBKDF2 work factor, strong password policy, no plaintext verifier).
* Casual tampering with credentials from inside the OS: `rm -s`, Notepad "save
  as", Files, Lua apps and both shells cannot read, write, rename or delete
  `system/security/*`. A checksum plus a last-known-good backup detects edits.
* Credential leakage through the UI, terminal echo, scrollback, history, klog or
  the audit log. Passwords are never drawn, logged or stored; only `*` markers
  are painted, and secret buffers are scrubbed.
* An unattended session (auto-lock), and a forgotten password on a machine with
  a persistent save target (one-time recovery code).

**It does NOT defend** (and no wording here claims otherwise):

* **No process isolation.** SCos is still a cooperative, single-address-space,
  ring-0 kernel. A native C app, a loaded GPU module or a Lua app is not a
  sandboxed process. This is an *authentication and access-control layer*, not a
  sandbox. A malicious native app can read kernel memory directly.
* **No signed boot / measured boot.** Secure Boot must currently be disabled for
  the unsigned image. Someone who can replace `BOOTX64.EFI` or `KERNEL.ELF` on
  the boot medium replaces the security code too.
* **No disk encryption.** `users.db` contains only salted PBKDF2 verifiers, but
  the rest of the SCos-data partition is plaintext. The SHA-256 database
  checksum is an **integrity check, not a MAC**: an attacker with write access to
  the disk can recompute it. Encrypted/signed storage needs a secret that an
  unsigned boot cannot keep.
* **Physical access wins.** A hardware debugger, a JTAG/ICE probe, or the QEMU
  GDB stub used by the test suite can call any kernel function. The QEMU harness
  deliberately calls the *production* setup/authentication entry points rather
  than a test-only bypass; there is no `#ifdef` backdoor in the kernel.
* **Rescue console.** `tty_run(0)` (WM already dead) skips re-authentication
  because there is no compositor to return to. Reaching it requires stopping the
  WM, which is an administrator-only command; a stopped desktop is already a
  physical-access situation.
* **RAM-only machines.** Without the unique verified ATA SCos-data partition,
  accounts are **session-only**: every boot shows first-run setup again. The
  secure screen says so explicitly. Use `save`/a supported ATA target for
  accounts that survive reboot.

---

## 3. Cryptography

All primitives are implemented in `sec_core.c` in about 400 lines of portable
C11, with no libc/libm calls:

* **SHA-256** (FIPS 180-4) with a streaming context. Verified on the host against
  the empty string, `"abc"`, the 56-byte two-block vector and one million
  `'a'`s.
* **HMAC-SHA256** (RFC 2104), with ipad/opad states precomputed once per
  password. Verified against RFC 4231 cases 1 and 2, plus the >64-byte key path.
* **PBKDF2-HMAC-SHA256** (RFC 8018), one 32-byte output block. Verified against
  `password`/`salt` at 1, 2 and 4096 iterations and the long-password/long-salt
  vector. A 129-byte password (longer than the HMAC block) is also exercised.
* **Constant-time comparison** for every verifier check and for the database
  digest. Verification of an unknown username performs the same PBKDF2 work as a
  real account (a fixed dummy salt) so response time does not enumerate users.
* **Salts** are 128 bits from an entropy pool seeded at boot from RDRAND (only
  when CPUID reports it), the TSC, the PIT tick count, the RTC, CPUID values and
  input-event counters. Keystrokes on the secure screen are mixed in; the pool
  ratchets with SHA-256 after every block of output. When the CPU has no
  RDRAND the kernel logs a reduced-entropy warning rather than pretending.
* **Work factor** is calibrated once at first-run setup using the TSC against the
  UEFI-measured `cpu_tsc_hz`, targeting roughly 250 ms of hashing, clamped to
  `[20 000, 1 000 000]` iterations. Each account stores its own iteration count,
  so raising `policy iters` never invalidates existing credentials (it applies
  to newly created accounts only). A slow emulator therefore gets a smaller but
  recorded work factor instead of an unbootable login delay.
* **Password history** keeps up to five previous `salt+verifier` pairs per
  account; a new password is rejected if it re-derives to any of them.

No password, hash, salt-in-plaintext-for-display, or recovery code is ever
written to the serial log, klog ring, terminal scrollback or audit file.

---

## 4. Usernames and passwords

**Usernames:** 3–31 ASCII characters, start and end alphanumeric, interior
`[a-z0-9._-]`, no `..`, at least one letter, compared case-insensitively after
normalization. Reserved: `root`, `nobody`, `system`, `security`, `kernel`,
`daemon`, `guest`, `default`, `scos`, `operator`, `superuser`, `sys`, `bin`.
UIDs start at 1000.

**Default password policy** (all administrator-tunable):

| Key | Default | Meaning |
| --- | --- | --- |
| `minlen` | 12 | Minimum length (8–64) |
| `classes` | 3 | Minimum of {lower, upper, digit, symbol} |
| `minbits` | 40 | Estimated entropy floor |
| `maxfails` | 5 | Failures before lockout |
| `lockbase` / `lockmax` | 30 s / 900 s | Exponential lockout window |
| `idle` | 15 min | Auto-lock (0 disables) |
| `maxage` | 0 (off) | Force a change after N days |
| `history` | 5 | Remembered passwords |
| `iters` | calibrated | PBKDF2 iterations for new accounts |

Rejections beyond length/classes: printable ASCII only; no username substring;
a ~40-word common-password blacklist (exact match, or a word that dominates the
string — `"password1234"` is refused); four or more identical characters in a
row; four-key keyboard walks (`abcd`, `9876`); short periodic patterns; and an
entropy estimate below the floor. An accepted password gets a strength label
(fair / good / strong / excellent) with an estimated bit count. The estimate is
a heuristic, not a guarantee — it is advisory, while the hard rules above are
enforced.

---

## 5. Account database

`/system/security/users.db` is a line-oriented text file in the RAM filesystem:

```
SCos-accounts-v1
P|minlen|classes|minbits|maxfails|lockbase|lockmax|idle|maxage|history|iters
R|iters|recovery_salt_hex|recovery_hash_hex|valid
U|uid|name|display|role|flags|created|last_login|pw_changed|fails|lock_until|iters|salt_hex|hash_hex|hist_count|hist_blob_hex
C|sha256_hex_of_all_preceding_bytes
```

* Parsing is strict: field counts, numeric ranges, hex lengths, duplicate
  usernames/UIDs, role/flag bits and the presence of at least one enabled
  administrator are all validated before any state is committed. The parse goes
  into a scratch array first, so a malformed file cannot leave half a table.
* `C|` is a SHA-256 checksum over the preceding bytes. On load a bad checksum
  falls back to `users.db.bak` (the previous good file, rotated on each write),
  logs the fallback, and repairs the primary. If both are unusable the boot goes
  to first-run setup with a clear serial message instead of silently logging in.
* Writing is exclusive to `security.c`: `vfs.c` refuses reads, writes, mkdir,
  rename and delete for the normalized `system/security` subtree (path
  normalization resolves `.`/`..` so `system/x/../security/users.db` is caught).
  `security_fs_restore()` re-materializes the database and audit log after a
  factory reset, so Settings cannot drop the signed-in account table.
* Persistence uses the **existing** verified ATA `save` image only: any account
  change sets a dirty flag and `security_wm_tick()` calls `fs_image_save()`; a
  failed save is audited and retried. No new storage driver was added. Without
  that target, accounts are explicitly session-only.
* `system/security/audit.log` is a bounded ring (48 entries) with RTC-epoch
  timestamps, event name, uid, user and a short detail string. Events include
  boot, setup, login ok/fail, verify, password change/reset, user create/delete,
  role and policy changes, lock/logout/auto-lock, recovery and disk-save failure.

---

## 6. Boot, sessions and screens

1. `desktop_start` builds the filesystem, then calls `security_init()`, which
   seeds the RNG, loads/validates the database and starts the audit ring. A boot
   log line reports this step.
2. After the splash, `security_login_gate()` runs **before** `wm_run()`:
   * No accounts → **first-run setup**: choose an administrator username and a
     policy-checked password twice, then a **one-time recovery code** is shown
     on a screen you must acknowledge. The plaintext code is scrubbed from RAM
     immediately afterwards; only its salted PBKDF2 verifier is stored.
   * Accounts exist → **sign-in**: username + masked password. Generic
     "Invalid username or password." for failures. `F8` redeems the recovery
     code (single use) to reset the administrator password.
   * A successful login can require an immediate password change
     (`MUSTCHANGE` flag, `--reset`, or an expired password); the gate will not
     hand over to the desktop until that is done.
3. Session state tracks the current uid/role, login epoch, lock state, failure
   counters and idle time. `input_last_tick` (already maintained by the input
   drivers) drives auto-lock after the configured idle period.
4. `security_wm_tick()` is called once per WM loop iteration **after all window
   callbacks have returned**. This is the safe point for full-screen secure
   flows: it cannot free a window whose key handler is still on the stack.
   `logout` closes every window before showing sign-in, so the next user does
   not inherit the previous user's documents.
5. Every secure screen owns the framebuffer directly (not a window), draws only
   `*` for masked fields, drains stale keyboard/mouse events, wipes the screen
   on exit and asks the compositor for a full repaint. `Ctrl+Alt+F1`–`F6` (or
   `tty` in Terminal) requires the current user's password on a masked screen
   before the maintenance console opens.

---

## 7. Roles and command authorization

| Role | Powers |
| --- | --- |
| `administrator` | Everything, including account management, policy, audit, `save`, power actions, `rm -s`, `kill --system`, theme changes and factory reset. |
| `standard user` | All normal apps/filesystem commands; cannot run privileged commands or change another account. |
| `guest` | Read-only: shell write commands are refused **and** `vfs_write`/`mkdir`/`delete`/`rename` are refused at the VFS layer. |

`security_command_allowed(line, …)` is a single table consulted before the
legacy command dispatch in both shells; no individual command handler needed
modification. It also refuses any direct reference to `system/security` paths
for everyone (administrators use the account commands instead). The first
account is always an administrator, so an existing single-user workflow is
unchanged.

---

## 8. Commands

Available in Terminal and in the TTY consoles (help text updated in both):

```
whoami / id              signed-in account, uid, role and session epoch
security                 full subsystem status (hash, work factor, storage,
                         integrity, auto-lock, lockout, scope)
passwd                   change your password on the masked secure screen
users                    account table (admin)
useradd <name> [role]    create account (admin) — role admin|user|guest
userdel <name> --confirm delete account (admin, re-authenticates)
usermod <name> <opt>     --admin --user --guest --lock --unlock --reset (admin)
policy [show|set k v]    view/change policy (admin)
audit [clear]            bounded security audit ring (admin)
lock / logout            lock screen / sign out (closes all windows)
su <name>                switch identity with the target's password
reauth                   re-enter your password
recovery                 generate a new one-time recovery code (admin)
```

Commands that need a secret (`passwd`, `useradd`, `userdel`, `su`, `reauth`,
`recovery`, `lock`, `logout`) queue a secure flow; the WM tick (or the TTY, which
owns the screen itself) runs it synchronously. Passwords are **never** typed as
command arguments, so they never enter command history or scrollback.

---

## 9. Verification performed

* `make` — clean build with `-Wall -Wextra -Werror`; kernel ELF 898 936 bytes.
* `python3 tools/check_milestone.py` — frozen r43 32-bit image unchanged.
* `tools/tests/test_security.py` — 55 host KATs (SHA-256, HMAC, PBKDF2, policy,
  codec, checksum, zero-history reboot regression), source-level wiring checks,
  a booted guest (USB and IDE) checking authentication, protection, password
  change/history, guest read-only enforcement, and an ATA reboot that reloads
  the checksummed database and verifies the stored password.
* Real QMP keystrokes completed first-run setup on the actual framebuffer and
  reached `wm: entering main loop`; screenshots of the setup and recovery screens
  were captured and inspected (masked fields, policy hint, no plaintext).
* Existing regression suites re-run with the updated QEMU harness:
  `test_textwrap`, `test_studio_edit`, `test_affordances`, `test_management`,
  `test_cat` (including its ATA save/reboot case) — all pass.
* Emulator success is **not** physical-PC acceptance. No claim is made about
  SMP, hardware vsync, or any new storage/GPU/network capability.

## 10. Guest-test harness note

Boot now stops at the sign-in gate before `wm: entering main loop`.
`tools/tests/qemu_support.py` waits for `security: sign-in required`, then uses
the existing GDB debug port (already used by every guest test) to call the
**production** `security_api_authenticate` / `security_create_admin` functions —
exactly what the secure screen calls — and resumes the CPU to the WM idle
boundary. `reboot()` waits for the authentication marker and signs in again, so
tests with a persistent ATA target exercise the real login path rather than
first-run setup. No kernel-side test hook or compile-time bypass was added.

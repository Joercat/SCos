# SCos full security: sign-in, signed boot and encrypted storage

SCos is still SCos: the same desktop, apps, windows, themes, filesystem commands,
Lua CAT packages, drivers and native x64 UEFI build targets. Nothing here is a
Linux port, and the frozen 32-bit milestone image is untouched.

This document covers the whole security design after the **full-security phase**:
accounts and access control (phase 1), an RSA-2048 signed boot chain, an
AES-256 encrypted filesystem vault, hardened defaults, and the tests that prove
the parts that can be proven in QEMU. Read §2 before treating any of it as a
boundary against a determined owner of the machine.

---

## 1. What existed before, and what changed

Before this work `whoami` returned the literal string `"user"`. There was no
identity, no password, no lock screen, no boot verification and the saved
filesystem image was plaintext. `os.html` (the design reference) is untouched.

| File | Role |
| --- | --- |
| `kernel/include/security.h` | Public API, limits, record types, vault/AEAD/RSA-adjacent declarations. Freestanding; used by host tests and by the UEFI stub. |
| `kernel/desktop/sec_core.c` | SHA-256, HMAC-SHA256, PBKDF2-HMAC-SHA256 (N blocks), constant-time compare, hex, password/username policy, record codec, database checksum. **No kernel dependencies** — also compiled for the host and for the UEFI stub. |
| `kernel/desktop/sec_aes.c` | AES-256 (FIPS 197), CTR mode (SP 800-38A), encrypt-then-MAC AEAD built from HMAC-SHA256, volume-key wrap/unwrap. |
| `kernel/desktop/sec_vault.c` | On-disk vault header, key slots, VEK lifetime, seal/open. |
| `kernel/desktop/security.c` | Account database, sessions, lockout, audit ring, role checks, shell commands, entropy pool. All state in BSS. |
| `kernel/desktop/login.c` | Full-screen sign-in / setup / lock / masked-prompt UI. |
| `boot/uefi/rsa_verify.c` | Small fixed-size RSA-2048 PKCS#1 v1.5 / SHA-256 verifier, linked into `BOOTX64.EFI`. |
| `boot/uefi/trust_anchor.h` | Generated public modulus — the root of trust inside the loader. |
| `tools/scos_sigformat.py`, `tools/scos_sign.py`, `tools/scos_keys.py` | Manifest wire format, build-time signer, key generation/rotation. |
| `tools/tests/test_security.py`, `tools/tests/test_signed_boot.py`, `tools/tests/security_host.c`, `tools/tests/rsa_host.c` | 86 host known-answer checks, guest encryption/tamper tests, RSA verifier KATs and booted tamper tests. |

Integration points: `platform.c` (init, gate, post-unlock reload), `wm.c`
(safe tick + reload saved state), `vfs.c` (protected-path guard), `ata.c`
(encrypted image load/save), `app_terminal.c`, `tty.c`, `theme.c`, `Makefile`,
`makedisk.py`, `check_delivery.py`, `tools/tests/qemu_support.py`.

---

## 2. Threat model — what this actually defends

**It defends:**

* Someone walking up to a booting or running SCos machine and using the desktop,
  Terminal, TTY console or filesystem without an account.
* Online password guessing: per-account and global rate limiting, exponential
  lockout, constant-work verification, generic error text.
* Offline cracking of credential metadata: random per-user 128-bit salts, a
  calibrated PBKDF2 work factor, strong password policy, no plaintext verifier.
* **Offline reading of a removed/copied disk.** The saved filesystem tree is
  AES-256-CTR ciphertext; the volume key is only ever wrapped under a
  password-derived KEK and never stored in cleartext.
* **Offline modification of boot files.** The UEFI stub refuses to load a kernel
  until an RSA-2048 signature over a manifest of every executable/security file
  verifies, and every listed file's SHA-256 matches its bytes on the ESP.
* **Offline modification of vault metadata.** The entire cleartext vault header
  (account DB, audit log, key slots, generation, IV) is the AEAD's **associated
  data**. A changed byte anywhere in it invalidates the tag at next unlock.
* Casual tampering with credentials from inside the OS: `system/security/*`
  cannot be read, written, renamed or deleted by apps or shells. A checksum plus
  a last-known-good backup detects corruption.
* Credential leakage through the UI, terminal echo, scrollback, history, klog or
  the audit log. Passwords are never drawn, logged or stored; secret buffers are
  scrubbed.
* An unattended session (auto-lock), and a forgotten password (one-time recovery
  code that also unwraps the volume key, so recovery does **not** destroy data).
* A stopped desktop: the rescue TTY now requires re-authentication (phase 1's
  exception was removed).
* Privilege escalation through a newly added command: guest authorization is an
  **allow-list**, not a deny-list.

**It does NOT defend** (and no wording here claims otherwise):

* **No process isolation.** SCos is still a cooperative, single-address-space,
  ring-0 kernel. A native C app, a loaded GPU module or a Lua app is not a
  sandboxed process and can read kernel memory — including the unlocked volume
  key — directly. This is authentication, access control, boot integrity and
  encryption at rest; it is not a sandbox.
* **Firmware Secure Boot is not implemented.** The trust root is a public key
  embedded in `BOOTX64.EFI`. Without enrolling that EFI application in firmware
  Secure Boot, an attacker who can rewrite the ESP can replace the loader (and
  therefore the public key) with their own. Signed boot protects the *payload
  chain*; firmware enrolment is what protects the root. Enrolling is a machine
  owner step, not something this repository can do.
* **The development signing key is published.** `keys/boot-signing.json` is in
  the repository so that anyone can build and boot the image. Its private half
  is therefore public: the shipped image's signature proves **integrity**, not
  publisher identity. Rotate the key (§4) and keep it private for a meaningful
  release; then enrol the resulting loader in Secure Boot.
* **No rollback protection and no TPM/sealed counter.** An attacker with write
  access to the disk can restore an older, still-valid signed image and an older,
  still-valid encrypted vault. The generation counter is authenticated but has
  no secure monotonic store to compare against.
* **No per-user file isolation inside the vault.** One volume key encrypts the
  whole tree; every account that can unwrap it can read all saved files. Roles
  restrict the *shell/VFS interface*, not the ciphertext.
* **Evil-maid / whole-disk replacement.** Someone who can write the disk can
  install an entirely different OS, or a different SCos with a different trust
  anchor. Signed boot helps only when the loader itself is authenticated.
* **Physical access wins.** A hardware debugger, JTAG/ICE, or the QEMU GDB stub
  used by the test suite can call any kernel function. The harness deliberately
  calls the *production* setup/authentication entry points; there is no
  `#ifdef` backdoor in the kernel.
* **RAM-only machines.** Without the unique verified ATA SCos-data partition,
  accounts are **session-only** and there is no vault: every boot shows
  first-run setup again. The secure screen says so explicitly.
* **Legacy multi-user migration.** A pre-encryption plaintext `SCOSFS2` image is
  loaded once and re-saved encrypted at first sign-in. Only the account that
  signs in gets a key slot immediately; other accounts must have their password
  set again by an administrator (which creates their slot). New installations
  and single-admin images are unaffected.
* **CRC32 in `KERNEL.CRC` and `DRVLIST.IDX` is not a security check.** It catches
  accidental corruption; the RSA-signed SHA-256 manifest is what provides
  authenticity.

---

## 3. Cryptography

Everything is implemented in the repository in portable freestanding C11 (no
libc, no OpenSSL in the kernel or the loader) and exercised by host
known-answer tests:

* **SHA-256** (FIPS 180-4), streaming. FIPS vectors: empty string, `"abc"`,
  56-byte two-block message, one million `'a'`s.
* **HMAC-SHA256** (RFC 2104) with precomputed ipad/opad states. RFC 4231 cases
  1 and 2 plus the >64-byte key path.
* **PBKDF2-HMAC-SHA256** (RFC 8018), 1..N output blocks. RFC 6070-family
  vectors at 1, 2 and 4096 iterations, long password/salt, and two-block
  vectors checked against Python `hashlib.pbkdf2_hmac(dklen=64)`.
* **AES-256** (FIPS 197) — FIPS 197 Appendix C.3 known-answer block; AES-256-CTR
  (SP 800-38A) — F.5.5 four-block vector, partial blocks, in-place operation.
* **AEAD** = AES-256-CTR (encrypt) + HMAC-SHA256 (MAC over
  `version || IV || len(AD) || len(CT) || AD || CT`), with independent
  encryption and MAC subkeys derived from the master key by labelled HMACs.
  This is **encrypt-then-MAC**: decryption is refused before the tag verifies,
  and the plaintext buffer is zeroed on failure. The IV is authenticated too —
  changing it cannot make the same tag verify against a different plaintext.
  Host tests cover round trips, tampering of CT/tag/AD/key/IV, truncation,
  zero-length messages and AD-length domain separation.
* **VEK wrap**: 32-byte volume key wrapped as 32 bytes of CTR ciphertext plus a
  16-byte HMAC tag, using a key derived from PBKDF2 output block 2. Wrong key,
  tampered ciphertext, tampered tag and wrong salt are all rejected; the output
  is zeroed on failure.
* **RSA-2048 / PKCS#1 v1.5 / SHA-256** (RFC 8017), verification only, in the
  UEFI stub: 32 64-bit limbs, schoolbook multiply, bitwise modular reduction,
  `e = 65537`. The encoded message is compared against the *exact* 225-byte
  EMSA prefix (`00 01 || ff×202 || 00 || DigestInfo(SHA-256)`) followed by the
  digest, so sloppy parsers and Bleichenbacher-style trailing-garbage forgeries
  are refused. The signer is pure Python (`pow()`), verified on the host against
  the very same C object that is linked into `BOOTX64.EFI`.

**Key separation.** A password produces two independent 32-byte values: block 1
is the login verifier, block 2 is the key-encryption key that wraps the VEK.
The AEAD derives its own encryption and MAC subkeys from the VEK. No key is
reused across purposes.

**Entropy.** 128-bit salts, 256-bit volume keys, 128-bit per-save IVs and the
recovery code come from a SHA-256 pool seeded at boot from RDRAND (only when
CPUID reports it), the TSC, PIT ticks, RTC, CPUID values and input-event
counters, ratcheted after every output block. Without RDRAND the kernel logs a
reduced-entropy warning rather than pretending.

---

## 4. Signed boot chain

### Format

`\SCOS\BOOT.SIG` on the ESP is a fixed-format container:

```
0    8    "SCOSSIG1"
8    4    version = 1 (LE)
12   4    manifest length (LE)
16   256  RSA-2048 signature over SHA-256(manifest)
272  ...  manifest
```

The manifest is `"SCOSMAN1"` + version + entry count + a domain string, followed
by 48-byte entries: `name[12]` (uppercase 8.3 with dot), `size`, `sha256`.
It lists `KERNEL.ELF`, `KERNEL.CRC`, `DRVLIST.IDX`, `BUILD.TXT` (the provenance
record) and every `.MOD` GPU driver. The mandatory four names are required by
the parser, so deleting a kernel entry does not help an attacker.

### Verification

Before parsing the ELF or staging any module, `BOOTX64.EFI`:

1. reads `BOOT.SIG`, validates the container lengths and version;
2. calls `boot_rsa_verify()` with the embedded public modulus;
3. validates the manifest (magic, version, count, domain, name syntax, exactly
   one dot, mandatory names);
4. re-reads **every** listed file and compares its length and SHA-256;
5. only then proceeds with the existing ELF/GOP checks.

A GPU module selected by PCI detection is additionally checked with
`manifest_has()` — a module on the disk that is not in the verified manifest is
never staged. Failure returns to firmware with
`HALTED: boot chain verification failed`; no kernel byte is executed.

### Key rotation (do this for a real deployment)

```
python3 tools/scos_keys.py --generate     # rewrites boot/uefi/trust_anchor.h + keys/boot-signing.json
python3 tools/scos_keys.py --verify       # header and private key agree
make -j4 && make gpu-modules && make dist
```

Keep `keys/boot-signing.json` out of any public repository, sign on a trusted
machine (`SCOS_SIGN_KEY=/secure/path/key.json make`), and enrol the resulting
`BOOTX64.EFI` in firmware Secure Boot. Rotating the key invalidates every
previously signed image. `tools/check_delivery.py` treats `keys/` and the
signing tools as image inputs, so a stale shipped image is detected.

`tools/tests/test_signed_boot.py` proves: the EFI verifier accepts genuine
signatures and rejects changed manifest/signature/key/message; the packed
manifest covers all seven boot files and every hash matches the packed bytes;
and real QEMU boots of images with one flipped `KERNEL.ELF` or `BOOT.SIG` byte
are refused before `security: sign-in required`.

---

## 5. Encrypted vault (data at rest)

The verified ATA SCos-data partition holds one 128 KiB image, written only in
the `SCOSVAUL` format:

```
byte 0        8   "SCOSVAUL"  (byte 8..11 is the format version LE)
byte 12       4   account database length
byte 16       4   slot count (18)
byte 20       4   generation counter (increments on every save)
byte 24      16   random vault id
byte 40      16   fresh random AEAD IV for this save
byte 56       4   payload length (98 304)
byte 60       4   flags (SEALED)
byte 64       4   audit log length
byte 96   2 880   18 × 160-byte key slots
byte 2 976 14 368 account database (cleartext, checksummed)
byte 17 344  7 792 audit log (cleartext)
byte 32 736     32 HMAC-SHA256 tag
byte 32 768 98 304 AES-256-CTR ciphertext of an SCOSFS2 tree
```

Everything from byte 0 to 32 735 is the AEAD's **associated data**. The header
must be cleartext because it is read before anyone has typed a password, but it
contains no plaintext secrets: only salts, PBKDF2 verifiers, the audit log and a
volume key wrapped under password-derived KEKs. Any edit — to a slot, the
account DB, the audit log, the generation, the IV or the vault id — breaks the
tag and is reported at the next unlock.

Each 160-byte slot holds: username (or `@recovery`), a fresh 16-byte KDF salt,
iteration count, flags, a 32-byte PBKDF2 verifier, the 48-byte wrapped VEK and
lockout counters. Up to 16 accounts plus the recovery slot are supported.

### Lifecycle

1. **Boot with a vault on disk.** `fs_image_load()` stages the header, publishes
   only the account DB and audit log into the RAM filesystem, and leaves the
   tree encrypted. The boot screen says
   `encrypted vault locked; sign in to decrypt the saved tree`.
2. **Sign in.** After the password verifier matches, `vault_unlock_password()`
   derives two PBKDF2 blocks from that password, checks the slot verifier and
   unwraps the VEK. `fs_image_unlock_apply()` verifies the AEAD tag, decrypts
   in place, applies the FS2 tree and calls `security_fs_restore()` to
   re-materialize the protected directory. Settings, custom themes, Lua apps,
   desktop icons and pins are reloaded after the gate, because the tree arrives
   later than `wm_init()`.
3. **Save.** `fs_image_save()` serializes the tree (excluding
   `system/security/*`, whose authoritative copy is in the authenticated
   header), builds a fresh header with a new IV and generation, seals it and
   writes it with the existing invalidate → write tail → commit first sector →
   readback-verify protocol. If the VEK is not held (before sign-in, or after
   logout) it **fails closed** and never writes plaintext.
4. **Password change** re-wraps the VEK under the new password's KEK *before*
   the account record is committed. User creation adds a slot; deletion removes
   it. A recovery code has its own slot, so redeeming it recovers both the
   administrator password **and** the volume key; the slot is then erased.
5. **Logout** burns the VEK. A screen *lock* keeps it (so background saves still
   work); a real logout/switch user re-derives it from the next password.
6. **Legacy plaintext migration.** A `SCOSFS2` image is applied once, then
   re-saved encrypted at the first sign-in. No new plaintext is ever written.

### Honest notes on the audit log

The audit ring (48 entries, RTC epoch, event, uid, user, detail) is stored in the
cleartext header, which is AEAD-authenticated as associated data. That is the
tamper evidence: an offline edit breaks the tag at unlock. A hash chain with no
secret would be security theatre — anyone who can edit the log can recompute it —
so there is no chain pretending to be a MAC. The audit log is **not secret**;
it is authentic (modulo §2's rollback limitation).

---

## 6. Usernames, passwords and hardened defaults

**Usernames:** 3–31 ASCII characters, start and end alphanumeric, interior
`[a-z0-9._-]`, no `..`, at least one letter, compared case-insensitively after
normalization. Reserved: `root`, `nobody`, `system`, `security`, `kernel`,
`daemon`, `guest`, `default`, `scos`, `operator`, `superuser`, `sys`, `bin`.
UIDs start at 1000.

**Default password policy** (all administrator-tunable; existing databases keep
their recorded values, so tightening these never locks anyone out):

| Key | Default | Meaning |
| --- | --- | --- |
| `minlen` | 12 | Minimum length (8–64) |
| `classes` | 3 | Minimum of {lower, upper, digit, symbol} |
| `minbits` | **50** | Estimated entropy floor (was 40) |
| `maxfails` | 5 | Failures before lockout |
| `lockbase` / `lockmax` | 30 s / 900 s | Exponential lockout window |
| `idle` | **10 min** | Auto-lock (was 15) |
| `maxage` | **90 days** | Force a change after N days (was off) |
| `history` | 5 | Remembered passwords |
| `iters` | calibrated | PBKDF2 iterations for new accounts |

Rejections beyond length/classes: printable ASCII only; no username substring;
a ~40-word common-password blacklist; four or more identical characters in a
row; four-key keyboard walks; short periodic patterns; entropy below the floor.
**Work factor** is calibrated once at first-run setup against the UEFI-measured
`cpu_tsc_hz`, targeting ~250 ms, clamped to `[20 000, 1 000 000]`. Each account
stores its own iteration count. Password history keeps up to five previous
`salt + verifier` pairs.

Guests are now restricted by an explicit **read-only allow-list** of commands
(`ls`, `cat`, `whoami`, `security`, `audit`, …). Any command added in a future
update is denied to guests by default.

---

## 7. Account database, boot, sessions and commands

`/system/security/users.db` remains a line-oriented, strictly parsed text file:

```
SCos-accounts-v1
P|minlen|classes|minbits|maxfails|lockbase|lockmax|idle|maxage|history|iters
R|iters|recovery_salt_hex|recovery_hash_hex|valid
U|uid|name|display|role|flags|created|last_login|pw_changed|fails|lock_until|iters|salt_hex|hash_hex|hist_count|hist_blob_hex
C|sha256_hex_of_all_preceding_bytes
```

* Field counts, numeric ranges, hex lengths, duplicate names/UIDs, role/flag
  bits and at least one enabled administrator are validated into a scratch
  array before state is committed.
* `C|` is a SHA-256 checksum; a bad one falls back to `users.db.bak`. It is a
  corruption check. **Authenticity comes from the vault's AEAD tag**, which
  covers the header copy of this file.
* `vfs.c` refuses reads/writes/mkdir/rename/delete for the normalized
  `system/security` subtree. `security_fs_restore()` re-materializes it after a
  factory reset and after the encrypted tree is applied.
* The full-screen gate runs before `wm_run()`. Setup shows a one-time recovery
  code; sign-in uses generic failure text; `F8` redeems the recovery code;
  `MUSTCHANGE`, `--reset` and password age force an immediate change.
* `Ctrl+Alt+F1`–`F6` and `tty` require a masked re-authentication — **including
  rescue mode where the WM has stopped**.
* `security_wm_tick()` runs queued secure flows once per WM iteration, after all
  window callbacks have returned.

Commands (Terminal and TTY):

```
whoami / id              signed-in account, uid, role and session epoch
security                 full status: hash, work factor, storage, vault,
                         signed boot chain, auto-lock, lockout, scope
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

Passwords are never command arguments, so they never enter history or
scrollback.

---

## 8. Verification performed

* `make -j4` — clean build with `-Wall -Wextra -Werror`; native x64 UEFI only.
* `python3 tools/check_milestone.py` — frozen r43 32-bit image unchanged.
* `python3 tools/scos_keys.py --verify` and `python3 tools/scos_sign.py --self-test`.
* `tools/tests/test_security.py` — 86 host KATs (SHA-256, HMAC, PBKDF2 one/two
  blocks, AES-256 FIPS 197, CTR SP 800-38A, AEAD, VEK wrap, policy, codec,
  checksum, zero-history reboot), source-level wiring checks, a booted guest
  (authentication, protection, password change/history, guest allow-list), an
  ATA reboot that reloads the vault, and a booted vault test that reads the
  saved image out of guest memory to prove there is **no plaintext** and that
  guest-side `vault_open()` rejects changes to the header, tag and ciphertext.
  It also proves `fs_image_save()` fails closed after `vault_lock()`.
* `tools/tests/test_signed_boot.py` — EFI-object RSA KATs, packed manifest
  completeness against the FAT32 ESP, and two real QEMU boots of tampered
  images that are refused before the sign-in gate.
* Existing suites re-run: `test_textwrap`, `test_studio_edit`,
  `test_affordances`, `test_management`, `test_cat` (including ATA save/reboot),
  `test_compositor`, `test_memory`, `test_buildinfo`, `test_verify_stick`,
  `test_gpu_render`, `test_gpu_detect`, `test_nvidia_ident`, `test_pci_scan`,
  `test_delivery`. `test_gpu_detect`'s module-tamper case now re-signs the
  deliberately modified image with the development key so the kernel's own
  module CRC remains the last line of defence, and separately proves the
  unsigned version is refused in UEFI.
* `make dist` refreshes `dist/scos.img`, `dist/scos.img.build.json` and
  `dist/scos.img.sha256` together; `python3 tools/check_delivery.py` passes.
* **Emulator success is not physical-PC acceptance.** No claim is made about
  SMP, hardware vsync, or any new GPU/NIC/browser capability.

## 9. Guest-test harness note

Boot stops at the sign-in gate before `wm: entering main loop`.
`tools/tests/qemu_support.py` waits for `security: sign-in required`, then uses
the existing GDB debug port to call the **production**
`security_api_authenticate` / `security_create_admin` functions — exactly what
the secure screen calls — and resumes the CPU. `reboot()` waits for the marker
and signs in again, so persistent-ATA tests exercise the real unlock path. No
kernel-side test hook or compile-time bypass exists. Tamper tests manipulate
copies of the image and use disposable QEMU snapshots; host physical disks are
never attached.

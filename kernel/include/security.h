/* SCos security: accounts, access control, AES/AEAD, encrypted vault.
 *
 * This header is deliberately freestanding: sec_core.c (SHA-256, HMAC,
 * PBKDF2, policy, record codec) and sec_aes.c include only this file and can
 * be compiled on the host, in the kernel and in the UEFI stub.  Everything
 * below the "kernel services" divider is implemented by security.c / login.c /
 * sec_vault.c and needs scos.h at the call site.
 *
 * Full security adds an AES-256-CTR / HMAC-SHA256 encrypted filesystem vault
 * whose volume key is wrapped per account, plus a user-controlled policy layer
 * (security choices).  SCos remains a cooperative, single-address-space
 * kernel: process isolation is a per-app USER CHOICE, never forced.  There is
 * no TPM/rollback protection.  This is not a sandbox and cannot defend against
 * a hardware debugger or an attacker who can replace the boot medium.  See
 * docs/migration/SECURITY.md before treating any of it as a stronger boundary.
 */
#ifndef SCOS_SECURITY_H
#define SCOS_SECURITY_H

#include <stdint.h>
#include <stddef.h>

/* ---------------------------------------------------------------- limits --- */
#define SC_NAME_MAX       32     /* username, including NUL */
#define SC_DISPLAY_MAX    48
#define SC_PASS_MAX      128     /* password, excluding NUL */
#define SC_SALT_BYTES     16
#define SC_HASH_BYTES     32
#define SC_HISTORY_MAX     5
#define SC_USERS_MAX      16
#define SC_RECORD_MAX    768     /* one serialized U| line including '\n' */
#define SC_DB_MAX       (SC_USERS_MAX * (SC_RECORD_MAX + 2) + 2048)
#define SC_MSG_MAX       192
#define SC_AUDIT_MAX      48
#define SC_AUDIT_LINE    160
#define SC_AUDIT_BYTES  (SC_AUDIT_MAX * (SC_AUDIT_LINE + 1) + 64)

#define SC_ROLE_ADMIN 0
#define SC_ROLE_USER  1
#define SC_ROLE_GUEST 2

#define SC_FLAG_DISABLED    0x01u
#define SC_FLAG_MUSTCHANGE  0x02u

/* ---------------------------------------------------------- small strings --- */
size_t sc_len(const char *s);
int    sc_eq(const char *a, const char *b);
int    sc_neq(const char *a, const char *b, size_t n);
char  *sc_copy(char *dst, size_t cap, const char *src);
char  *sc_cat(char *dst, size_t cap, const char *src);
char  *sc_cpy(char *dst, const char *src);           /* unbounded; fixed arrays only */
void   sc_fmt(char *dst, size_t cap, uint64_t v);
int    sc_num(const char *s, uint64_t *out);          /* strict decimal, no sign */
int    sc_find(const char *hay, const char *needle);  /* 1 when present */
void   sc_lower(char *dst, size_t cap, const char *src);

/* --------------------------------------------------------------- SHA-256 --- */
/* FIPS 180-4, one block at a time.  `total' counts message bytes. */
typedef struct {
    uint32_t h[8];
    uint8_t  block[64];
    size_t   used;
    uint64_t total;
} sc_sha256;

void sc_sha256_init(sc_sha256 *c);
void sc_sha256_update(sc_sha256 *c, const void *data, size_t len);
void sc_sha256_final(sc_sha256 *c, uint8_t out[SC_HASH_BYTES]);
void sc_sha256_hash(const void *data, size_t len, uint8_t out[SC_HASH_BYTES]);

/* HMAC-SHA256.  init() precomputes the ipad/opad states so PBKDF2 can reuse
 * them; final() works on a copy and leaves the prepared state untouched. */
typedef struct { sc_sha256 inner, outer; } sc_hmac;
void sc_hmac_init(sc_hmac *c, const void *key, size_t keylen);
void sc_hmac_update(sc_hmac *c, const void *data, size_t len);
void sc_hmac_final(const sc_hmac *c, uint8_t out[SC_HASH_BYTES]);
void sc_hmac_hash(const void *key, size_t keylen, const void *msg, size_t mlen,
                  uint8_t out[SC_HASH_BYTES]);

/* PBKDF2-HMAC-SHA256, one output block (32 bytes).  Iterations are stored per
 * account, so raising the default never invalidates existing credentials. */
void sc_pbkdf2(const void *pw, size_t pwlen,
               const uint8_t *salt, size_t saltlen,
               uint32_t iterations, uint8_t out[SC_HASH_BYTES]);
/* General PBKDF2 with a caller-chosen number of 32-byte output blocks.
 * Block 1 is byte-for-byte what sc_pbkdf2 returns; block 2 is the key-
 * encryption key used to wrap the disk volume key, so a password produces a
 * verifier and a wrapping key without ever storing either. */
void sc_pbkdf2_blocks(const void *pw, size_t pwlen,
                      const uint8_t *salt, size_t saltlen,
                      uint32_t iterations, uint32_t blocks, uint8_t *out);

/* ----------------------------------------------------------------- AES ---- */
void sc_aes256_encrypt_block(const uint8_t key[32], const uint8_t in[16], uint8_t out[16]);
void sc_aes256_ctr(const uint8_t key[32], const uint8_t iv[16],
                   uint8_t *out, const uint8_t *in, size_t len);
/* Encrypt-then-MAC.  Encryption and MAC subkeys are derived from `key' with
 * distinct labels.  `tag' is a full 32-byte HMAC-SHA256. */
void sc_aead_seal(const uint8_t key[32], const uint8_t iv[16],
                  const uint8_t *ad, size_t adlen,
                  const uint8_t *pt, size_t ptlen,
                  uint8_t *ct, uint8_t tag[32]);
int  sc_aead_open(const uint8_t key[32], const uint8_t iv[16],
                  const uint8_t *ad, size_t adlen,
                  const uint8_t *ct, size_t ctlen,
                  uint8_t *pt, const uint8_t tag[32]);
/* 48-byte wrapped volume key = 32-byte CTR ciphertext + 16-byte HMAC tag. */
void sc_vek_wrap(const uint8_t kek[32], const uint8_t nonce[16],
                 const uint8_t vek[32], uint8_t wrapped[48]);
int  sc_vek_unwrap(const uint8_t kek[32], const uint8_t nonce[16],
                   const uint8_t wrapped[48], uint8_t vek[32]);

/* Constant-work comparison and best-effort scrubbing of secret buffers. */
int  sc_ct_eq(const void *a, const void *b, size_t n);
void sc_burn(void *p, size_t n);

void sc_hex(char *out, size_t cap, const uint8_t *in, size_t n);
int  sc_unhex(uint8_t *out, size_t cap, const char *in, size_t hexchars);

/* Integrity line for the account database.  The digest covers every byte
 * before the final "C|" line.  This detects corruption and casual edits; it
 * is NOT a MAC (there is no secret on an unsigned boot medium). */
void sc_db_digest(const char *body, size_t len, char out[65]);
int  sc_db_verify(const char *data, size_t len);

/* ------------------------------------------------------- encrypted vault --- */
/* The ATA data partition holds one 128 KiB image: a 64-sector authenticated
 * metadata header followed by an AES-256-CTR/HMAC-SHA256 encrypted FS2 tree.
 * The header is the AEAD's associated data, so *any* edit to a key slot, the
 * account database or the audit log invalidates the tag and is detected when
 * the vault is unlocked.  The header is deliberately cleartext: it must be
 * readable before anybody has typed a password.  It contains no plaintext
 * secrets - only salts, PBKDF2 verifiers and password-wrapped volume keys.
 *
 * One random 256-bit volume encryption key (VEK) encrypts the whole image.
 * Each account (and the one-time recovery code) has a slot holding that VEK
 * wrapped under the second PBKDF2 block of its password.  Block 1 remains the
 * login verifier, so the same password authenticates and unwraps the disk
 * without ever being stored.  Changing a password re-wraps the VEK.
 */
#define SC_VEK_BYTES        32
#define SC_VEK_WRAP_BYTES   48
#define SC_VAULT_SLOTS      18      /* 16 accounts + recovery + spare */
#define SC_VAULT_SLOT_BYTES 160
#define SC_VAULT_SLOT_OFF   96
#define SC_VAULT_DB_OFF     (SC_VAULT_SLOT_OFF + SC_VAULT_SLOTS * SC_VAULT_SLOT_BYTES)
#define SC_VAULT_AUDIT_OFF  (SC_VAULT_DB_OFF + SC_DB_MAX)
#define SC_VAULT_CHOICES_OFF (SC_VAULT_AUDIT_OFF + SC_AUDIT_BYTES)
#define SC_VAULT_CHOICES_MAX 4096
#define SC_VAULT_TAG_OFF    32736
_Static_assert(SC_VAULT_CHOICES_OFF + SC_VAULT_CHOICES_MAX <= SC_VAULT_TAG_OFF,
               "vault metadata regions overlap the AEAD tag");
#define SC_CHOICES_PATH "system/security/choices.cfg"
#define SC_APP_CHOICES_MAX 24
#define SC_APP_ID_MAX 32

/* Per-app process isolation is a USER CHOICE, never forced.  0 = ask every
 * time (or use isolation.default), 1 = always isolated, 2 = never isolated. */
#define SC_ISO_ASK    0
#define SC_ISO_ALWAYS 1
#define SC_ISO_NEVER  2
#define SC_VAULT_HDR_BYTES  32768   /* 64 sectors */
#define SC_VAULT_IMAGE_BYTES 131072 /* 256 sectors; must match ata.c */
#define SC_VAULT_PAYLOAD    (SC_VAULT_IMAGE_BYTES - SC_VAULT_HDR_BYTES)
#define SC_VAULT_MAGIC      "SCOSVAUL"  /* 8 bytes; byte 8..11 is the format version */
#define SC_VAULT_VERSION    1u
#define SC_VAULT_SLOT_USED      1u
#define SC_VAULT_SLOT_RECOVERY  2u
#define SC_VAULT_FLAG_SEALED    1u

void security_random_bytes(void *out, size_t n);   /* audited entropy pool */
int  security_vault_status(char *out, size_t cap);

/* --------------------------------------------------- user security choices --- */
/* Everything that could restrict the owner of the machine is a choice.  The
 * defaults keep important protections ON, but every one of them can be turned
 * off from `choices` in either shell, or through the API below from an app the
 * user writes.  Values are stored in the AEAD-authenticated vault header, so
 * they survive reboot and cannot be edited offline without breaking the tag.
 * Choice keys and their defaults are in sec_choices.c. */
int  security_choice(const char *key);                  /* 0/1, -1 if unknown */
int  security_choice_set(const char *key, int value);  /* persists; 0 if unknown */
int  security_choice_count(void);
void security_choice_describe(int i, char *out, size_t cap); /* "key = on : help" */
int  security_choices_load(void);                      /* from VFS at boot */
int  security_choices_save(void);                      /* VFS + vault header */
void security_choices_default(void);
const char *security_choice_error(void);

/* Per-app isolation choice.  The WM asks on first launch; the answer is stored
 * and can be changed with `choices app <id> always|never|ask`.  The isolation
 * engine is deliberately NOT implemented: w->isolated is a flag apps/drivers
 * can honour (Lua CAT apps deny filesystem writes when set), and no app is
 * forced into isolation. */
int  security_app_isolation(const char *app_id);
int  security_app_isolation_set(const char *app_id, int mode);
int  security_app_isolation_pending(const char *app_id);
int  security_app_launch_isolated(const char *app_id, int isolated);
int  security_app_last_isolated(const char *app_id);
int  security_app_isolation_count(void);
void security_app_isolation_describe(int i, char *out, size_t cap);

/* Internal metadata-I/O helpers.  Only the account/vault/choices modules use
 * these; they are not a general-purpose way around the VFS guard. */
void security_bypass_scope(int on);
void security_mark_dirty(void);

int  vault_disk_present(void);
int  vault_unlocked(void);
void vault_lock(void);                 /* burn the VEK (logout / explicit lock) */
void vault_reset(void);                /* first-run setup discards an unreadable vault */
int  vault_header_valid(void);
int  vault_image_pending(void);
void vault_mark_applied(void);
uint32_t vault_db_len(void);
uint32_t vault_generation(void);   /* monotonic save counter in the AEAD header */
uint32_t vault_audit_len(void);
uint32_t vault_choices_len(void);
int  vault_stage(const uint8_t *image, uint32_t len);   /* pre-auth: parse + bound-check */
int  vault_open(uint8_t *image, uint32_t len);          /* post-auth: verify tag, decrypt in place */
int  vault_seal(uint8_t *image, uint32_t len);          /* build header + encrypt in place */
void vault_set_db(const char *data, uint32_t len);
void vault_set_audit(const char *data, uint32_t len);
void vault_set_choices(const char *data, uint32_t len);
int  vault_unlock_password(const char *name, const char *pw, uint32_t iters,
                           char *msg, size_t cap);
int  vault_unlock_recovery(const char *code, char *msg, size_t cap);
int  vault_rebind_password(const char *name, const char *pw, uint32_t iters);
int  vault_unbind(const char *name);
int  vault_bind_recovery(const char *code, uint32_t iters);
void vault_forget_recovery(void);
int  fs_image_unlock_apply(void);     /* ata.c: apply a staged encrypted image */

/* ----------------------------------------------------------------- policy --- */
struct sc_policy {
    uint32_t min_len;        /* 8..64           */
    uint32_t min_classes;    /* 1..4            */
    uint32_t min_bits;       /* estimated entropy floor */
    uint32_t max_fails;      /* failures before lockout  */
    uint32_t lock_base;      /* first lockout, seconds   */
    uint32_t lock_max;       /* lockout ceiling, seconds */
    uint32_t idle_minutes;   /* 0 disables auto-lock     */
    uint32_t max_age_days;   /* 0 disables age expiry    */
    uint32_t history;        /* remembered old passwords */
    uint32_t iters;          /* PBKDF2 work factor       */
};

void sc_policy_default(struct sc_policy *p);
int  sc_policy_valid(const struct sc_policy *p);
int  sc_policy_encode(const struct sc_policy *p, char *out, size_t cap);
int  sc_policy_decode(const char *line, struct sc_policy *p);

/* Fixed-size account record: no pointers, safe to keep in kernel BSS and safe
 * to serialize byte for byte.  Passwords never appear here. */
struct sc_account {
    uint32_t uid, role, flags, fails, iters;
    uint64_t created, last_login, pw_changed, lock_until;
    char     name[SC_NAME_MAX];
    char     display[SC_DISPLAY_MAX];
    uint8_t  salt[SC_SALT_BYTES];
    uint8_t  hash[SC_HASH_BYTES];
    uint8_t  hist_count;
    uint8_t  hist[SC_HISTORY_MAX][SC_SALT_BYTES + SC_HASH_BYTES];
    uint8_t  used;
};

int sc_account_encode(const struct sc_account *a, char *out, size_t cap);
int sc_account_decode(const char *line, struct sc_account *a);

/* Usernames: 3..31 ASCII [a-z0-9._-], start alphanumeric, no reserved names,
 * no "..", no trailing separator.  Comparison is always on the lowercase form. */
int  sc_username_valid(const char *name);
void sc_username_normal(const char *in, char out[SC_NAME_MAX]);

/* Password policy: printable ASCII only, length/classes/entropy floors,
 * username rejection, a small common-password blacklist, keyboard walks and
 * repeated patterns.  Returns 1 (strength text in msg) or 0 (reason in msg). */
int  sc_password_eval(const char *user, const char *pw,
                      const struct sc_policy *p, char *msg, size_t cap);
int  sc_password_bits(const char *pw);
void sc_policy_hint(const struct sc_policy *p, char *out, size_t cap);

/* -------------------------------------------------------- kernel services --- */
void security_init(int have_disk);
void security_login_gate(void);       /* boot-time gate, before the WM loop */
void security_wm_tick(void);          /* one safe point outside all callbacks */
void security_run_pending(void);      /* execute queued full-screen flows now */
void security_fs_restore(void);       /* re-materialize DB after a VFS reset */
int  security_initialized(void);
int  security_protected_path(const char *path);
int  security_bypass_active(void);
int  security_write_denied(void);

const char *security_current_user(void);
int  security_is_admin(void);
int  security_command_allowed(const char *line, char *out, size_t cap);
int  security_shell(const char *line, char *out, size_t cap);
int  security_reauthenticate(const char *reason);

/* `tlpdo`: the password gate for privileged commands.  Elevation is bound to
 * the signed-in account and ends after a period with no interaction at all -
 * it never expires while the owner is working, and never survives a user
 * switch or lock.  `sudo.password` (a user choice) can turn the prompt off. */
int  security_privileged_command(const char *line, char *what, size_t cap);
int  security_elevated(void);
void security_elevate_grant(void);
void security_elevate_drop(void);
int  security_elevation_seconds(void);
void security_mix_event(uint8_t code);

/* Production entry points used by the full-screen UI and by the QEMU test
 * harness (which drives them through the existing GDB debug port instead of
 * typing on a virtual keyboard).  No test-only bypass exists in the kernel. */
int  security_create_admin(const char *name, const char *pw, const char *confirm);
int  security_api_authenticate(const char *name, const char *pw, char *msg, size_t cap);
const char *security_api_last_error(void);

/* UI-facing state queries and operations. */
int  security_api_gate_satisfied(void);
int  security_api_force_change(void);
int  security_api_session(void);
const char *security_api_session_name(void);
int  security_api_user_count(void);
/* Name of account `index` (0-based), or 0 when there is no such account.  Used
 * by the per-app storage layer to erase one application's data for every
 * account, not only for whoever happens to be signed in. */
int  security_api_user_name(int index, char *out, size_t cap);
int  security_api_persistent(void);
/* True while the shipped default account still uses its initial password.
 * The console may show a hint; `passwd` clears it. */
int  security_api_default_credentials(void);
/* The shipped account has the HIGHEST role.  It is still an ordinary,
 * renameable/deletable account record - not a built-in uid-0 superuser.
 * `roles.enabled` (off by default) gives EVERY account top permission;
 * turning it on makes the admin/user/guest distinctions effective. */
#define SC_DEFAULT_USER "root"
#define SC_DEFAULT_PASS "scos"
#define SC_DEFAULT_ROLE SC_ROLE_ADMIN
int  security_api_denied(void);
uint32_t security_api_deny_remaining(void);
int  security_api_verify(int idx, const char *pw, char *msg, size_t cap);
int  security_api_password_ok(const char *name, const char *pw, char *msg, size_t cap);
int  security_api_change_password(int idx, const char *oldpw, const char *newpw,
                                  int require_old, char *msg, size_t cap);
int  security_api_create_user(const char *name, int role, const char *pw);
int  security_api_delete_user(const char *name);
int  security_api_set_role(const char *name, int role);
int  security_api_set_disabled(const char *name, int disabled);
int  security_api_reset_user(const char *name, char *msg, size_t cap);
int  security_api_recovery_active(void);
int  security_api_redeem_recovery(const char *code, const char *newpw, char *msg, size_t cap);
const char *security_api_recovery_code(void);
void security_api_ack_recovery(void);
void security_api_drop_session(void);
void security_api_lock_session(void);
void security_api_unlock_session(void);
int  security_api_locked(void);
void security_api_min_policy(struct sc_policy *p);
/* Any real interaction resets the idle timer.  Called by the input drivers
 * AND by the WM when it consumes an event, so no interaction path is missed. */
int  security_api_lock_now(void);      /* queue the lock screen from an app */
void security_note_activity(void);
uint32_t security_idle_ticks(void);
int  security_idle_would_lock(void);   /* no side effects; owner-visible */
uint32_t security_api_idle_minutes(void);
int  security_api_policy_set(const char *key, uint32_t value);
void security_api_build_hint(char *out, size_t cap);

/* Full-screen (framebuffer) flows implemented by login.c.  Every one of them
 * paints masked fields, never renders a password, and scrubs its buffers. */
void security_ui_setup(void);
void security_ui_login(void);
void security_ui_logout(void);
void security_ui_lock(void);
int  security_ui_secret(const char *title, const char *prompt, char *out, size_t cap);
void security_ui_info(const char *title, const char *text);
int  security_ui_reauth(const char *reason);
int  security_ui_new_password(const char *title, const char *user, char *out, size_t cap);
int  security_ui_change_password(int idx, int require_old);
int  security_ui_user_add(const char *name, int role);
int  security_ui_user_del(const char *name);
int  security_ui_switch_user(const char *name);
int  security_ui_recovery(void);

/* App-facing lock-screen API.  No lock-screen app is shipped; by default the
 * plain secure console is used.  Register a renderer here to replace it.  The
 * callback owns the framebuffer while the session is locked and must never
 * draw a typed password.  Return 1 when unlocked, -3 to switch user, 0 to
 * keep waiting.  See docs/migration/SECURITY.md for the full contract. */
typedef int (*security_lock_screen_fn)(const char *user);
void security_set_lock_screen(security_lock_screen_fn fn);

#endif /* SCOS_SECURITY_H */

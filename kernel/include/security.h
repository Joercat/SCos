/* SCos account, authentication and access-control subsystem.
 *
 * This header is deliberately freestanding: sec_core.c (SHA-256, HMAC,
 * PBKDF2, policy, record codec) includes only this file and can be compiled
 * and tested on the host.  Everything below the "kernel services" divider is
 * implemented by security.c / login.c and needs scos.h at the call site.
 *
 * Threat model and limitations are documented in docs/migration/SECURITY.md.
 * In particular: this is a cooperative, single-address-space kernel with no
 * ring-3 isolation, no signed boot and no encrypted disk.  This subsystem
 * protects the interactive console against unauthenticated use and against
 * casual inspection of credentials; it is not a sandbox and cannot defend
 * against someone who can replace the boot image or attach a hardware
 * debugger.  Nothing in this file pretends otherwise.
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
int  security_api_persistent(void);
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

#endif /* SCOS_SECURITY_H */

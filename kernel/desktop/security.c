/* SCos account database, sessions, credential verification, audit trail,
 * command authorization and the shell interface.  See docs/migration/SECURITY.md
 * for the threat model.  Layout of the work:
 *
 *   - All state lives in BSS.  No account data is heap allocated, so a failed
 *     allocation can never turn into an unauthenticated session.
 *   - The database is a checksummed text file in the RAM filesystem under
 *     system/security/.  Only this module writes it (vfs.c enforces that).
 *     With a verified ATA save target it is persisted with the normal `save'
 *     image; without one, accounts are session-only and the UI says so.
 *   - Passwords are never stored, logged, echoed or drawn.  Each account has a
 *     random 128-bit salt and a PBKDF2-HMAC-SHA256 verifier with a calibrated
 *     work factor; comparison is constant time and unknown users cost the same
 *     PBKDF2 work as known ones.
 */
#include "scos.h"
#include "security.h"

#define DB_MAGIC   "SCos-accounts-v1"
#define DB_PATH    "system/security/users.db"
#define DB_BAK     "system/security/users.db.bak"
#define AUDIT_PATH "system/security/audit.log"
#define RECOVERY_ALPHABET "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
#define RECOVERY_CHARS 20

enum {
    PEND_NONE = 0, PEND_LOCK, PEND_LOGOUT, PEND_PASSWD, PEND_USERADD,
    PEND_USERDEL, PEND_SU, PEND_REAUTH, PEND_RECOVERY
};

struct sec_state {
    int ready, have_disk, corrupt, db_loaded, bypass;
    int default_pw;               /* shipped root/scos credentials still active */
    int meta_io;                  /* scoped protected-metadata I/O (choices module) */
    int disk_dirty, last_save_try;
    struct sc_policy policy;
    struct sc_account users[SC_USERS_MAX];
    struct sc_account scratch[SC_USERS_MAX];
    int count, session, locked, force_change;
    uint64_t deny_until;
    uint32_t global_fails;
    uint32_t session_epoch;

    uint8_t  rng_pool[SC_HASH_BYTES];
    uint64_t rng_counter;
    int      rng_ready, rdrand_ok;

    uint8_t  rec_salt[SC_SALT_BYTES];
    uint8_t  rec_hash[SC_HASH_BYTES];
    uint32_t rec_iters;
    int      rec_valid;
    char     rec_code[RECOVERY_CHARS + 1];

    int pending, pending_idx, pending_role;
    char pending_arg[SC_NAME_MAX];
    char last_error[SC_MSG_MAX];

    char audit[SC_AUDIT_MAX][SC_AUDIT_LINE];
    int audit_head, audit_count;
};

static struct sec_state S;
static char db_buf[SC_DB_MAX];
static char db_work[SC_DB_MAX];
static char audit_buf[SC_AUDIT_BYTES];
static char line_buf[SC_RECORD_MAX];
static char hex_salt[SC_SALT_BYTES*2+1];
static char hex_hash[SC_HASH_BYTES*2+1];

/* --------------------------------------------------------------- utilities --- */
static void set_error(const char *s) { sc_copy(S.last_error, sizeof(S.last_error), s); }

const char *security_api_last_error(void) { return S.last_error; }

static uint64_t now_epoch(void)
{
    struct rtc_time t;
    rtc_read(&t);
    return (uint64_t)rtc_to_epoch(&t);
}

static int add(char *buf, size_t cap, size_t *at, const char *s)
{
    size_t n = sc_len(s);
    if (*at + n + 1 > cap) return 0;
    memcpy(buf + *at, s, n);
    *at += n;
    buf[*at] = 0;
    return 1;
}

static void cpuid_leaf(uint32_t leaf, uint32_t *a, uint32_t *b,
                       uint32_t *c, uint32_t *d)
{
    uint32_t ea, eb, ec, ed;
    __asm__ volatile("cpuid" : "=a"(ea), "=b"(eb), "=c"(ec), "=d"(ed)
                     : "a"(leaf), "c"(0) : "memory");
    *a = ea; *b = eb; *c = ec; *d = ed;
}

static int rdrand_word(uint64_t *out)
{
    uint64_t v;
    uint8_t ok;
    if (!S.rdrand_ok) return 0;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok) :: "cc");
    if (!ok) return 0;
    *out = v;
    return 1;
}

static void put_le64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8));
}

static void rng_mix(const void *data, size_t n)
{
    sc_sha256 c;
    uint8_t counter[8];
    if (!S.rng_ready) { S.rng_counter = 1; S.rng_ready = 1; }
    put_le64(counter, S.rng_counter++);
    sc_sha256_init(&c);
    sc_sha256_update(&c, S.rng_pool, sizeof(S.rng_pool));
    sc_sha256_update(&c, data, n);
    sc_sha256_update(&c, counter, sizeof(counter));
    sc_sha256_final(&c, S.rng_pool);
}

static void rng_init(void)
{
    uint8_t seed[128];
    size_t n = 0;
    uint32_t a, b, c, d;
    uint64_t v;
    cpuid_leaf(1, &a, &b, &c, &d);
    S.rdrand_ok = (c >> 30) & 1u;
    v = rdtsc();                       put_le64(seed + n, v); n += 8;
    v = (uint64_t)tick_count;          put_le64(seed + n, v); n += 8;
    v = now_epoch();                   put_le64(seed + n, v); n += 8;
    put_le64(seed + n, (uint64_t)a | ((uint64_t)b << 32)); n += 8;
    put_le64(seed + n, (uint64_t)c | ((uint64_t)d << 32)); n += 8;
    v = cpu_busy_cycles();             put_le64(seed + n, v); n += 8;
    for (int i = 0; i < 8 && n + 8 <= sizeof(seed); i++) {
        if (rdrand_word(&v)) { put_le64(seed + n, v); n += 8; }
    }
    v = rdtsc();                       put_le64(seed + n, v); n += 8;
    S.rng_counter = 1;
    S.rng_ready = 1;
    sc_sha256_hash(seed, n, S.rng_pool);
    sc_burn(seed, sizeof(seed));
    klog_raw(S.rdrand_ok ? "security: entropy pool seeded (RDRAND + TSC + RTC)"
                         : "security: entropy pool seeded (TSC + RTC; no RDRAND - limited)");
}

static void rng_bytes(void *out, size_t n)
{
    uint8_t *p = (uint8_t *)out;
    uint8_t jitter[24];
    if (!S.rng_ready) rng_init();
    put_le64(jitter, rdtsc());
    put_le64(jitter + 8, (uint64_t)tick_count);
    put_le64(jitter + 16, (uint64_t)input_key_enqueued);
    rng_mix(jitter, sizeof(jitter));
    while (n) {
        uint8_t block[SC_HASH_BYTES];
        sc_sha256 c;
        uint8_t counter[8];
        size_t take = n < SC_HASH_BYTES ? n : SC_HASH_BYTES;
        put_le64(counter, S.rng_counter++);
        sc_sha256_init(&c);
        sc_sha256_update(&c, S.rng_pool, sizeof(S.rng_pool));
        sc_sha256_update(&c, counter, sizeof(counter));
        sc_sha256_update(&c, jitter, sizeof(jitter));
        sc_sha256_final(&c, block);
        memcpy(p, block, take);
        p += take; n -= take;
        rng_mix(block, sizeof(block));       /* ratchet: past output cannot be recomputed */
        sc_burn(block, sizeof(block));
    }
    sc_burn(jitter, sizeof(jitter));
}

/* The vault needs the same audited entropy pool for volume keys, slot salts
 * and per-save IVs.  No other subsystem gets a raw-random API. */
void security_random_bytes(void *out, size_t n) { rng_bytes(out, n); }

void security_mix_event(uint8_t code)
{
    uint8_t e[16];
    put_le64(e, rdtsc());
    put_le64(e + 8, (uint64_t)tick_count);
    e[0] ^= code;
    rng_mix(e, sizeof(e));
}

static uint32_t calibrate_iters(void)
{
    uint8_t out[SC_HASH_BYTES], salt[SC_SALT_BYTES];
    uint64_t start, delta, want, calc, hz = cpu_tsc_hz;
    uint32_t i;
    if (!hz) return 100000;
    for (i = 0; i < sizeof(salt); i++) salt[i] = (uint8_t)(i * 13 + 7);
    start = rdtsc();
    sc_pbkdf2("scos-calibration", 16, salt, sizeof(salt), 2000, out);
    delta = rdtsc() - start;
    sc_burn(out, sizeof(out));
    if (!delta) return 100000;
    want = hz / 4;                                /* ~250 ms of work */
    calc = (uint64_t)2000 * want / delta;
    if (calc < 20000) calc = 20000;
    if (calc > 1000000) calc = 1000000;
    return (uint32_t)calc;
}

static void derive(const char *pw, const uint8_t *salt, uint32_t iters,
                   uint8_t out[SC_HASH_BYTES])
{
    sc_pbkdf2(pw, sc_len(pw), salt, SC_SALT_BYTES, iters, out);
}

/* ------------------------------------------------------------ path guard --- */
/* Lexical normalization so "system/x/../security/users.db" cannot sneak past
 * the string check.  A path that escapes the root is refused conservatively. */
static int norm_path(const char *path, char *out, size_t cap)
{
    size_t o = 0;
    const char *p = path;
    if (!path || !cap) return 0;
    out[0] = 0;
    while (*p) {
        size_t n;
        const char *s;
        while (*p == '/') p++;
        if (!*p) break;
        s = p;
        while (*p && *p != '/') p++;
        n = (size_t)(p - s);
        if (n == 1 && s[0] == '.') continue;
        if (n == 2 && s[0] == '.' && s[1] == '.') {
            if (!o) return 0;
            while (o && out[o-1] != '/') o--;
            if (o) o--;
            out[o] = 0;
            continue;
        }
        if (n >= VFS_NAME || o + n + (o ? 1 : 0) + 1 > cap) return 0;
        if (o) out[o++] = '/';
        memcpy(out + o, s, n);
        o += n;
        out[o] = 0;
    }
    return 1;
}

int security_protected_path(const char *path)
{
    static const char prefix[] = "system/security";
    char norm[256];
    size_t n = sc_len(prefix);
    if (!path) return 0;
    if (!norm_path(path, norm, sizeof(norm))) {
        /* An oversized segment or a path escaping the root is not a valid VFS
         * path anyway.  Only be conservative when it is plausibly aimed at the
         * account directory; unrelated long filenames stay ordinary files. */
        return sc_find(path, "security") || sc_find(path, "..");
    }
    return sc_len(norm) >= n && !sc_neq(norm, prefix, n) &&
           (norm[n] == 0 || norm[n] == '/');
}

int security_initialized(void) { return S.ready; }
int security_bypass_active(void) { return S.bypass || S.meta_io || !S.ready; }

/* A bounded, nestable scope for the account/vault/choices modules to write
 * their own protected metadata.  Not exported to apps or Lua. */
void security_bypass_scope(int on)
{
    if (on) { if (S.meta_io < 8) S.meta_io++; }
    else if (S.meta_io > 0) S.meta_io--;
}
void security_mark_dirty(void)
{
    S.disk_dirty = 1;
}

int security_write_denied(void)
{
    if (!S.ready || S.bypass || S.meta_io) return 0;
    if (!security_choice("roles.enabled")) return 0;
    if (S.session >= 0 && S.session < SC_USERS_MAX &&
        S.users[S.session].role == SC_ROLE_GUEST) return 1;
    return 0;
}

/* ------------------------------------------------------------------ audit --- */
static void audit_file_sync(void)
{
    int saved = S.bypass;
    int count = S.audit_count;
    size_t at = 0;
    audit_buf[0] = 0;
    add(audit_buf, sizeof(audit_buf), &at,
        "# SCos security audit log (oldest first; newest last)\n"
        "# epoch|event|uid|user|detail\n");
    if (count > SC_AUDIT_MAX) count = SC_AUDIT_MAX;
    for (int i = 0; i < count; i++) {
        int idx = (S.audit_head - count + i + SC_AUDIT_MAX * 2) % SC_AUDIT_MAX;
        if (!add(audit_buf, sizeof(audit_buf), &at, S.audit[idx])) break;
        if (!add(audit_buf, sizeof(audit_buf), &at, "\n")) break;
    }
    S.bypass = 1;
    vfs_write(AUDIT_PATH, audit_buf, (u32)at);
    S.bypass = saved;
    /* The audit log is part of the vault header's AEAD associated data: an
     * offline edit to history breaks the tag and is reported at next unlock. */
    vault_set_audit(audit_buf, (u32)at);
    S.disk_dirty = 1;
}

/* Reload the audit ring from the AEAD-authenticated header copy.  Lines are
 * `epoch|event|uid|user|detail`; comments start with '#'.  This is a bounded
 * parse into fixed BSS rows - never a heap allocation. */
static void audit_file_load(void)
{
    u32 len = 0;
    char *data;
    int saved = S.bypass;
    const char *p;
    S.bypass = 1;
    data = vfs_read(AUDIT_PATH, &len);
    S.bypass = saved;
    if (!data || !len || len > SC_AUDIT_BYTES) return;
    p = data;
    while (*p) {
        const char *nl = str_chr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : sc_len(p);
        if (n && p[0] != '#') {
            sc_copy(S.audit[S.audit_head], SC_AUDIT_LINE, p);
            S.audit[S.audit_head][n < SC_AUDIT_LINE ? n : SC_AUDIT_LINE - 1] = 0;
            S.audit_head = (S.audit_head + 1) % SC_AUDIT_MAX;
            if (S.audit_count < SC_AUDIT_MAX) S.audit_count++;
        }
        if (!nl) break;
        p = nl + 1;
    }
}

static void audit(const char *event, const char *detail)
{
    char line[SC_AUDIT_LINE];
    if (!security_choice("audit.enabled")) return;
    uint32_t uid = 0;
    const char *who = "-";
    size_t at = 0;
    char num[24];
    if (S.session >= 0 && S.session < SC_USERS_MAX) {
        uid = S.users[S.session].uid;
        who = S.users[S.session].name;
    }
    line[0] = 0;
    sc_fmt(num, sizeof(num), now_epoch());
    add(line, sizeof(line), &at, num);   add(line, sizeof(line), &at, "|");
    add(line, sizeof(line), &at, event); add(line, sizeof(line), &at, "|");
    sc_fmt(num, sizeof(num), uid);
    add(line, sizeof(line), &at, num);   add(line, sizeof(line), &at, "|");
    add(line, sizeof(line), &at, who);   add(line, sizeof(line), &at, "|");
    add(line, sizeof(line), &at, detail ? detail : "");
    sc_copy(S.audit[S.audit_head], SC_AUDIT_LINE, line);
    S.audit_head = (S.audit_head + 1) % SC_AUDIT_MAX;
    if (S.audit_count < SC_AUDIT_MAX) S.audit_count++;
    audit_file_sync();
}

/* --------------------------------------------------------------- database --- */
static int db_parse(const char *data, size_t len)
{
    char *p, *nl;
    int have_magic = 0, have_policy = 0, have_recovery = 0, nusers = 0;
    struct sc_policy pol;
    uint8_t r_salt[SC_SALT_BYTES], r_hash[SC_HASH_BYTES];
    uint32_t r_iters = 0, r_valid = 0;
    if (!sc_db_verify(data, len)) return 0;
    if (len >= sizeof(db_work)) return 0;
    memcpy(db_work, data, len);
    db_work[len] = 0;
    sc_policy_default(&pol);
    memset(r_salt, 0, sizeof(r_salt));
    memset(r_hash, 0, sizeof(r_hash));
    p = db_work;
    while (p && *p) {
        nl = str_chr(p, '\n');
        if (nl) *nl = 0;
        if (!have_magic) {
            if (!sc_eq(p, DB_MAGIC)) return 0;
            have_magic = 1;
        } else if (!sc_neq(p, "P|", 2)) {
            if (have_policy || !sc_policy_decode(p, &pol)) return 0;
            have_policy = 1;
        } else if (!sc_neq(p, "R|", 2)) {
            char buf[256], *f[5];
            uint64_t v;
            if (have_recovery || sc_len(p) >= sizeof(buf)) return 0;
            sc_cpy(buf, p);
            /* R|iters|salt_hex|hash_hex|valid */
            {
                int n = 0;
                char *q = buf;
                while (*q && n < 5) {
                    f[n++] = q;
                    while (*q && *q != '|') q++;
                    if (*q) *q++ = 0;
                }
                if (n != 5 || !sc_eq(f[0], "R")) return 0;
                if (!sc_num(f[1], &v) || v < 10000 || v > 2000000) return 0;
                r_iters = (uint32_t)v;
                if (sc_len(f[2]) != SC_SALT_BYTES*2 ||
                    sc_unhex(r_salt, sizeof(r_salt), f[2], SC_SALT_BYTES*2)) return 0;
                if (sc_len(f[3]) != SC_HASH_BYTES*2 ||
                    sc_unhex(r_hash, sizeof(r_hash), f[3], SC_HASH_BYTES*2)) return 0;
                if (!sc_num(f[4], &v) || v > 1) return 0;
                r_valid = (uint32_t)v;
            }
            have_recovery = 1;
        } else if (!sc_neq(p, "U|", 2)) {
            struct sc_account *a = &S.scratch[nusers];
            if (nusers >= SC_USERS_MAX || !sc_account_decode(p, a)) return 0;
            for (int i = 0; i < nusers; i++)
                if (S.scratch[i].uid == a->uid || sc_eq(S.scratch[i].name, a->name)) return 0;
            nusers++;
        } else if (!sc_neq(p, "C|", 2)) {
            break;
        } else {
            return 0;
        }
        p = nl ? nl + 1 : 0;
    }
    if (!have_magic || !have_policy || !nusers) return 0;
    {
        int enabled = 0;
        for (int i = 0; i < nusers; i++)
            if (!(S.scratch[i].flags & SC_FLAG_DISABLED)) enabled++;
        if (!enabled) return 0;         /* a database with nobody able to sign in */
        /* With equal permissions there is no such thing as a required admin:
         * the shipped root/scos account is a plain SC_ROLE_USER.  Roles can be
         * turned on later; security_is_admin() then falls back to equal rights
         * if no administrator exists, instead of rejecting this database and
         * resetting the vault (which would lose the owner's files). */
    }
    S.policy = pol;
    memcpy(S.users, S.scratch, sizeof(S.users));
    S.count = nusers;
    memcpy(S.rec_salt, r_salt, sizeof(r_salt));
    memcpy(S.rec_hash, r_hash, sizeof(r_hash));
    S.rec_iters = r_iters;
    S.rec_valid = r_valid && have_recovery;
    S.rec_code[0] = 0;                  /* plaintext codes never survive a reload */
    return 1;
}

static int db_present(const char *path)
{
    u32 len = 0;
    int saved = S.bypass;
    S.bypass = 1;
    char *data = vfs_read(path, &len);
    S.bypass = saved;
    return data != 0;
}

static int db_load(const char *path)
{
    u32 len = 0;
    char *data;
    int saved = S.bypass;
    S.bypass = 1;
    data = vfs_read(path, &len);
    S.bypass = saved;
    if (!data) { klog_raw("security: account database missing"); return 0; }
    if (!len || len >= SC_DB_MAX) { klog_raw("security: account database size invalid"); return 0; }
    if (!sc_db_verify(data, len)) { klog_raw("security: account database checksum mismatch"); return 0; }
    if (!db_parse(data, len)) { klog_raw("security: account database parse failed"); return 0; }
    return 1;
}

static void db_serialize(void)
{
    size_t at = 0;
    char digest[65], num[24], valid[8];
    db_buf[0] = 0;
    add(db_buf, sizeof(db_buf), &at, DB_MAGIC "\n");
    {
        int n = sc_policy_encode(&S.policy, line_buf, sizeof(line_buf));
        if (n <= 0) return;
        add(db_buf, sizeof(db_buf), &at, line_buf);
    }
    /* No recovery code is generated for the shipped default account (there is
     * no setup screen to show it on), so do not emit an R| line with zero
     * iterations - db_parse correctly rejects that.  `security recovery`
     * generates one later. */
    if (S.rec_iters) {
        sc_hex(hex_salt, sizeof(hex_salt), S.rec_salt, SC_SALT_BYTES);
        sc_hex(hex_hash, sizeof(hex_hash), S.rec_hash, SC_HASH_BYTES);
        sc_fmt(num, sizeof(num), S.rec_iters);
        sc_fmt(valid, sizeof(valid), S.rec_valid ? 1 : 0);
        add(db_buf, sizeof(db_buf), &at, "R|");
        add(db_buf, sizeof(db_buf), &at, num);   add(db_buf, sizeof(db_buf), &at, "|");
        add(db_buf, sizeof(db_buf), &at, hex_salt); add(db_buf, sizeof(db_buf), &at, "|");
        add(db_buf, sizeof(db_buf), &at, hex_hash); add(db_buf, sizeof(db_buf), &at, "|");
        add(db_buf, sizeof(db_buf), &at, valid); add(db_buf, sizeof(db_buf), &at, "\n");
    }
    for (int i = 0; i < S.count; i++) {
        int n = sc_account_encode(&S.users[i], line_buf, sizeof(line_buf));
        if (n <= 0) break;
        if (!add(db_buf, sizeof(db_buf), &at, line_buf)) break;
    }
    sc_db_digest(db_buf, at, digest);
    add(db_buf, sizeof(db_buf), &at, "C|");
    add(db_buf, sizeof(db_buf), &at, digest);
    add(db_buf, sizeof(db_buf), &at, "\n");
}

static void db_save(void)
{
    u32 oldlen = 0;
    char *old;
    int saved = S.bypass;
    /* Never replace a good database with an empty one.  A failed sign-in on a
     * machine that has not completed first-run setup must not create a file
     * that a later boot would reject. */
    if (!S.count) { audit_file_sync(); return; }
    S.bypass = 1;
    if (!vfs_lookup("system/security")) vfs_mkdir("system/security");
    old = vfs_read(DB_PATH, &oldlen);
    if (old && oldlen && oldlen < SC_DB_MAX && sc_db_verify(old, oldlen))
        vfs_write(DB_BAK, old, oldlen);
    db_serialize();
    vfs_write(DB_PATH, db_buf, (u32)sc_len(db_buf));
    S.bypass = saved;
    /* The account database is the authoritative half of the vault header.
     * It is cleartext (needed before sign-in) but the whole header is the
     * AEAD's associated data, so it cannot be edited offline unnoticed. */
    vault_set_db(db_buf, (u32)sc_len(db_buf));
    audit_file_sync();
    S.disk_dirty = 1;
}

void security_fs_restore(void)
{
    if (!S.ready) return;
    {
        int saved = S.bypass;
        S.bypass = 1;
        if (!vfs_lookup("system/security")) vfs_mkdir("system/security");
        S.bypass = saved;
    }
    db_save();
}

static void persist_now(void)
{
    if (!S.ready || !S.have_disk || !S.disk_dirty) return;
    if (!fs_image_available()) return;
    if (fs_image_save()) {
        S.disk_dirty = 0;
    } else {
        static int warned;
        if (!warned) { klog_raw("security: ATA save of the account database failed; retrying"); warned = 1; }
        audit("disk-save-failed", "ATA persistence failed");
    }
}

/* ------------------------------------------------------------- user lookup --- */
static int find_user(const char *name)
{
    char norm[SC_NAME_MAX];
    if (!name) return -1;
    sc_username_normal(name, norm);
    for (int i = 0; i < S.count; i++)
        if (S.users[i].used && sc_eq(S.users[i].name, norm)) return i;
    return -1;
}

static int verify_user(int idx, const char *pw)
{
    uint8_t got[SC_HASH_BYTES];
    int ok;
    if (idx < 0 || idx >= S.count || !pw) return 0;
    derive(pw, S.users[idx].salt, S.users[idx].iters, got);
    ok = sc_ct_eq(got, S.users[idx].hash, SC_HASH_BYTES);
    sc_burn(got, sizeof(got));
    return ok;
}

static void dummy_work(void)
{
    static const uint8_t salt[SC_SALT_BYTES] = {
        0x53,0x43,0x6f,0x53,0x2d,0x64,0x75,0x6d,
        0x6d,0x79,0x2d,0x73,0x61,0x6c,0x74,0x21 };
    uint8_t out[SC_HASH_BYTES];
    derive("no-such-account", salt, S.policy.iters, out);
    sc_burn(out, sizeof(out));
}

static void throttle(void)
{
    uint32_t shift = S.global_fails > 5 ? 5 : S.global_fails;
    if (!security_choice("lockout")) { S.deny_until = (uint64_t)tick_count + 5; return; }
    S.deny_until = (uint64_t)tick_count + (uint64_t)10 * (1u << shift);
    if (S.global_fails < 32) S.global_fails++;
}

static void record_failure(int idx, const char *event)
{
    uint64_t now = now_epoch();
    if (idx >= 0 && security_choice("lockout")) {
        struct sc_account *a = &S.users[idx];
        if (a->fails < 1000000) a->fails++;
        if (a->fails >= S.policy.max_fails) {
            uint32_t shift = a->fails - S.policy.max_fails;
            uint64_t delay = (uint64_t)S.policy.lock_base << (shift > 6 ? 6 : shift);
            if (delay > S.policy.lock_max) delay = S.policy.lock_max;
            a->lock_until = now + delay;
        }
    }
    throttle();
    audit(event, idx >= 0 ? S.users[idx].name : "unknown");
    db_save();
}

/* -------------------------------------------------------------- recovery --- */
static void recovery_generate(void)
{
    uint8_t raw[RECOVERY_CHARS];
    rng_bytes(raw, sizeof(raw));
    for (int i = 0; i < RECOVERY_CHARS; i++)
        S.rec_code[i] = RECOVERY_ALPHABET[raw[i] & 31];
    S.rec_code[RECOVERY_CHARS] = 0;
    sc_burn(raw, sizeof(raw));
    rng_bytes(S.rec_salt, sizeof(S.rec_salt));
    S.rec_iters = S.policy.iters;
    derive(S.rec_code, S.rec_salt, S.rec_iters, S.rec_hash);
    S.rec_valid = 1;
    /* A recovery code must also be able to unwrap the volume key: otherwise
     * recovering an administrator password would make saved data unreadable. */
    if (security_choice("vault.encryption") &&
        !vault_bind_recovery(S.rec_code, S.rec_iters)) {
        S.rec_valid = 0;
        klog_raw("security: recovery code could not be bound to the encrypted vault");
    }
    audit("recovery-generated", vault_disk_present() ?
          "one-time code shown on the secure screen; vault key slot bound" :
          "one-time code shown on the secure screen");
}

/* --------------------------------------------------------------- sessions --- */
static void begin_session(int idx)
{
    if (idx < 0 || idx >= S.count) return;
    S.session = idx;
    S.locked = 0;
    S.session_epoch = (uint32_t)now_epoch();
    S.deny_until = 0;
    S.global_fails = 0;
    input_last_tick = (u32)tick_count;
}

int security_api_authenticate(const char *name, const char *pw, char *msg, size_t cap)
{
    int idx;
    uint64_t now;
    struct sc_account *a;
    if (!S.ready) { sc_copy(msg, cap, "Security is not initialized."); return 0; }
    idx = find_user(name);
    if (idx < 0) {
        dummy_work();
        record_failure(-1, "login-fail-unknown");
        sc_copy(msg, cap, "Invalid username or password.");
        return 0;
    }
    a = &S.users[idx];
    now = now_epoch();
    if (a->flags & SC_FLAG_DISABLED) {
        dummy_work();
        sc_copy(msg, cap, "This account is disabled.");
        return 0;
    }
    if (security_choice("lockout") && a->lock_until > now) {
        char n[24];
        sc_fmt(n, sizeof(n), a->lock_until - now);
        sc_copy(msg, cap, "Sign-in temporarily blocked. Try again in ");
        sc_cat(msg, cap, n); sc_cat(msg, cap, " seconds.");
        return 0;
    }
    if (!verify_user(idx, pw)) {
        record_failure(idx, "login-fail");
        sc_copy(msg, cap, "Invalid username or password.");
        return 0;
    }
    /* Full-disk encryption: a correct account password is also the only thing
     * that unwraps the volume encryption key.  A valid verifier with a broken
     * or missing key slot cannot produce a usable session. */
    /* Even with encryption switched off, unwrap a pending legacy vault once so
     * the owner's data is readable before the next plaintext save. */
    if ((security_choice("vault.encryption") || vault_image_pending()) &&
        !vault_unlock_password(a->name, pw, a->iters, msg, cap)) {
        record_failure(idx, "vault-unlock-failed");
        sc_copy(msg, cap, "Saved data could not be unlocked with this password.");
        return 0;
    }
    if (vault_image_pending() && !fs_image_unlock_apply()) {
        audit("vault-auth-failed", a->name);
        sc_copy(msg, cap, "The encrypted vault did not authenticate. Saved data was withheld.");
        return 0;
    }
    a->fails = 0;
    a->lock_until = 0;
    a->last_login = now;
    begin_session(idx);
    S.force_change = (a->flags & SC_FLAG_MUSTCHANGE) != 0 ||
                     (security_choice("password.age") && S.policy.max_age_days &&
                      a->pw_changed && now > a->pw_changed &&
                      now - a->pw_changed > (uint64_t)S.policy.max_age_days * 86400u);
    audit("login-ok", a->name);
    db_save();
    sc_copy(msg, cap, "Welcome, ");
    sc_cat(msg, cap, a->name);
    sc_cat(msg, cap, ".");
    return 1;
}

int security_api_verify(int idx, const char *pw, char *msg, size_t cap)
{
    uint64_t now;
    struct sc_account *a;
    if (idx < 0 || idx >= S.count) { sc_copy(msg, cap, "No active session."); return 0; }
    a = &S.users[idx];
    now = now_epoch();
    if (a->lock_until > now) {
        char n[24];
        sc_fmt(n, sizeof(n), a->lock_until - now);
        sc_copy(msg, cap, "Verification blocked. Try again in ");
        sc_cat(msg, cap, n); sc_cat(msg, cap, " seconds.");
        return 0;
    }
    if (!verify_user(idx, pw)) {
        record_failure(idx, "verify-fail");
        sc_copy(msg, cap, "Password not accepted.");
        return 0;
    }
    a->fails = 0; a->lock_until = 0;
    S.deny_until = 0; S.global_fails = 0;
    audit("verify-ok", a->name);
    db_save();
    sc_copy(msg, cap, "Identity confirmed.");
    return 1;
}

int security_api_password_ok(const char *name, const char *pw, char *msg, size_t cap)
{
    return sc_password_eval(name, pw, &S.policy, msg, cap);
}

int security_api_change_password(int idx, const char *oldpw, const char *newpw,
                                  int require_old, char *msg, size_t cap)
{
    struct sc_account *a;
    uint8_t got[SC_HASH_BYTES];
    if (idx < 0 || idx >= S.count) { sc_copy(msg, cap, "No such account."); return 0; }
    a = &S.users[idx];
    if (require_old && !security_api_verify(idx, oldpw, msg, cap)) return 0;
    if (security_choice("password.policy")) {
        if (!sc_password_eval(a->name, newpw, &S.policy, msg, cap)) return 0;
    } else if (!newpw || sc_len(newpw) < 4) {
        sc_copy(msg, cap, "Password too short (minimum 4 characters)."); return 0;
    }
    /* Password history: each remembered entry keeps its own salt and verifier. */
    for (int i = 0; i < (security_choice("password.history") ? a->hist_count : 0); i++) {
        derive(newpw, a->hist[i], a->iters, got);
        if (sc_ct_eq(got, a->hist[i] + SC_SALT_BYTES, SC_HASH_BYTES)) {
            sc_burn(got, sizeof(got));
            sc_copy(msg, cap, "That password was used recently. Choose a different one.");
            return 0;
        }
    }
    sc_burn(got, sizeof(got));
    if (S.policy.history && security_choice("password.history")) {
        int keep = (int)S.policy.history - 1;
        if (a->hist_count > keep) a->hist_count = (uint8_t)keep;
        if (a->hist_count < SC_HISTORY_MAX) {
            for (int i = a->hist_count; i > 0; i--)
                memcpy(a->hist[i], a->hist[i-1], SC_SALT_BYTES + SC_HASH_BYTES);
            a->hist_count++;
        }
        memcpy(a->hist[0], a->salt, SC_SALT_BYTES);
        memcpy(a->hist[0] + SC_SALT_BYTES, a->hash, SC_HASH_BYTES);
    }
    /* Re-wrap the volume key under a KEK derived from the NEW password first.
     * If that fails the old credentials must stay valid, so the in-memory
     * account is only committed afterwards. */
    if (security_choice("vault.encryption") &&
        !vault_rebind_password(a->name, newpw, a->iters)) {
        sc_copy(msg, cap, "The encrypted vault is locked. Sign in before changing a password.");
        return 0;
    }
    rng_bytes(a->salt, sizeof(a->salt));
    derive(newpw, a->salt, a->iters, a->hash);
    a->pw_changed = now_epoch();
    if (sc_eq(a->name, SC_DEFAULT_USER)) S.default_pw = 0;
    a->flags &= ~SC_FLAG_MUSTCHANGE;
    a->fails = 0; a->lock_until = 0;
    S.force_change = 0;
    audit("password-changed", a->name);
    db_save();
    sc_copy(msg, cap, "Password changed. ");
    sc_cat(msg, cap, "Store it somewhere safe; the old one no longer works.");
    return 1;
}

int security_create_admin(const char *name, const char *pw, const char *confirm)
{
    struct sc_account *a;
    char norm[SC_NAME_MAX], msg[SC_MSG_MAX];
    if (!S.ready) { set_error("Security is not initialized."); return 0; }
    if (S.count) { set_error("Accounts already exist."); return 0; }
    if (!sc_username_valid(name)) { set_error("Use 3-31 letters, digits, dot, dash or underscore."); return 0; }
    if (!pw || !confirm || !sc_eq(pw, confirm)) { set_error("The passwords do not match."); return 0; }
    if (!sc_password_eval(name, pw, &S.policy, msg, sizeof(msg))) {
        set_error(msg);
        sc_burn(msg, sizeof(msg));
        return 0;
    }
    sc_username_normal(name, norm);
    S.policy.iters = calibrate_iters();
    memset(&S.users[0], 0, sizeof(S.users[0]));
    a = &S.users[0];
    sc_copy(a->name, sizeof(a->name), norm);
    sc_copy(a->display, sizeof(a->display), norm);
    a->uid = 1000; a->role = SC_ROLE_ADMIN; a->flags = 0;
    a->iters = S.policy.iters;
    a->created = a->pw_changed = now_epoch();
    a->last_login = a->created;
    rng_bytes(a->salt, sizeof(a->salt));
    derive(pw, a->salt, a->iters, a->hash);
    a->used = 1;
    S.count = 1;
    /* First-run setup is the only path allowed to discard an unreadable vault:
     * there are no accounts to authenticate against, so the old volume key is
     * gone anyway.  A brand new one is created and wrapped to this password. */
    if (security_choice("vault.encryption")) {
        vault_reset();
        if (!vault_unlock_password(a->name, pw, a->iters, 0, 0)) {
            S.count = 0;
            set_error("Could not create the encrypted vault.");
            return 0;
        }
    }
    begin_session(0);
    S.force_change = 0;
    recovery_generate();
    db_save();
    audit("setup", "first administrator created");
    sc_burn(msg, sizeof(msg));
    return 1;
}

int security_api_create_user(const char *name, int role, const char *pw)
{
    struct sc_account *a;
    char norm[SC_NAME_MAX], msg[SC_MSG_MAX];
    uint32_t uid = 1000;
    if (!S.ready || S.count >= SC_USERS_MAX) { set_error("User table is full."); return 0; }
    if (!security_is_admin()) { set_error("Administrator rights required."); return 0; }
    if (!sc_username_valid(name)) { set_error("Invalid username."); return 0; }
    sc_username_normal(name, norm);
    if (find_user(norm) >= 0) { set_error("That username already exists."); return 0; }
    if (role < SC_ROLE_ADMIN || role > SC_ROLE_GUEST) { set_error("Unknown role."); return 0; }
    if (!sc_password_eval(norm, pw, &S.policy, msg, sizeof(msg))) { set_error(msg); return 0; }
    for (int i = 0; i < S.count; i++) if (S.users[i].uid >= uid) uid = S.users[i].uid;
    a = &S.users[S.count];
    memset(a, 0, sizeof(*a));
    sc_copy(a->name, sizeof(a->name), norm);
    sc_copy(a->display, sizeof(a->display), norm);
    a->uid = uid + 1; a->role = (uint32_t)role; a->flags = SC_FLAG_MUSTCHANGE;
    a->iters = S.policy.iters;
    a->created = a->pw_changed = now_epoch();
    if (security_choice("vault.encryption") &&
        !vault_rebind_password(norm, pw, a->iters)) {
        memset(a, 0, sizeof(*a));
        set_error("The encrypted vault is locked. Sign in before adding an account.");
        return 0;
    }
    rng_bytes(a->salt, sizeof(a->salt));
    derive(pw, a->salt, a->iters, a->hash);
    a->used = 1;
    S.count++;
    audit("user-created", a->name);
    db_save();
    return 1;
}

int security_api_delete_user(const char *name)
{
    int idx = find_user(name), admins = 0;
    if (idx < 0) { set_error("No such user."); return 0; }
    if (!security_is_admin()) { set_error("Administrator rights required."); return 0; }
    if (idx == S.session) { set_error("Use logout before deleting your own account."); return 0; }
    for (int i = 0; i < S.count; i++)
        if (i != idx && S.users[i].role == SC_ROLE_ADMIN &&
            !(S.users[i].flags & SC_FLAG_DISABLED)) admins++;
    if (security_choice("roles.enabled") && !admins) {
        set_error("Cannot delete the last administrator."); return 0;
    }
    audit("user-deleted", S.users[idx].name);
    if (sc_eq(S.users[idx].name, SC_DEFAULT_USER)) S.default_pw = 0;
    vault_unbind(S.users[idx].name);
    for (int i = idx; i < S.count - 1; i++) S.users[i] = S.users[i+1];
    S.count--;
    memset(&S.users[S.count], 0, sizeof(S.users[0]));
    if (S.session > idx) S.session--;
    db_save();
    return 1;
}

int security_api_set_role(const char *name, int role)
{
    int idx = find_user(name);
    if (idx < 0 || role < SC_ROLE_ADMIN || role > SC_ROLE_GUEST) return 0;
    if (!security_is_admin()) return 0;
    if (security_choice("roles.enabled") &&
        S.users[idx].role == SC_ROLE_ADMIN && (uint32_t)role != SC_ROLE_ADMIN) {
        int admins = 0;
        for (int i = 0; i < S.count; i++)
            if (i != idx && S.users[i].role == SC_ROLE_ADMIN &&
                !(S.users[i].flags & SC_FLAG_DISABLED)) admins++;
        if (!admins) return 0;
    }
    if (S.users[idx].role == (uint32_t)role) return 1;
    S.users[idx].role = (uint32_t)role;
    audit(role == SC_ROLE_ADMIN ? "role-admin" :
          role == SC_ROLE_USER ? "role-user" : "role-guest", S.users[idx].name);
    db_save();
    return 1;
}

int security_api_set_disabled(const char *name, int disabled)
{
    int idx = find_user(name);
    if (idx < 0 || !security_is_admin()) return 0;
    if (idx == S.session && disabled) return 0;
    if (security_choice("roles.enabled") && disabled &&
        S.users[idx].role == SC_ROLE_ADMIN) {
        int admins = 0;
        for (int i = 0; i < S.count; i++)
            if (i != idx && S.users[i].role == SC_ROLE_ADMIN &&
                !(S.users[i].flags & SC_FLAG_DISABLED)) admins++;
        if (!admins) return 0;
    }
    if (disabled) S.users[idx].flags |= SC_FLAG_DISABLED;
    else { S.users[idx].flags &= ~SC_FLAG_DISABLED; S.users[idx].fails = 0; S.users[idx].lock_until = 0; }
    audit(disabled ? "account-locked" : "account-unlocked", S.users[idx].name);
    db_save();
    return 1;
}

int security_api_reset_user(const char *name, char *msg, size_t cap)
{
    int idx = find_user(name);
    if (idx < 0) { sc_copy(msg, cap, "No such user."); return 0; }
    if (!security_is_admin()) { sc_copy(msg, cap, "Administrator rights required."); return 0; }
    S.users[idx].flags |= SC_FLAG_MUSTCHANGE;
    S.users[idx].fails = 0;
    S.users[idx].lock_until = 0;
    if (idx == S.session) S.force_change = 1;
    audit("password-reset", S.users[idx].name);
    db_save();
    sc_copy(msg, cap, "Password reset: ");
    sc_cat(msg, cap, S.users[idx].name);
    sc_cat(msg, cap, " must choose a new one at the next sign-in.");
    return 1;
}

int security_api_recovery_active(void) {
    return S.rec_valid && security_choice("recovery.enabled");
}

const char *security_api_recovery_code(void) { return S.rec_code; }

void security_api_ack_recovery(void)
{
    sc_burn(S.rec_code, sizeof(S.rec_code));
}

int security_api_redeem_recovery(const char *code, const char *newpw, char *msg, size_t cap)
{
    uint8_t got[SC_HASH_BYTES];
    int admin = -1;
    if (!S.rec_valid || !code || !newpw) { sc_copy(msg, cap, "No recovery code is active."); return 0; }
    derive(code, S.rec_salt, S.rec_iters, got);
    if (!sc_ct_eq(got, S.rec_hash, SC_HASH_BYTES)) {
        sc_burn(got, sizeof(got));
        record_failure(-1, "recovery-fail");
        sc_copy(msg, cap, "Recovery code not accepted.");
        return 0;
    }
    sc_burn(got, sizeof(got));
    for (int i = 0; i < S.count; i++)
        if (!(S.users[i].flags & SC_FLAG_DISABLED) &&
            (!security_choice("roles.enabled") ||
             S.users[i].role == SC_ROLE_ADMIN))
            if (admin < 0 || S.users[i].uid < S.users[admin].uid) admin = i;
    if (admin < 0) { sc_copy(msg, cap, "No enabled account to recover."); return 0; }
    /* The one-time code also unwraps the volume key; without this step a
     * recovery would reset the password and destroy access to saved data. */
    if (security_choice("vault.encryption") &&
        !vault_unlock_recovery(code, msg, cap)) {
        record_failure(-1, "recovery-vault-failed");
        return 0;
    }
    if (vault_image_pending() && !fs_image_unlock_apply()) {
        sc_copy(msg, cap, "The encrypted vault did not authenticate. Saved data was withheld.");
        return 0;
    }
    if (!security_api_change_password(admin, 0, newpw, 0, msg, cap)) return 0;
    vault_forget_recovery();
    S.rec_valid = 0;
    S.rec_code[0] = 0;
    memset(S.rec_salt, 0, sizeof(S.rec_salt));
    memset(S.rec_hash, 0, sizeof(S.rec_hash));
    begin_session(admin);
    S.force_change = 0;
    audit("recovery-used", S.users[admin].name);
    db_save();
    sc_copy(msg, cap, "Account password reset. You are now signed in as ");
    sc_cat(msg, cap, S.users[admin].name);
    sc_cat(msg, cap, ".");
    return 1;
}

/* ------------------------------------------------------------- state API --- */
int  security_api_gate_satisfied(void) { return S.ready && S.session >= 0 && !S.force_change; }
int  security_api_force_change(void) { return S.force_change; }
int  security_api_session(void) { return S.session; }
int  security_api_user_count(void) { return S.count; }
int  security_api_persistent(void) { return S.have_disk && fs_image_available(); }
int  security_api_denied(void) { return (uint64_t)tick_count < S.deny_until; }

uint32_t security_api_deny_remaining(void)
{
    if (!security_api_denied()) return 0;
    return (uint32_t)(((S.deny_until - (uint64_t)tick_count) + 9) / 10);
}

const char *security_api_session_name(void)
{
    if (S.session >= 0 && S.session < S.count) return S.users[S.session].name;
    return "nobody";
}

void security_api_drop_session(void)
{
    S.session = -1;
    S.locked = 0;
    S.force_change = 0;
}

void security_api_lock_session(void) {
    if (security_choice("lock.enabled")) S.locked = 1;
}
void security_api_unlock_session(void)
{
    S.locked = 0;
    input_last_tick = (u32)tick_count;
}
int  security_api_locked(void) { return S.locked; }

void security_api_min_policy(struct sc_policy *p) { if (p) *p = S.policy; }

void security_api_build_hint(char *out, size_t cap)
{
    struct sc_policy p;
    security_api_min_policy(&p);
    sc_policy_hint(&p, out, cap);
    sc_cat(out, cap, " Passwords are never displayed.");
}

int security_vault_status(char *out, size_t cap)
{
    if (!vault_disk_present()) return 0;
    sc_copy(out, cap, "disk vault: ");
    sc_cat(out, cap, vault_image_pending() ? "locked (encrypted tree withheld)" :
           vault_unlocked() ? "unlocked (AES-256-CTR + HMAC-SHA256)" :
           "volume key not held");
    return 1;
}

const char *security_current_user(void) { return security_api_session_name(); }

int security_is_admin(void)
{
    int any_admin = 0;
    if (S.session < 0 || S.session >= S.count) return 0;
    /* All accounts are equal unless the owner explicitly enables roles. */
    if (!security_choice("roles.enabled")) return 1;
    for (int i = 0; i < S.count; i++)
        if (!(S.users[i].flags & SC_FLAG_DISABLED) &&
            S.users[i].role == SC_ROLE_ADMIN) { any_admin = 1; break; }
    /* Never let a role edit lock the owner out of the choices switch. */
    if (!any_admin) return 1;
    return S.users[S.session].role == SC_ROLE_ADMIN;
}

/* --------------------------------------------------- default account (Linux-like) --- */
/* At boot verification Linux asks for a login and password on a text console.
 * SCos does the same, and ships with one normal account so a fresh machine is
 * usable immediately: username `root`, password `scos`.  `root` is NOT a
 * superuser here - it is just a name, and all accounts have equal permissions
 * unless the owner turns `roles.enabled` on.  The password is deliberately a
 * weak default; the console hints at it and `passwd` clears the hint. */
static int default_pw_active(void)
{
    int idx = find_user(SC_DEFAULT_USER);
    return idx >= 0 && verify_user(idx, SC_DEFAULT_PASS);
}

static void create_default_account(void)
{
    struct sc_account *a = &S.users[0];
    S.policy.iters = calibrate_iters();
    memset(a, 0, sizeof(*a));
    sc_copy(a->name, sizeof(a->name), SC_DEFAULT_USER);
    sc_copy(a->display, sizeof(a->display), SC_DEFAULT_USER);
    a->uid = 1000;
    a->role = SC_ROLE_ADMIN;             /* highest power, but still just an account record */
    a->flags = 0;
    a->iters = S.policy.iters;
    a->created = a->pw_changed = a->last_login = now_epoch();
    rng_bytes(a->salt, sizeof(a->salt));
    derive(SC_DEFAULT_PASS, a->salt, a->iters, a->hash);
    a->used = 1;
    S.count = 1;
    S.db_loaded = 1;
    S.default_pw = 1;
    /* Like first-run setup, an empty account database means the old volume key
     * is gone.  Create a fresh vault and wrap its key to scos so sign-in works
     * and the saved filesystem is not silently discarded. */
    if (security_choice("vault.encryption")) {
        vault_reset();
        vault_unlock_password(SC_DEFAULT_USER, SC_DEFAULT_PASS, a->iters, 0, 0);
    }
    db_save();
    audit("default-account", "root/scos created; change it with passwd");
    klog_raw("security: default account root/scos created (administrator)");
}

int security_api_default_credentials(void) { return S.default_pw; }

/* ---------------------------------------------------------- initialization --- */
void security_init(int have_disk)
{
    memset(&S, 0, sizeof(S));
    security_choices_default();
    sc_policy_default(&S.policy);
    S.session = -1;
    S.have_disk = have_disk;
    rng_init();
    {
        int saved = S.bypass;
        S.bypass = 1;
        if (!vfs_lookup("system/security")) vfs_mkdir("system/security");
        S.bypass = saved;
    }
    S.ready = 1;
    /* Load choices first: the account database parser needs to know whether
     * role privileges are enabled before it validates the admin invariant. */
    security_choices_load();
    if (db_load(DB_PATH)) {
        S.db_loaded = 1;
    } else if (db_load(DB_BAK)) {
        S.db_loaded = 1;
        S.corrupt = 1;
        klog_raw("security: primary account database failed verification; backup loaded");
        db_save();
    } else {
        S.count = 0;
        /* A truly empty machine is not "corrupt"; only files that existed and
         * failed verification are.  `security' reports the difference. */
        S.corrupt = db_present(DB_PATH) || db_present(DB_BAK);
        klog_raw(S.corrupt ? "security: account database rejected; first-run setup required"
                           : "security: no account database; first-run setup required");
    }
    /* The header's audit log is AEAD-authenticated; load it before adding the
     * boot event so history survives a reboot and cannot be silently reset. */
    audit_file_load();
    if (!vfs_lookup(SC_CHOICES_PATH)) security_choices_save();
    if (!S.count) create_default_account();
    else S.default_pw = default_pw_active();
    audit("boot", S.db_loaded ? "accounts loaded" : "default account created");
    audit_file_sync();
}

void security_login_gate(void)
{
    if (!S.ready) return;
    klog_raw("security: sign-in required");
    if (!S.count) security_ui_setup();
    else if (!security_choice("login.required")) {
        /* Owner's choice: no login prompt.  With equal permissions the
         * lowest-uid enabled account is the implicit session; with roles on it
         * has to be an administrator.  A default account always exists. */
        int best = -1;
        for (int i = 0; i < S.count; i++)
            if (!(S.users[i].flags & SC_FLAG_DISABLED) &&
                (!security_choice("roles.enabled") ||
                 S.users[i].role == SC_ROLE_ADMIN) &&
                (best < 0 || S.users[i].uid < S.users[best].uid)) best = i;
        if (best >= 0) {
            begin_session(best);
            audit("auto-login", S.users[best].name);
            db_save();
        } else security_ui_login();
        if (S.session >= 0 && security_choice("vault.encryption")) {
            /* No password was typed, so a locked vault cannot be opened. */
            if (vault_image_pending()) {
                klog_raw("security: login.required=off but the encrypted vault needs a password");
                security_api_drop_session();
                security_ui_login();
            }
        }
    }
    else security_ui_login();
    if (S.session < 0) panic("security: sign-in gate exited without a session");
    S.locked = 0;
    S.force_change = 0;
    input_last_tick = (u32)tick_count;
    persist_now();
    wm_request_full();
}

void security_wm_tick(void)
{
    uint32_t t;
    if (!S.ready) return;
    t = (u32)tick_count;
    if (S.disk_dirty && (uint32_t)(t - S.last_save_try) >= 100) {
        S.last_save_try = t;
        persist_now();
    }
    if (S.pending) { security_run_pending(); wm_request_full(); return; }
    if (S.session >= 0 && !S.locked && security_choice("lock.enabled") &&
        security_choice("lock.idle") && S.policy.idle_minutes) {
        uint32_t idle = t - (uint32_t)input_last_tick;
        if (idle > S.policy.idle_minutes * 600u) {
            audit("auto-lock", "idle timeout");
            security_ui_lock();
            wm_request_full();
        }
    }
}

/* ---------------------------------------------------------- pending flows --- */
static void close_all_windows(void)
{
    int guard = 0;
    while (wm_win_count() > 0 && guard++ < 64) {
        struct window *w = wm_win_at(0);
        if (!w) break;
        wm_close_window(w);
    }
}

void security_run_pending(void)
{
    int kind = S.pending;
    char arg[SC_NAME_MAX];
    int idx = S.pending_idx, role = S.pending_role;
    if (!kind) return;
    sc_copy(arg, sizeof(arg), S.pending_arg);
    S.pending = PEND_NONE;
    S.pending_arg[0] = 0;
    switch (kind) {
    case PEND_LOCK:
        security_ui_lock();
        break;
    case PEND_LOGOUT:
        audit("logout", security_api_session_name());
        close_all_windows();
        security_api_drop_session();
        security_ui_logout();
        input_last_tick = (u32)tick_count;
        persist_now();
        break;
    case PEND_PASSWD:
        if (security_ui_change_password(idx, 1))
            wm_notify("Security", "Password changed successfully.", 0);
        else wm_notify("Security", "Password was not changed.", 1);
        break;
    case PEND_USERADD:
        if (security_ui_user_add(arg, role))
            wm_notify("Security", "Account created; the user must set a new password.", 0);
        else wm_notify("Security", security_api_last_error(), 1);
        break;
    case PEND_USERDEL:
        if (security_ui_user_del(arg))
            wm_notify("Security", "Account removed.", 0);
        else wm_notify("Security", security_api_last_error(), 1);
        break;
    case PEND_SU:
        if (security_ui_switch_user(arg))
            wm_notify("Security", security_current_user(), 0);
        else wm_notify("Security", "Switch user was cancelled.", 1);
        break;
    case PEND_REAUTH:
        if (security_ui_reauth("Re-authentication"))
            wm_notify("Security", "Identity confirmed.", 0);
        else wm_notify("Security", "Re-authentication failed.", 1);
        break;
    case PEND_RECOVERY: {
        char text[256];
        recovery_generate();
        sc_copy(text, sizeof(text),
                "Write this one-time recovery code down and keep it offline:\n\n    ");
        sc_cat(text, sizeof(text), S.rec_code);
        sc_cat(text, sizeof(text),
               "\n\nIt resets the administrator password from the sign-in screen (F8). "
               "It is valid once and is stored only as a salted hash.");
        security_ui_info("Recovery code", text);
        security_api_ack_recovery();
        db_save();
        wm_notify("Security", "A new recovery code was generated.", 0);
        break;
    }
    default:
        break;
    }
    input_last_tick = (u32)tick_count;
}

static int queue(int kind, const char *arg, int idx, int role)
{
    if (S.pending) return 0;
    S.pending = kind;
    S.pending_idx = idx;
    S.pending_role = role;
    sc_copy(S.pending_arg, sizeof(S.pending_arg), arg ? arg : "");
    return 1;
}

int security_reauthenticate(const char *reason)
{
    if (!S.ready || S.session < 0) return 0;
    return security_ui_reauth(reason ? reason : "Maintenance console");
}

/* --------------------------------------------------- command authorization --- */
static int first_tokens(const char *line, char *a, size_t acap, char *b, size_t bcap)
{
    char buf[256], *f[3];
    int n = 0;
    char *p;
    a[0] = 0; b[0] = 0;
    sc_copy(buf, sizeof(buf), line);
    p = buf;
    while (*p && n < 3) {
        while (*p == ' ') p++;
        if (!*p) break;
        f[n++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    if (n > 0) sc_copy(a, acap, f[0]);
    if (n > 1) sc_copy(b, bcap, f[1]);
    return n;
}

static int in_list(const char *s, const char *const *list)
{
    for (int i = 0; list[i]; i++) if (sc_eq(s, list[i])) return 1;
    return 0;
}

int security_command_allowed(const char *line, char *out, size_t cap)
{
    static const char *const admin_cmds[] = {
        "save", "shutdown", "poweroff", "reboot", "panic", "sysrq",
        "appuninstall", "make", 0
    };
    /* Default-deny for guests.  A deny-list silently grants every command a
     * future update adds; an allow-list grants only commands known to be
     * read-only (or already protected by their own admin check). */
    static const char *const guest_allow[] = {
        "help", "ls", "dir", "tree", "cat", "head", "wc", "grep", "hexdump",
        "dump", "pwd", "cd", "date", "time", "version", "neofetch", "sysinfo",
        "scinfo", "graphics", "dmesg", "klog", "free", "cpu", "uptime", "df",
        "disks", "procs", "history", "clear", "echo", "cal", "calc", "about",
        "whoami", "id", "security", "users", "audit", "policy", "passwd",
        "lock", "logout", "su", "reauth", "choices", 0
    };
    char cmd[64], arg[64];
    int role, n;
    if (!S.ready) return 1;
    if (!line) { sc_copy(out, cap, "No command."); return 0; }
    if (sc_find(line, "system/security") ||
        (sc_find(line, "security") && sc_find(line, ".."))) {
        sc_copy(out, cap, "Permission denied: security files are managed by the account system "
                          "(use 'security', 'users', 'policy' or 'audit').");
        return 0;
    }
    if (S.session < 0 || S.session >= S.count) {
        sc_copy(out, cap, "Authentication required.");
        return 0;
    }
    role = (int)S.users[S.session].role;
    n = first_tokens(line, cmd, sizeof(cmd), arg, sizeof(arg));
    if (!n) return 1;
    if (security_choice("roles.enabled") && role == SC_ROLE_GUEST &&
        security_choice("guest.restrict") && !in_list(cmd, guest_allow)) {
        sc_copy(out, cap, "Permission denied: the guest command allow-list is read-only. "
                          "Ask an administrator for a standard account.");
        return 0;
    }
    if (security_choice("roles.enabled") && role != SC_ROLE_ADMIN) {
        if (in_list(cmd, admin_cmds) ||
            (sc_eq(cmd, "kill") && sc_eq(arg, "--system")) ||
            (sc_eq(cmd, "rm") && (sc_eq(arg, "-s") || sc_eq(arg, "-f"))) ||
            (sc_eq(cmd, "theme") && n > 1)) {
            sc_copy(out, cap, "Permission denied: administrator rights required.");
            return 0;
        }
    }
    return 1;
}

/* --------------------------------------------------------------- shell --- */
struct outbuf { char *p, *end; };

static void ob(struct outbuf *b, const char *s)
{
    while (*s && b->p < b->end - 1) *b->p++ = *s++;
    *b->p = 0;
}

static void ob_num(struct outbuf *b, uint64_t v)
{
    char t[24];
    sc_fmt(t, sizeof(t), v);
    ob(b, t);
}

static const char *role_name(uint32_t r)
{
    return r == SC_ROLE_ADMIN ? "administrator" :
           r == SC_ROLE_USER ? "standard user" : "guest (read-only)";
}

static void shell_status(struct outbuf *b)
{
    struct sc_account *a = S.session >= 0 ? &S.users[S.session] : 0;
    ob(b, "SCos security status\n");
    ob(b, "  signed in      : "); ob(b, security_api_session_name());
    ob(b, " (uid "); ob_num(b, a ? a->uid : 0); ob(b, ", ");
    ob(b, security_choice("roles.enabled") ? (a ? role_name(a->role) : "none")
                                           : "administrator (all accounts equal)");
    ob(b, ")\n");
    ob(b, "  session start  : epoch "); ob_num(b, S.session_epoch); ob(b, "\n");
    ob(b, "  accounts       : "); ob_num(b, (uint64_t)S.count);
    ob(b, " in "); ob(b, S.db_loaded ? "verified database" : "first-run setup"); ob(b, "\n");
    ob(b, "  storage        : ");
    ob(b, security_api_persistent() ? "verified ATA persistence (survives reboot)" :
                                      "RAM only (accounts are session-only)");
    ob(b, "\n  password hash  : PBKDF2-HMAC-SHA256, 128-bit salt, ");
    ob_num(b, S.policy.iters); ob(b, " iterations (per-record)\n");
    ob(b, "  integrity      : SHA-256 database checksum + previous-good backup");
    ob(b, S.corrupt ? " (setup/repair was needed)\n" : "\n");
    ob(b, "  disk vault     : ");
    if (!vault_disk_present()) ob(b, "none (RAM-only machine)\n");
    else if (vault_image_pending()) ob(b, "LOCKED - encrypted tree withheld until sign-in\n");
    else if (vault_unlocked()) ob(b, "unlocked; AES-256-CTR + HMAC-SHA256, wrapped VEK per user\n");
    else ob(b, "volume key not held (sign in to unlock)\n");
    ob(b, "  auto-lock      : ");
    if (S.policy.idle_minutes) { ob_num(b, S.policy.idle_minutes); ob(b, " minutes idle\n"); }
    else ob(b, "disabled\n");
    ob(b, "  lockout        : "); ob_num(b, S.policy.max_fails);
    ob(b, " failures, "); ob_num(b, S.policy.lock_base); ob(b, "s to ");
    ob_num(b, S.policy.lock_max); ob(b, "s\n");
    ob(b, "  recovery code  : "); ob(b, S.rec_valid ? "active (one use)" : "not active"); ob(b, "\n");
    ob(b, "  user choices   : "); ob_num(b, (uint64_t)security_choice_count());
    ob(b, " switches (see 'choices'); isolation is a per-app user choice\n");
    ob(b, "  scope          : console authentication and disk encryption; no forced ");
    ob(b, "process isolation. See docs/migration/SECURITY.md.\n");
}

static void shell_users(struct outbuf *b)
{
    char n[24];
    ob(b, "uid   name                  role            state             last login   fails\n");
    for (int i = 0; i < S.count; i++) {
        struct sc_account *a = &S.users[i];
        const char *state = a->flags & SC_FLAG_DISABLED ? "disabled" :
                            a->flags & SC_FLAG_MUSTCHANGE ? "change required" :
                            a->lock_until > now_epoch() ? "locked out" : "active";
        sc_fmt(n, sizeof(n), a->uid); ob(b, n);
        ob(b, "  "); ob(b, a->name);
        for (int k = (int)sc_len(a->name); k < 22; k++) ob(b, " ");
        ob(b, role_name(a->role));
        for (int k = (int)sc_len(role_name(a->role)); k < 18; k++) ob(b, " ");
        ob(b, state);
        for (int k = (int)sc_len(state); k < 18; k++) ob(b, " ");
        sc_fmt(n, sizeof(n), a->last_login); ob(b, n);
        ob(b, "   "); sc_fmt(n, sizeof(n), a->fails); ob(b, n);
        ob(b, i == S.session ? "  <- you\n" : "\n");
    }
    if (!security_choice("roles.enabled"))
        ob(b, "roles.enabled is off: every account has full administrator permissions.\n");
}

static void shell_policy(struct outbuf *b, int argc, char **argv)
{
    struct sc_policy p = S.policy;
    if (argc == 1 || sc_eq(argv[1], "show")) {
        char h[192];
        ob(b, "min_len      : "); ob_num(b, p.min_len); ob(b, "\n");
        ob(b, "min_classes  : "); ob_num(b, p.min_classes); ob(b, "\n");
        ob(b, "min_bits     : "); ob_num(b, p.min_bits); ob(b, "\n");
        ob(b, "max_fails    : "); ob_num(b, p.max_fails); ob(b, "\n");
        ob(b, "lock_base    : "); ob_num(b, p.lock_base); ob(b, "s\n");
        ob(b, "lock_max     : "); ob_num(b, p.lock_max); ob(b, "s\n");
        ob(b, "idle         : "); ob_num(b, p.idle_minutes); ob(b, " min (0 = off)\n");
        ob(b, "max_age      : "); ob_num(b, p.max_age_days); ob(b, " days (0 = off)\n");
        ob(b, "history      : "); ob_num(b, p.history); ob(b, " passwords\n");
        ob(b, "iters        : "); ob_num(b, p.iters); ob(b, " (new accounts only)\n");
        sc_policy_hint(&p, h, sizeof(h));
        ob(b, h); ob(b, "\n");
        return;
    }
    if (argc != 4 || !sc_eq(argv[1], "set")) {
        ob(b, "Usage: policy [show|set <key> <value>]\n"
              "Keys: minlen classes minbits maxfails lockbase lockmax idle maxage history iters\n");
        return;
    }
    {
        uint64_t v;
        uint32_t *slot = 0;
        if (!sc_num(argv[3], &v) || v > 2000000) { ob(b, "Invalid value.\n"); return; }
        if (sc_eq(argv[2], "minlen")) slot = &p.min_len;
        else if (sc_eq(argv[2], "classes")) slot = &p.min_classes;
        else if (sc_eq(argv[2], "minbits")) slot = &p.min_bits;
        else if (sc_eq(argv[2], "maxfails")) slot = &p.max_fails;
        else if (sc_eq(argv[2], "lockbase")) slot = &p.lock_base;
        else if (sc_eq(argv[2], "lockmax")) slot = &p.lock_max;
        else if (sc_eq(argv[2], "idle")) slot = &p.idle_minutes;
        else if (sc_eq(argv[2], "maxage")) slot = &p.max_age_days;
        else if (sc_eq(argv[2], "history")) slot = &p.history;
        else if (sc_eq(argv[2], "iters")) slot = &p.iters;
        if (!slot) { ob(b, "Unknown policy key.\n"); return; }
        *slot = (uint32_t)v;
        if (!sc_policy_valid(&p)) { ob(b, "Value outside the allowed range.\n"); return; }
        S.policy = p;
        audit("policy-changed", argv[2]);
        db_save();
        ob(b, "Policy updated. Existing hashes keep their own work factor.\n");
    }
}

static void shell_audit(struct outbuf *b, int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && !sc_eq(argv[1], "clear"))) {
        ob(b, "Usage: audit [clear]\n");
        return;
    }
    if (argc == 2) {
        S.audit_head = 0; S.audit_count = 0;
        audit("audit-cleared", "by administrator");
        ob(b, "Audit ring cleared (the clear itself was recorded).\n");
        return;
    }
    {
        int count = S.audit_count;
        ob(b, "epoch       event                 uid   user              detail\n");
        if (count > SC_AUDIT_MAX) count = SC_AUDIT_MAX;
        for (int i = 0; i < count; i++) {
            int idx = (S.audit_head - count + i + SC_AUDIT_MAX * 2) % SC_AUDIT_MAX;
            ob(b, S.audit[idx]); ob(b, "\n");
        }
        if (!count) ob(b, "(empty)\n");
    }
}

static void shell_choices(struct outbuf *b, int argc, char **argv)
{
    char line[256];
    if (argc == 1 || sc_eq(argv[1], "show")) {
        ob(b, "Security choices - ALL are the owner's decision. Defaults keep the\n");
        ob(b, "important protections ON until you turn them off.\n");
        for (int i = 0; i < security_choice_count(); i++) {
            security_choice_describe(i, line, sizeof(line));
            ob(b, "  "); ob(b, line); ob(b, "\n");
        }
        ob(b, "\nPer-app isolation choices (never forced):\n");
        if (!security_app_isolation_count()) ob(b, "  (none yet - an app gets one on first launch)\n");
        for (int i = 0; i < security_app_isolation_count(); i++) {
            security_app_isolation_describe(i, line, sizeof(line));
            ob(b, "  "); ob(b, line); ob(b, "\n");
        }
        ob(b, "\nUsage: choices set <key> on|off\n"
              "       choices app <id> always|never|ask\n"
              "       choices default   (restore all defaults)\n");
        return;
    }
    if (!security_is_admin()) { ob(b, "Administrator rights required to change choices.\n"); return; }
    if (sc_eq(argv[1], "default") && argc == 2) {
        security_choices_default();
        security_choices_save();
        audit("choices-default", "all choices reset");
        ob(b, "All security choices restored to their defaults.\n");
        return;
    }
    if (sc_eq(argv[1], "set") && argc == 4) {
        int on = sc_eq(argv[3], "on") || sc_eq(argv[3], "1") || sc_eq(argv[3], "yes");
        int off = sc_eq(argv[3], "off") || sc_eq(argv[3], "0") || sc_eq(argv[3], "no");
        if ((!on && !off) || security_choice(argv[2]) < 0) {
            ob(b, "Unknown key or value. Run 'choices' for the list.\n"); return;
        }
        if (!security_choice_set(argv[2], on)) { ob(b, security_choice_error()); ob(b, "\n"); return; }
        audit("choice-changed", argv[2]);
        ob(b, "Choice updated: "); ob(b, argv[2]); ob(b, " = ");
        ob(b, on ? "on" : "off");
        if (sc_eq(argv[2], "vault.encryption") && on && !vault_unlocked() && vault_disk_present())
            ob(b, "  (sign out and back in to create the new volume key)");
        if (sc_eq(argv[2], "login.required") && !on && vault_disk_present() &&
            security_choice("vault.encryption"))
            ob(b, "  (an encrypted vault still needs a password at boot, so a "
                  "sign-in prompt will still appear until vault.encryption is off)");
        ob(b, "\n");
        return;
    }
    if (sc_eq(argv[1], "app") && argc == 4) {
        int mode = sc_eq(argv[3], "always") ? SC_ISO_ALWAYS :
                   sc_eq(argv[3], "never") ? SC_ISO_NEVER :
                   sc_eq(argv[3], "ask") ? SC_ISO_ASK : -1;
        if (mode < 0 || !security_app_isolation_set(argv[2], mode)) {
            ob(b, security_choice_error()); ob(b, "\n"); return;
        }
        audit("isolation-choice", argv[2]);
        ob(b, "Isolation choice saved for "); ob(b, argv[2]); ob(b, ".\n");
        return;
    }
    ob(b, "Usage: choices [show] | choices set <key> on|off | "
          "choices app <id> always|never|ask | choices default\n");
}

int security_shell(const char *line, char *out, size_t cap)
{
    char buf[512], *args[8];
    int argc = 0;
    char *p;
    struct outbuf b;
    if (!line || !out || cap < 16) return 0;
    out[0] = 0;
    b.p = out; b.end = out + cap;
    sc_copy(buf, sizeof(buf), line);
    p = buf;
    while (*p && argc < 8) {
        while (*p == ' ') p++;
        if (!*p) break;
        args[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    if (!argc) return 0;
    if (sc_eq(args[0], "choices")) {
        shell_choices(&b, argc, args);
        return 1;
    }
    if (sc_eq(args[0], "whoami")) {
        ob(&b, security_api_session_name());
        ob(&b, " (uid ");
        ob_num(&b, S.session >= 0 ? S.users[S.session].uid : 0);
        ob(&b, ")");
        return 1;
    }
    if (sc_eq(args[0], "id")) {
        ob(&b, "uid=");
        ob_num(&b, S.session >= 0 ? S.users[S.session].uid : 0);
        ob(&b, "("); ob(&b, security_api_session_name()); ob(&b, ") role=");
        ob(&b, S.session < 0 ? "none" :
           security_choice("roles.enabled") ? role_name(S.users[S.session].role) :
           "administrator (all accounts equal)");
        ob(&b, " session=epoch "); ob_num(&b, S.session_epoch);
        return 1;
    }
    if (sc_eq(args[0], "security")) { shell_status(&b); return 1; }
    if (sc_eq(args[0], "users")) {
        if (!security_is_admin()) { ob(&b, "Administrator rights required."); return 1; }
        shell_users(&b);
        return 1;
    }
    if (sc_eq(args[0], "policy")) {
        if (!security_is_admin()) { ob(&b, "Administrator rights required."); return 1; }
        shell_policy(&b, argc, args);
        return 1;
    }
    if (sc_eq(args[0], "audit")) {
        if (!security_is_admin()) { ob(&b, "Administrator rights required."); return 1; }
        shell_audit(&b, argc, args);
        return 1;
    }
    if (sc_eq(args[0], "lock")) {
        if (!queue(PEND_LOCK, 0, S.session, 0)) { ob(&b, "Another secure dialog is pending."); return 1; }
        audit("lock-requested", security_api_session_name());
        ob(&b, "Locking the session on the secure screen...");
        return 1;
    }
    if (sc_eq(args[0], "logout")) {
        if (!queue(PEND_LOGOUT, 0, S.session, 0)) { ob(&b, "Another secure dialog is pending."); return 1; }
        ob(&b, "Signing out; open windows will be closed...");
        return 1;
    }
    if (sc_eq(args[0], "passwd")) {
        if (argc != 1) { ob(&b, "Usage: passwd  (use 'usermod <name> --reset' to reset someone else)"); return 1; }
        if (!queue(PEND_PASSWD, 0, S.session, 0)) { ob(&b, "Another secure dialog is pending."); return 1; }
        ob(&b, "Password change continues on the secure screen...");
        return 1;
    }
    if (sc_eq(args[0], "useradd")) {
        /* Same permission as the base account by default.  Lower-power roles
         * are still available, explicitly, as the owner's choice. */
        int role = SC_ROLE_ADMIN;
        if (argc < 2 || argc > 3 || !sc_username_valid(args[1])) {
            ob(&b, "Usage: useradd <name> [admin|user|guest]  (default: admin)"); return 1;
        }
        if (!security_is_admin()) { ob(&b, "Administrator rights required."); return 1; }
        if (argc == 3) {
            if (sc_eq(args[2], "admin")) role = SC_ROLE_ADMIN;
            else if (sc_eq(args[2], "user")) role = SC_ROLE_USER;
            else if (sc_eq(args[2], "guest")) role = SC_ROLE_GUEST;
            else { ob(&b, "Unknown role: use admin, user or guest."); return 1; }
        }
        if (!queue(PEND_USERADD, args[1], S.session, role)) { ob(&b, "Another secure dialog is pending."); return 1; }
        ob(&b, "New account password continues on the secure screen...");
        return 1;
    }
    if (sc_eq(args[0], "userdel")) {
        if (argc != 3 || !sc_eq(args[2], "--confirm")) {
            ob(&b, "Usage: userdel <name> --confirm"); return 1;
        }
        if (!security_is_admin()) { ob(&b, "Administrator rights required."); return 1; }
        if (!queue(PEND_USERDEL, args[1], S.session, 0)) { ob(&b, "Another secure dialog is pending."); return 1; }
        ob(&b, "Administrator verification continues on the secure screen...");
        return 1;
    }
    if (sc_eq(args[0], "usermod")) {
        if (argc != 3) {
            ob(&b, "Usage: usermod <name> --admin|--user|--guest|--lock|--unlock|--reset");
            return 1;
        }
        if (!security_is_admin()) { ob(&b, "Administrator rights required."); return 1; }
        if (sc_eq(args[2], "--admin")) ob(&b, security_api_set_role(args[1], SC_ROLE_ADMIN) ? "Role changed." : "Could not change role (last administrator?).");
        else if (sc_eq(args[2], "--user")) ob(&b, security_api_set_role(args[1], SC_ROLE_USER) ? "Role changed." : "Could not change role.");
        else if (sc_eq(args[2], "--guest")) ob(&b, security_api_set_role(args[1], SC_ROLE_GUEST) ? "Role changed." : "Could not change role.");
        else if (sc_eq(args[2], "--lock")) ob(&b, security_api_set_disabled(args[1], 1) ? "Account disabled." : "Could not disable account.");
        else if (sc_eq(args[2], "--unlock")) ob(&b, security_api_set_disabled(args[1], 0) ? "Account enabled." : "Could not enable account.");
        else if (sc_eq(args[2], "--reset")) security_api_reset_user(args[1], out, cap);
        else ob(&b, "Unknown usermod option.");
        return 1;
    }
    if (sc_eq(args[0], "su")) {
        if (argc != 2) { ob(&b, "Usage: su <name>"); return 1; }
        if (find_user(args[1]) < 0) { ob(&b, "No such user."); return 1; }
        if (!queue(PEND_SU, args[1], S.session, 0)) { ob(&b, "Another secure dialog is pending."); return 1; }
        ob(&b, "Enter the target account's password on the secure screen...");
        return 1;
    }
    if (sc_eq(args[0], "reauth")) {
        if (!queue(PEND_REAUTH, 0, S.session, 0)) { ob(&b, "Another secure dialog is pending."); return 1; }
        ob(&b, "Re-authentication continues on the secure screen...");
        return 1;
    }
    if (sc_eq(args[0], "recovery")) {
        if (!security_is_admin()) { ob(&b, "Administrator rights required."); return 1; }
        if (!queue(PEND_RECOVERY, 0, S.session, 0)) { ob(&b, "Another secure dialog is pending."); return 1; }
        ob(&b, "Generating a one-time recovery code on the secure screen...");
        return 1;
    }
    return 0;
}

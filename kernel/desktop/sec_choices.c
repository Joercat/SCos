/* SCos security choices: every protection that could restrict the owner of
 * the machine is a persisted, user-controlled switch.
 *
 * Design rule from the project owner: important things are ON by default but
 * can be turned OFF; nothing is forced.  No app is shipped to edit these —
 * `choices` in either shell and the API in security.h are the interface, so a
 * proper Settings app can be written later without touching the kernel.
 *
 * The file is `system/security/choices.cfg`, a checksummed line format.  Its
 * bytes are also copied into the encrypted vault header, which is the AEAD's
 * associated data: an offline edit is detected when the volume key is
 * unwrapped.  On RAM-only machines the VFS copy is session-only, like accounts.
 */
#include "scos.h"
#include "security.h"

#define CHOICE_MAGIC "SCos-choices-v1"

struct choice_def { const char *key; int def; const char *help; };

/* Keep this table in one place: the shell listing, the persisted file and the
 * app-facing API all read it.  Adding a key here gives a new user choice. */
static const struct choice_def defs[] = {
    { "login.required",      1, "require a password before the desktop is usable" },
    { "lock.enabled",        1, "allow locking the session (console lock screen)" },
    { "lock.idle",           1, "lock automatically after the idle timeout" },
    { "password.policy",     1, "enforce the strong password policy" },
    { "password.history",    1, "reject recently used passwords" },
    { "password.age",        1, "force a change when a password expires" },
    { "lockout",             1, "delay repeated failed sign-ins" },
    { "guest.restrict",      1, "limit guest accounts to read-only commands" },
    { "audit.enabled",       1, "keep the security audit log" },
    { "vault.encryption",    1, "encrypt the saved filesystem image" },
    { "recovery.enabled",    1, "allow one-time recovery codes" },
    { "isolation.prompt",    1, "ask how to launch an app the first time" },
    { "isolation.default",   0, "isolate apps by default when no choice was made" },
    { "confirm.destructive", 1, "ask before destructive shell commands" },
};
#define NDEFS ((int)(sizeof(defs) / sizeof(defs[0])))

struct app_entry { char id[SC_APP_ID_MAX]; int mode; int last; };
static int vals[NDEFS];
static struct app_entry apps[SC_APP_CHOICES_MAX];
static int iso_count;
static char last_error[SC_MSG_MAX];
static char cfg[SC_VAULT_CHOICES_MAX];

/* ------------------------------------------------------------- small helpers --- */
static int put(char *p, size_t cap, size_t *at, const char *s)
{
    size_t n = sc_len(s);
    if (*at + n + 1 > cap) return 0;
    memcpy(p + *at, s, n);
    *at += n;
    p[*at] = 0;
    return 1;
}
static void put_num(char *p, size_t cap, size_t *at, int v)
{
    char n[16];
    sc_fmt(n, sizeof(n), (uint64_t)(v < 0 ? 0 : v));
    put(p, cap, at, n);
}
static int def_of(const char *key)
{
    if (!key) return -1;
    for (int i = 0; i < NDEFS; i++) if (sc_eq(defs[i].key, key)) return i;
    return -1;
}
static int app_of(const char *id)
{
    if (!id) return -1;
    for (int i = 0; i < iso_count; i++) if (sc_eq(apps[i].id, id)) return i;
    return -1;
}
static int app_id_valid(const char *id)
{
    size_t n;
    if (!id || !(n = sc_len(id)) || n >= SC_APP_ID_MAX || id[0] == '_') return 0;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == '_')) return 0;
    }
    return 1;
}
static void set_error(const char *s) { sc_copy(last_error, sizeof(last_error), s); }

const char *security_choice_error(void) { return last_error; }

void security_choices_default(void)
{
    for (int i = 0; i < NDEFS; i++) vals[i] = defs[i].def;
    memset(apps, 0, sizeof(apps));
    iso_count = 0;
    last_error[0] = 0;
}

int security_choice(const char *key)
{
    int i = def_of(key);
    return i < 0 ? -1 : vals[i];
}

int security_choice_count(void) { return NDEFS; }

void security_choice_describe(int i, char *out, size_t cap)
{
    if (i < 0 || i >= NDEFS) { sc_copy(out, cap, ""); return; }
    sc_copy(out, cap, defs[i].key);
    sc_cat(out, cap, " = ");
    sc_cat(out, cap, vals[i] ? "on" : "off");
    sc_cat(out, cap, "  (default ");
    sc_cat(out, cap, defs[i].def ? "on" : "off");
    sc_cat(out, cap, ") - ");
    sc_cat(out, cap, defs[i].help);
}

int security_choice_set(const char *key, int value)
{
    int i = def_of(key);
    if (i < 0) { set_error("Unknown choice key. Run 'choices' to list them."); return 0; }
    vals[i] = value ? 1 : 0;
    if (!security_choices_save()) return 0;
    return 1;
}

/* ------------------------------------------------------------------ codec --- */
static int encode(void)
{
    size_t at = 0;
    char digest[65];
    if (!put(cfg, sizeof(cfg), &at, CHOICE_MAGIC "\n")) return 0;
    for (int i = 0; i < NDEFS; i++) {
        if (!put(cfg, sizeof(cfg), &at, "K|") ||
            !put(cfg, sizeof(cfg), &at, defs[i].key) ||
            !put(cfg, sizeof(cfg), &at, "|")) return 0;
        put_num(cfg, sizeof(cfg), &at, vals[i]);
        if (!put(cfg, sizeof(cfg), &at, "\n")) return 0;
    }
    for (int i = 0; i < iso_count; i++) {
        if (!put(cfg, sizeof(cfg), &at, "A|") ||
            !put(cfg, sizeof(cfg), &at, apps[i].id) || !put(cfg, sizeof(cfg), &at, "|")) return 0;
        put_num(cfg, sizeof(cfg), &at, apps[i].mode);
        if (!put(cfg, sizeof(cfg), &at, "|")) return 0;
        put_num(cfg, sizeof(cfg), &at, apps[i].last);
        if (!put(cfg, sizeof(cfg), &at, "\n")) return 0;
    }
    sc_db_digest(cfg, at, digest);
    if (!put(cfg, sizeof(cfg), &at, "C|") ||
        !put(cfg, sizeof(cfg), &at, digest) ||
        !put(cfg, sizeof(cfg), &at, "\n")) return 0;
    return (int)at;
}

static int parse(const char *data, size_t len)
{
    char work[SC_VAULT_CHOICES_MAX], *p, *nl;
    int seen[NDEFS];
    int nvals[NDEFS];
    struct app_entry napp[SC_APP_CHOICES_MAX];
    int napps = 0;
    if (!data || len < 20 || len >= sizeof(work) || !sc_db_verify(data, len)) return 0;
    memcpy(work, data, len);
    work[len] = 0;
    for (int i = 0; i < NDEFS; i++) { seen[i] = 0; nvals[i] = defs[i].def; }
    memset(napp, 0, sizeof(napp));
    p = work;
    while (p && *p) {
        nl = str_chr(p, '\n');
        if (nl) *nl = 0;
        if (!sc_neq(p, CHOICE_MAGIC, sc_len(CHOICE_MAGIC))) { /* magic line */ }
        else if (!sc_neq(p, "K|", 2)) {
            char *bar = str_chr(p + 2, '|');
            int idx = -1;
            uint64_t v = 0;
            if (!bar) return 0;
            *bar = 0;
            idx = def_of(p + 2);
            if (idx < 0 || !sc_num(bar + 1, &v) || v > 1 || seen[idx]) return 0;
            seen[idx] = 1; nvals[idx] = (int)v;
        } else if (!sc_neq(p, "A|", 2)) {
            char *bar = str_chr(p + 2, '|');
            char *bar2;
            uint64_t mode = 0, last = 0;
            if (!bar || napps >= SC_APP_CHOICES_MAX) return 0;
            *bar = 0;
            bar2 = str_chr(bar + 1, '|');
            if (!bar2) return 0;
            *bar2 = 0;
            if (!app_id_valid(p + 2) || !sc_num(bar + 1, &mode) || mode > SC_ISO_NEVER ||
                !sc_num(bar2 + 1, &last) || last > 1) return 0;
            for (int i = 0; i < napps; i++) if (sc_eq(napp[i].id, p + 2)) return 0;
            sc_copy(napp[napps].id, sizeof(napp[0].id), p + 2);
            napp[napps].mode = (int)mode;
            napp[napps].last = (int)last;
            napps++;
        } else if (!sc_neq(p, "C|", 2)) break;
        else return 0;
        p = nl ? nl + 1 : 0;
    }
    /* Unknown/new keys keep their default, so adding a choice does not
     * invalidate an existing file. */
    for (int i = 0; i < NDEFS; i++) vals[i] = nvals[i];
    memcpy(apps, napp, sizeof(apps));
    iso_count = napps;
    return 1;
}

/* -------------------------------------------------------------- persistence --- */
int security_choices_save(void)
{
    int n = encode();
    if (n <= 0) { set_error("Choice file does not fit in its bounded slot."); return 0; }
    security_bypass_scope(1);
    if (!vfs_lookup("system/security")) vfs_mkdir("system/security");
    vfs_write(SC_CHOICES_PATH, cfg, (u32)n);
    security_bypass_scope(0);
    vault_set_choices(cfg, (uint32_t)n);
    security_mark_dirty();
    return 1;
}

int security_choices_load(void)
{
    u32 len = 0;
    char *data;
    security_choices_default();
    security_bypass_scope(1);
    data = vfs_read(SC_CHOICES_PATH, &len);
    security_bypass_scope(0);
    if (!data || !len || len >= SC_VAULT_CHOICES_MAX) { set_error("No saved choices; defaults active."); return 0; }
    if (!parse(data, len)) { set_error("Saved choices rejected; defaults active."); return 0; }
    vault_set_choices(cfg, (uint32_t)encode());
    return 1;
}

/* --------------------------------------------------- per-app isolation choice --- */
int security_app_isolation(const char *app_id)
{
    int i = app_of(app_id);
    return i < 0 ? SC_ISO_ASK : apps[i].mode;
}

int security_app_isolation_set(const char *app_id, int mode)
{
    int i;
    if (!app_id_valid(app_id) || mode < SC_ISO_ASK || mode > SC_ISO_NEVER) {
        set_error("Usage: choices app <id> always|never|ask");
        return 0;
    }
    i = app_of(app_id);
    if (i < 0) {
        if (iso_count >= SC_APP_CHOICES_MAX) { set_error("Per-app choice table is full."); return 0; }
        i = iso_count++;
        memset(&apps[i], 0, sizeof(apps[i]));
        sc_copy(apps[i].id, sizeof(apps[i].id), app_id);
    }
    apps[i].mode = mode;
    return security_choices_save();
}

int security_app_isolation_pending(const char *app_id)
{
    return security_choice("isolation.prompt") &&
           security_app_isolation(app_id) == SC_ISO_ASK;
}

int security_app_launch_isolated(const char *app_id, int isolated)
{
    int i = app_of(app_id);
    if (i < 0) {
        if (!app_id_valid(app_id) || iso_count >= SC_APP_CHOICES_MAX)
            return security_choice("isolation.default");
        i = iso_count++;
        memset(&apps[i], 0, sizeof(apps[i]));
        sc_copy(apps[i].id, sizeof(apps[i].id), app_id);
    }
    apps[i].last = isolated ? 1 : 0;
    security_choices_save();
    return apps[i].last;
}

int security_app_last_isolated(const char *app_id)
{
    int i = app_of(app_id);
    return i < 0 ? security_choice("isolation.default") : apps[i].last;
}

int security_app_isolation_count(void) { return iso_count; }

void security_app_isolation_describe(int i, char *out, size_t cap)
{
    if (i < 0 || i >= iso_count) { sc_copy(out, cap, ""); return; }
    sc_copy(out, cap, apps[i].id);
    sc_cat(out, cap, " = ");
    sc_cat(out, cap, apps[i].mode == SC_ISO_ALWAYS ? "always isolated" :
           apps[i].mode == SC_ISO_NEVER ? "never isolated" : "ask each first launch");
}

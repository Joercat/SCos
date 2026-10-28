/* Per-app private storage.
 *
 * One rule, enforced in one place: an application's data lives in exactly
 * `home/appdata/<account>/<its-id>/` and nowhere else.  Every entry point that names a
 * data file goes through this module, so the validation is not repeated (and
 * cannot drift) between the Lua API, the VFS and the uninstaller.
 *
 * This namespace is ALWAYS private, whether or not the owner chose to run the
 * app isolated.  Isolation is a memory/CPU/capability boundary the owner may
 * decline; storage separation is not optional, because losing a document to a
 * neighbouring app is not a preference.  The VFS calls app_storage_allowed()
 * while an app process is current, so even a native app that calls vfs_write()
 * directly cannot reach another app's data.
 *
 * Uninstalling an app wipes this directory.  Reinstalling therefore starts
 * clean: a stale save from the previous installation cannot resurface.
 */
#include "scos.h"
#include "process.h"

#define APPDATA_ROOT "home/appdata"

static int seg_ok(size_t n)
{
    return n > 0 && n < VFS_NAME;
}

/* Kernel-side maintenance window.  The storage layer itself has to create and
 * remove `home/appdata/<account>/`, which the account-scoped VFS guard refuses
 * to app code and to the shell by design.  Only this module raises the flag,
 * and only for the duration of one directory operation. */
static int maint_active;
int app_storage_maintenance(void) { return maint_active; }

/* The account segment.  Accounts are 3..31 ASCII [a-z0-9._-] starting with an
 * alphanumeric, so this is exactly as strict as the account itself: no '/', no
 * '.', no '..', no empty segment.  Whoever is signed in decides the directory,
 * so two accounts running the same package never share a file. */
static int account_ok(const char *name)
{
    size_t n;
    if (!name) return 0;
    n = strlen(name);
    if (!seg_ok(n) || n > 31) return 0;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') continue;
        return 0;
    }
    return 1;
}

/* The signed-in account, or the fixed fallback used before sign-in.  Apps run
 * only after sign-in, so the fallback is only reachable from kernel code. */
static int account_segment(char *out, size_t cap)
{
    const char *me = security_current_user();
    if (!account_ok(me)) me = "nobody";
    if (strlen(me) + 1 > cap) return 0;
    strcpy(out, me);
    return 1;
}

int app_storage_dir_for(const char *account, const char *id, char *out, size_t cap)
{
    char buf[96];
    if (!out || !cap || !account_ok(account) || !app_storage_id_valid(id)) return 0;
    strcpy(buf, APPDATA_ROOT);
    strcat(buf, "/");
    strcat(buf, account);
    strcat(buf, "/");
    strcat(buf, id);
    if (strlen(buf) + 1 > cap) return 0;
    strcpy(out, buf);
    return 1;
}

int app_storage_id_valid(const char *id)
{
    size_t n;
    if (!id) return 0;
    n = strlen(id);
    if (!seg_ok(n) || n > 30) return 0;
    if (!strcmp(id, ".") || !strcmp(id, "..")) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') continue;
        return 0;                       /* no '.', '/' or anything exotic */
    }
    return 1;
}

int app_storage_name_valid(const char *name)
{
    size_t n;
    if (!name) return 0;
    n = strlen(name);
    if (!seg_ok(n)) return 0;
    if (name[0] == '.') return 0;       /* no hidden files, no "." / ".." */
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') continue;
        return 0;
    }
    return 1;
}

int app_storage_dir(const char *id, char *out, size_t cap)
{
    char account[SC_NAME_MAX];
    if (!account_segment(account, sizeof(account))) return 0;
    return app_storage_dir_for(account, id, out, cap);
}

int app_storage_path(const char *id, const char *name, char *out, size_t cap)
{
    char dir[96];
    if (!app_storage_dir(id, dir, sizeof(dir)) || !app_storage_name_valid(name)) return 0;
    if (strlen(dir) + 1 + strlen(name) + 1 > cap) return 0;
    strcpy(out, dir);
    strcat(out, "/");
    strcat(out, name);
    return 1;
}

int app_storage_ensure_for(const char *account, const char *id)
{
    char dir[96], owner[96];
    struct vfs_node *n;
    if (!app_storage_dir_for(account, id, dir, sizeof(dir))) return 0;
    maint_active = 1;
    if (!vfs_lookup(APPDATA_ROOT)) vfs_mkdir(APPDATA_ROOT);
    strcpy(owner, APPDATA_ROOT);
    strcat(owner, "/");
    strcat(owner, account);
    if (!vfs_lookup(owner)) vfs_mkdir(owner);
    if (!vfs_lookup(dir)) vfs_mkdir(dir);
    maint_active = 0;
    n = vfs_lookup(dir);
    return n && n->is_dir;
}

int app_storage_ensure(const char *id)
{
    char account[SC_NAME_MAX];
    if (!account_segment(account, sizeof(account))) return 0;
    return app_storage_ensure_for(account, id);
}

static int dir_used(const char *dir, int *files, u32 *bytes)
{
    struct vfs_node *d, *f;
    int count = 0;
    u32 total = 0;
    if (files) *files = 0;
    if (bytes) *bytes = 0;
    if (!dir) return 0;
    d = vfs_lookup(dir);
    if (!d || !d->is_dir) return 1;
    for (f = d->child; f; f = f->sibling) {
        if (f->is_dir) continue;
        count++;
        total += f->size;
    }
    if (files) *files = count;
    if (bytes) *bytes = total;
    return 1;
}

int app_storage_used(const char *id, int *files, u32 *bytes)
{
    char dir[96];
    if (!app_storage_dir(id, dir, sizeof(dir))) {
        if (files) *files = 0;
        if (bytes) *bytes = 0;
        return 0;
    }
    return dir_used(dir, files, bytes);
}

/* May `name` grow to `want` bytes without breaking the per-app quota?  The
 * quota is enforced here rather than only in the Lua wrapper so a native app
 * gets the same limit. */
int app_storage_check(const char *id, const char *name, u32 want)
{
    char dir[96], path[192];
    struct vfs_node *old;
    int files;
    u32 bytes;
    if (!app_storage_dir(id, dir, sizeof(dir)) || !app_storage_name_valid(name)) return 0;
    if (!app_storage_path(id, name, path, sizeof(path))) return 0;
    old = vfs_lookup(path);
    if (old && old->is_dir) return 0;
    if (!app_storage_used(id, &files, &bytes)) return 0;
    if ((!old && files >= APP_STORAGE_MAX_FILES) ||
        bytes - (old ? old->size : 0) + want > APP_STORAGE_MAX_BYTES)
        return 0;
    return 1;
}

/* Kernel-side maintenance window.  Uninstalling is a kernel operation, but it
 * is reached from a shell/app callback that is running as an app process; the
 * boundary check below would then refuse to delete the private directory of a
 * *different* id.  Only app_storage_wipe() - never app code - raises this. */
static int wipe_active;

/* Delete exactly one private directory.  The boundary check is suspended only
 * for the duration of the call, and only this module can raise it. */
static int wipe_dir(const char *dir)
{
    int gone;
    if (!dir) return 0;
    maint_active = 1;
    if (!vfs_lookup(dir)) { maint_active = 0; return 1; }  /* goal is the state */
    wipe_active = 1;
    gone = vfs_delete(dir);
    wipe_active = 0;
    maint_active = 0;
    return gone;
}

int app_storage_wipe(const char *id, int *files_out, u32 *bytes_out)
{
    char dir[96];
    int files = 0;
    u32 bytes = 0;
    if (files_out) *files_out = 0;
    if (bytes_out) *bytes_out = 0;
    if (!app_storage_dir(id, dir, sizeof(dir))) return 0;
    dir_used(dir, &files, &bytes);
    if (files_out) *files_out = files;
    if (bytes_out) *bytes_out = bytes;
    return wipe_dir(dir);
}

/* Uninstalling is not a per-account act: the package is registered for the
 * machine, so every account's copy of its private data goes.  The legacy
 * `home/appdata/<id>/` directory from earlier images is erased as well - a
 * reinstall must not be able to read data that predates this layout. */
int app_storage_wipe_all(const char *id, int *files_out, u32 *bytes_out)
{
    char dir[96], account[SC_NAME_MAX];
    int files = 0, ok = 1, i, n;
    u32 bytes = 0;
    if (files_out) *files_out = 0;
    if (bytes_out) *bytes_out = 0;
    if (!app_storage_id_valid(id)) return 0;
    strcpy(dir, APPDATA_ROOT);
    strcat(dir, "/");
    strcat(dir, id);
    {
        int f = 0; u32 b = 0;
        dir_used(dir, &f, &b);
        files += f; bytes += b;
        if (!wipe_dir(dir)) ok = 0;
    }
    n = security_api_user_count();
    for (i = -1; i < n; i++) {          /* -1 = the before-sign-in segment */
        if (i < 0) {
            if (!security_initialized()) continue;
            strcpy(account, "nobody");
        } else if (!security_api_user_name(i, account, sizeof(account))) {
            continue;
        }
        if (!app_storage_dir_for(account, id, dir, sizeof(dir))) continue;
        {
            int f = 0; u32 b = 0;
            dir_used(dir, &f, &b);
            files += f; bytes += b;
            if (!wipe_dir(dir)) ok = 0;
        }
    }
    if (files_out) *files_out = files;
    if (bytes_out) *bytes_out = bytes;
    return ok;
}

/* Resolve "." and ".." without touching the filesystem, so the answer is about
 * the path the VFS would actually reach.  Returns 0 when the path is not a
 * plain relative path, is over-long, or climbs above the root. */
static int normalise(const char *path, char *out, size_t cap)
{
    char work[256];
    size_t i = 0, ol = 0, l;
    if (!path) return 0;
    while (*path == '/') path++;
    if (!*path || strlen(path) >= sizeof(work)) return 0;
    strcpy(work, path);
    l = strlen(work);
    while (l && work[l - 1] == '/') work[--l] = 0;
    if (!l || !cap) return 0;
    out[0] = 0;
    while (work[i]) {
        const char *seg = work + i;
        const char *slash = seg;
        size_t n;
        while (*slash && *slash != '/') slash++;
        n = (size_t)(slash - seg);
        if (!n || (n == 1 && seg[0] == '.')) {
            /* skip an empty or current-directory segment */
        } else if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            if (!ol) return 0;              /* climbs above the root */
            while (ol && out[ol - 1] != '/') ol--;
            if (ol) ol--;
            out[ol] = 0;
        } else {
            if (ol && ol + 2 > cap) return 0;
            if (ol) out[ol++] = '/';
            if (ol + n + 1 > cap) return 0;
            memcpy(out + ol, seg, n);
            ol += n;
            out[ol] = 0;
        }
        i += n;
        if (work[i] == '/') i++;
    }
    return ol > 0;
}

int app_storage_owns_path(const char *id, const char *path)
{
    char want[96], norm[256];
    size_t n;
    if (!app_storage_dir(id, want, sizeof(want))) return 0;
    if (!normalise(path, norm, sizeof(norm))) return 0;
    n = strlen(want);
    if (!strcmp(norm, want)) return 1;          /* the directory itself */
    if (strncmp(norm, want, n) || norm[n] != '/') return 0;
    /* Private storage is flat: exactly one filename, never a nested path. */
    return app_storage_name_valid(norm + n + 1);
}

/* Called at the VFS boundary for every read/write while an app process is
 * current.  Paths outside the shared appdata root are not its business. */
int app_storage_allowed(const char *path)
{
    char norm[256];
    const char *id;
    if (!path) return 1;
    /* Both maintenance windows are kernel-only and last one operation: the
     * storage layer must be able to create the account root and the app's
     * directory while an app process is the current one. */
    if (wipe_active || maint_active) return 1;
    if (!proc_current()) return 1;
    id = proc_app_id();
    if (!id || !*id) return 1;
    if (!normalise(path, norm, sizeof(norm)))
        return strncmp(path, APPDATA_ROOT, 12) != 0;
    if (strncmp(norm, APPDATA_ROOT, 12) != 0 ||
        (norm[12] && norm[12] != '/')) return 1;
    if (!strcmp(norm, APPDATA_ROOT)) return 1;
    /* home/appdata/<account>/<id>/... and nothing else: the account segment is
     * part of the owned path, so another account's copy is refused even for
     * the same package id, and the older flat home/appdata/<id>/ is refused
     * too. */
    return app_storage_owns_path(id, norm);
}

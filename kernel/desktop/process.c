/* SCos process isolation engine.
 *
 * This is the piece that used to be "a flag on a window".  Now an isolated app
 * really is a process:
 *
 *   - it has its own PML4 (memory_domain_create/switch in kernel/src/memory.c);
 *   - its private arena pages are removed from the shared identity map, so no
 *     other address space - and no physical-address poke - can reach them;
 *   - a kernel-held capability set gates VFS paths, global theme changes,
 *     notifications, dialogs and app-registration edits;
 *   - the WM switches both CR3 and the capability context around EVERY app
 *     callback, so native C apps and Lua CAT packages are treated identically.
 *
 * Isolation is still the owner's choice: proc_create() only builds a domain
 * when w->isolated is set.  `choices set isolation.default on` changes the
 * default answer, and the first-launch prompt is the per-app override.
 */
#include "scos.h"
#include "process.h"
#include "security.h"
#include "usermode.h"

int vfs_access(const char *path, int write);   /* kernel/desktop/vfs.c */

struct process {
    int         used;
    struct window *w;
    char        app_id[SC_APP_ID_MAX];
    char        owner[SC_NAME_MAX];
    int         isolated;
    uint32_t    caps;
    int         domain;                 /* memory-domain pid, 0 when shared */
    int         arena_pages;
};
static struct process P[PROC_MAX];
static int cur;                          /* index+1, 0 = kernel */
static int depth;
struct saved_state { int proc, domain; };
static struct saved_state saved[8];

void proc_init(void) { memset(&P, 0, sizeof(P)); cur = 0; depth = 0; usermode_init(); }

static int index_of(struct window *w)
{
    for (int i = 0; i < PROC_MAX; i++) if (P[i].used && P[i].w == w) return i;
    return -1;
}

static struct process *active(void) { return cur ? &P[cur - 1] : 0; }

int proc_pid_for_window(struct window *w)
{
    int i = index_of(w);
    return i < 0 ? 0 : i + 1;
}

int proc_create(struct window *w)
{
    int i;
    if (!w) return 0;
    if (index_of(w) >= 0) return 0;
    for (i = 0; i < PROC_MAX; i++) if (!P[i].used) break;
    if (i == PROC_MAX) return 0;
    memset(&P[i], 0, sizeof(P[i]));
    P[i].used = 1;
    P[i].w = w;
    sc_copy(P[i].app_id, sizeof(P[i].app_id), w->app && w->app->id ? w->app->id : "app");
    sc_copy(P[i].owner, sizeof(P[i].owner), security_current_user());
    P[i].isolated = w->isolated ? 1 : 0;
    /* Isolated apps may read/write ONLY their own namespace; the VFS boundary
     * enforces the namespace.  Global theme/IPC/app-registration edits are
     * withheld. */
    P[i].caps = P[i].isolated ? (PROC_CAP_VFS_READ | PROC_CAP_VFS_WRITE |
                                 PROC_CAP_SYSINFO)
                              : PROC_CAP_ALL;
    if (P[i].isolated) {
        P[i].domain = memory_domain_create();
        /* Every isolated process gets real private pages from the start, even
         * when it is a native C app that never calls proc_private_alloc().
         * Page 0 carries a marker so the domain can be verified at runtime. */
        if (P[i].domain && memory_domain_map(P[i].domain, 16)) {
            P[i].arena_pages = 16;
            volatile uint64_t *mark = (volatile uint64_t *)proc_arena_va(i + 1);
            *mark = UINT64_C(0x53434f5350524f43) + (uint64_t)(i + 1);
        }
        user_attach(i + 1, w);
    }
    return i + 1;
}

void proc_destroy(struct window *w)
{
    int i = index_of(w);
    if (i < 0) return;
    if (cur == i + 1) proc_deactivate();
    user_detach(i + 1);
    if (P[i].domain) memory_domain_destroy(P[i].domain);
    memset(&P[i], 0, sizeof(P[i]));
}

int proc_activate(int pid)
{
    if (pid < 0 || pid > PROC_MAX) return 0;
    if (pid && !P[pid - 1].used) return 0;
    cur = pid;
    memory_domain_switch(pid ? P[pid - 1].domain : 0);
    return 1;
}
void proc_deactivate(void) { cur = 0; memory_domain_switch(0); }

int proc_enter(struct window *w)
{
    int token = depth, pid = proc_pid_for_window(w);
    int slots = (int)(sizeof(saved) / sizeof(saved[0]));
    if (token >= 0 && token < slots) {
        saved[token].proc = cur;
        saved[token].domain = cur ? P[cur - 1].domain : 0;
    }
    depth++;
    if (pid) proc_activate(pid);
    return token;
}

void proc_leave(int token)
{
    int slots = (int)(sizeof(saved) / sizeof(saved[0]));
    if (token < 0 || token >= depth) return;
    depth = token;
    if (token >= slots) { proc_deactivate(); return; }
    cur = saved[token].proc;
    memory_domain_switch(saved[token].domain);
}

int  proc_current(void) { return cur; }
int  proc_isolated(void) { struct process *p = active(); return p && p->isolated; }
const char *proc_app_id(void) { struct process *p = active(); return p ? p->app_id : ""; }
uint32_t proc_caps(void) { struct process *p = active(); return p ? p->caps : PROC_CAP_ALL; }
const char *proc_owner(int pid)
{
    return pid > 0 && pid <= PROC_MAX && P[pid - 1].used ? P[pid - 1].owner : "";
}
int proc_domain(int pid)
{
    return pid > 0 && pid <= PROC_MAX && P[pid - 1].used ? P[pid - 1].domain : 0;
}
const char *proc_pid_app_id(int pid)
{
    return pid > 0 && pid <= PROC_MAX && P[pid - 1].used ? P[pid - 1].app_id : "";
}
int proc_pid_isolated(int pid)
{
    return pid > 0 && pid <= PROC_MAX && P[pid - 1].used ? P[pid - 1].isolated : 0;
}
uint32_t proc_pid_caps(int pid)
{
    return pid > 0 && pid <= PROC_MAX && P[pid - 1].used ? P[pid - 1].caps : 0;
}
int proc_count(void)
{
    int n = 0;
    for (int i = 0; i < PROC_MAX; i++) if (P[i].used) n++;
    return n;
}
int proc_isolation_count(void)
{
    int n = 0;
    for (int i = 0; i < PROC_MAX; i++) if (P[i].used && P[i].isolated) n++;
    return n;
}

int proc_pid_cap_allowed(int pid, uint32_t cap)
{
    if (!pid) return 1;                     /* kernel context */
    if (pid < 1 || pid > PROC_MAX || !P[pid - 1].used) return 1;
    if (!P[pid - 1].isolated) return 1;     /* owner chose "never isolated" */
    return (P[pid - 1].caps & cap) != 0;
}
int proc_cap_allowed(uint32_t cap) { return proc_pid_cap_allowed(cur, cap); }

/* A private VFS namespace.  Everything outside the app's own appdata
 * directory (and the two parent directories needed to reach it) is absent for
 * an isolated process.  This is checked at the VFS boundary, so it applies to
 * native C apps as well as Lua packages. */
int proc_pid_path_allowed(int pid, const char *path, int write)
{
    char norm[256], own[64], prefix[64];
    const char *p, *rest, *q;
    size_t len, n, own_len;
    (void)write;
    if (!pid) return 1;
    if (pid < 1 || pid > PROC_MAX || !P[pid - 1].used) return 1;
    if (!P[pid - 1].isolated) return 1;
    if (!path) return 0;
    p = path;
    while (*p == '/') p++;
    /* vfs_parent_path() produces parents with a trailing slash; normalise it.
     * A trailing slash is harmless, but "." / ".." / empty segments are NOT:
     * vfs_lookup() honours "..", so allowing one would escape the namespace. */
    len = sc_len(p);
    while (len && p[len - 1] == '/') len--;
    if (!len || len >= sizeof(norm)) return 0;
    memcpy(norm, p, len); norm[len] = 0;
    for (q = norm; *q;) {
        const char *slash = q;
        while (*slash && *slash != '/') slash++;
        n = (size_t)(slash - q);
        if (!n || (n == 1 && q[0] == '.') ||
            (n == 2 && q[0] == '.' && q[1] == '.')) return 0;
        q = *slash ? slash + 1 : slash;
    }
    sc_copy(own, sizeof(own), "home/appdata/");
    sc_cat(own, sizeof(own), P[pid - 1].app_id);
    own_len = sc_len(own);
    sc_copy(prefix, sizeof(prefix), own);
    sc_cat(prefix, sizeof(prefix), "/");
    if (sc_eq(norm, "home") || sc_eq(norm, "home/appdata") || sc_eq(norm, own))
        return 1;
    /* The runtime loader may read its OWN package, and nothing else, from the
     * two package directories.  No subdirectories, no traversal. */
    static const char *const dirs[] = { "home/apps/", "home/demos/", 0 };
    for (int k = 0; dirs[k]; k++) {
        size_t dn = sc_len(dirs[k]);
        if (sc_neq(norm, dirs[k], dn)) continue;
        const char *f = norm + dn;
        size_t idlen = sc_len(P[pid - 1].app_id);
        if (!sc_neq(f, P[pid - 1].app_id, idlen) && sc_eq(f + idlen, ".cat"))
            return 1;
    }
    if (sc_neq(norm, prefix, own_len + 1)) return 0;
    rest = norm + own_len + 1;
    if (!*rest) return 1;
    /* A private root: exactly one filename, never a nested path. */
    for (q = rest; *q; q++) if (*q == '/') return 0;
    return 1;
}

int proc_path_allowed(const char *path, int write)
{
    return proc_pid_path_allowed(cur, path, write);
}

/* Private arena.  The virtual address is fixed per domain, so the Lua VM's
 * bump allocator sees one contiguous range; the physical pages are not
 * contiguous and are mapped only in that domain (plus the kernel root's
 * management view of the same slot). */
void *proc_private_alloc(size_t bytes)
{
    struct process *p = active();
    size_t pages;
    if (!p || !p->isolated || !p->domain || !bytes) return 0;
    pages = (bytes + 4095) / 4096;
    if (pages + (size_t)p->arena_pages > 1024) return 0;
    if (!memory_domain_map(p->domain, p->arena_pages + (int)pages)) return 0;
    void *base = (void *)(memory_domain_base() +
                  (uintptr_t)(p->domain - 1) * memory_domain_stride() +
                  (uintptr_t)p->arena_pages * 4096);
    p->arena_pages += (int)pages;
    return base;
}
uintptr_t proc_arena_va(int pid)
{
    int d = proc_domain(pid);
    return d ? memory_domain_base() + (uintptr_t)(d - 1) * memory_domain_stride() : 0;
}
int proc_pid_mapped(int pid, uintptr_t va)
{
    int d = proc_domain(pid);
    return d ? memory_domain_mapped(d, va) : 0;
}
uint64_t proc_kernel_root(void) { return (uint64_t)(uintptr_t)memory_kernel_root(); }
uint64_t proc_current_root(void) { return memory_current_root(); }

/* Read one word from a process's private address space.  This is a real CR3
 * switch, used by the WM/SysMon to inspect app-owned state and by the isolation
 * tests.  A page that is not present in that process's PML4 returns 0 instead
 * of faulting - which is exactly the observable difference between "isolated"
 * and "shared". */
/* Query a path as a given process would see it, without letting app code
 * choose its own pid.  Used by the shell and by the isolation tests. */
int proc_pid_access(int pid, const char *path, int write)
{
    int prev = cur, ok;
    if (pid < 0 || pid > PROC_MAX || (pid && !P[pid - 1].used)) return 0;
    cur = pid;
    ok = vfs_access(path, write);
    cur = prev;
    return ok;
}

int proc_pid_probe(int pid, uintptr_t va, uint64_t *out)
{
    int d = proc_domain(pid);
    if (!d || !out) return 0;
    if (!memory_domain_mapped(d, va)) return 0;
    memory_domain_switch(d);
    *out = *(volatile uint64_t *)va;
    memory_domain_switch(0);
    return 1;
}

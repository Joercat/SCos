/* SCos user-mode (CPL3) syscall dispatcher, namespaces and resource limits.
 *
 * The hardware boundary lives in kernel/x86_64/usermode.S + interrupts_init():
 * CPL3 code has its own PML4 in which the kernel's pages are supervisor-only.
 * This file is the ONLY door back into the kernel.  It implements the Linux/
 * Flatpak layers on top of that:
 *
 *   seccomp       - default-deny allowlist of syscall numbers
 *   mount ns      - an app can only name basenames inside its private root
 *   PID/IPC ns    - only its own window is visible; no global dialogs/notify
 *   user ns       - a capability set (theme / IPC / appctl withheld)
 *   cgroups       - memory, file count/bytes, syscall count and CPU time
 *
 * A pointer from CPL3 is NEVER trusted: it must lie inside that process's
 * shared page, and string lengths are bounded before any kernel call.
 */
#include "scos.h"
#include "security.h"
#include "process.h"
#include "usermode.h"
#include "userland_image.h"

extern char sc_user_fault_return[];
uint32_t user_fault_taken;   /* consumed by isr_common (gen_vectors.py) */

static u8 allow[SC_NR_MAX + 1];
static u64 limits[USER_RLIMIT_COUNT];
static u64 faults, syscalls, denied, timeouts;
static int active_pid;
static struct window *active_window;
static char ns_dir[96];         /* private root: home/appdata/<account>/<id>/ */
static char ns_dir_of[PROC_MAX + 1][96];
static struct window *win_of[PROC_MAX + 1];
static u32 episode_syscalls;
static u32 episode_pixels;
static u64 episode_start;
/* One CPL3 episode may cost about as much drawing as the kernel-side host's
 * budget allows (4,000,000 pixels).  Without this an isolated app could issue
 * 100,000 maximum-size fills per callback and hang a CPU-rendered desktop. */
#define USER_PIXEL_BUDGET 4000000u
int user_attach_result;
static u32 last_result;
static struct {
    u32 keycode, pressed, shift, ctrl, alt;
    u32 mx, my, buttons;
    u32 mtype, mbutton, mdown, mwheel;
} input_state;
/* Per-process interval requested by scos.interval(); 0 means "not asked". */
static u16 pid_interval[PROC_MAX + 1];

static struct user_shared *shared(void) { return (struct user_shared *)(uintptr_t)USER_SHARED_VA; }

void usermode_init(void)
{
    for (int i = 1; i <= SC_NR_MAX; i++) allow[i] = 1;
    limits[USER_RLIMIT_MEMORY]      = USER_HEAP_SIZE;
    /* One source for the app-storage caps: the cgroup view and the kernel-side
     * host must not drift, or the same package would be allowed to write more
     * data isolated than unisolated. */
    limits[USER_RLIMIT_FILES]       = APP_STORAGE_MAX_FILES;
    limits[USER_RLIMIT_FILEBYTES]   = APP_STORAGE_MAX_BYTES;
    limits[USER_RLIMIT_SYSCALLS]    = 100000;
    limits[USER_RLIMIT_CPU_MS]      = 1000;
}
int  usermode_enabled(void) { return 1; }
int  user_in_usermode(void) { return sc_user_active != 0; }
/* Set when the last episode was stopped by the CPU budget rather than by a
 * fault, so the WM can tell the owner what actually happened. */
static int last_timeout;
int  user_last_was_timeout(void) { return last_timeout; }
uint64_t user_fault_count(void) { return faults; }
uint64_t user_syscall_count(void) { return syscalls; }
uint64_t user_denied_count(void) { return denied; }
int  user_seccomp_allowed(u64 nr)
{
    /* Compare the FULL 64-bit value: an attacker controlling RAX must not be
     * able to reach SC_NR_EXIT with 0x100000001 via an int truncation. */
    return nr >= 1 && nr <= SC_NR_MAX && allow[nr] != 0;
}
void user_set_seccomp(int nr, int on)
{
    if (nr >= 1 && nr <= SC_NR_MAX) allow[nr] = on ? 1 : 0;
}
int  user_namespace_active(void) { return active_pid != 0; }
uint64_t user_resource_limit(int which)
{
    return which >= 0 && which < USER_RLIMIT_COUNT ? limits[which] : 0;
}
void user_set_resource_limit(int which, u64 value)
{
    if (which >= 0 && which < USER_RLIMIT_COUNT) limits[which] = value;
}
uint32_t user_last_result(void) { return last_result; }
void user_set_input(int event, int keycode, int pressed, int shift, int ctrl, int alt,
                    int mx, int my, int buttons, int mouse_type, int mouse_button,
                    int mouse_down, int mouse_wheel)
{
    (void)event;
    input_state.keycode = (u32)keycode; input_state.pressed = (u32)pressed;
    input_state.shift = (u32)shift; input_state.ctrl = (u32)ctrl;
    input_state.alt = (u32)alt;
    input_state.mx = (u32)mx; input_state.my = (u32)my;
    input_state.buttons = (u32)buttons;
    input_state.mtype = (u32)mouse_type; input_state.mbutton = (u32)mouse_button;
    input_state.mdown = (u32)mouse_down; input_state.mwheel = (u32)mouse_wheel;
}
int user_pid_interval(int pid)
{
    return pid > 0 && pid <= PROC_MAX ? (int)pid_interval[pid] : 0;
}
uint64_t user_image_entry(void) { return USER_BASE; }
uint32_t user_image_span(void) { return USERLAND_IMAGE_BYTES; }
int user_pid_mapped(int pid, uintptr_t va)
{
    return memory_domain_mapped(proc_domain(pid), va);
}

/* --------------------------------------------------------------- namespaces --- */
/* Dots are legal in filenames but must never form a whole ".." segment. */
static int name_traversal(const char *n)
{
    for (const char *p = n; *p; p++) {
        if (*p != '.') continue;
        if ((p == n || p[-1] == '/') && p[1] == '.' &&
            (p[2] == 0 || p[2] == '/')) return 1;
    }
    return 0;
}
#define USER_STR_MAX 256
static const char *scratch_at(u32 off)
{
    struct user_shared *sh = shared();
    if (off >= sizeof(sh->scratch)) return "";
    const char *s = sh->scratch + off;
    /* Bounded: a 64 KiB scratch string must never become a 64 KiB syscall
     * argument (CPU denial of service) or an unclipped text draw. */
    u32 n = 0, cap = (u32)(sizeof(sh->scratch) - off);
    if (cap > USER_STR_MAX) cap = USER_STR_MAX;
    while (n < cap && s[n]) n++;
    if (n >= cap) {
        /* Not NUL-terminated within the bound: hand back a bounded copy. */
        static char bounded[USER_STR_MAX + 1];
        memcpy(bounded, s, cap - 1);
        bounded[cap - 1] = 0;
        return bounded;
    }
    return s;
}
static int ns_path(const char *name, char *out, size_t cap)
{
    size_t n;
    if (!name || !*name || name[0] == '/' || !strcmp(name, ".") ||
        !strcmp(name, "..") || name_traversal(name)) return 0;
    n = strlen(name);
    if (n >= 64) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= 'A' && name[i] <= 'Z') ||
              (name[i] >= '0' && name[i] <= '9') || name[i] == '.' ||
              name[i] == '_' || name[i] == '-')) return 0;
    sc_copy(out, cap, ns_dir);
    sc_cat(out, cap, name);
    return 1;
}
static int cap_ok(u32 cap) { return proc_pid_cap_allowed(active_pid, cap); }

/* --------------------------------------------------------------- attach/run --- */
int user_attach(int pid, struct window *w)
{
    int d = proc_domain(pid), pages, i;
    char tmp[96];
    user_attach_result = 0;
    if (!d) return 0;
    pages = (USERLAND_TOTAL_BYTES + 4095) / 4096;
    if (pages + USER_IMAGE_PAGE > USER_STACK_PAGE) return 0;
    if (!memory_domain_user_map(d, USER_IMAGE_PAGE, pages) ||
        !memory_domain_user_exec(d, USER_IMAGE_PAGE,
                                 (USERLAND_TEXT_BYTES + 4095) / 4096) ||
        !memory_domain_user_map(d, USER_STACK_PAGE, 16) ||
        !memory_domain_user_map(d, USER_HEAP_PAGE, 16) ||
        !memory_domain_user_map(d, USER_SHARED_PAGE, 40)) return 0;
    /* Fill the image through the (supervisor-only) identity alias. */
    for (i = 0; i < pages; i++) {
        uintptr_t p = memory_domain_phys(d, USER_IMAGE_PAGE + i);
        u32 off = (u32)i * 4096, n = 4096;
        if (!p) return 0;
        if (off >= USERLAND_IMAGE_BYTES) n = 0;
        else if (USERLAND_IMAGE_BYTES - off < n) n = USERLAND_IMAGE_BYTES - off;
        memset((void *)p, 0, 4096);
        if (n) memcpy((void *)p, userland_image + off, n);
    }
    {
        char safe[SC_APP_ID_MAX];
        const char *src = proc_pid_app_id(pid);
        size_t k = 0;
        for (; *src && k + 1 < sizeof(safe); src++) {
            char c = *src;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_') safe[k++] = c;
        }
        safe[k] = 0;
        if (!k) sc_copy(safe, sizeof(safe), "app");
        /* The account segment comes from the same function the kernel-side
         * host uses, and from the account that created this process, so both
         * hosts see home/appdata/<account>/<id>/ even after a user switch. */
        if (!app_storage_dir_for(proc_owner(pid), safe, ns_dir, sizeof(ns_dir) - 1))
            sc_copy(ns_dir, sizeof(ns_dir), "home/appdata/nobody/app");
        sc_cat(ns_dir, sizeof(ns_dir), "/");
    }
    /* Private root is created by the storage layer, never by app code: it is
     * the one module that knows the account segment and may create it. */
    sc_copy(tmp, sizeof(tmp), ns_dir);
    tmp[strlen(tmp) - 1] = 0;
    if (!vfs_lookup(tmp)) {
        /* create the account root and this app's directory as that account */
        char dir[96];
        if (!app_storage_dir_for(proc_owner(pid), proc_pid_app_id(pid), dir, sizeof(dir)))
            return 0;
        if (!app_storage_ensure_for(proc_owner(pid), proc_pid_app_id(pid))) return 0;
    }
    sc_copy(ns_dir_of[pid], sizeof(ns_dir_of[pid]), ns_dir);
    win_of[pid] = w;
    user_attach_result = 1;
    return 1;
}
void user_detach(int pid)
{
    if (pid >= 0 && pid <= PROC_MAX) {
        win_of[pid] = 0;
        ns_dir_of[pid][0] = 0;
        pid_interval[pid] = 0;
    }
    if (active_pid == pid) active_pid = 0;
    active_window = 0;
    ns_dir[0] = 0;
}

static void select_process(int pid)
{
    active_pid = pid;
    active_window = pid > 0 && pid <= PROC_MAX ? win_of[pid] : 0;
    sc_copy(ns_dir, sizeof(ns_dir), ns_dir_of[pid]);
}

/* Run one callback in CPL3.  The domain PML4 must already be active for the
 * shared page to exist; user_enter SYSRETs with that CR3 still loaded. */
static u64 run_event(int event, const char *source, u32 len, const char *app_id)
{
    struct user_shared *sh = shared();
    u64 r;
    memset(sh, 0, sizeof(*sh));
    sh->magic = USER_SHARED_MAGIC;
    sh->event = event;
    sc_copy(sh->app_id, sizeof(sh->app_id), app_id ? app_id : "");
    sh->app_id_len = (u32)strlen(sh->app_id);
    if (source && len && len < sizeof(sh->source)) {
        memcpy(sh->source, source, len);
        sh->source_len = len;
    }
    if (active_window) {
        sh->w = (u32)active_window->w;
        sh->h = (u32)active_window->h;
        sh->surface_w = (u32)active_window->surf.w;
        sh->surface_h = (u32)active_window->surf.h;
        sh->focused = wm_focused() == active_window;
    }
    sh->tick_ms = (u32)tick_count * 10;
    sh->keycode = input_state.keycode; sh->key_pressed = input_state.pressed;
    sh->key_shift = input_state.shift; sh->key_ctrl = input_state.ctrl;
    sh->key_alt = input_state.alt;
    sh->mouse_x = input_state.mx; sh->mouse_y = input_state.my;
    sh->mouse_buttons = input_state.buttons;
    sh->mouse_event = input_state.mtype; sh->mouse_button = input_state.mbutton;
    sh->mouse_down = input_state.mdown; sh->mouse_wheel = input_state.mwheel;
    if (active_window) {
        sh->window_x = (u32)active_window->x;
        sh->window_y = (u32)active_window->y;
        sh->window_state = (u32)active_window->state;
    }
    episode_syscalls = 0;
    episode_pixels = USER_PIXEL_BUDGET;
    episode_start = timer_ticks;
    faults = 0;
    last_timeout = 0;
    r = user_enter(USER_BASE, USER_STACK_TOP, USER_SHARED_VA);
    sh->syscall_count = episode_syscalls;
    sh->fault_count = (u32)faults;
    last_result = sh->result;
    return r;
}

int user_run_event(int pid, int event, const char *source, u32 len, const char *app_id)
{
    int d = proc_domain(pid), prev;
    u64 r;
    if (!d) return 0;
    select_process(pid);
    prev = proc_current();
    memory_domain_switch(d);
    r = run_event(event, source, len, app_id);
    memory_domain_switch(0);
    (void)prev;
    return r == (u64)-1 ? -1 : (int)r;
}

/* Test/owner-facing entry point: run a hardware escape probe and report whether
 * it was contained.  Each probe is a separate CPL3 episode. */
int user_sandbox_probe(int pid, int probe, u32 *result)
{
    int d = proc_domain(pid);
    struct user_shared *sh;
    u64 r;
    if (!d) return 0;
    select_process(pid);
    memory_domain_switch(d);
    sh = shared();
    memset(sh, 0, sizeof(*sh));
    sh->magic = USER_SHARED_MAGIC;
    sh->event = USER_EVENT_TICK;
    sc_copy(sh->app_id, sizeof(sh->app_id), proc_pid_app_id(pid));
    sh->tick_ms = ((u32)probe & 0xffffu) << 16;
    last_timeout = 0;
    episode_syscalls = 0;
    episode_pixels = USER_PIXEL_BUDGET;
    episode_start = timer_ticks;
    r = user_enter(USER_BASE, USER_STACK_TOP, USER_SHARED_VA);
    if (result) *result = sh->result;
    sh->fault_count = (u32)faults;
    memory_domain_switch(0);
    return r == (u64)-1 ? -1 : 1;
}

/* ------------------------------------------------------------------ faults --- */
/* The CPU budget also has to stop a package that never makes a syscall.
 * A bare `while true do end` would otherwise hold the machine forever: the
 * only other budget check runs inside the syscall dispatcher, and a tight
 * loop never gets there.  Called from the timer interrupt, and only when that
 * interrupt arrived in CPL3 code (CS.RPL == 3): interrupting a kernel-mode
 * syscall must never abandon the kernel stack. */
void user_tick_budget(void)
{
    if (!active_pid || !sc_user_active) return;
    if ((u32)(tick_count - episode_start) * 10 <= limits[USER_RLIMIT_CPU_MS]) return;
    timeouts++;
    last_timeout = 1;
    sc_user_active = 0;
    /* isr_common sees this and jumps to sc_user_fault_return, which adopts
     * user_enter's continuation instead of returning to the user loop. */
    user_fault_taken = 1;
}

void user_fault(struct interrupt_frame *f)
{
    faults++;
    klog("user: contained fault vec=%d rip=%x cr2=%x cs=%d",
         (int)f->vector, (u32)f->rip, (u32)read_cr2(), (int)f->cs);
    sc_user_active = 0;
    /* isr_common jumps straight to sc_user_fault_return, which adopts the
     * kernel continuation saved by user_enter.  No IRETQ, no frame rewrite. */
    user_fault_taken = 1;
}

/* --------------------------------------------------------------- dispatcher --- */
static u64 deny(void) { denied++; return (u64)-1; }

/* Drawing primitives loop over coordinate ranges.  Clamp every attacker
 * controlled value BEFORE the call so a huge radius/scale/extent cannot turn
 * one syscall into a CPU hang (CPU-render-only machines have no GPU to hide
 * behind).  Signed overflow in x+w would also be undefined behaviour. */
static int draw_coord(u64 v)
{
    i32 n = (i32)v;
    if (n < -4096) return -4096;
    if (n > 4096) return 4096;
    return n;
}
static int draw_extent(u64 v)
{
    i32 n = (i32)v;
    if (n < 0) return 0;
    if (n > 4096) return 4096;
    return n;
}

static u64 do_draw(u64 cmd, u64 a, u64 b, u64 c, u64 d)
{
    struct surface *s;
    int x, y, w, h;
    u32 col, cost = 1;
    if (!active_window || !active_window->surf.px) return deny();
    s = &active_window->surf;
    x = draw_coord(a); y = draw_coord(b);
    w = draw_extent(c >> 32); h = draw_extent(c);
    col = (u32)d;
    switch (cmd) {
        case SC_DRAW_CLEAR:  cost = (u32)s->w * (u32)s->h; break;
        case SC_DRAW_RECT: case SC_DRAW_FRAME: case SC_DRAW_GRADIENT:
            cost = (u32)w * (u32)h; break;
        case SC_DRAW_PIXEL:  cost = 1; break;
        case SC_DRAW_LINE: {
            int dx = draw_coord(c >> 32) - x, dy = draw_coord(c) - y;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            cost = (u32)(dx > dy ? dx : dy) + 1; break;
        }
        case SC_DRAW_CIRCLE: cost = 8u * draw_extent(c) + 8u; break;
        case SC_DRAW_DISC:   cost = 4u * draw_extent(c) * draw_extent(c) + 4u; break;
        case SC_DRAW_TEXT: case SC_DRAW_TEXT_CLIP: case SC_DRAW_TEXT_WRAP:
            cost = (u32)strlen(scratch_at(0)) * 128u + 1u; break;
        case SC_DRAW_TEXT_SCALED: {
            u32 sc2 = draw_extent(d);
            if (sc2 < 1) sc2 = 1;
            if (sc2 > 4) sc2 = 4;
            cost = (u32)strlen(scratch_at(0)) * 128u * sc2 * sc2 + 1u; break;
        }
        case SC_DRAW_ICON:   cost = 4096; break;
        default: break;
    }
    if (cost > episode_pixels) return deny();
    episode_pixels -= cost;
    switch (cmd) {
        case SC_DRAW_CLEAR:  s_fill(s, 0, 0, s->w, s->h, (u32)a); return 1;
        case SC_DRAW_RECT:   s_fill(s, x, y, w, h, col); return 1;
        case SC_DRAW_FRAME:  s_frame_rect(s, x, y, w, h, col); return 1;
        case SC_DRAW_PIXEL:  s_pixel(s, x, y, (u32)c); return 1;
        case SC_DRAW_LINE:
            s_line(s, x, y, draw_coord(c >> 32), draw_coord(c), col); return 1;
        case SC_DRAW_CIRCLE: s_circle(s, x, y, draw_extent(c), col); return 1;
        case SC_DRAW_DISC:   s_disc(s, x, y, draw_extent(c), col); return 1;
        case SC_DRAW_TEXT:   s_text(s, x, y, scratch_at(0), (u32)c); return 1;
        case SC_DRAW_TEXT_SCALED: {
            int sc = draw_extent(d);
            if (sc < 1) sc = 1;
            if (sc > 4) sc = 4;
            s_text_scaled(s, x, y, scratch_at(0), (u32)c, sc); return 1;
        }
        case SC_DRAW_TEXT_CLIP:
            /* colour in d, clip width in c: the same order as the kernel-side
             * api_text_clip(), not the packed (w,h) form. */
            s_clip_text(s, x, y, scratch_at(0), (u32)d, draw_extent(c)); return 1;
        case SC_DRAW_GRADIENT:
            /* Packed pair: low word is the top colour, high word the bottom. */
            s_vgrad(s, x, y, w, h, (u32)d, (u32)(d >> 32)); return 1;
        case SC_DRAW_ICON: {
            /* scos.icon(id, x, y, colour): the id rides in a, not in c. */
            int id = draw_extent(a);
            if (id < 0 || id >= ICON_COUNT) id = 0;
            s_icon(s, id, draw_coord(b), draw_coord(c), (u32)d); return 1;
        }
        default: return deny();
    }
}

static u64 do_file(u64 nr, u64 a, u64 b, u64 c)
{
    struct user_shared *sh = shared();
    char path[128];
    const char *name;
    int writing = nr == SC_NR_FILE_WRITE || nr == SC_NR_FILE_REMOVE ||
                  nr == SC_NR_FILE_RENAME;
    /* Capabilities are held in kernel memory; the namespace alone is not the
     * permission check.  A read-only process must not gain writes by picking
     * a legal filename. */
    if (!cap_ok(writing ? PROC_CAP_VFS_WRITE : PROC_CAP_VFS_READ)) return deny();
    if (nr == SC_NR_FILE_LIST) {
        /* LIST takes a scratch OFFSET, not a file name.  Running it through
         * ns_path() made every scos.files() call fail with "invalid data
         * filename", which the isolated host then reported as a capability
         * denial - so no isolated app could ever list its own directory. */
        struct vfs_node *dir = vfs_lookup(ns_dir);
        u32 n = 0;
        if (a >= sizeof(sh->scratch) - 1) return 0;
        u32 off = (u32)a;
        if (!dir || !dir->is_dir) return 0;
        for (struct vfs_node *f = dir->child; f; f = f->sibling) {
            size_t l = strlen(f->name);
            if (f->is_dir) continue;
            if (n > sizeof(sh->scratch) - off - 2 ||
                l > sizeof(sh->scratch) - off - n - 2) continue;
            memcpy(sh->scratch + off + n, f->name, l);
            sh->scratch[off + n + l] = '\n';
            n += (u32)l + 1;
        }
        sh->scratch[off + n] = 0;
        return n;
    }
    name = scratch_at((u32)a);
    if (!ns_path(name, path, sizeof(path))) return deny();
    if (nr == SC_NR_FILE_EXISTS) return vfs_lookup(path) ? 1 : 0;
    if (nr == SC_NR_FILE_SIZE) {
        /* -1 means "refused", -2 means "no such file": scos.file_size()
         * returns nil for the latter and raises for the former. */
        struct vfs_node *n = vfs_lookup(path);
        return n && !n->is_dir ? n->size : (u64)-2;
    }
    if (nr == SC_NR_FILE_READ) {
        u32 n = 0, got;
        char *data;
        if (b >= sizeof(sh->scratch)) return deny();
        u32 off = (u32)b;
        data = vfs_read(path, &n);
        if (!data) return (u64)-2;
        got = n < (u32)(sizeof(sh->scratch) - off) ? n : (u32)(sizeof(sh->scratch) - off);
        memcpy(sh->scratch + off, data, got);
        return got;
    }
    if (nr == SC_NR_FILE_WRITE) {
        u32 old_bytes = 0;
        struct vfs_node *old;
        char dirpath[96];
        u64 bytes = 0;
        u32 files = 0;
        /* Reject on the 64-bit values.  Casting first would let off=2^32-1
         * wrap `off + n` past the scratch size and write far out of bounds. */
        if (b >= sizeof(sh->scratch) || c > limits[USER_RLIMIT_FILEBYTES] ||
            c > sizeof(sh->scratch) - (u32)b) return deny();
        u32 n = (u32)c, off = (u32)b;
        old = vfs_lookup(path);
        if (old && old->is_dir) return 0;
        if (old) old_bytes = old->size;
        /* The quota is measured from the directory itself, not from counters
         * kept in kernel RAM.  Counters leaked: every delete used to leave
         * its entry charged, so after 16 lifetime writes an app could never
         * create a file again - its own report, notes or settings included. */
        sc_copy(dirpath, sizeof(dirpath), ns_dir);
        { size_t l = strlen(dirpath); if (l && dirpath[l - 1] == '/') dirpath[l - 1] = 0; }
        {
            struct vfs_node *dir = vfs_lookup(dirpath);
            if (dir && dir->is_dir)
                for (struct vfs_node *f = dir->child; f; f = f->sibling) {
                    if (f->is_dir) continue;
                    bytes += f->size;
                    files++;
                }
        }
        /* A refusal says which refusal: -3 is the quota.  Returning plain 0
         * made the CPL3 host report a *successful* write that never happened,
         * while the kernel-side host raised for the same call. */
        if (!old && files >= limits[USER_RLIMIT_FILES]) return (u64)-3;
        if (bytes - old_bytes + n > limits[USER_RLIMIT_FILEBYTES]) return (u64)-3;
        /* The per-app quota is app_storage_check()'s answer, not this loop's:
         * the resource limit above is a cgroup knob the machine may raise, so
         * asking the same validator the kernel-side host asks keeps both hosts
         * at 16 files / 64 KiB whatever the limit says. */
        if (!app_storage_check(proc_pid_app_id(active_pid), name, n)) return (u64)-3;
        if (!vfs_write(path, (const char *)sh->scratch + off, n)) return 0;
        return n;
    }
    if (nr == SC_NR_FILE_REMOVE) {
        return vfs_delete(path) ? 1 : 0;
    }
    if (nr == SC_NR_FILE_RENAME) {
        char dst[128];
        if (!ns_path(scratch_at((u32)b), dst, sizeof(dst))) return deny();
        return vfs_rename(path, dst) ? 1 : 0;
    }
    return deny();
}

uint64_t user_syscall_dispatch(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    u64 nr = sc_user_snr;
    struct user_shared *sh;
    struct rtc_time t;
    (void)a6;
    syscalls++; episode_syscalls++;
    if (!user_seccomp_allowed(nr)) { denied++; return (u64)-1; }
    /* No process context means no capabilities and no private namespace. */
    if (!active_pid && nr != SC_NR_EXIT) { denied++; return (u64)-1; }
    if (episode_syscalls > limits[USER_RLIMIT_SYSCALLS] ||
        (u32)(tick_count - episode_start) * 10 > limits[USER_RLIMIT_CPU_MS]) {
        sc_user_active = 2;            /* terminate the episode (CPU budget) */
        return (u64)-1;
    }
    sh = shared();
    switch (nr) {
        case SC_NR_EXIT:
            sc_user_active = 2;
            return a1;
        case SC_NR_LOG: {
            const char *s = scratch_at((u32)a1);
            if (s[0]) klog("user[%s]: %s", proc_pid_app_id(active_pid), s);
            return 1;
        }
        case SC_NR_CLOCK:      return (u64)tick_count * 10;
        case SC_NR_DATE: {
            /* Full record in the shared page (the packed return value keeps
             * the old shape for anything that used it); the CPL3 API host
             * and the kernel-side host then report the same table. */
            u32 fields[7];
            rtc_read(&t);
            fields[0] = t.year; fields[1] = t.mon; fields[2] = t.day;
            fields[3] = t.hour; fields[4] = t.min; fields[5] = t.sec;
            fields[6] = t.weekday;
            memcpy(sh->scratch, fields, sizeof fields);
            return (u64)t.year << 32 | (u64)t.mon << 24 | (u64)t.day << 16 |
                   (u64)t.hour << 8 | (u64)t.min;
        }
        case SC_NR_SCREEN:     return (u64)screen_w << 32 | (u32)screen_h;
        case SC_NR_MOUSE:      return (u64)sh->mouse_x << 32 | (u32)sh->mouse_y;
        case SC_NR_APP_ID:
            sc_copy(sh->scratch, sizeof(sh->scratch), proc_pid_app_id(active_pid));
            return strlen(sh->scratch);
        case SC_NR_WINDOW:
            return active_window ? ((u64)active_window->w << 32 | (u32)active_window->h) : 0;
        case SC_NR_FOCUSED:
            return active_window && wm_focused() == active_window ? 1 : 0;
        case SC_NR_MEMORY:     return heap_owner_bytes((uintptr_t)active_window);
        case SC_NR_INTERVAL:
            /* scos.interval(ms): the kernel-side host honours it, so the CPL3
             * host must too, or an isolated app ticks at a different rate. */
            if (a1 >= 100 && a1 <= 60000)
                pid_interval[active_pid] = (u16)a1;
            return 1;
        case SC_NR_TEXT_WIDTH: return (u64)s_text_width(scratch_at(0));
        case SC_NR_DRAW:       return do_draw(a1, a2, a3, a4, a5);
        case SC_NR_REDRAW:
            if (active_window) wm_redraw(active_window);
            return 1;
        case SC_NR_TITLE:
            if (active_window) wm_set_title(active_window, scratch_at(0));
            return 1;
        case SC_NR_RESIZE:
            return active_window ? wm_resize_window(active_window, (int)a1, (int)a2) : 0;
        case SC_NR_MOVE:
            return active_window ? wm_move_window(active_window, (int)a1, (int)a2) : 0;
        case SC_NR_MINIMIZE:
            if (active_window) wm_minimize_window(active_window);
            return 1;
        case SC_NR_NOTIFY:
            if (!cap_ok(PROC_CAP_IPC)) return deny();
            wm_notify(proc_pid_app_id(active_pid), scratch_at(0), 0);
            return 1;
        case SC_NR_MESSAGE:
            if (!cap_ok(PROC_CAP_IPC)) return deny();
            wm_dialog(proc_pid_app_id(active_pid), scratch_at(0), 0, 0, 0);
            return wm_dialog_active();
        case SC_NR_THEME_GET: {
            u32 v[10];
            const struct theme *th = theme_current();
            theme_values(th, v);
            /* id at 0, the ten palette values at 40: the same table the
             * kernel-side host returns from scos.theme(). */
            sc_copy(sh->scratch, 40, th->id);
            memcpy(sh->scratch + 40, v, sizeof v);
            return v[0];
        }
        case SC_NR_THEME_LIST: {
            /* The CPL3 host has no direct view of theme tables, so the IDs
             * travel through the scratch page exactly like file names. */
            u32 n = 0;
            int count = theme_count();
            for (int i = 0; i < count; i++) {
                const char *id = theme_get(i)->id;
                size_t l = strlen(id);
                if (n > sizeof(sh->scratch) - 2 || l > sizeof(sh->scratch) - n - 2) break;
                memcpy(sh->scratch + n, id, l);
                sh->scratch[n + l] = '\n';
                n += (u32)l + 1;
            }
            sh->scratch[n] = 0;
            return count;
        }
        case SC_NR_THEME_APPLY:
            if (!cap_ok(PROC_CAP_THEME)) return deny();
            {
                int i = theme_index_of_id(scratch_at(0));
                if (i < 0) return 0;
                theme_set_index(i);
                return 1;
            }
        case SC_NR_FILE_EXISTS: case SC_NR_FILE_SIZE: case SC_NR_FILE_READ:
        case SC_NR_FILE_WRITE:  case SC_NR_FILE_REMOVE: case SC_NR_FILE_RENAME:
        case SC_NR_FILE_LIST:
            return do_file(nr, a1, a2, a3);
        case SC_NR_INFO:
            sh->scratch[0] = 0;
            return 2;
        case SC_NR_THEME_STATUS:
            sc_copy(sh->scratch, 16, "idle");
            return 0;
        case SC_NR_SET_ICON:
            if (!cap_ok(PROC_CAP_APPCTL)) return deny();
            if (!active_window || !active_window->app) return 0;
            {
                int id = (int)a1;
                if (id < 0 || id >= ICON_COUNT) id = 0;
                if (active_window->app->icon != id) {
                    active_window->app->icon = id;
                    wm_request_full();
                }
            }
            return 1;
        case SC_NR_FAULT:
            faults++;
            return 1;
        default:
            return deny();
    }
}

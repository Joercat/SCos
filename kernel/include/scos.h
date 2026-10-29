/* SCos application/desktop API, adapted from the original r43 implementation.
 * Native pointers and sizes; device/protocol fields retain their wire widths. */
#ifndef SCOS_H
#define SCOS_H

#include "kernel.h"
#include "security.h" /* accounts, credentials, secure-screen and command policy */
#include "gpu.h"      /* display adapter registry: detection, matching, engine boundary */
#define SCOS_BUILD_TAG "x64-dev"
/* Which commit these bytes came from.  build/scosbuild.h is generated from the repository by
 * tools/buildinfo.py on every make, and tools/makedisk.py refuses to pack a kernel whose compiled-in
 * stamp differs from the record written next to the image.  Ask for it with `version` or `graphics`,
 * or read \SCOS\BUILD.TXT off the disk from another OS: an image that was built from uncommitted
 * code, or from an older tree than the one under discussion, says so instead of being argued about. */
/* __has_include, not a bare include: tools/tests compiles some of these files directly with their own
 * flags and has no build/ to look in.  An unstamped kernel is still a working kernel - and makedisk
 * refuses to pack one, so the fallback can never quietly reach a machine. */
#if defined(__has_include)
#  if __has_include("scosbuild.h")
#    include "scosbuild.h"
#  endif
#endif
#ifndef SCOS_BUILD_COMMIT
#  define SCOS_BUILD_COMMIT "unstamped"
#  define SCOS_BUILD_BRANCH "unstamped"
#  define SCOS_BUILD_TREE "unknown"
#  define SCOS_BUILD_DATE "unknown"
#endif
void scos_build_stamp(char *out, int cap);   /* "<commit> <tree>, <date>" (UTC) */
int scos_build_modified(void);              /* 1 when the build ran on a dirty tree */
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
#define true 1
#define false 0
#define tick_count timer_ticks
extern volatile int wm_in_idle;
#define regs interrupt_frame

/* ------------------------------------------------------------- string ---- */
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strcat(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);

/* ------------------------------------------------------------------ io ---- */
static inline u8 inb(u16 port) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void outb(u16 port, u8 v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline u16 inw(u16 port) { u16 v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void outw(u16 port, u16 v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port)); }
static inline u32 inl(u16 port) { u32 v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void outl(u16 port, u32 v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port)); }
static inline void io_wait(void) { outb(0x80, 0); }

void cpu_hlt(void);
void irq_enable(void);
void irq_disable(void);
void cpu_reboot_8042(void);
u64  rdtsc(void);
u64 cpu_busy_cycles(void);
extern u64 cpu_tsc_hz;
void cpu_meter_init(void);
void cpu_meter_tick(void);
void cpu_idle_begin(void);
u32  cpu_mhz(void);
u32  cpu_core_count(void);
u32  cpu_thread_count(void);
u32  cpu_signature(void);

/* pci */
u8   pci_read8(u8 bus, u8 dev, u8 fn, u8 off);
u32  pci_read32(u8 bus, u8 dev, u8 fn, u8 off);
void pci_write32(u8 bus, u8 dev, u8 fn, u8 off, u32 v);
int  pci_find_class(u8 class, u8 subclass, u8 progif,
                    u8 *bus, u8 *dev, u8 *fn, int max);
u16 pci_read16(u8,u8,u8,u8);
extern unsigned pci_config_writes;  /* total config-space writes; 0 is the display-safe state */
void pci_write16(u8,u8,u8,u8,u16);
int pci_scan_dump(void);
int pci_memory_bar(u8,u8,u8,u8,u64 *,u64 *);
/* Identity-map device memory for a driver that owns its function; already-mapped pages are
 * reused, so mapping a firmware framebuffer is a no-op rather than a page-table conflict.
 * device_unmap releases only pages that carry exactly these attributes. */
void *device_map(u64 physical,u64 bytes,int write_combine,u64 *mapped_bytes);
void device_unmap(u64 physical,u64 bytes);
const struct boot_handoff *kernel_boot_handoff(void);
u64 usb_dma_bytes(void);
void usb_init(void);
void usb_poll(void);
void usb_poll_events(void);
void usb_status(char *,int);
void usb_kbd_leds_off(void);
int ata_init(void);
int fs_image_load(void);
/* Anti-rollback anchor persisted outside the vault image (see ata.c). */
int  rollback_valid(void);
u32  rollback_high_water(void);
int  rollback_check(u32 gen);
int  rollback_bump(u32 gen);
/* usb (xHCI + HID boot) */
void input_status(char *out, int max);
void error_screen(const char *subsys, const char *msg,
                  const char *const *dump, int ndump);
void err_notify(const char *subsys, const char *msg,
                const char *const *dump, int ndump);
int  err_pending(void);
void err_show_pending(void);
int  klog_ring_count(void);
int  klog_ring(int i, char *out, int max);
/* input injectors for the USB HID path */
void mouse_inject(u8 buttons, i32 dx, i32 dy, i32 wheel);
void kbd_inject_hid(u8 mod, const u8 *keys, u8 *prev_keys, u8 *prev_mod);
int  mouse_present(void);
u32  cpu_usage_pct(void);

/* ---------------------------------------------------------------- klog ---- */
void klog(const char *fmt, ...);           /* serial console logger */
void klog_raw(const char *s);              /* no-varargs logger (debug-safe) */

/* ----------------------------------------------------------- interrupts --- */
typedef void (*irq_handler_t)(struct regs *);
void kernel_panic(const char *reason);
void kernel_panic_regs(const char *name, struct regs *r);

void idt_init(void);
void irq_install(u8 irq, irq_handler_t h);
void pic_remap(void);
void pic_clear_mask(u8 irq);
void pic_set_mask(u8 irq);
void pic_send_eoi(u8 irq);

/* ----------------------------------------------------------------- pit ---- */
void pit_init(u32 hz);
extern volatile u64 tick_count;            /* ticks since boot */
u32  uptime_ms(void);
void sleep_ms(u32 ms);

/* ------------------------------------------------------------ keyboard ---- */
enum {
    KEY_NONE = 0,
    KEY_UP = 128, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_DELETE, KEY_INSERT,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_LSHIFT, KEY_RSHIFT, KEY_LCTRL, KEY_RCTRL, KEY_LALT, KEY_RALT,
    KEY_CAPS, KEY_NUM, KEY_SCROLL,
    KEY_LSUPER, KEY_RSUPER       /* the Super/Windows modifier (E0 5B / E0 5C) */
};

struct key_event {
    u8   scancode;      /* raw set-1 code (make code; +0x80 when released) */
    u16  keycode;       /* ascii or KEY_* code */
    u8   pressed;
    u8   shift, ctrl, alt, super;
};

void kbd_init(void);
int  kbd_poll(struct key_event *out);      /* 1 if an event was dequeued */
extern volatile u32 input_last_tick;   /* tick of last kbd/mouse event */
extern int input_guard_armed;           /* WM arms the silence guard */

/* --------------------------------------------------------------- mouse ---- */
enum { MEV_MOVE = 1, MEV_BUTTON, MEV_WHEEL };
enum { MBTN_LEFT = 1, MBTN_RIGHT = 2, MBTN_MIDDLE = 4 };

struct mouse_event {
    u8  type;           /* MEV_* */
    i16 dx, dy;
    i8  wheel;
    u8  buttons;        /* button state bitmask after event */
    u8  button;         /* which button changed (for MEV_BUTTON) */
    u8  down;           /* 1 pressed / 0 released */
};

void mouse_init(void);
int  mouse_poll(struct mouse_event *out);

/* --------------------------------------------------------------- frame ---- */
struct surface {
    u32 *px;
    int  w, h;
    /* Clip rectangle in surface coordinates.  `cw == 0' means "not clipped",
     * which is what every zeroed surface is, so nothing that existed before
     * this field changed behaviour.  s_pixel() and s_fill() honour it - those
     * two are what every glyph, line and shape is made of - so a scrolled panel
     * can be clipped to its viewport once instead of every caller intersecting
     * its own rectangles and text, which is how half-drawn rows at a window edge
     * happened in the first place. */
    int  cx, cy, cw, ch;
};

/* Set the clip rectangle (intersected with the current one), and clear it. */
void s_clip_set(struct surface *s, int x, int y, int w, int h);
void s_clip_off(struct surface *s);

extern struct surface screen;              /* back buffer, full screen */
extern int screen_w, screen_h;

u32 fb_bpp(void);
void fb_init(void);
u32 fb_bpp(void);
void fb_flip(void);                        /* back buffer -> GOP */
void fb_scene_clip(int x,int y,int w,int h);
void fb_scene_unclip(void);
void fb_flip_rect(int x, int y, int w, int h);   /* damaged region only */
void fb_clear(u32 color);

void s_pixel(struct surface *s, int x, int y, u32 c);
void s_fill(struct surface *s, int x, int y, int w, int h, u32 c);
void s_frame_rect(struct surface *s, int x, int y, int w, int h, u32 c);
void s_line(struct surface *s, int x0, int y0, int x1, int y1, u32 c);
void s_circle(struct surface *s, int cx, int cy, int r, u32 c);
void s_disc(struct surface *s, int cx, int cy, int r, u32 c);
void s_ellipse(struct surface *s, int cx, int cy, int rx, int ry, u32 c);
void s_ellipse_fill(struct surface *s, int cx, int cy, int rx, int ry, u32 c);
void s_rrect(struct surface *s, int x, int y, int w, int h, int r, u32 c);
void s_triangle(struct surface *s, int x0, int y0, int x1, int y1, int x2, int y2, u32 c);
void s_vgrad(struct surface *s, int x, int y, int w, int h, u32 top, u32 bot);

#include "text.h"

extern const u8 font8x16[256][16];

void s_char(struct surface *s, int x, int y, char ch, u32 fg);
void s_char_bg(struct surface *s, int x, int y, char ch, u32 fg, u32 bg);
void s_text(struct surface *s, int x, int y, const char *str, u32 fg);
void s_text_bg(struct surface *s, int x, int y, const char *str, u32 fg, u32 bg);
char *strncat(char *d, const char *s, size_t n);
const char *strstr(const char *h, const char *n);
char *str_chr(const char *s, char c);
void cpu_brand(char *out, int max);
void s_scos_logo(struct surface *s, int x, int y, u32 color, int scale, int phase);
void s_text_scaled(struct surface *s, int x, int y, const char *str, u32 fg, int scale);
int  s_text_width(const char *str);
void s_clip_text(struct surface *s, int x, int y, const char *str, u32 fg, int max_w);

/* Flowing text: word-wrapped rows, and the row count every sizing caller needs.  See
 * kernel/desktop/textwrap.c - a label in a fixed cell clips, anything a person reads does not. */
int  s_text_cols(int width);
int  s_wrap_next(const char *text, int cols, int *rowlen, int *advance);
int  s_wrap_rows(const char *text, int cols);
const char *s_wrap_row(const char *text, int cols, char *out, int out_size);
int  s_text_wrap(struct surface *s, int x, int y, int width, const char *text, u32 fg);
void s_blit(struct surface *d, struct surface *s, int dx, int dy);

/* shared single-line text editor helper (returns 1 if buffer changed) */
int edit_line(char *buf, int *pos, int maxlen, struct key_event *e);

char *str_str(const char *hay, const char *needle);
double strtod_simple(const char *s, char **end);
void fmt_double(char *out, double v);

/* number formatting */
void fmt_u32(char *out, u32 v);
void fmt_u64(char *out, u64 v);
void fmt_i32(char *out, i32 v);
void fmt_pad2(char *out, u32 v);

/* icon drawing (procedural, 24x24 logical box at x,y) */
enum { ICON_FOLDER, ICON_TERMINAL, ICON_NOTEPAD, ICON_BROWSER,
       ICON_CALENDAR, ICON_SETTINGS, ICON_INFO, ICON_CARDS, ICON_CHART, ICON_SOL, ICON_COUNT };
void s_icon(struct surface *s, int id, int x, int y, u32 c);
u32  color_blend(u32 a, u32 b, int t);
int  card_w(void);
int  card_h(void);
void card_draw(struct surface *s, int x, int y, u8 card, int face_down, u32 back_color);
const char *card_rank_str(int rank);

/* ------------------------------------------------------------------ mm ---- */
void  mm_init(void);
void mm_stats(u64 *total_kb, u64 *free_kb);
void mm_ops(u32 *allocs, u32 *frees);      /* live palloc/pfree counters */
void *palloc(size_t bytes);
#define HEAP_WM 1
#define HEAP_VFS 2
void *palloc_owned(size_t bytes,uintptr_t owner);
uintptr_t heap_set_owner(uintptr_t owner);
u64 heap_owner_bytes(uintptr_t owner);                   /* page-granular, zeroed not guaranteed */
void  pfree(void *p, size_t bytes);
u64   mm_total_kb(void);
u64   mm_free_kb(void);

/* ----------------------------------------------------------------- vfs ---- */
#define VFS_NAME 48

struct vfs_node {
    char name[VFS_NAME];
    int  is_dir;
    char *data;
    u32  size, cap;
    struct vfs_node *child, *sibling, *parent;
};

extern struct vfs_node *vfs_root;

int vfs_init_defaults(void);
struct vfs_node *vfs_lookup(const char *path);
int   vfs_list(struct vfs_node *dir, char names[][VFS_NAME], int max);
char *vfs_read(const char *path, u32 *len);
int   vfs_write(const char *path, const char *data, u32 len);
int   vfs_mkdir(const char *path);
int   vfs_delete(const char *path);
struct vfs_node *vfs_child(struct vfs_node *dir, const char *name);
int   vfs_rename(const char *oldp, const char *newp);
int   vfs_factory_reset(void);
void  system_files_init(int have_disk);
int   vfs_is_dir(struct vfs_node *n);
u32   vfs_usage_bytes(void);
u32   str_to_u32(const char *s);
extern const char *const neofetch_art[];
extern const int neofetch_art_lines, neofetch_art_width;
extern const char *const panic_art[];
extern const int panic_art_lines, panic_art_width;
char *vfs_parent_path(const char *path, char *out);   /* "a/b/c" -> "a/b/" */

/* Per-app private storage: home/appdata/<account>/<id>/.  Always private,
 * independent of the owner's per-app isolation choice, separate per account,
 * and wiped for every account when the app is uninstalled. */
#define APP_STORAGE_MAX_FILES 16
#define APP_STORAGE_MAX_BYTES (64u * 1024u)
int  app_storage_id_valid(const char *id);
int  app_storage_name_valid(const char *name);
int  app_storage_dir(const char *id, char *out, size_t cap);
int  app_storage_dir_for(const char *account, const char *id, char *out, size_t cap);
int  app_storage_path(const char *id, const char *name, char *out, size_t cap);
int  app_storage_ensure(const char *id);
int  app_storage_ensure_for(const char *account, const char *id);
int  app_storage_used(const char *id, int *files, u32 *bytes);
int  app_storage_check(const char *id, const char *name, u32 bytes);
int  app_storage_wipe(const char *id, int *files, u32 *bytes);
int  app_storage_wipe_all(const char *id, int *files, u32 *bytes);
int  app_storage_owns_path(const char *id, const char *path);

/* Decoy files (kernel/desktop/decoys.c): bait planted on purpose, at every
 * layer of the tree, so the isolation test has specific paths to be refused
 * and nothing of the machine's to leak if one ever is not. */
void decoys_plant(void);
int  decoys_count(void);
const char *decoys_path(int i);
const char *decoys_body(int i);         /* shared buffer; read one at a time */
int  decoys_present(int i);             /* still byte-identical to the bait */
int  decoys_describe(char *out, int cap);
int  app_storage_allowed(const char *path);
/* Set only while this module creates or removes home/appdata/<account>/. */
int  app_storage_maintenance(void);

/* ----------------------------------------------------------------- ata ---- */
const char *ata_model(void);                  /* IDENTIFY model string or NULL */
int  ata_present(void);
int  fs_image_save(void);
int  fs_image_unlock_apply(void);  /* decrypt + apply the staged encrypted vault */
const char *fs_image_status(void); /* truthful one-line boot/provenance state */
int  fs_image_available(void);
const char *fs_image_target(void);
void vfs_free_tree(struct vfs_node *n);
extern int fs_image_found;

/* ----------------------------------------------------------------- rtc ---- */
struct rtc_time { u16 year; u8 mon, day, hour, min, sec, weekday; };
void rtc_read(struct rtc_time *t);
u32  rtc_to_epoch(const struct rtc_time *t);

/* ---------------------------------------------------------------- acpi ---- */
void acpi_init(void);
int  acpi_shutdown(void);                     /* 1 if power-off command issued */

/* --------------------------------------------------------------- themes --- */
struct theme {
    const char *id;
    const char *name;
    u32 main;          /* accent */
    u32 bg_top, bg_bot;   /* wallpaper gradient */
    u32 win_bg;
    u32 text;
    u32 title_text;    /* text on accent background */
    u32 taskbar_bg;
};

int theme_custom_count(void);
const struct theme *theme_custom_get(int);
int theme_id_valid(const char *);
int theme_custom_store(const char *,const u32[10],int);
void theme_custom_load(void);
void theme_values(const struct theme *,u32[10]);
int  theme_count(void);
const struct theme *theme_get(int i);
const struct theme *theme_current(void);
void theme_set_index(int i);
int  theme_index_of_id(const char *id);
void theme_load_from_settings(void);

/* -------------------------------------------------------------- windows --- */
struct window;
struct app;

struct app {
    const char *id;
    const char *title;
    int  icon;
    int  single;                    /* only one instance */
    int  def_w, def_h;              /* default window size */
    int  min_w, min_h;              /* smallest usable size (0 = 320x200) */
    int  uses_data;                 /* open() must set w->data (OOM check) */
    void (*open)(struct window *w, void *arg);   /* arg: app-specific (e.g. file path) */
    void (*paint)(struct window *w);
    void (*key)(struct window *w, struct key_event *e);
    void (*mouse)(struct window *w, struct mouse_event *e, int x, int y);
    void (*tick)(struct window *w);
    void (*close)(struct window *w);
    const char *desktop_label;    /* optional short icon label */
    int external;                /* runtime-loaded package, not a built-in */
    int file_editor;             /* default editable-document handler */
    const char *file_suffix;
    void (*document)(struct window *,const char *);
    const char *(*failure)(struct window *w); /* optional startup/runtime status */
    /* Optional drop target for a file dragged onto this app's window.  The application is asked
     * first, with the position in its own client coordinates, and answers 1 when it has consumed
     * the drop - File Explorer moves the file into whichever folder the pointer is over, which is a
     * thing only File Explorer knows.  Answering 0 keeps the ordinary meaning of a drop on a
     * window: open the file with this application. */
    int (*drop)(struct window *w, const char *path, int x, int y);
};

#define WIN_TITLEBAR 26
#define WIN_STATE_NORMAL 0
#define WIN_STATE_MIN 1
#define WIN_STATE_MAX 2

struct window {
    int  id;
    char title[64];
    struct app *app;
    void *data;
    int  x, y, w, h;                /* outer rect (incl. decorations) */
    int  px, py, pw, ph;            /* saved rect for maximize restore */
    int  z;
    int  state;
    struct surface surf;            /* content surface */
    int  dirty;
    int  closing;
    int chrome_dirty;
    size_t data_bytes;                /* legacy requested-payload tally; process metrics use
                                     * the allocator ownership ledger, including rounding */
    int restore_state;               /* state before minimize */
    void *console;                  /* terminal tab that launched this app
                                     * via `appstrt`: receives its logs */
    int  isolated;                  /* USER CHOICE: per-launch isolation flag.
                                     * Never forced.  Native C apps are not
                                     * sandboxed; Lua CAT apps honour it by
                                     * refusing filesystem/theme writes. */
    int  snapped;                   /* scoswm-arch: 1 while a snap layout owns
                                     * the geometry; px/py/pw/ph hold restore. */
};

int  wm_win_count(void);
struct window *wm_win_at(int i);
u32  wm_win_cpu_pct(struct window *w);   /* r39: TSC-measured, 0-100 */
int  wm_app_running(const char *app_id);       /* windows open for an id */

/* per-window real memory attribution (shown by sysmon + terminal `procs`) */
void wm_track_mem(struct window *w, ptrdiff_t delta);

/* appstrt console: the launching terminal tab receives the app's logs */
void wm_set_console(struct window *w, void *term);
void wm_set_pending_console(void *term);       /* attach BEFORE open */
void wm_launch_deferred(const char *app_id, const char *arg, int isolated, void *console);
void wm_service_launch(void);                  /* run by the WM loop, kernel context */
void wm_clear_console(void *term);             /* term tab/window died */
void app_log(struct window *w, const char *line);   /* real app events */
/* implemented by the terminal (console sink) */
void term_console_line(void *term, const char *line);
void term_console_exit(void *term, const char *app_id);

/* process table shared by sysmon and the terminal (app_sysmon.c) */
int  proc_sys_count(void);
const char *proc_sys_name(int i);
u64 proc_win_mem_kb(struct window *w);
u64 proc_wm_mem_kb(void);
u64 proc_kernel_mem_kb(void);

void wm_init(void);
void wm_reload_saved_state(void);
void wm_run(void);                              /* main loop: returns only
                                                 * if the WM dies (r36:
                                                 * kmain then falls into
                                                 * the maintenance tty) */
/* r36: kernel maintenance console (tty.c) - a rescue shell owned by the
 * kernel itself, independent of the WM.  `tty` in a terminal raises
 * tty_request and the WM loop hands over; if wm_run() ever returns,
 * kmain enters tty_run(0) rescue mode directly. */
extern int tty_request;
void tty_run(int return_to_wm);
struct window *wm_open_app(const char *app_id, void *arg);
/* Interactive (mouse/keyboard) launch path: shows the first-launch isolation
 * choice prompt before opening the app.  Programmatic wm_open_app() calls do
 * not prompt, so scripts and tests keep working unchanged. */
void wm_launch_app_interactive(const char *app_id, void *arg);
/* Explicit per-launch isolation override from a right-click menu.  The only
 * way a system (built-in) app can be started isolated. */
void wm_launch_app_override(const char *app_id, void *arg, int isolated);
int  wm_app_is_system(const char *app_id);
void wm_close_window(struct window *w);
void wm_set_title(struct window *w, const char *title);
int wm_resize_window(struct window *,int,int);
int wm_move_window(struct window *,int,int);
void wm_redraw(struct window *w);
void wm_request_full(void);     /* full repaint after a held overlay exits */
void wm_theme_changed(void);
void wm_focus(struct window *w);
void wm_minimize_window(struct window *w);
struct window *wm_focused(void);
int  wm_content_w(struct window *w);
static inline int mx_abs(struct window *w, int x) { return w->x + 1 + x; }
static inline int my_abs(struct window *w, int y) { return w->y + WIN_TITLEBAR + y; }
int  wm_content_h(struct window *w);

/* dialog: modal. input!=NULL shows a text field prefilled with input. */
typedef void (*dialog_cb)(int ok, const char *text, void *ud);
void wm_dialog(const char *title, const char *message, const char *input,
               dialog_cb cb, void *ud);
int  wm_dialog_active(void);

/* context menu */
typedef void (*menu_cb)(int item, void *ud);
void wm_menu(int x, int y, const char **items, int n, menu_cb cb, void *ud);

/* spawn an "ERROR" style popup (easter egg + shutdown screens) */
void wm_error_popup(const char *text);
void wm_fatal_screen(const char *line1, const char *line2);
void wm_poweroff_screen(void);

/* apps registry */
#define SCOS_APP(symbol, order) \
    static struct app * const scos_register_##symbol \
    __attribute__((used,section(".scos_apps." #order),aligned(8))) = &(symbol)
int app_register(struct app *app);
struct window *app_open_document(const char *path);
struct app *lua_app_install(const char *path);
void lua_apps_refresh(void);
void apps_register_all(void);
struct app *app_find(const char *id);
void wm_desktop_pin_file(const char *path);
int  app_count(void);
struct app *app_at(int i);


/* boot screen */
void boot_screen_step(const char *msg, int progress_pct);
void boot_screen_init(void);

/* persistence hooks */
void settings_save(void);
#define PREFS_MOUSE_DEFAULT 2
#define PREFS_DBL_DEFAULT 500
struct prefs { int mouse_sens; int dbl_ms; };
const struct prefs *prefs_get(void);
void prefs_set_mouse(int sens);
void prefs_set_dbl(int ms);
/* ------------------------------------------------------ drag and drop ---- */
/* Two payloads only, because those are the two things a desktop can carry: an application id or a
 * path.  The source (Files, the launcher) starts a drag, the WM resolves a drop from what is under the
 * pointer, and wm_dnd_target() answers "what would this do?" without doing it - so the behaviour is
 * assertable from a test on a known geometry instead of inferred from a screenshot. */
#define DND_APP 0
#define DND_FILE 1
int wm_dnd_begin(int kind, const char *payload, const char *label);
int wm_dnd_active(void);
int wm_dnd_cancel(void);
int wm_dnd_drop(int px, int py);
int wm_dnd_target(int px, int py, char *out, int cap);
/* A desktop icon's own actions, as one table that the right-click menu, the keyboard paths and the
 * regression suite all drive, so no two of them can describe the desktop differently. */
int wm_desk_actions(int item, const char **labels, int max);
int wm_desk_invoke(int item, int action);
int wm_desk_app_state(const char *app_id);   /* 1 hidden, 0 visible, -1 no desktop entry */
int wm_desk_show_app(const char *app_id);
void wm_desktop_restore(void);
void wm_desktop_install(const char *id);
int wm_desk_vis_count(void);
int wm_desk_vis_get(int idx, char *app, char *path, char *label, int *kind);
/* Read side of the icon grid.  The regression suites need to assert what a gesture *did* - which
 * icons are selected, where an icon ended up on the grid, whether an application's windows are all
 * put away - and one way to select a menu line by its text rather than by an index that shifts when
 * a line is added. */
u32  wm_desk_sel_mask(void);
int  wm_desk_pos(int vis_idx, int *gx, int *gy);
int  wm_desk_action_index(int item, const char *label);
int  wm_app_minimized(const char *app_id);      /* 1 put away, 0 on screen, -1 not running */
void wm_wallpaper_invalidate(void);
void system_reset(void);

/* ----------------------------------------------- scoswm-arch WM port ---- */
/* Live desktop preferences, persisted in home/desktop.cfg.  All of them are
 * owner choices; nothing here is forced on.  Default values reproduce the
 * original SCos desktop exactly (40 px taskbar, 24 px icons, grid on). */
#define WMP_BAR_HEIGHT   0
#define WMP_ICON_SIZE    1
#define WMP_GRID         2
#define WMP_HOTKEYS      3
#define WMP_OVERVIEW     4
#define WMP_SNAP         5
#define WMP_COUNT        6
int  wm_taskbar_height(void);
void wm_reflow_windows(void);   /* clamp windows after a work-area change */
void wm_prefs_init(void);
int  wm_desktop_pref(int key);
int  wm_desktop_pref_set(int key, int value);
const char *wm_desktop_pref_name(int key);
void wm_prefs_save(void);
/* Visual MRU window overview (right-click "Windows N"). */
int  wm_overview_active(void);
int  wm_overview_count(void);
int  wm_overview_tile(int index, int *x, int *y, int *w, int *h, int *win_id);
int  wm_overview_toggle(void);
void wm_overview_close(void);
int  wm_overview_hover(void);
/* Snap a window: 1 left, 2 right, 3 top/maximise, 4 restore. */
int  wm_snap_window(struct window *w, int side);
/* Cancel-default confirmation dialog (destructive power actions). */
void wm_confirm(const char *title, const char *message, const char *label,
                void (*cb)(int ok, const char *text, void *ud), void *ud);


/* Shared shell confirmation and real compositor stop. */
struct shell_confirm { char command[256]; };
int confirm_answer(const char *s);
int confirm_command(struct shell_confirm *c, char *line, unsigned cap, char *message);
int parse_pid(const char *s, int *pid);
extern int wm_stop_requested;
void tty_select(int number);
extern u32 input_key_enqueued, input_mouse_enqueued, input_key_dropped, input_mouse_dropped;

void desktop_start(const struct boot_framebuffer *fb);
void desktop_framebuffer(const struct boot_framebuffer *fb);
int fb_scroll_output(u32 x,u32 y,u32 w,u32 h,u32 step,u32 color);
void fb_scroll_cpu(u32 x,u32 y,u32 w,u32 h,u32 step,u32 color);
size_t allocation_bytes(const void *p);
u64 memory_reserved_pages(void);
uintptr_t pages_allocate(size_t count);
void pages_release(uintptr_t p,size_t count);

/* Application lifecycle, launch shortcuts and desktop notification services. */
int app_unregister(struct app *app);
int app_uninstall(const char *id,char *message,size_t capacity);
void app_request_uninstall(const char *id);
void wm_desktop_remove_app(const char *id);
int wm_taskbar_pin(const char *id);
int wm_taskbar_pinned(const char *id);
int wm_taskbar_capacity(void);
void wm_notify(const char *title,const char *text,int warning);
/* Where a notification box is on screen: x, y, width, height of notice `index'.
 * The stack grows upwards from the bottom-right corner above the taskbar, newest box nearest the
 * taskbar, so a notification never covers a window's Minimise/Maximise/Close buttons - the part of
 * the desktop a user has to be able to press.  Returns 0 when there is no such notice, so a caller
 * that walks 0.. until it sees 0 never reads a box that was not drawn. */
int wm_notice_rect(int index,int *out4);
void graphics_init(const struct boot_framebuffer *fb);
#include "gpu.h"
void gpu_init(const struct boot_framebuffer *fb);
/* The buffer every consumer of graphics_report() provides, named because the report outgrew an unnamed one.
 * The panel carries the detection lines, the whole inventory a driver module read from the chip (up to
 * 2048 bytes of it, which is the chip's decision and not this file's - 1152 was the number before a real
 * GB207's report measured 1151 of it and lost its last clause, which is why a copy now states its own
 * limit), and the paragraph about the
 * coprocessor's firmware - and with all three present on a booted machine the report measured 2559 bytes
 * into a 2560 buffer, i.e. it was clipped by one byte of headroom.
 *
 * It is 8192 for the measured reason in the test rather than a comfortable-looking one: the longest report
 * ever printed here was 4582 bytes - a complete firmware set staged, every image held and parsed, the module
 * inventory and all five adapter listings - and tools/tests/test_gsp_firmware.py requires the buffer to hold
 * that *plus* the headroom a driver module's own inventory needs (2048 bytes, struct gpu_module_state's
 * describe), because a card with both is the machine this ships for.  The number below is what leaves both
 * room, and the cost of it is stack, measured with -fstack-usage: a 13440-byte frame for the terminal's
 * command dispatcher and 8768 for the tty's command executor, each of which holds one of these buffers,
 * against the 64 KiB kernel stack entry.S sets up.  The buffers stay
 * locals rather than statics because six consoles and a window manager share this path, and a static report
 * buffer would be shared the same way. */
/* A command's whole response, in one buffer, in both the terminal window and the TTY.  8192 was set from a
 * measured worst case that has since grown: the panel plus the FSP paragraphs plus the detection list now runs
 * past it, and what the old size did with the overflow was nothing at all - the tail of the report was simply
 * not printed, mid-word, with no line saying bytes had been dropped.  Doubled, and the producers are handed the
 * same constant, so a report that still does not fit is a report that says so from inside the writer that knows
 * the limit rather than one that stops silently at the edge of a buffer. */
/* 32 KiB, and deliberately not smaller: the measured report runs to about 10 KiB, and the last time this was
 * sized from a worst case the report grew past it and the tail vanished mid-word with no notice.  The limit
 * that matters for reading is TERM_LINES, which bounds what the screen keeps; this one only bounds a buffer,
 * and the writer says out loud when it has filled it. */
#define SCOS_GRAPHICS_REPORT 32768
void graphics_report(char *out,size_t capacity);
/* The same three paragraphs the panel ends with - firmware intake, the parsed images, and the FSP mailbox -
 * and nothing else, for a surface that cannot carry the whole panel.  The panel is longer than what a person
 * can copy out of a console, and the paragraphs at its end are the ones that answer a question, so they get
 * their own command rather than their own wording: this calls the panel's own report functions. */
void graphics_fsp_report(char *out,size_t capacity);

#endif /* SCOS_H */

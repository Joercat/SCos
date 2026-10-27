/* SCos user-mode (CPL3) app boundary.
 *
 * Linux/Flatpak's stack, implemented directly for this kernel:
 *   - CPL3 + a per-process PML4 is the unescapable hardware boundary;
 *   - a numbered syscall table with a default-deny allowlist is seccomp;
 *   - a private VFS root per process is the mount namespace;
 *   - only the app's own window/process is visible (PID + IPC namespace);
 *   - capabilities gate theme/notification/app-registration changes;
 *   - memory, file, syscall and CPU limits are cgroups.
 *
 * A CPL3 app cannot read/write kernel memory (supervisor-only pages), cannot
 * see another app's pages (absent), cannot run a privileged instruction
 * (mov cr3 / wrmsr / cli / in / out -> #GP), and cannot reach a kernel
 * function except through this table.
 */
#ifndef SCOS_USERMODE_H
#define SCOS_USERMODE_H

#include "kernel.h"

struct window;

/* User address space.  The kernel's real mappings stay supervisor-only. */
/* USER_BASE is the domain's own 8 MiB slot (DOMAIN_BASE in memory.c).  Each
 * process sees ITS frames here; the kernel root sees them at a different VA. */
#define USER_DOMAIN_BASE  UINT64_C(0x0000600000000000)
/* The image starts after the 16-page CPL0 marker scratch (pages 0..0x0f), so
 * user-mode frames never alias a frame whose identity mapping was removed. */
#define USER_BASE         (USER_DOMAIN_BASE + UINT64_C(0x00040000))
#define USER_IMAGE_MAX    (1024u * 1024u)
#define USER_STACK_TOP    (USER_DOMAIN_BASE + UINT64_C(0x00290000))
#define USER_STACK_SIZE   (64u * 1024u)
#define USER_HEAP_VA      (USER_DOMAIN_BASE + UINT64_C(0x00300000))
#define USER_HEAP_SIZE    (1u * 1024u * 1024u)
#define USER_SHARED_VA    (USER_DOMAIN_BASE + UINT64_C(0x00400000))
#define USER_SHARED_SIZE  (160u * 1024u)
#define USER_ARENA_VA     (USER_DOMAIN_BASE + UINT64_C(0x00500000))
#define USER_ARENA_SIZE   (2u * 1024u * 1024u)
/* page indices inside a domain's 2048-page (8 MiB) slot */
#define USER_IMAGE_PAGE   0x40
#define USER_STACK_PAGE   0x280
#define USER_HEAP_PAGE    0x300
#define USER_SHARED_PAGE  0x400
#define USER_ARENA_PAGE   0x500

/* Syscall numbers - the seccomp allowlist is built from exactly these. */
#define SC_NR_EXIT        1
#define SC_NR_LOG         2
#define SC_NR_CLOCK       3
#define SC_NR_DATE        4
#define SC_NR_SCREEN      5
#define SC_NR_MOUSE       6
#define SC_NR_APP_ID      7
#define SC_NR_WINDOW      8
#define SC_NR_FOCUSED     9
#define SC_NR_MEMORY      10
#define SC_NR_INTERVAL    11
#define SC_NR_TEXT_WIDTH  12
#define SC_NR_DRAW        13
#define SC_NR_REDRAW      14
#define SC_NR_TITLE       15
#define SC_NR_RESIZE      16
#define SC_NR_MOVE        17
#define SC_NR_MINIMIZE    18
#define SC_NR_NOTIFY      19
#define SC_NR_MESSAGE     20
#define SC_NR_THEME_GET   21
#define SC_NR_THEME_LIST  22
#define SC_NR_THEME_APPLY 23
#define SC_NR_THEME_STATUS 24
#define SC_NR_FILE_EXISTS 25
#define SC_NR_FILE_SIZE   26
#define SC_NR_FILE_READ   27
#define SC_NR_FILE_WRITE  28
#define SC_NR_FILE_REMOVE 29
#define SC_NR_FILE_RENAME 30
#define SC_NR_FILE_LIST   31
#define SC_NR_INFO        32
#define SC_NR_SET_ICON    33
#define SC_NR_FAULT       34
#define SC_NR_MAX         34

/* DRAW subcommands. */
#define SC_DRAW_CLEAR     1
#define SC_DRAW_RECT      2
#define SC_DRAW_TEXT      3
#define SC_DRAW_PIXEL     4
#define SC_DRAW_LINE      5
#define SC_DRAW_FRAME     6
#define SC_DRAW_CIRCLE    7
#define SC_DRAW_DISC      8
#define SC_DRAW_GRADIENT  9
#define SC_DRAW_ICON      10
#define SC_DRAW_TEXT_SCALED 11
#define SC_DRAW_TEXT_CLIP 12
#define SC_DRAW_TEXT_WRAP 13
#define SC_DRAW_BUTTON    14
#define SC_DRAW_CHECKBOX  15
#define SC_DRAW_PROGRESS  16

#define USER_EVENT_OPEN   1
#define USER_EVENT_PAINT  2
#define USER_EVENT_KEY    3
#define USER_EVENT_MOUSE  4
#define USER_EVENT_TICK   5
#define USER_EVENT_CLOSE  6

struct user_shared {
    uint32_t magic;
    uint32_t event;
    uint32_t app_id_len;
    uint32_t source_len;
    uint32_t w, h;
    uint32_t focused;
    uint32_t mouse_x, mouse_y, mouse_buttons;
    uint32_t keycode, key_pressed, key_shift, key_ctrl, key_alt;
    uint32_t tick_ms;
    uint32_t surface_w, surface_h;
    uint32_t result;             /* app writes: 0 = ok, else error code */
    uint32_t fault_count;        /* hardware faults contained in this app */
    uint32_t denied_count;       /* seccomp / namespace denials */
    uint32_t syscall_count;
    char     app_id[32];
    char     source[65536];      /* CAT Lua source */
    char     scratch[65536];     /* syscall string/bulk data exchange */
};
#define USER_SHARED_MAGIC UINT32_C(0x53435553)

/* Kernel-side API (kernel/desktop/usermode.c). */
void     usermode_init(void);
int      usermode_enabled(void);
uint64_t user_enter(uint64_t entry, uint64_t user_rsp, uint64_t arg);
uint64_t user_syscall_dispatch(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6);
void     user_fault(struct interrupt_frame *f);
int      user_in_usermode(void);
uint64_t user_fault_count(void);
uint64_t user_syscall_count(void);
uint64_t user_denied_count(void);
int      user_seccomp_allowed(int nr);
void     user_set_seccomp(int nr, int on);
int      user_namespace_active(void);
uint64_t user_resource_limit(int which);
void     user_set_resource_limit(int which, uint64_t value);
int      user_attach(int pid, struct window *w);
void     user_detach(int pid);
int      user_run_event(int pid, int event, const char *source, uint32_t len,
                        const char *app_id);
int      user_sandbox_probe(int pid, int probe, uint32_t *result);
uint32_t user_last_result(void);
void     user_set_input(int event, int keycode, int pressed, int shift, int ctrl, int alt,
                        int mx, int my, int buttons);
int      user_pid_mapped(int pid, uintptr_t va);
uint64_t user_image_entry(void);
uint32_t user_image_span(void);

/* cgroup-style resource-limit indices. */
#define USER_RLIMIT_MEMORY     0
#define USER_RLIMIT_FILES      1
#define USER_RLIMIT_FILEBYTES  2
#define USER_RLIMIT_SYSCALLS   3
#define USER_RLIMIT_CPU_MS     4
#define USER_RLIMIT_COUNT      5

/* Low-level assembly state (kernel/x86_64/usermode.S). */
extern uint64_t sc_user_active;
extern uint64_t sc_user_krsp;
extern uint64_t sc_user_snr;

#endif

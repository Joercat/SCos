/* SCos process isolation API.
 *
 * An "isolated" app (the owner's per-app choice on struct window) is a real
 * process: it has its own PML4, its own private memory arena, and a kernel-held
 * capability set.  Native built-in C apps and Lua CAT packages both go through
 * the same entry/exit wrappers, so neither can bypass the checks by calling a
 * kernel function directly.
 *
 * The capability token is NEVER passed by app code.  proc_current() is set by
 * the WM around every app callback and cleared on exit; the checks below read
 * it from kernel state.  A package cannot forge a pid.
 */
#ifndef SCOS_PROCESS_H
#define SCOS_PROCESS_H

#include "kernel.h"

struct window;

#define PROC_MAX 24

/* Capabilities.  Kernel context (pid 0) and non-isolated apps hold ALL. */
#define PROC_CAP_VFS_READ   0x01u
#define PROC_CAP_VFS_WRITE  0x02u
#define PROC_CAP_THEME      0x04u   /* change the global desktop theme */
#define PROC_CAP_IPC        0x08u   /* notifications / dialogs / other-window control */
#define PROC_CAP_APPCTL     0x10u   /* change global app registration (icons etc.) */
#define PROC_CAP_SYSINFO    0x20u
#define PROC_CAP_ALL        0x3fu

void        proc_init(void);
int         proc_create(struct window *w);
void        proc_destroy(struct window *w);
int         proc_enter(struct window *w);
void        proc_leave(int token);

int         proc_current(void);            /* 0 = kernel / no app running */
int         proc_count(void);
int         proc_isolation_count(void);
int         proc_isolated(void);
const char *proc_app_id(void);
uint32_t    proc_caps(void);
int         proc_cap_allowed(uint32_t cap);
int         proc_pid_cap_allowed(int pid, uint32_t cap);
int         proc_path_allowed(const char *path, int write);
int         proc_pid_path_allowed(int pid, const char *path, int write);
int         proc_pid_for_window(struct window *w);

/* Private memory: pages live ONLY in this process's PML4 slot.  The kernel
 * root can still reach them for compositing, but another process cannot. */
void       *proc_private_alloc(size_t bytes);
uintptr_t   proc_arena_va(int pid);
int         proc_pid_mapped(int pid, uintptr_t va);
uint64_t    proc_kernel_root(void);
uint64_t    proc_current_root(void);
int         proc_activate(int pid);        /* switch CR3 + capability context */
void        proc_deactivate(void);         /* back to the kernel root */
const char *proc_owner(int pid);
int         proc_domain(int pid);
const char *proc_pid_app_id(int pid);
int         proc_pid_isolated(int pid);
uint32_t    proc_pid_caps(int pid);
int         proc_pid_probe(int pid, uintptr_t va, uint64_t *out);
int         proc_pid_access(int pid, const char *path, int write);

#endif

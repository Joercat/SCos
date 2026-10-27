/* SCos CPL3 app server.
 *
 * This is the code that actually executes at ring 3.  It is deliberately
 * freestanding: no kernel headers that define state, no direct kernel calls,
 * no pointer into supervisor memory.  Everything crosses the boundary as a
 * numbered syscall with scalar arguments; strings and bulk data travel
 * through the shared page the kernel maps read/write for this process.
 *
 * The image is linked absolute at USER_BASE and loaded into every user-mode
 * process's own PML4.  The build emits a flat binary; see tools/embed_userland.py.
 */
typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned char u8;

typedef long (*sc_fn)(long, long, long, long, long, long);
extern long sc(long nr, long a1, long a2, long a3, long a4, long a5);
extern void *memset(void *, int, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);
extern unsigned long strlen(const char *);

#define USER_SHARED_MAGIC 0x53435553u
struct user_shared {
    u32 magic, event, app_id_len, source_len;
    u32 w, h, focused;
    u32 mouse_x, mouse_y, mouse_buttons;
    u32 keycode, key_pressed, key_shift, key_ctrl, key_alt;
    u32 tick_ms, surface_w, surface_h;
    u32 result, fault_count, denied_count, syscall_count;
    char app_id[32];
    char source[65536];
    char scratch[65536];
    u32 mouse_event, mouse_button, mouse_down, mouse_wheel;
    u32 window_x, window_y, window_state;
};

#include "user_syscalls.h"

struct user_shared *SH;
struct user_shared *u_sh;
extern int user_cat_main(void);
#define SCRATCH (SH->scratch)

static long call1(long nr, long a)                    { return sc(nr, a, 0, 0, 0, 0); }
static long call2(long nr, long a, long b)            { return sc(nr, a, b, 0, 0, 0); }
static long call3(long nr, long a, long b, long c)    { return sc(nr, a, b, c, 0, 0); }
static long call4(long nr, long a, long b, long c, long d) { return sc(nr, a, b, c, d, 0); }

/* --------------------------------------------------------------- escape probes --- */
/* Each of these must be stopped by hardware.  The kernel runs them in separate
 * user-mode episodes; a contained fault is recorded and the machine survives. */
static void probe_read_kernel(void)
{
    volatile unsigned long *p = (volatile unsigned long *)0x100000ULL; /* kernel image */
    SH->result = (u32)p[0];
}
static void probe_write_kernel(void)
{
    volatile unsigned long *p = (volatile unsigned long *)0x100000ULL;
    p[0] = 0xdeadbeefULL;
}
static void probe_cr3(void)
{
    unsigned long v;
    __asm__ volatile("mov %%cr3,%0" : "=r"(v));
    __asm__ volatile("mov %0,%%cr3" :: "r"(v) : "memory");
}
static void probe_wrmsr(void)
{
    __asm__ volatile("wrmsr" :: "c"(0xc0000082), "a"(0), "d"(0) : "memory");
}
static void probe_cli(void)   { __asm__ volatile("cli"); }
static void probe_port(void)  { __asm__ volatile("outb %0,%1" :: "a"((unsigned char)0), "Nd"(0x80)); }
static void probe_other_app(void)
{
    /* Another process's private arena is at a different USER_BASE slot. */
    volatile unsigned long *p = (volatile unsigned long *)(0x6000000000700000ULL);
    SH->result = (u32)p[0];
}

/* ------------------------------------------------------------- a real app loop --- */
static void paint_demo(void)
{
    long sw = call1(SC_NR_SCREEN, 0), sh = (unsigned long)sw >> 32;
    (void)sh;
    call3(SC_NR_DRAW, SC_DRAW_CLEAR, 0, 0x101820);
    SCRATCH[0] = 0;
    char *msg = "SCos user-mode app (CPL3)";
    unsigned long n = strlen(msg);
    memcpy(SCRATCH, msg, n + 1);
    call4(SC_NR_DRAW, SC_DRAW_TEXT, 16, 20, 0xffffff);
    call1(SC_NR_LOG, (long)SCRATCH);
    long t = call1(SC_NR_CLOCK, 0);
    SH->result = (u32)t;
}

static void file_demo(void)
{
    const char *path = "notes.txt";
    memcpy(SCRATCH, path, 9);
    const char *data = "written from ring 3";
    unsigned long n = strlen(data);
    memcpy(SCRATCH + 256, data, n);
    long ok = call3(SC_NR_FILE_WRITE, 0, 256, (long)n);
    SH->result = (u32)ok;
    long got = call2(SC_NR_FILE_READ, 0, 256);
    if (got == (long)n) SH->result = 2;
}

void user_main(void *arg)
{
    SH = (struct user_shared *)arg;
    u_sh = SH;
    if (SH->magic != USER_SHARED_MAGIC) { call1(SC_NR_EXIT, 1); return; }
    if (SH->source_len) { user_cat_main(); return; }

    u32 probe = SH->tick_ms >> 16;      /* high half selects an escape probe */
    if (probe) {
        switch (probe) {
            case 1: probe_read_kernel();  break;
            case 2: probe_write_kernel(); break;
            case 3: probe_cr3();          break;
            case 4: probe_wrmsr();        break;
            case 5: probe_cli();          break;
            case 6: probe_port();         break;
            case 7: probe_other_app();    break;
            case 8: call1(999, 0);       break; /* syscall outside allowlist */
            case 9: {
                const char *p = "/home/documents/welcome.txt";
                unsigned long n = strlen(p);
                memcpy(SCRATCH, p, n + 1);
                SH->result = (u32)call2(SC_NR_FILE_READ, 0, 0); /* mount namespace */
                break;
            }
            /* Hostile-argument self-tests: these must be REFUSED cleanly,
             * never wrap a 32-bit bound and never fault the kernel. */
            case 10: {          /* off=2^32-1 + n=2 used to wrap off+n */
                const char *p = "x.txt"; memcpy(SCRATCH, p, 6);
                SH->result = (u32)sc(SC_NR_FILE_WRITE, 0, 0xffffffffULL, 2, 0, 0);
                break;
            }
            case 11:            /* 0x100000001 used to truncate to SC_NR_EXIT */
                SH->result = (u32)sc(0x100000001ULL, 0, 0, 0, 0, 0);
                break;
            case 12: {          /* FILE_LIST with a huge offset */
                SH->result = (u32)sc(SC_NR_FILE_LIST, 0xffffffffULL, 0, 0, 0, 0);
                break;
            }
            case 13: {          /* mount-namespace traversal */
                const char *p = "../escape"; memcpy(SCRATCH, p, 10);
                SH->result = (u32)sc(SC_NR_FILE_READ, 0, 0, 0, 0, 0);
                break;
            }
            default: break;
        }
        SH->result = SH->result ? SH->result : 0xff;
        call1(SC_NR_EXIT, 0);
        return;
    }

    switch (SH->event) {
        case USER_EVENT_PAINT: paint_demo(); break;
        case USER_EVENT_TICK:  file_demo();  break;
        default: break;
    }
    call1(SC_NR_EXIT, 0);
}

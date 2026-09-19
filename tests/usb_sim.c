/*
 * SCos USB logic simulator - runs the REAL driver code natively.
 *
 * Why this exists: rounds 25-29 each shipped a "surely this is it" USB fix
 * that the user had to flash and boot to falsify.  The failures were never
 * electrical - they were pure logic bugs (ring-wrap cycle stamping, a
 * SUSPEND-vs-ENABLE bitmask, a config-descriptor parser that threw away
 * composite devices).  Pure logic can be tested HERE, without hardware:
 *
 *   - this file textually #includes kernel/src/usb.c, so the code under
 *     test IS the shipping code (no reimplementation, no drift).
 *   - A mini xHC consumer simulates the controller side exactly where the
 *     bugs lived: consumer cycle-bit checks, Link TRB processing with
 *     Toggle Cycle, IOC -> transfer events on the event ring.
 *   - The descriptor tests replay the user's REAL field devices from the
 *     r29 boot-log photograph: Holtek composite keyboard+mouse 04d9:fc38,
 *     Razer keyboard 1532:02a2, ASUS AURA LED b05:19af, mass storage
 *     346d:5678.
 *
 * Build/run: make usbtest   (wired into `make test`)
 *
 * If the producer ever stamps a TRB with the wrong cycle bit, misplaces a
 * Link TRB, or the parser rejects a boot-HID device, the simulated xHC
 * stops completing transfers and these tests fail LOCALLY - before anyone
 * burns a flash-and-boot cycle.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/mman.h>

#include "../kernel/src/usb.c"      /* the real driver, statics and all */

/* scos.h declares the string functions with kernel prototypes (unsigned
 * sizes, const strstr) - glibc's <string.h> would conflict, so the sim
 * implements them itself via builtins.  No <string.h> include here. */
/* Hand-rolled: __builtin_* inside a same-named wrapper folds back into a
 * call to the wrapper at -O0 -> infinite recursion -> stack-overflow
 * segfault.  Never use builtins here. */
void *memcpy(void *d, const void *s, unsigned n)
{ char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, unsigned n)
{
    char *a = d; const char *b = s;
    if (a < b) { while (n--) *a++ = *b++; }
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
void *memset(void *d, int c, unsigned n)
{ char *a = d; while (n--) *a++ = (char)c; return d; }
int memcmp(const void *a, const void *b, unsigned n)
{
    const unsigned char *x = a, *y = b;
    while (n--) { if (*x != *y) return *x - *y; x++; y++; }
    return 0;
}
unsigned strlen(const char *s)
{ const char *p = s; while (*p) p++; return (unsigned)(p - s); }
int strcmp(const char *a, const char *b)
{ while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, unsigned n)
{ while (n && *a && *a == *b) { a++; b++; n--; }
  return n ? (unsigned char)*a - (unsigned char)*b : 0; }
char *strncpy(char *d, const char *s, unsigned n)
{
    char *r = d;
    while (n && *s) { *d++ = *s++; n--; }
    while (n) { *d++ = 0; n--; }
    return r;
}
char *strcpy(char *d, const char *s)
{ char *r = d; while ((*d++ = *s++)) { } return r; }
char *strcat(char *d, const char *s)
{ char *r = d; while (*d) d++; while ((*d++ = *s++)) { } return r; }
char *strncat(char *d, const char *s, u32 n)
{ char *r = d; while (*d) d++; while (n-- && *s) *d++ = *s++; *d = 0; return r; }
const char *strstr(const char *h, const char *n)
{
    if (!*n) return h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return h;
    }
    return NULL;
}

/* The driver truncates pointers to u32 (PA()) because the kernel is
 * 32-bit.  This harness is built 64-bit -no-pie (statics land below
 * 4 GB) and every dynamic allocation comes from a MAP_32BIT pool, so
 * every pointer the driver ever sees fits in 32 bits. */

/* --------------------------------------------------------- env stubs ---- */
volatile u64 tick_count = 100;

static int sim_verbose = 0;
void klog(const char *fmt, ...)
{
    if (!sim_verbose) return;
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}
void klog_raw(const char *s) { if (sim_verbose) fputs(s, stdout); }
int klog_ring_count(void) { return 0; }
int klog_ring(int i, char *o, int m) { (void)i; (void)o; (void)m; return 0; }

#define SIM_POOL (64u << 20)
static char *sim_pool;
static u32 sim_pool_off;

void *palloc(u32 bytes)
{
    u32 n = (bytes + 4095u) & ~4095u;
    if (!sim_pool) {
        sim_pool = mmap(NULL, SIM_POOL, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (sim_pool == MAP_FAILED) {
            fprintf(stderr, "SIM: MAP_32BIT pool failed\n");
            exit(1);
        }
    }
    if (sim_pool_off + n > SIM_POOL) {
        fprintf(stderr, "SIM: pool exhausted\n");
        exit(1);
    }
    void *p = sim_pool + sim_pool_off;
    sim_pool_off += n;
    memset(p, 0, n);
    return p;
}

static long sim_kbd_reports, sim_mouse_reports;

static u8 sim_m_btn; static i32 sim_m_dx, sim_m_dy, sim_m_wh;
static u16 sim_k_code; static u8 sim_k_ctrl, sim_k_pressed;

/* r35: NO MORE STUBS at the inject boundary.  The real mouse.c and
 * kbd.c are textually included exactly like usb.c, so every report the
 * simulated xHC delivers flows through the REAL mouse_inject ->
 * mouse_apply -> queue and kbd_inject_hid -> kbd_sc -> queue paths, and
 * the tests assert the events the WM would dequeue (mouse_poll /
 * kbd_poll).  The old stubs recorded only what usb.c handed them -
 * blind to everything downstream, which is exactly where the r34
 * inverted-vertical-axis bug lived (HID dy positive = DOWN, queue
 * convention = UP) and where the keyboard's real key-event generation
 * had never once been executed by any test. */
#include "../kernel/src/mouse.c"
#undef QUEUE                      /* mouse.c: 128 - kbd.c wants its own */
/* mouse.c and kbd.c share static names (queue/q_head/q_tail) - harmless
 * in the kernel (separate TUs), a hard error in this single-TU sim, so
 * rename kbd.c's copies around its include */
#define queue  kbd_sim_queue
#define q_head kbd_sim_q_head
#define q_tail kbd_sim_q_tail
#include "../kernel/src/kbd.c"
#undef queue
#undef q_head
#undef q_tail

/* r38: acpi.c joins the single-TU sim so parse_s5 (pure AML-walk logic)
 * is unit-gated.  The r37 field report - "shutdown doesn't shut down,
 * just says your pc is safe to shutdown" - traced to the _S5_ search
 * demanding a NameOp byte AFTER the nameseg where every real DSDT has
 * the PackageOp; slp_typa stayed 0 and firmware ignored the S5 write.
 * The tests below call parse_s5 only; acpi_init/acpi_shutdown are never
 * executed (their IO paths are the real inline-asm stubs). */
int is_v86_box(void) { return 0; }
#include "../kernel/src/acpi.c"

void irq_install(u8 irq, irq_handler_t h) { (void)irq; (void)h; }
void pic_clear_mask(u8 irq) { (void)irq; }

/* drain the real driver queues like the WM main loop does, recording
 * what the WM would see */
static void drain_input(void)
{
    struct mouse_event me;
    while (mouse_poll(&me)) {
        if (me.type == MEV_MOVE) {
            sim_mouse_reports++;
            sim_m_dx = me.dx; sim_m_dy = me.dy;
        } else if (me.type == MEV_BUTTON) {
            if (me.down) sim_m_btn |= me.button;
            else sim_m_btn &= (u8)~me.button;
        } else if (me.type == MEV_WHEEL) {
            sim_m_wh = me.wheel;
        }
    }
    struct key_event ke;
    while (kbd_poll(&ke)) {
        sim_kbd_reports++;
        sim_k_code = ke.keycode;
        sim_k_pressed = ke.pressed ? 1 : 0;
        sim_k_ctrl = ke.ctrl ? 1 : 0;
    }
}

u8   pci_read8(u8 b, u8 d, u8 f, u8 o)  { (void)b;(void)d;(void)f;(void)o; return 0xFF; }
u32  pci_read32(u8 b, u8 d, u8 f, u8 o) { (void)b;(void)d;(void)f;(void)o; return 0xFFFFFFFFu; }
void pci_write32(u8 b, u8 d, u8 f, u8 o, u32 v) { (void)b;(void)d;(void)f;(void)o;(void)v; }
int  pci_find_class(u8 c, u8 s, u8 p, u8 *b, u8 *d, u8 *f, int m)
{ (void)c;(void)s;(void)p;(void)b;(void)d;(void)f;(void)m; return 0; }
int  pci_scan_dump(void) { return 0; }
void fmt_u32(char *o, u32 v) { sprintf(o, "%u", v); }
void sleep_ms(u32 ms) { tick_count += (ms + 9) / 10; }
const char *ata_model(void) { return NULL; }

int vfs_write(const char *path, const char *data, u32 len) { (void)path;(void)data;(void)len;return 0; }
int fs_image_save(void) { return 0; }
void pfree(void *p, u32 n) { (void)p; (void)n; }

/* ------------------------------------------------- mini xHC simulator -- */
static u32 fake_op[512], fake_db[256], fake_rt[64], fake_cap[256];
static u64 fake_dcbaa[16];

struct sim_consumer {
    volatile u32 *ring;
    int ring_trbs;              /* total slots incl. the link at the end */
    u32 deq;                    /* consumer dequeue index */
    u32 cycle;                  /* expected consumer cycle bit */
    u32 slot, dci;
    u32 max_packet, pending_bytes;
    u32 report_len;             /* simulated payload length for events */
    int active;
    long tds;                   /* completed TDs (events posted) */
};
static struct sim_consumer cons[12];   /* r37: T6d + T8 grew the roster */
static int ncons;

static u32 sim_evt_idx;
static u32 sim_evt_cycle = 1;

static void sim_post_event(u32 slot, u32 dci, u32 code, u32 trb_ptr, u32 rem)
{
    volatile u32 *t = evt_ring + sim_evt_idx * 4;
    t[0] = trb_ptr;
    t[1] = 0;
    t[2] = (code << 24) | (rem & 0xFFFFFFu);
    t[3] = (slot << 24) | (dci << 16) | ((u32)EV_TRANSFER << 10) |
           sim_evt_cycle;
    sim_evt_idx++;
    if (sim_evt_idx == EVT_TRBS) { sim_evt_idx = 0; sim_evt_cycle ^= 1; }
}

/* A USB PACKET is not a TD. Full-size packets accumulate until the
 * request is satisfied; only short packets end a TD early. Device NAKs
 * (no report to send) are modeled by not calling this function at all. */
static volatile u32 *sim_current_trb(struct sim_consumer *c)
{
    for (int i=0;i<2;i++) {
        volatile u32 *t=c->ring+c->deq*4;
        if ((t[3]&1u)!=c->cycle) return NULL;
        if (((t[3]>>10)&63)!=TRB_LINK) return t;
        if(t[3]&2)c->cycle^=1;
        c->deq=0;
    }
    return NULL;
}
static int sim_deliver_packet(struct sim_consumer *c, const u8 *packet, u32 len)
{
    volatile u32 *t=sim_current_trb(c);
    if(!c->active || !t) return 0;
    if(len>c->max_packet) { fprintf(stderr,"SIM: packet exceeds endpoint MPS\n"); exit(1); }
    u32 requested=t[2]&0x1FFFFu;
    u32 left=requested-c->pending_bytes;
    u32 copied=len<left?len:left;
    memcpy((u8 *)(unsigned long)t[0]+c->pending_bytes,packet,copied);
    c->pending_bytes+=copied;
    int short_packet=len<c->max_packet;
    if(!short_packet && c->pending_bytes<requested) return 0;
    u32 code=len>left?3:(c->pending_bytes<requested?13:1);
    if ((t[3]&(1u<<5)) || (short_packet && (t[3]&(1u<<2))))
        sim_post_event(c->slot,c->dci,code,(u32)(unsigned long)t,requested-c->pending_bytes);
    c->pending_bytes=0; c->deq++; c->tds++;
    return 1;
}

/* Consume ready TRBs until an IOC fires (one TD per call, like a
 * scheduling xHC) or a not-ready TRB idles the endpoint.  This is where a
 * wrong-cycle producer TRB becomes visible: the consumer stops, the
 * driver times out, and the surrounding test fails. */
static int sim_consume_td(struct sim_consumer *c)
{
    if (!c->active) return 0;
    if (c->dci != 1) {
        volatile u32 *t=sim_current_trb(c);
        if (!t) return 0;
        u8 packet[64]; memcpy(packet,(const void *)(unsigned long)t[0],c->report_len);
        return sim_deliver_packet(c,packet,c->report_len);
    }
    for (int guard = 0; guard < 8 * c->ring_trbs + 8; guard++) {
        volatile u32 *t = c->ring + c->deq * 4;
        if ((t[3] & 1u) != c->cycle) return 0;       /* EP idles here */
        u32 type = (t[3] >> 10) & 0x3F;
        if (type == TRB_LINK) {
            if ((t[3] >> 1) & 1u) c->cycle ^= 1;     /* Toggle Cycle */
            c->deq = 0;                              /* link -> ring start */
            continue;
        }
        u32 ioc = (t[3] >> 5) & 1u;
        /* REAL xHC semantics (r32): a transfer event's TRB Transfer
         * Pointer is the address of the COMPLETED TRB in the ring - not
         * the data buffer.  The old sim posted t[0] (the data pointer),
         * which is exactly why the driver's data-pointer-only matching
         * passed every test and discarded every report on hardware. */
        u32 trb_addr = (u32)(unsigned long)(c->ring + c->deq * 4);
        c->deq++;
        if (ioc) {
            c->tds++;
            /* r37: post the TRUE transfer remainder, computed from the
             * TRB's own length field, like a real xHC.  The old hardcoded
             * rem=0 told the driver every report was a full 16 bytes -
             * the length rule (kbd report >= 9 -> Report-ID prefix) then
             * "passed" even for 8-byte plain reports, and the sim never
             * exercised real length semantics at all. */
            u32 trb_len = t[2] & 0x1FFFFu;
            u32 rem = trb_len > c->report_len ? trb_len - c->report_len : 0;
            sim_post_event(c->slot, c->dci, 1, trb_addr, rem);
            return 1;
        }
    }
    fprintf(stderr, "SIM: consumer runaway on slot %u dci %u\n",
            c->slot, c->dci);
    exit(1);
}

static void sim_pump(void)
{
    for (int i = 0; i < ncons; i++)
        while (sim_consume_td(&cons[i])) { }
}

/* every driver cpu_hlt() = one 10 ms tick + one xHC scheduling pass */
void cpu_hlt(void)
{
    tick_count++;
    sim_pump();
}

static struct sim_consumer *sim_add_consumer(volatile u32 *ring, int trbs,
                                             u32 slot, u32 dci, u32 replen)
{
    if (ncons >= (int)(sizeof cons / sizeof cons[0])) {
        fprintf(stderr, "SIM: consumer table full (%d)\n", ncons);
        exit(1);
    }
    struct sim_consumer *c = &cons[ncons++];
    memset(c, 0, sizeof(*c));
    c->ring = ring; c->ring_trbs = trbs;
    c->deq = 0; c->cycle = 1;
    c->slot = slot; c->dci = dci;
    c->report_len = replen;
    c->active = 1;
    return c;
}

/* The mini "Configure Endpoint" a REAL xHC performs (r31).  The driver
 * hands it an input endpoint context; the xHC decodes type / interval /
 * ESIT payload and only then puts the EP on its schedule.  The r30 field
 * failure is modeled here: an Intel xHC ACCEPTED a Bulk-IN-typed context
 * with zero Max ESIT Payload (command rc 1, rings armed, doorbells rung)
 * but NEVER scheduled it - zero transfer events, input dead, no error
 * anywhere.  sim_cfg_ep reproduces that silent death: returns NULL and
 * no consumer exists, so T3 delivers 0 reports and fails loudly.  PA()
 * is the identity in the sim, so the dequeue word maps straight back. */
static struct sim_consumer *sim_cfg_ep(const u32 *epw, u32 slot, u32 dci,
                                       u32 replen)
{
    u32 interval = (epw[0] >> 16) & 0xFFu;
    u32 type     = (epw[1] >> 3) & 7u;
    u32 mps      = (epw[1] >> 16) & 0xFFFFu;
    u32 deq      = epw[2];
    u32 esit     = (epw[4] >> 16) & 0xFFFFu;
    u32 avg_trb  = epw[4] & 0xFFFFu;
    if (type != 7) return NULL;          /* not INT_IN: never scheduled */
    if (esit < mps || avg_trb < mps) return NULL;   /* zero bandwidth */
    if (interval < 3 || interval > 10) return NULL; /* FS periodic limits */
    if (!(deq & ~0xFu)) return NULL;                 /* no dequeue ptr */
    struct sim_consumer *c = sim_add_consumer(
        (volatile u32 *)(unsigned long)(deq & ~0xFu), IN_TRBS, slot, dci,
        replen);
    c->max_packet = mps;
    c->deq = 0;                          /* dequeue = ring start here */
    c->cycle = deq & 1u;                 /* xHC takes the cycle from ctx */
    return c;
}

/* ------------------------------------------------------------ harness -- */
static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static void sim_env_init(void)
{
    cap = (volatile u8 *)fake_cap;
    op  = (volatile u8 *)fake_op;
    db  = (volatile u8 *)fake_db;
    rt  = (volatile u8 *)fake_rt;
    dcbaa = fake_dcbaa;
    evt_ring = palloc(EVT_TRBS * 16);
    cmd_ring = palloc(CMD_TRBS * 16);
    evt_idx = 0; evt_cycle = 1;
    sim_evt_idx = 0; sim_evt_cycle = 1;
    have_xhci = 1;
    csz = 32;
    max_ports = 15;
}

/* ======================= TEST 1: the r29 field descriptors ============== */
/* Reconstructed from the r29 boot-log photograph (hexdumps + the parser's
 * own iface lines).  The Holtek is a COMPOSITE boot device: iface 0 =
 * boot mouse (3/1/2, ep 81), iface 1 = boot keyboard (3/1/1, ep 82),
 * iface 2 = vendor HID (3/0/0).  The old parser saw iface 2, cleared the
 * selection and rejected the device - that one line is why input never
 * worked.  The Razer is a boot keyboard followed by two consumer-control
 * interfaces (3/0/1, 3/0/2) - same failure mode. */

/* Holtek 04d9:fc38 - wTotalLength 84, 3 interfaces (kbd+mouse combo) */
static const u8 desc_holtek[] = {
    0x09,0x02,0x54,0x00,0x03,0x01,0x00,0xA0,0x32,   /* config: 84 bytes */
    0x09,0x04,0x00,0x00,0x01,0x03,0x01,0x02,0x00,   /* iface 0: 3/1/2 mouse */
    0x09,0x21,0x10,0x01,0x00,0x01,0x22,0x43,0x00,   /* HID descriptor */
    0x07,0x05,0x81,0x03,0x08,0x00,0x01,             /* ep 81 IN int mps 8 */
    0x09,0x04,0x01,0x00,0x01,0x03,0x01,0x01,0x00,   /* iface 1: 3/1/1 kbd */
    0x09,0x21,0x10,0x01,0x00,0x01,0x22,0x2F,0x00,
    0x07,0x05,0x82,0x03,0x08,0x00,0x02,             /* ep 82 IN int mps 8 */
    0x09,0x04,0x02,0x00,0x01,0x03,0x00,0x00,0x00,   /* iface 2: 3/0/0 vendor */
    0x09,0x21,0x10,0x01,0x00,0x01,0x22,0x30,0x00,
    0x07,0x05,0x83,0x03,0x08,0x00,0x0A,             /* ep 83 IN - ignorable */
};

/* Razer 1532:02a2 - boot keyboard iface 0 + two consumer-control ifaces */
static const u8 desc_razer[] = {
    0x09,0x02,0x54,0x00,0x03,0x01,0x00,0xA0,0xFA,
    0x09,0x04,0x00,0x00,0x01,0x03,0x01,0x01,0x00,   /* iface 0: 3/1/1 kbd */
    0x09,0x21,0x11,0x01,0x00,0x01,0x22,0x3D,0x00,
    0x07,0x05,0x81,0x03,0x08,0x00,0x01,             /* ep 81 IN int mps 8 */
    0x09,0x04,0x01,0x00,0x01,0x03,0x00,0x00,0x00,   /* iface 1: 3/0/1 */
    0x09,0x21,0x11,0x01,0x00,0x01,0x22,0x9F,0x00,
    0x07,0x05,0x82,0x03,0x10,0x00,0x01,             /* ep 82 IN mps 16 */
    0x09,0x04,0x02,0x00,0x01,0x03,0x00,0x02,0x00,   /* iface 2: 3/0/2 */
    0x09,0x21,0x11,0x01,0x00,0x01,0x22,0x55,0x00,
    0x07,0x05,0x83,0x03,0x08,0x00,0x01,
};

/* ASUS AURA LED b05:19af - vendor iface + non-boot HID (3/0/0): rejected */
static const u8 desc_aura[] = {
    0x09,0x02,0x2B,0x00,0x02,0x01,0x00,0xA0,0x08,
    0x09,0x04,0x00,0x00,0x00,0xFF,0xFF,0xFF,0x00,   /* iface 0: ff/ff/ff */
    0x09,0x04,0x02,0x00,0x01,0x03,0x00,0x00,0x00,   /* iface 2: 3/0/0 */
    0x09,0x21,0x11,0x01,0x00,0x01,0x22,0x24,0x00,
    0x07,0x05,0x82,0x03,0x20,0x00,0x04,
};

/* mass storage 346d:5678 - class 8/6/80, two bulk EPs: rejected */
static const u8 desc_msc[] = {
    0x09,0x02,0x20,0x00,0x01,0x01,0x00,0x80,0x32,
    0x09,0x04,0x00,0x00,0x02,0x08,0x06,0x50,0x00,
    0x07,0x05,0x01,0x02,0x00,0x02,0x00,
    0x07,0x05,0x82,0x02,0x00,0x02,0x00,
};

/* synthetic: boot keyboard iface whose only endpoint is OUT - must drop */
static const u8 desc_no_in[] = {
    0x09,0x02,0x22,0x00,0x01,0x01,0x00,0xA0,0x32,
    0x09,0x04,0x00,0x00,0x01,0x03,0x01,0x01,0x00,
    0x09,0x21,0x11,0x01,0x00,0x01,0x22,0x3F,0x00,
    0x07,0x05,0x02,0x03,0x08,0x00,0x0A,             /* ep 02 OUT */
};

/* synthetic: same boot iface twice (alt settings) - keep exactly one */
static const u8 desc_alts[] = {
    0x09,0x02,0x2E,0x00,0x02,0x01,0x00,0xA0,0x32,
    0x09,0x04,0x00,0x00,0x01,0x03,0x01,0x01,0x00,   /* iface 0 alt 0 */
    0x09,0x21,0x11,0x01,0x00,0x01,0x22,0x3F,0x00,
    0x07,0x05,0x81,0x03,0x08,0x00,0x0A,
    0x09,0x04,0x00,0x01,0x01,0x03,0x01,0x01,0x00,   /* iface 0 alt 1 */
    0x09,0x21,0x11,0x01,0x00,0x01,0x22,0x3F,0x00,
    0x07,0x05,0x81,0x03,0x08,0x00,0x0A,
};

static void test_parser(void)
{
    struct hid_cand c[MAX_HID_EPS];
    int n;

    n = usb_hid_parse(desc_holtek, (int)sizeof(desc_holtek), c, MAX_HID_EPS, 0);
    CHECK(n == 2, "holtek: expected 2 boot ifaces, got %d", n);
    if (n == 2) {
        CHECK(c[0].iface == 0 && c[0].proto == 2 && c[0].ep_addr == 1 &&
              c[0].ep_mps == 8 && c[0].ep_interval == 1 &&
              c[0].rid_len == 67 && c[1].rid_len == 47,
              "holtek iface0: want mouse proto2 ep81 mps8 bInt1 rdesc 67, "
              "got %d/%d/ep%x/%d/bInt%d/rd%d",
              c[0].iface, c[0].proto, c[0].ep_addr, c[0].ep_mps,
              c[0].ep_interval, c[0].rid_len);
        CHECK(c[1].iface == 1 && c[1].proto == 1 && c[1].ep_addr == 2 &&
              c[1].ep_mps == 8 && c[1].ep_interval == 2,
              "holtek iface1: want kbd proto1 ep82 mps8 bInt2, "
              "got %d/%d/ep%x/%d/bInt%d",
              c[1].iface, c[1].proto, c[1].ep_addr, c[1].ep_mps,
              c[1].ep_interval);
    }

    n = usb_hid_parse(desc_razer, (int)sizeof(desc_razer), c, MAX_HID_EPS, 0);
    CHECK(n == 1, "razer: expected 1 boot iface, got %d", n);
    if (n == 1)
        CHECK(c[0].iface == 0 && c[0].proto == 1 && c[0].ep_addr == 1 &&
              c[0].ep_mps == 8 && c[0].ep_interval == 1,
              "razer iface0: want kbd proto1 ep81 mps8 bInt1, "
              "got %d/%d/ep%x/%d/bInt%d",
              c[0].iface, c[0].proto, c[0].ep_addr, c[0].ep_mps,
              c[0].ep_interval);

    n = usb_hid_parse(desc_aura, (int)sizeof(desc_aura), c, MAX_HID_EPS, 0);
    CHECK(n == 0, "aura LED: expected 0 boot ifaces, got %d", n);

    n = usb_hid_parse(desc_msc, (int)sizeof(desc_msc), c, MAX_HID_EPS, 0);
    CHECK(n == 0, "mass storage: expected 0 boot ifaces, got %d", n);

    n = usb_hid_parse(desc_no_in, (int)sizeof(desc_no_in), c, MAX_HID_EPS, 0);
    CHECK(n == 0, "boot iface without IN ep: expected 0, got %d", n);

    n = usb_hid_parse(desc_alts, (int)sizeof(desc_alts), c, MAX_HID_EPS, 0);
    CHECK(n == 1, "alt-setting dedup: expected 1, got %d", n);

    /* the field hexdumps only capture 64 bytes of an 84-byte descriptor -
     * a truncated tail (zeros) must not crash, hang, or lose candidates */
    u8 trunc[84];
    memcpy(trunc, desc_holtek, 64);
    memset(trunc + 64, 0, 20);
    n = usb_hid_parse(trunc, 84, c, MAX_HID_EPS, 0);
    CHECK(n == 2, "truncated-at-64 holtek: expected 2, got %d", n);
}

/* ================= TEST 2: EP0 ring across many wraps (the r28 bug) ===== */
static void test_ep0_ring(void)
{
    struct xdev *d = slot_alloc(1);
    CHECK(d != NULL, "slot_alloc(1) failed");
    if (!d) return;
    struct sim_consumer *c = sim_add_consumer(d->ep0, EP0_TRBS, 1, 1, 0);

    u8 buf[64];
    /* 60 transfers x 2-3 TRBs = ~150 TRBs through a 16-slot ring:
     * ~10 wrap crossings.  The r28 bug poisoned slot 14 on the very
     * first wrap and every later transfer timed out (rc -1). */
    for (int i = 0; i < 60; i++) {
        int in = i & 1;
        u16 len = in ? (u16)(8 + (i % 4) * 8) : 0;
        int rc = ctrl_xfer(1, in ? 0x80 : 0x00, 6, 0x0100, 0,
                           in ? buf : NULL, len, in);
        CHECK(rc == 1, "ep0 transfer %d: rc %d (want 1) - ring stalled?",
              i, rc);
        if (rc != 1) return;
    }
    CHECK(c->tds == 60, "ep0 consumer completed %ld TDs, want 60", c->tds);
    printf("  ep0 ring: 60 transfers / %ld TDs through ~%d wraps\n",
           c->tds, 150 / (EP0_TRBS - 1));
}

/* ====== TEST 3: composite interrupt rings over ~71 laps (r28 bug 2) ===== */
static void test_interrupt_rings(void)
{
    /* a composite device exactly like the Holtek: TWO boot rings on one
     * slot (keyboard dci 3 + mouse dci 5) */
    struct xdev *d = slot_alloc(2);
    CHECK(d != NULL, "slot_alloc(2) failed");
    if (!d) return;
    d->nhid = 2;
    d->kind = 3;
    struct sim_consumer *ck = NULL, *cm = NULL;
    for (int j = 0; j < 2; j++) {
        struct hid_ep *h = &d->hid[j];
        h->active = 1;
        h->iface = j;
        h->kind = j == 0 ? 1 : 2;
        h->ep_addr = (u8)(j + 1);
        h->dci = (u32)(h->ep_addr * 2 + 1);
        h->ep_mps = j == 0 ? 16 : 8;
        h->binterval = (u8)(j == 0 ? 2 : 1);  /* field: kbd 2, mouse 1 */
        h->inr = (volatile u32 *)palloc(4096);
        /* Synthetic prefix coverage: the 9-byte keyboard report needs
         * MPS >= 9. Actual field MPS8/report8 behavior is tested in T15. */
        h->has_id = 1;
        for (int b = 0; b < IN_TRBS; b++) {
            h->in_buf[b] = palloc(IN_BUF_BYTES);
            memset(h->in_buf[b], 0, IN_BUF_BYTES);
            if (j == 0) {             /* kbd: [ID][mods][rsvd][key...] */
                h->in_buf[b][0] = 1; h->in_buf[b][1] = 0;
                h->in_buf[b][2] = 0; h->in_buf[b][3] = 0x04;
            } else {                  /* mouse: [ID][btn][dx][dy] */
                h->in_buf[b][0] = 2; h->in_buf[b][1] = 0;
                h->in_buf[b][2] = 0x5A; h->in_buf[b][3] = 0x3C;
            }
        }
        h->inr_idx = 0; h->inr_cycle = 1;
        h->link_pend = 0; h->link_pend_cycle = 1;
        hid_arm_ring(d, j);           /* the REAL arming code */
        /* r31: build the endpoint context with the REAL enumerate_device
         * builder, then let the sim xHC schedule from those exact bytes.
         * The old manual sim_add_consumer here never looked at the
         * context - which is precisely how the Bulk-typed / zero-ESIT
         * bug sailed through the suite and died on the H510M-A. */
        u32 epw[5];
        hid_fill_ep_ctx(epw, h, 1);   /* FullSpeed, like the field */
        /* r37: true 9-byte kbd report ([ID][mod][rsv][6 keys]) - with the
         * sim now posting the REAL remainder, an 8-byte replen would make
         * the driver's len>=off+8 gate fail exactly like on hardware. */
        struct sim_consumer *c =
            sim_cfg_ep(epw, 2, h->dci, j == 0 ? 9u : 4u);
        CHECK(c != NULL,
              "xHC would NOT schedule slot 2 dci %u: type %u interval %u "
              "esit %u avg %u (r30-style silent input death)", h->dci,
              (epw[1] >> 3) & 7u, (epw[0] >> 16) & 0xFFu, epw[4] >> 16,
              epw[4] & 0xFFFFu);
        if (!c) continue;
        if (j == 0) ck = c; else cm = c;
    }
    if (!ck || !cm) {
        printf("  interrupt rings: NOT SCHEDULED by the sim xHC\n");
        return;
    }

    sim_kbd_reports = sim_mouse_reports = 0;
    sim_m_btn = 0; sim_m_dx = sim_m_dy = sim_m_wh = 0;
    sim_k_code = 0; sim_k_ctrl = 0; sim_k_pressed = 0;
    /* 500 reports per endpoint = ~71 laps of a 7-slot ring.  The old code
     * never rewrote the Link TRB after lap 1: the consumer met a stale
     * cycle bit at the link and input died permanently after ~14
     * reports.  Events flow through the REAL proc_events re-arm path,
     * and (r35) through the REAL mouse.c/kbd.c queues. */
    trace_active = 1; trace_count = 0;
    for (int i = 0; i < 500; i++) {
        sim_consume_td(ck);
        sim_consume_td(cm);
        proc_events();
        drain_input();
    }
    trace_active = 0;
    CHECK(trace_count == 1000 && input_trace[(trace_count-1)%TRACE_N].sequence == 1000,
          "flight recorder did not wrap/preserve the latest report");
    /* the kbd sends the SAME report 500 times: kbd_inject_hid diffs
     * against prev, so exactly one 'a' make event is correct - the ring
     * proof is the TD/event counts below, the content proof is the 'a' */
    CHECK(sim_kbd_reports == 1,
          "keyboard produced %ld key events for 500 identical reports "
          "(want exactly 1 'a' make)", sim_kbd_reports);
    CHECK(sim_mouse_reports == 500,
          "mouse delivered %ld/500 move events", sim_mouse_reports);
    CHECK(ck->tds == 500 && cm->tds == 500,
          "consumer TD counts %ld/%ld, want 500/500", ck->tds, cm->tds);
    /* content proof: the Report ID prefix must be skipped, so the axes
     * and keys arrive from the byte AFTER the ID (r33 field bug: with
     * off=0 the ID lands in buttons/mods and every axis shifts) - and
     * dy arrives in the QUEUE's PS/2 convention (r35: HID dy +0x3C =
     * physical down = queue dy NEGATIVE, WM does my -= dy) */
    CHECK(sim_m_btn == 0 && sim_m_dx == 0x5A && sim_m_dy == -0x3C,
          "mouse content wrong: btn %u dx %d dy %d (want btn 0 dx 90 "
          "dy -60: ID skipped + y flipped to PS/2 convention)",
          sim_m_btn, sim_m_dx, sim_m_dy);
    CHECK(sim_k_code == 'a' && !sim_k_ctrl && sim_k_pressed,
          "kbd content wrong: keycode %u ctrl %u pressed %u (want 'a' "
          "0x61, no stuck Ctrl - the r32 field symptom)",
          sim_k_code, sim_k_ctrl, sim_k_pressed);
    printf("  interrupt rings: 2 EPs x 500 reports (~71 laps each) - "
           "kbd %ld, mouse %ld delivered\n",
           sim_kbd_reports, sim_mouse_reports);
}

/* ===== TEST 5: Report ID detection from report descriptors (r33) ======== */
/* The r32 field round: mouse axes shifted by one byte and the keyboard
 * behaved like Ctrl-stuck - both devices prefix a HID Report ID byte to
 * every report even in boot protocol.  hid_report_id_present() walks the
 * report descriptor (fetched with GET_DESCRIPTOR 0x22) so the driver knows
 * to skip the prefix.  These vectors exercise the item walk, truncation
 * safety and long-item skipping. */
static void test_report_id(void)
{
    static const u8 with_id[] = {      /* mouse desc with Report ID 2 */
        0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,
        0x85, 0x02,                    /* Report ID 2 */
        0x09, 0x01, 0xA1, 0x00,
        0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
        0x15, 0x00, 0x25, 0x01,
        0x75, 0x01, 0x95, 0x03,
        0x81, 0x02,
        0xC0, 0xC0,
    };
    static const u8 no_id[] = {        /* plain boot keyboard desc */
        0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
        0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
        0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,
        0x81, 0x02,
        0x75, 0x08, 0x95, 0x06, 0x15, 0x00, 0x25, 0x65,
        0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
        0xC0,
    };
    /* long item: 0xFE, bDataSize=2, bLongTag, 2 data bytes = 5 bytes */
    static const u8 longitem[] = { 0xFE, 0x02, 0xAA, 0xBB, 0xCC,
                                   0x85, 0x01 };
    CHECK(hid_report_id_present(with_id, (int)sizeof(with_id)) == 1,
          "desc with Report ID tag not detected");
    CHECK(hid_report_id_present(no_id, (int)sizeof(no_id)) == 0,
          "plain desc falsely reports IDs");
    CHECK(hid_report_id_present(with_id, 6) == 0,
          "truncated before the ID tag must say no ID");
    CHECK(hid_report_id_present(no_id, 0) == 0, "empty desc");
    CHECK(hid_report_id_present(longitem, (int)sizeof(longitem)) == 1,
          "long-item walk missed the ID tag");
    printf("  report-ID detection: tag walk, truncation + long items\n");
}

/* ===== TEST 4: EP context words exactly as Linux writes them (r31) ====== */
static void test_ep_ctx(void)
{
    struct hid_ep h;
    u32 w[5];
    memset(&h, 0, sizeof h);
    h.ep_mps = 8;
    h.inr = (volatile u32 *)palloc(4096);

    /* FullSpeed, bInterval 1 (the Holtek mouse / Razer kbd case) */
    h.binterval = 1;
    hid_fill_ep_ctx(w, &h, 1);
    CHECK(((w[1] >> 3) & 7u) == 7, "FS: EP type %u, want 7 (INT_IN) - "
          "type 6 was the r30 silent-death bug", (w[1] >> 3) & 7u);
    CHECK(((w[1] >> 1) & 3u) == 3, "FS: CErr %u, want 3", (w[1] >> 1) & 3u);
    CHECK(((w[1] >> 16) & 0xFFFFu) == 8, "FS: MPS %u, want 8",
          (w[1] >> 16) & 0xFFFFu);
    CHECK(((w[0] >> 16) & 0xFFu) == 3, "FS bInt1: interval %u, want 3 "
          "(fls(8*1)-1)", (w[0] >> 16) & 0xFFu);
    CHECK((w[4] >> 16) == 8, "FS: Max ESIT Payload %u, want 8 (= mps)",
          w[4] >> 16);
    CHECK((w[4] & 0xFFFFu) == 8, "FS: Avg TRB Length %u, want 8",
          w[4] & 0xFFFFu);
    CHECK(w[2] == ((u32)(unsigned long)h.inr | 1u), "FS: dequeue|cycle");
    CHECK(w[3] == 0, "FS: dequeue hi");

    /* FullSpeed, bInterval 2 (the Holtek keyboard iface): fls(16)-1 = 4 */
    h.binterval = 2;
    hid_fill_ep_ctx(w, &h, 1);
    CHECK(((w[0] >> 16) & 0xFFu) == 4, "FS bInt2: interval %u, want 4",
          (w[0] >> 16) & 0xFFu);

    /* bInterval 0 (defensive): treated as 1 */
    h.binterval = 0;
    hid_fill_ep_ctx(w, &h, 1);
    CHECK(((w[0] >> 16) & 0xFFu) == 3, "FS bInt0: interval %u, want 3",
          (w[0] >> 16) & 0xFFu);

    /* HighSpeed exponent: Linux clamp(bi,1,16)-1 */
    h.binterval = 1;
    hid_fill_ep_ctx(w, &h, 3);
    CHECK(((w[0] >> 16) & 0xFFu) == 0, "HS bInt1: interval %u, want 0",
          (w[0] >> 16) & 0xFFu);
    h.binterval = 4;
    hid_fill_ep_ctx(w, &h, 3);
    CHECK(((w[0] >> 16) & 0xFFu) == 3, "HS bInt4: interval %u, want 3",
          (w[0] >> 16) & 0xFFu);

    /* SuperSpeed: field = clamp(bi,1,16) */
    h.binterval = 5;
    hid_fill_ep_ctx(w, &h, 4);
    CHECK(((w[0] >> 16) & 0xFFu) == 5, "SS bInt5: interval %u, want 5",
          (w[0] >> 16) & 0xFFu);

    printf("  ep ctx: INT_IN type 7, CErr 3, ESIT/AvgTRB = mps, "
           "FS/HS/SS intervals Linux-exact\n");
}

/* ===== TEST 6: r34 runtime report-offset fallbacks ====================== */
/* r33 field round: the descriptor-based Report ID verdict produced NO
 * observable change on the H510M-A - either the GET_DESCRIPTOR fetch
 * failed or the item walk missed, and no photo was taken to prove which.
 * r34 therefore lets the driver self-correct from the report STREAM:
 *   - boot keyboard: a plain boot report is EXACTLY 8 bytes, so a first
 *     report of 9+ bytes proves the Report ID prefix (length rule);
 *   - boot mouse: mouse_probe() collects byte-position statistics over
 *     60 non-idle reports and flips to the layout the data supports.
 * These vectors drive the REAL proc_events path through sim consumers
 * (like T3) and assert delivered CONTENT, so a disabled or inverted
 * fallback fails loudly. */
static struct sim_consumer *t6_arm(struct xdev *d, u32 slot, int j,
                                   int kind, u32 replen, const u8 *pay)
{
    struct hid_ep *h = &d->hid[j];
    memset(h, 0, sizeof *h);
    h->active = 1;
    h->iface = j;
    h->kind = kind;
    h->ep_addr = (u8)(j + 1);
    h->dci = (u32)(h->ep_addr * 2 + 1);
    h->ep_mps = replen > 8 ? 16 : 8;
    h->binterval = 2;                   /* FS, like the field devices */
    h->has_id = 0;                      /* descriptor evidence "failed" */
    h->inr = (volatile u32 *)palloc(4096);
    for (int b = 0; b < IN_TRBS; b++) {
        h->in_buf[b] = palloc(IN_BUF_BYTES);
        memset(h->in_buf[b], 0, IN_BUF_BYTES);
        memcpy(h->in_buf[b], pay,
               replen < IN_BUF_BYTES ? replen : IN_BUF_BYTES);
    }
    h->inr_idx = 0; h->inr_cycle = 1;
    h->link_pend = 0; h->link_pend_cycle = 1;
    hid_arm_ring(d, j);
    u32 epw[5];
    hid_fill_ep_ctx(epw, h, 1);
    return sim_cfg_ep(epw, slot, h->dci, replen);
}

static void test_runtime_fallbacks(void)
{
    /* 6a: Report-ID-prefixed mouse, descriptor verdict failed.
     * Payload [ID=2][btn=0][dx][dy]: byte 1 quiet, bytes 2-3 active on
     * every report -> the probe must flip to off 1 by report 60 and the
     * remaining reports must deliver the true axes. */
    struct xdev *dm = slot_alloc(3);
    CHECK(dm != NULL, "6a: slot_alloc(3) failed");
    if (dm) {
        dm->nhid = 1; dm->kind = 2;
        static const u8 pay_id[] = { 2, 0, 0x5A, 0x3C };
        struct hid_ep *h = &dm->hid[0];
        struct sim_consumer *c = t6_arm(dm, 3, 0, 2, 4, pay_id);
        CHECK(c != NULL, "6a: sim xHC did not schedule the probe EP");
        if (c) {
            sim_mouse_reports = 0;
            sim_m_btn = 0; sim_m_dx = sim_m_dy = sim_m_wh = 0;
            for (int i = 0; i < 70; i++) {
                sim_consume_td(c); proc_events(); drain_input();
                if (i == 55)
                    CHECK(sim_mouse_reports == 0,
                          "6a: %ld moves injected BEFORE the probe verdict "
                          "(hold broken: wrong-offset garbage would storm "
                          "the WM with phantom drags)", sim_mouse_reports);
            }
            CHECK(h->has_id == 1,
                  "6a: probe left has_id %u after 70 prefixed reports "
                  "(r33 field failure mode: silent wrong offset)",
                  (u32)h->has_id);
            CHECK(sim_mouse_reports == 11,
                  "6a: %ld moves delivered after the verdict (want 11: "
                  "held through report 59; report 60 IS the verdict and "
                  "already injects, then 61-70)", sim_mouse_reports);
            CHECK(sim_m_btn == 0 && sim_m_dx == 0x5A && sim_m_dy == -0x3C,
                  "6a: content wrong after probe: btn %u dx %d dy %d "
                  "(want btn 0 dx 90 dy -60)", sim_m_btn, sim_m_dx, sim_m_dy);
            printf("  6a prefixed mouse: held to verdict, probe flipped "
                   "off 0 -> 1, axes dx %d dy %d delivered\n",
                   sim_m_dx, sim_m_dy);
        }
    }

    /* 6b: PLAIN boot mouse - the probe must NOT flip the offset.
     * Payload [btn=1][dx][dy][wheel=0]: bytes 1-2 active, byte 3 never
     * -> plain layout confirmed, content parsed from byte 0. */
    struct xdev *dp = slot_alloc(4);
    CHECK(dp != NULL, "6b: slot_alloc(4) failed");
    if (dp) {
        dp->nhid = 1; dp->kind = 2;
        static const u8 pay_plain[] = { 0x01, 0x2A, 0x14, 0 };
        struct hid_ep *h = &dp->hid[0];
        struct sim_consumer *c = t6_arm(dp, 4, 0, 2, 4, pay_plain);
        CHECK(c != NULL, "6b: sim xHC did not schedule the plain EP");
        if (c) {
            sim_mouse_reports = 0;
            sim_m_btn = 0; sim_m_dx = sim_m_dy = sim_m_wh = 0;
            for (int i = 0; i < 70; i++) {
                sim_consume_td(c); proc_events(); drain_input();
            }
            CHECK(h->has_id == 0,
                  "6b: probe FALSELY flipped a plain mouse to has_id %u "
                  "(would shift every axis by one)", (u32)h->has_id);
            CHECK(sim_mouse_reports == 11,
                  "6b: %ld moves delivered (want 11: reports 60-70 after "
                  "the plain verdict)", sim_mouse_reports);
            CHECK(sim_m_btn == 1 && sim_m_dx == 0x2A && sim_m_dy == -0x14,
                  "6b: plain content wrong: btn %u dx %d dy %d (want "
                  "btn 1 dx 42 dy -20)", sim_m_btn, sim_m_dx, sim_m_dy);
            printf("  6b plain mouse: probe confirmed off 0, btn %u dx %d "
                   "dy %d delivered\n", sim_m_btn, sim_m_dx, sim_m_dy);
        }
    }

    /* 6c: keyboard whose FIRST report is 9 bytes -> the length rule must
     * flip to off 1 immediately (no 60-report wait) and parse mod/key
     * from behind the ID byte. */
    struct xdev *dk = slot_alloc(5);
    CHECK(dk != NULL, "6c: slot_alloc(5) failed");
    if (dk) {
        dk->nhid = 1; dk->kind = 1;
        static const u8 pay_kbd[] = { 1, 0x02, 0, 0x04, 0, 0, 0, 0, 0 };
        struct hid_ep *h = &dk->hid[0];
        struct sim_consumer *c = t6_arm(dk, 5, 0, 1, 9, pay_kbd);
        CHECK(c != NULL, "6c: sim xHC did not schedule the kbd EP");
        if (c) {
            sim_kbd_reports = 0; sim_k_code = 0; sim_k_ctrl = 0;
            for (int i = 0; i < 10; i++) {
                sim_consume_td(c); proc_events(); drain_input();
            }
            CHECK(h->has_id == 1,
                  "6c: length rule left has_id %u on 9-byte kbd reports "
                  "(r33 field failure: keyboard frozen)", (u32)h->has_id);
            /* payload: [ID=1][mod=LeftShift][rsv][a] -> through the REAL
             * kbd.c this must emerge as the shifted character 'A' */
            CHECK(sim_k_code == 'A' && !sim_k_ctrl,
                  "6c: kbd end-to-end wrong: keycode %u ctrl %u (want "
                  "'A' 0x41 via shift - ID parsed as modifier would give "
                  "keycode 0 / stuck Ctrl)", sim_k_code, sim_k_ctrl);
            printf("  6c 9-byte keyboard: length rule flipped off 0 -> 1 "
                   "on report 1, shift+'a' emerged as '%c'\n",
                   (char)sim_k_code);
        }
    }

    /* 6d (r37): PLAIN 8-byte keyboard reports - the length rule must NOT
     * fire.  The old sim posted rem=0 for every transfer, so the driver
     * saw len=16 even for an 8-byte report and this negative case was
     * literally inexpressible; with true remainders it is a real gate on
     * the exact rule that decides ID-prefixed keyboards. */
    struct xdev *dk2 = slot_alloc(6);
    CHECK(dk2 != NULL, "6d: slot_alloc(6) failed");
    if (dk2) {
        dk2->nhid = 1; dk2->kind = 1;
        static const u8 pay_plain_kbd[] = { 0x02, 0, 0x04, 0, 0, 0, 0, 0 };
        struct hid_ep *h = &dk2->hid[0];
        struct sim_consumer *c = t6_arm(dk2, 6, 0, 1, 8, pay_plain_kbd);
        CHECK(c != NULL, "6d: sim xHC did not schedule the plain kbd EP");
        if (c) {
            sim_kbd_reports = 0; sim_k_code = 0; sim_k_ctrl = 0;
            for (int i = 0; i < 10; i++) {
                sim_consume_td(c); proc_events(); drain_input();
            }
            CHECK(h->has_id == 0,
                  "6d: length rule FALSELY flipped an 8-byte plain kbd to "
                  "has_id %u (every key would parse one byte off)",
                  (u32)h->has_id);
            CHECK(sim_k_code == 'A' && !sim_k_ctrl,
                  "6d: plain kbd content wrong: keycode %u (want 'A' 0x41 "
                  "via shift at off 0)", sim_k_code);
            printf("  6d plain 8-byte keyboard: length rule stayed off, "
                   "'%c' delivered from byte 0\n", (char)sim_k_code);
        }
    }

    /* 6e (r37): 16-bit-axis mouse with NO usable descriptor - the
     * statistical probe must detect the wide layout from the report
     * stream itself (Model C, the r36 field fingerprint).  The device
     * already has has_id=1 (descriptor walk found an ID but the layout
     * parse failed): the probe still runs, collecting X-high-byte
     * evidence, and flips extraction to 16-bit LE pairs at the verdict.
     * Report: [ID=1][btn=0][XL=FB][XH=FF][YL=06][YH=00] = dx -5, dy +6
     * -> queue (-5, -6).  8-bit boot parsing of the same bytes gives
     * dy = (i8)XH = -1 -> flipped = +1: the constant up-drift the user
     * reported ("left goes diagonally UP-left"). */
    struct xdev *dw = slot_alloc(10);
    CHECK(dw != NULL, "6e: slot_alloc(10) failed");
    if (dw) {
        dw->nhid = 1; dw->kind = 2;
        static const u8 pay_wide[] = { 1, 0, 0xFB, 0xFF, 0x06, 0x00 };
        struct hid_ep *h = &dw->hid[0];
        struct sim_consumer *c = t6_arm(dw, 10, 0, 2, 6, pay_wide);
        CHECK(c != NULL, "6e: sim xHC did not schedule the wide16 EP");
        if (c) {
            h->has_id = 1;              /* descriptor said ID, parse failed */
            sim_mouse_reports = 0;
            sim_m_btn = 0; sim_m_dx = 0; sim_m_dy = 0; sim_m_wh = 0;
            for (int i = 0; i < 70; i++) {
                sim_consume_td(c); proc_events(); drain_input();
                if (i == 30)
                    CHECK(h->wide16 == 0,
                          "6e: wide16 verdict fired at report %d (want it "
                          "only at the probe verdict)", i);
            }
            CHECK(h->wide16 == 1,
                  "6e: probe never detected 16-bit axes (xh00ff %u len6 %u "
                  "nz4 %u nz5 %u of n %u)", (u32)h->xh00ff, (u32)h->len6,
                  (u32)h->nz[3], (u32)h->nz[4], (u32)h->probe_n);
            CHECK(sim_m_dx == -5 && sim_m_dy == -6,
                  "6e: wide16 content wrong: dx %d dy %d (want -5, -6 "
                  "with the r41 unity gain; dy +2 would be the r36 "
                  "X-high-byte up-drift)", sim_m_dx, sim_m_dy);
            printf("  6e 16-bit probe fallback: verdict at 60, dx %d dy %d "
                   "delivered (no descriptor needed)\n",
                   sim_m_dx, sim_m_dy);
        }
    }
}

/* ===== TEST 8: r37 report-descriptor layout parser + extraction ========= */
/* The r36 field fingerprint: physical LEFT moved the cursor diagonally
 * UP-left, RIGHT moved straight right, DOWN produced no vertical motion
 * at all (only X jitter), UP drifted sideways depending on angle.  That
 * is the exact signature of a 16-bit-axis REPORT-protocol mouse
 * ([ID][btn][X lo][X hi][Y lo][Y hi]) parsed as 8-bit boot data:
 * dx = X-lo (right looked right), dy = X-HI (0x00 moving right, 0xFF
 * moving left = constant up-drift), real Y never reaching an axis.  No
 * byte-offset probe can fix a FIELD-SIZE mismatch - the cure is the
 * Linux model: parse the descriptor into a bit-accurate field map.
 * These vectors replay a 16-bit gaming mouse, an NKRO keyboard and a
 * plain boot keyboard through the REAL parse + extract + queue chain. */

/* 16-bit gaming mouse, Report ID 1:
 * report = [ID][btn 3 bits + 5 pad][X 16][Y 16][wheel 8] = 7 bytes */
static const u8 rdesc_mouse16[] = {
    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x02,             /* Usage (Mouse) */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, 0x01,             /*   Report ID (1) */
    0x05, 0x09,             /*   Usage Page (Button) */
    0x19, 0x01, 0x29, 0x03, /*   Usage Min 1 / Max 3 */
    0x15, 0x00, 0x25, 0x01, /*   LogMin 0 / LogMax 1 */
    0x75, 0x01, 0x95, 0x03, /*   Report Size 1, Count 3 */
    0x81, 0x02,             /*   Input (Data,Var,Abs)  - buttons */
    0x75, 0x05, 0x95, 0x01, /*   Report Size 5, Count 1 */
    0x81, 0x03,             /*   Input (Const,Var,Abs) - padding */
    0x05, 0x01,             /*   Usage Page (Generic Desktop) */
    0x09, 0x01,             /*   Usage (Pointer) */
    0xA1, 0x00,             /*   Collection (Physical) */
    0x16, 0x00, 0x80,       /*     LogMin -32768 */
    0x26, 0xFF, 0x7F,       /*     LogMax 32767 */
    0x75, 0x10, 0x95, 0x02, /*     Report Size 16, Count 2 */
    0x09, 0x30, 0x09, 0x31, /*     Usage X, Usage Y */
    0x81, 0x06,             /*     Input (Data,Var,Rel) - X, Y */
    0xC0,                   /*   End Collection */
    0x09, 0x38,             /*   Usage (Wheel) */
    0x15, 0x81, 0x25, 0x7F, /*   LogMin -127 / LogMax 127 */
    0x75, 0x08, 0x95, 0x01, /*   Report Size 8, Count 1 */
    0x81, 0x06,             /*   Input (Data,Var,Rel)  - wheel */
    0xC0                    /* End Collection */
};

/* NKRO keyboard, Report ID 2:
 * report = [ID][mod 8][rsv 8][bitmap 104] = 16 bytes */
static const u8 rdesc_nkro[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,   /* GD / Keyboard / Application */
    0x85, 0x02,                            /* Report ID 2 */
    0x05, 0x07,                            /* Usage Page (Keyboard) */
    0x19, 0xE0, 0x29, 0xE7,                /* Usage Min E0 / Max E7 */
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08,                /* Size 1, Count 8 */
    0x81, 0x02,                            /* Input (Var) - modifiers */
    0x75, 0x08, 0x95, 0x01,                /* Size 8, Count 1 */
    0x81, 0x03,                            /* Input (Const) - reserved */
    0x19, 0x00, 0x29, 0x67,                /* Usage Min 0 / Max 0x67 */
    0x75, 0x01, 0x95, 0x68,                /* Size 1, Count 104 */
    0x81, 0x02,                            /* Input (Var) - NKRO bitmap */
    0xC0
};

/* plain boot keyboard (no Report IDs): [mod 8][rsv 8][keys 6x8] -
 * the canonical 8-byte-boot-kbd descriptor INCLUDING the reserved byte */
static const u8 rdesc_bootkbd[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,
    0x81, 0x02,
    0x95, 0x01, 0x75, 0x08,             /* reserved byte (constant) */
    0x81, 0x03,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0
};

/* consumer-control only: no mouse or keyboard fields - must NOT parse */
static const u8 rdesc_consumer[] = {
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,
    0x81, 0x02, 0xC0
};

static struct sim_consumer *t8_arm(struct xdev *d, u32 slot, int j,
                                   int kind, const struct hid_layout *lay,
                                   u32 replen, const u8 *pay)
{
    struct hid_ep *h = &d->hid[j];
    memset(h, 0, sizeof *h);
    h->active = 1;
    h->iface = j;
    h->kind = kind;
    h->ep_addr = (u8)(j + 1);
    h->dci = (u32)(h->ep_addr * 2 + 1);
    h->ep_mps = replen > 8 ? 16 : 8;
    h->binterval = 1;
    h->use_layout = 1;
    h->lay = *lay;
    h->inr = (volatile u32 *)palloc(4096);
    for (int b = 0; b < IN_TRBS; b++) {
        h->in_buf[b] = palloc(IN_BUF_BYTES);
        memset(h->in_buf[b], 0, IN_BUF_BYTES);
        memcpy(h->in_buf[b], pay,
               replen < IN_BUF_BYTES ? replen : IN_BUF_BYTES);
    }
    h->inr_idx = 0; h->inr_cycle = 1;
    h->link_pend = 0; h->link_pend_cycle = 1;
    hid_arm_ring(d, j);
    u32 epw[5];
    hid_fill_ep_ctx(epw, h, 1);
    return sim_cfg_ep(epw, slot, h->dci, replen);
}

/* refill every ring buffer with a new report + retune the consumer's
 * simulated report length (the ring TRBs keep pointing at in_buf[b]) */
static void t8_refill(struct xdev *d, int j, struct sim_consumer *c,
                      u32 replen, const u8 *pay)
{
    struct hid_ep *h = &d->hid[j];
    for (int b = 0; b < IN_TRBS; b++) {
        memset(h->in_buf[b], 0, IN_BUF_BYTES);
        memcpy(h->in_buf[b], pay,
               replen < IN_BUF_BYTES ? replen : IN_BUF_BYTES);
    }
    c->report_len = replen;
}

static void t8_pump(struct sim_consumer *c, int n)
{
    for (int i = 0; i < n; i++) {
        sim_consume_td(c); proc_events(); drain_input();
    }
}

static void test_layout_parser(void)
{
    /* ---- 8a: bit extraction primitives ---- */
    {
        static const u8 pat[] = { 0xF0, 0x0F, 0x00, 0x00 };
        CHECK(hid_bits(pat, 4, 4, 8) == 0xFF,
              "8a: cross-byte bit extract got %02x want ff",
              hid_bits(pat, 4, 4, 8));
        CHECK(hid_signed(0xFF, 8) == -1, "8a: 8-bit sign extend");
        CHECK(hid_signed(0x80, 8) == -128, "8a: 8-bit most negative");
        CHECK(hid_signed(0xFFF, 12) == -1, "8a: 12-bit sign extend");
        CHECK(hid_signed(0x012C, 16) == 300, "8a: 16-bit positive");
        CHECK(hid_signed(0xFF88, 16) == -120, "8a: 16-bit negative");
    }

    /* ---- 8b: parse the 16-bit gaming mouse descriptor ---- */
    struct hid_layout m16;
    memset(&m16, 0, sizeof m16);
    CHECK(hid_parse_layout(rdesc_mouse16, (int)sizeof rdesc_mouse16, 2,
                           &m16) == 1,
          "8b: 16-bit mouse descriptor did not parse");
    CHECK(m16.rid == 1, "8b: mouse rid %u want 1", m16.rid);
    CHECK(m16.btn_off == 0 && m16.btn_cnt == 3,
          "8b: buttons @%u/%u want @0/3", m16.btn_off, m16.btn_cnt);
    CHECK(m16.x_off == 8 && m16.x_sz == 16,
          "8b: X @%u/%u want @8/16 - THE r36 field bug was parsing this "
          "as an 8-bit byte", m16.x_off, m16.x_sz);
    CHECK(m16.y_off == 24 && m16.y_sz == 16,
          "8b: Y @%u/%u want @24/16", m16.y_off, m16.y_sz);
    CHECK(m16.w_off == 40 && m16.w_sz == 8,
          "8b: wheel @%u/%u want @40/8", m16.w_off, m16.w_sz);
    CHECK(m16.rpt_bits == 48, "8b: rpt %u bits want 48", m16.rpt_bits);
    CHECK(hid_parse_layout(rdesc_mouse16, (int)sizeof rdesc_mouse16, 1,
                           &m16) == 0,
          "8b: mouse descriptor falsely parsed as a keyboard");

    /* ---- 8c: NKRO keyboard descriptor ---- */
    struct hid_layout nk;
    memset(&nk, 0, sizeof nk);
    CHECK(hid_parse_layout(rdesc_nkro, (int)sizeof rdesc_nkro, 1, &nk) == 1,
          "8c: NKRO descriptor did not parse");
    CHECK(nk.rid == 2 && nk.mod_off == 0 && nk.mod_sz == 8,
          "8c: NKRO rid %u mod @%u/%u want rid 2 @0/8",
          nk.rid, nk.mod_off, nk.mod_sz);
    CHECK(nk.key_off == 16 && nk.key_sz == 1 && nk.key_cnt == 104,
          "8c: NKRO bitmap @%u sz %u cnt %u want @16/1/104",
          nk.key_off, nk.key_sz, nk.key_cnt);
    CHECK(nk.rpt_bits == 120, "8c: NKRO rpt %u bits want 120", nk.rpt_bits);

    /* ---- 8d: plain boot keyboard descriptor ---- */
    struct hid_layout bk;
    memset(&bk, 0, sizeof bk);
    CHECK(hid_parse_layout(rdesc_bootkbd, (int)sizeof rdesc_bootkbd, 1,
                           &bk) == 1,
          "8d: plain boot kbd descriptor did not parse");
    CHECK(bk.rid == 0 && bk.mod_off == 0 && bk.mod_sz == 8 &&
          bk.key_off == 16 && bk.key_sz == 8 && bk.key_cnt == 6,
          "8d: boot kbd layout rid %u mod @%u/%u keys @%u/%ux%u want "
          "rid 0 @0/8 @16/8x6", bk.rid, bk.mod_off, bk.mod_sz,
          bk.key_off, bk.key_sz, bk.key_cnt);

    /* ---- 8e: consumer-control-only descriptor must NOT parse ---- */
    {
        struct hid_layout junk;
        CHECK(hid_parse_layout(rdesc_consumer, (int)sizeof rdesc_consumer,
                               2, &junk) == 0,
              "8e: consumer-control descriptor falsely parsed as a mouse");
        CHECK(hid_parse_layout(rdesc_consumer, (int)sizeof rdesc_consumer,
                               1, &junk) == 0,
              "8e: consumer-control descriptor falsely parsed as a kbd");
        CHECK(hid_parse_layout(rdesc_mouse16, 6, 2, &junk) == 0,
              "8e: truncated descriptor must not parse");
    }

    /* ---- 8f: END-TO-END 16-bit mouse - the r36 field fingerprint ---- */
    struct xdev *dm = slot_alloc(7);
    CHECK(dm != NULL, "8f: slot_alloc(7) failed");
    if (dm) {
        dm->nhid = 1; dm->kind = 2;
        static const u8 pay_left[] = { 1, 0, 0xFB, 0xFF, 0, 0, 0 };
        struct hid_ep *h = &dm->hid[0];
        struct sim_consumer *c = t8_arm(dm, 7, 0, 2, &m16, 7, pay_left);
        CHECK(c != NULL, "8f: sim xHC did not schedule the layout EP");
        if (c) {
            /* physical LEFT, dx=-5 dy=0: X=0xFFFB LE {FB FF}.  Boot
             * parsing saw dy = X-hi = 0xFF = -1 -> flipped = +1 = the
             * constant UP-drift the user reported. */
            sim_mouse_reports = 0;
            sim_m_btn = 0; sim_m_dx = 12345; sim_m_dy = 12345; sim_m_wh = 0;
            t8_pump(c, 1);
            CHECK(sim_m_dx == -5 && sim_m_dy == 0,
                  "8f: physical left dx -5 gave dx %d dy %d (want -5 with "
                  "the r41 unity gain, 0 - dy nonzero is the r36 "
                  "up-drift bug)", sim_m_dx, sim_m_dy);

            /* physical DOWN, dy=+6 -> queue dy = -6 (r35 convention:
             * positive queue dy = UP).  r36 field: "cannot move down at
             * all" - boot parsing read Y-lo from the X-hi slot = 0. */
            static const u8 pay_down[] = { 1, 0, 0, 0, 6, 0, 0 };
            t8_refill(dm, 0, c, 7, pay_down);
            sim_m_dx = 12345; sim_m_dy = 12345;
            t8_pump(c, 1);
            CHECK(sim_m_dx == 0 && sim_m_dy == -6,
                  "8f: physical down gave dx %d dy %d (want 0, -6 with "
                  "the r41 unity gain - the r36 dead-vertical bug)",
                  sim_m_dx, sim_m_dy);

            /* diagonal: dx=+300 (0x012C), dy=-120 (0xFF88) -> queue
             * (300, +120) */
            static const u8 pay_diag[] = { 1, 0, 0x2C, 0x01, 0x88, 0xFF, 0 };
            t8_refill(dm, 0, c, 7, pay_diag);
            sim_m_dx = 0; sim_m_dy = 0;
            t8_pump(c, 1);
            CHECK(sim_m_dx == 300 && sim_m_dy == 120,
                  "8f: diagonal 16-bit move gave dx %d dy %d (want 300, "
                  "120 with r41 unity gain - beyond the 8-bit range "
                  "entirely)", sim_m_dx, sim_m_dy);

            /* wheel -2 */
            static const u8 pay_wheel[] = { 1, 0, 0, 0, 0, 0, 0xFE };
            t8_refill(dm, 0, c, 7, pay_wheel);
            sim_m_wh = 0;
            t8_pump(c, 1);
            CHECK(sim_m_wh == -2, "8f: wheel got %d want -2", sim_m_wh);

            /* foreign Report ID (consumer-control traffic on the shared
             * EP) must be DROPPED, never misparsed as a move */
            static const u8 pay_foreign[] = { 2, 0x55, 0x66, 0x77 };
            t8_refill(dm, 0, c, 4, pay_foreign);
            long before = sim_mouse_reports;
            sim_m_dx = 0; sim_m_dy = 0;
            t8_pump(c, 2);
            CHECK(sim_mouse_reports == before && sim_m_dx == 0 &&
                  sim_m_dy == 0,
                  "8f: foreign report id 2 injected a move (dx %d dy %d) "
                  "- shared-EP consumer traffic must be dropped",
                  sim_m_dx, sim_m_dy);
            CHECK(h->rid_skip_logged >= 1,
                  "8f: foreign-ID drop was not logged (photo diagnostics)");

            /* button press with the layout: btn bit 1 (right) */
            static const u8 pay_btn[] = { 1, 0x02, 0, 0, 0, 0, 0 };
            t8_refill(dm, 0, c, 7, pay_btn);
            sim_m_btn = 0;
            t8_pump(c, 1);
            CHECK(sim_m_btn == 2, "8f: layout buttons got %u want 2",
                  (u32)sim_m_btn);
            printf("  8f 16-bit mouse end-to-end (r41 unity gain): left(-5,"
                   "0) down(0,-6) diag(300,120) wheel -2 foreign-ID "
                   "dropped btn 2\n");
        }
    }

    /* ---- 8g: END-TO-END NKRO keyboard through the layout path ---- */
    struct xdev *dk = slot_alloc(8);
    CHECK(dk != NULL, "8g: slot_alloc(8) failed");
    if (dk) {
        dk->nhid = 1; dk->kind = 1;
        /* [ID=2][mod 0][rsv 0][bitmap: bit 4 = usage 4 = 'a'] */
        static const u8 pay_a[] = { 2, 0, 0, 0x10, 0, 0, 0, 0,
                                    0, 0, 0, 0, 0, 0, 0, 0 };
        struct sim_consumer *c = t8_arm(dk, 8, 0, 1, &nk, 16, pay_a);
        CHECK(c != NULL, "8g: sim xHC did not schedule the NKRO EP");
        if (c) {
            static const u8 pay_up[] = { 2, 0, 0, 0, 0, 0, 0, 0,
                                         0, 0, 0, 0, 0, 0, 0, 0 };
            sim_kbd_reports = 0; sim_k_code = 0; sim_k_ctrl = 0;
            t8_pump(c, 1);
            CHECK(sim_k_code == 'a' && sim_k_pressed && !sim_k_ctrl,
                  "8g: NKRO bitmap 'a' got keycode %u pressed %u ctrl %u",
                  sim_k_code, sim_k_pressed, sim_k_ctrl);
            /* all up: plain-letter breaks are NOT enqueued by kbd.c
             * design (T7 owns release semantics) - the contract here is
             * that the layout path produces NO spurious events */
            {
                long before = sim_kbd_reports;
                t8_refill(dk, 0, c, 16, pay_up);
                t8_pump(c, 1);
                CHECK(sim_kbd_reports == before,
                      "8g: NKRO all-up emitted %ld spurious events (want "
                      "0 - letter breaks are by-design silent)",
                      sim_kbd_reports - before);
            }
            /* shift + 'a' in ONE report -> 'A' (modifier field + bitmap
             * bit 4; single-report semantics exactly like T6c - a mod
             * change WHILE a key is held legitimately emits the raw
             * shift scancode event by kbd.c design and is not part of
             * the layout contract) */
            static const u8 pay_A[] = { 2, 0x02, 0, 0x10, 0, 0, 0, 0,
                                        0, 0, 0, 0, 0, 0, 0, 0 };
            t8_refill(dk, 0, c, 16, pay_A);
            t8_pump(c, 1);
            CHECK(sim_k_code == 'A' && sim_k_pressed,
                  "8g: NKRO shift+a got keycode %u want 'A'", sim_k_code);
            /* all up again: the shift BREAK is enqueued (modifier), so
             * the last event must be a release */
            t8_refill(dk, 0, c, 16, pay_up);
            t8_pump(c, 1);
            CHECK(!sim_k_pressed, "8g: NKRO all-up after shift left a key "
                  "pressed (the shift break must enqueue)");
            /* 'd' alone (bitmap bit 7 -> usage 7) */
            static const u8 pay_d[] = { 2, 0, 0, 0x80, 0, 0, 0, 0,
                                        0, 0, 0, 0, 0, 0, 0, 0 };
            t8_refill(dk, 0, c, 16, pay_d);
            t8_pump(c, 1);
            CHECK(sim_k_code == 'd' && sim_k_pressed,
                  "8g: NKRO 'd' got keycode %u want 'd' make", sim_k_code);
            {
                long before = sim_kbd_reports;
                t8_refill(dk, 0, c, 16, pay_up);
                t8_pump(c, 1);
                CHECK(sim_kbd_reports == before,
                      "8g: NKRO final all-up emitted %ld spurious events "
                      "(want 0)", sim_kbd_reports - before);
            }
            printf("  8g NKRO keyboard end-to-end: 'a', all-up, shift "
                   "'A', 'd' through the bitmap layout\n");
        }
    }

    /* ---- 8h: plain boot keyboard through the LAYOUT path (rid 0) ---- */
    struct xdev *db = slot_alloc(9);
    CHECK(db != NULL, "8h: slot_alloc(9) failed");
    if (db) {
        db->nhid = 1; db->kind = 1;
        static const u8 pay_bk[] = { 0x02, 0, 0x04, 0, 0, 0, 0, 0 };
        struct sim_consumer *c = t8_arm(db, 9, 0, 1, &bk, 8, pay_bk);
        CHECK(c != NULL, "8h: sim xHC did not schedule the boot kbd EP");
        if (c) {
            sim_kbd_reports = 0; sim_k_code = 0; sim_k_ctrl = 0;
            t8_pump(c, 1);
            CHECK(sim_k_code == 'A' && !sim_k_ctrl,
                  "8h: boot kbd via layout got keycode %u ctrl %u (want "
                  "'A' - the layout path must serve classic reports too)",
                  sim_k_code, sim_k_ctrl);
            printf("  8h plain boot keyboard via layout path: shift+'a' "
                   "-> '%c'\n", (char)sim_k_code);
        }
    }

    /* ---- 8i: the r37 FIELD KEYBOARD BUG - hidden Report ID prefix ----
     * Both field keyboards parse as rid 0 (their Report ID item belongs
     * to the LED OUTPUT report), yet they prefix EVERY report with the
     * ID byte on the wire (r35 proved it: the boot path with off=1
     * typed).  r37 parsed the ID byte as the modifier bitmap: ID 1 =
     * LeftCtrl = a permanent stuck Ctrl -> every letter became an
     * invisible control character -> "keyboard no response, just power".
     * The layout path must strip the hidden prefix, decided per report
     * from the length evidence (9 bytes arrived, layout says 64 bits). */
    struct xdev *dh = slot_alloc(11);
    CHECK(dh != NULL, "8i: slot_alloc(11) failed");
    if (dh) {
        /* boot-kbd inputs with NO input Report ID + an LED output that
         * DOES define one - the exact shape of the field descriptors */
        static const u8 rdesc_hidkbd[] = {
            0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
            0x05, 0x07,
            0x19, 0xE0, 0x29, 0xE7,
            0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,
            0x81, 0x02,                    /* modifiers */
            0x95, 0x01, 0x75, 0x08,
            0x81, 0x03,                    /* reserved */
            0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
            0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
            0x81, 0x00,                    /* keys 6x8 */
            0x85, 0x01,                    /* Report ID 1 - OUTPUT only */
            0x95, 0x05, 0x75, 0x01,
            0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
            0x91, 0x02,                    /* LED output */
            0x95, 0x01, 0x75, 0x03,
            0x91, 0x03,
            0xC0
        };
        struct hid_layout hk;
        memset(&hk, 0, sizeof hk);
        CHECK(hid_parse_layout(rdesc_hidkbd, (int)sizeof rdesc_hidkbd, 1,
                               &hk) == 1,
              "8i: hidden-ID kbd descriptor did not parse");
        CHECK(hk.rid == 0 && hk.saw_rid == 1,
              "8i: layout rid %u saw_rid %u want 0/1 (the ID belongs to "
              "the output report)", hk.rid, hk.saw_rid);
        CHECK(hk.rpt_bits == 64 && hk.key_off == 16 && hk.key_sz == 8,
              "8i: layout bits %u keys @%u/%u want 64 @16/8",
              hk.rpt_bits, hk.key_off, hk.key_sz);
        dh->nhid = 1; dh->kind = 1;
        /* wire report: [ID=1][mod=LeftShift][rsv][a] = 9 bytes */
        static const u8 pay_hid[] = { 1, 0x02, 0, 0x04, 0, 0, 0, 0, 0 };
        struct hid_ep *h = &dh->hid[0];
        struct sim_consumer *c = t8_arm(dh, 11, 0, 1, &hk, 9, pay_hid);
        CHECK(c != NULL, "8i: sim xHC did not schedule the hidden-ID EP");
        if (c) {
            static const u8 pay_up9[] = { 1, 0, 0, 0, 0, 0, 0, 0, 0 };
            sim_kbd_reports = 0; sim_k_code = 0; sim_k_ctrl = 0;
            t8_pump(c, 1);
            CHECK(sim_k_code == 'A' && sim_k_pressed && !sim_k_ctrl,
                  "8i: hidden-prefix kbd got keycode %u ctrl %u (want 'A' "
                  "with NO stuck Ctrl - r37 read the ID byte as the "
                  "modifier: Ctrl stuck forever, letters invisible)",
                  sim_k_code, sim_k_ctrl);
            CHECK(h->hid_prefix_logged == 1,
                  "8i: hidden-prefix strip was not logged (photo diag)");
            /* all up (9-byte) */
            t8_refill(dh, 0, c, 9, pay_up9);
            t8_pump(c, 1);
            /* same layout must ALSO serve an unprefixed 8-byte report:
             * the strip is per-report length evidence, not a mode */
            static const u8 pay_8[] = { 0, 0, 0x04, 0, 0, 0, 0, 0 };
            t8_refill(dh, 0, c, 8, pay_8);
            t8_pump(c, 1);
            CHECK(sim_k_code == 'a' && sim_k_pressed && !sim_k_ctrl,
                  "8i: 8-byte unprefixed report got keycode %u ctrl %u "
                  "(want 'a' - per-report adaptation broken)",
                  sim_k_code, sim_k_ctrl);
            printf("  8i hidden Report-ID keyboard: 9-byte wire report "
                   "-> '%c' no stuck Ctrl; 8-byte report -> '%c'\n",
                   'A', (char)sim_k_code);
        }
    }
}

/* ===== TEST 9: r38 move coalescing - button events survive floods ======= */
/* The r37 field cluster: "have to spam click for it to pick up", "double
 * click doesn't pick up", "if something is still highlighted I can't
 * interact until I click somewhere with no button".  A 1000 Hz mouse
 * floods the 128-slot event queue between the WM's 100 Hz drains, and
 * mouse_enqueue DROPS whatever arrives at a full queue - button
 * transitions (especially the RELEASE that ends a drag) were evicted by
 * move floods: stuck drag state, dead clicks.  Coalescing pending moves
 * into the queued tail makes a move flood occupy ONE slot. */
static void test_move_coalescing(void)
{
    struct mouse_event me;
    /* sync the persistent button state and drain leftovers */
    mouse_apply(0, 0, 0, 0);
    while (mouse_poll(&me)) { }

    /* 9a: two back-to-back moves coalesce into one summed event */
    mouse_apply(0, 10, -3, 0);
    mouse_apply(0, 5, 7, 0);
    int n = 0;
    i32 dx = 0, dy = 0;
    while (mouse_poll(&me)) { n++; dx += me.dx; dy += me.dy; }
    CHECK(n == 1, "9a: two back-to-back moves produced %d events (want 1 "
          "coalesced)", n);
    CHECK(dx == 15 && dy == 4, "9a: coalesced delta %d,%d want 15,4",
          dx, dy);

    /* 9b: a button event between moves breaks the merge chain - order
     * and separation must survive */
    mouse_apply(0, 1, 1, 0);
    mouse_apply(1, 0, 0, 0);            /* button 1 down */
    mouse_apply(1, 2, 2, 0);            /* still held: move only */
    n = 0;
    int t0 = -1, t1 = -1, t2 = -1;
    while (mouse_poll(&me) && n < 4) {
        if (n == 0) t0 = me.type;
        else if (n == 1) t1 = me.type;
        else if (n == 2) t2 = me.type;
        n++;
    }
    CHECK(n == 3 && t0 == MEV_MOVE && t1 == MEV_BUTTON && t2 == MEV_MOVE,
          "9b: move/button/move produced %d events types %d,%d,%d (want "
          "3: MOVE,BUTTON,MOVE)", n, t0, t1, t2);
    while (mouse_poll(&me)) { }         /* drain 9b events */
    mouse_apply(0, 0, 0, 0);            /* release the held button */
    while (mouse_poll(&me)) { }

    /* 9c: THE field regression - a 200-move drag flood must not evict
     * the button transitions bracketing it (queue is 128 slots: without
     * coalescing the RELEASE is dropped and the WM sticks in drag mode) */
    mouse_apply(1, 0, 0, 0);            /* press */
    for (int i = 0; i < 200; i++) mouse_apply(1, 3, 0, 0);
    mouse_apply(0, 0, 0, 0);            /* RELEASE - must survive */
    int saw_down = 0, saw_up = 0;
    i32 tot = 0;
    while (mouse_poll(&me)) {
        if (me.type == MEV_BUTTON && me.down) saw_down = 1;
        if (me.type == MEV_BUTTON && !me.down) saw_up = 1;
        if (me.type == MEV_MOVE) tot += me.dx;
    }
    CHECK(saw_down && saw_up,
          "9c: button transition LOST in a 200-move flood (down %d up %d) "
          "- this is the r37 stuck-drag / dead-click field bug",
          saw_down, saw_up);
    CHECK(tot == 600, "9c: flood deltas summed to %d want 600 (pixel-"
          "exact trajectory through coalescing)", tot);
    printf("  9 move coalescing: 2 moves -> 1 event (15,4); button "
           "order kept; 200-move flood + release fully survived\n");
}

/* ===== TEST 10: r38 ACPI _S5_ package walk ============================== */
static void test_acpi_s5(void)
{
    /* real-world shape of Name(\_S5_, Package(4){5,0,0,0}):
     *   08 5C 5F 53 35 5F 12 PkgLen NumEl 0A 05 ...
     * The NameOp 0x08 sits BEFORE the nameseg; right after "_S5_" comes
     * the PackageOp 0x12.  The pre-r38 walk demanded 0x08 at i+4 - a
     * byte pattern no real DSDT produces - so shutdown on hardware
     * always wrote SLP_TYP 0, which firmware ignores. */
    static const u8 d1[] = {
        0x08, 0x5C, 0x5F, 0x53, 0x35, 0x5F,      /* Name(\_S5_ */
        0x12, 0x0A, 0x04,                        /* Package, len 10, 4 el */
        0x0A, 0x05, 0x00, 0x00, 0x00, 0x00,      /* {5, 0, 0, 0} */
    };
    slp_typa = 0;
    CHECK(parse_s5(d1, (u32)sizeof d1) == 1,
          "10a: root-prefixed Name(_S5_) package not found (the r37 "
          "field shutdown bug)");
    CHECK(slp_typa == 5,
          "10a: SLP_TYPa %u want 5 (SLP_TYP 0 = firmware ignores the S5 "
          "write = 'safe to turn off' screen)", slp_typa);

    /* scoped, no root char, WordConst first element */
    static const u8 d2[] = {
        0x5B, 0x80,                              /* Scope( */
        0x08, 0x5F, 0x53, 0x35, 0x5F,            /* Name(_S5_ */
        0x12, 0x0B, 0x04,                        /* Package len 11 */
        0x0B, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00 /* {7, ...} word */
    };
    slp_typa = 0;
    CHECK(parse_s5(d2, (u32)sizeof d2) == 1, "10b: scoped _S5_ not found");
    CHECK(slp_typa == 7, "10b: SLP_TYPa %u want 7 (WordConst decode)",
          slp_typa);

    /* a different sleep state must NOT match */
    static const u8 d3[] = {
        0x08, 0x5F, 0x53, 0x30, 0x5F,            /* Name(_S0_ */
        0x12, 0x04, 0x02, 0x0A, 0x01
    };
    slp_typa = 0;
    CHECK(parse_s5(d3, (u32)sizeof d3) == 0,
          "10c: _S0_ package falsely matched as _S5_");

    /* the OLD (broken) pattern - "_S5_" followed by a NameOp byte, which
     * only appears in hand-written test vectors, not real AML - is no
     * longer required; a lone nameseg with no NameOp prefix must not
     * match (guards against random data) */
    static const u8 d4[] = { 0x5F, 0x53, 0x35, 0x5F, 0x12, 0x03, 0x01, 0x05 };
    slp_typa = 0;
    CHECK(parse_s5(d4, (u32)sizeof d4) == 0,
          "10d: nameseg without a NameOp prefix falsely matched");
    printf("  10 ACPI _S5_: root-prefixed + scoped packages decode "
           "SLP_TYPa 5/7; _S0_ and bare namesegs rejected\n");
}

/* ===== TEST 7: axis + key semantics through the REAL drivers (r35) ====== */
/* The r34 field report: "moving the physical mouse to the left made it
 * go diagonally down to left", "no way to move the mouse up", "stuck
 * under the screen".  Root cause: HID dy (positive = physical DOWN) was
 * handed to a queue/WM built on the PS/2 convention (positive = UP) -
 * the vertical axis was inverted, and because the WM clamps to the
 * screen the cursor ended up pinned to the bottom edge.  These checks
 * run the REAL mouse_inject -> mouse_apply -> mouse_poll and
 * kbd_inject_hid -> kbd_sc -> kbd_poll chains and apply the WM's own
 * update formula (wm.c: my -= dy * sens / 3, sens default 3). */
/* poll until a MOVE event arrives, discarding button/wheel events (the
 * queue may hold a leftover button-change from the previous test's
 * constant button state) */
static int t7_poll_move(struct mouse_event *out)
{
    struct mouse_event me;
    int found = 0;
    while (mouse_poll(&me))
        if (me.type == MEV_MOVE) { *out = me; found = 1; }
    return found;
}

static void test_direction_matrix(void)
{
    struct mouse_event me;
    struct key_event ke;
    { struct mouse_event d; while (mouse_poll(&d)); }   /* drain leftovers */
    while (kbd_poll(&ke));

    /* physical DOWN: HID dy = +10 -> queue dy must be -10 -> WM
     * my -= (-10) => screen y GROWS => cursor moves down */
    memset(&me, 0, sizeof me);
    mouse_inject(0, 0, 10, 0);
    int got = t7_poll_move(&me);
    CHECK(got && me.type == MEV_MOVE && me.dy == -10,
          "T7: HID dy +10 (physical DOWN) arrived as queue dy %d - want "
          "-10 (r34 field bug: vertical axis inverted)", me.dy);
    CHECK(got && -(int)me.dy * 3 / 3 > 0,
          "T7: physical down must grow screen y (WM formula my -= dy)");

    /* physical UP: HID dy = -10 -> queue dy = +10 -> screen y shrinks */
    memset(&me, 0, sizeof me);
    mouse_inject(0, 0, -10, 0);
    got = t7_poll_move(&me);
    CHECK(got && me.dy == 10,
          "T7: HID dy -10 (physical UP) -> queue dy %d, want +10", me.dy);
    CHECK(got && -(int)me.dy * 3 / 3 < 0,
          "T7: physical up must shrink screen y");

    /* horizontal passes through unflipped */
    memset(&me, 0, sizeof me);
    mouse_inject(0, 7, 0, 0);
    got = t7_poll_move(&me);
    CHECK(got && me.dx == 7 && me.dy == 0,
          "T7: horizontal wrong: dx %d dy %d (want 7, 0)", me.dx, me.dy);

    /* physical LEFT: dx = -7 -> screen x must shrink */
    memset(&me, 0, sizeof me);
    mouse_inject(0, -7, 0, 0);
    got = t7_poll_move(&me);
    CHECK(got && me.dx == -7,
          "T7: physical left -> queue dx %d, want -7", me.dx);

    /* wheel signs: HID wheel +1 (scroll up) and -1 (scroll down) must
     * arrive as MEV_WHEEL with the SAME signs (Linux maps both PS/2 and
     * USB wheels to REL_WHEEL unflipped) - r36 field report: "downward
     * scrolling doesn't work on anything" */
    memset(&me, 0, sizeof me);
    mouse_inject(0, 0, 0, 1);
    got = mouse_poll(&me);
    CHECK(got && me.type == MEV_WHEEL && me.wheel == 1,
          "T7: wheel up arrived as type %u wheel %d (want MEV_WHEEL +1)",
          (u32)me.type, me.wheel);
    memset(&me, 0, sizeof me);
    mouse_inject(0, 0, 0, -1);
    got = mouse_poll(&me);
    CHECK(got && me.type == MEV_WHEEL && me.wheel == -1,
          "T7: wheel DOWN arrived as type %u wheel %d (want MEV_WHEEL -1: "
          "a lost sign here kills downward scrolling everywhere)",
          (u32)me.type, me.wheel);

    /* modifier + key through the REAL kbd.c: Left Shift + 'a' = 'A' */
    u8 pk[6] = { 0, 0, 0, 0, 0, 0 };
    u8 pm = 0;
    u8 keys[6] = { 0x04, 0, 0, 0, 0, 0 };
    kbd_inject_hid(0x02, keys, pk, &pm);
    int kgot = 0;
    u16 last = 0;
    u8 lastp = 0;
    while (kbd_poll(&ke)) { kgot = 1; if (ke.pressed) { last = ke.keycode; lastp = 1; } }
    CHECK(kgot && last == 'A' && lastp,
          "T7: shift+'a' through real kbd.c -> keycode %u (want 'A' "
          "0x41)", last);

    /* release everything: empty report must emit breaks, not repeats */
    u8 zk[6] = { 0, 0, 0, 0, 0, 0 };
    kbd_inject_hid(0, zk, pk, &pm);
    int saw_break = 0, saw_make = 0;
    while (kbd_poll(&ke)) { if (ke.pressed) saw_make = 1; else saw_break = 1; }
    CHECK(saw_break && !saw_make,
          "T7: release report produced make %d break %d (want breaks "
          "only)", saw_make, saw_break);
    printf("  direction matrix: HID down/up -> queue -/+ , WM y grows/"
           "shrinks; shift+a -> 'A'; release -> breaks only\n");
}

/* ------------- T11 (r39): HID usage -> scancode translation table ------
 * Field findings this pins down: the physical Delete key emitted End
 * (the nav block 0x4A-0x4D sat one usage off from the HID table: 0x49
 * Ins, 0x4A Home, 0x4B PgUp, 0x4C Del, 0x4D End, 0x4E PgDn), so
 * Ctrl+Alt+Del never fired and Home silently ate text; F11 mapped to
 * the NumLock byte through a wrong range formula; and NumLock,
 * ScrollLock, Pause, PrintScreen plus the whole keypad were unmapped
 * dead keys. */
static void test_hid_table(void)
{
    static const struct { u8 usage, sc; const char *name; } v[] = {
        { 0x04, 0x1E, "a" }, { 0x1D, 0x2C, "z" },
        { 0x1E, 0x02, "digit 1" }, { 0x27, 0x0B, "digit 0" },
        { 0x28, 0x1C, "enter" }, { 0x29, 0x01, "esc" },
        { 0x2A, 0x0E, "backspace" }, { 0x2B, 0x0F, "tab" },
        { 0x2C, 0x39, "space" }, { 0x39, 0x3A, "caps lock" },
        { 0x3A, 0x3B, "F1" }, { 0x43, 0x44, "F10" },
        { 0x44, 0x57, "F11 (r39: old math returned 0x45=NumLock)" },
        { 0x45, 0x58, "F12" },
        { 0x46, 0x54, "print screen (r39: was dead)" },
        { 0x47, 0x46, "scroll lock (r39: was dead)" },
        { 0x48, 0, "pause (not NumLock)" },
        { 0x49, 0x52, "insert" },
        { 0x4A, 0x47, "home (r39: was DELETE scancode - ate text)" },
        { 0x4B, 0x49, "page up (r39: was home)" },
        { 0x4C, 0x53, "delete (r39: was END - Ctrl+Alt+Del was dead)" },
        { 0x4D, 0x4F, "end (r39: was page up)" },
        { 0x4E, 0x51, "page down" },
        { 0x4F, 0x4D, "right arrow" }, { 0x50, 0x4B, "left arrow" },
        { 0x51, 0x50, "down arrow" }, { 0x52, 0x48, "up arrow" },
        { 0x53, 0x45, "num lock (r39: was dead)" },
        { 0x54, 0x35, "keypad / (r39: was dead)" },
        { 0x55, 0x37, "keypad * (r39: was dead)" },
        { 0x56, 0x4A, "keypad - (r39: was dead)" },
        { 0x57, 0x4E, "keypad + (r39: was dead)" },
        { 0x58, 0x1C, "keypad enter (r39: was dead)" },
    };
    for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        u8 got = hid_make(v[i].usage);
        CHECK(got == v[i].sc, "T11: %s: usage %02X -> %02X, want %02X",
              v[i].name, (u32)v[i].usage, (u32)got, (u32)v[i].sc);
    }

    /* keypad with NumLock ON (driver default): digit-row scancodes */
    CHECK(hid_make(0x59) == 0x02, "T11: KP1 -> '1' 0x02, got %02X",
          (u32)hid_make(0x59));
    CHECK(hid_make(0x61) == 0x0A, "T11: KP9 -> '9' 0x0A, got %02X",
          (u32)hid_make(0x61));
    CHECK(hid_make(0x62) == 0x0B, "T11: KP0 -> '0' 0x0B, got %02X",
          (u32)hid_make(0x62));
    CHECK(hid_make(0x63) == 0x34, "T11: KP. -> '.' 0x34, got %02X",
          (u32)hid_make(0x63));

    /* end-to-end through the real diff/inject path: a report carrying
     * NumLock (usage 0x53) must produce a KEY_NUM press and flip the
     * keypad translation to nav mode; release must NOT toggle again */
    u8 pk[6] = {0}, pm = 0;
    u8 keys[6] = { 0x53, 0, 0, 0, 0, 0 };
    sim_kbd_reports = 0; sim_k_code = 0; sim_k_pressed = 9;
    kbd_inject_hid(0, keys, pk, &pm);
    drain_input();
    CHECK(sim_kbd_reports == 1 && sim_k_code == KEY_NUM &&
          sim_k_pressed == 1,
          "T11: NumLock press emitted %ld event(s) code %u pressed %u "
          "(want 1, KEY_NUM, 1)", sim_kbd_reports, (u32)sim_k_code,
          (u32)sim_k_pressed);
    CHECK(num_on == 0, "T11: NumLock press did not flip num_on");
    CHECK(hid_make(0x59) == 0x4F, "T11: NumLock off: KP1 -> End, got %02X",
          (u32)hid_make(0x59));
    CHECK(hid_make(0x60) == 0x48, "T11: NumLock off: KP8 -> Up, got %02X",
          (u32)hid_make(0x60));
    CHECK(hid_make(0x5D) == 0x00, "T11: NumLock off: KP5 -> none, got %02X",
          (u32)hid_make(0x5D));
    keys[0] = 0;
    kbd_inject_hid(0, keys, pk, &pm);   /* release edge */
    drain_input();
    CHECK(num_on == 0,
          "T11: NumLock RELEASE toggled again (must be press-edge only)");
    num_on = 1;                          /* restore driver default */

    /* Delete end-to-end: usage 0x4C must reach the WM as KEY_DELETE -
     * the exact event the Ctrl+Alt+Del handler waits for */
    keys[0] = 0x4C;
    sim_kbd_reports = 0; sim_k_code = 0;
    kbd_inject_hid(0, keys, pk, &pm);
    drain_input();
    CHECK(sim_kbd_reports == 1 && sim_k_code == KEY_DELETE,
          "T11: Delete press emitted code %u (want KEY_DELETE - the "
          "Ctrl+Alt+Del path)", (u32)sim_k_code);
    keys[0] = 0;
    kbd_inject_hid(0, keys, pk, &pm);
    drain_input();

    printf("  T11 hid_make: %u table vectors + NumLock toggle/keypad "
           "modes + Delete -> KEY_DELETE all correct\n",
           (u32)(sizeof(v) / sizeof(v[0])));
}

/* Idle HID must never trigger synchronous hub probing/reset. */
static void test_idle_dispatch(void)
{
    memset(devs, 0, sizeof(devs));
    ncons = 0;
    pending_portc = 0;
    have_xhci = 1;
    devs[1].used = 1; devs[1].slot = 1;
    devs[1].is_hub = 1; devs[1].hub_ports = 1;
    devs[1].ep0 = palloc(4096); devs[1].ep0_cycle = 1;
    devs[2].used = 1; devs[2].nhid = 1;
    devs[2].hid[0].active = 1;
    input_guard_armed = 1; input_last_tick = 1;
    for (int i = 0; i < 10; i++) {
        tick_count += 1000;
        u64 before = tick_count;
        usb_poll();
        CHECK(tick_count == before && devs[1].ep0_idx == 0,
              "T12: idle HID entered blocking hub control path");
        if (tick_count != before) return;
    }
    /* Repeated character with releases and long idle gaps. */
    u8 prev[6] = {0}, mod = 0, keys[6] = {4,0,0,0,0,0};
    struct key_event ke;
    drain_input();
    for (int i = 0; i < 20; i++) {
        kbd_inject_hid(0, keys, prev, &mod);
        CHECK(kbd_poll(&ke) && ke.pressed && ke.keycode == 'a',
              "T12: repeated a missing at %d", i);
        u8 up[6] = {0}; kbd_inject_hid(0, up, prev, &mod);
        tick_count += 1000; usb_poll();
    }
    /* Stationary presses/releases need no movement event. */
    struct mouse_event me;
    mouse_apply(0,0,0,0); drain_input();
    for (int i = 0; i < 20; i++) {
        mouse_apply(1,0,0,0);
        CHECK(mouse_poll(&me) && me.type == MEV_BUTTON && me.down,
              "T12: stationary press missing");
        mouse_apply(0,0,0,0);
        CHECK(mouse_poll(&me) && me.type == MEV_BUTTON && !me.down,
              "T12: stationary release missing");
    }
    printf("  T12 idle HID: no hub controls; repeated a and stationary clicks pass\n");
}

static void test_control_event_isolation(void)
{
    cc_code = 0xFF; cc_slot = 0;
    sim_post_event(2, 3, 1, 0, 0); proc_events();
    CHECK(cc_slot == 0 && cc_code == 0xFF,
          "T13: interrupt report falsely completed control transfer");
    sim_post_event(2, 1, 1, 0, 0);
    sim_post_event(2, 3, 13, 0, 0); proc_events();
    CHECK(cc_slot == 0x10102 && cc_code == 1,
          "T13: interrupt report overwrote EP0 completion in same batch");
    printf("  T13 EP0 completion isolated from interrupt reports\n");
}

/* Split controls may use independent IDs that also collide modulo 8. */
static void test_split_reports(void)
{
    static const u8 mouse_desc[] = {
        0x05,1, 0x85,1, 0x09,0x30, 0x09,0x31, 0x75,8, 0x95,2, 0x81,6,
        0x85,9, 0x05,9, 0x19,1, 0x29,3, 0x75,1, 0x95,3, 0x81,2,
        0x75,5, 0x95,1, 0x81,1,
        0xA4, /* save globals incl ID9 */
        0x85,17, 0x05,1, 0x09,0x38, 0x75,8, 0x95,1, 0x81,6,
        0xB4, /* restore ID9, size5/count1 */
        0x85,1, 0x91,1 /* same ID, output: MUST NOT erase 16 input bits */
    };
    struct hid_ep h; memset(&h,0,sizeof(h)); h.kind=2;
    h.nlayouts=hid_parse_layouts(mouse_desc,sizeof(mouse_desc),2,h.layouts,8);
    CHECK(h.nlayouts==3,"T14: split mouse parsed %u layouts, need 3",h.nlayouts);
    CHECK(h.layouts[0].rid==1 && h.layouts[0].rpt_bits==16 && h.layouts[1].rid==9 && h.layouts[2].rid==17,
          "T14: report ID alias/revisit corrupted layouts");
    h.lay=h.layouts[0];
    mouse_apply(0,0,0,0); drain_input();
    u8 down[]={9,1}, up[]={9,0}, wheel[]={17,1}, move[]={1,5,6};
    struct mouse_event e;
    CHECK(hid_inject_layout(&h,down,sizeof(down),1,3) && mouse_poll(&e) && e.type==MEV_BUTTON && e.down,
          "T14: stationary button-only report lost");
    for(int i=0;i<20;i++) {
        CHECK(hid_inject_layout(&h,wheel,sizeof(wheel),1,3) && mouse_poll(&e) && e.type==MEV_WHEEL && e.wheel==1,
              "T14: isolated wheel tick %d lost",i);
        CHECK(!mouse_poll(&e),"T14: wheel-only report synthesized a release");
    }
    CHECK(hid_inject_layout(&h,move,sizeof(move),1,3) && mouse_poll(&e) && e.type==MEV_MOVE && e.buttons==1,
          "T14: movement lost held-button state");
    CHECK(hid_inject_layout(&h,up,sizeof(up),1,3) && mouse_poll(&e) && e.type==MEV_BUTTON && !e.down,
          "T14: independent release lost");
    static const u8 kdesc[]={
        0x05,7, 0x85,1, 0x19,0xE0, 0x29,0xE7, 0x75,1, 0x95,8, 0x81,2,
        0x85,9, 0x19,0, 0x29,0x65, 0x75,8, 0x95,6, 0x81,0
    };
    memset(&h,0,sizeof(h)); h.kind=1;
    h.nlayouts=hid_parse_layouts(kdesc,sizeof(kdesc),1,h.layouts,8); h.lay=h.layouts[0];
    CHECK(h.nlayouts==2,"T14: split keyboard layouts missing");
    u8 shift[]={1,2}, keys[]={9,4,0,0,0,0,0}, release[]={9,0,0,0,0,0,0};
    hid_inject_layout(&h,shift,2,1,3); drain_input();
    struct key_event ke;
    for(int i=0;i<20;i++) {
        CHECK(hid_inject_layout(&h,keys,7,1,3) && kbd_poll(&ke) && ke.pressed && ke.keycode=='A',
              "T14: split modifiers/repeated key missing %d",i);
        hid_inject_layout(&h,release,7,1,3); drain_input();
    }
    shift[1]=0; hid_inject_layout(&h,shift,2,1,3); drain_input();
    keys[1]=0x53;
    CHECK(hid_inject_layout(&h,keys,7,1,3) && kbd_poll(&ke) && ke.keycode==KEY_NUM && ke.pressed,
          "T14: NumLock on alternate report ID lost");
    hid_inject_layout(&h,release,7,1,3); drain_input(); num_on=1;
    static const u8 pushed[]={
        0x05,1,0x85,1,0x75,8,0x95,1,0x09,0x30,0x81,6,
        0xA4,0x85,9,0x05,9,0x75,1,0x95,3,0x19,1,0x81,2,
        0xB4,0x09,0x31,0x81,6
    };
    struct hid_layout maps[8];
    CHECK(hid_parse_layouts(pushed,sizeof(pushed),2,maps,8)==2 &&
          maps[0].rid==1 && maps[0].y_off==8 && maps[0].y_sz==8 && maps[0].rpt_bits==16,
          "T14: Push/Pop did not restore page, count, size and ID");
    static const u8 bitmap[]={0x05,7,0x85,3,0x19,4,0x29,11,0x75,1,0x95,8,0x81,2};
    memset(&h,0,sizeof(h)); h.kind=1;
    h.nlayouts=hid_parse_layouts(bitmap,sizeof(bitmap),1,h.layouts,8);h.lay=h.layouts[0];
    u8 bitmap_key[]={3,1};
    CHECK(hid_inject_layout(&h,bitmap_key,2,1,3) && kbd_poll(&ke) && ke.keycode=='a',
          "T14: nonzero Usage Minimum bitmap interpreted as modifiers");
    bitmap_key[1]=0;hid_inject_layout(&h,bitmap_key,2,1,3);drain_input();
    printf("  T14 split report IDs: stationary clicks, all 20 wheel ticks, repeated keys and NumLock PASS\n");
}

/* T15: full-size interrupt PACKETS on the field devices' 8-byte EPs.
 * No refresh callback or unrelated input is involved in delivery. */
static void test_packet_boundaries(void)
{
    struct hid_layout kl;
    CHECK(hid_parse_layout(rdesc_bootkbd,sizeof(rdesc_bootkbd),1,&kl),"T15 keyboard descriptor");
    u8 minus[8]={0,0,0x2D,0,0,0,0,0}, release[8]={0};
    struct key_event ke;
    drain_input();
    struct xdev *old=slot_alloc(3); old->nhid=1; old->kind=1;
    struct sim_consumer *broken=t8_arm(old,3,0,1,&kl,8,minus);
    /* Demonstrate the old transfer behavior, not a mocked event drop. */
    old->hid[0].inr[2]=64;
    for(int i=0;i<8;i++) {
        int completed=sim_deliver_packet(broken,i%2?release:minus,8);
        CHECK(completed==(i==7),"T15 old 64-byte TD completed on packet %d",i+1);
        proc_events();
        if(i<7) CHECK(!kbd_poll(&ke),"T15 old oversized TD unexpectedly emitted a key early");
    }
    CHECK(kbd_poll(&ke) && ke.pressed && ke.keycode=='-',"T15 oversized TD first report not decoded");
    CHECK(!kbd_poll(&ke),"T15 old TD should have swallowed its seven trailing reports");
    sim_deliver_packet(broken,minus,8);proc_events();
    CHECK(!kbd_poll(&ke),"T15 old batch did not reproduce stuck repeated '-'");
    broken->active=0; old->hid[0].active=0;

    struct xdev *d=slot_alloc(4); d->nhid=1; d->kind=1;
    struct sim_consumer *c=t8_arm(d,4,0,1,&kl,8,minus);
    CHECK(c && c->max_packet==8,"T15 not testing field MPS8");
    static const u8 usages[]={0x2D,0x2D,0x2A,0x2A,0x53,0x53};
    static const u16 codes[]={'-','-',8,8,KEY_NUM,KEY_NUM};
    for(int i=0;i<200;i++) {
        u8 packet[8]={0,0,usages[i%6],0,0,0,0,0};
        tick_count+=50; // a deliberate pause; no GUI refresh or extra reports
        CHECK(sim_deliver_packet(c,packet,8)==1,"T15 key %d needs more USB packets to complete",i);
        proc_events();
        CHECK(kbd_poll(&ke) && ke.pressed && ke.keycode==codes[i%6],"T15 key %d missing/repeated wrong",i);
        CHECK(sim_deliver_packet(c,release,8)==1,"T15 release %d failed to complete",i);
        proc_events();drain_input();
    }
    c->active=0; d->hid[0].active=0; num_on=1;

    struct hid_layout ml={.kind=2,.ok=1,.btn_off=0,.btn_cnt=3,
        .x_off=8,.x_sz=16,.y_off=24,.y_sz=16,.w_off=40,.w_sz=8,.rpt_bits=64};
    struct xdev *m=slot_alloc(5);m->nhid=1;m->kind=2;
    struct sim_consumer *mc=t8_arm(m,5,0,2,&ml,8,release);
    mouse_apply(0,0,0,0);drain_input();
    struct mouse_event me;
    for(int i=0;i<200;i++) {
        u8 down[8]={1,0,0,0,0,0,0,0};
        tick_count+=50;
        CHECK(sim_deliver_packet(mc,down,8)==1,"T15 stationary down delayed"); proc_events();
        CHECK(mouse_poll(&me) && me.type==MEV_BUTTON && me.down,"T15 stationary down lost");
        sim_deliver_packet(mc,release,8);proc_events();
        CHECK(mouse_poll(&me) && me.type==MEV_BUTTON && !me.down,"T15 stationary up lost");
        u8 wheel[8]={0,0,0,0,0, i%2?0xFF:1,0,0};
        CHECK(sim_deliver_packet(mc,wheel,8)==1,"T15 wheel delayed"); proc_events();
        CHECK(mouse_poll(&me) && me.type==MEV_WHEEL && me.wheel==(i%2?-1:1),"T15 wheel lost");
    }
    mc->active=0;
    printf("  T15 reproduced 8-report batching/stuck '-' with old length; 400 key packets + 600 stationary mouse packets pass individually\n");
}

static void test_ps2_packet_fields(void)
{
    mouse_apply(0,0,0,0); drain_input();
    packet_len=4; mouse_id=4;
    packet[0]=8; packet[1]=200; packet[2]=180; packet[3]=0;
    handle_packet(); struct mouse_event e;
    CHECK(mouse_poll(&e) && e.type==MEV_MOVE && e.dx==200 && e.dy==180,
          "T16 PS/2 positive 9-bit deltas were sign-flipped at 128");
    packet[0]=8|0x30; packet[1]=56; packet[2]=76; handle_packet();
    CHECK(mouse_poll(&e) && e.dx==-200 && e.dy==-180,"T16 PS/2 negative 9-bit deltas wrong");
    packet[0]=8;packet[1]=packet[2]=0;packet[3]=0x1F;handle_packet();
    CHECK(mouse_poll(&e) && e.type==MEV_WHEEL && e.wheel==-1,
          "T16 Explorer wheel nibble/extra-button bits misdecoded");
    packet[3]=1;handle_packet();
    CHECK(mouse_poll(&e) && e.wheel==1,"T16 Explorer up wrong");
    mouse_id=3;packet[3]=0xFF;handle_packet();
    CHECK(mouse_poll(&e) && e.wheel==-1,"T16 IntelliMouse signed-byte wheel wrong");
    packet_len=3; mouse_id=0;
    printf("  T16 PS/2 9-bit axes and both wheel formats PASS\n");
}

static void test_report_assembly(void)
{
    caps_on=0;
    u8 desc[128]={0x85,1};
    memcpy(desc+2,rdesc_bootkbd,sizeof(rdesc_bootkbd));
    const u8 consumer[]={0x85,2,0x05,0x0C,0x75,8,0x95,16,0x81,2};
    memcpy(desc+2+sizeof(rdesc_bootkbd),consumer,sizeof(consumer));
    int dl=2+sizeof(rdesc_bootkbd)+sizeof(consumer);
    struct hid_layout lay;
    CHECK(hid_parse_layout(desc,dl,1,&lay),"T17 parse keyboard");
    u8 down[9]={1,0,0,4},up[9]={1}; struct key_event e={0};
    struct xdev *d=slot_alloc(3); d->nhid=1;d->kind=1;
    struct sim_consumer *c=t8_arm(d,3,0,1,&lay,8,down);
    struct hid_ep *h=&d->hid[0]; hid_set_frames(h,desc,dl);
    CHECK(h->nframes==2 && h->frame_sizes[1]==17,"T17 consumer framing lost");
    drain_input();
    for(int i=0;i<100;i++) {
        sim_deliver_packet(c,down,8);proc_events();
        CHECK(!kbd_poll(&e),"T17 injected partial report");
        sim_deliver_packet(c,down+8,1);proc_events();
        CHECK(kbd_poll(&e) && e.keycode=='a',"T17 8+1 report failed/repeated key latched: key=%u ctrl=%u used=%u goal=%u",e.keycode,e.ctrl,h->fragment_used,h->fragment_goal);
        sim_deliver_packet(c,up,8);proc_events();
        sim_deliver_packet(c,up+8,1);proc_events();
        CHECK(!kbd_poll(&e),"T17 release generated phantom input");
    }
    /* Consumer continuation deliberately resembles a keyboard report. */
    u8 foreign[17]={2};memcpy(foreign+8,down,9);
    sim_deliver_packet(c,foreign,8);proc_events();
    sim_deliver_packet(c,foreign+8,8);proc_events();
    sim_deliver_packet(c,foreign+16,1);proc_events();
    CHECK(!kbd_poll(&e) && !h->fragment_used,"T17 foreign continuation injected keys");
    /* Consume an over-capacity report without copying beyond the 64B buffer. */
    u8 oversized[65]={2};memcpy(oversized+8,down,9);h->frame_sizes[1]=65;
    for (int i=0;i<64;i+=8) { sim_deliver_packet(c,oversized+i,8);proc_events(); }
    sim_deliver_packet(c,oversized+64,1);proc_events();
    CHECK(!kbd_poll(&e) && !h->fragment_used,"T17 over-capacity report injected or wedged assembly");
    h->frame_sizes[1]=17;
    /* ZLP aborts a truncated report; subsequent valid input must recover. */
    sim_deliver_packet(c,down,8);proc_events();
    sim_deliver_packet(c,down,0);proc_events();
    CHECK(!kbd_poll(&e) && !h->fragment_used,"T17 zero-length termination kept stale fragment");
    sim_deliver_packet(c,down,8);proc_events();
    sim_deliver_packet(c,down+8,1);proc_events();
    CHECK(kbd_poll(&e) && e.keycode=='a',"T17 recovery after truncated report failed");
    sim_deliver_packet(c,up,8);proc_events();sim_deliver_packet(c,up+8,1);proc_events();drain_input();
    /* Exact multiple of MPS must complete at its declared size, no ZLP needed. */
    struct hid_layout nk;hid_parse_layout(rdesc_nkro,sizeof(rdesc_nkro),1,&nk);
    u8 report[16]={2,0,0,0x10};
    struct xdev *nd=slot_alloc(4);nd->nhid=1;nd->kind=1;
    struct sim_consumer *nc=t8_arm(nd,4,0,1,&nk,8,report);
    sim_deliver_packet(nc,report,8);proc_events();CHECK(!kbd_poll(&e),"T17 partial NKRO emitted");
    sim_deliver_packet(nc,report+8,8);proc_events();
    CHECK(kbd_poll(&e) && e.keycode=='a',"T17 exact 8+8 NKRO waited for unrelated activity");
    /* Hidden output-only Report ID, also split 8+1. */
    h->nframes=0;h->lay.rid=0;h->lay.saw_rid=1;
    sim_deliver_packet(c,down,8);proc_events();CHECK(!kbd_poll(&e),"T17 hidden prefix partial emitted");
    sim_deliver_packet(c,down+8,1);proc_events();
    CHECK(kbd_poll(&e) && e.keycode=='a' && !e.ctrl,"T17 hidden-prefix assembly failed");
    printf("  T17 400 split packets, foreign-ID framing, ZLP recovery, NKRO and hidden-prefix assembly PASS\n");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    sim_verbose = (argc > 1 && !strcmp(argv[1], "-v"));
    sim_env_init();
    printf("usb_sim: parser vs r29 field descriptors...\n");
    test_parser();
    printf("usb_sim: EP0 ring across wraps...\n");
    test_ep0_ring();
    printf("usb_sim: composite interrupt rings across ~71 laps...\n");
    test_interrupt_rings();
    printf("usb_sim: EP context bytes (r31 root fix)...\n");
    test_ep_ctx();
    printf("usb_sim: Report ID detection (r33)...\n");
    test_report_id();
    printf("usb_sim: r34 runtime offset fallbacks (probe + kbd length "
           "rule)...\n");
    test_runtime_fallbacks();
    printf("usb_sim: r37 report-descriptor layout parser + extraction...\n");
    test_layout_parser();
    printf("usb_sim: r38 move coalescing (button events survive floods)...\n");
    test_move_coalescing();
    printf("usb_sim: r38 ACPI _S5_ package walk...\n");
    test_acpi_s5();
    printf("usb_sim: r35 direction matrix through real mouse.c/kbd.c...\n");
    test_direction_matrix();
    printf("usb_sim: r39 HID usage -> scancode table (Delete/F11/NumLock/"
           "keypad fixes)...\n");
    test_hid_table();
    test_idle_dispatch();
    test_control_event_isolation();
    test_split_reports();
    test_packet_boundaries();
    test_ps2_packet_fields();
    test_report_assembly();
    if (failures) {
        printf("USB SIM: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("USB SIM: ALL TESTS PASSED\n");
    return 0;
}

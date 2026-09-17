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

void mouse_inject(u8 buttons, i32 dx, i32 dy, i32 wheel)
{
    (void)buttons; (void)dx; (void)dy; (void)wheel;
    sim_mouse_reports++;
}
void kbd_inject_hid(u8 mod, const u8 *keys, u8 *prev_keys, u8 *prev_mod)
{
    sim_kbd_reports++;
    memcpy(prev_keys, keys, 6);
    *prev_mod = mod;
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

/* ------------------------------------------------- mini xHC simulator -- */
static u32 fake_op[512], fake_db[256], fake_rt[64], fake_cap[256];
static u64 fake_dcbaa[16];

struct sim_consumer {
    volatile u32 *ring;
    int ring_trbs;              /* total slots incl. the link at the end */
    u32 deq;                    /* consumer dequeue index */
    u32 cycle;                  /* expected consumer cycle bit */
    u32 slot, dci;
    u32 report_len;             /* simulated payload length for events */
    int active;
    long tds;                   /* completed TDs (events posted) */
};
static struct sim_consumer cons[6];
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

/* Consume ready TRBs until an IOC fires (one TD per call, like a
 * scheduling xHC) or a not-ready TRB idles the endpoint.  This is where a
 * wrong-cycle producer TRB becomes visible: the consumer stops, the
 * driver times out, and the surrounding test fails. */
static int sim_consume_td(struct sim_consumer *c)
{
    if (!c->active) return 0;
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
        u32 ptr = t[0];
        c->deq++;
        if (ioc) {
            c->tds++;
            sim_post_event(c->slot, c->dci, 1, ptr, 0);
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
    struct sim_consumer *c = &cons[ncons++];
    memset(c, 0, sizeof(*c));
    c->ring = ring; c->ring_trbs = trbs;
    c->deq = 0; c->cycle = 1;
    c->slot = slot; c->dci = dci;
    c->report_len = replen;
    c->active = 1;
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
              c[0].ep_mps == 8,
              "holtek iface0: want mouse proto2 ep81 mps8, got %d/%d/ep%x/%d",
              c[0].iface, c[0].proto, c[0].ep_addr, c[0].ep_mps);
        CHECK(c[1].iface == 1 && c[1].proto == 1 && c[1].ep_addr == 2 &&
              c[1].ep_mps == 8,
              "holtek iface1: want kbd proto1 ep82 mps8, got %d/%d/ep%x/%d",
              c[1].iface, c[1].proto, c[1].ep_addr, c[1].ep_mps);
    }

    n = usb_hid_parse(desc_razer, (int)sizeof(desc_razer), c, MAX_HID_EPS, 0);
    CHECK(n == 1, "razer: expected 1 boot iface, got %d", n);
    if (n == 1)
        CHECK(c[0].iface == 0 && c[0].proto == 1 && c[0].ep_addr == 1 &&
              c[0].ep_mps == 8,
              "razer iface0: want kbd proto1 ep81 mps8, got %d/%d/ep%x/%d",
              c[0].iface, c[0].proto, c[0].ep_addr, c[0].ep_mps);

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
    for (int j = 0; j < 2; j++) {
        struct hid_ep *h = &d->hid[j];
        h->active = 1;
        h->iface = j;
        h->kind = j == 0 ? 1 : 2;
        h->ep_addr = (u8)(j + 1);
        h->dci = (u32)(h->ep_addr * 2 + 1);
        h->ep_mps = 8;
        h->inr = (volatile u32 *)palloc(4096);
        for (int b = 0; b < IN_TRBS; b++) {
            h->in_buf[b] = palloc(16);
            h->in_buf[b][1] = 1;      /* recognizable report payload */
        }
        h->inr_idx = 0; h->inr_cycle = 1;
        h->link_pend = 0; h->link_pend_cycle = 1;
        hid_arm_ring(d, j);           /* the REAL arming code */
    }
    struct sim_consumer *ck =
        sim_add_consumer(d->hid[0].inr, IN_TRBS, 2, d->hid[0].dci, 8);
    struct sim_consumer *cm =
        sim_add_consumer(d->hid[1].inr, IN_TRBS, 2, d->hid[1].dci, 4);

    sim_kbd_reports = sim_mouse_reports = 0;
    /* 500 reports per endpoint = ~71 laps of a 7-slot ring.  The old code
     * never rewrote the Link TRB after lap 1: the consumer met a stale
     * cycle bit at the link and input died permanently after ~14
     * reports.  Events flow through the REAL proc_events re-arm path. */
    for (int i = 0; i < 500; i++) {
        sim_consume_td(ck);
        sim_consume_td(cm);
        proc_events();
    }
    CHECK(sim_kbd_reports == 500,
          "keyboard delivered %ld/500 reports (ring stalled at a link?)",
          sim_kbd_reports);
    CHECK(sim_mouse_reports == 500,
          "mouse delivered %ld/500 reports", sim_mouse_reports);
    CHECK(ck->tds == 500 && cm->tds == 500,
          "consumer TD counts %ld/%ld, want 500/500", ck->tds, cm->tds);
    printf("  interrupt rings: 2 EPs x 500 reports (~71 laps each) - "
           "kbd %ld, mouse %ld delivered\n",
           sim_kbd_reports, sim_mouse_reports);
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
    if (failures) {
        printf("USB SIM: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("USB SIM: ALL TESTS PASSED\n");
    return 0;
}

/* SCos native - USB input driver: xHCI host controller + HID boot protocol.
 *
 * Real PCs attach keyboards and mice to USB, not PS/2. Firmware SMM
 * emulation sometimes fakes a PS/2 keyboard but essentially never a mouse,
 * and modern boards expose their USB ports only through an xHCI controller,
 * so without talking to the host controller the mouse is invisible on real
 * hardware (v86 wires PS/2 straight to the guest, which hid the problem).
 *
 * Scope: xHCI, HID boot protocol (keyboard 8-byte report, mouse 3/4-byte),
 * polled event ring from the WM loop (100 Hz), one control pipe plus one
 * interrupt-IN pipe per device, up to 4 slots, hot-plug port changes.
 * Reports are injected into the existing PS/2 input queues so everything
 * above the driver sees USB input identically.
 *
 * Ring discipline (the part that bites every first xHCI driver):
 *  - every ring reserves its last slot for a Link TRB; producer wraps there
 *    and toggles its cycle bit
 *  - the event ring consumer wraps on the Link slot and toggles its cycle
 *  - doorbell targets are endpoint IDs: control = 0, ep1-IN = 3
 */
#include "scos.h"

#define MAX_SLOTS 8
#define CMD_TRBS 128
#define EVT_TRBS 64
#define EP0_TRBS 16
#define IN_TRBS 8

#define TRB_NORMAL      1
#define TRB_SETUP       2
#define TRB_DATA        3
#define TRB_STATUS      4
#define TRB_LINK        6
#define TRB_ENABSLOT    9
#define TRB_DISABLESLOT 10
#define TRB_EVALCTX     13
#define TRB_ADDRDEV     11
#define TRB_CFGEP       12
#define EV_TRANSFER     32
#define EV_CMDCOMP      33
#define EV_PORTCHANGE   34

struct xdev {
    int used;
    int slot;
    int kind;                 /* 1 = keyboard, 2 = mouse */
    int iface;
    int ep_addr;              /* endpoint number of the interrupt IN ep */
    volatile u32 *ep0;
    int ep0_idx; u32 ep0_cycle;
    volatile u32 *inr;
    int inr_idx; u32 inr_cycle;
    u8 *in_buf[IN_TRBS];
    u8 prev_mod;
    u8 prev_keys[6];
    u32 last_rep_tick;
    u8 reported, silent_logged;
};

static volatile u8 *cap, *op, *db, *rt;
static int have_xhci;
static int csz = 32;
static int max_ports;
static volatile u32 *cmd_ring;
static int cmd_idx; static u32 cmd_cycle = 1;
static volatile u32 *evt_ring;
static int evt_idx; static u32 evt_cycle = 1;
static volatile u64 *dcbaa;
static u8 *devctx[MAX_SLOTS + 1];
static u8 *inctx[MAX_SLOTS + 1];
static struct xdev devs[MAX_SLOTS + 1];
static int n_devs;
static int fail_flag, hub_count;
static volatile u32 cc_code = 0xFF;
static volatile u32 cc_slot;
static volatile int cc_valid;
static u32 pending_portc;           /* ports with queued connect-change work */
static char status_line[96];
static u8 desc_buf[512];

static u64 now_ms(void) { return tick_count * 10; }
#define PA(p) ((u32)(p))

static void wr64(volatile u32 *reg, u64 v)
{
    reg[0] = (u32)(v & 0xFFFFFFFFu);
    reg[1] = (u32)(v >> 32);
}

static void ring_link(volatile u32 *ring, int last_idx, u32 cycle)
{
    volatile u32 *l = ring + (u32)last_idx * 4;
    l[0] = PA(ring);
    l[1] = 0;
    l[2] = 0;
    l[3] = (TRB_LINK << 10) | (1u << 1) | cycle;
}

static int enumerate_port(int port);

/* ------------------------------------------------------------- events ---- */
static int proc_events(void)
{
    int work = 0;
    for (int guard = 0; guard < 128; guard++) {
        volatile u32 *t = evt_ring + (u32)evt_idx * 4;
        if ((t[3] & 1) != evt_cycle) break;
        u32 type = (t[3] >> 10) & 0x3F;
        work = 1;
        if (type == EV_CMDCOMP) {
            cc_code = (t[2] >> 24) & 0xFF;      /* completion code: dw2[31:24] */
            cc_slot = (t[3] >> 24) & 0xFF;      /* slot id: dw3[31:24] (the
                                                 * TRB type is dw3[15:10]=33;
                                                 * dw0/1 is the command TRB
                                                 * pointer, NOT the slot!) */
            cc_valid = 1;
        } else if (type == EV_TRANSFER) {
            u32 slot = (t[3] >> 24) & 0xFF;
            u32 ep = (t[3] >> 16) & 0x1F;
            u32 code = (t[2] >> 24) & 0xFF;
            u32 rem = t[2] & 0xFFFFFF;       /* bytes NOT transferred */
            u32 len = rem <= 16u ? 16u - rem : 0u;
            u32 ptr = t[0];
            cc_code = code;
            cc_slot = slot | (ep << 8) | 0x10000u;
            if (slot <= MAX_SLOTS && devs[slot].used &&
                ep == (u32)(devs[slot].ep_addr * 2 + 1) &&
                (code == 1 || code == 12 || code == 13)) {
                struct xdev *d = &devs[slot];
                int i = -1;
                for (int b = 0; b < IN_TRBS; b++)
                    if (PA(d->in_buf[b]) == ptr) i = b;
                if (i >= 0) {
                    u8 *r = d->in_buf[i];
                    d->last_rep_tick = tick_count;
                    if (!d->reported) {
                        d->reported = 1;
                        klog("usb: first report from slot %d (%s)", slot,
                             d->kind == 2 ? "mouse" : "keyboard");
                    }
                    if (d->kind == 2 && len >= 3)
                        mouse_inject(r[0], (i32)(i8)r[1], (i32)(i8)r[2],
                                     len >= 4 ? (i32)(i8)r[3] : 0);
                    else if (d->kind == 1 && len >= 8)
                        kbd_inject_hid(r[0], r + 2, d->prev_keys, &d->prev_mod);
                }
                /* round-robin re-arm of the interrupt ring (skip link slot) */
                volatile u32 *tr = d->inr + (u32)d->inr_idx * 4;
                tr[0] = PA(d->in_buf[d->inr_idx]);
                tr[1] = 0;
                tr[2] = 16;
                /* IOC + ISP exactly like the pre-queued TRBs: without IOC no
                 * event fires, without ISP the always-short HID report never
                 * completes - input would die after the first ring lap */
                tr[3] = (TRB_NORMAL << 10) | (1u << 5) | (1u << 2) |
                        d->inr_cycle;
                d->inr_idx++;
                if (d->inr_idx == IN_TRBS - 1) {
                    d->inr_idx = 0;
                    d->inr_cycle ^= 1;
                }
            }
        } else if (type == EV_PORTCHANGE) {
            u32 port = (t[3] >> 24) & 0xFF;
            if (port >= 1 && port <= (u32)max_ports) {
                volatile u32 *ps =
                    (volatile u32 *)(op + 0x400 + 0x10 * (port - 1));
                u32 v = ps[0];
                ps[0] = (1u << 17) | (1u << 18) | (1u << 20) |
                        (1u << 21) | (1u << 22) | (1u << 23);
                /* never re-enumerate inline: our own port-reset PRC events
                 * fire DURING boot enumeration and nesting a full enumerate
                 * inside event processing burns slots and floods the ring.
                 * Queue it; the usb_poll heartbeat does real hotplug. */
                if (v & 1 && port < 32) pending_portc |= 1u << port;
            }
        }
        /* advance the dequeue pointer and ALWAYS write EHB: a transiently
         * full event ring latches Event-Handler-Busy and only an ERDP write
         * with EHB=1 makes the xHC resume posting (symptom otherwise: every
         * later command completes with code 17, Event Ring Full Error).
         * There is no Link TRB in this ring - the xHC wraps per the ERST
         * segment size - so consume all EVT_TRBS slots, exactly like Linux
         * (inc_deq wraps at TRBS_PER_SEGMENT). */
        evt_idx++;
        if (evt_idx == EVT_TRBS) {
            evt_idx = 0;
            evt_cycle ^= 1;
        }
        volatile u32 *erdp = (volatile u32 *)(rt + 0x20 + 0x18);
        wr64(erdp, (u64)PA(evt_ring + (u32)evt_idx * 4) | (1ull << 3));
    }
    return work;
}

static int wait_event(u64 timeout_ms)
{
    u64 t0 = now_ms();
    cc_code = 0xFF;
    cc_slot = 0;
    cc_valid = 0;
    while (now_ms() - t0 < timeout_ms) {
        proc_events();
        /* bit16 marks a TRANSFER event - not a command completion */
        if (cc_valid && !(cc_slot & 0x10000u)) return (int)cc_code;
        cpu_hlt();
    }
    return -1;
}

/* the xHC halted under us (e.g. after a doorbell/TD error): flip RS back on
 * and let the command ring resume where it stopped */
static void xhci_restart(void)
{
    volatile u32 *cmdr = (volatile u32 *)op;
    volatile u32 *sts = (volatile u32 *)(op + 4);
    /* kick the event-ring dequeue with EHB set FIRST: a ring that went
     * momentarily full latches Event-Handler-Busy and stops all posting
     * until software writes ERDP with EHB=1 */
    volatile u32 *erdp = (volatile u32 *)(rt + 0x20 + 0x18);
    wr64(erdp, (u64)PA(evt_ring + (u32)evt_idx * 4) | (1ull << 3));
    if (!(sts[0] & 1u)) return;
    klog("usb: xHC halted (usbsts %x) - restarting", sts[0]);
    cmdr[0] |= 1u;                       /* RS */
    u64 t0 = now_ms();
    while (now_ms() - t0 < 200 && (sts[0] & 1u)) cpu_hlt();
    if (sts[0] & 1u) klog("usb: restart FAILED (usbsts %x)", sts[0]);
    else klog("usb: xHC running again");
}

static void ring_db(u32 slot, u32 target)
{
    volatile u32 *d = (volatile u32 *)(db + slot * 4);
    d[0] = target & 0xFF;
}

static int run_cmd(u32 d0, u32 d1, u32 d2, u32 d3, u64 timeout)
{
    volatile u32 *t = cmd_ring + (u32)cmd_idx * 4;
    t[0] = d0; t[1] = d1; t[2] = d2;
    t[3] = d3 | cmd_cycle;
    cmd_idx++;
    if (cmd_idx == CMD_TRBS - 1) {
        ring_link(cmd_ring, CMD_TRBS - 1, cmd_cycle);
        cmd_idx = 0;
        cmd_cycle ^= 1;
    }
    *(volatile u32 *)db = 0;             /* command ring doorbell */
    int rc = wait_event(timeout);
    if (rc < 0) xhci_restart();          /* timed out: un-wedge if halted */
    return rc;
}

/* give an enabled-but-unusable slot back to the controller, or the slot
 * table fills up and later Enable Slot commands fail (No Slots/Bandwidth) */
static void disable_slot(int slot)
{
    if (slot >= 1 && slot <= MAX_SLOTS) {
        run_cmd(0, 0, 0, (u32)(TRB_DISABLESLOT << 10) | ((u32)slot << 24), 500);
        devs[slot].used = 0;
    }
}

/* the device's real EP0 max packet size differs from what we assumed at
 * Address Device time (full/low-speed HID devices usually say 8): patch the
 * controller's own EP0 context and Evaluate it, the same dance Linux does */
static int eval_ep0_mps(int slot, u32 mps)
{
    u8 *ic = inctx[slot];
    memset(ic, 0, csz * 34);
    ic[0] = (1u << 1);                       /* Add EP0 (DCI 1) only */
    memcpy(ic + csz * 2, devctx[slot] + csz, csz);  /* devctx: [0]=slot [1]=EP0 */
    u32 *epw = (u32 *)(ic + csz * 2);
    epw[1] = (epw[1] & 0x0000FFFFu) | ((mps & 0xFFFF) << 16);
    int rc = run_cmd(PA(ic), 0, 0,
                     (u32)(TRB_EVALCTX << 10) | ((u32)slot << 24), 500);
    if (rc != 1)
        klog("usb: evaluate ctx (ep0 mps %u) failed slot %d code %d",
             mps, slot, rc);
    return rc;
}

/* ------------------------------------------------------- control pipe ---- */
static volatile u32 *ep0_next(struct xdev *d)
{
    volatile u32 *t = d->ep0 + (u32)d->ep0_idx * 4;
    d->ep0_idx++;
    if (d->ep0_idx == EP0_TRBS - 1) {
        ring_link(d->ep0, EP0_TRBS - 1, d->ep0_cycle);
        d->ep0_idx = 0;
        d->ep0_cycle ^= 1;
    }
    return t;
}

static int ctrl_xfer(int slot, u8 rt_, u8 rq, u16 val, u16 idx,
                     u8 *buf, u16 len, int in)
{
    struct xdev *d = &devs[slot];
    volatile u32 *setup = ep0_next(d);
    setup[0] = (u32)rt_ | ((u32)rq << 8) | ((u32)val << 16);
    setup[1] = (u32)idx | ((u32)len << 16);
    setup[2] = 8;
    setup[3] = (TRB_SETUP << 10) | (1u << 6) |
               (len ? (in ? 3u : 2u) << 16 : 0) | d->ep0_cycle;
    if (len) {
        volatile u32 *data = ep0_next(d);
        data[0] = PA(buf);
        data[1] = 0;
        data[2] = len;
        data[3] = (TRB_DATA << 10) | (in ? (1u << 16) : 0) | d->ep0_cycle;
    }
    volatile u32 *stat = ep0_next(d);
    stat[0] = 0; stat[1] = 0; stat[2] = 0;
    stat[3] = (TRB_STATUS << 10) | (1u << 5) |
              (len ? (in ? 0u : (1u << 16)) : (1u << 16)) | d->ep0_cycle;
    cc_code = 0xFF; cc_slot = 0; cc_valid = 0;
    ring_db((u32)slot, 1);      /* DCI 1 = default control pipe (EP0);
                                 * target 0 is RESERVED on slot doorbells
                                 * and silently never starts the TD */
    u64 t0 = now_ms();
    while (now_ms() - t0 < 800) {
        proc_events();
        if ((cc_slot & 0x10000u) && (cc_slot & 0xFF) == (u32)slot)
            return (int)cc_code;
        cpu_hlt();
    }
    xhci_restart();                      /* ring-full/halt recovery kick */
    return -1;
}


/* ------------------------------------------------- 32-bit BAR relocation --
 * Some firmware parks the xHCI registers above 4 GB ("Above 4G decoding"),
 * which a 32-bit protected-mode kernel cannot reach at all. We can move the
 * BAR ourselves: map every occupied bus-0 memory window by sizing each BAR,
 * then pick a free, aligned slot inside the 32-bit MMIO hole (at or above
 * the lowest occupied MMIO base, so never inside RAM, and below the
 * LAPIC/IOAPIC/config reserve at 0xF8000000).
 */
struct mrange { u32 base, len; };
static struct mrange occ[24];
static int n_occ;

static int bar_probe(u8 b, u8 d, u8 f, u8 off, u32 *base, u32 *len)
{
    u32 lo = pci_read32(b, d, f, off);
    *base = *len = 0;
    if (lo & 0x1) return 0;                       /* IO bar: not our problem */
    int is64 = ((lo & 0x6) == 0x4);
    u32 hi = is64 ? pci_read32(b, d, f, off + 4) : 0;
    pci_write32(b, d, f, off, 0xFFFFFFFFu);       /* size probe */
    if (is64) pci_write32(b, d, f, off + 4, 0xFFFFFFFFu);
    u32 mlo = pci_read32(b, d, f, off) & 0xFFFFFFF0u;
    u32 mhi = is64 ? pci_read32(b, d, f, off + 4) : 0;
    pci_write32(b, d, f, off, lo);                /* restore */
    if (is64) pci_write32(b, d, f, off + 4, hi);
    *base = lo & 0xFFFFFFF0u;
    if (is64) {
        if (mhi == 0xFFFFFFFFu) return is64;      /* window > 4 GB: unusable */
        u64 sz = ~(((u64)mhi << 32) | mlo) + 1;
        if (!sz || sz > 0xFFFFFFFFu) return is64;
        *len = (u32)sz;
    } else {
        if (!mlo) return 0;
        *len = ~mlo + 1;
    }
    if (!*base) *len = 0;
    return is64;
}

static void occ_collect(u8 skip_dev)
{
    n_occ = 0;
    for (u16 d = 0; d < 32; d++) {
        for (u8 f = 0; f < 8; f++) {
            u32 id = pci_read32(0, (u8)d, f, 0);
            if (id == 0xFFFFFFFFu) { if (!f) break; continue; }
            for (u8 bar = 0; bar < 6; bar++) {
                u32 base, len;
                int is64 = bar_probe(0, (u8)d, f, 0x10 + bar * 4, &base, &len);
                if (is64) bar++;
                if (!len || !base || base >= 0xF8000000u) continue;
                if ((u8)d == skip_dev && !f) continue;
                if (n_occ < 24) { occ[n_occ].base = base; occ[n_occ].len = len; n_occ++; }
            }
            if (!(pci_read8(0, (u8)d, f, 0x0E) & 0x80)) break;
        }
    }
}

static int occ_free(u32 base, u32 len)
{
    for (int i = 0; i < n_occ; i++) {
        u32 oe = occ[i].base + occ[i].len;
        u32 ne = base + len;
        if (base < oe && occ[i].base < ne) return 0;
    }
    return 1;
}

static u32 relocate_bar(u8 d, u32 bar0, u32 want_len)
{
    occ_collect(d);
    u32 lowest = 0xF8000000u;
    for (int i = 0; i < n_occ; i++)
        if (occ[i].base < lowest) lowest = occ[i].base;
    if (lowest == 0xF8000000u) lowest = 0xA0000000u;
    if (!want_len) want_len = 0x10000;
    u32 start = (lowest + want_len - 1) & ~(want_len - 1);
    for (int tries = 0; tries < 256; tries++, start += want_len) {
        if (start + want_len > 0xF8000000u) break;
        if (!occ_free(start, want_len)) continue;
        pci_write32(0, d, 0, 0x10, start | (bar0 & 0xFu));
        pci_write32(0, d, 0, 0x14, 0);
        u32 rb = pci_read32(0, d, 0, 0x10) & 0xFFFFFFF0u;
        if (rb != start) continue;
        volatile u32 *c = (volatile u32 *)start;
        u32 caplen = c[0] & 0xFF;
        u32 ver = (c[2] >> 16) & 0xFFFF;          /* HCIVERSION */
        if (caplen < 0x20 || caplen > 0xFF || ver < 0x0096 || ver > 0x0200)
            continue;                              /* garbage: wrong window */
        klog("usb: relocated xHCI BAR to %x (len %x, %d mmio windows mapped)",
             start, want_len, n_occ);
        return start;
    }
    for (int i = 0; i < n_occ && i < 6; i++)
        klog("pci mmio window %d: %x + %x", i, occ[i].base, occ[i].len);
    return 0;
}

/* --------------------------------------------------------- enumeration ---- */
static u32 portsc(int port)
{
    volatile u32 *ps = (volatile u32 *)(op + 0x400 + 0x10 * (port - 1));
    return ps[0];
}

static void port_reset(int port)
{
    volatile u32 *ps = (volatile u32 *)(op + 0x400 + 0x10 * (port - 1));
    /* PORTSC mixes RW, RW1S and RW1C (PED, PRC, PLC, CEC) bits: every write
     * below sets ONLY the bits it means, never a read-modify-write, or a
     * completed reset's PED=1 gets written back as 1 and disables the port
     * (the exact failure seen on the H510M-A: "reset failed (not enabled)"). */
    ps[0] = 1u << 9;                      /* PP: port power */
    for (int i = 0; i < 4; i++)           /* clear stale change bits (CSC,
                                             OCC, PRC, PLC, CEC), keep PP */
        ps[0] = (1u << 18) | (1u << 20) | (1u << 21) | (1u << 22) | (1u << 23) | (1u << 9);
    /* PR with PP held on: on controllers where PP is a plain RW bit (not
     * write-1-only), a bare PR write would cut port power mid-reset - the
     * device drops off the bus and the reset completes with PED=0. That is
     * the H510M-A signature: every port "reset failed, device not enabled"
     * while the keyboard/mouse stay dark. */
    ps[0] = (1u << 4) | (1u << 9);        /* PR + PP */
    u64 t0 = now_ms();
    while (now_ms() - t0 < 400 && !(ps[0] & (1u << 21))) cpu_hlt();
    ps[0] = (1u << 21) | (1u << 9);       /* clear PRC (PED survives), keep PP */
}

static int port_speed(int port) { return (int)((portsc(port) >> 10) & 0xF); }

static int enumerate_port(int port)
{
    if (!(portsc(port) & 1)) return 0;    /* CCS */
    int enabled = 0;
    for (int attempt = 0; attempt < 3 && !enabled; attempt++) {
        port_reset(port);
        enabled = (portsc(port) & 2) != 0;   /* PED */
        if (!enabled && attempt < 2) {
            /* some controllers/ports need a power cycle before the reset
             * sticks: PP off, brief pause, PP on, debounce, clear changes */
            klog("usb: port %d reset attempt %d failed (portsc %08x) - power cycling",
                 port, attempt + 1, portsc(port));
            volatile u32 *ps = (volatile u32 *)(op + 0x400 + 0x10 * (port - 1));
            ps[0] = 0;                       /* RW bits off (PP, PR); W1C untouched */
            sleep_ms(30);
            ps[0] = 1u << 9;                 /* PP on, alone */
            sleep_ms(120);                   /* debounce */
            ps[0] = (1u << 21) | (1u << 22) | (1u << 23);
            if (!(portsc(port) & 1)) return 0;   /* device went away */
        }
    }
    if (!enabled) {
        u32 psc = portsc(port);
        klog("usb: port %d reset failed after 3 attempts", port);
        klog("usb:   portsc %08x: ccs=%d ped=%d prc=%d csc=%d pls=%u speed=%u pp=%d",
             psc, (int)(psc & 1), (int)((psc >> 1) & 1), (int)((psc >> 21) & 1),
             (int)((psc >> 18) & 1), (unsigned)((psc >> 5) & 0xF),
             (unsigned)((psc >> 10) & 0xF), (int)((psc >> 9) & 1));
        fail_flag = 1;
        return 0;
    }
    int speed = port_speed(port);
    klog("usb: port %d connected, speed %d", port, speed);
    if (speed < 1 || speed > 4) return 0;

    int rc = run_cmd(0, 0, 0, (u32)(TRB_ENABSLOT << 10), 500);
    if (rc != 1) {
        klog("usb: enable slot failed (code %d usbsts %x)", rc,
             *(volatile u32 *)(op + 4));
        return 0;
    }
    int slot = (int)cc_slot;
    if (slot < 1 || slot > MAX_SLOTS) {
        klog("usb: slot %d unusable (max %d)", slot, MAX_SLOTS);
        return 0;
    }
    if (devs[slot].used) disable_slot(slot);   /* stale entry: free and reuse */

    struct xdev *d = &devs[slot];
    d->used = 1;
    d->slot = slot;
    d->ep0 = (volatile u32 *)palloc(4096);
    devctx[slot] = palloc(csz * 4);
    inctx[slot] = palloc(csz * 34);
    if (!d->ep0 || !devctx[slot] || !inctx[slot]) { disable_slot(slot); return 0; }
    memset((void *)d->ep0, 0, 4096);
    memset(devctx[slot], 0, csz * 4);
    memset(inctx[slot], 0, csz * 34);
    d->ep0_idx = 0;
    d->ep0_cycle = 1;
    dcbaa[slot] = (u64)PA(devctx[slot]);

    u8 *ic = inctx[slot];
    ic[0] = 0x03;                                   /* add: slot + ep0 */
    u32 *slw = (u32 *)(ic + csz);
    /* Slot ctx dw0: speed [23:20], Context Entries [31:27] = 1 (EP0 only).
     * dw1: Root Hub Port Number [23:16]. (Entries used to be OR-ed into
     * dw1 bit 27 - reserved there, and 0 entries in dw0 made every
     * Address Device command invalid.) */
    slw[0] = ((u32)(speed & 0xF) << 20) | (1u << 27);
    slw[1] = ((u32)port & 0xFF) << 16;
    /* USB spec: LS control EP is ALWAYS 8 bytes; HS always 64; SS 2^9.
     * FS may be 8/16/32/64 - guess 64 and correct via Evaluate Context
     * after the first descriptor bytes (same as Linux). */
    u32 mps = (speed == 4) ? 9 : (speed == 2) ? 8 : 64;
    u32 *epw = (u32 *)(ic + csz * 2);               /* ep ctx index 2 = EP ID 0 */
    /* EP ctx (xHCI 6.2.3): dw1 = EP Type [5:3] (4 = control), CErr [2:1],
     * Max Packet Size [31:16]; dw2/3 = dequeue | DCS; dw4 = Avg TRB Len */
    epw[0] = 0;                                     /* state: disabled */
    epw[1] = (4u << 3) | (3u << 1) | ((u32)mps << 16);
    epw[2] = PA(d->ep0) | 1;
    epw[3] = 0;
    epw[4] = 8;

    rc = run_cmd(PA(ic), 0, 0,
                 (u32)(TRB_ADDRDEV << 10) | ((u32)slot << 24), 800);
    if (rc != 1) {
        klog("usb: address device failed (port %d code %d)", port, rc);
        disable_slot(slot);
        return 0;
    }

    int drc = ctrl_xfer(slot, 0x80, 6, 0x0100, 0, desc_buf, 18, 1);
    if ((drc != 1 && drc != 12 && drc != 13)) {   /* 13 = Success with Short Packet: normal
                                    * when the device's EP0 packets are
                                    * smaller than the 64 we assumed */
        klog("usb: device descriptor failed slot %d (code %d)", slot, drc);
        disable_slot(slot);
        return 0;
    }
    {   /* FS/LS devices cut the fetch short at their real EP0 max packet
         * (usually 8): honor it and refetch, or the config descriptor read
         * desyncs the pipe */
        u32 mps0 = desc_buf[7];
        u32 assumed = (speed == 2) ? 8 : 64;
        if (speed != 4 && mps0 != assumed &&
            (mps0 == 8 || mps0 == 16 || mps0 == 32 || mps0 == 64)) {
            if (eval_ep0_mps(slot, mps0) == 1) {
                drc = ctrl_xfer(slot, 0x80, 6, 0x0100, 0, desc_buf, 18, 1);
                if ((drc != 1 && drc != 12 && drc != 13)) {
                    klog("usb: descriptor refetch failed slot %d", slot);
                    disable_slot(slot);
                    return 0;
                }
            }
        }
    }
    drc = ctrl_xfer(slot, 0x80, 6, 0x0200, 0, desc_buf, 9, 1);
    if ((drc != 1 && drc != 12 && drc != 13)) {
        klog("usb: config descriptor failed slot %d (code %d)", slot, drc);
        disable_slot(slot); return 0;
    }
    if (desc_buf[4] == 9) {
        klog("usb: port %d is a HUB - devices behind hubs are not walked yet",
             port);
        hub_count++;
        fail_flag = 1;
        disable_slot(slot);
        return 3;
    }
    u16 tot = (u16)(desc_buf[2] | (desc_buf[3] << 8));
    if (tot > sizeof(desc_buf)) tot = sizeof(desc_buf);
    drc = ctrl_xfer(slot, 0x80, 6, 0x0200, 0, desc_buf, tot, 1);
    if ((drc != 1 && drc != 12 && drc != 13)) {
        klog("usb: full config descriptor failed slot %d (code %d)", slot, drc);
        disable_slot(slot); return 0;
    }
    u8 cfg_val = desc_buf[5];
    int iface = -1, proto = -1, ep_addr = 0, ep_mps = 8;
    int off = 9;
    while (off + 2 <= (int)tot) {
        u8 dl = desc_buf[off];
        if (!dl) break;
        u8 dt = desc_buf[off + 1];
        if (dt == 4 && dl >= 9) {
            if (desc_buf[off + 5] == 3 && desc_buf[off + 6] == 1 &&
                (desc_buf[off + 7] == 1 || desc_buf[off + 7] == 2)) {
                iface = desc_buf[off + 2];
                proto = desc_buf[off + 7];
                ep_addr = 0;
            } else iface = -1;
        } else if (dt == 5 && dl >= 7 && iface >= 0 && !ep_addr) {
            u8 ea = desc_buf[off + 2];
            if (ea & 0x80) {
                ep_addr = ea & 0xF;
                ep_mps = desc_buf[off + 4] | (desc_buf[off + 5] << 8);
            }
        }
        off += dl;
    }
    if (iface < 0 || !ep_addr) {
        klog("usb: slot %d not a boot HID device", slot);
        disable_slot(slot);
        return 0;
    }
    ctrl_xfer(slot, 0x00, 9, cfg_val, 0, 0, 0, 0);         /* set configuration */
    ctrl_xfer(slot, 0x21, 0x0B, 0, (u16)iface, 0, 0, 0);   /* boot protocol */
    ctrl_xfer(slot, 0x21, 0x0A, 0, (u16)iface, 0, 0, 0);   /* set idle */
    if (proto == 1) {
        u8 leds = 0;   /* BIOS often leaves NumLock lit: SCos has no NumLock
                        * state, so drive the LEDs off explicitly */
        ctrl_xfer(slot, 0x21, 0x09, 0x0200, (u16)iface, &leds, 1, 0);
    }

    d->iface = iface;
    d->ep_addr = ep_addr;
    d->kind = (proto == 2) ? 2 : 1;

    d->inr = (volatile u32 *)palloc(4096);
    if (!d->inr) { disable_slot(slot); return 0; }
    memset((void *)d->inr, 0, 4096);
    for (int b = 0; b < IN_TRBS; b++) d->in_buf[b] = palloc(16);
    d->inr_idx = 0;
    d->inr_cycle = 1;

    u32 dci = (u32)(ep_addr * 2 + 1);        /* EP 0x81 -> DCI 3, 0x82 -> 5 */
    memset(ic, 0, csz * 34);
    ic[0] = 0x01 | (1u << dci);              /* Add: slot ctx + this EP ctx */
    slw = (u32 *)(ic + csz);
    slw[0] = ((u32)(speed & 0xF) << 20) | (dci << 27); /* Context Entries = last DCI */
    slw[1] = ((u32)port & 0xFF) << 16;
    epw = (u32 *)(ic + csz * (dci + 1));     /* input ctx array: index DCI+1 */
    /* dw0: Interval [23:16] = 3 (4 ms FS / 250 us HS polling);
     * dw1: EP Type [5:3] = 6 (Interrupt IN), CErr [2:1] = 3, MPS [31:16] */
    epw[0] = (3u << 16);
    epw[1] = (6u << 3) | (3u << 1) | ((u32)(ep_mps & 0xFFFF) << 16);
    epw[2] = PA(d->inr) | 1;
    epw[3] = 0;
    epw[4] = 16;
    rc = run_cmd(PA(ic), 0, 0,
                 (u32)(TRB_CFGEP << 10) | ((u32)slot << 24), 800);
    if (rc != 1) {
        klog("usb: configure endpoint failed slot %d (code %d)", slot, rc);
        fail_flag = 1;
        disable_slot(slot);
        return 0;
    }

    /* pre-queue interrupt IN TRBs (all usable slots), link at the end */
    for (int b = 0; b < IN_TRBS - 1; b++) {
        volatile u32 *tr = d->inr + (u32)b * 4;
        tr[0] = PA(d->in_buf[b]);
        tr[1] = 0;
        tr[2] = 16;
        /* IOC: get a completion event per report; ISP: also complete on
         * short packets - an 8-byte keyboard report into a 16-byte buffer
         * is ALWAYS short, without ISP no event ever fires and input is
         * silently dead */
        tr[3] = (TRB_NORMAL << 10) | (1u << 5) | (1u << 2) | d->inr_cycle;
    }
    ring_link(d->inr, IN_TRBS - 1, d->inr_cycle);
    d->inr_cycle ^= 1;
    ring_db((u32)slot, (u32)(d->ep_addr * 2 + 1));     /* doorbell = EP ID */
    n_devs++;
    klog("usb: slot %d = HID %s (ep %d mps %d)", slot,
         d->kind == 2 ? "mouse" : "keyboard", ep_addr, ep_mps);
    return d->kind;
}

/* -------------------------------------------------------------- public ---- */
void usb_kbd_leds_off(void)
{
    if (!have_xhci) return;
    for (int sl = 1; sl <= MAX_SLOTS; sl++) {
        struct xdev *d = &devs[sl];
        if (d->used && d->kind == 1) {
            u8 leds = 0;
            /* HID SET_REPORT(Output) so no LED stays lit on USB standby
             * power after shutdown */
            ctrl_xfer(sl, 0x21, 0x09, 0x0200, (u16)d->iface, &leds, 1, 0);
        }
    }
}

void usb_poll(void)
{
    static u32 hb;
    if (!have_xhci) return;
    proc_events();
    if (tick_count - hb < 100) return;      /* 1 Hz heartbeat */
    hb = tick_count;
    if (pending_portc) {                    /* real hotplug, post-boot only */
        u32 m = pending_portc;
        pending_portc = 0;
        for (int p = 1; p <= max_ports && p < 32; p++)
            if (m & (1u << p)) enumerate_port(p);
    }
    for (int sl = 1; sl <= MAX_SLOTS; sl++) {
        struct xdev *d = &devs[sl];
        if (!d->used || !d->reported || d->silent_logged) continue;
        if (tick_count - d->last_rep_tick > 500) {
            d->silent_logged = 1;
            klog("usb: slot %d silent - no reports for 5 s (pipe stalled?)", sl);
        }
    }
}

void usb_init(void)
{
    have_xhci = 0;
    n_devs = 0;
    status_line[0] = 0;
    u8 bus[8], dev[8], fn[8];
    int n = pci_find_class(0x0C, 0x03, 0x30, bus, dev, fn, 8);
    if (!n) n = pci_find_class(0x0C, 0x03, 0xFF, bus, dev, fn, 8);
    if (!n) {
        pci_scan_dump();   /* put the whole bus in the log for the photo */
        strcpy(status_line, "usb: no xHCI controller - PS/2 input only");
        klog("%s", status_line);
        return;
    }
    u32 bar0 = pci_read32(bus[0], dev[0], fn[0], 0x10);
    u32 bar0h = pci_read32(bus[0], dev[0], fn[0], 0x14);
    if ((bar0 & 0x6) == 0x4 && bar0h) {
        klog("usb: BAR0 = %x:%x (64-bit, above 4G) - relocating into 32-bit hole",
             bar0h, bar0 & ~0xFu);
        u32 lo = bar0 & 0xFFFFFFF0u;
        u32 len = 0;
        {   /* size of the xHCI window itself */
            pci_write32(bus[0], dev[0], fn[0], 0x10, 0xFFFFFFFFu);
            pci_write32(bus[0], dev[0], fn[0], 0x14, 0xFFFFFFFFu);
            u32 mlo = pci_read32(bus[0], dev[0], fn[0], 0x10) & 0xFFFFFFF0u;
            u32 mhi = pci_read32(bus[0], dev[0], fn[0], 0x14);
            pci_write32(bus[0], dev[0], fn[0], 0x10, lo | (bar0 & 0xF));
            pci_write32(bus[0], dev[0], fn[0], 0x14, bar0h);
            if (mhi != 0xFFFFFFFFu) {
                u64 sz = ~(((u64)mhi << 32) | mlo) + 1;
                if (sz && sz <= 0xFFFFFFFFu) len = (u32)sz;
            }
        }
        u32 nb = relocate_bar(dev[0], bar0, len);
        if (!nb) {
            strcpy(status_line,
                   "usb: xHCI above 4 GB, no free 32-bit window - disable Above 4G Decoding in BIOS");
            fail_flag = 1;
            klog("%s", status_line);
            return;
        }
        bar0 = nb | (bar0 & 0xFu);
        bar0h = 0;
    }
    if (bar0 & 0x1) {          /* bit0 set = IO-space BAR (type bits 0b10 =
                                * 64-bit memory are perfectly valid here) */
        strcpy(status_line, "usb: xHCI BAR is IO-space - unsupported");
        klog("%s (bar0 %x)", status_line, bar0);
        return;
    }
    if (!(bar0 & ~0xFu)) {
        strcpy(status_line, "usb: xHCI BAR unassigned by firmware");
        klog("%s", status_line);
        return;
    }
    pci_write32(bus[0], dev[0], fn[0], 0x04,
                pci_read32(bus[0], dev[0], fn[0], 0x04) | 0x06);
    cap = (volatile u8 *)(bar0 & ~0xFu);
    u32 caplen = *(volatile u32 *)cap & 0xFF;
    u32 hcs1 = *(volatile u32 *)(cap + 4);
    u32 hcs2 = *(volatile u32 *)(cap + 8);
    u32 maxsp = (hcs2 >> 21) & 0x1F;      /* Max Scratchpad Buffers */
    u32 hcc1 = *(volatile u32 *)(cap + 0x10);
    u32 dboff = *(volatile u32 *)(cap + 0x14) & ~0x3u;
    u32 rtsoff = *(volatile u32 *)(cap + 0x18) & ~0x1Fu;
    csz = (hcc1 & 4) ? 64 : 32;
    max_ports = (int)((hcs1 >> 24) & 0xFF);
    if (max_ports > 15) max_ports = 15;
    op = cap + caplen;
    db = cap + dboff;
    rt = cap + rtsoff;
    klog("usb: xHCI at %x ports %d ctx %d", (u32)cap, max_ports, csz);

    /* BIOS/SMM ownership handoff (USB legacy support extended capability).
     * Without this, firmware that traps USB for "legacy support" keeps
     * generating SMIs and can hold the controller hostage. */
    {
        u32 xecp = (hcc1 >> 16) & 0xFFFF;
        int guard = 0;
        while (xecp >= 8 && xecp * 4 < 0x4000 && guard++ < 32) {
            volatile u32 *ec = (volatile u32 *)(cap + xecp * 4);
            u32 id = ec[0] & 0xFF;
            u32 next = (ec[0] >> 8) & 0xFF;
            if (id == 1) {                    /* USB legacy support */
                klog("usb: USBLEGSP found - taking ownership from BIOS");
                ec[0] |= 1u << 24;            /* OS owned */
                u64 t0 = now_ms();
                while (now_ms() - t0 < 400 && (ec[0] & (1u << 16))) cpu_hlt();
                if (ec[0] & (1u << 16)) {
                    klog("usb: BIOS did not release SMM ownership");
                    fail_flag = 1;
                } else {
                    klog("usb: SMM handoff complete");
                }
                ec[1] = 0;                    /* disable all legacy SMI enables */
            }
            if (!next) break;
            xecp += next;
        }
    }

    volatile u32 *cmd = (volatile u32 *)op;
    volatile u32 *sts = (volatile u32 *)(op + 4);
    if (cmd[0] & 1) {
        cmd[0] &= ~1u;
        u64 t0 = now_ms();
        while (now_ms() - t0 < 100 && !(sts[0] & 1u)) cpu_hlt();  /* HCHalted */
    }
    cmd[0] |= 1u << 1;                       /* HCRST */
    u64 t0 = now_ms();
    while (now_ms() - t0 < 500 && (cmd[0] & (1u << 1))) cpu_hlt();
    if (cmd[0] & (1u << 1)) {
        strcpy(status_line, "usb: xHCI reset timed out");
        klog("%s", status_line);
        return;
    }
    int slots = (int)(hcs1 & 0xFF);
    if (slots > MAX_SLOTS) slots = MAX_SLOTS;
    ((volatile u32 *)(op + 0x38))[0] = (u32)slots;

    dcbaa = (volatile u64 *)palloc(4096);
    memset((void *)dcbaa, 0, 4096);
    if (maxsp) {
        /* DCBAA entry 0 must point at the scratchpad buffer array or every
         * command fails with a Host Controller Error on controllers that
         * declare MaxScratchpadBuffers > 0 */
        u64 *spa = (u64 *)palloc(4096);
        if (spa) {
            for (u32 i = 0; i < maxsp; i++) {
                void *sb = palloc(4096);
                spa[i] = sb ? (u64)PA(sb) : 0;
            }
            dcbaa[0] = (u64)PA(spa);
            klog("usb: %u scratchpad buffers allocated", maxsp);
        }
    }
    wr64((volatile u32 *)(op + 0x30), (u64)PA(dcbaa));

    cmd_ring = (volatile u32 *)palloc(4096);
    memset((void *)cmd_ring, 0, 4096);
    cmd_idx = 0; cmd_cycle = 1;
    wr64((volatile u32 *)(op + 0x18), (u64)PA(cmd_ring) | 1);

    evt_ring = (volatile u32 *)palloc(4096);
    memset((void *)evt_ring, 0, 4096);
    evt_idx = 0; evt_cycle = 1;
    /* NO Link TRB here on purpose: the xHC wraps the event ring per the
     * ERST segment size (spec 4.9.3/4.9.4), it does not chase in-band
     * links. A static link at slot 63 gets overwritten by event #64 while
     * the consumer skips that slot - producer and consumer wrap out of
     * phase, the ring jams Full, and every later command dies with
     * completion code 17 (Event Ring Full Error). */
    u32 *erst = palloc(4096);
    memset(erst, 0, 4096);
    erst[0] = PA(evt_ring);
    erst[1] = 0;
    erst[2] = EVT_TRBS;
    erst[3] = 0;
    volatile u32 *ir = (volatile u32 *)(rt + 0x20);
    ir[2] = 1;                               /* ERSTSZ (0x2C is reserved:
                                                never write it) */
    wr64(ir + 4, (u64)PA(erst));             /* ERSTBA */
    wr64(ir + 6, (u64)PA(evt_ring));         /* ERDP */

    cmd[0] = 1;                              /* run */
    t0 = now_ms();
    while (now_ms() - t0 < 100 && (sts[0] & 1u)) cpu_hlt();       /* HCHalted */
    if (sts[0] & 1u) {
        strcpy(status_line, "usb: xHCI would not start");
        klog("%s (usbsts %x)", status_line, sts[0]);
        return;
    }
    if (sts[0] & (1u << 8))                  /* STS_HCE - bit 8, not 12 */
        klog("usb: WARNING host controller error (usbsts %x)", sts[0]);
    have_xhci = 1;

    /* Power EVERY port unconditionally, like every real OS does. Connect
     * status is electrical: an unpowered device cannot pull up, so waiting
     * for CCS before powering (the old order) deadlocked at zero devices
     * and left keyboards/mice dark. VBUS ramp + attach debounce ~200 ms. */
    for (int p = 1; p <= max_ports; p++) {
        volatile u32 *ps = (volatile u32 *)(op + 0x400 + 0x10 * (p - 1));
        ps[0] = 1u << 9;        /* PP alone: PORTSC has W1C bits, never RMW */
    }
    sleep_ms(250);
    {
        int pw = 0, cc = 0;
        for (int p = 1; p <= max_ports; p++) {
            u32 v = portsc(p);
            if (v & (1u << 9)) pw++;
            if (v & 1) cc++;
        }
        klog("usb: ports powered %d/%d, connected %d", pw, max_ports, cc);
    }

    int mk = 0, mm = 0;
    for (int p = 1; p <= max_ports; p++) {
        if (!(portsc(p) & 1)) continue;
        int k = enumerate_port(p);
        if (k == 1) mk++;
        if (k == 2) mm++;
    }
    pending_portc = 0;   /* our own boot-time resets queued stale changes */
    char nl[96], tmp[8];
    strcpy(nl, "usb: xHCI live - ");
    fmt_u32(tmp, (u32)mk); strcat(nl, tmp); strcat(nl, " keyboard, ");
    fmt_u32(tmp, (u32)mm); strcat(nl, tmp); strcat(nl, " mouse (HID boot)");
    if (hub_count) {
        strcat(nl, ", "); fmt_u32(tmp, (u32)hub_count); strcat(nl, tmp);
        strcat(nl, " hub(s)");
    }
    if (fail_flag) strcat(nl, " - see diagnostics");
    strcpy(status_line, nl);
    klog("%s", status_line);
}

int usb_diag_flag(void)
{
    return have_xhci && (n_devs == 0 || fail_flag);
}

void usb_status(char *out, int max)
{
    strncpy(out, status_line[0] ? status_line : "usb: not probed", max - 1);
    out[max - 1] = 0;
}

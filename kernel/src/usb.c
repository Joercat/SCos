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

#define MAX_SLOTS 12   /* hub + children + onboard devices need headroom */
#define CMD_TRBS 128
#define EVT_TRBS 256   /* 4 KiB segment, same as Linux: 64 filled up in the
                        * port-power link-training storm on the H510M-A */
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

#define MAX_HID_EPS 3

/* one boot-protocol HID interface of a device.  Composite devices are the
 * NORM, not the exception (r29 field capture: the user's Holtek keyboard
 * presents boot-mouse iface 0 + boot-keyboard iface 1 + a vendor iface;
 * the Razer presents boot-keyboard iface 0 + consumer-control ifaces).
 * Every boot interface gets its own interrupt ring and state. */
struct hid_ep {
    int active;
    int iface;                /* bInterfaceNumber */
    int kind;                 /* 1 = boot keyboard, 2 = boot mouse */
    u8 ep_addr;               /* IN endpoint number (0x81 -> 1) */
    u8 ep_mps;
    u32 dci;                  /* endpoint context index = ep_addr*2+1 */
    volatile u32 *inr;
    int inr_idx; u32 inr_cycle;
    u8 link_pend;             /* deferred link-TRB rewrite pending (r28) */
    u32 link_pend_cycle;
    u8 *in_buf[IN_TRBS];
    u8 prev_mod;
    u8 prev_keys[6];
    u32 last_rep_tick;
    u8 reported, silent_logged, kick1;
    u32 arm_tick;             /* tick when this ring was armed */
};

struct xdev {
    int used;
    int slot;
    int kind;                 /* OR of hid[] kinds: 1 kbd, 2 mouse, 3 both */
    int nhid;                 /* boot-HID interfaces found (hid[0..nhid-1]) */
    struct hid_ep hid[MAX_HID_EPS];
    volatile u32 *ep0;
    int ep0_idx; u32 ep0_cycle;
    u32 trb_cycle;            /* cycle of the TRB ep0_next just handed out:
                               * captured BEFORE the wrap flip, because the
                               * caller stamps the TRB afterwards */
    /* round 25: topology - where this device hangs in the tree */
    u32 route;                /* xHCI route string (hub path) */
    u8 root_port;             /* root hub port number */
    u8 mtt;                   /* FS/LS child of a multi-TT HS hub */
    u8 tt_slot;               /* TT Hub Slot ID: slot of the HS hub this
                               * FS/LS device hangs under (0 = none) */
    u8 tt_port;               /* TT Port Number: our port on that hub */
    u8 speed;                 /* xHCI port speed id (1 FS 2 LS 3 HS 4 SS) */
    u8 tier;                  /* hub tiers above it (0 = on the root hub) */
    u8 is_hub;
    u8 hub_ports;             /* downstream ports (hub only) */
    u8 hub_mtt;               /* hub itself has multiple TTs */
    u8 hub_p2g;               /* bPwrOn2PwrGood, 2 ms units */
    u8 child_slot[8];         /* hub: slot id per downstream port */
    int dead_polls;           /* hub: consecutive 1 Hz polls with no answer */
};
static u8 port_slot[32];      /* root port -> live slot (hotplug guard) */
static int last_enum_slot;    /* slot of the last enumerate_device */
static int cc_want_slot;      /* run_cmd: only accept THIS slot's completion */

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
static char status_line[128];
/* ring-24 forensics: counted and shown on the status line / diagnostics */
static u32 evt_seen;        /* events consumed from the ring */
static u32 evt_hcevent;     /* Host Controller Events (TRB type 37) */
static u32 ring_full_hits;  /* code-21 (Event Ring Full) sightings - 21 is
                             * the real ring-full code; 17 = Parameter Error
                             * (invalid input-context field, Intel-strict) */
static u32 restart_count;   /* xHC restarts after a halt */
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
static int enumerate_device(int root_port, u32 route, int speed, int mtt,
                            int tier, struct xdev *phub, int hport);
static void ring_db(u32 slot, u32 target);

/* ------------------------------------------------------------- events ---- */
static int proc_events(void)
{
    int work = 0;
    for (int guard = 0; guard < 128; guard++) {
        volatile u32 *t = evt_ring + (u32)evt_idx * 4;
        if ((t[3] & 1) != evt_cycle) break;
        u32 type = (t[3] >> 10) & 0x3F;
        work = 1;
        evt_seen++;
        if (type == 37) {
            /* Host Controller Event: posted when the xHC could not write
             * an event earlier (ring full). dw2[31:24] carries the code. */
            u32 hc = (t[2] >> 24) & 0xFF;
            evt_hcevent++;
            if (hc == 21) ring_full_hits++;
            klog("usb: HC event code %u (ring-full total %u)",
                 hc, ring_full_hits);
        } else if (type == EV_CMDCOMP) {
            cc_code = (t[2] >> 24) & 0xFF;      /* completion code: dw2[31:24] */
            cc_slot = (t[3] >> 24) & 0xFF;      /* slot id: dw3[31:24] (the
                                                 * TRB type is dw3[15:10]=33;
                                                 * dw0/1 is the command TRB
                                                 * pointer, NOT the slot!) */
            cc_valid = 1;
            if (cc_code == 21) ring_full_hits++;
        } else if (type == EV_TRANSFER) {
            u32 slot = (t[3] >> 24) & 0xFF;
            u32 ep = (t[3] >> 16) & 0x1F;
            u32 code = (t[2] >> 24) & 0xFF;
            u32 rem = t[2] & 0xFFFFFF;       /* bytes NOT transferred */
            u32 len = rem <= 16u ? 16u - rem : 0u;
            u32 ptr = t[0];
            cc_code = code;
            cc_slot = slot | (ep << 8) | 0x10000u;
            /* per-HID-interface rings: an event's EP field (DCI) selects
             * which boot interface completed.  The pre-r29 code keyed off
             * a single per-device ep_addr and scribbled low memory when
             * the ring was not armed - the guard now demands an ACTIVE
             * hid_ep whose DCI matches. */
            if (slot <= MAX_SLOTS && devs[slot].used) {
                struct xdev *d = &devs[slot];
                for (int j = 0; j < d->nhid; j++) {
                    struct hid_ep *h = &d->hid[j];
                    if (!h->active || !h->inr || ep != h->dci) continue;
                    if (code == 1 || code == 12 || code == 13) {
                        int i = -1;
                        for (int b = 0; b < IN_TRBS; b++)
                            if (PA(h->in_buf[b]) == ptr) i = b;
                        if (i >= 0) {
                            u8 *r = h->in_buf[i];
                            h->last_rep_tick = tick_count;
                            if (!h->reported) {
                                h->reported = 1;
                                klog("usb: first report from slot %d (%s "
                                     "iface %d)", slot,
                                     h->kind == 2 ? "mouse" : "keyboard",
                                     h->iface);
                            }
                            if (h->kind == 2 && len >= 3)
                                mouse_inject(r[0], (i32)(i8)r[1],
                                             (i32)(i8)r[2],
                                             len >= 4 ? (i32)(i8)r[3] : 0);
                            else if (h->kind == 1 && len >= 8)
                                kbd_inject_hid(r[0], r + 2, h->prev_keys,
                                               &h->prev_mod);
                        }
                    }
                    /* Deferred link-TRB maintenance (r28): this event is
                     * from the new lap, so the consumer has provably
                     * passed the previous link - only now is rewriting
                     * it race-free.  Re-arm on EVERY completion (even
                     * error codes) or producer/consumer positions skew. */
                    if (h->link_pend) {
                        ring_link(h->inr, IN_TRBS - 1, h->link_pend_cycle);
                        h->link_pend = 0;
                    }
                    volatile u32 *tr = h->inr + (u32)h->inr_idx * 4;
                    tr[0] = PA(h->in_buf[h->inr_idx]);
                    tr[1] = 0;
                    tr[2] = 16;
                    /* IOC + ISP exactly like the pre-queued TRBs: without
                     * IOC no event fires, without ISP the always-short
                     * HID report never completes - input would die after
                     * the first ring lap */
                    tr[3] = (TRB_NORMAL << 10) | (1u << 5) | (1u << 2) |
                            h->inr_cycle;
                    h->inr_idx++;
                    if (h->inr_idx == IN_TRBS - 1) {
                        h->link_pend_cycle = h->inr_cycle;
                        h->link_pend = 1;
                        h->inr_idx = 0;
                        h->inr_cycle ^= 1;
                    }
                    /* Linux rings the doorbell on every interrupt
                     * re-queue: an endpoint that idled on a not-ready
                     * TRB only restarts on a doorbell */
                    ring_db(slot, h->dci);
                    break;
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

/* Sleep WHILE draining the event ring. Port resets and power-on settle
 * windows are exactly when the xHC posts its port-change / link-training
 * storm; plain sleep_ms there let the ring fill to the brim before the
 * first wait_event got a chance to drain (H510M-A: everything then died
 * with completion code 17). */
static void drain_ms(u32 ms)
{
    u64 t0 = now_ms();
    while (now_ms() - t0 < ms) { proc_events(); cpu_hlt(); }
}

static int wait_event(u64 timeout_ms)
{
    u64 t0 = now_ms();
    cc_code = 0xFF;
    cc_slot = 0;
    cc_valid = 0;
    while (now_ms() - t0 < timeout_ms) {
        proc_events();
        /* bit16 marks a TRANSFER event - not a command completion;
         * cc_want_slot rejects stale/foreign completions entirely */
        if (cc_valid && !(cc_slot & 0x10000u) &&
            (!cc_want_slot || (cc_slot & 0xFF) == (u32)cc_want_slot)) {
            if (cc_code == 21) {
                /* Event-Ring-Full Error completion: the command stalled
                 * while the ring was jammed; its true completion follows
                 * once space frees (we drain every loop). Keep waiting. */
                klog("usb: cmd completion code 21 (ring full) - waiting on");
                cc_valid = 0;
            } else {
                /* r25.2: code 17 is PARAMETER ERROR (the xHC rejected an
                 * input-context field), NOT ring full - decode it on the
                 * spot so the boot log names the real failure */
                if (cc_code == 17)
                    klog("usb: completion code 17 = PARAMETER ERROR"
                         " (invalid input-context field)");
                return (int)cc_code;
            }
        }
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
    restart_count++;
    klog("usb: xHC halted (usbsts %x) - restarting", sts[0]);
    cmdr[0] &= ~1u;                      /* RS=0 while re-priming */
    u64 t0 = now_ms();
    while (now_ms() - t0 < 100 && !(sts[0] & 1u)) cpu_hlt();
    /* xHCI spec 4.21: a halted controller has Command Ring Running = 0,
     * and setting RS alone does NOT restart the ring - software must
     * write CRCR with the current dequeue pointer + cycle state FIRST.
     * Skipping this is why every Enable Slot after "running again" timed
     * out on the H510M-A (code -1, usbsts 14) and the replugged mouse
     * never came back. */
    wr64((volatile u32 *)(op + 0x18),
         (u64)PA(cmd_ring + (u32)cmd_idx * 4) | (u64)(cmd_cycle & 1));
    cmdr[0] |= 1u;                       /* RS */
    t0 = now_ms();
    while (now_ms() - t0 < 200 && (sts[0] & 1u)) cpu_hlt();
    if (sts[0] & 1u) klog("usb: restart FAILED (usbsts %x)", sts[0]);
    else klog("usb: xHC running again (usbsts %x)", sts[0]);
}

static void ring_db(u32 slot, u32 target)
{
    volatile u32 *d = (volatile u32 *)(db + slot * 4);
    d[0] = target & 0xFF;
}

/* want_slot > 0: only a completion carrying that slot id is accepted -
 * a late completion from an earlier command must never be mistaken for
 * ours (it would poison both this wait and the next one) */
static int run_cmd(u32 d0, u32 d1, u32 d2, u32 d3, u64 timeout,
                   int want_slot)
{
    cc_want_slot = want_slot;
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
    cc_want_slot = 0;
    if (rc < 0) xhci_restart();          /* timed out: un-wedge if halted */
    return rc;
}

/* give an enabled-but-unusable slot back to the controller, or the slot
 * table fills up and later Enable Slot commands fail (No Slots/Bandwidth) */
static void disable_slot(int slot)
{
    if (slot >= 1 && slot <= MAX_SLOTS) {
        run_cmd(0, 0, 0, (u32)(TRB_DISABLESLOT << 10) | ((u32)slot << 24),
                500, slot);
        devs[slot].used = 0;
    }
}

/* the device's real EP0 max packet size differs from what we assumed at
 * Address Device time (full/low-speed HID devices usually say 8): patch the
 * controller's own EP0 context and Evaluate it, the same dance Linux does */
/* Evaluate Context is gone on purpose: on the H510M-A the only command
 * that ever failed was Evaluate Context (ep0 mps 8 -> completion code 17)
 * and its failure truncated every later descriptor fetch. The re-address
 * flow below (Disable Slot -> Enable Slot -> Address Device with the real
 * MPS) uses only commands proven to work on this board. */

/* ------------------------------------------------------- control pipe ---- */
/* THE ring-wrap bug that killed the hub on real hardware (r28 root cause):
 * the TRB handed out here belongs to the CURRENT lap and must be stamped
 * with the CURRENT cycle bit - but the caller stamps it AFTER this function
 * returns.  When the hand-out crosses the wrap boundary the old code had
 * already flipped ep0_cycle, so the last usable slot got the NEXT lap's
 * cycle bit.  The xHC consumer arrives expecting the old cycle, sees a
 * mismatch, decides the TRB is "not ready" and stops silently - every later
 * transfer on that ring times out (rc -1) with the device perfectly healthy.
 * With EP0_TRBS=16 the hub's 15th TRB (first SET_PORT_FEATURE after the
 * 14-TRB descriptor dance) landed exactly on that slot, every single time,
 * on every recovery re-enumeration.  v86 never caught it: no xHCI there,
 * and no root-port device issues enough EP0 TRBs to wrap a ring. */
static volatile u32 *ep0_next(struct xdev *d)
{
    volatile u32 *t = d->ep0 + (u32)d->ep0_idx * 4;
    d->trb_cycle = d->ep0_cycle;      /* lap this slot belongs to */
    d->ep0_idx++;
    if (d->ep0_idx == EP0_TRBS - 1) {
        ring_link(d->ep0, EP0_TRBS - 1, d->trb_cycle);
        d->ep0_idx = 0;
        d->ep0_cycle ^= 1;
    }
    return t;
}

/* to_ms: completion wait budget; kick: run xhci_restart() on timeout
 * (enumeration wants the un-wedge, the 1 Hz background probe must not
 * stall the window manager for ~1 s per silent port). */
static int ctrl_xfer_to(int slot, u8 rt_, u8 rq, u16 val, u16 idx,
                        u8 *buf, u16 len, int in, u32 to_ms, int kick)
{
    struct xdev *d = &devs[slot];
    volatile u32 *setup = ep0_next(d);
    setup[0] = (u32)rt_ | ((u32)rq << 8) | ((u32)val << 16);
    setup[1] = (u32)idx | ((u32)len << 16);
    setup[2] = 8;
    setup[3] = (TRB_SETUP << 10) | (1u << 6) |
               (len ? (in ? 3u : 2u) << 16 : 0) | d->trb_cycle;
    if (len) {
        volatile u32 *data = ep0_next(d);
        data[0] = PA(buf);
        data[1] = 0;
        data[2] = len;
        data[3] = (TRB_DATA << 10) | (in ? (1u << 16) : 0) | d->trb_cycle;
    }
    volatile u32 *stat = ep0_next(d);
    stat[0] = 0; stat[1] = 0; stat[2] = 0;
    stat[3] = (TRB_STATUS << 10) | (1u << 5) |
              (len ? (in ? 0u : (1u << 16)) : (1u << 16)) | d->trb_cycle;
    cc_code = 0xFF; cc_slot = 0; cc_valid = 0;
    ring_db((u32)slot, 1);      /* DCI 1 = default control pipe (EP0);
                                 * target 0 is RESERVED on slot doorbells
                                 * and silently never starts the TD */
    u64 t0 = now_ms();
    while (now_ms() - t0 < to_ms) {
        proc_events();
        if ((cc_slot & 0x10000u) && (cc_slot & 0xFF) == (u32)slot)
            return (int)cc_code;
        cpu_hlt();
    }
    if (kick) xhci_restart();            /* ring-full/halt recovery kick */
    return -1;
}

static int ctrl_xfer(int slot, u8 rt_, u8 rq, u16 val, u16 idx,
                     u8 *buf, u16 len, int in)
{
    return ctrl_xfer_to(slot, rt_, rq, val, idx, buf, len, in, 800, 1);
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
static void hex2(char *p, u8 v)
{
    static const char hx[] = "0123456789abcdef";
    p[0] = hx[v >> 4];
    p[1] = hx[v & 0xF];
}

/* raw descriptor bytes into the log: the r24.1 photo proved we need to
 * SEE what the device actually sent, not just our interpretation of it */
static void hexdump(const char *tag, int slot, const u8 *b, int n)
{
    char line[64];
    for (int i = 0; i < n; i += 16) {
        char *p = line;
        for (int j = i; j < i + 16 && j < n; j++) {
            hex2(p, b[j]);
            p += 2;
            *p++ = ' ';
        }
        *p = 0;
        klog("usb: %s slot %d +%02d: %s", tag, slot, i, line);
    }
}

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
    while (now_ms() - t0 < 400 && !(ps[0] & (1u << 21))) {
        proc_events();                    /* drain the change-event storm */
        cpu_hlt();
    }
    ps[0] = (1u << 21) | (1u << 9);       /* clear PRC (PED survives), keep PP */
}

static int port_speed(int port) { return (int)((portsc(port) >> 10) & 0xF); }

/* allocate + wire up the host-side context buffers for a fresh slot */
static struct xdev *slot_alloc(int slot)
{
    struct xdev *d = &devs[slot];
    d->used = 1;
    d->slot = slot;
    d->kind = 0;
    d->nhid = 0;
    memset(d->hid, 0, sizeof(d->hid));
    for (int j = 0; j < MAX_HID_EPS; j++) d->hid[j].iface = -1;
    d->ep0 = (volatile u32 *)palloc(4096);
    devctx[slot] = palloc(csz * 4);
    inctx[slot] = palloc(csz * 34);
    if (!d->ep0 || !devctx[slot] || !inctx[slot]) return NULL;
    memset((void *)d->ep0, 0, 4096);
    memset(devctx[slot], 0, csz * 4);
    memset(inctx[slot], 0, csz * 34);
    memset(d->child_slot, 0, sizeof(d->child_slot));
    d->ep0_idx = 0;
    d->ep0_cycle = 1;
    d->trb_cycle = 1;
    /* stale-state guard: a slot reused after a hub or hub-child device
     * must not keep the old hub/TT topology fields */
    d->is_hub = 0;
    d->hub_ports = 0;
    d->hub_mtt = 0;
    d->hub_p2g = 0;
    d->tt_slot = 0;
    d->tt_port = 0;
    dcbaa[slot] = (u64)PA(devctx[slot]);
    return d;
}

/* Address Device with the slot's saved topology and the given EP0 max
 * packet size.  Linux (xhci_setup_addressable_virt_dev) semantics, which
 * the Intel xHC enforces:
 *  - the Hub bit is NEVER set here: Address Device with Hub=1 and no
 *    Number-of-Ports field is a Parameter Error (code 17) on Intel xHCI
 *    1.2 - hubs announce themselves later via Configure Endpoint
 *    (hub_enable), exactly like Linux's xhci_update_hub_device
 *  - the route string is only evaluated by the xHC for SuperSpeed
 *  - FS/LS children of an external HS hub carry TT info in slot dw2
 *    (TT Hub Slot ID | TT Port Number << 8) for split transactions
 *  - input control context: dw0 = DROP flags, dw1 = ADD flags (xHCI
 *    spec 6.2.5 / Linux struct xhci_input_control_ctx).  r25.1 wrote
 *    the add mask into dw0: Address Device tolerates that on Intel,
 *    Configure Endpoint does not. */
/* bsr = Block Set Request: build the slot WITHOUT sending SET_ADDRESS
 * on the wire (the device stays reachable at the default address) */
static int address_slot(struct xdev *d, u32 mps0, int bsr)
{
    u8 *ic = inctx[d->slot];
    ((u32 *)ic)[0] = 0;                  /* Drop Context Flags (dw0) */
    ((u32 *)ic)[1] = 0x03;               /* ADD: slot ctx + ep0 (dw1) */
    u32 *slw = (u32 *)(ic + csz);
    /* dw0: Route String [19:0] (SS only), Speed [23:20], MTT [25],
     * Context Entries [31:27] = 1 (EP0 only)
     * dw1: Root Hub Port [23:16] - the ROOT port, even for hub children
     * dw2: TT Hub Slot [7:0] | TT Port [15:8] (FS/LS behind a HS hub) */
    slw[0] = ((d->speed == 4) ? (d->route & 0xFFFFFu) : 0u) |
             ((u32)(d->speed & 0xF) << 20) |
             ((u32)(d->mtt & 1) << 25) | (1u << 27);
    slw[1] = ((u32)d->root_port & 0xFF) << 16;
    slw[2] = (u32)d->tt_slot | ((u32)d->tt_port << 8);
    slw[3] = 0;                          /* dev_state: xHC-owned */
    u32 *epw = (u32 *)(ic + csz * 2);
    epw[0] = 0;                          /* state: disabled */
    epw[1] = (4u << 3) | (3u << 1) | ((mps0 & 0xFFFF) << 16);
    /* dequeue = the ring's CURRENT position + its cycle state, exactly
     * like Linux (new_ring->dequeue): the real Address Device runs after
     * the BSR bootstrap already fetched 8 bytes, so pointing back at
     * TRB 0 would make the xHC re-execute those stale cycle-1 TRBs and
     * the 18-byte fetch would "complete" from the replayed short TD */
    epw[2] = PA(d->ep0 + (u32)d->ep0_idx * 4) | d->ep0_cycle;
    epw[3] = 0;
    /* Tx Info dw4 = Max Burst Size[7:0] | Avg TRB Length[31:16]: burst
     * is SuperSpeed-only; Linux writes 0 for every non-SS endpoint (the
     * old 8 here was tolerated by the H510M-A but is out-of-spec) */
    epw[4] = 0;
    return run_cmd(PA(ic), 0, 0,
                   (u32)(TRB_ADDRDEV << 10) | ((u32)d->slot << 24) |
                   (bsr ? (1u << 9) : 0u), 800, d->slot);
}

/* ------------------------------------------------------------- hub bits ---- */
/* One hub wire transfer with Linux-style retry + backoff: a cheap hub
 * restarting its port state machines goes briefly deaf, and a single
 * attempt cannot tell "busy" from "dead".  Final failures are logged
 * with the completion code so the boot photo names the culprit:
 * 4 = device not answering, 5 = TRB error (our ring), 6 = hub stalled
 * the request, -1 = no completion at all (timeout). */
static int hub_xfer(int slot, u8 rt, u8 rq, u16 val, u16 idx,
                    u8 *buf, u16 len, int in, int tries)
{
    int rc = -1;
    for (int i = 0; i < tries; i++) {
        rc = ctrl_xfer(slot, rt, rq, val, idx, buf, len, in);
        if (rc == 1 || rc == 12 || rc == 13) return 1;
        if (i + 1 < tries) drain_ms((u32)(80 + i * 160));
    }
    klog("usb: hub slot %d req %02x/%u idx %u: rc %d after %d tries",
         slot, (u32)rt, (u32)rq, (u32)idx, rc, tries);
    return rc;
}

/* GET_PORT_STATUS with retry - for one-shot bring-up paths.  The 1 Hz
 * poll uses the single-shot hub_port_status so a deaf hub cannot stall
 * the window manager (its dead_polls counter triggers recovery). */
static int hub_port_status_r(int hslot, int p, u32 *st, u32 *chg)
{
    int rc = hub_xfer(hslot, 0xA3, 0, 0, (u16)p, desc_buf, 4, 1, 3);
    if (rc != 1) return rc;
    *st = (u32)(desc_buf[0] | (desc_buf[1] << 8));
    *chg = (u32)(desc_buf[2] | (desc_buf[3] << 8));
    return 1;
}

static int hub_port_status(int hslot, int p, u32 *st, u32 *chg)
{
    /* background 1 Hz probe: SHORT budget and no controller restart -
     * this runs inside the WM loop (and inside the diag/error hold
     * loops); an 800 ms stall + restart per silent port froze the whole
     * UI at a ~3 s cadence behind a deaf hub (r28 field report) */
    int rc = ctrl_xfer_to(hslot, 0xA3, 0, 0, (u16)p, desc_buf, 4, 1, 120, 0);
    if (rc != 1 && rc != 12 && rc != 13) return rc;
    *st = (u32)(desc_buf[0] | (desc_buf[1] << 8));
    *chg = (u32)(desc_buf[2] | (desc_buf[3] << 8));
    return 1;
}

static int hub_port_reset(struct xdev *h, int p)
{
    hub_xfer(h->slot, 0x23, 3, 4, (u16)p, 0, 0, 0, 3);   /* PORT_RESET */
    u64 t0 = now_ms();
    int deaf = 0;
    while (now_ms() - t0 < 600) {
        drain_ms(5);
        u32 st = 0, chg = 0;
        if (hub_port_status(h->slot, p, &st, &chg) != 1) {
            /* transient misses while the hub churns the reset are normal;
             * only a persistently deaf hub ends the attempt */
            if (++deaf >= 6) {
                klog("usb: hub %d port %d: hub deaf during reset",
                     h->slot, p);
                return 0;
            }
            continue;
        }
        deaf = 0;
        if ((chg & 0x10) && !(st & 0x10)) {            /* C_RESET, done */
            ctrl_xfer(h->slot, 0x23, 1, 20, (u16)p, 0, 0, 0);  /* clr */
            u32 s2 = 0, c2 = 0;
            hub_port_status(h->slot, p, &s2, &c2);
            /* USB2 hub port status bit 1 = ENABLE (bit 2 is SUSPEND - the
             * r28 field log proved the old `& 4` check rejected perfectly
             * enabled ports: status 0x103/0x503 = connected+ENABLED+powered
             * yet "reset done but not enabled") */
            if (!(s2 & 2)) {
                klog("usb: hub %d port %d reset done but not enabled (%04x)",
                     h->slot, p, s2);
                return 0;
            }
            return 1;
        }
        if (!(st & 1)) return 0;                       /* unplugged */
    }
    klog("usb: hub %d port %d reset timed out", h->slot, p);
    return 0;
}

/* reset + enumerate one connected downstream port of hub h */
static int hub_attach(struct xdev *h, int p);

/* Linux xhci_update_hub_device, in spirit: once the hub descriptor is
 * known, a Configure Endpoint command re-adds the slot context with the
 * Hub bit set - plus the fields Intel validates for hubs that cannot
 * exist at Address-Device time:
 *   dw1[31:24] Number of Ports  (xHCI 1.2 table 6-8, hubs only)
 *   dw2[17:16] TT Think Time    (8/16/24/32-FS-bit-times encoding, from
 *                                wHubCharacteristics bits 6:5)
 *   dw0[25]    MTT              (hub has multiple TTs)
 * The slot context starts from the xHC's own OUT copy (like Linux's
 * xhci_slot_copy from out_ctx), dev_state is zeroed, and only the slot
 * flag is ADDed - the EP contexts stay zero and are ignored. */
static int hub_enable(struct xdev *d, int nports, int ttt, int multi)
{
    u8 *ic = inctx[d->slot];
    u32 *out_slot = (u32 *)devctx[d->slot];   /* xHC-maintained slot ctx */
    memset(ic, 0, csz * 34);
    ((u32 *)ic)[0] = 0;                  /* Drop Context Flags (dw0) */
    ((u32 *)ic)[1] = 0x01;               /* ADD: slot ctx only (dw1) */
    u32 *slw = (u32 *)(ic + csz);
    slw[0] = out_slot[0] | (1u << 26) | ((u32)(multi & 1) << 25);
    slw[1] = (out_slot[1] & 0x00FFFFFFu) | ((u32)(nports & 0xFF) << 24);
    slw[2] = (out_slot[2] & ~(3u << 16)) | ((u32)(ttt & 3) << 16);
    slw[3] = 0;
    int rc = run_cmd(PA(ic), 0, 0,
                     (u32)(TRB_CFGEP << 10) | ((u32)d->slot << 24),
                     800, d->slot);
    klog("usb: hub %d slot-ctx update Hub=1 ports=%d ttt=%d mtt=%d rc %d",
         d->slot, nports, ttt, multi, rc);
    return rc == 1;
}

static int hub_recoveries;      /* capped: never loop forever */
static int hub_walk_recovered;  /* hub_walk delegated to hub_recover and
                                 * its own slot was freed underneath it */

/* Hard-revive a silent hub: free its slot (and any children), reset the
 * root port so the hub comes back through a clean power-on reset, and
 * re-enumerate the whole branch.  A hub that browned out or wedged its
 * firmware during reconfiguration answers again afterwards - this is
 * what Linux's khubd does via usb_reset_device + re-enumeration.
 * At most 2 recoveries per boot. */
static int hub_recover(int root_port, int dead_slot)
{
    if (hub_recoveries >= 2) {
        klog("usb: hub recovery limit reached - port %d left alone",
             root_port);
        return -1;              /* -1 = recovery did NOT run */
    }
    hub_recoveries++;
    int rec_n = hub_recoveries;     /* nested recoveries share the counter;
                                     * log OUR number or the "result" lines
                                     * duplicate with the innermost one */
    klog("usb: hub recovery %d: freeing slot %d, resetting root port %d",
         rec_n, dead_slot, root_port);
    if (dead_slot >= 1 && dead_slot <= MAX_SLOTS && devs[dead_slot].used) {
        for (int i = 0; i < 8; i++) {
            u8 cs = devs[dead_slot].child_slot[i];
            if (cs && cs <= MAX_SLOTS && devs[cs].used) disable_slot(cs);
        }
        disable_slot(dead_slot);
    }
    if (hub_count > 0) hub_count--;      /* re-walk counts it again */
    port_slot[root_port] = 0;
    drain_ms(80);
    int kind = enumerate_port(root_port);
    klog("usb: hub recovery %d result: kind %d", rec_n, kind);
    return kind;
}

/* full hub bring-up: the slot was addressed WITHOUT the Hub bit (Intel
 * rejects Hub=1 at Address-Device time with Parameter Error); the flag
 * goes in via hub_enable once the hub descriptor has been read, exactly
 * like Linux.  Then config, port power and the walk.
 * Returns 3 (the "hub" kind). */
static int hub_walk(struct xdev *d, u8 hproto)
{
    if (d->tier >= 3) {   /* 20-bit route string = max 5 tiers anyway */
        klog("usb: hub slot %d too deep (tier %d) - not walking",
             d->slot, d->tier);
        fail_flag = 1;
        return 3;
    }
    int ns = d->slot;
    d->hub_mtt = (hproto == 2) ? 1 : 0;
    klog("usb: hub slot %d bring-up (multi-TT %d)", ns, d->hub_mtt);
    int drc = ctrl_xfer(ns, 0x80, 6, 0x0200, 0, desc_buf, 9, 1);
    if (drc != 1 && drc != 12 && drc != 13) {
        klog("usb: hub %d config fetch failed (code %d)", ns, drc);
        fail_flag = 1;
        return 3;
    }
    u8 cfg_val = desc_buf[5];
    drc = ctrl_xfer(ns, 0x00, 9, cfg_val, 0, 0, 0, 0);
    klog("usb: hub %d SET_CONFIGURATION(%d) rc %d", ns, cfg_val, drc);
    /* Hub-spec: (re)configuring drops every downstream port to the
     * Powered-off state - THIS is the moment the mouse LEDs die.  Give
     * the firmware a beat to restart its port state machines before we
     * talk to it again (Linux has natural probe delays right here). */
    drain_ms(100);
    /* class descriptor: GET_DESCRIPTOR(Hub) = type 0x29 */
    drc = hub_xfer(ns, 0xA0, 6, 0x2900, 0, desc_buf, 9, 1, 3);
    if (drc != 1) {
        klog("usb: hub %d descriptor failed (code %d)", ns, drc);
        fail_flag = 1;
        return 3;
    }
    hexdump("hubdesc", ns, desc_buf, 9);
    int nports = desc_buf[2];
    int p2g = desc_buf[5];
    if (nports < 1 || nports > 8) {
        klog("usb: hub %d: %d ports - unsupported, skipping", ns, nports);
        fail_flag = 1;
        return 3;
    }
    d->hub_ports = (u8)nports;
    d->hub_p2g = (u8)p2g;
    int chars = (int)(desc_buf[3] | (desc_buf[4] << 8));
    klog("usb: hub %d: %d ports, chars %04x, pwr2good %d ms", ns, nports,
         (u32)chars, p2g * 2);
    /* announce the hub to the xHC BEFORE any child is addressed: the
     * children's TT fields point at this slot, and the xHC must already
     * know it is a hub (Number of Ports included - Intel validates it) */
    if (!hub_enable(d, nports, (chars >> 5) & 3, d->hub_mtt)) {
        klog("usb: hub %d: xHC rejected the hub slot context -"
             " children will not enumerate", ns);
        fail_flag = 1;
        return 3;
    }
    drain_ms(30);
    /* Power the ports ONE AT A TIME.  Simultaneous cold-start of a
     * keyboard + mouse + boot stick behind a bus-powered hub inrushes
     * enough current to brown out the hub's own upstream VBUS - the
     * r25.2 real-hardware failure: hub goes deaf on EP0 the instant
     * all four ports were gang-powered (fast code-4 style failures on
     * every GET_PORT_STATUS right after). */
    for (int p = 1; p <= nports; p++) {
        int prc = hub_xfer(ns, 0x23, 3, 8, (u16)p, 0, 0, 0, 3); /* PORT_POWER */
        if (prc == -1) {
            /* hub stopped completing transfers: powering the remaining
             * ports would burn 3 retries x 800 ms each for nothing */
            klog("usb: hub %d deaf at port %d power-on - skipping rest",
                 ns, p);
            break;
        }
        drain_ms(30);
    }
    drain_ms((u32)(p2g * 2 + 150));
    int alive = 0;
    for (int p = 1; p <= nports; p++) {
        u32 st = 0, chg = 0;
        int prc = hub_port_status_r(ns, p, &st, &chg);
        if (prc != 1) {             /* hub_xfer already logged rc + port */
            if (prc == -1) {
                klog("usb: hub %d deaf at port %d status - skipping rest",
                     ns, p);
                break;
            }
            continue;
        }
        alive++;
        klog("usb: hub %d port %d: status %04x change %04x%s", ns, p,
             st, chg, (st & 1) ? " CONNECTED" : "");
        if (chg & 1) ctrl_xfer(ns, 0x23, 1, 16, (u16)p, 0, 0, 0); /* C_CONN */
        if (!(st & 1)) continue;
        hub_attach(d, p);
    }
    if (!alive) {
        /* The hub went silent right after reconfiguration - firmware
         * wedge or VBUS brownout.  Only cure: free everything, reset
         * the root port (clean power-on for the hub) and re-walk the
         * whole branch, exactly like Linux's usb_reset_device path. */
        int rp = d->root_port;
        klog("usb: hub %d DEAD on all %d ports (root %d portsc %08x)",
             ns, nports, rp, portsc(rp));
        fail_flag = 1;
        /* Set the flag only AFTER the recovery ran (and only if it ran):
         * the inner enumerate_device frames clear it on their way out,
         * so setting it beforehand would be stomped and this frame
         * would "restore" last_enum_slot to an already-freed slot. */
        if (hub_recover(rp, ns) >= 0)
            hub_walk_recovered = 1;
    }
    return 3;
}

/* debounce, reset and enumerate whatever sits on hub port p */
static int hub_attach(struct xdev *h, int p)
{
    u32 st = 0, chg = 0;
    if (hub_port_status_r(h->slot, p, &st, &chg) != 1) return 0;
    if (!(st & 1)) return 0;
    drain_ms(20);                              /* USB debounce */
    u32 st2 = 0, chg2 = 0;
    if (hub_port_status_r(h->slot, p, &st2, &chg2) != 1) return 0;
    if (!(st2 & 1)) {
        klog("usb: hub %d port %d connect bounced away", h->slot, p);
        return 0;
    }
    klog("usb: hub %d port %d attaching (status %04x)", h->slot, p, st2);
    if (!hub_port_reset(h, p)) {
        /* Linux retries a flaky port reset once; cheap FS/LS devices
         * occasionally miss the first attempt */
        drain_ms(100);
        klog("usb: hub %d port %d reset retry", h->slot, p);
        if (!hub_port_reset(h, p)) {
            klog("usb: hub %d port %d reset failed", h->slot, p);
            fail_flag = 1;
            return 0;
        }
    }
    u32 s3 = 0, c3 = 0;
    hub_port_status_r(h->slot, p, &s3, &c3);
    /* USB2 hub port status: bit 9 = low-speed, bit 10 = high-speed */
    int raw = (int)((s3 >> 9) & 3);
    int cspeed = (raw == 0) ? 1 : (raw == 1) ? 2 : (raw == 2) ? 3 : 0;
    if (!cspeed) {
        klog("usb: hub %d port %d bad speed bits (%04x)", h->slot, p, s3);
        return 0;
    }
    u32 croute = h->route | ((u32)p << (4 * h->tier));
    int cmtt = (h->speed == 3 && cspeed < 3 && h->hub_mtt) ? 1 : 0;
    klog("usb: hub %d port %d: child speed %d route %x mtt %d",
         h->slot, p, cspeed, croute, cmtt);
    int kind = enumerate_device(h->root_port, croute, cspeed, cmtt,
                                h->tier + 1, h, p);
    if (last_enum_slot > 0 && last_enum_slot <= MAX_SLOTS && p <= 8)
        h->child_slot[p - 1] = (u8)last_enum_slot;
    return kind;
}

static int enumerate_port(int port)
{
    if (!(portsc(port) & 1)) {            /* CCS gone: device unplugged */
        u8 old = port_slot[port];
        if (old && old <= MAX_SLOTS && devs[old].used) {
            klog("usb: port %d: slot %d disconnected", port, old);
            disable_slot(old);
        }
        port_slot[port] = 0;
        return 0;
    }
    if (port_slot[port] && port_slot[port] <= MAX_SLOTS &&
        devs[port_slot[port]].used)
        return 0;    /* already enumerated - spurious change event */
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
            drain_ms(30);
            ps[0] = 1u << 9;                 /* PP on, alone */
            drain_ms(120);                   /* debounce */
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
    klog("usb: port %d connected, speed %d, portsc %08x",
         port, speed, portsc(port));
    if (speed < 1 || speed > 4) return 0;
    int kind = enumerate_device(port, 0, speed, 0, 0, 0, 0);
    if (last_enum_slot > 0 && last_enum_slot <= MAX_SLOTS)
        port_slot[port] = (u8)last_enum_slot;
    return kind;
}

/* a boot-protocol HID interface candidate found in a config descriptor */
struct hid_cand { int iface, proto, ep_addr, ep_mps; };

/* Parse a full configuration descriptor for boot-protocol HID interfaces
 * (class 3, subclass 1, protocol 1=keyboard / 2=mouse), each paired with
 * its first IN endpoint.  PURE LOGIC except the optional iface log lines -
 * unit-tested against real field-captured descriptors in tests/usb_sim.c.
 *
 * r30 root fix: composite devices are the norm.  The user's Holtek
 * keyboard presents boot-mouse iface 0 + boot-keyboard iface 1 + a vendor
 * iface 2 (3/0/0); the Razer presents boot-keyboard iface 0 + two
 * consumer-control ifaces (3/0/1, 3/0/2).  The old single-slot parser
 * CLEARED the found boot interface when any later interface was not
 * bootable ("else iface = -1"), so both devices were rejected with
 * "not a boot HID device" despite their endpoints being found and logged.
 * A non-boot interface must only end ITS OWN candidacy - never erase a
 * completed candidate. */
int usb_hid_parse(const u8 *desc, int tot, struct hid_cand *c, int maxc,
                  int logif)
{
    int n = 0;
    int open = -1;          /* candidate still seeking its IN endpoint */
    int off = 9;            /* skip the config descriptor header */
    while (off + 2 <= tot) {
        int dl = desc[off];
        if (dl < 2) break;              /* 0 or 1 would stall the walk */
        u8 dt = desc[off + 1];
        if (dt == 4 && dl >= 9) {                       /* interface */
            open = -1;
            int ifn = desc[off + 2], cls = desc[off + 5];
            int sub = desc[off + 6], proto = desc[off + 7];
            if (logif)
                klog("usb:   iface %d alt %d: class %d/%d/%d eps %d%s",
                     ifn, desc[off + 3], cls, sub, proto, desc[off + 4],
                     (cls == 3 && sub == 1 && (proto == 1 || proto == 2))
                         ? " boot-HID" : "");
            if (cls == 3 && sub == 1 && (proto == 1 || proto == 2)) {
                int dup = 0;            /* extra alt setting: keep first */
                for (int j = 0; j < n; j++)
                    if (c[j].iface == ifn) { dup = 1; break; }
                if (!dup && n < maxc) {
                    c[n].iface = ifn;
                    c[n].proto = proto;
                    c[n].ep_addr = 0;
                    c[n].ep_mps = 8;
                    open = n;
                    n++;
                }
            }
        } else if (dt == 5 && dl >= 7 && open >= 0 && !c[open].ep_addr) {
            u8 ea = desc[off + 2];                       /* endpoint */
            if (ea & 0x80) {
                c[open].ep_addr = ea & 0xF;
                c[open].ep_mps = desc[off + 4] | (desc[off + 5] << 8);
            }
        }
        off += dl;
    }
    int w = 0;              /* drop boot ifaces that never showed an IN ep */
    for (int j = 0; j < n; j++)
        if (c[j].ep_addr) c[w++] = c[j];
    return w;
}

/* Pre-queue a boot-HID ring's IN TRBs, terminate with the link and ring
 * the doorbell.  Shared by enumeration AND the native simulator harness
 * (tests/usb_sim.c) so the tested code path is the real one. */
static void hid_arm_ring(struct xdev *d, int j)
{
    struct hid_ep *h = &d->hid[j];
    for (int b = 0; b < IN_TRBS - 1; b++) {
        volatile u32 *tr = h->inr + (u32)b * 4;
        tr[0] = PA(h->in_buf[b]);
        tr[1] = 0;
        tr[2] = 16;
        /* IOC: a completion event per report; ISP: complete on the
         * always-short HID report - without ISP no event ever fires and
         * input is silently dead */
        tr[3] = (TRB_NORMAL << 10) | (1u << 5) | (1u << 2) | h->inr_cycle;
    }
    ring_link(h->inr, IN_TRBS - 1, h->inr_cycle);
    h->inr_cycle ^= 1;
    h->arm_tick = tick_count;
    h->kick1 = 0;
    ring_db((u32)d->slot, h->dci);
    klog("usb: slot %d %s iface %d ep %02x ring armed (doorbell %u)",
         d->slot, h->kind == 2 ? "mouse" : "keyboard", h->iface,
         (u32)(0x80 | h->ep_addr), h->dci);
}

static int enumerate_device(int root_port, u32 route, int speed, int mtt,
                            int tier, struct xdev *phub, int hport)
{
    last_enum_slot = 0;
    int rc = run_cmd(0, 0, 0, (u32)(TRB_ENABSLOT << 10), 500, 0);
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
    klog("usb: root %d route %x tier %d -> slot %d", root_port, route,
         tier, slot);

    struct xdev *d = slot_alloc(slot);
    if (!d) { disable_slot(slot); return 0; }
    d->route = route;
    d->speed = (u8)speed;
    d->mtt = (u8)mtt;
    d->root_port = (u8)root_port;
    d->tier = (u8)tier;
    /* TT info (Linux xhci_setup_addressable_virt_dev): FS/LS devices
     * under an external HS hub name the hub's slot and their port on
     * it, so the xHC can build split-transaction tokens.  HS children
     * and root-attached devices keep both fields 0. */
    if (phub && (speed == 1 || speed == 2) && phub->speed == 3) {
        d->tt_slot = (u8)phub->slot;
        d->tt_port = (u8)hport;
        klog("usb: slot %d TT: hub slot %d port %d (FS/LS behind HS hub)",
             slot, d->tt_slot, d->tt_port);
    }
    last_enum_slot = slot;

    /* USB spec: LS control EP is ALWAYS 8 bytes; HS always 64; SS 512
     * (the xHCI EP-context MPS field is in BYTES for every speed - Linux
     * feeds it usb_endpoint_maxp, which is 1<<bMaxPacketSize0 for SS).
     * FS may be 8/16/32/64 - the bootstrap below learns the real value
     * before the device is ever addressed. */
    u32 mps = (speed == 4) ? 512 : (speed == 2) ? 8 : 64;
    /* BSR bootstrap (the Linux method): Address Device with Block Set
     * Request builds the slot WITHOUT sending SET_ADDRESS on the wire.
     * The device still answers at the default address, so we read the
     * first 8 descriptor bytes to learn the real EP0 max packet size -
     * then issue the one and only real Address Device.
     * r25.2, from the real Linux source: completion code 17 is
     * PARAMETER ERROR, not "event ring full" (that is 21).  The r25.1
     * hub failure was our Hub bit in the Address-Device slot context:
     * Intel wants the Number-of-Ports field with Hub=1, and that only
     * exists after the hub descriptor is read.  Linux NEVER sets Hub at
     * address time - it addresses hubs like any device and flips the
     * Hub bit afterwards with a Configure Endpoint (hub_enable). */
    rc = address_slot(d, mps, 1);
    if (rc != 1) {
        klog("usb: bootstrap address failed slot %d (code %d)", slot, rc);
        disable_slot(slot);
        return 0;
    }
    int drc = ctrl_xfer(slot, 0x80, 6, 0x0100, 0, desc_buf, 8, 1);
    u32 mps0 = (u32)desc_buf[7];
    u8 devclass = 0;
    int have8 = (drc == 1 || drc == 12 || drc == 13) &&
                desc_buf[0] == 0x12 && desc_buf[1] == 0x01 &&
                ((mps0 == 8 || mps0 == 16 || mps0 == 32 || mps0 == 64) ||
                 (speed == 4 && mps0 == 9));
    if (have8) {
        devclass = desc_buf[4];
        /* only 8 bytes were fetched - vid:pid live in bytes 8..11 and
         * are reported from the real 18-byte fetch below */
        klog("usb: slot %d boot8: class %d mps0 %u", slot, devclass, mps0);
    } else {
        klog("usb: slot %d boot8 unusable (code %d b0 %02x b1 %02x mps %u)"
             " - assuming default MPS", slot, drc,
             desc_buf[0], desc_buf[1], mps0);
        mps0 = mps;
    }
    if (speed == 4) mps0 = 512;          /* SS: EP0 is always 2^9 */
    rc = address_slot(d, mps0, 0);       /* Hub bit NEVER goes in here */
    if (rc != 1) {
        klog("usb: address device failed slot %d (code %d)", slot, rc);
        disable_slot(slot);
        return 0;
    }
    d->is_hub = (u8)(devclass == 9);
    u8 *ic = inctx[slot];
    u32 *slw, *epw;

    /* full 18-byte device descriptor - the real MPS is on the wire now */
    drc = ctrl_xfer(slot, 0x80, 6, 0x0100, 0, desc_buf, 18, 1);
    if ((drc != 1 && drc != 12 && drc != 13)) {
        klog("usb: descriptor 18B fetch failed slot %d (code %d)",
             slot, drc);
        disable_slot(slot);
        return 0;
    }
    hexdump("dev", slot, desc_buf, 18);
    klog("usb: slot %d dev: %02x:%02x vid:pid %04x:%04x class %d mps0 %u",
         slot, desc_buf[0], desc_buf[1],
         (u32)(desc_buf[8] | (desc_buf[9] << 8)),
         (u32)(desc_buf[10] | (desc_buf[11] << 8)),
         desc_buf[4], desc_buf[7]);
    if (desc_buf[1] != 0x01)   /* bDescriptorType must be DEVICE (1) */
        klog("usb: slot %d WARNING: desc type %u != 1", slot, desc_buf[1]);
    if (desc_buf[4] == 9) {   /* bDeviceClass 9 = hub: walk it */
        hub_count++;
        int k = hub_walk(d, desc_buf[6]);
        /* nested child enumeration (or a full hub recovery) overwrote
         * last_enum_slot; callers book THIS device against it.  Skip
         * the restore when the walk delegated to hub_recover and this
         * slot was freed underneath us. */
        if (!hub_walk_recovered) last_enum_slot = slot;
        hub_walk_recovered = 0;
        return k;
    }
    drc = ctrl_xfer(slot, 0x80, 6, 0x0200, 0, desc_buf, 9, 1);
    if ((drc != 1 && drc != 12 && drc != 13)) {
        klog("usb: config descriptor failed slot %d (code %d)", slot, drc);
        disable_slot(slot); return 0;
    }
    u16 tot = (u16)(desc_buf[2] | (desc_buf[3] << 8));
    klog("usb: slot %d config: wTotalLength %u, %d interface(s)",
         slot, (u32)tot, desc_buf[4]);
    if (tot > sizeof(desc_buf)) tot = sizeof(desc_buf);
    drc = ctrl_xfer(slot, 0x80, 6, 0x0200, 0, desc_buf, tot, 1);
    if ((drc != 1 && drc != 12 && drc != 13)) {
        klog("usb: full config descriptor failed slot %d (code %d)", slot, drc);
        disable_slot(slot); return 0;
    }
    hexdump("cfg", slot, desc_buf, (int)(tot > 64 ? 64 : tot));
    u8 cfg_val = desc_buf[5];
    struct hid_cand cand[MAX_HID_EPS];
    int ncand = usb_hid_parse(desc_buf, (int)tot, cand, MAX_HID_EPS, 1);
    for (int j = 0; j < ncand; j++)
        klog("usb:   boot-HID iface %d proto %d ep %02x IN mps %d",
             cand[j].iface, cand[j].proto, (u32)(0x80 | cand[j].ep_addr),
             cand[j].ep_mps);
    if (!ncand) {
        klog("usb: slot %d not a boot HID device", slot);
        disable_slot(slot);
        return 0;
    }

    int srcc;
    srcc = ctrl_xfer(slot, 0x00, 9, cfg_val, 0, 0, 0, 0);  /* set config */
    klog("usb: slot %d SET_CONFIGURATION(%d) rc %d", slot, cfg_val, srcc);
    for (int j = 0; j < ncand; j++) {
        /* per-interface boot handoff: boot protocol, no idle rate, and
         * keyboards get their BIOS NumLock LED explicitly extinguished */
        srcc = ctrl_xfer(slot, 0x21, 0x0B, 0, (u16)cand[j].iface, 0, 0, 0);
        klog("usb: slot %d iface %d SET_PROTOCOL(boot) rc %d",
             slot, cand[j].iface, srcc);
        ctrl_xfer(slot, 0x21, 0x0A, 0, (u16)cand[j].iface, 0, 0, 0);
        if (cand[j].proto == 1) {
            u8 leds = 0;
            ctrl_xfer(slot, 0x21, 0x09, 0x0200, (u16)cand[j].iface,
                      &leds, 1, 0);
        }
    }

    /* allocate one interrupt ring per boot interface */
    d->nhid = ncand;
    for (int j = 0; j < ncand; j++) {
        struct hid_ep *h = &d->hid[j];
        h->iface = cand[j].iface;
        h->kind = (cand[j].proto == 2) ? 2 : 1;
        h->ep_addr = (u8)cand[j].ep_addr;
        h->ep_mps = (u8)(cand[j].ep_mps & 0xFFFF);
        h->dci = (u32)(h->ep_addr * 2 + 1);   /* EP 0x81 -> DCI 3 */
        h->inr = (volatile u32 *)palloc(4096);
        if (!h->inr) { h->iface = -1; continue; }
        memset((void *)h->inr, 0, 4096);
        for (int b = 0; b < IN_TRBS; b++) h->in_buf[b] = palloc(16);
        if (!h->in_buf[0]) { h->iface = -1; continue; }
        h->inr_idx = 0;
        h->inr_cycle = 1;
        h->link_pend = 0;
        h->link_pend_cycle = 1;
        h->active = 1;
        d->kind |= h->kind;
    }
    if (!d->kind) {
        klog("usb: slot %d ring allocation failed", slot);
        disable_slot(slot);
        return 0;
    }

    /* ONE Configure Endpoint ADDs every boot EP context - the Linux way
     * to configure interfaces - keeping the FULL topology: dropping
     * route/MTT/TT here would cut a hub-attached device off the moment
     * its interrupt EPs are added */
    int maxdci = 0;
    u32 addf = 0x01u;                    /* slot context */
    for (int j = 0; j < ncand; j++) {
        struct hid_ep *h = &d->hid[j];
        if (!h->active) continue;
        addf |= 1u << h->dci;
        if ((int)h->dci > maxdci) maxdci = (int)h->dci;
    }
    memset(ic, 0, csz * 34);
    ((u32 *)ic)[0] = 0;                  /* Drop Context Flags (dw0) */
    ((u32 *)ic)[1] = addf;               /* ADD: slot ctx + every boot EP */
    slw = (u32 *)(ic + csz);
    slw[0] = ((speed == 4) ? (route & 0xFFFFFu) : 0u) |
             ((u32)(speed & 0xF) << 20) |
             ((u32)(mtt & 1) << 25) | ((u32)maxdci << 27);
    slw[1] = ((u32)root_port & 0xFF) << 16;
    slw[2] = (u32)d->tt_slot | ((u32)d->tt_port << 8);
    for (int j = 0; j < ncand; j++) {
        struct hid_ep *h = &d->hid[j];
        if (!h->active) continue;
        epw = (u32 *)(ic + csz * (h->dci + 1));
        /* dw0: Interval [23:16] = 3 (4 ms FS / 500 us HS polling);
         * dw1: EP Type [5:3] = 6 (Interrupt IN), CErr [2:1] = 3, MPS;
         * dw4: Max Burst 0 - SuperSpeed-only field, Linux writes 0 */
        epw[0] = (3u << 16);
        epw[1] = (6u << 3) | (3u << 1) | ((u32)(h->ep_mps & 0xFFFF) << 16);
        epw[2] = PA(h->inr) | 1;
        epw[3] = 0;
        epw[4] = 0;
    }
    rc = run_cmd(PA(ic), 0, 0,
                 (u32)(TRB_CFGEP << 10) | ((u32)slot << 24), 800, slot);
    if (rc != 1) {
        klog("usb: configure endpoint failed slot %d (code %d)", slot, rc);
        fail_flag = 1;
        disable_slot(slot);
        return 0;
    }

    for (int j = 0; j < ncand; j++)
        if (d->hid[j].active)
            hid_arm_ring(d, j);
    n_devs++;
    klog("usb: slot %d = HID %s (%d boot iface(s))", slot,
         d->kind == 3 ? "keyboard+mouse" : d->kind == 2 ? "mouse"
                                                        : "keyboard",
         ncand);
    return d->kind;
}

/* -------------------------------------------------------------- public ---- */
void usb_kbd_leds_off(void)
{
    if (!have_xhci) return;
    for (int sl = 1; sl <= MAX_SLOTS; sl++) {
        struct xdev *d = &devs[sl];
        if (!d->used) continue;
        for (int j = 0; j < d->nhid; j++) {
            struct hid_ep *h = &d->hid[j];
            if (!h->active || h->kind != 1) continue;
            u8 leds = 0;
            /* HID SET_REPORT(Output) so no LED stays lit on USB standby
             * power after shutdown */
            ctrl_xfer(sl, 0x21, 0x09, 0x0200, (u16)h->iface, &leds, 1, 0);
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
            if (m & (1u << p)) {
                enumerate_port(p);
                /* our own resets re-armed this bit during enumerate -
                 * dropping it here stops a 1 Hz re-enumeration loop */
                pending_portc &= ~(1u << p);
            }
    }
    for (int sl = 1; sl <= MAX_SLOTS; sl++) {   /* hub-port hotplug, 1 Hz */
        struct xdev *h = &devs[sl];
        if (!h->used || !h->is_hub) continue;
        if (h->dead_polls < 0) continue;   /* gave up: NO more probes at all -
                                            * each one blocks the WM/diag loop
                                            * for its full timeout */
        int alive = 0;
        for (int p = 1; p <= h->hub_ports && p <= 8; p++) {
            u32 st = 0, chg = 0;
            int prc = hub_port_status(sl, p, &st, &chg);
            if (prc != 1) {
                /* port 1 timing out means the HUB is silent - probing the
                 * other ports just multiplies the stall */
                if (prc == -1 && p == 1) break;
                continue;
            }
            alive++;
            if (!(chg & 0x13)) continue;        /* conn/enable/reset changes */
            klog("usb: hub %d port %d change %04x status %04x", sl, p,
                 chg, st);
            if (chg & 2) ctrl_xfer(sl, 0x23, 1, 17, (u16)p, 0, 0, 0);
            if (chg & 0x10) ctrl_xfer(sl, 0x23, 1, 20, (u16)p, 0, 0, 0);
            if (chg & 1) {
                ctrl_xfer(sl, 0x23, 1, 16, (u16)p, 0, 0, 0);
                u8 old = h->child_slot[p - 1];
                if (old && old <= MAX_SLOTS && devs[old].used) {
                    klog("usb: hub %d port %d: slot %d went away", sl, p,
                         old);
                    disable_slot(old);
                }
                h->child_slot[p - 1] = 0;
                if (st & 1) hub_attach(h, p);
            }
        }
        if (alive) { h->dead_polls = 0; continue; }
        if (++h->dead_polls < 4) continue;
        /* 4 s with no answer on ANY port: the hub died after boot
         * (brownout/wedge).  Revive it from the root port - the
         * diagnostics screen redraws live, so the recovery is
         * visible on the photo without reflashing. */
        if (hub_recoveries >= 2) {
            klog("usb: hub %d silent, recovery limit reached - giving up",
                 sl);
            h->dead_polls = -1;                   /* never re-trigger:
                                                   * keeps the log ring
                                                   * readable for photos */
            continue;
        }
        {
            int rp = h->root_port;
            klog("usb: hub %d silent 4 s - reviving via root port %d",
                 sl, rp);
            h->dead_polls = 0;
            hub_recover(rp, sl);
        }
        break;                /* slot table churned; next tick rescans */
    }
    for (int sl = 1; sl <= MAX_SLOTS; sl++) {
        struct xdev *d = &devs[sl];
        if (!d->used || !d->nhid) continue;
        for (int j = 0; j < d->nhid; j++) {
            struct hid_ep *h = &d->hid[j];
            if (!h->active || !h->inr) continue;
            if (!h->reported) {
                /* a healthy idle keyboard/mouse sends nothing until
                 * touched, but an endpoint that idled on a not-ready TRB
                 * needs a doorbell to restart - ring it once, 3 s after
                 * arming (a no-op hint on a running EP) */
                if (!h->kick1 && tick_count - h->arm_tick > 300) {
                    h->kick1 = 1;
                    ring_db((u32)sl, h->dci);
                    klog("usb: slot %d %s armed 3 s, no first report yet - "
                         "doorbell nudge sent", sl,
                         h->kind == 2 ? "mouse" : "keyboard");
                }
                continue;
            }
            if (!h->silent_logged && tick_count - h->last_rep_tick > 500) {
                h->silent_logged = 1;
                ring_db((u32)sl, h->dci);
                klog("usb: slot %d %s silent - no reports for 5 s "
                     "(pipe stalled?) - doorbell nudge sent", sl,
                     h->kind == 2 ? "mouse" : "keyboard");
            }
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

    evt_ring = (volatile u32 *)palloc(EVT_TRBS * 16);
    memset((void *)evt_ring, 0, EVT_TRBS * 16);
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
    drain_ms(250);   /* settle AND drain the 15-port link-training storm */
    {
        int pw = 0, cc = 0;
        for (int p = 1; p <= max_ports; p++) {
            u32 v = portsc(p);
            if (v & (1u << 9)) pw++;
            if (v & 1) cc++;
        }
        klog("usb: ports powered %d/%d, connected %d", pw, max_ports, cc);
    }

    for (int p = 1; p <= max_ports; p++) {
        u32 psc = portsc(p);
        if (!(psc & 1)) continue;
        klog("usb: enumerate port %d (portsc %08x speed %d)",
             p, psc, (int)((psc >> 10) & 0xF));
        port_slot[p] = 0;
        enumerate_port(p);
    }
    /* count from the device table, not from return values: hub children
     * never surface through enumerate_port's kind (and hub recoveries
     * re-walk whole branches deep inside a single enumerate_port call) */
    int mk = 0, mm = 0;
    for (int sl = 1; sl <= MAX_SLOTS; sl++) {
        if (!devs[sl].used) continue;
        if (devs[sl].kind & 1) mk++;     /* composite devices count once
                                          * for each interface kind */
        if (devs[sl].kind & 2) mm++;
    }
    pending_portc = 0;   /* our own boot-time resets queued stale changes */
    char nl[128], tmp[8];
    strcpy(nl, "usb: xHCI live - ");
    fmt_u32(tmp, (u32)mk); strcat(nl, tmp); strcat(nl, " keyboard, ");
    fmt_u32(tmp, (u32)mm); strcat(nl, tmp); strcat(nl, " mouse (HID boot)");
    if (hub_count) {
        strcat(nl, ", "); fmt_u32(tmp, (u32)hub_count); strcat(nl, tmp);
        strcat(nl, " hub(s)");
    }
    if (fail_flag) strcat(nl, " - see diag");
    strcat(nl, " | ev "); fmt_u32(tmp, evt_seen); strcat(nl, tmp);
    strcat(nl, " rf "); fmt_u32(tmp, ring_full_hits); strcat(nl, tmp);
    strcat(nl, " hc "); fmt_u32(tmp, evt_hcevent); strcat(nl, tmp);
    strcat(nl, " rs "); fmt_u32(tmp, restart_count); strcat(nl, tmp);
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

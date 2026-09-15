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

#define MAX_SLOTS 4
#define CMD_TRBS 64
#define EVT_TRBS 64
#define EP0_TRBS 16
#define IN_TRBS 8

#define TRB_NORMAL      1
#define TRB_SETUP       2
#define TRB_DATA        3
#define TRB_STATUS      4
#define TRB_LINK        6
#define TRB_ENABSLOT    9
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
static volatile u32 cc_code = 0xFF;
static volatile u32 cc_slot;
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
        if (type == TRB_LINK) {          /* our own link: wrap the consumer */
            evt_idx = 0;
            evt_cycle ^= 1;
            volatile u32 *erdp = (volatile u32 *)(rt + 0x20 + 0x18);
            wr64(erdp, (u64)PA(evt_ring));
            continue;
        }
        if (type == EV_CMDCOMP) {
            cc_code = (t[2] >> 24) & 0xFF;
            cc_slot = (t[3] >> 24) & 0xFF;
        } else if (type == EV_TRANSFER) {
            u32 slot = (t[3] >> 24) & 0xFF;
            u32 ep = (t[3] >> 16) & 0x1F;
            u32 code = (t[2] >> 24) & 0xFF;
            u32 len = t[2] & 0xFFFFFF;
            u32 ptr = t[0];
            cc_code = code;
            cc_slot = slot | (ep << 8) | 0x10000u;
            if (slot <= MAX_SLOTS && devs[slot].used &&
                ep == (u32)(devs[slot].ep_addr * 2 + 1) &&
                (code == 1 || code == 13)) {
                struct xdev *d = &devs[slot];
                int i = -1;
                for (int b = 0; b < IN_TRBS; b++)
                    if (PA(d->in_buf[b]) == ptr) i = b;
                if (i >= 0) {
                    u8 *r = d->in_buf[i];
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
                tr[3] = (TRB_NORMAL << 10) | d->inr_cycle;
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
                ps[0] = v | (1u << 21) | (1u << 22) | (1u << 23);
                if (v & 1) enumerate_port((int)port);   /* hot plug */
            }
        }
        evt_idx++;
        if (evt_idx == EVT_TRBS - 1) {   /* link slot: wrap */
            evt_idx = 0;
            evt_cycle ^= 1;
        }
        volatile u32 *erdp = (volatile u32 *)(rt + 0x20 + 0x18);
        wr64(erdp, (u64)PA(evt_ring + (u32)evt_idx * 4));
    }
    return work;
}

static int wait_event(u64 timeout_ms)
{
    u64 t0 = now_ms();
    cc_code = 0xFF;
    cc_slot = 0;
    while (now_ms() - t0 < timeout_ms) {
        proc_events();
        if (cc_slot && !(cc_slot & 0x10000u)) return (int)cc_code;
        cpu_hlt();
    }
    return -1;
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
    return wait_event(timeout);
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
    cc_code = 0xFF; cc_slot = 0;
    ring_db((u32)slot, 0);
    u64 t0 = now_ms();
    while (now_ms() - t0 < 800) {
        proc_events();
        if ((cc_slot & 0x10000u) && (cc_slot & 0xFF) == (u32)slot)
            return (int)cc_code;
        cpu_hlt();
    }
    return -1;
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
    ps[0] = ps[0] | (1u << 9);            /* PP: port power */
    for (int i = 0; i < 4; i++) {         /* clear stale change bits */
        u32 v = ps[0];
        ps[0] = v | (1u << 21) | (1u << 22) | (1u << 23);
    }
    ps[0] = ps[0] | (1u << 4);            /* PR: reset */
    u64 t0 = now_ms();
    while (now_ms() - t0 < 400 && !(ps[0] & (1u << 21))) cpu_hlt();
    ps[0] = ps[0] | (1u << 21);           /* clear PRC */
}

static int port_speed(int port) { return (int)((portsc(port) >> 10) & 0xF); }

static int enumerate_port(int port)
{
    if (!(portsc(port) & 1)) return 0;    /* CCS */
    port_reset(port);
    int speed = port_speed(port);
    if (speed < 1 || speed > 4) return 0;

    if (run_cmd(0, 0, 0, (u32)(TRB_ENABSLOT << 10), 500) != 1) {
        klog("usb: enable slot failed");
        return 0;
    }
    int slot = (int)cc_slot;
    if (slot < 1 || slot > MAX_SLOTS || devs[slot].used) return 0;

    struct xdev *d = &devs[slot];
    d->used = 1;
    d->slot = slot;
    d->ep0 = (volatile u32 *)palloc(4096);
    devctx[slot] = palloc(csz * 4);
    inctx[slot] = palloc(csz * 8);
    if (!d->ep0 || !devctx[slot] || !inctx[slot]) { d->used = 0; return 0; }
    memset((void *)d->ep0, 0, 4096);
    memset(devctx[slot], 0, csz * 4);
    memset(inctx[slot], 0, csz * 8);
    d->ep0_idx = 0;
    d->ep0_cycle = 1;
    dcbaa[slot] = (u64)PA(devctx[slot]);

    u8 *ic = inctx[slot];
    ic[0] = 0x03;                                   /* add: slot + ep0 */
    u32 *slw = (u32 *)(ic + csz);
    slw[0] = (u32)(speed & 0xF) << 20;
    slw[1] = ((u32)port & 0xFF) << 16 | (1u << 27); /* port, entries = 1 */
    u32 mps = (speed == 4) ? 9 : 64;
    u32 *epw = (u32 *)(ic + csz * 2);               /* ep ctx index 2 = EP ID 0 */
    epw[0] = (3u << 16) | (3u << 1) | (4u << 3);    /* interval, CErr, control */
    epw[1] = mps;
    epw[2] = PA(d->ep0) | 1;
    epw[3] = 0;
    epw[4] = 8;

    if (run_cmd(PA(ic), 0, 0,
                (u32)(TRB_ADDRDEV << 10) | ((u32)slot << 24), 800) != 1) {
        klog("usb: address device failed (port %d)", port);
        d->used = 0;
        return 0;
    }

    if (ctrl_xfer(slot, 0x80, 6, 0x0100, 0, desc_buf, 18, 1) != 1) {
        klog("usb: device descriptor failed slot %d", slot);
        d->used = 0;
        return 0;
    }
    if (ctrl_xfer(slot, 0x80, 6, 0x0200, 0, desc_buf, 9, 1) != 1) {
        d->used = 0; return 0;
    }
    u16 tot = (u16)(desc_buf[2] | (desc_buf[3] << 8));
    if (tot > sizeof(desc_buf)) tot = sizeof(desc_buf);
    if (ctrl_xfer(slot, 0x80, 6, 0x0200, 0, desc_buf, tot, 1) != 1) {
        d->used = 0; return 0;
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
        d->used = 0;
        return 0;
    }
    ctrl_xfer(slot, 0x00, 9, cfg_val, 0, 0, 0, 0);         /* set configuration */
    ctrl_xfer(slot, 0x21, 0x0B, 0, (u16)iface, 0, 0, 0);   /* boot protocol */
    ctrl_xfer(slot, 0x21, 0x0A, 0, (u16)iface, 0, 0, 0);   /* set idle */

    d->iface = iface;
    d->ep_addr = ep_addr;
    d->kind = (proto == 2) ? 2 : 1;

    d->inr = (volatile u32 *)palloc(4096);
    if (!d->inr) { d->used = 0; return 0; }
    memset((void *)d->inr, 0, 4096);
    for (int b = 0; b < IN_TRBS; b++) d->in_buf[b] = palloc(16);
    d->inr_idx = 0;
    d->inr_cycle = 1;

    memset(ic, 0, csz * 8);
    ic[0] = 0x01 | (1u << 5);        /* add slot + ep ctx index 5 (EP ID 3) */
    slw = (u32 *)(ic + csz);
    slw[0] = (u32)(speed & 0xF) << 20;
    slw[1] = ((u32)port & 0xFF) << 16 | (4u << 27);   /* entries through EP ID 3 */
    epw = (u32 *)(ic + csz * 5);
    epw[0] = (6u << 16) | (3u << 1) | (3u << 3);      /* interval, CErr, int IN */
    epw[1] = (u32)(ep_mps & 0xFFFF);
    epw[2] = PA(d->inr) | 1;
    epw[3] = 0;
    epw[4] = 16;
    if (run_cmd(PA(ic), 0, 0,
                (u32)(TRB_CFGEP << 10) | ((u32)slot << 24), 800) != 1)
        klog("usb: configure endpoint failed slot %d", slot);

    /* pre-queue interrupt IN TRBs (all usable slots), link at the end */
    for (int b = 0; b < IN_TRBS - 1; b++) {
        volatile u32 *tr = d->inr + (u32)b * 4;
        tr[0] = PA(d->in_buf[b]);
        tr[1] = 0;
        tr[2] = 16;
        tr[3] = (TRB_NORMAL << 10) | d->inr_cycle;
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
void usb_poll(void)
{
    if (have_xhci) proc_events();
}

void usb_init(void)
{
    have_xhci = 0;
    n_devs = 0;
    status_line[0] = 0;
    u8 bus[8], dev[8], fn[8];
    int n = pci_find_class(0x0C, 0x03, 0x30, bus, dev, fn, 8);
    if (!n) {
        strcpy(status_line, "usb: no xHCI controller - PS/2 input only");
        klog("%s", status_line);
        return;
    }
    u32 bar0 = pci_read32(bus[0], dev[0], fn[0], 0x10);
    if ((bar0 & 0x7) != 0) {
        strcpy(status_line, "usb: xHCI BAR not memory-mapped");
        klog("%s", status_line);
        return;
    }
    pci_write32(bus[0], dev[0], fn[0], 0x04,
                pci_read32(bus[0], dev[0], fn[0], 0x04) | 0x06);
    cap = (volatile u8 *)(bar0 & ~0xFu);
    u32 caplen = *(volatile u32 *)cap & 0xFF;
    u32 hcs1 = *(volatile u32 *)(cap + 4);
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

    volatile u32 *cmd = (volatile u32 *)op;
    volatile u32 *sts = (volatile u32 *)(op + 4);
    if (cmd[0] & 1) {
        cmd[0] &= ~1u;
        u64 t0 = now_ms();
        while (now_ms() - t0 < 100 && !(sts[0] & (1u << 2))) cpu_hlt();
    }
    cmd[0] |= 1u << 1;                       /* HCRST */
    u64 t0 = now_ms();
    while (now_ms() - t0 < 500 &&
           ((cmd[0] & (1u << 1)) || (sts[0] & (1u << 11)))) cpu_hlt();
    if (sts[0] & (1u << 11)) {
        strcpy(status_line, "usb: xHCI reset timed out");
        klog("%s", status_line);
        return;
    }
    int slots = (int)(hcs1 & 0xFF);
    if (slots > MAX_SLOTS) slots = MAX_SLOTS;
    ((volatile u32 *)(op + 0x38))[0] = (u32)slots;

    dcbaa = (volatile u64 *)palloc(4096);
    memset((void *)dcbaa, 0, 4096);
    wr64((volatile u32 *)(op + 0x30), (u64)PA(dcbaa));

    cmd_ring = (volatile u32 *)palloc(4096);
    memset((void *)cmd_ring, 0, 4096);
    cmd_idx = 0; cmd_cycle = 1;
    wr64((volatile u32 *)(op + 0x18), (u64)PA(cmd_ring) | 1);

    evt_ring = (volatile u32 *)palloc(4096);
    memset((void *)evt_ring, 0, 4096);
    evt_idx = 0; evt_cycle = 1;
    ring_link(evt_ring, EVT_TRBS - 1, 1);
    u32 *erst = palloc(4096);
    memset(erst, 0, 4096);
    erst[0] = PA(evt_ring);
    erst[1] = 0;
    erst[2] = EVT_TRBS;
    erst[3] = 0;
    volatile u32 *ir = (volatile u32 *)(rt + 0x20);
    ir[2] = 1;                               /* ERSTSZ */
    ir[3] = EVT_TRBS;                        /* event ring size */
    wr64(ir + 4, (u64)PA(erst));             /* ERSTBA */
    wr64(ir + 6, (u64)PA(evt_ring));         /* ERDP */

    cmd[0] = 1;                              /* run */
    t0 = now_ms();
    while (now_ms() - t0 < 100 && (sts[0] & (1u << 2))) cpu_hlt();
    if (sts[0] & (1u << 2)) {
        strcpy(status_line, "usb: xHCI would not start");
        klog("%s", status_line);
        return;
    }
    have_xhci = 1;

    int mk = 0, mm = 0;
    for (int p = 1; p <= max_ports; p++) {
        if (!(portsc(p) & 1)) continue;
        int k = enumerate_port(p);
        if (k == 1) mk++;
        if (k == 2) mm++;
    }
    char nl[96], tmp[8];
    strcpy(nl, "usb: xHCI live - ");
    fmt_u32(tmp, (u32)mk); strcat(nl, tmp); strcat(nl, " keyboard, ");
    fmt_u32(tmp, (u32)mm); strcat(nl, tmp); strcat(nl, " mouse (HID boot)");
    strcpy(status_line, nl);
    klog("%s", status_line);
}

void usb_status(char *out, int max)
{
    strncpy(out, status_line[0] ? status_line : "usb: not probed", max - 1);
    out[max - 1] = 0;
}

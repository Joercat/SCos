/* SCos native - USB input driver: xHCI host controller + HID boot protocol.
 *
 * Real PCs attach keyboards and mice to USB, not PS/2. Firmware SMM
 * emulation sometimes fakes a PS/2 keyboard but essentially never a mouse,
 * so without talking to the host controller directly the mouse is invisible
 * on real hardware (v86, which wires PS/2 devices straight to the guest,
 * never showed the problem).
 *
 * Scope: xHCI (the only controller type on modern boards), HID devices in
 * boot protocol (mouse = 3/4-byte report, keyboard = 8-byte report), polled
 * event ring (no MSI/legacy IRQ wiring). One control pipe + one interrupt-IN
 * pipe per device, up to 4 slots. Every stage is logged via klog and
 * summarised for the boot screen so a real-hardware failure names itself.
 */
#include "scos.h"

#define MAX_SLOTS 4
#define CMD_TRBS 64
#define EVT_TRBS 256
#define EP0_TRBS 16
#define IN_TRBS 8

/* TRB types */
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
    int ep_addr;              /* interrupt IN endpoint number */
    volatile u32 *ep0;        /* ep0 ring */
    int ep0_idx; u32 ep0_cycle;
    volatile u32 *inr;        /* interrupt IN ring */
    int inr_idx; u32 inr_cycle;
    u8 *in_buf[IN_TRBS];
    u8 prev_mod;
    u8 prev_keys[6];
};

static volatile u8 *cap, *op, *db, *rt;
static int have_xhci;
static int csz = 32;          /* context size bytes */
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
static volatile u32 cc_code = 0xFF;   /* last command completion code */
static volatile u32 cc_slot;
static char status_line[96];

static u64 now_ms(void) { return tick_count * 10; }

static void wr64(volatile u32 *reg, u64 v)
{
    reg[0] = (u32)(v & 0xFFFFFFFFu);
    reg[1] = (u32)(v >> 32);
}

/* ------------------------------------------------------------- events ---- */
static int proc_events(void)
{
    int work = 0;
    for (int guard = 0; guard < 64; guard++) {
        volatile u32 *t = evt_ring + (u32)evt_idx * 4;
        if ((t[3] & 1) != evt_cycle) break;
        u32 type = (t[3] >> 10) & 0x3F;
        work = 1;
        if (type == EV_CMDCOMP) {
            cc_code = (t[2] >> 24) & 0xFF;
            cc_slot = (t[3] >> 24) & 0xFF;
        } else if (type == EV_TRANSFER) {
            u32 slot = (t[3] >> 24) & 0xFF;
            u32 ep = (t[3] >> 16) & 0x1F;
            u32 code = (t[2] >> 24) & 0xFF;
            u32 len = t[2] & 0xFFFFFF;
            u64 ptr = (u64)t[0] | ((u64)t[1] << 32);
            if (slot <= MAX_SLOTS && devs[slot].used && ep ==
                (u32)(devs[slot].ep_addr * 2 + 1) && (code == 1 || code == 13)) {
                struct xdev *d = &devs[slot];
                int i = -1;
                for (int b = 0; b < IN_TRBS; b++)
                    if ((u64)(u32)d->in_buf[b] == ptr) i = b;
                if (i >= 0) {
                    if (d->kind == 2 && len >= 3) {
                        u8 *r = d->in_buf[i];
                        mouse_inject(r[0], (i32)(i8)r[1], (i32)(i8)r[2],
                                     len >= 4 ? (i32)(i8)r[3] : 0);
                    } else if (d->kind == 1 && len >= 8) {
                        u8 *r = d->in_buf[i];
                        kbd_inject_hid(r[0], r + 2, d->prev_keys, &d->prev_mod);
                    }
                }
                (void)i;
            }
            cc_code = code; cc_slot = slot | (ep << 8) | 0x10000;
            if (slot <= MAX_SLOTS && devs[slot].used && ep ==
                (u32)(devs[slot].ep_addr * 2 + 1)) {
                struct xdev *d2 = &devs[slot];
                volatile u32 *tr = d2->inr + (u32)d2->inr_idx * 4;
                tr[0] = (u32)d2->in_buf[d2->inr_idx];
                tr[1] = 0;
                tr[2] = 16;
                tr[3] = (TRB_NORMAL << 10) | d2->inr_cycle;
                d2->inr_idx++;
                if (d2->inr_idx == IN_TRBS) { d2->inr_idx = 0; d2->inr_cycle ^= 1; }
            }
        } else if (type == EV_PORTCHANGE) {
            u32 port = (t[3] >> 24) & 0xFF;
            if (port >= 1 && port <= (u32)max_ports) {
                volatile u32 *ps = (volatile u32 *)(op + 0x400 + 0x10 * (port - 1));
                ps[0] |= 1 << 1;        /* clear change status */
            }
        }
        evt_idx++;
        if (evt_idx == EVT_TRBS) { evt_idx = 0; evt_cycle ^= 1; }
        volatile u32 *erdp = (volatile u32 *)(rt + 0x20 + 0x18);
        wr64(erdp, (u64)(u32)(evt_ring + (u32)evt_idx * 4) | 1);
    }
    return work;
}

static int wait_event(u64 timeout_ms, u32 want_slot_mask)
{
    u64 t0 = now_ms();
    cc_code = 0xFF;
    cc_slot = 0;
    while (now_ms() - t0 < timeout_ms) {
        proc_events();
        if (cc_slot && (!want_slot_mask || (cc_slot & want_slot_mask)))
            return (int)cc_code;
        cpu_hlt();
    }
    return -1;
}

/* -------------------------------------------------------- rings/doorbells */
static void ring_db(u32 slot, u32 target)
{
    volatile u32 *d = (volatile u32 *)(db + (u32)slot * 4);
    d[0] = target & 0xFF;
}

static volatile u32 *cmd_next(void)
{
    volatile u32 *t = cmd_ring + (u32)cmd_idx * 4;
    return t;
}

static void cmd_advance(void)
{
    cmd_idx++;
    if (cmd_idx == CMD_TRBS - 1) {
        volatile u32 *l = cmd_ring + (u32)cmd_idx * 4;
        l[0] = (u32)(u32)cmd_ring;
        l[1] = 0;
        l[2] = 0;
        l[3] = (TRB_LINK << 10) | (1 << 1) | cmd_cycle;
        cmd_idx = 0;
        cmd_cycle ^= 1;
    }
}

static int run_cmd(volatile u32 *fill, u64 timeout)
{
    volatile u32 *t = cmd_next();
    t[0] = fill[0]; t[1] = fill[1]; t[2] = fill[2];
    t[3] = fill[3] | cmd_cycle;
    cmd_advance();
    *(volatile u32 *)db = 0;           /* command ring doorbell, target 0 */
    return wait_event(timeout, 0);
}

/* ------------------------------------------------------- control pipe ---- */
static int ctrl_xfer(int slot, u8 rt_, u8 rq, u16 val, u16 idx,
                     u8 *buf, u16 len, int in)
{
    struct xdev *d = &devs[slot];
    volatile u32 *setup = d->ep0 + (u32)d->ep0_idx * 4;
    u32 s0 = (u32)rt_ | ((u32)rq << 8) | ((u32)val << 16);
    u32 s1 = (u32)idx | ((u32)len << 16);
    setup[0] = s0;
    setup[1] = s1;
    setup[2] = 8;
    setup[3] = (TRB_SETUP << 10) | (1 << 6) |
               (len ? (in ? 3u : 2u) << 16 : 0) | d->ep0_cycle;
    d->ep0_idx++; if (d->ep0_idx == EP0_TRBS) d->ep0_idx = 0;
    volatile u32 *data = 0;
    if (len) {
        data = d->ep0 + (u32)d->ep0_idx * 4;
        data[0] = (u32)buf;
        data[1] = 0;
        data[2] = len;
        data[3] = (TRB_DATA << 10) | (in ? (1u << 16) : 0) | d->ep0_cycle;
        d->ep0_idx++; if (d->ep0_idx == EP0_TRBS) d->ep0_idx = 0;
    }
    volatile u32 *stat = d->ep0 + (u32)d->ep0_idx * 4;
    stat[0] = 0; stat[1] = 0; stat[2] = 0;
    stat[3] = (TRB_STATUS << 10) | (1 << 5) |
              (len ? (in ? 0u : 1u << 16) : (1u << 16)) | d->ep0_cycle;
    u64 stat_pa = (u64)(u32)stat;
    d->ep0_idx++; if (d->ep0_idx == EP0_TRBS) d->ep0_idx = 0;
    cc_code = 0xFF; cc_slot = 0;
    ring_db((u32)slot, 0);
    u64 t0 = now_ms();
    while (now_ms() - t0 < 800) {
        proc_events();
        if ((cc_slot & 0xFF) == (u32)slot && (cc_slot & 0x10000)) {
            (void)stat_pa;
            return (int)cc_code;
        }
        cpu_hlt();
    }
    return -1;
}

/* --------------------------------------------------------- enumeration ---- */
static u8 desc_buf[512];

static int port_speed(int port)
{
    volatile u32 *ps = (volatile u32 *)(op + 0x400 + 0x10 * (port - 1));
    return (int)((ps[0] >> 10) & 0xF);
}

static void port_reset(int port)
{
    volatile u32 *ps = (volatile u32 *)(op + 0x400 + 0x10 * (port - 1));
    ps[0] |= 1 << 4;                    /* PP: power */
    ps[0] |= 1 << 21;                   /* clear stale PRC */
    ps[0] |= 1 << 4;                    /* PR: reset port */
    u64 t0 = now_ms();
    while (now_ms() - t0 < 400) { if (ps[0] & (1 << 21)) break; cpu_hlt(); }
    ps[0] |= 1 << 21;
}

static int alloc_slot(void)
{
    for (int i = 1; i <= MAX_SLOTS; i++)
        if (!devs[i].used) return i;
    return -1;
}

static void ep0_ring_init(struct xdev *d)
{
    d->ep0_idx = 0;
    d->ep0_cycle = 1;
}

static int enumerate_port(int port)
{
    int speed = port_speed(port);
    if (speed < 1 || speed > 4) return 0;
    port_reset(port);
    speed = port_speed(port);
    if (speed < 1 || speed > 4) return 0;

    u32 cmd[4] = { 0, 0, 0, (u32)(TRB_ENABSLOT << 10) };
    if (run_cmd(cmd, 500) != 1) { klog("usb: enable slot failed"); return 0; }
    int slot = (int)cc_slot;
    if (slot < 1 || slot > MAX_SLOTS || devs[slot].used) return 0;

    struct xdev *d = &devs[slot];
    d->used = 1;
    d->slot = slot;
    d->ep0 = (volatile u32 *)palloc(4096);
    ep0_ring_init(d);
    devctx[slot] = palloc(csz * 4);
    inctx[slot] = palloc(csz * 5);
    if (!d->ep0 || !devctx[slot] || !inctx[slot]) { d->used = 0; return 0; }
    memset((void *)d->ep0, 0, 4096);
    memset(devctx[slot], 0, csz * 4);
    dcbaa[slot] = (u64)(u32)devctx[slot];

    /* input context: add slot + ep0 */
    u8 *ic = inctx[slot];
    memset(ic, 0, csz * 5);
    ic[0] = 0x03;                                   /* add flags: slot, ep0 */
    u32 *slw = (u32 *)(ic + csz);
    slw[0] = (u32)(speed & 0xF) << 20;                /* speed */
    slw[1] = ((u32)port & 0xFF) << 16 | (1u << 27);   /* port, entries = 1 */
    u8 *ep = ic + csz * 2;
    u32 mps = speed == 4 ? 9 : 64;
    u32 *epw = (u32 *)ep;
    epw[0] = (3u << 16) | (3u << 1) | (4u << 3);      /* interval, CErr, control */
    epw[1] = mps;                                     /* max packet */
    epw[2] = (u32)d->ep0 | 1;                         /* dequeue + cycle */
    epw[3] = 0;
    epw[4] = 8;                                       /* avg TRB length */

    u32 ac[4] = { (u32)ic, 0, 0,
                  (u32)(TRB_ADDRDEV << 10) | ((u32)slot << 24) };
    if (run_cmd(ac, 800) != 1) {
        klog("usb: address device failed (port %d)", port);
        d->used = 0;
        return 0;
    }

    /* device descriptor */
    if (ctrl_xfer(slot, 0x80, 6, 0x0100, 0, desc_buf, 18, 1) != 1) {
        klog("usb: get device desc failed");
        d->used = 0;
        return 0;
    }
    /* config descriptor header then full */
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
        klog("usb: slot %d is not a boot HID device", slot);
        d->used = 0;
        return 0;
    }
    ctrl_xfer(slot, 0x00, 9, cfg_val, 0, 0, 0, 0);          /* set config */
    ctrl_xfer(slot, 0x21, 0x0B, 0, (u16)iface, 0, 0, 0);    /* boot protocol */
    ctrl_xfer(slot, 0x21, 0x0A, 0, (u16)iface, 0, 0, 0);    /* set idle 0 */

    d->iface = iface;
    d->ep_addr = ep_addr;
    d->kind = (proto == 2) ? 2 : 1;

    /* interrupt IN ring + configure endpoint */
    d->inr = (volatile u32 *)palloc(4096);
    if (!d->inr) { d->used = 0; return 0; }
    memset((void *)d->inr, 0, 4096);
    d->inr_idx = 0;
    d->inr_cycle = 1;
    for (int b = 0; b < IN_TRBS; b++) d->in_buf[b] = palloc(16);
    memset(ic, 0, csz * 5);
    ic[0] = 0x01 | (1 << 3);                    /* add slot + ep1-in (bit3) */
    slw = (u32 *)(ic + csz);
    slw[0] = (u32)(speed & 0xF) << 20;
    slw[1] = ((u32)port & 0xFF) << 16 | (1u << 27);
    ep = ic + csz * 3;                          /* ep context index 3 */
    epw = (u32 *)ep;
    epw[0] = (6u << 16) | (3u << 1) | (3u << 3); /* interval, CErr, int IN */
    epw[1] = (u32)(ep_mps & 0xFFFF);
    epw[2] = (u32)d->inr | 1;
    epw[3] = 0;
    epw[4] = 16;                                /* avg TRB length */
    u32 cc2[4] = { (u32)ic, 0, 0,
                   (u32)(TRB_CFGEP << 10) | ((u32)slot << 24) };
    if (run_cmd(cc2, 800) != 1)
        klog("usb: configure ep failed slot %d", slot);

    /* pre-queue interrupt IN TRBs */
    for (int b = 0; b < IN_TRBS; b++) {
        volatile u32 *tr = d->inr + (u32)b * 4;
        tr[0] = (u32)d->in_buf[b];
        tr[1] = 0;
        tr[2] = 16;
        tr[3] = (TRB_NORMAL << 10) | d->inr_cycle;
    }
    d->inr_cycle ^= 1;   /* one full lap queued */
    ring_db((u32)slot, (u32)d->ep_addr);
    n_devs++;
    klog("usb: slot %d = HID %s (ep %d, mps %d)", slot,
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
        return;
    }
    pci_write32(bus[0], dev[0], fn[0], 0x04,
                pci_read32(bus[0], dev[0], fn[0], 0x04) | 0x06);
    cap = (volatile u8 *)(bar0 & ~0xFu);
    if (!cap) {
        strcpy(status_line, "usb: xHCI BAR unreadable");
        return;
    }
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
    klog("usb: xHCI at %x, %d ports, ctx %d", (u32)cap, max_ports, csz);

    volatile u32 *cmd = (volatile u32 *)op;
    volatile u32 *sts = (volatile u32 *)(op + 4);
    if (cmd[0] & 1) {                     /* running: stop first */
        cmd[0] &= ~1u;
        u64 t0 = now_ms();
        while (now_ms() - t0 < 100 && !(sts[0] & (1 << 2))) cpu_hlt();
    }
    cmd[0] |= 1 << 1;                     /* HCRST */
    u64 t0 = now_ms();
    while (now_ms() - t0 < 500 && ((cmd[0] & (1 << 1)) || (sts[0] & (1 << 11))))
        cpu_hlt();
    if (sts[0] & (1 << 11)) {
        strcpy(status_line, "usb: xHCI reset timed out");
        klog("%s", status_line);
        return;
    }
    int slots = (int)(hcs1 & 0xFF);
    if (slots > MAX_SLOTS) slots = MAX_SLOTS;
    ((volatile u32 *)(op + 0x38))[0] = (u32)slots;      /* CONFIG */

    dcbaa = (volatile u64 *)palloc(4096);
    memset((void *)dcbaa, 0, 4096);
    wr64((volatile u32 *)(op + 0x30), (u64)(u32)dcbaa);

    cmd_ring = (volatile u32 *)palloc(4096);
    memset((void *)cmd_ring, 0, 4096);
    cmd_idx = 0; cmd_cycle = 1;
    wr64((volatile u32 *)(op + 0x18), (u64)(u32)cmd_ring | 1);

    evt_ring = (volatile u32 *)palloc(4096);
    memset((void *)evt_ring, 0, 4096);
    evt_idx = 0; evt_cycle = 1;
    u32 *erst = palloc(4096);
    memset(erst, 0, 4096);
    erst[0] = (u32)evt_ring;
    erst[1] = 0;
    erst[2] = EVT_TRBS;
    erst[3] = 0;
    volatile u32 *ir = (volatile u32 *)(rt + 0x20);
    ir[2] = 1;                                      /* ERSTSZ */
    ir[3] = EVT_TRBS;                               /* event ring size */
    wr64(ir + 4, (u64)(u32)erst);                   /* ERSTBA */
    wr64(ir + 6, (u64)(u32)evt_ring | 1);           /* ERDP */

    cmd[0] = 1;                                     /* run */
    t0 = now_ms();
    while (now_ms() - t0 < 100 && (sts[0] & (1 << 2))) cpu_hlt();
    if (sts[0] & (1 << 2)) {
        strcpy(status_line, "usb: xHCI would not start");
        klog("%s", status_line);
        return;
    }
    have_xhci = 1;

    /* enumerate whatever is plugged in right now */
    int mk = 0, mm = 0;
    for (int p = 1; p <= max_ports; p++) {
        volatile u32 *ps = (volatile u32 *)(op + 0x400 + 0x10 * (p - 1));
        if (!(ps[0] & 1)) continue;                 /* no device connected */
        int k = enumerate_port(p);
        if (k == 1) mk++;
        if (k == 2) mm++;
    }
    char nl[96];
    strcpy(nl, "usb: xHCI live - ");
    char tmp[8];
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

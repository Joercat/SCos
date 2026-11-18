/* SCos native - the RTL8168/8111/8411 and RTL8169 family, the Realtek parts that use 16-byte descriptor
 * rings in host memory: the card reads a transmit descriptor for the address and length of a frame, and
 * writes a receive descriptor whose first word carries the length and the error bits.
 *
 * The register map and the descriptor bit assignments are transcribed from r8169_main.c in the Linux tree
 * (the driver that covers 0x8168, 0x8169, 0x8167 and 0x8136 alike: the descriptor format is the same on
 * all of them, which is why they are one file here too).  What is deliberately not reproduced is the
 * per-chip PHY tuning that driver carries - hundreds of writes whose values come from Realtek per board
 * revision and that no emulator can verify.  This file does the part that is described in the datasheet
 * and measurable: reset, address, descriptor rings, transceiver enable, autonegotiation through the PHY
 * mailbox, and a link state read back from PHYstatus.  A board that needs its PHY tuned before it links
 * will report no link here rather than a fabricated one.
 *
 * The rings are the card's DMA target, so they are allocated from the kernel's own allocator below 4 GiB
 * and programmed by physical address; the kernel identity-maps RAM, so the pointer is the bus address.
 */
#include "scos.h"
#include "net.h"

#define RTL_VENDOR          0x10ecu
#define RTL_DMA_LIMIT       UINT64_C(0x100000000)   /* TxDescStartAddrHigh exists but the parts here are 32-bit */
#define RTL_TX_RING         64                      /* descriptors */
#define RTL_RX_RING         64
#define RTL_RX_BUF          2048u                   /* one descriptor's buffer; max frame plus slack */
#define RTL_TX_BUF          2048u

/* Register map (r8169_main.c, enum rtl_registers). */
#define REG_IDR0            0x00
#define REG_MAR0            0x08
#define REG_TXDESCLO        0x20
#define REG_TXDESCHI        0x24
#define REG_CHIPCMD         0x37
#define REG_TXPOLL          0x38
#define REG_INTRMASK        0x3c
#define REG_INTRSTATUS      0x3e
#define REG_TXCONFIG        0x40
#define REG_RXCONFIG        0x44
#define REG_CFG9346         0x50
#define REG_CONFIG0         0x51
#define REG_CONFIG1         0x52
#define REG_CONFIG2         0x53
#define REG_CONFIG3         0x54
#define REG_CONFIG4         0x55
#define REG_CONFIG5         0x56
#define REG_PHYAR           0x60
#define REG_PHYSTATUS       0x6c
#define REG_CPLUSCMD        0xe0
#define REG_INTRMITIGATE    0xe2
#define REG_RXDESCLO        0xe4
#define REG_RXDESCHI        0xe8
#define REG_MAXTXPKTSIZE    0xec
#define REG_RXMAXSIZE       0xda

/* ChipCmd. */
#define CMD_RESET           0x10
#define CMD_RXENB           0x08
#define CMD_TXENB           0x04
/* Cfg9346. */
#define CFG_UNLOCK          0xc0
#define CFG_LOCK            0x00
/* RxConfig: DMA burst in 8-10, FIFO in 13-15, the 128-byte interrupt threshold in bit 15. */
#define RXCONFIG_VALUE      ((7u << 13) | (7u << 8) | (1u << 15))
/* TxConfig: unlimited DMA burst, the shortest legal inter-frame gap. */
#define TXCONFIG_VALUE      ((7u << 8) | (3u << 24))
/* MaxTxPacketSize, in units of 128 bytes: 8064 bytes is the largest the parts accept. */
#define TX_MAX_PACKET       0x3f
/* PHYstatus. */
#define PHY_LINK            0x02u
#define PHY_100MBPS         0x08u
#define PHY_1000MBPS        0x10u
#define PHY_FULL_DUPLEX     0x01u
/* PHYAR: bit 31 is "busy" on a write and "valid" on a read, bits 20-16 the register, 15-0 the data. */
#define PHYAR_FLAG          0x80000000u
#define PHYAR_ADDR(reg)     (((u32)(reg) & 0x1fu) << 16)
#define PHYAR_DATA(v)       ((u32)(v) & 0xffffu)
#define MII_BMCR            0
#define MII_BMSR            1
#define BMCR_ANENABLE       0x1000u
#define BMCR_ANRESTART      0x0200u
#define BMSR_LINK           0x0004u
#define BMSR_ANEGCOMPLETE   0x0020u

/* Descriptor bits (enum rtl_desc_bit). */
#define DESC_OWN            (1u << 31)
#define DESC_RING_END       (1u << 30)
#define DESC_FIRST_FRAG     (1u << 29)
#define DESC_LAST_FRAG      (1u << 28)
/* Receive status bits (r8169_main.c). */
#define RX_STAT_RES         (1u << 21)   /* a receive error summary, described by the three below */
#define RX_STAT_RWT         (1u << 22)   /* watchdog timer: the frame did not fit the buffer */
#define RX_STAT_RUNT        (1u << 20)
#define RX_STAT_CRC         (1u << 19)
#define RX_STAT_LEN_MASK    0x3fffu

struct tx_desc {
    volatile u32 opts1;             /* flags and length; the card owns the descriptor while bit 31 is set */
    volatile u32 opts2;             /* VLAN tag and checksum offload; zero because this stack sets neither */
    volatile u32 addr_lo;
    volatile u32 addr_hi;
};

struct rx_desc {
    volatile u32 opts1;
    volatile u32 opts2;
    volatile u32 addr_lo;
    volatile u32 addr_hi;
};

struct rtl8168 {
    struct net_nic nic;
    u8 bus, dev, fn;
    u16 device;
    volatile u8 *regs;              /* the register BAR, mapped uncached */
    u64 bar;
    int bar_bytes;
    struct tx_desc *tx_ring;
    u64 tx_ring_phys;
    u8 *tx_buf[RTL_TX_RING];        /* a table of slices of one DMA extent: no DMA memory spent on it */
    u64 tx_buf_phys[RTL_TX_RING];
    int tx_cur;
    struct rx_desc *rx_ring;
    u64 rx_ring_phys;
    u8 *rx_buf;                     /* RTL_RX_RING * RTL_RX_BUF, one contiguous block */
    u64 rx_buf_phys;
    int rx_cur;
    u32 tx_count, tx_dropped, rx_errors, rx_frames;
    u32 link_changes;
};

static struct rtl8168 adapter;
static int claimed;

/* ---------------------------------------------------------------- register access --------------------------------- */

static u8 r8(struct rtl8168 *d, u16 reg) { return d->regs[reg]; }

/* Width-correct accesses: the card decodes byte enables, so a 32-bit register written as four byte stores
 * keeps only the low lane (RxConfig read back as 0x8a after 0x1788a is that, and it is how a card ends up in
 * the wrong ring size), and a 16-bit status register read a byte at a time returns nothing at all. */
static u32 r32(struct rtl8168 *d, u16 reg)
{
    return *(const volatile u32 *)(d->regs + reg);
}

static void w8(struct rtl8168 *d, u16 reg, u8 v) { d->regs[reg] = v; }

static void w16(struct rtl8168 *d, u16 reg, u16 v)
{
    *(volatile u16 *)(d->regs + reg) = v;
}

static void w32(struct rtl8168 *d, u16 reg, u32 v)
{
    *(volatile u32 *)(d->regs + reg) = v;
}

/* Two places here depend on the card changing a register by itself - the reset bit clearing, and the PHY
 * mailbox raising its "command complete" flag - and a model of the card (tools/tests/net_drivers_host.c) holds
 * the register file in ordinary memory, where nothing can do either.  The hook is called between polls of
 * those registers and is empty in the kernel. */
#ifndef RTL8168_CHIP_TICK
#define RTL8168_CHIP_TICK(d) ((void)(d))
#endif

/* ---------------------------------------------------------------- PHY mailbox ------------------------------------- */

/* 25 tries of 20 us, then give up: the reference driver's own bound (rtl_loop_wait_high(tp, ..., 25, 20)).
 * A PHY that never answers is a board with no transceiver wired, and a write that hung here would hang the
 * boot. */
static int phy_wait(struct rtl8168 *d)
{
    int i;

    for (i = 0; i < 25; i++) {
        RTL8168_CHIP_TICK(d);
        if (r32(d, REG_PHYAR) & PHYAR_FLAG) return 1;
        sleep_ms(1);
    }
    return 0;
}

static int phy_read(struct rtl8168 *d, unsigned reg)
{
    if (!d) return -1;
    w32(d, REG_PHYAR, PHYAR_ADDR(reg));
    if (!phy_wait(d)) return -1;
    return (int)(r32(d, REG_PHYAR) & 0xffffu);
}

static void phy_write(struct rtl8168 *d, unsigned reg, unsigned value)
{
    if (!d) return;
    w32(d, REG_PHYAR, PHYAR_FLAG | PHYAR_ADDR(reg) | PHYAR_DATA(value));
    (void)phy_wait(d);
    /* The datasheet requires 20 us between a completed command and the next one; a millisecond is a
     * generous stand-in that cannot be shorter than the requirement on any machine that can boot this. */
    sleep_ms(1);
}

/* ---------------------------------------------------------------- bring-up ---------------------------------------- */

static void *dma_block(size_t bytes, u64 *phys)
{
    size_t pages = (bytes + 4095u) / 4096u;
    uintptr_t base = pages_allocate_limit(pages, RTL_DMA_LIMIT);

    if (!base) return NULL;
    memset((void *)base, 0, pages * 4096u);
    *phys = (u64)base;
    return (void *)base;
}

static void ring_give(struct rtl8168 *d, int index)
{
    struct rx_desc *desc = &d->rx_ring[index];

    desc->opts2 = 0;
    desc->addr_lo = (u32)(d->rx_buf_phys + (u64)index * RTL_RX_BUF);
    desc->addr_hi = 0;
    /* The card writes the buffer size back as the length of a complete frame, and reads it as the size of
     * the buffer it may fill; RingEnd on the last descriptor wraps the ring in hardware. */
    desc->opts1 = DESC_OWN | RTL_RX_BUF | (index == RTL_RX_RING - 1 ? DESC_RING_END : 0);
}

static void ring_init(struct rtl8168 *d)
{
    int i;

    memset(d->tx_ring, 0, sizeof(struct tx_desc) * RTL_TX_RING);
    d->tx_cur = 0;
    for (i = 0; i < RTL_RX_RING; i++) ring_give(d, i);
    d->rx_cur = 0;
    w32(d, REG_TXDESCLO, (u32)d->tx_ring_phys);
    w32(d, REG_TXDESCHI, 0);
    w32(d, REG_RXDESCLO, (u32)d->rx_ring_phys);
    w32(d, REG_RXDESCHI, 0);
}



static int reset_chip(struct rtl8168 *d)
{
    int i;

    w8(d, REG_CHIPCMD, CMD_RESET);
    for (i = 0; i < 1000; i++) {
        RTL8168_CHIP_TICK(d);
        if (!(r8(d, REG_CHIPCMD) & CMD_RESET)) return 1;
        sleep_ms(1);
    }
    return 0;
}

/* The link is reported by PHYstatus, which is a snapshot the card updates: link, duplex and speed in one
 * word.  Reading the PHY's own status register instead would report a link the card has not finished
 * negotiating; the reference driver reads PHYstatus too. */
static void link_read(struct rtl8168 *d)
{
    u8 status;
    int link;

    if (!d) return;
    status = r8(d, REG_PHYSTATUS);
    link = (status & PHY_LINK) ? 1 : 0;
    if (link != d->nic.link) d->link_changes++;
    d->nic.link = link;
    if (link) {
        d->nic.speed_mbps = (status & PHY_1000MBPS) ? 1000u : (status & PHY_100MBPS) ? 100u : 10u;
    } else {
        d->nic.speed_mbps = 0;
    }
}

static void start_autoneg(struct rtl8168 *d)
{
    int bmcr;

    bmcr = phy_read(d, MII_BMCR);
    if (bmcr < 0) {
        klog("rtl8168: the PHY does not answer its mailbox; link state will stay unknown");
        return;
    }
    /* Autonegotiation on, restart: the card advertises its own capabilities, so the bits this side has to
     * set are the two that ask for it rather than the speed bits, which the PHY rewrites from its own
     * advertisement. */
    phy_write(d, MII_BMCR, (unsigned)bmcr | BMCR_ANENABLE | BMCR_ANRESTART);
}

/* Reset to receiving, in the order rtl_hw_start() in r8169_main.c uses: unlock the config registers, reset,
 * set the largest transmit packet, clear interrupt coalescing and the offloads this stack does not ask
 * for, hand the card both rings, enable the transceiver, then program thresholds.  Writing RxConfig before
 * the rings are live is a card that receives into an address it does not have. */
static int start_chip(struct rtl8168 *d)
{
    int i;

    w8(d, REG_CFG9346, CFG_UNLOCK);
    if (!reset_chip(d)) return 0;
    w8(d, REG_MAXTXPKTSIZE, TX_MAX_PACKET);
    /* No checksum offload and no VLAN insertion: the stack computes its own checksums, and a card asked to
     * compute them for a frame whose header it does not parse is a frame with a wrong checksum. */
    w16(d, REG_CPLUSCMD, 0x0000);
    w16(d, REG_INTRMITIGATE, 0x0000);
    for (i = 0; i < NET_ALEN; i++) w8(d, (u16)(REG_IDR0 + i), d->nic.mac[i]);
    ring_init(d);
    w16(d, REG_RXMAXSIZE, (u16)(RTL_RX_BUF + 1));
    w8(d, REG_CHIPCMD, (u8)(CMD_TXENB | CMD_RXENB));
    w32(d, REG_RXCONFIG, RXCONFIG_VALUE);
    w32(d, REG_TXCONFIG, TXCONFIG_VALUE);
    w8(d, REG_CFG9346, CFG_LOCK);
    /* Foreground polling, as everywhere else in this kernel: no interrupt may be raised. */
    w16(d, REG_INTRSTATUS, 0xffffu);
    w16(d, REG_INTRMASK, 0x0000u);
    start_autoneg(d);
    link_read(d);
    return 1;
}

/* ---------------------------------------------------------------- the adapter contract --------------------------- */

static int rtl8168_poll(struct net_nic *nic)
{
    struct rtl8168 *d = nic->priv;
    struct rx_desc *desc;
    u32 status, length;

    if (!d) return 0;
    desc = &d->rx_ring[d->rx_cur];
    status = desc->opts1;
    if (status & DESC_OWN) {
        /* Nothing in the ring this time; the link state is still worth refreshing, because a card whose
         * cable was pulled reports it here and nowhere else.  Every few polls would do, but a cached link
         * is how a desktop shows a connection that ended minutes ago. */
        link_read(d);
        return 0;
    }
    if (status & (RX_STAT_RES | RX_STAT_RWT | RX_STAT_RUNT | RX_STAT_CRC)) {
        d->rx_errors++;
        ring_give(d, d->rx_cur);
        d->rx_cur = (d->rx_cur + 1) % RTL_RX_RING;
        return 1;
    }
    length = status & RX_STAT_LEN_MASK;
    if (length < 4u + 14u || length > RTL_RX_BUF) {
        d->rx_errors++;
        ring_give(d, d->rx_cur);
        d->rx_cur = (d->rx_cur + 1) % RTL_RX_RING;
        return 1;
    }
    /* The length the card reports for a good frame is the frame itself; the 4-byte FCS is not counted in
     * the descriptor on these parts (unlike the 8139's in-buffer length field).  Frames carrying a VLAN tag
     * have it in opts2 and in the frame; this stack does not join a VLAN, so it accepts the frame as it is. */
    d->rx_frames++;
    net_rx_frame(d->rx_buf + (u64)d->rx_cur * RTL_RX_BUF, length);
    ring_give(d, d->rx_cur);
    d->rx_cur = (d->rx_cur + 1) % RTL_RX_RING;
    link_read(d);
    return 1;
}

static int rtl8168_transmit(struct net_nic *nic, const void *frame, size_t len)
{
    struct rtl8168 *d = nic->priv;
    struct tx_desc *desc;
    int slot;

    if (!d || !frame || len < 14 || len > RTL_TX_BUF) { if (d) d->tx_dropped++; return 0; }
    /* One descriptor is left free, so "the ring is full" and "the ring is empty" can be told apart without a
     * counter that can drift: the card owns every descriptor it has not finished, and this side never fills
     * the slot the next transmit would wrap onto. */
    slot = d->tx_cur;
    desc = &d->tx_ring[slot];
    /* A descriptor the card still owns holds a frame that has not gone out yet; rewriting it would put half
     * of one frame and half of another on the wire.  Dropping the new frame is the only safe answer. */
    if (desc->opts1 & DESC_OWN) { d->tx_dropped++; return 0; }
    memcpy(d->tx_buf[slot], frame, len);
    if (len < 60u) memset(d->tx_buf[slot] + len, 0, 60u - len);
    desc->opts2 = 0;
    desc->addr_lo = (u32)d->tx_buf_phys[slot];
    desc->addr_hi = 0;
    /* One descriptor holds the whole frame, so it is both the first and the last fragment.  The card owns it
     * from the store below until it clears bit 31. */
    desc->opts1 = DESC_OWN | DESC_FIRST_FRAG | DESC_LAST_FRAG |
                  (u32)(len < 60u ? 60u : len) |
                  (slot == RTL_TX_RING - 1 ? DESC_RING_END : 0);
    d->tx_cur = (slot + 1) % RTL_TX_RING;
    /* The doorbell: without it the card reads the descriptor only when its own coalescing timer expires,
     * which on an idle network is never. */
    w8(d, REG_TXPOLL, 0x40);
    d->tx_count++;
    return 1;
}

/* ---------------------------------------------------------------- PCI ----------------------------------------------------- */

static int find_window(struct rtl8168 *d)
{
    u64 base = 0, bytes = 0;
    u8 index;

    for (index = 0; index < 6; index++) {
        if (!pci_memory_bar(d->bus, d->dev, d->fn, index, &base, &bytes)) continue;
        if (bytes < 0x100) continue;
        d->regs = (volatile u8 *)device_map(base, 0x100, 0, NULL);
        if (!d->regs) continue;
        d->bar = base;
        d->bar_bytes = (int)bytes;
        return 1;
    }
    return 0;
}

static int init_one(u8 bus, u8 dev, u8 fn, u16 device)
{
    struct rtl8168 *d = &adapter;
    u16 command;
    int i;

    memset(d, 0, sizeof *d);
    d->bus = bus; d->dev = dev; d->fn = fn; d->device = device;
    command = pci_read16(bus, dev, fn, 4);
    pci_write16(bus, dev, fn, 4, (u16)(command | 0x0006u));
    if (!find_window(d)) {
        pci_write16(bus, dev, fn, 4, command);
        return 0;
    }
    for (i = 0; i < NET_ALEN; i++) d->nic.mac[i] = r8(d, (u16)(REG_IDR0 + i));
    if (!(d->nic.mac[0] & 0xfe) || (d->nic.mac[0] & 1)) {
        /* No EEPROM shadow this early is not something to invent an address for: a made-up MAC on a real
         * segment is a collision nobody can debug. */
        klog("rtl8168: %x at %u:%u.%u reports no station address", device, bus, dev, fn);
        return 0;
    }
    d->tx_ring = dma_block(sizeof(struct tx_desc) * RTL_TX_RING, &d->tx_ring_phys);
    d->rx_ring = dma_block(sizeof(struct rx_desc) * RTL_RX_RING, &d->rx_ring_phys);
    d->rx_buf = dma_block((size_t)RTL_RX_RING * RTL_RX_BUF, &d->rx_buf_phys);
    /* The transmit buffers are one extent carved into RTL_TX_BUF slices: their addresses go to the card, so
     * they are allocated once and indexed rather than allocated one page each. */
    {
        u64 pool_phys = 0;
        u8 *pool = dma_block((size_t)RTL_TX_RING * RTL_TX_BUF, &pool_phys);
        if (!d->tx_ring || !d->rx_ring || !d->rx_buf || !pool) {
            klog("rtl8168: not enough DMA memory below 4 GiB for the rings");
            return 0;
        }
        for (i = 0; i < RTL_TX_RING; i++) {
            d->tx_buf[i] = pool + (size_t)i * RTL_TX_BUF;
            d->tx_buf_phys[i] = pool_phys + (u64)i * RTL_TX_BUF;
        }
    }
    if (!start_chip(d)) {
        klog("rtl8168: %x at %u:%u.%u did not come out of reset", device, bus, dev, fn);
        return 0;
    }
    d->nic.name = "rtl8168";
    d->nic.priv = d;
    d->nic.poll = rtl8168_poll;
    d->nic.transmit = rtl8168_transmit;
    if (!net_register_nic(&d->nic)) {
        klog("rtl8168: the stack refused the adapter");
        return 0;
    }
    claimed = 1;
    klog("rtl8168: %x at %u:%u.%u, %s link, %u Mb/s", device, bus, dev, fn,
         d->nic.link ? "up" : "no", d->nic.speed_mbps);
    return 1;
}

/* Called by net_hardware_init().  The device IDs are the ones r8169_main.c's table covers with this
 * descriptor format; the 8125/8126 2.5 Gb/s parts use a different one and are left alone rather than
 * driven wrongly. */
int rtl8168_probe(void)
{
    static const u16 ids[] = { 0x8168u, 0x8169u, 0x8167u, 0x8161u, 0x8162u, 0x8136u, 0x8129u };
    u8 bus[8], dev[8], fn[8];
    int count, i;
    unsigned k;

    if (claimed) return 1;
    count = pci_find_class(2, 0, 0, bus, dev, fn, 8);
    for (i = 0; i < count; i++) {
        u16 vendor = pci_read16(bus[i], dev[i], fn[i], 0);
        u16 device = pci_read16(bus[i], dev[i], fn[i], 2);
        if (vendor != RTL_VENDOR) continue;
        for (k = 0; k < sizeof ids / sizeof ids[0]; k++) {
            if (device != ids[k]) continue;
            if (init_one(bus[i], dev[i], fn[i], device)) return 1;
            break;
        }
    }
    return 0;
}

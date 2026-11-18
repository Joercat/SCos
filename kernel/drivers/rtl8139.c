/* SCos native - the RTL8139 family, the one Realtek part that is programmed through a pair of register
 * rings rather than descriptor rings: the card DMAs into a linear receive buffer it maintains a write
 * pointer for, and a transmit is a copy into one of four buffers followed by a length written to that
 * buffer's status register.
 *
 * Everything here is transcribed from the register description 8139too.c in the Linux tree uses (the
 * RTL8139/810x family datasheet's register map), not derived from a running card: the offsets, the
 * EEPROM bit-bang, the receive ring's 4-byte header and the 32-bit "the card owns this buffer" bit in
 * TxStatus are the published ones.  Where the reference driver and this file disagree, the reference
 * wins and the difference is a comment.
 *
 * Two consequences of the register map shape this driver:
 *   - the receive buffer has no descriptors, so a frame is preceded by a 4-byte header (status, length
 *     including the FCS) inside the buffer itself, and the ring size is selected by a field in RxConfig.
 *     32 KiB with wrap disabled, which is what makes a full ring a stall rather than a corrupt frame:
 *     the card drops instead of writing a frame across the end of the buffer.
 *   - it has no separate address-per-transmit, so a transmit is a copy, and the four buffers exist to
 *     let two transmits be in flight without waiting for the first to finish.
 *
 * The mapping is not write-combined: a ring in a combination buffer is a ring the card reads stale.
 */
#include "scos.h"
#include "net.h"

#define RTL_VENDOR          0x10ecu
#define RTL_DEV_8139        0x8139u
#define RTL_DMA_LIMIT       UINT64_C(0x100000000)   /* every address the card is given is 32 bits */
#define RTL_RX_RING         0x8000u                 /* 32 KiB, the largest ring that can be no-wrap */
#define RTL_RX_ALIGN        0x8000u                 /* the ring's base is aligned to its own size */
#define RTL_TX_SLOTS        4
#define RTL_TX_BUF          2048u                   /* one maximum Ethernet frame fits with room to spare */
#define RTL_RX_MAX_FRAME    1600u                   /* the largest frame the card will hold, FCS included */

/* Register map (8139too.c, enum RTL8139_registers). */
#define REG_IDR0            0x00
#define REG_TXSTATUS0       0x10
#define REG_TXADDR0         0x20
#define REG_RXBUF           0x30
#define REG_CHIPCMD         0x37
#define REG_RXBUFPTR        0x38        /* CAPR: this side's read offset, written by this side */
#define REG_RXBUFADDR       0x3a        /* CBR: the card's write offset, written by the card */
#define REG_INTRMASK        0x3c
#define REG_INTRSTATUS      0x3e
#define REG_TXCONFIG        0x40
#define REG_RXCONFIG        0x44
#define REG_RXMISSED        0x4c
#define REG_CFG9346         0x50
#define REG_CONFIG0         0x51
#define REG_CONFIG1         0x52
#define REG_CONFIG4         0x54
#define REG_HLTCLK          0x5b
#define REG_MULTIINTR       0x5c
#define REG_BASEMODECTRL    0x62
#define REG_BASEMODESTATUS  0x64
#define REG_NWAYLPAR        0x68

/* ChipCmd bits. */
#define CMD_RESET           0x10
#define CMD_RXENB           0x08
#define CMD_TXENB           0x04
#define CMD_RX_EMPTY        0x01        /* the card has nothing written into the receive buffer */
/* Cfg9346: the config registers are write-protected until this byte is 0xc0. */
#define CFG_UNLOCK          0xc0
#define CFG_LOCK            0x00
/* RxConfig: ring size in bits 11-12, no-wrap in bit 7, FIFO in 13-15, DMA burst in 8-10, filter in 0-3. */
#define RX_RING_32K         (1u << 12)
#define RX_NO_WRAP          (1u << 7)
#define RX_FIFO_THRESH      (7u << 13)
#define RX_DMA_BURST        (7u << 8)
#define RX_ACCEPT_BROADCAST 0x08u
#define RX_ACCEPT_MY_PHYS   0x02u
#define RXCONFIG_VALUE      (RX_RING_32K | RX_NO_WRAP | RX_FIFO_THRESH | RX_DMA_BURST | \
                             RX_ACCEPT_BROADCAST | RX_ACCEPT_MY_PHYS)
/* TxConfig: 9.6us inter-frame gap, unlimited DMA burst, 8 retries. */
#define TXCONFIG_VALUE      ((3u << 24) | (6u << 8) | (8u << 4))
#define TX_FLAGS            TXCONFIG_VALUE
#define TX_HOST_OWNS        0x2000u      /* TxStatus bit 13: set when the card has finished with the buffer */
#define TX_STAT_OK          0x8000u
/* The 4-byte header in front of every received frame. */
#define RX_STATUS_OK        0x0001u
#define RX_STATUS_RUNT      0x0010u
#define RX_STATUS_TOO_LONG  0x0008u
#define RX_STATUS_CRC       0x0004u
#define RX_STATUS_BAD_ALIGN 0x0002u
/* BasicModeStatus, reachable through the chip's internal PHY access. */
#define BMSR_LINK           0x0004u
/* 9346 EEPROM bit-bang, as 8139too.c shifts it. */
#define EE_SHIFT_CLK        0x04
#define EE_CS               0x08
#define EE_DATA_WRITE       0x02
#define EE_DATA_READ        0x01
#define EE_ENB              (0x80 | EE_CS)
#define EE_READ_CMD         6

/* The card's two rings and the bookkeeping that goes with them.  One adapter: an OS with one address
 * and one route has no use for a second 8139, and the stack counts what it will not route. */
struct rtl8139 {
    struct net_nic nic;
    u8 bus, dev, fn;
    volatile u8 *mmio;          /* the memory BAR, when the board decodes one */
    u16 port;                   /* the I/O BAR, when it does not */
    int io;
    u8 *rx;                     /* 32 KiB ring, aligned to 32 KiB */
    u64 rx_phys;
    u8 *tx[RTL_TX_SLOTS];
    u64 tx_phys[RTL_TX_SLOTS];
    int tx_next;
    u16 rx_cur;                 /* where the next unread frame starts, in ring bytes */
    u32 tx_count, tx_dropped, rx_errors, rx_frames;
};

static struct rtl8139 adapter;
static int claimed;

/* ---------------------------------------------------------------- register access --------------------------------- */

static u8 r8(struct rtl8139 *d, u16 reg)
{
    return d->io ? inb((u16)(d->port + reg)) : d->mmio[reg];
}

static u16 r16(struct rtl8139 *d, u16 reg)
{
    u16 v;

    if (d->io) return inw((u16)(d->port + reg));
    /* A register has a width, and the width is not decoration: the card decodes the byte enables of an
     * access, and a 32-bit register written as four byte stores leaves the card with whatever the last
     * store put in the low lane.  RxConfig read back as 0x8a after 0x1788a was written - only the accept
     * bits, because they are the low byte - is exactly that failure, and it is how a card ends up in the
     * wrong ring size with the wrong DMA burst.  Every access below is therefore the register's own width,
     * which on x86 also means an unaligned load is legal. */
    v = *(const volatile u16 *)(d->mmio + reg);
    return v;
}

static u32 r32(struct rtl8139 *d, u16 reg)
{
    if (d->io) return inl((u16)(d->port + reg));
    return *(const volatile u32 *)(d->mmio + reg);
}

static void w8(struct rtl8139 *d, u16 reg, u8 v)
{
    if (d->io) outb((u16)(d->port + reg), v);
    else d->mmio[reg] = v;
}

static void w16(struct rtl8139 *d, u16 reg, u16 v)
{
    if (d->io) { outw((u16)(d->port + reg), v); return; }
    *(volatile u16 *)(d->mmio + reg) = v;
}

static void w32(struct rtl8139 *d, u16 reg, u32 v)
{
    if (d->io) { outl((u16)(d->port + reg), v); return; }
    *(volatile u32 *)(d->mmio + reg) = v;
}


/* The one place this driver depends on the card changing a register by itself is the reset bit clearing, and a
 * model of the card - tools/tests/net_drivers_host.c - holds the register file in ordinary memory, where
 * nothing can clear that bit.  The hook is called between polls of the register and is empty in the kernel. */
#ifndef RTL8139_CHIP_TICK
#define RTL8139_CHIP_TICK(d) ((void)(d))
#endif

/* A read back after a write: the card has posted writes, and the reset bit is only meaningful once the
 * store has arrived. */
static void flush_write(struct rtl8139 *d, u16 reg) { (void)r8(d, reg); }

/* ---------------------------------------------------------------- EEPROM ----------------------------------------- */

/* Sixteen bits out of the serial EEPROM, through the 9346 command register: the read opcode, the address
 * shifted out on the data bit while the clock toggles, then sixteen clocks back in.  This is the only way
 * to learn the address the board's manufacturer burned in when the IDR registers read zero, and some
 * boards (a 8100 in a thin client, in the reference driver's own history) do exactly that. */
static u16 eeprom_read(struct rtl8139 *d, unsigned address, unsigned addr_bits)
{
    unsigned command = address | ((unsigned)EE_READ_CMD << addr_bits);
    u16 value = 0;
    int i;

    w8(d, REG_CFG9346, CFG_UNLOCK);
    w8(d, REG_CFG9346, (u8)(EE_ENB & ~EE_CS));
    w8(d, REG_CFG9346, (u8)EE_ENB);
    for (i = 4 + (int)addr_bits; i >= 0; i--) {
        u8 bit = (command & (1u << i)) ? EE_DATA_WRITE : 0;
        w8(d, REG_CFG9346, (u8)(EE_ENB | bit));
        w8(d, REG_CFG9346, (u8)(EE_ENB | bit | EE_SHIFT_CLK));
        w8(d, REG_CFG9346, (u8)(EE_ENB | bit));
        w8(d, REG_CFG9346, (u8)(EE_ENB | bit | EE_SHIFT_CLK));
        w8(d, REG_CFG9346, (u8)(EE_ENB | bit));
    }
    w8(d, REG_CFG9346, (u8)EE_ENB);
    for (i = 16; i > 0; i--) {
        w8(d, REG_CFG9346, (u8)(EE_ENB | EE_SHIFT_CLK));
        value = (u16)((value << 1) | (r8(d, REG_CFG9346) & EE_DATA_READ ? 1u : 0u));
        w8(d, REG_CFG9346, (u8)EE_ENB);
    }
    /* Terminate the cycle, then lock the config registers again. */
    w8(d, REG_CFG9346, EE_CS);
    w8(d, REG_CFG9346, CFG_LOCK);
    return value;
}

static int address_is_usable(const u8 *mac)
{
    int i, any = 0, all_ff = 1;

    for (i = 0; i < NET_ALEN; i++) { if (mac[i]) any = 1; if (mac[i] != 0xff) all_ff = 0; }
    /* A multicast or all-zero address is not an address: a card whose EEPROM could not be read would
     * otherwise join the network as 00:00:00:00:00:00 and every DHCP server would refuse it. */
    if (!any || all_ff) return 0;
    if (mac[0] & 0x01) return 0;
    return 1;
}

static int read_mac(struct rtl8139 *d, u8 *mac)
{
    int i;

    for (i = 0; i < NET_ALEN; i++) mac[i] = r8(d, (u16)(REG_IDR0 + i));
    if (address_is_usable(mac)) return 1;
    /* The IDR is loaded from the EEPROM at reset, so a zero IDR means the load has not happened or the
     * board has no EEPROM shadow; read it serially instead.  8-bit addresses on the 8139 (the 0x8129
     * signature at word 0 tells the wide parts apart). */
    {
        unsigned bits = eeprom_read(d, 0, 6) == 0x8129 ? 8 : 6;
        for (i = 0; i < 3; i++) {
            u16 word = eeprom_read(d, (unsigned)(i + 7), bits);
            mac[i * 2] = (u8)word;
            mac[i * 2 + 1] = (u8)(word >> 8);
        }
    }
    return address_is_usable(mac);
}

/* ---------------------------------------------------------------- bring-up ---------------------------------------- */

/* One page-aligned block from the kernel's own allocator, below 4 GiB because the card's address registers
 * are 32 bits wide.  The kernel identity-maps RAM, so the pointer is the bus address. */
static void *dma_block(size_t bytes, u64 *phys)
{
    size_t pages = (bytes + 4095u) / 4096u;
    uintptr_t base = pages_allocate_limit(pages, RTL_DMA_LIMIT);

    if (!base) return NULL;
    memset((void *)base, 0, pages * 4096u);
    *phys = (u64)base;
    return (void *)base;
}

static void ring_program(struct rtl8139 *d)
{
    int i;

    w32(d, REG_RXBUF, (u32)d->rx_phys);
    for (i = 0; i < RTL_TX_SLOTS; i++) w32(d, (u16)(REG_TXADDR0 + i * 4), (u32)d->tx_phys[i]);
}

static int reset_chip(struct rtl8139 *d)
{
    int i;

    w8(d, REG_CHIPCMD, CMD_RESET);
    flush_write(d, REG_CHIPCMD);
    for (i = 0; i < 1000; i++) {
        RTL8139_CHIP_TICK(d);
        if (!(r8(d, REG_CHIPCMD) & CMD_RESET)) return 1;
        sleep_ms(1);
    }
    return 0;
}

static void link_read(struct rtl8139 *d)
{
    u16 status;

    if (!d) return;
    status = r16(d, REG_BASEMODESTATUS);
    d->nic.link = (status & BMSR_LINK) ? 1 : 0;
    if (d->nic.link) {
        u16 lpa = r16(d, REG_NWAYLPAR);
        /* LPA bits 9:8 are the peer's technology: 01 is 10 Mb/s, 10 is 100 Mb/s.  The 8139 family has
         * no third speed, so anything else is "linked, speed unknown" rather than a guess. */
        d->nic.speed_mbps = ((lpa >> 8) & 3u) == 1u ? 10u : ((lpa >> 8) & 3u) == 2u ? 100u : 0u;
    } else {
        d->nic.speed_mbps = 0;
    }
}

/* Program the card from reset to receiving.  Order matters and follows the reference driver: reset, unlock
 * the config registers, restore the address, hand the card both ring addresses, enable the transceiver,
 * and only then set the thresholds - the datasheet requires Tx/Rx enabled before RxConfig/TxConfig are
 * written, and a card whose config is written first receives nothing. */
static int start_chip(struct rtl8139 *d)
{
    int i;

    /* The clock on the 8139D and the 810x parts is stopped when the firmware leaves the card in D3; 'R'
     * starts it again.  On the parts without the register this write is to a reserved location and the
     * reference driver makes the same write. */
    w8(d, REG_HLTCLK, 'R');
    if (!reset_chip(d)) return 0;

    w8(d, REG_CFG9346, CFG_UNLOCK);
    /* Config1 bits 0-1 are PWRDN and SLEEP on the earliest 8139s; leaving either set is a card that
     * resets to the same stopped state. */
    w8(d, REG_CONFIG1, (u8)(r8(d, REG_CONFIG1) & (u8)~0x03u));
    for (i = 0; i < NET_ALEN; i++) w8(d, (u16)(REG_IDR0 + i), d->nic.mac[i]);
    w8(d, REG_CFG9346, CFG_LOCK);

    d->rx_cur = 0;
    ring_program(d);
    w8(d, REG_CHIPCMD, (u8)(CMD_RXENB | CMD_TXENB));
    w32(d, REG_RXCONFIG, RXCONFIG_VALUE);
    w32(d, REG_TXCONFIG, TXCONFIG_VALUE);
    /* The missed-packet counter is write-to-clear and starts at whatever the firmware left. */
    w32(d, REG_RXMISSED, 0);
    /* Foreground polling: no interrupt is wanted, so make sure none can be raised.  The mask is cleared
     * last, after the status register, because a mask of zero with a stale status bit is still a card
     * that asserts its line. */
    w16(d, REG_MULTIINTR, (u16)(r16(d, REG_MULTIINTR) & 0xf000u));
    w16(d, REG_INTRSTATUS, 0xffffu);
    w16(d, REG_INTRMASK, 0x0000u);
    link_read(d);
    return 1;
}

/* ---------------------------------------------------------------- the adapter contract --------------------------- */

static int rtl8139_poll(struct net_nic *nic)
{
    struct rtl8139 *d = nic->priv;
    u16 status, length;
    u8 *frame;

    if (!d) return 0;
    link_read(d);
    /* "Is the ring empty" is the card's answer, not ours: ChipCmd bit 0 is the card saying it has nothing
     * written.  The receive buffer address register (0x3a) is the card's write pointer; the pointer at 0x38
     * is the driver's own read pointer (CAPR), which the card never writes - reading 0x38 to find out whether
     * a frame arrived is reading back the value this side last stored there, and a ring polled that way
     * re-parses whatever sits at the read offset forever. */
    if ((r8(d, REG_CHIPCMD) & CMD_RX_EMPTY) || r16(d, REG_RXBUFADDR) == d->rx_cur) return 0;

    /* A no-wrap ring never splits a frame, so a frame can only start where a maximum-sized one would fit;
     * when the next start would not fit, the card has wrapped and the header it left at the end of the buffer
     * is a marker, not a frame. */
    if ((u32)d->rx_cur + 4u + RTL_RX_MAX_FRAME > RTL_RX_RING) {
        d->rx_cur = 0;
        w16(d, REG_RXBUFPTR, (u16)(RTL_RX_RING - 16u));
        return 1;
    }

    frame = d->rx + d->rx_cur;
    status = (u16)(frame[0] | (frame[1] << 8));
    length = (u16)(frame[2] | (frame[3] << 8));
    if (!(status & RX_STATUS_OK) || (status & (RX_STATUS_RUNT | RX_STATUS_TOO_LONG |
                                              RX_STATUS_CRC | RX_STATUS_BAD_ALIGN)) ||
        length < 4u + 14u || length > RTL_RX_MAX_FRAME + 4u ||
        d->rx_cur + 4u + length > RTL_RX_RING) {
        /* Lost in the ring: the header cannot be trusted to say where the next one starts, so the receiver is
         * restarted from zero, which is what the reference driver does with a bad header rather than walking
         * off the end of its own buffer. */
        d->rx_errors++;
        d->rx_cur = 0;
        w8(d, REG_CHIPCMD, (u8)(r8(d, REG_CHIPCMD) & ~CMD_RXENB));
        w8(d, REG_CHIPCMD, (u8)(CMD_RXENB | CMD_TXENB));
        w16(d, REG_RXBUFPTR, (u16)(RTL_RX_RING - 16u));
        return 1;
    }
    /* The length field counts the 4-byte FCS the card appended; the stack is given the frame without it. */
    d->rx_frames++;
    net_rx_frame(frame + 4, (size_t)length - 4u);
    d->rx_cur = (u16)((d->rx_cur + 4u + length + 3u) & ~3u);
    if (d->rx_cur >= RTL_RX_RING) d->rx_cur = 0;
    /* Releasing the space: the card will not write past CAPR, so a driver that never updates it fills the ring
     * once and then receives nothing while still reporting a link.  The card's own convention, sixteen bytes
     * behind the read offset, is the reference driver's (RxBufPtr = cur_rx - 16). */
    w16(d, REG_RXBUFPTR, (u16)(d->rx_cur >= 16u ? d->rx_cur - 16u : RTL_RX_RING - 16u));
    return 1;
}

static int rtl8139_transmit(struct net_nic *nic, const void *frame, size_t len)
{
    struct rtl8139 *d = nic->priv;
    int slot;
    u32 status;
    size_t wire;

    if (!d || !frame || len < 14 || len > RTL_TX_BUF) { if (d) d->tx_dropped++; return 0; }
    slot = d->tx_next;
    /* TxStatus's host-owns bit is set when the card has finished the buffer; a buffer still owned by the
     * card must not be rewritten, or the frame on the wire becomes a mix of two. */
    status = r32(d, (u16)(REG_TXSTATUS0 + slot * 4));
    if (!(status & TX_HOST_OWNS)) { d->tx_dropped++; return 0; }
    /* The 8139 does not pad, so a frame shorter than the 60-byte Ethernet minimum needs the zeros here. */
    wire = len < 60u ? 60u : len;
    memcpy(d->tx[slot], frame, len);
    if (wire > len) memset(d->tx[slot] + len, 0, wire - len);
    d->tx_next = (slot + 1) % RTL_TX_SLOTS;
    /* Writing the length to TxStatus is the doorbell: the card reads the buffer only once this lands. */
    w32(d, (u16)(REG_TXSTATUS0 + slot * 4), TX_FLAGS | (u32)wire);
    d->tx_count++;
    return 1;
}

/* ---------------------------------------------------------------- PCI ----------------------------------------------------- */

/* The memory BAR, then the I/O BAR: a 8139 decodes whichever the board wired, and both are the same
 * register file.  A card that offers neither is not a card this driver can drive. */
static int find_window(struct rtl8139 *d)
{
    u64 base = 0, bytes = 0;
    u8 index;
    u32 value;

    for (index = 0; index < 6; index++) {
        if (!pci_memory_bar(d->bus, d->dev, d->fn, index, &base, &bytes)) continue;
        if (bytes < 0x100) continue;
        d->mmio = (volatile u8 *)device_map(base, 0x100, 0, NULL);
        if (!d->mmio) continue;
        d->io = 0;
        return 1;
    }
    for (index = 0; index < 6; index++) {
        value = pci_read32(d->bus, d->dev, d->fn, (u8)(0x10 + index * 4));
        if (!(value & 1u) || !(value & ~3u)) continue;
        d->port = (u16)(value & ~3u);
        d->io = 1;
        return 1;
    }
    return 0;
}

static int init_one(u8 bus, u8 dev, u8 fn)
{
    struct rtl8139 *d = &adapter;
    uintptr_t raw;
    u16 command;
    int i;

    memset(d, 0, sizeof *d);
    d->bus = bus; d->dev = dev; d->fn = fn;
    /* Memory decode and bus mastering: the card has to be able to read the frames this side hands it. */
    command = pci_read16(bus, dev, fn, 4);
    pci_write16(bus, dev, fn, 4, (u16)(command | 0x0006u));
    if (!find_window(d)) {
        pci_write16(bus, dev, fn, 4, command);
        return 0;
    }
    if (!read_mac(d, d->nic.mac)) {
        klog("rtl8139: %u:%u.%u has no readable station address", bus, dev, fn);
        return 0;
    }
    /* The ring is aligned to its own size: the card's write pointer is a 16-bit byte offset into a
     * buffer whose length is a power of two, and an unaligned base is a frame header split across the
     * end of the buffer in the parts that ignore no-wrap. */
    d->rx = dma_block(RTL_RX_RING + RTL_RX_ALIGN, &d->rx_phys);
    if (!d->rx) { klog("rtl8139: no DMA memory for the 32 KiB receive ring"); return 0; }
    raw = (uintptr_t)d->rx;
    raw = (raw + RTL_RX_ALIGN - 1u) & ~(uintptr_t)(RTL_RX_ALIGN - 1u);
    d->rx = (u8 *)raw;
    d->rx_phys = (u64)raw;
    for (i = 0; i < RTL_TX_SLOTS; i++) {
        d->tx[i] = dma_block(RTL_TX_BUF, &d->tx_phys[i]);
        if (!d->tx[i]) { klog("rtl8139: no DMA memory for transmit buffer %u", i); return 0; }
    }
    if (!start_chip(d)) {
        klog("rtl8139: %u:%u.%u did not come out of reset", bus, dev, fn);
        return 0;
    }
    d->nic.name = "rtl8139";
    d->nic.priv = d;
    d->nic.poll = rtl8139_poll;
    d->nic.transmit = rtl8139_transmit;
    if (!net_register_nic(&d->nic)) {
        klog("rtl8139: the stack refused the adapter");
        return 0;
    }
    claimed = 1;
    klog("rtl8139: %u:%u.%u %s, %s link, %u Mb/s", bus, dev, fn, d->io ? "I/O BAR" : "memory BAR",
         d->nic.link ? "up" : "no", d->nic.speed_mbps);
    return 1;
}

/* Called by net_hardware_init().  Returns 1 when this driver claimed an adapter, and is idempotent: a
 * second call does not reinitialize a card that is already receiving. */
int rtl8139_probe(void)
{
    u8 bus[8], dev[8], fn[8];
    int count, i;

    if (claimed) return 1;
    count = pci_find_class(2, 0, 0, bus, dev, fn, 8);
    for (i = 0; i < count; i++) {
        u16 vendor = pci_read16(bus[i], dev[i], fn[i], 0);
        u16 device = pci_read16(bus[i], dev[i], fn[i], 2);
        u8 revision = pci_read8(bus[i], dev[i], fn[i], 8);
        if (vendor != RTL_VENDOR || device != RTL_DEV_8139) continue;
        /* Revision 0x20 and later is the 8139C+, which can be driven two ways: the C mode this file
         * programs, or the C+ mode with descriptor rings at 0xe0 and a C+ command register.  C+ mode is
         * opt-in - it is entered by writing CpCmd (0xe0), a register this file never touches - so a C+
         * part left alone runs the C-mode register set and the C-mode receive buffer exactly as an
         * 8139C does, which is why the revision is reported and not refused.  The receive-buffer and
         * transmit paths below are the C-mode ones and stay that way: a chip in C+ mode would read the
         * ring addresses this file writes to RxBuf as descriptor addresses, so the mode is a decision,
         * not a fallback. */
        if (revision >= 0x20)
            klog("rtl8139: %u:%u.%u is a 8139C+ (rev %x) driven in C mode", bus[i], dev[i], fn[i], revision);
        if (init_one(bus[i], dev[i], fn[i])) return 1;
    }
    return 0;
}

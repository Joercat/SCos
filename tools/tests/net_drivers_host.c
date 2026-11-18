/* The two Realtek drivers, compiled against a model of the PCI function and the register file.
 *
 * kernel/drivers/rtl8139.c and kernel/drivers/rtl8168.c cannot be exercised on this machine - there is no
 * card - and they are exactly the kind of code where "it compiled" is worth nothing: a descriptor written
 * with the wrong address bit, a ring re-armed without its own bit, a doorbell never rung.  So the file
 * under test is included as it is (same source the kernel links) and everything below it is replaced by a
 * model: PCI configuration space answers with one function, device_map() hands back a register array, the
 * page allocator hands back a static arena so a ring address is a host pointer that can be inspected, and
 * the stack is a recorder instead of a network.
 *
 *   gcc -std=gnu11 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector \
 *      -fsanitize=address,undefined -fno-sanitize-recover=all -Ikernel/include \
 *      -DSCOS_TEST_RTL8139 tools/tests/net_drivers_host.c -o /tmp/rtl8139
 *   gcc ... -DSCOS_TEST_RTL8168 tools/tests/net_drivers_host.c -o /tmp/rtl8168
 *
 * What is asserted is the register programming the datasheet describes and the ring arithmetic: the
 * addresses the card is given are the ones the driver allocated, the transmit descriptor carries the
 * frame's own length and address, a receive descriptor is handed back to the card with its ownership bit
 * set, and a frame that fails the card's error bits never reaches the stack.
 */
#include <stdio.h>
#include <sys/mman.h>   /* the DMA arena is mapped below 4 GiB, because the card's addresses are 32 bits */

#include "scos.h"
#include "net.h"

/* ---------------------------------------------------------------- the model's state -------------------------------- */

/* The kernel's own string routines, as the kernel declares them: this harness is compiled freestanding and
 * links no C library, and scos.h's declarations are the ones the driver code under test calls. */
void *memcpy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, size_t n) { uint8_t *a = d; while (n--) *a++ = (uint8_t)c; return d; }
int memcmp(const void *a, const void *b, size_t n) { const uint8_t *x = a, *y = b; while (n--) { if (*x != *y) return *x - *y; x++; y++; } return 0; }
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }

static int checks, failures;

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) { failures++; printf("  FAIL %s\n", what); }
}

static unsigned mock_sleeps;
static unsigned mock_klog_lines;

/* ---------------------------------------------------------------- PCI model ---------------------------------------- */

#define MOCK_BAR_BYTES 0x100

struct mock_fn {
    int present;
    u8 bus, dev, fn;
    u16 vendor, device, command;
    u8 revision;
    u32 bar;
};

static struct mock_fn mock_function;
static u32 mock_bar[MOCK_BAR_BYTES / 4];      /* one register file, word addressed */
static unsigned mock_pci_writes;

static struct mock_fn *mock_lookup(u8 bus, u8 dev, u8 fn)
{
    if (!mock_function.present) return NULL;
    if (mock_function.bus != bus || mock_function.dev != dev || mock_function.fn != fn) return NULL;
    return &mock_function;
}

int pci_find_class(u8 cls, u8 sub, u8 pi, u8 *bus, u8 *dev, u8 *fn, int max)
{
    (void)pi;
    if (!mock_function.present || max < 1 || cls != 2 || sub != 0) return 0;
    bus[0] = mock_function.bus;
    dev[0] = mock_function.dev;
    fn[0] = mock_function.fn;
    return 1;
}

u16 pci_read16(u8 bus, u8 dev, u8 fn, u8 off)
{
    struct mock_fn *f = mock_lookup(bus, dev, fn);

    if (!f) return 0xffffu;
    switch (off) {
        case 0: return f->vendor;
        case 2: return f->device;
        case 4: return f->command;
        default: return 0;
    }
}

u8 pci_read8(u8 bus, u8 dev, u8 fn, u8 off)
{
    struct mock_fn *f = mock_lookup(bus, dev, fn);

    if (!f) return 0xffu;
    return off == 8 ? f->revision : 0;
}

u32 pci_read32(u8 bus, u8 dev, u8 fn, u8 off)
{
    struct mock_fn *f = mock_lookup(bus, dev, fn);

    if (!f) return 0xffffffffu;
    if (off == 0) return (u32)f->vendor | ((u32)f->device << 16);
    if (off == 0x10) return f->bar;
    return 0;
}

void pci_write16(u8 bus, u8 dev, u8 fn, u8 off, u16 v)
{
    struct mock_fn *f = mock_lookup(bus, dev, fn);

    mock_pci_writes++;
    if (f && off == 4) f->command = v;
}

void pci_write32(u8 bus, u8 dev, u8 fn, u8 off, u32 v)
{
    (void)bus; (void)dev; (void)fn; (void)off; (void)v;
    mock_pci_writes++;
}

int pci_memory_bar(u8 bus, u8 dev, u8 fn, u8 index, u64 *base, u64 *bytes)
{
    struct mock_fn *f = mock_lookup(bus, dev, fn);

    if (!f || index != 0 || !f->bar) return 0;
    *base = f->bar;
    *bytes = MOCK_BAR_BYTES;
    return 1;
}

void *device_map(u64 physical, u64 bytes, int write_combine, u64 *mapped_bytes)
{
    (void)write_combine;
    if (!mock_function.present || physical != mock_function.bar) return NULL;
    if (mapped_bytes) *mapped_bytes = bytes;
    return mock_bar;
}

/* ---------------------------------------------------------------- memory and the rest of the kernel ---------------- */

/* The driver hands these addresses to the card, and both parts take 32-bit addresses, so the model's arena has
 * to live below 4 GiB too - otherwise every "the address fits" check would pass for the wrong reason. */
static size_t mock_arena_used;
static uint8_t *mock_arena(void)
{
    static uint8_t *arena;

    if (!arena) {
        void *p = mmap(NULL, 512 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (p == MAP_FAILED) { printf("the harness could not map its DMA arena\n"); __builtin_trap(); }
        arena = p;
    }
    return arena;
}

uintptr_t pages_allocate_limit(size_t count, u64 limit)
{
    size_t bytes = count * 4096u;
    uintptr_t at;

    if (bytes > limit) return 0;
    if (mock_arena_used + bytes > 512u * 1024u) return 0;
    at = (uintptr_t)(mock_arena() + mock_arena_used);
    mock_arena_used += bytes;
    memset((void *)at, 0, bytes);
    return at;
}

void sleep_ms(u32 ms) { mock_sleeps += ms ? ms : 1; }
void klog(const char *fmt, ...) { (void)fmt; mock_klog_lines++; }
void klog_raw(const char *s) { (void)s; }
void panic(const char *s) { printf("PANIC: %s\n", s); __builtin_trap(); }

/* The stack: a recorder.  What the driver hands it is what these two functions see. */
static struct net_nic *mock_registered;
static int mock_registered_count;

int net_register_nic(struct net_nic *nic)
{
    mock_registered_count++;
    if (mock_registered) return 1;
    mock_registered = nic;
    return 1;
}

static uint8_t mock_frame[2048];
static size_t mock_frame_len;
static unsigned mock_rx_calls;

void net_rx_frame(const void *frame, size_t len)
{
    mock_rx_calls++;
    if (len > sizeof mock_frame) len = sizeof mock_frame;
    memcpy(mock_frame, frame, len);
    mock_frame_len = len;
}

/* ---------------------------------------------------------------- the chip's own progress --------------------------- */

/* A model of the card holds its register file in ordinary memory: a store lands and stays.  Two registers in
 * these drivers are expected to change themselves - the reset bit clearing, and the PHY mailbox's completion
 * flag - so the driver calls a hook between polls of them (empty in the kernel) and the model does here what
 * the silicon would.  A stuck reset is a flag away, because a card that never leaves reset is exactly the
 * failure this driver has to refuse rather than hang on. */
static int mock_reset_stuck;
static u16 mock_phy_bmcr = 0x7949;

static void mock_chip_tick(void *mmio_raw, u16 chip_cmd, u16 phyar)
{
    volatile u8 *m = mmio_raw;
    u32 mailbox;

    if (!m) return;
    if (!mock_reset_stuck && (m[chip_cmd] & 0x10u)) m[chip_cmd] &= (u8)~0x10u;
    if (!phyar) return;
    mailbox = ((u32)m[phyar]) | ((u32)m[phyar + 1] << 8) | ((u32)m[phyar + 2] << 16) | ((u32)m[phyar + 3] << 24);
    if (mailbox & 0x80000000u) return;          /* a command that has not completed yet, or one just written */
    if (((mailbox >> 16) & 0x1fu) == 0) {       /* a read of the control register */
        u32 answer = 0x80000000u | mock_phy_bmcr;
        m[phyar] = (u8)answer;
        m[phyar + 1] = (u8)(answer >> 8);
        m[phyar + 2] = (u8)(answer >> 16);
        m[phyar + 3] = (u8)(answer >> 24);
    }
}

#define RTL8139_CHIP_TICK(d) mock_chip_tick((void *)(d)->mmio, REG_CHIPCMD, 0)   /* no PHY mailbox */
#define RTL8168_CHIP_TICK(d) mock_chip_tick((void *)(d)->regs, REG_CHIPCMD, REG_PHYAR)

/* ---------------------------------------------------------------- the driver under test --------------------------- */

#if defined(SCOS_TEST_RTL8139)
#include "../../kernel/drivers/rtl8139.c"
#define DRIVER_NAME "rtl8139"
#elif defined(SCOS_TEST_RTL8168)
#include "../../kernel/drivers/rtl8168.c"
#define DRIVER_NAME "rtl8168"
#else
#error "define SCOS_TEST_RTL8139 or SCOS_TEST_RTL8168"
#endif

/* ---------------------------------------------------------------- helpers ------------------------------------------- */

static void reset_model(void)
{
    memset(&mock_function, 0, sizeof mock_function);
    memset(mock_bar, 0, sizeof mock_bar);
    mock_arena_used = 0;
    mock_sleeps = 0;
    mock_klog_lines = 0;
    mock_pci_writes = 0;
    mock_registered = NULL;
    mock_registered_count = 0;
    mock_frame_len = 0;
    mock_rx_calls = 0;
    mock_reset_stuck = 0;
    claimed = 0;
}

static u32 reg(u16 offset) { return mock_bar[offset / 4]; }
static u8 bar_byte(u16 offset) { return ((const u8 *)mock_bar)[offset]; }
static void set_reg(u16 offset, u32 value) { mock_bar[offset / 4] = value; }
/* 0x3a is not word aligned: the card's write pointer is a 16-bit register at an odd half of a word, which is
 * exactly why the driver accesses it as a 16-bit load and store rather than as a whole word.  The 8168 model
 * does not need it, because that card's registers are all word- or dword-sized. */
#ifdef SCOS_TEST_RTL8139
static void set_reg16(u16 offset, u16 value)
{
    ((u8 *)mock_bar)[offset] = (u8)value;
    ((u8 *)mock_bar)[offset + 1] = (u8)(value >> 8);
}
#endif

static void mac_from(const char *text)
{
    unsigned v[NET_ALEN];
    int i;

    if (sscanf(text, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != NET_ALEN) {
        printf("bad MAC in the harness itself\n");
        __builtin_trap();
    }
    for (i = 0; i < NET_ALEN; i++) mock_bar[i / 4] |= v[i] << (8 * (i % 4));
}

static void present(u16 vendor, u16 device, u8 revision)
{
    mock_function.present = 1;
    mock_function.bus = 0;
    mock_function.dev = 3;
    mock_function.fn = 0;
    mock_function.vendor = vendor;
    mock_function.device = device;
    mock_function.revision = revision;
    mock_function.bar = 0xf0000000u;
}

static void check_bus_mastering(const char *what)
{
    check(what, (mock_function.command & 0x0006u) == 0x0006u);
}

#if defined(SCOS_TEST_RTL8139)

/* ---------------------------------------------------------------- RTL8139 -------------------------------------------------- */

static void test_absent_card_claims_nothing(void)
{
    reset_model();
    check("no PCI function at all is not an adapter", rtl8139_probe() == 0);
    check("the stack was told nothing", mock_registered == NULL);
}

static void test_a_8139_is_programmed_from_reset_to_receiving(void)
{
    reset_model();
    present(0x10ec, 0x8139, 0x10);
    mac_from("52:54:00:12:34:56");
    check("the 8139 is claimed", rtl8139_probe() == 1);
    check("an adapter reached the stack", mock_registered != NULL && mock_registered_count == 1);
    check("the adapter is named for its driver", mock_registered && strcmp(mock_registered->name, "rtl8139") == 0);
    if (!mock_registered) { fflush(stdout); return; }
    check("the station address is the one in the IDR registers",
          memcmp(mock_registered->mac, (const uint8_t[]){ 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 }, 6) == 0);
    check_bus_mastering("memory decode and bus mastering were enabled on the card's function");
    check("the transceiver was enabled", bar_byte(REG_CHIPCMD) == (CMD_RXENB | CMD_TXENB));
    check("the receive buffer address was programmed", reg(REG_RXBUF) != 0 && (reg(REG_RXBUF) & 0x1f) == 0);
    check("the receive configuration is the 32 KiB no-wrap ring with both filters",
          reg(REG_RXCONFIG) == RXCONFIG_VALUE);
    check("the transmit configuration carries the 9.6us gap and the burst and retry fields",
          reg(REG_TXCONFIG) == TXCONFIG_VALUE);
    check("no interrupt can be raised", (reg(REG_INTRMASK) & 0xffffu) == 0);
    check("the config registers are locked again", bar_byte(REG_CFG9346) == CFG_LOCK);
    check("the driver read the link state instead of assuming one", mock_registered->link == 0);
    /* The four transmit buffers are all handed to the card: a buffer whose address the card does not have is a
     * transmit that never leaves. */
    for (int i = 0; i < RTL_TX_SLOTS; i++)
        check("every transmit buffer address is programmed", reg((u16)(REG_TXADDR0 + i * 4)) != 0);
    /* The ring is aligned to its own size, because the card's write pointer is a 16-bit offset into it. */
    check("the receive ring is aligned to 32 KiB", (adapter.rx_phys & (RTL_RX_ALIGN - 1u)) == 0);
    check("the receive ring is below 4 GiB", adapter.rx_phys + RTL_RX_RING <= 0x100000000ull);
    check("the transmit buffers are distinct", adapter.tx_phys[0] != adapter.tx_phys[1] &&
                                                adapter.tx_phys[1] != adapter.tx_phys[2]);
}

static void test_a_8139c_plus_is_driven_in_c_mode(void)
{
    /* Revision 0x20 and later is the 8139C+.  C+ mode is opt-in through the C+ command register at 0xe0, so a
     * part left alone runs the C-mode register set and the C-mode receive buffer this driver programs - which
     * is the only way a card that reports that revision can be used at all.  What must never happen is a write
     * to 0xe0, because that is what would turn the same chip into a descriptor-ring device reading this
     * driver's ring addresses as descriptors. */
    unsigned i;

    reset_model();
    present(0x10ec, 0x8139, 0x20);
    mac_from("52:54:00:aa:bb:cc");
    check("a 8139C+ is claimed and driven in C mode", rtl8139_probe() == 1);
    check("and the stack was told about it", mock_registered != NULL);
    for (i = 0; i < 8; i++)
        check("the C+ command register is never written", mock_bar[(0xe0 / 4) + i] == 0);
}

static void test_the_card_that_never_leaves_reset_fails_loudly(void)
{
    reset_model();
    present(0x10ec, 0x8139, 0x10);
    mac_from("52:54:00:12:34:56");
    mock_reset_stuck = 1;                   /* the card never clears its own reset bit */
    check("a card that will not reset is not claimed", rtl8139_probe() == 0);
    check("the failure was logged", mock_klog_lines >= 1);
    check("and the stack was not told about an adapter", mock_registered == NULL);
}

static void test_transmit_copies_the_frame_and_rings_the_doorbell(void)
{
    uint8_t frame[64];
    int i;

    reset_model();
    present(0x10ec, 0x8139, 0x10);
    mac_from("52:54:00:12:34:56");
    if (!rtl8139_probe()) { check("probe succeeded", 0); return; }
    for (i = 0; i < (int)sizeof frame; i++) frame[i] = (uint8_t)(i + 1);
    set_reg(REG_TXSTATUS0, TX_HOST_OWNS);   /* the card has finished with slot 0 */
    check("a transmit is accepted", mock_registered->transmit(mock_registered, frame, sizeof frame) == 1);
    check("the frame went into slot 0", memcmp(adapter.tx[0], frame, sizeof frame) == 0);
    check("the length written to TxStatus is the frame's, in the same word as the configuration bits",
          reg(REG_TXSTATUS0) == (TX_FLAGS | (u32)sizeof frame));

    set_reg(REG_TXSTATUS0 + 4, TX_HOST_OWNS);
    check("the next transmit uses the next slot", mock_registered->transmit(mock_registered, frame, sizeof frame) == 1);
    check("slot 1's status carries the length", reg(REG_TXSTATUS0 + 4) == (TX_FLAGS | (u32)sizeof frame));
    check("slot 0 is untouched by the second transmit", reg(REG_TXSTATUS0) == (TX_FLAGS | (u32)sizeof frame));
}

static void test_a_short_frame_is_padded_and_a_busy_slot_is_not_rewritten(void)
{
    uint8_t frame[14];
    int i;

    reset_model();
    present(0x10ec, 0x8139, 0x10);
    mac_from("52:54:00:12:34:56");
    if (!rtl8139_probe()) { check("probe succeeded", 0); return; }
    for (i = 0; i < 14; i++) frame[i] = (uint8_t)(0x40 + i);
    set_reg(REG_TXSTATUS0, TX_HOST_OWNS);
    check("a 14-byte frame is transmitted", mock_registered->transmit(mock_registered, frame, 14) == 1);
    check("the card is told sixty bytes, because a 8139 does not pad", reg(REG_TXSTATUS0) == (TX_FLAGS | 60u));
    check("the padding is zero", adapter.tx[0][14] == 0 && adapter.tx[0][59] == 0);
    /* A descriptor the card still owns must not be reused: the frame on the wire would be a mix of two. */
    set_reg(REG_TXSTATUS0 + 4, 0);          /* slot 1 busy */
    check("a slot the card owns refuses the frame", mock_registered->transmit(mock_registered, frame, 14) == 0);
    check("the refusal is counted", adapter.tx_dropped == 1);
    check("an oversize frame is refused", mock_registered->transmit(mock_registered, frame, 4096) == 0);
    check("a runt is refused", mock_registered->transmit(mock_registered, frame, 4) == 0);
}

static void put_rx_frame(const uint8_t *payload, size_t len, uint16_t status)
{
    uint8_t *at = adapter.rx + adapter.rx_cur;
    uint16_t wire = (uint16_t)(len + 4u);   /* the length field counts the FCS */

    at[0] = (uint8_t)status;
    at[1] = (uint8_t)(status >> 8);
    at[2] = (uint8_t)wire;
    at[3] = (uint8_t)(wire >> 8);
    memcpy(at + 4, payload, len);
    at[4 + len + 0] = 0xde;                 /* the FCS the card appended */
    at[4 + len + 1] = 0xad;
    at[4 + len + 2] = 0xbe;
    at[4 + len + 3] = 0xef;
    /* The card's own write pointer (0x3a) stands where the next frame would start: past the 4-byte header,
     * the length the field declares (which includes the FCS) and the 4-byte alignment.  0x38 is the driver's
     * read offset (CAPR) and is not the model's to write. */
    set_reg16(REG_RXBUFADDR, (u16)(adapter.rx_cur + ((4u + wire + 3u) & ~3u)));
}

static void test_a_received_frame_reaches_the_stack_and_the_ring_advances(void)
{
    uint8_t frame[60];
    int i;

    reset_model();
    present(0x10ec, 0x8139, 0x10);
    mac_from("52:54:00:12:34:56");
    if (!rtl8139_probe()) { check("probe succeeded", 0); return; }
    for (i = 0; i < 60; i++) frame[i] = (uint8_t)(i * 3);
    put_rx_frame(frame, sizeof frame, RX_STATUS_OK);
    check("the poll reports a frame", mock_registered->poll(mock_registered) == 1);
    check("the stack saw exactly one frame", mock_rx_calls == 1);
    check("the frame it saw is the one in the ring, without the FCS", mock_frame_len == sizeof frame &&
          memcmp(mock_frame, frame, sizeof frame) == 0);
    check("the ring cursor moved past the frame", adapter.rx_cur == ((4u + 64u + 3u) & ~3u));
    check("an empty ring reports nothing", mock_registered->poll(mock_registered) == 0);
}

static void test_a_damaged_frame_never_reaches_the_stack(void)
{
    uint8_t frame[60];

    reset_model();
    present(0x10ec, 0x8139, 0x10);
    mac_from("52:54:00:12:34:56");
    if (!rtl8139_probe()) { check("probe succeeded", 0); return; }
    memset(frame, 0xaa, sizeof frame);
    put_rx_frame(frame, sizeof frame, (uint16_t)(RX_STATUS_CRC | RX_STATUS_OK));
    check("the poll consumes the descriptor", mock_registered->poll(mock_registered) == 1);
    check("the stack never saw the frame", mock_rx_calls == 0);
    check("the damage was counted", adapter.rx_errors == 1);
    /* A receiver that has been restarted begins writing at the start of the ring again. */
    adapter.rx_cur = 0;
    set_reg16(REG_RXBUFADDR, 0);
    put_rx_frame(frame, sizeof frame, 0);   /* not the "OK" bit at all */
    (void)mock_registered->poll(mock_registered);
    check("a header without the OK bit is an error too", adapter.rx_errors == 2);
    check("and still nothing reached the stack", mock_rx_calls == 0);
}

static void test_the_link_is_read_from_the_phy_registers(void)
{
    reset_model();
    present(0x10ec, 0x8139, 0x10);
    mac_from("52:54:00:12:34:56");
    mock_bar[REG_BASEMODESTATUS / 4] = BMSR_LINK;
    mock_bar[REG_NWAYLPAR / 4] = 0x42e1;    /* LPA bits 9:8 = 10b: 100 Mb/s */
    if (!rtl8139_probe()) { check("probe succeeded", 0); return; }
    check("a link is reported", mock_registered->link == 1);
    check("the speed is read from the peer's advertisement", mock_registered->speed_mbps == 100);
    mock_bar[REG_NWAYLPAR / 4] = 0x41e1;    /* bits 9:8 = 01b: 10 Mb/s */
    (void)mock_registered->poll(mock_registered);
    check("10 Mb/s links are reported as 10", mock_registered->speed_mbps == 10);
    mock_bar[REG_BASEMODESTATUS / 4] = 0;
    (void)mock_registered->poll(mock_registered);
    check("a pulled cable takes the link down", mock_registered->link == 0);
    check("and the speed with it", mock_registered->speed_mbps == 0);
}

static void test_a_second_probe_does_not_reinitialize_the_card(void)
{
    reset_model();
    present(0x10ec, 0x8139, 0x10);
    mac_from("52:54:00:12:34:56");
    check("the first probe claims it", rtl8139_probe() == 1);
    set_reg(REG_RXCONFIG, 0xdeadbeefu);     /* evidence that the registers are not written again */
    check("the second probe answers from what it already did", rtl8139_probe() == 1);
    check("the card was not reprogrammed", reg(REG_RXCONFIG) == 0xdeadbeefu);
}

#else   /* SCOS_TEST_RTL8168 */

/* ---------------------------------------------------------------- RTL8168 / 8169 --------------------------------------------- */

static void phy_answer(u16 value) { mock_phy_bmcr = value; }

static void test_absent_card_claims_nothing(void)
{
    reset_model();
    check("no PCI function at all is not an adapter", rtl8168_probe() == 0);
    check("the stack was told nothing", mock_registered == NULL);
}

static void test_an_8168_is_programmed_into_descriptor_rings(void)
{
    reset_model();
    present(0x10ec, 0x8168, 0x06);
    mac_from("52:54:00:aa:00:01");
    phy_answer(0x7949);                     /* BMCR: autoneg able and enabled, as a PHY reports it */
    check("the 8168 is claimed", rtl8168_probe() == 1);
    check("an adapter reached the stack", mock_registered != NULL && mock_registered_count == 1);
    check("the adapter is named for its driver", mock_registered && strcmp(mock_registered->name, "rtl8168") == 0);
    if (!mock_registered) { fflush(stdout); return; }
    check("the station address is the one in the IDR registers",
          memcmp(mock_registered->mac, (const uint8_t[]){ 0x52, 0x54, 0x00, 0xaa, 0x00, 0x01 }, 6) == 0);
    check_bus_mastering("memory decode and bus mastering were enabled on the card's function");
    check("both descriptor rings were given to the card",
          reg(REG_TXDESCLO) == (u32)adapter.tx_ring_phys && reg(REG_RXDESCLO) == (u32)adapter.rx_ring_phys);
    check("the descriptor addresses are below 4 GiB",
          adapter.tx_ring_phys + sizeof(struct tx_desc) * RTL_TX_RING <= 0x100000000ull);
    check("the transceiver was enabled", bar_byte(REG_CHIPCMD) == (CMD_RXENB | CMD_TXENB));
    check("the receive configuration is the 128-byte threshold with the burst", reg(REG_RXCONFIG) == RXCONFIG_VALUE);
    check("the transmit configuration has the shortest gap and unlimited burst", reg(REG_TXCONFIG) == TXCONFIG_VALUE);
    check("the largest transmit packet is programmed", bar_byte(REG_MAXTXPKTSIZE) == TX_MAX_PACKET);
    check("no offload was asked for", (reg(REG_CPLUSCMD) & 0xffffu) == 0);
    check("interrupt coalescing is off, because nothing is interrupt driven", (reg(REG_INTRMITIGATE) & 0xffffu) == 0);
    check("no interrupt can be raised", (reg(REG_INTRMASK) & 0xffffu) == 0);
    check("the config registers are locked again", bar_byte(REG_CFG9346) == CFG_LOCK);
    check("autonegotiation was restarted through the PHY mailbox",
          (reg(REG_PHYAR) & (BMCR_ANENABLE | BMCR_ANRESTART)) == (BMCR_ANENABLE | BMCR_ANRESTART));
    check("the receive ring is inside the one buffer extent the driver allocated",
          adapter.rx_buf_phys == (u64)(uintptr_t)adapter.rx_buf);
}

static void test_every_receive_descriptor_is_armed_for_the_card(void)
{
    int i, armed = 0, ring_end = 0, addresses_ok = 1;

    reset_model();
    present(0x10ec, 0x8168, 0x06);
    mac_from("52:54:00:aa:00:01");
    phy_answer(0x7949);
    if (!rtl8168_probe()) { check("probe succeeded", 0); return; }
    for (i = 0; i < RTL_RX_RING; i++) {
        struct rx_desc *desc = &adapter.rx_ring[i];
        if (desc->opts1 & DESC_OWN) armed++;
        if (desc->opts1 & DESC_RING_END) ring_end++;
        if ((desc->opts1 & RX_STAT_LEN_MASK) != RTL_RX_BUF) addresses_ok = 0;
        if (desc->addr_lo != (u32)(adapter.rx_buf_phys + (u64)i * RTL_RX_BUF)) addresses_ok = 0;
        if (desc->addr_hi || desc->opts2) addresses_ok = 0;
    }
    check("every receive descriptor is owned by the card", armed == RTL_RX_RING);
    check("exactly the last one carries RingEnd, which is how the card wraps", ring_end == 1);
    check("each descriptor points at its own buffer with the right size", addresses_ok);
}

static void test_transmit_writes_a_descriptor_and_rings_the_doorbell(void)
{
    uint8_t frame[70];
    int i;

    reset_model();
    present(0x10ec, 0x8168, 0x06);
    mac_from("52:54:00:aa:00:01");
    phy_answer(0x7949);
    if (!rtl8168_probe()) { check("probe succeeded", 0); return; }
    set_reg(REG_TXPOLL, 0);
    for (i = 0; i < (int)sizeof frame; i++) frame[i] = (uint8_t)(i + 7);
    check("a transmit is accepted", mock_registered->transmit(mock_registered, frame, sizeof frame) == 1);
    check("the descriptor carries the frame's length", (adapter.tx_ring[0].opts1 & 0xffffu) == sizeof frame);
    check("the descriptor is owned by the card and is both the first and last fragment",
          (adapter.tx_ring[0].opts1 & (DESC_OWN | DESC_FIRST_FRAG | DESC_LAST_FRAG)) ==
          (DESC_OWN | DESC_FIRST_FRAG | DESC_LAST_FRAG));
    check("the descriptor points at the copied frame", adapter.tx_ring[0].addr_lo == (u32)adapter.tx_buf_phys[0]);
    check("the frame was copied into that buffer", memcmp(adapter.tx_buf[0], frame, sizeof frame) == 0);
    check("the doorbell was rung", (bar_byte(REG_TXPOLL) & 0x40u) == 0x40u);
    check("no checksum or VLAN offload was requested", adapter.tx_ring[0].opts2 == 0);
    /* A descriptor the card still owns is not rewritten. */
    check("a second transmit goes to the next descriptor", mock_registered->transmit(mock_registered, frame, sizeof frame) == 1);
    check("it is descriptor 1", adapter.tx_ring[1].opts1 & DESC_OWN);
    /* Short frames: the card pads to the Ethernet minimum, so the length says 60. */
    adapter.tx_ring[2].opts1 = 0;
    check("a 20-byte frame is accepted", mock_registered->transmit(mock_registered, frame, 20) == 1);
    check("and asks for the 60-byte minimum", (adapter.tx_ring[2].opts1 & 0xffffu) == 60);
    check("the padding is zero", adapter.tx_buf[2][20] == 0 && adapter.tx_buf[2][59] == 0);
    adapter.tx_ring[3].opts1 = DESC_OWN;
    check("a descriptor the card owns refuses the frame", mock_registered->transmit(mock_registered, frame, 20) == 0);
    check("the refusal is counted", adapter.tx_dropped == 1);
    check("an oversize frame is refused", mock_registered->transmit(mock_registered, frame, 4096) == 0);
}

static void test_a_received_frame_reaches_the_stack_and_the_descriptor_is_rearmed(void)
{
    uint8_t *buffer;
    int i;

    reset_model();
    present(0x10ec, 0x8168, 0x06);
    mac_from("52:54:00:aa:00:01");
    phy_answer(0x7949);
    if (!rtl8168_probe()) { check("probe succeeded", 0); return; }
    buffer = adapter.rx_buf;
    for (i = 0; i < 100; i++) buffer[i] = (uint8_t)(i ^ 0x5a);
    adapter.rx_ring[0].opts1 = 100;         /* the card cleared the ownership bit and wrote the length */
    check("the poll reports a frame", mock_registered->poll(mock_registered) == 1);
    check("the stack saw exactly one frame", mock_rx_calls == 1);
    check("it saw the frame and its length", mock_frame_len == 100 && memcmp(mock_frame, buffer, 100) == 0);
    check("the descriptor was handed back to the card", adapter.rx_ring[0].opts1 & DESC_OWN);
    check("the ring cursor advanced", adapter.rx_cur == 1);
    check("the next poll finds nothing", mock_registered->poll(mock_registered) == 0);
}

static void test_a_damaged_frame_never_reaches_the_stack(void)
{
    reset_model();
    present(0x10ec, 0x8168, 0x06);
    mac_from("52:54:00:aa:00:01");
    phy_answer(0x7949);
    if (!rtl8168_probe()) { check("probe succeeded", 0); return; }
    adapter.rx_ring[0].opts1 = RX_STAT_RES | RX_STAT_CRC | 64u;
    check("the poll consumes the descriptor", mock_registered->poll(mock_registered) == 1);
    check("the stack never saw it", mock_rx_calls == 0);
    check("the damage was counted", adapter.rx_errors == 1);
    check("the descriptor was re-armed anyway", adapter.rx_ring[0].opts1 & DESC_OWN);
    /* A run of zeroes is not a 4-byte frame: the length check has to refuse it rather than hand the stack a
     * header nobody wrote. */
    adapter.rx_ring[1].opts1 = 3;
    check("a three-byte 'frame' is refused", mock_registered->poll(mock_registered) == 1);
    check("as a receive error", adapter.rx_errors == 2);
    check("and still nothing reached the stack", mock_rx_calls == 0);
}

static void test_the_link_is_read_from_phy_status(void)
{
    reset_model();
    present(0x10ec, 0x8168, 0x06);
    mac_from("52:54:00:aa:00:01");
    phy_answer(0x7949);
    set_reg(REG_PHYSTATUS, PHY_LINK | PHY_1000MBPS | PHY_FULL_DUPLEX);
    if (!rtl8168_probe()) { check("probe succeeded", 0); return; }
    check("a gigabit link is reported", mock_registered->link == 1 && mock_registered->speed_mbps == 1000);
    set_reg(REG_PHYSTATUS, PHY_LINK | PHY_100MBPS);
    (void)mock_registered->poll(mock_registered);
    check("a 100 Mb/s link is reported as 100", mock_registered->speed_mbps == 100);
    set_reg(REG_PHYSTATUS, PHY_LINK);
    (void)mock_registered->poll(mock_registered);
    check("a link with no speed bit is 10 Mb/s, the part's slowest", mock_registered->speed_mbps == 10);
    set_reg(REG_PHYSTATUS, 0);
    (void)mock_registered->poll(mock_registered);
    check("a pulled cable takes the link down", mock_registered->link == 0 && mock_registered->speed_mbps == 0);
}

static void test_another_vendor_is_not_claimed(void)
{
    reset_model();
    present(0x8086, 0x8168, 0x06);          /* same device number, someone else's vendor */
    mac_from("52:54:00:aa:00:01");
    phy_answer(0x7949);
    check("a non-Realtek function is left alone", rtl8168_probe() == 0);
    check("and nothing reached the stack", mock_registered == NULL);
}

static void test_an_unknown_realtek_device_is_left_alone(void)
{
    reset_model();
    present(0x10ec, 0x8125, 0x01);          /* 2.5 Gb/s: a different descriptor format entirely */
    mac_from("52:54:00:aa:00:01");
    phy_answer(0x7949);
    check("the 8125 is not driven from this file", rtl8168_probe() == 0);
    check("and nothing reached the stack", mock_registered == NULL);
}

#endif

int main(void)
{
#if defined(SCOS_TEST_RTL8139)
    test_absent_card_claims_nothing();
    test_a_8139_is_programmed_from_reset_to_receiving();
    test_a_8139c_plus_is_driven_in_c_mode();
    test_the_card_that_never_leaves_reset_fails_loudly();
    test_transmit_copies_the_frame_and_rings_the_doorbell();
    test_a_short_frame_is_padded_and_a_busy_slot_is_not_rewritten();
    test_a_received_frame_reaches_the_stack_and_the_ring_advances();
    test_a_damaged_frame_never_reaches_the_stack();
    test_the_link_is_read_from_the_phy_registers();
    test_a_second_probe_does_not_reinitialize_the_card();
#else
    test_absent_card_claims_nothing();
    test_an_8168_is_programmed_into_descriptor_rings();
    test_every_receive_descriptor_is_armed_for_the_card();
    test_transmit_writes_a_descriptor_and_rings_the_doorbell();
    test_a_received_frame_reaches_the_stack_and_the_descriptor_is_rearmed();
    test_a_damaged_frame_never_reaches_the_stack();
    test_the_link_is_read_from_phy_status();
    test_another_vendor_is_not_claimed();
    test_an_unknown_realtek_device_is_left_alone();
#endif
    printf("%d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}

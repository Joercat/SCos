/* The card's own security processor: what this OS can ask it, and what it answers.
 *
 * Everything on the path to drawing with this GPU goes through one microcontroller.  On this generation the
 * host is not allowed to bring the chip up itself - the register block that owns the coprocessor answers a
 * cold reader with 0xbadf4100, a refusal the vendor's own driver names and waits on - and the thing that
 * clears that lock is the Flight Security Processor, the FSP.  The FSP has a mailbox of its own, reachable
 * before anything is unlocked, and the vendor driver uses it to hand over the firmware that boots the
 * coprocessor.  The mailbox is therefore the door, and this file is the first thing this OS has ever put
 * through it.
 *
 * What happens here, in order: the FSP's boot-complete scratch is read; if the FSP says it is up, the mailbox
 * queue registers are read; if those answer, a capability query - a read-only request the vendor driver
 * itself sends at exactly this stage of boot - is framed, written into the channel, and its reply read back.
 * Then, if the coprocessor's firmware was staged on the boot volume and this kernel parsed it, the handover
 * payload that would boot the GSP is assembled and checked, and the image the FSP is pointed at is copied
 * into pages this kernel owns, so the address given to a microcontroller is one nothing else can move or
 * edit while it is being read.
 *
 * What happens last: the handover.  The FMC image this OS was given is staged in pages of its own, described
 * by an 860-byte payload, and - only if the FSP answered a read-only query first, cleanly - put in the channel
 * and rung.  A card asked to boot a firmware image either does or refuses, and both are answers, so the two
 * words that would change either way are re-read after the attempt and reported beside the values they had
 * before it.  The registers this file writes are the channel's offset, its data window and the four queue
 * pointers, and the report counts them instead of leaving that to be inferred.
 *
 * Provenance, in full, because this kernel is its own and not a copy of anything: the offsets, field positions
 * and message framing are NVIDIA's, from `src/common/inc/swref/published/hopper/gh100/dev_fsp_pri.h` (the
 * mailbox, which this generation's code reaches through the same hardware-abstraction layer - GB100 publishes
 * only the scratch and MNOC registers of its own), `blackwell/gb100/dev_fsp_pri.h` and
 * `dev_fsp_addendum.h` (the scratch group, the FSP's firmware version and its fuse-check word, published in
 * absolute terms for this generation), `blackwell/gb100/dev_therm.h` with its addendum (the boot-complete
 * scratch), `hopper/gh100/dev_falcon_v4.h` (the coprocessor capability word this OS has always read as
 * refused), and the transport from `src/nvidia/arch/nvalloc/common/inc/mctp_format.h`,
 * `.../fsp/fsp_emem_channels.h`, `.../fsp/fsp_caps_query_rpc.h`, `inc/kernel/gpu/fsp/kern_fsp_cot_payload.h`
 * and the behaviour of `src/nvidia/src/kernel/gpu/fsp/{kern_fsp.c,arch/hopper/kern_fsp_gh100.c,
 * arch/blackwell/kern_fsp_gb100.c}`.  All MIT-licensed, fetched 2026-10-26.  The framing is rebuilt here
 * rather than copied: the vendor's version runs on a kernel with threads, timeouts and an error model, and
 * this one has a bounded loop and a refusal, which is a different shape of code with the same bytes on the
 * wire - and every byte of those bytes is pinned by tools/tests/gpu_fsp_host.c against a model of the
 * mailbox built from the published semantics, not against this file's own opinion of itself.
 */

#include "gpu.h"
#include "scos.h"
#include "boot.h"
#include "kernel.h"

#define FSP_LOG_PREFIX "gpu: fsp "

/* The FSP's internal memory has a 1 KiB channel reserved for the resource manager.  The host and the FSP each
 * own a head/tail pair around it: writing a tail *is* the request, so there is no command register, no
 * interrupt and no doorbell beyond these four pointers. */
#define NV_PFSP_EMEMC                  0x008f2ac0u  /* OFFS 7:2, BLK 15:8, AINCW bit 24, AINCR bit 25 */
#define NV_PFSP_EMEMD                  0x008f2ac4u  /* the window: every access moves the offset by one */
#define NV_PFSP_QUEUE_HEAD             0x008f2c00u  /* how far the FSP has read of what the host wrote */
#define NV_PFSP_QUEUE_TAIL             0x008f2c04u  /* how far the host has written */
#define NV_PFSP_MSGQ_HEAD              0x008f2c80u  /* the reply queue, the same arrangement the other way */
#define NV_PFSP_MSGQ_TAIL              0x008f2c84u
#define NV_THERM_FSP_BOOT_COMPLETE     0x000200bcu  /* 0xFF once the FSP has finished booting itself */
#define NV_PFSP_UCODE_VERSION          0x008f0334u  /* SCRATCH_GROUP_3(1), 11:0 */
#define NV_PFSP_FUSE_ERROR_CHECK       0x008f033cu  /* SCRATCH_GROUP_3(3), 0 = passed */
#define NV_PGSP_HWCFG2                 0x001100f4u  /* NV_PGSP base 0x110000 + NV_PFALCON_FALCON_HWCFG2 */
#define NV_PGSP_MAILBOX0               0x00110040u  /* NV_PFALCON_FALCON_MAILBOX0: the FMC's own report */
#define NV_PGSP_MAILBOX1               0x00110044u  /* and its partner: together they hold a 64-bit address */
#define FSP_HWCFG2_LOCKDOWN_BIT                  13u /* NV_PFALCON_FALCON_HWCFG2_RISCV_BR_PRIV_LOCKDOWN 13:13,
                                                       * LOCK=1 UNLOCK=0, from nvhw/ref/gh100/dev_falcon_v4.h */
#define FSP_BOOT_COMPLETE_SUCCESS    0x000000ffu    /* NV_THERM_I2CS_SCRATCH_FSP_BOOT_COMPLETE_STATUS_SUCCESS:
                                                       * a 32-bit compare, not a low-byte one, and FAILED is 0 */
#define FSP_BOOT_COMPLETE_FAILED     0x00000000u
#define FSP_PMC_BOOT_0                        0x0u  /* the first dword of the aperture, inside the VF window */
#define FSP_EMEM_CHANNEL_BYTES            1024u
#define FSP_EMEM_DWORDS_PER_BLOCK           64u      /* so an offset is a block number and a dword within it */
#define FSP_REFUSED(v)                   (((v) & 0xffffff00u) == 0xbadf4100u)
/* Any value in the 0xBADF.... family, of which the line above is the one cause.  The coprocessor's driver
 * masks a read against FLCN_CSB_PRIV_PRI_ERROR_CODE 0xBADF0000 under 0xFFFF0000 before it believes a value
 * (flcnretval.h), because the family is what the bus returns when a read could not be served at all; only
 * one member of it is the code it waits on the FSP to clear.  This file uses FSP_REFUSED for that one and
 * this test everywhere a value could otherwise be read as content by accident. */
#define FSP_NOT_CONTENT(v)               (((v) & 0xffff0000u) == 0xbadf0000u)
#define FSP_HEADER_BYTES                    8u      /* the MCTP transport dword and the NVDM dword */
#define FSP_WAIT_REPLY_MS                1500u       /* the reference driver gives a reply `int time = 1000'
                                                       * iterations of usleep_range(1000, 2000) - about a
                                                       * second and a half - and four seconds to the FSP's own
                                                       * boot (`unsigned timeout_ms = 4000' in wait_secure_boot
                                                       * and in the lockdown wait).  Those are the numbers a
                                                       * chip that works is timed against; a bound expressed as
                                                       * a count of uncached reads comes out tens of times too
                                                       * small, and prints "silent" about a device that was
                                                       * still on its way. */
#define FSP_WAIT_BOOT_MS                 4000u
#define FSP_POLL_LIMIT                 200000u       /* looks at a pair of registers, and on the reply wait a
                                                         * TSC deadline on top of them: an MMIO read dominates
                                                         * either bound, so both are counts of work rather
                                                         * than promises about time, and the panel prints how
                                                         * many looks each wait took */
#define FSP_BOOT_POLL_LIMIT            200000u       /* looks at one register for the FSP to report itself
                                                         * booted: tens of milliseconds on a modern core, and
                                                         * bounded by the device's own latency rather than by
                                                         * a clock this early in a boot cannot read */
#define FSP_IMAGE_MAX            (8u * 1024u * 1024u)

#define NVDM_TYPE_COT                    0x14u
#define NVDM_TYPE_FSP_RESPONSE           0x15u
#define NVDM_TYPE_CAPS_QUERY             0x16u
#define NVDM_TYPE_CLOCK_BOOST            0x20u      /* the capability asked about, which is what the vendor
                                                       * driver asks about before it has done anything else */
#define MCTP_VERSION                       1u
#define MCTP_TYPE_VENDOR_PCI            0x7eu
#define MCTP_VENDOR_ID_NVIDIA         0x10deu

/* NVDM_PAYLOAD_COT, packed, in the vendor's field order: two u16s, four offsets and sizes, then the three
 * authentication blobs, then the boot-arguments pointer.  Written as offsets rather than a struct because
 * the payload has to land in the channel byte for byte, and a struct would let a compiler disagree about the
 * padding in a way nobody would notice until a card refused to boot.  COT_SIZE is the sum: the test asserts
 * each field lands where the published layout puts it. */
#define COT_SIZE              GPU_FSP_PAYLOAD_BYTES   /* the published payload size, from gpu.h, so the
                                                          * buffer this is built in cannot be a different
                                                          * size from the record the panel reports */
#define COT_OFF_VERSION                     0u
#define COT_OFF_SIZE                        2u
#define COT_OFF_FMC_ADDRESS                 4u      /* physical, page aligned: the image, not the container */
#define COT_OFF_FRTS_SYSMEM                12u
#define COT_OFF_FRTS_SYSMEM_SIZE           20u
#define COT_OFF_FRTS_VIDMEM                24u      /* an offset back from the *end* of the frame buffer */
#define COT_OFF_FRTS_VIDMEM_SIZE           32u
#define COT_OFF_HASH                       36u      /* 48 bytes: SHA-384, exactly the FMC's hash section */
#define COT_OFF_PUBLICKEY                  84u      /* 384 */
#define COT_OFF_SIGNATURE                 468u      /* 384 */
#define COT_OFF_BOOT_ARGS                 852u      /* (uint64_t)-1 on a cold boot: nothing to reuse */
#define COT_PAYLOAD_VERSION                 2u      /* the version this chip family's HAL sets */

/* One mapping, its base inside BAR0, and how long it is.  All three of the file's register accesses go
 * through a window, and the length is what makes an access to a register the window does not contain an
 * answer of "not there" rather than a read of whatever the next device's registers are: the mailbox page this
 * OS asks for is 16 KiB, because the queue pointers sit 11 KiB into it, and a check hardcoded to one page
 * would silently drop every write to them. */
struct fsp_window {
    volatile uint32_t *page;
    uint64_t base, length;
};

static struct gpu_fsp_state fsp_state;
/* The timer block and the virtual-function page.  Both are published for generations before Blackwell and both
 * answer a question this OS cannot ask anywhere else: `NV_PTIMER_TIME_1'/_0 at 0x9410/0x9400 is the GPU's own
 * clock read the way NVIDIA's driver reads it (high, low, high again, repeating if the high half moved, because
 * a low-half carry between two reads could otherwise be taken for a time in the future), and the page at
 * BAR0+0x0000..0x2FFFF is `NV_VIRTUAL_FUNCTION_PRIV' in blackwell/gb100/dev_vm.h - the region whose doorbell,
 * mailbox scratch words and interrupt tree the host is allowed to reach while the coprocessor holds everything
 * else.  The legacy user-mode page at 0x810000 is the one NVIDIA's own source calls a thing "going away long
 * term" from Turing onwards, so a page there that shows nothing says nothing about this card; these two do. */
#define NV_VF_PAGE_BASE            0x0000u
#define NV_VF_PAGE_BYTES           0x3000u   /* enough for the four words read below, not the whole 192 KiB */
#define NV_VF_INTR_LEAF0           0x1000u   /* NV_VIRTUAL_FUNCTION_PRIV_CPU_INTR_LEAF(0) */
#define NV_VF_INTR_TOP0            0x1600u   /* ..._CPU_INTR_TOP(0) */
#define NV_VF_SCRATCH0             0x2100u   /* ..._PRIV_MAILBOX_SCRATCH(i), sixteen of them */
#define NV_VF_DOORBELL             0x2200u   /* ..._PRIV_DOORBELL, -W-4R: named here, never read or written */
#define NV_TIMER_PAGE_BASE         0x9000u
#define NV_TIMER_PAGE_BYTES        0x1000u
#define NV_TIMER_TIME_0            0x9400u   /* NV_PTIMER_TIME_0, absolute like every name here */
#define NV_TIMER_TIME_1            0x9410u   /* NV_PTIMER_TIME_1: read first and last, per the ref */
#define NV_TIMER_NSEC_STEP         32u       /* the field is NSEC 31:5, so one step is 32 ns */

static struct fsp_window w_therm, w_fsp, w_gsp, w_vf, w_timer;

const struct gpu_fsp_state *gpu_fsp_state(void)
{
    return fsp_state.present || fsp_state.attempted ? &fsp_state : 0;
}

static uint32_t load32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void store32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (i * 8));
}
static void store64(uint8_t *p, uint64_t v) { store32(p, (uint32_t)v); store32(p + 4, (uint32_t)(v >> 32)); }

static uint64_t fsp_ticks(void)
{
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return (uint64_t)high << 32 | low;
}

static uint32_t read32(const struct fsp_window *w, uint32_t offset)
{
    uint64_t within;
    uint32_t value;
    if (!w->page || offset < w->base) return 0xffffffffu;
    within = offset - w->base;
    if (within + 4 > w->length) return 0xffffffffu;      /* outside the mapping this window is: no guess */
    value = gpu_io_read32(w->page, within);
    fsp_state.reads++;
    if (FSP_NOT_CONTENT(value)) fsp_state.refusal_reads++;   /* the whole family: 0xbadf4100 and 0xbadf1100 */
    return value;
}

static void write32(struct fsp_window *w, uint32_t offset, uint32_t value)
{
    uint64_t within;
    if (!w->page || offset < w->base) return;
    within = offset - w->base;
    if (within + 4 > w->length) return;
    gpu_io_write32(w->page, within, value);
    fsp_state.writes++;
}

/* The FSP's registers sit 14 MiB into BAR0 and the therm scratch 128 KiB in, so each is reached through its
 * own window rather than by asking the kernel for a 14 MiB mapping it would refuse.  The FSP's window is four
 * pages, because its queue pointers are 11 KiB past its scratch registers; the other two are one page each.
 * device_map reuses leaves that already exist, so mapping a page a driver module also maps is a shared window,
 * not a second set of attributes on the same memory - which is why none of these ask for write combining. */
static void map_mailbox_pages(uint64_t bar0)
{
    /* Cleared before anything is asked for: a second note in one boot would otherwise leave a window pointing
     * at the mapping the first one took, and reads of a device that refused to be mapped again would be served
     * out of memory this kernel mapped an hour of uptime ago.  The host model found this. */
    memset(&w_therm, 0, sizeof w_therm);
    memset(&w_fsp, 0, sizeof w_fsp);
    memset(&w_gsp, 0, sizeof w_gsp);
    memset(&w_vf, 0, sizeof w_vf);
    memset(&w_timer, 0, sizeof w_timer);
    uint64_t mapped;
    void *page;

    mapped = 0;
    page = device_map(bar0 + 0x20000u, 0x1000u, 0, &mapped);
    if (page && mapped >= 0x1000u) { w_therm.page = page; w_therm.base = 0x20000u; w_therm.length = mapped; }
    mapped = 0;
    page = device_map(bar0 + 0x8f0000u, 0x4000u, 0, &mapped);
    if (page && mapped >= 0x4000u) { w_fsp.page = page; w_fsp.base = 0x8f0000u; w_fsp.length = mapped; }
    mapped = 0;
    page = device_map(bar0 + 0x110000u, 0x1000u, 0, &mapped);
    if (page && mapped >= 0x1000u) { w_gsp.page = page; w_gsp.base = 0x110000u; w_gsp.length = mapped; }
    page = device_map(bar0 + NV_VF_PAGE_BASE, NV_VF_PAGE_BYTES, 0, &mapped);
    if (page && mapped >= NV_VF_PAGE_BYTES) { w_vf.page = page; w_vf.base = NV_VF_PAGE_BASE; w_vf.length = mapped; }
    page = device_map(bar0 + NV_TIMER_PAGE_BASE, NV_TIMER_PAGE_BYTES, 0, &mapped);
    if (page && mapped >= NV_TIMER_PAGE_BYTES) {
        w_timer.page = page; w_timer.base = NV_TIMER_PAGE_BASE; w_timer.length = mapped;
    }
    /* Two windows of the same aperture, both read-only in this file, both named by a published
     * Blackwell header: see the defines above for why these two and not the legacy page. */
    /* Two of the five decide whether anything below runs at all; the other three are recorded as reachable or
     * not, because a reader needs the difference between "the chip refused" and "this kernel could not get a
     * window there". */
    fsp_state.vf_readable = w_vf.page != 0;
    fsp_state.timer_readable = w_timer.page != 0;
    fsp_state.mapped = w_therm.page && w_fsp.page;
    /* The GSP page is a bonus rather than a requirement: it is the page that has answered 0xbadf4100 to
     * every reader so far, and the first thing that changes if a boot ever succeeds is that it stops.  Not
     * having it mapped says nothing about the mailbox, so nothing below depends on it. */
}

static uint32_t emem_offset_dwords(void)
{
    uint32_t v = read32(&w_fsp, NV_PFSP_EMEMC);
    return ((v >> 2) & 0x3fu) + ((v >> 8) & 0xffu) * FSP_EMEM_DWORDS_PER_BLOCK;
}

static void emem_seek(uint32_t dwords, int auto_write, int auto_read)
{
    uint32_t v = ((dwords % FSP_EMEM_DWORDS_PER_BLOCK) << 2) |
                 ((dwords / FSP_EMEM_DWORDS_PER_BLOCK) << 8);
    if (auto_write) v |= 1u << 24;
    if (auto_read) v |= 1u << 25;
    write32(&w_fsp, NV_PFSP_EMEMC, v);
}

/* All four pointers, in one place, because "may I send" is the same question as "is there a reply": the FSP
 * sets head to tail on both queues as it consumes and as it produces. */
static int queues(int *idle)
{
    fsp_state.queue_head = read32(&w_fsp, NV_PFSP_QUEUE_HEAD);
    fsp_state.queue_tail = read32(&w_fsp, NV_PFSP_QUEUE_TAIL);
    fsp_state.msgq_head = read32(&w_fsp, NV_PFSP_MSGQ_HEAD);
    fsp_state.msgq_tail = read32(&w_fsp, NV_PFSP_MSGQ_TAIL);
    if (FSP_REFUSED(fsp_state.queue_head) || FSP_REFUSED(fsp_state.queue_tail) ||
        FSP_REFUSED(fsp_state.msgq_head) || FSP_REFUSED(fsp_state.msgq_tail)) {
        *idle = -1;
        return 0;                                      /* refused: the mailbox is not there to be used */
    }
    *idle = fsp_state.queue_head == fsp_state.queue_tail && fsp_state.msgq_head == fsp_state.msgq_tail;
    return 1;
}

static int reply_waiting(void)
{
    uint32_t head = read32(&w_fsp, NV_PFSP_MSGQ_HEAD);
    uint32_t tail = read32(&w_fsp, NV_PFSP_MSGQ_TAIL);
    if (FSP_REFUSED(head) || FSP_REFUSED(tail)) return 0;
    if (head == tail) return 0;
    fsp_state.msgq_head = head;
    fsp_state.msgq_tail = tail;
    return 1;
}

/* The reference driver waits up to four seconds at the point it has just trained the falcon.  This OS trains
 * nothing - it never resets a chip, so on a machine where the FSP is going to come up it already has - and so
 * waits for the order of tens of milliseconds, bounded by the count of reads below and by the cycle counter,
 * whichever expires first.  Both counts are reported, because "it did not answer in time" is only a fact once
 * a reader can see how much time that was. */
static int read_ptimer(uint32_t *nanoseconds);

/* The PTIMER is the only clock in this OS that the device itself is keeping, and NV_PTIMER counts 32 ns
 * steps, so it is what the cycle counter gets timed against once here: without that ratio a wait can be
 * counted in reads but not promised in milliseconds, and a bound nobody can state is a bound nobody can
 * check afterwards.  `wait_clocked' says which of the two happened. */
static void measure_tick_rate(void)
{
    uint32_t first = 0, last = 0;
    uint64_t t0 = fsp_ticks(), t1;
    uint64_t ns;

    if (!read_ptimer(&first)) return;
    for (volatile int spin = 0; spin < 8192; spin++) __asm__ volatile("pause");
    if (!read_ptimer(&last)) return;
    t1 = fsp_ticks();
    if (last == first || t1 <= t0) return;
    ns = (uint64_t)(uint32_t)(last - first) * 32ull;
    if (!ns) return;
    fsp_state.ticks_per_ms = (uint32_t)(((t1 - t0) * 1000000ull) / ns / 1000ull);
    fsp_state.wait_clocked = fsp_state.ticks_per_ms != 0;
}

static uint64_t waited_ms(uint64_t start)
{
    if (!fsp_state.ticks_per_ms) return 0;
    return (fsp_ticks() - start) / fsp_state.ticks_per_ms;
}

static int wait_for_timed(int (*condition)(void), int *count, uint32_t budget_ms, uint64_t *ms_used)
{
    uint64_t start = fsp_ticks();
    uint64_t deadline = fsp_state.ticks_per_ms
        ? start + (uint64_t)budget_ms * fsp_state.ticks_per_ms : 0;
    int hit = 0;

    for (unsigned i = 0; i < FSP_POLL_LIMIT; i++) {
        (*count)++;                                     /* every wait says how long it took, because "it did not
                                                           * answer" is only a fact once the reader can see what
                                                           * was waited on and for how long; `polled'
                                                           * above for the send, because they are two different
                                                           * waits and one number would be either vague or wrong */
        if (condition()) { hit = 1; break; }
        if (deadline && fsp_ticks() > deadline) break;
        if (!deadline && i >= FSP_POLL_LIMIT / 4u) break;   /* no clock: the read count is the bound, and the
                                                                * panel says the wait was bounded that way */
        for (volatile int spin = 0; spin < 32; spin++) __asm__ volatile("pause");
    }
    if (ms_used) *ms_used = waited_ms(start);
    return hit;
}

static int wait_for(int (*condition)(void), int *count)
{
    fsp_state.reply_ms = FSP_WAIT_REPLY_MS;       /* the budget the wait was given, printed beside the time it
                                                     * took: "it did not answer" is only a measurement once a
                                                     * reader can see what it was waited against */
    return wait_for_timed(condition, count, FSP_WAIT_REPLY_MS, &fsp_state.reply_ms_used);
}

/* The reference driver's own readiness test (gh100_gsp_lockdown_released): a pair of all-ones or a refusal is
 * no answer, the low byte of the therm scratch is compared as a whole dword against SUCCESS, and the
 * coprocessor is reachable once bit 13 of its capability word reads UNLOCK. */
static int boot_ready(void)
{
    fsp_state.boot_scratch = read32(&w_therm, NV_THERM_FSP_BOOT_COMPLETE);
    /* Any 0xbadf.... answer is an answer, not a state to wait out.  Measured on a GB207: the FSP scratch
     * window replies 0xbadf1100 - a different member of the family from the 0xbadf4100 that names a locked
     * PRIV target - and waiting on it burned the whole 4000 ms budget (the panel read "waited 4056 of 4000 ms")
     * for an event this reader can never see.  The reference driver stops on a refusal and reports it as an
     * error, which is the only honest thing to do with a window that says it will not be decoded. */
    if (FSP_NOT_CONTENT(fsp_state.boot_scratch) || fsp_state.boot_scratch == 0xffffffffu) return 1;
    return fsp_state.boot_scratch == FSP_BOOT_COMPLETE_SUCCESS;
}

static int lockdown_now(void)
{
    uint32_t hwcfg2;

    if (!w_gsp.page) return -1;
    hwcfg2 = read32(&w_gsp, NV_PGSP_HWCFG2);
    fsp_state.hwcfg2 = hwcfg2;
    fsp_state.gsp_readable = !FSP_REFUSED(hwcfg2) && !FSP_NOT_CONTENT(hwcfg2) && hwcfg2 != 0xffffffffu;
    if (!fsp_state.gsp_readable) return -1;
    return (int)((hwcfg2 >> FSP_HWCFG2_LOCKDOWN_BIT) & 1u);
}

/* One packet: the two header dwords and the payload, into the channel from offset 0, with the queue tail
 * moved as the last write because that is the moment the FSP is allowed to look.  Multi-packet messages -
 * which is what a reply larger than the channel would be - are not sent and not pretended at: the payload
 * here is one byte or 860 of them, and both fit. */
static int send_packet(const uint8_t *payload, uint32_t size, uint32_t nvdm_type)
{
    uint32_t header[2];
    uint32_t padded = (size + 3u) & ~3u, before, after;
    int idle;

    if (padded + FSP_HEADER_BYTES > FSP_EMEM_CHANNEL_BYTES) return 0;

    header[0] = MCTP_VERSION | (1u << 30) | (1u << 31);            /* seid 0, seq 0, end and start of message */
    header[1] = MCTP_TYPE_VENDOR_PCI | (MCTP_VENDOR_ID_NVIDIA << 8) | (nvdm_type << 24);

    fsp_state.polled = 0;
    for (unsigned i = 0; i < FSP_POLL_LIMIT; i++) {
        fsp_state.polled++;
        if (!queues(&idle)) { fsp_state.send_blocked = 2; return 0; }
        if (idle == 1) break;
    }
    if (idle != 1) { fsp_state.send_blocked = 1; return 0; }        /* busy the whole time: nothing written */

    emem_seek(0, 1, 0);
    /* Where the channel's offset is *after* it has been set to 0, which is what the reference driver measures
     * too: the seek itself must not move it, and if it did, the count below would be a complaint about the
     * seek rather than about the writes. */
    before = emem_offset_dwords();
    write32(&w_fsp, NV_PFSP_EMEMD, header[0]);
    write32(&w_fsp, NV_PFSP_EMEMD, header[1]);
    for (uint32_t i = 0; i < padded; i += 4) {
        uint32_t word = 0;
        for (uint32_t b = 0; b < 4 && i + b < size; b++) word |= (uint32_t)payload[i + b] << (b * 8);
        write32(&w_fsp, NV_PFSP_EMEMD, word);
    }
    after = emem_offset_dwords();
    /* Recorded, not obeyed.  This used to abort the request when the offset had not advanced by exactly one
     * dword per write, on the belief that the channel would then hold a fraction of a packet; the card said
     * otherwise.  Measured on the GB207: send blocked(3) with the packet already written and the queue idle,
     * which means the only thing the veto achieved was that no doorbell was ever rung and no exchange ever
     * happened - strictly less known than a reply.  What decides whether a packet is well formed is the FSP
     * itself, which is given the length in the header and authenticates the image by signature, so the walk is
     * reported here and the doorbell is rung.  If the walk is zero, that is in the panel as a separate fact and
     * the next question becomes why stores are swallowed, which is a hardware question with an answer. */
    fsp_state.send_walk = after - before;

    fsp_state.packet_bytes = padded + FSP_HEADER_BYTES;
    fsp_state.doorbell = padded + 4u;        /* the tail is the last dword written, not the one after it */
    write32(&w_fsp, NV_PFSP_QUEUE_TAIL, fsp_state.doorbell);
    write32(&w_fsp, NV_PFSP_QUEUE_HEAD, 0);
    return 1;
}

/* A reply, read the same way and acknowledged by moving the message queue's tail to its head.  The packet
 * carries the two header dwords first: the transport one says where the message starts and ends, the NVDM
 * one says whose message it is, and only after those eight bytes does the payload begin - with the type of
 * the response, then task, command and error as the vendor's own response struct lays them out. */
static int read_reply(uint8_t *buffer, uint32_t capacity, struct gpu_fsp_reply *reply)
{
    uint32_t size, transport, message;

    size = (fsp_state.msgq_tail - fsp_state.msgq_head) + 4u;
    if (size < 4u) return -1;
    if (size > capacity) { reply->oversize = (int)size; return 0; }

    emem_seek(0, 0, 1);
    for (uint32_t i = 0; i + 4 <= size; i += 4) store32(buffer + i, read32(&w_fsp, NV_PFSP_EMEMD));
    reply->bytes = size;
    /* The read has to have walked as many dwords as it claims to have returned, for the same reason the
     * write does: the packet came out of one register, and only the offset proves they were different dwords
     * of the channel rather than the same one read back five times. */
    reply->delta_ok = emem_offset_dwords() == size / 4u;
    /* Acknowledge by pulling *both* pointers to zero, which is what the driver that boots this family does
     * (`nvkm_wr32(MSGQ_TAIL, 0); nvkm_wr32(MSGQ_HEAD, 0)' in gh100_fsp_recv) - not up to the head, which is
     * how the other reference implementation leaves them.  The difference is invisible after one message and
     * decisive after two: this kernel reads a reply out of channel offset 0, so the second message - the
     * handover, right after the query - is only found there if the ring was restarted rather than left
     * wherever the first one ended.  Two writes, and the ring is empty from both ends. */
    write32(&w_fsp, NV_PFSP_MSGQ_TAIL, 0);
    write32(&w_fsp, NV_PFSP_MSGQ_HEAD, 0);

    transport = load32(buffer);
    message = load32(buffer + 4);
    reply->som = (int)((transport >> 31) & 1u);
    reply->eom = (int)((transport >> 30) & 1u);
    reply->framed = size >= FSP_HEADER_BYTES && (message & 0x7fu) == MCTP_TYPE_VENDOR_PCI &&
                    ((message >> 8) & 0xffffu) == MCTP_VENDOR_ID_NVIDIA;
    if (!reply->framed || !reply->delta_ok) return 0;
    reply->type = (int)(message >> 24);
    if (size >= FSP_HEADER_BYTES + 1u + 12u) {
        reply->kind = buffer[FSP_HEADER_BYTES];
        reply->task = load32(buffer + FSP_HEADER_BYTES + 1u);
        reply->command = load32(buffer + FSP_HEADER_BYTES + 5u);
        if (size >= FSP_HEADER_BYTES + 1u + 16u) reply->error = load32(buffer + FSP_HEADER_BYTES + 9u);
        reply->complete = 1;
    }
    return 1;
}

/* The FSP answers with one of the falcon return values, published as a numbered list rather than as anything a
 * caller can print generically: 0 is FLCN_OK and every other value is a named failure of the request or of the
 * firmware image.  Only the codes this message can receive are named, and an unnamed code is printed as a
 * number rather than dressed up as the nearest name - the two FMC entries are the ones a handover can hinge on,
 * because they mean the image was read and rejected rather than ignored. */
static const char *flcn_name(uint32_t code)
{
    switch (code) {
        case 0x00u: return "FLCN_OK";
        case 0x01u: return "FLCN_ERR_NOT_SUPPORTED";
        case 0x02u: return "FLCN_ERR_INVALID_ARGUMENT";
        case 0x03u: return "FLCN_ERR_ILLEGAL_OPERATION";
        case 0x04u: return "FLCN_ERR_TIMEOUT";
        case 0x05u: return "FLCN_ERR_NO_FREE_MEM";
        case 0x19u: return "FLCN_ERR_BAR0_PRIV_READ_ERROR";
        case 0x1au: return "FLCN_ERR_BAR0_PRIV_WRITE_ERROR";
        case 0x21u: return "FLCN_ERR_CSB_PRIV_READ_ERROR";
        case 0x22u: return "FLCN_ERR_CSB_PRIV_WRITE_ERROR";
        case 0x24u: return "FLCN_ERR_PRIV_SEC_VIOLATION";
        case 0x25u: return "FLCN_ERR_INVALID_VERSION";
        case 0x27u: return "FLCN_ERR_GPU_IN_DEBUG_MODE";
        case 0x35u: return "FLCN_ERR_INVALID_STATE";
        case 0xd6u: return "FLCN_ERR_MEMERR_ERROR_VIOLATION";
        case 0xd7u: return "FLCN_ERR_HUB_ERROR_VIOLATION";
        case 0xd8u: return "FLCN_ERR_DIOPMP_ERROR_VIOLATION";
        case 0xd9u: return "FLCN_ERR_RISCV_EXCEPTION_ERROR";
        case 0xdau: return "FLCN_ERR_RISCV_BOOTLOADER_ERROR";
        case 0xdbu: return "FLCN_ERR_FMC_BOOT_CHECK_FAIL";
        case 0xdcu: return "FLCN_ERR_SE_HW_INIT_FAIL";
        case 0xddu: return "FLCN_ERR_FMC_INVALID_DMA_TARGET";
        case 0xdeu: return "FLCN_ERR_TEGRA_CRYPTO_INIT_FAIL";
        case 0xdfu: return "FLCN_ERR_FMC_BOOT_FROM_INVALID_WPR";
        case 0xe0u: return "FLCN_ERR_PRESYNC_TIMEOUT_ERR";
        case 0xfeu: return "FLCN_ERR_BINARY_NOT_STARTED";
        case 0xffu: return "FLCN_ERROR";
        default: return 0;
    }
}

/* The FMC image as the handover wants it: the image section copied into pages this kernel owns, and the
 * three authentication blobs taken out of the container into the payload itself.  The sizes are not
 * preferences - the published payload has 12 words for a SHA-384, 96 for the key and 96 for the signature,
 * so an image whose sections are any other size cannot be described by this format, and saying so is the
 * whole point of checking before anything is sent. */
/* The reference driver's per-generation FSP parameters, in its own words: `gh100_fsp' is
 * { version 1, hash 48, public key 384, signature 384 } and both `gb100_fsp' and `gb202_fsp' are
 * { version 2, hash 48, public key 97, signature 96 } (the three files nvkm/subdev/fsp/gh100.c, gb100.c and gb202.c, MIT).  There is no FSP entry for
 * any other architecture there, which is the same statement as "that chip has no mailbox at these offsets":
 * Turing, Ampere and Ada reach their coprocessor through the older ACR path, and poking the FSP page of a
 * generation whose documentation does not define it is how a report gets a number nobody can interpret.
 *
 * `arch' here is read from the aperture rather than from a PCI id, because the sizes belong to the silicon.
 * A chip that will not say (0, all ones, or a PRI error) leaves every size at zero and the handover
 * unsendable: an unknown expectation is not a licence to assume one, and the panel prints the difference. */
static void cot_parameters(void)
{
    uint32_t boot0 = read32(&w_vf, FSP_PMC_BOOT_0);

    if (FSP_REFUSED(boot0) || FSP_NOT_CONTENT(boot0) || boot0 == 0 || boot0 == 0xffffffffu) {
        fsp_state.arch_unknown = 1;
        return;
    }
    fsp_state.arch = (boot0 >> 24) & 0x1fu;
    fsp_state.cot_hash_bytes = 48u;
    switch (fsp_state.arch) {
    case 0x18u:                                           /* GH100 */
        fsp_state.fsp_supported = 1;
        fsp_state.cot_version = 1u;
        fsp_state.cot_pkey_bytes = 384u;
        fsp_state.cot_sig_bytes = 384u;
        break;
    case 0x1Au:                                           /* GB100: the GB10x chips */
    case 0x1Bu:                                           /* GB200: the GB2xx chips, this card's family */
        fsp_state.fsp_supported = 1;
        fsp_state.cot_version = 2u;
        fsp_state.cot_pkey_bytes = 97u;
        fsp_state.cot_sig_bytes = 96u;
        break;
    default:
        fsp_state.fsp_supported = 0;
        fsp_state.cot_version = 0;
        fsp_state.cot_hash_bytes = 0;
        break;
    }
}

static void stage_fmc(void)
{
    const struct gpu_firmware_state *fws = gpu_firmware_state();
    const struct gpu_firmware_image *image;
    const uint8_t *base;
    uint64_t bytes;
    uintptr_t pages;

    if (!fws || !fws->resident || !fws->resident_bytes) return;
    image = &fws->image[1];                            /* BOOT_FIRMWARE_ROLE_FMC */
    if (!image->resident) return;        /* nothing was kept of this role, which the panel's firmware
                                            * paragraph already says in full: no second notice here */
    fsp_state.fmc_seen = 1;
    if (!image->checksum_matches) { fsp_state.cot_defect = 7; return; }
    if (!image->fmc_named) { fsp_state.cot_defect = 8; return; }
    if (!fsp_state.fsp_supported) { fsp_state.cot_defect = fsp_state.arch_unknown ? 9 : 10; return; }
    if (image->hash_bytes != fsp_state.cot_hash_bytes) { fsp_state.cot_defect = 1; return; }
    if (image->publickey_bytes != fsp_state.cot_pkey_bytes) { fsp_state.cot_defect = 2; return; }
    /* The FMC image's own signature section, which is a different field from the resource manager image's:
     * the two files name their authentication data differently, and the handover wants the FMC's, because the
     * FMC is what the FSP verifies before it will load anything. */
    if (image->fmc_signature_bytes != fsp_state.cot_sig_bytes) { fsp_state.cot_defect = 3; return; }
    if (!image->image_bytes || image->image_bytes > FSP_IMAGE_MAX) { fsp_state.cot_defect = 4; return; }
    if (!image->fmc_sections) { fsp_state.cot_defect = 5; return; }

    fsp_state.image_bytes = image->image_bytes;
    bytes = (image->image_bytes + 0xfffu) & ~0xfffu;
    pages = pages_allocate_limit((size_t)(bytes / 0x1000u), PHYSICAL_LIMIT);
    if (!pages) { fsp_state.cot_defect = 6; return; }
    base = (const uint8_t *)(uintptr_t)(fws->address + image->offset);
    memcpy((void *)pages, base + image->image_offset, image->image_bytes);
    memset((void *)(pages + image->image_bytes), 0, (size_t)(bytes - image->image_bytes));
    fsp_state.image_physical = pages;        /* this kernel maps the RAM it runs on one to one, so the */
    fsp_state.image_crc32 = gpu_firmware_crc32((const uint8_t *)pages, image->image_bytes);
    fsp_state.staged = 1;                    /* address the FSP is given is the address it was written at */

    memset(fsp_state.payload, 0, sizeof fsp_state.payload);
    store16(fsp_state.payload + COT_OFF_VERSION, fsp_state.cot_version);
    store16(fsp_state.payload + COT_OFF_SIZE, COT_SIZE);
    store64(fsp_state.payload + COT_OFF_FMC_ADDRESS, fsp_state.image_physical);
    store64(fsp_state.payload + COT_OFF_BOOT_ARGS, (uint64_t)-1);
    /* Two fields the driver that boots this chip fills and this one cannot yet.  The FRTS is a 1 MiB region
     * placed by an offset back from the *end* of the frame buffer (aligned to the 128 KiB WPR granularity),
     * and the boot arguments are a GSP_FMC_BOOT_PARAMS block in sysmem whose address the FMC reads back into
     * its mailbox when it starts.  This OS reserves no vidmem and has no resource-manager descriptor to point
     * at, so the FRTS fields are the declined encoding the vendor driver itself uses when it has no region
     * (offset 0, size 0) and the arguments are (uint64_t)-1, which is the same "none" that file writes.  A
     * handover accepted with these declined is a verified FMC image with nothing to boot *into*: the FSP
     * refusing or accepting it is still the measurement, and a silent zero pretending to be a reservation
     * would not be. */
    /* Each blob is copied at the length *this* generation's FSP table asks for, into a field of the published
     * size, exactly as the reference driver memcpy's `func->cot.size_hash/_pkey/_sig' bytes into hash384[12],
     * publicKey[96] and signature[96].  The rest of the field stays zero, which is why the payload is 860
     * bytes on Hopper and on Blackwell while the data inside it is not interchangeable. */
    memcpy(fsp_state.payload + COT_OFF_HASH, base + image->hash_offset, fsp_state.cot_hash_bytes);
    memcpy(fsp_state.payload + COT_OFF_PUBLICKEY, base + image->publickey_offset, fsp_state.cot_pkey_bytes);
    memcpy(fsp_state.payload + COT_OFF_SIGNATURE, base + image->fmc_signature_offset, fsp_state.cot_sig_bytes);
    fsp_state.cot_ready = 1;
}

/* The handover, sent.  Everything above this function was built to make this message possible, and it is the
 * only write in this OS that asks the card to do something.
 *
 * It is reached only when the FSP has answered a read-only query with a framed reply and no error, which is
 * this kernel's way of knowing there is somebody at the other end of the channel before a firmware image is put
 * in front of it.  What the card does with it is the FSP's business: verify the 48-byte hash against the copy
 * staged in this kernel's own pages, verify the signature against the public key, load the image into the
 * protected region and start the coprocessor - or refuse, with one of the falcon error codes decoded above.
 * The reference driver does exactly this, in this order, on this generation.
 *
 * What is measured afterwards is the point: the coprocessor's capability word and the FSP's boot-complete
 * scratch are read again, because those are the two words that change when a boot has happened.  A refusal is a
 * normal answer; an acceptance with the lock still on would be a strange one, and both are reported.  Nothing
 * else in this boot resets, touches or reconfigures the card, so a refusal leaves the machine exactly where it
 * was found. */
/* One sample of the GPU's clock, read the way the reference manual says it has to be read. */
static int read_ptimer(uint32_t *nanoseconds)
{
    for (int attempt = 0; attempt < 8; attempt++) {
        uint32_t high = read32(&w_timer, NV_TIMER_TIME_1);
        uint32_t low = read32(&w_timer, NV_TIMER_TIME_0);
        if (read32(&w_timer, NV_TIMER_TIME_1) == high) {
            *nanoseconds = ((uint64_t)high << 32) | low;
            return 1;
        }
    }
    return 0;
}

/* What a submission would need, and whether it is there: the clock it would time itself against, and the page
 * whose doorbell a channel is rung through.  Read-only, and deliberately not complete - this answers two
 * questions on the panel and writes nothing. */
static void measure_submission_surface(void)
{
    uint32_t words[6];
    int answered = 0;

    if (fsp_state.timer_readable) {
        uint32_t end;
        if (read_ptimer(&fsp_state.timer_start)) {
            for (volatile int spin = 0; spin < 4096; spin++) ;
            if (read_ptimer(&end)) {
                fsp_state.timer_end = end;
                fsp_state.timer_ticking = end > fsp_state.timer_start;
            }
        }
        fsp_state.timer_dead = fsp_state.timer_start == 0xffffffffu && fsp_state.timer_end == 0xffffffffu;
        fsp_state.timer_refused = FSP_NOT_CONTENT(fsp_state.timer_start) || FSP_NOT_CONTENT(fsp_state.timer_end);
        if (fsp_state.timer_refused) fsp_state.timer_ticking = 0;
    }
    if (!fsp_state.vf_readable) return;
    words[0] = fsp_state.vf_leaf0 = read32(&w_vf, NV_VF_INTR_LEAF0);
    words[1] = fsp_state.vf_top0 = read32(&w_vf, NV_VF_INTR_TOP0);
    for (unsigned i = 0; i < 4; i++)
        words[2 + i] = fsp_state.vf_scratch[i] = read32(&w_vf, NV_VF_SCRATCH0 + i * 4u);
    for (unsigned i = 0; i < 6; i++) {
        if (FSP_NOT_CONTENT(words[i])) fsp_state.vf_refusals++;
        else if (words[i] != 0 && words[i] != 0xffffffffu) answered++;
    }
    fsp_state.vf_answered = answered;
}

const char *gpu_fsp_flcn_name(uint32_t code)
{
    return flcn_name(code);
}

static void hand_over_fmc(void)
{
    uint8_t reply[128];
    int idle;

    fsp_state.handover_attempted = 1;
    if (!send_packet(fsp_state.payload, COT_SIZE, NVDM_TYPE_COT)) {
        fsp_state.handover_blocked = fsp_state.send_blocked;
        return;
    }
    fsp_state.handover_sent = 1;
    fsp_state.handover.request_bytes = fsp_state.packet_bytes;
    if (!wait_for(reply_waiting, &fsp_state.handover_polls)) return;
    if (read_reply(reply, sizeof reply, &fsp_state.handover) != 1) return;

    fsp_state.hwcfg2_after = read32(&w_gsp, NV_PGSP_HWCFG2);
    fsp_state.boot_scratch_after = read32(&w_therm, NV_THERM_FSP_BOOT_COMPLETE);
    fsp_state.gsp_mailbox0_after = read32(&w_gsp, NV_PGSP_MAILBOX0);
    fsp_state.gsp_mailbox1_after = read32(&w_gsp, NV_PGSP_MAILBOX1);
    if (fsp_state.timer_readable) read_ptimer(&fsp_state.timer_after);
    /* The reference driver's own completion test, in its own order (gh100_gsp_lockdown_released): a refusal is
     * "not yet", a non-zero mailbox pair is an error unless it is the address the driver handed the FMC as its
     * boot arguments, and the lock is bit 13.  There is no boot-args block in this OS, so a mailbox that
     * answers with anything at all is reported as an answer that cannot be matched rather than as success -
     * and `gsp_released' below is the bit, never "a register read something". */
    if (!FSP_REFUSED(fsp_state.hwcfg2_after) && !FSP_NOT_CONTENT(fsp_state.hwcfg2_after) &&
        fsp_state.hwcfg2_after != 0xffffffffu)
        fsp_state.lockdown_after = (int)((fsp_state.hwcfg2_after >> FSP_HWCFG2_LOCKDOWN_BIT) & 1u);
    else
        fsp_state.lockdown_after = -1;
    fsp_state.gsp_released = fsp_state.lockdown_after == 0;
    queues(&idle);
    fsp_state.queues_idle_after = idle == 1;
}

/* Does the queue accept a store from us?  Asked by storing, not by reading: the pair is zeroed the same way
 * the reference driver zeroes it on receive (gh100's nvkm_fsp_recv clears QUEUE_HEAD and QUEUE_TAIL together),
 * and the answer is what comes back afterwards.  A register that returns what was written to it is a register
 * this OS can submit through even when it will not disclose its previous value; a register that answers
 * 0xbadf... to the read-back has told us, at the only moment that matters, that the store did not land. */
static void probe_queue_write_access(void)
{
    if (!w_fsp.page) return;
    /* The *reply* queue, deliberately: writing the submit doorbell before anything is staged would be a packet
     * nobody asked for, and these two words are the ones the reference driver clears when it receives. */
    write32(&w_fsp, NV_PFSP_MSGQ_TAIL, 0);
    write32(&w_fsp, NV_PFSP_MSGQ_HEAD, 0);
    fsp_state.reset_head = read32(&w_fsp, NV_PFSP_MSGQ_HEAD);
    fsp_state.reset_tail = read32(&w_fsp, NV_PFSP_MSGQ_TAIL);
    fsp_state.queue_writable = !FSP_NOT_CONTENT(fsp_state.reset_tail) && fsp_state.reset_tail == 0;
    klog(FSP_LOG_PREFIX "the reply queue was written 0 in both words and read back head 0x%x tail 0x%x, which "
         "the queue %s", fsp_state.reset_head, fsp_state.reset_tail,
         fsp_state.queue_writable ? "accepted" : "refused");
}

static void probe_mailbox(void)
{
    int idle;

    /* The queue first, and before any waiting: on a card that refuses the FSP's message page outright, a
     * readiness poll has nothing to become ready for and still costs the caller the whole budget.  Measured on
     * a GB207 that was 4056 ms of desktop for a handover that was staged and never sent (reads 94, writes 0),
     * and the desktop's first interactions are the thing that pays it. */
    /* Wait for the FSP to say it is up, because on a card that is going to answer there is a window in which
     * it has not yet, and a report written inside that window would be a measurement of the wrong moment.
     * A refusal ends that wait on its first look (see boot_ready), which is the whole of the fix for the
     * 4056 ms measured on a GB207 whose FSP window answers 0xbadf1100: the budget is not the thing being spent
     * when a page says it will not be decoded, the polling is, and polling stops at the answer. */
    fsp_state.boot_ms = FSP_WAIT_BOOT_MS;
    (void)wait_for_timed(boot_ready, &fsp_state.boot_polls, FSP_WAIT_BOOT_MS, &fsp_state.boot_ms_used);
    /* No ++ here: the wait already counted the look that answered, and a count that adds one for the
     * reader's benefit is a count of nothing. */
    fsp_state.booted = fsp_state.boot_scratch == FSP_BOOT_COMPLETE_SUCCESS;
    fsp_state.ucode_version = read32(&w_fsp, NV_PFSP_UCODE_VERSION) & 0x7ffu;
    fsp_state.fuse_check = read32(&w_fsp, NV_PFSP_FUSE_ERROR_CHECK);
    fsp_state.lockdown = lockdown_now();
    fsp_state.gsp_released = fsp_state.lockdown == 0;
    fsp_state.gsp_mailbox0 = read32(&w_gsp, NV_PGSP_MAILBOX0);
    fsp_state.gsp_mailbox1 = read32(&w_gsp, NV_PGSP_MAILBOX1);
    int queue_readable = queues(&idle);
    if (!queue_readable) probe_queue_write_access();   /* a refusing card gets asked the one question that is
                                                           left; a card that answers is not reset under itself */
    /* The gate used to be "can we read the pair"; on a card that answers 0xbadf... to reads and would take a
     * store, that turned into a boot where nothing was ever written and nothing was ever learned.  It is now
     * "can we read the pair, or did the write we made stick": either one is enough to try the query, and the
     * read-back is reported so a refusal is a measurement and not an assumption. */
    if (!queue_readable && !fsp_state.queue_writable) return;  /* no mailbox this reader can use at all */
    fsp_state.mailbox_readable = 1;
    /* Send when the queue itself says it is empty, whether or not the boot scratch agreed.  Measured on the
     * GB207: the FSP scratch at 0x8f0320 answers four zeros and its clock never ticks, which the reference
     * driver describes as a *decoupled* window - the CPU is simply not permitted to see it - while the queue
     * registers answer and report themselves idle.  Requiring the scratch was therefore waiting on a signal this
     * reader cannot observe, and it is the reason the last two pastes end with the same number: writes 0, not
     * one store tried.  The pair of head/tail words is the condition the reference driver checks before it
     * writes QUEUE_TAIL, so that is the condition we check, and `booted' stays in the report as what it is: a
     * read of a window, not a permission to send. */
    if (!fsp_state.booted && !(queue_readable && idle == 1)) return;

    {
        uint8_t query = (uint8_t)NVDM_TYPE_CLOCK_BOOST;   /* which capability to ask about; asking is not
                                                              * setting it, whatever the answer is */
        uint8_t reply[128];
        int read;

        if (!send_packet(&query, 1, NVDM_TYPE_CAPS_QUERY)) return;
        fsp_state.queried = 1;
        fsp_state.query.request_bytes = fsp_state.packet_bytes;
        if (!wait_for(reply_waiting, &fsp_state.reply_polls)) return;
        read = read_reply(reply, sizeof reply, &fsp_state.query);
        fsp_state.responded = read == 1;
        /* The one condition under which a firmware image is offered: the card asked, answered, and the answer
         * was well-formed and clean.  Anything less is a channel with nobody behind it, and a handover written
         * into that could not be told apart from one that was never written at all. */
        if (fsp_state.responded && fsp_state.query.framed && fsp_state.query.complete &&
            fsp_state.query.error == 0 && fsp_state.query.command == NVDM_TYPE_CAPS_QUERY &&
            fsp_state.cot_ready && !fsp_state.cot_defect)
            hand_over_fmc();
    }
}

void gpu_fsp_note(const struct gpu_device *device, const struct boot_handoff *handoff)
{
    uint64_t bar0 = device ? device->bar[0] : 0;

    /* The state exists for every machine, not only for ones this can be asked about: the fact that the
     * question was not asked is the answer a reader on a QEMU boot or an AMD laptop needs, and a paragraph
     * that vanishes tells them nothing about which of the two it is. */
    (void)handoff;                                     /* the firmware record is reached through its own
                                                          * accessor, which is the only reason it exists */
    /* Every offset in this file is an NVIDIA decode of an NVIDIA BAR.  A machine whose display adapter is
     * somebody else's has three pages at the same distance into its own aperture, and they belong to a
     * device with no idea what a dword written at BAR+0x8f2ac4 means - so the vendor is checked before
     * anything is mapped, and a QEMU boot is silently, verifiably, untouched by all of this. */
    fsp_state.present = 1;
    fsp_state.vendor = device ? device->vendor : 0;
    fsp_state.bar0 = bar0;
    if (!device || device->vendor != 0x10deu) return;
    fsp_state.nvidia = 1;
    if (!bar0 || bar0 >> 46) { fsp_state.skipped = bar0 ? 2 : 1; return; }
    map_mailbox_pages(bar0);
    if (!fsp_state.mapped) {
        klog(FSP_LOG_PREFIX "the mailbox page at 0x%x and the therm scratch at 0x%x could not be mapped, so "
             "nothing was asked of them", 0x8f0000u, 0x20000u);
        return;
    }
    fsp_state.attempted = 1;
    /* Staged first, because the handover can be sent during the probe: the payload and the pages it names have
     * to exist before the message that describes them leaves this machine. */
    measure_submission_surface();
    measure_tick_rate();                          /* before any wait, so the waits can be promised in time */
    cot_parameters();                             /* and the generation is decided before a byte is staged */
    /* A chip the reference driver has no FSP for gets no packet at all - not even the read-only capability
     * query - because the page this file would write into is not documented as a queue on that generation,
     * and "it is only a query" is not a reason to write to an undocumented register file on a machine whose
     * firmware is the only thing between this OS and a hung display.  What was measured instead is kept. */
    if (!fsp_state.fsp_supported) {
        /* The two "not this chip" codes, so the panel's handover sentence names the architecture rather than
         * describing a defect in somebody's file. */
        fsp_state.cot_defect = fsp_state.arch_unknown ? 9u : 10u;
        klog(FSP_LOG_PREFIX "%s (architecture 0x%x read from the aperture), so nothing was written to it",
             fsp_state.arch_unknown ? "the architecture would not answer"
                                    : "no FSP mailbox exists for this architecture",
             (unsigned)fsp_state.arch);
        return;
    }
    stage_fmc();
    probe_mailbox();
    klog(FSP_LOG_PREFIX "boot scratch 0x%x, mailbox %s, query %s, reply %s, writes %u of %u reads",
         fsp_state.boot_scratch, fsp_state.mailbox_readable ? "answers" : "refuses",
         fsp_state.queried ? "sent" : "not sent", fsp_state.responded ? "read" : "none",
         (unsigned)fsp_state.writes, (unsigned)fsp_state.reads);
    if (fsp_state.handover_sent)
        klog(FSP_LOG_PREFIX "gsp-fmc handover sent: %u B reply, error 0x%x, hwcfg2 0x%x before 0x%x after",
             (unsigned)fsp_state.handover.bytes, (unsigned)fsp_state.handover.error,
             (unsigned)fsp_state.hwcfg2, (unsigned)fsp_state.hwcfg2_after);
    else if (fsp_state.staged)
        klog(FSP_LOG_PREFIX "gsp-fmc handover staged and not sent: %u B image at 0x%x, crc32 0x%x",
             (unsigned)fsp_state.image_bytes, (unsigned)fsp_state.image_physical,
             (unsigned)fsp_state.image_crc32);
    else if (fsp_state.cot_defect)
        klog(FSP_LOG_PREFIX "gsp-fmc handover not staged: reason %u", (unsigned)fsp_state.cot_defect);
}

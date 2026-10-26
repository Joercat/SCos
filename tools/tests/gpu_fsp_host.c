/* The security-processor mailbox, run against a model of the card's register file.
 *
 * What is under test is kernel/drivers/gpu/gpu_fsp.c exactly as the kernel compiles it: the framing of a
 * request, the order and values of the register writes that put it in the channel, the reading of a reply out
 * of the same window, and every reason the file decides not to send.  What is deliberately *not* here is a
 * second implementation of any of that logic: the model below is a FSP and answers from the published register
 * semantics - a channel whose offset walks by itself while its data window is read or written, a queue tail
 * whose write is the request, a head pulled up to the tail once the packet has been consumed.  That is why the
 * seam the file exposes is gpu_io_read32/gpu_io_write32 and nothing larger: the interesting behaviour of these
 * registers is that touching one changes another, and a test whose "card" were a flat array could only ever
 * prove that the driver agrees with itself.
 *
 * Every byte the model expects is written out here in full, because the point is that a change in the framing
 * - a swapped SOM/EOM, a type byte one dword over, a tail that names the dword *after* the last write instead
 * of the last one - fails a check here rather than on hardware, in front of somebody with a serial cable.
 *
 *   cc -std=gnu11 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector \
 *      -fsanitize=address,undefined -fno-sanitize-recover=all -Ikernel/include \
 *      tools/tests/gpu_fsp_host.c -o /tmp/fsp && /tmp/fsp
 *
 * The kernel sources come in by #include rather than being compiled alongside, because the rules under test
 * are file-static; the two host-side definitions near the bottom (device_map, pages_allocate_limit) are the
 * ones the kernel's own versions would otherwise collide with.
 */

/* stdio for the report, and nothing else: scos.h is what declares the memcpy/memcmp/strlen this harness uses,
 * the same declarations the kernel compiles against, and pulling in the host's <string.h> as well would give
 * strstr two incompatible prototypes - which is the same trap the other kernel-side harness in this directory
 * sidesteps, for the same reason. */
#include <stdio.h>
#include <stdint.h>

#include "boot.h"

void klog(const char *fmt, ...) { (void)fmt; }

#include "../../kernel/drivers/gpu/gpu_firmware.c"
#include "../../kernel/drivers/gpu/gpu_fsp.c"

/* ------------------------------------------------------------- the model -------------------------------------------------- */

#define MODEL_THERM   0x20000u
#define MODEL_FSP     0x8f0000u
#define MODEL_GSP     0x110000u
#define MODEL_VF      0x000000u
#define MODEL_TIMER   0x9000u
#define VF_INTR_LEAF0   (0x1000u >> 2)
#define VF_INTR_TOP0    (0x1600u >> 2)
#define VF_SCRATCH0     (0x2100u >> 2)
#define TIMER_TIME_0    (0x400u >> 2)      /* window-relative: the window starts at 0x9000 */
#define TIMER_TIME_1    (0x410u >> 2)
#define MODEL_BAR0    0xe0000000ull

/* Register indices, derived from the production macros rather than repeated as numbers: if the file under test
 * ever starts believing the mailbox lives somewhere else, the model has to move with it, or a test would keep
 * passing while both were wrong in the same direction. */
#define FSP_DWORD(off)      (((off) - MODEL_FSP) >> 2)
#define R_EMEMC             FSP_DWORD(NV_PFSP_EMEMC)
#define R_EMEMD             FSP_DWORD(NV_PFSP_EMEMD)
#define R_QUEUE_HEAD        FSP_DWORD(NV_PFSP_QUEUE_HEAD)
#define R_QUEUE_TAIL        FSP_DWORD(NV_PFSP_QUEUE_TAIL)
#define R_MSGQ_HEAD         FSP_DWORD(NV_PFSP_MSGQ_HEAD)
#define R_MSGQ_TAIL         FSP_DWORD(NV_PFSP_MSGQ_TAIL)
#define T_BOOT_COMPLETE     (((NV_THERM_FSP_BOOT_COMPLETE) - MODEL_THERM) >> 2)
#define G_HWCFG2            (((NV_PGSP_HWCFG2) - MODEL_GSP) >> 2)
#define S_UCODE             FSP_DWORD(NV_PFSP_UCODE_VERSION)

/* One bit per way a card can fail to be helpful. */
#define M_REFUSE      1u   /* the mailbox and the coprocessor's block both answer the locked-target refusal */
#define M_NOT_BOOTED  2u   /* the boot-complete scratch does not say 0xFF */
#define M_BUSY        4u   /* the command queue's head never catches its tail */
#define M_NO_REPLY    8u   /* consume the packet, answer nothing */
#define M_NO_AINC    16u   /* EMEMC does not walk, so every write lands on the same dword */
#define M_BAD_FRAME  32u   /* answer, with a vendor id that is not NVIDIA's */
#define M_OVERSIZE   64u   /* answer with more than the caller has room for */
#define M_NO_PAGES  128u   /* the page allocator cannot serve an image-sized copy */
#define M_MAP_REFUSE 256u  /* the mapping itself is refused, which is a different fact from a refusal */
#define M_RELEASED  512u   /* the coprocessor's capability word answers instead of refusing */
#define M_TIMER_FROZEN 1024u /* the timer block answers and its counter does not move */
#define M_VF_LIVE     2048u  /* the virtual-function page answers with content instead of zeros */
#define M_TIMER_DEAD  4096u  /* the block is not decoded: every read is all-ones */
#define M_COT_WPR     8192u  /* the FSP reads the handover and refuses it with the WPR error */
#define M_COT_SILENT 16384u  /* the FSP takes the handover and never answers */

static uint8_t model_therm[0x1000];
static uint8_t model_fsp[0x4000];
static uint8_t model_gsp[0x1000];
static uint8_t model_vf[0x3000];                      /* NV_VIRTUAL_FUNCTION_PRIV, BAR0+0x0..0x2fff */
static uint8_t model_timer[0x1000];                   /* NV_PTIMER, BAR0+0x9000..0x9fff */
static uint8_t model_channel[FSP_EMEM_CHANNEL_BYTES];
static uint8_t model_pages[16 * 1024] __attribute__((aligned(4096)));

struct model {
    unsigned mode;
    unsigned emem_dword;                          /* what EMEMC says the next window access lands on */
    int ainc_write, ainc_read;
    uint32_t reads, writes, refusals;             /* what the model was actually asked to do */
    uint32_t tail_writes;                         /* how often the doorbell was rung */
    uint32_t last_tail;                           /* and with what value the last time was */
    uint32_t reply_ack_writes;                    /* writes to either reply-queue pointer */
    uint32_t packet[FSP_EMEM_CHANNEL_BYTES / 4];  /* the channel as it stood when the tail moved */
    uint32_t packet_bytes;
    uint32_t cot[FSP_EMEM_CHANNEL_BYTES / 4];     /* and as it stood when the handover was taken */
    uint32_t cot_bytes;
    int cot_seen, cot_shape_ok, cot_target_ok, cot_blobs_ok;
    uint32_t timer_step;                          /* the clock, which moves once per read of the low half */
    int pending_consume;                          /* a packet is on its way to the FSP's head */
    uint32_t pending_tail;
    unsigned tail_seq, consumed_seq;              /* so the reply's ack cannot look like a second request */
    int framing_ok;                               /* the model's own verdict, from the published headers */
    unsigned checks, failures;
} model;

static uint32_t *page_of(int which)
{
    if (which == 0) return (uint32_t *)model_therm;
    if (which == 1) return (uint32_t *)model_fsp;
    if (which == 2) return (uint32_t *)model_gsp;
    if (which == 3) return (uint32_t *)model_vf;
    return (uint32_t *)model_timer;
}

static unsigned window_bytes(int which)
{
    if (which == 1) return 0x4000u;
    if (which == 3) return 0x3000u;
    return 0x1000u;
}

static int window_of(const volatile void *window)
{
    if (window == (const void *)model_therm) return 0;
    if (window == (const void *)model_fsp) return 1;
    if (window == (const void *)model_gsp) return 2;
    if (window == (const void *)model_vf) return 3;
    if (window == (const void *)model_timer) return 4;
    return -1;
}

static uint32_t channel_dword(unsigned dwords)
{
    uint32_t v = 0xa5a5a5a5u;
    if (dwords * 4u + 4u <= sizeof model_channel) memcpy(&v, model_channel + dwords * 4u, 4);
    return v;
}

static void channel_put_dword(unsigned dwords, uint32_t v)
{
    if (dwords * 4u + 4u <= sizeof model_channel) memcpy(model_channel + dwords * 4u, &v, 4);
}

static uint32_t ememc_value(void)
{
    uint32_t v = ((model.emem_dword % FSP_EMEM_DWORDS_PER_BLOCK) << 2) |
                 ((model.emem_dword / FSP_EMEM_DWORDS_PER_BLOCK) << 8);
    if (model.ainc_write) v |= 1u << 24;
    if (model.ainc_read) v |= 1u << 25;
    return v;
}

static void h_put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (i * 8));
}

static uint32_t m_read(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                                   ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* The FSP's half of a doorbell, as the register description says it works: the tail names the last dword the
 * host wrote, the packet starts at offset 0 of the channel, and the head is pulled up to the tail when the
 * packet has been taken.  What the answer is made of is written out byte by byte below, so this function is a
 * description of a card rather than a mirror of the driver. */
static void model_answer_cot(uint32_t bytes);
static void model_consume(void);

static void model_flush(void)
{
    if (model.pending_consume) model_consume();
}

/* The FSP's half of a doorbell, as the register description says it works: the tail names the last dword the
 * host wrote, the packet starts at offset 0 of the channel, and the head is pulled up to the tail when the
 * packet has been taken.  What the answer is made of is written out byte by byte below, so this function is a
 * description of a card rather than a mirror of the driver.
 *
 * It runs when the model gets round to the packet, not the instant the tail moves.  That distinction is the
 * whole reason a second message in one boot is testable here at all: the driver writes the tail and then the
 * head, and a model that completed the packet between those two writes would leave the head at the zero the
 * driver wrote and the queue looking permanently busy.  Real hardware moves the head itself as it consumes, so
 * the model defers one step behind the access that rang it - flush before a read is served, after a write. */
static void model_consume(void)
{
    uint32_t tail = model.pending_tail;
    uint32_t bytes = tail + 4u;
    uint8_t reply[256];
    uint32_t size = 28u;
    int oversize = (model.mode & M_OVERSIZE) != 0;
    int bad_frame = (model.mode & M_BAD_FRAME) != 0;

    model.pending_consume = 0;
    model.consumed_seq = model.tail_seq;
    model.packet_bytes = bytes > sizeof model.packet ? sizeof model.packet : bytes;
    memcpy(model.packet, model_channel, model.packet_bytes);

    model.framing_ok = bytes >= 8 && (m_read(model_channel) & (1u << 31)) && (m_read(model_channel) & (1u << 30)) &&
                       (m_read(model_channel + 4) & 0x7fu) == 0x7eu &&
                       ((m_read(model_channel + 4) >> 8) & 0xffffu) == 0x10deu;

    /* Consumed either way: a card that dislikes a request still takes it out of its queue.  M_BUSY is the
     * separate case of a card that is still working on the one before, and it is the head that stays put. */
    if (model.mode & M_BUSY) return;
    ((uint32_t *)model_fsp)[R_QUEUE_HEAD] = tail;
    if (model.mode & M_NO_REPLY || !model.framing_ok) return;
    if (bytes >= 8 && (m_read(model_channel + 4) >> 24) == NVDM_TYPE_COT) { model_answer_cot(bytes); return; }

    memset(reply, 0, sizeof reply);
    h_put32(reply + 0, 1u | (1u << 30) | (1u << 31));              /* version 1, SOM, EOM: one packet */
    h_put32(reply + 4, (uint32_t)NVDM_TYPE_FSP_RESPONSE << 24 |
           (bad_frame ? 0x1234u : 0x10deu) << 8 | 0x7eu);            /* vendor id swapped: */
    reply[8] = NVDM_TYPE_FSP_RESPONSE;                             /* the response payload names itself first */
    h_put32(reply + 9, 7);                                         /* task id: whatever this request was given */
    h_put32(reply + 13, NVDM_TYPE_CAPS_QUERY);                     /* the request being answered */
    h_put32(reply + 17, oversize ? 0x10001u : 0);                  /* error code */
    h_put32(reply + 21, 0xdeadbeefu);                              /* the capability bits */
    if (oversize) { memset(reply + 25, 0x5c, 200u - 25u); size = 200u; }

    memset(model_channel, 0, sizeof model_channel);
    memcpy(model_channel, reply, size);
    ((uint32_t *)model_fsp)[R_MSGQ_HEAD] = 0;
    ((uint32_t *)model_fsp)[R_MSGQ_TAIL] = ((size + 3u) & ~3u) - 4u;
}

/* The handover, judged by the thing it is asking for.  A query only reports a capability and costs the model
 * nothing to answer, so the caps reply above is unconditional; a handover asks this card to load a firmware
 * image, so the model looks at what was put in the channel and answers with one of the falcon codes the real
 * FSP uses for each way that can fail.  Every check below is a property the driver's payload has to have
 * whatever the card is: the shape the published struct describes, an address the copy actually lives at and
 * that the FSP can name, and the container's own three authentication blobs in their published places.  If the
 * driver ever sends something that is not that, the model refuses it and the test sees a refusal, which is the
 * point of building a card here at all. */
static void model_answer_cot(uint32_t bytes)
{
    const uint8_t *payload = model_channel + FSP_HEADER_BYTES;
    uint8_t reply[64];
    uint32_t size = 28u;
    uint32_t error = 0;

    model.cot_seen = 1;
    model.cot_bytes = bytes < (uint32_t)sizeof model.cot ? bytes : (uint32_t)sizeof model.cot;
    memcpy(model.cot, model_channel, model.cot_bytes);
    (void)0;

    model.cot_shape_ok = bytes >= FSP_HEADER_BYTES + COT_SIZE && (m_read(payload) & 0xffffu) == 2u &&
                         ((m_read(payload) >> 16) & 0xffffu) == COT_SIZE &&
                         m_read(payload + COT_OFF_BOOT_ARGS) == ~(uint32_t)0;
    model.cot_target_ok = m_read(payload + COT_OFF_FMC_ADDRESS) == (uint32_t)(uintptr_t)model_pages &&
                          m_read(payload + COT_OFF_FMC_ADDRESS + 4) == (uint32_t)(((uintptr_t)model_pages) >> 32) &&
                          ((uintptr_t)model_pages & 0xfffu) == 0;
    /* Not the 46-bit rule the driver is held to when it asks for the pages: a host allocation is not a
     * physical address, and the limit is enforced where the pages are requested and tested there (defect 6),
     * so repeating it here would compare a machine's memory map against an array's link address. */
    model.cot_blobs_ok = 1;
    for (unsigned i = 0; i < 48u; i++)
        if (payload[COT_OFF_HASH + i] != (uint8_t)0x11) model.cot_blobs_ok = 0;
    for (unsigned i = 0; i < 384u; i++) {
        if (payload[COT_OFF_PUBLICKEY + i] != (uint8_t)0x22) model.cot_blobs_ok = 0;
        if (payload[COT_OFF_SIGNATURE + i] != (uint8_t)0x33) model.cot_blobs_ok = 0;
    }

    if (!model.cot_shape_ok) error = 0x02u;             /* FLCN_ERR_INVALID_ARGUMENT */
    else if (!model.cot_target_ok) error = 0xddu;       /* FLCN_ERR_FMC_INVALID_DMA_TARGET */
    else if (!model.cot_blobs_ok) error = 0xdbu;        /* FLCN_ERR_FMC_BOOT_CHECK_FAIL */
    else if (model.mode & M_COT_WPR) error = 0xdfu;     /* FLCN_ERR_FMC_BOOT_FROM_INVALID_WPR */
    if (model.mode & M_COT_SILENT) return;

    memset(reply, 0, sizeof reply);
    h_put32(reply + 0, 1u | (1u << 30) | (1u << 31));
    h_put32(reply + 4, (uint32_t)NVDM_TYPE_FSP_RESPONSE << 24 | 0x10deu << 8 | 0x7eu);
    reply[8] = NVDM_TYPE_FSP_RESPONSE;
    h_put32(reply + 9, 8u);                             /* the task this request was given */
    h_put32(reply + 13, NVDM_TYPE_COT);                 /* the request being answered, by its own type */
    h_put32(reply + 17, error);
    memset(model_channel, 0, sizeof model_channel);
    memcpy(model_channel, reply, size);
    ((uint32_t *)model_fsp)[R_MSGQ_HEAD] = 0;
    ((uint32_t *)model_fsp)[R_MSGQ_TAIL] = size - 4u;
    /* The one thing a handover can change that has nothing to do with the answer's wording: while the
     * coprocessor's block stays locked to a cold reader, an accepted image is the event that releases it, and
     * the driver is expected to re-read rather than assume.  This is where that change comes from. */
    if (!error) ((uint32_t *)model_gsp)[G_HWCFG2] = 0x123u;
}

uint32_t gpu_io_read32(const volatile uint32_t *window, uint64_t within)
{
    int which = window_of(window);
    unsigned index = (unsigned)(within >> 2);
    uint32_t value;

    model_flush();                                /* the FSP got round to anything pending by now */
    model.reads++;
    if (which < 0) return 0xa5a5a5a5u;
    if (index * 4u + 4u > window_bytes(which)) return 0xa5a5a5a5u;
    if ((model.mode & M_REFUSE) && which != 0 && !(which == 2 && (model.mode & M_RELEASED))) {
        model.refusals++;
        return 0xbadf4100u;
    }
    /* A clock is not an array: it moves when you look at it, and a model that returned a fixed value would
     * make "the clock advanced" untestable and "it did not" unfalsifiable.  TIME_0 therefore steps on every
     * read, which is also how the driver's own high-low-high sampling rule gets exercised: the high half never
     * moves in this model, so the first pair is accepted rather than retried eight times. */
    if (which == 4 && (model.mode & M_TIMER_DEAD)) return 0xffffffffu;
    if (which == 4 && index == TIMER_TIME_0 && !(model.mode & M_TIMER_FROZEN)) return model.timer_step += 0x400u;
    if (which == 1 && index == R_EMEMC) return ememc_value();
    if (which == 1 && index == R_EMEMD) {
        value = channel_dword(model.emem_dword);
        if (model.ainc_read && !(model.mode & M_NO_AINC)) model.emem_dword++;
        return value;
    }
    return page_of(which)[index];
}

void gpu_io_write32(volatile uint32_t *window, uint64_t within, uint32_t value)
{
    int which = window_of(window);
    unsigned index = (unsigned)(within >> 2);

    model.writes++;
    if (which < 0) return;
    if (index * 4u + 4u > window_bytes(which)) return;
    if (which == 1 && index == R_EMEMC) {
        page_of(which)[index] = value;               /* a register reads back what it was written */
        model.emem_dword = ((value >> 2) & 0x3fu) + ((value >> 8) & 0xffu) * FSP_EMEM_DWORDS_PER_BLOCK;
        model.ainc_write = !(model.mode & M_NO_AINC) && (value & (1u << 24));
        model.ainc_read = !(model.mode & M_NO_AINC) && (value & (1u << 25));
        return;
    }
    if (which == 1 && index == R_EMEMD) {
        channel_put_dword(model.emem_dword, value);
        if (model.ainc_write && !(model.mode & M_NO_AINC)) model.emem_dword++;
        return;
    }
    page_of(which)[index] = value;
    if (index == R_MSGQ_TAIL || index == R_MSGQ_HEAD) model.reply_ack_writes++;
    /* The doorbell is the pair, not the tail: the reference driver writes the tail and then the head in one
     * call (`_kfspUpdateQueueHeadTail(0, paddedSize - 4)'), which is what tells the FSP that bytes 0..tail are
     * a packet.  So the model takes the packet on the head write, and `tail_seq' is what keeps the reply's own
     * acknowledgement - a write to the *message* queue's head, with nothing new in the command queue - from
     * looking like a second request. */
    if (index == R_QUEUE_TAIL) {
        model.tail_writes++;
        model.last_tail = value;
        model.tail_seq++;
    }
    if (index == R_QUEUE_HEAD && model.tail_seq != model.consumed_seq) {
        model.pending_consume = 1;
        model.pending_tail = model.last_tail;
    }
    model_flush();
}

void *device_map(uint64_t physical, uint64_t bytes, int write_combine, uint64_t *mapped_bytes)
{
    uint64_t within = physical - MODEL_BAR0;

    *mapped_bytes = 0;
    if (model.mode & M_MAP_REFUSE) return 0;
    if (write_combine) return 0;                     /* these registers must never be mapped combined */
    if (within == MODEL_THERM && bytes >= 0x1000u) { *mapped_bytes = 0x1000u; return model_therm; }
    if (within == MODEL_FSP && bytes >= 0x4000u) { *mapped_bytes = 0x4000u; return model_fsp; }
    if (within == MODEL_GSP && bytes >= 0x1000u) { *mapped_bytes = 0x1000u; return model_gsp; }
    if (within == MODEL_VF && bytes >= 0x3000u) { *mapped_bytes = 0x3000u; return model_vf; }
    if (within == MODEL_TIMER && bytes >= 0x1000u) { *mapped_bytes = 0x1000u; return model_timer; }
    return 0;
}

uintptr_t pages_allocate_limit(size_t count, uint64_t limit)
{
    if (model.mode & M_NO_PAGES) return 0;
    if (limit < 0x10000ull) return 0;
    if (count * 0x1000u > sizeof model_pages) return 0;
    return (uintptr_t)model_pages;
}

/* ------------------------------------------------------------- plumbing -------------------------------------------------- */

/* Kept outside the model, because resetting the model between scenarios must not reset the record of how many
 * things were checked: a suite whose count is restarted by the last case it ran would print a clean number
 * while having failed the earlier ones, which is what it did before these two lines moved. */
static unsigned checks_run, failures_run;

static void check(const char *what, int ok)
{
    checks_run++;
    if (ok) return;
    failures_run++;
    printf("FAIL: %s\n", what);
}

static void model_reset(unsigned mode)
{
    memset(&fsp_state, 0, sizeof fsp_state);
    memset(model_therm, 0, sizeof model_therm);
    memset(model_fsp, 0, sizeof model_fsp);
    memset(model_gsp, 0, sizeof model_gsp);
    memset(model_vf, 0, sizeof model_vf);
    memset(model_timer, 0, sizeof model_timer);
    memset(&w_vf, 0, sizeof w_vf);
    memset(&w_timer, 0, sizeof w_timer);
    memset(model_channel, 0, sizeof model_channel);
    memset(model_pages, 0, sizeof model_pages);
    if (mode & M_VF_LIVE) {
        h_put32(model_vf + 0x1000u, 0x1u);                 /* an interrupt leaf with something pending */
        h_put32(model_vf + 0x1600u, 0x2u);                 /* and the top node that routes it */
        for (unsigned i = 0; i < 4u; i++) h_put32(model_vf + 0x2100u + i * 4u, 0xa0a0a0a0u + i);
    }
    if (mode & M_TIMER_FROZEN) h_put32(model_timer + 0x400u, 0x1234u);
    memset(&model, 0, sizeof model);
    model.mode = mode;
    *(uint32_t *)(model_therm + T_BOOT_COMPLETE * 4u) = (mode & M_NOT_BOOTED) ? 0x12345600u : 0xffu;
    ((uint32_t *)model_fsp)[S_UCODE] = 42;
    *(uint32_t *)(model_fsp + (NV_PFSP_FUSE_ERROR_CHECK - MODEL_FSP)) = 0;
    ((uint32_t *)model_gsp)[G_HWCFG2] = (mode & M_RELEASED) ? 0x123u : 0xbadf4100u;
    memset(&w_therm, 0, sizeof w_therm);
    memset(&w_fsp, 0, sizeof w_fsp);
    memset(&w_gsp, 0, sizeof w_gsp);
}

static struct gpu_device nvidia_at(void)
{
    struct gpu_device device;
    memset(&device, 0, sizeof device);
    device.vendor = 0x10de;
    device.bar[0] = MODEL_BAR0;
    return device;
}

static void ask(unsigned mode)
{
    struct boot_handoff handoff;
    struct gpu_device device = nvidia_at();

    memset(&handoff, 0, sizeof handoff);
    model_reset(mode);
    gpu_fsp_note(&device, &handoff);
}

/* An FMC image with the sizes a handover payload has fields for - 48, 384 and 384 - rather than the short ones
 * tools/tests/gsp_fw_kernel_host.c uses to exercise the container's rules.  Each section's bytes are distinct
 * and the image's walk, so a payload that took the wrong section, or the right one from one byte off, cannot
 * pass a comparison by accident. */
#define IMAGE_BYTES 512u

static uint32_t build_fmc(uint8_t *p, uint32_t capacity, uint32_t hash, uint32_t key, uint32_t signature,
                          int unnamed)
{
    static const char *const want[] = { "hash", "publickey", "signature", "image" };
    static const uint8_t strings[] = "\0hash\0publickey\0signature\0image\0";
    uint32_t shoff = 52, shdrs = 6 * 40, strings_at = 52 + shdrs, strings_size = sizeof(strings);
    uint32_t size[4] = { hash, key, signature, IMAGE_BYTES };
    uint32_t at = strings_at + strings_size;

    if (capacity < strings_at + strings_size + hash + key + signature + IMAGE_BYTES) return 0;
    memset(p, 0, capacity);
    memcpy(p, fmc_header, sizeof fmc_header);
    memcpy(p + strings_at, strings, strings_size);
    for (unsigned i = 0; i < 4; i++) {
        uint32_t name = 0;
        uint8_t *shdr = p + shoff + 40 * (i + 2);
        for (uint32_t k = 1; k < strings_size; k++)
            if (!memcmp(&strings[k], want[i], strlen(want[i]) + 1)) { name = k; break; }
        if (unnamed == (int)i + 2) name = strings_size - 1;   /* a header pointing at the table's last NUL */
        h_put32(shdr, name);
        h_put32(shdr + 4, 1);                                 /* SHT_PROGBITS */
        h_put32(shdr + 8, 0xff00000au);                       /* the flags the FMC contract asks for */
        h_put32(shdr + 16, at);
        h_put32(shdr + 20, size[i]);
        memset(p + at, (uint8_t)(0x11 * (i + 1)), size[i]);
        if (i == 3) for (uint32_t k = 0; k < size[i]; k++) p[at + k] = (uint8_t)(k * 7u + 1u);
        h_put32(shdr + 28, gpu_firmware_crc32(p + at, size[i]));
        at += size[i];
    }
    {
        uint8_t *shdr = p + shoff + 40;                       /* the string table, index 1 */
        h_put32(shdr + 4, 3);                                 /* SHT_STRTAB */
        h_put32(shdr + 8, 0x20u);                             /* SHF_STRINGS */
        h_put32(shdr + 16, strings_at);
        h_put32(shdr + 20, strings_size);
    }
    return at;
}

static uint8_t staged_region[1u << 20];

/* Stage an FMC image as the boot stub would and let the kernel's own reader parse it, so what the handover is
 * built from is bytes that passed the real checks rather than a description of them. */
static void stage(uint32_t hash, uint32_t key, uint32_t signature, int unnamed, int bad_checksum)
{
    struct boot_handoff handoff;
    uint32_t length = build_fmc(staged_region, sizeof staged_region, hash, key, signature, unnamed);

    memset(&handoff, 0, sizeof handoff);
    handoff.firmware.address = (uintptr_t)staged_region;
    handoff.firmware.resident_bytes = sizeof staged_region;
    handoff.firmware.staged = 1;
    handoff.firmware.total_bytes = length;
    handoff.firmware.role[1].state = BOOT_FIRMWARE_STATE_STAGED;
    handoff.firmware.role[1].bytes = length;
    handoff.firmware.role[1].offset = 0;
    handoff.firmware.role[1].flags = BOOT_FIRMWARE_ELF32 | BOOT_FIRMWARE_FMC_IMAGE | BOOT_FIRMWARE_FMC_SECTIONS
                                   | BOOT_FIRMWARE_FMC_NAMED | BOOT_FIRMWARE_FMC_CRC;
    handoff.firmware.role[1].crc32 = gpu_firmware_crc32(staged_region, length) ^ (uint32_t)bad_checksum;
    gpu_firmware_note(&handoff);
}

/* One boot, staged and asked: the model is reset after the firmware record is written, because the record
 * belongs to the boot and the mailbox to the card, and a test that forgot the order would be testing a card
 * that had been unplugged and reinserted between the two. */
static void ask_with(unsigned mode, uint32_t hash, uint32_t key, uint32_t signature, int unnamed, int bad_crc)
{
    struct gpu_device device = nvidia_at();
    struct boot_handoff handoff;

    memset(&handoff, 0, sizeof handoff);
    stage(hash, key, signature, unnamed, bad_crc);
    model_reset(mode);
    gpu_fsp_note(&device, &handoff);
}

/* --------------------------------------------------------------- cases -------------------------------------------------- */

/* First, because it is the only case that has to be true before any image has been staged anywhere in this
 * process: with nothing on the boot volume there is nothing to describe, and a record left over from an
 * earlier boot in the same process would say otherwise. */
static void test_nothing_staged(void)
{
    struct gpu_device device = nvidia_at();
    struct boot_handoff handoff;

    memset(&handoff, 0, sizeof handoff);
    handoff.firmware.address = 0;
    handoff.firmware.resident_bytes = 0;
    model_reset(0);
    gpu_firmware_note(&handoff);
    gpu_fsp_note(&device, &handoff);
    check("a card that answers, with no firmware on the volume, is asked and reported all the same",
          fsp_state.attempted && fsp_state.mailbox_readable && fsp_state.queried && fsp_state.responded);
    check("and the handover is described by absence rather than invented",
          !fsp_state.fmc_seen && !fsp_state.staged && !fsp_state.cot_ready && !fsp_state.cot_defect);
    check("nothing is resident either, which is what the panel's firmware paragraph is for",
          !gpu_firmware_state()->resident && !gpu_firmware_state()->staged);
}

static void test_the_gates(void)
{
    struct boot_handoff empty;
    struct gpu_device device = nvidia_at();

    memset(&empty, 0, sizeof empty);
    model_reset(0);
    gpu_fsp_note(&(struct gpu_device){ .vendor = 0x1013, .bar = { MODEL_BAR0 } }, &empty);
    check("a card these offsets do not belong to is recognised as not being one",
          fsp_state.present && !fsp_state.nvidia && fsp_state.vendor == 0x1013);
    check("and nothing of it was mapped, read or written, because that is the whole point of the check",
          model.reads == 0 && model.writes == 0 && !fsp_state.mapped && !fsp_state.attempted);

    gpu_fsp_note(&(struct gpu_device){ .vendor = 0x10de }, &(struct boot_handoff){0});
    check("an NVIDIA function with no aperture says that it has none, rather than that it was not tried",
          fsp_state.nvidia && fsp_state.skipped == 1 && !fsp_state.attempted);
    check("and a function whose aperture is beyond what this kernel maps says that instead",
          !fsp_state.mapped && model.reads == 0);

    model_reset(M_MAP_REFUSE);
    gpu_fsp_note(&(struct gpu_device){ .vendor = 0x10de, .bar = { MODEL_BAR0 } }, &empty);
    check("a refused mapping is recorded as a refused mapping, and the probe never starts after it",
          !fsp_state.attempted && !fsp_state.mapped && fsp_state.present && fsp_state.nvidia);
    check("which the model agrees was never asked for a second time after the no", model.reads == 0);

    model_reset(M_REFUSE);
    gpu_fsp_note(&device, &empty);
    check("a mailbox that refuses while the FSP reports itself booted is the refused case, not the quiet one",
          fsp_state.booted && fsp_state.mapped && !fsp_state.mailbox_readable && !fsp_state.queried);
    check("and the refusals are counted where they happened, register by register",
          fsp_state.reads == model.reads && fsp_state.refusal_reads == model.refusals && model.refusals > 0);
    check("nothing whatsoever is written to a card that is refusing", model.writes == 0 && fsp_state.writes == 0);
    check("a scratch register that refuses is not polled to the limit: the wait stops at the refusal",
          fsp_state.boot_polls == 1);
    check("the coprocessor's own block is reported as still locked, which is the fact the panel leads with",
          !fsp_state.gsp_readable && fsp_state.hwcfg2 == 0xbadf4100u);

    model_reset(M_REFUSE | M_RELEASED);
    gpu_fsp_note(&device, &empty);
    check("and as readable the moment it answers, which is what a successful boot would change",
          fsp_state.gsp_readable && fsp_state.hwcfg2 == 0x123u);

    model_reset(M_NOT_BOOTED);
    gpu_fsp_note(&device, &empty);
    check("a mailbox that answers while the FSP is still booting is recorded as answering",
          fsp_state.mailbox_readable && !fsp_state.booted && fsp_state.boot_scratch == 0x12345600u);
    check("and nothing is sent, because a channel nobody is reading is not a question",
          !fsp_state.queried && model.writes == 0);
    check("the FSP's version and fuse check are read whether or not a request is possible",
          fsp_state.ucode_version == 42 && fsp_state.fuse_check == 0);
    check("and the wait for a boot that never happens is the bound the file says it is, not a hang",
          fsp_state.boot_polls == FSP_BOOT_POLL_LIMIT);

    model_reset(M_BUSY);
    ((uint32_t *)model_fsp)[R_QUEUE_TAIL] = 4;              /* a packet the FSP has not finished with */
    gpu_fsp_note(&device, &empty);
    check("a busy mailbox is waited for, and the wait is reported as the reason with its polling count",
          !fsp_state.queried && fsp_state.send_blocked == 1 && fsp_state.polled > 1);
    check("and the channel was never written while it was busy", model.writes == 0 && model.tail_writes == 0);
}

static void test_the_request_bytes(void)
{
    uint8_t buffer[128];
    uint8_t query = NVDM_TYPE_CLOCK_BOOST;
    const uint32_t *packet = model.packet;

    model_reset(0);
    map_mailbox_pages(MODEL_BAR0);
    check("the three windows are the ones the file says they are", w_therm.page && w_fsp.page && w_gsp.page);
    check("each addressed from its own base, and long enough to hold what lives at the far end of it",
          w_therm.base == MODEL_THERM && w_therm.length == 0x1000u && w_fsp.base == MODEL_FSP &&
          w_fsp.length >= 0x4000u && w_gsp.base == MODEL_GSP &&
          NV_PFSP_MSGQ_TAIL - MODEL_FSP + 4u <= w_fsp.length);

    check("a one-byte capability query is sent", send_packet(&query, 1, NVDM_TYPE_CAPS_QUERY) == 1);
    check("the transport header is version 1, both message flags set, and nothing else: no endpoint ids, "
          "no tag, no sequence, because this is the whole message", packet[0] == 0xc0000001u);
    check("the NVDM header is vendor-defined-PCI, NVIDIA's vendor id, and the capability-query type",
          packet[1] == ((uint32_t)NVDM_TYPE_CAPS_QUERY << 24 | 0x10deu << 8 | 0x7eu));
    check("the payload is the one byte a capability query carries, zero-padded up to a dword",
          (packet[2] & 0xffu) == NVDM_TYPE_CLOCK_BOOST && (packet[2] >> 8) == 0);
    check("the packet is twelve bytes, and the doorbell names the last dword written rather than the next",
          fsp_state.packet_bytes == 12 && fsp_state.doorbell == 8 && model.last_tail == 8);
    check("auto-increment was asked for the writes and not for reads, in the one control write",
          ((uint32_t *)model_fsp)[R_EMEMC] == (1u << 24));
    check("the channel offset walked by exactly the three dwords that were written", model.emem_dword == 3);
    check("the reply the model produced is pending, and reading it back needs the two pointers to differ",
          ((uint32_t *)model_fsp)[R_MSGQ_HEAD] == 0 && ((uint32_t *)model_fsp)[R_MSGQ_TAIL] == 24);
    check("so the wait sees it, and the read returns the whole packet",
          wait_for(reply_waiting, &fsp_state.reply_polls) &&
          read_reply(buffer, sizeof buffer, &fsp_state.query) == 1);
    check("28 bytes, both message flags set, framed as NVIDIA's",
          fsp_state.query.bytes == 28 && fsp_state.query.som == 1 && fsp_state.query.eom == 1 &&
          fsp_state.query.framed && fsp_state.query.delta_ok);
    check("the payload's own bytes are where the response format puts them",
          buffer[8] == NVDM_TYPE_FSP_RESPONSE && m_read(buffer + 9) == 7 &&
          m_read(buffer + 13) == NVDM_TYPE_CAPS_QUERY && m_read(buffer + 17) == 0 &&
          m_read(buffer + 21) == 0xdeadbeefu);
    check("and the reply queue was acknowledged by pulling the tail to the head, which frees the channel",
          model.reply_ack_writes == 2 && ((uint32_t *)model_fsp)[R_MSGQ_TAIL] == 0);

    /* A payload the size of a whole handover: the largest thing this file could ever be asked to put in the
     * channel, and the one where an off-by-one in the padding would be invisible in a 12-byte test. */
    model_reset(0);
    map_mailbox_pages(MODEL_BAR0);
    {
        uint8_t big[GPU_FSP_PAYLOAD_BYTES];
        for (unsigned i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 31u + 7u);
        check("a full handover-sized payload fits one packet and is sent as one",
              send_packet(big, sizeof big, NVDM_TYPE_COT) == 1);
        check("868 bytes of packet, doorbell at 864, and the channel holds all 860 bytes of payload",
              fsp_state.packet_bytes == 868 && fsp_state.doorbell == 864 && model.packet_bytes == 868);
        check("header dwords first, then the payload byte for byte, with no gap and no shift",
              memcmp(model.packet + 2, big, sizeof big) == 0);
        check("which is 217 dwords written at a walking offset", model.emem_dword == 2u + 860u / 4u);
    }
    model_reset(0);
    map_mailbox_pages(MODEL_BAR0);
    {
        uint8_t too_big[1017];
        unsigned before = model.writes;
        memset(too_big, 0xa5, sizeof too_big);
        check("1017 bytes of payload would overflow the channel, so it is refused before anything is written",
              send_packet(too_big, sizeof too_big, NVDM_TYPE_COT) == 0 && model.writes == before);
        check("and the refusal happens before the queue is even looked at, so send_blocked stays unwritten",
              fsp_state.send_blocked == 0 && model.tail_writes == 0);
        memset(too_big, 0xa5, sizeof too_big);
        too_big[1015] = 0x5a;                        /* one byte less is the largest packet the channel holds */
        check("1016 bytes plus the two header dwords is exactly the channel, and is sent",
              send_packet(too_big, 1016, NVDM_TYPE_COT) == 1 && fsp_state.doorbell == 1020);
    }
}

static void test_a_full_query_and_reply(void)
{
    model_reset(0);
    ask(0);
    check("the FSP that answers is answered: type, kind, task and command all read back",
          fsp_state.queried && fsp_state.responded && fsp_state.query.framed && fsp_state.query.complete);
    check("0x15 in the header and in the payload alike, and the request being answered named",
          fsp_state.query.type == NVDM_TYPE_FSP_RESPONSE && fsp_state.query.kind == NVDM_TYPE_FSP_RESPONSE &&
          fsp_state.query.command == NVDM_TYPE_CAPS_QUERY && fsp_state.query.task == 7);
    check("a zero error code is reported as a zero rather than as an absence of answer",
          fsp_state.query.error == 0 && fsp_state.responded);
    check("the reply was acknowledged on both pointers", model.reply_ack_writes == 2);
    check("the offset walked over the read as well, ending where 28 bytes of it says",
          fsp_state.query.delta_ok && model.emem_dword == 7);
    check("every read and write the counters claim is one the model served, and vice versa",
          fsp_state.reads == model.reads && fsp_state.writes == model.writes && model.writes == 9);
    check("and the only registers written are the channel's own: one control, three data, four pointers",
          model.tail_writes == 1);

    ask(M_NO_REPLY);
    check("a card that takes the request and never answers is reported as that, not as a failure to send",
          fsp_state.queried && !fsp_state.responded && !fsp_state.send_blocked && fsp_state.doorbell == 8);
    check("and the wait for its reply is counted as a wait for a reply, separately from the wait to send",
          fsp_state.reply_polls > 1 && fsp_state.polled > 0);

    ask(M_BAD_FRAME);
    check("a reply whose vendor id is somebody else's is not read as an answer to anything",
          !fsp_state.responded && fsp_state.query.bytes == 28 && !fsp_state.query.framed);

    ask(M_OVERSIZE);
    check("a reply longer than the caller's buffer is measured and left where it is",
          !fsp_state.responded && fsp_state.query.oversize == 200 && model.reply_ack_writes == 0);
    check("and the request was genuinely sent, which is why the reply is worth complaining about",
          model.tail_writes == 1);

    ask(M_NO_AINC);
    check("a channel whose offset does not walk stops the request before the doorbell, not after",
          fsp_state.send_blocked == 3 && !fsp_state.queried && model.tail_writes == 0);
    check("which is precisely the failure the walk-check exists for: three dwords written to one place",
          model.emem_dword == 0);
}

static void test_the_handover_payload(void)
{
    const struct gpu_firmware_image *image;
    const uint8_t *payload;

    ask_with(0, 48, 384, 384, -1, 0);
    image = &gpu_firmware_state()->image[1];
    payload = fsp_state.payload;
    check("the container was built to the size its sections add up to, and parsed as the FMC it is",
          image->fmc_template && image->elf32 && image->fmc_sections && image->fmc_named &&
          image->checksum_matches && !image->crc_failures);
    check("so the handover is staged and ready, with no defect to report",
          fsp_state.fmc_seen && fsp_state.staged && fsp_state.cot_ready && !fsp_state.cot_defect);
    check("the image was copied whole into pages this kernel asked for, and the payload points at that copy",
          fsp_state.image_bytes == IMAGE_BYTES && fsp_state.image_physical == (uintptr_t)model_pages &&
          memcmp(model_pages, staged_region + image->image_offset, IMAGE_BYTES) == 0);
    check("and it is zero-padded to a page boundary, which is what the page count was rounded up for",
          model_pages[IMAGE_BYTES] == 0 && model_pages[0xfff] == 0 && model_pages[0x1000] == 0);
    check("the payload's version and size are the two 16-bit words the published struct begins with, in "
          "that order and no wider",
          payload[COT_OFF_VERSION] == 2 && payload[COT_OFF_VERSION + 1] == 0 &&
          (uint32_t)(payload[COT_OFF_SIZE] | (payload[COT_OFF_SIZE + 1] << 8)) == COT_SIZE &&
          payload[COT_OFF_SIZE + 1] == 3 && COT_SIZE == GPU_FSP_PAYLOAD_BYTES);
    check("the resume store is described as absent, which is how a machine with nothing to resume is encoded",
          m_read(payload + COT_OFF_FRTS_SYSMEM) == 0 && m_read(payload + COT_OFF_FRTS_SYSMEM + 4) == 0 &&
          *(const uint32_t *)(payload + COT_OFF_FRTS_SYSMEM_SIZE) == 0 &&
          m_read(payload + COT_OFF_FRTS_VIDMEM) == 0 && *(const uint32_t *)(payload + COT_OFF_FRTS_VIDMEM_SIZE) == 0);
    check("the boot arguments are ~0, which is the cold-boot encoding, and they end where the payload does",
          m_read(payload + COT_OFF_BOOT_ARGS) == ~(uint32_t)0 &&
          m_read(payload + COT_OFF_BOOT_ARGS + 4) == ~(uint32_t)0 &&
          COT_OFF_BOOT_ARGS + 8u == GPU_FSP_PAYLOAD_BYTES);
    check("the three authentication blobs are the container's own sections, at the published offsets",
          memcmp(payload + COT_OFF_HASH, staged_region + image->hash_offset, 48) == 0 &&
          COT_OFF_HASH + 48u == COT_OFF_PUBLICKEY &&
          memcmp(payload + COT_OFF_PUBLICKEY, staged_region + image->publickey_offset, 384) == 0 &&
          COT_OFF_PUBLICKEY + 384u == COT_OFF_SIGNATURE &&
          memcmp(payload + COT_OFF_SIGNATURE, staged_region + image->fmc_signature_offset, 384) == 0);
    check("the CRC32 recorded is of the image as the card will read it: over the copy's 512 bytes, not over "
          "the page of padding that follows them",
          fsp_state.image_crc32 == gpu_firmware_crc32(model_pages, IMAGE_BYTES) &&
          fsp_state.image_crc32 != gpu_firmware_crc32(model_pages, 0x1000u));
    check("the query's own size is kept as the query's, not overwritten by the second message's: twelve bytes "
          "went out for the query and 868 for the handover, and the panel quotes the first in its sentence",
          fsp_state.query.request_bytes == 12 && fsp_state.handover.request_bytes == FSP_HEADER_BYTES + COT_SIZE &&
          fsp_state.packet_bytes == FSP_HEADER_BYTES + COT_SIZE);
    check("because the card answered that query cleanly, the handover left too: the model took a second packet, "
          "and it is the driver's payload with nothing changed in it",
          model.cot_seen == 1 && model.cot_bytes == FSP_HEADER_BYTES + COT_SIZE &&
          memcmp((const uint8_t *)model.cot + FSP_HEADER_BYTES, fsp_state.payload, COT_SIZE) == 0);
    check("which is the sentence that has to be true before any of this is sent to real silicon: the bytes the "
          "model rejected or accepted are byte for byte the bytes the panel describes",
          fsp_state.handover_attempted && fsp_state.handover_sent && !fsp_state.handover_blocked &&
          fsp_state.handover.bytes == 28 && fsp_state.handover.complete &&
          fsp_state.handover.command == NVDM_TYPE_COT && fsp_state.handover.error == 0);
    check("and the model's verdict on the payload is that it was a real one, so the acceptance below is a "
          "response to a well-formed handover and not a model that said yes to anything",
          model.cot_shape_ok && model.cot_target_ok && model.cot_blobs_ok);
    check("the release is then measured, not inferred: the capability word that was 0xbadf4100 before the "
          "message answers after it, because the model changes it only when it accepts an image",
          FSP_NOT_CONTENT(fsp_state.hwcfg2) && fsp_state.hwcfg2_after == 0x123u && fsp_state.gsp_released);
    check("the queue fell idle again after the exchange, which is what the next message would need",
          fsp_state.queues_idle_after);

    ask_with(0, 32, 384, 384, -1, 0);
    check("an FMC whose hash section is not 48 bytes is refused a payload, and told which section it was",
          fsp_state.fmc_seen && !fsp_state.staged && fsp_state.cot_defect == 1);
    ask_with(0, 48, 383, 384, -1, 0);
    check("the same for a public key one byte short of the 384 the payload has room for",
          fsp_state.cot_defect == 2);
    ask_with(0, 48, 384, 512, -1, 0);
    check("and for a signature of a size the format has no field for", fsp_state.cot_defect == 3);
    ask_with(0, 48, 384, 384, 2, 0);
    check("a container with the hash section's name pointing elsewhere has no image to describe",
          fsp_state.cot_defect == 8);
    ask_with(M_NO_PAGES, 48, 384, 384, -1, 0);
    check("and running out of memory for the copy is its own reason rather than a silent skip",
          fsp_state.fmc_seen && !fsp_state.staged && fsp_state.cot_defect == 6);

    ask_with(0, 48, 384, 384, -1, 1);
    check("bytes that no longer match the checksum the stub recorded are seen, but not trusted",
          fsp_state.fmc_seen && !fsp_state.staged && fsp_state.cot_defect == 7);

    /* The modelled FSP is the only thing in this file a driver could be wrong about, so the driver's own
     * verdict on the packet it wrote is checked against the model's: if they ever disagree, one of them has
     * been changed to agree with the other, and that is the moment this test stops being worth anything. */
    model_reset(0);
    map_mailbox_pages(MODEL_BAR0);
    {
        uint8_t query = NVDM_TYPE_CLOCK_BOOST;
        check("a packet both agree on", send_packet(&query, 1, NVDM_TYPE_CAPS_QUERY) == 1 &&
              model.framing_ok && fsp_state.packet_bytes == 12);
    }

    /* And a re-note with nothing staged has to retract the record, not keep the last one it was given. */
    {
        struct boot_handoff empty;
        struct gpu_device device = nvidia_at();
        memset(&empty, 0, sizeof empty);
        gpu_firmware_note(&empty);
        model_reset(0);
        gpu_fsp_note(&device, &empty);
        check("a boot that staged nothing clears what an earlier one staged, rather than inheriting it",
              !fsp_state.fmc_seen && !fsp_state.staged);
    }
}

/* What the panel says when the card takes the image and when it does not, and when it says nothing at all.
 * These three are the whole difference between a port that has been tried and one that has only been built, so
 * each is a separate boot of the model rather than three assertions on one. */
static void test_the_handover_answers(void)
{
    ask_with(M_COT_WPR, 48, 384, 384, -1, 0);
    check("a card that reads the image and rejects it is reported as having read and rejected it, with the "
          "falcon's own name for that answer",
          fsp_state.handover_sent && fsp_state.handover.error == 0xdfu && fsp_state.handover.complete &&
          !fsp_state.gsp_released && fsp_state.hwcfg2_after == 0xbadf4100u);
    check("and the name the panel prints is the published one for 0xdf rather than a paraphrase of it",
          gpu_fsp_flcn_name(0xdfu) && !strcmp(gpu_fsp_flcn_name(0xdfu), "FLCN_ERR_FMC_BOOT_FROM_INVALID_WPR") &&
          !strcmp(gpu_fsp_flcn_name(0u), "FLCN_OK") && gpu_fsp_flcn_name(0x9eu) == 0);
    check("the lock it did not lift is still the locked-target code, so nothing on this path claims a change",
          FSP_NOT_CONTENT(fsp_state.hwcfg2_after));

    ask_with(M_COT_SILENT, 48, 384, 384, -1, 0);
    check("a card that takes the handover and answers nothing is reported as exactly that: sent, and no reply",
          fsp_state.handover_sent && fsp_state.handover.bytes == 0 && fsp_state.handover.complete == 0 &&
          fsp_state.hwcfg2_after == 0 && !fsp_state.gsp_released);

    ask_with(M_BUSY, 48, 384, 384, -1, 0);
    check("a card still holding the queue open from the query is not sent a second message, and the reason is "
          "kept rather than flattened into a silence",
          !fsp_state.handover_sent && fsp_state.handover_attempted == 0);
}

/* The two things a submission needs, measured where this generation publishes them.  The legacy user-mode page
 * is not consulted by this file at all: an earlier build drew a conclusion about the card's clock from that
 * page, and these checks are the boundary against doing that again. */
static void test_the_clock_and_the_doorbell_page(void)
{
    ask(0);
    check("the timer block answers and moves, because the model's clock steps under every read",
          fsp_state.timer_readable && !fsp_state.timer_dead && !fsp_state.timer_refused &&
          fsp_state.timer_ticking && fsp_state.timer_end > fsp_state.timer_start);
    check("and the virtual-function page is there and empty, which is said as that and not as a refusal",
          fsp_state.vf_readable && fsp_state.vf_answered == 0 && fsp_state.vf_refusals == 0);

    ask(M_VF_LIVE);
    check("a page that answers with content is counted as content: six reads, six of them the chip's",
          fsp_state.vf_answered == 6 && fsp_state.vf_refusals == 0 && fsp_state.vf_leaf0 == 0x1u &&
          fsp_state.vf_top0 == 0x2u && fsp_state.vf_scratch[3] == 0xa0a0a0a3u);

    ask(M_TIMER_FROZEN);
    check("a block that answers the same value twice is reported as not moving, and that is its own sentence",
          fsp_state.timer_readable && !fsp_state.timer_ticking && !fsp_state.timer_dead &&
          !fsp_state.timer_refused && fsp_state.timer_start == fsp_state.timer_end &&
          fsp_state.timer_start == 0x1234u);

    ask(M_TIMER_DEAD);
    check("all-ones at both halves is the block not being decoded, kept apart from a block that is silent "
          "because it refused",
          fsp_state.timer_dead && !fsp_state.timer_ticking && !fsp_state.timer_refused);

    ask(M_REFUSE);
    check("and a refusal is read as a refusal: the family test catches 0xbadf4100 the way the vendor's driver "
          "does, so a fault is never printed as a measurement of a clock",
          fsp_state.timer_readable && fsp_state.timer_refused && !fsp_state.timer_ticking &&
          fsp_state.vf_readable && fsp_state.vf_refusals == 6 && fsp_state.vf_answered == 0);

    ask(M_MAP_REFUSE);
    check("a window this kernel could not map is its own answer, not the chip's: nothing is claimed about "
          "either page",
          !fsp_state.timer_readable && !fsp_state.vf_readable && !fsp_state.timer_ticking &&
          !fsp_state.vf_answered && fsp_state.vf_refusals == 0);
}

int main(void)
{
    test_nothing_staged();
    test_the_gates();
    test_the_request_bytes();
    test_a_full_query_and_reply();
    test_the_handover_payload();
    test_the_handover_answers();
    test_the_clock_and_the_doorbell_page();
    printf("%u check(s), %u failure(s)\n", checks_run, failures_run);
    return failures_run != 0;
}

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

#define MODEL_THERM   0xad0000u
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
#define M_NO_IDENTITY 32768u /* even NV_PMC_BOOT_0 refuses: nobody can say which chip this is */
#define M_SCRATCH_REFUSES 65536u /* the boot-complete scratch itself answers the locked-target refusal, which is
                                  * what a real GB207 did: BAR0+0xad00bc unreadable while the FSP was booting */
#define M_EMEM_JUNK  131072u  /* the channel takes the writes and hands back something else - the shape the GB207
                               * reported, where the read-back differed at dword 0 on a packet that landed */
#define M_DROP_TAIL  262144u  /* the command queue's tail register accepts the store and reads back 0, which is a
                               * doorbell the card never heard */

static uint8_t model_therm[0x1000];
static uint8_t model_fsp[0x4000];
static uint8_t model_gsp[0x1000];
static uint8_t model_vf[0x3000];                      /* NV_VIRTUAL_FUNCTION_PRIV, BAR0+0x0..0x2fff */

/* The architecture the modelled chip reports, which decides the sizes its FSP wants: 0x1b is the GB200
 * family's number, and 0x1b7000a1 is the dword a GB207 answers with.  A fixture that wants another
 * generation changes this before it asks, which is how one machine's answers become another's. */
static uint32_t model_arch_boot0 = 0x1b7000a1u;
static uint8_t model_timer[0x1000];                   /* NV_PTIMER, BAR0+0x9000..0x9fff */
static uint8_t model_channel[FSP_EMEM_CHANNEL_BYTES];
/* The channel's window, as the hardware describes it: a byte offset put in the control register, a direction
 * bit, and a counter that wraps at 256 bytes.  `emem_walk' is that counter, counted in dwords because every
 * access moves it by one; it is six bits wide, which is the whole reason a transfer longer than 0x100 bytes
 * has to be re-armed - and the reason a driver that arms once and writes 217 dwords sends a packet whose
 * second half has been written over its first.  The reference driver's own port declares `max = 0x100'
 * (nvkm/falcon/gp102.c) and its transfer loop re-arms per chunk, which is what the file under test now does. */
#define MODEL_EMEM_WINDOW_DWORDS 64u
/* Handed out in successive page-aligned chunks, because one boot really does take several allocations - the
 * FMC copy, the radix-3 tree, the two descriptor blocks - and a model that returned the same base for every
 * one of them would let the driver pass by writing its metadata on top of the image it was told to point at. */
static uint8_t model_pages[1u << 20] __attribute__((aligned(4096)));   /* the FMC copy, its radix tree,
                                                                          * the boot parameters and the four
                                                                          * LibOS regions: 0x30000 of logs alone */
static size_t model_pages_used;

struct model {
    unsigned mode;
    unsigned emem_base;                           /* the byte offset the window was last armed at */
    unsigned emem_walk;                           /* dwords since it was armed, wrapping at the window */
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

/* The offset the next window access lands on, as the chip computes it: the armed base plus the counter, and the
 * counter is truncated to the window.  This is what a model of the hardware has to do rather than what a model
 * of the driver would like: the overflow is the bug the file under test used to have. */
static unsigned emem_current(void)
{
    unsigned walking = model.ainc_write || model.ainc_read;
    return (model.emem_base / 4u) + (walking ? (model.emem_walk % MODEL_EMEM_WINDOW_DWORDS) : 0u);
}

static uint32_t ememc_value(void)
{
    uint32_t v = model.emem_base | ((model.emem_walk % MODEL_EMEM_WINDOW_DWORDS) << 2);
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

    /* What an FSP would check, at the widths *its* chip reports: version 2 and 48/97/96 for the GB200
     * family this model answers as, version 1 and 48/384/384 for Hopper.  Everything past the generation's
     * width has to still be zero, because a driver that copied the full 384-byte field into a container whose
     * key section is 97 bytes long would be describing bytes the file does not have - and a model that only
     * checked the front of the field would let that pass. */
    {
        uint32_t arch = (model_arch_boot0 >> 24) & 0x1fu;
        uint32_t want_version = (arch == 0x1au || arch == 0x1bu) ? 2u : 1u;
        uint32_t width[3] = { 48u, want_version == 2u ? 97u : 384u, want_version == 2u ? 96u : 384u };
        uint32_t off[3] = { COT_OFF_HASH, COT_OFF_PUBLICKEY, COT_OFF_SIGNATURE };
        uint32_t field[3] = { 48u, 384u, 384u };             /* hash384[12], publicKey[96], signature[96] */

        model.cot_shape_ok = bytes >= FSP_HEADER_BYTES + COT_SIZE &&
                             (m_read(payload) & 0xffffu) == want_version &&
                             ((m_read(payload) >> 16) & 0xffffu) == COT_SIZE &&
                             m_read(payload + COT_OFF_BOOT_ARGS) == ~(uint32_t)0;
        model.cot_blobs_ok = 1;
        for (unsigned which = 0u; which < 3u; which++)
            for (unsigned i = 0u; i < field[which]; i++) {   /* the published field's own width, not the copy's */
                /* Inside the width the generation asked for there has to be *something* - the fixture fills
                 * each section with a walking pattern, so equality against a fixed byte is not the test - and
                 * outside it there has to be nothing, which is the half that catches a driver copying a whole
                 * 384-byte field from a container whose section is 97 bytes long.  The byte-for-byte identity
                 * of what was copied is checked against the staged image itself, below. */
                if (i < width[which] && payload[off[which] + i] == 0u) model.cot_blobs_ok = 0;
                /* A field wider than this generation's blob has to be zero for the rest of its width - except
                 * the hash field, which is exactly 48 bytes on both rows of the table and so has no tail
                 * inside the struct at all: the next field starts where it stops. */
                if (i >= width[which] && field[which] != width[which] &&
                    payload[off[which] + i] != 0u) model.cot_blobs_ok = 0;
            }
    }
    model.cot_target_ok = m_read(payload + COT_OFF_FMC_ADDRESS) == (uint32_t)(uintptr_t)model_pages &&
                          m_read(payload + COT_OFF_FMC_ADDRESS + 4) == (uint32_t)(((uintptr_t)model_pages) >> 32) &&
                          ((uintptr_t)model_pages & 0xfffu) == 0;
    /* Not the 46-bit rule the driver is held to when it asks for the pages: a host allocation is not a
     * physical address, and the limit is enforced where the pages are requested and tested there (defect 6),
     * so repeating it here would compare a machine's memory map against an array's link address. */

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
    if ((model.mode & M_REFUSE) && which != 0 && !(which == 2 && (model.mode & M_RELEASED)) &&
        /* A chip whose coprocessor block is locked still answers NV_PMC_BOOT_0 - that is the machine this
         * driver exists for, and the panel of a real one says so: the boot register reads 0x1b7000a1 while
         * every GSP offset answers 0xbadf4100.  Refusing the identity as well would model a powered-down
         * function, which is a different fixture (M_NO_IDENTITY) and a different decision. */
        !(which == 3 && index == 0 && !(model.mode & M_NO_IDENTITY))) {
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
        value = (model.mode & M_EMEM_JUNK) ? 0x00000020u : channel_dword(emem_current());
        if (model.ainc_read && !(model.mode & M_NO_AINC)) model.emem_walk++;
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
        /* Arming the window: the byte offset in the low bits, the direction in bit 24 or 25, and the counter
         * back to zero.  A window that is not walked (M_NO_AINC) still takes the arming write, it just never
         * moves afterwards, which is the failure this mode models. */
        model.emem_base = value & 0xffffu & ~3u;
        model.emem_walk = 0;
        model.ainc_write = (value & (1u << 24)) != 0;
        model.ainc_read = (value & (1u << 25)) != 0;
        return;
    }
    if (which == 1 && index == R_EMEMD) {
        channel_put_dword(emem_current(), value);
        if (model.ainc_write && !(model.mode & M_NO_AINC)) model.emem_walk++;
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
        /* A tail the register will not hold: the store is taken and read back as zero, so the packet is in the
         * channel and the doorbell that would have announced it never happened. */
        if (model.mode & M_DROP_TAIL) { page_of(which)[index] = 0; model.tail_seq--; }
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
    uintptr_t at;

    if (model.mode & M_NO_PAGES) return 0;
    if (limit < 0x10000ull) return 0;
    if (model_pages_used + count * 0x1000u > sizeof model_pages) return 0;
    at = (uintptr_t)model_pages + model_pages_used;
    model_pages_used += count * 0x1000u;
    return at;
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

/* What a driver module read off the chip as its video memory, or 0 when nothing did: the one input the
 * reservation cannot be computed without, and the reason the layout is a function of it. */
static uint64_t model_vram;
static void model_reset(unsigned mode)
{
    memset(&fsp_state, 0, sizeof fsp_state);
    model_vram = 0;
    memset(model_therm, 0, sizeof model_therm);
    memset(model_fsp, 0, sizeof model_fsp);
    memset(model_gsp, 0, sizeof model_gsp);
    memset(model_vf, 0, sizeof model_vf);
    memset(model_timer, 0, sizeof model_timer);
    memset(&w_vf, 0, sizeof w_vf);
    memset(&w_timer, 0, sizeof w_timer);
    memset(model_channel, 0, sizeof model_channel);
    memset(model_pages, 0, sizeof model_pages);
    model_pages_used = 0;
    if (mode & M_VF_LIVE) {
        h_put32(model_vf + 0x1000u, 0x1u);                 /* an interrupt leaf with something pending */
        h_put32(model_vf + 0x1600u, 0x2u);                 /* and the top node that routes it */
        for (unsigned i = 0; i < 4u; i++) h_put32(model_vf + 0x2100u + i * 4u, 0xa0a0a0a0u + i);
    }
    if (mode & M_TIMER_FROZEN) h_put32(model_timer + 0x400u, 0x1234u);
    h_put32(model_vf + 0x0u, model_arch_boot0);        /* NV_PMC_BOOT_0, at the first dword of the aperture */
    memset(&model, 0, sizeof model);
    model.mode = mode;
    *(uint32_t *)(model_therm + T_BOOT_COMPLETE * 4u) = (mode & M_SCRATCH_REFUSES) ? 0xbadf1100u
        : (mode & M_NOT_BOOTED) ? 0x12345600u : 0xffu;
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

/* An FMC image at the sizes the modelled generation wants - 48, 97 and 96 for a GB200-family chip, which
 * are *not* the sizes the payload's fields are wide enough for - rather than the short ones
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
        h_put32(shdr + 8, 0xfff00102u);                       /* SHF_MASKPROC|SHF_MASKOS|SHF_OS_NONCONFORMING|SHF_ALLOC,
                                                             exactly what a real fmc-*.bin carries */
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
/* The resource-manager image, as the boot stub leaves it: an ELF64 whose section table names a .fwimage payload
 * and a .fwsignature_gb20x beside it, which is all r535_gsp_elf_section() asks of the file and all the
 * metadata block under test needs to be pointed at.  The two sizes are deliberately unequal and the payload is
 * filled with a ramp, so an address off by one - or payload and signature swapped - fails a byte comparison
 * rather than a size check that both numbers happen to pass. */
static uint32_t build_elf64_rm(uint8_t *p, uint32_t capacity, uint32_t payload_size, uint32_t signature_size)
{
    static const char names[] = "\0.fwimage\0.fwsignature_gb20x\0.shstrtab\0";
    const uint32_t names_at = 64u + 4u * 64u, payload_at = 1024u;
    uint32_t total = payload_at + payload_size + signature_size;

    if (capacity < total) return 0;
    memset(p, 0, total);
    p[0] = 0x7f; p[1] = 'E'; p[2] = 'L'; p[3] = 'F'; p[4] = 2; p[5] = 1; p[6] = 1;
    p[16] = 2; p[17] = 0;                      /* e_type: ET_EXEC */
    p[18] = 0x3e; p[19] = 0;                    /* e_machine: x86-64, which nothing here reads */
    p[40] = 64;                                 /* e_shoff, little-endian 64-bit */
    p[58] = 64; p[59] = 0;                      /* e_shentsize */
    p[60] = 4; p[61] = 0;                       /* e_shnum */
    p[62] = 3; p[63] = 0;                       /* e_shstrndx */
    memcpy(p + names_at, names, sizeof names);
    /* [1] .fwimage, [2] .fwsignature_gb20x, [3] .shstrtab.  sh_name at 0, sh_offset at 24, sh_size at 32. */
    for (unsigned i = 0; i < 3; i++) {
        uint8_t *shdr = p + 64u + (i + 1u) * 64u;
        uint32_t name = i == 0 ? 1u : (i == 1 ? 10u : 30u);
        uint32_t off = i == 0 ? payload_at : (i == 1 ? payload_at + payload_size : names_at);
        uint32_t size = i == 0 ? payload_size : (i == 1 ? signature_size : (uint32_t)sizeof names);
        shdr[0] = (uint8_t)name; shdr[1] = (uint8_t)(name >> 8);
        shdr[24] = (uint8_t)off; shdr[25] = (uint8_t)(off >> 8); shdr[26] = (uint8_t)(off >> 16); shdr[27] = (uint8_t)(off >> 24);
        shdr[32] = (uint8_t)size; shdr[33] = (uint8_t)(size >> 8); shdr[34] = (uint8_t)(size >> 16); shdr[35] = (uint8_t)(size >> 24);
    }
    for (uint32_t i = 0; i < payload_size; i++) p[payload_at + i] = (uint8_t)(i * 7u + 3u);
    for (uint32_t i = 0; i < signature_size; i++) p[payload_at + payload_size + i] = (uint8_t)(0xa0u + i);
    return total;
}

/* Set by the one scenario that has an RM image on the volume, so every other case keeps staging exactly the
 * FMC-only record a GB207 with no gsp-*.bin installed actually produces. */
static int stage_rm;
/* Five pages, so that the bottom level of the radix-3 tree has five entries and an off-by-one page cannot
 * survive the comparison below; 97 bytes of signature because that is the width the FMC's own key section has
 * and a payload and a signature of equal size would let one be mistaken for the other. */
static uint32_t rm_payload_size = 5u * 4096u, rm_signature_size = 97u;

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
    if (stage_rm) {
        /* The RM image and the bootloader go in after the FMC, page separated, because the region is the one
         * the driver is told about and an offset the driver invents has to be caught by the record agreeing
         * with it.  The bootloader is a header and nothing else: the metadata is meant to name where it is, and
         * no section of it is read by the code under test. */
        uint32_t at = (length + 4095u) & ~4095u;
        uint32_t rm = build_elf64_rm(staged_region + at, sizeof staged_region - at,
                                     rm_payload_size, rm_signature_size);
        handoff.firmware.role[0].state = BOOT_FIRMWARE_STATE_STAGED;
        handoff.firmware.role[0].offset = at;
        handoff.firmware.role[0].bytes = rm;
        handoff.firmware.role[0].crc32 = gpu_firmware_crc32(staged_region + at, rm);
        at += (rm + 4095u) & ~4095u;
        staged_region[at] = 0x7f; staged_region[at + 1] = 'E';
        staged_region[at + 2] = 'L'; staged_region[at + 3] = 'F';
        staged_region[at + 4] = 2; staged_region[at + 5] = 1;
        handoff.firmware.role[2].state = BOOT_FIRMWARE_STATE_STAGED;
        handoff.firmware.role[2].offset = at;
        handoff.firmware.role[2].bytes = 64u;
        handoff.firmware.role[2].crc32 = gpu_firmware_crc32(staged_region + at, 64u);
    }
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
    device.vram_bytes = model_vram;         /* whatever the module read, or nothing - read before the
                                             * reset, which clears the model's state and not the card's */
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
    check("a refusing card is written to exactly once, by the probe that asks whether the queue takes stores",
          model.writes == 2 && fsp_state.writes == 2 && !fsp_state.queue_writable && fsp_state.queue_probed);
    check("and the probe is recorded as having run, because the panel used to describe its result on cards where "
          "it never did: the queue registers read fine there, so nothing was written to them",
          fsp_state.queue_probed == 1);
    check("and this scratch says 0xFF, so the wait for it ends at the first look rather than at a bound",
          fsp_state.boot_polls == 1);
    check("the coprocessor's own block is reported as still locked, which is the fact the panel leads with",
          !fsp_state.gsp_readable && fsp_state.hwcfg2 == 0xbadf4100u);

    model_reset(M_REFUSE | M_RELEASED);
    gpu_fsp_note(&device, &empty);
    check("and as readable the moment it answers, which is what a successful boot would change",
          fsp_state.gsp_readable && fsp_state.hwcfg2 == 0x123u);
    /* The architecture gate, which decides everything downstream: the sizes a container is judged against are
     * a per-generation fact, so the first thing this path does is ask the chip which generation it is. */
    check("the identity is read even while the coprocessor's block refuses, and it selects this chip's row of "
          "the table - version 2 and 48/97/96 for a GB200-family part",
          fsp_state.fsp_supported && fsp_state.arch == 0x1bu && fsp_state.cot_version == 2u &&
          fsp_state.cot_hash_bytes == 48u && fsp_state.cot_pkey_bytes == 97u && fsp_state.cot_sig_bytes == 96u);
    check("and in the released state it is the bit that says so, not a word that happened to be non-zero",
          fsp_state.lockdown == 0 && fsp_state.gsp_released && fsp_state.hwcfg2 == 0x123u);

    model_reset(M_REFUSE | M_NO_IDENTITY);
    gpu_fsp_note(&device, &empty);
    check("a chip that will not say which chip it is is sent nothing at all: the width its FSP expects for a "
          "public key is unknown, and an unknown expectation is not a licence to assume one",
          fsp_state.arch_unknown && !fsp_state.fsp_supported && fsp_state.cot_defect == 9 &&
          !fsp_state.queried && !fsp_state.staged && fsp_state.writes == 0 && model.writes == 0);

    model_reset(M_NOT_BOOTED);
    gpu_fsp_note(&device, &empty);
    check("a mailbox that answers while the FSP is still booting is recorded as answering",
          fsp_state.mailbox_readable && !fsp_state.booted && fsp_state.boot_scratch == 0x12345600u);
    /* The contract changed here, on the strength of a real card's paste rather than of a preference.  The boot
     * scratch read all four zeros and never ticked on a GB207 whose queue answered and reported itself idle,
     * because that window is decoupled from CPU reads - so a rule that required the scratch before writing was
     * a rule that guaranteed writes 0 forever.  The idle head/tail pair is the condition the reference driver
     * uses before it stores QUEUE_TAIL, so that is the condition, and the scratch stays a reported fact. */
    check("and a request is sent when the queue is idle, even while the boot scratch has not agreed",
          fsp_state.queried && model.writes > 0);
    check("the FSP's version and fuse check are read whether or not a request is possible",
          fsp_state.ucode_version == 42 && fsp_state.fuse_check == 0);
    /* Two bounds exist on a wait and the file states both: the count of reads, and - when the device's own
     * clock could be read - the milliseconds the reference driver allows.  Either may end it first, and what
     * must never happen is a third way to stop, so the check is that one of the two stated bounds was the
     * one reached rather than that a particular number was. */
    check("and the wait for a boot that never happens is the bound the file says it is, not a hang",
          fsp_state.boot_polls == FSP_POLL_LIMIT || fsp_state.boot_ms_used >= FSP_WAIT_BOOT_MS);

    /* The register and the wait that a real card paid for this file to have: on the GB207 the boot scratch
     * answered 0xbadf1100 and the whole four-second wait collapsed into one read, because a refusal had been
     * taken as the answer.  Here it must not be - a window that is not decoded yet and a coprocessor that has
     * not finished booting are one event seen from outside, and the only way to tell them apart is to look
     * again at the reference driver's cadence for the driver's whole budget, which is what these two checks pin. */
    model_reset(M_NOT_BOOTED | M_SCRATCH_REFUSES);
    gpu_fsp_note(&device, &empty);
    check("a scratch register that refuses is polled out to one of the two stated bounds, not read once as a "
          "verdict: the wait is a wait, not a look",
          !fsp_state.booted && fsp_state.boot_scratch == 0xbadf1100u && fsp_state.refusal_reads > 1 &&
          (fsp_state.boot_polls > 1 || fsp_state.boot_ms_used >= FSP_WAIT_BOOT_MS) &&
          (fsp_state.boot_polls == FSP_POLL_LIMIT || fsp_state.boot_ms_used >= FSP_WAIT_BOOT_MS));
    check("and the request still goes out, because waiting for a card that has not unlocked the door is a way "
          "to learn nothing at all",
          fsp_state.queried && fsp_state.send_blocked != 1);

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
          "no tag, no sequence, and no version - the reference leaves bits 3:0 clear", packet[0] == 0xc0000000u);
    check("the NVDM header is vendor-defined-PCI, NVIDIA's vendor id, and the capability-query type",
          packet[1] == ((uint32_t)NVDM_TYPE_CAPS_QUERY << 24 | 0x10deu << 8 | 0x7eu));
    check("the payload is the one byte a capability query carries, zero-padded up to a dword",
          (packet[2] & 0xffu) == NVDM_TYPE_CLOCK_BOOST && (packet[2] >> 8) == 0);
    check("the packet is twelve bytes, and the doorbell names the last dword written rather than the next",
          fsp_state.packet_bytes == 12 && fsp_state.doorbell == 8 && model.last_tail == 8);
    check("the packet is in the channel at three distinct offsets, header first and payload after it",
          model.emem_base == 0 && model.emem_walk == 3 && model.packet[0] == 0xc0000000u &&
          (model.packet[2] & 0xffu) == NVDM_TYPE_CLOCK_BOOST);
    check("and the last arming of the window was a read of it, which is the read-back that proved the write",
          model.ainc_read && ((uint32_t *)model_fsp)[R_EMEMC] == (1u << 25));
    check("the reply the model produced is pending, and reading it back needs the two pointers to differ",
          ((uint32_t *)model_fsp)[R_MSGQ_HEAD] == 0 && ((uint32_t *)model_fsp)[R_MSGQ_TAIL] == 24);
    check("so the wait sees it, and the read returns the whole packet",
          wait_for(reply_waiting, &fsp_state.reply_polls) &&
          read_reply(buffer, sizeof buffer, &fsp_state.query) == 1);
    check("28 bytes, both message flags set, framed as NVIDIA's",
          fsp_state.query.bytes == 28 && fsp_state.query.som == 1 && fsp_state.query.eom == 1 &&
          fsp_state.query.framed && fsp_state.query.readback_ok);
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
        /* The bug this file was carrying, stated as a measurement: 868 bytes is more than three windows of
         * 256, so a single arming would have wrapped the counter and written the last 100 bytes over the
         * front of the packet.  Four bursts is what the transfer needs, and the packet is intact - which is
         * only true together. */
        check("868 bytes needs four windows, and the driver says so", fsp_state.send_bursts == 4);
        check("which is why the packet is still there: 217 dwords into a 64-dword window, in four arming writes",
              model.emem_walk == 217u % MODEL_EMEM_WINDOW_DWORDS &&
              memcmp(model.packet + 2, big, sizeof big) == 0);
        check("and the read-back that proves it ran, from the channel, before the doorbell",
              fsp_state.send_readback && fsp_state.send_mismatch == ~0u);
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
    check("the reply was read out and read again, and the two agreed: seven distinct dwords of the channel",
          fsp_state.query.readback_ok && model.emem_walk == 7);
    check("every read and write the counters claim is one the model served, and vice versa",
          fsp_state.reads == model.reads && fsp_state.writes == model.writes);
    check("and the writes are exactly the ones the request needs: the probe's two, the arming and three data "
          "words, the read-back's arming, the doorbell pair, and the reply's acknowledgement",
          model.writes == 2u + (1u + 3u + 1u + 2u) + 2u);
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
    /* A channel whose window never walks stores every dword of the packet on top of the first one.  The
     * read-back is what makes that visible; it is reported and not obeyed, because a GB207 measured here does
     * the same thing to a 4-dword query that it accepts and answers, so a host that vetoed the send on the
     * mismatch would never ring the doorbell on that card.  The measurement is the fact; the doorbell goes out
     * anyway, and whether the card took the packet is answered by the card. */
    check("a window that does not walk is caught by the read-back rather than by a counter",
          !fsp_state.send_readback && fsp_state.send_mismatch != ~0u && fsp_state.send_mismatch != UINT32_MAX);
    check("and the mismatch is reported at the dword it started, without a veto code",
          fsp_state.send_blocked == 0);
    check("and the doorbell goes out anyway, because not reading back is not the card refusing the packet",
          fsp_state.queried && model.tail_writes == 1 && fsp_state.doorbell == 8);
}

static void test_the_handover_payload(void)
{
    const struct gpu_firmware_image *image;
    const uint8_t *payload;

    ask_with(0, 48, 97, 96, -1, 0);
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
          m_read(payload + COT_OFF_FRTS_VIDMEM) == 0 &&
          *(const uint32_t *)(payload + COT_OFF_FRTS_VIDMEM_SIZE) == GSP_FRTS_SIZE);
    check("the boot arguments are ~0, which is the cold-boot encoding, and they end where the payload does",
          m_read(payload + COT_OFF_BOOT_ARGS) == ~(uint32_t)0 &&
          m_read(payload + COT_OFF_BOOT_ARGS + 4) == ~(uint32_t)0 &&
          COT_OFF_BOOT_ARGS + 8u == GPU_FSP_PAYLOAD_BYTES);
    check("the three authentication blobs are the container's own sections, at the published offsets",
          memcmp(payload + COT_OFF_HASH, staged_region + image->hash_offset, 48) == 0 &&
          COT_OFF_HASH + 48u == COT_OFF_PUBLICKEY &&
          memcmp(payload + COT_OFF_PUBLICKEY, staged_region + image->publickey_offset, 97) == 0 &&
          COT_OFF_PUBLICKEY + 384u == COT_OFF_SIGNATURE &&
          memcmp(payload + COT_OFF_SIGNATURE, staged_region + image->fmc_signature_offset, 96) == 0 &&
          /* The field is wide, the generation is not: everything past the 97 bytes this chip's table asks for
           * stays zero, which is how a 384-byte field and a 97-byte key coexist in one 860-byte payload. */
          payload[COT_OFF_PUBLICKEY + 97] == 0 && payload[COT_OFF_SIGNATURE + 96] == 0);
    check("the CRC32 recorded is of the image as the card will read it: over the copy's 512 bytes, not over "
          "the page of padding that follows them",
          fsp_state.image_crc32 == gpu_firmware_crc32(model_pages, IMAGE_BYTES) &&
          fsp_state.image_crc32 != gpu_firmware_crc32(model_pages, 0x1000u));
    check("each message keeps its own size now that the handover is the one that goes first: 868 for it, twelve "
          "bytes for the query that follows, and the shared packet counter left holding the last one sent",
          fsp_state.query.request_bytes == 12 && fsp_state.handover.request_bytes == FSP_HEADER_BYTES + COT_SIZE &&
          fsp_state.packet_bytes == 12u);   /* the query is one byte, padded to a dword, framed by two of them */
    check("the handover left without waiting for anything, and the model took it: the packet is the driver's "
          "payload with nothing changed in it",
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
    check("and the FSP moved its queue head off zero, so the packet was taken out of the channel rather than left "
          "in it: the difference between a card that ignored a message and one that never read it",
          fsp_state.submit_valid && fsp_state.submit_consumed &&
          fsp_state.submit_head == fsp_state.submit_tail && fsp_state.submit_tail != 0);
    ask_with(0, 32, 97, 96, -1, 0);
    check("an FMC whose hash section is not 48 bytes is refused a payload, and told which section it was",
          fsp_state.fmc_seen && !fsp_state.staged && fsp_state.cot_defect == 1);
    ask_with(0, 48, 96, 96, -1, 0);
    check("the same for a public key one byte short of the 97 this chip's table asks for (the payload has "
          "room for 384, and that is not the question)",
          fsp_state.cot_defect == 2);
    ask_with(0, 48, 97, 512, -1, 0);
    check("and for a signature of a size the format has no field for", fsp_state.cot_defect == 3);
    ask_with(0, 48, 97, 96, 2, 0);
    check("a container with the hash section's name pointing elsewhere has no image to describe",
          fsp_state.cot_defect == 8);
    ask_with(M_NO_PAGES, 48, 97, 96, -1, 0);
    check("and running out of memory for the copy is its own reason rather than a silent skip",
          fsp_state.fmc_seen && !fsp_state.staged && fsp_state.cot_defect == 6);

    ask_with(0, 48, 97, 96, -1, 1);
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
/* The two sysmem descriptors, and the payload field that points at them.  A GB207 whose volume carries a
 * gsp-*.bin is the case this whole file has been unable to reach so far: the FMC image was enough to assemble a
 * handover, and not nearly enough to boot a resource manager out of one. */
static void test_the_boot_descriptors(void)
{
    const struct gpu_firmware_state *fws;
    const struct gpu_firmware_image *rm, *bl;
    const struct gsp_fw_wpr_meta *meta;
    const struct gsp_fmc_boot_params *params;
    const uint8_t *payload;

    ask_with(0, 48, 97, 96, -1, 0);
    check("with the FMC alone nothing is staged, and the reason is the missing image rather than a missing page",
          !fsp_state.params_staged && fsp_state.params_declined == 3 &&
          fsp_state.boot_params_phys == 0 && fsp_state.wpr_meta_phys == 0);
    check("and the handover still went out, pointing at nothing, because declining the descriptor is not a reason to stop talking",
          fsp_state.cot_ready && model.cot_seen == 1 &&
          m_read(fsp_state.payload + COT_OFF_BOOT_ARGS) == ~(uint32_t)0);

    stage_rm = 1;
    ask_with(0, 48, 97, 96, -1, 0);
    stage_rm = 0;
    fws = gpu_firmware_state();
    rm = &fws->image[0];
    bl = &fws->image[2];
    meta = (const struct gsp_fw_wpr_meta *)(uintptr_t)fsp_state.wpr_meta_phys;
    params = (const struct gsp_fmc_boot_params *)(uintptr_t)fsp_state.boot_params_phys;
    payload = fsp_state.payload;
    check("the resource-manager image was staged as an ELF64 with the two sections the driver asks it to find",
          rm->resident && rm->elf64 && !rm->elf32 && rm->payload_bytes == rm_payload_size &&
          rm->signature_bytes == rm_signature_size && bl->resident && bl->bytes == 64u);
    check("both blocks are page aligned, adjacent, and reported back as what they are",
          fsp_state.params_staged && !fsp_state.params_declined &&
          fsp_state.wpr_meta_phys && (fsp_state.wpr_meta_phys & 0xfffu) == 0 &&
          fsp_state.boot_params_phys == fsp_state.wpr_meta_phys + 4096 &&
          fsp_state.wpr_meta_size == (uint32_t)sizeof *meta);
    check("the metadata opens with the magic and revision the header publishes, and nothing else claims those bytes",
          meta->magic == GSP_FW_WPR_META_MAGIC && meta->revision == GSP_FW_WPR_META_REVISION &&
          meta->gsp_fw_rsvd_start == 0 && meta->verified == 0);
    check("the metadata names a radix-3 tree, not the image, and it is indexed at the size of the image",
          meta->sysmem_addr_of_radix3_elf == fsp_state.radix3_phys &&
          meta->sysmem_addr_of_radix3_elf != fws->address + rm->offset + rm->payload_offset &&
          meta->size_of_radix3_elf == rm_payload_size && fsp_state.radix3_data_phys ==
          fws->address + rm->offset + rm->payload_offset && fsp_state.radix3_tree_bytes == 3u * 4096u);
    {
        const uint64_t *root = (const uint64_t *)(uintptr_t)fsp_state.radix3_phys;
        const uint64_t *lvl1 = (const uint64_t *)(uintptr_t)(fsp_state.radix3_phys + 4096u);
        const uint64_t *lvl2 = (const uint64_t *)(uintptr_t)(fsp_state.radix3_phys + 8192u);
        check("three levels of 4 KiB pages, each entry the page below it, and every entry past the last one zero",
              root[0] == fsp_state.radix3_phys + 4096u && root[1] == 0 && root[511] == 0 &&
              lvl1[0] == fsp_state.radix3_phys + 8192u && lvl1[1] == 0 &&
              lvl2[0] == fsp_state.radix3_data_phys && lvl2[4] == lvl2[0] + 4u * 4096u && lvl2[5] == 0);
        check("and the pages the tree indexes are the fixture's own ramp, read through the address the FMC gets",
              memcmp((const void *)(uintptr_t)lvl2[0], staged_region + rm->offset + rm->payload_offset,
                     rm_payload_size) == 0 && *(const uint8_t *)(uintptr_t)lvl2[0] == 3u &&
              *(const uint8_t *)(uintptr_t)(lvl2[0] + 4095u) == (uint8_t)(4095u * 7u + 3u) &&
              *(const uint8_t *)(uintptr_t)(lvl2[4] + 3u) == (uint8_t)(3u * 7u + 3u));
    }
    check("the signature section the metadata names is the one after it, not a copy of the payload",
          meta->u1.sig.sysmem_addr_of_signature == fws->address + rm->offset + rm->signature_offset &&
          meta->u1.sig.size_of_signature == rm_signature_size &&
          memcmp((const void *)(uintptr_t)meta->u1.sig.sysmem_addr_of_signature,
                 staged_region + rm->offset + rm->signature_offset, rm_signature_size) == 0);
    check("the bootloader is named by where it is, and the three offsets into it are left for whoever parses it",
          meta->sysmem_addr_of_bootloader == fws->address + bl->offset &&
          meta->size_of_bootloader == 64u && meta->bootloader_code_offset == 0);
    check("the sizes this OS can declare without reserving anything are declared, and the ones it cannot are zero",
          meta->frts_size == GSP_FRTS_SIZE && meta->vga_workspace_size == GSP_VGA_WORKSPACE_SIZE &&
          meta->frts_offset == 0 && meta->gsp_fw_wpr_start == 0 && meta->fb_size == 0);
    check("bootGspRmParams points at the metadata, with the size the driver's own struct reports and the RM flag set",
          params->boot_gsp_rm.target == GSP_DMA_TARGET_COHERENT_SYSTEM &&
          params->boot_gsp_rm.desc_offset == fsp_state.wpr_meta_phys &&
          params->boot_gsp_rm.desc_size == (uint32_t)sizeof *meta &&
          params->boot_gsp_rm.is_gsp_rm_boot == 1 && params->boot_gsp_rm.wpr_carveout_offset == 0);
    check("gspRmParams is non-coherent, the SPDM block is left absent rather than aimed, and the boot arguments "
          "name the LibOS record rather than the FMC copy as they did before that record existed",
          params->gsp_rm.target == GSP_DMA_TARGET_NONCOHERENT_SYSTEM &&
          params->gsp_rm.boot_args_offset == fsp_state.libos_phys &&
          params->gsp_rm.boot_args_offset != fsp_state.image_physical &&
          fsp_state.libos_staged && !fsp_state.libos_declined &&
          params->spdm.target == 0 && params->spdm.payload_offset == 0);
    {
        const struct libos_region *r = (const struct libos_region *)(uintptr_t)fsp_state.libos_phys;

        check("the record is four 32-byte descriptors in one page - three log buffers and the RM's own argument "
              "block - with the ids id8() packs big-endian out of the vendor's header",
              fsp_state.libos_phys % 0x1000u == 0 &&
              r[0].id8 == libos_id8("LOGINIT") && r[1].id8 == libos_id8("LOGINTR") &&
              r[2].id8 == libos_id8("LOGRM") && r[3].id8 == libos_id8("RMARGS"));
        check("64 KiB each for the three logs and 4 KiB for the arguments, all contiguous system memory, each "
              "region behind the last",
              r[0].size == 0x10000 && r[1].size == 0x10000 && r[2].size == 0x10000 && r[3].size == 0x1000 &&
              r[0].kind == 1 && r[1].kind == 1 && r[2].kind == 1 && r[3].kind == 1 &&
              r[0].loc == 1 && r[1].loc == 1 && r[2].loc == 1 && r[3].loc == 1 &&
              r[0].pa == fsp_state.libos_log_phys && r[1].pa == r[0].pa + 0x10000 &&
              r[2].pa == r[1].pa + 0x10000 && r[3].pa == fsp_state.libos_phys + 0x1000 &&
              r[3].pa == fsp_state.libos_rmargs_phys);
        check("every log buffer opens with a zero put pointer and then one address per 4 KiB page, which is the "
              "array the GSP walks to write into it",
              ((const uint64_t *)(uintptr_t)r[0].pa)[0] == 0 &&
              ((const uint64_t *)(uintptr_t)r[0].pa)[1] == r[0].pa &&
              ((const uint64_t *)(uintptr_t)r[0].pa)[0x10] == r[0].pa + 0xf000 &&
              ((const uint64_t *)(uintptr_t)r[2].pa)[1] == r[2].pa &&
              ((const uint64_t *)(uintptr_t)r[2].pa)[0x10] == r[2].pa + 0xf000);
        check("and none of it lands on the pages the FMC copy or the radix tree occupy",
              fsp_state.libos_log_phys != fsp_state.image_physical &&
              fsp_state.libos_phys >= (uintptr_t)model_pages &&
              fsp_state.libos_phys + 0x2000 <= (uintptr_t)model_pages + sizeof model_pages &&
              !(fsp_state.libos_log_phys >= fsp_state.radix3_phys &&
                fsp_state.libos_log_phys < fsp_state.radix3_phys + fsp_state.radix3_tree_bytes));
    }
    {
        const struct gsp_arguments_cached *args =
            (const struct gsp_arguments_cached *)(uintptr_t)fsp_state.libos_rmargs_phys;
        const struct msgq_tx_header *tx_cmd = (const struct msgq_tx_header *)(uintptr_t)fsp_state.queue_phys;
        const struct msgq_tx_header *tx_stat =
            (const struct msgq_tx_header *)(uintptr_t)fsp_state.queue_stat_phys;
        const uint64_t *ptes = (const uint64_t *)(uintptr_t)(fsp_state.queue_phys + fsp_state.queue_bytes);

        check("the RM's argument block is GSP_ARGUMENTS_CACHED and it names the ring this kernel built: one "
              "shared region, one page array describing it, command at the start and status after it",
              fsp_state.queue_staged && !fsp_state.queue_declined &&
              args->queue.shared_mem_phys == fsp_state.queue_phys &&
              args->queue.page_table_entries == fsp_state.queue_ptes &&
              args->queue.cmd_queue_offset == 0u && args->queue.stat_queue_offset == 0x8000u &&
              args->queue.lockless_cmd_offset == 0u && args->queue.lockless_stat_offset == 0u &&
              args->b_dmem_stack == 1u && args->sr.old_level == 0u && args->sr.in_pm_transition == 0u);
        check("both rings carry the header nvrm publishes, with the sizes this code actually allocated",
              tx_cmd->size == 0x8000u && tx_cmd->msg_size == 0x1000u && tx_cmd->msg_count == 7u &&
              tx_cmd->entry_off == 0x1000u && tx_cmd->rx_hdr_off == 0xff0u && tx_cmd->write_ptr == 0u &&
              tx_cmd->flags == 0u && tx_stat->size == 0x8000u && tx_stat->msg_size == 0x1000u &&
              fsp_state.queue_ptes == 16u && fsp_state.queue_bytes == 0x10000u);
        check("and the page array behind them is one physical address per 4 KiB page, which is how the GSP "
              "reaches memory at all",
              ptes[0] == fsp_state.queue_phys && ptes[1] == fsp_state.queue_phys + 0x1000 &&
              ptes[fsp_state.queue_ptes - 1] ==
                  fsp_state.queue_phys + (uint64_t)(fsp_state.queue_ptes - 1) * 0x1000);
        {
            /* The status queue is the direction RM writes in.  An entry left there is RM talking, and the
             * reader must consume exactly one and report the function number it found. */
            struct msgq_rx_header *rx = (struct msgq_rx_header *)(uintptr_t)(fsp_state.queue_stat_phys +
                                                                             tx_stat->rx_hdr_off);
            volatile uint32_t *entry = (volatile uint32_t *)(uintptr_t)(fsp_state.queue_stat_phys +
                                                                        tx_stat->entry_off);
            entry[0] = 0x00000097u;                   /* GSP_RM_CONTROL, as a number to read back */
            ((struct msgq_tx_header *)(uintptr_t)fsp_state.queue_stat_phys)->write_ptr = 1u;
            read_status_queue();
            check("an entry RM leaves on the status queue is read once, reported by function number, and "
                  "acknowledged by moving the consumer's cursor",
                  fsp_state.rpc_seen && fsp_state.rpc_function == 0x97u && rx->read_ptr == 1u &&
                  fsp_state.queue_write_ptr == 1u && fsp_state.queue_read_ptr == 1u);
            ((struct msgq_tx_header *)(uintptr_t)fsp_state.queue_stat_phys)->write_ptr = 0u;
            rx->read_ptr = 0u;
            fsp_state.rpc_seen = 0;
            fsp_state.rpc_function = 0u;
        }
    }
    check("so the handover's boot-arguments field is the address of the second block, which is the field that was ~0",
          m_read(payload + COT_OFF_BOOT_ARGS) == (uint32_t)fsp_state.boot_params_phys &&
          m_read(payload + COT_OFF_BOOT_ARGS + 4) == (uint32_t)(fsp_state.boot_params_phys >> 32) &&
          m_read(payload + COT_OFF_FMC_ADDRESS) == (uint32_t)fsp_state.image_physical);
    check("and the card is handed the payload with the descriptors in it, unchanged, in one packet",
          model.cot_seen == 1 && model.cot_bytes == FSP_HEADER_BYTES + COT_SIZE &&
          memcmp((const uint8_t *)model.cot + FSP_HEADER_BYTES, payload, COT_SIZE) == 0);
}


/* The reservation, with the number it needs.  Until a driver could read the frame buffer's size this file
 * could only prove that the fields were *declined*; now that both halves exist - `probe_vram()' in the module
 * and `gsp_fb_plan()' in the kernel - the case that matters is that the metadata and the payload carry the
 * same offsets, that they are inside the memory the chip reported, and that a card too small for them gets the
 * declined encoding rather than a placement it cannot hold. */
static void test_the_reservation(void)
{
    const uint64_t gib8 = 8ull << 30, mib = 1ull << 20;
    const struct gsp_fw_wpr_meta *meta;
    const uint8_t *payload;

    stage_rm = 1;
    model_vram = gib8;
    ask_with(0, 48, 97, 96, -1, 0);
    stage_rm = 0;
    model_vram = 0;
    meta = (const struct gsp_fw_wpr_meta *)(uintptr_t)fsp_state.wpr_meta_phys;
    payload = fsp_state.payload;

    check("a card that reports its video memory gets a reservation placed in it",
          fsp_state.params_staged && fsp_state.plan_ready && fsp_state.plan.vram_bytes == gib8 &&
          fsp_state.plan.usable_top == gib8);
    check("the plan's own numbers are the ones the metadata carries",
          meta->fb_size == gib8 && meta->frts_offset == fsp_state.plan.frts_offset &&
          meta->frts_size == fsp_state.plan.frts_bytes &&
          meta->gsp_fw_wpr_start == fsp_state.plan.wpr2_start &&
          meta->gsp_fw_wpr_end == fsp_state.plan.wpr2_end &&
          meta->gsp_fw_heap_offset == fsp_state.plan.heap_offset &&
          meta->gsp_fw_heap_size == fsp_state.plan.heap_bytes &&
          meta->boot_bin_offset == fsp_state.plan.bootloader_offset &&
          meta->gsp_fw_offset == fsp_state.plan.elf_offset &&
          meta->non_wpr_heap_offset == fsp_state.plan.non_wpr_heap_offset &&
          meta->non_wpr_heap_size == fsp_state.plan.non_wpr_heap_bytes &&
          meta->gsp_fw_rsvd_start == fsp_state.plan.rsvd_start);
    check("the reservation ends back from the top of that memory, one MiB of FRTS and no further",
          meta->frts_offset + meta->frts_size == gib8 &&
          meta->gsp_fw_wpr_end == gib8 && meta->gsp_fw_wpr_start < meta->gsp_fw_wpr_end &&
          meta->gsp_fw_wpr_end - meta->gsp_fw_wpr_start <= 4u * mib &&
          meta->frts_offset % (128u * 1024u) == 0 && meta->gsp_fw_wpr_start % mib == 0);
    check("the workspace the firmware expects below its carve-out is named, and the boot count is a cold boot",
          meta->vga_workspace_offset == gib8 - GSP_VGA_WORKSPACE_SIZE && meta->boot_count == 0);
    check("the payload's FRTS field is the same offset as the metadata's, not a second opinion",
          m_read(payload + COT_OFF_FRTS_VIDMEM) == (uint32_t)fsp_state.plan.frts_offset &&
          m_read(payload + COT_OFF_FRTS_VIDMEM + 4) == (uint32_t)(fsp_state.plan.frts_offset >> 32) &&
          *(const uint32_t *)(payload + COT_OFF_FRTS_VIDMEM_SIZE) == (uint32_t)fsp_state.plan.frts_bytes);
    check("and every region the plan names lies inside the memory the chip reported, in the order the "
          "firmware walks them",
          fsp_state.plan.non_wpr_heap_offset + fsp_state.plan.non_wpr_heap_bytes ==
              fsp_state.plan.wpr2_start &&
          fsp_state.plan.wpr2_start <= fsp_state.plan.heap_offset &&
          fsp_state.plan.heap_offset + fsp_state.plan.heap_bytes == fsp_state.plan.elf_offset &&
          fsp_state.plan.elf_offset + fsp_state.plan.elf_bytes <= fsp_state.plan.bootloader_offset &&
          fsp_state.plan.bootloader_offset + fsp_state.plan.bootloader_bytes <= fsp_state.plan.frts_offset &&
          fsp_state.plan.wpr2_end <= gib8);

    /* A card whose frame buffer cannot hold the reservation: the fields go back to the encoding a machine
     * with nothing to reserve sends, because a placement that does not fit is worse than none at all. */
    stage_rm = 1;
    model_vram = 2ull * mib;
    ask_with(0, 48, 97, 96, -1, 0);
    stage_rm = 0;
    model_vram = 0;
    meta = (const struct gsp_fw_wpr_meta *)(uintptr_t)fsp_state.wpr_meta_phys;
    payload = fsp_state.payload;
    check("a card too small for the reservation is refused, with the reason kept",
          !fsp_state.plan_ready && fsp_state.plan_state != 0);
    check("and nothing is placed: the FRTS keeps the size this tree has always declared and the offset it "
          "has always declined",
          meta->frts_offset == 0 && meta->frts_size == GSP_FRTS_SIZE && meta->fb_size == 0 &&
          meta->gsp_fw_wpr_start == 0 &&
          m_read(payload + COT_OFF_FRTS_VIDMEM) == 0 &&
          *(const uint32_t *)(payload + COT_OFF_FRTS_VIDMEM_SIZE) == GSP_FRTS_SIZE);
}

static void test_the_handover_answers(void)
{
    ask_with(M_COT_WPR, 48, 97, 96, -1, 0);
    check("a card that reads the image and rejects it is reported as having read and rejected it, with the "
          "falcon's own name for that answer",
          fsp_state.handover_sent && fsp_state.handover.error == 0xdfu && fsp_state.handover.complete &&
          !fsp_state.gsp_released && fsp_state.hwcfg2_after == 0xbadf4100u);
    check("and the name the panel prints is the published one for 0xdf rather than a paraphrase of it",
          gpu_fsp_flcn_name(0xdfu) && !strcmp(gpu_fsp_flcn_name(0xdfu), "FLCN_ERR_FMC_BOOT_FROM_INVALID_WPR") &&
          !strcmp(gpu_fsp_flcn_name(0u), "FLCN_OK") && gpu_fsp_flcn_name(0x9eu) == 0);
    check("the lock it did not lift is still the locked-target code, so nothing on this path claims a change",
          FSP_NOT_CONTENT(fsp_state.hwcfg2_after));

    ask_with(M_COT_SILENT, 48, 97, 96, -1, 0);
    check("a card that takes the handover and answers nothing is reported as exactly that: sent, no reply, and "
          "no lockdown word that could be mistaken for one",
          fsp_state.handover_sent && fsp_state.handover.bytes == 0 && fsp_state.handover.complete == 0 &&
          !fsp_state.settled && !fsp_state.hwcfg2_after_valid && !fsp_state.gsp_released);

    ask_with(M_BUSY, 48, 97, 96, -1, 0);
    check("a card that takes a packet and never consumes it still gets the handover, because nothing is waited "
          "for before the first message but its queue being free at the moment of writing - and the message that "
          "would have followed is declined with its reason kept: blocked(1), not silence",
          fsp_state.handover_attempted == 1 && fsp_state.handover_sent && fsp_state.handover.bytes == 0 &&
          !fsp_state.queried && fsp_state.send_blocked == 1);
    check("and a packet nobody read is reported as unanswered rather than unread: the head is still at the zero "
          "the host wrote, which is the one thing a reply-wait cannot tell apart from silence",
          fsp_state.submit_valid && !fsp_state.submit_consumed && fsp_state.submit_head == 0);
}

/* The three readings that turn "no reply" into a direction, exercised against the three ways a card really has
 * produced that sentence on this machine. */
static void test_the_channel_that_cannot_be_read_back(void)
{
    /* The GB207's shape: the writes are taken - the model's channel holds the packet - and reads through the
     * window hand back something that is not in it.  What must come out of that is a mismatch *and* a scan that
     * finds the packet nowhere, because those two together are what says "the read-back is the liar", not "the
     * store did not land". */
    ask_with(M_EMEM_JUNK, 48, 97, 96, -1, 0);
    check("a card whose window does not read back what was written is reported as a mismatch, not as a refusal",
          !fsp_state.send_readback && fsp_state.send_mismatch == 0 && fsp_state.emem_got0 == 0x20u);
    check("and the four dwords it hands back are kept beside the four that went in, so the two can be compared",
          fsp_state.emem_got[0] == 0x20u && fsp_state.emem_got[1] == 0x20u &&
          fsp_state.emem_want[0] == 0xc0000000u);
    check("the scan then says the packet is nowhere in the first kilobyte - while the model's channel has it, "
          "which is the whole distinction the scan exists to draw",
          fsp_state.packet_at == 0xffffffffu && model.packet[0] == 0xc0000000u);
    check("and the handover's own channel numbers were kept rather than the query's that followed it",
          fsp_state.handover_have_channel && fsp_state.handover_got[0] == 0x20u &&
          fsp_state.handover_want[0] == 0xc0000000u && fsp_state.handover_packet_at == 0xffffffffu &&
          fsp_state.handover_bursts == 4);
    check("the packet the model's card took is the whole 868 bytes, in order, header first",
          model.cot_bytes == 868 && model.cot[0] == 0xc0000000u);
    check("nor is the handover's channel reading taken after the doorbell: dword 0 after the wait is the "
          "model's own channel, which still holds the packet because this card never reads it",
          fsp_state.handover_emem0_after == 0x20u);
}

static void test_a_doorbell_that_does_not_land(void)
{
    ask_with(M_DROP_TAIL, 48, 97, 96, -1, 0);
    check("a tail register that accepts a store and reads back zero is reported as a doorbell that did not "
          "land, which is a different fact from a doorbell nobody answered",
          fsp_state.handover_sent && !fsp_state.handover_db_landed && fsp_state.handover_db_tail == 0 &&
          fsp_state.handover_doorbell == 864u);
    check("and the wait that follows is still reported, with the tail it left behind beside the one it wanted",
          !fsp_state.settled && fsp_state.handover_tail != fsp_state.handover_doorbell);
    check("the packet itself was written and read back first, and is not blamed for the doorbell's failure",
          fsp_state.send_readback && fsp_state.handover_bursts == 4u && fsp_state.handover_packet_at == 0u &&
          m_read(model_channel) == 0xc0000000u && model.cot_bytes == 0);

    ask_with(0, 48, 97, 96, -1, 0);
    check("and a card that takes the doorbell says so, with the tail it read back holding the value written",
          fsp_state.handover_db_landed && fsp_state.handover_db_tail == 864u);
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

/* The table this path reads its expectations out of is per generation, so the fixtures are too: two rows of
 * the reference driver's own table, the lock as the bit the driver reads, the mailbox pair the FMC answers
 * through, and the two bounds a wait is allowed to have. */
static void test_the_generation_table(void)
{
    struct gpu_device device = nvidia_at();
    struct boot_handoff empty;

    memset(&empty, 0, sizeof empty);
    model_arch_boot0 = 0x187000a1u;                       /* GH100: Hopper's row */
    ask_with(0, 48, 384, 384, -1, 0);
    check("a Hopper part is judged against Hopper's row - version 1 and 48/384/384 - and its own container "
          "passes it",
          fsp_state.arch == 0x18u && fsp_state.fsp_supported && fsp_state.cot_version == 1u &&
          fsp_state.cot_pkey_bytes == 384u && fsp_state.cot_sig_bytes == 384u &&
          fsp_state.staged && fsp_state.cot_defect == 0 &&
          model.cot_shape_ok && model.cot_blobs_ok && fsp_state.handover.error == 0);
    ask_with(0, 48, 97, 96, -1, 0);
    check("and the same chip shown a Blackwell-sized container refuses it on the public key: the two rows are "
          "not interchangeable in either direction, which is the whole reason the row is read from the chip",
          fsp_state.arch == 0x18u && !fsp_state.staged && fsp_state.cot_defect == 2);
    model_arch_boot0 = 0x197000a1u;                       /* AD100: no FSP entry in the reference driver */
    ask(0);
    check("a generation with no FSP in the reference driver is not written to, and the reason printed is the "
          "architecture rather than a failure of the card",
          fsp_state.arch == 0x19u && !fsp_state.fsp_supported && fsp_state.cot_defect == 10 &&
          !fsp_state.queried && !fsp_state.handover_attempted && fsp_state.writes == 0 && model.writes == 0);
    model_arch_boot0 = 0x1b7000a1u;                       /* back to the chip everything else models */

    model_reset(M_REFUSE | M_RELEASED);   /* M_RELEASED exempts the coprocessor's window from the refusal,
                                             * which is how a fixture gets to choose the word it answers with */
    ((uint32_t *)model_gsp)[G_HWCFG2] = 0x2a13u;          /* answers, and answers with bit 13 set */
    gpu_fsp_note(&device, &empty);
    check("a capability word that answers with the lockdown bit set is readable and still locked: the test "
          "before this one was \"the word was not zero\", which called this state released",
          fsp_state.gsp_readable && fsp_state.lockdown == 1 && !fsp_state.gsp_released &&
          fsp_state.hwcfg2 == 0x2a13u);
    model_reset(M_REFUSE | M_RELEASED);   /* M_RELEASED exempts the coprocessor's window from the refusal,
                                             * which is how a fixture gets to choose the word it answers with */
    ((uint32_t *)model_gsp)[G_HWCFG2] = 0xa13u;           /* the same word, that bit clear */
    gpu_fsp_note(&device, &empty);
    check("and the same read with the bit clear is the release - one bit apart, and the difference is all this "
          "path claims about the lock",
          fsp_state.gsp_readable && fsp_state.lockdown == 0 && fsp_state.gsp_released);
    model_reset(M_REFUSE | M_RELEASED);   /* M_RELEASED exempts the coprocessor's window from the refusal,
                                             * which is how a fixture gets to choose the word it answers with */
    ((uint32_t *)model_gsp)[G_HWCFG2] = 0xa13u;
    ((uint32_t *)model_gsp)[(NV_PGSP_MAILBOX0 - MODEL_GSP) / 4u] = 0xdeadbeefu;
    gpu_fsp_note(&device, &empty);
    check("the falcon's mailbox pair is recorded as read and not interpreted: this OS hands over no boot "
          "arguments for the FMC to write back, so a non-zero pair is an answer whose kind cannot be told",
          fsp_state.gsp_mailbox0 == 0xdeadbeefu);

    ask_with(0, 48, 97, 96, -1, 0);
    check("a reply is acknowledged by pulling both message pointers to zero, which is what the driver that "
          "boots this chip does, and what leaves the second message - the handover - at the channel offset "
          "this driver reads from",
          ((uint32_t *)model_fsp)[R_MSGQ_HEAD] == 0u && ((uint32_t *)model_fsp)[R_MSGQ_TAIL] == 0u);

    check("the waits carry the reference driver's budgets in milliseconds, not a count of reads that happens "
          "to land near one",
          fsp_state.reply_ms == FSP_WAIT_REPLY_MS && fsp_state.boot_ms == FSP_WAIT_BOOT_MS);
    model_reset(M_TIMER_DEAD);
    gpu_fsp_note(&device, &empty);
    check("with no clock to read the file says the wait was bounded by reads, instead of quoting a duration it "
          "cannot measure",
          !fsp_state.wait_clocked && fsp_state.ticks_per_ms == 0);
    ask(0);
    check("and with the clock there, the rate the cycle counter was measured against is on the state and the "
          "wait says how much of its budget it spent",
          fsp_state.wait_clocked && fsp_state.ticks_per_ms != 0 &&
          fsp_state.reply_ms_used <= FSP_WAIT_REPLY_MS && fsp_state.boot_ms_used <= FSP_WAIT_BOOT_MS);
}

int main(void)
{
    test_nothing_staged();
    test_the_gates();
    test_the_request_bytes();
    test_a_full_query_and_reply();
    test_the_handover_payload();
    test_the_boot_descriptors();
    test_the_reservation();
    test_the_handover_answers();
    test_the_channel_that_cannot_be_read_back();
    test_a_doorbell_that_does_not_land();
    test_the_clock_and_the_doorbell_page();
    test_the_generation_table();
    printf("%u check(s), %u failure(s)\n", checks_run, failures_run);
    return failures_run != 0;
}

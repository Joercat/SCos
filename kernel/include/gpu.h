/* GPU driver registry, auto-detection and the 2D engine boundary.
 *
 * Two rules govern this whole subsystem, and both come from what the target
 * machine needs to survive:
 *
 * 1. Matching is exact.  A family binds only to a vendor/device pair that the
 *    upstream driver for that family also binds to; the tables in
 *    `kernel/drivers/gpu/gpu_ids.h` are generated from those drivers' own
 *    `supportedDevices`/`kSupportedDevices`/`chipTable` arrays at a pinned
 *    commit, by `tools/research/gen_gpu_tables.py`.  Nothing here accepts a
 *    vendor as a proxy for a chip, and an unmatched ID is reported as unmatched
 *    rather than pushed at the closest driver.
 * 2. Detection never touches the display.  Identification reads configuration
 *    space only: no BAR sizing (which writes 0xffffffff into the BAR before
 *    restoring it), no reset, no power state, no clock or mode register.  Those
 *    operations can destroy the only usable console, so they live behind
 *    `gpu_driver_prepare()`, which nothing calls until a family's engine code
 *    has actually been ported and verified.  See `docs/migration/`
 *    `GPU-BASIC-2D-RESEARCH.md` for what has been measured about each family.
 */
#ifndef SCOS_GPU_H
#define SCOS_GPU_H

#include "gsp_layout.h"   /* the frame-buffer reservation the FSP handover places */
#include "kernel.h"
#include "gpu_match.h"
#include "gpu_abi.h"

/* Engine entry points, one per operation the compositor actually issues.  The
 * signatures mirror the accelerant engine ABI (fill rectangle, screen blit,
 * span fill, engine idle, token sync, retrace wait, CRTC pan) so a ported driver
 * can be dropped in without re-deriving its meaning; every one is optional.
 *
 * Contract for an implementation: coordinates are pixels in scanout space, the
 * colour is X8R8G8B8, and a call returns 0 only after the operation has been
 * submitted in a way the caller may immediately follow with `wait_idle`.  A
 * negative return means "this driver will not do this operation on this device",
 * and the caller must then do it on the CPU - never partially, never silently. */
struct gpu_engine_ops {
    int (*fill_rectangle)(int x, int y, int width, int height, uint32_t color);
    int (*screen_to_screen_blit)(int dst_x, int dst_y, int width, int height,
                                 int src_x, int src_y);
    int (*fill_span)(const int *runs, int run_count, uint32_t color);
    int (*wait_idle)(void);
    int (*retrace_wait)(void);
    int (*move_display)(int x, int y);
};

/* Port state of a family inside SCos, kept separate from the upstream inventory
 * so a log line can never imply more than the tree can do. */
enum gpu_port_state {
    GPU_PORT_NONE = 0,   /* matched family, no code ported yet -> CPU path */
    GPU_PORT_DISPLAY,    /* scanout/timing ported, no 2D engine */
    GPU_PORT_ENGINE      /* 2D engine ported; ops non-NULL */
};

struct gpu_device {
    uint8_t bus, slot, function, subclass;
    uint16_t vendor, device;
    uint8_t revision;
    const struct gpu_match *match;   /* family whose table matched, else NULL */
    const char *chip;                /* name from the matching row, may be NULL */
    int bound;                       /* a ported engine is attached to this device */
    const struct scos_gpu_engine_ops *module_ops;   /* loaded module's engine table, else NULL */
    int is_scanout;                  /* this function feeds the visible console */
    uint64_t bar[6];                 /* raw BAR register values, never sized */
    int command, header_type;        /* read-only copies for the report */
    uint8_t named_only;              /* row came from the naming table: a known chip, no driver claim */
    /* Real device access for a chip this tree has no engine for: the first two dwords of BAR0, read
     * through a mapping the kernel owns and never releases.  Probing is skipped for any function a
     * module could claim, because a second mapping of the same aperture would collide with the
     * module's own window.  reg_state: 0 not probed, 1 readable, 2 the device answered all-ones,
     * 3 the mapping was refused, 4 the function has memory decoding disabled in its command register. */
    uint64_t reg_base;
    uint32_t reg_first, reg_second;
    uint8_t reg_state;
    uint8_t reg_kernel_fallback;     /* the kernel read them only because no module was staged */
    /* Total video memory, as a module read it from the chip; 0 when no module could.  It is the one
     * number a firmware reservation has to have - a carve-out placed from offset 0 is not a
     * reservation - so "unknown" is recorded as unknown and never defaulted. */
    uint64_t vram_bytes;
};

#define GPU_MAX_DEVICES 8

void gpu_init(const struct boot_framebuffer *fb);
int gpu_device_count(void);
/* Whether a device's match comes from a driver's binding table (may load a module) or from the
 * naming table (must not). */
int gpu_module_eligible(const struct gpu_device *g);
const struct gpu_device *gpu_device(int index);
const struct gpu_device *gpu_scanout_device(void);
/* The bound driver, i.e. the one family whose ported code SCos will use.  NULL
 * while no family is both matched and ported, which is the normal state. */
const struct gpu_driver *gpu_bound_driver(void);
void gpu_report(char *out, size_t capacity);
const char *gpu_vendor_name(uint16_t vendor);
/* PCI configuration writes issued while detection ran.  This must be 0: naming a
 * display adapter reads config space only.  It is reported rather than merely asserted
 * so a boot log shows the invariant held on the machine being debugged. */
int gpu_config_writes_during_detect(void);
/* The generated family table (kernel/drivers/gpu/gpu_ids.h), one record per family
 * SCos can name.  `gpu_match_id_total()` is the number of exact IDs across them. */
int gpu_match_family_count(void);
/* Port state for a family name, or NULL when SCos has no record for it. */
const struct gpu_driver *gpu_port_for(const char *family);
int gpu_port_record_count(void);

/* A GPU display module is a separate file on the boot disk; the firmware reads only the one whose
 * family the shared matcher names for this machine's chip.  The state below is what this boot
 * actually loaded and verified, so a report can distinguish "matched a family" from "a driver is
 * resident and proved to move pixels". */
struct gpu_module_state {
    int present;                     /* header validated, relocations applied */
    int bound;                       /* init succeeded and the kernel accepted the module */
    int identification_only;         /* ... and it exposes no engine, only what the chip said */
    char name[24];
    char family[16];
    /* Enough for a driver's identity line *and* the engine inventory it read from the chip.  This used to
     * be 512 and the sentence was written to fit it, which is how a real card's report lost its last
     * clause; a panel now wraps, so length costs lines rather than facts, and the bound only has to be
     * generous enough that no driver's whole inventory can reach it.  The kernel copies without truncating,
     * so the number is a ceiling to check against, not a place where text dies quietly.  It went to 1152
     * when the report grew its per-target clauses: each named block a chip refuses to answer costs about a
     * hundred and thirty bytes to say so, and how many blocks that is is the chip's decision, not the
     * driver's - so the fixture seeds its widest case and asserts the length, rather than this note.
     *
     * The note was right and the fixture was not: a real GB207's inventory came back measuring 1151 bytes,
     * which is this field minus one, and the clause it lost was the one about the submission window's clock.
     * The widest case a fixture can seed is a guess about what a chip will say, so the ceiling has to be
     * large enough that no driver's whole report reaches it *and* the copy below has to say so when it clips.
     * 2048 is that: twice the length a real card produced, with room for the per-target clauses a chip with
     * more engines refused would add. */
    char describe[2048];
    uint64_t resident_bytes;         /* image in the executable boot-arena region */
    uint64_t file_bytes;             /* bytes read off the disk */
    uint32_t store_count;            /* modules on the disk, including those never opened */
    uint32_t store_bytes;
    uint32_t opened_bytes;           /* the one file the firmware was told to read */
    uint32_t unopened_bytes;         /* every other family: on the disk, never loaded, never run */
    uint32_t reloc_count, id_count;
    int refusal;                     /* nonzero predicate code when present == 0 */
    int self_test_pixels, self_test_matches;
};
/* The coprocessor's firmware set as the boot volume offered it, passed up from the firmware through
 * struct boot_firmware.  Every number here is a measurement the kernel is repeating on the boot stub's
 * word, because the bytes themselves were read and released by the stub - boot/uefi/gsp_fw.h explains why
 * they are not kept - which is exactly why this struct carries what `gpu_firmware_state' recomputed rather
 * than what it copied: the counts are re-added from the per-file records here, and a record set whose own
 * summary disagrees with them is reported as inconsistent instead of quietly believed. */
#define GPU_FIRMWARE_ROLES BOOT_FIRMWARE_ROLES
struct gpu_firmware_role {
    unsigned state;                  /* BOOT_FIRMWARE_STATE_ABSENT / _STAGED / _TOO_BIG / _UNREADABLE */
    uint32_t bytes;                  /* as the stub read it off the disk */
    uint32_t crc32;                  /* over those bytes */
    uint32_t flags;                  /* BOOT_FIRMWARE_ELF64 and friends */
    uint32_t payload_bytes;          /* .fwimage, the payload the falcon is started from */
    uint32_t signature_bytes;        /* whatever the image calls its signature section */
    uint32_t sections;               /* section headers walked: 0 of them is a different fact from "missing one" */
    uint32_t offset;                 /* where its bytes were copied in the staged region, ~0u when kept=n */
};
/* What one staged image turned out to contain, read by this kernel out of the bytes the boot stub kept.
 * `resident' says the window exists; `checksum_matches' says it is still the file that was on the disk; the
 * offsets are inside the image, not inside the region, because the boot sequence is handed a physical address
 * per image and needs the section relative to it. */
struct gpu_firmware_image {
    unsigned resident;
    unsigned checksum_matches;
    uint32_t offset, bytes;
    unsigned elf32, elf64;
    unsigned fmc_template;             /* the first 52 bytes equal the published FMC header */
    unsigned fmc_sections;             /* type, flags, bounds and CRC32 of every section check out */
    unsigned fmc_named;                /* image, hash, publickey and signature are all present */
    unsigned sections;
    unsigned crc_failures;             /* sections whose sh_info CRC32 disagrees with their bytes */
    uint32_t section_begin;            /* where the FMC format says the first section's data starts */
    uint32_t payload_offset, payload_bytes;           /* .fwimage, from the RM image */
    uint32_t signature_offset, signature_bytes;       /* its .fwsignature_* section */
    uint32_t image_offset, image_bytes;               /* "image", from the FMC image */
    uint32_t hash_offset, hash_bytes;                   /* the three authentication sections of an FMC image */
    uint32_t publickey_offset, publickey_bytes;         /* are offsets too, because a boot sequence is handed */
    uint32_t fmc_signature_offset, fmc_signature_bytes; /* them physical addresses, not pointers */
    char signature_name[24];
};
struct gpu_firmware_state {
    int resident;                      /* images whose bytes this kernel can read */
    unsigned parsed;                   /* gpu_firmware_scan() ran over them */
    uint32_t resident_bytes;
    uint64_t address;                  /* where in physical memory, as the stub allocated it */
    struct gpu_firmware_image image[GPU_FIRMWARE_ROLES];
    int present;                     /* any role at all was found on the volume */
    unsigned staged;                 /* roles the stub could open, whatever the outcome */
    unsigned complete;               /* the three an FSP part boots through are all staged and the first is
                                      * a 64-bit ELF holding both sections the boot sequence reads - not "all
                                      * five", because the booter pair is the other path (see boot.h) */
    unsigned consistent;             /* the stub's summary agrees with the records it came with */
    uint32_t total_bytes, flags;
    char signature_name[24];         /* the name the staged image gives its signature section */
    struct gpu_firmware_role role[GPU_FIRMWARE_ROLES];
};
void gpu_firmware_note(const struct boot_handoff *handoff);
/* The name of one of the falcon return values an FSP answer can carry, or 0 when the code is not in the
 * published list.  Exposed so the panel can print what the card said rather than a bare number. */
const char *gpu_fsp_flcn_name(uint32_t code);

/* The same CRC32 the staged images are verified with, exposed because the handover payload the security
 * processor is given has to be checked over the copy the card will read, not over the copy this kernel read. */
uint32_t gpu_firmware_crc32(const uint8_t *data, uint32_t length);

#define GPU_FSP_PAYLOAD_BYTES 860              /* one NVDM_PAYLOAD_COT, exactly, padded by the sender
                                                  * to a whole number of dwords before it is written */
/* The kernel's only raw access to a device register: `window' is what device_map() returned, `within' a byte
 * offset inside it.  Two functions and no policy - bounds, counting and the decision to stop belong to the
 * caller, where they can be tested with the logic that needs them - so that a caller whose registers have
 * behaviour (this card's channel offset walks as it is read, and its queue pointers move when the firmware
 * consumes a packet) can be run against a model of the register file instead of against an array.
 * kernel/drivers/gpu/gpu_io.c is the production pair; tools/tests/gpu_fsp_host.c supplies its own. */
uint32_t gpu_io_read32(const volatile uint32_t *window, uint64_t within);
void gpu_io_write32(volatile uint32_t *window, uint64_t within, uint32_t value);

/* One reply, as the message format describes it: the two header dwords' flags and types, and the response
 * payload's task, the request it answers, and its error code.  `framed' says the packet was NVIDIA's at all;
 * `readback_ok' says a second pass over the same channel returned the same bytes, which is what makes "these
 * are distinct offsets of the channel" a measurement rather than a belief about a counter; `complete' says
 * there were enough bytes for the task/command/error triple to have been read rather than assumed. */
struct gpu_fsp_reply {
    uint32_t bytes;
    uint32_t request_bytes;               /* how big the message this one answers was, as it left us */
    int som, eom, framed, readback_ok, complete, oversize;
    int type, kind;                          /* the NVDM type in the header, and the payload's first byte */
    uint32_t task, command, error;
};

/* What the card's flight security processor said when this OS asked.  Every field is a register read, a byte
 * count or a staging decision; nothing here is inferred, and a zero `reads' means the file never got as far as
 * touching the hardware.  See kernel/drivers/gpu/gpu_fsp.c for what each number is and who published it. */
/* One store this driver put on the card, kept so the panel can name it: a total number of writes cannot tell
 * "the doorbell was rung" from "the payload was written and the queue was left alone", which is the difference
 * between a card that ignored us and a driver that never asked. */
struct gpu_fsp_store {
    uint32_t offset;                             /* BAR0-relative, so it can be looked up in the published page */
    uint32_t value;
};

#define GPU_FSP_STORE_LOG 8u

struct gpu_fsp_state {
    int present, attempted;                  /* a display function was there; the probe ran on it */
    int nvidia;                              /* the vendor is the one whose decode these offsets are */
    uint16_t vendor;                         /* whose it was, whichever it turned out to be */
    uint64_t bar0;                           /* the aperture the three pages below are offsets into */
    int skipped;                             /* 1 no aperture at all, 2 one that cannot be mapped */
    int mapped, mailbox_readable;            /* the windows exist; the queue registers answer at all */
    int gsp_readable;                        /* the coprocessor's own block no longer refuses a reader */
    uint32_t boot_scratch;                   /* 0xFF in the low byte once the FSP has finished booting */
    int booted, boot_polls;                   /* and how many looks at it the wait took */
    uint32_t ucode_version;                  /* NV_PFSP_FSP_UCODE_VERSION, 11:0 */
    uint32_t fuse_check;                     /* the FSP's own fuse-consistency word: 0 means it passed */
    uint32_t hwcfg2;                         /* the capability word whose refusal was the blocker */
    uint32_t queue_head, queue_tail, msgq_head, msgq_tail;
    /* What the queue says after we store to it without being invited.  The reference driver zeroes both head
     * words on receive and never tests whether it is allowed to read them first; we gated every write on a
     * readable pair, which is how a card can finish a whole boot with writes 0 because nobody ever tried. */
    uint32_t reset_head, reset_tail;
    /* What the GSP boot needs besides the image: the two descriptors the FMC reads back out of sysmem before
     * it will run, staged in pages of their own and pointed at by the handover payload.  An address of zero
     * with `params_declined' naming the reason is the honest state; a made-up one is not. */
    uint64_t wpr_meta_phys, boot_params_phys;
    uint64_t radix3_phys, radix3_data_phys, signature_phys, bootloader_phys;
    uint32_t wpr_meta_size, radix3_bytes, radix3_tree_bytes, signature_bytes, bootloader_bytes;
    unsigned params_staged;                  /* both descriptors are in memory and pointed at */
    unsigned params_declined;      /* 1 no pages, 2 no FMC image, 3 no RM image on the volume, 4 no tree pages */
    /* The reservation the handover describes, and the one number it cannot be computed without: the
     * video memory a module read off the chip.  A plan that was refused leaves every offset zero and
     * names the reason, which is what makes "no carve-out" a recorded fact and not a silent default. */
    uint64_t vram_bytes;
    struct gsp_fb_plan plan;
    /* Two fields, because GSP_LAYOUT_OK is zero and a struct that has never been asked would otherwise
     * read as a plan that succeeded - which is how a zero-byte FRTS came to be one edit away from being
     * sent.  `plan_ready' is the only thing any caller may test. */
    int plan_ready;
    int plan_state;                          /* enum gsp_layout_refusal: why not, when not ready */
    struct gpu_fsp_store stores[GPU_FSP_STORE_LOG];  /* the last writes, oldest first, wrapped */
    uint32_t store_count;                            /* writes issued, not writes kept: the log is shorter */
    /* The channel's window is 256 bytes wide and wraps, so a packet is written in bursts and then read back
     * out of the channel and compared with what was written.  These three are that check: how many bursts the
     * packet needed, whether it came back byte for byte, and the first dword that did not - which is the number
     * a reader needs, because "the channel is not holding the packet" and "the packet is wrong" are different
     * faults and the failing offset tells them apart. */
    uint32_t send_bursts;
    int send_readback;
    uint32_t send_mismatch;
    int queue_writable;                  /* the value we wrote came back, whatever the read said before */
    int queried, responded, send_blocked;     /* a request went into the channel, a reply was read back off
                                               * it, and why a request could not go in */
    int polled, reply_polls;                  /* how many looks each of the two waits took */
    uint32_t packet_bytes, doorbell;
    struct gpu_fsp_reply query, handover;     /* the two requests this file can make, and what came back */
    uint64_t reads, writes;                  /* register accesses this file made, counted as it went */
    uint32_t refusal_reads;                  /* how many of the reads were the 0xbadf4100 refusal */
    int fmc_seen, staged, cot_ready, cot_defect;
    int handover_polls;                               /* looks at the reply queue while waiting for the handover answer */
    int handover_attempted, handover_sent, handover_blocked;   /* the COT message: tried, put in the channel,
                                                                   * and why not, in that order of detail */
    /* The two things a submission needs on this generation, measured at the addresses the published
     * Blackwell headers give for them rather than at the legacy user-mode page: the timer block, whose clock
     * has to be running and readable for anything to be timed against, and the virtual-function page, which
     * is where the doorbell a channel is rung through now lives.  `*_readable' means this kernel reached the
     * window at all; `dead' and `answered' are what the chip put back. */
    int timer_readable, timer_dead, timer_ticking, timer_refused;
    uint32_t timer_start, timer_end, timer_after;
    int vf_readable, vf_answered, vf_refusals;
    uint32_t vf_leaf0, vf_top0, vf_scratch[4];
    uint32_t hwcfg2_after, boot_scratch_after;                  /* re-read after the attempt: the two words
                                                                   * that change if the FSP took the image */
    int gsp_released, queues_idle_after;
    /* Whether the FSP booted the resource manager, decided the way the driver that does it decides: the falcon
     * mailbox pair the FMC writes its boot-arguments address - or an error code - into, and the lockdown bit of
     * the coprocessor's capability word, polled for four seconds.  It is NOT a reply on the channel: the
     * reference driver never waits for one, and a build that did could call a boot that worked a silence.  The
     * pair and the word are therefore recorded with a flag each, because a word this kernel never read is not a
     * zero and must not be printed as one. */
    int settled;                              /* 1 = the poll reached an answer, 0 = it timed out */
    uint64_t handover_ms_used;                /* how long that poll took, in milliseconds */
    int hwcfg2_after_valid, mailbox_after_valid, scratch_after_valid, queue_after_valid;
    int mbox_error;                           /* a non-zero mailbox pair that is not the boot-args address */
    uint32_t queue_tail_after, queue_head_after;   /* the reply queue after the doorbell: proves the store */
    /* Whether the coprocessor *took* the packet, which is a different question from whether it answered: on
     * this generation the host writes into EMEM at offset 0 and moves QUEUE_TAIL; the FSP moves QUEUE_HEAD up
     * as it consumes, and only then writes a reply.  A head that never moves says the packet is still sitting
     * in a channel nobody is reading, which is the one failure a reply-wait cannot distinguish from silence. */
    int queue_probed;                         /* the write-and-read-back probe actually ran */
    int submit_valid, submit_consumed;
    uint32_t submit_head, submit_tail;
    /* The LibOS argument record the resource manager is told to read: four descriptors, three 64 KiB log
     * buffers with an address array inside each, and the RM's own argument block.  `libos_declined' is why
     * there is none, and it is the difference between a handover that can boot an RM and one that cannot. */
    uint64_t libos_phys, libos_log_phys, libos_rmargs_phys;
    unsigned libos_staged, libos_declined;    /* 1 no pages */
    uint32_t libos_log_bytes;
    /* The shared ring RM is told to talk through: two 0x8000 regions (command and status), a page array, and
     * the argument block that names them.  `queue_declined' is why there is none. */
    uint64_t queue_phys, queue_stat_phys;
    unsigned queue_staged, queue_declined, queue_ptes;
    uint32_t queue_bytes;
    /* What RM said over the status queue after the handover, if anything: the first dword of the first entry is
     * the RPC function number, and a message on that queue is the only proof the resource manager is running
     * that does not depend on a register this kernel may not be allowed to read. */
    uint32_t queue_write_ptr, queue_read_ptr;
    int rpc_seen;
    uint32_t rpc_function;
    /* What the channel itself did when it was handed a packet: the control word the arm wrote and what it reads
     * back as, and the first dwords the data window returns against the ones written.  "differs at dword 0" is
     * not a diagnosis; the pair of values is. */
    uint32_t ememc_after, emem_got0, emem_want0;
    /* The first four dwords the window hands back against the four that were written, and - when they disagree
     * - the offset in the channel where the packet's first dword was actually found (0xffffffff for nowhere in
     * the first kilobyte).  A packet at a different offset and a packet that never landed are different faults
     * with the same symptom, and the scan is what separates them. */
    uint32_t emem_got[4], emem_want[4], packet_at;
    int handover_have_channel;               /* the numbers above belong to the handover, not to the query */
    uint32_t handover_got[4], handover_want[4], handover_packet_at, handover_bursts;
    uint32_t handover_emem0_after;           /* the channel's first dword after the wait, not before it */
    /* The submit queue as it stood when the handover's wait ended.  Kept apart from the query's, which is a
     * different wait over a different packet, because the handover is the one that matters. */
    uint32_t handover_head, handover_tail, handover_doorbell;
    /* What the two submit pointers held the instant after the doorbell was written - before any wait could let
     * the FSP consume.  A tail that reads back as what was written is a store that landed; a tail that reads
     * back as something else is a doorbell nobody ever got, and the two look identical in a wait that ends. */
    uint32_t doorbell_head, doorbell_tail;
    int doorbell_landed;
    uint32_t handover_db_head, handover_db_tail;
    int handover_db_landed;
    int handover_submit_valid, handover_consumed, handover_retried;
    /* What the reference driver's own per-chip FSP table decides, read from the chip's architecture field
     * rather than assumed: nouveau's `nvkm_fsp_func.cot' in nvkm/subdev/fsp/{gh100,gb100,gb202}.c, the driver
     * that boots this family today.  The sizes are not interchangeable - Hopper's public key and signature are
     * both 384 bytes while both Blackwell generations want 97 and 96 - so one hard-coded number is how a real
     * GB207's own firmware file gets refused as malformed by the OS that should have accepted it.  A chip
     * whose architecture has no entry has no FSP mailbox in that driver either, and says so instead of poking
     * a page that belongs to something else. */
    int fsp_supported, arch_unknown;
    uint32_t arch;                              /* NV_PMC_BOOT_0 28:24, as the chip answered it */
    uint32_t cot_version, cot_hash_bytes, cot_pkey_bytes, cot_sig_bytes;
    /* The FSP's completion signal in the form the working driver reads it: the lockdown bit of the
     * coprocessor's capability word, and the falcon mailbox pair the FMC writes its boot-arguments address -
     * or an error code - into once it stops being zero. */
    uint32_t gsp_mailbox0, gsp_mailbox1, gsp_mailbox0_after, gsp_mailbox1_after;
    int lockdown, lockdown_after;               /* 1 = bit 13 of HWCFG2 set = still locked */
    /* The two waits in the units the reference driver uses, milliseconds, measured against the device's own
     * PTIMER (whose rate the cycle counter is timed against once per probe).  A wait bounded by a count of
     * uncached reads is tens of times too short for a coprocessor that takes four seconds to train, and a
     * timeout printed as silence is a measurement of the wrong moment. */
    uint64_t reply_ms, reply_ms_used, boot_ms, boot_ms_used;
    int wait_clocked;                           /* 0 = no PTIMER, so look counts bounded the waits instead */
    uint32_t ticks_per_ms;                      /* what the measurement came to, printed so it can be argued */
    uint64_t image_bytes, image_physical, image_crc32;
    uint8_t payload[GPU_FSP_PAYLOAD_BYTES];  /* the handover, assembled and never sent */
};
void gpu_fsp_note(const struct gpu_device *device, const struct boot_handoff *handoff);
const struct gpu_fsp_state *gpu_fsp_state(void);
void gpu_firmware_scan(struct gpu_firmware_state *f, const uint8_t *base, uint32_t total);
const struct gpu_firmware_state *gpu_firmware_state(void);
const char *gpu_firmware_role_name(unsigned role);
const struct gpu_module_state *gpu_module_state(void);
const struct scos_gpu_engine_ops *gpu_module_ops(void);
int gpu_module_bind(struct gpu_device *device, const struct boot_handoff *handoff);
int gpu_module_fill(int x, int y, int width, int height, uint32_t color);
int gpu_module_copy(int dst_x, int dst_y, int src_x, int src_y, int width, int height);
int gpu_module_wait_idle(void);
void gpu_module_report(char *out, size_t capacity, const struct boot_handoff *handoff);

/* Engine boundary used by the compositor.  Each returns 0 on success and a
 * negative value when no engine is bound or the bound driver declines the
 * operation, which is the caller's cue to run its own CPU path. */
int gpu_engine_fill_rectangle(int x, int y, int width, int height, uint32_t color);
int gpu_engine_screen_to_screen_blit(int dst_x, int dst_y, int width, int height,
                                     int src_x, int src_y);
int gpu_engine_wait_idle(void);
int gpu_engine_retrace_wait(void);
int gpu_engine_move_display(int x, int y);
/* 1 when an engine is bound and willing to try the operation; used to choose a
 * path, never to claim a result. */
int gpu_engine_available(void);
/* Whether the engine's front surface is the memory the console actually scans out.  A bound engine
 * on a *second* card drives its own aperture only, and painting the desktop there would move pixels
 * nobody can see; the compositor asks this before it hands a rectangle to the GPU. */
int gpu_engine_drives_output(void);
/* PCI Express link state of the i'th detected display function, read from its own capability
 * structure: 1 when generation and lanes hold what the slot negotiated, 0 when the function has no
 * Express capability at all (which is a real answer for a device behind a conventional slot). */
int gpu_link_state(int index, unsigned *generation, unsigned *lanes);

/* Every accelerated drawing path reports what the engine did and what the CPU kept for itself, in
 * source pixels.  The two counters exist so "the GPU is doing the drawing" is a measured statement in
 * the boot report and the tests, not an inference from the fact that a driver loaded. */
void gpu_engine_note_work(uint32_t by_engine, uint32_t by_cpu);
void gpu_engine_work_totals(uint32_t *by_engine, uint32_t *by_cpu);

struct gpu_driver {
    const char *family;
    enum gpu_port_state state;
    const struct gpu_engine_ops *ops;
    /* What still stands between this family and GPU work in SCos, stated from the
     * measured source rather than assumed.  Printed by `gpu_report()` so a log
     * explains a fallback instead of hiding it. */
    const char *gap;
    /* Called only when the family is matched *and* SCos is allowed to own the
     * display; NULL while no port exists.  May map MMIO, never modeset. */
    int (*prepare)(struct gpu_device *device);
};

#endif /* SCOS_GPU_H */

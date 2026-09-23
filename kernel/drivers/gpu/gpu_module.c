/* Load exactly one GPU display module, the one this machine's chip needs.
 *
 * Why the selection happens before the kernel runs (boot/uefi/main.c) and the *validation* happens
 * here: the firmware has the filesystem, the kernel has the page tables and the device.  So the
 * stub reads the file named by the shared matcher and copies it into the executable region carved out
 * of the boot arena; this file then proves the bytes are what it claims to be, cross-checks the
 * family against the kernel's own detection of the same function, relocates it, binds it, and
 * exercises it.  Anything the stub got wrong is caught here rather than executed.
 *
 * The checks, in order, each with the reason it exists:
 *   magic, ABI, header size      - a file from a different build must never reach the entry point;
 *   header word checksum         - a torn or partially written directory entry gives sane sizes;
 *   payload CRC32                - the same polynomial the stub uses for the kernel image, so a
 *                                  corrupt module is a log line instead of a jump into garbage;
 *   sizes vs the region          - nothing may be written outside the carve-out;
 *   relocation types and targets - the loader implements four kinds; a module needing a fifth means
 *                                  the packer should have been changed, not the kernel;
 *   family name                  - must equal what this kernel detected for this function;
 *   id list                      - every id the module claims must appear in the generated table
 *                                  for that family, so a module cannot widen what detection binds;
 *   TAKES_DISPLAY flag           - refused, because reprogramming the display from a module is the
 *                                  operation that can destroy the only console; no module sets it.
 *
 * After that the module's init runs, and if it binds, a self-test drives the engine and reads the
 * result back through the same device memory the display scans out.  A module that fails the
 * self-test is detached: `gpu_engine_*()` returns -1 and the CPU compositor carries on.  That is
 * the whole reason the self-test exists - the alternative is a half-working blitter painting the
 * desktop.
 */
#include "scos.h"
#include "gpu.h"
#include "gpu_abi.h"

#define MODULE_LOG_PREFIX "gpu: module "

static u32 mod_crc32(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    u32 crc = ~0u;
    while (length--) {
        crc ^= *bytes++;
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xedb88320u & -(int)(crc & 1));
    }
    return ~crc;
}

struct module_record {
    int present;                 /* bytes were found and validated */
    int bound;                   /* init succeeded and the self-test passed */
    char name[24];
    char family[SCOS_GPU_FAMILY_NAME];
    char describe[160];
    u64 region, region_bytes;
    u64 file_bytes, load_bytes, image_bytes, resident_bytes;
    u32 header_crc, computed_crc;
    u32 reloc_count, id_count;
    u32 store_count;             /* modules on the disk, including the ones never opened */
    u32 store_bytes, opened_bytes, unopened_bytes;
    int refusal;                 /* first failing predicate, for the log line */
    const struct scos_gpu_engine_ops *ops;
    int self_test_pixels, self_test_matches;
    struct scos_gpu_surface front, back;
    int identification_only;      /* bound module exposes no engine operation: inventory only */
    u64 test_virtual;            /* CPU-visible start of the surface the engine writes */
    u32 test_stride, test_width, test_height;
};

static struct module_record record;
static struct scos_gpu_exports exports;
static struct scos_gpu_device_info info;
struct mapping {
    u64 physical, virtual, bytes;
};
/* Every mapping a module can reach, including the one the kernel made first for the self-test.  A
 * module may only read or write inside one of these, and only through the handle we handed back,
 * which is what keeps a bug in a port from turning into a write to an arbitrary physical address. */
static struct mapping mapping[4];
static u64 window_base, window_bytes;
static u32 saved[16 * 16];
static int saved_valid;

/* ------------------------------------------------------------ exports ---- */

static void module_log(const char *line)
{
    /* klog truncates near 120 bytes, and a module that overruns its own buffer would have shown
     * that already, so the line is copied out and logged without further formatting. */
    char buffer[160];
    unsigned i = 0;
    while (i + 1 < sizeof(buffer) && line[i]) { buffer[i] = line[i]; i++; }
    buffer[i] = 0;
    klog("gpu: %s", buffer);
}

#define DEVICE_MAP_LIMIT (4096u * 2048u)   /* the kernel's per-mapping cap, see device_map() */

static struct mapping *mapping_find(u64 handle)
{
    for (unsigned i = 0; i < sizeof(mapping) / sizeof(mapping[0]); i++)
        if (mapping[i].virtual && handle >= mapping[i].virtual &&
            handle + 4 <= mapping[i].virtual + mapping[i].bytes) return &mapping[i];
    return 0;
}

static u32 module_read32(u64 handle, u32 offset)
{
    struct mapping *m = mapping_find(handle);
    if (!m || (u64)offset + 4 > m->bytes) return 0xffffffffu;
    return *(volatile uint32_t*)(uintptr_t)(handle + offset);
}

static void module_write32(u64 handle, u32 offset, u32 value)
{
    struct mapping *m = mapping_find(handle);
    if (!m || (u64)offset + 4 > m->bytes) return;
    *(volatile uint32_t*)(uintptr_t)(handle + offset) = value;
}

static u64 mapping_size(u64 handle)
{
    struct mapping *m = mapping_find(handle);
    return m ? m->bytes - (handle - m->virtual) : 0;
}

static u64 map_window(u64 physical, u64 bytes, u32 write_combine)
{
    if (bytes > DEVICE_MAP_LIMIT) bytes = DEVICE_MAP_LIMIT;
    for (unsigned i = 0; i < sizeof(mapping) / sizeof(mapping[0]); i++)
        if (mapping[i].physical == physical && mapping[i].bytes >= bytes) return mapping[i].virtual;
    u64 mapped_bytes = 0;
    void *mapped = device_map(physical, bytes, write_combine ? 1 : 0, &mapped_bytes);
    if (!mapped) return 0;
    for (unsigned i = 0; i < sizeof(mapping) / sizeof(mapping[0]); i++) {
        if (mapping[i].virtual) continue;
        mapping[i] = (struct mapping){ .physical = physical, .virtual = (u64)(uintptr_t)mapped,
                                       .bytes = mapped_bytes };
        return (u64)(uintptr_t)mapped;
    }
    return 0;                              /* out of window slots: refuse rather than replace */
}

static u64 module_map(u64 physical, u64 bytes, u32 write_combine, u64 *mapped_bytes)
{
    /* Only the BARs of the function this module was loaded for may be mapped.  A module cannot walk
     * to another device because it is not given a way to name one, and it cannot walk past the end
     * of a BAR because the window table bounds every access. */
    int allowed = 0;
    for (int bar = 0; bar < 6; bar++)
        if (physical >= info.bar_base[bar] &&
            physical + bytes <= info.bar_base[bar] + info.bar_bytes[bar]) allowed = 1;
    if (!allowed) {
        module_log("refused a mapping outside this function's BARs");
        return 0;
    }
    u64 handle = map_window(physical, bytes, write_combine);
    if (!handle) { module_log("device memory mapping failed"); return 0; }
    /* The truth, not the request: a mapping is capped, and a driver that is told the real size
     * bounds its own accesses instead of faulting on a refused read later. */
    if (mapped_bytes) *mapped_bytes = mapping_size(handle);
    return handle;
}

static u64 module_ticks(void)
{
    return rdtsc();
}

static void module_spin(u32 iterations)
{
    for (u32 i = 0; i < iterations; i++) __asm__ volatile("pause");
}

static u32 module_wait_bit(u64 handle, u32 offset, u32 mask, u32 expect, u64 timeout_ticks)
{
    u64 deadline = rdtsc() + timeout_ticks;
    for (;;) {
        if ((module_read32(handle, offset) & mask) == expect) return 1;
        if (rdtsc() > deadline) return 0;
        module_spin(32);
    }
}

static u64 module_alloc(u64 bytes)
{
    if (!bytes || bytes > 4u * 1024 * 1024) return 0;
    return (u64)(uintptr_t)palloc_owned((size_t)bytes, HEAP_WM);
}

/* ---------------------------------------------------------- validation ---- */

static u32 header_checksum_of(const struct scos_gpu_module_header *header)
{
    const u32 *words = (const u32*)header;
    u32 total = 0;
    for (unsigned i = 0; i < SCOS_GPU_MODULE_HEADER_SIZE / 4; i++)
        total = (total + (i == 3 ? 0u : words[i])) & 0xffffffffu;
    return total;
}

static const char *reject_reason(int code)
{
    switch (code) {
    case 1: return "no module bytes in the boot arena";
    case 2: return "magic is not SGPM";
    case 3: return "ABI does not match this kernel";
    case 4: return "header size mismatch";
    case 5: return "header checksum mismatch";
    case 6: return "payload CRC mismatch (the file is corrupt)";
    case 7: return "sizes exceed the reserved module region";
    case 8: return "blocks are not contiguous";
    case 9: return "relocation table runs past the file";
    case 10: return "an entry point is outside the read/execute block";
    case 11: return "family name disagrees with detection";
    case 12: return "an id the module claims is not in the generated table";
    case 13: return "module asks to reprogram the display, which is refused";
    case 14: return "a relocation targets a bad type, place, or address";
    case 15: return "the module's init refused this device";
    case 16: return "engine self-test failed; detached";
    case 17: return "no id rules claimed";
    default: return "unspecified";
    }
}

static u32 rd32le(const u8 *at)
{
    return (u32)at[0] | ((u32)at[1] << 8) | ((u32)at[2] << 16) | ((u32)at[3] << 24);
}

static int same_name(const char *a, const u8 *b)
{
    for (unsigned i = 0; i < 8; i++) {
        char x = a[i], y = (char)(b[i] | 0x20);       /* the index holds upper-case 8.3 stems */
        if (x >= 'A' && x <= 'Z') x = (char)(x | 0x20);
        if (!x || x != y) return x == 0 && (y == 0 || y == ' ');
    }
    return 1;
}

/* Add up what the disk holds, from the index the firmware copied into the arena, so the report can
 * state the cost of the unused families in bytes instead of adjectives.  An index that disagrees with
 * the stub is ignored rather than trusted: it is used for reporting only, never for loading. */
static void measure_store(const struct boot_handoff *handoff)
{
    const u8 *idx = (const u8*)(uintptr_t)handoff->index_address;
    u32 size = handoff->module_index_size;
    u64 limit = GPU_MODULE_REGION * (handoff->module_store_count ? handoff->module_store_count : 1);
    record.store_bytes = record.opened_bytes = record.unopened_bytes = 0;
    if (!idx || size < 8u) return;
    u32 count = rd32le(idx), total = rd32le(idx + 4);
    /* `total' counts the module files, not this table, so it is bounded by the region a module could
     * ever be loaded into; anything larger means the index and the stub disagree and gets ignored. */
    if (count != handoff->module_store_count || 8u + count * 16u > size || total > limit) return;
    record.store_bytes = total;
    for (u32 i = 0; i < count; i++) {
        const u8 *e = idx + 8u + i * 16u;
        u32 bytes = rd32le(e + 8);
        if (same_name(handoff->module_name, e)) record.opened_bytes += bytes;
        else record.unopened_bytes += bytes;
    }
}

static int validate(const struct boot_handoff *handoff, const struct gpu_device *device,
                    const struct scos_gpu_module_header **out_header)
{
    struct scos_gpu_module_header *header;
    if (!handoff->module_bytes || !handoff->module_address) return 1;
    const u32 hdr = SCOS_GPU_MODULE_HEADER_SIZE;
    u64 region = GPU_MODULE_AREA(handoff->arena_start);
    u64 limit = GPU_MODULE_REGION;
    if (handoff->module_address != region || handoff->module_bytes > limit) return 7;
    record.region = region;
    record.region_bytes = limit;
    record.file_bytes = handoff->module_bytes;
    header = (struct scos_gpu_module_header*)(uintptr_t)region;
    if (header->magic != SCOS_GPU_MODULE_MAGIC) return 2;
    if (header->abi != SCOS_GPU_ABI_VERSION) return 3;
    if (header->header_size != hdr) return 4;
    if (header_checksum_of(header) != header->header_words) return 5;
    u64 payload_length = handoff->module_bytes - hdr;
    if (mod_crc32((void*)(uintptr_t)(region + hdr), (size_t)payload_length) != header->payload_crc)
        return 6;
    /* The origin rule for the whole format: an offset in a module header or relocation table counts
     * from byte zero of the file, which is also where this copy lives.  So the loaded blocks occupy
     * [hdr, hdr + load_size), .bss continues to hdr + image_size, and the header itself is read-only
     * metadata no relocation may touch. */
    u64 load_end = (u64)hdr + header->load_size;
    u64 image_end = (u64)hdr + header->image_size;
    if (header->rxe_off != hdr) return 7;
    if (load_end > handoff->module_bytes - (u64)header->reloc_count * 16u) return 7;
    if (image_end > handoff->module_bytes) return 9;   /* .bss bytes must at least be reserved */
    if (image_end > limit) return 7;
    if (header->data_off != header->rxe_off + header->rxe_size) return 8;
    if (header->load_size != header->rxe_size + header->data_size) return 7;
    if ((u64)header->reloc_off + header->reloc_count * 16u > handoff->module_bytes) return 9;
    /* Stored metadata must stay clear of the image, .bss included: the module writes to its own
     * globals, so a table that shared those bytes would be edited by the driver as it ran. */
    if (header->reloc_off < image_end) return 9;
    if (header->module_min_bytes > handoff->module_bytes) return 9;
    /* An entry point has to be inside the read/execute block: .text is where the code is, and the
     * region holding it is the only one the kernel marks executable. */
    if (header->entry_init < hdr || header->entry_init >= (u64)hdr + header->rxe_size) return 10;
    if (header->entry_teardown &&
        (header->entry_teardown < hdr ||
         header->entry_teardown >= (u64)hdr + header->rxe_size)) return 10;
    if (header->id_count == 0) return 17;
    if ((u64)header->id_off + header->id_count * sizeof(struct scos_gpu_pci_id) > load_end)
        return 9;
    if (header->flags & SCOS_GPU_MODULE_TAKES_DISPLAY) return 13;
    const char *family = (const char*)header->family;
    if (!device->match || strcmp(family, device->match->family) != 0) return 11;
    for (unsigned i = 0; i < header->reloc_count; i++) {
        struct scos_gpu_reloc *r = (struct scos_gpu_reloc*)(uintptr_t)
            (region + header->reloc_off + (u64)i * 16u);
        u32 width = r->type == SCOS_GPU_RELOC_ABS64 ? 8u : 4u;
        if (r->type != SCOS_GPU_RELOC_ABS64 && r->type != SCOS_GPU_RELOC_ABS32S &&
            r->type != SCOS_GPU_RELOC_PC32 && r->type != SCOS_GPU_RELOC_PLT32) return 14;
        if (r->offset < hdr || (u64)r->offset + width > load_end) return 14;
        if ((u64)r->symbol + (u64)(r->addend < 0 ? -r->addend : r->addend) > image_end) return 14;
        /* A module may not be pointed at kernel memory it was not given: every reference must land
         * inside its own image, which is what makes "no imports" enforceable rather than aspirational. */
        if (r->symbol < hdr) return 14;
    }
    /* Cross-check every id the module claims against what the generated table knows for its family.
     * The module is allowed to claim fewer, never more: an engine cannot widen detection. */
    struct scos_gpu_pci_id *claims = (struct scos_gpu_pci_id*)(uintptr_t)(region + header->id_off);
    for (unsigned i = 0; i < header->id_count; i++) {
        int known = 0;
        for (unsigned k = 0; k < device->match->id_count; k++)
            if (device->match->ids[k].vendor == claims[i].vendor &&
                device->match->ids[k].device == claims[i].device) { known = 1; break; }
        if (!known) return 12;
    }
    record.load_bytes = header->load_size;
    record.image_bytes = header->image_size;
    record.resident_bytes = (u64)hdr + header->image_size;
    record.reloc_count = header->reloc_count;
    record.id_count = header->id_count;
    *out_header = header;
    record.header_crc = header->payload_crc;
    record.computed_crc = mod_crc32((void*)(uintptr_t)(region + SCOS_GPU_MODULE_HEADER_SIZE),
                                    (size_t)payload_length);
    return 0;
}

static int relocate(u64 base, const struct scos_gpu_module_header *header)
{
    for (unsigned i = 0; i < header->reloc_count; i++) {
        struct scos_gpu_reloc *r = (struct scos_gpu_reloc*)(uintptr_t)
            (base + header->reloc_off + (u64)i * 16u);
        u64 place = base + r->offset;
        u64 target = base + r->symbol;
        /* The same predicate validate() applies, re-checked here: the loader must not fault on a
         * module whose header was rewritten between the two passes (a boot disk is read while the
         * firmware still owns the controller, and a torn read is exactly what this catches). */
        u32 width = r->type == SCOS_GPU_RELOC_ABS64 ? 8u : 4u;
        u64 load_end = SCOS_GPU_MODULE_HEADER_SIZE + (u64)header->load_size;
        if (r->offset < SCOS_GPU_MODULE_HEADER_SIZE || (u64)r->offset + width > load_end ||
            r->symbol < SCOS_GPU_MODULE_HEADER_SIZE ||
            (u64)r->symbol >= SCOS_GPU_MODULE_HEADER_SIZE + header->image_size) return -1;
        switch (r->type) {
        case SCOS_GPU_RELOC_ABS64:
            *(uint64_t*)(uintptr_t)place = target + (int64_t)r->addend;
            break;
        case SCOS_GPU_RELOC_ABS32S:
            /* The module region sits in the boot arena, which the firmware allocated below the
             * 4 GiB line, so a 32-bit absolute reference can only be satisfied by that address. */
            *(uint32_t*)(uintptr_t)place = (uint32_t)(target + (int64_t)r->addend);
            break;
        case SCOS_GPU_RELOC_PC32:
        case SCOS_GPU_RELOC_PLT32:
            /* S + A - P.  Both P and S are measured from the same base here, so the base cancels and
             * `place' itself is the address the 4 bytes are written to - adding another 4 (the width
             * of the field) is wrong for RELA records, whose addend already carries the -4 that
             * x86-64 uses for PC-relative references. */
            *(int32_t*)(uintptr_t)place =
                (int32_t)((int64_t)r->symbol + (int64_t)r->addend - (int64_t)r->offset);
            break;
        default: break;
        }
    }
    return 0;
}

/* ---------------------------------------------------------- self-test ---- */

/* Drive the engine into device memory and read the result back through the mapping the display
 * itself scans out of.  This is deliberately not a "call it and see if it returns 0" test: the
 * register sequence can be accepted by a card that then draws nothing, and only the pixels say
 * whether the port is right.  Any damage is repaired by the CPU immediately, before this function
 * returns, because the LFB is not the desktop's source of truth - `screen.px` is, and the next flip
 * repaints whatever the test touched. */
/* 0 = refused: the module cannot be used for drawing and is detached.  1 = the engine painted and read
 * back what it was asked to.  2 = bound, and this module deliberately exposes no engine operation: see
 * the comment on the empty-ops case below. */
static int engine_self_test(void)
{
    const struct scos_gpu_engine_ops *ops = record.ops;
    record.identification_only = 0;
    if (!ops) return 0;
    if (!ops->fill) {
        /* A driver that reads the chip and offers no rectangle operation is still worth binding: it is
         * the difference between "nothing in this tree knows this chip" and "the family was identified
         * from the chip's own register block".  There is nothing to paint-verify in that case, and
         * detaching the module would throw the inventory away with it, so the verdict is its own (2)
         * rather than a failure.  No drawing path is affected either way: every one of them checks the
         * operation pointer first, and this module leaves it null. */
        record.identification_only = 1;
        module_log("bound for identification: it offers no rectangle operation, so there is nothing to "
                   "paint-verify and the CPU keeps compositing");
        return 2;
    }
    u32 width = record.test_width, height = record.test_height;
    if (!record.test_virtual) {
        module_log("this function has no CPU-readable frame buffer; leaving the engine unbound");
        return 0;
    }
    if (width < 64 || height < 64) return 0;
    s32 x = (s32)width - 48, y = (s32)height - 32;
    volatile uint32_t *fb = (volatile uint32_t*)(uintptr_t)record.test_virtual;
    u32 stride = record.test_stride;
    /* Save the region we are about to overwrite, then restore it through the CPU at the end. */
    for (int row = 0; row < 16; row++)
        for (int col = 0; col < 16; col++)
            saved[row * 16 + col] = fb[(u32)y * stride + (u32)x + (u32)col];
    saved_valid = 1;
    const u32 colour = 0x00c0ffeeu;
    if (ops->fill(ops->context, SCOS_GPU_SURFACE_FRONT, x, y, 16, 16, colour) != 0) {
        module_log("engine refused the self-test fill");
        return 0;
    }
    if (ops->wait_idle && ops->wait_idle(ops->context) != 0) {
        module_log("engine did not settle after the self-test fill");
        return 0;
    }
    /* QEMU's model of this chip reads the size fields of the blit path as exact counts while the
     * databook convention (which the port follows) programs count-1, so a fill is checked over the
     * area both interpretations agree on, and mismatches are counted, not rounded away. */
    int matches = 0, pixels = 0;
    for (int row = 0; row < 15; row++)
        for (int col = 0; col < 15; col++) {
            pixels++;
            if (fb[((u32)y + (u32)row) * stride + (u32)x + (u32)col] == colour) matches++;
        }
    record.self_test_pixels = pixels;
    record.self_test_matches = matches;
    int fill_ok = matches == pixels;
    int copy_ok = 1;
    if (fill_ok && ops->copy) {
        /* Second operation: move what the engine just drew 16 pixels to the left and check both
         * rectangles, which catches a wrong source pitch or offset that a fill alone cannot see. */
        if (ops->copy(ops->context, SCOS_GPU_SURFACE_FRONT, x - 16, y, SCOS_GPU_SURFACE_FRONT,
                      x, y, 16, 16) != 0) copy_ok = 0;
        else {
            if (ops->wait_idle) ops->wait_idle(ops->context);
            int seen = 0;
            for (int row = 0; row < 15; row++)
                for (int col = 0; col < 15; col++)
                    if (fb[((u32)y + (u32)row) * stride + (u32)(x - 16) + (u32)col] == colour)
                        seen++;
            copy_ok = seen == pixels;
        }
        if (!copy_ok) module_log("engine blit did not land; check pitch and offset encoding");
    }
    for (int row = 0; row < 16; row++)
        for (int col = 0; col < 16; col++)
            fb[(u32)y * stride + (u32)x + (u32)col] = saved[row * 16 + col];
    if (ops->copy) {
        /* Undo the shifted copy too, by copying the restored original back to the left. */
        ops->copy(ops->context, SCOS_GPU_SURFACE_FRONT, x - 16, y, SCOS_GPU_SURFACE_FRONT, x, y,
                  16, 16);
        if (ops->wait_idle) ops->wait_idle(ops->context);
        for (int row = 0; row < 16; row++)
            for (int col = 0; col < 16; col++)
                fb[(u32)y * stride + (u32)(x - 16) + (u32)col] = 0u;   /* repainted by the WM */
    }
    __asm__ volatile("sfence" ::: "memory");
    saved_valid = 0;
    return fill_ok && copy_ok;
}

/* --------------------------------------------------------------- entry ---- */

int gpu_module_bind(struct gpu_device *device, const struct boot_handoff *handoff)
{
    const struct scos_gpu_module_header *header = 0;
    record.store_count = handoff->module_store_count;
    measure_store(handoff);
    /* Say what the disk offers and what this machine opened, on every boot whether or not the module
     * turns out to be usable: the whole point of one file per family is that a machine with fifteen
     * modules on its disk reads exactly one, and the only proof of that which costs nothing to run is
     * a log line naming both numbers. */
    klog(MODULE_LOG_PREFIX "%u module(s), %u B on the boot disk: %u B opened for this chip, %u B never "
         "read", record.store_count, record.store_bytes, record.opened_bytes, record.unopened_bytes);
    record.present = 0;
    record.bound = 0;
    record.ops = 0;
    for (unsigned i = 0; i < sizeof(mapping) / sizeof(mapping[0]); i++)
        mapping[i] = (struct mapping){0, 0, 0};
    if (handoff->module_name[0]) {
        unsigned i = 0;
        while (i + 1 < sizeof(record.name) && handoff->module_name[i]) {
            record.name[i] = handoff->module_name[i]; i++;
        }
        record.name[i] = 0;
    }
    int problem = validate(handoff, device, &header);
    record.refusal = problem;
    if (problem) {
        if (problem != 1)
            klog(MODULE_LOG_PREFIX "rejected: %s", reject_reason(problem));
        return -1;
    }
    record.present = 1;
    unsigned i = 0;
    while (i + 1 < sizeof(record.family) && header->family[i]) {
        record.family[i] = header->family[i]; i++;
    }
    record.family[i] = 0;
    if (relocate(record.region, header) != 0) {
        record.refusal = 14;
        klog(MODULE_LOG_PREFIX "rejected: a relocation left the image after loading");
        return -1;
    }
    /* .bss is part of the image size but not stored in the file: zero it before the module runs. */
    if (header->bss_size)
        memset((void*)(uintptr_t)(record.region + SCOS_GPU_MODULE_HEADER_SIZE + header->load_size),
               0, header->bss_size);
    klog(MODULE_LOG_PREFIX "%s (%u B code+data, %u B zeroed, %u reloc, %u id rule(s)) validated",
         record.name, header->load_size, header->bss_size, record.reloc_count, record.id_count);

    info = (struct scos_gpu_device_info){
        .size = sizeof(info),
        .vendor = device->vendor, .device = device->device,
        .bus = device->bus, .slot = device->slot, .function = device->function,
        .revision = device->revision,
        .framebuffer_base = handoff->framebuffer.base,
        .framebuffer_bytes = handoff->framebuffer.size,
        .framebuffer_pointer = handoff->framebuffer.base,
        .width = (u32)handoff->framebuffer.width, .height = (u32)handoff->framebuffer.height,
        .pitch = (u32)handoff->framebuffer.stride * 4u, .bytes_per_pixel = 4u,
    };
    for (int bar = 0; bar < 6; bar++) {
        u64 base = 0, bytes = 0;
        /* Sizing a BAR means writing all-ones into it.  Detection never does that; a *bound* engine
         * may, because at this point the kernel owns the function and has the module's own mapping
         * to fall back on, and it is restored by pci_memory_bar() itself. */
        if (pci_memory_bar(device->bus, device->slot, device->function, (u8)bar, &base, &bytes)) {
            info.bar_base[bar] = base;
            info.bar_bytes[bar] = bytes;
        }
    }
    /* The self-test and the module must look at the same memory: the surface *this function*
     * displays from.  When it is the one feeding the console, that is the LFB the firmware set up.
     * When it is a second card - an emulated primary in a VM, or onboard plus discrete - the console
     * belongs to another function altogether, so the aperture of this one is used and the console is
     * never touched.  Guessing here would mean either a test that proves nothing or a module asked to
     * draw into memory it cannot reach. */
    {
        u64 need = (u64)info.width * info.height * info.bytes_per_pixel;
        u64 physical = 0, physical_bytes = 0;
        if (device->is_scanout) {
            physical = handoff->framebuffer.base;
            physical_bytes = handoff->framebuffer.size;
        } else {
            for (int bar = 0; bar < 6; bar++)
                if (info.bar_base[bar] && info.bar_bytes[bar] >= need) {
                    physical = info.bar_base[bar];
                    physical_bytes = info.bar_bytes[bar];
                    break;
                }
        }
        /* The module bounds every operation against the device memory it was given, so it has to be
         * told how much there is: an engine handed only the console's extent would refuse blits into
         * the rest of its own aperture, and one handed more than exists would write off the end. */
        info.vram_bytes = physical_bytes > need ? physical_bytes : need;
        record.test_virtual = physical && need ? map_window(physical, need, 0) : 0;
        record.test_stride = device->is_scanout ? (u32)handoff->framebuffer.stride : info.pitch / 4u;
        record.test_width = info.width;
        record.test_height = info.height;
        if (record.test_virtual && !device->is_scanout) {
            info.framebuffer_base = physical;
            info.framebuffer_bytes = need;
            info.framebuffer_pointer = record.test_virtual;
            info.pitch = record.test_stride * info.bytes_per_pixel;
        }
        klog(MODULE_LOG_PREFIX "test surface: %s 0x%x+%u, %ux%u stride %u",
             device->is_scanout ? "console LFB at" : "aperture of this function at",
             (unsigned)(u32)physical, (unsigned)need, info.width, info.height, record.test_stride);
    }
    exports = (struct scos_gpu_exports){
        .size = sizeof(exports), .abi = SCOS_GPU_ABI_VERSION, .device = &info,
        .log = module_log, .map = module_map, .read32 = module_read32,
        .write32 = module_write32, .ticks = module_ticks, .spin = module_spin,
        .wait_bit = module_wait_bit, .alloc = module_alloc,
    };
    scos_gpu_module_init entry = (scos_gpu_module_init)(uintptr_t)(record.region +
                                                                    header->entry_init);
    struct scos_gpu_engine_ops *ops = 0;
    if (entry(&exports, &ops) != 0 || !ops) {
        record.refusal = 15;
        klog(MODULE_LOG_PREFIX "did not take the device: %s", reject_reason(15));
        return -1;
    }
    if (ops->abi != SCOS_GPU_ABI_VERSION || ops->size > sizeof(*ops) ||
        ops->size < 7u * sizeof(void *)) {   /* must at least reach wait_idle */
        record.refusal = 3;
        klog(MODULE_LOG_PREFIX "reports an engine table this kernel cannot call: size %u abi %u "
             "(kernel has %u abi %u)", ops->size, ops->abi, (u32)sizeof(*ops),
             (u32)SCOS_GPU_ABI_VERSION);
        return -1;
    }
    record.ops = ops;
    record.front = (struct scos_gpu_surface){
        .offset_bytes = 0, .pitch_bytes = info.pitch, .width = info.width, .height = info.height,
        .bytes_per_pixel = info.bytes_per_pixel,
    };
    if (ops->set_surfaces) ops->set_surfaces(ops->context, &record.front, 0);
    u32 window_offset = 0, window_size = 0;
    if (ops->vram_window && ops->vram_window(ops->context, &window_offset, &window_size) == 0 &&
        window_size) {
        record.back.bytes_per_pixel = info.bytes_per_pixel;
        klog(MODULE_LOG_PREFIX "advises a %u B off-screen window at +0x%x; not enabled, the "
             "compositor back buffer stays in RAM until the write-combining policy is settled",
             window_size, window_offset);
        window_base = record.front.offset_bytes + window_offset;
        window_bytes = window_size;
    }
    int self_test_verdict = engine_self_test();
    if (self_test_verdict == 0) {
        record.refusal = 16;
        record.ops = 0;
        klog(MODULE_LOG_PREFIX "self-test failed (%d/%d pixels); engine detached, CPU compositor "
             "keeps the screen", record.self_test_matches, record.self_test_pixels);
        if (header->entry_teardown)
            ((scos_gpu_module_teardown)(uintptr_t)(record.region + header->entry_teardown))(ops);
        return -1;
    }
    if (ops->describe) {
        const char *text = ops->describe(ops->context);
        unsigned n = 0;
        while (n + 1 < sizeof(record.describe) && text && text[n]) {
            record.describe[n] = text[n]; n++;
        }
        record.describe[n] = 0;
    }
    record.bound = 1;
    device->bound = 1;
    device->module_ops = ops;
    if (self_test_verdict == 2) {
        /* No readback number, because nothing was asked of the card that could be read back.  Printing
         * "0/0 pixels" as a verification result would be the kind of confident nonsense this loader
         * exists to refuse, and printing a real number here would be a lie. */
        klog(MODULE_LOG_PREFIX "%s bound: it read the chip's own registers and describes them; no "
             "engine was offered to verify", record.name);
    } else {
        klog(MODULE_LOG_PREFIX "%s bound: engine verified in device memory (%d/%d pixels)",
             record.name, record.self_test_matches, record.self_test_pixels);
    }
    return 0;
}

const struct gpu_module_state *gpu_module_state(void)
{
    static struct gpu_module_state snapshot;
    snapshot.present = record.present;
    snapshot.bound = record.bound;
    snapshot.identification_only = record.identification_only;
    for (unsigned i = 0; i < sizeof(snapshot.name); i++)
        snapshot.name[i] = i < sizeof(record.name) ? record.name[i] : 0;
    for (unsigned i = 0; i < sizeof(snapshot.family); i++)
        snapshot.family[i] = i < sizeof(record.family) ? record.family[i] : 0;
    for (unsigned i = 0; i < sizeof(snapshot.describe); i++)
        snapshot.describe[i] = i < sizeof(record.describe) ? record.describe[i] : 0;
    snapshot.resident_bytes = record.resident_bytes;
    snapshot.file_bytes = record.file_bytes;
    snapshot.store_count = record.store_count;
    snapshot.store_bytes = record.store_bytes;
    snapshot.opened_bytes = record.opened_bytes;
    snapshot.unopened_bytes = record.unopened_bytes;
    snapshot.reloc_count = record.reloc_count;
    snapshot.id_count = record.id_count;
    snapshot.refusal = record.refusal;
    snapshot.self_test_pixels = record.self_test_pixels;
    snapshot.self_test_matches = record.self_test_matches;
    return &snapshot;
}

const struct scos_gpu_engine_ops *gpu_module_ops(void)
{
    return record.bound ? record.ops : 0;
}

/* Used by the compositor boundary; -1 means "not available", never "not needed". */
int gpu_module_fill(int x, int y, int width, int height, uint32_t color)
{
    const struct scos_gpu_engine_ops *ops = record.bound ? record.ops : 0;
    if (!ops || !ops->fill) return -1;
    return ops->fill(ops->context, SCOS_GPU_SURFACE_FRONT, x, y, width, height, color);
}

int gpu_module_copy(int dst_x, int dst_y, int src_x, int src_y, int width, int height)
{
    const struct scos_gpu_engine_ops *ops = record.bound ? record.ops : 0;
    if (!ops || !ops->copy) return -1;
    return ops->copy(ops->context, SCOS_GPU_SURFACE_FRONT, dst_x, dst_y, SCOS_GPU_SURFACE_FRONT,
                     src_x, src_y, width, height);
}

int gpu_module_wait_idle(void)
{
    const struct scos_gpu_engine_ops *ops = record.bound ? record.ops : 0;
    if (!ops || !ops->wait_idle) return -1;
    return ops->wait_idle(ops->context);
}

/* The kernel log and the `graphics` report both use a bounded writer (see gpu_detect.c), because
 * there is no snprintf here and a report must never overrun the caller's buffer. */
struct writer { char *at, *end; };
static void put(struct writer *w, const char *text)
{
    while (*text && w->at + 1 < w->end) *w->at++ = *text++;
    *w->at = 0;
}
static void put_number(struct writer *w, unsigned value)
{
    char digits[12];
    int count = 0;
    do { digits[count++] = (char)('0' + value % 10u); value /= 10u; } while (value);
    while (count && w->at + 1 < w->end) *w->at++ = digits[--count];
    *w->at = 0;
}

void gpu_module_report(char *out, size_t capacity, const struct boot_handoff *handoff)
{
    (void)handoff;
    struct writer writer = { out + strlen(out), out + capacity };
    struct writer *w = &writer;
    if (!record.present) {
        put(w, "Driver module: none loaded");
        if (record.refusal && record.refusal != 1) {
            put(w, " - ");
            put(w, reject_reason(record.refusal));
        }
        put(w, "\n");
        put(w, "Modules available on the boot disk: ");
        put_number(w, record.store_count);
        put(w, ", opened: 0, so every family that is not this machine's costs no RAM and no CPU\n");
        return;
    }
    put(w, "Driver module loaded from storage: ");
    put(w, record.name);
    put(w, ", family ");
    put(w, record.family);
    put(w, "\n");
    put(w, "Resident in the boot arena: ");
    put_number(w, (unsigned)record.resident_bytes);
    put(w, " of ");
    put_number(w, (unsigned)record.region_bytes);
    put(w, " bytes reserved; relocations applied: ");
    put_number(w, record.reloc_count);
    put(w, "; id rules claimed: ");
    put_number(w, record.id_count);
    put(w, "\n");
    put(w, "Modules on the boot disk: ");
    put_number(w, record.store_count);
    put(w, ", opened by the firmware: 1 (");
    put_number(w, (unsigned)record.file_bytes);
    put(w, " bytes)");
    if (record.store_count > 1) {
        put(w, "; the other ");
        put_number(w, record.store_count - 1);
        put(w, " were never opened (");
        put_number(w, record.unopened_bytes);
        put(w, " B no RAM was given for)");
    }
    put(w, "\n");
    put(w, "Engine self-test: ");
    put_number(w, (unsigned)record.self_test_matches);
    put(w, " of ");
    put_number(w, (unsigned)record.self_test_pixels);
    put(w, " device pixels, engine ");
    put(w, record.bound ? "bound" : "detached");
    put(w, "\n");
    if (record.describe[0]) {
        put(w, "Engine: ");
        put(w, record.describe);
        put(w, "\n");
    }
}

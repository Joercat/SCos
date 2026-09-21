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
    u32 store_bytes;
    int refusal;                 /* first failing predicate, for the log line */
    const struct scos_gpu_engine_ops *ops;
    int self_test_pixels, self_test_matches;
    struct scos_gpu_surface front, back;
};

static struct module_record record;
static struct scos_gpu_exports exports;
static struct scos_gpu_device_info info;
static u64 mmio_handle, mmio_phys, mmio_bytes;
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

static u32 module_read32(u64 handle, u32 offset)
{
    if (handle != mmio_handle || offset + 4 > mmio_bytes) return 0xffffffffu;
    return *(volatile uint32_t*)(uintptr_t)(handle + offset);
}

static void module_write32(u64 handle, u32 offset, u32 value)
{
    if (handle != mmio_handle || offset + 4 > mmio_bytes) return;
    *(volatile uint32_t*)(uintptr_t)(handle + offset) = value;
}

static u64 module_map(u64 physical, u64 bytes, u32 write_combine, u64 *mapped_bytes)
{
    /* Only the BARs of the function this module was loaded for may be mapped, and only once.  A
     * module cannot walk to another device because it is not given a way to name one. */
    if (physical != info.bar_base[0] && physical != info.bar_base[1] &&
        physical != info.bar_base[2] && physical != info.bar_base[3] &&
        physical != info.bar_base[4] && physical != info.bar_base[5]) {
        module_log("refused a mapping outside this function's BARs");
        return 0;
    }
    if (mmio_handle) { *mapped_bytes = mmio_bytes; return mmio_handle; }
    void *mapped = device_map(physical, bytes, write_combine ? 1 : 0, mapped_bytes);
    if (!mapped) { module_log("device memory mapping failed"); return 0; }
    mmio_handle = (u64)(uintptr_t)mapped;
    mmio_phys = physical;
    mmio_bytes = *mapped_bytes;
    return mmio_handle;
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

static int validate(const struct boot_handoff *handoff, const struct gpu_device *device,
                    const struct scos_gpu_module_header **out_header)
{
    struct scos_gpu_module_header *header;
    if (!handoff->module_bytes || !handoff->module_address) return 1;
    u64 region = GPU_MODULE_AREA(handoff->arena_start);
    u64 limit = GPU_MODULE_REGION;
    if (handoff->module_address != region || handoff->module_bytes > limit) return 7;
    record.region = region;
    record.region_bytes = limit;
    record.file_bytes = handoff->module_bytes;
    header = (struct scos_gpu_module_header*)(uintptr_t)region;
    if (header->magic != SCOS_GPU_MODULE_MAGIC) return 2;
    if (header->abi != SCOS_GPU_ABI_VERSION) return 3;
    if (header->header_size != SCOS_GPU_MODULE_HEADER_SIZE) return 4;
    if (header_checksum_of(header) != header->header_words) return 5;
    u64 payload_length = handoff->module_bytes - SCOS_GPU_MODULE_HEADER_SIZE;
    if (mod_crc32((void*)(uintptr_t)(region + SCOS_GPU_MODULE_HEADER_SIZE),
                  (size_t)payload_length) != header->payload_crc) return 6;
    u64 load_end = (u64)header->rxe_off + header->rxe_size + header->data_size;
    if (header->rxe_off != SCOS_GPU_MODULE_HEADER_SIZE) return 7;
    if (load_end > handoff->module_bytes - header->reloc_count * 16u) return 7;
    if (header->image_size > limit) return 7;
    if (header->data_off != header->rxe_off + header->rxe_size) return 8;
    if (header->load_size != header->rxe_size + header->data_size) return 7;
    if ((u64)header->reloc_off + header->reloc_count * 16u > handoff->module_bytes) return 9;
    if (header->entry_init < SCOS_GPU_MODULE_HEADER_SIZE ||
        header->entry_init - SCOS_GPU_MODULE_HEADER_SIZE >= header->rxe_size) return 10;
    if (header->entry_teardown &&
        (header->entry_teardown < SCOS_GPU_MODULE_HEADER_SIZE ||
         header->entry_teardown - SCOS_GPU_MODULE_HEADER_SIZE >= header->rxe_size)) return 10;
    if (header->id_count == 0) return 17;
    if (header->flags & SCOS_GPU_MODULE_TAKES_DISPLAY) return 13;
    const char *family = (const char*)header->family;
    if (!device->match || strcmp(family, device->match->family) != 0) return 11;
    for (unsigned i = 0; i < header->reloc_count; i++) {
        struct scos_gpu_reloc *r = (struct scos_gpu_reloc*)(uintptr_t)
            (region + header->reloc_off + (u64)i * 16u);
        u32 width = r->type == SCOS_GPU_RELOC_ABS64 ? 8u : 4u;
        if (r->type != SCOS_GPU_RELOC_ABS64 && r->type != SCOS_GPU_RELOC_ABS32S &&
            r->type != SCOS_GPU_RELOC_PC32 && r->type != SCOS_GPU_RELOC_PLT32) return 14;
        if ((u64)r->offset + width > header->load_size) return 14;
        if ((u64)r->symbol + (u64)(r->addend < 0 ? -r->addend : r->addend) > header->image_size)
            return 14;
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
    *out_header = header;
    record.header_crc = header->payload_crc;
    record.computed_crc = mod_crc32((void*)(uintptr_t)(region + SCOS_GPU_MODULE_HEADER_SIZE),
                                    (size_t)payload_length);
    record.reloc_count = header->reloc_count;
    record.id_count = header->id_count;
    record.load_bytes = header->load_size;
    record.image_bytes = header->image_size;
    record.resident_bytes = header->image_size;
    return 0;
}

static void relocate(u64 base, const struct scos_gpu_module_header *header)
{
    for (unsigned i = 0; i < header->reloc_count; i++) {
        struct scos_gpu_reloc *r = (struct scos_gpu_reloc*)(uintptr_t)
            (base + header->reloc_off + (u64)i * 16u);
        u64 place = base + r->offset;
        u64 target = base + r->symbol;
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
            *(int32_t*)(uintptr_t)place =
                (int32_t)((int64_t)target + (int64_t)r->addend - (int64_t)(place + 4));
            break;
        default: break;
        }
    }
}

/* ---------------------------------------------------------- self-test ---- */

/* Drive the engine into device memory and read the result back through the mapping the display
 * itself scans out of.  This is deliberately not a "call it and see if it returns 0" test: the
 * register sequence can be accepted by a card that then draws nothing, and only the pixels say
 * whether the port is right.  Any damage is repaired by the CPU immediately, before this function
 * returns, because the LFB is not the desktop's source of truth - `screen.px` is, and the next flip
 * repaints whatever the test touched. */
static int engine_self_test(const struct boot_handoff *handoff)
{
    const struct scos_gpu_engine_ops *ops = record.ops;
    if (!ops || !ops->fill) return 0;
    u32 width = handoff->framebuffer.width, height = handoff->framebuffer.height;
    if (width < 64 || height < 64) return 0;
    s32 x = (s32)width - 48, y = (s32)height - 32;
    volatile uint32_t *fb = (volatile uint32_t*)(uintptr_t)handoff->framebuffer.base;
    u32 stride = handoff->framebuffer.stride;
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
    record.present = 0;
    record.bound = 0;
    record.ops = 0;
    mmio_handle = mmio_phys = mmio_bytes = 0;
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
    relocate(record.region, header);
    /* .bss is part of the image size but not stored in the file: zero it before the module runs. */
    if (header->bss_size)
        memset((void*)(uintptr_t)(record.region + header->load_size), 0, header->bss_size);
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
        klog(MODULE_LOG_PREFIX "reports an engine table this kernel cannot call");
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
    if (!engine_self_test(handoff)) {
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
    klog(MODULE_LOG_PREFIX "%s bound: engine verified in device memory (%d/%d pixels)",
         record.name, record.self_test_matches, record.self_test_pixels);
    return 0;
}

const struct gpu_module_state *gpu_module_state(void)
{
    static struct gpu_module_state snapshot;
    snapshot.present = record.present;
    snapshot.bound = record.bound;
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
        put(w, " were never read");
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

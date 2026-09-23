/* Host harness for drivers/gpu/nvidia/module.c: the module is compiled exactly as the kernel build
 * compiles it, against a fake `struct scos_gpu_exports` whose register file is an array.  This is not a
 * substitute for booting - the readback proof for a real engine can only come from a real chip - but it
 * is the proof that the identification driver does what it says, and it is the only place where the
 * refusal paths can be exercised at all: on a machine with no NVIDIA silicon, every one of them is the
 * behaviour that matters.
 *
 * What is asserted, in order of how much it costs if it is wrong:
 *   - the decode.  NV_PMC_BOOT_0's fields, as NVIDIA's published manual places them, must come out in
 *     the described text; a wrong shift is a wrong fact in front of a user.
 *   - the refusals.  A block that answers nothing, a value that moves between two reads, a function of
 *     another vendor's, a BAR too small to hold the register: each has to end with init returning -1 and
 *     the log saying why.  A driver that binds to a chip it cannot read is worse than no driver.
 *   - that no store was ever issued.  The trap below records every write32 the module attempts, and any
 *     attempt fails the run: "this driver changes nothing" has to be a measured property, not a comment.
 *   - the operation table.  Every drawing entry point must be null and only `describe` set, which is
 *     what the kernel's own predicates rely on to keep compositing on the CPU.
 *
 * No <string.h>: the module defines `memset` itself (a module may not import libc), and the two
 * definitions would fight over one symbol.  The handful of string operations needed are below.
 */
#include <stdio.h>

#include "gpu_abi.h"

#define REG_WORDS 4096
static u32 registers[REG_WORDS];
static int stores;                       /* every write32 the module attempted: must stay 0 */
static int maps;
static u64 mapped_physical;
static const char *logs[24];
static int log_count;

static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int has(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return 0;
    for (int i = 0; haystack[i]; i++) {
        int ok = 1;
        for (int j = 0; needle[j]; j++) {
            if (!haystack[i + j] || haystack[i + j] != needle[j]) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ the fake kernel ---- */
static void fake_log(const char *line)
{
    /* The kernel copies a log line into its own buffer; the harness keeps a private copy so a
       `static char text[]` inside the module cannot be rewritten by a later scenario. */
    static char pool[24][256];
    if (log_count >= 24) return;
    char *slot = pool[log_count];
    int i = 0;
    for (; i < 255 && line[i]; i++) slot[i] = line[i];
    slot[i] = 0;
    logs[log_count++] = slot;
}

static u64 fake_map(u64 physical, u64 bytes, u32 write_combine, u64 *mapped_bytes)
{
    (void)bytes;
    maps++;
    mapped_physical = physical;
    if (write_combine) {
        printf("  note: module asked for a write-combined mapping of a register BAR\n");
    }
    if (mapped_bytes) *mapped_bytes = 0x1000000ull;
    return 0x1000ull;                    /* any non-zero handle: reads below ignore it */
}

/* One device that answers differently the second time it is asked: the only way to test a driver's
 * "I do not believe this" path without a chip that is actually misbehaving. */
static int unstable, unstable_reads;
static u32 unstable_value;

static u32 fake_read32(u64 handle, u32 offset)
{
    (void)handle;
    if (unstable && offset == 0) {
        unstable_reads++;
        if (unstable_reads > 1) return unstable_value;
    }
    if (offset / 4u >= REG_WORDS) return 0xffffffffu;
    return registers[offset / 4u];
}

static void fake_write32(u64 handle, u32 offset, u32 value)
{
    (void)handle;
    stores++;
    printf("  FORBIDDEN: module stored 0x%x at +0x%x\n", value, offset);
    if (offset / 4u < REG_WORDS) registers[offset / 4u] = value;
}

static u64 fake_ticks(void) { return 0; }
static void fake_spin(u32 iterations) { (void)iterations; }
static u32 fake_wait_bit(u64 handle, u32 offset, u32 mask, u32 expect, u64 timeout)
{
    (void)handle; (void)offset; (void)mask; (void)expect; (void)timeout;
    return 0;
}
static u64 fake_alloc(u64 bytes) { (void)bytes; return 0; }

struct fixture {
    struct scos_gpu_exports exports;
    struct scos_gpu_device_info device;
};

static void fixture_reset(struct fixture *f, u32 vendor, u32 device_id, u64 framebuffer_base)
{
    struct scos_gpu_device_info *d = &f->device;
    *d = (struct scos_gpu_device_info){
        .size = sizeof(*d),
        .vendor = (u16)vendor, .device = (u16)device_id,
        .subvendor = 0x10de, .subdevice = 0x1000,
        .bus = 0, .slot = 1, .function = 0, .revision = 0xa1,
        .bar_base = {0xe0100000ull, framebuffer_base, 0, 0, 0, 0},
        .bar_bytes = {0x1000000ull, 0x10000000ull, 0, 0, 0, 0},
        .framebuffer_base = framebuffer_base,
        .framebuffer_bytes = framebuffer_base ? 0x10000000ull : 0,
        .width = 1920, .height = 1080, .pitch = 1920 * 4,
        .bytes_per_pixel = 4,
        .framebuffer_pointer = framebuffer_base,
        .vram_bytes = 0x10000000,
    };
    f->exports = (struct scos_gpu_exports){
        .size = sizeof(f->exports),
        .abi = SCOS_GPU_ABI_VERSION,
        .device = d,
        .log = fake_log,
        .map = fake_map,
        .read32 = fake_read32,
        .write32 = fake_write32,
        .ticks = fake_ticks,
        .spin = fake_spin,
        .wait_bit = fake_wait_bit,
        .alloc = fake_alloc,
    };
    stores = 0;
    maps = 0;
    unstable = 0;
    unstable_reads = 0;
    unstable_value = 0xffffffffu;
    mapped_physical = 0;
    log_count = 0;
    for (unsigned i = 0; i < REG_WORDS; i++) registers[i] = 0;
}

extern int scos_module_init(const struct scos_gpu_exports *exports,
                            struct scos_gpu_engine_ops **out_ops);
extern void scos_module_teardown(struct scos_gpu_engine_ops *ops);

/* ------------------------------------------------------------------------ checks ---- */
static int failures;
static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static const char *first_log(void) { return log_count ? logs[0] : ""; }

int main(void)
{
    struct fixture f;
    const u32 boot0 = 0x1a8000a1u;      /* arch 0x1a, implementation 8, major A, minor 1 */

    /* ---- 1. a Blackwell function that answers: bound, described, and nothing written. ---- */
    fixture_reset(&f, 0x10de, 0x2d83, 0x10000000ull);
    registers[0] = boot0;
    registers[1] = 0;
    struct scos_gpu_engine_ops *ops = (struct scos_gpu_engine_ops *)(long)-1;
    int verdict = scos_module_init(&f.exports, &ops);
    check(verdict == 0, "init accepts a Blackwell function whose boot register answers");
    check(ops && ops != (struct scos_gpu_engine_ops *)(long)-1, "init hands back an ops table");
    check(ops && ops->size == sizeof(*ops), "the ops table says how big it is");
    check(ops && ops->abi == SCOS_GPU_ABI_VERSION, "the ops table carries this kernel's ABI number");
    check(ops && !ops->fill && !ops->copy && !ops->wait_idle,
          "no rectangle, copy or wait operation is offered");
    check(ops && !ops->vram_window && !ops->set_surfaces,
          "no surface of its own is offered either, so the compositor is untouched");
    check(ops && !ops->fill_span && !ops->invert, "the optional tier is absent, not half-filled");
    check(ops && ops->describe, "the one operation it does offer is the description");
    const char *text = ops && ops->describe ? ops->describe(ops->context) : 0;
    check(text && str_len(text) > 40, "the description is non-empty");
    /* gpu_module.c copies a description into a 160-byte field, so anything longer arrives clipped. */
    check(text && str_len(text) < 160, "the description fits the kernel's describe field");
    char needle[64];
    snprintf(needle, sizeof needle, "NV_PMC_BOOT_0=0x%08x", boot0);
    check(has(text, needle), "the description quotes the raw dword, so the decode can be re-checked");
    check(has(text, "arch 0x1a (arch not in the published table)"),
          "an architecture the published table does not name is reported as unnamed, not refused");
    check(has(text, "impl 0x8"), "IMPLEMENTATION is taken from 23:20, where NVIDIA documents it");
    check(has(text, "rev A.1"), "MAJOR_REVISION 7:4 and MINOR_REVISION 3:0 are read as separate fields");
    check(has(text, "reads 2, writes 0"), "the read count is reported, and the write count is zero");
    check(has(text, "feeds the console"),
          "the function whose BAR holds the firmware's surface says it feeds the console");
    check(has(text, "at BAR 0xe0100000"), "the aperture it read is the one it reports");
    check(stores == 0, "not one store reached the register file");
    check(maps == 1 && mapped_physical == 0xe0100000ull,
          "exactly one mapping, and it was the register BAR, not the frame buffer");
    check(has(first_log(), "identified from the chip's own boot register"),
          "the log line says the chip was read, not named");
    check(has(first_log(), "10de:2d83"), "and names the function it read");
    if (ops && ops->teardown) ops->teardown(ops->context);
    scos_module_teardown(ops);

    /* ---- 2. the same chip with the console behind another function. ---- */
    fixture_reset(&f, 0x10de, 0x2d83, 0);
    registers[0] = boot0;
    verdict = scos_module_init(&f.exports, &ops);
    check(verdict == 0 && has(ops->describe(ops->context), "not the console owner"),
          "with no firmware surface in its BARs it says another function feeds the console");
    check(stores == 0, "the second run still wrote nothing");

    /* ---- 3. a block that answers nothing is refused, not reported. ---- */
    fixture_reset(&f, 0x10de, 0x2d83, 0x10000000ull);
    for (unsigned i = 0; i < 4; i++) registers[i] = 0xffffffffu;
    verdict = scos_module_init(&f.exports, &ops);
    check(verdict == -1, "an all-ones register block refuses the module");
    check(has(first_log(), "answers nothing readable"), "and says the register block did not answer");

    /* ---- 4. an all-zero block is the same failure. ---- */
    fixture_reset(&f, 0x10de, 0x2d83, 0x10000000ull);
    registers[0] = 0;
    verdict = scos_module_init(&f.exports, &ops);
    check(verdict == -1, "an all-zero register block refuses the module");

    /* ---- 5. a value that moves between the two reads is not believed. ---- */
    fixture_reset(&f, 0x10de, 0x2d83, 0x10000000ull);
    registers[0] = boot0;
    unstable = 1;
    unstable_value = 0x1a8000a2u;                /* one bit different: a chip that is not settled */
    verdict = scos_module_init(&f.exports, &ops);
    check(verdict == -1, "a boot register that changes between two reads is refused");
    check(has(first_log(), "changed between two reads"), "and the log says that is why");
    check(unstable_reads == 2, "the driver asked twice, not once and not three times");
    check(stores == 0, "an unstable device is refused without a write to settle it");

    /* ---- 6. another vendor's function is refused before anything is read. ---- */
    fixture_reset(&f, 0x1013, 0x00b8, 0xe0000000ull);
    registers[0] = boot0;
    verdict = scos_module_init(&f.exports, &ops);
    check(verdict == -1, "a Cirrus function is refused by an NVIDIA driver");
    check(has(first_log(), "not NVIDIA"), "and the refusal names the vendor mismatch");
    check(maps == 0 && stores == 0, "the refusal happened before a mapping or a store");

    /* ---- 7. no BAR big enough: the module reports that rather than reading a small one. ---- */
    fixture_reset(&f, 0x10de, 0x2d83, 0x10000000ull);
    f.device.bar_bytes[0] = 0x100;
    f.device.bar_base[2] = 0;
    verdict = scos_module_init(&f.exports, &ops);
    check(verdict == -1, "a 256 B BAR is not accepted as the control aperture");
    check(has(first_log(), "no BAR large enough"), "and the report says the BAR was the problem");

    /* The id list the module claims is checked where it can be compared against the table the loader
     * cross-checks it with: see tools/tests/test_nvidia_ident.py, which reads both files. */

    /* ---- 8. binding again after a teardown still describes the chip, not the last run. ---- */
    fixture_reset(&f, 0x10de, 0x2d83, 0);
    registers[0] = boot0;
    verdict = scos_module_init(&f.exports, &ops);
    text = ops->describe(ops->context);
    check(verdict == 0 && has(text, "not the console owner") && !has(text, "feeds the console"),
          "a rebind recomputes the inventory instead of repeating the previous one");
    check(has(text, "reads 2, writes 0"), "and the read counter restarted, as a measurement must");

    printf("%s: nvidia identification module, %d scenario(s), %d failure(s)\n",
           failures ? "FAIL" : "PASS", 8, failures);
    return failures ? 1 : 0;
}

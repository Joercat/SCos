/*
 * SCos native - ACPI power-off (RSDP -> RSDT/XSDT -> FADT -> PM1a_CNT,
 * SLP_TYP from the \_S5_ package in the DSDT). Falls back gracefully when
 * ACPI is unavailable (the classic "it is now safe..." screen is shown by
 * the caller).
 */
#include "scos.h"

struct rsdp {
    char sig[8];
    u8  checksum;
    char oem[6];
    u8  revision;
    u32 rsdt;
    u32 length;
    u64 xsdt;
    u8  ext_checksum;
    u8  reserved[3];
} __attribute__((packed));

static u32 pm1_cnt;
static u16 slp_typa;
static int acpi_ok;

static int mem_eq(const void *a, const char *b, int n)
{
    const u8 *x = a;
    for (int i = 0; i < n; i++) if (x[i] != (u8)b[i]) return 0;
    return 1;
}

static u8 sum_bytes(const u8 *p, u32 n)
{
    u8 s = 0;
    for (u32 i = 0; i < n; i++) s += p[i];
    return s;
}

static struct rsdp *find_rsdp(void)
{
    for (u32 addr = 0x000E0000; addr < 0x00100000; addr += 16) {
        struct rsdp *r = (struct rsdp *)addr;
        if (mem_eq(r->sig, "RSD PTR ", 8) && sum_bytes((const u8 *)r, 20) == 0)
            return r;
    }
    /* EBDA first KB */
    u16 ebda = (*(u16 *)0x40E) << 4;
    for (u32 addr = ebda; addr < (u32)ebda + 1024; addr += 16) {
        struct rsdp *r = (struct rsdp *)addr;
        if (mem_eq(r->sig, "RSD PTR ", 8) && sum_bytes((const u8 *)r, 20) == 0)
            return r;
    }
    return NULL;
}

static u32 find_table(u32 rsdt_phys, int is_xsdt, const char *sig)
{
    u32 *hdr = (u32 *)(u32)rsdt_phys;
    u32 length = *(u32 *)(rsdt_phys + 4);
    u32 entry_size = is_xsdt ? 8 : 4;
    u32 n = (length - 36) / entry_size;
    for (u32 i = 0; i < n; i++) {
        u32 phys = is_xsdt ? (u32)(*(u64 *)(rsdt_phys + 36 + i * 8))
                           : *(u32 *)(rsdt_phys + 36 + i * 4);
        if (mem_eq((void *)phys, sig, 4)) return phys;
    }
    (void)hdr;
    return 0;
}

static int parse_s5(const u8 *dsdt, u32 len)
{
    for (u32 i = 0; i + 6 < len; i++) {
        if (mem_eq(dsdt + i, "_S5_", 4) && dsdt[i + 4] == 0x08) {
            u32 j = i + 5;
            /* skip name prefix bytes */
            if (dsdt[j] == 0x5B) j++;
            while (j < len && (dsdt[j] == 0x08 || dsdt[j] == 0x5B)) j++;
            if (j >= len) return 0;
            if (dsdt[j] == 0x12) {                 /* PackageOp */
                u32 k = j + 2;
                if (dsdt[k] == 0x0A) slp_typa = dsdt[k + 1];
                else if (dsdt[k] == 0x0B) slp_typa = dsdt[k + 1] | (dsdt[k + 2] << 8);
                else slp_typa = dsdt[k];
                return 1;
            }
        }
    }
    return 0;
}

void acpi_init(void)
{
    acpi_ok = 0;
    struct rsdp *r = find_rsdp();
    if (!r) { klog("acpi: no RSDP"); return; }
    u32 rsdt = r->rsdt;
    int xsdt = 0;
    if (r->revision >= 2 && r->xsdt) { rsdt = (u32)r->xsdt; xsdt = 1; }

    u32 fadt = find_table(rsdt, xsdt, "FACP");
    if (!fadt) { klog("acpi: no FADT"); return; }
    pm1_cnt = *(u32 *)(fadt + 64);
    u32 dsdt = *(u32 *)(fadt + 40);
    if (!pm1_cnt || !dsdt) { klog("acpi: incomplete FADT"); return; }
    u32 dsdt_len = *(u32 *)(dsdt + 4);
    if (!parse_s5((u8 *)dsdt, dsdt_len)) {
        klog("acpi: no _S5_");
        slp_typa = 0;   /* try anyway with 0 on some firmware */
    }
    acpi_ok = 1;
    klog("acpi: pm1_cnt=%x slp_typ=%u", pm1_cnt, slp_typa);
}

int acpi_shutdown(void)
{
    if (!acpi_ok) return 0;
    outw(pm1_cnt, (u16)((slp_typa << 10) | (1 << 13)));
    return 1;
}

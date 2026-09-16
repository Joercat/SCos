/*
 * SCos native - ATA PIO driver (primary/secondary, master/slave) plus the
 * optional SCos filesystem disk image (persistence, like the web version's
 * localStorage).
 *
 * SAFETY: the kernel never writes to the disk on its own. An image is only
 * loaded at boot when the SCos magic is found at the image LBA, and only
 * written back by an explicit `save` terminal command, so a machine that has
 * never run `save` is never modified.
 */
#include "scos.h"

#define SECTOR 512
#define IMG_LBA 2048            /* 1 MiB into the disk */
#define IMG_MAX_SECTORS 256     /* 128 KB image budget */

struct ata_dev {
    char model[41];
    u16 io, ctrl;
    u8  slave;
    int present;
};

static struct ata_dev devs[4];
static int dev_count;
int fs_image_found;

static void ata_wait_ready(struct ata_dev *d)
{
    for (int i = 0; i < 100000; i++)
        if ((inb(d->io + 7) & 0x80) == 0) return;
}

static int ata_ident(struct ata_dev *d)
{
    ata_wait_ready(d);
    /* select the drive, then wait ~400 ns before sampling (ATA spec) */
    outb(d->io + 6, d->slave ? 0xB0 : 0xA0);
    for (int i = 0; i < 15; i++) inb(d->io + 7);
    /* signature check on LBA mid/high:
     *   0x00/0x00 = ATA, 0x3C/0xC3 = ATAPI, 0xFF/0xFF = floating bus.
     * On AHCI boxes with no legacy emulation every port read returns
     * 0xFF - the old code happily "identified" four ghost drives there
     * and then spammed PIO errors for every later access. */
    u8 lm = inb(d->io + 4), lh = inb(d->io + 5);
    /* 0xFF/0xFF = floating bus (AHCI boxes with no legacy emulation read
     * all-ones from every port - the old code "identified" four ghost
     * drives there and spammed PIO errors). 0x3C/0xC3 = ATAPI: no driver. */
    if (lm == 0xFF || lh == 0xFF) return 0;
    if (lm == 0x3C && lh == 0xC3) return 0;
    outb(d->io + 2, 0);
    outb(d->io + 3, 0);
    outb(d->io + 4, 0);
    outb(d->io + 5, 0);
    outb(d->io + 7, 0xEC);
    int done = 0;
    for (int i = 0; i < 100000; i++) {
        u8 st = inb(d->io + 7);
        if (st == 0xFF) return 0;                /* bus went away */
        if (st == 0) return 0;
        if (st & 0x80) continue;                 /* BSY */
        if (st & 1) return 0;                    /* error */
        done = 1;
        break;
    }
    if (!done) return 0;                         /* BSY stuck: not a drive */
    u16 buf[256];
    for (int i = 0; i < 256; i++) buf[i] = inw(d->io);
    if (buf[0] == 0xFFFF || buf[0] == 0x0000) return 0;   /* garbage */
    for (int i = 0; i < 20; i++) {
        u16 w = buf[27 + i];
        d->model[i * 2] = (char)(w >> 8);
        d->model[i * 2 + 1] = (char)(w & 0xFF);
    }
    d->model[40] = 0;
    int printable = 0;
    for (int i = 0; i < 40; i++)
        if (d->model[i] >= 32 && d->model[i] <= 126) printable++;
    if (printable < 4) return 0;                 /* not a real model string */
    for (int i = 39; i >= 0 && d->model[i] == ' '; i--) d->model[i] = 0;
    return 1;
}

int ata_init(void)
{
    static const u16 ios[2]   = { 0x1F0, 0x170 };
    static const u16 ctrls[2] = { 0x3F6, 0x376 };
    dev_count = 0;
    for (int bus = 0; bus < 2; bus++)
        for (int sl = 0; sl < 2; sl++) {
            struct ata_dev *d = &devs[dev_count];
            d->io = ios[bus]; d->ctrl = ctrls[bus]; d->slave = sl;
            if (ata_ident(d)) {
                d->present = 1;
                dev_count++;
            }
        }
    klog("ata: %d drive(s)", dev_count);
    return dev_count;
}

int ata_present(void) { return dev_count > 0; }

static void ata_err(u32 lba, u32 count, u8 st, struct ata_dev *d)
{
    static char l0[64], l1[64], a[8], b[8];
    strcpy(l0, "status 0x");
    a[0] = (char)((st >> 4) & 0xF); a[0] = (char)(a[0] < 10 ? a[0] + '0' : a[0] - 10 + 'a');
    a[1] = (char)(st & 0xF); a[1] = (char)(a[1] < 10 ? a[1] + '0' : a[1] - 10 + 'a');
    a[2] = 0;
    strcat(l0, a);
    strcat(l0, "  lba "); fmt_u32(b, lba); strcat(l0, b);
    strcat(l0, "  count "); fmt_u32(b, count); strcat(l0, b);
    strcpy(l1, "drive "); strcat(l1, d->slave ? "slave" : "master");
    strcat(l1, "  io 0x"); fmt_u32(b, d->io); strcat(l1, b);
    static const char *dump[2];
    dump[0] = l0; dump[1] = l1;
    err_notify("ata", "PIO transfer failed (drive error or timeout)", dump, 2);
}

static int ata_pio_transfer(struct ata_dev *d, u32 lba, u32 count, void *buf, int write)
{
    ata_wait_ready(d);
    outb(d->ctrl, 0);
    /* 0xE0: bit7 set, bit6 = LBA mode, bit5 set. (0xA0 would be CHS mode -
     * the drive would reinterpret our LBA registers as cylinder/head/sector
     * and read completely wrong sectors.) */
    outb(d->io + 6, (d->slave ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));
    outb(d->io + 2, count & 0xFF);
    outb(d->io + 3, lba & 0xFF);
    outb(d->io + 4, (lba >> 8) & 0xFF);
    outb(d->io + 5, (lba >> 16) & 0xFF);
    outb(d->io + 7, write ? 0x30 : 0x20);

    u16 *w = buf;
    for (u32 s = 0; s < count; s++) {
        int ok = 0;
        for (int attempt = 0; attempt < 2 && !ok; attempt++)
            for (int i = 0; i < 2000000; i++) {
                u8 st = inb(d->io + 7);
                if (st & 1) { ata_err(lba, count, st, d); return 0; }
                if ((st & 0x88) == 0x08) { ok = 1; break; }   /* DRQ ready */
            }
        if (!ok) {   /* slow device, not an error: log only, never hold a screen */
            klog("ata: DRQ timeout at lba %u count %u", lba, count);
            return 0;
        }
        if (write) {
            for (int i = 0; i < 256; i++) outw(d->io, w[i]);
        } else {
            for (int i = 0; i < 256; i++) w[i] = inw(d->io);
        }
        w += 256;
    }
    return 1;
}

int ata_read_sectors(u32 lba, u32 count, void *buf)
{
    if (!dev_count) return 0;
    return ata_pio_transfer(&devs[0], lba, count, buf, 0);
}

int ata_write_sectors(u32 lba, u32 count, const void *buf)
{
    if (!dev_count) return 0;
    return ata_pio_transfer(&devs[0], lba, count, (void *)buf, 1);
}

/* ------------------------------------------------------------ FS image --- */
/*
 * Layout (all little endian):
 *   0x00  magic "SCOSFS1\0"
 *   0x08  u32 entry_count
 *   0x0C  entries: u16 path_len, path bytes, u8 is_dir, u32 size, data bytes
 */
static u8 *img_buf;

static void walk_serialize(u8 *p, u32 *off, struct vfs_node *n, const char *prefix, u32 *count)
{
    for (struct vfs_node *c = n->child; c; c = c->sibling) {
        char path[256];
        strcpy(path, prefix);
        strcat(path, c->name);
        u16 plen = (u16)strlen(path);
        if (*off + 8 + plen + c->size > IMG_MAX_SECTORS * SECTOR) return;
        *(u16 *)(p + *off) = plen; *off += 2;
        memcpy(p + *off, path, plen); *off += plen;
        p[(*off)++] = (u8)c->is_dir;
        *(u32 *)(p + *off) = c->size; *off += 4;
        if (c->size) { memcpy(p + *off, c->data, c->size); *off += c->size; }
        (*count)++;
        if (c->is_dir) {
            char p2[256];
            strcpy(p2, path);
            strcat(p2, "/");
            walk_serialize(p, off, c, p2, count);
        }
    }
}

int fs_image_save(void)
{
    if (!dev_count) return 0;
    if (!img_buf) img_buf = palloc(IMG_MAX_SECTORS * SECTOR);
    memset(img_buf, 0, IMG_MAX_SECTORS * SECTOR);
    memcpy(img_buf, "SCOSFS1", 8);
    u32 off = 12, count = 0;
    walk_serialize(img_buf, &off, vfs_root, "", &count);
    *(u32 *)(img_buf + 8) = count;
    u32 sectors = (off + SECTOR - 1) / SECTOR;
    if (sectors > IMG_MAX_SECTORS) return 0;
    return ata_write_sectors(IMG_LBA, sectors, img_buf);
}

static void image_apply(const u8 *p, u32 len)
{
    u32 count = *(const u32 *)(p + 8);
    u32 off = 12;
    for (u32 i = 0; i < count && off + 7 < len; i++) {
        u16 plen = *(const u16 *)(p + off); off += 2;
        if (off + plen + 5 > len) return;
        char path[256];
        if (plen >= sizeof(path)) return;
        memcpy(path, p + off, plen); path[plen] = 0; off += plen;
        u8 is_dir = p[off++];
        u32 size = *(const u32 *)(p + off); off += 4;
        if (off + size > len) return;
        if (is_dir) vfs_mkdir(path);
        else vfs_write(path, (const char *)(p + off), size);
        off += size;
    }
}

int fs_image_load(void)
{
    fs_image_found = 0;
    if (!dev_count) return 0;
    if (!img_buf) img_buf = palloc(IMG_MAX_SECTORS * SECTOR);
    if (!ata_read_sectors(IMG_LBA, IMG_MAX_SECTORS, img_buf)) return 0;
    {
        int pristine = 1;
        for (int i = 0; i < 16; i++)
            if (img_buf[i] != 0x00 && img_buf[i] != 0xFF) pristine = 0;
        if (pristine) return 0;          /* never written: silently absent */
    }
    if (memcmp(img_buf, "SCOSFS1", 8)) {
        static char l0[64];
        strcpy(l0, "first bytes: ");
        static const char *dg = "0123456789abcdef";
        for (int i = 0; i < 8; i++) {
            l0[13 + i * 2] = dg[img_buf[i] >> 4];
            l0[14 + i * 2] = dg[img_buf[i] & 0xF];
        }
        l0[29] = 0;
        static const char *dump[1];
        dump[0] = l0;
        err_notify("filesystem",
                   "saved image has a bad magic - starting with defaults",
                   dump, 1);
        return 0;
    }
    /* start from a clean tree, then overlay the image */
    image_apply(img_buf, IMG_MAX_SECTORS * SECTOR);
    fs_image_found = 1;
    klog("ata: SCos fs image loaded from disk");
    return 1;
}

const char *ata_model(void)
{
    return dev_count ? devs[0].model : NULL;
}

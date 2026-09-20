/*
 * SCos native - ATA PIO driver (primary/secondary, master/slave) plus the
 * optional SCos filesystem disk image (persistence, like the web version's
 * localStorage).
 *
 * Writes require a unique, explicitly SCos-owned disk layout. Persistence
 * is not inferred from ATA enumeration order or a BIOS USB drive number.
 */
#include "scos.h"

#define SECTOR 512
#define IMG_LBA 129024          /* Dedicated GPT SCos-data partition, NOT ESP */
#define DISK_SECTORS 131072
#define IMG_MAX_SECTORS 256     /* 128 KB image budget */

struct ata_dev {
    char model[41];
    u16 io, ctrl;
    u8  slave;
    int present;
    u32 sectors;
};

static struct ata_dev devs[4];
static int dev_count;
static int storage_dev = -1;
static u8 storage_mbr[512];
static int ata_pio_transfer(struct ata_dev *, u32, u32, void *, int);
static u32 image_crc(const u8 *,u32);
static u8 gpt_entries[16384];
static int gpt_header(struct ata_dev *d,u32 lba,u32 other,u32 table,u32 *crc)
{
    u8 h[512];
    if(!ata_pio_transfer(d,lba,1,h,0)||memcmp(h,"EFI PART",8)||
       *(u32*)(h+8)!=0x10000||*(u32*)(h+12)!=92||*(u32*)(h+20)||
       *(u64*)(h+24)!=lba||*(u64*)(h+32)!=other||*(u64*)(h+40)!=34||
       *(u64*)(h+48)!=DISK_SECTORS-34||*(u64*)(h+72)!=table||
       *(u32*)(h+80)!=128||*(u32*)(h+84)!=128)return 0;
    u32 saved=*(u32*)(h+16);*(u32*)(h+16)=0;
    if(image_crc(h,92)!=saved)return 0;
    *crc=*(u32*)(h+88);return 1;
}
static int owned_layout(struct ata_dev *d,const u8 *p)
{
    /* Fail closed: only the exact authored extent, two partitions, both GPT
     * copies and CRCs, never an arbitrary disk with a familiar sector 0. */
    if(d->sectors<DISK_SECTORS||memcmp(p+400,"SCOSDATA64v1",12)||
       p[510]!=0x55||p[511]!=0xaa||*(u32*)(p+412)!=IMG_LBA||
       *(u32*)(p+416)!=IMG_MAX_SECTORS||*(u32*)(p+420)!=DISK_SECTORS||
       p[446]||p[450]!=0xee||*(u32*)(p+454)!=1||*(u32*)(p+458)!=DISK_SECTORS-1)return 0;
    for(int i=462;i<510;i++)if(p[i])return 0;
    u32 primary,backup;
    if(!gpt_header(d,1,DISK_SECTORS-1,2,&primary)||
       !gpt_header(d,DISK_SECTORS-1,1,DISK_SECTORS-33,&backup)||primary!=backup||
       !ata_pio_transfer(d,2,32,gpt_entries,0)||image_crc(gpt_entries,sizeof(gpt_entries))!=primary)return 0;
    static const u8 esp_type[16]={0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b};
    static const u8 data_type[16]={0x87,0xb7,0xfc,0x1f,0x04,0xa9,0xce,0x4f,0x8f,0xbd,0xc7,0x28,0xac,0x57,0xe3,0x33};
    if(memcmp(gpt_entries,esp_type,16)||memcmp(gpt_entries+128,data_type,16)||
       *(u64*)(gpt_entries+32)!=2048||*(u64*)(gpt_entries+40)!=IMG_LBA-1||
       *(u64*)(gpt_entries+160)!=IMG_LBA||*(u64*)(gpt_entries+168)!=IMG_LBA+IMG_MAX_SECTORS-1||
       *(u64*)(gpt_entries+48)||*(u64*)(gpt_entries+176))return 0;
    for(size_t i=256;i<sizeof(gpt_entries);i++)if(gpt_entries[i])return 0;
    u8 check[512];
    for(u32 i=0;i<32;i++)if(!ata_pio_transfer(d,DISK_SECTORS-33+i,1,check,0)||memcmp(check,gpt_entries+i*512,512))return 0;
    return 1;
}
int fs_image_available(void) { return storage_dev >= 0; }

const char *fs_image_target(void) { return storage_dev >= 0 ? devs[storage_dev].model : "none (unverified or ambiguous)"; }
int fs_image_found;

static int ata_wait_ready(struct ata_dev *d)
{
    for (int i = 0; i < 100000; i++)
        if ((inb(d->io + 7) & 0x80) == 0) return 1;
    return 0;
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
        if (st & 0x21) return 0;
        if (!(st & 8)) continue;                    /* error */
        done = 1;
        break;
    }
    if (!done) return 0;                         /* BSY stuck: not a drive */
    u16 buf[256];
    for (int i = 0; i < 256; i++) buf[i] = inw(d->io);
    if (buf[0] == 0xFFFF || buf[0] == 0x0000) return 0;   /* garbage */
    if (!(buf[49] & (1u << 9))) return 0;
    d->sectors = (u32)buf[60] | ((u32)buf[61] << 16);
    if (!d->sectors) return 0;
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
    storage_dev = -1;
    u8 pb[8],pd[8],pf[8];int controllers=pci_find_class(1,1,255,pb,pd,pf,8);int channels=0;
    for(int i=0;i<controllers;i++)if(pci_read16(pb[i],pd[i],pf[i],4)&1){u8 pi=pci_read8(pb[i],pd[i],pf[i],9);if(!(pi&1))channels|=1;if(!(pi&4))channels|=2;}
    for (int bus = 0; bus < 2; bus++)if(channels&(1<<bus))
        for (int sl = 0; sl < 2; sl++) {
            struct ata_dev *d = &devs[dev_count];
            d->io = ios[bus]; d->ctrl = ctrls[bus]; d->slave = sl;
            if (ata_ident(d)) {
                d->present = 1;
                dev_count++;
            }
        }
    int matches = 0, readable = 1;
    u8 sector[512];
    for (int i=0;i<dev_count;i++) {
        if (devs[i].sectors < DISK_SECTORS) continue;
        if (!ata_pio_transfer(&devs[i],0,1,sector,0)) { readable=0; continue; }
        if (owned_layout(&devs[i],sector)) {
            matches++; storage_dev=i; memcpy(storage_mbr,sector,512);
        }
    }
    if (matches != 1 || !readable) storage_dev=-1;
    klog("ata: %d drive(s); verified SCos persistence target: %s", dev_count, fs_image_target());
    return dev_count;
}

int ata_present(void) { return dev_count > 0; }

static void ata_err(u32 lba, u32 count, u8 st, struct ata_dev *d)
{
    static char l0[64], l1[64], a[8], b[12];
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
    if (!buf || !count || count > 256 || lba >= 0x10000000u ||
        count > 0x10000000u-lba || lba >= d->sectors || count > d->sectors-lba) return 0;
    if (!ata_wait_ready(d)) return 0;
    outb(d->ctrl, 2); /* polled PIO: do not assert unhandled IRQ14/15 */
    /* 0xE0: bit7 set, bit6 = LBA mode, bit5 set. (0xA0 would be CHS mode -
     * the drive would reinterpret our LBA registers as cylinder/head/sector
     * and read completely wrong sectors.) */
    outb(d->io + 6, (d->slave ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));
    for (int i=0;i<4;i++) inb(d->ctrl);
    if (!ata_wait_ready(d)) return 0;
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
                if (st & 0x80) continue;
                if (st == 0 || st == 0xff || (st & 0x21)) { ata_err(lba, count, st, d); return 0; }
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
        for (int i=0;i<4;i++) inb(d->ctrl);
    }
    /* Wait for command completion, then force completed writes out of cache. */
    for (int pass=0;pass<(write ? 2 : 1);pass++) {
        int done=0;
        for (int i=0;i<2000000;i++) {
            u8 st=inb(d->io+7);
            if (st & 0x80) continue;
            if (!st || st==0xff || (st & 0x21)) { ata_err(lba,count,st,d); return 0; }
            if (!(st & 8)) { done=1; break; }
        }
        if (!done) return 0;
        if (write && pass==0) outb(d->io+7,0xe7);
    }
    return 1;
}

int ata_read_sectors(u32 lba, u32 count, void *buf)
{
    if (storage_dev < 0 || lba >= DISK_SECTORS || count > DISK_SECTORS-lba) return 0;
    return ata_pio_transfer(&devs[storage_dev], lba, count, buf, 0);
}

int ata_write_sectors(u32 lba, u32 count, const void *buf)
{
    if (storage_dev < 0 || lba < IMG_LBA || lba >= IMG_LBA+IMG_MAX_SECTORS ||
        count > IMG_LBA+IMG_MAX_SECTORS-lba) return 0;
    return ata_pio_transfer(&devs[storage_dev], lba, count, (void *)buf, 1);
}

/* FS2: magic[8], entry_count:u32, payload CRC32:u32, then bounded entries.
 * Generated /system boot-chain copies are reconstructed, never serialized.
 * A single slot detects interrupted writes but is not a transactional journal.
 */
#define IMAGE_BYTES (IMG_MAX_SECTORS * SECTOR)
static u8 *img_buf;
static int image_error(const char *why)
{
    klog("filesystem: %s", why);
    return 0;
}
static u32 image_crc(const u8 *p, u32 n)
{
    u32 c=~0u;
    while (n--) { c ^= *p++; for (int i=0;i<8;i++) c=(c>>1)^((0u-(c&1))&0xedb88320u); }
    return ~c;
}
static int generated(const char *p)
{
    return !strcmp(p,"system/kernel.bin") || !strcmp(p,"system/README.txt") ||
           !strcmp(p,"system/boot") || !strncmp(p,"system/boot/",12);
}
static int walk_serialize(u8 *p, u32 *off, struct vfs_node *n, const char *prefix, u32 *count)
{
    for (struct vfs_node *c=n->child;c;c=c->sibling) {
        char path[256];
        u32 a=strlen(prefix), b=strlen(c->name);
        if (!b || a+b >= sizeof(path)-1) return 0;
        memcpy(path,prefix,a); memcpy(path+a,c->name,b+1);
        if (generated(path)) continue;
        u32 plen=a+b, size=c->is_dir ? 0 : c->size;
        if (*off > IMAGE_BYTES || 7+plen > IMAGE_BYTES-*off ||
            size > IMAGE_BYTES-*off-7-plen || (size && !c->data)) return 0;
        *(u16 *)(p+*off)=(u16)plen; *off+=2;
        memcpy(p+*off,path,plen); *off+=plen;
        p[(*off)++]=(u8)c->is_dir;
        *(u32 *)(p+*off)=size; *off+=4;
        if (size) { memcpy(p+*off,c->data,size); *off+=size; }
        (*count)++;
        if (c->is_dir) {
            path[plen]='/'; path[plen+1]=0;
            if (!walk_serialize(p,off,c,path,count)) return 0;
        }
    }
    return 1;
}
int fs_image_save(void)
{
    if (storage_dev < 0) return image_error("no unique verified SCos disk; no write attempted");
    u8 verify[512];
    if (!ata_read_sectors(0,1,verify) || memcmp(verify,storage_mbr,512) || !owned_layout(&devs[storage_dev],verify))
        return image_error("disk ownership changed; no write attempted");
    if (!img_buf) img_buf=palloc_owned(IMAGE_BYTES,HEAP_VFS);
    if (!img_buf) return image_error("out of memory; no write attempted");
    memset(img_buf,0,IMAGE_BYTES);
    memcpy(img_buf,"SCOSFS2",8);
    u32 off=16,count=0;
    if (!walk_serialize(img_buf,&off,vfs_root,"",&count))
        return image_error("tree too large or path invalid; no write attempted");
    *(u32 *)(img_buf+8)=count;
    *(u32 *)(img_buf+12)=image_crc(img_buf+8,IMAGE_BYTES-8);
    /* Invalidate first; clear unused tail as well as writing the new tree.
     * Commit the valid first sector only after all remaining sectors flush. */
    memset(verify,0,512);
    memcpy(verify,"SCOSBAD",8);
    if (!ata_write_sectors(IMG_LBA,1,verify) ||
        !ata_write_sectors(IMG_LBA+1,IMG_MAX_SECTORS-1,img_buf+512) ||
        !ata_write_sectors(IMG_LBA,1,img_buf)) return image_error("disk write/flush failed");
    for (u32 i=0;i<IMG_MAX_SECTORS;i++)
        if (!ata_read_sectors(IMG_LBA+i,1,verify) || memcmp(verify,img_buf+i*512,512))
            return image_error("disk readback verification failed");
    return 1;
}
static int valid_path(const char *p)
{
    if (!*p || *p=='/' || generated(p)) return 0;
    while (*p) {
        const char *start=p;
        while (*p && *p!='/') p++;
        u32 n=(u32)(p-start);
        if (!n || n>=VFS_NAME || (n==1 && start[0]=='.') ||
            (n==2 && start[0]=='.' && start[1]=='.')) return 0;
        if (*p && !*++p) return 0;
    }
    return 1;
}
static int image_apply(const u8 *p, u32 len)
{
    struct vfs_node *old=vfs_root;
    struct vfs_node *fresh=palloc_owned(sizeof(*fresh),HEAP_VFS);
    if (!fresh) return 0;
    memset(fresh,0,sizeof(*fresh)); fresh->is_dir=1; vfs_root=fresh;
    u32 count=*(const u32 *)(p+8), off=16;
    for (u32 i=0;i<count;i++) {
        if (off>len || len-off<7) goto bad;
        u32 plen=*(const u16 *)(p+off); off+=2;
        if (!plen || plen>=256 || plen>len-off-5) goto bad;
        char path[256];
        memcpy(path,p+off,plen); path[plen]=0; off+=plen;
        if (strlen(path)!=plen || !valid_path(path) || vfs_lookup(path)) goto bad;
        u8 dir=p[off++]; u32 size=*(const u32 *)(p+off); off+=4;
        if (dir>1 || size>len-off || (dir && size)) goto bad;
        if (dir ? !vfs_mkdir(path) : !vfs_write(path,(const char *)(p+off),size)) goto bad;
        off+=size;
    }
    const char *required[]={"/home","/home/documents","/system","/home/downloads","/home/desktop"};
    for(size_t i=0;i<sizeof(required)/sizeof(required[0]);i++){struct vfs_node *n=vfs_lookup(required[i]);if(!n||!n->is_dir)goto bad;}
    vfs_free_tree(old);
    return 1;
bad:
    vfs_root=old; vfs_free_tree(fresh);
    return 0;
}
int fs_image_load(void)
{
    fs_image_found=0;
    if (storage_dev<0) return 0;
    if (!img_buf) img_buf=palloc_owned(IMAGE_BYTES,HEAP_VFS);
    if (!img_buf) return image_error("out of memory loading saved tree");
    if (!ata_read_sectors(IMG_LBA,IMG_MAX_SECTORS,img_buf)) return image_error("saved tree read failed");
    int blank=1;
    for (u32 i=0;i<IMAGE_BYTES;i++) if (img_buf[i]) { blank=0; break; }
    if (blank) return 0;
    u32 stored=*(u32 *)(img_buf+12);
    *(u32 *)(img_buf+12)=0;
    u32 actual=image_crc(img_buf+8,IMAGE_BYTES-8);
    *(u32 *)(img_buf+12)=stored;
    if (memcmp(img_buf,"SCOSFS2",8) || stored!=actual ||
        !image_apply(img_buf,IMAGE_BYTES)) {
        err_notify("filesystem","saved tree invalid/incomplete; defaults retained",NULL,0);
        return 0;
    }
    fs_image_found=1;
    klog("filesystem: verified saved tree loaded");
    return 1;
}

const char *ata_model(void)
{
    return dev_count ? devs[0].model : NULL;
}

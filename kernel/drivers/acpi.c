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
static u32 pm1b_cnt;
static u32 smi_cmd;
static u8  acpi_enable_val;
static u16 slp_typa, slp_typb;
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

static const struct rsdp *find_rsdp(void)
{
    const struct rsdp *r=firmware_map(platform_rsdp,20);
    if(!r||!mem_eq(r->sig,"RSD PTR ",8)||sum_bytes((const u8*)r,20))return NULL;
    if(r->revision>=2){
        r=firmware_map(platform_rsdp,36);
        if(!r||r->length<36||r->length>4096)return NULL;
        r=firmware_map(platform_rsdp,r->length);
        if(!r||sum_bytes((const u8*)r,r->length))return NULL;
    }
    return r;
}
static int valid_table(u64 addr,const char *sig)
{
    const u8 *p=firmware_map(addr,36);if(!p)return 0;
    u32 len=*(const u32*)(p+4);
    if(!mem_eq(p,sig,4)||len<36||len>1024*1024)return 0;
    p=firmware_map(addr,len);return p&&!sum_bytes(p,len);
}
static u64 find_table(u64 root,int xsdt,const char *sig)
{
    if(!valid_table(root,xsdt?"XSDT":"RSDT"))return 0;
    const u8 *p=(const u8*)(uintptr_t)root;u32 len=*(const u32*)(p+4),step=xsdt?8:4;
    if((len-36)%step)return 0;
    for(u32 off=36;off+step<=len;off+=step){u64 ptr=xsdt?*(const u64*)(p+off):*(const u32*)(p+off);if(valid_table(ptr,sig))return ptr;}
    return 0;
}

static int aml_integer(const u8 *p, u32 end, u32 *at, u16 *out)
{
    if (*at>=end) return 0;
    u8 op=p[(*at)++];
    if (op<=1) { *out=op; return 1; }
    u32 n=op==0x0A ? 1 : op==0x0B ? 2 : op==0x0C ? 4 : op==0x0E ? 8 : 0;
    if (!n || n>end-*at) return 0;
    u64 v=0;
    for (u32 i=0;i<n;i++) v|=(u64)p[(*at)++]<<(8*i);
    if (v>7) return 0; /* SLP_TYP is a three-bit field, not an AML opcode. */
    *out=(u16)v; return 1;
}

/* Limited constant-package support, not an AML interpreter. Dynamic _S5,
 * _PTS/_GTS and hardware-reduced sleep require a future ACPI library port. */
static int parse_s5(const u8 *dsdt, u32 len)
{
    for (u32 i=0;i+6<len;i++) {
        if (dsdt[i]!=0x08) continue; /* NameOp */
        u32 name=i+1;
        if (dsdt[name]==0x5C) name++; /* root prefix */
        if (name+5>=len || !mem_eq(dsdt+name,"_S5_",4) || dsdt[name+4]!=0x12) continue;
        u32 start=name+5, at=start;
        u8 first=dsdt[at++], follow=first>>6;
        if (follow>len-at) continue;
        u32 size=follow ? first&15 : first&63;
        for (u32 j=0;j<follow;j++) size|=(u32)dsdt[at++]<<(4+8*j);
        if (size>len-start || size<=at-start) continue;
        u32 end=start+size;
        if (dsdt[at++]<2) continue;
        u16 a,b;
        if (!aml_integer(dsdt,end,&at,&a) || !aml_integer(dsdt,end,&at,&b)) continue;
        slp_typa=a; slp_typb=b;
        return 1;
    }
    return 0;
}

static u32 gas_io(u64 fadt, u32 len, u32 off)
{
    if (len<off+12) return 0;
    const u8 *p=(const u8 *)(fadt+off);
    u64 addr=*(const u64 *)(p+4);
    return p[0]==1 && p[1]>=16 && !p[2] && p[3]<=2 && addr && addr<=65534 ? (u32)addr : 0;
}

void acpi_init(void)
{
    acpi_ok = 0;
    const struct rsdp *r = find_rsdp();
    if (!r) { klog("acpi: no RSDP"); return; }
    u64 fadt=0;
    if (r->revision>=2 && r->length>=36 && r->length<=4096 &&
        !sum_bytes((const u8 *)r,r->length) && r->xsdt)
        fadt=find_table(r->xsdt,1,"FACP");
    if (!fadt) fadt=find_table(r->rsdt,0,"FACP");
    if (!fadt) { klog("acpi: valid FADT unavailable"); return; }
    u32 len=*(const u32 *)(fadt+4);
    if (len<116 || (*(const u32 *)(fadt+112)&(1u<<20))) {
        klog("acpi: unsupported FADT/hardware-reduced power controls"); return;
    }
    pm1_cnt=gas_io(fadt,len,172); pm1b_cnt=gas_io(fadt,len,184);
    if (!pm1_cnt) pm1_cnt=*(const u32 *)(fadt+64);
    if (!pm1b_cnt) pm1b_cnt=*(const u32 *)(fadt+68);
    smi_cmd=*(const u32 *)(fadt+48);
    acpi_enable_val=*(const u8 *)(fadt+52);
    u64 dsdt=*(const u32 *)(fadt+40);
    if (len>=148) {
        u64 x=*(const u64 *)(fadt+140);
        if (x && valid_table(x,"DSDT")) dsdt=x;
    }
    if (!pm1_cnt || pm1_cnt>65534 || pm1b_cnt>65534 || smi_cmd>65535 ||
        *(const u8 *)(fadt+89)<2 || !valid_table(dsdt,"DSDT")) {
        klog("acpi: unsupported ports or invalid DSDT"); return;
    }
    u32 dsdt_len=*(const u32 *)(dsdt+4);
    if (!parse_s5((const u8 *)(dsdt+36),dsdt_len-36)) {
        klog("acpi: constant _S5 package unavailable; not guessing a sleep type"); return;
    }
    acpi_ok=1;
    klog("acpi: power ready, PM1a=%x PM1b=%x S5a=%u S5b=%u",
         pm1_cnt,pm1b_cnt,slp_typa,slp_typb);
}

int acpi_shutdown(void)
{
    if (!acpi_ok) {
        klog("acpi: power-off unavailable"); return 0;
    }
    if (!(inw(pm1_cnt)&1) && smi_cmd && acpi_enable_val) {
        outb((u16)smi_cmd,acpi_enable_val);
        /* sleep_ms(1) used to round to ZERO at our 100 Hz PIT. */
        for (int i=0;i<300 && !(inw(pm1_cnt)&1);i++) sleep_ms(10);
    }
    if (!(inw(pm1_cnt)&1)) {
        klog("acpi: firmware did not enable ACPI mode"); return 0;
    }
    const u16 fields=(7u<<10)|(1u<<13);
    u16 a=(inw(pm1_cnt)&~fields)|(slp_typa<<10);
    u16 b=pm1b_cnt ? (inw(pm1b_cnt)&~fields)|(slp_typb<<10) : 0;
    /* Preserve SCI_EN and other unrelated control bits; each block has
     * its OWN sleep type from the two elements of _S5. */
    outw(pm1_cnt,a);
    if (pm1b_cnt) outw(pm1b_cnt,b);
    outw(pm1_cnt,a|(1u<<13));
    if (pm1b_cnt) outw(pm1b_cnt,b|(1u<<13));
    sleep_ms(1000);
    klog("acpi: S5 requested but firmware left the machine powered on");
    return 0; /* Successful power-off never returns. */
}

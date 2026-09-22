#include "efi.h"
#include "boot.h"
#include "gpu_match.h"
static struct system_table *system;
static struct boot_services *bs;
static struct boot_framebuffer fatal_framebuffer;
extern const uint8_t font8x16[256][16];
/* Keep an actual DIR64 fixup even though instruction references are PIC. */
static void *volatile relocation_anchor=&bs;
static const struct guid loaded_guid={0x5b1b31a1,0x9562,0x11d2,{0x8e,0x3f,0,0xa0,0xc9,0x69,0x72,0x3b}};
static const struct guid fs_guid={0x964e5b22,0x6459,0x11d2,{0x8e,0x39,0,0xa0,0xc9,0x69,0x72,0x3b}};
static const struct guid gop_guid={0x9042a9de,0x23dc,0x4a38,{0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}};
static const struct guid acpi_guid={0x8868e871,0xe4f1,0x11d3,{0xbc,0x22,0,0x80,0xc7,0x3c,0x88,0x81}};
void *memset(void *d,int c,size_t n){unsigned char *p=d;while(n--)*p++=(unsigned char)c;return d;}
void *memcpy(void *d,const void *s,size_t n){unsigned char *p=d;const unsigned char *q=s;while(n--)*p++=*q++;return d;}
static int equal(const void *a,const void *b,size_t n){const uint8_t *x=a,*y=b;while(n--)if(*x++!=*y++)return 0;return 1;}
static void out(uint16_t p,uint8_t v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}
static uint8_t in(uint16_t p){uint8_t v;__asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p));return v;}
static void serial(const char *p){while(*p){for(unsigned i=0;i<65536;i++)if(in(0x3fd)&32){out(0x3f8,*p);break;}p++;}}
static void log(const char *p){
 serial(p);char16 text[128];size_t i=0;
 while(*p){text[i++]=(uint8_t)*p++;if(i==127){text[i]=0;system->output->output(system->output,text);i=0;}}
 text[i]=0;system->output->output(system->output,text);
}
static _Noreturn void stopped(const char *p){
 serial(p);__asm__ volatile("cli");
 /* ExitBootServices may have partially shut firmware down even on failure.
  * Render directly through the already validated GOP buffer, without protocols,
  * allocation, locks or the kernel. Missing UART must not hide the error. */
 const struct boot_framebuffer *f=&fatal_framebuffer;
 if(f->base){
  volatile uint32_t *pixels=(void*)(uintptr_t)f->base;size_t x=0,y=0;
  uint32_t ink=f->red|f->green|f->blue;
  while(*p&&y+16<=f->height){
   uint8_t c=(uint8_t)*p++;
   if(c=='\n'||c=='\r'){x=0;y+=16;continue;}
   for(size_t dy=0;dy<16;dy++)for(size_t dx=0;dx<8;dx++)pixels[(y+dy)*f->stride+x+dx]=(font8x16[c][dy]&(128>>dx))?ink:0;
   x+=8;if(x+8>f->width){x=0;y+=16;}
  }
 }
 for(;;)__asm__ volatile("hlt");
}
/* PCI configuration access, read-only. The stub must name the GPU family to choose which single
 * module file to open, and it may not write a BAR, reset the device or touch a clock to find out
 * what the chip is - sizing a BAR writes 0xffffffff first, which is exactly how a boot loader
 * destroys the console it still needs. Configuration reads are harmless. */
static uint32_t in32(uint16_t port){uint32_t value;__asm__ volatile("inl %w1,%0":"=a"(value):"Nd"(port));return value;}
static void out32(uint16_t port,uint32_t value){__asm__ volatile("outl %0,%w1"::"a"(value),"Nd"(port));}
static uint32_t config_read(uint8_t bus,uint8_t slot,uint8_t function,uint8_t offset){
 if(slot>31||function>7)return 0;
 out32(0xcf8,0x80000000u|((uint32_t)bus<<16)|((uint32_t)slot<<11)|((uint32_t)function<<8)|(offset&0xfc));
 return in32(0xcfc);
}
/* First display function whose id the shared matcher binds.  Bus numbers are followed through
 * bridges because a discrete GPU normally sits behind a PCIe port, and a scan of bus 0 alone would
 * miss it; the walk is bounded and each secondary bus is visited once. */
static int scan_display(uint8_t bus,uint8_t depth,uint8_t *seen,unsigned *visited,
                        uint8_t *found_bus,uint8_t *found_slot,uint8_t *found_fn,
                        uint16_t *found_vendor,uint16_t *found_device,uint8_t *found_subclass,
                        const struct gpu_match **found_match){
 if(depth>8)return 0;
 for(uint8_t slot=0;slot<32;slot++){
  uint32_t vendev=config_read(bus,slot,0,0);
  if(vendev==0xffffffffu||!vendev)continue;
  uint32_t header=config_read(bus,slot,0,0xc);
  uint32_t ccrev=config_read(bus,slot,0,8);
  uint8_t subclass=(uint8_t)(ccrev>>16);
  for(uint8_t function=0;function<8;function++){
   if(function){
    uint32_t other=config_read(bus,slot,function,0);
    if(other==0xffffffffu||!other)continue;
    ccrev=config_read(bus,slot,function,8);subclass=(uint8_t)(ccrev>>16);
   }
   uint32_t cls=ccrev>>16;
   if(((cls>>8)&0xff)==3){
    uint16_t vendor=(uint16_t)vendev,device=(uint16_t)(vendev>>16);
    const struct gpu_match *match=gpu_match_device(vendor,device,subclass,0);
    if(match&&!*found_match){
     *found_match=match;*found_bus=bus;*found_slot=slot;*found_fn=function;
     *found_vendor=vendor;*found_device=device;*found_subclass=subclass;
    }
    if(function)break;
   }
  }
  if((header&0x7f)==1){
   uint32_t busnumbers=config_read(bus,slot,0,0x18);
   uint8_t secondary=(uint8_t)(busnumbers>>8),subordinate=(uint8_t)(busnumbers>>16);
   for(unsigned next=(unsigned)secondary+1;next<(unsigned)subordinate&&next<255u;next++){
    if(!next||seen[next])continue;
    seen[next]=1;if(*visited>=64)break;(*visited)++;
    if(scan_display(next,depth+1,seen,visited,found_bus,found_slot,found_fn,found_vendor,
                    found_device,found_subclass,found_match)&&*found_match)return 1;
   }
  }
 }
 return *found_match!=0;
}
static void family_file_name(const char *family,char16 *out){
 /* 8.3 stem derived from the family: first eight characters, uppercase, extension ".MOD".  The same
  * rule lives in tools/makedisk.py, and the module header carries the full family name, which the
  * kernel compares against its own detection before running a single instruction of it. */
 char stem[10];unsigned n=0;
 while(n<8&&family&&family[n]){stem[n]=(char)((family[n]>='a'&&family[n]<='z')?family[n]-32:family[n]);n++;}
 stem[n]=0;
 unsigned i=0;
 out[i++]=u'\\';out[i++]=u'S';out[i++]=u'C';out[i++]=u'O';out[i++]=u'S';out[i++]=u'\\';
 for(unsigned k=0;k<n;k++)out[i++]=(char16)stem[k];
 out[i++]=u'.';out[i++]=u'M';out[i++]=u'O';out[i++]=u'D';out[i]=0;
}
static uint64_t ticks(void){uint32_t a,d;__asm__ volatile("lfence; rdtsc":"=a"(a),"=d"(d)::"memory");return ((uint64_t)d<<32)|a;}
static uint32_t crc32(const uint8_t *p,size_t n){uint32_t c=~0u;while(n--){c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^(0xedb88320u&-(c&1));}return ~c;}
/* Read from the loaded image's own filesystem only; never search other disks. */
static status read_file(struct file *root,const char16 *name,void **data,size_t *length){
 struct file *f=0;status s=root->open(root,&f,name,1,0);if(FAILED(s))return s;
 uint64_t size=0;s=f->set_position(f,UINT64_MAX);
 if(!FAILED(s))s=f->get_position(f,&size);
 if(!FAILED(s)&&(!size||size>16*1024*1024))s=ERROR(4);
 if(!FAILED(s))s=f->set_position(f,0);
 if(!FAILED(s))s=bs->allocate_pool(2,(size_t)size,data);
 if(!FAILED(s)){
  size_t done=0;
  while(done<size){size_t part=(size_t)size-done;s=f->read(f,&part,(uint8_t*)*data+done);if(FAILED(s)||!part||part>size-done){s=ERROR(7);break;}done+=part;}
  if(FAILED(s)){bs->free_pool(*data);*data=0;}else *length=(size_t)size;
 }
 status close=f->close(f);return FAILED(s)?s:close;
}
struct elf_header {uint8_t ident[16];uint16_t type,machine;uint32_t version;uint64_t entry,phoff,shoff;uint32_t flags;uint16_t size,phsize,phcount,shsize,shcount,shstr;};
struct elf_program {uint32_t type,flags;uint64_t offset,virtual_address,physical,filesz,memsz,align;};
_Static_assert(sizeof(struct elf_header)==64&&sizeof(struct elf_program)==56,"ELF ABI");
/* Only a self-contained PIE is accepted: no interpreter, symbol resolution,
 * external dependencies, TLS or executable-page relocation writes. */
static status load_kernel(const void *buffer,size_t size,uint64_t *base_out,uint64_t *end,uint64_t *entry){
 const struct elf_header *h=buffer;
 if(size<sizeof(*h)||!equal(h->ident,"\177ELF\2\1\1",7)||h->type!=3||h->machine!=62||h->version!=1||h->size!=64||h->phsize!=56||!h->phcount||h->phcount>32||h->phoff<64||(h->phoff&7)||h->phoff>size||h->phcount>(size-h->phoff)/56)return ERROR(3);
 const struct elf_program *p=(const void*)((const uint8_t*)buffer+h->phoff);
 uint64_t high=0;int executable=0;const struct elf_program *dynamic=0;
 for(unsigned i=0;i<h->phcount;i++){
  if(p[i].type==3||p[i].type==7)return ERROR(3);
  if(p[i].type==2){if(dynamic)return ERROR(3);dynamic=&p[i];}
  if(p[i].type!=1)continue;
  if(p[i].physical!=p[i].virtual_address||p[i].physical>=KERNEL_SPAN_LIMIT||p[i].memsz>KERNEL_SPAN_LIMIT-p[i].physical||p[i].filesz>p[i].memsz||p[i].offset>size||p[i].filesz>size-p[i].offset||!(p[i].flags&4)||(p[i].flags&~7)||((p[i].flags&3)==3))return ERROR(3);
  if(p[i].align>1&&((p[i].align&(p[i].align-1))||((p[i].physical-p[i].offset)&(p[i].align-1))))return ERROR(3);
  if(p[i].physical&4095)return ERROR(3);
  uint64_t limit=(p[i].physical+p[i].memsz+4095)&~4095ULL;
  for(unsigned j=0;j<i;j++)if(p[j].type==1&&p[j].memsz&&p[i].memsz&&p[i].physical<((p[j].physical+p[j].memsz+4095)&~4095ULL)&&limit>p[j].physical)return ERROR(3);
  if((p[i].flags&1)&&h->entry>=p[i].physical&&h->entry-p[i].physical<p[i].filesz)executable=1;
  if(limit>high)high=limit;
 }
 if(!executable||h->entry!=0||!high||!dynamic||dynamic->offset>size||dynamic->filesz>size-dynamic->offset||dynamic->filesz%16||(dynamic->offset&7))return ERROR(3);
 uint64_t rela=0,rela_size=0,rela_stride=0;int terminated=0;
 const uint64_t *tags=(const void*)((const uint8_t*)buffer+dynamic->offset);
 for(size_t i=0;i<dynamic->filesz/16;i++){
  uint64_t tag=tags[i*2],value=tags[i*2+1];
  if(!tag){terminated=1;break;}
  if(tag==1||tag==17||tag==18||tag==22||tag==23||tag==35||tag==36||tag==37)return ERROR(3);
  if(tag==7)rela=value;
  if(tag==8)rela_size=value;
  if(tag==9)rela_stride=value;
 }
 if(!terminated||(rela&7)||rela_stride!=24||rela_size%24||rela_size>1024*1024)return ERROR(3);
 const uint64_t *relocations=0;
 for(unsigned i=0;i<h->phcount;i++)if(p[i].type==1&&rela>=p[i].physical&&rela-p[i].physical<=p[i].filesz&&rela_size<=p[i].filesz-(rela-p[i].physical))relocations=(const void*)((const uint8_t*)buffer+p[i].offset+rela-p[i].physical);
 if(!relocations)return ERROR(3);
 for(size_t r=0;r<rela_size/24;r++){
  uint64_t offset=relocations[r*3],info=relocations[r*3+1],addend=relocations[r*3+2];int writable=0;
  if((offset&7)||info!=8||addend>=high)return ERROR(3); /* R_X86_64_RELATIVE, symbol zero */
  for(unsigned i=0;i<h->phcount;i++)if(p[i].type==1&&(p[i].flags&2)&&p[i].memsz>=8&&offset>=p[i].physical&&offset-p[i].physical<=p[i].memsz-8)writable=1;
  if(!writable)return ERROR(3);
 }
 uint64_t base=0xffffffff;status s=bs->allocate_pages(1,1,high/4096,&base);if(FAILED(s))return s;
 if(!base){bs->free_pages(base,high/4096);return ERROR(3);}
 memset((void*)(uintptr_t)base,0,high);
 for(unsigned i=0;i<h->phcount;i++)if(p[i].type==1&&p[i].filesz)memcpy((void*)(uintptr_t)(base+p[i].physical),(const uint8_t*)buffer+p[i].offset,p[i].filesz);
 for(size_t r=0;r<rela_size/24;r++){
  uint64_t value=base+relocations[r*3+2];memcpy((void*)(uintptr_t)(base+relocations[r*3]),&value,8);
 }
 *base_out=base;*end=base+high;*entry=base+h->entry;return 0;
}
static int channel(uint32_t m){if(!m)return 0;while(!(m&1))m>>=1;return !(m&(m+1))&&m<=1023;}
static int mode_valid(const struct mode_info *m){
 if(m->width<640||m->height<400||m->width>4096||m->height>2160||m->stride<m->width||m->stride>16384)return 0;
 if(m->format<2)return 1;
 return m->format==2&&channel(m->red)&&channel(m->green)&&channel(m->blue)&&!(m->red&m->green)&&!(m->red&m->blue)&&!(m->green&m->blue);
}
static status graphics(struct boot_framebuffer *f){
 struct gop *g=0;status s=bs->handle_protocol(system->output_handle,&gop_guid,(void**)&g);
 if(FAILED(s))s=bs->locate_protocol(&gop_guid,0,(void**)&g);
 if(FAILED(s))return s;
 uint32_t skipped[6];size_t skipped_count=0;
 /* A mode list describes what the firmware can *describe*, not what the device can *hold*.  EDK2's
  * Cirrus GOP advertises 1024x768 while reporting a frame buffer only large enough for 800x600, and
  * a real card's list routinely carries sizes whose stride does not fit the reserved aperture.
  * Picking the most preferred mode and then refusing to boot over its aperture would leave a machine
  * with a perfectly usable smaller mode on a black screen, so the fit is checked after the mode is
  * actually set, and a mode that cannot hold its own screen is skipped rather than fatal. */
 for(;;){
  uint32_t selected=UINT32_MAX;uint64_t best=UINT64_MAX;
  for(uint32_t i=0;i<g->mode->max_mode&&i<4096;i++){
   int tried=0;for(size_t k=0;k<skipped_count;k++)if(skipped[k]==i)tried=1;
   if(tried)continue;
   size_t n=0;struct mode_info *m=0;s=g->query(g,i,&n,&m);
   if(!FAILED(s)&&m){if(n>=sizeof(*m)&&mode_valid(m)){uint64_t pixels=(uint64_t)m->width*m->height;uint64_t score=pixels>=1024*768?pixels-1024*768:(1ULL<<32)+1024*768-pixels;if(score<best){best=score;selected=i;}}bs->free_pool(m);}
  }
  if(selected==UINT32_MAX)return skipped_count?ERROR(3):s;
  s=g->set(g,selected);
  const struct mode_info *m=g->mode->info;
  if(FAILED(s)||g->mode->info_size<sizeof(*m)||!mode_valid(m)||!g->mode->base||
     g->mode->base>=PHYSICAL_LIMIT||g->mode->size>PHYSICAL_LIMIT-g->mode->base||
     (uint64_t)m->stride*m->height*4>g->mode->size){
   /* Unusable, whether the firmware refused it or handed it over too small: remember it so the next
    * pass chooses the best of what is left instead of picking the same mode again. */
   if(skipped_count<sizeof skipped/sizeof*skipped)skipped[skipped_count++]=selected;
   continue;
  }
  *f=(struct boot_framebuffer){g->mode->base,g->mode->size,m->width,m->height,m->stride,m->red,m->green,m->blue};
  if(m->format==0){f->red=255;f->green=65280;f->blue=16711680;}
  if(m->format==1){f->red=16711680;f->green=65280;f->blue=255;}
  return 0;
 }
}
status EFIAPI efi_main(handle image,struct system_table *table){
 system=table;
 if(!table||table->header.signature!=UINT64_C(0x5453595320494249)||!table->boot||!table->output)return ERROR(2);
 bs=table->boot;
 if(relocation_anchor!=&bs)return ERROR(3);
 out(0x3f9,0);out(0x3fb,0x80);out(0x3f8,1);out(0x3f9,0);out(0x3fb,3);out(0x3fa,7);out(0x3fc,3);
 log("SCos native x64 UEFI loader\r\n");
 uint32_t a,b,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0x80000000));
 if(a<0x80000001)goto unsupported;
 __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0x80000001));
 __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1));
 if((d&((1u<<4)|(1u<<5)|(1u<<16)))!=((1u<<4)|(1u<<5)|(1u<<16)))goto unsupported;
 __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0x80000001));
 uint64_t cr4;__asm__ volatile("mov %%cr4,%0":"=r"(cr4));
 if(!(d&(1u<<20))||(cr4&((1u<<12)|(1u<<17))))goto unsupported;
 const char *phase="disable watchdog";
 struct loaded_image *loaded=0;struct filesystem *fs=0;struct file *root=0;
 void *elf=0,*checksum=0;size_t elf_size=0,checksum_size=0;
 uint64_t kernel_base=0,end=0,entry=0,arena=0;int have_arena=0;
 status s=bs->watchdog(0,0,0,0);if(FAILED(s))goto fail;
 phase="loaded-image protocol";s=bs->handle_protocol(image,&loaded_guid,(void**)&loaded);if(FAILED(s))goto fail;
 phase="boot-volume filesystem";s=bs->handle_protocol(loaded->device,&fs_guid,(void**)&fs);if(FAILED(s))goto fail;
 phase="open boot volume";s=fs->open_volume(fs,&root);if(FAILED(s))goto fail;
 phase="read kernel ELF";s=read_file(root,(const char16*)u"\\SCOS\\KERNEL.ELF",&elf,&elf_size);
 if(!FAILED(s)){phase="read kernel CRC";s=read_file(root,(const char16*)u"\\SCOS\\KERNEL.CRC",&checksum,&checksum_size);}
 /* Choose the one GPU driver module this machine needs, while the boot volume handle is still
  * open: it is closed immediately below and nothing may use it afterwards.  The bytes land in a
  * pool for now and move into the executable boot-arena region once that region exists. */
 void *module_data=0;size_t module_size=0;void *index_data=0;size_t index_size=0;
 uint32_t store_count=0,select_state=0;char selected_family[16];selected_family[0]=0;
 {
  uint8_t seen[256];memset(seen,0,sizeof(seen));unsigned visited=0;
  uint8_t gbus=0,gslot=0,gfn=0,gsub=0;uint16_t gvendor=0,gdevice=0;
  const struct gpu_match *match=0;
  scan_display(0,0,seen,&visited,&gbus,&gslot,&gfn,&gvendor,&gdevice,&gsub,&match);
  const char *family=match?gpu_family_name(match):"";
  if(match&&!gpu_match_named_only(gvendor,gdevice,gsub)){
   /* A chip the naming table knows but no driver table binds gets no module read at all: firmware
    * must not hand the kernel bytes for a chip generation nothing can drive. */
   unsigned n=0;while(n<15&&family[n]){selected_family[n]=family[n];n++;}selected_family[n]=0;
   char16 path[32];family_file_name(family,path);
   phase="read gpu driver module";s=read_file(root,path,&module_data,&module_size);
   if(FAILED(s)||module_size>GPU_MODULE_REGION||!module_size){
    select_state=2;
    if(module_data){bs->free_pool(module_data);module_data=0;}
    module_size=0;
    /* EFI_NOT_FOUND: the disk simply has no module for this family, which is a normal state.  Any
     * other failure is an I/O problem and must not be reported as "not present". */
    if((uint64_t)(uint64_t)(int64_t)s==0x800000000000000Eull)
     log("No driver module on disk for the detected GPU family; CPU compositor\r\n");
    else log("GPU driver module could not be read; CPU compositor\r\n");
   }else{
    select_state=1;
    log("GPU driver module read for this chip\r\n");
   }
  }else log("No GPU family matched by the generated tables; no module read\r\n");
  /* The index is a small fixed table, not a directory walk: it lets the kernel report how many
   * modules exist on the disk without the firmware having opened any of them. */
  {void *index=0;size_t bytes=0;status found=read_file(root,(const char16*)u"\\SCOS\\DRVLIST.IDX",&index,&bytes);
   if(!FAILED(found)&&index&&bytes>=8){
    if(bytes>BOOT_INDEX_MAX)bytes=BOOT_INDEX_MAX;
    index_data=index;index_size=bytes;store_count=*(uint32_t*)index;
   }else if(index)bs->free_pool(index);}
 }
 {status close=root->close(root);root=0;if(!FAILED(s)&&FAILED(close)){phase="close boot volume";s=close;}}
 if(FAILED(s))goto fail;
 if(checksum_size!=4||crc32(elf,elf_size)!=*(uint32_t*)checksum){log("Kernel file CRC mismatch\r\n");s=ERROR(27);goto fail;}
 phase="validate/load ELF";s=load_kernel(elf,elf_size,&kernel_base,&end,&entry);if(FAILED(s))goto fail;
 bs->free_pool(elf);elf=0;bs->free_pool(checksum);checksum=0;
 phase="GOP framebuffer";struct boot_framebuffer fb;s=graphics(&fb);if(FAILED(s))goto fail;
 fatal_framebuffer=fb;
 phase="allocate boot arena";arena=0xffffffff;s=bs->allocate_pages(1,2,BOOT_ARENA_SIZE/4096,&arena);if(FAILED(s))goto fail;
 have_arena=1;
 memset((void*)(uintptr_t)arena,0,BOOT_ARENA_SIZE);
 struct boot_handoff *h=(void*)(uintptr_t)arena;
 *h=(struct boot_handoff){.magic=BOOT_MAGIC,.version=BOOT_VERSION,.size=sizeof(*h),.kernel_start=kernel_base,.kernel_end=end,.arena_start=arena,.arena_size=BOOT_ARENA_SIZE,.framebuffer=fb};
 for(size_t i=0;i<table->table_count&&i<4096;i++)if(equal(&table->tables[i].guid,&acpi_guid,sizeof(acpi_guid))){h->rsdp=(uintptr_t)table->tables[i].table;break;}
 phase="calibrate startup timeout";
 uint64_t start_ticks=ticks();s=bs->stall(10000);uint64_t elapsed=ticks()-start_ticks;
 if(FAILED(s)||elapsed<10000||elapsed>UINT64_C(1000000000)){s=ERROR(3);goto fail;}
 h->tsc_hz=elapsed*100;
 h->map_address=arena+4096;
 if(select_state==1&&module_data&&module_size){
  uint64_t module_area=GPU_MODULE_AREA(arena);
  memset((void*)(uintptr_t)module_area,0,GPU_MODULE_REGION);
  memcpy((void*)(uintptr_t)module_area,module_data,module_size);
  h->module_address=module_area;h->module_bytes=module_size;h->module_state=1;
  h->module_crc=crc32((const uint8_t*)(uintptr_t)module_area,module_size);
  unsigned n=0;while(n<15&&selected_family[n]){h->module_name[n]=selected_family[n];n++;}
  h->module_name[n]=0;
 }else h->module_state=select_state?select_state:0;
 if(index_data&&index_size){
  memcpy((void*)(uintptr_t)BOOT_INDEX_ADDRESS(arena),index_data,index_size);
  h->index_address=BOOT_INDEX_ADDRESS(arena);h->module_index_size=(uint32_t)index_size;
  h->module_store_count=store_count;
 }
 if(module_data)bs->free_pool(module_data);
 if(index_data)bs->free_pool(index_data);
 log(selected_family[0]?"GPU module staged for the kernel loader\r\n":"no GPU module staged; CPU compositor\r\n");
 log("ELF64 and GOP ready; leaving firmware services\r\n");
 /* Nothing allocating, printing through firmware or closing files may occur
  * between GetMemoryMap and ExitBootServices. Retry only with fresh map/key.
  * After the first attempt even failures must not call console/protocol APIs. */
 for(unsigned attempt=0;attempt<8;attempt++){
  size_t bytes=128*1024,key=0,stride=0;uint32_t version=0;
  s=bs->get_memory_map(&bytes,(void*)(uintptr_t)h->map_address,&key,&stride,&version);
  if(FAILED(s)||stride<40||stride>4096||!bytes||bytes%stride||bytes/stride>BOOT_MAP_MAX||version!=1)stopped("HALTED: unsupported final UEFI memory map\n");
  h->map_size=bytes;h->map_stride=stride;h->map_version=version;
  s=bs->exit_boot_services(image,key);
  if(!FAILED(s)){
   __asm__ volatile("cli":::"memory");out(0x70,0x80);out(0x21,255);out(0xa1,255);
   uint32_t lo,hi;__asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0xc0000080));lo|=1u<<11;
   __asm__ volatile("wrmsr"::"a"(lo),"d"(hi),"c"(0xc0000080):"memory");
   ((void(*)(const struct boot_handoff*))(uintptr_t)entry)(h);
   stopped("HALTED: kernel returned\n");
  }
  if(s!=INVALID_PARAMETER)stopped("HALTED: ExitBootServices failed\n");
 }
 stopped("HALTED: firmware memory map never stabilized\n");
unsupported:
 log("HALTED: NX/TSC/MSR/PAT required; LA57/PCID firmware paging unsupported\r\n");return ERROR(3);
fail:
 if(root)root->close(root);
 if(elf)bs->free_pool(elf);
 if(checksum)bs->free_pool(checksum);
 if(kernel_base)bs->free_pages(kernel_base,(end-kernel_base)/4096);
 if(have_arena)bs->free_pages(arena,BOOT_ARENA_SIZE/4096);
 log("SCos UEFI startup failed; firmware services retained\r\n");log(phase);log("\r\n");
 /* Report exact status, without a libc formatting dependency. */
 {char text[32]="status=0x";for(unsigned i=0;i<16;i++)text[9+i]="0123456789abcdef"[(s>>(60-i*4))&15];text[25]='\r';text[26]='\n';text[27]=0;log(text);}
 return s;
}

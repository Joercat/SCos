#include "kernel.h"
#define PAGE UINT64_C(4096)
#define HUGE UINT64_C(0x200000)
#define NX (UINT64_C(1)<<63)
#define ADDRESS UINT64_C(0x000ffffffffff000)
#define EXTENTS 1024
struct extent {uintptr_t start,end;};
static struct extent free_ranges[EXTENTS];
static size_t range_count;
static uint64_t free_pages;
static uintptr_t table_next,table_end;
static uint64_t *root;
static struct efi_memory owned_map[BOOT_MAP_MAX];
static size_t owned_count;
static int initialized;
static uint64_t reserved_pages;
static uint64_t tables_from_arena,tables_from_allocator;  /* reported: is the pool actually big enough? */
static uint64_t irq_save(void){uint64_t f;__asm__ volatile("pushfq;popq %0;cli":"=r"(f)::"memory");return f;}
static void irq_restore(uint64_t f){if(f&512)__asm__ volatile("sti":::"memory");}
/* Page-table frames come out of the boot arena's pool until the allocator exists, and out of the
 * allocator afterwards.  The pool is sized for everything the identity mapping and the module region
 * need (dozens of pages), but a mapping need must never be able to fail the machine at runtime: before
 * this fallback, growing a mapping past the pool panicked, and the only way to avoid that was to keep
 * megabytes of RAM reserved that nothing else could use.  pages_allocate() already returns zeroed
 * memory, which is what a table frame has to start as. */
static uint64_t *new_table(void){
 if(table_next<table_end){
  uint64_t *p=(void*)table_next;table_next+=PAGE;memset(p,0,PAGE);tables_from_arena++;return p;
 }
 uint64_t *p=0;
 if(initialized){
  uintptr_t got=pages_allocate(1);
  if(got){p=(void*)got;tables_from_allocator++;}
 }
 if(!p)panic("page-table frames unavailable");
 return p;
}
static uint64_t *descend(uint64_t *table,size_t index){
 if(table[index]&128)panic("conflicting page-table leaf");
 if(!(table[index]&1))table[index]=(uintptr_t)new_table()|3;
 return (void*)(uintptr_t)(table[index]&ADDRESS);
}
static void map_leaf(uintptr_t p,uint64_t flags,int huge){
 uint64_t *pdpt=descend(root,(p>>39)&511),*pd=descend(pdpt,(p>>30)&511);
 uint64_t *slot;
 if(huge)slot=&pd[(p>>21)&511];
 else {uint64_t *pt=descend(pd,(p>>21)&511);slot=&pt[(p>>12)&511];}
 if(*slot)panic("overlapping virtual mappings");
 *slot=p|flags|(huge?128:0);
}
static void map_range(uintptr_t start,uintptr_t end,uint64_t flags){
 while(start<end){int huge=!(start&(HUGE-1))&&end-start>=HUGE;map_leaf(start,flags,huge);start+=huge?HUGE:PAGE;}
}
static int guard(uintptr_t p){return p==(uintptr_t)stack_guard||p==(uintptr_t)df_guard||p==(uintptr_t)nmi_guard||p==(uintptr_t)mc_guard;}
static int overlaps(uint64_t a,uint64_t b,uint64_t c,uint64_t d){return a<d&&b>c;}
void memory_init(const struct boot_handoff *b,const struct efi_memory *map){
 if(initialized)panic("memory initialized twice");
 reserved_pages=(b->kernel_end-b->kernel_start+b->arena_size)/PAGE;
 owned_count=b->map_size/b->map_stride;
 memcpy(owned_map,map,owned_count*sizeof(*map));
 /* Reserve handoff/map prefix; table arena was explicitly allocated by UEFI,
  * never guessed free from a low-address hole. Loader memory stays owned. */
 /* The top of the boot arena is carved out as the only executable memory outside the kernel image:
  * the region a GPU display module is loaded into.  Everything else stays NX, so "modules are
  * writable code" is bounded to this one range, and the page allocator is cut short above it so no
  * other consumer can be handed these pages. */
 uint64_t module_area=GPU_MODULE_AREA(b->arena_start),arena_end=b->arena_start+b->arena_size;
 if(module_area<=b->arena_start+256*1024)panic("boot arena too small for a module region");
 table_next=b->arena_start+256*1024;table_end=module_area;
 root=new_table();
 uint64_t fb_start=b->framebuffer.base&~4095ULL;
 uint64_t fb_end=(b->framebuffer.base+b->framebuffer.size+4095)&~4095ULL;
 if(overlaps(fb_start,fb_end,b->kernel_start,b->kernel_end)||overlaps(fb_start,fb_end,b->arena_start,arena_end))panic("framebuffer overlaps owned RAM");
 for(size_t i=0;i<owned_count;i++)if(boot_memory_usable(&map[i])){
  uintptr_t start=map[i].physical,end=start+map[i].pages*PAGE;
  if(!start)start=PAGE; /* Never allocate or map address zero. */
  if(start==end)continue;
  if(overlaps(start,end,b->kernel_start,b->kernel_end)||overlaps(start,end,b->arena_start,arena_end)||overlaps(start,end,fb_start,fb_end))panic("firmware ownership conflict");
  map_range(start,end,3|NX);
  size_t pos=range_count;
  if(pos==EXTENTS)panic("too many RAM extents");
  while(pos&&free_ranges[pos-1].start>start){free_ranges[pos]=free_ranges[pos-1];pos--;}
  free_ranges[pos]=(struct extent){start,end};range_count++;free_pages+=(end-start)/PAGE;
 }
 for(uintptr_t p=b->kernel_start;p<b->kernel_end;p+=PAGE)if(!guard(p)){
  uint64_t flags=3|NX;
  if(p>=(uintptr_t)_text_start&&p<(uintptr_t)_text_end)flags=1;
  else if((p>=(uintptr_t)_rodata_start&&p<(uintptr_t)_rodata_end)||(p>=(uintptr_t)_relro_start&&p<(uintptr_t)_relro_end))flags=1|NX;
  map_leaf(p,flags,0);
 }
 map_range(b->arena_start,table_end,3|NX);
 /* RWX by necessity, and only here: a module is code the kernel jumps into after validating it, and
  * map_leaf() refuses to place a second mapping over the same pages, so the region cannot be
  * mapped NX for the allocator and executable for the loader separately.  It is never writable data
  * for anything else, and it is reported by `gpu: module region ...' in the boot log. */
 map_range(module_area,arena_end,3);
 /* PAT slot 1 is WC, only for validated GOP pixel memory. Device registers
  * remain UC in slot 3, RAM remains WB. No MTRRs are rewritten. */
 map_range(fb_start,fb_end,3|NX|0x08);
 if(!free_pages)panic("no conventional RAM available");
 /* Firmware may leave global translations/PAT settings behind. Disable global
  * translations, flush caches while changing PAT, use WC slot 1 for GOP and UC slot 3 for device registers.
  * No AP runs; no runtime firmware calls are permitted after this point. */
 uint64_t cr0,cr4;__asm__ volatile("mov %%cr0,%0":"=r"(cr0));
 __asm__ volatile("mov %%cr4,%0":"=r"(cr4));
 uint64_t cache_off=(cr0|(1ULL<<30))&~(1ULL<<29);
 __asm__ volatile("mov %0,%%cr0; wbinvd"::"r"(cache_off):"memory");
 __asm__ volatile("wrmsr"::"c"(0x277),"a"(0x00070106u),"d"(0x00070106u):"memory");
 cr4&=~(1ULL<<7);__asm__ volatile("mov %0,%%cr4"::"r"(cr4):"memory");
 __asm__ volatile("mov %0,%%cr3"::"r"((uintptr_t)root):"memory");
 cr0=(cr0|(1ULL<<16))&~((1ULL<<30)|(1ULL<<29));
 __asm__ volatile("wbinvd; mov %0,%%cr0"::"r"(cr0):"memory");
 initialized=1;
}
/* The leaf for p, only when the walk already exists.  This must not allocate: probing an unmapped
 * range may not leave an empty table behind, since the next caller to look at that 2MiB block would
 * otherwise find a half-built walk and have to decide whether it meant "mapped" or "not mapped". */
static uint64_t *device_leaf(uintptr_t p){
 uint64_t *pdpt=&root[(p>>39)&511];
 if(!(*pdpt&1))return 0;
 uint64_t *pd=(uint64_t*)(uintptr_t)(*pdpt&ADDRESS)+((p>>30)&511);
 if(!(*pd&1))return 0;
 if(*pd&128)return pd;                     /* a 2MiB leaf already covers p */
 uint64_t *pt=(uint64_t*)(uintptr_t)(*pd&ADDRESS)+((p>>21)&511);
 if(!(*pt&1))return 0;
 return (uint64_t*)(uintptr_t)(*pt&ADDRESS)+((p>>12)&511);
}
/* Map device memory for a driver that has taken ownership of its function.  The kernel identity-maps
 * physical RAM, so device ranges are mapped the same way: the address returned equals the physical
 * address, and a page that is already mapped is *reused* rather than remapped, which is what makes
 * mapping a framebuffer aperture the firmware already established (or that memory_init mapped for
 * GOP) a no-op instead of a page-table conflict.  Attributes are always the UC/WC pair used for the
 * console, never a third combination, so no two mappings of the same memory can disagree.
 * Returns 0 when the range is not mappable (over the limit, or a leaf that is not ours). */
void *device_map(uint64_t physical,uint64_t bytes,int write_combine,uint64_t *mapped_bytes){
 if(!initialized||!bytes||bytes>PAGE*2048)return 0;
 if(physical>>46)return 0;
 uintptr_t start=physical&~(uintptr_t)(PAGE-1);
 uintptr_t end=(physical+bytes+PAGE-1)&~(uintptr_t)(PAGE-1);
 uint64_t flags=3|NX|(write_combine?0:0x08);
 uintptr_t first=0;
 for(uintptr_t p=start;p<end;p+=PAGE){
  /* A leaf that exists *and carries an address* is reused as is; one whose walk is only half built
   * - the usual state just past a 2MiB boundary - is filled by the walk below.  Refusing in that
   * second case is what used to make every mapping that crossed a 2MiB line fail. */
  uint64_t *leaf=device_leaf(p);
  if(leaf&&*leaf)continue;
  uint64_t *pdpt=descend(root,(p>>39)&511),*pd=descend(pdpt,(p>>30)&511);
  uint64_t *pt=descend(pd,(p>>21)&511),*slot=&pt[(p>>12)&511];
  if(*slot)return 0;                     /* appeared mid-walk: refuse rather than double-map */
  *slot=p|flags;
  if(!first)first=p;
 }
 if(first){uint64_t cr3;__asm__ volatile("mov %%cr3,%0; mov %0,%%cr3":"=r"(cr3):"r"((uintptr_t)root):"memory");}
 if(mapped_bytes)*mapped_bytes=end-start;
 return (void*)(uintptr_t)(physical+(start-physical));
}
/* Unmap a range the caller mapped through device_map, so a driver that is detached does not leave
 * executable-adjacent device memory mapped.  Only pages carrying exactly the flags device_map
 * installs are removed, and the flush is a single global TLB invalidation. */
void device_unmap(uint64_t physical,uint64_t bytes){
 uintptr_t start=physical&~(uintptr_t)(PAGE-1),end=(physical+bytes+PAGE-1)&~(uintptr_t)(PAGE-1);
 for(uintptr_t p=start;p<end;p+=PAGE){
  uint64_t *slot=device_leaf(p);
  if(slot&&(*slot&~(uint64_t)0xfff)==(3|NX|0x08|p))*slot=0;
 }
 __asm__ volatile("mov %%cr3,%0; mov %0,%%cr3"::"r"((uintptr_t)root):"memory");
}

/* Contiguous extents are needed by existing surface/document buffers. Both the
 * single-page API and desktop heap use this one ownership/accounting authority. */
uintptr_t pages_allocate_limit(size_t count,uint64_t limit){
 uint64_t f=irq_save();
 if(!initialized)panic("allocation before memory ownership established");
 if(!count||count>PHYSICAL_LIMIT/PAGE){irq_restore(f);return 0;}
 uint64_t bytes=count*PAGE;
 for(size_t i=0;i<range_count;i++)if(free_ranges[i].end-free_ranges[i].start>=bytes&&free_ranges[i].start<limit&&bytes<=limit-free_ranges[i].start){
  uintptr_t p=free_ranges[i].start;free_ranges[i].start+=bytes;free_pages-=count;
  if(free_ranges[i].start==free_ranges[i].end){for(size_t j=i+1;j<range_count;j++)free_ranges[j-1]=free_ranges[j];range_count--;}
  memset((void*)p,0,bytes);irq_restore(f);return p;
 }
 irq_restore(f);return 0;
}
static int span_owned(uintptr_t p,uintptr_t end){
 while(p<end){uintptr_t next=p;
  for(size_t i=0;i<owned_count;i++)if(boot_memory_usable(&owned_map[i])&&p>=owned_map[i].physical&&p<owned_map[i].physical+owned_map[i].pages*PAGE){next=owned_map[i].physical+owned_map[i].pages*PAGE;break;}
  if(next==p)return 0;
  p=next;
 }
 return 1;
}
void pages_release(uintptr_t p,size_t count){
 uint64_t f=irq_save();
 if(!initialized||!p||(p&(PAGE-1))||!count||p>=PHYSICAL_LIMIT||count>(PHYSICAL_LIMIT-p)/PAGE||!span_owned(p,p+count*PAGE))panic("release outside owned conventional RAM");
 uintptr_t end=p+count*PAGE;
 size_t i=0;while(i<range_count&&free_ranges[i].end<=p)i++;
 if(i<range_count&&free_ranges[i].start<end)panic("double/overlapping page release");
 if(i&&free_ranges[i-1].end==p){
  free_ranges[i-1].end=end;
  if(i<range_count&&free_ranges[i].start==end){free_ranges[i-1].end=free_ranges[i].end;for(size_t j=i+1;j<range_count;j++)free_ranges[j-1]=free_ranges[j];range_count--;}
 }else if(i<range_count&&free_ranges[i].start==end)free_ranges[i].start=p;
 else{
  if(range_count==EXTENTS)panic("free-range fragmentation capacity exceeded");
  for(size_t j=range_count;j>i;j--)free_ranges[j]=free_ranges[j-1];
  free_ranges[i]=(struct extent){p,end};range_count++;
 }
 free_pages+=count;irq_restore(f);
}
uintptr_t page_allocate(void){return pages_allocate(1);}
void page_release(uintptr_t p){pages_release(p,1);}
uint64_t memory_free_pages(void){return free_pages;}
uint64_t memory_reserved_pages(void){return reserved_pages;}
/* Table frames taken from the boot arena's pool, and from the page allocator after init: the pair says
 * whether BOOT_ARENA_SIZE is still a sane reservation, and is asserted by tools/tests/test_memory.py. */
uint64_t memory_table_pages(int from_arena){return from_arena?tables_from_arena:tables_from_allocator;}

uintptr_t pages_allocate(size_t count){return pages_allocate_limit(count,PHYSICAL_LIMIT);}

/* Device mappings never alias allocated/firmware RAM with a different cache
 * policy. Firmware-table reads require ACPI reclaim/NVS ownership. No guessed
 * direct physical casts survive the switch away from firmware page tables. */
static uint64_t mapped_leaf(uintptr_t address){
 uint64_t *t=root;
 for(int level=3;level>=0;level--){uint64_t e=t[(address>>(12+level*9))&511];if(!(e&1))return 0;if(!level||(e&128))return e;t=(void*)(uintptr_t)(e&ADDRESS);}
 return 0;
}
static void *map_physical(uint64_t address,size_t bytes,int device){
 if(!initialized||!bytes||address<4096||address>=PHYSICAL_LIMIT||bytes>PHYSICAL_LIMIT-address)return NULL;
 uint64_t start=address&~4095ULL,end=(address+bytes+4095)&~4095ULL;
 if(device){
  for(size_t i=0;i<owned_count;i++)if(owned_map[i].pages&&owned_map[i].type!=0&&owned_map[i].type!=11&&overlaps(start,end,owned_map[i].physical,owned_map[i].physical+owned_map[i].pages*PAGE))return NULL;
 }else{
  int found=0;
  for(size_t i=0;i<owned_count;i++)if((owned_map[i].type==9||owned_map[i].type==10)&&start>=owned_map[i].physical&&end<=owned_map[i].physical+owned_map[i].pages*PAGE)found=1;
  if(!found)return NULL;
 }
 uint64_t flags=1|NX|(device?0x1a:0),saved=irq_save();
 for(uintptr_t p=start;p<end;p+=PAGE){uint64_t old=mapped_leaf(p);if(old&&((old&0x1b)!=(flags&0x1b))){irq_restore(saved);return NULL;}}
 for(uintptr_t p=start;p<end;p+=PAGE)if(!mapped_leaf(p)){map_leaf(p,flags,0);__asm__ volatile("invlpg (%0)"::"r"(p):"memory");}
 irq_restore(saved);return (void*)(uintptr_t)address;
}
void *mmio_map(uint64_t address,size_t bytes){return map_physical(address,bytes,1);}
const void *firmware_map(uint64_t address,size_t bytes){return map_physical(address,bytes,0);}

/* Fault-time inspection: no allocation, device reads or unmapped stack walks.
 * Only backed RAM in our current identity map is eligible. */
int memory_read_u64(uint64_t address,uint64_t *out){
 if(!initialized||!out||address>=PHYSICAL_LIMIT||8>PHYSICAL_LIMIT-address)return 0;
 int ram=0;
 for(size_t i=0;i<owned_count;i++)if((boot_memory_usable(&owned_map[i])||owned_map[i].type==1||owned_map[i].type==2)&&address>=owned_map[i].physical&&address+8<=owned_map[i].physical+owned_map[i].pages*PAGE){ram=1;break;}
 if(!ram||!mapped_leaf(address)||!mapped_leaf(address+7))return 0;
 memcpy(out,(const void*)(uintptr_t)address,8);return 1;
}

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
static uint64_t irq_save(void){uint64_t f;__asm__ volatile("pushfq;popq %0;cli":"=r"(f)::"memory");return f;}
static void irq_restore(uint64_t f){if(f&512)__asm__ volatile("sti":::"memory");}
static uint64_t *new_table(void){
 if(table_next>=table_end)panic("page-table arena exhausted");
 uint64_t *p=(void*)table_next;table_next+=PAGE;memset(p,0,PAGE);return p;
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
 owned_count=b->map_size/b->map_stride;
 memcpy(owned_map,map,owned_count*sizeof(*map));
 /* Reserve handoff/map prefix; table arena was explicitly allocated by UEFI,
  * never guessed free from a low-address hole. Loader memory stays owned. */
 table_next=b->arena_start+256*1024;table_end=b->arena_start+b->arena_size;
 root=new_table();
 uint64_t fb_start=b->framebuffer.base&~4095ULL;
 uint64_t fb_end=(b->framebuffer.base+b->framebuffer.size+4095)&~4095ULL;
 if(overlaps(fb_start,fb_end,b->kernel_start,b->kernel_end)||overlaps(fb_start,fb_end,b->arena_start,table_end))panic("framebuffer overlaps owned RAM");
 for(size_t i=0;i<owned_count;i++)if(boot_memory_usable(&map[i])){
  uintptr_t start=map[i].physical,end=start+map[i].pages*PAGE;
  if(!start)start=PAGE; /* Never allocate or map address zero. */
  if(start==end)continue;
  if(overlaps(start,end,b->kernel_start,b->kernel_end)||overlaps(start,end,b->arena_start,table_end)||overlaps(start,end,fb_start,fb_end))panic("firmware ownership conflict");
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
 map_range(fb_start,fb_end,3|NX|0x18);
 if(!free_pages)panic("no conventional RAM available");
 /* Firmware may leave global translations/PAT settings behind. Disable global
  * translations, flush caches while changing PAT, then use our UC slot 3.
  * No AP runs; no runtime firmware calls are permitted after this point. */
 uint64_t cr0,cr4;__asm__ volatile("mov %%cr0,%0":"=r"(cr0));
 __asm__ volatile("mov %%cr4,%0":"=r"(cr4));
 uint64_t cache_off=(cr0|(1ULL<<30))&~(1ULL<<29);
 __asm__ volatile("mov %0,%%cr0; wbinvd"::"r"(cache_off):"memory");
 __asm__ volatile("wrmsr"::"c"(0x277),"a"(0x00070406u),"d"(0x00070406u):"memory");
 cr4&=~(1ULL<<7);__asm__ volatile("mov %0,%%cr4"::"r"(cr4):"memory");
 __asm__ volatile("mov %0,%%cr3"::"r"((uintptr_t)root):"memory");
 cr0=(cr0|(1ULL<<16))&~((1ULL<<30)|(1ULL<<29));
 __asm__ volatile("wbinvd; mov %0,%%cr0"::"r"(cr0):"memory");
 initialized=1;
}
uintptr_t page_allocate(void){
 uint64_t f=irq_save();
 if(!initialized)panic("allocation before memory ownership established");
 if(!range_count){irq_restore(f);return 0;}
 uintptr_t p=free_ranges[0].start;free_ranges[0].start+=PAGE;free_pages--;
 if(free_ranges[0].start==free_ranges[0].end){for(size_t i=1;i<range_count;i++)free_ranges[i-1]=free_ranges[i];range_count--;}
 memset((void*)p,0,PAGE);irq_restore(f);return p;
}
void page_release(uintptr_t p){
 uint64_t f=irq_save();
 if(!initialized||!p||(p&(PAGE-1))||!boot_range_usable(owned_map,owned_count,p,PAGE))panic("release outside owned conventional RAM");
 size_t i=0;while(i<range_count&&free_ranges[i].end<=p)i++;
 if(i<range_count&&free_ranges[i].start<p+PAGE)panic("double page release");
 if(i&&free_ranges[i-1].end==p){
  free_ranges[i-1].end+=PAGE;
  if(i<range_count&&free_ranges[i].start==p+PAGE){free_ranges[i-1].end=free_ranges[i].end;for(size_t j=i+1;j<range_count;j++)free_ranges[j-1]=free_ranges[j];range_count--;}
 }else if(i<range_count&&free_ranges[i].start==p+PAGE)free_ranges[i].start=p;
 else{
  if(range_count==EXTENTS)panic("free-range fragmentation capacity exceeded");
  for(size_t j=range_count;j>i;j--)free_ranges[j]=free_ranges[j-1];
  free_ranges[i]=(struct extent){p,p+PAGE};range_count++;
 }
 free_pages++;irq_restore(f);
}
uint64_t memory_free_pages(void){return free_pages;}

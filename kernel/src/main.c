#include "kernel.h"
#include "gpu_abi.h"
extern uint64_t cpu_tsc_hz;
extern void desktop_start(const struct boot_framebuffer *);
uint64_t platform_rsdp;
static struct boot_handoff boot;
const struct boot_handoff *kernel_boot_handoff(void){return &boot;}
static struct efi_memory memory_map[BOOT_MAP_MAX];
static uint64_t ticks(void){uint32_t a,d;__asm__ volatile("lfence; rdtsc":"=a"(a),"=d"(d)::"memory");return ((uint64_t)d<<32)|a;}
static int channel(uint32_t m){if(!m)return 0;while(!(m&1))m>>=1;return !(m&(m+1))&&m<=1023;}
static int owned(uint64_t start,uint64_t end,uint32_t type,size_t n){
 for(size_t i=0;i<n;i++)if(memory_map[i].type==type&&start>=memory_map[i].physical&&end>=start&&end<=memory_map[i].physical+memory_map[i].pages*4096)return 1;
 return 0;
}
static int rsdp_valid(uint64_t address,size_t count){
 if(!address||address>PHYSICAL_LIMIT-36)return 0;
 uint64_t available=0;
 for(size_t i=0;i<count;i++)if((memory_map[i].type==9||memory_map[i].type==10)&&address>=memory_map[i].physical&&address<memory_map[i].physical+memory_map[i].pages*4096)available=memory_map[i].physical+memory_map[i].pages*4096-address;
 if(available<36)return 0;
 const uint8_t *p=(void*)(uintptr_t)address;
 const char signature[]="RSD PTR ";for(unsigned i=0;i<8;i++)if(p[i]!=(uint8_t)signature[i])return 0;
 uint8_t sum=0;for(unsigned i=0;i<20;i++)sum+=p[i];if(sum||p[15]<2)return 0;
 uint32_t length;memcpy(&length,p+20,4);if(length<36||length>4096||length>available)return 0;
 sum=0;for(unsigned i=0;i<length;i++)sum+=p[i];return !sum;
}
void kernel_main(const struct boot_handoff *incoming){
 interrupts_init(); /* Fault-safe stacks before dereferencing the handoff. */
 /* The loader owns the initial mapping and passes a live allocated descriptor.
  * No physical fixed address, firmware pointer narrowing or BIOS data remains. */
 if(!incoming||(uintptr_t)incoming>=PHYSICAL_LIMIT||((uintptr_t)incoming&4095))panic("invalid UEFI handoff pointer");
 memcpy(&boot,incoming,sizeof(boot));
 if(boot.magic!=BOOT_MAGIC||boot.version!=BOOT_VERSION||boot.size!=sizeof(boot)||boot.reserved||boot.arena_start!=(uintptr_t)incoming||boot.arena_size!=BOOT_ARENA_SIZE||boot.arena_start>PHYSICAL_LIMIT-boot.arena_size||boot.kernel_start!=(uintptr_t)_kernel_start||boot.kernel_end!=(uintptr_t)_kernel_end||boot.kernel_end<boot.kernel_start||boot.kernel_end-boot.kernel_start>KERNEL_SPAN_LIMIT||boot.map_address!=boot.arena_start+4096||boot.map_stride<40||boot.map_stride>4096||!boot.map_size||boot.map_size>128*1024||boot.map_size%boot.map_stride||boot.map_size/boot.map_stride>BOOT_MAP_MAX||boot.map_version!=1||(boot.module_state>3)||boot.module_name[15]||((boot.module_state==1)&&(!boot.module_bytes||boot.module_bytes>GPU_MODULE_REGION||boot.module_address!=GPU_MODULE_AREA(boot.arena_start)||boot.module_bytes<SCOS_GPU_MODULE_HEADER_SIZE))||(boot.module_index_size>BOOT_INDEX_MAX)||((boot.module_state==1)&&(boot.index_address!=BOOT_INDEX_ADDRESS(boot.arena_start)||!boot.module_index_size))||boot.tsc_hz<1000000||boot.tsc_hz>UINT64_C(100000000000))panic("incompatible UEFI handoff");
 size_t count=boot.map_size/boot.map_stride;
 for(size_t i=0;i<count;i++)memcpy(&memory_map[i],(void*)(uintptr_t)(boot.map_address+i*boot.map_stride),sizeof(memory_map[i]));
 if(!boot_map_valid(memory_map,count)||!owned(boot.kernel_start,boot.kernel_end,1,count)||!owned(boot.arena_start,boot.arena_start+boot.arena_size,2,count))panic("invalid UEFI memory ownership");
 const struct boot_framebuffer *f=&boot.framebuffer;
 if(!f->base||f->base>=PHYSICAL_LIMIT||f->size>PHYSICAL_LIMIT-f->base||f->width<640||f->width>4096||f->height<400||f->height>2160||f->stride<f->width||f->stride>16384||(uint64_t)f->stride*f->height*4>f->size||!channel(f->red)||!channel(f->green)||!channel(f->blue)||(f->red&f->green)||(f->red&f->blue)||(f->green&f->blue))panic("invalid GOP framebuffer");
 /* Validate ownership BEFORE the first framebuffer write, not after console
  * initialization: a corrupt handoff must not paint over RAM or boot tables. */
 for(size_t i=0;i<count;i++)if(memory_map[i].pages&&memory_map[i].type!=0&&memory_map[i].type!=11&&f->base<memory_map[i].physical+memory_map[i].pages*4096&&f->base+f->size>memory_map[i].physical)panic("GOP overlaps firmware-owned RAM");
 console_init(f);
 putstr("SCos native x64 UEFI startup (unnumbered)\n");
 putstr("Firmware boot services exited; no BIOS/CSM path\n");
 putstr("Kernel physical span: ");puthex(boot.kernel_start);putstr(" .. ");puthex(boot.kernel_end);putstr("\n");
 if(boot.rsdp&&!rsdp_valid(boot.rsdp,count)){putstr("Warning: invalid/unreserved ACPI RSDP ignored\n");boot.rsdp=0;}
 putstr("GDT/TSS/256-vector IDT and guarded fault stacks installed\n");
 uint32_t lo,hi;__asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0xc0000080));(void)hi;
 if((lo&0xd00)!=0xd00)panic("long mode/NX not active");
 memory_init(&boot,memory_map);
 putstr("Owned page tables active; RX text, R-NX constants, RW-NX RAM\n");
 putstr("Null/stack guards unmapped; GOP mapped write-combining (PAT)\n");
 putstr("UEFI descriptors: ");puthex(count);putstr(" free conventional pages: ");puthex(memory_free_pages());putstr("\n");
 for(size_t i=0;i<count;i++)if(boot_memory_usable(&memory_map[i])&&memory_map[i].physical+memory_map[i].pages*4096>UINT64_C(0x100000000)){
  putstr("Usable RAM above 4 GiB is mapped and allocatable\n");break;
 }
 if(boot.rsdp){putstr("ACPI RSDP preserved (firmware tables not reclaimed): ");puthex(boot.rsdp);putstr("\n");}
 uint64_t before=memory_free_pages();uintptr_t page=page_allocate();
 if(!page||memory_free_pages()!=before-1)panic("allocator failed");
 for(size_t i=0;i<4096;i++)if(((uint8_t*)page)[i])panic("allocator did not zero page");
 *(volatile uint64_t*)page=UINT64_C(0x53434f533634);
 if(*(volatile uint64_t*)page!=UINT64_C(0x53434f533634))panic("RAM write/read failed");
 page_release(page);if(memory_free_pages()!=before)panic("allocator accounting failed");
 timer_init();putstr("Waiting for real PIT IRQs and native interrupt return...\n");
 uint64_t start=ticks();__asm__ volatile("sti");
 while(timer_ticks<3){if(ticks()-start>boot.tsc_hz*3)panic("PIT IRQ timeout; platform IRQ routing unsupported");__asm__ volatile("pause");}
 putstr("Startup complete: real timer interrupts verified\n");
 putstr("Starting converted SCos apps and native platform drivers.\n");
 cpu_tsc_hz=boot.tsc_hz;
 platform_rsdp=boot.rsdp;
 desktop_start(&boot.framebuffer);
 for(;;)__asm__ volatile("hlt");
}

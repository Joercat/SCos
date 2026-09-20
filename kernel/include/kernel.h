#ifndef SCOS_KERNEL_H
#define SCOS_KERNEL_H
#include "boot.h"
_Static_assert(sizeof(void*)==8 && sizeof(uintptr_t)==8, "x86-64 kernel only");
static inline void out8(uint16_t p,uint8_t v) { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t in8(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint64_t read_cr3(void) { uint64_t v; __asm__ volatile("mov %%cr3,%0":"=r"(v)); return v; }
static inline uint64_t read_cr2(void) { uint64_t v; __asm__ volatile("mov %%cr2,%0":"=r"(v)); return v; }
void *memset(void *,int,size_t);
void *memcpy(void *,const void *,size_t);
void console_init(const struct boot_framebuffer *);
void console_fault_begin(void);
void putstr(const char *);
void puthex(uint64_t);
_Noreturn void panic(const char *);
struct interrupt_frame {
    uint64_t r15,r14,r13,r12,r11,r10,r9,r8,rdi,rsi,rbp,rdx,rcx,rbx,rax;
    uint64_t vector,error,rip,cs,rflags,rsp,ss;
};
_Static_assert(offsetof(struct interrupt_frame,vector)==120,"ISR ABI");
_Static_assert(sizeof(struct interrupt_frame)==176,"ISR frame size");
void interrupts_init(void);
void interrupt_dispatch(struct interrupt_frame *);
void timer_init(void);
extern volatile uint64_t timer_ticks;
void memory_init(const struct boot_handoff *,const struct efi_memory *);
uintptr_t pages_allocate(size_t count);
void pages_release(uintptr_t,size_t);
uint64_t memory_reserved_pages(void);
uintptr_t page_allocate(void);
void page_release(uintptr_t);
uint64_t memory_free_pages(void);
extern char _relro_start[],_relro_end[];
extern char _kernel_start[],_kernel_end[],_text_start[],_text_end[],_rodata_start[],_rodata_end[],_file_end[];
extern char stack_guard[],stack_top[],df_guard[],df_top[],nmi_guard[],nmi_top[],mc_guard[],mc_top[];
#endif

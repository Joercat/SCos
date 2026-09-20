#include "kernel.h"
struct table_pointer {uint16_t limit;uint64_t address;} __attribute__((packed));
struct gate {uint16_t low,selector;uint8_t ist,flags;uint16_t middle;uint32_t high,reserved;} __attribute__((packed));
struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0,rsp1,rsp2,reserved1,ist[7],reserved2;
    uint16_t reserved3,iomap;
} __attribute__((packed));
_Static_assert(sizeof(struct gate)==16 && sizeof(struct table_pointer)==10,"IDT ABI");
_Static_assert(sizeof(struct tss64)==104,"TSS ABI");
static struct gate idt[256] __attribute__((aligned(16)));
static struct tss64 tss;
static uint64_t gdt[5] __attribute__((aligned(16)));
extern const uintptr_t isr_stubs[256];
extern void load_gdt(const struct table_pointer *);
volatile uint64_t timer_ticks;
void interrupts_init(void) {
    memset(&tss,0,sizeof(tss));
    tss.rsp0=(uintptr_t)stack_top;
    tss.ist[0]=(uintptr_t)df_top;tss.ist[1]=(uintptr_t)nmi_top;tss.ist[2]=(uintptr_t)mc_top;
    tss.iomap=sizeof(tss);
    gdt[0]=0;gdt[1]=UINT64_C(0x00af9b000000ffff);gdt[2]=UINT64_C(0x00cf93000000ffff);
    uint64_t base=(uintptr_t)&tss, limit=sizeof(tss)-1;
    gdt[3]=limit|((base&0xffffff)<<16)|(UINT64_C(0x89)<<40)|(((base>>24)&255)<<56);
    gdt[4]=base>>32;
    struct table_pointer gdtr={sizeof(gdt)-1,(uintptr_t)gdt};
    load_gdt(&gdtr);
    /* No LDT is supported. Discard any inherited hidden descriptor state,
     * rather than allowing TI selectors to refer to firmware-era tables. */
    __asm__ volatile("lldt %0"::"r"((uint16_t)0):"memory");
    __asm__ volatile("ltr %0"::"r"((uint16_t)24):"memory");
    for(size_t i=0;i<256;i++) {
        uintptr_t a=isr_stubs[i];
        idt[i]=(struct gate){a&65535,8,i==8?1:i==2?2:i==18?3:0,0x8e,(a>>16)&65535,a>>32,0};
    }
    struct table_pointer idtr={sizeof(idt)-1,(uintptr_t)idt};
    __asm__ volatile("lidt %0"::"m"(idtr):"memory");
    out8(0x70,0); /* restore NMI only after safe exception stacks exist */
}
static void wait_io(void) {out8(0x80,0);}
void timer_init(void) {
    /* PIC remap with every source masked until the sole installed IRQ0 driver. */
    out8(0x21,255);out8(0xa1,255);
    out8(0x20,0x11);wait_io();out8(0xa0,0x11);wait_io();
    out8(0x21,32);wait_io();out8(0xa1,40);wait_io();
    out8(0x21,4);wait_io();out8(0xa1,2);wait_io();
    out8(0x21,1);wait_io();out8(0xa1,1);wait_io();
    out8(0x21,0xfe);out8(0xa1,255);
    uint16_t divisor=11932;
    out8(0x43,0x36);out8(0x40,(uint8_t)divisor);out8(0x40,(uint8_t)(divisor>>8));
}
void interrupt_dispatch(struct interrupt_frame *f) {
    if(f->vector==32) {timer_ticks++;out8(0x20,0x20);return;}
    if(f->vector==39) {out8(0x20,0x0b);if(!(in8(0x20)&128))return;}
    if(f->vector==47) {out8(0xa0,0x0b);if(!(in8(0xa0)&128)){out8(0x20,0x20);return;}}
    /* Snapshot fault addresses before console I/O. Never dereference the
     * interrupted RIP/RSP: they may themselves be unmapped/noncanonical. */
    uint64_t fault_address=read_cr2(), page_root=read_cr3();
    putstr("\nEXCEPTION/UNEXPECTED INTERRUPT vector=");puthex(f->vector);
    putstr(" error=");puthex(f->error);putstr("\nRIP=");puthex(f->rip);
    putstr(" RSP=");puthex(f->rsp);putstr(" RFLAGS=");puthex(f->rflags);
    putstr("\nCR2=");puthex(fault_address);putstr(" CR3=");puthex(page_root);
    putstr("\nRAX=");puthex(f->rax);putstr(" RBX=");puthex(f->rbx);
    putstr(" RCX=");puthex(f->rcx);putstr(" RDX=");puthex(f->rdx);
    panic("unhandled exception/interrupt");
}

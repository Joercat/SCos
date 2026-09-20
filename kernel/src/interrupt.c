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
/* Original rate protection, with warnings deferred out of the IRQ graph. */
static uint32_t irq_rate[16];
static uint16_t storm_pending,unhandled_pending;
static void irq_storm_sweep(void){
    if(timer_ticks%100)return;
    for(unsigned i=1;i<16;i++){
        if(irq_rate[i]>=1500){pic_set_mask((uint8_t)i);storm_pending|=(uint16_t)(1u<<i);}
        irq_rate[i]=0;
    }
}
extern void klog(const char *,...);
extern void err_notify(const char *,const char *,const char *const *,int);
void interrupt_poll(void){
    uint64_t flags;__asm__ volatile("pushfq;popq %0;cli":"=r"(flags)::"memory");
    uint16_t storms=storm_pending,unknown=unhandled_pending;storm_pending=unhandled_pending=0;
    if(flags&512)__asm__ volatile("sti":::"memory");
    for(unsigned i=1;i<16;i++)if((storms|unknown)&(1u<<i))klog("interrupt: IRQ %u masked (%s)",i,(storms&(1u<<i))?"storm":"no handler");
    if(storms||unknown)err_notify("interrupt controller","IRQ masked; see klog for line and cause",NULL,0);
}
static void (*irq_handlers[16])(struct interrupt_frame *);
extern void cpu_meter_tick(void);
extern void cpu_meter_irq_enter(void);
void irq_install(uint8_t irq,void (*handler)(struct interrupt_frame *)){
    if(irq>=16||irq==0)panic("invalid app-platform IRQ registration");
    irq_handlers[irq]=handler;
}
void pic_clear_mask(uint8_t irq){if(irq<16){uint16_t p=irq<8?0x21:0xa1;out8(p,in8(p)&~(1u<<(irq&7)));}}
void pic_set_mask(uint8_t irq){if(irq<16){uint16_t p=irq<8?0x21:0xa1;out8(p,in8(p)|(1u<<(irq&7)));}}

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
    if(f->vector>=32&&f->vector<48)cpu_meter_irq_enter();
    if(f->vector==39) {out8(0x20,0x0b);if(!(in8(0x20)&128))return;}
    if(f->vector==47) {out8(0xa0,0x0b);if(!(in8(0xa0)&128)){out8(0x20,0x20);return;}}
    if(f->vector>32&&f->vector<48&&irq_rate[f->vector-32]!=UINT32_MAX)irq_rate[f->vector-32]++;
    if(f->vector==32) {timer_ticks++;irq_storm_sweep();cpu_meter_tick();out8(0x20,0x20);return;}
    if(f->vector>32&&f->vector<48&&irq_handlers[f->vector-32]){
        irq_handlers[f->vector-32](f);
        if(f->vector>=40)out8(0xa0,0x20);
        out8(0x20,0x20);return;
    }
    if(f->vector>32&&f->vector<48){
        uint8_t irq=(uint8_t)(f->vector-32);pic_set_mask(irq);unhandled_pending|=(uint16_t)(1u<<irq);
        if(irq>=8)out8(0xa0,0x20);
        out8(0x20,0x20);return;
    }
    /* Snapshot fault addresses before console I/O. Stack inspection below
     * requires mapped RAM; RIP is never dereferenced. */
    uint64_t fault_address=read_cr2(), page_root=read_cr3();
    console_fault_begin();
    putstr("\nEXCEPTION/UNEXPECTED INTERRUPT vector=");puthex(f->vector);
    putstr(" error=");puthex(f->error);putstr("\nRIP=");puthex(f->rip);
    putstr(" RSP=");puthex(f->rsp);putstr(" RFLAGS=");puthex(f->rflags);
    putstr("\nCR2=");puthex(fault_address);putstr(" CR3=");puthex(page_root);
    putstr("\nRAX=");puthex(f->rax);putstr(" RBX=");puthex(f->rbx);
    putstr(" RCX=");puthex(f->rcx);putstr(" RDX=");puthex(f->rdx);
    putstr("\nRSI=");puthex(f->rsi);putstr(" RDI=");puthex(f->rdi);putstr(" RBP=");puthex(f->rbp);
    putstr("\nR8 =");puthex(f->r8);putstr(" R9 =");puthex(f->r9);putstr(" R10=");puthex(f->r10);putstr(" R11=");puthex(f->r11);
    putstr("\nR12=");puthex(f->r12);putstr(" R13=");puthex(f->r13);putstr(" R14=");puthex(f->r14);putstr(" R15=");puthex(f->r15);
    putstr("\nCS=");puthex(f->cs);putstr(" SS=");puthex(f->ss);
    putstr("\nStack at RSP (mapped RAM only):");
    for(unsigned i=0;i<16;i++){
        uint64_t value,offset=(uint64_t)i*8;
        if(f->rsp>UINT64_MAX-offset||!memory_read_u64(f->rsp+offset,&value)){putstr(" <unavailable>");break;}
        if(!(i%4))putstr("\n");else putstr(" ");puthex(value);
    }
    panic("unhandled exception/interrupt");
}

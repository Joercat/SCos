/* SCos native - IDT + PIC + interrupt dispatch */
#include "scos.h"

struct idt_entry {
    u16 base_lo;
    u16 sel;
    u8  always0;
    u8  flags;
    u16 base_hi;
} __attribute__((packed));

struct idt_ptr {
    u16 limit;
    u32 base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

extern u32 isr_stub_table[];

static irq_handler_t irq_handlers[16];

static void idt_set_gate(u8 n, u32 handler, u16 sel, u8 flags)
{
    idt[n].base_lo  = handler & 0xFFFF;
    idt[n].base_hi  = (handler >> 16) & 0xFFFF;
    idt[n].sel      = sel;
    idt[n].always0  = 0;
    idt[n].flags    = flags;
}

void idt_init(void)
{
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (u32)&idt;
    memset(&idt, 0, sizeof(idt));

    for (int i = 0; i < 48; i++)
        idt_set_gate(i, isr_stub_table[i], 0x08, 0x8E);

    __asm__ volatile("lidt %0" : : "m"(idtp));
}

void irq_install(u8 irq, irq_handler_t h)
{
    if (irq < 16) irq_handlers[irq] = h;
}

/* ------------------------- 8259 PIC ------------------------- */
#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

void pic_remap(void)
{
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, 0x20); io_wait();   /* IRQ 0-7  -> 32-39 */
    outb(PIC2_DATA, 0x28); io_wait();   /* IRQ 8-15 -> 40-47 */
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();
    outb(PIC1_DATA, 0xFC); io_wait();   /* mask all except cascade (IRQ2) */
    outb(PIC2_DATA, 0xFF); io_wait();
}

void pic_clear_mask(u8 irq)
{
    u16 port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    outb(port, inb(port) & ~(1 << irq));
}

void pic_set_mask(u8 irq)
{
    u16 port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    outb(port, inb(port) | (1 << irq));
}

void pic_send_eoi(u8 irq)
{
    if (irq >= 8) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

/* ------------------------- dispatch ------------------------- */
static const char *exc_names[] = {
    "Division by zero", "Debug", "NMI", "Breakpoint", "Overflow",
    "Bound range", "Invalid opcode", "Coprocessor", "Double fault",
    "Coproc segment", "Invalid TSS", "Segment not present", "Stack fault",
    "General protection", "Page fault", "Reserved", "x87 FPU",
    "Alignment check", "Machine check", "SIMD"
};

void isr_handler(struct regs *r)
{
    if (r->int_no < 32) {
        klog_raw("!! exception entered");
        const char *name = r->int_no < 20 ? exc_names[r->int_no] : "Reserved";
        klog("!! EXCEPTION %d (%s) eip-err=%x", r->int_no, name, r->err_code);
        irq_disable();
        wm_fatal_screen("KERNEL FAULT", name);
        for (;;) cpu_hlt();
    }
    u8 irq = (u8)(r->int_no - 32);
    if (irq_handlers[irq]) irq_handlers[irq](r);
    pic_send_eoi(irq);
}

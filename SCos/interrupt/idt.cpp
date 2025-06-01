
#include "idt.hpp"
#include "../debug/serial.hpp"

static idt_entry idt[256];
static idt_ptr idtp;

extern "C" void idt_load(uint32_t);
extern "C" void keyboard_interrupt_wrapper();
extern "C" void keyboard_handler();

void set_idt_gate(int n, uint32_t handler) {
    idt[n].offset_low = handler & 0xFFFF;
    idt[n].selector = 0x08;  // Code segment selector
    idt[n].zero = 0;
    idt[n].type_attr = 0x8E; // Present, DPL=0, 32-bit interrupt gate
    idt[n].offset_high = (handler >> 16) & 0xFFFF;
}

void init_pic() {
    // Remap PIC interrupts to avoid conflicts with CPU exceptions
    // Master PIC: IRQ 0-7 -> INT 32-39
    // Slave PIC: IRQ 8-15 -> INT 40-47
    
    // Start initialization sequence
    outb(0x20, 0x11); // Master PIC command
    outb(0xA0, 0x11); // Slave PIC command
    
    // Set vector offsets
    outb(0x21, 0x20); // Master PIC vector offset (32)
    outb(0xA1, 0x28); // Slave PIC vector offset (40)
    
    // Set up cascading
    outb(0x21, 0x04); // Tell master PIC about slave at IRQ2
    outb(0xA1, 0x02); // Tell slave PIC its cascade identity
    
    // Set mode to 8086
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    
    // Mask all interrupts initially
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

void enable_irq(uint8_t irq) {
    uint16_t port;
    uint8_t value;
    
    if (irq < 8) {
        port = 0x21; // Master PIC
    } else {
        port = 0xA1; // Slave PIC
        irq -= 8;
    }
    
    value = inb(port) & ~(1 << irq);
    outb(port, value);
}

void disable_irq(uint8_t irq) {
    uint16_t port;
    uint8_t value;
    
    if (irq < 8) {
        port = 0x21; // Master PIC
    } else {
        port = 0xA1; // Slave PIC
        irq -= 8;
    }
    
    value = inb(port) | (1 << irq);
    outb(port, value);
}

bool init_idt() {
    serial_printf("IDT initialization started\n");
    
    // Set up IDT pointer
    idtp.limit = (sizeof(idt_entry) * 256) - 1;
    idtp.base = (uint32_t)&idt;
    
    // Clear all IDT entries
    for (int i = 0; i < 256; i++) {
        idt[i].offset_low = 0;
        idt[i].selector = 0;
        idt[i].zero = 0;
        idt[i].type_attr = 0;
        idt[i].offset_high = 0;
    }
    
    // Initialize PIC
    init_pic();
    
    // Set up keyboard interrupt (IRQ1 -> INT 33)
    set_idt_gate(33, (uint32_t)keyboard_interrupt_wrapper);
    
    // Load the IDT
    idt_load((uint32_t)&idtp);
    
    // Enable keyboard IRQ (IRQ1)
    enable_irq(1);
    
    serial_printf("IDT initialization completed\n");
    return true;
}

#include "idt.hpp"
#include "../debug/serial.hpp"

static idt_entry idt[256];
static idt_ptr idtp;

extern "C" void idt_load(uint32_t);
extern "C" void keyboard_interrupt_wrapper();
extern "C" void keyboard_handler();

void set_idt_gate(int n, uint32_t handler) {
    idt[n].offset_low = handler & 0xFFFF;
    idt[n].selector = 0x08;
    idt[n].zero = 0;
    idt[n].type_attr = 0x8E;
    idt[n].offset_high = (handler >> 16) & 0xFFFF;
}

void init_pic() {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

void enable_irq(uint8_t irq) {
    uint16_t port;
    uint8_t value;
    
    if (irq < 8) {
        port = 0x21;
    } else {
        port = 0xA1;
        irq -= 8;
    }
    
    value = inb(port) & ~(1 << irq);
    outb(port, value);
}

void disable_irq(uint8_t irq) {
    uint16_t port;
    uint8_t value;
    
    if (irq < 8) {
        port = 0x21;
    } else {
        port = 0xA1;
        irq -= 8;
    }
    
    value = inb(port) | (1 << irq);
    outb(port, value);
}

bool init_idt() {
    serial_printf("IDT initialization started\n");
    
    idtp.limit = (sizeof(idt_entry) * 256) - 1;
    idtp.base = (uint32_t)&idt;
    
    for (int i = 0; i < 256; i++) {
        idt[i].offset_low = 0;
        idt[i].selector = 0;
        idt[i].zero = 0;
        idt[i].type_attr = 0;
        idt[i].offset_high = 0;
    }
    
    init_pic();
    
    set_idt_gate(33, (uint32_t)keyboard_interrupt_wrapper);
    
    idt_load((uint32_t)&idtp);
    
    enable_irq(1);
    
    serial_printf("IDT initialization completed\n");
    return true;
}

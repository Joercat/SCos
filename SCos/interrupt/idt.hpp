#pragma once
#include <stdint.h>

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t zero;
    uint8_t type_attr;
    uint16_t offset_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

// Core IDT functions
bool init_idt();
void set_idt_gate(int n, uint32_t handler);

// PIC functions
void init_pic();
void enable_irq(uint8_t irq);
void disable_irq(uint8_t irq);

// I/O helper functions
static inline uint8_t inb(uint16_t port) {
    uint8_t result;
    asm volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static inline void outb(uint16_t port, uint8_t data) {
    asm volatile("outb %0, %1" : : "a"(data), "Nd"(port));
}

// External assembly functions
extern "C" void keyboard_interrupt_wrapper();
extern "C" void keyboard_handler();
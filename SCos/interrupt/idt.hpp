#ifndef IDT_HPP
#define IDT_HPP

#pragma once
#include <stdint.h>
#include "../include/io_utils.h"

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


bool init_idt();
void set_idt_gate(int n, uint32_t handler);


void init_pic();
void enable_irq(uint8_t irq);
void disable_irq(uint8_t irq);




extern "C" void keyboard_interrupt_wrapper();
extern "C" void keyboard_handler();

#endif 

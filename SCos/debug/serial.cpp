#include "serial.hpp"
#include <stdint.h>
#include <stdarg.h>

#define SERIAL_PORT 0x3f8

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

bool init_serial() {
    outb(SERIAL_PORT + 1, 0x00);
    outb(SERIAL_PORT + 3, 0x80);
    outb(SERIAL_PORT + 0, 0x03);
    outb(SERIAL_PORT + 1, 0x00);
    outb(SERIAL_PORT + 3, 0x03);
    outb(SERIAL_PORT + 2, 0xC7);
    outb(SERIAL_PORT + 4, 0x0B);
    return true;
}

static bool is_transmit_empty() {
    return inb(SERIAL_PORT + 5) & 0x20;
}

static void write_serial(char c) {
    while (!is_transmit_empty());
    outb(SERIAL_PORT, c);
}

void serial_write(const char* data) {
    while (*data) {
        write_serial(*data++);
    }
}

void serial_printf(const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    
    int i = 0;
    while (*format && i < 255) {
        if (*format == '%' && *(format + 1) == 's') {
            const char* str = va_arg(args, const char*);
            while (*str && i < 255) {
                buffer[i++] = *str++;
            }
            format += 2;
        } else {
            buffer[i++] = *format++;
        }
    }
    buffer[i] = '\0';
    
    va_end(args);
    serial_write(buffer);
}
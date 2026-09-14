/* SCos native - serial console logger (COM1) */
#include "scos.h"

#define COM1 0x3f8

static void serial_putc(char c)
{
    while ((inb(COM1 + 5) & 0x20) == 0) ;
    outb(COM1, c);
}

static void serial_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

static void put_uint(u32 v, int base, int width, char pad)
{
    char buf[16];
    int i = 0;
    static const char digits[] = "0123456789abcdef";
    if (v == 0) buf[i++] = '0';
    while (v) { buf[i++] = digits[v % base]; v /= base; }
    while (i < width) buf[i++] = pad;
    while (i--) serial_putc(buf[i]);
}

static void put_int(i32 v, int width)
{
    if (v < 0) { serial_putc('-'); v = -v; }
    put_uint((u32)v, 10, width, ' ');
}

void klog_raw(const char *s)
{
    serial_puts(s);
    serial_putc('\r');
    serial_putc('\n');
}

void klog(const char *fmt, ...)
{
    /* minimal varargs via builtin */
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { serial_putc(*fmt); continue; }
        fmt++;
        int width = 0;
        char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        switch (*fmt) {
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            serial_puts(s ? s : "(null)");
            break;
        }
        case 'c':
            serial_putc((char)__builtin_va_arg(ap, int));
            break;
        case 'd':
            put_int(__builtin_va_arg(ap, i32), width);
            break;
        case 'u':
            put_uint(__builtin_va_arg(ap, u32), 10, width, pad);
            break;
        case 'x':
            put_uint(__builtin_va_arg(ap, u32), 16, width, pad);
            break;
        case '%':
            serial_putc('%');
            break;
        default:
            serial_putc('%');
            serial_putc(*fmt);
            break;
        }
    }
    __builtin_va_end(ap);
    serial_putc('\r');
    serial_putc('\n');
}

/* SCos native - PIT timer (IRQ0) and monotonic time */
#include "scos.h"

volatile u64 tick_count;
static u32 tick_hz = 100;

static void pit_irq(struct regs *r)
{
    (void)r;
    tick_count++;
    cpu_meter_tick();
}

void pit_init(u32 hz)
{
    tick_hz = hz;
    u32 divisor = 1193182 / hz;
    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
    irq_install(0, pit_irq);
    pic_clear_mask(0);
}

u32 uptime_ms(void)
{
    return (u32)(tick_count * 1000 / tick_hz);
}

void sleep_ms(u32 ms)
{
    u64 target = tick_count + ((u64)ms * tick_hz + 999) / 1000;
    while (tick_count < target) cpu_hlt();
}

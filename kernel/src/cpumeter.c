/*
 * SCos native - real CPU metering.
 *
 * CPU speed is measured by counting TSC ticks across a known PIT interval.
 * CPU usage is measured by marking the TSC right before the idle loop halts
 * the processor; the PIT interrupt adds the halted span to an idle
 * accumulator, so usage = 100% - idle% over each one-second window.
 * Nothing here is simulated: on a machine without RDTSC the meter simply
 * reports 0 MHz / 0% instead of inventing numbers.
 */
#include "scos.h"

static u32 tsc_per_ms = 0;
static u64 idle_mark = 0;
static u64 idle_acc = 0;
static u32 tick_acc = 0;
static u32 usage_pct = 0;
static int have_tsc = 0;

u64 rdtsc(void)
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

void cpu_meter_init(void)
{
    /* detect TSC: CPUID.1 EDX bit 4 */
    u32 ax, bx, cx, dx;
    __asm__ volatile("movl $1, %%eax; cpuid"
                     : "=a"(ax), "=b"(bx), "=c"(cx), "=d"(dx));
    if (!(dx & (1 << 4))) { have_tsc = 0; return; }
    have_tsc = 1;
    u64 a = rdtsc();
    sleep_ms(50);
    u64 d = rdtsc() - a;
    tsc_per_ms = (u32)(d / 50);
    if (!tsc_per_ms) tsc_per_ms = 1;
}

u32 cpu_mhz(void)
{
    if (!have_tsc) return 0;
    return tsc_per_ms / 1000;
}

void cpu_idle_begin(void)
{
    if (have_tsc) idle_mark = rdtsc();
}

void cpu_meter_tick(void)                 /* from the PIT ISR, 100 Hz */
{
    if (!have_tsc) return;
    if (idle_mark) {
        u64 m = idle_mark;
        idle_mark = 0;
        idle_acc += rdtsc() - m;
    }
    if (++tick_acc >= 100) {
        u64 per_sec = (u64)tsc_per_ms * 1000;
        u64 idle = idle_acc;
        if (idle > per_sec) idle = per_sec;
        usage_pct = (u32)(100 - (u32)(idle * 100 / per_sec));
        idle_acc = 0;
        tick_acc = 0;
    }
}

u32 cpu_usage_pct(void)
{
    return have_tsc ? usage_pct : 0;
}

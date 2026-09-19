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

u32 cpu_thread_count(void)
{
    u32 ax, bx, cx, dx;
    __asm__ volatile("movl $1, %%eax; cpuid"
                     : "=a"(ax), "=b"(bx), "=c"(cx), "=d"(dx));
    u32 logical = (bx >> 16) & 0xFF;         /* leaf 1 EBX[23:16] */
    return logical ? logical : 1;
}

u32 cpu_core_count(void)
{
    u32 ax, bx, cx, dx;
    __asm__ volatile("xorl %%eax, %%eax; cpuid"
                     : "=a"(ax), "=b"(bx), "=c"(cx), "=d"(dx));
    u32 maxleaf = ax;
    u32 logical = cpu_thread_count();
    if (maxleaf < 4) return logical;
    __asm__ volatile("movl $4, %%eax; xorl %%ecx, %%ecx; cpuid"
                     : "=a"(ax), "=b"(bx), "=c"(cx), "=d"(dx));
    u32 cores = ((ax >> 26) & 0x3F) + 1;     /* leaf 4 EAX[31:26]+1 */
    if (!cores || cores > logical) return logical;
    return cores;
}

u32 cpu_mhz(void)
{
    if (!have_tsc) return 0;
    return tsc_per_ms / 1000;
}

extern volatile int wm_in_idle;

void cpu_idle_begin(void)
{
    (void)0;   /* usage is tick-sampled now; kept for call-site stability */
}

void cpu_meter_tick(void)                 /* from the PIT ISR, 100 Hz */
{
    /* Sample the WM idle flag at timer rate: usage = share of ticks the
     * main loop was NOT halted. Works on CPUs whose TSC stops in halt or
     * that lack TSC entirely - both made the old TSC-span math lie. */
    tick_acc++;
    if (wm_in_idle) idle_acc++;
    if (tick_acc >= 100) {
        usage_pct = (u32)((tick_acc - (u32)idle_acc) * 100u / tick_acc);
        tick_acc = 0;
        idle_acc = 0;
    }
}

u32 cpu_usage_pct(void)
{
    return usage_pct;
}

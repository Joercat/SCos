/*
 * SCos native - real CPU metering.
 *
 * CPU speed is measured by counting TSC ticks across a known PIT interval.
 * CPU usage is measured by marking the TSC right before the idle loop halts
 * the processor; the PIT interrupt adds the halted span to an idle
 * accumulator, so usage = 100% - idle% over each one-second window.
 * The startup-calibrated TSC rate is a reference-clock rate, not instantaneous
 * core frequency. Only the bootstrap CPU runs; no SMP usage claim is made.
 */
#include "scos.h"

static volatile u32 usage_pct;
static int have_tsc;
static volatile u64 idle_start,idle_total;
static u64 last_idle,last_sample;
static volatile int idle_active;

u64 rdtsc(void)
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

void cpu_meter_init(void)
{
    have_tsc=cpu_tsc_hz!=0;
    if(have_tsc)last_sample=rdtsc();
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

u32 cpu_mhz(void){return (u32)(cpu_tsc_hz/1000000);}
/* Called with IRQs disabled immediately before the atomic STI/HLT sequence.
 * Every hardware IRQ ends that idle interval before doing device work. */
void cpu_idle_begin(void){if(have_tsc){idle_start=rdtsc();idle_active=1;}}
void cpu_meter_irq_enter(void){
    if(idle_active){idle_total+=rdtsc()-idle_start;idle_active=0;}
}
u64 cpu_busy_cycles(void){return rdtsc()-idle_total;}
void cpu_meter_tick(void){
    if(!have_tsc)return;
    u64 now=rdtsc(),elapsed=now-last_sample;
    if(elapsed>=cpu_tsc_hz){
        u64 idle=idle_total-last_idle;
        if(idle>elapsed)idle=elapsed;
        usage_pct=(u32)((elapsed-idle)*100/elapsed);
        last_sample=now;last_idle=idle_total;
    }
}
u32 cpu_usage_pct(void){return usage_pct;}

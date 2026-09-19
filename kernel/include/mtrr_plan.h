#ifndef SCOS_MTRR_PLAN_H
#define SCOS_MTRR_PLAN_H
#include "scos.h"
/* Plan an aligned power-of-two PREFIX wholly inside the framebuffer.
 * Never round outward across neighboring device registers. */
static int mtrr_wc_plan(u32 base, u32 bytes, unsigned physical_bits,
                       u64 *base_reg, u64 *mask_reg, u32 *covered)
{
    if ((base & 4095) || bytes < 4096 || physical_bits < 32 || physical_bits > 52)
        return 0;
    u64 size=4096;
    while (size*2 <= bytes && !((u64)base & (size*2-1))) size*=2;
    u64 address_mask=((1ull<<physical_bits)-1)&~4095ull;
    *base_reg=(u64)base | 1ull; /* architectural WC=1, WB=6 */
    *mask_reg=(~(size-1)&address_mask)|(1ull<<11);
    *covered=(u32)size;
    return 1;
}
#endif

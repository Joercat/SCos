/* The one place the kernel touches a device register with no driver in between.
 *
 * Two files need this: the security-processor mailbox in gpu_fsp.c, which is a page of a card's BAR0 read
 * and written without any driver in between, and anything else that grows the same need later.  Both have to
 * be able to be run against a model of the register file rather than against the array itself, for the same
 * reason a mailbox is worth modelling at all: the interesting behaviour of these registers is that reading or
 * writing one of them *changes another one* - the FSP's channel offset walks by itself, its queue pointers
 * move when the firmware consumes a packet - and a test that could not reproduce that would be asserting that
 * the driver under test disagrees with a card.
 *
 * So this is a file of two functions and no policy.  The bounds, the counting, the refusal detection and the
 * decision to stop all belong to the caller, where they can be tested together with the logic that needs them;
 * what belongs here is the single line of assembly-free memory access, because that is the one thing a host
 * test cannot provide for itself.
 */

#include "gpu.h"
#include "scos.h"

uint32_t gpu_io_read32(const volatile uint32_t *window, uint64_t within)
{
    return window[within >> 2];
}

void gpu_io_write32(volatile uint32_t *window, uint64_t within, uint32_t value)
{
    window[within >> 2] = value;
}

/* Owner of the generated device tables.
 *
 * `gpu_ids.h` is produced by tools/research/gen_gpu_tables.py from the upstream
 * drivers' own binding tables at a pinned Haiku commit; it declares `static`
 * arrays, so exactly one translation unit may include it.  This is that unit, and
 * the accessors below are what the rest of the kernel sees.  Keeping the ownership
 * here means a regenerated table cannot silently change two files.
 */
#include "scos.h"
#include "gpu.h"
#include "gpu_ids.h"

int gpu_match_family_count(void)
{
    return (int)GPU_MATCH_COUNT;
}

const struct gpu_match *gpu_match_family(int index)
{
    if (index < 0 || index >= (int)GPU_MATCH_COUNT) return 0;
    return &gpu_match_table[index];
}

int gpu_match_id_total(void)
{
    return (int)GPU_ID_TOTAL;
}

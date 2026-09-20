#include "boot.h"
/* Pure validators: no pointer narrowing, and reserved/unknown ranges always win.
 * A contiguous requested span must fit one usable entry; fragmented coverage is
 * deliberately not guessed/coalesced. Disabled and zero-length entries are ignored. */
int boot_map_valid(const struct boot_map_entry *m,size_t n) {
    if(!m || !n || n>BOOT_MAP_MAX) return 0;
    int usable=0;
    for(size_t i=0;i<n;i++) {
        if(!(m[i].attributes&1) || !m[i].length) continue;
        if(m[i].length>UINT64_MAX-m[i].base) return 0;
        if(m[i].type==1) usable=1;
    }
    return usable;
}
int boot_range_usable(const struct boot_map_entry *m,size_t n,uint64_t start,uint64_t length) {
    if(!boot_map_valid(m,n) || !length || length>UINT64_MAX-start) return 0;
    uint64_t end=start+length;
    int found=0;
    for(size_t i=0;i<n;i++) {
        if(!(m[i].attributes&1) || !m[i].length) continue;
        uint64_t limit=m[i].base+m[i].length;
        if(start>=limit || end<=m[i].base) continue;
        if(m[i].type!=1) return 0;
        if(start>=m[i].base && end<=limit) found=1;
    }
    return found;
}

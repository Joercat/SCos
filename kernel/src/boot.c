#include "boot.h"
/* Firmware descriptors may be unordered, but overlapping ownership is rejected.
 * Never reinterpret firmware/runtime/ACPI/loader pages as free conventional RAM. */
int boot_memory_usable(const struct efi_memory *m){
 /* Do not erase persistent/special-purpose RAM or override firmware access
  * restrictions. PAT selects WB only where firmware advertises WB support. */
 const uint64_t excluded=(UINT64_C(1)<<63)|0x1000|0x2000|0x8000|0x20000|0x40000;
 return m->type==7&&m->pages&&(m->attributes&8)&&!(m->attributes&excluded);
}
int boot_map_valid(const struct efi_memory *m,size_t n){
 if(!m||!n||n>BOOT_MAP_MAX)return 0;
 int usable=0;
 for(size_t i=0;i<n;i++){
  if(!m[i].pages)continue;
  if((m[i].physical&4095)||m[i].physical>=PHYSICAL_LIMIT||m[i].pages>(PHYSICAL_LIMIT-m[i].physical)/4096)return 0;
  uint64_t end=m[i].physical+m[i].pages*4096;
  for(size_t j=0;j<i;j++)if(m[j].pages&&m[i].physical<m[j].physical+m[j].pages*4096&&end>m[j].physical)return 0;
  if(boot_memory_usable(&m[i]))usable=1;
 }
 return usable;
}
int boot_range_usable(const struct efi_memory *m,size_t n,uint64_t start,uint64_t length){
 if(!length||start>=PHYSICAL_LIMIT||length>PHYSICAL_LIMIT-start)return 0;
 for(size_t i=0;i<n;i++)if(boot_memory_usable(&m[i])&&start>=m[i].physical&&start-m[i].physical<=m[i].pages*4096&&length<=m[i].pages*4096-(start-m[i].physical))return 1;
 return 0;
}

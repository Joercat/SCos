/* Original SCos mechanism-1 PCI access, made atomic and bridge-loop safe.
 * BARs retain firmware placement; no speculative relocation or status W1C RMW. */
#include "scos.h"
static u64 lock(void){u64 f;__asm__ volatile("pushfq;popq %0;cli":"=r"(f)::"memory");return f;}
static void unlock(u64 f){if(f&512)__asm__ volatile("sti":::"memory");}
static u32 address(u8 b,u8 d,u8 f,u8 o){return 0x80000000u|((u32)b<<16)|((u32)d<<11)|((u32)f<<8)|(o&0xfc);}
u32 pci_read32(u8 b,u8 d,u8 f,u8 o){u64 flags=lock();outl(0xcf8,address(b,d,f,o));u32 v=inl(0xcfc);unlock(flags);return v;}
void pci_write32(u8 b,u8 d,u8 f,u8 o,u32 v){u64 flags=lock();outl(0xcf8,address(b,d,f,o));outl(0xcfc,v);unlock(flags);}
u16 pci_read16(u8 b,u8 d,u8 f,u8 o){return (u16)(pci_read32(b,d,f,o)>>((o&2)*8));}
u8 pci_read8(u8 b,u8 d,u8 f,u8 o){return (u8)(pci_read32(b,d,f,o)>>((o&3)*8));}
void pci_write16(u8 b,u8 d,u8 f,u8 o,u16 v){u64 flags=lock();outl(0xcf8,address(b,d,f,o));outw(0xcfc+(o&2),v);unlock(flags);}
static int scan(u8 bus,u8 *visited,u8 cls,u8 sub,u8 pi,u8 *b,u8 *d,u8 *f,int max){
 if(visited[bus]||max<=0)return 0;
 visited[bus]=1;int n=0;
 for(u8 dev=0;dev<32&&n<max;dev++){
  if((pci_read32(bus,dev,0,0)&65535)==65535)continue;
  u8 count=(pci_read8(bus,dev,0,14)&128)?8:1;
  for(u8 fn=0;fn<count&&n<max;fn++){
   if((pci_read32(bus,dev,fn,0)&65535)==65535)continue;
   u32 id=pci_read32(bus,dev,fn,8);
   if((u8)(id>>24)==cls&&(u8)(id>>16)==sub&&(pi==255||(u8)(id>>8)==pi)){b[n]=bus;d[n]=dev;f[n++]=fn;}
   if((id>>16)==0x0604){u8 sec=pci_read8(bus,dev,fn,0x19);if(sec&&sec!=bus)n+=scan(sec,visited,cls,sub,pi,b+n,d+n,f+n,max-n);}
  }
 }
 return n;
}
int pci_find_class(u8 cls,u8 sub,u8 pi,u8 *b,u8 *d,u8 *f,int max){
 if(!b||!d||!f||max<=0)return 0;
 if(max>8)max=8;
 u8 visited[256]={0};return scan(0,visited,cls,sub,pi,b,d,f,max);
}
int pci_scan_dump(void){u8 b[8],d[8],f[8];int n=pci_find_class(12,3,255,b,d,f,8);for(int i=0;i<n;i++)klog("pci: USB controller %u:%u.%u class %x",b[i],d[i],f[i],pci_read32(b[i],d[i],f[i],8));return n;}
int pci_memory_bar(u8 b,u8 d,u8 f,u8 index,u64 *base,u64 *bytes){
 if(index>5)return 0;
 u8 off=0x10+index*4;u32 lo=pci_read32(b,d,f,off);
 if((lo&1)||((lo&6)!=0&&(lo&6)!=4)||((lo&6)==4&&index==5))return 0;
 int wide=(lo&6)==4;u32 hi=wide?pci_read32(b,d,f,off+4):0;
 u64 flags=lock();u16 command=pci_read16(b,d,f,4);
 pci_write16(b,d,f,4,command&~6u);
 pci_write32(b,d,f,off,~0u);if(wide)pci_write32(b,d,f,off+4,~0u);
 u32 ml=pci_read32(b,d,f,off)&~15u,mh=wide?pci_read32(b,d,f,off+4):0;
 pci_write32(b,d,f,off,lo);if(wide)pci_write32(b,d,f,off+4,hi);
 pci_write16(b,d,f,4,command);unlock(flags);
 *base=((u64)hi<<32)|(lo&~15u);
 *bytes=wide?~(((u64)mh<<32)|ml)+1:(u32)(~ml+1);
 return *base&&*bytes&&!(*bytes&(*bytes-1))&&*base<PHYSICAL_LIMIT&&*bytes<=PHYSICAL_LIMIT-*base;
}

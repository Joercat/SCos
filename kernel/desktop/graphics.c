/* Read-only display adapter discovery. Do not confuse GOP scanout with a GPU
 * rendering driver: no hardware acceleration backend is linked in this build.
 * In particular, never probe BAR sizes or modeset the active firmware display
 * merely to identify it. Those operations can destroy the only usable console. */
#include "scos.h"
#define GPU_MAX 8
struct gpu_device {u16 vendor,device;u8 bus,slot,function,subclass;};
static struct gpu_device adapters[GPU_MAX];
static int adapter_count;
static struct boot_framebuffer scanout;
static const char *vendor_name(u16 vendor){switch(vendor){case 0x8086:return "Intel";case 0x1002:return "AMD";case 0x10de:return "NVIDIA";case 0x15ad:return "VMware";case 0x1af4:return "Virtio";case 0x1234:return "QEMU";default:return "Unknown vendor";}}
static void hex4(char *out,u16 value){const char *digits="0123456789abcdef";for(int i=0;i<4;i++)out[i]=digits[(value>>(12-i*4))&15];out[4]=0;}
void graphics_init(const struct boot_framebuffer *fb){
    scanout=*fb;adapter_count=0;
    const u8 classes[]={0,1,2,0x80};
    for(unsigned c=0;c<sizeof(classes);c++){
        u8 bus[8],slot[8],function[8];int count=pci_find_class(3,classes[c],255,bus,slot,function,8);
        for(int i=0;i<count&&adapter_count<GPU_MAX;i++){
            u32 id=pci_read32(bus[i],slot[i],function[i],0);
            struct gpu_device *g=&adapters[adapter_count++];*g=(struct gpu_device){id&65535,id>>16,bus[i],slot[i],function[i],classes[c]};
            klog("graphics: PCI %u:%u.%u vendor=%x device=%x; no accelerated backend",g->bus,g->slot,g->function,g->vendor,g->device);
        }
    }
    klog("graphics: software compositor / GOP scanout; %u display adapter(s); no matching accelerated driver linked",adapter_count);
}
void graphics_report(char *out,size_t capacity){
    char report[1024],number[24];strcpy(report,"Renderer: CPU software compositor\nScanout: firmware GOP, PAT write-combining\nGPU acceleration: unavailable (no hardware backend linked)\nDisplay: ");fmt_u32(number,scanout.width);strcat(report,number);strcat(report,"x");fmt_u32(number,scanout.height);strcat(report,number);strcat(report,"\nPCI display adapters (segment 0, up to 8):\n");
    for(int i=0;i<adapter_count;i++){
        struct gpu_device *g=&adapters[i];strcat(report,vendor_name(g->vendor));strcat(report," ");hex4(number,g->vendor);strcat(report,number);strcat(report,":");hex4(number,g->device);strcat(report,number);strcat(report," at ");fmt_u32(number,g->bus);strcat(report,number);strcat(report,":");fmt_u32(number,g->slot);strcat(report,number);strcat(report,".");fmt_u32(number,g->function);strcat(report,number);strcat(report,"\n");
    }
    if(!adapter_count)strcat(report,"No supported PCI discovery result; firmware scanout retained.\n");
    strcat(report,"Vendor detection is not driver support. No GPU resets or modesets performed.");
    if(capacity){strncpy(out,report,capacity-1);out[capacity-1]=0;}
}

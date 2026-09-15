/* SCos native - minimal PCI configuration space access + enumeration.
 * Enough to find host controllers (USB) by class code, walking bridges.
 */
#include "scos.h"

#define CFG_ADDR 0xCF8
#define CFG_DATA 0xCFC

static u32 pci_cfg(u8 bus, u8 dev, u8 fn, u8 off)
{
    return 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) |
           ((u32)fn << 8) | (off & 0xFC);
}

u32 pci_read32(u8 bus, u8 dev, u8 fn, u8 off)
{
    outb(CFG_ADDR, 0); /* ensure dword alignment of the address port */
    outl(CFG_ADDR, pci_cfg(bus, dev, fn, off));
    return inl(CFG_DATA);
}

void pci_write32(u8 bus, u8 dev, u8 fn, u8 off, u32 v)
{
    outl(CFG_ADDR, pci_cfg(bus, dev, fn, off));
    outl(CFG_DATA, v);
}

u16 pci_read16(u8 bus, u8 dev, u8 fn, u8 off)
{
    return (u16)(pci_read32(bus, dev, fn, off & 0xFC) >> ((off & 2) * 8));
}

u8 pci_read8(u8 bus, u8 dev, u8 fn, u8 off)
{
    return (u8)(pci_read32(bus, dev, fn, off & 0xFC) >> ((off & 3) * 8));
}

struct pci_found {
    u8 bus, dev, fn;
};

static int scan_bus(u8 bus, int depth, u8 class, u8 subclass, u8 progif,
                    struct pci_found *out, int max)
{
    int n = 0;
    if (depth > 4) return 0;
    for (u16 d = 0; d < 32 && n < max; d++) {
        for (u8 f = 0; f < 8 && n < max; f++) {
            u32 id = pci_read32(bus, d, f, 0);
            if (id == 0xFFFFFFFFu) { if (f == 0) break; else continue; }
            u32 cls = pci_read32(bus, d, f, 8);
            /* reg 0x08: rev 0-7, progIF 8-15, subclass 16-23, base 24-31 */
            u8 pi = (u8)(cls >> 8), sc = (u8)(cls >> 16), c = (u8)(cls >> 24);
            if (c == class && sc == subclass &&
                (progif == 0xFF || pi == progif)) {
                out[n].bus = bus; out[n].dev = (u8)d; out[n].fn = f;
                n++;
            }
            /* bridge: follow it (xHCI can sit behind PCIe bridges) */
            if (c == 0x06 && sc == 0x04) {
                u8 sec = pci_read8(bus, d, f, 0x19);
                if (sec != bus) n += scan_bus(sec, depth + 1, class,
                                              subclass, progif, out + n,
                                              max - n);
            }
            u8 hdr = pci_read8(bus, d, f, 0x0E);
            if (!(hdr & 0x80)) break;   /* single-function device */
        }
    }
    return n;
}

int pci_find_class(u8 class, u8 subclass, u8 progif,
                   u8 *bus, u8 *dev, u8 *fn, int max)
{
    struct pci_found f[8];
    if (max > 8) max = 8;
    int n = scan_bus(0, 0, class, subclass, progif, f, max);
    for (int i = 0; i < n; i++) {
        bus[i] = f[i].bus; dev[i] = f[i].dev; fn[i] = f[i].fn;
    }
    return n;
}

/* Forensic sweep: log everything sitting on bus 0 so a failed class match
 * is still visible in the diagnostics ring. Returns function count. */
int pci_scan_dump(void)
{
    int funcs = 0, bridges = 0, usb = 0;
    for (u16 d = 0; d < 32; d++) {
        for (u8 f = 0; f < 8; f++) {
            u32 id = pci_read32(0, (u8)d, f, 0);
            if (id == 0xFFFFFFFFu) { if (f == 0) break; continue; }
            funcs++;
            u32 cls = pci_read32(0, (u8)d, f, 8);
            u8 pi = (u8)(cls >> 8), sc = (u8)(cls >> 16), c = (u8)(cls >> 24);
            if (c == 0x06 && sc == 0x04) bridges++;
            if (c == 0x0C && sc == 0x03) {
                usb++;
                klog("pci: USB ctrl %d:%d.%x vid %x dev %x prog %x",
                     0, (int)d, f, id & 0xFFFF, (id >> 16) & 0xFFFF, pi);
            }
            if (!(pci_read8(0, (u8)d, f, 0x0E) & 0x80)) break;
        }
    }
    klog("pci: bus 0: %d functions, %d bridges, %d USB controller(s)",
         funcs, bridges, usb);
    return funcs;
}

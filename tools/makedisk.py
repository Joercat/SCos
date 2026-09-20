#!/usr/bin/env python3
"""
SCos native - assemble the bootable disk image.

Layout:
  sector 0        stage1 (MBR)
  sectors 1..16   stage2 (padded to 8 KB)
  sector 17..     kernel flat binary

stage2 contains two patchable u32 fields (kernel_start_lba,
kernel_sector_count); their offsets are read from the stage2 ELF symbol
table and patched here.
"""
import re
import subprocess
import sys
import os

SECTOR = 512
STAGE2_SECTORS = 16
KERNEL_START = 17
IMG_SECTORS = 16384      # 8 MB


def symbol_offsets(elf, names):
    out = subprocess.run(["objdump", "-t", elf], capture_output=True, text=True)
    offs = {}
    for line in out.stdout.splitlines():
        for n in names:
            m = re.search(r"^([0-9a-f]+)\s.*\s\.text\s+" + n + r"$", line)
            if not m:
                m = re.search(r"^([0-9a-f]+)\s.*\s" + n + r"$", line)
            if m:
                offs[n] = int(m.group(1), 16)
    return offs


def main(build_dir):
    stage1 = open(os.path.join(build_dir, "stage1.bin"), "rb").read()
    stage2 = open(os.path.join(build_dir, "stage2.bin"), "rb").read()
    kernel = open(os.path.join(build_dir, "kernel.bin"), "rb").read()

    if len(stage1)!=SECTOR or stage1[510:]!=b"\x55\xaa":
        raise ValueError("invalid MBR")
    if any(stage1[400:510]):
        raise ValueError("MBR code overlaps reserved ownership descriptor")
    if len(stage2)>STAGE2_SECTORS*SECTOR:
        raise ValueError("stage2 too big")
    # Conservative legacy low-memory staging cap: 0x20000..0x80000.
    if not kernel or (len(kernel)+511)//512*512 > 0x60000:
        raise ValueError("kernel exceeds legacy BIOS staging policy")
    if KERNEL_START*512+len(kernel)>2048*512:
        raise ValueError("kernel overlaps persistence")
    import struct
    stage1=bytearray(stage1)
    stage1[400:412]=b"SCOSDISK32v1"
    struct.pack_into("<IIII",stage1,412,2048,256,IMG_SECTORS,len(kernel))

    offs = symbol_offsets(os.path.join(build_dir, "stage2.elf"),
                          ["kernel_start_lba", "kernel_sector_count"])
    kernel_lba = KERNEL_START
    kernel_sectors = (len(kernel) + SECTOR - 1) // SECTOR

    for name in ("kernel_start_lba","kernel_sector_count"):
        if name not in offs or not 0 <= offs[name]-0x8000 <= len(stage2)-4:
            raise ValueError("invalid stage2 patch symbol: "+name)
    stage2 = bytearray(stage2 + b"\0" * (STAGE2_SECTORS * SECTOR - len(stage2)))
    import struct
    o = offs["kernel_start_lba"] - 0x8000
    struct.pack_into("<I", stage2, o, kernel_lba)
    o = offs["kernel_sector_count"] - 0x8000
    struct.pack_into("<I", stage2, o, kernel_sectors)

    img = bytearray(IMG_SECTORS * SECTOR)
    img[0:SECTOR] = stage1
    img[SECTOR:SECTOR + len(stage2)] = stage2
    kstart = KERNEL_START * SECTOR
    img[kstart:kstart + len(kernel)] = kernel

    if len(img)!=IMG_SECTORS*SECTOR or img[510:512]!=b"\x55\xaa":
        raise ValueError("assembled image size/signature mismatch")
    out = os.path.join(build_dir, "scos.img")
    open(out, "wb").write(bytes(img))
    print(f"makedisk: {out}  kernel={len(kernel)} bytes "
          f"({kernel_sectors} sectors @ LBA {kernel_lba})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "build"))

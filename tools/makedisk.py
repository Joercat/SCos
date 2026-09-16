#!/usr/bin/env python3
"""
SCos native - assemble the bootable disk image.

Disk layout:
  sector 0        stage1 (MBR)
  sectors 1..16   stage2 (padded to 8 KB)
  sector 17..     kernel flat binary
  sector 2048..   SCos filesystem image
"""

import re
import subprocess
import sys
import os
import struct

SECTOR = 512
STAGE2_SECTORS = 16
KERNEL_START = 17

IMG_SECTORS = 16384

FS_LBA = 2048
FS_MAX_SECTORS = 256


def symbol_offsets(elf, names):
    out = subprocess.run(
        ["objdump", "-t", elf],
        capture_output=True,
        text=True
    )

    offs = {}

    for line in out.stdout.splitlines():
        for n in names:
            m = re.search(
                r"^([0-9a-f]+)\s.*\s\.text\s+" + n + r"$",
                line
            )

            if not m:
                m = re.search(
                    r"^([0-9a-f]+)\s.*\s" + n + r"$",
                    line
                )

            if m:
                offs[n] = int(m.group(1), 16)

    return offs


def fs_add_file(buf, offset, path, data):
    path_bytes = path.encode("utf-8")

    if len(path_bytes) > 0xFFFF:
        raise RuntimeError("filesystem path too long")

    needed = 2 + len(path_bytes) + 1 + 4 + len(data)

    if offset + needed > FS_MAX_SECTORS * SECTOR:
        raise RuntimeError(
            "filesystem image is too large for %d sectors" %
            FS_MAX_SECTORS
        )

    struct.pack_into("<H", buf, offset, len(path_bytes))
    offset += 2

    buf[offset:offset + len(path_bytes)] = path_bytes
    offset += len(path_bytes)

    buf[offset] = 0
    offset += 1

    struct.pack_into("<I", buf, offset, len(data))
    offset += 4

    buf[offset:offset + len(data)] = data
    offset += len(data)

    return offset


def build_fs_image():
    buf = bytearray(FS_MAX_SECTORS * SECTOR)
    buf[0:8] = b"SCOSFS1\0"

    offset = 12
    count = 0

    test_image = os.path.join("assets", "test.ppm")

    if os.path.exists(test_image):
        with open(test_image, "rb") as f:
            data = f.read()

        offset = fs_add_file(
            buf,
            offset,
            "/home/documents/test.ppm",
            data
        )

        count += 1
        print(
            "makedisk: added /home/documents/test.ppm (%d bytes)"
            % len(data)
        )
    else:
        print("makedisk: warning: assets/test.ppm not found")

    struct.pack_into("<I", buf, 8, count)

    sectors = (offset + SECTOR - 1) // SECTOR

    if sectors > FS_MAX_SECTORS:
        raise RuntimeError("filesystem image too large")

    return buf, sectors


def main(build_dir):
    stage1 = open(
        os.path.join(build_dir, "stage1.bin"),
        "rb"
    ).read()

    stage2 = open(
        os.path.join(build_dir, "stage2.bin"),
        "rb"
    ).read()

    kernel = open(
        os.path.join(build_dir, "kernel.bin"),
        "rb"
    ).read()

    assert len(stage1) == SECTOR, (
        f"stage1 must be 512 bytes, got {len(stage1)}"
    )

    assert len(stage2) <= STAGE2_SECTORS * SECTOR, (
        "stage2 too big"
    )

    offs = symbol_offsets(
        os.path.join(build_dir, "stage2.elf"),
        ["kernel_start_lba", "kernel_sector_count"]
    )

    kernel_lba = KERNEL_START
    kernel_sectors = (
        len(kernel) + SECTOR - 1
    ) // SECTOR

    stage2 = bytearray(
        stage2 +
        b"\0" * (
            STAGE2_SECTORS * SECTOR -
            len(stage2)
        )
    )

    o = offs["kernel_start_lba"] - 0x8000
    struct.pack_into("<I", stage2, o, kernel_lba)

    o = offs["kernel_sector_count"] - 0x8000
    struct.pack_into("<I", stage2, o, kernel_sectors)

    fs_img, fs_sectors = build_fs_image()

    img = bytearray(IMG_SECTORS * SECTOR)

    img[0:SECTOR] = stage1

    img[
        SECTOR:
        SECTOR + len(stage2)
    ] = stage2

    kstart = KERNEL_START * SECTOR

    img[
        kstart:
        kstart + len(kernel)
    ] = kernel

    fs_start = FS_LBA * SECTOR

    img[
        fs_start:
        fs_start + len(fs_img)
    ] = fs_img

    out = os.path.join(build_dir, "scos.img")

    with open(out, "wb") as f:
        f.write(bytes(img))

    print(
        f"makedisk: {out}  "
        f"kernel={len(kernel)} bytes "
        f"({kernel_sectors} sectors @ LBA {kernel_lba})"
    )

    print(
        f"makedisk: filesystem={fs_sectors} sectors "
        f"@ LBA {FS_LBA}"
    )

    return 0


if __name__ == "__main__":
    sys.exit(
        main(sys.argv[1] if len(sys.argv) > 1 else "build")
    )

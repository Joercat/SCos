#!/usr/bin/env python3
"""Package a matched BIOS bootstrap and ELF64 kernel; fail before writing on error.

The raw payload has no ELF header at runtime. Its CRC only detects corruption,
so build-time checks must establish that it really matches the linked ELF and
its load addresses. A stale kernel.bin must not receive a fresh, valid CRC.
"""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

KERNEL_BASE = 0x100000
STAGING_BYTES = 0x60000
MEMORY_LIMIT = 0x1000000
IMAGE_BYTES = 8 * 1024 * 1024


def symbols(path):
    result = {}
    output = subprocess.check_output(['nm', '--defined-only', str(path)], text=True)
    for line in output.splitlines():
        fields = line.split()
        if len(fields) == 3:
            result[fields[2]] = int(fields[0], 16)
    return result


def checked_kernel(elf, raw, linked):
    """Validate ELF program headers, then reconstruct its exact padded payload."""
    if len(elf) < 64 or elf[:7] != b'\x7fELF\x02\x01\x01':
        raise ValueError('kernel must be little-endian ELF64 version 1')
    (_, kind, machine, version, entry, phoff, _, _, ehsize, phsize,
     phcount, _, _, _) = struct.unpack_from('<16sHHIQQQIHHHHHH', elf)
    if (kind, machine, version, entry, ehsize, phsize) != (2, 62, 1, KERNEL_BASE, 64, 56):
        raise ValueError('invalid AMD64 executable/header/entry')
    if not 1 <= phcount <= 128 or phoff < 64 or phoff + phcount * phsize > len(elf):
        raise ValueError('invalid ELF program-header table')
    file_end, memory_end = linked['_file_end'], linked['_kernel_end']
    if (linked['_start'] != KERNEL_BASE or linked['_kernel_start'] != KERNEL_BASE or
            not KERNEL_BASE < file_end <= memory_end <= MEMORY_LIMIT or memory_end % 4096):
        raise ValueError('invalid linked kernel spans')
    file_size = file_end - KERNEL_BASE
    if not raw or len(raw) > file_size or file_size > STAGING_BYTES:
        raise ValueError('kernel does not fit BIOS staging policy')
    expected = bytearray(file_size)
    spans = []
    executable_entry = False
    for i in range(phcount):
        kind, flags, offset, virtual, physical, filesz, memsz, align = struct.unpack_from(
            '<IIQQQQQQ', elf, phoff + i * phsize)
        if kind in (2, 3):  # PT_DYNAMIC/PT_INTERP require a loader we do not have.
            raise ValueError('dynamic/interpreted kernels are not supported')
        if kind != 1:  # Only PT_LOAD contributes bytes or BSS.
            continue
        if (virtual != physical or filesz > memsz or offset + filesz > len(elf) or
                physical < KERNEL_BASE or physical + memsz > memory_end or
                (filesz and physical + filesz > file_end)):
            raise ValueError('ELF load segment outside linked/file bounds')
        if flags & ~7 or not flags & 4 or (flags & 3) == 3:
            raise ValueError('unsupported or writable-executable ELF segment')
        if align not in (0, 1) and (align & (align - 1) or (virtual - offset) % align):
            raise ValueError('invalid ELF segment alignment')
        if memsz:
            end = physical + memsz
            if any(physical < b and end > a for a, b in spans):
                raise ValueError('overlapping ELF load segments')
            spans.append((physical, end))
        if flags & 1 and physical <= entry < physical + filesz:
            executable_entry = True
        if filesz:
            start = physical - KERNEL_BASE
            expected[start:start + filesz] = elf[offset:offset + filesz]
    if not executable_entry or not spans:
        raise ValueError('entry is not backed by an executable load segment')
    padded = raw.ljust(file_size, b'\0')
    if padded != expected:
        raise ValueError('kernel.bin does not match kernel.elf load bytes')
    return padded


def main(directory):
    d = Path(directory)
    mbr = (d / 'stage1.bin').read_bytes()
    stage = bytearray((d / 'stage2.bin').read_bytes())
    linked = symbols(d / 'kernel.elf')
    stage_symbols = symbols(d / 'stage2.elf')
    kernel = checked_kernel((d / 'kernel.elf').read_bytes(),
                            (d / 'kernel.bin').read_bytes(), linked)
    if len(mbr) != 512 or mbr[510:] != b'\x55\xaa' or any(mbr[400:510]):
        raise ValueError('invalid MBR/reserved label area')
    if not 0 < len(stage) <= 8192 or stage_symbols['_start'] != 0x8000:
        raise ValueError('invalid stage2 size/entry')
    # nm offsets are meaningful only for the exact binary derived from that ELF.
    with tempfile.TemporaryDirectory(prefix='scos-package-') as temporary:
        extracted = Path(temporary) / 'stage2.bin'
        subprocess.run(['objcopy', '-O', 'binary', '-j', '.text',
                        str(d / 'stage2.elf'), str(extracted)], check=True)
        if extracted.read_bytes() != stage:
            raise ValueError('stage2.bin does not match stage2.elf')
    patches = {
        'kernel_start_lba': 17,
        'kernel_sector_count': (len(kernel) + 511) // 512,
        'kernel_file_bytes': len(kernel),
        'kernel_memory_end': linked['_kernel_end'],
        'kernel_crc32': zlib.crc32(kernel),
    }
    used = set()
    for name, value in patches.items():
        offset = stage_symbols[name] - 0x8000
        positions = set(range(offset, offset + 4))
        if not 0 <= offset <= len(stage) - 4 or used & positions:
            raise ValueError('bad/overlapping stage2 symbol ' + name)
        used.update(positions)
        struct.pack_into('<I', stage, offset, value)
    image = bytearray(IMAGE_BYTES)
    image[:512] = mbr
    image[400:412] = b'SCOSBOOT64v1'  # Not legacy writable-persistence ownership.
    image[512:512 + len(stage)] = stage
    image[17 * 512:17 * 512 + len(kernel)] = kernel
    # All input validation precedes publication; replace atomically on this host.
    output = d / 'scos.img'
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=d, prefix='.scos-', delete=False) as f:
            temporary = Path(f.name)
            f.write(image)
        temporary.replace(output)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    print(f'AMD64 startup-only image: {len(kernel)} kernel file bytes; '
          f'memory end {linked["_kernel_end"]:#x}')


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'build')

#!/usr/bin/env python3
"""Package the BIOS bootstrap + ELF64-derived kernel, never a 32-bit persistence disk."""
from pathlib import Path
import struct
import subprocess
import sys
import zlib

def symbols(path):
    result={}
    for line in subprocess.check_output(['nm','--defined-only',str(path)],text=True).splitlines():
        fields=line.split()
        if len(fields)==3: result[fields[2]]=int(fields[0],16)
    return result

def main(directory):
    d=Path(directory)
    mbr=(d/'stage1.bin').read_bytes()
    stage=bytearray((d/'stage2.bin').read_bytes())
    kernel=(d/'kernel.bin').read_bytes()
    elf=(d/'kernel.elf').read_bytes()
    if elf[:6]!=b'\x7fELF\x02\x01' or struct.unpack_from('<H',elf,18)[0]!=62:
        raise ValueError('kernel must be little-endian ELF64 AMD64')
    k=symbols(d/'kernel.elf');s=symbols(d/'stage2.elf')
    if k['_start']!=0x100000 or not 0x100000<k['_file_end']<=k['_kernel_end']<=0x1000000:
        raise ValueError('invalid linked kernel spans')
    file_size=k['_file_end']-0x100000
    if not kernel or len(kernel)>file_size or not 0<file_size<=0x60000:
        raise ValueError('kernel does not fit legacy BIOS staging policy')
    kernel=kernel.ljust(file_size,b'\0')
    if len(mbr)!=512 or mbr[510:]!=b'\x55\xaa' or any(mbr[400:510]):
        raise ValueError('invalid MBR/reserved label area')
    if len(stage)>8192: raise ValueError('stage2 exceeds 16 sectors')
    patches={'kernel_start_lba':17,'kernel_sector_count':(file_size+511)//512,
             'kernel_file_bytes':file_size,'kernel_memory_end':k['_kernel_end'],'kernel_crc32':zlib.crc32(kernel)}
    for name,value in patches.items():
        off=s[name]-0x8000
        if not 0<=off<=len(stage)-4: raise ValueError('bad stage2 symbol '+name)
        struct.pack_into('<I',stage,off,value)
    image=bytearray(8*1024*1024)
    image[:512]=mbr
    image[400:412]=b'SCOSBOOT64v1' # not recognized as writable by legacy SCos
    image[512:512+len(stage)]=stage
    image[17*512:17*512+file_size]=kernel
    if len(image)!=8*1024*1024 or image[510:512]!=b'\x55\xaa':
        raise ValueError('image length/signature mismatch')
    (d/'scos.img').write_bytes(image)
    print(f'AMD64 image: {file_size} kernel file bytes; memory end {k["_kernel_end"]:#x}')

if __name__=='__main__': main(sys.argv[1] if len(sys.argv)>1 else 'build')

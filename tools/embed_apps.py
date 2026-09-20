#!/usr/bin/env python3
"""Embed editable source examples in the initial VFS, not in the compositor."""
import pathlib
import sys
import struct
import zlib
out = pathlib.Path(sys.argv[1])
files = sorted(pathlib.Path('apps/examples').glob('*.lua'))
lines = ['/* Generated from apps/examples; do not edit. */']
for i, p in enumerate(files):
    data = p.read_bytes()
    if not 0 < len(data) <= 65536:
        raise SystemExit(f'invalid Lua source size: {p}')
    header=bytearray(128)
    header[:8]=b'SCOSPRJ1'
    struct.pack_into('<7I',header,8,128,len(data),0,2,0,560,360)
    app_id=('sample-'+p.stem).encode(); title=('Sample '+p.stem).encode()
    if len(app_id)>30 or len(title)>39:raise SystemExit('example metadata too long')
    header[36:36+len(app_id)]=app_id;header[68:68+len(title)]=title
    data=header+data;struct.pack_into('<I',data,16,zlib.crc32(data))
    lines.append(f'static const unsigned char lua_example_{i}[] = {{')
    for j in range(0, len(data), 24):
        lines.append(','.join(str(n) for n in data[j:j+24]) + ',')
    lines.append('};')
lines.append('static const struct { const char *path; const unsigned char *data; u32 size; } lua_examples[] = {')
for i, p in enumerate(files):
    lines.append(f'{{"home/projects/sample-{p.stem}.project",lua_example_{i},sizeof(lua_example_{i})}},')
lines.append('};')
notice = pathlib.Path('third_party/NOTICES.txt').read_bytes()
lines.append('static const unsigned char lua_notices[] = {')
for j in range(0, len(notice), 24):
    lines.append(','.join(str(n) for n in notice[j:j+24]) + ',')
lines.append('};')
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text('\n'.join(lines) + '\n')

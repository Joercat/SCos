#!/usr/bin/env python3
"""Embed optional CAT demo packages outside the installed-app discovery folder."""
import pathlib
import sys
import struct
import zlib
import json
out = pathlib.Path(sys.argv[1])
files = sorted(pathlib.Path('apps/examples').glob('*.lua'))
lines = ['/* Generated from apps/examples; do not edit. */']
for i, p in enumerate(files):
    data = p.read_bytes()
    if not 0 < len(data) <= 65536:
        raise SystemExit(f'invalid Lua source size: {p}')
    header=bytearray(128)
    header[:8]=b'SCOSCAT1'
    struct.pack_into('<7I',header,8,128,len(data),0,2,0,560,360)
    app_id=p.stem.encode(); title=p.stem.capitalize().encode()
    if len(app_id)>30 or len(title)>39:raise SystemExit('example metadata too long')
    header[36:36+len(app_id)]=app_id;header[68:68+len(title)]=title
    data=header+data;struct.pack_into('<I',data,16,zlib.crc32(data))
    lines.append(f'static const unsigned char lua_example_{i}[] = {{')
    for j in range(0, len(data), 24):
        lines.append(','.join(str(n) for n in data[j:j+24]) + ',')
    lines.append('};')
instructions = """OPTIONAL CAT APPLICATION DEMOS

Counter and Sketch are Lua demonstrations, not built-in system applications.
They are NOT installed or listed in the launcher on a fresh boot.

1. In Files open /home/demos.
2. Double-click counter.cat or sketch.cat.
3. Read the native install confirmation; Cancel changes nothing.
4. Approve to install a copy into /home/apps plus a desktop shortcut.
5. Later find it in the launcher or use appstrt counter / appstrt sketch.

To inspect or modify a demo, open App Studio, choose Open, and enter
/home/demos/counter.cat (or sketch.cat). Change its ID for your own app.
Ctrl+S saves a .project; Ctrl+B builds .cat; F5 builds and runs.

Studio: Shift+arrows or mouse drag selects text. Ctrl+A selects all.
Ctrl+C copies, Ctrl+X cuts, Ctrl+V pastes. Clipboard maximum: 16 KiB.
Source maximum: 64 KiB. Oversized edits are rejected, not truncated.
This clipboard belongs to Studio, not the host computer.

Installation saves the current filesystem on supported ATA disks. Failures are
reported. USB-only storage is session RAM: reboot loses the installation.
Use the confirmed Terminal save command to persist later edits on supported disks.
Only install trusted packages. A checksum is not a security signature.
"""
lines.append('static const unsigned char demo_readme[] = '+json.dumps(instructions)+';')
lines.append('static const struct { const char *path; const unsigned char *data; u32 size; } lua_examples[] = {')
for i, p in enumerate(files):
    lines.append(f'{{"home/demos/{p.stem}.cat",lua_example_{i},sizeof(lua_example_{i})}},')
lines.append('{"home/demos/README.txt",demo_readme,sizeof(demo_readme)-1},')
lines.append('};')
notice = pathlib.Path('third_party/NOTICES.txt').read_bytes()
lines.append('static const unsigned char lua_notices[] = {')
for j in range(0, len(notice), 24):
    lines.append(','.join(str(n) for n in notice[j:j+24]) + ',')
lines.append('};')
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text('\n'.join(lines) + '\n')

#!/usr/bin/env python3
"""Embed editable source examples in the initial VFS, not in the compositor."""
import pathlib
import sys
out = pathlib.Path(sys.argv[1])
files = sorted(pathlib.Path('apps/examples').glob('*.lua'))
lines = ['/* Generated from apps/examples; do not edit. */']
for i, p in enumerate(files):
    data = p.read_bytes()
    if not 0 < len(data) <= 65536:
        raise SystemExit(f'invalid Lua source size: {p}')
    lines.append(f'static const unsigned char lua_example_{i}[] = {{')
    for j in range(0, len(data), 24):
        lines.append(','.join(str(n) for n in data[j:j+24]) + ',')
    lines.append('};')
lines.append('static const struct { const char *path; const unsigned char *data; u32 size; } lua_examples[] = {')
for i, p in enumerate(files):
    lines.append(f'{{"home/apps/{p.name}",lua_example_{i},sizeof(lua_example_{i})}},')
lines.append('};')
notice = pathlib.Path('third_party/NOTICES.txt').read_bytes()
lines.append('static const unsigned char lua_notices[] = {')
for j in range(0, len(notice), 24):
    lines.append(','.join(str(n) for n in notice[j:j+24]) + ',')
lines.append('};')
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text('\n'.join(lines) + '\n')

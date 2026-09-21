#!/usr/bin/env python3
"""Generate SCos' GPU auto-detection tables from the upstream driver sources.

SCos must not invent device support: every ID it binds to is an ID that the driver
being ported actually binds to, read out of that driver's own binding table.  This
tool extracts those tables - and the per-family feature statuses measured next to
them - from a Haiku checkout at the pinned commit, and emits
`kernel/drivers/gpu/gpu_ids.h`.

Three table shapes exist upstream, and the difference is not cosmetic:

  * row form    `{ 0xNNNN, GROUP, "chip name" }`  - name comes from the string;
  * raw form    `{ 0x0020, /* Nvidia TNT1 */ }`  - name comes from the comment;
  * macro form  `{ DEVICE_ID_RADEON_QD, ... }`   - value comes from a `#define`.

Two exclusion rules are applied the way the C compiler applies them, because a row
the upstream driver cannot reach is not a supported chip:

  * a line-commented-out entry (`//  0x051A, /* MGA-1064 PCI Mystique */`) is not in
    the table - matrox and via each have such a row;
  * an entry inside `#if 0` is not in the table - `intel_extreme` has 6 such rows and
    `radeon_hd` has 119.

Each family also carries the vendor and class predicate upstream uses, since those
are part of exact matching (see `SPEC` and `CLASS_TEXT`), plus the hook-table and
engine-token statuses measured by `probe_haiku_display_features.py`, which this tool
imports rather than re-deriving, so the two cannot drift apart.

    python3 tools/research/gen_gpu_tables.py --haiku DIR \\
        --out kernel/drivers/gpu/gpu_ids.h
    python3 tools/research/gen_gpu_tables.py --haiku DIR \\
        --out kernel/drivers/gpu/gpu_ids.h --check     # verify the tree, write nothing

A row in the generated header is a matching rule, never a claim that SCos drives
that chip; `kernel/drivers/gpu/gpu_drivers.c` records which families have a ported
engine and what happens to the rest.
"""
import argparse
import importlib.util
import json
import os
import re
import subprocess
import sys

PIN = '7be0fef07df0ecbe6f40a4cf2a7687775f1f28a0'

# family -> upstream binding table.  `vendors` is either `define:<SYMBOL>` (one
# `#define` in the file) or `pairs:<ARRAY>` (the file's `{vendor, devices}` array,
# which pairs several rebranded vendors with their own device lists).  `cls` is the
# class-code predicate the family's own enumeration loop applies, if any.
SPEC = {
    'radeon': dict(file='src/add-ons/kernel/drivers/graphics/radeon/detect.c',
                   arrays=['radeon_device_list'], vendors='define:VENDOR_ID_ATI',
                   cls='none'),
    'nvidia': dict(file='src/add-ons/kernel/drivers/graphics/nvidia/driver.c',
                   arrays=['nvidia_device_list', 'elsa_device_list',
                           'nvstbsgs_device_list', 'varisys_device_list'],
                   vendors='pairs:SupportedDevices', cls='none'),
    'matrox': dict(file='src/add-ons/kernel/drivers/graphics/matrox/driver.c',
                   arrays=['gx00_device_list'], vendors='pairs:SupportedDevices',
                   cls='none'),
    'via': dict(file='src/add-ons/kernel/drivers/graphics/via/driver.c',
                arrays=['via_device_list'], vendors='pairs:SupportedDevices',
                cls='none'),
    'neomagic': dict(file='src/add-ons/kernel/drivers/graphics/neomagic/driver.c',
                     arrays=['nm_device_list'], vendors='pairs:SupportedDevices',
                     cls='none'),
    'et6x00': dict(file='src/add-ons/kernel/drivers/graphics/et6x00/driver.c',
                   arrays=['et6000DeviceList'], vendors='pairs:supportedDevices',
                   cls='none'),
    's3': dict(file='src/add-ons/kernel/drivers/graphics/s3/driver.cpp',
               arrays=['chipTable'], vendors='define:VENDOR_ID', cls='none'),
    'ati': dict(file='src/add-ons/kernel/drivers/graphics/ati/driver.cpp',
                arrays=['chipTable'], vendors='define:VENDOR_ID', cls='none'),
    '3dfx': dict(file='src/add-ons/kernel/drivers/graphics/3dfx/driver.cpp',
                 arrays=['chipTable'], vendors='define:VENDOR_ID', cls='none'),
    'intel_810': dict(file='src/add-ons/kernel/drivers/graphics/intel_810/driver.cpp',
                      arrays=['chipTable'], vendors='define:VENDOR_ID', cls='none'),
    'intel_extreme': dict(
        file='src/add-ons/kernel/drivers/graphics/intel_extreme/driver.cpp',
        arrays=['kSupportedDevices'], vendors='define:VENDOR_ID_INTEL',
        cls='vga_or_other'),
    'radeon_hd': dict(file='src/add-ons/kernel/drivers/graphics/radeon_hd/driver.cpp',
                      arrays=['kSupportedDevices'], vendors='define:VENDOR_ID_ATI',
                      cls='vga'),
}
# Families with no PCI ID table at all upstream: VBE, a firmware-provided linear
# framebuffer and the virtio transport.  They still get measured, because "this
# family cannot accelerate" is a result the kernel has to report rather than guess.
CLASS_ONLY = {
    'vesa': 'src/add-ons/accelerants/vesa',
    'framebuffer': 'src/add-ons/accelerants/framebuffer',
    'virtio': 'src/add-ons/accelerants/virtio',
}
CLASS_TEXT = {
    'none': 'no class test upstream (vendor + device only)',
    'vga': 'class 0x03 sub 0x00 only',
    'vga_or_other': 'class 0x03 sub 0x00 or 0x80',
}
CLASS_FIELDS = {
    'none': '0xff, 0xff, 0xff',
    'vga': '0x03, 0x00, 0xff',
    'vga_or_other': '0x03, 0x00, 0x80',
}
FAMILIES = list(SPEC) + list(CLASS_ONLY)
# Bit order of the generated capability record; kernel/include/gpu.h declares the
# same names in the same order, and the test asserts they line up.
BITS = [('engine2d', 'token'), ('vsync', 'vsync_retrace'), ('pan', 'pan_display'),
        ('cursor', 'hardware_cursor_shape'), ('overlay', 'overlay_allocate'),
        ('fill', 'fill_rectangle'), ('blit', 'screen_to_screen_blit'),
        ('span', 'fill_span'), ('modeset', 'set_display_mode'), ('dpms', 'dpms')]
LABEL = {'engine2d': 'engine', 'vsync': 'retrace', 'pan': 'pan', 'cursor': 'cursor',
         'overlay': 'overlay', 'fill': 'fill_rect', 'blit': 'screen_blit',
         'span': 'fill_span', 'modeset': 'set_mode', 'dpms': 'dpms'}
NAME_VAR = re.compile(r'static\s+(?:const\s+)?char\s*\*?\s*(\w+)\s*(?:\[\s*\])?\s*=\s*"([^"]*)"')
ROW = re.compile(r'\{\s*(0x[0-9a-fA-F]{1,4}|\w+)\s*,([^\n]*?)\}')
NAME_FROM_COMMENT = re.compile(r'/\*\s*(.*?)\s*\*/')
DEFINE_HEX = re.compile(r'#define\s+(\w+)[^\n]*?0x([0-9a-fA-F]{1,4})')
PAIR = re.compile(r'\{\s*([A-Za-z_]\w*|0x[0-9a-fA-F]{1,4})\s*,\s*(\w+)\s*\}')
SEARCH_SUBDIRS = ['src/add-ons/kernel/drivers/graphics/{}',
                  'src/add-ons/accelerants/{}', 'headers/private/graphics/{}']


def head_of(root):
    p = subprocess.run(['git', '-C', root, 'rev-parse', 'HEAD'],
                       capture_output=True, text=True)
    return p.stdout.strip() if p.returncode == 0 else 'not a git checkout'


def read(root, rel):
    with open(os.path.join(root, rel), 'rb') as fh:
        return fh.read().decode('utf-8', 'replace')


def load_probe():
    """Reuse the probe's measurement code rather than duplicating it here."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        'probe_haiku_display_features.py')
    spec = importlib.util.spec_from_file_location('probe_haiku', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def active_lines(text):
    """(raw, code, enabled) per line, with block comments inlined and `#if 0` tracked.

    `//...` is dropped before a line is examined, so a commented-out row cannot be
    mistaken for support; `/* ... */` is preserved for the name extraction and
    dropped for the structural test, since in the raw-list form the chip name lives
    in such a comment.
    """
    out, on = [], True
    for raw in text.split('\n'):
        line = raw
        code = re.sub(r'/\*.*?\*/', ' ', line).split('//')[0]
        stripped = code.strip()
        if stripped.startswith('#if 0'):
            on = False
            continue
        if stripped.startswith('#endif') and not on:
            on = True
            continue
        out.append((line, code, on))
    return out


def family_source_paths(root, family):
    for pattern in SEARCH_SUBDIRS:
        base = os.path.join(root, pattern.format(family))
        if not os.path.isdir(base):
            continue
        for dirpath, _, filenames in os.walk(base):
            for name in filenames:
                if name.endswith(('.h', '.c', '.cpp')):
                    yield os.path.join(dirpath, name)


def family_macros(root, family):
    """`#define`s visible to the driver: its own dirs plus its private header set.

    Vendor symbols often live in the family's private header (`VENDOR_ID_INTEL` is
    at `headers/private/graphics/intel_extreme/intel_extreme.h:22`), so resolving a
    symbol only inside the table's own file is not enough.
    """
    macros = {}
    for pattern in SEARCH_SUBDIRS:
        base = os.path.join(root, pattern.format(family))
        if not os.path.isdir(base):
            continue
        for path in family_source_paths(root, family):
            macros.update(macro_values(read(root, path)))
    return macros


def macro_values(text):
    return {name: int(value, 16) for name, value in DEFINE_HEX.findall(text)}


def array_body(lines, name):
    """The lines of `name[] = { ... };`, plus the file line number where it opens."""
    body, depth, started, opened_at = [], 0, False, 0
    for number, (line, code, on) in enumerate(lines, start=1):
        if not started:
            match = re.search(r'\b' + re.escape(name) + r'\s*\[\s*\]\s*=\s*\{', code)
            if not match:
                continue
            started, opened_at = True, number
            # The opening line is usually `} name[] = {`, where the leading brace
            # closes the struct definition; counting it would make the array body
            # look closed again on the very first line.
            code = code[match.end() - 1:]
        depth += code.count('{') - code.count('}')
        if on:
            body.append(line)
        if started and depth <= 0:
            break
    return None if not started else (body, opened_at)


def names_from_comment(line):
    found = NAME_FROM_COMMENT.findall(line)
    return ' '.join(x.strip() for x in found if x.strip()) or None


def extract(root, family):
    """[(vendor, device, name, source_line)] exactly as the driver's loop would match."""
    spec = SPEC[family]
    text = read(root, spec['file'])
    lines = active_lines(text)
    macros = family_macros(root, family)
    macros.update(macro_values(text))
    # ati and a few others name their chips through `static char name[] = "..."`
    # variables referenced by the row, so a row can carry no string literal at all
    # and still name a chip.  Resolve those the same way the compiler does.
    name_vars = {}
    for path in family_source_paths(root, family):
        name_vars.update(NAME_VAR.findall(read(root, path)))
    name_vars.update(NAME_VAR.findall(text))
    if spec['vendors'].startswith('define:'):
        symbol = spec['vendors'].split(':', 1)[1]
        vendor_by_list = {None: macros.get(symbol)}
        if vendor_by_list[None] is None:
            raise SystemExit(f'{family}: #define {symbol} not found in {spec["file"]}')
    else:
        pair_name = spec['vendors'].split(':', 1)[1]
        pair = array_body(lines, pair_name)[0]
        if pair is None:
            raise SystemExit(f'{family}: vendor pairing array {pair_name}[] not found')
        vendor_by_list = {}
        for line in pair:
            code = re.sub(r'/\*.*?\*/', ' ', line).split('//')[0]
            match = PAIR.search(code)
            if not match:
                continue
            vendor, listing = match.group(1), match.group(2)
            value = int(vendor, 16) if vendor.startswith('0x') else macros.get(vendor)
            if value is None:
                raise SystemExit(f'{family}: cannot resolve vendor symbol {vendor}')
            vendor_by_list[listing] = value
    entries, seen, collapsed = [], {}, []
    for name in spec['arrays']:
        got = array_body(lines, name)
        if got is None:
            raise SystemExit(f'{family}: array {name}[] not found in {spec["file"]}')
        body, opened_at = got
        vendor = vendor_by_list.get(name, vendor_by_list.get(None))
        if vendor is None:
            raise SystemExit(f'{family}: no vendor resolves for list {name}')
        found = 0
        for offset, line in enumerate(body):
            code = re.sub(r'/\*.*?\*/', ' ', line).split('//')[0].strip()
            if not code or code.startswith('#') or code in ('{', '}', '};', '},'):
                continue
            device, label = None, None
            if code.startswith('{'):
                match = re.match(r'\{\s*(0x[0-9a-fA-F]{1,4}|\w+)\s*,(.*)$', code)
                if not match:
                    continue
                token, rest = match.group(1), match.group(2)
                device = int(token, 16) if token.startswith('0x') else macros.get(token)
                quoted = re.findall(r'"([^"]*)"', rest)
                label = quoted[0] if quoted else None
                if label is None:
                    tail = [x.strip() for x in rest.split(',') if x.strip()]
                    if tail:
                        label = name_vars.get(re.sub(r'[^A-Za-z0-9_]', '', tail[-1]))
            else:
                match = re.match(r'(0x[0-9a-fA-F]{1,4})\s*,?', code)
                if match:
                    device = int(match.group(1), 16)
                    label = names_from_comment(line)
            if not device:                      # the `{ 0, 0, NULL }` terminator
                continue
            found += 1
            where = f'{spec["file"]}:{opened_at + offset}'
            key = (vendor, device)
            if key in seen:
                # Upstream keeps several rows for one ID when the chips differ in
                # flags (radeon 0x554d/0x554f/0x5b62); for *matching* they collapse.
                collapsed.append(f'{name} {hex(device)}')
                continue
            seen[key] = where
            entries.append((vendor, device, label, where))
        if not found:
            raise SystemExit(f'{family}: no rows parsed out of {name}[]')
    return entries, collapsed


def measure(root, family, probe):
    """Hook-table and engine-token statuses for one family, as measured text."""
    files = probe.family_files(root, family)
    hooks = probe.hooks(root, files)
    token = probe.engine_token(root, files)
    found = (hooks or {}).get('found') or {}
    bits, detail = {}, []
    for name, key in BITS:
        if key == 'token':
            value = bool(token and token.get('advertises_2d'))
            bits[name] = 1 if value else 0
            shown = 'advertises B_2D_ACCELERATION' if value else 'no 2D capability bit'
            where = f' ({token["file"]}:{token["line"]})' if token else \
                    ' (family has no engine source)'
            detail.append(f'{LABEL[name]}:{shown}{where}')
            continue
        entry = found.get(key)
        # `direct` means the hook is handed out unconditionally; the other statuses
        # the probe distinguishes (gated, denied, absent, disabled-by-comment) all
        # mean a port could not rely on the feature as-is, so the bit stays clear
        # and the wording is preserved verbatim in `detail`.
        status = entry['status'] if entry else 'absent'
        bits[name] = 1 if status == 'direct' else 0
        detail.append(f'{LABEL[name]}:{status}')
    return bits, detail


def wrap_literal(text, width):
    """A C string literal split into adjacent pieces, so long lines stay long-proof."""
    pieces = [text[i:i + width] for i in range(0, len(text), width)] or ['']
    return '\n'.join(' ' * 8 + json.dumps(piece) for piece in pieces).lstrip()


def sanitize(name, limit=58):
    if not name:
        return None
    name = ''.join(ch if 32 <= ord(ch) < 127 else ' ' for ch in name)
    name = ' '.join(name.split()).replace('\\', '').replace('"', "'")
    return name[:limit] or None


def cstr(text):
    return json.dumps(text if text is not None else '')


def emit(root, pin):
    probe = load_probe()
    out = [
        '/* Generated by tools/research/gen_gpu_tables.py - do not edit by hand.\n',
        ' * Source: Haiku ' + pin + ', per-family PCI binding tables and the\n',
        ' * accelerant hook tables next to them.  Rows a line comment or an `#if 0`\n',
        ' * block excludes are absent,\n',
        ' * macro-valued rows are resolved, and rebranded vendors keep their own list.\n',
        ' * Regenerate with:\n',
        ' *   python3 tools/research/gen_gpu_tables.py --haiku DIR \\\n',
        ' *       --out kernel/drivers/gpu/gpu_ids.h\n',
        ' * and verify with `--check`.  A row here is a matching rule, not a claim that\n',
        ' * SCos drives that chip: see gpu_drivers.c for what is actually ported.\n',
        ' */\n',
        '#ifndef SCOS_GPU_IDS_H\n#define SCOS_GPU_IDS_H\n\n',
    ]
    total, rows = 0, []
    for family in FAMILIES:
        if family in SPEC:
            entries, collapsed = extract(root, family)
            ident = re.sub(r'\W', '_', family)
            source, spec = SPEC[family]['file'], SPEC[family]
            cls = CLASS_FIELDS[spec['cls']]
            vendor = entries[0][0]
            out.append(f'/* {family}: {len(entries)} device IDs bound by '
                       f'{source}; {CLASS_TEXT[spec["cls"]]}. */\n')
            out.append(f'static const struct gpu_pci_id gpu_ids_{ident}[] = {{\n')
            for vendor_id, device, name, where in entries:
                # Keep provenance short enough that the table stays readable: the
                # directory is the same for the whole family and is in `source`.
                short = where.split('/')[-1] if '/' in where else where
                out.append(f'    {{{hex(vendor_id)}, {hex(device)}, '
                           f'{cstr(sanitize(name))}}}, /* {short} */\n')
            out.append('};\n\n')
            total += len(entries)
            notes = (f'{len(collapsed)} duplicate rows collapsed' if collapsed else None)
        else:
            ident = family
            entries, notes, source, cls, vendor = [], None, CLASS_ONLY[family], \
                '0xff, 0xff, 0xff', 0
        bits, detail = measure(root, family, probe)
        rows.append(dict(family=family, ident=re.sub(r'\W', '_', family),
                         vendor=vendor, cls=cls, count=len(entries),
                         bits=bits, detail=detail, source=source, note=notes))
    out.append('/* Per-family match records.  `.upstream` is what the family\'s hook table\n'
               ' * hands out today in that checkout; it is a capability *inventory*, not a\n'
               ' * statement that SCos has ported the code that uses it. */\n'
               'static const struct gpu_match gpu_match_table[] = {\n')
    for row in rows:
        names = [name for name, _ in BITS]
        caps = ',\n        '.join(
            ', '.join(f'.{name} = {row["bits"][name]}' for name in names[i:i + 5])
            for i in range(0, len(names), 5))
        detail = '; '.join(row['detail'])
        note = row['note'] or ('no PCI ID table: binds by class or firmware'
                              if row['count'] == 0 else None)
        out.append('    {\n')
        out.append(f'        .family = {cstr(row["family"])}, '
                   f'.primary_vendor = {hex(row["vendor"])},\n')
        out.append(f'        .class_base = {row["cls"].split(", ")[0]}, '
                   f'.class_sub_a = {row["cls"].split(", ")[1]}, '
                   f'.class_sub_b = {row["cls"].split(", ")[2]},\n')
        out.append(f'        .ids = {"gpu_ids_" + row["ident"] if row["count"] else "NULL"},'
                   f' .id_count = {row["count"]}u,\n')
        out.append(f'        .upstream = {{\n        {caps},\n        }},\n')
        features = sanitize('; '.join(row['detail']), 400)
        out.append('        .features = ' + wrap_literal(features, 92) + ',\n')
        out.append(f'        .source = {cstr(row["source"])},\n')
        if note:
            out.append(f'        .note = {cstr(note)},\n')
        out.append('    },\n')
    out.append('};\n\n')
    out.append(f'#define GPU_MATCH_COUNT {len(rows)}u\n')
    out.append(f'#define GPU_ID_TOTAL {total}u\n')
    out.append('\n#endif\n')
    return ''.join(out), total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--haiku', required=True)
    ap.add_argument('--out')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--allow-head', action='store_true')
    args = ap.parse_args()
    head = head_of(args.haiku)
    if head != PIN and not args.allow_head:
        raise SystemExit(f'Haiku checkout is at {head}, expected {PIN} '
                         f'(pass --allow-head to measure anyway)')
    text, total = emit(args.haiku, head)
    if args.check:
        if not args.out or not os.path.exists(args.out):
            raise SystemExit('--check needs --out pointing at an existing header')
        current = open(args.out).read()
        same = current == text
        print(('PASS: ' if same else 'FAIL: ') +
              f'{args.out} matches a fresh extraction from {head[:12]} '
              f'({total} device IDs across {len(FAMILIES)} families)')
        if not same:
            import difflib
            diff = list(difflib.unified_diff(current.split('\n'), text.split('\n'),
                                             'in tree', 'generated', n=1, lineterm=''))
            print('\n'.join(diff[:40]) or '(no textual difference)')
        return 0 if same else 1
    if not args.out:
        sys.stdout.write(text)
        return 0
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    old = open(args.out).read() if os.path.exists(args.out) else None
    with open(args.out, 'w') as handle:
        handle.write(text)
    print(f'wrote {args.out}: {len(text)} bytes, {total} device IDs, '
          f'{len(FAMILIES)} families, {"unchanged" if old == text else "changed"}')
    return 0


if __name__ == '__main__':
    sys.exit(main())

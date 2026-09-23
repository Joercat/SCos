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
from pathlib import Path
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


# --------------------------------------------------------------- naming rows (separate generated file)

# A family's own binding table in gpu_ids.h says which chips that upstream driver claims.  That is a
# promise a ported engine may be asked to keep, so it is never widened from a registry.  What the PCI
# id registry does give is the NAME of a chip no driver in this tree covers: a 2025 GPU should be
# reported as "GB207 [GeForce RTX 5050] (Blackwell), nothing in tree drives this generation" rather
# than as an unknown device.  Those rows go to their own generated header, are matched by a separate
# pass, and can never select a module: gpu_match_device() marks them, and both the boot stub and
# gpu_module.c refuse to load anything for them.
REGISTRY_SNAPSHOT = 'tools/research/pci.ids.display.txt'
REGISTRY_VENDORS = {'nvidia': (0x10de,), 'radeon_hd': (0x1002, 0x1022), 'intel_extreme': (0x8086,)}
# The registry's leading codename maps to a generation, worth carrying in the name because the generation
# is what decides whether any driver could exist at all.  Unknown codenames get no suffix, never a guess.
NV_ARCH = (('GF1', 'Fermi'), ('GK', 'Kepler'), ('GM', 'Maxwell'), ('GP', 'Pascal'), ('GV', 'Volta'),
           ('TU', 'Turing'), ('GA', 'Ampere'), ('AD', 'Ada'), ('GH', 'Hopper'), ('GB', 'Blackwell'),
           ('GR1', 'Rubin'))
SNAPSHOT_ROW = re.compile(r'^([0-9a-fA-F]{4})\t([0-9a-fA-F]{4})\t(.+?)\s*$')
# Keep a display product, drop a companion function: without the drop list the registry's
# "GB202 High Definition Audio Controller" would land in a display table.
SNAPSHOT_FILTER = {
    0x10de: (r'GeForce|Quadro|Tesla|NVS |RIVA|\bTNT\b|Vanta|nForce',
             r'Audio|HDMI|USB|SATA|IDE|SMBus|LPC|LAN|Ethernet|Bridge|NVMe|TPM|Modem|1394|xHCI|EHCI'),
    0x1002: (r'Radeon|FireGL|FirePro|FireMV|Rage|Wonder|Mach|All-In',
             r'Audio|HDMI|USB|SATA|IDE|Bridge|ETH|1394|SMBus|FP64|IOMMU|Fabric|XHCI'),
    0x1022: (r'Radeon|Instinct|Graphic', r'Audio|HDMI|USB|SATA|Bridge|Fabric|IOMMU|GMEM|NBIO|SMU|XHCI|PCI'),
    0x8086: (r'Graphics|Arc |Iris|UHD Graphics|HD Graphics|GMA|Extreme Graphics|i740|iAGP|Ironlake',
             r'Audio|HDMI|USB|SATA|IDE|Bridge|LAN|Ethernet|Thunderbolt|TBT|xHCI|EHCI|SMBus|DRAM|WiFi|AX2|CNVi'),
}


def nvidia_architecture(name):
    head = name.split(' [')[0].split('(')[0].strip()
    for prefix, arch in NV_ARCH:
        if head.startswith(prefix):
            return arch
    return None


def read_snapshot():
    """The in-tree naming snapshot: `vendor<TAB>device<TAB>name` rows plus its provenance comments.
    Keeping the rows in the tree is what makes --check-work-for-registry reproducible without the
    1.7 MB upstream file or network access."""
    rows, prov = [], []
    with open(REGISTRY_SNAPSHOT) as fh:
        for line in fh.read().splitlines():
            if line.startswith('#'):
                prov.append(line[2:] if line.startswith('# ') else line[1:])
                continue
            m = SNAPSHOT_ROW.match(line)
            if m:
                rows.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3)))
    return rows, prov


def extract_snapshot(src):
    """Rebuild the in-tree snapshot from a real pci.ids.  The only step that needs the upstream file,
    and it is run by hand every couple of years, never by the build."""
    with open(src, errors='replace') as fh:
        lines = fh.read().splitlines()
    vendor = re.compile(r'^([0-9a-fA-F]{4})  (\S.*)$')
    device = re.compile(r'^\t([0-9a-fA-F]{4})  (\S.*?)\s*$')
    out, current, version = [], None, ''
    for line in lines:
        if not line.startswith('\t'):
            m = vendor.match(line)
            if m:
                hid = m.group(1)
                current = int(hid, 16) if hid in ('10de', '1002', '1022', '8086') else None
            elif line.startswith('#\tVersion:'):
                version = line.split(':', 1)[1].strip()
            elif line.startswith('#\tDate:'):
                version += ' (dated ' + line.split(':', 1)[1].strip() + ')'
            continue
        if current is None:
            continue
        m = device.match(line)
        if not m:
            continue
        keep, drop = SNAPSHOT_FILTER[current]
        name = m.group(2)
        if re.search(keep, name, re.I) and not re.search(drop, name, re.I):
            out.append((current, int(m.group(1), 16), name))
    seen, rows = set(), []
    for v, d, n in sorted(out):
        if (v, d) in seen:
            continue
        seen.add((v, d))
        rows.append('%04x\t%04x\t%s' % (v, d, n))
    head = ('# PCI id registry snapshot: display-product device ids, used only to NAME a chip.\n'
            f'# Source: pci.ids (PCI ID Project), version {version}.\n'
            '#   Upstream: https://github.com/pciutils/pciids - licence GPL-2.0-or-later OR BSD-3-Clause.\n'
            '#   A dev-time input to tools/research/gen_gpu_tables.py: nothing here is compiled, and no\n'
            '#   row here can select or vouch for a driver.\n'
            '# Columns: vendor<TAB>device<TAB>name, sorted; companion functions (audio, bridges, USB,\n'
            '#   SATA, network) are filtered out so that every row names a display controller.\n')
    with open(REGISTRY_SNAPSHOT, 'w') as fh:
        fh.write(head + '\n'.join(rows) + '\n')
    return len(rows), version


def driver_pairs(ids_path):
    """Every (vendor, device) already bound by gpu_ids.h, read off the generated file itself, so
    registry mode needs no Haiku checkout and cannot contradict a driver table."""
    row = re.compile(r'^\s*\{0x([0-9a-fA-F]{1,4}), 0x([0-9a-fA-F]{1,4}), ')
    out = set()
    with open(ids_path) as fh:
        for line in fh:
            m = row.match(line)
            if m:
                out.add((int(m.group(1), 16), int(m.group(2), 16)))
    return out


def emit_registry(taken):
    """One row per distinct chip *name*, at the lowest id that carries it.  The registry lists a product
    under several ids - desktop, laptop, a refresh, an OEM SKU - and a table whose job is to answer
    "what is this thing" does not need five rows that all say GB207M [GeForce RTX 5050 Max-Q]: each
    kept row costs image space, and the dropped ones are ids nobody can look up.  Unknown and reserved
    placeholders are dropped outright, since naming a chip 'Reserved Dev ID B' would be a fake."""
    by_family = {}
    placeholder = re.compile(r'Reserved|Unidentified|Unknown|Device ID|not specified', re.I)
    named = {}
    for vendor, device, name in read_snapshot()[0]:
        if placeholder.search(name):
            continue
        key = (vendor, name)
        if key not in named or device < named[key]:
            named[key] = device
    for vendor, device, name in read_snapshot()[0]:
        if (vendor, device) in taken or named.get((vendor, name)) != device:
            continue                     # bound by a driver table already, or a duplicate id for a name
        for family, vendors in REGISTRY_VENDORS.items():
            if vendor in vendors:
                if family == 'nvidia':
                    arch = nvidia_architecture(name)
                    if arch:
                        name = name + ' (' + arch + ')'
                by_family.setdefault(family, []).append((vendor, device, name))
                break
    for family in by_family:
        by_family[family].sort()
    prov = [l for l in read_snapshot()[1] if l.startswith('Source:')]
    out = ['/* Generated by tools/research/gen_gpu_tables.py --registry-only - do not edit by hand.\n',
           ' *\n',
           ' * Chip NAMES only, from tools/research/pci.ids.display.txt.  These rows exist so that a\n'
           ' * machine can be told what it has when no driver in this tree covers it, instead of being\n'
           ' * told "unknown device".  A row here never selects a module and never implies\n'
           ' * acceleration: gpu_match_device() reports it separately, and the boot stub and\n'
           ' * gpu_module.c both refuse to load a driver for it.  The rows that bind a driver live in\n'
           ' * gpu_ids.h, generated from the pinned upstream driver sources; no id appears in both.\n']
    for line in prov:
        out.append(' * ' + line + '\n')
    out += [' *\n',
            ' * Refresh: gen_gpu_tables.py --extract-pci-ids <pci.ids>, then --registry-only.\n',
            ' */\n',
            '#ifndef SCOS_GPU_IDS_REGISTRY_H\n#define SCOS_GPU_IDS_REGISTRY_H\n\n',
            '/* Included only by kernel/drivers/gpu/gpu_match.c, which owns both generated tables: these\n',
            ' * are `static` arrays, so exactly one translation unit may include this file. */\n\n']
    total, tables = 0, []
    for family in sorted(by_family):
        ident = re.sub(r'\W', '_', family)
        entries = by_family[family]
        total += len(entries)
        tables.append((family, ident, len(entries)))
        out.append(f'/* {family}: {len(entries)} display ids named by the registry and bound by no\n'
                   f'   driver table in the tree - known chips with nothing ported for them. */\n')
        out.append(f'static const struct gpu_pci_id gpu_registry_{ident}[] = {{\n')
        for vendor, device, name in entries:
            out.append(f'    {{{hex(vendor)}, {hex(device)}, {cstr(sanitize(name))}}}, /* pci.ids */\n')
        out.append('};\n\n')
    out.append('struct gpu_registry_table {\n')
    out.append('    const char *family;\n')
    out.append('    const struct gpu_pci_id *ids;\n')
    out.append('    uint16_t count;\n')
    out.append('};\n\n')
    out.append('static const struct gpu_registry_table gpu_registry_table[] = {\n')
    for family, ident, count in tables:
        out.append(f'    {{{cstr(family)}, gpu_registry_{ident}, {count}u}},\n')
    out.append('};\n\n')
    out.append(f'#define GPU_REGISTRY_FAMILIES {len(tables)}u\n')
    out.append(f'#define GPU_REGISTRY_ID_TOTAL {total}u\n\n#endif\n')
    # The printed number is the number written: a summary one greater than the header it just produced is
    # how a table's count stops being trustworthy, and `--registry-only' is what a person reads after a
    # refresh, so the two must agree exactly.
    return ''.join(out), total


def run_registry(out_path, check):
    text, total = emit_registry(driver_pairs(os.path.join(
        os.path.dirname(os.path.abspath(out_path)), 'gpu_ids.h')))
    if check:
        old = open(out_path).read() if os.path.exists(out_path) else ''
        if old != text:
            raise SystemExit('gpu_ids_registry.h is stale: run '
                             'gen_gpu_tables.py --registry-only --out kernel/drivers/gpu/gpu_ids_registry.h')
        print(f'PASS: gpu_ids_registry.h matches the in-tree registry snapshot ({total} names)')
        return 0
    with open(out_path, 'w') as fh:
        fh.write(text)
    print(f'wrote {out_path}: {total} naming rows')
    return 0


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


# --------------------------------------------------------------------- local ----
# One record in the generated header is not generated from upstream sources.  The Cirrus CL-GD5446 has
# no 2D driver in the tree these tables are read from, so its id row and capability record describe
# SCos' own module (drivers/gpu/cirrus) and are authored here, deliberately inside the generator: a
# hand-edited section in a generated file is the kind of thing the next regeneration silently deletes,
# and the family would then load as "no driver for this chip" with nothing failing loudly.  `--check-local`
# proves the header still contains exactly this text, and it is checked without a Haiku checkout so the
# invariant holds on any machine.
LOCAL_FAMILY = 'cirrus'

# Wording of the note a family record carries when SCos added rows to its upstream id list.  Distinct
# from the local *record* note on purpose: one says rows were added to a generated family, the other
# says the whole record is local, and the test harness counts the two separately.
NOTE_NEW = 'id rows added by SCos (Blackwell), not from the upstream table'

LOCAL_IDS_TEXT = '''/* The one local record, described above the generator's own provenance header: this family is
 * not read out of an upstream driver.  The id row is what the module claims and the loader
 * cross-checks; deleting it here stops drivers/gpu/cirrus from loading at all rather than leaving it
 * unverified. */
static const struct gpu_pci_id gpu_ids_cirrus[] = {
    {0x1013, 0x00B8, "GD 5446 (PCI)"},        /* the last Cirrus with a bitBLT engine */
};

'''

# A second kind of local content: hand-authored *rows inside a family the upstream table already owns*.
# The record above had to be created from nothing because no upstream driver knows the CL-GD5446's
# bitBLT; this one is different - the `nvidia` family record and its 249 ids come from Haiku's binding
# table, and it stops before Blackwell, because that table was written while those chips were not out.
# So only ids are added, not capabilities: every row below is a device the PCI ID Project names in
# tools/research/pci.ids.display.txt, the same snapshot the registry is generated from, and the row moves
# a chip from "named, nothing binds it" to "bound to drivers/gpu/nvidia", which reads that chip's own
# boot register and offers no engine.  `run_registry' reads this header back to decide what the registry
# may repeat (driver_pairs), so an id cannot end up in both tables, and --check-local below proves each
# row is in the committed header exactly once.
LOCAL_EXTRA_ROWS = {
    'nvidia': ("""    /* Hand-authored by SCos, and the reason the count above is larger than the upstream family's:
     * the Blackwell display ids the in-tree registry named while nothing bound them.  A row here is a
     * matching rule that lets drivers/gpu/nvidia load for this chip and read NV_PMC_BOOT_0; it is not a
     * claim that anything can be drawn, which is what the capability record below and gpu_ports.c say. */
    {0x10DE, 0x2B85, "GB202 [GeForce RTX 5090]"},        /* Blackwell, identification only */
    {0x10DE, 0x2B87, "GB202 [GeForce RTX 5090 D]"},
    {0x10DE, 0x2B8C, "GB202 [GeForce RTX 5090 D V2]"},
    {0x10DE, 0x2C02, "GB203 [GeForce RTX 5080]"},
    {0x10DE, 0x2C05, "GB203 [GeForce RTX 5070 Ti]"},
    {0x10DE, 0x2C09, "GB203 [GeForce RTX 5070]"},
    {0x10DE, 0x2C18, "GB203M [GeForce RTX 5090 Max-Q / Mobile]"},
    {0x10DE, 0x2C19, "GB203M [GeForce RTX 5080 Max-Q / Mobile]"},
    {0x10DE, 0x2C58, "GB203M-X11 [GeForce RTX 5090 Max-Q / Mobile]"},
    {0x10DE, 0x2C59, "GB203M-X9 [GeForce RTX 5080 Max-Q / Mobile]"},
    {0x10DE, 0x2D04, "GB206 [GeForce RTX 5060 Ti]"},
    {0x10DE, 0x2D05, "GB206 [GeForce RTX 5060]"},
    {0x10DE, 0x2D18, "GB206M [GeForce RTX 5070 Max-Q / Mobile]"},
    {0x10DE, 0x2D19, "GB206M [GeForce RTX 5060 Max-Q / Mobile]"},
    {0x10DE, 0x2D83, "GB207 [GeForce RTX 5050]"},
    {0x10DE, 0x2D98, "GB207M [GeForce RTX 5050 Max-Q / Mobile]"},
    {0x10DE, 0x2F04, "GB205 [GeForce RTX 5070]"},
    {0x10DE, 0x2F06, "GB205 [GeForce RTX 5060]"},
    {0x10DE, 0x2F18, "GB205M [GeForce RTX 5070 Ti Mobile]"},
""", 19),
}

LOCAL_RECORD_TEXT = '''    {
        .family = "cirrus", .primary_vendor = 0x1013,
        .class_base = 0x03, .class_sub_a = 0x00, .class_sub_b = 0xff,
        .ids = gpu_ids_cirrus, .id_count = 1u,
        .upstream = {
        .engine2d = 1, .vsync = 0, .pan = 0, .cursor = 0, .overlay = 0,
        .fill = 1, .blit = 1, .span = 0, .modeset = 0, .dpms = 0,
        },
        .features = "engine:SCos-authored bitBLT driver, device-verified; retrace:absent; pan:absent;"
        " cursor:absent; overlay:absent; fill_rect:direct; screen_blit:direct; fill_span:absent; set_"
        "mode:absent; dpms:absent",
        .source = "drivers/gpu/cirrus (no upstream driver; verified against QEMU's CL-GD5446 model)",
        .note = "hand-authored id row and capability record, not generated from an upstream table",
    },
'''

# The capability flags above are what drivers/gpu/cirrus/module.c implements today, not what a
# CL-GD5446 is theoretically capable of: the module fills and copies, so fill and blit are 1 and
# cursor, overlay, span, modeset and DPMS are 0.  `features` follows the same rule.

LOCAL_COUNTS = ('/* GPU_MATCH_COUNT and GPU_ID_TOTAL include the hand-authored {family} record: {families} families, '
                'and\n * {ids} exact ids that bind a driver ({upstream} from upstream tables, the one local row, '
                'and the\n * {extra} rows SCos added inside {extra_families} upstream family array). */\n')


def check_local(path):
    """Verify the committed header carries the local section, byte for byte, and that its counts agree."""
    text = Path(path).read_text()
    problems = []
    if LOCAL_IDS_TEXT not in text:
        problems.append('the local id array in the header does not match LOCAL_IDS_TEXT')
    if LOCAL_RECORD_TEXT not in text:
        problems.append('the local family record in the header does not match LOCAL_RECORD_TEXT')
    for family, (rows_text, count) in LOCAL_EXTRA_ROWS.items():
        if rows_text not in text:
            problems.append(f'the hand-authored id rows for the {family} family are not in the header')
        for vendor, device in re.findall(r'^    \{0x([0-9A-Fa-f]{1,4}), 0x([0-9A-Fa-f]{1,4}),',
                                         rows_text, re.M):
            hits = len(re.findall(r'^    \{0x%s, 0x%s,' % (vendor, device), text, re.M | re.I))
            if hits != 1:
                problems.append(f'id 0x{vendor}:0x{device} appears {hits} times in the header, '
                                f'expected exactly once')
    if text.count('gpu_ids_cirrus[]') != 1:
        problems.append('gpu_ids_cirrus is defined %d times' % text.count('gpu_ids_cirrus[]'))
    families = text.count('        .family = ')
    ids = len(re.findall(r'^    \{0x[0-9A-Fa-f]+, 0x[0-9A-Fa-f]+,', text, re.M))
    declared = dict(re.findall(r'#define (GPU_MATCH_COUNT|GPU_ID_TOTAL) (\d+)u', text))
    if int(declared.get('GPU_MATCH_COUNT', -1)) != families:
        problems.append(f'GPU_MATCH_COUNT says {declared.get("GPU_MATCH_COUNT")} families, '
                        f'the table holds {families}')
    if int(declared.get('GPU_ID_TOTAL', -1)) != ids:
        problems.append(f'GPU_ID_TOTAL says {declared.get("GPU_ID_TOTAL")} ids, the table holds {ids}')
    print(('PASS: ' if not problems else 'FAIL: ') +
          f'{path} holds the local {LOCAL_FAMILY} record and agrees with its own counts '
          f'({families} families, {ids} ids)')
    for line in problems:
        print('  ' + line)
    return 0 if not problems else 1


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
        extra_text, extra_rows = LOCAL_EXTRA_ROWS.get(family, ('', 0))
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
            if extra_text:
                out.append(extra_text)
            out.append('};\n\n')
            total += len(entries) + extra_rows
            notes = (f'{len(collapsed)} duplicate rows collapsed' if collapsed else None)
            if extra_rows:
                notes = ((notes + '; ' if notes else '') +
                         f'{extra_rows} {NOTE_NEW}')
        else:
            ident = family
            entries, notes, source, cls, vendor = [], None, CLASS_ONLY[family], \
                '0xff, 0xff, 0xff', 0
        bits, detail = measure(root, family, probe)
        rows.append(dict(family=family, ident=re.sub(r'\W', '_', family),
                         vendor=vendor, cls=cls, count=len(entries) + extra_rows,
                         bits=bits, detail=detail, source=source, note=notes))
    out.append(LOCAL_IDS_TEXT)
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
    out.append(LOCAL_RECORD_TEXT)
    out.append('};\n\n')
    out.append(LOCAL_COUNTS.format(family=LOCAL_FAMILY, families=len(rows) + 1, ids=total + 1,
                                   upstream=total - sum(c for _, c in LOCAL_EXTRA_ROWS.values()),
                                   extra=sum(c for _, c in LOCAL_EXTRA_ROWS.values()),
                                   extra_families=len(LOCAL_EXTRA_ROWS)))
    out.append(f'#define GPU_MATCH_COUNT {len(rows) + 1}u\n')
    out.append(f'#define GPU_ID_TOTAL {total + 1}u\n')
    out.append('\n#endif\n')
    return ''.join(out), total



def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--haiku', help='a Haiku checkout at the pinned commit (gpu_ids.h source)')
    ap.add_argument('--out')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--check-local', action='store_true',
                    help='verify the hand-authored local family record in the committed header')
    ap.add_argument('--registry-only', action='store_true',
                    help='(re)generate gpu_ids_registry.h from the in-tree registry snapshot')
    ap.add_argument('--extract-pci-ids', metavar='PCI_IDS',
                    help='rebuild tools/research/pci.ids.display.txt from a real pci.ids file')
    ap.add_argument('--allow-head', action='store_true')
    args = ap.parse_args()
    if args.extract_pci_ids:
        count, version = extract_snapshot(args.extract_pci_ids)
        print(f'wrote {REGISTRY_SNAPSHOT}: {count} display rows from pci.ids {version}')
        return 0
    if args.check_local:
        return check_local(args.out or 'kernel/drivers/gpu/gpu_ids.h')
    if args.registry_only:
        return run_registry(args.out or 'kernel/drivers/gpu/gpu_ids_registry.h', args.check)
    if not args.haiku:
        raise SystemExit('--haiku DIR is required to regenerate gpu_ids.h; use --registry-only '                         'for the naming header, which reads the in-tree snapshot instead')
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

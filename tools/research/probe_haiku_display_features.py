#!/usr/bin/env python3
"""Measure what Haiku's per-family graphics drivers actually bind and expose.

This is the display-side companion to `audit_basic_2d_sources.py`.  That tool
measures the 2D blit/fill code (the smallest portable "GPU moves pixels" unit);
this one answers the two questions a display tier raises instead:

  * which PCI device IDs does the family's own binding table list, how many of
    them are inside a disabled `#if 0` block, and what is the oldest/newest chip
    name in that table (table breadth is NOT the same as acceleration);
  * which display-engine features does the accelerant hand out - retrace/vsync
    semaphore, `MOVE_DISPLAY` (CRTC panning), hardware cursor, overlay, DPMS,
    backlight - and is each one returned directly, gated behind a condition, or
    absent; plus the kernel side's interrupt path (MSI, faked vblank) and the
    engine capability token with its advertised capability bits.

Read-only source measurement: no build, no ABI check, no compatibility claim, no
hardware test, nothing imported into SCos.  Every answer is a file:line quote
taken from the checkout, so a reader can verify it with `sed`.

    python3 tools/research/probe_haiku_display_features.py --haiku DIR > out.json

`DIR` must be a checkout at the pinned commit below (see --allow-head).
"""
import argparse
import json
import os
import re
import subprocess
import sys

PIN = '7be0fef07df0ecbe6f40a4cf2a7687775f1f28a0'
FAMILIES = [
    'radeon', 'nvidia', 'matrox', 'via', 'neomagic', 's3', 'ati', '3dfx',
    'et6x00', 'intel_810', 'intel_extreme', 'radeon_hd', 'virtio', 'vesa',
    'framebuffer',
]
# Accelerant hook labels whose presence tells us which display-engine features
# the driver offers next to (or instead of) 2D drawing.
FEATURES = {
    'vsync_retrace': 'B_ACCELERANT_RETRACE_SEMAPHORE',
    'pan_display': 'B_MOVE_DISPLAY',
    'hardware_cursor_shape': 'B_SET_CURSOR_SHAPE',
    'hardware_cursor_move': 'B_MOVE_CURSOR',
    'overlay_allocate': 'B_ALLOCATE_OVERLAY_BUFFER',
    'fill_rectangle': 'B_FILL_RECTANGLE',
    'screen_to_screen_blit': 'B_SCREEN_TO_SCREEN_BLIT',
    'fill_span': 'B_FILL_SPAN',
    'set_display_mode': 'B_SET_DISPLAY_MODE',
    'dpms': 'B_SET_DPMS_MODE',
    'backlight': 'B_SET_BRIGHTNESS',
    'edid': 'B_GET_EDID_INFO',
    'frame_buffer_config': 'B_GET_FRAME_BUFFER_CONFIG',
    'engine_count': 'B_ACCELERANT_ENGINE_COUNT',
}
# Kernel-side signals that a driver owns its own scanout timing and memory.
KERNEL_SIGNALS = {
    'msi_interrupts': r'get_msi_count|configure_msi|enable_msi',
    'faked_vblank': r'fake_interrupts',
    'vblank_semaphore': r'vblank_sem',
    'gen8_interrupts': r'gen8_enable_interrupts|gen11_enable_global_interrupts',
    'gart_aperture': r'gGART|map_aperture|allocate_memory',
    'engine_stall_comment': r'acc engine not yet functional',
    'power_wells': r'PWR_WELL|power_well',
    'modern_pll': r'TigerLakePLL|SKL_DPLL|ICL_|TGL_',
}
TEXT = ('.c', '.h', '.cpp', '.hpp')
ROW = re.compile(r'\{\s*0x([0-9a-fA-F]{4})\s*,([^\n]*?)"([^"]+)"')


def head_of(root):
    p = subprocess.run(['git', '-C', root, 'rev-parse', 'HEAD'],
                       capture_output=True, text=True)
    return p.stdout.strip() if p.returncode == 0 else 'not a git checkout'


def family_files(root, family):
    out = []
    for prefix in ('src/add-ons/accelerants', 'src/add-ons/kernel/drivers/graphics',
                   'headers/private/graphics'):
        base = os.path.join(root, prefix, family)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [d for d in dirnames if d != '.git']
            for f in filenames:
                if f.endswith(TEXT):
                    out.append(os.path.relpath(os.path.join(dirpath, f), root))
    return sorted(set(out))


def lines_of(root, rel):
    with open(os.path.join(root, rel), 'rb') as fh:
        return fh.read().decode('utf-8', 'replace').split('\n')


ID_LIST = re.compile(
    r'(?:uint16|uint32|unsigned short|short)\s+\w*(?:device|Device|DEVICE)\w*\s*\[\]\s*=\s*\{(.*?)\}',
    re.S)
ID_MACRO = re.compile(r'#define\s+\w*DEVICE_ID\w*\s+0x([0-9a-fA-F]{4})')


def device_table(root, files):
    """Chip coverage exactly as the driver lists it, in the two forms Haiku uses.

    * row form `{ 0xNNNN, <group>, "Name" }`, counted with `#if 0` regions
      tracked, because a disabled row is not a supported chip;
    * raw-list form (`static uint16 nvidia_device_list[] = {0x0020, ...}` and
      `#define DEVICE_ID_RADEON_QD 0x5144`), counted as distinct 4-digit hex
      literals, since those lists carry no chip names.
    """
    rows, disabled, literals = [], 0, set()
    for rel in files:
        on = True
        for text in lines_of(root, rel):
            s = text.strip()
            if s.startswith('#if 0'):
                on = False
                continue
            if s.startswith('#endif'):
                on = True
                continue
            if not on:
                if ROW.match(s):
                    disabled += 1
                continue
            m = ROW.match(s)
            if m:
                rows.append({'id': '0x' + m.group(1).upper(), 'name': m.group(3),
                             'file': rel, 'text': s})
            d = ID_MACRO.search(s)
            if d:
                literals.add(d.group(1).upper())
        whole = '\n'.join(lines_of(root, rel))
        for block in ID_LIST.findall(whole):
            for hx in re.findall(r'0x([0-9a-fA-F]{4})\b', block):
                literals.add(hx.upper())
    seen, unique = set(), []
    for r in rows:
        if r['id'] not in seen:
            seen.add(r['id'])
            unique.append(r)
    return {'row_form': {'binding_rows': len(rows), 'distinct_ids': len(unique),
                         'rows_inside_if0': disabled,
                         'oldest_row': unique[0] if unique else None,
                         'newest_row': unique[-1] if unique else None,
                         'files': sorted({r['file'] for r in unique})},
            'raw_id_form': {'distinct_device_id_literals': len(literals),
                            'note': 'distinct 0xNNNN device literals in device-list '
                                    'arrays and DEVICE_ID macros; not a chip-name count'}}


MACRO_DEF = re.compile(r'#define\s+(\w+)\(\s*(?:feature\s*,\s*)?x\s*\)(.*)')


def hook_macros(root, rel):
    """`#define HOOK(x) case B_##x: return (void *)x` style dispatch macros.

    Several classic accelerants build their whole dispatch table out of such
    macros, so the gating (a hardware cursor only when a setting is on, no hook
    at all for some chips) lives in the macro body, not at the call site.
    """
    out = {}
    for i, text in enumerate(lines_of(root, rel)):
        m = MACRO_DEF.search(text)
        if m:
            out[m.group(1)] = {'line': i + 1, 'body': ' '.join(m.group(2).split())[:200]}
    return out


def macro_verdict(body):
    if 'return NULL' in body or 'return (void *)0' in body:
        return 'conditional' if '?' in body or 'if' in body else 'denied'
    if '?' in body or '&&' in body or '||' in body:
        return 'conditional'
    return 'direct'


def hooks(root, files):
    """Per feature: the dispatch site plus the lines/macro that decide what is
    returned.  A feature dispatched through a `#define` macro is judged by that
    macro's body, which is where these accelerants put their gating."""
    out = {}
    for rel in files:
        base = os.path.basename(rel)
        if not (base.startswith('hooks') or 'AccelerantHook' in base):
            continue
        macros = hook_macros(root, rel)
        src = lines_of(root, rel)
        for key, label in FEATURES.items():
            bare = label[2:] if label.startswith('B_') else label
            for i, text in enumerate(src):
                stripped = text.strip()
                if (label not in stripped and bare not in stripped) \
                        or stripped.startswith('*'):
                    continue
                used = next((name for name in macros
                             if re.search(rf'\b{name}\(\s*{re.escape(bare)}\s*\)', stripped)), None)
                if used:
                    body = macros[used]['body']
                    out[key] = {'file': rel, 'line': i + 1,
                                'status': ('disabled-by-comment' if stripped.startswith('//')
                                           else macro_verdict(body)),
                                'dispatch_macro': used, 'macro_line': macros[used]['line'],
                                'return': body[:140]}
                    continue
                if stripped.startswith('//') or 'case' not in stripped:
                    continue
                if 'return' in stripped:      # `case B_X: return NULL;` on one line
                    out[key] = {'file': rel, 'line': i + 1,
                                'status': ('disabled-by-comment'
                                          if stripped.split('return', 1)[1].lstrip()
                                               .startswith('//')
                                          else 'denied' if 'NULL' in stripped
                                          or '(void *)0' in stripped else 'direct'),
                                'return': stripped[:140],
                                'decision_text': stripped[:300]}
                    break
                window = []
                for j in range(i + 1, min(len(src), i + 30)):
                    s = src[j].strip()
                    if s.startswith('case ') or s.startswith('default:'):
                        break
                    window.append((j + 1, s))
                    if 'return' in s:
                        break
                if not window:
                    continue
                gated_at = next((k for k, (ln, s) in enumerate(window)
                                 if s.startswith('if') or s.startswith('} else')), None)
                ret_idx = next((k for k, (ln, s) in enumerate(window) if 'return' in s),
                               None)
                if ret_idx is None:
                    continue
                line, returns = window[ret_idx]
                if returns.lstrip().startswith('//'):
                    status = 'disabled-by-comment'
                elif 'return NULL' in returns:
                    status = 'denied-for-some-devices' if gated_at is not None else 'denied'
                elif gated_at is not None and gated_at < ret_idx:
                    status = 'gated'
                else:
                    status = 'direct'
                entry = {'file': rel, 'line': i + 1, 'returns_at_line': line,
                         'status': status, 'return': returns[:140],
                         'decision_text': ' | '.join(s for _, s in window[:ret_idx + 1])[:300]}
                prev = out.get(key)
                if prev is None or (prev['status'] == 'disabled-by-comment'
                                    and status != 'disabled-by-comment'):
                    out[key] = entry
    return {'found': out, 'absent': [k for k in FEATURES if k not in out]}


def kernel_signals(root, files):
    found = {}
    for key, pattern in KERNEL_SIGNALS.items():
        rx = re.compile(pattern)
        for rel in files:
            if not rel.startswith('src/add-ons/kernel/'):
                continue
            for i, text in enumerate(lines_of(root, rel)):
                if rx.search(text):
                    found[key] = {'file': rel, 'line': i + 1, 'text': text.strip()[:120]}
                    break
            if key in found:
                break
    return found


def engine_token(root, files):
    """The advertised engine capability bits, verbatim, plus any gate on them."""
    rx = re.compile(r'engine_token\s+\w+\s*=\s*\{(.*?)\}', re.S)
    for rel in files:
        src = lines_of(root, rel)
        text = '\n'.join(src)
        m = rx.search(text)
        if not m:
            continue
        # A capability written inside a comment is an annotation, not an
        # advertisement, so comments are removed before the flag is read.
        bare = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S)
        return {'file': rel, 'line': text[:m.start()].count('\n') + 1,
                'declaration': ' '.join(m.group(0).split())[:180],
                'advertises_2d': 'B_2D_ACCELERATION' in bare,
                'note': 'advertises_2d is read from the initializer after '
                        'removing /* */ comments, so `{1, 0 /*B_2D_ACCELERATION*/}` '
                        'reports False and `{1, B_2D_ACCELERATION, NULL}` True'}
    return None


def audit(root):
    report = {'kind': 'haiku-display-feature-probe',
              'status': 'source review only; no build, no ABI check, no hardware test',
              'pinned_commit': PIN,
              'head': head_of(root),
              'reproduce': 'python3 tools/research/probe_haiku_display_features.py '
                           '--haiku DIR',
              'method': 'device-binding tables counted with #if 0 regions tracked; '
                        'accelerant hook tables read as text (a hook whose return '
                        'line is commented out or NULL is reported as such, not '
                        'inferred); kernel-side signals matched by pattern',
              'cautions': [
                  'a device table that lists a chip is not evidence that chip works: '
                  'radeon_hd lists many newer parts than the ones its build supports, '
                  'and rows inside #if 0 are explicitly not bound',
                  'hook presence is not hook correctness: these hooks are no longer '
                  'exercised by Haiku app_server (see GPU-BASIC-2D-RESEARCH.md)',
                  'engine capability bits are the accelerant self-report; the 2D '
                  'entry points can exist while the token advertises nothing',
              ],
              'families': {}}
    for fam in FAMILIES:
        files = family_files(root, fam)
        if not files:
            report['families'][fam] = {'error': 'no such family in this checkout'}
            continue
        report['families'][fam] = {
            'closure_files': len(files),
            'device_table': device_table(root, files),
            'engine_token': engine_token(root, files),
            'hooks': hooks(root, files),
            'kernel_signals': kernel_signals(root, files),
        }
    return report


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--haiku', required=True, help='path to a Haiku checkout')
    ap.add_argument('--allow-head', action='store_true',
                    help='measure even when HEAD is not the pinned commit')
    a = ap.parse_args()
    if not os.path.isdir(os.path.join(a.haiku, 'src')):
        print(f'{a.haiku}: no src/ directory', file=sys.stderr)
        return 2
    head = head_of(a.haiku)
    if head != PIN and not a.allow_head:
        print(f'{a.haiku}: HEAD {head[:12]} != pinned {PIN[:12]}; '
              're-run with --allow-head to measure anyway', file=sys.stderr)
        return 3
    json.dump(audit(a.haiku), sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write('\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())

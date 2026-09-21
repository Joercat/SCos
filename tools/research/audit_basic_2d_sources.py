#!/usr/bin/env python3
"""Local, read-only inventory of lightweight display/2D GPU driver sources.

Scope: upstream code that can put an image on the screen and let the GPU move
pixels (scanout, page flip, blit/fill) WITHOUT a full OpenGL/Vulkan or vendor
resource-manager stack.  Source inventory only: no build, no ABI check, no
compatibility claim, no hardware test, and nothing is imported into SCos.

Method: exact pinned commits are checked out sparsely with `git` (partial,
blobless clone), then measured with local file walks.  This keeps GitHub API
usage to one clone per repository instead of thousands of file fetches.

    python3 tools/research/audit_basic_2d_sources.py --cache DIR [--fetch] [--jobs N]

With --fetch the tool creates the sparse checkouts it needs; without it, it
only measures checkouts that already exist at the pinned commit.
"""
import argparse
import json
import os
import re
import subprocess
import sys

PINS = {
    'haiku': ('https://github.com/Haiku/haiku', '7be0fef07df0ecbe6f40a4cf2a7687775f1f28a0'),
    'genode': ('https://github.com/genodelabs/genode',
               '0f275e7afaab44e9a45cecd0eac0b88691278477'),
    'linux': ('https://github.com/torvalds/linux',
              '93f51579e7df248780214094418f205253383cc5'),
    'libgfxinit': ('https://github.com/9elements/libgfxinit',
                   'd49b56baf97136e1b65f032a985291bf40a73260'),
    'coreboot': ('https://github.com/coreboot/coreboot',
                 '1879b6a34a6e93a93d691a0d9f2457d6251a17c1'),
}
SPARSE = {
    'haiku': ['src/add-ons/accelerants', 'src/add-ons/kernel/drivers/graphics',
              'headers/private/graphics'],
    'genode': ['repos/pc/src/driver/framebuffer', 'repos/os/src/driver/framebuffer',
               'repos/os/src/driver/gpu', 'repos/pc/recipes/src/pc_intel_fb',
               'repos/pc/run/intel_fb.run'],
    'linux': ['drivers/gpu/drm', 'drivers/video/fbdev/efifb.c'],
    'libgfxinit': ['common', 'configs', 'COPYING'],
    'coreboot': ['src/soc/intel', 'src/drivers/video'],
}
TEXT = ('.c', '.h', '.cpp', '.hpp', '.cc', '.ads', '.adb')

HAIKU_FAMILIES = [
    'radeon', 'radeon_hd', 'nvidia', 'intel_extreme', 'intel_810', 'matrox', 'via', 's3',
    'ati', 'neomagic', '3dfx', 'et6x00', 'vesa', 'virtio', 'framebuffer', 'common', 'skeleton',
]
HAIKU_PREFIXES = {
    'accelerants': 'src/add-ons/accelerants',
    'kernel_driver': 'src/add-ons/kernel/drivers/graphics',
    'private_headers': 'headers/private/graphics',
}
# Verified by reading the pinned sources: these files hold the GPU 2D operations
# (copy, solid fill, invert, span) and their command/ring plumbing.  This is the
# smallest honest port unit; the surrounding per-family totals include modesetting.
HAIKU_2D_CORE_FILES = {
    'radeon': ['Acceleration.c', 'CP.c', 'EngineManagment.c'],
    'intel_extreme': ['engine.cpp'],
    'intel_810': ['engine.cpp'],
    'nvidia': ['Acceleration.c', 'engine/nv_acc.c', 'engine/nv_acc_dma.c'],
    'matrox': ['engine/mga_acc.c'],
    'via': ['engine/acc.c'],
    'neomagic': ['engine/nm_acc.c'],
    'et6x00': ['Acceleration.c'],
    'ati': ['rage128_draw.cpp', 'mach64_draw.cpp'],
    '3dfx': ['3dfx_draw.cpp'],
    's3': ['accel.cpp', 'virge_draw.cpp', 'trio64_draw.cpp', 'savage_draw.cpp'],
}
LINUX_MEASURE = [
    'drivers/gpu/drm/i915', 'drivers/gpu/drm/amd', 'drivers/gpu/drm/radeon',
    'drivers/gpu/drm/nouveau', 'drivers/gpu/drm/tiny', 'drivers/gpu/drm/sysfb',
    'drivers/video/fbdev/efifb.c', 'drivers/gpu/drm/drm_crtc.c',
]
GENODE_MEASURE = [
    'repos/os/src/driver/gpu/intel', 'repos/pc/src/driver/framebuffer/intel',
    'repos/os/src/driver/framebuffer/virtio', 'repos/os/src/driver/framebuffer/boot',
]
COREBOOT_PATTERN = re.compile(r'(^/)(gma|graphics|display|gfx)[^/]*\.(c|h)$', re.I)


def run(args, cwd=None, check=True):
    p = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if check and p.returncode:
        raise RuntimeError(f'{args[0]} failed: {(p.stderr or p.stdout).strip()[:400]}')
    return p.stdout.strip()


def ensure(repo, cache, fetch):
    url, sha = PINS[repo]
    path = os.path.join(cache, repo)
    if os.path.isdir(os.path.join(path, '.git')):
        head = run(['git', 'rev-parse', 'HEAD'], cwd=path, check=False)
        if head == sha:
            return path, 'existing pinned checkout'
        if not fetch:
            raise RuntimeError(f'{path}: HEAD {head[:12]} != pinned {sha[:12]}; rerun --fetch')
    if not fetch:
        raise RuntimeError(f'{path} missing; rerun with --fetch to create it')
    os.makedirs(cache, exist_ok=True)
    if not os.path.isdir(os.path.join(path, '.git')):
        run(['git', 'clone', '--filter=blob:none', '--no-checkout', '--no-tags', url, path])
    run(['git', 'checkout', '--quiet', sha], cwd=path)
    run(['git', 'sparse-checkout', 'init', '--no-cone'], cwd=path)
    run(['git', 'sparse-checkout', 'set'] + SPARSE[repo], cwd=path)
    run(['git', 'checkout', '--quiet', sha], cwd=path)
    return path, 'created sparse checkout'


def walk_files(root, rel, suffixes=TEXT):
    base = os.path.join(root, rel)
    out = []
    if os.path.isfile(base):
        return [rel]
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = [d for d in dirnames if d not in ('.git',)]
        for f in filenames:
            if f.endswith(suffixes):
                out.append(os.path.relpath(os.path.join(dirpath, f), root))
    return sorted(out)


def measure(root, paths, suffixes=TEXT):
    files = []
    for p in paths:
        files += walk_files(root, p, suffixes)
    files = sorted(set(files))
    lines = 0
    for f in files:
        try:
            with open(os.path.join(root, f), 'rb') as fh:
                lines += fh.read().count(b'\n') + 1
        except OSError:
            pass
    return {'files': len(files), 'bytes': sum(os.path.getsize(os.path.join(root, f))
                                              for f in files), 'lines': lines,
            'paths': files if len(files) <= 12 else None}


def haiku(root):
    families = {}
    for name in HAIKU_FAMILIES:
        components = {}
        totals = {'files': 0, 'bytes': 0, 'lines': 0}
        for label, prefix in HAIKU_PREFIXES.items():
            rel = f'{prefix}/{name}'
            if not os.path.isdir(os.path.join(root, rel)):
                continue
            m = measure(root, [rel])
            m.pop('paths', None)
            components[label] = m
            for k in totals:
                totals[k] += m[k]
        core_rel = [f'src/add-ons/accelerants/{name}/{f}'
                    for f in HAIKU_2D_CORE_FILES.get(name, [])]
        core = measure(root, core_rel) if core_rel else None
        token = None
        for cand in ('EngineManagment.c', 'EngineManagement.c', 'engine.cpp'):
            p = os.path.join(root, 'src/add-ons/accelerants', name, cand)
            if os.path.isfile(p):
                text = open(p, 'r', errors='replace').read()
                found = re.search(r'\w+\s*=\s*\{\s*\d+\s*,[^}]*\}', text)
                if found:
                    token = ' '.join(found.group(0).split())
                break
        families[name] = {'components': components, 'totals': totals,
                          'gpu_2d_core_files': core, 'engine_token': token}
    return {'commit': PINS['haiku'][1], 'families': families,
            'note': 'Each family = Haiku kernel driver + accelerant + private headers: the '
                    'closure a real port needs. gpu_2d_core_files = only the files that '
                    'program the GPU copy/fill/ring path (manually verified at this commit). '
                    'engine_token is the capability the driver advertises today; '
                    '"0 /*B_2D_ACCELERATION*/" means Haiku does not enable 2D for that chip.'}


def genode(root):
    comps = {p: measure(root, [p]) for p in GENODE_MEASURE}
    for v in comps.values():
        v.pop('paths', None)
    out = {'commit': PINS['genode'][1], 'components': comps}
    lists = walk_files(root, 'repos/pc/src/driver/framebuffer/intel', ('.list',))
    entries = []
    for lst in lists:
        with open(os.path.join(root, lst), 'r', errors='replace') as fh:
            entries += [e.strip() for e in fh if e.strip() and not e.startswith('#')]
    out['linux_port_source_lists'] = {
        'files': lists,
        'listed_upstream_files': len(set(entries)),
        'i915_files': len({e for e in entries if '/i915/' in e}),
        'i915_display_files': len({e for e in entries if '/i915/display/' in e}),
        'drm_core_files': len({e for e in entries if re.search(r'/gpu/drm/[^/]+\.c$', e)}),
        'agp_or_gtt_files': len({e for e in entries if 'agp' in e or 'gtt' in e}),
        'sample': sorted(set(entries))[:6],
    }
    mk = os.path.join(root, 'repos/pc/recipes/src/pc_intel_fb/content.mk')
    out['pc_intel_fb_content_mk'] = open(mk, errors='replace').read().strip() \
        if os.path.isfile(mk) else None
    out['note'] = ('repos/pc/src/driver/framebuffer/intel is the ported Linux i915 DRM driver '
                   'plus Genode Linux-emulation glue (GPL-2.0 kernel COPYING), not a small '
                   'standalone driver; repos/os/src/driver/gpu/intel is the Genode GPU session '
                   'used with Mesa and contains no desktop copy/fill engine in this tree.')
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cache', required=True)
    ap.add_argument('--fetch', action='store_true')
    a = ap.parse_args()
    roots, methods = {}, {}
    for repo in PINS:
        roots[repo], methods[repo] = ensure(repo, a.cache, a.fetch)

    linux = {'commit': PINS['linux'][1], 'paths': {}}
    for path in LINUX_MEASURE:
        entry = measure(roots['linux'], [path])
        entry.pop('paths', None)
        linux['paths'][path] = entry
    libgfx = measure(roots['libgfxinit'], ['common'])
    libgfx.pop('paths', None)
    cb_files = []
    for dirpath, dirnames, filenames in os.walk(os.path.join(roots['coreboot'], 'src/soc/intel')):
        for f in filenames:
            if COREBOOT_PATTERN.search('/' + f):
                cb_files.append(os.path.relpath(os.path.join(dirpath, f), roots['coreboot']))
    cb_measured = measure(roots['coreboot'], sorted(cb_files)) if cb_files         else {'files': 0, 'bytes': 0, 'lines': 0}
    cb_measured.pop('paths', None)
    coreboot = {'commit': PINS['coreboot'][1], 'pattern': COREBOOT_PATTERN.pattern,
                'measured': cb_measured,
                'note': 'Files named gma/graphics/display/gfx under src/soc/intel. Small '
                        'per SoC because this is boot-display hand-off only; newer Intel '
                        'SoCs carry near-nothing here and rely on firmware/GOP, which is '
                        'the same situation SCos is in now.',
                'measured_paths': sorted(cb_files)[:24]}
    libgfx['configs'] = sorted(os.listdir(os.path.join(roots['libgfxinit'], 'configs'))) \
        if os.path.isdir(os.path.join(roots['libgfxinit'], 'configs')) else None

    report = {
        'kind': 'lightweight-display-and-2d-source-inventory',
        'status': 'source review only; no build, no ABI check, no compatibility claim, '
                  'no hardware test',
        'method': 'pinned sparse git checkouts measured locally; one clone per repository',
        'checkout_methods': methods,
        'pinned_commits': {k: v[1] for k, v in PINS.items()},
        'reproduce': 'python3 tools/research/audit_basic_2d_sources.py --cache DIR --fetch',
        'not_verified': [
            'any driver compiles against SCos kernel interfaces',
            'any driver binds the exact PCI device present in the target machine',
            'any GPU operation completes with correct pixels',
            'compositor integration, failure recovery and memory accounting',
            'browser or application rendering correctness',
        ],
        'haiku': haiku(roots['haiku']),
        'linux_drm_size_reference': linux,
        'genode': genode(roots['genode']),
        'libgfxinit': {'commit': PINS['libgfxinit'][1], 'measured': libgfx,
                       'note': 'Ada/SPARK Intel modesetting library: sets PLLs, pipes, planes '
                               'and connectors for a boot display. No 2D drawing engine, so it '
                               'cannot replace CPU pixel writes by itself.'},
        'coreboot': coreboot,
    }
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())

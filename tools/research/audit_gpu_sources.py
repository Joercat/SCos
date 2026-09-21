#!/usr/bin/env python3
"""Read-only, pinned upstream audit. Requires gh; downloads no driver firmware.
This is a source inventory, NOT a build, compatibility or hardware test.
Prints deterministic JSON; does not import upstream code into SCos.
"""
import concurrent.futures
import hashlib
import json
import subprocess

PINS = {
    'genodelabs/genode': '0f275e7afaab44e9a45cecd0eac0b88691278477',
    'X547/RadeonGfx': '797e94775d274f243d8e0fe18b434b672bdeb768',
    'X547/nvidia-haiku': 'cc1849cb2c5c327af4f7eccf993b8616855ad7c5',
    'NVIDIA/open-gpu-kernel-modules': '61dcc93722ecb418bb5f2e00923f05b4b8051dd1',
    'freebsd/drm-kmod': 'f252a30f27d157d9c763cd408850775096a6263f',
}
INSPECT = {
    'genodelabs/genode': ['LICENSE', 'repos/gems/sculpt/gpu/intel',
        'repos/libports/recipes/pkg/mesa_gpu-intel/README',
        'repos/pc/recipes/src/pc_intel_fb/content.mk'],
    'X547/RadeonGfx': ['meson.build', 'Units/InstantiateUnits.cpp',
        'RadeonDevice.cpp', 'RadeonFirmware.cpp'],
    'X547/nvidia-haiku': ['LICENSE', '.gitmodules', 'Build.sh',
        'nvidia_gsp/nvidia/os-haiku.cpp'],
    'NVIDIA/open-gpu-kernel-modules': ['COPYING', 'README.md'],
    'freebsd/drm-kmod': ['README.md'],
}
INTEL_DIR = 'repos/os/src/driver/gpu/intel/'

def api(path, raw=False):
    args = ['gh', 'api']
    if raw:
        args += ['-H', 'Accept: application/vnd.github.raw+json']
    return subprocess.check_output(args + [path], timeout=90)

def audit(repo, sha):
    tree = json.loads(api(f'repos/{repo}/git/trees/{sha}?recursive=1'))
    if tree.get('truncated'):
        raise RuntimeError(f'{repo}: truncated tree; cannot report totals')
    blobs = [e for e in tree['tree'] if e['type'] == 'blob']
    submodules = {e['path']: e['sha'] for e in tree['tree'] if e['type'] == 'commit'}
    paths = list(INSPECT[repo])
    if repo == 'genodelabs/genode':
        paths += [e['path'] for e in blobs if e['path'].startswith(INTEL_DIR)]
    def inspect(path):
        data = api(f'repos/{repo}/contents/{path}?ref={sha}', raw=True)
        return path, {'bytes': len(data), 'lines': len(data.splitlines()),
                      'sha256': hashlib.sha256(data).hexdigest()}
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        evidence = dict(pool.map(inspect, paths))
    result = {'commit': sha, 'tree_blob_count': len(blobs),
              'tree_blob_bytes': sum(e['size'] for e in blobs),
              'submodules_excluded_from_totals': submodules,
              'reviewed_files': evidence,
              'firmware_bin_files': {e['path']: e['size'] for e in blobs
                  if e['path'].startswith('firmware/') and e['path'].endswith('.bin')},
              'scos_build_test': 'not performed', 'scos_hardware_test': 'not performed'}
    if repo == 'genodelabs/genode':
        driver = [v for p, v in evidence.items() if p.startswith(INTEL_DIR)]
        result['intel_driver_directory_only'] = {
            'files': len(driver), 'bytes': sum(v['bytes'] for v in driver),
            'lines': sum(v['lines'] for v in driver),
            'excludes': 'Genode base/platform, Mesa, framebuffer driver, other dependencies'}
    return result

def main():
    report = {'date': '2026-09-20', 'kind': 'source audit, NOT working SCos drivers',
              'measurement': 'Git tree blob sizes; includes data/generated files/symlinks; '
                             'excludes submodule contents and external dependencies. '
                             'Not executable size or resident RAM.',
              'repositories': {repo: audit(repo, sha) for repo, sha in PINS.items()}}
    print(json.dumps(report, indent=2, sort_keys=True))

if __name__ == '__main__':
    main()

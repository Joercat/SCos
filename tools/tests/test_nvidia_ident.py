"""The NVIDIA Blackwell identification module, run here against a register file made of an array.

Why this suite exists: the driver in drivers/gpu/nvidia reads one register and reports what the chip
said, and it refuses when the chip says nothing.  Both halves matter, and neither can be measured on
the machines available here - there is no Blackwell GPU in the building, and QEMU models none.  So the
module is compiled exactly as `make gpu-modules` compiles it, linked against a fake
`struct scos_gpu_exports`, and run: the decode has to come out of the fixture word, every refusal path
has to refuse, and any store the module attempts is a failure of this run.  That is a real measurement
of the driver's logic; what it does not do is claim the chip was touched, which is the claim the report
text makes only after the read and write counts it prints, which are matched against what the device
served, not against what the driver meant to do.

The second half of the suite is the part that a C harness cannot see: the list of device ids the module
claims must be a subset of the family table the loader cross-checks it against, byte-for-byte equal to
the hand-authored rows the generator owns.  Those two drifting apart is how a driver ends up either
never loading or loading for a chip it was not written for.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

MODULE_CFLAGS = [
    '-m64', '-ffreestanding', '-fno-stack-protector', '-fno-builtin',
    '-fno-asynchronous-unwind-tables', '-mno-red-zone', '-O2',
    '-Wall', '-Wextra', '-Werror', '-std=c11',
]
HOST_CFLAGS = ['-m64', '-O1', '-std=c11', '-Wall', '-Wextra', '-Werror']


def run(cmd):
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 0, ' '.join(str(x) for x in cmd) + '\n' + result.stdout + result.stderr
    assert not result.stderr.strip(), ' '.join(str(x) for x in cmd) + '\n' + result.stderr
    return result


ROW = re.compile(r'\{0x([0-9a-fA-F]{1,4}), 0x([0-9a-fA-F]{1,4})(?:, 0x([0-9a-fA-F]+))?, '
                 r'(?:0, 0, 0, )?"([^"]+)"')


def module_rows():
    text = (ROOT / 'drivers/gpu/nvidia/nvidia_ids.inc').read_text()
    out = []
    for line in text.splitlines():
        m = re.match(r'\s*\{0x([0-9a-fA-F]{1,4}), 0x([0-9a-fA-F]{1,4}), 0, 0, 0\}', line)
        if m:
            out.append((int(m.group(1), 16), int(m.group(2), 16)))
    return out


def family_rows(family):
    """The generated array for one family, read the way the loader's checker reads it."""
    text = (ROOT / 'kernel/drivers/gpu/gpu_ids.h').read_text()
    body = re.search(r'static const struct gpu_pci_id gpu_ids_%s\[\] = \{(.*?)\n\};' % family,
                     text, re.S)
    assert body, 'no gpu_ids_%s array in the generated header' % family
    return [(int(v, 16), int(d, 16), name) for v, d, name in
            re.findall(r'\{0x([0-9A-Fa-f]{1,4}), 0x([0-9A-Fa-f]{1,4}), "([^"]+)"\}', body.group(1))]


def generator_rows(family):
    """The same rows, as the generator that owns them holds them."""
    sys.path.insert(0, str(ROOT / 'tools' / 'research'))
    import gen_gpu_tables
    text, count = gen_gpu_tables.LOCAL_EXTRA_ROWS[family]
    rows = [(int(v, 16), int(d, 16)) for v, d in
            re.findall(r'^    \{0x([0-9A-Fa-f]{1,4}), 0x([0-9A-Fa-f]{1,4}),', text, re.M)]
    assert count == len(rows), (count, len(rows))
    return rows


def test_the_claims_match_the_table_that_authorises_them():
    claimed = module_rows()
    table = [(v, d) for v, d, _ in family_rows('nvidia')]
    assert len(claimed) == 19, claimed
    missing = [hex(d) for _, d in claimed if (0x10DE, d) not in [(0x10DE, x) for _, x in table]]
    assert not missing, 'ids the module claims but the nvidia family table does not bind: ' + ', '.join(missing)
    assert claimed == generator_rows('nvidia'), 'the module and the generator disagree, row for row'
    assert (0x10DE, 0x2D83) in claimed, 'the RTX 5050 must be claimed, or nothing loads for that card'
    # An id that binds a driver must not also be filed as "named, and nothing more".
    registry = (ROOT / 'kernel/drivers/gpu/gpu_ids_registry.h').read_text()
    for _, device in claimed:
        assert not re.search(r'\{0x10de, 0x%x,' % device, registry), hex(device)
    print('PASS: %d claimed id(s) equal the generator\'s rows, bind through the family table, and are '
          'absent from the naming table' % len(claimed))


def test_the_family_record_counts_what_the_table_holds():
    text = (ROOT / 'kernel/drivers/gpu/gpu_ids.h').read_text()
    rows = family_rows('nvidia')
    declared = re.search(r'\.ids = gpu_ids_nvidia, \.id_count = (\d+)u', text)
    assert declared and int(declared.group(1)) == len(rows), (declared and declared.group(1), len(rows))
    total = re.search(r'#define GPU_ID_TOTAL (\d+)u', text)
    ids = len(re.findall(r'^    \{0x[0-9A-Fa-f]{1,4}, 0x[0-9A-Fa-f]{1,4},', text, re.M))
    assert int(total.group(1)) == ids, (total.group(1), ids)
    print('PASS: the nvidia record claims %u ids and the array holds %d; GPU_ID_TOTAL agrees with the '
          'file (%d rows)' % (int(declared.group(1)), len(rows), ids))


def test_the_provenance_and_the_no_write_rule_are_in_the_source():
    def prose(path):
        """Comments are wrapped in the source; compare them with the wrapping normalised away, so the
        test pins the sentence and not the column it happens to break at."""
        text = re.sub(r'^\s*/?\*+ ?', '', (ROOT / path).read_text(), flags=re.M)
        return ' '.join(re.sub(r'\s*\*\s*', ' ', text).split())
    regs, module = prose('drivers/gpu/nvidia/blackwell_regs.h'), prose('drivers/gpu/nvidia/module.c')
    raw_module = (ROOT / 'drivers/gpu/nvidia/module.c').read_text()
    assert 'manuals/ampere/ga100/dev_boot.ref.txt' in regs, \
        'the register file must name the published source it was read from'
    assert 'open-gpu-doc' in regs and 'MIT' in regs, 'and the licence it is used under'
    assert 'No driver source from any operating system was read' in module, \
        'the one-line provenance rule is asserted, not assumed'
    assert 'X->write32' not in raw_module, \
        'the module must not reach for the write accessor at all: identification only'
    for name in ('fill', 'copy', 'wait_idle', 'vram_window', 'set_surfaces', 'fill_span', 'invert'):
        assert not re.search(r'\.%s\s*=' % name, module), \
            'the engine operation %s must stay unset, or the CPU path is no longer guaranteed' % name
    print('PASS: the module cites NVIDIA\'s published register documentation, states that no other '
          'driver was read, and contains no write path')


def test_the_harness_runs_the_module():
    out = ROOT / 'build' / 'nvidia-ident-host'
    out.mkdir(parents=True, exist_ok=True)
    module_o, host_o = out / 'module.o', out / 'host.o'
    run(['gcc'] + MODULE_CFLAGS + ['-Ikernel/include', '-Idrivers/gpu/nvidia', '-c',
                                   'drivers/gpu/nvidia/module.c', '-o', str(module_o)])
    run(['gcc'] + HOST_CFLAGS + ['-Ikernel/include', '-Idrivers/gpu/nvidia', '-c',
                                 'tools/tests/nvidia_ident_host.c', '-o', str(host_o)])
    exe = out / 'exe'
    run(['gcc', str(module_o), str(host_o), '-o', str(exe)])
    result = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True)
    lines = [l for l in result.stdout.splitlines() if l.startswith(('PASS', 'FAIL'))]
    print('\n'.join(lines))
    assert result.returncode == 0, result.stdout[-4000:] + result.stderr[-2000:]
    assert not [l for l in lines if l.startswith('FAIL')], result.stdout
    assert 'FORBIDDEN' not in result.stdout and 'note:' not in result.stdout, result.stdout
    checked = len([l for l in lines if l.startswith('PASS')])
    assert checked >= 35, 'the harness shrank: %d checks' % checked
    print('PASS: the identification module passes all %d host checks, including that no store was ever '
          'attempted' % checked)


def test_the_module_builds_for_the_kernel_it_ships_with():
    """Same compiler flags as `make gpu-modules`, and the packer's own verification of the result."""
    build = subprocess.run(['make', 'gpu-modules'], cwd=ROOT, capture_output=True, text=True)
    assert build.returncode == 0, build.stdout[-3000:] + build.stderr[-3000:]
    line = [l for l in (build.stdout + build.stderr).splitlines() if 'nvidia.mod:' in l]
    assert line, build.stdout[-2000:]
    assert 'ids=19' in line[0], line[0]
    print('PASS: ' + line[0].strip())


SCRATCH_MODULE = r'''#include "gpu_abi.h"
/* The same shape as drivers/gpu/nvidia/module.c - an ops table with only `describe' - packed for a
 * family an emulated machine actually has, so that the kernel's identification-only state is measured on
 * a device instead of reasoned about.  Written by this test into build/, never committed. */
static const struct scos_gpu_exports *X;
static const char *describe(void *c) { (void)c; return "scratch module: reads nothing, offers no engine"; }
static struct scos_gpu_engine_ops ops = { sizeof(ops), SCOS_GPU_ABI_VERSION, 0,
  0, 0, 0, 0, 0, describe, 0, 0, 0 };
const struct scos_gpu_pci_id scos_module_ids[] = { {0x1013, 0x00b8, 0, 0, 0}, };
void scos_module_teardown(struct scos_gpu_engine_ops *b) { (void)b; }
int scos_module_init(const struct scos_gpu_exports *e, struct scos_gpu_engine_ops **out) {
    X = e;
    if (!X || X->abi != SCOS_GPU_ABI_VERSION || X->size < sizeof(*X)) return -1;
    if (X->log) X->log("scratch: bound so that the identification-only path is measured on a device");
    *out = &ops;
    return 0;
}
'''

MODULE_FLAGS = ['-m64', '-ffreestanding', '-fno-stack-protector', '-fno-builtin',
                '-fno-asynchronous-unwind-tables', '-mno-red-zone', '-O2',
                '-Wall', '-Wextra', '-Werror', '-std=c11', '-fno-pie', '-mcmodel=small',
                '-fno-strict-aliasing']


def test_the_kernel_holds_the_identification_state_on_a_device():
    """Boot a real image whose Cirrus module offers no engine, and read what the running system says.

    The driver in drivers/gpu/nvidia cannot be run here - there is no Blackwell card - but the kernel half
    of the arrangement can: a module that binds, describes, and exposes no fill must leave the CPU
    compositor in charge, must not be reported as an engine, and must keep the desktop alive.  So this
    writes a throwaway module of exactly that shape, packs it as the Cirrus family, boots it, and restores
    the real one afterwards whatever happens."""
    import shutil
    import sys
    sys.path.insert(0, str(ROOT / 'tools' / 'tests'))
    from qemu_support import Guest
    scratch = ROOT / 'build' / 'scratch-ident'
    scratch.mkdir(parents=True, exist_ok=True)
    source = scratch / 'scratch.c'
    source.write_text(SCRATCH_MODULE)
    real = ROOT / 'build' / 'gpu' / 'cirrus.mod'
    backup = scratch / 'cirrus.mod.real'
    # Build the family's real module first, from source, rather than trusting what happens to be in
    # build/: a stale file with a newer timestamp is exactly what `make' will not notice, and this test
    # overwrites the file it saves, so the copy it restores has to be provably the one the Makefile
    # produces from drivers/gpu/cirrus right now.
    real.unlink(missing_ok=True)
    run(['make', 'gpu-modules'])
    shutil.copyfile(real, backup)
    obj = scratch / 'scratch.o'
    run(['gcc'] + ['-Ibuild'] + MODULE_FLAGS + ['-Ikernel/include', '-c', str(source), '-o', str(obj)])
    run(['ld', '-r', '-m', 'elf_x86_64', '-o', str(scratch / 'scratch.linked.o'), str(obj)])
    run(['python3', 'tools/build_gpu_module.py', '--input', str(scratch / 'scratch.linked.o'),
         '--family', 'cirrus', '--out', str(real), '--report'])
    run(['python3', 'tools/makedisk.py', 'build'])
    try:
        with Guest(name='identification', vga='cirrus') as g:
            g.run(1.2)
            lines = [l for l in g.serial().splitlines() if l.startswith('gpu:')]
            joined = '\n'.join(lines)
            assert 'bound for identification: it offers no rectangle operation' in joined, joined[-1500:]
            assert 'no engine was offered to verify' in joined, joined[-1500:]
            assert 'it offers no engine, so the CPU compositor keeps the screen' in joined, joined[-1500:]
            assert 'nothing was asked of the chip, so rendering stays on the CPU' in joined, joined[-1500:]
            assert 'engine in use' not in joined and 'verified in device memory' not in joined, joined[-1500:]
            # And the user is told, without being asked to read a panel first.  The notice used to fall
            # through to "No 2D engine bound ... no supported 2D engine was matched", which is false on a
            # machine whose module did match and did read the chip; that machine is the one in question.
            serial = g.serial()
            assert 'notification: GPU is not rendering' in serial, serial[-1500:]
            assert 'No 2D engine bound' not in serial, \
                   'a bound module that implements no engine is not "nothing was matched": ' + serial[-800:]
            g.call('graphics_report', g.scratch, 4096)
            panel = g.string(g.scratch, 4096)
            # The first line a user reads has to carry the outcome, not a narrative about registers: this
            # is the state where a module is resident, the chip was read, and nothing was drawn, and a
            # reader should not have to infer "not rendering" from a paragraph.
            assert '- the GPU is NOT rendering: the module that knows this chip implements no engine' \
                   in panel, panel[-1200:]
            assert 'read this chip\u2019s own registers' in panel or \
                   "read this chip's own registers" in panel, panel[-1200:]
            assert 'verified by device readback' not in panel, panel[-1200:]
            # The module's own words - identity, and on real silicon the submission window and the engine
            # inventory - are on this panel and not only one command away, because this panel is what gets
            # pasted when a machine is being diagnosed.
            assert 'Engine: scratch module: reads nothing, offers no engine' in panel, panel[-1200:]
            g.call('gpu_report', g.scratch, 4096)
            report = g.string(g.scratch, 4096)
            assert 'SCos port: driver module bound for this family reads the chip and describes it' \
                   in report, report[-1600:]
            assert 'path in use: the CPU compositor paints this screen' in report, report[-1600:]
            assert 'pixels painted: 0 by the GPU' in report, report[-1600:]
            assert 'link:' in report, 'every device line states the negotiated link, or says there is none'
    finally:
        shutil.copyfile(backup, real)
        run(['python3', 'tools/makedisk.py', 'build'])
    print('PASS: a module that offers no engine binds, reports the chip, and leaves the CPU compositor in '
          'charge on a booted machine - measured in the serial log, the graphics panel and the report')


def test():
    test_the_claims_match_the_table_that_authorises_them()
    test_the_family_record_counts_what_the_table_holds()
    test_the_provenance_and_the_no_write_rule_are_in_the_source()
    test_the_harness_runs_the_module()
    test_the_module_builds_for_the_kernel_it_ships_with()
    test_the_kernel_holds_the_identification_state_on_a_device()


if __name__ == '__main__':
    test()
    print('PASS: drivers/gpu/nvidia identifies a Blackwell display function from its own boot register, '
          'refuses when the register block does not answer, offers no engine, and is authorised by the '
          'generated table it is checked against')

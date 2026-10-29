"""The self-test a person can run on the machine they just flashed.

`tools/tests/*` asks questions from outside, over QEMU and a debugger, which is
how a build is verified but not how a machine is trusted the first time it
boots.  `selftest` in the Terminal asks the same kind of questions from inside:
the planted decoys, the per-app storage quota, the CPL3 boundary, which renderer
is really painting, a filesystem round trip, the terminal's own row arithmetic
and the app registry.

The test here runs that command on a fresh guest and requires every line to say
PASS, the summary to agree with the number of lines, and nothing it created to
be left behind - a self-test that leaves litter in the tree is one whose second
run starts from a different machine.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'tests'))

from qemu_support import Guest          # noqa: E402


def send(g, text, chunk=8):
    for i in range(0, len(text), chunk):
        g.type(text[i:i + chunk])
        g.run(.12)


def transcript(g, win, tail=40):
    n = g.call('term_debug', win, 7)
    return [g.string(g.call('term_line', win, i)) for i in range(max(0, n - tail), n)]


def test_selftest_passes_from_inside_the_running_system():
    with Guest('selftest') as g:
        term = g.launch('terminal')
        g.call('wm_focus', term)
        g.run(.3)
        send(g, 'selftest\n')
        lines = []
        for _ in range(50):
            g.run(.2)
            lines = transcript(g, term)
            if any(l.startswith('SELF-TEST') for l in lines):
                break
        assert any(l.startswith('SELF-TEST PASSED') for l in lines), '\n'.join(lines[-14:])
        assert not any(l.startswith('FAIL') for l in lines), '\n'.join(lines[-14:])

        checks = [l for l in lines if l.startswith(('PASS  ', 'FAIL  '))]
        summary = [l for l in lines if l.startswith('SELF-TEST')][-1]
        passed, total = summary.split(':')[1].split()[:3:2]
        assert int(passed) == len(checks), (summary, len(checks))
        assert int(total) == len(checks), (summary, len(checks))
        assert len(checks) >= 7, checks

        # Each check names what it measured rather than saying "ok".
        for needle in ('decoy files', 'app storage', 'CPL3 user mode', 'renderer',
                       'filesystem', 'terminal layout', 'app registry'):
            assert any(needle in l for l in checks), 'no check reported ' + needle
        # The renderer line is one of two honest answers, never a claim.
        assert any('CPU compositor is painting' in l or '2D engine is painting' in l
                   for l in checks), checks

        # It cleaned up after itself, and left the decoys alone.
        assert g.read('home/selftest.tmp') is None, 'the self-test left a temporary file behind'
        for i in range(g.call('decoys_count')):
            assert g.call('decoys_present', i) == 1, 'the self-test disturbed a decoy'
        assert 'panic' not in g.serial().lower()
        print('\n'.join(checks), flush=True)
    print('PASS: the running system checks itself and every check names its measurement')


if __name__ == '__main__':
    test_selftest_passes_from_inside_the_running_system()

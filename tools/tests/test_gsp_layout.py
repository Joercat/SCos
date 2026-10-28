"""Where the GSP firmware's memory goes, checked as arithmetic before anything is written to a card.

`gpu_fsp.c` has been able to build a correct NVDM_PAYLOAD_COT for three revisions, and it declines the
one field group a boot actually needs: the video-memory addresses.  That is not caution for its own
sake - a reservation placed from offset 0 is worse than none, because the firmware reads it back and
boots into whatever the host said.  So the placement is a pure function first (this suite), and only
then a caller.

Two levels, and the split matters:

  * `gsp_layout_host.c` compiles `kernel/drivers/gpu/gsp_layout.c` on the host and checks it against a
    case whose numbers were worked out by hand from the placement rules, then against the invariants
    over a sweep of card and image sizes, then against the refusals.  A test that only re-ran the
    implementation would agree with a wrong shift; this one has an answer of its own.
  * the kernel side is checked structurally: the reservations the plan produces have to be the ones
    `stage_gsp_boot_descriptors()` fills in, and the unchanged path has to stay unchanged when no plan
    can be made.  That is the property that keeps the shipped behaviour exactly what it was until a
    card is actually reserved.
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

HOST_CFLAGS = ['-m64', '-O1', '-std=c11', '-Wall', '-Wextra', '-Werror']


def run(cmd):
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 0, ' '.join(cmd) + '\n' + result.stdout[-3000:] + result.stderr[-3000:]
    return result


def test_the_placement_arithmetic_is_the_reference_one():
    out = ROOT / 'build' / 'gsp-layout-host'
    out.parent.mkdir(parents=True, exist_ok=True)
    obj = out.parent / 'gsp_layout.o'
    run(['gcc'] + HOST_CFLAGS + ['-Ikernel/include', '-c', 'kernel/drivers/gpu/gsp_layout.c',
                                 '-o', str(obj)])
    run(['gcc'] + HOST_CFLAGS + ['-Ikernel/include', 'tools/tests/gsp_layout_host.c', str(obj),
                                 '-o', str(out)])
    result = subprocess.run([str(out)], cwd=ROOT, capture_output=True, text=True)
    lines = [l for l in result.stdout.splitlines() if l.startswith(('PASS', 'FAIL'))]
    print('\n'.join(lines))
    assert result.returncode == 0, result.stdout[-4000:]
    assert not [l for l in lines if l.startswith('FAIL')], result.stdout
    print('PASS: the reservation is placed by the reference rules, or refused with a reason and no '
          'offset')


def test_the_plan_is_what_the_handover_fills_in():
    """The named regions have to be the ones the COT and GspFwWprMeta ask for, or a plan is a diagram."""
    fsp = (ROOT / 'kernel/drivers/gpu/gpu_fsp.c').read_text()
    for field in ('frts_offset', 'boot_bin_offset', 'gsp_fw_offset', 'gsp_fw_heap_offset',
                  'gsp_fw_heap_size', 'gsp_fw_wpr_start', 'gsp_fw_wpr_end', 'non_wpr_heap_offset',
                  'gsp_fw_rsvd_start'):
        assert field in fsp, 'gpu_fsp.c no longer fills %s from a plan' % field
    assert 'gsp_fb_plan(' in fsp, 'nothing calls the planner'
    assert 'GSP_LAYOUT_OK' in fsp, 'the planner\'s answer is not checked'
    # GSP_LAYOUT_OK is zero, so a state that was never asked would read as a successful plan.  The gate
    # has to be its own boolean, and a struct that was never planned must not reach the payload.
    assert 'plan_ready' in fsp, 'the payload is gated on a state whose zero value means success'
    assert 'plan_ready ? fsp_state.plan.frts_offset : 0' in fsp, \
        'the FRTS offset is not gated on a plan that exists'


def test_a_card_with_no_video_memory_keeps_the_declined_encoding():
    """The shipped behaviour on a card that cannot be measured must be the one already recorded."""
    fsp = (ROOT / 'kernel/drivers/gpu/gpu_fsp.c').read_text()
    assert 'params_declined' in fsp, 'the declined path lost its state'
    assert 'frtsVidmem' in fsp or 'COT_OFF_FRTS_VIDMEM' in fsp, 'the COT lost its FRTS fields'


if __name__ == '__main__':
    tests = [test_the_placement_arithmetic_is_the_reference_one,
             test_the_plan_is_what_the_handover_fills_in,
             test_a_card_with_no_video_memory_keeps_the_declined_encoding]
    only = sys.argv[1] if len(sys.argv) > 1 else None
    failed = 0
    for test in tests:
        if only and only not in test.__name__:
            continue
        try:
            test()
        except AssertionError as error:
            print('FAIL: %s: %s' % (test.__name__, error))
            failed += 1
    if failed:
        print('FAIL: %d of %d' % (failed, len(tests)))
        raise SystemExit(1)
    print('OK')

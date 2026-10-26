"""The card's own security processor: its mailbox, its one request, and the payload that would boot the
coprocessor.

`tools/tests/gpu_fsp_host.c` is the substance of this: kernel/drivers/gpu/gpu_fsp.c compiled against a model of
the register file, asserting the bytes of a framed request, the order and values of the writes that put it in
the channel, and every reason the file refuses to send.  What is checked here, in python, is the shape around it
- that the offsets the model derives from the production source are the ones the published register description
gives (a test that derived its constants from the same place the driver does could never notice both being
wrong), that the handover payload's fields are where the vendor's header puts them, that the probe is gated on
the vendor whose decode these addresses are, and that none of it is mapped write-combined, because a mailbox
whose stores sit in a combination buffer is a mailbox nobody can trust to have arrived.

The last two tests are the ones that cost a QEMU boot and are worth it: the paragraph has to reach a real
panel, and on a machine with no NVIDIA function it has to say so while touching nothing.
"""

import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

FSP_CHECKS = 72                     # the modelled mailbox, pinned: a dropped case is a silent loosening

# The published register description, transcribed rather than read out of the file under test:
# src/common/inc/swref/published/hopper/gh100/dev_fsp_pri.h, which is the header GB100's hardware-abstraction
# layer includes, plus dev_therm.h and dev_falcon_v4.h from the same directory.  NVIDIA, MIT, 2026-10-26.
PUBLISHED = {
    'NV_PFSP_EMEMC': 0x008f2ac0,
    'NV_PFSP_EMEMD': 0x008f2ac4,
    'NV_PFSP_QUEUE_HEAD': 0x008f2c00,
    'NV_PFSP_QUEUE_TAIL': 0x008f2c04,
    'NV_PFSP_MSGQ_HEAD': 0x008f2c80,
    'NV_PFSP_MSGQ_TAIL': 0x008f2c84,
    'NV_THERM_FSP_BOOT_COMPLETE': 0x000200bc,
    'NV_PFSP_UCODE_VERSION': 0x008f0334,
    'NV_PFSP_FUSE_ERROR_CHECK': 0x008f033c,
    'NV_PGSP_HWCFG2': 0x001100f4,
}

# NVDM_PAYLOAD_COT from src/nvidia/arch/nvalloc/common/inc/kernel/gpu/fsp/kern_fsp_cot_payload.h, packed:
# version and size, the FMC image's physical address, the resume store's four fields, the three authentication
# blobs, and the boot-argument pointer that closes the record.
COT_FIELDS = [
    ('version', 0, 2), ('size', 2, 2), ('gspFmcSysmemOffset', 4, 8), ('frtsSysmemOffset', 12, 8),
    ('frtsSysmemSize', 20, 4), ('frtsVidmemOffset', 24, 8), ('frtsVidmemSize', 32, 4), ('hash384', 36, 48),
    ('publicKey', 84, 384), ('signature', 468, 384), ('gspBootArgsSysmemOffset', 852, 8),
]

GCC = ['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-ffreestanding', '-fno-builtin',
       '-fno-stack-protector', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
       '-Ikernel/include']


def run(cmd, **kwargs):
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, **kwargs)
    assert result.returncode == 0, ' '.join(str(x) for x in cmd) + '\n' + result.stdout + result.stderr
    assert not result.stderr.strip(), ' '.join(str(x) for x in cmd) + '\n' + result.stderr
    return result


def source(name):
    return (ROOT / name).read_text()


def test_the_modelled_mailbox_sends_exactly_the_published_framing():
    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / 'gpu_fsp_host'
        run(GCC + ['tools/tests/gpu_fsp_host.c', '-o', str(binary)])
        result = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, text=True)
        assert not result.stderr.strip(), 'the sanitizers said something: ' + result.stderr
        m = re.match(r'^(\d+) check\(s\), (\d+) failure\(s\)$', result.stdout.strip())
        assert m, 'the mailbox harness did not report its count: ' + result.stdout[-400:]
        assert int(m.group(1)) == FSP_CHECKS, \
            'the mailbox harness reports %s checks, pinned at %d - a scenario was dropped or added without ' \
            'this file noticing' % (m.group(1), FSP_CHECKS)
        assert int(m.group(2)) == 0, result.stdout


def test_the_registers_are_the_ones_the_silicon_publishes():
    text = source('kernel/drivers/gpu/gpu_fsp.c')
    for name, value in PUBLISHED.items():
        m = re.search(r'#define %s\s+(0x[0-9a-f]+)u' % name, text)
        assert m, '%s is not defined in the kernel file at all' % name
        assert int(m.group(1), 16) == value, \
            '%s is 0x%x in this tree and 0x%x in the published register description' % (
                name, int(m.group(1), 16), value)
    # The channel's geometry, because every length in the file is derived from it: a channel that is 1 KiB of
    # dwords holds 255 dwords of payload plus two header dwords, and the file's refusal of 1017-byte payloads
    # is only right while these two numbers are these.
    assert re.search(r'#define FSP_EMEM_CHANNEL_BYTES\s+1024u', text), 'the RM channel is not 1 KiB here'
    assert re.search(r'#define FSP_EMEM_DWORDS_PER_BLOCK\s+64u', text), 'the blocks are not 64 dwords here'


def test_the_handover_payload_is_laid_out_as_the_header_lays_it_out():
    """Every offset the payload is assembled with, as a set, against the published record's field positions.

    The C harness checks that each field's *bytes* land where these offsets say; this checks that these offsets
    are the published ones, which a harness deriving its own constants from the same file could not.  A field
    moved here is a card that reads a signature where it expects a key, which is not a failure a person
    debugging over a serial cable has any way to guess."""
    text = source('kernel/drivers/gpu/gpu_fsp.c')
    here = sorted(int(n) for n in re.findall(r'#define COT_OFF_[A-Z_]+\s+(\d+)u', text))
    published = sorted(offset for _, offset, _ in COT_FIELDS)
    assert here == published, \
        'the handover is built at %s, and the fields the published payload declares are at %s' % (here, published)
    widths = {offset: size for _, offset, size in COT_FIELDS}
    for name, offset, size in COT_FIELDS:
        if size == 48:
            assert 'memcpy(fsp_state.payload + COT_OFF_HASH' in text, 'the 48-byte hash is not copied as 48'
        if size == 8 and offset == 852:
            assert 'store64(fsp_state.payload + COT_OFF_BOOT_ARGS' in text, \
                'the boot-argument pointer is not stored as the 8 bytes it is'
    assert widths == {0: 2, 2: 2, 4: 8, 12: 8, 20: 4, 24: 8, 32: 4, 36: 48, 84: 384, 468: 384, 852: 8}
    total = re.search(r'#define GPU_FSP_PAYLOAD_BYTES (\d+)', source('kernel/include/gpu.h'))
    assert total and int(total.group(1)) == 860, \
        'the buffer the payload is built in is not the 860 bytes NVDM_PAYLOAD_COT is'
    last = COT_FIELDS[-1]
    assert last[1] + last[2] == 860, \
        'the boot-argument pointer no longer closes the payload: the record would have a hole or a tail'


def test_the_probe_is_gated_and_the_doorbell_is_counted():
    text = source('kernel/drivers/gpu/gpu_fsp.c')
    assert 'device->vendor != 0x10deu' in text, \
        'the mailbox is no longer gated on the vendor whose BAR decode these offsets are, which means a boot ' \
        'on another company\'s card would write into it'
    assert text.count('device_map(') == 3, 'this file maps %d windows, not the three it describes' % \
        text.count('device_map(')
    for call in re.finditer(r'device_map\([^;]*?\)', text, re.S):
        assert ', 0, &mapped)' in call.group(0), \
            'a mailbox window asked for write combining: %s' % call.group(0).replace('\n', ' ')
    assert len(re.findall(r'write32\(&w_', text)) == 8, \
        'this file writes the card with %d call sites, and the eight it should have are one control, three ' \
        'data, two queue pointers out, and two reply pointers back' % len(re.findall(r'write32\(&w_', text))
    # The two queue registers are written, and the only other writes are to the channel.  Pinned as a count of
    # write32 call sites because "how many registers did the OS touch" is the question this whole feature turns
    # on, and it should be answerable from the source before it is answerable from a card.


def test_the_panel_says_what_was_asked_and_what_was_not():
    panel = source('kernel/desktop/graphics.c')
    assert 'put_fsp_report(&w, gpu_fsp_state());' in panel, \
        'the paragraph exists but is never emitted, so the panel is quiet about the mailbox either way'
    for needle in ('FSP mailbox: ', 'not asked, and not because anything failed',
                   '0xbadf4100 refusal that says the coprocessor',
                   'It is not sent: asking the FSP to boot the coprocessor'):
        assert needle in panel, 'the panel no longer says: %s' % needle
    boot = source('kernel/drivers/gpu/gpu_detect.c')
    assert 'gpu_fsp_note(&devices[pick], handoff);' in boot, \
        'the probe is no longer reached from detection, so a boot with no staged module asks nothing'
    assert boot.count('gpu_fsp_note(') == 1, \
        'detection asks the security processor from %d places, and a panel that shows one answer cannot ' \
        'say which of them produced it' % boot.count('gpu_fsp_note(')
    assert 'gpu_firmware_note(handoff);' in boot, \
        'the firmware record is no longer noted before the handover is built, and the handover would be ' \
        'described as absent on a machine that has it'


def main():
    tests = [(n, f) for n, f in sorted(globals().items()) if n.startswith('test_') and callable(f)]
    for name, fn in tests:
        fn()
    print('PASS: the modelled mailbox, the published offsets it is pinned against, the handover payload\'s '
          'field positions, the vendor gate and write-combining ban, and the paragraph that says what was '
          'and was not asked')


if __name__ == '__main__':
    main()

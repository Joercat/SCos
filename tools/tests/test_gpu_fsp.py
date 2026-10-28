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

FSP_CHECKS = 119                    # the modelled mailbox, pinned: a dropped case is a silent loosening
FSP_SIZES = (48, 97, 96)            # what a GB2xx FSP asks the container for, from the reference driver

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
    assert text.count('device_map(') == 5, 'this file maps %d windows, not the five it describes' % \
        text.count('device_map(')
    for call in re.finditer(r'device_map\([^;]*?\)', text, re.S):
        assert ', 0, &mapped)' in call.group(0), \
            'a mailbox window asked for write combining: %s' % call.group(0).replace('\n', ' ')
    assert len(re.findall(r'write32\(&w_', text)) == 10, \
        'this file writes the card with %d call sites, and the ten it should have are one control, three data, ' \
        'two queue pointers out, two reply pointers back, and the pair that clears the reply queue when a ' \
        'refusal has left us no way to tell whether a store would land at all' % len(re.findall(r'write32\(&w_', text))
    # The two queue registers are written, and the only other writes are to the channel.  Pinned as a count of
    # write32 call sites because "how many registers did the OS touch" is the question this whole feature turns
    # on, and it should be answerable from the source before it is answerable from a card.


def test_the_panel_says_what_was_asked_and_what_was_not():
    panel = source('kernel/desktop/graphics.c')
    assert 'put_fsp_report(&w, gpu_fsp_state());' in panel, \
        'the paragraph exists but is never emitted, so the panel is quiet about the mailbox either way'
    for needle in ('FSP mailbox: ', 'not asked, and not because anything failed',
                   '0xbadf4100 refusal that says the coprocessor',
                   # The handover's three endings, each of which has to stay sayable: sent and the block opened,
                   # sent and it did not, and not sent because the queue said it was busy.  The middle clause of
                   # the first sentence used to read "only after the FSP has answered a query cleanly", which was
                   # the third self-written precondition this card disproved - it guaranteed the one message that
                   # matters was never put on the wire - so the pin is now the sentence that replaced it, and the
                   # reason for replacing it.
                   'first and without conditions, because that is what the driver that boots this chip does',
                   'the block that refused every read on this panel is answering',
                   'still the locked-target refusal - the FSP took the message ',
                   'A submission also needs a clock and a doorbell',
                   # the other half of the same sentence pair: what the panel says when a handover did go out
                   'including one handover of a firmware image the boot volume supplied',
                   'a code this build does not have a name for'):
        assert needle in panel, 'the panel no longer says: %s' % needle
    # The tail is printed whole or not at all, and the reader is told where the rest is.  This is the one
    # structural answer to the pastes that came back cut inside the detection list: a limit that says so is a
    # different thing from a sentence that ends in the middle of a number.
    assert '#define FSP_TAIL_LIMIT 6144' in panel, 'the tail has no scratch of its own any more, so its ' \
        'length cannot be known before it is written'
    assert 'if (need + 1u <= w.left && need + 1u < sizeof tail)' in panel, \
        'the three tail paragraphs can be printed partly again'
    assert "more than this build's own tail buffer of " in panel, 'a tail longer than its own scratch is ' \
        'not reported as its own finding'
    assert 'Run `graphics fsp`' in panel, 'the notice that replaces them does not say where the rest went'
    # The size is read back out of the header rather than repeated here, because this pin exists to notice when
    # the caller's buffer and the tail scratch stop agreeing - and it did its job: the report grew past 8192 and
    # the tail was being dropped mid-word with no notice, so the buffer is 24576 now and must stay comfortably
    # above the scratch the panel writes its tail into.
    hdr = source('kernel/include/scos.h')
    cap = re.search(r'#define\s+SCOS_GRAPHICS_REPORT\s+(\d+)', hdr)
    assert cap, 'the caller buffer is no longer a named constant, so the tail scratch is sized against nothing'
    assert int(cap.group(1)) >= 16384, \
        'the caller buffer is %s bytes, and the measured report needs room for the tail as well' % cap.group(1)
    assert 'FSP_TAIL_LIMIT 6144' in source('kernel/desktop/graphics.c'), \
        'the tail scratch no longer states its own size, so a clipped tail is not knowable'

    boot = source('kernel/drivers/gpu/gpu_detect.c')
    assert 'gpu_fsp_note(&devices[pick], handoff);' in boot, \
        'the probe is no longer reached from detection, so a boot with no staged module asks nothing'
    assert boot.count('gpu_fsp_note(') == 1, \
        'detection asks the security processor from %d places, and a panel that shows one answer cannot ' \
        'say which of them produced it' % boot.count('gpu_fsp_note(')
    assert 'gpu_firmware_note(handoff);' in boot, \
        'the firmware record is no longer noted before the handover is built, and the handover would be ' \
        'described as absent on a machine that has it'


def test_the_one_line_at_the_front_says_what_the_paragraphs_say():
    """The panel's decision belongs where a truncated copy still has it.

    Three real-card reports ended in the middle of a word, always before the paragraph that answers the
    question, so the answer is not a shorter paragraph: it is the same state, compressed, on the first line -
    and the first line is the one thing that has arrived every time.  Pinned here as *position* and *shape*;
    the length bound and the survival of the line through a 1400-byte copy are measured on a booted machine in
    test_gsp_firmware.py, because a limit derived from a fixture is a limit nobody believes.
    """
    panel = source('kernel/desktop/graphics.c')
    assert 'static void put_fsp_headline(struct writer *w, const struct gpu_fsp_state *f)' in panel, \
        'the summary line is gone, so the front of the panel is back to being prose'
    assert panel.count('put_fsp_headline(&w, gpu_fsp_state());') == 2, \
        'the headline is on %d of the two surfaces, and the narrow one is the one people are told to paste' \
        % panel.count('put_fsp_headline(&w, gpu_fsp_state());')
    assert panel.index('put_fsp_headline(&w, gpu_fsp_state());') < panel.index('put(&w, "Renderer: ")'), \
        'the summary moved below the longest text on the panel, which is the position that keeps losing it'
    start = panel.index('static void put_fsp_headline')
    body = panel[start:panel.index('static void put_fsp_report', start)]
    for needle in ('FSP: ', 'not booted (waited ', 'query answered', 'handover ', ' released', '" locked',
                   'clock 0x9400 ', 'VF page ', ', fw ', 'card reads ', 'Full text: `graphics fsp`'):
        assert needle in body, 'the headline no longer carries %r, so the paragraph is the only place it is said' % needle
    # Every clause has to come from the state the paragraphs use, not from a re-measurement: two reads of the
    # same register on the same boot can disagree, and a summary that disagreed with its own detail would be
    # the worst possible thing to put first.
    assert 'read32' not in body and 'gpu_io_' not in body, \
        'the headline reads the card itself, so it can disagree with the paragraph below it'
    assert body.count('f->') > 25 and 'f->query.error' in body and 'f->handover' in body, \
        'the headline stopped quoting the fields the paragraphs quote'


def test_the_generation_table_is_the_reference_driver_and_not_a_convenience():
    """The blob widths and the container version are per-chip facts in the driver that boots these cards, and
    they are not the numbers this file started with.

    `nvkm_fsp_func.cot' in nvkm/subdev/fsp/{gh100,gb100,gb202}.c of the Linux kernel's nouveau driver (MIT,
    fetched from the torvalds/linux tree on 2026-10-26) is the table below, transcribed rather than derived: a
    harness that read its expectations out of the driver under test could not notice both being wrong, and the
    consequence of being wrong here is that a person's real firmware file is refused as malformed.
    """
    table = {
        0x18: (1, 48, 384, 384),       # gh100_fsp: Hopper
        0x1a: (2, 48, 97, 96),         # gb100_fsp: Blackwell, the GB10x chips
        0x1b: (2, 48, 97, 96),         # gb202_fsp: Blackwell, the GB2xx chips - this project's target
    }
    text = source('kernel/drivers/gpu/gpu_fsp.c')
    body = text[text.index('static void cot_parameters(void)'):]
    body = body[:body.index('\n}\n')]
    for arch, (version, hash_bytes, pkey, sig) in table.items():
        # Up to this case's own `break;', which is how the two Blackwell rows share a body in the file: the
        # pin asks what the chip would be given, not how many lines the switch happens to have.
        case = re.search(r"case 0x%xu:.*?break;" % arch, body, re.S | re.I)   # the file writes 0x1Au upper-case
        assert case, 'architecture 0x%x has no row in cot_parameters(), so the chip would be measured against ' \
                     'whatever the file happened to assume' % arch
        clause = case.group(0)
        for name, value in (('cot_version', version), ('cot_hash_bytes', hash_bytes),
                            ('cot_pkey_bytes', pkey), ('cot_sig_bytes', sig)):
            if name == 'cot_hash_bytes':
                continue                    # 48 is set once above the switch, for every row that applies
            assert re.search(r'%s = %uu' % (name, value), clause), \
                'architecture 0x%x wants %s to be %d (the reference driver\'s table) and the file sets it ' \
                'from one hard-coded size for every chip' % (arch, name, value)
    assert FSP_SIZES == table[0x1b][1:], 'this file and the driver disagree about what a GB2xx FSP wants'
    # The two "not this chip" outcomes have to be said, not swallowed: an unknown architecture and an
    # architecture with no FSP are different sentences, and neither is a defect in anybody's file.
    assert 'fsp_state.cot_defect = fsp_state.arch_unknown ? 9u : 10u;' in text, \
        'the generation gate no longer records which of the two it refused, so a refusal to attempt looks like ' \
        'a broken container'
    gate = text[text.index('if (!fsp_state.fsp_supported) {'):]
    assert 'return;' in gate[:900], 'the gate no longer stops the probe: a chip whose generation is unknown ' \
                                    'would still get a packet written into its aperture'
    # ...and the panel has to print the expectation, not just the verdict.
    panel = source('kernel/desktop/graphics.c')
    for needle in ('The architecture field of NV_PMC_BOOT_0 reads 0x', 'whose FSP wants a hash of ',
                   "Hopper's table says 48/384/384", 'no FSP mailbox for arch 0x', 'bit13=',
                   "NV_PFALCON_FALCON_HWCFG2_RISCV_BR_PRIV_LOCKDOWN", "The falcon's own mailbox pair read 0x",
                   'ms of the ', 'reads at 0x200bc', 'cot '):
        assert needle in panel, 'the panel never prints %r, so a refusal cannot be told from a guess' % needle


def test_the_time_bounds_are_the_working_driver_s():
    """Two of this path's failures were not the card\'s: a wait bounded by a count of uncached reads is tens of
    times shorter than the driver that works, and the completion test was "the word was not zero" instead of the
    lockdown bit.  Both are pinned to the published numbers here, so they cannot quietly drift back.
    """
    text = source('kernel/drivers/gpu/gpu_fsp.c')
    for name, value in (('FSP_WAIT_REPLY_MS', 1500), ('FSP_WAIT_BOOT_MS', 4000),
                        ('FSP_HWCFG2_LOCKDOWN_BIT', 13)):
        m = re.search(r'#define %s\s+(\d+)u' % name, text)
        assert m, '%s is not a named bound in the file any more' % name
        assert int(m.group(1)) == value, \
            '%s is %s here and %s in nvkm/subdev/fsp (gh100_fsp_send, wait_secure_boot, dev_falcon_v4.h)' % (
                name, m.group(1), value)
    m = re.search(r'#define FSP_BOOT_COMPLETE_SUCCESS\s+(0x[0-9a-f]+)u', text)
    assert m and int(m.group(1), 16) == 0xff, \
        'boot-complete is no longer compared against the whole 0x000000FF the register publishes as SUCCESS'
    # The falcon pair the FMC answers through, at the coprocessor's own base.
    for name, offset in (('NV_PGSP_MAILBOX0', 0x40), ('NV_PGSP_MAILBOX1', 0x44)):
        m = re.search(r'#define %s\s+0x0011(\d{4})u' % name, text)
        assert m and int(m.group(1), 16) == offset, '%s is not at the falcon\'s offset 0x%x' % (name, offset)
    # A reply is acknowledged by restarting the ring, because the second message - the handover - is read out
    # of offset 0 and nothing else.
    ack = text[text.index('static int read_reply'):]
    ack = ack[:ack.index('\n}\n')]
    assert 'NV_PFSP_MSGQ_TAIL, 0' in ack and 'NV_PFSP_MSGQ_HEAD, 0' in ack, \
        'the acknowledge moved back to pulling the tail up to the head, which leaves the next reply somewhere ' \
        'this driver does not look'
    assert 'wait_clocked' in text and 'ticks_per_ms' in text, \
        'the waits are no longer reported as timed or untimed, which is the difference between a bound and a ' \
        'story about time'


def test_no_silent_cap_between_a_measurement_and_the_screen():
    """The report was cut off in the terminal twice, and both times the reason was a fixed-size copy sitting in
    the middle of the path rather than the buffer at either end: the panel writer clipped against its buffer and
    said so in words, while the terminal's typewriter held `pending[2048]' and said nothing, so the last clause
    - the one with the answer in it - disappeared inside a display component.  A buffer may be bounded if it
    announces it; a copy may not.  These are the places a byte can be dropped on the way from a register read to
    a pixel, each pinned to either be unbounded or to report its own limit, and the transport version the card
    is sent, which is why a query answered nothing at all."""
    panel = source('kernel/desktop/graphics.c')
    detection = source('kernel/drivers/gpu/gpu_detect.c')
    terminal = source('kernel/desktop/app_terminal.c')
    for name, text in (('the panel writer', panel), ('the detection report', detection)):
        assert 'w->dropped += n - room;' in text, name + ' still has a put() that can lose bytes without counting them'
        assert text.count('put_note(') >= 3, name + ' counts lost bytes but never tells the reader how many'
    assert not re.search(r'^\s+char pending\[', terminal, re.M), \
        'the terminal is back to holding typewriter text in a fixed array, which clips a long response in silence'
    assert 'palloc_owned(len + 1, HEAP_WM)' in terminal, \
        'term_type() is not allocating the answer to its own length'
    assert 'char pbuf[512]' not in terminal, 'the preview of a response is capped at 512 bytes again'
    rows = re.search(r'#define\s+TERM_LINES\s+(\d+)', terminal)
    assert rows and int(rows.group(1)) >= 1000, 'the scrollback is bounded below a screenful of the GPU report'
    report = re.search(r'#define\s+SCOS_GRAPHICS_REPORT\s+(\d+)', source('kernel/include/scos.h'))
    assert report and 'static char detail[' in panel and \
        'detail[SCOS_GRAPHICS_REPORT]' in panel, 'the report is handed to the terminal through a smaller ' \
        'buffer than the one it was assembled in, which is where 2560 used to bite'
    fsp = source('kernel/drivers/gpu/gpu_fsp.c')
    version = re.search(r'#define\s+MCTP_VERSION\s+(\d+)u', fsp)
    assert version and version.group(1) == '0', \
        'the MCTP transport header is being sent with a version the specification does not define; the ' \
        'reference driver leaves bits 3:0 clear, and a packet dropped for its version comes back as silence ' \
        'rather than as an error code'


def main():
    tests = [(n, f) for n, f in sorted(globals().items()) if n.startswith('test_') and callable(f)]
    for name, fn in tests:
        fn()
    print('PASS: the modelled mailbox, the published offsets it is pinned against, the handover payload\'s '
          'field positions, the vendor gate and write-combining ban, and the paragraph that says what was '
          'and was not asked')


if __name__ == '__main__':
    main()

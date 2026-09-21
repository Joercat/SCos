#!/usr/bin/env python3
"""Pack one linked relocatable object into a SCos GPU display module.
Packing is mechanical and original to SCos.  What it packs is not: the objects come from the ported
family code under drivers/gpu/, whose upstream provenance and licence each module states in its own
source header (the first module, `ati', descends from Haiku's rage128 accelerator under the MIT
licence).  Nothing in this script is copied from an upstream build system.

    tools/build_gpu_module.py --input build/gpu/ati.o --family ati --out build/gpu/ati.mod

The input is the result of `ld -r` over the module's own objects, so the packer sees a plain ELF
relocatable file: no program headers, no dynamic section, no imports.  The output is the flat image
described in kernel/include/gpu_abi.h -- two blocks of bytes to copy, a table of relocations to
apply, and metadata the kernel loader cross-checks against the generated detection table.

Refusals are the point of this tool, because every case it rejects would otherwise become a
special case inside the kernel's loader:

  * any undefined symbol -- a module may not import anything; it provides its own memcpy/memset and
    reaches the kernel only through the export table it is handed;
  * any relocation type outside the four the loader implements (abs64, abs32, pc32, plt32);
  * a relocation into a section the packer did not lay out (a stray runtime table, TLS, or a
    debug section that survived the link);
  * an entry point or ID table outside the read/execute block;
  * an image larger than the reserved module region.

--verify re-checks a produced file (the same predicates the kernel applies, so a module that fails
here cannot pass there), and is what the test suite runs.
"""

import argparse
import binascii
import os
import struct
import sys

ABI_VERSION = 1
MAGIC = 0x4D504753  # "SGPM"
HEADER_SIZE = 192
FAMILY_NAME = 16
MAX_IMAGE = 256 * 1024 - 192  # the boot arena's module region, minus the header
ID_RULE_SIZE = 20
RELOC_SIZE = 16            # sizeof(struct scos_gpu_reloc) in kernel/include/gpu_abi.h

RXE, DATA, BSS = 0, 1, 2
BLOCK_NAMES = {RXE: "rxe", DATA: "data", BSS: "bss"}

# ELF x86-64 relocation numbers, and the four the loader understands.
R_X86_64_NONE, R_X86_64_64, R_X86_64_PC32 = 0, 1, 24
R_X86_64_PLT32, R_X86_64_32S, R_X86_64_32 = 4, 10, 11
RELOC_TYPE = {R_X86_64_64: 1, R_X86_64_32S: 10, R_X86_64_32: 10, R_X86_64_PC32: 24,
              R_X86_64_PLT32: 4}
# A GOT-relative reference is legal in a module only in the one case where it needs no dynamic
# linker: the symbol is defined right here, so the reference can be folded to a plain PC-relative
# one, which is exactly what a final link's relaxation does.  Anything else that mentions the GOT
# is refused, because a module has no .got to point at (and the ALLOC check above would also catch
# a real .got section).
GOTPCREL = {2, 9}          # R_X86_64_REX_GOTPCRELX, R_X86_64_GOTPCREL
SHT_RELA, SHT_NOBITS = 4, 8
SECTION_KIND = {".text": RXE, ".rodata": RXE, ".data": DATA}

HEADER_FIELDS = "<IIIII16s17I8I"


class Packed(Exception):
    pass


def elf_sections(blob):
    # ELF64 file header: shoff@40 flags@48 ehsize@52 phentsize@54 phnum@56 shentsize@58
    # shnum@60 shstrndx@62 - the ordering below is that layout, not a guess at it.
    (e_shoff, sh_flags, _ehsize, _phentsize, _phnum, e_shentsize, e_shnum, e_shstrndx) = \
        struct.unpack_from("<QIHHHHHH", blob, 40)
    if e_shoff == 0 or e_shnum == 0:
        raise Packed("object has no section headers")
    names_at = e_shoff + e_shstrndx * e_shentsize
    strtab = struct.unpack_from("<Q", blob, names_at + 24)[0]
    sections = []
    for index in range(e_shnum):
        base = e_shoff + index * e_shentsize
        (sh_name, sh_type, sh_flags, _addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign,
         sh_entsize) = struct.unpack_from("<IIQQQQIIQQ", blob, base)
        end = blob.index(b"\0", strtab + sh_name)
        sections.append(dict(index=index, sname=blob[strtab + sh_name:end].decode(), type=sh_type, flags=sh_flags,
                             offset=sh_offset, size=sh_size, link=sh_link, info=sh_info,
                             addralign=max(1, sh_addralign), entsize=max(1, sh_entsize),
                             data=blob[sh_offset:sh_offset + sh_size]))
    return sections


def elf_symbols(blob, sections):
    for section in sections:
        if section["sname"] != ".symtab":
            continue
        strtab = sections[section["link"]]["offset"]
        out = []
        for slot in range(section["size"] // 24):
            (st_name, st_info, _other, st_shndx, st_value, st_size) = \
                struct.unpack_from("<IBBHQQ", section["data"], slot * 24)
            end = blob.index(b"\0", strtab + st_name)
            out.append((blob[strtab + st_name:end].decode(), st_shndx, st_value, st_size,
                        st_info & 0xF))
        return out
    raise Packed("the module object has no .symtab; check the link command")


def section_kind(name):
    for prefix, kind in SECTION_KIND.items():
        if name == prefix or name.startswith(prefix + "."):
            return kind
    return None


def header_checksum(words):
    """Sum of every header word with the checksum field itself treated as zero.  The kernel loader
    recomputes it the same way; it exists so a header whose size fields were torn by a bad write is
    refused instead of acted on."""
    total = 0
    for index, word in enumerate(words):
        if index == 3:
            word = 0
        total = (total + word) & 0xFFFFFFFF
    return total


def pack(args):
    blob = open(args.input, "rb").read()
    if blob[:4] != b"\x7fELF" or blob[4] != 2:
        raise Packed("%s is not a 64-bit ELF object" % args.input)
    if struct.unpack_from("<H", blob, 16)[0] != 1:
        raise Packed("%s is not ET_REL; a module is `ld -r' output, not a linked program"
                     % args.input)
    sections = elf_sections(blob)
    symbols = elf_symbols(blob, sections)

    # Lay the image out: read/execute (text + rodata) first, then writable data, then .bss sized
    # but not stored.  Section order inside a block follows the object, so the compiler's ordering
    # is preserved.  An ALLOC section outside those three families is a build error rather than a
    # silent drop: it would mean the module contains a table nobody thought about.
    placed, blocks, bss_total = {}, {RXE: bytearray(), DATA: bytearray()}, 0
    for section in sections:
        name = section["sname"]
        if not section["size"] or name.startswith((".rela", ".rel.", ".symtab", ".strtab",
                                                   ".shstrtab")):
            continue
        if section["type"] == SHT_RELA:
            continue
        if name.startswith((".init_array", ".fini_array", ".ctors", ".dtors", ".eh_frame",
                            ".gcc_except", ".tbss", ".tdata")):
            raise Packed("%s present: a module is plain code and data, with no runtime tables or "
                         "thread-local storage" % name)
        if name.startswith((".text", ".rodata")):
            kind = RXE
        elif name.startswith(".data"):
            kind = DATA
        elif section["type"] == SHT_NOBITS:
            kind = BSS
        elif section["flags"] & 0x2:
            raise Packed("%s is an ALLOC section the packer does not lay out" % name)
        else:
            continue  # .comment and friends: no bytes in the image
        if kind == BSS:
            while bss_total % 16:
                bss_total += 1
            placed[section["index"]] = (kind, bss_total)
            bss_total += section["size"]
            continue
        block = blocks[kind]
        while len(block) % 16:
            block.append(0)
        placed[section["index"]] = (kind, len(block))
        block += section["data"]
        if len(section["data"]) != section["size"]:
            raise Packed("%s: section data is truncated inside the object file" % name)

    rxe, data = blocks[RXE], blocks[DATA]
    if len(rxe) == 0:
        raise Packed("the read/execute block is empty")
    # Pad the read/execute block so the data block starts 16-byte aligned both in the file and in
    # memory; then the whole loaded region is one contiguous copy.
    while len(rxe) % 16:
        rxe.append(0)
    rxe_size, data_size = len(rxe), len(data)
    image_size = rxe_size + data_size + bss_total
    if image_size > MAX_IMAGE:
        raise Packed("image is %u bytes, over the %u-byte module region" % (image_size, MAX_IMAGE))

    def resolve(symbol_index):
        (name, shndx, value, size, st_kind) = symbols[symbol_index]
        if shndx == 0:
            raise Packed("%s is undefined; a module may not import anything, so link it in or "
                         "drop the call" % name)
        if shndx not in placed:
            raise Packed("%s refers to section %s, which the packer does not lay out"
                         % (name, sections[shndx]["sname"] if shndx < len(sections) else "#%u" % shndx))
        kind, base = placed[shndx]
        offset = base + value
        if kind == DATA:
            offset += rxe_size
        elif kind == BSS:
            offset += rxe_size + data_size
        # Every offset the header and the relocation table carry is measured from the start of the
        # module file, which is also where the kernel places the image, so the loader can add the
        # field to its load address with no further adjustment.  .bss has no bytes in the file, but
        # its address still has to be expressible, hence the same origin.
        return offset + HEADER_SIZE

    def find(name):
        for index, entry in enumerate(symbols):
            if entry[0] == name and entry[1] in placed:
                return index
        return None

    entries = {}
    for what in ("scos_module_init", "scos_module_teardown"):
        index = find(what)
        if index is None:
            raise Packed("%s is missing; a module must define it" % what)
        offset = resolve(index)
        if offset >= rxe_size:
            raise Packed("%s must live in the read/execute block" % what)
        entries[what] = offset

    ids_index = find("scos_module_ids")
    if ids_index is None:
        raise Packed("scos_module_ids is missing: a module that claims no device id is dead "
                     "weight, so refuse it")
    ids_size = symbols[ids_index][3]
    if ids_size == 0 or ids_size % ID_RULE_SIZE:
        raise Packed("scos_module_ids must be an array of %u-byte rules (got %u bytes)"
                     % (ID_RULE_SIZE, ids_size))
    ids_off = resolve(ids_index)
    if ids_off + ids_size > HEADER_SIZE + rxe_size:
        raise Packed("the id table must live inside the read/execute block")

    relocs = []
    for section in sections:
        if section["type"] != SHT_RELA or not section["size"]:
            continue
        target = section["info"]
        if target not in placed:
            continue  # relocations for a section we are not laying out at all
        kind, base = placed[target]
        for slot in range(section["size"] // section["entsize"]):
            (r_offset, r_info, r_addend) = struct.unpack_from("<QQq", section["data"],
                                                               slot * 24)
            r_type = r_info & 0xFFFFFFFF
            symbol = r_info >> 32
            if r_type == R_X86_64_NONE:
                continue
            if r_type in GOTPCREL:
                # Fold it, but only if the target is defined in this object; the resolve() call
                # below refuses otherwise.
                r_type = R_X86_64_PC32
            if r_type not in RELOC_TYPE:
                raise Packed("%s+0x%x: relocation type %u is not one of abs64/abs32/pc32/plt32"
                             % (section["sname"], r_offset, r_type))
            if symbol == 0:
                raise Packed("%s+0x%x: relocation against the null symbol" % (section["sname"],
                                                                               r_offset))
            name = symbols[symbol][0]
            if symbols[symbol][4] == 3 and symbols[symbol][1] not in placed:  # STT_SECTION elsewhere
                raise Packed("%s+0x%x: %s is in an unmapped section" % (section["sname"],
                                                                         r_offset, name))
            place = HEADER_SIZE + base + r_offset
            if kind == DATA:
                place += rxe_size
            if place >= HEADER_SIZE + rxe_size + data_size:
                raise Packed("%s+0x%x: relocation placed outside the loaded image"
                             % (section["sname"], r_offset))
            relocs.append((place, RELOC_TYPE[r_type], resolve(symbol), r_addend))
    relocs.sort()
    for previous, current in zip(relocs, relocs[1:]):
        if previous[0] == current[0]:
            raise Packed("two relocations land on image offset 0x%x" % current[0])

    # .bss has no contents, but it still occupies image bytes, so the metadata that follows the
    # image starts after it.  Placing the relocation table at the end of the *loaded* blocks instead
    # put it on top of .bss, and the module's own writes to its zero-initialised globals landed in the
    # middle of the table the loader is reading - which is how a correctly packed module came in looking
    # corrupt.  `load | bss | relocs' keeps stored bytes and image bytes disjoint.
    payload = (bytes(rxe) + bytes(data) + bytes(bss_total) +
               b"".join(struct.pack("<IIIi", *r) for r in relocs))
    stored = HEADER_SIZE + image_size + len(relocs) * RELOC_SIZE
    payload_crc = binascii.crc32(payload) & 0xFFFFFFFF
    fields = [MAGIC, ABI_VERSION, HEADER_SIZE, 0,
              HEADER_SIZE, rxe_size, HEADER_SIZE + rxe_size, data_size, bss_total,
              HEADER_SIZE + image_size, len(relocs), ids_off,
              ids_size // ID_RULE_SIZE,
              entries["scos_module_init"], entries["scos_module_teardown"],
              16, rxe_size + data_size, image_size, payload_crc,
              stored if not args.min_bytes else args.min_bytes, args.flags]
    head = struct.pack("<4I16s17I", *fields[:4], args.family.encode()[:FAMILY_NAME - 1], *fields[4:])
    head = head + b"\0" * (HEADER_SIZE - len(head))
    assert len(head) == HEADER_SIZE, len(head)
    words = list(struct.unpack("<%dI" % (HEADER_SIZE // 4), head))
    words[3] = header_checksum(words)
    head = struct.pack("<%dI" % (HEADER_SIZE // 4), *words)
    with open(args.out, "wb") as handle:
        handle.write(head + payload)
    return dict(family=args.family, rxe=rxe_size, data=data_size, bss=bss_total,
                relocs=len(relocs), ids=ids_size // ID_RULE_SIZE, crc=payload_crc,
                size=len(words) + len(payload), image=image_size)


def parse(blob):
    if len(blob) < HEADER_SIZE:
        raise Packed("file is smaller than the module header")
    fixed = struct.unpack_from("<4I", blob, 0)
    family = blob[16:32].rstrip(b"\0").decode("latin1")
    rest = struct.unpack_from("<17I", blob, 32)
    (rxe_off, rxe_size, data_off, data_size, bss_size, reloc_off, reloc_count, id_off, id_count,
     entry_init, entry_teardown, align, load_size, image_size, payload_crc, min_bytes,
     flags) = rest
    module = dict(magic=fixed[0], abi=fixed[1], header_size=fixed[2], checksum=fixed[3],
                  family=family, rxe_off=rxe_off, rxe_size=rxe_size, data_off=data_off,
                  data_size=data_size, bss_size=bss_size, reloc_off=reloc_off,
                  reloc_count=reloc_count, id_off=id_off, id_count=id_count,
                  entry_init=entry_init, entry_teardown=entry_teardown, align=align,
                  load_size=load_size, image_size=image_size, payload_crc=payload_crc,
                  min_bytes=min_bytes, flags=flags,
                  relocs=[struct.unpack_from("<IIIi", blob, reloc_off + RELOC_SIZE * n)
                          for n in range(reloc_count)],
                  ids=[struct.unpack_from("<IIIII", blob, id_off + ID_RULE_SIZE * n)
                       for n in range(id_count)])
    return module


def verify(path, table_path):
    blob = open(path, "rb").read()
    module = parse(blob)
    problems = []
    if module["magic"] != MAGIC:
        problems.append("magic is 0x%08x, not SGPM" % module["magic"])
    if module["abi"] != ABI_VERSION:
        problems.append("abi %u does not match the kernel's %u" % (module["abi"], ABI_VERSION))
    if module["header_size"] != HEADER_SIZE:
        problems.append("header size %u != %u" % (module["header_size"], HEADER_SIZE))
    words = struct.unpack("<%dI" % (HEADER_SIZE // 4), blob[:HEADER_SIZE])
    if header_checksum(words) != module["checksum"]:
        problems.append("header checksum is stale")
    payload = blob[HEADER_SIZE:]
    if binascii.crc32(payload) & 0xFFFFFFFF != module["payload_crc"]:
        problems.append("payload crc mismatch (the file is corrupt)")
    if len(blob) != HEADER_SIZE + module["image_size"] + module["reloc_count"] * RELOC_SIZE:
        problems.append("file size %u is not header + loaded image + relocation table"
                        % len(blob))
    if module["load_size"] != module["rxe_size"] + module["data_size"]:
        problems.append("load size does not match the two blocks")
    if module["data_off"] != module["rxe_off"] + module["rxe_size"]:
        problems.append("data block is not contiguous with the read/execute block")
    if module["reloc_off"] < HEADER_SIZE + module["image_size"]:
        problems.append("the relocation table overlaps the image (it must follow .bss)")
        problems.append("data block is not contiguous with the read/execute block")
    if module["image_size"] > MAX_IMAGE:
        problems.append("image size %u exceeds the module region" % module["image_size"])
    if module["image_size"] < module["load_size"]:
        problems.append("image size smaller than the loaded blocks")
    if not module["id_count"]:
        problems.append("no id rules claimed")
    if module["flags"] & 1:
        problems.append("SCOS_GPU_MODULE_TAKES_DISPLAY is not accepted by the kernel")
    for (place, rtype, target, _addend) in module["relocs"]:
        if rtype not in set(RELOC_TYPE.values()):
            problems.append("reloc type %u is not supported" % rtype)
        if place < HEADER_SIZE:
            problems.append("reloc at 0x%x would rewrite the module header" % place)
        if place + (8 if rtype == 1 else 4) > HEADER_SIZE + module["load_size"]:
            problems.append("reloc at 0x%x writes outside the loaded image" % place)
        if target < HEADER_SIZE or target >= HEADER_SIZE + module["image_size"]:
            problems.append("reloc target 0x%x is outside the image" % target)
    if module["entry_init"] < HEADER_SIZE or module["entry_init"] - HEADER_SIZE >= module["rxe_size"]:
        problems.append("entry_init is outside the read/execute block")
    if table_path:
        known = family_ids(table_path)
        claimed = known.get(module["family"])
        if claimed is None:
            problems.append("family %r has no generated id table in %s"
                            % (module["family"], table_path))
        else:
            for (vendor, device, _flags, _sv, _sd) in module["ids"]:
                if (vendor, device) not in claimed:
                    problems.append("module claims %04x:%04x, which the %s table does not list"
                                    % (vendor, device, module["family"]))
    return module, problems


def family_ids(table_path):
    """family -> set of (vendor, device), read from the generated table's own arrays.  The kernel
    cross-checks the same thing at load time against the live table; this is the build-time mirror of
    that predicate, so a module that would be refused at boot is refused by `make` instead."""
    rules, current = {}, None
    for line in open(table_path, encoding="utf-8"):
        stripped = line.strip()
        if stripped.startswith("static const struct gpu_pci_id gpu_ids_") and "[] = {" in stripped:
            current = stripped.split("gpu_ids_",1)[1].split("[]")[0]
            rules.setdefault(current, set())
            continue
        if current is None:
            continue
        if stripped.startswith("};"):
            current = None
            continue
        if stripped.startswith("{0x"):
            fields = stripped[1:stripped.index("}")].split(",")
            def number(text):
                text = text.strip()
                return int(text, 16) if text.startswith("0x") else int(text, 0)
            rules[current].add((number(fields[0]), number(fields[1])))
    return rules


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input")
    parser.add_argument("--out")
    parser.add_argument("--family")
    parser.add_argument("--flags", type=lambda text: int(text, 0), default=0)
    parser.add_argument("--min-bytes", type=lambda text: int(text, 0), default=0)
    parser.add_argument("--report", action="store_true")
    parser.add_argument("--verify", nargs="?", const="", default=None)
    parser.add_argument("--table", default="kernel/drivers/gpu/gpu_ids.h")
    args = parser.parse_args()
    if args.verify is not None:
        targets = [args.verify] if args.verify else sorted(
            os.path.join(root, name) for root, _d, files in os.walk("build/gpu")
            for name in files if name.endswith(".mod"))
        if not targets:
            print("no modules to verify; run make first", file=sys.stderr)
            return 2
        bad = 0
        for target in targets:
            module, problems = verify(target, args.table)
            print("%s %s: family=%s rxe=%u data=%u bss=%u relocs=%u ids=%u load=%u image=%u"
                  % ("OK  " if not problems else "FAIL", os.path.basename(target),
                     module["family"], module["rxe_size"], module["data_size"], module["bss_size"],
                     module["reloc_count"], module["id_count"], module["load_size"],
                     module["image_size"]))
            for problem in problems:
                print("       %s" % problem, file=sys.stderr)
                bad += 1
        if bad:
            print("gpu module verification failed (%d problem(s))" % bad, file=sys.stderr)
            return 1
        print("all %d module(s) verify against %s" % (len(targets), args.table))
        return 0
    for required in ("input", "out", "family"):
        if not getattr(args, required):
            print("--%s is required unless --verify is used" % required, file=sys.stderr)
            return 2
    stats = pack(args)
    if args.report:
        print("packed %(family)s module: rxe=%(rxe)u data=%(data)u bss=%(bss)u relocs=%(relocs)u "
              "ids=%(ids)u file=%(size)u image=%(image)u crc=%(crc)08x" % stats)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Packed as error:
        print("gpu module build refused: %s" % error, file=sys.stderr)
        sys.exit(1)

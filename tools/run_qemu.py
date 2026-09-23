#!/usr/bin/env python3
"""Run SCos in QEMU/TCG with a disposable disk snapshot and no guest networking."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--image", type=Path, default=ROOT / "build/scos.img",
                    help="current build by default; pass the retained milestone explicitly")
    ap.add_argument("--qemu-dir", type=Path, default=ROOT / ".tools/qemu")
    ap.add_argument("--memory", type=int, default=256, help="guest RAM in MiB")
    ap.add_argument("--usb-boot", action="store_true",
                    help="boot via emulated USB storage, with xHCI keyboard/mouse; native saves remain unsupported on USB")
    ap.add_argument("--xhci", action="store_true", help="add emulated USB keyboard/mouse")
    ap.add_argument("--vnc", action="store_true", help="VNC on a private Unix socket, never a TCP listener")
    ap.add_argument("--device", action="append", default=[], metavar="SPEC",
                    help="extra -device argument, repeatable; used by the GPU detection "
                         "test to attach emulated PCI display functions next to the boot one")
    ap.add_argument("--vga", choices=("std", "cirrus", "none"), default="std",
                    help="display device presenting the firmware console; cirrus is the one case "
                         "where the console and an emulated 2D engine are the same PCI function")
    ap.add_argument("--dry-run", action="store_true", help="print the command without starting QEMU")
    args = ap.parse_args()
    image = args.image.resolve()
    qemu = args.qemu_dir.resolve()
    if not image.is_file():
        ap.error(f"image does not exist: {image}; run make or supply --image")
    # Only regular image files, never /dev disks or passed-through devices.
    if not (qemu / "lib/ld-musl-x86_64.so.1").is_file():
        ap.error("QEMU is not prepared; run python3 tools/setup_qemu.py")
    if not 128 <= args.memory <= 8192:
        ap.error("--memory must be 128..8192 MiB for this UEFI environment")
    runs = ROOT / "build/qemu"
    runs.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="run-", dir=runs))
    firmware = qemu / "share/qemu/edk2-x86_64-code.fd"
    variables = qemu / "share/qemu/edk2-i386-vars.fd"
    if not firmware.is_file() or not variables.is_file():
        ap.error("x64 EDK2 firmware/variable template missing from QEMU bundle")
    # This package names the architecture-neutral variable template i386-vars;
    # only the x86_64 CODE image executes. Never modify shared firmware state.
    shutil.copyfile(variables, run / "vars.fd")
    escape = lambda p: str(p).replace(",", ",,")
    disk = (f"file={escape(image)},format=raw,if=none,id=bootdisk,snapshot=on"
            if args.usb_boot else f"file={escape(image)},format=raw,if=ide,snapshot=on")
    command = [str(qemu / "lib/ld-musl-x86_64.so.1"), "--library-path", str(qemu / "lib"),
               str(qemu / "bin/qemu-system-x86_64"), "-L", str(qemu / "share/qemu"),
               "-machine", "q35",
               "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={escape(firmware)}",
               "-drive", f"if=pflash,format=raw,unit=1,file={escape(run / 'vars.fd')}",
               "-accel", "tcg", "-cpu", "max", "-m", str(args.memory), "-smp", "1",
               "-drive", disk,
               "-vga", args.vga, "-nic", "none", "-display", "none",
               "-serial", f"file:{run / 'serial.log'}",
               "-qmp", f"unix:{escape(run / 'qmp.sock')},server=on,wait=off", "-no-reboot"]
    if args.xhci or args.usb_boot:
        command += ["-device", "qemu-xhci,id=xhci", "-device", "usb-kbd,bus=xhci.0",
                    "-device", "usb-mouse,bus=xhci.0"]
    if args.usb_boot:
        command += ["-device", "usb-storage,drive=bootdisk,bus=xhci.0,bootindex=1"]
    # Only device specifications, never whole arguments: the caller cannot inject a
    # host path, a snapshot-off flag or a passthrough this way.
    for spec in args.device:
        if not spec or spec.startswith("-") or any(c.isspace() for c in spec):
            ap.error(f"--device must be a single device spec with no whitespace: {spec!r}")
        # A device spec's own commas are QEMU's property separators (`pci-bridge,chassis_nr=1' is one
        # device), so they pass through as written.  Escaping them the way a *path* needs would leave no
        # way to describe a PCI bridge - and a bridge is how a GPU ends up on a bus of its own, which is
        # the topology this test suite must be able to boot.  Injection stays closed because of the two
        # checks above: no whitespace, no leading dash, so no new argument and no host path.
        command += ["-device", spec]
    if args.vnc:
        command += ["-vnc", f"unix:{run / 'vnc.sock'}"]
    print(f"QMP / serial log directory: {run}", flush=True)
    print("Disposable snapshot: original image unchanged; no guest networking or device passthrough.", flush=True)
    print(shlex.join(command), flush=True)
    if not args.dry_run:
        os.execv(command[0], command)


if __name__ == "__main__":
    main()

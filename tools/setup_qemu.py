#!/usr/bin/env python3
"""Prepare a pinned, third-party QEMU host tool without sudo or guest imports.

The normal distribution-package route is preferred when available. This fallback
uses qemu-portable's musl build and locally builds its musl loader. Both downloads
are pinned; no npm lifecycle scripts, auto-updaters or system installs are run.
See docs/migration/EMULATOR.md for provenance and limitations.
"""
import argparse
import hashlib
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
QEMU_URL = "https://registry.npmjs.org/qemu-portable-linux-x64-musl/-/qemu-portable-linux-x64-musl-0.2.1.tgz"
QEMU_SHA = "5933a148c77ec61e47474c8e6d53541295d78a5126805f381b57d8489cb97f81"
MUSL_URL = "https://codeload.github.com/ifduyue/musl/tar.gz/refs/tags/v1.2.5"
MUSL_SHA = "83ff394502d1c334b040ea9bc66ec48bba453585e25b05f4bde3741d8245d883"


def unpack(url, expected, destination):
    archive = destination.with_suffix(".tgz")
    subprocess.run(["curl", "--fail", "--location", "--max-time", "180",
                    "--output", str(archive), url], check=True)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
        raise ValueError(f"Checksum mismatch; refusing to extract {url}")
    destination.mkdir()
    with tarfile.open(archive) as tar:
        # These pinned archives contain regular files/directories. Refuse links,
        # devices and path traversal rather than following downloaded filenames.
        members = tar.getmembers()
        for m in members:
            path = Path(m.name)
            if path.is_absolute() or ".." in path.parts or not (m.isfile() or m.isdir()):
                raise ValueError(f"Unsafe archive member: {m.name}")
        tar.extractall(destination, members=members)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--destination", type=Path, default=ROOT / ".tools/qemu",
                    help="new directory; existing installations are never replaced")
    args = ap.parse_args()
    if platform.system() != "Linux" or platform.machine() not in ("x86_64", "amd64"):
        ap.error("this bootstrap is for Linux x86-64 hosts only")
    dest = args.destination.resolve()
    if dest.exists():
        ap.error(f"{dest} already exists; use it or choose a new destination")
    for tool in ("curl", "gcc", "make"):
        if not shutil.which(tool):
            ap.error(f"missing host dependency: {tool}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="scos-qemu-") as td:
        temp = Path(td)
        unpack(QEMU_URL, QEMU_SHA, temp / "qemu")
        unpack(MUSL_URL, MUSL_SHA, temp / "musl")
        musl = temp / "musl/musl-1.2.5"
        subprocess.run(["./configure", "--disable-static", "--prefix=/opt/scos-qemu-musl"],
                       cwd=musl, check=True)
        subprocess.run(["make", "-j", str(min(os.cpu_count() or 1, 4))], cwd=musl, check=True)
        package = temp / "qemu/package"
        stage = temp / "runtime"
        (stage / "bin").mkdir(parents=True)
        for binary in ("qemu-system-x86_64", "qemu-img"):
            shutil.copy2(package / "bin" / binary, stage / "bin" / binary)
        shutil.copytree(package / "lib", stage / "lib")
        shutil.copy2(musl / "lib/libc.so", stage / "lib/ld-musl-x86_64.so.1")
        shutil.copytree(package / "licenses", stage / "licenses")
        shutil.copy2(musl / "COPYRIGHT", stage / "licenses/MUSL-COPYRIGHT")
        shutil.copy2(package / "build-info.json", stage / "build-info.json")
        firmware = stage / "share/qemu"
        firmware.mkdir(parents=True)
        # Keep the small firmware/ROM files, including x86 BIOS and OVMF. Omit
        # the two 64-MiB ARM firmware files and the unused AArch64 emulator.
        for p in (package / "share/qemu").iterdir():
            if p.is_file() and p.stat().st_size < 8 * 1024 * 1024:
                shutil.copy2(p, firmware / p.name)
        shutil.copytree(package / "share/qemu/keymaps", firmware / "keymaps")
        subprocess.run([str(stage / "lib/ld-musl-x86_64.so.1"), "--library-path",
                        str(stage / "lib"), str(stage / "bin/qemu-system-x86_64"),
                        "--version"], check=True)
        shutil.copytree(stage, dest)
    print(f"Prepared QEMU 11.0.2 at {dest}; no OS image or kernel code was changed.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        raise SystemExit(f"QEMU setup failed: {exc}")

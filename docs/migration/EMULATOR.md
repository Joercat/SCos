# QEMU host test environment — obtained and exercised

QEMU **11.0.2**, `qemu-system-x86_64`, is available locally under `.tools/qemu`.
It supports x86-64 system emulation and also boots the existing i386 image.
TCG does not require KVM. This is a native host emulator, not a browser v86
replacement with the same UI. No v86 runtime or diagnostic guest UI was restored.

The current r43 safety corrections and further checks are documented in
[RELEASE-r43.md](../RELEASE-r43.md); r42 in Git history still has its old
defects. Temporary r43 test harnesses were retired after verification.

## Use

The prepared local runtime is approximately 57 MiB and is ignored by Git. On a
fresh Linux x86-64 checkout, the pinned bootstrap needs Python 3, curl, GCC and
Make; it does not need root, npm installation scripts, or a system libc upgrade:

```sh
python3 tools/setup_qemu.py          # new .tools/qemu directory only
make                               # current build/scos.img
python3 tools/run_qemu.py --dry-run
python3 tools/run_qemu.py            # CURRENT build, software TCG
python3 tools/run_qemu.py --xhci     # emulated USB keyboard and mouse
```

**Historical r42 images have a storage safety defect**, corrected in the
current r43 image: see [the audit](../AUDIT-32BIT.md). The launcher always uses a disposable snapshot,
so guest writes do not change the retained image. It never attaches a physical
disk, host USB device, network interface or shared host filesystem.

The launcher prints a private `build/qemu/run-...` directory containing the serial
log and QMP Unix socket. Use QMP for screenshots (`screendump`), input, status,
CPU-model inspection and quitting. `--vnc` optionally creates a Unix-domain VNC
socket, **not an exposed TCP service or browser preview**. This bundle has no
GTK/SDL window backend. On a graphical workstation, a trusted distribution QEMU
package with its graphical frontend may be more convenient.

A QMP client reads the greeting, sends `{"execute":"qmp_capabilities"}`, then
requests commands such as `{"execute":"query-status"}`. `screendump` takes a
host filename argument and writes a PPM screenshot. Do not confuse host QMP
`quit` with evidence of the guest's power-off path.

The default is now native x64 UEFI on q35. The launcher uses bundled
`edk2-x86_64-code.fd` read-only and copies the architecture-neutral variable
store template `edk2-i386-vars.fd` into each disposable run. The template's
historical filename does not select i386 execution. No BIOS/CSM fallback exists
in the current launcher or guest. To reproduce old 32-bit tests, consult the
historical tooling, not this command. Use at least 128 MiB guest RAM.

The image is unsigned: use non-Secure-Boot firmware. `--xhci` supplies emulated
hardware but does not enable a kernel USB input driver. See
[actual UEFI verification and failures](BOOT64.md). Firmware is a host testing
dependency from the pinned QEMU bundle, not shipped inside SCos. The bundle's
`edk2-licenses.txt` contains its firmware notices; its firmware build has not
been independently reproduced here.

## Acquisition/provenance

The normal Debian package installation failed here over both HTTP and HTTPS.
Upstream QEMU's GitHub repository did not offer a ready host release asset.
A third-party AppImage download also failed at the release-asset CDN. None of
those failures is represented as a successful installation.

The working fallback is a **third-party binary package**, not an official QEMU
Project binary and not a QEMU source build independently reproduced by SCos:

* Producer: [Giulio2002/qemu-portable-ts](https://github.com/Giulio2002/qemu-portable-ts).
* Package: `qemu-portable-linux-x64-musl@0.2.1` from the npm registry.
* Exact tarball:
  `https://registry.npmjs.org/qemu-portable-linux-x64-musl/-/qemu-portable-linux-x64-musl-0.2.1.tgz`
* Tarball SHA-256:
  `5933a148c77ec61e47474c8e6d53541295d78a5126805f381b57d8489cb97f81`.
* npm integrity, also checked during acquisition:
  `sha512-TVIqaJZ4z49ULdUkKE+jVLWgsRLJdjzpiDnTV5dF3Y964HbDSLhtlgcv+HU3qc6S1zhkYguGFc0oqgu4YfJRWw==`.
* Producer `build-info.json`: QEMU `v11.0.2`, built 2026-07-26;
  `x86_64-softmmu,aarch64-softmmu`, GTK/SDL disabled, slirp and nettle enabled.
  Producer-declared QEMU source archive SHA-256:
  `3745f6ea88e2e87fe0dc838b2b1d4e0a770bf48e01a1d5a186842a1fff76ccf5`.
  This source hash is recorded as provenance, not as an independently audited
  source-to-binary correspondence. Registry attestations were not independently
  verified. Pinned transport hashes do not certify freedom from malicious code
  or known vulnerabilities.

The glibc package was downloaded and integrity-checked but **could not run** on
this host because it requires GLIBC_2.38. Rather than replace the host libc, the
musl variant is launched through a local loader built from pinned musl source:

* Source mirror: `https://github.com/ifduyue/musl`, tag `v1.2.5`.
* Archive: `https://codeload.github.com/ifduyue/musl/tar.gz/refs/tags/v1.2.5`.
* Archive SHA-256:
  `83ff394502d1c334b040ea9bc66ec48bba453585e25b05f4bde3741d8245d883`.
* Bootstrap config: `--disable-static --prefix=/opt/scos-qemu-musl`;
  only the resulting shared loader is copied. No `make install` or system
  library replacement is performed. Its copyright notice is retained.

The bootstrap verifies pinned hashes **before extraction**, rejects archive
links/special files/path traversal, compiles musl, copies x86 emulator/utilities,
bundled libraries, small firmware files and supplied licenses, then runs the
emulator's version check. It omits the unused AArch64 emulator and two 64 MiB ARM
firmware files. Existing destinations are never overwritten. No auto-update is
configured. Large archives, scratch builds and binaries are not committed.

Prefer a trusted, maintained distribution package where available. Reassess
producer trust, upstream security advisories and license/source obligations
before upgrading or redistributing the runtime. This host-only installation
does not authorize importing any of these components into the SCos guest.

## Evidence and limitations

The fresh bootstrap completed successfully. Both PS/2 and emulated xHCI boot,
console/input, WM lifecycle and guest-triggered ACPI shutdown were exercised;
see [the full audit](../AUDIT-32BIT.md). Physical USB/firmware behavior remains a
separate test. No claim is made that a 64-bit SCos kernel exists simply because
the selected emulator supports long mode.

## Firmware used for the native UEFI checks

Bundled EDK2 files (actual local SHA256, not an independently reproduced build):

```text
33090cc07675baa5190d9f1e84bf5176b33bcbfa9bacac522961150cdb6dbb2a  edk2-x86_64-code.fd
5d2ac383371b408398accee7ec27c8c09ea5b74a0de0ceea6513388b15be5d1e  edk2-i386-vars.fd
```

The first is executable x64 firmware; the second is only a variable-store seed.
Each run uses a private writable copy of the seed. Neither belongs in the guest
image. Native UEFI checks and the constrained 64-MiB failure are in BOOT64.md;
older ACPI/i386 checks elsewhere in this document are historical results only.

# Reused GPU drivers and selective loading

Research date: 2026-09-20. **Research only: no driver has been ported, built for
SCos, or verified running on SCos in this work.** The shipped renderer is still CPU.

> **2026-09-20 scope narrowing.** The target moved to the *basic* tier: native display
> ownership plus GPU copy/fill, with no GL/Vulkan. That tier is researched and measured
> separately in [GPU-BASIC-2D-RESEARCH.md](GPU-BASIC-2D-RESEARCH.md), which revises two
> statements below: the light per-family engines carry no firmware, so they do not wait
> on the 16 MiB boot reader or disk streaming; and Genode's "Intel framebuffer" is not a
> small standalone driver but a build of 932 Linux i915/DRM source files under GPL-2.0.

## Corrected scope

The requested approach is multiple existing drivers, each matched to its actual
supported devices, not a universal driver and not restricting the OS to one PC.
Reuse upstream hardware implementations; write only necessary SCos integration,
loading and compositor glue. Browser work remains deferred. Storage work needed
for selective driver loading is now in scope for investigation.

This research does not authorize calling an upstream driver SCos-compatible just
because it supports the right PCI vendor. No investigated binary is a drop-in
module for the current custom kernel.

## Findings from pinned source, not just project descriptions

Exact commits, file hashes, tree sizes and submodule pins are recorded in
[GPU-SOURCE-AUDIT.json](GPU-SOURCE-AUDIT.json). Reproduce with:

```
python3 tools/research/audit_gpu_sources.py > /tmp/gpu-source-audit.json
cmp docs/migration/GPU-SOURCE-AUDIT.json /tmp/gpu-source-audit.json
```

Requires authenticated `gh`. The script uses read-only API requests, refuses
truncated trees and downloads selected source files, not firmware binaries.
Measurements are source-tree blob sizes, NOT module sizes, build dependency
closure, compiled code, or runtime RAM. Submodule contents are explicitly excluded.

### Intel: Genode's intel_gpu is the strongest smaller candidate

Pinned Genode: `0f275e7afaab44e9a45cecd0eac0b88691278477`.

- The complete `repos/os/src/driver/gpu/intel/` directory has **18 files,
  255,391 bytes, 9,505 lines** including comments/build description. This does not
  include Genode base/platform services, Mesa, or the separate framebuffer driver.
- `main.cc` actually handles contexts, GPU page tables, command submission and
  completion, rather than merely supplying a framebuffer.
- Its supplied Sculpt configuration lists exactly these Intel device IDs:
  `1606, 1616, 1622, 5a85, 1916, 191b, 5916, 5917, 591b, 3ea0, 9a49, 46a6`.
  The platform switch accepts Broadwell, Skylake, Kaby Lake, Whiskey Lake,
  Tiger Lake and Alder Lake. That is an upstream configuration list, not a SCos
  verified-device table, and not blanket support for those generations.
- Rocket Lake is not a platform name in the inspected switch or supplied list.
  Do not assume the user's i5-11400 integrated graphics works merely because
  another Gen12 part appears. Discrete Intel coverage is not established here.
- The Mesa package explicitly uses Iris. Its README offers disabling its
  buffer-object cache to trade performance for memory. This does not remove Mesa.
- The separate PC framebuffer recipe imports Linux-derived code and lx_emul;
  the small render-driver directory is not the entire display stack.
- Genode source carries AGPLv3 with an explicit linking exception for qualifying
  independent modules under approved licenses. The SCos repository has no root
  LICENSE file at audit time. Resolve distribution/license compatibility before
  importing this driver; do not describe it as MIT or ignore the exception's terms.

**Decision:** prioritize a dependency/licensing assessment of this existing driver
for its listed devices. Preserve i915/Xe plus matching Mesa as the larger route
for Intel families the smaller implementation does not cover. No invented PCI-ID
extension and no custom replacement hardware driver.

Sources: [driver](https://github.com/genodelabs/genode/tree/0f275e7afaab44e9a45cecd0eac0b88691278477/repos/os/src/driver/gpu/intel),
[device configuration](https://github.com/genodelabs/genode/blob/0f275e7afaab44e9a45cecd0eac0b88691278477/repos/gems/sculpt/gpu/intel),
[license](https://github.com/genodelabs/genode/blob/0f275e7afaab44e9a45cecd0eac0b88691278477/LICENSE).

### AMD: RadeonGfx is smaller but narrowly accelerated

Pinned X547/RadeonGfx: `797e94775d274f243d8e0fe18b434b672bdeb768`.

- Tree: **181 blobs, 7,248,693 bytes**, including firmware and large register
  headers. This excludes external build dependencies.
- `Units/InstantiateUnits.cpp` installs DMA V1 and GFX V6 units for **Cape Verde
  and Tahiti only**, returning B_NOT_SUPPORTED for other chipsets.
- Bundled Polaris firmware and larger detection tables are NOT proof of Polaris
  acceleration. They must not populate a claimed-working SCos table.
- Meson requires Haiku libraries/services, private headers, libdrm headers,
  synchronization, Locks, SADomains, ThreadLink and VideoStreams. These are real
  porting work, not resolved by translating a handful of register accesses.
- No top-level LICENSE/COPYING file was found in the pinned tree. Reviewed core
  files did not establish a complete project-wide license grant. Individual
  borrowed files and firmware need their own provenance/license audit. Public
  availability alone is not permission to redistribute the whole project.

**Decision:** retain as a narrow acceleration candidate, gated on licensing and
its dependency closure. For other AMD generations assess upstream radeon/amdgpu
with the appropriate Mesa driver; no lightweight broad-AMD replacement was
established in this audit.

Sources: [actual unit selection](https://github.com/X547/RadeonGfx/blob/797e94775d274f243d8e0fe18b434b672bdeb768/Units/InstantiateUnits.cpp),
[build dependencies](https://github.com/X547/RadeonGfx/blob/797e94775d274f243d8e0fe18b434b672bdeb768/meson.build).

### NVIDIA: Nebula is an existing adaptation, not a tiny complete stack

Pinned X547/nvidia-haiku: `cc1849cb2c5c327af4f7eccf993b8616855ad7c5`.

- Tree: **76 blobs, 91,960,458 bytes**, EXCLUDING submodule contents.
- Of that, two firmware binaries account for **91,675,464 bytes**:
  `570.86.16/gsp_tu10x.bin` = 28,345,432 bytes (~27.03 MiB),
  `570.86.16/gsp_ga10x.bin` = 63,330,032 bytes (~60.40 MiB).
- The repository provides Haiku OS callbacks, an accelerant and SDK wrappers.
  It also depends on forked NVIDIA resource-manager code, forked Mesa NVK/Zink,
  and VideoStreamsWsi. All four submodule commits are in the audit JSON.
- The port uses a 570.86.16-based resource-manager branch and matching firmware.
  Do NOT combine it with the independently inspected current NVIDIA 615.71.09
  code. Upstream explicitly requires matching firmware/userspace versions.
- The wrapper repository declares MIT unless otherwise noted; that does not
  replace submodule or firmware license requirements.
- Upstream NVIDIA open modules target Turing and later. Older NVIDIA generations
  require a different upstream route such as Nouveau; Nebula does not establish
  their support. Current Mesa NVK documentation also has its own kernel-interface
  requirements and evolving generation coverage, distinct from the pinned port.

**Decision:** promising reuse of an existing OS-adaptation layer for an initial
NVIDIA branch, but not lightweight in total. Review its exact supported devices,
full dependency closure and firmware licensing before an SCos port. Keep older
NVIDIA support as a separate Nouveau workstream, not an invented extension.

Sources: [pinned port](https://github.com/X547/nvidia-haiku/tree/cc1849cb2c5c327af4f7eccf993b8616855ad7c5),
[build](https://github.com/X547/nvidia-haiku/blob/cc1849cb2c5c327af4f7eccf993b8616855ad7c5/Build.sh),
[NVIDIA requirements](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/61dcc93722ecb418bb5f2e00923f05b4b8051dd1/README.md),
[NVK documentation](https://docs.mesa3d.org/drivers/nvk.html).

### Broader alternatives and misleading shortcuts

FreeBSD drm-kmod at `f252a30f27d157d9c763cd408850775096a6263f` explicitly ports
Linux DRM using linuxkpi and requires corresponding FreeBSD kernel sources.
Its tree contains 3,660 blobs / 529,123,437 bytes, including generated/data files.
This is evidence of a substantial ecosystem, NOT a compiled-size measurement.
It is an existing porting reference, not a small standalone BSD GPU library.
[Source](https://github.com/freebsd/drm-kmod/blob/f252a30f27d157d9c763cd408850775096a6263f/README.md).

Haiku's radeon_hd hardware table distinguishes modesetting from 2D/3D
acceleration; its listed mode support must not be sold as accelerated rendering.
[2](https://dev.haiku-os.org/wiki/HardwareInfo/video/ATI).

Virtio-gpu is a useful separate virtual-device target, not a physical-vendor
driver. QEMU distinguishes its software-rendered 2D backend from accelerated
virglrenderer/rutabaga backends. A passing 2D QEMU screenshot is not a native
GPU test or even proof of accelerated virtual rendering.
[Documentation](https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html).

## Selective loading: concrete SCos constraints and proposed stages

Inspected SCos baseline: `93046d406870213ae9ff2c0f82f398ddfa60bdf9`.

- `boot/uefi/main.c:read_file` loads a whole file and rejects sizes over 16 MiB.
  It reads from the boot image's own filesystem before ExitBootServices.
- `kernel/include/boot.h` has no module manifest/payload handoff.
- Existing ELF loading handles the kernel at boot, not runtime driver modules.
  Merely adding an ELF parser would not supply the Linux/Haiku/Genode APIs.
- VFS reads return RAM data; ATA persistence has a 128 KiB snapshot budget.
  This is not a disk-backed module filesystem. USB input support is not USB
  mass-storage support. Current drivers contain no AHCI/NVMe storage backend.

**Proposed stage A — selected boot loading, not claimed runtime streaming:**
keep a small driver/device manifest and separate packages on the boot filesystem.
Before ExitBootServices, inventory devices and read only the matched, enabled
package set and its exact firmware, through bounded I/O chunks. Pass selected
payloads and ownership/range metadata to the kernel. Leave all unrelated driver
binaries on disk. This can avoid requiring all storage drivers immediately on a
USB boot. It still needs a genuine module ABI/loader and the selected upstream
OS-service adaptations; none has been implemented here.

**Proposed stage B — genuine post-boot disk-backed loading:**
port a suitable existing storage implementation for the required transport(s),
add bounded offset reads and filesystem lookup, then load selected modules on
demand. USB boot needs a mass-storage transport and block layer, not an AHCI-only
solution. Identify the boot volume reliably; never pick the first writable disk.
Storage-source selection needs its own audit, not a claim that it is solved here.

**Memory rules:**
- Keep inactive packages on disk. Do not copy every driver into the RAM VFS.
- Record code/data, firmware staging, pinned DMA, GPU buffers and cache usage
  separately; budget the whole selected stack, not the small wrapper.
- Chunked file reads reduce staging allocations, not all required residency.
  A firmware upload may require a complete contiguous/pinned image. Streaming
  cannot promise that a 60 MiB firmware costs only a 64 KiB buffer.
- Keep executing driver code and live DMA/interrupt resources resident. Do not
  demand-page them without a safe fault/I/O design. Release temporary staging
  only once the upstream driver no longer references it.
- Verify module ABI, architecture, relocation bounds, package integrity and
  firmware versions before initializing hardware. Hashes provide integrity, not
  trust against a malicious replacement package.
- Multiple adapters can legitimately require multiple active drivers. Bind once
  per device, coordinate ownership, and do not reset the firmware display merely
  to try every candidate. Prefer a verified display-preserving path initially.
- CPU fallback after a failed modeset requires actual display restoration; the
  old GOP framebuffer is not guaranteed usable after a destructive GPU takeover.

## Verification gates: all currently NOT PASSED for these SCos ports

| Gate | Required evidence |
| --- | --- |
| Reuse/legal | Pinned source, complete dependency inventory, per-component notices and firmware redistribution terms |
| Build/ABI | SCos-target builds and symbol/relocation checks; no unresolved host-OS calls or fake success stubs |
| Matching/loading | Exact IDs/revisions; absent/unsupported device, missing/corrupt/wrong-version files; inactive packages remain unloaded |
| Safe initialization | Bounded waits, allocator/IRQ/DMA failures, ownership and teardown; no out-of-bounds MMIO or invalid fallback framebuffer |
| GPU execution | Actual submitted GPU operation, completion/fence, readback compared with a CPU reference; changing device/driver name is not evidence |
| Desktop | Real compositor rendering through the driver, resize/drag/text/cursor/damage, scanout, failure recovery and memory accounting |
| Physical | Exact PCI/subsystem/revision, firmware hash, monitor/output and test results on real hardware for each claimed-tested configuration |
| Regression | Existing CAT/Studio/compositor/management suites, no input regression, repeated boot and fallback tests |

The source audit itself was executed. It verifies reproducible file hashes,
source sizes and the cited selection/dependency evidence, NOT driver operation.
This sandbox has no exposed `/dev/dri` or `/dev/kvm` at inspection time and no
provided physical GPU passthrough. Therefore no physical Intel/AMD/NVIDIA test
was performed. Future QEMU tests must be labeled separately; unsupported and
untested devices must not be promoted to a verified list.

## Recommended next implementation boundary

Start with the shared selected-package loader/OS-service contract, then bring up
existing vendor drivers as separate ports. Genode Intel is the smallest complete
render-driver directory found here, but licensing and its missing Rocket Lake
configuration are real gates. RadeonGfx is narrower and has unresolved licensing;
Nebula has the most directly reusable NVIDIA OS glue but large dependencies.
Do not quietly import any of these as a sub-10,000-line feature: even Intel's
9,505-line directory excludes necessary services and Mesa. A broad port exceeds
that earlier size limit. Resolve this scope explicitly before importing it.

No compatible-and-verified SCos GPU driver was established by this research.
No driver code or firmware was added to the boot image; no new image is released.

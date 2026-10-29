# SCos

SCos is a native x64 UEFI operating system with its own kernel, desktop and apps.
It is not Linux and is not going to become Linux.

## What must stay true

* `dist/scos-32bit.img` is frozen at r43. Do not overwrite it, and keep its
  checksum/provenance guard intact.
* `os.html` stays as it is.
* Native x64 UEFI only. No BIOS/CSM, no 32-bit build target restored.
* The OS identity - kernel, desktop, apps, look - is the point. Do not replace
  working SCos code with something else in order to get a feature.

## The task

**Make the GPU accelerate SCos.** Everything else is in service of that.

* No size limits. If the firmware, the image or a buffer needs to be bigger, make
  it bigger. `dist/scos.img` carries whatever the card needs to be started, so the
  machine that boots it does not have to supply anything.
* No staged delivery. Fix everything that is in the way and deliver an image that
  boots on the user's RTX 5050 with the GPU doing work.
* Write the driver. Networking, a browser engine and other unrelated hardware are
  still out of scope; the NVIDIA/GSP path is the scope.
* Real hardware logs from the user are ground truth about the machine. QEMU can
  never prove a GB207 claim; it proves the code paths around it.
* Say what was measured and what was not. Do not dress an emulator result up as a
  hardware result, and do not withhold a working fix behind a verification gate.

## Practical notes

* `make` builds; `make gpu-modules` builds and verifies the driver modules;
  `make dist` writes `dist/scos.img` + record + checksum; `make ventoy` writes the
  same bytes to `build/scos-ventoy.img` for a stick.
* `python3 tools/check_delivery.py` and `python3 tools/check_milestone.py` run
  before delivery. `python3 tools/tests/test_*.py` is the suite.
* Use QEMU snapshots. Never attach host physical disks.

#!/usr/bin/env bash
# Write this build to a USB stick, together with the one firmware file no public archive carries.
#
# Why this exists, in one sentence: the resource-manager image that starts the GSP on a GB20x - the file
# /lib/firmware/nvidia/<chip>/gsp/gsp-<version>.bin, 63571696 bytes for the driver on the machine this project
# is developed against - is only ever shipped inside an installed NVIDIA driver, so it cannot be downloaded,
# cannot be committed (a git host refuses a 100 MiB file and this repository's whole image is 64 MiB), and a
# 64 MiB FAT32 volume physically cannot hold it.  The image in dist/ therefore carries the FMC container - the
# piece the security processor verifies and boots - and this script builds a second, bigger image, sized to
# the stick, with both files inside.  Nothing is copied onto a mounted filesystem afterwards and no partition
# tool is needed: the packer writes the whole disk, so the stick comes out bootable in one dd.
#
#   tools/flash.sh --device /dev/sdb
#   tools/flash.sh --device /dev/sdb --firmware /lib/firmware/nvidia/gb207/gsp
#   tools/flash.sh --device /dev/sdb --image dist/scos.img --yes     # exactly the tracked image, no RM image
#   tools/flash.sh --device /dev/sdb --dry-run                       # print what would be done, write nothing
#
# With --firmware (or one auto-detected under /lib/firmware/nvidia/*/gsp) the script re-packs build/ at the
# device's size using the same tools/makedisk.py that makes dist/scos.img, and refuses to write an image it
# could not read back out of its own FAT.  Without firmware it writes dist/scos.img as it stands.  Either way
# it re-reads the bytes it wrote and compares the hash, then prints the build record off the stick, because a
# flash that silently stopped short is the failure this whole tool exists to make impossible.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
device=''
image="$root/dist/scos.img"
firmware=''
size=''
yes=0
dry=0

die() { printf 'flash.sh: %s\n' "$*" >&2; exit 2; }
step() { printf '%s\n' "$*"; }
run() {
    if [ "$dry" = 1 ]; then printf '  would run: %s\n' "$*"; return 0; fi
    "$@"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --device)    device=${2:-}; shift 2 ;;
        --image)     image=${2:-}; shift 2 ;;
        --firmware)  firmware=${2:-}; shift 2 ;;
        --size)      size=${2:-}; shift 2 ;;
        --yes)       yes=1; shift ;;
        --dry-run)   dry=1; shift ;;
        -h|--help)   sed -n '2,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)           die "unknown argument '$1'; --help lists the five that exist" ;;
    esac
done

[ -n "$device" ] || die "--device is required, and it has to name a whole disk such as /dev/sdb"
[ -b "$device" ] || [ -f "$device" ] || die "$device is neither a block device nor a regular file; a file is
         accepted so the whole of this script can be rehearsed against an empty image before anything real is
         overwritten - see --help"
[ -e "$image" ] || die "$image is missing: run 'make dist' in $root first"
command -v python3 >/dev/null || die "python3 is needed to pack and to read the result back"

# A whole disk, not a partition, and not the disk the system is running from.  Both checks are about the one
# way this script can cost somebody their machine: `dd` writes what it is told to and does not ask twice.
# The test is whether the kernel calls it a disk in its own right: /sys/block/<name> exists for sda, vdb,
# nvme0n1 and mmcblk0 and not for their partitions, which is the only rule that holds for every naming scheme -
# a suffix test would reject an NVMe disk, whose own name ends in a digit.
disk=${device#/dev/}
if [ -b "$device" ] && [ ! -e "/sys/block/$disk" ]; then
    die "$device is not a whole disk (/sys/block/$disk does not exist): the image carries a partition table and
         has to be written from the first sector of the device, so name the disk itself and not one of its
         partitions - lsblk -o NAME,TYPE,SIZE,MODEL is the list to pick from"
fi
if [ -b "$device" ] && command -v lsblk >/dev/null; then
    [ "$(lsblk -dno TYPE "$device" 2>/dev/null || true)" = disk ] \
        || die "$device is not a whole disk according to lsblk"
    mounts=$(lsblk -nrpo MOUNTPOINT "$device" 2>/dev/null | sed '/^$/d' || true)
    [ -z "$mounts" ] || die "partitions of $device are mounted ($mounts); unmount them first - writing under " \
                           "a mounted filesystem corrupts both the stick and whatever was reading it"
    for parent in $(lsblk -nro PKNAME "$device" 2>/dev/null | sed '/^$/d' || true); do
        die "$device is a child of $parent; refuse to overwrite a device that is part of another one"
    done
fi
devsize=$( (blockdev --getsize64 "$device" 2>/dev/null) || stat -c %s "$device" 2>/dev/null || echo 0)
if [ -f "$device" ]; then
    step "rehearsal mode: $device is a file, so every check and every byte below runs against it as if it were"
    step "the stick, without needing root and without touching a device"
fi
[ "$devsize" -gt 0 ] || die "cannot tell how big $device is, so cannot tell whether the image fits on it"

# Where the firmware comes from.  One directory under /lib/firmware/nvidia/*/gsp is taken as the answer;
# two or more is a question, because the wrong chip's image is worse than none - it boots, and then the
# security processor refuses a signature that belongs to a different part.
if [ -z "$firmware" ]; then
    found=$(find /lib/firmware/nvidia -mindepth 2 -maxdepth 2 -type d -name gsp 2>/dev/null | sort || true)
    n=$(printf '%s' "$found" | grep -c . || true)
    if [ "${n:-0}" = 1 ]; then
        firmware=$found
    elif [ "${n:-0}" -gt 1 ]; then
        die "several driver firmware directories exist ($(printf '%s' "$found" | tr '\n' ' ')); pass the one " \
            "for the chip in this machine with --firmware DIR, which is also the answer when you know the " \
            "card is newer than the installed driver"
    fi
fi
[ -z "$firmware" ] || [ -d "$firmware" ] || die "$firmware is not a directory"

plan="$image"
work=''
if [ -n "$firmware" ]; then
    [ -d "$root/build/gsp" ] || die "$root/build is not packed: run 'make gpu-modules gsp-firmware' first, or " \
                                   "--image to flash a finished image instead of repacking one"
    bytes=${size:-$devsize}
    [ $((bytes % 512)) -eq 0 ] || bytes=$(( bytes / 512 * 512 ))
    [ "$bytes" -le "$devsize" ] || die "--size $size is larger than $device ($devsize bytes)"
    step "checking $(printf '%s' "$firmware" | sed 's|.*/nvidia/||;s|/gsp$||')'s coprocessor firmware"
    python3 "$root/tools/gspcheck.py" "$firmware" || die "gspcheck refused the files in $firmware; fix the file " \
        "it names, or pass the directory whose files pass - the packer will not be the one to discover that a " \
        "container is truncated"
    work=$(mktemp -t scos-stick-XXXXXXXX.img)
    step "packing build/ to $bytes bytes with the firmware from $firmware"
    if [ "$dry" = 1 ]; then
        printf '  would run: python3 %s/tools/makedisk.py build --size %d --firmware %s --out %s\n' \
               "$root" "$bytes" "$firmware" "$work"
        plan=$work
    else
        ( cd "$root" && python3 tools/makedisk.py build --size "$bytes" --firmware "$firmware" --out "$work" )
        plan=$work
        bytes=$(stat -c %s "$plan")
    fi
    image_sha=$(sha256sum "$plan" | cut -d' ' -f1)
else
    image_sha=$(sha256sum "$plan" | cut -d' ' -f1)
    bytes=$(stat -c %s "$plan")
    if [ "$bytes" -gt "$devsize" ]; then
        [ -f "$device" ] || die "$plan is $bytes bytes and $device holds $devsize: nothing was written"
        truncate -s "$bytes" "$device"
        devsize=$bytes
    fi
    step "no driver firmware found: writing the tracked image, which carries the FMC container and not the"
    step "resource-manager image, so the panel will report that the boot descriptors were not staged"
fi

step "about to write $bytes bytes to $device ($(basename "$device")), sha256 $image_sha"
step "everything currently on $device will be gone.  This cannot be undone by anything in this repository."
if [ "$yes" != 1 ] && [ "$dry" != 1 ]; then
    die "refusing to write without --yes, which is the flag that says the device named above is the stick and " \
        "not the disk the operating system is installed on"
fi

cleanup() { [ -n "$work" ] && rm -f "$work"; }
trap cleanup EXIT

if [ "$(id -u)" != 0 ] && [ "$dry" != 1 ] && [ -b "$device" ]; then
    die "writing to a raw disk needs root: rerun with sudo (or --device a file to rehearse the whole flow)"
fi
run dd if="$plan" of="$device" bs=1M conv=notrunc,fsync status=none
run sync
if [ "$dry" = 1 ]; then step "dry run: nothing was written to $device"; exit 0; fi

# Read back what the stick actually holds.  The hash of the first `bytes` of the device has to equal the hash of
# the image that was written, which catches a short write, a zero-filled USB controller that reported success,
# and the class of failure where the FAT directory is present but a file's clusters were not.
step "reading the stick back"
read_sha=$(python3 - "$device" "$bytes" <<'EOPY'
import hashlib, sys
path, limit = sys.argv[1], int(sys.argv[2])
digest = hashlib.sha256()
with open(path, 'rb', buffering=0) as handle:
    left = limit
    while left:
        block = handle.read(min(left, 1 << 21))
        if not block:
            break
        digest.update(block)
        left -= len(block)
if left:
    sys.exit('%s ends %d bytes short of the %d that were written' % (path, left, limit))
print(digest.hexdigest())
EOPY
)
[ "$read_sha" = "$image_sha" ] || die "$device holds $read_sha where the image is $image_sha: the write did not " \
    "land, so booting it would run whatever the stick had before, which is the most confusing thing that can " \
    "happen to a report"
python3 "$root/tools/verify-stick.py" "$device" || die "the record on $device could not be read: the stick was " \
    "written byte for byte, so this is the partition table or the ESP telling you something about the device"
step "safe to remove.  Boot from $device and the panel's 'graphics fsp' paragraph should now name the"
step "resource-manager image: with it, the boot descriptors are staged and the handover asks for a real load."

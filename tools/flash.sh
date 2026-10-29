#!/usr/bin/env bash
# Put this build on a USB stick - as a file, or on a raw disk when there is nothing else to keep.
#
# Two ways to get these bytes onto a stick, and the first one no longer destroys anything:
#
#   tools/flash.sh --ventoy /run/media/me/Ventoy      # writes SCOS-<commit>.img to copy beside your ISOs
#   tools/flash.sh                                    # finds a mounted Ventoy partition and does the same
#   tools/flash.sh --device /dev/sdb                  # raw write, for a stick with nothing on it
#
# Why the file mode exists: a Ventoy stick boots an image file directly - it presents the file to the firmware
# as a block device, needs no extraction, and does not care whether the file is contiguous - so the whole
# `dd`-the-disk step is only necessary on a stick that is not already set up to boot files. Overwriting a Ventoy
# install to write one OS image onto the same stick is the worse of the two options, which is why --device now
# refuses a disk whose partitions carry a Ventoy label.
#
# Why either mode has to size the volume at all: the resource-manager image that starts the GSP on a GB20x -
# /lib/firmware/nvidia/<chip>/gsp/gsp-<version>.bin, 63571696 bytes for the driver on the machine this project
# is developed against - ships only inside an installed NVIDIA driver. It cannot be downloaded, cannot be
# committed here, and does not fit the 64 MiB tracked image, whose ESP has about 60 MiB free. With --firmware
# (or one auto-detected) this script re-packs build/ with tools/makedisk.py at a size that holds it, which is
# the only way those bytes can arrive on a machine that has them and a stick that can carry them.
#
#   tools/flash.sh --device /dev/sdb --firmware /lib/firmware/nvidia/gb207/gsp
#   tools/flash.sh --image dist/scos.img --ventoy . --size 256M   # pack anyway, at a size you chose
#   tools/flash.sh --device /dev/sdb --yes                        # raw write, past the safety
#   tools/flash.sh --ventoy /tmp --dry-run                        # print what would be done, write nothing
#
# Both modes finish by reading the bytes back: the file mode reads the image it just packed out of its own FAT,
# and the device mode hashes what the stick now holds against the hash of what was written. A packer that can
# silently omit an input, or a write that silently stops short, is the class of failure that costs a boot
# cycle, and neither is allowed to be discovered by whoever has the machine in front of them.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
device=''
ventoy=''
image="$root/dist/scos.img"
firmware=''
size=''
yes=0
dry=0
force=0

die() { printf 'flash.sh: %s\n' "$*" >&2; exit 2; }
step() { printf '%s\n' "$*"; }
run() {
    if [ "$dry" = 1 ]; then printf '  would run: %s\n' "$*"; return 0; fi
    "$@"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --device)     device=${2:-}; shift 2 ;;
        --ventoy)     ventoy=${2:-}; shift 2 ;;
        --image)      image=${2:-}; shift 2 ;;
        --firmware)   firmware=${2:-}; shift 2 ;;
        --size)       size=${2:-}; shift 2 ;;
        --yes)        yes=1; shift ;;
        --force)      force=1; shift ;;
        --dry-run)    dry=1; shift ;;
        -h|--help)    sed -n '2,36p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)            die "unknown argument '$1'; --help lists the ones that exist" ;;
    esac
done

[ -z "$device" ] || [ -z "$ventoy" ] || die "--device and --ventoy are alternatives: one writes a disk, the" \
    "other writes a file to copy beside your ISOs"
[ -n "$device" ] || [ -n "$ventoy" ] || die "no target given; --ventoy DIR writes an image file you can drop" \
    "on a Ventoy stick (and finds the mounted stick itself if you give no DIR), --device /dev/sdX writes a raw" \
    "disk"
[ -e "$image" ] || die "$image is missing: run 'make dist' in $root first, which packs build/scos.img and
         copies it to dist/ with its checksum and its record beside it"
command -v python3 >/dev/null || die "python3 is needed to pack and to read the result back"

# Where the firmware comes from.  One directory under /lib/firmware/nvidia/*/gsp is taken as the answer; two or
# more is a question, because the wrong chip's image is worse than none - it boots, and then the security
# processor refuses a signature that belongs to a different part.
if [ -z "$firmware" ]; then
    found=$(find /lib/firmware/nvidia -mindepth 2 -maxdepth 2 -type d -name gsp 2>/dev/null | sort || true)
    n=$(printf '%s' "$found" | grep -c . || true)
    if [ "${n:-0}" = 1 ]; then
        firmware=$found
    elif [ "${n:-0}" -gt 1 ] && [ -n "$device$ventoy" ]; then
        die "several driver firmware directories exist ($(printf '%s' "$found" | tr '\n' ' ')); pass the one" \
            "for the chip in the machine being booted with --firmware DIR, or --firmware none to ship without" \
            "the resource-manager image at all"
    fi
fi
if [ "$firmware" = none ]; then firmware=''; fi
[ -z "$firmware" ] || [ -d "$firmware" ] || die "$firmware is not a directory"

plan="$image"
work=''
pack() {                      # $1: the file to write; the caller owns whether it is temporary
    # Pack (or refuse to) at the size the content actually needs.  `makedisk --min-size' answers with the
    # smallest image that fits what is staged, so the number comes from the same files the packer reads rather
    # than from a remembered size that rounds itself correct.
    local want out=$1 args=()
    [ -n "$firmware" ] && args+=(--firmware "$firmware")
    if [ -n "$size" ]; then
        want=$size
    else
        want=$(cd "$root" && python3 tools/makedisk.py build --min-size "${args[@]}" | cut -f1)
        want=$(( (want + 8388607) / 8388608 * 8388608 ))      # round up to 8 MiB, so the size reads as a size
    fi
    step "packing build/ to $want bytes ($(python3 -c "print('%dM' % ($want // 1048576))")) with ${firmware:-the firmware staged in build/gsp}"
    if [ "$dry" = 1 ]; then
        printf '  would run: python3 %s/tools/makedisk.py build --size %s --out %s%s\n' \
               "$root" "$want" "$out" "${firmware:+ --firmware $firmware}"
        return 0
    fi
    ( cd "$root" && python3 tools/makedisk.py build --size "$want" --out "$out" "${args[@]}" )
}

if [ -n "$ventoy" ]; then
    # The file mode: no root, no device, nothing destroyed but the file being written.
    if [ ! -d "$ventoy" ]; then
        # Not given a directory: look for a mounted Ventoy data partition, which is the stick being asked about.
        mount=$(lsblk -rno LABEL,MOUNTPOINT 2>/dev/null | awk 'tolower($1)=="ventoy" && $2!="" {print $2}' | head -1)
        [ -n "$mount" ] || die "$ventoy is not a directory and no mounted Ventoy partition was found; pass the
             directory the stick is mounted on, e.g. --ventoy /run/media/\$USER/Ventoy"
        ventoy=$mount
    fi
    [ -w "$ventoy" ] || [ "$dry" = 1 ] || die "$ventoy is not writable by $(id -un): the copy step is the part
             that needs to land on the stick, so mount it where you can write, or run this with sudo"
    commit=$(cd "$root" && git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
    target="$ventoy/SCOS-$commit.img"
    if [ -e "$target" ] && [ "$force" != 1 ]; then
        die "$target already exists; pass --force to overwrite it, which is a separate flag from --yes because
             overwriting a file on a stick is not the same as overwriting the stick"
    fi
    if [ -n "$firmware" ]; then
        [ -d "$root/build/gsp" ] || [ -d "$root/build/gpu" ] || die "$root/build is not packed: run" \
            "'make gpu-modules gsp-firmware' first, or drop --firmware to copy the finished image instead"
        step "checking the coprocessor firmware in $(basename "$(dirname "$firmware")")"
        python3 "$root/tools/gspcheck.py" "$firmware" || die "gspcheck refused the files in $firmware; fix the" \
            "file it names, or pass the directory whose files pass - the packer will not be the one to discover" \
            "that a container is truncated"
        pack "$target"
        plan=$target
    else
        step "no driver firmware given: copying $image as it stands, which carries the FMC container and not the"
        step "resource-manager image, so the panel will report that the boot descriptors were not staged"
        plan=$image
    fi
    step "the file to boot is $plan ($(stat -c %s "$plan" 2>/dev/null || echo "${size:-an unbuilt size} bytes"))"
    sha=$(sha256sum "$plan" | cut -d' ' -f1)
    [ "$dry" = 1 ] || { cp "$plan" "$target" 2>/dev/null || [ "$plan" = "$target" ]; }
    if [ "$dry" != 1 ]; then
        read_sha=$(python3 - "$target" <<'EOPY'
import hashlib, sys
with open(sys.argv[1], 'rb') as handle:
    print(hashlib.sha256(handle.read()).hexdigest())
EOPY
)
        [ "$read_sha" = "$sha" ] || die "$target reads back as $read_sha and the image is $sha: the copy did not" \
            "land, which on a stick written by a nervous file manager is exactly as bad as a short dd"
        python3 "$root/tools/verify-stick.py" "$target" || die "the record inside $target could not be read back"
        printf '  sha256 %s\n' "$sha"
    fi
    step "reboot, pick SCOS-$commit.img in the Ventoy menu, and the panel's 'graphics fsp' paragraph says what"
    step "the stick carried.  If the firmware wants to write settings back, the same file can be added again"
    step "later - nothing here touched the stick's own partition table."
    exit 0
fi

# The device mode.  A whole disk, not a partition, and never a stick that is already doing this job for
# something else: `dd` writes what it is told to and does not ask twice.
[ -b "$device" ] || [ -f "$device" ] || die "$device is neither a block device nor a regular file; a file is
         accepted so the whole of this script can be rehearsed against an empty image - or use --ventoy, which
         is the mode that writes a file on purpose"
disk=${device#/dev/}
if [ -b "$device" ] && [ ! -e "/sys/block/$disk" ]; then
    die "$device is not a whole disk (/sys/block/$disk does not exist): the image carries a partition table and
         has to be written from the first sector of the device, so name the disk itself and not one of its
         partitions - lsblk -o NAME,TYPE,SIZE,MOUNTPOINT is the list to pick from"
fi
if [ -b "$device" ] && command -v lsblk >/dev/null; then
    [ "$(lsblk -dno TYPE "$device" 2>/dev/null || true)" = disk ] \
        || die "$device is not a whole disk according to lsblk"
    mounts=$(lsblk -nrpo MOUNTPOINT "$device" 2>/dev/null | sed '/^$/d' || true)
    [ -z "$mounts" ] || die "partitions of $device are mounted ($mounts); unmount them first - writing under" \
                           "a mounted filesystem corrupts both the stick and whatever was reading it"
    labels=$(lsblk -rno LABEL "$device" 2>/dev/null | tr '[:upper:]' '[:lower:]' | grep -i ventoy || true)
    if [ -n "$labels" ] && [ "$force" != 1 ]; then
        die "$device has a Ventoy filesystem on it, and this would erase it: run the same command with" \
            "--ventoy instead, which writes an image file onto that stick for Ventoy to boot, or --force if" \
            "the Ventoy install is the thing you meant to remove"
    fi
fi
devsize=$( (blockdev --getsize64 "$device" 2>/dev/null) || stat -c %s "$device" 2>/dev/null || echo 0)
[ "$devsize" -gt 0 ] || die "cannot tell how big $device is, so cannot tell whether the image fits on it"
if [ -f "$device" ]; then
    step "rehearsal mode: $device is a file, so every check and every byte below runs against it as if it were"
    step "the stick, without needing root and without touching a device"
fi

if [ -n "$firmware" ]; then
    [ -d "$root/build/gsp" ] || [ -d "$root/build/gpu" ] || die "$root/build is not packed: run" \
        "'make gpu-modules gsp-firmware' first, or --image to flash a finished image instead of repacking one"
    bytes=${size:-$devsize}
    [ $((bytes % 512)) -eq 0 ] || bytes=$(( bytes / 512 * 512 ))
    [ "$bytes" -le "$devsize" ] || die "--size $size is larger than $device ($devsize bytes)"
    step "checking the coprocessor firmware in $(basename "$(dirname "$firmware")")"
    python3 "$root/tools/gspcheck.py" "$firmware" || die "gspcheck refused the files in $firmware; fix the file" \
        "it names, or pass the directory whose files pass - the packer will not be the one to discover that a" \
        "container is truncated"
    work=$(mktemp -t scos-stick-XXXXXXXX.img)
    size=$bytes
    pack "$work"
    plan=$work
    [ "$dry" = 1 ] || bytes=$(stat -c %s "$plan")
else
    bytes=$(stat -c %s "$plan")
    [ "$bytes" -le "$devsize" ] || die "$plan is $bytes bytes and $device holds $devsize: nothing was written"
    step "no driver firmware given: writing the tracked image, which carries the FMC container and not the"
    step "resource-manager image, so the panel will report that the boot descriptors were not staged"
fi
[ "$dry" = 1 ] || image_sha=$(sha256sum "$plan" | cut -d' ' -f1)
image_sha=${image_sha:-unreadable-in-dry-run}

step "about to write $bytes bytes to $device ($(basename "$device")), sha256 $image_sha"
step "everything currently on $device will be gone.  This cannot be undone by anything in this repository."
if [ "$yes" != 1 ] && [ "$dry" != 1 ]; then
    die "refusing to write without --yes, which is the flag that says the device named above is the stick and" \
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
[ "$read_sha" = "$image_sha" ] || die "$device holds $read_sha where the image is $image_sha: the write did not" \
    "land, so booting it would run whatever the stick had before, which is the most confusing thing that can" \
    "happen to a report"
python3 "$root/tools/verify-stick.py" "$device" || die "the record on $device could not be read: the stick was" \
    "written byte for byte, so this is the partition table or the ESP telling you something about the device"
step "safe to remove.  Boot it and the panel's 'graphics fsp' paragraph should now name the resource-manager"
step "image: with it, the boot descriptors are staged and the handover asks for a real load."

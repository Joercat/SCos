#!/usr/bin/env python3
"""Fail if the permanent 32-bit artifact, checksum or provenance drifts."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NAME = "dist/scos-32bit.img"
SHA256 = "84f87224a5fef6e2cd0982b28708e68a8d44f0a523b010f8c2cb23f43e7cb926"
SOURCE = "6d10e3f41cc7446c0ed77fb8d021cab54662fbc9"


def main():
    image = (ROOT / NAME).read_bytes()
    if len(image) != 8388608 or hashlib.sha256(image).hexdigest() != SHA256:
        raise ValueError("Permanent scos 32bit image was changed or truncated")
    if (ROOT / (NAME + ".sha256")).read_text() != f"{SHA256}  {NAME}\n":
        raise ValueError("Milestone checksum file does not match the pinned image")
    meta = json.loads((ROOT / "docs/milestones/scos-32bit.json").read_text())
    expected = dict(artifact=NAME, architecture="i386", build_tag="r42",
                    source_commit=SOURCE, sha256=SHA256, bytes=len(image))
    for key, value in expected.items():
        if meta.get(key) != value:
            raise ValueError(f"Milestone provenance mismatch: {key}")
    print(f"PASS: permanent scos 32bit / r42 — {SHA256}")
    name = "dist/scos-32bit-r43.img"
    sha = "43c2bce097389179aff9965779148c82a4907017c392f996a6602f727efb0c97"
    image43 = (ROOT / name).read_bytes()
    if len(image43) != 8388608 or hashlib.sha256(image43).hexdigest() != sha:
        raise ValueError("Permanent scos 32bit r43 image changed or truncated")
    if (ROOT / (name + ".sha256")).read_text() != f"{sha}  {name}\n":
        raise ValueError("r43 milestone checksum mismatch")
    meta43 = json.loads((ROOT / "docs/milestones/scos-32bit-r43.json").read_text())
    expected43 = dict(artifact=name, architecture="i386", build_tag="r43",
                      source_commit="edb89550fff5f32363be53d6f47a2ceb1973fa2f",
                      sha256=sha, bytes=8388608)
    for key, value in expected43.items():
        if meta43.get(key) != value:
            raise ValueError(f"r43 provenance mismatch: {key}")
    print(f"PASS: permanent scos 32bit / r43 — {sha}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as exc:
        raise SystemExit(f"Milestone check failed: {exc}")

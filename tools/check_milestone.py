#!/usr/bin/env python3
"""Fail if the current verified 32-bit artifact, checksum or provenance drifts."""
# Update pins only alongside a verified 32-bit patch before conversion.
# Freeze this image/pin when the user authorizes starting 64-bit.
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NAME = "dist/scos-32bit.img"
SHA256 = "43c2bce097389179aff9965779148c82a4907017c392f996a6602f727efb0c97"
SOURCE = "edb89550fff5f32363be53d6f47a2ceb1973fa2f"


def main():
    image = (ROOT / NAME).read_bytes()
    if len(image) != 8388608 or hashlib.sha256(image).hexdigest() != SHA256:
        raise ValueError("Verified scos 32bit image was changed or truncated")
    if (ROOT / (NAME + ".sha256")).read_text() != f"{SHA256}  {NAME}\n":
        raise ValueError("Milestone checksum file does not match the pinned image")
    meta = json.loads((ROOT / "docs/milestones/scos-32bit.json").read_text())
    expected = dict(artifact=NAME, architecture="i386", build_tag="r43",
                    source_commit=SOURCE, sha256=SHA256, bytes=len(image))
    for key, value in expected.items():
        if meta.get(key) != value:
            raise ValueError(f"Milestone provenance mismatch: {key}")
    print(f"PASS: verified scos 32bit / r43 — {SHA256}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as exc:
        raise SystemExit(f"Milestone check failed: {exc}")

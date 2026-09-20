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


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as exc:
        raise SystemExit(f"Milestone check failed: {exc}")

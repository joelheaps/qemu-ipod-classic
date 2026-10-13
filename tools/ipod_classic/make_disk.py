"""Create an empty bootable iPod classic disk as qcow2; the logic behind scripts/ipod/make-disk.

Layout (what the retailOS expects): an MBR in 4096-byte units, the firmware partition (a
Firmware-XX payload) at block 8 (type 0x00), then an empty 1 GiB FAT32 (type 0x0b). The firmware
partition carries the fonts the UI needs; without it a boot reset-loops. Flags:
README.md. Assumes it runs where mkfs.fat and BUILD_DIR/qemu-img are available (the
docker:ipod:make-disk wrapper runs the same task in the dev container).
"""

import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from cli import REPO, parse_flags, repo_path
from fwtool import read_firmware
from ipod_library import PORT_BLUE, populate

BS = 4096
FW_BLOCK = 8  # firmware partition starts at byte 0x8000
FAT_BYTES = 1 << 30
USAGE = (
    "usage: make-disk --firmware=PATH --disks=PATH [--track=PATH|portblue] "
    "(or FIRMWARE=, DISK=, TRACK= in the environment)"
)


def build_image(payload: bytes, out: Path, track: Path | None = None) -> None:
    """Write the disk for PAYLOAD to OUT as qcow2 (needs mkfs.fat and BUILD_DIR/qemu-img).

    With TRACK the volume holds a one-track library (needs mtools, ffprobe and libgpod too).
    """
    fw = payload + b"\0" * (-len(payload) % BS)
    with tempfile.TemporaryDirectory() as tmp:
        fat, raw = Path(tmp, "fat"), Path(tmp, "disk")
        with fat.open("wb") as f:
            f.truncate(FAT_BYTES)
        subprocess.check_call(["mkfs.fat", "-F", "32", "-s", "8", fat], stdout=subprocess.DEVNULL)  # noqa: S603, S607
        if track:
            populate(fat, track)
        fatb = bytearray(fat.read_bytes())
        # The osos volume classifier rejects a FAT volume unless "FAT" sits at +0x34 of the boot
        # sector (BPB reserved bytes) and its backup.
        bk = struct.unpack("<H", fatb[0x32:0x34])[0]
        for base in {0, bk * 512}:
            fatb[base + 0x34 : base + 0x37] = b"FAT"
        fw_blocks = len(fw) // BS
        parts = [(0x00, FW_BLOCK, fw_blocks), (0x0B, FW_BLOCK + fw_blocks, len(fatb) // BS)]
        mbr = bytearray(BS)
        for i, (ptype, lba, cnt) in enumerate(parts):
            e = 0x1BE + 16 * i
            mbr[e + 4] = ptype
            mbr[e + 8 : e + 16] = struct.pack("<II", lba, cnt)
        mbr[0x1FE:0x200] = b"\x55\xaa"
        raw.write_bytes(mbr + b"\0" * (BS * (FW_BLOCK - 1)) + fw + fatb)
        subprocess.check_call(  # noqa: S603
            [
                REPO / os.environ.get("BUILD_DIR", "build/native") / "qemu-img",
                "convert",
                "-O",
                "qcow2",
                raw,
                out,
            ]
        )


def parse_args(argv: list[str]) -> dict[str, str]:
    """Parse the flags (FIRMWARE and DISK are the environment defaults)."""
    return parse_flags(argv, {"firmware": "FIRMWARE", "disks": "DISK", "track": "TRACK"}, USAGE)


def prepare(argv: list[str]) -> tuple[Path, Path, bytes, str | None, Path | None] | None:
    """Resolve and validate the firmware/disks flags; return None when there is nothing to build."""
    opts = parse_args(argv)
    # With no firmware configured there is nothing to build.
    if not argv and not opts["firmware"]:
        print("make-disk: no --firmware/FIRMWARE given; nothing to do")
        return None
    if not opts["disks"]:
        raise SystemExit(USAGE)
    out = repo_path(opts["disks"])
    if out.exists():
        print(f"make-disk: {opts['disks']} exists")
        return None
    if not opts["firmware"]:
        raise SystemExit(USAGE)
    fw = repo_path(opts["firmware"])
    try:
        payload, version = read_firmware(fw)
    except (ValueError, OSError) as e:
        sys.exit(f"make-disk: {e}")
    track = (
        PORT_BLUE
        if opts["track"] == "portblue"
        else repo_path(opts["track"])
        if opts["track"]
        else None
    )
    if track and not track.is_file():
        sys.exit(f"make-disk: no such track {track}")
    return fw, out, payload, version, track


def main(argv: list[str]) -> int:
    """Run make-disk, assuming mkfs.fat and BUILD_DIR/qemu-img are available."""
    prepared = prepare(argv)
    if prepared is None:
        return 0
    _, out, payload, version, track = prepared
    out.parent.mkdir(parents=True, exist_ok=True)
    build_image(payload, out, track)
    print(
        f"make-disk: created {out}"
        + (f" from firmware {version}" if version else "")
        + (f" with track {track.name}" if track else "")
    )
    return 0

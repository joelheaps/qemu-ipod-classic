# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Parse and extract iPod Classic 6G (S5L8702) firmware images.

Handles two nested container formats:

  1. The classic iPod firmware partition ("]ih[" header + image directory).
     This is what the `Firmware-XX` payload inside an .ipsw is.
  2. The per-image S5L870x "8702" header that wraps each osos/aupd/rsrc blob.

Usage (flags as in scripts/ipod/run; see tools/ipod_classic/lib/cli.py):
    fwtool.py list    --firmware=PATH
    fwtool.py extract --firmware=PATH --outdir=DIR
    fwtool.py osos    --firmware=PATH --osos-encrypted=PATH

PATH is an .ipsw or a bare Firmware-XX payload. `osos` writes the encrypted osos image, header
first, for the on-device decrypt (emcore_s5l8702.py decrypt).
"""

import math
import plistlib
import struct
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
from cli import parse_flags, repo_path

FW_MAGIC_OFF = 0x100
FW_MAGIC = b"]ih["
FINAL_STAGE = 0x80  # NumVersion stage byte of a final release

# Directory entry, 40 bytes, little endian. Field names follow ipodpatcher.
DIRENT_FMT = "<4s4s8I"
DIRENT_SIZE = struct.calcsize(DIRENT_FMT)

# S5L870x image header. The header struct itself is small but the whole area is
# padded out to 0x800 bytes; the payload starts right after. All size/offset
# fields inside the header are relative to the start of the payload, not to the
# start of the file.
IMG_HDR_SIZE = 0x800
IMG_MAGIC = b"8702"
# The directory records the image size minus the 0x1000 that the header padding
# and alignment add on top of the payload.
DIR_LEN_SLACK = 0x1000

PRINTABLE = range(0x20, 0x7F)
HDR_FIELDS_END = 0x1C  # end of the u32 fields parsed from the image header
SHA1_OFF = 0x40
SHA1_LEN = 20
FORMAT_RETAIL = 0x03  # format / encryption type Apple ships
IMAGE_BASES = (0x1000, 0x200, 0x0)  # known offsets the directory is relative to
SCAN_START = 0x1000
SCAN_END = 0x20000
ENTROPY_SAMPLE = 0x4000
ENTROPY_ENCRYPTED = 7.9  # bits/byte above which a payload looks encrypted
BYTE_VALUES = 256


def _fourcc(raw: bytes) -> str:
    """Decode a reversed four-character code, showing junk as dots."""
    return "".join(chr(b) if b in PRINTABLE else "." for b in raw[::-1])


def _is_fourcc(raw: bytes) -> bool:
    """Return True if all four bytes are printable, i.e. this looks like a real tag."""
    return all(b in PRINTABLE for b in raw)


class DirEntry:
    """One 40-byte entry of the firmware partition's image directory."""

    def __init__(self, raw: bytes, index: int, base: int = 0) -> None:
        """Unpack `raw`; `base` is added to the stored image offset."""
        (
            dev,
            typ,
            self.id,
            dev_offset,
            self.length,
            self.addr,
            self.entry_offset,
            self.checksum,
            self.version,
            self.load_addr,
        ) = struct.unpack(DIRENT_FMT, raw)
        self.index = index
        self.dev = _fourcc(dev)
        self.type = _fourcc(typ)
        # Offsets in the directory are counted from the image base, not from
        # the start of the buffer. On a firmware partition read off the disk
        # that base is the second sector: the first one holds the ]ih[ header
        # and everything it points at is numbered from after it.
        self.raw_dev_offset = dev_offset
        self.dev_offset = dev_offset + base

    def __str__(self) -> str:
        """Format the entry as one line for `list`."""
        return (
            f"[{self.index}] {self.type:<4} dev={self.dev:<4} off=0x{self.dev_offset:08x} "
            f"len=0x{self.length:08x} ({self.length}) addr=0x{self.addr:08x} "
            f"entry=0x{self.entry_offset:08x} chk=0x{self.checksum:08x} "
            f"vers=0x{self.version:08x}"
        )


class ImageHeader:
    """The "8702" header in front of each image payload.

    Layout, verified against the osos and aupd images of iPod_24.1.1.2.ipsw:

        0x00  char[4]  "8702"
        0x04  char[3]  "1.0"
        0x07  uint8    format / encryption type (0x03 on retail images)
        0x08  uint32   unknown, zero
        0x0c  uint32   payload size, unpadded
        0x10  uint32   footer_offset + footer_size
        0x14  uint32   footer offset, relative to the payload start
        0x18  uint32   footer size
        0x40  byte[20] SHA-1
        ...            zero padding up to 0x800
    """

    def __init__(self, raw: bytes) -> None:
        """Unpack the header fields from the first bytes of an image."""
        self.magic = raw[0:4].decode("latin1")  # "8702", stored unreversed
        self.version = raw[4:7].decode("latin1")  # "1.0"
        self.format = raw[7]  # 0x03 seen on retail images
        (
            self.unknown_08,
            self.data_size,
            self.body_size,
            self.footer_offset,
            self.footer_size,
        ) = struct.unpack("<5I", raw[8:HDR_FIELDS_END])
        self.sha1 = raw[SHA1_OFF : SHA1_OFF + SHA1_LEN]

    @property
    def encrypted(self) -> bool:
        """Whether the format byte is the one Apple ships (a plain variant would differ)."""
        return self.format == FORMAT_RETAIL

    @property
    def total_size(self) -> int:
        """Size of the whole image on disk, header included."""
        return IMG_HDR_SIZE + self.body_size

    def __str__(self) -> str:
        """Format the header as one line for `list`."""
        return (
            f"magic={self.magic} ver={self.version} fmt=0x{self.format:02x} "
            f"payload=0x{self.data_size:x} footer=0x{self.footer_offset:x}+0x{self.footer_size:x} "
            f"total=0x{self.total_size:x} sha1={self.sha1.hex()}"
        )

    def check(self) -> list[str]:
        """Return a list of consistency problems, empty if the header adds up."""
        problems = []
        if self.data_size > self.footer_offset:
            problems.append(
                f"payload 0x{self.data_size:x} overruns footer at 0x{self.footer_offset:x}"
            )
        if self.footer_offset + self.footer_size != self.body_size:
            problems.append(
                f"footer_offset+footer_size 0x{self.footer_offset + self.footer_size:x} "
                f"!= 0x{self.body_size:x} at +0x10"
            )
        return problems


def _looks_like_dirent(data: bytes, off: int) -> bool:
    """Cheap validity test for a directory entry at a given offset."""
    raw = data[off : off + DIRENT_SIZE]
    if len(raw) < DIRENT_SIZE:
        return False
    if not (_is_fourcc(raw[0:4]) and _is_fourcc(raw[4:8])):
        return False
    dev_offset, length = struct.unpack("<2I", raw[0x0C:0x14])
    return 0 < dev_offset < len(data) and 0 < length <= len(data)


def _find_image_base(data: bytes, dir_offset: int) -> int:
    """Work out what the header's directory offset is relative to.

    The offset in the header is not relative to the start of the buffer, and
    what it *is* relative to varies: a Firmware-XX payload pulled out of an
    .ipsw puts the directory 0x1000 further along, while the firmware
    partition read off an iPod Video has it 0x200 further along. Rather than
    keep a table of fudge factors, try the known ones and then fall back to
    scanning for the first thing that parses as a directory entry.
    """
    for delta in IMAGE_BASES:
        if _looks_like_dirent(data, dir_offset + delta):
            return delta
    for off in range(SCAN_START, min(len(data), SCAN_END), 4):
        if _looks_like_dirent(data, off):
            return off - dir_offset
    msg = "could not locate image directory"
    raise ValueError(msg)


def read_directory(data: bytes) -> tuple[int, int, list[DirEntry]]:
    """Locate and parse the image directory.

    Returns (directory offset, image base, entries).
    """
    if data[FW_MAGIC_OFF : FW_MAGIC_OFF + 4] != FW_MAGIC:
        msg = f"no ]ih[ magic at 0x{FW_MAGIC_OFF:x}"
        raise ValueError(msg)
    (dir_offset,) = struct.unpack("<I", data[FW_MAGIC_OFF + 4 : FW_MAGIC_OFF + 8])
    image_base = _find_image_base(data, dir_offset)
    base = dir_offset + image_base

    entries = []
    off = base
    while off + DIRENT_SIZE <= len(data):
        raw = data[off : off + DIRENT_SIZE]
        if raw[:4] in (b"\0\0\0\0", b"\xff\xff\xff\xff"):
            break
        # The directory is not length-prefixed and is followed by whatever
        # happened to be on the disk, so stop as soon as an entry stops looking
        # like one. Both four-character codes must be printable and the image
        # has to start inside the file.
        if not (_is_fourcc(raw[0:4]) and _is_fourcc(raw[4:8])):
            break
        entry = DirEntry(raw, len(entries), image_base)
        if entry.dev_offset >= len(data):
            break
        entries.append(entry)
        off += DIRENT_SIZE
    return base, image_base, entries


def entropy(buf: bytes) -> float:
    """Return the Shannon entropy of `buf` in bits per byte."""
    if not buf:
        return 0.0
    counts = [0] * BYTE_VALUES
    for b in buf:
        counts[b] += 1
    n = len(buf)
    return -sum((c / n) * math.log2(c / n) for c in counts if c)


def decode_version(build_id: object) -> str:
    """Decode a manifest VisibleBuildID, a Mac NumVersion: 0x02058000 is "2.0.5".

    Bytes, high to low: major (BCD), minor and bug nibbles (BCD), stage (0x80 = final),
    pre-release revision. Anything but a final release is rejected, not guessed at.
    """
    if not isinstance(build_id, int) or not 0 <= build_id < 1 << 32:
        msg = f"VisibleBuildID {build_id!r} is not a 32-bit integer"
        raise ValueError(msg)
    major, minor, bug = (
        f"{build_id >> 24:02x}",
        f"{build_id >> 20 & 0xF:x}",
        f"{build_id >> 16 & 0xF:x}",
    )
    if not (major + minor + bug).isdigit() or build_id & 0xFFFF != FINAL_STAGE << 8:
        msg = f"VisibleBuildID 0x{build_id:08x} is not a final-release BCD version (0xMMmb8000)"
        raise ValueError(msg)
    return f"{int(major)}.{minor}.{bug}"


def read_firmware(path: Path) -> tuple[bytes, str | None]:
    """Return the Firmware-XX payload of an .ipsw or a bare payload, and the .ipsw's version.

    The payload must carry the ]ih[ magic. A bare payload has no version (None).
    """
    version = None
    try:
        if zipfile.is_zipfile(path):
            with zipfile.ZipFile(path) as z:
                info = plistlib.loads(z.read("manifest.plist"))["FirmwarePayload"]
                version = decode_version(info["VisibleBuildID"])
                data = z.read(str(info["FirmwareName"]))
        else:
            data = path.read_bytes()
    except (KeyError, TypeError, zipfile.BadZipFile, plistlib.InvalidFileException) as e:
        msg = f"{path}: not a usable .ipsw ({type(e).__name__}: {e})"
        raise ValueError(msg) from e
    if data[FW_MAGIC_OFF : FW_MAGIC_OFF + len(FW_MAGIC)] != FW_MAGIC:
        msg = f"{path}: not a firmware payload (no {FW_MAGIC.decode()} at 0x{FW_MAGIC_OFF:x})"
        raise ValueError(msg)
    return data, version


def cmd_list(path: Path, data: bytes) -> None:
    """Print the directory of a firmware partition and each image's header."""
    dir_base, image_base, entries = read_directory(data)
    print(f"file       : {path} (0x{len(data):x} bytes)")
    print(f"directory  : 0x{dir_base:x}, {len(entries)} entries (image base +0x{image_base:x})")
    for e in entries:
        print(e)
        if data[e.dev_offset : e.dev_offset + 4] == IMG_MAGIC:
            hdr = ImageHeader(data[e.dev_offset : e.dev_offset + IMG_HDR_SIZE])
            print(f"      {hdr}")
            for p in hdr.check():
                print(f"      WARNING: {p}")
            if e.length != hdr.total_size - DIR_LEN_SLACK:
                print(
                    f"      WARNING: dir len 0x{e.length:x} != total 0x{hdr.total_size:x} "
                    f"- 0x{DIR_LEN_SLACK:x}"
                )
            start = e.dev_offset + IMG_HDR_SIZE
            ent = entropy(data[start : start + ENTROPY_SAMPLE])
            note = "  -> encrypted" if ent > ENTROPY_ENCRYPTED else ""
            print(f"      payload entropy (first 16K): {ent:.3f} bits/byte{note}")
        else:
            head = data[e.dev_offset : e.dev_offset + 8]
            print(f"      (no 8702 header; first bytes {head.hex()})")


def cmd_extract(data: bytes, outdir: Path) -> None:
    """Write each image (and, for 8702 images, its payload and footer) to `outdir`."""
    _, _, entries = read_directory(data)
    outdir.mkdir(parents=True, exist_ok=True)
    for e in entries:
        size = e.length
        if data[e.dev_offset : e.dev_offset + 4] == IMG_MAGIC:
            size = ImageHeader(data[e.dev_offset : e.dev_offset + IMG_HDR_SIZE]).total_size
        blob = data[e.dev_offset : e.dev_offset + size]
        raw_path = outdir / f"{e.index}_{e.type}.img"
        raw_path.write_bytes(blob)
        print(f"wrote {raw_path} (0x{len(blob):x} bytes)")

        if blob[0:4] == IMG_MAGIC:
            hdr = ImageHeader(blob[:IMG_HDR_SIZE])
            body = blob[IMG_HDR_SIZE : IMG_HDR_SIZE + hdr.data_size]
            body_path = outdir / f"{e.index}_{e.type}.payload"
            body_path.write_bytes(body)
            print(f"  payload -> {body_path} (0x{len(body):x} bytes)")

            foot = blob[hdr.footer_offset : hdr.footer_offset + hdr.footer_size]
            if foot:
                foot_path = outdir / f"{e.index}_{e.type}.footer"
                foot_path.write_bytes(foot)
                print(f"  footer  -> {foot_path} (0x{len(foot):x} bytes)")


def cmd_osos(data: bytes, out: Path) -> None:
    """Write osos as the device decrypts it: 0x800-byte header first, padded to the AES block."""
    entry = next((e for e in read_directory(data)[2] if e.type == "osos"), None)
    if entry is None:
        msg = "no osos image in the directory"
        raise ValueError(msg)
    start = entry.dev_offset - IMG_HDR_SIZE
    blob = data[start : start + ((entry.length + IMG_HDR_SIZE + 15) & ~15)]
    out.write_bytes(blob)
    print(f"wrote {out} (0x{len(blob):x} bytes)")


COMMANDS = {
    "list": {"firmware": "FIRMWARE"},
    "extract": {"firmware": "FIRMWARE", "outdir": "OUTDIR"},
    "osos": {"firmware": "FIRMWARE", "osos-encrypted": "OSOS_ENCRYPTED"},
}


def main(argv: list[str]) -> None:
    """Run the command-line interface."""
    usage = "usage:" + (__doc__ or "").split("Usage", 1)[1].split("\n\n", 1)[0]
    if not argv or argv[0] not in COMMANDS:
        sys.exit(usage)
    opts = parse_flags(argv[1:], COMMANDS[argv[0]], usage)
    if not all(opts.values()):
        sys.exit(usage)
    path = repo_path(opts["firmware"])
    try:
        data, _ = read_firmware(path)
        if argv[0] == "list":
            cmd_list(path, data)
        elif argv[0] == "extract":
            cmd_extract(data, repo_path(opts["outdir"]))
        else:
            cmd_osos(data, repo_path(opts["osos-encrypted"]))
    except (ValueError, OSError) as e:
        sys.exit(f"fwtool: {e}")


if __name__ == "__main__":
    main(sys.argv[1:])

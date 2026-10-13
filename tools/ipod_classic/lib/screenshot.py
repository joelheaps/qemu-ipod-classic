"""screenshot.py -- the one place that reads, compares and writes screenshots (stdlib only).

Library:  f = screenshot.read('a.ppm'); screenshot.lit(f);
          screenshot.same(f, g, ignore=[(x0,y0,x1,y1)]);
          screenshot.diff(f, g) -> (npixels, bbox|None); screenshot.change_fraction(f, g);
          screenshot.moved_rows(f, g); f.write_png('a.png')
(comparing two frames or directories: screenshot_diff.py)

`ignore` arguments are lists of rectangles (x0, y0, x1, y1), end exclusive, whose pixels are never
compared. To compare only a watched area, pass `unwatched(watch, w, h)`, the rectangles outside it.
"""

import struct
import zlib
from collections.abc import Sequence
from itertools import pairwise
from pathlib import Path

PIXEL_BYTES = 3
BYTE_MASK = 255
HDR_FIELDS = 4
NEWLINE = 0x0A
FILTER_SUB, FILTER_UP, FILTER_AVG, FILTER_PAETH = range(1, 5)
RGB8_NONINTERLACED = (8, 2, 0)
CHUNK_HEADER = 8
CHUNK_OVERHEAD = 12
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
PNG_MAGIC_LEN = 8
# Highlighted-row geometry of the list screens: row 0 starts at y=21 and the pitch is 24.5 px
# (21, 46, 71, 96, 121, 145, 169, 193, 217).
ROW_PITCH = 24.5

Rectangle = tuple[int, int, int, int]
Ignore = Sequence[Rectangle]


class Frame:
    """A W x H RGB frame; `px` is the packed RGB bytes (3 per pixel, row major).

    Comparisons are the module functions below (lit, same, diff, change_fraction, moved_rows).
    """

    def __init__(self, w: int, h: int, px: bytes) -> None:
        """Build a frame of `w` x `h` pixels from the packed RGB bytes `px`."""
        self.w, self.h, self.px = w, h, bytes(px)

    def write_png(self, path: str | Path) -> None:
        """Write the frame as an 8-bit RGB PNG."""
        write_png(path, self.w, self.h, self.px)

    def write_ppm(self, path: str | Path) -> None:
        """Write the frame as a binary P6 PPM (what the monitor's screenshot produces)."""
        with Path(path).open("wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (self.w, self.h) + self.px)


def lit(f: Frame) -> int:
    """Count the non-black pixels: OR the three channels as big ints, count zero bytes."""
    n = f.w * f.h
    o = (
        int.from_bytes(f.px[0::3], "big")
        | int.from_bytes(f.px[1::3], "big")
        | int.from_bytes(f.px[2::3], "big")
    )
    return n - o.to_bytes(n, "big").count(0)


def same(a: Frame, b: Frame, ignore: Ignore = ()) -> bool:
    """Return whether two frames are pixel-identical, optionally ignoring rectangles.

    A fast path is one bytes compare; `ignore` holds (x0, y0, x1, y1) rectangles (end exclusive),
    for example a clock or a marquee.
    """
    if (a.w, a.h) != (b.w, b.h):
        return False
    if a.px == b.px:
        return True
    return bool(ignore) and not _differing(a, b, ignore)


def _differing(a: Frame, b: Frame, ignore: Ignore) -> list[tuple[int, int]]:
    """Return the (y, x) of differing pixels outside `ignore`, row by row.

    Only rows whose bytes differ are inspected (the rest are skipped with one slice compare).
    """
    w, out = a.w, []
    for y in range(a.h):
        ra, rb = a.px[y * w * 3 : (y + 1) * w * 3], b.px[y * w * 3 : (y + 1) * w * 3]
        if ra == rb:
            continue
        out.extend(
            (y, x)
            for x in range(w)
            if ra[x * 3 : x * 3 + 3] != rb[x * 3 : x * 3 + 3]
            and not any(x0 <= x < x1 and y0 <= y < y1 for x0, y0, x1, y1 in ignore)
        )
    return out


def diff(a: Frame, b: Frame, ignore: Ignore = ()) -> tuple[int, Rectangle | None]:
    """Return (number of differing pixels, (x0, y0, x1, y1) bbox or None).

    A size mismatch is (2**30, None). Bytes are compared first; per-pixel work only on mismatch.
    """
    if (a.w, a.h) != (b.w, b.h):
        return 1 << 30, None
    if a.px == b.px:
        return 0, None
    d = _differing(a, b, ignore)
    if not d:
        return 0, None
    ys, xs = [p[0] for p in d], [p[1] for p in d]
    return len(d), (min(xs), min(ys), max(xs) + 1, max(ys) + 1)


def _compared_area(a: Frame, ignore: Ignore) -> int:
    """Return the pixels of frame `a` outside the `ignore` rectangles (overlaps counted once)."""
    covered = 0
    for y in range(a.h):
        spans = sorted((max(0, x0), min(a.w, x1)) for x0, y0, x1, y1 in ignore if y0 <= y < y1)
        edge = 0
        for start, end in spans:
            covered += max(0, end - max(start, edge))
            edge = max(edge, end)
    return a.w * a.h - covered


def unwatched(watch: Sequence[Rectangle], width: int, height: int) -> tuple[Rectangle, ...]:
    """Return the rectangles outside all of the `watch` rectangles.

    The result is the `ignore` argument that compares only the watched area of a width x height
    frame.
    """
    xs = sorted({0, width, *(min(max(x, 0), width) for x0, _, x1, _ in watch for x in (x0, x1))})
    ys = sorted({0, height, *(min(max(y, 0), height) for _, y0, _, y1 in watch for y in (y0, y1))})
    return tuple(
        (xa, ya, xb, yb)
        for xa, xb in pairwise(xs)
        for ya, yb in pairwise(ys)
        if not any(x0 <= xa and xb <= x1 and y0 <= ya and yb <= y1 for x0, y0, x1, y1 in watch)
    )


def change_fraction(a: Frame, b: Frame, ignore: Ignore = ()) -> float:
    """Return the share (0..1) of compared pixels that differ.

    A page change moves most of what is compared; a setting toggled in place only a small region.
    """
    count, _ = diff(a, b, ignore)
    return min(1.0, count / max(1, _compared_area(a, ignore)))


def moved_rows(a: Frame, b: Frame, ignore: Ignore = ()) -> int:
    """Return how many list rows the highlight moved between two frames of one list.

    A step from row r to r+k repaints rows r..r+k, so (k+1) row pitches; 0 when nothing differs.
    Meaningful only while the highlight stays on a visible row; pass `ignore` rectangles that leave
    only the list pane, since the right pane can repaint with the selection.
    """
    _, box = diff(a, b, ignore)
    if box is None:
        return 0
    return max(0, round((box[3] - box[1]) / ROW_PITCH) - 1)


def parse_ppm(data: bytes) -> Frame:
    """Parse P6 bytes into a Frame.

    Header fields are whitespace separated and may carry # comments. Raises ValueError on a non-P6
    or truncated file (a half-written screenshot).
    """
    fields, i = [], 0
    while len(fields) < HDR_FIELDS:
        while i < len(data) and data[i : i + 1].isspace():
            i += 1
        if i >= len(data):
            msg = "truncated PPM header"
            raise ValueError(msg)
        if data[i : i + 1] == b"#":
            while i < len(data) and data[i] != NEWLINE:
                i += 1
            continue
        j = i
        while j < len(data) and not data[j : j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    if fields[0] != b"P6":
        msg = f"not a binary PPM: {fields[0]!r}"
        raise ValueError(msg)
    w, h = int(fields[1]), int(fields[2])
    px = data[i + 1 : i + 1 + w * h * PIXEL_BYTES]
    if len(px) != w * h * PIXEL_BYTES:
        msg = f"truncated PPM: {len(px)} of {w * h * PIXEL_BYTES} pixel bytes"
        raise ValueError(msg)
    return Frame(w, h, px)


def _unfilter(kind: int, row: bytearray, prev: bytearray) -> None:
    """Apply PNG filter `kind` to `row` in place, given the previous output row `prev`."""
    for x in range(len(row)):
        a = row[x - PIXEL_BYTES] if x >= PIXEL_BYTES else 0
        b, c = prev[x], (prev[x - PIXEL_BYTES] if x >= PIXEL_BYTES else 0)
        if kind == FILTER_SUB:
            row[x] = (row[x] + a) & BYTE_MASK
        elif kind == FILTER_UP:
            row[x] = (row[x] + b) & BYTE_MASK
        elif kind == FILTER_AVG:
            row[x] = (row[x] + (a + b) // 2) & BYTE_MASK
        elif kind == FILTER_PAETH:
            p = a + b - c
            pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            row[x] = (row[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & BYTE_MASK


def parse_png(data: bytes) -> Frame:
    """Parse PNG bytes into a Frame; only 8-bit RGB, non-interlaced (what write_png produces)."""
    if data[:PNG_MAGIC_LEN] != PNG_MAGIC:
        msg = "not a PNG"
        raise ValueError(msg)
    i, idat, w, h = PNG_MAGIC_LEN, b"", 0, 0
    while i < len(data):
        n, tag = struct.unpack(">I4s", data[i : i + CHUNK_HEADER])
        body = data[i + CHUNK_HEADER : i + CHUNK_HEADER + n]
        if tag == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if (depth, ctype, interlace) != RGB8_NONINTERLACED:
                msg = "unsupported PNG (need 8-bit RGB, not interlaced)"
                raise ValueError(msg)
        elif tag == b"IDAT":
            idat += body
        i += CHUNK_OVERHEAD + n
    raw, stride = zlib.decompress(idat), w * PIXEL_BYTES
    out, prev = bytearray(), bytearray(stride)
    for y in range(h):
        kind = raw[y * (stride + 1)]
        row = bytearray(raw[y * (stride + 1) + 1 : (y + 1) * (stride + 1)])
        _unfilter(kind, row, prev)
        out += row
        prev = row
    return Frame(w, h, bytes(out))


def read(path: str | Path) -> Frame:
    """Read a .ppm or .png file into a Frame."""
    data = Path(path).read_bytes()
    return parse_png(data) if data[:4] == b"\x89PNG" else parse_ppm(data)


def _chunk(tag: bytes, payload: bytes) -> bytes:
    """Return one PNG chunk: length, tag, payload and CRC."""
    body = tag + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def write_png(path: str | Path, w: int, h: int, rgb: bytes) -> None:
    """Write packed RGB bytes `rgb` of a `w` x `h` image to `path` as a PNG."""
    # Filter 0 on every scanline; level 1 is ~5x faster than 9 for a few % size on these UI frames.
    raw = b"".join(b"\x00" + rgb[y * w * 3 : (y + 1) * w * 3] for y in range(h))
    with Path(path).open("wb") as f:
        f.write(
            PNG_MAGIC
            + _chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + _chunk(b"IDAT", zlib.compress(raw, 1))
            + _chunk(b"IEND", b"")
        )

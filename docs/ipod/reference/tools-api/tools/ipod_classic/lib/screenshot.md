# Screenshot

[tools Index](../../../README.md#tools-index) / [Tools](../../index.md#tools) / [Ipod Classic](../index.md#ipod-classic) / [Lib](./index.md#lib) / Screenshot

> Auto-generated documentation for [tools.ipod_classic.lib.screenshot](../../../../../../../tools/ipod_classic/lib/screenshot.py) module.

```text
screenshot.py -- the one place that reads, compares and writes screenshots (stdlib only).

Library:  f = screenshot.read('a.ppm'); screenshot.lit(f);
          screenshot.same(f, g, ignore=[(x0,y0,x1,y1)]);
          screenshot.diff(f, g) -> (npixels, bbox|None); screenshot.change_fraction(f, g);
          screenshot.moved_rows(f, g); f.write_png('a.png')
(comparing two frames or directories: screenshot_diff.py)

`ignore` arguments are lists of rectangles (x0, y0, x1, y1), end exclusive, whose pixels are never
compared. To compare only a watched area, pass `unwatched(watch, w, h)`, the rectangles outside it.
```

#### Attributes

- `ROW_PITCH` - Highlighted-row geometry of the list screens: row 0 starts at y=21 and the pitch is 24.5 px (21, 46, 71, 96, 121, 145, 169, 193, 217).: 24.5

- [Screenshot](#screenshot)

  - [Frame](#frame)
    - [Frame().write_png](<#frame()write_png>)
    - [Frame().write_ppm](<#frame()write_ppm>)
  - [\_chunk](#_chunk)
  - [\_compared_area](#_compared_area)
  - [\_differing](#_differing)
  - [\_unfilter](#_unfilter)
  - [change_fraction](#change_fraction)
  - [diff](#diff)
  - [lit](#lit)
  - [moved_rows](#moved_rows)
  - [parse_png](#parse_png)
  - [parse_ppm](#parse_ppm)
  - [read](#read)
  - [same](#same)
  - [unwatched](#unwatched)
  - [write_png](#write_png)

## Frame

[Show source in screenshot.py:37](../../../../../../../tools/ipod_classic/lib/screenshot.py#L37)

A W x H RGB frame; `px` is the packed RGB bytes (3 per pixel, row major).

Comparisons are the module functions below (lit, same, diff, change_fraction, moved_rows).

#### Signature

```python
class Frame:
    def __init__(self, w: int, h: int, px: bytes) -> None: ...
```

### Frame().write_png

[Show source in screenshot.py:47](../../../../../../../tools/ipod_classic/lib/screenshot.py#L47)

Write the frame as an 8-bit RGB PNG.

#### Signature

```python
def write_png(self, path: str | Path) -> None: ...
```

### Frame().write_ppm

[Show source in screenshot.py:51](../../../../../../../tools/ipod_classic/lib/screenshot.py#L51)

Write the frame as a binary P6 PPM (what the monitor's screenshot produces).

#### Signature

```python
def write_ppm(self, path: str | Path) -> None: ...
```

## \_chunk

[Show source in screenshot.py:250](../../../../../../../tools/ipod_classic/lib/screenshot.py#L250)

Return one PNG chunk: length, tag, payload and CRC.

#### Signature

```python
def _chunk(tag: bytes, payload: bytes) -> bytes: ...
```

## \_compared_area

[Show source in screenshot.py:116](../../../../../../../tools/ipod_classic/lib/screenshot.py#L116)

Return the pixels of frame `a` outside the `ignore` rectangles (overlaps counted once).

#### Signature

```python
def _compared_area(a: Frame, ignore: Ignore) -> int: ...
```

#### See also

- [Frame](#frame)
- [Ignore](#ignore)

## \_differing

[Show source in screenshot.py:81](../../../../../../../tools/ipod_classic/lib/screenshot.py#L81)

Return the (y, x) of differing pixels outside `ignore`, row by row.

Only rows whose bytes differ are inspected (the rest are skipped with one slice compare).

#### Signature

```python
def _differing(a: Frame, b: Frame, ignore: Ignore) -> list[tuple[int, int]]: ...
```

#### See also

- [Frame](#frame)
- [Ignore](#ignore)

## \_unfilter

[Show source in screenshot.py:199](../../../../../../../tools/ipod_classic/lib/screenshot.py#L199)

Apply PNG filter `kind` to `row` in place, given the previous output row `prev`.

#### Signature

```python
def _unfilter(kind: int, row: bytearray, prev: bytearray) -> None: ...
```

## change_fraction

[Show source in screenshot.py:144](../../../../../../../tools/ipod_classic/lib/screenshot.py#L144)

Return the share (0..1) of compared pixels that differ.

A page change moves most of what is compared; a setting toggled in place only a small region.

#### Signature

```python
def change_fraction(a: Frame, b: Frame, ignore: Ignore = ()) -> float: ...
```

#### See also

- [Frame](#frame)
- [Ignore](#ignore)

## diff

[Show source in screenshot.py:100](../../../../../../../tools/ipod_classic/lib/screenshot.py#L100)

Return (number of differing pixels, (x0, y0, x1, y1) bbox or None).

A size mismatch is (2\*\*30, None). Bytes are compared first; per-pixel work only on mismatch.

#### Signature

```python
def diff(a: Frame, b: Frame, ignore: Ignore = ()) -> tuple[int, Rectangle | None]: ...
```

#### See also

- [Frame](#frame)
- [Ignore](#ignore)

## lit

[Show source in screenshot.py:57](../../../../../../../tools/ipod_classic/lib/screenshot.py#L57)

Count the non-black pixels: OR the three channels as big ints, count zero bytes.

#### Signature

```python
def lit(f: Frame) -> int: ...
```

#### See also

- [Frame](#frame)

## moved_rows

[Show source in screenshot.py:153](../../../../../../../tools/ipod_classic/lib/screenshot.py#L153)

Return how many list rows the highlight moved between two frames of one list.

A step from row r to r+k repaints rows r..r+k, so (k+1) row pitches; 0 when nothing differs. Meaningful only while the highlight stays on a visible row; pass `ignore` rectangles that leave only the list pane, since the right pane can repaint with the selection.

#### Signature

```python
def moved_rows(a: Frame, b: Frame, ignore: Ignore = ()) -> int: ...
```

#### See also

- [Frame](#frame)
- [Ignore](#ignore)

## parse_png

[Show source in screenshot.py:216](../../../../../../../tools/ipod_classic/lib/screenshot.py#L216)

Parse PNG bytes into a Frame; only 8-bit RGB, non-interlaced (what write_png produces).

#### Signature

```python
def parse_png(data: bytes) -> Frame: ...
```

#### See also

- [Frame](#frame)

## parse_ppm

[Show source in screenshot.py:166](../../../../../../../tools/ipod_classic/lib/screenshot.py#L166)

Parse P6 bytes into a Frame.

Header fields are whitespace separated and may carry # comments. Raises ValueError on a non-P6 or truncated file (a half-written screenshot).

#### Signature

```python
def parse_ppm(data: bytes) -> Frame: ...
```

#### See also

- [Frame](#frame)

## read

[Show source in screenshot.py:244](../../../../../../../tools/ipod_classic/lib/screenshot.py#L244)

Read a .ppm or .png file into a Frame.

#### Signature

```python
def read(path: str | Path) -> Frame: ...
```

#### See also

- [Frame](#frame)

## same

[Show source in screenshot.py:68](../../../../../../../tools/ipod_classic/lib/screenshot.py#L68)

Return whether two frames are pixel-identical, optionally ignoring rectangles.

A fast path is one bytes compare; `ignore` holds (x0, y0, x1, y1) rectangles (end exclusive), for example a clock or a marquee.

#### Signature

```python
def same(a: Frame, b: Frame, ignore: Ignore = ()) -> bool: ...
```

#### See also

- [Frame](#frame)
- [Ignore](#ignore)

## unwatched

[Show source in screenshot.py:128](../../../../../../../tools/ipod_classic/lib/screenshot.py#L128)

Return the rectangles outside all of the `watch` rectangles.

The result is the `ignore` argument that compares only the watched area of a width x height frame.

#### Signature

```python
def unwatched(
    watch: Sequence[Rectangle], width: int, height: int
) -> tuple[Rectangle, ...]: ...
```

#### See also

- [Rectangle](#rectangle)

## write_png

[Show source in screenshot.py:256](../../../../../../../tools/ipod_classic/lib/screenshot.py#L256)

Write packed RGB bytes `rgb` of a `w` x `h` image to `path` as a PNG.

#### Signature

```python
def write_png(path: str | Path, w: int, h: int, rgb: bytes) -> None: ...
```

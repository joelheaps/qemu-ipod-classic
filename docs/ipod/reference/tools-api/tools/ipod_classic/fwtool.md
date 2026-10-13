# Fwtool

[tools Index](../../README.md#tools-index) / [Tools](../index.md#tools) / [Ipod Classic](./index.md#ipod-classic) / Fwtool

> Auto-generated documentation for [tools.ipod_classic.fwtool](../../../../../../tools/ipod_classic/fwtool.py) module.

```text
Parse and extract iPod Classic 6G (S5L8702) firmware images.

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
```

#### Attributes

- `DIRENT_FMT` - Directory entry, 40 bytes, little endian. Field names follow ipodpatcher.: '\<4s4s8I'

- `IMG_HDR_SIZE` - S5L870x image header. The header struct itself is small but the whole area is padded out to 0x800 bytes; the payload starts right after. All size/offset fields inside the header are relative to the start of the payload, not to the start of the file.: 2048

- `DIR_LEN_SLACK` - The directory records the image size minus the 0x1000 that the header padding and alignment add on top of the payload.: 4096

- [Fwtool](#fwtool)

  - [DirEntry](#direntry)
    - [DirEntry().__str__](<#direntry()__str__>)
  - [ImageHeader](#imageheader)
    - [ImageHeader().__str__](<#imageheader()__str__>)
    - [ImageHeader().check](<#imageheader()check>)
    - [ImageHeader().encrypted](<#imageheader()encrypted>)
    - [ImageHeader().total_size](<#imageheader()total_size>)
  - [\_find_image_base](#_find_image_base)
  - [\_fourcc](#_fourcc)
  - [\_is_fourcc](#_is_fourcc)
  - [\_looks_like_dirent](#_looks_like_dirent)
  - [cmd_extract](#cmd_extract)
  - [cmd_list](#cmd_list)
  - [cmd_osos](#cmd_osos)
  - [decode_version](#decode_version)
  - [entropy](#entropy)
  - [main](#main)
  - [read_directory](#read_directory)
  - [read_firmware](#read_firmware)

## DirEntry

[Show source in fwtool.py:73](../../../../../../tools/ipod_classic/fwtool.py#L73)

One 40-byte entry of the firmware partition's image directory.

#### Signature

```python
class DirEntry:
    def __init__(self, raw: bytes, index: int, base: int = 0) -> None: ...
```

### DirEntry().__str__

[Show source in fwtool.py:100](../../../../../../tools/ipod_classic/fwtool.py#L100)

Format the entry as one line for `list`.

#### Signature

```python
def __str__(self) -> str: ...
```

## ImageHeader

[Show source in fwtool.py:110](../../../../../../tools/ipod_classic/fwtool.py#L110)

The "8702" header in front of each image payload.

Layout, verified against the osos and aupd images of iPod_24.1.1.2.ipsw:

0x00 char[4] "8702" 0x04 char[3] "1.0" 0x07 uint8 format / encryption type (0x03 on retail images) 0x08 uint32 unknown, zero 0x0c uint32 payload size, unpadded 0x10 uint32 footer_offset + footer_size 0x14 uint32 footer offset, relative to the payload start 0x18 uint32 footer size 0x40 byte[20] SHA-1 ... zero padding up to 0x800

#### Signature

```python
class ImageHeader:
    def __init__(self, raw: bytes) -> None: ...
```

### ImageHeader().__str__

[Show source in fwtool.py:151](../../../../../../tools/ipod_classic/fwtool.py#L151)

Format the header as one line for `list`.

#### Signature

```python
def __str__(self) -> str: ...
```

### ImageHeader().check

[Show source in fwtool.py:159](../../../../../../tools/ipod_classic/fwtool.py#L159)

Return a list of consistency problems, empty if the header adds up.

#### Signature

```python
def check(self) -> list[str]: ...
```

### ImageHeader().encrypted

[Show source in fwtool.py:141](../../../../../../tools/ipod_classic/fwtool.py#L141)

Whether the format byte is the one Apple ships (a plain variant would differ).

#### Signature

```python
@property
def encrypted(self) -> bool: ...
```

### ImageHeader().total_size

[Show source in fwtool.py:146](../../../../../../tools/ipod_classic/fwtool.py#L146)

Size of the whole image on disk, header included.

#### Signature

```python
@property
def total_size(self) -> int: ...
```

## \_find_image_base

[Show source in fwtool.py:185](../../../../../../tools/ipod_classic/fwtool.py#L185)

Work out what the header's directory offset is relative to.

The offset in the header is not relative to the start of the buffer, and what it *is* relative to varies: a Firmware-XX payload pulled out of an .ipsw puts the directory 0x1000 further along, while the firmware partition read off an iPod Video has it 0x200 further along. Rather than keep a table of fudge factors, try the known ones and then fall back to scanning for the first thing that parses as a directory entry.

#### Signature

```python
def _find_image_base(data: bytes, dir_offset: int) -> int: ...
```

## \_fourcc

[Show source in fwtool.py:63](../../../../../../tools/ipod_classic/fwtool.py#L63)

Decode a reversed four-character code, showing junk as dots.

#### Signature

```python
def _fourcc(raw: bytes) -> str: ...
```

## \_is_fourcc

[Show source in fwtool.py:68](../../../../../../tools/ipod_classic/fwtool.py#L68)

Return True if all four bytes are printable, i.e. this looks like a real tag.

#### Signature

```python
def _is_fourcc(raw: bytes) -> bool: ...
```

## \_looks_like_dirent

[Show source in fwtool.py:174](../../../../../../tools/ipod_classic/fwtool.py#L174)

Cheap validity test for a directory entry at a given offset.

#### Signature

```python
def _looks_like_dirent(data: bytes, off: int) -> bool: ...
```

## cmd_extract

[Show source in fwtool.py:317](../../../../../../tools/ipod_classic/fwtool.py#L317)

Write each image (and, for 8702 images, its payload and footer) to `outdir`.

#### Signature

```python
def cmd_extract(data: bytes, outdir: Path) -> None: ...
```

## cmd_list

[Show source in fwtool.py:291](../../../../../../tools/ipod_classic/fwtool.py#L291)

Print the directory of a firmware partition and each image's header.

#### Signature

```python
def cmd_list(path: Path, data: bytes) -> None: ...
```

## cmd_osos

[Show source in fwtool.py:344](../../../../../../tools/ipod_classic/fwtool.py#L344)

Write osos as the device decrypts it: 0x800-byte header first, padded to the AES block.

#### Signature

```python
def cmd_osos(data: bytes, out: Path) -> None: ...
```

## decode_version

[Show source in fwtool.py:248](../../../../../../tools/ipod_classic/fwtool.py#L248)

Decode a manifest VisibleBuildID, a Mac NumVersion: 0x02058000 is "2.0.5".

Bytes, high to low: major (BCD), minor and bug nibbles (BCD), stage (0x80 = final), pre-release revision. Anything but a final release is rejected, not guessed at.

#### Signature

```python
def decode_version(build_id: object) -> str: ...
```

## entropy

[Show source in fwtool.py:237](../../../../../../tools/ipod_classic/fwtool.py#L237)

Return the Shannon entropy of `buf` in bits per byte.

#### Signature

```python
def entropy(buf: bytes) -> float: ...
```

## main

[Show source in fwtool.py:363](../../../../../../tools/ipod_classic/fwtool.py#L363)

Run the command-line interface.

#### Signature

```python
def main(argv: list[str]) -> None: ...
```

## read_directory

[Show source in fwtool.py:205](../../../../../../tools/ipod_classic/fwtool.py#L205)

Locate and parse the image directory.

Returns (directory offset, image base, entries).

#### Signature

```python
def read_directory(data: bytes) -> tuple[int, int, list[DirEntry]]: ...
```

#### See also

- [DirEntry](#direntry)

## read_firmware

[Show source in fwtool.py:268](../../../../../../tools/ipod_classic/fwtool.py#L268)

Return the Firmware-XX payload of an .ipsw or a bare payload, and the .ipsw's version.

The payload must carry the \]ih\[ magic. A bare payload has no version (None).

#### Signature

```python
def read_firmware(path: Path) -> tuple[bytes, str | None]: ...
```

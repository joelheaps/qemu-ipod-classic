# Make Disk

[tools Index](../../README.md#tools-index) / [Tools](../index.md#tools) / [Ipod Classic](./index.md#ipod-classic) / Make Disk

> Auto-generated documentation for [tools.ipod_classic.make_disk](../../../../../../tools/ipod_classic/make_disk.py) module.

```text
Create an empty bootable iPod classic disk as qcow2; the logic behind scripts/ipod/make-disk.

Layout (what the retailOS expects): an MBR in 4096-byte units, the firmware partition (a
Firmware-XX payload) at block 8 (type 0x00), then an empty 1 GiB FAT32 (type 0x0b). The firmware
partition carries the fonts the UI needs; without it a boot reset-loops. Flags:
README.md. Assumes it runs where mkfs.fat and BUILD_DIR/qemu-img are available (the
docker:ipod:make-disk wrapper runs the same task in the dev container).
```

- [Make Disk](#make-disk)
  - [build_image](#build_image)
  - [main](#main)
  - [parse_args](#parse_args)
  - [prepare](#prepare)

## build_image

[Show source in make_disk.py:31](../../../../../../tools/ipod_classic/make_disk.py#L31)

Write the disk for PAYLOAD to OUT as qcow2 (needs mkfs.fat and BUILD_DIR/qemu-img).

With TRACK the volume holds a one-track library (needs mtools, ffprobe and libgpod too).

#### Signature

```python
def build_image(payload: bytes, out: Path, track: Path | None = None) -> None: ...
```

## main

[Show source in make_disk.py:108](../../../../../../tools/ipod_classic/make_disk.py#L108)

Run make-disk, assuming mkfs.fat and BUILD_DIR/qemu-img are available.

#### Signature

```python
def main(argv: list[str]) -> int: ...
```

## parse_args

[Show source in make_disk.py:71](../../../../../../tools/ipod_classic/make_disk.py#L71)

Parse the flags (FIRMWARE and DISK are the environment defaults).

#### Signature

```python
def parse_args(argv: list[str]) -> dict[str, str]: ...
```

## prepare

[Show source in make_disk.py:76](../../../../../../tools/ipod_classic/make_disk.py#L76)

Resolve and validate the firmware/disks flags; return None when there is nothing to build.

#### Signature

```python
def prepare(
    argv: list[str],
) -> tuple[Path, Path, bytes, str | None, Path | None] | None: ...
```

# Ipod Library

[tools Index](../../../README.md#tools-index) / [Tools](../../index.md#tools) / [Ipod Classic](../index.md#ipod-classic) / [Lib](./index.md#lib) / Ipod Library

> Auto-generated documentation for [tools.ipod_classic.lib.ipod_library](../../../../../../../tools/ipod_classic/lib/ipod_library.py) module.

```text
Put a one-track music library on a FAT image: the logic behind `make-disk --track`.

The retailOS lists a track only if iTunesDB carries libgpod's checksum, so the database is
rewritten through libgpod (which signs it with the FireWire GUID in Device/SysInfo) instead of
being patched by hand. The starting point is sample_library/ipod_control, a library written by
libgpod with one track (portblue.m4a, the vetted AAC song); the requested track replaces that
file and the fields libgpod keeps for it (title, path, size, length, bitrate). Needs mtools,
ffprobe and libgpod.so.4; the qemu-ipod-dev container has them.
```

#### Attributes

- `FIXTURE_VALUES` - What those offsets hold for the fixture track; checked before writing, so a different libgpod fails loudly.: {SIZE: 3425742, TRACKLEN: 106138, BITRATE: 257}

- [Ipod Library](#ipod-library)

  - [populate](#populate)
  - [probe](#probe)
  - [ptr](#ptr)
  - [rewrite_database](#rewrite_database)

## populate

[Show source in ipod_library.py:104](../../../../../../../tools/ipod_classic/lib/ipod_library.py#L104)

Copy the library with TRACK into the FAT image FAT (a freshly formatted, empty volume).

#### Signature

```python
def populate(fat: Path, track: Path = PORT_BLUE) -> None: ...
```

#### See also

- [PORT_BLUE](#port_blue)

## probe

[Show source in ipod_library.py:38](../../../../../../../tools/ipod_classic/lib/ipod_library.py#L38)

Return the track's duration (ms) and bitrate (kbps), from ffprobe.

#### Signature

```python
def probe(track: Path) -> dict[str, int]: ...
```

## ptr

[Show source in ipod_library.py:33](../../../../../../../tools/ipod_classic/lib/ipod_library.py#L33)

Read the pointer stored at ADDR (0 for NULL).

#### Signature

```python
def ptr(addr: int) -> int: ...
```

## rewrite_database

[Show source in ipod_library.py:56](../../../../../../../tools/ipod_classic/lib/ipod_library.py#L56)

Point the fixture database under ROOT (holding iPod_Control) at TRACK and re-sign it.

#### Signature

```python
def rewrite_database(root: Path, track: Path) -> None: ...
```

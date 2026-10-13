# Emcore S5l8702

[tools Index](../../README.md#tools-index) / [Tools](../index.md#tools) / [Ipod Classic](./index.md#ipod-classic) / Emcore S5l8702

> Auto-generated documentation for [tools.ipod_classic.emcore_s5l8702](../../../../../../tools/ipod_classic/emcore_s5l8702.py) module.

```text
Client for the emCORE USB debugger on an iPod Classic 6G (Samsung S5L8702).

Speaks the *pre-r891* monitor protocol.

Why this file exists
--------------------
The official client (freemyipod SVN, mirrored at github.com/freemyipod/freemyipod)
does not work against a stock device out of the box:

  * The HEAD client only binds interfaces with bInterfaceSubClass/Protocol == 0/0
    and drives the monitor over EP0 control transfers.  That protocol landed in
    SVN r891.  The last *released* emCORE is r859, whose interface is ff/ff/ff
    with four bulk endpoints.  HEAD therefore refuses to enumerate the device.
  * The r859 client calls pyusb as ``dev.write(ep, data, interface, timeout)``.
    The ``interface`` parameter was removed in pyusb 1.0.0b1, so on any modern
    pyusb it raises TypeError.
  * ``misc.py`` uses ``inspect.getargspec``, removed in Python 3.11.

This implementation talks the r859 wire protocol directly and has no dependency
on the emCORE source tree.  Verified against a real device running
emCORE Debugger v0.2.3 r859 (see docs/ipod/classic6g-firmware.md).

SAFETY
------
Only read-oriented commands are implemented.  Memory *writes* exist because the
hardware AES engine works in place on a RAM buffer, but they are constrained to
the device's malloc pool (DRAM).  The commands that mutate persistent state or
hand over control -- 23 bootflash write, 21 execimage, 24 runfirmware, and every
storage/NAND command -- are deliberately NOT implemented.  Command 22
``bootflashread`` IS implemented and is a pure read: it issues SPI opcode 3
(READ DATA), which cannot modify the flash.

Requires: pyusb (Debian/Ubuntu: python3-usb).  Needs write access to the USB
device node -- either run as root or install a udev rule:
    SUBSYSTEM=="usb", ATTR{idVendor}=="ffff", ATTR{idProduct}=="e000", MODE="0666"
```

#### Attributes

- `CMD_GETINFO` - --- monitor command ids (subset; read-oriented only) -----------------------: 1

- `KEY_GID` - hwkeyaes key indices, from Rockbox firmware/target/arm/s5l8702/crypto-s5l8702.h: 1

- [Emcore S5l8702](#emcore-s5l8702)

  - [AesDevice](#aesdevice)
    - [AesDevice().aes](<#aesdevice()aes>)
    - [AesDevice().free](<#aesdevice()free>)
    - [AesDevice().malloc](<#aesdevice()malloc>)
    - [AesDevice().read](<#aesdevice()read>)
    - [AesDevice().write](<#aesdevice()write>)
  - [Emcore](#emcore)
    - [Emcore.\_alignsplit](#emcore_alignsplit)
    - [Emcore().\_cmd](<#emcore()_cmd>)
    - [Emcore().aes](<#emcore()aes>)
    - [Emcore().bootflashread](<#emcore()bootflashread>)
    - [Emcore.decode_packetsize](#emcoredecode_packetsize)
    - [Emcore.decode_pool](#emcoredecode_pool)
    - [Emcore.decode_version](#emcoredecode_version)
    - [Emcore().describe](<#emcore()describe>)
    - [Emcore().free](<#emcore()free>)
    - [Emcore().malloc](<#emcore()malloc>)
    - [Emcore().read](<#emcore()read>)
    - [Emcore().write](<#emcore()write>)
  - [EmcoreError](#emcoreerror)
  - [\_int](#_int)
  - [decrypt_payload](#decrypt_payload)
  - [main](#main)

## AesDevice

[Show source in emcore_s5l8702.py:89](../../../../../../tools/ipod_classic/emcore_s5l8702.py#L89)

The part of [Emcore](#emcore) that [decrypt_payload](#decrypt_payload) needs (lets tests substitute a fake).

#### Signature

```python
class AesDevice(Protocol): ...
```

### AesDevice().aes

[Show source in emcore_s5l8702.py:108](../../../../../../tools/ipod_classic/emcore_s5l8702.py#L108)

Run the AES engine in place.

#### Signature

```python
def aes(self, addr: int, size: int, keyidx: int, encrypt: bool = False) -> None: ...
```

### AesDevice().free

[Show source in emcore_s5l8702.py:96](../../../../../../tools/ipod_classic/emcore_s5l8702.py#L96)

Release device memory.

#### Signature

```python
def free(self, ptr: int) -> None: ...
```

### AesDevice().malloc

[Show source in emcore_s5l8702.py:92](../../../../../../tools/ipod_classic/emcore_s5l8702.py#L92)

Allocate device memory.

#### Signature

```python
def malloc(self, size: int) -> int: ...
```

### AesDevice().read

[Show source in emcore_s5l8702.py:104](../../../../../../tools/ipod_classic/emcore_s5l8702.py#L104)

Read bytes from device memory.

#### Signature

```python
def read(self, addr: int, size: int) -> bytes: ...
```

### AesDevice().write

[Show source in emcore_s5l8702.py:100](../../../../../../tools/ipod_classic/emcore_s5l8702.py#L100)

Write bytes to device memory.

#### Signature

```python
def write(self, addr: int, data: bytes) -> None: ...
```

## Emcore

[Show source in emcore_s5l8702.py:117](../../../../../../tools/ipod_classic/emcore_s5l8702.py#L117)

Minimal emCORE monitor client (pre-r891, four bulk endpoints).

#### Signature

```python
class Emcore:
    def __init__(self, timeout: int = 10000) -> None: ...
```

### Emcore.\_alignsplit

[Show source in emcore_s5l8702.py:222](../../../../../../tools/ipod_classic/emcore_s5l8702.py#L222)

Split a transfer into (head, body, tail) so the body is DMA-aligned.

Lifted from libemcore.Emcore.\_alignsplit (r859) -- the device's DMA path requires 16-byte alignment, the command-pipe path does not.

#### Signature

```python
@staticmethod
def _alignsplit(
    addr: int, size: int, blksize: int, align: int
) -> tuple[int, int, int]: ...
```

### Emcore().\_cmd

[Show source in emcore_s5l8702.py:150](../../../../../../tools/ipod_classic/emcore_s5l8702.py#L150)

Send a command packet, read back the 16-byte header + `extra` bytes.

#### Signature

```python
def _cmd(self, packet: bytes, extra: int = 0, timeout: int | None = None) -> bytes: ...
```

### Emcore().aes

[Show source in emcore_s5l8702.py:325](../../../../../../tools/ipod_classic/emcore_s5l8702.py#L325)

Run the S5L8702 AES engine in place over \[addr, addr+size).

AES-128-CBC. emCORE zeroes AESIV at the top of every hwkeyaes() call (emcore/target/ipodnano3g/hwkeyaes.c), so each invocation restarts CBC with IV = 0. `size` must be a multiple of 16.

#### Signature

```python
def aes(
    self, addr: int, size: int, keyidx: int, encrypt: bool = False, timeout: int = 600000
) -> None: ...
```

### Emcore().bootflashread

[Show source in emcore_s5l8702.py:347](../../../../../../tools/ipod_classic/emcore_s5l8702.py#L347)

Copy `size` bytes of SPI NOR into device RAM.

Issues SPI opcode 3 (READ DATA), so it cannot modify the flash.

#### Signature

```python
def bootflashread(
    self, memaddr: int, flashaddr: int, size: int, timeout: int = 60000
) -> None: ...
```

### Emcore.decode_packetsize

[Show source in emcore_s5l8702.py:187](../../../../../../tools/ipod_classic/emcore_s5l8702.py#L187)

Decode a GETINFO packet-size response (max bytes per endpoint).

#### Signature

```python
@staticmethod
def decode_packetsize(raw: bytes) -> dict[str, int]: ...
```

### Emcore.decode_pool

[Show source in emcore_s5l8702.py:193](../../../../../../tools/ipod_classic/emcore_s5l8702.py#L193)

Decode a GETINFO malloc-pool response into (lower, upper) addresses.

#### Signature

```python
@staticmethod
def decode_pool(raw: bytes) -> tuple[int, int]: ...
```

### Emcore.decode_version

[Show source in emcore_s5l8702.py:174](../../../../../../tools/ipod_classic/emcore_s5l8702.py#L174)

Decode a GETINFO version response into named fields.

#### Signature

```python
@staticmethod
def decode_version(raw: bytes) -> dict[str, int]: ...
```

### Emcore().describe

[Show source in emcore_s5l8702.py:208](../../../../../../tools/ipod_classic/emcore_s5l8702.py#L208)

Return a human-readable summary of the device's identity and limits.

#### Signature

```python
def describe(self) -> str: ...
```

### Emcore().free

[Show source in emcore_s5l8702.py:320](../../../../../../tools/ipod_classic/emcore_s5l8702.py#L320)

Release a device heap allocation made with [Emcore().malloc](#emcoremalloc).

#### Signature

```python
def free(self, ptr: int) -> None: ...
```

### Emcore().malloc

[Show source in emcore_s5l8702.py:311](../../../../../../tools/ipod_classic/emcore_s5l8702.py#L311)

Allocate `size` bytes on the device heap and return its address.

#### Signature

```python
def malloc(self, size: int) -> int: ...
```

### Emcore().read

[Show source in emcore_s5l8702.py:258](../../../../../../tools/ipod_classic/emcore_s5l8702.py#L258)

Read `size` bytes of device memory starting at `addr`.

#### Signature

```python
def read(self, addr: int, size: int) -> bytes: ...
```

### Emcore().write

[Show source in emcore_s5l8702.py:280](../../../../../../tools/ipod_classic/emcore_s5l8702.py#L280)

Write to device RAM. Refuses anything outside the malloc pool.

#### Signature

```python
def write(self, addr: int, data: bytes) -> None: ...
```

## EmcoreError

[Show source in emcore_s5l8702.py:113](../../../../../../tools/ipod_classic/emcore_s5l8702.py#L113)

Raised for any failure talking to the device.

#### Signature

```python
class EmcoreError(Exception): ...
```

## \_int

[Show source in emcore_s5l8702.py:410](../../../../../../tools/ipod_classic/emcore_s5l8702.py#L410)

Parse an integer argument in any base Python accepts (`0x` prefix etc.).

#### Signature

```python
def _int(s: str) -> int: ...
```

## decrypt_payload

[Show source in emcore_s5l8702.py:359](../../../../../../tools/ipod_classic/emcore_s5l8702.py#L359)

Decrypt an Apple firmware payload with the SoC's fused key.

The engine restarts CBC with IV = 0 on every call, so a chunked run returns a wrong *first* block per chunk and correct blocks thereafter. CBC is P[i] = D(C[i]) ^ C[i-1], hence the repair for the seam is simply

P[0]\_true = P[0]\_returned ^ (last ciphertext block of previous chunk)

Bytes past the last whole AES block are copied through untouched.

#### Signature

```python
def decrypt_payload(
    dev: AesDevice,
    ciphertext: bytes,
    keyidx: int = KEY_GID,
    chunk: int = 1048576,
    progress: ProgressFn | None = None,
) -> bytes: ...
```

#### See also

- [AesDevice](#aesdevice)
- [KEY_GID](#key_gid)

## main

[Show source in emcore_s5l8702.py:438](../../../../../../tools/ipod_classic/emcore_s5l8702.py#L438)

Run the command-line interface.

#### Signature

```python
def main() -> None: ...
```

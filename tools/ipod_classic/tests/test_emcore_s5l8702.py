# /// script
# requires-python = ">=3.12"
# dependencies = ["cryptography"]
# ///
"""Offline tests for emcore_s5l8702.py -- no device required.

The expected values are the ones observed on a real iPod Classic 6G (emCORE
Debugger v0.2.3 r859) on 2026-08-09; see docs/ipod/classic6g-firmware.md.
Keeping them pinned here means a refactor of the response decoders cannot
silently drift away from the real wire format.

Run:  uv run --script tools/ipod_classic/tests/test_emcore_s5l8702.py
"""

import os
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from emcore_s5l8702 import AES_BLOCK, KEY_GID, Emcore, decrypt_payload

try:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

    HAVE_CRYPTO = True
except ImportError:
    HAVE_CRYPTO = False

POOL = (0x08000000, 0x0BFC0000)
IV_ZERO = b"\0" * AES_BLOCK
STRAY = b"\xde\xad\xbe\xef"


class TestResponseDecoders(unittest.TestCase):
    """Pin the 16-byte monitor response layouts to observed hardware values."""

    def test_version(self) -> None:
        """Version response decodes to revision 859, v0.2.3, emCORE Debugger on an iPod Classic."""
        # status=OK, revision=859, 0.2.3, swtypeid=2, hwtypeid='IPCL'
        raw = struct.pack("<IIBBBBI", 1, 859, 0, 2, 3, 2, 0x4C435049)
        self.assertEqual(len(raw), AES_BLOCK)
        v = Emcore.decode_version(raw)
        self.assertEqual(v["revision"], 859)
        self.assertEqual((v["major"], v["minor"], v["patch"]), (0, 2, 3))
        self.assertEqual(v["swtypeid"], 2)  # 2 == "emCORE Debugger"
        self.assertEqual(v["hwtypeid"], 0x4C435049)
        # PLATFORM_ID 0x4c435049 is the little-endian packing of "IPCL"
        # (emcore/target/ipodclassic/target.h:28) -- iPod Classic
        self.assertEqual(struct.pack("<I", v["hwtypeid"]), b"IPCL")

    def test_packetsize(self) -> None:
        """Packet-size response decodes to 512-byte command and 256 KiB data endpoints."""
        raw = struct.pack("<IHHII", 1, 512, 512, 262144, 262144)
        self.assertEqual(len(raw), AES_BLOCK)
        self.assertEqual(
            Emcore.decode_packetsize(raw),
            {"cout": 512, "cin": 512, "dout": 262144, "din": 262144},
        )

    def test_pool(self) -> None:
        """Pool response decodes to the DRAM malloc range."""
        raw = struct.pack("<IIII", 1, *POOL, 0)
        self.assertEqual(Emcore.decode_pool(raw), POOL)


class TestAlignsplit(unittest.TestCase):
    """The three parts must tile the request exactly and keep the body aligned."""

    def test_partitions_exactly(self) -> None:
        """Head, body and tail sum to the request size, with a 16-byte-aligned body."""
        for addr in (0, 1, 7, 16, 0x08000000, 0x08000003, 0x0800000F):
            for size in (0, 1, 15, 16, 17, 495, 496, 497, 4096, 100000):
                head, body, tail = Emcore._alignsplit(addr, size, 496, AES_BLOCK)  # noqa: SLF001
                self.assertEqual(head + body + tail, size, f"addr=0x{addr:X} size={size}")
                self.assertGreaterEqual(min(head, body, tail), 0)
                if body:
                    self.assertEqual((addr + head) % AES_BLOCK, 0, "body must start aligned")
                    self.assertEqual(body % AES_BLOCK, 0, "body length must be a multiple of 16")

    def test_small_transfer_is_all_head(self) -> None:
        """A transfer that fits in one command packet needs no alignment."""
        self.assertEqual(Emcore._alignsplit(0x08000000, 100, 496, 16), (100, 0, 0))  # noqa: SLF001


class FakeAesDevice:
    """Stands in for the SoC, backed by software AES-128-CBC decrypt.

    Like emCORE's hwkeyaes(), it forces the IV to 0 on every call.
    """

    def __init__(self, key: bytes) -> None:
        """Remember the key; memory is a dict of buffers keyed by their start address."""
        self.key = key
        self.mem: dict[int, bytearray] = {}
        self.calls = 0

    def malloc(self, size: int) -> int:  # noqa: ARG002
        """Hand out the start of the pool."""
        return POOL[0]

    def free(self, ptr: int) -> None:
        """Do nothing: the fake has one buffer."""

    def write(self, addr: int, data: bytes) -> None:
        """Store `data` at `addr`."""
        self.mem[addr] = bytearray(data)

    def read(self, addr: int, size: int) -> bytes:
        """Return the first `size` bytes stored at `addr`."""
        return bytes(self.mem[addr][:size])

    def aes(self, addr: int, size: int, keyidx: int, *, encrypt: bool = False) -> None:  # noqa: ARG002
        """Decrypt `size` bytes at `addr` in place, restarting CBC with IV = 0."""
        self.calls += 1
        buf = bytes(self.mem[addr][:size])
        d = Cipher(algorithms.AES(self.key), modes.CBC(IV_ZERO)).decryptor()
        self.mem[addr][:size] = d.update(buf) + d.finalize()


def _encrypt(key: bytes, plain: bytes) -> bytes:
    enc = Cipher(algorithms.AES(key), modes.CBC(IV_ZERO)).encryptor()
    return enc.update(plain) + enc.finalize()


@unittest.skipUnless(HAVE_CRYPTO, "python3-cryptography not installed")
class TestChunkedCbcStitch(unittest.TestCase):
    """Chunked decryption + seam repair must equal a single contiguous decrypt.

    This is the property that makes it safe to stream a 9.9 MB firmware image
    through a 1 MiB device buffer.  It was also confirmed on real hardware.
    """

    def test_stitch_matches_contiguous(self) -> None:
        """Eight 1 KiB chunks decrypt to the same bytes as one pass."""
        key = os.urandom(AES_BLOCK)
        plain = os.urandom(8 * 1024)
        dev = FakeAesDevice(key)
        got = decrypt_payload(dev, _encrypt(key, plain), keyidx=KEY_GID, chunk=1024)
        self.assertGreater(dev.calls, 1, "test must actually exercise >1 chunk")
        self.assertEqual(got, plain)

    def test_unaligned_tail_passes_through(self) -> None:
        """Bytes after the last whole AES block are returned unchanged."""
        key = os.urandom(AES_BLOCK)
        ct = _encrypt(key, os.urandom(1024)) + STRAY
        got = decrypt_payload(FakeAesDevice(key), ct, keyidx=KEY_GID, chunk=512)
        self.assertEqual(len(got), len(ct))
        self.assertEqual(got[-len(STRAY) :], STRAY)


if __name__ == "__main__":
    unittest.main(verbosity=2)

# Assets

Assets contains various data blobs to help with tasks in this repo. Everything here except the READMEs is gitignored, so it is the suggested place for Apple files (`.ipsw`, `osos_decrypted.bin`) and disks built from them; no script depends on its layout.

| Directory | Suggested contents |
| -- | -- |
| `firmware/` | `.ipsw`, `Firmware-XX` payloads, `osos_decrypted.bin` |
| `disks/` | Disk images, e.g. from `mise run ipod:make-disk` (`assets/disks/README.md`) |
| `test/` | `iPod_38.2.0.5.ipsw` for the real-firmware test in `tools/ipod/tests/test_make_disk.py` (or set `IPOD_TEST_IPSW`) |

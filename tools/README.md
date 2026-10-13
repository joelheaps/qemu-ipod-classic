# Tools

Host-side tools for the iPod work. The mise tasks that use them are in `scripts/ipod/` and are documented in the main `README.md`; the full API, generated from the docstrings, is `docs/ipod/reference/tools-api/README.md` (`mise run docs:api`). `ebpf/` is upstream QEMU's virtio-net RSS helper (`docs/devel/ebpf_rss.rst`) and `meson.build` is its build file (`docs/devel/build-system.rst`); neither is iPod-specific.

## `ipod_classic/` — iPod classic 6G

Run with `uv run --script`; each file declares its own dependencies.

| Tool | Use |
| -- | -- |
| `fwtool.py` | Inspect and extract firmware: the `]ih[` partition and the per-image S5L870x headers (commands below) |
| `emcore_s5l8702.py` | Client for the emCORE USB debugger on a real device (used to decrypt firmware payloads on the device). Task: `docs/ipod/how-to/decrypt-classic-firmware.md` |
| `tests/test_emcore_s5l8702.py` | Offline tests for the client's response decoders; no device needed |
| `make_disk.py` | The logic of the `ipod:make-disk` task (`README.md`) |
| `tests/test_fwtool.py`, `tests/test_make_disk.py` | Tests for `fwtool.py` and `make_disk.py`; disk integration tests skip without mkfs.fat and the selected qemu-img. Run: `mise run check:tests` |

### `fwtool.py`

`--firmware` takes an Apple `.ipsw` or its bare `Firmware-XX` payload (format: `docs/ipod/classic6g-firmware.md`); the payload must have `]ih[` at offset `0x100`.

| Command | Flags (env) | Does |
| -- | -- | -- |
| `list` | `--firmware` (`FIRMWARE`) | Print the image directory and each image's header |
| `extract` | `--firmware`, `--outdir` (`OUTDIR`) | Write each image, with its payload and footer, to the directory |
| `osos` | `--firmware`, `--osos-encrypted` (`OSOS_ENCRYPTED`) | Write the encrypted `osos` image as the on-device decrypt expects it: the 0x800-byte header first, padded to the 16-byte AES block |

### `make_disk.py`

For an `.ipsw`, `make_disk.py` reports the firmware version, read from the `manifest.plist` inside the archive (for example, `2.0.5`). The manifest stores the version as a single number, `VisibleBuildID` (here `0x02058000`), in Apple's classic Mac version format. A number that does not denote a final release is rejected with an error. A bare `Firmware-XX` payload contains no manifest, so no version is reported.

#### Test library

`--track=PATH` (or `portblue`, the bundled track `lib/sample_library/portblue.m4a`) also puts a one-track music library on the volume, so a freshly built disk can play. `ipod_library.py` copies `fixtures/ipod_control` (a library libgpod wrote, with its `SysInfo`), places the track in `iPod_Control/Music/F00`, and rewrites the database through libgpod so it carries the checksum the retailOS requires (a hand-patched database lists "No Songs"). The track's title, path, size, length and bitrate come from the file; the artist and album stay those of the fixture. Needs `mtools`, `ffprobe` and `libgpod.so.4`, all in the `qemu-ipod-dev` image.

#### Disk layout

The disk is an empty bootable qcow2 (thin-provisioned), laid out as the retailOS expects:

| Part | Where | Contents |
| -- | -- | -- |
| MBR | block 0 | Partition table in 4096-byte units, `0x55AA` signature |
| Firmware partition | block 8, type `0x00` | The `Firmware-XX` payload, which carries the fonts the UI needs; a boot without it reset-loops |
| Data partition | right after, type `0x0b` | Empty 1 GiB FAT32, with the bytes `FAT` at `+0x34` of the boot sector and of its backup copy (see below) |

> The `FAT` marker is not part of the FAT format. Offset `+0x34` falls in the reserved bytes of the FAT32 boot sector ([layout](https://en.wikipedia.org/wiki/Design_of_the_FAT_file_system#BPB)), which ordinary FAT drivers ignore. The retailOS apparently still requires it and will not mount a volume without the marker.

### Driving the emulated UI and capturing screens

Boot an osos image, drive the iPod menus and save one PNG per screen, verifying every step. Run with `uv run --script` from the repo root; QEMU comes from `$BUILD_DIR` (default `build/native`) and the default image and disk are `assets/firmware/2.0.5/osos_decrypted.bin` and `assets/disks/Emedia.img`.

| Tool | Use |
| -- | -- |
| `capture_screenshots.py` | The survey: `capture_screenshots.py IMAGE [--screens ...] [--parallel N] [--cold-boot] ...`; writes `<out>/<screen>.png` and fails on a bad boot, navigation check or black frame |
| `goto_screen.py` | Boot and drive to one screen of `screen_tree.json`; CLI `--to SCREEN` |
| `screenshot_diff.py` | Compare two screenshots or two survey directories |

How to: `docs/ipod/how-to/capture-ui-screenshots.md`. Reference: `docs/ipod/reference/screenshot-harness.md`.

### Libraries (`lib/`)

Shared by the tools above; not run directly.

| Module | Use |
| -- | -- |
| `cli.py` | Flag handling shared by the `scripts/ipod/` tasks (`README.md`): `--name=value` or `name=value`, environment defaults, paths from the repo root |
| `ipod_library.py` | Puts a one-track music library on a disk image; the logic behind `make-disk --track`. Its data is `sample_library/` |
| `qemu_session.py` | Launch a headless QEMU and drive it over the HMP monitor (`launch`, `Monitor`, `Pacer`) |
| `menu_navigation.py` | Relative navigation: `navigate_to`, `route` and the `verify` invariants (never pixel matching) |
| `screen_tree.py`, `screen_tree.json` | The screen tree: nested data definitions, loaded and validated |
| `screenshot.py` | Read, compare and write screenshots (`.ppm`/`.png`) |

Their functions and classes: `docs/ipod/reference/tools-api/README.md`.

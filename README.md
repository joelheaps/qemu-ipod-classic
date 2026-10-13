# iPod classic 6G in QEMU

This fork of [qemu-ipod-classic](https://github.com/davidmonterocrespo24/qemu-ipod-classic) is specifically for running iPod Classic 6th generation (and pseudo-7th generation) firmware.

|  |  |  |
| -- | -- | -- |
| ![Language](docs/ipod/screenshots/language.png) | ![Main menu](docs/ipod/screenshots/main-menu.png) | ![Music menu](docs/ipod/screenshots/music-menu.png) |
| Language screen | Main menu | Music menu |
| ![Now playing](docs/ipod/screenshots/now-playing.png) |  |  |
| Playback (mostly) works |  |  |

## Getting the firmware

Firmware images can be found at [The Apple Wiki](<https://theapplewiki.com/wiki/Firmware/iPod#iPod_classic_(6th_generation)>), but newer iPod Classic `osos` firmware file is encrypted and must be decrypted before use (e.g. with [wInd3x](https://github.com/freemyipod/wInd3x) or on-device under emCORE).

| Device | Path | Reference |
| -- | -- | -- |
| Classic 6G | Keep the `.ipsw` (it also builds the disk). Extract `osos` from it, then have the device decrypt it with its own GID key under emCORE (1.2 s over USB) | [`docs/ipod/how-to/decrypt-classic-firmware.md`](docs/ipod/how-to/decrypt-classic-firmware.md) |

## Building and running

Mise manages contributor tools and exposes the same tasks on the host and inside Docker. The build follows upstream QEMU's `configure` → `make` workflow. Native setup targets Ubuntu 24.04/26.04 and Fedora with full FFmpeg packages; macOS native builds are outside this first pass.

Install [mise](https://mise.jdx.dev), then choose either path. A visible window requires a Linux desktop; Docker windows need X11 or Xwayland and a readable Xauthority file. Headless Docker runs can also be launched from other Docker hosts.

**Native Linux:**

```sh
mise install
mise run setup                  # once: installs host packages, using sudo when needed
mise run build                  # build/native/qemu-system-arm, qemu-img and trace plugins
mise run ipod:make-disk -- --firmware=path/to/iPod_38.2.0.5.ipsw --disks=path/to/disk.qcow2
mise run ipod:run -- --osos=path/to/osos_decrypted.bin --disks=path/to/disk.qcow2
mise run ipod:run -- --headless --timeout=30 --osos=... --disks=...
```

`setup` wraps upstream's `apt-get build-dep --arch-only qemu` / `dnf builddep qemu`, then installs the fork's extra build and runtime tools. On Ubuntu, enable source repositories first (`Types: deb deb-src` in the Ubuntu deb822 source file, or equivalent `deb-src` entries). On Fedora, enable RPM Fusion yourself for full `ffmpeg` / `ffmpeg-devel` packages; distribution `ffmpeg-free` packages lack some codecs this fork needs. `--arch-only` omits distribution firmware rebuilds and their cross-compilers. Setup does not add repositories or upgrade the operating system. Builds check prerequisites and never invoke setup automatically.

**Docker:**

Requires a working Docker installation. Mise's Python runs the host-side wrappers; QEMU dependencies are installed only inside the images.

```sh
mise install
mise run docker:build            # builds its image as needed; build/docker/qemu-system-arm
mise run docker:ipod:make-disk -- --firmware=path/to/iPod_38.2.0.5.ipsw --disks=path/to/disk.qcow2
mise run docker:ipod:run -- --osos=path/to/osos_decrypted.bin --disks=path/to/disk.qcow2
mise run docker:ipod:run -- --headless --timeout=30 --osos=... --disks=...
```

Docker wrappers invoke `mise run build`, `mise run ipod:make-disk`, and `mise run ipod:run` inside Ubuntu. The Dockerfiles also invoke the shared `setup` task during image construction (`--runtime` for the dev image). Native setup never runs on the Docker host. Image caching tracks the Dockerfile, mise configuration and setup script. Execution tasks come from the mounted checkout, so changes to them do not rebuild images. Ubuntu 26.04 is the validated Docker LTS base. The full ARM emulator, qemu-img and both trace plugins build with uv-managed Python 3.14.8; synthetic disk, headless guest and GDB checks pass. RetailOS and desktop window/audio checks require real firmware and a Linux desktop.

Native windows run in the foreground. Docker windows are detached in the managed `ipod-live` container. `AUDIO=0` disables host audio; Docker audio forwards an available PulseAudio/PipeWire-pulse socket. Without that socket the Docker window runs silently. `DRYRUN=1` previews launch behavior when scripts are invoked directly; mise Docker task dependencies may build images first.

`BUILD_DIR` defaults to `build/native`; Docker wrappers select `build/docker`. `JOBS` overrides native/container build parallelism. QEMU uses uv-managed Python 3.14.8 (`QEMU_PYTHON` can override it), with a local `BUILD_DIR/bootstrap` virtual environment and pinned distlib. QEMU manages its own bundled Meson wheel. Docker images preinstall that Python version. Build defaults enable only the features needed by this fork, including SDL, PulseAudio, FFmpeg decoders and trace plugins. Pass configure flags to `mise run build -- ...` or `mise run docker:build -- ...`; supplying flags reconfigures the selected directory. Use separate build directories for distinct configurations.

The old `build-in-docker` task has been removed. `ipod:*` tasks now run in their current environment; use `docker:*` to select container execution. Existing binaries in `build/` are not reused. Tasks can also be invoked as scripts directly after tool/dependency setup; Docker script dependencies require the corresponding images/build to exist.

Emulation startup requires both an unencrypted Apple firmware blob (`osos` image) and iPod compatible disk image, which can be generated using `make-disk` and the encrypted firmware blob.

Default boot options can be configured via environment variables, optionally via `mise.local.toml`.

```toml
[env]
OSOS = "assets/firmware/osos_decrypted.bin"
DISK = "assets/disks/ipod-disk.qcow2"
FIRMWARE = "assets/firmware/iPod_38.2.0.5.ipsw"   # creates DISK on first run
```

| Variable | Argument | Effect |
| -- | -- | -- |
| `OSOS` | `--osos=PATH` | Decrypted `osos` to boot (see [Getting the firmware](#getting-the-firmware)); required |
| `DISK` | `--disks=PATH` | Disk image, qcow2 or raw; required |
| `FIRMWARE` | `--firmware=PATH` | `.ipsw` or `Firmware-XX` payload; if `DISK` is missing, `ipod:make-disk` creates it from this ([Disk and firmware tasks](#disk-and-firmware-tasks)) |
| `T` | `--timeout=SECS` | Run time limit in seconds; default 0 (no limit) |

Other arguments to `ipod:run` are passed to QEMU (under mise, after `--` if they start with `-`). Run other commands in the dev container with `docker run --rm -i --user $(id -u):$(id -g) -v $PWD:$PWD -w $PWD qemu-ipod-dev <cmd>`. Screenshots, keys, gdb, tracing and snapshots: [`docs/ipod/how-to/debug-a-classic-guest.md`](docs/ipod/how-to/debug-a-classic-guest.md).

### Disk and firmware tasks

Besides `ipod:run`, `scripts/ipod/` has one task for preparing a disk (`mise tasks` lists everything). Like `ipod:run`, it takes `--name=value` or `name=value` flags, each with an environment variable as its default; flags win. Relative paths are from the repository root, and no task assumes where you keep files.

`--firmware` is an Apple `.ipsw` or its bare `Firmware-XX` payload (accepted inputs: `tools/README.md`). To inspect or extract firmware, run `fwtool.py` directly (commands: `tools/README.md`).

#### `scripts/ipod/make-disk`

Creates a bootable disk, thin-provisioned qcow2, for `ipod:run`: empty, or with a one-track library when `--track` is given.

| Flag | Env | Meaning |
| -- | -- | -- |
| `--firmware=PATH` | `FIRMWARE` | The firmware to build the disk around |
| `--disks=PATH` | `DISK` | Disk to create |
| `--track=PATH` | `TRACK` | Optional: put a one-track library on the disk (`portblue` for the shipped AAC fixture, or any audio file; `tools/README.md`) |

The script behaves as follows:

- If the disk file already exists, the script leaves it unchanged. To build a new disk, delete the file first.
- If no flags are given and `FIRMWARE` is not set, the script does nothing and reports that. This allows an existing disk to be used without supplying firmware.
- `ipod:run` also calls the script when the disk named by `DISK` does not exist and a firmware was supplied (`--firmware` or `FIRMWARE`).

The script runs `tools/ipod_classic/make_disk.py` using `mkfs.fat` and `BUILD_DIR/qemu-img` in its current environment. `docker:ipod:make-disk` invokes the same task in the dev container. What it builds, and how it reads the firmware, is described in `tools/README.md`.

`ipod:run` boots with QEMU's `-snapshot`, so guest writes go to an overlay and never reach the disk file.

### Emulator input methods

| Key | Button |
| -- | -- |
| `q` / `e` (or mouse scroll on the classic) | wheel anticlockwise / clockwise |
| `Enter` | SELECT |
| `w` | MENU |
| `a` / `d` | previous / next |
| `s` | PLAY / PAUSE |

## Testing

iPod Classic tooling tests are in `tools/ipod_classic/tests/` and run under `pytest`:

```sh
mise run check:tests                 # native tests; integration tests skip without mkfs.fat/qemu-img
mise run check:tests -- -k version   # extra arguments are passed to pytest
mise run docker:check                # the same checks inside Docker
```

The disk end-to-end tests (`tools/ipod_classic/tests/test_make_disk.py`) require `mkfs.fat` and `BUILD_DIR/qemu-img`, so they work natively or inside Docker. The real-firmware test additionally needs an Apple `.ipsw` under Apple's file name `iPod_<family>.<version>.ipsw`, at `assets/test/iPod_38.2.0.5.ipsw` (or wherever `IPOD_TEST_IPSW` points).

## Limitations

**iPod Classic 6G**

- The SM1 audio engine decodes AAC only (libavcodec); other codecs are not wired.
- No USB, NAND, CE-ATA or AES.

## Additional iPod-related documentation

| Kind | Document | Contents |
| -- | -- | -- |
| How-to | [`docs/ipod/how-to/decrypt-classic-firmware.md`](docs/ipod/how-to/decrypt-classic-firmware.md) | `.ipsw` to `osos_decrypted.bin` with emCORE |
| How-to | [`docs/ipod/how-to/debug-a-classic-guest.md`](docs/ipod/how-to/debug-a-classic-guest.md) | Boot, screenshots, keys, memory, gdb, pctrace, audio capture, snapshots |
| Reference | [`hw/arm/ipod_classic/README.md`](hw/arm/ipod_classic/README.md) | S5L8702 memory map, device models, audio and video pipelines, environment variables, snapshot contract |
| Reference | [`docs/ipod/classic6g-firmware.md`](docs/ipod/classic6g-firmware.md) | `.ipsw`/`]ih[`/IM3 formats, emCORE decrypt, NOR layout, what cannot be dumped |
| Reference | [`tools/README.md`](tools/README.md) | Tools in this tree |

## External and upstream resources

Forked from [davidmonterocrespo24/qemu-ipod-classic](https://github.com/davidmonterocrespo24/qemu-ipod-classic), itself a fork of [devos50/qemu-ios](https://github.com/devos50/qemu-ios) (QEMU 8.2.0, branch `ipod_touch_2g`), which emulates the **iPod Touch 1G and 2G**.


QEMU's own documentation is in [`docs/`](docs/).

This work also depends on: **Rockbox** (register-level drivers: `firmware/target/arm/s5l8702/` and `firmware/export/s5l87xx.h`), **iPodLinux** (`ipodloader2/ipodhw.c`, the SysInfo block), **ipodpatcher** (the dual-boot layout) and **[freemyipod.org](https://freemyipod.org/wiki/Classic_6G)** (emCORE, and a QEMU model of the S5L8702 in `freemyipod/qemu@s5l8702-dev`).

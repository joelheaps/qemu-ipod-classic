# Two clickwheel iPods in QEMU, booting Apple's real firmware

This fork of [QEMU-iOS](https://github.com/devos50/qemu-ios) adds **two machines** that emulate clickwheel iPods
and **boot real software dumped from the physical devices**, with no boot ROM and without patching a firmware
instruction.

| Machine | SoC | What it boots | Reference |
|---|---|---|---|
| **`iPod-Classic`** | Samsung S5L8702 | **Apple's retailOS 2.0.5**: RTXC kernel, disk, fonts, clickwheel navigation, the music library and the Now Playing screen, with audio through the guest's own render pump, DMA and I2S | [`hw/arm/ipod_classic/README.md`](hw/arm/ipod_classic/README.md) |
| **`iPod-Video`** | PortalPlayer PP5022C | **Rockbox** to its menu, and **Apple's retailOS**: language screen → main menu → submenus → back | [`hw/arm/ipod_video/README.md`](hw/arm/ipod_video/README.md) |

Neither machine emulates Apple's boot ROM: the hardware the firmware touches is modelled, and what the ROM left in
memory is reproduced by the machine.

> **You need your own iPod.** This repository ships **no firmware**. Dumped artefacts live in `assets/`, which is
> kept out of git: they are not ours to redistribute.

![The retailOS music player running in QEMU](docs/screenshots/now-playing.png)

| | | |
|---|---|---|
| ![Language](docs/screenshots/language.png) | ![Main menu](docs/screenshots/main-menu.png) | ![Music menu](docs/screenshots/music-menu.png) |
| Language screen | Main menu | Music menu |

## Building and running

Builds run in a Docker dev container driven by [mise](https://mise.jdx.dev):

```sh
mise run docker:build-images     # once: the ipod-qemu-build image
mise run emulator:build          # configure (first time) and build build/qemu-system-arm
T=30 mise run emulator:boot      # headless retailOS 2.0.5 boot; OSOS=<image>, extra args go to QEMU
mise run emulator:run            # the same in an SDL window
```

`emulator:boot` expects `assets/firmware/2.0.5/osos_decrypted.bin` and a disk image (`DISK`, default
`assets/disks/E.img`). Driving the emulator (navigation, screenshots, snapshots, traces) and building disks and
firmware images is the sibling repo `../osos-re` (`tools/`, `firmware/`). Tool index: [`tools/README.md`](tools/README.md).

### Keys (both machines)

| Key | Button |
|---|---|
| `q` / `e` (or mouse scroll on the classic) | wheel anticlockwise / clockwise |
| `Enter` | SELECT |
| `w` | MENU |
| `a` / `d` | previous / next |
| `s` | PLAY / PAUSE |

### Getting the firmware

| Device | Path | Reference |
|---|---|---|
| Classic 6G | Extract `osos` from the `.ipsw`, then have the device decrypt it with its own GID key under emCORE (1.2 s over USB) | [`docs/classic6g-firmware.md`](docs/classic6g-firmware.md) |
| Video 5G | Unencrypted; dump the firmware partition (`tools/ipod_video/dump-firmware.ps1`) from a Rockbox dual-boot iPod | [`hw/arm/ipod_video/README.md`](hw/arm/ipod_video/README.md) |

## Known limits

**iPod Classic 6G**

- Elapsed time stays at 0:00 (the player's accounting never runs; see the classic README).
- The SM1 audio engine decodes AAC only (libavcodec); other codecs are not wired.
- No USB, NAND, CE-ATA, AES or bootrom. Full list in the classic README.

**iPod Video 5G**

- The wheel does not move the highlight on the retailOS language screen (measured, not diagnosed).
- No audio and no USB. The VideoCore's protocol is modelled, its processor does not run.
- DRAM size is an open question (64 MB from silicon vs 32 MB in two other sources; both boot).

**Both:** several constants are choices or inferences, marked `ASSUMED` / `INFERRED` / `UNDETERMINED` in the
code. `tools/ipod_video/navtest.sh` and `appletest.sh` print evidence and assert nothing.

## Where the detail lives

| Document | Contents |
|---|---|
| [`hw/arm/ipod_classic/README.md`](hw/arm/ipod_classic/README.md) | S5L8702 memory map, device models, env knobs, snapshots, debugging a running guest |
| [`hw/arm/ipod_video/README.md`](hw/arm/ipod_video/README.md) | PP5022 memory map, BCM/VideoCore interface, assumptions and deviations |
| [`docs/classic6g-firmware.md`](docs/classic6g-firmware.md) | `.ipsw`/`]ih[`/IM3 formats, emCORE decrypt, NOR layout, what cannot be dumped |
| [`docs/video-decoder.md`](docs/video-decoder.md) | The 6G video decoder block |
| [`docs/debugging-lessons.md`](docs/debugging-lessons.md) | Failure patterns and evidence rules from both ports |
| [`tools/README.md`](tools/README.md) | Emulator-side tools |

## This repository is a fork

Forked from [devos50/qemu-ios](https://github.com/devos50/qemu-ios) (QEMU 8.2.0), branch `ipod_touch_2g`, which
emulates the **iPod Touch 1G and 2G**. That work is here intact and its machines are listed in `-M help`:

- [Running the iPod Touch 1G](https://devos50.github.io/blog/2022/ipod-touch-qemu-pt2/) and the
  [technical write-up](https://devos50.github.io/blog/2022/ipod-touch-qemu/).
- [Running the iPod Touch 2G](https://github.com/devos50/qemu-ios/blob/ipod_touch_2g/RUNNING.md).

QEMU's own documentation is in [`docs/`](docs/).

This work depends on: **Rockbox** (register-level drivers for both devices: `firmware/target/arm/s5l8702/` and
`firmware/export/s5l87xx.h` for the classic, `firmware/target/arm/ipod/` and `pp5020.h` for the 5G),
**iPodLinux** (`ipodloader2/ipodhw.c`, the SysInfo block), **ipodpatcher** (the dual-boot layout) and
**[freemyipod.org](https://freemyipod.org/wiki/Classic_6G)** (emCORE, and a QEMU model of the S5L8702 in
`freemyipod/qemu@s5l8702-dev`).

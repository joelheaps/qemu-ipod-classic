# iPod classic 6G firmware

Reference for the 6G firmware containers, decrypting `osos` on a physical device, and what can and cannot be read from one. Dumps go under `assets/` (gitignored); nothing here is shipped.

## Firmware container

An `.ipsw` is a zip holding `Firmware-XX` (the firmware partition) and `manifest.plist` (`FirmwareName`, `BuildID`, `VisibleBuildID`, `FamilyID` 11, `UpdaterFamilyID` 24 or 38).

`VisibleBuildID` is the user-facing version as a Mac `NumVersion`: major (BCD), minor and bug nibbles, stage (`0x80` final), pre-release revision. `0x02058000` is 2.0.5. `BuildID` (`0x09058000` for 2.0.5) is not the version.

The firmware partition uses the `]ih[` container, the same one `ipodpatcher` handles:

| Offset | Field |
| -- | -- |
| `0x000` | 256-byte ASCII banner ("STOP / Copyright(C) 2001 Apple Computer, Inc.") |
| `0x100` | magic `]ih[` |
| `0x104` | directory offset, biased: real offset = field + `0x1000` in a 6G `.ipsw` (`+0x200` on a 5G disk) |
| `0x108` | `0x0003010c`, unidentified |

Every `devOffset` in the directory carries the same bias. Without it, reads land in padding and `rsrc` looks empty.

A directory entry is 40 bytes, little-endian: `dev` (stored reversed, `ATA!`), `type` (reversed: `osos`, `rsrc`, `aupd`, `hash`), `id`, `devOffset`, `len`, `addr`, `entryOffset`, `chksum`, `vers`, `loadAddr`.

| Image | Load address | Contents |
| -- | -- | -- |
| `osos` | `0x08000000` | retailOS, IM3-wrapped and encrypted |
| `aupd` | `0x08000000` | updater, encrypted |
| `rsrc` | none | FAT16 volume `IPODRESOURC` (OEM `MTOOL399`, 78 MB): `Resources/Fonts` (Helvetica, CJK11/16 TTF, `.SBI`), `Games/`, `Trainers/`, `Voices/` |
| `hash` | none | `0xFF` fill, generated on the device |

The UI needs `rsrc`: without it the font loader returns NULL and the firmware calls through it. A diskless boot therefore reset-loops (`CPU_exception_handler` on semihosting) and never draws.

## IM3 (`8702`) header

`struct Im3Info` (Rockbox `crypto-s5l8702.h`). The header is `0x800` bytes (`0x600` on the S5L8720).

| Offset | Field |
| -- | -- |
| `0x00` | `"8702"` (not reversed) |
| `0x04` | version `"1.0"` |
| `0x07` | `enc_type`: 1 signed+encrypted, 2 signed, 3 X.509 signed+encrypted, 4 X.509 signed |
| `0x08` | entry point |
| `0x0C` | data size |
| `0x10` | types 1/2: `data_sign[16]` + 32 bytes reserved. Types 3/4: `sign_off`, `cert_off`, `cert_sz` + 36 bytes |
| `0x40` | `info_sign[16]` = AES(key, SHA1(hdr[0:0x40])[0:16]) |

In type 3/4 headers, offsets are relative to the payload (`0x800`). `osos` and `aupd` are type 3 with a `0xbe3`-byte signature/certificate footer; the directory `len` is the total size minus `0x1000`. Verification checks the signed header hash, then an X.509 chain rooted in the ROM (Apple Root CA). All AES uses the GID key.

## Decrypting on the device

The `osos` payload is AES-128-CBC under the per-model GID key, which is fused into the SoC and cannot be extracted. The device can still decrypt with it, because emCORE's monitor exposes the AES engine.

| Item | Detail |
| -- | -- |
| emCORE build | v0.2.3 r859, USB `ffff:e000`, `hwtypeid` `"IPCL"` |
| Protocol | pre-r891: four bulk endpoints in descriptor order (cmd OUT/IN, data OUT/IN), 16-byte header, status 1 = OK |
| Memory read | cmd 4 (up to 496 B), or cmd 6 over the data pipe (up to 256 KiB, 16-byte aligned) |
| AES | cmd 25: `{id, dir (0 dec / 1 enc), 0, keyidx u16, addr, size}`, in place. `keyidx` 1 = GID, 2 = UID |
| IV reset | `hwkeyaes()` zeroes the IV on every call, which corrupts the first block of each chunk in chunked CBC. Fix: `P0 ^= last ciphertext block of the previous chunk` |
| Throughput | 9.9 MB in 1.2 s |
| Official clients | Neither works: HEAD expects interface `ff/00/00` and EP0; r859 passes a pyusb argument removed in 1.0.0b1 |

A correct decrypt shows ARM vectors at offset 0, `Copyright 2001-2012 Apple Inc.`, `RTXC v3.2b`, UI strings (`Cover Flow`, `Now Playing`) and about 42k ASCII strings. `N25` is Apple's codename for the classic.

## NOR flash

The NOR is 1 MiB on SPI0. Dump it read-only with emCORE cmd 22 (`bootflashread`, SPI READ DATA).

| Offset | Contents |
| -- | -- |
| `0x00000` | SysCfg (`SCfg`): 20-byte entries, tag stored byte-reversed: `SrNm`, `FwId`, `HwId`, `HwVr`, `Codc`, `SwVr`, `MLBN`, `Mod#`, `Regn` |
| `0x08000` | NOR boot IM3 (`NORBOOT_OFF`, max 128 KiB; header at `+0`, body at `+0x800`) |
| end − `0x200` | `flsh` directory |

`HwVr` (`0x00130000`) is hardware id 19, which the `osos` requires in the boot info block; otherwise it reboots to `softupdt`.

On a device with emCORE installed, `0x8000` holds `emCORE Loader` signed with the UID key (written by the installer, not Apple), the emCORE files start at `0x1000`, and Apple's NOR boot image is gone. The `.ipsw` does not carry it either.

The loader shows the state the bootrom hands over. It turns the MMU on with dirty caches (it disables and cleans them), then uses a flat 1:1 map, cacheable below `0x38000000`. It pushes the framebuffer to `LCD_WDATA` (`0x38300040`) through PL080 LLIs, waits on `CLK_BASE+0x40` bit 2 (PLL lock), and programs timers and GPIO.

## Unreadable regions

| Target | Result |
| -- | -- |
| Bootrom `0x20000000` | Locked after boot: reads return four constants, one per 16 KiB bank, repeating every 64 KiB. No public dump exists; DFU plus an exploit (wInd3x) is the route |
| `0x20100000` | Hangs the emCORE monitor: it still enumerates but every command times out, and MENU+SELECT resets it. Read only DRAM `0x08000000`, SRAM `0x22000000` and documented peripherals |
| Chip ID `0x3D100000` | Reads 0 (clock-gated) |

## Boot chain

On the real device: bootrom (`0x20000000`, aliased at 0), then the NOR boot IM3 (6G), NAND (nano 3G) or USB DFU (SELECT+MENU), then an EFI-based second stage that brings up DRAM, LCD and the VICs, then `osos` from the hard disk. DFU payloads run at `0x2200030C`.

The emulator skips everything before `osos`; see `hw/arm/ipod_classic/README.md`.

## Sources

- Rockbox: `firmware/export/s5l87xx.h`, `firmware/target/arm/s5l8702/` (`crypto-s5l8702.{c,h}`, `norboot-target.h`, `dma-s5l8702.h`), `utils/mks5lboot`
- Inside Macintosh: `NumVersion` (major/minor/bug BCD, stage constants)
- freemyipod.org wiki: `Boot_Process`, `Bootrom`, `IMG1`, `Classic_6G`, `EmCORE_Monitor_Protocol`
- `github.com/freemyipod/freemyipod` (r859 = `92b156a3b52f`)
- `github.com/freemyipod/qemu`, branch `s5l8702-dev`: an independent S5L8702 model that needs a bootrom dump

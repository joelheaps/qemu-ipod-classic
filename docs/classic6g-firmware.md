# iPod classic 6G firmware and the real device

Reference for the firmware containers, the decrypt-on-device path and what can and cannot be read from a
physical 6G. The tools live in `../osos-re`: `firmware/fwtool.py`, `firmware/norboot_im3.py`,
`tools/device/emcore_s5l8702.py`. Nothing here is shipped; dumps go under `assets/` (gitignored).

## `.ipsw` → firmware partition → images

The `.ipsw` is a zip: `Firmware-XX` (the firmware partition) plus `manifest.plist` (`FirmwareName`, `BuildID`,
`FamilyID` 11, `UpdaterFamilyID` 24).

Firmware partition (`]ih[`, the same container `ipodpatcher` handles):

| Offset | Field |
|---|---|
| `0x000` | 256 B ASCII banner ("STOP / Copyright(C) 2001 Apple Computer, Inc.") |
| `0x100` | magic `]ih[` |
| `0x104` | directory offset, **biased**: real offset = field + `0x1000` in a 6G `.ipsw` (`+0x200` on a 5G disk) |
| `0x108` | `0x0003010c`, unidentified |

Every `devOffset` in the directory carries the same bias. Reading without it lands in padding: that is how
`rsrc` was once taken for empty. `fwtool.py` tries the known biases, then scans.

Directory entry, 40 bytes LE: `dev` (stored reversed, `ATA!`), `type` (reversed: `osos`, `rsrc`, `aupd`,
`hash`), `id`, `devOffset`, `len`, `addr`, `entryOffset`, `chksum`, `vers`, `loadAddr`.

| Image | Load `addr` | Contents |
|---|---|---|
| `osos` | `0x08000000` (= SDRAM base) | the retailOS, IM3-wrapped, encrypted |
| `aupd` | `0x08000000` | the updater, encrypted |
| `rsrc` | — | **FAT16 volume** `IPODRESOURC` (OEM `MTOOL399`, 78 MB): `Resources/Fonts` (Helvetica, CJK11/16 TTF, `.SBI` bitmaps), `Games/`, `Trainers/`, `Voices/` |
| `hash` | — | `0xFF` fill, generated on device |

The UI cannot draw without `rsrc`: the font loader returns NULL and the firmware calls through it. That is
why a diskless boot reset-loops (`CPU_exception_handler` on semihosting) and never shows a pixel.

## IM3 (`8702`) image header

`struct Im3Info` (Rockbox `crypto-s5l8702.h`), header size `0x800` (`0x600` on the S5L8720):

| Offset | Field |
|---|---|
| `0x00` | `"8702"` (not reversed) |
| `0x04` | version `"1.0"` |
| `0x07` | `enc_type`: 1 signed+encrypted, 2 signed, 3 X.509 signed+encrypted, 4 X.509 signed |
| `0x08` | entry (LE) |
| `0x0C` | data size (LE) |
| `0x10` | type 1/2: `data_sign[16]` + 32 B reserved. Type 3/4: `sign_off`, `cert_off`, `cert_sz` + 36 B |
| `0x40` | `info_sign[16]` = AES(key, SHA1(hdr[0:0x40])[0:16]) |

Offsets in type 3/4 headers are relative to the payload (`0x800`). The `osos` and `aupd` are type 3 with a
`0xbe3`-byte signature/certificate footer; directory `len` = total − `0x1000`. Verification is the signed header
hash, then an X.509 chain rooted in the ROM (Apple Root CA). iPod images use the GID key for all AES.

## Decrypting on the device (emCORE)

The `osos` payload is AES-128-CBC with the per-model GID key, fused into the SoC. It cannot be extracted, but the
device will decrypt with it: emCORE's monitor exposes the AES engine.

| Fact | Value |
|---|---|
| emCORE build that ships | v0.2.3 r859, USB `ffff:e000`, `hwtypeid` `"IPCL"` |
| Protocol | pre-r891: four bulk endpoints in descriptor order (cmd OUT/IN, data OUT/IN), 16-byte header, status 1 = OK |
| Memory read | cmd 4 (≤ 496 B) or cmd 6 over the data pipe (≤ 256 KiB, 16-byte aligned) |
| AES | cmd 25: `{id, dir (0 dec / 1 enc), 0, keyidx u16, addr, size}`, in place. keyidx 1 = GID (GKEY), 2 = UID (UKEY) |
| **IV trap** | `hwkeyaes()` zeroes the IV on every call. Chunked CBC breaks the first block of each chunk; fix it with `P0 ^= last ciphertext block of the previous chunk` |
| Speed | 9.9 MB in 1.2 s |
| Official clients | neither works: HEAD wants interface `ff/00/00` and EP0; r859 passes a pyusb argument removed in 1.0.0b1 |

Decrypted `osos` checks: ARM vectors at 0, `Copyright 2001-2008 Apple Inc.`, `RTXC v3.2b`, UI strings
(`Cover Flow`, `Now Playing`), ~42k ASCII strings (0 before). `N25` is Apple's codename for the classic.

## NOR (SPI0, 1 MiB)

Read-only dump: emCORE cmd 22 (`bootflashread`, SPI READ DATA).

| Offset | Contents |
|---|---|
| `0x00000` | SysCfg (`SCfg`), 20-byte entries, tag stored byte-reversed: `SrNm`, `FwId`, `HwId`, `HwVr`, `Codc`, `SwVr`, `MLBN`, `Mod#`, `Regn` |
| `0x08000` | NOR boot IM3 (`NORBOOT_OFF`, max 128 KiB; header `+0`, body `+0x800`) |
| end − `0x200` | `flsh` directory |

`HwVr` (`0x00130000`) is the hardware id 19 the `osos` demands in the boot info block (otherwise it reboots to
`softupdt`). On a device with emCORE installed, `0x8000` holds `emCORE Loader` signed with the UID key (so
written by the installer, not Apple), the emCORE files sit at `0x1000+`, and Apple's ONB is gone; the `.ipsw` does
not carry it either. `fwtool.py` parses only `enc_type` 3/4; use `norboot_im3.py` for 1/2.

What the loader does, which is what the bootrom hands over: MMU on with dirty caches (it disables and cleans
them), then a flat 1:1 map, cacheable below `0x38000000`. It pushes the framebuffer to `LCD_WDATA`
(`0x38300040`) through PL080 LLIs, waits on `CLK_BASE+0x40` bit 2 (PLL lock), and programs timers and GPIO.

## What cannot be read

| Target | Result |
|---|---|
| Bootrom `0x20000000` | locked after boot: four constants, one per 16 KiB bank, repeating every 64 KiB. No public dump exists; DFU + exploit (wInd3x) would be the route |
| `0x20100000` | **hangs the emCORE monitor** (still enumerates, every command times out; MENU+SELECT reset). Read only DRAM `0x08000000`, SRAM `0x22000000` and documented peripherals |
| Chip ID `0x3D100000` | reads 0 (clock-gated) |

## Boot chain (real device)

Bootrom (`0x20000000`, aliased at 0) → NOR boot IM3 (6G) / NAND (nano 3G) / USB DFU (SELECT+MENU) →
second stage (EFI-based; loads DRAM, LCD, VICs) → `osos` from the hard disk. DFU payloads run at `0x2200030C`.
The emulator skips everything before the `osos` (`hw/arm/ipod_classic/README.md`).

## Sources

Rockbox `firmware/export/s5l87xx.h`, `firmware/target/arm/s5l8702/` (`crypto-s5l8702.{c,h}`,
`norboot-target.h`, `dma-s5l8702.h`), `utils/mks5lboot`; freemyipod.org wiki (`Boot_Process`, `Bootrom`,
`IMG1`, `Classic_6G`, `EmCORE_Monitor_Protocol`); `github.com/freemyipod/freemyipod` (r859 =
`92b156a3b52f`); `github.com/freemyipod/qemu` branch `s5l8702-dev` (an independent S5L8702 model that needs a
bootrom dump).

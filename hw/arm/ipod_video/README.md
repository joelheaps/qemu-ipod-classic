# iPod video 5G (PP5022) — `-M iPod-Video`

Hardware reference for the PortalPlayer machine, what is modelled, and the model's known deviations. Boots Rockbox (to its menu) and Apple's retailOS (language → main menu → submenus) from the same dual-boot `osos`. Task steps: `docs/ipod/how-to/run-an-ipod-video.md`.

Primary source: Rockbox `firmware/export/pp5020.h` (PP5020 and PP5022 share one map), `firmware/target/arm/pp/`, `bootloader/ipod.c`; iPodLinux `ipodloader2/ipodhw.c`. Apple-only facts cite retailOS offsets (load base `0x10000000`). Code marks its own guesses `ASSUMED` / `INFERRED` / `UNDETERMINED`.

## The device

| Fact | Value | Source |
| -- | -- | -- |
| SoC | **PP5022C** | Rockbox, read from silicon (the `MA146` model number implies a PP5021C/32 MB unit; this one has mixed parts and an iFlash card) |
| RAM | 64 MB (see open questions) | Rockbox |
| `osos` `vers` | `0x0000b012` → `0xb0` = "Video (5th Generation)" | firmware; `ipodpatcher getmodel()` |
| Disk | MBR; partition 0 type `0x00` at LBA 63 = firmware (`]ih[`, directory at `0x4200`); partition 1 FAT32 `IPOD` | dump |
| Firmware images | `osos` (`addr 0x10000000`, `entryOffset 0x735a00`), `rsrc`, `aupd`, `hibe` | directory |
| Encryption | none (entropy ~5.5, `portalplayer`, `retailOS`, `diskmode`, `softupdt` strings) |  |

### Dual boot and the boot path

The PP ROM (in NOR) loads the whole `osos` at `DRAM_START` and jumps to `DRAM_START + entryOffset`. `ipodpatcher` keeps Apple's image at offset 0 and appends Rockbox's bootloader at `entryOffset`:

| Piece | `osos` range | VA | Starts with |
| -- | -- | -- | -- |
| Apple retailOS | `0x000000`-`0x735a00` | `0x10000000` | ARM vectors; `"portalplayer"` at `+0x20` (the marker Rockbox's bootloader checks to chain back) |
| Rockbox bootloader | `0x735a00`-`0x742514` | `0x10735a00` | `msr cpsr_c, #0xd3` |

The emulator skips the ROM and must reproduce what it left: `GPO32` bit 14 (BCM powered), and the SysInfo `{tag "IsyS", ptr}` pair at `0x4001ff18`/`+4`, which the `osos` only ever compares (one instruction, `0x13fc`), so it must come from outside.

### NOR ROM dump (optional)

Not needed to boot either firmware. Rockbox: System → Debug (Keep Out!) → `Dump ROM contents` writes `/internal_rom_000000-0FFFFF.bin` (1 MiB, silently, no confirmation). Good dump: `hslf` at `0xFFE00`. Directory there: `0x28`-byte entries `flsh`, id (`disk`, `diag`, `logo`, `scan`, `vmcs`), offset, length, load address, checksum, version (`0xb0` high byte). `vmcs` is the Broadcom firmware; `tools/ipod_video/extract-vc.py` takes it from the disk's firmware partition instead.

## Two cores

CPU and COP (ARM7TDMI) run the same image and branch on `PROCESSOR_ID` (`0x60000000`: `0x55` CPU, `0xaa` COP). The model creates both: `PROCESSOR_ID` answers per `current_cpu`, `CPU_CTL`/`COP_CTL` (`0x60007000`/`+4`) bit 31 really halts and wakes, and the interrupt controller has one output per core with its own enable mask. The COP's first act is to put itself to sleep.

## Memory map

| Base | Block | Model |
| -- | -- | -- |
| `0x00000000` | NOR at boot, then DRAM via remap | blank window with HW revision at `+0x405c` |
| `0x04000000` | low window with bit 26 set | alias |
| `0x10000000` | DRAM (`DRAM_START`) | RAM |
| `0x14000000`, `0x90000000`, `0x94000000` | DRAM with bit 26 / bit 31 (uncached) / both | aliases. The memory controller does not decode bit 26 and the retailOS uses both forms; it tags its non-IRAM buffers with bit 31 |
| `0x20000000` | NOR, remapped | alias |
| `0x30000000` | Broadcom BCM2722 | `ipod_video_bcm.c`, `ipod_video_vc.c` |
| `0x40000000` | IRAM, 128 KB (derived: Apple puts globals at `IRAM_END-0x100+N`, `IRAM_END` `0x40020000` on PP5022, `0x40018000` on PP5020) | RAM |
| `0x60000000` | `PROCESSOR_ID` | yes |
| `0x60001000` | CPU-COP mailbox | no (floor) |
| `0x60004000` | interrupt controller; high bank at `+0x100` | `ipod_video_intc.c` |
| `0x60005000` | timers; `USEC_TIMER` `+0x10` | yes |
| `0x60006000` | clock/power; `PLL_STATUS` `+0x3c` | yes |
| `0x60007000` | `CPU_CTL`, `COP_CTL` | yes |
| `0x60008000` / `0x6000a000` | DMA block A / B (channels at `+0x1000`) | `ipod_video_dma.c` |
| `0x6000c000` | cache control (`READY` bit 14 always set) | yes |
| `0x6000d000` | GPIO | yes (`INPUT_VAL` reads `0xffffffff`: active-low, 0 = every button and hold pressed) |
| `0x70000000` | device control: `PP_VER1/2`, `DEV_INIT`, `+0x28`, `GPO32` | yes |
| `0x70002800` | I2S | no |
| `0x70003000` | PP LCD controller | no (the panel is on the Broadcom) |
| `0x70006000` | UART 0, 16550 stride 4 | `hw/char/serial.c`, IRQ unwired |
| `0x7000c000` | I2C (`+0x00`) and click wheel (`+0x100`) | `ipod_video_i2c.c`, `ipod_video_pcf50605.c`, wheel in `ipod_video.c` |
| `0xc3000000` | IDE | `ipod_video_ide.c` |
| `0xc5000000` | USB | unimp |
| `0xf0000000` | cache arrays (data, mirror, CPU/COP status, flush, invalidate) | RAM |
| `0xf000f000` | remap unit `MMAP0..7` | yes, `mask` field not applied |

`0x60000000` and `0x70000000` are backed by unimplemented-device floors: Apple touches registers `pp5020.h` does not name, and an unmapped access before the vectors are installed never returns.

`PP_VER1/2` must spell `"PP5022A "` (`0x32324120`/`0x50503530`), not `PP5022C`: Apple reads character 5 only (`cmp #'2'`) and knows the `A` forms; any other answer selects the PP5020 IRAM base.

Remap: Apple's DRAM-at-0 window is `MMAP1` (its `MMAP0` maps physical 0). Logical bit *k* covers address bit *k*+16 (`crt0-pp.S`: `0x3e00` = 32 MB, `0x3c00` = 64 MB).

### IRQs

| IRQ | Source | Driven by the model |
| -- | -- | -- |
| 0, 1 | timer 1, 2 | yes |
| 4 | mailbox | no |
| 10 | I2S | no |
| 20 | USB | no |
| 23 | IDE | yes |
| 26 | DMA block B (`DMA_IRQ`) | yes |
| 27 | DMA block A (not in `pp5020.h`; Apple's table) | yes |
| 30 | `HI_IRQ`, summary of the high bank | yes (synthetic) |
| 32-34 | GPIO 0-2 | no |
| 36, 37 | serial 0, 1 | no |
| 40 | I2C **and** click wheel | yes (wheel) |

`HI_IRQ` matters: Rockbox reads `CPU_HI_INT_STAT` directly, but Apple's dispatcher (`0x277128`) ignores the high bank unless low bit 30 is set. Without it every wheel event was taken and dropped by the guest.

## Status bits the firmware spins on

Each one is a silent infinite loop when missing.

| Register | Bit | Meaning | Waiter |
| -- | -- | -- | -- |
| `PLL_STATUS` `0x6000603c` | 31 | PLL locked | `system-pp502x.c` |
| `0x70000028` | 7 | USB block out of reset | `usb-fw-pp502x.c` |
| `GPO32_VAL` `0x70000080` | 14 | BCM powered (else Rockbox tries to upload `vmcs`, gives up, and drops every draw) | `lcd-video.c` |
| `IDE0_CFG` `0xc3000028` | 3 | `INTRQ`; a latch, W1C | `ata-pp5020.c` |
| `0x7000c104` | 26 | wheel packet ready | `button-clickwheel.c`, Apple `0x281350` |
| `CACHE_CTL` `0x6000c000` | 14 | `READY` |  |
| `I2C_STATUS` `0x7000c01c` | 6, 2:0 | `BUSY`, result code (Apple aborts if non-zero) | Apple `0x282f44` |
| PCF50605 `ADCS2` (`0x31`) | 7 | `ADCRDY`; no timeout | Apple `0x28341c` |
| `BCM_ALT_CONTROL` `0x30070000` | 7, 6 | busy / ready (BCM boot) | Rockbox `bcm_init()`, Apple `0x2876d0` |

## IDE

Standard ATA task file at 4-byte stride (QEMU's IDE core) plus a PP flat-buffer DMA engine:

| Register | Address | Function |
| -- | -- | -- |
| `IDE0_PRI_TIMING0/1` | `0xc3000000`/`+4` | PIO/DMA timings |
| `IDE0_CFG` | `0xc3000028` | bit 3 `INTRQ`, bit 15 DMA active |
| `IDE_DMA_CONTROL` | `0xc3000400` | bit 0 start, bit 1 arm, bit 3 direction (1 = to memory) |
| `IDE_DMA_LENGTH` | `0xc3000408` | bytes − 4 |
| `IDE_DMA_ADDR` | `0xc300040c` | physical address |

Address and length are set before the ATA command, the start bit after, so `start_dma` only stores the callback. Rockbox's bootloader reads with `READ MULTIPLE` (PIO), the app with `READ DMA`.

## I2C and the PCF50605

Not a bit-banger: slave address, up to 4 data bytes and a length, one start bit runs the transfer. Both firmwares poll; neither uses the interrupt.

| Register | Offset | Bits |
| -- | -- | -- |
| `CTRL` | `+0x00` | 7 `SEND` (self-clearing), 5 read, 2:1 length − 1 |
| `ADDR` | `+0x04` | `(addr7 << 1) \| R/W` |
| `ADDR2` | `+0x08` | Apple only: read-phase address of a combined write+read (`CTRL = 0x48 \| SEND`); the byte lands in `DATA1` |
| `DATA0..3` | `+0x0c`..`+0x18` |  |
| `STATUS` | `+0x1c` | 6 `BUSY`, 2:0 result code (0 = OK) |

The block must work uninitialised: Apple's reset routine (`0x282e98`) only runs after a timeout. PCF50605 at 7-bit `0x08`: register-pointer device whose pointer survives across transactions (both drivers rely on it). Modelled: register file, ADC (`ADCC2` `0x2f` bit 0 start, 4:1 input, 7 = 8-bit; result `ADCS1` `0x30` high 8, `ADCS2[1:0]`), RTC from the host clock. Not modelled: charging, power control (`GOSTDBY` ignored), regulators, alarm, interrupts. Apple also talks to `0x2a` (unnamed anywhere); the codec at `0x1a` is never accessed.

## DMA

Two identical blocks; block A is in no public source (Apple uses it to upload the BCM firmware).

| Block | Master | Channels | IRQ |
| -- | -- | -- | -- |
| A | `0x60008000` | `0x60009000 + n·0x20` | 27 |
| B | `0x6000a000` | `0x6000b000 + n·0x20` | 26 |

Master: `+0x00` bit 31 enable, `+0x04` `MASTER_STATUS` (Apple never reads it), `+0x08` request status. Channel: `+0x00` `CMD` (15:0 bytes − 4, 19:16 `REQ_ID`, 24 `WAIT_REQ`, 26 `SINGLE`, 27 `RAM_TO_PER`, 30 `INTR`, 31 `START`), `+0x04` `STATUS` (remaining, 30 `INTR`, 31 `BUSY`; **reading clears the interrupt**), `+0x10` `RAM_ADDR` (advances), `+0x14` memory-side descriptor, `+0x18` `PER_ADDR` (fixed port, inferred), `+0x1c` peripheral-side descriptor (width 30:28, 0 = 8 bit). `+0x08`/`+0x0c` unused by both firmwares.

## Broadcom BCM2722 and the VideoCore host interface

The panel is driven by the BCM, which runs its own VideoCore firmware (`vmcs`, 201 376 bytes) uploaded by the host.

| Register | Address | Function |
| -- | -- | -- |
| `BCM_DATA` | `0x30000000` | data port, access-width |
| `BCM_WR_ADDR` | `0x30010000` | 16-bit write pointer, loaded as two halfwords |
| `BCM_RD_ADDR` | `0x30020000` | read pointer; read bit 0 = ready for a new address |
| `BCM_CONTROL` | `0x30030000` | 1 write-ready, 4 read-ready, 6 booted, 7 busy; halfword `0x0031` = host→VC doorbell |
| `BCM_ALT_*` | `+0x40000` | second port, own pointers, same memory. Apple pushes pixels through it |

| Chip address | Meaning |
| -- | -- |
| `0x00000000` | SRAM, `vmcs` upload target |
| `0x000001F0` | handshake: host writes `{1, 0xA5A5}`, expects `{2, 0xA5A5}` |
| `0x000001F8` | `BCMA_COMMAND`; must read exactly 1 when done (`0x288080`) |
| `0x000001FC` | where the VC publishes its host-interface base |
| `0x000E0000` | Rockbox's pixel target; Apple never uses it |
| `0x10000C00` | VC start: write bit 31 releases, read bit 0 acks |

Host interface (`ipod_video_vc.c`): header of eight `u16` channel-block offsets (0 = absent), host signal/ack bytes at `+0x10`/`+0x11`, VC ack/signal at `+0x20`/`+0x21`; an event on channel *n* is bit *n* of `signal ^ ack`. Channel block (80 B): `+0x04` type (1 ASCII, 2 graphics, 5 VCFS, 6 VCHR, 7 ?), TX/RX ring start/end at `+0x06..+0x0c`, pointers at `+0x10` (TX read, VC), `+0x20` (TX write, host), `+0x30` (RX read, host), `+0x40` (RX write, VC). Usable capacity = end − start − 16. Message: 16-byte header (`0xF1A55A1F`, sequence, opcode/result, `u16` length, `u16` 0) + payload padded to 16. Length `0xFFFF` (NUL-terminated stream) is **not implemented**: it would never fit the ring.

Request/response needs no interrupt (every request is synchronous). VC-initiated traffic would need the BCM's interrupt line, which is unidentified; the VCIH task waits on an event nothing in the image writes.

Channel type 1 takes ASCII `gencmd` commands (20 from `vmcs.bin:0xBE24` plus 5 `pm_*` subcommands the retailOS sends top-level). Boot sends `set_vll_dir`, `display_control` ×2, `power_management set_policy`, `pm_set_policy min`; success needs at least one payload byte back. Channel type 2 is binary graphics RPC: opcode 8 creates a surface (format, w, h, stride, chip address; 0 = model picks, reply = handle + address), opcode 3 presents (handle, w, h); 1/2/4/9/16 pass tokens and are undetermined. The host pushes the pixels itself through the alt port into the surface address. Format codes are `VC_IMAGE_T` (1 RGB565, 5 RGB888, 15 RGBA32, 17 RGBA565, 18 RGBA16); this machine sees 1.

## Click wheel

Shares `0x7000c000` and IRQ 40 with I2C; interrupt-driven.

| Address | Function |
| -- | -- |
| `0x7000c100` | config (`opto_i2c_init` writes `0xc00a1f00`; Apple acks with `\|= 0x60000000`) |
| `0x7000c104` | status, bit 26 = packet; ack by writing `0x0c000000` (Apple: bit 26 only) |
| `0x7000c140` | `CLICKWHEEL_DATA` |

Packet: valid when `(d & 0x800000ff) == 0x8000001a` (Apple: mask `0xbc0000ff`); bits 8-12 SELECT, RIGHT, LEFT, PLAY, MENU; bit 30 touched; bits 16-22 position 0..0x5F, clockwise increments; 96 clicks per turn. Rockbox needs 4 clicks per item and resets the accumulator on direction change.

## Assumptions (choices, not data)

| Assumption | Choice | Basis |
| -- | -- | -- |
| DRAM size | 64 MB | Rockbox reads 64 from silicon; Apple's internal default and iPodLinux (`hw_ver 0xb`) say 32. Both boot (`ipod_video.h`) |
| SysInfo block location | `0x4001fe00` | ours; the pointer target is unknown without a ROM dump. Only 4 fields written; `sdram_size` lives in an iPodLinux-undecoded pad |
| `BCM_VC_DONE = 1` for any finished command | reuse | derived for the boot handshake only |
| I2C NAK result code | 1 | placeholder; codes 1..6 undetermined |
| PCF50605 ADC | instant conversion | firmware spins on ready |
| VC host-interface base, BCM SDRAM size (2 vs 4 MB) | `BCM_IFACE_BASE`, ours | `UNDETERMINED` |
| Apple ADC value 390, PCF50605 ID `0x00` |  | `ASSUMED` |

## Known deviations from hardware

- `TIMERn_VAL` reads 0. No interrupt priority or FIQ (FIQ reads 0).
- GPIO raises no interrupts.
- DMA ignores peripheral request lines (`WAIT_REQ` channels stay idle) and takes access width from the peripheral-side descriptor only.
- The wheel never reports "untouched" once touched.
- Remap `mask` and permission flags ignored.
- IDE is not migratable; no snapshot wiring on this machine.
- The BCM starts with the VideoCore already running (like `GPO32` bit 14); its firmware is never executed.
- `LCD_UPDATERECT` rect ignored, full repaint.
- No audio (I2S, WM8758), no USB, no mailbox.

## Open questions

- What bit 26 means to the real memory controller (the model ignores it; `INFERRED`).
- Why the wheel does not move the highlight on the retailOS language screen (0 px measured; works in the main menu). Undiagnosed.
- `CTRL` bits 6 and 3 separately, I2C result codes 1..6, the chip at `0x2a`.
- DMA descriptor bits 26:24; whether hardware clears `START` on completion.
- The BCM interrupt line; opcodes 1/2/4/9/16; channel type 7; `0x10001400`.

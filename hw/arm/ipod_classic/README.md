# iPod classic 6G (S5L8702) — `-M iPod-Classic`

Reference for the 6G machine: hardware, device models, env knobs, snapshot contract. Task steps for debugging a
guest: `docs/how-to/debug-a-classic-guest.md`. Guest VAs are 2.0.5 runtime addresses; `file_off = VA - 0x07FF5068`. Register-level detail and its evidence live in each device's
header under `include/hw/arm/ipod_classic/`. Firmware format and the real device: `docs/classic6g-firmware.md`.

## Board

| Part | Chip | Notes |
|---|---|---|
| SoC | Samsung S5L8702 (Apple `337S3473`) | ARM926EJ-S, 216 MHz |
| SDRAM | K4X51163PE | 64 MB |
| NOR | SST25VF080B, SPI0 | 1 MiB: SysCfg, NOR boot IM3, `flsh` files (`docs/classic6g-firmware.md`) |
| PMU | NXP PCF50635, I2C0 `0x73` (8-bit `0xE6`) | also RTC, ADC, backlight, hold switch (GPIO2) |
| Codec | Cirrus Logic CS42L55, I2C0 `0x4A` (8-bit `0x94`) | reset on GPIO 3.3 |
| "Mikey" | I2C0 `0x39` (8-bit `0x72`) | headphone mic / inline remote, not the wheel; not modelled |
| Storage | 1.8" PATA HDD, ATA at `0x38700000` | CE-ATA over SDCI is the other strap (`PDAT(11) & 2`); only PATA is modelled |
| Panel | 320×240 RGB565, MPU/8080 interface | four panel types, `PDAT6[5:4]` (`LCDTYPE`) |
| Charger | LTC4066 | |

Clocks: OSC0 12 MHz, OSC1 32768 Hz (from the PMU), ECLK 12 MHz (timer reference). Rockbox's plan: CClk 216,
HClk 108, PClk 54, SM1Clk 27 MHz.

## Memory map

Sources: Rockbox `firmware/export/s5l87xx.h` preprocessed for `CONFIG_CPU==S5L8702`, freemyipod, and Apple's
code for the blocks neither names. Several `s5l87xx.h` bases are S5L8700/8701 only; the table gives the 8702 value.

| Region | Base | Size |
|---|---|---|
| SDRAM (cached) | `0x08000000` | 64 MB |
| SDRAM (uncached alias, bit 31) | `0x88000000` | 64 MB. The guest programs DMAC0 with the cached view and runs on the uncached one, and hands SHA-1 uncached addresses |
| IRAM0 | `0x22000000` | 128 KB; also aliased at `0x0`, where the ARM926 takes exceptions |
| IRAM1 | `0x22020000` | 128 KB (64 KB on the S5L8720); last 256 B at `0x2203FF00` = boot info block |
| Bootrom | `0x20000000` | 50 or 64 KiB (unreconciled); locked after boot, no dump exists |
| Peripheral window | `0x38000000`..`0x3FFFFFFF` | the retailOS maps all of it uncached, so an unmapped address is a data abort: an `-d unimp` floor sits under the whole window |

| Base | Block | IRQ | Model |
|---|---|---|---|
| `0x38000000` | SHA-1 | 40 | `ipodtouch.sha1` (same register map); `SHA1_OFF` = stub |
| `0x38100000` | MIU | | unimp |
| `0x38200000` | DMAC0 (PL080) | 16 | `hw/dma/pl080.c`, 4 KB window, system memory as master port |
| `0x38300000` | LCD interface + panel | 14 | `ipod_classic_lcd.c` |
| `0x38400000` | USB OTG (DWC) | 19 | unimp |
| `0x38500000` | SM1 audio engine | 18 | `ipod_classic_sm1.c` (below) |
| `0x38700000` | ATA | 29 | `ipod_classic_ata.c`: Apple's bridge (a register read starts a cycle; result in `ATA_PIO_RDATA`), PIO + flat DMA |
| `0x38900000` | display pipe / compositor | | `ipod_classic_disp.c`. Not in any public source; map from Apple's code (`ipod_classic_disp.h`) |
| `0x38A00000` | NAND/FMI (`ATA_UNKNOWN_BASE`) | 20 | unimp; `+0x2000` NAND ECC |
| `0x38B00000` | SDCI (CE-ATA) | 44 | unimp |
| `0x38C00000` | AES (bus master) | 39 | unimp |
| `0x38E00000` | VIC0 / VIC1 (`+0x1000`), PL192 | | `hw/intc/pl192.c`; the retailOS dispatcher at `0x22004DA0` reads both banks with no cross-condition |
| `0x38E02000` | edge IC | | `ipod_classic_edgeic.c`: per-source rising-edge latch, `+0x4` enable, `+0xc` W1C; bit 3 = video decoder done → VIC1 line 3 (35) |
| `0x39000000` / `0x3F000000` | CalmADM2E DSP SFR / data | | unimp. The retailOS never touches either (its `0x3F000000` literals are `0.5f`) |
| `0x39300000` | display/scaler output (SDO) | 30 | `ipod_classic_sdo.c`: frame-done edge, `+0x280` ack, `SDO_FRAME_IRQ=1` (default off) |
| `0x39600000` | JPEG decoder | | not mapped (floor) |
| `0x39800000` | video decoder | 35 | `ipod_classic_vdec.c`, `docs/video-decoder.md` |
| `0x39900000` | DMAC1 (PL080) | 17 | `hw/dma/pl080.c` |
| `0x39A00000` | EIC (7 groups × 32; groups → IRQ 0-3, 31-33) | | unimp; the guest programs it, nothing fires |
| `0x3C100000` | RNG (inferred) | | `ipod_classic.c`: LCG, `+0` ready `[2:0]` (no enable bit: gating it hangs), `+4` data, `+8` seed. Only reached diskless |
| `0x3C200000` | click wheel | 23 | `ipod_classic_wheel.c` |
| `0x3C300000` | SPI0 (NOR) | 9 | not mapped (floor); SPI1 `0x3CE00000`, SPI2 `0x3D200000` |
| `0x3C400000` | USB PHY | | unimp |
| `0x3C500000` | clock / PLL | | `ipod_classic_clk.c`: `PLLLOCK` reports enabled PLLs locked, `SWRCON` reboots, sub-word access |
| `0x3C600000` | I2C0 | 21 | `ipod_classic_i2c.c` + PMU + CS42L55. I2C1 at `0x3C900000` (22), not modelled |
| `0x3C700000` | timers | 8 (A-D), 7 (E-H) | `ipod_classic_timer.c`, interval mode only |
| `0x3C800000` | WDT | | unimp |
| `0x3CA00000` | I2S0 | | `ipod_classic_i2s.c` (4 KB window); I2S1 `0x3CD00000`, I2S2 `0x3D400000` |
| `0x3CB00000` | S/PDIF | | unimp |
| `0x3CC00000` | UART, 4 ports, stride `0x4000` | 24-27 | port 0 only, `hw/char/exynos4210_uart.c`; on the dock connector (iAP by default) |
| `0x3CF00000` | GPIO, 16 groups × 8, stride `0x20` | | `ipod_classic_gpio.c`: pin 86 = media present (`GPIO_MEDIA`), `LCDTYPE` straps; rest read 0 |
| `0x3D100000` | chip ID | | unimp |

The table's "unimp" entries are `create_unimplemented_device()` windows; "floor" is the window-wide
`s5l8702.io` region. Both log under `-d unimp` and read 0. The `ipod_classic.h` defines for `0x3CD00000`
(SPI), `0x3CE00000` (ADC), `0x3D200000` (RTC), `0x39400000` (FMC), `0x38600000`/`0x38800000` (USB HC/FC) and
`0x39C00000` (INT) are S5L8700/8701 names; on the 8702 those addresses are SPI1/SPI2/I2S1 or nothing.

Boot without a bootrom: `-kernel` loads the raw `osos` at `0x08000000` and the machine writes the boot info
block (`BOOTINFO`, `FWGUID`). For a Rockbox app image it also reproduces the bootloader hand-off
(`ipod_classic_handoff`): SP at the top of IRAM0 and timer E at 1 MHz, since `crt0.S` calls `memory_init` before
setting stacks and `udelay` spins on `TECNT`. Semihosting is on by default: the retailOS prints its panics through
it (stderr).

Not modelled: USB, NAND/ECC, CE-ATA/SDCI, AES, the DSP, EIC, SPI/NOR, S/PDIF, WDT, chip ID, Mikey, I2C1, UARTs
1-3, the bootrom.

### Open hardware questions

| Question | Status |
|---|---|
| Bootrom size (50 vs 64 KiB) and the register that remaps `0x0` (ROM → IRAM0) | unknown; the model keeps the IRAM0 alias at 0 unless `-M ...,bootrom=` is given |
| What GPIO pin 86 is on the board | its role in Apple's firmware is verified, the board net is not |
| `WHEELCON` bits, `WHEELDIV` `0x3A980`, `WHEELPEND`/`WHEELMASK` bits 1-2, why both `WHEELTX` encodings work | unknown, not needed |
| LCD `+0x7C`, `+0x88`; whether `+0x70` latches a frame or enables the scan; `LCD_STATUS` bits 2 and 8 | unknown |
| The rest of the `0x38900000` map | only what the image and `DISPTRACE=1` show |
| Part numbers of the four panels; UART2/3 pin mux; a PKE block at `0x3D000000` | unknown |

## Audio / display models

| Area | File | What it does | Default |
|---|---|---|---|
| SM1 frame decoder | `ipod_classic_sm1.c`, `ipod_classic_sm1_dec.c` | The engine's sub-block 4: consumes the compressed frame the guest submits (chain at `+0x100`, descriptor `{0, len<<16\|flags, buf}`), decodes it with libavcodec (AAC today; the enable word `0x2203ff00 & 7 == 7`, rate index at `+6`), writes PCM and state into the IRAM1 output-slot ring (hdr `0x2203ff28`, slots `0x22038000`: count `+2`, stride halfwords `+4`, per-slot state/size `+0xa`/`+0xc`; buffers `0x22038100`) and holds VIC line 18 until the guest acks via `+0xc48`. Needs libavcodec at build time (`-Dsm1_decoder=auto`). | on (`SM1_DECODE=0` off) |
| SM1 engine state | `ipod_classic_sm1.c` | DEV TOGGLE: publishes invented engine geometry (descriptor + slot array + "ready" halfwords) into IRAM1. Writes no PCM. | off (`SM1_SLOTS=1`) |
| Video decoder | `ipod_classic_vdec.c`, `ipod_classic_vdec_dec.c` | `0x39800000`, the video analogue of SM1. Latch-only register file (`+0xcc/d0/d4` current YUV, `+0xdc` descriptor table, `+0xe8` start `0x88003001`, `+0xf0/f4` status) by default. `VDEC_DECODE=1` reads the AU + avcC from the pctrace `audump=` files, decodes the whole AU with libavcodec H.264, writes YUV420 into `+0xcc/d0/d4`, sets `+0xf0` bit 0 and pulses done. See `docs/video-decoder.md`. | on (stub; decode off) |
| Edge interrupt controller | `ipod_classic_edgeic.c` | `0x38e02000`, behind the two VICs: latches a rising edge per source, `+0x4` = enable, `+0xc` = write-1-to-clear. Bit 3 carries the video-decoder done line to VIC1 line 3 (id 35); the guest's handler acks it at `+0xc`. Without it a held device line re-entered the handler for ever. | on |
| SM1 interrupt | `ipod_classic_sm1.c` | Raises VIC0 line 18 (frame-done report) on the guest's arming write. Model approximation. | unset = guest-armed (`SM1_IRQ=1`); `0` off |
| I2S paced drain | `ipod_classic_i2s.c` | Drains the audio DMA to the I2S FIFO in real time, which is what advances the audio clock and the SM1 stream position. | unset = on (`AUDIOPACE=0` off) |
| Compositor | `ipod_classic_disp.c` | Front-to-back `source-over` composition with the 32-bit layer's alpha channel. | unset = on (`DISP_ALPHA=0` off) |
| LCD frame IRQ | `ipod_classic_lcd.c` | Pulses the frame-done interrupt at each frame. Partial: completes the NP entry animation, can starve the audio pump. | unset = off (`LCD_FRAME_IRQ=1` on) |
| PMU charger | `ipod_classic_pmu.c` | Reports a charger present so retailOS does not show Low Battery. Model choice. | unset = wall adapter (`PMUCHARGER=usb` / `none`) |

The retailOS does not decode audio in software: it hands each compressed frame to the SM1 hardware and reads PCM
back from IRAM1, so the SM1 model does that step and the guest's own audio path, effects and output queue run
unchanged. Codecs other than AAC are not wired (the guest's enable word selects them). Dev aids:
`SM1_DEC_DUMP=file` (decoded s16le), `SM1_DEC_DUMP_FRAMES=file`.

The defaults are the working combination: paced I2S, source-over compositing, wall adapter.

## Snapshots (savevm / loadvm)

The machine supports QEMU internal snapshots on a qcow2 overlay: `savevm` captures RAM, every device's vmstate
and the guest's disk writes atomically, and a fresh process restores it with `-loadvm`. Steps:
`docs/how-to/debug-a-classic-guest.md`.

| Rule | Detail |
|---|---|
| Same env on restore | `SM1_IRQ`, `AUDIOPACE`, `DISP_ALPHA`, `LCD_FRAME_IRQ`, `SHA1_OFF`, `PMUCHARGER`, `PMUHOLD` and the SM1 slot knobs change device behaviour or topology; restore with the values used at save. An exported-but-empty knob still counts as set for presence switches (`SHA1_OFF`) |
| ATA DMA window | `dma_cb` is a host pointer latched between the ATA command and the go-bit write; `pre_save` refuses a snapshot in that window (a few instructions). In-flight block AIO is drained by `savevm` |
| PMU RTC | `rtc_guest_ms` is saved and re-based in `post_load`, so the guest clock continues on virtual time (default `-rtc clock=host` only) |
| IRQ lines | Not migrated; every device re-derives its line in `post_load` (PL192 `pl192_update`, timer/I2C/LCD/SM1/ATA `update_irq`, the wheel, the UART from `UINTP`) |
| PL192 daisy chain | `daisy_callback` is rebuilt from the fixed `daisy_source` (vic0 ← vic1) |
| iPod-Video | No snapshot wiring |

## Environment knobs (each disables/tunes one thing)

| Var | Meaning |
|---|---|
| `SM1=0` | master off: SM1 blocks back to the register-only model |
| `SM1_SLOTS=1` | DEV TOGGLE: publish invented engine geometry in IRAM1 (writes no PCM) |
| `SM1_NSLOTS=N` | slots published (default 2; 7 of 2048 frames claims 28 KiB and stomps IRAM1) |
| `SM1_IRQ=0/1/2` | 0 off, 1 guest-armed (default), 2 pulse regardless of arming |
| `SM1_IRQ_EVERY=N` | 5 ms ticks between reports (default 5) |
| `SM1_POS=N` | override the position ring wrap (0 pins at the ring base) |
| `SM1_FRAME_BIT=1` | also report status bit 12 while a report is pending (experiment) |
| `SM1TRACE=1`, `SM1HI=1` | per-access log / count writes above the modelled 4 KiB page |
| `VDECTRACE=1` | log every `0x39800000` video-decoder register access (addr, value, guest PC) |
| `VDEC_DECODE=1` | DEV SHORTCUT: decode the tapped AU with libavcodec (needs `VDEC_AU_FILE`/`VDEC_CFG_FILE`); default off |
| `VDEC_AU_FILE`, `VDEC_CFG_FILE` | access unit / avcC files written by the pctrace `audump=` tap |
| `AUDIOPACE=0` | un-paced I2S drain (baseline) |
| `AUDIOWAV=/path.wav` | capture every PCM write to the I2S FIFO |
| `AUDIOLIVE=1`, `I2SCOUNT=1`, `I2STRACE=1` | SDL audio out / write census / per-register trace |
| `DISP_ALPHA=0` | back-to-front opaque composition (old order) |
| `LCD_FRAME_IRQ=1` | pulse the LCD frame-done line |
| `LCD_DISPON=0` | require the guest's own 0x29 before the panel shows (default on) |
| `PMUCHARGER=usb` / `none` | USB power / battery only (unset or `adapter` = wall adapter). Any other value means USB; on 2.0.5 USB puts up Low Battery after ~2 min of Now Playing |
| `PMURTC=1` | log every PMU RTC block read (guest date/time + virtual ms) |
| `DISPLOG=1` | log each display source-address change (`s5l8702.disp: SOURCE ADDRESS ...`; default off) |
| `PMUHOLD=on` | hold switch engaged, drops every button (default off) |
| `PMUOOCSTAT=raw` | report stored on/off-controller status, not forced healthy (default healthy) |
| `GPIO_MEDIA=0/1` | pin 86 media-present level (default: a -drive is attached) |
| `PMUBATT`, `PMUTRACE`, `LCDTRACE`, `DISPTRACE`, `ATALBA`, `ATATRACE`, `TIMERTRACE`, `WHEELTRACE`, `GPIOTRACE`, `BOOTINFO`, `IRAMNOISE`, `IRAMSPILL`, `FWGUID`, `SHA1_OFF`, `LCDTYPE` | bring-up / driver / debug knobs, unchanged |

## Clock / PLL block

`ipod_classic_clk.c` accepts 1/2/4-byte accesses with sub-word shift/mask (`8 * (offset & 3)`): Rockbox's
`cg16_config()` read-modify-writes a 16-bit clock gate with a 32-bit access and polls it with `ldrh`
(`0x3c500012`, `CG16_RTIME`).

## Key guest addresses

| Address | Meaning |
|---|---|
| `0x38500000` | SM1 block; `+0xc80` identity, `+0xa98` bit2 status, `+0x400` run, `+0xa8c` int enable, `+0x110/+0x114` sub-block cmd/state, `+0xa48` stream position |
| `0x22006118` | IRAM1 render pump (dpump) |
| `0x22008c58` | render-credit event the pump waits on |
| `0x2203ff28` / `0x22038000` / `0x22038100` | engine descriptor / slot array / PCM ring in IRAM1 |
| `0x08a873c0`, `0x08a8b3c0` | the two codec output buffers |
| `0x08017378` | VIC0 line-18 handler that signals the credit event |
| `0x0808170c` | engine render entry |
| `0x081db4c8` | two-buffer dispenser the pump reads from |
| `0x08150d84` | `StartPlayback` (media player bring-up) |
| `0x0815d814` | decoder producer: copies PCM into the player ring and adds the byte count to `[player+0x33c]` |
| `0x08ada288` | media player object (`DAT_0815d4a8`); `+0x1c` playing flag, `+0x330/+0x334/+0x338` ring indices, `+0x33c` cumulative PCM bytes |
| `0x0802c03c` | signed divide (used by the time formatter) |
| `0x08199834` | `h:mm:ss` / `m:ss` formatter (takes seconds) |
| `0x0819a574` | elapsed/remaining getter; formats `[player+0x33c]` into the view, gated by `[view+0x8a]` |
| `0x08199a30` | activates the time view (`[view+0x8a]=1`) |
| `0x0be5c820` | the Now Playing time view (heap; `+0x8a` active, `+0xd0` extra counter) |

## I2C / PMU

The S5L8702 I2C controller (`ipod_classic_i2c.c`) is the master for the PCF50635 PMU (`ipod_classic_pmu.c`, 7-bit
`0x73`, also the RTC) and the CS42L55 codec (`0x4A`). Contract the guest relies on:

| Behaviour | Why |
|---|---|
| STOP (`IICSTAT` `0xd0`) ends the transaction and delivers `I2C_FINISH` to the slave | The keycode dispatcher reads the hold switch (PMU `0x87`) as set-pointer, STOP, one-byte read; the PMU must see the boundary. BUSY is owned by the start/stop encodings, not copied from the written value |
| Only a fresh `IICDS` write or an unread received byte starts a transfer | The ISR acks with `IICCON = IICCON`; treating that as GO advanced the PMU's shared register pointer |
| Byte-done is a level, held until the driver consumes the byte (next byte, `IICDS` read, or STOP) | The ISR acks each byte by clearing `IICSTAT2`; an emulated ISR can outlast `S5L8702_I2C_BYTE_US` (25 µs), and an edge would be lost |
| The PMU's register pointer persists across transactions, with a restore-at-STOP compensation | See the note above `ipod_classic_pmu_recv` |
| The PMU reports a cold boot (`OOCSHDWN` `0x0c` bit 3 `COLDBOOT` at reset) | Rockbox's `pmu_is_hibernated()` is `!COLDBOOT && !GPIO3CFG`; clear, it resumes via `launch_onb()` into the unmodelled NOR. `pmu_preinit()` clears it |

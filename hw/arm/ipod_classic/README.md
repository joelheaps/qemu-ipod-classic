# iPod classic 6G (S5L8702) — `-M iPod-Classic`

Hardware reference, device models, env knobs and guest debugging. Guest VAs are 2.0.5 runtime
addresses; `file_off = VA - 0x07FF5068`. Register-level detail and its evidence live in each device's
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

The old host-side audio substitutes (ffmpeg FAT decode, the I2S data-path feed,
the SM1 slot fill and the HLE_NP bootstrap) were REMOVED: they bypassed the guest.
The retailOS does not decode in software: it hands each compressed frame to the
SM1 hardware and reads PCM back from IRAM1, so the SM1 model above does that
decode step (the guest's own audio path, effects and output queue run unchanged).
Codecs other than AAC are not wired yet (the guest's enable word selects them).
Dev aids: `SM1_DEC_DUMP=file` (decoded s16le), `SM1_DEC_DUMP_FRAMES=file`. Storage
mounting, UI navigation, the compositor, input, SHA-1/library hashing and
Rockbox are unaffected.

Working combination, no env needed: `AUDIOPACE=on` + `DISP_ALPHA=on` +
wall adapter (`PMUCHARGER` unset).

## Snapshots (savevm / loadvm)

Boot + key navigation to Now Playing takes minutes and is flaky (the retailOS drops SELECT);
save that state once and restore it in ~4 s instead. The recipe below is an external script (`snapshot.py`, `save|load|verify`, not in this tree). It uses a **qcow2 overlay** (raw `Emedia.img` as backing file) +
QEMU internal snapshots (`savevm` / `-loadvm`), not migration-to-file, because the overlay makes
RAM + every device's vmstate + the guest's disk writes one atomic snapshot, and a fresh process can
`-loadvm` it. The overlay is a normal qcow2 file; `snapshot.py` copies it per load
so restores cannot leak writes into each other (needed for pctrace determinism).

| Task | What it does |
|---|---|
| `snapshot.py save` | Boot, navigate (navigate.py), `savevm snap` into `$OUT/snap.qcow2`; keeps `$OUT/frame.ppm` as the reference frame and `$OUT/config.json` with the env knobs |
| `snapshot.py load` | Fresh `-loadvm snap` (paused), pixel-compare against `frame.ppm`, then a key press to prove input works |
| `snapshot.py verify` | Two restores with pctrace; asserts the logs match (host timestamps stripped) |

Env: `OUT` (default `$SCRATCH/runs/snapshot`), `SNAP` (name, default `snap`), `OSOS`, `DISK`,
`FWGUID`, `PLUGIN` (pctrace spec for load/verify), plus the device knobs in the table below.
`snapshot.py load|verify` refuses to run when a recorded device-behaviour knob differs from the save
(the machine topology would silently change); rerun with the same env. Exported-but-empty knobs are
dropped before QEMU launches, so a task's `VAR="${VAR:-}"` forwarding cannot flip a
`getenv()`-presence switch (e.g. `SHA1_OFF`).

Honest gaps (fail loudly rather than restore wrong state):

- **ATA DMA callback.** `dma_cb` is a host function pointer latched when the guest writes the ATA command and
  fired by the later go-bit write. No stable representation exists, so `pre_save` refuses a snapshot taken in
  that window (a few instructions). Stable states have it NULL. In-flight block AIO is drained by `savevm` itself.
- **PMU RTC.** The guest clock follows host wall time. `rtc_guest_ms` is captured at save and the bases are
  re-based in `post_load`, so the clock continues from the saved value on virtual time only. This rebase
  covers the default `-rtc clock=host` path; with `-rtc clock=vm` the UART/rtc path bypasses it (not used by
  the wrapper).
- **Env-dependent devices.** `SM1_IRQ`, `AUDIOPACE`, `DISP_ALPHA`, `LCD_FRAME_IRQ`, `SHA1_OFF`, `PMUCHARGER`,
  `PMUHOLD` and the SM1 slot knobs change device behaviour/topology; the wrapper records and checks them.
- **Sysbus IRQ lines are not migrated.** Every device that drives one re-derives it in `post_load`
  (PL192 calls `pl192_update`; timer/I2C/LCD/SM1/ATA re-run their `update_irq`; the wheel recomputes; the
  exynos UART is re-driven from the restored `UINTP` and no longer arms a spurious Rx timeout).
- **PL192 daisy chain.** `daisy_callback` is a runtime back-pointer; it is rebuilt from a fixed
  `daisy_source` the machine sets (vic0 <- vic1), so the pointer itself is not in the stream.
- **pctrace timestamps.** Log lines carry host-monotonic µs, which cannot match across processes;
  `snap-verify` strips that field and compares the PC sequence and hit summary.
- **iPod-Video** has no snapshot wiring (not done; Classic only).

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
| `PMUCHARGER=usb` / `none` | USB power / battery only (unset or `adapter` = wall adapter). Any other value means USB; on 2.0.5 USB put up Low Battery after ~2 min of Now Playing (`npqemu`) |
| `PMURTC=1` | log every PMU RTC block read (guest date/time + virtual ms) |
| `DISPLOG=1` | log each display source-address change (`s5l8702.disp: SOURCE ADDRESS ...`; default off) |
| `PMUHOLD=on` | hold switch engaged, drops every button (default off) |
| `PMUOOCSTAT=raw` | report stored on/off-controller status, not forced healthy (default healthy) |
| `GPIO_MEDIA=0/1` | pin 86 media-present level (default: a -drive is attached) |
| `PMUBATT`, `PMUTRACE`, `LCDTRACE`, `DISPTRACE`, `ATALBA`, `ATATRACE`, `TIMERTRACE`, `WHEELTRACE`, `GPIOTRACE`, `BOOTINFO`, `IRAMNOISE`, `IRAMSPILL`, `FWGUID`, `SHA1_OFF`, `LCDTYPE` | bring-up / driver / debug knobs, unchanged |

## Debugging a running guest (verified on 2.0.5 + `Emedia.img`)

| Topic | Fact |
|---|---|
| Boot | A full boot needs `-drive if=ide,format=raw,file=assets/disks/Emedia.img` and `FWGUID=000A270021574219`. `-kernel` without the disk reset-loops (`SWRCON=0xaa5`, 6 resets in 25 s). |
| Address spaces | CPU loads the image at physical `0x08000000`; the boot copy shifts it **down** `0xAF98`, so the resident image runs at `VA == physical` and gdb, `os.dis`, Ghidra and pctrace label `file_off k` as `k + 0x07FF5068` (= resident VA for `k ≥ 0xAF98`). The file's first `0xAF98` bytes are unmapped. Vectors at `0x0` and IRQ-path code run from the LOW IRAM0 alias `0x0..0x20000` (alias of `0x22000000`, 128 KiB): a breakpoint at `0x18` hits, one at `0x22000018` never does. Use the low address for anything on the exception/IRQ path. |
| Not modelled | NOR flash, bootrom (unless `-bootrom`), SPI: Apple OF/ONB/DFU paths cannot be rehearsed. |
| Display source | `s5l8702.disp: SOURCE ADDRESS +0xOFF = 0xADDR (pc~..)` is logged to stderr, only with `DISPLOG=1`, on every CHANGE of a source register (layer base moves, double-buffer flips; ~16 lines in 40 s of menu use). Live value from the monitor in the same run: `xp /wx 0x38900060` (slot 0) / `0x38900078` (slot 1) / `0x38900090`, `0xa8`, `0xc0` (slots 2-4); planes at `+0x38..+0x44`. Slot 0 flips between two buffers; read it right after the event you care about. `DISPTRACE=1` logs every register access. |
| Monitor memory | `xp` physical, `pmemsave` physical, `memsave` virtual. The boot copy is a **down-shift by 0xAF98**, so at runtime `VA == physical` and the file's first 0xAF98 bytes are unmapped: `memsave 0x07ff5068` → Invalid addr, `pmemsave 0x07ff5068` → zeros. Read a resident image word at `memsave (0x08000000 + file_off − 0xAF98)`. **Filenames:** HMP parses `SIZE` as an expression, so an unquoted absolute path makes `SIZE /abs…` a division: `pmemsave ADDR 16 /abs/path` → `invalid char 'h' in expression` and **no file**. `pmemsave ADDR 16 "/abs/path"` (double quotes) writes the absolute path; a plain relative name is written relative to QEMU's **cwd**, not the monitor socket's dir. `rehearse:rolo`'s `DUMP=` quotes for you. |
| Disassembly | No `x/i` (built `--disable-capstone`: "Asm output not supported"): use `assets/cache/disasm/os.dis` or Ghidra `/iram.bin`. |

gdb, one-shot and non-interactive (QEMU started with `-gdb tcp::17xx`, no `-S`, attached any time later):

```
gdb-multiarch -nx -batch -ex 'set arm abi AAPCS' -ex 'target remote :17xx' \
  -ex 'break *0x08069a5c' -ex continue -ex 'info registers' -ex bt -ex detach
```

- Hit on `fill_rect` (`0x08069a5c`) 30 s into a boot; after `detach` the guest keeps running and a second attach/break works. No reset, no `SWRCON`.
- Deterministic frozen recipe (when the attach race bites): start QEMU with `-S -gdb tcp::17xx`, attach, set the breakpoints, `continue`, *then* navigate; the breakpoints exist before the first instruction. A breakpoint on the input path can still eat a key, so for a redraw capture navigate first and attach after.
- `Cannot execute this command while the target is running` means `continue` never stopped: the breakpoint address is never executed (wrong alias, e.g. `0x22xxxxxx` for IRQ code, or a cold function). Not a QEMU bug; wrap in `timeout` and pick a hotter/low-alias address. No extra `set non-stop` needed.
- `bt` shows 2 frames, then "previous frame identical (corrupt stack?)": the guest's frames are not APCS-walkable. Read `$lr`/the stack with `x/32wx $sp` or use the pctrace plugin for call chains.
- pctrace `regpc=`/`sendex=`/`memr=`: a PC that lands mid-TB is skipped (read refused, warned once, counted as `[mid-TB skipped=N]`), so hits undercount; probe a TB head instead. In `memr=ADDR:REG:OFF:LEN:LABEL`, REG is a gdb core-register **index** (0..12=r0..r12, 13=sp, 14=lr, 15=pc), not a base-register value; the read is `[reg REG + OFF]`. Plugin path: `navigate.py.py --plugin` resolves a relative `.so` against the repo root.
- Guest timers are virtual, so a stop is invisible to the guest.
- Adding `-gdb tcp::17xx` to a `-kernel` boot is safe (a 45 s boot reaches the Language screen); if QEMU is `Killed` during a long run, suspect host memory/dmesg or a port clash, not the stub ([clicker-audio]).

## Recipes

Containers don't share the host's `127.0.0.1`: run QEMU and its client (emulator.py, gdb) in the same `mise run sh -- bash -c '...'`. Use your own ports/sockets (the live window owns `live.sock` and gdb `:1405`). Scratch under `$SCRATCH` (`assets/scratch`).

| Goal | Command | Checked |
|---|---|---|
| Headless boot | `T=30 mise run emulator:boot` (`OSOS=image`; extra args go to QEMU, e.g. `-d in_asm -D log`); add `DISPLOG=1` for display source lines | yes |
| One screenshot | monitor `screendump $SCRATCH/screens/NAME.ppm` after boot | read only |
| Press keys, verify the screen | monitor `sendkey` then `screendump`; a press is one `sendkey`, no wake or re-send | yes |
| Capture audio output | the "Press keys" recipe with `AUDIOWAV=$SCRATCH/runs/out.wav` prefixed to the `build/qemu-system-arm` command, then press into a song | flag exists; no PCM is produced yet so output unverified |
| Attach gdb | `mise run sh -- bash -c 'T=40 tools/ipod_classic/boot205.sh -display none -gdb tcp::1701 >/dev/null 2>&1 & sleep 25; gdb-multiarch -nx -batch -ex "set arm abi AAPCS" -ex "target remote :1701" -ex "break *0x08069a5c" -ex continue -ex "info registers pc" -ex detach'` (recipe above; `-S` freezes at start) | yes |

## Clock / PLL block (boot path)

`ipod_classic_clk.c` accepts 1/2/4-byte accesses and applies sub-word
shift/mask (`8 * (offset & 3)`). Rockbox's `cg16_config()` does a 32-bit
read-modify-write of a 16-bit clock gate and then polls it with an `ldrh`
(`0x3c500012` = `CG16_RTIME`); with 4-byte-only accesses every poll was a data
abort and the bootloader hung (first in `usec_timer_init()`, before its LCD came
up).

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

## Elapsed time — open

The displayed elapsed is `FUN_0819a574` → `FUN_08199834` over a value that comes
from `FUN_081a67e4` → `FUN_0815d440` → `[0x08ada288+0x33c]`. That counter is
advanced only by the guest decoder's producer `FUN_0815d814`, which never runs
because the MeCCA playback start does not complete (`StartPlayback` = 0, the
player object stays zero). The removed host HLE used to carry audio downstream of
the decoder but never that accounting, so it did not make this advance either.
There is a second gate: the time view is
inactive (`[view+0x8a]=0`) and is not re-registered, so it does not repaint. The
progress bar reads the same counter. Making it advance requires the player
accounting (and the view activation) to run; that is the remaining gap.

## I2C / PMU

The S5L8702 I2C controller (`ipod_classic_i2c.c`) is the master for the PMU
(`ipod_classic_pmu.c`, PCF50635) and the RTC. Apple's keycode dispatcher reads
the hold switch (PMU register `0x87`) as set-pointer, STOP, then a one-byte
read, so the controller must deliver `I2C_FINISH` at STOP or the PMU never sees
the boundary between those two transactions. Two controller bugs broke that:

- **STOP did not end the transaction.** The `IICSTAT` write copied bit 5 (BUSY)
  from the written value, but the start/stop encodings carry it themselves
  (`0xf0` start, `0xd0` stop). STOP therefore cleared BUSY before the STOP
  branch ran, so `i2c_end_transfer()` (and its `I2C_FINISH`) was never called
  and the bus stayed busy. The assignment now masks BUSY out; the start/stop
  cases own it.
- **The interrupt-ack write re-triggered a transfer.** The guest's I2C ISR acks
  by writing `IICCON = IICCON` while the byte-done flag is set; `go()` treated
  that as a fresh GO and did a second send/receive, overwriting `IICDS` and
  advancing the PMU's shared register pointer (the hold read got `0x88`, not
  `0x87`, and SELECT was dropped). `tx_valid`/`rx_valid` now gate GO: only a
  fresh `IICDS` write (or an unread received byte) starts work.

The PMU model also keeps its own restore-at-STOP compensation for the shared
register pointer (see the note above `ipod_classic_pmu_recv`); it is unchanged.

A third controller bug produced the residual loss, now fixed:

- **The byte-done flag was lost to the interrupt ack.** The ISR acks each byte
  by clearing `IICSTAT2`; a byte that completed while the previous byte's ISR
  was still running had its `BYTE` bit cleared by that ack, so the guest's
  per-byte state machine timed out (`i2c_read_reg` returns `0x60`) and the
  hold-switch read was dropped. The emulated ISR can take longer in virtual
  time than `S5L8702_I2C_BYTE_US` (25); a real 90 us wire byte makes the race
  impossible on hardware. `byte_done` is now a level held until the driver
  consumes the byte (starts the next one, reads `IICDS`, or stops), and the
  interrupt is held on it. See `update_irq`.

MEASURED (snapshot load, monitor `sendkey ret` x50, pctrace on `key_dispatch`
`0x08056900` and the `i2c_read_reg` return `0x082dc030`): edges dropped by the
hold gate before -> after the fix were `47/100 -> 0/100` at hold 50 ms,
`33/100 -> 0/100` at 150 ms, `37/100 -> 0/100` at 400 ms, with no `0x60`
timeouts after; MENU and SELECT each move the screen on the first press with no
wheel nudge. The earlier PMU register-pointer race is not observed after the
fix (`PMUTRACE` shows no read of `0x88`).

The PMU reports a cold boot: reset seeds `OOCSHDWN` (`0x0c`) `COLDBOOT` (bit 3),
as power-on silicon does. Rockbox's `pmu_is_hibernated()` is
`!COLDBOOT && !GPIO3CFG`, so with the bit clear it decides the machine is
resuming and runs `launch_onb()` before anything else, into the unmodelled
SPI/NOR. The driver's own `pmu_preinit()` clears it (writes `OOCSHDWN = 0`), so
it only steers that first branch.

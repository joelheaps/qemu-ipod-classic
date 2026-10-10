# S5L8702 video decoder (`0x39800000`) — reference

Long-term map of the retailOS video-decode path on the iPod classic 6G, and the
QEMU model. Terse facts + evidence.

## Verdict

Video decode is **not** done by the main ARM. The ARM parses the container,
SPS/PPS and slice headers and **offloads pixel reconstruction to a hardware
block at `0x39800000`**, the video analogue of the SM1 audio engine
(`hw/arm/ipod_classic/ipod_classic_sm1.c`). The front-end compacts the access
unit **in place** (NAL + slice headers stripped) before submit, so the block
only ever sees entropy payloads; SPS/PPS are parsed separately and never reach
it. The decoded YUV420 planes are handed to the **display pipe's planar-YUV
layer** (fmt 8), which converts them to the panel's RGB565 image.

The model (`hw/arm/ipod_classic/ipod_classic_vdec.c`) is a latch-only stub by
default; `VDEC_DECODE=1` turns on the libavcodec H.264 decode fed by the
pctrace `audump=` tap, and signals done on VIC1 line 3 (id 35) through the
edge-IC. See "Model" below.

## Hardware block (`0x39800000`)

Register map observed in the submit routine `FUN_081b65c4` (guest VAs; `base =
0x39800000`, held in decoder-object field `+0x38`):

| Offset | Use |
|---|---|
| `+0xcc/+0xd0/+0xd4` | current frame Y/U/V plane addresses (frame `+0x74/+0x7c/+0x80`) |
| `+0x00/+0x04/+0x08` | reference-frame Y/U/V triples |
| `+0xd8` | input/work buffer base (`[obj+0xc]`) |
| `+0xdc` | descriptor-table pointer (`[obj+0x1c]`), 8 words/slice |
| `+0xe0` | `(H_MB << 8) | W_MB` (6 bits each) |
| `+0xe4` | `([obj+0x184] << 16) | [obj+0x188]` = chroma/luma width (160/320) |
| `+0xe8` | `(slice_count << 16) | (2*W_MB*H_MB)`; `|= 0x88003001` = START (bit 0) |
| `+0xec` | context control; `FUN_081b6a64` clears bit 0 |
| `+0xf0` | status, bit 0 = done |
| `+0xf4` | status; success = bit 29 clear and `((v & 0x1fffffff) >> 18) == 0` |
| `+0x118` | constant result word `0x82625a00` |
| `+0x120/+0x124/+0x128` | reference frame Y/U/V; `+0x12c` = reference-set flag |

`FUN_081b6a14` zeroes `+0x00..+0x12c` at the start of every submit. The block is
fed stripped entropy payloads (record `word5 = [obj+0x14] + payload_off`,
`word6 = payload_len`); the full AU does not exist anywhere the block can read.

## Done IRQ / edge-IC

- Done is id 35: `irq_init(35)`, handler `0x0804b36c`, enable `0x0804b354`.
  id 35 = VIC1 bit 3 (`s5l8702_get_irq(nms, 35)`).
- Handler `0x0804b36c` acks the **edge-IC** at `0x38e02000+0xc` (writes 8) and
  signals event `0x08a123e4`; it never writes the vdec's `+0xf0`.
- `0x0804b354` sets bit 3 of `0x38e02000+0x4` (edge-IC source enable).
- `hw/arm/ipod_classic/ipod_classic_edgeic.c` models the edge-IC: latch a rising
  edge on bit 3, ack (write-1-to-clear) at `+0xc`, output = pending & enable to
  VIC1 line 3. Without it a held/level vdec line re-enters the handler for ever
  (measured storm: the guest spins in the ISR and never reaches the done poll).
- The vdec **pulses** its line on done; the edge-IC latches it.

## Model (`VDEC_DECODE=1`, default off)

The firmware destroys the AU in place before submit, so the block cannot see it.
The model is fed from files dumped by the pctrace `audump=` tap:

| tap | PC | base | off | len | when |
|---|---|---|---|---|---|
| access unit | `0x081b6ee8` | `r1` | 0 | `r2` | front-end entry, before compaction |
| SPS/PPS config | `0x081b6cb8` | `r5` | 0 | `r6` | branch target in `FUN_081b6a80` |

`audump=PC:REG_BASE:OFF:REG_LEN:PATH` writes `[base+off]` for `REG_LEN` bytes to
`PATH` (binary, truncate), at a TB head. Env:

- `VDEC_DECODE=1` — enable the model (default off; a labelled host-side shortcut).
- `VDEC_AU_FILE` — the per-frame access unit (4-byte AVCC length-prefixed).
- `VDEC_CFG_FILE` — the AVCDecoderConfigurationRecord (avcC), prepended once.

On the START write the device reads both files, decodes the whole AU with
libavcodec (`ipod_classic_vdec_dec.c`), writes YUV420 into `+0xcc/d0/d4`
(stride W for Y, W/2 for chroma), sets `+0xf0` bit 0 and `+0xf4 = 0`, and
pulses the done line. `+0xf0` is device-owned: writes are ignored, the status
survives the context reset, and the guest's done poll reads it.

## Display path — planar-YUV layer (fmt 8)

The decoded planes are presented through the display pipe (`0x38900000`), not a
software RGB convert. `0x081445cc` case 5 programs:

| display reg | plane |
|---|---|
| `+0x038` | Y |
| `+0x03c` | U |
| `+0x040` | unused (written 0) |
| `+0x044` | V |

with destination rect `+0x030` (origin) / `+0x034` (limit). `ipod_classic_disp.c`
implements the conversion (BT.601) into the panel RGB565 image, drawn at the
back before the RGB layer slots. The UI also programs this path (a 240x240
destination), so the model only renders the full-panel (W x H) blit until the
fmt/geometry rules are fully pinned. The pipe's colour-matrix tables
(`+0x0f0..`) are not decoded; the contract lists them open.

## Software front-end

| Piece | VA | Role |
|---|---|---|
| `MeCCAVideoDecode` task | `0x08088768` | RTXC task; pulls packets and drives the decoder class |
| decoder class vtable | `0x089949c0` | instance `0x08aed1d8` |
| ctor | `0x081b7220` | sets `[obj+0x38] = 0x39800000`, creates event `0x08a123e4` |
| SPS/PPS / stream setup | `0x081b6a80` | parses avcC, allocates buffers/frames, registers line 35 |
| context reset | `0x081b6a14` | zeroes `+0x00..+0x12c` |
| front-end | `0x081b6ee8` | parses NAL/slice, compacts the AU in place |
| hardware submit | `0x081b65c4` | programs the block + descriptor table, waits for done |
| display planar-YUV | `0x081445cc` | programs `+0x038..+0x044` for fmt 8/9/10 |

## Runtime (QEMU, `Emedia-sync.img`, staged Star Trek m4v)

- With the model on, the tapped AUs decode (first `923 B` SEI+IDR, later tiny
  P-slices) and the planes are written. Pre-placing a t=15 keyframe AU in
  `au.bin` puts the TNG starfield on the panel.
- **Resolved (negative, 2026-10-10):** real playback decodes a burst of ~8 frames and stops because
the present completion is never armed — the AVS ring is on the mode-0 engine and only the mode-1 path
arms the display frame-done (VIC line 30, handler `0x0808b66c`), which is registered but never enabled
(`VIC0 IntEnable` bit 30 stays `0x21a7010f`). No reachable model edge. This is a guest-side gap, not
the block; the stream-cache/audio stall is the same class. The `0x39300000 frame-done is modelled default-off (`SDO_FRAME_IRQ=1`).
- The separate IRAM1 blit abort `FUN_0802e5ec` is a guest overrun (owner =
  SM1/codec audio `pcm_push`; it cannot block the video path).
  Not a model gap: caught at the store `0x0802e6b0 strhne r3,[r2],#2`, surface
  `r5=0x08957038` with `[r5+0xc]=0x08957028` (a valid heap base), and the band
  descriptor at `r4=0x0bb0e798` is byte-identical across two fresh snapshot runs
  (the model is not corrupting it). `DFAR 0x220428d8` = `0x28d8` past IRAM1 end
  `0x22040000`; the same snapshot also gave `0x18d8`/`0x24d8` earlier, so the runaway
  offset varies with playback progress. The runaway is
  `rect[0xc]` (`0x1d4d8`) added to the plane base + the growing `+0xa48` position term
  from `FUN_080ab6d0` — a 120 KB band offset into an IRAM1 surface, guest-side. `IRAMSPILL=0x10000` is a
  measurement aid, not a fix.

## Tooling

- `VDECTRACE=1`, `VDEC_DECODE=1`, `VDEC_AU_FILE`, `VDEC_CFG_FILE`,
  `DISPLOG=1`, `DISPTRACE=1`, `IRAMSPILL`.

## Evidence

`contrib/plugins/pctrace.c` (pctrace plugin).

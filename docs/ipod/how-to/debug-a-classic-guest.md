# How to inspect and debug a running iPod classic guest

Tasks against `-M iPod-Classic` running retailOS 2.0.5. Addresses, environment variables and device behaviour: `hw/arm/ipod_classic/README.md`.

Ground rules:

- For native runs, use `mise run ipod:gdb`; for the managed Docker window use `mise run docker:ipod:gdb`.
- When using Docker, run QEMU and its client (gdb, a monitor script) in the same `docker run … qemu-ipod-dev bash -c '...'`: containers do not share the host's `127.0.0.1`.
- Use your own ports and sockets. The live window (`mise run ipod:run` or `mise run docker:ipod:run`) owns `$SCRATCH/live.sock` and gdb `:1405`.
- Keep output under `$SCRATCH` (`assets/scratch`); the container's `/tmp` is not the host's.

## Boot headless

```sh
mise run ipod:run -- --headless --timeout=30   # also --osos=PATH, --disks=PATH; extra args go to QEMU
mise run ipod:run -- --headless --timeout=30 -d in_asm -D $SCRATCH/in_asm.log
```

A full boot needs a disk (`DISK`; build one with `mise run ipod:make-disk`, see `README.md`) and `FWGUID=000A270021574219`. Without the disk the guest reset-loops (`SWRCON=0xaa5`): the fonts are on the disk. Read stderr: the guest prints its panics there through semihosting.

## Take a screenshot

Start QEMU with `-monitor unix:$SCRATCH/mon.sock,server,nowait`, then send `screendump $SCRATCH/screens/NAME.ppm` to the socket.

## Press keys

Monitor `sendkey <key>` (`q`/`e` wheel, `ret` SELECT, `w` MENU, `a`/`d`, `s`), then `screendump` to check the screen. One `sendkey` is one press; no wake-up or re-send is needed. Language → main menu takes about 2 s.

## Read guest memory

1. Pick the command: `xp` and `pmemsave` take physical addresses, `memsave` virtual. At runtime VA == physical.
1. Translate image offsets: the boot copy shifts the image down by `0xAF98`, so `file_off k` runs at `k + 0x07FF5068` and the file's first `0xAF98` bytes are not mapped (`memsave 0x07ff5068` fails, `pmemsave` reads zeros).
1. Quote absolute paths: `pmemsave ADDR LEN "/abs/path"`. Unquoted, HMP parses `LEN /abs…` as a division and writes no file. A relative name lands in QEMU's cwd.
1. For exception and IRQ-path code use the low IRAM0 alias (`0x0`..`0x20000`): a breakpoint at `0x18` hits, `0x22000018` never does.

There is no `x/i` (the build has no capstone); disassemble offline.

## Find the current display buffer

Run with `DISPLOG=1`: every change of a layer source register prints `s5l8702.disp: SOURCE ADDRESS +0xOFF = 0xADDR`. Or read it live: `xp /wx 0x38900060` (slot 0), `0x38900078` (slot 1), `0x38900090`/`0xa8`/`0xc0` (slots 2-4). Slot 0 double-buffers, so read it right after the event. Dump the buffer with `pmemsave` (ARGB8888, 320×240). `DISPTRACE=1` logs every register access.

## Attach gdb

```sh
docker run --rm -i --user $(id -u):$(id -g) -v $PWD:$PWD -w $PWD qemu-ipod-dev \
  bash -c 'T=40 build/docker/qemu-system-arm -M iPod-Classic \
  -kernel assets/firmware/2.0.5/osos_decrypted.bin -serial null \
  -drive if=ide,format=raw,file=assets/disks/E.img -snapshot -L build/docker/pc-bios \
  -display none -gdb tcp::1701 >/dev/null 2>&1 &
  sleep 25; gdb-multiarch -nx -batch -ex "set arm abi AAPCS" -ex "target remote :1701" \
  -ex "break *0x08069a5c" -ex continue -ex "info registers" -ex detach'
```

- Always `set arm abi AAPCS` before `target remote`; the default APCS makes gdb's prologue scan read bogus frames and disconnect.
- After `detach` the guest keeps running and you can attach again. Guest timers are virtual, so a stop is invisible to the guest.
- To have breakpoints in place before the first instruction, start QEMU with `-S`, attach, set breakpoints, `continue`.
- Navigate first, then attach: a stop on the input path can eat a key.
- "Cannot execute this command while the target is running" means the breakpoint was never hit: wrong alias (`0x22…` for IRQ code) or a cold address. Wrap gdb in `timeout`.
- `bt` stops after two frames (the guest's frames are not APCS-walkable): read `$lr` and `x/32wx $sp`, or use pctrace.

## Trace without stopping (pctrace)

`mise run build` or `mise run docker:build` builds `BUILD_DIR/contrib/plugins/libpctrace.so` (`build/native` or `build/docker`). Add `-plugin BUILD_DIR/contrib/plugins/libpctrace.so,<spec>` to the boot. The default trigger is `key_dispatch` `0x08056900`; options are documented at the top of `contrib/plugins/pctrace.c`.

- Register and memory reads (`regpc=`, `sendex=`, `memr=`) only happen at a TB head; a mid-TB PC is skipped and counted as `[mid-TB skipped=N]`. Probe a function entry or a return address.
- In `memr=ADDR:REG:OFF:LEN:LABEL`, `REG` is a register index (0-12 = r0-r12, 13 sp, 14 lr, 15 pc).

## Capture audio

Prefix the boot with `AUDIOWAV=$SCRATCH/out.wav` and play a song: every PCM write to the I2S FIFO goes to the WAV. `AUDIOLIVE=1` plays it through SDL instead.

## Save and restore a running state

Use a qcow2 overlay on the raw disk and QEMU's internal snapshots: `savevm NAME` in the monitor, then start a fresh QEMU with the same command line, the same environment variables and `-loadvm NAME`. Copy the overlay before each restore so restores do not leak disk writes into each other. What a snapshot does and does not carry: `hw/arm/ipod_classic/README.md`, "Snapshots".

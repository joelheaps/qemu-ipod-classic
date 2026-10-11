# pctrace-fix: `memr` byte order and truncated per-hit logs

Two defects in `contrib/plugins/pctrace.c` found while tracing retailOS 2.0.5, both fixed and
verified against the 2.0.5 image.

## Defect 1 — `memr=` logged target-endian bytes, so a pointer looked byte-swapped

### Symptom

`memr=ADDR:REG:OFF:LEN:LABEL` reads guest memory correctly, but it printed the bytes in memory
(address) order with `%02x`. For a 4-byte little-endian word that reads as the byte-swapped
integer. At `pc=0x0820DA24`, `memr=0x0820DA24:0:0:4:goto_self_vt` logged
`mem[0x09290EA0]=ec2c9a08`, which looks like the implausible value `0xec2c9a08`; gdb read
`*(unsigned int*)0x09290EA0 = 0x089A2CEC` (a vtable pointer). The plugin had in fact read the
right bytes — `ec 2c 9a 08` little-endian is `0x089a2cec` — the output just wasn't a value.

### Root cause

`vcpu_regpc_exec()` dumped `mem_buf` byte by byte. `qemu_plugin_read_memory_vaddr()` uses
`cpu_memory_rw_debug()`, which fills the buffer in target byte order (ARM is little-endian), so a
word printed as bytes reads backwards.

### Fix

New `print_mem_value()` helper: LEN 1/2/4/8 is printed as a little-endian numeric value; any other
LEN keeps the raw byte string (a byte dump is still what you want for an opaque field). A 4-byte
pointer now prints exactly like `x/wx`.

### Evidence

Reproduced with a stable ROM literal pointer: at the hot PC `0x0802608c`, `r15 - 0x3c = 0x08026050`
holds a pointer. Both plugins were run against the same image and options; gdb read the same runtime
VA after the guest booted (MMU on).

```
$ OSOS=assets/scratch/osos_decrypted.bin DISK=assets/scratch/ipod-disk.qcow2 \
    scripts/ipod/run --headless --timeout=5 \
    -plugin <PLUGIN>,out=<LOG>,nodefault=on,memr=0x0802608c:15:-0x3c:4:lit,always=on,regcap=2
```

Old plugin (`assets/scratch/libpctrace_old.so`, base 95bd22db521b):

```
PCTRACE +       0us cpu0  REGPC lit  pc=0x0802608c  ...  mem[0x08026050]=707fb308
```

New plugin (`build/contrib/plugins/libpctrace.so`):

```
PCTRACE +       0us cpu0  REGPC lit  pc=0x0802608c  ...  mem[0x08026050]=0x08b37f70
```

gdb (same address, guest booted so the runtime MMU mapping is active):

```
(gdb) x/1xw 0x08026050
0x8026050:	0x08b37f70
(gdb) x/4xb 0x08026050
0x8026050:	0x70	0x7f	0xb3	0x08
```

`0x08b37f70` is `70 7f b3 08` little-endian, matching the new `memr` output and the firmware
literal. The old `707fb308` was the same four bytes in address order.

## Defect 2 — per-hit log stopped long before the run ended

### Symptom

With `always=on`, the `PCTRACE +…us` per-hit lines stopped while the guest kept running. The
summary still showed the watched PC executing (e.g. `hits=82729`), so the log was a silent
undercount.

### Root cause

Two silent counters bounded the log:

1. `cap` (per-PC, default **2000**) — in `always` mode each PC logged only its first `cap` hits,
   then went quiet. This is the cause seen here: watching hot PC `0x0802608c` (~16k hits/s), the
   log ended at hit #2000 after 5.5 ms.
2. `PCTRACE_LINE_MAX` (global, hard-coded **5000000**, undocumented) — once 5M lines were emitted
   `vcpu_pc_exec()`/`vcpu_range_exec()` dropped every further line.

Neither printed a notice, so the truncation was invisible.

### Fix

- `cap=N` now defaults to **0 = unlimited**, so `always=on` is complete unless bounded explicitly.
  A non-zero cap still stops logging but prints a one-time notice naming the PC.
- The global cap became the `linemax=N` option, default **0 = unlimited**, with a one-time notice
  if a non-zero value is reached.

### Evidence

Same image, same hot PC `0x0802608c`, `always=on`, `--timeout=6`:

```
$ OSOS=assets/scratch/osos_decrypted.bin DISK=assets/scratch/ipod-disk.qcow2 \
    scripts/ipod/run --headless --timeout=6 \
    -plugin <PLUGIN>,out=<LOG>,nodefault=on,pc=0x0802608c:hot,always=on
```

Old plugin:

```
per-hit lines: 2000
PCTRACE +    5548us cpu0  hot  @0x0802608c  #1999
PCTRACE +    5549us cpu0  hot  @0x0802608c  #2000
=== PCTRACE summary ===
  hot                          @0x0802608c  hits=82729
```

New plugin:

```
per-hit lines: 82617
PCTRACE + 5644791us cpu0  hot  @0x0802608c  #82616
PCTRACE + 5644794us cpu0  hot  @0x0802608c  #82617
=== PCTRACE summary ===
  hot                          @0x0802608c  hits=82617
```

The log now runs to the end of the 6 s run (last line at +5.64 s) and the line count equals the
summary hit count.

The opt-in caps still work and now announce themselves:

```
$ ... -plugin ...,pc=0x0802608c:hot,always=on,linemax=5
PCTRACE WARNING: linemax=5 reached; all further per-hit lines suppressed (linemax=0 disables)

$ ... -plugin ...,pc=0x0802608c:hot,always=on,cap=3
PCTRACE WARNING: cap=3 reached for hot @0x0802608c; that PC is no longer logged (cap=0 disables)
```

## Build

`make -C build/contrib/plugins pctrace` now builds the plugin (the Makefile gained the short
`pctrace` alias; the canonical target remains `libpctrace.so`), and `scripts/build-in-docker`
still builds `libpctrace.so`. `-Wall` build is clean.

## Files changed

- `contrib/plugins/pctrace.c` — `print_mem_value()` for `memr`; `cap` default 0 with one-time
  notice; global cap becomes `linemax=` (default 0) with one-time notice; docstring updated.
- `contrib/plugins/Makefile` — `pctrace: libpctrace.so` convenience target.
- `docs/issues/pctrace-fix.md` — this document.

## Friction

- The brief's `make -C build/contrib/plugins pctrace` did not exist in the base tree: the contrib
  Makefile only has `libpctrace.so` (`NAMES` is not a target). Added the alias.
- The workspace `assets/firmware/2.0.5` and `assets/disks/2.0.5` are symlinks to the main
  checkout. `scripts/ipod/run` resolves mounts with `d=$(cd "$(dirname "$f")" && pwd)`, and POSIX
  `pwd` keeps the logical (symlinked) path, so the loop believes the target is inside `$PWD` and
  adds no bind mount; inside the container the dangling symlink makes QEMU fail with
  `Could not open 'assets/…': No such file or directory`. Worked around by copying
  `osos_decrypted.bin` and `ipod-disk.qcow2` into `assets/scratch/` and pointing `OSOS`/`DISK`
  there. A `readlink -f` (or `pwd -P`) in that loop would fix it.
- The gdb-vs-plugin comparison needs the *runtime* VA mapping. At reset (`-S`) the MMU is off, so
  gdb reads physical `0x08026050` (file offset `0x26050`), not the runtime image; attach after the
  guest has booted to read the same VA the plugin sees.

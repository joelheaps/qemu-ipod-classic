# Qemu Session

[tools Index](../../../README.md#tools-index) / [Tools](../../index.md#tools) / [Ipod Classic](../index.md#ipod-classic) / [Lib](./index.md#lib) / Qemu Session

> Auto-generated documentation for [tools.ipod_classic.lib.qemu_session](../../../../../../../tools/ipod_classic/lib/qemu_session.py) module.

```text
qemu_session.py -- launch and drive a headless iPod classic 6G QEMU.

The library behind goto_screen.py and the screenshot harness. Stdlib only.

QEMU is $BUILD_DIR/qemu-system-arm, with BUILD_DIR defaulting to build/native (the layout of
scripts/ipod/run); its firmware directory is $BUILD_DIR/pc-bios. When the host lacks QEMU's runtime
libraries, run the caller inside the dev image (docker/Dockerfile.dev).

    from qemu_session import launch
    with launch(out='assets/scratch/runs/me') as e:            # reference timing (the default)
        e.wait_booted()                            # first Language frame is dumpable
        e.mon.key('ret'); f = e.mon.wait_stable(30)
        f.write_png('after.png')

    Monitor(sock=PATH | port=N)  one HMP session; no module state, so any number can coexist.
        cmd(c, timeout=20) -> str        returns as soon as the '(qemu) ' prompt reappears
        key(k, hold=None, *, wait=False)  sendkey; hold ms defaults per key (HOLD)
        keys('q*12 e ret')               several keys, queued by QEMU in order
        shot(path=None) -> Frame         screenshot (synchronous in HMP) and parse it
        wait_for(pred, timeout, interval=0.2)   poll pred() until truthy
        wait_stable(timeout, ignore=(), min_lit=0) -> Frame
        close()
    launch(image, disk, out, fast=False, ...) -> Emulator(.mon, .out, .wait_booted(), .close())
    launch(..., fast=True) opts into `-icount shift=auto,sleep=off` (see the fast risk note below).

Parallel runs: each launch() has its own out dir and monitor socket, so up to MAX_PARALLEL can run
side by side.

CLI against a running monitor ($EMU_SOCK unix path or $EMU_PORT tcp; shots to $EMU_OUT):
    qemu_session.py shot NAME | key K [HOLD] | raw 'MONITOR CMD' | seq TOK...
        TOK = NAME | K:HOLD | wait:SECS
    qemu_session.py bootshot [--fast] NAME [--disk F] [--image F] [--mon CMD]... [-- QEMU ARGS]

Keys (hw/arm/ipod_classic/ipod_classic_wheel.c): q/e wheel up/down, ret SELECT, w MENU,
a/d left/right, s play/pause. The Language -> main menu transition takes about 2 s, not 25 s.
```

#### Attributes

- `DEFAULT_ENV` - Knobs every run gets unless the caller's environment (or env=) sets them non-empty. FWGUID: the music DB is rejected without it. IRAMSPILL: maps RAM above IRAM1 (needed by the NP/video work; harmless for the menus).: {'FWGUID': '000A270021574219', 'IRAMSPILL': '0x10000'}

- `FAST_ARGS` - Device-model knobs, e.g. FWGUID, IRAMSPILL, PMUCHARGER, SM1\_\*, are read by the guest QEMU. launch() passes the whole effective environment to the process.: ['-icount', 'shift=auto,sleep=off']

- `DISP_SRC` - xp /wx here goes non-zero at the instant the first frame is dumpable (display source address).: 948961400

- `GUEST_CLOCK` - TECNT of timer E: the guest's microsecond clock (1 MHz, programmed by the OS), counted on QEMU's virtual clock, so under -icount sleep=off it races ahead while the guest idles. It wraps every 2\*\*32 us (71 min).: 1013973172

- `HOLD` - sendkey hold times in ms: SELECT needs 250 (shorter ones were lost on the Language screen), MENU 150, wheel steps 120.: {'ret': 250, 'w': 150}

- [Qemu Session](#qemu-session)

  - [Emulator](#emulator)
    - [Emulator().__enter__](<#emulator()__enter__>)
    - [Emulator().__exit__](<#emulator()__exit__>)
    - [Emulator().close](<#emulator()close>)
    - [Emulator().wait_booted](<#emulator()wait_booted>)
  - [Monitor](#monitor)
    - [Monitor().__enter__](<#monitor()__enter__>)
    - [Monitor().__exit__](<#monitor()__exit__>)
    - [Monitor().\_read_prompt](<#monitor()_read_prompt>)
    - [Monitor().close](<#monitor()close>)
    - [Monitor().cmd](<#monitor()cmd>)
    - [Monitor().guest_us](<#monitor()guest_us>)
    - [Monitor().key](<#monitor()key>)
    - [Monitor().keys](<#monitor()keys>)
    - [Monitor().shot](<#monitor()shot>)
    - [Monitor().wait_for](<#monitor()wait_for>)
    - [Monitor().wait_stable](<#monitor()wait_stable>)
  - [Pacer](#pacer)
    - [Pacer().\_halt](<#pacer()_halt>)
    - [Pacer().\_resume](<#pacer()_resume>)
    - [Pacer().\_settle](<#pacer()_settle>)
    - [Pacer().guard](<#pacer()guard>)
    - [Pacer().key](<#pacer()key>)
    - [Pacer().settle](<#pacer()settle>)
    - [Pacer().shot](<#pacer()shot>)
    - [Pacer().wait_until](<#pacer()wait_until>)
  - [Timing](#timing)
  - [\_connect](#_connect)
  - [\_no_log](#_no_log)
  - [\_no_row](#_no_row)
  - [\_out_dir](#_out_dir)
  - [\_save](#_save)
  - [abspath](#abspath)
  - [build_cmd](#build_cmd)
  - [effective_env](#effective_env)
  - [launch](#launch)
  - [main](#main)
  - [scratch_runs](#scratch_runs)

## Emulator

[Show source in qemu_session.py:542](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L542)

A running QEMU plus its Monitor. close() (or `with`) terminates it and removes temp files.

#### Signature

```python
class Emulator:
    def __init__(
        self,
        proc: subprocess.Popen,
        mon: Monitor,
        out: str,
        cmd: list[str],
        cleanup: Sequence[str] = (),
    ) -> None: ...
```

#### See also

- [Monitor](#monitor)

### Emulator().__enter__

[Show source in qemu_session.py:595](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L595)

Return self.

#### Signature

```python
def __enter__(self) -> Self: ...
```

### Emulator().__exit__

[Show source in qemu_session.py:599](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L599)

Close the emulator.

#### Signature

```python
def __exit__(self, *_a: object) -> None: ...
```

### Emulator().close

[Show source in qemu_session.py:581](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L581)

Close the monitor, terminate QEMU and remove temp files.

#### Signature

```python
def close(self) -> None: ...
```

### Emulator().wait_booted

[Show source in qemu_session.py:557](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L557)

Block until the first Language frame is dumpable and return that Frame.

Primary: poll `xp /wx 0x38900078` (the display source address) until it is non-zero; it flips at the exact moment a screenshot has content, so no timer is involved. Fallback (the xp output is unparsable): poll screenshots until lit and stable. Both paths then wait for two identical frames, because the display source can flip mid-fade. FUDGE: the stability window is one 0.2 s interval; the Language right pane cycles languages, so callers that compare frames should ignore it (screen_tree.ignore_for('language')).

#### Signature

```python
def wait_booted(self, timeout: float = 90, min_lit: int = 5000) -> screenshot.Frame: ...
```

## Monitor

[Show source in qemu_session.py:220](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L220)

One HMP monitor session over a unix socket (`sock`) or TCP (`port`).

#### Signature

```python
class Monitor:
    def __init__(
        self,
        sock: str | None = None,
        port: int | None = None,
        host: str = "127.0.0.1",
        tmp: str | Path | None = None,
        connect_timeout: float = 30,
        alive: Callable[[], bool] | None = None,
    ) -> None: ...
```

### Monitor().__enter__

[Show source in qemu_session.py:370](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L370)

Return self.

#### Signature

```python
def __enter__(self) -> Self: ...
```

### Monitor().__exit__

[Show source in qemu_session.py:374](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L374)

Close the monitor.

#### Signature

```python
def __exit__(self, *_a: object) -> None: ...
```

### Monitor().\_read_prompt

[Show source in qemu_session.py:247](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L247)

Read until the `(qemu)` prompt; TimeoutError after `timeout` seconds.

#### Signature

```python
def _read_prompt(self, timeout: float) -> bytes: ...
```

### Monitor().close

[Show source in qemu_session.py:365](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L365)

Close the monitor socket.

#### Signature

```python
def close(self) -> None: ...
```

### Monitor().cmd

[Show source in qemu_session.py:264](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L264)

Send one HMP command and return its output once the prompt is back.

`timeout` is only the safety net (TimeoutError); it is never waited out normally.

#### Signature

```python
def cmd(self, c: str, timeout: float = 20) -> str: ...
```

### Monitor().guest_us

[Show source in qemu_session.py:313](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L313)

Return the guest's microsecond clock (GUEST_CLOCK), modulo 2\*\*32.

#### Signature

```python
def guest_us(self) -> int: ...
```

### Monitor().key

[Show source in qemu_session.py:275](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L275)

Send `k` for `hold` ms (default per key, see HOLD).

HMP returns at once and QEMU queues successive sendkeys in order, each keeping its hold, so back-to-back calls are safe. wait=True also sleeps the hold, i.e. returns when the key is released.

#### Signature

```python
def key(self, k: str, hold: int | None = None, wait: bool = False) -> None: ...
```

### Monitor().keys

[Show source in qemu_session.py:287](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L287)

Send 'q\*6 e ret' in order; `k*N` repeats and `k:HOLD` sets the hold.

Not waited on: the caller must wait_stable/wait_for afterwards (the guest is still moving).

#### Signature

```python
def keys(self, seq: str) -> None: ...
```

### Monitor().shot

[Show source in qemu_session.py:298](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L298)

Take a screenshot and return it as a Frame.

HMP screendump writes the whole file before the prompt returns, so there is no polling; a short file (ValueError) would mean QEMU itself failed. With `path` the .ppm is kept there; otherwise a scratch file is reused.

#### Signature

```python
def shot(self, path: str | Path | None = None) -> screenshot.Frame: ...
```

### Monitor().wait_for

[Show source in qemu_session.py:322](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L322)

Poll `pred()` until it returns something truthy (which is returned).

#### Signature

```python
def wait_for(
    self, pred: Callable[[], object], timeout: float = 10, interval: float = 0.2
) -> object: ...
```

### Monitor().wait_stable

[Show source in qemu_session.py:336](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L336)

Screenshot until two frames match outside `ignore` and have >= min_lit lit pixels.

`ignore` holds (x0, y0, x1, y1) rectangles. Returns that frame; TimeoutError otherwise. FUDGE: 'identical for one `interval`' is the whole stability test, so an animation that pauses longer than `interval` is mistaken for settled; callers that know the destination should wait_for it and use this only to let scrolling die down.

#### Signature

```python
def wait_stable(
    self,
    timeout: float = 10,
    ignore: screenshot.Ignore = (),
    min_lit: int = 0,
    interval: float = 0.1,
) -> screenshot.Frame: ...
```

## Pacer

[Show source in qemu_session.py:379](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L379)

Profile-paced input plus closed-loop screenshot polling for the walkers.

`timing` is REFERENCE (default) or FAST. `row` maps a Frame to the highlighted list row (pass screen_tree.highlight_row); it is used only by the keepalive watchdog, which fires when no key has been sent for `timing.gap_limit` and sends a harmless wheel-up when the list is at row 0, or raises. Fast timing needs this because an idle wall gap over ~2.5 s blanks the LCD and keys do not wake it. `key` sleeps the profile's gap so steps never pile up in QEMU; `settle` requires a stable window (a held pre-change frame must not read as settled).

`freeze` keeps guest time still whenever the caller is between calls: `key`, `wait_until` and `settle` resume the VM (`cont`) and stop it again (`stop`) when they return, and `key` waits out the hold in guest time instead of sleeping a wall gap. Under FAST timing an idle guest's clock races ahead of wall time, so any wall pause between two presses is seconds of guest idle time; freezing makes scripted scenarios reproducible.

#### Signature

```python
class Pacer:
    def __init__(
        self,
        mon: Monitor,
        timing: Timing = REFERENCE,
        row: Callable[[screenshot.Frame], int] | None = None,
        log: Callable[[str], None] | None = None,
        freeze: bool = False,
    ) -> None: ...
```

#### See also

- [Monitor](#monitor)
- [REFERENCE](#reference)
- [Timing](#timing)

### Pacer().\_halt

[Show source in qemu_session.py:426](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L426)

Stop the VM when frozen.

#### Signature

```python
def _halt(self) -> None: ...
```

### Pacer().\_resume

[Show source in qemu_session.py:421](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L421)

Resume the VM when frozen.

#### Signature

```python
def _resume(self) -> None: ...
```

### Pacer().\_settle

[Show source in qemu_session.py:519](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L519)

Return the settled frame, polling until `deadline` (the body of settle()).

#### Signature

```python
def _settle(
    self,
    pred: Callable[[screenshot.Frame], object],
    ignore: screenshot.Ignore,
    deadline: float,
) -> screenshot.Frame | None: ...
```

### Pacer().guard

[Show source in qemu_session.py:459](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L459)

Keep the LCD awake.

A `q` at the top list row is a no-op; `force` also allows it during a post-SELECT settle, where the walker re-clamps before the next commit. Without either it fails loudly, because a wheel on a non-list screen (e.g. Now Playing) changes state.

#### Signature

```python
def guard(self, frame: screenshot.Frame, force: bool = False) -> None: ...
```

### Pacer().key

[Show source in qemu_session.py:431](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L431)

Send one key with `hold` ms (default from the profile) and sleep its gap.

ret/w are buttons, everything else a wheel step. Frozen: no wall gap; the call returns, VM stopped, once the hold has passed in guest time.

#### Signature

```python
def key(self, k: str, hold: int | None = None) -> None: ...
```

### Pacer().settle

[Show source in qemu_session.py:498](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L498)

Poll until `pred(frame)` holds and the frame has been stable outside `ignore`.

"Stable" means unchanged for `timing.stable_us` microseconds of guest time, so a late-drawn pane or a held transition frame is not settled. Returns that frame. On timeout the latest frame is returned if `pred` holds on it (a screen that animates forever, e.g. Cover Flow), else None. The keepalive is forced, so a slow first-visit draw never trips the watchdog (the caller re-places the highlight before the next commit).

#### Signature

```python
def settle(
    self,
    pred: Callable[[screenshot.Frame], object],
    ignore: screenshot.Ignore = (),
    timeout: float | None = None,
) -> screenshot.Frame | None: ...
```

### Pacer().shot

[Show source in qemu_session.py:413](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L413)

Screenshot via the monitor, counting the shot and its wall time.

#### Signature

```python
def shot(self) -> screenshot.Frame: ...
```

### Pacer().wait_until

[Show source in qemu_session.py:476](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L476)

Poll screenshots every `timing.poll` until `pred(frame)`; None on timeout.

#### Signature

```python
def wait_until(
    self,
    pred: Callable[[screenshot.Frame], object],
    timeout: float | None = None,
    force: bool = False,
) -> screenshot.Frame | None: ...
```

## Timing

[Show source in qemu_session.py:83](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L83)

Closed-loop walker pacing (see menu_navigation.Session and qemu_session.Pacer).

`poll` is the sleep between screenshots; the holds and gaps are per key class; `gap_limit` is the wall seconds without a key before the keepalive watchdog acts (below the LCD-blanking timeout under fast timing); `key_timeout` is the longest wall wait for a wheel key's first effect on the screen; `select_timeout` the same for SELECT/MENU; `key_settle` the longest wait for a wheel move to come to rest; `arrive` the longest wait for a screen. Nothing is resent: the emulated wheel loses no input, so a timeout is a failure, and the limits are generous because a wait that is too short misreads a loaded host as a lost key. `stable_us` is how many microseconds of guest time a frame must stay unchanged to count as settled (Pacer.settle); both profiles use 2 s, long enough that a list's row chevron and right pane have finished drawing before the frame is recorded. `long_menu_hold` is a MENU hold long enough to count as a long press. REFERENCE keeps the original pacing (one key at a time, each waited out) and has no watchdog; FAST shortens the holds and gaps, enables the watchdog, and must be matched by the launch's `-icount` arguments.

#### Signature

```python
class Timing: ...
```

## \_connect

[Show source in qemu_session.py:189](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L189)

Connect to the monitor socket, retrying until `deadline`; `alive` can abort early.

#### Signature

```python
def _connect(
    sock: str | None,
    host: str,
    port: int | None,
    deadline: float,
    alive: Callable[[], bool] | None,
) -> socket.socket: ...
```

## \_no_log

[Show source in qemu_session.py:185](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L185)

Discard a log message (the Pacer log when the caller gives none).

#### Signature

```python
def _no_log(*_a: object) -> None: ...
```

## \_no_row

[Show source in qemu_session.py:180](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L180)

Return -1 (the Pacer row supplier when the caller gives none).

#### Signature

```python
def _no_row(_f: screenshot.Frame) -> int: ...
```

## \_out_dir

[Show source in qemu_session.py:749](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L749)

Return the monitor CLI's output directory (EMU_OUT or scratch/runs/remote).

#### Signature

```python
def _out_dir() -> str: ...
```

## \_save

[Show source in qemu_session.py:756](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L756)

Save a screenshot at <path>.ppm and <path>.png, and print its lit-pixel count.

#### Signature

```python
def _save(m: Monitor, path: str | Path) -> None: ...
```

#### See also

- [Monitor](#monitor)

## abspath

[Show source in qemu_session.py:163](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L163)

Return the absolute path of `p` as a string.

#### Signature

```python
def abspath(p: str | Path) -> str: ...
```

## build_cmd

[Show source in qemu_session.py:604](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L604)

Return the QEMU argv for a headless boot of `kernel` with `disk` and the given monitor.

#### Signature

```python
def build_cmd(
    kernel: str | Path,
    disk: str | Path,
    mon_arg: str,
    disk_format: str = "raw",
    snapshot: bool = True,
    gdb: int = 0,
    plugin: str = "",
    loadvm: str = "",
    paused: bool = False,
    fast: bool = False,
    extra: Sequence[str] = (),
    qemu: str | None = None,
) -> list[str]: ...
```

## effective_env

[Show source in qemu_session.py:168](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L168)

Return the environment QEMU sees: os.environ + `env` + DEFAULT_ENV for empty knobs.

An exported-but-empty knob would otherwise count as set (the device code tests getenv()).

#### Signature

```python
def effective_env(env: dict[str, str] | None = None) -> dict[str, str]: ...
```

## launch

[Show source in qemu_session.py:649](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L649)

Start QEMU on an osos .bin (-kernel) and connect a Monitor; return an Emulator.

Every instance has its own `out` dir (qemu.out/qemu.err, the monitor socket), so up to MAX_PARALLEL can run side by side.

disk raw (or qcow2 with disk_format) image, read-only under -snapshot (default). port TCP monitor on 127.0.0.1:port instead of the unix socket in `out`. loadvm/paused `-loadvm NAME` / `-S` (snapshot restore). env extra environment for QEMU on top of DEFAULT_ENV (FWGUID, IRAMSPILL); arbitrary keys are passed through. fast TIMING FUDGE, opt-in: `-icount shift=auto,sleep=off`. The guest's virtual clock runs ahead of wall time whenever it idles, so boot is quicker but the guest's idle timer fires within a few wall seconds: the LCD blanks (battery-only) or the charger screen covers it, and keys do not wake a blanked display. Use fast for a boot-only screenshot, or with menu_navigation.keep_awake for a whole walk. Anything the guest paces against wall clock (audio/I2S, PMU RTC) drifts, so playback and music-DB behaviour are unverified. launch() defaults to reference wall-clock timing; goto_screen.py and capture_screenshots.py default to fast.

#### Signature

```python
def launch(
    image: str | Path = DEFAULT_IMAGE,
    disk: str | Path = DEFAULT_DISK,
    out: str | Path | None = None,
    disk_format: str = "raw",
    snapshot: bool = True,
    gdb: int = 0,
    plugin: str = "",
    port: int = 0,
    loadvm: str = "",
    paused: bool = False,
    fast: bool = False,
    env: dict[str, str] | None = None,
    extra: Sequence[str] = (),
    qemu: str | None = None,
) -> Emulator: ...
```

#### See also

- [DEFAULT_DISK](#default_disk)
- [DEFAULT_IMAGE](#default_image)
- [Emulator](#emulator)

## main

[Show source in qemu_session.py:764](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L764)

Run the CLI (see the module docstring); return the exit status.

#### Signature

```python
def main(a: list[str]) -> int: ...
```

## scratch_runs

[Show source in qemu_session.py:739](../../../../../../../tools/ipod_classic/lib/qemu_session.py#L739)

Return the per-run scratch directory (SCRATCH/runs), created if needed.

#### Signature

```python
def scratch_runs() -> str: ...
```

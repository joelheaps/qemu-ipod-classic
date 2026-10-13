#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""qemu_session.py -- launch and drive a headless iPod classic 6G QEMU.

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
"""

import contextlib
import os
import re
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Self

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]  # the repo root
sys.path.insert(0, str(HERE))
import screenshot  # noqa: E402

BUILD_DIR = ROOT / (os.environ.get("BUILD_DIR") or "build/native")
QEMU = str(BUILD_DIR / "qemu-system-arm")
QEMU_IMG = str(BUILD_DIR / "qemu-img")
PC_BIOS = str(BUILD_DIR / "pc-bios")
DEFAULT_IMAGE = str(ROOT / "assets/firmware/2.0.5/osos_decrypted.bin")
DEFAULT_DISK = str(ROOT / "assets/disks/Emedia.img")
MAX_PARALLEL = 8
# Knobs every run gets unless the caller's environment (or env=) sets them non-empty.
# FWGUID: the music DB is rejected without it. IRAMSPILL: maps RAM above IRAM1 (needed by the
# NP/video work; harmless for the menus).
DEFAULT_ENV = {"FWGUID": "000A270021574219", "IRAMSPILL": "0x10000"}
# Device-model knobs, e.g. FWGUID, IRAMSPILL, PMUCHARGER, SM1_*, are read by the guest QEMU.
# launch() passes the whole effective environment to the process.
FAST_ARGS = ["-icount", "shift=auto,sleep=off"]
SOCKET_PATH_MAX = 100  # sun_path limit is 108
EXIT_USAGE = 2
HOLD_ARG_INDEX = 2


@dataclass(frozen=True)
class Timing:
    """Closed-loop walker pacing (see menu_navigation.Session and qemu_session.Pacer).

    `poll` is the sleep between screenshots; the holds and gaps are per key class; `gap_limit`
    is the wall seconds without a key before the keepalive watchdog acts (below the LCD-blanking
    timeout under fast timing); `key_timeout` is the longest wall wait for a wheel key's first
    effect on the screen; `select_timeout` the same for SELECT/MENU; `key_settle` the longest
    wait for a wheel move to come to rest; `arrive` the longest wait for a screen. Nothing is
    resent: the emulated wheel loses no input, so a timeout is a failure, and the limits are
    generous because a wait that is too short misreads a loaded host as a lost key. `stable_us`
    is how many microseconds of guest time a frame must stay unchanged to count as settled
    (Pacer.settle); both profiles use 2 s, long enough that a list's row chevron and right pane
    have finished drawing before the frame is recorded. `long_menu_hold` is a MENU hold long
    enough to count as a long press. REFERENCE keeps the original pacing
    (one key at a time, each waited out) and has no watchdog; FAST shortens the holds and gaps,
    enables the watchdog, and must be matched by the launch's `-icount` arguments.
    """

    poll: float
    wheel_hold: int
    wheel_gap: float
    button_hold: int
    menu_hold: int
    button_gap: float
    gap_limit: float
    key_timeout: float
    arrive: float
    select_timeout: float
    key_settle: float
    stable_us: int
    long_menu_hold: int


REFERENCE = Timing(
    poll=0.1,
    wheel_hold=120,
    wheel_gap=0.12,
    button_hold=250,
    menu_hold=250,
    button_gap=0.25,
    gap_limit=float("inf"),
    key_timeout=8.0,
    arrive=15.0,
    select_timeout=15.0,
    key_settle=24.0,
    # 2 s like FAST: a shorter window settles on a list frame before its chevron is drawn, which
    # then fails the restore round-trip check.
    stable_us=2_000_000,
    long_menu_hold=1000,
)
FAST = Timing(
    poll=0.005,
    wheel_hold=60,
    wheel_gap=0.05,
    button_hold=250,
    menu_hold=150,
    button_gap=0.10,
    gap_limit=2.0,
    key_timeout=10.0,
    arrive=8.0,
    select_timeout=10.0,
    key_settle=3.0,
    stable_us=2_000_000,
    long_menu_hold=600,
)
# xp /wx here goes non-zero at the instant the first frame is dumpable (display source address).
DISP_SRC = 0x38900078
# TECNT of timer E: the guest's microsecond clock (1 MHz, programmed by the OS), counted on QEMU's
# virtual clock, so under -icount sleep=off it races ahead while the guest idles. It wraps every
# 2**32 us (71 min).
GUEST_CLOCK = 0x3C7000B4
GUEST_CLOCK_MASK = 0xFFFFFFFF
# sendkey hold times in ms: SELECT needs 250 (shorter ones were lost on the Language screen),
# MENU 150, wheel steps 120.
HOLD = {"ret": 250, "w": 150}
DEFAULT_HOLD = 120
PROMPT = b"(qemu)"
_ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def abspath(p: str | Path) -> str:
    """Return the absolute path of `p` as a string."""
    return str(Path(p).resolve())


def effective_env(env: dict[str, str] | None = None) -> dict[str, str]:
    """Return the environment QEMU sees: os.environ + `env` + DEFAULT_ENV for empty knobs.

    An exported-but-empty knob would otherwise count as set (the device code tests getenv()).
    """
    e = dict(os.environ, **(env or {}))
    for k, v in DEFAULT_ENV.items():
        if not e.get(k):
            e[k] = v
    return {k: v for k, v in e.items() if not (k in DEFAULT_ENV and v == "")}


def _no_row(_f: screenshot.Frame) -> int:
    """Return -1 (the Pacer row supplier when the caller gives none)."""
    return -1


def _no_log(*_a: object) -> None:
    """Discard a log message (the Pacer log when the caller gives none)."""


def _connect(
    sock: str | None,
    host: str,
    port: int | None,
    deadline: float,
    alive: Callable[[], bool] | None,
) -> socket.socket:
    """Connect to the monitor socket, retrying until `deadline`; `alive` can abort early."""
    while True:
        s = None
        try:
            s = (
                socket.socket(socket.AF_UNIX)
                if sock
                else socket.create_connection((host, port), timeout=5)
            )
            if sock:
                s.connect(sock)
        except OSError as e:
            # create_connection raises before returning; close only what this loop opened.
            if s is not None:
                s.close()
            # QEMU creates the socket with server,nowait: retry until it is up.
            if time.time() > deadline or (alive and not alive()):
                msg = f"monitor {sock or port} never came up: {e}"
                raise OSError(msg) from e
            time.sleep(0.1)
        else:
            return s


class Monitor:
    """One HMP monitor session over a unix socket (`sock`) or TCP (`port`)."""

    def __init__(  # noqa: PLR0913, PLR0917 - the connection setup is one object
        self,
        sock: str | None = None,
        port: int | None = None,
        host: str = "127.0.0.1",
        tmp: str | Path | None = None,
        connect_timeout: float = 30,
        alive: Callable[[], bool] | None = None,
    ) -> None:
        """Connect to the monitor.

        `tmp` is the scratch .ppm used by shot() when no path is given (default beside the socket
        or in the temp dir). `alive` is an optional callable; False aborts the connect retry loop
        early.
        """
        if not (sock or port):
            sock, port = os.environ.get("EMU_SOCK"), int(os.environ.get("EMU_PORT") or 0)
        self.sock_path, self.port = sock, port
        base = Path(sock).parent if sock else Path(tempfile.gettempdir())
        self.tmp = str(tmp or base / f"_mon{os.getpid()}.ppm")
        deadline = time.time() + connect_timeout
        self.s = _connect(sock, host, port, deadline, alive)
        self._read_prompt(connect_timeout)  # banner + first prompt

    def _read_prompt(self, timeout: float) -> bytes:
        """Read until the `(qemu)` prompt; TimeoutError after `timeout` seconds."""
        buf, deadline = b"", time.time() + timeout
        while True:
            left = deadline - time.time()
            if left <= 0:
                msg = f"no (qemu) prompt within {timeout:g}s; got {buf[-200:]!r}"
                raise TimeoutError(msg)
            if not select.select([self.s], [], [], left)[0]:
                continue
            d = self.s.recv(65536)
            if not d:  # `quit` closes the socket
                return buf
            buf += d
            if buf.rstrip().endswith(PROMPT):
                return buf

    def cmd(self, c: str, timeout: float = 20) -> str:
        """Send one HMP command and return its output once the prompt is back.

        `timeout` is only the safety net (TimeoutError); it is never waited out normally.
        """
        self.s.settimeout(None)
        self.s.sendall((c + "\n").encode())
        out = _ANSI.sub("", self._read_prompt(timeout).decode("latin1"))
        out = out.split("\n", 1)[1] if "\n" in out else ""  # first line is readline's echo
        return out.rsplit("(qemu)", 1)[0] if out.rstrip().endswith("(qemu)") else out

    def key(self, k: str, hold: int | None = None, *, wait: bool = False) -> None:
        """Send `k` for `hold` ms (default per key, see HOLD).

        HMP returns at once and QEMU queues successive sendkeys in order, each keeping its hold, so
        back-to-back calls are safe. wait=True also sleeps the hold, i.e. returns when the key is
        released.
        """
        hold = hold or HOLD.get(k, DEFAULT_HOLD)
        self.cmd(f"sendkey {k} {hold}")
        if wait:
            time.sleep(hold / 1000.0)

    def keys(self, seq: str) -> None:
        """Send 'q*6 e ret' in order; `k*N` repeats and `k:HOLD` sets the hold.

        Not waited on: the caller must wait_stable/wait_for afterwards (the guest is still moving).
        """
        for tok in seq.split():
            k, _, n = tok.partition("*")
            k, _, h = k.partition(":")
            for _ in range(int(n or 1)):
                self.key(k, int(h) if h else None)

    def shot(self, path: str | Path | None = None) -> screenshot.Frame:
        """Take a screenshot and return it as a Frame.

        HMP screendump writes the whole file before the prompt returns, so there is no polling; a
        short file (ValueError) would mean QEMU itself failed. With `path` the .ppm is kept there;
        otherwise a scratch file is reused.
        """
        p = str(Path(path or self.tmp).resolve())  # QEMU's cwd is the run's out dir
        out = self.cmd(f'screendump "{p}"')
        try:
            return screenshot.read(p)
        except (OSError, ValueError) as e:
            msg = f"screenshot {p} failed: {e} {out.strip()}"
            raise RuntimeError(msg) from e

    def guest_us(self) -> int:
        """Return the guest's microsecond clock (GUEST_CLOCK), modulo 2**32."""
        r = self.cmd(f"xp /wx 0x{GUEST_CLOCK:x}")
        mt = re.search(r":\s*0x([0-9a-fA-F]{8})", r)
        if mt is None:
            msg = f"unparsable guest clock: {r!r}"
            raise RuntimeError(msg)
        return int(mt.group(1), 16)

    def wait_for(
        self, pred: Callable[[], object], timeout: float = 10, interval: float = 0.2
    ) -> object:
        """Poll `pred()` until it returns something truthy (which is returned)."""
        deadline = time.time() + timeout
        while True:
            r = pred()
            if r:
                return r
            if time.time() > deadline:
                msg = f"wait_for timed out after {timeout:g}s"
                raise TimeoutError(msg)
            time.sleep(interval)

    def wait_stable(
        self,
        timeout: float = 10,
        ignore: screenshot.Ignore = (),
        min_lit: int = 0,
        interval: float = 0.1,
    ) -> screenshot.Frame:
        """Screenshot until two frames match outside `ignore` and have >= min_lit lit pixels.

        `ignore` holds (x0, y0, x1, y1) rectangles. Returns that frame; TimeoutError otherwise.
        FUDGE: 'identical for one `interval`' is the whole stability test, so an animation that
        pauses longer than `interval` is mistaken for settled; callers that know the destination
        should wait_for it and use this only to let scrolling die down.
        """
        prev, deadline = None, time.time() + timeout
        while True:
            f = self.shot()
            if (
                prev is not None
                and screenshot.same(f, prev, ignore)
                and screenshot.lit(f) >= min_lit
            ):
                return f
            prev = f
            if time.time() > deadline:
                msg = f"screen not stable within {timeout:g}s (min_lit={min_lit})"
                raise TimeoutError(msg)
            time.sleep(interval)

    def close(self) -> None:
        """Close the monitor socket."""
        with contextlib.suppress(OSError):
            self.s.close()

    def __enter__(self) -> Self:
        """Return self."""
        return self

    def __exit__(self, *_a: object) -> None:
        """Close the monitor."""
        self.close()


class Pacer:
    """Profile-paced input plus closed-loop screenshot polling for the walkers.

    `timing` is REFERENCE (default) or FAST. `row` maps a Frame to the highlighted list row (pass
    screen_tree.highlight_row); it is used only by the keepalive watchdog, which fires when no key
    has been sent for `timing.gap_limit` and sends a harmless wheel-up when the list is at row 0, or
    raises. Fast timing needs this because an idle wall gap over ~2.5 s blanks the LCD and keys do
    not wake it. `key` sleeps the profile's gap so steps never pile up in QEMU; `settle` requires a
    stable window (a held pre-change frame must not read as settled).

    `freeze` keeps guest time still whenever the caller is between calls: `key`, `wait_until` and
    `settle` resume the VM (`cont`) and stop it again (`stop`) when they return, and `key` waits out
    the hold in guest time instead of sleeping a wall gap. Under FAST timing an idle guest's clock
    races ahead of wall time, so any wall pause between two presses is seconds of guest idle time;
    freezing makes scripted scenarios reproducible.
    """

    def __init__(
        self,
        mon: Monitor,
        timing: Timing = REFERENCE,
        *,
        row: Callable[[screenshot.Frame], int] | None = None,
        log: Callable[[str], None] | None = None,
        freeze: bool = False,
    ) -> None:
        """Pace `mon` with `timing`; `row`/`log`/`freeze` override the defaults above."""
        self.freeze = freeze
        self.mon, self.timing, self.row = mon, timing, row or _no_row
        self.log = log or _no_log
        self.last_key = time.time()
        self.shots = self.keys = self.keepalives = 0
        self.shot_time = 0.0

    def shot(self) -> screenshot.Frame:
        """Screenshot via the monitor, counting the shot and its wall time."""
        t = time.time()
        f = self.mon.shot()
        self.shot_time += time.time() - t
        self.shots += 1
        return f

    def _resume(self) -> None:
        """Resume the VM when frozen."""
        if self.freeze:
            self.mon.cmd("cont")

    def _halt(self) -> None:
        """Stop the VM when frozen."""
        if self.freeze:
            self.mon.cmd("stop")

    def key(self, k: str, hold: int | None = None) -> None:
        """Send one key with `hold` ms (default from the profile) and sleep its gap.

        ret/w are buttons, everything else a wheel step. Frozen: no wall gap; the call returns, VM
        stopped, once the hold has passed in guest time.
        """
        button = k in ("ret", "w")
        hold = hold or (
            self.timing.menu_hold
            if k == "w"
            else self.timing.button_hold
            if button
            else self.timing.wheel_hold
        )
        self._resume()
        start = self.mon.guest_us() if self.freeze else 0
        self.mon.key(k, hold)
        self.keys += 1
        self.last_key = time.time()
        if self.freeze:
            while (self.mon.guest_us() - start) & GUEST_CLOCK_MASK < hold * 1000:
                pass
            self._halt()
            return
        gap = self.timing.button_gap if button else self.timing.wheel_gap
        if gap:
            time.sleep(gap)

    def guard(self, frame: screenshot.Frame, *, force: bool = False) -> None:
        """Keep the LCD awake.

        A `q` at the top list row is a no-op; `force` also allows it during a post-SELECT settle,
        where the walker re-clamps before the next commit. Without either it fails loudly, because
        a wheel on a non-list screen (e.g. Now Playing) changes state.
        """
        gap = time.time() - self.last_key
        if gap <= self.timing.gap_limit:
            return
        if force or self.row(frame) == 0:
            self.keepalives += 1
            self.key("q")
            return
        msg = f"gap watchdog: {gap:.1f}s without a key, no harmless keepalive"
        raise RuntimeError(msg)

    def wait_until(
        self,
        pred: Callable[[screenshot.Frame], object],
        timeout: float | None = None,
        *,
        force: bool = False,
    ) -> screenshot.Frame | None:
        """Poll screenshots every `timing.poll` until `pred(frame)`; None on timeout."""
        deadline = time.time() + (self.timing.arrive if timeout is None else timeout)
        self._resume()
        try:
            while True:
                f = self.shot()
                self.guard(f, force=force)
                if pred(f):
                    return f
                if time.time() > deadline:
                    return None
                time.sleep(self.timing.poll)
        finally:
            self._halt()

    def settle(
        self,
        pred: Callable[[screenshot.Frame], object],
        ignore: screenshot.Ignore = (),
        timeout: float | None = None,
    ) -> screenshot.Frame | None:
        """Poll until `pred(frame)` holds and the frame has been stable outside `ignore`.

        "Stable" means unchanged for `timing.stable_us` microseconds of guest time, so a late-drawn
        pane or a held transition frame is not settled. Returns that frame. On timeout the latest
        frame is returned if `pred` holds on it (a screen that animates forever, e.g. Cover Flow),
        else None. The keepalive is forced, so a slow first-visit draw never trips the watchdog (the
        caller re-places the highlight before the next commit).
        """
        deadline = time.time() + (self.timing.arrive if timeout is None else timeout)
        self._resume()
        try:
            return self._settle(pred, ignore, deadline)
        finally:
            self._halt()

    def _settle(
        self,
        pred: Callable[[screenshot.Frame], object],
        ignore: screenshot.Ignore,
        deadline: float,
    ) -> screenshot.Frame | None:
        """Return the settled frame, polling until `deadline` (the body of settle())."""
        since = prev = f = None
        while time.time() < deadline:
            f = self.shot()
            self.guard(f, force=True)
            if pred(f) and prev is not None and screenshot.same(f, prev, ignore):
                now = self.mon.guest_us()
                since = now if since is None else since
                if (now - since) & GUEST_CLOCK_MASK >= self.timing.stable_us:
                    return f
            else:
                since = None
            prev = f
            time.sleep(self.timing.poll)
        return f if f is not None and pred(f) else None


class Emulator:
    """A running QEMU plus its Monitor. close() (or `with`) terminates it and removes temp files."""

    def __init__(
        self,
        proc: subprocess.Popen,
        mon: Monitor,
        out: str,
        cmd: list[str],
        cleanup: Sequence[str] = (),
    ) -> None:
        """Wrap a QEMU `proc`, its `mon`, the `out` directory and the `cmd` that started it."""
        self.proc, self.mon, self.out, self.cmd = proc, mon, out, cmd
        self._cleanup = list(cleanup)

    def wait_booted(self, timeout: float = 90, min_lit: int = 5000) -> screenshot.Frame:
        """Block until the first Language frame is dumpable and return that Frame.

        Primary: poll `xp /wx 0x38900078` (the display source address) until it is non-zero; it
        flips at the exact moment a screenshot has content, so no timer is involved. Fallback (the
        xp output is unparsable): poll screenshots until lit and stable. Both paths then wait for
        two identical frames, because the display source can flip mid-fade. FUDGE: the stability
        window is one 0.2 s interval; the Language right pane cycles languages, so callers that
        compare frames should ignore it (screen_tree.ignore_for('language')).
        """
        m, deadline = self.mon, time.time() + timeout
        while time.time() < deadline:
            r = m.cmd(f"xp /wx 0x{DISP_SRC:x}")
            mt = re.search(r":\s*0x([0-9a-fA-F]{8})", r)
            if not mt:
                return m.wait_stable(max(1, deadline - time.time()), min_lit=min_lit)
            if int(mt.group(1), 16):
                # The first presented frame fades (~0.15 s) and the Language -> main slide can start
                # soon after; 0.2 s apart skips a held fade frame. The right pane cycles languages.
                return m.wait_stable(max(1, deadline - time.time()), min_lit=min_lit, interval=0.2)
            time.sleep(0.005)
        msg = f"not booted within {timeout:g}s (display source still 0)"
        raise TimeoutError(msg)

    def close(self) -> None:
        """Close the monitor, terminate QEMU and remove temp files."""
        self.mon.close()
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        for p in self._cleanup:
            with contextlib.suppress(OSError):
                shutil.rmtree(p) if Path(p).is_dir() else Path(p).unlink()

    def __enter__(self) -> Self:
        """Return self."""
        return self

    def __exit__(self, *_a: object) -> None:
        """Close the emulator."""
        self.close()


def build_cmd(  # noqa: PLR0913 - one boot's flags
    kernel: str | Path,
    disk: str | Path,
    mon_arg: str,
    *,
    disk_format: str = "raw",
    snapshot: bool = True,
    gdb: int = 0,
    plugin: str = "",
    loadvm: str = "",
    paused: bool = False,
    fast: bool = False,
    extra: Sequence[str] = (),
    qemu: str | None = None,
) -> list[str]:
    """Return the QEMU argv for a headless boot of `kernel` with `disk` and the given monitor."""
    c = [
        qemu or os.environ.get("QEMU") or QEMU,
        "-M",
        "iPod-Classic",
        "-kernel",
        str(kernel),
        "-drive",
        f"if=ide,format={disk_format},file={disk}",
    ]
    if snapshot:
        c.append("-snapshot")
    c += ["-display", "none", "-serial", "null", "-L", PC_BIOS, "-monitor", mon_arg]
    if gdb:
        c += ["-gdb", f"tcp::{gdb}"]
    if plugin:  # relative lib paths resolve against the checkout
        lib, sep, rest = plugin.partition(",")
        c += [
            "-plugin",
            (lib if Path(lib).is_absolute() else str(ROOT / lib)) + sep + rest,
        ]
    if loadvm:
        c += ["-loadvm", loadvm]
    if paused:
        c.append("-S")
    if fast:
        c += FAST_ARGS
    return c + list(extra)


def launch(  # noqa: PLR0913 - one boot's flags
    image: str | Path = DEFAULT_IMAGE,
    disk: str | Path = DEFAULT_DISK,
    out: str | Path | None = None,
    *,
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
) -> Emulator:
    """Start QEMU on an osos .bin (-kernel) and connect a Monitor; return an Emulator.

    Every instance has its own `out` dir (qemu.out/qemu.err, the monitor socket), so up to
    MAX_PARALLEL can run side by side.

    disk    raw (or qcow2 with disk_format) image, read-only under -snapshot (default).
    port    TCP monitor on 127.0.0.1:port instead of the unix socket in `out`.
    loadvm/paused  `-loadvm NAME` / `-S` (snapshot restore).
    env     extra environment for QEMU on top of DEFAULT_ENV (FWGUID, IRAMSPILL); arbitrary keys
            are passed through.
    fast    TIMING FUDGE, opt-in: `-icount shift=auto,sleep=off`. The guest's virtual clock runs
            ahead of wall time whenever it idles, so boot is quicker but the guest's idle timer
            fires within a few wall seconds: the LCD blanks (battery-only) or the charger screen
            covers it, and keys do not wake a blanked display. Use fast for a boot-only screenshot,
            or with menu_navigation.keep_awake for a whole walk. Anything the guest paces against
            wall clock (audio/I2S, PMU RTC) drifts, so playback and music-DB behaviour are
            unverified. launch() defaults to reference wall-clock timing; goto_screen.py and
            capture_screenshots.py default to fast.
    """
    out = Path(out or tempfile.mkdtemp(prefix="emulator-", dir=scratch_runs()))
    out.mkdir(parents=True, exist_ok=True)
    image, disk, cleanup = abspath(image), abspath(disk), []
    sock = str(out / "mon.sock")
    if len(sock) > SOCKET_PATH_MAX:
        # Keep it short AND inside ROOT, so the whole run stays under one mountable directory.
        sd = tempfile.mkdtemp(prefix=".emu-sock-", dir=ROOT)
        sock, cleanup = str(Path(sd) / "mon.sock"), [*cleanup, sd]
        if len(sock) > SOCKET_PATH_MAX:
            msg = f"monitor socket path too long for sun_path: {sock}"
            raise ValueError(msg)
    if Path(sock).exists():
        Path(sock).unlink()
    mon_arg = f"tcp:127.0.0.1:{port},server,nowait" if port else f"unix:{sock},server,nowait"
    cmd = build_cmd(
        image,
        disk,
        mon_arg,
        disk_format=disk_format,
        snapshot=snapshot,
        gdb=gdb,
        plugin=plugin,
        loadvm=loadvm,
        paused=paused,
        fast=fast,
        extra=extra,
        qemu=qemu,
    )
    e = effective_env(env)
    # QEMU's monitor `memsave NAME` writes relative to its cwd, so run it in `out`.
    qemu_out = (out / "qemu.out").open("wb")
    qemu_err = (out / "qemu.err").open("wb")
    proc = subprocess.Popen(  # noqa: S603 - fixed local tool
        cmd,
        cwd=out,
        env=e,
        stdout=qemu_out,
        stderr=qemu_err,
    )
    try:
        mon = Monitor(
            None if port else sock,
            port or None,
            tmp=out / "_mon.ppm",
            alive=lambda: proc.poll() is None,
        )
    except OSError as ex:
        proc.kill()
        err = (out / "qemu.err").read_text(errors="replace")[-2000:]
        msg = f"{ex}\n--- {out}/qemu.err ---\n{err}"
        raise OSError(msg) from ex
    return Emulator(proc, mon, str(out), cmd, cleanup)


def scratch_runs() -> str:
    """Return the per-run scratch directory (SCRATCH/runs), created if needed."""
    d = Path(os.environ.get("SCRATCH") or ROOT / "assets/scratch") / "runs"
    d.mkdir(parents=True, exist_ok=True)
    return str(d)


# ---------------------------------------------------------------- CLI


def _out_dir() -> str:
    """Return the monitor CLI's output directory (EMU_OUT or scratch/runs/remote)."""
    d = Path(os.environ.get("EMU_OUT") or Path(scratch_runs()) / "remote")
    d.mkdir(parents=True, exist_ok=True)
    return str(d)


def _save(m: Monitor, path: str | Path) -> None:
    """Save a screenshot at <path>.ppm and <path>.png, and print its lit-pixel count."""
    p = Path(path)
    f = m.shot(f"{p}.ppm")
    f.write_png(f"{p}.png")
    print(f"{p.name}: nonblack={screenshot.lit(f)} -> {p}.png")


def main(a: list[str]) -> int:  # noqa: C901, PLR0912 - one flat CLI
    """Run the CLI (see the module docstring); return the exit status."""
    if not a:
        print(__doc__)
        return EXIT_USAGE
    if a[0] == "bootshot":
        rest, extra, disk, image, mons, fast = a[1:], [], DEFAULT_DISK, DEFAULT_IMAGE, [], False
        name = None
        while rest:
            t = rest.pop(0)
            if t == "--disk":
                disk = rest.pop(0)
            elif t == "--image":
                image = rest.pop(0)
            elif t == "--mon":
                mons.append(rest.pop(0))
            elif t == "--fast":
                fast = True
            elif t == "--":
                extra, rest = rest, []
            else:
                name = t
        shots = Path(scratch_runs()).parent / "screens"
        shots.mkdir(parents=True, exist_ok=True)
        with launch(
            image,
            disk,
            out=tempfile.mkdtemp(prefix="bootshot-", dir=scratch_runs()),
            fast=fast,
            extra=extra,
        ) as e:
            e.wait_booted()
            for c in mons:
                print(e.mon.cmd(c))
            _save(e.mon, shots / (name or "bootshot"))
        return 0
    m = Monitor()
    out = Path(_out_dir())
    if a[0] == "shot":
        _save(m, out / a[1])
    elif a[0] == "key":
        m.key(a[1], int(a[2]) if len(a) > HOLD_ARG_INDEX else None)
    elif a[0] == "raw":
        sys.stdout.write(m.cmd(a[1]))
    elif a[0] == "seq":
        for tok in a[1:]:
            k, sep, v = tok.partition(":")
            if sep and k == "wait":
                time.sleep(float(v))
            elif sep:
                m.key(k, int(v), wait=True)
            else:
                _save(m, out / tok)
    else:
        print(__doc__)
        return EXIT_USAGE
    m.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

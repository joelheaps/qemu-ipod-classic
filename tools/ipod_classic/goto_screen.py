#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = ["msgspec"]
# ///
r"""goto_screen.py -- boot an osos image and drive the UI to a screen of screen_tree.json.

Exit 0 and `NAV_OK=1`, or non-zero and `NAV_FAIL=<screen>`.

Library:
    from goto_screen import boot, start_session
    with qemu_session.launch(image, out=d) as e:
        session = start_session(e, tree, outdir=d)     # waits for the Language screen
        menu_navigation.navigate_to(session, tree, target, session.here)
Screens, rows and watch areas are data in screen_tree.json (screen_tree.py). The image is an osos
.bin passed to -kernel, for example assets/firmware/2.0.5/osos_decrypted.bin. The disk is read-only
(-snapshot) unless --no-snapshot. QEMU runs natively from $BUILD_DIR (see qemu_session.py).

Two timing profiles (qemu_session.Timing): REFERENCE (wall-clock pacing, one key at a time) and FAST
(`-icount shift=auto,sleep=off`). Under FAST the guest idle timer runs ahead of wall time, so a wall
gap over ~2.5 s without a key blanks the LCD and no key wakes it. Every wait therefore polls
screenshots and keeps a key flowing; `keep_awake(session, tree)` removes the cause
(Settings > Backlight > Always On) for scripted scenarios.

CLI (`tools/ipod_classic/goto_screen.py ...`); env in parentheses:
  --to SCREEN (default now_playing; any screen of screen_tree.json)  --image FILE (OSOS)
  --disk FILE (DISK)
  --out DIR (OUT)  --plugin SPEC (PLUGIN)  --gdb PORT (GDB)  --port N (PORT, TCP monitor)
  --fwguid HEX (FWGUID)  --no-fast (reference timing; the default is -icount shift=auto,sleep=off,
  see qemu_session.launch)
  --keep-awake  set Settings > Backlight > Always On first (FAST timing then never blanks the LCD)
  --post S  settle wait on the target before the final shot/dumps (default 5)
  --no-snapshot  --disk-format raw|qcow2  --save NAME (savevm; needs qcow2 + --no-snapshot)
  --dump ADDR:LEN:FILE (repeatable)  --gdb-pre / --gdb-post FILE (gdb batch scripts before the last
  SELECT / after the target; need --gdb)

Examples:
  tools/ipod_classic/goto_screen.py --to settings
  tools/ipod_classic/goto_screen.py --to songs --no-fast
  env OSOS=assets/firmware/2.0.5/osos_decrypted.bin \\
      tools/ipod_classic/goto_screen.py --to songs --gdb 1413

"""

import argparse
import dataclasses
import os
import subprocess
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "lib"))
import menu_navigation  # noqa: E402
import qemu_session  # noqa: E402
import screen_tree  # noqa: E402
import screenshot  # noqa: E402

MIN_LIT = menu_navigation.MIN_LIT


@dataclass
class WalkArgs:
    """The parsed CLI fields `walk_to` reads."""

    gdb: int = 0
    gdb_pre: str | None = None


def log(msg: str) -> None:
    """Print `msg` (the default log of start_session)."""
    print(msg, flush=True)


def gdb_run(port: int, script: str) -> None:
    """Run the gdb batch `script` against the emulator's gdb stub on `port`, then detach."""
    log(f"navigate: gdb -x {script}")
    cmd = [
        "gdb-multiarch",
        "-nx",
        "-batch",
        "-ex",
        "set arm abi AAPCS",
        "-ex",
        f"target remote :{port}",
        "-x",
        str(Path(script).resolve()),
        "-ex",
        "detach",
        "-ex",
        "quit",
    ]
    subprocess.run(cmd, check=True)  # noqa: S603 - fixed local tool


def snap(
    mon: qemu_session.Monitor, name: str, outdir: str | Path, f: screenshot.Frame | None = None
) -> screenshot.Frame:
    """Save <name>.ppm/.png in `outdir` and return the frame.

    Uses frame `f` when given (the settled frame, not a fresh screenshot that could catch an
    animation mid-step), else a new screenshot.
    """
    ppm, png = Path(outdir) / f"{name}.ppm", Path(outdir) / f"{name}.png"
    if f is None:
        f = mon.shot(ppm)
    else:
        f.write_ppm(ppm)
    f.write_png(png)
    return f


def boot(e: qemu_session.Emulator, secs: float = 90) -> screenshot.Frame | None:
    """Wait up to `secs` for the Language screen and return its settled frame.

    Uses Emulator.wait_booted (display-source poll, no fixed sleep); None on a timeout.
    """
    try:
        return e.wait_booted(secs)
    except TimeoutError:
        return None


def start_session(  # noqa: PLR0913 - the run setup is one object
    e: qemu_session.Emulator,
    tree: screen_tree.ScreenTree,
    outdir: str | Path,
    *,
    fast: bool = True,
    first: screenshot.Frame | None = None,
    log: Callable[[str], None] = log,
    checkpoints: bool = False,
) -> menu_navigation.Session | None:
    """Return a menu_navigation.Session on the booted emulator `e`, on the root (Language) screen.

    Every screen it later passes is saved as <screen>.ppm/.png in `outdir`; the root is saved now.
    `checkpoints` turns on snapshot checkpoints (the VM needs a qcow2 disk and no -snapshot).
    `fast` picks the timing profile (it must match how `e` was launched), `first` the Language frame
    if the caller already booted (else `boot(e)` waits for it), and `log` receives progress
    messages. None when the boot timed out.
    """
    first = first or boot(e)
    if first is None:
        return None
    timing = qemu_session.FAST if fast else qemu_session.REFERENCE

    def save(name: str, f: screenshot.Frame) -> None:
        snap(e.mon, name, outdir, f)

    save(tree.root.name, first)
    return menu_navigation.Session(
        e.mon, timing, tree.root, first, save, log, checkpoints=checkpoints
    )


def _anything(_f: screenshot.Frame) -> bool:
    """Return True (a settle predicate for a screen with nothing to recognise)."""
    return True


def keep_awake(
    session: menu_navigation.Session, tree: screen_tree.ScreenTree, log: Callable[[str], None] = log
) -> screenshot.Frame:
    """Set Settings > Backlight > Always On, then back out to the main menu.

    Under FAST timing the guest's backlight timeout (10 s by default) fires within a few wall
    seconds of the last key and blanks the LCD for good. With Always On a scenario may idle as long
    as it likes, so it needs no keepalive presses. The setting lives in the guest's RAM (the disk is
    -snapshot), and it changes what Settings and Backlight show, so a screenshot survey leaves it
    off unless asked (--keep-awake). It also switches the keepalive watchdog off
    (Timing.gap_limit = inf): with the screen always on, the watchdog's stray wheel-up presses would
    only disturb the scenario. Returns the settled main-menu frame.
    """
    names = screen_tree.by_name(tree)
    if walk_to(session, tree, names["backlight"], WalkArgs()) != 0:
        msg = "keep_awake: could not reach Settings > Backlight"
        raise RuntimeError(msg)
    pacer = session.pacer
    for _ in range(3):  # 10 Seconds -> 20 -> 30 -> Always On
        before = pacer.shot()
        pacer.key("e")
        pacer.wait_until(lambda f, b=before: not screenshot.same(f, b), timeout=1.0)
    pacer.key("ret")  # applies in place; the screen does not change
    pacer.settle(_anything, timeout=5)
    for _ in range(2):  # Backlight -> Settings -> main
        before = pacer.shot()
        pacer.key("w")
        pacer.wait_until(lambda f, b=before: not screenshot.same(f, b), timeout=3.0)
        session.last = pacer.settle(_anything, timeout=5) or session.last
    session.here = names["main"]
    # The keepalive watchdog stopped the LCD blanking; with Always On it only injects stray key
    # presses into the scenario, so switch it off.
    session.pacer.timing = session.timing = dataclasses.replace(
        session.timing, gap_limit=float("inf")
    )
    # Settings now lists "Always On" where it showed "10 Seconds": remembered frames are stale.
    session.entered.clear()
    session.learned.clear()
    log("navigate: backlight Always On")
    return session.last


def walk_to(
    session: menu_navigation.Session,
    tree: screen_tree.ScreenTree,
    target: screen_tree.ScreenDefinition,
    a: WalkArgs,
) -> int:
    """Navigate to `target` (an overlay is awaited, not navigated).

    Returns 0 on arrival, else 1 after logging NAV_FAIL. With `a.gdb_pre` the script runs on the
    parent screen, just before the last SELECT.
    """
    try:
        if target.name in {o.name for o in tree.overlays}:
            menu_navigation.await_overlay(session, target)
        else:
            if a.gdb_pre:
                parent = screen_tree.chain(tree, target.name)[-2]
                menu_navigation.navigate_to(session, tree, parent, session.here)
                gdb_run(a.gdb, a.gdb_pre)
            menu_navigation.navigate_to(session, tree, target, session.here)
    except (menu_navigation.NavigationError, TimeoutError) as error:
        log(f"navigate: {error}")
        log(f"NAV_FAIL={target.name}")
        return 1
    return 0


def main() -> int:  # noqa: C901, PLR0915 - one flat CLI
    """Run the CLI (see the module docstring); return the exit status."""
    tree = screen_tree.load()
    names = screen_tree.by_name(tree)
    ap = argparse.ArgumentParser(
        usage=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    env = os.environ.get
    ap.add_argument("--image", default=env("OSOS") or qemu_session.DEFAULT_IMAGE)
    ap.add_argument("--disk", default=env("DISK") or qemu_session.DEFAULT_DISK)
    ap.add_argument("--to", default="now_playing", choices=sorted(names))
    ap.add_argument(
        "--out",
        default=env("OUT") or str(Path(qemu_session.scratch_runs()) / "navigate"),
    )
    ap.add_argument("--plugin", default=env("PLUGIN", ""))
    ap.add_argument("--gdb", type=int, default=int(env("GDB") or 0))
    ap.add_argument("--fwguid", default=env("FWGUID") or "")
    ap.add_argument("--port", type=int, default=int(env("PORT") or 0))
    ap.add_argument("--post", type=float, default=5)
    ap.add_argument(
        "--no-fast",
        dest="fast",
        action="store_false",
        help="reference timing instead of -icount shift=auto,sleep=off (see qemu_session.launch)",
    )
    ap.add_argument("--no-snapshot", action="store_true")
    ap.add_argument("--save", default="")
    ap.add_argument("--disk-format", default="raw", choices=["raw", "qcow2"])
    ap.add_argument("--dump", action="append", default=[])
    ap.add_argument(
        "--keep-awake",
        action="store_true",
        help="first set Settings > Backlight > Always On (FAST timing then never blanks)",
    )
    ap.add_argument("--gdb-pre")
    ap.add_argument("--gdb-post")
    args = ap.parse_args()
    if (args.gdb_pre or args.gdb_post) and not args.gdb:
        ap.error("--gdb-pre/--gdb-post need --gdb PORT")
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    status = 1
    try:
        e = qemu_session.launch(
            args.image,
            args.disk,
            out,
            disk_format=args.disk_format,
            snapshot=not args.no_snapshot,
            gdb=args.gdb,
            plugin=args.plugin,
            port=args.port,
            fast=args.fast,
            env={"FWGUID": args.fwguid} if args.fwguid else None,
        )
    except OSError as ex:
        log(f"navigate: {ex}")
        return 1
    log(f"navigate: {' '.join(e.cmd)}")
    with e:
        m = e.mon
        session = start_session(e, tree, out, fast=args.fast)
        if session is None:
            log("NAV_FAIL=language")
        else:
            if args.keep_awake:
                keep_awake(session, tree)
            status = walk_to(
                session, tree, names[args.to], WalkArgs(gdb=args.gdb, gdb_pre=args.gdb_pre)
            )
        if status == 0:
            time.sleep(args.post)  # FUDGE: plain wait so playback/animations reach their state
            f = snap(m, "settled", out)
            log(f"navigate: settled {args.to}: {screenshot.lit(f)} lit")
            if args.save:
                m.cmd(f"savevm {args.save}", timeout=120)
                if args.save not in m.cmd("info snapshots"):
                    log(f"navigate: ASSERT savevm {args.save}: not present in info snapshots")
                    status = 1
                else:
                    log(f"navigate: savevm {args.save} ok")
            for spec in args.dump:
                addr, length, path = spec.split(":", 2)
                m.cmd(f'memsave {addr} {length} "{path}"')
                log(f"navigate: memsave {addr} {length} -> {path}")
            if args.gdb_post:
                gdb_run(args.gdb, args.gdb_post)
    if status == 0:
        log(f"NAV_OK=1 screen={args.to} out={out}")
    return status


if __name__ == "__main__":
    sys.exit(main())

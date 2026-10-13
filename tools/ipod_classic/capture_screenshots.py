#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = ["msgspec"]
# ///
"""capture_screenshots.py -- cold-boot an osos image and dump many UI screens as fast as possible.

    tools/ipod_classic/capture_screenshots.py assets/firmware/2.0.5/osos_decrypted.bin
    tools/ipod_classic/capture_screenshots.py IMAGE --screens settings,about,brightness --parallel 4
    tools/ipod_classic/screenshot_diff.py DIR_A DIR_B    # compare two surveys

Options: IMAGE (required; any osos image the emulator takes as -kernel), --screens a,b,... (default
every non-overlay screen), --without a,b,... (leave screens out of the default set), --out DIR
(default: the scratch runs directory), --disk FILE (default assets/disks/Emedia.img), --parallel N
(default 8, at most 8), --no-fast (reference timing), --cold-boot (every branch cold-boots instead
of restoring the snapshot of one boot), --keep-awake (see below).

Boots the image with -kernel, once per charger state, saves a VM snapshot at the Language screen and
restores a copy of it per branch (--cold-boot: every branch boots from scratch), navigates with
menu_navigation.navigate_to and writes <out>/<screen hierarchy>.png for each screen reached
(intermediates included), named from the parent chain of screen_tree.json starting at main, joined
with underscores (main_settings_about.png; language.png is standalone). Names are stable across
runs, so
`screenshot_diff.py DIR_A DIR_B` pairs two surveys. Exit status is non-zero on a failed boot, a
failed navigation check (menu_navigation.verify), an all-black frame, or a requested screen with no
frame; failures are printed as `FAIL <screen>: why`.

Screens come from screen_tree.json. The requested set is split into branches -- the leaves of the
requested subtree, in depth-first order, chunked so siblings share a boot -- and each branch gets
its own cold boot, up to --parallel at once (qemu_session.MAX_PARALLEL is 8). A branch's walk dumps
the screens on the way, so one boot covers a branch. The disk defaults to Emedia.img, which serves
the menus and the Cover Flow / Songs / Now Playing screens alike.

Timing: FAST is the default (`-icount shift=auto,sleep=off`, see qemu_session.launch) and
`--no-fast` selects reference timing. The guest idle timer then runs ahead of wall time, so
menu_navigation.navigate_to never lets a wall gap over ~2 s pass without a key; `--keep-awake`
removes the cause instead (Settings > Backlight > Always On in each boot, see
goto_screen.keep_awake), at the price of changed Settings and Backlight screenshots. The boot's
PMUCHARGER comes from the
`requires_charger_state` of the screens in that branch (`battery` unless a screen asks for
`wall`/`usb`), so charger screens and battery-only screens never share a boot.
"""

import argparse
import atexit
import concurrent.futures
import math
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from collections.abc import Callable
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "lib"))
import goto_screen  # noqa: E402
import menu_navigation  # noqa: E402
import qemu_session  # noqa: E402
import screen_tree  # noqa: E402
import screenshot  # noqa: E402

MIN_LIT = menu_navigation.MIN_LIT
SNAPSHOT = "language"  # name of the VM snapshot taken at the Language screen
TREE = screen_tree.load()
NAMES = screen_tree.by_name(TREE)
PARENTS = screen_tree.parents(TREE)
OVERLAYS = {overlay.name for overlay in TREE.overlays}
_PRINT_LOCK = threading.Lock()


# ---------------------------------------------------------------- screen_tree.json access


def full_name(name: str) -> str:
    """Return the hierarchical output name of `name`.

    The parent chain from main, joined with underscores, for example main_settings_about. The
    language screen and the overlays are standalone.
    """
    parts = []
    current: str | None = name
    while current:
        parts.append(current)
        current = PARENTS[current]
    parts.reverse()
    if len(parts) > 1 and parts[0] == TREE.root.name:
        parts = parts[1:]
    return "_".join(parts)


def charger_of(name: str) -> str:
    """Return the PMUCHARGER state the screen needs; screens without one boot on battery."""
    return NAMES[name].requires_charger_state or "battery"


def _split(text: str) -> list[str]:
    """Return the comma-separated names in `text`, whitespace stripped and empties dropped."""
    return [n.strip() for n in text.split(",") if n.strip()]


def select_screens(spec: str, without: str = "") -> tuple[list[str], list[str]]:
    """Return (requested, unknown) for a comma-separated screen list.

    Empty `spec` selects all non-overlay screens. `without` is a comma-separated list of screens to
    leave out of the selection (for an image that lacks them); an unknown name in either list is
    returned in `unknown`.

    Library entry point for callers that drive `survey()`.
    """
    left_out = _split(without)
    requested = _split(spec) or [n for n in screen_tree.depth_first(TREE) if n not in OVERLAYS]
    unknown = [n for n in requested + left_out if n not in NAMES]
    return [n for n in requested if n not in left_out], unknown


def branches(requested: list[str], parallel: int) -> list[tuple[str, list[str]]]:
    """Return (charger, screens) groups for the requested screens.

    A group holds requested screens with no requested descendant, in DFS order, split by charger so
    one boot never mixes charger states, then chunked into `parallel`-ish contiguous groups so a
    group's screens share most of their navigation path.
    """
    sel = set(requested)
    covered = set()
    for name in sel:
        parent = PARENTS[name]
        while parent:
            if parent in sel:
                covered.add(parent)
            parent = PARENTS[parent]
    leaves = [n for n in screen_tree.depth_first(TREE) if n in sel and n not in covered]
    groups = []
    for charger in dict.fromkeys(charger_of(n) for n in leaves):
        subset = [n for n in leaves if charger_of(n) == charger]
        size = max(1, math.ceil(len(subset) / max(1, min(parallel, len(subset)))))
        groups.extend((charger, subset[i : i + size]) for i in range(0, len(subset), size))
    return groups


# ---------------------------------------------------------------- one branch = one cold boot


def _branch_log(index: int) -> Callable[[str], None]:
    """Return a log that prefixes each line with the branch `index`."""

    def log(msg: str) -> None:
        with _PRINT_LOCK:
            print(f"[{index}] {msg}", flush=True)

    return log


def staging_dir() -> Path:
    """Return a fresh private directory for one survey's VM files, on tmpfs when there is one.

    Each SELECT saves a VM snapshot of about 12 MB into the branch's qcow2 overlay: in RAM a save is
    fast and nothing is written to disk. A unix monitor socket is nested in the directory, so keep
    the path well under the 100-char sun_path limit. Without /dev/shm the directory is under the
    scratch directory.
    """
    ram = Path("/dev/shm")  # noqa: S108 - the tmpfs mount point, not a predictable temp name
    if ram.is_dir() and os.access(ram, os.W_OK):
        return Path(tempfile.mkdtemp(prefix="osos-survey-", dir=ram))
    parent = Path(qemu_session.scratch_runs()).parent / "survey-stages"
    parent.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix="s-", dir=parent))


def make_base(
    image: str | Path, disk: str | Path, work: str | Path, *, fast: bool, env: dict[str, str]
) -> Path:
    """Cold-boot `image` once to the Language screen and save a VM snapshot of it.

    Returns the qcow2 overlay holding the snapshot, backed by the raw `disk` (never written). Each
    branch then restores a copy instead of cold-booting. One per charger state: the charger is read
    at boot, so it is part of what the snapshot holds.
    """
    work = Path(work)
    work.mkdir(parents=True, exist_ok=True)
    overlay = work / "base.qcow2"
    subprocess.run(  # noqa: S603 - fixed local tool
        [qemu_session.QEMU_IMG, "create", "-q", "-f", "qcow2", "-b", disk, "-F", "raw", overlay],
        check=True,
    )
    with qemu_session.launch(
        image, overlay, out=work, disk_format="qcow2", snapshot=False, fast=fast, env=env
    ) as e:
        e.wait_booted()
        e.mon.cmd(f"savevm {SNAPSHOT}", timeout=120)
    return overlay


def run_branch(  # noqa: PLR0913, PLR0917 - one branch job
    index: int,
    group: list[str],
    image: str | Path,
    disk: str | Path,
    stage: str | Path,
    env: dict[str, str],
    *,
    fast: bool,
    base: Path | None = None,
    keep_awake: bool = False,
) -> list[str]:
    """Boot once and navigate to each screen of the branch, saving every screen passed.

    `index` labels the log lines, `group` is the screen names to visit in order, `stage` the
    directory the frames land in, `fast` the timing profile and `env` the extra QEMU environment
    (PMUCHARGER). `base` is a make_base overlay to restore a copy of instead of cold-booting.
    `keep_awake` first sets Settings > Backlight > Always On (changes the Settings and Backlight
    screens the survey photographs). Returns the error list (empty on success).
    """
    log, errors = _branch_log(index), []
    stage = Path(stage)
    stage.mkdir(parents=True, exist_ok=True)
    started = time.time()
    log(f"branch {index}: {','.join(group)}")
    kwargs: dict = {}
    if base:
        disk = stage / "disk.qcow2"
        shutil.copy(base, disk)
        kwargs = {"disk_format": "qcow2", "snapshot": False, "loadvm": SNAPSHOT}
    try:
        with qemu_session.launch(image, disk, out=stage, fast=fast, env=env, **kwargs) as e:
            session = goto_screen.start_session(
                e, TREE, stage, fast=fast, log=log, checkpoints=bool(base)
            )
            if session is None:
                return ["boot: the Language screen never appeared"]
            if keep_awake:
                goto_screen.keep_awake(session, TREE, log=log)
            for name in group:
                try:
                    if name in OVERLAYS:
                        goto_screen.snap(
                            e.mon, name, stage, menu_navigation.await_overlay(session, NAMES[name])
                        )
                    else:
                        menu_navigation.navigate_to(session, TREE, NAMES[name], session.here)
                except (menu_navigation.NavigationError, TimeoutError) as ex:
                    errors.append(f"{name}: {ex}")
            log(
                f"done in {time.time() - started:.1f}s: {session.pacer.shots} screenshots "
                f"{dict(session.events)}"
            )
    except Exception as ex:  # noqa: BLE001 - launch/connection
        errors.append(f"{image}: launch: {ex}")
    return errors


# ---------------------------------------------------------------- collect and report


def survey(  # noqa: C901, PLR0913 - one survey job
    image: str | Path,
    disk: str | Path,
    out: str | Path,
    requested: list[str],
    parallel: int,
    *,
    fast: bool,
    cold_boot: bool = False,
    keep_awake: bool = False,
) -> list[str]:
    """Boot `image` across parallel branches and merge the frames into `out`.

    `requested` is a list of screen names (see select_screens), `parallel` the most concurrent boots
    and `fast` the timing profile. Writes <out>/<full_name(screen)>.png per screen reached. Unless
    `cold_boot`, the image is cold-booted once per charger state and every branch restores a
    snapshot of that boot (see make_base); with `cold_boot` each branch cold-boots on its own.
    Returns the error list.
    """
    groups = branches(requested, parallel)
    print(f"survey: {len(requested)} screens -> {len(groups)} branch(es), parallel {parallel}")
    if not groups:
        return ["no screens to survey"]
    stage_root = staging_dir()
    atexit.register(shutil.rmtree, stage_root, ignore_errors=True)  # also after a crash
    stages = [stage_root / f"b{i}" for i in range(len(groups))]
    errors = []
    envs = {
        charger: {"PMUCHARGER": {"wall": "adapter", "usb": "usb"}.get(charger, charger)}
        for charger, _ in groups
    }
    with concurrent.futures.ThreadPoolExecutor(max_workers=parallel) as pool:
        bases = {}
        if not cold_boot:
            made = {
                c: pool.submit(make_base, image, disk, stage_root / f"base-{c}", fast=fast, env=env)
                for c, env in envs.items()
            }
            started = time.time()
            bases = {c: future.result() for c, future in made.items()}
            print(f"survey: boot snapshot ready in {time.time() - started:.1f}s", flush=True)
        futures = []
        for i, (charger, g) in enumerate(groups):
            futures.append(
                pool.submit(
                    run_branch,
                    i,
                    g,
                    image,
                    disk,
                    stages[i],
                    envs[charger],
                    fast=fast,
                    base=bases.get(charger),
                    keep_awake=keep_awake,
                )
            )
        for future in concurrent.futures.as_completed(futures):
            errors.extend(future.result())
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    order = [n for n in screen_tree.depth_first(TREE) if n in requested]
    picked = {}
    for stage in stages:  # first branch that reached a screen wins
        for name in screen_tree.depth_first(TREE):
            path = stage / f"{name}.ppm"
            if name not in picked and path.exists():
                picked[name] = screenshot.read(path)
    for name, f in picked.items():
        f.write_png(out / f"{full_name(name)}.png")
    for name in order:
        f = picked.get(name)
        if f is None:
            errors.append(f"{full_name(name)}: no frame captured")
            continue
        if screenshot.lit(f) < MIN_LIT:
            errors.append(f"{full_name(name)}: all-black frame ({screenshot.lit(f)} lit)")
    shutil.rmtree(stage_root, ignore_errors=True)
    return errors


# ---------------------------------------------------------------- CLI


def main() -> int:
    """Run the CLI (see the module docstring); return the exit status, 1 on any failure."""
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("image", help="osos .bin to survey (cold boot with -kernel)")
    parser.add_argument(
        "--screens", default="", help="comma-separated screens (default: all non-overlay)"
    )
    parser.add_argument(
        "--without",
        default="",
        help="comma-separated screens to leave out (e.g. for an image that lacks them)",
    )
    parser.add_argument("--out", default=str(Path(qemu_session.scratch_runs()) / "survey"))
    parser.add_argument(
        "--disk", default=qemu_session.DEFAULT_DISK, help="disk image (default Emedia.img)"
    )
    parser.add_argument(
        "--parallel",
        type=int,
        default=8,
        help=f"concurrent cold boots (max {qemu_session.MAX_PARALLEL})",
    )
    parser.add_argument(
        "--no-fast",
        dest="fast",
        action="store_false",
        help="reference timing instead of -icount shift=auto,sleep=off",
    )
    parser.add_argument(
        "--keep-awake",
        action="store_true",
        help="first set Settings > Backlight > Always On in every boot (changes the Settings and "
        "Backlight screenshots; removes the fast-timing LCD blanking)",
    )
    parser.add_argument(
        "--cold-boot",
        action="store_true",
        help="cold-boot every branch instead of restoring one boot snapshot",
    )
    args = parser.parse_args()

    requested, unknown = select_screens(args.screens, args.without)
    if unknown:
        parser.error(f"unknown screens: {', '.join(unknown)}")
    # Real paths, so a symlinked (workspace) image or disk is used as it is.
    image, disk = Path(args.image).resolve(), Path(args.disk).resolve()
    out = Path(args.out).resolve()
    parallel = max(1, min(args.parallel, qemu_session.MAX_PARALLEL))
    print(f"survey: {image} -> {out}", flush=True)
    started = time.time()
    errors = survey(
        image,
        disk,
        out,
        requested,
        parallel,
        fast=args.fast,
        cold_boot=args.cold_boot,
        keep_awake=args.keep_awake,
    )
    for e in errors:
        print(f"survey: FAIL {e}", file=sys.stderr)
    if not errors:
        print(f"survey: ok, {len(requested)} screen(s) in {time.time() - started:.1f}s -> {out}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())

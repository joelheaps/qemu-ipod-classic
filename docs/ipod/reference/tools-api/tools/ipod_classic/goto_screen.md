# Goto Screen

[tools Index](../../README.md#tools-index) / [Tools](../index.md#tools) / [Ipod Classic](./index.md#ipod-classic) / Goto Screen

> Auto-generated documentation for [tools.ipod_classic.goto_screen](../../../../../../tools/ipod_classic/goto_screen.py) module.

```text
goto_screen.py -- boot an osos image and drive the UI to a screen of screen_tree.json.

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
```

- [Goto Screen](#goto-screen)
  - [WalkArgs](#walkargs)
  - [\_anything](#_anything)
  - [boot](#boot)
  - [gdb_run](#gdb_run)
  - [keep_awake](#keep_awake)
  - [log](#log)
  - [main](#main)
  - [snap](#snap)
  - [start_session](#start_session)
  - [walk_to](#walk_to)

## WalkArgs

[Show source in goto_screen.py:66](../../../../../../tools/ipod_classic/goto_screen.py#L66)

The parsed CLI fields [walk_to](#walk_to) reads.

#### Signature

```python
class WalkArgs: ...
```

## \_anything

[Show source in goto_screen.py:159](../../../../../../tools/ipod_classic/goto_screen.py#L159)

Return True (a settle predicate for a screen with nothing to recognise).

#### Signature

```python
def _anything(_f: screenshot.Frame) -> bool: ...
```

## boot

[Show source in goto_screen.py:116](../../../../../../tools/ipod_classic/goto_screen.py#L116)

Wait up to `secs` for the Language screen and return its settled frame.

Uses Emulator.wait_booted (display-source poll, no fixed sleep); None on a timeout.

#### Signature

```python
def boot(e: qemu_session.Emulator, secs: float = 90) -> screenshot.Frame | None: ...
```

## gdb_run

[Show source in goto_screen.py:78](../../../../../../tools/ipod_classic/goto_screen.py#L78)

Run the gdb batch `script` against the emulator's gdb stub on `port`, then detach.

#### Signature

```python
def gdb_run(port: int, script: str) -> None: ...
```

## keep_awake

[Show source in goto_screen.py:164](../../../../../../tools/ipod_classic/goto_screen.py#L164)

Set Settings > Backlight > Always On, then back out to the main menu.

Under FAST timing the guest's backlight timeout (10 s by default) fires within a few wall seconds of the last key and blanks the LCD for good. With Always On a scenario may idle as long as it likes, so it needs no keepalive presses. The setting lives in the guest's RAM (the disk is -snapshot), and it changes what Settings and Backlight show, so a screenshot survey leaves it off unless asked (--keep-awake). It also switches the keepalive watchdog off (Timing.gap_limit = inf): with the screen always on, the watchdog's stray wheel-up presses would only disturb the scenario. Returns the settled main-menu frame.

#### Signature

```python
def keep_awake(
    session: menu_navigation.Session,
    tree: screen_tree.ScreenTree,
    log: Callable[[str], None] = log,
) -> screenshot.Frame: ...
```

#### See also

- [log](#log)

## log

[Show source in goto_screen.py:73](../../../../../../tools/ipod_classic/goto_screen.py#L73)

Print `msg` (the default log of start_session).

#### Signature

```python
def log(msg: str) -> None: ...
```

## main

[Show source in goto_screen.py:233](../../../../../../tools/ipod_classic/goto_screen.py#L233)

Run the CLI (see the module docstring); return the exit status.

#### Signature

```python
def main() -> int: ...
```

## snap

[Show source in goto_screen.py:99](../../../../../../tools/ipod_classic/goto_screen.py#L99)

Save <name>.ppm/.png in `outdir` and return the frame.

Uses frame `f` when given (the settled frame, not a fresh screenshot that could catch an animation mid-step), else a new screenshot.

#### Signature

```python
def snap(
    mon: qemu_session.Monitor,
    name: str,
    outdir: str | Path,
    f: screenshot.Frame | None = None,
) -> screenshot.Frame: ...
```

## start_session

[Show source in goto_screen.py:127](../../../../../../tools/ipod_classic/goto_screen.py#L127)

Return a menu_navigation.Session on the booted emulator `e`, on the root (Language) screen.

Every screen it later passes is saved as <screen>.ppm/.png in `outdir`; the root is saved now. `checkpoints` turns on snapshot checkpoints (the VM needs a qcow2 disk and no -snapshot). `fast` picks the timing profile (it must match how `e` was launched), `first` the Language frame if the caller already booted (else `boot(e)` waits for it), and [log](#log) receives progress messages. None when the boot timed out.

#### Signature

```python
def start_session(
    e: qemu_session.Emulator,
    tree: screen_tree.ScreenTree,
    outdir: str | Path,
    fast: bool = True,
    first: screenshot.Frame | None = None,
    log: Callable[[str], None] = log,
    checkpoints: bool = False,
) -> menu_navigation.Session | None: ...
```

#### See also

- [log](#log)

## walk_to

[Show source in goto_screen.py:206](../../../../../../tools/ipod_classic/goto_screen.py#L206)

Navigate to `target` (an overlay is awaited, not navigated).

Returns 0 on arrival, else 1 after logging NAV_FAIL. With `a.gdb_pre` the script runs on the parent screen, just before the last SELECT.

#### Signature

```python
def walk_to(
    session: menu_navigation.Session,
    tree: screen_tree.ScreenTree,
    target: screen_tree.ScreenDefinition,
    a: WalkArgs,
) -> int: ...
```

#### See also

- [WalkArgs](#walkargs)

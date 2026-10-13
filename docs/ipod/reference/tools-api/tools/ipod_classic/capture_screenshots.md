# Capture Screenshots

[tools Index](../../README.md#tools-index) / [Tools](../index.md#tools) / [Ipod Classic](./index.md#ipod-classic) / Capture Screenshots

> Auto-generated documentation for [tools.ipod_classic.capture_screenshots](../../../../../../tools/ipod_classic/capture_screenshots.py) module.

```text
capture_screenshots.py -- cold-boot an osos image and dump many UI screens as fast as possible.

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
```

- [Capture Screenshots](#capture-screenshots)
  - [\_branch_log](#_branch_log)
  - [\_split](#_split)
  - [branches](#branches)
  - [charger_of](#charger_of)
  - [full_name](#full_name)
  - [main](#main)
  - [make_base](#make_base)
  - [run_branch](#run_branch)
  - [select_screens](#select_screens)
  - [staging_dir](#staging_dir)
  - [survey](#survey)

## \_branch_log

[Show source in capture_screenshots.py:147](../../../../../../tools/ipod_classic/capture_screenshots.py#L147)

Return a log that prefixes each line with the branch `index`.

#### Signature

```python
def _branch_log(index: int) -> Callable[[str], None]: ...
```

## \_split

[Show source in capture_screenshots.py:100](../../../../../../tools/ipod_classic/capture_screenshots.py#L100)

Return the comma-separated names in `text`, whitespace stripped and empties dropped.

#### Signature

```python
def _split(text: str) -> list[str]: ...
```

## branches

[Show source in capture_screenshots.py:120](../../../../../../tools/ipod_classic/capture_screenshots.py#L120)

Return (charger, screens) groups for the requested screens.

A group holds requested screens with no requested descendant, in DFS order, split by charger so one boot never mixes charger states, then chunked into `parallel`-ish contiguous groups so a group's screens share most of their navigation path.

#### Signature

```python
def branches(requested: list[str], parallel: int) -> list[tuple[str, list[str]]]: ...
```

## charger_of

[Show source in capture_screenshots.py:95](../../../../../../tools/ipod_classic/capture_screenshots.py#L95)

Return the PMUCHARGER state the screen needs; screens without one boot on battery.

#### Signature

```python
def charger_of(name: str) -> str: ...
```

## full_name

[Show source in capture_screenshots.py:78](../../../../../../tools/ipod_classic/capture_screenshots.py#L78)

Return the hierarchical output name of `name`.

The parent chain from main, joined with underscores, for example main_settings_about. The language screen and the overlays are standalone.

#### Signature

```python
def full_name(name: str) -> str: ...
```

## main

[Show source in capture_screenshots.py:342](../../../../../../tools/ipod_classic/capture_screenshots.py#L342)

Run the CLI (see the module docstring); return the exit status, 1 on any failure.

#### Signature

```python
def main() -> int: ...
```

## make_base

[Show source in capture_screenshots.py:173](../../../../../../tools/ipod_classic/capture_screenshots.py#L173)

Cold-boot `image` once to the Language screen and save a VM snapshot of it.

Returns the qcow2 overlay holding the snapshot, backed by the raw `disk` (never written). Each branch then restores a copy instead of cold-booting. One per charger state: the charger is read at boot, so it is part of what the snapshot holds.

#### Signature

```python
def make_base(
    image: str | Path,
    disk: str | Path,
    work: str | Path,
    fast: bool,
    env: dict[str, str],
) -> Path: ...
```

## run_branch

[Show source in capture_screenshots.py:197](../../../../../../tools/ipod_classic/capture_screenshots.py#L197)

Boot once and navigate to each screen of the branch, saving every screen passed.

`index` labels the log lines, `group` is the screen names to visit in order, `stage` the directory the frames land in, `fast` the timing profile and `env` the extra QEMU environment (PMUCHARGER). `base` is a make_base overlay to restore a copy of instead of cold-booting. `keep_awake` first sets Settings > Backlight > Always On (changes the Settings and Backlight screens the survey photographs). Returns the error list (empty on success).

#### Signature

```python
def run_branch(
    index: int,
    group: list[str],
    image: str | Path,
    disk: str | Path,
    stage: str | Path,
    env: dict[str, str],
    fast: bool,
    base: Path | None = None,
    keep_awake: bool = False,
) -> list[str]: ...
```

## select_screens

[Show source in capture_screenshots.py:105](../../../../../../tools/ipod_classic/capture_screenshots.py#L105)

Return (requested, unknown) for a comma-separated screen list.

Empty `spec` selects all non-overlay screens. `without` is a comma-separated list of screens to leave out of the selection (for an image that lacks them); an unknown name in either list is returned in `unknown`.

Library entry point for callers that drive `survey()`.

#### Signature

```python
def select_screens(spec: str, without: str = "") -> tuple[list[str], list[str]]: ...
```

## staging_dir

[Show source in capture_screenshots.py:157](../../../../../../tools/ipod_classic/capture_screenshots.py#L157)

Return a fresh private directory for one survey's VM files, on tmpfs when there is one.

Each SELECT saves a VM snapshot of about 12 MB into the branch's qcow2 overlay: in RAM a save is fast and nothing is written to disk. A unix monitor socket is nested in the directory, so keep the path well under the 100-char sun_path limit. Without /dev/shm the directory is under the scratch directory.

#### Signature

```python
def staging_dir() -> Path: ...
```

## survey

[Show source in capture_screenshots.py:258](../../../../../../tools/ipod_classic/capture_screenshots.py#L258)

Boot `image` across parallel branches and merge the frames into `out`.

`requested` is a list of screen names (see select_screens), `parallel` the most concurrent boots and `fast` the timing profile. Writes <out>/\<full_name(screen)>.png per screen reached. Unless `cold_boot`, the image is cold-booted once per charger state and every branch restores a snapshot of that boot (see make_base); with `cold_boot` each branch cold-boots on its own. Returns the error list.

#### Signature

```python
def survey(
    image: str | Path,
    disk: str | Path,
    out: str | Path,
    requested: list[str],
    parallel: int,
    fast: bool,
    cold_boot: bool = False,
    keep_awake: bool = False,
) -> list[str]: ...
```

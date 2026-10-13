# Menu Navigation

[tools Index](../../../README.md#tools-index) / [Tools](../../index.md#tools) / [Ipod Classic](../index.md#ipod-classic) / [Lib](./index.md#lib) / Menu Navigation

> Auto-generated documentation for [tools.ipod_classic.lib.menu_navigation](../../../../../../../tools/ipod_classic/lib/menu_navigation.py) module.

```text
menu_navigation.py -- navigate_to: move a running emulator to a screen of the screen tree.

    navigate_to(session, tree, target, current_screen)      # returns None; raises NavigationError

Navigation is relative: MENU up to the common ancestor, then per descent step the wheel delta from
the row the guest remembers for that list (`Session.positions`), then SELECT. A list never visited
is at row 0, so nothing is anchored by hand; only a position found wrong is scrolled to the top and
re-derived. Nothing about a screen's pixels is authored: every step is checked by invariants the UI
must satisfy, all in `verify` (its docstring lists them).

Layout: `route` (pure) plans the moves, `Session.send` presses the keys and watches the screen,
`verify` judges what was seen (one flat list of guard clauses), `attempt` sends a move and judges it
(a key that changed nothing is a KeyDroppedError, never resent: the emulated wheel loses no input),
`navigate_to` replans once when a list position turns out wrong.

What is compared is only ever the area a screen `watch`es (screen_tree.json): `outside_watch` turns
it into the `ignore` rectangles of the screenshot.py comparisons. Every failure is a NavigationError
subclass carrying the move and the frames it was judged on; `verify` lists them with their rules.

Constants (module level):
  MIN_LIT            5000 lit pixels: fewer is a blanked display, not a screen
  PAGE_CHANGE_MIN    0.05: share of the compared area a SELECT or MENU must change; a toggle in
                     place changes far less
  LAST_VISIBLE_ROW   8: the last list row that stays on screen; past it a step is only checked
                     for "changed"
  REPLANS            1: a wrong list position is re-derived from the top this many times

Library:
    session = goto_screen.start_session(emulator_handle, tree, outdir)    # on the Language screen
    menu_navigation.navigate_to(session, tree, names["about"], session.here)
```

#### Attributes

- `PAGE_CHANGE_MIN` - Share of the frame that must differ for a page change (SELECT into a screen, MENU out of one); a setting toggled in place changes far less.: 0.05

- `LAST_VISIBLE_ROW` - Last list row that stays fully visible. Settings shows rows 0..8; once a scrolled list reaches it the highlight saturates there. Past it a step is only checked for "changed".: 8

- [Menu Navigation](#menu-navigation)

  - [ArrivalMismatchError](#arrivalmismatcherror)
  - [DidNotSettleError](#didnotsettleerror)
  - [DisplayBlankError](#displayblankerror)
  - [KeyDroppedError](#keydroppederror)
  - [Move](#move)
  - [NavigationError](#navigationerror)
  - [NotAPageError](#notapageerror)
  - [Observation](#observation)
  - [PositionMismatchError](#positionmismatcherror)
  - [RoundTripMismatchError](#roundtripmismatcherror)
  - [Session](#session)
    - [Session().\_keepalive_row](<#session()_keepalive_row>)
    - [Session().\_save_checkpoint](<#session()_save_checkpoint>)
    - [Session().\_send_restore](<#session()_send_restore>)
    - [Session().\_send_top](<#session()_send_top>)
    - [Session().arrival_key](<#session()arrival_key>)
    - [Session().record](<#session()record>)
    - [Session().send](<#session()send>)
  - [TopNotReachedError](#topnotreachederror)
  - [\_anything](#_anything)
  - [\_discard](#_discard)
  - [\_reset](#_reset)
  - [\_tag](#_tag)
  - [\_wheel_moves](#_wheel_moves)
  - [attempt](#attempt)
  - [await_overlay](#await_overlay)
  - [move_to_row](#move_to_row)
  - [navigate_to](#navigate_to)
  - [outside_watch](#outside_watch)
  - [route](#route)
  - [verify](#verify)

## ArrivalMismatchError

[Show source in menu_navigation.py:113](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L113)

The screen differs from the frame the first arrival at it showed in this run.

#### Signature

```python
class ArrivalMismatchError(NavigationError): ...
```

#### See also

- [NavigationError](#navigationerror)

## DidNotSettleError

[Show source in menu_navigation.py:93](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L93)

The screen kept changing until the move's `arrive` seconds ran out.

#### Signature

```python
class DidNotSettleError(NavigationError): ...
```

#### See also

- [NavigationError](#navigationerror)

## DisplayBlankError

[Show source in menu_navigation.py:81](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L81)

The display is dark; under fast timing no key wakes it again.

#### Signature

```python
class DisplayBlankError(NavigationError): ...
```

#### See also

- [NavigationError](#navigationerror)

## KeyDroppedError

[Show source in menu_navigation.py:85](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L85)

The key changed nothing on the screen.

The emulated wheel does not lose input (qemu-ipod's ordered queue), so this is a failure to investigate, not a reason to press again.

#### Signature

```python
class KeyDroppedError(NavigationError): ...
```

#### See also

- [NavigationError](#navigationerror)

## Move

[Show source in menu_navigation.py:118](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L118)

One key action.

`kind` is "top" (scroll `source` to the top of its list), "wheel" (`steps` list rows, positive = down), "select" (open the child `destination`), "menu" (back out to the parent `destination`), "hold" (a long MENU hold on `source` that opens `destination`, a screen with `via` "menu_hold") or "restore" (load the snapshot taken just before `destination` last opened a child: the way up when snapshots are available, in place of any number of "menu" moves). `visible` says the highlight stays on a visible row for the whole wheel move, so the rows moved can be counted.

#### Signature

```python
class Move: ...
```

## NavigationError

[Show source in menu_navigation.py:69](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L69)

A check of [verify](#verify) failed.

`move` is the Move that was judged and `seen` the Observation (its frames), for diagnosis.

#### Signature

```python
class NavigationError(Exception):
    def __init__(self, move: "Move", detail: str, seen: "Observation") -> None: ...
```

## NotAPageError

[Show source in menu_navigation.py:105](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L105)

SELECT or MENU changed too little of the screen to be a page change (toggle in place?).

#### Signature

```python
class NotAPageError(NavigationError): ...
```

#### See also

- [NavigationError](#navigationerror)

## Observation

[Show source in menu_navigation.py:137](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L137)

What sending a Move showed.

The settled frame `before`, the frame `after`, whether the screen came to rest within the move's time limit (`settled`) and, for "top" only, the `probe` frame after one more wheel-up.

#### Signature

```python
class Observation: ...
```

## PositionMismatchError

[Show source in menu_navigation.py:97](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L97)

The highlight moved other than the asked number of rows. Recoverable by replanning.

#### Signature

```python
class PositionMismatchError(NavigationError): ...
```

#### See also

- [NavigationError](#navigationerror)

## RoundTripMismatchError

[Show source in menu_navigation.py:109](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L109)

MENU returned to a frame that differs from the one shown before entering the child.

#### Signature

```python
class RoundTripMismatchError(NavigationError): ...
```

#### See also

- [NavigationError](#navigationerror)

## Session

[Show source in menu_navigation.py:294](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L294)

A booted emulator being navigated: the key pacer, the timing profile and what has been seen.

Constructed with the monitor `mon`, the `timing` profile (qemu_session.FAST or REFERENCE), the `root` screen and its `first_frame`; `snap(name, frame)` is called with each screen reached (None = no saving) and `log(message)` receives the pacer's messages. With `checkpoints` the monitor's VM has a snapshottable disk (a qcow2 overlay): every SELECT first saves a snapshot of the screen it leaves, and the way up is loading it (see "restore" in Move) instead of pressing MENU.

positions list screen name -> the row the guest highlights in it (absent = row 0, never visited; None = found wrong, rescrolled from the top before the next move) entered screen name -> its settled frame just before a child was selected (round trip) learned (screen name, list row) -> the frame of its first arrival this run events counts of recoveries and failures by kind, for the stress test here the screen the emulator is on, as of the last verified move last the settled frame the next move is compared against checkpoint_positions screen name -> a copy of `positions` when its snapshot was taken (with checkpoints) pacer the qemu_session.Pacer that sends keys and polls screenshots

#### Signature

```python
class Session:
    def __init__(
        self,
        mon: qemu_session.Monitor,
        timing: qemu_session.Timing,
        root: screen_tree.ScreenDefinition,
        first_frame: screenshot.Frame,
        snap: Callable[[str, screenshot.Frame], object] | None = None,
        log: Callable[[str], None] = _discard,
        checkpoints: bool = False,
    ) -> None: ...
```

#### See also

- [\_discard](#_discard)

### Session().\_keepalive_row

[Show source in menu_navigation.py:339](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L339)

0 when a wheel-up is a no-op here (the highlight is at the top of a list), else -1.

#### Signature

```python
def _keepalive_row(self, _seen: screenshot.Frame) -> int: ...
```

### Session().\_save_checkpoint

[Show source in menu_navigation.py:381](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L381)

Snapshot the VM as it is on `screen`; recorded once the SELECT out of it is verified.

#### Signature

```python
def _save_checkpoint(self, screen: screen_tree.ScreenDefinition) -> None: ...
```

### Session().\_send_restore

[Show source in menu_navigation.py:389](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L389)

Load the snapshot of `move.destination` and wait for the screen to rest.

#### Signature

```python
def _send_restore(self, move: Move, before: screenshot.Frame) -> Observation: ...
```

#### See also

- [Move](#move)
- [Observation](#observation)

### Session().\_send_top

[Show source in menu_navigation.py:411](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L411)

Wheel up the longest list's length, rest, then one more wheel-up as the probe.

#### Signature

```python
def _send_top(self, move: Move, before: screenshot.Frame) -> Observation: ...
```

#### See also

- [Move](#move)
- [Observation](#observation)

### Session().arrival_key

[Show source in menu_navigation.py:347](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L347)

Key of `learned`: a list shows its remembered row, so the same screen differs per row.

#### Signature

```python
def arrival_key(self, screen: screen_tree.ScreenDefinition) -> tuple: ...
```

### Session().record

[Show source in menu_navigation.py:423](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L423)

Remember what a verified move established.

#### Signature

```python
def record(self, move: Move, seen: Observation) -> None: ...
```

#### See also

- [Move](#move)
- [Observation](#observation)

### Session().send

[Show source in menu_navigation.py:351](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L351)

Press the keys of `move` and watch the screen: wait for it to change, then to rest.

#### Signature

```python
def send(self, move: Move) -> Observation: ...
```

#### See also

- [Move](#move)
- [Observation](#observation)

## TopNotReachedError

[Show source in menu_navigation.py:101](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L101)

A further wheel-up still changed the list after the scroll to the top.

#### Signature

```python
class TopNotReachedError(NavigationError): ...
```

#### See also

- [NavigationError](#navigationerror)

## \_anything

[Show source in menu_navigation.py:64](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L64)

Settle predicate for a screen with nothing to recognise: any frame will do.

#### Signature

```python
def _anything(_seen: screenshot.Frame) -> bool: ...
```

## \_discard

[Show source in menu_navigation.py:60](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L60)

Default log: say nothing.

#### Signature

```python
def _discard(_message: str) -> None: ...
```

## \_reset

[Show source in menu_navigation.py:467](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L467)

Start the branch again after a wrong position on the list `screen`; return where we are.

With snapshots that is the parent of `screen`, restored as it was before `screen` was selected (the plan then selects it again, a list opening at row 0). Without, the position is marked wrong and `screen` is rescrolled from the top.

#### Signature

```python
def _reset(
    session: Session, tree: screen_tree.ScreenTree, screen: screen_tree.ScreenDefinition
) -> screen_tree.ScreenDefinition: ...
```

#### See also

- [Session](#session)

## \_tag

[Show source in menu_navigation.py:55](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L55)

Return the snapshot name of `screen`'s checkpoint.

#### Signature

```python
def _tag(screen: screen_tree.ScreenDefinition) -> str: ...
```

## \_wheel_moves

[Show source in menu_navigation.py:205](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L205)

Wheel moves that take the highlight of `parent` from `row` (None = unknown) to `want`.

#### Signature

```python
def _wheel_moves(
    parent: screen_tree.ScreenDefinition, row: int | None, want: int
) -> list[Move]: ...
```

#### See also

- [Move](#move)

## attempt

[Show source in menu_navigation.py:449](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L449)

Send `move` once and verify it; every failure propagates with the frames it was judged on.

#### Signature

```python
def attempt(session: Session, move: Move) -> Observation: ...
```

#### See also

- [Move](#move)
- [Observation](#observation)
- [Session](#session)

## await_overlay

[Show source in menu_navigation.py:527](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L527)

Wait for a screen that appears by itself (the Charging screen), return its settled frame.

Judged only by how much of the screen changed (PAGE_CHANGE_MIN); there is nothing to press. Raises TimeoutError when it does not appear within `overlay.arrive` seconds.

#### Signature

```python
def await_overlay(
    session: Session, overlay: screen_tree.ScreenDefinition
) -> screenshot.Frame: ...
```

#### See also

- [Session](#session)

## move_to_row

[Show source in menu_navigation.py:457](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L457)

Wheel the highlight of the list `screen` (the one the emulator is on) to row `want`.

Uses the verified moves navigate_to uses for the same job (scroll to the top first when the row is unknown).

#### Signature

```python
def move_to_row(
    session: Session, screen: screen_tree.ScreenDefinition, want: int
) -> None: ...
```

#### See also

- [Session](#session)

## navigate_to

[Show source in menu_navigation.py:486](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L486)

Move the emulator from `current_screen` (where it is now; `session.here`) to `target`.

Raises NavigationError on failure. A wrong list position restarts the branch at the parent (from the parent's snapshot, or a scroll to the top without snapshots) and replans, REPLANS times. Raises ValueError, before any key is sent, for a target that is the boot screen (MENU does not lead back to it) and, without snapshots (`session.checkpoints`), for leaving a screen with `cannot_leave_in_fast_timing` under fast timing.

#### Signature

```python
def navigate_to(
    session: Session,
    tree: screen_tree.ScreenTree,
    target: screen_tree.ScreenDefinition,
    current_screen: screen_tree.ScreenDefinition,
) -> None: ...
```

#### See also

- [Session](#session)

## outside_watch

[Show source in menu_navigation.py:150](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L150)

Return the rectangles outside the area `screen` watches.

That is the `ignore` argument of the screenshot.py comparisons, so passing it compares only what the screen asked to be judged by. Cached: screens are immutable.

#### Signature

```python
@cache
def outside_watch(screen: screen_tree.ScreenDefinition) -> tuple: ...
```

## route

[Show source in menu_navigation.py:163](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L163)

Plan the moves from `current` to `target`, touching no emulator.

MENU up to the common ancestor, then for each step down the wheel moves from the remembered row of that list and a SELECT. `positions` maps a list's screen name to the row the guest highlights in it, as `Session.positions` does: a list never visited is at row 0 (every list opens there), and None marks a position found wrong, which is re-derived with a scroll to the top; Wheel moves are single steps, each checked. With `checkpoints` (screen name -> the positions when its snapshot was taken, as `Session.checkpoint_positions`) the way up is one "restore" of the common ancestor's snapshot instead of "menu" moves, and the way down starts from the positions that snapshot held. Rows past the visible window are planned one step at a time as wheel moves that cannot be counted (`visible` false), and a long way up is planned as a scroll to the top followed by the way down.

#### Signature

```python
def route(
    tree: screen_tree.ScreenTree,
    positions: dict,
    current: screen_tree.ScreenDefinition,
    target: screen_tree.ScreenDefinition,
    checkpoints: dict | None = None,
) -> list[Move]: ...
```

#### See also

- [Move](#move)

## verify

[Show source in menu_navigation.py:230](../../../../../../../tools/ipod_classic/lib/menu_navigation.py#L230)

Raise the NavigationError of the first rule `seen` breaks; return None when all hold.

The rules, in order (every comparison covers only the watched area of the screen involved):

- fewer than MIN_LIT lit pixels after: DisplayBlankError
- after equals before (not for "top"): KeyDroppedError
- the screen did not come to rest in time: DidNotSettleError
- "top": one more wheel-up still changed the list: TopNotReachedError
- a visible "wheel" move whose highlight moved another number of rows: PositionMismatchError
- a "select", "hold", "menu" or "restore" that changed less than PAGE_CHANGE_MIN of the frame: NotAPageError
- "menu" or "restore" to a frame unlike the one before the child was entered: RoundTripMismatchError
- "select" into a frame unlike the first arrival at that screen and row: ArrivalMismatchError

No loops, no nesting, no side effects: a new rule is a new `if`.

#### Signature

```python
def verify(move: Move, seen: Observation, session: "Session") -> None: ...
```

#### See also

- [Move](#move)
- [Observation](#observation)

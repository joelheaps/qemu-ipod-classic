"""menu_navigation.py -- navigate_to: move a running emulator to a screen of the screen tree.

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
"""

import time
from collections import Counter
from collections.abc import Callable
from dataclasses import dataclass
from functools import cache

import qemu_session
import screen_tree
import screenshot

MIN_LIT = 5000  # a frame with fewer lit pixels is a blanked display, not a screen
# Share of the frame that must differ for a page change (SELECT into a screen, MENU out of one); a
# setting toggled in place changes far less.
PAGE_CHANGE_MIN = 0.05
# Last list row that stays fully visible. Settings shows rows 0..8; once a scrolled list reaches it
# the highlight saturates there. Past it a step is only checked for "changed".
LAST_VISIBLE_ROW = 8
REPLANS = 1  # a wrong list position is re-derived from the top this many times
SNAPSHOT_TIMEOUT = 120  # seconds for the monitor to save or load a VM snapshot
ROW_KEYS = {1: "e", -1: "q"}  # wheel direction -> key (e: down, q: up)


def _tag(screen: screen_tree.ScreenDefinition) -> str:
    """Return the snapshot name of `screen`'s checkpoint."""
    return f"cp_{screen.name}"


def _discard(_message: str) -> None:
    """Default log: say nothing."""


def _anything(_seen: screenshot.Frame) -> bool:
    """Settle predicate for a screen with nothing to recognise: any frame will do."""
    return True


class NavigationError(Exception):
    """A check of `verify` failed.

    `move` is the Move that was judged and `seen` the Observation (its frames), for diagnosis.
    """

    def __init__(self, move: "Move", detail: str, seen: "Observation") -> None:
        """Name the move and the failed rule; keep `seen` (the frames it was judged on)."""
        super().__init__(f"{move.kind} {move.source.name} -> {move.destination.name}: {detail}")
        self.move, self.seen = move, seen


class DisplayBlankError(NavigationError):
    """The display is dark; under fast timing no key wakes it again."""


class KeyDroppedError(NavigationError):
    """The key changed nothing on the screen.

    The emulated wheel does not lose input (qemu-ipod's ordered queue), so this is a failure to
    investigate, not a reason to press again.
    """


class DidNotSettleError(NavigationError):
    """The screen kept changing until the move's `arrive` seconds ran out."""


class PositionMismatchError(NavigationError):
    """The highlight moved other than the asked number of rows. Recoverable by replanning."""


class TopNotReachedError(NavigationError):
    """A further wheel-up still changed the list after the scroll to the top."""


class NotAPageError(NavigationError):
    """SELECT or MENU changed too little of the screen to be a page change (toggle in place?)."""


class RoundTripMismatchError(NavigationError):
    """MENU returned to a frame that differs from the one shown before entering the child."""


class ArrivalMismatchError(NavigationError):
    """The screen differs from the frame the first arrival at it showed in this run."""


@dataclass(frozen=True)
class Move:
    """One key action.

    `kind` is "top" (scroll `source` to the top of its list), "wheel" (`steps` list rows, positive =
    down), "select" (open the child `destination`), "menu" (back out to the parent `destination`),
    "hold" (a long MENU hold on `source` that opens `destination`, a screen with `via` "menu_hold")
    or "restore" (load the snapshot taken just before `destination` last opened a child: the way
    up when snapshots are available, in place of any number of "menu" moves). `visible` says the
    highlight stays on a visible row for the whole wheel move, so the rows moved can be counted.
    """

    kind: str  # 'top' | 'wheel' | 'select' | 'menu'
    source: screen_tree.ScreenDefinition
    destination: screen_tree.ScreenDefinition
    steps: int = 0
    visible: bool = False


@dataclass
class Observation:
    """What sending a Move showed.

    The settled frame `before`, the frame `after`, whether the screen came to rest within the
    move's time limit (`settled`) and, for "top" only, the `probe` frame after one more wheel-up.
    """

    before: screenshot.Frame
    after: screenshot.Frame
    settled: bool
    probe: screenshot.Frame | None = None


@cache
def outside_watch(screen: screen_tree.ScreenDefinition) -> tuple:
    """Return the rectangles outside the area `screen` watches.

    That is the `ignore` argument of the screenshot.py comparisons, so passing it compares only
    what the screen asked to be judged by. Cached: screens are immutable.
    """
    return screenshot.unwatched(screen.watch or (), *screen_tree.SCREEN_SIZE)


# Planning (pure):


def route(
    tree: screen_tree.ScreenTree,
    positions: dict,
    current: screen_tree.ScreenDefinition,
    target: screen_tree.ScreenDefinition,
    *,
    checkpoints: dict | None = None,
) -> list[Move]:
    """Plan the moves from `current` to `target`, touching no emulator.

    MENU up to the common ancestor, then for each step down the wheel moves from the remembered row
    of that list and a SELECT. `positions` maps a list's screen name to the row the guest highlights
    in it, as `Session.positions` does: a list never visited is at row 0 (every list opens there),
    and None marks a position found wrong, which is re-derived with a scroll to the top;
    Wheel moves are single steps, each checked. With `checkpoints` (screen name -> the
    positions when its snapshot was taken, as `Session.checkpoint_positions`) the way up is one
    "restore" of the common ancestor's snapshot instead of "menu" moves, and the way down starts
    from the positions that snapshot held. Rows past the visible window are
    planned one step at a time as wheel moves that cannot be counted (`visible` false), and a long
    way up is planned as a scroll to the top followed by the way down.
    """
    here, there = screen_tree.chain(tree, current.name), screen_tree.chain(tree, target.name)
    shared = 0
    while shared < min(len(here), len(there)) and here[shared].name == there[shared].name:
        shared += 1
    ancestor = here[shared - 1]
    if len(here) > shared and checkpoints is not None and ancestor.name in checkpoints:
        moves = [Move("restore", here[-1], ancestor)]
        rows = dict(checkpoints[ancestor.name])
    else:
        moves = [Move("menu", here[i], here[i - 1]) for i in range(len(here) - 1, shared - 1, -1)]
        rows = dict(positions)
    for parent, child in zip(there[shared - 1 :], there[shared:], strict=False):
        if child.via == "menu_hold":
            moves.append(Move("hold", parent, child))
            continue
        moves += _wheel_moves(parent, rows.get(parent.name, 0), child.row or 0)
        rows[parent.name] = child.row or 0
        moves.append(Move("select", parent, child))
    return moves


def _wheel_moves(parent: screen_tree.ScreenDefinition, row: int | None, want: int) -> list[Move]:
    """Wheel moves that take the highlight of `parent` from `row` (None = unknown) to `want`."""
    moves = []
    if row is None or (want < row and row > LAST_VISIBLE_ROW):
        moves.append(Move("top", parent, parent))
        row = 0
    direction = 1 if want > row else -1
    while row != want:
        end = row + direction
        moves.append(
            Move(
                "wheel",
                parent,
                parent,
                direction,
                visible=max(row, end) <= LAST_VISIBLE_ROW,
            )
        )
        row = end
    return moves


# Judging (one flat list):


def verify(move: Move, seen: Observation, session: "Session") -> None:
    """Raise the NavigationError of the first rule `seen` breaks; return None when all hold.

    The rules, in order (every comparison covers only the watched area of the screen involved):

    - fewer than MIN_LIT lit pixels after: DisplayBlankError
    - after equals before (not for "top"): KeyDroppedError
    - the screen did not come to rest in time: DidNotSettleError
    - "top": one more wheel-up still changed the list: TopNotReachedError
    - a visible "wheel" move whose highlight moved another number of rows: PositionMismatchError
    - a "select", "hold", "menu" or "restore" that changed less than PAGE_CHANGE_MIN of the frame:
      NotAPageError
    - "menu" or "restore" to a frame unlike the one before the child was entered:
      RoundTripMismatchError
    - "select" into a frame unlike the first arrival at that screen and row: ArrivalMismatchError

    No loops, no nesting, no side effects: a new rule is a new `if`.
    """
    own = outside_watch(move.source)
    there = outside_watch(move.destination)
    wheel_visible = move.kind == "wheel" and move.visible
    expected = session.entered.get(move.destination.name)
    learned = session.learned.get(session.arrival_key(move.destination))
    if screenshot.lit(seen.after) < MIN_LIT:
        raise DisplayBlankError(move, f"{screenshot.lit(seen.after)} lit pixels", seen)
    if move.kind != "top" and screenshot.same(seen.after, seen.before, own):
        raise KeyDroppedError(move, "screen unchanged", seen)
    if not seen.settled:
        raise DidNotSettleError(move, f"still changing after {move.destination.arrive}s", seen)
    if (
        move.kind == "top"
        and seen.probe is not None
        and not screenshot.same(seen.probe, seen.after, own)
    ):
        raise TopNotReachedError(move, "a further wheel-up still changed the list", seen)
    if wheel_visible and screenshot.moved_rows(seen.before, seen.after, own) != abs(move.steps):
        moved = screenshot.moved_rows(seen.before, seen.after, own)
        raise PositionMismatchError(move, f"moved {moved} rows, wanted {abs(move.steps)}", seen)
    if (
        move.kind in ("select", "hold", "menu", "restore")
        and screenshot.change_fraction(seen.before, seen.after, own) < PAGE_CHANGE_MIN
    ):
        raise NotAPageError(
            move,
            f"only {screenshot.change_fraction(seen.before, seen.after, own):.0%} changed",
            seen,
        )
    if (
        move.kind in ("menu", "restore")
        and expected is not None
        and not screenshot.same(seen.after, expected, there)
    ):
        raise RoundTripMismatchError(move, "differs from the frame before entering the child", seen)
    if (
        move.kind in ("select", "hold")
        and learned is not None
        and not screenshot.same(seen.after, learned, there)
    ):
        raise ArrivalMismatchError(move, "differs from the first arrival this run", seen)


# The emulator side:


class Session:
    """A booted emulator being navigated: the key pacer, the timing profile and what has been seen.

    Constructed with the monitor `mon`, the `timing` profile (qemu_session.FAST or REFERENCE), the
    `root` screen and its `first_frame`; `snap(name, frame)` is called with each screen reached
    (None = no saving) and `log(message)` receives the pacer's messages. With `checkpoints` the
    monitor's VM has a snapshottable disk (a qcow2 overlay): every SELECT first saves a snapshot of
    the screen it leaves, and the way up is loading it (see "restore" in Move) instead of pressing
    MENU.

    positions   list screen name -> the row the guest highlights in it (absent = row 0, never
                visited; None = found wrong, rescrolled from the top before the next move)
    entered     screen name -> its settled frame just before a child was selected (round trip)
    learned     (screen name, list row) -> the frame of its first arrival this run
    events      counts of recoveries and failures by kind, for the stress test
    here        the screen the emulator is on, as of the last verified move
    last        the settled frame the next move is compared against
    checkpoint_positions
                screen name -> a copy of `positions` when its snapshot was taken (with checkpoints)
    pacer       the qemu_session.Pacer that sends keys and polls screenshots
    """

    def __init__(  # noqa: PLR0913, PLR0917 - one session object carries the whole run setup
        self,
        mon: qemu_session.Monitor,
        timing: qemu_session.Timing,
        root: screen_tree.ScreenDefinition,
        first_frame: screenshot.Frame,
        snap: Callable[[str, screenshot.Frame], object] | None = None,
        log: Callable[[str], None] = _discard,
        *,
        checkpoints: bool = False,
    ) -> None:
        """Start on `root` showing `first_frame`; `snap(name, frame)` saves each screen reached."""
        self.timing, self.snap, self.log = timing, snap, log
        self.checkpoints = checkpoints
        self.checkpoint_positions: dict[str, dict] = {}
        self._pending: tuple[str, dict] | None = None
        self.here, self.last = root, first_frame
        self.positions: dict[str, int | None] = {}
        self.entered: dict[str, screenshot.Frame] = {}
        self.learned: dict[tuple, screenshot.Frame] = {}
        self.events: Counter = Counter()
        self.pacer = qemu_session.Pacer(mon, timing, row=self._keepalive_row, log=log)

    def _keepalive_row(self, _seen: screenshot.Frame) -> int:
        """0 when a wheel-up is a no-op here (the highlight is at the top of a list), else -1."""
        return (
            0
            if screen_tree.is_list(self.here) and self.positions.get(self.here.name, 0) == 0
            else -1
        )

    def arrival_key(self, screen: screen_tree.ScreenDefinition) -> tuple:
        """Key of `learned`: a list shows its remembered row, so the same screen differs per row."""
        return (screen.name, self.positions.get(screen.name, 0))

    def send(self, move: Move) -> Observation:
        """Press the keys of `move` and watch the screen: wait for it to change, then to rest."""
        before = self.last
        rest_ignore = outside_watch(move.destination)
        if move.kind == "top":
            return self._send_top(move, before)
        if move.kind == "restore":
            return self._send_restore(move, before)
        if move.kind in ("select", "hold") and self.checkpoints:
            self._save_checkpoint(move.source)
        keys = {"select": "ret", "menu": "w", "hold": "w"}.get(move.kind) or ROW_KEYS[
            1 if move.steps > 0 else -1
        ]
        hold = self.timing.long_menu_hold if move.kind == "hold" else None
        for _ in range(1 if move.kind in ("select", "hold", "menu") else abs(move.steps)):
            self.pacer.key(keys, hold)
        patience = self.timing.key_timeout if move.kind == "wheel" else self.timing.select_timeout
        changed = self.pacer.wait_until(
            lambda f: not screenshot.same(f, before, outside_watch(move.source)),
            patience,
            force=move.kind != "wheel",
        )
        if changed is None:
            self.last = self.pacer.shot()
            return Observation(before, self.last, settled=True)
        limit = move.destination.arrive if move.kind != "wheel" else self.timing.key_settle
        rested = self.pacer.settle(_anything, rest_ignore, limit)
        self.last = rested if rested is not None else self.pacer.shot()
        return Observation(before, self.last, settled=rested is not None)

    def _save_checkpoint(self, screen: screen_tree.ScreenDefinition) -> None:
        """Snapshot the VM as it is on `screen`; recorded once the SELECT out of it is verified."""
        out = self.pacer.mon.cmd(f"savevm {_tag(screen)}", timeout=SNAPSHOT_TIMEOUT)
        if out.strip():
            msg = f"savevm {_tag(screen)}: {out.strip()}"
            raise RuntimeError(msg)
        self._pending = (screen.name, dict(self.positions))

    def _send_restore(self, move: Move, before: screenshot.Frame) -> Observation:
        """Load the snapshot of `move.destination` and wait for the screen to rest."""
        out = self.pacer.mon.cmd(f"loadvm {_tag(move.destination)}", timeout=SNAPSHOT_TIMEOUT)
        if out.strip():
            msg = f"loadvm {_tag(move.destination)}: {out.strip()}"
            raise RuntimeError(msg)
        self.pacer.last_key = time.time()
        # The display can still show the child for a moment after the load: wait for it to change.
        changed = self.pacer.wait_until(
            lambda f: not screenshot.same(f, before, outside_watch(move.source)),
            self.timing.select_timeout,
            force=True,
        )
        if changed is None:
            self.last = self.pacer.shot()
            return Observation(before, self.last, settled=True)
        rested = self.pacer.settle(
            _anything, outside_watch(move.destination), move.destination.arrive
        )
        self.last = rested if rested is not None else self.pacer.shot()
        return Observation(before, self.last, settled=rested is not None)

    def _send_top(self, move: Move, before: screenshot.Frame) -> Observation:
        """Wheel up the longest list's length, rest, then one more wheel-up as the probe."""
        for _ in range(screen_tree.SCROLL_TO_TOP_PRESSES):
            self.pacer.key("q")
        limit = self.timing.key_settle
        rested = self.pacer.settle(_anything, outside_watch(move.source), limit)
        after = rested if rested is not None else self.pacer.shot()
        self.pacer.key("q")
        probe = self.pacer.settle(_anything, outside_watch(move.source), limit) or self.pacer.shot()
        self.last = probe
        return Observation(before, after, settled=rested is not None, probe=probe)

    def record(self, move: Move, seen: Observation) -> None:
        """Remember what a verified move established."""
        self.here = move.destination
        if move.kind == "top":
            self.positions[move.source.name] = 0
        if move.kind == "wheel":
            self.positions[move.source.name] = (
                self.positions.get(move.source.name) or 0
            ) + move.steps
        if move.kind == "restore":
            self.positions = dict(self.checkpoint_positions[move.destination.name])
        if move.kind in ("select", "hold"):
            if self._pending is not None:
                self.checkpoint_positions[self._pending[0]] = self._pending[1]
                self._pending = None
            self.entered[move.source.name] = seen.before
            if screen_tree.is_list(move.destination):
                self.positions.setdefault(move.destination.name, 0)
            self.learned.setdefault(self.arrival_key(move.destination), seen.after)
            if self.snap is not None:
                self.snap(move.destination.name, seen.after)


# Recovery and entry point:


def attempt(session: Session, move: Move) -> Observation:
    """Send `move` once and verify it; every failure propagates with the frames it was judged on."""
    seen = session.send(move)
    verify(move, seen, session)
    session.record(move, seen)
    return seen


def move_to_row(session: Session, screen: screen_tree.ScreenDefinition, want: int) -> None:
    """Wheel the highlight of the list `screen` (the one the emulator is on) to row `want`.

    Uses the verified moves navigate_to uses for the same job (scroll to the top first when the row
    is unknown).
    """
    for move in _wheel_moves(screen, session.positions.get(screen.name), want):
        attempt(session, move)


def _reset(
    session: Session, tree: screen_tree.ScreenTree, screen: screen_tree.ScreenDefinition
) -> screen_tree.ScreenDefinition:
    """Start the branch again after a wrong position on the list `screen`; return where we are.

    With snapshots that is the parent of `screen`, restored as it was before `screen` was selected
    (the plan then selects it again, a list opening at row 0). Without, the position is marked
    wrong and `screen` is rescrolled from the top.
    """
    trail = screen_tree.chain(tree, screen.name)
    parent = trail[-2] if len(trail) > 1 else None
    if session.checkpoints and parent is not None and parent.name in session.checkpoint_positions:
        attempt(session, Move("restore", screen, parent))
        return parent
    session.positions[screen.name] = None
    session.here = screen
    return screen


def navigate_to(
    session: Session,
    tree: screen_tree.ScreenTree,
    target: screen_tree.ScreenDefinition,
    current_screen: screen_tree.ScreenDefinition,
) -> None:
    """Move the emulator from `current_screen` (where it is now; `session.here`) to `target`.

    Raises NavigationError on failure. A wrong list position restarts the branch at the parent (from
    the parent's snapshot, or a scroll to the top without snapshots) and replans, REPLANS times.
    Raises ValueError, before any key is sent, for a target that is the boot screen (MENU does not
    lead back to it) and, without snapshots (`session.checkpoints`), for leaving a screen with
    `cannot_leave_in_fast_timing` under fast timing.
    """
    if target.name == tree.root.name and current_screen.name != target.name:
        msg = f"{target.name} is the boot screen: MENU does not lead back to it"
        raise ValueError(msg)
    if (
        current_screen.cannot_leave_in_fast_timing
        and not session.checkpoints
        and session.timing is qemu_session.FAST
        and target.name != current_screen.name
    ):
        msg = f"{current_screen.name} cannot be left under fast timing (MENU is ignored there)"
        raise ValueError(msg)
    here = current_screen
    for replan in range(REPLANS + 1):
        try:
            checkpoints = session.checkpoint_positions if session.checkpoints else None
            for move in route(tree, session.positions, here, target, checkpoints=checkpoints):
                attempt(session, move)
                here = move.destination
        except (PositionMismatchError, TopNotReachedError) as error:
            session.events[type(error).__name__] += 1
            if replan == REPLANS:
                raise
            here = _reset(session, tree, error.move.source)
        else:
            return


def await_overlay(session: Session, overlay: screen_tree.ScreenDefinition) -> screenshot.Frame:
    """Wait for a screen that appears by itself (the Charging screen), return its settled frame.

    Judged only by how much of the screen changed (PAGE_CHANGE_MIN); there is nothing to press.
    Raises TimeoutError when it does not appear within `overlay.arrive` seconds.
    """
    before = session.last
    ignore = outside_watch(session.here)
    deadline = time.time() + overlay.arrive
    # No keepalive here: a key would reset the guest's idle timer, and the idle timer is what raises
    # the Charging screen. (With the wall adapter the LCD does not blank under fast timing.)
    arrived = None
    while arrived is None and time.time() < deadline:
        seen = session.pacer.shot()
        if screenshot.change_fraction(before, seen, ignore) >= PAGE_CHANGE_MIN:
            arrived = seen
        else:
            time.sleep(session.timing.poll)
    if arrived is None:
        msg = f"{overlay.name} did not appear within {overlay.arrive}s"
        raise TimeoutError(msg)
    rested = session.pacer.settle(_anything, outside_watch(overlay), overlay.arrive)
    session.last = rested if rested is not None else arrived
    return session.last

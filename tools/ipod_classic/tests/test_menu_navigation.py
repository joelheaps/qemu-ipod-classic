# /// script
# requires-python = ">=3.12"
# dependencies = ["pytest", "msgspec"]
# ///
"""Tests for the screen-tree loader, `route` and every guard clause of `verify`.

Synthetic frames, no emulator. Run with pytest, or directly: plain asserts.
"""

import sys
import tempfile
from collections.abc import Callable
from pathlib import Path
from types import SimpleNamespace

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "lib"))
import menu_navigation  # noqa: E402
import screen_tree  # noqa: E402
import screenshot  # noqa: E402

TREE = screen_tree.load()
NAMES = screen_tree.by_name(TREE)
SCREEN_COUNT = 25
WIDTH, HEIGHT = 320, 240
GREY, WHITE, BLACK = (60, 60, 60), (230, 230, 230), (0, 0, 0)
Colour = tuple[int, int, int]


def raises(error: type[Exception], call: Callable, *args: object, **kwargs: object) -> Exception:
    """Return the `error` that `call` raises; fail when it raises nothing."""
    try:
        call(*args, **kwargs)
    except error as caught:
        return caught
    msg = f"{call!r} did not raise {error.__name__}"
    raise AssertionError(msg)


# ---------------------------------------------------------------- loader


def load_text(text: str) -> screen_tree.ScreenTree:
    """Load a screen_tree.json given as text."""
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as handle:
        handle.write(text)
        path = Path(handle.name)
    try:
        return screen_tree.load(path)
    finally:
        path.unlink()


HOLD_TREE = load_text(
    '{"watch": [[0, 0, 320, 240]],'
    ' "root": {"name": "main", "children": [{"name": "panel", "via": "menu_hold"}]}}'
)
HOLD_NAMES = screen_tree.by_name(HOLD_TREE)


def test_shipped_tree_loads() -> None:
    """screen_tree.json loads, nests and carries the charger requirement."""
    assert len(NAMES) == SCREEN_COUNT
    path = [s.name for s in screen_tree.chain(TREE, "about")]
    assert path == ["language", "main", "settings", "about"]
    assert NAMES["charging"].requires_charger_state == "wall"


def tree_text(root: str) -> str:
    """Return a screen_tree.json with the default watch area and the given `root` object."""
    return f'{{"watch": [[0, 0, 320, 240]], "root": {root}}}'


def test_loader_rejects_bad_files() -> None:
    """Duplicate names, a missing row, unsorted rows, a typo and a bad enum value all raise."""
    assert load_text(tree_text('{"name": "r", "children": [{"name": "a", "row": 0}]}')).root
    child = '{"name": "a", "row": 0}'
    for root in (
        f'{{"name": "r", "children": [{child}, {{"name": "a", "row": 1}}]}}',
        '{"name": "r", "children": [{"name": "a"}]}',
        '{"name": "r", "children": [{"name": "a", "row": 2}, {"name": "b", "row": 1}]}',
        '{"name": "r", "children": [{"name": "a", "row": 0, "arive": 3}]}',
        '{"name": "r", "requires_charger_state": "solar"}',
    ):
        raises(ValueError, load_text, tree_text(root))
    raises(ValueError, load_text, '{"root": {"name": "r"}}')  # the default watch is required
    held = '{"name": "o", "via": "menu_hold"}'
    loaded = load_text(tree_text(f'{{"name": "r", "children": [{held}]}}'))
    assert loaded.root.children[0].via == "menu_hold"
    raises(
        ValueError,
        load_text,
        tree_text('{"name": "r", "children": [{"name": "o", "via": "menu_hold", "row": 1}]}'),
    )


def test_watch_defaults_and_overrides() -> None:
    """Every screen carries a watch area: the tree default unless the screen names its own."""
    assert NAMES["about"].watch == tuple(map(tuple, TREE.watch))
    assert NAMES["main"].watch != NAMES["about"].watch
    assert all(s.watch for s in NAMES.values())


def test_unwatched_is_the_complement() -> None:
    """The rectangles outside the watched area cover exactly the pixels not watched."""
    watch = ((0, 0, 100, 10), (0, 10, 50, 20))
    outside = screenshot.unwatched(watch, 100, 20)
    assert outside == ((50, 10, 100, 20),)
    assert screenshot.unwatched(((0, 0, 100, 20),), 100, 20) == ()
    assert screenshot.unwatched((), 4, 4) == ((0, 0, 4, 4),)


# ---------------------------------------------------------------- route


def kinds(moves: list[menu_navigation.Move]) -> list[str]:
    """Return the kind of each move."""
    return [m.kind for m in moves]


def test_route_from_boot_to_about() -> None:
    """From boot every list is at row 0: single steps down each list, no scroll to the top."""
    moves = menu_navigation.route(TREE, {}, NAMES["language"], NAMES["about"])
    assert kinds(moves) == ["select", *["wheel"] * 5, "select", "select"]
    arrivals = [m.destination.name for m in moves if m.kind == "select"]
    assert arrivals == ["main", "settings", "about"]
    assert all(m.steps == 1 and m.visible for m in moves if m.kind == "wheel")


def test_route_up_then_down_uses_remembered_row() -> None:
    """Going sideways: MENU up once, then wheel from the row the list remembers."""
    positions = {"main": 5, "settings": 0}
    moves = menu_navigation.route(TREE, positions, NAMES["about"], NAMES["brightness"])
    assert kinds(moves) == ["menu", *["wheel"] * 7, "select"]
    assert moves[0].source.name == "about"
    assert moves[0].destination.name == "settings"
    assert [m.steps for m in moves if m.kind == "wheel"] == [1] * 7


def test_route_hidden_rows_and_upward_moves() -> None:
    """Rows past the visible window are unverifiable; a long way up rescrolls from the top."""
    down = menu_navigation.route(TREE, {"settings": 7}, NAMES["settings"], NAMES["legal"])
    assert [m.visible for m in down if m.kind == "wheel"] == [True] + [False] * 7
    up = menu_navigation.route(TREE, {"settings": 15}, NAMES["settings"], NAMES["about"])
    assert kinds(up) == ["top", "select"]
    near = menu_navigation.route(TREE, {"settings": 5}, NAMES["settings"], NAMES["backlight"])
    assert [m.steps for m in near if m.kind == "wheel"] == [1]


def test_route_a_menu_hold_screen_needs_no_wheel() -> None:
    """A screen opened by a long MENU hold is one hold move from its parent, with no wheel moves."""
    moves = menu_navigation.route(HOLD_TREE, {"main": 3}, HOLD_NAMES["main"], HOLD_NAMES["panel"])
    assert kinds(moves) == ["hold"]
    assert moves[0].source.name == "main"
    assert moves[0].destination.name == "panel"


def test_route_a_wrong_position_is_rescrolled_from_the_top() -> None:
    """A position marked None (found wrong) is re-derived: scroll to the top, then down."""
    moves = menu_navigation.route(TREE, {"settings": None}, NAMES["settings"], NAMES["backlight"])
    assert kinds(moves) == ["top", *["wheel"] * 6, "select"]


def test_route_with_checkpoints_restores_instead_of_menu() -> None:
    """With snapshots the way up is one restore, and the way down starts from the saved rows."""
    checkpoints = {"settings": {"main": 5, "settings": 0}}
    moves = menu_navigation.route(
        TREE, {"settings": 9}, NAMES["about"], NAMES["brightness"], checkpoints=checkpoints
    )
    assert kinds(moves) == ["restore", *["wheel"] * 7, "select"]
    assert moves[0].source.name == "about"
    assert moves[0].destination.name == "settings"
    without = menu_navigation.route(
        TREE, {"settings": 0}, NAMES["about"], NAMES["brightness"], checkpoints={}
    )
    assert kinds(without)[0] == "menu"


# ---------------------------------------------------------------- verify


def painted(*boxes: tuple, base: Colour = GREY) -> screenshot.Frame:
    """Return a lit frame of `base` with each (x0, y0, x1, y1, colour) box painted on it."""
    rows = [[base] * WIDTH for _ in range(HEIGHT)]
    for x0, y0, x1, y1, colour in boxes:
        for y in range(y0, y1):
            rows[y][x0:x1] = [colour] * (x1 - x0)
    return screenshot.Frame(WIDTH, HEIGHT, bytes(c for row in rows for pixel in row for c in pixel))


def stub_session(entered: dict | None = None, learned: dict | None = None) -> SimpleNamespace:
    """Return the part of a Session that `verify` reads."""
    return SimpleNamespace(
        entered=entered or {},
        learned=learned or {},
        arrival_key=lambda screen: (screen.name, None),
    )


def check(  # noqa: PLR0913 - one call shape for every rule under test
    move: menu_navigation.Move,
    before: screenshot.Frame,
    after: screenshot.Frame,
    *,
    settled: bool = True,
    probe: screenshot.Frame | None = None,
    session: SimpleNamespace | None = None,
) -> None:
    """Run `verify` on one synthetic observation."""
    seen = menu_navigation.Observation(before, after, settled, probe)
    menu_navigation.verify(move, seen, session or stub_session())  # ty: ignore[invalid-argument-type]


MAIN, SETTINGS = NAMES["main"], NAMES["settings"]
ROW0 = (0, 21, 160, 46, WHITE)
ROW1 = (0, 46, 160, 70, WHITE)
ROW3 = (0, 95, 160, 120, WHITE)
PAGE = (0, 0, 160, 240, WHITE)
STEP = menu_navigation.Move("wheel", MAIN, MAIN, 1, visible=True)
ENTER = menu_navigation.Move("select", MAIN, SETTINGS)
BACK = menu_navigation.Move("menu", SETTINGS, MAIN)
RESTORE = menu_navigation.Move("restore", SETTINGS, MAIN)


def test_verify_accepts_a_good_wheel_step_and_a_good_page_change() -> None:
    """A one-row highlight move and a whole-page change pass every rule."""
    check(STEP, painted(ROW0), painted(ROW1))
    check(ENTER, painted(ROW0), painted(PAGE))


def test_verify_display_blank() -> None:
    """A dark frame is DisplayBlankError."""
    raises(menu_navigation.DisplayBlankError, check, STEP, painted(ROW0), painted(base=BLACK))


def test_verify_key_dropped() -> None:
    """An unchanged frame is KeyDroppedError; a changed status icon alone does not count."""
    raises(menu_navigation.KeyDroppedError, check, STEP, painted(ROW0), painted(ROW0))
    bolt = (280, 5, 300, 15, WHITE)
    raises(menu_navigation.KeyDroppedError, check, ENTER, painted(ROW0), painted(ROW0, bolt))


def test_attempt_sends_a_dropped_key_once() -> None:
    """A key that changed nothing fails the move; it is never resent."""
    sent = []

    def send(move: menu_navigation.Move) -> menu_navigation.Observation:
        sent.append(move)
        return menu_navigation.Observation(painted(ROW0), painted(ROW0), settled=True)

    session = stub_session()
    session.send = send
    raises(menu_navigation.KeyDroppedError, menu_navigation.attempt, session, STEP)
    assert len(sent) == 1


def test_verify_did_not_settle() -> None:
    """A move that never came to rest is DidNotSettleError."""
    raises(
        menu_navigation.DidNotSettleError, check, STEP, painted(ROW0), painted(ROW1), settled=False
    )


def test_verify_top_not_reached() -> None:
    """After the scroll to the top, one more wheel-up must change nothing."""
    top = menu_navigation.Move("top", MAIN, MAIN)
    check(top, painted(ROW1), painted(ROW0), probe=painted(ROW0))
    raises(
        menu_navigation.TopNotReachedError,
        check,
        top,
        painted(ROW3),
        painted(ROW1),
        probe=painted(ROW0),
    )


def test_verify_position_mismatch() -> None:
    """The rows the highlight moved are counted, but only while it stays on a visible row."""
    raises(menu_navigation.PositionMismatchError, check, STEP, painted(ROW0), painted(ROW3))
    hidden = menu_navigation.Move("wheel", SETTINGS, SETTINGS, 1, visible=False)
    check(hidden, painted(ROW0), painted(ROW3))


def test_verify_not_a_page() -> None:
    """A small change after SELECT is NotAPageError (a toggle in place, not a page)."""
    small = (0, 100, 40, 110, WHITE)
    raises(menu_navigation.NotAPageError, check, ENTER, painted(ROW0), painted(ROW0, small))


def test_verify_round_trip() -> None:
    """MENU must return to the frame shown before the child was entered."""
    session = stub_session({"main": painted(ROW1)})
    check(BACK, painted(PAGE), painted(ROW1), session=session)
    raises(
        menu_navigation.RoundTripMismatchError,
        check,
        BACK,
        painted(PAGE),
        painted(ROW0),
        session=session,
    )


def test_verify_restore_is_judged_like_a_round_trip() -> None:
    """A restore must land on the frame saved before the child was entered."""
    session = stub_session({"main": painted(ROW1)})
    check(RESTORE, painted(PAGE), painted(ROW1), session=session)
    raises(
        menu_navigation.RoundTripMismatchError,
        check,
        RESTORE,
        painted(PAGE),
        painted(ROW0),
        session=session,
    )


def test_verify_hold_is_judged_like_a_select() -> None:
    """A long MENU hold must change the screen enough to be a page."""
    hold = menu_navigation.Move("hold", MAIN, HOLD_NAMES["panel"])
    panel = (0, 100, 320, 160, WHITE)
    check(hold, painted(ROW0), painted(ROW0, panel))
    raises(menu_navigation.KeyDroppedError, check, hold, painted(ROW0), painted(ROW0))


def test_verify_arrival_matches_the_first_visit() -> None:
    """A later arrival must match the frame the first arrival showed."""
    first = painted(PAGE)
    session = stub_session(learned={("settings", None): first})
    check(ENTER, painted(ROW0), first, session=session)
    narrower = (0, 0, 100, 240, WHITE)
    raises(
        menu_navigation.ArrivalMismatchError,
        check,
        ENTER,
        painted(ROW0),
        painted(narrower),
        session=session,
    )


if __name__ == "__main__":
    tests = [(name, fn) for name, fn in sorted(globals().items()) if name.startswith("test_")]
    for name, fn in tests:
        fn()
        print(f"ok {name}")
    print(f"{len(tests)} passed")

"""screen_tree.py -- the screen tree: nested, data-only definitions loaded from screen_tree.json.

A screen is where a SELECT on a list row of its parent leads. The file has three top-level keys:
`watch` (the default area compared, required), `root` (the boot screen, children nested inside
it) and `overlays` (screens that appear over any other and are dismissed with MENU; never
navigation targets). Adding a screen = adding an object with its `name` and its `row` (the
parent list's row, counted in wheel-downs from the top):

    {"name": "about", "row": 0}

Fields (all but `name` and `row` optional):
  row                      row of the parent's list that opens this screen; required except on roots
                           and for `via` other than "select"
  via                      how the parent opens it: "select" (default: wheel to `row`, SELECT) or
                           "menu_hold" (a long MENU hold on the parent)
  arrive                   max seconds for the screen to settle after SELECT (default 10)
  watch                    rectangles [x0, y0, x1, y1] (end exclusive) looked at when deciding the
                           screen is still or changed; the rest is never compared. Absent = the
                           file's top-level `watch`, which is required, so every area compared is
                           written down: leave out the regions that change by themselves (the
                           status icons, a marquee, a progress bar, a list's animated right pane)
  requires_charger_state   power the emulator must be launched with: "battery", "wall" or "usb";
                           absent = no requirement
  cannot_leave_in_fast_timing
                           MENU does nothing on it under fast timing, so a boot ends there (Now
                           Playing, for example); reference timing leaves it at once. Such screens
                           (and the subtrees above them) are visited last.
  comment                  free text for people; carries the pitfalls of the screen
  children                 screens reached from this list, sorted by `row`

Nothing here depends on the theme: screens are never recognised by their pixels
(menu_navigation.py).
"""

from pathlib import Path
from typing import Literal

import msgspec

SCREENS_JSON = Path(__file__).resolve().parent / "screen_tree.json"
# Wheel-up presses that surely reach the top of the longest list (Settings, 17 rows).
SCROLL_TO_TOP_PRESSES = 24
SCREEN_SIZE = (320, 240)  # the iPod Classic 6G LCD, as dumped by the monitor

Rectangle = tuple[int, int, int, int]


class ScreenDefinition(msgspec.Struct, frozen=True, forbid_unknown_fields=True):
    """One screen: its place in the tree (`row` in its parent's list) and how to wait for it.

    The fields are the keys of a screen object in screen_tree.json, documented in the module
    docstring. After `load`, `watch` is never None: an absent one has been replaced by the file's
    default.
    """

    name: str
    row: int | None = None
    via: Literal["select", "menu_hold"] = "select"
    arrive: float = 10.0
    watch: tuple[Rectangle, ...] | None = None
    requires_charger_state: Literal["battery", "wall", "usb"] | None = None
    cannot_leave_in_fast_timing: bool = False
    comment: str = ""
    children: tuple["ScreenDefinition", ...] = ()


class ScreenTree(msgspec.Struct, frozen=True, forbid_unknown_fields=True):
    """The whole screen_tree.json: the default `watch`, the boot screen and below, the overlays."""

    watch: tuple[Rectangle, ...]
    root: ScreenDefinition
    overlays: tuple[ScreenDefinition, ...] = ()


def load(path: str | Path = SCREENS_JSON) -> ScreenTree:
    """Load the validated ScreenTree of `path`; ValueError names the JSON path of the problem."""
    path = Path(path)
    with path.open("rb") as handle:
        try:
            tree = msgspec.json.decode(handle.read(), type=ScreenTree)
        except msgspec.ValidationError as error:
            msg = f"{path}: {error}"
            raise ValueError(msg) from error
    seen = set()
    _validate(tree.root, "$.root", seen, is_root=True)
    for i, overlay in enumerate(tree.overlays):
        _validate(overlay, f"$.overlays[{i}]", seen, is_root=True)
    return msgspec.structs.replace(
        tree,
        root=_with_watch(tree.root, tree.watch),
        overlays=tuple(_with_watch(o, tree.watch) for o in tree.overlays),
    )


def _with_watch(screen: ScreenDefinition, default: tuple[Rectangle, ...]) -> ScreenDefinition:
    """`screen` and everything below it with an absent `watch` replaced by `default`."""
    return msgspec.structs.replace(
        screen,
        watch=screen.watch if screen.watch is not None else default,
        children=tuple(_with_watch(child, default) for child in screen.children),
    )


def _validate(screen: ScreenDefinition, where: str, seen: set, *, is_root: bool = False) -> None:
    if screen.name in seen:
        msg = f"{where}: duplicate screen name {screen.name!r}"
        raise ValueError(msg)
    seen.add(screen.name)
    if not is_root and screen.via == "select" and screen.row is None:
        msg = f'{where} ({screen.name}): missing "row"'
        raise ValueError(msg)
    if screen.via != "select" and screen.row is not None:
        msg = f"{where} ({screen.name}): a screen opened by {screen.via} has no row"
        raise ValueError(msg)
    rows = [child.row for child in screen.children if child.row is not None]
    if rows != sorted(set(rows)):
        msg = f"{where} ({screen.name}): children need unique ascending rows, got {rows}"
        raise ValueError(msg)
    for i, child in enumerate(screen.children):
        _validate(child, f"{where}.children[{i}]", seen)


def by_name(tree: ScreenTree) -> dict[str, ScreenDefinition]:
    """{name: ScreenDefinition} for every screen, overlays included."""
    out = {}

    def visit(screen: ScreenDefinition) -> None:
        out[screen.name] = screen
        for child in screen.children:
            visit(child)

    visit(tree.root)
    for overlay in tree.overlays:
        visit(overlay)
    return out


def chain(tree: ScreenTree, name: str) -> tuple[ScreenDefinition, ...]:
    """Return the screens from the root down to `name`; ValueError if `name` is not in the tree."""

    def find(screen: ScreenDefinition, trail: tuple) -> tuple | None:
        trail = (*trail, screen)
        if screen.name == name:
            return trail
        for child in screen.children:
            found = find(child, trail)
            if found:
                return found
        return None

    found = find(tree.root, ())
    if found is None:
        msg = f"{name!r} is not in the screen tree"
        raise ValueError(msg)
    return found


def is_list(screen: ScreenDefinition) -> bool:
    """Return whether `screen` is a list: it has children and the guest keeps its row."""
    return bool(screen.children)


def parents(tree: ScreenTree) -> dict[str, str | None]:
    """{name: parent name or None} for every screen; roots and overlays have no parent."""
    out = {tree.root.name: None}
    for overlay in tree.overlays:
        out[overlay.name] = None

    def visit(screen: ScreenDefinition) -> None:
        for child in screen.children:
            out[child.name] = screen.name
            visit(child)

    visit(tree.root)
    return out


def depth_first(tree: ScreenTree) -> list[str]:
    """Return the tree's screen names, parents before children, overlays last.

    Siblings are in row order, except that a subtree holding an unleavable screen
    (cannot_leave_in_fast_timing) comes after its siblings.
    """
    out = []

    def visit(screen: ScreenDefinition) -> None:
        out.append(screen.name)
        for child in sorted(screen.children, key=leads_to_unleavable):  # stable: unleavable last
            visit(child)

    visit(tree.root)
    return out + [overlay.name for overlay in tree.overlays]


def leads_to_unleavable(screen: ScreenDefinition) -> bool:
    """Return True when `screen` or anything below it cannot be left in fast timing."""
    return screen.cannot_leave_in_fast_timing or any(
        leads_to_unleavable(child) for child in screen.children
    )

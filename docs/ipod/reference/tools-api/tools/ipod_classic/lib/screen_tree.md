# ScreenTree

[tools Index](../../../README.md#tools-index) / [Tools](../../index.md#tools) / [Ipod Classic](../index.md#ipod-classic) / [Lib](./index.md#lib) / ScreenTree

> Auto-generated documentation for [tools.ipod_classic.lib.screen_tree](../../../../../../../tools/ipod_classic/lib/screen_tree.py) module.

```text
screen_tree.py -- the screen tree: nested, data-only definitions loaded from screen_tree.json.

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
```

#### Attributes

- `SCROLL_TO_TOP_PRESSES` - Wheel-up presses that surely reach the top of the longest list (Settings, 17 rows).: 24

- [ScreenTree](#screentree)

  - [ScreenDefinition](#screendefinition)
  - [ScreenTree](#screentree-1)
  - [\_with_watch](#_with_watch)
  - [by_name](#by_name)
  - [chain](#chain)
  - [depth_first](#depth_first)
  - [is_list](#is_list)
  - [leads_to_unleavable](#leads_to_unleavable)
  - [load](#load)
  - [parents](#parents)

## ScreenDefinition

[Show source in screen_tree.py:48](../../../../../../../tools/ipod_classic/lib/screen_tree.py#L48)

One screen: its place in the tree (`row` in its parent's list) and how to wait for it.

The fields are the keys of a screen object in screen_tree.json, documented in the module docstring. After [load](#load), `watch` is never None: an absent one has been replaced by the file's default.

#### Signature

```python
class ScreenDefinition(msgspec.Struct): ...
```

## ScreenTree

[Show source in screen_tree.py:67](../../../../../../../tools/ipod_classic/lib/screen_tree.py#L67)

The whole screen_tree.json: the default `watch`, the boot screen and below, the overlays.

#### Signature

```python
class ScreenTree(msgspec.Struct): ...
```

## \_with_watch

[Show source in screen_tree.py:95](../../../../../../../tools/ipod_classic/lib/screen_tree.py#L95)

`screen` and everything below it with an absent `watch` replaced by `default`.

#### Signature

```python
def _with_watch(
    screen: ScreenDefinition, default: tuple[Rectangle, ...]
) -> ScreenDefinition: ...
```

#### See also

- [Rectangle](#rectangle)
- [ScreenDefinition](#screendefinition)

## by_name

[Show source in screen_tree.py:123](../../../../../../../tools/ipod_classic/lib/screen_tree.py#L123)

{name: ScreenDefinition} for every screen, overlays included.

#### Signature

```python
def by_name(tree: ScreenTree) -> dict[str, ScreenDefinition]: ...
```

#### See also

- [ScreenDefinition](#screendefinition)
- [ScreenTree](#screentree)

## chain

[Show source in screen_tree.py:138](../../../../../../../tools/ipod_classic/lib/screen_tree.py#L138)

Return the screens from the root down to `name`; ValueError if `name` is not in the tree.

#### Signature

```python
def chain(tree: ScreenTree, name: str) -> tuple[ScreenDefinition, ...]: ...
```

#### See also

- [ScreenDefinition](#screendefinition)
- [ScreenTree](#screentree)

## depth_first

[Show source in screen_tree.py:178](../../../../../../../tools/ipod_classic/lib/screen_tree.py#L178)

Return the tree's screen names, parents before children, overlays last.

Siblings are in row order, except that a subtree holding an unleavable screen (cannot_leave_in_fast_timing) comes after its siblings.

#### Signature

```python
def depth_first(tree: ScreenTree) -> list[str]: ...
```

#### See also

- [ScreenTree](#screentree)

## is_list

[Show source in screen_tree.py:158](../../../../../../../tools/ipod_classic/lib/screen_tree.py#L158)

Return whether `screen` is a list: it has children and the guest keeps its row.

#### Signature

```python
def is_list(screen: ScreenDefinition) -> bool: ...
```

#### See also

- [ScreenDefinition](#screendefinition)

## leads_to_unleavable

[Show source in screen_tree.py:195](../../../../../../../tools/ipod_classic/lib/screen_tree.py#L195)

Return True when `screen` or anything below it cannot be left in fast timing.

#### Signature

```python
def leads_to_unleavable(screen: ScreenDefinition) -> bool: ...
```

#### See also

- [ScreenDefinition](#screendefinition)

## load

[Show source in screen_tree.py:75](../../../../../../../tools/ipod_classic/lib/screen_tree.py#L75)

Load the validated ScreenTree of `path`; ValueError names the JSON path of the problem.

#### Signature

```python
def load(path: str | Path = SCREENS_JSON) -> ScreenTree: ...
```

#### See also

- [SCREENS_JSON](#screens_json)
- [ScreenTree](#screentree)

## parents

[Show source in screen_tree.py:163](../../../../../../../tools/ipod_classic/lib/screen_tree.py#L163)

{name: parent name or None} for every screen; roots and overlays have no parent.

#### Signature

```python
def parents(tree: ScreenTree) -> dict[str, str | None]: ...
```

#### See also

- [ScreenTree](#screentree)

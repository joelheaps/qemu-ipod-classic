# Screenshot Diff

[tools Index](../../README.md#tools-index) / [Tools](../index.md#tools) / [Ipod Classic](./index.md#ipod-classic) / Screenshot Diff

> Auto-generated documentation for [tools.ipod_classic.screenshot_diff](../../../../../../tools/ipod_classic/screenshot_diff.py) module.

```text
screenshot_diff.py A B [--tolerance N] -- compare two screenshots or two directories of them.

The frames of a directory are paired by file name.

Prints `name bbox=(x0,y0,x1,y1) diff_px=N` per frame. Exit status 1 if any frame differs by more
than --tolerance pixels (default 0) or is missing in B.
```

- [Screenshot Diff](#screenshot-diff)
  - [main](#main)

## main

[Show source in screenshot_diff.py:24](../../../../../../tools/ipod_classic/screenshot_diff.py#L24)

Compare the two paths in `argv`; return the exit status (2 on usage, 1 on a difference).

#### Signature

```python
def main(argv: list[str]) -> int: ...
```

#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""screenshot_diff.py A B [--tolerance N] -- compare two screenshots or two directories of them.

The frames of a directory are paired by file name.

Prints `name bbox=(x0,y0,x1,y1) diff_px=N` per frame. Exit status 1 if any frame differs by more
than --tolerance pixels (default 0) or is missing in B.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import screenshot

USAGE_ARGS = 2
USAGE = 2


def main(argv: list[str]) -> int:
    """Compare the two paths in `argv`; return the exit status (2 on usage, 1 on a difference)."""
    if len(argv) < USAGE_ARGS:
        print(__doc__)
        return USAGE
    a, b = Path(argv[0]), Path(argv[1])
    tol = int(argv[argv.index("--tolerance") + 1]) if "--tolerance" in argv else 0
    if a.is_dir():
        pairs = [
            (p.name, p, b / p.name) for p in sorted(a.iterdir()) if p.suffix in (".ppm", ".png")
        ]
    else:
        pairs = [(a.name, a, b)]
    bad = 0
    for n, fa, fb in pairs:
        if not fb.exists():
            print(n, "missing in B")
            bad += 1
            continue
        cnt, bbox = screenshot.diff(screenshot.read(fa), screenshot.read(fb))
        print(f"{n} bbox={bbox} diff_px={cnt}")
        bad += cnt > tol
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

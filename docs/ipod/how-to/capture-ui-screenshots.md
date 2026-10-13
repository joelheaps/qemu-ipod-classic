# How to capture UI screenshots

Task recipes for the rapid screenshot harness (`tools/ipod_classic/capture_screenshots.py`). Output naming, exit status, `screen_tree.json` and environment variables: [`reference/screenshot-harness.md`](../reference/screenshot-harness.md); the full flag list and library functions: [`reference/tools-api/README.md`](../reference/tools-api/README.md).

Prerequisites: build the emulator first (`mise run build`, or `mise run docker:build` for the Docker build); the harness runs `BUILD_DIR/qemu-system-arm` (default `build/native`). Run everything from the repository root. The scripts bootstrap their own dependencies with `uv`. The run keeps its VM snapshots in a private directory on `/dev/shm` (a few hundred MB while it runs, deleted at the end).

## Capture every screen of an image

```sh
uv run --script tools/ipod_classic/capture_screenshots.py assets/firmware/2.0.5/osos_decrypted.bin --out out/stock
```

One PNG per screen lands directly in `out/stock/`, named by the path through the menus (`main_settings_about.png`). The exit status is the verdict: 0 means every requested screen was reached and drew; a non-zero status prints `survey: FAIL <screen>: <reason>` for each screen that did not. Overlays are left out of the default set.

## Capture only some screens

```sh
uv run --script tools/ipod_classic/capture_screenshots.py assets/firmware/2.0.5/osos_decrypted.bin --screens brightness,about --out out/two
```

Names are the `name` fields in `screen_tree.json`. The screens passed through on the way (`main`, `settings`) are saved too. An unknown name stops the run with exit status 2 before any boot, and nothing is written.

## Compare two images

Run the harness once per image into separate directories, then `screenshot_diff.py` them:

```sh
uv run --script tools/ipod_classic/capture_screenshots.py IMAGE_A --out out/a
uv run --script tools/ipod_classic/capture_screenshots.py IMAGE_B --out out/b
uv run --script tools/ipod_classic/screenshot_diff.py out/a out/b
```

Names are identical across runs, so the diff pairs them. Each line is `<file> bbox=<changed box> diff_px=<pixels>`; `bbox=None diff_px=0` is identical, and a screen missing from the second directory prints `missing in B`. `--tolerance N` counts differences of at most N pixels as equal; the exit status is 1 when any screen differs by more than that. An image that lacks a screen in `screen_tree.json` needs `--without NAME`, because every requested screen must be reached or the run fails.

Expect small differences even between identical images on Now Playing (elapsed time) and Cover Flow (animation).

## Capture the Charging screen

```sh
uv run --script tools/ipod_classic/capture_screenshots.py assets/firmware/2.0.5/osos_decrypted.bin --screens charging --out out/charging
```

Overlays are left out of the default run, so ask for it by name. The harness boots it on the wall adapter by itself (the screen's `requires_charger_state`) and waits for it to appear on its own.

## Add a screen to screen_tree.json

1. Find the screen's parent and the row of the parent's list that opens it (count wheel-downs from the top).

1. Add an object to `tools/ipod_classic/lib/screen_tree.json`, inside the parent's `children`, ordered by `row`:

   ```json
   {"name": "my_screen", "row": 3}
   ```

1. Validate the file with `mise run check:tests` (the loader names the JSON path of any mistake), then capture it: `--screens my_screen`.

A screen opened by a long MENU hold takes `"via": "menu_hold"` instead of a `row`. Add `"watch": [[x0, y0, x1, y1]]` only if the screen has a part that changes by itself (a marquee, a progress bar): list the rectangles that stay still. Without it the screen uses the file's default area. If MENU does nothing on the screen under fast timing, add `"cannot_leave_in_fast_timing": true`; that matters for `goto_screen.py` and library use, because the harness leaves screens by restoring a snapshot.

## Read the exit status

The run prints a summary on success and `survey: FAIL <screen>: <reason>` on stderr for each failure. Exit status 0 means every requested screen was reached and drew; 1 means at least one failed; 2 means an unknown name on the command line, before any boot.

| Reason | What to do |
| -- | -- |
| `N lit pixels` | The display went dark. Rerun; if it repeats, use `--no-fast`. |
| `screen unchanged` | A key changed nothing. Rerun; if the host is busy, lower `--parallel`. |
| `still changing after Ns` | The screen animates. Add a `watch` area that leaves the moving part out, or raise its `arrive`. |
| `a further wheel-up still changed the list` | The list was not at the top after the scroll to the top. Rerun. |
| `moved N rows, wanted M` | The list highlight is not where the tree says. The run restarts the branch once by itself; if it still fails, check the `row` values against the real list. |
| `only N% changed` | The SELECT did not open a page (it toggled something in place). Check the `row`. |
| `differs from the frame before entering the child` | Going back up did not land on the saved parent frame. Rerun; if it repeats, narrow the parent's `watch`. |
| `differs from the first arrival this run` | The screen drew differently the second time. Add a `watch` area that leaves the changing part out. |
| `<screen> did not appear within Ns` (overlay) | Raise the overlay's `arrive`. |

To confirm a surprising result, rerun with `--no-fast` (reference wall-clock timing, which avoids the fast-timing failures) or `--cold-boot` (every branch boots from scratch instead of restoring one boot snapshot), or both.

# Rapid screenshot harness reference

Facts about the rapid screenshot harness (`tools/ipod_classic/capture_screenshots.py`): output file naming, exit status, the `screen_tree.json` schema and its validation, charger states and environment variables. Recipes: [`how-to/capture-ui-screenshots.md`](../how-to/capture-ui-screenshots.md). Command-line flags and library functions: [`tools-api/README.md`](tools-api/README.md).

## Output file naming

One PNG per reached screen is written directly in `--out` (default under `$SCRATCH`). A name is the screen's path through the menus, parent first, joined with underscores, rooted at `main`: `main.png`, `main_settings_about.png`, `main_music_cover_flow.png`, `main_music_songs_now_playing.png`. The boot screen is standalone, `language.png`, and so is an overlay, `charging.png`. Screens passed through on the way are written too. Names are stable across runs, so `screenshot_diff.py DIR_A DIR_B` pairs two surveys.

## Exit status

| Status | Meaning |
| -- | -- |
| 0 | Every requested screen was reached and drew. |
| 1 | A boot, navigation check, all-black frame or missing frame failed. Each failure prints `survey: FAIL <screen>: <reason>`. |
| 2 | An unknown `--screens` or `--without` name (before any boot; nothing is written). |

`screenshot_diff.py` exits 1 when any paired screen differs by more than `--tolerance` (default 0) or is missing from the second directory.

## screen_tree.json

`tools/ipod_classic/lib/screen_tree.json` is the screen tree the harness walks. The file has three top-level keys:

| Key | Meaning |
| -- | -- |
| `watch` | Required. Default rectangles `[x0, y0, x1, y1]` (end-exclusive) compared when judging a screen still or changed. |
| `root` | Required. The boot screen; its descendants are nested under `children`. |
| `overlays` | Screens that appear over any other screen and are dismissed with MENU. Never navigation targets. |

A screen object:

| Field | Required | Meaning |
| -- | -- | -- |
| `name` | yes | Screen name, unique across the whole tree. |
| `row` | yes except on a root and when `via` is not `select` | Row of the parent's list that opens it, counted in wheel-downs from the top. |
| `via` | no | `select` (default: wheel to `row`, then SELECT) or `menu_hold` (a long MENU hold on the parent). |
| `arrive` | no | Max seconds for the screen to settle after opening (default 10). |
| `watch` | no | Rectangles to judge; absent uses the file's `watch`. |
| `requires_charger_state` | no | `battery`, `wall` or `usb`; absent means no requirement. |
| `cannot_leave_in_fast_timing` | no | MENU does nothing here under fast timing, so a boot ends on this screen. |
| `comment` | no | Free text. |
| `children` | no | Screens reached from this list, in ascending `row`. |

Validation rejects a bad file and names the JSON path of the problem:

- Unknown fields, wrong field types and bad enum values are rejected.
- Screen names must be unique.
- A screen whose `via` is `select` must have a `row`; a screen opened any other way must not.
- A parent's children must have unique ascending `row` values.
- `watch` must be present at the top level.
- Every screen gets a `watch` after loading (its own or the file default), so the areas compared are always explicit.

## Charger states

`requires_charger_state` says what the emulated power state must be when the branch boots: `battery`, `wall` (adapter) or `usb`. A screen without it boots on battery. The harness maps the state to `PMUCHARGER` (`wall` -> `adapter`) and splits the requested screens into branches so one cold boot never mixes states; the state is read at boot, so it is part of the boot snapshot. The `charging` overlay requires `wall`, is left out of the default screen set and appears on its own (see the how-to).

## Environment variables

| Variable | Effect |
| -- | -- |
| `BUILD_DIR` | Directory holding `qemu-system-arm` (default `build/native`). |
| `SCRATCH` | Scratch directory (default `assets/scratch`); holds the default `--out` and the staging fallback when `/dev/shm` is unavailable. |
| `PMUCHARGER` | Set by the harness per branch (`battery`, `adapter` or `usb`); leave it unset. |
| `FWGUID` | Firmware GUID forwarded to QEMU (default `000A270021574219`); the music database is rejected without it. |
| `IRAMSPILL` | Bytes above IRAM1 the guest may map (default `0x10000`). |

Other device-model knobs (`AUDIOWAV`, `DISPLOG`, `DISPTRACE`, ...) are forwarded from the environment.

## Known limits

- Fast timing is the default; a wall gap without a key blanks the LCD, and no key wakes it again, so use `--no-fast` to confirm a surprising result.
- Under fast timing Now Playing cannot be left with MENU; the harness visits it last in its branch and leaves it by restoring a snapshot.
- Playback and the music database under fast timing are unverified.
- The emulated wheel loses no input, so a key that changed nothing fails the run instead of being resent.
- Reset Settings is entered and backed out with MENU, never confirmed.
- Now Playing elapsed time and the Cover Flow animation vary between runs, so they can differ in a diff even between identical images.

# assets/

Untracked data; only `README.md` files are in git. Firmware is Apple's and is not redistributed:
supply your own dumps (see the top-level README) and pass paths to the tools.

| Folder | Holds | Regenerable |
|---|---|---|
| `disks/` | QEMU disk images (iPod HDD layouts with a music DB) | yes (external disk-image tooling, not in this tree) |
| `scratch/` | throwaway: `$SCRATCH` (default write target of tools), run dirs, screenshots | n/a |

Firmware inputs go under `assets/firmware/...`. `build/` is not tracked: rebuild with
`mise run emulator:build`.

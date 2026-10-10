# How to run the iPod video 5G

Tasks against `-M iPod-Video`. Hardware reference, the VideoCore interface and the model's deviations: `hw/arm/ipod_video/README.md`.

| Step | Command |
| -- | -- |
| Dump the firmware partition (Windows, admin, read-only) | `tools/ipod_video/dump-firmware.ps1`; use `ipod-video-fwpart.bin`, not the full dump (which starts at the MBR), when extracting images |
| Build a disk | `tools/ipod_video/mkdisk.sh <fw-dump> <files-dir> <output> [size-mb]` |
| Boot | `tools/ipod_video/run.sh rockbox\|apple` (`-M iPod-Video,entry=0x735a00` / `entry=0`, `-kernel osos_full.bin`) |
| Evidence | `navtest.sh`, `appletest.sh`: they print pixel numbers and assert nothing (`\|\| true`); read the numbers |

Properties: `entry` (offset into the image to start at), `remap` (DRAM at 0 from reset, to run a Rockbox app image directly), `semihosting` (default on; the retailOS prints panics through it). Keys as on the classic: `q`/`e` wheel, `Enter` SELECT, `w` MENU, `a`/`d` prev/next, `s` play.

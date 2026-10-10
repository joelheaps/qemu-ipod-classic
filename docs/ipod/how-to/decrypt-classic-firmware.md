# How to decrypt iPod classic 6G firmware

Turn an Apple `.ipsw` into `osos_decrypted.bin`, the file `scripts/ipod/run` boots. The chip's key can't be read out, so the iPod itself has to decrypt it. File formats: `docs/ipod/classic6g-firmware.md`.

You need an iPod classic 6G running [emCORE](https://freemyipod.org/wiki/Classic_6G) and plugged in, a computer with direct USB access (not the Docker container) and [uv](https://docs.astral.sh/uv/) (the scripts install their own dependencies), and the `.ipsw` from [The Apple Wiki](<https://theapplewiki.com/wiki/Firmware/iPod#iPod_classic_(6th_generation)>). Run as root, or allow USB device `ffff:e000` with a udev rule.

```sh
ipsw=iPod_<id>.<version>.ipsw
osos=osos_decrypted.bin             # wherever you want them
uv run --script tools/ipod_classic/fwtool.py osos --firmware=$ipsw --osos-encrypted=$osos.enc
uv run --script tools/ipod_classic/emcore_s5l8702.py decrypt $osos.enc $osos
```

`fwtool.py osos` copies the `osos` image, header first, out of the `.ipsw` (a bare `Firmware-XX` payload works too); `FIRMWARE` and `OSOS_ENCRYPTED` in the environment replace the flags. `uv run --script tools/ipod_classic/fwtool.py list --firmware=$ipsw` shows the images it finds.

Check that `xxd -l 4 $osos` prints `58 22 00 ea` (2.0.5). Anything else means the wrong image was decrypted. Then boot with `scripts/ipod/run --osos=$osos`.

If the iPod stops responding, reset it (hold MENU+SELECT) and retry.

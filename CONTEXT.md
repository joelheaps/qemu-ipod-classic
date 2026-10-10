# qemu-ipod

A QEMU fork that emulates iPod hardware closely enough to boot and run Apple's own firmware, aimed at the iPod classic 6G (with the video 5G work kept intact). This glossary fixes the vocabulary for the firmware and for the ways the emulator is run.

## Devices

**iPod Classic 6G**: The S5L8702-based iPod classic (Apple codename `N25`); the primary emulation target. _Avoid_: classic, 6G, iPod classic

**iPod Video 5G**: The PP5022/BCM-based fifth-generation iPod (the "iPod with video"); the older target. _Avoid_: 5G, video iPod

## Firmware

**osos**: Refers to the `osos` firmware image section in the iPod's firmware partition. It's a lower-level, technical identifier rather than the conventional name for the OS. _Avoid_: the OS, ROM, kernel

**RetailOS**: Commonly used to distinguish Apple's original firmware from Rockbox or other alternative firmware. _Avoid_: Apple OS, stock OS

**Rockbox**: An alternative open-source firmware, used here as a second boot target and as a reference for hardware behaviour. _Avoid_: —

**emCORE**: A third-party iPod bootloader/monitor used here to decrypt and dump the device's firmware. Not an emulation target. _Avoid_: bootloader, firmware (for Apple's own boot stages)

## Running the emulator

**headless run** (`ipod:run --headless`): A boot of the retailOS with no display, bounded by `--timeout` seconds. _Avoid_: boot-headless, run-headless-as-SDL, headless boot

**live window** (`ipod:run`): The persistent, managed SDL window (container `ipod-live`) with monitor, gdb and optional host audio. _Avoid_: live, emulator:run, emulator:live

**attach gdb** (`ipod:live-gdb`): GDB attached to the running live window. _Avoid_: —

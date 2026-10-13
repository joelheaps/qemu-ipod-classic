"""Construct Docker invocations of the same mise tasks used natively."""

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FORWARD = (
    "OSOS",
    "DISK",
    "FIRMWARE",
    "TRACK",
    "SCRATCH",
    "IPOD_TEST_IPSW",
    "JOBS",
    "QEMU_PYTHON",
    "T",
    "FWGUID",
    "IRAMSPILL",
    "AUDIO",
    "AUDIOWAV",
    "AUDIOLIVE",
    "DUMP",
    "GDB",
    "IRAMNOISE",
    "BOOTINFO",
    "LCDTYPE",
    "PMUCHARGER",
    "PMUHOLD",
    "PMUOOCSTAT",
    "GPIO_MEDIA",
    "LCD_DISPON",
    "DISPTRACE",
    "DISPLOG",
    "WHEELTRACE",
    "PMUTRACE",
    "PLUGIN",
)


def paths(args):
    """Resolve firmware/disk arguments for mounts without rewriting task arguments."""
    values = {key: os.environ.get(key, "") for key in ("OSOS", "DISK", "FIRMWARE", "TRACK")}
    flags = {"osos": "OSOS", "disks": "DISK", "firmware": "FIRMWARE", "track": "TRACK"}
    for arg in args:
        name, sep, value = arg.removeprefix("--").partition("=")
        if sep and name in flags:
            values[flags[name]] = value
    return values


def command(image, task, args):
    """Preserve uid, external input/output directories, flags and selected environment."""
    mounts = {ROOT}
    values = list(paths(args).values())
    values += [os.environ.get(key, "") for key in ("SCRATCH", "IPOD_TEST_IPSW", "AUDIOWAV")]
    for value in values:
        if not value:
            continue
        directory = (ROOT / value).resolve().parent
        while not directory.exists():
            directory = directory.parent
        if directory != ROOT and ROOT not in directory.parents:
            mounts.add(directory)
    cmd = ["docker", "run", "--rm", "-i", "--user", f"{os.getuid()}:{os.getgid()}"]
    for directory in sorted(mounts):
        cmd += ["-v", f"{directory}:{directory}"]
    for key in FORWARD:
        if key in os.environ:
            cmd += ["-e", key]
    cmd += [
        "-e",
        "BUILD_DIR=build/docker",
        "-e",
        f"MISE_CACHE_DIR=/tmp/mise-cache-{os.getuid()}",
        "-e",
        f"MISE_STATE_DIR=/tmp/mise-state-{os.getuid()}",
        "-e",
        f"UV_CACHE_DIR=/tmp/uv-cache-{os.getuid()}",
        "-e",
        f"UV_TOOL_DIR=/tmp/uv-tools-{os.getuid()}",
        "-e",
        f"PIP_CACHE_DIR=/tmp/pip-cache-{os.getuid()}",
        "-w",
        str(ROOT),
        image,
        "mise",
        "run",
        task,
        "--",
        *args,
    ]
    return cmd


if __name__ == "__main__":
    image, task, *args = sys.argv[1:]
    raise SystemExit(subprocess.call(command(image, task, args)))

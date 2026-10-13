"""Put a one-track music library on a FAT image: the logic behind `make-disk --track`.

The retailOS lists a track only if iTunesDB carries libgpod's checksum, so the database is
rewritten through libgpod (which signs it with the FireWire GUID in Device/SysInfo) instead of
being patched by hand. The starting point is sample_library/ipod_control, a library written by
libgpod with one track (portblue.m4a, the vetted AAC song); the requested track replaces that
file and the fields libgpod keeps for it (title, path, size, length, bitrate). Needs mtools,
ffprobe and libgpod.so.4; the qemu-ipod-dev container has them.
"""

import ctypes
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SAMPLE_LIBRARY = Path(__file__).resolve().parent / "sample_library"
PORT_BLUE = SAMPLE_LIBRARY / "portblue.m4a"
FOLDER = "F00"  # the Music folder the track goes in (the retailOS reads F00..F49)

# Itdb_Track field offsets in libgpod 0.8.3 (x86-64): gchar* title, gchar* ipod_path, then gint32s.
TITLE, IPOD_PATH, SIZE, TRACKLEN, BITRATE = 8, 16, 220, 224, 244
FILETYPE_MARKER = 352  # guint32: the four-character container code iTunesDB stores, e.g. 'M4A '
MARKERS = {".m4a": b"M4A ", ".mp3": b"MP3 ", ".wav": b"WAV ", ".aif": b"AIFF", ".aiff": b"AIFF"}
# What those offsets hold for the fixture track; checked before writing, so a different libgpod
# fails loudly.
FIXTURE_VALUES = {SIZE: 3425742, TRACKLEN: 106138, BITRATE: 257}


def ptr(addr: int) -> int:
    """Read the pointer stored at ADDR (0 for NULL)."""
    return ctypes.c_void_p.from_address(addr).value or 0


def probe(track: Path) -> dict[str, int]:
    """Return the track's duration (ms) and bitrate (kbps), from ffprobe."""
    out = subprocess.check_output(  # noqa: S603
        [  # noqa: S607
            "ffprobe",
            "-v",
            "error",
            "-show_entries",
            "format=duration,bit_rate",
            "-of",
            "json",
            track,
        ]
    )
    fmt = json.loads(out)["format"]
    return {"ms": round(float(fmt["duration"]) * 1000), "kbps": round(int(fmt["bit_rate"]) / 1000)}


def rewrite_database(root: Path, track: Path) -> None:
    """Point the fixture database under ROOT (holding iPod_Control) at TRACK and re-sign it."""
    gpod = ctypes.CDLL("libgpod.so.4")
    glib = ctypes.CDLL("libglib-2.0.so.0")
    gpod.itdb_parse.restype = ctypes.c_void_p
    gpod.itdb_parse.argtypes = [ctypes.c_char_p, ctypes.c_void_p]
    gpod.itdb_track_remove.argtypes = [ctypes.c_void_p]
    gpod.itdb_playlist_remove_track.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    gpod.itdb_write.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    glib.g_strdup.restype = ctypes.c_void_p
    err = ctypes.c_void_p()
    db = gpod.itdb_parse(str(root).encode(), ctypes.byref(err))
    if not db:
        sys.exit("make-disk: libgpod could not read the fixture database")
    tracks = ptr(db)  # Itdb_iTunesDB.tracks, a GList
    node = ptr(tracks)  # its data: the first Itdb_Track
    extra, rest = [], ptr(tracks + 8)  # GList.next
    while rest:  # the fixture also lists a video whose file is not shipped: drop it
        extra.append(ptr(rest))
        rest = ptr(rest + 8)
    for other in extra:
        playlist = ptr(db + 8)  # Itdb_iTunesDB.playlists, a GList
        while playlist:  # itdb_track_remove alone leaves the track referenced by the playlists
            gpod.itdb_playlist_remove_track(ptr(playlist), other)
            playlist = ptr(playlist + 8)
        gpod.itdb_track_remove(other)
    for off, want in FIXTURE_VALUES.items():
        if ctypes.c_int32.from_address(node + off).value != want:
            sys.exit(f"make-disk: unexpected libgpod Itdb_Track layout at +{off}")
    ctypes.c_uint32.from_address(node + FILETYPE_MARKER).value = int.from_bytes(
        MARKERS[track.suffix.lower()], "big"
    )
    info = probe(track)
    for off, value in {
        SIZE: track.stat().st_size,
        TRACKLEN: info["ms"],
        BITRATE: info["kbps"],
    }.items():
        ctypes.c_int32.from_address(node + off).value = value
    for off, text in {
        TITLE: track.stem,
        IPOD_PATH: f":iPod_Control:Music:{FOLDER}:{track.name}",
    }.items():
        ctypes.c_void_p.from_address(node + off).value = glib.g_strdup(text.encode())
    if not gpod.itdb_write(db, ctypes.byref(err)):
        sys.exit("make-disk: libgpod could not write the database")


def populate(fat: Path, track: Path = PORT_BLUE) -> None:
    """Copy the library with TRACK into the FAT image FAT (a freshly formatted, empty volume)."""
    if track.suffix.lower() not in MARKERS:
        sys.exit(f"make-disk: no container code for {track.suffix} (known: {', '.join(MARKERS)})")
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        shutil.copytree(SAMPLE_LIBRARY / "ipod_control", root / "iPod_Control")
        (root / "iPod_Control/Music" / FOLDER).mkdir(parents=True)
        shutil.copy(track, root / "iPod_Control/Music" / FOLDER / track.name)
        rewrite_database(root, root / "iPod_Control/Music" / FOLDER / track.name)
        env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
        subprocess.check_call(  # noqa: S603
            ["mcopy", "-s", "-n", "-i", fat, root / "iPod_Control", "::"],  # noqa: S607
            env=env,
        )

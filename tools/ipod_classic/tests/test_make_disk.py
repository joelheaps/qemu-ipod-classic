# /// script
# requires-python = ">=3.12"
# dependencies = ["pytest"]
# ///
"""Tests for make_disk.py. Run: mise run check:tests.

The end-to-end tests need docker, the qemu-ipod-dev image and build/qemu-img, and skip without
them. The real-firmware test also needs an Apple .ipsw under Apple's file name
(iPod_<family>.<version>.ipsw) at $IPOD_TEST_IPSW, default assets/test/iPod_38.2.0.5.ipsw; keep it
under assets/ (gitignored). All other fixtures are synthesized here.
"""

import os
import plistlib
import re
import shutil
import struct
import subprocess
import zipfile
from pathlib import Path

import pytest
from fwtool import decode_version, read_firmware
from ipod_library import PORT_BLUE, populate
from make_disk import BS, FAT_BYTES, FW_BLOCK, REPO, main, parse_args, prepare

QEMU_IMG = REPO / "build/qemu-img"


def fake_payload(size: int = 3 * BS + 123) -> bytes:
    """Return a payload that passes the ]ih[ check and is not a multiple of the block size."""
    data = bytearray(bytes(range(256)) * (size // 256 + 1))[:size]
    data[0x100:0x104] = b"]ih["
    return bytes(data)


def make_ipsw(path: Path, payload: bytes, manifest: dict[str, object] | None = None) -> Path:
    """Write a minimal .ipsw: the payload plus a manifest.plist naming it."""
    info = {"FirmwareName": "Firmware-1.2.3", "VisibleBuildID": 0x02058000}
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("Firmware-1.2.3", payload)
        if manifest != {}:
            z.writestr(
                "manifest.plist", plistlib.dumps({"FirmwarePayload": info | (manifest or {})})
            )
    return path


@pytest.mark.parametrize(
    ("build_id", "want"),
    [
        (0x02058000, "2.0.5"),  # the 2.0.5 .ipsw; its BuildID 0x09058000 is not the version
        (0x01128000, "1.1.2"),  # the 1.1.2 .ipsw
        (0x10008000, "10.0.0"),  # major is BCD
        (0x020A8000, None),  # minor not BCD
        (0x02056000, None),  # beta stage
        (0x02058001, None),  # pre-release revision
        (1 << 32, None),
        ("2.0.5", None),
    ],
)
def test_decode_version(build_id: object, want: str | None) -> None:
    """VisibleBuildID decodes as a final-release NumVersion; anything else raises."""
    if want is None:
        with pytest.raises(ValueError, match="VisibleBuildID"):
            decode_version(build_id)
    else:
        assert decode_version(build_id) == want


def test_read_firmware(tmp_path: Path) -> None:
    """An .ipsw yields its payload and version, a bare payload no version; bad input is named."""
    payload = fake_payload()
    assert read_firmware(make_ipsw(tmp_path / "ok.ipsw", payload)) == (payload, "2.0.5")
    (tmp_path / "Firmware-x").write_bytes(payload)
    assert read_firmware(tmp_path / "Firmware-x") == (payload, None)
    (tmp_path / "Firmware-bad").write_bytes(b"\0" * 0x200)
    make_ipsw(tmp_path / "badpayload.ipsw", b"\0" * 0x200)
    make_ipsw(tmp_path / "nomanifest.ipsw", payload, {})
    make_ipsw(tmp_path / "nomember.ipsw", payload, {"FirmwareName": "Firmware-9"})
    make_ipsw(tmp_path / "beta.ipsw", payload, {"VisibleBuildID": 0x02056000})
    for name, err in [
        ("Firmware-bad", r"no \]ih\[ at 0x100"),
        ("badpayload.ipsw", r"no \]ih\[ at 0x100"),
        ("nomanifest.ipsw", "manifest.plist"),
        ("nomember.ipsw", "Firmware-9"),
        ("beta.ipsw", "not a final-release"),
        ("missing.ipsw", "No such file"),
    ]:
        with pytest.raises((ValueError, OSError), match=err):
            read_firmware(tmp_path / name)


@pytest.mark.parametrize("form", ["--{}={}", "{}={}"])
def test_parse_args(form: str, monkeypatch: pytest.MonkeyPatch) -> None:
    """Both flag forms parse over the FIRMWARE/DISK env defaults; bad flags exit with the usage."""
    monkeypatch.setenv("FIRMWARE", "env.ipsw")
    monkeypatch.setenv("DISK", "env.qcow2")
    assert parse_args([form.format("disks", "d.qcow2")]) == {
        "firmware": "env.ipsw",
        "disks": "d.qcow2",
        "track": "",
    }
    for bad in (form.format("bogus", "x"), form.format("disks", "")):
        with pytest.raises(SystemExit, match="usage"):
            parse_args([bad])


@pytest.fixture
def no_env(monkeypatch: pytest.MonkeyPatch) -> None:
    """Unset FIRMWARE/DISK and empty PATH, so any docker or qemu-img call would fail."""
    monkeypatch.delenv("FIRMWARE", raising=False)
    monkeypatch.delenv("DISK", raising=False)
    monkeypatch.delenv("TRACK", raising=False)
    monkeypatch.setenv("PATH", "")


@pytest.mark.usefixtures("no_env")
def test_noops(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    """Nothing configured (as ipod:run's depends=) or an existing disk: exit 0, touch nothing."""
    assert main([]) == 0
    assert "nothing to do" in capsys.readouterr().out
    disk = tmp_path / "d.qcow2"
    disk.write_bytes(b"keep")
    for argv in ([f"--disks={disk}", f"--firmware={tmp_path / 'absent.ipsw'}"], [f"disks={disk}"]):
        assert main(argv) == 0
        assert "exists" in capsys.readouterr().out
    assert disk.read_bytes() == b"keep"


@pytest.mark.usefixtures("no_env")
def test_errors(tmp_path: Path) -> None:
    """Missing flags give the usage; a bad payload fails before any container starts."""
    disk = tmp_path / "d.qcow2"
    for argv in ([f"--disks={disk}"], [f"--firmware={tmp_path / 'a.ipsw'}"]):
        with pytest.raises(SystemExit, match="usage"):
            main(argv)
    (tmp_path / "Firmware-bad").write_bytes(b"\0" * 0x200)
    with pytest.raises(SystemExit, match="not a firmware payload"):
        main([f"--firmware={tmp_path / 'Firmware-bad'}", f"--disks={disk}"])
    assert not disk.exists()


def _e2e_missing() -> str:
    if not shutil.which("docker"):
        return "docker not installed"
    image = subprocess.run(
        ["docker", "image", "inspect", "qemu-ipod-dev"],  # noqa: S607
        capture_output=True,
        check=False,
    )
    if image.returncode:
        return "docker unreachable or no qemu-ipod-dev image (mise run docker:build-dev-image)"
    if not QEMU_IMG.exists():
        return "no build/qemu-img (mise run build-in-docker)"
    return ""


E2E_MISSING = _e2e_missing()
e2e = pytest.mark.skipif(bool(E2E_MISSING), reason=E2E_MISSING)


def build_and_check(firmware: Path, disk: Path, payload: bytes) -> str:
    """Run make-disk, check the image (MBR, FAT markers, firmware bytes); return its output."""
    run = subprocess.run(  # noqa: S603
        [REPO / "scripts/ipod/make-disk", f"--firmware={firmware}", f"--disks={disk}"],
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run([QEMU_IMG, "check", disk], check=True, capture_output=True)  # noqa: S603
    raw = disk.with_suffix(".raw")
    subprocess.run([QEMU_IMG, "convert", "-O", "raw", disk, raw], check=True)  # noqa: S603
    fw_blocks = -(-len(payload) // BS)
    fat = (FW_BLOCK + fw_blocks) * BS
    with raw.open("rb") as f:
        mbr = f.read(512)
        parts = [struct.unpack_from("<4xB3xII", mbr, 0x1BE + 16 * i) for i in range(2)]
        assert parts == [(0x00, FW_BLOCK, fw_blocks), (0x0B, FW_BLOCK + fw_blocks, FAT_BYTES // BS)]
        assert mbr[0x1FE:] == b"\x55\xaa"
        f.seek(FW_BLOCK * BS)
        assert f.read(fw_blocks * BS) == payload + b"\0" * (fw_blocks * BS - len(payload))
        boot = f.read(512)
        assert boot[0x34:0x37] == b"FAT"
        (backup,) = struct.unpack_from("<H", boot, 0x32)
        assert backup
        f.seek(fat + backup * 512 + 0x34)
        assert f.read(3) == b"FAT"
    return run.stdout


@e2e
def test_build_synthetic(tmp_path: Path) -> None:
    """A synthetic .ipsw outside the repo builds a disk outside the repo."""
    payload = fake_payload()
    out = build_and_check(make_ipsw(tmp_path / "a.ipsw", payload), tmp_path / "d.qcow2", payload)
    assert "from firmware 2.0.5" in out


@pytest.mark.usefixtures("no_env")
def test_track_flag(tmp_path: Path) -> None:
    """--track=portblue names the shipped fixture; a missing file fails before any container starts."""
    firmware = make_ipsw(tmp_path / "a.ipsw", fake_payload())
    base = [f"--firmware={firmware}", f"--disks={tmp_path / 'd.qcow2'}"]
    assert prepare([*base, "--track=portblue"])[4] == PORT_BLUE  # type: ignore[index]
    assert prepare(base)[4] is None  # type: ignore[index]
    with pytest.raises(SystemExit, match="no such track"):
        prepare([*base, f"--track={tmp_path / 'absent.m4a'}"])


def test_unknown_container(tmp_path: Path) -> None:
    """A track whose extension has no iTunesDB container code is refused before anything is copied."""
    with pytest.raises(SystemExit, match="no container code"):
        populate(tmp_path / "fat", tmp_path / "song.flac")


@e2e
def test_build_track(tmp_path: Path) -> None:
    """--track puts the song and a libgpod-signed database on the volume, under the track's name."""
    payload = fake_payload()
    disk = tmp_path / "d.qcow2"
    firmware = make_ipsw(tmp_path / "a.ipsw", payload)
    out = subprocess.run(  # noqa: S603
        [REPO / "scripts/ipod/make-disk", f"--firmware={firmware}", f"--disks={disk}", "--track=portblue"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    assert "with track portblue.m4a" in out
    raw = tmp_path / "d.raw"
    subprocess.run([QEMU_IMG, "convert", "-O", "raw", disk, raw], check=True)  # noqa: S603
    fat = (FW_BLOCK + -(-len(payload) // BS)) * BS
    mtools = ["docker", "run", "--rm", "--user", f"{os.getuid()}:{os.getgid()}", "-v", f"{tmp_path}:{tmp_path}"]
    mtools += ["-e", "MTOOLS_SKIP_CHECK=1", "qemu-ipod-dev", "mtype", "-i", f"{raw}@@{fat}"]
    song = subprocess.run([*mtools, "::iPod_Control/Music/F00/portblue.m4a"], check=True, capture_output=True)  # noqa: S603
    assert song.stdout == PORT_BLUE.read_bytes()
    db = subprocess.run([*mtools, "::iPod_Control/iTunes/iTunesDB"], check=True, capture_output=True).stdout  # noqa: S603
    assert db[:4] == b"mhbd"
    assert "portblue".encode("utf-16-le") in db
    assert b"Music:F00:portblue.m4a" in db or "Music:F00:portblue.m4a".encode("utf-16-le") in db


REAL_IPSW = Path(os.environ.get("IPOD_TEST_IPSW", REPO / "assets/test/iPod_38.2.0.5.ipsw"))


@e2e
@pytest.mark.skipif(not REAL_IPSW.is_file(), reason=f"no Apple .ipsw at {REAL_IPSW}")
def test_build_real_ipsw(tmp_path: Path) -> None:
    """A real .ipsw: the manifest version matches Apple's file name; its payload is partition 1."""
    m = re.fullmatch(r"iPod_\d+\.(\d+\.\d+\.\d+)\.ipsw", REAL_IPSW.name)
    assert m, f"{REAL_IPSW.name} is not Apple's iPod_<family>.<version>.ipsw"
    payload, version = read_firmware(REAL_IPSW)
    assert version == m[1]
    assert f"from firmware {version}" in build_and_check(REAL_IPSW, tmp_path / "d.qcow2", payload)

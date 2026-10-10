# /// script
# requires-python = ">=3.12"
# dependencies = ["pytest"]
# ///
"""Tests for fwtool.py's CLI. Run: mise run check:tests.

The osos tests need an Apple .ipsw at $IPOD_TEST_IPSW, default assets/test/iPod_38.2.0.5.ipsw
(gitignored), and skips without it.
"""

import os
from pathlib import Path

import pytest
from fwtool import IMG_HDR_SIZE, main, read_directory, read_firmware

REPO = Path(__file__).resolve().parents[3]
REAL_IPSW = Path(os.environ.get("IPOD_TEST_IPSW", REPO / "assets/test/iPod_38.2.0.5.ipsw"))


@pytest.mark.parametrize(
    "argv",
    [
        [],
        ["bogus"],
        ["extract"],
        ["extract", "--firmware=x"],
        ["list", "--osos=x"],
        ["list", "--firmware="],
    ],
)
def test_usage_errors(argv: list[str], monkeypatch: pytest.MonkeyPatch) -> None:
    """Unknown commands, missing or empty flags and another command's flags exit with the usage."""
    monkeypatch.delenv("FIRMWARE", raising=False)
    monkeypatch.delenv("OUTDIR", raising=False)
    with pytest.raises(SystemExit) as e:
        main(argv)
    assert "usage" in str(e.value)


real = pytest.mark.skipif(not REAL_IPSW.is_file(), reason=f"no Apple .ipsw at {REAL_IPSW}")


@real
@pytest.mark.parametrize("form", ["--{}={}", "{}={}"])
def test_osos_real_ipsw(form: str, tmp_path: Path) -> None:
    """`osos` writes the header-first, block-padded slice the device decrypts (both flag forms)."""
    out = tmp_path / "osos.enc"
    main(["osos", form.format("firmware", REAL_IPSW), form.format("osos-encrypted", out)])
    data, _ = read_firmware(REAL_IPSW)
    entry = next(e for e in read_directory(data)[2] if e.type == "osos")
    blob = out.read_bytes()
    assert len(blob) == (entry.length + IMG_HDR_SIZE + 15) & ~15
    assert blob[IMG_HDR_SIZE : IMG_HDR_SIZE + 16] == data[entry.dev_offset : entry.dev_offset + 16]

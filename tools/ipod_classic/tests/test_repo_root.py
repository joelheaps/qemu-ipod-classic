"""The repo root is computed from file locations in two places; both must find the same checkout."""

from pathlib import Path

import cli
import qemu_session


def test_repo_root_is_the_checkout() -> None:
    """`cli.REPO` and `qemu_session.ROOT` must both be the directory that holds mise.toml."""
    assert cli.REPO == qemu_session.ROOT
    assert (cli.REPO / "mise.toml").is_file()
    assert Path(cli.__file__).resolve().is_relative_to(cli.REPO / "tools/ipod_classic/lib")

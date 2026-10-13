"""Exercise task boundaries without Docker, package installs, compilation or network access."""

import json
import os
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
pytestmark = pytest.mark.usefixtures("task_env")


def executable(path: Path, body: str) -> None:
    """Create a recording fake executable."""
    path.write_text("#!/bin/sh\n" + body)
    path.chmod(0o755)


@pytest.fixture
def task_env(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    """Shadow platform/package commands; never touch the host package manager."""
    binaries = tmp_path / "bin"
    binaries.mkdir()
    executable(binaries / "uname", "echo Linux\n")
    executable(binaries / "id", "echo 0\n")
    for tool in ("apt-get", "dnf", "make", "ninja", "timeout", "docker"):
        executable(binaries / tool, f'echo "{tool} $*" >> "$COMMAND_LOG"\n')
    monkeypatch.setenv("PATH", f"{binaries}:{os.environ['PATH']}")
    monkeypatch.setenv("COMMAND_LOG", str(tmp_path / "commands"))
    monkeypatch.setenv("MISE_PROJECT_ROOT", str(REPO))
    monkeypatch.delenv("FIRMWARE", raising=False)
    monkeypatch.delenv("DISK", raising=False)
    monkeypatch.delenv("OSOS", raising=False)
    return binaries


def invoke(script: str, args: list[str] | None = None) -> subprocess.CompletedProcess[str]:
    """Run a script against the recording commands."""
    return subprocess.run(  # noqa: S603
        [str(REPO / "scripts" / script), *(args or [])],
        cwd=REPO,
        capture_output=True,
        text=True,
        check=False,
    )


@pytest.mark.parametrize("distribution", ["ubuntu", "fedora"])
def test_setup_dispatch(distribution: str, tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """Native setup uses upstream build-dep/builddep plus media development packages."""
    release = tmp_path / "os-release"
    release.write_text(f"ID={distribution}\n")
    monkeypatch.setenv("OS_RELEASE", str(release))
    result = invoke("setup")
    assert result.returncode == 0, result.stderr
    commands = (tmp_path / "commands").read_text()
    assert (
        "apt-get build-dep --arch-only -y qemu"
        if distribution == "ubuntu"
        else "dnf builddep -y qemu"
    ) in commands
    assert ("libavcodec-dev" if distribution == "ubuntu" else "ffmpeg-devel") in commands
    assert "docker" not in commands


def test_runtime_setup_omits_build_dependencies(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The dev image installs runtime tools without compilers or build-dep."""
    release = tmp_path / "os-release"
    release.write_text("ID=ubuntu\n")
    monkeypatch.setenv("OS_RELEASE", str(release))
    assert invoke("setup", ["--runtime"]).returncode == 0
    commands = (tmp_path / "commands").read_text()
    assert "dosfstools" in commands
    assert "build-dep" not in commands
    assert "build-essential" not in commands


def test_native_build_never_installs_packages(
    task_env: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Incremental native builds use the selected directory and shared QEMU targets."""
    for tool in ("cc", "pkg-config"):
        executable(task_env / tool, "exit 0\n")
    executable(task_env / "uv", 'echo "uv $*" >> "$COMMAND_LOG"\n')
    build = tmp_path / "native build"
    build.mkdir()
    (build / "build.ninja").touch()
    monkeypatch.setenv("BUILD_DIR", str(build))
    monkeypatch.setenv("JOBS", "2")
    result = invoke("build")
    assert result.returncode == 0, result.stderr
    commands = (tmp_path / "commands").read_text()
    assert "uv venv --allow-existing --managed-python --python 3.14.8 bootstrap" in commands
    assert "uv pip install --python bootstrap/bin/python distlib==0.4.0" in commands
    assert "make -j 2 qemu-system-arm qemu-img" in commands
    assert (
        "ninja contrib/plugins/libpctrace.so contrib/plugins/libprovtrace.so"
        in commands
    )
    assert "apt-get" not in commands
    assert "dnf" not in commands


def test_docker_build_delegates_and_preserves_arguments(task_env: Path, tmp_path: Path) -> None:
    """The wrapper only invokes the shared build task, never host setup."""
    executable(
        task_env / "docker",
        """exec python3 -c 'import json, os, sys
open(os.environ["COMMAND_LOG"], "w").write(json.dumps(sys.argv[1:]))' "$@"
""",
    )
    result = invoke("docker/build", ["--extra-cflags=-DNAME=two words"])
    assert result.returncode == 0, result.stderr
    args = json.loads((tmp_path / "commands").read_text())
    assert args[-5:] == ["mise", "run", "build", "--", "--extra-cflags=-DNAME=two words"]
    assert "BUILD_DIR=build/docker" in args
    assert "setup" not in args


def test_native_headless_command(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """Selected binaries, qcow2 format, snapshot and timeout reach QEMU intact."""
    build = tmp_path / "build"
    build.mkdir()
    executable(build / "qemu-system-arm", "exit 0\n")
    firmware = tmp_path / "decrypted firmware.bin"
    firmware.touch()
    disk = tmp_path / "guest disk.qcow2"
    disk.touch()
    monkeypatch.setenv("BUILD_DIR", str(build))
    monkeypatch.setenv("SCRATCH", str(tmp_path / "scratch"))
    result = invoke(
        "ipod/run", ["--headless", "--timeout=30", f"--osos={firmware}", f"--disks={disk}"]
    )
    assert result.returncode == 0, result.stderr
    commands = (tmp_path / "commands").read_text()
    assert f"timeout 30 {build}/qemu-system-arm" in commands
    assert f"if=ide,format=qcow2,file={disk}" in commands
    assert "-snapshot" in commands
    assert "-display none" in commands


def test_docker_missing_display_preserves_live_container(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A failed window preflight must not remove an existing container."""
    firmware, disk = tmp_path / "osos", tmp_path / "disk"
    firmware.touch()
    disk.touch()
    monkeypatch.setenv("DISPLAY", "")
    monkeypatch.setenv("XAUTHORITY_DIR", str(tmp_path))
    monkeypatch.delenv("XAUTHORITY", raising=False)
    result = invoke("docker/ipod/run", [f"--osos={firmware}", f"--disks={disk}"])
    assert result.returncode != 0
    assert "existing container untouched" in result.stderr
    assert not (tmp_path / "commands").exists()


def test_docker_headless_mounts_external_paths(task_env: Path, tmp_path: Path) -> None:
    """Headless launch delegates to ipod:run and safely mounts paths containing spaces."""
    executable(
        task_env / "docker",
        """exec python3 -c 'import json, os, sys
open(os.environ["COMMAND_LOG"], "w").write(json.dumps(sys.argv[1:]))' "$@"
""",
    )
    firmware, disk = tmp_path / "osos with spaces", tmp_path / "disk.qcow2"
    firmware.touch()
    disk.touch()
    result = invoke("docker/ipod/run", ["--headless", f"--osos={firmware}", f"--disks={disk}"])
    assert result.returncode == 0, result.stderr
    args = json.loads((tmp_path / "commands").read_text())
    assert f"{tmp_path}:{tmp_path}" in args
    assert args[-7:] == [
        "mise",
        "run",
        "ipod:run",
        "--",
        "--headless",
        f"--osos={firmware}",
        f"--disks={disk}",
    ]
    assert "-d" not in args


@pytest.mark.parametrize(("status", "expected"), [(124, 0), (7, 7)])
def test_headless_exit_status(
    status: int, expected: int, task_env: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A timeout is a successful bounded boot; other QEMU failures propagate."""
    executable(task_env / "timeout", f"exit {status}\n")
    build = tmp_path / "build"
    build.mkdir()
    executable(build / "qemu-system-arm", "exit 0\n")
    firmware, disk = tmp_path / "osos", tmp_path / "disk"
    firmware.touch()
    disk.touch()
    monkeypatch.setenv("BUILD_DIR", str(build))
    monkeypatch.setenv("SCRATCH", str(tmp_path / "scratch"))
    result = invoke("ipod/run", ["--headless", f"--osos={firmware}", f"--disks={disk}"])
    assert result.returncode == expected

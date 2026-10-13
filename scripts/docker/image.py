"""Build an image only when its Dockerfile or copied mise/task inputs change."""

import hashlib
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
kind = sys.argv[1]
image = f"qemu-ipod-{kind}"
dockerfile = root / f"docker/Dockerfile.{kind}"
files = [dockerfile, root / "mise.toml", root / ".dockerignore", root / "scripts/setup"]
digest = hashlib.sha256()
for path in files:
    digest.update(str(path.relative_to(root)).encode())
    digest.update(path.read_bytes())
    digest.update(str(path.stat().st_mode).encode())
hash_value = digest.hexdigest()
current = subprocess.run(
    ["docker", "image", "inspect", "-f", '{{index .Config.Labels "mise.inputs"}}', image],
    capture_output=True,
    text=True,
    check=False,
)
if current.returncode == 0 and current.stdout.strip() == hash_value:
    print(f"{image} is up to date")
else:
    raise SystemExit(
        subprocess.call(
            [
                "docker",
                "build",
                "--label",
                f"mise.inputs={hash_value}",
                "-t",
                image,
                "-f",
                str(dockerfile),
                str(root),
            ]
        )
    )

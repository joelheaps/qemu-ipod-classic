"""Flag handling shared by the ipod scripts, the same contract as scripts/ipod/run.

A flag is `--name=value` or `name=value`; an environment variable gives its default. Relative
paths are taken from the repo root, as `mise run` does.
"""

import os
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]


def parse_flags(argv: list[str], flags: dict[str, str], usage: str) -> dict[str, str]:
    """Return {name: value} for FLAGS ({name: ENV_VAR}); USAGE on an unknown or empty flag."""
    opts = {name: os.environ.get(env, "") for name, env in flags.items()}
    for arg in argv:
        name, _, value = arg.removeprefix("--").partition("=")
        if name not in flags or not value:
            raise SystemExit(usage)
        opts[name] = value
    return opts


def repo_path(path: str) -> Path:
    """Resolve PATH against the repo root."""
    return (REPO / path).resolve()

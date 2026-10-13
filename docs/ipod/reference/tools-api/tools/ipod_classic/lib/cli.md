# Cli

[tools Index](../../../README.md#tools-index) / [Tools](../../index.md#tools) / [Ipod Classic](../index.md#ipod-classic) / [Lib](./index.md#lib) / Cli

> Auto-generated documentation for [tools.ipod_classic.lib.cli](../../../../../../../tools/ipod_classic/lib/cli.py) module.

```text
Flag handling shared by the ipod scripts, the same contract as scripts/ipod/run.

A flag is `--name=value` or `name=value`; an environment variable gives its default. Relative
paths are taken from the repo root, as `mise run` does.
```

- [Cli](#cli)
  - [parse_flags](#parse_flags)
  - [repo_path](#repo_path)

## parse_flags

[Show source in cli.py:13](../../../../../../../tools/ipod_classic/lib/cli.py#L13)

Return {name: value} for FLAGS ({name: ENV_VAR}); USAGE on an unknown or empty flag.

#### Signature

```python
def parse_flags(
    argv: list[str], flags: dict[str, str], usage: str
) -> dict[str, str]: ...
```

## repo_path

[Show source in cli.py:24](../../../../../../../tools/ipod_classic/lib/cli.py#L24)

Resolve PATH against the repo root.

#### Signature

```python
def repo_path(path: str) -> Path: ...
```

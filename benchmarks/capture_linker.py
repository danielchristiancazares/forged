#!/usr/bin/env python3

from __future__ import annotations

import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import time


def sanitize_filename(value: str) -> str:
    value = re.sub(r"[^A-Za-z0-9._-]+", "-", value).strip("-")
    return value or "unknown"


def parse_output_path(args: list[str]) -> str | None:
    for idx, arg in enumerate(args[:-1]):
        if arg == "-o":
            return args[idx + 1]
    return None


def is_ld_command(path: str) -> bool:
    return pathlib.Path(path).name.startswith("ld")


def derive_ld_argv(real_linker: str, args: list[str], cwd: str) -> list[str] | None:
    completed = subprocess.run(
        [real_linker, "-###", *args],
        cwd=cwd,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )

    candidates: list[list[str]] = []
    for line in completed.stdout.splitlines():
        stripped = line.strip()
        if not stripped.startswith('"'):
            continue
        argv = shlex.split(stripped)
        if is_ld_command(argv[0]):
            candidates.append(argv)

    if not candidates:
        return None
    return candidates[-1]


def should_copy_input(path: pathlib.Path) -> bool:
    return path.is_absolute() and path.is_file() and (
        path.suffix == ".o" or any(part.startswith("rustc") for part in path.parts)
    )


def copy_replay_inputs(
    args: list[str],
    capture_dir: pathlib.Path,
    copied: dict[str, str] | None = None,
) -> dict[str, str]:
    inputs_dir = capture_dir / "inputs"
    inputs_dir.mkdir(parents=True, exist_ok=True)

    copied = {} if copied is None else dict(copied)
    for arg in args:
        path = pathlib.Path(arg)
        if not should_copy_input(path) or str(path) in copied:
            continue

        stem = sanitize_filename("-".join(path.parts[-2:]))
        destination = inputs_dir / stem
        suffix = 1
        while destination.exists():
            destination = inputs_dir / f"{stem}-{suffix}"
            suffix += 1
        shutil.copy2(path, destination)
        copied[str(path)] = str(destination)

    return copied


def rewrite_argv_inputs(argv: list[str] | None, copied_inputs: dict[str, str]) -> list[str] | None:
    if argv is None:
        return None
    return [copied_inputs.get(arg, arg) for arg in argv]


def main() -> int:
    capture_dir = os.environ.get("SOLD_CAPTURE_DIR")
    if not capture_dir:
        print("capture_linker.py: SOLD_CAPTURE_DIR is not set", file=sys.stderr)
        return 2

    real_linker = os.environ.get("SOLD_REAL_LINKER", "clang")
    capture_path = pathlib.Path(capture_dir)
    capture_path.mkdir(parents=True, exist_ok=True)

    started_at = time.time()
    output_path = parse_output_path(sys.argv[1:])
    output_name = sanitize_filename(pathlib.Path(output_path).name if output_path else "unknown")
    record_path = capture_path / f"{int(started_at * 1000)}-{os.getpid()}-{output_name}.json"
    copied_inputs = copy_replay_inputs(sys.argv[1:], capture_path)
    derived_ld_argv = derive_ld_argv(real_linker, sys.argv[1:], os.getcwd())
    if derived_ld_argv is not None:
        copied_inputs = copy_replay_inputs(derived_ld_argv, capture_path, copied_inputs)
    derived_ld_argv = rewrite_argv_inputs(derived_ld_argv, copied_inputs)

    try:
        completed = subprocess.run([real_linker, *sys.argv[1:]], check=False)
        exit_status = completed.returncode
    except FileNotFoundError:
        print(f"capture_linker.py: real linker not found: {real_linker}", file=sys.stderr)
        exit_status = 127

    finished_at = time.time()
    record = {
        "argv0": sys.argv[0],
        "cwd": os.getcwd(),
        "driver": real_linker,
        "driver_argv": sys.argv[1:],
        "derived_ld_argv": derived_ld_argv,
        "duration_s": finished_at - started_at,
        "exit_status": exit_status,
        "finished_at": finished_at,
        "input_copies": copied_inputs,
        "output_path": output_path,
        "pid": os.getpid(),
        "started_at": started_at,
        "target_env": {
            key: os.environ[key]
            for key in sorted(os.environ)
            if key.startswith("CARGO_") or key.startswith("RUST")
        },
    }

    temp_path = record_path.with_suffix(".tmp")
    temp_path.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    os.replace(temp_path, record_path)
    return exit_status


if __name__ == "__main__":
    raise SystemExit(main())

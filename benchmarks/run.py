#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import pathlib
import platform
import shlex
import statistics
import subprocess
import sys
import time
import tomllib
from dataclasses import dataclass
from datetime import datetime, timezone


SIDE_OUTPUT_FLAGS = {
    "-dependency_info": "dependency-info",
    "-map": "map",
    "-object_path_lto": "object-path-lto",
    "-o": "binary",
}

SMOKE_TIMEOUT_S = 30
OUTPUT_WAIT_TIMEOUT_S = 5


@dataclass
class TargetConfig:
    name: str
    project_root: pathlib.Path
    cargo_args: list[str]
    binary_relpath: pathlib.Path
    smoke_cmd: list[str]


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def run_checked(cmd: list[str], **kwargs) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, check=True, text=True, **kwargs)


def load_targets(path: pathlib.Path) -> list[TargetConfig]:
    with path.open("rb") as fp:
        data = tomllib.load(fp)

    entries = data.get("target", [])
    targets: list[TargetConfig] = []

    for idx, entry in enumerate(entries):
        name = entry.get("name")
        project_root = pathlib.Path(entry["project_root"]).expanduser()
        cargo_args = list(entry["cargo_args"])
        binary_relpath = pathlib.Path(entry["binary_relpath"])
        smoke_cmd = list(entry["smoke_cmd"])

        if not project_root.is_absolute():
            raise ValueError(f"target[{idx}]: project_root must be absolute")
        if not cargo_args:
            raise ValueError(f"target[{idx}]: cargo_args must not be empty")
        if "{binary}" not in smoke_cmd:
            raise ValueError(f"target[{idx}]: smoke_cmd must contain {{binary}}")

        if not name:
            name = binary_relpath.name

        targets.append(
            TargetConfig(
                name=name,
                project_root=project_root,
                cargo_args=cargo_args,
                binary_relpath=binary_relpath,
                smoke_cmd=smoke_cmd,
            )
        )

    return targets


def resolve_cargo_target_dir(out_dir: pathlib.Path, target: TargetConfig) -> pathlib.Path:
    return out_dir / "targets" / target.name / "cargo-target"


def resolve_expected_output(target: TargetConfig, cargo_target_dir: pathlib.Path) -> pathlib.Path:
    parts = target.binary_relpath.parts
    if parts and parts[0] == "target":
        return cargo_target_dir.joinpath(*parts[1:]).resolve()
    return (target.project_root / target.binary_relpath).resolve()


def resolve_output_path(record: dict[str, object]) -> pathlib.Path | None:
    output_path = record.get("output_path")
    cwd = record.get("cwd")
    if not output_path or not cwd:
        return None

    path = pathlib.Path(str(output_path))
    if not path.is_absolute():
        path = pathlib.Path(str(cwd)) / path
    return path.resolve()


def is_ld_command(path: str) -> bool:
    return pathlib.Path(path).name.startswith("ld")


def normalize_bin_name(value: str) -> str:
    return value.replace("-", "_")


def choose_capture(
    target: TargetConfig,
    capture_dir: pathlib.Path,
    cargo_target_dir: pathlib.Path,
) -> dict[str, object]:
    expected_output = resolve_expected_output(target, cargo_target_dir)
    records = []

    for path in sorted(capture_dir.glob("*.json")):
        record = json.loads(path.read_text())
        record["_path"] = str(path)
        records.append(record)

    def matches_target(record: dict[str, object]) -> bool:
        resolved = resolve_output_path(record)
        if resolved is None:
            return False
        if resolved == expected_output:
            return True

        target_env = record.get("target_env", {})
        cargo_bin_name = None
        if isinstance(target_env, dict):
            cargo_bin_name = target_env.get("CARGO_BIN_NAME")

        expected_name = expected_output.name
        normalized_expected = normalize_bin_name(expected_name)
        normalized_target = normalize_bin_name(target.name)
        normalized_cargo_bin = normalize_bin_name(str(cargo_bin_name)) if cargo_bin_name else None
        if normalized_cargo_bin not in {normalized_expected, normalized_target}:
            return False

        resolved_name = normalize_bin_name(resolved.name)
        if resolved_name == normalized_expected or resolved_name.startswith(f"{normalized_expected}_"):
            if resolved.parent.name == "deps" and resolved.parent.parent == expected_output.parent:
                return True

        return False

    matches = [
        record
        for record in records
        if matches_target(record) and record.get("exit_status") == 0
    ]

    if not matches:
        raise RuntimeError(
            f"no successful capture matched {expected_output} in {capture_dir}"
        )

    return max(matches, key=lambda record: float(record["finished_at"]))


def derive_ld_command(record: dict[str, object], apple_ld: pathlib.Path) -> list[str]:
    input_copies = record.get("input_copies", {})

    def rewrite_inputs(argv: list[str]) -> list[str]:
        if not isinstance(input_copies, dict):
            return argv
        return [str(input_copies.get(str(arg), str(arg))) for arg in argv]

    prederived = record.get("derived_ld_argv")
    if isinstance(prederived, list) and prederived:
        argv = rewrite_inputs([str(arg) for arg in prederived])
        argv[0] = str(apple_ld)
        return argv

    driver = str(record["driver"])
    driver_argv = [str(arg) for arg in record["driver_argv"]]
    cwd = str(record["cwd"])

    completed = subprocess.run(
        [driver, "-###", *driver_argv],
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
        raise RuntimeError(f"failed to parse ld command from clang -### output:\n{completed.stdout}")

    argv = rewrite_inputs(candidates[-1])
    argv[0] = str(apple_ld)
    return argv


def make_ld64_entrypoint(tool_path: pathlib.Path, out_dir: pathlib.Path) -> pathlib.Path:
    if tool_path.name.startswith("ld64"):
        return tool_path

    bin_dir = out_dir / "bin"
    bin_dir.mkdir(parents=True, exist_ok=True)
    entrypoint = bin_dir / "ld64.sold"
    if entrypoint.exists() or entrypoint.is_symlink():
        entrypoint.unlink()
    entrypoint.symlink_to(tool_path)
    return entrypoint


def rewrite_output_paths(argv: list[str], out_dir: pathlib.Path, linker_name: str) -> tuple[list[str], pathlib.Path]:
    rewritten: list[str] = []
    binary_path: pathlib.Path | None = None
    linker_out_dir = out_dir / linker_name
    linker_out_dir.mkdir(parents=True, exist_ok=True)

    idx = 0
    while idx < len(argv):
        arg = argv[idx]
        rewritten.append(arg)

        if arg in SIDE_OUTPUT_FLAGS and idx + 1 < len(argv):
            original = pathlib.Path(argv[idx + 1])
            category = SIDE_OUTPUT_FLAGS[arg]
            category_dir = linker_out_dir / category
            category_dir.mkdir(parents=True, exist_ok=True)
            new_path = category_dir / original.name
            rewritten.append(str(new_path))
            if arg == "-o":
                binary_path = new_path
            idx += 2
            continue

        idx += 1

    if binary_path is None:
        raise RuntimeError("ld command did not contain -o")
    return rewritten, binary_path


def parse_time_output(stderr: str) -> float:
    for line in stderr.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0] == "real":
            return float(parts[1])
    raise RuntimeError(f"failed to parse /usr/bin/time -lp output:\n{stderr}")


def timed_run(cmd: list[str], cwd: pathlib.Path) -> tuple[subprocess.CompletedProcess[str], float]:
    completed = subprocess.run(
        ["/usr/bin/time", "-lp", *cmd],
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return completed, parse_time_output(completed.stderr)


def wait_for_output_file(path: pathlib.Path) -> None:
    deadline = time.monotonic() + OUTPUT_WAIT_TIMEOUT_S
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(0.05)
    raise RuntimeError(f"output file was not created: {path}")


def verify_binary(binary_path: pathlib.Path) -> dict[str, object]:
    file_proc = run_checked(["file", str(binary_path)], stdout=subprocess.PIPE)
    lipo_proc = run_checked(["lipo", "-archs", str(binary_path)], stdout=subprocess.PIPE)
    otool_proc = subprocess.run(
        ["otool", "-l", str(binary_path)],
        text=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )

    archs = lipo_proc.stdout.strip().split()
    return {
        "file_output": file_proc.stdout.strip(),
        "lipo_archs": archs,
        "mach_o_pass": "Mach-O 64-bit executable" in file_proc.stdout and "x86_64" in archs,
        "otool_exit_status": otool_proc.returncode,
        "otool_pass": otool_proc.returncode == 0,
    }


def run_smoke(target: TargetConfig, binary_path: pathlib.Path) -> tuple[bool, int]:
    argv = [str(binary_path) if arg == "{binary}" else arg for arg in target.smoke_cmd]
    try:
        completed = subprocess.run(
            argv,
            cwd=target.project_root,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=SMOKE_TIMEOUT_S,
        )
        return completed.returncode == 0, completed.returncode
    except subprocess.TimeoutExpired:
        return False, 124


def run_cargo_capture(
    target: TargetConfig,
    capture_tool: pathlib.Path,
    capture_dir: pathlib.Path,
    cargo_target_dir: pathlib.Path,
    real_linker: str,
    cargo_clean: bool,
) -> dict[str, object]:
    env = os.environ.copy()
    env["CARGO_TARGET_X86_64_APPLE_DARWIN_LINKER"] = str(capture_tool)
    env["CARGO_TARGET_DIR"] = str(cargo_target_dir)
    env["SOLD_CAPTURE_DIR"] = str(capture_dir)
    env["SOLD_REAL_LINKER"] = real_linker

    cargo_target_dir.mkdir(parents=True, exist_ok=True)

    if cargo_clean:
        run_checked(["cargo", "clean"], cwd=target.project_root, env=env, stdout=subprocess.PIPE)

    completed = subprocess.run(["cargo", *target.cargo_args], cwd=target.project_root, env=env)
    if completed.returncode != 0:
        raise RuntimeError(f"cargo capture failed for target {target.name}")

    return choose_capture(target, capture_dir, cargo_target_dir)


def benchmark_linker(
    name: str,
    argv: list[str],
    replay_cwd: pathlib.Path,
    target: TargetConfig,
    target_out_dir: pathlib.Path,
    warmups: int,
    runs: int,
) -> dict[str, object]:
    rewritten_argv, binary_path = rewrite_output_paths(argv, target_out_dir, name)
    log_dir = target_out_dir / name / "logs"
    log_dir.mkdir(parents=True, exist_ok=True)

    warmup_times: list[float] = []
    run_times: list[float] = []
    exit_status = 0

    for phase, iterations in (("warmup", warmups), ("run", runs)):
        for iteration in range(iterations):
            if binary_path.exists():
                binary_path.unlink()

            completed, real_s = timed_run(rewritten_argv, replay_cwd)
            log_prefix = log_dir / f"{phase}-{iteration + 1:02d}"
            (log_prefix.with_suffix(".stdout.log")).write_text(completed.stdout)
            (log_prefix.with_suffix(".stderr.log")).write_text(completed.stderr)

            exit_status = completed.returncode
            if exit_status != 0:
                return {
                    "binary_path": str(binary_path),
                    "binary_size_bytes": None,
                    "exit_status": exit_status,
                    "linker": name,
                    "median_wall_s": None,
                    "smoke_exit_status": None,
                    "smoke_pass": False,
                    "target": target.name,
                    "timings_s": run_times,
                    "warmup_timings_s": warmup_times,
                }

            if phase == "warmup":
                warmup_times.append(real_s)
            else:
                run_times.append(real_s)

    wait_for_output_file(binary_path)
    binary_checks = verify_binary(binary_path)
    smoke_pass, smoke_exit_status = run_smoke(target, binary_path)

    return {
        "binary_checks": binary_checks,
        "binary_path": str(binary_path),
        "binary_size_bytes": binary_path.stat().st_size,
        "exit_status": exit_status,
        "linker": name,
        "median_wall_s": statistics.median(run_times),
        "smoke_exit_status": smoke_exit_status,
        "smoke_pass": smoke_pass,
        "target": target.name,
        "timings_s": run_times,
        "warmup_timings_s": warmup_times,
    }


def ensure_supported_host() -> None:
    if platform.system() != "Darwin":
        raise RuntimeError("this benchmark harness only supports macOS")
    if platform.machine() != "x86_64":
        raise RuntimeError("this MVP benchmark harness only supports x86_64 macOS hosts")


def main() -> int:
    parser = argparse.ArgumentParser(description="Benchmark sold against Apple ld on captured Rust link commands.")
    parser.add_argument("--corpus", default="benchmarks/corpus.toml", help="Path to the corpus manifest")
    parser.add_argument("--out-dir", default="benchmarks/out", help="Directory for captures, logs, and results")
    parser.add_argument("--apple-ld", help="Path to Apple ld; defaults to xcrun -f ld")
    parser.add_argument("--real-linker", default="clang", help="Real linker driver used during capture")
    parser.add_argument("--sold-bin", required=True, help="Path to sold binary or ld64 symlink")
    parser.add_argument(
        "--target",
        dest="target_names",
        action="append",
        default=[],
        help="Limit the run to one or more target names from the corpus",
    )
    parser.add_argument("--warmups", type=int, default=1, help="Number of warmup runs per linker")
    parser.add_argument("--runs", type=int, default=10, help="Number of measured runs per linker")
    parser.add_argument("--min-win-pct", type=float, default=10.0, help="Required sold median win percentage")
    parser.add_argument(
        "--cargo-clean",
        dest="cargo_clean",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Whether to run cargo clean before capture",
    )
    args = parser.parse_args()

    ensure_supported_host()

    corpus_path = pathlib.Path(args.corpus).resolve()
    out_dir = pathlib.Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    apple_ld = pathlib.Path(args.apple_ld).resolve() if args.apple_ld else pathlib.Path(
        run_checked(["xcrun", "-f", "ld"], stdout=subprocess.PIPE).stdout.strip()
    )
    sold_bin = pathlib.Path(args.sold_bin).resolve()
    sold_ld64 = make_ld64_entrypoint(sold_bin, out_dir)
    capture_tool = pathlib.Path(__file__).with_name("capture_linker.py").resolve()

    targets = load_targets(corpus_path)
    if args.target_names:
        selected = set(args.target_names)
        targets = [target for target in targets if target.name in selected]
    if not targets:
        raise RuntimeError(f"no [[target]] entries found in {corpus_path}")

    results: list[dict[str, object]] = []
    comparisons: list[dict[str, object]] = []

    for target in targets:
        target_dir = out_dir / "targets" / target.name
        capture_dir = target_dir / "captures"
        cargo_target_dir = resolve_cargo_target_dir(out_dir, target)
        capture_dir.mkdir(parents=True, exist_ok=True)

        capture_record = run_cargo_capture(
            target=target,
            capture_tool=capture_tool,
            capture_dir=capture_dir,
            cargo_target_dir=cargo_target_dir,
            real_linker=args.real_linker,
            cargo_clean=args.cargo_clean,
        )

        driver_argv = [str(arg) for arg in capture_record["driver_argv"]]
        if any(arg.startswith("-flto") for arg in driver_argv):
            raise RuntimeError(f"target {target.name} used LTO, but the MVP benchmark excludes LTO")

        apple_argv = derive_ld_command(capture_record, apple_ld)
        sold_argv = list(apple_argv)
        sold_argv[0] = str(sold_ld64)

        (target_dir / "capture.json").write_text(json.dumps(capture_record, indent=2, sort_keys=True) + "\n")
        (target_dir / "apple-ld-command.json").write_text(json.dumps(apple_argv, indent=2) + "\n")
        (target_dir / "sold-ld-command.json").write_text(json.dumps(sold_argv, indent=2) + "\n")

        replay_cwd = pathlib.Path(str(capture_record["cwd"]))
        apple_result = benchmark_linker(
            name="apple_ld",
            argv=apple_argv,
            replay_cwd=replay_cwd,
            target=target,
            target_out_dir=target_dir,
            warmups=args.warmups,
            runs=args.runs,
        )
        sold_result = benchmark_linker(
            name="sold",
            argv=sold_argv,
            replay_cwd=replay_cwd,
            target=target,
            target_out_dir=target_dir,
            warmups=args.warmups,
            runs=args.runs,
        )

        results.extend([apple_result, sold_result])

        win_pct = None
        if apple_result["median_wall_s"] and sold_result["median_wall_s"]:
            win_pct = (float(apple_result["median_wall_s"]) - float(sold_result["median_wall_s"])) / float(
                apple_result["median_wall_s"]
            ) * 100.0

        comparisons.append(
            {
                "target": target.name,
                "apple_median_wall_s": apple_result["median_wall_s"],
                "sold_median_wall_s": sold_result["median_wall_s"],
                "sold_win_pct": win_pct,
                "threshold_pct": args.min_win_pct,
            }
        )

    summary = {
        "apple_ld": str(apple_ld),
        "corpus": str(corpus_path),
        "generated_at": now_iso(),
        "results": results,
        "sold_bin": str(sold_bin),
        "sold_entrypoint": str(sold_ld64),
        "comparisons": comparisons,
    }
    results_path = out_dir / "results.json"
    results_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")

    failures = []
    for result in results:
        if result["exit_status"] != 0 or not result["smoke_pass"]:
            failures.append(f"{result['target']}:{result['linker']}")

    for comparison in comparisons:
        win_pct = comparison["sold_win_pct"]
        if win_pct is None or win_pct < args.min_win_pct:
            failures.append(f"{comparison['target']}:sold-win")

    if failures:
        print(f"benchmark failures: {', '.join(sorted(set(failures)))}", file=sys.stderr)
        return 1

    print(f"wrote {results_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

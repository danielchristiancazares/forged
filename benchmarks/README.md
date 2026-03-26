# Benchmark Harness

This directory contains the Rust-first macOS benchmark harness for `sold`.
The harness captures the exact linker-driver argv that `rustc` emits during
`cargo build`, derives the concrete Apple `ld` command via `clang -###`, and
then replays that final link in isolation against Apple `ld` and `sold`.

Optionally, pass `--lld-bin` to also benchmark **LLVM’s Mach-O linker**
(`ld64.lld`, the same binary Homebrew installs as `lld` / under
`$(brew --prefix llvm)/bin/ld64.lld`). The replay uses the same argv as Apple
`ld` with `argv[0]` replaced by that path. LLVM lld may not accept every Apple
`ld` flag on a given capture; by default a failed or invalid lld replay is
recorded in `results.json` but does **not** fail the harness exit status. Use
`--require-lld` to treat lld failures like sold failures.

Cargo builds are redirected into a benchmark-owned target directory under
`benchmarks/out/targets/<name>/cargo-target` so the harness does not mutate
or race with the source project's normal `target/` tree.

## Files

- `corpus.toml`: corpus manifest describing the Rust targets to benchmark
- `capture_linker.py`: linker-driver wrapper used during Cargo capture
- `run.py`: orchestrates Cargo capture, isolated replay, smoke tests, and
  machine-readable results

## Workflow

1. Build `sold`.
2. Fill in `benchmarks/corpus.toml` with one or more real Rust targets.
3. Run:

```sh
python3 benchmarks/run.py --sold-bin /path/to/ld64
```

Apple ld vs LLVM lld vs sold (example on Homebrew LLVM):

```sh
python3 benchmarks/run.py \
  --sold-bin ./build/ld64 \
  --lld-bin "$(brew --prefix llvm)/bin/ld64.lld"
```

If `--sold-bin` points to the built `mold` executable instead of an `ld64`
symlink, the runner creates a temporary `ld64.sold` symlink automatically so
the Mach-O entrypoint is selected.

Use `--target <name>` to run only a subset of the corpus during bring-up or
scanning.

Use `--collect-sold-perf` to run one extra sold-only replay per target with
`-perf -stats`. That extra replay is stored separately from the measured
benchmark runs so it does not affect the Apple-vs-sold median wall-time
comparison.

Results are written to `benchmarks/out/results.json`. Per-target captures,
derived commands, replay logs, and binaries are written under
`benchmarks/out/targets/`. When `--lld-bin` is set, each target also writes
`lld-ld-command.json`, and `comparisons[]` includes `lld_median_wall_s`,
`sold_vs_lld_win_pct` (positive when sold is faster than lld), and
`lld_vs_apple_win_pct` (positive when lld is faster than Apple ld).

When `--collect-sold-perf` is enabled, sold results in `results.json` also
include:

- `perf_archive_fallbacks`: eager fallback archives with a machine-readable reason and path
- `perf_counters`: parsed `-stats` `name=value` counters
- `perf_timers`: parsed `-perf` timer rows with nesting depth
- `perf_timer_totals_real_s`: per-name sums of timer real time across repeated rows

## Manifest format

Each target is declared as a `[[target]]` table:

```toml
[[target]]
name = "my-binary"
project_root = "/absolute/path/to/project"
cargo_args = ["build", "--release", "--bin", "my-binary"]
binary_relpath = "target/release/my-binary"
smoke_cmd = ["{binary}", "--help"]
```

Rules:

- `project_root` must be an absolute path.
- `cargo_args` are the arguments passed after `cargo`.
- `binary_relpath` is relative to `project_root`. If it starts with
  `target/`, the runner automatically remaps it into the benchmark-owned
  Cargo target directory.
- `smoke_cmd` is an argv array. Use `{binary}` anywhere the produced binary
  path should be substituted.

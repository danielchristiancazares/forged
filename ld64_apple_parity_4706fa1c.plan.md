---
name: MVP Apple ld parity
overview: "Narrow the goal to one MVP outcome: sold reaches practical speed parity with Apple's Mach-O linker (`ld`) on the two existing benchmark repos, within an explicit noise budget and while keeping the current replay correctness bar green. This is now a performance-focused bring-up plan that uses the existing replay harness to drive sold-vs-Apple ld comparisons, and only pursues compatibility work when it blocks fair benchmarking."
todos:
  - id: freeze-scope
    content: "Lock the MVP scope to forge and tools-mcp x86_64 replay-only Apple ld parity, with explicit non-goals. Deliverable: `run_mvp.sh` wrapper script and `baseline_config.json`."
    status: pending
  - id: stabilize-measurement
    content: "Define a fixed benchmark protocol, dedicated runner, and explicit noise budget so sold-vs-Apple ld comparisons are meaningful."
    status: pending
  - id: collect-sold-perf
    content: "Run the two corpus targets with --collect-sold-perf to establish the first solid sold-vs-Apple baseline and extract shared hotspots. Deliverables: `results.json` run output, `perf_baseline.md` report."
    status: pending
  - id: rank-hotspots
    content: "Rank bottlenecks by absolute milliseconds needed to close the sold-vs-Apple ld gap, prioritizing hotspots that appear in both repos. Deliverable: `hotspots.md` table."
    status: pending
  - id: optimize-critical-path
    content: "Implement the smallest set of hot-path optimizations needed to bring sold within the accepted Apple ld noise band on both repos. Deliverables: performance PRs with validated before/after ms metric logs."
    status: pending
  - id: preserve-correctness
    content: "Re-run replay validation after each optimization and treat correctness regressions as blocking. Deliverable: Pre-commit/CI checks enforcing zero-tolerance on validation regressions."
    status: pending
  - id: declare-done
    content: "Declare MVP done only when sold median wall time is within the accepted noise budget of Apple ld on both targets under the chosen measurement protocol. Deliverables: Final `results.json` proving parity."
    status: pending
isProject: false
---

# MVP path to Apple ld parity on two benchmark repos

## Goal

- Reach practical speed parity with Apple's Mach-O linker `ld` on the two current benchmark targets in `[/Users/dcazares/Documents/Code/forged/benchmarks/corpus.toml](/Users/dcazares/Documents/Code/forged/benchmarks/corpus.toml)`:
  - `forge`
  - `tools-mcp`
- For this MVP, parity means sold median wall time is no worse than Apple `ld` by more than the accepted noise budget on either target.
- Keep the current correctness bar intact:
  - successful replayed link
  - `file` / `lipo` / `otool` checks
  - target smoke pass
- Use the existing replay harness in `[/Users/dcazares/Documents/Code/forged/benchmarks/README.md](/Users/dcazares/Documents/Code/forged/benchmarks/README.md)`, specifically `--collect-sold-perf`, to drive decisions.

## Non-goals

- Broad Mach-O feature completeness outside these two workloads
- Platform expansion beyond the existing x86_64 replayed benchmark path
- `arm64e`, watchOS, simulator, and relocatable `-r` work unless one of the two repos actually demands it
- Bit-identical output to Apple `ld`
- General “faster than Apple on all Mach-O links” claims
- Per-sample dominance over Apple `ld`

## Current baseline (already in tree)

- The harness already captures the real Rust-driven link command, derives the Apple `ld` invocation via `clang -###`, and replays that same argv. Thus, the harness natively pits Apple `ld` against `sold`: `[/Users/dcazares/Documents/Code/forged/benchmarks/README.md](/Users/dcazares/Documents/Code/forged/benchmarks/README.md)` and `[/Users/dcazares/Documents/Code/forged/benchmarks/run.py](/Users/dcazares/Documents/Code/forged/benchmarks/run.py)`.
- The active corpus is already the correct scope boundary for this MVP: `[/Users/dcazares/Documents/Code/forged/benchmarks/corpus.toml](/Users/dcazares/Documents/Code/forged/benchmarks/corpus.toml)`.
- The current Apple-vs-sold replay-only baseline in `[/Users/dcazares/Documents/Code/forged/benchmarks/out-direct-replay/results.json](/Users/dcazares/Documents/Code/forged/benchmarks/out-direct-replay/results.json)` is an initial proof of concept, showing:
  - `forge`: Apple `0.31s`, sold `0.60s` (about `+0.29s`, roughly `1.9x` slower)
  - `tools-mcp`: Apple `0.22s`, sold `0.32s` (about `+0.10s`, roughly `1.45x` slower)
- The first task is to formalize this into a stable, repeatable benchmark protocol before optimizing to close that specific gap.

## What this changes architecturally

- Compatibility work is now subordinate to performance work.
- Only fix linker-behavior gaps when they block:
  - capturing the real link
  - replaying the real link through both sold and Apple `ld`
  - measuring sold-vs-Apple apples-to-apples
- The main system boundary is now:
  - inputs: the two benchmark repos and the exact link commands they emit
  - outputs: measured sold-vs-Apple wall-time gap and still-valid binaries

## Recommended path

### 1. Freeze scope and measurement policy

- Keep the corpus fixed to `forge` and `tools-mcp`.
- Use a dedicated benchmark output directory so runs do not race with stale artifacts, consistent with the harness guidance in `[/Users/dcazares/Documents/Code/forged/benchmarks/README.md](/Users/dcazares/Documents/Code/forged/benchmarks/README.md)`.
- Define one measurement policy and stick to it for the entire MVP:
  - fixed warmup count
  - fixed measured run count
  - same host
  - same build type
- Freeze an explicit noise budget for parity: sold passes a target when its median wall time is within `max(5%, 0.015s)` of the Apple `ld` median for that target.
- Compare medians from the fixed run set, not individual samples.
- If sold misses the band on a target, rerun the exact same protocol once before calling it a regression so transient host noise does not dominate engineering decisions.
- Do not move the target by adding more repos before parity is reached on these two.

**Measurable Deliverables:**
- [ ] A dedicated wrapper script (`benchmarks/run_mvp.sh`) that hardcodes the accepted measurement policy (e.g., 3 warmups, 10 measured runs, `--out-dir benchmarks/out-mvp`).
- [ ] A documented `baseline_config.json` capturing the host specs (CPU, RAM, macOS version) and build type (`Release` with specific CMake flags) to ensure repeatability.

### 2. Collect sold-internal performance data first

- Use `--collect-sold-perf` so the harness records:
  - `perf_counters`
  - `perf_timers`
  - `perf_timer_totals_real_s`
  - `perf_archive_fallbacks`
- This makes the first optimization pass evidence-driven instead of guessing from source structure.
- The first question is not “what features are missing?” It is “what is the sold-vs-Apple gap on these two exact links, and where are the missing milliseconds?”

**Measurable Deliverables:**
- [ ] Execution of `run_mvp.sh` on both targets to generate the initial `results.json` containing Apple `ld` medians vs. `sold` medians.
- [ ] A `perf_baseline.md` report summarizing the absolute sold-vs-Apple gap in milliseconds for both `forge` and `tools-mcp`.
- [ ] Extraction of the top 5 `perf_timers` from `sold` across both targets to identify exactly where time is spent.

### 3. Rank hotspots by shared impact, not by code curiosity

- Prioritize work that reduces time on both `forge` and `tools-mcp`.
- Prefer hotspots measured in absolute milliseconds over percentage-only wins.
- Deprioritize one-off cleanups unless one target cannot reach Apple `ld` parity without them.
- Treat likely candidates as hypotheses only until the timer data proves them.

**Measurable Deliverables:**
- [ ] A `hotspots.md` document containing a ranked table listing: Linker Phase, Time in ms (forge), Time in ms (tools-mcp), and Potential ms savings.
- [ ] Selection of the top 1-3 shared bottlenecks to tackle first.

### 4. Optimize only the critical path the benchmarks actually exercise

- Make the smallest code changes that directly reduce the measured dominant phases.
- Avoid broad compatibility projects, parser expansions, or feature work unless the benchmark capture requires them to replay cleanly through both sold and Apple `ld`.

**Measurable Deliverables:**
- [ ] Code commits addressing the selected hotspots.
- [ ] For every merged optimization, an updated benchmark table in PR descriptions showing the before/after milliseconds for both targets.

### 5. Keep correctness as a hard gate during tuning

- After each optimization, re-run the replay validation for both targets.
- Do not accept a speed win that changes:
  - replay success
  - binary validation
  - target smoke behavior

**Measurable Deliverables:**
- [ ] 100% pass rate on `file`, `lipo`, and `otool` checks for every performance-improving commit.
- [ ] 100% pass rate on the target smoke commands (`/bin/test -x` for `forge` and `--help` for `tools-mcp`).
- [ ] A zero-tolerance policy enforced: any speed win that causes the harness validation to fail is reverted or blocked.

### 6. Define done narrowly with an explicit noise policy

- MVP is done only when sold is within the accepted noise band of Apple `ld` on both benchmark targets under the frozen measurement policy.
- For this plan, the accepted noise band is `max(5%, 0.015s)` relative to the Apple `ld` median for each target.

**Measurable Deliverables:**
- [ ] Final `results.json` run artifact proving `sold` median wall time ≤ Apple `ld` median wall time + `max(5%, 0.015s)` for the `forge` target.
- [ ] Final `results.json` run artifact proving `sold` median wall time ≤ Apple `ld` median wall time + `max(5%, 0.015s)` for the `tools-mcp` target.
- [ ] Formal declaration of MVP completion logging the final medians and closing the parity milestone.

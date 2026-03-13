# Mach-O x86_64 Compatibility Plan

## Current Status

This repository already contains a substantial Mach-O linker. In sold builds,
`LICENSE.md` enables the Mach-O path, CMake instantiates Mach-O targets for
`X86_64`, `ARM64`, and `ARM64_32`, and test builds create a real `ld64`
entrypoint in the build tree. The implementation under `macho/` already
covers:

- ld64-style command-line parsing
- object, archive, universal binary, dylib, framework, and `.tbd` ingestion
- symbol resolution and dead stripping
- relocation scanning and target-specific thunks
- LTO integration
- synthetic chunk construction and final output writing
- bind/rebase/export metadata, fixup chains, code signatures, unwind info,
  stubs, GOT, and TLS-related chunks

The practical meaning is that the shortest path to "fully functional on macOS
x86_64" is not a redesign. It is a compatibility burn-down with stricter
validation on real Intel hardware.

## What Works Today

- The Mach-O path is already live behind the `ld64` entrypoint.
- The Rust-first benchmark/replay harness under `benchmarks/` is usable on
  macOS and currently targets x86_64 replay.
- The active replay corpus in `benchmarks/corpus.toml` contains two real local
  Rust executables: `forge` and `tools-mcp`.
- The current local replay-only baseline in
  `benchmarks/out-direct-replay/results.json` shows sold successfully replaying
  both targets through a real `ld64` entrypoint and passing `file`, `lipo`,
  `otool`, and target smoke checks.
- The current replay-only timings are a bring-up baseline, not a stable
  performance claim:
  - `forge`: Apple `0.31s`, sold `0.60s`
  - `tools-mcp`: Apple `0.22s`, sold `0.32s`

Recent bring-up fixes also matter for interpreting current status:

- `macho/input-files.cc` now falls back to `find_subsection(ctx, msym.value)`
  for `N_SECT` symbols when `sym_to_subsec[i]` is null. This is required for
  always-split sections such as `__cstring`.
- Defined `private extern` symbols must remain resolvable within the current
  link. They are module-local for export visibility, not fully file-local.
- Recent command-line and autolink compatibility fixes now cover
  `-fatal_warnings`, the `-macosx_version_min` alias, both
  `-needed_framework` and `-needed-framework`, and the common
  `LC_LINKER_OPTION` autolink forms without treating every unfamiliar embedded
  directive as a hard fatal error.

## Phase 1 Definition Of Done

Phase 1 should stay narrow:

- Host: a real Intel Mac running macOS
- Outputs: x86_64 Mach-O main executables
- Inputs: real Rust/Cargo workloads first, then focused handwritten fixtures
- Success criteria: correct runtime behavior, not just successful linking

The source of truth for this phase should be differential validation against
Apple `ld`, using the existing replay harness plus targeted shell tests.

## Highest-Leverage Compatibility Work

### 1. Command-line Fidelity

The fastest wins are in `macho/cmdline.cc`. The remaining work is not to make
the linker more feature-rich in the abstract, but to match the option spellings
and behaviors emitted by real Apple toolchains and drivers. Each option seen in
captured invocations should be classified as one of:

- fully implemented
- accepted and ignored
- accepted with warning
- hard error because semantics matter

Unsupported but harmless flags should not abort the link unless Apple `ld`
would also abort.

### 2. Autolinking And Embedded Link Directives

`LC_LINKER_OPTION` handling is a first-order compatibility surface. Modern
Apple toolchains do emit autolink directives, so the linker must accept the
common forms seen in real captures and avoid treating every unfamiliar embedded
option as a fatal error. The initial goal is compatibility with captured Rust
and mixed-toolchain workloads, not exhaustive support for every obscure option.

### 3. Runtime-Correctness Corners

Unwind and metadata correctness should be treated as bring-up blockers, not as
late polish:

- `__compact_unwind`
- `__eh_frame`
- personality references and LSDA
- panic/unwind across crates
- mixed Rust/C++ binaries
- deployment-target-sensitive behaviors such as chained fixups and
  `__init_offsets`

These are exactly the cases that tend to fail after simple binaries already
look healthy.

## Validation Strategy

The current harness already provides the right backbone:

- capture Cargo's real linker-driver argv
- derive the final Apple `ld` invocation via `clang -###`
- replay the same link against Apple `ld` and sold
- validate the output with `file`, `lipo`, `otool`, and a per-target smoke
  command

For x86_64 bring-up, validation should be tightened in this order:

1. Keep growing the replay corpus from real Rust workloads.
2. Add focused shell fixtures for dylibs, bundles, frameworks, weak/reexported
   libraries, TLV, dead strip, order files, LTO, and unwind-heavy binaries.
3. Require Intel-host regression gates: `ctest`, replay corpus, and
   differential checks against Apple `ld`.
4. Treat arm64-host x86_64 test coverage as supplemental signal, not the main
   gate.

## Priority Order

If choosing only a few near-term items, do them in this order:

1. Fix parser and option-compatibility bugs in `macho/cmdline.cc`.
2. Make `LC_LINKER_OPTION` handling accept the common autolink forms from real
   captures.
3. Turn real Rust captures into required x86_64 regression cases.
4. Add unwind-focused regression binaries.
5. Only after correctness is stable, use the replay harness to rank
   performance hotspots.

## Bottom Line

The current sold tree is already well past the "stub linker" stage for Mach-O.
The remaining work to make x86_64 dependable on macOS is primarily
compatibility and validation discipline: tighter ld64 option fidelity, broader
autolink coverage, deeper unwind/runtime tests, and real Intel-host regression
gates.

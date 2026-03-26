# AGENTS.md

## Repository Identity

This checkout is a local `sold` renamed `forged`  worktree: a Mach-O-capable revival of `mold`.
The build system, binary name, version string, and many docs still use the
name `mold`, but when `LICENSE.md` exists in the repository root CMake sets
`MOLD_IS_SOLD=ON` and includes the Mach-O linker codepath in addition to the
ELF linker.

The top-level executable entrypoint is `common/main.cc`:

- the built executable is still named `mold`
- if `argv[0]` is `ld64` or starts with `ld64.`, control goes to
  `mold::macho::main`
- all other invocations go to `mold::elf::main`

Do not treat the top-level `README.md` as authoritative for build, test, or
architecture details. It is intentionally sparse and macOS-oriented. The real
sources of truth are `CMakeLists.txt`, the code under `common/`, `elf/`, and
`macho/`, the tests under `test/`, the benchmark scripts and docs under
`benchmarks/`, and the docs in `docs/`.

## Current Local Status

These statements are true in this worktree as of the current milestone and are
important context for future agents:

- The Rust-first macOS benchmark harness under `benchmarks/` exists and is
  usable.
- The active seeded corpus in `benchmarks/corpus.toml` currently contains two
  real local executable targets:
  - `forge` from `/Users/dcazares/Documents/Code/forge-fresh`
  - `tools-mcp` from `/Users/dcazares/Documents/Code/tools-mcp`
- The real Mach-O linker path now replays both seeded Rust targets
  successfully when invoked through a real `ld64` entrypoint.
- A replay-only validation run was written to
  `benchmarks/out-direct-replay/results.json` with single measured runs and no
  warmups. The current baseline there is:
  - `forge`: Apple `0.31s`, sold `0.60s`
  - `tools-mcp`: Apple `0.22s`, sold `0.32s`
- Those numbers are useful as a current baseline, not as a statistically
  stable performance claim.
- Those replay-only outputs passed `file`, `lipo`, `otool`, and target smoke
  checks.
- The shared `benchmarks/out` tree is mutable during Cargo capture/replay and
  can become stale or inconsistent if another benchmark run is touching the
  same target directory at the same time. If you need isolated results, use a
  separate `--out-dir`.

Recent code-level bring-up details that should not be regressed:

- The key Rust/Mach-O replay fix is in `macho/input-files.cc` inside
  `ObjectFile<E>::resolve_symbols`. For `N_SECT` symbols, if
  `sym_to_subsec[i]` is null, the code must fall back to
  `find_subsection(ctx, msym.value)`. This matters for always-split sections
  such as `__cstring`.
- Do not reintroduce the idea that defined `private extern` symbols should be
  treated as fully file-local. Rust archive replay depends on those symbols
  remaining globally resolvable within the current link even though they are
  module-private for export visibility.

## Working Rules For Agents

- You MUST NOT call codex.list_mcp_resources or codex.list_mcp_resource_templates. 
  * They don't provide any useful information.
- Treat the core linker as a C++20 codebase. The benchmark harness is
  Python/TOML with Rust/Cargo integration. On non-MSVC builds, the C++ linker
  code compiles with `-fno-exceptions`, `-fno-unwind-tables`, and
  `-fno-asynchronous-unwind-tables`. Do not introduce exception-based control
  flow into the linker.
- Keep builds out of source.
- Do not edit generated per-target wrapper `.cc` files in the build tree.
  Edit the real sources under `common/`, `elf/`, or `macho/`.
- If you change user-visible CLI behavior, help text, version output, or
  installed names, review `docs/mold.1`, `docs/mold.md`, and the shell tests.
- If you change build options or dependency handling, review both
  `CMakeLists.txt` and `install-build-deps.sh`.
- If you change benchmark harness behavior, update `benchmarks/README.md` and
  keep `benchmarks/corpus.toml` assumptions honest.
- If you change Mach-O symbol resolution, subsection handling, archive
  extraction, or Rust-relevant linker behavior, do not stop at unit reasoning.
  Re-run at least the focused Mach-O tests and preferably one captured Rust
  replay target.
- If you change sold-only behavior, remember that this checkout is a sold
  build because `LICENSE.md` is present.

## Configure, Build, Install, Test

Use out-of-source builds. From the repository root, these commands are
correct:

```bash
# Configure
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build

# Run all configured tests
ctest --test-dir build --output-on-failure

# Run a single test by regex
ctest --test-dir build -R '<test-regex>' --output-on-failure

# Install
cmake --install build
```

Important build and test facts:

- `include(CTest)` enables `BUILD_TESTING` unless it is explicitly turned off.
- The main binary is `build/mold`.
- When `BUILD_TESTING=ON` and `WIN32` is false, the build adds post-build
  symlinks in the build directory:
  - `build/ld`
  - `build/ld64` only when `MOLD_IS_SOLD=ON`
- On Apple hosts, CTest adds `test/macho` only when `MOLD_IS_SOLD=ON`.
- On non-Apple UNIX hosts, CTest adds `test/elf`.
- Mach-O test names are prefixed with `CMAKE_HOST_SYSTEM_PROCESSOR`, and on
  `arm64` hosts CMake also adds an `x86_64-...` copy of each Mach-O test.
- ELF test names are machine-prefixed by the triple setup in
  `test/elf/CMakeLists.txt`, for example `x86_64-common`.
- ELF test discovery is filename-driven: underscore-free `*.sh` are treated as
  common tests, while `${MACHINE}_*.sh` are machine-specific tests for that
  architecture.
- In ELF test setup, host normalization maps `arm.*` to `arm` and `ppc64` to
  `powerpc64` before native-triple matching.
- Native ELF tests get `SKIP_REGULAR_EXPRESSION "skipped"`. Non-native ELF
  tests are only added when `MOLD_ENABLE_QEMU_TESTS=ON` and receive
  `TRIPLE=<triple>` in their environment.

Current useful configure options:

- `-DMOLD_USE_ASAN=ON`: AddressSanitizer and UBSan
- `-DMOLD_USE_TSAN=ON`: ThreadSanitizer
- `-DMOLD_LTO=ON`: link-time optimization for the linker binary itself
- `-DMOLD_USE_MOLD=ON`: build `mold` with `-fuse-ld=mold`
- `-DMOLD_USE_MIMALLOC=OFF`: disable mimalloc
- `-DMOLD_USE_SYSTEM_MIMALLOC=ON`: use a system mimalloc instead of vendored
- `-DMOLD_USE_SYSTEM_TBB=ON`: use system TBB instead of vendored
- `-DMOLD_MOSTLY_STATIC=ON`: Linux packaging-style build with static
  libstdc++ and libcrypto
- `-DBUILD_TESTING=OFF`: disable tests and the build-tree `ld`/`ld64`
  symlink setup
- `-DMOLD_ENABLE_QEMU_TESTS=ON`: enable non-native ELF tests
- `-DMOLD_ENABLE_QEMU_TESTS_RV32=ON`: add RV32 ELF tests
- `-DMOLD_ENABLE_QEMU_TESTS_POWER10=ON`: add Power10 ELF tests

CMake 4 compatibility note:

- `CMakeLists.txt` already sets `CMAKE_POLICY_VERSION_MINIMUM=3.5` when
  `CMAKE_VERSION >= 4.0` so vendored oneTBB can still configure without
  requiring a manual policy override.

Install behavior:

- `cmake --install` installs the `mold` executable into
  `${CMAKE_INSTALL_BINDIR}`.
- It installs `docs/mold.1` into `${CMAKE_INSTALL_MANDIR}/man1/`.
- If a root-level `LICENSE` file exists, it is installed into
  `${CMAKE_INSTALL_DOCDIR}`. The sold-enabling file in this checkout is
  `LICENSE.md`, and that path is not installed by the current CMake logic.
- It installs a relative symlink at `${CMAKE_INSTALL_LIBEXECDIR}/mold/ld`
  pointing back to the `mold` executable.
- It installs `${CMAKE_INSTALL_BINDIR}/ld.mold` and a matching manpage symlink
  `ld.mold.1`.
- In sold builds it also installs:
  - `${CMAKE_INSTALL_BINDIR}/ld64.mold`
  - `${CMAKE_INSTALL_BINDIR}/ld.sold`
  - `${CMAKE_INSTALL_BINDIR}/ld64.sold`

## Build Dependencies And Linkage

The build prefers some system libraries when available and otherwise falls
back to vendored copies under `third-party/`.

- TBB is mandatory. By default the vendored copy is built; use
  `-DMOLD_USE_SYSTEM_TBB=ON` for a system TBB.
- zlib is used from the system if found, otherwise `third-party/zlib` is
  built.
- zstd is linked from the system if `zstd.h` is available, otherwise the
  vendored `third-party/zstd` build is used.
- mimalloc is enabled by default only on non-Apple, non-Android, non-32-bit
  builds. On Apple it is off by default.
- On non-Apple, non-Windows, non-`MOLD_MOSTLY_STATIC` builds, OpenSSL Crypto
  is required.
- `libm` and `-latomic` are added conditionally where needed.
- On Windows, plain MSVC is rejected; the build requires Clang.

Platform-specific extras:

- On non-Apple and non-Windows builds, CMake also builds `mold-wrapper.so`
  from `elf/mold-wrapper.c` and installs it under `${CMAKE_INSTALL_LIBDIR}/mold`.
- `elf/subprocess.cc` looks for `mold-wrapper.so` beside the executable, under
  `${MOLD_LIBDIR}/mold/`, and under `../lib/mold/`.
- `mold-wrapper.so` is not built on Apple.

## Repository Map

### `common/`

Shared infrastructure used by both ELF and Mach-O:

- `common/main.cc`: version string composition, signal handling, cleanup,
  default thread count, and ELF/Mach-O dispatch
- `common/common.h`: core types and helpers such as `Atomic<T>`,
  `ConcurrentMap`, `Counter`, `Timer`, `MappedFile`, `OutputFile`,
  `SyncOut`, `Warn`, `Error`, and `Fatal`
- `common/output-file-unix.h` and `common/output-file-win32.h`: output file
  implementations
- `common/filetype.h`: file-type detection, including LTO object detection
- additional utilities: compression, demangling, globbing, paths,
  HyperLogLog, UUIDs, tar support, and performance counters

### `elf/`

ELF linker implementation:

- `elf/main.cc`: top-level ELF driver
- `elf/cmdline.cc`: GNU `ld`-style argument parsing and help/version output
- `elf/passes.cc`: major ELF pass pipeline
- `elf/input-files.cc`: object/shared-library parsing and symbol ingestion
- `elf/input-sections.cc`: input-section handling and relocation-side data
- `elf/output-chunks.cc`: output sections, GOT/PLT, dynamic tables, symbol
  tables, build IDs, debug compression, and final writers
- `elf/linker-script.cc`: linker scripts, version scripts, and dynamic lists
- `elf/gc-sections.cc`: section garbage collection
- `elf/icf.cc`: identical code/data folding
- `elf/relocatable.cc`: relocatable-output path
- `elf/mapfile.cc`: mapfile generation
- `elf/thunks.cc` and `elf/tls.cc`: thunks and TLS support
- `elf/lto*.cc`: LTO integration
- `elf/subprocess.cc` and `elf/mold-wrapper.c`: subprocess wrapper support
- `elf/arch-*.cc`: backend-specific relocation and thunk logic
- `elf/mold.h`: main ELF context and most ELF-side data structures
- `elf/elf.h`: ELF type definitions and target descriptors

### `macho/`

Mach-O linker implementation, built only when sold support is enabled:

- `macho/main.cc`: top-level Mach-O driver and major link pipeline
- `macho/cmdline.cc`: `ld64`-style argument parsing
- `macho/input-files.cc`: Mach-O objects, archives, dylibs, universal
  binaries, text-based stubs, and symbol/subsection handling
- `macho/input-sections.cc`: subsection and relocation-facing input logic
- `macho/output-chunks.cc`: segment and section layout, bind/rebase/export
  data, fixup chains, code signatures, unwind info, stubs, GOT, TLS-related
  chunks, and final output writing
- `macho/dead-strip.cc`: dead stripping
- `macho/mapfile.cc`: map output
- `macho/thunks.cc`: branch thunk logic
- `macho/lto*.cc`: Mach-O LTO integration
- `macho/tapi.cc` and `macho/yaml.cc`: `.tbd` and YAML parsing
- `macho/mold.h`: main Mach-O context and most Mach-O data structures
- `macho/macho.h`: Mach-O constants and architecture type definitions
- `macho/README.md`: good background on Mach-O concepts and how this linker
  models them

### `test/`

Shell-based CTest suites:

- `test/elf/`: ELF tests generated per architecture/triple
- `test/macho/`: Mach-O tests generated for the host architecture and, on
  `arm64` hosts, an additional `x86_64` run
- `test/elf/common.inc` and `test/macho/common.inc`: shared harness logic

Focused Mach-O tests that mattered for recent Rust replay bring-up:

- `x86_64-basic`
- `x86_64-hello`
- `x86_64-private-extern`

### `benchmarks/`

Rust-first macOS benchmark harness:

- `benchmarks/README.md`: harness overview
- `benchmarks/corpus.toml`: current benchmark corpus manifest
- `benchmarks/capture_linker.py`: Cargo linker wrapper and capture tool
- `benchmarks/run.py`: orchestrates capture, replay, validation, smoke tests,
  timing, and result summaries
- `benchmarks/out/`: ignored generated output

### Other Important Paths

- `docs/coding-guidelines.md`: canonical local style guidance
- `docs/design.md`: architecture and performance background
- `docs/mold.1`: installable manpage
- `docs/mold.md`: CLI documentation
- `install-build-deps.sh`: distro-oriented dependency bootstrap script
- `dist.sh`: packaging-oriented build script
- `third-party/`: vendored dependencies

## Benchmark Harness Details

The benchmark harness is intentionally Rust-first and macOS-native. It is not
a generic linker benchmark suite, and it is not wired into CMake or CTest. Run
it manually with `python3 benchmarks/run.py ...`.

Capture flow:

- `benchmarks/run.py` sets `CARGO_TARGET_X86_64_APPLE_DARWIN_LINKER` to
  `benchmarks/capture_linker.py`
- it also sets:
  - `CARGO_TARGET_DIR` to a benchmark-owned directory under
    `benchmarks/out/targets/<name>/cargo-target`
  - `SOLD_CAPTURE_DIR` to the target capture directory
  - `SOLD_REAL_LINKER` to the real linker driver, defaulting to `clang`
- `capture_linker.py` runs the real linker driver, records the argv, derives
  the concrete final `ld` command from `clang -###`, and copies replay inputs
  that would otherwise disappear

Replay-input preservation details:

- the capture tool copies absolute `.o` files
- it also copies inputs whose path components include `rustc...`, which is how
  many temporary Rust linker inputs are preserved for replay
- the capture record stores a mapping from original input paths to copied
  inputs and rewrites derived replay argv to point at those copies

Replay and validation behavior in `benchmarks/run.py`:

- only supports `Darwin` hosts
- currently enforces an x86_64-only MVP
- requires a Python with stdlib `tomllib`, plus external tools including
  `cargo`, the real linker driver (`clang` by default), `xcrun`, `/usr/bin/time`,
  `file`, `lipo`, and `otool`
- rejects captured links that use LTO
- can take `--sold-bin` as either a true `ld64` entrypoint or a built `mold`
  executable; if the latter is used, the runner creates an `ld64.sold`
  symlink under the chosen output directory so the Mach-O entrypoint is used
- rewrites side-effect outputs under the benchmark output tree for:
  - `-o`
  - `-dependency_info`
  - `-map`
  - `-object_path_lto`
- validates produced binaries with:
  - `file`
  - `lipo -archs`
  - `otool -l`
- runs a per-target smoke command after each replayed link
- writes machine-readable summaries to `results.json`
- defaults to `1` warmup run, `10` measured runs, `--cargo-clean`, and a
  required sold win threshold of `10%`
- returns exit status `1` if either linker fails, if smoke validation fails,
  or if sold misses the configured win threshold

Current corpus details:

- `forge`
  - project root:
    `/Users/dcazares/Documents/Code/forge-fresh`
  - cargo args:
    `build --release -p forge --bin forge`
  - binary relpath:
    `target/release/forge`
  - smoke:
    `/bin/test -x {binary}`
  - reason for weak smoke: the Forge CLI currently appears to require a PTY
    for simple startup paths, so `--help` and `--version` are not dependable
    non-interactive smokes
- `tools-mcp`
  - project root:
    `/Users/dcazares/Documents/Code/tools-mcp`
  - cargo args:
    `build --release --bin tools-mcp`
  - binary relpath:
    `target/release/tools-mcp`
  - smoke:
    `{binary} --help`

Practical benchmark caveats:

- `benchmarks/out*` directories are ignored and generated, not source
- a shared output tree can become inconsistent if another benchmark run is
  still building or cleaning the same Cargo target directory
- if you need reliable bring-up or A/B comparison data, use a dedicated
  `--out-dir`
- output directories named `benchmarks/out*` are ignored by `.gitignore`;
  other custom output directories are not ignored automatically
- `benchmarks/out-direct-replay/results.json` is currently the cleanest
  verified replay-only baseline for real sold-vs-Apple results in this
  worktree, but it is still a local generated artifact rather than a tracked
  source file

Representative benchmark commands:

```bash
python3 benchmarks/run.py \
  --corpus benchmarks/corpus.toml \
  --target forge \
  --sold-bin ./build/ld64

python3 benchmarks/run.py \
  --corpus benchmarks/corpus.toml \
  --target tools-mcp \
  --sold-bin ./build/mold

python3 benchmarks/run.py \
  --corpus benchmarks/corpus.toml \
  --target tools-mcp \
  --sold-bin ./build/ld64 \
  --lld-bin "$(brew --prefix llvm)/bin/ld64.lld"
```

Useful flags:

- `--target <name>`: run only selected corpus targets
- `--out-dir <dir>`: keep results isolated from another run
- `--no-cargo-clean`: reuse an existing benchmark-owned Cargo target dir
- `--warmups <n>` and `--runs <n>`: control benchmark length
- `--min-win-pct <pct>`: required sold win threshold
- `--lld-bin <path>`: also benchmark LLVM Mach-O `ld64.lld` (optional; failures are recorded unless `--require-lld`)

## Template Instantiation Strategy

This codebase relies heavily on target-templated source files. To avoid
compiling one giant all-target translation unit, `CMakeLists.txt` generates
per-target wrapper `.cc` files in the build directory. Each generated file:

- defines `MOLD_<TARGET>`
- defines `MOLD_TARGET <TARGET>`
- includes the real source file from the source tree

Never edit those generated files directly.

Current instantiated target lists:

- ELF targets:
  `X86_64`, `I386`, `ARM64`, `ARM32`, `RV32LE`, `RV32BE`, `RV64LE`,
  `RV64BE`, `PPC32`, `PPC64V1`, `PPC64V2`, `S390X`, `SPARC64`, `M68K`,
  `SH4`, `ALPHA`
- Mach-O targets:
  `X86_64`, `ARM64`, `ARM64_32`

Current instantiated template counts from `CMakeLists.txt`:

- 16 ELF template source files x 16 ELF targets = 256 generated ELF
  translation units
- 11 Mach-O template source files x 3 Mach-O targets = 33 generated Mach-O
  translation units when sold is enabled

Non-template source details that matter:

- `macho/arch-x86-64.cc` is added as a non-template source
- `macho/yaml.cc` is added as a non-template source

If you add a new templated source file or target, you almost certainly need
to update these lists in `CMakeLists.txt`.

## Execution Model, Memory, And Performance

Concurrency model:

- the codebase uses Intel oneTBB extensively, especially `parallel_for` and
  `parallel_for_each`
- high-level linker stages are orchestrated serially
- the heavy work inside most stages is parallelized over files, sections,
  relocations, and synthesized records
- shared state uses a mix of relaxed-order `Atomic<T>`, TBB concurrent
  containers, and selective locking
- `common/main.cc` caps the default thread count at
  `min(tbb max allowed parallelism, 32)`
- ELF exposes thread-count control through `--thread-count`
- Mach-O exposes thread-count control through `-thread_count`
- performance instrumentation uses `Timer` and `Counter`

I/O behavior:

- input files are memory-mapped via `MappedFile`
- regular Unix output files normally use `MemoryMappedOutputFile`
- special outputs such as stdout or non-regular files use a malloc-backed
  output path instead
- Windows output uses the Win32-specific output implementation
- mapfiles are separate outputs and are not part of the main mmap-backed image
- because output may be memory-mapped, `common/main.cc` installs signal
  handling to turn disk-full write faults into a clearer fatal message

## Local Coding Conventions

The canonical style document is `docs/coding-guidelines.md`. Follow it unless
surrounding code has a stronger local pattern.

Key rules and caveats:

- prefer `i64` for integers unless object size matters enough to justify a
  smaller type
- avoid `auto` unless the actual type is obvious in immediate context
- keep inheritance shallow
- use `ASSERT` for always-on checks; `assert` still exists for debug-only
  assumptions
- prefer existing diagnostics helpers (`SyncOut`, `Warn`, `Error`, `Fatal`)
  over ad hoc output code
- prefer existing mapped-input and mapped-output helpers over introducing new
  file I/O patterns
- preserve the current data-oriented style: explicit loops, explicit types,
  and direct manipulation of linker state are normal here

## Agent-Specific Gotchas

- `README.md` is not enough. Always verify behavior in code and CMake.
- Because `LICENSE.md` exists in this checkout, Mach-O code and sold install
  aliases are active here even though upstream mold can be ELF-only.
- If you change a templated source file, expect a wide rebuild because of
  per-target instantiation.
- If you add a new user-facing linker option, you usually need matching test
  coverage and possibly help-text or manpage updates.
- If you touch subprocess, wrapper, or Linux packaging logic, review
  `elf/subprocess.cc`, `elf/mold-wrapper.c`, `mold-wrapper.so` handling, and
  `dist.sh` together.
- If you touch Mach-O symbol resolution, always keep Rust replay in mind.
  Recent failures were not obvious from small synthetic tests alone.
- The worktree currently contains local helper files such as `CLAUDE.md`,
  `justfile`, `repomix.config.json`, and `sold.txt`. Do not assume they are
  upstream project files, but do not delete or overwrite them casually
  either.

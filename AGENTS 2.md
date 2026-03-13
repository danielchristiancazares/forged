# AGENTS.md

## Repository Identity

This repository builds the `mold` linker. In this checkout, `LICENSE.md`
exists, so CMake sets `MOLD_IS_SOLD=ON` and compiles the Mach-O "sold"
codepath in addition to the ELF linker.

The top-level executable entrypoint is `common/main.cc`:

- If the binary is invoked as `ld64` or `ld64.*`, control goes to
  `mold::macho::main`.
- All other invocations go to `mold::elf::main`.

Do not assume the minimal `README.md` is authoritative for build or test
behavior. The real source of truth is `CMakeLists.txt`, the code under
`common/`, `elf/`, and `macho/`, and the docs in `docs/`.

## Working Rules for Agents

- Treat this as a C++20 codebase with non-MSVC builds using
  `-fno-exceptions`, `-fno-unwind-tables`, and
  `-fno-asynchronous-unwind-tables`. Do not introduce exception-based
  control flow.
- Keep builds out of source.
- Do not edit generated per-target `.cc` files in the build directory.
  Edit the real sources under `common/`, `elf/`, or `macho/`.
- If you change user-visible CLI behavior, help text, version output, or
  installed names, review `docs/mold.1`, `docs/mold.md`, and the shell
  tests.
- If you change build options or dependencies, review both
  `CMakeLists.txt` and `install-build-deps.sh`.
- If you change sold-specific behavior, remember that this checkout is a
  sold build because `LICENSE.md` is present.

## Configure, Build, Install, Test

Use out-of-source builds. These commands are correct from the repository
root:

```bash
# Configure
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build

# Run all tests
ctest --test-dir build --output-on-failure

# Run a single test by regex
ctest --test-dir build -R 'x86_64-basic' --output-on-failure

# Install
cmake --install build
```

Important details:

- `BUILD_TESTING` comes from `include(CTest)` and is enabled unless you
  explicitly turn it off.
- The main binary is `build/mold`.
- When `BUILD_TESTING=ON` and `WIN32` is false, the build adds post-build
  symlinks in the build directory:
  - `build/ld`
  - `build/ld64` only when `MOLD_IS_SOLD=ON`
- Install rules always create `ld.mold`; sold builds also install
  `ld64.mold`, `ld.sold`, and `ld64.sold`.
- On non-Apple, non-Windows builds, CMake also builds `mold-wrapper.so`.
  `elf/subprocess.cc` expects it for subprocess-based flows.

Useful configure options:

- `-DMOLD_USE_ASAN=ON`: enables AddressSanitizer and UBSan.
- `-DMOLD_USE_TSAN=ON`: enables ThreadSanitizer.
- `-DMOLD_LTO=ON`: enables interprocedural optimization for the linker
  binary itself.
- `-DMOLD_USE_MOLD=ON`: builds `mold` with `-fuse-ld=mold`.
- `-DMOLD_USE_MIMALLOC=OFF`: disables mimalloc.
- `-DMOLD_USE_SYSTEM_MIMALLOC=ON`: uses a system mimalloc instead of the
  vendored copy.
- `-DMOLD_USE_SYSTEM_TBB=ON`: uses system TBB instead of the vendored copy.
- `-DMOLD_MOSTLY_STATIC=ON`: packaging-oriented Linux build that
  statically links libstdc++ and libcrypto.
- `-DBUILD_TESTING=OFF`: disables tests and the test symlink setup.

ELF test options:

- `-DMOLD_ENABLE_QEMU_TESTS=ON`
- `-DMOLD_ENABLE_QEMU_TESTS_RV32=ON`
- `-DMOLD_ENABLE_QEMU_TESTS_POWER10=ON`

Platform and test behavior:

- macOS: if `MOLD_IS_SOLD=ON`, CTest adds `test/macho`.
- Other UNIX platforms: CTest adds `test/elf`.
- Windows: Windows-specific build code exists, but these top-level CTest
  suites are not wired in.
- ELF test names are architecture-prefixed, e.g. `x86_64-common`.
- Mach-O test names are prefixed with `CMAKE_HOST_SYSTEM_PROCESSOR`; on
  `arm64` hosts, CMake also adds an `x86_64-...` copy of each Mach-O test.

## Build Dependencies and Linkage

The build system prefers system libraries when available for some
dependencies and otherwise falls back to vendored copies under
`third-party/`.

- TBB is mandatory. By default the vendored copy is built; use
  `-DMOLD_USE_SYSTEM_TBB=ON` for a system TBB.
- zlib is taken from the system if found, otherwise `third-party/zlib` is
  built.
- zstd is linked from the system when `zstd.h` is available; otherwise the
  vendored `third-party/zstd` build is used.
- mimalloc is enabled by default only on non-Apple, non-Android, non-32-bit
  builds. It can use either the vendored or system version.
- On non-Apple, non-Windows, non-`MOLD_MOSTLY_STATIC` builds, OpenSSL
  Crypto is required.
- `libm` and `-latomic` are added conditionally where needed.
- On Windows, Clang is required; the CMake file explicitly rejects plain
  MSVC.

## Repository Map

### `common/`

Shared infrastructure used by both ELF and Mach-O:

- `common/main.cc`: version string composition, signal handling, cleanup,
  default thread count, and ELF/Mach-O dispatch.
- `common/common.h`: core types and helpers such as `Atomic<T>`,
  `ConcurrentMap`, `Counter`, `Timer`, `MappedFile`, `OutputFile`,
  `SyncOut`, `Warn`, `Error`, and `Fatal`.
- `common/output-file-unix.h` and `common/output-file-win32.h`: output file
  implementations.
- `common/filetype.h`: file type detection, including LTO object detection.
- Other utilities include compression, demangling, globbing, paths, UUIDs,
  tar support, and performance counters.

### `elf/`

ELF linker implementation:

- `elf/main.cc`: frontend driver for ELF linking. It handles input
  discovery, machine-type deduction, file loading, library lookup, thread
  setup, and the top-level link flow.
- `elf/cmdline.cc`: GNU `ld`-compatible argument parsing, help/version
  output, emulation selection, and thread-count parsing.
- `elf/passes.cc`: the main ELF pass pipeline.
- `elf/input-files.cc`: object/shared-library parsing and symbol ingestion.
- `elf/input-sections.cc`: input section handling and relocation-side data.
- `elf/output-chunks.cc`: output sections/chunks, GOT/PLT, dynamic tables,
  symbol tables, build IDs, GDB index output, and final writers.
- `elf/linker-script.cc`: linker script, version script, and dynamic list
  parsing.
- `elf/gc-sections.cc`: mark/sweep section garbage collection.
- `elf/icf.cc`: identical code/data folding.
- `elf/relocatable.cc`: relocatable-output path.
- `elf/mapfile.cc`: mapfile generation.
- `elf/thunks.cc` and `elf/tls.cc`: thunking and TLS support.
- `elf/lto*.cc`: LTO integration.
- `elf/subprocess.cc` and `elf/mold-wrapper.c`: subprocess wrapper support.
- `elf/elf.h`: ELF type definitions and target descriptors.
- `elf/mold.h`: main ELF context and most ELF-side data structures.
- `elf/arch-*.cc`: backend-specific relocation and thunk logic.

### `macho/`

Mach-O linker implementation, enabled only when sold support is compiled in:

- `macho/main.cc`: frontend driver and overall Mach-O link pipeline.
- `macho/cmdline.cc`: `ld64`-style argument parsing and thread-count
  handling.
- `macho/input-files.cc`: Mach-O object, archive, dylib, and `.tbd` input
  handling.
- `macho/input-sections.cc`: subsection and relocation-facing input logic.
- `macho/output-chunks.cc`: segment/section layout, bind/rebase/export
  tables, fixup chains, code signatures, unwind info, stubs, GOT, thread
  pointers, and final output writing.
- `macho/dead-strip.cc`: dead stripping.
- `macho/mapfile.cc`: map output.
- `macho/thunks.cc`: branch thunk logic.
- `macho/lto*.cc`: Mach-O LTO integration.
- `macho/tapi.cc` and `macho/yaml.cc`: parsing of text-based stubs and YAML.
- `macho/mold.h`: main Mach-O context and most data structures.
- `macho/macho.h`: Mach-O constants and architecture type definitions.
- `macho/README.md`: good background reading on Mach-O concepts and how this
  linker models them.

### `test/`

Shell-based CTest suites:

- `test/elf/`: ELF tests. `test/elf/CMakeLists.txt` generates target-specific
  test names from host or QEMU triples.
- `test/macho/`: Mach-O tests. `test/macho/CMakeLists.txt` generates tests
  for the host architecture and, on `arm64` hosts, an additional `x86_64`
  pass.
- `test/elf/common.inc` and `test/macho/common.inc`: shared harness logic.

### Other Important Paths

- `docs/coding-guidelines.md`: canonical local style guidance.
- `docs/design.md`: deep background on mold's architecture and performance
  model.
- `docs/mold.1`: installable manpage.
- `install-build-deps.sh`: distro-specific dependency bootstrap script.
- `dist.sh`: packaging-oriented build script.
- `third-party/`: vendored dependencies.
- `benchmarks/`: benchmark-related material.

## Template Instantiation Strategy

This codebase relies heavily on target-templated source files. To avoid
compiling a giant all-target translation unit, `CMakeLists.txt` generates
per-target wrapper `.cc` files in the build tree. Each generated file:

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
- 11 Mach-O template source files x 3 Mach-O targets = 33 generated
  Mach-O translation units when sold is enabled

If you add a new templated source file or target, you almost certainly need
to update these lists in `CMakeLists.txt`.

## Execution Model and Major Pipelines

### ELF

The ELF driver in `elf/main.cc` and `elf/passes.cc` follows this broad flow:

1. Parse arguments and select/deduce the emulation target.
2. Read input files, archives, shared objects, linker scripts, and LTO
   objects.
3. Resolve symbols and extract archive members as needed.
4. Run optimization/layout passes such as COMDAT elimination, garbage
   collection, ICF, merged-section sizing, output section creation, and
   relocation scanning.
5. Compute output layout, synthesize linker-generated chunks, write chunks,
   and emit tables such as dynamic sections, symbol tables, versioning data,
   build IDs, and optional compressed debug output.

Useful pass landmarks in `elf/passes.cc` include:

- `resolve_symbols`
- `eliminate_comdats`
- `gc`
- `icf`
- `create_output_sections`
- `claim_unresolved_symbols`
- `scan_relocations`
- `set_osec_offsets`
- `copy_chunks`
- `compress_debug_sections`

ELF input handling supports regular object files, shared libraries, fat and
thin archives, linker scripts, GCC LTO objects, and LLVM bitcode.

### Mach-O

The Mach-O driver in `macho/main.cc` follows this broad flow:

1. Parse `ld64`-style arguments and select the architecture.
2. Read object files, archives, universal binaries, dylibs, frameworks, and
   `.tbd` stubs.
3. Resolve symbols, perform LTO when needed, and prune unreachable inputs.
4. Run dead stripping, merge/uniquify subsections and literals, scan
   relocations, and build synthetic chunks.
5. Assign segment/section offsets and write the output image, including
   bind/rebase/export metadata, fixup chains, code signatures, unwind info,
   stubs, GOT, and TLS-related chunks.

Useful pass landmarks in `macho/main.cc` include:

- `resolve_symbols`
- `handle_exported_symbols_list`
- `handle_unexported_symbols_list`
- `create_internal_file`
- `remove_unreferenced_subsections`
- `claim_unresolved_symbols`
- `create_synthetic_chunks`
- `merge_mergeable_sections`
- `scan_relocations`
- `assign_offsets`
- `copy_sections_to_output_file`

## Concurrency, Memory, and Performance Model

- The codebase uses Intel oneTBB extensively, especially `parallel_for` and
  `parallel_for_each`.
- High-level linker stages are orchestrated serially, but the heavy work
  inside most stages is parallelized over files, sections, relocations, or
  synthesized records.
- Shared state uses a mix of relaxed-order `Atomic<T>`, TBB concurrent
  containers, and selective locking.
- `common/main.cc` caps the default thread count at
  `min(tbb max allowed parallelism, 32)`.
- ELF exposes thread count controls via `--thread-count` and related
  parsing in `elf/cmdline.cc`.
- Mach-O exposes thread count control via `-thread_count`.
- Performance instrumentation is built into the codebase through `Timer`
  and `Counter`.

I/O details:

- Input files are memory-mapped via `MappedFile`.
- Regular output files on Unix are usually memory-mapped through
  `MemoryMappedOutputFile`.
- Special outputs such as stdout or non-regular files use a malloc-backed
  path instead.
- Windows output uses a malloc-backed output file implementation.
- Mapfile output is separate and uses standard stream I/O in `elf/mapfile.cc`.
- Because output may be memory-mapped, `common/main.cc` installs signal
  handling to turn disk-full write faults into a clearer fatal message.

## Local Coding Conventions

The canonical style document is `docs/coding-guidelines.md`. Follow it
unless the surrounding code clearly does something more specific.

Key rules and caveats:

- Prefer `i64` for integers unless object size matters enough to justify a
  smaller type.
- Avoid `auto` unless the type is obvious from immediate context. The docs
  say "currently we use `auto` only for lambdas", but the tree already uses
  a few other obvious local `auto` cases too, so follow the spirit rather
  than that sentence literally.
- Keep inheritance shallow. The codebase mostly uses concrete types with
  short hierarchies.
- Use `ASSERT` for always-on checks; `assert` is still present for
  debug-only assumptions.
- Prefer existing diagnostics helpers (`SyncOut`, `Warn`, `Error`, `Fatal`)
  over ad hoc output code.
- Prefer existing memory-mapped/input/output helpers over introducing new
  file I/O patterns.
- Preserve the current data-oriented style: explicit loops, explicit types,
  and direct manipulation of linker state are normal here.

## Agent-Specific Gotchas

- `README.md` is intentionally sparse and macOS-focused. Do not use it as
  your only source when documenting or changing behavior.
- Because `LICENSE.md` exists in this checkout, Mach-O code and sold install
  aliases are active here even though upstream mold can be ELF-only.
- If you change a templated source file, expect a wide rebuild because of
  per-target instantiation.
- If you add a new user-facing linker option, you usually need matching test
  coverage and possibly manpage/help-text updates.
- If you touch subprocess, wrapper, or packaging logic on Linux, check
  `mold-wrapper.so`, `dist.sh`, and install symlink behavior together.

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

sold is the commercial version of mold, a high-performance linker. It supports both ELF (Linux) and Mach-O (macOS) targets. The project is written in C++20 with `-fno-exceptions`. The open-source mold repo only includes the ELF linker; this repo (sold) adds the Mach-O linker on top. The build system detects this via the presence of `LICENSE.md`.

## Build Commands

```bash
# Configure (out-of-source build required)
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..

# Build (uses all cores by default with template instantiation strategy)
cmake --build build

# Run tests (on macOS runs Mach-O tests, on Linux runs ELF tests)
cd build && ctest

# Run a single test by name
cd build && ctest -R <test-name>

# Build with sanitizers
cmake -DMOLD_USE_ASAN=ON ..   # AddressSanitizer
cmake -DMOLD_USE_TSAN=ON ..   # ThreadSanitizer
```

The binary is at `build/mold`. Symlinks `build/ld` and `build/ld64` are created automatically for testing.

## Architecture

### Source Layout

- **`common/`** - Shared utilities: entry point (`main.cc`), compression, demangling, file I/O, concurrent data structures, timers. `common.h` defines core types (`i64`, custom `Atomic<T>` with relaxed ordering, `ConcurrentMap`, `MappedFile`, etc.).
- **`elf/`** - ELF linker. 16 architecture backends (X86_64, I386, ARM64, ARM32, RV32/64 LE/BE, PPC32, PPC64V1/V2, S390X, SPARC64, M68K, SH4, ALPHA). Key files: `main.cc` (entry), `cmdline.cc` (arg parsing), `passes.cc` (linker passes), `input-files.cc`, `output-chunks.cc`.
- **`macho/`** - Mach-O linker (sold-only). 3 architecture backends (X86_64, ARM64, ARM64_32). `mold.h` contains all Mach-O context and data structures.
- **`test/elf/`** and **`test/macho/`** - Shell-script-based tests integrated with CTest.

### Template Instantiation Strategy

Almost all functions are templated on target architecture type (e.g., `X86_64`, `ARM64`). To parallelize compilation, CMakeLists.txt generates a separate `.cc` file per (source, target) pair that `#define`s the target and `#include`s the real source. This means there are ~256 ELF compilation units and ~33 Mach-O units from template files alone.

### Concurrency Model

mold uses data parallelism via Intel TBB's `parallel_for_each`. Linker passes run serially, but each pass parallelizes internally over input data (files, sections, relocations). Shared state uses atomic variables (custom `Atomic<T>` with relaxed ordering by default) and TBB concurrent hashmaps. No channels, futures, or high-level synchronization primitives.

### Key Dependencies (bundled in `third-party/`)

TBB (threading), mimalloc (allocator, non-Apple 64-bit only), zlib/zstd (compression), xxhash (hashing), rust-demangle.

## Coding Conventions

- Use `i64` for integers by default (alias for `int64_t`); use smaller types only when allocating millions of objects.
- Do not use `auto` unless the type is obvious from immediate context. Currently `auto` is used only for lambdas.
- Keep class hierarchies shallow (max depth of 2).
- `ASSERT` is always-enabled; `assert` is debug-only.
- All file I/O is mmap-based (`MappedFile`).

# GEMINI.md

## Project Overview
This repository contains `sold` (or `forged`), a high-performance linker for macOS (targeting Mach-O files). It was built because Apple's older `ld` linker was considered too slow prior to Xcode 15. The project is a descendant of the `mold` linker and supports both ELF and Mach-O formats, though its primary modern focus appears to be macOS compatibility (x86_64, ARM64, ARM64_32) via the Mach-O subsystem. 

The core of the linker is written in **C++20**. It implements `ld64`-style command-line parsing, symbol resolution, LTO integration, dead stripping, and synthetic chunk construction. The repository also includes a set of Python-based benchmarking and replay tools under the `benchmarks/` directory.

## Building and Running
The project uses **CMake** as its build system. 

To build the project, standard CMake commands are typically used:
```bash
# Generate the build system
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build the project
cmake --build build
```
*(Optional Configuration: `-DCMAKE_CXX_COMPILER=g++-12` or `-DCMAKE_INSTALL_PREFIX=/usr`)*

The `benchmarks/` folder contains scripts (like `run.py` and `test_run.py`) to run a custom test harness focused on real-world Rust executables and performance baselines.

## Development Conventions
The codebase has specific local coding rules designed to ensure simplicity and reproducibility:
- **C++ Standard**: The codebase strictly uses **C++20**.
- **Data Types**: Always use `i64` (an alias for `int64_t`) for integers unless you have a specific reason not to (e.g., allocating millions of the same object where size matters). Do not spend time optimizing 32-bit vs. 64-bit local variables.
- **Type Inference**: **Do NOT** use `auto` unless the actual type is incredibly obvious in a narrow context (currently, `auto` is almost exclusively used for lambdas). This is to ensure the code remains readable for first-time contributors without needing an IDE to infer types.
- **Object-Oriented Design**: **Do NOT** overuse inheritance. Most classes should not have parents, and if they do, the hierarchy should be extremely shallow (e.g., maximum depth of two: abstract interface and its implementation).
- **Format / Tools**: A `justfile` is present for some automation commands, such as `just pack` or `just zip`.

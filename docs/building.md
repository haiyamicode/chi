# Building

## Prerequisites

- C++17 compiler (GCC 9+ or Clang 10+)
- CMake 3.12+
- [Conan 1.x](https://conan.io/) package manager
- LLVM 17 (development headers and shared libraries)
- libclang (optional, enables C interop)
- System libraries: `zlib`, `zstd`, `libtinfo` (Linux) or `curses` (macOS)
  - macOS also needs `libxml2` (transitive LLVM/libclang dependency, ships with Xcode)

### Platform-specific dependencies

**macOS:**
```bash
brew install llvm@17 zstd
```

**Linux (Ubuntu/Debian):**
```bash
apt install llvm-17-dev libclang-17-dev zlib1g-dev libzstd-dev libncurses-dev
```

**Linux (Fedora/RHEL):**
```bash
dnf install llvm17-devel clang17-devel zlib-devel libzstd-devel ncurses-devel
```

**Windows:**
Native build on Windows is not yet tested / supported.
Please use Windows Subsystem for Linux and follow the Linux (Ubuntu/Debian) instructions above.

## Build

```bash
# Install Conan dependencies (one-time, from build/ directory)
mkdir -p build && cd build && conan install .. --build=missing && cd ..

# Build (Debug mode by default)
make build

# Or explicitly set build mode
BUILD_MODE=Release make build
```

## Test

```bash
# Run compiler tests
make test

# Run a specific test
cd tests && make 00_fib.test

# Run all tests (compiler + formatter + analyzer)
make test_all
```

## Options

```bash
# Enable AddressSanitizer (off by default)
cmake -DENABLE_ASAN=ON build

# Rebuild from scratch
make clean && make build
```

## Project Structure

```
src/
  lexer.cpp        Tokenizer
  parser.cpp       Parser → AST
  ast.cpp          AST node definitions
  resolver.cpp     Symbol resolution
  sema.cpp         Type checking
  codegen.cpp      LLVM IR generation
  builder.cpp      Build orchestration
  analyzer.cpp     IDE/LSP analysis mode
  runtime/         C runtime support
  stdlib/          Standard library (.xs)
tests/             Test suite
tests/analyzer/    Analyzer / error-recovery tests
tests/stress/      Stress and allocator tests
```

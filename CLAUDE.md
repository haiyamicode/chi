# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Chi is a programming language compiler/interpreter that compiles its own custom language (`.xc` files) to native executables, the compiler is written in C++. The project uses CMake for build configuration and includes LLVM-based code generation.

## Build Commands

### Prerequisites

- Install dependencies: `cd build && conan install $(pwd)/.. --build=missing`

### Core Build Commands

- **Build the compiler**: `make build` (builds in Debug mode by default)
- **Build release**: `BUILD_MODE=Release make build`
- **Clean build**: `make clean`
- **Install**: `make install` (after building)

### Testing

- **Run all tests**: `make test` (runs compiler tests)
- **Run specific test**: `cd tests && make 00_fib.test` (replace with test name)

### Development Workflow

- **Compile a chi program**: `make compile_example INPUT_FILE=local/test.xc`
- **Run compiled program**: `make run_example`
- **Debug mode compilation**: `make compile_example_debug`
- **Compile package**: `make compile_example_package INPUT_PACKAGE=local/test_package`
- **Generate AST**: `make ast INPUT_FILE=local/test.xc`
- **Generate assembly**: `make asm INPUT_FILE=local/test.xc`

## Architecture

### Core Components

**Frontend (Language Processing)**

- `lexer.cpp/.h` - Tokenizes chi source code (`.xc` files)
- `parser.cpp/.h` - Parses tokens into Abstract Syntax Tree (AST)
- `ast.cpp/.h` - Defines AST node types and structures
- `resolver.cpp/.h` - Symbol resolution and scope management
- `sema.cpp/.h` - Semantic analysis and type checking

**Backend (Code Generation)**

- `codegen.cpp/.h` - LLVM-based code generation
- `builder.cpp/.h` - High-level build orchestration
- `analyzer.cpp/.h` - Static analysis and optimization

**Runtime System**

- `runtime/` - Runtime support libraries
- `stdlib/` - Standard library implementation in chi

**Entry Point**

- `chi.cpp` - Main compiler driver with command-line interface

### Chi Language Features

The compiler supports:

- Structs and enums (including enum structs with variants)
- Functions with static method support
- Generics and type parameters
- Interfaces and implementations
- Module system with imports/exports
- Memory management with destructors
- Float operations and type inference
- **Managed Memory Mode**: Files with `.x` extension use automatic memory management with escape analysis
  - Escape analysis determines whether variables should be allocated on the stack or managed heap
  - Lambda captures are automatically analyzed for escape scenarios
  - Garbage collection handles memory cleanup for escaped allocations
  - Supports complex lambda scenarios including nested captures, recursive lambdas, and lambda chains

### Build System

- Uses CMake with Conan for dependency management
- Dependencies: fmt, libbacktrace, libuv
- Creates bundled static libraries for distribution
- Supports both Debug and Release configurations

### Test Framework

Tests are located in `tests/` directory with `.xc` source files and corresponding `.expect` files containing expected output. The test runner compiles each test and compares actual vs expected output.

## Working with the Codebase

### Key File Extensions

- `.xc` - Chi source files
- `.x` - Chi module files (managed memory mode)
- `.expect` - Expected test output files

### Debugging Compiler Crashes

- You can use lldb to debug the compiler or the output executable.
- Take a look at .vscode/launch.json for a working debugging setup using lldb, the configuration is for VSCode, but you can
  use a similar setup with command line.

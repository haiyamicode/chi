# Building

**Prerequisites:** LLVM, CMake, Conan 1.x, a C++17 compiler.

```bash
# Install dependencies
cd build && conan install $(pwd)/.. --build=missing

# Build
make build

# Run tests
make test

# Run a specific test
cd tests && make 00_fib.test
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

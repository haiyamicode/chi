# Development

## Workflow

Write test code in `local/test.xs`, then:

```bash
make run_example

# For managed-memory (.x) files:
INPUT_FILE=local/test.x make run_example
```

## Debugging with LLVM IR

Use `--emit-ir <path>` to write LLVM IR — useful for debugging codegen.

```bash
# make run_example writes IR to local/test.ll automatically
make run_example

# Or pass the flag directly:
chic -c myfile.xs -o myfile --emit-ir myfile.ll
```

## Debugging with lldb

See `.vscode/launch.json` for preconfigured debug targets.

```bash
# Debug the compiled test program
make compile_example
lldb -o run ./local/test

# Debug the compiler itself
lldb -o run -- build/src/bin/chic -d -c local/test.xs -o local/test -w local/build
```

## Tips

- Isolate issues into minimal test cases
- Prioritize debug prints over debugger
- Check the generated LLVM IR first when debugging codegen issues

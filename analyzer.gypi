{
    "variables": {
        "llvm_dir": "<!(test -n \"$LLVM_DIR\" || { echo 'ERROR: LLVM_DIR must be set to an LLVM 17 install root (e.g. /opt/homebrew/opt/llvm@17)' >&2; exit 1; }; echo $LLVM_DIR)",
    },
    "sources": [
        "src/analyzer.cpp",
        "src/resolver.cpp",
        "src/lexer.cpp",
        "src/parser.cpp",
        "src/sema.cpp",
        "src/context.cpp",
        "src/ast_printer.cpp",
        "src/ast.cpp",
        "src/c_importer.cpp",
        "src/package_config.cpp",
    ],
    "include_dirs": [
        "src/include",
        "src",
        "<(llvm_dir)/include",
    ],
    "defines": ["FMT_HEADER_ONLY", "HAVE_LIBCLANG", "CHI_RT_EXPORT=", "CHI_NO_RUNTIME"],
    "conditions": [
        # Link libclang by absolute path instead of -L<llvm>/lib + -lclang.
        # Adding $LLVM_DIR/lib to ld's search path collides with node-gyp's
        # -Wl,-search_paths_first: resolving libSystem's libunwind reexport,
        # ld finds LLVM's own libunwind.dylib (different install_name, can't
        # be matched as a reexport parent), falls back to linking it
        # directly, and writes a stale LC_LOAD_DYLIB for
        # /opt/homebrew/.../libunwind.1.dylib into the output — clashes
        # with the system libunwind in the dyld shared cache at runtime
        # and SIGTRAPs on C++ throw.
        ["OS==\"mac\"", {
            "libraries": ["<(llvm_dir)/lib/libclang.dylib"],
        }],
        ["OS==\"linux\"", {
            "libraries": ["-lclang"],
            "library_dirs": ["<(llvm_dir)/lib"],
        }],
    ],
}

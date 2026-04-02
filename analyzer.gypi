{
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
        "<!(echo ${LLVM_DIR:-NOT_SET}/include)",
    ],
    "defines": ["FMT_HEADER_ONLY", "HAVE_LIBCLANG", "CHI_RT_EXPORT=", "CHI_NO_RUNTIME"],
    "libraries": ["-lclang"],
    "library_dirs": [
        "<!(echo ${LLVM_DIR:-NOT_SET}/lib)",
    ],
}

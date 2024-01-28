include(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)
conan_basic_setup(TARGETS)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

execute_process(COMMAND llvm-config --libfiles --link-shared OUTPUT_VARIABLE LLVM_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
message(STATUS "LLVM libs: " ${LLVM_LIBS})

set(RUNTIME_SOURCE_FILES runtime/internals.cpp runtime/trace.cpp)
set(RUNTIME_HEADER_FILES runtime/internals.h runtime/source.h runtime/trace.h)

set(HEADER_FILES builder.h codegen.h ${RUNTIME_HEADER_FILES} util.h sema.h resolver.h lexer.h parser.h include/enum.h ast.h errors.h ast_printer.h include/optional.h include/variant.h include/tgc/tgc.h)
set(SOURCE_FILES analyzer.cpp builder.cpp ${RUNTIME_SOURCE_FILES} context.cpp codegen.cpp resolver.cpp lexer.cpp parser.cpp sema.cpp ast_printer.cpp include/tgc/tgc.c)
set(PKG_LIBS CONAN_PKG::fmt CONAN_PKG::libbacktrace)
set(LIBS ${PKG_LIBS} ${LLVM_LIBS} pthread dl)

# Compiler executable
add_executable(chi chc.cpp ${HEADER_FILES} ${SOURCE_FILES})
target_include_directories(chi PUBLIC ${LLVM_INCLUDE_DIRS})

set(ASAN_FLAGS "-fno-omit-frame-pointer -fsanitize=address")
set_target_properties(chi PROPERTIES COMPILE_FLAGS_DEBUG "${ASAN_FLAGS}>")
target_link_libraries(chi ${LIBS} "$<$<CONFIG:DEBUG>:${ASAN_FLAGS}>")

# Runtime library
add_library(rt STATIC ${HEADER_FILES} ${RUNTIME_SOURCE_FILES} include/tgc/tgc.c)
set_target_properties(rt PROPERTIES COMPILE_FLAGS_DEBUG "${ASAN_FLAGS}>")
target_link_libraries(rt PUBLIC fmt backtrace)
target_link_libraries(rt PRIVATE ${PKG_LIBS})

bundle_static_library(rt chrt)

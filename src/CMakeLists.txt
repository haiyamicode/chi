include(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)
conan_basic_setup(TARGETS)

find_package(LLVM 17 REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

execute_process(COMMAND llvm-config --libfiles --link-shared OUTPUT_VARIABLE LLVM_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
message(STATUS "LLVM libs: " ${LLVM_LIBS})

set(RUNTIME_SOURCE_FILES runtime/internals.cpp runtime/trace.cpp)
set(RUNTIME_HEADER_FILES runtime/internals.h runtime/trace.h)

set(HEADER_FILES builder.h codegen.h ${RUNTIME_HEADER_FILES} util.h sema.h resolver.h lexer.h parser.h include/enum.h ast.h errors.h ast_printer.h include/optional.h include/variant.h include/tgc/tgc.h include/hashdict/hashdict.h)
set(SOURCE_FILES analyzer.cpp builder.cpp ${RUNTIME_SOURCE_FILES} context.cpp codegen.cpp resolver.cpp lexer.cpp parser.cpp sema.cpp ast.cpp ast_printer.cpp include/tgc/tgc.c include/hashdict/hashdict.c)
set(PKG_LIBS CONAN_PKG::fmt CONAN_PKG::libbacktrace CONAN_PKG::libuv)
set(LIBS ${PKG_LIBS} ${LLVM_LIBS} pthread dl)

# Runtime library
include(GenerateExportHeader)
add_library(rt STATIC ${HEADER_FILES} ${RUNTIME_SOURCE_FILES} include/tgc/tgc.c include/hashdict/hashdict.c)
generate_export_header(rt PREFIX_NAME CHI_ EXPORT_FILE_NAME ${CMAKE_BINARY_DIR}/generated/rt_export.h)
target_include_directories(rt PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include" ${PROJECT_BINARY_DIR})
target_link_libraries(rt PUBLIC fmt backtrace uv)
target_link_libraries(rt PRIVATE ${PKG_LIBS})
bundle_static_library(rt chrt)

# Compiler executable
add_executable(chi chc.cpp ${SOURCE_FILES})
install(TARGETS chi)
target_include_directories(chi PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include" ${LLVM_INCLUDE_DIRS} ${PROJECT_BINARY_DIR})

set(ASAN_FLAGS "-fno-omit-frame-pointer -fsanitize=address")
set_target_properties(chi PROPERTIES COMPILE_FLAGS_DEBUG "${ASAN_FLAGS}>")
target_link_libraries(chi ${LIBS} "$<$<CONFIG:DEBUG>:${ASAN_FLAGS}>")

add_subdirectory(support)
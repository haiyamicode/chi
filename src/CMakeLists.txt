find_package(fmt REQUIRED)
find_package(libbacktrace REQUIRED)
find_package(libuv REQUIRED)
find_package(valijson REQUIRED)

find_package(LLVM 17 REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

message(STATUS "LLVM tools dir: ${LLVM_TOOLS_BINARY_DIR}")
# Link LLVM as a shared library so we share libLLVM-17.so.1 with libclang.
# Using --link-static would clash with libclang's own dynamic libLLVM
# ("CommandLine option registered more than once").
execute_process(COMMAND ${LLVM_TOOLS_BINARY_DIR}/llvm-config --libs --link-shared OUTPUT_VARIABLE LLVM_LIBS_RAW OUTPUT_STRIP_TRAILING_WHITESPACE)
separate_arguments(LLVM_LIBS UNIX_COMMAND "${LLVM_LIBS_RAW}")
message(STATUS "LLVM libs (shared): " ${LLVM_LIBS})
link_directories(${LLVM_LIBRARY_DIRS})

# libclang (C API) — only available as a shared lib in standard distributions
find_library(LIBCLANG_LIBRARY clang HINTS ${LLVM_LIBRARY_DIRS})
if(LIBCLANG_LIBRARY)
    message(STATUS "Found libclang: ${LIBCLANG_LIBRARY}")
    add_definitions(-DHAVE_LIBCLANG)
else()
    message(WARNING "libclang not found - C interop will be disabled")
endif()

set(RUNTIME_SOURCE_FILES runtime/internals.cpp runtime/format.cpp runtime/trace.cpp)
set(RUNTIME_HEADER_FILES runtime/internals.h runtime/format.h runtime/trace.h)

set(HEADER_FILES builder.h codegen.h ${RUNTIME_HEADER_FILES} util.h sema.h resolver.h lexer.h parser.h include/enum.h ast.h errors.h ast_printer.h include/optional.h include/variant.h include/tgc/tgc.h include/hashdict/hashdict.h package_config.h c_importer.h)
set(SOURCE_FILES analyzer.cpp builder.cpp ${RUNTIME_SOURCE_FILES} context.cpp codegen.cpp resolver.cpp lexer.cpp parser.cpp sema.cpp ast.cpp ast_printer.cpp include/tgc/tgc.c include/hashdict/hashdict.c package_config.cpp c_importer.cpp)
set(PKG_LIBS fmt::fmt libbacktrace::libbacktrace uv ValiJSON::valijson)
set(LIBS ${PKG_LIBS} ${LLVM_LIBS} pthread dl)
if(LIBCLANG_LIBRARY)
    list(APPEND LIBS ${LIBCLANG_LIBRARY})
endif()

# System libs required by LLVM/clang static linking
if(APPLE)
    find_library(LIB_ZSTD zstd REQUIRED)
    find_library(LIB_CURSES curses REQUIRED)
    find_library(LIB_XML2 xml2 REQUIRED)
    list(APPEND LIBS z ${LIB_ZSTD} ${LIB_CURSES} ${LIB_XML2})
else()
    list(APPEND LIBS z zstd tinfo)
endif()

# Resolve static library paths for bundling
find_library(FMT_STATIC_LIB NAMES fmtd fmt PATHS ${fmt_LIB_DIRS_DEBUG} ${fmt_LIB_DIRS_RELEASE} ${fmt_fmt_fmt_LIB_DIRS_DEBUG} ${fmt_fmt_fmt_LIB_DIRS_RELEASE} NO_DEFAULT_PATH)
find_library(BACKTRACE_STATIC_LIB NAMES backtrace PATHS ${libbacktrace_LIB_DIRS_DEBUG} ${libbacktrace_LIB_DIRS_RELEASE} NO_DEFAULT_PATH)
find_library(UV_STATIC_LIB NAMES uv PATHS ${libuv_LIB_DIRS_DEBUG} ${libuv_LIB_DIRS_RELEASE} NO_DEFAULT_PATH)
set(BUNDLE_DEPS ${FMT_STATIC_LIB} ${BACKTRACE_STATIC_LIB} ${UV_STATIC_LIB})
message(STATUS "Bundle deps: ${BUNDLE_DEPS}")

# Runtime library
include(GenerateExportHeader)
add_library(rt STATIC ${RUNTIME_SOURCE_FILES} include/tgc/tgc.c include/hashdict/hashdict.c)
generate_export_header(rt PREFIX_NAME CHI_ EXPORT_FILE_NAME ${CMAKE_BINARY_DIR}/generated/rt_export.h)
target_include_directories(rt PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include" ${PROJECT_BINARY_DIR})
target_link_libraries(rt PRIVATE ${PKG_LIBS})
bundle_static_library(rt chrt ${BUNDLE_DEPS})

add_library(rt_debug STATIC ${RUNTIME_SOURCE_FILES} include/tgc/tgc.c include/hashdict/hashdict.c)
target_include_directories(rt_debug PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include" ${PROJECT_BINARY_DIR})
target_link_libraries(rt_debug PRIVATE ${PKG_LIBS})
target_compile_definitions(rt_debug PRIVATE CHI_DEBUG_ALLOCATOR_RUNTIME=1)
bundle_static_library(rt_debug chrt_debug ${BUNDLE_DEPS})

# Generate embedded package schema header from JSON
set(PACKAGE_SCHEMA_INPUT ${CMAKE_CURRENT_SOURCE_DIR}/package_schema.json)
set(PACKAGE_SCHEMA_OUTPUT ${CMAKE_BINARY_DIR}/generated/package_schema_embed.h)
add_custom_command(
    OUTPUT ${PACKAGE_SCHEMA_OUTPUT}
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${PACKAGE_SCHEMA_INPUT}
        -DOUTPUT_FILE=${PACKAGE_SCHEMA_OUTPUT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_file.cmake
    DEPENDS ${PACKAGE_SCHEMA_INPUT} ${CMAKE_CURRENT_SOURCE_DIR}/embed_file.cmake
    COMMENT "Embedding package_schema.json"
    VERBATIM)

# Compiler executable
add_executable(chic chic.cpp ${SOURCE_FILES} ${PACKAGE_SCHEMA_OUTPUT})
set_target_properties(chic PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
install(TARGETS chic)
target_include_directories(chic PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include" ${LLVM_INCLUDE_DIRS} ${PROJECT_BINARY_DIR})
target_compile_definitions(chic PRIVATE LLVM_TOOLS_BINARY_DIR="${LLVM_TOOLS_BINARY_DIR}")

option(ENABLE_ASAN "Enable AddressSanitizer in Debug builds" OFF)
target_compile_options(chic PRIVATE -fno-omit-frame-pointer)
if(ENABLE_ASAN)
    set(ASAN_FLAGS "-fsanitize=address")
    set_target_properties(chic PROPERTIES COMPILE_FLAGS_DEBUG "${ASAN_FLAGS}")
    target_link_libraries(chic ${LIBS} "$<$<CONFIG:DEBUG>:${ASAN_FLAGS}>")
else()
    target_link_libraries(chic ${LIBS})
endif()

set(CHI_BOOTSTRAP_OUTPUT ${CMAKE_BINARY_DIR}/bin/chi)
set(CHI_BOOTSTRAP_WORKDIR ${CMAKE_BINARY_DIR}/chi_bootstrap)

add_custom_command(
    OUTPUT ${CHI_BOOTSTRAP_OUTPUT}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/bin
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CHI_BOOTSTRAP_WORKDIR}
    COMMAND ${CMAKE_COMMAND} -E env LIBRARY_PATH=${CMAKE_BINARY_DIR}
            ASAN_OPTIONS=detect_leaks=0
            $<TARGET_FILE:chic> -o ${CHI_BOOTSTRAP_OUTPUT} -c ${CMAKE_CURRENT_SOURCE_DIR}/chi/main.xs -w ${CHI_BOOTSTRAP_WORKDIR}
    DEPENDS chic ${CMAKE_CURRENT_SOURCE_DIR}/chi/main.xs
    COMMENT "Bootstrapping chi command"
    VERBATIM)

add_custom_target(chi ALL DEPENDS ${CHI_BOOTSTRAP_OUTPUT})
install(PROGRAMS ${CHI_BOOTSTRAP_OUTPUT} DESTINATION bin)

add_subdirectory(support)

include(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)
conan_basic_setup(TARGETS)

find_package(LLVM 17 REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

execute_process(COMMAND llvm-config --libfiles --link-shared OUTPUT_VARIABLE LLVM_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
message(STATUS "LLVM libs: " ${LLVM_LIBS})

# Find libclang for C interop
find_library(LIBCLANG_LIBRARY clang HINTS ${LLVM_LIBRARY_DIRS})
if(LIBCLANG_LIBRARY)
    message(STATUS "Found libclang: ${LIBCLANG_LIBRARY}")
else()
    message(WARNING "libclang not found - C interop will be disabled")
endif()

set(RUNTIME_SOURCE_FILES runtime/internals.cpp runtime/format.cpp runtime/trace.cpp)
set(RUNTIME_HEADER_FILES runtime/internals.h runtime/format.h runtime/trace.h)

set(HEADER_FILES builder.h codegen.h ${RUNTIME_HEADER_FILES} util.h sema.h resolver.h lexer.h parser.h include/enum.h ast.h errors.h ast_printer.h include/optional.h include/variant.h include/tgc/tgc.h include/hashdict/hashdict.h package_config.h c_importer.h)
set(SOURCE_FILES analyzer.cpp builder.cpp ${RUNTIME_SOURCE_FILES} context.cpp codegen.cpp resolver.cpp lexer.cpp parser.cpp sema.cpp ast.cpp ast_printer.cpp include/tgc/tgc.c include/hashdict/hashdict.c package_config.cpp c_importer.cpp)
set(PKG_LIBS CONAN_PKG::fmt CONAN_PKG::libbacktrace CONAN_PKG::libuv CONAN_PKG::valijson)
set(LIBS ${PKG_LIBS} ${LLVM_LIBS} pthread dl)

# Add libclang if available
if(LIBCLANG_LIBRARY)
    list(APPEND LIBS ${LIBCLANG_LIBRARY})
    add_definitions(-DHAVE_LIBCLANG)
endif()

# Runtime library
include(GenerateExportHeader)
add_library(rt STATIC ${RUNTIME_SOURCE_FILES} include/tgc/tgc.c include/hashdict/hashdict.c)
generate_export_header(rt PREFIX_NAME CHI_ EXPORT_FILE_NAME ${CMAKE_BINARY_DIR}/generated/rt_export.h)
target_include_directories(rt PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include" ${PROJECT_BINARY_DIR})
target_link_libraries(rt PUBLIC fmt backtrace uv)
target_link_libraries(rt PRIVATE ${PKG_LIBS})
bundle_static_library(rt chrt)

add_library(rt_debug STATIC ${RUNTIME_SOURCE_FILES} include/tgc/tgc.c include/hashdict/hashdict.c)
target_include_directories(rt_debug PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include" ${PROJECT_BINARY_DIR})
target_link_libraries(rt_debug PUBLIC fmt backtrace uv)
target_link_libraries(rt_debug PRIVATE ${PKG_LIBS})
target_compile_definitions(rt_debug PRIVATE CHI_DEBUG_ALLOCATOR_RUNTIME=1)
bundle_static_library(rt_debug chrt_debug)

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
install(TARGETS chic)
target_include_directories(chic PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include" ${LLVM_INCLUDE_DIRS} ${PROJECT_BINARY_DIR})

option(ENABLE_ASAN "Enable AddressSanitizer in Debug builds" ON)
target_compile_options(chic PRIVATE -fno-omit-frame-pointer)
if(ENABLE_ASAN)
    set(ASAN_FLAGS "-fsanitize=address")
    set_target_properties(chic PROPERTIES COMPILE_FLAGS_DEBUG "${ASAN_FLAGS}")
    target_link_libraries(chic ${LIBS} "$<$<CONFIG:DEBUG>:${ASAN_FLAGS}>")
else()
    target_link_libraries(chic ${LIBS})
endif()

set(CHI_BOOTSTRAP_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/bin/chi)
set(CHI_BOOTSTRAP_WORKDIR ${CMAKE_CURRENT_BINARY_DIR}/chi_bootstrap)

add_custom_command(
    OUTPUT ${CHI_BOOTSTRAP_OUTPUT}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/bin
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CHI_BOOTSTRAP_WORKDIR}
    COMMAND ${CMAKE_COMMAND} -E env LIBRARY_PATH=${CMAKE_BINARY_DIR}
            $<TARGET_FILE:chic> -o ${CHI_BOOTSTRAP_OUTPUT} -c ${CMAKE_CURRENT_SOURCE_DIR}/chi/main.xs -w ${CHI_BOOTSTRAP_WORKDIR}
    DEPENDS chic ${CMAKE_CURRENT_SOURCE_DIR}/chi/main.xs
    COMMENT "Bootstrapping chi command"
    VERBATIM)

add_custom_target(chi ALL DEPENDS ${CHI_BOOTSTRAP_OUTPUT})
install(PROGRAMS ${CHI_BOOTSTRAP_OUTPUT} DESTINATION bin)

add_subdirectory(support)

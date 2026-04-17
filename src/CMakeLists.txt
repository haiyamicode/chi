find_package(fmt REQUIRED)
find_package(libbacktrace REQUIRED)
find_package(libuv REQUIRED)
find_package(valijson REQUIRED)

option(RUNTIME_ONLY "Build only runtime libraries, skip compiler" OFF)
option(ENABLE_STATIC_LINK "Enable fully static LLVM/clang linking (Linux only)" OFF)
set(STATIC_LLVM_DIR "" CACHE PATH "Path to static LLVM/Clang build with libclang.a (for ENABLE_STATIC_LINK)")

if(ENABLE_STATIC_LINK AND APPLE)
    message(FATAL_ERROR "ENABLE_STATIC_LINK is not supported on macOS. Use shared linking (the default).")
endif()

if(NOT RUNTIME_ONLY)
    find_package(LLVM 17 REQUIRED CONFIG)
    message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
    message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
    message(STATUS "LLVM tools dir: ${LLVM_TOOLS_BINARY_DIR}")
    link_directories(${LLVM_LIBRARY_DIRS})

    # LLVM target components: static libclang initializes all backends internally,
    # so static linking always needs all-targets. Shared linking only needs native.
    if(ENABLE_STATIC_LINK OR APPLE)
        set(LLVM_TARGET_COMPONENTS all-targets)
    else()
        set(LLVM_TARGET_COMPONENTS native)
    endif()
    set(LLVM_BASE_COMPONENTS core support irreader bitwriter passes analysis target mc)

    if(ENABLE_STATIC_LINK)
        # Fully static LLVM/clang linking.
        message(STATUS "Static LLVM/clang linking enabled")

        execute_process(COMMAND ${LLVM_TOOLS_BINARY_DIR}/llvm-config --libfiles --link-static
            ${LLVM_TARGET_COMPONENTS} ${LLVM_BASE_COMPONENTS} windowsdriver frontendhlsl option
            OUTPUT_VARIABLE LLVM_LIBS_RAW OUTPUT_STRIP_TRAILING_WHITESPACE)
        separate_arguments(LLVM_LIBS UNIX_COMMAND "${LLVM_LIBS_RAW}")
        message(STATUS "LLVM libs (static): ${LLVM_LIBS}")

        # Search LLVM_LIBRARY_DIRS first (works on macOS/Alpine), then custom build path
        set(LIBCLANG_SEARCH_PATHS ${LLVM_LIBRARY_DIRS})
        if(STATIC_LLVM_DIR)
            list(APPEND LIBCLANG_SEARCH_PATHS "${STATIC_LLVM_DIR}/lib")
        endif()
        find_library(LIBCLANG_LIBRARY libclang.a HINTS ${LIBCLANG_SEARCH_PATHS} REQUIRED)
        message(STATUS "Found static libclang: ${LIBCLANG_LIBRARY}")
        add_definitions(-DHAVE_LIBCLANG)

        get_filename_component(CLANG_LIB_DIR ${LIBCLANG_LIBRARY} DIRECTORY)
        set(CLANG_COMPONENT_LIBS
            ${CLANG_LIB_DIR}/libclangAST.a
            ${CLANG_LIB_DIR}/libclangBasic.a
            ${CLANG_LIB_DIR}/libclangDriver.a
            ${CLANG_LIB_DIR}/libclangExtractAPI.a
            ${CLANG_LIB_DIR}/libclangFrontend.a
            ${CLANG_LIB_DIR}/libclangIndex.a
            ${CLANG_LIB_DIR}/libclangLex.a
            ${CLANG_LIB_DIR}/libclangRewrite.a
            ${CLANG_LIB_DIR}/libclangSema.a
            ${CLANG_LIB_DIR}/libclangSerialization.a
            ${CLANG_LIB_DIR}/libclangTooling.a
            ${CLANG_LIB_DIR}/libclangARCMigrate.a
            ${CLANG_LIB_DIR}/libclangAnalysis.a
            ${CLANG_LIB_DIR}/libclangASTMatchers.a
            ${CLANG_LIB_DIR}/libclangEdit.a
            ${CLANG_LIB_DIR}/libclangFormat.a
            ${CLANG_LIB_DIR}/libclangParse.a
            ${CLANG_LIB_DIR}/libclangSupport.a
            ${CLANG_LIB_DIR}/libclangToolingCore.a
            ${CLANG_LIB_DIR}/libclangToolingInclusions.a
        )
        if(APPLE)
            set(LIBCLANG_LIBS ${LIBCLANG_LIBRARY} ${CLANG_COMPONENT_LIBS})
        else()
            # GNU ld needs linker group for circular dependencies between clang static libs
            set(LIBCLANG_LIBS -Wl,--start-group ${LIBCLANG_LIBRARY} ${CLANG_COMPONENT_LIBS} -Wl,--end-group)
        endif()
    else()
        # Shared LLVM linking (default).
        # Link LLVM as shared library to avoid symbol conflicts with libclang's dynamic LLVM.
        execute_process(COMMAND ${LLVM_TOOLS_BINARY_DIR}/llvm-config --libs --link-shared
            OUTPUT_VARIABLE LLVM_LIBS_RAW OUTPUT_STRIP_TRAILING_WHITESPACE)
        separate_arguments(LLVM_LIBS UNIX_COMMAND "${LLVM_LIBS_RAW}")
        message(STATUS "LLVM libs (shared): ${LLVM_LIBS}")

        # libclang (C API) — only available as shared lib in standard distributions
        find_library(LIBCLANG_LIBRARY clang HINTS ${LLVM_LIBRARY_DIRS})
        if(LIBCLANG_LIBRARY)
            message(STATUS "Found libclang: ${LIBCLANG_LIBRARY}")
            add_definitions(-DHAVE_LIBCLANG)
            set(LIBCLANG_LIBS ${LIBCLANG_LIBRARY})
        else()
            message(WARNING "libclang not found - C interop will be disabled")
            set(LIBCLANG_LIBS "")
        endif()
    endif()
endif()

set(RUNTIME_SOURCE_FILES runtime/internals.cpp runtime/format.cpp runtime/trace.cpp)
set(RUNTIME_HEADER_FILES runtime/internals.h runtime/format.h runtime/trace.h)

set(PKG_LIBS fmt::fmt libbacktrace::libbacktrace ValiJSON::valijson)
if(RUNTIME_ONLY OR ENABLE_STATIC_LINK)
    # Use Conan's libuv target (static .a with include paths)
    list(APPEND PKG_LIBS libuv::uv_a)
else()
    list(APPEND PKG_LIBS uv)
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

if(NOT RUNTIME_ONLY)
    set(HEADER_FILES builder.h codegen.h ${RUNTIME_HEADER_FILES} util.h sema.h resolver.h lexer.h parser.h include/enum.h ast.h errors.h ast_printer.h include/optional.h include/variant.h include/tgc/tgc.h include/hashdict/hashdict.h package_config.h c_importer.h)
    set(SOURCE_FILES analyzer.cpp builder.cpp ${RUNTIME_SOURCE_FILES} context.cpp codegen.cpp resolver.cpp lexer.cpp parser.cpp sema.cpp ast.cpp ast_printer.cpp include/tgc/tgc.c include/hashdict/hashdict.c package_config.cpp c_importer.cpp)
    set(LIBS ${PKG_LIBS} ${LIBCLANG_LIBS} ${LLVM_LIBS} pthread dl)

    # System libs required by LLVM/clang linking
    if(APPLE)
        find_library(LIB_ZSTD zstd REQUIRED)
        find_library(LIB_CURSES curses REQUIRED)
        find_library(LIB_XML2 xml2 REQUIRED)
        list(APPEND LIBS z ${LIB_ZSTD} ${LIB_CURSES} ${LIB_XML2})
    else()
        if(ENABLE_STATIC_LINK)
            # Statically link LLVM's transitive system deps to minimize shared lib requirements
            find_library(ZLIB_STATIC_LIB libz.a)
            find_library(ZSTD_STATIC_LIB libzstd.a)
            # Alpine has no separate libtinfo — it's in libncursesw
            find_library(TINFO_STATIC_LIB NAMES libtinfo.a libncursesw.a)
            list(APPEND LIBS ${ZLIB_STATIC_LIB} ${ZSTD_STATIC_LIB} ${TINFO_STATIC_LIB} xml2 m)
        else()
            list(APPEND LIBS z zstd tinfo)
        endif()
    endif()

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

    if(ENABLE_STATIC_LINK AND NOT APPLE)
        target_link_options(chic PRIVATE -static-libstdc++ -static-libgcc)
    endif()

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
endif()

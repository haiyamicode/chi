include(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)
conan_basic_setup(TARGETS)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

execute_process(COMMAND llvm-config --libfiles --link-shared OUTPUT_VARIABLE LLVM_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
message(STATUS "LLVM libs: " ${LLVM_LIBS})

set(HEADER_FILES builder.h codegen.h util.h sema.h resolver.h lexer.h parser.h include/enum.h ast.h errors.h ast_printer.h internals.h include/optional.h include/variant.h)
set(SOURCE_FILES builder.cpp codegen.cpp resolver.cpp lexer.cpp parser.cpp sema.cpp ast_printer.cpp internals.cpp)
set(LIBS CONAN_PKG::fmt CONAN_PKG::backward-cpp ${LLVM_LIBS} pthread dl)

# Compiler executable
add_executable(chi chc.cpp ${HEADER_FILES} ${SOURCE_FILES})
target_include_directories(chi PUBLIC ${LLVM_INCLUDE_DIRS})

# Runtime library
add_library(rt STATIC ${HEADER_FILES} internals.cpp)
set_target_properties(rt PROPERTIES COMPILE_FLAGS_DEBUG "${ASAN_FLAGS}>")
target_link_libraries(rt PUBLIC fmt backward)

set(ASAN_FLAGS "-fno-omit-frame-pointer -fsanitize=address")
set_target_properties(chi PROPERTIES COMPILE_FLAGS_DEBUG "${ASAN_FLAGS}>")
target_link_libraries(chi ${LIBS} "$<$<CONFIG:DEBUG>:${ASAN_FLAGS}>")
# install(TARGETS rt DESTINATION ${CMAKE_INSTALL_PREFIX}/lib)

bundle_static_library(rt chrt)


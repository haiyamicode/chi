set(HEADER_FILES builder.h util.h sema.h resolver.h lexer.h parser.h include/enum.h ast.h errors.h ast_printer.h jit.h internals.h include/optional.h include/variant.h)
set(SOURCE_FILES builder.cpp resolver.cpp lexer.cpp parser.cpp sema.cpp ast_printer.cpp jit.cpp internals.cpp)
set(LIBS fmt jit jitplus Zydis)

add_library(chi SHARED ${HEADER_FILES} ${SOURCE_FILES})
add_library(chrt STATIC ${HEADER_FILES} internals.cpp)
add_executable(chc chc.cpp ${HEADER_FILES} ${SOURCE_FILES})

target_link_libraries(chi ${LIBS})

set(ASAN_FLAGS "-fno-omit-frame-pointer -fsanitize=address")
set_target_properties(chc PROPERTIES COMPILE_FLAGS_DEBUG "${ASAN_FLAGS}>")
target_link_libraries(chc ${LIBS} "$<$<CONFIG:DEBUG>:${ASAN_FLAGS}>")

install(TARGETS chi DESTINATION ${CMAKE_INSTALL_PREFIX}/lib)
install(TARGETS chrt DESTINATION ${CMAKE_INSTALL_PREFIX}/lib)


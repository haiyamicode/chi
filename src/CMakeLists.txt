add_executable(cx cx.cpp builder.h compat.h sema.h builder.cpp include/sso23.h include/optional.h resolver.cpp resolver.h lexer.h lexer.cpp parser.cpp parser.h include/enum.h ast.h sema.cpp include/tsl/hopscotch_hash.h include/tsl/hopscotch_map.h include/tsl/hopscotch_growth_policy.h error.h ast_printer.cpp ast_printer.h jit.cpp jit.h)
target_link_libraries(cx fmt jit jitplus)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
set(CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
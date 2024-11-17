
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_OSX_DEPLOYMENT_TARGET 14)

set(BINDING_SOURCE_FILES binding.cpp)

add_library(binding STATIC ${BINDING_SOURCE_FILES} )
include_directories(
  "${CMAKE_CURRENT_LIST_DIR}/include",
  "${PROJECT_SOURCE_DIR}/src",
  "${PROJECT_SOURCE_DIR}/src/include",
)
target_link_libraries(binding PUBLIC fmt)
target_link_libraries(binding PRIVATE ${PKG_LIBS})
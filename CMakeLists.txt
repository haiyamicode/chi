set(CONAN_DISABLE_CHECK_COMPILER True CACHE BOOL "" FORCE )
cmake_minimum_required(VERSION 3.12)
project(chi)

set(CMAKE_CXX_STANDARD 17)

include_directories("${PROJECT_SOURCE_DIR}/src")
add_subdirectory(src)
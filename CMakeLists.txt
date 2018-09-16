cmake_minimum_required(VERSION 3.12)
project(chi)

set(CMAKE_CXX_STANDARD 14)

include_directories("${PROJECT_SOURCE_DIR}/src")
add_subdirectory(src)
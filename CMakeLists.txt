set(CONAN_DISABLE_CHECK_COMPILER True CACHE BOOL "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "12" CACHE STRING "Minimum OS X deployment version")

cmake_minimum_required(VERSION 3.12)
project(chi LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)

# Bundling libraries
function(bundle_static_library tgt_name bundled_tgt_name)
  list(APPEND static_libs ${tgt_name})

  function(_recursively_collect_dependencies input_target)
    set(_input_link_libraries LINK_LIBRARIES)
    get_target_property(_input_type ${input_target} TYPE)

    if(${_input_type} STREQUAL "INTERFACE_LIBRARY")
      set(_input_link_libraries INTERFACE_LINK_LIBRARIES)
    endif()

    get_target_property(public_dependencies ${input_target} ${_input_link_libraries})
    message(STATUS "Public dependencies of ${input_target}: " ${public_dependencies})

    foreach(dependency IN LISTS public_dependencies)
      find_library(${dependency}_full_name ${dependency})
      message(STATUS "Dependency: " ${dependency} " -> " ${${dependency}_full_name})
      list(APPEND dep_libs ${${dependency}_full_name})
    endforeach()

    set(dep_libs ${dep_libs} PARENT_SCOPE)
  endfunction()

  message(STATUS "Bundling ${tgt_name} into ${bundled_tgt_name}")
  _recursively_collect_dependencies(${tgt_name})
  list(REMOVE_DUPLICATES dep_libs)
  message(STATUS "Static libs: " ${static_libs} "; Dep libs: " ${dep_libs})

  set(bundled_tgt_full_name
    ${CMAKE_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}${bundled_tgt_name}${CMAKE_STATIC_LIBRARY_SUFFIX})

  if(CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|GNU|AppleClang)$")
    file(WRITE ${CMAKE_BINARY_DIR}/${bundled_tgt_name}.ar.in
      "CREATE ${bundled_tgt_full_name}\n")

    foreach(tgt IN LISTS static_libs)
      message(STATUS "Adding ${tgt} to ${bundled_tgt_name}")
      file(APPEND ${CMAKE_BINARY_DIR}/${bundled_tgt_name}.ar.in
        "ADDLIB $<TARGET_FILE:${tgt}>\n")
    endforeach()

    foreach(tgt IN LISTS dep_libs)
      message(STATUS "Adding ${tgt} to ${bundled_tgt_name}")
      file(APPEND ${CMAKE_BINARY_DIR}/${bundled_tgt_name}.ar.in
        "ADDLIB ${tgt}\n")
    endforeach()

    file(APPEND ${CMAKE_BINARY_DIR}/${bundled_tgt_name}.ar.in "SAVE\n")
    file(APPEND ${CMAKE_BINARY_DIR}/${bundled_tgt_name}.ar.in "END\n")

    file(GENERATE
      OUTPUT ${CMAKE_BINARY_DIR}/${bundled_tgt_name}.ar
      INPUT ${CMAKE_BINARY_DIR}/${bundled_tgt_name}.ar.in)

    set(ar_tool "llvm-ar")

    if(CMAKE_INTERPROCEDURAL_OPTIMIZATION)
      set(ar_tool ${CMAKE_CXX_COMPILER_AR})
    endif()

    add_custom_command(
      COMMAND ${ar_tool} -M < ${CMAKE_BINARY_DIR}/${bundled_tgt_name}.ar
      DEPENDS ${static_libs}
      OUTPUT ${bundled_tgt_full_name}
      COMMENT "Bundling ${bundled_tgt_name} into ${bundled_tgt_full_name}"
      VERBATIM)
  elseif(MSVC)
    find_program(lib_tool lib)

    foreach(tgt IN LISTS static_libs)
      list(APPEND static_libs_full_names $<TARGET_FILE:${tgt}>)
    endforeach()

    add_custom_command(
      COMMAND ${lib_tool} /NOLOGO /OUT:${bundled_tgt_full_name} ${static_libs_full_names}
      DEPENDS ${static_libs}
      OUTPUT ${bundled_tgt_full_name}
      COMMENT "Bundling ${bundled_tgt_name}"
      VERBATIM)

  else()
    message(FATAL_ERROR "Unknown bundle scenario: " ${CMAKE_CXX_COMPILER_ID})
  endif()

  add_custom_target(bundling_target ALL DEPENDS ${bundled_tgt_full_name})
  add_dependencies(bundling_target ${tgt_name})
  install(FILES ${bundled_tgt_full_name} DESTINATION ${CMAKE_INSTALL_PREFIX}/lib)

  add_library(${bundled_tgt_name} STATIC IMPORTED)

  set_target_properties(${bundled_tgt_name}
    PROPERTIES
    IMPORTED_LOCATION ${bundled_tgt_full_name}
    INTERFACE_INCLUDE_DIRECTORIES $<TARGET_PROPERTY:${tgt_name},INTERFACE_INCLUDE_DIRECTORIES>)

  add_dependencies(${bundled_tgt_name} bundling_target)
endfunction()

include_directories("${PROJECT_SOURCE_DIR}/src")
add_subdirectory(src)
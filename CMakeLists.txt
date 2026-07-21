cmake_minimum_required(VERSION 3.25)

project(
  retrace
  VERSION 0.0.0
  DESCRIPTION "Native Linux process tracing and fault injection"
  LANGUAGES C CXX
)

include(CTest)
include(GNUInstallDirs)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)

option(RETRACE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(
  RETRACE_SANITIZER
  "none"
  CACHE STRING
  "Sanitizer configuration: none, address, undefined, address-undefined, or thread"
)
set_property(
  CACHE RETRACE_SANITIZER
  PROPERTY STRINGS none address undefined address-undefined thread
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

configure_file(
  cmake/version.hpp.in
  "${CMAKE_CURRENT_BINARY_DIR}/generated/retrace/version.hpp"
  @ONLY
)

add_library(
  retrace_core STATIC
  src/cli/commands.cpp
  src/cli/trace_commands.cpp
  src/process/process.cpp
  src/trace/reader.cpp
  src/trace/writer.cpp
)
add_library(retrace::core ALIAS retrace_core)

target_include_directories(
  retrace_core
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_compile_features(retrace_core PUBLIC cxx_std_20)
retrace_set_project_warnings(retrace_core)
retrace_enable_sanitizers(retrace_core "${RETRACE_SANITIZER}")

add_executable(retrace src/main.cpp)
target_link_libraries(retrace PRIVATE retrace::core)
retrace_set_project_warnings(retrace)
retrace_enable_sanitizers(retrace "${RETRACE_SANITIZER}")

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

install(TARGETS retrace RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

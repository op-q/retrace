# Top-level build graph: configure language/tooling policy, build the reusable
# C++ core and CLI, then add the injected C runtime, examples, and tests.

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

# The installed CLI finds the runtime relative to its own executable, preserving
# relocatability while still supporting GNUInstallDirs layouts such as lib64.
file(
  RELATIVE_PATH
  RETRACE_INSTALL_LIBDIR_FROM_BINDIR
  "${CMAKE_INSTALL_FULL_BINDIR}"
  "${CMAKE_INSTALL_FULL_LIBDIR}"
)

# These cache options are shared by local presets and GitHub Actions.
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

# Generate the development version header from the CMake project version so the
# CLI and trace metadata use one source of truth.
configure_file(
  cmake/version.hpp.in
  "${CMAKE_CURRENT_BINARY_DIR}/generated/retrace/version.hpp"
  @ONLY
)

# Most behavior lives in a library. The tiny executable and unit tests both link
# this target, ensuring they exercise the same implementation.
add_library(
  retrace_core STATIC
  src/cli/commands.cpp
  src/cli/trace_commands.cpp
  src/process/process.cpp
  src/process/runtime_channel.cpp
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

add_subdirectory(runtime)

# Build-tree binaries use the exact generated-library path. Installed binaries
# use the configured relative libdir so they never fall back to a private build
# artifact that merely happens to remain on the developer's machine.
target_compile_definitions(
  retrace_core
  PRIVATE
    RETRACE_BUILD_EXECUTABLE_DIRECTORY="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    RETRACE_BUILD_RUNTIME_LIBRARY_PATH="$<TARGET_FILE:retrace_runtime>"
    RETRACE_INSTALL_LIBDIR_FROM_BINDIR="${RETRACE_INSTALL_LIBDIR_FROM_BINDIR}"
)
add_dependencies(retrace_core retrace_runtime)

add_subdirectory(examples)

# include(CTest) defines BUILD_TESTING; consumers may disable the entire test
# graph without changing production targets.
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

install(TARGETS retrace RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(
  FILES include/retrace/runtime_protocol.h
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/retrace"
)

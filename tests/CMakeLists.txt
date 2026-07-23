add_executable(retrace_cli_test unit/cli_test.cpp)
target_link_libraries(retrace_cli_test PRIVATE retrace::core)
target_compile_definitions(
  retrace_cli_test
  PRIVATE RETRACE_SIGNAL_FIXTURE_PATH="$<TARGET_FILE:retrace_signal_fixture>"
)
retrace_set_project_warnings(retrace_cli_test)
retrace_enable_sanitizers(retrace_cli_test "${RETRACE_SANITIZER}")

add_test(NAME cli.unit COMMAND retrace_cli_test)

add_executable(retrace_trace_test unit/trace_test.cpp)
target_link_libraries(retrace_trace_test PRIVATE retrace::core)
retrace_set_project_warnings(retrace_trace_test)
retrace_enable_sanitizers(retrace_trace_test "${RETRACE_SANITIZER}")

add_test(NAME trace.unit COMMAND retrace_trace_test)

add_executable(retrace_trace_reader_test unit/trace_reader_test.cpp)
target_link_libraries(retrace_trace_reader_test PRIVATE retrace::core)
retrace_set_project_warnings(retrace_trace_reader_test)
retrace_enable_sanitizers(retrace_trace_reader_test "${RETRACE_SANITIZER}")

add_test(NAME trace-reader.unit COMMAND retrace_trace_reader_test)

add_executable(retrace_stream_fixture fixtures/stream_fixture.c)
retrace_set_project_warnings(retrace_stream_fixture)
retrace_enable_sanitizers(retrace_stream_fixture "${RETRACE_SANITIZER}")

add_executable(retrace_signal_fixture fixtures/signal_fixture.c)
retrace_set_project_warnings(retrace_signal_fixture)
retrace_enable_sanitizers(retrace_signal_fixture "${RETRACE_SANITIZER}")
add_dependencies(retrace_cli_test retrace_signal_fixture)

add_executable(retrace_runtime_test runtime/runtime_test.c)
target_include_directories(
  retrace_runtime_test
  PRIVATE "${PROJECT_SOURCE_DIR}/include"
)
target_compile_definitions(
  retrace_runtime_test
  PRIVATE RETRACE_RUNTIME_LIBRARY_PATH="$<TARGET_FILE:retrace_runtime>"
)
target_link_libraries(retrace_runtime_test PRIVATE "${CMAKE_DL_LIBS}")
retrace_set_project_warnings(retrace_runtime_test)
retrace_enable_sanitizers(retrace_runtime_test "${RETRACE_SANITIZER}")
add_dependencies(retrace_runtime_test retrace_runtime)

add_executable(retrace_process_test integration/process_test.cpp)
target_link_libraries(retrace_process_test PRIVATE retrace::core)
target_compile_definitions(
  retrace_process_test
  PRIVATE
    RETRACE_SIGNAL_FIXTURE_PATH="$<TARGET_FILE:retrace_signal_fixture>"
    RETRACE_STREAM_FIXTURE_PATH="$<TARGET_FILE:retrace_stream_fixture>"
)
add_dependencies(retrace_process_test retrace_signal_fixture retrace_stream_fixture)
retrace_set_project_warnings(retrace_process_test)
retrace_enable_sanitizers(retrace_process_test "${RETRACE_SANITIZER}")

add_test(NAME process.integration COMMAND retrace_process_test)
add_test(NAME runtime.handshake COMMAND retrace_runtime_test)

add_test(NAME cli.version COMMAND retrace version)
set_tests_properties(
  cli.version
  PROPERTIES PASS_REGULAR_EXPRESSION "^retrace ${PROJECT_VERSION}-dev"
)

add_test(NAME cli.run.echo COMMAND retrace run -- /bin/echo retrace-smoke)
set_tests_properties(
  cli.run.echo
  PROPERTIES PASS_REGULAR_EXPRESSION "^retrace-smoke"
)

add_test(
  NAME cli.run.stream-fixture
  COMMAND retrace run -- $<TARGET_FILE:retrace_stream_fixture>
)
set_tests_properties(
  cli.run.stream-fixture
  PROPERTIES PASS_REGULAR_EXPRESSION "fixture: stdout"
)

add_test(
  NAME cli.run.example-normal
  COMMAND retrace run -- $<TARGET_FILE:retrace_example_normal>
)
set_tests_properties(
  cli.run.example-normal
  PROPERTIES PASS_REGULAR_EXPRESSION "normal example: stdout"
)

# Intentionally crashing an instrumented target adds sanitizer-runtime behavior
# to a recorder test. Regular GCC and Clang CI still exercise the crash path.
if(RETRACE_SANITIZER STREQUAL "none")
  add_test(
    NAME cli.run.example-crash
    COMMAND
      ${CMAKE_COMMAND}
      -DRETRACE_EXECUTABLE=$<TARGET_FILE:retrace>
      -DCRASH_EXAMPLE_EXECUTABLE=$<TARGET_FILE:retrace_example_crash>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_crash_example.cmake
  )
endif()

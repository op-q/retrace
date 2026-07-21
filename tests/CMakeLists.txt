add_executable(retrace_cli_test unit/cli_test.cpp)
target_link_libraries(retrace_cli_test PRIVATE retrace::core)
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

add_executable(retrace_process_test integration/process_test.cpp)
target_link_libraries(retrace_process_test PRIVATE retrace::core)
target_compile_definitions(
  retrace_process_test
  PRIVATE RETRACE_STREAM_FIXTURE_PATH="$<TARGET_FILE:retrace_stream_fixture>"
)
add_dependencies(retrace_process_test retrace_stream_fixture)
retrace_set_project_warnings(retrace_process_test)
retrace_enable_sanitizers(retrace_process_test "${RETRACE_SANITIZER}")

add_test(NAME process.integration COMMAND retrace_process_test)

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

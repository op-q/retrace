# CTest helper for an outcome that is intentionally non-zero. execute_process
# captures the shell-style status and both streams so CTest itself can succeed
# only when the expected crash was observed.

if(NOT DEFINED RETRACE_EXECUTABLE OR NOT DEFINED CRASH_EXAMPLE_EXECUTABLE)
  message(FATAL_ERROR "retrace and crash-example executable paths are required")
endif()

execute_process(
  COMMAND
    "${RETRACE_EXECUTABLE}" run -- "${CRASH_EXAMPLE_EXECUTABLE}"
  RESULT_VARIABLE run_status
  OUTPUT_VARIABLE run_output
  ERROR_VARIABLE run_error
)

if(NOT run_status EQUAL 139)
  message(FATAL_ERROR "intentional crash returned ${run_status}, expected 139")
endif()
if(NOT run_error MATCHES "crash example: raising SIGSEGV")
  message(FATAL_ERROR "intentional crash output was not captured")
endif()
if(NOT run_error MATCHES "target terminated by signal 11")
  message(FATAL_ERROR "intentional crash signal was not reported")
endif()

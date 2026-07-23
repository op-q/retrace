add_executable(retrace_example_normal normal.c)
retrace_set_project_warnings(retrace_example_normal)
retrace_enable_sanitizers(retrace_example_normal "${RETRACE_SANITIZER}")

add_executable(retrace_example_crash crash.c)
retrace_set_project_warnings(retrace_example_crash)
retrace_enable_sanitizers(retrace_example_crash "${RETRACE_SANITIZER}")

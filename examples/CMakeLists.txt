# These tiny C programs are both documentation examples and black-box targets
# executed through the finished `retrace run` binary.

add_executable(retrace_example_normal normal.c)
retrace_set_project_warnings(retrace_example_normal)

add_executable(retrace_example_crash crash.c)
retrace_set_project_warnings(retrace_example_crash)

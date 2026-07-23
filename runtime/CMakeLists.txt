add_library(retrace_runtime SHARED runtime.c)

target_include_directories(
  retrace_runtime
  PRIVATE "${PROJECT_SOURCE_DIR}/include"
)
target_compile_features(retrace_runtime PRIVATE c_std_17)
set_target_properties(
  retrace_runtime
  PROPERTIES
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
retrace_set_project_warnings(retrace_runtime)
retrace_enable_sanitizers(retrace_runtime "${RETRACE_SANITIZER}")

install(
  TARGETS retrace_runtime
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)

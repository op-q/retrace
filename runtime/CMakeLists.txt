# Build the injected runtime as a hidden-symbol C17 shared library. Keeping it
# separate from retrace_core prevents the C++ runtime from entering the target.

add_library(retrace_runtime SHARED runtime.c)

function(retrace_configure_runtime target)
  target_include_directories(
    "${target}"
    PRIVATE "${PROJECT_SOURCE_DIR}/include"
  )
  target_compile_features("${target}" PRIVATE c_std_17)
  set_target_properties(
    "${target}"
    PROPERTIES
      C_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN YES
  )
  retrace_set_project_warnings("${target}")
endfunction()

retrace_configure_runtime(retrace_runtime)

# The production preload must be usable inside an ordinary uninstrumented
# target. Sanitizer runtimes cannot safely arrive after an LD_PRELOAD library,
# so sanitizer configurations build a second test-only copy instead.
if(RETRACE_SANITIZER STREQUAL "none")
  set(RETRACE_RUNTIME_TEST_TARGET retrace_runtime PARENT_SCOPE)
else()
  add_library(retrace_runtime_sanitized SHARED runtime.c)
  set_target_properties(
    retrace_runtime_sanitized
    PROPERTIES OUTPUT_NAME retrace_runtime_sanitized
  )
  retrace_configure_runtime(retrace_runtime_sanitized)
  retrace_enable_sanitizers(
    retrace_runtime_sanitized
    "${RETRACE_SANITIZER}"
  )
  set(RETRACE_RUNTIME_TEST_TARGET retrace_runtime_sanitized PARENT_SCOPE)
endif()

install(
  TARGETS retrace_runtime
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)

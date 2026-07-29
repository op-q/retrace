# Apply one reviewed warning policy to every project-owned target. Keeping this
# in a function avoids accidentally adding flags to third-party dependencies.

function(retrace_set_project_warnings target)
  if(MSVC)
    set(warnings /W4 /permissive-)
    if(RETRACE_WARNINGS_AS_ERRORS)
      list(APPEND warnings /WX)
    endif()
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set(
      warnings
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wshadow
      -Wsign-conversion
    )
    if(RETRACE_WARNINGS_AS_ERRORS)
      list(APPEND warnings -Werror)
    endif()
  else()
    message(WARNING "No project warning set for ${CMAKE_CXX_COMPILER_ID}")
  endif()

  target_compile_options("${target}" PRIVATE ${warnings})
endfunction()

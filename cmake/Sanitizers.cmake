# Attach the requested sanitizer to both compilation and linking for one target.
# A cache string (rather than several booleans) makes incompatible modes explicit.

function(retrace_enable_sanitizers target sanitizer)
  if(sanitizer STREQUAL "none")
    return()
  endif()

  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "RETRACE_SANITIZER requires GCC or Clang")
  endif()

  if(sanitizer STREQUAL "address")
    set(flags -fsanitize=address -fno-omit-frame-pointer)
  elseif(sanitizer STREQUAL "undefined")
    set(flags -fsanitize=undefined -fno-omit-frame-pointer)
  elseif(sanitizer STREQUAL "address-undefined")
    set(flags -fsanitize=address,undefined -fno-omit-frame-pointer)
  elseif(sanitizer STREQUAL "thread")
    set(flags -fsanitize=thread -fno-omit-frame-pointer)
  else()
    message(FATAL_ERROR "Unknown RETRACE_SANITIZER value: ${sanitizer}")
  endif()

  target_compile_options("${target}" PRIVATE ${flags})
  target_link_options("${target}" PRIVATE ${flags})
endfunction()

function(pulse_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
    return()
  endif()
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
    -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion
    -Wimplicit-fallthrough)
  if(PULSE_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()

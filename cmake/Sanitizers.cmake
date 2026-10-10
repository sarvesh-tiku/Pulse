# Sanitizers are applied to every Pulse target (library, apps, tests) so that
# instrumented and uninstrumented code are never mixed across the boundary.
if(PULSE_SANITIZER STREQUAL "address")
  set(PULSE_SANITIZER_FLAGS -fsanitize=address -fno-omit-frame-pointer)
elseif(PULSE_SANITIZER STREQUAL "undefined")
  set(PULSE_SANITIZER_FLAGS -fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
elseif(PULSE_SANITIZER STREQUAL "thread")
  set(PULSE_SANITIZER_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
elseif(PULSE_SANITIZER STREQUAL "")
  set(PULSE_SANITIZER_FLAGS "")
else()
  message(FATAL_ERROR "Unknown PULSE_SANITIZER='${PULSE_SANITIZER}'")
endif()

if(PULSE_SANITIZER_FLAGS)
  message(STATUS "Pulse: building with -fsanitize=${PULSE_SANITIZER}")
  # Global so that gtest/benchmark are instrumented consistently too.
  add_compile_options(${PULSE_SANITIZER_FLAGS})
  add_link_options(${PULSE_SANITIZER_FLAGS})
endif()

function(pulse_apply_sanitizer target)
  # Flags are applied globally above; kept as a hook for per-target tweaks.
endfunction()

cmake_minimum_required(VERSION 3.24)
project(pulse VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PULSE_BUILD_TESTS "Build unit/integration tests" ON)
option(PULSE_BUILD_BENCHMARKS "Build Google Benchmark micro-benchmarks and pulse-bench" ON)
option(PULSE_WERROR "Treat warnings as errors" OFF)
option(PULSE_NATIVE "Compile with -march=native / -mcpu=native" OFF)
set(PULSE_SANITIZER "" CACHE STRING "One of: address, undefined, thread, or empty")

include(cmake/CompilerWarnings.cmake)
include(cmake/Sanitizers.cmake)

# ---------------------------------------------------------------------------
# Core library: everything except the executables' main() functions.
# ---------------------------------------------------------------------------
add_library(pulse_core
  src/core/cpu.cpp
  src/core/instrument.cpp
  src/protocol/codec.cpp
  src/protocol/checksum.cpp
  src/orderbook/map_order_book.cpp
  src/market_data/sequencer.cpp
  src/market_data/session_file.cpp
  src/market_data/generator.cpp
  src/market_data/fault_injector.cpp
  src/market_data/rewind.cpp
  src/execution/exchange_simulator.cpp
  src/execution/order_manager.cpp
  src/risk/risk_engine.cpp
  src/portfolio/position.cpp
  src/strategy/strategies.cpp
  src/engine/digest.cpp
  src/engine/trading_core.cpp
  src/engine/pipeline.cpp
  src/telemetry/histogram.cpp
  src/telemetry/dashboard.cpp
  src/net/udp.cpp
  src/net/tcp.cpp
  src/net/retransmit.cpp
)
add_library(pulse::core ALIAS pulse_core)
target_include_directories(pulse_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
pulse_set_warnings(pulse_core)
pulse_apply_sanitizer(pulse_core)
find_package(Threads REQUIRED)
target_link_libraries(pulse_core PUBLIC Threads::Threads)
# Bit-identical floating point across compilers/architectures (no FMA
# contraction), so a seed produces the same session everywhere.
target_compile_options(pulse_core PRIVATE -ffp-contract=off)
if(PULSE_NATIVE)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-march=native" PULSE_HAS_MARCH_NATIVE)
  if(PULSE_HAS_MARCH_NATIVE)
    target_compile_options(pulse_core PUBLIC -march=native)
  else()
    target_compile_options(pulse_core PUBLIC -mcpu=native)
  endif()
endif()

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
add_executable(pulse apps/pulse/main.cpp apps/pulse/cli.cpp)
target_link_libraries(pulse PRIVATE pulse::core)
pulse_set_warnings(pulse)

add_executable(pulse-gen tools/feed_generator/main.cpp)
target_link_libraries(pulse-gen PRIVATE pulse::core)
pulse_set_warnings(pulse-gen)

add_executable(pulse-feed tools/feed_server/main.cpp)
target_link_libraries(pulse-feed PRIVATE pulse::core)
pulse_set_warnings(pulse-feed)

# ---------------------------------------------------------------------------
# Third-party test / benchmark dependencies (fetched, pinned)
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

if(PULSE_BUILD_TESTS)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  enable_testing()
  add_subdirectory(tests)
endif()

if(PULSE_BUILD_BENCHMARKS)
  FetchContent_Declare(benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.1
    GIT_SHALLOW TRUE)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)
  add_subdirectory(benchmarks)
endif()

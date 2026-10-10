add_executable(pulse-micro micro/bench_main.cpp)
target_link_libraries(pulse-micro PRIVATE pulse::core benchmark::benchmark)
pulse_set_warnings(pulse-micro)

add_executable(pulse-bench e2e/pulse_bench.cpp)
target_link_libraries(pulse-bench PRIVATE pulse::core)
pulse_set_warnings(pulse-bench)

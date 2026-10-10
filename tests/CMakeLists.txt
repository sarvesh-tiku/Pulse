include(GoogleTest)

set(PULSE_TEST_SOURCES
  test_codec.cpp
  test_flat_id_map.cpp
  test_order_book.cpp
  test_generator.cpp
  test_sequencer.cpp
  test_feed_handler.cpp
  test_position.cpp
  test_risk.cpp
  test_execution.cpp
  test_concurrency.cpp
  test_pipeline.cpp
)

add_executable(pulse_tests ${PULSE_TEST_SOURCES})
target_link_libraries(pulse_tests PRIVATE pulse::core GTest::gtest_main)
pulse_set_warnings(pulse_tests)
gtest_discover_tests(pulse_tests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 120 PROPERTIES TIMEOUT 600)

add_executable(tape_tests
    test_invariants.cpp
    test_price_time_priority.cpp
    test_partial_fills.cpp
    test_order_types.cpp
    test_cancel_modify.cpp
    test_known_answer_scenarios.cpp
    test_memory_lifecycle.cpp
    test_determinism.cpp
    test_queue_position_and_models.cpp
    test_phase1_remaining.cpp
    test_config.cpp
    test_sim_defects.cpp
)

target_include_directories(tape_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_options(tape_tests PRIVATE ${TAPE_COMMON_FLAGS})
target_link_libraries(tape_tests PRIVATE tape_core Catch2::Catch2WithMain)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(tape_tests PRIVATE ${TAPE_RELEASE_FLAGS})
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(tape_tests PRIVATE ${TAPE_DEBUG_FLAGS})
    target_link_options(tape_tests PRIVATE -fsanitize=address,undefined)
endif()

enable_testing()
add_test(NAME tape_tests COMMAND tape_tests)

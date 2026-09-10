# ─────────────────────────────────────────────────────────────────────────────
#  hydra_tests
#
#  One test binary, several .cpp files. This is safe with the project's
#  current "include the .cpp as if it were a header" pattern: every symbol
#  defined by that chain (matching_engine.cpp, limit_order_book.cpp, ...) is
#  either a class member (implicitly inline) or constexpr/template, so the
#  same chain can be #included from multiple test .cpp files and linked into
#  one binary without ODR/duplicate-symbol errors. Verified empirically
#  before writing any test — see the Phase 1 summary for how.
#
#  When Phase 2 splits these into real .hpp/.cpp pairs, only the #include
#  lines at the top of each test file change; the test bodies do not.
# ─────────────────────────────────────────────────────────────────────────────

add_executable(hydra_tests
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
)

target_include_directories(hydra_tests PRIVATE ${HYDRA_INCLUDE_DIRS})
target_compile_options(hydra_tests PRIVATE ${HYDRA_COMMON_FLAGS})
target_link_libraries(hydra_tests PRIVATE Catch2::Catch2WithMain)

# Same Debug/Release compiler-flag policy as the main targets, so a test
# failure that only reproduces under -O3 (or only under ASan/UBSan) doesn't
# quietly slip past because the test binary is warmer or colder than the
# hydra/hydra_bench binaries it is meant to be characterising.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(hydra_tests PRIVATE ${HYDRA_RELEASE_FLAGS})
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(hydra_tests PRIVATE ${HYDRA_DEBUG_FLAGS})
    target_link_options(hydra_tests PRIVATE -fsanitize=address,undefined)
endif()

enable_testing()
# Registered as a single CTest entry rather than via catch_discover_tests's
# per-TEST_CASE registration: it needs no post-build discovery step (nothing
# to go stale or fail silently in CI), and a failure's exact TEST_CASE/SECTION
# is still reported clearly in the binary's own Catch2 console output, which
# `ctest --output-on-failure` surfaces in full.
add_test(NAME hydra_tests COMMAND hydra_tests)

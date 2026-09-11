cmake_minimum_required(VERSION 3.20)
project(Tape VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(TAPE_COMMON_FLAGS
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND TAPE_COMMON_FLAGS -Wno-interference-size)
endif()

set(TAPE_RELEASE_FLAGS
    -O3
    -march=native
    -mtune=native
    -ffast-math
    -funroll-loops
    -DNDEBUG
)

set(TAPE_DEBUG_FLAGS
    -O0
    -g3
    -fsanitize=address,undefined
    -fno-omit-frame-pointer
)

add_library(tape_core STATIC
    src/price_level.cpp
    src/limit_order_book.cpp
    src/matching_engine.cpp
    src/risk.cpp
    src/gateway.cpp
    src/agents.cpp
    src/analytics.cpp
    src/models.cpp
    src/simulation.cpp
)
target_include_directories(tape_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_options(tape_core PRIVATE ${TAPE_COMMON_FLAGS})
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(tape_core PRIVATE ${TAPE_RELEASE_FLAGS})
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(tape_core PRIVATE ${TAPE_DEBUG_FLAGS})
endif()

function(tape_apply_flags tgt)
    target_link_libraries(${tgt} PRIVATE tape_core)
    target_include_directories(${tgt} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(${tgt} PRIVATE ${TAPE_COMMON_FLAGS})
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_compile_options(${tgt} PRIVATE ${TAPE_RELEASE_FLAGS})
    elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(${tgt} PRIVATE ${TAPE_DEBUG_FLAGS})
        target_link_options(${tgt} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

add_executable(tape main.cpp)
tape_apply_flags(tape)

add_executable(tape_bench main.cpp)
tape_apply_flags(tape_bench)
target_compile_definitions(tape_bench PRIVATE NDEBUG)

add_executable(tape_offline examples/offline_sim.cpp)
tape_apply_flags(tape_offline)

find_package(OpenSSL QUIET)
if(OpenSSL_FOUND OR OPENSSL_FOUND)
    find_package(Threads REQUIRED)
    message(STATUS "  OpenSSL found: ${OPENSSL_VERSION} — building tape_live")
    add_executable(tape_live examples/live_binance.cpp)
    target_include_directories(tape_live PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${OPENSSL_INCLUDE_DIR}
    )
    target_compile_options(tape_live PRIVATE ${TAPE_COMMON_FLAGS} ${TAPE_RELEASE_FLAGS})
    target_link_libraries(tape_live PRIVATE
        OpenSSL::SSL OpenSSL::Crypto
        Threads::Threads
        $<$<PLATFORM_ID:Windows>:ws2_32>
        $<$<PLATFORM_ID:Windows>:crypt32>
    )
else()
    message(STATUS "  OpenSSL NOT found — skipping tape_live")
endif()

option(TAPE_BUILD_TESTS "Build the Catch2 test suite" OFF)
if(TAPE_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.16.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS tape tape_offline DESTINATION bin)
install(DIRECTORY include/tape DESTINATION include)

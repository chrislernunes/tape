cmake_minimum_required(VERSION 3.20)
project(HydraExchange VERSION 1.0.0 LANGUAGES CXX)

# ─────────────────────────────────────────────────────────────────────────────
#  C++20, aggressive optimisation
# ─────────────────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ─────────────────────────────────────────────────────────────────────────────
#  Compiler flags
# ─────────────────────────────────────────────────────────────────────────────
set(HYDRA_COMMON_FLAGS
    -Wall
    -Wextra
    -Wshadow
    -Wno-unused-parameter
    -Wno-pedantic
)

# -Wno-interference-size is GCC-only. Clang treats unknown -W flags as
# errors, so gate it on the actual compiler.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND HYDRA_COMMON_FLAGS -Wno-interference-size)
endif()

set(HYDRA_RELEASE_FLAGS
    -O3
    -march=native           # Enable AVX2/AVX-512 if available
    -mtune=native
    -ffast-math             # Aggressive FP (safe for our use: no NaN/Inf)
    -funroll-loops
    -DNDEBUG
    # Link-time optimisation
    # -flto                 # Uncomment for further ~5-10% speedup
)

set(HYDRA_DEBUG_FLAGS
    -O0
    -g3
    -fsanitize=address,undefined
    -fno-omit-frame-pointer
)

# ─────────────────────────────────────────────────────────────────────────────
#  Include paths
# ─────────────────────────────────────────────────────────────────────────────
set(HYDRA_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/infrastructure
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/matching_engine
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/orderbook
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/gateway
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/market_data
    ${CMAKE_CURRENT_SOURCE_DIR}/simulation
    ${CMAKE_CURRENT_SOURCE_DIR}/simulation/latency_model
    ${CMAKE_CURRENT_SOURCE_DIR}/simulation/execution_model
    ${CMAKE_CURRENT_SOURCE_DIR}/simulation/replay
    ${CMAKE_CURRENT_SOURCE_DIR}/agents
    ${CMAKE_CURRENT_SOURCE_DIR}/analytics
    ${CMAKE_CURRENT_SOURCE_DIR}/api
    ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks
)

# ─────────────────────────────────────────────────────────────────────────────
#  Main executable
# ─────────────────────────────────────────────────────────────────────────────
add_executable(hydra main.cpp)

target_include_directories(hydra PRIVATE ${HYDRA_INCLUDE_DIRS})
target_compile_options(hydra PRIVATE ${HYDRA_COMMON_FLAGS})

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(hydra PRIVATE ${HYDRA_RELEASE_FLAGS})
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(hydra PRIVATE ${HYDRA_DEBUG_FLAGS})
    target_link_options(hydra PRIVATE -fsanitize=address,undefined)
endif()

# ─────────────────────────────────────────────────────────────────────────────
#  Benchmark executable (separate to avoid debug overhead)
# ─────────────────────────────────────────────────────────────────────────────
add_executable(hydra_bench main.cpp)
target_include_directories(hydra_bench PRIVATE ${HYDRA_INCLUDE_DIRS})
target_compile_options(hydra_bench PRIVATE ${HYDRA_COMMON_FLAGS} ${HYDRA_RELEASE_FLAGS})
target_compile_definitions(hydra_bench PRIVATE NDEBUG)

# ─────────────────────────────────────────────────────────────────────────────
#  Optional: Python bindings via pybind11
#  cmake -DBUILD_PYTHON_BINDINGS=ON ..
# ─────────────────────────────────────────────────────────────────────────────
option(BUILD_PYTHON_BINDINGS "Build pybind11 Python module" OFF)

if(BUILD_PYTHON_BINDINGS)
    find_package(pybind11 REQUIRED)
    pybind11_add_module(hydra_py api/python_bindings.cpp)
    target_include_directories(hydra_py PRIVATE ${HYDRA_INCLUDE_DIRS})
    target_compile_options(hydra_py PRIVATE ${HYDRA_COMMON_FLAGS} ${HYDRA_RELEASE_FLAGS})
endif()

# ─────────────────────────────────────────────────────────────────────────────
#  Install
# ─────────────────────────────────────────────────────────────────────────────
install(TARGETS hydra DESTINATION bin)
install(DIRECTORY include/hydra     DESTINATION include)
install(DIRECTORY infrastructure    DESTINATION include/hydra/infrastructure)

# ─────────────────────────────────────────────────────────────────────────────
#  Live Binance feed target — requires OpenSSL
#  MSYS2 install:  pacman -S mingw-w64-ucrt-x86_64-openssl
#  Ubuntu install: sudo apt install libssl-dev
# ─────────────────────────────────────────────────────────────────────────────
find_package(OpenSSL QUIET)

if(OpenSSL_FOUND OR OPENSSL_FOUND)
    message(STATUS "  OpenSSL found: ${OPENSSL_VERSION} — building hydra_live")

    add_executable(hydra_live examples/live_binance.cpp)

    target_include_directories(hydra_live PRIVATE
        ${HYDRA_INCLUDE_DIRS}
        ${OPENSSL_INCLUDE_DIR}
    )

    target_compile_options(hydra_live PRIVATE
        ${HYDRA_COMMON_FLAGS}
        ${HYDRA_RELEASE_FLAGS}
    )

    target_link_libraries(hydra_live PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        $<$<PLATFORM_ID:Windows>:ws2_32>
        $<$<PLATFORM_ID:Windows>:crypt32>
        Threads::Threads
    )

    find_package(Threads REQUIRED)

    message(STATUS "  Build hydra_live with: cmake --build build -j4")
    message(STATUS "  Run:  ./build/hydra_live.exe BTCUSDT")
else()
    message(STATUS "  OpenSSL NOT found — skipping hydra_live target")
    message(STATUS "  To build live feed:")
    message(STATUS "    MSYS2:  pacman -S mingw-w64-ucrt-x86_64-openssl")
    message(STATUS "    Ubuntu: sudo apt install libssl-dev")
endif()

# ─────────────────────────────────────────────────────────────────────────────
#  Tests (Catch2 v3, fetched from source and pinned to an exact tag so the
#  test toolchain is identical on every machine and in CI — no dependency on
#  whatever Catch2 version a given distro's package manager happens to carry).
#
#  Off by default, matching this project's "no external dependencies" promise
#  for a plain build (same convention as BUILD_PYTHON_BINDINGS above); opt in
#  explicitly, exactly as CI does:
#    cmake -B build -DHYDRA_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
#    cmake --build build -j$(nproc)
#    ctest --test-dir build --output-on-failure
# ─────────────────────────────────────────────────────────────────────────────
option(HYDRA_BUILD_TESTS "Build the Catch2 test suite (requires network access once, to fetch Catch2)" OFF)

if(HYDRA_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.16.0   # pinned deliberately; bump with intent, not silently
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)

    enable_testing()
    add_subdirectory(tests)
endif()

# ─────────────────────────────────────────────────────────────────────────────
#  Status report
# ─────────────────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "  ╔═══════════════════════════════════╗")
message(STATUS "  ║  HydraExchange Build Config       ║")
message(STATUS "  ╠═══════════════════════════════════╣")
message(STATUS "  ║  Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  ║  C++ std    : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  ║  Compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  ║  Python API : ${BUILD_PYTHON_BINDINGS}")
message(STATUS "  ║  Tests      : ${HYDRA_BUILD_TESTS}")
message(STATUS "  ╚═══════════════════════════════════╝")
message(STATUS "")
message(STATUS "  Build:   cmake -B build -DCMAKE_BUILD_TYPE=Release")
message(STATUS "           cmake --build build -j$(nproc)")
message(STATUS "  Run:     ./build/hydra --sim --bench")
message(STATUS "")

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  timestamp.hpp — Cross-platform nanosecond clock utilities
//
//  Supports: Linux, macOS, Windows (MSYS2/MinGW/MSVC)
//
//  Clock::now_wall()  — wall-clock nanoseconds since epoch
//  Clock::now_mono()  — monotonic nanoseconds (for latency measurement)
//  Clock::rdtsc()     — raw CPU cycle counter (x86 only)
//  SimClock           — deterministic virtual clock for simulation
//  ScopedTimer        — RAII latency probe
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <ctime>
#include <chrono>
#include <string>
#include <cstdio>

// ── Platform detection ────────────────────────────────────────────────────────
#if defined(_WIN32) || defined(_WIN64) || defined(__MINGW32__) || defined(__MINGW64__)
    #define HYDRA_WINDOWS 1
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #define HYDRA_POSIX 1
#endif

namespace hydra {

// ─────────────────────────────────────────────────────────────────────────────
//  Clock
// ─────────────────────────────────────────────────────────────────────────────
class Clock {
public:
    using ns_t = int64_t;

    // ── Wall clock (nanoseconds since Unix epoch) ────────────────────────────
    [[nodiscard]] static ns_t now_wall() noexcept {
        using namespace std::chrono;
        auto tp = system_clock::now().time_since_epoch();
        return static_cast<ns_t>(duration_cast<nanoseconds>(tp).count());
    }

    // ── Monotonic clock (nanoseconds; best for latency measurement) ──────────
    [[nodiscard]] static ns_t now_mono() noexcept {
#if defined(HYDRA_WINDOWS)
        // QueryPerformanceCounter: sub-microsecond resolution on Windows
        static const double ns_per_tick = []() -> double {
            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);
            return 1'000'000'000.0 / static_cast<double>(freq.QuadPart);
        }();
        LARGE_INTEGER count;
        QueryPerformanceCounter(&count);
        return static_cast<ns_t>(
            static_cast<double>(count.QuadPart) * ns_per_tick);
#else
        struct timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<ns_t>(ts.tv_sec)  * 1'000'000'000LL
             + static_cast<ns_t>(ts.tv_nsec);
#endif
    }

    // ── CPU cycle counter (x86/x64 only) ────────────────────────────────────
#if defined(__x86_64__) || defined(_M_X64) || \
    defined(__i386__)   || defined(_M_IX86)
    [[nodiscard]] static inline uint64_t rdtsc() noexcept {
#if defined(_MSC_VER)
        return __rdtsc();
#else
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        return (static_cast<uint64_t>(hi) << 32) | lo;
#endif
    }
#else
    [[nodiscard]] static inline uint64_t rdtsc() noexcept {
        return static_cast<uint64_t>(now_mono());
    }
#endif

    // ── Timestamp formatting (ISO-8601) ──────────────────────────────────────
    [[nodiscard]] static std::string format_ns(ns_t ns) {
        time_t sec = static_cast<time_t>(ns / 1'000'000'000LL);
        long   rem = static_cast<long> (ns % 1'000'000'000LL);
        struct tm tm_buf{};

        // gmtime_r  = POSIX thread-safe (Linux / macOS)
        // gmtime_s  = Windows thread-safe (note: reversed arg order)
#if defined(HYDRA_WINDOWS)
        gmtime_s(&tm_buf, &sec);
#else
        gmtime_r(&sec, &tm_buf);
#endif

        char buf[64];
        std::snprintf(buf, sizeof(buf),
            "%04d-%02d-%02dT%02d:%02d:%02d.%09ldZ",
            tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
            tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, rem);
        return buf;
    }

    [[nodiscard]] static std::string format_duration_us(ns_t ns) {
        char buf[64];
        std::snprintf(buf, sizeof(buf),
            "%.3f us", static_cast<double>(ns) / 1000.0);
        return buf;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  SimClock — deterministic virtual clock for simulation mode
//
//  Single-threaded by design. The simulation event loop is the sole
//  authority that advances time; all agents and the engine read from it.
// ─────────────────────────────────────────────────────────────────────────────
class SimClock {
public:
    using ns_t = int64_t;

    explicit SimClock(ns_t start_ns = 0) noexcept : current_(start_ns) {}

    [[nodiscard]] ns_t now() const noexcept { return current_; }

    void set(ns_t t)               noexcept { current_ = t; }
    void advance(ns_t delta_ns)    noexcept { current_ += delta_ns; }
    void advance_us(double us)     noexcept { current_ += static_cast<ns_t>(us  * 1e3); }
    void advance_ms(double ms)     noexcept { current_ += static_cast<ns_t>(ms  * 1e6); }
    void advance_sec(double sec)   noexcept { current_ += static_cast<ns_t>(sec * 1e9); }

private:
    ns_t current_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  ScopedTimer — RAII latency probe
//
//  Usage:
//      ScopedTimer t("my_op");
//      do_work();
//      int64_t ns = t.elapsed_ns();
// ─────────────────────────────────────────────────────────────────────────────
struct ScopedTimer {
    using ns_t = Clock::ns_t;
    const char* label;
    ns_t        start;

    explicit ScopedTimer(const char* l) noexcept
        : label(l), start(Clock::now_mono()) {}

    ~ScopedTimer() noexcept {
        (void)(Clock::now_mono() - start); // hook into stats accumulator here
    }

    [[nodiscard]] ns_t elapsed_ns() const noexcept {
        return Clock::now_mono() - start;
    }

    [[nodiscard]] double elapsed_us() const noexcept {
        return static_cast<double>(elapsed_ns()) / 1000.0;
    }
};

} // namespace hydra

// BOTW_DIAG=1 records Cafe FS and sync calls into a fixed buffer (no log I/O on the
// guest path) and prints them when a fatal fault happens. The same switch prints the
// title-screen draw / audio / VPAD counters (title_diag.h) every 10 seconds.
// --trace-hle hides the Windows crash, so this stays off the logging mutex.
#pragma once

#include <atomic>

extern std::atomic<int> g_diag_on_flag;
bool diag_init_slow();

inline bool diag_on() {
    int v = g_diag_on_flag.load(std::memory_order_relaxed);
    if (__builtin_expect(v >= 0, 1)) return v == 1;
    return diag_init_slow();
}

void diag_fs_slow(const char* path, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void diag_sync_slow(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void diag_dump(const char* why);
// The run-5 fault object. +28 decimal is guest 0x26cd1004.
void diag_crash_object();

#define diag_fs(path, ...) \
    do { \
        if (__builtin_expect(diag_on(), 0)) diag_fs_slow(path, __VA_ARGS__); \
    } while (0)

#define diag_sync(...) \
    do { \
        if (__builtin_expect(diag_on(), 0)) diag_sync_slow(__VA_ARGS__); \
    } while (0)

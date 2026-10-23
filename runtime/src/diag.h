// BOTW_DIAG=1 records Cafe FS and sync calls into a fixed buffer (no log I/O on the
// guest path) and prints them when a fatal fault happens. The same switch prints the
// title-screen draw / audio / VPAD counters (title_diag.h) every 10 seconds.
// --trace-hle hides the Windows crash, so this stays off the logging mutex.
#pragma once

bool diag_on();
void diag_fs(const char* path, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void diag_sync(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void diag_dump(const char* why);
// The run-5 fault object. +28 decimal is guest 0x26cd1004.
void diag_crash_object();

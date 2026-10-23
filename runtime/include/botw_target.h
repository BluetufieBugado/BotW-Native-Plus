// Platform macros the Apple sources test, plus a couple of host helpers shared by the runtime.
// On Apple this is TargetConditionals.h. Everywhere else the TARGET_OS_* tests are zero.
#pragma once

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#elif defined(__linux__)
#include <dlfcn.h>
#include <pthread.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#if defined(__MINGW32__) || defined(__MINGW64__)
#include <pthread.h>
#endif
#else
#include <pthread.h>
#endif

#if !defined(__APPLE__)
#define TARGET_OS_IOS 0
#define TARGET_OS_OSX 0
#define TARGET_OS_MAC 0
#define TARGET_OS_TV 0
#define TARGET_OS_WATCH 0
#endif

#include <stdint.h>

inline void botw_set_thread_name(const char* name) {
#if defined(__APPLE__)
    pthread_setname_np(name);
#elif defined(__linux__) || defined(__FreeBSD__) || defined(__MINGW32__) || defined(__MINGW64__)
    pthread_setname_np(pthread_self(), name);
#else
    (void)name;
#endif
}

// Base of the main executable, for log lines that a symbolizer turns back into source locations.
inline uintptr_t botw_image_base() {
#if defined(__APPLE__)
    return (uintptr_t)_dyld_get_image_header(0);
#elif defined(__linux__)
    Dl_info info{};
    if (dladdr((const void*)&botw_image_base, &info) && info.dli_fbase) return (uintptr_t)info.dli_fbase;
    return 0;
#elif defined(_WIN32)
    return (uintptr_t)GetModuleHandleW(nullptr);
#else
    return 0;
#endif
}

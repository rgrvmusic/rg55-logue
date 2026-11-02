#pragma once

/*
 * The unit's own log. Neither host has a console, so what the unit does is
 * written next to its ROMs, in the userfs: <userfs>/Programs/rg55.log, where
 * the unit's config.mk points to with RFX_USERFS_PROGRAMS_PATH.
 *
 * Only a build with RG55_DEBUG writes anything. In every other build the log
 * calls compute their arguments and do nothing, because a flash write from
 * the audio path is not free. Note: the memory wears out on heavy writing.
 */

#include <cstdarg>
#include <cstdio>
#include <cstdint>

#if defined(__linux__)
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

/* Set by the unit's config.mk; the drumlogue's path is only the fallback. */
#ifndef RFX_USERFS_PROGRAMS_PATH
#define RFX_USERFS_PROGRAMS_PATH /var/lib/drumlogued/userfs/Programs
#endif

#define RFX_STRINGIFY_IMPL(value) #value
#define RFX_STRINGIFY(value) RFX_STRINGIFY_IMPL(value)

#define RG55_LOG_PATH RFX_STRINGIFY(RFX_USERFS_PROGRAMS_PATH) "/rg55.log"

namespace rg55 {

inline FILE* logFile() {
    static FILE* file = nullptr;
    static bool opened = false;
    if (!opened) {
        opened = true;
        file = std::fopen(RG55_LOG_PATH, "a");
        if (file) std::setvbuf(file, nullptr, _IOLBF, 0);
    }
    return file;
}

inline long threadId() {
#if defined(__linux__)
    return static_cast<long>(::syscall(SYS_gettid));
#else
    return 0;
#endif
}

/* Monotonic seconds, for timing a load. */
inline double seconds() {
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

#if defined(RG55_DEBUG)

/* Seconds since the first line, so the gaps between events can be read. */
inline double sinceStart() {
    static const double start = seconds();
    return seconds() - start;
}

inline void log(const char* format, ...) {
    FILE* file = logFile();
    if (!file) return;
    std::fprintf(file, "[%ld %8.3f] ", threadId(), sinceStart());
    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fputc('\n', file);
    std::fflush(file);
}

#else

inline void log(const char*, ...) {}

#endif


/* Bytes in a file, or -1 if it cannot be opened. */
inline long fileSize(const char* path) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return -1;
    long size = -1;
    if (std::fseek(file, 0, SEEK_END) == 0) size = std::ftell(file);
    std::fclose(file);
    return size;
}

}  // namespace rg55

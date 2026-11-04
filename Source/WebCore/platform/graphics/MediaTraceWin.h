/*
 * MyBrowser: temporary media pipeline trace for the Windows GStreamer bring-up.
 *
 * vcpkg builds GStreamer in Release with gst_debug=false, so GST_DEBUG logs are
 * compiled out. This writes a few lines to a file instead. Enabled only when the
 * MYBROWSER_MEDIA_TRACE environment variable names a file prefix; each process
 * appends to "<prefix>-<pid>.log". Every call site logs at most 40 times.
 */
#pragma once

#if OS(WINDOWS) && USE(GSTREAMER)

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <process.h>
#include <algorithm>
#include <wtf/MainThread.h>
#include <wtf/MonotonicTime.h>

namespace WebCore {

inline FILE* mediaTraceFile()
{
    static FILE* file = [] () -> FILE* {
        const char* prefix = std::getenv("MYBROWSER_MEDIA_TRACE");
        if (!prefix || !*prefix)
            return nullptr;
        char path[1024];
        std::snprintf(path, sizeof(path), "%s-%d.log", prefix, _getpid());
        return std::fopen(path, "a");
    }();
    return file;
}

inline void mediaTrace(const char* where, const char* format, ...)
{
    FILE* file = mediaTraceFile();
    if (!file)
        return;
    static std::mutex lock;
    std::lock_guard guard(lock);
    std::fprintf(file, "[%s] %s: ", isMainThread() ? "main" : "gst ", where);
    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fputc('\n', file);
    std::fflush(file);
}

// Accumulates durations per name and prints "count, avg, max" for each name about every 2 s.
inline void mediaTraceTiming(const char* name, double milliseconds)
{
    if (!mediaTraceFile())
        return;
    struct Accumulator {
        const char* name { nullptr };
        unsigned count { 0 };
        double total { 0 };
        double max { 0 };
        double windowStart { -1 };
    };
    static std::mutex lock;
    static Accumulator accumulators[32];
    double now = MonotonicTime::now().secondsSinceEpoch().milliseconds();
    Accumulator* accumulator = nullptr;
    {
        std::lock_guard guard(lock);
        for (auto& candidate : accumulators) {
            if (candidate.name == name || !candidate.name) {
                candidate.name = name;
                accumulator = &candidate;
                break;
            }
        }
        if (!accumulator)
            return;
        if (accumulator->windowStart < 0)
            accumulator->windowStart = now;
        accumulator->count++;
        accumulator->total += milliseconds;
        accumulator->max = std::max(accumulator->max, milliseconds);
        if (now - accumulator->windowStart < 2000)
            return;
    }
    double seconds = (now - accumulator->windowStart) / 1000;
    mediaTrace("timing", "%-28s %5.1f/s  avg %6.2f ms  max %6.2f ms", name, accumulator->count / seconds,
        accumulator->total / accumulator->count, accumulator->max);
    std::lock_guard guard(lock);
    *accumulator = { name, 0, 0, 0, now };
}

class MediaTraceScope {
public:
    explicit MediaTraceScope(const char* name)
        : m_name(name)
        , m_start(MonotonicTime::now())
    {
    }
    ~MediaTraceScope() { mediaTraceTiming(m_name, (MonotonicTime::now() - m_start).milliseconds()); }

private:
    const char* m_name;
    MonotonicTime m_start;
};

} // namespace WebCore

#define MEDIA_TRACE_CONCAT_(a, b) a##b
#define MEDIA_TRACE_CONCAT(a, b) MEDIA_TRACE_CONCAT_(a, b)
#define MEDIA_TRACE_TIME(name) WebCore::MediaTraceScope MEDIA_TRACE_CONCAT(mediaTraceScope, __LINE__) { name }

#define MEDIA_TRACE(...) do { \
    static std::atomic<int> mediaTraceCount { 0 }; \
    if (mediaTraceCount++ < 40) \
        WebCore::mediaTrace(__FUNCTION__, __VA_ARGS__); \
} while (0)

#else
#define MEDIA_TRACE(...) do { } while (0)
#define MEDIA_TRACE_TIME(name) do { } while (0)
#endif

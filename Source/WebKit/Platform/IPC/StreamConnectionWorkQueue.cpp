/*
 * Copyright (C) 2021 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "StreamConnectionWorkQueue.h"

#if USE(FOUNDATION)
#include <wtf/AutodrainedPool.h>
#endif
#include <wtf/Borrow.h>

#if OS(WINDOWS)
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <thread>
#endif
#if OS(WINDOWS) && USE(GSTREAMER)
#include <WebCore/MediaTraceWin.h>
#endif

namespace IPC {

#if OS(WINDOWS)
// MyBrowser: hang watchdog for the GPU process work queues. A queue stuck in one task for longer
// than the limit is turned into a crash (with __fastfail, so Windows Error Reporting
// writes a dump with every thread's stack) instead of a silent freeze. Enabled together with the
// media trace (MYBROWSER_MEDIA_TRACE), or with MYBROWSER_HANG_DUMP=<seconds>; 0 turns it off.
namespace {

struct WatchdogSlot {
    std::atomic<int64_t> startMs { 0 };
    std::atomic<const char*> name { nullptr };
};

static std::array<WatchdogSlot, 32> s_watchdogSlots;

static int64_t watchdogNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() + 1;
}

static int64_t watchdogLimitMs()
{
    static int64_t limit = [] {
        if (const char* value = std::getenv("MYBROWSER_HANG_DUMP"))
            return static_cast<int64_t>(std::atoi(value)) * 1000;
        return std::getenv("MYBROWSER_MEDIA_TRACE") ? static_cast<int64_t>(10000) : static_cast<int64_t>(0);
    }();
    return limit;
}

static void watchdogThreadMain()
{
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        int64_t now = watchdogNowMs();
        for (auto& slot : s_watchdogSlots) {
            int64_t start = slot.startMs.load(std::memory_order_acquire);
            if (!start || now - start < watchdogLimitMs())
                continue;
            char message[256];
            _snprintf_s(message, sizeof(message), _TRUNCATE, "MyBrowser hang watchdog: work queue '%s' stuck for %lld ms, crashing for a dump\n",
                slot.name.load() ? slot.name.load() : "?", static_cast<long long>(now - start));
            fputs(message, stderr);
            fflush(stderr);
            __fastfail(7); // FAST_FAIL_FATAL_APP_EXIT
        }
    }
}

static WatchdogSlot* acquireWatchdogSlot(const char* name)
{
    if (watchdogLimitMs() <= 0)
        return nullptr;
    static std::once_flag startOnce;
    std::call_once(startOnce, [] {
        std::thread(watchdogThreadMain).detach();
    });
    static std::atomic<unsigned> nextSlot { 0 };
    unsigned index = nextSlot.fetch_add(1);
    if (index >= s_watchdogSlots.size())
        return nullptr;
    s_watchdogSlots[index].name.store(name);
    return &s_watchdogSlots[index];
}

class WatchdogScope {
public:
    explicit WatchdogScope(WatchdogSlot* slot)
        : m_slot(slot)
    {
        if (m_slot)
            m_slot->startMs.store(watchdogNowMs(), std::memory_order_release);
    }
    ~WatchdogScope()
    {
        if (m_slot)
            m_slot->startMs.store(0, std::memory_order_release);
    }
private:
    WatchdogSlot* m_slot;
};

} // namespace
#endif

StreamConnectionWorkQueue::StreamConnectionWorkQueue(ASCIILiteral name)
    : m_name(name)
{
}

StreamConnectionWorkQueue::~StreamConnectionWorkQueue()
{
    // `StreamConnectionWorkQueue::stopAndWaitForCompletion()` should be called if anything has been dispatched or listened to.
    ASSERT(!m_processingThread);
}

void StreamConnectionWorkQueue::dispatch(WTF::Function<void()>&& function)
{
    {
        Locker locker { m_lock };
        // Currently stream IPC::Connection::Client::didClose is delivered after
        // scheduling the work queue stop. This is ignored, as the client function
        // is not used at the moment. Later on, the closure signal should be set synchronously,
        // not dispatched as a function.
        if (m_shouldQuit)
            return;
        m_functions.append(WTF::move(function));
        if (!m_shouldQuit && !m_processingThread) {
            startProcessingThread();
            return;
        }
    }
    wakeUp();
}

void StreamConnectionWorkQueue::addStreamConnection(StreamServerConnection& connection)
{
    {
        Locker locker { m_lock };
        ASSERT(m_connections.findIf([&connection](StreamServerConnection& other) { return &other == &connection; }) == notFound); // NOLINT
        m_connections.append(connection);
        if (!m_shouldQuit && !m_processingThread) {
            startProcessingThread();
            return;
        }
    }
    wakeUp();
}

void StreamConnectionWorkQueue::removeStreamConnection(StreamServerConnection& connection)
{
    {
        Locker locker { m_lock };
        bool didRemove = m_connections.removeFirstMatching([&connection](StreamServerConnection& other) {
            return &other == &connection;
        });
        ASSERT_UNUSED(didRemove, didRemove);
    }
    wakeUp();
}

void StreamConnectionWorkQueue::stopAndWaitForCompletion(NOESCAPE WTF::Function<void()>&& cleanupFunction)
{
    RefPtr<Thread> processingThread;
    {
        Locker locker { m_lock };
        m_cleanupFunction = WTF::move(cleanupFunction);
        processingThread = m_processingThread;
        m_shouldQuit = true;
    }
    if (!processingThread)
        return;
    ASSERT(Thread::currentSingleton().uid() != processingThread->uid());
    wakeUp();
    processingThread->waitForCompletion();
    {
        Locker locker { m_lock };
        m_processingThread = nullptr;
    }
}

void StreamConnectionWorkQueue::wakeUp()
{
    m_wakeUpSemaphore.signal();
}

void StreamConnectionWorkQueue::startProcessingThread()
{
    SUPPRESS_UNCOUNTED_LAMBDA_CAPTURE auto task = [this]() mutable {
        for (;;) {
            processStreams();
            if (m_shouldQuit) {
                processStreams();
                WTF::Function<void()> cleanup = nullptr;
                {
                    Locker locker { m_lock };
                    cleanup = WTF::move(m_cleanupFunction);

                }
                if (cleanup)
                    cleanup();
                return;
            }
            m_wakeUpSemaphore.wait();
        }
    };
    m_processingThread = Thread::create(m_name, WTF::move(task), ThreadType::Graphics, Thread::QOS::UserInteractive);
}

void StreamConnectionWorkQueue::processStreams()
{
    constexpr size_t defaultMessageLimit = 1000;
#if OS(WINDOWS) && USE(GSTREAMER)
    // MyBrowser: how long each wake-up of a GPU thread keeps it busy (rate x avg = busy ms/s).
    static thread_local const char* busyTraceName = std::strstr(m_name.characters(), "RemoteRenderingBackend")
        ? "GPU: raster thread busy" : "GPU: compositor thread busy";
    MEDIA_TRACE_TIME(busyTraceName);
#endif
    bool hasMoreToProcess = false;
    do {
#if USE(FOUNDATION)
        AutodrainedPool perProcessingIterationPool;
#endif
        Deque<WTF::Function<void()>> functions;
        Vector<Ref<StreamServerConnection>> connections;
        {
            Locker locker { m_lock };
            functions.swap(m_functions);
            connections = m_connections;
        }
#if OS(WINDOWS)
        static thread_local WatchdogSlot* watchdogSlot = acquireWatchdogSlot(m_name.characters());
#endif
        for (auto& function : functions) {
#if OS(WINDOWS)
            WatchdogScope watchdogScope(watchdogSlot);
#endif
            WTF::move(function)();
        }

        hasMoreToProcess = false;
        for (auto& connection : borrow(connections)) {
#if OS(WINDOWS)
            WatchdogScope watchdogScope(watchdogSlot);
#endif
            hasMoreToProcess |= connection->dispatchStreamMessages(defaultMessageLimit) == StreamServerConnection::HasMoreMessages;
        }
    } while (hasMoreToProcess);
}

bool StreamConnectionWorkQueue::isCurrent() const
{
    Locker locker { m_lock };
    return m_processingThread ? m_processingThread->uid() == Thread::currentSingleton().uid() : false;
}

}

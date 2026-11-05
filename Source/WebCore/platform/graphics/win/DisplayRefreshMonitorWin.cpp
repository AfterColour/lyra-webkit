/*
 * Copyright (C) 2020 Sony Interactive Entertainment Inc.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "config.h"
#include "DisplayRefreshMonitorWin.h"

#include <windows.h>
#include <dxgi.h>
#include <wtf/Condition.h>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/ThreadSafeWeakPtr.h>
#include <wtf/Threading.h>

namespace WebCore {

// Waits for the next vertical blank of the primary display. DwmFlush cannot be used: it blocks
// for good in a process without windows on the desktop (the web process), which left pages without
// rendering updates. IDXGIOutput::WaitForVBlank works in any process.
class VBlankWaiter {
public:
    VBlankWaiter()
    {
        using CreateFactoryFunction = HRESULT (WINAPI *)(REFIID, void**);
        HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
        auto createFactory = dxgi ? reinterpret_cast<CreateFactoryFunction>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
        IDXGIFactory1* factory = nullptr;
        if (!createFactory || FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
            return;
        HMONITOR primary = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        IDXGIAdapter1* adapter = nullptr;
        for (UINT a = 0; !m_output && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
            IDXGIOutput* output = nullptr;
            for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
                DXGI_OUTPUT_DESC desc;
                if (!m_output && SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == primary) {
                    m_output = output;
                    continue;
                }
                output->Release();
            }
            adapter->Release();
        }
        factory->Release();
    }

    ~VBlankWaiter()
    {
        if (m_output)
            m_output->Release();
    }

    // False if there is no output to wait on (the caller then sleeps for a frame instead).
    bool wait() { return m_output && SUCCEEDED(m_output->WaitForVBlank()); }

private:
    IDXGIOutput* m_output { nullptr };
};

static FramesPerSecond primaryDisplayRefreshRate()
{
    DEVMODEW mode { };
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
        return mode.dmDisplayFrequency;
    return 60;
}

// One thread per monitor, started once and kept: it sleeps on a condition while nobody needs
// frames, and waits for vblanks while somebody does. Start/stop only flip a flag, so the frequent
// start/stop calls of DisplayRefreshMonitor never lose a vblank or leave the monitor without a thread.
class DisplayRefreshMonitorWin::VSyncThread : public ThreadSafeRefCounted<VSyncThread> {
public:
    static Ref<VSyncThread> create(DisplayRefreshMonitorWin& monitor, FramesPerSecond framesPerSecond)
    {
        Ref thread = adoptRef(*new VSyncThread(monitor, framesPerSecond));
        Thread::create("DisplayRefreshMonitorWin"_s, [thread = thread.copyRef()] {
            thread->run();
        }, WTF::ThreadType::Graphics, WTF::ThreadQOS::UserInteractive)->detach();
        return thread;
    }

    void setActive(bool active)
    {
        {
            Locker locker { m_lock };
            m_active = active;
        }
        m_condition.notifyAll();
    }

    void shutDown()
    {
        {
            Locker locker { m_lock };
            m_shutDown = true;
        }
        m_condition.notifyAll();
    }

private:
    VSyncThread(DisplayRefreshMonitorWin& monitor, FramesPerSecond framesPerSecond)
        : m_monitor(monitor)
        , m_interval(1_s / framesPerSecond)
    {
    }

    bool isActive()
    {
        Locker locker { m_lock };
        return m_active && !m_shutDown;
    }

    void run()
    {
        VBlankWaiter vblank;
        while (true) {
            {
                Locker locker { m_lock };
                while (!m_active && !m_shutDown)
                    m_condition.wait(m_lock);
                if (m_shutDown)
                    return;
            }
            // A wait far longer than a frame means vblanks are not coming (display asleep, odd
            // drivers): fall back to sleeping for a frame from then on.
            auto start = MonotonicTime::now();
            if (m_useVBlank && !vblank.wait())
                m_useVBlank = false;
            if (m_useVBlank && MonotonicTime::now() - start > 250_ms && ++m_slowVBlanks > 2)
                m_useVBlank = false;
            if (!m_useVBlank)
                WTF::sleep(m_interval);
            if (!isActive())
                continue;
            // Coalesce: while the main thread has not handled the previous vblank, skip this one.
            if (m_callbackPending.exchange(true))
                continue;
            RunLoop::mainSingleton().dispatch([thread = Ref { *this }] {
                thread->m_callbackPending = false;
                if (!thread->isActive())
                    return;
                if (RefPtr monitor = thread->m_monitor.get())
                    monitor->displayLinkCallbackFired();
            });
        }
    }

    WeakPtr<DisplayRefreshMonitorWin> m_monitor; // Only used on the main thread.
    Seconds m_interval;
    Lock m_lock;
    Condition m_condition;
    bool m_active WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_shutDown WTF_GUARDED_BY_LOCK(m_lock) { false };
    std::atomic<bool> m_callbackPending { false };
    bool m_useVBlank { true }; // Vsync thread only.
    unsigned m_slowVBlanks { 0 }; // Vsync thread only.
};

RefPtr<DisplayRefreshMonitorWin> DisplayRefreshMonitorWin::create(PlatformDisplayID displayID)
{
    return adoptRef(*new DisplayRefreshMonitorWin(displayID));
}

DisplayRefreshMonitorWin::DisplayRefreshMonitorWin(PlatformDisplayID displayID)
    : DisplayRefreshMonitor(displayID)
    , m_framesPerSecond(primaryDisplayRefreshRate())
    , m_currentUpdate({ 0, m_framesPerSecond })
{
}

DisplayRefreshMonitorWin::~DisplayRefreshMonitorWin()
{
    if (m_vsyncThread)
        m_vsyncThread->shutDown();
}

void DisplayRefreshMonitorWin::displayLinkCallbackFired()
{
    displayLinkFired(m_currentUpdate);
    m_currentUpdate = m_currentUpdate.nextUpdate();
}

bool DisplayRefreshMonitorWin::startNotificationMechanism()
{
    if (!m_vsyncThread)
        m_vsyncThread = VSyncThread::create(*this, m_framesPerSecond);
    m_vsyncThread->setActive(true);
    return true;
}

void DisplayRefreshMonitorWin::stopNotificationMechanism()
{
    if (m_vsyncThread)
        m_vsyncThread->setActive(false);
}

} // namespace WebCore

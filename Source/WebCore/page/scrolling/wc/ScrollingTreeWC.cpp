/*
 * Copyright (C) 2026 Lyra
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ScrollingTreeWC.h"

#if ENABLE(ASYNC_SCROLLING) && ENABLE(SCROLLING_THREAD) && USE(GRAPHICS_LAYER_WC)

#include "AsyncScrollingCoordinator.h"
#include "ScrollingThread.h"
#include "ScrollingTreeFrameHostingNode.h"
#include "ScrollingTreeNodesWC.h"
#include <windows.h>
#include <dxgi.h>
#include <wtf/Condition.h>
#include <wtf/RunLoop.h>
#include <wtf/Threading.h>

namespace WebCore {

namespace {

// Waits for the vertical blank of the primary display (as DisplayRefreshMonitorWin does: DwmFlush
// blocks for good in a process without windows on the desktop).
class ScrollingVBlankWaiter {
public:
    ScrollingVBlankWaiter()
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

    ~ScrollingVBlankWaiter()
    {
        if (m_output)
            m_output->Release();
    }

    bool wait() { return m_output && SUCCEEDED(m_output->WaitForVBlank()); }

private:
    IDXGIOutput* m_output { nullptr };
};

static Seconds primaryDisplayFrameInterval()
{
    DEVMODEW mode { };
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
        return 1_s / mode.dmDisplayFrequency;
    return 1_s / 60;
}

} // namespace

// The display link of the scrolling tree: while the tree is busy (wheel events in the last few
// frames, or scroll animations), calls displayDidRefresh() at every vertical blank. That is what
// runs scroll animations on the scrolling thread and moves layers there when the main thread
// falls behind. Sleeps otherwise; scrolls and animations wake it.
class ScrollingTreeRefreshDriverWC : public ThreadSafeRefCounted<ScrollingTreeRefreshDriverWC> {
public:
    static Ref<ScrollingTreeRefreshDriverWC> create(ScrollingTreeWC& tree)
    {
        Ref driver = adoptRef(*new ScrollingTreeRefreshDriverWC(tree));
        Thread::create("WebCore: Scrolling refresh"_s, [driver = driver.copyRef()] {
            driver->run();
        }, WTF::ThreadType::Graphics, WTF::ThreadQOS::UserInteractive)->detach();
        return driver;
    }

    void wake()
    {
        Locker locker { m_lock };
        if (m_woken)
            return;
        m_woken = true;
        m_condition.notifyOne();
    }

    void stop()
    {
        Locker locker { m_lock };
        m_tree = nullptr;
        m_stopped = true;
        m_condition.notifyOne();
    }

private:
    explicit ScrollingTreeRefreshDriverWC(ScrollingTreeWC& tree)
        : m_tree(&tree)
        , m_interval(primaryDisplayFrameInterval())
    {
    }

    void run()
    {
        ScrollingVBlankWaiter vblank;
        bool useVBlank = true;
        unsigned slowVBlanks = 0;
        unsigned idleFrames = 0;
        // A few idle frames before sleeping: the tree counts as busy for 50 ms after a wheel event.
        constexpr unsigned idleFramesBeforeSleeping = 8;

        while (true) {
            {
                Locker locker { m_lock };
                if (m_stopped)
                    return;
                if (idleFrames >= idleFramesBeforeSleeping && !m_woken) {
                    m_condition.waitFor(m_lock, 1_s, [&] {
                        assertIsHeld(m_lock);
                        return m_woken || m_stopped;
                    });
                    if (m_stopped)
                        return;
                    idleFrames = 0;
                }
                m_woken = false;
            }

            auto start = MonotonicTime::now();
            if (useVBlank && !vblank.wait())
                useVBlank = false;
            if (useVBlank && MonotonicTime::now() - start > 250_ms && ++slowVBlanks > 2)
                useVBlank = false;
            if (!useVBlank)
                WTF::sleep(m_interval);

            RefPtr<ScrollingTreeWC> tree;
            {
                Locker locker { m_lock };
                if (m_stopped)
                    return;
                tree = m_tree;
            }
            if (!tree)
                return;

            if (!tree->hasRecentActivity()) {
                ++idleFrames;
                continue;
            }
            idleFrames = 0;
            tree->displayDidRefresh(tree->displayID());
        }
    }

    Lock m_lock;
    Condition m_condition;
    ScrollingTreeWC* m_tree WTF_GUARDED_BY_LOCK(m_lock);
    bool m_woken WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_stopped WTF_GUARDED_BY_LOCK(m_lock) { false };
    const Seconds m_interval;
};

Ref<ScrollingTreeWC> ScrollingTreeWC::create(AsyncScrollingCoordinator& scrollingCoordinator, Ref<ScrollingLayerSinkWC>&& sink)
{
    return adoptRef(*new ScrollingTreeWC(scrollingCoordinator, WTF::move(sink)));
}

ScrollingTreeWC::ScrollingTreeWC(AsyncScrollingCoordinator& scrollingCoordinator, Ref<ScrollingLayerSinkWC>&& sink)
    : ThreadedScrollingTree(scrollingCoordinator)
    , m_sink(WTF::move(sink))
    , m_refreshDriver(ScrollingTreeRefreshDriverWC::create(*this))
{
}

ScrollingTreeWC::~ScrollingTreeWC()
{
    m_refreshDriver->stop();
}

void ScrollingTreeWC::invalidate()
{
    m_refreshDriver->stop();
    ThreadedScrollingTree::invalidate();
}

Ref<ScrollingTreeNode> ScrollingTreeWC::createScrollingTreeNode(ScrollingNodeType nodeType, ScrollingNodeID nodeID)
{
    switch (nodeType) {
    case ScrollingNodeType::MainFrame:
    case ScrollingNodeType::Subframe:
        return ScrollingTreeFrameScrollingNodeWC::create(*this, nodeType, nodeID);
    case ScrollingNodeType::FrameHosting:
        return ScrollingTreeFrameHostingNode::create(*this, nodeID);
    case ScrollingNodeType::Overflow:
        return ScrollingTreeOverflowScrollingNodeWC::create(*this, nodeID);
    case ScrollingNodeType::OverflowProxy:
        return ScrollingTreeOverflowScrollProxyNodeWC::create(*this, nodeID);
    case ScrollingNodeType::Fixed:
        return ScrollingTreeFixedNodeWC::create(*this, nodeID);
    case ScrollingNodeType::Sticky:
        return ScrollingTreeStickyNodeWC::create(*this, nodeID);
    case ScrollingNodeType::Positioned:
        return ScrollingTreePositionedNodeWC::create(*this, nodeID);
    case ScrollingNodeType::PluginScrolling:
    case ScrollingNodeType::PluginHosting:
        RELEASE_ASSERT_NOT_REACHED();
    }

    RELEASE_ASSERT_NOT_REACHED();
}

void ScrollingTreeWC::addLayerUpdate(std::optional<PlatformLayerIdentifier> layerID, const FloatPoint& point, bool isBoundsOrigin)
{
    if (!layerID)
        return;
    Locker locker { m_layerUpdatesLock };
    auto index = m_layerUpdates.findIf([&](auto& update) {
        return update.layerID == *layerID && update.isBoundsOrigin == isBoundsOrigin;
    });
    if (index != notFound) {
        m_layerUpdates[index].point = point;
        return;
    }
    m_layerUpdates.append({ *layerID, point, isBoundsOrigin });
}

void ScrollingTreeWC::applyLayerPositionsInternal()
{
    assertIsHeld(m_treeLock);
    ThreadedScrollingTree::applyLayerPositionsInternal();

    Vector<ScrollingLayerUpdateWC> updates;
    {
        Locker locker { m_layerUpdatesLock };
        updates = std::exchange(m_layerUpdates, { });
    }
    m_sink->send(WTF::move(updates));
}

RefPtr<ScrollingTreeNode> ScrollingTreeWC::scrollingNodeForPoint(FloatPoint)
{
    // Only frames scroll on the scrolling thread in this port (overflow scrolling and subframes
    // stay on the main thread, which knows them through the event tracking regions).
    return rootNode();
}

void ScrollingTreeWC::scrollingTreeNodeDidScroll(ScrollingTreeScrollingNode& node, ScrollingLayerPositionAction action)
{
    ThreadedScrollingTree::scrollingTreeNodeDidScroll(node, action);
    if (!RunLoop::isMain())
        m_refreshDriver->wake();
}

void ScrollingTreeWC::hasNodeWithAnimatedScrollChanged(bool hasNodeWithAnimatedScroll)
{
    if (hasNodeWithAnimatedScroll) {
        m_refreshDriver->wake();
        didScheduleRenderingUpdate();
    }

    RefPtr scrollingCoordinator = m_scrollingCoordinator;
    if (!scrollingCoordinator)
        return;
    RunLoop::mainSingleton().dispatch([scrollingCoordinator = WTF::move(scrollingCoordinator), hasNodeWithAnimatedScroll] {
        scrollingCoordinator->hasNodeWithAnimatedScrollChanged(hasNodeWithAnimatedScroll);
    });
}

} // namespace WebCore

#endif // ENABLE(ASYNC_SCROLLING) && ENABLE(SCROLLING_THREAD) && USE(GRAPHICS_LAYER_WC)

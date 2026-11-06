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

#pragma once

#if ENABLE(ASYNC_SCROLLING) && ENABLE(SCROLLING_THREAD) && USE(GRAPHICS_LAYER_WC)

#include <WebCore/FloatPoint.h>
#include <WebCore/PlatformLayerIdentifier.h>
#include <wtf/Function.h>
#include <wtf/Lock.h>
#include <wtf/MainThread.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>

namespace WebCore {

class Page;
class ScrollingCoordinator;

// Lyra: threaded scrolling for the WC (GPU process compositor) port.
//
// The scrolling tree moves layers that are already painted: the scrolled contents layer of the
// main frame, fixed and sticky layers. On the scrolling thread these moves go straight to the GPU
// process (Sender), which applies them to its copy of the layer tree and composites again, so
// a busy main thread no longer stops scrolling. Moves the main thread applies itself (when it
// keeps up, in its rendering update) travel with that rendering update instead.
//
// Every batch gets a sequence number. The GPU process keeps, per layer, the number of the last
// batch that moved it, and ignores older positions of that layer: an update of the main thread
// may arrive after newer moves from the scrolling thread.
struct ScrollingLayerUpdateWC {
    PlatformLayerIdentifier layerID;
    FloatPoint point;
    bool isBoundsOrigin { false };
};

class ScrollingLayerSinkWC : public ThreadSafeRefCounted<ScrollingLayerSinkWC> {
public:
    using Sender = Function<void(uint64_t sequence, Vector<ScrollingLayerUpdateWC>&&)>;

    static Ref<ScrollingLayerSinkWC> create() { return adoptRef(*new ScrollingLayerSinkWC); }

    // The drawing area, once the GPU process has the layer tree host (and again after losing it).
    void setSender(Sender&& sender)
    {
        Locker locker { m_lock };
        m_sender = WTF::move(sender);
    }

    bool hasSender() const
    {
        Locker locker { m_lock };
        return !!m_sender;
    }

    // Any thread, from the scrolling tree.
    void send(Vector<ScrollingLayerUpdateWC>&& updates)
    {
        if (updates.isEmpty())
            return;
        Locker locker { m_lock };
        uint64_t sequence = ++m_sequence;
        if (isMainThread()) {
            for (auto& update : updates) {
                auto index = m_mainThreadUpdates.findIf([&](auto& pending) {
                    return pending.layerID == update.layerID && pending.isBoundsOrigin == update.isBoundsOrigin;
                });
                if (index == notFound)
                    m_mainThreadUpdates.append(update);
                else
                    m_mainThreadUpdates[index] = update;
            }
            m_mainThreadSequence = sequence;
            return;
        }
        if (m_sender)
            m_sender(sequence, WTF::move(updates));
    }

    struct MainThreadUpdates {
        uint64_t sequence { 0 };
        Vector<ScrollingLayerUpdateWC> updates;
    };

    // Main thread, while the drawing area builds a layer tree update. The update stands for the
    // state of the scrolling tree when the main thread last applied it.
    MainThreadUpdates takeMainThreadUpdates()
    {
        Locker locker { m_lock };
        if (m_mainThreadUpdates.isEmpty())
            return { m_sequence, { } };
        return { m_mainThreadSequence, std::exchange(m_mainThreadUpdates, { }) };
    }

private:
    ScrollingLayerSinkWC() = default;

    mutable Lock m_lock;
    Sender m_sender WTF_GUARDED_BY_LOCK(m_lock);
    uint64_t m_sequence WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    uint64_t m_mainThreadSequence WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    Vector<ScrollingLayerUpdateWC> m_mainThreadUpdates WTF_GUARDED_BY_LOCK(m_lock);
};

WEBCORE_EXPORT Ref<ScrollingCoordinator> createScrollingCoordinatorWC(Page&);

} // namespace WebCore

#endif // ENABLE(ASYNC_SCROLLING) && ENABLE(SCROLLING_THREAD) && USE(GRAPHICS_LAYER_WC)

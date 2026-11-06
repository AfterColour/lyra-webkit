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
#include "ScrollingLayerSinkWC.h"

#if ENABLE(ASYNC_SCROLLING) && ENABLE(SCROLLING_THREAD) && USE(GRAPHICS_LAYER_WC)

#include "ScrollingTreeWC.h"
#include "ThreadedScrollingCoordinator.h"

namespace WebCore {

// Lyra: the scrolling coordinator of the WC port. Wheel events are handled on the scrolling
// thread (EventDispatcher), which moves the painted layers through ScrollingLayerSinkWC.
class ScrollingCoordinatorWC final : public ThreadedScrollingCoordinator {
public:
    static Ref<ScrollingCoordinatorWC> create(Page& page)
    {
        return adoptRef(*new ScrollingCoordinatorWC(page));
    }

    ~ScrollingCoordinatorWC() final = default;

private:
    explicit ScrollingCoordinatorWC(Page& page)
        : ThreadedScrollingCoordinator(&page)
        , m_sink(ScrollingLayerSinkWC::create())
    {
        setScrollingTree(ScrollingTreeWC::create(*this, m_sink.copyRef()));
    }

    ScrollingLayerSinkWC* scrollingLayerSinkWC() final { return m_sink.ptr(); }

    const Ref<ScrollingLayerSinkWC> m_sink;
};

Ref<ScrollingCoordinator> createScrollingCoordinatorWC(Page& page)
{
    return ScrollingCoordinatorWC::create(page);
}

} // namespace WebCore

#endif // ENABLE(ASYNC_SCROLLING) && ENABLE(SCROLLING_THREAD) && USE(GRAPHICS_LAYER_WC)

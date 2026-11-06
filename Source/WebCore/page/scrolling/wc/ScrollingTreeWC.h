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

#include "ScrollingLayerSinkWC.h"
#include "ThreadedScrollingTree.h"

namespace WebCore {

class ScrollingTreeRefreshDriverWC;

// Lyra: the scrolling tree of the WC port. Nodes do not own platform layers here (those live in
// the GPU process); they know the layers by identifier and report where they move them with
// addLayerUpdate(). The moves are sent in one batch at the end of each layer position pass.
class ScrollingTreeWC final : public ThreadedScrollingTree {
public:
    static Ref<ScrollingTreeWC> create(AsyncScrollingCoordinator&, Ref<ScrollingLayerSinkWC>&&);
    virtual ~ScrollingTreeWC();

    // While the tree applies layer positions (with the tree lock held).
    void addLayerUpdate(std::optional<PlatformLayerIdentifier>, const FloatPoint&, bool isBoundsOrigin = false);

    void invalidate() final;

private:
    ScrollingTreeWC(AsyncScrollingCoordinator&, Ref<ScrollingLayerSinkWC>&&);

    Ref<ScrollingTreeNode> createScrollingTreeNode(ScrollingNodeType, ScrollingNodeID) final;
    void applyLayerPositionsInternal() final;
    RefPtr<ScrollingTreeNode> scrollingNodeForPoint(FloatPoint) final;
    void scrollingTreeNodeDidScroll(ScrollingTreeScrollingNode&, ScrollingLayerPositionAction = ScrollingLayerPositionAction::Sync) final;
    void hasNodeWithAnimatedScrollChanged(bool) final;

    const Ref<ScrollingLayerSinkWC> m_sink;
    const Ref<ScrollingTreeRefreshDriverWC> m_refreshDriver;

    Lock m_layerUpdatesLock;
    Vector<ScrollingLayerUpdateWC> m_layerUpdates WTF_GUARDED_BY_LOCK(m_layerUpdatesLock);
};

} // namespace WebCore

#endif // ENABLE(ASYNC_SCROLLING) && ENABLE(SCROLLING_THREAD) && USE(GRAPHICS_LAYER_WC)

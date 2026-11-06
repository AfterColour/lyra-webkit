/*
 * Copyright (C) 2018, 2024 Igalia S.L.
 * Copyright (C) 2026 Lyra (Windows WC port of the threaded scrolling tree)
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

#include "ScrollingTreeFixedNode.h"
#include "ScrollingTreeFrameScrollingNode.h"
#include "ScrollingTreeOverflowScrollProxyNode.h"
#include "ScrollingTreeOverflowScrollingNode.h"
#include "ScrollingTreePositionedNode.h"
#include "ScrollingTreeStickyNode.h"
#include <wtf/Markable.h>

namespace WebCore {

// Lyra: scrolling tree nodes of the WC port (after the Coordinated Graphics ones). They keep the
// identifiers of their layers and report layer moves to ScrollingTreeWC.

class ScrollingTreeScrollingNodeDelegateWC;

class ScrollingTreeFrameScrollingNodeWC final : public ScrollingTreeFrameScrollingNode {
public:
    static Ref<ScrollingTreeFrameScrollingNode> create(ScrollingTree&, ScrollingNodeType, ScrollingNodeID);
    virtual ~ScrollingTreeFrameScrollingNodeWC();

private:
    ScrollingTreeFrameScrollingNodeWC(ScrollingTree&, ScrollingNodeType, ScrollingNodeID);

    ScrollingTreeScrollingNodeDelegateWC& delegate() const;

    bool commitStateBeforeChildren(const ScrollingStateNode&) override;
    WheelEventHandlingResult handleWheelEvent(const PlatformWheelEvent&, EventTargeting) override;
    void currentScrollPositionChanged(ScrollType, ScrollingLayerPositionAction) override;
    void repositionScrollingLayers() override;
    void repositionRelatedLayers() override;

    Markable<PlatformLayerIdentifier> m_rootContentsLayer;
    Markable<PlatformLayerIdentifier> m_counterScrollingLayer;
    Markable<PlatformLayerIdentifier> m_insetClipLayer;
    Markable<PlatformLayerIdentifier> m_contentShadowLayer;
    Markable<PlatformLayerIdentifier> m_headerLayer;
    Markable<PlatformLayerIdentifier> m_footerLayer;
};

class ScrollingTreeOverflowScrollingNodeWC final : public ScrollingTreeOverflowScrollingNode {
public:
    static Ref<ScrollingTreeOverflowScrollingNode> create(ScrollingTree&, ScrollingNodeID);
    virtual ~ScrollingTreeOverflowScrollingNodeWC();

private:
    ScrollingTreeOverflowScrollingNodeWC(ScrollingTree&, ScrollingNodeID);

    ScrollingTreeScrollingNodeDelegateWC& delegate() const;

    bool commitStateBeforeChildren(const ScrollingStateNode&) override;
    void repositionScrollingLayers() override;
    WheelEventHandlingResult handleWheelEvent(const PlatformWheelEvent&, EventTargeting) override;
};

class ScrollingTreeFixedNodeWC final : public ScrollingTreeFixedNode {
public:
    static Ref<ScrollingTreeFixedNodeWC> create(ScrollingTree&, ScrollingNodeID);
    virtual ~ScrollingTreeFixedNodeWC();

private:
    ScrollingTreeFixedNodeWC(ScrollingTree&, ScrollingNodeID);

    ScrollingPlatformLayer* layer() const override { return nullptr; }
    bool commitStateBeforeChildren(const ScrollingStateNode&) override;
    void applyLayerPositions() override;

    Markable<PlatformLayerIdentifier> m_layerID;
};

class ScrollingTreeStickyNodeWC final : public ScrollingTreeStickyNode {
public:
    static Ref<ScrollingTreeStickyNodeWC> create(ScrollingTree&, ScrollingNodeID);
    virtual ~ScrollingTreeStickyNodeWC();

private:
    ScrollingTreeStickyNodeWC(ScrollingTree&, ScrollingNodeID);

    ScrollingPlatformLayer* layer() const override { return nullptr; }
    bool commitStateBeforeChildren(const ScrollingStateNode&) override;
    void applyLayerPositions() override;
    FloatPoint layerTopLeft() const override;
    bool hasViewportClippingLayer() const override;

    // With a clipping layer (the sticky box sits in a composited clip of an ancestor), m_layerID
    // is that clipping layer and m_viewportAnchorLayerID the sticky box inside it.
    Markable<PlatformLayerIdentifier> m_layerID;
    Markable<PlatformLayerIdentifier> m_viewportAnchorLayerID;
    FloatPoint m_layerTopLeft;
};

class ScrollingTreePositionedNodeWC final : public ScrollingTreePositionedNode {
public:
    static Ref<ScrollingTreePositionedNodeWC> create(ScrollingTree&, ScrollingNodeID);
    virtual ~ScrollingTreePositionedNodeWC();

    ScrollingPlatformLayer* layer() const override { return nullptr; }

private:
    ScrollingTreePositionedNodeWC(ScrollingTree&, ScrollingNodeID);

    bool commitStateBeforeChildren(const ScrollingStateNode&) override;
    void applyLayerPositions() override;

    Markable<PlatformLayerIdentifier> m_layerID;
};

class ScrollingTreeOverflowScrollProxyNodeWC final : public ScrollingTreeOverflowScrollProxyNode {
public:
    static Ref<ScrollingTreeOverflowScrollProxyNodeWC> create(ScrollingTree&, ScrollingNodeID);
    virtual ~ScrollingTreeOverflowScrollProxyNodeWC();

private:
    ScrollingTreeOverflowScrollProxyNodeWC(ScrollingTree&, ScrollingNodeID);

    ScrollingPlatformLayer* layer() const override { return nullptr; }
    bool commitStateBeforeChildren(const ScrollingStateNode&) override;
    void applyLayerPositions() override;

    Markable<PlatformLayerIdentifier> m_layerID;
};

} // namespace WebCore

#endif // ENABLE(ASYNC_SCROLLING) && ENABLE(SCROLLING_THREAD) && USE(GRAPHICS_LAYER_WC)

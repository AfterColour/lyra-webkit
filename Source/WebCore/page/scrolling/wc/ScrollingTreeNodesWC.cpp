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

#include "config.h"
#include "ScrollingTreeNodesWC.h"

#if ENABLE(ASYNC_SCROLLING) && ENABLE(SCROLLING_THREAD) && USE(GRAPHICS_LAYER_WC)

#include "GraphicsLayer.h"
#include "LocalFrameView.h"
#include "ScrollingStateFixedNode.h"
#include "ScrollingStateFrameScrollingNode.h"
#include "ScrollingStateOverflowScrollProxyNode.h"
#include "ScrollingStatePositionedNode.h"
#include "ScrollingStateStickyNode.h"
#include "ScrollingTreeWC.h"
#include "ThreadedScrollingTreeScrollingNodeDelegate.h"
#include "WheelEventTestMonitor.h"

namespace WebCore {

// The WC port commits the scrolling tree with layer identifiers (PlatformLayerIDRepresentation),
// so this is never used; LayerRepresentation still needs it.
ScrollingPlatformLayer* LayerRepresentation::platformLayerFromGraphicsLayer(GraphicsLayer& graphicsLayer)
{
    return graphicsLayer.platformLayer();
}

class ScrollingTreeScrollingNodeDelegateWC final : public ThreadedScrollingTreeScrollingNodeDelegate {
public:
    ScrollingTreeScrollingNodeDelegateWC(ScrollingTreeScrollingNode& scrollingNode, bool scrollAnimatorEnabled)
        : ThreadedScrollingTreeScrollingNodeDelegate(scrollingNode)
        , m_scrollAnimatorEnabled(scrollAnimatorEnabled)
    {
    }

    void updateVisibleLengths()
    {
        m_scrollController.contentsSizeChanged();
    }

    bool handleWheelEvent(const PlatformWheelEvent& wheelEvent)
    {
        auto deferrer = ScrollingTreeWheelEventTestMonitorCompletionDeferrer { *scrollingTree(), scrollingNode()->scrollingNodeID(), WheelEventTestMonitor::DeferReason::HandlingWheelEvent };
        updateUserScrollInProgressForEvent(wheelEvent);
        return m_scrollController.handleWheelEvent(wheelEvent);
    }

private:
    // ScrollingEffectsControllerClient.
    bool scrollAnimationEnabled() const final { return m_scrollAnimatorEnabled; }

    bool m_scrollAnimatorEnabled { false };
};

// MARK: Frames

Ref<ScrollingTreeFrameScrollingNode> ScrollingTreeFrameScrollingNodeWC::create(ScrollingTree& scrollingTree, ScrollingNodeType nodeType, ScrollingNodeID nodeID)
{
    return adoptRef(*new ScrollingTreeFrameScrollingNodeWC(scrollingTree, nodeType, nodeID));
}

ScrollingTreeFrameScrollingNodeWC::ScrollingTreeFrameScrollingNodeWC(ScrollingTree& scrollingTree, ScrollingNodeType nodeType, ScrollingNodeID nodeID)
    : ScrollingTreeFrameScrollingNode(scrollingTree, nodeType, nodeID)
{
    m_delegate = makeUnique<ScrollingTreeScrollingNodeDelegateWC>(*this, downcast<ThreadedScrollingTree>(scrollingTree).scrollAnimatorEnabled());
}

ScrollingTreeFrameScrollingNodeWC::~ScrollingTreeFrameScrollingNodeWC() = default;

ScrollingTreeScrollingNodeDelegateWC& ScrollingTreeFrameScrollingNodeWC::delegate() const
{
    return *static_cast<ScrollingTreeScrollingNodeDelegateWC*>(m_delegate.get());
}

bool ScrollingTreeFrameScrollingNodeWC::commitStateBeforeChildren(const ScrollingStateNode& stateNode)
{
    if (!ScrollingTreeFrameScrollingNode::commitStateBeforeChildren(stateNode))
        return false;

    auto* scrollingStateNode = dynamicDowncast<ScrollingStateFrameScrollingNode>(stateNode);
    if (!scrollingStateNode)
        return false;

    if (scrollingStateNode->hasChangedProperty(ScrollingStateNode::Property::RootContentsLayer))
        m_rootContentsLayer = scrollingStateNode->rootContentsLayer().layerID();
    if (scrollingStateNode->hasChangedProperty(ScrollingStateNode::Property::CounterScrollingLayer))
        m_counterScrollingLayer = scrollingStateNode->counterScrollingLayer().layerID();
    if (scrollingStateNode->hasChangedProperty(ScrollingStateNode::Property::InsetClipLayer))
        m_insetClipLayer = scrollingStateNode->insetClipLayer().layerID();
    if (scrollingStateNode->hasChangedProperty(ScrollingStateNode::Property::ContentShadowLayer))
        m_contentShadowLayer = scrollingStateNode->contentShadowLayer().layerID();
    if (scrollingStateNode->hasChangedProperty(ScrollingStateNode::Property::HeaderLayer))
        m_headerLayer = scrollingStateNode->headerLayer().layerID();
    if (scrollingStateNode->hasChangedProperty(ScrollingStateNode::Property::FooterLayer))
        m_footerLayer = scrollingStateNode->footerLayer().layerID();

    m_delegate->updateFromStateNode(*scrollingStateNode);
    return true;
}

WheelEventHandlingResult ScrollingTreeFrameScrollingNodeWC::handleWheelEvent(const PlatformWheelEvent& wheelEvent, EventTargeting eventTargeting)
{
    if (!canHandleWheelEvent(wheelEvent, eventTargeting))
        return WheelEventHandlingResult::unhandled();

    bool handled = delegate().handleWheelEvent(wheelEvent);
    delegate().updateSnapScrollState();
    return WheelEventHandlingResult::result(handled);
}

void ScrollingTreeFrameScrollingNodeWC::currentScrollPositionChanged(ScrollType scrollType, ScrollingLayerPositionAction action)
{
    ScrollingTreeFrameScrollingNode::currentScrollPositionChanged(scrollType, hasSynchronousScrollingReasons() ? ScrollingLayerPositionAction::Set : action);
}

void ScrollingTreeFrameScrollingNodeWC::repositionScrollingLayers()
{
    static_cast<ScrollingTreeWC&>(*scrollingTree()).addLayerUpdate(scrolledContentsLayer().layerID(), -currentScrollPosition());
}

void ScrollingTreeFrameScrollingNodeWC::repositionRelatedLayers()
{
    RefPtr protectedTree = scrollingTree();
    auto& tree = static_cast<ScrollingTreeWC&>(*protectedTree);
    auto scrollPosition = currentScrollPosition();
    auto layoutViewport = this->layoutViewport();

    if (m_counterScrollingLayer)
        tree.addLayerUpdate(m_counterScrollingLayer.asOptional(), layoutViewport.location());

    auto contentInsets = obscuredContentInsets();
    if (m_insetClipLayer && m_rootContentsLayer) {
        auto insetClipPosition = LocalFrameView::insetClipLayerRect(scrollPosition, totalContentsSize(), contentInsets, sizeForVisibleContent()).location();
        tree.addLayerUpdate(m_insetClipLayer.asOptional(), insetClipPosition);

        auto rootContentsPosition = LocalFrameView::positionForRootContentLayer(scrollPosition, scrollOrigin(), contentInsets, headerHeight());
        tree.addLayerUpdate(m_rootContentsLayer.asOptional(), rootContentsPosition);
        if (m_contentShadowLayer)
            tree.addLayerUpdate(m_contentShadowLayer.asOptional(), rootContentsPosition);
    }

    if (m_headerLayer || m_footerLayer) {
        float horizontalScrollOffsetForBanner = layoutViewport.x();
        if (m_headerLayer)
            tree.addLayerUpdate(m_headerLayer.asOptional(), FloatPoint(horizontalScrollOffsetForBanner, LocalFrameView::yPositionForHeaderLayer(scrollPosition, contentInsets.top())));
        if (m_footerLayer)
            tree.addLayerUpdate(m_footerLayer.asOptional(), FloatPoint(horizontalScrollOffsetForBanner, LocalFrameView::yPositionForFooterLayer(scrollPosition, contentInsets.top(), totalContentsSize().height(), footerHeight())));
    }

    delegate().updateVisibleLengths();
}

// MARK: Overflow scrolling (stays on the main thread in this port; kept for completeness)

Ref<ScrollingTreeOverflowScrollingNode> ScrollingTreeOverflowScrollingNodeWC::create(ScrollingTree& scrollingTree, ScrollingNodeID nodeID)
{
    return adoptRef(*new ScrollingTreeOverflowScrollingNodeWC(scrollingTree, nodeID));
}

ScrollingTreeOverflowScrollingNodeWC::ScrollingTreeOverflowScrollingNodeWC(ScrollingTree& scrollingTree, ScrollingNodeID nodeID)
    : ScrollingTreeOverflowScrollingNode(scrollingTree, nodeID)
{
    m_delegate = makeUnique<ScrollingTreeScrollingNodeDelegateWC>(*this, downcast<ThreadedScrollingTree>(scrollingTree).scrollAnimatorEnabled());
}

ScrollingTreeOverflowScrollingNodeWC::~ScrollingTreeOverflowScrollingNodeWC() = default;

ScrollingTreeScrollingNodeDelegateWC& ScrollingTreeOverflowScrollingNodeWC::delegate() const
{
    return *static_cast<ScrollingTreeScrollingNodeDelegateWC*>(m_delegate.get());
}

bool ScrollingTreeOverflowScrollingNodeWC::commitStateBeforeChildren(const ScrollingStateNode& stateNode)
{
    if (!ScrollingTreeOverflowScrollingNode::commitStateBeforeChildren(stateNode))
        return false;

    m_delegate->updateFromStateNode(downcast<ScrollingStateScrollingNode>(stateNode));
    return true;
}

void ScrollingTreeOverflowScrollingNodeWC::repositionScrollingLayers()
{
    static_cast<ScrollingTreeWC&>(*scrollingTree()).addLayerUpdate(scrollContainerLayer().layerID(), currentScrollOffset(), true);
    delegate().updateVisibleLengths();
}

WheelEventHandlingResult ScrollingTreeOverflowScrollingNodeWC::handleWheelEvent(const PlatformWheelEvent& wheelEvent, EventTargeting eventTargeting)
{
    if (!canHandleWheelEvent(wheelEvent, eventTargeting))
        return WheelEventHandlingResult::unhandled();

    return WheelEventHandlingResult::result(delegate().handleWheelEvent(wheelEvent));
}

// MARK: Fixed

Ref<ScrollingTreeFixedNodeWC> ScrollingTreeFixedNodeWC::create(ScrollingTree& scrollingTree, ScrollingNodeID nodeID)
{
    return adoptRef(*new ScrollingTreeFixedNodeWC(scrollingTree, nodeID));
}

ScrollingTreeFixedNodeWC::ScrollingTreeFixedNodeWC(ScrollingTree& scrollingTree, ScrollingNodeID nodeID)
    : ScrollingTreeFixedNode(scrollingTree, nodeID)
{
}

ScrollingTreeFixedNodeWC::~ScrollingTreeFixedNodeWC() = default;

bool ScrollingTreeFixedNodeWC::commitStateBeforeChildren(const ScrollingStateNode& stateNode)
{
    if (!is<ScrollingStateFixedNode>(stateNode))
        return false;

    if (stateNode.hasChangedProperty(ScrollingStateNode::Property::Layer))
        m_layerID = stateNode.layer().layerID();

    return ScrollingTreeFixedNode::commitStateBeforeChildren(stateNode);
}

void ScrollingTreeFixedNodeWC::applyLayerPositions()
{
    auto layerPosition = computeLayerPosition();
    static_cast<ScrollingTreeWC&>(*scrollingTree()).addLayerUpdate(m_layerID.asOptional(), layerPosition - m_constraints.alignmentOffset());
}

// MARK: Sticky

Ref<ScrollingTreeStickyNodeWC> ScrollingTreeStickyNodeWC::create(ScrollingTree& scrollingTree, ScrollingNodeID nodeID)
{
    return adoptRef(*new ScrollingTreeStickyNodeWC(scrollingTree, nodeID));
}

ScrollingTreeStickyNodeWC::ScrollingTreeStickyNodeWC(ScrollingTree& scrollingTree, ScrollingNodeID nodeID)
    : ScrollingTreeStickyNode(scrollingTree, nodeID)
{
}

ScrollingTreeStickyNodeWC::~ScrollingTreeStickyNodeWC() = default;

bool ScrollingTreeStickyNodeWC::commitStateBeforeChildren(const ScrollingStateNode& stateNode)
{
    if (stateNode.hasChangedProperty(ScrollingStateNode::Property::Layer))
        m_layerID = stateNode.layer().layerID();
    if (stateNode.hasChangedProperty(ScrollingStateNode::Property::ViewportAnchorLayer)) {
        if (auto* stickyStateNode = dynamicDowncast<ScrollingStateStickyNode>(stateNode))
            m_viewportAnchorLayerID = stickyStateNode->viewportAnchorLayer().layerID();
    }

    if (!ScrollingTreeStickyNode::commitStateBeforeChildren(stateNode))
        return false;

    // Where the main thread put the sticky box in its last layout.
    m_layerTopLeft = m_constraints.layerPositionAtLastLayout() - m_constraints.alignmentOffset();
    return true;
}

bool ScrollingTreeStickyNodeWC::hasViewportClippingLayer() const
{
    return m_viewportAnchorLayerID && m_viewportAnchorLayerID != m_layerID;
}

// As ScrollingTreeStickyNodeCocoa (and ScrollingStateStickyNode on the main thread): with a
// clipping layer, the clip moves to the clipping position and the box moves inside the clip.
void ScrollingTreeStickyNodeWC::applyLayerPositions()
{
    auto& tree = static_cast<ScrollingTreeWC&>(*scrollingTree());
    auto anchorLayerPosition = computeConstrainingRectAndAnchorLayerPosition().second;
    if (hasViewportClippingLayer()) {
        auto clippingLayerPosition = computeClippingLayerPosition();
        tree.addLayerUpdate(m_layerID.asOptional(), clippingLayerPosition);
        anchorLayerPosition.moveBy(-clippingLayerPosition);
        m_layerTopLeft = anchorLayerPosition - m_constraints.alignmentOffset();
        tree.addLayerUpdate(m_viewportAnchorLayerID.asOptional(), m_layerTopLeft);
        return;
    }
    m_layerTopLeft = anchorLayerPosition - m_constraints.alignmentOffset();
    tree.addLayerUpdate((m_viewportAnchorLayerID ? m_viewportAnchorLayerID : m_layerID).asOptional(), m_layerTopLeft);
}

FloatPoint ScrollingTreeStickyNodeWC::layerTopLeft() const
{
    return m_layerTopLeft + m_constraints.alignmentOffset();
}

// MARK: Positioned (relative to an overflow scroller)

Ref<ScrollingTreePositionedNodeWC> ScrollingTreePositionedNodeWC::create(ScrollingTree& scrollingTree, ScrollingNodeID nodeID)
{
    return adoptRef(*new ScrollingTreePositionedNodeWC(scrollingTree, nodeID));
}

ScrollingTreePositionedNodeWC::ScrollingTreePositionedNodeWC(ScrollingTree& scrollingTree, ScrollingNodeID nodeID)
    : ScrollingTreePositionedNode(scrollingTree, nodeID)
{
}

ScrollingTreePositionedNodeWC::~ScrollingTreePositionedNodeWC() = default;

bool ScrollingTreePositionedNodeWC::commitStateBeforeChildren(const ScrollingStateNode& stateNode)
{
    if (stateNode.hasChangedProperty(ScrollingStateNode::Property::Layer))
        m_layerID = stateNode.layer().layerID();
    return ScrollingTreePositionedNode::commitStateBeforeChildren(stateNode);
}

void ScrollingTreePositionedNodeWC::applyLayerPositions()
{
    FloatSize delta = scrollDeltaSinceLastCommit();
    FloatPoint layerPosition = m_constraints.layerPositionAtLastLayout() - delta;
    static_cast<ScrollingTreeWC&>(*scrollingTree()).addLayerUpdate(m_layerID.asOptional(), layerPosition - m_constraints.alignmentOffset());
}

// MARK: Overflow scroll proxy

Ref<ScrollingTreeOverflowScrollProxyNodeWC> ScrollingTreeOverflowScrollProxyNodeWC::create(ScrollingTree& scrollingTree, ScrollingNodeID nodeID)
{
    return adoptRef(*new ScrollingTreeOverflowScrollProxyNodeWC(scrollingTree, nodeID));
}

ScrollingTreeOverflowScrollProxyNodeWC::ScrollingTreeOverflowScrollProxyNodeWC(ScrollingTree& scrollingTree, ScrollingNodeID nodeID)
    : ScrollingTreeOverflowScrollProxyNode(scrollingTree, nodeID)
{
}

ScrollingTreeOverflowScrollProxyNodeWC::~ScrollingTreeOverflowScrollProxyNodeWC() = default;

bool ScrollingTreeOverflowScrollProxyNodeWC::commitStateBeforeChildren(const ScrollingStateNode& stateNode)
{
    if (stateNode.hasChangedProperty(ScrollingStateNode::Property::Layer))
        m_layerID = stateNode.layer().layerID();
    return ScrollingTreeOverflowScrollProxyNode::commitStateBeforeChildren(stateNode);
}

void ScrollingTreeOverflowScrollProxyNodeWC::applyLayerPositions()
{
    static_cast<ScrollingTreeWC&>(*scrollingTree()).addLayerUpdate(m_layerID.asOptional(), computeLayerPosition(), true);
}

} // namespace WebCore

#endif // ENABLE(ASYNC_SCROLLING) && ENABLE(SCROLLING_THREAD) && USE(GRAPHICS_LAYER_WC)

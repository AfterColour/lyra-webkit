/*
 * Copyright (C) 2021 Sony Interactive Entertainment Inc.
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

#pragma once

#if USE(GRAPHICS_LAYER_WC)

#include "DrawingArea.h"
#include "GraphicsLayerWC.h"
#include "RemoteWCLayerTreeHostProxy.h"
#include "WCLayerFactory.h"
#include <WebCore/GraphicsLayerFactory.h>
#include <WebCore/Timer.h>

namespace WebCore {
class ScrollingLayerSinkWC;
}

namespace WebKit {

class DrawingAreaWC final
    : public DrawingArea
    , public GraphicsLayerWC::Observer {
public:
    static Ref<DrawingAreaWC> create(WebPage& webPage, const WebPageCreationParameters& parameters)
    {
        return adoptRef(*new DrawingAreaWC(webPage, parameters));
    }

    ~DrawingAreaWC() override;

private:
    DrawingAreaWC(WebPage&, const WebPageCreationParameters&);

    // DrawingArea
    WebCore::GraphicsLayerFactory* graphicsLayerFactory() override;
    void setNeedsDisplay() override;
    void setNeedsDisplayInRect(const WebCore::IntRect&) override;
    void scroll(const WebCore::IntRect& scrollRect, const WebCore::IntSize& scrollDelta) override;
    void updateRenderingWithForcedRepaintAsync(WebPage&, CompletionHandler<void()>&&) override;
    void triggerRenderingUpdate() override;
    bool enterAcceleratedCompositingModeIfNeeded() override { return false; }
    void setLayerTreeStateIsFrozen(bool) override;
    bool layerTreeStateIsFrozen() const override { return m_isRenderingSuspended; }
#if USE(GRAPHICS_LAYER_TEXTURE_MAPPER)    
    void updateGeometry(const WebCore::IntSize&, CompletionHandler<void()>&&) override { }
#endif
    void updateGeometryWC(uint64_t, WebCore::IntSize, float deviceScaleFactor, float intrinsicDeviceScaleFactor) override;
    void setRootCompositingLayer(WebCore::Frame&, WebCore::GraphicsLayer*) override;
    void addRootFrame(WebCore::FrameIdentifier) override;
    void attachViewOverlayGraphicsLayer(WebCore::FrameIdentifier, WebCore::GraphicsLayer*) override;
    void updatePreferences(const WebPreferencesStore&) override;
    bool shouldUseTiledBackingForFrameView(const WebCore::LocalFrameView&) const override;
    void displayDidRefresh(MonotonicTime = MonotonicTime::now()) override;
#if ENABLE(SCROLLING_THREAD)
    // Lyra: wheel scrolling on the scrolling thread (WebCore::ScrollingCoordinatorWC).
    bool supportsAsyncScrolling() const override { return true; }
    void registerScrollingTree() override;
    void unregisterScrollingTree() override;
#endif
    // GraphicsLayerWC::Observer
    void graphicsLayerAdded(GraphicsLayerWC&) override;
    void graphicsLayerRemoved(GraphicsLayerWC&) override;
    void commitLayerUpdateInfo(WCLayerUpdateInfo&&) override;
    RefPtr<WebCore::ImageBuffer> createImageBuffer(WebCore::FloatSize, float deviceScaleFactor) override;

    bool isCompositingMode();
    void updateRendering();
    void sendUpdateAC();
    void sendUpdateNonAC();
    void updateRootLayers();
    void updateRootLayerDeviceScaleFactor(WebCore::GraphicsLayer&);

    struct RootLayerInfo {
        Ref<WebCore::GraphicsLayer> layer;
        RefPtr<WebCore::GraphicsLayer> contentLayer;
        RefPtr<WebCore::GraphicsLayer> viewOverlayRootLayer;
        WebCore::FrameIdentifier frameID;
    };
    RootLayerInfo* rootLayerInfoWithFrameIdentifier(WebCore::FrameIdentifier);

    WebCore::GraphicsLayerClient m_rootLayerClient;
    // Reference counted, not uniquely owned: GPUProcessConnection keeps weak pointers to it as a
    // client, and those only notice the object is gone if it dies through its last deref.
    const Ref<RemoteWCLayerTreeHostProxy> m_remoteWCLayerTreeHostProxy;
    WCLayerFactory m_layerFactory;
    DoublyLinkedList<GraphicsLayerWC> m_liveGraphicsLayers;
    WebCore::Timer m_updateRenderingTimer;
    bool m_isRenderingSuspended { false };
    bool m_hasDeferredRenderingUpdate { false };
    bool m_inUpdateRendering { false };
    bool m_waitDidUpdate { false };
    bool m_isForceRepaintCompletionHandlerDeferred { false };
    WCUpdateInfo m_updateInfo;
    Vector<RootLayerInfo, 1> m_rootLayers;
    const Ref<WorkQueue> m_commitQueue;
    int64_t m_backingStoreStateID { 0 };
    WebCore::Region m_dirtyRegion;
    WebCore::IntRect m_scrollRect;
    WebCore::IntSize m_scrollOffset;
    CompletionHandler<void()> m_forceRepaintCompletionHandler;
    // MyBrowser: tile buffers are reused once the GPU process has uploaded them (the update reply),
    // instead of allocating new shared memory for every repainted tile of every frame.
    // MYBROWSER_DISABLE_TILE_POOL=1 turns this off.
    Vector<RefPtr<WebCore::ImageBuffer>> takeTileBuffers(WCUpdateInfo&);
    void recycleTileBuffers(Vector<RefPtr<WebCore::ImageBuffer>>&&);
    Vector<RefPtr<WebCore::ImageBuffer>> m_freeTileBuffers;
    // MyBrowser: the GPU process crashed or was restarted; resend the whole layer tree.
    void resyncAfterGPUProcessLoss();
    uint64_t m_compositorGeneration { 0 };

#if ENABLE(SCROLLING_THREAD)
    // Lyra: where the scrolling tree reports the layers it moves; the scrolling thread sends its
    // moves straight to the GPU process once the layer tree host exists there.
    WebCore::ScrollingLayerSinkWC* scrollingLayerSink();
    void takeScrollingLayerUpdates(WCUpdateInfo&);
    void updateScrollingLayerSender();
    RefPtr<WebCore::ScrollingLayerSinkWC> m_scrollingLayerSink;
#endif

#if OS(WINDOWS) && USE(GSTREAMER)
    double m_traceUpdateStart { -1 };
#endif
};

} // namespace WebKit

#endif // USE(GRAPHICS_LAYER_WC)

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

#include "config.h"
#include "DrawingAreaWC.h"

#if USE(GRAPHICS_LAYER_WC)

#include "DrawingAreaProxyMessages.h"
#include "GraphicsLayerWC.h"
#include "ImageBufferShareableBitmapBackend.h"
#include "MessageSenderInlines.h"
#include "RemoteRenderingBackendProxy.h"
#include "RemoteWCLayerTreeHostProxy.h"
#include "UpdateInfo.h"
#include "WebFrame.h"
#include "WebPage.h"
#include "WebPageCreationParameters.h"
#include "WebPageInlines.h"
#include "WebPreferencesKeys.h"
#include "WebProcess.h"
#include <WebCore/ImageBuffer.h>
#include <WebCore/MediaTraceWin.h>
#include <cstdlib>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameView.h>
#include <WebCore/Page.h>
#include <WebCore/Settings.h>
#include <wtf/Borrow.h>
#include <wtf/text/MakeString.h>

namespace WebKit {
using namespace WebCore;

DrawingAreaWC::DrawingAreaWC(WebPage& webPage, const WebPageCreationParameters& parameters)
    : DrawingArea(parameters.drawingAreaIdentifier, webPage)
    , m_remoteWCLayerTreeHostProxy(adoptRef(*new RemoteWCLayerTreeHostProxy(webPage, parameters.usesOffscreenRendering)))
    , m_layerFactory(*this)
    , m_updateRenderingTimer(*this, &DrawingAreaWC::updateRendering)
    , m_commitQueue(WorkQueue::create("DrawingAreaWC CommitQueue"_s))
{
    m_remoteWCLayerTreeHostProxy->setDidLoseGPUProcessHandler([this, weakThis = WeakPtr(*this)] {
        if (weakThis)
            resyncAfterGPUProcessLoss();
    });
}

void DrawingAreaWC::resyncAfterGPUProcessLoss()
{
    MEDIA_TRACE("GPU process lost: resending %u layers", static_cast<unsigned>(m_liveGraphicsLayers.size()));
    ++m_compositorGeneration;
    // Buffers of the old GPU process are gone with it.
    m_freeTileBuffers.clear();
    m_updateInfo = { };
    for (auto* layer = m_liveGraphicsLayers.head(); layer; layer = layer->next()) {
        m_updateInfo.addedLayers.append(*layer->primaryLayerID());
        layer->resyncWithNewCompositor();
    }
    triggerRenderingUpdate();
}

DrawingAreaWC::~DrawingAreaWC()
{
    m_remoteWCLayerTreeHostProxy->setDidLoseGPUProcessHandler({ });
    while (auto layer = m_liveGraphicsLayers.removeHead())
        layer->clearObserver();
}

GraphicsLayerFactory* DrawingAreaWC::graphicsLayerFactory()
{
    return &m_layerFactory;
}

void DrawingAreaWC::updateRootLayers()
{
    for (auto& rootLayer : borrow(m_rootLayers).get()) {
        Vector<Ref<GraphicsLayer>> children;
        if (rootLayer.contentLayer) {
            children.append(*rootLayer.contentLayer);
            if (rootLayer.viewOverlayRootLayer)
                children.append(*rootLayer.viewOverlayRootLayer);
        }
        rootLayer.layer->setChildren(WTF::move(children));
    }
    triggerRenderingUpdate();
}

void DrawingAreaWC::setRootCompositingLayer(WebCore::Frame& frame, GraphicsLayer* rootGraphicsLayer)
{
    for (auto& rootLayer : m_rootLayers) {
        if (rootLayer.frameID == frame.frameID())
            rootLayer.contentLayer = rootGraphicsLayer;
    }
    if (rootGraphicsLayer)
        send(Messages::DrawingAreaProxy::EnterAcceleratedCompositingMode(m_backingStoreStateID, { }));
    updateRootLayers();
}

void DrawingAreaWC::addRootFrame(WebCore::FrameIdentifier frameID)
{
    auto layer = GraphicsLayer::create(graphicsLayerFactory(), this->m_rootLayerClient);
    layer->setName(makeString("drawing area root "_s, frameID.toUInt64()));
    layer->setAnchorPoint({ });
    m_rootLayers.append(RootLayerInfo {
        WTF::move(layer),
        nullptr,
        nullptr,
        frameID
    });
}

void DrawingAreaWC::attachViewOverlayGraphicsLayer(WebCore::FrameIdentifier frameID, GraphicsLayer* layer)
{
    if (auto* layerInfo = rootLayerInfoWithFrameIdentifier(frameID)) {
        layerInfo->viewOverlayRootLayer = layer;
        updateRootLayers();
    }
}

static bool gpuPaintedTiles();

void DrawingAreaWC::updatePreferences(const WebPreferencesStore& store)
{
    Settings& settings = m_webPage->corePage()->settings();
    settings.setAcceleratedCompositingForFixedPositionEnabled(settings.acceleratedCompositingEnabled());
    // MyBrowser: with GPU-painted tiles, always composite. Otherwise a page without compositing
    // layers is painted on the CPU into one big bitmap every frame (sendUpdateNonAC), and pages
    // that gain and lose layers flash white while switching between the two modes.
    settings.setForceCompositingMode(store.getBoolValueForKey(WebPreferencesKey::siteIsolationEnabledKey())
        || store.getBoolValueForKey(WebPreferencesKey::forceCompositingModeKey())
        || (settings.acceleratedCompositingEnabled() && gpuPaintedTiles()));
}

bool DrawingAreaWC::shouldUseTiledBackingForFrameView(const WebCore::LocalFrameView& frameView) const
{
    return frameView.frame().isRootFrame();
}

void DrawingAreaWC::setLayerTreeStateIsFrozen(bool isFrozen)
{
    if (m_isRenderingSuspended == isFrozen)
        return;
    m_isRenderingSuspended = isFrozen;
    if (!m_isRenderingSuspended && m_hasDeferredRenderingUpdate) {
        m_hasDeferredRenderingUpdate = false;
        triggerRenderingUpdate();
    }
}

void DrawingAreaWC::updateGeometryWC(uint64_t backingStoreStateID, IntSize viewSize, float deviceScaleFactor, float intrinsicDeviceScaleFactor)
{
    m_backingStoreStateID = backingStoreStateID;
    m_webPage->setDeviceScaleFactor(deviceScaleFactor);
    m_webPage->setIntrinsicDeviceScaleFactor(intrinsicDeviceScaleFactor);
    m_webPage->setSize(viewSize);
}

void DrawingAreaWC::setNeedsDisplay()
{
    m_dirtyRegion = m_webPage->bounds();
    m_scrollRect = { };
    m_scrollOffset = { };
    triggerRenderingUpdate();
}

void DrawingAreaWC::setNeedsDisplayInRect(const IntRect& rect)
{
    if (isCompositingMode())
        return;
    IntRect dirtyRect = rect;
    dirtyRect.intersect(m_webPage->bounds());
    m_dirtyRegion.unite(dirtyRect);
    triggerRenderingUpdate();
}

void DrawingAreaWC::scroll(const IntRect& scrollRect, const IntSize& scrollDelta)
{
    if (isCompositingMode())
        return;

    if (scrollRect != m_scrollRect) {
        // Just repaint the entire current scroll rect, we'll scroll the new rect instead.
        setNeedsDisplayInRect(m_scrollRect);
        m_scrollRect = { };
        m_scrollOffset = { };
    }
    // Get the part of the dirty region that is in the scroll rect.
    Region dirtyRegionInScrollRect = intersect(scrollRect, m_dirtyRegion);
    if (!dirtyRegionInScrollRect.isEmpty()) {
        // There are parts of the dirty region that are inside the scroll rect.
        // We need to subtract them from the region, move them and re-add them.
        m_dirtyRegion.subtract(scrollRect);
        // Move the dirty parts.
        Region movedDirtyRegionInScrollRect = intersect(translate(dirtyRegionInScrollRect, scrollDelta), scrollRect);
        // And add them back.
        m_dirtyRegion.unite(movedDirtyRegionInScrollRect);
    }
    // Compute the scroll repaint region.
    Region scrollRepaintRegion = subtract(scrollRect, translate(scrollRect, scrollDelta));
    m_dirtyRegion.unite(scrollRepaintRegion);
    m_scrollRect = scrollRect;
    m_scrollOffset += scrollDelta;
    triggerRenderingUpdate();
}

void DrawingAreaWC::updateRenderingWithForcedRepaintAsync(WebPage&, CompletionHandler<void()>&& completionHandler)
{
    m_forceRepaintCompletionHandler = WTF::move(completionHandler);
    m_isForceRepaintCompletionHandlerDeferred = m_waitDidUpdate;
    setNeedsDisplay();
}

void DrawingAreaWC::triggerRenderingUpdate()
{
    if (m_isRenderingSuspended || m_waitDidUpdate) {
        m_hasDeferredRenderingUpdate = true;
        return;
    }
    if (m_updateRenderingTimer.isActive())
        return;
    m_updateRenderingTimer.startOneShot(0_s);
}

bool DrawingAreaWC::isCompositingMode()
{
    auto& mainWebFrame = m_webPage->mainWebFrame();
    if (!mainWebFrame.isRootFrame())
        return true;
    return rootLayerInfoWithFrameIdentifier(mainWebFrame.frameID())->contentLayer;
}

void DrawingAreaWC::updateRendering()
{
    if (m_isRenderingSuspended) {
        m_hasDeferredRenderingUpdate = true;
        return;
    }

    // This function is not reentrant, e.g. a rAF callback may force repaint.
    if (m_inUpdateRendering)
        return;
    SetForScope change(m_inUpdateRendering, true);

    ASSERT(!m_waitDidUpdate);
    m_waitDidUpdate = true;

#if OS(WINDOWS) && USE(GSTREAMER)
    // MyBrowser: frame pipeline timings (MYBROWSER_MEDIA_TRACE).
    {
        static double lastStart = -1;
        double now = MonotonicTime::now().secondsSinceEpoch().milliseconds();
        if (lastStart >= 0)
            mediaTraceTiming("WP: rendering update interval", now - lastStart);
        lastStart = now;
        m_traceUpdateStart = now;
    }
#endif

    Ref webPage = m_webPage.get();
    {
        MEDIA_TRACE_TIME("WP: updateRendering (rAF/style/layout)");
        webPage->updateRendering();
    }
    webPage->flushPendingEditorStateUpdate();

    OptionSet<FinalizeRenderingUpdateFlags> flags;
    webPage->finalizeRenderingUpdate(flags);
    webPage->didUpdateRendering();

    willStartRenderingUpdateDisplay();

    MEDIA_TRACE_TIME("WP: commit layers + record paint");
    if (isCompositingMode())
        sendUpdateAC();
    else
        sendUpdateNonAC();
}

void DrawingAreaWC::sendUpdateAC()
{
    bool didProcessMainFrame = false;
    Borrow rootLayers = m_rootLayers;
    for (size_t i = 0; i < rootLayers->size(); i++) {
        auto& rootLayer = rootLayers.get()[i];
        auto frame = WebProcess::singleton().webFrame(rootLayer.frameID);
        ASSERT(frame);
        m_updateInfo.remoteContextHostedIdentifier = frame->layerHostingContextIdentifier();
        m_updateInfo.rootLayer = rootLayer.layer->primaryLayerID();

        bool isLastFrame = (i + 1) == rootLayers->size();
        bool isMainFrame = frame->isMainFrame();
        didProcessMainFrame = didProcessMainFrame || isMainFrame;
        bool willCallDisplayDidRefresh = isMainFrame || (!didProcessMainFrame && isLastFrame);
        IntSize size;
        if (isMainFrame)
            size = m_webPage->size();
        else
            size = frame->size();
        FloatSize viewport { size };
        viewport.scale(m_webPage->intrinsicDeviceScaleFactor());
        m_updateInfo.viewport = WebCore::expandedIntSize(viewport);
        updateRootLayerDeviceScaleFactor(rootLayer.layer);
        rootLayer.layer->flushCompositingStateForThisLayerOnly();

        // Because our view-relative overlay root layer is not attached to the FrameView's GraphicsLayer tree, we need to flush it manually.
        FloatRect visibleRect({ }, size);
        if (rootLayer.viewOverlayRootLayer)
            rootLayer.viewOverlayRootLayer->flushCompositingState(visibleRect);

        auto fence = m_webPage->ensureRemoteRenderingBackendProxy().flushImageBuffers();
        auto tileBuffers = takeTileBuffers(m_updateInfo);

        m_commitQueue->dispatch([this, weakThis = WeakPtr(*this), stateID = m_backingStoreStateID, updateInfo = std::exchange(m_updateInfo, { }), fence = WTF::move(fence), willCallDisplayDidRefresh, tileBuffers = WTF::move(tileBuffers), generation = m_compositorGeneration]() mutable {
            {
                MEDIA_TRACE_TIME("WP: wait for GPU process paint (fence)");
                fence();
            }
            RunLoop::mainSingleton().dispatch([this, weakThis = WTF::move(weakThis), stateID, updateInfo = WTF::move(updateInfo), willCallDisplayDidRefresh, tileBuffers = WTF::move(tileBuffers), generation]() mutable {
                if (!weakThis)
                    return;
                if (generation != m_compositorGeneration) {
                    // Painted for a GPU process that is gone; the resync repaints everything.
                    if (willCallDisplayDidRefresh)
                        displayDidRefresh();
                    return;
                }
                double sentAt = MonotonicTime::now().secondsSinceEpoch().milliseconds();
                m_remoteWCLayerTreeHostProxy->update(WTF::move(updateInfo), [this, weakThis = WTF::move(weakThis), stateID, willCallDisplayDidRefresh, sentAt, tileBuffers = WTF::move(tileBuffers), generation](std::optional<UpdateInfo> updateInfo) mutable {
#if OS(WINDOWS) && USE(GSTREAMER)
                    mediaTraceTiming("WP: scene update round trip", MonotonicTime::now().secondsSinceEpoch().milliseconds() - sentAt);
#else
                    UNUSED_PARAM(sentAt);
#endif
                    if (!weakThis)
                        return;
                    // The scene has uploaded these tiles; their buffers can be painted into again.
                    if (generation == m_compositorGeneration)
                        recycleTileBuffers(WTF::move(tileBuffers));
                    if (updateInfo && stateID == m_backingStoreStateID) {
                        ASSERT(willCallDisplayDidRefresh);
                        send(Messages::DrawingAreaProxy::Update(m_backingStoreStateID, WTF::move(*updateInfo)));
                        return;
                    }
                    if (willCallDisplayDidRefresh)
                        displayDidRefresh();
                });
            });
        });
    }
}

void DrawingAreaWC::updateRootLayerDeviceScaleFactor(WebCore::GraphicsLayer& layer)
{
    TransformationMatrix m;
    m.scale(m_webPage->intrinsicDeviceScaleFactor());
    layer.setTransform(m);
}

static bool shouldPaintBoundsRect(const IntRect& bounds, const Vector<IntRect, 1>& rects)
{
    constexpr size_t rectThreshold = 10;
    constexpr double wastedSpaceThreshold = 0.75;

    if (rects.size() <= 1 || rects.size() > rectThreshold)
        return true;
    // Attempt to guess whether or not we should use the region bounds rect or the individual rects.
    // We do this by computing the percentage of "wasted space" in the bounds. If that wasted space
    // is too large, then we will do individual rect painting instead.
    unsigned boundsArea = bounds.width() * bounds.height();
    unsigned rectsArea = 0;
    for (size_t i = 0; i < rects.size(); ++i)
        rectsArea += rects[i].width() * rects[i].height();

    double wastedSpace = 1 - (static_cast<double>(rectsArea) / boundsArea);
    return wastedSpace <= wastedSpaceThreshold;
}

void DrawingAreaWC::sendUpdateNonAC()
{
    if (m_dirtyRegion.isEmpty()) {
        displayDidRefresh();
        return;
    }
    IntRect bounds = m_dirtyRegion.bounds();
    Ref webPage = m_webPage.get();
    ASSERT(webPage->bounds().contains(bounds));
    IntSize bitmapSize = bounds.size();
    float deviceScaleFactor = webPage->corePage()->deviceScaleFactor();
    // The non-composited path hands the pixels to the UI process: always a shareable bitmap.
    RefPtr<ImageBuffer> image;
    if (WebProcess::singleton().shouldUseRemoteRenderingFor(RenderingPurpose::DOM))
        image = webPage->ensureRemoteRenderingBackendProxy().createImageBuffer(bitmapSize, RenderingMode::Unaccelerated, RenderingPurpose::DOM, deviceScaleFactor, ColorSpace::SRGB(), { PixelFormat::BGRA8 });
    else
        image = createImageBuffer(bitmapSize, deviceScaleFactor);
    auto rects = m_dirtyRegion.rects();
    if (shouldPaintBoundsRect(bounds, rects)) {
        rects.clear();
        rects.append(bounds);
    }
    m_dirtyRegion = { };

    UpdateInfo updateInfo;
    updateInfo.viewSize = webPage->size();
    updateInfo.deviceScaleFactor = deviceScaleFactor;
    updateInfo.updateRectBounds = bounds;
    updateInfo.updateRects = rects;
    updateInfo.scrollRect = m_scrollRect;
    updateInfo.scrollOffset = m_scrollOffset;
    m_scrollRect = { };
    m_scrollOffset = { };

    auto& graphicsContext = image->context();
    graphicsContext.translate(-bounds.x(), -bounds.y());
    for (const auto& rect : rects)
        webPage->drawRect(graphicsContext, rect);
    image->flushDrawingContextAsync();

    auto fence = m_webPage->ensureRemoteRenderingBackendProxy().flushImageBuffers();

    m_commitQueue->dispatch([this, weakThis = WeakPtr(*this), stateID = m_backingStoreStateID, updateInfo = WTF::move(updateInfo), image = WTF::move(image), fence = WTF::move(fence)]() mutable {
        fence();
        RunLoop::mainSingleton().dispatch([this, weakThis = WTF::move(weakThis), stateID, updateInfo = WTF::move(updateInfo), image = WTF::move(image)]() mutable {
            if (!weakThis)
                return;
            if (stateID != m_backingStoreStateID) {
                displayDidRefresh();
                return;
            }

            auto* sharing = image->toBackendSharing();
            if (is<ImageBufferBackendHandleSharing>(sharing)) {
                if (auto handle = downcast<ImageBufferBackendHandleSharing>(*sharing).createBackendHandle())
                    updateInfo.bitmapHandle = std::get<ShareableBitmap::Handle>(WTF::move(*handle));
            }

            send(Messages::DrawingAreaProxy::Update(stateID, WTF::move(updateInfo)));
        });
    });
}

void DrawingAreaWC::graphicsLayerAdded(GraphicsLayerWC& layer)
{
    m_liveGraphicsLayers.append(&layer);
    m_updateInfo.addedLayers.append(*layer.primaryLayerID());
}

void DrawingAreaWC::graphicsLayerRemoved(GraphicsLayerWC& layer)
{
    m_liveGraphicsLayers.remove(&layer);
    m_updateInfo.removedLayers.append(*layer.primaryLayerID());
}

void DrawingAreaWC::commitLayerUpdateInfo(WCLayerUpdateInfo&& info)
{
    m_updateInfo.changedLayers.append(WTF::move(info));
}

// MyBrowser: page tiles painted with Skia on the GPU in the GPU process (see WCTileTextureRegistry).
// MYBROWSER_DISABLE_GPU_RASTER=1 goes back to CPU-painted shared-memory tiles.
static bool gpuPaintedTiles()
{
    static bool enabled = [] {
        const char* value = std::getenv("MYBROWSER_DISABLE_GPU_RASTER");
        return !(value && *value == '1');
    }();
    return enabled;
}

static bool tilePoolEnabled()
{
    static bool enabled = [] {
        const char* value = std::getenv("MYBROWSER_DISABLE_TILE_POOL");
        return !(value && *value == '1');
    }();
    return enabled;
}

Vector<RefPtr<ImageBuffer>> DrawingAreaWC::takeTileBuffers(WCUpdateInfo& updateInfo)
{
    Vector<RefPtr<ImageBuffer>> buffers;
    if (!tilePoolEnabled())
        return buffers;
    for (auto& layer : updateInfo.changedLayers) {
        for (auto& tile : layer.background.tileUpdates) {
            if (auto* buffer = tile.backingStore.imageBuffer())
                buffers.append(buffer);
        }
    }
    return buffers;
}

void DrawingAreaWC::recycleTileBuffers(Vector<RefPtr<ImageBuffer>>&& buffers)
{
    // Keep at most ~64 MB of idle tile memory; the oldest buffers go first.
    constexpr double maximumPooledBytes = 64 * 1024 * 1024;
    for (auto& buffer : buffers) {
        if (buffer)
            m_freeTileBuffers.append(WTF::move(buffer));
    }
    double bytes = 0;
    for (auto& buffer : m_freeTileBuffers) {
        auto size = buffer->backendSize();
        bytes += 4.0 * size.width() * size.height();
    }
    while (bytes > maximumPooledBytes && !m_freeTileBuffers.isEmpty()) {
        auto size = m_freeTileBuffers.first()->backendSize();
        bytes -= 4.0 * size.width() * size.height();
        m_freeTileBuffers.removeAt(0);
    }
}

RefPtr<ImageBuffer> DrawingAreaWC::createImageBuffer(FloatSize size, float deviceScaleFactor)
{
    if (tilePoolEnabled()) {
        for (size_t i = 0; i < m_freeTileBuffers.size(); ++i) {
            auto& candidate = m_freeTileBuffers[i];
            if (candidate->logicalSize() != size || candidate->resolutionScale() != deviceScaleFactor)
                continue;
            RefPtr buffer = WTF::move(candidate);
            m_freeTileBuffers.removeAt(i);
            // Painting may leave pixels untouched (transparent layers): start from a clear buffer
            // like a newly allocated one.
            buffer->context().clearRect({ { }, size });
            MEDIA_TRACE_TIME("WP: tile buffer reused");
            return buffer;
        }
        MEDIA_TRACE_TIME("WP: tile buffer allocated");
    }

    if (WebProcess::singleton().shouldUseRemoteRenderingFor(RenderingPurpose::DOM))
        return Ref { m_webPage.get() }->ensureRemoteRenderingBackendProxy().createImageBuffer(size, gpuPaintedTiles() ? RenderingMode::Accelerated : RenderingMode::Unaccelerated, RenderingPurpose::DOM, deviceScaleFactor, ColorSpace::SRGB(), { PixelFormat::BGRA8 });
    return ImageBuffer::create<ImageBufferShareableBitmapBackend>(size, deviceScaleFactor, ColorSpace::SRGB(), { PixelFormat::BGRA8 }, RenderingPurpose::DOM, { });
}

void DrawingAreaWC::displayDidRefresh(MonotonicTime)
{
#if OS(WINDOWS) && USE(GSTREAMER)
    if (m_traceUpdateStart >= 0)
        mediaTraceTiming("WP: frame total (start -> displayed)", MonotonicTime::now().secondsSinceEpoch().milliseconds() - m_traceUpdateStart);
#endif
    m_waitDidUpdate = false;
    didCompleteRenderingUpdateDisplay();
    didCompleteRenderingFrame();
    if (m_forceRepaintCompletionHandler) {
        if (m_isForceRepaintCompletionHandlerDeferred)
            m_isForceRepaintCompletionHandlerDeferred = false;
        else
            m_forceRepaintCompletionHandler();
    }
    if (m_hasDeferredRenderingUpdate) {
        m_hasDeferredRenderingUpdate = false;
        triggerRenderingUpdate();
    }
}

DrawingAreaWC::RootLayerInfo* DrawingAreaWC::rootLayerInfoWithFrameIdentifier(WebCore::FrameIdentifier frameID)
{
    auto index = m_rootLayers.findIf([&] (const auto& layer) {
        return layer.frameID == frameID;
    });
    if (index == WTF::notFound) {
        ASSERT_NOT_REACHED();
        return nullptr;
    }
    return &m_rootLayers[index];
}

} // namespace WebKit

#endif // USE(GRAPHICS_LAYER_WC)

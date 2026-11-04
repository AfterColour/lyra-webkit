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
#include "WCScene.h"

#if USE(GRAPHICS_LAYER_WC)

#include "RemoteGraphicsContextGL.h"
#include "WCContentBuffer.h"
#include "WCContentBufferManager.h"
#include "WCRemoteFrameHostLayerManager.h"
#include "WCSceneContext.h"
#include "WCTileTextureRegistry.h"
#include "WCUpdateInfo.h"
#include <WebCore/BitmapTexturePool.h>
#include <WebCore/NativeImage.h>
#include <WebCore/ShareableBitmap.h>
#include <WebCore/TextureMapper.h>
#include <WebCore/TextureMapperGLHeaders.h>
#include <WebCore/TextureMapperLayer.h>
#include <WebCore/TextureMapperPlatformLayer.h>
#include <WebCore/TextureMapperSparseBackingStore.h>
#include <WebCore/MediaTraceWin.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {

WTF_MAKE_TZONE_ALLOCATED_IMPL(WCScene);

struct WCScene::Layer final : public WCContentBuffer::Client {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(WCScene::Layer);
public:
    explicit Layer(WCScene& scene)
        : scene(scene)
    {
    }
    ~Layer()
    {
        if (contentBuffer)
            contentBuffer->setClient(nullptr);
        if (hostIdentifier)
            WCRemoteFrameHostLayerManager::singleton().releaseRemoteFrameHostLayer(*hostIdentifier);
    }

    // WCContentBuffer::Client
    void platformLayerWillBeDestroyed() override
    {
        contentBuffer = nullptr;
        texmapLayer.setContentsLayer(nullptr);
    }

    bool contentBufferNeedsDisplay() override
    {
        return scene.recomposite();
    }

    WCScene& scene;

    WebCore::TextureMapperLayer texmapLayer;
    std::unique_ptr<WebCore::TextureMapperSparseBackingStore> backingStore;
    std::unique_ptr<WebCore::TextureMapperLayer> backdropLayer;
    WCContentBuffer* contentBuffer { nullptr };
    Markable<WebCore::LayerHostingContextIdentifier> hostIdentifier;
};

void WCScene::initialize(WCSceneContext& context)
{
    // The creation of the TextureMapper needs an active OpenGL context.
    m_context = &context;
    if (!m_context->makeContextCurrent())
        return;
    m_textureMapper = m_context->createTextureMapper();
}

WCScene::WCScene(WebCore::ProcessIdentifier webProcessIdentifier, bool usesOffscreenRendering)
    : m_webProcessIdentifier(webProcessIdentifier)
    , m_usesOffscreenRendering(usesOffscreenRendering)
{
}

WCScene::~WCScene()
{
    if (!m_context->makeContextCurrent())
        return;
    m_textureMapper = nullptr;
}

std::optional<UpdateInfo> WCScene::update(WCUpdateInfo&& update)
{
    MEDIA_TRACE_TIME("GPU: scene update total");
    if (!m_context->makeContextCurrent())
        return std::nullopt;
    m_textureMapper->releaseUnusedTexturesNow();

    for (auto id : update.addedLayers) {
        auto layer = makeUnique<Layer>(*this);
        m_layers.add(id, WTF::move(layer));
    }

    for (auto& layerUpdate : update.changedLayers) {
        auto layer = m_layers.get(layerUpdate.id);
        if (!layer) {
            // MyBrowser: a layer this scene never got (out of sync with the web process); skip it
            // rather than crash the GPU process.
            MEDIA_TRACE("update for an unknown layer ignored");
            continue;
        }
        if (layerUpdate.changes & WCLayerChange::Children) {
            Vector<WebCore::TextureMapperLayer*> children;
            for (auto id : layerUpdate.children) {
                if (auto child = m_layers.get(id))
                    children.append(&child->texmapLayer);
            }
            layer->texmapLayer.setChildren(WTF::move(children));
        }
        if (layerUpdate.changes & WCLayerChange::MaskLayer) {
            WebCore::TextureMapperLayer* maskLayer = nullptr;
            if (layerUpdate.maskLayer) {
                if (auto mask = m_layers.get(*layerUpdate.maskLayer))
                    maskLayer = &mask->texmapLayer;
            }
            layer->texmapLayer.setMaskLayer(maskLayer);
        }
        if (layerUpdate.changes & WCLayerChange::ReplicaLayer) {
            WebCore::TextureMapperLayer* replicaLayer = nullptr;
            if (layerUpdate.replicaLayer) {
                if (auto replica = m_layers.get(*layerUpdate.replicaLayer))
                    replicaLayer = &replica->texmapLayer;
            }
            layer->texmapLayer.setReplicaLayer(replicaLayer);
        }
        if (layerUpdate.changes & WCLayerChange::Position)
            layer->texmapLayer.setPosition(layerUpdate.position);
        if (layerUpdate.changes & WCLayerChange::AnchorPoint)
            layer->texmapLayer.setAnchorPoint(layerUpdate.anchorPoint);
        if (layerUpdate.changes & WCLayerChange::Size)
            layer->texmapLayer.setSize(layerUpdate.size);
        if (layerUpdate.changes & WCLayerChange::BoundsOrigin)
            layer->texmapLayer.setBoundsOrigin(layerUpdate.boundsOrigin);
        if (layerUpdate.changes & WCLayerChange::Preserves3D)
            layer->texmapLayer.setPreserves3D(layerUpdate.preserves3D);
        if (layerUpdate.changes & WCLayerChange::ContentsRect)
            layer->texmapLayer.setContentsRect(layerUpdate.contentsRect);
        if (layerUpdate.changes & WCLayerChange::ContentsClippingRect)
            layer->texmapLayer.setContentsClippingRect(layerUpdate.contentsClippingRect);
        if (layerUpdate.changes & WCLayerChange::ContentsRectClipsDescendants)
            layer->texmapLayer.setContentsRectClipsDescendants(layerUpdate.contentsRectClipsDescendants);
        if (layerUpdate.changes & WCLayerChange::ContentsVisible)
            layer->texmapLayer.setContentsVisible(layerUpdate.contentsVisible);
        if (layerUpdate.changes & WCLayerChange::BackfaceVisibility)
            layer->texmapLayer.setBackfaceVisibility(layerUpdate.backfaceVisibility);
        if (layerUpdate.changes & WCLayerChange::MasksToBounds)
            layer->texmapLayer.setMasksToBounds(layerUpdate.masksToBounds);
        if (layerUpdate.changes & WCLayerChange::Background) {
            if (layerUpdate.background.hasBackingStore) {
                if (!layer->backingStore) {
                    layer->backingStore = makeUnique<WebCore::TextureMapperSparseBackingStore>();
                    auto& backingStore = *layer->backingStore;
                    layer->texmapLayer.setBackgroundColor({ });
                    layer->texmapLayer.setBackingStore(&backingStore);
                }
                auto& backingStore = *layer->backingStore;
                backingStore.setSize(layerUpdate.background.backingStoreSize);
                for (auto& tileUpdate : layerUpdate.background.tileUpdates) {
                    if (tileUpdate.willRemove)
                        backingStore.removeTile(tileUpdate.index);
                    else {
                        MEDIA_TRACE_TIME("GPU: upload tile");
#if USE(SKIA)
                        if (auto bufferIdentifier = tileUpdate.backingStore.acceleratedBufferIdentifier()) {
                            // GPU-painted tile: copy texture to texture.
                            auto entry = WCTileTextureRegistry::singleton().get(m_webProcessIdentifier, *bufferIdentifier);
                            if (!entry)
                                MEDIA_TRACE("GPU-painted tile %llu was not published", static_cast<unsigned long long>(bufferIdentifier->toUInt64()));
                            else if (entry->textureID) {
                                backingStore.updateContentsFromTexture(tileUpdate.index, entry->textureID, tileUpdate.dirtyRect);
                            } else if (entry->image) {
                                MEDIA_TRACE("tile from pixel copy");
                                backingStore.updateContents(tileUpdate.index, *entry->image, tileUpdate.dirtyRect);
                            }
                            continue;
                        }
#endif
                        auto bitmap = tileUpdate.backingStore.bitmap();
                        if (bitmap) {
                            if (RefPtr image = WebCore::NativeImage::create(bitmap->createPlatformImage(WebCore::BackingStoreCopy::DontCopyBackingStore)))
                                backingStore.updateContents(tileUpdate.index, *image, tileUpdate.dirtyRect);
                        }
                    }
                }
            } else {
                layer->texmapLayer.setBackgroundColor(layerUpdate.background.color);
                layer->texmapLayer.setBackingStore(nullptr);
                layer->backingStore = nullptr;
            }
        }
        if (layerUpdate.changes & WCLayerChange::SolidColor)
            layer->texmapLayer.setSolidColor(layerUpdate.solidColor);
        if (layerUpdate.changes & WCLayerChange::ShowDebugBorder)
            layer->texmapLayer.setShowDebugBorder(layerUpdate.showDebugBorder);
        if (layerUpdate.changes & WCLayerChange::DebugBorderColor)
            layer->texmapLayer.setDebugBorderColor(layerUpdate.debugBorderColor);
        if (layerUpdate.changes & WCLayerChange::DebugBorderWidth)
            layer->texmapLayer.setDebugBorderWidth(layerUpdate.debugBorderWidth);
        if (layerUpdate.changes & WCLayerChange::ShowRepaintCounter)
            layer->texmapLayer.setShowRepaintCounter(layerUpdate.showRepaintCounter);
        if (layerUpdate.changes & WCLayerChange::RepaintCount)
            layer->texmapLayer.setRepaintCount(layerUpdate.repaintCount);
        if (layerUpdate.changes & WCLayerChange::Opacity)
            layer->texmapLayer.setOpacity(layerUpdate.opacity);
        if (layerUpdate.changes & WCLayerChange::Transform)
            layer->texmapLayer.setTransform(layerUpdate.transform);
        if (layerUpdate.changes & WCLayerChange::ChildrenTransform)
            layer->texmapLayer.setChildrenTransform(layerUpdate.childrenTransform);
        if (layerUpdate.changes & WCLayerChange::Filters)
            layer->texmapLayer.setFilters(layerUpdate.filters);
        if (layerUpdate.changes & WCLayerChange::BackdropFilters || layerUpdate.changes & WCLayerChange::BackdropFiltersRect) {
            if (!layer->backdropLayer) {
                layer->backdropLayer = makeUnique<WebCore::TextureMapperLayer>();
                layer->backdropLayer->setAnchorPoint({ });
                layer->backdropLayer->setContentsVisible(true);
                layer->backdropLayer->setMasksToBounds(true);
            }
        }
        if (layerUpdate.changes & WCLayerChange::BackdropFilters) {
            if (layerUpdate.backdropFilters.isEmpty())
                layer->texmapLayer.setBackdropLayer(nullptr);
            else {
                layer->backdropLayer->setFilters(layerUpdate.backdropFilters);
                layer->texmapLayer.setBackdropLayer(layer->backdropLayer.get());
            }
        }
        if (layerUpdate.changes & WCLayerChange::BackdropFiltersRect) {
            layer->backdropLayer->setSize(layerUpdate.backdropFiltersRect.rect().size());
            layer->backdropLayer->setPosition(layerUpdate.backdropFiltersRect.rect().location());
            layer->texmapLayer.setBackdropFiltersRect(layerUpdate.backdropFiltersRect);
        }
        if (layerUpdate.changes & WCLayerChange::PlatformLayer) {
            if (!layerUpdate.platformLayer.hasLayer) {
                if (layer->contentBuffer) {
                    layer->contentBuffer->setClient(nullptr);
                    layer->contentBuffer = nullptr;
                }
                layer->texmapLayer.setContentsLayer(nullptr);
            } else {
                WCContentBuffer* contentBuffer = nullptr;
                for (auto identifier : layerUpdate.platformLayer.identifiers)
                    contentBuffer = WCContentBufferManager::singleton().releaseContentBufferIdentifier(m_webProcessIdentifier, identifier);
                if (contentBuffer) {
                    if (layer->contentBuffer)
                        layer->contentBuffer->setClient(nullptr);
                    contentBuffer->setClient(layer);
                    layer->contentBuffer = contentBuffer;
                    layer->texmapLayer.setContentsLayer(contentBuffer->platformLayer());
                }
            }
        }
        if (layerUpdate.changes & WCLayerChange::RemoteFrame) {
            if (layerUpdate.hostIdentifier) {
                auto platformLayer = WCRemoteFrameHostLayerManager::singleton().acquireRemoteFrameHostLayer(*layerUpdate.hostIdentifier, m_webProcessIdentifier);
                layer->texmapLayer.setContentsLayer(platformLayer);
            } else if (layer->hostIdentifier)
                WCRemoteFrameHostLayerManager::singleton().releaseRemoteFrameHostLayer(*layer->hostIdentifier);
            layer->hostIdentifier = layerUpdate.hostIdentifier;
        }
    }

    for (auto id : update.removedLayers)
        m_layers.remove(id);

    auto* rootLayerEntry = update.rootLayer ? m_layers.get(*update.rootLayer) : nullptr;
    if (!rootLayerEntry) {
        MEDIA_TRACE("update without a known root layer ignored");
        return std::nullopt;
    }
    auto rootLayer = &rootLayerEntry->texmapLayer;
    rootLayer->applyAnimationsRecursively(MonotonicTime::now());

    m_lastRootLayer = *update.rootLayer;
    m_lastViewport = update.viewport;
    m_canRecomposite = !update.remoteContextHostedIdentifier && !m_usesOffscreenRendering;

    WebCore::BitmapTexture* surface = nullptr;
    RefPtr<WebCore::BitmapTexture> texture;
    bool showFPS = true;
    bool readPixel = false;
    RefPtr<WebCore::ShareableBitmap> bitmap;

    if (update.remoteContextHostedIdentifier) {
        showFPS = false;
        texture = WebCore::BitmapTexturePool::singleton().acquireTexture(update.viewport, { WebCore::BitmapTexture::Flags::SupportsAlpha });
        surface = texture.get();
    } else if (m_usesOffscreenRendering) {
        readPixel = true;
        texture = WebCore::BitmapTexturePool::singleton().acquireTexture(update.viewport, { WebCore::BitmapTexture::Flags::SupportsAlpha });
        surface = texture.get();
    } else
        glViewport(0, 0, update.viewport.width(), update.viewport.height());

    m_textureMapper->beginPainting(WebCore::TextureMapper::FlipY::No, surface);
    rootLayer->paint(*m_textureMapper);
    if (showFPS)
        m_fpsCounter.updateFPSAndDisplay(*m_textureMapper);
    if (readPixel) {
        bitmap = WebCore::ShareableBitmap::create({ update.viewport });
        glReadPixels(0, 0, update.viewport.width(), update.viewport.height(), GL_BGRA, GL_UNSIGNED_BYTE, bitmap->mutableSpan().data());
    }
    m_textureMapper->endPainting();

    std::optional<UpdateInfo> result;
    if (update.remoteContextHostedIdentifier)
        WCRemoteFrameHostLayerManager::singleton().updateTexture(*update.remoteContextHostedIdentifier, m_webProcessIdentifier, WTF::move(texture));
    else if (m_usesOffscreenRendering) {
        if (auto handle = bitmap->createHandle()) {
            result.emplace();
            result->viewSize = update.viewport;
            result->deviceScaleFactor = 1;
            result->updateScaleFactor = 1;
            WebCore::IntRect viewport = { { }, update.viewport };
            result->updateRectBounds = viewport;
            result->updateRects.append(viewport);
            result->bitmapHandle = WTF::move(*handle);
        }
    } else {
        MEDIA_TRACE_TIME("GPU: swapBuffers (vsync wait)");
        m_context->swapBuffers();
    }
    return result;
}

bool WCScene::recomposite()
{
    if (!m_canRecomposite || !m_lastRootLayer || !m_textureMapper)
        return false;
    auto* root = m_layers.get(*m_lastRootLayer);
    if (!root || !m_context->makeContextCurrent())
        return false;

    root->texmapLayer.applyAnimationsRecursively(MonotonicTime::now());
    glViewport(0, 0, m_lastViewport.width(), m_lastViewport.height());
    m_textureMapper->beginPainting(WebCore::TextureMapper::FlipY::No, nullptr);
    root->texmapLayer.paint(*m_textureMapper);
    m_fpsCounter.updateFPSAndDisplay(*m_textureMapper);
    m_textureMapper->endPainting();
    m_context->swapBuffers();
    return true;
}

} // namespace WebKit

#endif // USE(GRAPHICS_LAYER_WC)

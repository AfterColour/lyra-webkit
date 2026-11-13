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

#include "UpdateInfo.h"
#include <WebCore/GraphicsLayer.h>
#include <WebCore/ProcessIdentifier.h>
#include <WebCore/TextureMapperFPSCounter.h>
#include <wtf/Forward.h>
#include <wtf/HashMap.h>
#include <atomic>
#include <wtf/TZoneMalloc.h>

namespace WebCore {
class TextureMapper;
class TextureMapperLayer;
class TextureMapperPlatformLayer;
class TextureMapperTiledBackingStore;
}

namespace WebKit {

class WCSceneContext;
struct WCScrollingLayerUpdate;
struct WCUpdateInfo;

class WCScene {
    WTF_MAKE_TZONE_ALLOCATED(WCScene);
public:
    WCScene(WebCore::ProcessIdentifier, bool usesOffscreenRendering);
    ~WCScene();
    void initialize(WCSceneContext&);
    std::optional<UpdateInfo> update(WCUpdateInfo&&);

    // Paints the last committed layer tree again, for layers whose contents change without a
    // layer tree update from the web process (the D3D11 video layer). On-screen scenes only.
    bool recomposite();

    // Lyra: asks for the scene to be painted again soon, after whatever else is queued for the
    // scene's thread. Video frames, scrolling-thread moves and layer tree updates each painted and
    // presented a frame of their own: on a page with a playing video that was more frames than the
    // display shows, and presenting them queued up behind each other (uneven motion while
    // scrolling). Requests that arrive while one is pending, and a pending one when a layer tree
    // update paints first, now make a single frame. Returns false if the scene cannot be painted.
    bool requestRecomposite();

    // Lyra: layers moved by the scrolling thread of the web process. willApply... on the thread
    // that receives the message, apply... on the scene's thread; the scene is painted again once
    // the last of the queued batches is applied.
    void willApplyScrollingLayerUpdates() { ++m_pendingScrollingLayerUpdates; }
    void applyScrollingLayerUpdates(uint64_t sequence, Vector<WCScrollingLayerUpdate>&&);

private:
    struct Layer;
    using LayerMap = HashMap<WebCore::PlatformLayerIdentifier, std::unique_ptr<Layer>>;

    WebCore::ProcessIdentifier m_webProcessIdentifier;
    WCSceneContext* m_context { nullptr };
    std::unique_ptr<WebCore::TextureMapper> m_textureMapper;
    WebCore::TextureMapperFPSCounter m_fpsCounter;
    LayerMap m_layers;
    bool m_usesOffscreenRendering;
    std::optional<WebCore::PlatformLayerIdentifier> m_lastRootLayer;
    WebCore::IntSize m_lastViewport;
    bool m_canRecomposite { false };
    std::atomic<unsigned> m_pendingScrollingLayerUpdates { 0 };
    bool m_scrollingLayersChanged { false };
    bool m_needsRecomposite { false };
    bool m_recompositeScheduled { false };
    bool m_animationFrameScheduled { false };
    void scheduleAnimationFrameIfNeeded();
    uint64_t m_sceneID { 0 };
    WebCore::Color m_backgroundColor { WebCore::Color::white };
    void clearToBackground();

    bool applyScrollingLayerUpdate(Layer&, uint64_t sequence, const WCScrollingLayerUpdate&);
    bool ownsWindow() const;
};

} // namespace WebKit

#endif // USE(GRAPHICS_LAYER_WC)

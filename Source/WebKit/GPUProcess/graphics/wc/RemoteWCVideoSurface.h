/*
 * Copyright (C) 2026 MyBrowser contributors.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES ARE DISCLAIMED.
 */

#pragma once

#if USE(GRAPHICS_LAYER_WC) && USE(GSTREAMER)

#include "WCContentBufferIdentifier.h"
#include <WebCore/ProcessIdentifier.h>
#include <WebCore/TextureMapperPlatformLayer.h>
#include <array>
#include <atomic>
#include <optional>
#include <wrl/client.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>
#include <wtf/win/Win32Handle.h>

struct ID3D11Texture2D;
struct IDXGIKeyedMutex;

namespace WebKit {

// GPU process half of the D3D11 video layer (the web process half is WCVideoLayerProxy).
//
// The web process owns a few shared D3D11 textures (NT handles, keyed mutex). They are opened here
// on ANGLE's D3D11 device and bound as GL textures through EGL_ANGLE_image_d3d11_texture, so the
// compositor (WCScene) draws a video frame with a single textured quad.
//
// Keyed mutex protocol, per texture: the web process acquires key 0, copies a frame, releases key 1.
// present() acquires key 1 of the new texture and releases key 0 of the one shown before, so the
// texture being displayed is never written to.
//
// Everything except construction runs on remoteGraphicsStreamWorkQueue(), the thread of WCScene and
// of the compositor's GL context.
class RemoteWCVideoSurface final : public ThreadSafeRefCounted<RemoteWCVideoSurface>, public WebCore::TextureMapperPlatformLayer {
public:
    static Ref<RemoteWCVideoSurface> create(WebCore::ProcessIdentifier webProcessIdentifier)
    {
        return adoptRef(*new RemoteWCVideoSurface(webProcessIdentifier));
    }
    ~RemoteWCVideoSurface();

    void setTextures(Vector<Win32Handle>&&, unsigned width, unsigned height);
    // Main thread, when a PresentWCVideoSurface message arrives: lets present() skip compositing
    // frames that are already superseded by a newer one.
    void willPresent() { ++m_pendingPresents; }
    std::optional<WCContentBufferIdentifier> present(unsigned slot);
    void invalidate();

    // Set when the textures cannot be used here; the web process then paints in software.
    bool hasFailed() const { return m_failed; }

    // ANGLE's D3D11 adapter, encoded like GStreamer's "adapter-luid"; 0 if unknown. Any thread.
    static int64_t angleAdapterLUID();

private:
    explicit RemoteWCVideoSurface(WebCore::ProcessIdentifier);

    // TextureMapperPlatformLayer
    void paintToTextureMapper(WebCore::TextureMapper&, const WebCore::FloatRect&, const WebCore::TransformationMatrix&, float opacity) final;

    void releaseTextures();

    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex;
        void* eglImage { nullptr };
    };

    WebCore::ProcessIdentifier m_webProcessIdentifier;
    Vector<Slot> m_slots;
    int m_currentSlot { -1 };
    unsigned m_width { 0 };
    unsigned m_height { 0 };
    bool m_invalidated { false };
    std::atomic<unsigned> m_pendingPresents { 0 };
    std::atomic<bool> m_failed { false };
};

} // namespace WebKit

#endif // USE(GRAPHICS_LAYER_WC) && USE(GSTREAMER)

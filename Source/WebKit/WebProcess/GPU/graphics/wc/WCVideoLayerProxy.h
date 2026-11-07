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

#include "WCPlatformLayerGCGL.h"
#include <WebCore/AcceleratedVideoLayerWin.h>
#include <array>
#include <atomic>
#include <dxgiformat.h>
#include <wrl/client.h>
#include <wtf/Lock.h>

struct ID3D11Device;
struct ID3D11Texture2D;
struct IDXGIKeyedMutex;

namespace IPC {
class Connection;
}

namespace WebKit {

// Web process half of the D3D11 video layer (WebCore::AcceleratedVideoLayerWin); the GPU process half
// is RemoteWCVideoSurface. Frames are copied on the decoder's D3D11 device into a pool of shared
// textures; the GPU process opens them once and is told which one to show for every frame.
class WCVideoLayerProxy final : public WebCore::AcceleratedVideoLayerWin {
public:
    static void registerFactory();
    ~WCVideoLayerProxy();

    PlatformLayer* platformLayer() final { return &m_layer; }
    bool pushFrame(ID3D11Texture2D*, unsigned subresource, unsigned width, unsigned height) final;
    void setFailureHandler(Function<void()>&&) final;
    int64_t adapterLUID() const final { return m_adapterLUID; }

private:
    WCVideoLayerProxy(Ref<IPC::Connection>&&, int64_t adapterLUID);

    class Layer final : public WCPlatformLayerGCGL {
    public:
        explicit Layer(WCVideoLayerProxy& proxy)
            : m_proxy(proxy)
        {
        }
        bool notifiesContentsChanged() const final { return true; }
        void didAttachWithoutContentBuffer() final { m_proxy.presentLastFrameAgain(); }
        ~Layer()
        {
            if (client())
                client()->platformLayerWillBeDestroyed();
        }
        // Makes GraphicsLayerWC send the new content buffer identifier with the next layer flush.
        void contentsChanged()
        {
            if (client())
                client()->setPlatformLayerNeedsDisplay();
        }
    private:
        WCVideoLayerProxy& m_proxy;
    };

    void presentLastFrameAgain(unsigned attemptsLeft = 8); // Main thread.

    bool ensureTextures(ID3D11Device*, DXGI_FORMAT, unsigned width, unsigned height) WTF_REQUIRES_LOCK(m_lock);
    void didPresent(std::optional<WCContentBufferIdentifier>, bool succeeded);
    void notifyFailure(); // Any thread.

    // One shown by the GPU process, the rest queued or free: with the GPU process thread also
    // rasterizing pages, a few presents can be in flight at once.
    static constexpr unsigned textureCount = 4;

    const uint64_t m_identifier;
    const Ref<IPC::Connection> m_connection;
    const int64_t m_adapterLUID;
    Layer m_layer; // Main thread.
    Function<void()> m_failureHandler; // Main thread.
    std::atomic<bool> m_failureNotified { false };

    Lock m_lock;
    Microsoft::WRL::ComPtr<ID3D11Device> m_device WTF_GUARDED_BY_LOCK(m_lock);
    DXGI_FORMAT m_format WTF_GUARDED_BY_LOCK(m_lock) { DXGI_FORMAT_UNKNOWN };
    unsigned m_width WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    unsigned m_height WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex;
    };
    std::array<Slot, textureCount> m_slots WTF_GUARDED_BY_LOCK(m_lock);
    unsigned m_nextSlot WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    std::optional<unsigned> m_lastPresentedSlot WTF_GUARDED_BY_LOCK(m_lock);
    bool m_failed WTF_GUARDED_BY_LOCK(m_lock) { false };
};

} // namespace WebKit

#endif // USE(GRAPHICS_LAYER_WC) && USE(GSTREAMER)

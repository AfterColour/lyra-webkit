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

#include "WCContentBufferIdentifier.h"
#include "WCContentBufferManager.h"
#include <WebCore/ProcessIdentifier.h>
#include <WebCore/TextureMapperPlatformLayer.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {

// Public base: RemoteWCVideoSurface gets back to its WCContentBuffer from its platform layer client.
class WCContentBuffer final : public WebCore::TextureMapperPlatformLayer::Client {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(WCContentBuffer);
public:
    class Client {
    public:
        virtual void platformLayerWillBeDestroyed() = 0;
        // The platform layer has new contents (D3D11 video layer). Returns false if the client
        // cannot composite on its own, so the update has to come from the web process.
        virtual bool contentBufferNeedsDisplay() { return false; }
    };
    
    WCContentBuffer(WCContentBufferManager& manager, WebCore::ProcessIdentifier processIdentifier, WebCore::TextureMapperPlatformLayer* platformLayer)
        : m_manager(manager)
        , m_processIdentifier(processIdentifier)
        , m_platformLayer(platformLayer)
    {
        m_platformLayer->setClient(this);
    }

    ~WCContentBuffer()
    {
        m_platformLayer->setClient(nullptr);
    }

    void setClient(Client* client)
    {
        m_client = client;
    }

    WebCore::TextureMapperPlatformLayer* platformLayer() const
    {
        return m_platformLayer;
    }

    WCContentBufferIdentifier identifier()
    {
        return m_identifier;
    }

    // Composites the scene showing this buffer again, without a layer tree update from the web
    // process. False when no scene shows it or the scene cannot do that (offscreen rendering).
    bool compositeForNewContents() { return m_client && m_client->contentBufferNeedsDisplay(); }

private:
    void platformLayerWillBeDestroyed() override
    {
        if (m_client)
            m_client->platformLayerWillBeDestroyed();
        m_manager.removeContentBuffer(m_processIdentifier, *this);
    }
    void setPlatformLayerNeedsDisplay() override
    {
        compositeForNewContents();
    }

    WCContentBufferManager& m_manager;
    WebCore::ProcessIdentifier m_processIdentifier;
    WCContentBufferIdentifier m_identifier { WCContentBufferIdentifier::generate() };
    WebCore::TextureMapperPlatformLayer* m_platformLayer;
    Client* m_client { nullptr };
};

} // namespace WebKit

#endif // USE(GRAPHICS_LAYER_WC)

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

#if USE(GRAPHICS_LAYER_WC) && USE(SKIA)

#include <WebCore/NativeImage.h>
#include <WebCore/ProcessIdentifier.h>
#include <WebCore/RenderingResourceIdentifier.h>
#include <optional>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>

namespace WebKit {

// GPU-painted page tiles (MyBrowser). The web process paints tiles into accelerated ImageBuffers
// of the RemoteRenderingBackend; when such a buffer is flushed, the backend publishes here what
// WCScene needs to update the tile: the GL texture of the Skia surface (same share group as the
// compositor's context), or a copy of the pixels if the buffer could not be accelerated.
// Written on the rendering backend's queue, read on the WCScene queue.
class WCTileTextureRegistry {
public:
    struct Entry {
        unsigned textureID { 0 };
        RefPtr<WebCore::NativeImage> image;
    };

    static WCTileTextureRegistry& singleton()
    {
        static NeverDestroyed<WCTileTextureRegistry> registry;
        return registry;
    }

    void set(WebCore::ProcessIdentifier process, WebCore::RenderingResourceIdentifier buffer, Entry&& entry)
    {
        Locker locker { m_lock };
        m_entries.set(key(process, buffer), WTF::move(entry));
    }

    std::optional<Entry> get(WebCore::ProcessIdentifier process, WebCore::RenderingResourceIdentifier buffer)
    {
        Locker locker { m_lock };
        auto it = m_entries.find(key(process, buffer));
        if (it == m_entries.end())
            return std::nullopt;
        return it->value;
    }

    void remove(WebCore::ProcessIdentifier process, WebCore::RenderingResourceIdentifier buffer)
    {
        Locker locker { m_lock };
        m_entries.remove(key(process, buffer));
    }

private:
    static std::pair<uint64_t, uint64_t> key(WebCore::ProcessIdentifier process, WebCore::RenderingResourceIdentifier buffer)
    {
        return { process.toUInt64(), buffer.toUInt64() };
    }

    Lock m_lock;
    HashMap<std::pair<uint64_t, uint64_t>, Entry> m_entries WTF_GUARDED_BY_LOCK(m_lock);
};

} // namespace WebKit

#endif // USE(GRAPHICS_LAYER_WC) && USE(SKIA)

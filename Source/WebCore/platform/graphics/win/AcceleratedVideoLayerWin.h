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

#if OS(WINDOWS) && USE(GRAPHICS_LAYER_WC)

#include "PlatformLayer.h"
#include <wtf/Function.h>
#include <wtf/ThreadSafeRefCounted.h>

struct ID3D11Texture2D;

namespace WebCore {

// A compositor layer for <video> fed with D3D11 textures (Windows, GraphicsLayerWC).
//
// WebCore only sees this interface. WebKit implements it in the web process: the frames are copied
// into a small pool of shared D3D11 textures (NT handles, keyed mutex) that the GPU process opens on
// ANGLE's device and draws in the compositor, so a frame never goes through the CPU.
// Destroyed on the main thread: the layer is owned by GraphicsLayer clients there.
class AcceleratedVideoLayerWin : public ThreadSafeRefCounted<AcceleratedVideoLayerWin, WTF::DestructionThread::Main> {
public:
    using Factory = Function<RefPtr<AcceleratedVideoLayerWin>()>;

    // Called once by WebKit when the web process starts.
    WEBCORE_EXPORT static void setFactory(Factory&&);

    // Main thread. Null when the layer cannot be used (no factory, disabled with
    // MYBROWSER_DISABLE_D3D11_VIDEO=1); callers then paint frames in software.
    WEBCORE_EXPORT static RefPtr<AcceleratedVideoLayerWin> create();

    virtual ~AcceleratedVideoLayerWin() = default;

    // Main thread. The layer to hand to GraphicsLayer::setContentsToPlatformLayer().
    virtual PlatformLayer* platformLayer() = 0;

    // Any thread. Copies the given subresource of `texture` (the top-left `width`x`height` part,
    // BGRA) into the next shared texture and schedules it for display. The caller must hold the
    // lock that guards the immediate context of the texture's device. Returns false if the frame
    // could not be used (unsupported format, device error); true also when it was dropped because
    // the compositor still holds every shared texture.
    virtual bool pushFrame(ID3D11Texture2D* texture, unsigned subresource, unsigned width, unsigned height) = 0;

    // Main thread. Called at most once, on the main thread, when the layer stops working (the GPU
    // process could not use the textures, a device error, the GPU process went away). The player
    // then paints frames in software instead of showing an empty video.
    virtual void setFailureHandler(Function<void()>&&) = 0;

    // LUID of the adapter ANGLE renders with in the GPU process, encoded like GStreamer's
    // "adapter-luid" ((HighPart << 32) | LowPart); 0 if unknown. Decoding must happen on the same
    // adapter, or the GPU process cannot open the shared textures (laptops with two GPUs).
    virtual int64_t adapterLUID() const = 0;
};

} // namespace WebCore

#endif // OS(WINDOWS) && USE(GRAPHICS_LAYER_WC)

/*
 * Copyright (C) 2026 MyBrowser contributors. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if USE(GRAPHICS_LAYER_WC)

#include <WebCore/ImageBufferBackend.h>
#include <WebCore/NullGraphicsContext.h>
#include <wtf/TZoneMalloc.h>

namespace WebKit {

// Stand-in for an accelerated ImageBuffer that lives only in the GPU process (a Skia GPU
// surface there). This process records drawing into it through RemoteImageBufferProxy and
// never touches its pixels, so there is no backing store here. The counterpart of
// ImageBufferRemoteIOSurfaceBackend for ports without IOSurface.
class ImageBufferRemoteAcceleratedBackendWC final : public WebCore::ImageBufferBackend {
    WTF_MAKE_TZONE_ALLOCATED(ImageBufferRemoteAcceleratedBackendWC);
    WTF_MAKE_NONCOPYABLE(ImageBufferRemoteAcceleratedBackendWC);
public:
    static std::unique_ptr<ImageBufferRemoteAcceleratedBackendWC> create(const WebCore::ImageBufferParameters&);
    ~ImageBufferRemoteAcceleratedBackendWC();

    WebCore::RenderingMode renderingMode() const final { return WebCore::RenderingMode::Accelerated; }
    size_t memoryCost() const final;

private:
    using WebCore::ImageBufferBackend::ImageBufferBackend;

    WebCore::NullGraphicsContext& context() final { return m_context; }
    RefPtr<WebCore::NativeImage> copyNativeImage() final { return nullptr; }
    RefPtr<WebCore::NativeImage> createNativeImageReference() final { return nullptr; }
    void getPixelBuffer(const WebCore::IntRect&, WebCore::PixelBuffer&) final;
    void putPixelBuffer(const WebCore::PixelBufferSourceView&, const WebCore::IntRect&, const WebCore::IntPoint&, WebCore::AlphaPremultiplication) final { }
    bool canMapBackingStore() const final { return false; }
    unsigned bytesPerRow() const final { return 0; }
    String debugDescription() const final;

    WebCore::NullGraphicsContext m_context;
};

} // namespace WebKit

#endif // USE(GRAPHICS_LAYER_WC)

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

#include "config.h"
#include "ImageBufferRemoteAcceleratedBackendWC.h"

#if USE(GRAPHICS_LAYER_WC)

#include <WebCore/PixelBuffer.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/TextStream.h>

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(ImageBufferRemoteAcceleratedBackendWC);

std::unique_ptr<ImageBufferRemoteAcceleratedBackendWC> ImageBufferRemoteAcceleratedBackendWC::create(const ImageBufferParameters& parameters)
{
    return std::unique_ptr<ImageBufferRemoteAcceleratedBackendWC> { new ImageBufferRemoteAcceleratedBackendWC { parameters } };
}

ImageBufferRemoteAcceleratedBackendWC::~ImageBufferRemoteAcceleratedBackendWC() = default;

size_t ImageBufferRemoteAcceleratedBackendWC::memoryCost() const
{
    // The texture lives in the GPU process, but it is still memory this buffer keeps alive.
    auto backendSize = size();
    if (backendSize.isEmpty())
        return 0;
    return ImageBufferBackend::calculateMemoryCost(backendSize, CheckedUint32(backendSize.width()) * 4);
}

void ImageBufferRemoteAcceleratedBackendWC::getPixelBuffer(const IntRect&, PixelBuffer& destination)
{
    // RemoteImageBufferProxy reads pixels from the GPU process; this is never reached in practice.
    destination.zeroFill();
}

String ImageBufferRemoteAcceleratedBackendWC::debugDescription() const
{
    TextStream stream;
    stream << "ImageBufferRemoteAcceleratedBackendWC " << this;
    return stream.release();
}

} // namespace WebKit

#endif // USE(GRAPHICS_LAYER_WC)

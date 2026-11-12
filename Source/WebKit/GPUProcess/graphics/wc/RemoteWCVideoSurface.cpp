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

#include "config.h"
#include "RemoteWCVideoSurface.h"

#if USE(GRAPHICS_LAYER_WC) && USE(GSTREAMER)

#include "WCContentBuffer.h"
#include "WCContentBufferManager.h"
#include <WebCore/GLDisplay.h>
#include <WebCore/MediaTraceWin.h>
#include <WebCore/PlatformDisplay.h>
#include <WebCore/TextureMapper.h>
#include <WebCore/TextureMapperFlags.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <mutex>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

namespace WebKit {
using namespace WebCore;

// How long present() waits for the web process to finish a copy (normally already done).
static constexpr DWORD presentAcquireTimeoutMs = 100;

// GLDisplay::createImage() and PlatformDisplay::eglDisplay() are not exported from WebCore.
static EGLDisplay compositorEGLDisplay()
{
    return PlatformDisplay::sharedDisplay().glDisplay().eglDisplay();
}

static PFNEGLCREATEIMAGEKHRPROC createImageKHR()
{
    static auto function = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    return function;
}

static PFNEGLDESTROYIMAGEKHRPROC destroyImageKHR()
{
    static auto function = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    return function;
}

static Microsoft::WRL::ComPtr<ID3D11Device1> angleD3D11Device()
{
    // ANGLE's D3D11 device: the GL textures the compositor samples must live on it.
    static PFNEGLQUERYDISPLAYATTRIBEXTPROC queryDisplayAttrib = reinterpret_cast<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(eglGetProcAddress("eglQueryDisplayAttribEXT"));
    static PFNEGLQUERYDEVICEATTRIBEXTPROC queryDeviceAttrib = reinterpret_cast<PFNEGLQUERYDEVICEATTRIBEXTPROC>(eglGetProcAddress("eglQueryDeviceAttribEXT"));
    if (!queryDisplayAttrib || !queryDeviceAttrib)
        return nullptr;

    EGLAttrib eglDevice = 0;
    if (!queryDisplayAttrib(compositorEGLDisplay(), EGL_DEVICE_EXT, &eglDevice) || !eglDevice)
        return nullptr;
    EGLAttrib d3d11Device = 0;
    if (!queryDeviceAttrib(reinterpret_cast<EGLDeviceEXT>(eglDevice), EGL_D3D11_DEVICE_ANGLE, &d3d11Device) || !d3d11Device)
        return nullptr;

    Microsoft::WRL::ComPtr<ID3D11Device1> device1;
    reinterpret_cast<ID3D11Device*>(d3d11Device)->QueryInterface(IID_PPV_ARGS(&device1));
    // The device is driven from several GPU process threads (compositor, Skia rasterization):
    // let the D3D11 runtime serialize use of its immediate context.
    static std::once_flag protectOnce;
    std::call_once(protectOnce, [&] {
        Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(reinterpret_cast<ID3D11Device*>(d3d11Device)->QueryInterface(IID_PPV_ARGS(&multithread))))
            multithread->SetMultithreadProtected(TRUE);
    });
    return device1;
}

static int64_t adapterLUID(ID3D11Device* device)
{
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc { };
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc)))
        return 0;
    return static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) | desc.AdapterLuid.LowPart);
}

int64_t RemoteWCVideoSurface::angleAdapterLUID()
{
    auto device = angleD3D11Device();
    int64_t luid = device ? adapterLUID(device.Get()) : 0;
    MEDIA_TRACE("ANGLE adapter LUID %08llx", static_cast<unsigned long long>(luid));
    return luid;
}

RemoteWCVideoSurface::RemoteWCVideoSurface(WebCore::ProcessIdentifier webProcessIdentifier)
    : m_webProcessIdentifier(webProcessIdentifier)
{
}

RemoteWCVideoSurface::~RemoteWCVideoSurface()
{
    ASSERT(m_invalidated);
}

void RemoteWCVideoSurface::setTextures(Vector<Win32Handle>&& handles, unsigned width, unsigned height)
{
    if (m_invalidated)
        return;
    if (m_currentSlot >= 0 && static_cast<unsigned>(m_currentSlot) < m_slots.size()) {
        releaseRetainedSlot();
        // Still acquired (key 1) by us: its contents stay as they are.
        m_retainedSlot = std::exchange(m_slots[m_currentSlot], { });
        m_retainedWidth = m_width;
        m_retainedHeight = m_height;
        m_currentSlot = -1;
    }
    releaseTextures();

    auto device = angleD3D11Device();
    if (!device) {
        MEDIA_TRACE("no ANGLE D3D11 device (EGL_EXT_device_query / D3D11 backend missing)");
        m_failed = true;
        return;
    }

    Vector<Slot> slots;
    for (auto& handle : handles) {
        Slot slot;
        HRESULT hr = device->OpenSharedResource1(handle.get(), IID_PPV_ARGS(&slot.texture));
        if (FAILED(hr)) {
            // Most likely the web process created them on another adapter (laptops with two GPUs).
            MEDIA_TRACE("OpenSharedResource1 failed: 0x%08lx", static_cast<unsigned long>(hr));
            m_failed = true;
            return;
        }
        hr = slot.texture.As(&slot.mutex);
        if (FAILED(hr)) {
            MEDIA_TRACE("shared texture has no keyed mutex: 0x%08lx", static_cast<unsigned long>(hr));
            m_failed = true;
            return;
        }
        slots.append(WTF::move(slot));
    }

    m_slots = WTF::move(slots);
    m_failed = false;
    m_width = width;
    m_height = height;
    MEDIA_TRACE("opened %u shared textures %ux%u", static_cast<unsigned>(m_slots.size()), width, height);
}

std::optional<WCContentBufferIdentifier> RemoteWCVideoSurface::present(unsigned slot)
{
    bool isLatestFrame = !--m_pendingPresents;
    if (m_invalidated || m_failed)
        return std::nullopt;
    if (slot >= m_slots.size()) {
        m_failed = true;
        return std::nullopt;
    }

    if (static_cast<int>(slot) != m_currentSlot) {
        HRESULT hr = m_slots[slot].mutex->AcquireSync(1, presentAcquireTimeoutMs);
        if (hr != S_OK) {
            MEDIA_TRACE("AcquireSync(1) on slot %u: 0x%08lx", slot, static_cast<unsigned long>(hr));
            return std::nullopt;
        }
        if (m_currentSlot >= 0)
            m_slots[m_currentSlot].mutex->ReleaseSync(0);
        m_currentSlot = slot;
        releaseRetainedSlot();
    }
    drawPictureInPicture();

    // Already shown by a WCScene layer: composite right away. Going through the web process instead
    // (identifier -> layer flush -> scene update) costs a main-thread round trip per frame and caps
    // the video at ~40 fps with uneven pacing.
    if (auto* contentBuffer = static_cast<WCContentBuffer*>(client())) {
        if (!isLatestFrame)
            return std::nullopt; // A newer frame is already queued; composite that one.
        MEDIA_TRACE_TIME("GPU proc: recomposite for video");
        if (contentBuffer->compositeForNewContents())
            return std::nullopt;
    }

    // Not attached yet: hand an identifier to the web process, which sends it with its next layer
    // flush. One identifier per platform layer is valid at a time; std::nullopt means the previous
    // one has not been consumed yet.
    return WCContentBufferManager::singleton().acquireContentBufferIdentifier(m_webProcessIdentifier, this);
}

void RemoteWCVideoSurface::paintToTextureMapper(TextureMapper& textureMapper, const FloatRect& targetRect, const TransformationMatrix& matrix, float opacity)
{
    Slot* current = nullptr;
    unsigned frameWidth = m_width;
    unsigned frameHeight = m_height;
    if (m_currentSlot >= 0 && static_cast<unsigned>(m_currentSlot) < m_slots.size())
        current = &m_slots[m_currentSlot];
    else if (m_retainedSlot) {
        current = &*m_retainedSlot;
        frameWidth = m_retainedWidth;
        frameHeight = m_retainedHeight;
    }
    if (!current)
        return;

    MEDIA_TRACE_TIME("GPU proc: draw video layer");
    auto& slot = *current;
    if (!ensureEGLImage(slot))
        return;

    static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTargetTexture2D = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!imageTargetTexture2D)
        return;

    // A GL texture name per draw: the compositor may draw with a different GL context (one per
    // window), and texture names are not shared between them. Binding an EGLImage is cheap.
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    imageTargetTexture2D(GL_TEXTURE_2D, slot.eglImage);
    // Lyra: the frames are scaled for the player's size on the device already (MediaPlayerPrivate-
    // GStreamer::updateD3D11VideoScaling, which rounds to even sizes and ignores a few pixels of
    // jitter). Resampling them again by a fraction of a pixel, or at a position between device
    // pixels, blurs the whole picture: draw them pixel for pixel when they are that close.
    FloatRect drawRect = targetRect;
    if (matrix.isAffine() && !matrix.b() && !matrix.c() && matrix.a() > 0 && matrix.d() > 0) {
        FloatRect device = matrix.mapRect(targetRect);
        float width = frameWidth;
        float height = frameHeight;
        if (std::abs(device.width() - width) <= 3 && std::abs(device.height() - height) <= 3) {
            FloatRect snapped { std::round(device.x() + (device.width() - width) / 2), std::round(device.y() + (device.height() - height) / 2), width, height };
            if (auto inverse = matrix.inverse())
                drawRect = inverse->mapRect(snapped);
        }
    }
    textureMapper.drawTexture(texture, { }, drawRect, matrix, opacity);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDeleteTextures(1, &texture);
}

bool RemoteWCVideoSurface::ensureEGLImage(Slot& slot)
{
    if (slot.eglImage)
        return true;
    if (!createImageKHR())
        return false;
    const EGLint attributes[] = { EGL_NONE };
    slot.eglImage = createImageKHR()(compositorEGLDisplay(), EGL_NO_CONTEXT, EGL_D3D11_TEXTURE_ANGLE, reinterpret_cast<EGLClientBuffer>(slot.texture.Get()), attributes);
    if (slot.eglImage == EGL_NO_IMAGE_KHR)
        slot.eglImage = nullptr;
    if (!slot.eglImage) {
        MEDIA_TRACE("eglCreateImage(EGL_D3D11_TEXTURE_ANGLE) failed: 0x%x", eglGetError());
        m_failed = true;
        return false;
    }
    return true;
}

bool RemoteWCVideoSurface::currentFrame(Slot*& slot, unsigned& width, unsigned& height)
{
    if (m_currentSlot >= 0 && static_cast<unsigned>(m_currentSlot) < m_slots.size()) {
        slot = &m_slots[m_currentSlot];
        width = m_width;
        height = m_height;
        return true;
    }
    if (m_retainedSlot) {
        slot = &*m_retainedSlot;
        width = m_retainedWidth;
        height = m_retainedHeight;
        return true;
    }
    return false;
}

// Lyra: picture-in-picture. The window belongs to the UI process; one surface draws into it at a time.
static HashMap<HWND, RemoteWCVideoSurface*>& pictureInPictureOwners()
{
    static NeverDestroyed<HashMap<HWND, RemoteWCVideoSurface*>> owners;
    return owners;
}

// Draws with its own GL context: keep the compositor's current (WCScene draws without making it current).
class KeepCurrentGLContext {
public:
    KeepCurrentGLContext()
        : m_display(eglGetCurrentDisplay())
        , m_draw(eglGetCurrentSurface(EGL_DRAW))
        , m_read(eglGetCurrentSurface(EGL_READ))
        , m_context(eglGetCurrentContext())
    {
    }
    ~KeepCurrentGLContext()
    {
        if (m_context != EGL_NO_CONTEXT)
            eglMakeCurrent(m_display, m_draw, m_read, m_context);
        else
            eglMakeCurrent(compositorEGLDisplay(), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
private:
    EGLDisplay m_display;
    EGLSurface m_draw;
    EGLSurface m_read;
    EGLContext m_context;
};

void RemoteWCVideoSurface::setPictureInPictureWindow(uint64_t windowValue, bool redraw)
{
    if (m_invalidated)
        return;
    HWND window = reinterpret_cast<HWND>(windowValue);
    if (!window) {
        releasePictureInPicture();
        return;
    }
    if (m_pictureInPicture && m_pictureInPicture->window == window) {
        if (redraw)
            drawPictureInPicture();
        return;
    }
    releasePictureInPicture();
    if (auto* owner = pictureInPictureOwners().get(window))
        owner->releasePictureInPicture();

    KeepCurrentGLContext keep;
    EGLDisplay display = compositorEGLDisplay();
    const EGLint configAttributes[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    EGLConfig config = nullptr;
    EGLint configCount = 0;
    if (!eglChooseConfig(display, configAttributes, &config, 1, &configCount) || !configCount) {
        MEDIA_TRACE("picture-in-picture: no EGL config");
        return;
    }
    auto pip = std::unique_ptr<PictureInPicture>(new PictureInPicture); // plain struct: not FastMalloc-annotated
    pip->window = window;
    pip->surface = eglCreateWindowSurface(display, config, reinterpret_cast<EGLNativeWindowType>(window), nullptr);
    const EGLint contextAttributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    if (pip->surface != EGL_NO_SURFACE)
        pip->context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
    if (pip->surface == EGL_NO_SURFACE || pip->context == EGL_NO_CONTEXT || !eglMakeCurrent(display, pip->surface, pip->surface, pip->context)) {
        MEDIA_TRACE("picture-in-picture: no EGL surface or context: 0x%x", eglGetError());
        if (pip->context != EGL_NO_CONTEXT)
            eglDestroyContext(display, pip->context);
        if (pip->surface != EGL_NO_SURFACE)
            eglDestroySurface(display, pip->surface);
        return;
    }
    eglSwapInterval(display, 0); // never wait for vblank on the compositor's thread
    pip->textureMapper = TextureMapper::create();
    m_pictureInPicture = WTF::move(pip);
    pictureInPictureOwners().set(window, this);
    MEDIA_TRACE("drawing into the picture-in-picture window");
    drawPictureInPicture();
}

void RemoteWCVideoSurface::drawPictureInPicture()
{
    if (!m_pictureInPicture)
        return;
    Slot* slot = nullptr;
    unsigned frameWidth = 0, frameHeight = 0;
    RECT client { };
    if (!currentFrame(slot, frameWidth, frameHeight) || !frameWidth || !frameHeight) {
        MEDIA_TRACE("picture-in-picture: no frame yet");
        return;
    }
    if (!GetClientRect(m_pictureInPicture->window, &client) || client.right <= 0 || client.bottom <= 0) {
        MEDIA_TRACE("picture-in-picture: window gone or empty");
        return;
    }

    KeepCurrentGLContext keep;
    EGLDisplay display = compositorEGLDisplay();
    if (!eglMakeCurrent(display, m_pictureInPicture->surface, m_pictureInPicture->surface, m_pictureInPicture->context)) {
        MEDIA_TRACE("picture-in-picture: eglMakeCurrent failed 0x%x", eglGetError());
        return;
    }
    if (!ensureEGLImage(*slot))
        return;
    static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTargetTexture2D = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!imageTargetTexture2D)
        return;

    MEDIA_TRACE_TIME("GPU proc: draw picture-in-picture");
    glViewport(0, 0, client.right, client.bottom);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    // Letterboxed, at whole pixels.
    float fit = std::min(static_cast<float>(client.right) / frameWidth, static_cast<float>(client.bottom) / frameHeight);
    float width = std::round(frameWidth * fit), height = std::round(frameHeight * fit);
    FloatRect target { std::round((client.right - width) / 2), std::round((client.bottom - height) / 2), width, height };

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    imageTargetTexture2D(GL_TEXTURE_2D, slot->eglImage);
    auto& textureMapper = *m_pictureInPicture->textureMapper;
    textureMapper.beginPainting(TextureMapper::FlipY::No, nullptr);
    textureMapper.drawTexture(texture, { }, target, { }, 1);
    textureMapper.endPainting();
    glBindTexture(GL_TEXTURE_2D, 0);
    glDeleteTextures(1, &texture);
    eglSwapBuffers(display, m_pictureInPicture->surface);
}

void RemoteWCVideoSurface::releasePictureInPicture()
{
    if (!m_pictureInPicture)
        return;
    auto owner = pictureInPictureOwners().find(m_pictureInPicture->window);
    if (owner != pictureInPictureOwners().end() && owner->value == this)
        pictureInPictureOwners().remove(owner);
    {
        KeepCurrentGLContext keep;
        EGLDisplay display = compositorEGLDisplay();
        if (eglMakeCurrent(display, m_pictureInPicture->surface, m_pictureInPicture->surface, m_pictureInPicture->context))
            m_pictureInPicture->textureMapper = nullptr;
    }
    EGLDisplay display = compositorEGLDisplay();
    eglDestroyContext(display, m_pictureInPicture->context);
    eglDestroySurface(display, m_pictureInPicture->surface);
    m_pictureInPicture = nullptr;
    MEDIA_TRACE("stopped drawing into the picture-in-picture window");
}

void RemoteWCVideoSurface::releaseRetainedSlot()
{
    if (!m_retainedSlot)
        return;
    if (m_retainedSlot->mutex)
        m_retainedSlot->mutex->ReleaseSync(0);
    if (m_retainedSlot->eglImage && destroyImageKHR())
        destroyImageKHR()(compositorEGLDisplay(), m_retainedSlot->eglImage);
    m_retainedSlot = std::nullopt;
}

void RemoteWCVideoSurface::releaseTextures()
{
    if (m_currentSlot >= 0 && static_cast<unsigned>(m_currentSlot) < m_slots.size())
        m_slots[m_currentSlot].mutex->ReleaseSync(0);
    m_currentSlot = -1;
    for (auto& slot : m_slots) {
        if (slot.eglImage && destroyImageKHR())
            destroyImageKHR()(compositorEGLDisplay(), slot.eglImage);
    }
    m_slots.clear();
}

void RemoteWCVideoSurface::invalidate()
{
    if (m_invalidated)
        return;
    m_invalidated = true;
    releasePictureInPicture();
    releaseRetainedSlot();
    releaseTextures();
    // Detaches the layer from WCScene and WCContentBufferManager.
    if (client())
        client()->platformLayerWillBeDestroyed();
}

} // namespace WebKit

#endif // USE(GRAPHICS_LAYER_WC) && USE(GSTREAMER)

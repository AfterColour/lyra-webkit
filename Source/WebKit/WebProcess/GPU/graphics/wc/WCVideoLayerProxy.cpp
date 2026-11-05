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
#include "WCVideoLayerProxy.h"

#if USE(GRAPHICS_LAYER_WC) && USE(GSTREAMER)

#include "GPUConnectionToWebProcessMessages.h"
#include "GPUProcessConnection.h"
#include "WebProcess.h"
#include <WebCore/MediaTraceWin.h>
#include <atomic>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wtf/MainThread.h>

namespace WebKit {
using namespace WebCore;

static uint64_t nextIdentifier()
{
    static std::atomic<uint64_t> identifier { 0 };
    return ++identifier;
}

void WCVideoLayerProxy::registerFactory()
{
    AcceleratedVideoLayerWin::setFactory([]() -> RefPtr<AcceleratedVideoLayerWin> {
        Ref connection = WebProcess::singleton().ensureGPUProcessConnection().connection();
        // Asked once per web process (the GPU process and its adapter do not change underneath it
        // without the connection going away, which fails the layers anyway).
        static std::optional<int64_t> adapterLUID;
        if (!adapterLUID) {
            auto sendResult = connection->sendSync(Messages::GPUConnectionToWebProcess::GetWCVideoAdapterLUID(), 0);
            auto [luid] = sendResult.takeReplyOr(0);
            adapterLUID = luid;
            MEDIA_TRACE("ANGLE adapter LUID from the GPU process: %08llx", static_cast<unsigned long long>(luid));
        }
        return adoptRef(*new WCVideoLayerProxy(WTF::move(connection), *adapterLUID));
    });
}

WCVideoLayerProxy::WCVideoLayerProxy(Ref<IPC::Connection>&& connection, int64_t adapterLUID)
    : m_identifier(nextIdentifier())
    , m_connection(WTF::move(connection))
    , m_adapterLUID(adapterLUID)
{
    ASSERT(isMainThread());
    m_connection->send(Messages::GPUConnectionToWebProcess::CreateWCVideoSurface(m_identifier), 0);
}

WCVideoLayerProxy::~WCVideoLayerProxy()
{
    // Can run on a GStreamer thread when the player goes away there; IPC::Connection::send is thread-safe.
    m_connection->send(Messages::GPUConnectionToWebProcess::ReleaseWCVideoSurface(m_identifier), 0);
}

bool WCVideoLayerProxy::ensureTextures(ID3D11Device* device, DXGI_FORMAT format, unsigned width, unsigned height)
{
    if (m_device.Get() == device && m_format == format && m_width == width && m_height == height && m_slots[0].texture)
        return true;

    for (auto& slot : m_slots)
        slot = { };
    m_device = device;
    m_format = format;
    m_width = width;
    m_height = height;
    m_nextSlot = 0;

    D3D11_TEXTURE2D_DESC desc { };
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    // ANGLE binds the texture as a GL texture (shader resource) and may also render to it.
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    Vector<Win32Handle> handles;
    for (auto& slot : m_slots) {
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &slot.texture);
        if (FAILED(hr)) {
            MEDIA_TRACE("CreateTexture2D %ux%u format %d failed: 0x%08lx", width, height, static_cast<int>(format), static_cast<unsigned long>(hr));
            return false;
        }
        slot.texture.As(&slot.mutex);
        Microsoft::WRL::ComPtr<IDXGIResource1> resource;
        HANDLE handle = nullptr;
        if (!slot.mutex || FAILED(slot.texture.As(&resource))
            || FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle))) {
            MEDIA_TRACE("could not share the texture");
            return false;
        }
        handles.append(Win32Handle::adopt(handle));
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapterDesc { };
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&adapterDesc)))
        MEDIA_TRACE("decoder adapter LUID %08lx:%08lx", adapterDesc.AdapterLuid.HighPart, adapterDesc.AdapterLuid.LowPart);
    MEDIA_TRACE("created %u shared textures %ux%u", textureCount, width, height);

    m_connection->send(Messages::GPUConnectionToWebProcess::SetWCVideoSurfaceTextures(m_identifier, WTF::move(handles), width, height), 0);
    return true;
}

bool WCVideoLayerProxy::pushFrame(ID3D11Texture2D* source, unsigned subresource, unsigned width, unsigned height)
{
    Locker locker { m_lock };
    if (m_failed)
        return false;

    D3D11_TEXTURE2D_DESC sourceDesc;
    source->GetDesc(&sourceDesc);
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    source->GetDevice(&device);
    width = std::min(width, sourceDesc.Width);
    height = std::min(height, sourceDesc.Height);
    if (!device || !width || !height)
        return false;

    if (!ensureTextures(device.Get(), sourceDesc.Format, width, height)) {
        m_failed = true;
        return false;
    }
    if (HRESULT reason = device->GetDeviceRemovedReason(); FAILED(reason)) {
        // TDR, driver update, GPU unplugged: the shared textures are gone with the device.
        MEDIA_TRACE("decoder device removed: 0x%08lx", static_cast<unsigned long>(reason));
        m_failed = true;
        return false;
    }

    // Key 0: the GPU process is not showing this texture (a slot queued for presentation was
    // released with key 1 and cannot be taken). Never wait here (GStreamer streaming thread): take
    // the next free slot in order, and drop the frame only if all of them are busy.
    unsigned slotIndex = m_nextSlot;
    HRESULT hr = E_FAIL;
    for (unsigned attempt = 0; attempt < textureCount; ++attempt) {
        slotIndex = (m_nextSlot + attempt) % textureCount;
        hr = m_slots[slotIndex].mutex->AcquireSync(0, 0);
        if (hr == S_OK)
            break;
    }
    if (hr != S_OK) {
        MEDIA_TRACE("all %u slots in use (0x%08lx), frame dropped", textureCount, static_cast<unsigned long>(hr));
        return true;
    }
    auto& slot = m_slots[slotIndex];

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    D3D11_BOX box { 0, 0, 0, width, height, 1 };
    context->CopySubresourceRegion(slot.texture.Get(), 0, 0, 0, 0, source, subresource, &box);
    slot.mutex->ReleaseSync(1);
    m_nextSlot = (slotIndex + 1) % textureCount;

    m_connection->sendWithAsyncReply(Messages::GPUConnectionToWebProcess::PresentWCVideoSurface(m_identifier, slotIndex),
        [protectedThis = Ref { *this }](std::optional<WCContentBufferIdentifier> identifier, bool succeeded) {
            // A reply cancelled because the GPU process went away arrives as (nullopt, false).
            protectedThis->didPresent(identifier, succeeded);
        }, 0);
    return true;
}

void WCVideoLayerProxy::setFailureHandler(Function<void()>&& handler)
{
    ASSERT(isMainThread());
    m_failureHandler = WTF::move(handler);
}

void WCVideoLayerProxy::notifyFailure()
{
    if (m_failureNotified.exchange(true))
        return;
    callOnMainThread([protectedThis = Ref { *this }] {
        if (auto handler = std::exchange(protectedThis->m_failureHandler, nullptr))
            handler();
    });
}

void WCVideoLayerProxy::didPresent(std::optional<WCContentBufferIdentifier> identifier, bool succeeded)
{
    ASSERT(isMainThread());
    if (!succeeded) {
        MEDIA_TRACE("the GPU process could not show the frame");
        notifyFailure();
        return;
    }
    // No identifier: the GPU process already composited the frame itself (or the previous identifier
    // is still on its way). Only attaching the layer to the scene needs a layer flush.
    if (!identifier)
        return;
    m_layer.addContentBufferIdentifier(*identifier);
    m_layer.contentsChanged();
}

} // namespace WebKit

#endif // USE(GRAPHICS_LAYER_WC) && USE(GSTREAMER)

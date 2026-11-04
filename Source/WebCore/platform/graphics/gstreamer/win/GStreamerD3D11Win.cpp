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
#include "GStreamerD3D11Win.h"

#if OS(WINDOWS) && USE(GRAPHICS_LAYER_WC) && USE(GSTREAMER)

#include "AcceleratedVideoLayerWin.h"
#include "MediaTraceWin.h"
#include <gst/video/video.h>

#ifndef GST_USE_UNSTABLE_API
#define GST_USE_UNSTABLE_API
#endif
#include <gst/d3d11/gstd3d11.h>
#include <atomic>
#include <mutex>
#include <wtf/Threading.h>

namespace WebCore {

static std::atomic<int64_t> s_adapterLUID { 0 };

static GstD3D11Device* sharedD3D11Device()
{
    static GstD3D11Device* device = [] {
        MEDIA_TRACE_TIME("create shared GstD3D11Device");
        // The flags GStreamer's own elements use.
        constexpr guint flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        int64_t luid = s_adapterLUID.load();
        GstD3D11Device* device = nullptr;
        if (luid)
            device = gst_d3d11_device_new_for_adapter_luid(luid, flags);
        if (!device)
            device = gst_d3d11_device_new(0, flags);
        MEDIA_TRACE("GstD3D11Device for adapter LUID %08llx: %s", static_cast<unsigned long long>(luid), device ? "ok" : "failed");
        return device;
    }();
    return device;
}

bool handleGStreamerD3D11NeedContext(GstElement* element, const char* contextType)
{
    if (g_strcmp0(contextType, GST_D3D11_DEVICE_HANDLE_CONTEXT_TYPE))
        return false;
    auto* device = sharedD3D11Device();
    if (!device)
        return false;
    GstContext* context = gst_d3d11_context_new(device);
    gst_element_set_context(element, context);
    gst_context_unref(context);
    MEDIA_TRACE("shared D3D11 device given to %s", GST_ELEMENT_NAME(element));
    return true;
}

void prewarmGStreamerD3D11(int64_t adapterLUID)
{
    static std::once_flag once;
    std::call_once(once, [adapterLUID] {
        s_adapterLUID = adapterLUID;
        Thread::create("GStreamer D3D11 warm-up"_s, [] {
            sharedD3D11Device();
        })->detach();
    });
}

bool pushGStreamerD3D11Frame(AcceleratedVideoLayerWin& layer, GstBuffer* buffer, GstCaps* caps)
{
    GstMemory* memory = buffer ? gst_buffer_peek_memory(buffer, 0) : nullptr;
    if (!memory || !gst_is_d3d11_memory(memory)) {
        MEDIA_TRACE("frame is not in D3D11 memory");
        return false;
    }

    GstVideoInfo info;
    if (!caps || !gst_video_info_from_caps(&info, caps))
        return false;

    auto* d3d11Memory = GST_D3D11_MEMORY_CAST(memory);
    GstMapInfo mapInfo;
    // GST_MAP_D3D11 gives the ID3D11Resource and makes sure pending CPU writes reached the texture.
    if (!gst_memory_map(memory, &mapInfo, static_cast<GstMapFlags>(GST_MAP_READ | GST_MAP_D3D11))) {
        MEDIA_TRACE("gst_memory_map(GST_MAP_D3D11) failed");
        return false;
    }

    ID3D11Texture2D* texture = nullptr;
    reinterpret_cast<ID3D11Resource*>(mapInfo.data)->QueryInterface(IID_PPV_ARGS(&texture));
    bool pushed = false;
    if (texture) {
        // The decoder's device has one immediate context, shared with the GStreamer elements.
        gst_d3d11_device_lock(d3d11Memory->device);
        pushed = layer.pushFrame(texture, gst_d3d11_memory_get_subresource_index(d3d11Memory),
            GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info));
        gst_d3d11_device_unlock(d3d11Memory->device);
        texture->Release();
    }
    gst_memory_unmap(memory, &mapInfo);
    MEDIA_TRACE("%dx%d pushed=%d", GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info), pushed);
    return pushed;
}

} // namespace WebCore

#endif

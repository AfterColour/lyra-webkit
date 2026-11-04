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

#if OS(WINDOWS) && USE(GRAPHICS_LAYER_WC) && USE(GSTREAMER)

#include <gst/gst.h>

namespace WebCore {

class AcceleratedVideoLayerWin;

// Hands a frame in D3D11 memory (memory:D3D11Memory, BGRA) to the video layer. Kept out of the
// GStreamer unified sources: it needs <d3d11.h> and therefore <windows.h>.
bool pushGStreamerD3D11Frame(AcceleratedVideoLayerWin&, GstBuffer*, GstCaps*);

// One GstD3D11Device for every pipeline of the process (need-context "gst.d3d11.device.handle").
// Without it each pipeline (every video, ad and some quality switches on YouTube) creates its own
// device and d3d11convert compiles its shaders again, which shows up as a stall when a video starts.
bool handleGStreamerD3D11NeedContext(GstElement*, const char* contextType);

// Creates that device, on the given adapter (0: the default one), on a background thread ahead of
// the first pipeline that needs it. The first call decides the adapter.
void prewarmGStreamerD3D11(int64_t adapterLUID);

} // namespace WebCore

#endif

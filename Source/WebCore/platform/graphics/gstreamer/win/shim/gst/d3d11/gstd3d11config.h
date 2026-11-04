/*
 * MyBrowser: vcpkg's gstreamer port installs the GStreamer D3D11 library headers but not this
 * meson-generated one, which every other header of the library includes. Same contents as the
 * generated file of the desktop (non-UWP) build.
 */
#pragma once

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_D3D11_WINAPI_ONLY_APP 0
#define GST_D3D11_WINAPI_APP 1

G_END_DECLS

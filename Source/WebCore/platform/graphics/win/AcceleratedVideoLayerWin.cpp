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
#include "AcceleratedVideoLayerWin.h"

#if OS(WINDOWS) && USE(GRAPHICS_LAYER_WC)

#include <cstdlib>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>

namespace WebCore {

static AcceleratedVideoLayerWin::Factory& factory()
{
    static NeverDestroyed<AcceleratedVideoLayerWin::Factory> factory;
    return factory;
}

void AcceleratedVideoLayerWin::setFactory(Factory&& newFactory)
{
    ASSERT(isMainThread());
    factory() = WTF::move(newFactory);
}

RefPtr<AcceleratedVideoLayerWin> AcceleratedVideoLayerWin::create()
{
    ASSERT(isMainThread());
    static bool disabled = [] {
        const char* value = std::getenv("MYBROWSER_DISABLE_D3D11_VIDEO");
        return value && *value == '1';
    }();
    if (disabled || !factory())
        return nullptr;
    return factory()();
}

} // namespace WebCore

#endif // OS(WINDOWS) && USE(GRAPHICS_LAYER_WC)

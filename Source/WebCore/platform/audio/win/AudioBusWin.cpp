/*
 * Copyright (C) 2026 Lyra
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#if ENABLE(WEB_AUDIO)

#include "AudioBus.h"

#include "AudioFileReader.h"
#include <fstream>
#include <iterator>
#include <vector>
#include <windows.h>

namespace WebCore {

// Lyra: the audio resources (the HRTF impulse responses of PannerNode) are files next to
// WebCore.dll, in WebKit.resources\audio (the GTK and WPE ports keep them in a GResource).
RefPtr<AudioBus> AudioBus::loadPlatformResource(StringView name, float sampleRate)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&AudioBus::loadPlatformResource), &module))
        return nullptr;

    wchar_t modulePath[MAX_PATH] = { };
    DWORD length = GetModuleFileNameW(module, modulePath, MAX_PATH);
    if (!length || length >= MAX_PATH)
        return nullptr;
    std::wstring path(modulePath, length);
    path.erase(path.find_last_of(L"\\/") + 1);
    path += L"WebKit.resources\\audio\\";
    for (char16_t c : name.codeUnits())
        path += static_cast<wchar_t>(c);
    if (!name.endsWith(".wav"_s))
        path += L".wav";

    std::ifstream file(path, std::ios::binary);
    if (!file)
        return nullptr;
    std::vector<uint8_t> data { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    if (data.empty())
        return nullptr;
    return createBusFromInMemoryAudioFile(std::span<const uint8_t> { data.data(), data.size() }, false, sampleRate);
}

} // namespace WebCore

#endif // ENABLE(WEB_AUDIO)

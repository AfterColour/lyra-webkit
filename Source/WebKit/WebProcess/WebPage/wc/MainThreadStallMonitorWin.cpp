/*
 * Copyright (C) 2026 Lyra contributors. All rights reserved.
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
#include "MainThreadStallMonitorWin.h"

#if OS(WINDOWS)

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <windows.h>
#include <dbghelp.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/text/ASCIILiteral.h>

namespace WebKit {

static std::atomic<int64_t> s_lastMainThreadTick { 0 };

static int64_t stallMonitorNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

using MiniDumpWriteDumpFunction = BOOL (WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

static void writeStallDump(const char* folder, int64_t stalledMs, unsigned index)
{
    static MiniDumpWriteDumpFunction writeDump = [] () -> MiniDumpWriteDumpFunction {
        HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
        return dbghelp ? reinterpret_cast<MiniDumpWriteDumpFunction>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
    }();
    if (!writeDump)
        return;
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\stall-WebKitWebProcess-%lu-%u-%lldms.dmp", folder, GetCurrentProcessId(), index, static_cast<long long>(stalledMs));
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo), nullptr, nullptr, nullptr);
    CloseHandle(file);
}

void startMainThreadStallMonitor()
{
    const char* folder = std::getenv("MYBROWSER_STALL_DUMPS");
    if (!folder || !*folder)
        return;
    int64_t thresholdMs = 700;
    if (const char* value = std::getenv("MYBROWSER_STALL_MS"); value && std::atoi(value) > 0)
        thresholdMs = std::atoi(value);

    s_lastMainThreadTick = stallMonitorNowMs();
    // The heartbeat: a main thread timer. It does not fire while the main thread is busy.
    static NeverDestroyed<RunLoop::Timer> heartbeat(Ref { RunLoop::mainSingleton() }, "StallMonitorHeartbeat"_s, [] {
        s_lastMainThreadTick = stallMonitorNowMs();
    });
    heartbeat->startRepeating(50_ms);

    std::thread([folder = std::string(folder), thresholdMs] {
        unsigned dumps = 0;
        bool inStall = false;
        while (dumps < 20) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            int64_t stalled = stallMonitorNowMs() - s_lastMainThreadTick.load();
            if (stalled < thresholdMs) {
                inStall = false;
                continue;
            }
            if (inStall)
                continue;
            // One dump per stall, taken while the main thread is still stuck.
            inStall = true;
            writeStallDump(folder.c_str(), stalled, ++dumps);
        }
    }).detach();
}

} // namespace WebKit

#endif

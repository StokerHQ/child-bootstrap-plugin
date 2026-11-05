#pragma once

#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#include <cstring>

// The pipe's per-user name and its owner check, as stoker-runtime has them: the launcher serves this pipe.
#include "agent_pipe_windows.h"

namespace stoker::plugin
{
    // This request deliberately uses the local pipe even when the SDK uses TCP. A launch token
    // identifies the run, not the calling process; only the kernel can authenticate its parent.
    // No shim initialization, hello, or additional Stoker client is needed in the parent here.
    inline bool TransferSession(DWORD child) noexcept
    {
        wchar_t endpoint[512], configured[480];
        const DWORD length = GetEnvironmentVariableW(L"STOKER_AGENT_PIPE", configured, 480);
        if (length >= 480) return false;
        if (!length && wcscpy_s(configured, stoker::agentpipe::DefaultName().c_str()) != 0) return false;
        if (wcsncmp(configured, L"\\\\.\\pipe\\", 9) == 0) wcscpy_s(endpoint, configured);
        else if (swprintf_s(endpoint, L"\\\\.\\pipe\\%s", configured) <= 0) return false;

        constexpr DWORD timeout = 5000;
        const ULONGLONG deadline = GetTickCount64() + timeout;
        HANDLE pipe = INVALID_HANDLE_VALUE;
        do
        {
            pipe = CreateFileW(endpoint, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) break;
            if (GetLastError() != ERROR_PIPE_BUSY) return false;
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline || !WaitNamedPipeW(endpoint, static_cast<DWORD>(deadline - now))) return false;
        } while (GetTickCount64() < deadline);
        if (pipe == INVALID_HANDLE_VALUE) return false;

        // A transfer names a process for the launcher to adopt; it goes to this user's launcher or nowhere.
        if (!stoker::agentpipe::ServedByCurrentUser(pipe)) { CloseHandle(pipe); return false; }

        const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) { CloseHandle(pipe); return false; }
        auto io = [&](void* buffer, DWORD bytes, bool write)
        {
            auto cursor = static_cast<unsigned char*>(buffer);
            while (bytes)
            {
                OVERLAPPED pending{};
                pending.hEvent = event;
                ResetEvent(event);
                DWORD done = 0;
                BOOL ok = write ? WriteFile(pipe, cursor, bytes, &done, &pending) : ReadFile(pipe, cursor, bytes, &done, &pending);
                if (!ok && GetLastError() == ERROR_IO_PENDING)
                {
                    const ULONGLONG now = GetTickCount64();
                    if (WaitForSingleObject(event, static_cast<DWORD>(now < deadline ? deadline - now : 0)) != WAIT_OBJECT_0)
                    {
                        CancelIoEx(pipe, &pending);
                        // The OVERLAPPED and caller buffer remain alive until cancellation finishes.
                        GetOverlappedResult(pipe, &pending, &done, TRUE);
                        return false;
                    }
                    ok = GetOverlappedResult(pipe, &pending, &done, FALSE);
                }
                if (!ok || !done || done > bytes) return false;
                bytes -= done;
                cursor += done;
            }
            return true;
        };

        char request[160];
        const int count = sprintf_s(request, "{\"v\":1,\"type\":\"request\",\"id\":1,\"op\":\"session.transfer\",\"body\":{\"processId\":%lu}}", child);
        DWORD frame = count > 0 ? static_cast<DWORD>(count) : 0;
        char response[2048]{};
        bool accepted = frame && io(&frame, 4, true) && io(request, frame, true) && io(&frame, 4, false)
            && frame > 0 && frame < sizeof response && io(response, frame, false);
        // The agent's response is compact JSON. Accept only its success field, not an error string
        // containing that text. It is the trusted local agent, not data returned by the game.
        accepted = accepted && std::strstr(response, "\"ok\":true") != nullptr;
        CloseHandle(event);
        CloseHandle(pipe);
        return accepted;
    }
}
#endif

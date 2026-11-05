#pragma once

// The launcher's pipe on Windows: its name, and whether the process serving it is ours. Header-only,
// because the hook plugins (integrations/hook-plugins/common/session_transfer.h) reach the pipe without
// linking stoker-runtime, and the two must agree on both.
//
// Per user: \\.\pipe\stoker-agent-<SID>. Pipe names are one namespace for the whole machine, not per
// session, so with two people logged in at once (fast user switching, Remote Desktop) a second launcher
// could not create a shared name - the first one's ACL refuses everyone else - and its games would fall
// to loopback TCP. The SID keeps each user's launcher on a name of its own.
//
// The name is not what keeps a game from talking to an impostor: anyone can work out a SID, and any
// process on the machine may create a pipe of that name before the launcher does. The check is that the
// process serving it runs as the same user as the game (ServedByCurrentUser), asked of the kernel after
// connecting - the counterpart of the agent checking its client. A pipe that fails it is treated as no
// launcher, and the game goes on to TCP, whose token only the real launcher knows.

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>

#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace stoker::agentpipe
{
    /// The user a process runs as, as a SID string ("S-1-5-21-..."); empty when it cannot be asked.
    inline std::wstring UserSidOf(HANDLE process)
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return {};

        std::wstring result;
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);

        if (size > 0)
        {
            std::vector<unsigned char> buffer(size);
            LPWSTR text = nullptr;

            if (GetTokenInformation(token, TokenUser, buffer.data(), size, &size)
                && ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text))
            {
                result = text;
                LocalFree(text);
            }
        }

        CloseHandle(token);
        return result;
    }

    inline const std::wstring& CurrentUserSid()
    {
        static const std::wstring sid = UserSidOf(GetCurrentProcess());
        return sid;
    }

    /// "stoker-agent-<SID>", the name the launcher serves for this user. The old shared name when the SID
    /// cannot be read, which the launcher still answers on too (WindowsAgentPlatform.LegacyLocalEndpoints).
    inline std::wstring DefaultName()
    {
        const std::wstring& sid = CurrentUserSid();
        return sid.empty() ? L"stoker-agent" : L"stoker-agent-" + sid;
    }

    /// Whether the process serving `pipe` runs as the same user as this one. False when Windows will not
    /// say: a launcher that cannot be shown to be ours is not one.
    inline bool ServedByCurrentUser(HANDLE pipe)
    {
        ULONG serverId = 0;
        if (!GetNamedPipeServerProcessId(pipe, &serverId) || serverId == 0) return false;

        const HANDLE server = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, serverId);
        if (server == nullptr) return false;

        const std::wstring sid = UserSidOf(server);
        CloseHandle(server);

        return !sid.empty() && sid == CurrentUserSid();
    }
}

#endif

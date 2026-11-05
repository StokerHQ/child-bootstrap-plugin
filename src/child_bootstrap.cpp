// child-bootstrap: loads a Stoker bootstrap into a child process that would never load one itself,
// and runs that bootstrap's startup hook pass there, all before the child runs its own code.
//
// Why: a bootstrap - and with it the hook engine and every plugin - only reaches a process that
// loads it. This plugin keeps control when one program starts another, as a launcher in front of the program it starts does.
//
// How: CreateProcessW is detoured. A child whose executable is named by `executable` is created
// suspended - whatever the caller asked for - then the bootstrap in the child's own folder is loaded
// into it by the remote Windows loader, and the child's startup pass is requested over the bootstrap's
// startup channel (Local\StokerBootstrapHooksStartup-v1-<pid>-*, hooks_windows.cpp) exactly as its
// own first API call would. Only when the pass has completed - plugins initialized, detours
// enabled - is the child's first thread resumed. Nothing about the child is decided by waiting and
// hoping: a hand-mapped library 120 ms in is a race any asynchronous approach would lose.
//
// Rejected: an APC queued to the child's main thread. It runs LoadLibraryW early enough, but the
// bootstrap's hook worker only starts after it, and the main thread would already be running the
// game by then. A proxy DLL the child does load (VERSION.dll) would also work, but is a new kind
// of bootstrap with its own export list to keep, for every game that needs it.
//
// When anything fails, the child is resumed untouched and runs as it would have without Stoker.
// Never killed: a game that loses Stoker features is better than a Play button that does nothing.
//
// Options (configJson):
//   executable  required  file name of the child to take: "SecondProgram.exe"
//   bootstrap   required  file name of the bootstrap to load, found in the child's own folder:
//                         "steam_api64.dll". Its stoker_hooks.json is the child's configuration.
//   process     optional  only act inside this executable: "Launcher.exe" (plugin_support.h)

#include "stoker/plugin_support.h"

STOKER_PLUGIN_MANIFEST(R"json({
  "manifestVersion": 1,
  "id": "app.stokerhq.child-bootstrap",
  "name": "Child bootstrap and session transfer",
  "type": "hook",
  "version": "1.0.3",
  "abi": 1,
  "description": "Loads a Stoker bootstrap into a suspended child and transfers the existing run to that child before it starts.",
  "hosts": ["steamworks", "ubisoft", "galaxy", "eos"],
  "config": {
    "type": "object",
    "additionalProperties": false,
    "properties": {
      "process": {
        "type": "string", "title": "Parent executable",
        "description": "Only act inside this executable. Omit to act in every process. File name only, compared without regard to case.",
        "minLength": 1, "maxLength": 259, "pattern": "^(?!\\.{1,2}$)[^\\\\/:]+$", "examples": ["Launcher.exe"]
      },
      "executable": {
        "type": "string", "title": "Child executable",
        "description": "The child whose bootstrap and session should be prepared before its main thread runs. File name only.",
        "minLength": 1, "maxLength": 259, "pattern": "^(?!\\.{1,2}$)[^\\\\/:]+$", "examples": ["Game.exe"]
      },
      "bootstrap": {
        "type": "string", "title": "Bootstrap DLL",
        "description": "Loaded from the child's executable folder, where stoker_hooks.json must also be installed.",
        "minLength": 1, "maxLength": 259, "pattern": "^(?!\\.{1,2}$)[^\\\\/:]+$", "examples": ["steam_api64.dll"]
      }
    },
    "required": ["executable", "bootstrap"]
  }
})json");

#ifdef _WIN32
#include "session_transfer.h"
// The loading and the startup pass are the same as the Stoker launcher's stoker-inject, which starts a
// game itself this way when the game's own bootstrap would arrive too late.
#include "remote_bootstrap.h"
namespace
{
    using stoker::plugin::Log;

    using CreateProcessWFn = BOOL (WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                           LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
    CreateProcessWFn g_real = nullptr;

    wchar_t g_executable[MAX_PATH];
    wchar_t g_bootstrap[MAX_PATH];

    /// The executable a CreateProcessW call names, before the process exists: the application name,
    /// or the command line's first token. Only a pre-filter - `Take` checks the image actually created.
    bool Names(LPCWSTR application, LPCWSTR commandLine)
    {
        wchar_t path[MAX_PATH * 2];
        if (application)
        {
            if (wcsncpy_s(path, application, _TRUNCATE) != 0) return false;
        }
        else if (commandLine)
        {
            const wchar_t* c = commandLine;
            while (*c == L' ' || *c == L'\t') ++c;
            const wchar_t end = *c == L'"' ? L'"' : L' ';
            if (*c == L'"') ++c;
            size_t length = 0;
            while (c[length] && c[length] != end && (end == L'"' || c[length] != L'\t') && length < MAX_PATH * 2 - 1) ++length;
            wmemcpy(path, c, length);
            path[length] = 0;
        }
        else return false;

        if (stoker::plugin::NamesFile(path, g_executable)) return true;
        // "Game" for "Game.exe": CreateProcess appends .exe to a command line that omits it.
        wchar_t withExtension[MAX_PATH * 2];
        return swprintf_s(withExtension, L"%s.exe", path) > 0 && stoker::plugin::NamesFile(withExtension, g_executable);
    }

    /// Everything between "created suspended" and "resumed", for a child that `Names` matched.
    void Take(const PROCESS_INFORMATION& child)
    {
        wchar_t image[MAX_PATH * 2];
        DWORD length = MAX_PATH * 2;
        if (!QueryFullProcessImageNameW(child.hProcess, 0, image, &length))
        {
            Log("could not read the child's executable (error %lu); leaving it alone", GetLastError());
            return;
        }
        if (!stoker::plugin::NamesFile(image, g_executable)) return;
        if (!stoker::remote::SameArchitecture(child.hProcess))
        {
            Log("pid %lu is not this plugin's architecture; upload the other build for it", child.dwProcessId);
            return;
        }

        // The caller is still inside CreateProcess, and the child's main thread is suspended.
        // Register the handoff now, while the agent can verify both live processes. Failure of
        // injection must not itself make playtime stop when the parent exits.
        if (stoker::plugin::TransferSession(child.dwProcessId))
            Log("pid %lu: transferred the existing Stoker session to the child", child.dwProcessId);
        else
            Log("pid %lu: session transfer was not acknowledged; update the launcher or check the agent endpoint", child.dwProcessId);

        wchar_t bootstrap[MAX_PATH * 2];
        wcscpy_s(bootstrap, image);
        *const_cast<wchar_t*>(stoker::plugin::FileName(bootstrap)) = 0;
        if (wcscat_s(bootstrap, g_bootstrap) != 0) return;
        if (GetFileAttributesW(bootstrap) == INVALID_FILE_ATTRIBUTES)
        {
            Log("pid %lu: there is no bootstrap at %ls", child.dwProcessId, bootstrap);
            return;
        }

        if (!stoker::remote::LoadInto(child.hProcess, child.dwProcessId, bootstrap, Log)) return;
        if (stoker::remote::RunStartupPass(child.dwProcessId, Log))
            Log("pid %lu (%ls): loaded %ls and ran its startup hook pass", child.dwProcessId, stoker::plugin::FileName(image), g_bootstrap);
    }

    BOOL WINAPI HookedCreateProcessW(LPCWSTR application, LPWSTR commandLine, LPSECURITY_ATTRIBUTES processAttributes,
                                     LPSECURITY_ATTRIBUTES threadAttributes, BOOL inheritHandles, DWORD flags, LPVOID environment,
                                     LPCWSTR directory, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION information)
    {
        const DWORD incomingError = GetLastError();
        bool wanted = false;
        __try { wanted = information != nullptr && Names(application, commandLine); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        SetLastError(incomingError);
        if (!wanted)
            return g_real(application, commandLine, processAttributes, threadAttributes, inheritHandles, flags, environment,
                          directory, startup, information);

        if (!g_real(application, commandLine, processAttributes, threadAttributes, inheritHandles, flags | CREATE_SUSPENDED,
                    environment, directory, startup, information)) return FALSE;

        const DWORD error = GetLastError();
        Take(*information);
        if (!(flags & CREATE_SUSPENDED) && ResumeThread(information->hThread) == static_cast<DWORD>(-1))
            Log("could not resume pid %lu (error %lu)", information->dwProcessId, GetLastError());
        SetLastError(error);
        return TRUE;
    }

    bool ReadFileNameOption(const char* key, wchar_t* output)
    {
        char value[MAX_PATH];
        return stoker::plugin::Option(key, value, sizeof value) == stoker::plugin::OptionRead::Present &&
               stoker::plugin::PlainFileName(value) && stoker::plugin::Wide(value, output, MAX_PATH);
    }
}
#endif

STOKER_HOOK_PLUGIN_ENTRY int STOKER_HOOK_CALL stoker_hook_plugin_init(const stoker_hook_host_v1* host)
{
    if (!stoker::plugin::Accept(host)) return STOKER_HOOK_UNSUPPORTED;
#ifdef _WIN32
    bool here = false;
    if (!stoker::plugin::ForThisProcess(here)) return STOKER_HOOK_INVALID_ARGUMENT;
    if (!here) return STOKER_HOOK_OK;
    if (!ReadFileNameOption("executable", g_executable) || !ReadFileNameOption("bootstrap", g_bootstrap))
    {
        Log("options need a file name in both \"executable\" and \"bootstrap\"");
        return STOKER_HOOK_INVALID_ARGUMENT;
    }
    Log("will load %ls into %ls", g_bootstrap, g_executable);
    return host->hook_export(host->user, "kernel32.dll", "CreateProcessW", reinterpret_cast<void*>(&HookedCreateProcessW),
                             reinterpret_cast<void**>(&g_real));
#else
    host->log(host->user, "child-bootstrap is Windows only");
    return STOKER_HOOK_UNSUPPORTED;
#endif
}

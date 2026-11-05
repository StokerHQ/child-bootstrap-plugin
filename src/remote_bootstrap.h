// Loads a bootstrap into a process created suspended, and runs its startup hook pass there, before
// that process runs any code of its own. This plugin does it from inside a game's own launcher for a
// child that loads no bootstrap; the Stoker launcher's stoker-inject does the same for a game whose
// bootstrap would otherwise arrive too late, with the same code.
//
// Header-only because the plugin is built as one source file by the SDK's build, and the injector
// as another; neither links anything of the bootstrap's. The startup channel's names are the
// bootstrap's own (hooks_windows.cpp) and carry a version, so renaming them there breaks this.
//
// Only the injector's own architecture: the remote loader's address is the injector's ntdll export
// translated to the child's ntdll, which is only the same image when both are the same bitness.
// The launcher is always x64 and carries one injector per architecture for that reason.

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <cstring>
#include <cwchar>

namespace stoker::remote
{
    /// printf-style; each user passes its own log.
    using LogFn = void (*)(const char* format, ...) noexcept;

    /// How long the child's loader may take to load the bootstrap: the remote thread is the child's
    /// first, so it also initializes the process - every DLL the executable imports, their DllMains.
    constexpr DWORD LoadWaitMs = 30000;
    /// How long the bootstrap's worker may take to read its configuration and offer a startup pass.
    constexpr DWORD ReadyWaitMs = 5000;
    /// How long the pass itself may take: hashing and loading each plugin, initializing it,
    /// enabling its detours.
    constexpr DWORD PassWaitMs = 15000;

    inline const wchar_t* FileName(const wchar_t* path)
    {
        const wchar_t* name = path;
        for (const wchar_t* c = path; *c; ++c)
            if (*c == L'\\' || *c == L'/') name = c + 1;
        return name;
    }

    /// Whether `path` ends in the file name `name`, case-insensitively as Windows compares them.
    inline bool NamesFile(const wchar_t* path, const wchar_t* name)
    {
        return CompareStringOrdinal(FileName(path), -1, name, -1, TRUE) == CSTR_EQUAL;
    }

    inline bool SameArchitecture(HANDLE child)
    {
        BOOL childWow = FALSE, selfWow = FALSE;
        return IsWow64Process(child, &childWow) && IsWow64Process(GetCurrentProcess(), &selfWow) && childWow == selfWow;
    }

    // Only ntdll is guaranteed to be mapped before process initialization. A remote thread runs
    // after that initialization, but its start address must exist when CreateRemoteThread is called.
    // Calling LdrLoadDll through a small ABI adapter avoids guessing where KernelBase will appear.
    inline void* RemoteLoader(HANDLE child, DWORD pid)
    {
        const auto local = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrLoadDll");
        HMODULE owner = nullptr;
        wchar_t path[MAX_PATH * 2];
        if (!local || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(local), &owner) || !GetModuleFileNameW(owner, path, MAX_PATH * 2)) return nullptr;
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
        void* result = nullptr;
        if (snapshot != INVALID_HANDLE_VALUE)
        {
            MODULEENTRY32W entry{};
            entry.dwSize = sizeof entry;
            if (Module32FirstW(snapshot, &entry)) do
            {
                if (NamesFile(path, entry.szModule))
                {
                    const auto offset = reinterpret_cast<uintptr_t>(local) - reinterpret_cast<uintptr_t>(owner);
                    if (offset < entry.modBaseSize) result = entry.modBaseAddr + offset;
                    break;
                }
            } while (Module32NextW(snapshot, &entry));
            CloseHandle(snapshot);
        }
        if (result) return result;
        // A freshly suspended child's loader list is not initialized yet (Toolhelp returns
        // ERROR_PARTIAL_COPY). Its system images are mapped nevertheless. Query the mappings
        // themselves, which does not depend on that list being ready.
        MEMORY_BASIC_INFORMATION region{};
        uintptr_t cursor = 0;
        while (VirtualQueryEx(child, reinterpret_cast<void*>(cursor), &region, sizeof region))
        {
            wchar_t remotePath[MAX_PATH * 2];
            if (region.Type == MEM_IMAGE && region.BaseAddress == region.AllocationBase &&
                GetMappedFileNameW(child, region.AllocationBase, remotePath, MAX_PATH * 2) &&
                NamesFile(remotePath, FileName(path)))
            {
                result = static_cast<unsigned char*>(region.AllocationBase) +
                    (reinterpret_cast<uintptr_t>(local) - reinterpret_cast<uintptr_t>(owner));
                break;
            }
            const uintptr_t next = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
            if (next <= cursor) break;
            cursor = next;
        }
        return result;
    }

    inline bool LoadInto(HANDLE child, DWORD pid, const wchar_t* path, LogFn Log)
    {
        const auto load = RemoteLoader(child, pid);
        if (!load) { Log("could not find the child's ntdll loader"); return false; }
        const SIZE_T pathBytes = (wcslen(path) + 1) * sizeof(wchar_t);
        // First page is code, second page is writable arguments/output. No writable executable page
        // survives construction. The only remote operation is loading the configured bootstrap.
        constexpr SIZE_T bytes = 8192;
        auto remote = static_cast<unsigned char*>(VirtualAllocEx(child, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!remote) { Log("no memory in the child for the bootstrap's path (error %lu)", GetLastError()); return false; }
        struct UnicodeString { USHORT length, maximum; wchar_t* buffer; };
        UnicodeString name{static_cast<USHORT>(pathBytes - 2), static_cast<USHORT>(pathBytes), reinterpret_cast<wchar_t*>(remote + 4096 + 64)};
        unsigned char code[96]{};
        size_t length = 0;
        auto byte = [&](unsigned char value) { code[length++] = value; };
        auto pointer = [&](const void* value) { std::memcpy(code + length, &value, sizeof value); length += sizeof value; };
#ifdef _WIN64
        // sub rsp,28h; xor ecx,ecx; xor edx,edx; mov r8,name; mov r9,module; mov rax,loader;
        // call rax; add rsp,28h; ret. Return the complete NTSTATUS, not a truncated HMODULE.
        byte(0x48); byte(0x83); byte(0xec); byte(0x28);
        byte(0x31); byte(0xc9); byte(0x31); byte(0xd2);
        byte(0x49); byte(0xb8); pointer(remote + 4096);
        byte(0x49); byte(0xb9); pointer(remote + 4096 + 32);
        byte(0x48); byte(0xb8); pointer(load);
        byte(0xff); byte(0xd0);
        byte(0x48); byte(0x83); byte(0xc4); byte(0x28); byte(0xc3);
#else
        // LdrLoadDll is stdcall, as is the remote thread entry point (one unused parameter).
        byte(0x68); pointer(remote + 4096 + 32); byte(0x68); pointer(remote + 4096);
        byte(0x6a); byte(0); byte(0x6a); byte(0);
        byte(0xb8); pointer(load); byte(0xff); byte(0xd0); byte(0xc2); byte(4); byte(0);
#endif
        DWORD old = 0;
        if (pathBytes > bytes - 4096 - 64 ||
            !WriteProcessMemory(child, remote + 4096, &name, sizeof name, nullptr) ||
            !WriteProcessMemory(child, remote + 4096 + 64, path, pathBytes, nullptr) ||
            !WriteProcessMemory(child, remote, code, length, nullptr) ||
            !VirtualProtectEx(child, remote, 4096, PAGE_EXECUTE_READ, &old) ||
            !FlushInstructionCache(child, remote, length))
        {
            Log("could not prepare the child's bootstrap loader (error %lu)", GetLastError());
            VirtualFreeEx(child, remote, 0, MEM_RELEASE);
            return false;
        }

        const HANDLE thread = CreateRemoteThread(child, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(remote), nullptr, 0, nullptr);
        if (!thread)
        {
            Log("could not start a thread in the child (error %lu)", GetLastError());
            VirtualFreeEx(child, remote, 0, MEM_RELEASE);
            return false;
        }

        const DWORD waited = WaitForSingleObject(thread, LoadWaitMs);
        DWORD status = 0;
        const bool loaded = waited == WAIT_OBJECT_0 && GetExitCodeThread(thread, &status) && static_cast<LONG>(status) >= 0;
        CloseHandle(thread);

        // A thread still inside the loader still reads the path; leave it rather than free it.
        if (waited == WAIT_OBJECT_0) VirtualFreeEx(child, remote, 0, MEM_RELEASE);
        if (waited != WAIT_OBJECT_0) Log("the child did not finish loading the bootstrap within %lu s", LoadWaitMs / 1000);
        else if (!loaded) Log("the child could not load the bootstrap (status 0x%08lx)", status);
        return loaded;
    }

    inline HANDLE OpenEventPolling(const wchar_t* name, DWORD access, DWORD timeoutMs)
    {
        // The worker creates the channel only after reading a configuration that has rules or
        // plugins, on its own thread; until then the name does not exist yet.
        for (DWORD waited = 0;; waited += 10)
        {
            if (const HANDLE handle = OpenEventW(access, FALSE, name)) return handle;
            if (waited >= timeoutMs) return nullptr;
            Sleep(10);
        }
    }

    /// What the bootstrap's own first API call does in hooks::BeforeFirstCall, from outside: take
    /// the gate, ask for a pass, wait for it.
    inline bool RunStartupPass(DWORD pid, LogFn Log)
    {
        wchar_t prefix[96];
        swprintf_s(prefix, L"Local\\StokerBootstrapHooksStartup-v1-%lu", pid);
        wchar_t name[128];

        swprintf_s(name, L"%s-ready", prefix);
        const HANDLE ready = OpenEventPolling(name, SYNCHRONIZE, ReadyWaitMs);
        if (!ready)
        {
            Log("the child's bootstrap offered no startup pass within %lu s - is there a stoker_hooks.json beside it?", ReadyWaitMs / 1000);
            return false;
        }
        const bool isReady = WaitForSingleObject(ready, ReadyWaitMs) == WAIT_OBJECT_0;
        CloseHandle(ready);
        if (!isReady) { Log("the child's bootstrap did not finish its first hook pass"); return false; }

        swprintf_s(name, L"%s-request", prefix);
        const HANDLE request = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
        swprintf_s(name, L"%s-completed", prefix);
        const HANDLE completed = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);
        swprintf_s(name, L"%s-gate", prefix);
        const HANDLE gate = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name);

        bool passed = false;
        if (request && completed && gate)
        {
            const DWORD owned = WaitForSingleObject(gate, PassWaitMs);
            if (owned == WAIT_OBJECT_0 || owned == WAIT_ABANDONED)
            {
                ResetEvent(completed);
                passed = SetEvent(request) && WaitForSingleObject(completed, PassWaitMs) == WAIT_OBJECT_0;
                ReleaseMutex(gate);
            }
        }
        if (!passed) Log("the child's startup hook pass did not complete");

        const HANDLE handles[] = {request, completed, gate};
        for (const HANDLE handle : handles)
            if (handle) CloseHandle(handle);
        return passed;
    }
}

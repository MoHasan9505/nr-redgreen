// nvapi-gate - a stand-in nvapi64.dll for the game folder.
//
// Cyberpunk 2077 rendering on an AMD GPU with an NVIDIA GPU + driver also in
// the machine finds NVAPI, loads Streamline/NGX/DLSS for the NVIDIA card, and
// then fails "Ray Tracing initialization" on the AMD device - with or without
// nr-bridge loaded (tools/watch-launch.ps1, run3-addon-off).
//
// This DLL sits beside the game exe, so the game's LoadLibrary("nvapi64.dll")
// finds it first. Every caller gets the answer for its own module:
//   - the game, its Streamline and anything else in the game folder: NVAPI is
//     "not implemented" (nullptr), exactly what an AMD-only machine looks like;
//   - NVIDIA driver components (anything under %SystemRoot%), our add-on
//     (module name contains "nvngx.dll") and the private DLSS-NR snippet in
//     the mgpu\ subfolder: the real System32\nvapi64.dll, untouched.
//
// Decisions are logged once per calling module to nvapi-gate.log beside this DLL.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

#include <cstdio>
#include <cwchar>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

using PfnQueryInterface = void*(__cdecl*)(unsigned int);
using PfnDirectGetMethod = void*(__cdecl*)(void*, void*, void*, void*);  // signature not public; args passed through

HMODULE gSelf = nullptr;
std::once_flag gRealOnce;
HMODULE gReal = nullptr;
PfnQueryInterface gRealQuery = nullptr;
PfnDirectGetMethod gRealDirect = nullptr;

std::mutex gMutex;
std::unordered_map<HMODULE, bool> gDecisions;  // calling module -> allowed
std::wstring gGameDir, gWindowsDir, gLogPath;

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring moduleFile(HMODULE m) {
    wchar_t p[MAX_PATH * 2] = {};
    GetModuleFileNameW(m, p, MAX_PATH * 2);
    return p;
}

void loadReal() {
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring path = std::wstring(sys) + L"\\nvapi64.dll";
    gReal = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (gReal && gReal != gSelf) {
        gRealQuery = (PfnQueryInterface)GetProcAddress(gReal, "nvapi_QueryInterface");
        gRealDirect = (PfnDirectGetMethod)GetProcAddress(gReal, "nvapi_Direct_GetMethod");
    }
}

void logLine(const std::wstring& text) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, gLogPath.c_str(), L"a, ccs=UTF-8") == 0 && f) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fwprintf(f, L"%02u:%02u:%02u.%03u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, text.c_str());
        fclose(f);
    }
}

// Allowed: NVIDIA's own driver components, our add-on, and the private NR snippet.
bool decide(const void* returnAddress, HMODULE* outModule) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)returnAddress, &m);
    *outModule = m;
    std::lock_guard<std::mutex> lock(gMutex);
    if (auto it = gDecisions.find(m); it != gDecisions.end()) return it->second;

    std::wstring path = moduleFile(m);
    std::wstring lp = lower(path);
    std::wstring name = lp.substr(lp.find_last_of(L"\\/") + 1);
    bool allow;
    const wchar_t* why;
    if (!m) { allow = false; why = L"unknown caller"; }
    else if (!gWindowsDir.empty() && lp.rfind(gWindowsDir, 0) == 0) { allow = true; why = L"Windows / driver store"; }
    else if (name.find(L"nvngx.dll") != std::wstring::npos) { allow = true; why = L"nr-bridge add-on"; }
    else if (lp.find(L"\\mgpu\\") != std::wstring::npos) { allow = true; why = L"private DLSS-NR snippet"; }
    else { allow = false; why = L"game side"; }
    gDecisions[m] = allow;
    logLine(std::wstring(allow ? L"ALLOW " : L"DENY  ") + path + L"  (" + why + L")");
    return allow;
}

}  // namespace

extern "C" void* __cdecl nvapi_QueryInterface(unsigned int id) {
    HMODULE caller = nullptr;
    if (!decide(_ReturnAddress(), &caller)) return nullptr;
    std::call_once(gRealOnce, loadReal);
    return gRealQuery ? gRealQuery(id) : nullptr;
}

extern "C" void* __cdecl nvapi_Direct_GetMethod(void* a, void* b, void* c, void* d) {
    HMODULE caller = nullptr;
    if (!decide(_ReturnAddress(), &caller)) return nullptr;
    std::call_once(gRealOnce, loadReal);
    return gRealDirect ? gRealDirect(a, b, c, d) : nullptr;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        gSelf = inst;
        DisableThreadLibraryCalls(inst);
        std::wstring self = moduleFile(inst);
        gGameDir = lower(self.substr(0, self.find_last_of(L"\\/") + 1));
        gLogPath = self.substr(0, self.find_last_of(L"\\/") + 1) + L"nvapi-gate.log";
        wchar_t win[MAX_PATH] = {};
        GetWindowsDirectoryW(win, MAX_PATH);
        gWindowsDir = lower(std::wstring(win) + L"\\");
        wchar_t exe[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        logLine(L"---- nvapi-gate loaded into " + std::wstring(exe));
    }
    return TRUE;
}

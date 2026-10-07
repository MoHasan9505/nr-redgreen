// sl-standin - nr-bridge's own sl.interposer.dll for Cyberpunk 2077 on an AMD render GPU with an
// NVIDIA neural GPU in the same machine. Replaces RTInitFix's (Nexus 29089) copy; see
// docs/rt-init-error.md.
//
// The game imports DXGI, D3D11 and D3D12 entry points and the sl* API from sl.interposer.dll
// instead of from the system DLLs. This stand-in:
//   - sends DXGI calls to the dxgi.dll beside it when there is one (ReShade), else System32.
//     RTInitFix loads System32\dxgi.dll by full path, so the game's swapchain never went through
//     ReShade: no ReShade overlay, and the bridge never armed (results run8);
//   - hides NVIDIA adapters from game-side callers only (HideNvidia=1). NVIDIA's own modules,
//     the bridge add-on and the private DLSS-NR snippet still see every adapter;
//   - with MaskDXR=1 (the shipped sl-standin.ini) reports RaytracingTier NOT_SUPPORTED to the
//     game the way RTInitFix does. MaskDXR=0 still fails RT init on this rig (run10);
//   - answers every sl* call the way RTInitFix does (Streamline off), which the game accepts.
//
// Settings: sl-standin.ini beside this DLL. Log: sl-standin.log beside this DLL, rewritten on
// every launch.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cwchar>

#if defined(_MSC_VER)
#include <intrin.h>
#include "vk_forwards_msvc.inc"   // vk* exports, forwarded to vulkan-1.dll
#define CALLER_ADDRESS() _ReturnAddress()
#else
#define CALLER_ADDRESS() __builtin_return_address(0)
#endif

namespace {

constexpr UINT kVendorNvidia = 0x10DE;
constexpr UINT kVendorMicrosoft = 0x1414;   // WARP / Basic Render Driver

HMODULE gSelf = nullptr;
wchar_t gDir[MAX_PATH] = {};      // this DLL's folder, with trailing backslash
wchar_t gLogPath[MAX_PATH] = {};
CRITICAL_SECTION gLogLock;
bool gLog = true;
bool gHideNvidia = true;
bool gHideWarp = true;
bool gMaskDxr = false;
bool gPreloadNvapiGate = true;

// ---------------------------------------------------------------- log

void logf(const char *fmt, ...)
{
    if (!gLog) return;
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    EnterCriticalSection(&gLogLock);
    FILE *f = nullptr;
    if (_wfopen_s(&f, gLogPath, L"a") == 0 && f)
    {
        fprintf(f, "%02u:%02u:%02u.%03u [%5lu] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                GetCurrentThreadId(), line);
        fclose(f);
    }
    LeaveCriticalSection(&gLogLock);
}

// ---------------------------------------------------------------- callers

void lowerInPlace(wchar_t *s) { for (; *s; ++s) *s = (wchar_t)towlower(*s); }

// Game side = a module in this DLL's folder or below, except the bridge add-on (its file name
// contains "nvngx.dll") and anything under an mgpu\ folder (the private DLSS-NR snippet).
// Everything else - System32, the driver store, NVIDIA's NGX core, other folders - is not.
bool isGameSideCaller(const void *address)
{
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &m) || !m)
        return false;
    wchar_t path[MAX_PATH * 2] = {};
    if (!GetModuleFileNameW(m, path, MAX_PATH * 2)) return false;
    lowerInPlace(path);
    wchar_t dir[MAX_PATH] = {};
    wcscpy_s(dir, MAX_PATH, gDir);
    lowerInPlace(dir);
    if (wcsncmp(path, dir, wcslen(dir)) != 0) return false;
    const wchar_t *name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    if (wcsstr(name, L"nvngx.dll")) return false;
    if (wcsstr(path, L"\\mgpu\\")) return false;
    return true;
}

// File name of the module containing `address`, for the log.
void callerName(const void *address, char *out, size_t n)
{
    HMODULE m = nullptr;
    wchar_t path[MAX_PATH * 2] = {};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(address), &m) && m)
        GetModuleFileNameW(m, path, MAX_PATH * 2);
    const wchar_t *name = wcsrchr(path, L'\\');
    snprintf(out, n, "%ls", name ? name + 1 : (path[0] ? path : L"?"));
}

// ---------------------------------------------------------------- real modules

HMODULE gDxgi = nullptr, gD3d11 = nullptr, gD3d12 = nullptr;
INIT_ONCE gModulesOnce = INIT_ONCE_STATIC_INIT;

// The copy beside this DLL wins (that is where ReShade installs itself as dxgi.dll), as it would
// for a game that loaded the system DLLs by name. Otherwise System32, by full path.
HMODULE loadNextTo(const wchar_t *name)
{
    wchar_t p[MAX_PATH * 2];
    swprintf(p, MAX_PATH * 2, L"%s%s", gDir, name);
    if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES)
    {
        HMODULE m = LoadLibraryExW(p, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (m)
        {
            logf("%ls: using the copy in the game folder (ReShade or another proxy)", name);
            return m;
        }
        logf("%ls: game-folder copy failed to load (error %lu) - using System32", name, GetLastError());
    }
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    swprintf(p, MAX_PATH * 2, L"%s\\%s", sys, name);
    HMODULE m = LoadLibraryExW(p, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    logf("%ls: using %ls (%s)", name, p, m ? "ok" : "FAILED");
    return m;
}

BOOL CALLBACK loadModules(PINIT_ONCE, PVOID, PVOID *)
{
    gDxgi = loadNextTo(L"dxgi.dll");
    gD3d11 = loadNextTo(L"d3d11.dll");
    gD3d12 = loadNextTo(L"d3d12.dll");
    return TRUE;
}

FARPROC resolve(HMODULE m, const char *name, const wchar_t *sysName)
{
    FARPROC p = m ? GetProcAddress(m, name) : nullptr;
    if (!p)
    {
        // A proxy DLL that does not export this name: take the system one.
        wchar_t sys[MAX_PATH], path[MAX_PATH * 2];
        GetSystemDirectoryW(sys, MAX_PATH);
        swprintf(path, MAX_PATH * 2, L"%s\\%s", sys, sysName);
        HMODULE s = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        p = s ? GetProcAddress(s, name) : nullptr;
        logf("%s: not exported by the game-folder %ls - System32's %s", name, sysName, p ? "used" : "MISSING too");
    }
    return p;
}

FARPROC dxgiProc(const char *name)
{
    InitOnceExecuteOnce(&gModulesOnce, loadModules, nullptr, nullptr);
    return resolve(gDxgi, name, L"dxgi.dll");
}
FARPROC d3d11Proc(const char *name)
{
    InitOnceExecuteOnce(&gModulesOnce, loadModules, nullptr, nullptr);
    return resolve(gD3d11, name, L"d3d11.dll");
}
FARPROC d3d12Proc(const char *name)
{
    InitOnceExecuteOnce(&gModulesOnce, loadModules, nullptr, nullptr);
    return resolve(gD3d12, name, L"d3d12.dll");
}

#define REAL(getter, type, name) reinterpret_cast<type>(reinterpret_cast<void *>(getter(name)))

// ---------------------------------------------------------------- vtable patching

bool patchSlot(void **vtbl, int slot, void *hook, void **original)
{
    if (vtbl[slot] == hook) return false;   // already ours
    DWORD old;
    if (!VirtualProtect(&vtbl[slot], sizeof(void *), PAGE_READWRITE, &old)) return false;
    *original = vtbl[slot];
    vtbl[slot] = hook;
    VirtualProtect(&vtbl[slot], sizeof(void *), old, &old);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return true;
}

// Original entries, per patched vtable. A process has very few distinct factory / device vtables.
struct Patched
{
    void **vtbl;
    void *orig[3];
};
constexpr int kMaxVtables = 8;
Patched gFactoryVtables[kMaxVtables] = {};
Patched gDeviceVtables[kMaxVtables] = {};
SRWLOCK gPatchLock = SRWLOCK_INIT;

Patched *findPatched(Patched *table, void *object)
{
    void **vtbl = *static_cast<void ***>(object);
    for (int i = 0; i < kMaxVtables; ++i)
        if (table[i].vtbl == vtbl) return &table[i];
    return nullptr;
}

// ---------------------------------------------------------------- adapter hiding

// IDXGIFactory slots: EnumAdapters 7, EnumAdapters1 12; IDXGIFactory6: EnumAdapterByGpuPreference 29.
enum { kSlotEnumAdapters = 7, kSlotEnumAdapters1 = 12, kSlotEnumAdapterByGpuPreference = 29 };
using PfnEnumAdapters = HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory *, UINT, IDXGIAdapter **);
using PfnEnumAdapters1 = HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory1 *, UINT, IDXGIAdapter1 **);
using PfnEnumByPref = HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory6 *, UINT, DXGI_GPU_PREFERENCE, REFIID, void **);

LONG gEnumLogBudget = 40;   // EnumAdapters is called in loops; log the first calls only

bool isHidden(IUnknown *adapter)
{
    IDXGIAdapter *a = nullptr;
    if (FAILED(adapter->QueryInterface(__uuidof(IDXGIAdapter), reinterpret_cast<void **>(&a)))) return false;
    DXGI_ADAPTER_DESC d{};
    const bool hide = SUCCEEDED(a->GetDesc(&d)) &&
                      (d.VendorId == kVendorNvidia || (gHideWarp && d.VendorId == kVendorMicrosoft));
    a->Release();
    return hide;
}

// Index `want` of the list without NVIDIA adapters: walk the real list and skip them.
template <class Get>
HRESULT filteredIndex(UINT want, Get get)
{
    UINT seen = 0;
    for (UINT real = 0;; ++real)
    {
        IUnknown *a = nullptr;
        HRESULT hr = get(real, &a);
        if (FAILED(hr)) return hr;   // DXGI_ERROR_NOT_FOUND past the end
        if (isHidden(a))
        {
            a->Release();
            continue;
        }
        if (seen++ == want) return S_OK;   // `a` is kept by the caller through get()
        a->Release();
    }
}

HRESULT STDMETHODCALLTYPE hookEnumAdapters(IDXGIFactory *self, UINT index, IDXGIAdapter **out)
{
    Patched *p = findPatched(gFactoryVtables, self);
    auto orig = reinterpret_cast<PfnEnumAdapters>(p ? p->orig[0] : nullptr);
    if (!orig) return E_FAIL;
    if (!gHideNvidia || !out || !isGameSideCaller(CALLER_ADDRESS())) return orig(self, index, out);
    IUnknown *last = nullptr;
    HRESULT hr = filteredIndex(index, [&](UINT i, IUnknown **a) {
        IDXGIAdapter *x = nullptr;
        HRESULT r = orig(self, i, &x);
        *a = last = x;
        return r;
    });
    *out = SUCCEEDED(hr) ? static_cast<IDXGIAdapter *>(last) : nullptr;
    if (InterlockedDecrement(&gEnumLogBudget) >= 0) logf("EnumAdapters(%u) for the game -> 0x%08lX", index, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE hookEnumAdapters1(IDXGIFactory1 *self, UINT index, IDXGIAdapter1 **out)
{
    Patched *p = findPatched(gFactoryVtables, self);
    auto orig = reinterpret_cast<PfnEnumAdapters1>(p ? p->orig[1] : nullptr);
    if (!orig) return E_FAIL;
    if (!gHideNvidia || !out || !isGameSideCaller(CALLER_ADDRESS())) return orig(self, index, out);
    IUnknown *last = nullptr;
    HRESULT hr = filteredIndex(index, [&](UINT i, IUnknown **a) {
        IDXGIAdapter1 *x = nullptr;
        HRESULT r = orig(self, i, &x);
        *a = last = x;
        return r;
    });
    *out = SUCCEEDED(hr) ? static_cast<IDXGIAdapter1 *>(last) : nullptr;
    if (InterlockedDecrement(&gEnumLogBudget) >= 0) logf("EnumAdapters1(%u) for the game -> 0x%08lX", index, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE hookEnumAdapterByGpuPreference(IDXGIFactory6 *self, UINT index, DXGI_GPU_PREFERENCE pref,
                                                         REFIID riid, void **out)
{
    Patched *p = findPatched(gFactoryVtables, self);
    auto orig = reinterpret_cast<PfnEnumByPref>(p ? p->orig[2] : nullptr);
    if (!orig) return E_FAIL;
    if (!gHideNvidia || !out || !isGameSideCaller(CALLER_ADDRESS())) return orig(self, index, pref, riid, out);
    IUnknown *last = nullptr;
    HRESULT hr = filteredIndex(index, [&](UINT i, IUnknown **a) {
        IUnknown *x = nullptr;
        HRESULT r = orig(self, i, pref, __uuidof(IUnknown), reinterpret_cast<void **>(&x));
        *a = last = x;
        return r;
    });
    *out = nullptr;
    if (SUCCEEDED(hr))
    {
        hr = last->QueryInterface(riid, out);
        last->Release();
    }
    if (InterlockedDecrement(&gEnumLogBudget) >= 0)
        logf("EnumAdapterByGpuPreference(%u, pref %d) for the game -> 0x%08lX", index, (int)pref, hr);
    return hr;
}

void patchFactory(void *factory)
{
    if (!gHideNvidia || !factory) return;
    IUnknown *unk = static_cast<IUnknown *>(factory);
    AcquireSRWLockExclusive(&gPatchLock);
    void **vtbl = *static_cast<void ***>(factory);
    if (!findPatched(gFactoryVtables, factory))
    {
        Patched *slot = nullptr;
        for (auto &e : gFactoryVtables) if (!e.vtbl) { slot = &e; break; }
        if (slot)
        {
            slot->vtbl = vtbl;
            patchSlot(vtbl, kSlotEnumAdapters, reinterpret_cast<void *>(&hookEnumAdapters), &slot->orig[0]);
            patchSlot(vtbl, kSlotEnumAdapters1, reinterpret_cast<void *>(&hookEnumAdapters1), &slot->orig[1]);
            // Slot 29 exists only when the object implements IDXGIFactory6 on this vtable.
            IDXGIFactory6 *f6 = nullptr;
            if (SUCCEEDED(unk->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void **>(&f6))))
            {
                if (*reinterpret_cast<void ***>(f6) == vtbl)
                    patchSlot(vtbl, kSlotEnumAdapterByGpuPreference,
                              reinterpret_cast<void *>(&hookEnumAdapterByGpuPreference), &slot->orig[2]);
                f6->Release();
            }
            logf("factory vtable %p patched: NVIDIA hidden from game-side EnumAdapters*%s", (void *)vtbl,
                 slot->orig[2] ? " (incl. EnumAdapterByGpuPreference)" : "");
        }
        else
        {
            logf("factory vtable %p NOT patched: table full", (void *)vtbl);
        }
    }
    ReleaseSRWLockExclusive(&gPatchLock);
}

// ---------------------------------------------------------------- DXR mask (MaskDXR=1 only)

enum { kSlotCheckFeatureSupport = 13 };
using PfnCheckFeatureSupport = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device *, D3D12_FEATURE, void *, UINT);
LONG gDxrLogBudget = 24;

HRESULT STDMETHODCALLTYPE hookCheckFeatureSupport(ID3D12Device *self, D3D12_FEATURE feature, void *data, UINT size)
{
    Patched *p = findPatched(gDeviceVtables, self);
    auto orig = reinterpret_cast<PfnCheckFeatureSupport>(p ? p->orig[0] : nullptr);
    if (!orig) return E_FAIL;
    HRESULT hr = orig(self, feature, data, size);
    if (SUCCEEDED(hr) && feature == D3D12_FEATURE_D3D12_OPTIONS5 && data &&
        size >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5))
    {
        auto *o5 = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS5 *>(data);
        const bool game = isGameSideCaller(CALLER_ADDRESS());
        if (InterlockedDecrement(&gDxrLogBudget) >= 0)
        {
            char who[MAX_PATH];
            callerName(CALLER_ADDRESS(), who, sizeof who);
            logf("CheckFeatureSupport(OPTIONS5) from %s: RaytracingTier %d%s", who, (int)o5->RaytracingTier,
                 game ? " -> 0 (masked for the game)" : " (not game side - unchanged)");
        }
        if (game) o5->RaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    }
    return hr;
}

void patchDevice(void *device)
{
    if (!gMaskDxr || !device) return;
    AcquireSRWLockExclusive(&gPatchLock);
    if (!findPatched(gDeviceVtables, device))
    {
        for (auto &e : gDeviceVtables)
        {
            if (e.vtbl) continue;
            e.vtbl = *static_cast<void ***>(device);
            patchSlot(e.vtbl, kSlotCheckFeatureSupport, reinterpret_cast<void *>(&hookCheckFeatureSupport), &e.orig[0]);
            logf("device vtable %p patched: DXR masked for the game", (void *)e.vtbl);
            break;
        }
    }
    ReleaseSRWLockExclusive(&gPatchLock);
}

// ---------------------------------------------------------------- settings

void readSettings()
{
    wchar_t ini[MAX_PATH * 2];
    swprintf(ini, MAX_PATH * 2, L"%ssl-standin.ini", gDir);
    gLog = GetPrivateProfileIntW(L"sl-standin", L"Log", 1, ini) != 0;
    gHideNvidia = GetPrivateProfileIntW(L"sl-standin", L"HideNvidia", 1, ini) != 0;
    gHideWarp = GetPrivateProfileIntW(L"sl-standin", L"HideWarp", 1, ini) != 0;
    gMaskDxr = GetPrivateProfileIntW(L"sl-standin", L"MaskDXR", 0, ini) != 0;
    gPreloadNvapiGate = GetPrivateProfileIntW(L"sl-standin", L"PreloadNvapiGate", 1, ini) != 0;
}

// ---------------------------------------------------------------- NVAPI gate preload
//
// nvapi-gate (tools/nvapi-gate) is installed beside the exe as nvapi64.dll so that the game
// is refused NVAPI. Measured 2026-10-07: it never loaded. The process held only
// System32\nvapi64.dll (+ the driver's nvapi64_impl.dll), loaded by something before the
// game's own LoadLibrary("nvapi64.dll") - and once a module with that base name is loaded,
// a by-name load returns it, whatever folder it came from. So the game saw NVIDIA through
// NVAPI, and with MaskDXR=0 it failed "Ray Tracing initialization".
//
// The exe imports this DLL statically, so it loads before nearly everything else. Loading
// the gate by full path here makes it the first nvapi64.dll in the process; later by-name
// loads get the gate, which refuses game-side callers and passes NVIDIA's own components,
// the bridge add-on and the DLSS-NR snippet through to the real one. The gate's own
// DllMain only opens its log, and it imports nothing but kernel32, which is what makes a
// load from here acceptable.
void preloadNvapiGate()
{
    if (!gPreloadNvapiGate) return;
    const bool realFirst = GetModuleHandleW(L"nvapi64.dll") != nullptr;
    wchar_t gate[MAX_PATH * 2];
    swprintf(gate, MAX_PATH * 2, L"%snvapi64.dll", gDir);
    if (GetFileAttributesW(gate) == INVALID_FILE_ATTRIBUTES)
    {
        logf("PreloadNvapiGate: no nvapi64.dll beside sl-standin - nothing to preload");
        return;
    }
    HMODULE h = LoadLibraryExW(gate, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    HMODULE byName = GetModuleHandleW(L"nvapi64.dll");
    logf("PreloadNvapiGate: %s (error %lu)%s; a by-name nvapi64.dll now resolves to the %s",
         h ? "loaded the gate" : "load FAILED", h ? 0ul : GetLastError(),
         realFirst ? " - System32's nvapi64.dll was ALREADY loaded, so the gate may be bypassed" : "",
         (h && byName == h) ? "gate" : "real NVAPI");
}

}  // namespace

// ================================================================= exports (names in the .def)

extern "C" {

// ---- DXGI: through the dxgi.dll beside us (ReShade), else System32 ----

HRESULT WINAPI Sl_CreateDXGIFactory(REFIID riid, void **pp)
{
    using F = HRESULT(WINAPI *)(REFIID, void **);
    auto f = REAL(dxgiProc, F, "CreateDXGIFactory");
    HRESULT hr = f ? f(riid, pp) : E_NOTIMPL;
    logf("CreateDXGIFactory -> 0x%08lX", hr);
    if (SUCCEEDED(hr) && pp) patchFactory(*pp);
    return hr;
}

HRESULT WINAPI Sl_CreateDXGIFactory1(REFIID riid, void **pp)
{
    using F = HRESULT(WINAPI *)(REFIID, void **);
    auto f = REAL(dxgiProc, F, "CreateDXGIFactory1");
    HRESULT hr = f ? f(riid, pp) : E_NOTIMPL;
    logf("CreateDXGIFactory1 -> 0x%08lX", hr);
    if (SUCCEEDED(hr) && pp) patchFactory(*pp);
    return hr;
}

HRESULT WINAPI Sl_CreateDXGIFactory2(UINT flags, REFIID riid, void **pp)
{
    using F = HRESULT(WINAPI *)(UINT, REFIID, void **);
    auto f = REAL(dxgiProc, F, "CreateDXGIFactory2");
    HRESULT hr = f ? f(flags, riid, pp) : E_NOTIMPL;
    logf("CreateDXGIFactory2(flags 0x%X) -> 0x%08lX", flags, hr);
    if (SUCCEEDED(hr) && pp) patchFactory(*pp);
    return hr;
}

HRESULT WINAPI Sl_DXGIGetDebugInterface1(UINT flags, REFIID riid, void **pp)
{
    using F = HRESULT(WINAPI *)(UINT, REFIID, void **);
    auto f = REAL(dxgiProc, F, "DXGIGetDebugInterface1");
    return f ? f(flags, riid, pp) : E_NOINTERFACE;
}

// ---- D3D11 ----

HRESULT WINAPI Sl_D3D11CreateDevice(IDXGIAdapter *a, D3D_DRIVER_TYPE t, HMODULE sw, UINT flags,
                                    const D3D_FEATURE_LEVEL *fl, UINT nfl, UINT sdk, ID3D11Device **dev,
                                    D3D_FEATURE_LEVEL *outFl, ID3D11DeviceContext **ctx)
{
    using F = decltype(&Sl_D3D11CreateDevice);
    auto f = REAL(d3d11Proc, F, "D3D11CreateDevice");
    HRESULT hr = f ? f(a, t, sw, flags, fl, nfl, sdk, dev, outFl, ctx) : E_NOTIMPL;
    logf("D3D11CreateDevice -> 0x%08lX", hr);
    return hr;
}

HRESULT WINAPI Sl_D3D11CreateDeviceAndSwapChain(IDXGIAdapter *a, D3D_DRIVER_TYPE t, HMODULE sw, UINT flags,
                                                const D3D_FEATURE_LEVEL *fl, UINT nfl, UINT sdk,
                                                const DXGI_SWAP_CHAIN_DESC *scd, IDXGISwapChain **sc,
                                                ID3D11Device **dev, D3D_FEATURE_LEVEL *outFl,
                                                ID3D11DeviceContext **ctx)
{
    using F = decltype(&Sl_D3D11CreateDeviceAndSwapChain);
    auto f = REAL(d3d11Proc, F, "D3D11CreateDeviceAndSwapChain");
    HRESULT hr = f ? f(a, t, sw, flags, fl, nfl, sdk, scd, sc, dev, outFl, ctx) : E_NOTIMPL;
    logf("D3D11CreateDeviceAndSwapChain -> 0x%08lX", hr);
    return hr;
}

// ---- D3D12 ----

HRESULT WINAPI Sl_D3D12CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void **pp)
{
    using F = decltype(&Sl_D3D12CreateDevice);
    auto f = REAL(d3d12Proc, F, "D3D12CreateDevice");

    // MaskDXR: the game creates its real device without going through this export (run11:
    // ReShade logged it, this file did not), so patching only devices returned here never
    // reached it. The patch is on the device vtable, which every device of the same class
    // shares - so create one device here, on the first adapter the game asks about, patch
    // it, and release it. RTInitFix got the same effect from the devices it saw.
    static LONG probed = 0;
    if (gMaskDxr && f && adapter && InterlockedExchange(&probed, 1) == 0)
    {
        ID3D12Device *probe = nullptr;
        HRESULT ph = f(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void **>(&probe));
        logf("MaskDXR: probe device -> 0x%08lX", ph);
        if (SUCCEEDED(ph) && probe)
        {
            patchDevice(probe);
            probe->Release();
        }
    }

    HRESULT hr = f ? f(adapter, fl, riid, pp) : E_NOTIMPL;
    UINT vendor = 0;
    if (adapter)
    {
        IDXGIAdapter *a = nullptr;
        if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter), reinterpret_cast<void **>(&a))))
        {
            DXGI_ADAPTER_DESC d{};
            if (SUCCEEDED(a->GetDesc(&d))) vendor = d.VendorId;
            a->Release();
        }
    }
    logf("D3D12CreateDevice(adapter vendor 0x%04X, fl 0x%X, %s) -> 0x%08lX", vendor, (unsigned)fl,
         pp ? "create" : "test only", hr);
    if (SUCCEEDED(hr) && pp && *pp) patchDevice(*pp);
    return hr;
}

HRESULT WINAPI Sl_D3D12CreateRootSignatureDeserializer(const void *src, SIZE_T size, REFIID riid, void **pp)
{
    using F = decltype(&Sl_D3D12CreateRootSignatureDeserializer);
    auto f = REAL(d3d12Proc, F, "D3D12CreateRootSignatureDeserializer");
    return f ? f(src, size, riid, pp) : E_NOTIMPL;
}

HRESULT WINAPI Sl_D3D12CreateVersionedRootSignatureDeserializer(const void *src, SIZE_T size, REFIID riid, void **pp)
{
    using F = decltype(&Sl_D3D12CreateVersionedRootSignatureDeserializer);
    auto f = REAL(d3d12Proc, F, "D3D12CreateVersionedRootSignatureDeserializer");
    return f ? f(src, size, riid, pp) : E_NOTIMPL;
}

HRESULT WINAPI Sl_D3D12EnableExperimentalFeatures(UINT n, const IID *iids, void *structs, UINT *sizes)
{
    using F = decltype(&Sl_D3D12EnableExperimentalFeatures);
    auto f = REAL(d3d12Proc, F, "D3D12EnableExperimentalFeatures");
    return f ? f(n, iids, structs, sizes) : E_NOTIMPL;
}

HRESULT WINAPI Sl_D3D12GetDebugInterface(REFIID riid, void **pp)
{
    using F = decltype(&Sl_D3D12GetDebugInterface);
    auto f = REAL(d3d12Proc, F, "D3D12GetDebugInterface");
    return f ? f(riid, pp) : E_NOINTERFACE;
}

HRESULT WINAPI Sl_D3D12GetInterface(REFCLSID clsid, REFIID riid, void **pp)
{
    using F = decltype(&Sl_D3D12GetInterface);
    auto f = REAL(d3d12Proc, F, "D3D12GetInterface");
    return f ? f(clsid, riid, pp) : E_NOINTERFACE;
}

HRESULT WINAPI Sl_D3D12SerializeRootSignature(const void *desc, int version, void **blob, void **err)
{
    using F = decltype(&Sl_D3D12SerializeRootSignature);
    auto f = REAL(d3d12Proc, F, "D3D12SerializeRootSignature");
    return f ? f(desc, version, blob, err) : E_NOTIMPL;
}

HRESULT WINAPI Sl_D3D12SerializeVersionedRootSignature(const void *desc, void **blob, void **err)
{
    using F = decltype(&Sl_D3D12SerializeVersionedRootSignature);
    auto f = REAL(d3d12Proc, F, "D3D12SerializeVersionedRootSignature");
    return f ? f(desc, blob, err) : E_NOTIMPL;
}

// ---- Streamline: off. Same answers as RTInitFix v4, which the game is known to accept. ----
// sl::Result is a 32-bit enum. Arguments are only touched where an out-parameter must be set.

int Sl_slInit(const void *, uint64_t) { logf("slInit -> 4 (Streamline off)"); return 4; }
int Sl_slShutdown() { return 0; }
int Sl_slSetD3DDevice(void *) { return 0; }
int Sl_slSetVulkanInfo(const void *) { return 0; }
int Sl_slGetNativeInterface(void *proxy, void **native)
{
    if (native) *native = proxy;
    return 0;
}
int Sl_slUpgradeInterface(void **) { return 0; }
int Sl_slGetFeatureFunction(uint32_t, const char *, void **fn)
{
    if (fn) *fn = nullptr;
    return 1;
}
int Sl_slGetFeatureRequirements(uint32_t, void *) { return 4; }
int Sl_slGetFeatureVersion(uint32_t, void *) { return 1; }
int Sl_slIsFeatureSupported(uint32_t, const void *) { return 4; }
int Sl_slIsFeatureLoaded(uint32_t, bool *loaded)
{
    if (loaded) *loaded = false;
    return 0;
}
int Sl_slSetFeatureLoaded(uint32_t, bool) { return 0; }
int Sl_slSetTag(const void *, const void *, uint32_t, void *) { return 0; }
int Sl_slSetConstants(const void *, const void *, const void *) { return 0; }
int Sl_slAllocateResources(void *, uint32_t, const void *) { return 1; }
int Sl_slEvaluateFeature(uint32_t, const void *, const void **, uint32_t, void *) { return 1; }
int Sl_slFreeResources(uint32_t, const void *) { return 1; }

}  // extern "C"

namespace {
// sl::FrameToken: an interface whose only member is `virtual operator uint32_t() const`.
struct FrameToken
{
    virtual operator uint32_t() const { return index; }
    uint32_t index = 0;
};
FrameToken gToken;
}  // namespace

extern "C" int Sl_slGetNewFrameToken(FrameToken **token, const uint32_t *frameIndex)
{
    gToken.index = frameIndex ? *frameIndex : gToken.index + 1;
    if (token) *token = &gToken;
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        gSelf = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&gLogLock);
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(inst, self, MAX_PATH);
        wchar_t *slash = wcsrchr(self, L'\\');
        if (slash) slash[1] = L'\0';
        wcscpy_s(gDir, MAX_PATH, self);
        swprintf(gLogPath, MAX_PATH, L"%ssl-standin.log", gDir);
        readSettings();
        if (gLog)
        {
            FILE *f = nullptr;   // one launch per log
            if (_wfopen_s(&f, gLogPath, L"w") == 0 && f) fclose(f);
        }
        logf("sl-standin (nr-bridge) loaded. HideNvidia=%d HideWarp=%d MaskDXR=%d PreloadNvapiGate=%d",
             gHideNvidia, gHideWarp, gMaskDxr, gPreloadNvapiGate);
        preloadNvapiGate();
    }
    return TRUE;
}

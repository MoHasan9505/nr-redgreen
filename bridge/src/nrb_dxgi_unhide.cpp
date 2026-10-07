// nr-bridge [NRB6]: give NVIDIA's own code an unfiltered DXGI adapter list.
//
// RTInitFix (the Cyberpunk "Ray Tracing initialization" fix) patches the DXGI
// factory vtable so EnumAdapters / EnumAdapters1 / EnumAdapterByGpuPreference
// skip NVIDIA adapters. A vtable is shared by every factory in the process, so
// NGX can no longer find the RTX 5070 either, and CreateFeature(DLSS-NR) fails
// with 0xBAD00002 FAIL_PlatformError. Reproduced with tools/nr-probe:
//   --rtfix-hooks dxgi -> FAIL_PlatformError, d3d12 -> works, none -> works.
//
// This wraps the (already patched) slots once more and answers by caller:
//   - NVIDIA driver-store modules, System32\nvapi64.dll, modules whose name
//     contains "nvngx.dll" (nr-bridge, nr-probe) and the private DLSS-NR snippet
//     in an \mgpu\ folder get the filtered list followed by the adapters it hid
//     (found through DXCore, opened with EnumAdapterByLuid, which is not hooked);
//   - everyone else, the game included, gets exactly what RTInitFix returns.
// Without RTInitFix the filtered list is already complete and nothing changes.

#include "nrb_dxgi_unhide.hpp"

#include <windows.h>
#include <intrin.h>
#include <initguid.h>
#include <dxcore.h>
#include <dxgi1_6.h>

#include <atomic>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace nrb
{
namespace
{
    using PfnEnumAdapters  = HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory *, UINT, IDXGIAdapter **);
    using PfnEnumAdapters1 = HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory1 *, UINT, IDXGIAdapter1 **);
    using PfnEnumByPref    = HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory6 *, UINT, DXGI_GPU_PREFERENCE, REFIID, void **);

    constexpr int kSlotEnumAdapters = 7, kSlotEnumAdapters1 = 12, kSlotEnumByPref = 29;

    // A DXGI factory exposes more than one vtable (one per interface pointer it hands out), and
    // RTInitFix patches whichever one the game's riid returned. So every distinct vtable is
    // wrapped, and each hook looks up what was in ITS slot before us.
    struct Prev
    {
        PfnEnumAdapters  enum0 = nullptr;   // RTInitFix's hook, or DXGI's own
        PfnEnumAdapters1 enum1 = nullptr;
        PfnEnumByPref    pref  = nullptr;
    };
    // Fixed table, filled before any hook goes live: a game thread can call through a vtable
    // the moment its slot is written, so its entry must already be readable without a lock.
    struct Wrapped
    {
        void **vtbl = nullptr;
        Prev prev;
    };
    Wrapped g_wrapped[8];
    std::atomic<int> g_wrapped_n{0};
    void (*g_log)(const char *) = nullptr;

    const Prev &prev_of(void *self)
    {
        static const Prev none;
        void **vtbl = *reinterpret_cast<void ***>(self);
        const int n = g_wrapped_n.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i)
            if (g_wrapped[i].vtbl == vtbl) return g_wrapped[i].prev;
        return none;
    }

    std::mutex g_mutex;
    std::unordered_map<HMODULE, bool> g_callers;   // calling module -> sees hidden adapters

    void log(const char *s) { if (g_log) g_log(s); }

    // Is the caller NVIDIA's own code (or ours)? Decided once per module and logged.
    bool unfiltered_for(const void *ret)
    {
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(ret), &m);
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_callers.find(m);
        if (it != g_callers.end()) return it->second;

        wchar_t path[MAX_PATH * 2] = {};
        if (m != nullptr) GetModuleFileNameW(m, path, MAX_PATH * 2);
        wchar_t low[MAX_PATH * 2] = {};
        for (size_t i = 0; path[i] && i + 1 < MAX_PATH * 2; ++i) low[i] = (wchar_t)towlower(path[i]);
        const wchar_t *name = wcsrchr(low, L'\\');
        name = name ? name + 1 : low;

        const bool allow = (m != nullptr) &&
                           (wcsstr(low, L"\\driverstore\\filerepository\\nv") != nullptr ||   // NVIDIA driver packages
                            wcsstr(low, L"\\system32\\nvapi64.dll") != nullptr ||
                            wcsstr(name, L"nvngx.dll") != nullptr ||                         // nr-bridge add-on, nr-probe
                            wcsstr(low, L"\\mgpu\\") != nullptr);                             // private DLSS-NR snippet
        g_callers[m] = allow;

        char line[MAX_PATH * 2 + 96];
        snprintf(line, sizeof line, "[MGPU][NRB6] DXGI adapter list for %ls: %s", path[0] ? path : L"(unknown caller)",
                 allow ? "UNFILTERED (NVIDIA side)" : "as filtered (game side)");
        log(line);
        return allow;
    }

    // The adapters DXCore knows that a filtered enumeration on `factory` does not return.
    std::vector<LUID> hidden_luids(IDXGIFactory1 *factory, PfnEnumAdapters1 filtered)
    {
        std::vector<LUID> listed;
        for (UINT i = 0;; ++i)
        {
            IDXGIAdapter1 *a = nullptr;
            if (FAILED(filtered(factory, i, &a)) || a == nullptr) break;
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            listed.push_back(d.AdapterLuid);
            a->Release();
        }
        std::vector<LUID> hidden;
        IDXCoreAdapterFactory *xf = nullptr;
        IDXCoreAdapterList *list = nullptr;
        const GUID attr = DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS;
        if (SUCCEEDED(DXCoreCreateAdapterFactory(__uuidof(IDXCoreAdapterFactory), reinterpret_cast<void **>(&xf))) &&
            SUCCEEDED(xf->CreateAdapterList(1, &attr, __uuidof(IDXCoreAdapterList), reinterpret_cast<void **>(&list))))
        {
            for (uint32_t i = 0; i < list->GetAdapterCount(); ++i)
            {
                IDXCoreAdapter *xa = nullptr;
                LUID luid{};
                if (FAILED(list->GetAdapter(i, __uuidof(IDXCoreAdapter), reinterpret_cast<void **>(&xa))) || !xa) continue;
                const bool ok = SUCCEEDED(xa->GetProperty(DXCoreAdapterProperty::InstanceLuid, sizeof luid, &luid));
                xa->Release();
                if (!ok) continue;
                bool known = false;
                for (const LUID &l : listed)
                    if (l.LowPart == luid.LowPart && l.HighPart == luid.HighPart) { known = true; break; }
                if (!known) hidden.push_back(luid);
            }
        }
        if (list) list->Release();
        if (xf) xf->Release();
        return hidden;
    }

    // Fixed at install, read-only afterwards: the adapters the filtered list lacks, and a private
    // factory to open them from. A DXGI 1.0 factory (CreateDXGIFactory) refuses IDXGIFactory1/4,
    // and NVAPI enumerates through exactly such a factory, so nothing may depend on the caller's.
    std::vector<LUID> g_hidden;
    IDXGIFactory4 *g_private = nullptr;

    // Unfiltered = the hidden adapters first, then the caller's own filtered list. `filtered(i, &a)`
    // is the caller's previous slot, so each factory flavour keeps its own semantics.
    template <typename Filtered>
    HRESULT unfiltered(UINT idx, REFIID riid, void **out, Filtered filtered)
    {
        if (idx < g_hidden.size())
            return g_private ? g_private->EnumAdapterByLuid(g_hidden[idx], riid, out) : DXGI_ERROR_NOT_FOUND;
        IDXGIAdapter *a = nullptr;
        const HRESULT hr = filtered(idx - (UINT)g_hidden.size(), &a);
        if (FAILED(hr) || a == nullptr) return FAILED(hr) ? hr : DXGI_ERROR_NOT_FOUND;
        const HRESULT qi = a->QueryInterface(riid, out);
        a->Release();
        return qi;
    }

    // NRB6_TRACE=1 in the environment logs every call: which slot, who called, what came back.
    bool tracing()
    {
        static const bool on = [] {
            wchar_t v[8] = {};
            return GetEnvironmentVariableW(L"NRB6_TRACE", v, 8) > 0 && v[0] == L'1';
        }();
        return on;
    }

    void trace(const char *slot, const void *ret, UINT idx, bool unf, HRESULT hr, void *adapter)
    {
        if (!tracing()) return;
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(ret), &m);
        wchar_t path[MAX_PATH] = {};
        if (m) GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t *name = wcsrchr(path, L'\\');
        UINT vendor = 0;
        if (SUCCEEDED(hr) && adapter)
        {
            DXGI_ADAPTER_DESC d{};
            if (SUCCEEDED(static_cast<IDXGIAdapter *>(adapter)->GetDesc(&d))) vendor = d.VendorId;
        }
        char line[400];
        snprintf(line, sizeof line, "[MGPU][NRB6] trace %s(%u) from %ls +0x%llX %s -> hr=0x%08X vendor=0x%04X", slot, idx,
                 name ? name + 1 : L"?", (unsigned long long)((const char *)ret - (const char *)m),
                 unf ? "UNFILTERED" : "filtered", (unsigned)hr, vendor);
        log(line);
    }

    HRESULT STDMETHODCALLTYPE hook_enum(IDXGIFactory *self, UINT idx, IDXGIAdapter **out)
    {
        const void *ret = _ReturnAddress();
        const Prev &p = prev_of(self);
        const bool unf = unfiltered_for(ret);
        const HRESULT hr = unf ? unfiltered(idx, __uuidof(IDXGIAdapter), reinterpret_cast<void **>(out),
                                            [&](UINT i, IDXGIAdapter **a) { return p.enum0(self, i, a); })
                               : p.enum0(self, idx, out);
        trace("EnumAdapters", ret, idx, unf, hr, out ? *out : nullptr);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hook_enum1(IDXGIFactory1 *self, UINT idx, IDXGIAdapter1 **out)
    {
        const void *ret = _ReturnAddress();
        const Prev &p = prev_of(self);
        const bool unf = unfiltered_for(ret);
        const HRESULT hr = unf ? unfiltered(idx, __uuidof(IDXGIAdapter1), reinterpret_cast<void **>(out),
                                            [&](UINT i, IDXGIAdapter **a) {
                                                IDXGIAdapter1 *a1 = nullptr;
                                                const HRESULT r = p.enum1(self, i, &a1);
                                                *a = a1;
                                                return r;
                                            })
                               : p.enum1(self, idx, out);
        trace("EnumAdapters1", ret, idx, unf, hr, out ? *out : nullptr);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hook_pref(IDXGIFactory6 *self, UINT idx, DXGI_GPU_PREFERENCE pref, REFIID riid, void **out)
    {
        const void *ret = _ReturnAddress();
        const Prev &p = prev_of(self);
        const bool unf = unfiltered_for(ret);
        // Order by preference only matters to the game; NVIDIA's code looks adapters up by LUID.
        const HRESULT hr = unf ? unfiltered(idx, riid, out,
                                            [&](UINT i, IDXGIAdapter **a) {
                                                return p.pref(self, i, pref, __uuidof(IDXGIAdapter), reinterpret_cast<void **>(a));
                                            })
                               : p.pref(self, idx, pref, riid, out);
        trace("EnumAdapterByGpuPreference", ret, idx, unf, hr, nullptr);
        return hr;
    }

    void write_slot(void **vtbl, int slot, void *hook)
    {
        DWORD old = 0;
        if (!VirtualProtect(&vtbl[slot], sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) return;
        vtbl[slot] = hook;
        VirtualProtect(&vtbl[slot], sizeof(void *), old, &old);
    }

    // Wrap one vtable. `level` is the highest factory interface this pointer was obtained as,
    // which bounds which slots exist in it (EnumAdapters1 from 1, EnumAdapterByGpuPreference from 6).
    void wrap_vtable(void **vtbl, int level)
    {
        const int n = g_wrapped_n.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
            if (g_wrapped[i].vtbl == vtbl) return;
        if (n >= (int)(sizeof g_wrapped / sizeof g_wrapped[0])) return;

        // Record what is there now and publish it, THEN redirect the slots.
        Wrapped &w = g_wrapped[n];
        w.vtbl = vtbl;
        w.prev.enum0 = reinterpret_cast<PfnEnumAdapters>(vtbl[kSlotEnumAdapters]);
        if (level >= 1) w.prev.enum1 = reinterpret_cast<PfnEnumAdapters1>(vtbl[kSlotEnumAdapters1]);
        if (level >= 6) w.prev.pref = reinterpret_cast<PfnEnumByPref>(vtbl[kSlotEnumByPref]);
        g_wrapped_n.store(n + 1, std::memory_order_release);

        write_slot(vtbl, kSlotEnumAdapters, reinterpret_cast<void *>(&hook_enum));
        if (level >= 1) write_slot(vtbl, kSlotEnumAdapters1, reinterpret_cast<void *>(&hook_enum1));
        if (level >= 6) write_slot(vtbl, kSlotEnumByPref, reinterpret_cast<void *>(&hook_pref));

        char line[160];
        snprintf(line, sizeof line, "[MGPU][NRB6] wrapped DXGI factory vtable %p (IDXGIFactory%d slots)", (void *)vtbl, level);
        log(line);
    }
}

bool install_dxgi_unhide(void (*logger)(const char *))
{
    static std::once_flag once;
    static bool installed = false;
    g_log = logger;
    std::call_once(once, [] {
        // A factory straight from System32's dxgi.dll: its vtable is the one every factory in
        // the process shares, and the one RTInitFix patched.
        wchar_t sys[MAX_PATH] = {};
        GetSystemDirectoryW(sys, MAX_PATH);
        wchar_t p[MAX_PATH + 16] = {};
        swprintf_s(p, L"%s\\dxgi.dll", sys);
        HMODULE dxgi = GetModuleHandleW(p);
        if (dxgi == nullptr) dxgi = LoadLibraryW(p);
        using PfnCreate = HRESULT(WINAPI *)(UINT, REFIID, void **);
        auto create = dxgi ? reinterpret_cast<PfnCreate>(GetProcAddress(dxgi, "CreateDXGIFactory2")) : nullptr;
        IDXGIFactory6 *f = nullptr;
        if (create == nullptr || FAILED(create(0, __uuidof(IDXGIFactory6), reinterpret_cast<void **>(&f))) || f == nullptr)
        {
            log("[MGPU][NRB6] could not create a System32 DXGI factory - adapter unhiding not installed");
            return;
        }
        // Work out what is hidden BEFORE any hook goes live, using the slot as it is now
        // (RTInitFix's filter, or DXGI's own on a machine without it). The factory is kept as the
        // private source of hidden adapters for the life of the process.
        const auto filtered = reinterpret_cast<PfnEnumAdapters1>((*reinterpret_cast<void ***>(f))[kSlotEnumAdapters1]);
        g_hidden = hidden_luids(f, filtered);
        g_private = f;   // IDXGIFactory6 derives from IDXGIFactory4

        // Ask for every factory interface and wrap each distinct vtable that comes back.
        const IID iids[] = {__uuidof(IDXGIFactory), __uuidof(IDXGIFactory1), __uuidof(IDXGIFactory2),
                            __uuidof(IDXGIFactory3), __uuidof(IDXGIFactory4), __uuidof(IDXGIFactory5),
                            __uuidof(IDXGIFactory6), __uuidof(IDXGIFactory7)};
        for (int level = 7; level >= 0; --level)   // highest first, so a shared vtable gets every slot
        {
            IUnknown *itf = nullptr;
            if (FAILED(f->QueryInterface(iids[level], reinterpret_cast<void **>(&itf))) || itf == nullptr) continue;
            wrap_vtable(*reinterpret_cast<void ***>(itf), level);
            itf->Release();
        }
        installed = g_wrapped_n.load() > 0;

        char line[200];
        snprintf(line, sizeof line,
                 "[MGPU][NRB6] DXGI adapter unhiding %s: %zu adapter(s) are hidden from the filtered list and will "
                 "be shown to NVIDIA's own code only",
                 installed ? "installed" : "FAILED", g_hidden.size());
        log(line);
    });
    return installed;
}
}  // namespace nrb

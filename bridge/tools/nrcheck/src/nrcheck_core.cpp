// nrcheck_core.cpp - built as nvngx.dll_nrcheck.dll
//
// WHY THIS IS A DLL AND WHY IT HAS THIS NAME
// nvngx_dlssnr.dll checks the module that CALLS it and requires that module's
// file path to contain the substring "nvngx.dll". A caller that fails the
// check gets 0xBAD00002 FAIL_PlatformError - the same code the two reports
// show. If this check called NGX from the exe, every run would fail for that
// reason alone and the result would mean nothing. The add-on solves it by
// shipping as nvngx.dll_mgpu_bridge.addon64; this solves it the same way.
// An EXECUTABLE with that name breaks process startup, so the exe stays
// nrcheck.exe and only this DLL carries the name.
//
// WHAT IT DOES
// It runs the SELF-HEALING ARM the add-on would run, in a process shaped like
// a game: the other GPU's device is created first and stays alive the whole
// time (that is the game's device), then the neural device. The add-on can
// arm at any point it chooses, so a failed create is not final: this cleans
// up and arms again, one change per attempt, and stops at the first success.
//
//   attempt 1  first arm - what the add-on does today
//   attempt 2  same NGX session, 2 s later, fresh command list
//   attempt 3  clean up (release, DestroyParameters), Init again, arm
//   attempt 4  as 3, after 2 s
//   attempt 5  as 3, after 4 s
//   attempt 6  as 3, after 8 s
//   attempt 7  a NEW device on the neural GPU (old one kept), Init on it, arm
//   attempt 8  old neural devices RELEASED, another new one, Init on it, arm
//
// --synthetic N makes attempts 1..N call CreateFeature from nrcheck.exe, which
// NGX refuses with its own 0xBAD00002 (see create_foreign_guarded). On a
// healthy PC the ladder should then read HEALED at attempt N+1.
//
// The attempt that first succeeds is the shape the add-on's self-healing arm
// needs. A crash inside NGX ends the ladder: the add-on never re-enters NGX
// after a caught fault (V44), and neither does this.
//
// On the neural device it runs the add-on's startup probe (P1.0c) call for
// call: core Init -> core GetCapabilityParameters -> snippet Init_Ext ->
// snippet PopulateParameters_Impl -> Width/Height set -> snippet
// CreateFeature(Reserved18) at 1280x720. Same app id (0), same data path
// rule (the directory of the calling module), same sizes, same module
// preferences. Nothing is added that the add-on does not do.
//
// Shutdown1 is NOT called, for the same reason the add-on never calls it
// (V48): it has taken processes down on a clean teardown. The process exits
// right after, so the session goes with it.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "nvsdk_ngx.h"

namespace
{
    FILE *g_out = nullptr;

    void out(const char *fmt, ...)
    {
        char buf[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        fputs(buf, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        if (g_out != nullptr) { fputs(buf, g_out); fputc('\n', g_out); fflush(g_out); }
    }

    // ---- typedefs: copied from gpu1_context.cpp so the ABI is the add-on's ----
    typedef NVSDK_NGX_Result (NVSDK_CONV *pf_init)(unsigned long long, const wchar_t *,
        ID3D12Device *, const NVSDK_NGX_FeatureCommonInfo *, NVSDK_NGX_Version);
    typedef NVSDK_NGX_Result (NVSDK_CONV *pf_init_ext)(unsigned long long, const wchar_t *,
        ID3D12Device *, NVSDK_NGX_Version, const NVSDK_NGX_Parameter *);
    typedef NVSDK_NGX_Result (NVSDK_CONV *pf_get_caps)(NVSDK_NGX_Parameter **);
    typedef NVSDK_NGX_Result (NVSDK_CONV *pf_populate)(NVSDK_NGX_Parameter *);
    typedef NVSDK_NGX_Result (NVSDK_CONV *pf_create)(ID3D12GraphicsCommandList *,
        NVSDK_NGX_Feature, NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
    typedef NVSDK_NGX_Result (NVSDK_CONV *pf_release)(NVSDK_NGX_Handle *);
    typedef NVSDK_NGX_Result (NVSDK_CONV *pf_destroy)(NVSDK_NGX_Parameter *);

    const unsigned SEH_FAULT = 0xDEAD0001u;

    NVSDK_NGX_Result create_guarded(pf_create fn, ID3D12GraphicsCommandList *cl,
                                    NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **h,
                                    unsigned long *code)
    {
        __try { return fn(cl, NVSDK_NGX_Feature_Reserved18, p, h); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *code = (unsigned long)GetExceptionCode();
            return (NVSDK_NGX_Result)SEH_FAULT;
        }
    }

    // ---- SYNTHETIC: a real 0xBAD00002 from NGX, on purpose ----
    //
    // nvngx_dlssnr.dll refuses CreateFeature with 0xBAD00002
    // FAIL_PlatformError when the module that calls it does not have
    // "nvngx.dll" in its path. nrcheck.exe does not. So for a synthetic
    // attempt the call is made through a forwarder exported by nrcheck.exe,
    // and NGX itself refuses it - nothing is faked here, the code is NGX's.
    // This is how a healthy PC produces a reference report that shows the
    // self-healing arm recovering from a real PlatformError.
    typedef unsigned (*pf_foreign)(void *fn, void *cl, int feat, void *params, void **h);

    NVSDK_NGX_Result create_foreign_guarded(pf_foreign fwd, pf_create fn,
                                            ID3D12GraphicsCommandList *cl,
                                            NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **h,
                                            unsigned long *code)
    {
        __try
        {
            return (NVSDK_NGX_Result)fwd((void *)fn, cl, (int)NVSDK_NGX_Feature_Reserved18, p,
                                         (void **)h);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *code = (unsigned long)GetExceptionCode();
            return (NVSDK_NGX_Result)SEH_FAULT;
        }
    }

    const char *rname(NVSDK_NGX_Result r)
    {
        switch ((unsigned)r)
        {
        case NVSDK_NGX_Result_Success:                        return "Success";
        case NVSDK_NGX_Result_FAIL_FeatureNotSupported:       return "FAIL_FeatureNotSupported";
        case NVSDK_NGX_Result_FAIL_PlatformError:             return "FAIL_PlatformError";
        case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists:      return "FAIL_FeatureAlreadyExists";
        case NVSDK_NGX_Result_FAIL_FeatureNotFound:           return "FAIL_FeatureNotFound";
        case NVSDK_NGX_Result_FAIL_InvalidParameter:          return "FAIL_InvalidParameter";
        case NVSDK_NGX_Result_FAIL_NotInitialized:            return "FAIL_NotInitialized";
        case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return "FAIL_UnableToInitializeFeature";
        case NVSDK_NGX_Result_FAIL_OutOfDate:                 return "FAIL_OutOfDate";
        case NVSDK_NGX_Result_FAIL_OutOfGPUMemory:            return "FAIL_OutOfGPUMemory";
        case NVSDK_NGX_Result_FAIL_Denied:                    return "FAIL_Denied";
        case NVSDK_NGX_Result_FAIL_NotImplemented:            return "FAIL_NotImplemented";
        case SEH_FAULT:                                       return "CRASH_INSIDE_NGX";
        default:                                              return "unmapped";
        }
    }

    // NGX's own messages. The snippet's NvAPI failure line, if there is one,
    // arrives here - that line is what separates the two failure kinds.
    void NVSDK_CONV ngx_cb(const char *msg, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
    {
        if (msg == nullptr) return;
        char b[1600];
        snprintf(b, sizeof b, "  [NGX] %s", msg);
        size_t n = strlen(b);
        while (n > 0 && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = '\0';
        out("%s", b);
    }

    IDXGIAdapter1 *adapter_at(UINT index)
    {
        IDXGIFactory1 *f = nullptr;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return nullptr;
        IDXGIAdapter1 *a = nullptr;
        if (f->EnumAdapters1(index, &a) != S_OK) a = nullptr;
        f->Release();
        return a;
    }

    ID3D12Device *make_device(UINT index, const char *role)
    {
        IDXGIAdapter1 *a = adapter_at(index);
        if (a == nullptr) { out("%s device: adapter[%u] not found", role, index); return nullptr; }
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        ID3D12Device *dev = nullptr;
        HRESULT hr = D3D12CreateDevice(a, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
        out("%s device: adapter[%u] \"%ls\" luid=%08lX-%08lX D3D12CreateDevice hr=0x%08X",
            role, index, d.Description, (unsigned long)d.AdapterLuid.HighPart,
            (unsigned long)d.AdapterLuid.LowPart, (unsigned)hr);
        a->Release();
        return SUCCEEDED(hr) ? dev : nullptr;
    }

    HMODULE load_core()
    {
        HMODULE m = GetModuleHandleW(L"_nvngx.dll");
        if (m != nullptr) { out("core _nvngx.dll: already resident"); return m; }
        m = LoadLibraryW(L"_nvngx.dll");
        if (m != nullptr) { out("core _nvngx.dll: loaded by name"); return m; }
        // The registry route recorded in P0_RECORD section 09.
        wchar_t dir[MAX_PATH] = {};
        DWORD sz = sizeof dir;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore",
                         L"FullPath", RRF_RT_REG_SZ, nullptr, dir, &sz) == ERROR_SUCCESS)
        {
            wchar_t p[MAX_PATH * 2] = {};
            _snwprintf_s(p, MAX_PATH * 2, _TRUNCATE, L"%s\\_nvngx.dll", dir);
            m = LoadLibraryW(p);
            out("core _nvngx.dll: registry FullPath \"%ls\" -> %s", p, m ? "loaded" : "NOT loaded");
            return m;
        }
        out("core _nvngx.dll: NOT FOUND (no NVIDIA NGX core on this system?)");
        return nullptr;
    }

    FARPROC pick(HMODULE preferred, HMODULE other, const char *name, const char **from,
                 const char *pn, const char *on)
    {
        FARPROC p = preferred ? GetProcAddress(preferred, name) : nullptr;
        if (p != nullptr) { *from = pn; return p; }
        p = other ? GetProcAddress(other, name) : nullptr;
        *from = (p != nullptr) ? on : "missing";
        return p;
    }
}

namespace
{
    struct ngx_api
    {
        pf_init init = nullptr;
        pf_get_caps caps = nullptr;
        pf_destroy destroy = nullptr;
        pf_create create = nullptr;
        pf_release release = nullptr;
        pf_init_ext s_iext = nullptr;
        pf_populate s_pop = nullptr;
        wchar_t data_path[MAX_PATH] = {};
        NVSDK_NGX_FeatureCommonInfo common{};
    };

    // Init -> GetCapabilityParameters -> snippet Init_Ext -> Populate -> sizes.
    // Init in the add-on's order: once (probe); if not Success, once more on
    // the same device (arm), result ignored (V31). FAIL_OutOfDate is not a gate.
    NVSDK_NGX_Parameter *open_session(const ngx_api &n, ID3D12Device *dev)
    {
        NVSDK_NGX_Result r = n.init(0ULL, n.data_path, dev, &n.common, NVSDK_NGX_Version_API);
        out("  core Init: 0x%08X (%s)", (unsigned)r, rname(r));
        if (r != NVSDK_NGX_Result_Success)
        {
            r = n.init(0ULL, n.data_path, dev, &n.common, NVSDK_NGX_Version_API);
            out("  core Init again (arm order, V31 - result ignored): 0x%08X (%s)", (unsigned)r, rname(r));
        }
        NVSDK_NGX_Parameter *params = nullptr;
        r = n.caps(&params);
        out("  GetCapabilityParameters: 0x%08X (%s)", (unsigned)r, rname(r));
        if (r != NVSDK_NGX_Result_Success || params == nullptr) return nullptr;
        if (n.s_iext != nullptr)
        {
            r = n.s_iext(0ULL, n.data_path, dev, NVSDK_NGX_Version_API, params);
            out("  snippet Init_Ext: 0x%08X (%s)", (unsigned)r, rname(r));
        }
        if (n.s_pop != nullptr)
        {
            r = n.s_pop(params);
            out("  snippet PopulateParameters_Impl: 0x%08X (%s)", (unsigned)r, rname(r));
        }
        const unsigned W = 1280, H = 720;
        params->Set(NVSDK_NGX_Parameter_Width, W);
        params->Set(NVSDK_NGX_Parameter_Height, H);
        params->Set("DLSSNR.Width", W);
        params->Set("DLSSNR.Height", H);
        return params;
    }

    // One arm: fresh queue, allocator, list and fence on `dev`, CreateFeature,
    // drain, release the handle. Returns the CreateFeature result, or
    // SEH_FAULT with *seh set. Nothing NGX recorded is executed after a fault.
    NVSDK_NGX_Result arm_once(const ngx_api &n, ID3D12Device *dev, NVSDK_NGX_Parameter *params,
                              unsigned long *seh, pf_foreign synthetic)
    {
        ID3D12CommandQueue *q = nullptr;
        ID3D12CommandAllocator *al = nullptr;
        ID3D12GraphicsCommandList *cl = nullptr;
        ID3D12Fence *fe = nullptr;
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) ||
            FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al))) ||
            FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, IID_PPV_ARGS(&cl))) ||
            FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fe))))
        {
            out("  D3D12 objects for the arm could not be created");
            if (cl) cl->Release(); if (al) al->Release(); if (q) q->Release(); if (fe) fe->Release();
            return NVSDK_NGX_Result_FAIL_PlatformError;
        }

        NVSDK_NGX_Handle *h = nullptr;
        LARGE_INTEGER f{}, t0{}, t1{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        const NVSDK_NGX_Result r = (synthetic != nullptr)
            ? create_foreign_guarded(synthetic, n.create, cl, params, &h, seh)
            : create_guarded(n.create, cl, params, &h, seh);
        QueryPerformanceCounter(&t1);
        if ((unsigned)r == SEH_FAULT)
        {
            out("  CreateFeature(Reserved18): CRASH INSIDE NGX (exception 0x%08lX)", *seh);
            return r;   // deliberately leaked: nothing NGX touched is reused
        }
        out("  CreateFeature(Reserved18) 1280x720%s: 0x%08X (%s) handle=%p elapsed=%.0fms",
            synthetic ? " [SYNTHETIC - called from nrcheck.exe]" : "",
            (unsigned)r, rname(r), (void *)h,
            (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart);

        cl->Close();
        ID3D12CommandList *ls[1] = { cl };
        q->ExecuteCommandLists(1, ls);
        q->Signal(fe, 1);
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (ev != nullptr && fe->GetCompletedValue() < 1)
        {
            fe->SetEventOnCompletion(1, ev);
            WaitForSingleObject(ev, 10000);
        }
        if (ev != nullptr) CloseHandle(ev);
        if (h != nullptr && n.release != nullptr) n.release(h);
        cl->Release(); al->Release(); fe->Release(); q->Release();
        return (r == NVSDK_NGX_Result_Success && h != nullptr) ? NVSDK_NGX_Result_Success : r;
    }
}

// Exit codes the parent reads:
//   0      attempt 1 succeeded - the first arm works on this GPU
//   10+k   attempt k (2..8) was the first to succeed - the arm self-healed
//   1      no attempt succeeded
//   2      a crash inside NGX ended the ladder
//   3      the check could not start (devices, modules, exports)
//   6      SYNTHETIC run only: a synthetic attempt did not return
//          0xBAD00002, so the reference run is not valid
//   5      NGX was not usable in this process, so no attempt reached
//          CreateFeature. This is about the check, not the machine: the
//          parent starts a fresh process and tries again. Measured on a
//          healthy rig 2026-09-29: once the first Init in a cold process
//          leaves GetCapabilityParameters at FAIL_NotInitialized, every later
//          attempt in that process does too, new devices included, while a
//          fresh process has come up clean.
extern "C" __declspec(dllexport)
int nrcheck_run(int case_id, int neural_index, int other_index,
                const wchar_t *snippet_path, const wchar_t *log_path, int synthetic_n)
{
    if (log_path != nullptr && log_path[0] != L'\0') _wfopen_s(&g_out, log_path, L"a");
    const bool two_gpus = (case_id == 1);   // 1 = other GPU present, 3 = neural GPU only

    out("---- neural adapter[%d]: self-healing arm%s ----", neural_index,
        two_gpus ? ", the other GPU's device created first and kept alive" : " (one GPU)");

    ID3D12Device *other = nullptr;
    if (two_gpus)
    {
        other = make_device((UINT)other_index, "other (the game's)");
        if (other == nullptr) { out("RESULT: SKIPPED (no device on the other GPU)"); return 3; }
    }
    ID3D12Device *devs[3] = {};
    devs[0] = make_device((UINT)neural_index, "neural");
    if (devs[0] == nullptr) { out("RESULT: SKIPPED (no device on the neural GPU)"); return 3; }

    HMODULE core = load_core();
    HMODULE snip = LoadLibraryW(snippet_path);
    out("snippet \"%ls\": %s", snippet_path, snip ? "loaded" : "NOT loaded");
    if (core == nullptr || snip == nullptr) { out("RESULT: SKIPPED (modules)"); return 3; }

    ngx_api n;
    const char *w1, *w2, *w3, *w4, *w5;
    n.init    = (pf_init)    pick(core, snip, "NVSDK_NGX_D3D12_Init", &w1, "core", "snippet!FALLBACK");
    n.caps    = (pf_get_caps)pick(core, snip, "NVSDK_NGX_D3D12_GetCapabilityParameters", &w2, "core", "snippet!FALLBACK");
    n.destroy = (pf_destroy) pick(core, snip, "NVSDK_NGX_D3D12_DestroyParameters", &w3, "core", "snippet!FALLBACK");
    n.create  = (pf_create)  pick(snip, core, "NVSDK_NGX_D3D12_CreateFeature", &w4, "snippet", "core!FALLBACK");
    n.release = (pf_release) pick(snip, core, "NVSDK_NGX_D3D12_ReleaseFeature", &w5, "snippet", "core!FALLBACK");
    n.s_iext  = (pf_init_ext)GetProcAddress(snip, "NVSDK_NGX_D3D12_Init_Ext");
    n.s_pop   = (pf_populate)GetProcAddress(snip, "NVSDK_NGX_D3D12_PopulateParameters_Impl");
    out("exports: Init=%s Caps=%s Destroy=%s CreateFeature=%s Release=%s snippet Init_Ext=%s Populate=%s",
        w1, w2, w3, w4, w5, n.s_iext ? "yes" : "no", n.s_pop ? "yes" : "no");
    if (!n.init || !n.caps || !n.create) { out("RESULT: SKIPPED (missing exports)"); return 3; }

    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&nrcheck_run, &self);
        GetModuleFileNameW(self, n.data_path, MAX_PATH);
        wchar_t *sl = wcsrchr(n.data_path, L'\\');
        if (sl != nullptr) sl[1] = L'\0';
    }
    n.common.LoggingInfo.LoggingCallback = ngx_cb;
    n.common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
    n.common.LoggingInfo.DisableOtherLoggingSinks = false;

    static const char *const kWhat[9] = {
        "",
        "first arm - what the add-on does today",
        "same NGX session, 2 s later, fresh command list",
        "clean up, Init again, arm",
        "clean up, Init again, arm - after 2 s",
        "clean up, Init again, arm - after 4 s",
        "clean up, Init again, arm - after 8 s",
        "NEW device on the neural GPU (old one kept), Init on it, arm",
        "old neural devices RELEASED, another new device, Init on it, arm" };

    ID3D12Device *dev = devs[0];
    NVSDK_NGX_Parameter *params = nullptr;
    bool reached_create = false;   // only attempts that called CreateFeature count
    for (int k = 1; k <= 8; ++k)
    {
        out("");
        out("attempt %d: %s", k, kWhat[k]);

        // ---- what changes for this attempt ----
        if (k == 2) Sleep(2000);
        if (k >= 3)
        {
            // Clean up the failed arm: the handle was already released by
            // arm_once; the parameter block goes here.
            if (params != nullptr && n.destroy != nullptr)
            {
                const NVSDK_NGX_Result d = n.destroy(params);
                out("  DestroyParameters: 0x%08X (%s)", (unsigned)d, rname(d));
            }
            params = nullptr;
        }
        if (k == 4) Sleep(2000);
        if (k == 5) Sleep(4000);
        if (k == 6) Sleep(8000);
        if (k == 7)
        {
            devs[1] = make_device((UINT)neural_index, "neural (new, old kept)");
            if (devs[1] == nullptr) { out("  skipped: no new device"); continue; }
            dev = devs[1];
        }
        if (k == 8)
        {
            for (int i = 0; i < 2; ++i)
                if (devs[i] != nullptr) { devs[i]->Release(); devs[i] = nullptr; }
            out("  earlier neural devices released");
            devs[2] = make_device((UINT)neural_index, "neural (new, old released)");
            if (devs[2] == nullptr) { out("  skipped: no new device"); continue; }
            dev = devs[2];
        }

        if (params == nullptr) params = open_session(n, dev);
        if (params == nullptr)
        {
            out("  no parameter block - this attempt could not arm");
            if (!reached_create)
            {
                // Nothing in this process has reached CreateFeature, and the
                // measurement above says nothing later in it will. Hand back
                // to the parent for a fresh process instead of burning the
                // ladder on setup.
                out("SETUP: NGX not usable in this process - CreateFeature never reached");
                return 5;
            }
            continue;
        }
        reached_create = true;

        unsigned long seh = 0;
        pf_foreign fwd = nullptr;
        if (k <= synthetic_n)
        {
            fwd = (pf_foreign)GetProcAddress(GetModuleHandleW(nullptr), "nrcheck_foreign_create");
            if (fwd == nullptr)
            {
                out("RESULT: SYNTHETIC NOT AVAILABLE - nrcheck.exe does not export the forwarder");
                return 6;
            }
            out("  this attempt is SYNTHETIC: NGX is expected to refuse it with 0xBAD00002");
        }
        const NVSDK_NGX_Result r = arm_once(n, dev, params, &seh, fwd);
        if (fwd != nullptr && (unsigned)r != SEH_FAULT &&
            r != NVSDK_NGX_Result_FAIL_PlatformError)
        {
            out("RESULT: SYNTHETIC INVALID - NGX returned 0x%08X (%s), not 0xBAD00002, "
                "for a call from nrcheck.exe", (unsigned)r, rname(r));
            if (params != nullptr && n.destroy != nullptr) n.destroy(params);
            return 6;
        }
        if ((unsigned)r == SEH_FAULT)
        {
            out("RESULT: CRASH inside NGX at attempt %d - ladder stopped (V44)", k);
            return 2;
        }
        if (r == NVSDK_NGX_Result_Success)
        {
            if (k == 1) out("RESULT: PASS at attempt 1 - the first arm works on this GPU");
            else        out("RESULT: HEALED at attempt %d - %s", k, kWhat[k]);
            if (params != nullptr && n.destroy != nullptr) n.destroy(params);
            return (k == 1) ? 0 : 10 + k;
        }
    }
    out("RESULT: NOT HEALED - CreateFeature was reached and failed on every attempt that reached it");
    (void)other;   // the game's device stays alive until the process exits
    return 1;
}

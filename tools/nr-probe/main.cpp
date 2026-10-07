// nr-probe - phase 2 bring-up for nr-bridge.
//
// Starts DLSS Neural Rendering (DLSS 5) on the NVIDIA GPU from a process that
// also holds a D3D12 device on the AMD render GPU - the situation the bridge is
// in inside Cyberpunk running on the R9700 - then runs it on a still image at
// several resolutions, times it with GPU timestamps and saves before/after PNGs.
//
// The startup is MGPU Bridge's working sequence:
//   core     _nvngx.dll        NVSDK_NGX_D3D12_Init, GetCapabilityParameters
//   snippet  nvngx_dlssnr.dll  Init_Ext, PopulateParameters_Impl,
//                              CreateFeature(Reserved18), EvaluateFeature, ReleaseFeature
// Unlike upstream, the core is loaded from the driver folder named by
// HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\FullPath, because a process
// rendering on AMD never has it loaded already.
//
// The snippet only serves callers whose module path contains "nvngx.dll", so this
// program is built as nvngx.dll_nr-probe.exe.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <initguid.h>
#include <dxcore.h>
#include <dxgi1_6.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "nvsdk_ngx.h"
#include "nrb_dxgi_unhide.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------- errors, text

struct HrError : std::runtime_error {
    HRESULT hr;
    HrError(const std::string& what, HRESULT h) : std::runtime_error(what), hr(h) {}
};

std::string hex32(uint32_t v) {
    char b[16];
    snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

void check(HRESULT hr, const std::string& what) {
    if (FAILED(hr)) throw HrError(what + " -> " + hex32((uint32_t)hr), hr);
}

#define CHECK(expr) check((expr), #expr)

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

FILE* gLogFile = nullptr;
std::mutex gOutMutex;

// Everything goes to stdout and to <out>\nr-probe.log, flushed per line: NGX
// may take over the console, and a crash inside it must not eat the tail.
void out(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(gOutMutex);
    fputs(buf, stdout);
    fflush(stdout);
    if (gLogFile) {
        fputs(buf, gLogFile);
        fflush(gLogFile);
    }
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

const char* ngxResultName(NVSDK_NGX_Result r) {
    switch ((uint32_t)r) {
    case NVSDK_NGX_Result_Success: return "Success";
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported: return "FAIL_FeatureNotSupported";
    case NVSDK_NGX_Result_FAIL_PlatformError: return "FAIL_PlatformError (caller gate?)";
    case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists: return "FAIL_FeatureAlreadyExists";
    case NVSDK_NGX_Result_FAIL_FeatureNotFound: return "FAIL_FeatureNotFound";
    case NVSDK_NGX_Result_FAIL_InvalidParameter: return "FAIL_InvalidParameter";
    case NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall: return "FAIL_ScratchBufferTooSmall";
    case NVSDK_NGX_Result_FAIL_NotInitialized: return "FAIL_NotInitialized";
    case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat: return "FAIL_UnsupportedInputFormat";
    case NVSDK_NGX_Result_FAIL_RWFlagMissing: return "FAIL_RWFlagMissing";
    case NVSDK_NGX_Result_FAIL_MissingInput: return "FAIL_MissingInput";
    case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return "FAIL_UnableToInitializeFeature";
    case NVSDK_NGX_Result_FAIL_OutOfDate: return "FAIL_OutOfDate (harmless at Init per upstream)";
    case NVSDK_NGX_Result_FAIL_OutOfGPUMemory: return "FAIL_OutOfGPUMemory";
    case NVSDK_NGX_Result_FAIL_UnsupportedFormat: return "FAIL_UnsupportedFormat";
    case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath: return "FAIL_UnableToWriteToAppDataPath";
    case NVSDK_NGX_Result_FAIL_UnsupportedParameter: return "FAIL_UnsupportedParameter";
    case NVSDK_NGX_Result_FAIL_Denied: return "FAIL_Denied";
    case NVSDK_NGX_Result_FAIL_NotImplemented: return "FAIL_NotImplemented";
    default: return "?";
    }
}

// ---------------------------------------------------------------- NGX entry points
// Typedefs spelled out for the exact exported ABI, as upstream does: the app-facing
// header declares Init with C++ overloads that do not match the core's export.

// [NRB11] The core's D3D12_Init takes (AppId, DataPath, Device, SDKVersion) - the DLSS SDK 310.7 header and
// the 617.14 driver agree. Declaring a FeatureCommonInfo* 4th put a stack address where the version goes,
// and the core's "version > 0x15 -> FAIL_OutOfDate" check then passed or failed with ASLR.
using PfnInit = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version);
using PfnInitExt = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
                                                 const NVSDK_NGX_Parameter*);
using PfnGetCapParams = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using PfnPopulateParams = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);  // not in any public header
using PfnCreateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                                       NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using PfnEvaluateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                         const NVSDK_NGX_Parameter*, void*);
using PfnReleaseFeature = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);

constexpr uint32_t kSehFault = 0xDEAD0001u;  // ours, not an NGX code

// SEH boundaries around vendor calls we cannot inspect. Their own functions
// because MSVC forbids __try where objects need unwinding.
NVSDK_NGX_Result guardedCreate(PfnCreateFeature fn, ID3D12GraphicsCommandList* cl, NVSDK_NGX_Parameter* p,
                               NVSDK_NGX_Handle** out, DWORD* code) {
    __try {
        return fn(cl, NVSDK_NGX_Feature_Reserved18, p, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return (NVSDK_NGX_Result)kSehFault;
    }
}

NVSDK_NGX_Result guardedEvaluate(PfnEvaluateFeature fn, ID3D12GraphicsCommandList* cl, const NVSDK_NGX_Handle* h,
                                 const NVSDK_NGX_Parameter* p, DWORD* code) {
    __try {
        return fn(cl, h, p, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return (NVSDK_NGX_Result)kSehFault;
    }
}

bool gVerbose = false;
int gInitAttempts = 5;
DWORD gInitRetryMs = 1000;
bool gSnippetLate = false;
bool gReloadCore = false;
bool gShutdown = false;    // --shutdown: DestroyParameters + Shutdown1 before exit (default: leave it to process exit)   // --reload-core: FreeLibrary + LoadLibrary _nvngx.dll between Init attempts
unsigned gSdkVersion = NVSDK_NGX_Version_API;   // --sdk-version: what we tell NGX we were built against

void NVSDK_CONV ngxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    size_t n = strlen(message);
    out("    [ngx] %s%s", message, (n && message[n - 1] == '\n') ? "" : "\n");
}

// ---------------------------------------------------------------- options

struct Options {
    std::wstring snippet, image, outDir;
    std::vector<std::pair<UINT, UINT>> sizes;
    UINT frames = 120;
    float intensity = 2.0f;
    bool amdDevice = true;
    std::wstring rtfix;             // RTInitFix sl.interposer.dll to load first, as the game would
    std::string rtfixHooks = "both";  // dxgi | d3d12 | both
    bool unhide = false;            // install the bridge's NRB6 caller-aware adapter unhiding
};

bool parseRes(const std::string& r, UINT& w, UINT& h) {
    if (r == "1080p") { w = 1920; h = 1080; return true; }
    if (r == "1440p") { w = 2560; h = 1440; return true; }
    if (r == "1800p") { w = 2880; h = 1800; return true; }
    if (r == "4k" || r == "2160p") { w = 3840; h = 2160; return true; }
    return sscanf_s(r.c_str(), "%ux%u", &w, &h) == 2 && w >= 64 && h >= 64;
}

std::wstring repoRoot() {
    // <repo>\build\nr-probe\nvngx.dll_nr-probe.exe -> <repo>
    wchar_t p[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
    std::wstring s = p;
    for (int i = 0; i < 3; ++i) s = s.substr(0, s.find_last_of(L"\\/"));
    return s;
}

void usage() {
    puts(R"(nr-probe - start DLSS Neural Rendering on the NVIDIA GPU and time it

  --snippet PATH   nvngx_dlssnr.dll to load (default <repo>\vendor\nvidia\nvngx_dlssnr.dll)
  --image PATH     still image to process (default: the Dawnwalker scene from the MGPU repo)
  --res LIST       comma-separated: 1080p,1440p,1800p,4k or WxH (default 1080p,1440p,1800p,4k)
  --frames N       evaluates per resolution for timing (default 120)
  --intensity F    DLSSNR.Intensity (default 2.0, MGPU Bridge's default)
  --no-amd         don't create a device on the AMD GPU first
  --rtfix PATH     load RTInitFix's sl.interposer.dll first and trigger its process-wide hooks
  --rtfix-hooks H  dxgi (hide NVIDIA adapters) | d3d12 (mask DXR) | both (default)
  --unhide         install the bridge's NRB6 fix: NVIDIA's own code sees hidden adapters again
  --out DIR        where PNGs and NGX logs go (default <repo>\results\nr-probe)
  --verbose        NGX verbose logging
  --init-attempts N    core Init attempts before giving up (default 5)
  --init-retry-ms MS   pause between Init attempts (default 1000)
  --shutdown           close the NGX session (DestroyParameters, Shutdown1) before exiting)");
}

Options parseArgs(int argc, char** argv) {
    Options o;
    std::wstring root = repoRoot();
    o.snippet = root + L"\\vendor\\nvidia\\nvngx_dlssnr.dll";
    o.image = root + L"\\reference\\Neural-coprocessor\\docs\\scenes\\Scene.png";
    o.outDir = root + L"\\results\\nr-probe";
    std::string res = "1080p,1440p,1800p,4k";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value after " + a);
            return argv[++i];
        };
        if (a == "--snippet") o.snippet = widen(next());
        else if (a == "--image") o.image = widen(next());
        else if (a == "--res") res = next();
        else if (a == "--frames") o.frames = std::max(1u, (UINT)std::stoul(next()));
        else if (a == "--intensity") o.intensity = std::stof(next());
        else if (a == "--no-amd") o.amdDevice = false;
        else if (a == "--rtfix") o.rtfix = widen(next());
        else if (a == "--rtfix-hooks") o.rtfixHooks = next();
        else if (a == "--unhide") o.unhide = true;
        else if (a == "--snippet-late") gSnippetLate = true;
        else if (a == "--shutdown") gShutdown = true;
        else if (a == "--reload-core") gReloadCore = true;
        else if (a == "--sdk-version") gSdkVersion = (unsigned)std::stoul(next(), nullptr, 0);
        else if (a == "--out") o.outDir = widen(next());
        else if (a == "--verbose") gVerbose = true;
        else if (a == "--init-attempts") gInitAttempts = std::max(1, std::stoi(next()));
        else if (a == "--init-retry-ms") gInitRetryMs = (DWORD)std::stoul(next());
        else if (a == "-h" || a == "--help") { usage(); exit(0); }
        else throw std::runtime_error("unknown option " + a + " (try --help)");
    }
    size_t start = 0;
    while (start <= res.size()) {
        size_t end = res.find(',', start);
        std::string tok = res.substr(start, end == std::string::npos ? std::string::npos : end - start);
        UINT w = 0, h = 0;
        if (!parseRes(tok, w, h)) throw std::runtime_error("bad resolution '" + tok + "'");
        o.sizes.push_back({w, h});
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return o;
}

// ---------------------------------------------------------------- images (WIC)

ComPtr<IWICImagingFactory> wic() {
    static ComPtr<IWICImagingFactory> f;
    if (!f) CHECK(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)));
    return f;
}

std::vector<uint8_t> loadRgba(const std::wstring& path, UINT w, UINT h) {
    ComPtr<IWICBitmapDecoder> dec;
    check(wic()->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec),
          "open image " + narrow(path));
    ComPtr<IWICBitmapFrameDecode> frame;
    CHECK(dec->GetFrame(0, &frame));
    ComPtr<IWICBitmapScaler> scaler;
    CHECK(wic()->CreateBitmapScaler(&scaler));
    CHECK(scaler->Initialize(frame.Get(), w, h, WICBitmapInterpolationModeFant));
    ComPtr<IWICFormatConverter> conv;
    CHECK(wic()->CreateFormatConverter(&conv));
    CHECK(conv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                           WICBitmapPaletteTypeCustom));
    std::vector<uint8_t> px((size_t)w * h * 4);
    CHECK(conv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data()));
    return px;
}

void savePng(const std::wstring& path, UINT w, UINT h, const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> bgra(rgba);
    for (size_t i = 0; i < bgra.size(); i += 4) {
        std::swap(bgra[i], bgra[i + 2]);
        bgra[i + 3] = 255;
    }
    ComPtr<IWICStream> stream;
    CHECK(wic()->CreateStream(&stream));
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "create " + narrow(path));
    ComPtr<IWICBitmapEncoder> enc;
    CHECK(wic()->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc));
    CHECK(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    CHECK(enc->CreateNewFrame(&frame, nullptr));
    CHECK(frame->Initialize(nullptr));
    CHECK(frame->SetSize(w, h));
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    CHECK(frame->SetPixelFormat(&fmt));
    CHECK(frame->WritePixels(h, w * 4, (UINT)bgra.size(), bgra.data()));
    CHECK(frame->Commit());
    CHECK(enc->Commit());
}

// ---------------------------------------------------------------- D3D12 on the NVIDIA GPU

struct Gpu {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 value = 0;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~Gpu() { CloseHandle(event); }

    void open() {
        CHECK(alloc->Reset());
        CHECK(list->Reset(alloc.Get(), nullptr));
    }

    void submitAndWait() {
        CHECK(list->Close());
        ID3D12CommandList* l[] = {list.Get()};
        queue->ExecuteCommandLists(1, l);
        CHECK(queue->Signal(fence.Get(), ++value));
        if (fence->GetCompletedValue() < value) {
            CHECK(fence->SetEventOnCompletion(value, event));
            if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) {
                HRESULT why = dev->GetDeviceRemovedReason();
                throw std::runtime_error("GPU did not finish within 10 s (device removed reason " + hex32((uint32_t)why) + ")");
            }
        }
    }
};

ComPtr<ID3D12Resource> makeTexture(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags,
                                   D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Flags = flags;
    ComPtr<ID3D12Resource> r;
    CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)));
    return r;
}

ComPtr<ID3D12Resource> makeBuffer(ID3D12Device* dev, D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)));
    return r;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

struct Adapters {
    ComPtr<IDXGIAdapter1> nvidia, amd;
    std::string nvidiaName, amdName;
};

Adapters findAdapters() {
    ComPtr<IDXGIFactory6> factory;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    Adapters out;
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i, a.Reset()) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (d.VendorId == 0x10DE && !out.nvidia) {
            out.nvidia = a;
            out.nvidiaName = narrow(d.Description);
        } else if (d.VendorId == 0x1002 && !out.amd) {
            ComPtr<ID3D12Device> dev;
            if (FAILED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)))) continue;
            D3D12_FEATURE_DATA_ARCHITECTURE1 arch{};
            dev->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &arch, sizeof arch);
            if (arch.UMA) continue;  // skip the iGPU; we want the render card
            out.amd = a;
            out.amdName = narrow(d.Description);
        }
    }

    // Same fallback as the bridge's NRB5: RTInitFix hides NVIDIA adapters from every DXGI
    // factory in the process, so find the card through DXCore and open it by LUID.
    if (!out.nvidia) {
        ComPtr<IDXCoreAdapterFactory> xf;
        ComPtr<IDXCoreAdapterList> list;
        const GUID attr = DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS;
        if (SUCCEEDED(DXCoreCreateAdapterFactory(IID_PPV_ARGS(&xf))) &&
            SUCCEEDED(xf->CreateAdapterList(1, &attr, IID_PPV_ARGS(&list)))) {
            for (uint32_t i = 0; i < list->GetAdapterCount() && !out.nvidia; ++i) {
                ComPtr<IDXCoreAdapter> xa;
                LUID luid{};
                if (FAILED(list->GetAdapter(i, IID_PPV_ARGS(&xa))) ||
                    FAILED(xa->GetProperty(DXCoreAdapterProperty::InstanceLuid, sizeof luid, &luid)))
                    continue;
                ComPtr<IDXGIAdapter1> byLuid;
                if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&byLuid)))) continue;
                DXGI_ADAPTER_DESC1 d{};
                byLuid->GetDesc1(&d);
                if (d.VendorId == 0x10DE) {
                    out.nvidia = byLuid;
                    out.nvidiaName = narrow(d.Description) + " (hidden from DXGI enumeration, found via DXCore)";
                }
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------- RTInitFix in-process

using PfnCreateFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
using PfnCreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
PfnCreateDevice gRtFixCreateDevice = nullptr;

// Loads RTInitFix's sl.interposer.dll and triggers the hooks the game would: creating a DXGI
// factory through it patches the factory vtable (hides NVIDIA), and creating a D3D12 device
// through it patches CheckFeatureSupport (masks DXR). Both are process-wide.
void loadRtFix(const Options& o) {
    if (o.rtfix.empty()) return;
    HMODULE m = LoadLibraryExW(o.rtfix.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) throw std::runtime_error("cannot load " + narrow(o.rtfix) + " (error " + std::to_string(GetLastError()) + ")");
    out("RTInitFix    %s (hooks: %s)\n", narrow(o.rtfix).c_str(), o.rtfixHooks.c_str());
    if (o.rtfixHooks == "dxgi" || o.rtfixHooks == "both") {
        auto f2 = reinterpret_cast<PfnCreateFactory2>(GetProcAddress(m, "CreateDXGIFactory2"));
        ComPtr<IDXGIFactory4> f;
        if (!f2 || FAILED(f2(0, IID_PPV_ARGS(&f)))) throw std::runtime_error("RTInitFix CreateDXGIFactory2 failed");
        out("             DXGI factory created through it -> EnumAdapters* now hide NVIDIA\n");
    }
    if (o.rtfixHooks == "d3d12" || o.rtfixHooks == "both")
        gRtFixCreateDevice = reinterpret_cast<PfnCreateDevice>(GetProcAddress(m, "D3D12CreateDevice"));
}

// ---------------------------------------------------------------- NGX modules

std::wstring ngxCorePathFromRegistry() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return {};
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof buf, type = 0, installed = 0, isz = sizeof installed;
    LSTATUS s1 = RegQueryValueExW(key, L"FullPath", nullptr, &type, (BYTE*)buf, &size);
    LSTATUS s2 = RegQueryValueExW(key, L"Installed", nullptr, nullptr, (BYTE*)&installed, &isz);
    RegCloseKey(key);
    if (s1 != ERROR_SUCCESS || (s2 == ERROR_SUCCESS && installed != 1)) return {};
    std::wstring p = buf;
    if (!p.empty() && p.back() != L'\\') p += L'\\';
    return p + L"_nvngx.dll";
}

std::string fileVersion(const std::wstring& path) {
    DWORD dummy = 0, n = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!n) return "?";
    std::vector<uint8_t> buf(n);
    if (!GetFileVersionInfoW(path.c_str(), 0, n, buf.data())) return "?";
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) || !fi) return "?";
    char b[64];
    snprintf(b, sizeof b, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
             HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    return b;
}

std::wstring modulePath(HMODULE m) {
    wchar_t p[MAX_PATH * 2] = {};
    GetModuleFileNameW(m, p, MAX_PATH * 2);
    return p;
}

struct Ngx {
    HMODULE core = nullptr, snippet = nullptr;
    PfnInit init = nullptr;
    PfnGetCapParams getCapParams = nullptr;
    PfnInitExt initExt = nullptr;
    PfnPopulateParams populate = nullptr;
    PfnCreateFeature create = nullptr;
    PfnEvaluateFeature evaluate = nullptr;
    PfnReleaseFeature release = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
};

template <typename T>
T resolve(HMODULE m, const char* name, const char* from) {
    auto p = reinterpret_cast<T>(GetProcAddress(m, name));
    if (!p) throw std::runtime_error(std::string(name) + " is not exported by " + from);
    return p;
}

void loadSnippet(Ngx& n, const Options& o);

void loadNgx(Ngx& n, const Options& o) {
    // Does creating the NVIDIA device pull the core in by itself? Worth knowing for the bridge.
    n.core = GetModuleHandleW(L"_nvngx.dll");
    if (n.core) {
        out("  core     already resident: %s\n", narrow(modulePath(n.core)).c_str());
    } else {
        std::wstring p = ngxCorePathFromRegistry();
        if (p.empty()) throw std::runtime_error("NGXCore\\FullPath not found in the registry - is the NVIDIA driver installed?");
        n.core = LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!n.core) throw std::runtime_error("LoadLibrary failed for " + narrow(p) + " (error " + std::to_string(GetLastError()) + ")");
        out("  core     loaded from registry path: %s\n", narrow(p).c_str());
    }
    out("           version %s\n", fileVersion(modulePath(n.core)).c_str());

    n.init = resolve<PfnInit>(n.core, "NVSDK_NGX_D3D12_Init", "the core");
    n.getCapParams = resolve<PfnGetCapParams>(n.core, "NVSDK_NGX_D3D12_GetCapabilityParameters", "the core");
    if (!gSnippetLate) loadSnippet(n, o);
}

// The DLSS-NR snippet. Upstream loads it before the core's Init; --snippet-late loads it
// after, to test whether the core's NGXInitValidateSnippets check (the intermittent
// FAIL_OutOfDate) is reacting to the snippet already being in the process.
void loadSnippet(Ngx& n, const Options& o) {
    n.snippet = LoadLibraryExW(o.snippet.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!n.snippet)
        throw std::runtime_error("LoadLibrary failed for " + narrow(o.snippet) + " (error " + std::to_string(GetLastError()) + ")");
    out("  snippet  %s\n           version %s%s\n", narrow(o.snippet).c_str(), fileVersion(o.snippet).c_str(),
        gSnippetLate ? "  (loaded after core Init)" : "");
    n.initExt = resolve<PfnInitExt>(n.snippet, "NVSDK_NGX_D3D12_Init_Ext", "the snippet");
    n.populate = resolve<PfnPopulateParams>(n.snippet, "NVSDK_NGX_D3D12_PopulateParameters_Impl", "the snippet");
    n.create = resolve<PfnCreateFeature>(n.snippet, "NVSDK_NGX_D3D12_CreateFeature", "the snippet");
    n.evaluate = resolve<PfnEvaluateFeature>(n.snippet, "NVSDK_NGX_D3D12_EvaluateFeature", "the snippet");
    n.release = resolve<PfnReleaseFeature>(n.snippet, "NVSDK_NGX_D3D12_ReleaseFeature", "the snippet");
}

void initNgx(Ngx& n, const Options& o, ID3D12Device* dev, const std::wstring& dataPath) {
    NVSDK_NGX_FeatureCommonInfo common{};
    common.LoggingInfo.LoggingCallback = ngxLog;
    common.LoggingInfo.MinimumLoggingLevel = gVerbose ? NVSDK_NGX_LOGGING_LEVEL_VERBOSE : NVSDK_NGX_LOGGING_LEVEL_ON;
    common.LoggingInfo.DisableOtherLoggingSinks = false;

    // Core Init intermittently returns FAIL_OutOfDate and leaves the core uninitialised
    // (GetCapabilityParameters -> FAIL_NotInitialized). Retry in-process to learn whether
    // that recovers without relaunching - the bridge cannot relaunch the game.
    NVSDK_NGX_Result r{};
    for (int attempt = 1; attempt <= gInitAttempts; ++attempt) {
        if (attempt > 1) Sleep(gInitRetryMs);
        if (attempt > 1 && gReloadCore) {
            // Unload the core completely and load it again, so the next Init starts from a
            // freshly initialised module rather than one that has already refused once.
            wchar_t corePath[MAX_PATH * 2] = {};
            GetModuleFileNameW(n.core, corePath, MAX_PATH * 2);
            int frees = 0;
            while (GetModuleHandleW(L"_nvngx.dll") && frees < 16 && FreeLibrary(n.core)) ++frees;
            const bool gone = GetModuleHandleW(L"_nvngx.dll") == nullptr;
            // The core loads NVAPI and NVAPI stays resident; drop it too so its per-process
            // state is rebuilt on the next Init.
            int nvapiFrees = 0;
            for (HMODULE nv; (nv = GetModuleHandleW(L"nvapi64.dll")) != nullptr && nvapiFrees < 32 && FreeLibrary(nv);) ++nvapiFrees;
            out("  nvapi64.dll: %d FreeLibrary, still loaded=%s; nvapi64_impl.dll still loaded=%s\n", nvapiFrees,
                GetModuleHandleW(L"nvapi64.dll") ? "yes" : "no", GetModuleHandleW(L"nvapi64_impl.dll") ? "yes" : "no");
            n.core = LoadLibraryExW(corePath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!n.core) throw std::runtime_error("core reload failed");
            n.init = resolve<PfnInit>(n.core, "NVSDK_NGX_D3D12_Init", "the core");
            n.getCapParams = resolve<PfnGetCapParams>(n.core, "NVSDK_NGX_D3D12_GetCapabilityParameters", "the core");
            out("  core reloaded (%d FreeLibrary, fully unloaded=%s)\n", frees, gone ? "yes" : "no");
        }
        r = n.init(0ULL, dataPath.c_str(), dev, (NVSDK_NGX_Version)gSdkVersion);
        out("  core Init (attempt %d)     %s %s\n", attempt, hex32((uint32_t)r).c_str(), ngxResultName(r));
        r = n.getCapParams(&n.params);
        out("  GetCapabilityParameters   %s %s\n", hex32((uint32_t)r).c_str(), ngxResultName(r));
        if (NVSDK_NGX_SUCCEED(r) && n.params) break;
    }
    if (NVSDK_NGX_FAILED(r) || !n.params)
        throw std::runtime_error("no NGX parameter block - cannot continue");

    if (gSnippetLate) loadSnippet(n, o);
    r = n.initExt(0ULL, dataPath.c_str(), dev, (NVSDK_NGX_Version)gSdkVersion, n.params);
    out("  snippet Init_Ext          %s %s\n", hex32((uint32_t)r).c_str(), ngxResultName(r));
    r = n.populate(n.params);
    out("  PopulateParameters_Impl   %s %s\n", hex32((uint32_t)r).c_str(), ngxResultName(r));
}

// ---------------------------------------------------------------- one resolution

struct Timing {
    double avg = 0, p50 = 0, p95 = 0, max = 0;
};

Timing summarize(std::vector<double> v) {
    Timing t;
    if (v.empty()) return t;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    t.avg = sum / (double)v.size();
    t.p50 = v[v.size() / 2];
    t.p95 = v[std::min(v.size() - 1, (size_t)(0.95 * (double)(v.size() - 1) + 0.5))];
    t.max = v.back();
    return t;
}

struct ResResult {
    UINT w = 0, h = 0;
    std::string error;
    double createMs = 0;
    Timing gpu;
    double meanAbsDiff = 0;
};

ResResult runResolution(Gpu& g, Ngx& n, const Options& o, UINT w, UINT h) {
    ResResult rr;
    rr.w = w;
    rr.h = h;
    const DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;  // UNORM, never _SRGB (upstream P7.8)
    out("    %ux%u: loading image\n", w, h);
    auto pixels = loadRgba(o.image, w, h);

    auto color = makeTexture(g.dev.Get(), w, h, fmt, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    auto output = makeTexture(g.dev.Get(), w, h, fmt, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Upload the image.
    D3D12_RESOURCE_DESC cd = color->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    g.dev->GetCopyableFootprints(&cd, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto upload = makeBuffer(g.dev.Get(), D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* up = nullptr;
    CHECK(upload->Map(0, nullptr, (void**)&up));
    for (UINT y = 0; y < h; ++y) memcpy(up + y * fp.Footprint.RowPitch, pixels.data() + (size_t)y * w * 4, (size_t)w * 4);
    upload->Unmap(0, nullptr);

    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = 2;
    ComPtr<ID3D12QueryHeap> qheap;
    CHECK(g.dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&qheap)));
    auto qread = makeBuffer(g.dev.Get(), D3D12_HEAP_TYPE_READBACK, 16, D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64 tsFreq = 0;
    CHECK(g.queue->GetTimestampFrequency(&tsFreq));

    g.open();
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = color.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    auto b = transition(color.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    g.list->ResourceBarrier(1, &b);
    g.submitAndWait();

    out("    %ux%u: CreateFeature\n", w, h);
    // Create the feature at this size.
    n.params->Set("DLSSNR.Width", (unsigned int)w);
    n.params->Set("DLSSNR.Height", (unsigned int)h);
    NVSDK_NGX_Handle* handle = nullptr;
    DWORD seh = 0;
    g.open();
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    NVSDK_NGX_Result r = guardedCreate(n.create, g.list.Get(), n.params, &handle, &seh);
    QueryPerformanceCounter(&t1);
    rr.createMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    if ((uint32_t)r == kSehFault) {
        rr.error = "CreateFeature crashed inside NGX (exception " + hex32(seh) + ")";
        return rr;  // do not touch NGX or this list again
    }
    g.submitAndWait();
    if (NVSDK_NGX_FAILED(r) || !handle) {
        rr.error = "CreateFeature -> " + hex32((uint32_t)r) + " " + ngxResultName(r);
        return rr;
    }

    out("    %ux%u: evaluating %u frames\n", w, h, o.frames);
    // Evaluate repeatedly, timing each on the GPU.
    std::vector<double> gpuMs;
    for (UINT i = 0; i < o.frames; ++i) {
        n.params->Set("DLSSNR.Color", color.Get());
        n.params->Set("DLSSNR.Output", output.Get());
        n.params->Set("DLSSNR.ColorSubrectBaseX", 0u);
        n.params->Set("DLSSNR.ColorSubrectBaseY", 0u);
        n.params->Set("DLSSNR.ColorSubrectWidth", (unsigned int)w);
        n.params->Set("DLSSNR.ColorSubrectHeight", (unsigned int)h);
        n.params->Set("DLSSNR.OutputSubrectBaseX", 0u);
        n.params->Set("DLSSNR.OutputSubrectBaseY", 0u);
        n.params->Set("DLSSNR.OutputSubrectWidth", (unsigned int)w);
        n.params->Set("DLSSNR.OutputSubrectHeight", (unsigned int)h);
        n.params->Set("DLSSNR.MVec", (ID3D12Resource*)nullptr);   // color-only, like upstream v0.1.0
        n.params->Set("DLSSNR.Depth", (ID3D12Resource*)nullptr);
        n.params->Set("DLSSNR.Intensity", o.intensity);
        n.params->Set("DLSSNR.Reset", i == 0 ? 1u : 0u);

        g.open();
        g.list->EndQuery(qheap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        r = guardedEvaluate(n.evaluate, g.list.Get(), handle, n.params, &seh);
        if ((uint32_t)r == kSehFault) {
            rr.error = "EvaluateFeature crashed inside NGX (exception " + hex32(seh) + ")";
            return rr;
        }
        g.list->EndQuery(qheap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        g.list->ResolveQueryData(qheap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, qread.Get(), 0);
        g.submitAndWait();
        if (NVSDK_NGX_FAILED(r)) {
            rr.error = "EvaluateFeature -> " + hex32((uint32_t)r) + " " + ngxResultName(r);
            n.release(handle);
            return rr;
        }
        UINT64* ts = nullptr;
        CHECK(qread->Map(0, nullptr, (void**)&ts));
        if (i > 0) gpuMs.push_back((double)(ts[1] - ts[0]) * 1000.0 / (double)tsFreq);  // skip the reset frame
        D3D12_RANGE none{0, 0};
        qread->Unmap(0, &none);
    }
    rr.gpu = summarize(gpuMs);

    out("    %ux%u: reading back and saving PNGs\n", w, h);
    // Read the output back and save before / after / split.
    D3D12_RESOURCE_DESC od = output->GetDesc();
    g.dev->GetCopyableFootprints(&od, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto readback = makeBuffer(g.dev.Get(), D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
    g.open();
    b = transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.list->ResourceBarrier(1, &b);
    dst = {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    src = {};
    src.pResource = output.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    g.submitAndWait();
    n.release(handle);

    std::vector<uint8_t> out((size_t)w * h * 4);
    uint8_t* rb = nullptr;
    CHECK(readback->Map(0, nullptr, (void**)&rb));
    for (UINT y = 0; y < h; ++y) memcpy(out.data() + (size_t)y * w * 4, rb + y * fp.Footprint.RowPitch, (size_t)w * 4);
    D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);

    uint64_t diff = 0;
    for (size_t i = 0; i < out.size(); i += 4)
        for (int c = 0; c < 3; ++c) diff += (uint64_t)std::abs((int)out[i + c] - (int)pixels[i + c]);
    rr.meanAbsDiff = (double)diff / ((double)w * h * 3);

    std::vector<uint8_t> split(pixels);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = w / 2; x < w; ++x) {
            size_t i = ((size_t)y * w + x) * 4;
            bool line = x < w / 2 + 2;
            for (int c = 0; c < 3; ++c) split[i + c] = line ? 255 : out[i + c];
        }
    std::wstring tag = std::to_wstring(w) + L"x" + std::to_wstring(h);
    savePng(o.outDir + L"\\before_" + tag + L".png", w, h, pixels);
    savePng(o.outDir + L"\\after_" + tag + L".png", w, h, out);
    savePng(o.outDir + L"\\split_" + tag + L".png", w, h, split);
    return rr;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Options o = parseArgs(argc, argv);
        CHECK(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        CreateDirectoryW((repoRoot() + L"\\results").c_str(), nullptr);
        CreateDirectoryW(o.outDir.c_str(), nullptr);
        _wfopen_s(&gLogFile, (o.outDir + L"\\nr-probe.log").c_str(), L"w");

        loadRtFix(o);
        if (o.unhide) nrb::install_dxgi_unhide([](const char* s) { out("             %s\n", s); });
        Adapters ad = findAdapters();
        if (!ad.nvidia) throw std::runtime_error("no NVIDIA adapter found");

        // Hold a device on the AMD GPU for the whole run, like a game rendering there.
        ComPtr<ID3D12Device> amdDevice;
        if (o.amdDevice && ad.amd) {
            if (gRtFixCreateDevice) {
                // As the game does: through RTInitFix, which hooks CheckFeatureSupport on it.
                CHECK(gRtFixCreateDevice(ad.amd.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&amdDevice)));
                D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
                amdDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5);
                out("             AMD device created through it -> RaytracingTier now reads %d\n", (int)o5.RaytracingTier);
            } else
            CHECK(D3D12CreateDevice(ad.amd.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&amdDevice)));
            out("render GPU   %s  (D3D12 device created first and held open)\n", ad.amdName.c_str());
        } else {
            out("render GPU   (none - %s)\n", o.amdDevice ? "no AMD discrete GPU found" : "--no-amd");
        }

        Gpu g;
        CHECK(D3D12CreateDevice(ad.nvidia.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g.dev)));
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        CHECK(g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue)));
        CHECK(g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc)));
        CHECK(g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc.Get(), nullptr, IID_PPV_ARGS(&g.list)));
        CHECK(g.list->Close());
        CHECK(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)));
        out("neural GPU   %s\n\nNGX modules:\n", ad.nvidiaName.c_str());

        Ngx n;
        loadNgx(n, o);
        out("\nNGX startup:\n");
        initNgx(n, o, g.dev.Get(), o.outDir);

        out("\nimage: %s\n\n", narrow(o.image).c_str());
        out("  %-11s %-10s %-44s %s\n", "resolution", "create", "GPU time per evaluate (ms)", "mean |after-before|");
        std::vector<ResResult> results;
        for (auto [w, h] : o.sizes) {
            ResResult rr = runResolution(g, n, o, w, h);
            char res[32];
            snprintf(res, sizeof res, "%ux%u", w, h);
            if (!rr.error.empty()) {
                out("  %-11s FAILED: %s\n", res, rr.error.c_str());
                if (rr.error.find("crashed") != std::string::npos) break;
                continue;
            }
            out("  %-11s %7.0f ms  avg %6.2f  p50 %6.2f  p95 %6.2f  max %6.2f   %5.2f / 255\n", res, rr.createMs,
                   rr.gpu.avg, rr.gpu.p50, rr.gpu.p95, rr.gpu.max, rr.meanAbsDiff);
        }
        out("\nPNGs (before_*, after_*, split_*): %s\n", narrow(o.outDir).c_str());
        // Process exit reclaims the NGX session by default; upstream found Shutdown1 can crash.
        // --shutdown tests whether an unclosed session is what makes the NEXT process's Init fail
        // FAIL_OutOfDate (runs alternate success/fail when each one exits without closing).
        if (gShutdown) {
            using PfnDestroyParams = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
            using PfnShutdown1 = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
            auto destroyParams = reinterpret_cast<PfnDestroyParams>(GetProcAddress(n.core, "NVSDK_NGX_D3D12_DestroyParameters"));
            auto shutdown1 = reinterpret_cast<PfnShutdown1>(GetProcAddress(n.core, "NVSDK_NGX_D3D12_Shutdown1"));
            if (destroyParams && n.params) {
                out("  DestroyParameters ...\n");
                const NVSDK_NGX_Result r = destroyParams(n.params);
                out("  DestroyParameters         %s %s\n", hex32((uint32_t)r).c_str(), ngxResultName(r));
                n.params = nullptr;
            }
            if (shutdown1) {
                out("  Shutdown1 ...\n");
                const NVSDK_NGX_Result r = shutdown1(g.dev.Get());
                out("  Shutdown1                 %s %s\n", hex32((uint32_t)r).c_str(), ngxResultName(r));
            } else {
                out("  Shutdown1 not exported by the core\n");
            }
        }
        out("done\n");
        return 0;
    } catch (const std::exception& e) {
        out("error: %s\n", e.what());
        return 1;
    }
}

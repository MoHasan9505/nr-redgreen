// xadapter-probe - phase 1 go/no-go test for nr-bridge.
//
// Every "frame" it moves what the neural-rendering GPU would need - output-res
// color plus render-res depth and motion vectors - from a source GPU to a
// destination GPU, the way MGPU Bridge does, and reports latency, throughput
// and data integrity. No game, no NGX: just the transport.
//
// Routes:
//   shared      cross-adapter heap created on src, opened on dst (MGPU's route A)
//   shared-dst  same, but the heap is created on dst and opened on src
//   address     one VirtualAlloc region opened on both devices via
//               OpenExistingHeapFromAddress (MGPU's route A')
//   cpu         src -> readback buffer, CPU memcpy, upload buffer -> dst
//               (the always-works fallback)
//
// Sync: a SHARED_CROSS_ADAPTER fence lets the dst queue wait on src on the GPU.
// If that can't be created or opened, the probe falls back to CPU waits.
//
// Integrity: each frame stamps its id into the first and last texels of the
// color lane and writes a 64-byte seal after the payload. The destination reads
// both back; a mismatch is reported as seal / torn / stale.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------- errors

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

const char* vendorName(UINT id) {
    switch (id) {
    case 0x1002: return "AMD";
    case 0x10DE: return "NVIDIA";
    case 0x8086: return "Intel";
    case 0x1414: return "Microsoft";
    default: return "other";
    }
}

// ---------------------------------------------------------------- timing

double qpcFreq() {
    static const double f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return (double)x.QuadPart;
    }();
    return f;
}

int64_t now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double msBetween(int64_t a, int64_t b) { return (double)(b - a) * 1000.0 / qpcFreq(); }

struct Summary {
    double avg = 0, p50 = 0, p95 = 0, p99 = 0, max = 0;
};

Summary summarize(std::vector<double> v) {
    Summary s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    auto pct = [&](double p) { return v[std::min(v.size() - 1, (size_t)(p * (double)(v.size() - 1) + 0.5))]; };
    s.avg = sum / (double)v.size();
    s.p50 = pct(0.50);
    s.p95 = pct(0.95);
    s.p99 = pct(0.99);
    s.max = v.back();
    return s;
}

// ---------------------------------------------------------------- options

struct Options {
    int src = -1, dst = -1;
    UINT width = 2560, height = 1440;
    double scale = 0.667;
    bool hdr = false, depth = true, mvec = true;
    UINT frames = 600, warmup = 30, slots = 3;
    bool copyQueue = true;
    std::string route = "all", mode = "both", fence = "auto";
    bool list = false, debug = false;
};

void usage() {
    puts(R"(xadapter-probe - measure GPU-to-GPU frame transport for nr-bridge

  --list                 list adapters and their cross-adapter capabilities, then exit
  --src N / --dst N      adapter indices from --list (default: first AMD -> first NVIDIA,
                         or the next discrete GPU if there is no NVIDIA card)
  --res R                1080p | 1440p | 1800p (2880x1800) | 4k | WxH   (default 1440p)
  --scale S              render scale for the depth/motion lanes (default 0.667 = FSR Quality)
  --hdr                  RGBA16F color instead of RGBA8
  --no-depth, --no-mv    drop a lane
  --route R              shared | shared-dst | address | cpu | all     (default all)
  --mode M               serial | pipelined | both                     (default both)
  --fence F              auto | gpu | cpu                              (default auto)
  --queue Q              copy | direct                                 (default copy)
  --frames N             measured frames per run (default 600, plus 30 warm-up)
  --slots N              ring slots in flight (default 3)
  --debug                enable the D3D12 debug layer and print its warnings/errors)");
}

bool parseRes(const std::string& r, UINT& w, UINT& h) {
    if (r == "1080p") { w = 1920; h = 1080; return true; }
    if (r == "1440p") { w = 2560; h = 1440; return true; }
    if (r == "1800p") { w = 2880; h = 1800; return true; }
    if (r == "4k" || r == "2160p") { w = 3840; h = 2160; return true; }
    return sscanf_s(r.c_str(), "%ux%u", &w, &h) == 2 && w >= 4 && h >= 1;
}

Options parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value after " + a);
            return argv[++i];
        };
        if (a == "--list") o.list = true;
        else if (a == "--src") o.src = std::stoi(next());
        else if (a == "--dst") o.dst = std::stoi(next());
        else if (a == "--res") { if (!parseRes(next(), o.width, o.height)) throw std::runtime_error("bad --res"); }
        else if (a == "--scale") o.scale = std::stod(next());
        else if (a == "--hdr") o.hdr = true;
        else if (a == "--no-depth") o.depth = false;
        else if (a == "--no-mv") o.mvec = false;
        else if (a == "--route") o.route = next();
        else if (a == "--mode") o.mode = next();
        else if (a == "--fence") o.fence = next();
        else if (a == "--queue") o.copyQueue = next() != "direct";
        else if (a == "--frames") o.frames = std::max(2u, (UINT)std::stoul(next()));
        else if (a == "--slots") o.slots = std::max(1u, (UINT)std::stoul(next()));
        else if (a == "--debug") o.debug = true;
        else if (a == "-h" || a == "--help") { usage(); exit(0); }
        else throw std::runtime_error("unknown option " + a + " (try --help)");
    }
    return o;
}

// ---------------------------------------------------------------- adapters

struct Adapter {
    UINT index = 0;
    std::string name;
    UINT vendor = 0;
    LUID luid{};
    uint64_t vram = 0;
    bool software = false, d3d12 = false, uma = false;
    BOOL rowMajorXA = FALSE;
    D3D12_CROSS_NODE_SHARING_TIER crossNode{};
    D3D12_RESOURCE_HEAP_TIER heapTier{};
    D3D12_SHARED_RESOURCE_COMPATIBILITY_TIER sharedTier{};
    ComPtr<IDXGIAdapter1> dxgi;
};

std::vector<Adapter> enumerateAdapters() {
    ComPtr<IDXGIFactory6> factory;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    std::vector<Adapter> out;
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        Adapter e;
        e.index = i;
        e.name = narrow(d.Description);
        e.vendor = d.VendorId;
        e.luid = d.AdapterLuid;
        e.vram = d.DedicatedVideoMemory;
        e.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        e.dxgi = a;
        ComPtr<ID3D12Device> dev;
        if (SUCCEEDED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)))) {
            e.d3d12 = true;
            D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
            if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o))) {
                e.rowMajorXA = o.CrossAdapterRowMajorTextureSupported;
                e.crossNode = o.CrossNodeSharingTier;
                e.heapTier = o.ResourceHeapTier;
            }
            D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4{};
            if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &o4, sizeof o4)))
                e.sharedTier = o4.SharedResourceCompatibilityTier;
            D3D12_FEATURE_DATA_ARCHITECTURE1 arch{};
            if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &arch, sizeof arch)))
                e.uma = arch.UMA;
        }
        out.push_back(std::move(e));
        a.Reset();
    }
    return out;
}

void printAdapters(const std::vector<Adapter>& as) {
    printf("Adapters:\n");
    for (auto& a : as) {
        printf("  [%u] %-36s %-9s vram %5.1f GB  luid %08lX:%08lX  %s\n", a.index, a.name.c_str(),
               vendorName(a.vendor), (double)a.vram / 1073741824.0, (unsigned long)a.luid.HighPart,
               (unsigned long)a.luid.LowPart,
               a.software ? "software" : !a.d3d12 ? "no D3D12" : a.uma ? "integrated" : "discrete");
        if (a.d3d12 && !a.software)
            printf("       crossAdapterRowMajorTextures=%d  crossNodeSharingTier=%d  resourceHeapTier=%d  "
                   "sharedResourceCompatibilityTier=%d\n",
                   a.rowMajorXA, (int)a.crossNode, (int)a.heapTier, (int)a.sharedTier);
    }
}

void pickAdapters(const std::vector<Adapter>& as, const Options& o, int& src, int& dst) {
    auto usable = [&](int i) { return i >= 0 && i < (int)as.size() && as[i].d3d12 && !as[i].software; };
    auto first = [&](auto pred, int exclude) {
        for (int i = 0; i < (int)as.size(); ++i)
            if (usable(i) && i != exclude && pred(as[i])) return i;
        return -1;
    };
    src = o.src;
    if (src < 0) src = first([](const Adapter& a) { return a.vendor == 0x1002 && !a.uma; }, -1);
    if (src < 0) src = first([](const Adapter& a) { return !a.uma; }, -1);
    dst = o.dst;
    if (dst < 0) dst = first([](const Adapter& a) { return a.vendor == 0x10DE; }, src);
    if (dst < 0) dst = first([](const Adapter& a) { return !a.uma; }, src);
    if (dst < 0) dst = first([](const Adapter&) { return true; }, src);
    if (!usable(src) || !usable(dst)) throw std::runtime_error("could not find two usable D3D12 adapters (see --list)");
    if (src == dst) throw std::runtime_error("--src and --dst must be different adapters");
}

void enableDebugLayer() {
    ComPtr<ID3D12Debug> d;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&d)))) d->EnableDebugLayer();
    else puts("warning: D3D12 debug layer unavailable (install the 'Graphics Tools' optional Windows feature)");
}

void hookDebugMessages(ID3D12Device* dev) {
    ComPtr<ID3D12InfoQueue1> q;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&q)))) return;
    DWORD cookie = 0;
    q->RegisterMessageCallback(
        [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY sev, D3D12_MESSAGE_ID, LPCSTR desc, void*) {
            if (sev <= D3D12_MESSAGE_SEVERITY_WARNING)
                fprintf(stderr, "  [d3d12 %s] %s\n", sev <= D3D12_MESSAGE_SEVERITY_ERROR ? "error" : "warning", desc);
        },
        D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
}

// ---------------------------------------------------------------- frame layout

// Each ring slot in the bridge buffer: [seal][color][depth][mvec], every lane
// at a 512-byte aligned offset in the layout GetCopyableFootprints gives it.
constexpr UINT64 kSealRegion = 512;
constexpr UINT64 kStampStride = 1024;  // src upload, per slot: [0] stamp texels, [512] seal
constexpr UINT64 kCheckStride = 2048;  // dst readback, per slot: [0] seal, [512] first stamp, [1024] last stamp
constexpr UINT kStampTexels = 4;
constexpr uint32_t kSealMagic = 0x4C53524E;  // "NRSL"

struct Seal {
    uint32_t magic, slot;
    uint64_t frame, qpc;
    uint8_t pad[40];
};
static_assert(sizeof(Seal) == 64);

UINT64 alignUp(UINT64 v, UINT64 a) { return (v + a - 1) / a * a; }

struct Lane {
    const char* name;
    DXGI_FORMAT format;
    UINT bpp, width, height;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};  // Offset is relative to the slot base
    UINT64 footprintBytes = 0;
    uint64_t payloadBytes() const { return (uint64_t)width * height * bpp; }
};

struct Layout {
    std::vector<Lane> lanes;
    UINT64 slotStride = 0;
    uint64_t payload = 0;
};

D3D12_RESOURCE_DESC texDesc(const Lane& l) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = l.width;
    d.Height = l.height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = l.format;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return d;
}

Layout makeLayout(ID3D12Device* dev, const Options& o) {
    Layout L;
    UINT rw = std::max(1u, (UINT)std::lround(o.width * o.scale));
    UINT rh = std::max(1u, (UINT)std::lround(o.height * o.scale));
    L.lanes.push_back({"color", o.hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM,
                       o.hdr ? 8u : 4u, o.width, o.height});
    if (o.depth) L.lanes.push_back({"depth", DXGI_FORMAT_R32_FLOAT, 4, rw, rh});
    if (o.mvec) L.lanes.push_back({"mvec", DXGI_FORMAT_R16G16_FLOAT, 4, rw, rh});
    UINT64 off = kSealRegion;
    for (auto& l : L.lanes) {
        auto d = texDesc(l);
        UINT64 total = 0;
        dev->GetCopyableFootprints(&d, 0, 1, 0, &l.footprint, nullptr, nullptr, &total);
        l.footprint.Offset = off;
        l.footprintBytes = total;
        off = alignUp(off + total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        L.payload += l.payloadBytes();
    }
    L.slotStride = alignUp(off, 65536);
    return L;
}

// ---------------------------------------------------------------- D3D12 helpers

D3D12_RESOURCE_DESC bufDesc(UINT64 size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    return d;
}

ComPtr<ID3D12Resource> makeBuffer(ID3D12Device* dev, D3D12_HEAP_TYPE type, UINT64 size,
                                  D3D12_RESOURCE_STATES state, uint8_t** map) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    auto d = bufDesc(size);
    ComPtr<ID3D12Resource> r;
    CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)));
    if (map) {
        D3D12_RANGE none{0, 0};
        CHECK(r->Map(0, type == D3D12_HEAP_TYPE_READBACK ? nullptr : &none, reinterpret_cast<void**>(map)));
    }
    return r;
}

D3D12_TEXTURE_COPY_LOCATION subLoc(ID3D12Resource* r) {
    D3D12_TEXTURE_COPY_LOCATION l{};
    l.pResource = r;
    l.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    l.SubresourceIndex = 0;
    return l;
}

D3D12_TEXTURE_COPY_LOCATION fpLoc(ID3D12Resource* r, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp) {
    D3D12_TEXTURE_COPY_LOCATION l{};
    l.pResource = r;
    l.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    l.PlacedFootprint = fp;
    return l;
}

D3D12_PLACED_SUBRESOURCE_FOOTPRINT stampFootprint(DXGI_FORMAT fmt, UINT64 offset) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT f{};
    f.Offset = offset;
    f.Footprint.Format = fmt;
    f.Footprint.Width = kStampTexels;
    f.Footprint.Height = 1;
    f.Footprint.Depth = 1;
    f.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    return f;
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

void fillStamp(uint8_t* p, uint64_t frame, UINT bytes) {
    uint32_t v = 0x3C000000u | (uint32_t)(frame & 0xFFFFFF);
    for (UINT i = 0; i < bytes; i += 4) memcpy(p + i, &v, 4);
}

void waitFence(ID3D12Fence* f, UINT64 value, HANDLE ev, const char* what) {
    if (f->GetCompletedValue() >= value) return;
    CHECK(f->SetEventOnCompletion(value, ev));
    if (WaitForSingleObject(ev, 5000) != WAIT_OBJECT_0)
        throw std::runtime_error(std::string("timed out after 5 s waiting for ") + what + " (fence at " +
                                 std::to_string(f->GetCompletedValue()) + ", wanted " + std::to_string(value) + ")");
}

// ---------------------------------------------------------------- rig

struct Side {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    std::vector<ComPtr<ID3D12CommandAllocator>> allocs;  // one per slot
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;  // local completion fence
    std::vector<ComPtr<ID3D12Resource>> tex;  // one per lane
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~Side() { CloseHandle(event); }
};

std::unique_ptr<Side> makeSide(IDXGIAdapter1* adapter, const Layout& L, const Options& o) {
    auto s = std::make_unique<Side>();
    CHECK(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&s->dev)));
    if (o.debug) hookDebugMessages(s->dev.Get());
    auto type = o.copyQueue ? D3D12_COMMAND_LIST_TYPE_COPY : D3D12_COMMAND_LIST_TYPE_DIRECT;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = type;
    CHECK(s->dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&s->queue)));
    s->allocs.resize(o.slots);
    for (auto& a : s->allocs) CHECK(s->dev->CreateCommandAllocator(type, IID_PPV_ARGS(&a)));
    CHECK(s->dev->CreateCommandList(0, type, s->allocs[0].Get(), nullptr, IID_PPV_ARGS(&s->list)));
    CHECK(s->list->Close());
    CHECK(s->dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s->fence)));
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    for (auto& l : L.lanes) {
        auto d = texDesc(l);
        ComPtr<ID3D12Resource> t;
        CHECK(s->dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                              IID_PPV_ARGS(&t)));
        s->tex.push_back(t);
    }
    return s;
}

// The memory both GPUs see, and how they synchronize on it.
struct Transport {
    ComPtr<ID3D12Heap> srcHeap, dstHeap;
    ComPtr<ID3D12Resource> srcBuf, dstBuf;      // src's and dst's view of the bridge memory
    uint8_t* srcMap = nullptr;                  // cpu route: readback on src
    uint8_t* dstMap = nullptr;                  // cpu route: upload on dst
    ComPtr<ID3D12Fence> xSrc, xDst;             // shared cross-adapter fence: src's / dst's view
    void* region = nullptr;                     // address route
    std::string note;
    ~Transport() {
        srcBuf.Reset();
        dstBuf.Reset();
        srcHeap.Reset();
        dstHeap.Reset();
        if (region) VirtualFree(region, 0, MEM_RELEASE);
    }
};

void setupSharedHeap(Transport& t, Side& src, Side& dst, bool heapOnSrc, UINT64 size) {
    Side& owner = heapOnSrc ? src : dst;
    Side& other = heapOnSrc ? dst : src;
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = size;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
    ComPtr<ID3D12Heap> ownerHeap, otherHeap;
    check(owner.dev->CreateHeap(&hd, IID_PPV_ARGS(&ownerHeap)), "CreateHeap(SHARED|SHARED_CROSS_ADAPTER)");
    HANDLE h = nullptr;
    check(owner.dev->CreateSharedHandle(ownerHeap.Get(), nullptr, GENERIC_ALL, nullptr, &h), "CreateSharedHandle(heap)");
    HRESULT hr = other.dev->OpenSharedHandle(h, IID_PPV_ARGS(&otherHeap));
    CloseHandle(h);
    check(hr, "OpenSharedHandle(heap) on the other adapter");
    auto d = bufDesc(size, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
    ComPtr<ID3D12Resource> ownerBuf, otherBuf;
    check(owner.dev->CreatePlacedResource(ownerHeap.Get(), 0, &d, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                          IID_PPV_ARGS(&ownerBuf)),
          "CreatePlacedResource on the heap's owner");
    check(other.dev->CreatePlacedResource(otherHeap.Get(), 0, &d, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                          IID_PPV_ARGS(&otherBuf)),
          "CreatePlacedResource on the opened heap");
    t.srcHeap = heapOnSrc ? ownerHeap : otherHeap;
    t.dstHeap = heapOnSrc ? otherHeap : ownerHeap;
    t.srcBuf = heapOnSrc ? ownerBuf : otherBuf;
    t.dstBuf = heapOnSrc ? otherBuf : ownerBuf;
}

void setupAddressHeap(Transport& t, Side& src, Side& dst, UINT64 size) {
    t.region = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!t.region) throw std::runtime_error("VirtualAlloc failed");
    ComPtr<ID3D12Device3> s3, d3;
    check(src.dev.As(&s3), "ID3D12Device3 on src");
    check(dst.dev.As(&d3), "ID3D12Device3 on dst");
    check(s3->OpenExistingHeapFromAddress(t.region, IID_PPV_ARGS(&t.srcHeap)), "OpenExistingHeapFromAddress on src");
    check(d3->OpenExistingHeapFromAddress(t.region, IID_PPV_ARGS(&t.dstHeap)), "OpenExistingHeapFromAddress on dst");
    auto place = [&](ID3D12Device* dev, ID3D12Heap* heap, ComPtr<ID3D12Resource>& out, const char* who) {
        auto hd = heap->GetDesc();
        auto flags = (hd.Flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER) ? D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER
                                                                      : D3D12_RESOURCE_FLAG_NONE;
        auto d = bufDesc(size, flags);
        check(dev->CreatePlacedResource(heap, 0, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out)),
              std::string("CreatePlacedResource on ") + who + "'s address heap");
        char b[96];
        snprintf(b, sizeof b, "%s heap flags 0x%X ", who, (unsigned)hd.Flags);
        t.note += b;
    };
    place(src.dev.Get(), t.srcHeap.Get(), t.srcBuf, "src");
    place(dst.dev.Get(), t.dstHeap.Get(), t.dstBuf, "dst");
}

void setupCpuStaging(Transport& t, Side& src, Side& dst, UINT64 size) {
    t.srcBuf = makeBuffer(src.dev.Get(), D3D12_HEAP_TYPE_READBACK, size, D3D12_RESOURCE_STATE_COPY_DEST, &t.srcMap);
    t.dstBuf = makeBuffer(dst.dev.Get(), D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ, &t.dstMap);
}

bool setupSharedFence(Transport& t, Side& src, Side& dst, std::string& why) {
    try {
        check(src.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER,
                                   IID_PPV_ARGS(&t.xSrc)),
              "CreateFence(SHARED|SHARED_CROSS_ADAPTER) on src");
        HANDLE h = nullptr;
        check(src.dev->CreateSharedHandle(t.xSrc.Get(), nullptr, GENERIC_ALL, nullptr, &h), "CreateSharedHandle(fence)");
        HRESULT hr = dst.dev->OpenSharedHandle(h, IID_PPV_ARGS(&t.xDst));
        CloseHandle(h);
        check(hr, "OpenSharedHandle(fence) on dst");
        return true;
    } catch (const std::exception& e) {
        why = e.what();
        t.xSrc.Reset();
        t.xDst.Reset();
        return false;
    }
}

struct Rig {
    std::unique_ptr<Side> src, dst;
    ComPtr<ID3D12Resource> stampUpload;    // src: per-slot stamp texels + seal
    uint8_t* stampMap = nullptr;
    ComPtr<ID3D12Resource> checkReadback;  // dst: per-slot seal + stamp texels read back
    uint8_t* checkMap = nullptr;
    Transport xport;
    bool gpuWait = false;  // dst queue waits on the shared fence instead of the CPU
    const Layout* L = nullptr;
    const Options* o = nullptr;

    ID3D12Fence* srcFence() const { return xport.xSrc ? xport.xSrc.Get() : src->fence.Get(); }
    UINT64 slotBase(UINT slot) const { return slot * L->slotStride; }

    void prepareSlot(UINT slot, uint64_t frame) {
        uint8_t* p = stampMap + slot * kStampStride;
        fillStamp(p, frame, kStampTexels * L->lanes[0].bpp);
        Seal s{};
        s.magic = kSealMagic;
        s.slot = slot;
        s.frame = frame;
        s.qpc = (uint64_t)now();
        memcpy(p + 512, &s, sizeof s);
    }

    // Stamp the frame id into the color lane, then copy every lane and the seal
    // out into this slot of the bridge memory.
    void submitSrc(UINT slot, uint64_t frame) {
        Side& s = *src;
        auto& cl = s.list;
        CHECK(s.allocs[slot]->Reset());
        CHECK(cl->Reset(s.allocs[slot].Get(), nullptr));
        std::vector<D3D12_RESOURCE_BARRIER> b;
        b.push_back(transition(s.tex[0].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST));
        for (size_t i = 1; i < s.tex.size(); ++i)
            b.push_back(transition(s.tex[i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE));
        cl->ResourceBarrier((UINT)b.size(), b.data());

        const Lane& c = L->lanes[0];
        auto stamp = fpLoc(stampUpload.Get(), stampFootprint(c.format, slot * kStampStride));
        auto color = subLoc(s.tex[0].Get());
        cl->CopyTextureRegion(&color, 0, 0, 0, &stamp, nullptr);
        cl->CopyTextureRegion(&color, c.width - kStampTexels, c.height - 1, 0, &stamp, nullptr);
        auto t = transition(s.tex[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->ResourceBarrier(1, &t);

        for (size_t i = 0; i < L->lanes.size(); ++i) {
            auto fp = L->lanes[i].footprint;
            fp.Offset += slotBase(slot);
            auto to = fpLoc(xport.srcBuf.Get(), fp);
            auto from = subLoc(s.tex[i].Get());
            cl->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }
        cl->CopyBufferRegion(xport.srcBuf.Get(), slotBase(slot), stampUpload.Get(), slot * kStampStride + 512,
                             sizeof(Seal));

        b.clear();
        for (auto& tex : s.tex)
            b.push_back(transition(tex.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON));
        cl->ResourceBarrier((UINT)b.size(), b.data());
        CHECK(cl->Close());
        ID3D12CommandList* lists[] = {cl.Get()};
        s.queue->ExecuteCommandLists(1, lists);
        CHECK(s.queue->Signal(srcFence(), frame + 1));
    }

    // CPU route only: move the slot from src's readback memory to dst's upload memory.
    void cpuCopy(UINT slot) {
        if (xport.srcMap) memcpy(xport.dstMap + slotBase(slot), xport.srcMap + slotBase(slot), L->slotStride);
    }

    // Copy every lane into dst-local textures, then read back the seal and the
    // two color stamps so the CPU can verify what arrived.
    void submitDst(UINT slot, uint64_t frame) {
        Side& d = *dst;
        auto& cl = d.list;
        if (gpuWait) CHECK(d.queue->Wait(xport.xDst.Get(), frame + 1));
        CHECK(d.allocs[slot]->Reset());
        CHECK(cl->Reset(d.allocs[slot].Get(), nullptr));
        std::vector<D3D12_RESOURCE_BARRIER> b;
        for (auto& tex : d.tex)
            b.push_back(transition(tex.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST));
        cl->ResourceBarrier((UINT)b.size(), b.data());

        for (size_t i = 0; i < L->lanes.size(); ++i) {
            auto fp = L->lanes[i].footprint;
            fp.Offset += slotBase(slot);
            auto to = subLoc(d.tex[i].Get());
            auto from = fpLoc(xport.dstBuf.Get(), fp);
            cl->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }
        cl->CopyBufferRegion(checkReadback.Get(), slot * kCheckStride, xport.dstBuf.Get(), slotBase(slot), sizeof(Seal));

        b.clear();
        b.push_back(transition(d.tex[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE));
        for (size_t i = 1; i < d.tex.size(); ++i)
            b.push_back(transition(d.tex[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON));
        cl->ResourceBarrier((UINT)b.size(), b.data());

        const Lane& c = L->lanes[0];
        auto color = subLoc(d.tex[0].Get());
        D3D12_BOX first{0, 0, 0, kStampTexels, 1, 1};
        D3D12_BOX last{c.width - kStampTexels, c.height - 1, 0, c.width, c.height, 1};
        auto a = fpLoc(checkReadback.Get(), stampFootprint(c.format, slot * kCheckStride + 512));
        auto z = fpLoc(checkReadback.Get(), stampFootprint(c.format, slot * kCheckStride + 1024));
        cl->CopyTextureRegion(&a, 0, 0, 0, &color, &first);
        cl->CopyTextureRegion(&z, 0, 0, 0, &color, &last);
        auto t = transition(d.tex[0].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        cl->ResourceBarrier(1, &t);
        CHECK(cl->Close());
        ID3D12CommandList* lists[] = {cl.Get()};
        d.queue->ExecuteCommandLists(1, lists);
        CHECK(d.queue->Signal(d.fence.Get(), frame + 1));
    }

    enum class Verdict { ok, seal, torn, stale };

    Verdict verify(UINT slot, uint64_t frame) const {
        const uint8_t* p = checkMap + slot * kCheckStride;
        Seal seal;
        memcpy(&seal, p, sizeof seal);
        if (seal.magic != kSealMagic || seal.frame != frame || seal.slot != slot) return Verdict::seal;
        UINT bytes = kStampTexels * L->lanes[0].bpp;
        uint8_t want[64];
        fillStamp(want, frame, bytes);
        bool a = memcmp(p + 512, want, bytes) == 0;
        bool z = memcmp(p + 1024, want, bytes) == 0;
        if (a && z) return Verdict::ok;
        return a != z ? Verdict::torn : Verdict::stale;
    }

    // Best effort: release any queue stuck on the shared fence and let both go idle.
    void drain() {
        try {
            if (xport.xSrc) xport.xSrc->Signal(1ull << 62);
            for (Side* s : {src.get(), dst.get()}) {
                if (!s) continue;
                s->queue->Signal(s->fence.Get(), 1ull << 62);
                if (s->fence->GetCompletedValue() < (1ull << 62)) {
                    s->fence->SetEventOnCompletion(1ull << 62, s->event);
                    WaitForSingleObject(s->event, 2000);
                }
            }
        } catch (...) {
        }
    }
};

// ---------------------------------------------------------------- runs

struct RunResult {
    std::string mode;
    std::vector<double> total, hop1, cpu, hop2;
    double fps = 0;
    uint32_t sealErr = 0, torn = 0, stale = 0;
    std::string error;

    void tally(Rig::Verdict v) {
        if (v == Rig::Verdict::seal) ++sealErr;
        else if (v == Rig::Verdict::torn) ++torn;
        else if (v == Rig::Verdict::stale) ++stale;
    }
    uint32_t bad() const { return sealErr + torn + stale; }
};

// One frame at a time, CPU-timed at each boundary: shows where the time goes.
RunResult runSerial(Rig& g, uint64_t& frame) {
    const Options& o = *g.o;
    RunResult r;
    r.mode = "serial";
    try {
        for (UINT i = 0; i < o.warmup + o.frames; ++i, ++frame) {
            UINT slot = (UINT)(frame % o.slots);
            g.prepareSlot(slot, frame);
            int64_t t0 = now();
            g.submitSrc(slot, frame);
            waitFence(g.srcFence(), frame + 1, g.src->event, "source copy-out");
            int64_t t1 = now();
            g.cpuCopy(slot);
            int64_t t2 = now();
            g.submitDst(slot, frame);
            waitFence(g.dst->fence.Get(), frame + 1, g.dst->event, "destination copy-in");
            int64_t t3 = now();
            r.tally(g.verify(slot, frame));
            if (i >= o.warmup) {
                r.hop1.push_back(msBetween(t0, t1));
                r.cpu.push_back(msBetween(t1, t2));
                r.hop2.push_back(msBetween(t2, t3));
                r.total.push_back(msBetween(t0, t3));
            }
        }
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    return r;
}

// Up to `slots` frames in flight, like a real game loop: shows sustained
// throughput and the latency each frame sees while the pipe is full.
RunResult runPipelined(Rig& g, uint64_t& frame) {
    const Options& o = *g.o;
    const UINT n = o.warmup + o.frames;
    const uint64_t first = frame;
    std::vector<int64_t> submitted(n), done(n);
    std::atomic<UINT> completed{0};
    std::atomic<bool> failed{false};
    std::string watcherError;
    RunResult r;
    r.mode = "pipelined";

    std::thread watcher([&] {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        try {
            for (UINT i = 0; i < n; ++i) {
                uint64_t f = first + i;
                waitFence(g.dst->fence.Get(), f + 1, ev, "destination copy-in");
                done[i] = now();
                r.tally(g.verify((UINT)(f % o.slots), f));
                completed.store(i + 1);
                completed.notify_all();
            }
        } catch (const std::exception& e) {
            watcherError = e.what();
            failed = true;
            completed.store(n);
            completed.notify_all();
        }
        CloseHandle(ev);
    });

    try {
        for (UINT i = 0; i < n && !failed; ++i) {
            if (i >= o.slots) {
                UINT need = i - o.slots + 1, c;
                while ((c = completed.load()) < need) completed.wait(c);
                if (failed) break;
            }
            uint64_t f = first + i;
            UINT slot = (UINT)(f % o.slots);
            g.prepareSlot(slot, f);
            submitted[i] = now();
            g.submitSrc(slot, f);
            if (!g.gpuWait) {
                waitFence(g.srcFence(), f + 1, g.src->event, "source copy-out");
                g.cpuCopy(slot);
            }
            g.submitDst(slot, f);
        }
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    watcher.join();
    frame = first + n;
    if (r.error.empty()) r.error = watcherError;
    if (!r.error.empty()) return r;

    for (UINT i = o.warmup; i < n; ++i) r.total.push_back(msBetween(submitted[i], done[i]));
    double secs = msBetween(done[o.warmup], done[n - 1]) / 1000.0;
    r.fps = secs > 0 ? (double)(n - 1 - o.warmup) / secs : 0;
    return r;
}

struct RouteReport {
    std::string route, fence, note, setupError;
    std::vector<RunResult> runs;
};

const char* routeBlurb(const std::string& r) {
    if (r == "shared") return "cross-adapter heap created on src, opened on dst";
    if (r == "shared-dst") return "cross-adapter heap created on dst, opened on src";
    if (r == "address") return "one VirtualAlloc region opened on both (OpenExistingHeapFromAddress)";
    if (r == "cpu") return "src readback -> CPU memcpy -> dst upload";
    return "?";
}

RouteReport runRoute(const std::string& route, const Adapter& sa, const Adapter& da, const Layout& L,
                     const Options& o) {
    RouteReport rep;
    rep.route = route;
    Rig g;
    g.L = &L;
    g.o = &o;
    try {
        g.src = makeSide(sa.dxgi.Get(), L, o);
        g.dst = makeSide(da.dxgi.Get(), L, o);
        g.stampUpload = makeBuffer(g.src->dev.Get(), D3D12_HEAP_TYPE_UPLOAD, o.slots * kStampStride,
                                   D3D12_RESOURCE_STATE_GENERIC_READ, &g.stampMap);
        g.checkReadback = makeBuffer(g.dst->dev.Get(), D3D12_HEAP_TYPE_READBACK, o.slots * kCheckStride,
                                     D3D12_RESOURCE_STATE_COPY_DEST, &g.checkMap);
        UINT64 size = L.slotStride * o.slots;
        if (route == "shared") setupSharedHeap(g.xport, *g.src, *g.dst, true, size);
        else if (route == "shared-dst") setupSharedHeap(g.xport, *g.src, *g.dst, false, size);
        else if (route == "address") setupAddressHeap(g.xport, *g.src, *g.dst, size);
        else if (route == "cpu") setupCpuStaging(g.xport, *g.src, *g.dst, size);
        else throw std::runtime_error("unknown route '" + route + "'");
        rep.note = g.xport.note;

        rep.fence = "cpu";
        if (route != "cpu" && o.fence != "cpu") {
            std::string why;
            if (setupSharedFence(g.xport, *g.src, *g.dst, why)) {
                rep.fence = "gpu";
                g.gpuWait = true;
            } else if (o.fence == "gpu") {
                throw std::runtime_error("shared fence: " + why);
            } else {
                rep.note += "shared fence unavailable, using CPU waits: " + why;
            }
        }
    } catch (const std::exception& e) {
        rep.setupError = e.what();
        g.drain();
        return rep;
    }

    uint64_t frame = 0;
    if (o.mode == "serial" || o.mode == "both") rep.runs.push_back(runSerial(g, frame));
    if (o.mode == "pipelined" || o.mode == "both") rep.runs.push_back(runPipelined(g, frame));
    g.drain();
    return rep;
}

void printReport(const RouteReport& rep, const Layout& L) {
    printf("\n== %s  (%s)\n", rep.route.c_str(), routeBlurb(rep.route));
    if (!rep.setupError.empty()) {
        printf("   SETUP FAILED: %s\n", rep.setupError.c_str());
        return;
    }
    printf("   sync: %s%s%s\n", rep.fence == "gpu" ? "GPU wait on shared cross-adapter fence" : "CPU waits",
           rep.note.empty() ? "" : "  | ", rep.note.c_str());
    double mb = (double)L.payload / 1e6;
    for (auto& r : rep.runs) {
        if (!r.error.empty()) {
            printf("   %-9s FAILED: %s\n", r.mode.c_str(), r.error.c_str());
            continue;
        }
        auto t = summarize(r.total);
        if (r.mode == "serial") {
            auto h1 = summarize(r.hop1), c = summarize(r.cpu), h2 = summarize(r.hop2);
            printf("   serial     end-to-end avg %6.2f ms  p95 %6.2f  max %6.2f | src->bridge %5.2f ms (%5.1f GB/s)"
                   "  cpu %5.2f  bridge->dst %5.2f ms (%5.1f GB/s)\n",
                   t.avg, t.p95, t.max, h1.avg, h1.avg > 0 ? mb / h1.avg : 0, c.avg, h2.avg,
                   h2.avg > 0 ? mb / h2.avg : 0);
        } else {
            printf("   pipelined  %7.1f fps  %5.1f GB/s | per-frame latency avg %6.2f ms  p50 %6.2f  p95 %6.2f  "
                   "p99 %6.2f  max %6.2f\n",
                   r.fps, r.fps * mb / 1000.0, t.avg, t.p50, t.p95, t.p99, t.max);
        }
        printf("              integrity: %s", r.bad() ? "" : "all frames intact\n");
        if (r.bad()) printf("%u seal, %u torn, %u stale frames\n", r.sealErr, r.torn, r.stale);
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Options o = parseArgs(argc, argv);
        if (o.debug) enableDebugLayer();
        auto adapters = enumerateAdapters();
        printAdapters(adapters);
        if (o.list) return 0;

        int si = -1, di = -1;
        pickAdapters(adapters, o, si, di);
        const Adapter& sa = adapters[si];
        const Adapter& da = adapters[di];
        printf("\nsource      [%d] %s (%s)\ndestination [%d] %s (%s)%s\n", si, sa.name.c_str(), vendorName(sa.vendor), di,
               da.name.c_str(), vendorName(da.vendor),
               sa.vendor != da.vendor ? "   <- cross-vendor" : "   (same vendor: not the real target pairing)");

        Layout L;
        {
            ComPtr<ID3D12Device> dev;
            CHECK(D3D12CreateDevice(sa.dxgi.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)));
            L = makeLayout(dev.Get(), o);
        }
        printf("\nper frame:\n");
        for (auto& l : L.lanes)
            printf("  %-6s %5u x %-5u %u B/px  %6.2f MB\n", l.name, l.width, l.height, l.bpp, (double)l.payloadBytes() / 1e6);
        printf("  total  %.2f MB payload, %.2f MB slot stride, %u slots, %s queues, %u frames (+%u warm-up)\n",
               (double)L.payload / 1e6, (double)L.slotStride / 1e6, o.slots, o.copyQueue ? "copy" : "direct", o.frames,
               o.warmup);

        std::vector<std::string> routes;
        if (o.route == "all") routes = {"shared", "shared-dst", "address", "cpu"};
        else routes = {o.route};

        for (auto& route : routes) {
            fflush(stdout);
            printReport(runRoute(route, sa, da, L, o), L);
        }
        return 0;
    } catch (const std::exception& e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}

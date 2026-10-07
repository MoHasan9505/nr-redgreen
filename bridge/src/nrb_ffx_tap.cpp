// nr-bridge [NRB9]: FFX tap - see nrb_ffx_tap.hpp.
#include "nrb_ffx_tap.hpp"

#include <windows.h>
#include <d3d12.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "diag.hpp"

// nr-bridge [NRB10]: the MVec lane's producer copy (gpu1_context.cpp). Records one CopyTextureRegion of
// the given resource into this frame's transport slot; inert unless MVec=3 and the stream is armed.
// Declared here rather than including gpu1_context.hpp, which this file needs nothing else from.
namespace mgpu::gpu1
{
void stream_mvec_copy(void *cmd_list, unsigned long long mvec_handle);
bool stream_depth_publish(unsigned long long depth_handle);   // nr-bridge NRB16
void stream_depth_copy(void *cmd_list, unsigned long long depth_handle);
}

namespace nrb::ffx
{
namespace
{
// ---- FidelityFX SDK 3.0.3 / 3.0.4 layouts (ffx_types.h, ffx_fsr3upscaler.h; MIT) ----
// Checked against the game's ffx_fsr3upscaler_x64.dll: its dispatch code reads every field from
// jitterOffset (0x6E8) to viewSpaceToMetersFactor (0x720) at these offsets.

struct FfxResource
{
    void *resource;            // ID3D12Resource* on the DX12 backend
    uint32_t type, format, width, height, depth, mip_count, flags, usage;   // FfxResourceDescription
    uint32_t state;            // FfxResourceStates
    wchar_t name[64];
};
static_assert(sizeof(FfxResource) == 176, "FfxResource layout");

struct FfxFsr3UpscalerDispatchDescription
{
    void *command_list;
    FfxResource color, depth, motion_vectors, exposure, reactive, transparency_and_composition;
    FfxResource dilated_depth, dilated_motion_vectors, reconstructed_prev_nearest_depth;
    FfxResource output;
    float jitter_x, jitter_y;
    float mv_scale_x, mv_scale_y;
    uint32_t render_w, render_h;
    bool enable_sharpening;
    float sharpness, frame_time_delta, pre_exposure;
    bool reset;
    float camera_near, camera_far, camera_fov_v, view_space_to_meters;
};
static_assert(offsetof(FfxFsr3UpscalerDispatchDescription, depth) == 184, "depth offset");
static_assert(offsetof(FfxFsr3UpscalerDispatchDescription, motion_vectors) == 360, "mv offset");
static_assert(offsetof(FfxFsr3UpscalerDispatchDescription, output) == 1592, "output offset");
static_assert(offsetof(FfxFsr3UpscalerDispatchDescription, jitter_x) == 0x6E8, "jitter offset");
static_assert(offsetof(FfxFsr3UpscalerDispatchDescription, render_w) == 0x6F8, "renderSize offset");
static_assert(offsetof(FfxFsr3UpscalerDispatchDescription, reset) == 0x710, "reset offset");
static_assert(offsetof(FfxFsr3UpscalerDispatchDescription, view_space_to_meters) == 0x720, "last field");

struct FfxFsr3UpscalerContextDescription
{
    uint32_t flags;
    uint32_t max_render_w, max_render_h;
    uint32_t display_w, display_h;
    // FfxInterface backendInterface and fpMessage follow; not read.
};

// ---- FidelityFX API (amd_fidelityfx_dx12.dll 1.0.1.41314, SDK 1.1.x ffx_api.h / ffx_upscale.h; MIT) ----

struct ffxApiHeader
{
    uint64_t type;
    ffxApiHeader *next;
};
struct FfxApiResource
{
    void *resource;
    uint32_t type, format, width, height, depth, mip_count, flags, usage;
    uint32_t state;
};
static_assert(sizeof(FfxApiResource) == 48, "FfxApiResource layout");

struct ffxCreateContextDescUpscale
{
    ffxApiHeader header;
    uint32_t flags;
    uint32_t max_render_w, max_render_h;
    uint32_t max_upscale_w, max_upscale_h;
};

struct ffxDispatchDescUpscale
{
    ffxApiHeader header;
    void *command_list;
    FfxApiResource color, depth, motion_vectors, exposure, reactive, transparency_and_composition, output;
    float jitter_x, jitter_y;
    float mv_scale_x, mv_scale_y;
    uint32_t render_w, render_h;
    uint32_t upscale_w, upscale_h;
    bool enable_sharpening;
    float sharpness, frame_time_delta, pre_exposure;
    bool reset;
    float camera_near, camera_far, camera_fov_v, view_space_to_meters;
    uint32_t flags;
};
static_assert(offsetof(ffxDispatchDescUpscale, depth) == 72, "api depth offset");
static_assert(offsetof(ffxDispatchDescUpscale, motion_vectors) == 120, "api mv offset");
static_assert(offsetof(ffxDispatchDescUpscale, jitter_x) == 360, "api jitter offset");
static_assert(offsetof(ffxDispatchDescUpscale, render_w) == 376, "api renderSize offset");
static_assert(offsetof(ffxDispatchDescUpscale, reset) == 408, "api reset offset");
static_assert(offsetof(ffxDispatchDescUpscale, flags) == 428, "api flags offset");

constexpr uint64_t kApiCreateUpscale = 0x00010000u;
constexpr uint64_t kApiDispatchUpscale = 0x00010001u;

enum : uint32_t
{
    kHdr = 1u << 0,
    kDisplayResMv = 1u << 1,
    kMvJitterCancel = 1u << 2,
    kDepthInverted = 1u << 3,
    kDepthInfinite = 1u << 4,
    kAutoExposure = 1u << 5,
    kDynamicRes = 1u << 6,
};

using FfxErrorCode = int32_t;
using PfnCreate = FfxErrorCode (*)(void *ctx, const FfxFsr3UpscalerContextDescription *desc);
using PfnDispatch = FfxErrorCode (*)(void *ctx, const FfxFsr3UpscalerDispatchDescription *desc);
using PfnDestroy = FfxErrorCode (*)(void *ctx);

constexpr const wchar_t *kUpscalerModule = L"ffx_fsr3upscaler_x64.dll";
constexpr const wchar_t *kApiModule = L"amd_fidelityfx_dx12.dll";

using PfnApiCreate = uint32_t (*)(void **ctx, ffxApiHeader *desc, const void *mem_cb);
using PfnApiDispatch = uint32_t (*)(void **ctx, const ffxApiHeader *desc);
using PfnApiDestroy = uint32_t (*)(void **ctx, const void *mem_cb);
HMODULE g_api = nullptr;
bool g_api_tried = false;
PfnApiCreate g_tramp_api_create = nullptr;
PfnApiDispatch g_tramp_api_dispatch = nullptr;
PfnApiDestroy g_tramp_api_destroy = nullptr;

HMODULE g_upscaler = nullptr;
PfnCreate g_real_create = nullptr;
PfnDispatch g_real_dispatch = nullptr;
PfnDestroy g_real_destroy = nullptr;

std::atomic<uint64_t> g_polls{0};
std::atomic<int> g_enabled{-1};   // -1 not read yet
std::atomic<unsigned> g_slots{0};

// Create flags per context. A game has one or two upscaler contexts at a time.
struct ctx_entry
{
    void *ctx;
    uint32_t flags, max_w, max_h, disp_w, disp_h;
};
constexpr int kMaxContexts = 8;
ctx_entry g_contexts[kMaxContexts] = {};
std::mutex g_mu;   // guards g_contexts, g_latest, g_log_*
upscale_inputs g_latest;
bool g_have_latest = false;
uint64_t g_dispatches = 0;
uint64_t g_last_log_qpc = 0, g_qpf = 0;
uint64_t g_dispatches_at_last_log = 0;

void info(const char *s) { mgpu::diag::info(s); }

uint32_t flags_for(void *ctx)
{
    for (const auto &e : g_contexts) if (e.ctx == ctx) return e.flags;
    return 0xFFFFFFFFu;
}

const char *state_name(uint32_t s)
{
    switch (s)
    {
    case 1u << 0: return "COMMON";
    case 1u << 1: return "UAV";
    case 1u << 2: return "COMPUTE_READ";
    case 1u << 3: return "PIXEL_READ";
    case (1u << 3) | (1u << 2): return "PIXEL_COMPUTE_READ";
    case 1u << 4: return "COPY_SRC";
    case (1u << 4) | (1u << 2): return "GENERIC_READ";
    case 1u << 8: return "RENDER_TARGET";
    default: return "?";
    }
}

// Size and DXGI format straight from the resource, so the log does not depend on the FFX enum.
void d3d_desc(void *res, char *out, size_t n)
{
    if (res == nullptr) { snprintf(out, n, "null"); return; }
    const D3D12_RESOURCE_DESC d = static_cast<ID3D12Resource *>(res)->GetDesc();
    snprintf(out, n, "%p %llux%u dxgi=%u", res, (unsigned long long)d.Width, d.Height, (unsigned)d.Format);
}

// ---- NRB10: motion vectors into the MVec lane ----
//
// Phase 4.3a. Inside the upscale dispatch, before calling through, the motion-vector resource holds this
// frame's final vectors. Copy it into the transport the way the calibrator's evaluate route does for a
// DLSS game (calibrator.cpp hook_evaluate): transition to COPY_SOURCE on the game's own command list,
// hand it to stream_mvec_copy, transition back to the state FSR declared. Once per present: the copy
// must land between this frame's finish_effects and the next (porting notes 1.6). FfxMvec=0 turns it off.
std::atomic<int> g_mvec_copy{1};
std::atomic<uint64_t> g_last_copy_present{~0ull};
std::atomic<uint64_t> g_mv_copies{0}, g_mv_skips_state{0};

D3D12_RESOURCE_STATES d3d_state(uint32_t ffx)
{
    D3D12_RESOURCE_STATES r = D3D12_RESOURCE_STATE_COMMON;
    if (ffx & (1u << 1)) r |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (ffx & (1u << 2)) r |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (ffx & (1u << 3)) r |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (ffx & (1u << 4)) r |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (ffx & (1u << 5)) r |= D3D12_RESOURCE_STATE_COPY_DEST;
    if (ffx & (1u << 8)) r |= D3D12_RESOURCE_STATE_RENDER_TARGET;
    return r;
}

void copy_motion_vectors(void *cmd_list, void *mv, uint32_t ffx_state)
{
    if (cmd_list == nullptr || mv == nullptr || g_mvec_copy.load(std::memory_order_relaxed) == 0) return;
    const uint64_t present = g_polls.load(std::memory_order_relaxed);
    if (g_last_copy_present.exchange(present, std::memory_order_relaxed) == present) return;

    auto *cl = static_cast<ID3D12GraphicsCommandList *>(cmd_list);
    auto *res = static_cast<ID3D12Resource *>(mv);
    const D3D12_RESOURCE_STATES before = d3d_state(ffx_state);
    // COMMON (0) or UAV would need a different barrier pairing than this lane has been validated with;
    // FSR declares its inputs readable (run13/run15: PIXEL_COMPUTE_READ). Anything else: skip and count.
    if ((before & (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                   D3D12_RESOURCE_STATE_COPY_SOURCE)) == 0 ||
        (before & (D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_RENDER_TARGET |
                   D3D12_RESOURCE_STATE_COPY_DEST)) != 0)
    {
        g_mv_skips_state.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool need_barrier = (before & D3D12_RESOURCE_STATE_COPY_SOURCE) == 0;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (need_barrier)
    {
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &b);
    }
    mgpu::gpu1::stream_mvec_copy(cmd_list, (unsigned long long)(uintptr_t)mv);
    if (need_barrier)
    {
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = before;
        cl->ResourceBarrier(1, &b);
    }
    g_mv_copies.fetch_add(1, std::memory_order_relaxed);
}

// nr-bridge NRB16: Depth=3. FSR's depth input, copied here for the same reason as the motion vectors:
// the state is the one FSR declares, and the buffer holds this frame's depth at this point. Once per
// present. FfxDepth=0 turns it off.
std::atomic<int> g_depth_copy{1};
std::atomic<uint64_t> g_last_depth_present{~0ull};
std::atomic<uint64_t> g_depth_copies{0}, g_depth_skips_state{0};

void copy_depth(void *cmd_list, void *depth, uint32_t ffx_state)
{
    if (cmd_list == nullptr || depth == nullptr || g_depth_copy.load(std::memory_order_relaxed) == 0) return;
    if (!mgpu::gpu1::stream_depth_publish((unsigned long long)(uintptr_t)depth)) return;
    const uint64_t present = g_polls.load(std::memory_order_relaxed);
    if (g_last_depth_present.exchange(present, std::memory_order_relaxed) == present) return;

    auto *cl = static_cast<ID3D12GraphicsCommandList *>(cmd_list);
    auto *res = static_cast<ID3D12Resource *>(depth);
    const D3D12_RESOURCE_STATES before = d3d_state(ffx_state);
    if ((before & (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                   D3D12_RESOURCE_STATE_COPY_SOURCE)) == 0 ||
        (before & (D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_RENDER_TARGET |
                   D3D12_RESOURCE_STATE_COPY_DEST)) != 0)
    {
        g_depth_skips_state.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool need_barrier = (before & D3D12_RESOURCE_STATE_COPY_SOURCE) == 0;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;   // both planes of a depth-stencil
    if (need_barrier)
    {
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &b);
    }
    mgpu::gpu1::stream_depth_copy(cmd_list, (unsigned long long)(uintptr_t)depth);
    if (need_barrier)
    {
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = before;
        cl->ResourceBarrier(1, &b);
    }
    g_depth_copies.fetch_add(1, std::memory_order_relaxed);
}

// ---- shared recording ----

void remember_context(void *ctx, uint32_t flags, uint32_t mw, uint32_t mh, uint32_t dw, uint32_t dh)
{
    std::lock_guard<std::mutex> lock(g_mu);
    ctx_entry *slot = nullptr;
    for (auto &e : g_contexts) if (e.ctx == ctx || (!slot && e.ctx == nullptr)) slot = &e;
    if (slot) *slot = {ctx, flags, mw, mh, dw, dh};
}

void forget_context(void *ctx)
{
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto &e : g_contexts) if (e.ctx == ctx) e = {};
    if (g_have_latest && g_latest.context == ctx) g_have_latest = false;
}

void log_create(const char *api, void *ctx, int r, uint32_t f, uint32_t mw, uint32_t mh, uint32_t dw, uint32_t dh)
{
    char line[400];
    snprintf(line, sizeof line,
             "[MGPU][NRB9] %s upscale context %p created r=%d flags=0x%X (%s%s%s%s%s%s%s) max render %ux%u, "
             "output %ux%u",
             api, ctx, r, f, (f & kHdr) ? "HDR " : "", (f & kDisplayResMv) ? "display-res-MV " : "",
             (f & kMvJitterCancel) ? "MV-jitter-cancel " : "", (f & kDepthInverted) ? "depth-inverted " : "",
             (f & kDepthInfinite) ? "depth-infinite " : "", (f & kAutoExposure) ? "auto-exposure " : "",
             (f & kDynamicRes) ? "dynamic-res" : "", mw, mh, dw, dh);
    info(line);
}

// One upscale dispatch, from either API. Publishes it for latest() and logs once per second.
void record(const char *api, upscale_inputs u, float cam_near, float cam_far, float fov_v, float pre_exposure)
{
    bool log_now = false;
    uint64_t per_s = 0;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        u.create_flags = flags_for(u.context);
        u.frame = ++g_dispatches;
        g_latest = u;
        g_have_latest = true;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (g_qpf == 0)
        {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            g_qpf = (uint64_t)f.QuadPart;
        }
        if ((uint64_t)now.QuadPart - g_last_log_qpc >= g_qpf)
        {
            log_now = true;
            g_last_log_qpc = (uint64_t)now.QuadPart;
            per_s = u.frame - g_dispatches_at_last_log;
            g_dispatches_at_last_log = u.frame;
        }
    }
    if (!log_now) return;
    char c[96], dp[96], mv[96], out[96], line[1100];
    d3d_desc(u.color, c, sizeof c);
    d3d_desc(u.depth, dp, sizeof dp);
    d3d_desc(u.motion_vectors, mv, sizeof mv);
    d3d_desc(u.output, out, sizeof out);
    snprintf(line, sizeof line,
             "[MGPU][NRB9] %s upscale #%llu (%llu in the last second) ctx=%p flags=0x%X render %ux%u | color %s | "
             "depth %s %s | mv %s %s | output %s | jitter (%.4f, %.4f) mv scale (%.2f, %.2f) reset=%d near=%.3f "
             "far=%.1f fovV=%.3f preExposure=%.3f | inferred flags 0x%X | MV copies %llu (state skips %llu) | "
             "depth copies %llu (state skips %llu)",
             api, (unsigned long long)u.frame, (unsigned long long)per_s, u.context, u.create_flags, u.render_w,
             u.render_h, c, dp, state_name(u.depth_state), mv, state_name(u.mv_state), out, u.jitter_x, u.jitter_y,
             u.mv_scale_x, u.mv_scale_y, (int)u.reset, cam_near, cam_far, fov_v, pre_exposure, u.inferred_flags,
             (unsigned long long)g_mv_copies.load(std::memory_order_relaxed),
             (unsigned long long)g_mv_skips_state.load(std::memory_order_relaxed),
             (unsigned long long)g_depth_copies.load(std::memory_order_relaxed),
             (unsigned long long)g_depth_skips_state.load(std::memory_order_relaxed));
    info(line);
}

template <class R>
void fill(upscale_inputs &u, void *ctx, const R &d)
{
    u.context = ctx;
    u.command_list = d.command_list;
    u.color = d.color.resource;
    u.depth = d.depth.resource;
    u.motion_vectors = d.motion_vectors.resource;
    u.output = d.output.resource;
    u.depth_state = d.depth.state;
    u.mv_state = d.motion_vectors.state;
    u.render_w = d.render_w;
    u.render_h = d.render_h;
    u.mv_w = d.motion_vectors.width;
    u.mv_h = d.motion_vectors.height;
    u.jitter_x = d.jitter_x;
    u.jitter_y = d.jitter_y;
    u.mv_scale_x = d.mv_scale_x;
    u.mv_scale_y = d.mv_scale_y;
    u.reset = d.reset;
}

// ---- FSR 3.0 hooks (import slots) ----

FfxErrorCode hook_create(void *ctx, const FfxFsr3UpscalerContextDescription *desc)
{
    const FfxErrorCode r = g_real_create(ctx, desc);
    if (desc == nullptr) return r;
    if (r == 0) remember_context(ctx, desc->flags, desc->max_render_w, desc->max_render_h, desc->display_w, desc->display_h);
    log_create("FSR3.0", ctx, (int)r, desc->flags, desc->max_render_w, desc->max_render_h, desc->display_w, desc->display_h);
    return r;
}

FfxErrorCode hook_destroy(void *ctx)
{
    forget_context(ctx);
    char line[128];
    snprintf(line, sizeof line, "[MGPU][NRB9] FSR3.0 upscale context %p destroyed", ctx);
    info(line);
    return g_real_destroy(ctx);
}

FfxErrorCode hook_dispatch(void *ctx, const FfxFsr3UpscalerDispatchDescription *d)
{
    if (d != nullptr)
    {
        upscale_inputs u;
        fill(u, ctx, *d);
        record("FSR3.0", u, d->camera_near, d->camera_far, d->camera_fov_v, d->pre_exposure);
        copy_motion_vectors(d->command_list, d->motion_vectors.resource, d->motion_vectors.state);
        copy_depth(d->command_list, d->depth.resource, d->depth.state);   // NRB16
    }
    return g_real_dispatch(ctx, d);
}

// ---- FFX API hooks (inline, amd_fidelityfx_dx12.dll) ----

const ffxApiHeader *find_desc(const ffxApiHeader *h, uint64_t type)
{
    for (; h != nullptr; h = h->next) if (h->type == type) return h;
    return nullptr;
}

uint32_t hook_api_create(void **ctx, ffxApiHeader *desc, const void *mem_cb)
{
    const uint32_t r = g_tramp_api_create(ctx, desc, mem_cb);
    const auto *up = reinterpret_cast<const ffxCreateContextDescUpscale *>(find_desc(desc, kApiCreateUpscale));
    if (up != nullptr && ctx != nullptr)
    {
        if (r == 0) remember_context(*ctx, up->flags, up->max_render_w, up->max_render_h, up->max_upscale_w, up->max_upscale_h);
        log_create("FFX API", *ctx, (int)r, up->flags, up->max_render_w, up->max_render_h, up->max_upscale_w,
                   up->max_upscale_h);
    }
    return r;
}

uint32_t hook_api_destroy(void **ctx, const void *mem_cb)
{
    if (ctx != nullptr)
    {
        bool ours = false;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            for (const auto &e : g_contexts) if (e.ctx == *ctx) ours = true;
        }
        if (ours)
        {
            forget_context(*ctx);
            char line[128];
            snprintf(line, sizeof line, "[MGPU][NRB9] FFX API upscale context %p destroyed", *ctx);
            info(line);
        }
    }
    return g_tramp_api_destroy(ctx, mem_cb);
}

uint32_t hook_api_dispatch(void **ctx, const ffxApiHeader *desc)
{
    if (ctx != nullptr && desc != nullptr && desc->type == kApiDispatchUpscale)
    {
        const auto &d = *reinterpret_cast<const ffxDispatchDescUpscale *>(desc);
        upscale_inputs u;
        fill(u, *ctx, d);
        // The tap starts from presents, usually after the game created this context, so the create
        // flags are not known. Infer the two that matter downstream from the dispatch itself.
        u.inferred_flags = (d.camera_near > d.camera_far ? kDepthInverted : 0u) |
                           (d.motion_vectors.width > d.render_w ? kDisplayResMv : 0u);
        record("FFX API", u, d.camera_near, d.camera_far, d.camera_fov_v, d.pre_exposure);
        copy_motion_vectors(d.command_list, d.motion_vectors.resource, d.motion_vectors.state);
        copy_depth(d.command_list, d.depth.resource, d.depth.state);   // NRB16
    }
    return g_tramp_api_dispatch(ctx, desc);
}

// ---- inline hooks (FFX API) ----
//
// Each hooked export's first `n` bytes must equal the prologue of the exact amd_fidelityfx_dx12.dll the
// game ships (1.0.1.41314, read 2026-10-06). Those bytes are position-independent, so they run unchanged
// from a trampoline. The entry is overwritten with `mov rax, imm64; jmp rax` (rax is not an argument
// register) and int3 padding.
//
// Writing code another thread may be executing is the dangerous part (run14 crashed after a related
// bug). So every write happens with every other thread of the process suspended, and is retried while
// any of them has its instruction pointer inside the bytes being replaced. Nothing is allocated or logged
// while threads are suspended: a suspended thread may hold the heap lock.

struct inline_site
{
    const char *name;
    uint8_t *target = nullptr;
    size_t n = 0;
    uint8_t original[16] = {};
    uint8_t patch[16] = {};
    uint8_t *tramp = nullptr;
    bool live = false;
};
inline_site g_sites[3];

constexpr int kMaxThreads = 2048;

// Suspends every other thread. Returns how many were suspended into `held`; sets `busy` when one of
// them is executing inside any site's first n bytes.
int suspend_others(HANDLE *held, bool &busy)
{
    busy = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    int count = 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok && count < kMaxThreads; ok = Thread32Next(snap, &te))
    {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        if (t == nullptr) continue;
        if (SuspendThread(t) == (DWORD)-1)
        {
            CloseHandle(t);
            continue;
        }
        held[count++] = t;
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(t, &c))
            for (const auto &site : g_sites)
                if (site.target && c.Rip >= (DWORD64)site.target && c.Rip < (DWORD64)(site.target + site.n)) busy = true;
    }
    CloseHandle(snap);
    return count;
}

void resume_all(HANDLE *held, int count)
{
    for (int i = 0; i < count; ++i)
    {
        ResumeThread(held[i]);
        CloseHandle(held[i]);
    }
}

// Writes `install ? patch : original` into every prepared site, with all other threads suspended.
bool write_sites(bool install)
{
    static HANDLE held[kMaxThreads];   // static: no allocation while threads are suspended
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        bool busy = false;
        const int count = suspend_others(held, busy);
        if (count < 0) return false;
        if (!busy)
        {
            for (auto &site : g_sites)
            {
                if (!site.target || !site.tramp || site.live == install) continue;
                DWORD old = 0;
                if (!VirtualProtect(site.target, site.n, PAGE_EXECUTE_READWRITE, &old)) continue;
                memcpy(site.target, install ? site.patch : site.original, site.n);
                VirtualProtect(site.target, site.n, old, &old);
                FlushInstructionCache(GetCurrentProcess(), site.target, site.n);
                site.live = install;
            }
            resume_all(held, count);
            return true;
        }
        resume_all(held, count);
        Sleep(1);
    }
    return false;
}

// Checks the prologue and builds the trampoline and the patch bytes. Writes nothing into the DLL.
bool prepare_site(inline_site &site, HMODULE mod, const char *name, const uint8_t *expect, size_t n, void *hook)
{
    site.name = name;
    uint8_t *target = reinterpret_cast<uint8_t *>(GetProcAddress(mod, name));
    char line[256];
    if (target == nullptr || n < 12 || n > sizeof site.original || memcmp(target, expect, n) != 0)
    {
        snprintf(line, sizeof line,
                 "[MGPU][NRB9] FFX API %s: prologue differs from the known build - not hooked (this "
                 "amd_fidelityfx_dx12.dll is not 1.0.1.41314?)", name);
        info(line);
        return false;
    }
    // Trampoline: the n original bytes, then jmp qword ptr [rip+0] back to target+n. Never freed: a
    // thread may be inside it when the hooks come off.
    uint8_t *tramp = static_cast<uint8_t *>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (tramp == nullptr) return false;
    memcpy(tramp, target, n);
    uint8_t *j = tramp + n;
    j[0] = 0xFF; j[1] = 0x25; j[2] = j[3] = j[4] = j[5] = 0;
    const uint64_t back = reinterpret_cast<uint64_t>(target + n);
    memcpy(j + 6, &back, 8);
    FlushInstructionCache(GetCurrentProcess(), tramp, 64);

    site.target = target;
    site.n = n;
    site.tramp = tramp;
    memcpy(site.original, target, n);
    site.patch[0] = 0x48; site.patch[1] = 0xB8;
    const uint64_t to = reinterpret_cast<uint64_t>(hook);
    memcpy(site.patch + 2, &to, 8);
    site.patch[10] = 0xFF; site.patch[11] = 0xE0;
    for (size_t i = 12; i < n; ++i) site.patch[i] = 0xCC;
    return true;
}

void install_api_hooks(HMODULE m)
{
    static const uint8_t destroy_prologue[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20,
                                               0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9};
    static const uint8_t create_prologue[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C,
                                              0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20};
    static const uint8_t dispatch_prologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48,
                                                0x83, 0xEC, 0x20, 0x48, 0x8B, 0xFA};
    if (prepare_site(g_sites[0], m, "ffxDestroyContext", destroy_prologue, sizeof destroy_prologue, (void *)&hook_api_destroy))
        g_tramp_api_destroy = reinterpret_cast<PfnApiDestroy>(g_sites[0].tramp);
    if (prepare_site(g_sites[1], m, "ffxCreateContext", create_prologue, sizeof create_prologue, (void *)&hook_api_create))
        g_tramp_api_create = reinterpret_cast<PfnApiCreate>(g_sites[1].tramp);
    if (prepare_site(g_sites[2], m, "ffxDispatch", dispatch_prologue, sizeof dispatch_prologue, (void *)&hook_api_dispatch))
        g_tramp_api_dispatch = reinterpret_cast<PfnApiDispatch>(g_sites[2].tramp);

    const bool ok = write_sites(true);
    char line[256];
    snprintf(line, sizeof line, "[MGPU][NRB9] FFX API hooks %s: ffxDestroyContext=%d ffxCreateContext=%d ffxDispatch=%d",
             ok ? "installed" : "NOT installed (a thread stayed inside a prologue)", (int)g_sites[0].live,
             (int)g_sites[1].live, (int)g_sites[2].live);
    info(line);
}

// ---- import patching (value compare, as calibrator.cpp patch_module) ----

unsigned patch_module(HMODULE mod, void *find, void *repl)
{
    if (mod == nullptr || find == nullptr) return 0;
    BYTE *base = (BYTE *)mod;
    volatile unsigned hits = 0;
    __try
    {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (dir.VirtualAddress == 0 || dir.Size == 0) return 0;
        for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name != 0;
             ++imp)
        {
            if (imp->FirstThunk == 0) continue;
            for (IMAGE_THUNK_DATA *t = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk); t->u1.Function != 0; ++t)
            {
                if ((void *)(uintptr_t)t->u1.Function != find) continue;
                DWORD old = 0;
                if (!VirtualProtect(&t->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) continue;
                t->u1.Function = (ULONGLONG)(uintptr_t)repl;
                VirtualProtect(&t->u1.Function, sizeof(void *), old, &old);
                ++hits;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return (unsigned)hits;
    }
    return (unsigned)hits;
}

HMODULE self_module()
{
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&self_module), &m);
    return m;
}

// Patches every module in the process (except the upscaler itself and us). Returns new slots.
unsigned scan_and_patch()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return 0;
    const HMODULE self = self_module();
    unsigned hits = 0;
    MODULEENTRY32W me{};
    me.dwSize = sizeof me;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
    {
        if (me.hModule == g_upscaler || me.hModule == self) continue;
        const unsigned h = patch_module(me.hModule, (void *)g_real_dispatch, (void *)&hook_dispatch) +
                           patch_module(me.hModule, (void *)g_real_create, (void *)&hook_create) +
                           patch_module(me.hModule, (void *)g_real_destroy, (void *)&hook_destroy);
        if (h != 0)
        {
            char line[MAX_PATH + 96];
            snprintf(line, sizeof line, "[MGPU][NRB9] FFX tap: patched %u import slot(s) in %ls", h, me.szModule);
            info(line);
            hits += h;
        }
    }
    CloseHandle(snap);
    return hits;
}

// FfxTap=0 in the mgpu.ini beside the add-on turns the tap off, FfxMvec=0 only the motion-vector copy.
// Absent or anything else: on.
bool read_enabled()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(self_module(), path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return true;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash == nullptr) return true;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"mgpu.ini");
    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return true;
    char line[512];
    bool on = true;
    while (fgets(line, sizeof line, f))
    {
        const char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (strncmp(p, "FfxTap=", 7) == 0) on = p[7] != '0';
        if (strncmp(p, "FfxMvec=", 8) == 0) g_mvec_copy.store(p[8] != '0' ? 1 : 0, std::memory_order_relaxed);
        if (strncmp(p, "FfxDepth=", 9) == 0) g_depth_copy.store(p[9] != '0' ? 1 : 0, std::memory_order_relaxed);
    }
    fclose(f);
    return on;
}
}  // namespace

void poll()
{
    // Every 240 presents (~2-4 s): FSR's DLLs can load late, when FSR is switched on.
    if (g_polls.fetch_add(1, std::memory_order_relaxed) % 240 != 0) return;
    poll_now();
}

void poll_now()
{

    int en = g_enabled.load(std::memory_order_relaxed);
    if (en < 0)
    {
        en = read_enabled() ? 1 : 0;
        g_enabled.store(en, std::memory_order_relaxed);
        info(en ? "[MGPU][NRB9] FFX tap ON (FfxTap=0 in mgpu.ini turns it off): watching for amd_fidelityfx_dx12.dll "
                   "and ffx_fsr3upscaler_x64.dll"
                : "[MGPU][NRB9] FFX tap OFF (FfxTap=0)");
        if (en)
            info(g_mvec_copy.load() ? "[MGPU][NRB10] FSR motion vectors -> MVec lane ON (needs MVec=3; FfxMvec=0 turns it off)"
                                    : "[MGPU][NRB10] FSR motion vectors -> MVec lane OFF (FfxMvec=0)");
    }
    if (en == 0) return;

    // FSR 4: on RDNA 4 the AMD driver can serve the FidelityFX API itself (amdxcffx64.dll from the
    // driver store) for games that use FSR 3.1. Logged once, so a run shows which library is live;
    // the inline hooks above only accept the game's own amd_fidelityfx_dx12.dll 1.0.1.41314.
    static bool driver_ffx_logged = false;
    if (!driver_ffx_logged && GetModuleHandleW(L"amdxcffx64.dll") != nullptr)
    {
        driver_ffx_logged = true;
        info("[MGPU][NRB9] amdxcffx64.dll (AMD driver FidelityFX / FSR 4 provider) is loaded in the game");
    }

    if (!g_api_tried)
    {
        HMODULE m = GetModuleHandleW(kApiModule);
        if (m != nullptr)
        {
            g_api_tried = true;
            g_api = m;
            info("[MGPU][NRB9] amd_fidelityfx_dx12.dll found - hooking ffxCreateContext/ffxDispatch/ffxDestroyContext");
            install_api_hooks(m);
        }
    }

    if (g_upscaler == nullptr)
    {
        HMODULE m = GetModuleHandleW(kUpscalerModule);
        if (m == nullptr) return;   // FSR 3.0 DLLs not loaded (yet)
        g_real_create = reinterpret_cast<PfnCreate>(GetProcAddress(m, "ffxFsr3UpscalerContextCreate"));
        g_real_dispatch = reinterpret_cast<PfnDispatch>(GetProcAddress(m, "ffxFsr3UpscalerContextDispatch"));
        g_real_destroy = reinterpret_cast<PfnDestroy>(GetProcAddress(m, "ffxFsr3UpscalerContextDestroy"));
        if (!g_real_create || !g_real_dispatch || !g_real_destroy)
        {
            info("[MGPU][NRB9] ffx_fsr3upscaler_x64.dll is loaded but lacks the FSR 3.0 exports - FSR 3.0 path not tapped");
            g_real_create = nullptr; g_real_dispatch = nullptr; g_real_destroy = nullptr;
            g_upscaler = reinterpret_cast<HMODULE>(1);   // do not look again
            return;
        }
        g_upscaler = m;
        // Pin it: our hooks call into it, and the game may unload it when FSR is switched off.
        HMODULE pin = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, kUpscalerModule, &pin);
        info("[MGPU][NRB9] ffx_fsr3upscaler_x64.dll found - patching importers of its Create/Dispatch/Destroy");
    }
    if (g_real_dispatch == nullptr) return;
    const unsigned h = scan_and_patch();
    if (h != 0) g_slots.fetch_add(h, std::memory_order_relaxed);
}

bool latest(upscale_inputs &out)
{
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_have_latest) return false;
    out = g_latest;
    return true;
}

void shutdown()
{
    // Import slots first (FSR 3.0): put the real pointers back wherever ours are.
    if (g_real_dispatch != nullptr)
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snap != INVALID_HANDLE_VALUE)
        {
            MODULEENTRY32W me{};
            me.dwSize = sizeof me;
            for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
            {
                patch_module(me.hModule, (void *)&hook_dispatch, (void *)g_real_dispatch);
                patch_module(me.hModule, (void *)&hook_create, (void *)g_real_create);
                patch_module(me.hModule, (void *)&hook_destroy, (void *)g_real_destroy);
            }
            CloseHandle(snap);
        }
    }
    // Then the FFX API prologues. The trampolines stay allocated.
    write_sites(false);
}
}  // namespace nrb::ffx

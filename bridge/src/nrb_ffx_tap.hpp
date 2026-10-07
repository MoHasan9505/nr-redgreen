#pragma once

// nr-bridge [NRB9]: FFX tap (docs/test-log.md, steps 4.1-4.2).
//
// On an AMD render GPU the game upscales with FSR 3.0 (FidelityFX SDK 3.0.3/3.0.4 DLLs), not DLSS,
// so the depth and motion vectors DLSS-NR wants come from the FSR upscale dispatch. Every FSR 3
// upscale goes through ffxFsr3UpscalerContextDispatch in ffx_fsr3upscaler_x64.dll: ffx_fsr3_x64.dll
// imports it, and so would the game if it called the upscaler directly. This patches that import
// (and ContextCreate / ContextDestroy, for the per-context flags) in every loaded module, by value,
// the way the calibrator patches NGX imports.
//
// 4.2 OBSERVES ONLY: once per second it logs what FSR received (resources, sizes, formats, states,
// jitter, MV scale, reset, the context's create flags). Nothing in the stream reads it yet.
// FfxTap=0 in mgpu.ini disables it (no patching at all).

#include <cstdint>

namespace nrb::ffx
{
// From the game's present. Cheap: one counter compare per call; every ~2 s it looks for the
// FSR upscaler module and patches importers it has not patched yet.
void poll();

// The same check right now, regardless of the present count. NOT for init_device: ReShade loads and
// unloads the add-on once per startup device, and hooks installed by an instance that is then
// unloaded crashed the game (run14).
void poll_now();

// Removes every hook (import slots and FFX API prologues). From DllMain's FreeLibrary path, so nothing
// in the process points into this image after it unloads.
void shutdown();

// What the last upscale dispatch carried, for Phase 4.3. Resources are the game's
// ID3D12Resource pointers, valid only on the game's render thread during that frame.
struct upscale_inputs
{
    void *context = nullptr;
    void *command_list = nullptr;   // ID3D12GraphicsCommandList*
    void *color = nullptr, *depth = nullptr, *motion_vectors = nullptr, *output = nullptr;
    uint32_t depth_state = 0, mv_state = 0;   // FfxResourceStates
    uint32_t render_w = 0, render_h = 0;
    uint32_t mv_w = 0, mv_h = 0;
    float jitter_x = 0, jitter_y = 0;
    float mv_scale_x = 0, mv_scale_y = 0;
    bool reset = false;
    uint32_t create_flags = 0;   // FFX_FSR3UPSCALER_ENABLE_*; 0xFFFFFFFF if the context predates the tap
    uint32_t inferred_flags = 0; // FFX API only: depth-inverted (near > far), display-res MVs (MV wider than render)
    uint64_t frame = 0;          // dispatch counter
};
bool latest(upscale_inputs &out);
}  // namespace nrb::ffx

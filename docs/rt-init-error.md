# Cyberpunk "error during Ray Tracing initialization" on R9700 + RTX 5070

## Symptom

About 11-14 s after launch, Cyberpunk 2.31 shows:

> Cyberpunk 2077 encountered an error during Ray Tracing initialization and will now be
> forced to close.

The game renders on the R9700 (Windows per-app GPU preference `SpecificAdapter=1002&7551`)
and the RTX 5070 is installed and drives the monitor.

## Isolation (2026-10-06, tools/watch-launch.ps1, logs in results/)

| Run | ReShade | nr-bridge | nvapi-gate | Aftermath | Result |
|---|---|---|---|---|---|
| run1, run2 | on | on | - | original | RT error |
| run3 | on | **off** | - | original | RT error at +13 s |
| run4 | on | off | in game folder (never loaded) | original | RT error |
| run5 | **off** | off | off | original | RT error at +11 s (CrashInfo.json) |
| run7 | on | on | on | RTInitFix stub | **launches**, with RTInitFix `sl.interposer.dll` |

So the error is not caused by ReShade or nr-bridge: plain Cyberpunk fails as soon as an NVIDIA
card is in the machine. With the NVIDIA driver present, the game process loads `nvapi64.dll`
(from System32, so a copy in the game folder is never used), Streamline loads `_nvngx.dll`,
`nvngx_dlss/dlssg/dlssd.dll` and opens a D3D12 device on the 5070, and ray-tracing setup on
the AMD device fails.

## First fix: RTInitFix (Nexus mod 29089, "Dual-GPU AMD-NVIDIA RT init FIX", v1.0)

Inspected copy: `reference/rtinitfix/` (source in `dist/src`). Its `sl.interposer.dll` replaces
Streamline outright (the game imports 19 functions from it):

- forwards the DXGI / D3D11 / D3D12 entry points to System32;
- patches the DXGI factory vtable (`EnumAdapters`, `EnumAdapters1`, `EnumAdapterByGpuPreference`)
  to hide NVIDIA and WARP adapters - **process-wide**, which is why nr-bridge needed NRB5;
- patches `ID3D12Device::CheckFeatureSupport` to report `RaytracingTier = NOT_SUPPORTED` -
  **ray tracing is off in the game**, also process-wide;
- stubs every `sl*` function (`slInit` returns 4, no supported adapter).

Its `GFSDK_Aftermath_Lib.x64.dll` stubs NVIDIA Aftermath. Its `nvapi64.dll` stub is not used here:
nvapi-gate takes that name instead, and the game loads NVAPI from System32 anyway.

The prebuilt DLLs could not be checked against the source (the trampolines use GCC inline
assembly, and MinGW is not installed). `tools/deploy.ps1` installs them from `vendor/rtinitfix/`
and moves the game's originals into `bin\x64\_nr-bridge-backup\`.

## Fix in use now: sl-standin (`tools/sl-standin`, our own `sl.interposer.dll`)

RTInitFix had a side effect: ReShade never saw the game's swapchain. Its `sl.interposer.dll` loads
`System32\dxgi.dll` by full path (its own log, `vendor/rtinitfix/sl_proxy_debug.txt`), so the game's
DXGI calls skip ReShade's `dxgi.dll`. In run8 ReShade loaded, but no swapchain was ever created
through it in 3 minutes: no overlay, and the bridge never armed.

sl-standin exports the same 261 names as RTInitFix (checked against its export table) and:

- sends DXGI calls to the `dxgi.dll` beside it (ReShade) when there is one, else System32; D3D11 and
  D3D12 the same way;
- hides NVIDIA adapters from **game-side callers only**: modules in the game folder, except the
  bridge add-on (`*nvngx.dll*`) and the private snippet under `mgpu\`. NVIDIA's NGX, the driver and
  the bridge still see the 5070. WARP stays visible, as on an AMD-only machine;
- with `MaskDXR=1` (the shipped default) reports `RaytracingTier = NOT_SUPPORTED` to the game, as
  RTInitFix does. **Run10: with `MaskDXR=0` the RT-init error came back even with NVIDIA hidden from
  the game**, so hiding the adapter seemed not to be enough and ray tracing stayed off (until the NVAPI root cause, below);
- answers every `sl*` call with RTInitFix's values (`slInit` returns 4: Streamline off);
- forwards the 229 `vk*` names to `vulkan-1.dll`;
- logs every factory and device creation to `sl-standin.log` beside it.

`tools/deploy.ps1` installs it by default. `tools/deploy.ps1 -UseRTInitFix` installs RTInitFix's DLL
instead, for an A/B. RTInitFix's Aftermath stub is still used.

Run11 findings (2026-10-06): ReShade now sees the swapchain (banner, overlay, bridge reached
`init_swapchain`). The RT-init error remained with `MaskDXR=1`, because the game creates its real D3D12
device by calling d3d12.dll directly, not through `sl.interposer.dll`, so the DXR patch was never
applied. sl-standin now creates one short-lived device on the first adapter the game probes and patches
the shared device vtable through it, and also hides WARP (`HideWarp=1`), as RTInitFix did.
The bridge then refused to pick a neural GPU: DXGI listed the 5070 twice (NRB8 in
`docs/bridge-changes.md`).

Since run12 the game starts every time with sl-standin, and every in-game run since (13-32) used it.

## Root cause and fix: NVAPI (2026-10-07)

Ray tracing was forced off (`MaskDXR=1`) from run10 until 2026-10-07. The cause turned out to be **NVAPI**,
NVIDIA's driver interface:

- `nvapi-gate` (our stand-in `nvapi64.dll` in `bin\x64`) is meant to refuse NVAPI to the game, but it never
  loaded: the process held only `System32\nvapi64.dll` (+ the driver's `nvapi64_impl.dll`), loaded before
  the game's own by-name request. Once a module named `nvapi64.dll` is loaded, Windows hands that one to every
  later by-name load. No `nvapi-gate.log` was ever written.
- So the game could still detect the NVIDIA card through NVAPI, and with ray tracing allowed it failed
  "Ray Tracing initialization" on the AMD card. A user report on the RTInitFix page (9070 XT + RTX 3070)
  pointed the same way: the mod's `nvapi64.dll` alone fixed it with ray tracing on.

**Fix:** sl-standin, which the exe imports and so loads before nearly everything else, now loads the gate from
`bin\x64` first (`PreloadNvapiGate=1`). Every later by-name `nvapi64.dll` load gets the gate, which refuses the
game and passes NVIDIA's own components, the bridge and the DLSS-NR snippet through. With that, `MaskDXR=0`
starts and ray tracing runs on the R9700 with DLSS 5 on the 5070.

One side effect: with NVAPI refused, the game created its device on the R9700's **second** DXGI entry, the one
NRB8 drops as a duplicate, and the bridge refused to select a GPU. NRB21 matches the game through dropped
duplicates (`docs/bridge-changes.md`).

`MaskDXR=0` is now the shipped default. `MaskDXR=1` with `PreloadNvapiGate=0` restores the old, ray-tracing-off
behaviour.

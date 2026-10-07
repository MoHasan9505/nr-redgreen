# Test system

Every result in this repository was measured on this one PC (October 2026).

## Hardware

| Part | Details |
|---|---|
| Base system | Mini PC, AMD Ryzen 7 7840HS (Radeon 780M integrated graphics, unused) |
| Render GPU | AMD Radeon AI PRO R9700, 32 GB, PCIe 4.0 x4 link to the host |
| Neural GPU | NVIDIA GeForce RTX 5070, 12 GB, PCIe 4.0 x4; drives the monitor |
| PCIe throughput | 5.5-6.5 GB/s measured per direction on each link |
| Display | One monitor at 2560x1440, on the RTX 5070 |

## Software

| Component | Version |
|---|---|
| AMD driver | Adrenalin 32.0.31041.1004 |
| NVIDIA driver | 617.14 (32.0.16.1714) |
| Cyberpunk 2077 | 2.31 (Steam) |
| ReShade | 6.8.0.2155 with add-on support |
| DLSS 5 neural-rendering runtime | `nvngx_dlssnr.dll` 310.8.0.0 (supplied separately, see `vendor/README.md`) |

The NVIDIA driver provides the NGX core (`_nvngx.dll`) but not the DLSS 5 neural-rendering runtime, which is why
it has to be supplied. `tools/deploy.ps1` installs it in `bin\x64\mgpu\` rather than beside the game exe, where
the game's own Streamline would load it and bind it to the wrong GPU.

## What the game ships

- **Two FSR stacks, both as separate DLLs**, which is what makes them hookable:
  - the FidelityFX API (`amd_fidelityfx_dx12.dll` 1.0.1.41314), used when **FSR 4** is selected;
  - FSR 3.0-style DLLs (`ffx_fsr3upscaler_x64.dll` and friends), used when **FSR 3.0** is selected.

  The FFX tap (NRB9) hooks both, so either option works.
- Streamline 2.7.1, DLSS Super Resolution 310.9.1 and XeSS 2.0.1. These go unused on the AMD card;
  sl-standin keeps Streamline off.

## D3D12 cross-adapter capabilities

| Adapter | Row-major cross-adapter textures | Resource heap tier | Shared resource tier |
|---|---|---|---|
| RTX 5070 | no | 2 | 2 |
| R9700 | no | 2 | 2 |

Neither card supports row-major cross-adapter textures, so frames cross between them in shared **buffers**,
copied to and from textures on each side. MGPU Bridge and `xadapter-probe` both work this way.

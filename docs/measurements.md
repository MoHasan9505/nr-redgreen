# Measurements and project history

The standalone probes that came before the in-game bridge, what they measured, and the phases the project went
through. For the in-game test runs see [`test-log.md`](test-log.md); for the hardware see
[`test-system.md`](test-system.md).

## Phases

0. **Hardware baseline:** RTX 5070 installed, NVIDIA driver 617.14, monitor on the 5070. *Done.*
1. **Transport test** (`xadapter-probe`): cross-adapter heap + fence, AMD → NVIDIA. *Passed.*
2. **NGX bring-up** (`nr-probe`): DLSS 5 NR on the 5070 in a process that also holds an AMD device. *Passed.*
3. **In game** (`bridge/`), colour only. *Working, run 12.*
4. **FSR motion vectors and depth**, then transport, pipeline, latency and pacing work. *Done, runs 13-32.*
5. **Next:** see the roadmap in the [README](../README.md#roadmap).

## DLSS 5 NR on the RTX 5070 (`nr-probe`)

Still image, 120 evaluates per size.

| Size | CreateFeature | GPU ms avg | p95 |
|---|---|---|---|
| 1920x1080 | 285 ms | 7.11 | 8.26 |
| 2560x1440 | 120 ms | 11.37 | 12.20 |
| 2880x1800 | 122 ms | 17.10 | 19.14 |
| 3840x2160 | 126 ms | 25.30 | 28.03 |

In game at 2560x1440 the evaluate measures 10-13 ms, depending on how busy the 5070 is.

## Transport, R9700 → RTX 5070 (`xadapter-probe`)

Both cards on PCIe 4.0 x4.

| Output | Payload/frame | Per-hop speed | One frame, end to end | Max sustained |
|---|---|---|---|---|
| 1080p | 15.7 MB | 5.7-6.1 GB/s | 5.3 ms | 393 fps |
| 1440p | 27.9 MB | 5.2-6.1 GB/s | 10.0 ms | 225 fps |
| 2880x1800 | 39.2 MB | 5.8-5.9 GB/s | 13.4 ms | 160 fps |
| 4K | 62.7 MB | 5.8-6.0 GB/s | 21.2 ms | 100 fps |

Every route (`shared`, `shared-dst`, `address`, `cpu`) works, and the shared fence works across vendors. Use the
shared heap: the `cpu` route's memcpy caps it near 98 fps at 1440p.

## The `FAIL_OutOfDate` bug (NRB11)

Core `Init` failed on roughly half of all launches with "installed NGX API is older than the one used by client
application". The driver's `_nvngx.dll` (617.14) export takes four arguments (AppId, DataPath, Device,
**SDKVersion**), the `NGX_SNIPPET_BUILD` form in the DLSS SDK header. It fails if the 4th is greater than `0x15`
in a signed comparison. The bridge, nr-probe and upstream MGPU Bridge passed a five-argument form with an
`NVSDK_NGX_FeatureCommonInfo*` as the 4th argument, so the driver read the low 32 bits of a stack address as the
version. That passed only when bit 31 happened to be set: at random, because of ASLR. Reboots, service restarts,
closing the NVIDIA App and clearing the NGX models folder all only seemed to help. With the 4-argument call, five
back-to-back nr-probe runs and every game session since started first time.

The plain-English version is in [`how-it-works.md`](how-it-works.md#the-hardest-problem-why-dlss-5-failed-on-half-the-launches).

## Running the probes

```powershell
build\xadapter-probe\xadapter-probe.exe --res 1440p            # first AMD -> first NVIDIA
build\nr-probe\nvngx.dll_nr-probe.exe                          # DLSS 5 NR on the 5070, 4 sizes
build\nr-probe\nvngx.dll_nr-probe.exe --res 1440p --image my_screenshot.png
```

`nr-probe` must keep `nvngx.dll` in its file name: the DLSS-NR snippet refuses callers whose module path lacks
it. Its log is `results\nr-probe\nr-probe.log`.

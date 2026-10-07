# Test log: motion vectors, depth, transport and latency

Goal: give DLSS-NR on the 5070 the game's real motion vectors and depth (at the start it got colour only,
`Depth=0 MVec=0`). On an AMD render GPU the game calls FSR, not DLSS, so the inputs come from the FSR
upscale dispatch instead of the game's NGX parameter block.

**Status (2026-10-07): done.** Motion vectors (NRB10, run19) and depth (NRB16, run29) come from FSR, and the
work went on into transport, pipeline, latency and pacing (runs 21-32). This file is a run log: each section
is what was known on that day, and later sections correct earlier ones (for example, the `FAIL_OutOfDate`
workarounds before its root cause was found).

## Steps

| Step | What | Status |
|---|---|---|
| 4.1 | Find which FSR API the game calls and how it reaches it | done (run13) |
| 4.2 | FFX tap: hook the upscale dispatch, publish a table like the NGX calibrator's (colour, depth, motion vectors, jitter, MV scale, render/upscale size, reset, create-time flags), and log it once per second. No transport change yet. | done (NRB9, run13 + run15) |
| 4.3a | Motion vectors into the existing MVec lane (NRB10): copy inside the dispatch hook once per present, `set_mvec_override` every frame, MVecScale from FSR. | done (run19 onwards) |
| 4.3b | Depth from FSR: needs a mid-frame depth copy (no seam exists; depth is only taken from the ReShade tap at present time), R32_FLOAT texture on GPU 1 for the typeless D32S8 source, arm sized from it. | done (NRB16, run29) |
| 4.4 | Units and signs: map FSR's MV scale, jitter, inverted depth and display-resolution MV flags to the `DLSSNR.*` keys; check visually and with the R74/R85 content probes. | MV scale from FSR (NRB10) and depth as R32_FLOAT (NRB16); checked visually in game (runs 19-32). The content probes were not run. |
| 4.5 | Generalise the `MvecFromEval` auto-fallback (keyed on the NGX calibrator table today). | not done; not needed with the deploy profile's explicit `MVec=3` |

FSR frame generation stays off for all of this: untested with the pipeline, and its dispatches also
carry motion vectors.

## 4.1 procedure

1. In game: Graphics → Resolution Scaling → **AMD FSR 3** (any quality mode), **Frame Generation off**.
2. Add `ProcessCensus=1` to `mgpu.ini` in the game's `bin\x64` (one launch lists every module in the
   process at arm, with versions; `[MGPU][R210]` lines).
3. `tools\ffx-scan.ps1` → `results\ffx-scan.txt` (exports of the FFX DLLs, and who imports them).
4. `tools\launch.ps1 -Watch run13`, get into gameplay until the stream arms, quit.

What decides 4.2:

| Finding | Hook |
|---|---|
| `amd_fidelityfx_dx12.dll` loaded, exe imports `ffxDispatch` statically | patch the exe's import slot for `ffxCreateContext` / `ffxDispatch` / `ffxDestroyContext` |
| `amd_fidelityfx_dx12.dll` loaded, no static import | hook `GetProcAddress` for those names before the game resolves them, or patch the DLL's export table |
| only `ffx_fsr3upscaler_x64.dll` loaded | same, for `ffxFsr3UpscalerContextCreate` / `ffxFsr3UpscalerContextDispatch` |
| no FFX DLL loaded with FSR selected | FSR is linked into the exe: fall back to the barrier route or a signature scan |

## What the game's DLLs show (2026-10-06)

Cyberpunk 2.31 ships **two** FSR stacks; which in-game option uses which was seen in run13 (next section).

| DLL | What | Tap |
|---|---|---|
| `ffx_fsr3_x64.dll`, `ffx_fsr3upscaler_x64.dll`, `ffx_frameinterpolation_x64.dll`, `ffx_opticalflow_x64.dll`, `ffx_backend_dx12_x64.dll` | FidelityFX SDK FSR 3.0.3/3.0.4 (CDPR-signed). `ffx_fsr3_x64` imports `ffxFsr3UpscalerContextDispatch` from the upscaler DLL, so every FSR 3.0 upscale passes there. | import-slot patch, every module |
| `amd_fidelityfx_dx12.dll` 1.0.1.41314 | FidelityFX API (FSR 3.1). Exports only `ffxCreateContext/ffxConfigure/ffxDispatch/ffxQuery/ffxDestroyContext`; games usually resolve them with GetProcAddress. | inline hook, prologue-checked |
| `amd_ags_x64.dll` 5.4.0 | AMD GPU Services (not FSR) | - |

Layouts: `FfxFsr3UpscalerDispatchDescription` (SDK fsr3-v3.0.3/3.0.4 headers; the DLL's dispatch code reads
offsets 0x6E8-0x720 exactly as the header places jitter...viewSpaceToMetersFactor) and `ffxDispatchDescUpscale`
(SDK v1.1.4 `ffx_upscale.h`). Both are `static_assert`ed in `nrb_ffx_tap.cpp`.

**FSR 4.** On RDNA 4 the AMD driver can upgrade FSR 3.1 (FidelityFX API) games to FSR 4. The game still
calls `ffxDispatch` with the same inputs, so the tap sees the same depth and motion vectors. If the driver
serves the API from its own `amdxcffx64.dll` instead of the game's DLL, the inline hooks will not match;
the tap logs that module's presence so a run shows which case applies.

## Run13 (2026-10-06): the tap sees both FSR options

The game menu offers **FSR 3.0** and **FSR 4** (no 3.1).

- **FSR 4** goes through the FidelityFX API: `amd_fidelityfx_dx12.dll` entry points (hooked inline), with the
  AMD driver's `amdxcffx64.dll` loaded behind them. ~100 upscale dispatches/s.
- **FSR 3.0** goes through `ffx_fsr3upscaler_x64.dll` (import slots in `ffx_fsr3_x64.dll`). Switching in the
  menu destroyed one context and created the other; the tap followed.

What FSR receives at 2880x1800 output, Quality:

| Input | Value |
|---|---|
| render size | 1920x1200 (1.5x) |
| colour | 1920x1200 `R16G16B16A16_FLOAT` (HDR, pre-tonemap) |
| depth | 1920x1200 `R32G8X24_TYPELESS` (D32 + stencil), state PIXEL_COMPUTE_READ; reversed (near=16000, far=0) |
| motion vectors | 1920x1200 `R16G16_FLOAT`, state PIXEL_COMPUTE_READ; scale (1920, 1200), i.e. stored in UV units |
| FSR 3.0 create flags | 0x69: HDR, depth-inverted, auto-exposure, dynamic-res. MVs at render resolution, jitter not cancelled |
| jitter | per-frame, within +-0.5 px |

The FFX API context was created before the tap installed (flags unknown). Installing from `init_device`
crashed the game (run14: ReShade unloads the add-on between startup devices, leaving patched slots pointing
into freed code). Fixed by installing from presents only, removing every hook on unload, suspending other
threads while writing prologues, and inferring the FFX API flags from each dispatch.

NGX `Init` failed `FAIL_OutOfDate` in run13 with the NVIDIA App closed **and its overlay disabled** (no
"Alt+Z" popup). So neither is the whole cause; the intermittent failure remains. NRB7 also logs
`NVIDIA Overlay.exe` now, and `tools/launch.ps1` closes it, to keep that variable out of future runs. Next to
test: a reboot without opening the NVIDIA App; whether `NvBackend.exe` is running on failing launches.
*(Superseded: the cause was NRB11; NRB7 and the closing were removed on 2026-10-07.)*

### For 4.3

DLSS-NR's MVec input has to be fed at render resolution (1920x1200) with a scale that turns UV units into
pixels (`MVecScaleX/Y` = render size, or the FSR scale as given), and depth is a typeless D32S8 that needs an
R32 view or a copy to R32_FLOAT before transport.

## Run15 (2026-10-06): stable, both paths, NGX up

After a reboot, App and overlay closed: NGX `Init` Success, first neural frame at +21 s, 2.5 minutes of play,
clean exit. Tap installed once (12:21:16), FFX API hooks 3/3, FSR 3.0 import slots 3. The game started on
FSR 3.0 (setting saved from run13), the menu switch to FSR 4 was caught at create:

| Context | Create flags |
|---|---|
| FSR 3.0 | 0x69: HDR, depth-inverted, auto-exposure, dynamic-res |
| FSR 4 (FFX API) | 0x28: depth-inverted, auto-exposure (no HDR flag); inferred-from-dispatch 0x8 agrees |

~63 upscale dispatches/s in gameplay with the neural stream running. 4.2 is done; next is 4.3.

## Run16 (2026-10-06): FSR motion vectors reach GPU 1

MVec=3 with NRB10: arm held 239 frames for a stable handle, then armed with an MV region of 1920x1200
R16G16_FLOAT (9.2 MB/frame on top of 20.7 MB colour). By frame 601: producer copies 603, missing 0,
size-rejected 0, lock-skipped 0; consumer seals valid 601, invalid 0, contract mismatches 0. Tap: 9652 copies,
0 state skips. Scale from FSR (1920, 1200) x ini (1, 1).

NGX `Init` failed `FAIL_OutOfDate` this launch, so no evaluate ran and the MV effect on the image is not yet
seen. NGX results so far alternate: run12 OK, run13 fail, run14 crash before NGX, reboot, run15 OK, run16 fail.
Hypothesis: the NGX session is left open on a clean exit (DllMain skips `stream_shutdown` when the process is
terminating), which poisons the next launch (the V21 comment in dllmain.cpp describes this), and the failed
launch creates no session, so the one after it works. Test: run17 should succeed, run18 should fail.

## NGX `FAIL_OutOfDate`: cause and workaround (2026-10-06, nr-probe)

Run17 failed too, so the "alternates" reading of run12-16 was wrong. With `nr-probe`, in the failing state:

| Test | Result |
|---|---|
| default (API 0x15, snippet loaded first, AMD device first) | FAIL_OutOfDate |
| `--sdk-version 0x14` | FAIL_OutOfDate |
| `--snippet-late` (no DLSS-NR snippet loaded at Init) | FAIL_OutOfDate |
| `--no-amd` | FAIL_OutOfDate |
| after `Restart-Service NvContainerLocalSystem` | FAIL_OutOfDate |
| after `Restart-Service NVDisplay.ContainerLocalSystem` | **Success**; DLSS-NR 1440p 9.13 ms avg |

NGX's verbose log on success shows "Override shared memory was opened/mapped successfully": the state behind
the check is held by the display container. `tools/launch.ps1` now restarts that service before each launch.
*(Superseded: chance, see the next section; the restart was removed.)*

## NGX `FAIL_OutOfDate`: root cause found (2026-10-06)

The display-container "fix" above was chance: later tests failed 3 s and 30 s after a restart, and with the NGX
models folder emptied nr-probe went S/F/S, then (with `--shutdown`) S/S/F/F/F. Disassembling `_nvngx.dll`:
`NVSDK_NGX_D3D12_Init` passes its 4th argument to a check `cmp r9d, 0x15; jle ok; else FAIL_OutOfDate`. The
real signature has the SDK version 4th; ours passed `&FeatureCommonInfo` there. Result depends on bit 31 of a
stack address. Fixed in NRB11.

Verified: with NRB11, five back-to-back nr-probe runs all returned `core Init ... Success` (2026-10-06),
where the same loop had failed 3 of 5 before the fix.

## Run19 (2026-10-06): DLSS-NR evaluates with FSR's motion vectors

NGX `Init` Success first try (NRB11), first neural frame 12 s after arm, ~4 minutes of play. MV lane: producer
copies 15605, consumer seals valid 15514; on GPU 1 unpacked 15110 and **bound to DLSS-NR 12076**; 87.6% of
sealed frames carried vectors (the rest coincide with FSR not dispatching: menus, pauses, the ReShade overlay).
Scale (1920, 1200). The ReShade overlay (Home) unroots the bridge visual by design (V53), 23 toggles logged;
the view/peek hotkeys need CTRL+ALT (F6/F7 alone do nothing).

## Run20 (2026-10-06): frame rate

PresentVsync=0, EvalTimes=1 (the per-evaluate summary only prints on bounded runs, so no NR time in this log).
Bridge loop per output frame 18-21 ms (run19 with vsync: 23-31 ms) -> ~50 neural fps on screen (was ~35).
Game on the R9700 in gameplay: median 58 fps in run18 (NR failed, transport only), run19 and run20 alike; ~95-100
in menus. The 5070 is the output limit: DLSS-NR at 2880x1800 ~17 ms. Next: a bridge-off baseline in the same
spot, and running NR below display resolution with DLSS SR on GPU 1 (SRUpscale) or design B.

## Run21 (2026-10-06): 2560x1440 output - the 5070 stops being the limit

Bridge-off baseline at 2880x1800 (in-game counter): ~75 fps, peaks high 80s. Run21 at 2560x1440 output (render
1707x960, FSR 4), DLSS-NR on: game median 64 fps (p25 59, p75 75, p90 80); bridge loop 4-10 ms per output frame,
so the 5070 waits on the game and the screen follows the game rate. Remaining cost is on the R9700: the producer
copies (colour 14.7 MB + MV 6.6 MB per frame) are recorded on the game's graphics queue. Next: move them to a
copy queue on the game device so they overlap rendering.

## Run22 (2026-10-06): NRB12 producer copy queue

Same spot and settings as run21 (2560x1440 output, render 1707x960, FSR 4), `ProducerCopyQueue=1`: built first
try (128 MB staging), no errors, ~6.5 minutes. Gameplay windows only (menus excluded, where FSR does not
dispatch): **game median 64.4 -> 75.4 fps** (quartiles 59/64/76 -> 68/75/81), which is the bridge-off level.
Neural evaluates 61.6 -> 64.2 per second. The 5070 is the limit again: it now skips game frames (79% of
evaluates one game frame apart, was 90%), so the screen gained ~3 fps, not 11. Seal latency (producer to
consumer) median 39.4 -> 44.0 ms: the copy queue's +~5 ms, as expected. Next: the 5070 side - DLSS-NR below
output resolution with DLSS SR (SRUpscale), or frame generation on the 5070.

## Run23 (2026-10-06): where the 5070's frame time goes (NRB13)

Same settings as run22. 57 windows of 600 output frames in gameplay. Typical window: **wall 15.6 ms per output
frame (64 fps), DLSS-NR EVALUATE 12.3 ms** (range 10.3-12.8), copy-queue unpack 3.55 ms (21.4 MB over PCIe,
~6 GB/s), seal/output ~0. The bridge thread's poll scope averages 14.3 ms, the rest of the loop ~1.3 ms.
So GPU 1's 3D engine is busy ~79% of each frame: the loop waits on the CPU for frame f's evaluate before it
submits f+1, and the unpack of the frame it then picks is often not finished, so ~3.3 ms per frame is idle.
DLSS-NR at full 2560x1440 alone would allow ~81 fps, above the game's ~75. Next: keep GPU 1 fed (submit the
next frame's work before the current one completes) instead of lowering NR resolution or adding frame generation.

## Run24 (2026-10-06): NRB14 GPU 1 pipeline

GpuPipeline=1 came on first try, no device errors, ~11 minutes. Per 600-frame window, game fps (R9700) against
frames shown (5070):

| Stretch | Game fps | Shown (run23, serial) | Shown (run24, pipelined) |
|---|---|---|---|
| Game at 76-88 fps | 76-88 | 61-65 | **72-76** |
| Game at 57-72 fps | 57-72 | 58-69 (= game) | 58-72 (= game) |

In the 5070-bound stretches 85-100% of frames were submitted behind a running one and wall time per output frame
fell to 13.1-13.8 ms (73-76 fps). The evaluate bracket grows from ~10.3 to ~13 ms when GPU 1 is never idle: other
work on that GPU (composition, our present) now lands inside it, so ~75 fps is the 5070's real ceiling at
2560x1440 here. Seal latency in gameplay unchanged (median 45.8 -> 44.6 ms).

Defect found: 60 DROPPED/REORDERED/STALE seal errors in the first ~10 s (menus at 140-190 fps). Skipped frames'
seals were copied in the direct list, which runs only after the frame ahead finishes, so the producer had reused
the slot by then. Fixed: when pipelining, seals are copied on the copy queue with the frame's own unpack, into a
per-submission seal readback.

## Run25 (2026-10-06): NRB14 in a long gameplay session

~13 minutes, 110+ gameplay windows: **game 77-84 fps, shown 75-77 fps** in every window, 593-600 of 600 frames
submitted behind a running one, evaluate 12.9-13.3 ms. The 5070's ceiling at 2560x1440 is ~76-77 fps and the
screen now sits on it. No device errors.

The run24 seal fix did not remove the menu errors (60 again, at 140-190 fps). The remaining cause: the pick still
read the seals of the frames it stepped over, up to five frames back, and at 190 fps the producer reuses the
oldest of those slots within milliseconds. The serial loop never reads them - its ring window (RingWindow=1, the
default) skips them and moves last_seen past. The pipeline now does the same, at finish time so the order holds.

## Run26 (2026-10-06): high fps, uneven pacing

Shown fps 75-76 in gameplay, but the R87 spacing between consecutive evaluated frames, gameplay only:
run23 (serial) 1 game frame 83.2%, 2 frames 16.7%, 3+ 0.2%; run25/run26 (pipelined) 1 frame 96-98%, **2 frames
0.0%, 3 frames 2-4%**. The pipeline chose the tex_in half by frame parity, so the frame after the one in flight
could never be two ahead: every single-frame skip became a three-frame jump, ~3 times a second. Also: seal
faults continued in gameplay (60 logged, 501 more suppressed), from frames whose copy fell back to the direct
list - which runs after the frame ahead finishes, late enough for the producer to reuse the slot.

Fixed: the half is chosen by submission (pipe_seq & 1); no parity rule; a pipelined frame whose copy-queue
submission fails is picked again next poll rather than copied late; the first pipelined pick is never at or behind
a speculative copy the serial loop left queued.

The game at 77-84 fps against 76 shown still drops ~5% of frames. A game frame cap at or just under the 5070's
rate (74-75) lets every frame through.

## Run27 (2026-10-06): parity fix, no frame cap - pacing smooth

Gameplay (44 windows): game median 72.9 fps, shown 72.4 (min 58.9, where the game itself was at ~59), evaluate
11.9 ms. Spacing between evaluated frames: 1 game frame 97.8%, 2 frames 2.1%, **3 frames 0.0%** (run26: 3.6%).
Seal latency median 47.5 ms. NO in-game cap was set
(corrected after the fact): the smoothness is the parity fix alone. The game happened to run at ~73 here, under
the 5070's ~76, so almost nothing was dropped; where the game runs above ~76 (run25: 77-84) a cap of ~74 should
still remove the remaining 2-frame skips.

Seal faults: ~1060, ALL between 16:33:40 and 16:35:08 while a menu ran at 188-193 fps. None at 141 fps in menus, none in gameplay. Pattern: the evaluated frame's slot already holds
f+6 when its seal is read, so above ~150 fps the copy for a pipelined frame can still start more than six game
frames after the pick. Not explained yet - the serial loop had none at 170 fps (run23). Open item; harmless in
gameplay, at worst a torn menu frame.

## Run28 (2026-10-06): NRB15 SignalAt=2 + lapped-frame guard

Gameplay (45 windows), against run27: game 71.8 fps (73.1), shown 71.6 (71.7), evaluate 12.7 ms (12.0), spacing
1 frame 97.5% / 2 frames 2.4% / 3 frames 0.1% (unchanged). **Seal latency in gameplay, median 44.8 -> 33.0 ms**:
one game frame (~13.9 ms at 72 fps) less, minus the copy-queue leg, as NRB15 predicted.

**Seal faults: 0** (run27: ~1060). The guard discarded 202 frames, all in two menu windows at 153 and 182 fps,
none in gameplay - the menu frames that used to be read after the producer reused their slot are now dropped
instead of shown. The cause of the late copy at those rates is still not identified; the guard makes it harmless.

## Run29 (2026-10-06): Depth=3 (depth from FSR)

Works: FSR's depth 1707x960 (fmt 19) carried as R32_FLOAT, 6.9 MB per frame, bound on every gameplay frame, no
NGX errors, no seal faults. The picture is visibly better with depth, but driving feels less responsive: gameplay game
73.7 / shown 73.6 fps (no fps loss), evaluate 12.7 -> 13.3 ms, copy-queue unpack 3.1 -> 4.0 ms, frames queued
behind a running one 464 -> 564 of 600, **seal latency 33.0 -> 44.5 ms**. GPU 1 crossed from "usually has slack"
to "always busy" at ~74 fps, so every frame waited a whole evaluate in the queue. Next: NRB17 AutoCap - pace the
game just below GPU 1's rate so nothing queues.

## Run30 (2026-10-06): Depth=3 + AutoCap (first version) - more responsive

Gameplay medians against run28 (no depth) / run29 (depth): game 69.5 / shown 69.1 fps (72 / 74), seal latency
**36.2 ms** (33.0 / 44.5), p90 49.1 ms (52.1 / 59.5). But the controller paced the game as low as ~40 fps in
places: its queue-wait signal was measured on the bridge thread, which notices a finish only when the present
loop gets back to it - the GPU had started the next frame long before. Replaced by GPU 1's busy fraction (its
own timestamps over wall time, 120-frame windows), held between 85% and 95%: run28 91% -> 33 ms, run29 98% ->
44.5 ms, run30 88% -> 36 ms.

## Run31 (2026-10-06): AutoCap on GPU busy - the limiter never ran

Gameplay: game 68.6 / shown 68.5 fps, seal latency 37.4 ms (p90 52.0), GPU 1 86% busy. But the "paced to" value
drifted to 33-50 fps while the game kept running at 66-77 (and menus at 140): the limiter's deadline was
thread_local and Cyberpunk presents from more than one thread, so no thread ever saw itself early and none slept.
The run30 latency gain was therefore not AutoCap (that run's scene ran the game at 69.5 instead of 73.7). Fixed:
one process-wide deadline; the timing line now counts paced presents, so the next run proves it ran.

## Run32 (2026-10-06): AutoCap working - depth with run28's latency

The limiter now runs ("599 presents paced" per 600-frame window) and the game sits on the paced rate (70.0 fps
paced -> 70.0 game). The controller held GPU 1 at 84-98% busy (median 89%), between 67 and 88 fps as scenes
changed. Gameplay, Depth=3 + AutoCap: game 70.0 / shown 69.8 fps, evaluate 12.8 ms, **seal latency median 34.0
ms, p90 42.8 ms** - against run28 (no depth) 33.0 / 52.1 and run29 (depth, no cap) 44.5 / 59.5. Depth's picture
at run28's median latency, with the best tail of any run, for ~4 fps. 0 seal faults.

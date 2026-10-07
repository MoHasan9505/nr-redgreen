# How NR-RedGreen works

A plain-English overview of the project: what it does, how a frame travels through it, what each part is
for, and what was hard. For the change-by-change record see `bridge-changes.md`; for the test runs and
their numbers see `test-log.md`.

## The idea in one paragraph

NVIDIA's DLSS 5 "neural rendering" makes a game's image look more realistic, but it only runs on NVIDIA
cards. NR-RedGreen lets a game render on an **AMD Radeon AI PRO R9700** while DLSS 5 runs on a **second card,
an RTX 5070**, in the same PC. The game never knows the second card exists: every finished frame is copied
across to the 5070, improved there, and shown on screen. It is built on MGPU Bridge, an open-source project by
Marcelo Guibout that did this between two NVIDIA cards; NR-RedGreen adapts it to an AMD + NVIDIA pair.

## The journey of one frame

```
 R9700 (renders the game)                          RTX 5070 (runs DLSS 5, drives the monitor)
 ─────────────────────────                         ──────────────────────────────────────────
 1. Game renders the frame (FSR upscales it)
    └─ inside FSR: copy motion vectors + depth ┐
 2. Frame finished: copy the image             ├─► staging buffer in the R9700's own memory
 3. R9700 copy queue: staging ──── PCIe ───────┴─► shared buffer (a 6-slot ring)
 4. "Frame N is ready" signal (a shared fence) ──────────────► 5. 5070 copy queue: ring ─► its own memory
                                                               6. DLSS 5 runs on the frame
                                                               7. Result is shown on the monitor
 8. AutoCap: the game is paced so frames never
    pile up waiting for the 5070
```

About 28 MB crosses PCIe per frame (image 14.7 MB, motion vectors 6.6 MB, depth 6.9 MB).

## The parts, and why each exists

| Part | What it does | Why it is needed |
|---|---|---|
| **ReShade add-on** | Loads inside the game and sees every finished frame | It is the hook into the game: no game files are modified |
| **sl-standin** | Hides the NVIDIA card from the game, and hides ray tracing from it | Otherwise the game tries to use the NVIDIA card, or fails to start ray tracing on the AMD one |
| **FSR tap** | Hooks AMD's FSR upscaler and copies its motion vectors and depth mid-frame | DLSS 5 needs to know how things move and how far away they are, and FSR already has both |
| **Transport ring** | Six slots in memory both cards can reach, each holding one frame plus an ID "seal" | Lets the R9700 keep producing while the 5070 is still working on an earlier frame |
| **Copy queue (R9700)** | Does the slow PCIe copy on a separate engine | The game no longer waits for the bus (game fps 64 → ~75) |
| **Pipeline (5070)** | Starts the next frame the moment the previous one finishes | Removed the 5070's idle time (shown fps ~62 → ~72-76) |
| **Early hand-off (`SignalAt=2`)** | Tells the 5070 a frame is ready right after the game's Present | One game frame less latency (~45 → ~33 ms) |
| **AutoCap** | Paces the game just below the 5070's speed, measured from the 5070's own timings | Without it, frames queue on the 5070 and each one waits a whole DLSS 5 pass |
| **Seal checks** | Every frame carries its number; the 5070 checks it | Catches frames that were lost, out of order, or overwritten mid-copy |

## Where it ended up (2560x1440, DLSS 5 at full resolution)

| | Start of the work | Now |
|---|---|---|
| Frames on screen | ~62 fps | ~70 fps, matching the game |
| Hand-off latency (finished frame → picked up by the 5070) | 39 ms | 34 ms (43 ms for the slowest 10% of frames) |
| Depth for DLSS 5 | none | on, from FSR |
| Smoothness | 3-frame jumps, errors in menus | clean |

The hand-off latency doesn't include the DLSS 5 pass or the monitor. The limit now is DLSS 5 itself: about
13 ms per frame on the 5070.

## The hardest problem: why DLSS 5 failed on half the launches

DLSS 5 started on some launches and failed with `FAIL_OutOfDate` on others. Several causes looked plausible
(the NVIDIA App being open, the overlay, a background service, the model files) and each seemed to help for
a while, because the failure was random. The way out was to stop guessing and read the NVIDIA driver library
itself.

- Its start-up function takes **4** arguments: `Init(AppId, DataPath, Device, Version)`. The bridge, following
  NVIDIA's public header, passed **5**, with a pointer inserted before the version.
- On 64-bit Windows the 4th argument travels in the R9 register, so the driver read that **pointer** as the
  version number.
- The driver checks `version <= 0x15` (0x15 = 21, which is API version "1.5") with a **signed** comparison,
  looking at the low 32 bits only.
- Windows randomises memory addresses on every launch. When the top bit of those 32 bits happened to be set,
  the "version" read as a negative number (e.g. -1.9 billion), which is below 21: it passed. Otherwise it
  read as a large positive number: `FAIL_OutOfDate`. A coin flip per launch.

Calling it with 4 arguments fixed it: 5 of 5 launches succeeded. The original bridge has the same bug.

Lessons: with an intermittent bug, a fix that "seems to help" proves nothing; find the root cause, then confirm
it with a repeatable test.

## Other problems worth knowing about

- **"High fps but choppy"**: the pipeline assigned its two input buffers by frame-number parity, so skipping
  one frame turned into skipping two. Assigning them in submission order fixed it.
- **Depth made driving feel sluggish**: depth cost the 5070 about 1.5 ms per frame, enough to make it the
  bottleneck, and frames started queuing (+11 ms). AutoCap was built to stop the queue forming.
- **AutoCap at first never ran**: its timer was per thread, and the game presents from several threads. One
  shared timer fixed it; a counter in the log now proves it runs.

## Glossary

- **PCIe**: the slot connection between a graphics card and the rest of the PC. Here both cards run at
  PCIe 4.0 x4, about 6 GB/s in practice.
- **Fence**: a counter shared by the two cards. "Fence = N" means frame N has arrived.
- **Copy queue**: a separate engine on a GPU that moves data while the main engine renders.
- **Motion vectors / depth**: per-pixel information about how things move between frames and how far away
  they are.
- **Latency**: the delay between something happening in the game and it appearing on screen.

## Credits

Built on [MGPU Bridge / Neural-coprocessor](https://github.com/maohgad-web/Neural-coprocessor) by Marcelo
Guibout (MIT licence). Uses ReShade, NVIDIA DLSS, AMD FidelityFX FSR, and RTInitFix. NR-RedGreen was designed,
tested and validated by its author on their own hardware; the code for the changes was written with Claude (AI).

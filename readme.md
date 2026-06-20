# Q2RTX Headless Benchmark for PenguinBurner

This fork turns Quake II RTX into a deterministic, headless Vulkan workload for
[PenguinBurner](https://github.com/jpietek/PenguinBurner).

It is not a general replacement for upstream Quake II RTX game builds. The goal
is a small, predictable benchmark binary that PenguinBurner can launch, monitor,
and score without display-server or OpenSSL compatibility workarounds.

## What This Fork Changes

- Headless benchmark mode is the default with `+bench 1`.
- No gamescope, Xvfb, X11 window, Wayland window, or fullscreen output is
  required for the benchmark path.
- The renderer uses an offscreen Vulkan target instead of a presentation
  swapchain in headless mode.
- The benchmark uses local demo content only, currently `q2demo1`.
- Q2RTX emits JSONL lifecycle/FPS events for PenguinBurner.
- Q2RTX measures FPS and frame timing only.
- PenguinBurner owns watts, GPU utilization, clocks, throttling, and VRAM
  telemetry externally.
- Native 4K is the default benchmark resolution.
- Adaptive resolution and upscaling are disabled for benchmark runs.
- RT/path tracing quality defaults are forced high for benchmark mode.

## Removed Workarounds

The benchmark build is intended to remove the PenguinBurner-side hacks needed
for the official Linux Q2RTX binary:

- no `gamescope --backend headless` wrapper,
- no hidden X11/Wayland window fallback,
- no `compat-openssl11` extraction,
- no `rpm2cpio`/archive fallback path for SSL libraries,
- no copying `libssl.so.1.1` or `libcrypto.so.1.1` beside `q2rtx`,
- no RUNPATH rewrite for the official binary,
- no timedemo text scraping for final FPS.

## Dependency Policy

The benchmark profile disables the network/download stack:

- no `libcurl`,
- no `libssl`,
- no `libcrypto`,
- no `libidn2`,
- no `libpsl`.

It also avoids dynamic SDL/OpenAL/display/audio dependencies in the headless
benchmark binary. Current verified dynamic dependencies on the Fedora build host
are:

```text
libvulkan.so.1
libatomic.so.1
libm.so.6
libstdc++.so.6
libc.so.6
```

This is not a fully static binary yet. Vulkan and the C/C++ runtime are still
external.

## Build

Portable release artifact, using the same `manylinux_2_28` x86_64 baseline as
PenguinBurner's native overlay wheel:

```bash
PB_BENCHMARK_VERSION=pb-benchmark-v0.1.1 \
  scripts/build-pb-benchmark-release.sh
```

The release tarball is written under `dist/pb-benchmark/`.

Local developer build:

```bash
cmake -B build-pb-benchmark -GNinja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCONFIG_PB_BENCHMARK=ON
cmake --build build-pb-benchmark
```

The locally built executable is `./q2rtx` at the repository root.

Verify the dependency goal:

```bash
readelf -d ./q2rtx | grep -Ei 'curl|ssl|crypto|idn|psl|SDL|openal|X11|wayland' && exit 1 || echo clean
```

## Run Manually

Headless 60 second 4K benchmark:

```bash
env -u DISPLAY -u WAYLAND_DISPLAY ./q2rtx \
  +set basedir /path/to/q2rtx-data \
  +bench 1 \
  +bench_seconds 60 \
  +bench_event_stdout 1
```

Use 1440p for lower-VRAM cards:

```bash
env -u DISPLAY -u WAYLAND_DISPLAY ./q2rtx \
  +set basedir /path/to/q2rtx-data \
  +bench 1 \
  +bench_seconds 60 \
  +bench_width 2560 \
  +bench_height 1440 \
  +bench_event_stdout 1
```

Visible debug mode is still available:

```bash
./q2rtx +bench 1 +bench_headless 0 +bench_event_stdout 1
```

## PenguinBurner Contract

PenguinBurner launches Q2RTX as a child process and passes an inherited event
file descriptor:

```bash
./q2rtx \
  +bench 1 \
  +bench_seconds 60 \
  +bench_demo q2demo1 \
  +bench_headless 1 \
  +bench_constant_load 1 \
  +bench_event_fd 3
```

Important cvars:

```text
+bench 1                 enable benchmark mode
+bench_seconds <seconds> measured benchmark duration, default 60
+bench_demo q2demo1      local demo workload
+bench_headless 1        offscreen Vulkan benchmark mode
+bench_constant_load 1   keep rendering after demo EOF until target duration
+bench_min_loops 1       complete at least one demo loop
+bench_width 3840        default width
+bench_height 2160       default height
+bench_event_fd <fd>     JSONL event stream for PenguinBurner
+bench_event_stdout 1    print JSONL events to stdout for manual testing
```

There is no warmup timer in the benchmark contract. Q2RTX starts scoring when
the first real demo frame has been rendered and emits:

```json
{"event":"phase","name":"measure_start","trigger":"demo_first_render"}
```

That event is the correct boundary for PenguinBurner to start watts, utilization,
clock, and VRAM aggregation.

## Event Stream

Q2RTX writes one JSON object per line. The important events are:

```text
start          benchmark mode initialized
loop_start     demo loop began
phase          measure_start or hold_last_scene
loop_done      demo loop completed
done           final FPS/frame-time summary
fatal          classified benchmark failure when available
```

Example final event:

```json
{"event":"done","reason":"target","loops":1,"render_frames":1773,"target_ms":30000,"measured_ms":30040,"fps_avg":59.021,"fps_min":29.412,"fps_max":1000.000,"frame_ms_mean":16.934}
```

PenguinBurner should treat the event stream plus process exit code as the run
result. Q2RTX stdout/stderr outside JSONL events are diagnostics only.

## Telemetry Boundary

Do not add NVML or NVIDIA telemetry calls to Q2RTX.

Q2RTX is responsible for:

- loading the deterministic demo workload,
- maintaining GPU render load,
- recording frame count and frame timing,
- reporting renderer/game failures.

PenguinBurner is responsible for:

- watts,
- GPU utilization,
- VRAM usage,
- clocks,
- throttling reasons,
- temperature and fan telemetry,
- pass/fail policy for sustained load.

## Current Validation Snapshot

On the local Fedora/NVIDIA test host, a 30 second headless default 4K run
completed with:

```text
measured_ms=30040
render_frames=1773
fps_avg=59.021
q2rtx process VRAM ~= 5305 MB, measured externally by PenguinBurner-style tooling
```

Those telemetry numbers are hardware and driver dependent. The API contract is
the event stream, not those exact values.

## Data Files

The benchmark needs local Q2RTX demo/shareware data under `baseq2`, including
the demo and Q2RTX media assets:

```text
baseq2/pak0.pak
baseq2/q2rtx_media.pkz
baseq2/shaders.pkz
baseq2/blue_noise.pkz
```

PenguinBurner may stage those files in its own data directory and launch Q2RTX
with `+set basedir <path>`.

## Upstream And License

This fork is based on [NVIDIA Quake II RTX](https://github.com/NVIDIA/Q2RTX),
which builds on Q2VKPT and Q2PRO. See [license.txt](license.txt) for the GPLv2
source license.

Quake II game data remains separately copyrighted by id Software and must not be
redistributed unless the data license allows it.

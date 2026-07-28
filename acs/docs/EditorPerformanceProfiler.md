# Editor Performance Profiler

The editor profiler separates three measurements that are easy to confuse:

- **Editor FPS** is observed wall-clock viewport cadence: monotonic elapsed
  time divided by the native presented-frame index delta. It includes editor
  scheduling and presentation stalls and is therefore the number used for the
  300 FPS goal.
- **Native reported FPS** is the renderer's smoothed frame-interval diagnostic.
  Direct render bursts can make it much higher than wall-clock editor cadence,
  so it is shown and exported separately and is not acceptance evidence.
- **CPU frame** is renderer command recording and submission work measured on
  the CPU.
- **Native active CPU** starts only after the cooperative GPU-ready preflight
  succeeds and stops before submit. It therefore excludes both Dispatcher
  queue delay and GPU-busy retry time.
- **Present CPU** measures the terminal submit/`Present` call separately. It
  is not a GPU timestamp and is not display scan-out latency.
- **GPU frame** comes from asynchronous timestamp queries. The UI reports query
  latency and never relabels CPU timings as GPU timings.

“Presented” here means that ACS completed submission and the swapchain
`Present` call returned successfully. It is a render/presentation-call
throughput measure, not proof that the monitor scanned out 300 distinct images;
physical display cadence remains bounded by the selected display, DWM, and
driver presentation policy.

## Frame-budget analysis

Choose a target from the profiler toolbar to compare the recent bounded history
against its frame budget. The default is 300 FPS (3.33 ms). This is an analysis
control only; changing it does not lower quality, cap the renderer, or change
the editor update cadence.

CPU and GPU p95 values use nearest-rank percentiles over recent 10 Hz profiler
samples. They are deliberately labelled with their sample counts: they help
locate persistent bottlenecks, but they are not a claim that every rendered
frame was captured. GPU p95 uses individual completed timestamp queries rather
than a percentile of rolling averages.

The status is:

- `WITHIN BUDGET` when the sampled CPU and available GPU p95 values fit.
- `CPU OVER BUDGET`, `GPU OVER BUDGET`, or `CPU + GPU OVER BUDGET` when the
  corresponding p95 exceeds the selected frame budget.
- `WAITING` until a CPU sample is available. Missing GPU timestamps remain
  `N/A`; they are never interpreted as zero GPU cost.

Hover either p95 value to see the exact over-budget sample count.

## Capture export

`Export CSV` writes the current bounded history without pausing the renderer.
The file contains:

- the selected target FPS and derived frame budget;
- frame index, monotonic sample timestamp, observed editor cadence, and the
  separate native-reported FPS diagnostic;
- CPU frame, GPU-ready native active-render CPU, and submit/Present times;
- the individual completed GPU query and native GPU rolling average;
- major CPU pass timings.

The export is written asynchronously to a unique sibling temporary file and
then atomically moved over the selected destination. A failed export never
deletes or truncates the selected destination.

Use the CSV for regression comparisons and longer-term graphing. For a
performance acceptance claim, pair it with the exact scene, resolution,
hardware, build configuration, warm-up period, and GPU-driver version.

## Unattended capture

An interaction soak can publish the profiler's bounded history without any
dialog or input:

```powershell
$capture = Join-Path $env:TEMP "acs-profiler-horizon.csv"
$report = Join-Path $env:TEMP "acs-profiler-horizon-soak.json"
AcsEditor.exe .\Game.acsproject `
  --secondary-monitor --unattended --show-profiler `
  --interaction-soak 30 --interaction-soak-report $report `
  --profiler-capture $capture
```

`--profiler-capture` is valid only with `--interaction-soak`; it never invents
an output path. The explicit destination must be a `.csv` below the process
`TEMP` root, its parent must already exist, and the destination and every
ancestor through `TEMP` must be regular files/directories without a reparse
point. Publication is capped at 1 MiB and uses a flushed, uniquely named
sibling followed by a same-directory atomic move. A validation, size, write,
or move failure does not truncate the previous destination.

The command forces the profiler panel visible, so it reuses the existing 10 Hz
sampler; it does not add a per-frame readback or work to the renderer hot path.
History remains bounded to the latest 120 unique native frames. A 30-second
soak therefore reports exactly the samples retained for roughly its final
12 seconds, not every rendered frame.

Immediately before the ready-state soak begins, ACS clears both bounded
histories and resets native, scheduling, log-pump, and Dispatcher diagnostic
state. The native reset also invalidates current CPU/GPU values, the GPU-valid
flag, smoothed FPS, and the cloud-workload payload. Sampling then waits for a
presented frame whose reset serial exactly matches that boundary. Dispatcher
heartbeat time, active-stall state, and diagnostic counts are rebased at the
same point. Startup shader compilation, attachment, and warm-up therefore
cannot inflate capture values, maxima, or counts.

The CSV metadata and the soak report's `ProfilerSummary` state:

- retained sample count and first/last native frame index;
- cadence source, observed Editor FPS, native-reported FPS, observed
  per-frame interval average/p95, and the FPS equivalent of p95 interval;
- CPU frame, native active-render, and Present average and nearest-rank p95
  with sample counts;
- the maximum rolling native active-render/Present peak observed by any
  retained sample, presented-frame count since reset, reset serial, and
  capture-boundary consistency. A rolling peak may decrease after an older
  frame leaves its 120-frame native window; validation therefore compares each
  row's current value only with that same row's rolling peak;
- completed GPU-query average and nearest-rank p95 with sample counts, plus
  throughput equivalents derived from those measured milliseconds;
- sampled CPU pass average/p95 for opaque, atmosphere, cloud, fog, and post;
- the latest native unique-query GPU window count/capacity/latency and its
  frame/pass average and peak;
- the latest render-state proof: 2D/3D and Game View flags, scene-publication
  suppression, enabled clouds/fog/aerial perspective, draw/dispatch/triangle
  counts, viewport/cloud resolution, trace steps, and cloud render scale;
- the negotiated cloud-workload snapshot, including submission/history/TSR
  state, trace/output dimensions, steady/bake/shadow dispatches, launched
  threads, logical invocations, and maximum view/light sample bounds;
- the matching editor-runtime snapshot: native render-call and slow-call counts,
  cooperative GPU-backpressure yields, input-priority retries, low-priority
  fallbacks, ready-after-retry completions, renderer-fairness yields,
  latest/maximum backpressure-epoch duration, peak presented-frame burst and
  active CPU time, latest/maximum native-call duration, and Dispatcher
  heartbeat gaps, stalls, age, and phase.
- a separate bounded runtime timeline sampled even when the native frame index
  does not advance. It records cumulative GPU-busy/retry/ready/fairness events,
  Input-continuation and maintenance-yield counts, their distinct queue waits,
  GPU-busy epoch duration, Dispatcher gap, and heartbeat age.

The retry and fallback counters distinguish GPU fence latency from Dispatcher
starvation. Direct private-HWND bursts always end at an Input-priority
Dispatcher checkpoint within 64 presented frames or 64 milliseconds of active
CPU work. GPU-busy polling retains its tighter independent bound of 256
attempts or eight milliseconds before the same mandatory checkpoint. These
limits apply to unattended captures as well as interactive launches; current
keyboard or pointer work requests an earlier checkpoint.

The 10 Hz profiler and 500 ms interaction-health/heartbeat timers use the same
FIFO Input priority, so a due sample advances between bounded render bursts
without polling native state per frame. A periodic Background drain also owns
one continuation during hidden startup and every 500 ms. This is required
because repeatedly returning from an Input callback directly into another
private HWND message can prevent timer promotion and lower-priority startup
finalization. Input and Background queue waits are reported independently, so
a busy GPU and either Dispatcher delay cannot be mistaken for one another. A
large Background-drain wait is real render downtime and diagnoses excess
lower-priority editor backlog.

The sampled GPU-query percentile, latest native rolling pass window, observed
editor cadence, and native-reported burst FPS are separate contracts and are
labelled separately. A capture is not evidence for 3D/cloud performance unless
its render-state section proves `View3D`, cloud enablement, non-suppressed
presentation, non-zero submitted work for the intended fixture, and an
available editor-runtime diagnostic snapshot. The cloud workload's profiler
frame must also lie between the first and last accepted capture frame; a
pre-reset cloud payload fails validation. An
unavailable measurement is
`N/A` in CSV and `null` with an explicit zero sample count or `Available=false`
in JSON; it is never converted to zero milliseconds. An explicitly requested
capture makes the soak fail with a specific `PROFILER_CAPTURE_*` fault if it
has no samples, has no completed GPU queries, lacks the GPU pass window, or
cannot publish the file.

When `--camera3d` is present, the capture also fails closed unless the latest
validated snapshot proves a non-suppressed 3D view with at least one draw or
dispatch. This prevents an empty legacy 2D startup override from being reported
as a 3D/cloud benchmark.

For the official same-quality horizon/zenith comparison, including exact cloud
resolution/sample-work validation and an optional 300 FPS release threshold,
use [Cloud Performance Verification](CloudPerformanceVerification.md).

`--profiler-selftest` covers reset-generation gating, cloud-frame containment,
summary math, availability, invariant CSV,
TEMP/path/reparse policy, the output limit, atomic replacement, temporary-file
cleanup, and preservation of an existing destination on failure. It is part of
both the fast and full `verify_editor.ps1` gates.

## Visual regression fixture

The populated profiler layout can be rendered without a live GPU:

```powershell
AcsEditor.exe --profilershot C:\Temp\acs-profiler.png
```

The fixture uses a 980-pixel window with deterministic CPU/GPU samples,
including a 300 FPS budget violation. It is intended to catch clipped toolbar,
budget, card, graph, and pass-metric geometry. It does not constitute a
performance benchmark.

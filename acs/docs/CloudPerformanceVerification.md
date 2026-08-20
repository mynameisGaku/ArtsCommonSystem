# Cloud Performance Verification

`profile_cloud_quality.ps1` is the reproducible acceptance harness for ACS
volumetric-cloud performance. It captures the same 3D fixture twice:

- `horizon`: a nearly horizontal view that includes the horizon;
- `zenith`: an 89-degree upward view that exercises the worst visible cloud
  coverage.

Both runs are unattended and sequential. They use the existing editor
automation path (`--show-profiler`, `--hide-grid`, `--interaction-soak`,
`--profiler-capture`, and `--camera3d`), so the harness does not introduce a
second renderer or synthetic timing path.

## Run

```powershell
$editor = ".\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64\AcsEditor.exe"
$project = "C:\path\to\RenderingShowcase\RenderingShowcase.acsproject"

.\acs\scripts\profile_cloud_quality.ps1 `
  -EditorExe $editor `
  -Project $project `
  -SoakSeconds 30 `
  -Monitor secondary
```

`-Project` must point to a prepared cloud showcase whose configured initial
scene is the intended 3D scene. The harness deliberately does not manufacture
or silently substitute a fixture; the editor report must prove that the loaded
scene rendered real 3D cloud work.

An explicit monitor index is also supported:

```powershell
.\acs\scripts\profile_cloud_quality.ps1 `
  -EditorExe $editor -Project $project -MonitorIndex 1
```

The default output is a unique directory below `TEMP`. An explicit
`-OutputDirectory` must also be a child of `TEMP`, because unattended profiler
captures deliberately reject destinations outside the process temporary root.
The harness rejects reparse-point ancestors and refuses to overwrite any
existing report, capture, log, or summary.

The editor and project inputs must be non-empty regular files rather than
reparse-point leaves. Input hashes are read without writer/delete sharing.
This prevents a concurrent writer from producing a hash of partially replaced
content. Provenance includes the editor app host, managed assembly, native
`acs_editor_abi.dll` renderer, dependency manifest, runtime configuration, and
project manifest; hashing only the small app host is not accepted as renderer
identity. The harness keeps read-only, non-writer/non-delete-sharing leases on
all six inputs for the complete run, closing the check-to-launch
replace-and-restore window rather than relying only on before/after hashes.

The editor is launched with `--unattended`, so neither scenario receives
mouse/keyboard input or activates itself. `-Monitor secondary` keeps both
captures on the first active secondary display; use `-Monitor none` only when
the host process should choose placement.

Use `-DryRun` to validate inputs and print both exact editor commands without
creating output or starting the editor. Use `-SelfTest` without other
parameters to run the synthetic parser and validation boundary suite plus
short isolated child-process checks for exit `0`, exit `7`, timeout, asynchronous
stdout/stderr draining, and descendant cleanup. It does not start the editor or
perform GPU work.

`verify_editor.ps1 -Mode full` runs that GPU-independent `-SelfTest` as the
`rendering / cloud profiler harness self-test` step. Fast and managed modes do
not include it. This validates the harness contract on every complete
verification run without starting the editor or turning a hardware-dependent
FPS measurement into a build gate; real horizon/zenith captures remain an
explicit release-performance run.

## Fail-closed quality evidence

Each scenario must prove all of the following:

- report result is `PASS`, with no report or capture faults;
- observed editor cadence has real samples;
- a non-suppressed 3D view rendered non-zero work with clouds enabled;
- completed GPU queries and the native GPU pass window are available;
- cloud frame/pass timings are finite and positive;
- viewport and cloud trace resolutions are coherent;
- cloud scale is exactly `0.25`, with 192 view samples and 8 light samples;
- cloud work was attempted and submitted;
- temporal history was available and reused without invalidation, with TSR
  enabled;
- the steady cloud frame remains exactly two compute dispatches plus one
  composite draw, with no one-time bake or shadow-cache dispatch leaking into
  the measured frame;
- logical invocation, launched-thread, and maximum view/light sample totals
  are internally coherent;
- native render, Dispatcher heartbeat, GPU retry/fallback, ready-after-retry,
  and renderer-fairness diagnostics are present.

JSON is decoded as strict UTF-8. Numeric schema fields must be finite JSON
numbers of the appropriate integer or duration shape: `null`, numeric strings,
`NaN`, infinities, fractional counters, and out-of-range counters fail closed.
Boolean fields must be JSON booleans rather than `0`/`1` or string lookalikes,
and status enums are case-sensitive.
GPU query count may not exceed query capacity, and zero-work fields must be
reported explicitly rather than omitted or represented by `null`.

The horizon and zenith quality snapshots are then compared field by field.
Viewport/trace resolution, quality flags, query window configuration,
dispatches, TSR, and exact maximum sample work must remain identical. A camera
change therefore cannot silently buy performance by reducing cloud quality.

## Runtime settings safety

`CVolumetricClouds` normalizes every public layer, lighting, range, and upper-
layer setting on the CPU before storing or uploading it. Non-finite values use
the field default; distances, fade, step growth, sample count, transmittance,
phase eccentricity, and contribution ratios are bounded to the ranges declared
in `Sky.h`. An upper layer that cannot remain above the normalized lower layer
is disabled instead of being uploaded as an intersecting or zero-thickness
density band.

Only an effective normalized change invalidates temporal history. Lower- or
upper-layer changes also invalidate the sun-depth cache because they change the
density field. Lighting, distance, and reference-mode changes retain the raw
density cache, while rejecting accumulated screen-space color. The exact and
cached light paths both use the configured `LightExtinction`; no legacy fixed
extinction coefficient is allowed in cache confidence or early termination.

These bounds establish numerical and cache-coherency safety. They do not by
themselves prove that the heuristic density, phase, or multiple-scattering
model is physically calibrated; that remains a visual-reference requirement.

Both the Editor and legacy Scene3D paths update cloud illumination from the
current scene before dispatch. They evaluate atmospheric RGB transmittance at
the normalized cloud-layer midpoint, use the current zenith color for the
top-to-bottom sky-light gradient, and use the current lower-hemisphere color
for ground bounce. This prevents low-sun clouds from retaining white midday
direct light and prevents the base and top of the cloud from sharing one
horizon-color ambient term.

The directional-light approximation keeps the single-scattering term intact
and adds one reduced second-order term:

`S = exp(-tau) * phase0 + a * exp(-b * tau) * phase1`

`MultiScatterContribution` is `a` and `MultiScatterOcclusion` is `b`. Runtime
normalization enforces `0 <= a <= b <= 1`, so the reduced scattering
coefficient cannot exceed the reduced extinction coefficient. Both phase terms
use the configured phase bounds. This is a bounded two-order approximation,
not a claim of a complete multiple-scattering solution. The model follows the
coefficient-reduction and additive-order contract described in Frostbite's
[SIGGRAPH 2016 course notes](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016-pbs-frostbite-sky-clouds-new.pdf);
the Beer/Henyey-Greenstein/powder foundation and visual-reference requirement
remain aligned with Guerrilla's [Horizon cloud presentation](https://www.guerrilla-games.com/read/the-real-time-volumetric-cloudscapes-of-horizon-zero-dawn).

## Quality-preserving optimization rules

Ultra cloud optimization keeps the `0.25` trace scale, 192 view-sample ceiling,
8 light probes, sixteen-phase TSR, world-space density coordinates, and every
accepted probe position fixed. A candidate is retained only when the same
provenance-locked horizon/zenith harness shows a repeatable GPU improvement;
FXC instruction count is diagnostic evidence, not acceptance by itself.

The view marcher keeps its `shape <= 0.006` empty-space consumer contract.
Progressive four-lobe and three-lobe shape rejection uses the exact maximum
weight of all unvisited lobes, so those existing skips cannot create a false
negative. Additional pre-fetch bounds are not accepted from algebra alone:
they must also beat the baseline on both horizon and zenith captures without
introducing enough branch divergence to erase the saved texture work.

The first three light probes retain detail erosion. The remaining five retain
the same positions, height/profile equations, threshold, and three shape
fetches but return their scalar macro extinction directly. This shortens
near-probe-only value lifetimes; it does not replace, move, or reduce any light
probe. The experimental shadow cache remains disabled until an identical
quality capture demonstrates a net GPU win.

Curved-shell intersection also preserves the original factorized quadratic.
Camera position relative to the rebased tangent origin and the inner/outer
`c` terms depend only on camera, layer, and world origin, so the CPU writes
them once per frame. Each trace ray still solves the same two roots with the
same planet radius and chooses the same nearest continuous interval; only
duplicated per-pixel construction of those invariant terms is removed.

## Results and the 300 FPS target

`cloud-quality-summary.json` is the machine-readable result. The terminal table
reports, for each camera:

- observed Editor FPS average;
- FPS converted from the p95 observed frame interval;
- GPU throughput converted from average and p95 query milliseconds;
- cloud GPU average and peak milliseconds;
- input-priority retry, ready-after-retry, background fallback, and
  fairness-yield counts, plus the maximum backpressure epoch and peak presented
  burst.

The same summary also records the UTC capture time, OS and architecture, every
reported GPU adapter and driver version/date, and the canonical paths,
versions, sizes, timestamps, and SHA-256 hashes of `AcsEditor.exe`, its four
required runtime artifacts, and the `.acsproject`. All six inputs are hashed
immediately before and after each capture, then once more after both captures.
Any checkpoint differing from the initial identity fails the affected scenario
and `ProvenanceGate`. The terminal provenance tables print the same artifact,
project, GPU, and driver identities, so a result cannot be compared without
seeing which renderer and machine produced it.

The 300 FPS target is informational by default. `TargetGate.Result` says
`MISS` when either view misses any of the four cadence/throughput checks, while
the script still succeeds if its quality and evidence gates pass. This keeps a
measured performance gap visible without mislabelling it as missing or corrupt
quality evidence.

For a release threshold, opt in explicitly:

```powershell
.\acs\scripts\profile_cloud_quality.ps1 `
  -EditorExe $editor -Project $project `
  -RequireTargetFps -TargetFps 300
```

With `-RequireTargetFps`, a target miss returns exit code 1. After PowerShell
parameter binding succeeds, input/path/setup errors return exit code 2.
Malformed parameter types and out-of-range values are rejected by PowerShell
before the harness can assign an exit code. The reported FPS is
render/presentation-call throughput; it is not a claim that the physical
monitor scanned out that many distinct images.

If an editor capture exceeds its soak plus startup-grace timeout, the harness
terminates the Windows process tree. A failed `taskkill` is checked by exit code
and falls back to terminating the root process instead of being treated as
successful cleanup. Editor launch uses `System.Diagnostics.Process.Start`
directly rather than `Start-Process -PassThru -Redirect*`, so Windows
PowerShell 5.1 reads the exit code from the same process object that performed
the launch. Standard output and error are copied concurrently to
create-new-only log files before waiting, preventing pipe deadlock and
preserving the no-overwrite guarantee.

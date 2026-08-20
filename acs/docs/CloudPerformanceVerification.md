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

## 解像度による原因切り分け

Editor の `Rendering/CloudRenderScale` は内部描画の品質倍率であり、`1.0` は画面寸法の
`1/4`、`2.0` は `1/2`、`4.0` は等倍になる。通常の品質・性能検査は`1.0`を使い、
出力解像度の履歴へ16段階で再構成する。`4.0`は全画素を毎フレーム描画するため、低解像度描画や
時間再構成が形状の乱れを作っているかを目視で切り分ける場合だけ使う。通常描画と同じ192刻みを
保つので、比較対象を画面解像度へ限定できる。

`Rendering/CloudReferenceMode=true`は内部描画を等倍へ固定し、時間再構成を無効にして、視線レイの
刻み上限を512へ増やす。通常C++利用側では`CVolumetricClouds::SetReferenceMode(true)`が同じ役割を
持つ。刻み上限の7/8を実積分、1/8を空領域から細密領域へ戻る余裕へ使うため、通常192刻みは168分割、
参照512刻みは448分割になる。`CloudRenderScale=4.0`でも乱れが残り、参照描画だけで消える場合は
レイ積分が原因である。参照描画でも残る場合は、密度場または照明式を監査する。どちらも常用時の
性能基準ではなく、原因を分離するための診断設定として扱う。

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

## 雲形状の時間変化

風の移流だけでは密度場全体が同じ方向へ平行移動し、雲の輪郭や内部の盛り上がりは変化しない。
現在の実装は、天候領域と基本形状の第 1 領域をワールド座標へ固定したまま、残りの基本形状領域、
第 2 渦領域、二つの侵食領域へ異なる低速の位相ずれを加える。これにより大きな雲塊の連続性を
保ちながら、輪郭と細部が非剛体に成長・浸食する。

位相ずれは `ResolveVolumetricCloudEvolutionFrameTerms()` が CPU で 1 フレームに一度だけ求め、
視線方向と光方向の密度評価で共有する。既存のテクスチャ採取位置を動かすだけなので、形状、
天候、渦、侵食の採取回数は増えない。時刻 0 では全項が 0 となり従来の密度場を保つ。無風でも
緩やかな対流変形は続き、風速の絶対値が大きい場合だけ変化速度を制限範囲内で上げる。

時間再構成は 0.25 秒を超える時刻飛びで履歴を無効化する。通常フレーム間の位相差は十分小さく、
既存の色・深度検査で局所的な形状変化を処理する。実験的な太陽深度キャッシュは現在無効である。
再び有効にする場合は、位相ずれをキャッシュ鍵へ含めることを必須とする。

## 雲頂の対流形状

雲種ごとの高さ分布を正規化高度へそのまま適用すると、同じ雲種が続く広域で密度の境界も同じ
高さとなり、雲頂が横に平らな層として見える。現在の密度評価は、既に取得した天候値から柱ごとの
高さ変形量を求める。被覆の強い中心は持ち上げ、薄い縁は押し下げる。層雲では変形量を小さく、
積雲または降水域では最大 `0.18` まで広げる。天候のゆがみ値へ時間位相を小さく加えるため、
無風時でも雲頂が一様な上下動ではなく柱ごとに緩やかに変化する。

正規化高度 `h`、柱の変形量 `s`、上層倍率 `b` に対する変形後高度は次式とする。

`h' = saturate(h - s * 4 h^2 (1 - h) * b)`

下層では `b = 1`、薄い上層では `b = 0.30` とする。この式は `h = 0` と `h = 1` を固定するため、
雲底と物理層の上端を越えない。`|s| <= 0.18` では区間全体で単調増加となり、高さが折り返して
同じ柱に不連続な密度面を作らない。変形後高度は高さ分布だけでなく基本形状の三次元採取座標にも
使うため、輪郭だけを切り取る処理ではなく雲体全体が縦に伸縮する。

視線密度で求めた雲種、降水量、柱の変形量は、近距離3点と遠距離5点の光採取へ同じ値を渡す。
したがって密度形状と自己遮蔽の高さは一致する。既存の天候、基本形状、渦、侵食テクスチャの
採取回数は増えない。単体試験は層の両端、値域、全許容変形量での単調性、上層の抑制、光採取への
共有、および採取回数を検査する。

Both the Editor and legacy Scene3D paths update cloud illumination from the
current scene before dispatch. They evaluate atmospheric RGB transmittance at
the normalized cloud-layer midpoint, use the current zenith color for the
top-to-bottom sky-light gradient, and use the current lower-hemisphere color
for ground bounce. This prevents low-sun clouds from retaining white midday
direct light and prevents the base and top of the cloud from sharing one
horizon-color ambient term.

The directional-light approximation applies a bounded in-scatter probability
to the single-scattering term and adds one independent reduced second-order
term. For low-LOD density `d` and normalized layer height `h`:

`pDepth = saturate(0.05 + pow(d, lerp(0.5, 2.0, saturate((h - 0.30) / 0.55))))`

`pVertical = pow(lerp(0.10, 1.0, saturate((h - 0.07) / 0.07)), 0.8)`

`fInScatter = lerp(1, pDepth * pVertical, PowderStrength)`

`S = exp(-tau) * phase0 * fInScatter + a * exp(-b * tau) * phase1`

`PowderStrength` keeps its compatibility name, but is now a blend ratio in
`[0, 1]`. The factor cannot amplify incident light. It derives low-LOD density
from the already sampled base noise using the final weather-coverage and height
thresholds. It does not reuse the deliberately wider empty-space occupancy
field, add a texture fetch, or attenuate the explicit second-order term a
second time. The former arbitrary near-light probe, `edgeBoost`, and its `1.08`
energy increase are absent.

`MultiScatterContribution` is `a` and `MultiScatterOcclusion` is `b`. Runtime
normalization enforces `0 <= a <= b <= 1`, so the reduced scattering
coefficient cannot exceed the reduced extinction coefficient. Both phase terms
use the configured phase bounds. This is a bounded two-order approximation,
not a claim of a complete multiple-scattering solution. The model follows the
coefficient-reduction and additive-order contract described in Frostbite's
[SIGGRAPH 2016 course notes](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016-pbs-frostbite-sky-clouds-new.pdf);
the density-and-height in-scatter probability follows Guerrilla's updated
[Nubis SIGGRAPH 2017 cloud-lighting model](https://advances.realtimerendering.com/s2017/Nubis%20-%20Authoring%20Realtime%20Volumetric%20Cloudscapes%20with%20the%20Decima%20Engine%20-%20Final%20.pdf).
This is still a production approximation, not a claim of complete radiative
transfer; the locked visual-reference capture remains an acceptance gate.

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

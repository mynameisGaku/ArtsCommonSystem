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
- 定常フレームは影キャッシュ生成、視線積分、時間再構成の計3回の計算ディスパッチと、
  合成描画1回だけであり、一度限りの雑音生成が計測区間へ混入していないこと。
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
参照512刻みは448分割になる。参照描画では時間平均を行わないため、視線と光の積分位置を区間中央へ
固定し、毎フレーム変わる未平均の粒状誤差を基準画像へ混ぜない。`CloudRenderScale=4.0`でも乱れが残り、参照描画だけで消える場合は
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
既存の色・深度検査で局所的な形状変化を処理する。太陽方向深さキャッシュは、移流だけを鍵にして
古い密度を再利用せず、現在の対流位相を含む密度場から毎フレーム生成する。

## 雲底と雲頂の対流形状

雲種ごとの高さ分布を正規化高度へそのまま適用すると、同じ雲種が続く広域で密度の境界も同じ
高さとなり、雲底と雲頂が横に平らな層として見える。以前の高さ変形は `h²` を含んでいたため、
雲底へ近づくほど柱ごとの差が二次的に消え、雲頂だけを直しても下端の水平線が残っていた。
現在の密度評価は、既に取得した天候値から柱ごとの高さ変形量を求め、雲層の内部全体へ適用する。
被覆の強い中心は持ち上げ、薄い縁は押し下げる。層雲では変形量を小さく、積雲または降水域では
最大 `0.18` まで広げる。天候のゆがみ値へ時間位相を小さく加えるため、無風時でも雲体が一様な
上下動ではなく柱ごとに緩やかに変化する。

正規化高度 `h`、柱の変形量 `s`、上層倍率 `b` に対する変形後高度は次式とする。

`h' = saturate(h - s * 4 h (1 - h) * b)`

下層では `b = 1`、薄い上層では `b = 0.30` とする。この式は `h = 0` と `h = 1` を固定するため、
物理層の上下端を越えない。導関数の下限は `1 - 4 |s| b` であり、`|s| <= 0.18` では下層でも
`0.28` 以上となる。このため区間全体で単調増加し、高さが折り返して同じ柱に不連続な密度面を
作らない。変形後高度は高さ分布だけでなく基本形状の三次元採取座標にも使うため、輪郭だけを
切り取る処理ではなく雲体全体が縦に伸縮する。

この変形が対象とするのは雲体内部の密度境界である。曲面雲層そのものが遠方で地平線へ収束して
生じる画面上の境界は物理層の交差結果なので、この式では移動させない。

視線密度で求めた雲種、降水量、柱の変形量は、近距離3点と遠距離5点の光採取へ同じ値を渡す。
したがって密度形状と自己遮蔽の高さは一致する。既存の天候、基本形状、渦、侵食テクスチャの
採取回数は増えない。単体試験は層の両端、値域、全許容変形量での単調性、上層の抑制、光採取への
共有、および採取回数を検査する。

## 高さ分布の二重適用防止

雲種ごとの高さ分布は、基本形状を雲として採用するしきい値へ反映する。この値を最終密度へも
そのまま掛けると、同じ高さ変化を二度適用し、地平線に近い視線では層境界が水平な濃淡の段として
強調される。現在は、しきい値用の重みが `0.12` に達するまでだけ滑らかに閉じ、それより内側では
最終密度を減らさない。基本形状の三次元ノイズが輪郭を決める範囲を広げつつ、物理層の端では密度を
確実にゼロへ戻す。

視線の空領域判定、詳細密度、追加テクスチャ採取をしない低詳細度密度、近距離と遠距離の光採取は、
すべて同じ層端用重みを使う。この変更は算術演算だけであり、テクスチャ採取回数を増やさない。

## 自己遮蔽の位相分散と環境光

一つの視線内で太陽方向の疎な積分をすべて同じ位相から始めると、採取誤差が同じ高さへ揃い、
遠近法によって横縞として見える。視線上の採取番号 `i` と黄金比の小数部 `φ` を使い、光採取の
開始位相を `j_i = frac(j_view + i φ)` とする。192 個の視線採取では 16 個の位相区画へ各 11〜13 個が
入り、特定の高さへ偏らない。光円すいの8方向は、この開始位相から従来どおり黄金角で回す。
参照描画では `j_view` を区間中央へ固定するため、時間平均を使わなくても同じ入力から同じ画像を得る。

## 太陽方向積分の区間中央採取

太陽方向の各採取区間は長さが指数的に増える。以前は区間終端の密度へ区間全長を掛けていたため、
散乱点の直近が濃く、終端が薄い雲縁では局所的な光学的深さを過小評価していた。区間内で密度が
一次的に変化する場合、終点法には傾きに比例する誤差が残るが、区間中央の密度へ区間長を掛ければ
積分値と一致する。

現在は進行量の半分だけ進めて密度を採取し、同じ半分をもう一度進めて区間終端へ到達する。
主描画の近距離3点と遠距離5点、影キャッシュを作る遠距離5点で同じ順序を使う。近距離3点の後に
影キャッシュを引く位置は従来と同じ区間終端なので、正確な経路とキャッシュ経路の境界はずれない。
密度の採取回数とテクスチャ採取回数は増えず、各採取点でベクトル加算が一つ増える。

## 光路密度と上層倍率の統一

自己遮蔽に使う密度は、視線密度と同じ被覆、高度、降水量、上層設定を使う。以前の遠距離5点と
影キャッシュは基本形状と高さ分布だけを使い、雲底から雲頂へ `1.10` から `0.92` へ変える密度補正、
降水域を最大 `1.28` 倍にする補正を落としていた。近距離3点から遠距離5点へ切り替わる位置で、
同じ雲の消散係数が別の式になるため、降水域の自己遮蔽を過小評価していた。

現在は高さ `h` と降水量 `r` による倍率を次式へまとめ、詳細侵食を使う視線・近距離経路と、
侵食を省く遠距離・影キャッシュ経路で共有する。

`S(h, r) = lerp(1.10, 0.92, h) * lerp(1.0, 1.28, r)`

上層では被覆倍率を密度の飽和処理より前、濃さ倍率を飽和処理より後へ適用する。この順序は既存の
視線密度と同じである。既定の上層設定では被覆が `0.55`、濃さが `0.30` なので、両方を落とした
光路は意図した上層密度を最大で約6倍に見積もり得た。両倍率は密度を確定する関数の内部だけで
適用し、呼び出し元や巨視的採取結果の構造体へ保持しない。光円すい全体へ一時値を持ち越さず、
追加のテクスチャ採取も発生しない。

環境光の遮蔽にカメラまでの透過率を使うと、同じ雲でも視点を動かしただけで空と地面反射の明るさが
変わる。現在は低詳細度の局所密度 `d` と正規化高度 `h` から、入射側の層境界までの光学的深さを
別々に求める。

`tau_sky = d * (0.35 + 0.65 * (1 - h))`

`tau_ground = d * (0.35 + 0.65 * h)`

`V = exp(-0.60 * tau)`

空の環境光には `V_sky`、地面反射には `V_ground` を掛ける。両者は高さを反転すると対称になり、
値は常に 0 より大きく 1 以下となる。カメラ方向の累積透過率はこの計算へ含めない。

## 高さ比率の未初期化警告防止

下層と上層の高さ比率は同じ関数で選択する。以前は各分岐から直接値を返していたため、式としては
全経路で値が決まるにもかかわらず、FXC がインライン展開後の戻り値を未初期化の可能性ありと判定した。
現在は下層の式で局所変数を初期化し、上層の場合だけ同じ局所変数を上書きして、一つの経路から
範囲を 0〜1 に収めて返す。下層と上層の式、選択条件、範囲制限の順序は変更していない。

単体試験は層外、層端、層内の代表高度で従来の条件式と結果が完全に一致することを検査する。
固定カメラの地平線・天頂実行では、以前の `heightFractionFromAltitude` に対する未初期化警告が
どちらも出ないことを確認する。

## 採取間隔による侵食帯域の制限

侵食用の3次元テクスチャは一つの詳細度だけを持つ。最も細かな成分は約20〜30m、低周波成分でも
約100mの周期なので、地平線方向を数百m間隔で積分しながら詳細度0を読むと標本化できず、時間的な
ちらつきや筋へ変わる。カメラからの距離だけでは、上向きの長いレイと短いレイを区別できないため、
現在は視線レイの実際の細密刻み幅を使う。

刻み幅が10m以下なら従来の侵食を保ち、10〜48mで高周波成分と侵食量を滑らかに減らし、48m以上では
基本形状へ戻す。最後の端数区間だけ細部が再出現しないよう、各積分区間の長さではなくレイ全体で
一定の刻み幅を使う。侵食寄与が消えたレイでは、二つの3次元テクスチャ採取も分岐前に省く。
近距離3点の光採取は従来どおり完全な侵食を使い、自己遮蔽の形は変えない。

## 粗密切り替え後の視線採取位相

空領域では粗い間隔で密度の有無を調べ、密度を見つけると一つ前の粗い区間へ戻って細かく積分する。
以前は粗い採取点の位相をそのまま細密積分へ持ち込んでいたため、粗い刻みが細密刻みの2倍となる
代表的な条件では、参照描画の位相 `0.5` が細密区間の境界へ変換されていた。これは「区間中央を使う」
基準画像の契約と一致せず、時間平均を無効にした画像へ高さ方向の積分誤差を固定する。

現在は、粗い採取点から一つ前の区間始点を求め、その位置へ `jitter * fineStep` を加えて細密積分を
始める。参照描画では各細密区間の中央、通常描画ではフレームごとの乱数位相になる。粗い空領域判定、
採取上限、密度評価式は変えず、粗密切り替え時の算術演算だけを変更するため、テクスチャ採取回数は
増えない。

## 動的な自己遮蔽キャッシュ

各視線標本から太陽方向へ採取する8点のうち、侵食の影響が大きい近距離3点は従来どおり正確に
積分する。遠距離5点は、`96 x 32 x 96` の `RG16F` 3次元テクスチャへ先に積分した光学的深さを
利用する。保存量は二つの採取模様から求めた平均深さと差であり、差がしきい値を超える場所、
キャッシュ範囲外、上層雲では遠距離5点も正確な積分へ戻す。

密度場は風移流とは別に対流変形するため、キャッシュは毎フレーム一度だけ現在の時刻で作り直す。
以前の二段目は第一段の値を複写するだけで空間勾配を品質判定へ反映していなかったため廃止した。
これによりGPU上の3次元テクスチャは2個から1個へ、生成ディスパッチも2回から1回へ減る。
固定時刻の参照画像では、正確な8点積分に対する相関係数 `0.9992`、平均二乗誤差の平方根 `0.607`、
平均輝度差 `0.071` で、横方向の帯状誤差は約17%減少した。重い参照描画でのGPU時間も
約 `68.5 ms` から `60.0 ms` へ短縮したため、品質と速度の両方を満たす構成として有効化する。

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

最初の3個の光標本は細かな侵食を保つ。残り5個も位置、高さ分布、しきい値を変えず、影キャッシュが
利用できない場所では同じ式で正確に積分する。利用できる場所だけ、毎フレーム生成した遠距離の
光学的深さへ置き換える。標本数や近距離の品質を減らす最適化ではない。

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

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
- 定常フレームは視線積分、時間再構成、雲内部の影キャッシュ、3D受光面用ワールド雲影の
  計4回の計算ディスパッチと、合成描画1回だけであり、一度限りの雑音生成が計測区間へ
  混入していないこと。影テクスチャ自体は `96 x 32 x 96` と `256 x 256` を保ち、安定時の
  1回のディスパッチでは4位相のうち `48 x 32 x 48` と `128 x 128` を更新した処理量として
  記録する。
- 有効呼び出し数、端数を含む起動スレッド数、視線・光・ワールド雲影の最大試料数が、
  各内訳と合計で一致すること。
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

通常描画では、各細密区間の長さと密度の重みを保ったまま、区間内の採取位置だけを
`p_i = frac(p_0 + i * (sqrt(2) - 1))` でずらす。同じ相対位置を全区間で採ると、地平線で広がった
刻み幅が基本形状の周期と一致した際に、同じ密度だけを繰り返し拾って細かな粒や縞になるためである。
192個の位相は16区画へ各11〜13個が入り、採取数やテクスチャ取得数を増やさずに共振を分散する。
参照描画は比較画像を決定的に保つため、この分散を使わず全区間の中央`0.5`へ固定する。

`Rendering/CloudReferenceMode=true`は内部描画を等倍へ固定し、時間再構成を無効にして、視線レイの
刻み上限を512へ増やす。通常C++利用側では`CVolumetricClouds::SetReferenceMode(true)`が同じ役割を
持つ。刻み上限の7/8を実積分、1/8を空領域から細密領域へ戻る余裕へ使うため、通常192刻みは168分割、
参照512刻みは448分割になる。参照描画では時間平均を行わないため、視線と光の積分位置を区間中央へ
固定し、毎フレーム変わる未平均の粒状誤差を基準画像へ混ぜない。`CloudRenderScale=4.0`でも乱れが残り、参照描画だけで消える場合は
レイ積分が原因である。参照描画でも残る場合は、密度場または照明式を監査する。どちらも常用時の
性能基準ではなく、原因を分離するための診断設定として扱う。

Editor の雲負荷契約は、通常描画の `trace pixel数 * 192` と、全解像度かつ時間再構成なしの参照描画に
限った `trace pixel数 * 512` を別々に受理する。最大光採取数は、受理した実際の最大視線採取数へ8を
掛けて検証する。これにより参照描画を未知の負荷として誤って拒否せず、256段など契約外の値は拒否する。

時間履歴には地平線の部分被覆を掛ける前の雲色、不透明度、代表深さを保存する。地平線被覆は
通常合成と大気対応合成の最終画素で一度だけ求め、不透明度へ適用する。被覆を履歴へ保存した後で
次フレームにも掛けると、被覆を`c`とした履歴が`a_n = a_0 * c^n`で減衰する。16段階再構成のうち
15回が履歴だけになる画素では、`c = 0.5`が`0.5^15`まで消えてから実標本で戻り、地平線に周期的な
欠けを作るためである。既存の全解像度画素中心と横・縦の隣接視線から求める解析的被覆を後段へ
移すだけなので、視線積分数とテクスチャ取得数は増えない。画素中心の同次遠点は全画面三角形の
三頂点で逆射影して線形補間し、横・縦の隣接遠点は逆射影行列の対応行から求める。したがって、
全画素で逆射影行列を掛け直す費用も追加しない。

## 遠方座標での視線復元

空と雲の視線方向は、カメラ位置を含むビュープロジェクション行列を反転した後で
カメラ位置を引いて求めてはならない。例えばカメラが `(64000, 1, 96000)` にある場合、
遠方平面のワールド座標とカメラ位置はどちらも大きな値になる。単精度で両者を保持してから
差を取ると、差に必要な下位桁が既に失われ、隣接画素の視線が同じ値へ丸められる。この誤差は、
雲では大きな格子模様、通常の空では細い四角形の輪郭として現れる。

`BuildCameraRelativeInverseViewProjection()` は、ビュー行列から平行移動を除き、回転と投影を
合成してから反転する。シェーダーはこの行列で得たカメラ相対座標をそのまま正規化し、密度場を
採取するワールド位置が必要な箇所だけ、最後にカメラ位置を加える。処理順は次のとおりとする。

`視線 = normalize(逆(平行移動なしビュー * 投影) * クリップ座標)`

`ワールド位置 = カメラ位置 + 視線 * 視線距離`

ビュー行列を作る前にも同じ注意が必要である。既知の向きを `注視点 = eye + direction` へ変換すると、
遠方では加算時点で方向の下位桁が失われる。`CCamera::SetLookDirection()` は原点で回転を作ってから
平行移動を加える。エディターの視点、シーンカメラ、通常C++の自由カメラ、軌道カメラは、向きを
既に持っている場合にこの入口を使う。

同じ契約を、雲だけでなく環境空、物理大気、局所霧にも適用する。物理大気の体積表生成は
揺らぎなしのカメラ相対逆行列を使い、深度から表面距離を復元する合成と解析的な太陽円盤は、
深度を書いたTAAの画面揺らぎを含むカメラ相対逆行列を使う。シェーダーは復元した相対位置の
`length()`を直接使い、大きなワールド位置からカメラ位置を引かない。

既存の `CVolumetricClouds::RenderCompute()`、`CSkyAtmosphere::BuildAerialPerspective()`、
`CompositeAerialPerspective()`、`CompositeLocalFog()`、`CImageBasedLighting::DrawSkybox()`、
`DrawEnvSkybox()` は互換性のため残す。遠方座標を扱う新しい呼び出し元は、それぞれの
`CameraRelative`入口へカメラ相対逆行列を明示的に渡す。エディター、通常の`CSky`、
`ALegacyScene3DAdapter`は同じ補助関数を使い、実装ごとの計算差を作らない。

物理大気と局所霧は別の体積表へ焼く。物理大気だけの処理では高さ霧を、局所霧だけの処理では
Rayleigh散乱、Mie散乱、大気表参照を分岐で省く。現在の高さ霧はX/Z方向に一様なので、同じ向きと
高さのまま水平移動しても再生成しない。物理大気もカメラ高度を別の入力で持つため、水平位置を
再生成キーへ含めない。品質の採取数や体積表解像度を下げず、無効な媒質の計算だけを除く。

時間履歴の再投影でも、大きなワールド位置を一度作ってから前カメラ位置を引いてはならない。
現在の雲までの相対位置へ「現在カメラ位置－前カメラ位置」と風の逆移流量だけを加え、前フレームの
カメラ相対位置を直接作る。

`前相対位置 = 現在視線 * 雲距離 + (現在カメラ位置 - 前カメラ位置) - 風移動量`

この位置を前フレームのカメラ相対ビュープロジェクション行列へ掛ける。深度整合性も
`length(前相対位置)` で判定し、ワールド座標同士の差を使わない。

2026-08-21 の目視検査では、参照描画、画面内雲解像度 `864 x 438`、視線採取数 `512`、
時間再構成なしの条件で確認した。原点ではなく `(64000, 1, 96000)` へカメラを置いた天頂視点で、
修正前に再現していた雲の格子模様と空の四角形の輪郭がともに消えた。通常描画でも16段階の時間再構成を
収束させた後、同じ遠方座標の天頂と地平線で輪郭状の履歴破綻や全面黒化がないことを確認した。
単体試験では、向きと投影が同じ原点カメラと遠方カメラから得るカメラ相対逆行列が許容誤差内で
一致することに加え、空、雲、大気、霧、環境光、時間再構成、深度合成、エディター、通常C++
アダプターの全経路が同じ契約を使うことを固定する。軌道カメラは原点と遠方で同じ角度を与えた時の
カメラ相対行列が一致すること、水平移動では物理大気と高さ霧の生成回数が増えないことも検査する。

2026-08-22 の上空比較では、被覆 `0.60`、遠方座標 `(64000, *, 96000)`、表示寸法 `864 x 438` を固定した。
通常描画は `216 x 110`、192段、時間再構成ありで雲GPU平均 `4.51 ms`、参照描画は等倍、512段、
時間再構成なしで平均 `39.08 ms` だった。参照描画でも白く平坦な雲頂が残り、局所霧と空気遠近法を
無効にした通常描画でも残った。この条件では、採取数、時間再構成、後段の媒質合成ではなく、密度場と
照明の釣り合いを次に監査する。

同日の通常品質計測では、内部寸法倍率 `0.25`、視線採取数 `192`、光採取数 `8`、時間再構成ありの
同一条件で品質判定を通過した。雲処理時間は地平線が平均 `2.14 ms`、最大 `3.21 ms`、天頂が平均
`1.05 ms`、最大 `1.70 ms` であり、GPU計測から換算した平均処理能力はそれぞれ `456.17 FPS` と
`778.36 FPS` だった。エディター全体の実測更新頻度に対する `300 FPS` 目標は、CPU処理と画面更新の
待機も含むため参考値として未達だったが、雲品質・処理量・GPU計測の受け入れ条件はすべて通過した。

同じ生成配布物を使い、単一ヘッダーを直接利用する通常の C++ 実行ファイルと、外部 ACS Framework
本体を Debug・Release でビルドして実行した。Framework の単体試験は両構成とも `538` 件中
`0` 件失敗であり、エディター専用経路に依存していないことも確認した。

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

従来はこれらの形状位相を動かしても、雲が存在するかを決める天候被覆は固定だった。
現在は、天候のゆがみと雲種をそれぞれ `[-1, 1]` の局所係数とし、形状用と細部用の時間位相と
組み合わせる。基礎被覆を `c` 、位相と局所係数から得た有界な変化を `p` とすると、変化後は次式となる。

`c' = saturate(c + clamp(p, -0.14, 0.14) * 16 c² (1 - c)²)`

`c = 0` と `c = 1` で変化量は厳密に 0 となるため、完全な空が一斉に曇ったり、濃い雲の中心が同期して明滅したりしない。
変化が最大になるのは `c = 0.5` の雲縁であり、地点ごとの係数の符号が異なるため、同じ時刻に成長する縁と消散する縁が共存する。
変化後の天候値を占有判定、詳細密度、光方向密度のすべてで共有するため、空間省略と描画結果が食い違わない。
計算は既に採取した値への算術演算だけで、天候テクスチャの採取回数は増えない。

時間再構成は 0.25 秒を超える時刻飛びで履歴を無効化する。Ultra では各出力画素の実標本が
16 フレームに 1 回だけ更新されるため、静止視点で現在値の重みを常に `0.125` にすると、成長または
消散した雲縁まで長く過去形状へ引かれる。現在被覆と履歴被覆の差を `d` とし、静止視点の現在値の
重みを次式で求める。

`w = lerp(0.125, 0.70, smoothstep(0.025, 0.18, d))`

`d <= 0.025` の小さな採取誤差は従来どおり長く平均し、明確な被覆変化だけを最大 `0.70` で現在形状へ
追従させる。カメラ移動中は従来どおり `0.70` を使う。履歴色と深さは既に読み込み済みなので、
この判定は算術演算だけであり、テクスチャ採取回数を増やさない。太陽方向深さキャッシュは、移流だけを
鍵にして古い密度を再利用せず、現在の対流位相を含む密度場から毎フレーム生成する。

## 雲底と雲頂の対流形状

天候テクスチャの雲種は、周期の異なる二領域を混ぜた後で高さ形状の選択値へ変換する。従来は
`smoothstep(0.34, 0.58, type)` を使っていたが、決定的な生成式を 50 万地点で監査すると、混合後の
中央値は約 `0.541`、10～90 パーセンタイルは約 `0.433～0.637` だった。この分布へ従来式を適用すると
約 60% が積雲側の確定範囲に入り、約 31% は値 `1` に完全飽和した。層雲、層積雲、積雲を使い分ける
という実装意図に反し、広域がほぼ同じ高い形状になっていた。

現在は実分布に合わせて `smoothstep(0.42, 0.66, type)` とする。中央値付近は約 `0.50` となり、全地点の
約 26% が層雲側、約 23% が積雲側、残りが連続した中間形状になる。値域と単調性を保ち、既に採取した
天候値への算術演算だけを変えるため、テクスチャ採取回数は増えない。この修正は雲種ごとの高さ差を
復元するものであり、基礎形状そのものの大きな雲塊を保証するものではない。

2026-08-21 の20秒計測では品質判定を通過し、地平線視点は平均 `3.29 ms`、最大 `4.09 ms`、天頂視点は
平均 `3.25 ms`、最大 `4.37 ms` だった。最大視線採取数 `192`、光採取数 `8`、定常時の雲描画投入数 `2` は
変更前と同じである。雲量 `0.50` の画面比較では高さ形状の種類が戻った一方、密な天頂領域では旧変換と
新変換の両方にワールド XZ 軸へ固定された矩形状の基本形状が残ることも確認した。この既存欠陥は
雲種変換とは分離し、基本形状の生成・採取品質として別に修正する。

雲種ごとの高さ分布を正規化高度へそのまま適用すると、同じ雲種が続く広域で密度の境界も同じ
高さとなり、雲底と雲頂が横に平らな層として見える。以前の高さ変形は `h²` を含んでいたため、
雲底へ近づくほど柱ごとの差が二次的に消え、雲頂だけを直しても下端の水平線が残っていた。
現在の密度評価は、既に取得した天候値から柱ごとの高さ変形量を求め、雲層の内部全体へ適用する。
被覆の強い中心は持ち上げ、薄い縁は押し下げる。以前は生の天候値を固定範囲
`smoothstep(0.38, 0.74, weather.r)` へ入れていた。しかし既定雲量 `0.5` で実際に残る天候値は
およそ `0.625` 以上なので、見える柱の大半が同じ中心側となり、説明とは逆に広い雲頂を作っていた。
現在は最終密度と同じ被覆補間値 `m` を使い、`smoothstep(0.08, 0.92, m)` で雲の境界から
中心までの位置を求める。空領域判定用に広げた被覆は柱形状へ使わないため、雲量を変えても
見える縁は低く、中心は高くなる。後段の密度も同じ `m` を再利用し、テクスチャ採取と被覆計算を増やさない。
層雲では変形量を小さく、積雲または降水域では
最大 `0.18` まで広げる。時間位相を全地点へ同じ符号で加えると、高被覆時には雲面全体が同期して
上下する。そこで、既に採取した低周波のゆがみと雲種をそれぞれ `[-1, 1]` の連続した空間係数へ
変換し、周期の異なる二つの時間位相との内積を柱ごとの局所位相にする。ある地点が上昇中でも別の
地点は下降できるため、無風時にも雲体が一様に呼吸せず、局所的に盛り上がりと沈み込みが進む。

正規化高度 `h`、柱の変形量 `s`、上層倍率 `b` に対する変形後高度は次式とする。

`h' = saturate(h - s * 4 h (1 - h) * b)`

下層では `b = 1`、薄い上層では `b = 0.30` とする。この式は `h = 0` と `h = 1` を固定するため、
物理層の上下端を越えない。導関数の下限は `1 - 4 |s| b` であり、`|s| <= 0.18` では下層でも
`0.28` 以上となる。このため区間全体で単調増加し、高さが折り返して同じ柱に不連続な密度面を
作らない。変形後高度は高さ分布だけでなく基本形状の三次元採取座標にも使うため、輪郭だけを
切り取る処理ではなく雲体全体が縦に伸縮する。

目視確認では、雲量 `0.85` の入力は広い曇天として残り、雲量だけを `0.50` にした一時複製では
空の抜けと柱ごとの高低差を確認できた。一方で輪郭には薄く流れた部分がまだ多く、今回の修正だけで
大きな積雲塊の品質を達成したとは扱わない。次の形状改善では、この被覆境界を保ったまま大域形状と
雲種ごとの隆起量を監査する。

この変形が対象とするのは雲体内部の密度境界である。曲面雲層そのものが遠方で地平線へ収束して
生じる画面上の境界は物理層の交差結果なので、この式では移動させない。

視線密度で求めた雲種、降水量、柱の変形量は、採取間隔に応じて侵食帯域を制限する近距離3点へ共有する。
遠距離5点は毎フレーム作る影キャッシュが各地点の天候、渦、柱形状を採取する。キャッシュを
利用できない場所も、視線位置の値を既定の2500 m層で最大約2.05 km先まで流用せず、各光標本の天候、雲種、降水、
柱高、基本形状を再評価する。渦は基本形状の座標を最大44 mしか移動させないため視線標本から共有し、最小約315 mの天候模様は
必ず地点ごとに採取する。通常のキャッシュ採用経路ではテクスチャ採取回数を増やさない。単体試験は層の両端、値域、全許容変形量での単調性、上層の抑制、および近距離共有と遠距離
再採取の境界を検査する。局所位相は算術演算だけで求めるため、視線密度の天候採取回数は増えない。
時刻 0 では二つの時間位相がともに 0 となり、従来の静的な柱形状と完全に一致する。

## 高さ分布の二重適用防止

雲種ごとの高さ分布は、基本形状を雲として採用するしきい値へ反映する。この値を最終密度へも
そのまま掛けると、同じ高さ変化を二度適用し、地平線に近い視線では層境界が水平な濃淡の段として
強調される。現在は、しきい値用の重みが `0.12` に達するまでだけ滑らかに閉じ、それより内側では
最終密度を減らさない。基本形状の三次元ノイズが輪郭を決める範囲を広げつつ、物理層の端では密度を
確実にゼロへ戻す。

視線の空領域判定、詳細密度、追加テクスチャ採取をしない低詳細度密度、近距離と遠距離の光採取は、
すべて同じ層端用重みを使う。この変更は算術演算だけであり、テクスチャ採取回数を増やさない。

## 基本形状の分散維持

基本形状は周期と向きの異なる四つの3次元領域を採取する。以前は各領域を
`0.45 : 0.27 : 0.17 : 0.11` の正値加重平均にしていた。独立した値を平均すると模様の反復は弱まるが、
形状の分散も同時に失われる。各領域を0〜1の16段階で全組み合わせした検査では、平均値は`0.5`のまま、
分散が単一領域より小さい`0.0298822`まで低下していた。このため空と雲の境界が狭い値域へ集まり、
上空から見た大きな雲塊が平らな濃淡へ潰れていた。

現在は第1領域を主形状としてそのまま残し、第2〜第4領域を平均0の揺らぎとして
`0.30 : 0.18 : 0.10`で加える。光路用の三領域版も同じ考え方で`0.30 : 0.18`を加える。
全組み合わせ検査では平均値`0.5`を維持しながら分散が`0.0967626`となり、旧平均の約`3.24`倍へ戻る。
追加のテクスチャ採取はなく、未採取領域が加えられる正の最大値`0.29`、`0.14`、`0.05`と
`0.24`、`0.09`を使う早期棄却も、最終密度がしきい値を越える標本を捨てない。

同じ上空カメラで通常描画と、等倍・512段・時間再構成なしの参照描画を確認した。大形状の濃淡は
現れたが、両方とも細かな房状の輪郭が不足しており、ぼけた面に見える問題が残った。旧式との改善幅は
上記の数値検査で保証し、見た目の完成をこの検査から推定しない。侵食係数だけを強めた比較にも
視認できる改善がなかったため、その調整値は採用していない。この修正を超高品質な雲の完成とは扱わず、
細部密度と照明を分離して確認する次の監査を継続する。

## 細部侵食の処理順

上空の参照描画を、不透明度、全放射輝度、直接光、単散乱へ一時的に分離して確認した。
不透明度には大形状しかなく、全放射輝度と直接光はさらに平坦だった。詳細体積の分散だけを
約3倍へ戻す比較も、列積分後の不透明度を視認できるほど変えなかったため採用していない。

原因は詳細値ではなく適用順にあった。以前は基本密度を詳細値で侵食し、その後で天候被覆、
高さ形状、降水補正を掛けていた。基本密度が`1`の雲芯は侵食後も`1`のため、最後に掛ける
滑らかな高さ形状が雲頂を決め、詳細体積は実際の表面を変えられなかった。

現在は基本密度、天候被覆、高さ形状、降水補正から0～1の粗密度を先に確定し、その粗密度を
詳細値で侵食する。基本密度`1`、被覆`0.75`、高さ閉鎖`0.20`、詳細値`0.60`、侵食量`0.24`の
境界例では、旧順序の最終密度`0.15`に対して補正後は約`0.00701`となる。詳細を無効にした場合は
同じ粗密度へ戻り、追加のテクスチャ採取もない。上空の不透明度表示では空隙と不規則な雲頂輪郭が
現れることを確認した。一方、通常照明ではこの細部がまだ弱い。単一散乱だけの診断では空隙と
大域的な陰影が通常照明より明確になったため、多重散乱と環境光が表面コントラストを埋めている。
ただし、多重散乱へ空の遮蔽をそのまま掛ける比較と、既定係数だけを下げる比較は改善が小さく、
調整値を採用していない。照明の式と密度採取を分けて監査し、見た目だけで係数を決めない。

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

各層の密度積分は、層全体を基準光学的深さ `1.6` へ写す。厚さを `H` とした尺度は
`s = 1.6 / H` であり、一様密度なら `H * s = 1.6` となる。以前は上層にも下層の尺度を使っていたため、
下層 `2500 m`、上層 `900 m` の場合、上層全体の積分値が意図した `1.6` ではなく
`900 * (1.6 / 2500) = 0.576` まで減っていた。

現在は密度採取時に確定した上層・下層の判定を再利用し、視線の不透明度、近距離と遠距離の太陽光、
動的自己遮蔽キャッシュ、3D受光面用ワールド雲影の全経路で、その標本が属する層の尺度を選ぶ。
光円すいの基準間隔も同じ層厚から求める。上層用の尺度と間隔は CPU で一度だけ計算するため、
GPU の標本数、テクスチャ採取数、定数バッファーの大きさは増えない。層判定に使った高度も
巨視的密度標本へ保持し、密度式ごとに同じ高度を再計算しない。

2026-08-22 の実画面検査では、上層 `7400～9200 m` を有効にした通常 C++ の ACS Framework を
地上から5秒以上動かし、停止、明暗の跳ね、シェーダー生成失敗がないことを確認した。上層の
自己遮蔽は働くが、地平線付近には帯状の密度が残り、超高品質な雲の合格状態ではない。

上空で雲が欠落したように見えた最初の比較は、視点から約 `200 m` の狭い範囲だけを見ており、
偶然に雲の空隙へ入っていた。雲殻との交差区間と地面側の地平線制限を監査し、雲底より上では
地面側の打ち切りが無効になることを確認した。約 `10200 m` から数 km の範囲を見下ろす比較では
下層雲が表示されたため、上空描画の欠落とは判定しない。ただし Editor はまだ上層設定を公開して
いないため、上層設定の公開は別工程として残す。

環境光の遮蔽にカメラまでの透過率を使うと、同じ雲でも視点を動かしただけで空と地面反射の明るさが
変わる。また、太陽方向の細い光円すいから得た `tau_light` は直接光用の経路であり、空半球または
地面から届く拡散光の代表経路ではない。これを流用すると、太陽が高いほど全天空光まで直接影と
同じ形で失われ、厚い雲が一様な灰色へ沈む。

現在は低詳細度の局所密度 `d`、正規化高度 `h`、高次散乱で縮小した環境光用消散率
`sigma_ambient` から、各層境界までの鉛直経路を評価する。

`d = saturate(lowLodDensity * Density)`

`sigma_ambient = 0.60 * MultiScatterOcclusion * LightExtinction`

`tau_sky = d * (0.35 + 0.65 * (1 - h))`

`tau_ground = d * (0.35 + 0.65 * h)`

`V_sky = exp(-sigma_ambient * tau_sky)`

`V_ground = exp(-sigma_ambient * tau_ground)`

既定の `LightExtinction=5` と `MultiScatterOcclusion=0.28` では、飽和密度の空可視率は雲底で
約 `0.4317`、雲頂で約 `0.7453` となる。消散係数を含めつつ、高次散乱で方向を失った光を直接光と
同じ率では消さない。空の環境光には `V_sky`、地面反射には `V_ground` を掛け、太陽高度、
`tau_light`、カメラ方向の累積透過率は含めない。追加のテクスチャ採取は発生しない。

通常 C++ の ACS Framework で式だけを差し替えた比較では、雲底の過度な沈み込みは減ったが、
被覆 `0.68`、密度 `2.8`、上層雲有効の天候指定は引き続き空を灰色の連続層で閉じた。これは
散乱式ではなく曇天相当の設定である。晴天表示は被覆 `0.50`、密度 `2.1`、上層の被覆倍率 `0.22`、
濃度倍率 `0.12`へ分け、青空の隙間を保つ。一方、房状の雲頂と内部陰影はまだ不足しているため、
この環境光補正だけを超高品質な雲の完成とは扱わない。

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
現在は視線レイと近距離の太陽方向積分で、それぞれ実際の刻み幅を使う。

刻み幅が10m以下なら従来の侵食を保ち、10〜48mで高周波成分と侵食量を滑らかに減らし、48m以上では
基本形状へ戻す。最後の端数区間だけ細部が再出現しないよう、各積分区間の長さではなくレイ全体で
一定の刻み幅を使う。侵食寄与が消えた標本では、二つの3次元テクスチャ採取も分岐前に省く。

以前の光採取は、既定の2500 m層で第1～3区間が乱数位相により `13.50～24.00 m`、
`22.28～39.60 m`、`36.75～65.34 m` となるにもかかわらず、3区間すべてへ最高周波数と侵食量を
強制していた。第3区間の一部は48mを越えるため、採取できない細部が時間平均で平らな陰影や
ちらつきへ変わり得る。

現在の光採取は基準値を `0.0075 / layer.w`、区間成長率を `1.8` とする。既定層の第1～3区間は
`8.44～15.00 m`、`15.19～27.00 m`、`27.34～48.60 m` となり、各区間の実間隔で侵食を
滑らかに減らす。第4区間は最小位相でも `49.21 m` なので、遠距離5点は低詳細度密度へ移れる。
`layer.w` は `1.6 / 層厚` であり、単純な層厚の逆数ではない。旧文書と旧テストの約3.2kmという
値はこの係数を落として1.6倍へ誤算していたため訂正した。最大位相で8区間の終端は旧式の
`1991.55 m`から`2047.49 m`、最後の区間中央は`1592.00 m`から`1588.33 m`となる。標本数8と
遠方到達範囲を維持し、追加のテクスチャ採取を行わずに細部帯域と採取間隔を一致させる。

上空の単一散乱比較では、この補正だけで房状の細かな陰影が劇的に増えたとは判定できなかった。
したがって超高品質な雲の完成とは扱わず、採取不能な細部を除く正しさと時間安定性の修正として採用する。
通常照明へ戻した上空・地平線をそれぞれ5秒以上観察し、新しい横縞、粒状ちらつき、明暗の跳ねが
出ないことを確認した。一方、両視点とも雲は白く広い面へ潰れており、房状の雲頂と内部陰影は不足する。
次の照明監査では、多重散乱と環境光を同時に弱める係数調整ではなく、各項のエネルギー上限、遮蔽の
適用先、表面と内部の寄与を数値で分離する。

## 粗密切り替え後の視線採取位相

空領域では粗い間隔で密度の有無を調べ、密度を見つけると一つ前の粗い区間へ戻って細かく積分する。
以前は粗い採取点の位相をそのまま細密積分へ持ち込んでいたため、粗い刻みが細密刻みの2倍となる
代表的な条件では、参照描画の位相 `0.5` が細密区間の境界へ変換されていた。これは「区間中央を使う」
基準画像の契約と一致せず、時間平均を無効にした画像へ高さ方向の積分誤差を固定する。

現在は、粗い採取点から一つ前の区間始点を求め、その位置へ `jitter * fineStep` を加えて細密積分を
始める。参照描画では各細密区間の中央、通常描画ではフレームごとの乱数位相になる。粗い空領域判定、
採取上限、密度評価式は変えず、粗密切り替え時の算術演算だけを変更するため、テクスチャ採取回数は
増えない。

## 視線標本と担当区間の一致

乱数位相付きの位置をそのまま積分区間の始点として扱うと、位相より前の距離を失い、標本位置から
さらに半刻み先を代表深度として記録してしまう。参照描画のように位相が `0.5` の場合、区間
`[100, 110]` を刻み幅 `6` で積分すると、以前は位置 `103` と `109` から長さ `6` と `1` だけを積み、
全長 `10` のうち `7` しか消散へ反映していなかった。一様密度の代表深度も、本来の `105` ではなく
`106.5` へずれていた。

現在は位相付き標本位置 `t_s`、位相 `j`、刻み幅 `d` から担当区間の始点
`c = max(t_s - j d, t_0)` を復元する。担当長は `l = min(d, t_1 - c)` とし、末尾の端数区間では
標本位置を `c + j l` へ収める。上記の例では位置 `103` が長さ `6`、位置 `108` が長さ `4` を担当し、
全長と一様密度の代表深度がそれぞれ `10` と `105` に一致する。消散には担当長、深度モーメントには
実際の標本位置を使うため、通常描画の乱数位相でも距離を欠落させない。テクスチャ採取回数と採取上限は
増えない。

## 動的な自己遮蔽キャッシュ

各視線標本から太陽方向へ採取する8点のうち、侵食の影響が大きい近距離3点は従来どおり正確に
積分する。遠距離5点は、`96 x 32 x 96` の `RG16F` 3次元テクスチャへ先に積分した光学的深さを
利用する。保存量は二つの採取模様から求めた平均深さと差であり、差がしきい値を超える場所、
キャッシュ範囲外、上層雲では遠距離5点も正確な積分へ戻す。

退避経路は各遠距離標本で天候、高さ分布、基本形状を再採取する。以前は視線標本の被覆と
柱形状をそのまま使っていたため、光路が天候模様の雲縁を跨いでも同じ密度が続き、キャッシュを
使えない場合だけ自己影が広い層へ潰れていた。追加採取はキャッシュが完全に採用された画素では
実行されない。渦の差による座標誤差は最大44 mに制限し、任意機能の初期化失敗時も天候境界を跨ぐ誤った密度固定へ戻さない。

密度場は風移流とは別に対流変形するため、キャッシュは毎フレーム更新する。初回、履歴無効、
影の基準格子または投影地図の移動、参照描画では全体を更新する。連続した安定フレームでは
XZまたはXYの偶奇位置を4位相で巡回し、各体積画素・画素を遅くとも3フレーム前までの密度へ
更新する。1フレームの有効呼び出し数は全更新の4分の1になるが、テクスチャ解像度、各地点の
密度採取数、光路長、密度式は変えない。描画を送信できなかった場合は両方の影を無効化し、
次の正常フレームで全体を更新する。

2026-08-21の20秒計測では、同じ入力、同じ表示寸法、同じ視線・光の採取数を保ったまま品質判定を
通過した。雲のGPU平均時間は、全更新時の地平線視点5.40 ms、天頂視点6.58 msから、四位相更新時の
地平線視点3.43 ms、天頂視点3.96 msへ短縮した。短間隔の連続画像では、斜め格子成分の差が画面全体の
変化量の0.33%であり、格子状のちらつきは目視でも確認されなかった。

同日の柱境界修正後の20秒計測も品質判定を通過し、地平線視点は平均 `3.404 ms`、最大 `4.467 ms`、
天頂視点は平均 `5.096 ms`、最大 `6.065 ms` だった。最大視線採取数、最大光採取数、定常時の
ディスパッチ数は修正前と同じで、テクスチャ採取も追加していない。天頂側の増加は、以前ほぼ同じ
高さへ潰れていた中心柱を被覆境界からの位置に応じて持ち上げ、実際に積分される密度領域を戻した
結果として記録する。全更新時の `6.58 ms` は下回るが、今後の大域形状改善ではこの値を基準に
不要な占有領域を増やさない。

以前の二段目は第一段の値を複写するだけで空間勾配を品質判定へ反映していなかったため廃止した。
これによりGPU上の3次元テクスチャは2個から1個へ、生成ディスパッチも2回から1回へ減る。
固定時刻の参照画像では、正確な8点積分に対する相関係数 `0.9992`、平均二乗誤差の平方根 `0.607`、
平均輝度差 `0.071` で、横方向の帯状誤差は約17%減少した。重い参照描画でのGPU時間も
約 `68.5 ms` から `60.0 ms` へ短縮したため、品質と速度の両方を満たす構成として有効化する。

Editor と従来の Scene3D 経路は、描画処理を開始する前に現在の場面から雲照明を更新する。
雲層中央の正規化高度で大気の RGB 透過率を評価し、上下方向の空照明勾配には現在の天頂色、
地面からの反射には現在の下半球色を使う。これにより、太陽が低いときも昼間の白い直接光が
雲へ残る問題と、雲底と雲頂が同じ地平色の環境光を受ける問題を防ぐ。

方向光の近似では、現在地点の密度と区間不透明度が一次散乱の発生量を既に制限する。
二次・三次散乱は周囲にも散乱源が必要なため、低 LOD 密度 `d` と層内の正規化高さ `h` から
周囲散乱源の確率を求め、その係数を高次散乱だけへ適用する。

`pDepth = saturate(0.05 + pow(d, lerp(0.5, 2.0, saturate((h - 0.30) / 0.55))))`

`pVertical = pow(lerp(0.10, 1.0, saturate((h - 0.07) / 0.07)), 0.8)`

`fSurround = lerp(1, pDepth * pVertical, PowderStrength)`

`S = exp(-tau) * phase0`

`  + fSurround * (`

`      a * exp(-b * tau) * phase1`

`    + a^2 * exp(-b^2 * tau) * phase1)`

`PowderStrength` は互換性のため名前を維持するが、現在の意味は `[0, 1]` の混ぜ率である。
この係数は入射光を増幅しない。低 LOD 密度は、最終的な天候被覆と高さのしきい値を用いて、
すでに採取した基本形状から求める。空間を広めに取る空領域判定は流用せず、テクスチャ採取も
増やさない。以前は `fSurround` を一次散乱へ掛け、周囲媒質を必要とする二次・三次散乱を
雲縁でも全量加えていた。この順序では方向性を持つ表面光だけが失われ、等方に近い内部光が残る。
光学的深さ `tau=2`、一次位相 `0.4`、高次位相 `1.0`、既定の `a=b=0.28`、疎な雲頂の
`fSurround=0.715` では、旧順序の合計 `0.26567` に対して補正後は `0.21641` となる。
密な領域では `fSurround=1` のため結果は変わらない。以前の任意な近距離光標本、
`edgeBoost`、および `1.08` のエネルギー増幅は使わない。

`MultiScatterContribution` を `a`、`MultiScatterOcclusion` を `b` とする。実行時の正規化で
`0 <= a <= b <= 1` を保証するため、二次の `a <= b` だけでなく三次の `a^2 <= b^2` も成立し、
縮小後の散乱係数は消散係数を越えない。高次散乱は方向を失う近似として、二次と三次で同じ
有界な位相を使う。これは三次までの有界な近似であり、完全な多重散乱解ではない。
係数縮小と次数加算は Frostbite の
[SIGGRAPH 2016講義資料](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016-pbs-frostbite-sky-clouds-new.pdf)
に従い、同資料の比較対象である `N=3` まで積算する。密度と高さによる周囲散乱源の分布は Guerrilla の
[Nubis SIGGRAPH 2017雲照明モデル](https://advances.realtimerendering.com/s2017/Nubis%20-%20Authoring%20Realtime%20Volumetric%20Cloudscapes%20with%20the%20Decima%20Engine%20-%20Final%20.pdf)
に従う。引き続き実用向け近似であり、完全な放射輸送を主張するものではないため、固定した
視覚参照の撮影を合格条件として維持する。

通常照明の上空視点と地平線視点をそれぞれ5秒以上観察し、新しいちらつき、白飛び、明暗の跳ね、
停止を起こさないことを確認した。ただし両視点とも広い雲面が白く均され、房状の細部と内部陰影は
まだ不足している。この補正だけを超高品質な雲の達成とは扱わず、上層を含む光学的深さの尺度と
環境光の寄与を引き続き分離して監査する。

## 水滴雲の単散乱アルベド

視線区間の不透明度 `a = 1 - exp(-density * step * ViewExtinction)` は、散乱と吸収を合わせた消散を
既に含む。以前は、この不透明度から発生する直接光へ `SunScatter=0.14` を掛けていた。この値を
単なる明るさ調整として小さくすると、消散した光の大半が吸収された扱いとなり、晴天用の雲量でも
雲塊全体が灰色の面へ沈む。

現在は `SunScatter` を消散に対する散乱の割合、すなわち単散乱アルベドとして扱い、可視光の
水滴雲に近い既定値 `0.92` とする。値は `0～1` に制限し、太陽付近の強い前方散乱は既存の位相上限と
自動露出で制御する。追加のテクスチャ採取や視線標本はない。

Editor の実画面では、約 `10200 m` からの見下ろしで暗い灰色の染みだった雲が白い雲塊として
地表から分離し、地上視点でも青空の隙間と白い雲頂を維持した。これは黒つぶれ対策ではなく、
晴天時に灰色の連続面へ見える問題の補正である。一方、地平線付近には参照描画の採取位置が揃うことで
生じる水平帯が残るため、次の工程で決定論を保ったまま画素ごとの採取位相を分散する。

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

最初の3個の光標本は採取間隔で侵食帯域を制限する。残り5個も位置、高さ分布、しきい値を変えず、影キャッシュが
利用できない場所では同じ式で正確に積分する。利用できる場所だけ、現在または直近3フレーム以内に
生成した遠距離の光学的深さへ置き換える。標本数、テクスチャ解像度、近距離の品質を減らす最適化ではない。

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

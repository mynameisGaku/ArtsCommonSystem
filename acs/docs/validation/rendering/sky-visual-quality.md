# 空の視覚品質

空の描画は、太陽円盤、光彩、地平線散乱、大気、雲との合成を分けて判定します。式と設定値のテストに加え、固定入力の線形 HDR を2回取得し、走査線の不連続、太陽円盤の消失、過大な発光面を数値で検出します。

## 太陽プロファイル

- 太陽円盤の角半径は `FSkySunProfile` の定義値を使用する。
- 光彩の広がりと強度は昼・夕方のプロファイルごとに有限範囲へ制限する。
- 地平線散乱を太陽円盤の代わりに使用しない。
- 最大輝度付近の画素が画面の広い範囲を占める場合は、太陽円盤ではなく過大な発光面として不合格にする。

`sky_sun_profile_tests.cpp` は `FSkySunProfile` の定義値、設定関数、昼・夕方のプリセット、角度から強度を求める式とシェーダー契約を検証します。`atmosphere_composite_tests.cpp` は大気環境値の有限性と非負性、入力変化への連続性、合成経路を検証します。これらだけでは実際の HDR 画像を合格にしません。

## 取得契約

`tests/fixtures/sky_visual_quality/scenarios.json` は次の条件を固定します。

- 描画寸法は `640x360`、固定時間差は `0.016666667` 秒、安定化は `20` フレーム、取得は `2` 回。
- `fallback_sky_sun_profile` のカメラと太陽設定を固定する。
- `CloudCoverage` は `0.002` とし、雲形状が太陽判定を妨げない状態にする。
- `CloudWind`、`Taa`、`AutoExposure`、`BloomIntensity`、大気遠近法、霧、光芒、粒状効果、モーションブラーなど、判定を変動させる効果を無効にする。

Editor ABI は HDR フレームと雲送信番号を対応付けるため、雲処理は同期用に実行されます。空の判定には雲形状を使用しません。

```pwsh
python .\scripts\capture_cloud_visual_quality.py `
  --editor-abi .\Intermediate\layout\dx12-release\Binaries\acs_editor_abi.dll `
  --config .\tests\fixtures\sky_visual_quality\scenarios.json `
  --output-directory .\Intermediate\sky-visual-candidate
```

承認済み基準画像と比較する場合は、そのマニフェストを指定します。

```pwsh
python .\scripts\capture_cloud_visual_quality.py `
  --editor-abi .\Intermediate\layout\dx12-release\Binaries\acs_editor_abi.dll `
  --config .\tests\fixtures\sky_visual_quality\scenarios.json `
  --output-directory .\Intermediate\sky-visual-comparison `
  --reference-manifest .\Intermediate\sky-visual-reference\cloud-visual-quality-manifest.json
```

出力先はまだ存在しないディレクトリを指定します。各回の RGBA16F、PNG、SHA-256、統計値、反復比較、基準比較を `cloud-visual-quality-manifest.json` へ記録します。

## 自動ゲート

`fallback_sky_sun_profile` は2回とも、共通の雲処理量ゲートと次の画像ゲートを満たす必要があります。

| 検査値 | 合格条件 | 検出対象 |
|---|---:|---|
| `maximumMeanAbsoluteRowLuminanceStep` | `0.04` 以下 | 画面を横断する水平の帯や不連続。 |
| `nearPeakLuminancePixelCount` | `4` 以上 | 太陽円盤の消失。 |
| `nearPeakLuminancePixelFraction` | `0.01` 以下 | 太陽円盤や光彩が過大な発光面へ広がる回帰。 |

最大輝度付近とは、その画像の最大線形輝度の `90%` 以上です。2回の表示用画像は RMSE `0.005` 以下、8x8 SSIM `0.995` 以上でなければなりません。数値ゲートの合格だけでは、円盤の見かけの大きさや輪郭の自然さを保証しません。

## 基準画像の承認

基準マニフェストは[共通の承認契約](README.md#基準画像の承認契約)に加え、`fallback_sky_sun_profile.result: "PASS"` を満たす必要があります。`referenceApproval.scenarioRawSha256` は `fallback_sky_sun_profile` だけを含み、その値には最初の取得 `captures[0].hashes.rawSha256` を指定します。

`acceptance` の `FAIL`、`HOLD_REFERENCE_NOT_REVIEWED`、`PASS` は、[描画品質検証の共通承認状態](README.md#基準画像の承認状態)に従います。

## 目視判定

基準画像の承認では次を確認します。

- 太陽円盤の見かけの大きさと輪郭が連続している。
- 円盤から光彩、大気、地平線散乱への遷移に段差や帯がない。
- 地平線との交差で円盤が欠けたり、別の発光面へ連結したりしない。
- 朝、昼、夕方の色と露出が各 `FSkySunProfile` に対して一貫する。
- カメラ移動で円盤、光彩、地平線の位置や輝度が不連続に変化しない。

同じカメラ、解像度、プロファイル、描画バックエンドで取得し、基準画像の承認単位を分けます。

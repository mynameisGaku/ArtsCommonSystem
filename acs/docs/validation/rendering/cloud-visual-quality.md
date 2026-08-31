# ボリューム雲の視覚品質

ボリューム雲は、固定入力から取得した線形 HDR、反復再現性、承認済み基準画像、目視判定を組み合わせて評価します。`captureGate` の合格は取得経路と数値ゲートの成立を示しますが、自然な形状、十分な厚み、時間方向の安定性まで保証しません。

## 取得契約

`scripts/capture_cloud_visual_quality.py` は Editor ABI の `hdr-frame-capture-v1` から、雲合成後かつトーンマップ前の完了済み HDR 描画先を同期して読み戻します。各取得では次を保存します。

- 線形色空間の RGBA16F 原画像。
- 表示確認用の PNG。
- フレーム番号、雲送信番号、処理量、統計値、SHA-256、反復比較、基準比較を含む `cloud-visual-quality-manifest.json`。

各反復は、新しく作成した非表示の子 `HWND` と Editor ホストを使用し、他の取得と描画履歴を共有しません。最小の空3Dシーン `ACS3D v2` を読み込み、設定で `CloudWind=0` を要求し、固定時間差で提示に成功した20フレームを進めてから取得します。反復ごとにホストを破棄するため、前回のカメラ、時間履歴、スワップチェーン内容を次の反復へ持ち越しません。

既定の `tests/fixtures/cloud_visual_quality/scenarios.json` は、`640x360`、固定時間差 `0.016666667` 秒、安定化 `20` フレーム、反復 `2` 回で次の3シナリオを定義します。

| シナリオ | 確認対象 |
|---|---|
| `horizon_noon` | 地平線付近の雲層、雲底、水平の不連続。 |
| `oblique_cloud_shape` | 斜め視点での大・中・小形状、自己影、奥行き。 |
| `forward_scattering_low_sun` | 低い太陽に対する前方散乱、内部透過、自己影。 |

## 実行

対象バックエンドの Release `acs_editor_abi.dll` を指定します。出力先は、まだ存在しないディレクトリでなければなりません。

```pwsh
python .\scripts\capture_cloud_visual_quality.py `
  --editor-abi .\Intermediate\layout\dx12-release\Binaries\acs_editor_abi.dll `
  --config .\tests\fixtures\cloud_visual_quality\scenarios.json `
  --output-directory .\Intermediate\cloud-visual-candidate
```

承認済み基準画像と比較する場合は、そのマニフェストを追加します。

```pwsh
python .\scripts\capture_cloud_visual_quality.py `
  --editor-abi .\Intermediate\layout\dx12-release\Binaries\acs_editor_abi.dll `
  --config .\tests\fixtures\cloud_visual_quality\scenarios.json `
  --output-directory .\Intermediate\cloud-visual-comparison `
  --reference-manifest .\Intermediate\cloud-visual-reference\cloud-visual-quality-manifest.json
```

各起動区間と安定化区間の上限は `--timeout-seconds` で `5..600` 秒に設定でき、既定値は `120` 秒です。GPUを使わない契約検査は次で実行します。

```pwsh
python .\scripts\capture_cloud_visual_quality.py --self-test
```

`--self-test` は `--editor-abi`、`--output-directory`、`--reference-manifest` と同時に指定しません。

## 自動ゲート

各シナリオの `captureGate` は次を検証します。

- 雲処理が試行され、GPUへ送信されている。
- HDR画像の寸法、フレーム番号、雲送信番号が対応する。
- 定常フレームが計算ディスパッチ `2` 回と合成描画 `1` 回である。
- 視線方向の最大標本数が `192`、各視線標本に対する光源方向の最大標本数が `8` である。
- 時間履歴が利用可能かつ再利用済みで、時間超解像が有効である。
- 線形 HDR に非有限値がなく、隣接行間の平均絶対輝度差の最大値が `0.04` 以下である。
- 2回の表示用画像比較で RMSE が `0.005` 以下、8x8 SSIM が `0.995` 以上である。

出力ファイルは新規作成だけを許可し、既存の原画像、PNG、マニフェストを上書きしません。取得中に `acs_editor_abi.dll` のバイト数またはSHA-256が変化した場合も失敗します。

## 品質モデルと判定範囲

現在の雲照明は、前方散乱と後方散乱の固定係数、手動の多重散乱近似、密度侵食を組み合わせた経験的なモデルです。物理現象を参考にしていますが、校正済みの放射輸送計算ではありません。固定係数による画像の再現性を、物理的な正しさの証明として扱いません。

既定の取得が自動判定できる範囲は、固定した `640x360` の3つの静止カメラ、既定の描画バックエンド、空3Dシーン、固定した雲設定です。次の項目は別の取得契約が必要です。

- カメラ移動、風、遮蔽解除、履歴無効化を含む連続フレームの残像、尾、ちらつき。
- 地上、雲上、雲中から見た厚み、密度遷移、形状の自然さ。
- 異なる描画バックエンド間の画像一致。
- 物理大気、遠景の大気遠近法、自動露出、ブルームを組み合わせた合成品質。
- 承認済み基準画像がない状態での自動画像回帰判定。

`captureGate` は取得経路、処理量、有限値、水平不連続、反復再現性を検査します。柱状形状、粒状ノイズ、十分な厚み、散乱の自然さは目視条件であり、`captureGate` の合格だけでは承認しません。

## 基準画像の承認

基準マニフェストは[共通の承認契約](README.md#基準画像の承認契約)に加え、3シナリオすべての `result: "PASS"` を満たす必要があります。`referenceApproval.scenarioRawSha256` のキー集合は次の3件と完全一致させ、各値には対応する最初の取得 `captures[0].hashes.rawSha256` を指定します。

- `horizon_noon`
- `oblique_cloud_shape`
- `forward_scattering_low_sun`

`acceptance` の `FAIL`、`HOLD_REFERENCE_NOT_REVIEWED`、`PASS` は、[描画品質検証の共通承認状態](README.md#基準画像の承認状態)に従います。

## 目視判定

基準画像の承認では少なくとも次を確認します。

- 地平線と惑星接線に直線状の切れ目、帯、穴がない。
- 雲に大・中・小の形状階層があり、柱状の反復やボクセル状・粒状ノイズがない。
- 雲底、雲頂、雲内部に十分な厚みと密度遷移がある。
- 低い太陽で輪郭だけが白い面にならず、内部透過と自己影が連続する。
- 太陽、影、大気、雲の方向関係が一致する。
- カメラ移動、雲移動、履歴無効化で残像、尾、ちらつき、明るさの跳びが出ない。

既定の3シナリオは静止画の地平線、斜め形状、低い太陽を対象とします。地上、雲上、雲中、移動中の連続フレームは、それぞれ同じ入力固定、由来記録、承認手順で追加して判定します。

## 実装変更時の確認

視線区間、標本間隔、密度、光源方向の標本化、影キャッシュ、時間方向の再利用を変更した場合は、式、実行順、CPU/HLSL 間の値、資源の割り当て、状態遷移、実際の GPU 読み戻しを同時に確認します。係数変更だけで欠陥を隠した結果は合格にしません。

性能判定は[ボリューム雲の性能検証](cloud-performance.md)で分離します。

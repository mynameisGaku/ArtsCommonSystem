# ボリューム雲の性能検証

`scripts/profile_cloud_quality.ps1` は、同一品質設定のボリューム雲を `horizon` と `zenith` の固定カメラで順番に描画し、処理量、CPU/GPU 時間、入力の由来を検証します。性能検証は実際の Editor 描画経路を使用し、画像品質の合否は[ボリューム雲の視覚品質](cloud-visual-quality.md)で別に判定します。

## 実行

`EditorExe` には `AcsEditor.exe`、`Project` には初期シーンが3Dの雲検証シーンである `.acsproject` を指定します。

```pwsh
$editor = ".\editor\AcsEditor\bin\Release\net10.0-windows\win-x64\AcsEditor.exe"
$project = "C:\acs-projects\RenderingShowcase\RenderingShowcase.acsproject"

pwsh .\scripts\profile_cloud_quality.ps1 `
  -EditorExe $editor `
  -Project $project `
  -SoakSeconds 30 `
  -Monitor secondary
```

`Monitor` は `secondary`、`primary`、`none` のいずれかです。非負の `MonitorIndex` を指定した場合は `Monitor` より優先し、対象の表示装置番号を Editor の `--monitor` へ渡します。

出力先を明示する場合は、現在のプロセスの `TEMP` 配下を指定します。

```pwsh
$output = Join-Path ([IO.Path]::GetTempPath()) "acs-cloud-quality-candidate"

pwsh .\scripts\profile_cloud_quality.ps1 `
  -EditorExe $editor `
  -Project $project `
  -OutputDirectory $output `
  -SoakSeconds 30 `
  -MonitorIndex 1
```

### 引数の範囲

| 引数 | 既定値 | 契約 |
|---|---:|---|
| `SoakSeconds` | `30` | 各カメラの測定時間。`5..600` 秒。 |
| `Monitor` | `secondary` | `secondary`、`primary`、`none`。 |
| `MonitorIndex` | `-1` | `-1..31`。非負なら `Monitor` より優先。 |
| `TargetFps` | `300` | 比較する目標値。`1..2000`。 |
| `StartupGraceSeconds` | `120` | Editor 起動猶予。`30..600` 秒。 |
| `RequireTargetFps` | 無効 | 目標未達を終了コード `1` に含める。 |

## 事前検査

`DryRun` は入力と出力先を検証し、`horizon` と `zenith` の正確な Editor コマンド、入力の識別情報、予定する要約パスを表示します。出力ディレクトリを作成せず、Editor も起動しません。

```pwsh
pwsh .\scripts\profile_cloud_quality.ps1 `
  -EditorExe $editor `
  -Project $project `
  -SoakSeconds 30 `
  -Monitor secondary `
  -DryRun
```

`SelfTest` は GPU を使わず、厳密な JSON 解析、値域、由来ハッシュの再確認、上書き拒否、子プロセスの終了コード、標準出力と標準エラーの同時回収、時間切れ時のプロセスツリー終了を検証します。子プロセス検査は `$PSHOME\powershell.exe` を使用するため、Windows PowerShell から実行します。

```pwsh
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\profile_cloud_quality.ps1 `
  -SelfTest
```

## 品質を維持する性能ゲート

各カメラは、次の条件をすべて満たす必要があります。

- Editor の報告結果が `PASS` であり、実測フレームと GPU 問い合わせ結果が存在する。
- 省略されていない3D表示で雲処理が試行され、GPUへ送信されている。
- 出力解像度と追跡解像度が整合し、実効追跡倍率が `0.25` である。
- 視線方向の最大標本数が `192`、各視線標本に対する光源方向の最大標本数が `8` である。
- 時間履歴が利用可能かつ再利用済みで、無効化されておらず、4x4の16位相を使う時間超解像が有効である。
- 定常フレームが計算ディスパッチ `2` 回と合成描画 `1` 回であり、一度だけの生成処理や影キャッシュ処理が混入していない。
- 論理呼び出し数、起動スレッド数、最大視線標本数、最大光源標本数が相互に整合する。
- GPU 時間、Editor のフレーム間隔、スケジューラー診断が有限値として揃う。

`horizon` と `zenith` の間では、解像度、品質設定、GPU 問い合わせ範囲、ディスパッチ数、時間超解像、最大標本処理量を照合します。カメラ変更によって品質または処理量を下げた結果は、性能改善として扱いません。

### ソースで固定する品質条件

処理量マニフェストだけでは密度積分の近似方法まで判定できないため、同品質の性能改善には次のソース契約と `volumetric_cloud_world_anchor_tests.cpp` の合格も必要です。

- 形状の早期棄却は `shape <= 0.006` の場合だけ行う。まだ評価していない形状ローブが持ち得る最大寄与を現在値へ加えてもしきい値を超えない場合に限り省略し、密度が存在し得る標本を捨てない。
- 光源方向の8標本のうち、最初の3標本は天候の大域形状と細部侵食を含む詳細密度を評価する。残り5標本は位置、雲層プロファイル、消散式、しきい値、3回の形状取得を維持したまま、大域形状の消散へ限定する。
- `kVolumetricCloudShadowCacheEnabled` は `false` のままとする。影キャッシュは、同じ固定品質の画像取得で品質を維持し、かつ GPU 時間が改善したことを示すまで有効化しない。
- 実効追跡倍率 `0.25`、最大視線標本数 `192`、最大光源標本数 `8`、16位相の時間超解像を変更した結果は、既存測定との同品質比較に使用しない。

## 成果物

出力先を省略すると、`TEMP` の下へ `acs-cloud-quality-<UTC>-<識別子>` 形式のディレクトリを作成します。明示した出力先も `TEMP` の子でなければならず、途中の再解析ポイントを拒否します。既存の成果物は上書きしません。

各シナリオには次の4ファイルを作成します。

- `<scenario>-soak-report.json`: Editor の測定報告。
- `<scenario>-profile.csv`: プロファイラーの測定値。
- `<scenario>-stdout.log`: Editor の標準出力。
- `<scenario>-stderr.log`: Editor の標準エラー。

全体の `cloud-quality-summary.json` には、`Result`、`FaultCodes`、`RunEnvironment`、`ProvenanceGate`、`QualityGate`、`TargetGate`、`QualityComparison`、`Scenarios` を記録します。`TargetGate.Result` は `MET` または `MISS` であり、`RequireTargetFps` を指定しない限り `MISS` は情報として残るだけです。

## 入力の由来

測定開始時に次の6入力を正規化し、パス、バイト数、更新日時、SHA-256を記録します。

- `AcsEditor.exe`
- `AcsEditor.dll`
- `acs_editor_abi.dll`
- `AcsEditor.deps.json`
- `AcsEditor.runtimeconfig.json`
- `.acsproject`

`AcsEditor.exe` と `AcsEditor.dll` にはファイル版と製品版も記録します。さらに、OS、実行環境、GPU、ドライバーの識別情報を `RunEnvironment` へ格納します。

6入力は測定中を通して読み取り専用で開き、書き込みと削除を共有しません。各シナリオの起動直前と終了直後、および全シナリオ終了後にSHA-256を再計算します。内容が変化した場合は `ProvenanceGate` を失敗させ、異なる実行ファイルやプロジェクトによる結果を同一測定として扱いません。

## 終了コード

| 終了コード | 意味 |
|---:|---|
| `0` | 品質ゲートが合格し、必要な場合は目標FPSも達成した。`DryRun` または `SelfTest` も正常終了した。 |
| `1` | 品質、由来、シナリオ間整合性のいずれかが不合格、または `RequireTargetFps` 指定時に目標未達となった。 |
| `2` | 入力、パス、環境、起動、解析などの設定・実行エラーが発生した。 |

PowerShell の引数結合時に型または値域が不正な場合は、スクリプト本体へ入る前に PowerShell が拒否します。

## 最適化の受け入れ条件

性能の改善候補は、同じ入力の由来、同じ `horizon` と `zenith`、同じ品質ゲートで再現でき、[ボリューム雲の視覚品質](cloud-visual-quality.md)を維持する場合だけ採用します。シェーダー命令数や単発のFPSだけでは合格にしません。

## 適用限界

この検証は、固定した `horizon` と `zenith` の定常描画に対する処理量と時間を比較します。地上、雲上、雲中、移動中の品質、履歴の離反と再収束、自然な雲形状、異なる描画バックエンドの同等性は判定しません。性能マニフェストの合格は、線形 HDR の視覚承認や表示装置への走査を意味しません。

# パフォーマンスプロファイラーのキャプチャー

ACS Editor は、画面の `Export CSV` と、`--interaction-soak` による無人キャプチャーを提供します。画面からの出力は選択した保存先を使います。このページの `TEMP` 制限は `--profiler-capture` にだけ適用されます。

画面に表示する指標と更新間隔は[パフォーマンスプロファイラー](performance-profiler.md)を参照してください。

## 無人キャプチャー

`--profiler-capture` は `--interaction-soak` と同時に1回だけ指定し、出力先を明示します。実行中は `Profiler` パネルを表示し、既存の100 ms間隔の標本取得を使用します。描画フレームごとの追加読取りは行いません。

```pwsh
$capture = Join-Path $env:TEMP "acs-profiler-capture.csv"
$report = Join-Path $env:TEMP "acs-profiler-soak.json"

AcsEditor.exe .\Game.acsproject `
  --secondary-monitor --unattended --show-profiler `
  --interaction-soak 30 --interaction-soak-report $report `
  --profiler-capture $capture
```

## 出力先の安全条件

無人キャプチャーの出力先は、次の条件をすべて満たす必要があります。

- 拡張子が `.csv` で、正規化後のパスが1024文字以下であること。
- プロセスの `TEMP` ルートより下にあり、親ディレクトリが既に存在すること。
- 親ディレクトリから `TEMP` ルートまでの全ディレクトリが通常のディレクトリで、再解析ポイントではないこと。
- 出力先が存在する場合は、ディレクトリ、再解析ポイント、デバイスではない通常ファイルであること。
- UTF-8へ変換したCSVが1 MiB以下であること。

公開時は、同じディレクトリに一意な一時ファイルを新規作成し、`FileOptions.WriteThrough` で書き込み、`Flush(flushToDisk: true)` でディスクへ反映します。移動直前に出力先とディレクトリ連鎖を再検証し、同じディレクトリ内の移動で既存CSVを置き換えます。検証、書き込み、ディスク反映、移動のいずれかに失敗しても既存CSVは保持し、ACSが作成した一意な一時ファイルだけを削除します。

## リセット境界とキャプチャー世代

キャプチャー開始時は、ネイティブ側のプロファイラー、管理側の履歴、描画スケジューラー、ログ取込み、`Dispatcher` 診断を同じ境界でリセットします。ネイティブ側は次を無効化または初期化し、`profiler_reset_serial` を進めます。

- 現在のCPU・GPU時間とGPU有効フラグ
- 平滑化FPSとボリューム雲の処理量
- リセット後の提示フレーム数
- 描画時間と `Present` 時間の最大値

ACS Editor は `profiler_reset_serial` をキャプチャー世代の識別子として扱います。リセット直後の値を読み取り、その値と一致する後続の提示済みフレームが到着するまで標本を受理しません。キャプチャーに含める全行は同じ世代で、`presented_frame_count_since_reset` が0より大きく単調増加し、最後のスナップショットが最後の行と一致する必要があります。境界を確定できない場合や世代が混在した場合は `PROFILER_CAPTURE_INVALID_RESET_BOUNDARY` として失敗します。

## `Dispatcher` のチェックポイント

提示に成功したフレームは、最大64フレームまたは有効CPU処理64 msの早い方まで連続できます。境界に達すると、次の描画メッセージを送る前に `DispatcherPriority.Input` のチェックポイントを1回通します。キーボードまたはポインター入力が待機している場合は、上限より前にチェックポイントへ移ります。

ネイティブ描画結果 `0` は GPU フレーム枠の待機を表します。この状態の直接再試行は最大256回または8 msの早い方までで、上限後は同じ `Dispatcher` チェックポイントを必須とします。待機中の試行は提示フレーム、シミュレーション時間、プロファイラー標本を進めません。

非表示起動中、または前回の保守処理から500 ms以上経過した場合は、`DispatcherPriority.Background` の保守処理へ継続権を渡します。これにより、入力優先のチェックポイントだけでは進まない起動完了処理とタイマーを実行できます。

## 保持範囲と判定

プロファイラー履歴は、重複しないネイティブフレームを直近120件まで保持します。無人キャプチャーはこの保持範囲を CSV へ出力するため、長い実行でも全フレームを保存するわけではありません。

利用できない時間値は CSV で `N/A`、JSON で `null` または `Available=false` として扱い、0 msへ変換しません。3D表示を要求したキャプチャーは、抑制されていない3D表示と0以外の描画またはディスパッチをスナップショットで確認できない場合に失敗します。ボリューム雲の処理量は、そのプロファイラーフレームがキャプチャーの先頭と末尾の範囲内にある場合だけ受理します。

## CSV の構成

無人キャプチャーの CSV はスキーマ版 `4` です。先頭のメタデータには、次を記録します。

- 保持標本数、先頭と末尾のネイティブフレーム番号、提示頻度の算出元。
- ネイティブ報告 FPS、Editor が観測する FPS、観測フレーム間隔の平均、P95、標本数。
- CPU フレーム、CPU 送信、ネイティブ有効 CPU、`Present` CPU の平均、P95、標本数と移動最大値。
- 個別 GPU 問い合わせの平均、P95、標本数、時間から換算した処理頻度。
- CPU パス別の平均、P95、標本数と、最新 GPU 問い合わせ窓の件数、容量、遅延、パス別平均・最大値。
- 最新の描画状態、ボリューム雲の処理量、Editor 描画スケジューラー、`Dispatcher` 診断、リセット世代の整合性。

続く標本行は、[画面からの CSV 出力](performance-profiler.md#画面からの-csv-出力)と同じフレーム番号、時刻、CPU/GPU 時間、パス別時間を持ちます。`EditorFps` は CSV 行の `native_reported_fps` を読み替えた値ではなく、`Stopwatch` の経過時間とフレーム番号差から要約時に算出します。

## `ProfilerSummary`

操作継続検査の JSON 報告には、同じ標本から作った `ProfilerSummary` を格納します。主な区分は次のとおりです。

- `SampleCount`、`FirstFrameIndex`、`LastFrameIndex`、`UsesObservedCadence`
- `NativeReportedFps`、`EditorFps`、`ObservedFrameIntervalMilliseconds`
- `CpuFrameMilliseconds`、`CpuSubmitMilliseconds`、`NativeRenderActiveCpuMilliseconds`、`NativePresentCpuMilliseconds`
- `GpuQueryMilliseconds`、`CpuPasses`、`LatestGpuQueryWindow`
- `LatestRenderState`: 2D/3D、`Game View`、シーン表示の抑制、雲・霧・大気遠近法、描画・ディスパッチ・三角形数、表示寸法、雲寸法と標本上限。
- `LatestCloudWorkload`: 送信、履歴、時間超解像、追跡・出力寸法、計算ディスパッチ、論理呼出し数、起動スレッド数、最大標本数。
- `LatestEditorRuntime`: ネイティブ呼出し、GPU 使用中の譲り、入力再試行、保守処理、描画公平性、`Dispatcher` の間隔と停止状態。
- `RuntimeTimeline`: フレーム進行とは独立して取得した実行時標本の要約。

各時間要約は `SampleCount`、`Average`、`P95` を持ちます。利用できない時間は `Average: null`、`P95: null`、`SampleCount: 0` とし、最新状態を取得できない区分は `Available=false` とします。

## 実行時タイムライン

実行時タイムラインは、ネイティブフレーム番号が進まない場合も100 ms間隔で直近120件まで取得します。CSV の `runtime_timeline_sample` には、次を記録します。

```csv
# runtime_timeline_header,frame_index,sample_timestamp,gpu_busy_yields,input_retries,background_fallbacks,ready_after_retry,fairness_yields,input_continuation_yields,maintenance_yields,busy_epoch_ms,continuation_queue_wait_ms,maintenance_queue_wait_ms,dispatcher_gap_ms,dispatcher_heartbeat_age_ms
```

- フレーム番号と `Stopwatch` の標本時刻。
- GPU使用中の譲り、入力優先再試行、低優先処理への切替え、再試行後の準備完了、描画公平性の各累積回数。
- 入力継続処理と保守処理へ制御を渡した累積回数。
- GPU 使用中区間の長さ、入力継続処理と保守処理の待ち時間、`Dispatcher` の間隔、心拍の経過時間。

これらをフレーム時間と分けることで、GPU の待ち、入力優先処理の遅れ、低優先処理の滞留を同じ原因として扱いません。

## 失敗コード

操作継続検査の `FaultCodes` は重複を除いて並べ替え、1件でもあれば報告結果を `FAIL` にします。プロファイラーキャプチャーが追加するコードは次のとおりです。

| コード | 意味 |
|---|---|
| `PROFILER_CAPTURE_FAILED` | 要約、直列化、またはキャプチャー処理で予期しない例外が発生した。 |
| `PROFILER_CAPTURE_WRITE_FAILED` | 出力先検証、一時ファイル書込み、ディスク反映、置換のいずれかに失敗した。 |
| `PROFILER_CAPTURE_NO_SAMPLES` | 同じリセット世代の提示済み標本がない。 |
| `PROFILER_CAPTURE_NO_GPU_SAMPLES` | 完了した個別 GPU 問い合わせがない。 |
| `PROFILER_CAPTURE_NO_GPU_PASS_WINDOW` | 最新の GPU パス別問い合わせ窓を取得できない。 |
| `PROFILER_CAPTURE_INVALID_RESET_BOUNDARY` | 世代、提示成功数、フレーム順、または移動最大値の境界が不整合である。 |
| `PROFILER_CAPTURE_NO_RENDER_STATE` | 最新の描画状態を取得できない。 |
| `PROFILER_CAPTURE_EXPECTED_VIEW3D` | 3D を要求した取得が3D表示ではない。 |
| `PROFILER_CAPTURE_SCENE_SUPPRESSED` | シーンの公開が抑制された状態である。 |
| `PROFILER_CAPTURE_NO_RENDER_WORK` | 描画回数とディスパッチ回数がともに0である。 |
| `PROFILER_CAPTURE_NO_EDITOR_RUNTIME_DIAGNOSTICS` | Editor のネイティブ呼出しとスケジューラー診断を取得できない。 |
| `PROFILER_CAPTURE_NO_CLOUD_WORKLOAD` | 雲が有効なのに、対応する送信済み処理量を取得できない。 |
| `PROFILER_CAPTURE_CLOUD_FRAME_OUTSIDE_CAPTURE` | 雲の処理量がキャプチャーの先頭・末尾フレーム範囲外である。 |

詳細な例外文字列は `ProfilerCaptureError`、機械判定用の要約は `ProfilerSummary` に分けて格納します。

## 検証

```pwsh
AcsEditor.exe --profiler-selftest
$image = Join-Path $env:TEMP "acs-profiler.png"
AcsEditor.exe --profilershot $image
```

`--profiler-selftest` はリセット世代、標本境界、CSV と `ProfilerSummary` のスキーマ、実行時タイムライン、`N/A` と `null`、失敗コード、`TEMP` 配下のパス、再解析ポイント拒否、1 MiB上限、置換失敗時の既存ファイル保持を検証します。`--profilershot` は決定的な表示データでプロファイラー画面を描画しますが、性能値の合否判定には使用しません。

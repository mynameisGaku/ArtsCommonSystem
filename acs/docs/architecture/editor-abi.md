# Editor ネイティブ ABI

ACS Editor は、WPF 側の管理ホストから `acs_editor_abi.dll` の C ABI を呼び出します。管理側はネイティブのシーン、描画器、GPU資源を所有せず、不透明なホストハンドルと版・機能情報だけを保持します。

## 互換性の照会

ネイティブホストを作成する前に、`acs_editor_abi_query` へ契約版と必須機能のビット列を渡します。現在の要求契約版は `1` です。提供側の契約版が要求版以上で、次の機能がすべて公開されている場合だけ互換と判定します。

| 機能 | 表示名 | 必須とする理由 |
|---|---|---|
| `FrameResultContract` | `frame-result-v1` | 提示成功、GPU待機、致命的失敗をフレーム結果で区別するため |
| `IncrementalStartup` | `incremental-startup` | ネイティブ描画器を段階的に起動するため |
| `ResizeResultContract` | `resize-result-v1` | サイズ変更の成功、待機、失敗を区別するため |
| `SparseTransformMutationV1` | `sparse-transform-mutation-v1` | Details の混在値編集で未入力成分を上書きしないため |

`ProfilerV4`、`ProfilerV5`、`VolumetricCloudWorkloadV1`、`UnifiedSceneDocument`、`MaterialPreviewQuality`、`SubstrateGraph`、`InteractiveWater3D`、`CameraAuthoringV1`、`CameraViewRequestsV1`、`OptionalServiceDiagnosticsV2`、`HdrFrameCaptureV1` は個別に判定する任意機能です。製品版文字列から利用可否を推測しません。

## 失敗時の境界

次のいずれかに該当する場合、ネイティブホストをUIへ公開せず、ネイティブビューポートを無効のまま保ちます。

- `acs_editor_abi_query` が存在しない、または照会を拒否した場合
- DLLを読み込めない、実行形式のアーキテクチャが一致しない場合
- 提供側の契約版が古い、または必須機能が不足している場合
- `acs_editor_create` が空のハンドルを返した場合
- ホスト作成中に例外または取消が発生した場合

互換性照会に失敗した場合は `acs_editor_create` を呼びません。作成後かつ公開前に失敗したハンドルは破棄し、部分的なホストをWPF側へ公開しません。ホストの作成、世代確認、破棄の順序は[起動時の応答性](../guides/editor/startup-responsiveness.md)で説明します。

## 任意サービス診断

`OptionalServiceDiagnosticsV2` は、Profiler、ボリューム雲の処理量、Camera View要求の状態を問い合わせる読み取り専用機能です。この機能自体は管理ホストの必須条件ではありません。

呼び出し側は `acs_editor_optional_service_diagnostic_get` に版と構造体サイズを設定した領域を渡します。

| 版 | 構造体サイズ | 内容 |
|---|---:|---|
| `1` | 192バイト | サービス、状態、理由、フラグ、ホスト世代、160バイトのNUL終端UTF-8メッセージ領域 |
| `2` | 256バイト | 版1の192バイト接頭部に、エラー領域、エラー番号、診断世代、48バイトのNUL終端 `stable_code_utf8` を持つ64バイトの末尾領域を追加 |

提供側は版1では192バイト以上、版2では256バイト以上の一貫した領域を受理し、それぞれ192バイトまたは256バイトを書き戻します。未知の版、必要サイズ未満の領域、宣言サイズと実領域の不一致は、ホストを参照する前に拒否します。構造が正しい問い合わせでは、空、破棄済み、未登録のホストや未知のサービスも、`InvalidHost` や `UnknownService` を含む診断値として返します。

状態は `Enabled`、`Disabled`、`Pending`、`Inactive`、`Failed` のいずれかです。理由、`Callable`、`Retryable`、エラー領域、エラー番号の組み合わせが状態と一致しない結果は利用しません。

## 世代と公開

各ネイティブホストには0以外の `host_generation` が割り当てられます。版2の各サービス診断には、単調増加する `diagnostic_generation` があります。

`EditorOptionalServiceUiSession` は次を確認してから診断をUIへ公開します。

1. 問い合わせ前後で管理側のホストハンドルと `EngineViewport` 世代が同じであること。
2. 返却された `host_generation` が現在のネイティブホスト世代と一致すること。
3. 対象サービスが要求したサービスと一致すること。
4. メッセージと `stable_code_utf8` が領域内でNUL終端された正しいUTF-8であること。
5. `diagnostic_generation` が、そのサービスで最後に受理した値より大きいこと。

遅延結果、再送された世代、壊れた文字列、矛盾した状態は、対象サービスだけを利用不可として扱います。Profilerの一時停止や保持済み履歴の出力、Camera Viewの再ドッキングなど、診断対象外の操作は維持します。`OptionalServiceDiagnosticsV2` が公開されていない場合は、各任意機能の機能ビットに基づく互換経路を使用します。

## 検証

```pwsh
AcsEditor.exe --abi-contract-selftest
AcsEditor.exe --optional-service-ui-selftest
```

ネイティブ側の版、サイズ、無効ホスト、世代、問い合わせと破棄の競合は `ACS.EditorAbiLifecycle` で検証します。

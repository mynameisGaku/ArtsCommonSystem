# HotReload の安全性契約

`FHotReloadWatcher` は、開発時のファイル変更を上限付きで集約する
`gameframework/HotReload.h` の監視ハブである。Windows では
`ReadDirectoryChangesW` をオーバーラップ I/O で駆動し、独自の変更元は
`TryEnqueueEvent` から同じ FIFO キューへイベントを投入できる。

この API は単一スレッド専用である。同じインスタンスを複数スレッドから同時に
操作するための同期機構は持たない。

## 安定した結果名

検証付き API の結果は `EHotReloadResult` で分類される。ログや診断では
列挙値を独自に `switch` せず、`HotReloadResultName` を使うこと。
この関数は Shipping と Shipping 以外のどちらでも利用でき、次の 11 個の実結果に
安定した文字列を返す。

| 結果 | `HotReloadResultName` | 意味 |
| --- | --- | --- |
| `Success` | `"Success"` | 操作が成功した |
| `AlreadyRegistered` | `"AlreadyRegistered"` | 同じ監視または同じ `(callback, user)` が登録済み |
| `NotRegistered` | `"NotRegistered"` | 解除対象のコールバック組が登録されていない |
| `InvalidArgument` | `"InvalidArgument"` | null、空、制御文字、無効な時間値など |
| `InvalidUtf8` | `"InvalidUtf8"` | パスが正しい UTF-8 ではない |
| `PathTooLong` | `"PathTooLong"` | パスが `kMaxPathBytes` を超えた |
| `LimitExceeded` | `"LimitExceeded"` | 固定上限を超えた |
| `OutOfMemory` | `"OutOfMemory"` | 所有コピー、配列予約、スナップショット作成などに失敗した |
| `OsError` | `"OsError"` | ネイティブ監視、変換、通知記録、読み取り再発行などに失敗した |
| `ReentrantCall` | `"ReentrantCall"` | コールバックから `TryTick` へ再入した |
| `NativeOverflow` | `"NativeOverflow"` | Windows の通知バッファで変更を取りこぼした |

`EHotReloadResult::Count` は実結果ではなく列挙網羅性を検査する末尾の番兵であり、
`HotReloadResultName(EHotReloadResult::Count)` は `"Unknown"` を返す。範囲外の値も
同じく `"Unknown"` になる。列挙値の序数を永続化や外部プロトコルに使わず、診断には
安定文字列を使う。

## 検証付き登録と一括した失敗処理

新規コードでは次の検証付き API を優先する。

```cpp
FHotReloadWatcher watcher;
watcher.Init();

const EHotReloadResult watch_result =
    watcher.TryWatchDirectory("Assets", true);
if (watch_result != EHotReloadResult::Success) {
    // 既存アセット状態を維持し、登録失敗を報告する。
}

const EHotReloadResult callback_result =
    watcher.TryRegisterCallback(&OnAssetChanged, this);
```

`WatchDirectory`、`WatchFile`、`RegisterCallback`、`Tick` はソース互換性のために
残したラッパーであり、内部で検証結果を捨てる。
失敗理由や復旧判断が必要な処理ではラッパーを使わない。

`TryWatchDirectory` は、次の処理がすべて成功するまで公開中の監視数を変更しない。

1. パスの検証と所有コピー
2. 配列容量の確保
3. UTF-16 への変換
4. ディレクトリハンドルの作成
5. 最初の非同期読み取りの発行

途中で失敗した場合、ハンドルと一時所有物を解放し、監視パスだけが残るような
部分登録を行わない。先に同じパスを `TryWatchFile` でフィルター登録していた場合は、
その一件を重複加算せずネイティブのディレクトリ監視へ昇格できる。同じネイティブ
ディレクトリ監視の二重登録は `AlreadyRegistered` になる。

`TryWatchFile` はパスの所有登録とフィルター処理の境界であり、ファイルごとのネイティブ
ハンドルは作らない。ネイティブ通知ストリームを開始するのは `TryWatchDirectory` である。
独自バックエンドは `TryEnqueueEvent` を使う。

パスは監視側がコピーするため、呼び出し側の入力バッファを生存させる必要はない。
次の入力は登録前またはキュー追加前に拒否される。

- null または空のパス
- `0x20` 未満の制御文字を含むパス
- 不正な UTF-8
- `kMaxPathBytes` を超えるパス

同一パス、および同一 `(callback, user)` 組の重複は
`AlreadyRegistered` になる。コールバック関数が null の登録・解除は
`InvalidArgument` である。`user` は監視側が所有せず、null も有効な
利用者コンテキストとして扱える。

## 固定上限

信頼できないエディター入力やイベント集中でメモリ使用量が無制限に増えないよう、
次の上限を設ける。

| 資源 | 定数 | 上限 |
| --- | --- | ---: |
| 監視パス | `kMaxWatchedPaths` | 256 |
| ネイティブのディレクトリ監視 | `kMaxDirectoryWatches` | 64 |
| コールバック組 | `kMaxCallbacks` | 64 |
| 保留イベント | `kMaxPendingEvents` | 1024 |
| UTF-8 パスのバイト数 | `kMaxPathBytes` | 4096 |
| 連続変更の統合秒数 | `kMaxDebounceSeconds` | 60.0 |

上限超過は `LimitExceeded` を返し、対応する配列へ部分追加しない。イベントとその
所有パスは常に同期して追加・削除される。確保失敗も `OutOfMemory` を返し、
片方だけを公開状態に残さない。

## 追加確保を伴わない診断スナップショット

`CaptureDiagnostics()` は、監視ごとの通知パイプライン診断値を
`FHotReloadDiagnostics` の値コピーで返す。読み取り時に確保せず、監視側の
キューや診断状態も変更しない。監視本体と同じく単一スレッド専用である。

| フィールド | 意味 |
| --- | --- |
| `enqueued_event_count` | 新しい保留イベント組を一括して確定できた累積件数 |
| `coalesced_event_count` | 連続変更の統合により既存の保留イベントへまとめた累積件数 |
| `dispatched_event_count` | コールバック配信の対象として FIFO キューから取り出した累積イベント件数 |
| `rejected_event_count` | パス検証、ネイティブ通知記録の検証、キュー追加などで拒否した累積イベント件数 |
| `loss_incident_count` | 正確な通知集合を保証できなくなった累積事象件数 |
| `last_failure` | キュー追加、ネイティブ監視、配信で直近に観測した非 `Success` 結果 |
| `authoritative_rescan_required` | 正本からの全量再走査が必要なら `true` |

全カウンタは `u64` 最大値で飽和し、長時間稼働しても 0 へ折り返さない。
`NativeOverflow` は実際に失われたイベント数を特定できないため、
`loss_incident_count` はイベント数ではなく「欠落を観測した回数」を数える。
有効な外部イベントをキュー上限または OOM で受理できなかった場合も、1 回の
欠落事象として数える。

`last_failure` と `authoritative_rescan_required` は、明示的に消去するまで維持される。
後続のキュー追加や更新が成功しても `Success` または `false` へ戻らない。診断の確認と
必要な再走査が完了した後にだけ `ClearDiagnostics()` を呼ぶ。この操作は診断値だけを初期化し、
監視登録、コールバック、保留イベントには触れない。`Shutdown()` も診断値を暗黙には
初期化しないため、停止後に失敗原因を調査できる。

```cpp
const FHotReloadDiagnostics diagnostics = watcher.CaptureDiagnostics();
if (diagnostics.authoritative_rescan_required) {
    RescanAssetsFromAuthoritativeDatabase();
    watcher.ClearDiagnostics();
}
```

無効な `dt`、`TryTick` 再入、コールバックスナップショットの OOM は `last_failure` に残るが、
特定イベントの拒否または通知欠落ではないためイベント・欠落カウンターは増やさない。
監視・コールバック登録や連続変更の統合設定の検証結果は呼び出し元がその場で処理する
契約であり、この通知パイプラインのスナップショット集計対象外である。

## キュー追加、FIFO、連続変更の統合

`TrySetDebounceSeconds` は有限な `[0, kMaxDebounceSeconds]` の値だけを受理する。
既定値は 50 ms である。

同じパスの最新保留イベントに対して、新しい時刻値が単調増加し、差が
統合時間内なら一件へまとめる。このとき新しい時刻値と `removed` が残る。
時間外の古いイベントは FIFO 上の位置を保つ。時刻値が逆行するイベントはまとめない。

時刻値の単位は単調増加ミリ秒である。ネイティブイベントは `GetTickCount64` を使う。
独自変更元も同じ単位を使い、同じ基準時計が生成したイベント同士だけを比較する。
時計の分解能が整数ミリ秒なので、0 より大きく 1 ms 未満の統合時間は 1 更新単位
として扱う。統合を完全に無効にできる値は厳密な `0` だけである。

ネイティブイベントと外部イベントは同じ上限付き FIFO キューを共有する。

```cpp
watcher.TryEnqueueEvent(
    "Assets/generated.mesh", timestamp_ms, false);
const EHotReloadResult tick_result = watcher.TryTick(delta_seconds);
```

`TryTick` は負数、NaN、無限大の `dt` を、ネイティブ監視と配信を始める前に
`InvalidArgument` で拒否する。コールバックから再帰的に呼んだ `TryTick` は
`ReentrantCall` になる。

同じ更新内で複数の問題が見つかった場合、`TryTick` の代表返却値だけは復旧判断に
重要な結果を残すため
`OsError`、`NativeOverflow`、`OutOfMemory`、`LimitExceeded` の順に優先する。
正常なイベントが処理できたことを理由に、同時に起きた通知欠落を `Success` へ
上書きしない。一方、`last_failure` は優先度集約せず、実際に最後に観測した失敗を
保持する。

## ネイティブ通知の欠落と正本からの再走査

Windows から受け取る `FILE_NOTIFY_INFORMATION` は、実際の完了バイト数の範囲内で
次の項目を検証してから UTF-8 イベントへ変換する。

- 記録の接頭部とファイル名が完了バッファ内に収まること
- ファイル名のバイト数が 0 ではなく `sizeof(WCHAR)` の倍数であること
- 操作種別が仕様上の `FILE_ACTION_*` 値 1〜5 のいずれかであること
- 0 ではない `NextEntryOffset` が整列と最小長を満たし、バッファ末尾より前に実在する
  次の記録を指すこと

空のファイル名、未知の操作種別、不正なオフセット、不正 UTF-16、パス長違反から偽の利用者イベント
を作らない。これらは `OsError` として報告し、その後の読み取りを再発行する。
UTF-16 変換またはパス連結中の確保失敗は `OutOfMemory` として区別する。どちらも
有効なネイティブイベントを失った状態なので欠落カウンターと再走査要求を設定する。

完了バイト数が 0、または Windows が `ERROR_NOTIFY_ENUM_DIR` を返した場合は通知欠落で
ある。読み取りの再発行に成功しても `NativeOverflow` を返し、再発行にも失敗した場合は
`OsError` を返す。監視を再開できたことは、欠落した変更を復元できたことを
意味しない。実際の監視とテストビルドの模擬完了は同じ内部処理を
通り、結果分類だけでなく欠落カウンター、再走査要求の保持、`last_failure` まで同じ経路で
更新する。

次の場合、呼び出し側はディスクや資産 DB などの正本から全量再走査を
行う。

- `NativeOverflow`
- ネイティブ監視、通知解析、文字変換、パス連結、読み取り再発行由来の `OsError`
- ネイティブイベントのキュー追加で `LimitExceeded` または `OutOfMemory` が返り、
  正確な変更集合が必要な場合

停止中または通知を欠落した監視は `Success` を返して問題を隠さない。

## コールバックの再入、安全な変更、寿命

配信開始時にコールバック一覧の安定したスナップショットを作る。作成に失敗した
場合は `OutOfMemory` を返し、保留イベントを取り出さない。各スナップショット項目を
呼ぶ直前には、完全一致する `(callback, user)` 組が有効な登録にまだ存在するかを
再確認する。

- コールバック内の `UnregisterCallback` は、その組が同じ配信の後段で
  呼ばれることを防ぐ
- コールバック内の `ClearEvents` は安全である。現在イベントの残りコールバックは完走するが、
  外側の FIFO キュー取り出しはそのイベント後に停止する
- コールバック内の `Shutdown` は状態を消去し、残りの配信を中止する
- 配信中に登録したコールバックは現在のスナップショットに入らず、次回配信から
  対象になる
- コールバックが追加したイベントは次の `TryTick` まで延期され、同じ更新内で
  無制限に自己増殖しない
- コールバックが `ClearEvents` または `ConsumeNextEvent` で開始時キューを縮めた後に
  追加しても、新しいイベントが空いた配信枠へ入り込むことはない
- コールバック内の `TryTick` 再入は `ReentrantCall` で拒否する

`FHotReloadEvent::file_path` は借用ポインターである。

- コールバック引数では、そのコールバックが戻るまでだけ有効
- `ConsumeNextEvent` の出力では、次の `ConsumeNextEvent`、`ClearEvents`、
  `Shutdown` のいずれかまで有効

この境界を越えてパスが必要なら、コールバックまたは取り出し処理の呼び出し中に
所有文字列へコピーする。`user` も借用ポインターなので、登録中は指し先を生存させ、
破棄前に `UnregisterCallback` または監視側の `Shutdown` を行う。

`Shutdown` は冪等である。コールバック中に呼ばれても安全で、発行中のオーバーラップ読み取り
を取り消し、完了を回収してから `OVERLAPPED` と受信バッファを破棄する。
デストラクタも `Shutdown` を呼ぶ。

## Shipping ビルド

`ACS_GAME_SHIPPING` ではネイティブ監視、監視パス、コールバック、保留イベントの
状態領域を持たない。クラスの API シンボルは何もしない外殻として残るため、呼び出し側を
ビルド構成ごとの `#ifdef` だらけにする必要はない。ただし Shipping の正しさを
HotReload に依存させてはならない。

Shipping 外殻の検証付き戻り値は次のとおりである。

- `TryWatchDirectory`、`TryWatchFile`、`TryEnqueueEvent` は null / 空のパスを
  `InvalidArgument`、それ以外を `Success` とするが、登録も詳細な UTF-8・長さ検証も
  行わない
- `TryRegisterCallback` は null コールバックを `InvalidArgument`、それ以外を
  `Success` とするが登録しない
- `UnregisterCallback` は null コールバックを `InvalidArgument`、それ以外を
  `NotRegistered` とする
- `TrySetDebounceSeconds` は有限な範囲内を `Success`、それ以外を
  `InvalidArgument` とするが、`DebounceSeconds()` は常に `0.0f` を返す
- `TryTick` は有限な非負値を `Success`、それ以外を `InvalidArgument` とするが、
  監視も配信も行わない
- `WatchedCount()` と `PendingEventCount()` は常に 0、
`ConsumeNextEvent()` は常に `false`
- `CaptureDiagnostics()` は状態領域を確保せず常に全フィールドが 0、
  `last_failure == Success`、`authoritative_rescan_required == false` の決定的
  スナップショットを返し、`ClearDiagnostics()` は何もしない

`HotReloadResultName` は Shipping 条件分岐の外にあり、Shipping でも 11 個の安定名、
`Count` と未知値に対する `"Unknown"` という同じ契約を保つ。
Shipping 実装には `sizeof(FHotReloadWatcher) == 1` と全値が 0 の診断スナップショットに対する
コンパイル時アサーションがあり、Shipping 構文検査でもこの契約を検証する。

## 回帰テスト

`tests/hot_reload_safety_tests.cpp` は、現実装の次の契約を検証する。

- 11 個の安定結果名、`Count`、範囲外値の `"Unknown"`
- ネイティブ操作種別、空のファイル名、奇数バイト長を拒否する純粋な解析処理境界
- パスの所有、重複排除、解除
- null、空、制御文字、不正 UTF-8、パス長超過の一括した拒否
- 監視パス、コールバック組、保留イベントの固定上限と部分追加防止
- ディレクトリの OS 登録失敗時に公開件数を増やさないこと
- ファイル登録済みパスをディレクトリ監視へ昇格できること
- コールバックの検証付き登録、重複排除、解除
- 連続変更の統合設定と `dt` の非有限値拒否
- 最新の同一パスイベントとの統合、FIFO、`removed` 更新
- 正の 1 ミリ秒未満の統合時間を時計の 1 更新単位として扱うこと
- 取り出し後のパス所有寿命
- コールバックからの `TryTick` 再入拒否と、同じ配信中の登録解除
- コールバック中の `Shutdown` による残り配信の安全な中止
- コールバックが追加したイベントを次の更新まで延期すること
- コールバックが A/B の配信中に `ClearEvents` して C を追加しても、A の
  残りコールバックだけを完走し、C を次の更新まで延期すること
- コールバックが A/B/C の配信中に B を `ConsumeNextEvent` して D を追加
  しても、A の残りコールバックを完走し、C/D を次の更新で FIFO 配信すること
- キュー追加、統合、配信、拒否、欠落の診断カウンター
- 診断カウンターの最大値飽和、失敗と再走査要求の維持
- `ClearDiagnostics` が監視登録、コールバック、保留イベントを消さず診断値だけを
  初期化すること
- テストビルド限定の一度限りの障害注入でネイティブ通知欠落、読み取り再開失敗、
  模擬ネイティブ通知記録の解析失敗、UTF-16 変換 OOM を実際の
  `TryTick` 診断経路へ通すこと
- ネイティブ通知欠落と再開失敗の障害注入が、実際の監視と同じ完了処理で
  結果・欠落・再走査要求・直近失敗を更新すること
- `TryTick` の代表返却値を優先度集約しても、`last_failure` は最後に観測した失敗を
  保持すること

`tests/win32_resource_tests.cpp` は Windows 上で実際のディレクトリハンドルを開き、
`Unwatch` と `Shutdown` が資源を解放する経路も検証する。

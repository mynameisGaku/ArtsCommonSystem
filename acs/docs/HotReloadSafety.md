# HotReload の安全性契約

`FHotReloadWatcher` は、開発時のファイル変更を上限付きで集約する
`gameframework/HotReload.h` の監視ハブである。Windows では
`ReadDirectoryChangesW` を overlapped I/O で駆動し、独自の変更元は
`TryEnqueueEvent` から同じ FIFO へイベントを投入できる。

この API は単一スレッド専用である。同じインスタンスを複数スレッドから同時に
操作するための同期機構は持たない。

## 安定した結果名

checked API の結果は `EHotReloadResult` で分類される。ログや診断では
enum 値を独自に `switch` せず、`HotReloadResultName` を使うこと。
この関数は Shipping / non-Shipping のどちらでも利用でき、次の 11 個の実結果に
安定した文字列を返す。

| 結果 | `HotReloadResultName` | 意味 |
| --- | --- | --- |
| `Success` | `"Success"` | 操作が成功した |
| `AlreadyRegistered` | `"AlreadyRegistered"` | 同じ監視または同じ `(callback, user)` が登録済み |
| `NotRegistered` | `"NotRegistered"` | 解除対象の callback pair が登録されていない |
| `InvalidArgument` | `"InvalidArgument"` | null、空、制御文字、無効な時間値など |
| `InvalidUtf8` | `"InvalidUtf8"` | path が正しい UTF-8 ではない |
| `PathTooLong` | `"PathTooLong"` | path が `kMaxPathBytes` を超えた |
| `LimitExceeded` | `"LimitExceeded"` | 固定上限を超えた |
| `OutOfMemory` | `"OutOfMemory"` | 所有コピー、配列予約、snapshot 作成などに失敗した |
| `OsError` | `"OsError"` | native 監視、変換、通知レコード、read 再発行などに失敗した |
| `ReentrantCall` | `"ReentrantCall"` | callback から `TryTick` へ再入した |
| `NativeOverflow` | `"NativeOverflow"` | Windows の通知バッファで変更を取りこぼした |

`EHotReloadResult::Count` は実結果ではなく列挙網羅性を検査する末尾 sentinel であり、
`HotReloadResultName(EHotReloadResult::Count)` は `"Unknown"` を返す。範囲外の値も
同じく `"Unknown"` になる。enum の序数を永続化や外部プロトコルに使わず、診断には
安定文字列を使う。

## checked 登録と transactional な失敗

新規コードでは `TryWatchDirectory`、`TryWatchFile`、`TryRegisterCallback`、
`TryTick` の checked 結果を処理し、失敗時は公開中の監視状態を維持する。

`WatchDirectory`、`WatchFile`、`RegisterCallback`、`Tick` は source
compatibility のために残した wrapper であり、内部で checked 結果を捨てる。
失敗理由や復旧判断が必要な処理では wrapper を使わない。

`TryWatchDirectory` は、次の処理がすべて成功するまで公開中の監視数を変更しない。

1. path の検証と所有コピー
2. 配列容量の確保
3. UTF-16 への変換
4. directory handle の作成
5. 最初の非同期 read の発行

途中で失敗した場合、handle と一時所有物を解放し、監視 path だけが残るような
部分登録を行わない。先に同じ path を `TryWatchFile` で filter 登録していた場合は、
その一件を重複加算せず native directory watcher へ昇格できる。同じ native
directory watcher の二重登録は `AlreadyRegistered` になる。

`TryWatchFile` は path の所有登録と filter の seam であり、ファイルごとの native
handle は作らない。native 通知ストリームを開始するのは `TryWatchDirectory` である。
独自 backend は `TryEnqueueEvent` を使う。

path は watcher がコピーするため、呼び出し側の入力バッファを生存させる必要はない。
次の入力は登録前または enqueue 前に拒否される。

- null または空 path
- `0x20` 未満の制御文字を含む path
- 不正な UTF-8
- `kMaxPathBytes` を超える path

同一 path、および同一 `(callback, user)` pair の重複は
`AlreadyRegistered` になる。callback 関数が null の登録・解除は
`InvalidArgument` である。`user` は watcher が所有せず、null も有効な
context として扱える。

## 固定上限

信頼できない editor 入力やイベント burst でメモリ使用量が無制限に増えないよう、
次の上限を設ける。

| 資源 | 定数 | 上限 |
| --- | --- | ---: |
| 監視 path | `kMaxWatchedPaths` | 256 |
| native directory watcher | `kMaxDirectoryWatches` | 64 |
| callback pair | `kMaxCallbacks` | 64 |
| pending event | `kMaxPendingEvents` | 1024 |
| UTF-8 path byte 数 | `kMaxPathBytes` | 4096 |
| debounce 秒数 | `kMaxDebounceSeconds` | 60.0 |

上限超過は `LimitExceeded` を返し、対応する配列へ部分追加しない。event とその
所有 path は常に lockstep で追加・削除される。確保失敗も `OutOfMemory` を返し、
片方だけを公開状態に残さない。

## allocation-free 診断 snapshot

`CaptureDiagnostics()` は、watcher ごとの通知パイプライン診断値を
`FHotReloadDiagnostics` の値コピーで返す。読み取り時に確保せず、watcher の
queue や診断状態も変更しない。監視本体と同じく単一スレッド専用である。

| フィールド | 意味 |
| --- | --- |
| `enqueued_event_count` | 新しい pending event pair を transactional に commit できた累積件数 |
| `coalesced_event_count` | debounce により既存 pending event へ統合した累積件数 |
| `dispatched_event_count` | callback dispatch の対象として FIFO から取り出した累積 event 件数 |
| `rejected_event_count` | path 検証、native record 検証、queue 追加などで拒否した累積 event 件数 |
| `loss_incident_count` | 正確な通知集合を保証できなくなった累積 incident 件数 |
| `last_failure` | enqueue、native poll、dispatch で直近に観測した非 `Success` 結果 |
| `authoritative_rescan_required` | 正本からの全量再走査が必要なら `true` |

全カウンタは `u64` 最大値で飽和し、長時間稼働しても 0 へ折り返さない。
`NativeOverflow` は実際に失われた event 数を特定できないため、
`loss_incident_count` は event 数ではなく「欠落を観測した回数」を数える。
有効な外部 event を queue 上限または OOM で受理できなかった場合も、1 回の
欠落 incident として数える。

`last_failure` と `authoritative_rescan_required` は sticky である。後続の enqueue や
tick が成功しても `Success` または `false` へ戻らない。診断の確認と必要な rescan が
完了した後にだけ `ClearDiagnostics()` を呼ぶ。この clear は診断値だけを初期化し、
監視登録、callback、pending event には触れない。`Shutdown()` も診断値を暗黙には
clear しないため、停止後に失敗原因を調査できる。

無効な `dt`、`TryTick` 再入、callback snapshot の OOM は `last_failure` に残るが、
特定 event の拒否または通知欠落ではないため event/loss counter は増やさない。
監視・callback 登録や debounce 設定の checked 結果は呼び出し元がその場で処理する
契約であり、この通知パイプライン snapshot の集計対象外である。

## enqueue、FIFO、debounce

`TrySetDebounceSeconds` は有限な `[0, kMaxDebounceSeconds]` の値だけを受理する。
既定値は 50 ms である。

同じ path の最新 pending event に対して、新しい timestamp が単調増加し、差が
debounce 窓内なら一件へまとめる。このとき新しい timestamp と `removed` が残る。
窓外の古い event は FIFO 上の位置を保つ。timestamp が逆行する event はまとめない。

timestamp の単位は単調増加ミリ秒である。native event は `GetTickCount64` を使う。
独自変更元も同じ単位を使い、同じ source clock が生成した event 同士だけを比較する。
clock の分解能が整数ミリ秒なので、0 より大きく 1 ms 未満の debounce は 1 tick
として扱う。coalescing を完全に無効にできる値は厳密な `0` だけである。

native event と外部 event は同じ上限付き FIFO を共有する。

`TryTick` は負数、NaN、無限大の `dt` を、native poll と dispatch を始める前に
`InvalidArgument` で拒否する。callback から再帰的に呼んだ `TryTick` は
`ReentrantCall` になる。

同じ tick 内で複数の問題が見つかった場合、`TryTick` の代表返却値だけは復旧判断に
重要な結果を残すため
`OsError`、`NativeOverflow`、`OutOfMemory`、`LimitExceeded` の順に優先する。
正常な event が処理できたことを理由に、同時に起きた通知欠落を `Success` へ
上書きしない。一方、`last_failure` は優先度集約せず、実際に最後に観測した失敗を
保持する。

## native overflow と authoritative rescan

Windows から受け取る `FILE_NOTIFY_INFORMATION` は、実際の完了 byte 数の範囲内で
次の項目を検証してから UTF-8 event へ変換する。

- record prefix と filename が完了 buffer 内に収まること
- filename byte 数が 0 ではなく `sizeof(WCHAR)` の倍数であること
- action が仕様上の `FILE_ACTION_*` 値 1〜5 のいずれかであること
- 非 zero の `NextEntryOffset` が整列、最小長を満たし、buffer 末尾より前に実在する
  次 record を指すこと

空 filename、未知 action、不正 offset、不正 UTF-16、path 長違反から偽の user event
を作らない。これらは `OsError` として報告し、その後の read を再発行する。
UTF-16 変換または path 連結中の確保失敗は `OutOfMemory` として区別する。どちらも
有効な native event を失った状態なので loss counter と rescan 要求を設定する。

完了 byte 数が 0、または Windows が `ERROR_NOTIFY_ENUM_DIR` を返した場合は通知欠落で
ある。read の再発行に成功しても `NativeOverflow` を返し、再発行にも失敗した場合は
`OsError` を返す。監視を再 arm できたことは、欠落した変更を復元できたことを
意味しない。実 watcher と test build の synthetic completion は同じ内部 handler を
通り、結果分類だけでなく loss counter、rescan latch、`last_failure` まで同じ配線で
更新する。

次の場合、呼び出し側はディスクや資産 DB などの正本から authoritative rescan を
行う。

- `NativeOverflow`
- native poll、通知解析、文字変換、path 連結、read 再発行由来の `OsError`
- native event の queue 追加で `LimitExceeded` または `OutOfMemory` が返り、
  正確な変更集合が必要な場合

停止中または lossy な watcher は `Success` を返して問題を隠さない。

## callback の再入、安全な変更、寿命

dispatch 開始時に callback 一覧の安定 snapshot を作る。snapshot 作成に失敗した
場合は `OutOfMemory` を返し、pending event を drain しない。各 snapshot entry を
呼ぶ直前には、完全一致する `(callback, user)` pair が live 登録にまだ存在するかを
再確認する。

- callback 内の `UnregisterCallback` は、その pair が同じ dispatch の後段で
  呼ばれることを防ぐ
- callback 内の `ClearEvents` は安全である。現在 event の残り callback は完走するが、
  外側の FIFO drain はその event 後に停止する
- callback 内の `Shutdown` は状態を消去し、残りの dispatch を中止する
- dispatch 中に登録した callback は現在の snapshot に入らず、次回 dispatch から
  対象になる
- callback が enqueue した event は次の `TryTick` まで延期され、同じ tick で
  無制限に自己増殖しない
- callback が `ClearEvents` または `ConsumeNextEvent` で開始時 queue を縮めた後に
  enqueue しても、新 event が空いた dispatch 枠へ入り込むことはない
- callback 内の `TryTick` 再入は `ReentrantCall` で拒否する

`FHotReloadEvent::file_path` は借用 pointer である。

- callback 引数では、その callback が戻るまでだけ有効
- `ConsumeNextEvent` の出力では、次の `ConsumeNextEvent`、`ClearEvents`、
  `Shutdown` のいずれかまで有効

この境界を越えて path が必要なら、callback または consume の呼び出し中に
所有文字列へコピーする。`user` も借用 pointer なので、登録中は指し先を生存させ、
破棄前に `UnregisterCallback` または watcher の `Shutdown` を行う。

`Shutdown` は冪等である。callback 中に呼ばれても安全で、発行中の overlapped read
を cancel し、完了を回収してから `OVERLAPPED` と受信 buffer を破棄する。
デストラクタも `Shutdown` を呼ぶ。

## Shipping build

`ACS_GAME_SHIPPING` では native watcher、監視 path、callback、pending event の
storage を持たない。クラスの API symbol は no-op shell として残るため、呼び出し側を
build-mode の `#ifdef` だらけにする必要はない。ただし Shipping の正しさを hot
reload に依存させてはならない。

Shipping shell の checked 戻り値は次のとおりである。

- `TryWatchDirectory`、`TryWatchFile`、`TryEnqueueEvent` は null / 空 path を
  `InvalidArgument`、それ以外を `Success` とするが、登録も詳細な UTF-8・長さ検証も
  行わない
- `TryRegisterCallback` は null callback を `InvalidArgument`、それ以外を
  `Success` とするが登録しない
- `UnregisterCallback` は null callback を `InvalidArgument`、それ以外を
  `NotRegistered` とする
- `TrySetDebounceSeconds` は有限な範囲内を `Success`、それ以外を
  `InvalidArgument` とするが、`DebounceSeconds()` は常に `0.0f` を返す
- `TryTick` は有限な非負値を `Success`、それ以外を `InvalidArgument` とするが、
  poll も dispatch も行わない
- `WatchedCount()` と `PendingEventCount()` は常に 0、
  `ConsumeNextEvent()` は常に false
- `CaptureDiagnostics()` は storage を確保せず常に全フィールドが 0、
  `last_failure == Success`、`authoritative_rescan_required == false` の決定的
  snapshot を返し、`ClearDiagnostics()` は no-op

`HotReloadResultName` は Shipping guard の外にあり、Shipping でも 11 個の安定名、
`Count` と未知値に対する `"Unknown"` という同じ契約を保つ。
Shipping 実装には `sizeof(FHotReloadWatcher) == 1` とゼロ診断 snapshot の
compile-time assertion があり、Shipping 構文検査でもこの契約を検証する。

## 回帰テスト

`tests/hot_reload_safety_tests.cpp` は、現実装の次の契約を検証する。

- 11 個の安定結果名、`Count`、範囲外値の `"Unknown"`
- native action、空 filename、奇数 byte 長を拒否する純粋 parser seam
- path の所有、重複排除、解除
- null、空、制御文字、不正 UTF-8、path 長超過の transactional な拒否
- 監視 path、callback pair、pending event の固定上限と部分追加防止
- directory OS 登録失敗時に公開件数を増やさないこと
- file 登録済み path を directory watcher へ昇格できること
- callback の checked 登録、重複排除、解除
- debounce 設定と `dt` の非有限値拒否
- 最新 same-path event との coalescing、FIFO、`removed` 更新
- 正の sub-millisecond debounce を 1 clock tick として扱うこと
- consume 後の path 所有寿命
- callback からの `TryTick` 再入拒否と、同じ dispatch 中の登録解除
- callback 中の `Shutdown` による残り dispatch の安全な中止
- callback が enqueue した event を次の tick まで延期すること
- callback が A/B の dispatch 中に `ClearEvents` して C を enqueue しても、A の
  残り callback だけを完走し、C を次の tick まで延期すること
- callback が A/B/C の dispatch 中に B を `ConsumeNextEvent` して D を enqueue
  しても、A の残り callback を完走し、C/D を次の tick に FIFO 配信すること
- enqueue、coalesce、dispatch、拒否、loss の診断 counter
- 診断 counter の最大値飽和、失敗と rescan 要求の sticky 性
- `ClearDiagnostics` が監視登録、callback、pending event を消さず診断値だけを
  初期化すること
- test build 限定の one-shot fault injection で native overflow、read 再 arm 失敗、
  synthetic native record の parser failure、UTF-16 変換 OOM を実際の
  `TryTick` 診断経路へ通すこと
- native overflow と再 arm 失敗の fault injection が、実 watcher と同じ completion
  handler で結果・loss・rescan・直近失敗を更新すること
- `TryTick` の代表返却値を優先度集約しても、`last_failure` は最後に観測した失敗を
  保持すること

`tests/win32_resource_tests.cpp` は Windows 上で実 directory handle を開き、
`Unwatch` と `Shutdown` が resource を解放する経路も検証する。

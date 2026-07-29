# Foundation Optimization Wave B

## 目的と判断基準

Wave B は、基盤層の待機・投入・イベント配送・タイマー・ファイル I/O に残っていた、要素数に比例する不要処理と所有権の曖昧さを減らす変更である。既存の `TArray`、`TAtomic`、`FMutex`、`FConditionVar`、`FPoolAllocator`、`TResult` を利用し、別系統のスケジューラーや I/O サブシステムは追加しない。

採用判断は次の四点に基づく。

- 目的: idle 時のポーリング、burst 投入時の過剰通知、線形 cancel、キュー先頭削除、ファイル全体読み込み時の中間コピーを除く。
- 効果: 同じ公開動作を維持しながら、処理量を決定的カウンタで減少確認できること。
- 依存: Foundation の既存同期・コンテナ・メモリアロケーターだけを使い、新しい所有者不在の機構を作らないこと。
- 検証可能性: Release/Debug 実行、100 回反復、既存全ユニットテスト、規約監査、実行ファイルのサイズと SHA-256 で再確認できること。

## 対象

| ID | 対象 | 実装内容 |
|---|---|---|
| T06 | `FThreadPool` | idle の時間指定ポーリングを条件変数待機へ変更し、sleep 中ワーカー数と wake 予約数に基づく通知へ変更。外部投入 FIFO は最大 16 件を一括取得し、残件を steal 可能な owner deque へ公開。 |
| T07 | `FJobGraph` | Kahn 法で topology を一度検証・キャッシュし、反復実行では dependency counter だけをリセット。先頭 32 job と実行 scratch をインライン保持。 |
| T08 | `TMessagePipe` | 既定 MPMC の先頭 index と償却 compaction、最大深度、batch push/pop を追加し、pop ごとの配列 shift を廃止。 |
| T09 | `FTimerManager` | active slot bitset、generation 付き直接 cancel、active count の定数時間取得を追加。tick は active word と active slot だけを走査。 |
| T10 | `FFileSystem` / `FStorage` | text の直接一回読み込み、4 GiB 超過拒否、厳密な directory 作成、同一 directory 内 temp を使う atomic write を実装。 |
| T26 | 所有 callable | `FThreadPool::SubmitCallable` と `FJobGraph::AddCallable` を追加。小型 callable は inline、超過時だけ heap を使い、成功・失敗・停止の各経路で一度だけ破棄。 |
| T27 | `FMessageBroker` | `SubscribeTyped` を追加し、callback signature を compile time 検証。ID から slot を直接参照する unsubscribe と定数時間 active count を実装。 |
| T28 | `TMessagePipe` SPSC | compile-time policy と 2 の累乗 capacity を持つ acquire/release ring specialization を追加。値操作は noexcept を要求し、Close は最後の Push 完了後に行う契約を明記。 |
| T29 | path / storage safety | ASCII extension の constexpr 分類を追加し、Unicode 本文を保持。atomic replace 前の reparse 再検査と失敗時 temp cleanup を追加。拡張子だけで Storage 内容を拒否せず従来動作を維持。 |
| T30 | typed timer | `Schedule<Policy, Callback, User>` を追加し、Once/Repeating と callback signature を compile time に固定。 |

## 公開 API と依存関係

owner API は `MessagePipe.h`、`MessageBroker.h`、`Timer.h`、`FileSystem.h`、`ThreadPool.h`、`JobGraph.h` に置き、公開補助型は一主要型一 header の規約に従って `MessagePipePolicy.h`、`TimerSchedulePolicy.h`、`TimerDiagnostics.h`、`FileExtensionKind.h`、`FileSystemDiagnostics.h`、`ThreadPoolDiagnostics.h`、`JobGraphDiagnostics.h` へ分離した。補助型は状態や独立ライフサイクルを持たず、owner header から include するため既存利用者の互換経路も維持する。従来の raw callback API と既定 MPMC pipe も残している。

`acsbuild gen` で Event / Platform / Threading の `Module.cmake` に分離 header を登録し、module source 監査と配布 header 生成から参照できるようにした。`acs/tests/CMakeLists.txt` には同じテストソースを使う専用 target `acs_foundation_optimization_wave_b_tests` と CTest `ACS.FoundationOptimizationWaveB` を登録した。外部 library 依存は追加していない。

## 決定的な前後比較

| 観測項目 | 変更前 | 変更後 |
|---|---:|---:|
| idle worker の timeout wake | 最大 500 回/worker/秒（2 ms wait） | 0（条件通知または shutdown のみ） |
| 512 task burst の `NotifyOne` | submit ごとに最大 512 回 | 4 回（4 sleeper、100 回反復で min=max=4） |
| 512 task burst の drain lock | 512 回（1 task/lock） | 32～35 回（100 回反復） |
| 8,192 timer の cancel probe 合計 | 33,550,336 | 8,191 |
| cancel 後 1 active timer の tick 走査 | 8,192 slot | active 1 slot + 128 bitset word |
| 40 job graph を 3 回 submit | topology build 3、entry scan 3 | topology build 1、entry scan 1 |
| 8 要素 pipe を全 pop | 28 要素 move（先頭 erase の解析値） | 0 shift |
| `ReadAllText` の中間 payload copy | N byte | 0 byte |

カウンタは `FThreadPoolDiagnostics`、`FJobGraphDiagnostics`、`FTimerDiagnostics`、`FFileSystemDiagnostics` と専用テスト出力から取得する。timer の変更前 probe 数と pipe の変更前 move 数は旧アルゴリズムから算出した解析値であり、wall-clock 推測ではない。

## ビルド・実行計測

同一 Windows x64 環境、Visual Studio 18、Release、`--parallel 16` で測定した。

| 計測 | 変更前 | 変更後 |
|---|---:|---:|
| Release configure | 58.4 s | 49.001 s |
| Release clean `acs_unit_tests` build | 123.3 s | 113.913 s |
| Release 全 unit runtime | 35,562.240 ms（1 sample） | 33,067.802 / 35,840.896 / 35,254.706 / 32,451.103 / 32,531.845 / 34,297.036 ms、median 33,682.419 ms |
| `acs_unit_tests.exe` | 5,616,128 byte | 5,658,624 byte（+42,496 byte、+0.76%） |

compile/runtime の変更前値は一回測定、runtime の変更後値は六回測定であり、OS scheduler、antivirus、同時に進行した別 build の影響を含む。clean build の 113.913 s も並列作業中の一回測定である。差分を個別変更の因果効果とは扱わず、決定的カウンタを主たる性能根拠とする。

## 安全性契約

- ThreadPool: wake 予約は `wake_lock` 内で sleeper 数と再確認し、burst 中の予約重複と lost wake を防ぐ。shutdown は全 waiter を解除し、worker/callable lifecycle 中の自己 wait を行わない。
- Callable: inline/heap の保存先を明示し、実行後、submit 失敗、graph 破棄の全経路で destructor を一度だけ呼ぶ。worker が submit 戻り前にノードを返却できるため、返却後にノードへ触れない。`SubmitCallable` / `AddCallable` は noexcept 構築・呼び出し・破棄できる型だけを `static_assert` で受理し、暗黙 terminate となる throw 可能型を拒否する。
- JobGraph: cycle と不正 dependency を submit 前に拒否し、topology 成功時だけ cache を再利用する。capacity 超過は heap fallback として診断できる。
- MessagePipe/Broker: MPMC FIFO、SPSC 順序、bounded push、batch 順序、nested publish 中の cancel をテストする。Publish 中の新規購読は空き slot を再利用せず末尾へ追加し、発行開始後の購読者を次回配送まで呼ばない。SPSC は noexcept の既定構築・move 構築・move 代入・破棄を要求し、Close と Push を並行させない。typed API は raw ABI thunk を介して従来配送と同じ reentrancy を保ち、noexcept callback だけを受理する。
- Timer: handle generation、slot 再利用順、nested callback、Once/Repeating の無効 period cleanup を維持する。typed callback は noexcept をコンパイル時に要求する。
- File/Storage: null/empty、4 GiB 超、親が通常 file、locked destination を安全に処理する。atomic write は同一 directory の temp を flush 後に replace し、失敗時に旧内容を維持して temp を削除する。reparse target は link 自体を置換しない。`.json` / `.bin` / `.acpak` を含む従来の Storage path は引き続き保存・読込できる。

## 検証

以下を最終ソースで実行する。

```powershell
dotnet run --project acs\tools\acsbuild -- gen
cmake -S acs\engine -B .wave-b-build -G "Visual Studio 18 2026" -A x64 -DACS_LAYOUT_ROOT="C:/Users/g0190/AppData/Local/Temp/acs-foundation-opt-b/.wave-b-layout" -DACS_BUILD_TESTS=ON -DACS_BUILD_SAMPLES=OFF -DACS_BUILD_TOOLS=OFF
cmake --build .wave-b-build --config Release --target acs_unit_tests --clean-first --parallel 16
cmake --build .wave-b-build --config Debug --target acs_foundation_optimization_wave_b_tests --parallel 16
.\.wave-b-layout\Binaries\Release\acs_unit_tests.exe
.\.wave-b-layout\Binaries\Release\acs_foundation_optimization_wave_b_tests.exe
.\.wave-b-layout\Binaries\Debug\acs_foundation_optimization_wave_b_tests.exe
ctest --test-dir .wave-b-build -C Release --output-on-failure -R "^(ACS.FoundationOptimizationWaveB|ACS.CppConventionsAuditSelfTest|ACS.CppConventionsAudit|ACS.ReferenceTypeNamesAuditSelfTest|ACS.ReferenceTypeNamesAudit|ACS.ModuleSourcesAuditSelfTest|ACS.ModuleSourcesAudit|ACS.SingleHeaderPipelineSelfTest|ACS.AmalgamationDrift|ACS.DistributionConventions|ACS.DistributionHeaderSyntax)$"
python acs\scripts\audit_cpp_conventions.py --root acs --scope src/event --scope src/platform --scope src/threading
python C:\Users\g0190\AppData\Local\Temp\acs-foundation-opt-integration\acs\scripts\audit_changed_cpp_rules.py --root C:\Users\g0190\AppData\Local\Temp\acs-foundation-opt-b --base-ref origin/main
```

確定した結果:

- Release 全 unit: 6/6 process 成功、各 `passed=1106 failed=0`。clean build 後の SHA-256 記録対象 artifact も直接実行済み。
- Wave B Release: `passed=8 failed=0`。100 回反復も `100/100` 成功。
- Wave B Debug: `passed=8 failed=0`。
- Release clean unit build: exit 0、113,913.254 ms。
- CTest: Wave B、規約／参照／module source、単一 header、amalgamation、配布検証が 11/11 成功。
- 変更 C++ 規約監査: `changed_cpp_rules=ok files=20 lines=2621`。
- 既存 C++ 規約監査: `ACS C++ conventions audit passed: 43 file(s)`。
- 100 回反復の ThreadPool: wake 4～4、drain lock 32～35、5,638.499 ms。
- Timer 8,192 件: cancel probe 8,191、cancel 後 tick は active visit 1 / word load 128。
- File text read: system call 1、payload intermediate copy 0 byte。
- Release `acs_unit_tests.exe`: 5,658,624 byte、SHA-256 `9F25FCC1850B5B64088654654D1F389122CE7AEAF560A47631DCD05A876D0ACE`。
- Release Wave B test: 108,032 byte、SHA-256 `AA812AD22564183DA5F2F0FA8341FC0B67F62154A4D420D5DC441D41238FE11A`。
- Debug Wave B test: 367,616 byte、SHA-256 `BB61D43D3654E1B0BBC9E5DED9CFE9FABAA510F975E7F3842AD3F9ECF55A1454`。
- 配布単一 header `dist/acs.h`: 4,132,052 byte、SHA-256 `80AE1D16DA826691CE53503D3F80DBC88AFE348345C8F40E42888CF367F992F4`。

Temp 配下の worktree であるため MSBuild は `MSB8029` を警告する。これは中間／出力 directory の場所に対する警告で、build exit code、実行テスト、artifact hash は別途検証する。ゲーム配布 package 生成は Wave B の対象外であり、ここでは Release/Debug 実行ファイルの存在・直接実行・byte size・SHA-256 と、公開 API 配布物 `dist/acs.h` の drift・構文・規約・hash を生成物検証とする。

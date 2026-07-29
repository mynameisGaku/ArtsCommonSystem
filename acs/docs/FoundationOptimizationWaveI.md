# Foundation Optimization Wave I/J

## 目的

Wave I/J は、描画品質、計算精度、永続化形式、既存 API の結果を変えず、スレッド実行、
一時領域、enum 検証、固定幅直列化の反復コストを減らす。採用判断は目的、期待効果、
依存関係、決定的な検証可能性の四点で行い、効果を固定できない変更は残さない。

## 採用判断

| ID | 目的 | 期待効果 | 依存関係 | 決定的な検証 |
|---|---|---|---|---|
| T41 | Chase-Lev deque の共有書き込み競合を減らす | stealer の `top` と owner の `bottom` の cache line 往復を除去 | 64 byte cache line、既存 `TAtomic` | `offsetof` と診断値で別 line を固定 |
| T43 | 通常規模の ParallelFor 一時確保を除く | 32 chunk 以下の呼び出し時 heap 確保 1 回から 0 回 | 呼び出し stack、同期 `Wait` | `parallel_for_inline_calls` と heap fallback 0 |
| T44 | 大規模 ParallelFor の一時領域を再利用する | 毎回の可変長 HeapAlloc を固定 free-list block 取得へ置換 | 既存 `FPoolAllocator`、PoolState pin | pool/heap block 数と Shutdown 競合 stress |
| T45 | 連続 enum の検証と名前取得を生成する | 最大 3 比較または switch を範囲検査と index 参照へ統一 | `TContiguousEnumLookup` | compile-time 境界検査と全 error 名 test |
| T51 | little endian の重複実装を型付きで統一する | 型ごとの手書き shift/copy を共通 inline template へ集約 | 固定幅 scalar/enum、`memcpy` | exact byte、AssetPack、AnimationCurve の既存互換 test |
| T52 | frame arena の予約と reset をまとめる | N 領域を cursor 予約 1 回、通常 Reset を N page 走査から 1 page へ削減 | Reset gate、GrowLock、page 世代 | batch/領域数、page visit 診断と並行 Reset stress |

### 未採用

- T42 の thread handle hot/cold 分離案は、現行 `FPoolState` で実行時に参照する値との境界が
  小さく、決定的な削減量を固定できなかったため取り消した。
- T46 の branch hint 案は MSVC で `ACS_LIKELY` / `ACS_UNLIKELY` が code generation を
  変えないため採用しなかった。例外なしの既存 `TResult` 契約は変更していない。

## 実装

### ThreadPool

`FWorkerDeque::top` と `bottom` だけを個別に 64 byte 整列した。`top` は複数 stealer、
`bottom` は owner が書くため、従来の隣接配置は実共有頻度が高い。task buffer と
`FPoolState` の atomic は測定根拠がなく、局所性と状態サイズを悪化させるため分離して
いない。分離は内部 `offsetof` assertion で固定し、既存公開診断型の ABI は変えない。
ParallelFor の inline/pool/heap 経路は独立した `FParallelForDiagnostics` から返す。

`FThreadPool::ParallelFor` は 32 context までを呼び出し stack に置く。超過分は 64 context
単位の block とし、ThreadPool 初期化時に作る既存 `FPoolAllocator` から取得する。固定
pool は 32 block、約 49 KiB である。8 外部 thread が各 128 chunk を同時実行する
代表 stress の最大同時貸し出しは 16 block であり、その 2 倍を上限とした。32 block を
超える同時利用は OS heap へ退避するため、結果を変えず常駐量を固定できる。block は
`Wait` 完了後に一括して free-list へ戻す。ParallelFor 全体で `FPoolPin` を保持するため、
並行 Shutdown は context pool を先に破棄できない。固定 pool の構築に失敗した場合も
OS heap へ戻り、公開結果と task 順序は変えない。

### Arena

`FArenaAllocator::AllocBatch` は同じ size/alignment の領域を一つの連続予約へまとめる。
利用者 byte 数と領域数は従来の個別確保と同じ統計へ加算し、cursor CAS だけを 1 回にする。
通常の単発 `Alloc` に診断用 atomic RMW は追加せず、batch 専用の成功回数と領域数だけを
`AllocBatch` 成功後に記録する。保持 page 数は診断取得時に GrowLock 内で数え、通常確保へ
page 数更新用 atomic を追加しない。
失敗時は全出力を `nullptr` にし、byte、領域数、batch 診断を変更しない。

`Reset(false)` は世代を進め、先頭 page だけを即時初期化する。残りは GrowLock 内で再公開
する直前に初期化するため、通常 reset の page visit は保持 page 数に関係なく 0 または 1
である。`m_Generation` と各 page の `generation` は atomic ではないが、Reset gate が新規
操作を閉じ、入場済み操作が 0 になってから更新する。`m_Current` は現世代へ初期化済みの
page だけを公開する不変条件とし、通常確保は世代を再比較しない。旧 page の世代検査と
`m_Current` 公開は GrowLock 内の低頻度経路で行うため happens-before が成立する。
世代が 64 bit 上限で循環する一度だけ全 page を再標識する。`Reset(true)` は従来どおり全
page を backing へ返すため O(N) である。

### Enum と直列化

`TContiguousEnumLookup` は 0 始まりで欠番のない enum の有効範囲と名前列を constexpr に
生成する。AnimationCurve の補間、wrap、error と AnimationCurveArchive の wire 検証へ
接続した。範囲外は従来どおり false または `"Unknown"` となる。

`WriteLittleEndian` / `ReadLittleEndian` は 1、2、4、8 byte の整数、浮動小数、enum を
little endian byte 列へ変換する。任意 byte が無効表現になり得る `bool` は compile-time に
拒否する。AcpakReader/Writer と AnimationCurveArchive をこの共通経路へ接続したが、
magic、version、field offset、CRC、予約 byte を含む永続形式は一切変更していない。

## 固定コスト指標

| 対象 | 従来 | Wave I/J |
|---|---:|---:|
| deque の `top` / `bottom` 共有 cache line | 1 | 0 |
| ParallelFor 32 chunk 以下の呼び出し時 heap 確保 | 1 | 0 |
| ParallelFor 128 chunk の per-call OS heap block | 1 | 0、固定 pool 2 block |
| ParallelFor 8 同時呼び出しの pool high-water | 観測不可 | 16 / 上限 32 block |
| arena の N 領域 batch cursor 予約 | N | 1 |
| arena の通常 `Reset(false)` page visit | N | 0 または 1 |
| 3 値 enum の妥当性比較上限 | 3 | 1 範囲検査 |

時間値は診断にのみ用い、上表の構造カウンタ、exact byte、並行性 stress を合否判定に使う。

### Release 計測

Visual Studio 18 2026、x64 Release、同一 PC で、基準 `13b5a61` と Wave I/J を交互に 31 回
実行した。各実行は warm-up 500 回後、24 byte・alignment 8 の通常 `Alloc` を 256 件 x
4000 reset 世代、合計 1,024,000 件測る。初案は中央値 19.573 ms から 20.509 ms
（+4.79%）へ退行したため採用せず、`m_Current` の現世代不変条件を使って冗長な世代比較を
低頻度経路へ移し、共通予約処理を強制 inline 化した。最終値は基準 19.592 ms、Wave I/J
19.589 ms、差 -0.015% であり通常確保の退行は計測ノイズ内だった。

最終 Release 診断 benchmark では、256 領域 x 4000 回が個別確保 19.390 ms、batch
0.402 ms、`batch_allocations=1`、`batch_suballocations=256` だった。ParallelFor は
200 回ずつの 16/128 chunk 実行を 15.668 ms で完了し、inline 200、pool block 400、
high-water 2、heap 0 だった。時間は環境依存の参考値であり、回数と heap 0 を
決定的な合否値とする。

## 公開 layout

Win64 の公開診断 snapshot は次の予算で compile-time 固定する。

- `FThreadPoolDiagnostics`: 既存 96 byte、alignment 8 を維持する。
- `FParallelForDiagnostics`: 40 byte、alignment 8。inline、累計 pool、現在貸出、
  最大貸出、heap の 5 個の `u64` だけを持つ。
- `FArenaAllocatorDiagnostics`: 40 byte、alignment 8。5 個の `u64` 診断値だけを持つ。
- `FArenaAllocator`: Win64 で旧 80 byte から 120 byte。追加 40 byte は page 世代 8 byte と、
  batch 2 値、直前 reset page visit、遅延 reset の 4 個の `u64` 診断値である。arena は
  frame/phase owner ごとに置く型であり、要素ごとには保持しない。通常単発確保に新しい
  atomic RMW はなく、常駐増加と reset/batch の決定的観測を交換する。

両型は owner の状態や寿命を持たない値 snapshot であり、各 owner header から参照する。

## 検証

`ACS.FoundationOptimizationWaveI` は endian exact byte、constexpr enum 境界、arena batch、
旧世代 pointer 拒否、Reset(false)/true、巨大 alignment 失敗時の状態不変、4 thread と
500 reset の競合、ParallelFor inline/pool、8 同時呼び出しでの 16 block high-water、
Shutdown 競合を検証する。AnimationCurve と AssetPack の既存 persistence/safety test を
全 unit で再実行し、永続 byte 互換を確認する。最終結果は Release で専用 6/6・全体
1138/1138、Debug で専用 6/6・全体 1142/1142、両構成の専用テスト反復は各 100/100
だった。C++ 規約、module source、reference、配布 header の各監査も通過した。

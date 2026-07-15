<!-- SPDX-License-Identifier: Apache-2.0 -->
# メモリ並行安全 / Memory Concurrency Safety

ACS のアロケータと診断は、**Init/Shutdown/再初期化を使用中と並行して呼んでも安全**な
ように設計する。本書はその仕組み——ライフサイクルゲート、mimalloc heap-lock、猶予回収、
確保状態機械、各アロケータのスレッド安全契約、ロック順序——を1箇所にまとめる。これらは
実測で見つけた並行バグ（下記 mimalloc の項）を含む知見の集約であり、**改変時に破ってはい
けない不変条件**を明文化する。

---

## 1. ライフサイクルゲート / Lifecycle gate

`FMimallocAllocator` と `FShardedTlsfAllocator` が共有する、公開操作（Alloc/Free/Realloc/
統計取得）と保守操作（Init/Shutdown/Collect/InspectHeap）を直列化する機構。1 つの 64bit
atomic `m_LifecycleGate` に 3 情報を詰める。

| ビット | 意味 |
|---|---|
| bit 63 (`kLifecycleAcceptingBit`) | 立っている間だけ新規公開操作を受け付ける |
| 中位ビット (`kLifecycleGenerationMask`) | 停止・再開・再初期化をまたぐ世代 |
| 下位 32bit (`kLifecycleOperationCountMask`) | 実行中の公開操作件数 |

- **`TryBeginLifecycleOperation`**: 受付ビットが立っていて、かつ入場時と同じ世代なら件数を
  CAS で +1 して入場成功。受付停止中／世代変化時は入場失敗（公開 API は nullptr / 既定値を返す）。
- **`EndLifecycleOperation`**: 件数を -1。受付停止中の最後の 1 件だけは待機ロック内で 0 にし、
  条件変数で保守側へ完了通知する（lost-wakeup を防ぐ）。
- **`CloseLifecycleGateAndWait`**: 受付ビットを落とし、件数が 0 になるまで条件変数で待つ
  （drain）。保守操作は制御ロック保持中にこれを呼ぶ。
- **`OpenLifecycleGate`**: 完全初期化後に世代を 1 進め、受付ビットを立てて新規入場を再開する。

**不変条件**: 保守操作が heap を触るのは drain 完了後（実行中の公開操作ゼロ）から次の
`OpenLifecycleGate` までの間だけ。よって保守操作と公開操作は heap 上で相互排他になる。
Shutdown は再開しない（受付停止のまま）。

---

## 2. mimalloc first-class heap の直列化 (m_HeapLock)

**実測で見つかったバグ（2026-07）**: `FMimallocAllocator` は `mi_heap_new()` の first-class
heap を 1 つ共有する。mimalloc の first-class heap は**複数スレッドからの同時 `mi_heap_malloc`
を許さない**（所有スレッド専用）。ライフサイクルゲートは保守と使用は分離するが、複数スレッドの
同時 Alloc は素通ししていた。並行 alloc が heap 内部を破壊し、スレッド終了時の
`_mi_thread_done` で mimalloc 内部 assert が発火してプロセスがハングした。

**対策**: `m_HeapLock`（FMutex）で、入場済み公開操作の全 mimalloc 呼び出しを直列化する。
- `mi_heap_malloc`（Alloc / Realloc）
- `ValidateAllocationMetadata` 内の `mi_heap_contains` / `mi_usable_size`（Free / Realloc / OwnsAllocation）

収集・列挙・破棄・猶予解放（`mi_heap_collect` / `mi_heap_visit_blocks` / `mi_heap_destroy` /
retired の `mi_free`）は**ゲート閉塞下で単一スレッド実行**のため m_HeapLock を取らない
（§1 の相互排他で保証）。

**不変条件**: `m_Heap` への mimalloc 呼び出しは、必ず「m_HeapLock 下」または「ゲート閉塞
（drain 完了）下」のどちらかで行う。新しい `mi_heap_*` 呼び出しを追加するなら必ずこの規約に従う。
将来 per-thread / sharded heap 化すれば直列化を外せる（general セグメントはホットパスでない——
フレーム確保はロックフリー arena——ため現状の直列化で許容できる）。

---

## 3. 猶予回収 / Retired reclamation (mimalloc)

Free/Realloc は物理 `mi_free` を即時に行わず、ブロックを侵入リストへ積む（retire）。理由:
生存ブロックの列挙 `mi_heap_visit_blocks` と `mi_free` が並行すると mimalloc 内部が競合する
ため、物理解放をゲート閉塞下の単一スレッド実行に遅延させる。

- Free/Realloc: 確保状態を `Released` にし、要求量・件数カウンタを即時に減算、生ブロックを
  retired リストへ積む。閾値（既定 64 件 or 1 MiB）到達で回収を要求。
- 物理解放: `CollectRetiredAllocationsIfNeeded`（公開操作の admission を解除した後にゲートを
  閉じて実行）、Collect / InspectHeap / Shutdown のいずれかで、ゲート閉塞下に retired を全解放。

**注意**: `CollectRetiredAllocationsIfNeeded` は必ず自分の admission を解除してから呼ぶ
（入場したまま drain を待つと自己デッドロックする）。

---

## 4. 確保状態機械 / Allocation state machine (mimalloc)

各割り当てのヘッダに atomic な `State`（Active / Reallocating / Released）を持つ。同一ポインタ
への並行 Free/Realloc は、`Active` から次状態への CAS に成功した 1 操作だけが所有権を得る。
競合した操作は旧領域に触れず失敗を返す。これで「1 ポインタを 2 スレッドが同時に解放/再確保」
しても二重解放・UAF にならない（ただし逐次 UAF——解放完了後に古いポインタを再使用——は契約外）。

---

## 5. 各アロケータのスレッド安全契約 / Per-allocator contract

| アロケータ | 並行安全 | 方式 |
|---|---|---|
| `FMimallocAllocator` | ○ | ライフサイクルゲート + m_HeapLock（§1,§2）+ 確保状態機械 + retired 回収 |
| `FShardedTlsfAllocator` | ○ | ライフサイクルゲート + シャード毎ロック。Alloc/Free は所有シャードをロック。thread-local マガジンはロックフリー hot path |
| `FTlsfAllocator` | ✕ (単体) | 単一インスタンスは非同期。ShardedTlsf のシャードとして専用ロック下で使う前提 |
| `FLinearAllocator` | ○ | `m_Used` の CAS でロックフリー bump。Reset/寿命は制御スピンロックで直列化 |
| `FArenaAllocator` | ○ | ページ毎 `used` の CAS でロックフリー bump。新ページ確保時のみ grow ロック |
| `FPoolAllocator` | ○ | FMutex で Alloc/Free を保護。ThreadPool から並行利用される |
| `FSystemAllocator` | ○ | 侵入ヘッダに owner/magic/世代を持ち、別インスタンス/旧世代の Free/Realloc を拒否 |
| `FRelocatableAllocator` | ✕ | **設計上単一スレッド専用**。Compact が全ポインタを無効化するため内部同期しない。利用側が保証する |
| `DiligentMemoryAdapter` | ○ | レジストリを RwLock で保護、bind/寿命を別 RwLock、世代検証で stale free を拒否。backing FAllocator へ委譲 |

---

## 6. ロック順序 / Lock ordering

デッドロックを避けるため、各サブシステム内で以下の順序を厳守する（下位ロックを保持したまま
上位ロックを取らない）。

- **mimalloc**: `m_LifecycleControlLock`（保守の直列化）→ `m_LifecycleDrainLock`（drain 待機、
  一時）／ `m_RetiredAllocationLock` → `m_HeapLock`（leaf）。m_HeapLock は常に単独・短命で取り、
  内部で他ロックを取らない。drain 待機中は保守側が m_HeapLock を保持しないので、入場済み操作は
  m_HeapLock を取って完了でき、drain は進む。
- **ShardedTlsf**: `m_LifecycleControlLock` → thread-cache レジストリ → シャードロック。TLS
  デストラクタは「レジストリ → シャード」だけを取り、シャード保持中に上位を取らない。
- **DiligentMemoryAdapter**: `m_LifetimeLock`（Allocate/Free は共有、bind/shutdown は排他）→
  レジストリ `m_Lock`（排他）。

---

## 7. 検証 / Verification

並行安全の変更は Debug / Release / AddressSanitizer / Diligent(GPU) の全構成で検証する
（[MemoryDiagnostics.md](MemoryDiagnostics.md) の検証マトリクス参照）。データ競合は単一実行では
確率的にしか顕在化しないため、**Debug 全スイートのストレスループ反復**（過去に mimalloc の
ハングと Logger の Flush 競合を発見）と ASan を併用する。並行契約テストは
`acs/tests/mimalloc_allocator_tests.cpp` / `sharded_tlsf_lifecycle_tests.cpp` /
`linear_allocator_lifecycle_tests.cpp` / `win32_resource_tests.cpp` 等にある。

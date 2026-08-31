<!-- SPDX-License-Identifier: Apache-2.0 -->
# メモリの並行安全

`FMimallocAllocator` と `FShardedTlsfAllocator` は、ライフサイクルゲートで公開操作と保守操作を直列化します。`FTlsfAllocator` と `FRelocatableAllocator` は内部同期を持たないため、並行利用時は呼び出し側が直列化します。ここでは各アロケータの並行安全契約とロック順序をまとめます。

---

## 1. ライフサイクルゲート

`FMimallocAllocator` と `FShardedTlsfAllocator` が共有する、公開操作（`Alloc` / `Free` / `Realloc` /
統計取得）と保守操作（`Init` / `Shutdown` / `Collect` / `InspectHeap`）を直列化する機構です。1 つの 64 ビット
アトミック値 `m_LifecycleGate` に 3 種類の情報を保持します。

| ビット | 意味 |
|---|---|
| 第63ビット（`kLifecycleAcceptingBit`） | 立っている間だけ新規公開操作を受け付ける |
| 中位ビット (`kLifecycleGenerationMask`) | 停止・再開・再初期化をまたぐ世代 |
| 下位32ビット (`kLifecycleOperationCountMask`) | 実行中の公開操作件数 |

- **`TryBeginLifecycleOperation`**: 受付ビットが立っていて、かつ入場時と同じ世代なら件数を
  CAS で 1 増やして入場します。受付停止中または世代変化時は入場に失敗し、公開 API は `nullptr` または既定値を返します。
- **`EndLifecycleOperation`**: 件数を 1 減らします。受付停止中の最後の 1 件だけは待機ロック内で 0 にし、
  条件変数で保守側へ完了を通知して通知の取りこぼしを防ぎます。
- **`CloseLifecycleGateAndWait`**: 受付ビットを落とし、件数が 0 になるまで条件変数で待つ
  （排出完了）。保守操作は制御ロック保持中にこれを呼びます。
- **`OpenLifecycleGate`**: 完全初期化後に世代を 1 進め、受付ビットを立てて新規入場を再開します。

**不変条件**: 保守操作がヒープを扱うのは排出完了後（実行中の公開操作が 0）から次の
`OpenLifecycleGate` までの間だけです。このため、保守操作と公開操作はヒープ上で相互排他になります。
`Shutdown` 後は受付を再開しません。

---

## 2. mimalloc 専用ヒープの直列化

`FMimallocAllocator` は `mi_heap_new()` で作る専用ヒープを1つ共有します。このヒープへ複数スレッドから同時に `mi_heap_malloc` を呼ばないよう、すべてのアクセスを `m_HeapLock` で直列化します。ライフサイクルゲートだけでは使用者同士の同時アクセスを排除しないため、ヒープ固有のロックを省略できません。

**対策**: `m_HeapLock`（`FMutex`）で、入場済み公開操作の全 mimalloc 呼び出しを直列化します。
- `mi_heap_malloc`（`Alloc` / `Realloc`）
- `ValidateAllocationMetadata` 内の `mi_heap_contains` / `mi_usable_size`（`Free` / `Realloc` / `OwnsAllocation`）

収集・列挙・破棄・猶予解放（`mi_heap_collect` / `mi_heap_visit_blocks` / `mi_heap_destroy` /
退役済み領域の `mi_free`）は**ゲート閉塞下で単一スレッド実行**するため `m_HeapLock` を取りません
（§1 の相互排他で保証）。

**不変条件**: `m_Heap` への mimalloc 呼び出しは、必ず「`m_HeapLock` 保持中」または「ゲート閉塞
（排出完了）後」のどちらかで行います。新しい `mi_heap_*` 呼び出しもこの規約に従います。

---

## 3. 退役済み領域の猶予回収

`Free` / `Realloc` は物理 `mi_free` を即時に行わず、ブロックを侵入リストへ積んで退役させます。
生存ブロックの列挙 `mi_heap_visit_blocks` と `mi_free` が並行すると mimalloc 内部が競合する
ため、物理解放をゲート閉塞下の単一スレッド実行に遅延させます。

- `Free` / `Realloc`: 確保状態を `Released` にし、要求量・件数カウンターを即時に減算し、生存ブロックを
  退役リストへ積みます。しきい値（既定 64 件または 1 MiB）へ到達すると回収を要求します。
- 物理解放: `CollectRetiredAllocationsIfNeeded`（公開操作の入場権を解除した後にゲートを
  閉じて実行）、`Collect` / `InspectHeap` / `Shutdown` のいずれかで、ゲート閉塞下に退役済み領域をすべて解放します。

**注意**: `CollectRetiredAllocationsIfNeeded` は必ず自分の入場権を解除してから呼びます。
入場したまま排出完了を待つと自己デッドロックします。

---

## 4. 確保状態機械

各割り当てのヘッダーにアトミックな `State`（`Active` / `Reallocating` / `Released`）を保持します。同一ポインター
への並行 `Free` / `Realloc` は、`Active` から次状態への CAS に成功した 1 操作だけが所有権を得ます。
競合した操作は旧領域に触れず失敗を返します。これにより、1 つのポインターを 2 スレッドが同時に解放または再確保しても二重解放や解放後参照を防ぎます。解放完了後に古いポインターを再使用する逐次的な解放後参照は契約外です。

---

## 5. 各アロケータのスレッド安全契約

| アロケータ | 並行安全 | 方式 |
|---|---|---|
| `FMimallocAllocator` | ○ | ライフサイクルゲート + `m_HeapLock`（§1、§2）+ 確保状態機械 + 退役済み領域の回収 |
| `FShardedTlsfAllocator` | ○ | ライフサイクルゲート + シャードごとのロック。`Alloc` / `Free` は所有シャードをロック。スレッドローカルマガジンはロックなしの高速経路 |
| `FTlsfAllocator` | ✕（単体） | 単体では並行安全ではありません。`FShardedTlsfAllocator` のシャードとして専用ロック下で使用します |
| `FLinearAllocator` | ○ | `m_Used` の CAS でロックなしに前進。`Reset` と寿命操作は制御スピンロックで直列化 |
| `FArenaAllocator` | ○ | ページごとの `used` の CAS でロックなしに前進。新ページ確保時だけ拡張ロックを使用 |
| `FPoolAllocator` | ○ | `FMutex` で `Alloc` / `Free` を保護。`CThreadPool` から並行利用される |
| `FSystemAllocator` | ○ | 侵入ヘッダーに所有元、マジック値、世代を持ち、別インスタンスまたは旧世代の `Free` / `Realloc` を拒否 |
| `FRelocatableAllocator` | ✕ | **設計上単一スレッド専用**。`Compact` が全ポインターを無効化するため内部同期しない。利用側が保証する |
| `CDiligentMemoryAdapter` | ○ | レジストリを `FRwLock` で保護し、割り当て先と寿命を別の `FRwLock` で保護します。世代検証で古い `Free` を拒否し、基盤の `FAllocator` へ委譲 |

---

## 6. ロック順序

デッドロックを避けるため、各サブシステム内で以下の順序を厳守する（下位ロックを保持したまま
上位ロックを取らない）。

- **mimalloc**: `m_LifecycleControlLock`（保守の直列化）→ `m_LifecycleDrainLock`（排出完了の待機、
  一時）／ `m_RetiredAllocationLock` → `m_HeapLock`（最下位）。`m_HeapLock` は常に単独・短命で取り、
  内部で他ロックを取りません。排出完了の待機中は保守側が `m_HeapLock` を保持しないため、入場済み操作は
  `m_HeapLock` を取得して完了でき、排出処理が進みます。
- **`FShardedTlsfAllocator`**: `m_LifecycleControlLock` → スレッドキャッシュレジストリ → シャードロック。TLS
  デストラクタは「レジストリ → シャード」だけを取り、シャード保持中に上位を取らない。
- **`CDiligentMemoryAdapter`**: `m_LifetimeLock`（`Allocate` / `Free` は共有、割り当て先設定 / `Shutdown` は排他）→
  レジストリ `m_Lock`（排他）。

---

## 7. 検証

並行安全の変更は Debug / Release / AddressSanitizer / Diligent（GPU）の全構成で検証します。
[メモリ診断](../../operations/diagnostics/memory.md)の検証表も参照してください。データ競合は単一実行で再現しない場合があるため、Debug 全テストの反復実行と AddressSanitizer を併用します。並行契約テストは
`.\tests\mimalloc_allocator_tests.cpp`、`.\tests\sharded_tlsf_lifecycle_tests.cpp`、
`.\tests\linear_allocator_lifecycle_tests.cpp`、`.\tests\win32_resource_tests.cpp` などにあります。

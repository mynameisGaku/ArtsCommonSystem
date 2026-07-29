// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS ECS — FParallelEntityCommandBuffer (EachParallel 用の per-worker 遅延記録)
// -----------------------------------------------------------------------------
// Query::EachParallel の fn は FWorld を構造変更 (Add/Remove/Destroy) してはならない
// (Query.h の契約参照)。また FEntityCommandBuffer は単一スレッド前提なので、複数
// ワーカーから同じバッファへ記録すると TArray が競合して壊れる。
//
// 本クラスは «ワーカー数 + 1» 本の FEntityCommandBuffer を持ち、記録時に
// FThreadPool::CurrentWorkerIndex() で自スレッド専用のバッファへ振り分けることで、
// EachParallel の fn 内からロックなしで安全に構造変更を記録できるようにする。
// +1 本は非ワーカースレッド用 (ParallelFor の呼び出し元は Wait 中に仕事を
// 盗んで body を実行するため、worker index を持たないスレッドでも記録が起きる)。
//
// 例:
//   FParallelEntityCommandBuffer cmd(world);
//   world.Query<FHealth>().EachParallel([&](FEntityId e, FHealth& h) {
//       if (h.value <= 0) cmd.Destroy(e);   // 自ワーカー専用バッファへ記録 (ロック不要)
//   });
//   cmd.Flush();                            // ParallelFor 完了後に単一スレッドで一括適用
//
// スレッド安全性の契約:
//   ・Destroy/Add/Remove: プールワーカー + «1 本だけの» 非ワーカースレッド (通常は
//     EachParallel の呼び出し元) から並行可。非ワーカー 2 スレッド以上からの並行
//     記録は同じ予備スロットを共有するため不可。
//   ・Flush/Clear/Size/HasOverflowed: 記録が全て完了した後 (= EachParallel が
//     return した後) に単一スレッドから呼ぶこと。
//   ・FThreadPool::Init より後に構築すること (スロット数を構築時の WorkerCount()
//     で確定するため。後から Init してワーカーが増えた場合、範囲外 index の記録は
//     落として HasOverflowed()=true で検知できる)。
//
// 適用順の注意: Flush はスロット順 (worker 0, 1, ..., 非ワーカー) に各バッファを
// 記録順で適用する。スロットをまたいだ操作の相対順は実行タイミング依存なので、
// 順序に意味がある操作列は同一エンティティ内 (= 同一 fn 呼び出し内) に閉じること。
// =============================================================================
#pragma once

#include "ecs/EntityCommandBuffer.h"
#include "threading/ThreadPool.h"
#include "threading/Atomic.h"

namespace acs {

/**
 * EachParallel の fn から安全に構造変更を記録するための per-worker コマンドバッファ束。
 *
 * @details ワーカー数 + 1 (非ワーカー用) 本の FEntityCommandBuffer を持ち、記録は
 * 自スレッド専用スロットへロックなしで積む。Flush は単一スレッドで全スロットを適用する。
 * 非コピー。
 */
class FParallelEntityCommandBuffer {
public:
    /**
     * 適用先 FWorld とアロケータを束ね、ワーカー数 + 1 本のバッファを確保して構築する。
     *
     * @details 確保に失敗したら以降の記録は落ち、HasOverflowed() が true になる
     * (IsValid() で構築成否を確認できる)。FThreadPool::Init より後に構築すること。
     * @param world 適用先の FWorld (参照を保持)。
     * @param alloc 各バッファの記録・退避に使うアロケータ。
     */
    explicit FParallelEntityCommandBuffer(FWorld& world, FAllocator& alloc = DefaultAllocator()) noexcept
        : m_Alloc(&alloc), m_Buffers(alloc)
    {
        const u32 slots = FThreadPool::WorkerCount() + 1u;   // +1 = 非ワーカー用予備
        if (!m_Buffers.TryReserve(slots)) {
            return;                                          // IsValid()=false (記録は全て落ちる)
        }
        for (u32 i = 0; i < slots; ++i) {
            FEntityCommandBuffer* const buffer = New<FEntityCommandBuffer>(alloc, world, alloc);
            if (buffer == nullptr || !m_Buffers.TryPushBack(buffer)) {
                if (buffer != nullptr) Delete(alloc, buffer);
                ReleaseBuffers();                            // 部分構築は全て畳んで無効化
                return;
            }
        }
    }

    /** 未適用の記録があれば破棄し、全バッファを解放する。 */
    ~FParallelEntityCommandBuffer() noexcept
    {
        ReleaseBuffers();
    }

    FParallelEntityCommandBuffer(const FParallelEntityCommandBuffer&) = delete;
    FParallelEntityCommandBuffer& operator=(const FParallelEntityCommandBuffer&) = delete;

    /**
     * エンティティ破棄を自スレッド専用スロットへ記録する。
     *
     * @param e 破棄するエンティティ。
     */
    void Destroy(FEntityId e) noexcept
    {
        if (FEntityCommandBuffer* const buffer = Slot()) buffer->Destroy(e);
    }

    /**
     * T コンポーネントの追加を自スレッド専用スロットへ記録する (値はヒープ退避)。
     *
     * @tparam T 追加するコンポーネント型。
     * @param e 追加先のエンティティ。
     * @param value 格納する値 (ムーブで退避する)。
     */
    template<typename T>
    void Add(FEntityId e, T value) noexcept
    {
        if (FEntityCommandBuffer* const buffer = Slot()) buffer->Add<T>(e, Move(value));
    }

    /**
     * T コンポーネントの除去を自スレッド専用スロットへ記録する。
     *
     * @tparam T 除去するコンポーネント型。
     * @param e 除去対象のエンティティ。
     */
    template<typename T>
    void Remove(FEntityId e) noexcept
    {
        if (FEntityCommandBuffer* const buffer = Slot()) buffer->Remove<T>(e);
    }

    /**
     * 空エンティティの生成を自スレッド専用スロットへ記録する (Flush 時に生成)。
     *
     * @details FWorld::Create はスレッドセーフでないため、EachParallel 中の生成は本記録を使う。
     */
    void Create() noexcept
    {
        if (FEntityCommandBuffer* const buffer = Slot()) buffer->Create();
    }

    /**
     * 「生成 + T を付与」を自スレッド専用スロットへ記録する (Flush 時に生成)。
     *
     * @tparam T 生成と同時に付与するコンポーネント型。
     * @param value 格納する値 (ムーブで退避する)。
     */
    template<typename T>
    void CreateWith(T value) noexcept
    {
        if (FEntityCommandBuffer* const buffer = Slot()) buffer->CreateWith<T>(Move(value));
    }

    /**
     * 全スロットの記録を FWorld へ適用し、空にする (単一スレッドから呼ぶこと)。
     *
     * @details スロット順 (worker 0..N-1, 非ワーカー) に、各スロット内は記録順で適用する。
     */
    void Flush() noexcept
    {
        for (usize i = 0; i < m_Buffers.Size(); ++i) {
            m_Buffers[i]->Flush();
        }
    }

    /** 全スロットの記録を適用せず破棄する (単一スレッドから呼ぶこと)。 */
    void Clear() noexcept
    {
        for (usize i = 0; i < m_Buffers.Size(); ++i) {
            m_Buffers[i]->Clear();
        }
    }

    /**
     * 各ワーカースロットに同じ件数を事前予約する。記録開始前に単一スレッドから呼ぶ。
     */
    bool TryReservePerSlot(usize command_count) noexcept
    {
        bool ok = !m_Buffers.IsEmpty();
        for (usize i = 0; i < m_Buffers.Size(); ++i) {
            ok = m_Buffers[i]->TryReserve(command_count) && ok;
        }
        if (!ok) m_DroppedRecords.FetchAdd(1u);
        return ok;
    }

    /**
     * 全スロットの記録済み操作数の合計を返す (単一スレッドから呼ぶこと)。
     *
     * @return 未適用の記録数。
     */
    usize Size() const noexcept
    {
        usize total = 0;
        for (usize i = 0; i < m_Buffers.Size(); ++i) {
            total += m_Buffers[i]->Size();
        }
        return total;
    }

    /**
     * 構築 (全スロットの確保) に成功しているかを返す。
     *
     * @return 使用可能なら true。false のとき記録は全て落ちる。
     */
    bool IsValid() const noexcept
    {
        return !m_Buffers.IsEmpty();
    }

    /**
     * どこかで記録を落としたことがあるかを返す (単一スレッドから呼ぶこと)。
     *
     * @return 構築失敗 / スロット OOM / 範囲外 worker index が一度でもあれば true。
     */
    bool HasOverflowed() const noexcept
    {
        if (m_DroppedRecords.Load(EMemoryOrder::Relaxed) != 0u) return true;
        for (usize i = 0; i < m_Buffers.Size(); ++i) {
            if (m_Buffers[i]->HasOverflowed()) return true;
        }
        return false;
    }

private:
    /**
     * 自スレッド専用スロットを返す (無ければ記録落ちとして数えて nullptr)。
     *
     * @details ワーカーは自 index、非ワーカーは末尾の予備スロット。構築失敗時や、
     * 構築後の FThreadPool::Init でワーカーが増えて index が範囲外になった場合は
     * nullptr を返し、m_DroppedRecords を進める (HasOverflowed で検知)。
     * @return 自スレッドが専有するバッファ。使用不能なら nullptr。
     */
    FEntityCommandBuffer* Slot() noexcept
    {
        const usize count = m_Buffers.Size();
        if (count == 0) {
            m_DroppedRecords.FetchAdd(1u);
            return nullptr;
        }
        const u32 worker = FThreadPool::CurrentWorkerIndex();
        const usize slot = (worker == FThreadPool::kNotAWorker)
                               ? count - 1                        // 非ワーカー用の予備 (末尾)
                               : static_cast<usize>(worker);
        if (slot >= count) {
            m_DroppedRecords.FetchAdd(1u);                        // 構築後に Init されて増えたワーカー
            return nullptr;
        }
        return m_Buffers[slot];
    }

    /** 全バッファを (未適用の記録ごと) 破棄して解放する。 */
    void ReleaseBuffers() noexcept
    {
        for (usize i = 0; i < m_Buffers.Size(); ++i) {
            Delete(*m_Alloc, m_Buffers[i]);
        }
        m_Buffers.Clear();
    }

    /** バッファの確保・解放に使うアロケータ。 */
    FAllocator* m_Alloc = nullptr;

    /** スロット別バッファ (index = worker index、末尾 = 非ワーカー用)。 */
    TArray<FEntityCommandBuffer*> m_Buffers;

    /** スロット不在で落とした記録数 (並行記録から進めるため atomic)。 */
    TAtomic<u32> m_DroppedRecords{0u};
};

} // namespace acs

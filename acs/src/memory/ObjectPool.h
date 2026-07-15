// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — TObjectPool<T> / FObjectHandle / TPoolRef<T>
//   世代付きスロットマッププール (generational slot map)。
// -----------------------------------------------------------------------------
// 「GC の代替となる、速いオブジェクト寿命管理」。UE の UObject GC のような mark-sweep
// ではなく、ハンドル + プール方式で:
//   ・Create / Destroy が O(1) (フリーリスト再利用)
//   ・GC のような «ポーズ» が一切無い (決定的)
//   ・オブジェクトのアドレスが安定 (チャンク格納。TArray 再確保で動かない)
//   ・解放後の参照は «世代カウンタ» で安全に無効化 (dangling を検出して nullptr)
//   ・生存オブジェクトを密な配列で反復 (キャッシュ効率)
//
// 例:
//   TObjectPool<FEnemy> pool;
//   FObjectHandle h = pool.Create(/*ctor args*/);
//   if (FEnemy* e = pool.Get(h)) e->Update(dt);   // 生きていれば取得
//   pool.Destroy(h);                              // 解放 → 以後 Get(h) は nullptr
//   pool.ForEach([](FEnemy& e, FObjectHandle){ e.Tick(); });
//
// ACS_CLASS で定義したユーザー型もそのまま T に使える (リフレクションとは独立)。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Move.h" // Forward / 配置 new
#include "container/Array.h"
#include "memory/New.h"
#include "memory/Allocator.h"

namespace acs {

/**
 * プール内オブジェクトへの世代付きハンドル。
 *
 * @details index = スロット番号、gen = そのスロットの世代。スロットが解放/再利用されると
 * 世代が進み、古い (gen 不一致) ハンドルは無効と判定される (use-after-free を安全に検出)。
 * 世代は 64bit とし、長時間の高 churn でも旧ハンドルが現実的に再一致しない。
 */
struct FObjectHandle {
    static constexpr u32 kInvalidIndex = 0xFFFFFFFFu;

    u32 index = kInvalidIndex; /**< スロット番号 (未設定は kInvalidIndex)。 */
    u64 gen = 0;               /**< スロットの世代 (確保/解放で進む)。 */

    /** 何らかのスロットを指していれば true (生存判定ではない)。 */
    bool IsSet() const noexcept
    {
        return index != kInvalidIndex;
    }
    bool operator==(const FObjectHandle& Other) const noexcept
    {
        return index == Other.index && gen == Other.gen;
    }
    bool operator!=(const FObjectHandle& Other) const noexcept
    {
        return !(*this == Other);
    }
};

/**
 * 世代付きスロットマッププール。任意の T を O(1) で確保/解放し、ハンドルで安全に参照する。
 *
 * @tparam T 格納するオブジェクト型 (FObject 継承や reflection は不要。任意の型)。
 */
template<class T>
class TObjectPool {
public:
    using Handle = FObjectHandle;

    /** 1 チャンクのスロット数 (チャンク単位で確保 → アドレス安定)。 */
    static constexpr u32 kChunkSize = 256u;

    /** 確保元アロケータを指定して空のプールを作る。 */
    explicit TObjectPool(FAllocator& Allocator = DefaultAllocator()) noexcept
        : m_Alloc(&Allocator), m_Chunks(Allocator), m_Free(Allocator), m_Live(Allocator)
    {
    }

    /** 生存オブジェクトを破棄し、全ての保持領域を解放する。 */
    ~TObjectPool() noexcept
    {
        ReleaseStorage();
    }

    TObjectPool(const TObjectPool&) = delete;
    TObjectPool& operator=(const TObjectPool&) = delete;

    /**
     * オブジェクトを 1 つ構築してハンドルを返す。
     *
     * @details フリーリストに空きがあれば再利用、無ければ新スロットを足す (チャンクは必要時のみ確保)。
     * @param Arguments T のコンストラクタへ転送する引数。
     * @return 構築したオブジェクトの世代付きハンドル。
     */
    template<class... Args>
    Handle Create(Args&&... Arguments) noexcept
    {
        u32 SlotIndex = Handle::kInvalidIndex;
        bool bReusedSlot = false;
        if (m_Free.Size() > 0) {
            SlotIndex = m_Free[m_Free.Size() - 1];
            m_Free.PopBack();
            bReusedSlot = true;
        } else {
            if (m_SlotCount >= Handle::kInvalidIndex) return {};
            SlotIndex = static_cast<u32>(m_SlotCount);
            if (!EnsureChunkFor(SlotIndex)) return {};
        }

        if (!m_Live.TryPushBack(SlotIndex)) {
            if (bReusedSlot) m_Free.PushBack(SlotIndex);
            return {};
        }

        Slot& PoolSlot = SlotRef(SlotIndex);
        PoolSlot.liveIdx = static_cast<u32>(m_Live.Size() - 1u);
        ::new (static_cast<void*>(PoolSlot.storage)) T(Forward<Args>(Arguments)...);
        PoolSlot.alive = true;
        if (!bReusedSlot) ++m_SlotCount;
        return Handle{SlotIndex, PoolSlot.gen};
    }

    /**
     * ハンドルの指すオブジェクトを破棄する (既に死んでいれば何もしない)。
     *
     * @details デストラクタを呼び、スロットの世代を進めて (古いハンドルを無効化し)、スロットを
     * フリーリストへ返す。密な生存リストからは swap-remove で O(1) に外す。
     * @param ObjectHandle 破棄するオブジェクトのハンドル。
     * @return 破棄したら true、無効ハンドルなら false。
     */
    bool Destroy(Handle ObjectHandle) noexcept
    {
        Slot* PoolSlot = Resolve(ObjectHandle);
        if (PoolSlot == nullptr) return false;
        if (!m_Free.TryReserve(m_Free.Size() + 1u)) return false;

        reinterpret_cast<T*>(PoolSlot->storage)->~T();
        PoolSlot->alive = false;
        PoolSlot->gen = AdvanceGeneration(PoolSlot->gen);    // 世代を進める → 既存ハンドルは無効に
        const u32 LastSlotIndex = m_Live[m_Live.Size() - 1]; // 密な生存リストから swap-remove
        m_Live[PoolSlot->liveIdx] = LastSlotIndex;
        SlotRef(LastSlotIndex).liveIdx = PoolSlot->liveIdx;
        m_Live.PopBack();
        m_Free.PushBack(ObjectHandle.index);
        return true;
    }

    /** ハンドルが今も生きていれば対象ポインタを、死んでいれば nullptr を返す。 */
    T* Get(Handle ObjectHandle) noexcept
    {
        Slot* PoolSlot = Resolve(ObjectHandle);
        return (PoolSlot != nullptr) ? reinterpret_cast<T*>(PoolSlot->storage) : nullptr;
    }
    const T* Get(Handle ObjectHandle) const noexcept
    {
        const Slot* PoolSlot = Resolve(ObjectHandle);
        return (PoolSlot != nullptr) ? reinterpret_cast<const T*>(PoolSlot->storage) : nullptr;
    }

    /** ハンドルが生存しているか。 */
    bool IsAlive(Handle ObjectHandle) const noexcept
    {
        return Resolve(ObjectHandle) != nullptr;
    }

    /** 生存オブジェクト数。 */
    u32 Count() const noexcept
    {
        return static_cast<u32>(m_Live.Size());
    }

    /**
     * 生存オブジェクトを密に反復する (キャッシュ効率の良い順)。Function(T&, Handle)。
     *
     * @details 反復中に Create/Destroy しないこと (生存リストを変更するため)。
     */
    template<class Fn>
    void ForEach(Fn&& Function) noexcept
    {
        for (usize i = 0; i < m_Live.Size(); ++i) {
            const u32 SlotIndex = m_Live[i];
            Slot& PoolSlot = SlotRef(SlotIndex);
            Function(*reinterpret_cast<T*>(PoolSlot.storage), Handle{SlotIndex, PoolSlot.gen});
        }
    }

    /** 全生存オブジェクトを破棄する (チャンクは保持。再利用可能)。 */
    void Clear() noexcept
    {
        ACS_CHECKF(TryClear(), "TObjectPool::Clear failed to reserve free-list storage");
    }

    /** 全生存オブジェクトの破棄を試み、管理領域の確保失敗時は何も変更しない。 */
    bool TryClear() noexcept
    {
        if (m_Live.Size() > (~usize(0)) - m_Free.Size() ||
            !m_Free.TryReserve(m_Free.Size() + m_Live.Size())) {
            return false;
        }
        for (usize i = 0; i < m_Live.Size(); ++i) {
            const u32 SlotIndex = m_Live[i];
            Slot& PoolSlot = SlotRef(SlotIndex);
            reinterpret_cast<T*>(PoolSlot.storage)->~T();
            PoolSlot.alive = false;
            PoolSlot.gen = AdvanceGeneration(PoolSlot.gen);
            m_Free.PushBack(SlotIndex);
        }
        m_Live.Clear();
        return true;
    }

    /**
     * 全オブジェクトとチャンク、管理配列の容量を確保元へ返す。
     *
     * @details Clear() と異なりチャンクを再利用しない完全解放。解放前のハンドルは
     * 世代 seed を進めて無効のままにする。
     */
    void ReleaseStorage() noexcept
    {
        // 全スロットの現在世代より先を次チャンクの初期世代にする。単純な seed + 1 では、
        // 同じスロットが複数回再利用済みのときに過去ハンドルと早期一致し得る。
        u64 LatestGeneration = m_GenerationSeed;
        for (usize SlotIndex = 0; SlotIndex < m_SlotCount; ++SlotIndex) {
            const u64 Generation = SlotRef(static_cast<u32>(SlotIndex)).gen;
            if (Generation > LatestGeneration) LatestGeneration = Generation;
        }
        m_GenerationSeed = AdvanceGeneration(LatestGeneration);

        // 完全解放中はフリーリストへ戻さない。Clear() 経由だと破棄処理中に
        // m_Free が拡張され、不要な再確保や確保失敗を招く可能性がある。
        for (usize i = 0; i < m_Live.Size(); ++i) {
            Slot& PoolSlot = SlotRef(m_Live[i]);
            reinterpret_cast<T*>(PoolSlot.storage)->~T();
            PoolSlot.alive = false;
        }
        m_Live.Clear();
        FreeChunks();
        m_Chunks.ReleaseStorage();
        m_Free.ReleaseStorage();
        m_Live.ReleaseStorage();
        m_SlotCount = 0;
    }

private:
    struct Slot {
        u64 gen = 0;
        u32 liveIdx = 0;
        bool alive = false;
        alignas(T) unsigned char storage[sizeof(T)];
    };
    struct Chunk {
        Slot slots[kChunkSize];
    };

    /** 世代を 1 つ進め、wrap 後は初期世代との即時一致を避けるため 0 を飛ばす。 */
    static u64 AdvanceGeneration(u64 Generation) noexcept
    {
        ++Generation;
        if (Generation == 0u) ++Generation;
        return Generation;
    }

    bool EnsureChunkFor(u32 SlotIndex) noexcept
    {
        const u32 ChunkIndex = SlotIndex / kChunkSize;
        while (static_cast<u32>(m_Chunks.Size()) <= ChunkIndex) {
            Chunk* NewChunk = New<Chunk>(*m_Alloc); // チャンクはアドレス固定 (移動しない)
            if (NewChunk == nullptr) return false;
            for (u32 i = 0; i < kChunkSize; ++i)
                NewChunk->slots[i].gen = m_GenerationSeed;
            if (!m_Chunks.TryPushBack(NewChunk)) {
                Delete(*m_Alloc, NewChunk);
                return false;
            }
        }
        return true;
    }
    Slot& SlotRef(u32 SlotIndex) noexcept
    {
        return m_Chunks[SlotIndex / kChunkSize]->slots[SlotIndex % kChunkSize];
    }
    const Slot& SlotRef(u32 SlotIndex) const noexcept
    {
        return m_Chunks[SlotIndex / kChunkSize]->slots[SlotIndex % kChunkSize];
    }

    Slot* Resolve(Handle ObjectHandle) noexcept
    {
        if (ObjectHandle.index >= m_SlotCount) return nullptr;
        Slot& PoolSlot = SlotRef(ObjectHandle.index);
        return (PoolSlot.alive && PoolSlot.gen == ObjectHandle.gen) ? &PoolSlot : nullptr;
    }
    const Slot* Resolve(Handle ObjectHandle) const noexcept
    {
        if (ObjectHandle.index >= m_SlotCount) return nullptr;
        const Slot& PoolSlot = SlotRef(ObjectHandle.index);
        return (PoolSlot.alive && PoolSlot.gen == ObjectHandle.gen) ? &PoolSlot : nullptr;
    }
    void FreeChunks() noexcept
    {
        for (usize i = 0; i < m_Chunks.Size(); ++i)
            Delete(*m_Alloc, m_Chunks[i]);
        m_Chunks.Clear();
    }

    FAllocator* m_Alloc;
    TArray<Chunk*> m_Chunks;  /**< チャンク (各 kChunkSize スロット)。ポインタは固定。 */
    TArray<u32> m_Free;       /**< 再利用可能なスロット番号。 */
    TArray<u32> m_Live;       /**< 生存スロット番号の密な配列 (反復用)。 */
    usize m_SlotCount = 0;    /**< これまでに確保したスロット総数。 */
    u64 m_GenerationSeed = 0; /**< ReleaseStorage 後の旧ハンドル復活を防ぐ初期世代。 */
};

/**
 * プールとハンドルを束ねた「安全な弱参照」ポインタ。アクセスごとにハンドルを解決する
 * (対象が破棄されていれば nullptr)。GC の所有グラフ無しで dangling を防ぐ。
 *
 * @tparam T 参照先の型。
 */
template<class T>
struct TPoolRef {
    TObjectPool<T>* pool = nullptr;
    FObjectHandle handle{};

    TPoolRef() noexcept = default;
    TPoolRef(TObjectPool<T>& Pool, FObjectHandle ObjectHandle) noexcept : pool(&Pool), handle(ObjectHandle)
    {
    }

    /** 生きていれば対象ポインタ、死んでいれば nullptr。 */
    T* Get() const noexcept
    {
        return pool ? pool->Get(handle) : nullptr;
    }
    T* operator->() const noexcept
    {
        return Get();
    }
    T& operator*() const noexcept
    {
        return *Get();
    }
    bool IsValid() const noexcept
    {
        return Get() != nullptr;
    }
    explicit operator bool() const noexcept
    {
        return IsValid();
    }
};

} // namespace acs

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
#include "foundation/Move.h"      // Forward / 配置 new
#include "container/Array.h"
#include "memory/New.h"
#include "memory/Allocator.h"

namespace acs {

/**
 * プール内オブジェクトへの世代付きハンドル。
 *
 * @details index = スロット番号、gen = そのスロットの世代。スロットが解放/再利用されると
 * 世代が進み、古い (gen 不一致) ハンドルは無効と判定される (use-after-free を安全に検出)。
 * 8 バイトで値渡し可能。
 */
struct FObjectHandle {
    static constexpr u32 kInvalidIndex = 0xFFFFFFFFu;

    u32 index = kInvalidIndex;   /**< スロット番号 (未設定は kInvalidIndex)。 */
    u32 gen   = 0;               /**< スロットの世代 (確保/解放で進む)。 */

    /** 何らかのスロットを指していれば true (生存判定ではない)。 */
    bool IsSet() const noexcept { return index != kInvalidIndex; }
    bool operator==(const FObjectHandle& o) const noexcept { return index == o.index && gen == o.gen; }
    bool operator!=(const FObjectHandle& o) const noexcept { return !(*this == o); }
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
    explicit TObjectPool(FAllocator& alloc = DefaultAllocator()) noexcept : m_Alloc(&alloc) {}

    /** 生存オブジェクトを破棄し、チャンクを解放する。 */
    ~TObjectPool() noexcept { Clear(); FreeChunks(); }

    TObjectPool(const TObjectPool&) = delete;
    TObjectPool& operator=(const TObjectPool&) = delete;

    /**
     * オブジェクトを 1 つ構築してハンドルを返す。
     *
     * @details フリーリストに空きがあれば再利用、無ければ新スロットを足す (チャンクは必要時のみ確保)。
     * @param args T のコンストラクタへ転送する引数。
     * @return 構築したオブジェクトの世代付きハンドル。
     */
    template<class... Args>
    Handle Create(Args&&... args) noexcept {
        u32 slot;
        if (m_Free.Size() > 0) {
            slot = m_Free[m_Free.Size() - 1];
            m_Free.PopBack();
        } else {
            slot = static_cast<u32>(m_SlotCount);
            EnsureChunkFor(slot);
            ++m_SlotCount;
        }
        Slot& s = SlotRef(slot);
        s.alive   = true;
        s.liveIdx = static_cast<u32>(m_Live.Size());
        m_Live.PushBack(slot);
        ::new (static_cast<void*>(s.storage)) T(Forward<Args>(args)...);
        return Handle{ slot, s.gen };
    }

    /**
     * ハンドルの指すオブジェクトを破棄する (既に死んでいれば何もしない)。
     *
     * @details デストラクタを呼び、スロットの世代を進めて (古いハンドルを無効化し)、スロットを
     * フリーリストへ返す。密な生存リストからは swap-remove で O(1) に外す。
     * @param h 破棄するオブジェクトのハンドル。
     * @return 破棄したら true、無効ハンドルなら false。
     */
    bool Destroy(Handle h) noexcept {
        Slot* s = Resolve(h);
        if (s == nullptr) return false;
        reinterpret_cast<T*>(s->storage)->~T();
        s->alive = false;
        ++s->gen;                                   // 世代を進める → 既存ハンドルは無効に
        const u32 last = m_Live[m_Live.Size() - 1]; // 密な生存リストから swap-remove
        m_Live[s->liveIdx]      = last;
        SlotRef(last).liveIdx   = s->liveIdx;
        m_Live.PopBack();
        m_Free.PushBack(h.index);
        return true;
    }

    /** ハンドルが今も生きていれば対象ポインタを、死んでいれば nullptr を返す。 */
    T* Get(Handle h) noexcept {
        Slot* s = Resolve(h);
        return (s != nullptr) ? reinterpret_cast<T*>(s->storage) : nullptr;
    }
    const T* Get(Handle h) const noexcept {
        const Slot* s = Resolve(h);
        return (s != nullptr) ? reinterpret_cast<const T*>(s->storage) : nullptr;
    }

    /** ハンドルが生存しているか。 */
    bool IsAlive(Handle h) const noexcept { return Resolve(h) != nullptr; }

    /** 生存オブジェクト数。 */
    u32 Count() const noexcept { return static_cast<u32>(m_Live.Size()); }

    /**
     * 生存オブジェクトを密に反復する (キャッシュ効率の良い順)。fn(T&, Handle)。
     *
     * @details 反復中に Create/Destroy しないこと (生存リストを変更するため)。
     */
    template<class Fn>
    void ForEach(Fn&& fn) noexcept {
        for (usize i = 0; i < m_Live.Size(); ++i) {
            const u32 slot = m_Live[i];
            Slot& s = SlotRef(slot);
            fn(*reinterpret_cast<T*>(s.storage), Handle{ slot, s.gen });
        }
    }

    /** 全生存オブジェクトを破棄する (チャンクは保持。再利用可能)。 */
    void Clear() noexcept {
        for (usize i = 0; i < m_Live.Size(); ++i) {
            const u32 slot = m_Live[i];
            Slot& s = SlotRef(slot);
            reinterpret_cast<T*>(s.storage)->~T();
            s.alive = false;
            ++s.gen;
            m_Free.PushBack(slot);
        }
        m_Live.Clear();
    }

private:
    struct Slot {
        u32  gen     = 0;
        u32  liveIdx = 0;
        bool alive   = false;
        alignas(T) unsigned char storage[sizeof(T)];
    };
    struct Chunk { Slot slots[kChunkSize]; };

    void EnsureChunkFor(u32 slot) noexcept {
        const u32 chunk = slot / kChunkSize;
        while (static_cast<u32>(m_Chunks.Size()) <= chunk) {
            Chunk* c = New<Chunk>(*m_Alloc);   // チャンクはアドレス固定 (移動しない)
            m_Chunks.PushBack(c);
        }
    }
    Slot&       SlotRef(u32 slot)       noexcept { return m_Chunks[slot / kChunkSize]->slots[slot % kChunkSize]; }
    const Slot& SlotRef(u32 slot) const noexcept { return m_Chunks[slot / kChunkSize]->slots[slot % kChunkSize]; }

    Slot* Resolve(Handle h) noexcept {
        if (h.index >= m_SlotCount) return nullptr;
        Slot& s = SlotRef(h.index);
        return (s.alive && s.gen == h.gen) ? &s : nullptr;
    }
    const Slot* Resolve(Handle h) const noexcept {
        if (h.index >= m_SlotCount) return nullptr;
        const Slot& s = SlotRef(h.index);
        return (s.alive && s.gen == h.gen) ? &s : nullptr;
    }
    void FreeChunks() noexcept {
        for (usize i = 0; i < m_Chunks.Size(); ++i) Delete(*m_Alloc, m_Chunks[i]);
        m_Chunks.Clear();
    }

    FAllocator*    m_Alloc;
    TArray<Chunk*> m_Chunks;     /**< チャンク (各 kChunkSize スロット)。ポインタは固定。 */
    TArray<u32>    m_Free;       /**< 再利用可能なスロット番号。 */
    TArray<u32>    m_Live;       /**< 生存スロット番号の密な配列 (反復用)。 */
    usize          m_SlotCount = 0;   /**< これまでに確保したスロット総数。 */
};

/**
 * プールとハンドルを束ねた「安全な弱参照」ポインタ。アクセスごとにハンドルを解決する
 * (対象が破棄されていれば nullptr)。GC の所有グラフ無しで dangling を防ぐ。
 *
 * @tparam T 参照先の型。
 */
template<class T>
struct TPoolRef {
    TObjectPool<T>* pool   = nullptr;
    FObjectHandle   handle {};

    TPoolRef() noexcept = default;
    TPoolRef(TObjectPool<T>& p, FObjectHandle h) noexcept : pool(&p), handle(h) {}

    /** 生きていれば対象ポインタ、死んでいれば nullptr。 */
    T*   Get()        const noexcept { return pool ? pool->Get(handle) : nullptr; }
    T*   operator->() const noexcept { return Get(); }
    T&   operator*()  const noexcept { return *Get(); }
    bool IsValid()    const noexcept { return Get() != nullptr; }
    explicit operator bool() const noexcept { return IsValid(); }
};

} // namespace acs

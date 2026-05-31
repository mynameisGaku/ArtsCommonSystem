// SPDX-License-Identifier: Apache-2.0
// Robin Hood ハッシュマップ（密値配列 + 8 バイトインデックスバケット）
#pragma once

#include "foundation/Types.h"
#include "foundation/Move.h"
#include "foundation/TypeTraits.h"
#include "foundation/Assert.h"
#include "container/Array.h"
#include "container/Hash.h"

namespace acs {

// キー / 値ペア
template<typename K, typename V>
struct Pair {
    K first;
    V second;
};

template<typename K, typename V, typename H = THasher<K>>
class THashMap {
public:
    using EntryType = Pair<K, V>;

    THashMap() noexcept : m_Values(DefaultAllocator()), m_Alloc(&DefaultAllocator()) {}
    explicit THashMap(FAllocator& a) noexcept : m_Values(a), m_Alloc(&a) {}

    THashMap(const THashMap&) = delete;
    THashMap& operator=(const THashMap&) = delete;

    THashMap(THashMap&& o) noexcept
        : m_Values(Move(o.m_Values)), m_Buckets(o.m_Buckets),
          m_BucketCount(o.m_BucketCount), m_BucketMask(o.m_BucketMask),
          m_Alloc(o.m_Alloc) {
        o.m_Buckets = nullptr;
        o.m_BucketCount = 0;
        o.m_BucketMask = 0;
    }
    THashMap& operator=(THashMap&& o) noexcept {
        if (this == &o) return *this;
        if (m_Buckets) m_Alloc->Free(m_Buckets);
        m_Values = Move(o.m_Values);
        m_Buckets = o.m_Buckets;
        m_BucketCount = o.m_BucketCount;
        m_BucketMask  = o.m_BucketMask;
        m_Alloc = o.m_Alloc;
        o.m_Buckets = nullptr;
        o.m_BucketCount = 0;
        o.m_BucketMask = 0;
        return *this;
    }
    ~THashMap() noexcept { if (m_Buckets) m_Alloc->Free(m_Buckets); }

    usize Size() const noexcept     { return m_Values.Size(); }
    bool  IsEmpty() const noexcept  { return m_Values.IsEmpty(); }

    // 挿入または上書き
    void Insert(const K& key, V value) noexcept {
        // load factor 超過なら容量倍増
        if ((Size() + 1) * 100 > m_BucketCount * kLoadFactorPct) Rehash(NextCapacity());
        InsertImpl(key, Move(value));
    }

    // 検索（見つかれば値ポインタ、なければ nullptr）
    V* Find(const K& key) noexcept {
        if (m_BucketCount == 0) return nullptr;
        u64 h = H{}(key);
        u32 ideal = static_cast<u32>(h) & m_BucketMask;
        u32 fp = static_cast<u32>((h >> 56) | 0x01);  // fingerprint（0 を避ける）
        u32 dist = 0;
        while (true) {
            const Bucket& b = m_Buckets[ideal];
            if (b.dist_fp == 0) return nullptr;            // 空スロット → 未存在
            if (b.Distance() < dist) return nullptr;       // Robin Hood: 自分より距離小 → 未存在
            if (b.Fingerprint() == fp) {
                // fingerprint 一致なら実キー比較
                if (m_Values[b.value_idx].first == key) return &m_Values[b.value_idx].second;
            }
            ideal = (ideal + 1) & m_BucketMask;
            ++dist;
        }
    }

    const V* Find(const K& key) const noexcept {
        return const_cast<THashMap*>(this)->Find(key);
    }

    bool Contains(const K& key) const noexcept { return Find(key) != nullptr; }

    // 削除（後方シフトで埋めて tombstone を残さない）
    bool Remove(const K& key) noexcept {
        if (m_BucketCount == 0) return false;
        u64 h = H{}(key);
        u32 ideal = static_cast<u32>(h) & m_BucketMask;
        u32 fp = static_cast<u32>((h >> 56) | 0x01);
        u32 dist = 0;
        while (true) {
            Bucket& b = m_Buckets[ideal];
            if (b.dist_fp == 0) return false;
            if (b.Distance() < dist) return false;
            if (b.Fingerprint() == fp && m_Values[b.value_idx].first == key) {
                u32 vidx = b.value_idx;
                m_Values.RemoveAtSwap(vidx);  // 末尾と入れ替えて削除
                // 末尾要素が動いたなら、それを指していたバケットの value_idx を更新
                if (vidx != m_Values.Size()) {
                    u64 mh = H{}(m_Values[vidx].first);
                    u32 m_ideal = static_cast<u32>(mh) & m_BucketMask;
                    u32 m_fp = static_cast<u32>((mh >> 56) | 0x01);
                    u32 m_dist = 0;
                    while (true) {
                        Bucket& mb = m_Buckets[m_ideal];
                        if (mb.dist_fp != 0 && mb.Fingerprint() == m_fp && mb.value_idx == m_Values.Size()) {
                            mb.value_idx = vidx;
                            break;
                        }
                        m_ideal = (m_ideal + 1) & m_BucketMask;
                        ++m_dist;
                        if (m_dist > m_BucketCount) break;
                    }
                }
                // 後方シフト: 削除位置に後続を 1 つずつ詰める
                u32 next_idx = (ideal + 1) & m_BucketMask;
                while (true) {
                    Bucket& nx = m_Buckets[next_idx];
                    if (nx.dist_fp == 0 || nx.Distance() == 0) {
                        m_Buckets[ideal].dist_fp = 0;
                        m_Buckets[ideal].value_idx = 0;
                        return true;
                    }
                    m_Buckets[ideal] = nx;
                    m_Buckets[ideal].SetDistance(nx.Distance() - 1);
                    ideal = next_idx;
                    next_idx = (next_idx + 1) & m_BucketMask;
                }
            }
            ideal = (ideal + 1) & m_BucketMask;
            ++dist;
        }
    }

    void Clear() noexcept {
        m_Values.Clear();
        if (m_Buckets) MemSet(m_Buckets, 0, sizeof(Bucket) * m_BucketCount);
    }

    // 容量予約（事前に呼んでおくと挿入中の rehash を防げる）
    void Reserve(usize n) noexcept {
        usize need = (n * 100 + kLoadFactorPct - 1) / kLoadFactorPct;
        usize cap = 16;
        while (cap < need) cap <<= 1;
        if (cap > m_BucketCount) Rehash(cap);
    }

    // range-for 用（密配列の begin/end を直接公開）
    EntryType*       begin()       noexcept { return m_Values.begin(); }
    EntryType*       end()         noexcept { return m_Values.end();   }
    const EntryType* begin() const noexcept { return m_Values.begin(); }
    const EntryType* end()   const noexcept { return m_Values.end();   }

private:
    static constexpr u32 kLoadFactorPct = 80;

    // バケット (8 バイト): dist_fp 上位 24bit=距離 / 下位 8bit=fingerprint, value_idx=値 idx
    struct Bucket {
        u32 dist_fp;
        u32 value_idx;

        u32 Distance()    const noexcept { return dist_fp >> 8; }
        u32 Fingerprint() const noexcept { return dist_fp & 0xFFu; }
        void Set(u32 d, u32 fp) noexcept { dist_fp = (d << 8) | (fp & 0xFFu); }
        void SetDistance(u32 d) noexcept { dist_fp = (d << 8) | (dist_fp & 0xFFu); }
    };

    usize NextCapacity() noexcept {
        return m_BucketCount == 0 ? 16 : m_BucketCount * 2;
    }

    // 全バケットを破棄して new_count 分を再構築（値配列は維持）
    void Rehash(usize new_count) noexcept {
        ACS_ASSERT((new_count & (new_count - 1)) == 0);
        Bucket* old_buckets = m_Buckets;

        void* mem = m_Alloc->Alloc(sizeof(Bucket) * new_count, alignof(Bucket), FSourceLoc::Current());
        ACS_ASSERTF(mem, "THashMap::Rehash: alloc failed (cap=%zu)", new_count);
        m_Buckets = static_cast<Bucket*>(mem);
        m_BucketCount = new_count;
        m_BucketMask  = static_cast<u32>(new_count - 1);
        MemSet(m_Buckets, 0, sizeof(Bucket) * new_count);

        // 全 value を順に再挿入
        for (u32 vi = 0; vi < m_Values.Size(); ++vi) {
            ReinsertBucket(vi);
        }

        if (old_buckets) m_Alloc->Free(old_buckets);
    }

    // value_idx vidx を Robin Hood 挿入（より遠いブロックに出会ったら入れ替え）
    void ReinsertBucket(u32 vidx) noexcept {
        u64 h = H{}(m_Values[vidx].first);
        u32 ideal = static_cast<u32>(h) & m_BucketMask;
        u32 fp = static_cast<u32>((h >> 56) | 0x01);
        Bucket nb;
        nb.Set(0, fp);
        nb.value_idx = vidx;
        u32 dist = 0;
        while (true) {
            Bucket& slot = m_Buckets[ideal];
            if (slot.dist_fp == 0) {
                slot = nb;
                slot.SetDistance(dist);
                return;
            }
            // 自分より距離が小さいスロットを見つけたら入れ替え
            if (slot.Distance() < dist) {
                Bucket tmp = slot;
                slot = nb;
                slot.SetDistance(dist);
                nb = tmp;
                dist = nb.Distance();
            }
            ideal = (ideal + 1) & m_BucketMask;
            ++dist;
        }
    }

    void InsertImpl(const K& key, V&& value) noexcept {
        u64 h = H{}(key);
        u32 ideal = static_cast<u32>(h) & m_BucketMask;
        u32 fp = static_cast<u32>((h >> 56) | 0x01);

        // 既存キーチェック
        u32 probe = ideal;
        u32 dist = 0;
        while (true) {
            const Bucket& b = m_Buckets[probe];
            if (b.dist_fp == 0) break;
            if (b.Distance() < dist) break;
            if (b.Fingerprint() == fp && m_Values[b.value_idx].first == key) {
                m_Values[b.value_idx].second = Move(value);  // 上書き
                return;
            }
            probe = (probe + 1) & m_BucketMask;
            ++dist;
        }

        // 新規エントリ: 値配列末尾に追加 → Robin Hood 挿入
        ACS_ASSERT(m_Values.Size() < 0xFFFFFFFFull);  // u32 への切り詰めによる無言の index 破壊を捕捉
        u32 new_idx = static_cast<u32>(m_Values.Size());
        m_Values.PushBack(EntryType{ key, Move(value) });
        Bucket nb;
        nb.Set(0, fp);
        nb.value_idx = new_idx;
        u32 d = 0;
        u32 i = ideal;
        while (true) {
            Bucket& slot = m_Buckets[i];
            if (slot.dist_fp == 0) {
                slot = nb;
                slot.SetDistance(d);
                return;
            }
            if (slot.Distance() < d) {
                Bucket tmp = slot;
                slot = nb;
                slot.SetDistance(d);
                nb = tmp;
                d = nb.Distance();
            }
            i = (i + 1) & m_BucketMask;
            ++d;
        }
    }

    TArray<EntryType> m_Values;            // 密値配列（イテレーションが速い）
    Bucket*          m_Buckets      = nullptr;
    usize            m_BucketCount = 0;
    u32              m_BucketMask  = 0;  // bucket_count - 1
    FAllocator*       m_Alloc        = nullptr;
};

} // namespace acs

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

template<typename K, typename V, typename H = Hasher<K>>
class THashMap {
public:
    using EntryType = Pair<K, V>;

    THashMap() noexcept : _values(DefaultAllocator()), _alloc(&DefaultAllocator()) {}
    explicit THashMap(Allocator& a) noexcept : _values(a), _alloc(&a) {}

    THashMap(const THashMap&) = delete;
    THashMap& operator=(const THashMap&) = delete;

    THashMap(THashMap&& o) noexcept
        : _values(Move(o._values)), _buckets(o._buckets),
          _bucket_count(o._bucket_count), _bucket_mask(o._bucket_mask),
          _alloc(o._alloc) {
        o._buckets = nullptr;
        o._bucket_count = 0;
        o._bucket_mask = 0;
    }
    THashMap& operator=(THashMap&& o) noexcept {
        if (this == &o) return *this;
        if (_buckets) _alloc->Free(_buckets);
        _values = Move(o._values);
        _buckets = o._buckets;
        _bucket_count = o._bucket_count;
        _bucket_mask  = o._bucket_mask;
        _alloc = o._alloc;
        o._buckets = nullptr;
        o._bucket_count = 0;
        o._bucket_mask = 0;
        return *this;
    }
    ~THashMap() noexcept { if (_buckets) _alloc->Free(_buckets); }

    usize Size() const noexcept     { return _values.Size(); }
    bool  IsEmpty() const noexcept  { return _values.IsEmpty(); }

    // 挿入または上書き
    void Insert(const K& key, V value) noexcept {
        // load factor 超過なら容量倍増
        if ((Size() + 1) * 100 > _bucket_count * kLoadFactorPct) Rehash(NextCapacity());
        InsertImpl(key, Move(value));
    }

    // 検索（見つかれば値ポインタ、なければ nullptr）
    V* Find(const K& key) noexcept {
        if (_bucket_count == 0) return nullptr;
        u64 h = H{}(key);
        u32 ideal = static_cast<u32>(h) & _bucket_mask;
        u32 fp = static_cast<u32>((h >> 56) | 0x01);  // fingerprint（0 を避ける）
        u32 dist = 0;
        while (true) {
            const Bucket& b = _buckets[ideal];
            if (b.dist_fp == 0) return nullptr;            // 空スロット → 未存在
            if (b.Distance() < dist) return nullptr;       // Robin Hood: 自分より距離小 → 未存在
            if (b.Fingerprint() == fp) {
                // fingerprint 一致なら実キー比較
                if (_values[b.value_idx].first == key) return &_values[b.value_idx].second;
            }
            ideal = (ideal + 1) & _bucket_mask;
            ++dist;
        }
    }

    const V* Find(const K& key) const noexcept {
        return const_cast<THashMap*>(this)->Find(key);
    }

    bool Contains(const K& key) const noexcept { return Find(key) != nullptr; }

    // 削除（後方シフトで埋めて tombstone を残さない）
    bool Remove(const K& key) noexcept {
        if (_bucket_count == 0) return false;
        u64 h = H{}(key);
        u32 ideal = static_cast<u32>(h) & _bucket_mask;
        u32 fp = static_cast<u32>((h >> 56) | 0x01);
        u32 dist = 0;
        while (true) {
            Bucket& b = _buckets[ideal];
            if (b.dist_fp == 0) return false;
            if (b.Distance() < dist) return false;
            if (b.Fingerprint() == fp && _values[b.value_idx].first == key) {
                u32 vidx = b.value_idx;
                _values.RemoveAtSwap(vidx);  // 末尾と入れ替えて削除
                // 末尾要素が動いたなら、それを指していたバケットの value_idx を更新
                if (vidx != _values.Size()) {
                    u64 mh = H{}(_values[vidx].first);
                    u32 m_ideal = static_cast<u32>(mh) & _bucket_mask;
                    u32 m_fp = static_cast<u32>((mh >> 56) | 0x01);
                    u32 m_dist = 0;
                    while (true) {
                        Bucket& mb = _buckets[m_ideal];
                        if (mb.dist_fp != 0 && mb.Fingerprint() == m_fp && mb.value_idx == _values.Size()) {
                            mb.value_idx = vidx;
                            break;
                        }
                        m_ideal = (m_ideal + 1) & _bucket_mask;
                        ++m_dist;
                        if (m_dist > _bucket_count) break;
                    }
                }
                // 後方シフト: 削除位置に後続を 1 つずつ詰める
                u32 next_idx = (ideal + 1) & _bucket_mask;
                while (true) {
                    Bucket& nx = _buckets[next_idx];
                    if (nx.dist_fp == 0 || nx.Distance() == 0) {
                        _buckets[ideal].dist_fp = 0;
                        _buckets[ideal].value_idx = 0;
                        return true;
                    }
                    _buckets[ideal] = nx;
                    _buckets[ideal].SetDistance(nx.Distance() - 1);
                    ideal = next_idx;
                    next_idx = (next_idx + 1) & _bucket_mask;
                }
            }
            ideal = (ideal + 1) & _bucket_mask;
            ++dist;
        }
    }

    void Clear() noexcept {
        _values.Clear();
        if (_buckets) MemSet(_buckets, 0, sizeof(Bucket) * _bucket_count);
    }

    // 容量予約（事前に呼んでおくと挿入中の rehash を防げる）
    void Reserve(usize n) noexcept {
        usize need = (n * 100 + kLoadFactorPct - 1) / kLoadFactorPct;
        usize cap = 16;
        while (cap < need) cap <<= 1;
        if (cap > _bucket_count) Rehash(cap);
    }

    // range-for 用（密配列の begin/end を直接公開）
    EntryType*       begin()       noexcept { return _values.begin(); }
    EntryType*       end()         noexcept { return _values.end();   }
    const EntryType* begin() const noexcept { return _values.begin(); }
    const EntryType* end()   const noexcept { return _values.end();   }

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
        return _bucket_count == 0 ? 16 : _bucket_count * 2;
    }

    // 全バケットを破棄して new_count 分を再構築（値配列は維持）
    void Rehash(usize new_count) noexcept {
        ACS_ASSERT((new_count & (new_count - 1)) == 0);
        Bucket* old_buckets = _buckets;

        void* mem = _alloc->Alloc(sizeof(Bucket) * new_count, alignof(Bucket), SourceLoc::Current());
        ACS_ASSERTF(mem, "THashMap::Rehash: alloc failed (cap=%zu)", new_count);
        _buckets = static_cast<Bucket*>(mem);
        _bucket_count = new_count;
        _bucket_mask  = static_cast<u32>(new_count - 1);
        MemSet(_buckets, 0, sizeof(Bucket) * new_count);

        // 全 value を順に再挿入
        for (u32 vi = 0; vi < _values.Size(); ++vi) {
            ReinsertBucket(vi);
        }

        if (old_buckets) _alloc->Free(old_buckets);
    }

    // value_idx vidx を Robin Hood 挿入（より遠いブロックに出会ったら入れ替え）
    void ReinsertBucket(u32 vidx) noexcept {
        u64 h = H{}(_values[vidx].first);
        u32 ideal = static_cast<u32>(h) & _bucket_mask;
        u32 fp = static_cast<u32>((h >> 56) | 0x01);
        Bucket nb;
        nb.Set(0, fp);
        nb.value_idx = vidx;
        u32 dist = 0;
        while (true) {
            Bucket& slot = _buckets[ideal];
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
            ideal = (ideal + 1) & _bucket_mask;
            ++dist;
        }
    }

    void InsertImpl(const K& key, V&& value) noexcept {
        u64 h = H{}(key);
        u32 ideal = static_cast<u32>(h) & _bucket_mask;
        u32 fp = static_cast<u32>((h >> 56) | 0x01);

        // 既存キーチェック
        u32 probe = ideal;
        u32 dist = 0;
        while (true) {
            const Bucket& b = _buckets[probe];
            if (b.dist_fp == 0) break;
            if (b.Distance() < dist) break;
            if (b.Fingerprint() == fp && _values[b.value_idx].first == key) {
                _values[b.value_idx].second = Move(value);  // 上書き
                return;
            }
            probe = (probe + 1) & _bucket_mask;
            ++dist;
        }

        // 新規エントリ: 値配列末尾に追加 → Robin Hood 挿入
        u32 new_idx = static_cast<u32>(_values.Size());
        _values.PushBack(EntryType{ key, Move(value) });
        Bucket nb;
        nb.Set(0, fp);
        nb.value_idx = new_idx;
        u32 d = 0;
        u32 i = ideal;
        while (true) {
            Bucket& slot = _buckets[i];
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
            i = (i + 1) & _bucket_mask;
            ++d;
        }
    }

    TArray<EntryType> _values;            // 密値配列（イテレーションが速い）
    Bucket*          _buckets      = nullptr;
    usize            _bucket_count = 0;
    u32              _bucket_mask  = 0;  // bucket_count - 1
    Allocator*       _alloc        = nullptr;
};

} // namespace acs

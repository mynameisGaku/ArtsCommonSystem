// ACS Container — Robin Hood hashmap (ankerl::unordered_dense layout).
//
// Layout:
//   * `_buckets` — power-of-two-sized index table; each bucket is 8 bytes:
//     {dist:24, fingerprint:8, value_idx:32}.
//   * `_values`  — dense Array<Pair<K,V>> for cache-friendly iteration.
//
// Lookup: hash → bucket → fingerprint check → key compare. Robin Hood probing
// keeps probe distances bounded; backward-shift deletion avoids tombstones.
//
// Load factor: 0.8. Capacity always a power of two.
//
// NOT thread-safe.
#pragma once

#include "foundation/Types.h"
#include "foundation/Move.h"
#include "foundation/TypeTraits.h"
#include "foundation/Assert.h"
#include "container/Array.h"
#include "container/Hash.h"

namespace acs {

template<typename K, typename V>
struct Pair {
    K first;
    V second;
};

template<typename K, typename V, typename H = Hasher<K>>
class HashMap {
public:
    using EntryType = Pair<K, V>;

    HashMap() noexcept : _values(DefaultAllocator()), _alloc(&DefaultAllocator()) {}
    explicit HashMap(Allocator& a) noexcept : _values(a), _alloc(&a) {}

    HashMap(const HashMap&) = delete;
    HashMap& operator=(const HashMap&) = delete;

    HashMap(HashMap&& o) noexcept
        : _values(Move(o._values)), _buckets(o._buckets),
          _bucket_count(o._bucket_count), _bucket_mask(o._bucket_mask),
          _alloc(o._alloc) {
        o._buckets = nullptr;
        o._bucket_count = 0;
        o._bucket_mask = 0;
    }
    HashMap& operator=(HashMap&& o) noexcept {
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
    ~HashMap() noexcept { if (_buckets) _alloc->Free(_buckets); }

    usize Size() const noexcept     { return _values.Size(); }
    bool  IsEmpty() const noexcept  { return _values.IsEmpty(); }

    // Insert or assign.
    void Insert(const K& key, V value) noexcept {
        if ((Size() + 1) * 100 > _bucket_count * kLoadFactorPct) Rehash(NextCapacity());
        InsertImpl(key, Move(value));
    }

    V* Find(const K& key) noexcept {
        if (_bucket_count == 0) return nullptr;
        u64 h = H{}(key);
        u32 ideal = static_cast<u32>(h) & _bucket_mask;
        u32 fp = static_cast<u32>((h >> 56) | 0x01);
        u32 dist = 0;
        while (true) {
            const Bucket& b = _buckets[ideal];
            if (b.dist_fp == 0) return nullptr;
            if (b.Distance() < dist) return nullptr;
            if (b.Fingerprint() == fp) {
                if (_values[b.value_idx].first == key) return &_values[b.value_idx].second;
            }
            ideal = (ideal + 1) & _bucket_mask;
            ++dist;
        }
    }

    const V* Find(const K& key) const noexcept {
        return const_cast<HashMap*>(this)->Find(key);
    }

    bool Contains(const K& key) const noexcept { return Find(key) != nullptr; }

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
                _values.RemoveAtSwap(vidx);
                // If we swapped, update the bucket pointing at the moved value.
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
                // Backward-shift to fill the slot.
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

    void Reserve(usize n) noexcept {
        usize need = (n * 100 + kLoadFactorPct - 1) / kLoadFactorPct;
        usize cap = 16;
        while (cap < need) cap <<= 1;
        if (cap > _bucket_count) Rehash(cap);
    }

    EntryType*       begin()       noexcept { return _values.begin(); }
    EntryType*       end()         noexcept { return _values.end();   }
    const EntryType* begin() const noexcept { return _values.begin(); }
    const EntryType* end()   const noexcept { return _values.end();   }

private:
    static constexpr u32 kLoadFactorPct = 80;

    struct Bucket {
        u32 dist_fp;   // upper 24 bits: distance, lower 8 bits: fingerprint
        u32 value_idx;

        u32 Distance()    const noexcept { return dist_fp >> 8; }
        u32 Fingerprint() const noexcept { return dist_fp & 0xFFu; }
        void Set(u32 d, u32 fp) noexcept { dist_fp = (d << 8) | (fp & 0xFFu); }
        void SetDistance(u32 d) noexcept { dist_fp = (d << 8) | (dist_fp & 0xFFu); }
    };

    usize NextCapacity() noexcept {
        return _bucket_count == 0 ? 16 : _bucket_count * 2;
    }

    void Rehash(usize new_count) noexcept {
        ACS_ASSERT((new_count & (new_count - 1)) == 0);
        Bucket* old_buckets = _buckets;
        usize   old_count   = _bucket_count;

        void* mem = _alloc->Alloc(sizeof(Bucket) * new_count, alignof(Bucket), SourceLoc::Current());
        ACS_ASSERTF(mem, "HashMap::Rehash: alloc failed (cap=%zu)", new_count);
        _buckets = static_cast<Bucket*>(mem);
        _bucket_count = new_count;
        _bucket_mask  = static_cast<u32>(new_count - 1);
        MemSet(_buckets, 0, sizeof(Bucket) * new_count);

        // Re-insert by walking dense values.
        for (u32 vi = 0; vi < _values.Size(); ++vi) {
            ReinsertBucket(vi);
        }

        if (old_buckets) _alloc->Free(old_buckets);
        (void)old_count;
    }

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

        // Check for existing key.
        u32 probe = ideal;
        u32 dist = 0;
        while (true) {
            const Bucket& b = _buckets[probe];
            if (b.dist_fp == 0) break;
            if (b.Distance() < dist) break;
            if (b.Fingerprint() == fp && _values[b.value_idx].first == key) {
                _values[b.value_idx].second = Move(value);
                return;
            }
            probe = (probe + 1) & _bucket_mask;
            ++dist;
        }

        // New entry: append to dense storage, robin-hood insert into buckets.
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

    Array<EntryType> _values;
    Bucket*          _buckets      = nullptr;
    usize            _bucket_count = 0;
    u32              _bucket_mask  = 0;
    Allocator*       _alloc        = nullptr;
};

} // namespace acs

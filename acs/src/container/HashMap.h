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

/**
 * キー / 値ペア (THashMap の格納エントリ)。
 *
 * @tparam K キー型。
 * @tparam V 値型。
 */
template<typename K, typename V>
struct Pair {
    /** キー。 */
    K first;

    /** 値。 */
    V second;
};

/**
 * Robin Hood 法のハッシュマップ。
 *
 * @details
 * 値は密配列 (m_Values) に連続格納してイテレーションを速くし、別途 8 バイトの
 * インデックスバケット配列で位置を管理する。各バケットは距離 + fingerprint と
 * 値配列インデックスを持つ。挿入は Robin Hood (距離が小さいスロットを奪う) で
 * 偏りを抑え、削除は後方シフトで詰めて tombstone を残さない。コピー禁止のムーブ専用。
 * @tparam K キー型。
 * @tparam V 値型。
 * @tparam H キーのハッシュ functor 型 (既定は THasher<K>)。
 */
template<typename K, typename V, typename H = THasher<K>>
class THashMap {
public:
    /** 格納エントリの型 (Pair<K, V>)。 */
    using EntryType = Pair<K, V>;

    /** 既定アロケータで空のマップを構築する。 */
    THashMap() noexcept : m_Values(DefaultAllocator()), m_Alloc(&DefaultAllocator()) {}

    /**
     * 指定アロケータで空のマップを構築する。
     *
     * @param a 値配列・バケット確保に使うアロケータ。
     */
    explicit THashMap(FAllocator& a) noexcept : m_Values(a), m_Alloc(&a) {}

    /** コピー禁止 (ムーブ専用)。 */
    THashMap(const THashMap&) = delete;

    /** コピー代入も禁止。 */
    THashMap& operator=(const THashMap&) = delete;

    /**
     * ムーブ構築する (値配列とバケット所有権を奪う)。
     *
     * @param o ムーブ元 (バケットが空状態にリセットされる)。
     */
    THashMap(THashMap&& o) noexcept
        : m_Values(Move(o.m_Values)), m_Buckets(o.m_Buckets),
          m_BucketCount(o.m_BucketCount), m_BucketMask(o.m_BucketMask),
          m_Alloc(o.m_Alloc) {
        o.m_Buckets = nullptr;
        o.m_BucketCount = 0;
        o.m_BucketMask = 0;
    }

    /**
     * ムーブ代入する (既存バケットを解放してから所有権を奪う)。
     *
     * @param o ムーブ元 (バケットが空状態にリセットされる)。
     * @return *this。
     */
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

    /** バケット配列を解放する (値配列は m_Values のデストラクタが解放)。 */
    ~THashMap() noexcept { if (m_Buckets) m_Alloc->Free(m_Buckets); }

    /**
     * 要素数を返す。
     *
     * @return 格納エントリ数。
     */
    usize Size() const noexcept     { return m_Values.Size(); }

    /**
     * 空かどうかを返す。
     *
     * @return エントリが無ければ true。
     */
    bool  IsEmpty() const noexcept  { return m_Values.IsEmpty(); }

    /**
     * キーを挿入、または既存キーなら値を上書きする。
     *
     * @details load factor (80%) 超過なら容量を倍増して rehash してから挿入する。
     * @param key 挿入するキー。
     * @param value 挿入する値 (ムーブされる)。
     */
    void Insert(const K& key, V value) noexcept {
        // load factor 超過なら容量倍増
        if ((Size() + 1) * 100 > m_BucketCount * kLoadFactorPct) Rehash(NextCapacity());
        InsertImpl(key, Move(value));
    }

    /**
     * キーに対応する値ポインタを検索する。
     *
     * @details Robin Hood の距離不変条件で早期に未存在を判定する。
     * @param key 検索するキー。
     * @return 見つかれば値へのポインタ、無ければ nullptr。
     */
    V* Find(const K& key) noexcept {
        if (m_BucketCount == 0) return nullptr;
        const u64 h = H{}(key);
        u32 ideal = static_cast<u32>(h) & m_BucketMask;
        const u32 fp = static_cast<u32>((h >> 56) | 0x01);  // fingerprint（0 を避ける）
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

    /**
     * キーに対応する値を const で検索する。
     *
     * @param key 検索するキー。
     * @return 見つかれば値への const ポインタ、無ければ nullptr。
     */
    const V* Find(const K& key) const noexcept {
        return const_cast<THashMap*>(this)->Find(key);
    }

    /**
     * キーが存在するかを返す。
     *
     * @param key 判定するキー。
     * @return 存在すれば true。
     */
    bool Contains(const K& key) const noexcept { return Find(key) != nullptr; }

    /**
     * キーを削除する (後方シフトで詰めて tombstone を残さない)。
     *
     * @details 値配列は末尾入れ替え削除し、動いた末尾要素を指すバケットの index も
     * 更新する。バケット列は削除位置以降を 1 つずつ前方へシフトする。
     * @param key 削除するキー。
     * @return 削除したら true、見つからなければ false。
     */
    bool Remove(const K& key) noexcept {
        if (m_BucketCount == 0) return false;
        const u64 h = H{}(key);
        u32 ideal = static_cast<u32>(h) & m_BucketMask;
        const u32 fp = static_cast<u32>((h >> 56) | 0x01);
        u32 dist = 0;
        while (true) {
            Bucket& b = m_Buckets[ideal];
            if (b.dist_fp == 0) return false;
            if (b.Distance() < dist) return false;
            if (b.Fingerprint() == fp && m_Values[b.value_idx].first == key) {
                const u32 vidx = b.value_idx;
                m_Values.RemoveAtSwap(vidx);  // 末尾と入れ替えて削除
                // 末尾要素が動いたなら、それを指していたバケットの value_idx を更新
                if (vidx != m_Values.Size()) {
                    const u64 mh = H{}(m_Values[vidx].first);
                    u32 cur_idx = static_cast<u32>(mh) & m_BucketMask;
                    const u32 cur_fp = static_cast<u32>((mh >> 56) | 0x01);
                    u32 cur_dist = 0;
                    while (true) {
                        Bucket& mb = m_Buckets[cur_idx];
                        if (mb.dist_fp != 0 && mb.Fingerprint() == cur_fp && mb.value_idx == m_Values.Size()) {
                            mb.value_idx = vidx;
                            break;
                        }
                        cur_idx = (cur_idx + 1) & m_BucketMask;
                        ++cur_dist;
                        if (cur_dist > m_BucketCount) break;
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

    /** 全エントリを削除する (値配列をクリアし、バケットを 0 埋め。容量は保持)。 */
    void Clear() noexcept {
        m_Values.Clear();
        if (m_Buckets) MemSet(m_Buckets, 0, sizeof(Bucket) * m_BucketCount);
    }

    /**
     * 容量を予約する (事前に呼ぶと挿入中の rehash を防げる)。
     *
     * @details load factor を考慮し、n 要素を収められる 2 の冪のバケット数を確保する。
     * @param n 収めたいエントリ数。
     */
    void Reserve(usize n) noexcept {
        const usize need = (n * 100 + kLoadFactorPct - 1) / kLoadFactorPct;
        usize cap = 16;
        while (cap < need) cap <<= 1;
        if (cap > m_BucketCount) Rehash(cap);
    }

    /**
     * range-for 用の先頭イテレータを返す (密値配列の begin)。
     *
     * @return 先頭エントリへのポインタ。
     */
    EntryType*       begin()       noexcept { return m_Values.begin(); }

    /**
     * range-for 用の終端イテレータを返す (密値配列の end)。
     *
     * @return 末尾の次を指すポインタ。
     */
    EntryType*       end()         noexcept { return m_Values.end();   }

    /**
     * range-for 用の先頭 const イテレータを返す。
     *
     * @return 先頭エントリへの const ポインタ。
     */
    const EntryType* begin() const noexcept { return m_Values.begin(); }

    /**
     * range-for 用の終端 const イテレータを返す。
     *
     * @return 末尾の次を指す const ポインタ。
     */
    const EntryType* end()   const noexcept { return m_Values.end();   }

private:
    /** load factor の上限 (パーセント)。これを超えると rehash する。 */
    static constexpr u32 kLoadFactorPct = 80;

    /**
     * インデックスバケット (8 バイト: 距離 + fingerprint + 値配列 index)。
     *
     * @details dist_fp の上位 24bit が探索距離、下位 8bit が fingerprint。dist_fp==0 は
     * 空スロットを表す。value_idx は m_Values 内の格納位置。
     */
    struct Bucket {
        /** 上位 24bit=距離 / 下位 8bit=fingerprint (0 は空スロット)。 */
        u32 dist_fp;

        /** 値配列 m_Values 内のインデックス。 */
        u32 value_idx;

        /**
         * 探索距離 (ideal からのずれ) を返す。
         *
         * @return dist_fp の上位 24bit。
         */
        u32 Distance()    const noexcept { return dist_fp >> 8; }

        /**
         * fingerprint を返す。
         *
         * @return dist_fp の下位 8bit。
         */
        u32 Fingerprint() const noexcept { return dist_fp & 0xFFu; }

        /**
         * 距離と fingerprint をまとめて設定する。
         *
         * @param d 探索距離。
         * @param fp fingerprint (下位 8bit のみ使用)。
         */
        void Set(u32 d, u32 fp) noexcept { dist_fp = (d << 8) | (fp & 0xFFu); }

        /**
         * fingerprint を保ったまま距離だけ書き換える。
         *
         * @param d 新しい探索距離。
         */
        void SetDistance(u32 d) noexcept { dist_fp = (d << 8) | (dist_fp & 0xFFu); }
    };

    /**
     * rehash 時の次のバケット容量を返す (空なら 16、以降は倍増)。
     *
     * @return 次のバケット数。
     */
    usize NextCapacity() noexcept {
        return m_BucketCount == 0 ? 16 : m_BucketCount * 2;
    }

    /**
     * バケット配列を new_count で作り直し、全 value を再挿入する (値配列は維持)。
     *
     * @details new_count は 2 の冪である必要がある (ACS_ASSERT で検査)。
     * @param new_count 新しいバケット数 (2 の冪)。
     */
    void Rehash(usize new_count) noexcept {
        ACS_ASSERT((new_count & (new_count - 1)) == 0);
        Bucket* const old_buckets = m_Buckets;

        void* const mem = m_Alloc->Alloc(sizeof(Bucket) * new_count, alignof(Bucket), FSourceLoc::Current());
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

    /**
     * 値配列 index を Robin Hood 法でバケットへ挿入する (rehash 専用)。
     *
     * @details 既存キー比較は不要 (rehash 中は重複しない前提)。距離が小さいスロットに
     * 出会ったら入れ替えて再配置する。
     * @param vidx 挿入する値配列インデックス。
     */
    void ReinsertBucket(u32 vidx) noexcept {
        const u64 h = H{}(m_Values[vidx].first);
        u32 ideal = static_cast<u32>(h) & m_BucketMask;
        const u32 fp = static_cast<u32>((h >> 56) | 0x01);
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

    /**
     * 挿入/上書きの本体 (rehash 判定後に呼ばれる)。
     *
     * @details 既存キーがあれば値を上書きして戻る。新規なら値配列末尾へ追加してから
     * Robin Hood 挿入する。u32 への index 切り詰めは ACS_ASSERT で検出する。
     * @param key 挿入するキー。
     * @param value 挿入する値 (ムーブされる)。
     */
    void InsertImpl(const K& key, V&& value) noexcept {
        const u64 h = H{}(key);
        const u32 ideal = static_cast<u32>(h) & m_BucketMask;
        const u32 fp = static_cast<u32>((h >> 56) | 0x01);

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
        const u32 new_idx = static_cast<u32>(m_Values.Size());
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

    /** 密値配列 (連続格納でイテレーションが速い)。 */
    TArray<EntryType> m_Values;

    /** インデックスバケット配列 (m_BucketCount 個)。 */
    Bucket*          m_Buckets      = nullptr;

    /** バケット数 (2 の冪)。 */
    usize            m_BucketCount = 0;

    /** バケットマスク (bucket_count - 1、index 算出に使う)。 */
    u32              m_BucketMask  = 0;

    /** 確保に使うアロケータ。 */
    FAllocator*       m_Alloc        = nullptr;
};

} // namespace acs

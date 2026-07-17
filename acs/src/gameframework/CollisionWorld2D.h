// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar F — FCollisionWorld2D + SpatialGrid
//
// 2D 衝突判定の高レベル API。形状 (Aabb2 / Circle) を `FShapeId` で管理し、
// 内部の SpatialGrid (一様グリッド broad-phase) で O(N + K) クエリ
// (K = 結果数) を提供する。narrow phase は `math/Collision2D.h` の
// 既存関数 (Intersect / Resolve / RaycastAabb / RaycastCircle) を再利用。
//
// 使い方:
//   FCollisionWorld2D world;
//   world.Init(/*cell_size=*/64.0f);
//
//   FShapeId player = world.AddCircle({ {0,0}, 16.0f });
//   FShapeId wall   = world.AddAabb({ {100,0}, {16,32} });
//
//   // 毎フレーム移動した形状を Update
//   world.UpdateCircle(player, { player_pos, 16.0f });
//
//   // クエリ
//   TArray<FShapeId> hits;
//   world.OverlapCircle({ player_pos, 32.0f }, hits);  // 32 範囲のもの全部
//
//   RayHit2 rh;
//   FShapeId hit_id;
//   if (world.Raycast({ origin, dir }, /*max_t=*/100.0f, rh, hit_id)) {
//       // rh.point/normal/t、hit_id でどの形状か分かる
//   }
//
// 設計 (Pillar F):
//   ・**FShapeId**: 32bit packed = 24bit index + 8bit generation。removed slot
//     を再利用しても古い handle は無効化される (FNodeId と同パターン)。
//   ・**一様グリッド**: `cell_size` で固定、shape は overlapping cells に
//     全部登録 (= shape は複数 cell に存在し得る)。クエリ時は cell 走査して
//     候補集合を作り、narrow phase で実際の交差判定。
//   ・**dirty flag + lazy rebuild**: Add/Update/Remove で `m_Dirty = true`、
//     query 直前にグリッド再構築。per-frame full rebuild。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "math/Vec.h"
#include "math/Collision2D.h"

namespace acs::game {

/**
 * 衝突形状を一意に指すハンドル (24bit index + 8bit generation を packed)。
 *
 * @details
 * 0 = invalid。slot を再利用しても generation が進むため、削除済みスロットを指す
 * 古い handle は IsValid / Generation 照合で無効化される (FNodeId と同パターン)。
 */
struct FShapeId {
    /** packed 値 (0=invalid、low24=index, high8=generation)。 */
    u32 m_Packed = 0;

    /** invalid (m_Packed=0) な FShapeId を構築する。 */
    constexpr FShapeId() noexcept = default;

    /**
     * index と generation から packed handle を構築する。
     *
     * @param index slot インデックス (low 24bit)。
     * @param gen generation カウンタ (high 8bit)。
     */
    constexpr FShapeId(u32 index, u8 gen) noexcept
        : m_Packed((index & 0x00FFFFFFu) | (static_cast<u32>(gen) << 24)) {}

    /**
     * slot インデックスを取り出す。
     *
     * @return packed の low 24bit。
     */
    constexpr u32  Index() const noexcept { return m_Packed & 0x00FFFFFFu; }

    /**
     * generation カウンタを取り出す。
     *
     * @return packed の high 8bit。
     */
    constexpr u8   Generation() const noexcept {
        return static_cast<u8>(m_Packed >> 24);
    }

    /**
     * 有効な handle かを返す。
     *
     * @return m_Packed が 0 でなければ true。
     */
    constexpr bool IsValid() const noexcept { return m_Packed != 0; }

    /**
     * 等価比較。
     *
     * @param o 比較対象の handle。
     * @return packed 値が一致すれば true。
     */
    constexpr bool operator==(FShapeId o) const noexcept { return m_Packed == o.m_Packed; }

    /**
     * 非等価比較。
     *
     * @param o 比較対象の handle。
     * @return packed 値が異なれば true。
     */
    constexpr bool operator!=(FShapeId o) const noexcept { return m_Packed != o.m_Packed; }
};

/**
 * 一様グリッド broad-phase を持つ 2D 衝突判定の高レベル API。
 *
 * @details
 * 形状 (Aabb2 / Circle / ConvexPoly2 / Obb2) を FShapeId で管理し、内部の
 * SpatialGrid (cell_size 固定の一様グリッド) で O(N + K) クエリを提供する
 * (K = 結果数)。narrow phase は math/Collision2D.h の既存関数を再利用する。
 * Add/Update/Remove で dirty フラグを立て、クエリ直前にグリッドを遅延再構築する。
 */
class FCollisionWorld2D {
public:
    /** 空状態で構築する (cell_size は Init まで既定 64)。 */
    FCollisionWorld2D() noexcept = default;

    /** 破棄する (slot / grid は TArray が解放)。 */
    ~FCollisionWorld2D() noexcept = default;

    /** コピー禁止。 */
    FCollisionWorld2D(const FCollisionWorld2D&)            = delete;

    /** コピー代入も禁止。 */
    FCollisionWorld2D& operator=(const FCollisionWorld2D&) = delete;

    /**
     * グリッドのセルサイズを設定する。
     *
     * @details 0 以下を渡した場合 (および未呼出時) は 64.0f が既定。
     * @param cell_size SpatialGrid のセルサイズ (world unit、典型的に形状最大サイズの 2-3 倍)。
     */
    void Init(f32 cell_size = 64.0f) noexcept {
        m_CellSize = cell_size > 0.0f ? cell_size : 64.0f;
    }

    /** 全レイヤを表すマスク (= レイヤ無指定の既定値)。 */
    static constexpr u32 kAllLayers = 0xFFFFFFFFu;

    /**
     * AABB 形状を登録して handle を返す。
     *
     * @param a 登録する軸並行境界ボックス。
     * @param layer この shape が属するレイヤ bitmask (クエリ mask と AND して候補判定)。
     * @return 新しい FShapeId。
     */
    FShapeId AddAabb   (const Aabb2& a, u32 layer = kAllLayers) noexcept;

    /**
     * 円形状を登録して handle を返す。
     *
     * @param c 登録する円。
     * @param layer この shape が属するレイヤ bitmask。
     * @return 新しい FShapeId。
     */
    FShapeId AddCircle (const Circle& c, u32 layer = kAllLayers) noexcept;

    /**
     * 凸ポリゴン形状を登録して handle を返す。
     *
     * @param p 登録する凸ポリゴン。
     * @param layer この shape が属するレイヤ bitmask。
     * @return 新しい FShapeId。
     */
    FShapeId AddPolygon(const ConvexPoly2& p, u32 layer = kAllLayers) noexcept;

    /**
     * OBB (有向境界ボックス = 回転矩形) を登録して handle を返す。
     *
     * @details 内部は 4 頂点凸ポリとして SAT 判定する。
     * @param o 登録する有向境界ボックス。
     * @param layer この shape が属するレイヤ bitmask。
     * @return 新しい FShapeId。
     */
    FShapeId AddObb    (const Obb2& o, u32 layer = kAllLayers) noexcept;

    /**
     * AABB 形状を更新する (移動した時)。
     *
     * @details handle と種別が一致しなければ no-op。dirty にして次クエリで再構築する。
     * @param id 更新対象の handle。
     * @param a 新しい AABB。
     */
    void UpdateAabb   (FShapeId id, const Aabb2& a) noexcept;

    /**
     * 円形状を更新する (移動した時)。
     *
     * @details handle と種別が一致しなければ no-op。
     * @param id 更新対象の handle。
     * @param c 新しい円。
     */
    void UpdateCircle (FShapeId id, const Circle& c) noexcept;

    /**
     * 凸ポリゴン形状を更新する (移動した時)。
     *
     * @details handle と種別が一致しなければ no-op。
     * @param id 更新対象の handle。
     * @param p 新しい凸ポリゴン。
     */
    void UpdatePolygon(FShapeId id, const ConvexPoly2& p) noexcept;

    /**
     * OBB 形状を更新する (移動した時)。
     *
     * @details handle と種別が一致しなければ no-op。
     * @param id 更新対象の handle。
     * @param o 新しい有向境界ボックス。
     */
    void UpdateObb    (FShapeId id, const Obb2& o) noexcept;

    /**
     * shape を削除する。
     *
     * @details slot は再利用され、generation が進むので古い handle は無効化される。
     * @param id 削除する handle。
     */
    void Remove(FShapeId id) noexcept;

    /** 全 shape を破棄し grid もクリアする。 */
    void ClearAll() noexcept;

    /**
     * 登録中の shape 数を返す。
     *
     * @return アクティブな shape 数。
     */
    u32 ShapeCount() const noexcept { return m_ShapeCount; }

    /**
     * AABB と重なる shape を列挙する。
     *
     * @param a 問い合わせる AABB。
     * @param out 結果を格納する配列 (呼び出し時にクリアされる)。
     * @param exclude 除外したい shape (自己 overlap 抑止用、invalid なら除外無し)。
     * @param mask ヒット候補を絞るレイヤ bitmask (shape.layer & mask == 0 は無視)。
     */
    void OverlapAabb   (const Aabb2& a,   TArray<FShapeId>& out, FShapeId exclude = {}, u32 mask = kAllLayers) noexcept;

    /**
     * 円と重なる shape を列挙する。
     *
     * @param c 問い合わせる円。
     * @param out 結果を格納する配列 (呼び出し時にクリアされる)。
     * @param exclude 除外したい shape (invalid なら除外無し)。
     * @param mask ヒット候補を絞るレイヤ bitmask。
     */
    void OverlapCircle (const Circle& c,  TArray<FShapeId>& out, FShapeId exclude = {}, u32 mask = kAllLayers) noexcept;

    /**
     * 凸ポリゴンと重なる shape を列挙する。
     *
     * @param p 問い合わせる凸ポリゴン (頂点数 3 未満なら何もしない)。
     * @param out 結果を格納する配列 (呼び出し時にクリアされる)。
     * @param exclude 除外したい shape (invalid なら除外無し)。
     * @param mask ヒット候補を絞るレイヤ bitmask。
     */
    void OverlapPolygon(const ConvexPoly2& p, TArray<FShapeId>& out, FShapeId exclude = {}, u32 mask = kAllLayers) noexcept;

    /**
     * レイをキャストし最も近い shape を 1 つ返す。
     *
     * @param ray 発射するレイ。
     * @param max_t レイの最大距離。
     * @param out_hit ヒット時の交点・法線・t を書き込む先。
     * @param out_id ヒットした shape の handle を書き込む先。
     * @param mask ヒット候補を絞るレイヤ bitmask。
     * @return いずれかにヒットしたら true。
     */
    bool Raycast(const Ray2& ray, f32 max_t, RayHit2& out_hit, FShapeId& out_id, u32 mask = kAllLayers) noexcept;

    /**
     * 円を重なっている全 shape から押し出す合計ベクトルを返す (collide-and-slide 用)。
     *
     * @details
     * 重なり無しなら {0,0}。body が move 後にこれで貫通解消し、押し出し法線方向の
     * 速度成分を消すことでスライドする。
     * @param c 押し出し対象の円。
     * @param exclude 除外したい shape (自己 overlap 抑止用、invalid なら除外無し)。
     * @param mask 対象を絞るレイヤ bitmask。
     * @return 押し出しベクトルの合計。
     */
    FVec2 ResolveCircle (const Circle& c,      FShapeId exclude = {}, u32 mask = kAllLayers) noexcept;

    /**
     * 凸ポリゴンを重なっている全 shape から押し出す合計ベクトルを返す (collide-and-slide 用)。
     *
     * @details 重なり無しなら {0,0}。
     * @param p 押し出し対象の凸ポリゴン。
     * @param exclude 除外したい shape (invalid なら除外無し)。
     * @param mask 対象を絞るレイヤ bitmask。
     * @return 押し出しベクトルの合計。
     */
    FVec2 ResolvePolygon(const ConvexPoly2& p, FShapeId exclude = {}, u32 mask = kAllLayers) noexcept;

private:
    /** slot に格納された形状の種別タグ。 */
    enum class Kind : u8 {
        /** 空 slot (未使用)。 */
        None = 0,

        /** 軸並行境界ボックス。 */
        FAabb,

        /** 円。 */
        Circle,

        /** 凸ポリゴン。 */
        Poly,

        /** 有向境界ボックス (回転矩形)。 */
        Obb,
    };

    /** 1 形状分の格納スロット (全形状種を union 的に保持)。 */
    struct Slot {
        /** 格納されている形状の種別。 */
        Kind        kind   = Kind::None;

        /** スロットが使用中か。 */
        bool        active = false;

        /** generation カウンタ (handle 照合用)。 */
        u8          gen    = 0;

        /** 所属レイヤ bitmask。 */
        u32         layer  = kAllLayers;

        /** AABB データ (kind=FAabb のとき有効)。 */
        Aabb2       aabb   {};

        /** 円データ (kind=Circle のとき有効)。 */
        Circle      circle {};

        /** 凸ポリゴンデータ (kind=Poly のとき有効)。 */
        ConvexPoly2 poly   {};

        /** OBB データ (kind=Obb のとき有効)。 */
        Obb2        obb    {};
    };

    /** SpatialGrid の 1 セル (cell 座標と、そこに重なる slot index 群)。 */
    struct GridCell {
        /** セルの x 座標。 */
        i32 cx = 0;

        /** セルの y 座標。 */
        i32 cy = 0;

        /** このセルに重なる shape の slot index 群。 */
        TArray<u32> shapes;
    };

    /**
     * 空き slot を確保してインデックスを返す。
     *
     * @details index 0 は invalid 用に予約。空きが無ければ末尾に追加する。
     * @return 確保した slot のインデックス。
     */
    u32  AcquireSlot() noexcept;

    /** グリッドを dirty にマークし、次のクエリで再構築させる。 */
    void MarkDirty() noexcept { m_Dirty = true; }

    /** dirty なときだけグリッドを全再構築する。 */
    void RebuildGridIfDirty() noexcept;

    /**
     * AABB が重なるセル範囲を求める。
     *
     * @param a 対象の AABB。
     * @param cx_min セル x 最小を書き込む先。
     * @param cy_min セル y 最小を書き込む先。
     * @param cx_max セル x 最大を書き込む先。
     * @param cy_max セル y 最大を書き込む先。
     */
    void CellRange(const Aabb2& a, i32& cx_min, i32& cy_min,
                                    i32& cx_max, i32& cy_max) const noexcept;

    /**
     * 円が重なるセル範囲を求める (円を AABB で囲って委譲)。
     *
     * @param c 対象の円。
     * @param cx_min セル x 最小を書き込む先。
     * @param cy_min セル y 最小を書き込む先。
     * @param cx_max セル x 最大を書き込む先。
     * @param cy_max セル y 最大を書き込む先。
     */
    void CellRange(const Circle& c, i32& cx_min, i32& cy_min,
                                    i32& cx_max, i32& cy_max) const noexcept;

    /**
     * (cx, cy) のセルを取得し、無ければ新規作成して返す。
     *
     * @param cx セル x 座標。
     * @param cy セル y 座標。
     * @return 対象セルへの参照。
     */
    GridCell& GetOrCreateCell(i32 cx, i32 cy) noexcept;

    /**
     * (cx, cy) のセルを線形探索する。
     *
     * @param cx セル x 座標。
     * @param cy セル y 座標。
     * @return 見つかればセルへのポインタ、無ければ nullptr。
     */
    GridCell* FindCell(i32 cx, i32 cy) noexcept;

    /**
     * slot を重なる全セルに登録する。
     *
     * @param slot_idx 登録する slot のインデックス。
     */
    void InsertSlotIntoCells(u32 slot_idx) noexcept;

    /**
     * クエリ AABB のブロードフェーズ候補 slot を m_QueryScratch へ集める。
     *
     * @details
     * m_QueryMarks を初期化し、セル走査 + m_HugeShapes の合算を重複なしで積む。
     * クエリ範囲自体のセル数が上限を超える場合はセル走査を諦め、全アクティブ
     * slot の線形走査へフォールバックする (巨大クエリでもハングしない)。
     * @param box クエリ範囲を囲む AABB。
     */
    void CollectCandidates(const Aabb2& box) noexcept;

    /**
     * slot[idx] が AABB と交差するかを判定する (narrow phase)。
     *
     * @param slot_idx 判定する slot のインデックス。
     * @param a 相手の AABB。
     * @return 交差すれば true。
     */
    bool NarrowIntersectAabb  (u32 slot_idx, const Aabb2& a) const noexcept;

    /**
     * slot[idx] が円と交差するかを判定する (narrow phase)。
     *
     * @param slot_idx 判定する slot のインデックス。
     * @param c 相手の円。
     * @return 交差すれば true。
     */
    bool NarrowIntersectCircle(u32 slot_idx, const Circle& c) const noexcept;

    /**
     * slot[idx] が凸ポリゴンと交差するかを判定する (narrow phase)。
     *
     * @param slot_idx 判定する slot のインデックス。
     * @param p 相手の凸ポリゴン。
     * @return 交差すれば true。
     */
    bool NarrowIntersectPoly  (u32 slot_idx, const ConvexPoly2& p) const noexcept;

    /** 形状スロット配列 (index 0 は invalid 用に予約)。 */
    TArray<Slot>     m_Slots;

    /** アクティブな shape 数。 */
    u32             m_ShapeCount = 0;

    /** グリッドセル群 (cx/cy で線形検索)。 */
    TArray<GridCell> m_Cells;

    /** グリッドのセルサイズ (world unit)。 */
    f32             m_CellSize   = 64.0f;

    /** dirty フラグ (true なら次クエリで grid 再構築)。 */
    bool            m_Dirty       = false;

    /** クエリ中の重複除去マーク (slot 数長の bool array)。 */
    TArray<u8>       m_QueryMarks;

    /**
     * セル範囲が kMaxCellsPerShape を超えた巨大形状 (slot index) の退避先。
     *
     * @details 巨大 (inf / 1e30 級) の extent はセル範囲が i32 全域にクランプされ、
     * セル二重ループが事実上終わらない。グリッドに入れず本リストへ退避し、
     * 全クエリがセル走査に加えて本リストも検査する (RebuildGridIfDirty で再構築)。
     */
    TArray<u32>      m_HugeShapes;

    /** CollectCandidates が積むブロードフェーズ候補 (slot index、クエリ間で使い回す)。 */
    TArray<u32>      m_QueryScratch;
};

} // namespace acs::game

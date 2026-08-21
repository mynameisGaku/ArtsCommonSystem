// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Array.h"
#include "gameframework/CollisionShapeId3D.h"
#include "math/Collision3D.h"

namespace acs::game {

/**
 * AABBとsphereを世代付きhandleで管理するGPU非依存の3D collision query world。
 *
 * @details shapeは呼び出し側が明示的に登録・更新する。queryはslot index順の決定的な線形走査で、
 * layer maskと自己除外を共通に扱う。scene同期、動的剛体、固定tick所有は上位adapterの責務とする。
 */
class CCollisionWorld3D {
public:
    /** 全layerをquery対象にするmask。 */
    static constexpr u32 kAllLayers = 0xFFFFFFFFu;

    /** shapeを持たないworldを構築する。 */
    CCollisionWorld3D() noexcept = default;

    /** 登録shapeと内部配列を破棄する。 */
    ~CCollisionWorld3D() noexcept = default;

    /** world所有権を重複させないためcopyを禁止する。 */
    CCollisionWorld3D(const CCollisionWorld3D&) = delete;

    /** world所有権を重複させないためcopy代入を禁止する。 */
    CCollisionWorld3D& operator=(const CCollisionWorld3D&) = delete;

    /**
     * AABBを登録する。
     *
     * @param bounds world空間の有限な中心と0以上の半サイズ。
     * @param layer shapeが属するlayer bitmask。0は全queryから除外される。
     * @return 登録済みhandle。入力不正、容量超過、確保失敗では無効handle。
     */
    FCollisionShapeId3D TryAddAabb(const FAabb3& bounds, u32 layer = kAllLayers) noexcept;

    /**
     * sphereを登録する。
     *
     * @param sphere world空間の有限な中心と0より大きい半径。
     * @param layer shapeが属するlayer bitmask。0は全queryから除外される。
     * @return 登録済みhandle。入力不正、容量超過、確保失敗では無効handle。
     */
    FCollisionShapeId3D TryAddSphere(const FSphere& sphere, u32 layer = kAllLayers) noexcept;

    /**
     * 登録済みAABBを更新する。
     *
     * @param id 更新対象handle。
     * @param bounds 新しいworld空間AABB。
     * @return handle・shape種別・入力が有効で更新できた場合だけtrue。
     */
    bool TryUpdateAabb(FCollisionShapeId3D id, const FAabb3& bounds) noexcept;

    /**
     * 登録済みsphereを更新する。
     *
     * @param id 更新対象handle。
     * @param sphere 新しいworld空間sphere。
     * @return handle・shape種別・入力が有効で更新できた場合だけtrue。
     */
    bool TryUpdateSphere(FCollisionShapeId3D id, const FSphere& sphere) noexcept;

    /**
     * 登録shapeのlayerを変更する。
     *
     * @param id 更新対象handle。
     * @param layer 新しいlayer bitmask。
     * @return handleが現在生存していればtrue。
     */
    bool TrySetLayer(FCollisionShapeId3D id, u32 layer) noexcept;

    /**
     * 登録shapeのlayerを取得する。
     *
     * @param id 取得対象handle。
     * @param out_layer layerの書き込み先。失敗時は変更しない。
     * @return handleが現在生存していればtrue。
     */
    bool TryGetLayer(FCollisionShapeId3D id, u32& out_layer) const noexcept;

    /**
     * shapeを削除してhandleを失効させる。
     *
     * @param id 削除対象handle。
     * @return 現在生存するshapeを削除できた場合だけtrue。
     */
    bool TryRemove(FCollisionShapeId3D id) noexcept;

    /** handleがこのworldで現在生存していればtrueを返す。 */
    bool IsAlive(FCollisionShapeId3D id) const noexcept;

    /** 現在登録されているshape数を返す。 */
    u32 ShapeCount() const noexcept
    {
        return m_ShapeCount;
    }

    /** 全shapeを削除する。既存handleはslot再利用後もgeneration不一致で失効する。 */
    void ClearAll() noexcept;

    /**
     * AABBと重なるshapeをslot index順で列挙する。
     *
     * @param bounds world空間のquery AABB。
     * @param out_shapes 結果の書き込み先。失敗時は変更しない。
     * @param exclude queryから除外するshape。無効またはstaleなら除外なし。
     * @param mask layerとのANDが0でないshapeだけを含めるmask。
     * @return 入力検証と結果構築に成功した場合true。重なり0件でもtrue。
     */
    bool TryOverlapAabb(const FAabb3& bounds, TArray<FCollisionShapeId3D>& out_shapes, FCollisionShapeId3D exclude = {}, u32 mask = kAllLayers) const noexcept;

    /**
     * sphereと重なるshapeをslot index順で列挙する。
     *
     * @param sphere world空間のquery sphere。
     * @param out_shapes 結果の書き込み先。失敗時は変更しない。
     * @param exclude queryから除外するshape。無効またはstaleなら除外なし。
     * @param mask layerとのANDが0でないshapeだけを含めるmask。
     * @return 入力検証と結果構築に成功した場合true。重なり0件でもtrue。
     */
    bool TryOverlapSphere(const FSphere& sphere, TArray<FCollisionShapeId3D>& out_shapes, FCollisionShapeId3D exclude = {}, u32 mask = kAllLayers) const noexcept;

    /**
     * 指定区間のrayに最初に当たるshapeを返す。
     *
     * @details directionは非正規化でもよく、TとPointは`origin + T * direction`で対応する。
     * 同じTではslot indexが小さいshapeを選ぶ。失敗・外れでは出力を変更しない。
     * @param ray world空間ray。
     * @param minimum_t 含める最小T。有限かつ0以上。
     * @param maximum_t 含める最大T。有限かつminimum_t以上。
     * @param out_hit T、world命中点、world法線の書き込み先。
     * @param out_shape 命中shape handleの書き込み先。
     * @param exclude queryから除外するshape。無効またはstaleなら除外なし。
     * @param mask layerとのANDが0でないshapeだけを含めるmask。
     * @return 入力が有効でshapeへ命中した場合だけtrue。
     */
    bool TryRaycast(const FRay3& ray, f32 minimum_t, f32 maximum_t, FRayHit3& out_hit, FCollisionShapeId3D& out_shape, FCollisionShapeId3D exclude = {}, u32 mask = kAllLayers) const noexcept;

private:
    /** slotに格納したshape種別。 */
    enum class EKind : u8 {
        /** 未使用slot。 */
        None = 0,

        /** 軸並行境界box。 */
        Aabb,

        /** 球。 */
        Sphere,
    };

    /** 一つのshapeとhandle検証情報を保持するslot。 */
    struct FSlot {
        /** 格納shape種別。 */
        EKind Kind = EKind::None;

        /** slotが現在使用中ならtrue。 */
        bool Active = false;

        /** slot再利用を識別する世代番号。 */
        u8 Generation = 0u;

        /** query選別に使うlayer bitmask。 */
        u32 Layer = kAllLayers;

        /** KindがAabbのとき有効なshape値。 */
        FAabb3 Aabb{};

        /** KindがSphereのとき有効なshape値。 */
        FSphere Sphere{};
    };

    /** handleの24bit indexで表現できる最大slot index。 */
    static constexpr u32 kMaximumSlotIndex = 0x00FFFFFFu;

    /**
     * 未使用slotを取得する。
     *
     * @param out_index 確保したslot indexの書き込み先。失敗時は変更しない。
     * @return 既存slot再利用または配列拡張に成功した場合true。
     */
    bool TryAcquireSlot_Internal(u32& out_index) noexcept;

    /** handleが指す生存slotを返す。無効、範囲外、世代不一致ではnullptr。 */
    FSlot* FindSlot_Internal(FCollisionShapeId3D id) noexcept;

    /** handleが指す生存slotを返す。無効、範囲外、世代不一致ではnullptr。 */
    const FSlot* FindSlot_Internal(FCollisionShapeId3D id) const noexcept;

    /** index 0を無効値用に予約したshape slot配列。 */
    TArray<FSlot> m_Slots;

    /** 現在生存しているshape数。 */
    u32 m_ShapeCount = 0u;
};

} // namespace acs::game

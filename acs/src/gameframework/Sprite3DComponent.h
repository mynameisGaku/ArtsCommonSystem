// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "asset/ImageAsset.h"
#include "container/String.h"
#include "container/StringView.h"
#include "gameframework/AComponent.h"
#include "math/Collision3D.h"
#include "memory/SharedPtr.h"

namespace acs::game {

/**
 * ノードに固定向きの3Dスプライト画像を持たせるコンポーネント。
 *
 * @details ノードのローカルXY平面に画像を描くためのCPU側状態だけを所有する。
 * GPUリソースは描画アダプターが所有し、本型には保持しない。
 */
class ASprite3DComponent final : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(ASprite3DComponent)

    /** 参照する画像パスを返す。 */
    FStringView TexturePath() const noexcept;

    /** 参照する画像パスを設定する。 */
    void SetTexturePath(FStringView path) noexcept;

    /** デコード済み画像を共有所有する。nullを渡すと画像だけを外す。 */
    void SetImageAsset(TSharedPtr<AAsset> image) noexcept;

    /** 所有している画像アセットを返す。 */
    const TSharedPtr<AAsset>& ImageAsset() const noexcept;

    /** デコード済み画像を所有している場合にtrueを返す。 */
    bool HasImageAsset() const noexcept;

    /** 所有画像をAImageAssetとして返し、未設定ならnullptrを返す。 */
    AImageAsset* Image() const noexcept;

    /** ローカルXY単位板の最小座標と最大座標を返す。 */
    void LocalBounds(FVec3& minimum, FVec3& maximum) const noexcept;

    /**
     * ローカルXY単位板へraycastする純粋計算。
     *
     * @param ray コンポーネントのローカル空間へ変換済みのray。
     * @param maximum_t 探索する有限t上限。0以上でなければならない。
     * @return 板との最初のhit。入力不正、平行、板の外側ではhit=false。
     */
    FRayHit3 RaycastLocalGeometry(const FRay3& ray, f32 maximum_t = 3.4028235e38f) const noexcept;

private:
    /** SPR3Dに記録された画像アセットパス。 */
    FString m_TexturePath;

    /** シーン読込時にデコードした画像の共有所有権。 */
    TSharedPtr<AAsset> m_ImageAsset;
};

} // namespace acs::game

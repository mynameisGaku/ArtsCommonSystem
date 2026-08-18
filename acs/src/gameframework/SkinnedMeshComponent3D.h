// SPDX-License-Identifier: Apache-2.0
// GameFramework — ASkinnedMeshComponent3D
//
// 骨で動くメッシュをノードへ付ける部品。静的な AMeshComponent3D の «動く» 版。
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "container/String.h"
#include "container/StringView.h"
#include "memory/SharedPtr.h"
#include "asset/SkinnedMesh.h"
#include "gameframework/AComponent.h"

namespace acs::game {

/**
 * 骨で動くメッシュをノードへ付ける部品。
 *
 * @details
 * `AMeshComponent3D` の «動く» 版。**あちらとは別の描画経路を通る** ので、同じノードへ
 * 両方付けると二重に描かれる。
 *
 * 姿勢の計算は `CAnimationPlayer` が持っている。この部品がするのは、
 * **アセットと再生位置をノードに結び付けて、毎フレーム時刻を進めること**だけ。
 *
 * ```cpp
 * ASkinnedMeshComponent3D& Skin = Node->AddComponent<ASkinnedMeshComponent3D>();
 * Skin.SetMeshAsset( Loaded );
 * Skin.Play( 0 );
 * ```
 *
 * @note
 * 描く側 (`CSkinnedShader`) は Blinn-Phong で、静的メッシュの物理ベースとは別の
 * 照らし方をする。**同じ場面に並べると質感が揃わない。**
 */
class ASkinnedMeshComponent3D : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(ASkinnedMeshComponent3D)

    /** 空の状態で構築する (アセット未設定では描かれない)。 */
    ASkinnedMeshComponent3D() noexcept = default;

    /**
     * 骨付きメッシュを設定する。
     *
     * @details
     * 設定すると再生位置は先頭へ戻る。`LoadSkinnedMeshFromFbxMemory` の結果を渡す。
     *
     * @param asset 骨付きメッシュ。空を渡すと描かれなくなる。
     */
    void SetMeshAsset(TSharedPtr<ASkinnedMeshAsset> asset) noexcept;

    /**
     * 設定されている骨付きメッシュを返す。
     *
     * @return 骨付きメッシュ。未設定なら空。
     */
    const TSharedPtr<ASkinnedMeshAsset>& MeshAsset() const noexcept { return m_Mesh; }

    /**
     * 描ける状態か。
     *
     * @return アセットが在り、頂点と骨を持っていれば true。
     */
    bool IsRenderable() const noexcept;

    /**
     * アニメーションを再生する。
     *
     * @param index クリップ番号。範囲外なら何も起きない。
     * @param loop 繰り返すなら true。
     */
    void Play(u32 index, bool loop = true) noexcept;

    /**
     * 名前でアニメーションを探して再生する。
     *
     * @details
     * **番号は書き出しの度に変わりうる。** 名前で指定できる方が壊れにくい。
     *
     * @param name クリップ名。
     * @param loop 繰り返すなら true。
     * @return 見つかって再生を始めたら true。
     */
    bool PlayByName(FStringView name, bool loop = true) noexcept;

    /** 再生を止める (姿勢はその場に残る)。 */
    void Pause() noexcept;

    /**
     * 再生を進める部分。
     *
     * @param dt 経過秒。
     */
    void OnUpdate(f32 dt) noexcept override;

    /**
     * 再生位置と姿勢を持っているプレイヤを返す。
     *
     * @details 描く側がボーンパレットを取り出すのに使う。
     * @return プレイヤ。
     */
    const CAnimationPlayer& Player() const noexcept { return m_Player; }

    /**
     * アルベド色を返す。
     *
     * @return 現在の色。
     */
    FVec3 Color() const noexcept { return m_Color; }

    /**
     * アルベド色を設定する。
     *
     * @param color 新しい色。
     */
    void SetColor(FVec3 color) noexcept { m_Color = color; }

    /**
     * 時間を進めるかどうかを設定する。
     *
     * @details 止めても姿勢は保たれる。ポーズ画面などで使う。
     * @param advance 進めるなら true。
     */
    void SetAdvancing(bool advance) noexcept { m_Advancing = advance; }

    /**
     * 時間を進める状態か。
     *
     * @return 進めるなら true。
     */
    bool IsAdvancing() const noexcept { return m_Advancing; }

private:
    /** 骨付きメッシュ (所有を分け合う)。 */
    TSharedPtr<ASkinnedMeshAsset> m_Mesh;

    /** 再生位置と姿勢。 */
    CAnimationPlayer m_Player;

    /** アルベド色。 */
    FVec3 m_Color{1.0f, 1.0f, 1.0f};

    /** 毎フレーム時間を進めるか。 */
    bool m_Advancing = true;
};

} // namespace acs::game

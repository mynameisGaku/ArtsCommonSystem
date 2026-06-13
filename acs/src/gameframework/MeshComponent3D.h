// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — FMeshComponent3D
//
// FNode3D に attach する «描画データ» コンポーネント (データのみ・GPU 非依存)。
// 「このノードが何を描くか」(プリミティブ種別 or メッシュアセットパス、色) を保持
// するだけで、実際の描画は外部のレンダラ (editor_abi の 3D ビューポート等) がノード
// ツリーを走査してこのコンポーネントを読み取って行う。
//
// editor_abi の ENode3D のデータモデル (prim 0=Cube/1=Sphere/2=Plane/3=Mesh, color,
// mesh_path) と 1:1 対応させ、将来エンジン側シーングラフを編集対象の実体にできる橋渡し。
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "container/String.h"
#include "container/StringView.h"
#include "gameframework/Component3D.h"

namespace acs::game {

/**
 * メッシュコンポーネントが描くプリミティブ種別 (editor_abi の ENode3D.prim と一致)。
 */
enum class EMeshPrimitive3D : u8 {
    /** 立方体。 */
    Cube   = 0,
    /** 球。 */
    Sphere = 1,
    /** 平面 (床)。 */
    Plane  = 2,
    /** 外部メッシュアセット (MeshPath() を読む)。 */
    Mesh   = 3,
};

/**
 * FNode3D に「何を描くか」を持たせるデータのみのコンポーネント (GPU 非依存)。
 *
 * @details
 * プリミティブ種別 / メッシュパス / 色を保持するだけで描画はしない。外部レンダラが
 * シーンを走査して読み取る。editor_abi の ENode3D と同じデータモデルなので、エンジンの
 * 3D シーングラフをエディタの編集対象に昇格させる土台になる。
 */
class FMeshComponent3D : public FComponent3D {
public:
    ACS_GAME_COMPONENT3D_KIND(FMeshComponent3D)

    /** 既定 (Cube・白) で構築する。 */
    FMeshComponent3D() noexcept = default;

    /**
     * プリミティブ種別を指定して構築する。
     *
     * @param prim 描くプリミティブ種別。
     */
    explicit FMeshComponent3D(EMeshPrimitive3D prim) noexcept : m_Prim(prim) {}

    /**
     * プリミティブ種別を返す。
     *
     * @return 現在のプリミティブ種別。
     */
    EMeshPrimitive3D Primitive() const noexcept { return m_Prim; }

    /**
     * プリミティブ種別を設定する。
     *
     * @param p 設定するプリミティブ種別。
     */
    void SetPrimitive(EMeshPrimitive3D p) noexcept { m_Prim = p; }

    /**
     * 外部メッシュアセットのパスを返す (Primitive() == Mesh のときに有効)。
     *
     * @return メッシュパスの文字列ビュー。
     */
    FStringView MeshPath() const noexcept { return m_MeshPath.View(); }

    /**
     * 外部メッシュアセットのパスを設定する (種別も Mesh に切り替える)。
     *
     * @param path メッシュアセットのパス。
     */
    void SetMeshPath(FStringView path) noexcept { m_MeshPath = FString(path); m_Prim = EMeshPrimitive3D::Mesh; }

    /**
     * アルベド色 (RGBA) を返す。
     *
     * @return 現在の色。
     */
    FVec4 Color() const noexcept { return m_Color; }

    /**
     * アルベド色 (RGBA) を設定する。
     *
     * @param c 設定する色。
     */
    void SetColor(FVec4 c) noexcept { m_Color = c; }

    /**
     * 影を落とすかのフラグを返す。
     *
     * @return 影キャスターなら true。
     */
    bool CastsShadow() const noexcept { return m_CastShadow; }

    /**
     * 影を落とすかのフラグを設定する。
     *
     * @param b true で影キャスターにする。
     */
    void SetCastsShadow(bool b) noexcept { m_CastShadow = b; }

    /**
     * 外部レンダラが GPU メッシュ等を紐付けるための非所有ポインタを返す。
     *
     * @details エンジンは中身を解釈しない (レンダラ/エディタが意味付けする)。
     * @return 紐付けられた非所有ポインタ (未設定なら nullptr)。
     */
    void* RenderHandle() const noexcept { return m_RenderHandle; }

    /**
     * 外部レンダラ用の非所有ポインタを設定する。
     *
     * @param h 紐付ける非所有ポインタ。
     */
    void SetRenderHandle(void* h) noexcept { m_RenderHandle = h; }

private:
    /** 描くプリミティブ種別 (既定 Cube)。 */
    EMeshPrimitive3D m_Prim = EMeshPrimitive3D::Cube;

    /** 影キャスターか (既定 true)。 */
    bool             m_CastShadow = true;

    /** 外部メッシュアセットのパス (Mesh 種別で使用)。 */
    FString          m_MeshPath;

    /** アルベド色 (RGBA、既定 白)。 */
    FVec4            m_Color{ 1, 1, 1, 1 };

    /** 外部レンダラが紐付ける非所有ポインタ (GPU メッシュ等、エンジンは非解釈)。 */
    void*            m_RenderHandle = nullptr;
};

} // namespace acs::game

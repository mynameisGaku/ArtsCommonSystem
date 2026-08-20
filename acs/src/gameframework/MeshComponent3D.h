// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — AMeshComponent3D
//
// ANode に attach する «描画データ» コンポーネント (データのみ・GPU 非依存)。
// 「このノードが何を描くか」(プリミティブ種別 or メッシュアセットパス、色) を保持
// するだけで、実際の描画は外部のレンダラ (editor_abi の 3D ビューポート等) がノード
// ツリーを走査してこのコンポーネントを読み取って行う。
//
// editor_abi の従来 3D ノードデータモデル (prim 0=Cube/1=Sphere/2=Plane/3=Mesh, color,
// mesh_path) と 1:1 対応させ、将来エンジン側シーングラフを編集対象の実体にできる橋渡し。
#pragma once

#include "foundation/Types.h"
#include "math/Collision3D.h"
#include "math/Vec.h"
#include "container/String.h"
#include "container/StringView.h"
#include "memory/SharedPtr.h"
#include "asset/MeshAsset.h"   // AMeshAsset / AAsset (Render→Asset 経由で gameframework から可視)
#include "gameframework/AComponent.h"
#include "gameframework/Material2D.h"   // FMaterial2D (.acsmat マテリアルアセット = 2D と共通の材質型)

namespace acs::game {

/**
 * メッシュコンポーネントが描くプリミティブ種別 (editor_abi の 3D ノード prim と一致)。
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
 * ANode に「何を描くか」を持たせるデータのみのコンポーネント (GPU 非依存)。
 *
 * @details
 * プリミティブ種別 / メッシュパス / 色を保持するだけで描画はしない。外部レンダラが
 * シーンを走査して読み取る。editor_abi の従来 3D ノードと同じデータモデルなので、エンジンの
 * 3D シーングラフをエディタの編集対象に昇格させる土台になる。
 */
class AMeshComponent3D : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(AMeshComponent3D)

    /** 既定 (Cube・白) で構築する。 */
    AMeshComponent3D() noexcept = default;

    /**
     * プリミティブ種別を指定して構築する。
     *
     * @param prim 描くプリミティブ種別。
     */
    explicit AMeshComponent3D(EMeshPrimitive3D prim) noexcept : m_Prim(prim) {}

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

    /**
     * CPU メッシュアセット (頂点/インデックス) を «所有» して設定する。
     *
     * @details
     * editor の 3D ノード mesh (TSharedPtr<AAsset>) と同じ所有モデル。non-null を渡すと種別も
     * Mesh に切り替える。コンポーネントが強参照を持つので、ノードが生きている間メッシュは
     * 解放されない (= 描画/ピックの side-table 不要)。
     * @param a 所有するメッシュアセット (AMeshAsset を指す AAsset。null で外す)。
     */
    void SetMeshAsset(TSharedPtr<AAsset> a) noexcept {
        m_MeshAsset = static_cast<TSharedPtr<AAsset>&&>(a);
        if (m_MeshAsset) m_Prim = EMeshPrimitive3D::Mesh;
    }

    /**
     * 所有しているメッシュアセット (AAsset 基底) への共有ポインタを返す。
     *
     * @return 所有メッシュアセット (未設定なら空)。
     */
    const TSharedPtr<AAsset>& MeshAsset() const noexcept { return m_MeshAsset; }

    /**
     * メッシュアセットを所有しているかを返す。
     *
     * @return 非 null のメッシュアセットを持つなら true。
     */
    bool HasMeshAsset() const noexcept { return static_cast<bool>(m_MeshAsset); }

    /**
     * 所有メッシュを AMeshAsset 型として返す (頂点/インデックスへの直接アクセス)。
     *
     * @details 本コンポーネントが保持する AAsset は常に AMeshAsset 前提 (ローダ出力)。
     * @return AMeshAsset へのポインタ (未設定なら nullptr)。
     */
    AMeshAsset* Mesh() const noexcept { return static_cast<AMeshAsset*>(m_MeshAsset.Get()); }

    /**
     * component local空間の描画形状へraycastする。
     *
     * @details Cube、Sphere、有限Planeは各primitive形状、Meshは有効なauthored triangleを使う。
     * GPU resourceやnode transformには依存せず、不正入力または形状なしでは非命中を返す。
     * @param ray component local空間のray。
     * @param maximum_t 探索する有限t上限。0以上でなければならない。
     * @return 最も手前の形状hit。外れまたは入力不正ではhit=false。
     */
    FRayHit3 RaycastLocalGeometry(const FRay3& ray, f32 maximum_t = 3.4028235e38f) const noexcept;

    // --- マテリアルアセット (.acsmat) 参照 ---------------------------------------------
    // 2D の ANode::m_Mat (ランタイム POD の材質状態) に相当するものをコンポーネントへ持たせる。
    // ノードは «どの .acsmat を使うか» を material_path で参照し、FMaterial2D へ遅延ロードして
    // キャッシュする。これがランタイム真実 (standalone/Play も material_path から自前で解決可能)。

    /** 参照中の .acsmat パスを返す (未設定なら空)。 */
    FStringView MaterialPath() const noexcept { return m_MaterialPath.View(); }

    /** 参照する .acsmat パスを設定する (キャッシュは MarkMaterialDirty で次回再ロード)。 */
    void SetMaterialPath(FStringView path) noexcept { m_MaterialPath = FString(path); m_MaterialLoaded = false; }

    /** 材質パスを持つか (非空)。 */
    bool HasMaterial() const noexcept { return !m_MaterialPath.IsEmpty(); }

    /** 材質参照を外す (パス・ロード状態をクリア)。 */
    void ClearMaterial() noexcept { m_MaterialPath = FString(); m_Material = FMaterial2D{}; m_MaterialLoaded = false; }

    /** キャッシュ済みのマテリアル (読み取り) を返す。 */
    const FMaterial2D& Material() const noexcept { return m_Material; }

    /** キャッシュ済みのマテリアル (書込み可。ロード結果の格納先) を返す。 */
    FMaterial2D& MaterialMut() noexcept { return m_Material; }

    /** マテリアル値をコピーして焼き込む (2D の ANode::SetMaterial 鏡映)。 */
    void SetMaterial(const FMaterial2D& m) noexcept { m_Material = m; m_MaterialLoaded = true; }

    /** material_path を解析済み (キャッシュ有効) か。 */
    bool MaterialLoaded() const noexcept { return m_MaterialLoaded; }

    /** キャッシュ有効フラグを設定する (ロード完了時に true)。 */
    void SetMaterialLoaded(bool b) noexcept { m_MaterialLoaded = b; }

    /** キャッシュを無効化して次回再ロードさせる (.acsmat 保存後の reload 用)。 */
    void MarkMaterialDirty() noexcept { m_MaterialLoaded = false; }

private:
    /** 描くプリミティブ種別 (既定 Cube)。 */
    EMeshPrimitive3D m_Prim = EMeshPrimitive3D::Cube;

    /** 影キャスターか (既定 true)。 */
    bool             m_CastShadow = true;

    /** 外部メッシュアセットのパス (Mesh 種別で使用)。 */
    FString          m_MeshPath;

    /** アルベド色 (RGBA、既定 白)。 */
    FVec4            m_Color{ 1, 1, 1, 1 };

    /** 所有する CPU メッシュアセット (AMeshAsset。prim==Mesh で描画/ピックに使う)。 */
    TSharedPtr<AAsset> m_MeshAsset;

    /** 外部レンダラが紐付ける非所有ポインタ (GPU メッシュ等、エンジンは非解釈)。 */
    void*            m_RenderHandle = nullptr;

    /** 参照する .acsmat マテリアルアセットのパス (空=未設定)。 */
    FString          m_MaterialPath;

    /** material_path を解析した FMaterial2D キャッシュ (ランタイム真実)。 */
    FMaterial2D      m_Material{};

    /** material キャッシュが有効か (false で次回 LoadAcsmatFile し直し)。 */
    bool             m_MaterialLoaded = false;
};

} // namespace acs::game

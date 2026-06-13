// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — FScene3D
//
// 3D シーングラフの実用コンテナ (FScene2D の軽量 3D 版)。root FNode3D ツリーを所有し、
// update/fixed-update の伝播 + 構造変更の解決をまとめて行う。描画は «3D レンダラ» が
// 別途このツリーを走査して FMeshComponent3D 等を読む (本クラスは GPU 非依存)。
//
// 注: 本クラスは Scene 基底 (2D の描画/サービス前提) を継承せず、純粋なシーングラフ
//     コンテナとして独立させている。3D レンダーパイプラインがエンジンに入った段階で
//     描画フックを «末尾に追加» する。
#pragma once

#include "foundation/Types.h"
#include "container/StringView.h"
#include "gameframework/Node3D.h"

namespace acs::game {

/**
 * 3D シーングラフを所有・駆動する実用コンテナ (FScene2D の軽量 3D 版)。
 *
 * @details
 * root FNode3D ツリーを所有し、Update/FixedUpdate で subtree 全体に伝播 + フレーム境界の
 * 構造変更 (destroy/reparent) を解決する。名前によるノード検索とノード数集計を提供する。
 * 描画は外部の 3D レンダラがツリーを走査して行う (本クラスは GPU 非依存)。
 */
class FScene3D {
public:
    /** 空のシーンを構築する (root のみ)。 */
    FScene3D() noexcept = default;

    /** シーンを破棄する (root ツリーごと解放)。 */
    ~FScene3D() noexcept = default;

    /** コピー禁止 (FNode3D ツリーを単独所有するため)。 */
    FScene3D(const FScene3D&)            = delete;

    /** コピー代入も禁止。 */
    FScene3D& operator=(const FScene3D&) = delete;

    /** ムーブ禁止。 */
    FScene3D(FScene3D&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FScene3D& operator=(FScene3D&&)      = delete;

    /**
     * シーンの root ノードへの可変参照を返す (ここに子を AddChild してツリーを組む)。
     *
     * @return root FNode3D への参照。
     */
    FNode3D&       Root()       noexcept { return m_Root; }

    /**
     * シーンの root ノードへの const 参照を返す。
     *
     * @return root FNode3D への const 参照。
     */
    const FNode3D& Root() const noexcept { return m_Root; }

    /**
     * 名前を付けて子ノードを生成し、参照を返す簡易ヘルパ。
     *
     * @details parent==nullptr のときは root の子にする。OnSpawn は AddChild 内で即時発火。
     * @param name 新規ノードの名前。
     * @param parent 親ノード (nullptr なら root)。
     * @return 生成した子ノードへの参照。
     */
    FNode3D& Spawn(FStringView name, FNode3D* parent = nullptr) noexcept;

    /**
     * 毎フレームの update。
     *
     * @details root の UpdateTree → 構造変更の解決 の順で実行する。
     * @param dt 経過秒。
     */
    void Update(f32 dt) noexcept;

    /**
     * 固定刻みの update。
     *
     * @details root の FixedUpdateTree → 構造変更の解決 の順で実行する。
     * @param fixed_dt 固定刻みの秒。
     */
    void FixedUpdate(f32 fixed_dt) noexcept;

    /**
     * 名前でノードを検索する (root を含む subtree の深さ優先探索、最初の一致)。
     *
     * @param name 検索するノード名。
     * @return 最初に一致したノード (無ければ nullptr)。
     */
    FNode3D* FindByName(FStringView name) noexcept;

    /**
     * subtree のノード総数を返す (root を含む)。
     *
     * @return ノード総数。
     */
    u32 NodeCount() const noexcept;

private:
    /** シーンの root ノード (ツリーの起点、名前 "Root")。 */
    FNode3D m_Root{ FStringView("Root") };
};

} // namespace acs::game

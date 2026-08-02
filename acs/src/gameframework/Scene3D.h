// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — CScene3D
//
// スタック上に置ける 3D シーングラフの実用コンテナ。実体は CSceneNodeGraph への
// 委譲だけに縮退しており、checked loader (Scene3DSerialize) や editor の staging の
// «一時グラフをスタックに構築して SwapContents で公開する» 用途のために残している
// (docs/SceneUnification.md Phase 2)。シーンに載る正規のグラフは AScene が保持する。
//
// 注: 本クラスは AScene 基底 (シーン lifecycle/サービス前提) を継承せず、純粋な
//     シーングラフコンテナとして独立している。AScene 派生または alias にすると
//     スタック上の一時グラフが CSubsystemCollection と TUniquePtr<CSceneServices> を
//     丸ごと抱えるため、この分離は意図的である。
#pragma once

#include "foundation/Types.h"
#include "container/StringView.h"
#include "gameframework/Forward.h"
#include "math/Collision3D.h"   // FRay3 / FAabb3 (Raycast を使う既存 consumer の互換)
#include "gameframework/ANode.h"
#include "gameframework/NodePool.h"
#include "gameframework/NodeId.h"
#include "gameframework/SceneNodeGraph.h"

namespace acs::game {

/**
 * 3D シーングラフを所有・駆動する実用コンテナ (CSceneNodeGraph の薄い wrapper)。
 *
 * @details
 * root ANode ツリーと generational pool の実体は member の CSceneNodeGraph が持ち、
 * 本クラスは全操作をそこへ委譲する。シーン文脈を持たずスタック上に置けるため、
 * checked loader が一時グラフを構築して SwapContents で公開する用途に使う。
 * 描画は外部の 3D レンダラがツリーを走査して行う (本クラスは GPU 非依存)。
 */
class CScene3D {
public:
    /** 空のシーンを構築する (root のみ。pool を初期化し root も登録する)。 */
    CScene3D() noexcept = default;

    /** シーンを破棄する (root ツリーごと解放。pool は非所有なので何も delete しない)。 */
    ~CScene3D() noexcept = default;

    /** コピー禁止 (ANode ツリーを単独所有するため)。 */
    CScene3D(const CScene3D&)            = delete;

    /** コピー代入も禁止。 */
    CScene3D& operator=(const CScene3D&) = delete;

    /** ムーブ禁止。 */
    CScene3D(CScene3D&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CScene3D& operator=(CScene3D&&)      = delete;

    /**
     * Atomically exchange complete scene ownership.
     *
     * Used by checked loaders to build a replacement graph off to the side
     * and publish it only after every allocation and component commit succeeds.
     */
    void SwapContents(CScene3D& other) noexcept { m_Graph.SwapContents(other.m_Graph); }

    /**
     * 内部のノードグラフへの可変参照を返す。
     *
     * @details graph 単位の API (AScene::Graph() と同じ型) を直接使う呼び出し側や、
     * CSceneNodeGraph を取る loader へ渡すための正規入口。
     * @return 所有する CSceneNodeGraph への参照。
     */
    CSceneNodeGraph&       Graph()       noexcept { return m_Graph; }

    /**
     * 内部のノードグラフへの const 参照を返す。
     *
     * @return 所有する CSceneNodeGraph への const 参照。
     */
    const CSceneNodeGraph& Graph() const noexcept { return m_Graph; }

    /**
     * シーンの root ノードへの可変参照を返す (ここに子を AddChild してツリーを組む)。
     *
     * @return root ANode への参照。
     */
    ANode&       Root()       noexcept { return m_Graph.Root(); }

    /**
     * シーンの root ノードへの const 参照を返す。
     *
     * @return root ANode への const 参照。
     */
    const ANode& Root() const noexcept { return m_Graph.Root(); }

    /**
     * ノードを生成し、pool登録と親へのattachを原子的に試みる。
     *
     * @details
     * parent==nullptr は root を表す。外部シーンのparent、破棄予定parent、深度上限超過を
     * 拒否する。新規ノードは先にpoolへ仮登録し、TryAddChild失敗時はUnregisterして破棄する
     * ため、失敗時にツリー、active slot数、既存ノードのIdを変更しない。
     */
    FScene3DSpawnResult TrySpawn(
        FStringView name, ANode* parent = nullptr) noexcept {
        return m_Graph.TrySpawn(name, parent);
    }

    /**
     * 名前を付けて子ノードを生成し、generational id を振って参照を返す簡易ヘルパ。
     *
     * @details
     * parent==nullptr のときは root の子にする。成功時の挙動は従来互換。
     * 失敗時は安全な sentinel として、有効なparentならparent、無効parentならrootを返す。
     * 失敗を区別する新規コードは TrySpawn を使う。
     * @param name 新規ノードの名前。
     * @param parent 親ノード (nullptr なら root)。
     * @return 生成した子ノードへの参照。
     */
    ANode& Spawn(FStringView name, ANode* parent = nullptr) noexcept {
        return m_Graph.Spawn(name, parent);
    }

    /**
     * generational id から ANode を取り出す (stale / invalid なら nullptr)。
     *
     * @param id 取り出す FNodeId。
     * @return 対応するノード (stale なら nullptr)。
     */
    ANode* Get(FNodeId id) noexcept { return m_Graph.Get(id); }

    /**
     * id が現在も生きているか (stale 検出)。
     *
     * @param id 検証する FNodeId。
     * @return 生きていれば true。
     */
    bool IsValid(FNodeId id) const noexcept { return m_Graph.IsValid(id); }

    /**
     * ノードポインタから FNodeId を逆引きする。
     *
     * @param node 逆引きするノード。
     * @return 対応する FNodeId (未登録なら invalid)。
     */
    FNodeId IdOf(ANode* node) noexcept { return m_Graph.IdOf(node); }

    /**
     * id 指定でノードを破棄予定にする (実際の reap は次の Update)。
     *
     * @details root は破棄できない (false を返す)。pool からの登録解除は次の Update の
     * PurgePendingDestroy が行う (= 破棄予定の間も id は valid のまま、reap 前に外れる)。
     * @param id 破棄するノードの FNodeId。
     * @return 破棄予定にしたら true、未登録 / root なら false。
     */
    bool Destroy(FNodeId id) noexcept { return m_Graph.Destroy(id); }

    /**
     * pool に登録されている (生きている) ノード数を返す (root を含む)。
     *
     * @return active なノード数。
     */
    u32 RegisteredCount() const noexcept { return m_Graph.RegisteredCount(); }

    /**
     * 毎フレームの update。
     *
     * @details root の UpdateTree → pool の purge → 構造変更の解決 の順で実行する。
     * @param dt 経過秒。
     */
    void Update(f32 dt) noexcept { m_Graph.Update(dt); }

    /**
     * 固定刻みの update。
     *
     * @details root の FixedUpdateTree → pool の purge → 構造変更の解決 の順で実行する。
     * @param fixed_dt 固定刻みの秒。
     */
    void FixedUpdate(f32 fixed_dt) noexcept { m_Graph.FixedUpdate(fixed_dt); }

    /**
     * 名前でノードを検索する (root を含む subtree の深さ優先探索、最初の一致)。
     *
     * @param name 検索するノード名。
     * @return 最初に一致したノード (無ければ nullptr)。
     */
    ANode* FindByName(FStringView name) noexcept { return m_Graph.FindByName(name); }

    /**
     * ワールド空間レイで最も手前のノードをピックする (AMeshComponent3D を持つノードのみ対象)。
     *
     * @details
     * 各ノードの World() 変形を逆適用してレイをローカル空間へ移し、プリミティブ種別ごとの
     * ローカル AABB と交差判定する (= 回転/スケール/階層を正しく扱う OBB ピック)。t は元の
     * world レイ上のパラメータ。Mesh 種別は頂点 AABB を使う。
     * @param ray ワールド空間のピックレイ (direction は非正規化でも可)。
     * @param out_t 非 null なら命中 t (world レイ上、`ray.origin + t*ray.direction` が命中点) を書く。
     * @return 最も手前で命中したノードの FNodeId (外れは invalid)。
     */
    FNodeId Raycast(const FRay3& ray, f32* out_t = nullptr) const noexcept {
        return m_Graph.Raycast(ray, out_t);
    }

    /**
     * subtree のノード総数を返す (root を含む)。
     *
     * @return ノード総数。
     */
    u32 NodeCount() const noexcept { return m_Graph.NodeCount(); }

    /**
     * root の全子孫を破棄してシーンを空にする (root 自身は残し、transform/名前を既定へ戻す)。
     *
     * @details
     * 各 top-level 子を Destroy → pool を purge → 即時 reap する (Update を待たない)。
     * シーン読み込み (LoadScene3DText) の «置き換え» 前処理に使う。
     */
    void Clear() noexcept { m_Graph.Clear(); }

private:
    /** root ツリーと pool の実体を所有するノードグラフ (全 API の委譲先)。 */
    CSceneNodeGraph m_Graph;
};

} // namespace acs::game

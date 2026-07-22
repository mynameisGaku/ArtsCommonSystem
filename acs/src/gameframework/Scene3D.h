// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — FScene3D
//
// 3D シーングラフの実用コンテナ (FScene2D の軽量 3D 版)。root ANode ツリーを所有し、
// update/fixed-update の伝播 + 構造変更の解決をまとめて行う。描画は «3D レンダラ» が
// 別途このツリーを走査して AMeshComponent3D 等を読む (本クラスは GPU 非依存)。
//
// 注: 本クラスは FScene 基底 (2D の描画/サービス前提) を継承せず、純粋なシーングラフ
//     コンテナとして独立させている。3D レンダーパイプラインがエンジンに入った段階で
//     描画フックを «末尾に追加» する。
#pragma once

#include "foundation/Types.h"
#include "container/StringView.h"
#include "math/Collision3D.h"   // FRay3 / FAabb3 (Raycast)
#include "gameframework/ANode.h"
#include "gameframework/NodePool.h"
#include "gameframework/NodeId.h"

namespace acs::game {

/** FScene3D::TrySpawn の大分類。詳細は PoolError / AddChildResult を参照する。 */
enum class EScene3DSpawnError : u8 {
    None = 0,
    InvalidParent,
    NodeAllocationFailure,
    PoolRegistrationFailure,
    ChildAttachRejected,
};

/** checked ノード生成結果。失敗時は Node が null でツリー/poolを変更しない。 */
struct FScene3DSpawnResult {
    ANode* Node = nullptr;
    FNodeId Id{};
    EScene3DSpawnError Error = EScene3DSpawnError::None;
    ENodePoolRegisterError PoolError = ENodePoolRegisterError::None;
    EAddChildResult AddChildResult = EAddChildResult::Added;

    bool Succeeded() const noexcept {
        return Error == EScene3DSpawnError::None && Node != nullptr && Id.IsValid();
    }
    explicit operator bool() const noexcept { return Succeeded(); }
};

/**
 * 3D シーングラフを所有・駆動する実用コンテナ (FScene2D の軽量 3D 版)。
 *
 * @details
 * root ANode ツリーを所有し、Update/FixedUpdate で subtree 全体に伝播 + フレーム境界の
 * 構造変更 (destroy/reparent) を解決する。名前によるノード検索とノード数集計を提供する。
 * 描画は外部の 3D レンダラがツリーを走査して行う (本クラスは GPU 非依存)。
 */
class FScene3D {
public:
    /** 空のシーンを構築する (root のみ。pool を初期化し root も登録する)。 */
    FScene3D() noexcept : m_Root(NewObject<ANode>(FStringView("Root"))) {
        m_Pool.Init(256);
        m_Pool.RegisterExistingNode(m_Root.Get());   // root にも有効な FNodeId を振る
    }

    /** シーンを破棄する (root ツリーごと解放。pool は非所有なので何も delete しない)。 */
    ~FScene3D() noexcept = default;

    /** コピー禁止 (ANode ツリーを単独所有するため)。 */
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
     * @return root ANode への参照。
     */
    ANode&       Root()       noexcept { return *m_Root; }

    /**
     * シーンの root ノードへの const 参照を返す。
     *
     * @return root ANode への const 参照。
     */
    const ANode& Root() const noexcept { return *m_Root; }

    /**
     * ノードを生成し、pool登録と親へのattachを原子的に試みる。
     *
     * @details
     * parent==nullptr は root を表す。外部シーンのparent、破棄予定parent、深度上限超過を
     * 拒否する。新規ノードは先にpoolへ仮登録し、TryAddChild失敗時はUnregisterして破棄する
     * ため、失敗時にツリー、active slot数、既存ノードのIdを変更しない。
     */
    FScene3DSpawnResult TrySpawn(
        FStringView name, ANode* parent = nullptr) noexcept;

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
    ANode& Spawn(FStringView name, ANode* parent = nullptr) noexcept;

    /**
     * generational id から ANode を取り出す (stale / invalid なら nullptr)。
     *
     * @param id 取り出す FNodeId。
     * @return 対応するノード (stale なら nullptr)。
     */
    ANode* Get(FNodeId id) noexcept { return m_Pool.Get(id); }

    /**
     * id が現在も生きているか (stale 検出)。
     *
     * @param id 検証する FNodeId。
     * @return 生きていれば true。
     */
    bool IsValid(FNodeId id) const noexcept { return m_Pool.IsValid(id); }

    /**
     * ノードポインタから FNodeId を逆引きする。
     *
     * @param node 逆引きするノード。
     * @return 対応する FNodeId (未登録なら invalid)。
     */
    FNodeId IdOf(ANode* node) noexcept { return m_Pool.IdOf(node); }

    /**
     * id 指定でノードを破棄予定にする (実際の reap は次の Update)。
     *
     * @details root は破棄できない (false を返す)。pool からの登録解除は次の Update の
     * PurgePendingDestroy が行う (= 破棄予定の間も id は valid のまま、reap 前に外れる)。
     * @param id 破棄するノードの FNodeId。
     * @return 破棄予定にしたら true、未登録 / root なら false。
     */
    bool Destroy(FNodeId id) noexcept {
        ANode* n = m_Pool.Get(id);
        if (n == nullptr || n == m_Root.Get()) return false;
        n->Destroy();
        return true;
    }

    /**
     * pool に登録されている (生きている) ノード数を返す (root を含む)。
     *
     * @return active なノード数。
     */
    u32 RegisteredCount() const noexcept { return m_Pool.ActiveCount(); }

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
    ANode* FindByName(FStringView name) noexcept;

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
    FNodeId Raycast(const FRay3& ray, f32* out_t = nullptr) const noexcept;

    /**
     * subtree のノード総数を返す (root を含む)。
     *
     * @return ノード総数。
     */
    u32 NodeCount() const noexcept;

    /**
     * root の全子孫を破棄してシーンを空にする (root 自身は残し、transform/名前を既定へ戻す)。
     *
     * @details
     * 各 top-level 子を Destroy → pool を purge → 即時 reap する (Update を待たない)。
     * シーン読み込み (LoadScene3DText) の «置き換え» 前処理に使う。
     */
    void Clear() noexcept;

private:
    /** シーンの root ノード (ツリーの起点、名前 "Root")。 */
    TObjectPtr<ANode> m_Root;

    /** generational id レジストリ (非所有。Spawn で登録、Update で破棄予定を purge)。 */
    FNodePool m_Pool;
};

} // namespace acs::game

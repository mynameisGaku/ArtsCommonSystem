// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — CSceneNodeGraph
//
// root ANode ツリーと CNodePool をまとめて所有する、シーン文脈非依存のノードグラフ。
// AScene が正規のシーングラフとして保持するほか、checked loader や editor の staging の
// ようにスタック上へ一時グラフを構築する用途でも使う。
// CSubsystemCollection / CSceneServices を一切持たないため、一時グラフがシーン文脈を
// 抱え込むことはない。
#pragma once

#include "foundation/Types.h"
#include "container/StringView.h"
#include "gameframework/Forward.h"
#include "gameframework/ANode.h"
#include "gameframework/NodePool.h"
#include "gameframework/NodeId.h"

namespace acs {

struct FRay3;   // math/Collision3D.h — Raycast は参照でしか受けないため前方宣言で足りる

namespace game {

/** CSceneNodeGraph::TrySpawn の大分類。詳細は PoolError / AddChildResult を参照する。 */
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
 * root ANode ツリーと generational pool を所有・駆動するノードグラフ。
 *
 * @details
 * root ANode ツリーを所有し、Update/FixedUpdate で subtree 全体に伝播 + フレーム境界の
 * 構造変更 (destroy/reparent) を解決する。名前によるノード検索とノード数集計を提供する。
 * 描画は外部のレンダラがツリーを走査して行う (本クラスは GPU 非依存)。シーン文脈を
 * 持たないため、スタック上の一時グラフとしても安全に構築できる。
 */
class CSceneNodeGraph {
public:
    /** 空のグラフを構築する (root のみ。pool を初期化し root も登録する)。 */
    CSceneNodeGraph() noexcept : m_Root(NewObject<ANode>(FStringView("Root"))) {
        m_Pool.Init(256);
        m_Pool.RegisterExistingNode(m_Root.Get());   // root にも有効な FNodeId を振る
    }

    /** グラフを破棄する (root ツリーごと解放。pool は非所有なので何も delete しない)。 */
    ~CSceneNodeGraph() noexcept = default;

    /** コピー禁止 (ANode ツリーを単独所有するため)。 */
    CSceneNodeGraph(const CSceneNodeGraph&)            = delete;

    /** コピー代入も禁止。 */
    CSceneNodeGraph& operator=(const CSceneNodeGraph&) = delete;

    /** ムーブ禁止。 */
    CSceneNodeGraph(CSceneNodeGraph&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CSceneNodeGraph& operator=(CSceneNodeGraph&&)      = delete;

    /**
     * Atomically exchange complete graph ownership.
     *
     * @details
     * Used by checked loaders to build a replacement graph off to the side
     * and publish it only after every allocation and component commit succeeds.
     * root が差し替わるため、swap 後に両グラフの root-swap hook (登録済みなら) を
     * 呼び、owner (AScene) が root への service/subsystem 配線をやり直せるようにする。
     * hook 自体は owner に属するため swap しない。
     */
    void SwapContents(CSceneNodeGraph& other) noexcept {
        m_Root.Swap(other.m_Root);
        m_Pool.Swap(other.m_Pool);
        if (m_RootSwapHook != nullptr) m_RootSwapHook(m_RootSwapHookUser);
        if (other.m_RootSwapHook != nullptr) other.m_RootSwapHook(other.m_RootSwapHookUser);
    }

    /**
     * root 差し替え時の再配線 hook を登録する (内部用。owner の AScene が設定する)。
     *
     * @details SwapContents が root を差し替えた直後に呼ばれる。グラフ自体はシーン文脈を
     * 持たないため、配線のやり直しは hook 側 (owner) の責務とする。
     * @param hook 呼び出す関数 (nullptr で解除)。
     * @param user hook へそのまま渡す owner 文脈。
     */
    void _SetRootSwapHook(void (*hook)(void* user) noexcept, void* user) noexcept {
        m_RootSwapHook     = hook;
        m_RootSwapHookUser = user;
    }

    /**
     * root ノードが確保済みかを返す (OOM で ctor が root を確保できなかった場合のみ false)。
     *
     * @return root が存在すれば true。
     */
    bool HasRoot() const noexcept { return m_Root.Get() != nullptr; }

    /**
     * グラフの root ノードへの可変参照を返す (ここに子を AddChild してツリーを組む)。
     *
     * @return root ANode への参照。
     */
    ANode&       Root()       noexcept { return *m_Root; }

    /**
     * グラフの root ノードへの const 参照を返す。
     *
     * @return root ANode への const 参照。
     */
    const ANode& Root() const noexcept { return *m_Root; }

    /**
     * ノードを生成し、pool登録と親へのattachを原子的に試みる。
     *
     * @details
     * parent==nullptr は root を表す。外部グラフのparent、破棄予定parent、深度上限超過を
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
     * @details root の UpdateTree → pool の purge → 構造変更の解決 の順で実行する。
     * @param dt 経過秒。
     */
    void Update(f32 dt) noexcept;

    /**
     * 固定刻みの update。
     *
     * @details root の FixedUpdateTree → pool の purge → 構造変更の解決 の順で実行する。
     * @param fixed_dt 固定刻みの秒。
     */
    void FixedUpdate(f32 fixed_dt) noexcept;

    /**
     * tick を伴わずに破棄予定ノードの purge と構造変更の解決だけを行う。
     *
     * @details シーン退場時など、update を回さずフレーム境界の後始末だけが要る場面で使う。
     * reap される前に破棄予定ノードを pool から外す順序は Update と同じ。
     */
    void ResolveStructuralChanges() noexcept;

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
     * 有効かつ可視なsubtreeを指定t区間だけraycastする。
     *
     * @details 親が無効、非表示、破棄予定ならsubtree全体を除外する。minimum_tより手前のhitを
     * 除外できるため、ray原点を含む追従対象を無視して3D camera障害物を探せる。
     * @param ray world空間ray。距離としてtを使う場合はdirectionを正規化する。
     * @param minimum_t 含める最小t。有限かつ0以上。
     * @param maximum_t 含める最大t。有限かつminimum_t以上。
     * @param out_t 非nullかつ命中時だけ最近hitのtを書き込む。
     * @return 区間内で最も手前の有効mesh node。入力不正または外れはinvalid。
     */
    FNodeId RaycastActiveRange(const FRay3& ray, f32 minimum_t, f32 maximum_t, f32* out_t = nullptr) const noexcept;

    /**
     * 有効かつ可視なmesh boundsへworld空間の球を指定t区間だけsweepする。
     *
     * @details node local AABBをworld scaleに応じて保守的に拡張し、球中心rayとの最初の接触を返す。
     * 回転と非一様scaleを扱い、角では安全側へ早く命中する場合がある。radius 0はRaycastActiveRangeと同じ。
     * @param center_ray world空間を移動する球中心ray。距離としてtを使う場合はdirectionを正規化する。
     * @param radius world空間の球半径。有限かつ0以上。
     * @param minimum_t 含める最小t。有限かつ0以上。
     * @param maximum_t 含める最大t。有限かつminimum_t以上。
     * @param out_t 非nullかつ命中時だけ最近hitのtを書き込む。
     * @return 区間内で最も手前の有効mesh node。入力不正または外れはinvalid。
     */
    FNodeId SweepSphereActiveRange(const FRay3& center_ray, f32 radius, f32 minimum_t, f32 maximum_t, f32* out_t = nullptr) const noexcept;

    /**
     * subtree のノード総数を返す (root を含む)。
     *
     * @return ノード総数。
     */
    u32 NodeCount() const noexcept;

    /**
     * root の全子孫を破棄してグラフを空にする (root 自身は残し、transform/名前を既定へ戻す)。
     *
     * @details
     * 各 top-level 子を Destroy → pool を purge → 即時 reap する (Update を待たない)。
     * シーン読み込み (LoadScene3DText) の «置き換え» 前処理に使う。root は差し替えないため、
     * owner が root へ張った service/subsystem 配線はそのまま残る。
     */
    void Clear() noexcept;

private:
    /** グラフの root ノード (ツリーの起点、名前 "Root")。 */
    TObjectPtr<ANode> m_Root;

    /** generational id レジストリ (非所有。Spawn で登録、Update で破棄予定を purge)。 */
    CNodePool m_Pool;

    /** SwapContents で root が差し替わった直後に呼ぶ再配線 hook (owner が登録)。 */
    void (*m_RootSwapHook)(void* user) noexcept = nullptr;

    /** m_RootSwapHook へ渡す owner 文脈。 */
    void* m_RootSwapHookUser = nullptr;
};

} // namespace game
} // namespace acs

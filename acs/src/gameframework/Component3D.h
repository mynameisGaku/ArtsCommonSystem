// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — FComponent3D
//
// FNode3D に attach する「振る舞いパーツ」の基底 (FComponent2D の 3D 版)。
// メッシュ描画 / カスタムロジック / カメラ などを継承ではなく合成で組む。
//
// 設計選択 (FComponent2D と同一):
//   ・RTTI 不使用の型 ID: `ComponentKindOf<T>()` (template static のアドレス)。
//   ・Owner& アクセス: OnAttach(FNode3D&) で owner を保存、以降 Owner() で取得。
//   ・同じ型を 1 ノードに複数 attach 可能。GetComponent<T>() は最初の一致を返す。
//   ・lifecycle: AddComponent 即時 OnAttach、Node 破棄時に OnDetach。
//
// 注: この基底はピュアロジック (GPU/描画コンテキストに依存しない)。3D 描画は将来
//     導入する 3D レンダーコンテキストで OnDraw を «末尾に追加» する (vtable 安定性)。
#pragma once

#include "foundation/Types.h"

namespace acs::game {

class FNode3D;

/**
 * 型 T ごとに一意なコンポーネント種別 ID を返す (RTTI 不使用、FComponent3D 用)。
 *
 * @details
 * `template static int` のアドレスを ID に使う。T ごとに別 instantiation = 別アドレス
 * となるため、static_cast の安全性チェックに使える。FComponent2D 側の同名関数とは
 * 別 namespace スコープではなくシグネチャ (戻り値) で区別する必要はない (型引数で別物)。
 * @tparam T 種別 ID を取りたい FComponent3D 派生型。
 * @return T に固有の安定したポインタ ID。
 */
template<typename T>
const void* Component3DKindOf() noexcept {
    static const int s_tag = 0;
    return static_cast<const void*>(&s_tag);
}

/**
 * FNode3D に attach する「振る舞いパーツ」の基底 (FComponent2D の 3D 版)。
 *
 * @details
 * メッシュ描画 / カスタムロジック / カメラ等を継承ではなく合成で組み上げる。owner ノードを
 * `Owner()` で参照し、必要な lifecycle フックだけ override する。同じ型を 1 ノードに複数
 * attach 可能で、種別 ID は Component3DKindOf<T>() による RTTI 不使用の型タグで識別する。
 */
class FComponent3D {
public:
    /** 空のコンポーネントを構築する (owner は attach 時に設定)。 */
    FComponent3D() noexcept = default;

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~FComponent3D() noexcept = default;

    /** コピー禁止 (コンポーネントは TUniquePtr で単独所有するため)。 */
    FComponent3D(const FComponent3D&)            = delete;

    /** コピー代入も禁止。 */
    FComponent3D& operator=(const FComponent3D&) = delete;

    /** ムーブ禁止 (owner が保持するポインタの安定性を保つため)。 */
    FComponent3D(FComponent3D&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FComponent3D& operator=(FComponent3D&&)      = delete;

    /**
     * このコンポーネントの種別 ID を返す (派生は ACS_GAME_COMPONENT3D_KIND で実装)。
     *
     * @return Component3DKindOf<派生型>() の安定ポインタ ID。
     */
    virtual const void* Kind() const noexcept = 0;

    /**
     * リフレクション登録名 (= クラス名) を返す。未対応は nullptr。
     *
     * @return クラス名文字列 (反射カタログの登録名と一致)。未対応は nullptr。
     */
    virtual const char* ReflectName() const noexcept { return nullptr; }

    /**
     * 依存コンポーネント宣言フック (Unity の [RequireComponent] 相当)。
     *
     * @details AddComponent が OnAttach の前に 1 度だけ呼ぶ。既定 no-op。
     * @param owner このコンポーネントが attach される先のノード。
     */
    virtual void OnRequire(FNode3D& /*owner*/) noexcept {}

    /**
     * attach 直後に 1 回呼ばれる初期化フック。
     *
     * @details 既定 no-op。
     * @param owner このコンポーネントの owner ノード。
     */
    virtual void OnAttach(FNode3D& /*owner*/) noexcept {}

    /**
     * 毎フレーム呼ばれる可変刻み update フック。
     *
     * @details Node の OnUpdate の後に呼ばれる。既定 no-op。
     * @param dt 前フレームからの経過秒。
     */
    virtual void OnUpdate(f32 /*dt*/) noexcept {}

    /**
     * 固定刻み update フック (物理・決定論ロジック)。
     *
     * @details 既定 no-op。
     * @param fixed_dt 固定刻みの秒。
     */
    virtual void OnFixedUpdate(f32 /*fixed_dt*/) noexcept {}

    /**
     * detach (owner 破棄) 直前に 1 回呼ばれる後始末フック。
     *
     * @details 既定 no-op。
     */
    virtual void OnDetach() noexcept {}

    /**
     * owner ノードへの可変参照を返す。
     *
     * @return owner ノードへの参照。
     */
    FNode3D& Owner() noexcept { return *m_Owner; }

    /**
     * owner ノードへの const 参照を返す。
     *
     * @return owner ノードへの const 参照。
     */
    const FNode3D& Owner() const noexcept { return *m_Owner; }

    /**
     * owner が設定済みかを返す。
     *
     * @return owner が非 null なら true。
     */
    bool HasOwner() const noexcept { return m_Owner != nullptr; }

    /**
     * owner ポインタを設定する (内部用。FNode3D::AddComponent が呼ぶ)。
     *
     * @param o 設定する owner ノード。
     */
    void _SetOwner(FNode3D* o) noexcept { m_Owner = o; }

private:
    /** owner ノード (attach 前は nullptr)。 */
    FNode3D* m_Owner = nullptr;
};

/** 派生クラスで Kind() + ReflectName() を 1 行で override するためのマクロ。 */
#define ACS_GAME_COMPONENT3D_KIND(T)                                                 \
    const void* Kind() const noexcept override                                       \
    { return ::acs::game::Component3DKindOf<T>(); }                                   \
    const char* ReflectName() const noexcept override { return #T; }

} // namespace acs::game

// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — FComponent2D
//
// FNode2D に attach する「振る舞いパーツ」の基底。sprite 描画 / 当たり判定 /
// アニメーション / 音再生 / カスタムロジック などを **継承ではなく合成** で組み
// 上げる (composition over inheritance)。
//
// 使い方:
//   class RotateComponent : public FComponent2D {
//   public:
//       ACS_GAME_COMPONENT_KIND(RotateComponent)
//       explicit RotateComponent(f32 speed_rps) noexcept : m_Speed(speed_rps) {}
//       void OnUpdate(f32 dt) noexcept override {
//           Owner().Local().rotation += m_Speed * dt;
//       }
//   private:
//       f32 m_Speed;
//   };
//
//   auto node = MakeUnique<FNode2D>();
//   node->AddComponent<RotateComponent>(/*speed_rps=*/1.0f);
//   root.AddChild(Move(node));
//
// 設計選択:
//   ・**RTTI 不使用の型 ID**: `template static const int` のアドレスを使う
//     (`ComponentKindOf<T>()`、AppState と同パターン)。`virtual Kind()` で
//     返して `FNode2D::GetComponent<T>()` の static_cast に使う。
//   ・**Owner&** アクセス: コンポーネントは `OnAttach(FNode2D&)` で owner ref
//     を保存。以降 `Owner()` で取り出す (raw pointer、stale はあり得ない =
//     コンポーネントは owner の寿命より長く生きない)。
//   ・**multiple components per kind**: 同じ型のコンポーネントを 1 ノードに
//     複数 attach 可能 (例: 2 つの blink タイマー)。`GetComponent<T>()` は
//     最初の一致を返す (線形探索)。
//   ・**lifecycle**: AddComponent 即時 `OnAttach`、Node 破棄時に `OnDetach`
//     (compact pattern で OnDespawn と一緒に呼ぶ)。OnUpdate/OnDraw は Node の
//     対応フックの後に呼ばれる。
#pragma once

#include "foundation/Types.h"

namespace acs::game {

class FNode2D;
class RenderContext;

/**
 * 型 T ごとに一意なコンポーネント種別 ID を返す (RTTI 不使用)。
 *
 * @details
 * `template static int` のアドレスを ID に使う。T ごとに別 instantiation =
 * 別アドレスとなるため、static_cast の安全性チェックに使える。
 * @tparam T 種別 ID を取りたい FComponent2D 派生型。
 * @return T に固有の安定したポインタ ID。
 */
template<typename T>
const void* ComponentKindOf() noexcept {
    static const int s_tag = 0;
    return static_cast<const void*>(&s_tag);
}

/**
 * FNode2D に attach する「振る舞いパーツ」の基底。
 *
 * @details
 * sprite 描画 / 当たり判定 / アニメーション / 音再生 / カスタムロジックを継承では
 * なく合成で組み上げる (composition over inheritance)。owner ノードを `Owner()` で
 * 参照し、必要な lifecycle フックだけ override する。同じ型を 1 ノードに複数 attach
 * 可能で、種別 ID は ComponentKindOf<T>() による RTTI 不使用の型タグで識別する。
 */
class FComponent2D {
public:
    /** 空のコンポーネントを構築する (owner は attach 時に設定)。 */
    FComponent2D() noexcept = default;

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~FComponent2D() noexcept = default;

    /** コピー禁止 (コンポーネントは TUniquePtr で単独所有するため)。 */
    FComponent2D(const FComponent2D&)            = delete;

    /** コピー代入も禁止。 */
    FComponent2D& operator=(const FComponent2D&) = delete;

    /** ムーブ禁止 (owner が保持するポインタの安定性を保つため)。 */
    FComponent2D(FComponent2D&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FComponent2D& operator=(FComponent2D&&)      = delete;

    /**
     * このコンポーネントの種別 ID を返す (派生は ACS_GAME_COMPONENT_KIND で実装)。
     *
     * @return ComponentKindOf<派生型>() の安定ポインタ ID。
     */
    virtual const void* Kind() const noexcept = 0;

    /**
     * 依存コンポーネント宣言フック (Unity の [RequireComponent] 相当)。
     *
     * @details
     * AddComponent が OnAttach の前に 1 度だけ呼ぶ。実装側で
     * `owner.GetOrAddComponent<Dep>()` を呼べば必要な兄弟コンポーネントを自動確保
     * できる (型安全・reflection 不要)。依存は本コンポーネントより先に attach される
     * ので、OnAttach/OnUpdate から GetComponent<Dep>() で必ず取れる。既定 no-op。
     * @param owner このコンポーネントが attach される先のノード。
     */
    virtual void OnRequire(FNode2D& /*owner*/) noexcept {}

    /**
     * attach 直後に 1 回呼ばれる初期化フック。
     *
     * @details 既定 no-op。必要な派生のみ override する。
     * @param owner このコンポーネントの owner ノード。
     */
    virtual void OnAttach(FNode2D& /*owner*/) noexcept {}

    /**
     * 毎フレーム呼ばれる可変刻み update フック。
     *
     * @details Node の OnUpdate の後に呼ばれる。既定 no-op。
     * @param dt 前フレームからの経過秒。
     */
    virtual void OnUpdate(f32 /*dt*/)        noexcept {}

    /**
     * 固定刻み update フック (物理・決定論ロジック)。
     *
     * @details
     * FGame の fixed-step accumulator から Scene 経由で呼ばれ、同フレームで複数回
     * (catch-up) または 0 回 (slow-down clamp) 呼ばれ得る。既定 no-op。
     * @param fixed_dt 固定刻みの秒。
     */
    virtual void OnFixedUpdate(f32 /*fixed_dt*/) noexcept {}

    /**
     * 描画フック。スプライト等を積む。
     *
     * @details 既定 no-op。
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    virtual void OnDraw(RenderContext& /*rc*/) noexcept {}

    /**
     * OnDraw と子ツリー描画の「後」に呼ばれる後処理フック。
     *
     * @details
     * OnDraw で設定した描画状態 (ステンシルマスク等) を子ツリーの描画後に解除する
     * のに使う。これにより 1 コンポーネントで「子ツリーをマスクで囲う」が書ける。既定 no-op。
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    virtual void OnDrawPostChildren(RenderContext& /*rc*/) noexcept {}

    /**
     * detach (owner 破棄) 直前に 1 回呼ばれる後始末フック。
     *
     * @details 既定 no-op。
     */
    virtual void OnDetach()                  noexcept {}

    /**
     * owner ノードへの可変参照を返す。
     *
     * @return owner ノードへの参照。
     */
    FNode2D& Owner() noexcept { return *m_Owner; }

    /**
     * owner ノードへの const 参照を返す。
     *
     * @return owner ノードへの const 参照。
     */
    const FNode2D& Owner() const noexcept { return *m_Owner; }

    /**
     * owner が設定済みかを返す。
     *
     * @return owner が非 null なら true。
     */
    bool HasOwner() const noexcept { return m_Owner != nullptr; }

    /**
     * owner ポインタを設定する (内部用。FNode2D::AddComponent が呼ぶ)。
     *
     * @param o 設定する owner ノード。
     */
    void _SetOwner(FNode2D* o) noexcept { m_Owner = o; }

private:
    /** owner ノード (attach 前は nullptr)。 */
    FNode2D* m_Owner = nullptr;
};

/** 派生クラスで Kind() を 1 行で override するためのマクロ。 */
#define ACS_GAME_COMPONENT_KIND(T)                                                  \
    const void* Kind() const noexcept override                                       \
    { return ::acs::game::ComponentKindOf<T>(); }

} // namespace acs::game

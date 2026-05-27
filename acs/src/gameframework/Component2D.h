// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — FComponent2D (Phase 7)
//
// FNode2D に attach する「振る舞いパーツ」の基底。sprite 描画 / 当たり判定 /
// アニメーション / 音再生 / カスタムロジック などを **継承ではなく合成** で組み
// 上げる (composition over inheritance)。
//
// 使い方:
//   class RotateComponent : public FComponent2D {
//   public:
//       ACS_GAME_COMPONENT_KIND(RotateComponent)
//       explicit RotateComponent(f32 speed_rps) noexcept : _speed(speed_rps) {}
//       void OnUpdate(f32 dt) noexcept override {
//           Owner().Local().rotation += _speed * dt;
//       }
//   private:
//       f32 _speed;
//   };
//
//   auto node = MakeUnique<FNode2D>();
//   node->AddComponent<RotateComponent>(/*speed_rps=*/1.0f);
//   root.AddChild(Move(node));
//
// 設計選択 (Phase 7 = Pillar B Phase 2):
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

// 型 T 一意 ID = `template static int` のアドレス (T ごとに別 instantiation = 別アドレス)。
template<typename T>
const void* ComponentKindOf() noexcept {
    static const int s_tag = 0;
    return static_cast<const void*>(&s_tag);
}

class FComponent2D {
public:
    FComponent2D() noexcept = default;
    virtual ~FComponent2D() noexcept = default;

    FComponent2D(const FComponent2D&)            = delete;
    FComponent2D& operator=(const FComponent2D&) = delete;
    FComponent2D(FComponent2D&&)                 = delete;
    FComponent2D& operator=(FComponent2D&&)      = delete;

    // 派生クラスは ACS_GAME_COMPONENT_KIND(MyComp) で実装。
    virtual const void* Kind() const noexcept = 0;

    // Lifecycle (override only what you need)
    virtual void OnAttach(FNode2D& /*owner*/) noexcept {}
    virtual void OnUpdate(f32 /*dt*/)        noexcept {}
    // 固定刻み update。FGame の fixed-step accumulator から Scene 経由で呼ばれ、
    // 同フレームで複数回 (catch-up) または 0 回 (slow-down clamp) 呼ばれ得る。
    // 物理シミュレーションや決定論的ロジックはここに置く。default は no-op。
    virtual void OnFixedUpdate(f32 /*fixed_dt*/) noexcept {}
    virtual void OnDraw(RenderContext& /*rc*/) noexcept {}
    virtual void OnDetach()                  noexcept {}

    FNode2D& Owner() noexcept { return *_owner; }
    const FNode2D& Owner() const noexcept { return *_owner; }
    bool HasOwner() const noexcept { return _owner != nullptr; }

    // FNode2D::AddComponent から呼ぶ (internal)
    void _SetOwner(FNode2D* o) noexcept { _owner = o; }

private:
    FNode2D* _owner = nullptr;
};

// 派生クラスで `Kind()` を 1 行で override するための macro。
#define ACS_GAME_COMPONENT_KIND(T)                                                  \
    const void* Kind() const noexcept override                                       \
    { return ::acs::game::ComponentKindOf<T>(); }

} // namespace acs::game

// SPDX-License-Identifier: Apache-2.0
// HelloRaycast3D — レイキャスト対象オブジェクト群と「現在ヒット中」状態。
//
// なぜ独立クラス化したか:
//   ・配置ロジック (円周状) と選択状態 (hit_index / hit_point) はシーンの
//     見た目とは独立した概念なので、描画と分離するとテストしやすい。
//   ・RayCaster は const RaycastTargets& を取れば良くなり、依存方向が単純化する。
#pragma once

#include "Types.h"

#include "math/Vec.h"

namespace helloraycast3d {

class RaycastTargets {
public:
    // 円周上に kNumObjects 個を配置し、ShapeKind と虹色を割り当てる。
    void Init() noexcept;

    // ヒット情報 (RayCaster から呼ばれる)
    void SetHit(acs::i32 index, acs::Vec3 point) noexcept;
    void ClearHit() noexcept;

    // 読み取り専用アクセス
    acs::u32        Count()           const noexcept { return kNumObjects; }
    const Object&   At(acs::u32 i)    const noexcept { return _objects[i]; }
    acs::i32        HitIndex()        const noexcept { return _hit_index; }
    acs::Vec3       HitPoint()        const noexcept { return _hit_point; }
    bool            HasHit()          const noexcept { return _hit_index >= 0; }

private:
    // HSV→RGB (0..1)。色相を順に振って虹色配置するため。
    static acs::Vec3 _hsv_to_rgb(acs::f32 h, acs::f32 s, acs::f32 v) noexcept;

    Object    _objects[kNumObjects]{};
    acs::i32  _hit_index = -1;
    acs::Vec3 _hit_point{};
};

} // namespace helloraycast3d

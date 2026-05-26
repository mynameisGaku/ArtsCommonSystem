// SPDX-License-Identifier: Apache-2.0
// システム関数の登録と実行（FWorld に対して毎フレーム呼ばれる関数の登録機構）
//
// 使い方:
//   void MovementSystem(FWorld& w, f32 dt) {
//       w.Query<Position, Velocity>().Each([dt](FEntityId, Position& p, Velocity& v) {
//           p.x += v.x * dt;
//       });
//   }
//
//   FSystemScheduler s;
//   s.Add(&MovementSystem);
//   while (running) { s.Tick(world, dt); }
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs {

class FWorld;

// システム関数のシグネチャ
using SystemFn = void (*)(FWorld& world, f32 dt);

class FSystemScheduler {
public:
    FSystemScheduler() noexcept = default;

    // システム関数を登録（順序は登録順）
    void Add(SystemFn fn) noexcept { _systems.PushBack(fn); }

    // 全システムを順に呼ぶ（dt は前フレームからの経過秒）
    void Tick(FWorld& world, f32 dt) noexcept {
        for (usize i = 0; i < _systems.Size(); ++i) {
            _systems[i](world, dt);
        }
    }

    void Clear() noexcept { _systems.Clear(); }
    usize Count() const noexcept { return _systems.Size(); }

private:
    TArray<SystemFn> _systems;
};

} // namespace acs

// システム関数の登録と実行（World に対して毎フレーム呼ばれる関数の登録機構）
//
// 使い方:
//   void MovementSystem(World& w, f32 dt) {
//       w.Query<Position, Velocity>().Each([dt](EntityId, Position& p, Velocity& v) {
//           p.x += v.x * dt;
//       });
//   }
//
//   SystemScheduler s;
//   s.Add(&MovementSystem);
//   while (running) { s.Tick(world, dt); }
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs {

class World;

// システム関数のシグネチャ
using SystemFn = void (*)(World& world, f32 dt);

class SystemScheduler {
public:
    SystemScheduler() noexcept = default;

    // システム関数を登録（順序は登録順）
    void Add(SystemFn fn) noexcept { _systems.PushBack(fn); }

    // 全システムを順に呼ぶ（dt は前フレームからの経過秒）
    void Tick(World& world, f32 dt) noexcept {
        for (usize i = 0; i < _systems.Size(); ++i) {
            _systems[i](world, dt);
        }
    }

    void Clear() noexcept { _systems.Clear(); }
    usize Count() const noexcept { return _systems.Size(); }

private:
    Array<SystemFn> _systems;
};

} // namespace acs

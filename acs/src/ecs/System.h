// SPDX-License-Identifier: Apache-2.0
// システム関数の登録と実行（World に対して毎フレーム呼ばれる関数の登録機構）
//
// 使い方:
//   void MovementSystem(FWorld& w, f32 dt) {
//       w.Query<FPosition, FVelocity>().Each([dt](FEntityId, FPosition& p, FVelocity& v) {
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

/** システム関数のシグネチャ (FWorld と経過秒を受け取る関数ポインタ)。 */
using SystemFn = void (*)(FWorld& world, f32 dt);

/**
 * 登録したシステム関数を毎フレーム登録順に実行するスケジューラ。
 *
 * @details
 * システムは (FWorld&, f32 dt) の自由関数ポインタとして保持する。Tick で登録順に
 * 1 回ずつ呼ぶだけの単純な逐次スケジューラで、依存解決や並列化は行わない。
 */
class FSystemScheduler {
public:
    /** 空のスケジューラを構築する。 */
    FSystemScheduler() noexcept = default;

    /**
     * システム関数を登録する (実行順は登録順)。
     *
     * @param fn 登録するシステム関数。
     */
    void Add(SystemFn fn) noexcept { m_Systems.PushBack(fn); }

    /**
     * 登録した全システムを登録順に 1 回ずつ呼ぶ。
     *
     * @param world 各システムへ渡す FWorld。
     * @param dt 前フレームからの経過秒。
     */
    void Tick(FWorld& world, f32 dt) noexcept {
        for (usize i = 0; i < m_Systems.Size(); ++i) {
            m_Systems[i](world, dt);
        }
    }

    /** 登録済みシステムを全て取り除く。 */
    void Clear() noexcept { m_Systems.Clear(); }

    /**
     * 登録済みシステム数を返す。
     *
     * @return 登録されているシステム関数の数。
     */
    usize Count() const noexcept { return m_Systems.Size(); }

private:
    /** 登録順に保持したシステム関数の配列。 */
    TArray<SystemFn> m_Systems;
};

} // namespace acs

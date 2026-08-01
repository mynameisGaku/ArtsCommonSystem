// SPDX-License-Identifier: Apache-2.0
// GameFramework — FWorldClockSubsystem (World スコープの経過時間・フレーム時計)
//
// World サブシステム。シーン開始からの経過秒・フレーム数・直近 dt を OnTick で自動集計し、
// ワールド内のどこからでも GetSubsystem<FWorldClockSubsystem>() で参照できる共有サービス。
// «時間» に依存するゲームロジック (クールダウン、アニメ位相、経過時間 UI 等) が、個別に
// dt を積むのではなく単一の権威ある時計を引けるようにする。
//
//   auto* clock = GetSubsystem<acs::game::FWorldClockSubsystem>();
//   const f64 t = clock->ElapsedSeconds();
//
// dt は «そのスコープのスケール済み» 値 (World はシーンと同じスケール済み dt)。ポーズ/スロー
// モーションで dt=0 や縮小 dt が渡れば、経過時間もそれに追従する。
#pragma once

#include "foundation/Types.h"
#include "gameframework/Subsystem.h"

namespace acs::game {

/**
 * シーン開始からの経過時間・フレーム数・直近 dt を集計する World サブシステム。
 *
 * @details OnTick(dt) で毎フレーム自動更新する。経過秒は長時間セッションでも精度を保つよう
 * f64 で累積する。dt はスコープのスケール済み値なので、ポーズ (dt=0) やスローモーションにも
 * 自然に追従する。
 */
class FWorldClockSubsystem : public FSubsystem {
public:
    ACS_GAME_SUBSYSTEM_KIND(FWorldClockSubsystem)

    /** スコープ開始時に時計を 0 から始める。 */
    void OnInitialize() noexcept override
    {
        m_ElapsedSeconds = 0.0;
        m_FrameCount = 0u;
        m_LastDeltaSeconds = 0.0f;
    }

    /** 毎フレーム、スケール済み dt を経過時間へ積み、フレーム数を進める。 */
    void OnTick(f32 dt) noexcept override
    {
        m_LastDeltaSeconds = dt;
        m_ElapsedSeconds += static_cast<f64>(dt);
        ++m_FrameCount;
    }

    /**
     * シーン開始からの経過秒を返す (スケール済み dt の累積、f64 精度)。
     *
     * @return 経過秒。
     */
    f64 ElapsedSeconds() const noexcept
    {
        return m_ElapsedSeconds;
    }

    /**
     * OnTick が呼ばれた回数 (= 経過フレーム数) を返す。
     *
     * @return フレーム数。
     */
    u64 FrameCount() const noexcept
    {
        return m_FrameCount;
    }

    /**
     * 直近 OnTick の dt 秒を返す (スケール済み)。
     *
     * @return 直近フレームの経過秒。まだ Tick していなければ 0。
     */
    f32 LastDeltaSeconds() const noexcept
    {
        return m_LastDeltaSeconds;
    }

private:
    /** シーン開始からの累積秒 (f64 で長時間精度)。 */
    f64 m_ElapsedSeconds = 0.0;

    /** OnTick 呼び出し回数。 */
    u64 m_FrameCount = 0u;

    /** 直近 OnTick の dt。 */
    f32 m_LastDeltaSeconds = 0.0f;
};

} // namespace acs::game

namespace acs {

/** GameFramework 内の実装型をトップレベルから参照する正規入口。 */
using game::FWorldClockSubsystem;

} // namespace acs

// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar C — FTween / FTweenManager
//
// 値書き戻し型 FTween: 利用者の f32/FVec2/FVec3 変数のポインタを渡し、
// FTweenManager が毎 Tick で補間して書き込む。コールバック不要 (= ACS の
// std::function 非使用方針と整合)。Easing は関数ポインタで指定。
//
// 使い方:
//   class GameplayScene : public Scene {
//       acs::game::FTweenManager m_Tweens;
//       acs::FVec3 m_Color{0, 0, 0};
//
//       void OnEnter() noexcept override {
//           m_Tweens.Tween(&m_Color,
//                          acs::FVec3{0.05f, 0.20f, 0.10f},
//                          acs::FVec3{0.10f, 0.30f, 0.20f},
//                          /*duration=*/2.0f,
//                          Easing::InOutSine);
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           m_Tweens.Tick(dt);
//           GetGame().SetClearColor(m_Color.x, m_Color.y, m_Color.z);
//       }
//   };
//
// 安全性:
//   ・Handle は (index, generation) で stale 参照を検出。Cancel(h) は
//     完了済 or 別 FTween に再利用された slot を弄らない。
//   ・duration <= 0 を渡すと「即時設定」(target に to を書いて Handle=invalid 返す)。
//   ・target が null なら no-op + invalid handle 返却。
//   ・FTween 完了時は target に正確に `to` を書く (浮動小数誤差を 1 frame 残さない)。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "math/Vec.h"
#include "gameframework/Easing.h"

namespace acs::game {

/**
 * 進行中の FTween を識別する handle (index + generation)。
 *
 * @details generation == 0 を invalid と定義し、stale 参照を検出する。
 */
struct FTweenHandle {
    /** slot のインデックス。 */
    u32  index      = 0xFFFFFFFFu;

    /** generation カウンタ (0 = invalid)。 */
    u32  generation = 0;

    /**
     * 有効な handle かを返す。
     *
     * @return generation != 0 なら true。
     */
    bool IsValid() const noexcept { return generation != 0; }
};

/**
 * f32 / FVec2 / FVec3 変数を毎 Tick 補間して書き戻す値書き戻し型 tween マネージャ。
 *
 * @details
 * 利用者の変数ポインタを渡すと、Tick ごとに from→to の補間値を target に書き込む。
 * コールバックは使わず (std::function 非使用方針と整合)、easing は関数ポインタで指定する。
 * Handle は (index, generation) で stale 参照を検出する。完了時は浮動小数誤差を残さず
 * target に正確に to を書く。
 */
class FTweenManager {
public:
    /** 空の tween マネージャを構築する。 */
    FTweenManager() noexcept = default;

    /** 破棄する。 */
    ~FTweenManager() noexcept = default;

    /** コピー禁止 (進行中 tween の状態を単独所有するため)。 */
    FTweenManager(const FTweenManager&)            = delete;

    /** コピー代入も禁止。 */
    FTweenManager& operator=(const FTweenManager&) = delete;

    /**
     * f32 変数の tween を開始する。
     *
     * @param target 書き戻し先の f32 変数ポインタ (null なら no-op)。
     * @param from 開始値。
     * @param to 終了値。
     * @param duration 補間時間 (秒)。<= 0 なら即時 *target = to。
     * @param ease easing 関数 (null なら Linear 扱い)。
     * @return 進行中 tween の handle。target が null / duration <= 0 なら invalid handle。
     */
    FTweenHandle Tween(f32* target,  f32  from, f32  to, f32 duration,
                       Easing::EasingFn ease = Easing::Linear) noexcept;

    /**
     * FVec2 変数の tween を開始する。
     *
     * @param target 書き戻し先の FVec2 変数ポインタ (null なら no-op)。
     * @param from 開始値。
     * @param to 終了値。
     * @param duration 補間時間 (秒)。<= 0 なら即時 *target = to。
     * @param ease easing 関数 (null なら Linear 扱い)。
     * @return 進行中 tween の handle。target が null / duration <= 0 なら invalid handle。
     */
    FTweenHandle Tween(FVec2* target, FVec2 from, FVec2 to, f32 duration,
                       Easing::EasingFn ease = Easing::Linear) noexcept;

    /**
     * FVec3 変数の tween を開始する。
     *
     * @param target 書き戻し先の FVec3 変数ポインタ (null なら no-op)。
     * @param from 開始値。
     * @param to 終了値。
     * @param duration 補間時間 (秒)。<= 0 なら即時 *target = to。
     * @param ease easing 関数 (null なら Linear 扱い)。
     * @return 進行中 tween の handle。target が null / duration <= 0 なら invalid handle。
     */
    FTweenHandle Tween(FVec3* target, FVec3 from, FVec3 to, f32 duration,
                       Easing::EasingFn ease = Easing::Linear) noexcept;

    /**
     * 進行中の tween を中止する (target は最後に書いた値で止まる)。
     *
     * @param h 中止する tween の handle (stale handle は無視)。
     */
    void Cancel(FTweenHandle h) noexcept;

    /**
     * 全 tween を即座に完了させる (target に完了値 to を書く)。
     *
     * @details Scene::OnExit 等で確実に状態を確定させたいときに使う。
     */
    void CompleteAll() noexcept;

    /** 全 tween を即座に破棄する (target に最終書き込みなし)。 */
    void CancelAll() noexcept;

    /**
     * handle の tween が進行中かを返す。
     *
     * @param h 確認する tween の handle。
     * @return active かつ generation 一致なら true。
     */
    bool IsActive(FTweenHandle h) const noexcept;

    /**
     * 進行中の tween 数を返す。
     *
     * @return アクティブな tween 数。
     */
    u32  ActiveCount() const noexcept;

    /**
     * 毎フレーム呼んで全 tween を進める。
     *
     * @param dt ゲーム時間の経過秒 (Clock::Dt() か Scene::OnUpdate の dt)。
     */
    void Tick(f32 dt) noexcept;

private:
    /** slot が補間する値の型種別。 */
    enum class Kind : u8 { None = 0, F32, FVec2, FVec3 };

    /**
     * 1 tween 分の内部 slot。
     *
     * @details from/to は型ごとに別フィールドを持ち、active な kind のみが意味を持つ。
     */
    struct Slot {
        /** 補間する値の型種別。 */
        Kind kind        = Kind::None;

        /** slot 使用中かのフラグ。 */
        bool active      = false;

        /** generation カウンタ (0 = invalid)。 */
        u32  generation  = 0;

        /** 書き戻し先の変数ポインタ。 */
        void* target     = nullptr;

        /** f32 tween の開始値。 */
        f32  from_f      = 0.0f;

        /** f32 tween の終了値。 */
        f32  to_f        = 0.0f;

        /** FVec2 tween の開始値。 */
        FVec2 from_v2     {};

        /** FVec2 tween の終了値。 */
        FVec2 to_v2       {};

        /** FVec3 tween の開始値。 */
        FVec3 from_v3     {};

        /** FVec3 tween の終了値。 */
        FVec3 to_v3       {};

        /** 経過時間 (秒)。 */
        f32  elapsed     = 0.0f;

        /** 補間時間 (秒)。 */
        f32  duration    = 1.0f;

        /** easing 関数。 */
        Easing::EasingFn ease = Easing::Linear;
    };

    /**
     * 空き slot を確保して index を返す (inactive slot を再利用、無ければ末尾追加)。
     *
     * @return 確保した slot のインデックス。
     */
    u32  AcquireSlot() noexcept;

    /**
     * slot の共通フィールド (target / duration / ease / active / generation) を初期化する。
     *
     * @param s 初期化対象の slot。
     * @param target 書き戻し先の変数ポインタ。
     * @param duration 補間時間 (秒)。
     * @param ease easing 関数 (null なら Linear に補正)。
     */
    void FillCommon(Slot& s, void* target, f32 duration,
                    Easing::EasingFn ease) noexcept;

    /** tween slot 列。 */
    TArray<Slot> m_Slots;

    /** アクティブな tween 数。 */
    u32         m_ActiveCount = 0;
};

} // namespace acs::game

// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — CSceneServices
//
// シーンが必要なサービス (CSceneClock / CTweenManager / CSequenceRunner / FInputMap)
// を bit flag (`ESvc`) で宣言、CSceneServices が遅延 alloc して保持する取り付けハブ。
// CGame/CSceneManager が自動で tick + scene 切替に追従。
//
// 使い方:
//   class FGameplayScene : public AScene {
//   public:
//       ESvc WantedServices() const noexcept override {
//           return ESvc::Default2D;  // Clock | Tweens | Sequences | Input
//       }
//       void OnEnter() noexcept override {
//           Services().Input().BindKey(FActionId("Jump"), EKey::Space);
//           Services().Tweens().Tween(&m_Color, c1, c2, 2.0f, Easing::InOutSine);
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           // dt は Clock 経由 scaled (services 有効時)。Tweens/Sequences は
//           // この OnUpdate の **後** に自動 tick されるので、ここで新規スケジュール
//           // した tween は次フレームから進行する。
//           if (Services().Input().IsPressed(FActionId("Jump"))) DoJump();
//       }
//   };
//
// 設計選択:
//   ・**bit flag 宣言**: `WantedServices()` で宣言したサービスだけ alloc。
//     使わないシーン (例: メニュー) は Physics/Tweens のコストを払わない。
//   ・**遅延 alloc**: constructor 内で wanted bit を見て TUniquePtr<T> を作る。
//     未要求のサービスは TUniquePtr が null、accessor 呼出は assert で検出。
//   ・**2 phase tick**: PreUpdate (Clock 進行) → scene.OnUpdate → PostUpdate
//     (Tweens/Sequences tick)。新規スケジュールは次フレーム頭から進行 (predictable)。
//   ・**自動 pause**: シーンが下位に追いやられた間は OnUpdate が呼ばれず、
//     Clock も tick されないので tween/seq は自然に止まる。明示的 Pause 不要。
//
// 範囲外 (本クラスでは持たない):
//   ・Audio / Events / Debug / Timers / Ui の各サービス
//     (該当 Pillar 実装時に ESvc enum と CSceneServices に追加)。
#pragma once

#include "foundation/Types.h"
#include "gameframework/Forward.h"
#include "memory/UniquePtr.h"
#include "gameframework/Clock.h"
#include "gameframework/Tween.h"
#include "gameframework/Sequence.h"
#include "gameframework/InputMap.h"
#include "gameframework/Camera2D.h"
#include "gameframework/CollisionWorld2D.h"
#include "gameframework/TriggerWorld2D.h"

namespace acs::game {

/**
 * シーンが要求するサービスを宣言する bit flag。
 *
 * @details WantedServices() でこのマスクを返すと、CSceneServices が該当サービスだけを遅延 alloc する。
 */
enum class ESvc : u32 {
    /** サービスを一切要求しない。 */
    None       = 0,

    /** CSceneClock (時間スケール付き dt)。 */
    Clock      = 1u << 0,

    /** CTweenManager (補間アニメーション)。 */
    Tweens     = 1u << 1,

    /** CSequenceRunner (時系列スクリプト)。 */
    Sequences  = 1u << 2,

    /** FInputMap (アクションへの入力束ね)。 */
    Input      = 1u << 3,

    /** CCamera2D (2D カメラ)。 */
    Camera2D    = 1u << 4,

    /** CCollisionWorld2D (2D 衝突)。 */
    Physics2D  = 1u << 5,

    /** CTriggerWorld2D (overlap enter/stay/exit イベント)。 */
    Triggers   = 1u << 6,

    /** 2D ゲームの既定セット (Clock | Tweens | Sequences | Input)。 */
    Default2D  = Clock | Tweens | Sequences | Input,
};

/**
 * 2 つの ESvc マスクの bit OR を返す。
 *
 * @param a 左辺のマスク。
 * @param b 右辺のマスク。
 * @return a と b の bit OR を取ったマスク。
 */
constexpr ESvc operator|(ESvc a, ESvc b) noexcept {
    return static_cast<ESvc>(static_cast<u32>(a) | static_cast<u32>(b));
}

/**
 * 2 つの ESvc マスクの bit AND を返す。
 *
 * @param a 左辺のマスク。
 * @param b 右辺のマスク。
 * @return a と b の bit AND を取ったマスク。
 */
constexpr ESvc operator&(ESvc a, ESvc b) noexcept {
    return static_cast<ESvc>(static_cast<u32>(a) & static_cast<u32>(b));
}

/**
 * マスクが指定フラグを含むかを返す。
 *
 * @param mask 検査対象のマスク。
 * @param flag 含まれるか調べるフラグ。
 * @return mask に flag の bit が立っていれば true。
 */
constexpr bool SvcHas(ESvc mask, ESvc flag) noexcept {
    return (static_cast<u32>(mask) & static_cast<u32>(flag)) != 0u;
}

/**
 * シーンのサービス (Clock/Tweens/Sequences/Input/Camera/Physics/Triggers) を遅延 alloc して保持するハブ。
 *
 * @details
 * constructor で wanted bit を見て該当サービスだけ TUniquePtr<T> を作る。未要求のサービスは null
 * のままで、accessor 呼出は ACS_CHECK で検出する (Release でも停止)。tick は 2 phase 構成で、PreUpdate (Clock 進行)
 * → scene.OnUpdate → PostUpdate (Tweens/Sequences/Camera/Triggers tick) の順に駆動される。
 * CGame/CSceneManager がフレームごとに自動で tick + scene 切替に追従する。
 */
class CSceneServices {
public:
    /**
     * wanted bit を見て該当サービスを alloc する。
     *
     * @details 未要求のサービスは null のまま。Physics2D / Triggers は alloc 後に Init も呼ぶ。
     * @param wanted 要求するサービスの bit mask。
     */
    explicit CSceneServices(ESvc wanted) noexcept;

    /** サービスを破棄する (各サービスは TUniquePtr が解放)。 */
    ~CSceneServices() noexcept = default;

    /** コピー禁止 (サービスを単独所有するため)。 */
    CSceneServices(const CSceneServices&)            = delete;

    /** コピー代入も禁止。 */
    CSceneServices& operator=(const CSceneServices&) = delete;

    /**
     * 構築時に要求されたサービスマスクを返す。
     *
     * @return constructor に渡された wanted マスク。
     */
    ESvc  Wanted() const noexcept { return m_Wanted; }

    /**
     * 指定サービスが要求されているかを返す。
     *
     * @param s 調べるサービスフラグ。
     * @return wanted マスクに s が含まれていれば true。
     */
    bool Has(ESvc s) const noexcept { return SvcHas(m_Wanted, s); }

    /**
     * CSceneClock への参照を返す。
     *
     * @details ESvc::Clock が要求されていなければ ACS_CHECK で停止する (Release でも nullptr 参照せず停止)。
     * @return クロックサービスへの参照。
     */
    CSceneClock&          Clock()     noexcept;

    /**
     * CTweenManager への参照を返す。
     *
     * @details ESvc::Tweens が要求されていなければ ACS_CHECK で停止する (Release でも nullptr 参照せず停止)。
     * @return tween サービスへの参照。
     */
    CTweenManager&        Tweens()    noexcept;

    /**
     * CSequenceRunner への参照を返す。
     *
     * @details ESvc::Sequences が要求されていなければ ACS_CHECK で停止する (Release でも nullptr 参照せず停止)。
     * @return sequence サービスへの参照。
     */
    CSequenceRunner&      Sequences() noexcept;

    /**
     * FInputMap への参照を返す。
     *
     * @details ESvc::Input が要求されていなければ ACS_CHECK で停止する (Release でも nullptr 参照せず停止)。
     * @return 入力サービスへの参照。
     */
    FInputMap&            Input()     noexcept;

    /**
     * CCamera2D への参照を返す。
     *
     * @details ESvc::Camera2D が要求されていなければ ACS_CHECK で停止する (Release でも nullptr 参照せず停止)。
     * @return カメラサービスへの参照。
     */
    acs::game::CCamera2D& Camera()    noexcept;

    /**
     * CCollisionWorld2D への参照を返す。
     *
     * @details ESvc::Physics2D が要求されていなければ ACS_CHECK で停止する (Release でも nullptr 参照せず停止)。
     * @return 物理サービスへの参照。
     */
    CCollisionWorld2D&    Physics()   noexcept;

    /**
     * CTriggerWorld2D への参照を返す。
     *
     * @details ESvc::Triggers が要求されていなければ ACS_CHECK で停止する (Release でも nullptr 参照せず停止)。
     * @return トリガサービスへの参照。
     */
    CTriggerWorld2D&      Triggers()  noexcept;

    /**
     * scene.OnUpdate の前に呼ぶ前段 tick (利用者は触らない)。
     *
     * @details Clock を Tick(raw_dt) して時間を進め、scaled dt を確定する。
     * @param raw_dt スケール前の生 dt。
     */
    void _PreUpdate(f32 raw_dt) noexcept;

    /**
     * scene.OnUpdate の後に呼ぶ後段 tick (利用者は触らない)。
     *
     * @details Tweens/Sequences/Camera/Triggers を scaled_dt で Tick して更新を適用する。
     * @param scaled_dt Clock で確定したスケール済み dt。
     */
    void _PostUpdate(f32 scaled_dt) noexcept;

    /**
     * scene の OnUpdate に渡す dt を返す。
     *
     * @details _PreUpdate 後に呼ぶ。Clock 未要求なら raw_dt をそのまま返す。
     * @param raw_dt スケール前の生 dt。
     * @return Clock がスケールした dt (未要求なら raw_dt)。
     */
    f32  _ScaledDt(f32 raw_dt) const noexcept;

private:
    friend class CSceneManager;

    /** 要求された全サービスが生成済みかを返す。 */
    bool IsReady() const noexcept;

    /** 構築時に要求されたサービスマスク。 */
    ESvc                       m_Wanted = ESvc::None;

    /** クロックサービス (未要求なら null)。 */
    TUniquePtr<CSceneClock>     m_Clock;

    /** tween サービス (未要求なら null)。 */
    TUniquePtr<CTweenManager>   m_Tweens;

    /** sequence サービス (未要求なら null)。 */
    TUniquePtr<CSequenceRunner> m_Sequences;

    /** 入力サービス (未要求なら null)。 */
    TUniquePtr<FInputMap>       m_Input;

    /** カメラサービス (未要求なら null)。 */
    TUniquePtr<acs::game::CCamera2D> m_Camera;

    /** 物理サービス (未要求なら null)。 */
    TUniquePtr<CCollisionWorld2D>    m_Physics;

    /** トリガサービス (未要求なら null)。 */
    TUniquePtr<CTriggerWorld2D>      m_Triggers;
};

} // namespace acs::game

namespace acs {

/** Scene が要求する service 集合のビット列挙をトップレベルへ公開する。 */
using game::ESvc;

} // namespace acs

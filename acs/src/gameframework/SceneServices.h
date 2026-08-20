// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — FSceneServices
//
// シーンが必要なサービス (FSceneClock / FTweenManager / FSequenceRunner / FInputMap)
// を bit flag (`ESvc`) で宣言、FSceneServices が遅延 alloc して保持する取り付けハブ。
// FGame/FSceneManager が自動で tick + scene 切替に追従。
//
// 使い方:
//   class FGameplayScene : public FScene {
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
//     (該当 Pillar 実装時に ESvc enum と FSceneServices に追加)。
#pragma once

#include "foundation/Types.h"
#include "memory/UniquePtr.h"
#include "gameframework/Clock.h"
#include "gameframework/Tween.h"
#include "gameframework/Sequence.h"
#include "gameframework/InputMap.h"
#include "gameframework/FixedStepInputBuffer.h"
#include "gameframework/Camera2D.h"
#include "gameframework/CollisionWorld2D.h"
#include "gameframework/TriggerWorld2D.h"

namespace acs::game {

/**
 * シーンが要求するサービスを宣言する bit flag。
 *
 * @details WantedServices() でこのマスクを返すと、FSceneServices が該当サービスだけを遅延 alloc する。
 */
enum class ESvc : u32 {
    /** サービスを一切要求しない。 */
    None = 0,

    /** FSceneClock (時間スケール付き dt)。 */
    Clock = 1u << 0,

    /** FTweenManager (補間アニメーション)。 */
    Tweens = 1u << 1,

    /** FSequenceRunner (時系列スクリプト)。 */
    Sequences = 1u << 2,

    /** FInputMap (アクションへの入力束ね)。 */
    Input = 1u << 3,

    /** FCamera2D (2D カメラ)。 */
    Camera2D = 1u << 4,

    /** FCollisionWorld2D (2D 衝突)。 */
    Physics2D = 1u << 5,

    /** FTriggerWorld2D (overlap enter/stay/exit イベント)。 */
    Triggers = 1u << 6,

    /** 2D ゲームの既定セット (Clock | Tweens | Sequences | Input)。 */
    Default2D = Clock | Tweens | Sequences | Input,
};

/**
 * 2 つの ESvc マスクの bit OR を返す。
 *
 * @param a 左辺のマスク。
 * @param b 右辺のマスク。
 * @return a と b の bit OR を取ったマスク。
 */
constexpr ESvc operator|(ESvc a, ESvc b) noexcept
{
    return static_cast<ESvc>(static_cast<u32>(a) | static_cast<u32>(b));
}

/**
 * 2 つの ESvc マスクの bit AND を返す。
 *
 * @param a 左辺のマスク。
 * @param b 右辺のマスク。
 * @return a と b の bit AND を取ったマスク。
 */
constexpr ESvc operator&(ESvc a, ESvc b) noexcept
{
    return static_cast<ESvc>(static_cast<u32>(a) & static_cast<u32>(b));
}

/**
 * マスクが指定フラグを含むかを返す。
 *
 * @param mask 検査対象のマスク。
 * @param flag 含まれるか調べるフラグ。
 * @return mask に flag の bit が立っていれば true。
 */
constexpr bool SvcHas(ESvc mask, ESvc flag) noexcept
{
    return (static_cast<u32>(mask) & static_cast<u32>(flag)) != 0u;
}

/**
 * シーンのサービス (Clock/Tweens/Sequences/Input/Camera/Physics/Triggers) を遅延 alloc して保持するハブ。
 *
 * @details
 * constructor で wanted bit を見て該当サービスだけ TUniquePtr<T> を作る。未要求のサービスは null
 * のままで、accessor 呼出は ACS_CHECK で検出する (Release でも停止)。tick は 2 phase 構成で、PreUpdate (Clock 進行)
 * → scene.OnUpdate → PostUpdate (Tweens/Sequences/Camera/Triggers tick) の順に駆動される。
 * FGame/FSceneManager がフレームごとに自動で tick + scene 切替に追従する。
 */
class FSceneServices {
public:
    /**
     * wanted bit を見て該当サービスを alloc する。
     *
     * @details 未要求のサービスは null のまま。Physics2D / Triggers は alloc 後に Init も呼ぶ。
     * @param wanted 要求するサービスの bit mask。
     */
    explicit FSceneServices(ESvc wanted) noexcept;

    /** サービスを破棄する (各サービスは TUniquePtr が解放)。 */
    ~FSceneServices() noexcept = default;

    /** コピー禁止 (サービスを単独所有するため)。 */
    FSceneServices(const FSceneServices&) = delete;

    /** コピー代入も禁止。 */
    FSceneServices& operator=(const FSceneServices&) = delete;

    /**
     * 構築時に要求されたサービスマスクを返す。
     *
     * @return constructor に渡された wanted マスク。
     */
    ESvc Wanted() const noexcept
    {
        return m_Wanted;
    }

    /**
     * 指定サービスが要求されているかを返す。
     *
     * @param s 調べるサービスフラグ。
     * @return wanted マスクに s が含まれていれば true。
     */
    bool Has(ESvc s) const noexcept
    {
        return SvcHas(m_Wanted, s);
    }

    /**
     * FSceneClock への参照を返す。
     *
     * @details ESvc::Clock が要求されていなければ ACS_CHECK で停止する (Release でも nullptr 参照せず停止)。
     * @return クロックサービスへの参照。
     */
    FSceneClock& Clock() noexcept;

    /**
     * FTweenManager への参照を返す。
     *
     * @details ESvc::Tweens が要求されていなければ ACS_CHECK で停止する (Release でも nullptr 参照せず停止)。
     * @return tween サービスへの参照。
     */
    FTweenManager& Tweens() noexcept;

    /**
     * FSequenceRunner への参照を返す。
     *
     * @details ESvc::Sequences が要求されていなければ ACS_CHECK で停止する (Release でも nullptr 参照せず停止)。
     * @return sequence サービスへの参照。
     */
    FSequenceRunner& Sequences() noexcept;

    /**
     * FInputMap への参照を返す。
     *
     * @details ESvc::Input が要求されていなければ ACS_CHECK で停止する (Release でも nullptr 参照せず停止)。
     * @return 入力サービスへの参照。
     */
    FInputMap& Input() noexcept;

    /**
     * 現在の固定 tick へ割り当てられた入力状態を返す。
     * @return FInputMap::Evaluate へ渡せる読み取り専用入力状態。
     */
    const IInputStateView& FixedInput() const noexcept;

    /**
     * FCamera2D への参照を返す。
     *
     * @details ESvc::Camera2D が要求されていなければ ACS_CHECK で停止する (Release でも nullptr 参照せず停止)。
     * @return カメラサービスへの参照。
     */
    acs::game::FCamera2D& Camera() noexcept;

    /**
     * FCollisionWorld2D への参照を返す。
     *
     * @details ESvc::Physics2D が要求されていなければ ACS_CHECK で停止する (Release でも nullptr 参照せず停止)。
     * @return 物理サービスへの参照。
     */
    FCollisionWorld2D& Physics() noexcept;

    /**
     * FTriggerWorld2D への参照を返す。
     *
     * @details ESvc::Triggers が要求されていなければ ACS_CHECK で停止する (Release でも nullptr 参照せず停止)。
     * @return トリガサービスへの参照。
     */
    FTriggerWorld2D& Triggers() noexcept;

private:
    /** 可変フレーム入力を固定 tick 入力へ蓄積する内部処理。 */
    bool SubmitFrameInput_Internal(const IInputStateView& input) noexcept;

    /** 次の固定 tick 入力を確定し、押下・解放を一度だけ消費する内部処理。 */
    void BeginFixedStepInput_Internal() noexcept;

    /** シーン遷移、pause、時計設定変更で固定入力を初期化する内部処理。 */
    void ResetFixedInput_Internal() noexcept;

    /** 未消費の固定入力を保存値へ複製する内部処理。 */
    bool TryCaptureFixedInputSnapshot_Internal(FFixedStepInputBufferSnapshot& snapshot) const noexcept;

    /** 保存値から未消費の固定入力を復元する内部処理。 */
    bool TryRestoreFixedInputSnapshot_Internal(const FFixedStepInputBufferSnapshot& snapshot) noexcept;

    /** Clock を進めてスケール済み時間を確定する内部処理。 */
    void PreUpdate_Internal(f32 raw_dt) noexcept;

    /** Tween、Sequence、Camera、Trigger を進める内部処理。 */
    void PostUpdate_Internal(f32 scaled_dt) noexcept;

    /** シーンへ渡すスケール済み時間を返す内部処理。 */
    f32 ScaledDt_Internal(f32 raw_dt) const noexcept;

public:
    /** サービス更新処理を明示的に呼び出す非所有アダプター。 */
    class FUpdateAdapter final {
    public:
        /** 呼び出し先のサービス束を保持する。 */
        explicit FUpdateAdapter(FSceneServices& services) noexcept : m_Services(services)
        {
        }

        /** Clock を進めてスケール済み時間を確定する。 */
        void PreUpdate(f32 raw_delta_seconds) noexcept
        {
            m_Services.PreUpdate_Internal(raw_delta_seconds);
        }

        /** 一フレーム分の入力を固定 tick 用に蓄積する。 */
        bool SubmitFrameInput(const IInputStateView& input) noexcept
        {
            return m_Services.SubmitFrameInput_Internal(input);
        }

        /** 次の固定 tick へ渡す入力を確定する。 */
        void BeginFixedStepInput() noexcept
        {
            m_Services.BeginFixedStepInput_Internal();
        }

        /** 未消費の固定入力と現在 tick 入力を初期化する。 */
        void ResetFixedInput() noexcept
        {
            m_Services.ResetFixedInput_Internal();
        }

        /** 未消費の固定入力を保存値へ複製する。 */
        bool TryCaptureFixedInputSnapshot(FFixedStepInputBufferSnapshot& snapshot) const noexcept
        {
            return m_Services.TryCaptureFixedInputSnapshot_Internal(snapshot);
        }

        /** 保存値から未消費の固定入力を復元する。 */
        bool TryRestoreFixedInputSnapshot(const FFixedStepInputBufferSnapshot& snapshot) noexcept
        {
            return m_Services.TryRestoreFixedInputSnapshot_Internal(snapshot);
        }

        /** シーン更新後のサービスを進める。 */
        void PostUpdate(f32 scaled_delta_seconds) noexcept
        {
            m_Services.PostUpdate_Internal(scaled_delta_seconds);
        }

        /** シーンへ渡すスケール済み時間を返す。 */
        f32 ScaledDt(f32 raw_delta_seconds) const noexcept
        {
            return m_Services.ScaledDt_Internal(raw_delta_seconds);
        }

    private:
        /** 呼び出し先のサービス束。 */
        FSceneServices& m_Services;
    };

    /** シーン更新用アダプターを返す。 */
    FUpdateAdapter UpdateAccess() noexcept
    {
        return FUpdateAdapter(*this);
    }

private:
    /** InputMap と固定 tick 入力を一つの所有スロットへまとめる値。 */
    struct FInputServiceState {
        /** 物理入力を名前付きアクションへ変換する割り当て。 */
        FInputMap input_map;

        /** 可変フレーム入力の未消費状態。 */
        FFixedStepInputBuffer fixed_input_buffer;

        /** 現在の固定 tick へ公開する入力状態。 */
        FInputStateSnapshot fixed_input;
    };

    /** 構築時に要求されたサービスマスク。 */
    ESvc m_Wanted = ESvc::None;

    /** クロックサービス (未要求なら null)。 */
    TUniquePtr<FSceneClock> m_Clock;

    /** tween サービス (未要求なら null)。 */
    TUniquePtr<FTweenManager> m_Tweens;

    /** sequence サービス (未要求なら null)。 */
    TUniquePtr<FSequenceRunner> m_Sequences;

    /** 入力割り当てと固定 tick 入力の状態 (未要求なら null)。 */
    TUniquePtr<FInputServiceState> m_Input;

    /** カメラサービス (未要求なら null)。 */
    TUniquePtr<acs::game::FCamera2D> m_Camera;

    /** 物理サービス (未要求なら null)。 */
    TUniquePtr<FCollisionWorld2D> m_Physics;

    /** トリガサービス (未要求なら null)。 */
    TUniquePtr<FTriggerWorld2D> m_Triggers;
};

} // namespace acs::game

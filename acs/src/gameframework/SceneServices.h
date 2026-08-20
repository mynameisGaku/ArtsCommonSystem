// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "gameframework/Forward.h"
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
     * 現在の固定tickへ割り当てられた入力状態を返す。
     * @return FInputMap::Evaluateへ渡せる読み取り専用入力状態。
     */
    const IInputStateView& FixedInput() const noexcept;

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

    /** サービス更新処理を明示的に呼び出す非所有アダプター。 */
    class FUpdateAdapter final {
    public:
        /** 呼び出し先のサービス束を保持する。 */
        explicit FUpdateAdapter(CSceneServices& services) noexcept : m_Services(services)
        {
        }

        /** 要求された全サービスが生成済みならtrueを返す。 */
        bool IsReady() const noexcept
        {
            return m_Services.IsReady_Internal();
        }

        /** Clockを進めてスケール済み時間を確定する。 */
        void PreUpdate(f32 raw_delta_seconds) noexcept
        {
            m_Services.PreUpdate_Internal(raw_delta_seconds);
        }

        /** 一フレーム分の入力を固定tick用に蓄積する。 */
        bool SubmitFrameInput(const IInputStateView& input) noexcept
        {
            return m_Services.SubmitFrameInput_Internal(input);
        }

        /** 次の固定tickへ渡す入力を確定する。 */
        void BeginFixedStepInput() noexcept
        {
            m_Services.BeginFixedStepInput_Internal();
        }

        /** 未消費の固定入力と現在tick入力を初期化する。 */
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
        CSceneServices& m_Services;
    };

    /** シーン管理器へ渡すサービス更新用アダプターを返す。 */
    FUpdateAdapter UpdateAccess() noexcept
    {
        return FUpdateAdapter(*this);
    }

private:
    /** 要求された全サービスが生成済みかを返す内部処理。 */
    bool IsReady_Internal() const noexcept;

    /** 可変フレーム入力を固定tick入力へ蓄積する内部処理。 */
    bool SubmitFrameInput_Internal(const IInputStateView& input) noexcept;

    /** 次の固定tick入力を確定し、押下・解放を一度だけ消費する内部処理。 */
    void BeginFixedStepInput_Internal() noexcept;

    /** シーン遷移、pause、時計設定変更で固定入力を初期化する内部処理。 */
    void ResetFixedInput_Internal() noexcept;

    /** 未消費の固定入力を保存値へ複製する内部処理。 */
    bool TryCaptureFixedInputSnapshot_Internal(FFixedStepInputBufferSnapshot& snapshot) const noexcept;

    /** 保存値から未消費の固定入力を復元する内部処理。 */
    bool TryRestoreFixedInputSnapshot_Internal(const FFixedStepInputBufferSnapshot& snapshot) noexcept;

    /** Clockを進めてスケール済み時間を確定する内部処理。 */
    void PreUpdate_Internal(f32 raw_dt) noexcept;

    /** Tween、Sequence、Camera、Triggerを進める内部処理。 */
    void PostUpdate_Internal(f32 scaled_dt) noexcept;

    /** シーンへ渡すスケール済み時間を返す内部処理。 */
    f32 ScaledDt_Internal(f32 raw_dt) const noexcept;

    /** InputMapと固定tick入力を一つの所有スロットへまとめる値。 */
    struct FInputServiceState {
        /** 物理入力を名前付きアクションへ変換する割り当て。 */
        FInputMap input_map;

        /** 可変フレーム入力の未消費状態。 */
        FFixedStepInputBuffer fixed_input_buffer;

        /** 現在の固定tickへ公開する入力状態。 */
        FInputStateSnapshot fixed_input;
    };

    /** 構築時に要求されたサービスマスク。 */
    ESvc                       m_Wanted = ESvc::None;

    /** クロックサービス (未要求なら null)。 */
    TUniquePtr<CSceneClock>     m_Clock;

    /** tween サービス (未要求なら null)。 */
    TUniquePtr<CTweenManager>   m_Tweens;

    /** sequence サービス (未要求なら null)。 */
    TUniquePtr<CSequenceRunner> m_Sequences;

    /** 入力割り当てと固定tick入力の状態 (未要求ならnull)。 */
    TUniquePtr<FInputServiceState> m_Input;

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

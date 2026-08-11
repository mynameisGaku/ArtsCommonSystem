// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "ecs/RollbackBuffer.h"
#include "gameframework/Lockstep.h"   // FInputFrame (入力 POD を共有)

namespace acs::game {

/**
 * CRollbackSession の初期化パラメータ。
 */
struct FRollbackSessionConfig {
    /** プレイヤー数 (1..kMaxRollbackPlayers)。 */
    u32 player_count   = 2;

    /**
     * 状態履歴と入力台帳のリング長 (1 以上)。
     *
     * @details 最古の 1 slot は「次に実行する現在 tick」と共有予約されるため、
     * 実効的に巻き戻せるのは history_length - 1 tick まで。
     */
    u32 history_length = 8;

    /** 未確定のまま先行できる tick 数の上限 (0 = 無制限)。history_length 未満であること。 */
    u32 max_prediction = 0;
};

/** CRollbackSession が扱えるプレイヤー数の上限。 */
inline constexpr u32 kMaxRollbackPlayers = 8;

/**
 * rollback netcode の統合層 (状態履歴 + 入力台帳 + 予測 + 再シミュレーション制御)。
 *
 * @details
 * 毎 tick、AdvanceTick が「(必要なら) 巻き戻し再実行 → 状態 snapshot → 入力収集
 * (確定 or 予測) → sim コールバック → tick 前進」を行う。確定入力は SubmitInput で
 * いつでも (past tick でも) 投入でき、予測と食い違っていた場合のみ次の AdvanceTick
 * 冒頭で自動的に巻き戻して再実行する。non-copy / non-move 型。
 */
class CRollbackSession {
public:
    /**
     * 1 tick 分のシミュレーションを進める決定論コールバック。
     *
     * @param world 進める CWorld。
     * @param tick 現在の tick。
     * @param inputs player_id 昇順に並んだ全プレイヤーの入力 (確定 or 予測)。
     * @param input_count inputs の要素数 (= player_count)。
     * @param user SetSimCallback で渡した user データ。
     */
    using SimTickFn = void (*)(CWorld& world, u32 tick, const FInputFrame* inputs,
                               u32 input_count, void* user);

    /** 未初期化状態で構築する。使用前に Init を呼ぶ。 */
    CRollbackSession() noexcept = default;

    /** 破棄する (CWorld は非所有なので触らない)。 */
    ~CRollbackSession() noexcept = default;

    /** コピー禁止 (CWorld* と履歴を抱える長寿命オブジェクト)。 */
    CRollbackSession(const CRollbackSession&)            = delete;

    /** コピー代入も禁止。 */
    CRollbackSession& operator=(const CRollbackSession&) = delete;

    /** ムーブ禁止。 */
    CRollbackSession(CRollbackSession&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CRollbackSession& operator=(CRollbackSession&&)      = delete;

    /**
     * セッションを初期化する (再 Init 可、既存の履歴は破棄)。
     *
     * @details world の全コンポーネント型はコピー構築可能であること
     * (CWorld::CopyFrom の契約)。確保失敗時は未初期化状態に戻して false。
     * @param world 対象の CWorld (非所有、セッションより長生きすること)。
     * @param config プレイヤー数 / 履歴長 / 予測上限。
     * @return 初期化できたら true。引数不正 (world=nullptr / player_count 範囲外 /
     *         history_length=0 / max_prediction >= history_length) や OOM は false。
     */
    bool Init(CWorld* world, const FRollbackSessionConfig& config) noexcept;

    /**
     * sim コールバックを設定する (AdvanceTick の前に必須)。
     *
     * @param fn 1 tick 進める決定論コールバック (nullptr で解除)。
     * @param user コールバックへ渡す任意の user データ。
     */
    void SetSimCallback(SimTickFn fn, void* user) noexcept
    {
        m_SimFn   = fn;
        m_SimUser = user;
    }

    /**
     * 確定入力を投入する (ローカル / リモート、現在 tick または過去 tick)。
     *
     * @details
     * 過去 tick への投入は、その tick がまだ実効巻き戻し窓 (history_length - 1 tick)
     * に残っている場合のみ受理し、予測と食い違っていれば次の AdvanceTick で自動的に
     * 巻き戻し再実行される (bit 一致なら再実行しない)。窓から追い出された古い tick は
     * false。未来 tick (> CurrentTick) も false — 早着した入力は呼び出し側の
     * トランスポート層でバッファし、tick が追いついてから投入すること (台帳と
     * 状態履歴が同位相リングのため、先行受理は巻き戻し対象の過去 slot を破壊する)。
     * @param frame 確定入力 (tick / player_id を正しく設定すること)。
     * @return 受理したら true。
     */
    bool SubmitInput(const FInputFrame& frame) noexcept;

    /**
     * 1 tick 進める (必要なら冒頭で自動巻き戻し + 再シミュレーション)。
     *
     * @details
     * 失敗時 (未初期化 / コールバック未設定 / 予測上限到達 / snapshot 失敗) は
     * CWorld を進めずに false を返す。予測上限到達は入力が届けば解消する正常系。
     * @return 進めたら true。
     */
    bool AdvanceTick() noexcept;

    /**
     * 次に実行する tick を返す。
     *
     * @return 現在の tick カウンタ。
     */
    u32 CurrentTick() const noexcept { return m_CurrentTick; }

    /**
     * 全プレイヤーの確定入力が揃っている先頭連続 tick (= 巻き戻しがもう起きない床) を返す。
     *
     * @return 確定済み床 tick (CurrentTick と等しければ予測なしで進んでいる)。
     */
    u32 ConfirmedFloor() const noexcept { return m_ConfirmedFloor; }

    /**
     * 未確定のまま先行している tick 数 (= CurrentTick - ConfirmedFloor) を返す。
     *
     * @return 予測深度。
     */
    u32 PredictionDepth() const noexcept { return m_CurrentTick - m_ConfirmedFloor; }

    /**
     * 巻き戻し再実行が保留中かを返す (次の AdvanceTick 冒頭で実行される)。
     *
     * @return 予測と食い違う確定入力を受けていれば true。
     */
    bool NeedsResimulation() const noexcept { return m_DirtyTick != kNoDirtyTick; }

    /**
     * tick カウンタと履歴を start_tick から仕切り直す (CWorld の現在状態は保持)。
     *
     * @param start_tick 新しい開始 tick。
     */
    void Reset(u32 start_tick = 0) noexcept;

    /**
     * 初期化済みかを返す。
     *
     * @return Init 成功後なら true。
     */
    bool IsInitialized() const noexcept { return m_World != nullptr; }

private:
    /** tick 用リング slot を初期化する (再利用時に確定フラグを落とす)。 */
    void EnsureSlot(u32 tick) noexcept;

    /** tick 分の入力 (確定 or 予測) を m_TickInputs / used 台帳へ書き込む。 */
    void GatherInputs(u32 tick) noexcept;

    /** 確定済み床 tick を前進させる。 */
    void AdvanceConfirmedFloor() noexcept;

    /** 保留中の巻き戻し再実行を行う。 */
    bool ResimulateIfNeeded() noexcept;

    /** dirty 無しを表す番兵。 */
    static constexpr u32 kNoDirtyTick     = 0xFFFFFFFFu;

    /** 未使用 slot を表す番兵 (tick は 0xFFFFFFFF 近辺まで使わない想定)。 */
    static constexpr u32 kInvalidSlotTick = 0xFFFFFFFFu;

    /** 対象 CWorld (非所有)。IsInitialized の判定にも使う。 */
    CWorld*             m_World          = nullptr;

    /** 状態履歴 (tick 開始時点の CWorld スナップショット)。 */
    FRollbackBuffer     m_History;

    /** sim コールバック。 */
    SimTickFn          m_SimFn          = nullptr;

    /** sim コールバックへ渡す user データ。 */
    void*              m_SimUser        = nullptr;

    /** プレイヤー数。 */
    u32               m_PlayerCount   = 0;

    /** 履歴リング長。 */
    u32               m_HistoryLength = 0;

    /** 予測上限 (0 = 無制限)。 */
    u32               m_MaxPrediction = 0;

    /** 次に実行する tick。 */
    u32               m_CurrentTick   = 0;

    /** 全員確定済みの先頭連続 tick。 */
    u32               m_ConfirmedFloor = 0;

    /** 巻き戻し起点 (kNoDirtyTick = 保留なし)。 */
    u32               m_DirtyTick     = kNoDirtyTick;

    /** slot が現在どの tick を表しているか (リング再利用の検出)。長さ = history。 */
    TArray<u32>        m_SlotTick;

    /** 確定フラグ (slot * player)。 */
    TArray<u8>         m_Confirmed;

    /** 確定入力の台帳 (slot * player)。 */
    TArray<FInputFrame> m_Ledger;

    /** 実際に sim へ渡した入力 (slot * player、予測一致判定と繰り返し予測に使う)。 */
    TArray<FInputFrame> m_Used;

    /** GatherInputs が組み立てる 1 tick 分の入力 (player_id 昇順)。 */
    TArray<FInputFrame> m_TickInputs;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FRollbackSession = CRollbackSession;

} // namespace acs::game

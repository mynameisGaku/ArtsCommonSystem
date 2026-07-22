// SPDX-License-Identifier: Apache-2.0
// GameFramework — FRollbackSession (rollback netcode の統合層)
//
// 役割:
//   ecs::FRollbackBuffer (状態履歴) と自前の入力台帳を束ね、GGPO 風の
//   「予測実行 → 遅延して届いた確定入力 → 自動巻き戻し + 再シミュレーション」
//   ループを 1 クラスで回せるようにする。レイヤ関係:
//     ・FWorld::CopyFrom     … 状態の複製プリミティブ (ecs)
//     ・FRollbackBuffer      … 直近 N tick の状態履歴リング (ecs)
//     ・FRollbackSession     … 入力台帳 + 予測 + 再シミュレーション制御 (本クラス)
//   FLockstep は「全入力確定済み」前提の入力リプレイ層で、本クラスとは別物
//   (決定論検証の ComputeChecksum は併用できる)。
//
// 使い方 (2P 対戦の典型):
//   FRollbackSession session;
//   FRollbackSessionConfig cfg;
//   cfg.player_count   = 2;
//   cfg.history_length = 8;      // 8 tick まで巻き戻せる
//   session.Init(&world, cfg);
//   session.SetSimCallback(&FMyGame::SimTick, this);
//
//   // 毎 tick:
//   session.SubmitInput(local_input);            // 自分の入力 (確定)
//   while (RemoteInputAvailable())
//       session.SubmitInput(PopRemoteInput());   // 相手の入力 (遅れて届く)
//   session.AdvanceTick();                       // 必要なら自動で巻き戻し→再実行
//
// 設計選択:
//   ・**入力は tick % history のリング台帳**: 確定値 (ledger) と「実際に sim へ
//     渡した値」(used) を分けて持つ。遅延到着した確定入力が予測と bit 一致なら
//     再シミュレーションを省略できる (GGPO と同じ最適化)。
//   ・**予測 = 直前 tick で使った入力の繰り返し**: 格闘/アクションで最も外れにくい
//     標準予測。tick 0 or 履歴切れはニュートラル (ゼロ入力)。
//   ・**再シミュレーションは AdvanceTick 冒頭で自動実行**: 呼び出し側は dirty 管理を
//     意識しない。RestoreFrame 失敗 (= 追い出し済み tick への巻き戻し要求) は
//     SubmitInput 側で先に弾くため通常起こらない。
//   ・**max_prediction**: 未確定のまま先行できる tick 数の上限。超えると
//     AdvanceTick が false を返して停止する (入力が届くまで待つ = 実質 lockstep
//     に退化する GGPO の標準挙動)。0 = 無制限。
//   ・**sim コールバックは C 関数ポインタ + user データ**: STL 不使用方針のため
//     TFunction は使わない (FJobGraph 等と同じ規約)。コールバックは決定論であること
//     (同じ world 状態 + 同じ入力列 → 同じ結果) が正しさの前提。
//   ・**コピー / ムーブ禁止**: FWorld* と履歴を抱える長寿命オブジェクト。
//
// 範囲外:
//   ・ネットワーク送受信 / シリアライズ (FInputFrame の I/O は FLockstep 参照)
//   ・desync 検出 (FLockstep::ComputeChecksum や FWorld 側 checksum を併用)
//   ・可変 tick rate / フレームスキップ制御
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "ecs/RollbackBuffer.h"
#include "gameframework/Lockstep.h"   // FInputFrame (入力 POD を共有)

namespace acs::game {

/**
 * FRollbackSession の初期化パラメータ。
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

/** FRollbackSession が扱えるプレイヤー数の上限。 */
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
class FRollbackSession {
public:
    /**
     * 1 tick 分のシミュレーションを進める決定論コールバック。
     *
     * @param world 進める FWorld。
     * @param tick 現在の tick。
     * @param inputs player_id 昇順に並んだ全プレイヤーの入力 (確定 or 予測)。
     * @param input_count inputs の要素数 (= player_count)。
     * @param user SetSimCallback で渡した user データ。
     */
    using SimTickFn = void (*)(FWorld& world, u32 tick, const FInputFrame* inputs,
                               u32 input_count, void* user);

    /** 未初期化状態で構築する。使用前に Init を呼ぶ。 */
    FRollbackSession() noexcept = default;

    /** 破棄する (FWorld は非所有なので触らない)。 */
    ~FRollbackSession() noexcept = default;

    /** コピー禁止 (FWorld* と履歴を抱える長寿命オブジェクト)。 */
    FRollbackSession(const FRollbackSession&)            = delete;

    /** コピー代入も禁止。 */
    FRollbackSession& operator=(const FRollbackSession&) = delete;

    /** ムーブ禁止。 */
    FRollbackSession(FRollbackSession&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FRollbackSession& operator=(FRollbackSession&&)      = delete;

    /**
     * セッションを初期化する (再 Init 可、既存の履歴は破棄)。
     *
     * @details world の全コンポーネント型はコピー構築可能であること
     * (FWorld::CopyFrom の契約)。確保失敗時は未初期化状態に戻して false。
     * @param world 対象の FWorld (非所有、セッションより長生きすること)。
     * @param config プレイヤー数 / 履歴長 / 予測上限。
     * @return 初期化できたら true。引数不正 (world=nullptr / player_count 範囲外 /
     *         history_length=0 / max_prediction >= history_length) や OOM は false。
     */
    bool Init(FWorld* world, const FRollbackSessionConfig& config) noexcept;

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
     * FWorld を進めずに false を返す。予測上限到達は入力が届けば解消する正常系。
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
     * tick カウンタと履歴を start_tick から仕切り直す (FWorld の現在状態は保持)。
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

    /** 対象 FWorld (非所有)。IsInitialized の判定にも使う。 */
    FWorld*             m_World          = nullptr;

    /** 状態履歴 (tick 開始時点の FWorld スナップショット)。 */
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

} // namespace acs::game

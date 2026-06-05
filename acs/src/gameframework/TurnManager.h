// SPDX-License-Identifier: Apache-2.0
// GameFramework ジャンルキット — FTurnManager (ターン進行 + アクションポイント)
//
// SRPG / ローグライク / 戦略系に必須の「ターン制」基盤。複数 side (player /
// enemy / 環境) を initiative 順に並べ、各 side が固有の AP (action point)
// 予算で行動する古典的ターンマネージャ。
//
// 主要 API:
//   ・AddSide / RemoveSide  — side の動的登録解除
//   ・StartRound            — 全 side の AP refill + initiative 順並べ替え +
//                              最初の side を current に
//   ・EndCurrentTurn        — 次 side へ。全 side 完了なら EndOfRound 経由で
//                              StartRound を再実行 (= 次ラウンド開始)
//   ・TryConsumeAP          — 現ターン side から AP 消費。残量不足なら false
//   ・GetSideState          — UI / AI 表示用 read-only snapshot
//
// 設計選択:
//   ・**FTurnSideId** は 24bit index + 8bit gen の packed handle (CooldownId /
//     FSceneTimer / FCollisionWorld2D と同じパターン)。RemoveSide → 再 AddSide
//     で slot が再利用されても、古い ID が stale として検出できる。
//   ・**Side ストレージは AoS TArray**: side 数は通常 2〜8 程度、多くて十数
//     なので AoS で十分。AP 更新が支配的なので cache 局所性も悪くない。
//   ・**turn order は別 TArray<u32>**: initiative 順に並べ替えた slot index を
//     保持。TArray<SideSlot> 自体を並べ替えると stable ID が崩れるため。
//   ・**ETurnPhase** は 5 値: Setup (Init 直後 / StartRound 前) / PlayerTurn /
//     EnemyTurn / EnvironmentTurn / EndOfRound (StartRound 直後の一瞬の遷移
//     状態。callback 経由で外部に通知される)。
//   ・**SideKind 推定**: is_player_controlled=true → PlayerTurn。false かつ
//     display_name が "Env*" で始まる → EnvironmentTurn、それ以外 → EnemyTurn。
//     厳密な分類は caller の自由なので、判定は単純な命名規約ベースに留める。
//   ・**callback は単一スロット**: OnTurnStart と OnRoundEnd を各 1 個保持。
//     std::function 不使用、void* user で context を回す。
//   ・**EndCurrentTurn の連鎖**: 最後の side で EndCurrentTurn が呼ばれると
//     EndOfRound に遷移 → RoundEndCallback 発火 → StartRound を内部で呼んで
//     次 round を即開始する (1 呼び出しで「ターン送り + 次ラウンド開始」)。
//   ・**StartRound 時の並べ替え**: insertion sort で initiative 降順。同じ
//     initiative の場合は AddSide 順 (= slot index 昇順) で安定ソート。
//   ・**非コピー・非ムーブ、全 noexcept、STL 不使用、<string> 禁止**。
//
// 範囲外 (将来):
//   ・行動予約 (queued action) / interrupt (反撃割込み) — Combat 系モジュールに委譲
//   ・AP 以外の resource (mana / stamina / 連携ゲージ) — caller 側で別管理
//   ・PvP / マルチプレイヤーの同期 — Network 系モジュールに委譲
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * ターンの進行フェーズ。
 *
 * @details
 * 値は安定なので save / replay に書ける (将来追加時は末尾追加のみ、既存値の意味は
 * 不変とする規約)。EndOfRound は StartRound 内で一瞬発生する遷移状態で、
 * RoundEndCallback 発火直後に最初の side の phase に切り替わる。
 */
enum class ETurnPhase : u8 {
    /** Init 直後 / StartRound 前 (side 登録中)。 */
    Setup            = 0,

    /** is_player_controlled=true な side のターン。 */
    PlayerTurn       = 1,

    /** 通常敵 side のターン。 */
    EnemyTurn        = 2,

    /** 環境 (天候 / トラップ / 中立 NPC) のターン。 */
    EnvironmentTurn  = 3,

    /** 全 side 行動完了直後 (callback 発火用、すぐ次ラウンドへ)。 */
    EndOfRound       = 4,
};

/**
 * side を識別する packed handle (24bit index + 8bit generation)。
 *
 * @details m_Packed == 0 を invalid と定義 (gen は常に 1 以上に保たれる)。
 */
struct FTurnSideId {
    /** index と generation を詰めた 32bit 値 (0 = invalid)。 */
    u32 m_Packed = 0u;

    /**
     * 有効な handle かを返す。
     *
     * @return m_Packed != 0 なら true。
     */
    bool IsValid() const noexcept { return m_Packed != 0u; }

    /** index 部に割り当てる bit 数。 */
    static constexpr u32 kIndexBits = 24u;

    /** index 部を取り出すマスク (0x00FFFFFF)。 */
    static constexpr u32 kIndexMask = (1u << kIndexBits) - 1u;

    /** index の最大値 (16777215 個)。 */
    static constexpr u32 kMaxIndex  = kIndexMask;

    /**
     * index と generation を packed handle に詰める。
     *
     * @param index slot のインデックス。
     * @param gen generation カウンタ。
     * @return 構築した FTurnSideId。
     */
    static FTurnSideId Pack(u32 index, u8 gen) noexcept {
        FTurnSideId h;
        h.m_Packed = (static_cast<u32>(gen) << kIndexBits) | (index & kIndexMask);
        return h;
    }

    /**
     * index 部を取り出す。
     *
     * @return slot のインデックス。
     */
    u32 Index() const noexcept { return m_Packed & kIndexMask; }

    /**
     * generation 部を取り出す。
     *
     * @return generation カウンタ。
     */
    u8  Gen()   const noexcept { return static_cast<u8>(m_Packed >> kIndexBits); }

    /**
     * 等価比較。
     *
     * @param o 比較対象。
     * @return packed 値が等しければ true。
     */
    constexpr bool operator==(FTurnSideId o) const noexcept { return m_Packed == o.m_Packed; }

    /**
     * 非等価比較。
     *
     * @param o 比較対象。
     * @return packed 値が異なれば true。
     */
    constexpr bool operator!=(FTurnSideId o) const noexcept { return m_Packed != o.m_Packed; }
};

/**
 * side 状態の公開 snapshot (GetSideState で返す read-only view)。
 *
 * @details Slot から AP / has_acted を read-only にコピーした UI / AI / save 用の view。
 */
struct FTurnSideState {
    /** AddSide 時のラベル (caller が寿命管理)。 */
    const char* display_name         = nullptr;

    /** StartRound での AP refill 量。 */
    u32         max_action_points    = 0u;

    /** 現在の AP 残量 (TryConsumeAP で減る)。 */
    u32         current_action_points = 0u;

    /** 行動順 (大きいほど先)。 */
    u32         initiative           = 0u;

    /** PlayerTurn 判定用フラグ。 */
    bool        is_player_controlled = false;

    /** 今ラウンドで既に行動済かのフラグ。 */
    bool        has_acted            = false;
};

/**
 * ターン開始時に呼ばれる callback 型 (current side 切り替え直後)。
 *
 * @param user SetOnTurnStartCallback で登録した context ポインタ。
 * @param side 新しい current side の ID。
 * @param round 現在のラウンド番号 (StartRound 内なら新ラウンド番号)。
 */
using TurnStartCallback = void(*)(void* user, FTurnSideId side, u32 round) noexcept;

/**
 * ラウンド終了時 (EndOfRound 遷移時) に呼ばれる callback 型。
 *
 * @details
 * 次ラウンドの StartRound に進む直前に発火する。caller はここで報酬計算 / DoT 処理 /
 * weather tick 等を行う。
 * @param user SetOnRoundEndCallback で登録した context ポインタ。
 * @param round 終了したラウンド番号 (= 終了したラウンドの旧番号)。
 */
using RoundEndCallback = void(*)(void* user, u32 round) noexcept;

/**
 * 複数 side を initiative 順に進めるターン制マネージャ (AP 予算管理付き)。
 *
 * @details
 * SRPG / ローグライク / 戦略系向けのターン制基盤。各 side は固有の AP (action point)
 * 予算で行動し、StartRound で AP refill + initiative 順並べ替え、EndCurrentTurn で
 * 次 side へ進む。最後の side 完了時は EndOfRound を経由して StartRound を内部で再実行
 * し、次ラウンドを即開始する。side は AoS の TArray に格納し、turn order は別の
 * TArray<u32> で保持して stable ID を保つ。非コピー・非ムーブ。
 */
class FTurnManager {
public:
    /** 空のターンマネージャを構築する (side 未登録、phase=Setup)。 */
    FTurnManager() noexcept = default;

    /** 破棄する。 */
    ~FTurnManager() noexcept = default;

    /** コピー禁止 (callback の self ポインタとの競合を防ぐため)。 */
    FTurnManager(const FTurnManager&)            = delete;

    /** コピー代入も禁止。 */
    FTurnManager& operator=(const FTurnManager&) = delete;

    /** ムーブ禁止 (callback の self ポインタとの競合を防ぐため)。 */
    FTurnManager(FTurnManager&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FTurnManager& operator=(FTurnManager&&)      = delete;

    /**
     * 全 side / turn order を捨て、phase=Setup, round=0 に初期化する。
     *
     * @details callback は保持される (登録解除しない)。
     */
    void Init() noexcept;

    /** 全 side / callback も含めてクリアする (完全リセット)。 */
    void ClearAll() noexcept;

    /**
     * side を登録する。
     *
     * @details
     * 登録は Setup phase でも進行中でも可能。進行中に追加した side は次ラウンドから
     * turn order に組み込まれる (今ラウンドには参加しない、has_acted=true 扱いで除外)。
     * @param display_name side のラベル (caller が寿命管理。nullptr は不正)。
     * @param max_ap StartRound での AP refill 量。
     * @param initiative 行動順 (大きいほど先)。
     * @param is_player_controlled PlayerTurn 判定に使うフラグ。
     * @return 割り当てた FTurnSideId。display_name が nullptr / 既に kMaxIndex 個登録済みなら invalid id。
     */
    FTurnSideId AddSide(const char* display_name, u32 max_ap, u32 initiative,
                       bool is_player_controlled) noexcept;

    /**
     * side を除去する (slot を invalidate して gen を進め、turn order からも除去)。
     *
     * @details 現在 turn 中の side を除去した場合は EndCurrentTurn 相当の挙動で次へ進む。
     * @param id 除去する side の ID。
     */
    void RemoveSide(FTurnSideId id) noexcept;

    /**
     * ラウンドを開始する。
     *
     * @details
     * 全 side の AP を max_action_points に refill + has_acted=false + initiative 降順で
     * turn order を再構築 + 先頭 side を current に + phase 更新 + TurnStartCallback 発火。
     * 登録 side が 0 個なら何もしない (phase=Setup のまま)。
     */
    void StartRound() noexcept;

    /**
     * 現ターンを終了し次の side へ進める。
     *
     * @details
     * 現 side の has_acted=true にして次 side へ。次 side が無い場合は EndOfRound に遷移 →
     * RoundEndCallback 発火 → StartRound を内部で呼んで次ラウンドを即開始する
     * (1 呼び出しで「ターン送り + 次ラウンド開始」がまとめて起こる)。
     * Setup / EndOfRound 中の呼び出しは no-op。
     */
    void EndCurrentTurn() noexcept;

    /**
     * 現ターン side から AP を消費する。
     *
     * @param amount 消費する AP 量。
     * @return 消費できたら true。残量不足 / 現ターン無し (Setup / EndOfRound) / amount==0 なら false (AP 据置)。
     */
    bool TryConsumeAP(u32 amount) noexcept;

    /**
     * 現ターン側の side ID を返す。
     *
     * @return 現ターン side の ID。Setup / EndOfRound 中は invalid id。
     */
    FTurnSideId CurrentTurnSide() const noexcept;

    /**
     * 現在のフェーズを返す。
     *
     * @return 現在の ETurnPhase。
     */
    ETurnPhase CurrentPhase() const noexcept { return m_Phase; }

    /**
     * 現在のラウンド番号を返す。
     *
     * @return 現在のラウンド番号 (StartRound で +1)。
     */
    u32       CurrentRound() const noexcept { return m_Round; }

    /**
     * side 状態の read-only snapshot を返す。
     *
     * @details
     * 返却された pointer は次の AddSide / RemoveSide / ClearAll / Init までしか有効ではない
     * (TArray 再確保で無効化される可能性あり)。
     * @param id 取得する side の ID。
     * @return side 状態へのポインタ。stale handle / 未登録 ID なら nullptr。
     */
    const FTurnSideState* GetSideState(FTurnSideId id) const noexcept;

    /**
     * 登録 side 数を返す。
     *
     * @return 登録中の side 数 (RemoveSide で除去した分は含まない)。
     */
    u32 SideCount() const noexcept { return m_ActiveCount; }

    /**
     * 今ラウンドで残り未行動の side 数を返す。
     *
     * @details
     * turn order 中で has_acted=false の数。現ターン中の side も「まだ行動完結していない」
     * ので残数に含まれる。Setup / EndOfRound 中は 0。
     * @return 残り未行動 side 数。
     */
    u32 TurnsRemainingThisRound() const noexcept;

    /**
     * ターン開始 callback を登録する。
     *
     * @param cb 登録する callback (nullptr で登録解除)。
     * @param user callback に渡す context ポインタ。
     */
    void SetOnTurnStartCallback(TurnStartCallback cb, void* user) noexcept {
        m_OnTurnStart      = cb;
        m_OnTurnStartUser = user;
    }

    /**
     * ラウンド終了 callback を登録する。
     *
     * @param cb 登録する callback (nullptr で登録解除)。
     * @param user callback に渡す context ポインタ。
     */
    void SetOnRoundEndCallback(RoundEndCallback cb, void* user) noexcept {
        m_OnRoundEnd      = cb;
        m_OnRoundEndUser = user;
    }

private:
    /**
     * 1 side 分の内部 slot。
     *
     * @details
     * display_name の寿命は caller 管理。公開 view (FTurnSideState) を内部にそのまま埋め込み、
     * GetSideState ではそのアドレスを返す。これにより layout 依存の reinterpret_cast を避ける。
     */
    struct SideSlot {
        /** 公開 snapshot (このアドレスを GetSideState が返す)。 */
        FTurnSideState view;

        /** slot 使用中かのフラグ。 */
        bool          active = false;

        /** generation カウンタ (0 = 未使用)。 */
        u8            gen    = 0u;
    };

    /**
     * 空き slot を確保して index を返す。
     *
     * @return 確保した slot のインデックス。
     */
    u32         AcquireSlot() noexcept;

    /**
     * handle から slot を解決する。
     *
     * @param id 解決する side の ID。
     * @return 対応する SideSlot へのポインタ。stale / 未登録なら nullptr。
     */
    SideSlot*       Resolve(FTurnSideId id) noexcept;

    /**
     * handle から slot を解決する (const 版)。
     *
     * @param id 解決する side の ID。
     * @return 対応する SideSlot への const ポインタ。stale / 未登録なら nullptr。
     */
    const SideSlot* Resolve(FTurnSideId id) const noexcept;

    /** initiative 降順 (安定ソート) で m_TurnOrder を再構築する。 */
    void RebuildTurnOrder() noexcept;

    /**
     * 次の行動者へ進める。
     *
     * @details
     * turn order 内で order index >= start_from な最初の「未行動 (has_acted=false) かつ
     * active」slot を見つけて m_CurrentOrderIndex に設定する。見つからなければ
     * kInvalidOrderIndex を設定する。
     * @param start_from 探索を開始する order index。
     */
    void AdvanceToNextActor(u32 start_from) noexcept;

    /**
     * current slot の属性から ETurnPhase を推定する。
     *
     * @param s 判定対象の side slot。
     * @return is_player_controlled / display_name から推定した ETurnPhase。
     */
    static ETurnPhase ClassifyPhase(const SideSlot& s) noexcept;

    /**
     * display_name が "Env" (3 文字) で始まるかを判定する。
     *
     * @param name 判定する display_name。
     * @return "Env" で始まれば true。
     */
    static bool IsEnvironmentName(const char* name) noexcept;

    /** side slot 列 (AoS、AddSide 順)。 */
    TArray<SideSlot> m_Slots;

    /** initiative 順に並んだ slot index 列。 */
    TArray<u32>      m_TurnOrder;

    /** アクティブな side 数 (RemoveSide で減る)。 */
    u32             m_ActiveCount          = 0u;

    /** 現在のラウンド番号 (StartRound で +1)。 */
    u32             m_Round                 = 0u;

    /** 現在のターンフェーズ。 */
    ETurnPhase       m_Phase                 = ETurnPhase::Setup;

    /** 「現ターン無し」を表す order index 値。 */
    static constexpr u32 kInvalidOrderIndex = 0xFFFFFFFFu;

    /** m_TurnOrder 内の現在位置 (kInvalidOrderIndex = 現ターン無し)。 */
    u32 m_CurrentOrderIndex = kInvalidOrderIndex;

    /** ターン開始 callback。 */
    TurnStartCallback m_OnTurnStart      = nullptr;

    /** ターン開始 callback に渡す context。 */
    void*             m_OnTurnStartUser = nullptr;

    /** ラウンド終了 callback。 */
    RoundEndCallback  m_OnRoundEnd       = nullptr;

    /** ラウンド終了 callback に渡す context。 */
    void*             m_OnRoundEndUser  = nullptr;
};

} // namespace acs::game

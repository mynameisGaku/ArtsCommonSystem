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

// ----- ターンフェーズ -----------------------------------------------------
// 値は安定なので save / replay に書ける (将来追加時は末尾追加のみ、既存値
// の意味は不変とする規約)。EndOfRound は StartRound 内で一瞬発生する遷移
// 状態で、RoundEndCallback 発火直後に最初の side の phase に切り替わる。
enum class ETurnPhase : u8 {
    Setup            = 0, // Init 直後 / StartRound 前 (side 登録中)
    PlayerTurn       = 1, // is_player_controlled=true な side のターン
    EnemyTurn        = 2, // 通常敵 side のターン
    EnvironmentTurn  = 3, // 環境 (天候 / トラップ / 中立 NPC) のターン
    EndOfRound       = 4, // 全 side 行動完了直後 (callback 発火用、すぐ次ラウンドへ)
};

// ----- side handle --------------------------------------------------------
// 24bit index + 8bit generation。_packed == 0 を invalid と定義
// (gen は常に 1 以上に保たれる)。
struct FTurnSideId {
    u32 _packed = 0u;

    bool IsValid() const noexcept { return _packed != 0u; }

    static constexpr u32 kIndexBits = 24u;
    static constexpr u32 kIndexMask = (1u << kIndexBits) - 1u; // 0x00FFFFFF
    static constexpr u32 kMaxIndex  = kIndexMask;              // 16777215 個

    static FTurnSideId Pack(u32 index, u8 gen) noexcept {
        FTurnSideId h;
        h._packed = (static_cast<u32>(gen) << kIndexBits) | (index & kIndexMask);
        return h;
    }
    u32 Index() const noexcept { return _packed & kIndexMask; }
    u8  Gen()   const noexcept { return static_cast<u8>(_packed >> kIndexBits); }

    constexpr bool operator==(FTurnSideId o) const noexcept { return _packed == o._packed; }
    constexpr bool operator!=(FTurnSideId o) const noexcept { return _packed != o._packed; }
};

// ----- side 状態 (公開 snapshot) ------------------------------------------
// GetSideState で返す。Slot から AP / has_acted を read-only にコピーした
// 表示用 view (UI / AI / save 用)。
struct FTurnSideState {
    const char* display_name         = nullptr; // AddSide 時のラベル (caller 寿命管理)
    u32         max_action_points    = 0u;      // StartRound での refill 量
    u32         current_action_points = 0u;     // 現残量 (TryConsumeAP で減る)
    u32         initiative           = 0u;      // 行動順 (大きいほど先)
    bool        is_player_controlled = false;   // PlayerTurn 判定用
    bool        has_acted            = false;   // 今ラウンドで既に行動済か
};

// ----- callback -----------------------------------------------------------
// ターン開始時。current side が切り替わった直後に呼ばれる。
//   side  : 新しい current side の ID
//   round : 現在のラウンド番号 (StartRound 内なら新ラウンド番号)
using TurnStartCallback = void(*)(void* user, FTurnSideId side, u32 round) noexcept;

// ラウンド終了時 (EndOfRound 遷移時)。次ラウンドの StartRound に進む直前
// に発火する。caller はここで報酬計算 / DoT 処理 / weather tick 等を行う。
//   round : 終了したラウンド番号 (= 終了したラウンドの旧番号)
using RoundEndCallback = void(*)(void* user, u32 round) noexcept;

class FTurnManager {
public:
    FTurnManager() noexcept = default;
    ~FTurnManager() noexcept = default;

    // 非コピー・非ムーブ (callback の self ポインタとの競合を防ぐ)
    FTurnManager(const FTurnManager&)            = delete;
    FTurnManager& operator=(const FTurnManager&) = delete;
    FTurnManager(FTurnManager&&)                 = delete;
    FTurnManager& operator=(FTurnManager&&)      = delete;

    // ----- 初期化 / リセット -----
    // Init: 全 side / order を捨て、phase=Setup, round=0 に。callback は保持。
    void Init() noexcept;

    // 全 side / callback も含めてクリア (Reset 相当)。
    void ClearAll() noexcept;

    // ----- side 登録 -----
    // display_name == nullptr / 既に kMaxIndex 個登録済みなら invalid id を返す。
    // 登録は Setup phase でも進行中でも可能。進行中に追加した side は次ラウンド
    // から turn order に組み込まれる (今ラウンドには参加しない、has_acted=true
    // 扱いで除外)。
    FTurnSideId AddSide(const char* display_name, u32 max_ap, u32 initiative,
                       bool is_player_controlled) noexcept;

    // slot を invalidate (gen 進む)。turn order からも除去。
    // 現在 turn 中の side を除去した場合は EndCurrentTurn 相当の挙動で次へ進む。
    void RemoveSide(FTurnSideId id) noexcept;

    // ----- ラウンド進行 -----
    // 全 side の AP を max_action_points に refill + has_acted=false + initiative
    // 降順で turn order を再構築 + 先頭 side を current に + phase 更新 +
    // TurnStartCallback 発火。
    // 登録 side が 0 個なら何もしない (phase=Setup のまま)。
    void StartRound() noexcept;

    // 現 side の has_acted=true → 次 side へ。次 side が無い場合は
    // EndOfRound に遷移 → RoundEndCallback 発火 → StartRound を内部で呼んで
    // 次ラウンド開始 (1 呼び出しで「ターン送り + 次ラウンド開始」がまとめて起こる)。
    // Setup / EndOfRound 中の呼び出しは no-op。
    void EndCurrentTurn() noexcept;

    // 現ターン side から AP を consume。残量不足なら false を返して AP 据置。
    // 現ターン無し (Setup / EndOfRound) / amount == 0 は false。
    bool TryConsumeAP(u32 amount) noexcept;

    // ----- 問い合わせ -----
    // 現ターン側 ID。Setup / EndOfRound 中は invalid id を返す。
    FTurnSideId CurrentTurnSide() const noexcept;

    ETurnPhase CurrentPhase() const noexcept { return _phase; }
    u32       CurrentRound() const noexcept { return _round; }

    // stale handle / 未登録 ID は nullptr。返却された pointer は次の
    // AddSide / RemoveSide / ClearAll / Init までしか有効ではない (TArray 再確保で
    // 無効化される可能性あり)。
    const FTurnSideState* GetSideState(FTurnSideId id) const noexcept;

    // 登録 side 数 (RemoveSide で除去した分は含まない)。
    u32 SideCount() const noexcept { return _active_count; }

    // 今ラウンドで残り未行動の side 数 (= turn order 中 has_acted=false の数)。
    // 現ターン中の side も「まだ行動完結していない」ので残数に含まれる。
    // Setup / EndOfRound 中は 0。
    u32 TurnsRemainingThisRound() const noexcept;

    // ----- callback -----
    // cb == nullptr で登録解除。重複登録は不可 (上書き)。
    void SetOnTurnStartCallback(TurnStartCallback cb, void* user) noexcept {
        _on_turn_start      = cb;
        _on_turn_start_user = user;
    }
    void SetOnRoundEndCallback(RoundEndCallback cb, void* user) noexcept {
        _on_round_end      = cb;
        _on_round_end_user = user;
    }

private:
    // 1 side 分の内部 slot。display_name の寿命は caller 管理。
    // 公開 view (FTurnSideState) を内部にそのまま埋め込み、GetSideState では
    // そのアドレスを返す。これにより layout 依存の reinterpret_cast を避ける。
    struct SideSlot {
        FTurnSideState view;             // 公開 snapshot (このアドレスを GetSideState が返す)
        bool          active = false;   // slot 使用中か
        u8            gen    = 0u;      // 0 = 未使用
    };

    // ----- 内部 helper -----
    u32         AcquireSlot() noexcept;
    SideSlot*       Resolve(FTurnSideId id) noexcept;
    const SideSlot* Resolve(FTurnSideId id) const noexcept;

    // initiative 降順 (安定ソート) で _turn_order を再構築。
    void RebuildTurnOrder() noexcept;

    // turn order 内で order index >= start_from な最初の「未行動 (has_acted=false)
    // かつ active」slot を見つけ、_current_order_index に設定。
    // 見つからなければ kInvalidOrderIndex を設定。
    void AdvanceToNextActor(u32 start_from) noexcept;

    // current slot の is_player_controlled / display_name から ETurnPhase を推定。
    static ETurnPhase ClassifyPhase(const SideSlot& s) noexcept;

    // display_name が "Env" (3 文字) で始まるかの簡易判定。
    static bool IsEnvironmentName(const char* name) noexcept;

    // ----- データ -----
    TArray<SideSlot> _slots;          // AoS、AddSide 順
    TArray<u32>      _turn_order;     // initiative 順に並んだ slot index 列
    u32             _active_count          = 0u; // RemoveSide で減る
    u32             _round                 = 0u; // StartRound で +1
    ETurnPhase       _phase                 = ETurnPhase::Setup;

    // _turn_order 内の現在位置。kInvalidOrderIndex は「現ターン無し」。
    static constexpr u32 kInvalidOrderIndex = 0xFFFFFFFFu;
    u32 _current_order_index = kInvalidOrderIndex;

    // ----- callback -----
    TurnStartCallback _on_turn_start      = nullptr;
    void*             _on_turn_start_user = nullptr;
    RoundEndCallback  _on_round_end       = nullptr;
    void*             _on_round_end_user  = nullptr;
};

} // namespace acs::game

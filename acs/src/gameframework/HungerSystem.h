// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R/I — FHungerSystem (Survival ジャンルキット: 空腹/喉/疲労/睡眠)
//
// サバイバル系ゲーム (DayZ / The Long Dark / Don't Starve 等) で頻出する
// 「複数 survivor (= プレイヤー / NPC) に対して、空腹・喉の渇き・疲労・正気度・
// 体温などの生存統計値を秒単位で decay させ、critical 閾値を切ったら警告し、
// 0 に達したら HP ダメージを FHealthSystem 経由で与える」マネージャ。
//
// FHealthSystem との関係:
//   ・本クラスは HP を直接管理しない。0 到達時の zero_damage_per_sec を
//     DamageCallback で外部へ通知し、上位 (= GameDirector / SurvivalDirector)
//     が FHealthSystem::ApplyDamage を呼ぶ「弱結合 bridge」を取る。
//   ・IsAlive(SurvivorId) は本クラスの世界での生死判定 = 全 stat の何れかが
//     0 ではない & HealthBridge が「HP > 0」を返すか否か。HealthBridge を
//     セットしない場合は stat 部分だけで判定する。
//
// 設計選択 (Pillar R/I — FHungerSystem):
//   ・**SurvivorId は 24bit index + 8bit gen の packed handle**: FHealthId /
//     FBuffOwnerId / FNodeId と同規約。`_packed == 0` を invalid とし gen は
//     常に 1 以上で配る。AddSurvivor 後に RemoveSurvivor された slot を再利用
//     しても古い handle は gen 不一致で確実に弾ける。
//   ・**stat enum は固定 7 値 (Hunger / Thirst / Energy / Sanity / Warmth /
//     Custom1 / Custom2)**: array index として直接使う。Custom1/2 はゲーム
//     固有 (例: Radiation, Oxygen) を載せる拡張枠。
//   ・**各 survivor は固定 7 stat の TArray を保持**: SoA にする旨味は少ない
//     (1 survivor あたり stat は 7 個固定で、横断クエリは Tick だけなので
//     dense layout で十分)。AddSurvivor で 7 個ぶん push して以後固定。
//   ・**StatConfig は stat 種別ごとに 1 個固定** (= 全 survivor で共通): 個体差
//     (= 強キャラは空腹に強い等) は将来 Modifier レイヤで載せる想定で、今は
//     共通 config + per-survivor StatState の単純構造を採る。
//   ・**Tick の流れ**: 全 survivor × 全 stat を 1 重ループで回し、
//       1) stat.current -= config.decay_per_sec * dt
//       2) clamp(0, max)
//       3) critical_threshold を新たに跨いだら CriticalCallback 発火 (entered)
//       4) 復帰した (threshold より上に戻った) ら CriticalCallback 発火 (exited)
//       5) stat.current == 0 の場合は zero_damage_per_sec * dt を DamageCallback
//          経由で通知 (= 1 フレで複数 stat が同時に発火する可能性あり)
//   ・**RestoreStat / DrainStat / SetStat は clamp(0, max) を強制**: 上位の
//     ロジックバグで負値や max 超過が乗らない安全側設計。
//   ・**callback は C 関数ポインタ + void* user** (`<functional>` 禁止)。複数
//     リスナーが必要なら呼出側で fan-out。critical / damage は別 callback で
//     独立に attach/detach 可能。
//   ・**OverallSurvivalHealth(id)** : 全 stat の (current / max) の平均を [0,1]
//     で返す。UI のキャラ状態バー (= 「生存度メータ」) 表示用。
//   ・**非コピー・非ムーブ**: 内部 TArray<SurvivorSlot> がさらに TArray<StatState>
//     を持つ二段ネスト構造で、callback の発火タイミングで外部参照が破綻する
//     可能性があるため。FGame / Scene 単位で 1 個保持の想定。
//   ・**全 noexcept、STL 不使用、`<string>` 禁止**: ACS 規約。失敗は bool / 哨兵で表現。
//
// 使い方:
//   FHungerSystem hs;
//   hs.Init();
//
//   // 1) stat ごとに decay/critical/damage を config
//   hs.ConfigureStat(ESurvivalStat::Hunger,
//       StatConfig{ /*max*/100.0f, /*decay*/0.5f, /*critical*/20.0f, /*zero_dmg*/2.0f });
//   hs.ConfigureStat(ESurvivalStat::Thirst,
//       StatConfig{ 100.0f, 1.0f, 25.0f, 3.0f });   // 喉は空腹より速い
//
//   // 2) survivor を追加 (= 全 stat が max でスタート)
//   SurvivorId player = hs.AddSurvivor();
//
//   // 3) コールバックを attach (FHealthSystem への bridge)
//   hs.SetOnCriticalCallback(&MyOnCritical, &game_ctx);
//   hs.SetOnDamageCallback(&MyOnDamage, &game_ctx);
//
//   // 4) ゲーム中: アイテム使用で stat 回復
//   hs.RestoreStat(player, ESurvivalStat::Hunger, 30.0f);   // 肉を食べた
//
//   // 5) 毎フレ
//   hs.Tick(dt);
//
//   // 6) UI 表示
//   const f32 hp = hs.OverallSurvivalHealth(player);   // [0,1]
//
// 範囲外 (将来 Phase で):
//   ・stat 間の連鎖 (空腹 → 疲労増加) → 上位ロジックが Tick 後に DrainStat で実装
//   ・天候 / 環境による decay 倍率 → Modifier レイヤで別途
//   ・永続化 → Pillar J Serialize に委譲
//   ・stat の表示名 / 説明文 / アイコン → 呼出側の UI 層で別 table を引く
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"

namespace acs::game {

// ---- ESurvivalStat ----------------------------------------------------------
// 生存統計値の種別。array index として直接使うため値は連続。Custom1 / Custom2
// はゲーム固有 (例: Radiation / Oxygen / Bleeding) を載せる拡張枠。
//   kCount = stat 種別の総数 (= 7)。配列サイズや loop 終端に使う。
enum class ESurvivalStat : u8 {
    Hunger  = 0,   // 空腹
    Thirst  = 1,   // 喉の渇き
    Energy  = 2,   // 疲労 (= スタミナの長期版)
    Sanity  = 3,   // 正気度 (恐怖 / ホラー系)
    Warmth  = 4,   // 体温 (寒冷地サバイバル)
    Custom1 = 5,   // ゲーム固有 1 (例: Radiation)
    Custom2 = 6,   // ゲーム固有 2 (例: Oxygen)
};
inline constexpr u32 kSurvivalStatCount = 7u;

// ---- StatConfig: stat 種別ごとの設定 (全 survivor で共通) -------------------
// max_value          : stat の上限値 (= AddSurvivor 直後の初期値)。0 以下指定は
//                      防御的に 1.0 として扱う。
// decay_per_sec      : 1 秒あたりの自然減少量。0 以下なら decay しない (= 永続)。
// critical_threshold : この値を下回ると is_critical=true + CriticalCallback 発火。
//                      復帰時 (threshold より上に戻る) にも callback 発火。
//                      max_value より大きい指定は max_value で clamp。
// zero_damage_per_sec: stat == 0 のとき、1 秒あたり FHealthSystem へ通知するダメージ。
//                      0 以下なら通知しない (= 「不快なだけで死なない」stat 用)。
struct StatConfig {
    f32 max_value          = 100.0f;
    f32 decay_per_sec      = 0.0f;
    f32 critical_threshold = 20.0f;
    f32 zero_damage_per_sec = 0.0f;
};

// ---- StatState: ある survivor の 1 stat の実状態 ----------------------------
// current     : 現在値 ([0, max] でクランプ)。
// max         : 上限値 (StatConfig::max_value のコピー。SetMaxStat 系は将来拡張)。
// is_critical : critical_threshold を下回っているか。Tick 内で更新される。
struct StatState {
    f32  current     = 0.0f;
    f32  max         = 0.0f;
    bool is_critical = false;
};

// ---- SurvivorId: 24bit index + 8bit gen を packed した opaque handle -------
// `_packed == 0` を invalid と定義 (gen は常に 1 以上で配る)。FHealthId /
// FBuffOwnerId と同一規約。
struct SurvivorId {
    u32 _packed = 0u;

    static constexpr u32 kIndexBits = 24u;
    static constexpr u32 kIndexMask = (1u << kIndexBits) - 1u; // 0x00FFFFFF
    static constexpr u32 kMaxIndex  = kIndexMask;              // 16777215

    bool IsValid() const noexcept { return _packed != 0u; }

    static SurvivorId Pack(u32 index, u8 gen) noexcept {
        SurvivorId o;
        o._packed = (static_cast<u32>(gen) << kIndexBits) | (index & kIndexMask);
        return o;
    }
    u32 Index() const noexcept { return _packed & kIndexMask; }
    u8  Gen()   const noexcept { return static_cast<u8>(_packed >> kIndexBits); }

    bool operator==(SurvivorId o) const noexcept { return _packed == o._packed; }
    bool operator!=(SurvivorId o) const noexcept { return _packed != o._packed; }
};

// ---- FHungerSystem ----------------------------------------------------------
class FHungerSystem {
public:
    // ---- callback 型 ------------------------------------------------------
    // critical 状態の遷移通知。entered_critical=true で「閾値を割って入った」、
    // false で「復帰した」を意味する。発火タイミングは Tick 内 + stat の値が
    // 書き換わった直後。
    using CriticalCallback = void(*)(void* user, SurvivorId id, ESurvivalStat stat,
                                      bool entered_critical) noexcept;

    // stat=0 滞在中の HP ダメージ通知。Tick 内で dt × zero_damage_per_sec を
    // 計算して呼び出す。HealthBridge を実装する側はここで FHealthSystem::ApplyDamage
    // を呼ぶ想定。1 フレで複数 stat 同時発火しうる。
    using DamageCallback = void(*)(void* user, SurvivorId id, ESurvivalStat stat,
                                    f32 damage) noexcept;

    FHungerSystem()  noexcept = default;
    ~FHungerSystem() noexcept = default;

    // 非コピー・非ムーブ: 内部 TArray<SurvivorSlot> + その中の TArray<StatState>。
    FHungerSystem(const FHungerSystem&)            = delete;
    FHungerSystem& operator=(const FHungerSystem&) = delete;
    FHungerSystem(FHungerSystem&&)                 = delete;
    FHungerSystem& operator=(FHungerSystem&&)      = delete;

    // ---- 初期化 -----------------------------------------------------------
    // 7 stat 分の StatConfig をデフォルト値で初期化する。コンストラクタでは
    // やらないのは「Director パターンで明示的に Init/Shutdown を踏ませる」方針
    // に合わせるため。再呼び出しは全 survivor を破棄して config もリセット。
    void Init() noexcept;

    // ---- stat config ------------------------------------------------------
    // stat 種別ごとの decay / critical / damage 設定を上書き。Init 後に呼ぶ。
    // 既存 survivor の StatState には影響しない (= 既に走っている survivor の
    // max が変わると不整合になるので、ゲーム起動時に一括設定する想定)。
    void ConfigureStat(ESurvivalStat stat, const StatConfig& config) noexcept;

    // ---- survivor 管理 ----------------------------------------------------
    // 新規 survivor を登録。全 stat が config.max_value で初期化される。
    // 24bit index 上限 (16,777,215) に達した場合は invalid handle を返す。
    SurvivorId AddSurvivor() noexcept;

    // survivor を破棄 (= slot 解放 + gen 進める)。callback は発火しない。
    // invalid / stale / 範囲外 handle は no-op。
    void RemoveSurvivor(SurvivorId id) noexcept;

    // ---- stat 操作 (clamp(0, max) を強制) ---------------------------------
    // 加算 (回復)。0 以下 amount は no-op。invalid id は no-op。
    void RestoreStat(SurvivorId id, ESurvivalStat stat, f32 amount) noexcept;

    // 減算 (消費)。0 以下 amount は no-op。invalid id は no-op。
    // critical 跨ぎ / zero 到達があれば次回 Tick で callback が発火する設計
    // (= 即時 callback はせず、Tick で一元管理して再入を避ける)。
    void DrainStat(SurvivorId id, ESurvivalStat stat, f32 amount) noexcept;

    // 絶対値で設定。負値は 0 に、max 超過は max に clamp。invalid id は no-op。
    void SetStat(SurvivorId id, ESurvivalStat stat, f32 value) noexcept;

    // ---- 問い合わせ -------------------------------------------------------
    // 現在値を返す。invalid id / 範囲外 stat は 0.0f。
    f32 GetStat(SurvivorId id, ESurvivalStat stat) const noexcept;

    // is_critical フラグを返す。invalid id は false。
    bool IsCritical(SurvivorId id, ESurvivalStat stat) const noexcept;

    // この survivor が「生きているか」: 全 stat が 0 ではない & (HealthBridge が
    // セットされていれば) HP > 0。HealthBridge 未設定なら stat 部分だけで判定。
    // invalid / removed survivor は false。
    bool IsAlive(SurvivorId id) const noexcept;

    // [0, 1] の総合生存度。全 stat の (current / max) の平均値。
    // invalid id は 0.0f、stat が 1 個も configure されてなければ 0.0f。
    f32 OverallSurvivalHealth(SurvivorId id) const noexcept;

    // 全 active survivor 数 (生死問わず)。
    u32 SurvivorCount() const noexcept { return _survivor_count; }

    // ---- driver -----------------------------------------------------------
    // 全 survivor × 全 stat を dt 秒進める:
    //   1) stat.current -= config.decay_per_sec * dt → clamp(0, max)
    //   2) critical_threshold 跨ぎを検出 → CriticalCallback 発火
    //   3) stat.current == 0 なら zero_damage_per_sec * dt を DamageCallback で通知
    // dt <= 0 は no-op。
    void Tick(f32 dt) noexcept;

    // ---- callback ---------------------------------------------------------
    // nullptr で detach。user は所有しない (= 呼出側の責務)。
    void SetOnCriticalCallback(CriticalCallback cb, void* user) noexcept;
    void SetOnDamageCallback(DamageCallback cb, void* user) noexcept;

    // ---- 一括破棄 ---------------------------------------------------------
    // 全 survivor を破棄 + stat config を default に戻す。callback は保持
    // (= 結線は維持して entity だけリセットしたいことが多いため)。
    void ClearAll() noexcept;

private:
    // ---- 内部 survivor slot ----------------------------------------------
    // 各 survivor の StatState を 7 個固定で持つ。`in_use=false` の slot は
    // AddSurvivor で再利用される。gen は 1 以上で配り、0 は「未使用」を意味する。
    struct SurvivorSlot {
        TArray<StatState> stats {};
        u8               gen     = 0u;
        bool             in_use  = false;
    };

    // ---- 検索 -----------------------------------------------------------
    // survivor handle → SurvivorSlot* (gen 一致 + in_use + 範囲チェック)。
    // 失敗時は nullptr。const / non-const 二口。
    SurvivorSlot*       ResolveSurvivor(SurvivorId id) noexcept;
    const SurvivorSlot* ResolveSurvivor(SurvivorId id) const noexcept;

    static f32 Clamp(f32 v, f32 lo, f32 hi) noexcept;

    // stat enum を array index に変換 (範囲外は ~0u で哨兵)。
    static u32 StatIndex(ESurvivalStat stat) noexcept;

    // ---- 状態 -----------------------------------------------------------
    TArray<StatConfig>    _configs   {};   // 7 stat の共通 config
    TArray<SurvivorSlot>  _survivors {};   // SurvivorSlot 配列 (generational)
    u32                  _survivor_count = 0u;

    CriticalCallback     _on_critical       = nullptr;
    void*                _on_critical_user  = nullptr;
    DamageCallback       _on_damage         = nullptr;
    void*                _on_damage_user    = nullptr;
};

} // namespace acs::game

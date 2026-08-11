// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Array.h"
#include "foundation/Types.h"

namespace acs::game {

/**
 * 生存統計値の種別。
 *
 * @details
 * array index として直接使うため値は連続。Custom1 / Custom2 はゲーム固有
 * (例: Radiation / Oxygen / Bleeding) を載せる拡張枠。
 */
enum class ESurvivalStat : u8 {
    /** 空腹。 */
    Hunger  = 0,

    /** 喉の渇き。 */
    Thirst  = 1,

    /** 疲労 (= スタミナの長期版)。 */
    Energy  = 2,

    /** 正気度 (恐怖 / ホラー系)。 */
    Sanity  = 3,

    /** 体温 (寒冷地サバイバル)。 */
    Warmth  = 4,

    /** ゲーム固有 1 (例: Radiation)。 */
    Custom1 = 5,

    /** ゲーム固有 2 (例: Oxygen)。 */
    Custom2 = 6,
};

/** stat 種別の総数 (配列サイズや loop 終端に使う)。 */
inline constexpr u32 kSurvivalStatCount = 7u;

/**
 * stat 種別ごとの設定 (全 survivor で共通)。
 *
 * @details 個体差は将来 Modifier レイヤで載せる想定で、今は共通 config を採る。
 */
struct FStatConfig {
    /** stat の上限値 (= AddSurvivor 直後の初期値)。0 以下指定は防御的に 1.0 扱い。 */
    f32 max_value          = 100.0f;

    /** 1 秒あたりの自然減少量。0 以下なら decay しない (= 永続)。 */
    f32 decay_per_sec      = 0.0f;

    /**
     * critical 判定の閾値。
     *
     * @details
     * この値を下回ると is_critical=true になり CriticalCallback が発火。復帰時
     * (threshold より上に戻る) にも callback が発火する。max_value より大きい指定は
     * max_value で clamp。
     */
    f32 critical_threshold = 20.0f;

    /**
     * stat == 0 滞在中に 1 秒あたり CHealthSystem へ通知するダメージ量。
     *
     * @details 0 以下なら通知しない (= 「不快なだけで死なない」stat 用)。
     */
    f32 zero_damage_per_sec = 0.0f;
};

/** ある survivor の 1 stat の実状態。 */
struct FStatState {
    /** 現在値 ([0, max] でクランプ)。 */
    f32  current     = 0.0f;

    /** 上限値 (FStatConfig::max_value のコピー)。 */
    f32  max         = 0.0f;

    /** critical_threshold を下回っているか (Tick 内で更新される)。 */
    bool is_critical = false;
};

/**
 * 24bit index + 8bit gen を packed した opaque な survivor handle。
 *
 * @details
 * m_Packed == 0 を invalid と定義 (gen は常に 1 以上で配る)。FHealthId /
 * FBuffOwnerId と同一規約。
 */
struct FSurvivorId {
    /** index と gen を詰めた packed 値 (0 = invalid)。 */
    u32 m_Packed = 0u;

    /** index フィールドのビット幅。 */
    static constexpr u32 kIndexBits = 24u;

    /** index フィールドの抽出マスク (0x00FFFFFF)。 */
    static constexpr u32 kIndexMask = (1u << kIndexBits) - 1u;

    /** index の最大値 (16777215)。 */
    static constexpr u32 kMaxIndex  = kIndexMask;

    /**
     * 有効な handle かを返す。
     *
     * @return m_Packed が非 0 なら true。
     */
    bool IsValid() const noexcept { return m_Packed != 0u; }

    /**
     * index と gen から packed handle を生成する。
     *
     * @param index slot インデックス (下位 24bit)。
     * @param gen 世代番号 (上位 8bit)。
     * @return 生成した FSurvivorId。
     */
    static FSurvivorId Pack(u32 index, u8 gen) noexcept {
        FSurvivorId o;
        o.m_Packed = (static_cast<u32>(gen) << kIndexBits) | (index & kIndexMask);
        return o;
    }

    /**
     * slot インデックスを取り出す。
     *
     * @return 下位 24bit の index。
     */
    u32 Index() const noexcept { return m_Packed & kIndexMask; }

    /**
     * 世代番号を取り出す。
     *
     * @return 上位 8bit の gen。
     */
    u8  Gen()   const noexcept { return static_cast<u8>(m_Packed >> kIndexBits); }

    /**
     * 等価比較する。
     *
     * @param o 比較相手。
     * @return packed 値が一致すれば true。
     */
    bool operator==(FSurvivorId o) const noexcept { return m_Packed == o.m_Packed; }

    /**
     * 非等価比較する。
     *
     * @param o 比較相手。
     * @return packed 値が異なれば true。
     */
    bool operator!=(FSurvivorId o) const noexcept { return m_Packed != o.m_Packed; }
};

/**
 * 複数 survivor の生存統計値 (空腹 / 喉 / 疲労 / 正気 / 体温 / Custom) を管理する。
 *
 * @details
 * slot+gen パターンで survivor を管理し、Tick で各 stat を秒単位に decay させて
 * critical 閾値跨ぎを検出し、0 到達時は DamageCallback 経由で HP ダメージを通知する。
 * HP 自体は管理せず弱結合 bridge を取る。非コピー・非ムーブ (TArray<FSurvivorSlot> が
 * さらに TArray<FStatState> を持つ二段ネスト構造のため)。
 */
class CHungerSystem {
public:
    /**
     * critical 状態の遷移を通知するコールバックの型。
     *
     * @details 発火タイミングは Tick 内で stat の値が書き換わった直後。
     * @param user SetOnCriticalCallback で渡したコンテキストポインタ。
     * @param id 対象 survivor の handle。
     * @param stat 遷移した stat 種別。
     * @param entered_critical true で「閾値を割って入った」、false で「復帰した」。
     */
    using CriticalCallback = void(*)(void* user, FSurvivorId id, ESurvivalStat stat,
                                      bool entered_critical) noexcept;

    /**
     * stat=0 滞在中の HP ダメージを通知するコールバックの型。
     *
     * @details
     * Tick 内で dt × zero_damage_per_sec を計算して呼ぶ。bridge 実装側はここで
     * CHealthSystem::ApplyDamage を呼ぶ想定。1 フレで複数 stat 同時発火しうる。
     * @param user SetOnDamageCallback で渡したコンテキストポインタ。
     * @param id 対象 survivor の handle。
     * @param stat 0 に達した stat 種別。
     * @param damage 今フレームで適用すべきダメージ量。
     */
    using DamageCallback = void(*)(void* user, FSurvivorId id, ESurvivalStat stat,
                                    f32 damage) noexcept;

    /** 空状態で構築する (stat config は Init で確保)。 */
    CHungerSystem()  noexcept = default;

    /** 破棄する。 */
    ~CHungerSystem() noexcept = default;

    /** コピー禁止 (内部の二段ネスト TArray の所有を曖昧にしないため)。 */
    CHungerSystem(const CHungerSystem&)            = delete;

    /** コピー代入も禁止。 */
    CHungerSystem& operator=(const CHungerSystem&) = delete;

    /** ムーブ禁止。 */
    CHungerSystem(CHungerSystem&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CHungerSystem& operator=(CHungerSystem&&)      = delete;

    /**
     * 7 stat 分の FStatConfig をデフォルト値で初期化する。
     *
     * @details
     * コンストラクタでやらないのは Director パターンで明示的に Init/Shutdown を踏ませる
     * 方針に合わせるため。再呼び出しは全 survivor を破棄して config もリセットする。
     */
    void Init() noexcept;

    /**
     * stat 種別ごとの decay / critical / damage 設定を上書きする。
     *
     * @details
     * Init 後に呼ぶ。既存 survivor の FStatState には影響しないため、ゲーム起動時に
     * 一括設定する想定。
     * @param stat 設定する stat 種別。
     * @param config 上書きする設定値 (負値や max 超過は内部で sanitize される)。
     */
    void ConfigureStat(ESurvivalStat stat, const FStatConfig& config) noexcept;

    /**
     * 新規 survivor を登録する。
     *
     * @details 全 stat が config.max_value で初期化される。
     * @return 新しい survivor の handle (24bit index 上限到達時は invalid)。
     */
    FSurvivorId AddSurvivor() noexcept;

    /**
     * survivor を破棄する (= slot 解放 + gen を進める)。
     *
     * @details callback は発火しない。invalid / stale / 範囲外 handle は no-op。
     * @param id 破棄する survivor の handle。
     */
    void RemoveSurvivor(FSurvivorId id) noexcept;

    /**
     * stat を加算 (回復) する。
     *
     * @details 結果は clamp(0, max)。0 以下 amount / invalid id は no-op。
     * @param id 対象 survivor の handle。
     * @param stat 回復する stat 種別。
     * @param amount 加算量。
     */
    void RestoreStat(FSurvivorId id, ESurvivalStat stat, f32 amount) noexcept;

    /**
     * stat を減算 (消費) する。
     *
     * @details
     * 結果は clamp(0, max)。0 以下 amount / invalid id は no-op。critical 跨ぎ /
     * zero 到達があっても即時 callback はせず、次回 Tick で一元的に発火する
     * (再入回避)。
     * @param id 対象 survivor の handle。
     * @param stat 消費する stat 種別。
     * @param amount 減算量。
     */
    void DrainStat(FSurvivorId id, ESurvivalStat stat, f32 amount) noexcept;

    /**
     * stat を絶対値で設定する。
     *
     * @details 負値は 0 に、max 超過は max に clamp。invalid id は no-op。
     * @param id 対象 survivor の handle。
     * @param stat 設定する stat 種別。
     * @param value 設定する値。
     */
    void SetStat(FSurvivorId id, ESurvivalStat stat, f32 value) noexcept;

    /**
     * stat の現在値を返す。
     *
     * @param id 対象 survivor の handle。
     * @param stat 取得する stat 種別。
     * @return 現在値 (invalid id / 範囲外 stat は 0.0f)。
     */
    f32 GetStat(FSurvivorId id, ESurvivalStat stat) const noexcept;

    /**
     * stat の is_critical フラグを返す。
     *
     * @param id 対象 survivor の handle。
     * @param stat 判定する stat 種別。
     * @return critical 状態なら true (invalid id は false)。
     */
    bool IsCritical(FSurvivorId id, ESurvivalStat stat) const noexcept;

    /**
     * この survivor が「生きているか」を返す。
     *
     * @details
     * 全 stat が 0 ではない (= 1 個でも非 0) かどうかで判定する。HealthBridge が
     * セットされていれば HP > 0 も加味するが、現状は stat 部分のみで判定する。
     * @param id 対象 survivor の handle。
     * @return 生存していれば true (invalid / removed survivor は false)。
     */
    bool IsAlive(FSurvivorId id) const noexcept;

    /**
     * 総合生存度を [0, 1] で返す。
     *
     * @details 有効な max を持つ全 stat の (current / max) の平均値。
     * @param id 対象 survivor の handle。
     * @return 総合生存度 (invalid id / configure 済み stat が無い場合は 0.0f)。
     */
    f32 OverallSurvivalHealth(FSurvivorId id) const noexcept;

    /**
     * 全 active survivor 数を返す (生死問わず)。
     *
     * @return 現在登録中の survivor 数。
     */
    u32 SurvivorCount() const noexcept { return m_SurvivorCount; }

    /**
     * 全 survivor × 全 stat を dt 秒進める。
     *
     * @details
     * 各 stat について decay 適用 → clamp(0, max) → critical_threshold 跨ぎ検出で
     * CriticalCallback 発火 → stat==0 なら zero_damage_per_sec * dt を DamageCallback で
     * 通知、を行う。dt <= 0 は no-op。
     * @param dt 経過秒。
     */
    void Tick(f32 dt) noexcept;

    /**
     * critical コールバックを設定する。
     *
     * @param cb critical 遷移時に呼ぶコールバック (nullptr で detach)。
     * @param user cb に渡すコンテキストポインタ (所有しない)。
     */
    void SetOnCriticalCallback(CriticalCallback cb, void* user) noexcept;

    /**
     * damage コールバックを設定する。
     *
     * @param cb stat=0 ダメージ通知時に呼ぶコールバック (nullptr で detach)。
     * @param user cb に渡すコンテキストポインタ (所有しない)。
     */
    void SetOnDamageCallback(DamageCallback cb, void* user) noexcept;

    /**
     * 全 survivor を破棄し、stat config を default に戻す。
     *
     * @details callback は保持する (結線は維持して entity だけリセットする用途のため)。
     */
    void ClearAll() noexcept;

private:
    /**
     * 内部 survivor slot。
     *
     * @details
     * 各 survivor の FStatState を 7 個固定で持つ。in_use=false の slot は AddSurvivor で
     * 再利用される。gen は 1 以上で配り、0 は「未使用」を意味する。
     */
    struct FSurvivorSlot {
        /** この survivor の 7 stat の実状態。 */
        TArray<FStatState> stats {};

        /** 世代番号 (再利用検出用、0 は未使用)。 */
        u8               gen     = 0u;

        /** この slot が使用中か。 */
        bool             in_use  = false;
    };

    /**
     * survivor handle から slot を解決する (non-const)。
     *
     * @details gen 一致 + in_use + 範囲チェックを行う。
     * @param id 解決する survivor の handle。
     * @return 対応する slot へのポインタ (解決失敗時は nullptr)。
     */
    FSurvivorSlot*       ResolveSurvivor(FSurvivorId id) noexcept;

    /**
     * survivor handle から slot を解決する (const)。
     *
     * @details gen 一致 + in_use + 範囲チェックを行う。
     * @param id 解決する survivor の handle。
     * @return 対応する slot への const ポインタ (解決失敗時は nullptr)。
     */
    const FSurvivorSlot* ResolveSurvivor(FSurvivorId id) const noexcept;

    /**
     * 値を [lo, hi] に制限する。
     *
     * @param v 制限する値。
     * @param lo 下限。
     * @param hi 上限。
     * @return clamp した値。
     */
    static f32 Clamp(f32 v, f32 lo, f32 hi) noexcept;

    /**
     * stat enum を array index に変換する。
     *
     * @param stat 変換する stat 種別。
     * @return 配列インデックス (範囲外は ~0u で哨兵)。
     */
    static u32 StatIndex(ESurvivalStat stat) noexcept;

    /** 7 stat の共通 config。 */
    TArray<FStatConfig>    m_Configs   {};

    /** FSurvivorSlot 配列 (generational、index 0 は dummy)。 */
    TArray<FSurvivorSlot>  m_Survivors {};

    /** 現在登録中の active survivor 数。 */
    u32                  m_SurvivorCount = 0u;

    /** critical 遷移コールバック。 */
    CriticalCallback     m_OnCritical       = nullptr;

    /** critical コールバックに渡す user ポインタ。 */
    void*                m_OnCriticalUser  = nullptr;

    /** stat=0 ダメージ通知コールバック。 */
    DamageCallback       m_OnDamage         = nullptr;

    /** damage コールバックに渡す user ポインタ。 */
    void*                m_OnDamageUser    = nullptr;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FHungerSystem = CHungerSystem;

} // namespace acs::game

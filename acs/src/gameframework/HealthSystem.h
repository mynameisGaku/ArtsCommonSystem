// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R/I — CHealthSystem (HP / damage / death / respawn 管理)
//
// 複数 entity (敵 / プレイヤー / 破壊可能オブジェクト) の HP を一元管理する
// 高レベル API。slot+generation パターンの `FHealthId` で entity を識別し、
// ApplyDamage / Heal / Revive / SetInvulnerable などの操作を提供する。
//
// 使い方:
//   CHealthSystem hp;
//   FHealthId player = hp.Spawn(/*max_hp=*/100.0f);
//   FHealthId enemy  = hp.Spawn(50.0f);
//
//   // ダメージ適用 (戻り値 true = 致死)
//   if (hp.ApplyDamage(enemy, 30.0f, EDamageType::Fire)) {
//       // 死亡処理 (callback 経由でも通知)
//   }
//
//   // 無敵時間付与 (例: 被弾後の i-frame)
//   hp.SetInvulnerable(player, 0.5f);
//
//   // 毎フレーム invuln_timer を進める
//   hp.Tick(dt);
//
//   // 復活 (HP 半分で蘇生など)
//   hp.Revive(player, 0.5f);
//
// 設計選択 (Pillar R/I):
//   ・**FHealthId は 24bit idx + 8bit gen の packed u32**: CCollisionWorld2D の
//     FShapeId / ANode の FNodeId と同パターン。removed slot を再利用しても古い
//     handle は無効化される。0 は invalid 予約 (index 0 dummy)。
//   ・**slot 配列 + active フラグ**: AcquireSlot で空きを線形検索、無ければ末尾
//     拡張。Despawn では generation を進めて handle 無効化。
//   ・**DeathCallback fire は state 確定後**: ApplyDamage 内で hp<=0 にしてから
//     is_alive=false を確定し、その後 callback を発火。callback 内で同 entity
//     を Revive することも可能 (再入安全)。
//   ・**invuln 中の ApplyDamage は no-op で false 返却**: 即死扱いではなく
//     「ダメージ無し」を返す。caller がフィードバック (sound/spark) を出すなら
//     IsInvulnerable() で先に確認する。
//   ・**Heal は max_hp で clamp**: 死亡中 (is_alive=false) でも回復は受け付けるが、
//     is_alive は自動で true にならない (Revive 経由を強制)。
//   ・**Revive は is_alive=false 専用**: 生存中の Revive は no-op。hp_fraction で
//     復活時 HP 割合を指定 (0.0 = 1 HP に clamp、1.0 = full)。
//   ・**EDamageType は enum**: 属性 (Fire / Ice 等) は将来の耐性計算 / VFX 振り分け
//     用。本クラスでは値をそのまま受け取り callback に伝えるだけで、ダメージ倍率は
//     掛けない (caller が Resistance を考慮した最終量を渡す方針)。
//   ・**非コピー・非ムーブ**: CGame / AScene 単位で 1 個保持される想定で、所有権
//     移動は不要。
//
// 範囲外 (将来 Phase で):
//   ・自然回復 (regen) は本クラスでは扱わない (Tick 内で全 entity を回す方式は
//     pillar 越境になるため、caller が Heal を毎フレ呼ぶ方が柔軟)
//   ・DoT (継続ダメージ) 管理 — Status Effect モジュールで別途
//   ・ResistanceTable (EDamageType → 倍率) — 戦闘パラメータレイヤで別途
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * HP 管理対象 entity の識別子。
 *
 * @details
 * 32bit packed = 下位 24bit index + 上位 8bit generation。0 は invalid 予約
 * (index 0 dummy)。Despawn → slot 再利用しても、古い handle は generation 不一致で
 * 弾かれる。
 */
struct FHealthId {
    /** packed 値 (0 = invalid。layout: low24=index, high8=generation)。 */
    u32 m_Packed = 0;

    /** invalid (m_Packed == 0) な ID を構築する。 */
    constexpr FHealthId() noexcept = default;

    /**
     * index と generation を packing して ID を構築する。
     *
     * @param index slot インデックス (下位 24bit に格納)。
     * @param gen generation (上位 8bit に格納)。
     */
    constexpr FHealthId(u32 index, u8 gen) noexcept
        : m_Packed((index & 0x00FFFFFFu) | (static_cast<u32>(gen) << 24)) {}

    /**
     * slot インデックス (下位 24bit) を取り出す。
     *
     * @return slot インデックス。
     */
    constexpr u32  Index() const noexcept { return m_Packed & 0x00FFFFFFu; }

    /**
     * generation (上位 8bit) を取り出す。
     *
     * @return generation 値。
     */
    constexpr u8   Generation() const noexcept {
        return static_cast<u8>(m_Packed >> 24);
    }

    /**
     * 有効な ID かどうかを返す。
     *
     * @return m_Packed != 0 なら true。
     */
    constexpr bool IsValid() const noexcept { return m_Packed != 0; }

    /**
     * 2 つの ID が等しいかを返す。
     *
     * @param o 比較対象の ID。
     * @return packed 値が一致すれば true。
     */
    constexpr bool operator==(FHealthId o) const noexcept { return m_Packed == o.m_Packed; }

    /**
     * 2 つの ID が異なるかを返す。
     *
     * @param o 比較対象の ID。
     * @return packed 値が異なれば true。
     */
    constexpr bool operator!=(FHealthId o) const noexcept { return m_Packed != o.m_Packed; }
};

/**
 * 1 entity 分の HP 状態。
 *
 * @details GetState() で外部へ const 参照を返す。
 */
struct FHealthState {
    /** 現在 HP。0 以下になると is_alive=false に遷移する。 */
    f32  current_hp    = 0.0f;

    /** HP 上限。Heal の clamp に使い、SetMaxHp で更新できる。 */
    f32  max_hp        = 0.0f;

    /** 生存フラグ (true = 生存、false = 死亡。Revive で復帰可能)。 */
    bool is_alive      = false;

    /** 無敵フラグ (true 中は ApplyDamage が no-op で false 返却)。 */
    bool invulnerable  = false;

    /** 残り無敵時間 (秒)。Tick で減算し、0 到達で invulnerable=false。 */
    f32  invuln_timer  = 0.0f;
};

/**
 * ダメージ属性。
 *
 * @details
 * 耐性計算 / VFX 振り分けに使う識別子。本クラスはダメージ倍率を掛けないが、callback /
 * 履歴に種別として伝える。
 */
enum class EDamageType : u8 {
    /** 物理ダメージ。 */
    Physical  = 0,

    /** 炎ダメージ。 */
    Fire      = 1,

    /** 氷ダメージ。 */
    Ice       = 2,

    /** 雷ダメージ。 */
    Lightning = 3,

    /** 毒ダメージ。 */
    Poison    = 4,

    /** 耐性無効の確定ダメージ (デバッグ / 環境ダメージ用)。 */
    True      = 5,
};

/**
 * 死亡時コールバックの関数ポインタ型。
 *
 * @details
 * std::function 不使用ポリシーに従い void* user でコンテキストを引き回す。state 更新後
 * (is_alive=false 確定後) に呼ばれるため、callback 内で同 entity を Revive しても安全。
 * @param user SetOnDeathCallback で渡したコンテキスト。
 * @param id 死亡した entity の FHealthId。
 * @param lethal_type 致死を与えた EDamageType。
 */
using DeathCallback = void(*)(void* user, FHealthId id, EDamageType lethal_type) noexcept;

/**
 * 複数 entity の HP / ダメージ / 死亡 / 復活 / 無敵時間を一元管理する高レベル API。
 *
 * @details
 * slot+generation パターンの FHealthId で entity を識別し、ApplyDamage / Heal / Revive /
 * SetInvulnerable 等を提供する。死亡通知は DeathCallback で行う。CGame / AScene 単位で 1 個
 * 保持される想定の非コピー・非ムーブ型。
 */
class CHealthSystem {
public:
    /** 空の状態で構築する (entity なし、callback 未登録)。 */
    CHealthSystem() noexcept = default;

    /** 破棄する。 */
    ~CHealthSystem() noexcept = default;

    /** コピー禁止 (1 個保持を前提とし所有権移動を不要とするため)。 */
    CHealthSystem(const CHealthSystem&)            = delete;

    /** コピー代入も禁止。 */
    CHealthSystem& operator=(const CHealthSystem&) = delete;

    /** ムーブ禁止。 */
    CHealthSystem(CHealthSystem&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CHealthSystem& operator=(CHealthSystem&&)      = delete;

    /**
     * 新規 entity を full HP で登録する。
     *
     * @param max_hp HP 上限 (0 以下なら防御的に 1.0 にクランプ)。
     * @return 登録した entity の FHealthId。
     */
    FHealthId Spawn(f32 max_hp) noexcept;

    /**
     * entity を破棄する。
     *
     * @details slot は次の Spawn で generation を進めて再利用される。invalid id は no-op。
     * @param id 破棄する entity の ID。
     */
    void Despawn(FHealthId id) noexcept;

    /**
     * ダメージを適用する。
     *
     * @details
     * invuln 中 / 既に死亡中 / amount<=0 はいずれも HP 変動なしの no-op で false を返す。
     * 致死した場合は is_alive=false に遷移し DeathCallback を発火する。
     * @param id 対象 entity の ID。
     * @param amount ダメージ量 (倍率は掛けない。caller が最終量を渡す)。
     * @param type ダメージ属性 (callback に伝える。既定 Physical)。
     * @return この一撃で致死したら true、それ以外は false。
     */
    bool ApplyDamage(FHealthId id, f32 amount, EDamageType type = EDamageType::Physical) noexcept;

    /**
     * HP を回復する。
     *
     * @details max_hp で clamp する。死亡中でも HP は加算されるが is_alive は変動しない
     * (復活は Revive 経由のみ)。amount<=0 は no-op。
     * @param id 対象 entity の ID。
     * @param amount 回復量。
     */
    void Heal(FHealthId id, f32 amount) noexcept;

    /**
     * 一定時間 invulnerable にする。
     *
     * @details
     * Tick で減算し 0 到達で解除する。既に invuln 中の場合は max(現在残り, duration_sec) で
     * 延長する (短い無敵で上書きしない)。duration_sec<=0 で即解除。
     * @param id 対象 entity の ID。
     * @param duration_sec 無敵時間 (秒)。
     */
    void SetInvulnerable(FHealthId id, f32 duration_sec) noexcept;

    /**
     * 死亡状態から復活させる。
     *
     * @details 生存中なら no-op。hp_fraction は [0, 1] に clamp され、最低 1 HP は確保される。
     * @param id 対象 entity の ID。
     * @param hp_fraction 復活時の HP 割合 (0.0 でも最低 1 HP。既定 1.0 = full)。
     */
    void Revive(FHealthId id, f32 hp_fraction = 1.0f) noexcept;

    /**
     * HP 上限を変更する。
     *
     * @details new_max<=0 は 1.0 にクランプ。refill=true でも is_alive は据置 (復活は Revive 経由)。
     * @param id 対象 entity の ID。
     * @param new_max 新しい HP 上限。
     * @param refill true なら current_hp も new_max に揃える (full heal)、false なら据置で new_max に clamp。
     */
    void SetMaxHp(FHealthId id, f32 new_max, bool refill) noexcept;

    /**
     * HP 状態への const 参照を取得する。
     *
     * @param id 対象 entity の ID。
     * @return HP 状態。無効 id / Despawn 済 / generation 不一致なら nullptr。
     */
    const FHealthState* GetState(FHealthId id) const noexcept;

    /**
     * 現在 HP を取得する。
     *
     * @param id 対象 entity の ID。
     * @return 現在 HP。無効 id なら 0.0。
     */
    f32 GetCurrentHp(FHealthId id) const noexcept;

    /**
     * HP の割合 (current_hp / max_hp) を取得する。
     *
     * @param id 対象 entity の ID。
     * @return [0, 1] の HP 割合。無効 id or max_hp<=0 なら 0.0。
     */
    f32 GetHpFraction(FHealthId id) const noexcept;

    /**
     * 生存しているかを返す。
     *
     * @param id 対象 entity の ID。
     * @return 生存中なら true。無効 id は false。
     */
    bool IsAlive(FHealthId id) const noexcept;

    /**
     * 無敵中かを返す。
     *
     * @param id 対象 entity の ID。
     * @return invulnerable フラグの値 (生死は問わない)。無効 id は false。
     */
    bool IsInvulnerable(FHealthId id) const noexcept;

    /**
     * active な全 entity 数を返す。
     *
     * @return 生死問わず active な entity 数。
     */
    u32 EntityCount() const noexcept { return m_EntityCount; }

    /**
     * 生存 entity 数を返す。
     *
     * @return is_alive=true な entity 数。
     */
    u32 AliveCount() const noexcept;

    /**
     * invuln_timer を進める driver。
     *
     * @details 毎フレーム呼ぶ。タイマが 0 到達した entity は invulnerable=false に戻す。
     * @param dt 前フレームからの経過秒 (0 以下は no-op)。
     */
    void Tick(f32 dt) noexcept;

    /**
     * 死亡時コールバックを登録する。
     *
     * @details state 更新後 (is_alive=false 確定後) に呼ばれる。
     * @param cb 死亡時に呼ぶコールバック (nullptr で登録解除)。
     * @param user コールバックへ渡すコンテキスト。
     */
    void SetOnDeathCallback(DeathCallback cb, void* user) noexcept;

    /**
     * 全 entity を破棄する。
     *
     * @details slot 配列をクリアし entity 数を 0 に戻す。登録済み callback は保持する。
     */
    void ClearAll() noexcept;

private:
    /** 1 entity 分の slot (状態 + active フラグ + generation)。 */
    struct FSlot {
        /** entity の HP 状態。 */
        FHealthState state;

        /** 使用中フラグ (false なら空き slot)。 */
        bool        active = false;

        /** generation (Spawn ごとに進めて古い handle を無効化する)。 */
        u8          gen    = 0;
    };

    /**
     * 空き slot のインデックスを取得する。
     *
     * @details 空きを線形検索し、無ければ末尾を拡張する。index 0 は invalid 予約 dummy。
     * @return 確保した slot のインデックス。
     */
    u32 AcquireSlot() noexcept;

    /**
     * id に対応する slot を取得する。
     *
     * @param id 検索する entity の ID。
     * @return 有効なら slot へのポインタ、無効なら nullptr。
     */
    FSlot*       FindSlot(FHealthId id) noexcept;

    /**
     * id に対応する slot を取得する (const 版)。
     *
     * @param id 検索する entity の ID。
     * @return 有効なら slot への const ポインタ、無効なら nullptr。
     */
    const FSlot* FindSlot(FHealthId id) const noexcept;

    /**
     * 値を [lo, hi] に clamp する。
     *
     * @param v clamp する値。
     * @param lo 下限。
     * @param hi 上限。
     * @return [lo, hi] に収めた値。
     */
    static f32 Clamp(f32 v, f32 lo, f32 hi) noexcept;

    /** entity slot 配列 (index 0 は invalid 予約 dummy)。 */
    TArray<FSlot>    m_Slots;

    /** active な entity 数。 */
    u32            m_EntityCount = 0;

    /** 死亡時コールバック (未登録なら nullptr)。 */
    DeathCallback  m_OnDeath       = nullptr;

    /** 死亡時コールバックへ渡す user ポインタ。 */
    void*          m_OnDeathUser  = nullptr;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FHealthSystem = CHealthSystem;

} // namespace acs::game

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

/**
 * 武器のジャンル。
 *
 * @details
 * UI 表示 / アニメーション分岐 / damage type 判定等のためのメタ情報。機械的な
 * fire 挙動は FWeaponDef の各パラメタで完全に決まり、kind は参考値。
 */
enum class EWeaponKind : u8 {
    /** 単発ハンドガン。 */
    Pistol,

    /** フルオート / セミオート長銃。 */
    Rifle,

    /** ペレット拡散。 */
    Shotgun,

    /** 高ダメージ低 fire-rate。 */
    Sniper,

    /** 爆発物。 */
    RocketLauncher,

    /** 近接斬撃。 */
    Sword,

    /** 弓 (チャージはここでは未対応)。 */
    Bow,

    /** それ以外 (= 独自実装で kind 値を流用したい場合)。 */
    Custom,
};

/**
 * 武器 1 種類の不変定義。
 *
 * @details Register / Equip / AddReserveAmmo は id をキーに参照する。文字列は非所有 (寿命は呼出側)。
 */
struct FWeaponDef {
    /** 武器キー (Register / Equip / AddReserveAmmo のキー)。文字列リテラル想定。 */
    const char* id                = nullptr;

    /** UI 表示名 (非所有)。 */
    const char* display_name      = nullptr;

    /** 武器ジャンル (メタ情報、機械挙動には不使用)。 */
    EWeaponKind  kind              = EWeaponKind::Custom;

    /** 1 秒あたりの発射回数。<=0 は「1.0 sec/発」フォールバック扱い。 */
    f32         fire_rate_per_sec = 1.0f;

    /** reload 1 サイクルに要する秒数。<=0 は「即時 reload」扱い。 */
    f32         reload_sec        = 1.0f;

    /** マガジン容量 (1 発射 = mag_size から 1 消費)。0 は「マガジン非搭載」(= reserve から直接消費)。 */
    u32         mag_size          = 1u;

    /** 予備弾薬の上限 (AddReserveAmmo のクランプ用)。0 は「無制限」扱い (~0u と等価)。 */
    u32         max_reserve       = 0u;

    /** 1 発 1 ペレットあたりの基本ダメージ (FireCallback へ伝達)。 */
    f32         base_damage       = 1.0f;

    /** 拡散角度 (deg、片側)。FireCallback へ伝達のみ。 */
    f32         spread_deg        = 0.0f;

    /** 1 発で発射されるペレット数 (Shotgun 用)。0 は 1 として扱う。 */
    u32         pellets_per_shot  = 1u;

    /** 発射する弾種の id (非所有)。FireCallback へ伝達して呼出側が spawn 判定。 */
    const char* projectile_id     = nullptr;
};

/**
 * 現在装備中武器のランタイム状態。
 *
 * @details 主に UI 表示 / デバッグ用の snapshot。内部更新は CWeaponSystem が行う。
 */
struct FWeaponState {
    /** 装備中武器の id (= 内部 m_CurrentDef->id と同値)。 */
    const char* current_def_id      = nullptr;

    /** マガジン内の残弾。 */
    u32         ammo_in_mag         = 0u;

    /** 予備弾薬残量。 */
    u32         reserve_ammo        = 0u;

    /** この時刻まで TryFire は失敗する (= m_ElapsedTime >= next_fire_time で発射可)。 */
    f32         next_fire_time_sec  = 0.0f;

    /** reload 中フラグ。 */
    bool        reloading           = false;

    /** reload 完了までの残り秒数。 */
    f32         reload_remaining_sec = 0.0f;
};

/**
 * 発射時に呼ばれる callback 型。
 *
 * @details
 * 1 回の TryFire で 1 回だけ発火する (pellets_per_shot は引数で伝達するだけで、
 * 複数回呼ばない設計)。
 * @param user SetOnFireCallback で渡したコンテキスト (Manager は所有しない)。
 * @param projectile_id FWeaponDef::projectile_id (非所有)。
 * @param damage FWeaponDef::base_damage (1 ペレットあたり)。
 * @param spread_deg FWeaponDef::spread_deg (呼出側で乱数適用)。
 * @param pellets FWeaponDef::pellets_per_shot (1 以上)。
 */
using FireCallback = void(*)(void* user, const char* projectile_id, f32 damage,
                             f32 spread_deg, u32 pellets) noexcept;

/**
 * reload 完了時に呼ばれる callback 型。
 *
 * @param user SetOnReloadCompleteCallback で渡したコンテキスト。
 * @param weapon_id reload が完了した武器の id (非所有)。
 */
using ReloadCallback = void(*)(void* user, const char* weapon_id) noexcept;

/**
 * 1 entity ぶんの武器ロードアウト / 弾薬 / 発射制御をまとめた per-entity マネージャ。
 *
 * @details
 * 複数武器の定義を登録し、装備切替・発射・リロード・弾薬補給を管理する。Tick(dt)
 * で内部時計を進め、fire_rate / reload_sec に基づいて可否を判定する。発射と reload
 * 完了は callback で通知し、実際の projectile spawn / damage 適用は呼出側に委ねる。
 * non-copy / non-move で、callback の user ポインタとの参照競合を防ぐ。
 */
class CWeaponSystem {
public:
    /** 空状態で構築する (武器未登録、装備なし)。 */
    CWeaponSystem()  noexcept = default;

    /** 破棄する (TArray が内部リソースを解放)。 */
    ~CWeaponSystem() noexcept = default;

    /** コピー禁止 (callback の self ポインタとの競合を防ぐため)。 */
    CWeaponSystem(const CWeaponSystem&)            = delete;

    /** コピー代入も禁止。 */
    CWeaponSystem& operator=(const CWeaponSystem&) = delete;

    /** ムーブ禁止 (内部配列を指す m_CurrentDef ポインタの安定性を保つため)。 */
    CWeaponSystem(CWeaponSystem&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CWeaponSystem& operator=(CWeaponSystem&&)      = delete;

    /**
     * 武器定義を登録し、対応する reserve スロットを 0 で初期化する。
     *
     * @details
     * 同 id の 2 重登録は WARN + no-op、def.id == nullptr も no-op。Add で
     * 配列が再確保された場合は装備中の m_CurrentDef を slot index から張り直す。
     * @param def 登録する武器定義 (文字列は呼出側で長寿命を保証すること)。
     */
    void RegisterWeapon(const FWeaponDef& def) noexcept;

    /**
     * 指定 id の武器を装備する。
     *
     * @details
     * reload 中でも遮らずに切替可能で、元武器の reload は破棄される (ammo_in_mag は
     * reload 開始前の値のまま保持)。成功時に内部状態を新武器の保存値から復元する
     * (別武器のときだけ next_fire_time をリセット)。初装備で完全 zero-state の武器は
     * mag_size まで自動充填される。
     * @param weapon_id 装備する武器の id。
     * @return 登録済みで装備に成功したら true、未登録 id / nullptr なら false。
     */
    bool EquipWeapon(const char* weapon_id) noexcept;

    /**
     * 発射を試みる。
     *
     * @details
     * fire_rate / mag / reload チェックを通れば弾を 1 発消費し、next_fire_time を
     * 更新して FireCallback を発火する。mag_size == 0 の武器は reserve から直接 1 発
     * 消費する。失敗 (装備なし / reload 中 / 連射間隔未到 / 弾切れ) は side effect なし。
     * @return 発射に成功したら true、失敗なら false。
     */
    bool TryFire() noexcept;

    /**
     * リロードを開始する。
     *
     * @details
     * 装備なし / reload 中 / mag 満タン / reserve 0 / マガジン非搭載 のいずれかなら
     * no-op。reload_sec <= 0 は「即時 reload」として内部で完了処理まで行う。
     */
    void StartReload() noexcept;

    /** reload 中なら remaining=0 / reloading=false にして打ち切る (ammo は変化しない)。 */
    void CancelReload() noexcept;

    /**
     * 指定武器の予備弾薬を加算する。
     *
     * @details
     * reserve_ammo に amount を加算 (max_reserve でクランプ、0=無制限)。装備中武器なら
     * _state 側にも反映する。未登録 weapon_id / nullptr / amount==0 は no-op。
     * @param weapon_id 補給する武器の id。
     * @param amount 加算する弾数。
     */
    void AddReserveAmmo(const char* weapon_id, u32 amount) noexcept;

    /**
     * 装備中武器のマガジン内残弾を返す。
     *
     * @return ammo_in_mag。
     */
    u32 GetAmmoInMag()  const noexcept { return _state.ammo_in_mag; }

    /**
     * 装備中武器の予備弾薬残量を返す。
     *
     * @return reserve_ammo。
     */
    u32 GetReserveAmmo() const noexcept { return _state.reserve_ammo; }

    /**
     * 装備中武器の定義を返す。
     *
     * @return 装備中武器の定義 (EquipWeapon 成功後でないと nullptr)。
     */
    const FWeaponDef* CurrentDef() const noexcept { return m_CurrentDef; }

    /**
     * 装備中武器のランタイム状態への const 参照を返す。
     *
     * @return FWeaponState への const 参照。
     */
    const FWeaponState& State() const noexcept { return _state; }

    /**
     * reload 中かどうかを返す。
     *
     * @return reload 中なら true。
     */
    bool IsReloading() const noexcept { return _state.reloading; }

    /**
     * reload 進行度を返す。
     *
     * @return reload 進行度 [0, 1]。reload 中でないときは 0.0、完了直後は 1.0。
     */
    f32 ReloadProgress() const noexcept;

    /**
     * 発射 callback を設定する。
     *
     * @details cb = nullptr で detach。user は所有しない (= 呼出側の責務)。
     * @param cb 発射時に呼ぶ callback (nullptr で解除)。
     * @param user callback に渡すコンテキスト。
     */
    void SetOnFireCallback(FireCallback cb, void* user) noexcept {
        m_OnFire = cb;
        m_OnFireUser = user;
    }

    /**
     * reload 完了 callback を設定する。
     *
     * @details cb = nullptr で detach。user は所有しない (= 呼出側の責務)。
     * @param cb reload 完了時に呼ぶ callback (nullptr で解除)。
     * @param user callback に渡すコンテキスト。
     */
    void SetOnReloadCompleteCallback(ReloadCallback cb, void* user) noexcept {
        m_OnReload = cb;
        m_OnReloadUser = user;
    }

    /**
     * 内部時計を進め、reload 進行を更新する。
     *
     * @details
     * dt <= 0 は無視。内部時計に dt を加算し、reload 中なら reload_remaining_sec を
     * 減算、0 到達で mag に reserve を補填して ReloadCallback を発火する。
     * @param dt 経過秒。
     */
    void Tick(f32 dt) noexcept;

    /** 武器定義 / reserve / current state / コールバックをすべてクリアする (デバッグ / シーン切替用)。 */
    void ClearAll() noexcept;

private:
    /**
     * per-weapon の予備弾薬 / mag を保持する並行スロット。
     *
     * @details 武器定義 m_Defs[i] に対して m_Reserves[i] が対応する。
     */
    struct FReserveSlot {
        /** m_Defs[i].id へのコピー (非所有、寿命は呼出側)。 */
        const char* weapon_id    = nullptr;

        /** 予備弾薬残量。 */
        u32         reserve_ammo = 0u;

        /** 装備外時の mag 保存 (装備切替時の継続のため)。 */
        u32         ammo_in_mag  = 0u;
    };

    /**
     * 武器 id を内部配列位置へ変換する (per-byte 線形検索)。
     *
     * @param weapon_id 探す武器の id。
     * @return 見つかった slot の index、未検出は ~0u。
     */
    u32 FindWeaponSlot(const char* weapon_id) const noexcept;

    /** reload を完了させる (mag に reserve を補填 + ReloadCallback 発火)。 */
    void CompleteReload() noexcept;

    /** 装備中武器の per-weapon 状態を m_Reserves[] へ書き戻す (装備切替前に呼ぶ)。 */
    void SaveCurrentToSlot() noexcept;

    /** 武器定義 (id 線形検索)。 */
    TArray<FWeaponDef>   m_Defs;

    /** m_Defs[i] と並行な per-weapon reserve スロット。 */
    TArray<FReserveSlot> m_Reserves;

    /** 装備中武器の定義 (Equip 前は nullptr)。 */
    const FWeaponDef* m_CurrentDef  = nullptr;

    /** m_Defs / m_Reserves の index (Equip 前は ~0u)。 */
    u32              m_CurrentSlot = ~0u;

    /** 装備中のランタイム状態。 */
    FWeaponState      _state{};

    /** Tick で累積する内部時計 (next_fire_time との比較に使用)。 */
    f32 m_ElapsedTime = 0.0f;

    /** 発射 callback (未設定なら nullptr)。 */
    FireCallback   m_OnFire        = nullptr;

    /** 発射 callback に渡すコンテキスト。 */
    void*          m_OnFireUser   = nullptr;

    /** reload 完了 callback (未設定なら nullptr)。 */
    ReloadCallback m_OnReload      = nullptr;

    /** reload 完了 callback に渡すコンテキスト。 */
    void*          m_OnReloadUser = nullptr;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FWeaponSystem = CWeaponSystem;

} // namespace acs::game

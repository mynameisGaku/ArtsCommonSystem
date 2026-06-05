// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar I/R — FWeaponSystem (武器切替 + 弾薬 + 連射制御)
//
// 1 entity (= 1 player or 1 NPC) ぶんの武器ロードアウト / 弾薬 / 発射制御を
// まとめた小型マネージャ。`Tick(dt)` で内部時計を進め、`fire_rate_per_sec` /
// `reload_sec` に基づいて発射 / リロードの可否を判定する。複数 entity 同時
// 管理は外側で複数 instance を持つ設計 (= manager 自体は per-entity)。
//
// 想定する位置付け:
//   ・Pillar I (Combat) と Pillar R (Skills/Cooldowns) の橋渡し。武器毎の
//     fire rate は FCooldownTimer と同等の役目だが、武器切替 / マガジン /
//     reserve ammo / スプレッド / ペレット数 を一体で持つため、FCooldownTimer
//     とは別 API として独立させる。
//   ・FireCallback で「弾を出せ」のイベントだけを通知し、実際の projectile
//     spawn / damage 適用は呼出側 (= Pillar I FCombatStateMachine や独自
//     Projectile manager) で行う設計。FWeaponSystem は時系列と弾薬数の管理に
//     責務を限定する。
//
// 使い方:
//   acs::game::FWeaponSystem ws;
//
//   // 武器定義 (文字列リテラルは caller 側で長寿命を保証)。
//   ws.RegisterWeapon({ "pistol", "9mm Pistol",
//                       acs::game::EWeaponKind::Pistol,
//                       /*fire_rate*/ 6.0f, /*reload*/ 1.4f,
//                       /*mag*/ 12, /*reserve*/ 48,
//                       /*dmg*/ 18.0f, /*spread*/ 1.5f,
//                       /*pellets*/ 1, "proj.bullet_9mm" });
//   ws.RegisterWeapon({ "shotgun", "Pump Shotgun",
//                       acs::game::EWeaponKind::Shotgun,
//                       1.0f, 2.6f, 6, 24,
//                       12.0f, 6.0f, 8, "proj.pellet_12g" });
//
//   ws.AddReserveAmmo("pistol",  48);
//   ws.AddReserveAmmo("shotgun", 24);
//   ws.EquipWeapon("pistol");
//   ws.SetOnFireCallback(&OnFire, user);
//   ws.SetOnReloadCompleteCallback(&OnReload, user);
//
//   void Update(f32 dt) {
//       ws.Tick(dt);
//       if (input.JustPressed(EKey::Mouse0)) ws.TryFire();
//       if (input.JustPressed(EKey::R))      ws.StartReload();
//       if (input.JustPressed(EKey::Num1))   ws.EquipWeapon("pistol");
//       if (input.JustPressed(EKey::Num2))   ws.EquipWeapon("shotgun");
//   }
//
// 設計選択:
//   ・Registry は TArray<FWeaponDef>: 武器数は per-entity でせいぜい 5〜10、
//     線形検索で十分。const char* per-byte 比較。重複登録は WARN + no-op。
//   ・reserve は並行 TArray で per-weapon 管理: 弾薬は武器毎に independent な
//     ため、weapon_id をキーに { weapon_id, reserve_ammo, ammo_in_mag } を保持
//     する。EquipWeapon で current_def に切り替えた後、ammo_in_mag / reserve_ammo
//     をその武器の slot から復元する設計。
//   ・装填中の武器切替は遮らずに許可: reload 中でも別武器に切り替えられ、切替の
//     瞬間に元武器の reload はキャンセルされる。ammo_in_mag は reload 開始前の値
//     のまま保持される (= reload は途中なので無効化)。
//   ・fire_rate_per_sec: 1 秒間の発射回数。連射間隔 = 1.0 / fire_rate。0 以下は
//     1.0 sec/発 のフォールバックを使う (NaN / 0 除算回避)。
//   ・reload_sec: reload に要する時間。0 以下は「即時 reload」扱い。StartReload は
//     reserve から min(mag_size - mag, reserve) を予約し、reload_remaining_sec を
//     進めて 0 で実際に ammo_in_mag に加算する設計。
//   ・コールバック: FireCallback (= 1 発分の damage / spread / pellets を通知) と
//     ReloadCallback (= reload 完了通知) の 2 種類。C 関数ポインタ + void* user。
//     複数 listener が必要なら呼出側で fan-out。
//   ・spread / pellets: FWeaponDef で固定値を持ち、TryFire 時に FireCallback へ
//     そのまま渡す (= 武器側の仕様)。実際の乱数 spread 適用は呼出側で行う。
//   ・全 noexcept、非コピー・非ムーブ: 他 Manager 系と統一。
//   ・STL 不使用、`<string>` 禁止: const char* 非所有のみ。
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
 * @details 主に UI 表示 / デバッグ用の snapshot。内部更新は FWeaponSystem が行う。
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
class FWeaponSystem {
public:
    /** 空状態で構築する (武器未登録、装備なし)。 */
    FWeaponSystem()  noexcept = default;

    /** 破棄する (TArray が内部リソースを解放)。 */
    ~FWeaponSystem() noexcept = default;

    /** コピー禁止 (callback の self ポインタとの競合を防ぐため)。 */
    FWeaponSystem(const FWeaponSystem&)            = delete;

    /** コピー代入も禁止。 */
    FWeaponSystem& operator=(const FWeaponSystem&) = delete;

    /** ムーブ禁止 (内部配列を指す m_CurrentDef ポインタの安定性を保つため)。 */
    FWeaponSystem(FWeaponSystem&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FWeaponSystem& operator=(FWeaponSystem&&)      = delete;

    /**
     * 武器定義を登録し、対応する reserve スロットを 0 で初期化する。
     *
     * @details
     * 同 id の 2 重登録は WARN + no-op、def.id == nullptr も no-op。PushBack で
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
    struct ReserveSlot {
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
    TArray<ReserveSlot> m_Reserves;

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

} // namespace acs::game

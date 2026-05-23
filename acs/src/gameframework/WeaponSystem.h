// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar I/R — WeaponSystem (武器切替 + 弾薬 + 連射制御)
//
// 1 entity (= 1 player or 1 NPC) ぶんの武器ロードアウト / 弾薬 / 発射制御を
// まとめた小型マネージャ。`Tick(dt)` で内部時計を進め、`fire_rate_per_sec` /
// `reload_sec` に基づいて発射 / リロードの可否を判定する。複数 entity 同時
// 管理は外側で複数 instance を持つ設計 (= manager 自体は per-entity)。
//
// 想定する位置付け:
//   ・Pillar I (Combat) と Pillar R (Skills/Cooldowns) の橋渡し。武器毎の
//     fire rate は CooldownTimer と同等の役目だが、武器切替 / マガジン /
//     reserve ammo / スプレッド / ペレット数 を一体で持つため、CooldownTimer
//     とは別 API として独立させる。
//   ・FireCallback で「弾を出せ」のイベントだけを通知し、実際の projectile
//     spawn / damage 適用は呼出側 (= Pillar I CombatStateMachine や独自
//     Projectile manager) で行う設計。WeaponSystem は時系列と弾薬数の管理に
//     責務を限定する。
//
// 使い方:
//   acs::game::WeaponSystem ws;
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
//   ・**Registry は Array<WeaponDef>**: 武器数は per-entity でせいぜい 5〜10、
//     線形検索で十分。const char* per-byte 比較 (Entitlement / EconomyDirector
//     と同設計)。重複登録は WARN + 黙って no-op。
//   ・**reserve は別 Array で per-weapon 管理**: 弾薬は武器毎に independent な
//     ため、weapon_id をキーに `{ weapon_id, reserve_ammo }` を保持する。
//     EquipWeapon で current_def に切り替えた後、ammo_in_mag / reserve_ammo を
//     その武器の slot から復元する設計。
//   ・**装填中の武器切替は遮らずに許可**: 仕様通り「reload 中でも別武器に
//     切り替えられる」= 切替の瞬間に元武器の reload はキャンセル + 装填済み
//     ammo は既に mag に入っていない (= reload は途中なので無効化)。reload
//     remaining は破棄、ammo_in_mag はそのまま (= reload 開始時の値)。
//   ・**fire_rate_per_sec**: 1 秒間の発射回数。連射間隔 = 1.0 / fire_rate。
//     0 以下は「単発のみ」(= 連射防止) として扱い、TryFire 1 回ごとに
//     `next_fire_time = elapsed + ∞ 相当` とはせず、`next_fire_time = elapsed +
//     1.0` (= 1 秒に 1 発) のフォールバックを使う (NaN / 0 除算回避)。
//   ・**reload_sec**: reload に要する時間。0 以下は「即時 reload」扱い。
//     StartReload は reserve から `min(mag_size - mag, reserve)` を予約し、
//     reload_remaining_sec を進めて 0 で実際に ammo_in_mag に加算する設計。
//   ・**コールバック**: FireCallback (= 1 発分の damage / spread / pellets を
//     通知) と ReloadCallback (= reload 完了通知) の 2 種類。STL <functional>
//     禁止のため C 関数ポインタ + void* user。複数 listener が必要なら呼出側
//     で fan-out。
//   ・**spread / pellets**: WeaponDef で固定値を持ち、TryFire 時に
//     FireCallback へそのまま渡す (= 武器側の仕様)。実際の乱数 spread 適用は
//     呼出側で `Random::UniformInRange(-spread, +spread)` 等を行う設計。
//   ・**全 noexcept、非コピー・非ムーブ**: 他 Manager 系と統一。
//   ・**STL 不使用、`<string>` 禁止**: const char* 非所有のみ。
//
// 範囲外 (Phase 2+ で):
//   ・武器熱量 (overheat) / charge weapon (タメ攻撃) — 別 manager で拡張。
//   ・武器アタッチメント (スコープ / マズル) — Pillar I のオプション拡張で。
//   ・武器スワップアニメーション同期 — Pillar G AnimationCurve と統合。
//   ・近接武器のヒットボックス / リーチ — Pillar P CollisionWorld2D 側で。
//   ・残弾の永続化 (Save/Load) — Pillar J Serialize と統合。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// ---- EWeaponKind: 武器のジャンル ------------------------------------------
// UI 表示 / アニメーション分岐 / damage type 判定等のためのメタ情報。
// 機械的な fire 挙動は WeaponDef の各パラメタで完全に決まり、`kind` は参考値。
enum class EWeaponKind : u8 {
    Pistol,         // 単発ハンドガン
    Rifle,          // フルオート / セミオート長銃
    Shotgun,        // ペレット拡散
    Sniper,         // 高ダメージ低 fire-rate
    RocketLauncher, // 爆発物
    Sword,          // 近接斬撃
    Bow,            // 弓 (チャージはここでは未対応、Phase 2+)
    Custom,         // それ以外 (= 独自実装で kind 値を流用したい場合)
};

// ---- WeaponDef: 武器 1 種類の不変定義 ------------------------------------
// id                 : 武器キー (Register / Equip / AddReserveAmmo のキー)。文字列リテラル想定。
// display_name       : UI 表示名 (非所有)。
// kind               : 武器ジャンル (メタ情報、機械挙動には不使用)。
// fire_rate_per_sec  : 1 秒あたりの発射回数。<=0 は「1.0 sec/発」フォールバック扱い。
// reload_sec         : reload 1 サイクルに要する秒数。<=0 は「即時 reload」扱い。
// mag_size           : マガジン容量 (1 発射 = mag_size から 1 消費)。0 は「マガジン非搭載」(= reserve から直接消費) 扱い。
// max_reserve        : 予備弾薬の上限 (AddReserveAmmo のクランプ用)。0 は「無制限」扱い (~0u と等価)。
// base_damage        : 1 発 1 ペレットあたりの基本ダメージ (FireCallback へ伝達)。
// spread_deg         : 拡散角度 (deg、片側)。FireCallback へ伝達のみ。
// pellets_per_shot   : 1 発で発射されるペレット数 (Shotgun 用)。0 は 1 として扱う。
// projectile_id      : 発射する弾種の id (非所有)。FireCallback へ伝達して呼出側が spawn 判定。
struct WeaponDef {
    const char* id                = nullptr;
    const char* display_name      = nullptr;
    EWeaponKind  kind              = EWeaponKind::Custom;
    f32         fire_rate_per_sec = 1.0f;
    f32         reload_sec        = 1.0f;
    u32         mag_size          = 1u;
    u32         max_reserve       = 0u;     // 0 = 無制限
    f32         base_damage       = 1.0f;
    f32         spread_deg        = 0.0f;
    u32         pellets_per_shot  = 1u;
    const char* projectile_id     = nullptr;
};

// ---- WeaponState: 現在装備中武器のランタイム状態 -------------------------
// 主に UI 表示 / デバッグ用の snapshot。内部更新は WeaponSystem が行う。
struct WeaponState {
    const char* current_def_id      = nullptr; // 装備中武器の id (= 内部 _current_def->id と同値)
    u32         ammo_in_mag         = 0u;      // マガジン内の残弾
    u32         reserve_ammo        = 0u;      // 予備弾薬残量
    f32         next_fire_time_sec  = 0.0f;    // この時刻まで TryFire は失敗する (= _elapsed >= next_fire_time で発射可)
    bool        reloading           = false;   // reload 中フラグ
    f32         reload_remaining_sec = 0.0f;   // reload 完了までの残り秒数
};

// 発射時に呼ばれる callback。1 回の TryFire で 1 回だけ発火する
// (pellets_per_shot は引数で伝達するだけで、複数回呼ばない設計)。
//   user          : SetOnFireCallback で渡したコンテキスト (Manager は所有しない)
//   projectile_id : WeaponDef::projectile_id (非所有)
//   damage        : WeaponDef::base_damage (1 ペレットあたり)
//   spread_deg    : WeaponDef::spread_deg (呼出側で乱数適用)
//   pellets       : WeaponDef::pellets_per_shot (1 以上)
using FireCallback = void(*)(void* user, const char* projectile_id, f32 damage,
                             f32 spread_deg, u32 pellets) noexcept;

// reload 完了時に呼ばれる callback。
//   user      : SetOnReloadCompleteCallback で渡したコンテキスト
//   weapon_id : reload が完了した武器の id (非所有)
using ReloadCallback = void(*)(void* user, const char* weapon_id) noexcept;

// ---- WeaponSystem ---------------------------------------------------------
class WeaponSystem {
public:
    WeaponSystem()  noexcept = default;
    ~WeaponSystem() noexcept = default;

    // 非コピー・非ムーブ (callback の self ポインタとの競合を防ぐ)
    WeaponSystem(const WeaponSystem&)            = delete;
    WeaponSystem& operator=(const WeaponSystem&) = delete;
    WeaponSystem(WeaponSystem&&)                 = delete;
    WeaponSystem& operator=(WeaponSystem&&)      = delete;

    // ---- 武器定義 ---------------------------------------------------------
    // 同 id の 2 重登録は no-op (WARN)。`def.id == nullptr` も no-op。
    // 同時に reserve スロットも 0 で初期化する。
    void RegisterWeapon(const WeaponDef& def) noexcept;

    // ---- 武器切替 ---------------------------------------------------------
    // 未登録 id / nullptr は false。reload 中でも遮らずに切替可能 (元武器の
    // reload は破棄される = ammo_in_mag は reload 開始前の値のままで保持)。
    // 成功時に _current_state を新武器の保存値から復元する。
    bool EquipWeapon(const char* weapon_id) noexcept;

    // ---- 発射 -------------------------------------------------------------
    // fire_rate / mag / reload チェックを通れば ammo_in_mag-- + next_fire_time
    // 更新 + FireCallback 発火、true を返す。失敗 (装備なし / reload 中 /
    // 連射間隔未到 / mag 空) は false で side effect なし。
    // mag_size == 0 の武器は reserve から直接 1 発消費する。
    bool TryFire() noexcept;

    // ---- リロード ---------------------------------------------------------
    // 装備なし / reload 中 / mag 満タン / reserve 0 のいずれかなら no-op。
    // reload_sec <= 0 は「即時 reload」として StartReload 内で完了処理まで行う。
    void StartReload() noexcept;

    // reload 中なら remaining=0 / reloading=false にして打ち切る。ammo は変化しない。
    void CancelReload() noexcept;

    // ---- 弾薬補給 ---------------------------------------------------------
    // weapon_id の reserve_ammo に amount を加算 (max_reserve でクランプ)。
    // 未登録 weapon_id / nullptr / amount==0 は no-op。
    void AddReserveAmmo(const char* weapon_id, u32 amount) noexcept;

    // ---- 状態照会 ---------------------------------------------------------
    u32 GetAmmoInMag()  const noexcept { return _state.ammo_in_mag; }
    u32 GetReserveAmmo() const noexcept { return _state.reserve_ammo; }

    // 装備中武器の定義 (`EquipWeapon` 成功後でないと nullptr)。
    const WeaponDef* CurrentDef() const noexcept { return _current_def; }
    const WeaponState& State() const noexcept { return _state; }

    bool IsReloading() const noexcept { return _state.reloading; }

    // reload 進行度 [0, 1]。reload 中でないときは 0.0、完了直後は 1.0。
    f32 ReloadProgress() const noexcept;

    // ---- コールバック -----------------------------------------------------
    // cb = nullptr で detach。user は所有しない (= 呼出側の責務)。
    void SetOnFireCallback(FireCallback cb, void* user) noexcept {
        _on_fire = cb;
        _on_fire_user = user;
    }

    void SetOnReloadCompleteCallback(ReloadCallback cb, void* user) noexcept {
        _on_reload = cb;
        _on_reload_user = user;
    }

    // ---- フレーム更新 -----------------------------------------------------
    // dt <= 0 は無視。_elapsed_time に dt を加算し、reload 中なら
    // reload_remaining_sec を減算、0 到達で mag に reserve を補填して
    // ReloadCallback を発火する (1 回の Tick 内で 1 回だけ)。
    void Tick(f32 dt) noexcept;

    // ---- 全リセット (デバッグ / シーン切替時) -----------------------------
    // 武器定義 / reserve / current state / コールバックすべてクリア。
    void ClearAll() noexcept;

private:
    // 並行 Array で per-weapon の reserve_ammo を保持する。
    // (= 武器定義 _defs[i] に対して _reserves[i] が対応)
    struct ReserveSlot {
        const char* weapon_id    = nullptr; // _defs[i].id へのコピー (非所有、寿命は呼出側)
        u32         reserve_ammo = 0u;
        u32         ammo_in_mag  = 0u;      // 装備外時の mag 保存 (装備切替時の継続のため)
    };

    // 武器 id → 内部配列位置の per-byte 線形検索。未検出は ~0u。
    u32 FindWeaponSlot(const char* weapon_id) const noexcept;

    // reload を完了させる内部 helper (mag に reserve を補填 + callback)。
    void CompleteReload() noexcept;

    // 装備中武器の per-weapon 状態を _reserves[] へ書き戻す (装備切替前)。
    void SaveCurrentToSlot() noexcept;

    Array<WeaponDef>   _defs;       // 武器定義 (id 線形検索)
    Array<ReserveSlot> _reserves;   // _defs[i] と並行

    const WeaponDef* _current_def  = nullptr; // 装備中武器の定義 (Equip 前は nullptr)
    u32              _current_slot = ~0u;     // _defs / _reserves の index (Equip 前は ~0u)
    WeaponState      _state{};                // 装備中のランタイム状態

    f32 _elapsed_time = 0.0f; // Tick で累積する内部時計 (next_fire_time との比較に使用)

    FireCallback   _on_fire        = nullptr;
    void*          _on_fire_user   = nullptr;
    ReloadCallback _on_reload      = nullptr;
    void*          _on_reload_user = nullptr;
};

} // namespace acs::game

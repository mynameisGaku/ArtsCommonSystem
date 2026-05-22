// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R/I — HealthSystem (HP / damage / death / respawn 管理)
//
// 複数 entity (敵 / プレイヤー / 破壊可能オブジェクト) の HP を一元管理する
// 高レベル API。slot+generation パターンの `HealthId` で entity を識別し、
// ApplyDamage / Heal / Revive / SetInvulnerable などの操作を提供する。
//
// 使い方:
//   HealthSystem hp;
//   HealthId player = hp.Spawn(/*max_hp=*/100.0f);
//   HealthId enemy  = hp.Spawn(50.0f);
//
//   // ダメージ適用 (戻り値 true = 致死)
//   if (hp.ApplyDamage(enemy, 30.0f, DamageType::Fire)) {
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
//   ・**HealthId は 24bit idx + 8bit gen の packed u32**: CollisionWorld2D の
//     ShapeId / Node2D の NodeId と同パターン。removed slot を再利用しても古い
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
//   ・**DamageType は enum**: 属性 (Fire / Ice 等) は将来の耐性計算 / VFX 振り分け
//     用。本クラスでは値をそのまま受け取り callback に伝えるだけで、ダメージ倍率は
//     掛けない (caller が Resistance を考慮した最終量を渡す方針)。
//   ・**非コピー・非ムーブ**: Game / Scene 単位で 1 個保持される想定で、所有権
//     移動は不要。
//
// 範囲外 (将来 Phase で):
//   ・自然回復 (regen) は本クラスでは扱わない (Tick 内で全 entity を回す方式は
//     pillar 越境になるため、caller が Heal を毎フレ呼ぶ方が柔軟)
//   ・DoT (継続ダメージ) 管理 — Status Effect モジュールで別途
//   ・ResistanceTable (DamageType → 倍率) — 戦闘パラメータレイヤで別途
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

// HP 管理対象 entity の識別子。32bit packed = 24bit index + 8bit generation。
// 0 は invalid 予約 (index 0 dummy)。Despawn → 再利用しても古い handle は
// generation 不一致で弾かれる。
struct HealthId {
    u32 _packed = 0;   // 0 = invalid。layout: low24=index, high8=generation

    constexpr HealthId() noexcept = default;
    constexpr HealthId(u32 index, u8 gen) noexcept
        : _packed((index & 0x00FFFFFFu) | (static_cast<u32>(gen) << 24)) {}

    constexpr u32  Index() const noexcept { return _packed & 0x00FFFFFFu; }
    constexpr u8   Generation() const noexcept {
        return static_cast<u8>(_packed >> 24);
    }
    constexpr bool IsValid() const noexcept { return _packed != 0; }

    constexpr bool operator==(HealthId o) const noexcept { return _packed == o._packed; }
    constexpr bool operator!=(HealthId o) const noexcept { return _packed != o._packed; }
};

// 1 entity 分の HP 状態。GetState() で外部へ const 参照を返す。
//   current_hp     : 現在 HP。0 以下になると is_alive=false に遷移。
//   max_hp         : HP 上限。Heal の clamp、SetMaxHp で更新可能。
//   is_alive       : true = 生存、false = 死亡 (Revive で復帰可能)。
//   invulnerable   : true = 無敵中 (ApplyDamage が no-op で false 返却)。
//   invuln_timer   : 残り無敵時間 (秒)。Tick で減算、0 到達で invulnerable=false。
struct HealthState {
    f32  current_hp    = 0.0f;
    f32  max_hp        = 0.0f;
    bool is_alive      = false;
    bool invulnerable  = false;
    f32  invuln_timer  = 0.0f;
};

// ダメージ属性。耐性計算 / VFX 振り分けに使う識別子。
// 本クラスはダメージ倍率を掛けないが、callback / 履歴に種別として伝える。
//   True: 耐性無効の確定ダメージ (デバッグ / 環境ダメージ用)。
enum class DamageType : u8 {
    Physical  = 0,
    Fire      = 1,
    Ice       = 2,
    Lightning = 3,
    Poison    = 4,
    True      = 5,
};

// 死亡時 callback。std::function 不使用ポリシーに従い、void* user を介して
// コンテキストを引き回す。state 更新後 (is_alive=false 確定後) に呼ばれる。
//   id           : 死亡した entity の HealthId
//   lethal_type  : 致死を与えた DamageType
using DeathCallback = void(*)(void* user, HealthId id, DamageType lethal_type) noexcept;

class HealthSystem {
public:
    HealthSystem() noexcept = default;
    ~HealthSystem() noexcept = default;

    HealthSystem(const HealthSystem&)            = delete;
    HealthSystem& operator=(const HealthSystem&) = delete;
    HealthSystem(HealthSystem&&)                 = delete;
    HealthSystem& operator=(HealthSystem&&)      = delete;

    // ----- entity 登録 / 解除 -----
    // 新規 entity を full HP で登録。max_hp <= 0 の場合は 1.0 にクランプして登録。
    HealthId Spawn(f32 max_hp) noexcept;

    // entity を破棄。slot は再利用 (generation 進む)。invalid id は no-op。
    void Despawn(HealthId id) noexcept;

    // ----- ダメージ / 回復 / 状態操作 -----
    // ダメージ適用。戻り値 true = この一撃で致死 (is_alive=false に遷移し、
    // DeathCallback が発火した)。
    //   ・invuln 中: false 返却で no-op (HP 変動無し、callback 不発火)
    //   ・既に死亡中: false 返却で no-op (HP は変動しない)
    //   ・amount <= 0: false 返却で no-op
    //   ・HP > 0 で残った: false 返却 (致死では無い)
    bool ApplyDamage(HealthId id, f32 amount, DamageType type = DamageType::Physical) noexcept;

    // 回復。max_hp で clamp。死亡中でも HP は加算されるが is_alive は変動しない
    // (Revive 経由のみ復活可能)。amount <= 0 は no-op。
    void Heal(HealthId id, f32 amount) noexcept;

    // duration_sec 間 invulnerable=true に。Tick で減算、0 到達で解除。
    // 既に invuln 中の場合は max(現在残り, duration_sec) で延長する。
    void SetInvulnerable(HealthId id, f32 duration_sec) noexcept;

    // 死亡状態から復活させる。生存中なら no-op。
    //   hp_fraction: [0, 1] の HP 割合 (0.0 でも最低 1 HP は確保)。
    //                値域外は clamp。
    void Revive(HealthId id, f32 hp_fraction = 1.0f) noexcept;

    // max_hp を変更。new_max <= 0 は 1.0 にクランプ。
    //   refill=true:  current_hp も new_max に揃える (full heal)
    //   refill=false: current_hp は据置だが new_max で clamp する
    void SetMaxHp(HealthId id, f32 new_max, bool refill) noexcept;

    // ----- 問い合わせ -----
    // 無効 id / Despawn 済 / generation 不一致なら nullptr。
    const HealthState* GetState(HealthId id) const noexcept;

    // 無効なら 0.0 を返す。
    f32 GetCurrentHp(HealthId id) const noexcept;

    // current_hp / max_hp。無効 or max_hp<=0 なら 0.0。
    f32 GetHpFraction(HealthId id) const noexcept;

    // 無効 id は false。
    bool IsAlive(HealthId id) const noexcept;

    // 無効 id は false。invulnerable フラグをそのまま返す (生死は問わない)。
    bool IsInvulnerable(HealthId id) const noexcept;

    // 全 entity 数 (生死問わず active なもの)。
    u32 EntityCount() const noexcept { return _entity_count; }

    // 生存 entity 数 (is_alive=true)。Tick / 戦闘ロジックで利用。
    u32 AliveCount() const noexcept;

    // ----- driver -----
    // 毎フレーム呼び、invuln_timer を進める。0 到達で invulnerable=false。
    void Tick(f32 dt) noexcept;

    // ----- callback -----
    // cb == nullptr で登録解除。state 更新後 (is_alive=false 確定後) に呼ばれる。
    void SetOnDeathCallback(DeathCallback cb, void* user) noexcept;

    // ----- 一括破棄 -----
    // 全 entity を破棄。callback は保持 (Reset とは違う)。
    void ClearAll() noexcept;

private:
    struct Slot {
        HealthState state;
        bool        active = false;
        u8          gen    = 0;
    };

    // 空き slot を取得 (index 0 は invalid 予約 dummy)。
    u32 AcquireSlot() noexcept;

    // 内部アクセサ: id が有効なら slot 参照を返し、無効なら nullptr。
    Slot*       FindSlot(HealthId id) noexcept;
    const Slot* FindSlot(HealthId id) const noexcept;

    static f32 Clamp(f32 v, f32 lo, f32 hi) noexcept;

    Array<Slot>    _slots;
    u32            _entity_count = 0;

    DeathCallback  _on_death       = nullptr;
    void*          _on_death_user  = nullptr;
};

} // namespace acs::game

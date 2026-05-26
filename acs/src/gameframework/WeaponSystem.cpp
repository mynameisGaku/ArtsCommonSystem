// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar I/R — WeaponSystem 実装
//
// 設計上のポイント (ヘッダの設計コメントと対応):
//   ・武器定義は const char* 線形検索 (EconomyDirector / Entitlement と同設計)。
//     per-entity の武器数は通常 5〜10 オーダーで、線形で十分。
//   ・装備中武器の per-weapon 状態 (ammo_in_mag) は装備切替前に
//     _reserves[_current_slot] に書き戻し、新武器切替時に新 slot から
//     復元する。reload 中の切替は ammo_in_mag を「reload 開始前の値」のまま
//     保持する (= reload は単に破棄される)。
//   ・fire_rate_per_sec <= 0 は「1 秒に 1 発」フォールバック (NaN / 0 除算回避)。
//   ・reload_sec <= 0 は StartReload 内で即時 CompleteReload を呼ぶ。
//   ・mag_size == 0 は「マガジン非搭載」(= reserve から直接 1 発消費)。
//   ・max_reserve == 0 は「無制限」(~0u と等価) として扱う。
#include "gameframework/WeaponSystem.h"
#include "foundation/Log.h"

namespace acs::game {

namespace {

// const char* の per-byte 安全比較 (EconomyDirector.cpp / Entitlement.cpp と同設計)。
// どちらかが nullptr なら false。
bool StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

// 「id 未発見」を表す哨兵値 (他 Manager と統一)。
constexpr u32 kNotFound = ~static_cast<u32>(0);

// u32 加算でオーバーフローしないようにクランプ加算。
u32 SaturatingAdd(u32 a, u32 b) noexcept {
    const u32 kMax = ~static_cast<u32>(0);
    if (b > kMax - a) return kMax;
    return a + b;
}

} // namespace

// =============================================================================
// 内部検索
// =============================================================================

u32 WeaponSystem::FindWeaponSlot(const char* weapon_id) const noexcept {
    if (weapon_id == nullptr) return kNotFound;
    const usize n = _defs.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(_defs[i].id, weapon_id)) return static_cast<u32>(i);
    }
    return kNotFound;
}

// =============================================================================
// 武器定義
// =============================================================================

void WeaponSystem::RegisterWeapon(const WeaponDef& def) noexcept {
    // defensive: id == nullptr は意味を持たないので静かに弾く。
    if (def.id == nullptr) return;

    // 同 id の 2 重登録は no-op (アセット二重ロード保護)。
    if (FindWeaponSlot(def.id) != kNotFound) {
        ACS_LOG_WARN("WeaponSystem: duplicate weapon registration ignored ('%s')", def.id);
        return;
    }

    _defs.PushBack(def);
    // 並行 TArray — reserve は 0、mag も 0 で初期化。EquipWeapon 時に新規装備の
    // mag が 0 のままだと使い物にならないので、Equip 時に「初装備なら mag_size
    // まで充填する」フォールバックを入れる (詳細は EquipWeapon 参照)。
    ReserveSlot slot;
    slot.weapon_id    = def.id;
    slot.reserve_ammo = 0u;
    slot.ammo_in_mag  = 0u;
    _reserves.PushBack(slot);
}

// =============================================================================
// 装備中状態の保存 / 復元
// =============================================================================

void WeaponSystem::SaveCurrentToSlot() noexcept {
    if (_current_def == nullptr) return;
    if (_current_slot == kNotFound) return;
    if (_current_slot >= static_cast<u32>(_reserves.Size())) return;

    ReserveSlot& slot = _reserves[_current_slot];
    slot.reserve_ammo = _state.reserve_ammo;
    slot.ammo_in_mag  = _state.ammo_in_mag;
}

// =============================================================================
// 武器切替
// =============================================================================

bool WeaponSystem::EquipWeapon(const char* weapon_id) noexcept {
    const u32 slot = FindWeaponSlot(weapon_id);
    if (slot == kNotFound) return false;

    // 同武器への再装備も許可するが、reload 状態 / next_fire_time はリセット
    // しない方が「切替で fire-rate ペナルティ無し」になるため、別武器のとき
    // だけ next_fire_time をリセットする (= 武器切替には硬直時間を入れない)。
    const bool is_same = (_current_slot == slot);

    // 装備中武器の per-weapon 状態を保存 (reload 中なら ammo_in_mag は reload
    // 開始前の値のまま保存される = reload は破棄される仕様)。
    if (!is_same) {
        SaveCurrentToSlot();
    }

    // 新武器の slot から状態を復元する。
    const ReserveSlot& src = _reserves[slot];
    const WeaponDef&   def = _defs[slot];

    _current_def  = &_defs[slot];
    _current_slot = slot;

    _state.current_def_id = def.id;
    _state.reserve_ammo   = src.reserve_ammo;

    // 初装備時 (= mag が 0 で reserve も 0 で、まだ Reload も呼ばれていない)、
    // mag_size まで自動充填して即発射可能にする。これは「初期ロードアウトを
    // 呼出側が AddReserveAmmo → EquipWeapon の順で組むときの体験」を優先した
    // フォールバック。すでに 1 度装備した武器 (= ammo_in_mag が記録済みか、
    // reserve_ammo が消費済み) であれば触らない。
    u32 mag = src.ammo_in_mag;
    if (mag == 0u && src.reserve_ammo == 0u && def.mag_size > 0u) {
        // 完全 zero-state → mag を満タンに (reserve は 0 のまま)。
        mag = def.mag_size;
    }
    _state.ammo_in_mag = mag;

    // reload 中の切替は破棄 (仕様)。next_fire_time は別武器のときだけ 0 に
    // 戻して連射間隔を新武器の fire_rate に従わせる。
    _state.reloading            = false;
    _state.reload_remaining_sec = 0.0f;
    if (!is_same) {
        _state.next_fire_time_sec = 0.0f;
    }
    return true;
}

// =============================================================================
// 発射
// =============================================================================

bool WeaponSystem::TryFire() noexcept {
    // 装備なし / reload 中は失敗。
    if (_current_def == nullptr) return false;
    if (_state.reloading) return false;

    // 連射間隔未到。
    if (_elapsed_time < _state.next_fire_time_sec) return false;

    const WeaponDef& def = *_current_def;

    // mag_size == 0 (= マガジン非搭載) は reserve から直接 1 発消費する。
    // それ以外は ammo_in_mag をチェック。
    if (def.mag_size == 0u) {
        if (_state.reserve_ammo == 0u) return false;
        --_state.reserve_ammo;
    } else {
        if (_state.ammo_in_mag == 0u) return false;
        --_state.ammo_in_mag;
    }

    // 連射間隔を next_fire_time に反映 (fire_rate <= 0 は 1.0 sec のフォールバック)。
    const f32 interval = (def.fire_rate_per_sec > 0.0f)
                            ? (1.0f / def.fire_rate_per_sec)
                            : 1.0f;
    _state.next_fire_time_sec = _elapsed_time + interval;

    // pellets は 0 入力を 1 に正規化 (= 1 発単発相当)。
    const u32 pellets = (def.pellets_per_shot == 0u) ? 1u : def.pellets_per_shot;

    if (_on_fire != nullptr) {
        _on_fire(_on_fire_user, def.projectile_id, def.base_damage, def.spread_deg, pellets);
    }
    return true;
}

// =============================================================================
// リロード
// =============================================================================

void WeaponSystem::StartReload() noexcept {
    // 装備なし / reload 中は no-op。
    if (_current_def == nullptr) return;
    if (_state.reloading) return;

    const WeaponDef& def = *_current_def;

    // mag_size == 0 (= マガジン非搭載) は reload 概念なし、no-op。
    if (def.mag_size == 0u) return;

    // mag 満タンなら no-op。
    if (_state.ammo_in_mag >= def.mag_size) return;

    // reserve 0 なら no-op (補充しようがない)。
    if (_state.reserve_ammo == 0u) return;

    // reload_sec <= 0 は「即時 reload」として CompleteReload を呼ぶ。
    if (def.reload_sec <= 0.0f) {
        _state.reloading            = true;
        _state.reload_remaining_sec = 0.0f;
        CompleteReload();
        return;
    }

    _state.reloading            = true;
    _state.reload_remaining_sec = def.reload_sec;
}

void WeaponSystem::CancelReload() noexcept {
    // reload 中でなければ no-op。ammo_in_mag は変えない (reload はそもそも
    // mag 完了時に補充するので、途中 cancel での部分補充は仕様外)。
    if (!_state.reloading) return;
    _state.reloading            = false;
    _state.reload_remaining_sec = 0.0f;
}

void WeaponSystem::CompleteReload() noexcept {
    // 装備なしは defensive: ここに来る前に StartReload で弾いている前提。
    if (_current_def == nullptr) {
        _state.reloading            = false;
        _state.reload_remaining_sec = 0.0f;
        return;
    }

    const WeaponDef& def = *_current_def;
    if (def.mag_size > 0u) {
        const u32 need = def.mag_size - _state.ammo_in_mag;
        const u32 take = (need < _state.reserve_ammo) ? need : _state.reserve_ammo;
        _state.ammo_in_mag  += take;
        _state.reserve_ammo -= take;
    }

    _state.reloading            = false;
    _state.reload_remaining_sec = 0.0f;

    if (_on_reload != nullptr) {
        _on_reload(_on_reload_user, def.id);
    }
}

f32 WeaponSystem::ReloadProgress() const noexcept {
    if (!_state.reloading) return 0.0f;
    if (_current_def == nullptr) return 0.0f;
    const f32 total = _current_def->reload_sec;
    if (total <= 0.0f) return 1.0f;
    const f32 done = total - _state.reload_remaining_sec;
    if (done <= 0.0f) return 0.0f;
    if (done >= total) return 1.0f;
    return done / total;
}

// =============================================================================
// 弾薬補給
// =============================================================================

void WeaponSystem::AddReserveAmmo(const char* weapon_id, u32 amount) noexcept {
    if (amount == 0u) return;
    const u32 slot = FindWeaponSlot(weapon_id);
    if (slot == kNotFound) return;

    const WeaponDef&   def    = _defs[slot];
    ReserveSlot&       res    = _reserves[slot];
    const u32          maxRes = (def.max_reserve == 0u) ? ~static_cast<u32>(0) : def.max_reserve;

    // 装備中武器なら _state 側にも反映する (= 二重管理を避けるため
    // 「装備中は _state が真実、非装備中は _reserves が真実」と決め、
    // 補給時は両方を整合させる)。
    if (_current_slot == slot) {
        const u32 next = SaturatingAdd(_state.reserve_ammo, amount);
        _state.reserve_ammo = (next > maxRes) ? maxRes : next;
        res.reserve_ammo    = _state.reserve_ammo;
    } else {
        const u32 next = SaturatingAdd(res.reserve_ammo, amount);
        res.reserve_ammo = (next > maxRes) ? maxRes : next;
    }
}

// =============================================================================
// フレーム更新
// =============================================================================

void WeaponSystem::Tick(f32 dt) noexcept {
    if (dt <= 0.0f) return;
    _elapsed_time += dt;

    // reload 進行 (装備中のみ)。
    if (_state.reloading) {
        _state.reload_remaining_sec -= dt;
        if (_state.reload_remaining_sec <= 0.0f) {
            CompleteReload();
        }
    }
}

// =============================================================================
// 全リセット
// =============================================================================

void WeaponSystem::ClearAll() noexcept {
    _defs.Clear();
    _reserves.Clear();
    _current_def  = nullptr;
    _current_slot = kNotFound;
    _state        = WeaponState{};
    _elapsed_time = 0.0f;
    _on_fire        = nullptr;
    _on_fire_user   = nullptr;
    _on_reload      = nullptr;
    _on_reload_user = nullptr;
}

} // namespace acs::game

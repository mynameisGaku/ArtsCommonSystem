// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar I/R — FWeaponSystem 実装
//
// 設計上のポイント (ヘッダの設計コメントと対応):
//   ・武器定義は const char* 線形検索 (FEconomyDirector / FEntitlement と同設計)。
//     per-entity の武器数は通常 5〜10 オーダーで、線形で十分。
//   ・装備中武器の per-weapon 状態 (ammo_in_mag) は装備切替前に
//     m_Reserves[m_CurrentSlot] に書き戻し、新武器切替時に新 slot から
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

/**
 * const char* の per-byte 安全比較を行う。
 *
 * @details どちらかが nullptr なら false。終端まで一致したときのみ true。
 * @param a 比較する一方の文字列。
 * @param b 比較する他方の文字列。
 * @return 内容が一致すれば true。
 */
bool StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

/** 「id 未発見」を表す哨兵値 (他 Manager と統一)。 */
constexpr u32 kNotFound = ~static_cast<u32>(0);

/**
 * u32 加算をオーバーフローさせずにクランプ加算する。
 *
 * @param a 加算の一方。
 * @param b 加算の他方。
 * @return a + b、ただし u32 上限を超える場合は上限値。
 */
u32 SaturatingAdd(u32 a, u32 b) noexcept {
    const u32 kMax = ~static_cast<u32>(0);
    if (b > kMax - a) return kMax;
    return a + b;
}

} // namespace

/** 武器 id を内部配列位置へ変換する (per-byte 線形検索、未検出は kNotFound)。 */
u32 FWeaponSystem::FindWeaponSlot(const char* weapon_id) const noexcept {
    if (weapon_id == nullptr) return kNotFound;
    const usize n = m_Defs.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(m_Defs[i].id, weapon_id)) return static_cast<u32>(i);
    }
    return kNotFound;
}

/** 武器定義を登録し、対応する reserve スロットを 0 で初期化する。 */
void FWeaponSystem::RegisterWeapon(const FWeaponDef& def) noexcept {
    // defensive: id == nullptr は意味を持たないので静かに弾く。
    if (def.id == nullptr) return;

    // 同 id の 2 重登録は no-op (アセット二重ロード保護)。
    if (FindWeaponSlot(def.id) != kNotFound) {
        ACS_LOG_WARN("FWeaponSystem: duplicate weapon registration ignored ('%s')", def.id);
        return;
    }

    m_Defs.PushBack(def);
    // 並行 TArray — reserve は 0、mag も 0 で初期化。EquipWeapon 時に新規装備の
    // mag が 0 のままだと使い物にならないので、Equip 時に「初装備なら mag_size
    // まで充填する」フォールバックを入れる (詳細は EquipWeapon 参照)。
    ReserveSlot slot;
    slot.weapon_id    = def.id;
    slot.reserve_ammo = 0u;
    slot.ammo_in_mag  = 0u;
    m_Reserves.PushBack(slot);

    // m_Defs を PushBack で再確保した可能性があるため、装備中なら m_CurrentDef を
    // slot index から張り直す (旧バッファへの dangling pointer = UAF を防ぐ)。
    if (m_CurrentSlot != kNotFound && m_CurrentSlot < static_cast<u32>(m_Defs.Size())) {
        m_CurrentDef = &m_Defs[m_CurrentSlot];
    }
}

/** 装備中武器の per-weapon 状態 (reserve / mag) を m_Reserves[] へ書き戻す。 */
void FWeaponSystem::SaveCurrentToSlot() noexcept {
    if (m_CurrentDef == nullptr) return;
    if (m_CurrentSlot == kNotFound) return;
    if (m_CurrentSlot >= static_cast<u32>(m_Reserves.Size())) return;

    ReserveSlot& slot = m_Reserves[m_CurrentSlot];
    slot.reserve_ammo = _state.reserve_ammo;
    slot.ammo_in_mag  = _state.ammo_in_mag;
}

/** 指定 id の武器を装備し、状態を新武器の保存値から復元する。 */
bool FWeaponSystem::EquipWeapon(const char* weapon_id) noexcept {
    const u32 slot = FindWeaponSlot(weapon_id);
    if (slot == kNotFound) return false;

    // 同武器への再装備も許可するが、reload 状態 / next_fire_time はリセット
    // しない方が「切替で fire-rate ペナルティ無し」になるため、別武器のとき
    // だけ next_fire_time をリセットする (= 武器切替には硬直時間を入れない)。
    const bool is_same = (m_CurrentSlot == slot);

    // 装備中武器の per-weapon 状態を保存 (reload 中なら ammo_in_mag は reload
    // 開始前の値のまま保存される = reload は破棄される仕様)。
    if (!is_same) {
        SaveCurrentToSlot();
    }

    // 新武器の slot から状態を復元する。
    const ReserveSlot& src = m_Reserves[slot];
    const FWeaponDef&   def = m_Defs[slot];

    m_CurrentDef  = &m_Defs[slot];
    m_CurrentSlot = slot;

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

/** 発射を試み、可否チェックを通れば弾を 1 発消費して FireCallback を発火する。 */
bool FWeaponSystem::TryFire() noexcept {
    // 装備なし / reload 中は失敗。
    if (m_CurrentDef == nullptr) return false;
    if (_state.reloading) return false;

    // 連射間隔未到。
    if (m_ElapsedTime < _state.next_fire_time_sec) return false;

    const FWeaponDef& def = *m_CurrentDef;

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
    _state.next_fire_time_sec = m_ElapsedTime + interval;

    // pellets は 0 入力を 1 に正規化 (= 1 発単発相当)。
    const u32 pellets = (def.pellets_per_shot == 0u) ? 1u : def.pellets_per_shot;

    if (m_OnFire != nullptr) {
        m_OnFire(m_OnFireUser, def.projectile_id, def.base_damage, def.spread_deg, pellets);
    }
    return true;
}

/** リロードを開始する (reload_sec<=0 は即時完了)。 */
void FWeaponSystem::StartReload() noexcept {
    // 装備なし / reload 中は no-op。
    if (m_CurrentDef == nullptr) return;
    if (_state.reloading) return;

    const FWeaponDef& def = *m_CurrentDef;

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

/** reload 中なら打ち切る (ammo は変化しない)。 */
void FWeaponSystem::CancelReload() noexcept {
    // reload 中でなければ no-op。ammo_in_mag は変えない (reload はそもそも
    // mag 完了時に補充するので、途中 cancel での部分補充は仕様外)。
    if (!_state.reloading) return;
    _state.reloading            = false;
    _state.reload_remaining_sec = 0.0f;
}

/** reload を完了させ、mag に reserve を補填して ReloadCallback を発火する。 */
void FWeaponSystem::CompleteReload() noexcept {
    // 装備なしは defensive: ここに来る前に StartReload で弾いている前提。
    if (m_CurrentDef == nullptr) {
        _state.reloading            = false;
        _state.reload_remaining_sec = 0.0f;
        return;
    }

    const FWeaponDef& def = *m_CurrentDef;
    if (def.mag_size > 0u) {
        const u32 need = def.mag_size - _state.ammo_in_mag;
        const u32 take = (need < _state.reserve_ammo) ? need : _state.reserve_ammo;
        _state.ammo_in_mag  += take;
        _state.reserve_ammo -= take;
    }

    _state.reloading            = false;
    _state.reload_remaining_sec = 0.0f;

    if (m_OnReload != nullptr) {
        m_OnReload(m_OnReloadUser, def.id);
    }
}

/** reload 進行度 [0, 1] を返す (reload 中でなければ 0.0)。 */
f32 FWeaponSystem::ReloadProgress() const noexcept {
    if (!_state.reloading) return 0.0f;
    if (m_CurrentDef == nullptr) return 0.0f;
    const f32 total = m_CurrentDef->reload_sec;
    if (total <= 0.0f) return 1.0f;
    const f32 done = total - _state.reload_remaining_sec;
    if (done <= 0.0f) return 0.0f;
    if (done >= total) return 1.0f;
    return done / total;
}

/** 指定武器の予備弾薬を加算する (max_reserve でクランプ、装備中なら _state にも反映)。 */
void FWeaponSystem::AddReserveAmmo(const char* weapon_id, u32 amount) noexcept {
    if (amount == 0u) return;
    const u32 slot = FindWeaponSlot(weapon_id);
    if (slot == kNotFound) return;

    const FWeaponDef&   def    = m_Defs[slot];
    ReserveSlot&       res    = m_Reserves[slot];
    const u32          max_res = (def.max_reserve == 0u) ? ~static_cast<u32>(0) : def.max_reserve;

    // 装備中武器なら _state 側にも反映する (= 二重管理を避けるため
    // 「装備中は _state が真実、非装備中は m_Reserves が真実」と決め、
    // 補給時は両方を整合させる)。
    if (m_CurrentSlot == slot) {
        const u32 next = SaturatingAdd(_state.reserve_ammo, amount);
        _state.reserve_ammo = (next > max_res) ? max_res : next;
        res.reserve_ammo    = _state.reserve_ammo;
    } else {
        const u32 next = SaturatingAdd(res.reserve_ammo, amount);
        res.reserve_ammo = (next > max_res) ? max_res : next;
    }
}

/** 内部時計を進め、reload 中なら残り時間を減算して 0 到達で完了させる。 */
void FWeaponSystem::Tick(f32 dt) noexcept {
    if (dt <= 0.0f) return;
    m_ElapsedTime += dt;

    // reload 進行 (装備中のみ)。
    if (_state.reloading) {
        _state.reload_remaining_sec -= dt;
        if (_state.reload_remaining_sec <= 0.0f) {
            CompleteReload();
        }
    }
}

/** 武器定義 / reserve / current state / コールバックをすべてクリアする。 */
void FWeaponSystem::ClearAll() noexcept {
    m_Defs.Clear();
    m_Reserves.Clear();
    m_CurrentDef  = nullptr;
    m_CurrentSlot = kNotFound;
    _state        = FWeaponState{};
    m_ElapsedTime = 0.0f;
    m_OnFire        = nullptr;
    m_OnFireUser   = nullptr;
    m_OnReload      = nullptr;
    m_OnReloadUser = nullptr;
}

} // namespace acs::game

// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar I — ProjectileSystem 実装
//
// 仕様の意図は ProjectileSystem.h のヘッダコメントを参照。本ファイルでは:
//   ・固定容量 Slot pool の Acquire / Release (generational handle)
//   ・def_id 名前引きでの ProjectileDef 取得
//   ・Tick 内での物理積分 (semi-implicit Euler) + homing 向き補正
//   ・HitTestFn → HitCallback → 貫通カウント → 必要なら despawn
//   ・lifetime 超過 → ExpireCallback → despawn
//   ・AllAlive 用の連続スナップショット buffer の再構築
// を実装する。すべて noexcept、STL 不使用、<string> 不使用 (アドレス + 文字列
// 一致で id 比較)。
#include "gameframework/ProjectileSystem.h"

#include "math/Math.h"   // Sqrt

namespace acs::game {

namespace {
// const char* 同士の比較: アドレス一致 (string literal の dedupe を期待) を最優先、
// 落ちた場合のみ文字列内容比較。strcmp 相当を内製 (STL 不使用ポリシー)。
inline bool CStrEquals(const char* a, const char* b) noexcept {
    if (a == b) return true;
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a; ++b;
    }
    return *a == *b; // 両方 '\0' で終わったか
}
} // namespace

// =============================================================================
// Init
// -----------------------------------------------------------------------------
// pool 容量を確定し、Slot 配列と AllAlive 用 snapshot buffer を default 構築する。
// 二度目以降の呼び出しは no-op (固定容量ポリシー: 走行中の resize は禁止)。
// max_concurrent == 0 は誤呼出しと見なし 256 を採用 (silently fail を避ける)。
// =============================================================================
void ProjectileSystem::Init(u32 max_concurrent) noexcept {
    if (_capacity != 0u) return;
    if (max_concurrent == 0u) max_concurrent = 256u;

    _slots.Resize(static_cast<usize>(max_concurrent));
    _alive_snapshot.Resize(static_cast<usize>(max_concurrent));

    // 各 slot を inactive 初期化 (Resize default 構築で active=false になっているが
    // gen はゼロのまま。最初の Spawn で gen=1 から払い出す)。
    for (usize i = 0; i < _slots.Size(); ++i) {
        _slots[i].active            = false;
        _slots[i].gen               = 0u;
        _slots[i].has_homing_target = false;
    }

    _capacity            = max_concurrent;
    _alive_count         = 0u;
    _snapshot_dirty_size = 0u;
}

// =============================================================================
// RegisterDef
// -----------------------------------------------------------------------------
// 1) id == nullptr / lifetime_sec <= 0 の def は無効 (発射しても即死するので意味
//    が無い)。
// 2) 既に同 id (アドレス or 文字列一致) が登録済みなら値だけ更新 (= 上書き)。
// 3) 新規なら末尾に追加。
//
// 名前引きは Spawn のたびに行うが、_defs の数は通常 10〜30 程度なので線形探索で
// 十分。
// =============================================================================
void ProjectileSystem::RegisterDef(const ProjectileDef& def) noexcept {
    if (def.id == nullptr) return;
    if (def.lifetime_sec <= 0.0f) return;

    const usize n = _defs.Size();
    for (usize i = 0; i < n; ++i) {
        if (CStrEquals(_defs[i].id, def.id)) {
            _defs[i] = def;
            return;
        }
    }
    _defs.PushBack(def);
}

// =============================================================================
// FindDef
// -----------------------------------------------------------------------------
// 線形検索。アドレス一致 → 文字列一致の順 (string literal なら通常 1 比較で済む)。
// =============================================================================
const ProjectileDef* ProjectileSystem::FindDef(const char* id) const noexcept {
    if (id == nullptr) return nullptr;
    const usize n = _defs.Size();
    for (usize i = 0; i < n; ++i) {
        if (CStrEquals(_defs[i].id, id)) return &_defs[i];
    }
    return nullptr;
}

// =============================================================================
// AcquireSlot
// -----------------------------------------------------------------------------
// 既存の inactive slot を線形探索で再利用。pool は固定容量なので満杯時は
// kInvalidIdx を返す (= 上限を超えた発射要求はサイレントに失敗、フレーム落ちより
// は弾が出ないことを優先する保守的ポリシー)。
// =============================================================================
u32 ProjectileSystem::AcquireSlot() noexcept {
    if (_capacity == 0u) return kInvalidIdx;
    // alive_count >= capacity の早期 return で満杯時の無駄ループを排除。
    if (_alive_count >= _capacity) return kInvalidIdx;

    for (usize i = 0; i < _slots.Size(); ++i) {
        if (!_slots[i].active) {
            return static_cast<u32>(i);
        }
    }
    return kInvalidIdx;  // 理論上ここには来ない (alive_count < capacity を確認済)
}

// =============================================================================
// FindSlot
// -----------------------------------------------------------------------------
// id (packed handle) から内部 slot 参照を引く。invalid / 範囲外 / inactive /
// gen 不一致は nullptr を返す。stale handle 検出はここで完結する。
// =============================================================================
ProjectileSystem::Slot* ProjectileSystem::FindSlot(ProjectileId id) noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx >= _slots.Size()) return nullptr;
    Slot& s = _slots[static_cast<usize>(idx)];
    if (!s.active) return nullptr;
    if (s.gen != id.Gen()) return nullptr;
    return &s;
}

const ProjectileSystem::Slot* ProjectileSystem::FindSlot(ProjectileId id) const noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx >= _slots.Size()) return nullptr;
    const Slot& s = _slots[static_cast<usize>(idx)];
    if (!s.active) return nullptr;
    if (s.gen != id.Gen()) return nullptr;
    return &s;
}

// =============================================================================
// Spawn
// -----------------------------------------------------------------------------
// 1) def_id が未登録なら invalid を返す。
// 2) 空き slot を取得。満杯なら invalid を返す。
// 3) gen を 1 進める (0 にラップしたら 1 に戻す。0 は IsValid==false 予約)。
// 4) instance を初期化。owner_id / damage は呼出側の値をそのまま使う。
// 5) homing target は SetHomingTarget が後で設定する想定なのでここでは false。
// =============================================================================
ProjectileId ProjectileSystem::Spawn(const char* def_id, Vec2 pos, Vec2 velocity,
                                     u32 owner_id, f32 damage) noexcept {
    const ProjectileDef* def = FindDef(def_id);
    if (def == nullptr) return ProjectileId{};

    const u32 idx = AcquireSlot();
    if (idx == kInvalidIdx) return ProjectileId{};

    Slot& s = _slots[static_cast<usize>(idx)];

    u8 new_gen = static_cast<u8>(s.gen + 1u);
    if (new_gen == 0u) new_gen = 1u;

    s.inst.def_id      = def->id;   // RegisterDef した string literal を直接保持
    s.inst.position    = pos;
    s.inst.velocity    = velocity;
    s.inst.elapsed_sec = 0.0f;
    s.inst.hit_count   = 0u;
    s.inst.owner_id    = owner_id;
    s.inst.damage      = damage;

    s.homing_tgt        = Vec2{0.0f, 0.0f};
    s.has_homing_target = false;
    s.gen               = new_gen;
    s.active            = true;

    ++_alive_count;
    // snapshot は AllAlive 呼出時 / Tick 末尾で再構築。即時返却用に dirty 化のみ。
    _snapshot_dirty_size = 0u;
    return ProjectileId::Pack(idx, new_gen);
}

// =============================================================================
// Despawn
// -----------------------------------------------------------------------------
// 強制削除。ExpireCallback は発火しない (= サイレント、Cancel 系の慣例)。
// gen は維持し、次の AcquireSlot で +1 して払い出される (= 古い handle が無効化
// される)。
// =============================================================================
void ProjectileSystem::Despawn(ProjectileId id) noexcept {
    Slot* s = FindSlot(id);
    if (s == nullptr) return;
    s->active            = false;
    s->has_homing_target = false;
    if (_alive_count > 0u) --_alive_count;
    _snapshot_dirty_size = 0u;
}

// =============================================================================
// GetInstance / AllAlive
// -----------------------------------------------------------------------------
// GetInstance は単純な引き。AllAlive は内部 snapshot buffer に alive 個だけを
// 詰めて返す (= 連続配列なので描画ループが書きやすい)。snapshot は Spawn /
// Despawn / Tick で dirty 化し、ここで lazy に再構築する。
// =============================================================================
const ProjectileInstance* ProjectileSystem::GetInstance(ProjectileId id) const noexcept {
    const Slot* s = FindSlot(id);
    if (s == nullptr) return nullptr;
    return &s->inst;
}

const ProjectileInstance* ProjectileSystem::AllAlive(u32& out_count) const noexcept {
    // 非 const 内部状態 (_alive_snapshot / _snapshot_dirty_size) を lazy に
    // 再構築するため const_cast で書き換える。これは mutable cache の慣例。
    auto* self = const_cast<ProjectileSystem*>(this);

    if (self->_snapshot_dirty_size != self->_alive_count) {
        self->RebuildAliveSnapshot();
    }

    out_count = self->_alive_count;
    if (self->_alive_count == 0u) return nullptr;
    return self->_alive_snapshot.Data();
}

// =============================================================================
// RebuildAliveSnapshot
// -----------------------------------------------------------------------------
// _slots を走査し、active なものだけを _alive_snapshot に詰める。
// _snapshot_dirty_size = _alive_count にすることで次回 AllAlive は cache hit する。
// =============================================================================
void ProjectileSystem::RebuildAliveSnapshot() noexcept {
    u32 written = 0u;
    const usize n = _slots.Size();
    for (usize i = 0; i < n; ++i) {
        if (!_slots[i].active) continue;
        _alive_snapshot[static_cast<usize>(written)] = _slots[i].inst;
        ++written;
        if (written >= _alive_count) break;  // 早期 exit (cache friendly)
    }
    _snapshot_dirty_size = _alive_count;
}

// =============================================================================
// Tick
// -----------------------------------------------------------------------------
// 各 alive projectile に対して:
//   1) gravity を velocity に加算 (semi-implicit Euler)。
//   2) homing なら target 方向の単位ベクトルと現 velocity の単位ベクトルを
//      homing_strength で LERP し、速度の大きさは維持。
//   3) position += velocity * dt。
//   4) HitTestFn を呼び、true なら hit_count++ / HitCallback 発火。
//      pierces=false (or max_pierces 超過) なら despawn (ExpireCallback 不発火)。
//   5) elapsed_sec += dt → lifetime 超過なら despawn + ExpireCallback。
//
// dt <= 0 / Init 前 (_capacity == 0) は no-op (defensive)。
// =============================================================================
void ProjectileSystem::Tick(f32 dt) noexcept {
    if (dt <= 0.0f) return;
    if (_capacity == 0u) return;

    const usize n = _slots.Size();
    for (usize i = 0; i < n; ++i) {
        Slot& s = _slots[i];
        if (!s.active) continue;

        const ProjectileDef* def = FindDef(s.inst.def_id);
        // def が消えている (RegisterDef を未呼び出し) ことは通常無いが、防御的に
        // null check してそのまま物理だけ進める (lifetime チェックは skip)。
        const f32  lifetime  = (def != nullptr) ? def->lifetime_sec : 0.0f;
        const f32  gravity_y = (def != nullptr) ? def->gravity_y    : 0.0f;
        const bool homing    = (def != nullptr) ? def->homing       : false;
        const f32  homing_k  = (def != nullptr) ? def->homing_strength : 0.0f;
        const bool pierces   = (def != nullptr) ? def->pierces      : false;
        const u32  max_pierces = (def != nullptr) ? def->max_pierces : 0u;

        // --- (1) 重力加算 (semi-implicit Euler: v <- v + g*dt before p <- p + v*dt)
        s.inst.velocity.y += gravity_y * dt;

        // --- (2) homing: target 方向に向き補正 (速度大きさは保持)
        if (homing && s.has_homing_target && homing_k > 0.0f) {
            const Vec2 to_target = s.homing_tgt - s.inst.position;
            const f32 tgt_len2 = to_target.x * to_target.x + to_target.y * to_target.y;
            const f32 vel_len2 = s.inst.velocity.x * s.inst.velocity.x
                               + s.inst.velocity.y * s.inst.velocity.y;
            if (tgt_len2 > 1e-6f && vel_len2 > 1e-6f) {
                const f32 inv_tlen = 1.0f / Sqrt(tgt_len2);
                const f32 vel_len  = Sqrt(vel_len2);
                const f32 inv_vlen = 1.0f / vel_len;
                // 単位ベクトル LERP: dir = normalize((1-k)*vel_hat + k*tgt_hat)
                const f32 vx_hat = s.inst.velocity.x * inv_vlen;
                const f32 vy_hat = s.inst.velocity.y * inv_vlen;
                const f32 tx_hat = to_target.x * inv_tlen;
                const f32 ty_hat = to_target.y * inv_tlen;
                const f32 k = (homing_k > 1.0f) ? 1.0f : homing_k;
                const f32 nx = (1.0f - k) * vx_hat + k * tx_hat;
                const f32 ny = (1.0f - k) * vy_hat + k * ty_hat;
                const f32 nlen2 = nx * nx + ny * ny;
                if (nlen2 > 1e-6f) {
                    const f32 inv_nlen = 1.0f / Sqrt(nlen2);
                    s.inst.velocity.x = nx * inv_nlen * vel_len;
                    s.inst.velocity.y = ny * inv_nlen * vel_len;
                }
            }
        }

        // --- (3) 位置更新
        s.inst.position.x += s.inst.velocity.x * dt;
        s.inst.position.y += s.inst.velocity.y * dt;

        // --- (4) 命中チェック
        bool consumed = false;
        if (_hit_test_fn != nullptr) {
            u32 target_id   = 0u;
            f32 damage_done = 0.0f;
            const bool hit = _hit_test_fn(_hit_test_user, s.inst,
                                          target_id, damage_done);
            if (hit) {
                ++s.inst.hit_count;
                // HitCallback 発火 (再入安全: state 更新後)
                if (_on_hit != nullptr) {
                    const ProjectileId pid = ProjectileId::Pack(
                        static_cast<u32>(i), s.gen);
                    _on_hit(_on_hit_user, pid, s.inst.def_id, target_id, damage_done);
                }
                // 貫通判定: pierces=false なら 1 hit で消滅。
                // pierces=true なら max_pierces+1 hit で消滅
                // (= max_pierces=2 → 3 体目で消える)。
                const u32 hit_cap = pierces ? (max_pierces + 1u) : 1u;
                if (s.inst.hit_count >= hit_cap) {
                    // 命中で despawn (ExpireCallback 不発火)
                    s.active            = false;
                    s.has_homing_target = false;
                    if (_alive_count > 0u) --_alive_count;
                    _snapshot_dirty_size = 0u;
                    consumed = true;
                }
            }
        }
        if (consumed) continue;

        // --- (5) 寿命チェック
        s.inst.elapsed_sec += dt;
        if (lifetime > 0.0f && s.inst.elapsed_sec >= lifetime) {
            // ExpireCallback 発火 (state 更新前: callback 内で参照可能)
            if (_on_expire != nullptr) {
                const ProjectileId pid = ProjectileId::Pack(
                    static_cast<u32>(i), s.gen);
                _on_expire(_on_expire_user, pid, s.inst.def_id);
            }
            s.active            = false;
            s.has_homing_target = false;
            if (_alive_count > 0u) --_alive_count;
            _snapshot_dirty_size = 0u;
        }
    }

    // Tick で alive bullet 全部の position を更新したので、snapshot は
    // alive_count が変わっていなくても **古いまま**。必ず rebuild する。
    // (これを忘れると AllAlive が古い position を返し、bullet が動いていない
    //  ように見える — 2026-05-26 ユーザー実機検証で発覚)
    RebuildAliveSnapshot();
}

// =============================================================================
// SetHitTestFn / SetOnHitCallback / SetOnExpireCallback
// -----------------------------------------------------------------------------
// fn / cb == nullptr で登録解除可能。user pointer も同時に更新する。
// =============================================================================
void ProjectileSystem::SetHitTestFn(HitTestFn fn, void* user) noexcept {
    _hit_test_fn   = fn;
    _hit_test_user = user;
}

void ProjectileSystem::SetOnHitCallback(HitCallback cb, void* user) noexcept {
    _on_hit      = cb;
    _on_hit_user = user;
}

void ProjectileSystem::SetOnExpireCallback(ExpireCallback cb, void* user) noexcept {
    _on_expire      = cb;
    _on_expire_user = user;
}

// =============================================================================
// SetHomingTarget
// -----------------------------------------------------------------------------
// homing=true の def を持つ弾のみ反応 (それ以外は no-op)。stale handle も no-op。
// SetHomingTarget の後で別の Spawn が同 slot を再利用すると、has_homing_target は
// Spawn で false に戻されるので、stale handle 経由のターゲット汚染は起きない。
// =============================================================================
void ProjectileSystem::SetHomingTarget(ProjectileId id, Vec2 target_pos) noexcept {
    Slot* s = FindSlot(id);
    if (s == nullptr) return;
    const ProjectileDef* def = FindDef(s->inst.def_id);
    if (def == nullptr || !def->homing) return;
    s->homing_tgt        = target_pos;
    s->has_homing_target = true;
}

// =============================================================================
// ClearAll
// -----------------------------------------------------------------------------
// 全 slot を inactive に。gen は維持 (= 古い handle が次の Spawn 後に無効化される)。
// callback / def 登録は維持。ExpireCallback は発火しない (= サイレント全消去)。
// =============================================================================
void ProjectileSystem::ClearAll() noexcept {
    const usize n = _slots.Size();
    for (usize i = 0; i < n; ++i) {
        _slots[i].active            = false;
        _slots[i].has_homing_target = false;
    }
    _alive_count         = 0u;
    _snapshot_dirty_size = 0u;
}

} // namespace acs::game

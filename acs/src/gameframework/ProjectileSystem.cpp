// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar I — FProjectileSystem 実装
//
// 仕様の意図は FProjectileSystem.h のヘッダコメントを参照。本ファイルでは:
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
/**
 * const char* 同士を比較する (strcmp 相当の内製版)。
 *
 * @details
 * アドレス一致 (string literal の dedupe を期待) を最優先で見て、落ちた場合のみ
 * 文字列内容を比較する。STL 不使用ポリシーのため strcmp に依存しない。
 * @param a 比較する文字列 A (nullptr 可)。
 * @param b 比較する文字列 B (nullptr 可)。
 * @return 内容が一致すれば true。
 */
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

/**
 * pool 容量を確定し、Slot 配列と AllAlive 用 snapshot buffer を確保する。
 *
 * @details
 * 二度目以降の呼び出しは no-op (固定容量ポリシー: 走行中の resize は禁止)。
 * max_concurrent == 0 は誤呼出しと見なし 256 を採用する (silently fail を避ける)。
 * @param max_concurrent 同時に存在できる弾の最大数 (0 は 256 に補正)。
 */
void FProjectileSystem::Init(u32 max_concurrent) noexcept {
    if (m_Capacity != 0u) return;
    if (max_concurrent == 0u) max_concurrent = 256u;

    m_Slots.Resize(static_cast<usize>(max_concurrent));
    m_AliveSnapshot.Resize(static_cast<usize>(max_concurrent));

    // 各 slot を inactive 初期化 (Resize default 構築で active=false になっているが
    // gen はゼロのまま。最初の Spawn で gen=1 から払い出す)。
    for (usize i = 0; i < m_Slots.Size(); ++i) {
        m_Slots[i].active            = false;
        m_Slots[i].gen               = 0u;
        m_Slots[i].has_homing_target = false;
    }

    m_Capacity            = max_concurrent;
    m_AliveCount         = 0u;
    m_SnapshotDirtySize = 0u;
}

/**
 * 弾の定義を登録する (同 id が既存なら上書き、新規なら末尾に追加)。
 *
 * @details
 * id == nullptr または lifetime_sec <= 0 の def は無効として無視する (発射しても
 * 即死するため意味が無い)。同 id 判定はアドレス一致 → 文字列一致の順。名前引きは
 * Spawn のたびに行うが m_Defs の数は通常 10〜30 程度なので線形探索で十分。
 * @param def 登録する弾定義。
 */
void FProjectileSystem::RegisterDef(const ProjectileDef& def) noexcept {
    if (def.id == nullptr) return;
    if (def.lifetime_sec <= 0.0f) return;

    const usize n = m_Defs.Size();
    for (usize i = 0; i < n; ++i) {
        if (CStrEquals(m_Defs[i].id, def.id)) {
            m_Defs[i] = def;
            return;
        }
    }
    m_Defs.PushBack(def);
}

/**
 * id から弾定義を線形検索で引く。
 *
 * @details アドレス一致 → 文字列一致の順に比較する (string literal なら通常 1 比較で済む)。
 * @param id 探す弾定義の id (nullptr なら nullptr を返す)。
 * @return 見つかった ProjectileDef へのポインタ (無ければ nullptr)。
 */
const ProjectileDef* FProjectileSystem::FindDef(const char* id) const noexcept {
    if (id == nullptr) return nullptr;
    const usize n = m_Defs.Size();
    for (usize i = 0; i < n; ++i) {
        if (CStrEquals(m_Defs[i].id, id)) return &m_Defs[i];
    }
    return nullptr;
}

/**
 * 空き (inactive) slot の index を線形探索で確保する。
 *
 * @details
 * pool は固定容量なので満杯時は kInvalidIdx を返す (上限を超えた発射要求はサイレントに
 * 失敗させ、フレーム落ちより弾が出ないことを優先する保守的ポリシー)。
 * @return 確保した slot の index (満杯なら kInvalidIdx)。
 */
u32 FProjectileSystem::AcquireSlot() noexcept {
    if (m_Capacity == 0u) return kInvalidIdx;
    // alive_count >= capacity の早期 return で満杯時の無駄ループを排除。
    if (m_AliveCount >= m_Capacity) return kInvalidIdx;

    for (usize i = 0; i < m_Slots.Size(); ++i) {
        if (!m_Slots[i].active) {
            return static_cast<u32>(i);
        }
    }
    return kInvalidIdx;  // 理論上ここには来ない (alive_count < capacity を確認済)
}

/**
 * packed handle から内部 slot 参照を引く。
 *
 * @details invalid / 範囲外 / inactive / gen 不一致は nullptr を返す。stale handle 検出はここで完結する。
 * @param id 解決する弾ハンドル。
 * @return 対応する Slot へのポインタ (無効・stale なら nullptr)。
 */
FProjectileSystem::Slot* FProjectileSystem::FindSlot(FProjectileId id) noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx >= m_Slots.Size()) return nullptr;
    Slot& s = m_Slots[static_cast<usize>(idx)];
    if (!s.active) return nullptr;
    if (s.gen != id.Gen()) return nullptr;
    return &s;
}

/**
 * packed handle から内部 slot 参照を引く (const 版)。
 *
 * @details invalid / 範囲外 / inactive / gen 不一致は nullptr を返す。stale handle 検出はここで完結する。
 * @param id 解決する弾ハンドル。
 * @return 対応する Slot への const ポインタ (無効・stale なら nullptr)。
 */
const FProjectileSystem::Slot* FProjectileSystem::FindSlot(FProjectileId id) const noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx >= m_Slots.Size()) return nullptr;
    const Slot& s = m_Slots[static_cast<usize>(idx)];
    if (!s.active) return nullptr;
    if (s.gen != id.Gen()) return nullptr;
    return &s;
}

/**
 * 弾を 1 発発射し、その generational handle を返す。
 *
 * @details
 * def_id が未登録なら invalid を返す。空き slot を取得 (満杯なら invalid)、gen を 1
 * 進めて (0 にラップしたら 1 に戻す。0 は IsValid==false 予約) instance を初期化する。
 * homing target は SetHomingTarget が後で設定する想定なのでここでは未設定にする。
 * @param def_id 発射する弾定義の id。
 * @param pos 発射位置。
 * @param velocity 初速度ベクトル。
 * @param owner_id 発射元の owner id (そのまま instance に保持)。
 * @param damage 命中時のダメージ量 (そのまま instance に保持)。
 * @return 発射した弾の FProjectileId (失敗時は invalid)。
 */
FProjectileId FProjectileSystem::Spawn(const char* def_id, FVec2 pos, FVec2 velocity,
                                     u32 owner_id, f32 damage) noexcept {
    const ProjectileDef* def = FindDef(def_id);
    if (def == nullptr) return FProjectileId{};

    const u32 idx = AcquireSlot();
    if (idx == kInvalidIdx) return FProjectileId{};

    Slot& s = m_Slots[static_cast<usize>(idx)];

    u8 new_gen = static_cast<u8>(s.gen + 1u);
    if (new_gen == 0u) new_gen = 1u;

    s.inst.def_id      = def->id;   // RegisterDef した string literal を直接保持
    s.inst.position    = pos;
    s.inst.velocity    = velocity;
    s.inst.elapsed_sec = 0.0f;
    s.inst.hit_count   = 0u;
    s.inst.owner_id    = owner_id;
    s.inst.damage      = damage;

    s.homing_tgt        = FVec2{0.0f, 0.0f};
    s.has_homing_target = false;
    s.gen               = new_gen;
    s.active            = true;

    ++m_AliveCount;
    // snapshot は AllAlive 呼出時 / Tick 末尾で再構築。即時返却用に dirty 化のみ。
    m_SnapshotDirtySize = 0u;
    return FProjectileId::Pack(idx, new_gen);
}

/**
 * 弾を強制削除する (ExpireCallback は発火しない)。
 *
 * @details
 * Cancel 系の慣例でサイレントに消す。gen は維持し、次の Spawn で +1 して払い出される
 * ことで古い handle が無効化される。stale handle は no-op。
 * @param id 削除する弾ハンドル。
 */
void FProjectileSystem::Despawn(FProjectileId id) noexcept {
    Slot* s = FindSlot(id);
    if (s == nullptr) return;
    s->active            = false;
    s->has_homing_target = false;
    if (m_AliveCount > 0u) --m_AliveCount;
    m_SnapshotDirtySize = 0u;
}

/**
 * handle から弾の instance を引く。
 *
 * @param id 取得する弾ハンドル。
 * @return 対応する ProjectileInstance への const ポインタ (無効・stale なら nullptr)。
 */
const ProjectileInstance* FProjectileSystem::GetInstance(FProjectileId id) const noexcept {
    const Slot* s = FindSlot(id);
    if (s == nullptr) return nullptr;
    return &s->inst;
}

/**
 * alive な全弾を連続配列で返す (描画ループ向け)。
 *
 * @details
 * 内部 snapshot buffer に alive 個だけを詰めて返す。snapshot は Spawn / Despawn /
 * Tick で dirty 化され、ここで lazy に再構築する (const_cast による mutable cache)。
 * @param out_count alive な弾の個数を受け取る出力引数。
 * @return alive 弾の連続スナップショット配列の先頭 (0 個なら nullptr)。
 */
const ProjectileInstance* FProjectileSystem::AllAlive(u32& out_count) const noexcept {
    // 非 const 内部状態 (m_AliveSnapshot / m_SnapshotDirtySize) を lazy に
    // 再構築するため const_cast で書き換える。これは mutable cache の慣例。
    auto* self = const_cast<FProjectileSystem*>(this);

    if (self->m_SnapshotDirtySize != self->m_AliveCount) {
        self->RebuildAliveSnapshot();
    }

    out_count = self->m_AliveCount;
    if (self->m_AliveCount == 0u) return nullptr;
    return self->m_AliveSnapshot.Data();
}

/**
 * m_Slots を走査し active な instance だけを m_AliveSnapshot に詰め直す。
 *
 * @details m_SnapshotDirtySize = m_AliveCount にすることで次回 AllAlive を cache hit させる。
 */
void FProjectileSystem::RebuildAliveSnapshot() noexcept {
    u32 written = 0u;
    const usize n = m_Slots.Size();
    for (usize i = 0; i < n; ++i) {
        if (!m_Slots[i].active) continue;
        m_AliveSnapshot[static_cast<usize>(written)] = m_Slots[i].inst;
        ++written;
        if (written >= m_AliveCount) break;  // 早期 exit (cache friendly)
    }
    m_SnapshotDirtySize = m_AliveCount;
}

/**
 * 全 alive 弾の物理積分・homing・命中・寿命処理を 1 ステップ進める。
 *
 * @details
 * 各 alive projectile に対し次を順に行う。
 *   1) gravity を velocity に加算 (semi-implicit Euler)。
 *   2) homing なら target 方向の単位ベクトルと現 velocity の単位ベクトルを
 *      homing_strength で LERP し、速度の大きさは維持。
 *   3) position += velocity * dt。
 *   4) HitTestFn を呼び、true なら hit_count++ / HitCallback 発火。pierces=false
 *      (または max_pierces 超過) なら despawn (ExpireCallback 不発火)。
 *   5) elapsed_sec += dt し、lifetime 超過なら despawn + ExpireCallback 発火。
 * 最後に snapshot を必ず再構築する。dt <= 0 / Init 前 (m_Capacity == 0) は no-op。
 * @param dt 経過秒 (0 以下は no-op)。
 */
void FProjectileSystem::Tick(f32 dt) noexcept {
    if (dt <= 0.0f) return;
    if (m_Capacity == 0u) return;

    const usize n = m_Slots.Size();
    for (usize i = 0; i < n; ++i) {
        Slot& s = m_Slots[i];
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

        // (1) 重力加算 (semi-implicit Euler: v <- v + g*dt before p <- p + v*dt)
        s.inst.velocity.y += gravity_y * dt;

        // (2) homing: target 方向に向き補正 (速度大きさは保持)
        if (homing && s.has_homing_target && homing_k > 0.0f) {
            const FVec2 to_target = s.homing_tgt - s.inst.position;
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

        // (3) 位置更新
        s.inst.position.x += s.inst.velocity.x * dt;
        s.inst.position.y += s.inst.velocity.y * dt;

        // (4) 命中チェック
        //
        // HitTestFn / HitCallback / ExpireCallback はユーザーコードで、再入で
        // Despawn(この弾) や Despawn+Spawn (slot 再利用で gen が進む) を行い得る。
        // 各コールバックの後に active と gen を再チェックし、既に despawn / 再利用
        // 済みならこの弾の処理を打ち切る (打ち切らないと despawn の二重実行で
        // m_AliveCount が二重デクリメントされ、AllAlive が生存弾を取りこぼす)。
        bool consumed = false;
        const u8 entry_gen = s.gen;
        if (m_HitTestFn != nullptr) {
            u32 target_id   = 0u;
            f32 damage_done = 0.0f;
            const bool hit = m_HitTestFn(m_HitTestUser, s.inst,
                                          target_id, damage_done);
            if (!s.active || s.gen != entry_gen) continue;
            if (hit) {
                ++s.inst.hit_count;
                // HitCallback 発火 (state 更新後)
                if (m_OnHit != nullptr) {
                    const FProjectileId pid = FProjectileId::Pack(
                        static_cast<u32>(i), s.gen);
                    m_OnHit(m_OnHitUser, pid, s.inst.def_id, target_id, damage_done);
                    if (!s.active || s.gen != entry_gen) continue;
                }
                // 貫通判定: pierces=false なら 1 hit で消滅。
                // pierces=true なら max_pierces+1 hit で消滅
                // (= max_pierces=2 → 3 体目で消える)。
                const u32 hit_cap = pierces ? (max_pierces + 1u) : 1u;
                if (s.inst.hit_count >= hit_cap) {
                    // 命中で despawn (ExpireCallback 不発火)
                    s.active            = false;
                    s.has_homing_target = false;
                    if (m_AliveCount > 0u) --m_AliveCount;
                    m_SnapshotDirtySize = 0u;
                    consumed = true;
                }
            }
        }
        if (consumed) continue;

        // (5) 寿命チェック
        s.inst.elapsed_sec += dt;
        if (lifetime > 0.0f && s.inst.elapsed_sec >= lifetime) {
            // ExpireCallback 発火 (state 更新前: callback 内で参照可能)
            if (m_OnExpire != nullptr) {
                const FProjectileId pid = FProjectileId::Pack(
                    static_cast<u32>(i), s.gen);
                m_OnExpire(m_OnExpireUser, pid, s.inst.def_id);
                if (!s.active || s.gen != entry_gen) continue;  // callback が despawn 済み
            }
            s.active            = false;
            s.has_homing_target = false;
            if (m_AliveCount > 0u) --m_AliveCount;
            m_SnapshotDirtySize = 0u;
        }
    }

    // Tick で alive bullet 全部の position を更新したので、snapshot は
    // alive_count が変わっていなくても **古いまま**。必ず rebuild する。
    // (これを忘れると AllAlive が古い position を返し、bullet が動いていない
    //  ように見える — 2026-05-26 ユーザー実機検証で発覚)
    RebuildAliveSnapshot();
}

/**
 * 命中判定コールバックを登録する (nullptr で登録解除)。
 *
 * @param fn 命中判定関数 (nullptr で解除)。
 * @param user fn に渡される user pointer。
 */
void FProjectileSystem::SetHitTestFn(HitTestFn fn, void* user) noexcept {
    m_HitTestFn   = fn;
    m_HitTestUser = user;
}

/**
 * 命中時コールバックを登録する (nullptr で登録解除)。
 *
 * @param cb 命中時に呼ばれるコールバック (nullptr で解除)。
 * @param user cb に渡される user pointer。
 */
void FProjectileSystem::SetOnHitCallback(HitCallback cb, void* user) noexcept {
    m_OnHit      = cb;
    m_OnHitUser = user;
}

/**
 * 寿命切れコールバックを登録する (nullptr で登録解除)。
 *
 * @param cb 寿命切れ時に呼ばれるコールバック (nullptr で解除)。
 * @param user cb に渡される user pointer。
 */
void FProjectileSystem::SetOnExpireCallback(ExpireCallback cb, void* user) noexcept {
    m_OnExpire      = cb;
    m_OnExpireUser = user;
}

/**
 * 弾の追尾ターゲット座標を設定する。
 *
 * @details
 * homing=true の def を持つ弾のみ反応する (それ以外は no-op)。stale handle も no-op。
 * 後で別の Spawn が同 slot を再利用すると has_homing_target は Spawn で false に戻される
 * ため、stale handle 経由のターゲット汚染は起きない。
 * @param id 対象の弾ハンドル。
 * @param target_pos 追尾先のワールド座標。
 */
void FProjectileSystem::SetHomingTarget(FProjectileId id, FVec2 target_pos) noexcept {
    Slot* s = FindSlot(id);
    if (s == nullptr) return;
    const ProjectileDef* def = FindDef(s->inst.def_id);
    if (def == nullptr || !def->homing) return;
    s->homing_tgt        = target_pos;
    s->has_homing_target = true;
}

/**
 * 全弾を inactive にして一括消去する (ExpireCallback は発火しない)。
 *
 * @details
 * gen は維持するため古い handle は次の Spawn 後に無効化される。callback / def 登録は
 * 維持する。サイレントな全消去。
 */
void FProjectileSystem::ClearAll() noexcept {
    const usize n = m_Slots.Size();
    for (usize i = 0; i < n; ++i) {
        m_Slots[i].active            = false;
        m_Slots[i].has_homing_target = false;
    }
    m_AliveCount         = 0u;
    m_SnapshotDirtySize = 0u;
}

} // namespace acs::game

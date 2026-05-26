// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R / I — FBuffSystem 実装
//
// 仕様の意図は BuffSystem.h を参照。本ファイルでは:
//   ・const char* per-byte 比較で FBuffDef registry / FOwnerSlot 内 buff を検索
//   ・generational owner slot の Acquire / Release (gen 1 以上、0 ラップで 1 に巻戻し)
//   ・ApplyBuff の 3 種 StackPolicy (Refresh / Stack / Ignore) の分岐
//   ・Tick 内で remaining_sec 減算 + tick_interval 累積消化 + swap-and-pop 期限切れ
// すべて noexcept、STL 不使用。
#include "gameframework/BuffSystem.h"

#include "foundation/Log.h"

namespace acs::game {

namespace {

// const char* の per-byte 安全比較。FEconomyDirector / FAchievementManager と同設計。
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

// 「未発見」を表す哨兵値。FEconomyDirector / FSeasonPass と同設計。
constexpr u32 kNotFound = ~static_cast<u32>(0);

} // namespace

// =============================================================================
// 検索 (private)
// =============================================================================

u32 FBuffSystem::FindBuffDefSlot(const char* buff_id) const noexcept {
    if (buff_id == nullptr) return kNotFound;
    const usize n = _registry.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(_registry[i].id, buff_id)) return static_cast<u32>(i);
    }
    return kNotFound;
}

FBuffSystem::FOwnerSlot* FBuffSystem::ResolveOwner(FBuffOwnerId owner) noexcept {
    if (!owner.IsValid()) return nullptr;
    const u32 idx = owner.Index();
    if (idx >= _owners.Size()) return nullptr;
    FOwnerSlot& s = _owners[static_cast<usize>(idx)];
    if (!s.in_use || s.gen != owner.Gen()) return nullptr;
    return &s;
}

const FBuffSystem::FOwnerSlot* FBuffSystem::ResolveOwner(FBuffOwnerId owner) const noexcept {
    if (!owner.IsValid()) return nullptr;
    const u32 idx = owner.Index();
    if (idx >= _owners.Size()) return nullptr;
    const FOwnerSlot& s = _owners[static_cast<usize>(idx)];
    if (!s.in_use || s.gen != owner.Gen()) return nullptr;
    return &s;
}

u32 FBuffSystem::FindBuffInstance(const FOwnerSlot& slot, const char* buff_id) noexcept {
    if (buff_id == nullptr) return kNotFound;
    const usize n = slot.buffs.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(slot.buffs[i].id, buff_id)) return static_cast<u32>(i);
    }
    return kNotFound;
}

// =============================================================================
// バフ定義
// =============================================================================

void FBuffSystem::RegisterBuff(const FBuffDef& def) noexcept {
    // defensive: id == nullptr は意味を持たないので静かに弾く。
    if (def.id == nullptr) return;

    // 同 id の 2 重登録は no-op (アセット二重ロード保護)。
    if (FindBuffDefSlot(def.id) != kNotFound) {
        ACS_LOG_WARN("FBuffSystem: duplicate buff registration ignored ('%s')", def.id);
        return;
    }

    // max_stack == 0 は defensive に 1 に正規化 (= 0 のまま記録すると Stack policy
    // で clamp に失敗して挙動が崩れる)。
    FBuffDef normalized = def;
    if (normalized.max_stack == 0u) normalized.max_stack = 1u;

    _registry.PushBack(normalized);
}

// =============================================================================
// owner 管理
// =============================================================================

FBuffOwnerId FBuffSystem::CreateOwner() noexcept {
    // 既存 inactive slot を線形探索で再利用 (= FParticleEffectSystem と同設計)。
    const usize n = _owners.Size();
    for (usize i = 0; i < n; ++i) {
        FOwnerSlot& s = _owners[i];
        if (s.in_use) continue;

        // gen を 1 進める (0 にラップしたら 1 に戻す)。0 は invalid なので配らない。
        u8 new_gen = static_cast<u8>(s.gen + 1u);
        if (new_gen == 0u) new_gen = 1u;
        s.gen    = new_gen;
        s.in_use = true;
        // 念のため再利用前の残骸を消す (DestroyOwner 時にも消すが二重保険)。
        s.buffs.Clear();
        return FBuffOwnerId::Pack(static_cast<u32>(i), new_gen);
    }

    // 末尾追加。24bit index 上限に達したら invalid。
    if (n >= static_cast<usize>(FBuffOwnerId::kMaxIndex)) {
        ACS_LOG_WARN("FBuffSystem: owner index space exhausted (>= 16M)");
        return FBuffOwnerId{};
    }
    _owners.PushBack({});
    FOwnerSlot& s = _owners[n];
    s.gen    = 1u;        // 新規 slot は gen=1 から開始
    s.in_use = true;
    return FBuffOwnerId::Pack(static_cast<u32>(n), 1u);
}

void FBuffSystem::DestroyOwner(FBuffOwnerId owner) noexcept {
    FOwnerSlot* s = ResolveOwner(owner);
    if (s == nullptr) return;

    // 「キャラ消滅」と「効果時間切れ」を区別するため、ここでは ExpireCallback を
    // 発火しない (仕様コメント参照)。buff 配列は破棄するだけ。
    s->buffs.Clear();
    s->in_use = false;
    // gen はここでは進めない。次の CreateOwner で +1 して払い出す
    // (= FParticleEffectSystem / FSceneTimer と同パターン)。
}

// =============================================================================
// バフの適用 / 除去
// =============================================================================

bool FBuffSystem::ApplyBuff(FBuffOwnerId owner, const char* buff_id) noexcept {
    FOwnerSlot* s = ResolveOwner(owner);
    if (s == nullptr) return false;

    const u32 def_slot = FindBuffDefSlot(buff_id);
    if (def_slot == kNotFound) return false;

    const FBuffDef& def = _registry[static_cast<usize>(def_slot)];

    // duration_sec <= 0 は「適用しても即時消滅」となるので拒否
    // (ApplyBuff/Tick/ExpireCallback の発火順がフレーム境界で曖昧になるのを防ぐ)。
    if (def.duration_sec <= 0.0f) return false;

    const u32 inst_slot = FindBuffInstance(*s, def.id);

    if (inst_slot == kNotFound) {
        // 新規追加。stack は 1 から開始 (max_stack >= 1 は RegisterBuff で保証済)。
        FBuffInstance bi;
        bi.id            = def.id;
        bi.remaining_sec = def.duration_sec;
        bi.tick_accum    = 0.0f;
        bi.stack         = 1u;
        s->buffs.PushBack(bi);
        return true;
    }

    // 既存あり → policy で分岐
    FBuffInstance& existing = s->buffs[static_cast<usize>(inst_slot)];
    switch (def.stack_policy) {
        case EBuffStackPolicy::Refresh:
            // タイマだけ巻き直し。stack は据置。
            existing.remaining_sec = def.duration_sec;
            // tick_accum は据置 (= 「次の毒 tick まで残り 0.3 秒」を保つ方が UX 上自然)。
            return true;

        case EBuffStackPolicy::Stack:
            // stack++ を max_stack で clamp。max_stack 到達済でも true を返す
            // (= 「掛け直し成功」として扱う、タイマは確実にリフレッシュされる)。
            if (existing.stack < def.max_stack) {
                ++existing.stack;
            }
            existing.remaining_sec = def.duration_sec;
            // tick_accum は据置 (Refresh と同方針)。
            return true;

        case EBuffStackPolicy::Ignore:
            // 既存があれば何もしない。
            return false;
    }
    // 上記 switch は全 enum を網羅。万一未知の値が来たら defensive に false。
    return false;
}

bool FBuffSystem::RemoveBuff(FBuffOwnerId owner, const char* buff_id) noexcept {
    FOwnerSlot* s = ResolveOwner(owner);
    if (s == nullptr) return false;
    if (buff_id == nullptr) return false;

    const u32 inst_slot = FindBuffInstance(*s, buff_id);
    if (inst_slot == kNotFound) return false;

    // 期限切れ callback 用に id を取り出してから swap-and-pop。
    // (TArray<FBuffInstance>::RemoveAtSwap は POD なので安全)。
    const char* removed_id = s->buffs[static_cast<usize>(inst_slot)].id;
    s->buffs.RemoveAtSwap(static_cast<usize>(inst_slot));

    // ExpireCallback はコールバック中に Apply/Remove が起きても安全になるよう、
    // 配列操作を済ませてから発火する (= 仕様コメント参照)。
    if (_on_expire != nullptr) {
        _on_expire(_on_expire_user, owner, removed_id);
    }
    return true;
}

// =============================================================================
// 照会
// =============================================================================

u32 FBuffSystem::BuffCountOnOwner(FBuffOwnerId owner) const noexcept {
    const FOwnerSlot* s = ResolveOwner(owner);
    if (s == nullptr) return 0u;
    return static_cast<u32>(s->buffs.Size());
}

bool FBuffSystem::HasBuff(FBuffOwnerId owner, const char* buff_id) const noexcept {
    const FOwnerSlot* s = ResolveOwner(owner);
    if (s == nullptr) return false;
    return FindBuffInstance(*s, buff_id) != kNotFound;
}

u32 FBuffSystem::GetStack(FBuffOwnerId owner, const char* buff_id) const noexcept {
    const FOwnerSlot* s = ResolveOwner(owner);
    if (s == nullptr) return 0u;
    const u32 inst_slot = FindBuffInstance(*s, buff_id);
    if (inst_slot == kNotFound) return 0u;
    return s->buffs[static_cast<usize>(inst_slot)].stack;
}

f32 FBuffSystem::GetRemaining(FBuffOwnerId owner, const char* buff_id) const noexcept {
    const FOwnerSlot* s = ResolveOwner(owner);
    if (s == nullptr) return 0.0f;
    const u32 inst_slot = FindBuffInstance(*s, buff_id);
    if (inst_slot == kNotFound) return 0.0f;
    return s->buffs[static_cast<usize>(inst_slot)].remaining_sec;
}

const FBuffInstance* FBuffSystem::AllBuffsOfOwner(FBuffOwnerId owner, u32& out_count) const noexcept {
    const FOwnerSlot* s = ResolveOwner(owner);
    if (s == nullptr) {
        out_count = 0u;
        return nullptr;
    }
    out_count = static_cast<u32>(s->buffs.Size());
    if (out_count == 0u) return nullptr;
    return s->buffs.Data();
}

void FBuffSystem::ClearAllOnOwner(FBuffOwnerId owner) noexcept {
    FOwnerSlot* s = ResolveOwner(owner);
    if (s == nullptr) return;
    // ExpireCallback は発火しない (= DestroyOwner と同じ「強制クリア」)。
    s->buffs.Clear();
}

// =============================================================================
// コールバック
// =============================================================================

void FBuffSystem::SetOnTickCallback(TickCallback cb, void* user) noexcept {
    _on_tick      = cb;
    _on_tick_user = user;
}

void FBuffSystem::SetOnExpireCallback(ExpireCallback cb, void* user) noexcept {
    _on_expire      = cb;
    _on_expire_user = user;
}

// =============================================================================
// driver
// =============================================================================

void FBuffSystem::Tick(f32 dt) noexcept {
    if (dt <= 0.0f) return;

    const usize owner_n = _owners.Size();
    for (usize oi = 0; oi < owner_n; ++oi) {
        FOwnerSlot& s = _owners[oi];
        if (!s.in_use) continue;

        // この owner の handle を再構成 (ExpireCallback で渡すため)。
        const FBuffOwnerId owner_handle = FBuffOwnerId::Pack(static_cast<u32>(oi), s.gen);

        // swap-and-pop で消す可能性があるため、index は降順走査が無難。
        // (前から走査 + swap-and-pop だと swap で前の方に来た要素を見落とす。)
        // ただし tick callback の発火順は仕様で規定していないので降順で OK。
        usize bi = s.buffs.Size();
        while (bi > 0) {
            --bi;
            FBuffInstance& b = s.buffs[bi];

            // remaining_sec を進める。
            b.remaining_sec -= dt;

            // tick callback 発火 — duration が残っている間に、tick_interval を
            // 消化できる回数分発火する。`def` は registry から find する
            // (FBuffInstance に kind/magnitude/interval を持たせると registry の
            // ホットスワップで挙動が変わって混乱するため、常に registry を参照)。
            const u32 def_slot = FindBuffDefSlot(b.id);
            if (def_slot != kNotFound) {
                const FBuffDef& def = _registry[static_cast<usize>(def_slot)];
                if (def.tick_interval_sec > 0.0f) {
                    b.tick_accum += dt;
                    while (b.tick_accum >= def.tick_interval_sec) {
                        b.tick_accum -= def.tick_interval_sec;
                        if (_on_tick != nullptr) {
                            _on_tick(_on_tick_user, owner_handle, b.id, b.stack, def.magnitude);
                        }
                    }
                }
            }

            // 期限切れ判定 → swap-and-pop + ExpireCallback
            if (b.remaining_sec <= 0.0f) {
                const char* expired_id = b.id;
                s.buffs.RemoveAtSwap(bi);
                if (_on_expire != nullptr) {
                    _on_expire(_on_expire_user, owner_handle, expired_id);
                }
                // bi はここまで「消したばかりの index」を指している。次の
                // ループ先頭の `--bi` で 1 つ前に進むため、追加の補正は不要。
                // (swap で末尾要素が bi に移ったが、これは降順走査ではこれから
                // 訪れる予定の位置ではないので問題ない。)
            }
        }
    }
}

// =============================================================================
// 全リセット
// =============================================================================

void FBuffSystem::ClearAll() noexcept {
    // 全 owner slot の buff 配列を破棄して in_use 落とし、registry も空に。
    // gen は維持しない (= ClearAll はテスト / シーン切替時の「丸ごとリセット」
    // セマンティクスで、stale handle 検出は呼出側の責任になる)。
    const usize n = _owners.Size();
    for (usize i = 0; i < n; ++i) {
        _owners[i].buffs.Clear();
        _owners[i].gen    = 0u;
        _owners[i].in_use = false;
    }
    _owners.Clear();
    _registry.Clear();
    _on_tick        = nullptr;
    _on_tick_user   = nullptr;
    _on_expire      = nullptr;
    _on_expire_user = nullptr;
}

} // namespace acs::game

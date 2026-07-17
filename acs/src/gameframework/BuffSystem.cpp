// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R / I — FBuffSystem 実装
//
// 仕様の意図は FBuffSystem.h を参照。本ファイルでは:
//   ・const char* per-byte 比較で FBuffDef registry / OwnerSlot 内 buff を検索
//   ・generational owner slot の Acquire / Release (gen 1 以上、0 ラップで 1 に巻戻し)
//   ・ApplyBuff の 3 種 StackPolicy (Refresh / Stack / Ignore) の分岐
//   ・Tick 内で remaining_sec 減算 + tick_interval 累積消化 + swap-and-pop 期限切れ
// すべて noexcept、STL 不使用。
#include "gameframework/BuffSystem.h"

#include "foundation/Log.h"

namespace acs::game {

namespace {

/**
 * const char* を per-byte で安全比較する。
 *
 * @details FEconomyDirector / FAchievementManager と同設計。
 * @param a 比較対象 1 (nullptr なら false)。
 * @param b 比較対象 2 (nullptr なら false)。
 * @return 両者が同じ内容の非 null 文字列なら true。
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

/** 「未発見」を表す哨兵値 (FEconomyDirector / FSeasonPass と同設計)。 */
constexpr u32 kNotFound = ~static_cast<u32>(0);

} // namespace

/** buff_id に一致する登録 def の slot を線形探索する (見つからなければ kNotFound)。 */
u32 FBuffSystem::FindBuffDefSlot(const char* buff_id) const noexcept {
    if (buff_id == nullptr) return kNotFound;
    const usize n = m_Registry.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(m_Registry[i].id, buff_id)) return static_cast<u32>(i);
    }
    return kNotFound;
}

/** owner handle を gen 照合しつつ有効な OwnerSlot へ解決する (無効なら nullptr)。 */
FBuffSystem::OwnerSlot* FBuffSystem::ResolveOwner(FBuffOwnerId owner) noexcept {
    if (!owner.IsValid()) return nullptr;
    const u32 idx = owner.Index();
    if (idx >= m_Owners.Size()) return nullptr;
    OwnerSlot& s = m_Owners[static_cast<usize>(idx)];
    if (!s.in_use || s.gen != owner.Gen()) return nullptr;
    return &s;
}

/** owner handle を gen 照合しつつ有効な OwnerSlot へ解決する (const 版、無効なら nullptr)。 */
const FBuffSystem::OwnerSlot* FBuffSystem::ResolveOwner(FBuffOwnerId owner) const noexcept {
    if (!owner.IsValid()) return nullptr;
    const u32 idx = owner.Index();
    if (idx >= m_Owners.Size()) return nullptr;
    const OwnerSlot& s = m_Owners[static_cast<usize>(idx)];
    if (!s.in_use || s.gen != owner.Gen()) return nullptr;
    return &s;
}

/** slot 内で buff_id に一致する buff instance の index を線形探索する (なければ kNotFound)。 */
u32 FBuffSystem::FindBuffInstance(const OwnerSlot& slot, const char* buff_id) noexcept {
    if (buff_id == nullptr) return kNotFound;
    const usize n = slot.buffs.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(slot.buffs[i].id, buff_id)) return static_cast<u32>(i);
    }
    return kNotFound;
}

/** buff def を registry に登録する (id==nullptr / 重複は弾き、max_stack==0 は 1 に正規化)。 */
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

    m_Registry.PushBack(normalized);
}

/** 非使用 slot を再利用 (なければ末尾追加) して新しい generational owner handle を払い出す。 */
FBuffOwnerId FBuffSystem::CreateOwner() noexcept {
    // 既存 inactive slot を線形探索で再利用 (= FParticleEffectSystem と同設計)。
    const usize n = m_Owners.Size();
    for (usize i = 0; i < n; ++i) {
        OwnerSlot& s = m_Owners[i];
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
    m_Owners.PushBack({});
    OwnerSlot& s = m_Owners[n];
    s.gen    = 1u;        // 新規 slot は gen=1 から開始
    s.in_use = true;
    return FBuffOwnerId::Pack(static_cast<u32>(n), 1u);
}

/** owner の buff を全破棄して slot を非使用に戻す (gen は据置、ExpireCallback は発火しない)。 */
void FBuffSystem::DestroyOwner(FBuffOwnerId owner) noexcept {
    OwnerSlot* s = ResolveOwner(owner);
    if (s == nullptr) return;

    // 「キャラ消滅」と「効果時間切れ」を区別するため、ここでは ExpireCallback を
    // 発火しない (仕様コメント参照)。buff 配列は破棄するだけ。
    s->buffs.Clear();
    s->in_use = false;
    // gen はここでは進めない。次の CreateOwner で +1 して払い出す
    // (= FParticleEffectSystem / FSceneTimer と同パターン)。
}

/** owner に buff を適用する。新規は追加、既存は StackPolicy (Refresh/Stack/Ignore) で分岐する。 */
bool FBuffSystem::ApplyBuff(FBuffOwnerId owner, const char* buff_id) noexcept {
    OwnerSlot* s = ResolveOwner(owner);
    if (s == nullptr) return false;

    const u32 def_slot = FindBuffDefSlot(buff_id);
    if (def_slot == kNotFound) return false;

    const FBuffDef& def = m_Registry[static_cast<usize>(def_slot)];

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

/** owner から buff を swap-and-pop で除去し、ExpireCallback を発火する。 */
bool FBuffSystem::RemoveBuff(FBuffOwnerId owner, const char* buff_id) noexcept {
    OwnerSlot* s = ResolveOwner(owner);
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
    if (m_OnExpire != nullptr) {
        m_OnExpire(m_OnExpireUser, owner, removed_id);
    }
    return true;
}

/** owner が持つ buff 数を返す (無効 owner なら 0)。 */
u32 FBuffSystem::BuffCountOnOwner(FBuffOwnerId owner) const noexcept {
    const OwnerSlot* s = ResolveOwner(owner);
    if (s == nullptr) return 0u;
    return static_cast<u32>(s->buffs.Size());
}

/** owner が指定 buff を持っているかを返す。 */
bool FBuffSystem::HasBuff(FBuffOwnerId owner, const char* buff_id) const noexcept {
    const OwnerSlot* s = ResolveOwner(owner);
    if (s == nullptr) return false;
    return FindBuffInstance(*s, buff_id) != kNotFound;
}

/** owner の指定 buff の現在 stack 数を返す (無ければ 0)。 */
u32 FBuffSystem::GetStack(FBuffOwnerId owner, const char* buff_id) const noexcept {
    const OwnerSlot* s = ResolveOwner(owner);
    if (s == nullptr) return 0u;
    const u32 inst_slot = FindBuffInstance(*s, buff_id);
    if (inst_slot == kNotFound) return 0u;
    return s->buffs[static_cast<usize>(inst_slot)].stack;
}

/** owner の指定 buff の残り秒を返す (無ければ 0)。 */
f32 FBuffSystem::GetRemaining(FBuffOwnerId owner, const char* buff_id) const noexcept {
    const OwnerSlot* s = ResolveOwner(owner);
    if (s == nullptr) return 0.0f;
    const u32 inst_slot = FindBuffInstance(*s, buff_id);
    if (inst_slot == kNotFound) return 0.0f;
    return s->buffs[static_cast<usize>(inst_slot)].remaining_sec;
}

/** owner の buff 配列先頭ポインタを返し、out_count に要素数を書き込む (空 / 無効なら nullptr)。 */
const FBuffInstance* FBuffSystem::AllBuffsOfOwner(FBuffOwnerId owner, u32& out_count) const noexcept {
    const OwnerSlot* s = ResolveOwner(owner);
    if (s == nullptr) {
        out_count = 0u;
        return nullptr;
    }
    out_count = static_cast<u32>(s->buffs.Size());
    if (out_count == 0u) return nullptr;
    return s->buffs.Data();
}

/** owner の buff を全消去する (ExpireCallback は発火しない強制クリア)。 */
void FBuffSystem::ClearAllOnOwner(FBuffOwnerId owner) noexcept {
    OwnerSlot* s = ResolveOwner(owner);
    if (s == nullptr) return;
    // ExpireCallback は発火しない (= DestroyOwner と同じ「強制クリア」)。
    s->buffs.Clear();
}

/** tick callback (定期発火) を設定する。 */
void FBuffSystem::SetOnTickCallback(TickCallback cb, void* user) noexcept {
    m_OnTick      = cb;
    m_OnTickUser = user;
}

/** expire callback (期限切れ / 除去時発火) を設定する。 */
void FBuffSystem::SetOnExpireCallback(ExpireCallback cb, void* user) noexcept {
    m_OnExpire      = cb;
    m_OnExpireUser = user;
}

/** 全 owner の buff の残り時間を減算し、tick interval 消化と期限切れ除去を行う。 */
void FBuffSystem::Tick(f32 dt) noexcept {
    if (dt <= 0.0f) return;

    // コールバックは «全 owner の配列走査を終えてから» 発火する。tick/expire
    // コールバックが ApplyBuff 等で s.buffs を再確保しても、走査中に保持している
    // FBuffInstance& が dangling (use-after-realloc) しないようにするため
    // (RemoveBuff と同じ「配列操作を済ませてから発火」規約を Tick へも適用する)。
    // 発火に必要な値だけを退避する — id は非所有の文字列リテラル、owner は値なので
    // 退避後に s.buffs が動いても安全。イベントが無ければ確保も起きない。
    struct FTickEvent   { FBuffOwnerId owner; const char* id; u32 stack; f32 magnitude; };
    struct FExpireEvent { FBuffOwnerId owner; const char* id; };
    TArray<FTickEvent>   tick_events;
    TArray<FExpireEvent> expire_events;

    const usize owner_n = m_Owners.Size();
    for (usize oi = 0; oi < owner_n; ++oi) {
        OwnerSlot& s = m_Owners[oi];
        if (!s.in_use) continue;

        // この owner の handle を再構成 (コールバックで渡すため)。
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

            // tick 発火予約 — duration が残っている間に、tick_interval を消化できる
            // 回数分イベントを積む。`def` は registry から find する (FBuffInstance に
            // kind/magnitude/interval を持たせると registry のホットスワップで挙動が
            // 変わって混乱するため、常に registry を参照)。
            const u32 def_slot = FindBuffDefSlot(b.id);
            if (def_slot != kNotFound) {
                const FBuffDef& def = m_Registry[static_cast<usize>(def_slot)];
                if (def.tick_interval_sec > 0.0f) {
                    b.tick_accum += dt;
                    while (b.tick_accum >= def.tick_interval_sec) {
                        b.tick_accum -= def.tick_interval_sec;
                        if (m_OnTick != nullptr) {
                            (void)tick_events.TryPushBack(
                                FTickEvent{ owner_handle, b.id, b.stack, def.magnitude });
                        }
                    }
                }
            }

            // 期限切れ判定 → swap-and-pop + ExpireCallback 発火予約
            if (b.remaining_sec <= 0.0f) {
                const char* expired_id = b.id;
                s.buffs.RemoveAtSwap(bi);
                if (m_OnExpire != nullptr) {
                    (void)expire_events.TryPushBack(FExpireEvent{ owner_handle, expired_id });
                }
                // bi はここまで「消したばかりの index」を指している。次の
                // ループ先頭の `--bi` で 1 つ前に進むため、追加の補正は不要。
                // (swap で末尾要素が bi に移ったが、これは降順走査ではこれから
                // 訪れる予定の位置ではないので問題ない。)
            }
        }
    }

    // 走査完了後にまとめて発火する。この時点で s.buffs への参照は保持していないので、
    // コールバックが ApplyBuff / RemoveBuff / DestroyOwner を呼んでも安全。
    for (usize i = 0; i < tick_events.Size(); ++i) {
        const FTickEvent& e = tick_events[i];
        m_OnTick(m_OnTickUser, e.owner, e.id, e.stack, e.magnitude);
    }
    for (usize i = 0; i < expire_events.Size(); ++i) {
        const FExpireEvent& e = expire_events[i];
        m_OnExpire(m_OnExpireUser, e.owner, e.id);
    }
}

/** 全 owner slot / registry / callback を初期状態に丸ごとリセットする。 */
void FBuffSystem::ClearAll() noexcept {
    // 全 owner slot の buff 配列を破棄して in_use 落とし、registry も空に。
    // gen は維持しない (= ClearAll はテスト / シーン切替時の「丸ごとリセット」
    // セマンティクスで、stale handle 検出は呼出側の責任になる)。
    const usize n = m_Owners.Size();
    for (usize i = 0; i < n; ++i) {
        m_Owners[i].buffs.Clear();
        m_Owners[i].gen    = 0u;
        m_Owners[i].in_use = false;
    }
    m_Owners.Clear();
    m_Registry.Clear();
    m_OnTick        = nullptr;
    m_OnTickUser   = nullptr;
    m_OnExpire      = nullptr;
    m_OnExpireUser = nullptr;
}

} // namespace acs::game

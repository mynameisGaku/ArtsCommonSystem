// SPDX-License-Identifier: Apache-2.0
// GameFramework 完成度システム v7 — CProgression 実装
//
// CAchievementManager / FEntitlement と同じ「Def + State の並行 TArray」pattern。
// id 比較は STL <cstring> も避けて per-byte ループを自前で書く (FEntitlement
// / CSettings と同じ StrEq pattern)。
//
// レベル計算 (floor(log2(xp + 1))) は浮動小数 log2 を使わずに、上位ビット位置
// を整数ループで走査する形で実装する。プラットフォーム固有の intrinsic
// (`_BitScanReverse` 等) は memory/Tlsf.cpp の範囲に閉じ込めて gameframework
// 側からは引かない方針 (依存最小化)。Milestone 達成判定は通常 1 セッション
// あたり数十〜数百回しか走らないので、ループのオーバーヘッドは無視できる。
#include "gameframework/Progression.h"
#include "gameframework/SaveArchive.h"
#include "platform/Time.h"
#include "container/Array.h"
#include "foundation/Log.h"

namespace acs::game {

namespace {

/**
 * const char* 同士を nullptr 安全に per-byte 比較する。
 *
 * @details STL <cstring> 禁止のため自前実装。終端ヌルまで一致した時のみ true。
 * @param a 比較対象の文字列 A。
 * @param b 比較対象の文字列 B。
 * @return 内容が完全一致すれば true。
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

/** CSaveArchive payload の schema バージョン (レイアウト変更時に bump)。 */
constexpr u32 kProgressionSaveVersion = 1u;

/**
 * id 文字列の FNV-1a 32bit ハッシュを計算する。
 *
 * @details
 * InputMap.h の ActionHash と同一規約 (offset basis 2166136261, prime 16777619)。
 * 永続化キーとして使い、リテラル提示順に依存しない安定 ID を得る。
 * @param id ハッシュ対象の文字列 (nullptr なら 0 を返す)。
 * @return id の FNV-1a 32bit ハッシュ。
 */
u32 HashId(const char* id) noexcept {
    if (id == nullptr) return 0u;
    u32 h = 2166136261u;
    while (*id != '\0') {
        h ^= static_cast<u32>(static_cast<unsigned char>(*id));
        h *= 16777619u;
        ++id;
    }
    return h;
}

/**
 * u32 を little-endian で 4 バイトへ書き込む。
 *
 * @details CSaveArchive と同じ LE 規約。シフトで書くのでホスト endianness 非依存。
 * @param p 書き込み先の4バイト。
 * @param v 書き出す u32 値。
 */
void WriteU32LE(u8* p, u32 v) noexcept {
    p[0] = static_cast<u8>(v         & 0xFFu);
    p[1] = static_cast<u8>((v >> 8)  & 0xFFu);
    p[2] = static_cast<u8>((v >> 16) & 0xFFu);
    p[3] = static_cast<u8>((v >> 24) & 0xFFu);
}

/**
 * u64 を little-endian で 8 バイトへ書き込む。
 *
 * @param p 書き込み先の8バイト。
 * @param v 書き出す u64 値。
 */
void WriteU64LE(u8* p, u64 v) noexcept {
    for (u32 i = 0; i < 8; ++i) {
        p[i] = static_cast<u8>((v >> (i * 8)) & 0xFFull);
    }
}

/**
 * little-endian の 4 バイトを u32 として読み出す。
 *
 * @details 境界チェックは呼出側の責務。
 * @param p 読み出し元の 4 バイト先頭ポインタ。
 * @return デコードした u32 値。
 */
u32 ReadU32LE(const u8* p) noexcept {
    return static_cast<u32>(p[0])
         | (static_cast<u32>(p[1]) << 8)
         | (static_cast<u32>(p[2]) << 16)
         | (static_cast<u32>(p[3]) << 24);
}

/**
 * little-endian の 8 バイトを u64 として読み出す。
 *
 * @details 境界チェックは呼出側の責務。
 * @param p 読み出し元の 8 バイト先頭ポインタ。
 * @return デコードした u64 値。
 */
u64 ReadU64LE(const u8* p) noexcept {
    u64 v = 0;
    for (u32 i = 0; i < 8; ++i) {
        v |= static_cast<u64>(p[i]) << (i * 8);
    }
    return v;
}

/**
 * 1 milestone entry のバイト数。
 *
 * @details id_hash(u32=4) + achieved(u8=1) + pad(3) + timestamp(u64=8) = 16 バイト。pad で timestamp を 8byte 境界に揃える。
 */
constexpr usize kEntrySize  = 16;

/** payload ヘッダのバイト数 (xp(u32) + count(u32) = 8)。 */
constexpr usize kHeaderPart = 8;

/**
 * Load が受け付ける canonical payload の最大バイト数。
 *
 * @details
 * header 8 バイト + kMaxPersistedMilestones × 16 バイト。entry count を読む前でも
 * domain上限を超える確保を拒否できる。
 */
constexpr u64 kMaxLoadPayloadBytes =
    static_cast<u64>(kHeaderPart)
    + static_cast<u64>(CProgression::kMaxPersistedMilestones)
        * static_cast<u64>(kEntrySize);

/** enum class を FErrorCode.subcode の u16 へ安全に縮約する。 */
constexpr u16 ProgressionSub(EProgressionPersistenceSubCode code) noexcept {
    return static_cast<u16>(static_cast<u32>(code));
}

/**
 * floor(log2(v)) を非負整数ループで算出する。
 *
 * @details v == 0 は呼出側で弾く前提 (log2(0) は未定義)。32bit 値なら最大 31 回ループするだけ。
 * @param v 対象の値 (1 以上であること)。
 * @return floor(log2(v))。
 */
u32 Floor_Log2_NonZero(u32 v) noexcept {
    u32 r = 0;
    while (v > 1u) {
        v >>= 1;
        ++r;
    }
    return r;
}

} // namespace

/** id に一致する Def の index を線形探索する (見つからなければ -1)。 */
isize CProgression::FindIndex(const char* id) const noexcept {
    if (id == nullptr) return -1;
    const usize n = m_Defs.Num();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(m_Defs[i].id, id)) return static_cast<isize>(i);
    }
    return -1;
}

/** milestone 定義を登録し、対応する State を 1:1 で追加する (id 重複は no-op)。 */
void CProgression::RegisterMilestone(const FMilestoneDef& def) noexcept {
    // id == nullptr は意味を持たないので静かに弾く (アセット欠損時等の保険)。
    if (def.id == nullptr) return;
    // 同 id の 2 重登録は no-op (CAchievementManager / CModRegistry と同じ防御)。
    if (FindIndex(def.id) >= 0) return;

    if (m_Defs.Num() != m_States.Num()) {
        ACS_LOG_ERROR("CProgression::RegisterMilestone: definition/state invariant broken");
        return;
    }
    if (m_Defs.Num() >= static_cast<usize>(kMaxPersistedMilestones)) {
        ACS_LOG_WARN("CProgression::RegisterMilestone: persistence limit reached (%u)",
                     kMaxPersistedMilestones);
        return;
    }

    const u32 new_hash = HashId(def.id);
    for (usize i = 0; i < m_Defs.Num(); ++i) {
        if (HashId(m_Defs[i].id) == new_hash) {
            ACS_LOG_ERROR("CProgression::RegisterMilestone: persistence hash collision "
                          "(new=%s, existing=%s, hash=0x%08x)",
                          def.id, m_Defs[i].id, new_hash);
            return;
        }
    }

    // State は Def と同 index に 1:1 で並ぶ。id は Def 側の文字列リテラルを
    // ポインタ参照コピーする (中身を duplicate しない)。
    FMilestoneState st;
    st.id                 = def.id;
    st.achieved           = false;
    st.achieved_timestamp = 0;

    const usize new_size = m_Defs.Num() + 1u;
    if (!m_Defs.TryReserve(new_size) || !m_States.TryReserve(new_size)) {
        ACS_LOG_ERROR("CProgression::RegisterMilestone: allocation failed");
        return;
    }
    if (!m_Defs.TryAdd(def)) {
        ACS_LOG_ERROR("CProgression::RegisterMilestone: definition append failed");
        return;
    }
    if (!m_States.TryAdd(st)) {
        m_Defs.Pop();
        ACS_LOG_ERROR("CProgression::RegisterMilestone: state append failed");
    }
}

/** 累計 XP を加算し (オーバーフローはクランプ)、未達成 milestone の達成判定を行う。 */
void CProgression::AwardXp(u32 amount) noexcept {
    if (amount == 0) return;

    // u32 オーバーフロー防御: amount を加算しても u32 範囲を超えそうなら
    // u32_max にクランプする (silent wrap で進行が巻き戻る事故を避ける)。
    constexpr u32 kMaxXp = static_cast<u32>(~0u);
    if (amount > kMaxXp - m_Xp) {
        m_Xp = kMaxXp;
    } else {
        m_Xp += amount;
    }

    // 全 milestone を線形走査して「未達成 → 達成」遷移を判定。
    // 累計 XP が required_xp 以上なら達成扱い。
    // timestamp は Clock::MillisSinceStartup() を 1 回だけ取得して全達成に
    // 同じ値を入れる (同フレームで複数達成しても順序情報は持たない設計)。
    const u64 now = ::acs::CClock::MillisSinceStartup();
    const usize n = m_Defs.Num();
    for (usize i = 0; i < n; ++i) {
        FMilestoneState& st  = m_States[i];
        const FMilestoneDef& d = m_Defs[i];
        if (st.achieved) continue;
        if (m_Xp < d.required_xp) continue;

        st.achieved           = true;
        st.achieved_timestamp = now;

        // FCallback 通知。設定されていなければ no-op。
        // 呼出中に callback がさらに AwardXp / RegisterMilestone を叩く可能性
        // はあるが、m_Defs / m_States は TArray 内部で再確保される場合があるため
        // callback 内での再入は推奨しない (API ドキュメント側で注意喚起)。
        // ただし最低限 m_Defs.Size() を毎ループ取り直すのではなく、最初に取った
        // n を信頼することで「callback 内 RegisterMilestone は次回 AwardXp で
        // 評価される」という挙動に固定する (現在の達成走査では拾わない)。
        if (m_OnAchieved != nullptr) {
            m_OnAchieved(m_OnAchievedUser, d.id);
        }
    }
}

/** 現在の累計 XP を返す。 */
u32 CProgression::CurrentXp() const noexcept {
    return m_Xp;
}

/** 累計 XP から floor(log2(xp + 1)) でレベルを算出する。 */
u32 CProgression::CurrentLevel() const noexcept {
    // floor(log2(xp + 1))。
    //   xp = 0   → log2(1)  = 0
    //   xp = 1   → log2(2)  = 1
    //   xp = 2   → log2(3)  ≈ 1.58 → 1
    //   xp = 3   → log2(4)  = 2
    //   xp = 7   → log2(8)  = 3
    //   xp = 15  → log2(16) = 4
    // xp = u32_max のとき xp + 1 が u32 でラップするので、ここだけ手動で
    // 32 を返す (= 32bit の上限を超えた場合の論理レベル)。
    constexpr u32 kMaxXp = static_cast<u32>(~0u);
    if (m_Xp == kMaxXp) return 32u;

    const u32 v = m_Xp + 1u;  // v >= 1 が保証される
    return Floor_Log2_NonZero(v);
}

/** 指定 id の milestone が達成済みかを返す (未登録なら false)。 */
bool CProgression::IsMilestoneAchieved(const char* id) const noexcept {
    const isize idx = FindIndex(id);
    if (idx < 0) return false;
    return m_States[static_cast<usize>(idx)].achieved;
}

/** 登録済み milestone の総数を返す。 */
u32 CProgression::MilestoneCount() const noexcept {
    // 件数は通常 u32 範囲を超えない (タイトル 1 つで通常 10〜100)。
    return static_cast<u32>(m_Defs.Num());
}

/** 達成済み milestone の数を数えて返す。 */
u32 CProgression::AchievedCount() const noexcept {
    u32 count = 0;
    const usize n = m_States.Num();
    for (usize i = 0; i < n; ++i) {
        if (m_States[i].achieved) ++count;
    }
    return count;
}

/** 指定 id の milestone 状態へのポインタを返す (未登録なら nullptr)。 */
const FMilestoneState* CProgression::GetState(const char* id) const noexcept {
    const isize idx = FindIndex(id);
    if (idx < 0) return nullptr;
    return &m_States[static_cast<usize>(idx)];
}

/** 全 milestone 状態の配列先頭を返し、件数を out_count に書き戻す。 */
const FMilestoneState* CProgression::AllStates(u32& out_count) const noexcept {
    out_count = static_cast<u32>(m_States.Num());
    return m_States.GetData();
}

/** XP を 0 に戻し全 milestone を未達成に戻す (定義配列は保持)。 */
void CProgression::ResetProgress() noexcept {
    m_Xp = 0;
    // 定義配列 (m_Defs) は保持。State 側だけ未達成に戻す。
    const usize n = m_States.Num();
    for (usize i = 0; i < n; ++i) {
        m_States[i].achieved           = false;
        m_States[i].achieved_timestamp = 0;
    }
}

/** milestone 達成時に呼ぶコールバックとユーザーポインタを登録する。 */
void CProgression::SetOnAchievedCallback(MilestoneCallback cb, void* user) noexcept {
    m_OnAchieved      = cb;
    m_OnAchievedUser = user;
}

/**
 * 進行状況を CSaveArchive 経由でバイナリファイルに保存する。
 *
 * @details
 * payload レイアウト (すべて little-endian、schema version = kProgressionSaveVersion):
 *
 *   offset  size  field        説明
 *   ------  ----  -----------  -------------------------------------------------
 *   0x00    4     xp           累計 XP (m_Xp)
 *   0x04    4     count        後続 entry 数 (= 登録 milestone 数)
 *   0x08    16*N  entries[N]   各 milestone の {id_hash, achieved, pad, timestamp}
 *
 *   1 entry (16 バイト):
 *     +0   4   id_hash             def.id の FNV-1a 32bit hash (永続キー)
 *     +4   1   achieved            0/1
 *     +5   3   pad                 ゼロ詰め (timestamp の 8byte 整列用)
 *     +8   8   achieved_timestamp  Clock::MillisSinceStartup() の値
 *
 * header / magic / CRC32 / atomic replace は CSaveArchive が担う。id は文字列
 * ではなく FNV-1a hash で書き出すことで、リテラルの提示順やアドレスに依存しない安定キーで
 * Load 時に突き合わせできる。
 * @param file_path 保存先ファイルパス。
 * @return 成功なら空の TResult、null パス / I/O 失敗ならエラー。
 */
TResult<void> CProgression::Save(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) {
        return ACS_ERR(IO,
                       static_cast<u16>(ESaveArchiveSubCode::kSubInvalidArgument),
                       "CProgression::Save: file_path is null");
    }

    const usize n = m_Defs.Num();
    if (n != m_States.Num()) {
        return ACS_ERR(Asset,
                       ProgressionSub(EProgressionPersistenceSubCode::kSubStateInvariant),
                       "CProgression::Save: definition/state invariant broken");
    }
    if (n > static_cast<usize>(kMaxPersistedMilestones)) {
        return ACS_ERR(Asset,
                       ProgressionSub(
                           EProgressionPersistenceSubCode::kSubMilestoneLimitExceeded),
                       "CProgression::Save: milestone count exceeds safety limit");
    }
    for (usize i = 0; i < n; ++i) {
        if (m_Defs[i].id == nullptr ||
            !StrEq(m_Defs[i].id, m_States[i].id) ||
            (!m_States[i].achieved && m_States[i].achieved_timestamp != 0u)) {
            return ACS_ERR(Asset,
                           ProgressionSub(
                               EProgressionPersistenceSubCode::kSubStateInvariant),
                           "CProgression::Save: milestone state is not canonical");
        }
    }

    constexpr usize kMaxUsize = static_cast<usize>(-1);
    if (n > (kMaxUsize - kHeaderPart) / kEntrySize) {
        return ACS_ERR(Container,
                       ProgressionSub(
                           EProgressionPersistenceSubCode::kSubMilestoneLimitExceeded),
                       "CProgression::Save: payload size calculation overflow");
    }
    const usize payload_size = kHeaderPart + n * kEntrySize;
    if (payload_size > static_cast<usize>(kMaxLoadPayloadBytes) ||
        payload_size > static_cast<usize>(CSaveArchive::kMaxPayloadSize)) {
        return ACS_ERR(Asset,
                       ProgressionSub(
                           EProgressionPersistenceSubCode::kSubMilestoneLimitExceeded),
                       "CProgression::Save: payload exceeds persistence limit");
    }

    // checked allocation 後に固定offsetへ書き、PushBack途中のOOMを排除する。
    TArray<u8> payload;
    if (!payload.TrySetNum(payload_size)) {
        return ACS_ERR(Memory,
                       ProgressionSub(EProgressionPersistenceSubCode::kSubAllocationFailed),
                       "CProgression::Save: payload allocation failed");
    }
    u8* const bytes = payload.GetData();
    WriteU32LE(bytes + 0, m_Xp);
    WriteU32LE(bytes + 4, static_cast<u32>(n));

    for (usize i = 0; i < n; ++i) {
        const FMilestoneState& st = m_States[i];
        u8* const entry = bytes + kHeaderPart + i * kEntrySize;
        WriteU32LE(entry + 0, HashId(m_Defs[i].id));
        entry[4] = st.achieved ? 1u : 0u;
        entry[5] = 0u;
        entry[6] = 0u;
        entry[7] = 0u;
        WriteU64LE(entry + 8, st.achieved_timestamp);
    }

    return CSaveArchive::WriteToFile(file_path,
                                     kProgressionSaveVersion,
                                     payload.GetData(),
                                     static_cast<u64>(payload_size));
}

/**
 * 保存ファイルから進行状況を復元する。
 *
 * @details
 * payload サイズを先読みして検証し、xp と各 entry を id_hash で現在登録済みの milestone に
 * 突き合わせて復元する。突き合わない id_hash (旧版で削除された milestone 等) は警告して skip。
 * @param file_path 読み込み元ファイルパス。
 * @return 成功なら空の TResult、null パス / 破損 / I/O 失敗ならエラー。
 */
TResult<void> CProgression::Load(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) {
        return ACS_ERR(IO,
                       static_cast<u16>(ESaveArchiveSubCode::kSubInvalidArgument),
                       "CProgression::Load: file_path is null");
    }

    // payload サイズを先読みしてバッファを確保する。
    const auto size_r = CSaveArchive::PeekPayloadSize(file_path);
    if (size_r.IsErr()) return size_r.Error();
    const u64 payload_size = size_r.Value();

    if (payload_size < static_cast<u64>(kHeaderPart)) {
        return ACS_ERR(Asset,
                       ProgressionSub(EProgressionPersistenceSubCode::kSubMalformedPayload),
                       "CProgression::Load: payload smaller than header");
    }
    if (payload_size > kMaxLoadPayloadBytes) {
        return ACS_ERR(Asset,
                       ProgressionSub(
                           EProgressionPersistenceSubCode::kSubMilestoneLimitExceeded),
                       "CProgression::Load: payload exceeds milestone safety limit");
    }
    if (payload_size > static_cast<u64>(static_cast<usize>(-1))) {
        return ACS_ERR(Container,
                       ProgressionSub(
                           EProgressionPersistenceSubCode::kSubMilestoneLimitExceeded),
                       "CProgression::Load: payload does not fit address space");
    }

    TArray<u8> payload;
    if (!payload.TrySetNum(static_cast<usize>(payload_size))) {
        return ACS_ERR(Memory,
                       ProgressionSub(EProgressionPersistenceSubCode::kSubAllocationFailed),
                       "CProgression::Load: payload allocation failed");
    }

    u64 actual_size = 0;
    const auto rd = CSaveArchive::ReadFromFile(file_path,
                                               payload.GetData(),
                                               payload_size,
                                               kProgressionSaveVersion,
                                               actual_size);
    if (rd.IsErr()) return rd.Error();
    if (actual_size != payload_size) {
        return ACS_ERR(Asset,
                       ProgressionSub(EProgressionPersistenceSubCode::kSubMalformedPayload),
                       "CProgression::Load: payload changed between peek and read");
    }

    const u8* p = payload.GetData();
    const u32 xp    = ReadU32LE(p + 0);
    const u32 count = ReadU32LE(p + 4);

    if (count > kMaxPersistedMilestones) {
        return ACS_ERR(Asset,
                       ProgressionSub(
                           EProgressionPersistenceSubCode::kSubMilestoneLimitExceeded),
                       "CProgression::Load: entry count exceeds safety limit");
    }
    const u64 entries_size = payload_size - static_cast<u64>(kHeaderPart);
    if ((entries_size % static_cast<u64>(kEntrySize)) != 0u ||
        entries_size / static_cast<u64>(kEntrySize) != static_cast<u64>(count)) {
        return ACS_ERR(Asset,
                       ProgressionSub(EProgressionPersistenceSubCode::kSubMalformedPayload),
                       "CProgression::Load: entry count does not exactly match payload");
    }

    const usize ns = m_States.Num();
    if (m_Defs.Num() != ns) {
        return ACS_ERR(Asset,
                       ProgressionSub(EProgressionPersistenceSubCode::kSubStateInvariant),
                       "CProgression::Load: definition/state invariant broken");
    }
    if (ns > static_cast<usize>(kMaxPersistedMilestones)) {
        return ACS_ERR(Asset,
                       ProgressionSub(
                           EProgressionPersistenceSubCode::kSubMilestoneLimitExceeded),
                       "CProgression::Load: registered milestone count exceeds safety limit");
    }

    // 既存状態と分離した staging を用意し、全entry検証後にだけcommitする。
    TArray<FMilestoneState> staged;
    TArray<u32> seen_hashes;
    if (!staged.TrySetNum(ns) || !seen_hashes.TrySetNum(static_cast<usize>(count))) {
        return ACS_ERR(Memory,
                       ProgressionSub(EProgressionPersistenceSubCode::kSubAllocationFailed),
                       "CProgression::Load: staging allocation failed");
    }
    for (usize i = 0; i < ns; ++i) {
        staged[i].id = m_Defs[i].id;
        staged[i].achieved = false;
        staged[i].achieved_timestamp = 0;
    }

    usize off = kHeaderPart;
    u32 unknown_count = 0;
    for (u32 e = 0; e < count; ++e) {
        const u8* ep            = p + off;
        const u32 id_hash       = ReadU32LE(ep + 0);
        const u8 achieved_byte  = ep[4];
        const u64 timestamp     = ReadU64LE(ep + 8);
        off += kEntrySize;

        if (achieved_byte > 1u || ep[5] != 0u || ep[6] != 0u || ep[7] != 0u ||
            (achieved_byte == 0u && timestamp != 0u)) {
            return ACS_ERR(Asset,
                           ProgressionSub(
                               EProgressionPersistenceSubCode::kSubMalformedPayload),
                           "CProgression::Load: non-canonical milestone entry");
        }
        for (u32 prior = 0; prior < e; ++prior) {
            if (seen_hashes[prior] == id_hash) {
                return ACS_ERR(Asset,
                               ProgressionSub(
                                   EProgressionPersistenceSubCode::kSubMalformedPayload),
                               "CProgression::Load: duplicate milestone hash");
            }
        }
        seen_hashes[e] = id_hash;

        bool matched = false;
        for (usize i = 0; i < ns; ++i) {
            if (HashId(m_Defs[i].id) == id_hash) {
                staged[i].achieved = achieved_byte == 1u;
                staged[i].achieved_timestamp = timestamp;
                matched = true;
                break;
            }
        }
        if (!matched) {
            ++unknown_count;
        }
    }

    // ここから先は失敗しない。全検証済みのstagingを一括反映する。
    m_Xp = xp;
    for (usize i = 0; i < ns; ++i) {
        m_States[i].achieved = staged[i].achieved;
        m_States[i].achieved_timestamp = staged[i].achieved_timestamp;
    }
    if (unknown_count > 0u) {
        ACS_LOG_WARN("CProgression::Load: %u unknown milestone entries skipped",
                     unknown_count);
    }

    return Ok();
}

} // namespace acs::game

// SPDX-License-Identifier: Apache-2.0
// GameFramework 完成度システム v7 — FProgression 実装
//
// FAchievementManager / FEntitlement と同じ「Def + State の並行 TArray」pattern。
// id 比較は STL <cstring> も避けて per-byte ループを自前で書く (FEntitlement
// / FSettings と同じ StrEq pattern)。
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

/** FSaveArchive payload の schema バージョン (レイアウト変更時に bump)。 */
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
 * u32 を little-endian で TArray<u8> 末尾に 4 バイト追記する。
 *
 * @details FSaveArchive と同じ LE 規約。シフトで書くのでホスト endianness 非依存。
 * @param buf 追記先のバイト列。
 * @param v 書き出す u32 値。
 */
void AppendU32LE(TArray<u8>& buf, u32 v) noexcept {
    buf.PushBack(static_cast<u8>(v        & 0xFFu));
    buf.PushBack(static_cast<u8>((v >> 8 ) & 0xFFu));
    buf.PushBack(static_cast<u8>((v >> 16) & 0xFFu));
    buf.PushBack(static_cast<u8>((v >> 24) & 0xFFu));
}

/**
 * u64 を little-endian で TArray<u8> 末尾に 8 バイト追記する。
 *
 * @param buf 追記先のバイト列。
 * @param v 書き出す u64 値。
 */
void AppendU64LE(TArray<u8>& buf, u64 v) noexcept {
    for (u32 i = 0; i < 8; ++i) {
        buf.PushBack(static_cast<u8>((v >> (i * 8)) & 0xFFull));
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
isize FProgression::FindIndex(const char* id) const noexcept {
    if (id == nullptr) return -1;
    const usize n = m_Defs.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(m_Defs[i].id, id)) return static_cast<isize>(i);
    }
    return -1;
}

/** milestone 定義を登録し、対応する State を 1:1 で追加する (id 重複は no-op)。 */
void FProgression::RegisterMilestone(const MilestoneDef& def) noexcept {
    // id == nullptr は意味を持たないので静かに弾く (アセット欠損時等の保険)。
    if (def.id == nullptr) return;
    // 同 id の 2 重登録は no-op (FAchievementManager / FModRegistry と同じ防御)。
    if (FindIndex(def.id) >= 0) return;

    m_Defs.PushBack(def);

    // State は Def と同 index に 1:1 で並ぶ。id は Def 側の文字列リテラルを
    // ポインタ参照コピーする (中身を duplicate しない)。
    MilestoneState st;
    st.id                 = def.id;
    st.achieved           = false;
    st.achieved_timestamp = 0;
    _states.PushBack(st);
}

/** 累計 XP を加算し (オーバーフローはクランプ)、未達成 milestone の達成判定を行う。 */
void FProgression::AwardXp(u32 amount) noexcept {
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
    const u64 now = ::acs::Clock::MillisSinceStartup();
    const usize n = m_Defs.Size();
    for (usize i = 0; i < n; ++i) {
        MilestoneState& st  = _states[i];
        const MilestoneDef& d = m_Defs[i];
        if (st.achieved) continue;
        if (m_Xp < d.required_xp) continue;

        st.achieved           = true;
        st.achieved_timestamp = now;

        // FCallback 通知。設定されていなければ no-op。
        // 呼出中に callback がさらに AwardXp / RegisterMilestone を叩く可能性
        // はあるが、m_Defs / _states は TArray 内部で再確保される場合があるため
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
u32 FProgression::CurrentXp() const noexcept {
    return m_Xp;
}

/** 累計 XP から floor(log2(xp + 1)) でレベルを算出する。 */
u32 FProgression::CurrentLevel() const noexcept {
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
bool FProgression::IsMilestoneAchieved(const char* id) const noexcept {
    const isize idx = FindIndex(id);
    if (idx < 0) return false;
    return _states[static_cast<usize>(idx)].achieved;
}

/** 登録済み milestone の総数を返す。 */
u32 FProgression::MilestoneCount() const noexcept {
    // 件数は通常 u32 範囲を超えない (タイトル 1 つで通常 10〜100)。
    return static_cast<u32>(m_Defs.Size());
}

/** 達成済み milestone の数を数えて返す。 */
u32 FProgression::AchievedCount() const noexcept {
    u32 count = 0;
    const usize n = _states.Size();
    for (usize i = 0; i < n; ++i) {
        if (_states[i].achieved) ++count;
    }
    return count;
}

/** 指定 id の milestone 状態へのポインタを返す (未登録なら nullptr)。 */
const MilestoneState* FProgression::GetState(const char* id) const noexcept {
    const isize idx = FindIndex(id);
    if (idx < 0) return nullptr;
    return &_states[static_cast<usize>(idx)];
}

/** 全 milestone 状態の配列先頭を返し、件数を out_count に書き戻す。 */
const MilestoneState* FProgression::AllStates(u32& out_count) const noexcept {
    out_count = static_cast<u32>(_states.Size());
    return _states.Data();
}

/** XP を 0 に戻し全 milestone を未達成に戻す (定義配列は保持)。 */
void FProgression::ResetProgress() noexcept {
    m_Xp = 0;
    // 定義配列 (m_Defs) は保持。State 側だけ未達成に戻す。
    const usize n = _states.Size();
    for (usize i = 0; i < n; ++i) {
        _states[i].achieved           = false;
        _states[i].achieved_timestamp = 0;
    }
}

/** milestone 達成時に呼ぶコールバックとユーザーポインタを登録する。 */
void FProgression::SetOnAchievedCallback(MilestoneCallback cb, void* user) noexcept {
    m_OnAchieved      = cb;
    m_OnAchievedUser = user;
}

/**
 * 進行状況を FSaveArchive 経由でバイナリファイルに保存する。
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
 * header / magic / CRC32 / atomic な truncate-write は FSaveArchive が担う。id は文字列
 * ではなく FNV-1a hash で書き出すことで、リテラルの提示順やアドレスに依存しない安定キーで
 * Load 時に突き合わせできる。
 * @param file_path 保存先ファイルパス。
 * @return 成功なら空の TResult、null パス / I/O 失敗ならエラー。
 */
TResult<void> FProgression::Save(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) {
        return ACS_ERR(IO, 0, "FProgression::Save: file_path is null");
    }

    const usize n = m_Defs.Size();

    // payload バッファを組み立てる (xp + count + N*entry)。
    TArray<u8> payload;
    payload.Reserve(kHeaderPart + n * kEntrySize);

    AppendU32LE(payload, m_Xp);
    AppendU32LE(payload, static_cast<u32>(n));

    for (usize i = 0; i < n; ++i) {
        const MilestoneState& st = _states[i];
        AppendU32LE(payload, HashId(m_Defs[i].id));
        payload.PushBack(st.achieved ? 1u : 0u);
        payload.PushBack(0u);  // pad
        payload.PushBack(0u);  // pad
        payload.PushBack(0u);  // pad
        AppendU64LE(payload, st.achieved_timestamp);
    }

    return FSaveArchive::WriteToFile(file_path,
                                     kProgressionSaveVersion,
                                     payload.Data(),
                                     static_cast<u64>(payload.Size()));
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
TResult<void> FProgression::Load(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) {
        return ACS_ERR(IO, 0, "FProgression::Load: file_path is null");
    }

    // payload サイズを先読みしてバッファを確保する。
    const auto size_r = FSaveArchive::PeekPayloadSize(file_path);
    if (size_r.IsErr()) return size_r.Error();
    const u64 payload_size = size_r.Value();

    if (payload_size < static_cast<u64>(kHeaderPart)) {
        return ACS_ERR(Asset, 0, "FProgression::Load: payload smaller than header");
    }

    TArray<u8> payload;
    payload.Resize(static_cast<usize>(payload_size));

    u64 actual_size = 0;
    const auto rd = FSaveArchive::ReadFromFile(file_path,
                                         payload.Data(),
                                         payload_size,
                                         kProgressionSaveVersion,
                                         actual_size);
    if (rd.IsErr()) return rd.Error();

    const u8* p = payload.Data();
    const u32 xp    = ReadU32LE(p + 0);
    const u32 count = ReadU32LE(p + 4);

    // entry 群が payload に収まることを検証 (破損 / 改竄に対する境界チェック)。
    const u64 need = static_cast<u64>(kHeaderPart)
                   + static_cast<u64>(count) * static_cast<u64>(kEntrySize);
    if (need > payload_size) {
        return ACS_ERR(Asset, 0, "FProgression::Load: entry count exceeds payload size");
    }

    // 検証を通ったので状態を反映する。まず全状態を未達成に戻し、
    // ファイルに記録された分だけ id_hash で突き合わせて復元する。
    m_Xp = xp;
    const usize ns = _states.Size();
    for (usize i = 0; i < ns; ++i) {
        _states[i].achieved           = false;
        _states[i].achieved_timestamp = 0;
    }

    usize off = kHeaderPart;
    for (u32 e = 0; e < count; ++e) {
        const u8* ep            = p + off;
        const u32 id_hash       = ReadU32LE(ep + 0);
        const bool achieved     = (ep[4] != 0u);
        const u64 timestamp     = ReadU64LE(ep + 8);
        off += kEntrySize;

        // 現在登録済みの milestone から同 hash を線形探索して復元する。
        // 見つからない id_hash (旧バージョンで削除された milestone 等) は
        // 警告して skip する。
        bool matched = false;
        for (usize i = 0; i < ns; ++i) {
            if (HashId(m_Defs[i].id) == id_hash) {
                _states[i].achieved           = achieved;
                _states[i].achieved_timestamp = timestamp;
                matched = true;
                break;
            }
        }
        if (!matched) {
            ACS_LOG_WARN("FProgression::Load: unknown milestone id_hash=0x%08x (skipped)",
                         id_hash);
        }
    }

    return Ok();
}

} // namespace acs::game

// SPDX-License-Identifier: Apache-2.0
// GameFramework 完成度システム v7 — FProgression 実装
//
// FAchievementManager / Entitlement と同じ「Def + FState の並行 TArray」pattern。
// id 比較は STL <cstring> も避けて per-byte ループを自前で書く (Entitlement
// / FSettings と同じ StrEq pattern)。
//
// レベル計算 (floor(log2(xp + 1))) は浮動小数 log2 を使わずに、上位ビット位置
// を整数ループで走査する形で実装する。プラットフォーム固有の intrinsic
// (`_BitScanReverse` 等) は memory/Tlsf.cpp の範囲に閉じ込めて gameframework
// 側からは引かない方針 (依存最小化)。Milestone 達成判定は通常 1 セッション
// あたり数十〜数百回しか走らないので、ループのオーバーヘッドは無視できる。
#include "gameframework/Progression.h"
#include "platform/Time.h"

namespace acs::game {

namespace {

// const char* の per-byte 比較。nullptr 安全。
// Entitlement.cpp / Settings.cpp と同じ実装 (STL <cstring> 禁止のため)。
bool StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

// floor(log2(v)) を非負整数ループで算出。
//   v == 0 は呼出側で弾く前提 (log2(0) は未定義)。
// 32bit 値なら最大 31 回ループするだけなので十分高速。
u32 Floor_Log2_NonZero(u32 v) noexcept {
    u32 r = 0;
    while (v > 1u) {
        v >>= 1;
        ++r;
    }
    return r;
}

} // namespace

// ============================================================================
// 内部検索ヘルパ
// ============================================================================
isize FProgression::FindIndex(const char* id) const noexcept {
    if (id == nullptr) return -1;
    const usize n = _defs.Size();
    for (usize i = 0; i < n; ++i) {
        if (StrEq(_defs[i].id, id)) return static_cast<isize>(i);
    }
    return -1;
}

// ============================================================================
// 定義登録
// ============================================================================
void FProgression::RegisterMilestone(const FMilestoneDef& def) noexcept {
    // id == nullptr は意味を持たないので静かに弾く (アセット欠損時等の保険)。
    if (def.id == nullptr) return;
    // 同 id の 2 重登録は no-op (FAchievementManager / FModRegistry と同じ防御)。
    if (FindIndex(def.id) >= 0) return;

    _defs.PushBack(def);

    // FState は Def と同 index に 1:1 で並ぶ。id は Def 側の文字列リテラルを
    // ポインタ参照コピーする (中身を duplicate しない)。
    FMilestoneState st;
    st.id                 = def.id;
    st.achieved           = false;
    st.achieved_timestamp = 0;
    _states.PushBack(st);
}

// ============================================================================
// XP 操作 + Milestone 達成判定
// ============================================================================
void FProgression::AwardXp(u32 amount) noexcept {
    if (amount == 0) return;

    // u32 オーバーフロー防御: amount を加算しても u32 範囲を超えそうなら
    // u32_max にクランプする (silent wrap で進行が巻き戻る事故を避ける)。
    constexpr u32 kMaxXp = static_cast<u32>(~0u);
    if (amount > kMaxXp - _xp) {
        _xp = kMaxXp;
    } else {
        _xp += amount;
    }

    // 全 milestone を線形走査して「未達成 → 達成」遷移を判定。
    // 累計 XP が required_xp 以上なら達成扱い。
    // timestamp は FClock::MillisSinceStartup() を 1 回だけ取得して全達成に
    // 同じ値を入れる (同フレームで複数達成しても順序情報は持たない設計)。
    const u64 now = ::acs::FClock::MillisSinceStartup();
    const usize n = _defs.Size();
    for (usize i = 0; i < n; ++i) {
        FMilestoneState& st  = _states[i];
        const FMilestoneDef& d = _defs[i];
        if (st.achieved) continue;
        if (_xp < d.required_xp) continue;

        st.achieved           = true;
        st.achieved_timestamp = now;

        // FCallback 通知。設定されていなければ no-op。
        // 呼出中に callback がさらに AwardXp / RegisterMilestone を叩く可能性
        // はあるが、_defs / _states は TArray 内部で再確保される場合があるため
        // callback 内での再入は推奨しない (API ドキュメント側で注意喚起)。
        // ただし最低限 _defs.Size() を毎ループ取り直すのではなく、最初に取った
        // n を信頼することで「callback 内 RegisterMilestone は次回 AwardXp で
        // 評価される」という挙動に固定する (現在の達成走査では拾わない)。
        if (_on_achieved != nullptr) {
            _on_achieved(_on_achieved_user, d.id);
        }
    }
}

// ============================================================================
// 照会
// ============================================================================
u32 FProgression::CurrentXp() const noexcept {
    return _xp;
}

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
    if (_xp == kMaxXp) return 32u;

    const u32 v = _xp + 1u;  // v >= 1 が保証される
    return Floor_Log2_NonZero(v);
}

bool FProgression::IsMilestoneAchieved(const char* id) const noexcept {
    const isize idx = FindIndex(id);
    if (idx < 0) return false;
    return _states[static_cast<usize>(idx)].achieved;
}

u32 FProgression::MilestoneCount() const noexcept {
    // 件数は通常 u32 範囲を超えない (タイトル 1 つで通常 10〜100)。
    return static_cast<u32>(_defs.Size());
}

u32 FProgression::AchievedCount() const noexcept {
    u32 count = 0;
    const usize n = _states.Size();
    for (usize i = 0; i < n; ++i) {
        if (_states[i].achieved) ++count;
    }
    return count;
}

const FMilestoneState* FProgression::GetState(const char* id) const noexcept {
    const isize idx = FindIndex(id);
    if (idx < 0) return nullptr;
    return &_states[static_cast<usize>(idx)];
}

const FMilestoneState* FProgression::AllStates(u32& out_count) const noexcept {
    out_count = static_cast<u32>(_states.Size());
    return _states.Data();
}

// ============================================================================
// リセット
// ============================================================================
void FProgression::ResetProgress() noexcept {
    _xp = 0;
    // 定義配列 (_defs) は保持。FState 側だけ未達成に戻す。
    const usize n = _states.Size();
    for (usize i = 0; i < n; ++i) {
        _states[i].achieved           = false;
        _states[i].achieved_timestamp = 0;
    }
}

// ============================================================================
// FCallback
// ============================================================================
void FProgression::SetOnAchievedCallback(MilestoneCallback cb, void* user) noexcept {
    _on_achieved      = cb;
    _on_achieved_user = user;
}

// ============================================================================
// 永続化 (Phase 2 で実装)
// ============================================================================
// Phase 1 は TODO スタブ。形だけ TResult<void> を返して呼出側の構造を
// 先に組めるようにする。Phase 2 で FSaveSlot<ProgressionSaveData> 経由の
// atomic write + 読み取りに接続する。
//
// 永続化対象 (Phase 2 設計案):
//   ・累計 XP (u32)
//   ・各 milestone の達成フラグと timestamp を id でキーにしたペア配列
//     → スキーマ進化を考えると単純な memcpy では足りないため、
//        FSaveArchive 経由の field-by-field writer を導入してから実装する。
TResult<void> FProgression::Save(const wchar_t* file_path) noexcept {
    (void)file_path;
    // TODO(Phase 2): FSaveSlot<ProgressionSaveData> 経由で atomic write。
    //   ・xp (u32)
    //   ・achieved_count (u32)
    //   ・[ {id_hash, achieved_flag, achieved_timestamp_ms}, ... ]
    //   id は文字列ではなく FNV-1a 等のハッシュ値で保存する想定 (リテラル文字列を
    //   そのまま書き出すと提示順次第で破損するため)。
    return Ok();
}

TResult<void> FProgression::Load(const wchar_t* file_path) noexcept {
    (void)file_path;
    // TODO(Phase 2): Save と対称な reader を実装。読み込んだ id_hash を
    //   現在登録済みの milestone 群と突き合わせ、一致した分だけ achieved /
    //   achieved_timestamp を復元する。未知の id_hash は警告して skip
    //   (旧バージョンで登録されたが現バージョンで削除された milestone)。
    return Ok();
}

} // namespace acs::game

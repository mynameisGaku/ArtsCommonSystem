// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R/O — CScoreSystem 実装
//
// 設計上のポイント (ヘッダの設計コメントと対応):
//   ・combo→倍率は f32 で計算するが、ScoreEntry には ×100 整数化して記録する
//     (= bit 一致を保証して Replay / Telemetry の偽差分を防ぐ)。
//   ・u64 加算は ~0ull クランプで安全側に。AddScore は base * multiplier_x100
//     を中間 u64 で計算し、/ 100 で丸めて weighted_value を得る。中間の
//     base * multiplier_x100 がオーバーフローする可能性があるため、上限判定
//     を入れている。
//   ・entry log は kMaxEntries で capped。超えたら先頭要素を 1 個削って末尾に
//     追加する単純実装 (= O(N) shift だが kMaxEntries=100 と小さく実害なし)。
//   ・milestone 通過判定は AddScore のたびに全件走査。件数は通常 5〜20 程度を
//     想定しており線形で十分。m_MilestoneHit を 1:1 並行 TArray で持つことで
//     「初回通過時のみ通知」を実現する。
//   ・WARN は他 Manager と同じ Log.h 経由。重複 milestone 登録時に出力する。
#include "gameframework/ScoreSystem.h"
#include "foundation/Log.h"

namespace acs::game {

namespace {

/** u64 全ビット 1 (オーバーフロー時のクランプ値)。 */
constexpr u64 kMaxU64 = ~static_cast<u64>(0);

/** デフォルト倍率関数の clamp 下限。 */
constexpr f32 kMinDefaultMult = 1.0f;

/** デフォルト倍率関数の clamp 上限。 */
constexpr f32 kMaxDefaultMult = 10.0f;

/**
 * u64 を飽和加算する (オーバーフロー時は kMaxU64 にクランプ)。
 *
 * @param a 加算元。
 * @param b 加算する値。
 * @return a + b (オーバーフローなら kMaxU64)。
 */
inline u64 SaturatingAddU64(u64 a, u64 b) noexcept {
    if (b > kMaxU64 - a) return kMaxU64;
    return a + b;
}

} // namespace

f32 CScoreSystem::DefaultMultiplierFn(u32 combo) noexcept {
    // 1.0 + combo * 0.1 を [1.0, 10.0] にクランプ。
    // combo == 0 (= NotifyHit 前 / NotifyMiss 直後) でも 1.0x が返る。
    f32 m = 1.0f + static_cast<f32>(combo) * 0.1f;
    if (m < kMinDefaultMult) m = kMinDefaultMult;
    if (m > kMaxDefaultMult) m = kMaxDefaultMult;
    return m;
}

void CScoreSystem::Init() noexcept {
    // Init は「最初の状態に戻す」セマンティクスで Reset + 設定リセット相当。
    // HighScore は保持する (= ゲーム再起動時に外部から SetHighScore で復元
    // される運用)。multiplier_fn / milestone callback もリセットして
    // 「呼出側が改めて Register / Set してくれ」のスタンスにする。
    m_CurrentScore   = 0;
    m_ComboCount     = 0;
    m_ComboTimer     = 0.0f;
    m_ComboDuration  = 3.0f;
    m_Entries.Clear();
    m_Milestones.Clear();
    m_MilestoneHit.Clear();
    m_MultiplierFn      = nullptr;
    m_OnMilestone       = nullptr;
    m_OnMilestoneUser  = nullptr;
}

void CScoreSystem::Reset() noexcept {
    // セッション開始の典型 reset: スコア / combo / entry / milestone 通過は
    // 消すが、HighScore / milestone 定義 / multiplier 関数 / callback は保持。
    m_CurrentScore = 0;
    m_ComboCount   = 0;
    m_ComboTimer   = 0.0f;
    m_Entries.Clear();
    // milestone 定義は保持、通過フラグだけ false に戻して再武装する。
    const usize n = m_MilestoneHit.Size();
    for (usize i = 0; i < n; ++i) {
        m_MilestoneHit[i] = false;
    }
}

void CScoreSystem::ClearAll() noexcept {
    // テスト / セーブデータ削除時の完全リセット。HighScore も含めて消す。
    m_CurrentScore      = 0;
    m_HighScore         = 0;
    m_ComboCount        = 0;
    m_ComboTimer        = 0.0f;
    m_ComboDuration     = 3.0f;
    m_Entries.Clear();
    m_Milestones.Clear();
    m_MilestoneHit.Clear();
    m_MultiplierFn      = nullptr;
    m_OnMilestone       = nullptr;
    m_OnMilestoneUser  = nullptr;
}

void CScoreSystem::AddScore(const char* category, u64 base_value) noexcept {
    // 現在倍率 (f32) を ×100 整数に変換。負値 / NaN は防御的に 100 (1.0x) に。
    const f32 mult_f = ComboMultiplier();
    u32 multiplier_x100;
    if (!(mult_f > 0.0f)) {  // NaN / 負値 / 0 はすべて此処に落ちる
        multiplier_x100 = 100;
    } else {
        // (mult_f * 100 + 0.5) で四捨五入。範囲は DefaultMultiplierFn なら
        // 100〜1000 だが、ユーザ提供 MultiplierFn は無制限のため u32 上限で
        // クランプする。
        const f32 m100f = mult_f * 100.0f + 0.5f;
        if (m100f >= 4294967295.0f) {
            multiplier_x100 = ~static_cast<u32>(0);
        } else {
            multiplier_x100 = static_cast<u32>(m100f);
        }
    }

    // weighted = base * multiplier_x100 / 100。
    // base * multiplier_x100 が u64 を超える場合は飽和。
    u64 weighted;
    if (base_value == 0 || multiplier_x100 == 0) {
        weighted = 0;
    } else if (base_value > kMaxU64 / static_cast<u64>(multiplier_x100)) {
        // 中間オーバーフロー → kMaxU64 にクランプ。
        weighted = kMaxU64;
    } else {
        weighted = (base_value * static_cast<u64>(multiplier_x100)) / 100ull;
    }

    // current_score に飽和加算。
    m_CurrentScore = SaturatingAddU64(m_CurrentScore, weighted);

    // HighScore 自動更新 (= SetHighScore とは別系統。ゲーム中の record 更新)。
    if (m_CurrentScore > m_HighScore) {
        m_HighScore = m_CurrentScore;
    }

    // entry log に記録 (base_value == 0 でも倍率履歴として残す方針)。
    FScoreEntry e{};
    e.category        = category;
    e.base_value      = base_value;
    e.weighted_value  = weighted;
    e.multiplier_x100 = multiplier_x100;
    PushEntry(e);

    // milestone 通過判定。
    CheckMilestones();
}

void CScoreSystem::NotifyHit() noexcept {
    // u32 オーバーフローは防御的に max クランプ (~0u)。実ゲームでは到達しないが、
    // 自動テストでの 100M 連打等を保護。
    if (m_ComboCount != ~static_cast<u32>(0)) {
        ++m_ComboCount;
    }
    m_ComboTimer = m_ComboDuration;
}

void CScoreSystem::NotifyMiss() noexcept {
    m_ComboCount = 0;
    m_ComboTimer = 0.0f;
}

void CScoreSystem::Tick(f32 dt) noexcept {
    if (!(dt > 0.0f)) return;  // 0 / 負 / NaN は no-op
    if (m_ComboTimer <= 0.0f) return;  // 既にリセット済

    m_ComboTimer -= dt;
    if (m_ComboTimer <= 0.0f) {
        // タイムアウト → コンボリセット (NotifyMiss と同等)。
        m_ComboCount = 0;
        m_ComboTimer = 0.0f;
    }
}

void CScoreSystem::SetComboDuration(f32 sec) noexcept {
    // 負値 / NaN は 0 に丸める (= コンボ無効化相当の挙動: NotifyHit 直後の
    // Tick で即リセットされる)。
    m_ComboDuration = (sec > 0.0f) ? sec : 0.0f;
}

f32 CScoreSystem::ComboMultiplier() const noexcept {
    // 関数が差し替えられていればそれを優先、未設定なら内部デフォルト。
    MultiplierFn fn = (m_MultiplierFn != nullptr) ? m_MultiplierFn : &DefaultMultiplierFn;
    return fn(m_ComboCount);
}

void CScoreSystem::SetMultiplierFn(MultiplierFn fn) noexcept {
    // nullptr で内部デフォルトに戻る (ヘッダ仕様)。
    m_MultiplierFn = fn;
}

u32 CScoreSystem::EntryCount() const noexcept {
    return static_cast<u32>(m_Entries.Size());
}

const FScoreEntry* CScoreSystem::AllEntries(u32& out_count) const noexcept {
    out_count = static_cast<u32>(m_Entries.Size());
    return m_Entries.Data();
}

void CScoreSystem::PushEntry(const FScoreEntry& e) noexcept {
    if (m_Entries.Size() >= kMaxEntries) {
        // 最古を捨てる単純実装。kMaxEntries=100 なので memmove コストは
        // 実害なしの範囲。リング化はプロファイラで実測されたら検討。
        const usize n = m_Entries.Size();
        for (usize i = 1; i < n; ++i) {
            m_Entries[i - 1] = m_Entries[i];
        }
        m_Entries[n - 1] = e;
    } else {
        m_Entries.PushBack(e);
    }
}

void CScoreSystem::RegisterMilestone(u64 milestone_score) noexcept {
    // 0 は意味不明 (= 初期状態で即通過してしまう) なので無視。
    if (milestone_score == 0) return;

    // 重複登録は no-op + WARN (他 Manager と同パターン)。
    const usize n = m_Milestones.Size();
    for (usize i = 0; i < n; ++i) {
        if (m_Milestones[i] == milestone_score) {
            ACS_LOG_WARN("CScoreSystem: duplicate milestone ignored (%llu)",
                         static_cast<unsigned long long>(milestone_score));
            return;
        }
    }

    m_Milestones.PushBack(milestone_score);
    // 通過フラグも 1:1 で追加。既に CurrentScore がこの milestone を超えていても
    // 「登録時点では未通過扱い」にして、次の AddScore で初回通過させる方針。
    // (登録時に即発火すると順序依存になり予測しづらいため)
    m_MilestoneHit.PushBack(false);
}

void CScoreSystem::SetOnMilestoneCallback(MilestoneCallback cb, void* user) noexcept {
    m_OnMilestone      = cb;
    m_OnMilestoneUser = user;
}

void CScoreSystem::CheckMilestones() noexcept {
    if (m_OnMilestone == nullptr) {
        // callback 未登録なら通過フラグだけ更新する (= 後から callback を attach
        // しても過去の通過を再通知しない、という単方向設計)。
        const usize n = m_Milestones.Size();
        for (usize i = 0; i < n; ++i) {
            if (!m_MilestoneHit[i] && m_CurrentScore >= m_Milestones[i]) {
                m_MilestoneHit[i] = true;
            }
        }
        return;
    }

    // サイズはキャッシュせず毎周読む: callback (ユーザーコード) が再入で
    // ClearAll / RegisterMilestone を呼ぶと配列長が変わるため、キャッシュした
    // 長さで回すと縮小時に範囲外アクセスになる。
    for (usize i = 0; i < m_Milestones.Size(); ++i) {
        if (m_MilestoneHit[i]) continue;
        if (m_CurrentScore >= m_Milestones[i]) {
            m_MilestoneHit[i] = true;
            // callback 内で再帰的に AddScore を呼ばれても再入安全になるよう、
            // フラグを立ててから呼ぶ。
            m_OnMilestone(m_OnMilestoneUser, m_Milestones[i], m_CurrentScore);
        }
    }
}

} // namespace acs::game

// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R/O — ScoreSystem 実装
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
//     想定しており線形で十分。_milestone_hit を 1:1 並行 TArray で持つことで
//     「初回通過時のみ通知」を実現する。
//   ・WARN は他 Manager と同じ Log.h 経由。重複 milestone 登録時に出力する。
#include "gameframework/ScoreSystem.h"
#include "foundation/Log.h"

namespace acs::game {

namespace {

// u64 全ビット 1 (= オーバーフロー時のクランプ値)。
constexpr u64 kMaxU64 = ~static_cast<u64>(0);

// 倍率関数の clamp 範囲 (= デフォルト関数で使用)。
constexpr f32 kMinDefaultMult = 1.0f;
constexpr f32 kMaxDefaultMult = 10.0f;

// u64 を飽和加算 (= overflow → kMaxU64)。
inline u64 SaturatingAddU64(u64 a, u64 b) noexcept {
    if (b > kMaxU64 - a) return kMaxU64;
    return a + b;
}

} // namespace

// =============================================================================
// デフォルト倍率関数
// =============================================================================

f32 ScoreSystem::DefaultMultiplierFn(u32 combo) noexcept {
    // 1.0 + combo * 0.1 を [1.0, 10.0] にクランプ。
    // combo == 0 (= NotifyHit 前 / NotifyMiss 直後) でも 1.0x が返る。
    f32 m = 1.0f + static_cast<f32>(combo) * 0.1f;
    if (m < kMinDefaultMult) m = kMinDefaultMult;
    if (m > kMaxDefaultMult) m = kMaxDefaultMult;
    return m;
}

// =============================================================================
// 初期化 / リセット
// =============================================================================

void ScoreSystem::Init() noexcept {
    // Init は「最初の状態に戻す」セマンティクスで Reset + 設定リセット相当。
    // HighScore は保持する (= ゲーム再起動時に外部から SetHighScore で復元
    // される運用)。multiplier_fn / milestone callback もリセットして
    // 「呼出側が改めて Register / Set してくれ」のスタンスにする。
    _current_score   = 0;
    _combo_count     = 0;
    _combo_timer     = 0.0f;
    _combo_duration  = 3.0f;
    _entries.Clear();
    _milestones.Clear();
    _milestone_hit.Clear();
    _multiplier_fn      = nullptr;
    _on_milestone       = nullptr;
    _on_milestone_user  = nullptr;
}

void ScoreSystem::Reset() noexcept {
    // セッション開始の典型 reset: スコア / combo / entry / milestone 通過は
    // 消すが、HighScore / milestone 定義 / multiplier 関数 / callback は保持。
    _current_score = 0;
    _combo_count   = 0;
    _combo_timer   = 0.0f;
    _entries.Clear();
    // milestone 定義は保持、通過フラグだけ false に戻して再武装する。
    const usize n = _milestone_hit.Size();
    for (usize i = 0; i < n; ++i) {
        _milestone_hit[i] = false;
    }
}

void ScoreSystem::ClearAll() noexcept {
    // テスト / セーブデータ削除時の完全リセット。HighScore も含めて消す。
    _current_score      = 0;
    _high_score         = 0;
    _combo_count        = 0;
    _combo_timer        = 0.0f;
    _combo_duration     = 3.0f;
    _entries.Clear();
    _milestones.Clear();
    _milestone_hit.Clear();
    _multiplier_fn      = nullptr;
    _on_milestone       = nullptr;
    _on_milestone_user  = nullptr;
}

// =============================================================================
// スコア加算
// =============================================================================

void ScoreSystem::AddScore(const char* category, u64 base_value) noexcept {
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
    _current_score = SaturatingAddU64(_current_score, weighted);

    // HighScore 自動更新 (= SetHighScore とは別系統。ゲーム中の record 更新)。
    if (_current_score > _high_score) {
        _high_score = _current_score;
    }

    // entry log に記録 (base_value == 0 でも倍率履歴として残す方針)。
    ScoreEntry e{};
    e.category        = category;
    e.base_value      = base_value;
    e.weighted_value  = weighted;
    e.multiplier_x100 = multiplier_x100;
    PushEntry(e);

    // milestone 通過判定。
    CheckMilestones();
}

// =============================================================================
// コンボ操作
// =============================================================================

void ScoreSystem::NotifyHit() noexcept {
    // u32 オーバーフローは防御的に max クランプ (~0u)。実ゲームでは到達しないが、
    // 自動テストでの 100M 連打等を保護。
    if (_combo_count != ~static_cast<u32>(0)) {
        ++_combo_count;
    }
    _combo_timer = _combo_duration;
}

void ScoreSystem::NotifyMiss() noexcept {
    _combo_count = 0;
    _combo_timer = 0.0f;
}

void ScoreSystem::Tick(f32 dt) noexcept {
    if (!(dt > 0.0f)) return;  // 0 / 負 / NaN は no-op
    if (_combo_timer <= 0.0f) return;  // 既にリセット済

    _combo_timer -= dt;
    if (_combo_timer <= 0.0f) {
        // タイムアウト → コンボリセット (NotifyMiss と同等)。
        _combo_count = 0;
        _combo_timer = 0.0f;
    }
}

void ScoreSystem::SetComboDuration(f32 sec) noexcept {
    // 負値 / NaN は 0 に丸める (= コンボ無効化相当の挙動: NotifyHit 直後の
    // Tick で即リセットされる)。
    _combo_duration = (sec > 0.0f) ? sec : 0.0f;
}

f32 ScoreSystem::ComboMultiplier() const noexcept {
    // 関数が差し替えられていればそれを優先、未設定なら内部デフォルト。
    MultiplierFn fn = (_multiplier_fn != nullptr) ? _multiplier_fn : &DefaultMultiplierFn;
    return fn(_combo_count);
}

void ScoreSystem::SetMultiplierFn(MultiplierFn fn) noexcept {
    // nullptr で内部デフォルトに戻る (ヘッダ仕様)。
    _multiplier_fn = fn;
}

// =============================================================================
// entry log
// =============================================================================

u32 ScoreSystem::EntryCount() const noexcept {
    return static_cast<u32>(_entries.Size());
}

const ScoreEntry* ScoreSystem::AllEntries(u32& out_count) const noexcept {
    out_count = static_cast<u32>(_entries.Size());
    return _entries.Data();
}

void ScoreSystem::PushEntry(const ScoreEntry& e) noexcept {
    if (_entries.Size() >= kMaxEntries) {
        // 最古を捨てる単純実装。kMaxEntries=100 なので memmove コストは
        // 実害なしの範囲。リング化はプロファイラで実測されたら検討。
        const usize n = _entries.Size();
        for (usize i = 1; i < n; ++i) {
            _entries[i - 1] = _entries[i];
        }
        _entries[n - 1] = e;
    } else {
        _entries.PushBack(e);
    }
}

// =============================================================================
// milestone
// =============================================================================

void ScoreSystem::RegisterMilestone(u64 milestone_score) noexcept {
    // 0 は意味不明 (= 初期状態で即通過してしまう) なので無視。
    if (milestone_score == 0) return;

    // 重複登録は no-op + WARN (他 Manager と同パターン)。
    const usize n = _milestones.Size();
    for (usize i = 0; i < n; ++i) {
        if (_milestones[i] == milestone_score) {
            ACS_LOG_WARN("ScoreSystem: duplicate milestone ignored (%llu)",
                         static_cast<unsigned long long>(milestone_score));
            return;
        }
    }

    _milestones.PushBack(milestone_score);
    // 通過フラグも 1:1 で追加。既に CurrentScore がこの milestone を超えていても
    // 「登録時点では未通過扱い」にして、次の AddScore で初回通過させる方針。
    // (登録時に即発火すると順序依存になり予測しづらいため)
    _milestone_hit.PushBack(false);
}

void ScoreSystem::SetOnMilestoneCallback(MilestoneCallback cb, void* user) noexcept {
    _on_milestone      = cb;
    _on_milestone_user = user;
}

void ScoreSystem::CheckMilestones() noexcept {
    if (_on_milestone == nullptr) {
        // callback 未登録なら通過フラグだけ更新する (= 後から callback を attach
        // しても過去の通過を再通知しない、という単方向設計)。
        const usize n = _milestones.Size();
        for (usize i = 0; i < n; ++i) {
            if (!_milestone_hit[i] && _current_score >= _milestones[i]) {
                _milestone_hit[i] = true;
            }
        }
        return;
    }

    const usize n = _milestones.Size();
    for (usize i = 0; i < n; ++i) {
        if (_milestone_hit[i]) continue;
        if (_current_score >= _milestones[i]) {
            _milestone_hit[i] = true;
            // callback 内で再帰的に AddScore を呼ばれても再入安全になるよう、
            // フラグを立ててから呼ぶ。
            _on_milestone(_on_milestone_user, _milestones[i], _current_score);
        }
    }
}

} // namespace acs::game

// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — FReplayDirector 実装 (Phase R-3 スケルトン)
//
// 現フェーズ:
//   ・Init / StartRecording / StopRecording / StartPlayback /
//     PausePlayback / ResumePlayback / StopPlayback /
//     SetPlaybackSpeed / SeekToTick / Tick / ProgressNormalized は本実装。
//   ・SaveReplay / LoadReplay は ACS_ERR(IO, kSub_NotImplemented) を返す stub。
//     Phase R-4 で FInputRecorder / FLockstep の SaveToBuffer / LoadFromBuffer と
//     metadata sidecar (.meta) の bit-precise layout を接続予定。
//
// 設計メモ:
//   ・Mode 遷移は明示的: Idle → (StartRecording) → Recording → (StopRecording) → Idle
//                       Idle → (StartPlayback)  → Playback  → (StopPlayback)  → Idle
//                       Playback ↔ (Pause/Resume) ↔ Paused
//                       Paused → (StopPlayback) → Idle
//     Recording から Playback への直接遷移、および Idle から Pause/Resume への
//     遷移は kSub_BadMode で拒否 (一旦 Stop を挟むことを強制)。
//   ・Tick(dt) は録画中と再生中で意味が変わる:
//     - Recording: _tick_rate_hz * dt 分の tick を _current_tick に加算する。
//       実際の入力 capture は FInputRecorder / FLockstep 側で別途行う。
//     - Playback: dt * _playback_speed を accumulator に貯め、tick_rate_hz 単位
//       で _current_tick に加算する。duration_ticks に達したら自動 Stop。
//   ・SetPlaybackSpeed は { 0.25, 0.5, 1, 2, 4, 8, 16 } 等を想定するが、
//     UI スクラブで任意 f32 が来ても扱えるよう範囲 clamp する (0.25 未満は
//     0.25、16 超は 16)。0 や負値は 1.0 にリセットして "誤呼び出しからの自動
//     復帰" を担保する。
//   ・SeekToTick は duration_ticks を上限に clamp。Mode は変えない (Paused 中
//     の scrub もできるよう)。accumulator も 0 にリセットする (sub-tick を
//     持ち越さない)。
//   ・ProgressNormalized は duration_ticks == 0 のとき 0.0 を返す (録画開始
//     直後 / metadata 未設定の安全側)。
#include "gameframework/ReplayDirector.h"

namespace acs::game {

namespace {

// 再生速度の clamp 範囲。0.25x / 0.5x / 1x / 2x / 4x / 8x / 16x の範囲外を弾く。
constexpr f32 kMinSpeed = 0.25f;
constexpr f32 kMaxSpeed = 16.0f;

// SetPlaybackSpeed の範囲 clamp + 異常値ガード。
inline f32 ClampSpeed(f32 v) noexcept {
    // 0 / 負値 / NaN は誤呼び出しとして 1.0 に強制復帰。Pause は別 API なので
    // ここで 0 を許容すると意味が二重になる。
    if (!(v > 0.0f)) {
        return 1.0f;  // NaN 比較は false になるので NaN もこの枝で 1.0 に。
    }
    if (v < kMinSpeed) return kMinSpeed;
    if (v > kMaxSpeed) return kMaxSpeed;
    return v;
}

} // namespace

// -----------------------------------------------------------------------------
// 初期化
// -----------------------------------------------------------------------------

void FReplayDirector::Init() noexcept {
    _mode             = EReplayMode::Idle;
    _metadata         = FReplayMetadata{};   // 全 field を default に戻す
    _current_tick     = 0;
    _playback_speed   = 1.0f;
    _tick_accumulator = 0.0f;
    _tick_rate_hz     = 60;
}

// -----------------------------------------------------------------------------
// 録画開始 / 停止
// -----------------------------------------------------------------------------

TResult<void> FReplayDirector::StartRecording(const FReplayMetadata& meta) noexcept {
    // Idle 以外からの直接遷移は禁止。Recording 中の再開や Playback 中の
    // 切り替えは意図しない上書きが起こりやすいため、明示的な Stop を強制する。
    if (_mode != EReplayMode::Idle) {
        return ACS_ERR(Generic, kSub_BadMode,
                       "FReplayDirector::StartRecording: must be Idle");
    }
    _mode             = EReplayMode::Recording;
    _metadata         = meta;            // POD copy (const char* はポインタコピー)
    _current_tick     = 0;
    _tick_accumulator = 0.0f;
    // _playback_speed は触らない (録画中は無意味だが、StartPlayback 後にも
    // 直前の倍速を引き継ぎたい UX を許容する)。
    return TResult<void>{};
}

TResult<void> FReplayDirector::StopRecording() noexcept {
    // Recording 以外からの呼び出しは誤用扱い。Playback 中に StopRecording を
    // 呼んでしまった場合に黙って Idle にすると metadata が壊れるため明示エラー。
    if (_mode != EReplayMode::Recording) {
        return ACS_ERR(Generic, kSub_BadMode,
                       "FReplayDirector::StopRecording: must be Recording");
    }
    // 録画した tick 数を確定。LoadReplay 後の再生で ProgressNormalized が
    // 正しく計算できるよう metadata に焼き込む。
    _metadata.duration_ticks = _current_tick;
    _mode                    = EReplayMode::Idle;
    _tick_accumulator        = 0.0f;
    return TResult<void>{};
}

// -----------------------------------------------------------------------------
// 再生開始 / 一時停止 / 停止
// -----------------------------------------------------------------------------

TResult<void> FReplayDirector::StartPlayback() noexcept {
    if (_mode != EReplayMode::Idle) {
        return ACS_ERR(Generic, kSub_BadMode,
                       "FReplayDirector::StartPlayback: must be Idle");
    }
    _mode             = EReplayMode::Playback;
    _current_tick     = 0;
    _tick_accumulator = 0.0f;
    // _metadata はそのまま (StartRecording 直後の再生、または LoadReplay 後の
    // 再生いずれも metadata が既に正しく設定されている前提)。
    return TResult<void>{};
}

void FReplayDirector::PausePlayback() noexcept {
    // Playback 以外では no-op (Paused 中の再 Pause / Idle 中の誤呼び出しを許容)。
    if (_mode == EReplayMode::Playback) {
        _mode = EReplayMode::Paused;
    }
}

void FReplayDirector::ResumePlayback() noexcept {
    // Paused 以外では no-op。
    if (_mode == EReplayMode::Paused) {
        _mode = EReplayMode::Playback;
    }
}

void FReplayDirector::StopPlayback() noexcept {
    // Playback / Paused → Idle。Recording / Idle 中の呼び出しは黙って no-op
    // (Stop は冪等であってほしい UX)。
    if (_mode == EReplayMode::Playback || _mode == EReplayMode::Paused) {
        _mode             = EReplayMode::Idle;
        _tick_accumulator = 0.0f;
    }
}

// -----------------------------------------------------------------------------
// 再生速度 / Seek / Progress
// -----------------------------------------------------------------------------

void FReplayDirector::SetPlaybackSpeed(f32 speed) noexcept {
    _playback_speed = ClampSpeed(speed);
    // accumulator はそのまま。速度変更時に sub-tick の dt を捨てると
    // 再生が微妙にジャンプするのを防ぐ。
}

void FReplayDirector::SeekToTick(u32 tick) noexcept {
    // duration_ticks を上限に clamp。LoadReplay 前 (duration = 0) の seek は
    // 0 に張り付くが、これは UI のスクラブバーで「録画されていない」状態を
    // 明示するため意図通り。
    const u32 limit = _metadata.duration_ticks;
    _current_tick     = (tick > limit) ? limit : tick;
    _tick_accumulator = 0.0f;  // sub-tick を持ち越さない
}

f32 FReplayDirector::ProgressNormalized() const noexcept {
    const u32 d = _metadata.duration_ticks;
    if (d == 0) {
        return 0.0f;  // 録画開始直後 / metadata 未設定の安全側
    }
    const f32 p = static_cast<f32>(_current_tick) / static_cast<f32>(d);
    // 上限 1.0 で clamp (Tick で duration を超えた瞬間に 1.0+ になる可能性が
    // あるため)。下限は _current_tick が unsigned なので不要。
    return (p > 1.0f) ? 1.0f : p;
}

// -----------------------------------------------------------------------------
// 毎フレーム呼び出し
// -----------------------------------------------------------------------------

void FReplayDirector::Tick(f32 dt) noexcept {
    // 異常な dt (NaN / 負) はゲームループの早期 frame skip / pause からの復帰時に
    // 紛れ込みやすい。0 でガードして無視する。
    if (!(dt > 0.0f)) {
        return;
    }

    if (_mode == EReplayMode::Recording) {
        // 録画中: tick_rate_hz * dt 分だけ _current_tick を進める。
        // 実入力 capture は FInputRecorder / FLockstep 側で別途行う前提なので、
        // ここは単純な tick カウンタの前進のみ。
        _tick_accumulator += dt * static_cast<f32>(_tick_rate_hz);
        if (_tick_accumulator >= 1.0f) {
            const u32 steps = static_cast<u32>(_tick_accumulator);
            _current_tick    += steps;
            _tick_accumulator -= static_cast<f32>(steps);
        }
        return;
    }

    if (_mode == EReplayMode::Playback) {
        // 再生中: dt * playback_speed * tick_rate_hz 分だけ _current_tick を進める。
        _tick_accumulator += dt * _playback_speed * static_cast<f32>(_tick_rate_hz);
        if (_tick_accumulator >= 1.0f) {
            const u32 steps = static_cast<u32>(_tick_accumulator);
            _current_tick    += steps;
            _tick_accumulator -= static_cast<f32>(steps);
        }
        // duration_ticks に達したら自動的に Idle へ落とす (replay 終了)。
        // 0 のときは metadata 未設定 (load 前) と解釈し、自動停止しない。
        const u32 d = _metadata.duration_ticks;
        if (d != 0 && _current_tick >= d) {
            _current_tick     = d;
            _mode             = EReplayMode::Idle;
            _tick_accumulator = 0.0f;
        }
        return;
    }

    // Paused / Idle: no-op (Tick が呼ばれても何もしない)。
}

// -----------------------------------------------------------------------------
// SaveReplay — Phase R-3: stub (NotImplemented を返す)
// -----------------------------------------------------------------------------
// Phase R-4 の擬似コード:
//   1. file_path が null なら kSub_NullPath
//   2. metadata を sidecar (.meta) ファイルに書き出す (game_version /
//      level_id / seed / timestamp / duration_ticks / player_name /
//      checksum_hex を field 単位で little-endian 化)
//   3. FInputRecorder::SaveToBuffer で .acsr を、FLockstep::SaveToBuffer で
//      .acsl を file_path の隣に書き出す
//   4. 失敗時は temp ファイルを削除して原子性を担保
TResult<void> FReplayDirector::SaveReplay(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) {
        return ACS_ERR(IO, kSub_NullPath,
                       "FReplayDirector::SaveReplay: file_path is null");
    }
    return ACS_ERR(IO, kSub_NotImplemented,
                   "FReplayDirector::SaveReplay is not yet implemented (Phase R-3 stub)");
}

// -----------------------------------------------------------------------------
// LoadReplay — Phase R-3: stub (NotImplemented を返す)
// -----------------------------------------------------------------------------
// Phase R-4 の擬似コード:
//   1. file_path が null なら kSub_NullPath
//   2. .meta sidecar を読み、game_version / level_id / seed / timestamp /
//      duration_ticks / player_name / checksum_hex を _metadata に復元
//   3. .acsr / .acsl を読み、FInputRecorder::LoadFromBuffer /
//      FLockstep::LoadFromBuffer に渡す
//   4. _current_tick = 0, _mode = Idle に戻して StartPlayback 待ち状態とする
TResult<void> FReplayDirector::LoadReplay(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) {
        return ACS_ERR(IO, kSub_NullPath,
                       "FReplayDirector::LoadReplay: file_path is null");
    }
    return ACS_ERR(IO, kSub_NotImplemented,
                   "FReplayDirector::LoadReplay is not yet implemented (Phase R-3 stub)");
}

} // namespace acs::game

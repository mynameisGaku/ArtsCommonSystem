// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — AudioDirector 実装 (Phase 1: bridge スケルトン)
//
// state machine + volume 計算を完全実装。AudioEngine 接続箇所には
// ACS_LOG_INFO("TODO: ...") を一度だけ残し、Phase 2 で繋ぐ。
#include "gameframework/AudioDirector.h"
#include "foundation/Log.h"

namespace acs::game {

// ----------------------------------------------------------------------------
// helpers
// ----------------------------------------------------------------------------

f32 AudioDirector::Clamp01(f32 v) noexcept {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

// 同一 TODO メッセージを毎フレーム吐かないよう、ファイル static の `done` flag
// で 1 度きりに絞る。マルチスレッドからは Tick が呼ばれない前提 (data race なし)。
void AudioDirector::LogTodoOnce(const char* what) noexcept {
    static bool s_logged_play_bgm = false;
    static bool s_logged_stop_bgm = false;
    static bool s_logged_play_sfx = false;
    static bool s_logged_stop_all = false;
    static bool s_logged_pause    = false;
    static bool s_logged_resume   = false;
    static bool s_logged_tick     = false;

    // what は文字列リテラル比較で十分 (ポインタ identity を当て込む)。
    bool* slot = nullptr;
    if      (what == "PlayBgm") slot = &s_logged_play_bgm;
    else if (what == "StopBgm") slot = &s_logged_stop_bgm;
    else if (what == "PlaySfx") slot = &s_logged_play_sfx;
    else if (what == "StopAll") slot = &s_logged_stop_all;
    else if (what == "Pause")   slot = &s_logged_pause;
    else if (what == "Resume")  slot = &s_logged_resume;
    else if (what == "Tick")    slot = &s_logged_tick;

    if (slot != nullptr && *slot) return;
    if (slot != nullptr) *slot = true;
    ACS_LOG_INFO("AudioDirector: %s — Phase 2 で acs::AudioEngine と接続予定 (現在は state のみ)", what);
}

// ----------------------------------------------------------------------------
// construction
// ----------------------------------------------------------------------------

AudioDirector::AudioDirector() noexcept {
    // SFX ring を固定容量で予約。Resize で SfxEntry をデフォルト構築 (active=false)。
    _sfx.Resize(kMaxSfxVoices);
}

// ----------------------------------------------------------------------------
// volume バス
// ----------------------------------------------------------------------------

void AudioDirector::SetMasterVolume(f32 v) noexcept {
    const f32 c = Clamp01(v);
    if (c != v) {
        ACS_LOG_WARN("AudioDirector::SetMasterVolume: out-of-range %.3f → clamped to %.3f", v, c);
    }
    _master_volume = c;
}

void AudioDirector::SetBgmVolume(f32 v) noexcept {
    const f32 c = Clamp01(v);
    if (c != v) {
        ACS_LOG_WARN("AudioDirector::SetBgmVolume: out-of-range %.3f → clamped to %.3f", v, c);
    }
    _bgm_volume = c;
}

void AudioDirector::SetSfxVolume(f32 v) noexcept {
    const f32 c = Clamp01(v);
    if (c != v) {
        ACS_LOG_WARN("AudioDirector::SetSfxVolume: out-of-range %.3f → clamped to %.3f", v, c);
    }
    _sfx_volume = c;
}

// ----------------------------------------------------------------------------
// BGM クロスフェード
// ----------------------------------------------------------------------------

void AudioDirector::PlayBgm(const char* name, f32 fade_in_sec, bool loop) noexcept {
    if (name == nullptr) {
        ACS_LOG_WARN("AudioDirector::PlayBgm: name=nullptr → ignored");
        return;
    }
    // 同一 BGM 再要求は no-op (current の loop / target を尊重)。
    if (_bgm[0].active && _bgm[0].name == name) {
        // ポインタ一致 = literal 同一を意図 (Phase 2 で strcmp に置き換え検討)。
        return;
    }
    LogTodoOnce("PlayBgm");

    // 既存遷移中 (slot[1] active) → 強制的に current に格上げして上書き。
    if (_bgm[1].active) {
        _bgm[0] = _bgm[1];
        _bgm[1] = BgmSlot{};
    }

    if (fade_in_sec <= 0.0f) {
        // 即時切替: current を直ちに置換、gain=1 にスナップ。
        _bgm[0].name     = name;
        _bgm[0].gain     = 1.0f;
        _bgm[0].target   = 1.0f;
        _bgm[0].fade_per_sec = 0.0f;
        _bgm[0].loop     = loop;
        _bgm[0].active   = true;
        _bgm[1]          = BgmSlot{};
        return;
    }

    // クロスフェード開始: current は fade out、new は fade in。
    if (_bgm[0].active) {
        _bgm[0].target       = 0.0f;
        _bgm[0].fade_per_sec = _bgm[0].gain / fade_in_sec;  // 現在 gain から 0 まで
    }
    _bgm[1].name         = name;
    _bgm[1].gain         = 0.0f;
    _bgm[1].target       = 1.0f;
    _bgm[1].fade_per_sec = 1.0f / fade_in_sec;
    _bgm[1].loop         = loop;
    _bgm[1].active       = true;
}

void AudioDirector::StopBgm(f32 fade_out_sec) noexcept {
    if (!_bgm[0].active && !_bgm[1].active) return;
    LogTodoOnce("StopBgm");

    if (fade_out_sec <= 0.0f) {
        _bgm[0] = BgmSlot{};
        _bgm[1] = BgmSlot{};
        return;
    }
    // 現行 + 遷移中ともに fade out。
    for (u32 i = 0; i < 2; ++i) {
        if (!_bgm[i].active) continue;
        _bgm[i].target       = 0.0f;
        _bgm[i].fade_per_sec = _bgm[i].gain / fade_out_sec;
    }
}

// ----------------------------------------------------------------------------
// SFX one-shot
// ----------------------------------------------------------------------------

void AudioDirector::PlaySfx(const char* name, f32 volume_scale) noexcept {
    if (name == nullptr) {
        ACS_LOG_WARN("AudioDirector::PlaySfx: name=nullptr → ignored");
        return;
    }
    if (volume_scale <= 0.0f) {
        // 0 ゲインは「鳴らさない」明示要求とみなし no-op (警告も出さない)。
        return;
    }
    LogTodoOnce("PlaySfx");

    // 空きを探す。なければ ring 先頭 (= 最古) を上書き。
    u32 slot = kMaxSfxVoices;
    for (u32 i = 0; i < kMaxSfxVoices; ++i) {
        if (!_sfx[i].active) { slot = i; break; }
    }
    if (slot == kMaxSfxVoices) {
        slot = _sfx_head;
        _sfx_head = (_sfx_head + 1u) % kMaxSfxVoices;
        ACS_LOG_WARN("AudioDirector::PlaySfx: ring full → overwriting slot %u", slot);
    }
    _sfx[slot].name         = name;
    _sfx[slot].volume_scale = volume_scale;
    _sfx[slot].active       = true;
    // Phase 2: ここで AudioEngine に one-shot を投げる。完了コールバックで
    // _sfx[slot].active = false に戻す。現在は state を維持するだけ。
}

// ----------------------------------------------------------------------------
// ダッキング
// ----------------------------------------------------------------------------

void AudioDirector::Duck(f32 duration_sec, f32 depth) noexcept {
    if (duration_sec <= 0.0f) {
        ACS_LOG_WARN("AudioDirector::Duck: duration_sec=%.3f <= 0 → ignored", duration_sec);
        return;
    }
    const f32 clamped_depth = Clamp01(depth);
    // depth=1.0 = 抑制なし (no-op の表明)。
    if (clamped_depth >= 0.999f) {
        _duck_active = false;
        return;
    }
    // 既存 Duck を上書き (スタックしない設計)。
    _duck_active     = true;
    _duck_depth      = clamped_depth;
    _duck_total      = duration_sec + 2.0f * kDuckFadeWindow;
    _duck_remaining  = _duck_total;
}

f32 AudioDirector::ComputeDuckEnvelope() const noexcept {
    if (!_duck_active) return 1.0f;
    // _duck_remaining は _duck_total から減っていく。
    //   [total .. total - fade_window]                = fade in  (1.0 → depth)
    //   [total - fade_window .. fade_window]          = hold     (depth)
    //   [fade_window .. 0]                            = fade out (depth → 1.0)
    const f32 elapsed = _duck_total - _duck_remaining;
    if (elapsed < kDuckFadeWindow) {
        const f32 t = elapsed / kDuckFadeWindow;  // 0..1
        return 1.0f + (_duck_depth - 1.0f) * t;
    }
    const f32 remain_for_out = _duck_remaining;
    if (remain_for_out < kDuckFadeWindow) {
        const f32 t = remain_for_out / kDuckFadeWindow;  // 1..0 (残り少→0)
        return _duck_depth + (1.0f - _duck_depth) * (1.0f - t);
    }
    return _duck_depth;
}

// ----------------------------------------------------------------------------
// グローバル制御
// ----------------------------------------------------------------------------

void AudioDirector::Pause() noexcept {
    if (_paused) return;
    _paused = true;
    LogTodoOnce("Pause");
}

void AudioDirector::Resume() noexcept {
    if (!_paused) return;
    _paused = false;
    LogTodoOnce("Resume");
}

void AudioDirector::StopAll() noexcept {
    _bgm[0] = BgmSlot{};
    _bgm[1] = BgmSlot{};
    for (u32 i = 0; i < kMaxSfxVoices; ++i) {
        _sfx[i] = SfxEntry{};
    }
    _sfx_head        = 0;
    _duck_active     = false;
    _duck_remaining  = 0.0f;
    _duck_total      = 0.0f;
    LogTodoOnce("StopAll");
}

// ----------------------------------------------------------------------------
// driver
// ----------------------------------------------------------------------------

void AudioDirector::Tick(f32 dt) noexcept {
    if (_paused) return;
    if (dt < 0.0f) dt = 0.0f;
    LogTodoOnce("Tick");

    // 1) BGM クロスフェード進行
    for (u32 i = 0; i < 2; ++i) {
        BgmSlot& s = _bgm[i];
        if (!s.active) continue;
        const f32 delta = s.fade_per_sec * dt;
        if (s.gain < s.target) {
            s.gain += delta;
            if (s.gain >= s.target) s.gain = s.target;
        } else if (s.gain > s.target) {
            s.gain -= delta;
            if (s.gain <= s.target) s.gain = s.target;
        }
        // fade out 完了で slot 開放
        if (s.target == 0.0f && s.gain <= 0.0f) {
            s = BgmSlot{};
        }
    }
    // 2) 遷移完了で swap (slot[1] が満タンに達した → slot[0] に格上げ)
    if (_bgm[1].active && _bgm[1].gain >= _bgm[1].target && _bgm[1].target >= 1.0f) {
        // current slot は既に fade out で消えているか、ここで強制クリア。
        _bgm[0] = _bgm[1];
        _bgm[1] = BgmSlot{};
    }

    // 3) ダッキング timer
    if (_duck_active) {
        _duck_remaining -= dt;
        if (_duck_remaining <= 0.0f) {
            _duck_active    = false;
            _duck_remaining = 0.0f;
        }
    }

    // 4) SFX one-shot の状態更新は Phase 2 で AudioEngine からの完了 cb 経由。
    //    現状は active を維持し続ける (= ring overwrite で押し出される)。
}

// ----------------------------------------------------------------------------
// 派生 volume
// ----------------------------------------------------------------------------

f32 AudioDirector::EffectiveBgmVolume() const noexcept {
    if (_paused) return 0.0f;
    // 現行 + 遷移中の bgm ゲイン合計 (クロスフェード中は 0..1+0..1 だが
    // 概ね 1.0 を保つように fade_per_sec が組まれている)。
    const f32 bgm_mix = _bgm[0].gain + _bgm[1].gain;
    return _master_volume * _bgm_volume * ComputeDuckEnvelope() * bgm_mix;
}

f32 AudioDirector::EffectiveSfxVolume() const noexcept {
    if (_paused) return 0.0f;
    return _master_volume * _sfx_volume;
}

} // namespace acs::game

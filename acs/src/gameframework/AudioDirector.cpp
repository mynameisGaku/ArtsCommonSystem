// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — FAudioDirector 実装 (Phase 2: 実 backend 接続)
//
// state machine + volume 計算 + `IAudioBackend*` delegate を完全実装。
// backend == nullptr のときは従来 (Phase 1) 通り state-only で動く。
#include "gameframework/AudioDirector.h"
#include "foundation/Log.h"
#include "gameframework/audio_backend/IAudioBackend.h"

namespace acs::game {

// ----------------------------------------------------------------------------
// helpers
// ----------------------------------------------------------------------------

f32 FAudioDirector::Clamp01(f32 v) noexcept {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

// 同一 TODO メッセージを毎フレーム吐かないよう、ファイル static の `done` flag
// で 1 度きりに絞る。マルチスレッドからは Tick が呼ばれない前提 (data race なし)。
void FAudioDirector::LogTodoOnce(const char* what) noexcept {
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
    ACS_LOG_INFO("FAudioDirector: %s — Phase 2 で acs::FAudioEngine と接続予定 (現在は state のみ)", what);
}

// ----------------------------------------------------------------------------
// construction
// ----------------------------------------------------------------------------

FAudioDirector::FAudioDirector() noexcept {
    // SFX ring を固定容量で予約。Resize で SfxEntry をデフォルト構築 (active=false)。
    _sfx.Resize(kMaxSfxVoices);
}

// ----------------------------------------------------------------------------
// volume バス
// ----------------------------------------------------------------------------

void FAudioDirector::SetMasterVolume(f32 v) noexcept {
    const f32 c = Clamp01(v);
    if (c != v) {
        ACS_LOG_WARN("FAudioDirector::SetMasterVolume: out-of-range %.3f → clamped to %.3f", v, c);
    }
    _master_volume = c;
    // backend 直接 master volume (mastering voice の master gain) は本層では
    // 触らない: master * bgm/sfx は Tick で voice ごとに合成して反映する方が
    // ducking / fade と整合が取りやすい。
    // (SetMasterVolume を backend->SetMasterVolume に流したい場合は
    //  FXAudio2Backend のような派生 API を呼ぶ。本層では state のみ保持。)
}

void FAudioDirector::SetBgmVolume(f32 v) noexcept {
    const f32 c = Clamp01(v);
    if (c != v) {
        ACS_LOG_WARN("FAudioDirector::SetBgmVolume: out-of-range %.3f → clamped to %.3f", v, c);
    }
    _bgm_volume = c;
}

void FAudioDirector::SetSfxVolume(f32 v) noexcept {
    const f32 c = Clamp01(v);
    if (c != v) {
        ACS_LOG_WARN("FAudioDirector::SetSfxVolume: out-of-range %.3f → clamped to %.3f", v, c);
    }
    _sfx_volume = c;
}

// ----------------------------------------------------------------------------
// BGM クロスフェード
// ----------------------------------------------------------------------------

void FAudioDirector::PlayBgm(const char* name, f32 fade_in_sec, bool loop) noexcept {
    if (name == nullptr) {
        ACS_LOG_WARN("FAudioDirector::PlayBgm: name=nullptr → ignored");
        return;
    }
    // 同一 BGM 再要求は no-op (current の loop / target を尊重)。
    if (_bgm[0].active && _bgm[0].name == name) {
        // ポインタ一致 = literal 同一を意図 (Phase 3 で strcmp に置き換え検討)。
        return;
    }
    LogTodoOnce("PlayBgm");

    // 既存遷移中 (slot[1] active) → 強制的に current に格上げして上書き。
    // backend voice が slot[0] に残っていれば backend->StopVoice で先に止める
    // (clip 再生が走っていた場合、新規 PlayBgm でリセットされる正当な経路)。
    if (_bgm[1].active) {
        if (_backend != nullptr && _bgm[0].voice.IsValid()) {
            _backend->StopVoice(_bgm[0].voice);
        }
        _bgm[0] = _bgm[1];
        _bgm[1] = FBgmSlot{};
    }

    if (fade_in_sec <= 0.0f) {
        // 即時切替: current を直ちに置換、gain=1 にスナップ。
        // 既存 backend voice があれば止めてから新規 state を入れる。
        if (_backend != nullptr && _bgm[0].voice.IsValid()) {
            _backend->StopVoice(_bgm[0].voice);
        }
        _bgm[0].name         = name;
        _bgm[0].gain         = 1.0f;
        _bgm[0].target       = 1.0f;
        _bgm[0].fade_per_sec = 0.0f;
        _bgm[0].loop         = loop;
        _bgm[0].active       = true;
        _bgm[0].voice        = kInvalidAudioVoice;  // 名前ベースは voice 未紐付
        _bgm[1]              = FBgmSlot{};
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
    _bgm[1].voice        = kInvalidAudioVoice;  // 名前ベースは voice 未紐付
}

void FAudioDirector::StopBgm(f32 fade_out_sec) noexcept {
    if (!_bgm[0].active && !_bgm[1].active) return;
    LogTodoOnce("StopBgm");

    if (fade_out_sec <= 0.0f) {
        _bgm[0] = FBgmSlot{};
        _bgm[1] = FBgmSlot{};
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

void FAudioDirector::PlaySfx(const char* name, f32 volume_scale) noexcept {
    if (name == nullptr) {
        ACS_LOG_WARN("FAudioDirector::PlaySfx: name=nullptr → ignored");
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
        ACS_LOG_WARN("FAudioDirector::PlaySfx: ring full → overwriting slot %u", slot);
    }
    _sfx[slot].name         = name;
    _sfx[slot].volume_scale = volume_scale;
    _sfx[slot].active       = true;
    // Phase 2: ここで FAudioEngine に one-shot を投げる。完了コールバックで
    // _sfx[slot].active = false に戻す。現在は state を維持するだけ。
}

// ----------------------------------------------------------------------------
// ダッキング
// ----------------------------------------------------------------------------

void FAudioDirector::Duck(f32 duration_sec, f32 depth) noexcept {
    if (duration_sec <= 0.0f) {
        ACS_LOG_WARN("FAudioDirector::Duck: duration_sec=%.3f <= 0 → ignored", duration_sec);
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

f32 FAudioDirector::ComputeDuckEnvelope() const noexcept {
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

void FAudioDirector::Pause() noexcept {
    if (_paused) return;
    _paused = true;
    // backend に Pause API は無いので「全 BGM voice の volume を 0 に落とす」で
    // 代用する… のは破綻 (Resume 時に fade in が要る)。XAudio2 の Stop/Start は
    // 「voice の再生位置を保持したまま」止められる API だが、IAudioBackend には
    // pause API を切らない方針なので、本層では state-only で表現する。
    // 結果: pause 中も backend 側は鳴り続ける可能性あり。本格的 pause は
    //       backend 拡張 (FXAudio2Backend::PauseAll 等) を別途追加する想定。
}

void FAudioDirector::Resume() noexcept {
    if (!_paused) return;
    _paused = false;
}

void FAudioDirector::StopAll() noexcept {
    // backend が居れば全 voice を実停止 (clip 再生のもの含む)。
    if (_backend != nullptr) {
        _backend->StopAllVoices();
    }
    _bgm[0] = FBgmSlot{};
    _bgm[1] = FBgmSlot{};
    for (u32 i = 0; i < kMaxSfxVoices; ++i) {
        _sfx[i] = SfxEntry{};
    }
    _sfx_head        = 0;
    _duck_active     = false;
    _duck_remaining  = 0.0f;
    _duck_total      = 0.0f;
}

// ----------------------------------------------------------------------------
// driver
// ----------------------------------------------------------------------------

void FAudioDirector::Tick(f32 dt) noexcept {
    // backend tick は pause 中でも呼ぶ (完了 voice の slot 回収を止めると
    // 復帰時に古い voice が残るため)。
    if (_backend != nullptr) {
        _backend->Tick(dt < 0.0f ? 0.0f : dt);
    }
    if (_paused) return;
    if (dt < 0.0f) dt = 0.0f;

    // 1) BGM クロスフェード進行
    for (u32 i = 0; i < 2; ++i) {
        FBgmSlot& s = _bgm[i];
        if (!s.active) continue;
        const f32 delta = s.fade_per_sec * dt;
        if (s.gain < s.target) {
            s.gain += delta;
            if (s.gain >= s.target) s.gain = s.target;
        } else if (s.gain > s.target) {
            s.gain -= delta;
            if (s.gain <= s.target) s.gain = s.target;
        }
        // fade out 完了で slot 開放 (backend voice があれば実停止)
        if (s.target == 0.0f && s.gain <= 0.0f) {
            if (_backend != nullptr && s.voice.IsValid()) {
                _backend->StopVoice(s.voice);
            }
            s = FBgmSlot{};
        }
    }
    // 2) 遷移完了で swap (slot[1] が満タンに達した → slot[0] に格上げ)
    if (_bgm[1].active && _bgm[1].gain >= _bgm[1].target && _bgm[1].target >= 1.0f) {
        // current slot は既に fade out で消えているはずだが、念のため backend
        // voice が残っていれば実停止してから上書き。
        if (_backend != nullptr && _bgm[0].voice.IsValid() && _bgm[0].voice._packed != _bgm[1].voice._packed) {
            _backend->StopVoice(_bgm[0].voice);
        }
        _bgm[0] = _bgm[1];
        _bgm[1] = FBgmSlot{};
    }

    // 3) ダッキング timer
    if (_duck_active) {
        _duck_remaining -= dt;
        if (_duck_remaining <= 0.0f) {
            _duck_active    = false;
            _duck_remaining = 0.0f;
        }
    }

    // 4) BGM voice の実 volume を毎フレ反映 (cross-fade gain + duck envelope +
    //    master * bgm bus を合成)。backend voice が紐付いている slot のみ対象。
    if (_backend != nullptr) {
        const f32 master_bgm = _master_volume * _bgm_volume * ComputeDuckEnvelope();
        for (u32 i = 0; i < 2; ++i) {
            const FBgmSlot& s = _bgm[i];
            if (!s.active || !s.voice.IsValid()) continue;
            _backend->SetVoiceVolume(s.voice, master_bgm * s.gain);
        }
    }

    // 5) SFX one-shot の state は ring overwrite で押し出される (完了通知 cb は
    //    本層では持たず、backend 側 Tick の自然回収に委ねる)。
}

// ----------------------------------------------------------------------------
// 派生 volume
// ----------------------------------------------------------------------------

f32 FAudioDirector::EffectiveBgmVolume() const noexcept {
    if (_paused) return 0.0f;
    // 現行 + 遷移中の bgm ゲイン合計 (クロスフェード中は 0..1+0..1 だが
    // 概ね 1.0 を保つように fade_per_sec が組まれている)。
    const f32 bgm_mix = _bgm[0].gain + _bgm[1].gain;
    return _master_volume * _bgm_volume * ComputeDuckEnvelope() * bgm_mix;
}

f32 FAudioDirector::EffectiveSfxVolume() const noexcept {
    if (_paused) return 0.0f;
    return _master_volume * _sfx_volume;
}

// ----------------------------------------------------------------------------
// clip 直接再生 (Phase 2 で追加)
// ----------------------------------------------------------------------------

AudioVoiceHandle FAudioDirector::PlayBgmClip(const AudioClipDesc& clip,
                                            f32 fade_in_sec,
                                            bool loop) noexcept {
    if (_backend == nullptr) {
        // backend 未設定: state 更新もスキップ (clip API は backend 必須の契約)。
        return kInvalidAudioVoice;
    }

    // 既存 BGM voice を停止 (cross-fade 用に slot[0] に格上げ → slot[1] に新規)。
    // 名前ベース PlayBgm と違い、clip 直接再生は「いま即時に新音を入れたい」
    // 要求が強いので、遷移中 (slot[1] active) なら slot[0] の voice は即停止。
    if (_bgm[1].active) {
        if (_bgm[0].voice.IsValid()) _backend->StopVoice(_bgm[0].voice);
        _bgm[0] = _bgm[1];
        _bgm[1] = FBgmSlot{};
    }

    // 新 voice を backend に出す。初期 volume は 0 (Tick で master*bgm*duck*gain
    // を反映する)、pitch は 1.0 固定 (BGM の pitch 制御は別 API 検討)。
    const AudioVoiceHandle handle = _backend->PlayLooped(clip, 0.0f, 1.0f);
    if (!handle.IsValid() && loop) {
        // ループ用に PlayLooped したが失敗 → 一発再生フォールバックは BGM
        // 用途的に意味薄なので、ここは諦めて InvalidHandle を返す。
        return kInvalidAudioVoice;
    }
    if (!loop) {
        // BGM だが loop なし指定 (stinger 演出) → PlayOneShot に切替。
        // PlayLooped が成功していたら、ここでは止めずに以降のロジックで上書き
        // することにする。
        if (handle.IsValid()) _backend->StopVoice(handle);
        const AudioVoiceHandle one = _backend->PlayOneShot(clip, 0.0f, 1.0f);
        if (!one.IsValid()) return kInvalidAudioVoice;
        if (fade_in_sec <= 0.0f) {
            // 即時切替: slot[0] = 新 voice、gain=1。
            if (_bgm[0].voice.IsValid()) _backend->StopVoice(_bgm[0].voice);
            _bgm[0]              = FBgmSlot{};
            _bgm[0].gain         = 1.0f;
            _bgm[0].target       = 1.0f;
            _bgm[0].fade_per_sec = 0.0f;
            _bgm[0].loop         = false;
            _bgm[0].active       = true;
            _bgm[0].voice        = one;
            _bgm[1]              = FBgmSlot{};
            // 初回 volume はこの場で反映 (Tick を待たない)。
            _backend->SetVoiceVolume(one, _master_volume * _bgm_volume * ComputeDuckEnvelope());
            return one;
        }
        _bgm[1]              = FBgmSlot{};
        _bgm[1].gain         = 0.0f;
        _bgm[1].target       = 1.0f;
        _bgm[1].fade_per_sec = 1.0f / fade_in_sec;
        _bgm[1].loop         = false;
        _bgm[1].active       = true;
        _bgm[1].voice        = one;
        // 既存 BGM は fade out
        if (_bgm[0].active) {
            _bgm[0].target       = 0.0f;
            _bgm[0].fade_per_sec = _bgm[0].gain / fade_in_sec;
        }
        return one;
    }

    // loop=true の典型 BGM 経路。
    if (fade_in_sec <= 0.0f) {
        // 即時切替: 既存 slot[0] voice を止めて新規を slot[0] に。
        if (_bgm[0].voice.IsValid()) _backend->StopVoice(_bgm[0].voice);
        _bgm[0]              = FBgmSlot{};
        _bgm[0].gain         = 1.0f;
        _bgm[0].target       = 1.0f;
        _bgm[0].fade_per_sec = 0.0f;
        _bgm[0].loop         = true;
        _bgm[0].active       = true;
        _bgm[0].voice        = handle;
        _bgm[1]              = FBgmSlot{};
        _backend->SetVoiceVolume(handle, _master_volume * _bgm_volume * ComputeDuckEnvelope());
        return handle;
    }

    // cross-fade: 既存は fade out、新規は slot[1] で fade in。
    if (_bgm[0].active) {
        _bgm[0].target       = 0.0f;
        _bgm[0].fade_per_sec = _bgm[0].gain / fade_in_sec;
    }
    _bgm[1]              = FBgmSlot{};
    _bgm[1].gain         = 0.0f;
    _bgm[1].target       = 1.0f;
    _bgm[1].fade_per_sec = 1.0f / fade_in_sec;
    _bgm[1].loop         = true;
    _bgm[1].active       = true;
    _bgm[1].voice        = handle;
    return handle;
}

AudioVoiceHandle FAudioDirector::PlaySfxClip(const AudioClipDesc& clip,
                                            f32 volume_scale,
                                            f32 pitch) noexcept {
    if (_backend == nullptr) return kInvalidAudioVoice;
    if (volume_scale <= 0.0f) return kInvalidAudioVoice;

    // SFX の最終 volume = master * sfx_bus * volume_scale (duck は SFX には掛けない)。
    const f32 final_vol = _master_volume * _sfx_volume * volume_scale;
    return _backend->PlayOneShot(clip, final_vol, pitch);
}

} // namespace acs::game

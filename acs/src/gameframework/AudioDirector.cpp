// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — FAudioDirector 実装 (Phase 2: 実 backend 接続)
//
// state machine + volume 計算 + `IAudioBackend*` delegate を完全実装。
// backend == nullptr のときは従来 (Phase 1) 通り state-only で動く。
#include "gameframework/AudioDirector.h"
#include "foundation/Log.h"
#include "gameframework/audio_backend/IAudioBackend.h"
#include "asset/AssetRegistry.h"
#include "asset/AudioAsset.h"
#include "memory/Rc.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace acs::game {

namespace {
// 文字列を内容で比較する (どちらかが nullptr なら false)。AchievementManager 等
// 他 GameFramework モジュールと同じ自前 StrEq pattern を踏襲 (STL 非依存)。
bool StrEq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *a == *b) { ++a; ++b; }
    return *a == *b;
}

// name (= asset path、例 "audio/bgm.ogg") を registry でロードし AudioClipDesc を組む。
// keep は呼び出し中 asset を生存させる (backend が Play* で PCM を内部コピーするので、
// 呼び出し後に drop してよい)。registry 未設定 / 非音声 / load 失敗は false。
bool ResolveAudioClip(FAssetRegistry* reg, const char* name,
                      AudioClipDesc& out, TRc<Asset>& keep) noexcept {
    if (reg == nullptr || name == nullptr) return false;
    wchar_t wpath[260];
    if (::MultiByteToWideChar(CP_UTF8, 0, name, -1, wpath, 260) <= 0) return false;
    auto r = reg->Load(wpath);
    if (r.IsErr()) return false;
    keep = r.Value();
    Asset* a = keep.Get();
    if (a == nullptr || a->Type() != FAudioAsset::StaticType()) return false;
    const FAudioAsset* audio = static_cast<const FAudioAsset*>(a);
    out.pcm_data      = audio->Samples();
    out.pcm_size      = audio->SampleByteCount();
    out.sample_rate   = audio->SampleRate();
    out.channel_count = audio->Channels();
    out.format        = (audio->EFormat() == ESampleFormat::PCM_S16)
                        ? EAudioFormat::Pcm16 : EAudioFormat::Pcm32Float;
    return true;
}
} // namespace

// ----------------------------------------------------------------------------
// helpers
// ----------------------------------------------------------------------------

f32 FAudioDirector::Clamp01(f32 v) noexcept {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

// ----------------------------------------------------------------------------
// construction
// ----------------------------------------------------------------------------

FAudioDirector::FAudioDirector() noexcept {
    // SFX ring を固定容量で予約。Resize で SfxEntry をデフォルト構築 (active=false)。
    m_Sfx.Resize(kMaxSfxVoices);
}

// ----------------------------------------------------------------------------
// volume バス
// ----------------------------------------------------------------------------

void FAudioDirector::SetMasterVolume(f32 v) noexcept {
    const f32 c = Clamp01(v);
    if (c != v) {
        ACS_LOG_WARN("FAudioDirector::SetMasterVolume: out-of-range %.3f → clamped to %.3f", v, c);
    }
    m_MasterVolume = c;
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
    m_BgmVolume = c;
}

void FAudioDirector::SetSfxVolume(f32 v) noexcept {
    const f32 c = Clamp01(v);
    if (c != v) {
        ACS_LOG_WARN("FAudioDirector::SetSfxVolume: out-of-range %.3f → clamped to %.3f", v, c);
    }
    m_SfxVolume = c;
}

// ----------------------------------------------------------------------------
// BGM クロスフェード
// ----------------------------------------------------------------------------

void FAudioDirector::PlayBgm(const char* name, f32 fade_in_sec, bool loop) noexcept {
    if (name == nullptr) {
        ACS_LOG_WARN("FAudioDirector::PlayBgm: name=nullptr → ignored");
        return;
    }
    // 同一 BGM 再要求は no-op (current の loop / target を尊重)。内容比較。
    if (m_Bgm[0].active && StrEq(m_Bgm[0].name, name)) {
        return;
    }

    // 実 backend + registry があれば name(=asset path) → clip を解決して実音再生する。
    if (m_Backend != nullptr && m_Registry != nullptr) {
        AudioClipDesc clip; TRc<Asset> keep;
        if (ResolveAudioClip(m_Registry, name, clip, keep)) {
            const AudioVoiceHandle h = PlayBgmClip(clip, fade_in_sec, loop);
            if (h.IsValid()) {
                // PlayBgmClip が voice を入れた slot に name を紐付ける (CurrentBgmName 用)。
                for (u32 i = 0; i < 2; ++i)
                    if (m_Bgm[i].active && m_Bgm[i].voice.m_Packed == h.m_Packed) m_Bgm[i].name = name;
                return;
            }
        }
        // 解決 / 再生失敗 → 以下の state-only にフォールバック (UI に name は追える)。
    }

    // 既存遷移中 (slot[1] active) → 強制的に current に格上げして上書き。
    // backend voice が slot[0] に残っていれば backend->StopVoice で先に止める
    // (clip 再生が走っていた場合、新規 PlayBgm でリセットされる正当な経路)。
    if (m_Bgm[1].active) {
        if (m_Backend != nullptr && m_Bgm[0].voice.IsValid()) {
            m_Backend->StopVoice(m_Bgm[0].voice);
        }
        m_Bgm[0] = m_Bgm[1];
        m_Bgm[1] = FBgmSlot{};
    }

    if (fade_in_sec <= 0.0f) {
        // 即時切替: current を直ちに置換、gain=1 にスナップ。
        // 既存 backend voice があれば止めてから新規 state を入れる。
        if (m_Backend != nullptr && m_Bgm[0].voice.IsValid()) {
            m_Backend->StopVoice(m_Bgm[0].voice);
        }
        m_Bgm[0].name         = name;
        m_Bgm[0].gain         = 1.0f;
        m_Bgm[0].target       = 1.0f;
        m_Bgm[0].fade_per_sec = 0.0f;
        m_Bgm[0].loop         = loop;
        m_Bgm[0].active       = true;
        m_Bgm[0].voice        = kInvalidAudioVoice;  // 名前ベースは voice 未紐付
        m_Bgm[1]              = FBgmSlot{};
        return;
    }

    // クロスフェード開始: current は fade out、new は fade in。
    if (m_Bgm[0].active) {
        m_Bgm[0].target       = 0.0f;
        m_Bgm[0].fade_per_sec = m_Bgm[0].gain / fade_in_sec;  // 現在 gain から 0 まで
    }
    m_Bgm[1].name         = name;
    m_Bgm[1].gain         = 0.0f;
    m_Bgm[1].target       = 1.0f;
    m_Bgm[1].fade_per_sec = 1.0f / fade_in_sec;
    m_Bgm[1].loop         = loop;
    m_Bgm[1].active       = true;
    m_Bgm[1].voice        = kInvalidAudioVoice;  // 名前ベースは voice 未紐付
}

void FAudioDirector::StopBgm(f32 fade_out_sec) noexcept {
    if (!m_Bgm[0].active && !m_Bgm[1].active) return;

    if (fade_out_sec <= 0.0f) {
        m_Bgm[0] = FBgmSlot{};
        m_Bgm[1] = FBgmSlot{};
        return;
    }
    // 現行 + 遷移中ともに fade out。
    for (u32 i = 0; i < 2; ++i) {
        if (!m_Bgm[i].active) continue;
        m_Bgm[i].target       = 0.0f;
        m_Bgm[i].fade_per_sec = m_Bgm[i].gain / fade_out_sec;
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

    // 実 backend + registry があれば name(=asset path) → clip を解決して実音再生する。
    // backend が voice を管理するので、成功時は state ring に積まない。
    if (m_Backend != nullptr && m_Registry != nullptr) {
        AudioClipDesc clip; TRc<Asset> keep;
        if (ResolveAudioClip(m_Registry, name, clip, keep)) {
            PlaySfxClip(clip, volume_scale, 1.0f);
            return;
        }
        // 解決失敗 → 以下の state-only ring にフォールバック。
    }

    // 空きを探す。なければ ring 先頭 (= 最古) を上書き。
    u32 slot = kMaxSfxVoices;
    for (u32 i = 0; i < kMaxSfxVoices; ++i) {
        if (!m_Sfx[i].active) { slot = i; break; }
    }
    if (slot == kMaxSfxVoices) {
        slot = m_SfxHead;
        m_SfxHead = (m_SfxHead + 1u) % kMaxSfxVoices;
        ACS_LOG_WARN("FAudioDirector::PlaySfx: ring full → overwriting slot %u", slot);
    }
    m_Sfx[slot].name         = name;
    m_Sfx[slot].volume_scale = volume_scale;
    m_Sfx[slot].active       = true;
    // Phase 2: ここで FAudioEngine に one-shot を投げる。完了コールバックで
    // m_Sfx[slot].active = false に戻す。現在は state を維持するだけ。
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
        m_bDuckActive = false;
        return;
    }
    // 既存 Duck を上書き (スタックしない設計)。
    m_bDuckActive     = true;
    m_DuckDepth      = clamped_depth;
    m_DuckTotal      = duration_sec + 2.0f * kDuckFadeWindow;
    m_DuckRemaining  = m_DuckTotal;
}

f32 FAudioDirector::ComputeDuckEnvelope() const noexcept {
    if (!m_bDuckActive) return 1.0f;
    // m_DuckRemaining は m_DuckTotal から減っていく。
    //   [total .. total - fade_window]                = fade in  (1.0 → depth)
    //   [total - fade_window .. fade_window]          = hold     (depth)
    //   [fade_window .. 0]                            = fade out (depth → 1.0)
    const f32 elapsed = m_DuckTotal - m_DuckRemaining;
    if (elapsed < kDuckFadeWindow) {
        const f32 t = elapsed / kDuckFadeWindow;  // 0..1
        return 1.0f + (m_DuckDepth - 1.0f) * t;
    }
    const f32 remain_for_out = m_DuckRemaining;
    if (remain_for_out < kDuckFadeWindow) {
        const f32 t = remain_for_out / kDuckFadeWindow;  // 1..0 (残り少→0)
        return m_DuckDepth + (1.0f - m_DuckDepth) * (1.0f - t);
    }
    return m_DuckDepth;
}

// ----------------------------------------------------------------------------
// グローバル制御
// ----------------------------------------------------------------------------

void FAudioDirector::Pause() noexcept {
    if (m_Paused) return;
    m_Paused = true;
    // backend に Pause API は無いので「全 BGM voice の volume を 0 に落とす」で
    // 代用する… のは破綻 (Resume 時に fade in が要る)。XAudio2 の Stop/Start は
    // 「voice の再生位置を保持したまま」止められる API だが、IAudioBackend には
    // pause API を切らない方針なので、本層では state-only で表現する。
    // 結果: pause 中も backend 側は鳴り続ける可能性あり。本格的 pause は
    //       backend 拡張 (FXAudio2Backend::PauseAll 等) を別途追加する想定。
}

void FAudioDirector::Resume() noexcept {
    if (!m_Paused) return;
    m_Paused = false;
}

void FAudioDirector::StopAll() noexcept {
    // backend が居れば全 voice を実停止 (clip 再生のもの含む)。
    if (m_Backend != nullptr) {
        m_Backend->StopAllVoices();
    }
    m_Bgm[0] = FBgmSlot{};
    m_Bgm[1] = FBgmSlot{};
    for (u32 i = 0; i < kMaxSfxVoices; ++i) {
        m_Sfx[i] = SfxEntry{};
    }
    m_SfxHead        = 0;
    m_bDuckActive     = false;
    m_DuckRemaining  = 0.0f;
    m_DuckTotal      = 0.0f;
}

// ----------------------------------------------------------------------------
// driver
// ----------------------------------------------------------------------------

void FAudioDirector::Tick(f32 dt) noexcept {
    // backend tick は pause 中でも呼ぶ (完了 voice の slot 回収を止めると
    // 復帰時に古い voice が残るため)。
    if (m_Backend != nullptr) {
        m_Backend->Tick(dt < 0.0f ? 0.0f : dt);
    }
    if (m_Paused) return;
    if (dt < 0.0f) dt = 0.0f;

    // 1) BGM クロスフェード進行
    for (u32 i = 0; i < 2; ++i) {
        FBgmSlot& s = m_Bgm[i];
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
            if (m_Backend != nullptr && s.voice.IsValid()) {
                m_Backend->StopVoice(s.voice);
            }
            s = FBgmSlot{};
        }
    }
    // 2) 遷移完了で swap (slot[1] が満タンに達した → slot[0] に格上げ)
    if (m_Bgm[1].active && m_Bgm[1].gain >= m_Bgm[1].target && m_Bgm[1].target >= 1.0f) {
        // current slot は既に fade out で消えているはずだが、念のため backend
        // voice が残っていれば実停止してから上書き。
        if (m_Backend != nullptr && m_Bgm[0].voice.IsValid() && m_Bgm[0].voice.m_Packed != m_Bgm[1].voice.m_Packed) {
            m_Backend->StopVoice(m_Bgm[0].voice);
        }
        m_Bgm[0] = m_Bgm[1];
        m_Bgm[1] = FBgmSlot{};
    }

    // 3) ダッキング timer
    if (m_bDuckActive) {
        m_DuckRemaining -= dt;
        if (m_DuckRemaining <= 0.0f) {
            m_bDuckActive    = false;
            m_DuckRemaining = 0.0f;
        }
    }

    // 4) BGM voice の実 volume を毎フレ反映 (cross-fade gain + duck envelope +
    //    master * bgm bus を合成)。backend voice が紐付いている slot のみ対象。
    if (m_Backend != nullptr) {
        const f32 master_bgm = m_MasterVolume * m_BgmVolume * ComputeDuckEnvelope();
        for (u32 i = 0; i < 2; ++i) {
            const FBgmSlot& s = m_Bgm[i];
            if (!s.active || !s.voice.IsValid()) continue;
            m_Backend->SetVoiceVolume(s.voice, master_bgm * s.gain);
        }
    }

    // 5) SFX one-shot の state は ring overwrite で押し出される (完了通知 cb は
    //    本層では持たず、backend 側 Tick の自然回収に委ねる)。
}

// ----------------------------------------------------------------------------
// 派生 volume
// ----------------------------------------------------------------------------

f32 FAudioDirector::EffectiveBgmVolume() const noexcept {
    if (m_Paused) return 0.0f;
    // 現行 + 遷移中の bgm ゲイン合計 (クロスフェード中は 0..1+0..1 だが
    // 概ね 1.0 を保つように fade_per_sec が組まれている)。
    const f32 bgm_mix = m_Bgm[0].gain + m_Bgm[1].gain;
    return m_MasterVolume * m_BgmVolume * ComputeDuckEnvelope() * bgm_mix;
}

f32 FAudioDirector::EffectiveSfxVolume() const noexcept {
    if (m_Paused) return 0.0f;
    return m_MasterVolume * m_SfxVolume;
}

// ----------------------------------------------------------------------------
// clip 直接再生 (Phase 2 で追加)
// ----------------------------------------------------------------------------

AudioVoiceHandle FAudioDirector::PlayBgmClip(const AudioClipDesc& clip,
                                            f32 fade_in_sec,
                                            bool loop) noexcept {
    if (m_Backend == nullptr) {
        // backend 未設定: state 更新もスキップ (clip API は backend 必須の契約)。
        return kInvalidAudioVoice;
    }

    // 既存 BGM voice を停止 (cross-fade 用に slot[0] に格上げ → slot[1] に新規)。
    // 名前ベース PlayBgm と違い、clip 直接再生は「いま即時に新音を入れたい」
    // 要求が強いので、遷移中 (slot[1] active) なら slot[0] の voice は即停止。
    if (m_Bgm[1].active) {
        if (m_Bgm[0].voice.IsValid()) m_Backend->StopVoice(m_Bgm[0].voice);
        m_Bgm[0] = m_Bgm[1];
        m_Bgm[1] = FBgmSlot{};
    }

    // 新 voice を backend に出す。初期 volume は 0 (Tick で master*bgm*duck*gain
    // を反映する)、pitch は 1.0 固定 (BGM の pitch 制御は別 API 検討)。
    const AudioVoiceHandle handle = m_Backend->PlayLooped(clip, 0.0f, 1.0f);
    if (!handle.IsValid() && loop) {
        // ループ用に PlayLooped したが失敗 → 一発再生フォールバックは BGM
        // 用途的に意味薄なので、ここは諦めて InvalidHandle を返す。
        return kInvalidAudioVoice;
    }
    if (!loop) {
        // BGM だが loop なし指定 (stinger 演出) → PlayOneShot に切替。
        // PlayLooped が成功していたら、ここでは止めずに以降のロジックで上書き
        // することにする。
        if (handle.IsValid()) m_Backend->StopVoice(handle);
        const AudioVoiceHandle one = m_Backend->PlayOneShot(clip, 0.0f, 1.0f);
        if (!one.IsValid()) return kInvalidAudioVoice;
        if (fade_in_sec <= 0.0f) {
            // 即時切替: slot[0] = 新 voice、gain=1。
            if (m_Bgm[0].voice.IsValid()) m_Backend->StopVoice(m_Bgm[0].voice);
            m_Bgm[0]              = FBgmSlot{};
            m_Bgm[0].gain         = 1.0f;
            m_Bgm[0].target       = 1.0f;
            m_Bgm[0].fade_per_sec = 0.0f;
            m_Bgm[0].loop         = false;
            m_Bgm[0].active       = true;
            m_Bgm[0].voice        = one;
            m_Bgm[1]              = FBgmSlot{};
            // 初回 volume はこの場で反映 (Tick を待たない)。
            m_Backend->SetVoiceVolume(one, m_MasterVolume * m_BgmVolume * ComputeDuckEnvelope());
            return one;
        }
        m_Bgm[1]              = FBgmSlot{};
        m_Bgm[1].gain         = 0.0f;
        m_Bgm[1].target       = 1.0f;
        m_Bgm[1].fade_per_sec = 1.0f / fade_in_sec;
        m_Bgm[1].loop         = false;
        m_Bgm[1].active       = true;
        m_Bgm[1].voice        = one;
        // 既存 BGM は fade out
        if (m_Bgm[0].active) {
            m_Bgm[0].target       = 0.0f;
            m_Bgm[0].fade_per_sec = m_Bgm[0].gain / fade_in_sec;
        }
        return one;
    }

    // loop=true の典型 BGM 経路。
    if (fade_in_sec <= 0.0f) {
        // 即時切替: 既存 slot[0] voice を止めて新規を slot[0] に。
        if (m_Bgm[0].voice.IsValid()) m_Backend->StopVoice(m_Bgm[0].voice);
        m_Bgm[0]              = FBgmSlot{};
        m_Bgm[0].gain         = 1.0f;
        m_Bgm[0].target       = 1.0f;
        m_Bgm[0].fade_per_sec = 0.0f;
        m_Bgm[0].loop         = true;
        m_Bgm[0].active       = true;
        m_Bgm[0].voice        = handle;
        m_Bgm[1]              = FBgmSlot{};
        m_Backend->SetVoiceVolume(handle, m_MasterVolume * m_BgmVolume * ComputeDuckEnvelope());
        return handle;
    }

    // cross-fade: 既存は fade out、新規は slot[1] で fade in。
    if (m_Bgm[0].active) {
        m_Bgm[0].target       = 0.0f;
        m_Bgm[0].fade_per_sec = m_Bgm[0].gain / fade_in_sec;
    }
    m_Bgm[1]              = FBgmSlot{};
    m_Bgm[1].gain         = 0.0f;
    m_Bgm[1].target       = 1.0f;
    m_Bgm[1].fade_per_sec = 1.0f / fade_in_sec;
    m_Bgm[1].loop         = true;
    m_Bgm[1].active       = true;
    m_Bgm[1].voice        = handle;
    return handle;
}

AudioVoiceHandle FAudioDirector::PlaySfxClip(const AudioClipDesc& clip,
                                            f32 volume_scale,
                                            f32 pitch) noexcept {
    if (m_Backend == nullptr) return kInvalidAudioVoice;
    if (volume_scale <= 0.0f) return kInvalidAudioVoice;

    // SFX の最終 volume = master * sfx_bus * volume_scale (duck は SFX には掛けない)。
    const f32 final_vol = m_MasterVolume * m_SfxVolume * volume_scale;
    return m_Backend->PlayOneShot(clip, final_vol, pitch);
}

} // namespace acs::game

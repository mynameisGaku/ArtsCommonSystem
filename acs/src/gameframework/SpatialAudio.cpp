// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — SpatialAudio 実装 (Phase 3)
//
// 3D listener + source の集中管理、距離減衰 / pan 計算、HRTF stub。
// 実 AudioEngine voice バインドは Phase 2 で AudioDirector と統合予定。
#include "gameframework/SpatialAudio.h"
#include "math/Math.h"
#include "foundation/Log.h"

namespace acs::game {

// =============================================================================
// HrtfRendererStub — 簡易 stereo panning だけ
// =============================================================================

TResult<void> HrtfRendererStub::Init() noexcept {
    _initialized = true;
    // Phase 2 で KEMAR 256-tap IR を埋め込み配列から構築する。stub は何も
    // ロードしない (= ~140KB 削減)。一度きりログで「HRTF off」を明示。
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        ACS_LOG_INFO("HrtfRendererStub::Init: HRTF disabled (Phase H-3 stub, panning only)");
    }
    return Ok();
}

void HrtfRendererStub::Shutdown() noexcept {
    _initialized = false;
}

void HrtfRendererStub::SetListener(const AudioListener& listener) noexcept {
    _listener = listener;
}

void HrtfRendererStub::ProcessSource(const AudioSource3D& source,
                                     const f32* mono_input,
                                     f32* stereo_output,
                                     u32 sample_count) noexcept {
    if (mono_input == nullptr || stereo_output == nullptr || sample_count == 0) {
        return;
    }
    // 非アクティブ source は 0 埋めで明示 (= mute)。
    if (!source.active) {
        for (u32 i = 0; i < sample_count; ++i) {
            stereo_output[i * 2 + 0] = 0.0f;
            stereo_output[i * 2 + 1] = 0.0f;
        }
        return;
    }
    // pan を listener 基準で計算 (SpatialAudio::ComputePan と同じ式)。
    // right = up × forward。標準姿勢 (forward=Z+, up=Y+) で右ベクトル X+ を
    // 返す式で、左手系 / 右手系どちらでも符号一貫 (Y+up を共通慣習にしている)。
    const FVec3 right = Cross(_listener.up, _listener.forward);
    const FVec3 to_src = source.position - _listener.position;
    f32 pan = 0.0f;
    const f32 len_sq = LengthSq(to_src);
    if (len_sq > kEpsilon) {
        const FVec3 dir = Normalize(to_src);
        const FVec3 right_n = Normalize(right);
        pan = Dot(dir, right_n);
        // 数値誤差で [-1,1] を越えうるので clamp。
        if (pan < -1.0f) pan = -1.0f;
        if (pan >  1.0f) pan =  1.0f;
    }
    const f32 gain_l = (1.0f - pan) * 0.5f * source.volume;
    const f32 gain_r = (1.0f + pan) * 0.5f * source.volume;
    for (u32 i = 0; i < sample_count; ++i) {
        const f32 s = mono_input[i];
        stereo_output[i * 2 + 0] = s * gain_l;
        stereo_output[i * 2 + 1] = s * gain_r;
    }
}

// =============================================================================
// SpatialAudio — 構築・listener
// =============================================================================

SpatialAudio::SpatialAudio() noexcept {
    _sources.Reserve(kInitialSourceCapacity);
}

void SpatialAudio::SetListener(const AudioListener& l) noexcept {
    _listener = l;
}

// =============================================================================
// SpatialAudio — source 管理
// =============================================================================

u32 SpatialAudio::RegisterSource(FVec3 pos, f32 max_distance,
                                  EAttenuationCurve curve) noexcept {
    AudioSource3D s {};
    s.source_id    = _next_source_id++;
    s.position     = pos;
    s.velocity     = FVec3::Zero();
    s.volume       = 1.0f;
    // 不正値は既定 20m。負 / 0 では culling が即発火して常に無音になり混乱。
    s.max_distance = (max_distance > 0.0f) ? max_distance : 20.0f;
    s.active       = true;
    s.curve        = static_cast<u8>(curve);
    _sources.PushBack(s);
    return s.source_id;
}

void SpatialAudio::UpdateSource(u32 id, FVec3 pos, FVec3 vel) noexcept {
    const usize idx = FindIndex(id);
    if (idx >= _sources.Size()) {
        ACS_LOG_WARN("SpatialAudio::UpdateSource: stale id=%u → ignored", id);
        return;
    }
    _sources[idx].position = pos;
    _sources[idx].velocity = vel;
}

void SpatialAudio::SetSourceVolume(u32 id, f32 v) noexcept {
    const usize idx = FindIndex(id);
    if (idx >= _sources.Size()) {
        ACS_LOG_WARN("SpatialAudio::SetSourceVolume: stale id=%u → ignored", id);
        return;
    }
    const f32 c = Saturate(v);
    if (c != v) {
        ACS_LOG_WARN("SpatialAudio::SetSourceVolume: out-of-range %.3f → clamped to %.3f", v, c);
    }
    _sources[idx].volume = c;
}

void SpatialAudio::RemoveSource(u32 id) noexcept {
    const usize idx = FindIndex(id);
    if (idx >= _sources.Size()) {
        // 既に削除済みは静かに無視 (典型: クロスフェード後の cleanup)。
        return;
    }
    // active=false のままにして、Tick で物理的に圧縮するのが理想だが、
    // Phase H-3 は素朴に末尾と swap して削除 (順序不問)。
    _sources.RemoveAtSwap(idx);
}

// =============================================================================
// SpatialAudio — 計算結果取得
// =============================================================================

f32 SpatialAudio::ComputeAttenuatedVolume(u32 id) const noexcept {
    const usize idx = FindIndex(id);
    if (idx >= _sources.Size()) return 0.0f;
    const AudioSource3D& s = _sources[idx];
    if (!s.active) return 0.0f;

    const FVec3 to_src = s.position - _listener.position;
    const f32  d      = Length(to_src);
    if (d >= s.max_distance) return 0.0f;  // culling

    // 0 距離は max 音量。各 curve で d=0 → 1, d=max → 0 を満たすように設計。
    f32 atten = 1.0f;
    const EAttenuationCurve curve = static_cast<EAttenuationCurve>(s.curve);
    switch (curve) {
    case EAttenuationCurve::Linear: {
        // 素朴な線形: vol = 1 - d / max_d
        atten = 1.0f - (d / s.max_distance);
        break;
    }
    case EAttenuationCurve::Inverse: {
        // 1 / (1 + r) 型を max_distance でスケール。
        //   ref_d = max_d / 8 を「半減距離の目安」とし、max_d で 0 に到達するよう
        //   tail を線形ブレンドで打ち切る (純粋 inverse は無限遠で 0 にしか
        //   ならないため culling と一貫させる)。
        const f32 ref_d = s.max_distance * 0.125f;
        const f32 base  = 1.0f / (1.0f + d / ref_d);
        const f32 t     = d / s.max_distance;   // 0..1
        atten = base * (1.0f - t);              // tail で 0 へ収束
        break;
    }
    case EAttenuationCurve::Exponential: {
        // e^(-k * d) 型、k = 4 / max_d で max_d で約 e^-4 ≈ 0.018。
        // 同じく tail を線形ブレンドして max_d で 0 に到達させる。
        const f32 k     = 4.0f / s.max_distance;
        const f32 base  = Exp(-k * d);
        const f32 t     = d / s.max_distance;
        atten = base * (1.0f - t);
        break;
    }
    }
    if (atten < 0.0f) atten = 0.0f;
    return s.volume * atten;
}

f32 SpatialAudio::ComputePan(u32 id) const noexcept {
    const usize idx = FindIndex(id);
    if (idx >= _sources.Size()) return 0.0f;
    const AudioSource3D& s = _sources[idx];
    if (!s.active) return 0.0f;

    const FVec3 to_src = s.position - _listener.position;
    const f32  len_sq = LengthSq(to_src);
    if (len_sq <= kEpsilon) {
        // listener の真上に重なっている場合は中央 (= 0)。
        return 0.0f;
    }
    // listener の右ベクトル = up × forward (HrtfRendererStub と同じ式)。
    const FVec3 right = Cross(_listener.up, _listener.forward);
    if (LengthSq(right) <= kEpsilon) {
        // forward と up が縮退している (= 不正な姿勢) → 中央扱い。
        return 0.0f;
    }
    const FVec3 dir     = Normalize(to_src);
    const FVec3 right_n = Normalize(right);
    f32 pan = Dot(dir, right_n);
    // 数値誤差で [-1,1] を越えうる (Normalize 後でも丸め誤差で +-1.00001 等)。
    if (pan < -1.0f) pan = -1.0f;
    if (pan >  1.0f) pan =  1.0f;
    return pan;
}

// =============================================================================
// SpatialAudio — 統計・driver
// =============================================================================

u32 SpatialAudio::SourceCount() const noexcept {
    u32 n = 0;
    for (usize i = 0; i < _sources.Size(); ++i) {
        if (_sources[i].active) ++n;
    }
    return n;
}

void SpatialAudio::Tick(f32 /*dt*/) noexcept {
    // Phase H-3 は state を進めない (listener / source は外から push)。
    // Phase 2 で:
    //   ・Doppler shift 計算 (velocity 比率 → pitch)
    //   ・古い inactive source の物理 GC
    //   ・streaming AudioEngine voice の更新
    // を追加予定。
}

void SpatialAudio::Clear() noexcept {
    _sources.Clear();
    // _next_source_id はリセットしない。Clear 直後に登録した source は新 ID
    // を受け取り、外部に握られた古い ID とは衝突しない (stale 検出が機能する)。
}

// =============================================================================
// 内部 helper
// =============================================================================

usize SpatialAudio::FindIndex(u32 id) const noexcept {
    if (id == 0) return _sources.Size();  // 0 は無効予約
    for (usize i = 0; i < _sources.Size(); ++i) {
        if (_sources[i].source_id == id) return i;
    }
    return _sources.Size();
}

} // namespace acs::game

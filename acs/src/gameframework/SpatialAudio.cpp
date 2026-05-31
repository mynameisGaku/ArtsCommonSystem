// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — FSpatialAudio 実装 (Phase 3)
//
// 3D listener + source の集中管理、距離減衰 / constant-power stereo panning。
// stereo パン + 距離減衰は実数学 (in-repo 完結) で、真のバイノーラル化
// (HRTF / KEMAR 256-tap convolution、外部 IR データ必須) のみが seam として残る。
// 実 FAudioEngine voice バインドは FAudioDirector と統合予定。
#include "gameframework/SpatialAudio.h"
#include "math/Math.h"
#include "foundation/Log.h"

namespace acs::game {

namespace {

// -----------------------------------------------------------------------------
// 距離減衰ゲイン — curve 種別に応じて d ∈ [0, max_d] を gain ∈ [0, 1] に写像。
// d >= max_d は culling (0)。d <= 0 は最大 (1)。FSpatialAudio::ComputeAttenuatedVolume
// と HrtfRendererStub::ProcessSource の双方で同一の式を共有する。
// -----------------------------------------------------------------------------
f32 ComputeAttenuationGain(f32 d, f32 max_distance, EAttenuationCurve curve) noexcept {
    if (max_distance <= 0.0f) return 0.0f;
    if (d <= 0.0f)            return 1.0f;
    if (d >= max_distance)    return 0.0f;  // culling

    f32 atten = 1.0f;
    switch (curve) {
    case EAttenuationCurve::Linear: {
        // 素朴な線形: gain = clamp(1 - d / max_d, 0, 1)。
        atten = 1.0f - (d / max_distance);
        break;
    }
    case EAttenuationCurve::Inverse: {
        // 1 / (1 + r) 型を max_distance でスケール。ref_d = max_d / 8 を
        // 半減距離の目安とし、tail を線形ブレンドで max_d で 0 に収束させる
        // (純粋 inverse は無限遠でしか 0 にならないため culling と一貫させる)。
        const f32 ref_d = max_distance * 0.125f;
        const f32 base  = 1.0f / (1.0f + d / ref_d);
        const f32 t     = d / max_distance;   // 0..1
        atten = base * (1.0f - t);            // tail で 0 へ収束
        break;
    }
    case EAttenuationCurve::Exponential: {
        // e^(-k * d) 型、k = 4 / max_d で max_d で約 e^-4 ≈ 0.018。
        // 同じく tail を線形ブレンドして max_d で 0 に到達させる。
        const f32 k    = 4.0f / max_distance;
        const f32 base = Exp(-k * d);
        const f32 t    = d / max_distance;
        atten = base * (1.0f - t);
        break;
    }
    }
    return Saturate(atten);
}

// -----------------------------------------------------------------------------
// Constant-power (等パワー) stereo パンゲイン — pan ∈ [-1, +1] を左右ゲインへ。
//   θ = (pan + 1) * π/4   (pan=-1→θ=0, pan=0→θ=π/4, pan=+1→θ=π/2)
//   left  = cos(θ),  right = sin(θ)
// linear pan ((1±pan)/2) と違い、中央でも left²+right² = 1 を満たすため
// 左右に振っても知覚音量 (パワー) が一定に保たれる (-3dB pan law)。
// -----------------------------------------------------------------------------
void ComputeConstantPowerGains(f32 pan, f32& left, f32& right) noexcept {
    if (pan < -1.0f) pan = -1.0f;
    if (pan >  1.0f) pan =  1.0f;
    const f32 theta = (pan + 1.0f) * (kPi * 0.25f);
    left  = Cos(theta);
    right = Sin(theta);
}

} // namespace

// =============================================================================
// HrtfRendererStub — constant-power stereo panning + 距離減衰 (HRTF seam のみ stub)
// =============================================================================

TResult<void> HrtfRendererStub::Init() noexcept {
    m_Initialized = true;
    // 真の HRTF (KEMAR 256-tap convolution) は外部 IR データを要するため
    // seam として保留。本 stub は constant-power パン + 距離減衰を実数学で行う。
    // 一度きりログで「HRTF off (panning は実装済)」を明示。
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        ACS_LOG_INFO("HrtfRendererStub::Init: HRTF binaural disabled (seam), "
                     "using real constant-power stereo panning + distance attenuation");
    }
    return Ok();
}

void HrtfRendererStub::Shutdown() noexcept {
    m_Initialized = false;
}

void HrtfRendererStub::SetListener(const FAudioListener& listener) noexcept {
    m_Listener = listener;
}

void HrtfRendererStub::ProcessSource(const FAudioSource3D& source,
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
    // pan を listener 基準で計算 (FSpatialAudio::ComputePan と同じ式)。
    // right = up × forward。標準姿勢 (forward=Z+, up=Y+) で右ベクトル X+ を
    // 返す式で、左手系 / 右手系どちらでも符号一貫 (Y+up を共通慣習にしている)。
    const FVec3 right  = Cross(m_Listener.up, m_Listener.forward);
    const FVec3 to_src = source.position - m_Listener.position;
    const f32   d      = Length(to_src);
    f32 pan = 0.0f;
    if (d > kEpsilon && LengthSq(right) > kEpsilon) {
        const FVec3 dir     = Normalize(to_src);
        const FVec3 right_n = Normalize(right);
        pan = Dot(dir, right_n);
        // 数値誤差で [-1,1] を越えうるので clamp。
        if (pan < -1.0f) pan = -1.0f;
        if (pan >  1.0f) pan =  1.0f;
    }
    // 距離減衰 (curve 種別) を listener 基準で適用。FSpatialAudio::
    // ComputeAttenuatedVolume と同一の式を共有するので、pull API と
    // per-sample 出力で減衰量が一致する。
    const EAttenuationCurve curve = static_cast<EAttenuationCurve>(source.curve);
    const f32 atten = ComputeAttenuationGain(d, source.max_distance, curve);
    // constant-power (等パワー) パン: left² + right² = 1 を満たし、
    // 左右に振っても知覚音量が一定 (linear pan の中央 -6dB ディップを回避)。
    f32 pan_l = 0.0f;
    f32 pan_r = 0.0f;
    ComputeConstantPowerGains(pan, pan_l, pan_r);
    const f32 gain_l = pan_l * source.volume * atten;
    const f32 gain_r = pan_r * source.volume * atten;
    for (u32 i = 0; i < sample_count; ++i) {
        const f32 s = mono_input[i];
        stereo_output[i * 2 + 0] = s * gain_l;
        stereo_output[i * 2 + 1] = s * gain_r;
    }
}

// =============================================================================
// FSpatialAudio — 構築・listener
// =============================================================================

FSpatialAudio::FSpatialAudio() noexcept {
    m_Sources.Reserve(kInitialSourceCapacity);
}

void FSpatialAudio::SetListener(const FAudioListener& l) noexcept {
    m_Listener = l;
}

// =============================================================================
// FSpatialAudio — source 管理
// =============================================================================

u32 FSpatialAudio::RegisterSource(FVec3 pos, f32 max_distance,
                                  EAttenuationCurve curve) noexcept {
    FAudioSource3D s {};
    s.source_id    = m_NextSourceId++;
    s.position     = pos;
    s.velocity     = FVec3::Zero();
    s.volume       = 1.0f;
    // 不正値は既定 20m。負 / 0 では culling が即発火して常に無音になり混乱。
    s.max_distance = (max_distance > 0.0f) ? max_distance : 20.0f;
    s.active       = true;
    s.curve        = static_cast<u8>(curve);
    m_Sources.PushBack(s);
    return s.source_id;
}

void FSpatialAudio::UpdateSource(u32 id, FVec3 pos, FVec3 vel) noexcept {
    const usize idx = FindIndex(id);
    if (idx >= m_Sources.Size()) {
        ACS_LOG_WARN("FSpatialAudio::UpdateSource: stale id=%u → ignored", id);
        return;
    }
    m_Sources[idx].position = pos;
    m_Sources[idx].velocity = vel;
}

void FSpatialAudio::SetSourceVolume(u32 id, f32 v) noexcept {
    const usize idx = FindIndex(id);
    if (idx >= m_Sources.Size()) {
        ACS_LOG_WARN("FSpatialAudio::SetSourceVolume: stale id=%u → ignored", id);
        return;
    }
    const f32 c = Saturate(v);
    if (c != v) {
        ACS_LOG_WARN("FSpatialAudio::SetSourceVolume: out-of-range %.3f → clamped to %.3f", v, c);
    }
    m_Sources[idx].volume = c;
}

void FSpatialAudio::RemoveSource(u32 id) noexcept {
    const usize idx = FindIndex(id);
    if (idx >= m_Sources.Size()) {
        // 既に削除済みは静かに無視 (典型: クロスフェード後の cleanup)。
        return;
    }
    // active=false のままにして、Tick で物理的に圧縮するのが理想だが、
    // Phase H-3 は素朴に末尾と swap して削除 (順序不問)。
    m_Sources.RemoveAtSwap(idx);
}

// =============================================================================
// FSpatialAudio — 計算結果取得
// =============================================================================

f32 FSpatialAudio::ComputeAttenuatedVolume(u32 id) const noexcept {
    const usize idx = FindIndex(id);
    if (idx >= m_Sources.Size()) return 0.0f;
    const FAudioSource3D& s = m_Sources[idx];
    if (!s.active) return 0.0f;

    const FVec3 to_src = s.position - m_Listener.position;
    const f32  d      = Length(to_src);
    // 距離減衰は ProcessSource と共有する curve-aware helper で算出
    // (d >= max_distance の culling、d=0 → 1, d=max → 0 を満たす)。
    const EAttenuationCurve curve = static_cast<EAttenuationCurve>(s.curve);
    const f32 atten = ComputeAttenuationGain(d, s.max_distance, curve);
    return s.volume * atten;
}

f32 FSpatialAudio::ComputePan(u32 id) const noexcept {
    const usize idx = FindIndex(id);
    if (idx >= m_Sources.Size()) return 0.0f;
    const FAudioSource3D& s = m_Sources[idx];
    if (!s.active) return 0.0f;

    const FVec3 to_src = s.position - m_Listener.position;
    const f32  len_sq = LengthSq(to_src);
    if (len_sq <= kEpsilon) {
        // listener の真上に重なっている場合は中央 (= 0)。
        return 0.0f;
    }
    // listener の右ベクトル = up × forward (HrtfRendererStub と同じ式)。
    const FVec3 right = Cross(m_Listener.up, m_Listener.forward);
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
// FSpatialAudio — 統計・driver
// =============================================================================

u32 FSpatialAudio::SourceCount() const noexcept {
    u32 n = 0;
    for (usize i = 0; i < m_Sources.Size(); ++i) {
        if (m_Sources[i].active) ++n;
    }
    return n;
}

void FSpatialAudio::Tick(f32 /*dt*/) noexcept {
    // Phase H-3 は state を進めない (listener / source は外から push)。
    // Phase 2 で:
    //   ・Doppler shift 計算 (velocity 比率 → pitch)
    //   ・古い inactive source の物理 GC
    //   ・streaming FAudioEngine voice の更新
    // を追加予定。
}

void FSpatialAudio::Clear() noexcept {
    m_Sources.Clear();
    // m_NextSourceId はリセットしない。Clear 直後に登録した source は新 ID
    // を受け取り、外部に握られた古い ID とは衝突しない (stale 検出が機能する)。
}

// =============================================================================
// 内部 helper
// =============================================================================

usize FSpatialAudio::FindIndex(u32 id) const noexcept {
    if (id == 0) return m_Sources.Size();  // 0 は無効予約
    for (usize i = 0; i < m_Sources.Size(); ++i) {
        if (m_Sources[i].source_id == id) return i;
    }
    return m_Sources.Size();
}

} // namespace acs::game

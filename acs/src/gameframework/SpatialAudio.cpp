// SPDX-License-Identifier: Apache-2.0
// CSpatialAudio は scene-local な listener/source 状態から距離減衰と
// constant-power stereo pan を計算し、CAudioDirector が再生中 voice へ反映する。
#include "gameframework/SpatialAudio.h"
#include "math/Math.h"
#include "foundation/Log.h"

namespace acs::game {

namespace {

/** 非有限の各成分を 0 に置き換えた決定論的な位置・方向を返す。 */
FVec3 MakeFiniteVector(FVec3 value) noexcept {
    if (!std::isfinite(value.x)) value.x = 0.0f;
    if (!std::isfinite(value.y)) value.y = 0.0f;
    if (!std::isfinite(value.z)) value.z = 0.0f;
    return value;
}

/**
 * curve 種別に応じて距離 d を距離減衰ゲイン [0, 1] に写像する。
 *
 * @details
 * d >= max_distance は culling (0)、d <= 0 は最大 (1)。CSpatialAudio::
 * ComputeAttenuatedVolume と HrtfRendererStub::ProcessSource の双方で同一の式を
 * 共有する。
 * @param d listener から source までの距離。
 * @param max_distance この距離以上で culling する最大可聴距離。
 * @param curve 適用する減衰カーブ種別。
 * @return [0, 1] の減衰ゲイン。
 */
f32 ComputeAttenuationGain(f32 d, f32 max_distance, EAttenuationCurve curve) noexcept {
    if (!std::isfinite(d) || !std::isfinite(max_distance) || max_distance <= 0.0f) return 0.0f;
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

/**
 * pan ∈ [-1, +1] を constant-power (等パワー) の左右ステレオゲインへ変換する。
 *
 * @details
 * θ = (pan + 1) * π/4 とし、left = cos(θ)、right = sin(θ)。linear pan ((1±pan)/2) と
 * 違い中央でも left² + right² = 1 を満たすため、左右に振っても知覚音量 (パワー) が
 * 一定に保たれる (-3dB pan law)。pan は内部で [-1, +1] に clamp する。
 * @param pan パンニング位置 [-1=左, 0=中央, +1=右]。
 * @param left 左チャンネルゲインの書き戻し先。
 * @param right 右チャンネルゲインの書き戻し先。
 */
void ComputeConstantPowerGains(f32 pan, f32& left, f32& right) noexcept {
    if (pan < -1.0f) pan = -1.0f;
    if (pan >  1.0f) pan =  1.0f;
    const f32 theta = (pan + 1.0f) * (kPi * 0.25f);
    left  = Cos(theta);
    right = Sin(theta);
}

} // namespace

/** stub renderer を初期化する (constant-power パン + 距離減衰を使う)。 */
TResult<void> CHrtfRendererStub::Init() noexcept {
    m_Initialized = true;
    // 外部 IR は読み込まず、constant-power パン + 距離減衰を実数学で適用する。
    // 初期化時に現在の出力モードを一度ログへ通知する。
    // C++ のローカル static 初期化保証を使い、並行 Init でもログを一度だけ出す。
    static const bool bLogged = []() noexcept
    {
        ACS_LOG_INFO("HrtfRendererStub::Init: HRTF binaural disabled (seam), "
                     "using real constant-power stereo panning + distance attenuation");
        return true;
    }();
    (void)bLogged;
    return Ok();
}

/** stub renderer を停止する。 */
void CHrtfRendererStub::Shutdown() noexcept {
    m_Initialized = false;
}

/** パン / 減衰計算の基準となる listener を設定する。 */
void CHrtfRendererStub::SetListener(const FAudioListener& listener) noexcept {
    m_Listener.position = MakeFiniteVector(listener.position);
    m_Listener.forward = MakeFiniteVector(listener.forward);
    m_Listener.up = MakeFiniteVector(listener.up);
}

/** mono 入力に pan + 距離減衰を適用し interleaved stereo 出力へ書き込む。 */
void CHrtfRendererStub::ProcessSource(const FAudioSource3D& source,
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
    // pan を listener 基準で計算 (CSpatialAudio::ComputePan と同じ式)。
    // right = up × forward。標準姿勢 (forward=Z+, up=Y+) で右ベクトル X+ を
    // 返す式で、左手系 / 右手系どちらでも符号一貫 (Y+up を共通慣習にしている)。
    const FVec3 right  = Cross(m_Listener.up, m_Listener.forward);
    const FVec3 to_src = MakeFiniteVector(source.position) - m_Listener.position;
    const f32   d      = Length(to_src);
    f32 pan = 0.0f;
    const f32 right_len_sq = LengthSq(right);
    if (std::isfinite(d) && d > kEpsilon && std::isfinite(right_len_sq) && right_len_sq > kEpsilon) {
        const FVec3 dir     = Normalize(to_src);
        const FVec3 right_n = Normalize(right);
        pan = Dot(dir, right_n);
        if (!std::isfinite(pan)) pan = 0.0f;
        // 数値誤差で [-1,1] を越えうるので clamp。
        if (pan < -1.0f) pan = -1.0f;
        if (pan >  1.0f) pan =  1.0f;
    }
    // 距離減衰 (curve 種別) を listener 基準で適用。CSpatialAudio::
    // ComputeAttenuatedVolume と同一の式を共有するので、pull API と
    // per-sample 出力で減衰量が一致する。
    const EAttenuationCurve curve = static_cast<EAttenuationCurve>(source.curve);
    const f32 atten = ComputeAttenuationGain(d, source.max_distance, curve);
    // constant-power (等パワー) パン: left² + right² = 1 を満たし、
    // 左右に振っても知覚音量が一定 (linear pan の中央 -6dB ディップを回避)。
    f32 pan_l = 0.0f;
    f32 pan_r = 0.0f;
    ComputeConstantPowerGains(pan, pan_l, pan_r);
    f32 source_volume = source.volume;
    if (!std::isfinite(source_volume)) source_volume = 0.0f;
    source_volume = Saturate(source_volume);
    f32 gain_l = pan_l * source_volume * atten;
    f32 gain_r = pan_r * source_volume * atten;
    if (!std::isfinite(gain_l)) gain_l = 0.0f;
    if (!std::isfinite(gain_r)) gain_r = 0.0f;
    for (u32 i = 0; i < sample_count; ++i) {
        const f32 s = std::isfinite(mono_input[i]) ? mono_input[i] : 0.0f;
        f32 output_l = s * gain_l;
        f32 output_r = s * gain_r;
        if (!std::isfinite(output_l)) output_l = 0.0f;
        if (!std::isfinite(output_r)) output_r = 0.0f;
        stereo_output[i * 2 + 0] = output_l;
        stereo_output[i * 2 + 1] = output_r;
    }
}

/** source 配列を初期容量で予約して構築する。 */
CSpatialAudio::CSpatialAudio() noexcept {
    m_Sources.Reserve(kInitialSourceCapacity);
}

/** 計算基準となる listener (位置・姿勢) を設定する。 */
void CSpatialAudio::SetListener(const FAudioListener& l) noexcept {
    m_Listener.position = MakeFiniteVector(l.position);
    m_Listener.forward = MakeFiniteVector(l.forward);
    m_Listener.up = MakeFiniteVector(l.up);
}

/** 3D source を登録して割り当てた id を返す (不正 max_distance は 20m に既定)。 */
u32 CSpatialAudio::RegisterSource(FVec3 pos, f32 max_distance,
                                  EAttenuationCurve curve) noexcept {
    if (m_NextSourceId == 0u) {
        ACS_LOG_WARN("CSpatialAudio::RegisterSource: source ID space exhausted → ignored");
        return 0u;
    }
    FAudioSource3D s {};
    s.source_id    = m_NextSourceId++;
    s.position     = MakeFiniteVector(pos);
    s.velocity     = FVec3::Zero();
    s.volume       = 1.0f;
    // 不正値は既定 20m。負 / 0 では culling が即発火して常に無音になり混乱。
    s.max_distance = (std::isfinite(max_distance) && max_distance > 0.0f) ? max_distance : 20.0f;
    s.active       = true;
    s.curve        = static_cast<u8>(curve);
    m_Sources.Add(s);
    return s.source_id;
}

#if defined(ACS_GAMEFRAMEWORK_TEST_HOOKS)
/** 次に払い出す source ID を境界値に設定するテスト専用 hook。 */
void CSpatialAudio::SetNextSourceIdForTesting(u32 next_source_id) noexcept {
    m_NextSourceId = next_source_id;
}
#endif

/** source の位置と速度を更新する (stale id は警告して無視)。 */
void CSpatialAudio::UpdateSource(u32 id, FVec3 pos, FVec3 vel) noexcept {
    const usize idx = FindIndex(id);
    if (idx >= m_Sources.Num()) {
        ACS_LOG_WARN("CSpatialAudio::UpdateSource: stale id=%u → ignored", id);
        return;
    }
    m_Sources[idx].position = MakeFiniteVector(pos);
    m_Sources[idx].velocity = MakeFiniteVector(vel);
}

/** source の基準音量を設定する ([0,1] に clamp、stale id は警告して無視)。 */
void CSpatialAudio::SetSourceVolume(u32 id, f32 v) noexcept {
    const usize idx = FindIndex(id);
    if (idx >= m_Sources.Num()) {
        ACS_LOG_WARN("CSpatialAudio::SetSourceVolume: stale id=%u → ignored", id);
        return;
    }
    const f32 c = std::isfinite(v) ? Saturate(v) : 0.0f;
    if (c != v) {
        ACS_LOG_WARN("CSpatialAudio::SetSourceVolume: out-of-range %.3f → clamped to %.3f", v, c);
    }
    m_Sources[idx].volume = c;
}

/** source を削除する (未登録 / 削除済みは静かに無視、順序非保持)。 */
void CSpatialAudio::RemoveSource(u32 id) noexcept {
    const usize idx = FindIndex(id);
    if (idx >= m_Sources.Num()) {
        // 既に削除済みは静かに無視 (典型: クロスフェード後の cleanup)。
        return;
    }
    // 順序を維持せず末尾要素と入れ替えて即時削除する。
    m_Sources.RemoveAtSwap(idx);
}

/** source の距離減衰後の音量を返す (未登録 / 非アクティブは 0)。 */
f32 CSpatialAudio::ComputeAttenuatedVolume(u32 id) const noexcept {
    const usize idx = FindIndex(id);
    if (idx >= m_Sources.Num()) return 0.0f;
    const FAudioSource3D& s = m_Sources[idx];
    if (!s.active) return 0.0f;

    const FVec3 to_src = s.position - m_Listener.position;
    const f32  d      = Length(to_src);
    // 距離減衰は ProcessSource と共有する curve-aware helper で算出
    // (d >= max_distance の culling、d=0 → 1, d=max → 0 を満たす)。
    const EAttenuationCurve curve = static_cast<EAttenuationCurve>(s.curve);
    const f32 atten = ComputeAttenuationGain(d, s.max_distance, curve);
    const f32 volume = s.volume * atten;
    return std::isfinite(volume) ? Saturate(volume) : 0.0f;
}

/** source の pan [-1,+1] を listener 基準で返す (未登録 / 縮退姿勢は 0=中央)。 */
f32 CSpatialAudio::ComputePan(u32 id) const noexcept {
    const usize idx = FindIndex(id);
    if (idx >= m_Sources.Num()) return 0.0f;
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
    if (!std::isfinite(pan)) return 0.0f;
    // 数値誤差で [-1,1] を越えうる (Normalize 後でも丸め誤差で +-1.00001 等)。
    if (pan < -1.0f) pan = -1.0f;
    if (pan >  1.0f) pan =  1.0f;
    return pan;
}

/** アクティブな source の数を返す。 */
u32 CSpatialAudio::SourceCount() const noexcept {
    u32 n = 0;
    for (usize i = 0; i < m_Sources.Num(); ++i) {
        if (m_Sources[i].active) ++n;
    }
    return n;
}

/** listener/source は外部更新のため state を変更しない。 */
void CSpatialAudio::Tick(f32 /*dt*/) noexcept {
}

/** 全 source を削除する (m_NextSourceId はリセットしない)。 */
void CSpatialAudio::Clear() noexcept {
    m_Sources.Reset();
    // m_NextSourceId はリセットしない。Clear 直後に登録した source は新 ID
    // を受け取り、外部に握られた古い ID とは衝突しない (stale 検出が機能する)。
}

/** id 一致 source の index を返す (未検出 / id==0 は m_Sources.Size())。 */
usize CSpatialAudio::FindIndex(u32 id) const noexcept {
    if (id == 0) return m_Sources.Num();  // 0 は無効予約
    for (usize i = 0; i < m_Sources.Num(); ++i) {
        if (m_Sources[i].source_id == id) return i;
    }
    return m_Sources.Num();
}

/** source ID が現在登録されているかを返す。 */
bool CSpatialAudio::HasSource(u32 id) const noexcept {
    const usize idx = FindIndex(id);
    return idx < m_Sources.Num() && m_Sources[idx].active;
}

} // namespace acs::game

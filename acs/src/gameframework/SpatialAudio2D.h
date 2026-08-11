// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Math.h"
#include "container/Array.h"
#include "gameframework/SpatialAudio.h"   // EAttenuationCurve を共有する

namespace acs::game {

/**
 * 2D 平面上の listener (耳位置 + 向き)。
 *
 * @details 3D の forward/up 三次元姿勢を「向き角 1 つ」へ縮約したもの。
 */
struct FAudioListener2D {
    /** 世界座標 (pan / 距離の基準点)。 */
    FVec2 position = FVec2::Zero();

    /** 向き角 (ラジアン)。forward = (cos, sin)。Y+ up 慣習。 */
    f32   angle    = 0.0f;
};

/**
 * 2D 平面上の 1 音源。FAudioSource3D の 2D 版。
 */
struct FAudioSource2D {
    /** CSpatialAudio2D が払い出す一意 ID (0 = 無効、1.. = 有効)。 */
    u32   source_id    = 0;

    /** 世界座標。 */
    FVec2 position     = FVec2::Zero();

    /** Doppler 計算用の速度。距離減衰と pan 計算では参照しない。 */
    FVec2 velocity     = FVec2::Zero();

    /** 基準ゲイン [0, 1] (距離減衰前の素の音量)。 */
    f32   volume       = 1.0f;

    /** culling 距離 (これより遠いと vol=0)。 */
    f32   max_distance = 20.0f;

    /** false なら減衰 / パン計算をスキップ。 */
    bool  active       = true;

    /** 距離減衰カーブ種別 (既定 Linear)。 */
    u8    curve        = 0; // = EAttenuationCurve::Linear
};

// =============================================================================
// 純数学 free function 群 (state を持たない。AScene が独自に source 配列を持つ場合に直叩き)
// =============================================================================

/**
 * listener と source の平面距離を返す。
 *
 * @param a 基準点 (典型: listener 耳位置)。
 * @param b 対象点 (典型: source 位置)。
 * @return |b - a| (ユークリッド距離、ピタゴラス)。
 */
ACS_FORCEINLINE f32 ComputeDistance2D(FVec2 a, FVec2 b) noexcept {
    return Length(b - a);
}

/**
 * 距離と curve から減衰ゲイン [0, 1] を返す (3D ComputeAttenuationGain と同一式)。
 *
 * @details
 * Linear:      gain = 1 - d/max。
 * Inverse:     ref_d = max/8、base = 1/(1 + d/ref_d)、tail = (1 - d/max) を乗じ max で 0 収束。
 * Exponential: k = 4/max、base = e^(-k·d)、同じく tail blend で max で 0 到達。
 * d <= 0 は 1、d >= max_range は 0 (culling)、max_range <= 0 は 0。
 * @param distance listener からの距離 (>= 0 を想定)。
 * @param max_range culling 距離。
 * @param curve 減衰カーブ種別。
 * @return [0, 1] の減衰ゲイン。
 */
ACS_FORCEINLINE f32 ComputeVolume2D(f32 distance, f32 max_range,
                                    EAttenuationCurve curve) noexcept {
    if (max_range <= 0.0f) return 0.0f;
    if (distance <= 0.0f)  return 1.0f;
    if (distance >= max_range) return 0.0f;   // culling

    f32 atten = 1.0f;
    switch (curve) {
    case EAttenuationCurve::Linear:
        atten = 1.0f - (distance / max_range);
        break;
    case EAttenuationCurve::Inverse: {
        const f32 ref_d = max_range * 0.125f;          // 半減距離の目安 = max/8
        const f32 base  = 1.0f / (1.0f + distance / ref_d);
        const f32 t     = distance / max_range;        // 0..1
        atten = base * (1.0f - t);                     // tail で 0 へ収束
        break;
    }
    case EAttenuationCurve::Exponential: {
        const f32 k    = 4.0f / max_range;             // max で約 e^-4
        const f32 base = Exp(-k * distance);
        const f32 t    = distance / max_range;
        atten = base * (1.0f - t);
        break;
    }
    }
    return Saturate(atten);
}

/**
 * listener の向きを基準にした左右パン [-1, +1] を返す。
 *
 * @details
 * forward = (cos angle, sin angle)、右手ベクトル right = (forward.y, -forward.x)
 * (forward を時計回り 90° 回転)。pan = dot(normalize(to_src), right) を [-1,1] に clamp。
 * 真正面 / 真後ろ = 0、真右 = +1、真左 = -1。listener に重なる点は 0。
 * @param listener_pos listener の世界座標。
 * @param listener_angle_rad listener の向き角 (ラジアン、Y+ up)。
 * @param source_pos source の世界座標。
 * @return パン値 (-1=完全左, 0=正面/真後ろ, +1=完全右)。
 */
ACS_FORCEINLINE f32 ComputePan2D(FVec2 listener_pos, f32 listener_angle_rad,
                                 FVec2 source_pos) noexcept {
    const FVec2 to_src = source_pos - listener_pos;
    if (LengthSq(to_src) <= kEpsilon) return 0.0f;       // 重なり → 中央

    const FVec2 fwd   = { Cos(listener_angle_rad), Sin(listener_angle_rad) };
    const FVec2 right = { fwd.y, -fwd.x };               // forward を CW 90° = 右手
    const FVec2 dir   = Normalize(to_src);
    f32 pan = Dot(dir, right);
    if (pan < -1.0f) pan = -1.0f;                         // 丸め誤差クランプ
    if (pan >  1.0f) pan =  1.0f;
    return pan;
}

/**
 * pan ∈ [-1, +1] を constant-power (等パワー / -3dB) の左右ステレオゲインに変換する。
 *
 * @details θ = (pan + 1)·π/4、left = cos θ、right = sin θ。中央でも left²+right² = 1。
 * 3D ComputeConstantPowerGains と同式。pan は内部で [-1, +1] に clamp。
 * @param pan パンニング位置 [-1=左, 0=中央, +1=右]。
 * @param left 左チャンネルゲインの書き戻し先。
 * @param right 右チャンネルゲインの書き戻し先。
 */
ACS_FORCEINLINE void ComputeConstantPowerStereo2D(f32 pan, f32& left, f32& right) noexcept {
    if (pan < -1.0f) pan = -1.0f;
    if (pan >  1.0f) pan =  1.0f;
    const f32 theta = (pan + 1.0f) * (kPi * 0.25f);
    left  = Cos(theta);
    right = Sin(theta);
}

// =============================================================================
// CSpatialAudio2D — listener 1 + source N の集中管理 (CSpatialAudio の 2D 版、header-only)
// =============================================================================

/**
 * 2D listener + source を集中管理する空間化レイヤ (CSpatialAudio の 2D 版)。
 *
 * @details
 * AScene 局所 instance としての所有を想定。1 listener + N source を AoS で保持し、
 * 毎フレーム distance / volume / pan を pull で取得する。source_id は単調増加で
 * 再利用しない (stale ID 検出が単純)。全メソッド inline (header-only)。
 */
class CSpatialAudio2D {
public:
    /** source 配列の初期容量。 */
    static constexpr u32 kInitialSourceCapacity = 16;

    /** source 配列を初期容量で Reserve して構築する。 */
    CSpatialAudio2D() noexcept { m_Sources.Reserve(kInitialSourceCapacity); }

    /** 破棄する。 */
    ~CSpatialAudio2D() noexcept = default;

    /** コピー禁止 (シーン局所 instance として単独所有)。 */
    CSpatialAudio2D(const CSpatialAudio2D&)            = delete;
    CSpatialAudio2D& operator=(const CSpatialAudio2D&) = delete;
    CSpatialAudio2D(CSpatialAudio2D&&)                 = delete;
    CSpatialAudio2D& operator=(CSpatialAudio2D&&)      = delete;

    /**
     * listener (耳位置 + 向き) を設定する。
     *
     * @param l 新しい listener 状態。
     */
    void SetListener(const FAudioListener2D& l) noexcept { m_Listener = l; }

    /**
     * 現在の listener への const 参照を返す。
     *
     * @return 保持中の listener。
     */
    const FAudioListener2D& GetListener() const noexcept { return m_Listener; }

    /**
     * 新規 source を登録する。
     *
     * @param pos 初期世界座標。
     * @param max_distance culling 距離 (<= 0 は既定 20m にクランプ)。
     * @param curve 距離減衰カーブ種別。
     * @return 払い出した source_id (1.. の単調増加)。
     */
    u32 RegisterSource(FVec2 pos, f32 max_distance, EAttenuationCurve curve) noexcept {
        FAudioSource2D s{};
        s.source_id    = m_NextSourceId++;
        s.position     = pos;
        s.max_distance = (max_distance > 0.0f) ? max_distance : 20.0f;
        s.curve        = static_cast<u8>(curve);
        s.active       = true;
        m_Sources.Add(s);
        return s.source_id;
    }

    /**
     * source の位置 / 速度を更新する (stale ID は no-op)。
     *
     * @param id 更新対象の source_id。
     * @param pos 新しい世界座標。
     * @param vel 新しい速度 (既定ゼロ)。
     */
    void UpdateSource(u32 id, FVec2 pos, FVec2 vel = FVec2::Zero()) noexcept {
        const usize idx = FindIndex(id);
        if (idx >= m_Sources.Num()) return;
        m_Sources[idx].position = pos;
        m_Sources[idx].velocity = vel;
    }

    /**
     * source の基準ゲインを [0,1] に clamp して設定する (stale ID は no-op)。
     *
     * @param id 対象の source_id。
     * @param v 新しい基準ゲイン。
     */
    void SetSourceVolume(u32 id, f32 v) noexcept {
        const usize idx = FindIndex(id);
        if (idx >= m_Sources.Num()) return;
        m_Sources[idx].volume = Saturate(v);
    }

    /**
     * source を削除する (末尾と swap 除去、slot は再利用しない、stale ID は無視)。
     *
     * @param id 削除対象の source_id。
     */
    void RemoveSource(u32 id) noexcept {
        const usize idx = FindIndex(id);
        if (idx >= m_Sources.Num()) return;
        const usize last = m_Sources.Num() - 1;
        if (idx != last) m_Sources[idx] = m_Sources[last];
        m_Sources.Pop();
    }

    /**
     * listener との距離と curve から算出した最終 volume を返す。
     *
     * @param id 対象の source_id。
     * @return source.volume × 減衰ゲイン。無効 ID / inactive / dist >= max では 0。
     */
    f32 ComputeAttenuatedVolume(u32 id) const noexcept {
        const usize idx = FindIndex(id);
        if (idx >= m_Sources.Num()) return 0.0f;
        const FAudioSource2D& s = m_Sources[idx];
        if (!s.active) return 0.0f;
        const f32 d = ComputeDistance2D(m_Listener.position, s.position);
        const f32 g = ComputeVolume2D(d, s.max_distance,
                                      static_cast<EAttenuationCurve>(s.curve));
        return s.volume * g;
    }

    /**
     * listener 基準の左右パン [-1, +1] を返す。
     *
     * @param id 対象の source_id。
     * @return パン値。無効 ID / inactive で 0。
     */
    f32 ComputePan(u32 id) const noexcept {
        const usize idx = FindIndex(id);
        if (idx >= m_Sources.Num()) return 0.0f;
        const FAudioSource2D& s = m_Sources[idx];
        if (!s.active) return 0.0f;
        return ComputePan2D(m_Listener.position, m_Listener.angle, s.position);
    }

    /**
     * active な source の数を返す。
     *
     * @return active==true の source 数。
     */
    u32 SourceCount() const noexcept {
        u32 n = 0;
        for (usize i = 0; i < m_Sources.Num(); ++i) {
            if (m_Sources[i].active) ++n;
        }
        return n;
    }

    /** 全 source を空にする (listener は保持、source_id カウンタは継続)。 */
    void Clear() noexcept { m_Sources.Reset(); }

private:
    /**
     * source_id を index に線形検索で変換する。
     *
     * @param id 検索する source_id (0 は無効予約)。
     * @return 見つかった index、無ければ m_Sources.Size()。
     */
    usize FindIndex(u32 id) const noexcept {
        if (id == 0) return m_Sources.Num();
        for (usize i = 0; i < m_Sources.Num(); ++i) {
            if (m_Sources[i].source_id == id) return i;
        }
        return m_Sources.Num();
    }

    /** 保持中の listener 状態。 */
    FAudioListener2D       m_Listener {};

    /** 登録済み 2D 音源の配列。 */
    TArray<FAudioSource2D> m_Sources;

    /** 次に払い出す source_id (0 = 無効予約)。 */
    u32                    m_NextSourceId = 1;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FSpatialAudio2D = CSpatialAudio2D;

} // namespace acs::game

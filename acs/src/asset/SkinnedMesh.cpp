// SPDX-License-Identifier: Apache-2.0
// ASkinnedMeshAsset + CAnimationPlayer 実装
#include "asset/SkinnedMesh.h"
#include "math/Math.h"
#include "foundation/Move.h"

#include <cmath>

namespace acs {

namespace {

/**
 * ローカル TRS から行列を合成する (row-major: scale → rotation → translation)。
 *
 * @param t 平行移動。
 * @param r 回転。
 * @param s スケール。
 * @return 合成したローカル変換行列。
 */
FMat4 ComposeTRS(FVec3 t, FQuat r, FVec3 s) noexcept {
    return FMat4::Scale(s) * ToMatrix(r) * FMat4::Translation(t);
}

/**
 * アニメーションチャネルから時刻 t の TRS を補間サンプリングする。
 *
 * @details キーが空なら単位 TRS、端の外なら端のキー値、それ以外は隣接 2 キーを
 * 線形補間 (回転は slerp) する。キー探索は線形 (チャネルあたりのキー数が少ない前提)。
 * @param ch サンプリングするアニメーションチャネル。
 * @param t サンプリングする時刻 (秒)。
 * @param out_t 補間後の平行移動を受け取る。
 * @param out_r 補間後の回転を受け取る。
 * @param out_s 補間後のスケールを受け取る。
 */
void SampleChannel(const FAnimationChannel& ch, f32 t,
                   FVec3& out_t, FQuat& out_r, FVec3& out_s) noexcept {
    const usize n = ch.keys.Num();
    if (n == 0) {
        out_t = FVec3{0, 0, 0};
        out_r = FQuat{};
        out_s = FVec3{1, 1, 1};
        return;
    }
    if (n == 1 || t <= ch.keys[0].time) {
        const FAnimationKey& k = ch.keys[0];
        out_t = k.translation; out_r = k.rotation; out_s = k.scale;
        return;
    }
    if (t >= ch.keys[n - 1].time) {
        const FAnimationKey& k = ch.keys[n - 1];
        out_t = k.translation; out_r = k.rotation; out_s = k.scale;
        return;
    }
    // 二分探索ではなく線形（チャネルあたりキー数は通常少ない）
    usize i = 0;
    while (i + 1 < n && ch.keys[i + 1].time < t) ++i;
    const FAnimationKey& a = ch.keys[i];
    const FAnimationKey& b = ch.keys[i + 1];
    const f32 dt = b.time - a.time;
    const f32 alpha = dt > 1e-6f ? (t - a.time) / dt : 0.0f;
    out_t = FVec3{
        a.translation.x + (b.translation.x - a.translation.x) * alpha,
        a.translation.y + (b.translation.y - a.translation.y) * alpha,
        a.translation.z + (b.translation.z - a.translation.z) * alpha,
    };
    out_r = Slerp(a.rotation, b.rotation, alpha);
    out_s = FVec3{
        a.scale.x + (b.scale.x - a.scale.x) * alpha,
        a.scale.y + (b.scale.y - a.scale.y) * alpha,
        a.scale.z + (b.scale.z - a.scale.z) * alpha,
    };
}

} // namespace

/** 各ボーンのバインドワールド行列を親から合成し、その逆行列を inverse_bind に格納する。 */
void ASkinnedMeshAsset::ComputeInverseBindMatrices() noexcept {
    const u32 n = static_cast<u32>(m_Bones.Num());
    if (n == 0) return;

    // 1) 各ボーンのバインド世界行列を親から順に計算する。
    //    TArray<FBone> は親が i より小さい番号で並んでいる前提（前向き列挙可）。
    TArray<FMat4> world_at_bind;
    world_at_bind.SetNum(n);

    for (u32 i = 0; i < n; ++i) {
        const FBone& b = m_Bones[i];
        const FMat4 local = ComposeTRS(b.bind_translation, b.bind_rotation, b.bind_scale);
        // 親 index は子 (i) より小さい前提。不正な glTF で parent>=i や parent>=n の場合、
        // 未計算/範囲外の world_at_bind[parent] を読むと未初期化値読み or OOB になる。
        // parent が 0..i-1 の範囲にあるときだけ親乗算し、それ以外はローカルを採用する。
        if (b.parent >= 0 && static_cast<u32>(b.parent) < i) {
            world_at_bind[i] = local * world_at_bind[b.parent];
        } else {
            world_at_bind[i] = local;
        }
    }

    // 2) 逆行列 = inverse(bind_world)
    for (u32 i = 0; i < n; ++i) {
        m_Bones[i].inverse_bind = Inverse(world_at_bind[i]);
    }
}

/** 指定アニメーションを先頭から再生開始する (mesh 未設定・範囲外は無視)。 */
void CAnimationPlayer::Play(u32 anim_index, bool loop) noexcept {
    if (!m_Mesh) return;
    if (anim_index >= m_Mesh->Animations().Num()) return;
    m_Anim = static_cast<i32>(anim_index);
    m_bLoop = loop;
    m_Time = 0;
    m_Playing = true;
}

/** 再生時刻を dt 進め、ループ時は wrap、非ループ時は終端でクランプして停止する。 */
void CAnimationPlayer::Update(f32 dt) noexcept {
    if (!m_Playing || !m_Mesh || m_Anim < 0) return;
    if (m_Anim >= static_cast<i32>(m_Mesh->Animations().Num())) return;
    const FAnimation& a = m_Mesh->Animations()[m_Anim];

    if (m_bLoop && a.duration > 0.0f && std::isfinite(a.duration)) {
        // 非有限入力では再生時刻と再生状態を変更しない。
        if (!std::isfinite(m_Time) || !std::isfinite(dt)) return;

        /** wrapに使う正の有限duration。 */
        const f64 duration = static_cast<f64>(a.duration);

        /** f32 の加算overflowを避けるため、wrap前の時刻を倍精度で合成する。 */
        const f64 summed_time = static_cast<f64>(m_Time) + static_cast<f64>(dt);

        /** duration内へ一回で折り返した時刻。 */
        f64 wrapped_time = std::fmod(summed_time, duration);
        if (wrapped_time < 0.0) wrapped_time += duration;
        // 負の0を公開しない。
        if (wrapped_time == 0.0) wrapped_time = 0.0;
        m_Time = static_cast<f32>(wrapped_time);
        return;
    }

    m_Time += dt;
    if (a.duration > 0) {
        if (m_bLoop) {
            // fmod 風（負値ガード付き）
            while (m_Time >= a.duration) m_Time -= a.duration;
            while (m_Time < 0)            m_Time += a.duration;
        } else {
            if (m_Time > a.duration) {
                m_Time = a.duration;
                m_Playing = false;
            }
        }
    }
}

/** 現在時刻の (world * inverse_bind) ボーンパレットを書き込み、書き込んだボーン数を返す。 */
u32 CAnimationPlayer::WritePalette(FMat4* out_palette, u32 max_count) const noexcept {
    if (!m_Mesh || !out_palette) return 0;
    const TArray<FBone>& bones = m_Mesh->Bones();
    const u32 nb = static_cast<u32>(bones.Num());
    const u32 count = nb < max_count ? nb : max_count;
    if (count == 0) return 0;

    // 1) 各ボーンの「アニメーション後ローカル」を求める
    //    アニメ無し（m_Anim == -1）の場合はバインド姿勢の TRS を使う
    static constexpr u32 kStackBones = 256;
    FMat4 local_pose[kStackBones];
    FMat4 world_pose[kStackBones];
    if (count > kStackBones) {
        // ボーン数が多すぎる場合は max_count に丸めて palette に書く
        // パレット計算自体は正しく動く
    }
    const u32 effective = count < kStackBones ? count : kStackBones;

    // 初期値: バインドローカル
    for (u32 i = 0; i < effective; ++i) {
        const FBone& b = bones[i];
        local_pose[i] = ComposeTRS(b.bind_translation, b.bind_rotation, b.bind_scale);
    }

    // アニメーションチャネルで上書き
    if (m_Anim >= 0 && m_Anim < static_cast<i32>(m_Mesh->Animations().Num())) {
        const FAnimation& a = m_Mesh->Animations()[m_Anim];
        for (usize ci = 0; ci < a.channels.Num(); ++ci) {
            const FAnimationChannel& ch = a.channels[ci];
            if (ch.bone_index < 0 || ch.bone_index >= static_cast<i32>(effective)) continue;
            FVec3 t, s; FQuat r;
            SampleChannel(ch, m_Time, t, r, s);
            local_pose[ch.bone_index] = ComposeTRS(t, r, s);
        }
    }

    // 2) ローカル → ワールド（親が小さい index にいる前提）
    for (u32 i = 0; i < effective; ++i) {
        const i32 parent = bones[i].parent;
        if (parent < 0) {
            world_pose[i] = local_pose[i];
        } else if (parent < static_cast<i32>(i)) {
            world_pose[i] = local_pose[i] * world_pose[parent];
        } else {
            // 想定外（親が後ろにある）→ ローカルをそのまま使う
            world_pose[i] = local_pose[i];
        }
    }

    // 3) palette = world * inverse_bind
    for (u32 i = 0; i < effective; ++i) {
        out_palette[i] = bones[i].inverse_bind * world_pose[i];
    }

    // 余り（max_count > kStackBones の場合）は単位行列で埋める
    for (u32 i = effective; i < count; ++i) {
        out_palette[i] = FMat4::Identity();
    }
    return count;
}

} // namespace acs

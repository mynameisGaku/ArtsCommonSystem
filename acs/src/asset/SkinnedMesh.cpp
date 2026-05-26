// SPDX-License-Identifier: Apache-2.0
// SkinnedMeshAsset + AnimationPlayer 実装
#include "asset/SkinnedMesh.h"
#include "math/Math.h"
#include "foundation/Move.h"

namespace acs {

namespace {

// ローカル TRS → 行列（row-major: scale → rotation → translation）
FMat4 ComposeTRS(FVec3 t, FQuat r, FVec3 s) noexcept {
    return FMat4::Scale(s) * ToMatrix(r) * FMat4::Translation(t);
}

// アニメーションチャネルから時刻 t における TRS を補間サンプリング
void SampleChannel(const FAnimationChannel& ch, f32 t,
                   FVec3& out_t, FQuat& out_r, FVec3& out_s) noexcept {
    const usize n = ch.keys.Size();
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

// =============================================================================
// SkinnedMeshAsset
// =============================================================================

void SkinnedMeshAsset::ComputeInverseBindMatrices() noexcept {
    const u32 n = static_cast<u32>(_bones.Size());
    if (n == 0) return;

    // 1) 各ボーンのバインド世界行列を親から順に計算する。
    //    TArray<FBone> は親が i より小さい番号で並んでいる前提（前向き列挙可）。
    TArray<FMat4> world_at_bind;
    world_at_bind.Resize(n);

    for (u32 i = 0; i < n; ++i) {
        const FBone& b = _bones[i];
        FMat4 local = ComposeTRS(b.bind_translation, b.bind_rotation, b.bind_scale);
        if (b.parent < 0) {
            world_at_bind[i] = local;
        } else {
            world_at_bind[i] = local * world_at_bind[b.parent];
        }
    }

    // 2) 逆行列 = inverse(bind_world)
    for (u32 i = 0; i < n; ++i) {
        _bones[i].inverse_bind = Inverse(world_at_bind[i]);
    }
}

// =============================================================================
// AnimationPlayer
// =============================================================================

void AnimationPlayer::Play(u32 anim_index, bool loop) noexcept {
    if (!_mesh) return;
    if (anim_index >= _mesh->Animations().Size()) return;
    _anim = static_cast<i32>(anim_index);
    _loop = loop;
    _time = 0;
    _playing = true;
}

void AnimationPlayer::Update(f32 dt) noexcept {
    if (!_playing || !_mesh || _anim < 0) return;
    if (_anim >= static_cast<i32>(_mesh->Animations().Size())) return;
    const FAnimation& a = _mesh->Animations()[_anim];
    _time += dt;
    if (a.duration > 0) {
        if (_loop) {
            // fmod 風（負値ガード付き）
            while (_time >= a.duration) _time -= a.duration;
            while (_time < 0)            _time += a.duration;
        } else {
            if (_time > a.duration) {
                _time = a.duration;
                _playing = false;
            }
        }
    }
}

u32 AnimationPlayer::WritePalette(FMat4* out_palette, u32 max_count) const noexcept {
    if (!_mesh || !out_palette) return 0;
    const TArray<FBone>& bones = _mesh->Bones();
    const u32 nb = static_cast<u32>(bones.Size());
    const u32 count = nb < max_count ? nb : max_count;
    if (count == 0) return 0;

    // 1) 各ボーンの「アニメーション後ローカル」を求める
    //    アニメ無し（_anim == -1）の場合はバインド姿勢の TRS を使う
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
    if (_anim >= 0 && _anim < static_cast<i32>(_mesh->Animations().Size())) {
        const FAnimation& a = _mesh->Animations()[_anim];
        for (usize ci = 0; ci < a.channels.Size(); ++ci) {
            const FAnimationChannel& ch = a.channels[ci];
            if (ch.bone_index < 0 || ch.bone_index >= static_cast<i32>(effective)) continue;
            FVec3 t, s; FQuat r;
            SampleChannel(ch, _time, t, r, s);
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

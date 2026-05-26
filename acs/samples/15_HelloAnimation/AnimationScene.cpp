// SPDX-License-Identifier: Apache-2.0
// HelloAnimation — AnimationScene 実装。
#include "AnimationScene.h"

#include "math/Math.h"
#include "math/Quat.h"

using namespace acs;

namespace helloanim {

TRc<SkinnedMeshAsset> AnimationScene::BuildSnake() noexcept {
    auto m = MakeRc<SkinnedMeshAsset>();
    if (!m) return TRc<SkinnedMeshAsset>();
    auto& V = m->Vertices();
    auto& I = m->Indices();
    auto& B = m->Bones();
    auto& A = m->Animations();

    // ===== ボーン =====
    B.Resize(kBoneCount);
    for (u32 i = 0; i < kBoneCount; ++i) {
        Bone& b = B[i];
        b.parent = (i == 0) ? -1 : static_cast<i32>(i - 1);
        // 各ボーンは親の +Y 方向に "セグメント長" 進んだ場所
        const f32 seg = kHeight / static_cast<f32>(kBoneCount);
        b.bind_translation = (i == 0) ? FVec3{0, 0, 0} : FVec3{0, seg, 0};
        b.bind_rotation = FQuat{};
        b.bind_scale    = FVec3{1, 1, 1};
    }
    m->ComputeInverseBindMatrices();

    // ===== 頂点 (ring × segment) =====
    V.Reserve(kRingCount * kSegmentCount);
    for (u32 r = 0; r < kRingCount; ++r) {
        const f32 t_y = static_cast<f32>(r) / static_cast<f32>(kRingCount - 1);
        const f32 y   = t_y * kHeight;
        const f32 radius = kRadiusBase + (kRadiusTip - kRadiusBase) * t_y;   // 先端に向け細くなる
        // 頂点の所属ボーンを y 位置から決める (隣接 2 本にブレンド)
        const f32 bone_pos    = t_y * static_cast<f32>(kBoneCount - 1);
        i32 bone_lo = static_cast<i32>(bone_pos);
        if (bone_lo > static_cast<i32>(kBoneCount - 1)) bone_lo = static_cast<i32>(kBoneCount - 1);
        i32 bone_hi = bone_lo + 1;
        if (bone_hi > static_cast<i32>(kBoneCount - 1)) bone_hi = static_cast<i32>(kBoneCount - 1);
        f32 blend = bone_pos - static_cast<f32>(bone_lo);
        if (bone_lo == bone_hi) blend = 0.0f;     // 末端ピンチ

        for (u32 s = 0; s < kSegmentCount; ++s) {
            const f32 ang = (kPi * 2.0f) * static_cast<f32>(s) / static_cast<f32>(kSegmentCount);
            const f32 cx = Cos(ang) * radius;
            const f32 cz = Sin(ang) * radius;

            SkinnedVertex v{};
            v.position = FVec3{cx, y, cz};
            v.normal   = FVec3{Cos(ang), 0, Sin(ang)};   // 簡易法線 (横方向)
            v.u        = static_cast<f32>(s) / static_cast<f32>(kSegmentCount);
            v.v        = t_y;
            v.bones[0] = static_cast<u8>(bone_lo);
            v.bones[1] = static_cast<u8>(bone_hi);
            v.bones[2] = 0;
            v.bones[3] = 0;
            v.weights[0] = 1.0f - blend;
            v.weights[1] = blend;
            v.weights[2] = 0.0f;
            v.weights[3] = 0.0f;
            V.PushBack(v);
        }
    }

    // ===== インデックス (リングごとに四角を 2 三角形) =====
    I.Reserve((kRingCount - 1) * kSegmentCount * 6);
    for (u32 r = 0; r + 1 < kRingCount; ++r) {
        for (u32 s = 0; s < kSegmentCount; ++s) {
            const u32 sn = (s + 1) % kSegmentCount;
            const u32 i00 = r * kSegmentCount + s;
            const u32 i01 = r * kSegmentCount + sn;
            const u32 i10 = (r + 1) * kSegmentCount + s;
            const u32 i11 = (r + 1) * kSegmentCount + sn;
            // 反時計回り (+Y 上、外向き法線)
            I.PushBack(i00); I.PushBack(i10); I.PushBack(i01);
            I.PushBack(i01); I.PushBack(i10); I.PushBack(i11);
        }
    }

    // ===== アニメーション =====
    Animation anim;
    anim.name     = FString("slither");
    anim.duration = kAnimDuration;

    // bone 0 はアニメ無し (ルート固定)。bone 1..3 を sin 波で振らせる。
    // 進行波になるよう各ボーンに位相オフセットを与える。
    for (u32 b = 1; b < kBoneCount; ++b) {
        AnimationChannel ch;
        ch.bone_index = static_cast<i32>(b);
        ch.keys.Reserve(kAnimKeys);
        const f32 phase = static_cast<f32>(b) * 0.9f;
        for (u32 k = 0; k < kAnimKeys; ++k) {
            const f32 time = static_cast<f32>(k) / static_cast<f32>(kAnimKeys - 1) * kAnimDuration;
            const f32 angle = Sin(time * (kPi * 2.0f) / kAnimDuration + phase) * kAnimAngleMax;
            AnimationKey key;
            key.time = time;
            key.translation = FVec3{0, kHeight / kBoneCount, 0};   // バインドと同じ
            key.rotation    = FQuat::AxisAngle(FVec3{0, 0, 1}, angle);
            key.scale       = FVec3{1, 1, 1};
            ch.keys.PushBack(key);
        }
        anim.channels.PushBack(Move(ch));
    }
    A.PushBack(Move(anim));
    return m;
}

void AnimationScene::Render(Sky&                sky,
                            StandardShader&     std_shader,
                            SkinnedShader&      skin_shader,
                            IRhiCommandList&    cl,
                            const Camera&       camera,
                            const GpuMesh&      plane,
                            const SkinnedGpuMesh& snake_gpu,
                            const FMat4*         palette,
                            u32                 palette_n) noexcept {
    // 1. 空
    sky.Render(cl, camera);

    // 2. 地面 (StandardShader)
    DirLight lights[2];
    lights[0].direction = sky.SunDirection();
    lights[0].color     = sky.SunColor();
    lights[1].direction = FVec3{-sky.SunDirection().x,
                                sky.SunDirection().y * 0.5f,
                               -sky.SunDirection().z};
    lights[1].color     = FVec3{0.20f, 0.10f, 0.18f};
    const FVec3 ambient{0.15f, 0.10f, 0.12f};

    std_shader.SetLights(camera.ViewProjection(), camera.Eye(),
                         lights, 2, ambient);
    cl.SetPipeline(*std_shader.Pipeline());
    cl.SetConstantBuffer(0, *std_shader.PerFrameCB());
    cl.SetConstantBuffer(1, *std_shader.PerObjectCB());
    cl.SetTexture(0, *std_shader.DefaultWhiteTexture());
    cl.SetTexture(1, *std_shader.ShadowTextureOrDefault());
    std_shader.SetObject(FMat4::Translation(FVec3{0, 0, 0}),
                         FVec3{0.50f, 0.45f, 0.40f}, 0.05f, 4.0f);
    cl.SetVertexBuffer(*plane.vertex_buffer, plane.vertex_stride);
    cl.SetIndexBuffer(*plane.index_buffer);
    cl.DrawIndexed(plane.index_count);

    // 3. スキンメッシュ (SkinnedShader)
    skin_shader.SetLights(camera.ViewProjection(), camera.Eye(),
                          lights, 2, ambient);

    skin_shader.SetBonePalette(palette, palette_n);

    skin_shader.SetObject(FMat4::Translation(FVec3{0, 0.0f, 0}),
                          FVec3{0.95f, 0.55f, 0.35f}, 0.5f, 32.0f);

    cl.SetPipeline(*skin_shader.Pipeline());
    cl.SetConstantBuffer(0, *skin_shader.PerFrameCB());
    cl.SetConstantBuffer(1, *skin_shader.PerObjectCB());
    cl.SetConstantBuffer(2, *skin_shader.BonesCB());
    cl.SetTexture(0, *skin_shader.DefaultWhiteTexture());
    cl.SetVertexBuffer(*snake_gpu.vertex_buffer, snake_gpu.vertex_stride);
    cl.SetIndexBuffer(*snake_gpu.index_buffer);
    cl.DrawIndexed(snake_gpu.index_count);
}

} // namespace helloanim

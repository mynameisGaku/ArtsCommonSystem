// ACS の HelloAnimation サンプル（手続き生成スキンメッシュ）
//
// 動作:
//   ・4 ボーンの長い円柱を生成（ヘビ風）
//   ・各ボーンの回転を sin 波でアニメーションしてうねうね動く
//   ・地面 + 1 灯ライティング + Sky で雰囲気付け
//   ・矢印キー / ←→ でカメラ周回
//   ・スペースで再生 / 一時停止
//   ・Esc 終了
//
// 学習ポイント:
//   ・SkinnedMeshAsset / Bone / Animation の構築
//   ・SkinnedShader によるスキニング描画
//   ・AnimationPlayer で時刻 → ボーンパレット
//   ・GPU スキニング: VS で 4 ボーンの加重平均

#include "app/Application.h"
#include "app/EntryPoint.h"
#include "app/Sample.h"
#include "platform/Input.h"

#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"
#include "asset/SkinnedMesh.h"

#include "render/SkinnedShader.h"
#include "render/StandardShader.h"
#include "render/RenderAssets.h"
#include "render/Sky.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"

#include "math/Camera.h"
#include "math/Mat.h"
#include "math/Quat.h"
#include "math/Math.h"

#include "memory/UniquePtr.h"
#include "memory/Rc.h"
#include "foundation/Log.h"

#include <cstdio>

using namespace acs;

namespace {

constexpr u32 kBoneCount    = 4;        // ボーン数
constexpr u32 kSegmentCount = 16;       // 円周分割
constexpr u32 kRingCount    = 33;       // 縦方向リング数（多いほど滑らか）
constexpr f32 kHeight       = 4.0f;     // 円柱の全長
constexpr f32 kRadiusBase   = 0.45f;
constexpr f32 kRadiusTip    = 0.18f;
constexpr u32 kAnimKeys     = 33;        // アニメキーフレーム数
constexpr f32 kAnimDuration = 2.0f;
constexpr f32 kAnimAngleMax = 0.45f;

// 4 ボーンを縦に並べる（vertical chain）+ ヘビ風アニメ生成
Rc<SkinnedMeshAsset> BuildSnake() noexcept {
    auto m = MakeRc<SkinnedMeshAsset>();
    if (!m) return Rc<SkinnedMeshAsset>();
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
        b.bind_translation = (i == 0) ? Vec3{0, 0, 0} : Vec3{0, seg, 0};
        b.bind_rotation = Quat{};
        b.bind_scale    = Vec3{1, 1, 1};
    }
    m->ComputeInverseBindMatrices();

    // ===== 頂点（ring × segment）=====
    V.Reserve(kRingCount * kSegmentCount);
    for (u32 r = 0; r < kRingCount; ++r) {
        const f32 t_y = static_cast<f32>(r) / static_cast<f32>(kRingCount - 1);    // 0..1
        const f32 y   = t_y * kHeight;                                              // 0..4
        const f32 radius = kRadiusBase + (kRadiusTip - kRadiusBase) * t_y;          // 細くなる
        // 頂点の所属ボーンを y から決める
        const f32 bone_pos    = t_y * static_cast<f32>(kBoneCount - 1);             // 0..(N-1)
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
            v.position = Vec3{cx, y, cz};
            v.normal   = Vec3{Cos(ang), 0, Sin(ang)};   // 簡易法線（横方向）
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

    // ===== インデックス（リングごとに四角を 2 三角形）=====
    I.Reserve((kRingCount - 1) * kSegmentCount * 6);
    for (u32 r = 0; r + 1 < kRingCount; ++r) {
        for (u32 s = 0; s < kSegmentCount; ++s) {
            const u32 sn = (s + 1) % kSegmentCount;
            const u32 i00 = r * kSegmentCount + s;
            const u32 i01 = r * kSegmentCount + sn;
            const u32 i10 = (r + 1) * kSegmentCount + s;
            const u32 i11 = (r + 1) * kSegmentCount + sn;
            // 反時計回り（+Y 上、外向き法線）
            I.PushBack(i00); I.PushBack(i10); I.PushBack(i01);
            I.PushBack(i01); I.PushBack(i10); I.PushBack(i11);
        }
    }

    // ===== アニメーション =====
    Animation anim;
    anim.name     = String("slither");
    anim.duration = kAnimDuration;

    // bone 0 はアニメ無し（ルート固定）。bone 1..3 を sin 波で振らせる。
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
            key.translation = Vec3{0, kHeight / kBoneCount, 0};   // バインドと同じ
            key.rotation    = Quat::AxisAngle(Vec3{0, 0, 1}, angle);
            key.scale       = Vec3{1, 1, 1};
            ch.keys.PushBack(key);
        }
        anim.channels.PushBack(Move(ch));
    }
    A.PushBack(Move(anim));
    return m;
}

} // namespace

class HelloAnimation : public Application {
public:
    void OnStart() noexcept override {
        IRhiDevice* dev = GetRenderer().Device();
        if (!dev) { Quit(); return; }

        ACS_SAMPLE_INIT(_sky.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat()));
        _sky.PresetSunset();

        ACS_SAMPLE_INIT(_shader.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat()));
        ACS_SAMPLE_INIT(_std_shader.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat()));

        // 地面用プレーン
        auto plane = Primitive::MakePlane(40.0f, 40.0f);
        ACS_SAMPLE_INIT(UploadMesh(*dev, *plane, _gm_plane));

        // スキンメッシュ生成 + GPU アップロード
        _snake = BuildSnake();
        if (!_snake) { Quit(); return; }
        ACS_SAMPLE_INIT(UploadSkinnedMesh(*dev, *_snake, _gm_snake));
        _player.SetMesh(_snake.Get());
        _player.Play(0, /*loop=*/true);

        // 2D HUD
        _batch.Init(*dev, GetRenderer().ColorFormat());
        (void)Sample::TryLoadDefaultUIFont(_font, *dev, 18.0f, 1024, true);

        const f32 aspect = static_cast<f32>(GetRenderer().Swapchain()->Width()) /
                           static_cast<f32>(GetRenderer().Swapchain()->Height());
        _camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 100.0f);

        ACS_LOG_INFO("HelloAnimation initialized: %u verts, %u indices, %u bones",
                     static_cast<u32>(_snake->Vertices().Size()),
                     static_cast<u32>(_snake->Indices().Size()),
                     static_cast<u32>(_snake->Bones().Size()));
    }

    void OnUpdate(f32 dt) noexcept override {
        if (Input::IsKeyPressed(Key::Escape)) Quit();
        if (Input::IsKeyPressed(Key::Space)) {
            if (_player.IsPlaying()) _player.Pause(); else _player.Resume();
        }

        if (Input::IsKeyDown(Key::Left))  _cam_yaw -= dt * 1.0f;
        if (Input::IsKeyDown(Key::Right)) _cam_yaw += dt * 1.0f;
        const f32 cam_dist = 8.0f;
        _cam_pos = Vec3{ Sin(_cam_yaw) * cam_dist, 3.0f, -Cos(_cam_yaw) * cam_dist };
        _camera.SetLookAt(_cam_pos, Vec3{0, 2, 0});

        _player.Update(dt);
    }

    void OnRender() noexcept override {
        IRhiCommandList* cl = GetRenderer().CommandList();
        if (!cl) return;

        // 1. 空
        _sky.Render(*cl, _camera);

        // 2. 地面（StandardShader）
        DirLight lights[2];
        lights[0].direction = _sky.SunDirection();
        lights[0].color     = _sky.SunColor();
        lights[1].direction = Vec3{-_sky.SunDirection().x, _sky.SunDirection().y * 0.5f, -_sky.SunDirection().z};
        lights[1].color     = Vec3{0.20f, 0.10f, 0.18f};
        Vec3 ambient{0.15f, 0.10f, 0.12f};

        _std_shader.SetLights(_camera.ViewProjection(), _camera.Eye(),
                              lights, 2, ambient);
        cl->SetPipeline(*_std_shader.Pipeline());
        cl->SetConstantBuffer(0, *_std_shader.PerFrameCB());
        cl->SetConstantBuffer(1, *_std_shader.PerObjectCB());
        cl->SetTexture(0, *_std_shader.DefaultWhiteTexture());
        cl->SetTexture(1, *_std_shader.ShadowTextureOrDefault());
        _std_shader.SetObject(Mat4::Translation(Vec3{0, 0, 0}),
                              Vec3{0.50f, 0.45f, 0.40f}, 0.05f, 4.0f);
        cl->SetVertexBuffer(*_gm_plane.vertex_buffer, _gm_plane.vertex_stride);
        cl->SetIndexBuffer(*_gm_plane.index_buffer);
        cl->DrawIndexed(_gm_plane.index_count);

        // 3. スキンメッシュ（SkinnedShader）
        _shader.SetLights(_camera.ViewProjection(), _camera.Eye(),
                          lights, 2, ambient);

        Mat4 palette[SkinnedShader::kMaxBones];
        const u32 nb = _player.WritePalette(palette, SkinnedShader::kMaxBones);
        _shader.SetBonePalette(palette, nb);

        _shader.SetObject(Mat4::Translation(Vec3{0, 0.0f, 0}),
                          Vec3{0.95f, 0.55f, 0.35f}, 0.5f, 32.0f);

        cl->SetPipeline(*_shader.Pipeline());
        cl->SetConstantBuffer(0, *_shader.PerFrameCB());
        cl->SetConstantBuffer(1, *_shader.PerObjectCB());
        cl->SetConstantBuffer(2, *_shader.BonesCB());
        cl->SetTexture(0, *_shader.DefaultWhiteTexture());
        cl->SetVertexBuffer(*_gm_snake.vertex_buffer, _gm_snake.vertex_stride);
        cl->SetIndexBuffer(*_gm_snake.index_buffer);
        cl->DrawIndexed(_gm_snake.index_count);

        // 4. HUD
        if (_font.AtlasTexture()) {
            const u32 sw = GetRenderer().Swapchain()->Width();
            const u32 sh = GetRenderer().Swapchain()->Height();
            _batch.Begin(*cl, sw, sh);
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "ボーン: %u  時刻: %.2fs  状態: %s",
                          nb, static_cast<double>(_player.Time()),
                          _player.IsPlaying() ? "再生中" : "停止");
            _batch.DrawString(_font, buf, 20, 20, Vec4{1,1,1,1});
            _batch.DrawString(_font, "Space: 再生/停止  ←→: カメラ  Esc: 終了",
                            20, 44, Vec4{0.8f,0.85f,0.95f,1});
            _batch.End();
        }
    }

    void OnShutdown() noexcept override {
        if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
        _font.Shutdown();
        _batch.Shutdown();
        _gm_snake = SkinnedGpuMesh{};
        _gm_plane = GpuMesh{};
        _snake = Rc<SkinnedMeshAsset>();
        _std_shader.Shutdown();
        _shader.Shutdown();
        _sky.Shutdown();
    }

private:
    Sky               _sky;
    SkinnedShader     _shader;
    StandardShader    _std_shader;
    SpriteBatch       _batch;
    Font              _font;

    Rc<SkinnedMeshAsset> _snake;
    SkinnedGpuMesh       _gm_snake;
    GpuMesh              _gm_plane;
    AnimationPlayer      _player;

    Camera _camera;
    Vec3   _cam_pos;
    f32    _cam_yaw = 0.6f;
};

ACS_DEFINE_MAIN(HelloAnimation)

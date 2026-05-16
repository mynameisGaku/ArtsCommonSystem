// ACS の HelloLightmap サンプル — Phase 33f-2: 静的ライトマップ baker
//
// 動作:
//   ・古典的な Cornell box (床 / 天井 / 奥壁 / 左壁(赤) / 右壁(緑)) を構築
//   ・天井を発光面とみなし、各面の lightmap テクセルへ CPU で 1-bounce GI を
//     ベイクする (hemisphere ray cast + 軸並行平面との交差判定)
//   ・ベイク結果を R8G8B8A8 テクスチャ化し、PbrShader の lightmap slot (t8、
//     Phase 33f で追加) 経由で表示する
//   ・WASD でカメラ移動、矢印で視点回転、Esc 終了
//
// 学習ポイント:
//   ・動的ライティング無しでも、事前計算した間接光で「赤/緑の壁の照り返しが
//     床に色づく」color bleeding が表現できる
//   ・lightmap は mesh の uv で引く (このサンプルは 1 面 = 1 テクスチャ)
#include "app/Application.h"
#include "app/EntryPoint.h"
#include "app/Sample.h"
#include "platform/Input.h"

#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"

#include "render/PbrShader.h"
#include "render/RenderAssets.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"

#include "container/Array.h"
#include "math/Camera.h"
#include "math/Mat.h"
#include "math/Vec.h"
#include "math/Math.h"

#include "memory/UniquePtr.h"
#include "foundation/Log.h"

#include <cstdio>

using namespace acs;

namespace {

// Cornell box の寸法: x∈[-1,1], y∈[0,2], z∈[-1,2] (手前 z=-1 は開口)
constexpr f32 kBoxMinX = -1.0f, kBoxMaxX = 1.0f;
constexpr f32 kBoxMinY =  0.0f, kBoxMaxY = 2.0f;
constexpr f32 kBoxMinZ = -1.0f, kBoxMaxZ = 2.0f;

constexpr u32 kLmSize = 64;        // lightmap 解像度 (1 面あたり)
constexpr u32 kBakeRays = 24;      // texel あたりの hemisphere ray 数

// Cornell box の 1 面。MakePlane (XZ 平面、法線 +Y) を model で配置する。
//   axis / axis_value / u_min.. : ray cast 用の world-space 軸並行平面パラメータ
//   plane_w / plane_h           : MakePlane に渡したローカルサイズ。baker が
//                                 texel uv → ローカル座標 → model → world と
//                                 変換するのに使う (回転を model に委ねる)。
struct Quad {
    GpuMesh                mesh;
    Mat4                   model;
    Vec3                   albedo;
    Vec3                   normal;        // world-space 法線 (model から導出)
    f32                    plane_w, plane_h;  // MakePlane のローカルサイズ
    UniquePtr<IRhiTexture> lightmap;
    Array<u8>              lm_data;        // bake 結果 (RGBA8)
    // 面が乗る軸 (0=x, 1=y, 2=z) と、その軸の値。残り 2 軸が矩形範囲。
    i32                    axis;
    f32                    axis_value;
    f32                    u_min, u_max;   // 矩形範囲 (axis 以外の 1 軸目)
    f32                    v_min, v_max;   // 矩形範囲 (axis 以外の 2 軸目)
    bool                   emissive;       // 天井 = 光源
};

// 軸並行平面 axis(=value) 上の矩形 [u_min,u_max]x[v_min,v_max] と
// ray (o,d) の交差。hit したら t を返す (なければ -1)。
f32 RayQuad(Vec3 o, Vec3 d, const Quad& q) noexcept {
    f32 od, dd;
    if (q.axis == 0)      { od = o.x; dd = d.x; }
    else if (q.axis == 1) { od = o.y; dd = d.y; }
    else                  { od = o.z; dd = d.z; }
    if (dd > -1e-6f && dd < 1e-6f) return -1.0f;     // 平面と平行
    const f32 t = (q.axis_value - od) / dd;
    if (t < 1e-3f) return -1.0f;                     // 後方 or 自己交差
    const Vec3 p{o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
    // axis 以外の 2 軸を取り出す
    f32 pu, pv;
    if (q.axis == 0)      { pu = p.y; pv = p.z; }
    else if (q.axis == 1) { pu = p.x; pv = p.z; }
    else                  { pu = p.x; pv = p.y; }
    if (pu < q.u_min || pu > q.u_max || pv < q.v_min || pv > q.v_max) return -1.0f;
    return t;
}

} // namespace

class HelloLightmap : public Application {
public:
    void OnStart() noexcept override {
        IRhiDevice* dev = GetRenderer().Device();
        if (!dev) { Quit(); return; }

        ACS_SAMPLE_INIT(_pbr.Init(*dev, GetRenderer().ColorFormat(),
                                  GetRenderer().DepthFormat()));

        BuildCornellBox(*dev);
        BakeLightmaps(*dev);

        _batch.Init(*dev, GetRenderer().ColorFormat());
        (void)Sample::TryLoadDefaultUIFont(_font, *dev, 18.0f, 1024, true);

        const f32 aspect = static_cast<f32>(GetRenderer().Swapchain()->Width()) /
                           static_cast<f32>(GetRenderer().Swapchain()->Height());
        _camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.05f, 100.0f);
        _cam_pos = Vec3{0.0f, 1.0f, -0.9f};
    }

    void OnUpdate(f32 dt) noexcept override {
        if (Input::IsKeyPressed(Key::Escape)) Quit();
        if (Input::IsKeyPressed(Key::L)) _show_lightmap = !_show_lightmap;

        const f32 mv = 2.0f * dt, tr = 1.4f * dt;
        if (Input::IsKeyDown(Key::Left))  _cam_yaw -= tr;
        if (Input::IsKeyDown(Key::Right)) _cam_yaw += tr;
        if (Input::IsKeyDown(Key::Up))    _cam_pitch -= tr * 0.8f;
        if (Input::IsKeyDown(Key::Down))  _cam_pitch += tr * 0.8f;
        const f32 limit = 0.45f * kPi;
        if (_cam_pitch >  limit) _cam_pitch =  limit;
        if (_cam_pitch < -limit) _cam_pitch = -limit;
        Vec3 forward{ Sin(_cam_yaw) * Cos(_cam_pitch),
                     -Sin(_cam_pitch),
                      Cos(_cam_yaw) * Cos(_cam_pitch) };
        Vec3 right{ Cos(_cam_yaw), 0, -Sin(_cam_yaw) };
        if (Input::IsKeyDown(Key::W)) _cam_pos += forward * mv;
        if (Input::IsKeyDown(Key::S)) _cam_pos -= forward * mv;
        if (Input::IsKeyDown(Key::D)) _cam_pos += right   * mv;
        if (Input::IsKeyDown(Key::A)) _cam_pos -= right   * mv;
        _camera.SetLookAt(_cam_pos, _cam_pos + forward);
    }

    void OnRender() noexcept override {
        IRhiCommandList* cl = GetRenderer().CommandList();
        if (!cl) return;

        // 動的ライトは使わず ambient のみ。間接光は lightmap が担う。
        // lightmap は ambient/IBL 項に加算合成されるので、ここでの ambient は
        // ごく弱くしておく (真っ黒を避ける程度)。
        _pbr.SetLights(_camera.ViewProjection(), _camera.Eye(),
                       nullptr, 0, Vec3{0.02f, 0.02f, 0.02f});
        _pbr.SetPointLights(nullptr, 0);

        cl->SetPipeline(*_pbr.Pipeline());
        cl->SetConstantBuffer(0, *_pbr.PerFrameCB());
        cl->SetConstantBuffer(1, *_pbr.PerObjectCB());
        cl->SetTexture(0, *_pbr.DefaultWhiteTexture());

        for (u32 i = 0; i < kQuadCount; ++i) {
            Quad& q = _quads[i];
            // lightmap を slot 8 に bind (L キーで OFF にすると flat ambient のみ)
            if (_show_lightmap && q.lightmap) {
                _pbr.SetLightmap(q.lightmap.Get(), 1.0f);
            } else {
                _pbr.SetLightmap(nullptr, 0.0f);
            }
            _pbr.SetObject(q.model, q.albedo, /*metallic=*/0.0f,
                           /*roughness=*/0.9f, /*ao=*/1.0f);
            _pbr.BindIblTextures(*cl);
            cl->SetVertexBuffer(*q.mesh.vertex_buffer, q.mesh.vertex_stride);
            cl->SetIndexBuffer(*q.mesh.index_buffer);
            cl->DrawIndexed(q.mesh.index_count);
        }

        if (_font.AtlasTexture()) {
            const u32 sw = GetRenderer().Swapchain()->Width();
            const u32 sh = GetRenderer().Swapchain()->Height();
            _batch.Begin(*cl, sw, sh);
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "Cornell box - baked lightmap (%u rays/texel)  FPS: %.1f",
                          kBakeRays, static_cast<double>(FPS()));
            _batch.DrawString(_font, buf, 20, 20, Vec4{1, 1, 1, 1});
            std::snprintf(buf, sizeof(buf), "Lightmap: %s   (L で切替)",
                          _show_lightmap ? "ON" : "OFF");
            _batch.DrawString(_font, buf, 20, 44, Vec4{1.0f, 0.95f, 0.7f, 1});
            _batch.DrawString(_font, "WASD: 移動   矢印: 視点   Esc: 終了",
                              20, 68, Vec4{0.7f, 0.85f, 1.0f, 1});
            _batch.End();
        }
    }

    void OnShutdown() noexcept override {
        if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
        _font.Shutdown();
        _batch.Shutdown();
        for (u32 i = 0; i < kQuadCount; ++i) {
            _quads[i].lightmap.Reset();
            _quads[i].mesh = GpuMesh{};
        }
        _pbr.Shutdown();
    }

private:
    static constexpr u32 kQuadCount = 5;

    // 1 面を初期化 (mesh upload + 平面パラメータ設定)。
    // world 法線は手書きせず、ローカル +Y を model で変換して導出する。
    void InitQuad(IRhiDevice& dev, Quad& q, f32 w, f32 h, const Mat4& model,
                  Vec3 albedo, i32 axis, f32 axis_value,
                  f32 u_min, f32 u_max, f32 v_min, f32 v_max,
                  bool emissive) noexcept {
        auto plane = Primitive::MakePlane(w, h);
        (void)UploadMesh(dev, *plane, q.mesh);
        q.model      = model;
        q.albedo     = albedo;
        q.plane_w    = w;
        q.plane_h    = h;
        q.normal     = Normalize(TransformVector(Vec3{0, 1, 0}, model));
        q.axis       = axis;
        q.axis_value = axis_value;
        q.u_min = u_min; q.u_max = u_max;
        q.v_min = v_min; q.v_max = v_max;
        q.emissive   = emissive;
    }

    void BuildCornellBox(IRhiDevice& dev) noexcept {
        const Vec3 white{0.72f, 0.72f, 0.72f};
        const Vec3 red  {0.65f, 0.10f, 0.10f};
        const Vec3 green{0.10f, 0.55f, 0.12f};

        // model は Rotation * Translation の順 (ACS の row-major では「先に
        // 回転、次に平行移動」= ローカルで回転してから world 位置へ移動)。
        // MakePlane(w,h) は XZ 平面: ローカル u→+X, v→-Z。回転後の world 軸への
        // 写像を考慮して w/h を割り当てる (例: 壁は回転で v→Z, u→Y になる)。

        // 床: y=0、法線 +Y。回転なし。u→X(幅2), v→Z(奥行3)。
        InitQuad(dev, _quads[0], 2.0f, 3.0f,
                 Mat4::Translation(Vec3{0.0f, kBoxMinY, 0.5f}),
                 white, /*axis(y)=*/1, kBoxMinY,
                 kBoxMinX, kBoxMaxX, kBoxMinZ, kBoxMaxZ, false);
        // 天井: y=2、法線 -Y。X 軸 π 回転で裏返す。emissive = 光源。u→X, v→Z。
        InitQuad(dev, _quads[1], 2.0f, 3.0f,
                 Mat4::RotationX(kPi) * Mat4::Translation(Vec3{0.0f, kBoxMaxY, 0.5f}),
                 white, /*axis(y)=*/1, kBoxMaxY,
                 kBoxMinX, kBoxMaxX, kBoxMinZ, kBoxMaxZ, true);
        // 奥壁: z=2、法線 -Z。X 軸 -π/2 回転。u→X(幅2), v→Y(高さ2)。
        InitQuad(dev, _quads[2], 2.0f, 2.0f,
                 Mat4::RotationX(-kPi * 0.5f) * Mat4::Translation(Vec3{0.0f, 1.0f, kBoxMaxZ}),
                 white, /*axis(z)=*/2, kBoxMaxZ,
                 kBoxMinX, kBoxMaxX, kBoxMinY, kBoxMaxY, false);
        // 左壁: x=-1、法線 +X、赤。Z 軸 -π/2 回転。u→Y(高さ2), v→Z(奥行3)。
        InitQuad(dev, _quads[3], 2.0f, 3.0f,
                 Mat4::RotationZ(-kPi * 0.5f) * Mat4::Translation(Vec3{kBoxMinX, 1.0f, 0.5f}),
                 red, /*axis(x)=*/0, kBoxMinX,
                 kBoxMinY, kBoxMaxY, kBoxMinZ, kBoxMaxZ, false);
        // 右壁: x=1、法線 -X、緑。Z 軸 +π/2 回転。u→Y(高さ2), v→Z(奥行3)。
        InitQuad(dev, _quads[4], 2.0f, 3.0f,
                 Mat4::RotationZ(kPi * 0.5f) * Mat4::Translation(Vec3{kBoxMaxX, 1.0f, 0.5f}),
                 green, /*axis(x)=*/0, kBoxMaxX,
                 kBoxMinY, kBoxMaxY, kBoxMinZ, kBoxMaxZ, false);
    }

    // 面 q のテクセル (tu,tv)∈[0,1] を world 座標へ。
    // MakePlane の uv 規約 (u→ローカル +X, v→ローカル -Z) でローカル平面座標に
    // 戻し、quad の model 行列で world へ変換する。これで描画時に PbrShader が
    // mesh の uv で lightmap を引く位置と、baker が焼いた位置が厳密に一致する
    // (回転・平行移動は model に委ねるので軸の取り違えが起きない)。
    Vec3 TexelToWorld(const Quad& q, f32 tu, f32 tv) const noexcept {
        const f32 lx = (tu - 0.5f) * q.plane_w;
        const f32 lz = (0.5f - tv) * q.plane_h;
        return TransformPoint(Vec3{lx, 0.0f, lz}, q.model);
    }

    // hemisphere ray cast で 1-bounce GI を焼く
    void BakeLightmaps(IRhiDevice& dev) noexcept {
        // 天井光源の放射色 (HDR 的に強め)
        const Vec3 kLight{3.2f, 3.0f, 2.7f};

        for (u32 qi = 0; qi < kQuadCount; ++qi) {
            Quad& q = _quads[qi];
            q.lm_data.Resize(static_cast<usize>(kLmSize) * kLmSize * 4u);

            // 面ローカルの tangent / bitangent (法線に直交する 2 軸)
            Vec3 N = q.normal;
            Vec3 T = (Abs(N.y) > 0.9f) ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
            T = Normalize(T - N * Dot(T, N));
            Vec3 B = Cross(N, T);

            for (u32 ty = 0; ty < kLmSize; ++ty) {
                for (u32 tx = 0; tx < kLmSize; ++tx) {
                    const f32 tu = (static_cast<f32>(tx) + 0.5f) / kLmSize;
                    const f32 tv = (static_cast<f32>(ty) + 0.5f) / kLmSize;
                    const Vec3 wp = TexelToWorld(q, tu, tv);
                    const Vec3 origin = wp + N * 0.01f;     // self-hit 回避

                    Vec3 accum{0, 0, 0};
                    for (u32 r = 0; r < kBakeRays; ++r) {
                        // hemisphere の準乱数方向 (cosine-weighted 近似)
                        const f32 a = (static_cast<f32>(r) + 0.5f) / kBakeRays;
                        const f32 phi = a * 6.2831853f * 7.0f;          // 螺旋
                        const f32 cz  = Sqrt(1.0f - a);                  // cosine 寄り
                        const f32 sr  = Sqrt(1.0f - cz * cz);
                        const Vec3 local{sr * Cos(phi), sr * Sin(phi), cz};
                        const Vec3 dir = Normalize(T * local.x + B * local.y + N * local.z);

                        // Cornell box の全 5 面と交差、最近を採用
                        f32 best_t = 1e9f;
                        i32 best_q = -1;
                        for (u32 hj = 0; hj < kQuadCount; ++hj) {
                            if (hj == qi) continue;
                            const f32 t = RayQuad(origin, dir, _quads[hj]);
                            if (t > 0.0f && t < best_t) { best_t = t; best_q = static_cast<i32>(hj); }
                        }
                        if (best_q < 0) continue;            // 開口へ抜けた
                        const Quad& hitq = _quads[best_q];
                        if (hitq.emissive) {
                            // 直接光: 天井光源にレイが到達
                            accum += kLight;
                        } else {
                            // 1-bounce: 当たった壁の albedo を弱い反射光として
                            // (壁自体も天井光を受けている、という近似で固定係数)
                            accum += hitq.albedo * 0.55f;
                        }
                    }
                    accum = accum * (1.0f / static_cast<f32>(kBakeRays));

                    const usize idx = (static_cast<usize>(ty) * kLmSize + tx) * 4u;
                    q.lm_data[idx + 0] = ToU8(accum.x);
                    q.lm_data[idx + 1] = ToU8(accum.y);
                    q.lm_data[idx + 2] = ToU8(accum.z);
                    q.lm_data[idx + 3] = 255;
                }
            }

            TextureDesc td{};
            td.width  = kLmSize;
            td.height = kLmSize;
            td.format = Format::R8G8B8A8_UNorm;
            td.initial_data      = q.lm_data.Data();
            td.initial_data_size = q.lm_data.Size();
            auto r = CreateRhiTexture(dev, td);
            if (r.IsErr()) {
                ACS_LOG_ERROR("HelloLightmap: lightmap texture 生成失敗 (quad %u)", qi);
            } else {
                q.lightmap = Move(r.Value());
            }
        }
    }

    static u8 ToU8(f32 v) noexcept {
        const f32 c = Saturate(v);
        return static_cast<u8>(c * 255.0f + 0.5f);
    }

    PbrShader   _pbr;
    Quad        _quads[kQuadCount];
    SpriteBatch _batch;
    Font        _font;
    Camera      _camera;
    Vec3        _cam_pos{0, 1.0f, -0.9f};
    f32         _cam_yaw   = 0.0f;
    f32         _cam_pitch = 0.0f;
    bool        _show_lightmap = true;
};

ACS_DEFINE_MAIN(HelloLightmap)

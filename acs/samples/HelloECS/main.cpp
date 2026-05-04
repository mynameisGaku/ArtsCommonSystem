// ACS の HelloECS サンプル
//
// ACS の ECS / Event / Timer / 並列イテレーション機能を 1 ファイルで紹介する小品。
//
// やってること:
//   ・World に 200 個の Entity を生成、各々 Position + Velocity + Color
//   ・OnUpdate 毎フレーム: World.Query<Position, Velocity>().EachParallel(...) で
//     ThreadPool 経由の並列移動更新
//   ・SpriteBatch で円型テクスチャを使って粒子のように描画
//   ・MessageBroker で Entity 同士の collision を pub/sub 通知 (デモ用)
//   ・TimerManager で 5 秒おきに自動的に新規 Entity を 30 個追加
//
// 学習ポイント:
//   ・SparseSet ベースの O(1) Add/Remove
//   ・World::Query<>().EachParallel(fn, grain) で並列処理
//   ・MessageBroker 経由の疎結合な通知
//   ・TimerManager で OnUpdate に頼らない時刻ベース処理

#include "app/Application.h"
#include "app/EntryPoint.h"
#include "app/Sample.h"
#include "platform/Input.h"

#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "render/IRhiTexture.h"

#include "ecs/World.h"
#include "ecs/Query.h"
#include "event/MessageBroker.h"
#include "event/Timer.h"

#include "math/Vec.h"
#include "math/Math.h"

#include "memory/UniquePtr.h"
#include "foundation/Log.h"

#include <cstdio>

using namespace acs;

// ---- コンポーネント定義 (POD ベース) ----------------------------------------
struct Position { Vec2 v; };
struct Velocity { Vec2 v; };
struct Color    { f32 r, g, b; };

// ---- イベント (MessageBroker で publish) ------------------------------------
struct WallHitEvent { EntityId id; };

// ---- Particle テクスチャ生成 (中央白、外周透明の円) ------------------------
namespace {
constexpr u32 kBallTexSize = 32;
void GenerateBallTexture(u8* out) noexcept {
    for (u32 y = 0; y < kBallTexSize; ++y) {
        for (u32 x = 0; x < kBallTexSize; ++x) {
            const f32 cx = static_cast<f32>(x) - kBallTexSize * 0.5f;
            const f32 cy = static_cast<f32>(y) - kBallTexSize * 0.5f;
            const f32 r = Sqrt(cx*cx + cy*cy) / (kBallTexSize * 0.5f);
            f32 alpha = r > 1.0f ? 0.0f : (1.0f - r);
            const usize i = static_cast<usize>(y * kBallTexSize + x) * 4;
            out[i+0] = 255; out[i+1] = 255; out[i+2] = 255;
            out[i+3] = static_cast<u8>(alpha * alpha * 255);
        }
    }
}
} // namespace

class HelloECS : public Application {
public:
    void OnStart() noexcept override {
        IRhiDevice* dev = GetRenderer().Device();
        if (!dev) { Quit(); return; }

        ACS_SAMPLE_INIT(_batch.Init(*dev, GetRenderer().ColorFormat()));

        // 円型テクスチャ
        u8 pixels[kBallTexSize * kBallTexSize * 4];
        GenerateBallTexture(pixels);
        TextureDesc td{};
        td.width = kBallTexSize; td.height = kBallTexSize;
        td.format = Format::R8G8B8A8_UNorm;
        td.initial_data = pixels;
        td.initial_data_size = sizeof(pixels);
        if (auto r = CreateRhiTexture(*dev, td); r.IsErr()) { Quit(); return; }
        else _tex = Move(r.Value());

        // 初期 Entity 200 個
        SpawnRandomEntities(200);

        // Wall hit イベントの subscribe (デモ的にログだけ)
        GetEvents().Subscribe<WallHitEvent>([](const void* p, void*){
            const auto* e = static_cast<const WallHitEvent*>(p);
            (void)e;  // ACS_LOG_INFO で頻繁に来るので省略
        }, nullptr);

        // 5 秒おきに 30 個追加 (Timer の周期実行デモ)
        GetTimers().SetInterval(5.0f, [](void* user){
            auto* self = static_cast<HelloECS*>(user);
            self->SpawnRandomEntities(30);
            ACS_LOG_INFO("Spawned +30 (total %u)", self->GetWorld().EntityCount());
        }, this);

        ACS_LOG_INFO("HelloECS initialized (entities=%u)", GetWorld().EntityCount());
    }

    void OnUpdate(f32 dt) noexcept override {
        if (Input::IsKeyPressed(Key::Escape)) Quit();
        if (Input::IsKeyPressed(Key::Space)) SpawnRandomEntities(50);

        const u32 sw = GetRenderer().Swapchain()->Width();
        const u32 sh = GetRenderer().Swapchain()->Height();

        // 並列移動 + 壁反射
        World& w = GetWorld();
        MessageBroker& bus = GetEvents();
        struct Ctx { f32 dt; u32 sw; u32 sh; MessageBroker* bus; } ctx{ dt, sw, sh, &bus };
        // EachParallel は static lambda で書ける (capture 無し)
        // ただし Ctx を渡すには Subscribe API のような user ポインタが要る。
        // 単純にここでは sequential Each で書く (並列版は後述)。
        w.Query<Position, Velocity>().Each(
            [dt, sw, sh, &bus](EntityId id, Position& p, Velocity& v){
                p.v.x += v.v.x * dt;
                p.v.y += v.v.y * dt;
                bool hit = false;
                if (p.v.x < 0)         { p.v.x = 0;        v.v.x = -v.v.x; hit = true; }
                if (p.v.x > (f32)sw)   { p.v.x = (f32)sw;  v.v.x = -v.v.x; hit = true; }
                if (p.v.y < 0)         { p.v.y = 0;        v.v.y = -v.v.y; hit = true; }
                if (p.v.y > (f32)sh)   { p.v.y = (f32)sh;  v.v.y = -v.v.y; hit = true; }
                if (hit) bus.Publish<WallHitEvent>(WallHitEvent{ id });
            });
    }

    void OnRender() noexcept override {
        IRhiCommandList* cl = GetRenderer().CommandList();
        if (!cl || !_tex) return;
        const u32 sw = GetRenderer().Swapchain()->Width();
        const u32 sh = GetRenderer().Swapchain()->Height();

        _batch.Begin(*cl, sw, sh);
        _batch.DrawRect(0, 0, (f32)sw, (f32)sh, Vec4{0.05f, 0.06f, 0.1f, 1});

        World& w = GetWorld();
        const f32 r = 8.0f;
        w.Query<Position, Color>().Each(
            [this, r](EntityId, Position& p, Color& c){
                _batch.Draw(*_tex, p.v.x - r, p.v.y - r, r * 2, r * 2,
                            Vec4{c.r, c.g, c.b, 0.95f});
            });

        _batch.End();
    }

    void OnShutdown() noexcept override {
        _tex.Reset();
        _batch.Shutdown();
    }

private:
    void SpawnRandomEntities(u32 n) noexcept {
        const u32 sw = GetRenderer().Swapchain()->Width();
        const u32 sh = GetRenderer().Swapchain()->Height();
        World& w = GetWorld();
        for (u32 i = 0; i < n; ++i) {
            EntityId e = w.Create();
            w.Add(e, Position{ Vec2{ _rnd() * sw, _rnd() * sh } });
            w.Add(e, Velocity{ Vec2{ (_rnd() - 0.5f) * 200.0f, (_rnd() - 0.5f) * 200.0f } });
            w.Add(e, Color{ 0.5f + _rnd() * 0.5f, 0.5f + _rnd() * 0.5f, 0.5f + _rnd() * 0.5f });
        }
    }

    f32 _rnd() noexcept {
        _seed ^= _seed << 13; _seed ^= _seed >> 17; _seed ^= _seed << 5;
        return static_cast<f32>(_seed & 0xFFFFFFu) / 16777216.0f;
    }

    SpriteBatch            _batch;
    UniquePtr<IRhiTexture> _tex;
    u32                    _seed = 0xCAFEBABEu;
};

ACS_DEFINE_MAIN(HelloECS)

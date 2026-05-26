// SPDX-License-Identifier: Apache-2.0
// HelloECS — Application 実装。
//
// 学習ポイント:
//   ・SparseSet ベースの O(1) Add/Remove
//   ・World::Query<>().EachParallel(fn, grain) で並列処理。fn は Entity ごとに
//     別スレッドで独立に呼ばれるので、共有資源には触らないこと
//   ・MessageBroker はシングルスレッド前提の同期 pub/sub。よって publish は
//     並列ループの外（メインスレッド）から行う
//   ・TimerManager で OnUpdate に頼らない時刻ベース処理
#include "HelloECSApp.h"
#include "Types.h"

#include "app/Sample.h"
#include "platform/Input.h"

#include "render/Font.h"

#include "ecs/World.h"
#include "ecs/Query.h"
#include "event/MessageBroker.h"
#include "event/Timer.h"

#include "math/Vec.h"
#include "math/Math.h"

#include "foundation/Log.h"

using namespace acs;

namespace hello04 {

void HelloECSApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    ACS_SAMPLE_INIT(_batch.Init(*dev, GetRenderer().ColorFormat()));

    // 円型テクスチャ
    u8 pixels[kBallTexSize * kBallTexSize * 4];
    GenerateBallTexture(pixels);
    TextureDesc td{};
    td.width = kBallTexSize; td.height = kBallTexSize;
    td.format = EFormat::R8G8B8A8_UNorm;
    td.initial_data = pixels;
    td.initial_data_size = sizeof(pixels);
    if (auto r = CreateRhiTexture(*dev, td); r.IsErr()) {
        ACS_LOG_ERROR("ball texture: %s", r.Error().message);
        Quit(); return;
    } else {
        _tex = Move(r.Value());
    }

    // 初期 Entity 200 個
    SpawnRandomEntities(200);

    // SpawnEvent を購読 (Entity が追加されたらログに出す)
    GetEvents().Subscribe<SpawnEvent>(&OnSpawnEvent, nullptr);

    // 5 秒おきに 30 個追加 (Timer の周期実行デモ)。
    // Timer コールバックはメインスレッドで呼ばれるので、ここから
    // MessageBroker に publish しても安全。
    GetTimers().SetInterval(5.0f, [](void* user){
        auto* self = static_cast<HelloECSApp*>(user);
        self->SpawnRandomEntities(30);
        self->GetEvents().Publish<SpawnEvent>(
            SpawnEvent{ self->GetWorld().EntityCount() });
    }, this);

    ACS_LOG_INFO("HelloECS initialized (entities=%u)", GetWorld().EntityCount());
}

void HelloECSApp::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) Quit();
    if (Input::IsKeyPressed(EKey::Space)) {
        SpawnRandomEntities(50);
        // publish はメインスレッドの OnUpdate から。並列ループの外なので安全。
        GetEvents().Publish<SpawnEvent>(SpawnEvent{ GetWorld().EntityCount() });
    }

    const f32 sw = static_cast<f32>(GetRenderer().Swapchain()->Width());
    const f32 sh = static_cast<f32>(GetRenderer().Swapchain()->Height());

    // 並列移動 + 壁反射。EachParallel は Entity ごとに fn を別スレッドで呼ぶ。
    // 各 Entity の Position / Velocity は他と独立なので並列化して安全
    // (共有資源には触らない — イベント発行はここでは行わない)。
    // grain を小さめにして実際に複数スレッドへ分散させる
    // (エンティティ数が少ないデモのため。実プロジェクトでは 1024 程度が目安)。
    GetWorld().Query<Position, Velocity>().EachParallel(
        [dt, sw, sh](EntityId, Position& p, Velocity& v){
            p.v.x += v.v.x * dt;
            p.v.y += v.v.y * dt;
            if (p.v.x < 0)  { p.v.x = 0;  v.v.x = -v.v.x; }
            if (p.v.x > sw) { p.v.x = sw; v.v.x = -v.v.x; }
            if (p.v.y < 0)  { p.v.y = 0;  v.v.y = -v.v.y; }
            if (p.v.y > sh) { p.v.y = sh; v.v.y = -v.v.y; }
        }, 64);
}

void HelloECSApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl || !_tex) return;
    const u32 sw = GetRenderer().Swapchain()->Width();
    const u32 sh = GetRenderer().Swapchain()->Height();

    _batch.Begin(*cl, sw, sh);
    _batch.DrawRect(0, 0, (f32)sw, (f32)sh, Vec4{0.05f, 0.06f, 0.1f, 1});

    // 描画は単一スレッドの SpriteBatch を使うのでここは逐次 Each()。
    // EachParallel は描画には使えない（コマンド記録はスレッド非安全）。
    const f32 r = 8.0f;
    GetWorld().Query<Position, Color>().Each(
        [this, r](EntityId, Position& p, Color& c){
            _batch.Draw(*_tex, p.v.x - r, p.v.y - r, r * 2, r * 2,
                        Vec4{c.r, c.g, c.b, 0.95f});
        });

    _batch.End();
}

void HelloECSApp::OnShutdown() noexcept {
    _tex.Reset();
    _batch.Shutdown();
}

void HelloECSApp::SpawnRandomEntities(u32 n) noexcept {
    const f32 sw = static_cast<f32>(GetRenderer().Swapchain()->Width());
    const f32 sh = static_cast<f32>(GetRenderer().Swapchain()->Height());
    World& w = GetWorld();
    for (u32 i = 0; i < n; ++i) {
        EntityId e = w.Create();
        w.Add(e, Position{ Vec2{ _rnd() * sw, _rnd() * sh } });
        w.Add(e, Velocity{ Vec2{ (_rnd() - 0.5f) * 200.0f, (_rnd() - 0.5f) * 200.0f } });
        w.Add(e, Color{ 0.5f + _rnd() * 0.5f, 0.5f + _rnd() * 0.5f, 0.5f + _rnd() * 0.5f });
    }
}

f32 HelloECSApp::_rnd() noexcept {
    _seed ^= _seed << 13; _seed ^= _seed >> 17; _seed ^= _seed << 5;
    return static_cast<f32>(_seed & 0xFFFFFFu) / 16777216.0f;
}

} // namespace hello04

// SPDX-License-Identifier: Apache-2.0
// HelloECS — FApplication 実装。
//
// 学習ポイント:
//   ・SparseSet ベースの O(1) Add/Remove
//   ・World::Query<>().EachParallel(fn, grain) で並列処理。fn は Entity ごとに
//     別スレッドで独立に呼ばれるので、共有資源には触らないこと
//   ・MessageBroker はシングルスレッド前提の同期 pub/sub。よって publish は
//     並列ループの外（メインスレッド）から行う
//   ・FTimerManager で OnUpdate に頼らない時刻ベース処理
#include "HelloECSApp.h"
#include "Types.h"

#include "app/Sample.h"
#include "platform/Input.h"

#include "ecs/World.h"
#include "ecs/Query.h"
#include "event/MessageBroker.h"
#include "event/Timer.h"

#include "math/Vec.h"
#include "math/Math.h"

#include "foundation/Log.h"

using namespace acs;

namespace hello04 {

void FHelloEcsApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    ACS_SAMPLE_INIT(m_Batch.Init(*dev, GetRenderer().ColorFormat()));

    u8 pixels[kBallTexSize * kBallTexSize * 4];
    GenerateBallTexture(pixels);
    FTextureDesc td{};
    td.width = kBallTexSize; td.height = kBallTexSize;
    td.format = EFormat::R8G8B8A8_UNorm;
    td.initial_data = pixels;
    td.initial_data_size = sizeof(pixels);
    if (auto r = CreateRhiTexture(*dev, td); r.IsErr()) {
        ACS_LOG_ERROR("ball texture: %s", r.Error().message);
        Quit(); return;
    } else {
        m_Tex = Move(r.Value());
    }

    SpawnRandomEntities(200);
    GetEvents().Subscribe<FSpawnEvent>(&OnSpawnEvent, nullptr);

    // Timer コールバックは OnUpdate と同じメインスレッドで呼ばれるので、
    // ここから MessageBroker::Publish (シングルスレッド契約) を呼んで安全。
    GetTimers().SetInterval(5.0f, [](void* user){
        auto* self = static_cast<FHelloEcsApp*>(user);
        self->SpawnRandomEntities(30);
        self->GetEvents().Publish<FSpawnEvent>(
            FSpawnEvent{ self->GetWorld().EntityCount() });
    }, this);

    ACS_LOG_INFO("HelloECS initialized (entities=%u)", GetWorld().EntityCount());
}

void FHelloEcsApp::OnUpdate(f32 dt) noexcept {
    if (FInput::IsKeyPressed(EKey::Escape)) Quit();
    if (FInput::IsKeyPressed(EKey::Space)) {
        SpawnRandomEntities(50);
        // Publish はメインスレッド (= 並列ループの外) から呼ぶ契約。
        GetEvents().Publish<FSpawnEvent>(FSpawnEvent{ GetWorld().EntityCount() });
    }

    const f32 sw = static_cast<f32>(GetRenderer().Swapchain()->Width());
    const f32 sh = static_cast<f32>(GetRenderer().Swapchain()->Height());

    // ラムダ内は別スレッドから呼ばれるので、外部の共有資源 (MessageBroker /
    // FSpriteBatch など) に触らないこと。Position / Velocity は Entity 毎に
    // 独立しているので並列化して安全。
    // grain=64 は demo の Entity 数が少ないため意図的に小さい (実プロジェクト
    // では 1024 程度を目安に — タスク生成オーバーヘッドを減らす)。
    GetWorld().Query<FPosition, FVelocity>().EachParallel(
        [dt, sw, sh](FEntityId, FPosition& p, FVelocity& v){
            p.v.x += v.v.x * dt;
            p.v.y += v.v.y * dt;
            if (p.v.x < 0)  { p.v.x = 0;  v.v.x = -v.v.x; }
            if (p.v.x > sw) { p.v.x = sw; v.v.x = -v.v.x; }
            if (p.v.y < 0)  { p.v.y = 0;  v.v.y = -v.v.y; }
            if (p.v.y > sh) { p.v.y = sh; v.v.y = -v.v.y; }
        }, 64);
}

void FHelloEcsApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl || !m_Tex) return;
    const u32 sw = GetRenderer().Swapchain()->Width();
    const u32 sh = GetRenderer().Swapchain()->Height();

    m_Batch.Begin(*cl, sw, sh);
    m_Batch.DrawRect(0, 0, (f32)sw, (f32)sh, FVec4{0.05f, 0.06f, 0.1f, 1});

    // 描画は FSpriteBatch のコマンド記録がスレッド非安全なので必ず逐次 Each()。
    // EachParallel をここで使うと未定義動作になる。
    const f32 r = 8.0f;
    GetWorld().Query<FPosition, FColor>().Each(
        [this, r](FEntityId, FPosition& p, FColor& c){
            m_Batch.Draw(*m_Tex, p.v.x - r, p.v.y - r, r * 2, r * 2,
                        FVec4{c.r, c.g, c.b, 0.95f});
        });

    m_Batch.End();
}

void FHelloEcsApp::OnShutdown() noexcept {
    m_Tex.Reset();
    m_Batch.Shutdown();
}

void FHelloEcsApp::SpawnRandomEntities(u32 n) noexcept {
    const f32 sw = static_cast<f32>(GetRenderer().Swapchain()->Width());
    const f32 sh = static_cast<f32>(GetRenderer().Swapchain()->Height());
    FWorld& w = GetWorld();
    for (u32 i = 0; i < n; ++i) {
        FEntityId e = w.Create();
        w.Add(e, FPosition{ FVec2{ m_Rnd() * sw, m_Rnd() * sh } });
        w.Add(e, FVelocity{ FVec2{ (m_Rnd() - 0.5f) * 200.0f, (m_Rnd() - 0.5f) * 200.0f } });
        w.Add(e, FColor{ 0.5f + m_Rnd() * 0.5f, 0.5f + m_Rnd() * 0.5f, 0.5f + m_Rnd() * 0.5f });
    }
}

f32 FHelloEcsApp::m_Rnd() noexcept {
    m_Seed ^= m_Seed << 13; m_Seed ^= m_Seed >> 17; m_Seed ^= m_Seed << 5;
    return static_cast<f32>(m_Seed & 0xFFFFFFu) / 16777216.0f;
}

} // namespace hello04

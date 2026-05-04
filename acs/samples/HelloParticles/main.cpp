// ACS の HelloParticles サンプル
//
// 動作:
//   ・マウス位置からパーティクル発射
//   ・1 / 2 / 3 / 4 キーで Fire / Sparks / Fountain / Smoke 切替
//   ・左クリックで爆発（バースト）
//   ・スペースで連続生成 ON/OFF
//   ・Esc 終了

#include "app/Application.h"
#include "app/EntryPoint.h"
#include "app/Sample.h"
#include "platform/Input.h"

#include "render/SpriteBatch.h"
#include "render/Particles.h"
#include "render/Font.h"
#include "render/IRhiTexture.h"

#include "math/Math.h"
#include "memory/UniquePtr.h"
#include "foundation/Log.h"

#include <cstdio>

using namespace acs;

namespace {

constexpr u32 kTexSize = 32;

// 中央が明るい円型テクスチャ（パーティクルの粒）
void GenerateGlow(u8* out) noexcept {
    for (u32 y = 0; y < kTexSize; ++y) {
        for (u32 x = 0; x < kTexSize; ++x) {
            const f32 cx = static_cast<f32>(x) - kTexSize * 0.5f;
            const f32 cy = static_cast<f32>(y) - kTexSize * 0.5f;
            const f32 r = Sqrt(cx*cx + cy*cy) / (kTexSize * 0.5f);
            f32 a = 1.0f - r;
            if (a < 0) a = 0;
            // ガウシアンっぽいフォールオフで中央を強調
            a = a * a;
            const u8 byte_a = static_cast<u8>(a * 255);
            const usize i = static_cast<usize>(y * kTexSize + x) * 4;
            out[i+0] = 255; out[i+1] = 255; out[i+2] = 255; out[i+3] = byte_a;
        }
    }
}

const char* kPresetNames[] = {
    "[1] 火 (Fire)",
    "[2] 火花 (Sparks)",
    "[3] 噴水 (Fountain)",
    "[4] 煙 (Smoke)",
};

} // namespace

class HelloParticles : public Application {
public:
    void OnStart() noexcept override {
        IRhiDevice* dev = GetRenderer().Device();
        if (!dev) { Quit(); return; }

        ACS_SAMPLE_INIT(_batch.Init(*dev, GetRenderer().ColorFormat()));
        ACS_SAMPLE_INIT(_ps.Init(8192));

        u8 px[kTexSize * kTexSize * 4];
        GenerateGlow(px);
        TextureDesc td{};
        td.width = kTexSize; td.height = kTexSize;
        td.format = Format::R8G8B8A8_UNorm;
        td.initial_data = px; td.initial_data_size = sizeof(px);
        if (auto r = CreateRhiTexture(*dev, td); r.IsErr()) { Quit(); return; }
        else _glow = Move(r.Value());
        _ps.SetTexture(_glow.Get());

        // フォント
        (void)Sample::TryLoadDefaultUIFont(_font, *dev, 18.0f, 1024, true);

        ApplyPreset(0, Vec2{400, 400});
        ACS_LOG_INFO("HelloParticles initialized");
    }

    void OnUpdate(f32 dt) noexcept override {
        if (Input::IsKeyPressed(Key::Escape)) Quit();
        if (Input::IsKeyPressed(Key::Space)) {
            _ps.Emitter().active = !_ps.Emitter().active;
        }

        Vec2 mp = Input::MousePos();
        if (Input::IsKeyPressed(Key::Num1)) ApplyPreset(0, mp);
        if (Input::IsKeyPressed(Key::Num2)) ApplyPreset(1, mp);
        if (Input::IsKeyPressed(Key::Num3)) ApplyPreset(2, mp);
        if (Input::IsKeyPressed(Key::Num4)) ApplyPreset(3, mp);

        // エミッタを毎フレームマウス位置に追従
        _ps.Emitter().position = mp;

        // 左クリックで爆発
        if (Input::IsMouseButtonPressed(MouseButton::Left)) {
            EmitterDesc burst = EmitterDesc::Sparks(mp);
            burst.rate_per_sec = 0;            // バーストのみ
            burst.life_seconds = 0.8f;
            EmitterDesc save = _ps.Emitter();
            _ps.SetEmitter(burst);
            _ps.EmitBurst(120);
            _ps.SetEmitter(save);
        }

        if (dt > 0.05f) dt = 0.05f;            // 安全 clamp
        _ps.Update(dt);
    }

    void OnRender() noexcept override {
        IRhiCommandList* cl = GetRenderer().CommandList();
        if (!cl) return;
        const u32 sw = GetRenderer().Swapchain()->Width();
        const u32 sh = GetRenderer().Swapchain()->Height();

        _batch.Begin(*cl, sw, sh);
        // 暗めの背景（粒が映える）
        _batch.DrawRect(0, 0, static_cast<f32>(sw), static_cast<f32>(sh),
                        Vec4{0.05f, 0.06f, 0.10f, 1});

        _ps.Render(_batch);

        // HUD
        if (_font.AtlasTexture()) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "粒子: %u / %u   FPS: %.1f   %s",
                          _ps.ActiveCount(), _ps.Capacity(),
                          static_cast<double>(FPS()),
                          _ps.Emitter().active ? "ON" : "OFF");
            _batch.DrawString(_font, buf, 20, 20, Vec4{1,1,1,1});
            _batch.DrawString(_font, kPresetNames[_preset], 20, 44, Vec4{1, 0.85f, 0.4f, 1});
            _batch.DrawString(_font,
                            "1/2/3/4: 種類  Space: 連続 ON/OFF  左クリック: 爆発  Esc: 終了",
                            20, static_cast<f32>(sh - 32),
                            Vec4{0.7f, 0.8f, 0.95f, 1});
        }
        _batch.End();
    }

    void OnShutdown() noexcept override {
        if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
        _font.Shutdown();
        _ps.Shutdown();
        _glow.Reset();
        _batch.Shutdown();
    }

private:
    void ApplyPreset(u32 idx, Vec2 pos) noexcept {
        _preset = idx;
        EmitterDesc d;
        switch (idx) {
            case 0: d = EmitterDesc::Fire(pos);     break;
            case 1: d = EmitterDesc::Sparks(pos);   break;
            case 2: d = EmitterDesc::Fountain(pos); break;
            case 3: d = EmitterDesc::Smoke(pos);    break;
            default:d = EmitterDesc::Fire(pos);     break;
        }
        _ps.SetEmitter(d);
    }

    SpriteBatch    _batch;
    ParticleSystem _ps;
    Font           _font;
    UniquePtr<IRhiTexture> _glow;
    u32 _preset = 0;
};

ACS_DEFINE_MAIN(HelloParticles)

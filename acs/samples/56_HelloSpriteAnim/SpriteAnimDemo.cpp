// SPDX-License-Identifier: Apache-2.0
// HelloSpriteAnim - Package A デモ: スプライトシートアニメ + HUD テキスト。
//
// 新しい基盤機能を 1 画面で示す:
//   - ASprite2DComponent の UV サブ矩形
//   - ASpriteAnimComponent.InitGrid(...) でグリッドシートを再生
//   - CGame が配線する共有フォントで OnDrawHud から DrawString
//
// 手続き生成した 4 フレームのシート (各セルでドットが 90° ずつ回る) を 8fps で
// ループ再生し、ノード自体も左右に揺らしてアニメ + transform を同時に見せる。
#include "gameframework/GameFramework.h"
#include "render/IRhiTexture.h"
#include "platform/InputCodes.h"
#include "math/Math.h"

#include <cstdio>

using namespace acs;
using namespace acs::game;

namespace {

constexpr FActionId kQuit("Quit");

// 4 フレーム (各 64x64、横並び 256x64) のスプライトシートを手続き生成する。
// frame c のセルに、角度 c*90° の位置を中心とした明るいドットを描く。
// 各フレームで色味も変え、アニメしているのが一目で分かるようにする。
TUniquePtr<IRhiTexture> MakeAnimSheet(IRhiDevice& device) noexcept {
    constexpr u32 kCell = 64, kFrames = 4, kW = kCell * kFrames, kH = kCell;
    static u8 px[kW * kH * 4];

    for (u32 c = 0; c < kFrames; ++c) {
        const f32 ang = static_cast<f32>(c) * 1.5707963f;     // c * 90°
        const f32 dotx = 32.0f + Cos(ang) * 18.0f;
        const f32 doty = 32.0f + Sin(ang) * 18.0f;
        // フレームごとの基調色 (RGB)。
        const u8 br = static_cast<u8>(80 + c * 30);
        const u8 bg = static_cast<u8>(180 - c * 30);
        const u8 bb = static_cast<u8>(120 + ((c % 2) ? 90 : 0));
        for (u32 y = 0; y < kCell; ++y) {
            for (u32 x = 0; x < kCell; ++x) {
                const f32 dx = static_cast<f32>(x) - dotx;
                const f32 dy = static_cast<f32>(y) - doty;
                const bool dot = (dx * dx + dy * dy) <= (11.0f * 11.0f);
                const u32 gx = c * kCell + x;
                const u32 i = (y * kW + gx) * 4u;
                if (dot) {
                    px[i + 0] = 250; px[i + 1] = 240; px[i + 2] = 90; px[i + 3] = 255;
                } else {
                    // 縁取りで各セルの境界を見せる
                    const bool edge = (x < 2 || x >= kCell - 2 || y < 2 || y >= kCell - 2);
                    px[i + 0] = edge ? 40 : br;
                    px[i + 1] = edge ? 40 : bg;
                    px[i + 2] = edge ? 40 : bb;
                    px[i + 3] = 255;
                }
            }
        }
    }

    FTextureDesc td{};
    td.width = kW; td.height = kH; td.format = EFormat::R8G8B8A8_UNorm;
    td.initial_data = px; td.initial_data_size = kW * kH * 4u;
    auto r = CreateRhiTexture(device, td);
    if (r.IsErr()) return {};
    return Move(r.Value());
}

class AAnimScene final : public AScene2D {
public:
    void OnReady() noexcept override {
        SetPixelsPerUnit(96.0f);
        Services().Input().ClearAll();
        Services().Input().BindKey(kQuit, EKey::Escape);

        IRhiDevice* dev = GetGame().GetRenderer().Device();
        if (dev != nullptr) m_Sheet = MakeAnimSheet(*dev);

        auto node = NewObject<ANode>();
        node->SetPosition2D(FVec2{0.0f, 0.0f});
        auto& spr = node->AddComponent<ASprite2DComponent>(FVec2{2.0f, 2.0f});
        if (m_Sheet) spr.SetTexture(m_Sheet.Get());
        auto& anim = node->AddComponent<ASpriteAnimComponent>();
        anim.InitGrid(/*cols=*/4, /*rows=*/1, /*frame_count=*/4, /*fps=*/8.0f,
                      EPlayMode::Loop);
        anim.Play();
        m_Anim = &anim;
        m_Node = &Root().AddChild(Move(node));

        Services().Camera().SetPosition(FVec2{0.0f, 0.0f});
        Services().Camera().SetZoom(1.0f);
        GetGame().SetClearColor(0.06f, 0.07f, 0.09f);
    }

    void OnTick(f32 dt) noexcept override {
        if (Services().Input().IsPressed(kQuit)) { GetGame().Quit(); return; }
        m_Elapsed += dt;
        if (m_Node) {
            FVec2 Position = m_Node->Position2D();
            Position.x = Sin(m_Elapsed * 1.5f) * 1.5f;
            m_Node->SetPosition2D(Position);
        }
    }

    void OnDrawHud(FRenderContext& rc, CSpriteBatch& sb) noexcept override {
        sb.DrawRect(8.0f, 8.0f, 470.0f, 32.0f, FVec4{0.0f, 0.0f, 0.0f, 0.45f});
        if (!rc.HasFont()) return;
        char line[160];
        std::snprintf(line, sizeof(line),
                      "HelloSpriteAnim  frame=%u  fps=8 loop  [Esc] quit",
                      m_Anim != nullptr ? m_Anim->CurrentFrame() : 0u);
        sb.DrawString(rc.GetFont(), line, 16.0f, 15.0f, FVec4{0.90f, 0.95f, 1.0f, 1.0f});
    }

private:
    TUniquePtr<IRhiTexture> m_Sheet;
    ANode*                m_Node = nullptr;
    ASpriteAnimComponent*   m_Anim = nullptr;
    f32                     m_Elapsed = 0.0f;
};

class CAnimGame final : public CGame {
protected:
    TUniquePtr<AScene> InitialScene() noexcept override {
        return MakeUnique<AAnimScene>();
    }
};

} // namespace

ACS_GAME_MAIN(CAnimGame)

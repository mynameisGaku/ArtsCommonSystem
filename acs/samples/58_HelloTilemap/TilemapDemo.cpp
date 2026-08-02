// SPDX-License-Identifier: Apache-2.0
// HelloTilemap - Package C デモ: タイルマップ描画 + フェード付きシーン遷移。
//
//   Title シーン (テキストのみ) ── [Space] ──▶ Level シーン (タイルマップ)
//          ▲───────────────────── [Esc] ──────────────┘
//   遷移は CGame::TransitionTo() の FadeInOut で行う (暗転中にシーン差し替え)。
//   タイルは ATilemapComponent が 2x2 アトラスのセルとして描画する。
#include "gameframework/GameFramework.h"
#include "render/IRhiTexture.h"
#include "platform/InputCodes.h"

using namespace acs;
using namespace acs::game;

namespace {

constexpr FActionId kStart("Start");
constexpr FActionId kBack ("Back");

class ATitleScene;
class ALevelScene;

// 2x2 = 4 タイルのアトラス (各セル 32x32 → 64x64)。
//   cell0=草(緑) cell1=石(灰) cell2=水(青) cell3=砂(黄)
TUniquePtr<IRhiTexture> MakeTileAtlas(IRhiDevice& device) noexcept {
    constexpr u32 kCell = 32, kCols = 2, kRows = 2, kW = kCell * kCols, kH = kCell * kRows;
    static u8 px[kW * kH * 4];
    const u8 colors[4][3] = {
        { 70, 150,  70 },   // grass
        {130, 130, 140 },   // stone
        { 60, 110, 200 },   // water
        {200, 180,  90 },   // sand
    };
    for (u32 cy = 0; cy < kRows; ++cy) {
        for (u32 cx = 0; cx < kCols; ++cx) {
            const u32 ci = cy * kCols + cx;
            for (u32 y = 0; y < kCell; ++y) {
                for (u32 x = 0; x < kCell; ++x) {
                    const bool edge = (x == 0 || y == 0 || x == kCell - 1 || y == kCell - 1);
                    const u32 gx = cx * kCell + x;
                    const u32 gy = cy * kCell + y;
                    const u32 i = (gy * kW + gx) * 4u;
                    px[i + 0] = edge ? static_cast<u8>(colors[ci][0] * 6u / 10u) : colors[ci][0];
                    px[i + 1] = edge ? static_cast<u8>(colors[ci][1] * 6u / 10u) : colors[ci][1];
                    px[i + 2] = edge ? static_cast<u8>(colors[ci][2] * 6u / 10u) : colors[ci][2];
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

class ALevelScene final : public AScene {
public:
    /** 2D の標準サービス構成 (Default2D | Camera2D | Physics2D) を要求する。 */
    ESvc WantedServices() const noexcept override { return kScene2DServices; }

    void OnReady() noexcept override {
        SetPixelsPerUnit(48.0f);
        Services().Input().ClearAll();
        Services().Input().BindKey(kBack, EKey::Escape);

        IRhiDevice* dev = GetGame().GetRenderer().Device();
        if (dev != nullptr) m_Atlas = MakeTileAtlas(*dev);

        auto node = NewObject<ANode>();
        node->SetPosition2D(FVec2{-8.0f, -5.5f});      // マップ原点 (左上寄り。Y-down: row0 が画面上)
        auto& tm = node->AddComponent<ATilemapComponent>();
        tm.Map().Init(/*w=*/16, /*h=*/11, /*layers=*/1, /*tile_size=*/1.0f);
        tm.Map().Fill(FTileId{1});                          // 全面 草
        tm.Map().FillRect(0, 0, 15, 0,  FTileId{2});        // 上端 石 (row0 = 画面上)
        tm.Map().FillRect(0, 10, 15, 10, FTileId{2});       // 下端 石 (最大 row = 画面下)
        tm.Map().FillRect(0, 0, 0, 10,  FTileId{2});        // 左端 石
        tm.Map().FillRect(15, 0, 15, 10, FTileId{2});       // 右端 石
        tm.Map().FillRect(6, 4, 9, 6,   FTileId{3});        // 中央に 水たまり
        if (m_Atlas) { tm.SetTexture(m_Atlas.Get()); tm.SetAtlasGrid(2, 2); }
        Root().AddChild(Move(node));

        Services().Camera().SetPosition(FVec2{0.0f, 0.0f});
        GetGame().SetClearColor(0.05f, 0.06f, 0.08f);
    }

    void OnTick(f32 dt) noexcept override;   // 下で定義 (ATitleScene を参照するため)

    void OnDrawHud(FRenderContext& rc, CSpriteBatch& sb) noexcept override {
        sb.DrawRect(8.0f, 8.0f, 470.0f, 30.0f, FVec4{0, 0, 0, 0.45f});
        if (rc.HasFont()) {
            sb.DrawString(rc.GetFont(), "Level (tilemap)  [Esc] -> title",
                          16.0f, 15.0f, FVec4{0.90f, 0.95f, 1.0f, 1.0f});
        }
    }

private:
    TUniquePtr<IRhiTexture> m_Atlas;
};

class ATitleScene final : public AScene {
public:
    /** 2D の標準サービス構成 (Default2D | Camera2D | Physics2D) を要求する。 */
    ESvc WantedServices() const noexcept override { return kScene2DServices; }

    void OnReady() noexcept override {
        Services().Input().ClearAll();
        Services().Input().BindKey(kStart, EKey::Space);
        Services().Input().BindKey(kBack, EKey::Escape);
        GetGame().SetClearColor(0.08f, 0.06f, 0.10f);
    }

    void OnTick(f32 dt) noexcept override;   // 下で定義

    void OnDrawHud(FRenderContext& rc, CSpriteBatch& sb) noexcept override {
        sb.DrawRect(8.0f, 8.0f, 540.0f, 30.0f, FVec4{0, 0, 0, 0.45f});
        if (rc.HasFont()) {
            sb.DrawString(rc.GetFont(), "HelloTilemap Title   [Space] start   [Esc] quit",
                          16.0f, 15.0f, FVec4{1.0f, 0.95f, 0.90f, 1.0f});
        }
    }
};

// ---- 相互参照する OnTick は両クラス定義後に out-of-line で実装 ----
void ALevelScene::OnTick(f32 /*dt*/) noexcept {
    if (GetGame().Fade().IsActive()) return;          // 遷移中は入力を無視
    if (Services().Input().IsPressed(kBack)) {
        GetGame().TransitionTo(MakeUnique<ATitleScene>(), 0.3f, 0.3f);
    }
}

void ATitleScene::OnTick(f32 /*dt*/) noexcept {
    if (GetGame().Fade().IsActive()) return;
    if (Services().Input().IsPressed(kStart)) {
        GetGame().TransitionTo(MakeUnique<ALevelScene>(), 0.3f, 0.3f);
    } else if (Services().Input().IsPressed(kBack)) {
        GetGame().Quit();
    }
}

class CTilemapGame final : public CGame {
protected:
    TUniquePtr<AScene> InitialScene() noexcept override {
        return MakeUnique<ATitleScene>();
    }
};

} // namespace

ACS_GAME_MAIN(CTilemapGame)

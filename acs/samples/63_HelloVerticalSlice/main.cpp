// SPDX-License-Identifier: Apache-2.0
// HelloVerticalSlice — 2D ゲーム制作基盤の「縦スライス」統合デモ。
//
//   Title ──[Play/Space]──▶ Play ──[全コイン取得 or 時間切れ]──▶ GameOver
//     ▲                       │ [P/Esc]                              │
//     │                    (Pause overlay: Resume / Quit to Title)    │
//     └──────────[Title]──────┴──────────[Retry → Play]──────────────┘
//
// 1 本で 2D 基盤の P0 を全部見せる:
//   ・FScene2D による画面遷移 (ChangeScene)
//   ・クリック可能 UI メニュー (FUiLayer、スクリーン空間)
//   ・自前 atlas テクスチャ + FSpritePack フレーム → プレイヤースプライト
//   ・FTilemapComponent によるタイルマップ描画
//   ・FSettings によるハイスコア永続化 (INI、次回起動でロード)
//
// 座標は ACS 2D 正規規約 = **Y-down** (左上原点・+X 右・+Y 下)。
//   - tile row 0 / 小さい Y = 画面上。床/下側 = 大きい Y。
//   - 「上へ移動」= -Y (W キー)、「下へ」= +Y (S キー)。
//   - UI/HUD はスクリーン空間 (左上原点・ピクセル)。
#include "gameframework/GameFramework.h"
#include "gameframework/UiLayer.h"
#include "gameframework/Settings.h"
#include "gameframework/SpritePack.h"
#include "gameframework/Sprite2DComponent.h"
#include "gameframework/TilemapComponent.h"
#include "render/IRhiTexture.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "app/Sample.h"
#include "platform/Event.h"
#include "platform/InputCodes.h"
#include "math/Math.h"

#include <cstdio>

using namespace acs;
using namespace acs::game;

namespace {

// ---- 定数 -----------------------------------------------------------------
constexpr wchar_t kSavePath[] = L"vslice_save.ini";   // ハイスコア保存先 (相対 = exe 隣)

// レベル: 16x11 タイル、1 タイル = 1 world 単位。原点を画面中央に置くため
// マップノードを左上 (-8,-5.5) に配置 (Y-down: 負 Y = 画面上)。
constexpr u32 kMapW = 16, kMapH = 11;
constexpr f32 kTile = 1.0f;
constexpr f32 kOriginX = -static_cast<f32>(kMapW) * 0.5f;   // -8.0
constexpr f32 kOriginY = -static_cast<f32>(kMapH) * 0.5f;   // -5.5

constexpr f32 kPlayerSpeed = 5.0f;       // world unit / sec
constexpr f32 kCoinRadius  = 0.55f;      // 取得判定半径
constexpr f32 kGameTime    = 30.0f;      // 制限時間 (秒)
constexpr u32 kNumCoins    = 6;

// プレイ範囲は石の外周 (実 AABB コライダー) が FPhysicsBody2D の collide-and-slide で
// 囲うので、手動クランプは不要 (壁/水に沿って滑る)。

// コインのタイル座標 (草マスのみ、外周/水たまりを避ける)。
constexpr u32 kCoinTileX[kNumCoins] = { 3, 12,  2, 13,  7,  8 };
constexpr u32 kCoinTileY[kNumCoins] = { 2,  2,  8,  8,  2,  8 };

// 色
constexpr FVec4 kColTopBar { 0.0f, 0.0f, 0.0f, 0.55f };
constexpr FVec4 kColText   { 0.92f, 0.95f, 1.0f, 1.0f };
constexpr FVec4 kColAccent { 1.0f, 0.85f, 0.35f, 1.0f };
constexpr FVec4 kColDim    { 0.02f, 0.03f, 0.05f, 0.72f };

// タイル ID → 中心 world 座標 (Y-down)。
FVec2 TileCenter(u32 tx, u32 ty) noexcept {
    return FVec2{ kOriginX + static_cast<f32>(tx) + 0.5f,
                 kOriginY + static_cast<f32>(ty) + 0.5f };
}

// 2x2 = 4 タイルのアトラス (各 32x32)。cell0=草 cell1=石 cell2=水 cell3=砂。
TUniquePtr<IRhiTexture> MakeTileAtlas(IRhiDevice& dev) noexcept {
    constexpr u32 kCell = 32, kCols = 2, kRows = 2, kW = kCell * kCols, kH = kCell * kRows;
    static u8 px[kW * kH * 4];
    const u8 colors[4][3] = {
        { 70, 150,  70 },   // grass
        {120, 120, 132 },   // stone
        { 60, 110, 200 },   // water
        {200, 180,  90 },   // sand
    };
    for (u32 cy = 0; cy < kRows; ++cy)
        for (u32 cx = 0; cx < kCols; ++cx) {
            const u32 ci = cy * kCols + cx;
            for (u32 y = 0; y < kCell; ++y)
                for (u32 x = 0; x < kCell; ++x) {
                    const bool edge = (x == 0 || y == 0 || x == kCell - 1 || y == kCell - 1);
                    const u32 gx = cx * kCell + x, gy = cy * kCell + y;
                    const u32 i = (gy * kW + gx) * 4u;
                    px[i + 0] = edge ? static_cast<u8>(colors[ci][0] * 6u / 10u) : colors[ci][0];
                    px[i + 1] = edge ? static_cast<u8>(colors[ci][1] * 6u / 10u) : colors[ci][1];
                    px[i + 2] = edge ? static_cast<u8>(colors[ci][2] * 6u / 10u) : colors[ci][2];
                    px[i + 3] = 255;
                }
        }
    FTextureDesc td{};
    td.width = kW; td.height = kH; td.format = EFormat::R8G8B8A8_UNorm;
    td.initial_data = px; td.initial_data_size = kW * kH * 4u;
    auto r = CreateRhiTexture(dev, td);
    if (r.IsErr()) return {};
    return Move(r.Value());
}

// 64x32 ヒーローアトラス: 左 32x32 = プレイヤー (シアンの顔)、右 32x32 = 予備フレーム。
TUniquePtr<IRhiTexture> MakeHeroAtlas(IRhiDevice& dev) noexcept {
    constexpr u32 kW = 64, kH = 32;
    static u8 px[kW * kH * 4];
    for (u32 y = 0; y < kH; ++y)
        for (u32 x = 0; x < kW; ++x) {
            const u32 i = (y * kW + x) * 4u;
            const bool left = (x < 32);
            const u32 lx = left ? x : x - 32;            // フレームローカル X
            const bool border = (lx == 0 || y == 0 || lx == 31 || y == 31);
            // 目 (フレームローカル座標)
            const bool eye = (y >= 10 && y <= 14) && ((lx >= 9 && lx <= 12) || (lx >= 19 && lx <= 22));
            u8 r, g, b;
            if (border)   { r = 20;  g = 30;  b = 40; }
            else if (eye) { r = 15;  g = 20;  b = 28; }
            else if (left) { r = 60; g = 210; b = 230; }   // シアン本体
            else           { r = 230; g = 120; b = 200; }  // 予備=マゼンタ
            px[i + 0] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = 255;
        }
    FTextureDesc td{};
    td.width = kW; td.height = kH; td.format = EFormat::R8G8B8A8_UNorm;
    td.initial_data = px; td.initial_data_size = kW * kH * 4u;
    auto r = CreateRhiTexture(dev, td);
    if (r.IsErr()) return {};
    return Move(r.Value());
}

// 入力アクション
constexpr ActionId kMoveX("vs.MoveX");
constexpr ActionId kMoveY("vs.MoveY");
constexpr ActionId kStart("vs.Start");
constexpr ActionId kPause("vs.Pause");
constexpr ActionId kQuit ("vs.Quit");

class TitleScene;
class PlayScene;
class GameOverScene;

// ===========================================================================
// アプリ: ハイスコア永続化 (FSettings) + タイトル用大フォントを保持。
// ===========================================================================
class FVerticalSliceGame final : public FGame {
public:
    i32 HighScore() const noexcept { return m_HighScore; }

    // ハイスコア更新時のみ INI に保存。戻り値 = 自己ベスト更新したか。
    bool SubmitScore(i32 score) noexcept {
        if (score <= m_HighScore) return false;
        m_HighScore = score;
        m_Cfg.SetI32("hi.score", score);
        auto r = m_Cfg.Save(kSavePath);
        if (r.IsErr()) ACS_LOG_WARN("vslice: highscore save failed: %s", r.Error().message);
        return true;
    }

    // タイトル/見出し用の大きめフォント (英字)。device が出来てから遅延ロード。
    // 失敗時 (フォント不在環境) は nullptr を返し、呼び側は共有フォントへ fallback。
    Font* TitleFont() noexcept {
        if (!m_TitleTried) {
            m_TitleTried = true;
            IRhiDevice* dev = GetRenderer().Device();
            if (dev) m_TitleReady =
                FSample::TryLoadDefaultUIFont(m_TitleFont, *dev, 44.0f, 1024, false).IsOk();
        }
        return m_TitleReady ? &m_TitleFont : nullptr;
    }

protected:
    void OnStart() noexcept override {
        // 設定ファイルがあればハイスコアを読む (無ければ Err → 0 のまま)。
        (void)m_Cfg.Load(kSavePath);
        m_HighScore = m_Cfg.GetI32("hi.score", 0);
        FGame::OnStart();   // InitialScene() を push
    }

    void OnShutdown() noexcept override {
        if (m_TitleReady) m_TitleFont.Shutdown();
        FGame::OnShutdown();
    }

    TUniquePtr<Scene> InitialScene() noexcept override;   // 下で定義 (TitleScene 完全後)

private:
    FSettings m_Cfg;
    i32       m_HighScore  = 0;
    Font      m_TitleFont;
    bool      m_TitleTried = false;
    bool      m_TitleReady = false;
};

// 共有: スクリーン中央寄せでテキストを描く。
void DrawCentered(FSpriteBatch& sb, Font* font, const char* s, f32 cx, f32 y, FVec4 col) noexcept {
    if (!font) return;
    const f32 w = font->MeasureWidth(s);
    sb.DrawString(*font, s, cx - w * 0.5f, y, col);
}

FVerticalSliceGame& App(Scene& s) noexcept {
    return static_cast<FVerticalSliceGame&>(s.GetGame());
}

// ===========================================================================
// Title
// ===========================================================================
class TitleScene final : public FScene2D {
public:
    void OnReady() noexcept override {
        GetGame().SetClearColor(0.06f, 0.07f, 0.11f);
        FInputMap& in = Services().Input();
        in.ClearAll();
        in.BindKey(kStart, EKey::Space);
        in.BindKey(kQuit,  EKey::Escape);
        m_Ui.Init();
        Services().Camera().SetPosition(FVec2{0.0f, 0.0f});
    }

    void OnEvent(const Event& e) noexcept override { m_Ui.HandleInput(e); }

    void OnTick(f32 dt) noexcept override;   // 遷移を含むので out-of-line

    void OnDrawHud(RenderContext& rc, FSpriteBatch& sb) noexcept override {
        BuildUi(rc.Width(), rc.Height());
        Font* big   = App(*this).TitleFont();
        Font* small = rc.HasFont() ? &rc.GetFont() : nullptr;
        const f32 cx = static_cast<f32>(rc.Width()) * 0.5f;
        DrawCentered(sb, big ? big : small, "ACS Vertical Slice", cx, rc.Height() * 0.22f, kColText);

        char line[96];
        std::snprintf(line, sizeof(line), "HIGH SCORE: %d", App(*this).HighScore());
        DrawCentered(sb, small, line, cx, rc.Height() * 0.34f, kColAccent);

        m_Ui.Draw(rc);   // PLAY / QUIT ボタン (sb は rc 内部から取得)
        DrawCentered(sb, small, "Click a button, or press Space to play / Esc to quit",
                     cx, rc.Height() - 40.0f, FVec4{0.7f, 0.75f, 0.85f, 1.0f});
    }

    void OnExit() noexcept override { m_Ui.Shutdown(); FScene2D::OnExit(); }

private:
    void BuildUi(u32 w, u32 h) noexcept {
        if (m_UiBuilt) return;
        m_UiBuilt = true;
        const f32 bw = 240.0f, bh = 52.0f;
        const f32 x  = (static_cast<f32>(w) - bw) * 0.5f;
        const f32 y0 = static_cast<f32>(h) * 0.46f;
        m_PlayBtn = m_Ui.AddButton("PLAY", FVec2{x, y0},          FVec2{bw, bh});
        m_QuitBtn = m_Ui.AddButton("QUIT", FVec2{x, y0 + bh + 16.0f}, FVec2{bw, bh});
    }

    FUiLayer m_Ui;
    u32      m_PlayBtn = 0, m_QuitBtn = 0;
    bool     m_UiBuilt = false;
};

// ===========================================================================
// Play (タイルマップ + atlas プレイヤー + コイン + ポーズ state)
// ===========================================================================
class PlayScene final : public FScene2D {
public:
    void OnReady() noexcept override {
        SetPixelsPerUnit(44.0f);
        GetGame().SetClearColor(0.05f, 0.06f, 0.08f);

        FInputMap& in = Services().Input();
        in.ClearAll();
        in.BindAxisKeys(kMoveX, EKey::A, EKey::D);   // A=-X(左) D=+X(右)
        in.BindAxisKeys(kMoveY, EKey::W, EKey::S);   // W=-Y(画面上) S=+Y(画面下)  ※Y-down
        in.BindKey(kPause, EKey::P);
        in.BindKey(kPause, EKey::Escape);

        IRhiDevice* dev = GetGame().GetRenderer().Device();

        // --- タイルマップ ---
        if (dev) m_TileAtlas = MakeTileAtlas(*dev);
        auto mapNode = MakeUnique<FNode2D>();
        mapNode->Local().position = FVec2{kOriginX, kOriginY};   // 左上 (Y-down)
        auto& tm = mapNode->AddComponent<FTilemapComponent>();
        tm.Map().Init(kMapW, kMapH, 1, kTile);
        tm.Map().Fill(FTileId{1});                                  // 草
        tm.Map().FillRect(0, 0, kMapW - 1, 0, FTileId{2});          // 上端 石 (row0 = 画面上)
        tm.Map().FillRect(0, kMapH - 1, kMapW - 1, kMapH - 1, FTileId{2}); // 下端 石
        tm.Map().FillRect(0, 0, 0, kMapH - 1, FTileId{2});          // 左端 石
        tm.Map().FillRect(kMapW - 1, 0, kMapW - 1, kMapH - 1, FTileId{2}); // 右端 石
        tm.Map().FillRect(6, 4, 9, 6, FTileId{3});                  // 中央 水たまり
        if (m_TileAtlas) { tm.SetTexture(m_TileAtlas.Get()); tm.SetAtlasGrid(2, 2); }
        Root().AddChild(Move(mapNode));

        // --- 壁/水を実コライダー化 (collide-and-slide 用)。Y-down で AABB を配置 ---
        FCollisionWorld2D& phy = Services().Physics();
        phy.Init(2.0f);
        const f32 hw = static_cast<f32>(kMapW) * 0.5f;            // マップ半幅/半高
        const f32 hh = static_cast<f32>(kMapH) * 0.5f;
        phy.AddAabb(Aabb2{FVec2{0.0f, kOriginY + 0.5f},                         FVec2{hw, 0.5f}});  // 上端 石
        phy.AddAabb(Aabb2{FVec2{0.0f, kOriginY + static_cast<f32>(kMapH) - 0.5f}, FVec2{hw, 0.5f}}); // 下端 石
        phy.AddAabb(Aabb2{FVec2{kOriginX + 0.5f, 0.0f},                         FVec2{0.5f, hh}});  // 左端 石
        phy.AddAabb(Aabb2{FVec2{kOriginX + static_cast<f32>(kMapW) - 0.5f, 0.0f}, FVec2{0.5f, hh}}); // 右端 石
        phy.AddAabb(Aabb2{FVec2{0.0f, 0.0f},                                    FVec2{2.0f, 1.5f}}); // 中央 水たまり
        // 斜めの OBB バー (有向境界ボックス)。回転矩形の実コライダー。
        m_ObbBar = Obb2{ FVec2{-4.5f, -2.2f}, FVec2{1.5f, 0.28f}, 0.5f };
        phy.AddObb(m_ObbBar);

        // --- プレイヤー (atlas の "hero" フレームを UV で貼る + 物理ボディ) ---
        if (dev) m_HeroAtlas = MakeHeroAtlas(*dev);
        SpritePackInfo info{}; info.atlas_width = 64; info.atlas_height = 32;
        m_Pack.Init(info);
        SpriteFrame fr{}; fr.name = "hero"; fr.x = 0; fr.y = 0; fr.w = 32; fr.h = 32;
        m_Pack.AddFrame(fr);
        auto player = MakeUnique<FNode2D>();
        player->Local().position = TileCenter(3, 5);               // 草の上 (左寄り)
        auto& spr = player->AddComponent<FSprite2DComponent>(FVec2{0.9f, 0.9f});
        if (const SpriteFrame* f = m_Pack.FindFrame("hero"); f && m_HeroAtlas) {
            spr.SetTexture(m_HeroAtlas.Get());
            spr.SetUvRect(m_Pack.ComputeUv(*f));                   // atlas 左半分のみ
        }
        auto& body = player->AddComponent<FPhysicsBody2D>(phy);
        body.SetCircle(0.4f);
        body.gravity = FVec2{0.0f, 0.0f};   // 見下ろし: 重力なし
        body.slide   = true;                // 壁/水に沿って滑る (collide-and-slide)
        m_PlayerBody = &body;
        m_Player = &Root().AddChild(Move(player));

        // --- コイン (data のみ。OnDrawWorld で描く) ---
        for (u32 i = 0; i < kNumCoins; ++i) {
            m_CoinPos[i] = TileCenter(kCoinTileX[i], kCoinTileY[i]);
            m_CoinGot[i] = false;
        }
        m_Collected = 0; m_Score = 0; m_TimeLeft = kGameTime; m_State = EState::Playing;

        Services().Camera().SetPosition(FVec2{0.0f, 0.0f});
        Services().Camera().SetZoom(1.0f);
        m_Ui.Init();
    }

    void OnEvent(const Event& e) noexcept override { m_Ui.HandleInput(e); }

    void OnTick(f32 dt) noexcept override;       // 遷移/ロジックを含むので out-of-line

    void OnDrawWorld(RenderContext& /*rc*/, FSpriteBatch& sb) noexcept override {
        // 斜めの OBB バー (回転矩形コライダー) を可視化。collider と同じ中心/回転。
        sb.DrawRectRotated(m_ObbBar.center.x, m_ObbBar.center.y,
                           m_ObbBar.half_size.x * 2.0f, m_ObbBar.half_size.y * 2.0f,
                           m_ObbBar.rotation, FVec4{0.52f, 0.36f, 0.20f, 1.0f});
        // コイン (world 空間)。取得済みは描かない。軽く明滅。
        const f32 pulse = 0.75f + 0.25f * Sin(m_Time * 6.0f);
        for (u32 i = 0; i < kNumCoins; ++i) {
            if (m_CoinGot[i]) continue;
            const FVec2 c = m_CoinPos[i];
            sb.DrawRect(c.x - 0.22f, c.y - 0.22f, 0.44f, 0.44f,
                        FVec4{1.0f, 0.84f * pulse, 0.25f, 1.0f});
        }
    }

    void OnDrawHud(RenderContext& rc, FSpriteBatch& sb) noexcept override {
        BuildUi(rc.Width(), rc.Height());
        Font* small = rc.HasFont() ? &rc.GetFont() : nullptr;
        const f32 cx = static_cast<f32>(rc.Width()) * 0.5f;

        // 上部 HUD バー: スコア / コイン / 残り時間。
        sb.DrawRect(0.0f, 0.0f, static_cast<f32>(rc.Width()), 34.0f, kColTopBar);
        if (small) {
            char hud[128];
            std::snprintf(hud, sizeof(hud), "SCORE %d    COINS %u/%u    TIME %d",
                          m_Score, m_Collected, kNumCoins,
                          static_cast<int>(m_TimeLeft + 0.999f));
            sb.DrawString(*small, hud, 14.0f, 9.0f, kColText);
        }
        DrawCentered(sb, small, "WASD move (W=up, Y-down)   P/Esc pause",
                     cx, rc.Height() - 28.0f, FVec4{0.65f, 0.7f, 0.8f, 1.0f});

        if (m_State == EState::Paused) {
            // 全画面ディム + メニュー。
            sb.DrawRect(0.0f, 0.0f, static_cast<f32>(rc.Width()), static_cast<f32>(rc.Height()), kColDim);
            Font* big = App(*this).TitleFont();
            DrawCentered(sb, big ? big : small, "PAUSED", cx, rc.Height() * 0.30f, kColText);
            m_Ui.Draw(rc);   // RESUME / QUIT TO TITLE
        }
    }

    void OnExit() noexcept override { m_Ui.Shutdown(); FScene2D::OnExit(); }

private:
    enum class EState { Playing, Paused };

    void BuildUi(u32 w, u32 h) noexcept {
        if (m_UiBuilt) return;
        m_UiBuilt = true;
        const f32 bw = 280.0f, bh = 50.0f;
        const f32 x  = (static_cast<f32>(w) - bw) * 0.5f;
        const f32 y0 = static_cast<f32>(h) * 0.44f;
        m_ResumeBtn = m_Ui.AddButton("RESUME",        FVec2{x, y0},               FVec2{bw, bh});
        m_TitleBtn  = m_Ui.AddButton("QUIT TO TITLE", FVec2{x, y0 + bh + 14.0f},  FVec2{bw, bh});
        // ポーズ中だけ表示。
        const bool show = (m_State == EState::Paused);
        m_Ui.SetVisible(m_ResumeBtn, show);
        m_Ui.SetVisible(m_TitleBtn,  show);
    }

    void SetPaused(bool paused) noexcept {
        m_State = paused ? EState::Paused : EState::Playing;
        if (m_UiBuilt) {
            m_Ui.SetVisible(m_ResumeBtn, paused);
            m_Ui.SetVisible(m_TitleBtn,  paused);
        }
    }

    void EndGame(bool win) noexcept;   // GameOverScene へ (out-of-line)

    TUniquePtr<IRhiTexture> m_TileAtlas;
    TUniquePtr<IRhiTexture> m_HeroAtlas;
    FSpritePack             m_Pack;
    FNode2D*                m_Player = nullptr;
    FPhysicsBody2D*         m_PlayerBody = nullptr;
    Obb2                    m_ObbBar;
    FVec2  m_CoinPos[kNumCoins]{};
    bool   m_CoinGot[kNumCoins]{};
    u32    m_Collected = 0;
    i32    m_Score = 0;
    f32    m_TimeLeft = kGameTime;
    f32    m_Time = 0.0f;
    EState m_State = EState::Playing;

    FUiLayer m_Ui;
    u32      m_ResumeBtn = 0, m_TitleBtn = 0;
    bool     m_UiBuilt = false;
};

// ===========================================================================
// GameOver (スコア表示 + ハイスコア保存 + Retry / Title)
// ===========================================================================
class GameOverScene final : public FScene2D {
public:
    GameOverScene(i32 score, bool win) noexcept : m_Score(score), m_Win(win) {}

    void OnReady() noexcept override {
        GetGame().SetClearColor(m_Win ? 0.05f : 0.10f, 0.07f, m_Win ? 0.10f : 0.06f);
        m_NewHigh = App(*this).SubmitScore(m_Score);   // 自己ベストなら INI 保存
        FInputMap& in = Services().Input();
        in.ClearAll();
        in.BindKey(kStart, EKey::Space);
        in.BindKey(kQuit,  EKey::Escape);
        m_Ui.Init();
        Services().Camera().SetPosition(FVec2{0.0f, 0.0f});
    }

    void OnEvent(const Event& e) noexcept override { m_Ui.HandleInput(e); }

    void OnTick(f32 dt) noexcept override;   // 遷移を含むので out-of-line

    void OnDrawHud(RenderContext& rc, FSpriteBatch& sb) noexcept override {
        BuildUi(rc.Width(), rc.Height());
        Font* big   = App(*this).TitleFont();
        Font* small = rc.HasFont() ? &rc.GetFont() : nullptr;
        const f32 cx = static_cast<f32>(rc.Width()) * 0.5f;

        DrawCentered(sb, big ? big : small, m_Win ? "YOU WIN!" : "GAME OVER",
                     cx, rc.Height() * 0.20f, m_Win ? kColAccent : kColText);

        char line[96];
        std::snprintf(line, sizeof(line), "SCORE: %d", m_Score);
        DrawCentered(sb, small, line, cx, rc.Height() * 0.33f, kColText);
        std::snprintf(line, sizeof(line), "HIGH SCORE: %d", App(*this).HighScore());
        DrawCentered(sb, small, line, cx, rc.Height() * 0.39f, kColAccent);
        if (m_NewHigh)
            DrawCentered(sb, small, "** NEW HIGH SCORE **", cx, rc.Height() * 0.45f,
                         FVec4{0.5f, 1.0f, 0.6f, 1.0f});

        m_Ui.Draw(rc);
        DrawCentered(sb, small, "Space: retry    Esc: title",
                     cx, rc.Height() - 40.0f, FVec4{0.7f, 0.75f, 0.85f, 1.0f});
    }

    void OnExit() noexcept override { m_Ui.Shutdown(); FScene2D::OnExit(); }

private:
    void BuildUi(u32 w, u32 h) noexcept {
        if (m_UiBuilt) return;
        m_UiBuilt = true;
        const f32 bw = 240.0f, bh = 52.0f;
        const f32 x  = (static_cast<f32>(w) - bw) * 0.5f;
        const f32 y0 = static_cast<f32>(h) * 0.56f;
        m_RetryBtn = m_Ui.AddButton("RETRY", FVec2{x, y0},               FVec2{bw, bh});
        m_TitleBtn = m_Ui.AddButton("TITLE", FVec2{x, y0 + bh + 16.0f},  FVec2{bw, bh});
    }

    i32  m_Score = 0;
    bool m_Win = false;
    bool m_NewHigh = false;
    FUiLayer m_Ui;
    u32  m_RetryBtn = 0, m_TitleBtn = 0;
    bool m_UiBuilt = false;
};

// ---- 相互参照する遷移メソッドを各クラス定義後に out-of-line で実装 ----------

TUniquePtr<Scene> FVerticalSliceGame::InitialScene() noexcept {
    return MakeUnique<TitleScene>();
}

void TitleScene::OnTick(f32 dt) noexcept {
    m_Ui.Tick(dt);
    FInputMap& in = Services().Input();
    if (m_Ui.IsButtonPressed(m_PlayBtn) || in.IsPressed(kStart)) {
        Scenes().ChangeScene(MakeUnique<PlayScene>());
    } else if (m_Ui.IsButtonPressed(m_QuitBtn) || in.IsPressed(kQuit)) {
        GetGame().Quit();
    }
}

void PlayScene::OnTick(f32 dt) noexcept {
    m_Time += dt;
    m_Ui.Tick(dt);
    FInputMap& in = Services().Input();

    if (m_State == EState::Paused) {
        if (m_Ui.IsButtonPressed(m_ResumeBtn) || in.IsPressed(kPause)) {
            SetPaused(false);
        } else if (m_Ui.IsButtonPressed(m_TitleBtn)) {
            Scenes().ChangeScene(MakeUnique<TitleScene>());
        }
        return;   // ポーズ中はゲーム更新を凍結
    }

    if (in.IsPressed(kPause)) { SetPaused(true); return; }

    // --- 移動 (Y-down): 入力を velocity にして FPhysicsBody2D の collide-and-slide に
    //     任せる。壁/水の AABB に当たると軸ごとに止まりつつ沿って滑る。位置は body が
    //     m_Root.UpdateTree (OnTick の後) で更新する。 ---
    f32 dx = in.Axis(kMoveX), dy = in.Axis(kMoveY);
    const f32 len = Sqrt(dx * dx + dy * dy);
    if (len > 0.0001f) { dx /= len; dy /= len; }
    if (m_PlayerBody) m_PlayerBody->velocity = FVec2{dx * kPlayerSpeed, dy * kPlayerSpeed};
    const FVec2 p = m_Player->Local().position;   // body が前フレームに更新した現在位置

    // --- コイン取得 ---
    for (u32 i = 0; i < kNumCoins; ++i) {
        if (m_CoinGot[i]) continue;
        const f32 ddx = p.x - m_CoinPos[i].x, ddy = p.y - m_CoinPos[i].y;
        if (ddx * ddx + ddy * ddy < kCoinRadius * kCoinRadius) {
            m_CoinGot[i] = true; ++m_Collected; m_Score += 10;
        }
    }

    // --- 勝敗 ---
    m_TimeLeft -= dt;
    if (m_Collected >= kNumCoins) { m_Score += static_cast<i32>(m_TimeLeft) * 2; EndGame(true); return; }
    if (m_TimeLeft <= 0.0f)       { m_TimeLeft = 0.0f; EndGame(false); return; }
}

void PlayScene::EndGame(bool win) noexcept {
    Scenes().ChangeScene(MakeUnique<GameOverScene>(m_Score, win));
}

void GameOverScene::OnTick(f32 dt) noexcept {
    m_Ui.Tick(dt);
    FInputMap& in = Services().Input();
    if (m_Ui.IsButtonPressed(m_RetryBtn) || in.IsPressed(kStart)) {
        Scenes().ChangeScene(MakeUnique<PlayScene>());
    } else if (m_Ui.IsButtonPressed(m_TitleBtn) || in.IsPressed(kQuit)) {
        Scenes().ChangeScene(MakeUnique<TitleScene>());
    }
}

} // namespace

ACS_GAME_MAIN(FVerticalSliceGame)

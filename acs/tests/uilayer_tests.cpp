// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — FUiLayer (Button/Text の state + ヒットテスト + クリック検出)
// (GPU 描画は伴わない純 logic テスト。HandleInput に合成 Event を流して検証する)
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/UiLayer.h"
#include "platform/Event.h"

using namespace acs;
using acs::game::FUiLayer;

namespace {

/** 合成 MouseMoved イベントを作る。 */
FEvent MouseMove(f32 x, f32 y) noexcept {
    FEvent e{};
    e.type = EEventType::MouseMoved;
    e.mouse_move.x = x;
    e.mouse_move.y = y;
    return e;
}

/** 合成 左ボタン押下イベントを作る。 */
FEvent MouseDown() noexcept {
    FEvent e{};
    e.type = EEventType::MouseButtonPressed;
    e.mouse_button.button = EMouseButton::Left;
    return e;
}

/** 合成 左ボタン解放イベントを作る。 */
FEvent MouseUp() noexcept {
    FEvent e{};
    e.type = EEventType::MouseButtonReleased;
    e.mouse_button.button = EMouseButton::Left;
    return e;
}

/** (x,y) で move → down → up を流してクリックを合成する。 */
void ClickAt(FUiLayer& ui, f32 x, f32 y) noexcept {
    ui.HandleInput(MouseMove(x, y));
    ui.HandleInput(MouseDown());
    ui.HandleInput(MouseUp());
}

} // namespace

// ---- 追加と件数・ハンドル発行 --------------------------------------------
ACS_TEST(UiLayer, AddAndCount) {
    FUiLayer ui;
    ui.Init();
    const u32 b = ui.AddButton("Play", FVec2{10, 10}, FVec2{100, 40});
    const u32 t = ui.AddText("Title", FVec2{10, 60});
    EXPECT_TRUE(b != 0);
    EXPECT_TRUE(t != 0);
    EXPECT_TRUE(b != t);
    EXPECT_EQ(ui.WidgetCount(), u32(2));
    ui.Shutdown();
}

// ---- Init 前の Add は 0 を返して無視 -------------------------------------
ACS_TEST(UiLayer, AddBeforeInitIgnored) {
    FUiLayer ui;
    const u32 b = ui.AddButton("x", FVec2{0, 0}, FVec2{10, 10});
    EXPECT_EQ(b, u32(0));
    EXPECT_EQ(ui.WidgetCount(), u32(0));
}

// ---- クリックで IsButtonPressed が 1 回だけ true (consume-on-read) --------
ACS_TEST(UiLayer, ClickDetectionConsumeOnce) {
    FUiLayer ui;
    ui.Init();
    const u32 b = ui.AddButton("OK", FVec2{10, 10}, FVec2{100, 40});
    EXPECT_FALSE(ui.IsButtonPressed(b));

    ClickAt(ui, 50, 30);                  // ボタン内
    EXPECT_TRUE(ui.IsButtonPressed(b));    // 1 回目 true
    EXPECT_FALSE(ui.IsButtonPressed(b));   // 2 回目は consume 済み
    ui.Shutdown();
}

// ---- ボタン外で離したらクリック不成立 ------------------------------------
ACS_TEST(UiLayer, ReleaseOutsideNoClick) {
    FUiLayer ui;
    ui.Init();
    const u32 b = ui.AddButton("OK", FVec2{10, 10}, FVec2{100, 40});
    ui.HandleInput(MouseMove(50, 30));
    ui.HandleInput(MouseDown());
    ui.HandleInput(MouseMove(500, 500));   // 大きく外す
    ui.HandleInput(MouseUp());
    EXPECT_FALSE(ui.IsButtonPressed(b));
    ui.Shutdown();
}

// ---- 重なり: 後から追加した方 (最前面) がヒット ---------------------------
ACS_TEST(UiLayer, TopmostHit) {
    FUiLayer ui;
    ui.Init();
    const u32 back  = ui.AddButton("back",  FVec2{0, 0}, FVec2{100, 100});
    const u32 front = ui.AddButton("front", FVec2{0, 0}, FVec2{100, 100});
    ClickAt(ui, 50, 50);
    EXPECT_TRUE(ui.IsButtonPressed(front));
    EXPECT_FALSE(ui.IsButtonPressed(back));
    ui.Shutdown();
}

// ---- 非表示ボタンはヒットしない ------------------------------------------
ACS_TEST(UiLayer, HiddenNotHit) {
    FUiLayer ui;
    ui.Init();
    const u32 b = ui.AddButton("OK", FVec2{10, 10}, FVec2{100, 40});
    ui.SetVisible(b, false);
    ClickAt(ui, 50, 30);
    EXPECT_FALSE(ui.IsButtonPressed(b));
    ui.Shutdown();
}

// ---- Text widget は押下対象外 --------------------------------------------
ACS_TEST(UiLayer, TextNotPressable) {
    FUiLayer ui;
    ui.Init();
    const u32 t = ui.AddText("hello", FVec2{10, 10});
    ClickAt(ui, 12, 12);
    EXPECT_FALSE(ui.IsButtonPressed(t));
    ui.Shutdown();
}

// ---- Remove / Clear -------------------------------------------------------
ACS_TEST(UiLayer, RemoveAndClear) {
    FUiLayer ui;
    ui.Init();
    const u32 a = ui.AddButton("a", FVec2{0,  0}, FVec2{10, 10});
    const u32 b = ui.AddButton("b", FVec2{20, 0}, FVec2{10, 10});
    EXPECT_EQ(ui.WidgetCount(), u32(2));

    ui.Remove(a);
    EXPECT_EQ(ui.WidgetCount(), u32(1));

    ClickAt(ui, 25, 5);                    // 残った b はクリック可能
    EXPECT_TRUE(ui.IsButtonPressed(b));

    ui.Clear();
    EXPECT_EQ(ui.WidgetCount(), u32(0));
    ui.Shutdown();
}

// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — CUiLayer (Button/Text の state + ヒットテスト + クリック検出)
// (GPU 描画は伴わない純 logic テスト。HandleInput に合成 Event を流して検証する)
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/UiLayer.h"
#include "platform/Event.h"

#include <cstring>

using namespace acs;
using acs::game::CUiLayer;

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
void ClickAt(CUiLayer& ui, f32 x, f32 y) noexcept {
    ui.HandleInput(MouseMove(x, y));
    ui.HandleInput(MouseDown());
    ui.HandleInput(MouseUp());
}

} // namespace

// ---- 追加と件数・ハンドル発行 --------------------------------------------
ACS_TEST(UiLayer, AddAndCount) {
    CUiLayer ui;
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
    CUiLayer ui;
    const u32 b = ui.AddButton("x", FVec2{0, 0}, FVec2{10, 10});
    EXPECT_EQ(b, u32(0));
    EXPECT_EQ(ui.WidgetCount(), u32(0));
}

// ---- クリックで IsButtonPressed が 1 回だけ true (consume-on-read) --------
ACS_TEST(UiLayer, ClickDetectionConsumeOnce) {
    CUiLayer ui;
    ui.Init();
    const u32 b = ui.AddButton("OK", FVec2{10, 10}, FVec2{100, 40});
    EXPECT_FALSE(ui.IsButtonPressed(b));

    ClickAt(ui, 50, 30);                  // ボタン内
    EXPECT_TRUE(ui.IsButtonPressed(b));    // 1 回目 true
    EXPECT_FALSE(ui.IsButtonPressed(b));   // 2 回目は consume 済み
    ui.Shutdown();
}

// ---- 明示 consume API も 1 クリックを 1 回だけ返す -----------------------
ACS_TEST(UiLayer, ConsumeButtonPressIsOneShot) {
    CUiLayer ui;
    ui.Init();
    const u32 b = ui.AddButton("OK", FVec2{10, 10}, FVec2{100, 40});

    ClickAt(ui, 50, 30);
    EXPECT_TRUE(ui.ConsumeButtonPress(b));
    EXPECT_FALSE(ui.ConsumeButtonPress(b));
    EXPECT_FALSE(ui.IsButtonPressed(b));
    ui.Shutdown();
}

// ---- ボタン外で離したらクリック不成立 ------------------------------------
ACS_TEST(UiLayer, ReleaseOutsideNoClick) {
    CUiLayer ui;
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
    CUiLayer ui;
    ui.Init();
    const u32 back  = ui.AddButton("back",  FVec2{0, 0}, FVec2{100, 100});
    const u32 front = ui.AddButton("front", FVec2{0, 0}, FVec2{100, 100});
    ClickAt(ui, 50, 50);
    EXPECT_TRUE(ui.IsButtonPressed(front));
    EXPECT_FALSE(ui.IsButtonPressed(back));
    ui.Shutdown();
}

// ---- 途中削除後も追加順を保ち、最前面判定を変えない ----------------------
ACS_TEST(UiLayer, RemovalPreservesTopmostOrder) {
    CUiLayer ui;
    ui.Init();
    const u32 back = ui.AddButton(
        "back", FVec2{0, 0}, FVec2{100, 100});
    const u32 middle = ui.AddButton(
        "middle", FVec2{0, 0}, FVec2{100, 100});
    const u32 front = ui.AddButton(
        "front", FVec2{0, 0}, FVec2{100, 100});
    ui.Remove(middle);

    ClickAt(ui, 50, 50);
    EXPECT_TRUE(ui.ConsumeButtonPress(front));
    EXPECT_FALSE(ui.ConsumeButtonPress(back));
    ui.Shutdown();
}

// ---- 非表示ボタンはヒットしない ------------------------------------------
ACS_TEST(UiLayer, HiddenNotHit) {
    CUiLayer ui;
    ui.Init();
    const u32 b = ui.AddButton("OK", FVec2{10, 10}, FVec2{100, 40});
    ui.SetVisible(b, false);
    ClickAt(ui, 50, 30);
    EXPECT_FALSE(ui.IsButtonPressed(b));
    ui.Shutdown();
}

// ---- Text widget は押下対象外 --------------------------------------------
ACS_TEST(UiLayer, TextNotPressable) {
    CUiLayer ui;
    ui.Init();
    const u32 t = ui.AddText("hello", FVec2{10, 10});
    ClickAt(ui, 12, 12);
    EXPECT_FALSE(ui.IsButtonPressed(t));
    ui.Shutdown();
}

// ---- Remove / Clear -------------------------------------------------------
ACS_TEST(UiLayer, RemoveAndClear) {
    CUiLayer ui;
    ui.Init();
    const u32 a = ui.AddButton("a", FVec2{0,  0}, FVec2{10, 10});
    const u32 b = ui.AddButton("b", FVec2{20, 0}, FVec2{10, 10});
    EXPECT_EQ(ui.WidgetCount(), u32(2));

    ui.Remove(a);
    EXPECT_EQ(ui.WidgetCount(), u32(1));

    ClickAt(ui, 25, 5);                    // 残った b はクリック可能
    EXPECT_TRUE(ui.IsButtonPressed(b));

    ui.HandleInput(MouseMove(25, 5));
    ui.HandleInput(MouseDown());
    ui.Clear();
    EXPECT_EQ(ui.WidgetCount(), u32(0));
    const u32 next = ui.AddButton(
        "next", FVec2{20, 0}, FVec2{10, 10});
    ui.HandleInput(MouseUp());
    EXPECT_FALSE(ui.ConsumeButtonPress(next));
    ui.Shutdown();
}

// ---- 文字列はコピー所有し、SetText は安全に差し替える -------------------
ACS_TEST(UiLayer, OwnsAndReplacesWidgetText) {
    CUiLayer ui;
    ui.Init();
    char caller_text[] = "Play";
    const u32 button = ui.AddButton(
        caller_text, FVec2{0, 0}, FVec2{100, 40});
    caller_text[0] = 'X';
    EXPECT_TRUE(ui.Text(button) != nullptr);
    EXPECT_TRUE(std::strcmp(ui.Text(button), "Play") == 0);

    EXPECT_TRUE(ui.SetText(button, "Continue"));
    EXPECT_TRUE(std::strcmp(ui.Text(button), "Continue") == 0);
    EXPECT_TRUE(ui.SetText(button, ui.Text(button)));
    EXPECT_TRUE(std::strcmp(ui.Text(button), "Continue") == 0);
    EXPECT_FALSE(ui.SetText(0u, "invalid"));

    const u32 empty = ui.AddText(nullptr, FVec2{0, 50});
    EXPECT_TRUE(ui.Text(empty) != nullptr);
    EXPECT_TRUE(std::strcmp(ui.Text(empty), "") == 0);
    ui.Remove(button);
    EXPECT_TRUE(ui.Text(button) == nullptr);
    ui.Shutdown();
}

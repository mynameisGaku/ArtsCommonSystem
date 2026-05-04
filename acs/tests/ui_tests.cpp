// =============================================================================
// ACS UI — Widget レイアウト + 入力ハンドリング
// (Renderer 不要、Layout 計算 + Observable 値伝搬の純 logic テスト)
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "ui/Widget.h"
#include "ui/Widgets.h"

using namespace acs;

// ---- StackPanel: 縦並び ----------------------------------------------------
ACS_TEST(Ui, StackPanelVertical) {
    StackPanel root;
    root.dir = StackDir::Vertical;
    root.padding = UiPadding{ 8, 8, 8, 8 };
    root.spacing = 4.0f;

    auto* a = root.Add<Label>("A");
    auto* b = root.Add<Label>("B");
    auto* c = root.Add<Label>("C");
    a->requested.h = 20;
    b->requested.h = 30;
    c->requested.h = 25;

    root.Layout(0, 0, 200, 200);

    EXPECT_EQ(a->rect.x, 8.0f);
    EXPECT_EQ(a->rect.y, 8.0f);
    EXPECT_EQ(a->rect.w, 184.0f);  // 200 - 8 - 8
    EXPECT_EQ(a->rect.h, 20.0f);

    EXPECT_EQ(b->rect.y, 32.0f);   // 8 + 20 + 4 (spacing)
    EXPECT_EQ(b->rect.h, 30.0f);

    EXPECT_EQ(c->rect.y, 66.0f);   // 32 + 30 + 4
    EXPECT_EQ(c->rect.h, 25.0f);
}

// ---- StackPanel: 横並び ----------------------------------------------------
ACS_TEST(Ui, StackPanelHorizontal) {
    StackPanel root;
    root.dir = StackDir::Horizontal;
    root.padding = UiPadding{ 0, 0, 0, 0 };
    root.spacing = 10.0f;

    auto* a = root.Add<Label>("A");
    auto* b = root.Add<Label>("B");
    a->requested.w = 50;
    b->requested.w = 80;

    root.Layout(0, 0, 300, 50);

    EXPECT_EQ(a->rect.x, 0.0f);
    EXPECT_EQ(a->rect.w, 50.0f);
    EXPECT_EQ(b->rect.x, 60.0f);    // 0 + 50 + 10
    EXPECT_EQ(b->rect.w, 80.0f);
}

// ---- HitTest 再帰: 子の方が優先される -------------------------------------
ACS_TEST(Ui, HitTestRecursive) {
    StackPanel root;
    root.padding = UiPadding{ 0, 0, 0, 0 };
    auto* btn = root.Add<Button>("Click");
    btn->requested.h = 30.0f;

    root.Layout(0, 0, 200, 100);
    EXPECT_EQ(btn->rect.x, 0.0f);
    EXPECT_EQ(btn->rect.y, 0.0f);

    Widget* hit = root.HitTestRecursive(50, 15);
    EXPECT_TRUE(hit == btn);

    Widget* miss = root.HitTestRecursive(50, 99);
    // ボタンの外、root のヒットエリアには入る
    EXPECT_TRUE(miss == &root);

    Widget* outside = root.HitTestRecursive(500, 500);
    EXPECT_TRUE(outside == nullptr);
}

// ---- Button: pointer down/up でクリック検出 -------------------------------
ACS_TEST(Ui, ButtonClick) {
    StackPanel root;
    root.padding = UiPadding{ 0, 0, 0, 0 };
    auto* btn = root.Add<Button>("OK");
    btn->requested.h = 30.0f;
    root.Layout(0, 0, 100, 50);

    int clicks = 0;
    btn->clicked.Subscribe(
        [](const bool& v, void* user){
            if (v) ++(*static_cast<int*>(user));
        },
        &clicks);

    btn->OnPointerDown(50, 15);
    EXPECT_TRUE(btn->pressed);

    btn->OnPointerUp(50, 15);
    EXPECT_FALSE(btn->pressed);
    EXPECT_EQ(clicks, 1);
}

// ---- Button: pointer up が rect 外なら click 発生せず ---------------------
ACS_TEST(Ui, ButtonReleaseOutsideNoClick) {
    StackPanel root;
    root.padding = UiPadding{ 0, 0, 0, 0 };
    auto* btn = root.Add<Button>("OK");
    btn->requested.h = 30.0f;
    root.Layout(0, 0, 100, 50);

    int clicks = 0;
    btn->clicked.Subscribe(
        [](const bool& v, void* user){
            if (v) ++(*static_cast<int*>(user));
        },
        &clicks);

    btn->OnPointerDown(50, 15);
    btn->OnPointerUp(500, 500);   // 大きく外す
    EXPECT_EQ(clicks, 0);
}

// ---- Slider: ドラッグで value 更新 ----------------------------------------
ACS_TEST(Ui, SliderValueUpdate) {
    StackPanel root;
    root.padding = UiPadding{ 0, 0, 0, 0 };
    auto* sl = root.Add<Slider>(0.0f, 100.0f);
    sl->requested.h = 24.0f;
    root.Layout(0, 0, 200, 50);

    sl->OnPointerDown(0, 12);     // 左端
    EXPECT_EQ(sl->value.Get(), 0.0f);

    sl->OnPointerMove(100, 12);   // 中央 (rect.w=200)
    EXPECT_EQ(sl->value.Get(), 50.0f);

    sl->OnPointerMove(200, 12);   // 右端
    EXPECT_EQ(sl->value.Get(), 100.0f);

    // pointer up したら drag 終了
    sl->OnPointerUp(200, 12);
    EXPECT_FALSE(sl->pressed);

    // pressed=false なので move 無視
    sl->OnPointerMove(50, 12);
    EXPECT_EQ(sl->value.Get(), 100.0f);
}

// ---- Checkbox: 値トグル --------------------------------------------------
ACS_TEST(Ui, CheckboxToggle) {
    StackPanel root;
    root.padding = UiPadding{ 0, 0, 0, 0 };
    auto* cb = root.Add<Checkbox>("Test");
    cb->requested.h = 24.0f;
    root.Layout(0, 0, 100, 30);

    EXPECT_FALSE(cb->checked.Get());

    cb->OnPointerUp(50, 12);
    EXPECT_TRUE(cb->checked.Get());

    cb->OnPointerUp(50, 12);
    EXPECT_FALSE(cb->checked.Get());

    // rect 外は無視
    cb->OnPointerUp(500, 500);
    EXPECT_FALSE(cb->checked.Get());
}

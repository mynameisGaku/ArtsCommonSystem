// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS UI — Widget レイアウト + 入力ハンドリング
// (FRenderer 不要、Layout 計算 + Observable 値伝搬の純 logic テスト)
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "ui/Widget.h"
#include "ui/Widgets.h"

using namespace acs;

// ---- StackPanel: 縦並び ----------------------------------------------------
ACS_TEST(Ui, StackPanelVertical) {
    StackPanel root;
    root.dir = EStackDir::Vertical;
    root.padding = FUiPadding{ 8, 8, 8, 8 };
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
    root.dir = EStackDir::Horizontal;
    root.padding = FUiPadding{ 0, 0, 0, 0 };
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
    root.padding = FUiPadding{ 0, 0, 0, 0 };
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
    root.padding = FUiPadding{ 0, 0, 0, 0 };
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
    root.padding = FUiPadding{ 0, 0, 0, 0 };
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
    root.padding = FUiPadding{ 0, 0, 0, 0 };
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
    root.padding = FUiPadding{ 0, 0, 0, 0 };
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

// ---- TextInput: 文字入力で末尾追加 ----------------------------------------
ACS_TEST(Ui, TextInputTyping) {
    TextInput ti;
    ti.focused = true;
    ti.OnTextInput('H');
    ti.OnTextInput('i');
    ti.OnTextInput('!');
    EXPECT_TRUE(ti.text.Get() == FStringView("Hi!"));
    EXPECT_EQ(ti.text.Get().Size(), usize(3));
}

// ---- TextInput: focus 外 / 制御文字 / 非 ASCII は無視 ---------------------
ACS_TEST(Ui, TextInputFiltering) {
    TextInput ti;
    ti.OnTextInput('X');            // focus 外 → 無視
    EXPECT_EQ(ti.text.Get().Size(), usize(0));

    ti.focused = true;
    ti.OnTextInput('\n');           // 制御文字 (0x0A) → 無視
    ti.OnTextInput(0x3042);         // 'あ' (非 ASCII) → 無視
    ti.OnTextInput('A');
    EXPECT_TRUE(ti.text.Get() == FStringView("A"));
}

// ---- TextInput: Backspace で末尾削除 (空でも安全) -------------------------
ACS_TEST(Ui, TextInputBackspace) {
    TextInput ti;
    ti.focused = true;
    ti.OnTextInput('a');
    ti.OnTextInput('b');
    ti.OnTextInput('c');
    ti.OnKey(0x08, true);           // Backspace → "ab"
    EXPECT_TRUE(ti.text.Get() == FStringView("ab"));

    ti.OnKey(0x08, false);          // release は無視 → "ab" のまま
    EXPECT_TRUE(ti.text.Get() == FStringView("ab"));

    ti.OnKey(0x08, true);
    ti.OnKey(0x08, true);
    EXPECT_EQ(ti.text.Get().Size(), usize(0));

    ti.OnKey(0x08, true);           // 空で Backspace → no-op (クラッシュ無し)
    EXPECT_EQ(ti.text.Get().Size(), usize(0));
}

// ---- Anchor: ComputeAnchoredRect の基本式 ---------------------------------
ACS_TEST(Ui, AnchorComputeRect) {
    const FUiRect parent{ 100, 50, 800, 600 };

    // フルストレッチ (既定): 親そのまま。
    {
        FUiAnchor a;  // min(0,0) max(1,1) offset0
        const FUiRect r = ComputeAnchoredRect(parent, a);
        EXPECT_EQ(r.x, 100.0f); EXPECT_EQ(r.y, 50.0f);
        EXPECT_EQ(r.w, 800.0f); EXPECT_EQ(r.h, 600.0f);
    }
    // 四辺マージン付きストレッチ。
    {
        const FUiRect r = ComputeAnchoredRect(parent, FUiAnchor::Stretch(16, 8, 16, 24));
        EXPECT_EQ(r.x, 116.0f);             // 100 + 16
        EXPECT_EQ(r.y, 58.0f);              // 50 + 8
        EXPECT_EQ(r.w, 800.0f - 16 - 16);   // 768
        EXPECT_EQ(r.h, 600.0f - 8 - 24);    // 568
    }
}

// ---- Anchor: 点アンカー (角固定 + 中央) -----------------------------------
ACS_TEST(Ui, AnchorPointCorners) {
    const FUiRect parent{ 0, 0, 1000, 800 };

    // 右上 (1,0) に 120x32、内側へ ox=-128, oy=8。
    {
        const FUiRect r = ComputeAnchoredRect(parent, FUiAnchor::Point({1, 0}, 120, 32, -128, 8));
        EXPECT_EQ(r.x, 1000.0f - 128);  // 872
        EXPECT_EQ(r.y, 8.0f);
        EXPECT_EQ(r.w, 120.0f);
        EXPECT_EQ(r.h, 32.0f);
    }
    // 中央 100x60。
    {
        const FUiRect r = ComputeAnchoredRect(parent, FUiAnchor::Centered(100, 60));
        EXPECT_EQ(r.x, 500.0f - 50);    // 450
        EXPECT_EQ(r.y, 400.0f - 30);    // 370
        EXPECT_EQ(r.w, 100.0f);
        EXPECT_EQ(r.h, 60.0f);
    }
}

// ---- AnchorPanel: 子をアンカーで配置し、リサイズで追従する ---------------
ACS_TEST(Ui, AnchorPanelResponsive) {
    AnchorPanel hud;
    auto* tl  = hud.Add<Label>("tl");
    auto* br  = hud.Add<Label>("br");
    auto* bar = hud.Add<Container>();
    tl->anchor  = FUiAnchor::Point({0, 0}, 80, 24, 8, 8);     // 左上固定
    br->anchor  = FUiAnchor::Point({1, 1}, 80, 24, -88, -32); // 右下固定
    bar->anchor = FUiAnchor::Stretch(0, 0, 0, 40);            // 上端を左右いっぱい (高さは下40除く)

    // 1280x720 でレイアウト。
    hud.Layout(0, 0, 1280, 720);
    EXPECT_EQ(tl->rect.x, 8.0f);    EXPECT_EQ(tl->rect.y, 8.0f);
    EXPECT_EQ(br->rect.x, 1280.0f - 88);   // 1192
    EXPECT_EQ(br->rect.y, 720.0f - 32);    // 688
    EXPECT_EQ(bar->rect.x, 0.0f);
    EXPECT_EQ(bar->rect.w, 1280.0f);
    EXPECT_EQ(bar->rect.h, 720.0f - 40);   // 680

    // 画面を 1920x1080 にリサイズ → 右下/ストレッチが追従、左上は不動。
    hud.Layout(0, 0, 1920, 1080);
    EXPECT_EQ(tl->rect.x, 8.0f);    EXPECT_EQ(tl->rect.y, 8.0f);   // 左上は不変
    EXPECT_EQ(br->rect.x, 1920.0f - 88);   // 1832 追従
    EXPECT_EQ(br->rect.y, 1080.0f - 32);   // 1048 追従
    EXPECT_EQ(bar->rect.w, 1920.0f);       // 横いっぱい追従
    EXPECT_EQ(bar->rect.h, 1080.0f - 40);  // 1040
}

// ---- AnchorPanel: 非 visible な子は配置スキップ ---------------------------
ACS_TEST(Ui, AnchorPanelSkipsHidden) {
    AnchorPanel p;
    auto* a = p.Add<Label>("a");
    a->anchor = FUiAnchor::Point({0, 0}, 50, 50, 10, 10);
    a->visible = false;
    a->rect = { -1, -1, -1, -1 };   // 番兵

    p.Layout(0, 0, 400, 400);
    // visible=false なので Layout されず rect は番兵のまま。
    EXPECT_EQ(a->rect.x, -1.0f);
}

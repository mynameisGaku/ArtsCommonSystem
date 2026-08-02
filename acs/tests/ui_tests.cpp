// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS UI — Widget レイアウト + 入力ハンドリング
// (CRenderer 不要、Layout 計算 + Observable 値伝搬の純 logic テスト)
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "ui/Widget.h"
#include "ui/Widgets.h"
#include "ui/UiRenderer.h"
#include "foundation/Move.h"
#include "memory/SystemAllocator.h"
#include "platform/Event.h"
#include "platform/Input.h"
#include "platform/InputCodes.h"

#include <new>

using namespace acs;

namespace {

/** TextInput の transactional editing を allocation failure 下で検証する backing。 */
class FUiSwitchableFailAllocator final : public IAllocator {
public:
    explicit FUiSwitchableFailAllocator(IAllocator& backing) noexcept
        : m_Backing(&backing) {}

    void SetFailing(bool failing) noexcept { m_Failing = failing; }

    void* Alloc(usize size, usize alignment, FSourceLoc location) noexcept override {
        return m_Failing ? nullptr : m_Backing->Alloc(size, alignment, location);
    }

    void Free(void* pointer) noexcept override {
        m_Backing->Free(pointer);
    }

private:
    IAllocator* m_Backing = nullptr;
    bool m_Failing = false;
};

FEvent MakeUiKeyEvent(EKey key, bool down) noexcept {
    FEvent event{};
    event.type = down ? EEventType::KeyPressed : EEventType::KeyReleased;
    event.key.key = key;
    event.key.repeat = false;
    return event;
}

void FeedUiKey(EKey key, bool down) noexcept {
    CInput::OnEvent(MakeUiKeyEvent(key, down));
}

void FeedUiMouseMove(f32 x, f32 y) noexcept {
    FEvent event{};
    event.type = EEventType::MouseMoved;
    event.mouse_move.x = x;
    event.mouse_move.y = y;
    CInput::OnEvent(event);
}

void FeedUiLeftMouse(bool down) noexcept {
    FEvent event{};
    event.type = down ? EEventType::MouseButtonPressed
                      : EEventType::MouseButtonReleased;
    event.mouse_button.button = EMouseButton::Left;
    CInput::OnEvent(event);
}

void FeedUiChar(u32 codepoint) noexcept {
    FEvent event{};
    event.type = EEventType::CharInput;
    event.char_input.codepoint = codepoint;
    CInput::OnEvent(event);
}

/**
 * CInput はプロセス共有なので、UI Dispatch テストが使う入力を解放済みの
 * フレーム境界へ揃える。
 */
void ResetUiInputFeed() noexcept {
    static constexpr EKey kKeys[] = {
        EKey::A,
        EKey::Backspace,
        EKey::Left,
        EKey::Right,
        EKey::Home,
        EKey::End,
        EKey::LeftShift,
        EKey::RightShift,
        EKey::LeftCtrl,
        EKey::RightCtrl,
        EKey::LeftAlt,
        EKey::RightAlt,
        EKey::LeftSuper,
        EKey::RightSuper,
    };
    for (EKey key : kKeys) FeedUiKey(key, false);
    FeedUiLeftMouse(false);
    CInput::Update();
}

/** root 自身をクリックして CUiInput の focus を与え、mouse release まで進める。 */
void FocusUiRoot(CUiInput& input, AWidget& root) noexcept {
    FeedUiMouseMove(8.0f, 8.0f);
    FeedUiLeftMouse(true);
    input.Dispatch(root);
    CInput::Update();
    FeedUiLeftMouse(false);
    input.Dispatch(root);
    CInput::Update();
}

class FUiMutableCallbackRoot final : public AContainer {
public:
    void RemoveAllChildren() noexcept { m_Children.Clear(); }

    void RemoveFirstChild() noexcept {
        if (m_Children.Size() > 0) m_Children.RemoveAt(0);
    }
};

enum class EUiRemovalCallback : u8 {
    PointerMove,
    PointerDown,
    PointerUp,
    TextInput,
    Key,
};

class FUiRemovingCallbackWidget final : public AWidget {
public:
    FUiRemovingCallbackWidget(FUiMutableCallbackRoot& owner,
                              EUiRemovalCallback callback,
                              bool remove_self,
                              i32& call_count) noexcept
        : m_Owner(&owner),
          m_Callback(callback),
          m_RemoveSelf(remove_self),
          m_CallCount(&call_count) {}

    void OnPointerMove(f32, f32) noexcept override {
        RemoveFromCallback(EUiRemovalCallback::PointerMove);
    }

    void OnPointerDown(f32, f32) noexcept override {
        RemoveFromCallback(EUiRemovalCallback::PointerDown);
    }

    void OnPointerUp(f32, f32) noexcept override {
        RemoveFromCallback(EUiRemovalCallback::PointerUp);
    }

    void OnTextInput(u32) noexcept override {
        RemoveFromCallback(EUiRemovalCallback::TextInput);
    }

    void OnKey(i32, bool pressed_) noexcept override {
        if (pressed_) RemoveFromCallback(EUiRemovalCallback::Key);
    }

private:
    void RemoveFromCallback(EUiRemovalCallback callback) noexcept {
        if (m_Fired || callback != m_Callback) return;

        FUiMutableCallbackRoot* const owner = m_Owner;
        i32* const call_count = m_CallCount;
        const bool remove_self = m_RemoveSelf;
        m_Fired = true;
        ++(*call_count);
        if (remove_self) owner->RemoveAllChildren();
        else owner->RemoveFirstChild();
        // remove_self の場合はここで *this が破棄済み。member へ再接触しない。
    }

    FUiMutableCallbackRoot* m_Owner = nullptr;
    EUiRemovalCallback m_Callback = EUiRemovalCallback::PointerMove;
    bool m_RemoveSelf = false;
    bool m_Fired = false;
    i32* m_CallCount = nullptr;
};

} // namespace

// ---- StackPanel: 縦並び ----------------------------------------------------
ACS_TEST(Ui, StackPanelVertical) {
    AStackPanel root;
    root.dir = EStackDir::Vertical;
    root.padding = FUiPadding{ 8, 8, 8, 8 };
    root.spacing = 4.0f;

    auto* a = root.Add<ALabel>("A");
    auto* b = root.Add<ALabel>("B");
    auto* c = root.Add<ALabel>("C");
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
    AStackPanel root;
    root.dir = EStackDir::Horizontal;
    root.padding = FUiPadding{ 0, 0, 0, 0 };
    root.spacing = 10.0f;

    auto* a = root.Add<ALabel>("A");
    auto* b = root.Add<ALabel>("B");
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
    AStackPanel root;
    root.padding = FUiPadding{ 0, 0, 0, 0 };
    auto* btn = root.Add<AButton>("Click");
    btn->requested.h = 30.0f;

    root.Layout(0, 0, 200, 100);
    EXPECT_EQ(btn->rect.x, 0.0f);
    EXPECT_EQ(btn->rect.y, 0.0f);

    AWidget* hit = root.HitTestRecursive(50, 15);
    EXPECT_TRUE(hit == btn);

    AWidget* miss = root.HitTestRecursive(50, 99);
    // ボタンの外、root のヒットエリアには入る
    EXPECT_TRUE(miss == &root);

    AWidget* outside = root.HitTestRecursive(500, 500);
    EXPECT_TRUE(outside == nullptr);
}

// ---- Button: pointer down/up でクリック検出 -------------------------------
ACS_TEST(Ui, ButtonClick) {
    AStackPanel root;
    root.padding = FUiPadding{ 0, 0, 0, 0 };
    auto* btn = root.Add<AButton>("OK");
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
    AStackPanel root;
    root.padding = FUiPadding{ 0, 0, 0, 0 };
    auto* btn = root.Add<AButton>("OK");
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
    AStackPanel root;
    root.padding = FUiPadding{ 0, 0, 0, 0 };
    auto* sl = root.Add<ASlider>(0.0f, 100.0f);
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
    AStackPanel root;
    root.padding = FUiPadding{ 0, 0, 0, 0 };
    auto* cb = root.Add<ACheckbox>("Test");
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
    ATextInput ti;
    ti.focused = true;
    ti.OnTextInput('H');
    ti.OnTextInput('i');
    ti.OnTextInput('!');
    EXPECT_TRUE(ti.text.Get() == FStringView("Hi!"));
    EXPECT_EQ(ti.text.Get().Size(), usize(3));
}

// ---- TextInput: focus 外と不正 scalar は無視、正規 Unicode は UTF-8 化 ------------
ACS_TEST(Ui, TextInputFiltering) {
    ATextInput ti;
    ti.OnTextInput('X');            // focus 外 → 無視
    EXPECT_EQ(ti.text.Get().Size(), usize(0));

    ti.focused = true;
    ti.OnTextInput('\n');           // 制御文字 (0x0A) → 無視
    ti.OnTextInput(0x7F);           // DEL → 無視
    ti.OnTextInput(0xD800);         // UTF-16 surrogate → 無視
    ti.OnTextInput(0x110000);       // Unicode 範囲外 → 無視
    ti.OnTextInput(0x2028);         // line separator → 1 行入力では無視
    ti.OnTextInput(0x3042);         // 'あ' → E3 81 82
    ti.OnTextInput(0xFFFD);         // 正規の replacement character は受理
    ti.OnTextInput('A');
    EXPECT_TRUE(ti.text.Get() == FStringView("\xE3\x81\x82\xEF\xBF\xBD" "A"));
    EXPECT_EQ(ti.CursorByteOffset(), usize(7));
}

// ---- TextInput: Backspace で末尾削除 (空でも安全) -------------------------
ACS_TEST(Ui, TextInputBackspace) {
    ATextInput ti;
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

// ---- TextInput: UTF-8 cursor 移動・中間挿入・削除は codepoint 単位 ---------------
ACS_TEST(Ui, TextInputUtf8CursorEditing) {
    ATextInput ti;
    ti.focused = true;
    EXPECT_TRUE(ti.TryInsertCodepoint('A'));
    EXPECT_TRUE(ti.TryInsertCodepoint(0x3042));   // あ (3 bytes)
    EXPECT_TRUE(ti.TryInsertCodepoint(0x1F600));  // 😀 (4 bytes)
    EXPECT_TRUE(ti.text.Get() == FStringView("A\xE3\x81\x82\xF0\x9F\x98\x80"));
    EXPECT_EQ(ti.CursorByteOffset(), usize(8));

    ti.OnKey(0x25, true); // Left: emoji の手前
    EXPECT_EQ(ti.CursorByteOffset(), usize(4));
    EXPECT_TRUE(ti.TryInsertCodepoint('B'));
    EXPECT_TRUE(ti.text.Get() == FStringView("A\xE3\x81\x82" "B\xF0\x9F\x98\x80"));
    EXPECT_EQ(ti.CursorByteOffset(), usize(5));

    EXPECT_TRUE(ti.TryEraseBeforeCursor()); // B
    EXPECT_EQ(ti.CursorByteOffset(), usize(4));
    EXPECT_TRUE(ti.TryEraseBeforeCursor()); // あ (1 byte ではなく 3 bytes)
    EXPECT_TRUE(ti.text.Get() == FStringView("A\xF0\x9F\x98\x80"));
    EXPECT_EQ(ti.CursorByteOffset(), usize(1));

    EXPECT_TRUE(ti.TryEraseAtCursor()); // emoji (4 bytes)
    EXPECT_TRUE(ti.text.Get() == FStringView("A"));
    EXPECT_EQ(ti.CursorByteOffset(), usize(1));
}

// ---- TextInput: Home/End/Left/Right/Delete と key release -----------------------
ACS_TEST(Ui, TextInputNavigationAndDelete) {
    ATextInput ti;
    ti.focused = true;
    ti.OnTextInput('A');
    ti.OnTextInput(0x3042); // あ
    ti.OnTextInput('B');

    ti.OnKey(0x24, true);   // 先頭
    EXPECT_EQ(ti.CursorByteOffset(), usize(0));
    ti.OnKey(0x27, true);   // A の右へ移動
    EXPECT_EQ(ti.CursorByteOffset(), usize(1));
    ti.OnKey(0x7F, false);  // Delete の解放は無視する
    EXPECT_TRUE(ti.text.Get() == FStringView("A\xE3\x81\x82" "B"));
    ti.OnKey(0x7F, true);   // Delete あ
    EXPECT_TRUE(ti.text.Get() == FStringView("AB"));
    EXPECT_EQ(ti.CursorByteOffset(), usize(1));

    ti.OnKey(0x23, true);   // 末尾
    EXPECT_EQ(ti.CursorByteOffset(), usize(2));
    ti.OnKey(0x25, true);   // 左
    ti.OnTextInput('X');
    EXPECT_TRUE(ti.text.Get() == FStringView("AXB"));

    ti.focused = false;
    ti.OnKey(0x08, true);
    EXPECT_TRUE(ti.text.Get() == FStringView("AXB"));
}

// ---- TextInput: cursor API は codepoint 途中を拒否し外部更新後も正規化 -----------
ACS_TEST(Ui, TextInputCursorBoundaryAndExternalTextChange) {
    ATextInput ti;
    ti.text.Set(FString{"A\xE3\x81\x82" "B"});
    EXPECT_EQ(ti.CursorByteOffset(), usize(5)); // default は末尾追従

    EXPECT_FALSE(ti.TrySetCursorByteOffset(2)); // あ の continuation byte
    EXPECT_EQ(ti.CursorByteOffset(), usize(5));
    EXPECT_TRUE(ti.TrySetCursorByteOffset(4));  // あ の直後
    EXPECT_EQ(ti.CursorByteOffset(), usize(4));

    ti.text.Set(FString{"Z"});                 // binding 側が短縮
    EXPECT_EQ(ti.CursorByteOffset(), usize(1)); // 範囲内の境界へ clamp

    ti.OnPointerDown(0.0f, 0.0f);
    EXPECT_TRUE(ti.focused);
    EXPECT_EQ(ti.CursorByteOffset(), usize(1)); // click は末尾へ
}

// ---- TextInput: configurable byte cap は multibyte を途中で受け入れない ----------
ACS_TEST(Ui, TextInputMaxBytesIsAtomic) {
    ATextInput ti;
    ti.focused = true;
    EXPECT_TRUE(ti.TrySetMaxTextBytes(4));
    EXPECT_TRUE(ti.TryInsertCodepoint('A'));
    EXPECT_TRUE(ti.TryInsertCodepoint(0x3042)); // 合計 4 bytes
    EXPECT_FALSE(ti.TryInsertCodepoint('B'));
    EXPECT_TRUE(ti.text.Get() == FStringView("A\xE3\x81\x82"));
    EXPECT_EQ(ti.CursorByteOffset(), usize(4));

    EXPECT_FALSE(ti.TrySetMaxTextBytes(ATextInput::kHardMaxTextBytes + 1u));
    EXPECT_EQ(ti.MaxTextBytes(), usize(4));
    EXPECT_TRUE(ti.TrySetMaxTextBytes(0));
    EXPECT_FALSE(ti.TryInsertCodepoint('C'));
    EXPECT_TRUE(ti.TryEraseBeforeCursor()); // cap を下げても削除は可能
    EXPECT_TRUE(ti.text.Get() == FStringView("A"));
}

// ---- TextInput: external binding 正規化も失敗時は commit しない -----------------
ACS_TEST(Ui, TextInputFailedEditDoesNotCommitStagedCursorNormalization) {
    ATextInput ti;
    ti.text.Set(FString{"ABC"});
    EXPECT_TRUE(ti.TrySetCursorByteOffset(2));
    EXPECT_TRUE(ti.TrySetMaxTextBytes(1));

    // Binding 側で短縮すると、編集に使う cursor 候補は 1 へ clamp される。
    // 上限違反で編集自体が失敗した場合、その候補を内部状態へ commit しない。
    ti.text.Set(FString{"Z"});
    EXPECT_FALSE(ti.TryInsertCodepoint('Q'));
    EXPECT_TRUE(ti.text.Get() == FStringView("Z"));

    // 元の長さへ戻すと、失敗前の raw cursor=2 が保存されていたことを確認できる。
    ti.text.Set(FString{"ABC"});
    EXPECT_EQ(ti.CursorByteOffset(), usize(2));
}

// ---- TextInput: OOM 時は text と cursor をともに変更しない -----------------------
ACS_TEST(Ui, TextInputAllocationFailurePreservesState) {
    CSystemAllocator backing;
    FUiSwitchableFailAllocator allocator(backing);
    ATextInput ti;
    FString initial(allocator);
    EXPECT_TRUE(initial.TryAppend("abcdefghijklmnopqrstuvwxyzABCDEF")); // 32 bytes
    ti.text.Set(Move(initial));
    ti.focused = true;
    ti.MoveCursorToStart();

    allocator.SetFailing(true);
    EXPECT_FALSE(ti.TryInsertCodepoint('Z'));
    EXPECT_TRUE(ti.text.Get() == FStringView("abcdefghijklmnopqrstuvwxyzABCDEF"));
    EXPECT_EQ(ti.CursorByteOffset(), usize(0));

    EXPECT_FALSE(ti.TryEraseAtCursor()); // 31-byte の結果にも heap 確保が必要
    EXPECT_TRUE(ti.text.Get() == FStringView("abcdefghijklmnopqrstuvwxyzABCDEF"));
    EXPECT_EQ(ti.CursorByteOffset(), usize(0));
    allocator.SetFailing(false);
}

// ---- TextInput: programmatic selection は UTF-8 境界を維持して置換する -----------
ACS_TEST(Ui, TextInputSelectionReplacementAndBoundaries) {
    ATextInput ti;
    ti.focused = true;
    ti.text.Set(FString{"A\xE3\x81\x82\xF0\x9F\x98\x80" "B"});

    // 「あ」と絵文字を選択する。continuation byte を端点にする指定は全状態を維持する。
    EXPECT_TRUE(ti.TrySetSelection(1, 8));
    EXPECT_TRUE(ti.HasSelection());
    EXPECT_EQ(ti.SelectionStart(), usize(1));
    EXPECT_EQ(ti.SelectionEnd(), usize(8));
    EXPECT_EQ(ti.CursorByteOffset(), usize(8));
    EXPECT_FALSE(ti.TrySetSelection(2, 8));
    EXPECT_TRUE(ti.HasSelection());
    EXPECT_EQ(ti.SelectionStart(), usize(1));
    EXPECT_EQ(ti.SelectionEnd(), usize(8));

    EXPECT_TRUE(ti.TryInsertCodepoint('X'));
    EXPECT_TRUE(ti.text.Get() == FStringView("AXB"));
    EXPECT_EQ(ti.CursorByteOffset(), usize(2));
    EXPECT_FALSE(ti.HasSelection());

    ti.SelectAll();
    EXPECT_TRUE(ti.HasSelection());
    EXPECT_EQ(ti.SelectionStart(), usize(0));
    EXPECT_EQ(ti.SelectionEnd(), usize(3));
    EXPECT_TRUE(ti.TryInsertCodepoint(0x1F600));
    EXPECT_TRUE(ti.text.Get() == FStringView("\xF0\x9F\x98\x80"));
    EXPECT_FALSE(ti.HasSelection());

    // 現在値より小さい cap でも、選択置換後の最終値が収まるなら縮小編集できる。
    EXPECT_TRUE(ti.TrySetMaxTextBytes(1));
    ti.SelectAll();
    EXPECT_TRUE(ti.TryInsertCodepoint('Z'));
    EXPECT_TRUE(ti.text.Get() == FStringView("Z"));
}

// ---- TextInput: Backspace/Delete は選択範囲を優先して削除する -------------------
ACS_TEST(Ui, TextInputSelectionDeletionAndNavigation) {
    ATextInput ti;
    ti.focused = true;
    ti.text.Set(FString{"A\xE3\x81\x82" "BC"});

    EXPECT_TRUE(ti.TrySetSelection(1, 4)); // 「あ」
    ti.OnKey(0x08, true);
    EXPECT_TRUE(ti.text.Get() == FStringView("ABC"));
    EXPECT_EQ(ti.CursorByteOffset(), usize(1));
    EXPECT_FALSE(ti.HasSelection());

    EXPECT_TRUE(ti.TrySetSelection(1, 2)); // B
    ti.OnKey(0x7F, true);
    EXPECT_TRUE(ti.text.Get() == FStringView("AC"));
    EXPECT_EQ(ti.CursorByteOffset(), usize(1));

    ti.SelectAll();
    ti.OnKey(0x08, true);
    EXPECT_EQ(ti.text.Get().Size(), usize(0));
    EXPECT_FALSE(ti.HasSelection());

    ti.text.Set(FString{"A\xE3\x81\x82" "B"});
    EXPECT_TRUE(ti.TrySetSelection(1, 4));
    ti.MoveCursorLeft();
    EXPECT_EQ(ti.CursorByteOffset(), usize(1));
    EXPECT_FALSE(ti.HasSelection());

    EXPECT_TRUE(ti.TrySetSelection(1, 4));
    ti.MoveCursorRight();
    EXPECT_EQ(ti.CursorByteOffset(), usize(4));
    EXPECT_FALSE(ti.HasSelection());
}

// ---- TextInput: Shift 移動は UTF-8 境界を保って選択を拡張・折り畳む ----------
ACS_TEST(Ui, TextInputShiftSelectionUsesUtf8Boundaries) {
    ATextInput ti;
    ti.focused = true;
    ti.text.Set(FString{"A\xE3\x81\x82\xF0\x9F\x98\x80" "B"});

    int notifications = 0;
    ti.text.Subscribe(
        [](const FString&, void* user) {
            ++(*static_cast<int*>(user));
        },
        &notifications);

    FUiKeyModifiers shift;
    shift.bShift = true;

    ti.OnKey(0x25, true, shift);  // B の手前
    EXPECT_EQ(ti.CursorByteOffset(), usize(8));
    EXPECT_EQ(ti.SelectionStart(), usize(8));
    EXPECT_EQ(ti.SelectionEnd(), usize(9));

    ti.OnKey(0x25, false, shift); // 解放では選択を変更しない
    EXPECT_EQ(ti.CursorByteOffset(), usize(8));
    ti.OnKey(0x25, true, shift);  // 絵文字を1コードポイントで越える
    EXPECT_EQ(ti.CursorByteOffset(), usize(4));
    EXPECT_EQ(ti.SelectionStart(), usize(4));
    EXPECT_EQ(ti.SelectionEnd(), usize(9));

    ti.OnKey(0x27, true, shift);
    EXPECT_EQ(ti.CursorByteOffset(), usize(8));
    EXPECT_EQ(ti.SelectionStart(), usize(8));
    EXPECT_EQ(ti.SelectionEnd(), usize(9));

    // 修飾キーなし Right は従来どおり選択末尾へ折り畳む。
    ti.OnKey(0x27, true);
    EXPECT_EQ(ti.CursorByteOffset(), usize(9));
    EXPECT_FALSE(ti.HasSelection());

    ti.MoveCursorToStart();
    ti.OnKey(0x27, true, shift);  // A
    ti.OnKey(0x27, true, shift);  // あ
    EXPECT_EQ(ti.CursorByteOffset(), usize(4));
    EXPECT_EQ(ti.SelectionStart(), usize(0));
    EXPECT_EQ(ti.SelectionEnd(), usize(4));

    ti.OnKey(0x24, true, shift);  // Shift+Home で anchor へ戻る
    EXPECT_EQ(ti.CursorByteOffset(), usize(0));
    EXPECT_FALSE(ti.HasSelection());
    ti.OnKey(0x23, true, shift);  // Shift+End で全体を選択
    EXPECT_EQ(ti.CursorByteOffset(), usize(9));
    EXPECT_EQ(ti.SelectionStart(), usize(0));
    EXPECT_EQ(ti.SelectionEnd(), usize(9));

    // 選択操作は text を変更せず、observable 通知も発生させない。
    EXPECT_TRUE(ti.text.Get() == FStringView("A\xE3\x81\x82\xF0\x9F\x98\x80" "B"));
    EXPECT_EQ(notifications, 0);
}

// ---- TextInput: Shift 選択は anchor を越えて逆方向へ伸ばせる ----------------
ACS_TEST(Ui, TextInputShiftSelectionCanReverseDirection) {
    ATextInput ti;
    ti.focused = true;
    ti.text.Set(FString{"A\xE3\x81\x82\xF0\x9F\x98\x80" "B"});
    EXPECT_TRUE(ti.TrySetCursorByteOffset(4)); // 「あ」の直後を anchor にする

    FUiKeyModifiers shift;
    shift.bShift = true;

    ti.OnKey(0x25, true, shift);
    EXPECT_EQ(ti.CursorByteOffset(), usize(1));
    EXPECT_EQ(ti.SelectionStart(), usize(1));
    EXPECT_EQ(ti.SelectionEnd(), usize(4));

    ti.OnKey(0x27, true, shift); // anchor で折り畳む
    EXPECT_EQ(ti.CursorByteOffset(), usize(4));
    EXPECT_FALSE(ti.HasSelection());
    ti.OnKey(0x27, true, shift); // anchor を越えて順方向へ伸ばす
    EXPECT_EQ(ti.CursorByteOffset(), usize(8));
    EXPECT_EQ(ti.SelectionStart(), usize(4));
    EXPECT_EQ(ti.SelectionEnd(), usize(8));

    ti.OnKey(0x24, true, shift); // anchor を越えて先頭側へ反転
    EXPECT_EQ(ti.CursorByteOffset(), usize(0));
    EXPECT_EQ(ti.SelectionStart(), usize(0));
    EXPECT_EQ(ti.SelectionEnd(), usize(4));

    // 外部 binding で短縮しても両端を境界へ正規化し、折り畳み後に復活させない。
    ti.text.Set(FString{"Z"});
    EXPECT_EQ(ti.SelectionStart(), usize(0));
    EXPECT_EQ(ti.SelectionEnd(), usize(1));
    ti.OnKey(0x27, true, shift);
    EXPECT_FALSE(ti.HasSelection());
    EXPECT_EQ(ti.CursorByteOffset(), usize(1));
    ti.text.Set(FString{"XYZ"});
    EXPECT_FALSE(ti.HasSelection());
    EXPECT_EQ(ti.CursorByteOffset(), usize(3));
}

// ---- TextInput: Ctrl+A と従来 OnKey override の互換転送 --------------------
ACS_TEST(Ui, TextInputCtrlAAndLegacyOnKeyCompatibility) {
    ATextInput ti;
    ti.focused = true;
    ti.text.Set(FString{"A\xE3\x81\x82" "B"});

    int notifications = 0;
    ti.text.Subscribe(
        [](const FString&, void* user) {
            ++(*static_cast<int*>(user));
        },
        &notifications);

    FUiKeyModifiers control;
    control.bControl = true;
    ti.OnKey(0x41, true);            // 従来経路では A を編集コマンドにしない
    ti.OnKey(0x41, false, control);  // 解放も無視する
    EXPECT_FALSE(ti.HasSelection());

    FUiKeyModifiers alt_gr = control;
    alt_gr.bAlt = true;
    ti.OnKey(0x41, true, alt_gr);    // AltGr の文字入力を Ctrl+A と誤認しない
    EXPECT_FALSE(ti.HasSelection());

    ti.OnKey(0x41, true, control);
    EXPECT_TRUE(ti.HasSelection());
    EXPECT_EQ(ti.SelectionStart(), usize(0));
    EXPECT_EQ(ti.SelectionEnd(), usize(5));
    EXPECT_EQ(notifications, 0);

    ti.ClearSelection();
    ti.focused = false;
    ti.OnKey(0x41, true, control);
    EXPECT_FALSE(ti.HasSelection());

    class FUiLegacyKeyWidget final : public AWidget {
    public:
        void OnKey(i32 key, bool pressed_) noexcept override {
            LastKey = key;
            bLastPressed = pressed_;
            ++CallCount;
        }

        i32 LastKey = 0;
        i32 CallCount = 0;
        bool bLastPressed = false;
    };

    FUiLegacyKeyWidget legacy;
    AWidget* const widget = &legacy;
    FUiKeyModifiers modified;
    modified.bShift = true;
    widget->OnKey(0x25, true, modified);
    EXPECT_EQ(legacy.CallCount, 1);
    EXPECT_EQ(legacy.LastKey, 0x25);
    EXPECT_TRUE(legacy.bLastPressed);
}

// ---- TextInput: nested 3→2 OnKey 後に外側 modifier snapshot を復元する ----
ACS_TEST(Ui, TextInputNestedLegacyOnKeyRestoresOuterModifiers) {
    class FUiNestedLegacyTextInput final : public ATextInput {
    public:
        void OnKey(i32 key, bool pressed_) noexcept override {
            ++CallCount;
            if (!bInNestedCall && pressed_) {
                bInNestedCall = true;
                const FUiKeyModifiers inner_modifiers{};
                ATextInput::OnKey(0x0D, true, inner_modifiers);
                bInNestedCall = false;
            }
            ATextInput::OnKey(key, pressed_);
        }

        i32 CallCount = 0;
        bool bInNestedCall = false;
    };

    FUiNestedLegacyTextInput input;
    input.focused = true;
    input.text.Set(FString{"ABC"});
    FUiKeyModifiers outer_modifiers;
    outer_modifiers.bShift = true;

    AWidget* const widget = &input;
    widget->OnKey(0x25, true, outer_modifiers);
    EXPECT_EQ(input.CallCount, 2);
    EXPECT_EQ(input.CursorByteOffset(), usize(2));
    EXPECT_EQ(input.SelectionStart(), usize(2));
    EXPECT_EQ(input.SelectionEnd(), usize(3));
}

// ---- UiInput: 実 Dispatch は修飾付き押下/解放を旧2引数 override へ転送する ----
ACS_TEST(Ui, UiInputDispatchesKeyPressAndReleaseToLegacyOverride) {
    ResetUiInputFeed();

    class FUiLegacyDispatchProbe final : public AWidget {
    public:
        void OnKey(i32 key, bool pressed_) noexcept override {
            if (CallCount < 2) {
                Keys[CallCount] = key;
                Pressed[CallCount] = pressed_;
            }
            ++CallCount;
        }

        i32 Keys[2]{};
        bool Pressed[2]{};
        i32 CallCount = 0;
    };

    FUiLegacyDispatchProbe root;
    root.Layout(0.0f, 0.0f, 100.0f, 40.0f);
    CUiInput input;
    FocusUiRoot(input, root);
    EXPECT_TRUE(root.focused);

    // 右 Shift も左右集約された modifier として押下中になる。旧 override には
    // modifier 自体は渡らないが、3引数基底実装から押下イベントが転送される。
    FeedUiKey(EKey::RightShift, true);
    FeedUiKey(EKey::Left, true);
    input.Dispatch(root);
    EXPECT_EQ(root.CallCount, 1);
    EXPECT_EQ(root.Keys[0], 0x25);
    EXPECT_TRUE(root.Pressed[0]);

    // Shift を保持したまま Left を解放し、同じ旧 override へ false を届ける。
    CInput::Update();
    FeedUiKey(EKey::Left, false);
    input.Dispatch(root);
    EXPECT_EQ(root.CallCount, 2);
    EXPECT_EQ(root.Keys[1], 0x25);
    EXPECT_FALSE(root.Pressed[1]);

    input.Reset(root);
    EXPECT_FALSE(root.focused);
    ResetUiInputFeed();
}

// ---- UiInput: Shift 選択・Ctrl+A・AltGr を実 CInput フィードから検証する ----
ACS_TEST(Ui, UiInputDispatchesTextEditingModifiers) {
    ResetUiInputFeed();

    class FUiTextDispatchProbe final : public ATextInput {
    public:
        void OnKey(i32 key, bool pressed_,
                   const FUiKeyModifiers& modifiers) noexcept override {
            if (key == 0x25 && pressed_) {
                bSawShiftMove = modifiers.bShift;
            }
            if (key == 0x41) {
                if (pressed_) {
                    bSawControlAPress = modifiers.bControl;
                } else {
                    bSawControlARelease = true;
                    bControlOnARelease = modifiers.bControl;
                }
                ++ControlACallCount;
            }
            ATextInput::OnKey(key, pressed_, modifiers);
        }

        i32 ControlACallCount = 0;
        bool bSawShiftMove = false;
        bool bSawControlAPress = false;
        bool bSawControlARelease = false;
        bool bControlOnARelease = true;
    };

    FUiTextDispatchProbe root;
    root.Layout(0.0f, 0.0f, 140.0f, 40.0f);
    root.text.Set(FString{"A\xE3\x81\x82" "B"});
    CUiInput input;
    FocusUiRoot(input, root);

    FeedUiKey(EKey::RightShift, true);
    FeedUiKey(EKey::Left, true);
    input.Dispatch(root);
    EXPECT_TRUE(root.bSawShiftMove);
    EXPECT_EQ(root.CursorByteOffset(), usize(4));
    EXPECT_EQ(root.SelectionStart(), usize(4));
    EXPECT_EQ(root.SelectionEnd(), usize(5));

    // key release も届くが、ATextInput は公開契約どおり編集しない。
    CInput::Update();
    FeedUiKey(EKey::Left, false);
    input.Dispatch(root);
    EXPECT_EQ(root.CursorByteOffset(), usize(4));
    EXPECT_EQ(root.SelectionStart(), usize(4));
    EXPECT_EQ(root.SelectionEnd(), usize(5));

    CInput::Update();
    FeedUiKey(EKey::RightShift, false);
    CInput::Update();

    FeedUiKey(EKey::RightCtrl, true);
    FeedUiKey(EKey::A, true);
    input.Dispatch(root);
    EXPECT_TRUE(root.bSawControlAPress);
    EXPECT_EQ(root.ControlACallCount, 1);
    EXPECT_EQ(root.SelectionStart(), usize(0));
    EXPECT_EQ(root.SelectionEnd(), usize(5));

    // Ctrl を先に離しても、A の解放は押下を受けた widget へ対応付けて配送する。
    CInput::Update();
    FeedUiKey(EKey::RightCtrl, false);
    CInput::Update();
    FeedUiKey(EKey::A, false);
    input.Dispatch(root);
    EXPECT_TRUE(root.bSawControlARelease);
    EXPECT_FALSE(root.bControlOnARelease);
    EXPECT_EQ(root.ControlACallCount, 2);
    EXPECT_EQ(root.SelectionStart(), usize(0));
    EXPECT_EQ(root.SelectionEnd(), usize(5));
    CInput::Update();

    root.ClearSelection();
    FeedUiKey(EKey::LeftCtrl, true);
    FeedUiKey(EKey::RightAlt, true);
    FeedUiKey(EKey::A, true);
    input.Dispatch(root);
    EXPECT_FALSE(root.HasSelection()); // AltGr (Ctrl+Alt) は Ctrl+A にしない
    EXPECT_EQ(root.ControlACallCount, 2);

    input.Reset(root);
    ResetUiInputFeed();
}

// ---- Widget identity: generation wrap でも予約値 0 を返さない ---------------
ACS_TEST(Ui, WidgetInputIdentitySeparatesModulesAndSkipsZeroAtWrap) {
    const u64 max_generation = ~static_cast<u64>(0);
    TAtomic<u64> counter{ max_generation };
    TAtomic<u64> other_module_counter{ 1 };

    const usize module_token =
        ui_detail::WidgetInputModuleToken(counter);
    const usize other_module_token =
        ui_detail::WidgetInputModuleToken(other_module_counter);
    EXPECT_NE(module_token, usize(0));
    EXPECT_NE(other_module_token, usize(0));
    EXPECT_NE(module_token, other_module_token);

    const ui_detail::FWidgetInputIdentity first_identity{
        usize(0x1000), module_token, u64(7)
    };
    const ui_detail::FWidgetInputIdentity same_identity{
        usize(0x1000), module_token, u64(7)
    };
    const ui_detail::FWidgetInputIdentity other_module_identity{
        usize(0x1000), other_module_token, u64(7)
    };
    const ui_detail::FWidgetInputIdentity missing_module_identity{
        usize(0x1000), usize(0), u64(7)
    };
    EXPECT_TRUE(first_identity.IsSet());
    EXPECT_TRUE(first_identity == same_identity);
    EXPECT_TRUE(first_identity != other_module_identity);
    EXPECT_FALSE(missing_module_identity.IsSet());

    EXPECT_EQ(
        ui_detail::AcquireNonZeroWidgetInputGeneration(counter),
        max_generation);
    EXPECT_EQ(
        ui_detail::AcquireNonZeroWidgetInputGeneration(counter),
        u64(1));
    EXPECT_EQ(
        ui_detail::AcquireNonZeroWidgetInputGeneration(counter),
        u64(2));
}

// ---- UiInput: 同じアドレスへ root を再構築しても旧 focus を再利用しない ----
ACS_TEST(Ui, UiInputRejectsReusedRootAddress) {
    ResetUiInputFeed();

    class FUiLifetimeProbe final : public AWidget {
    public:
        void OnKey(i32, bool) noexcept override { ++KeyCalls; }
        i32 KeyCalls = 0;
    };

    alignas(FUiLifetimeProbe) u8 storage[sizeof(FUiLifetimeProbe)]{};
    CUiInput input;

    auto* first = ::new (static_cast<void*>(storage)) FUiLifetimeProbe();
    first->Layout(0.0f, 0.0f, 100.0f, 40.0f);
    FocusUiRoot(input, *first);
    EXPECT_TRUE(first->hovered);
    EXPECT_TRUE(first->focused);

    // Reset を呼ばず破棄し、まったく同じアドレスへ別実体を構築する。
    first->~FUiLifetimeProbe();
    auto* second = ::new (static_cast<void*>(storage)) FUiLifetimeProbe();
    second->Layout(0.0f, 0.0f, 100.0f, 40.0f);

    input.Dispatch(*second);
    EXPECT_TRUE(second->hovered);
    EXPECT_FALSE(second->focused);

    // 古い focus ID を新実体へ誤適用していれば、このキーが配送されてしまう。
    FeedUiKey(EKey::Left, true);
    input.Dispatch(*second);
    EXPECT_EQ(second->KeyCalls, 0);

    input.Reset(*second);
    EXPECT_FALSE(second->hovered);
    second->~FUiLifetimeProbe();
    ResetUiInputFeed();
}

// ---- UiInput: 同じ root から focused child を除去しても解放済み領域を触らない ----
ACS_TEST(Ui, UiInputDropsRemovedChildIdentity) {
    ResetUiInputFeed();

    class FUiMutableInputRoot final : public AContainer {
    public:
        void RemoveAllChildren() noexcept { m_Children.Clear(); }
        void OnKey(i32, bool) noexcept override { ++KeyCalls; }
        i32 KeyCalls = 0;
    };
    class FUiChildProbe final : public AWidget {};

    FUiMutableInputRoot root;
    root.Add<FUiChildProbe>();
    root.Layout(0.0f, 0.0f, 100.0f, 40.0f);
    CUiInput input;
    FocusUiRoot(input, root);

    root.RemoveAllChildren();
    input.Dispatch(root);
    EXPECT_TRUE(root.hovered);
    EXPECT_FALSE(root.focused);

    FeedUiKey(EKey::Left, true);
    input.Dispatch(root);
    EXPECT_EQ(root.KeyCalls, 0); // 除去済み child の focus を root へ誤継承しない

    input.Reset(root);
    ResetUiInputFeed();
}

// ---- UiInput: ATextInput 派生の旧2引数 OnKey も実 Dispatch から受け取る -----
ACS_TEST(Ui, UiInputDispatchesToLegacyTextInputOverride) {
    ResetUiInputFeed();

    class FUiLegacyTextInputProbe final : public ATextInput {
    public:
        void OnKey(i32 key, bool pressed_) noexcept override {
            if (CallCount < 2) {
                Keys[CallCount] = key;
                Pressed[CallCount] = pressed_;
            }
            ++CallCount;
            ATextInput::OnKey(key, pressed_);
        }

        i32 Keys[2]{};
        bool Pressed[2]{};
        i32 CallCount = 0;
    };

    FUiLegacyTextInputProbe root;
    root.Layout(0.0f, 0.0f, 120.0f, 40.0f);
    root.text.Set(FString{"AB"});
    CUiInput input;
    FocusUiRoot(input, root);

    FeedUiKey(EKey::RightShift, true);
    FeedUiKey(EKey::Left, true);
    input.Dispatch(root);
    EXPECT_EQ(root.CallCount, 1);
    EXPECT_EQ(root.Keys[0], 0x25);
    EXPECT_TRUE(root.Pressed[0]);
    EXPECT_EQ(root.SelectionStart(), usize(1));
    EXPECT_EQ(root.SelectionEnd(), usize(2));

    CInput::Update();
    FeedUiKey(EKey::Left, false);
    input.Dispatch(root);
    EXPECT_EQ(root.CallCount, 2);
    EXPECT_EQ(root.Keys[1], 0x25);
    EXPECT_FALSE(root.Pressed[1]);
    EXPECT_EQ(root.SelectionStart(), usize(1));
    EXPECT_EQ(root.SelectionEnd(), usize(2));

    input.Reset(root);
    ResetUiInputFeed();
}

// ---- ATextInput bridge: 旧2引数 override が自身を除去しても snapshot に触れない
ACS_TEST(Ui, LegacyTextInputOverrideMayRemoveItself) {
    ResetUiInputFeed();

    class FUiRemovingLegacyTextInput final : public ATextInput {
    public:
        FUiRemovingLegacyTextInput(FUiMutableCallbackRoot& owner,
                                   i32& call_count) noexcept
            : m_Owner(&owner), m_CallCount(&call_count) {}

        void OnKey(i32, bool pressed_) noexcept override {
            if (!pressed_) return;
            FUiMutableCallbackRoot* const owner = m_Owner;
            i32* const call_count = m_CallCount;
            ++(*call_count);
            owner->RemoveAllChildren();
            // *this は破棄済み。3引数 bridge は lifetime guard だけを確認する。
        }

    private:
        FUiMutableCallbackRoot* m_Owner = nullptr;
        i32* m_CallCount = nullptr;
    };

    FUiMutableCallbackRoot root;
    i32 call_count = 0;
    root.Add<FUiRemovingLegacyTextInput>(root, call_count);
    root.Layout(0.0f, 0.0f, 120.0f, 40.0f);
    CUiInput input;
    FocusUiRoot(input, root);

    FeedUiKey(EKey::Left, true);
    input.Dispatch(root);
    EXPECT_EQ(call_count, 1);
    EXPECT_EQ(root.ChildCount(), usize(0));

    CInput::Update();
    FeedUiKey(EKey::Left, false);
    input.Dispatch(root);
    CInput::Update();
    input.Reset(root);
    ResetUiInputFeed();
}

// ---- UiInput: 全 callback 中の current / other child 除去を再解決する --------
ACS_TEST(Ui, UiInputCallbacksMayRemoveCurrentOrOtherChild) {
    static constexpr EUiRemovalCallback kCallbacks[] = {
        EUiRemovalCallback::PointerMove,
        EUiRemovalCallback::PointerDown,
        EUiRemovalCallback::PointerUp,
        EUiRemovalCallback::TextInput,
        EUiRemovalCallback::Key,
    };
    static constexpr bool kRemoveSelf[] = { false, true };

    for (bool remove_self : kRemoveSelf) {
        for (EUiRemovalCallback callback : kCallbacks) {
            ResetUiInputFeed();

            FUiMutableCallbackRoot root;
            if (!remove_self) root.Add<AWidget>();
            i32 call_count = 0;
            FUiRemovingCallbackWidget* const target =
                root.Add<FUiRemovingCallbackWidget>(
                    root, callback, remove_self, call_count);
            root.Layout(0.0f, 0.0f, 120.0f, 40.0f);
            CUiInput input;

            switch (callback) {
            case EUiRemovalCallback::PointerMove:
                FeedUiMouseMove(8.0f, 8.0f);
                input.Dispatch(root);
                CInput::Update();
                break;

            case EUiRemovalCallback::PointerDown:
                FeedUiMouseMove(8.0f, 8.0f);
                FeedUiLeftMouse(true);
                input.Dispatch(root);
                CInput::Update();
                FeedUiLeftMouse(false);
                input.Dispatch(root);
                CInput::Update();
                break;

            case EUiRemovalCallback::PointerUp:
                FocusUiRoot(input, root);
                break;

            case EUiRemovalCallback::TextInput:
                FocusUiRoot(input, root);
                FeedUiChar('Q');
                input.Dispatch(root);
                CInput::Update();
                break;

            case EUiRemovalCallback::Key:
                FocusUiRoot(input, root);
                FeedUiKey(EKey::Left, true);
                input.Dispatch(root);
                CInput::Update();
                FeedUiKey(EKey::Left, false);
                input.Dispatch(root);
                CInput::Update();
                break;
            }

            EXPECT_EQ(call_count, 1);
            if (remove_self) {
                EXPECT_EQ(root.ChildCount(), usize(0));
            } else {
                EXPECT_EQ(root.ChildCount(), usize(1));
                EXPECT_TRUE(root.Child(0) == target);
            }

            // stale identity が残っていても、生存 subtree の再解決だけで安全に進む。
            input.Dispatch(root);
            input.Reset(root);
            ResetUiInputFeed();
        }
    }
}

// ---- Observable callback: Button pulse 中に current / sibling を除去できる --
ACS_TEST(Ui, ButtonClickSubscriberMayRemoveCurrentOrOtherChild) {
    static constexpr bool kRemoveSelf[] = { false, true };

    for (bool remove_self : kRemoveSelf) {
        ResetUiInputFeed();

        FUiMutableCallbackRoot root;
        if (!remove_self) root.Add<AWidget>();
        AButton* const button = root.Add<AButton>("Remove");
        struct FContext {
            FUiMutableCallbackRoot* Root = nullptr;
            i32 Calls = 0;
            bool RemoveSelf = false;
        } context{ &root, 0, remove_self };

        button->clicked.Subscribe(
            [](const bool& clicked, void* user) {
                if (!clicked) return;
                auto* const context = static_cast<FContext*>(user);
                ++context->Calls;
                if (context->RemoveSelf) context->Root->RemoveAllChildren();
                else context->Root->RemoveFirstChild();
            },
            &context);

        root.Layout(0.0f, 0.0f, 120.0f, 40.0f);
        CUiInput input;
        FocusUiRoot(input, root);

        EXPECT_EQ(context.Calls, 1);
        if (remove_self) {
            EXPECT_EQ(root.ChildCount(), usize(0));
        } else {
            EXPECT_EQ(root.ChildCount(), usize(1));
            EXPECT_TRUE(root.Child(0) == button);
            EXPECT_FALSE(button->pressed);
            EXPECT_FALSE(button->clicked.Get());
        }

        input.Dispatch(root);
        input.Reset(root);
        ResetUiInputFeed();
    }
}

// ---- Button pulse: false 通知で自身を除去しても guard destructor は安全 -----
ACS_TEST(Ui, ButtonFalsePulseSubscriberMayRemoveCurrentChild) {
    ResetUiInputFeed();

    FUiMutableCallbackRoot root;
    AButton* const button = root.Add<AButton>("RemoveOnFalse");
    struct FContext {
        FUiMutableCallbackRoot* Root = nullptr;
        i32 FalseCalls = 0;
    } context{ &root, 0 };

    button->clicked.Subscribe(
        [](const bool& clicked, void* user) {
            if (clicked) return;
            auto* const context = static_cast<FContext*>(user);
            ++context->FalseCalls;
            context->Root->RemoveAllChildren();
        },
        &context);

    root.Layout(0.0f, 0.0f, 120.0f, 40.0f);
    CUiInput input;
    FocusUiRoot(input, root);

    EXPECT_EQ(context.FalseCalls, 1);
    EXPECT_EQ(root.ChildCount(), usize(0));
    input.Dispatch(root);
    input.Reset(root);
    ResetUiInputFeed();
}

// ---- Observable callback: TextInput Set 中に current / sibling を除去できる -
ACS_TEST(Ui, TextInputSubscriberMayRemoveCurrentOrOtherChild) {
    static constexpr bool kRemoveSelf[] = { false, true };

    for (bool remove_self : kRemoveSelf) {
        ResetUiInputFeed();

        FUiMutableCallbackRoot root;
        if (!remove_self) root.Add<AWidget>();
        ATextInput* const text_input = root.Add<ATextInput>();
        struct FContext {
            FUiMutableCallbackRoot* Root = nullptr;
            i32 Calls = 0;
            bool RemoveSelf = false;
        } context{ &root, 0, remove_self };

        text_input->text.Subscribe(
            [](const FString&, void* user) {
                auto* const context = static_cast<FContext*>(user);
                ++context->Calls;
                if (context->RemoveSelf) context->Root->RemoveAllChildren();
                else context->Root->RemoveFirstChild();
            },
            &context);

        root.Layout(0.0f, 0.0f, 120.0f, 40.0f);
        CUiInput input;
        FocusUiRoot(input, root);
        FeedUiChar('Q');
        input.Dispatch(root);
        CInput::Update();

        EXPECT_EQ(context.Calls, 1);
        if (remove_self) {
            EXPECT_EQ(root.ChildCount(), usize(0));
        } else {
            EXPECT_EQ(root.ChildCount(), usize(1));
            EXPECT_TRUE(root.Child(0) == text_input);
            EXPECT_TRUE(text_input->text.Get() == FString{"Q"});
            EXPECT_EQ(text_input->CursorByteOffset(), usize(1));
        }

        input.Dispatch(root);
        input.Reset(root);
        ResetUiInputFeed();
    }
}

// ---- TextInput erase commit: subscriber が current / sibling を除去できる ---
ACS_TEST(Ui, TextInputEraseSubscriberMayRemoveCurrentOrOtherChild) {
    static constexpr bool kRemoveSelf[] = { false, true };

    for (bool remove_self : kRemoveSelf) {
        ResetUiInputFeed();

        FUiMutableCallbackRoot root;
        if (!remove_self) root.Add<AWidget>();
        ATextInput* const text_input = root.Add<ATextInput>();
        text_input->text.Set(FString{"AB"});
        struct FContext {
            FUiMutableCallbackRoot* Root = nullptr;
            i32 Calls = 0;
            bool RemoveSelf = false;
        } context{ &root, 0, remove_self };

        text_input->text.Subscribe(
            [](const FString&, void* user) {
                auto* const context = static_cast<FContext*>(user);
                ++context->Calls;
                if (context->RemoveSelf) {
                    context->Root->RemoveAllChildren();
                } else {
                    context->Root->RemoveFirstChild();
                }
            },
            &context);

        root.Layout(0.0f, 0.0f, 120.0f, 40.0f);
        CUiInput input;
        FocusUiRoot(input, root);
        FeedUiKey(EKey::Backspace, true);
        input.Dispatch(root);
        CInput::Update();
        FeedUiKey(EKey::Backspace, false);
        input.Dispatch(root);
        CInput::Update();

        EXPECT_EQ(context.Calls, 1);
        if (remove_self) {
            EXPECT_EQ(root.ChildCount(), usize(0));
        } else {
            EXPECT_EQ(root.ChildCount(), usize(1));
            EXPECT_TRUE(root.Child(0) == text_input);
            EXPECT_TRUE(text_input->text.Get() == FString{"A"});
            EXPECT_EQ(text_input->CursorByteOffset(), usize(1));
        }

        input.Dispatch(root);
        input.Reset(root);
        ResetUiInputFeed();
    }
}

// ---- UiRenderer: hidden root は Layout と render callback をともに省略する ----
ACS_TEST(Ui, UiRendererSkipsHiddenRootLayoutAndRendering) {
    class FUiRenderChildProbe final : public AWidget {
    public:
        void Layout(f32 x, f32 y, f32 w, f32 h) noexcept override {
            ++LayoutCalls;
            AWidget::Layout(x, y, w, h);
        }

        void Render(CUiRenderer&) noexcept override { ++RenderCalls; }

        i32 LayoutCalls = 0;
        i32 RenderCalls = 0;
    };
    class FUiRenderRootProbe final : public AContainer {
    public:
        void Layout(f32 x, f32 y, f32 w, f32 h) noexcept override {
            ++LayoutCalls;
            AContainer::Layout(x, y, w, h);
        }

        void Render(CUiRenderer& renderer) noexcept override {
            ++RenderCalls;
            AWidget::Render(renderer);
        }

        i32 LayoutCalls = 0;
        i32 RenderCalls = 0;
    };

    FUiRenderRootProbe root;
    FUiRenderChildProbe* const child = root.Add<FUiRenderChildProbe>();
    CUiRenderer renderer;
    root.visible = false;
    const bool hidden_visited = ui_detail::VisitVisibleUiRoot(
        root, 120.0f, 40.0f,
        [&root, &renderer]() noexcept { root.Render(renderer); });
    EXPECT_FALSE(hidden_visited);
    EXPECT_EQ(root.LayoutCalls, 0);
    EXPECT_EQ(root.RenderCalls, 0);
    EXPECT_EQ(child->LayoutCalls, 0);
    EXPECT_EQ(child->RenderCalls, 0);

    root.visible = true;
    const bool visible_visited = ui_detail::VisitVisibleUiRoot(
        root, 120.0f, 40.0f,
        [&root, &renderer]() noexcept { root.Render(renderer); });
    EXPECT_TRUE(visible_visited);
    EXPECT_EQ(root.LayoutCalls, 1);
    EXPECT_EQ(root.RenderCalls, 1);
    EXPECT_EQ(child->LayoutCalls, 1);
    EXPECT_EQ(child->RenderCalls, 1);
}

// ---- UiInput: no-arg Reset 後の同じ live root も次 Dispatch で初期化する ------
ACS_TEST(Ui, UiInputNoArgResetReinitializesLiveRootOnNextDispatch) {
    ResetUiInputFeed();

    AWidget root;
    root.Layout(0.0f, 0.0f, 100.0f, 40.0f);
    CUiInput input;
    FocusUiRoot(input, root);
    EXPECT_TRUE(root.hovered);
    EXPECT_TRUE(root.focused);

    input.Reset();
    FeedUiMouseMove(500.0f, 500.0f);
    input.Dispatch(root);
    EXPECT_FALSE(root.hovered);
    EXPECT_FALSE(root.focused);
    EXPECT_FALSE(root.pressed);

    input.Reset(root);
    ResetUiInputFeed();
}

// ---- TextInput: binding 短縮時は cursor と anchor を同じ境界へ正規化する --------
ACS_TEST(Ui, TextInputSelectionNormalizesAfterExternalBindingChange) {
    ATextInput ti;
    ti.SelectAll();
    ti.text.Set(FString{"Q"});
    EXPECT_FALSE(ti.HasSelection()); // 空文字列での SelectAll は後続 binding を選択しない

    ti.text.Set(FString{"A\xE3\x81\x82" "B"});
    EXPECT_TRUE(ti.TrySetSelection(1, 5));

    ti.text.Set(FString{"Z"});
    EXPECT_FALSE(ti.HasSelection());
    EXPECT_EQ(ti.CursorByteOffset(), usize(1));
    EXPECT_EQ(ti.SelectionStart(), usize(1));
    EXPECT_EQ(ti.SelectionEnd(), usize(1));

    // 折り畳まれた両端点は end-follow を共有し、後続の伸長で選択が復活しない。
    ti.text.Set(FString{"XYZ"});
    EXPECT_FALSE(ti.HasSelection());
    EXPECT_EQ(ti.CursorByteOffset(), usize(3));

    ti.SelectAll();
    ti.ClearSelection();
    EXPECT_FALSE(ti.HasSelection());
    EXPECT_EQ(ti.CursorByteOffset(), usize(3));
}

// ---- TextInput: selection 編集失敗は全状態と通知回数を維持する -----------------
ACS_TEST(Ui, TextInputSelectionFailureIsTransactional) {
    CSystemAllocator backing;
    FUiSwitchableFailAllocator allocator(backing);
    ATextInput ti;
    FString initial(allocator);
    EXPECT_TRUE(initial.TryAppend("abcdefghijklmnopqrstuvwxyzABCDEF")); // 32 bytes
    ti.text.Set(Move(initial));
    ti.focused = true;
    EXPECT_TRUE(ti.TrySetSelection(1, 2));

    int notifications = 0;
    ti.text.Subscribe(
        [](const FString&, void* user) {
            ++(*static_cast<int*>(user));
        },
        &notifications);

    EXPECT_TRUE(ti.TrySetMaxTextBytes(31));
    EXPECT_FALSE(ti.TryInsertCodepoint('Z')); // 選択置換後も 32 bytes のため上限超過
    EXPECT_TRUE(ti.text.Get() == FStringView("abcdefghijklmnopqrstuvwxyzABCDEF"));
    EXPECT_TRUE(ti.HasSelection());
    EXPECT_EQ(ti.SelectionStart(), usize(1));
    EXPECT_EQ(ti.SelectionEnd(), usize(2));
    EXPECT_EQ(notifications, 0);
    EXPECT_TRUE(ti.TrySetMaxTextBytes(32));

    allocator.SetFailing(true);
    EXPECT_FALSE(ti.TryInsertCodepoint('Z')); // 32-byte の結果には heap 確保が必要
    EXPECT_TRUE(ti.text.Get() == FStringView("abcdefghijklmnopqrstuvwxyzABCDEF"));
    EXPECT_TRUE(ti.HasSelection());
    EXPECT_EQ(ti.SelectionStart(), usize(1));
    EXPECT_EQ(ti.SelectionEnd(), usize(2));
    EXPECT_EQ(ti.CursorByteOffset(), usize(2));
    EXPECT_EQ(notifications, 0);

    EXPECT_FALSE(ti.TryEraseBeforeCursor()); // 31-byte の結果にも heap 確保が必要
    EXPECT_TRUE(ti.text.Get() == FStringView("abcdefghijklmnopqrstuvwxyzABCDEF"));
    EXPECT_TRUE(ti.HasSelection());
    EXPECT_EQ(ti.SelectionStart(), usize(1));
    EXPECT_EQ(ti.SelectionEnd(), usize(2));
    EXPECT_EQ(notifications, 0);
    allocator.SetFailing(false);
}

// ---- TextInput: 選択ハイライトは入力欄の内容領域へ切り詰める ---------------
ACS_TEST(Ui, TextInputSelectionHighlightGeometryIsClamped) {
    const FUiRect input{ 10.0f, 20.0f, 100.0f, 30.0f };

    // 通常の選択範囲は左右位置を維持し、上下余白だけを適用する。
    const FUiRect normal =
        ui_detail::ComputeTextSelectionHighlightRect(input, 8.0f, 40.0f);
    EXPECT_EQ(normal.x, 24.0f);
    EXPECT_EQ(normal.y, 23.0f);
    EXPECT_EQ(normal.w, 32.0f);
    EXPECT_EQ(normal.h, 24.0f);

    // 長い選択は右側の内容境界を越えない。
    const FUiRect long_selection =
        ui_detail::ComputeTextSelectionHighlightRect(input, 8.0f, 400.0f);
    EXPECT_EQ(long_selection.x, 24.0f);
    EXPECT_EQ(long_selection.y, 23.0f);
    EXPECT_EQ(long_selection.w, 80.0f);
    EXPECT_EQ(long_selection.h, 24.0f);

    // 左側へはみ出す測定値も内容境界へ切り詰める。
    const FUiRect left_clamped =
        ui_detail::ComputeTextSelectionHighlightRect(input, -20.0f, 10.0f);
    EXPECT_EQ(left_clamped.x, 16.0f);
    EXPECT_EQ(left_clamped.w, 10.0f);

    // 左右余白を確保できない狭さでは描画幅を 0 にする。
    const FUiRect narrow =
        ui_detail::ComputeTextSelectionHighlightRect(
            FUiRect{ 10.0f, 20.0f, 12.0f, 30.0f }, 0.0f, 100.0f);
    EXPECT_EQ(narrow.x, 16.0f);
    EXPECT_EQ(narrow.w, 0.0f);
    EXPECT_EQ(narrow.h, 24.0f);

    // 上下余白を確保できない高さでは描画高さを 0 にする。
    const FUiRect short_height =
        ui_detail::ComputeTextSelectionHighlightRect(
            FUiRect{ 10.0f, 20.0f, 100.0f, 6.0f }, 0.0f, 20.0f);
    EXPECT_EQ(short_height.y, 23.0f);
    EXPECT_EQ(short_height.w, 20.0f);
    EXPECT_EQ(short_height.h, 0.0f);
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
    AAnchorPanel hud;
    auto* tl  = hud.Add<ALabel>("tl");
    auto* br  = hud.Add<ALabel>("br");
    auto* bar = hud.Add<AContainer>();
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
    AAnchorPanel p;
    auto* a = p.Add<ALabel>("a");
    a->anchor = FUiAnchor::Point({0, 0}, 50, 50, 10, 10);
    a->visible = false;
    a->rect = { -1, -1, -1, -1 };   // 番兵

    p.Layout(0, 0, 400, 400);
    // visible=false なので Layout されず rect は番兵のまま。
    EXPECT_EQ(a->rect.x, -1.0f);
}

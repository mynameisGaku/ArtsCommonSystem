// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — UiLayer 実装 (Phase H-1 スケルトン)
//
// 現フェーズ: state holder のみ完全実装。実 `acs::ui` Widget tree 構築 / 描画 /
// hit-test / event 配送は seam として未接続で、Init / Tick / HandleInput の中身は
// state 操作と TODO コメントのみ。これにより Scene 側は通常通り
// AddButton / AddText / SetVisible / Remove を呼び始めることができ、Phase H-2 で
// `acs::ui` 接続を入れるだけで実 UI が表示される設計。
//
// 設計メモ:
//   ・handle 発行は単純な単調増加 (`_next_handle++`)。重複しない正の u32 を保証。
//     現実的なシーンで widget 数が 2^32 を超えることはないため reuse は不要。
//   ・FindIndex の戻り値 0xFFFFFFFFu はセンチネル。`_widgets.Size()` と比較して
//     見つからなかったか判定する (signed/unsigned 混在を避ける慣用)。
//   ・Tick で `just_pressed` を一括クリアする。これにより IsButtonPressed は
//     **直前フレームに発生した押下** のみ true を返すワンショットセマンティクス
//     となる (押しっぱなしで連続 true を返さない)。Phase H-2 で event 配送が
//     繋がると、HandleInput で該当 widget の just_pressed を立てる流れになる。
//   ・全 noexcept、STL 不使用、<string> 不使用 (規約 §1, §2)。
#include "gameframework/UiLayer.h"

#include "platform/Event.h"

namespace acs::game {

namespace {

// FindIndex が見つからなかった時に返すセンチネル値。u32 の MAX。
constexpr u32 kInvalidIndex = 0xFFFFFFFFu;

} // namespace

void UiLayer::Init() noexcept {
    if (_initialized) {
        // 冪等性確保。重複 Init はログのみで何もしない (Shutdown 経由せず再 Init
        // するケースもあり得るが、現状は警告レベルではなく Debug で記録)。
        ACS_LOG_DEBUG("UiLayer::Init called twice (ignored)");
        return;
    }
    // TODO(Phase H-2): `acs::ui::Container` を root として `MakeUnique` で確保し
    //   メンバ保持する。現フェーズは state holder のみで root インスタンス不要。
    _widgets.Clear();
    _next_handle = 1;
    _initialized = true;
    ACS_LOG_DEBUG("UiLayer::Init: state holder ready (root widget TODO)");
}

void UiLayer::Shutdown() noexcept {
    if (!_initialized) {
        // 未初期化での Shutdown は no-op (冪等)。
        return;
    }
    // TODO(Phase H-2): root Container を破棄。現フェーズは保持していないので
    //   _widgets をクリアするだけで完結。
    _widgets.Clear();
    _next_handle = 1;
    _initialized = false;
    ACS_LOG_DEBUG("UiLayer::Shutdown: state cleared");
}

void UiLayer::Tick(f32 /*dt*/) noexcept {
    if (!_initialized) return;
    // ワンショット押下フラグをクリア。HandleInput で立てられた just_pressed は
    // 同フレーム中の OnUpdate で IsButtonPressed として消費されるので、次の
    // Tick 開始時にゼロに戻す。
    //
    // 注意: Scene のフレーム順は OnEvent → OnUpdate → OnRender。すなわち本実装は
    //   「Tick の最初に clear」では 1 フレーム遅延する。Phase H-2 で event ベース
    //   配送に切り替える際は、HandleInput で立てた直後の OnUpdate で読めるよう、
    //   Tick の末尾 (= 次フレーム入る前) でクリアするのが正解。現スケルトンでは
    //   末尾クリアで統一する。
    for (u32 i = 0; i < _widgets.Size(); ++i) {
        _widgets[i].just_pressed = false;
    }
    // TODO(Phase H-2): `acs::ui::Widget::Layout(0, 0, screen_w, screen_h)` 等で
    //   レイアウトを再計算し、tween / animation の進行を更新する。
}

void UiLayer::HandleInput(const acs::Event& /*event*/) noexcept {
    if (!_initialized) return;
    // TODO(Phase H-2): event.type に応じて分岐:
    //   ・MouseButtonPressed   → カーソル位置で hit-test、ボタン widget の
    //                            just_pressed = true を立てる
    //   ・MouseMoved           → hover 状態を更新 (Widget::hovered)
    //   ・KeyPressed (Tab)     → focus 遷移
    //   ・KeyPressed (Enter)   → focused widget へ「クリック」相当を流す
    //   ・CharInput            → focused TextInput widget に codepoint を渡す
    //
    //   現フェーズは event を破棄するだけ (state holder API テスト目的)。
}

u32 UiLayer::WidgetCount() const noexcept {
    return static_cast<u32>(_widgets.Size());
}

u32 UiLayer::AddButton(const char* label, acs::FVec2 pos, acs::FVec2 size) noexcept {
    if (!_initialized) {
        // Init せずに使われたら warn (Scene 側の OnEnter で Init 漏れ検出)。
        ACS_LOG_WARN("UiLayer::AddButton called before Init (ignored)");
        return 0;
    }
    WidgetEntry e{};
    e.handle       = _next_handle++;
    e.kind         = EWidgetKind::Button;
    e.pos          = pos;
    e.size         = size;
    e.text         = label;          // 非所有、寿命は呼び出し側保証
    e.visible      = true;
    e.just_pressed = false;
    _widgets.PushBack(e);
    return e.handle;
}

u32 UiLayer::AddText(const char* text, acs::FVec2 pos) noexcept {
    if (!_initialized) {
        ACS_LOG_WARN("UiLayer::AddText called before Init (ignored)");
        return 0;
    }
    WidgetEntry e{};
    e.handle       = _next_handle++;
    e.kind         = EWidgetKind::Text;
    e.pos          = pos;
    e.size         = acs::FVec2{0.0f, 0.0f};  // Phase H-2 でフォントメトリックから計算
    e.text         = text;                    // 非所有
    e.visible      = true;
    e.just_pressed = false;
    _widgets.PushBack(e);
    return e.handle;
}

bool UiLayer::IsButtonPressed(u32 handle) const noexcept {
    const u32 idx = FindIndex(handle);
    if (idx == kInvalidIndex) return false;
    const WidgetEntry& e = _widgets[idx];
    // Text widget には押下概念がない。Button のみが押下対象。
    if (e.kind != EWidgetKind::Button) return false;
    return e.just_pressed;
}

void UiLayer::SetVisible(u32 handle, bool visible) noexcept {
    const u32 idx = FindIndex(handle);
    if (idx == kInvalidIndex) {
        ACS_LOG_WARN("UiLayer::SetVisible: invalid handle %u", handle);
        return;
    }
    _widgets[idx].visible = visible;
}

void UiLayer::Remove(u32 handle) noexcept {
    const u32 idx = FindIndex(handle);
    if (idx == kInvalidIndex) {
        // 削除で invalid を渡すケースは「既に消されたものを再度消す」など
        // 起こり得るので Debug レベルに留める (Warn だとログが煩い)。
        ACS_LOG_DEBUG("UiLayer::Remove: invalid handle %u (already removed?)", handle);
        return;
    }
    // 末尾と swap して PopBack (順序非保持の高速削除、ハンドル探索は線形なので
    // 順序保持不要)。TArray に EraseSwap 等の API があればそちらを使うがここでは
    // 明示的に書く。
    const u32 last = static_cast<u32>(_widgets.Size()) - 1u;
    if (idx != last) {
        _widgets[idx] = _widgets[last];
    }
    _widgets.PopBack();
}

void UiLayer::Clear() noexcept {
    _widgets.Clear();
    // _next_handle はリセットしない: Clear 後も以前の handle が外部に残っている
    // 可能性があり、再利用すると意図しないヒットが起こり得る。Shutdown まで
    // 単調増加を維持する。
}

u32 UiLayer::FindIndex(u32 handle) const noexcept {
    if (handle == 0) return kInvalidIndex;
    for (u32 i = 0; i < _widgets.Size(); ++i) {
        if (_widgets[i].handle == handle) return i;
    }
    return kInvalidIndex;
}

} // namespace acs::game

// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — FUiLayer 実装
//
// state holder を完全実装する。実 `acs::ui` Widget tree 構築 / 描画 /
// hit-test / event 配送は seam として未接続で、Init / Tick / HandleInput の中身は
// state 操作と TODO コメントのみ。これにより Scene 側は通常通り
// AddButton / AddText / SetVisible / Remove を呼び始めることができ、後から
// `acs::ui` 接続を入れるだけで実 UI が表示される設計。
//
// 設計メモ:
//   ・handle 発行は単純な単調増加 (`m_NextHandle++`)。重複しない正の u32 を保証。
//     現実的なシーンで widget 数が 2^32 を超えることはないため reuse は不要。
//   ・FindIndex の戻り値 0xFFFFFFFFu はセンチネル。`m_Widgets.Size()` と比較して
//     見つからなかったか判定する (signed/unsigned 混在を避ける慣用)。
//   ・Tick で `just_pressed` を一括クリアする。これにより IsButtonPressed は
//     **直前フレームに発生した押下** のみ true を返すワンショットセマンティクス
//     となる (押しっぱなしで連続 true を返さない)。event 配送が繋がると、
//     HandleInput で該当 widget の just_pressed を立てる流れになる。
//   ・全 noexcept、STL 不使用、<string> 不使用 (規約 §1, §2)。
#include "gameframework/UiLayer.h"
#include "gameframework/RenderContext.h"

#include "platform/Event.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"

namespace acs::game {

namespace {

// FindIndex が見つからなかった時に返すセンチネル値。u32 の MAX。
constexpr u32 kInvalidIndex = 0xFFFFFFFFu;

} // namespace

void FUiLayer::Init() noexcept {
    if (m_Initialized) {
        // 冪等性確保。重複 Init はログのみで何もしない (Shutdown 経由せず再 Init
        // するケースもあり得るが、現状は警告レベルではなく Debug で記録)。
        ACS_LOG_DEBUG("FUiLayer::Init called twice (ignored)");
        return;
    }
    // TODO: `acs::ui::Container` を root として `MakeUnique` で確保し
    //   メンバ保持する。現状は state holder のみで root インスタンス不要。
    m_Widgets.Clear();
    m_NextHandle = 1;
    m_Initialized = true;
    ACS_LOG_DEBUG("FUiLayer::Init: state holder ready (root widget TODO)");
}

void FUiLayer::Shutdown() noexcept {
    if (!m_Initialized) {
        // 未初期化での Shutdown は no-op (冪等)。
        return;
    }
    // TODO: root Container を破棄。現状は保持していないので
    //   m_Widgets をクリアするだけで完結。
    m_Widgets.Clear();
    m_NextHandle = 1;
    m_Initialized = false;
    ACS_LOG_DEBUG("FUiLayer::Shutdown: state cleared");
}

void FUiLayer::Tick(f32 /*dt*/) noexcept {
    if (!m_Initialized) return;
    // just_pressed は IsButtonPressed の consume-on-read で消費する方式にしたため、
    // Tick で一括クリアはしない (OnEvent→OnUpdate のフレーム順で this-frame の
    // クリックを誤って消さないため)。将来 layout 再計算 / animation 更新をここに。
}

void FUiLayer::HandleInput(const acs::Event& event) noexcept {
    if (!m_Initialized) return;
    switch (event.type) {
        case acs::EventType::MouseMoved: {
            m_MouseX = event.mouse_move.x;
            m_MouseY = event.mouse_move.y;
            // hover 状態を更新 (ボタンのみ)。
            for (u32 i = 0; i < m_Widgets.Size(); ++i) {
                WidgetEntry& e = m_Widgets[i];
                if (e.kind != EWidgetKind::Button) continue;
                e.hovered = e.visible
                    && m_MouseX >= e.pos.x && m_MouseX < e.pos.x + e.size.x
                    && m_MouseY >= e.pos.y && m_MouseY < e.pos.y + e.size.y;
            }
            break;
        }
        case acs::EventType::MouseButtonPressed: {
            if (event.mouse_button.button != acs::EMouseButton::Left) break;
            m_PressedHandle = HitTopButton(m_MouseX, m_MouseY);
            if (m_PressedHandle != 0) {
                const u32 idx = FindIndex(m_PressedHandle);
                if (idx != kInvalidIndex) m_Widgets[idx].pressed_down = true;
            }
            break;
        }
        case acs::EventType::MouseButtonReleased: {
            if (event.mouse_button.button != acs::EMouseButton::Left) break;
            // press 開始ボタンの上で離したらクリック成立。
            if (m_PressedHandle != 0 && HitTopButton(m_MouseX, m_MouseY) == m_PressedHandle) {
                const u32 idx = FindIndex(m_PressedHandle);
                if (idx != kInvalidIndex) m_Widgets[idx].just_pressed = true;
            }
            // 押し込み状態を全解除。
            for (u32 i = 0; i < m_Widgets.Size(); ++i) m_Widgets[i].pressed_down = false;
            m_PressedHandle = 0;
            break;
        }
        default:
            break;
    }
}

// (x,y) を含む最前面 (= 最後に追加) の visible なボタンの handle。無ければ 0。
u32 FUiLayer::HitTopButton(f32 x, f32 y) const noexcept {
    for (u32 i = static_cast<u32>(m_Widgets.Size()); i > 0; --i) {
        const WidgetEntry& e = m_Widgets[i - 1];
        if (e.kind == EWidgetKind::Button && e.visible
            && x >= e.pos.x && x < e.pos.x + e.size.x
            && y >= e.pos.y && y < e.pos.y + e.size.y) {
            return e.handle;
        }
    }
    return 0;
}

void FUiLayer::Draw(RenderContext& rc) const noexcept {
    if (!rc.HasSprites()) return;
    FSpriteBatch& sb = rc.Sprites();
    const bool has_font = rc.HasFont();
    for (u32 i = 0; i < m_Widgets.Size(); ++i) {
        const WidgetEntry& e = m_Widgets[i];
        if (!e.visible) continue;
        if (e.kind == EWidgetKind::Button) {
            const FVec4 bg = e.pressed_down ? FVec4{0.18f, 0.34f, 0.62f, 0.95f}
                           : e.hovered      ? FVec4{0.32f, 0.52f, 0.88f, 0.95f}
                                            : FVec4{0.22f, 0.26f, 0.34f, 0.92f};
            sb.DrawRect(e.pos.x, e.pos.y, e.size.x, e.size.y, bg);
            if (has_font && e.text) {
                sb.DrawString(rc.GetFont(), e.text, e.pos.x + 12.0f,
                              e.pos.y + (e.size.y - 18.0f) * 0.5f, FVec4{1, 1, 1, 1});
            }
        } else if (e.kind == EWidgetKind::Text) {
            if (has_font && e.text) {
                sb.DrawString(rc.GetFont(), e.text, e.pos.x, e.pos.y,
                              FVec4{0.92f, 0.92f, 0.95f, 1.0f});
            }
        }
    }
}

u32 FUiLayer::WidgetCount() const noexcept {
    return static_cast<u32>(m_Widgets.Size());
}

u32 FUiLayer::AddButton(const char* label, acs::FVec2 pos, acs::FVec2 size) noexcept {
    if (!m_Initialized) {
        // Init せずに使われたら warn (Scene 側の OnEnter で Init 漏れ検出)。
        ACS_LOG_WARN("FUiLayer::AddButton called before Init (ignored)");
        return 0;
    }
    WidgetEntry e{};
    e.handle       = m_NextHandle++;
    e.kind         = EWidgetKind::Button;
    e.pos          = pos;
    e.size         = size;
    e.text         = label;          // 非所有、寿命は呼び出し側保証
    e.visible      = true;
    e.just_pressed = false;
    m_Widgets.PushBack(e);
    return e.handle;
}

u32 FUiLayer::AddText(const char* text, acs::FVec2 pos) noexcept {
    if (!m_Initialized) {
        ACS_LOG_WARN("FUiLayer::AddText called before Init (ignored)");
        return 0;
    }
    WidgetEntry e{};
    e.handle       = m_NextHandle++;
    e.kind         = EWidgetKind::Text;
    e.pos          = pos;
    e.size         = acs::FVec2{0.0f, 0.0f};  // フォントメトリックから計算予定
    e.text         = text;                    // 非所有
    e.visible      = true;
    e.just_pressed = false;
    m_Widgets.PushBack(e);
    return e.handle;
}

bool FUiLayer::IsButtonPressed(u32 handle) const noexcept {
    const u32 idx = FindIndex(handle);
    if (idx == kInvalidIndex) return false;
    const WidgetEntry& e = m_Widgets[idx];
    // Text widget には押下概念がない。Button のみが押下対象。
    if (e.kind != EWidgetKind::Button) return false;
    if (!e.just_pressed) return false;
    // consume-on-read: 1 クリックにつき 1 回だけ true を返す (フレーム順に非依存で、
    // OnEvent で立てた just_pressed を OnUpdate の本呼び出しで確実に拾える)。
    const_cast<WidgetEntry&>(e).just_pressed = false;
    return true;
}

void FUiLayer::SetVisible(u32 handle, bool visible) noexcept {
    const u32 idx = FindIndex(handle);
    if (idx == kInvalidIndex) {
        ACS_LOG_WARN("FUiLayer::SetVisible: invalid handle %u", handle);
        return;
    }
    m_Widgets[idx].visible = visible;
}

void FUiLayer::Remove(u32 handle) noexcept {
    const u32 idx = FindIndex(handle);
    if (idx == kInvalidIndex) {
        // 削除で invalid を渡すケースは「既に消されたものを再度消す」など
        // 起こり得るので Debug レベルに留める (Warn だとログが煩い)。
        ACS_LOG_DEBUG("FUiLayer::Remove: invalid handle %u (already removed?)", handle);
        return;
    }
    // 末尾と swap して PopBack (順序非保持の高速削除、ハンドル探索は線形なので
    // 順序保持不要)。TArray に EraseSwap 等の API があればそちらを使うがここでは
    // 明示的に書く。
    const u32 last = static_cast<u32>(m_Widgets.Size()) - 1u;
    if (idx != last) {
        m_Widgets[idx] = m_Widgets[last];
    }
    m_Widgets.PopBack();
}

void FUiLayer::Clear() noexcept {
    m_Widgets.Clear();
    // m_NextHandle はリセットしない: Clear 後も以前の handle が外部に残っている
    // 可能性があり、再利用すると意図しないヒットが起こり得る。Shutdown まで
    // 単調増加を維持する。
}

u32 FUiLayer::FindIndex(u32 handle) const noexcept {
    if (handle == 0) return kInvalidIndex;
    for (u32 i = 0; i < m_Widgets.Size(); ++i) {
        if (m_Widgets[i].handle == handle) return i;
    }
    return kInvalidIndex;
}

} // namespace acs::game

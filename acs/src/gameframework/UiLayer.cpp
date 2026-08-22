// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — CUiLayer 実装
//
// 設計メモ:
//   ・handle 発行は単純な単調増加 (`m_NextHandle++`)。重複しない正の u32 を保証。
//     wrap 後は 0 を返して新規追加を止め、既存 handle を再利用しない。
//   ・FindIndex の戻り値 0xFFFFFFFFu はセンチネル。`m_Widgets.Num()` と比較して
//     見つからなかったか判定する (signed/unsigned 混在を避ける慣用)。
//   ・表示文字列は DefaultAllocator から個別確保する。FWidgetEntry のポインタ幅と
//     CUiLayer の member 構成を維持しながら、呼出し側の文字列寿命へ依存しない。
//   ・全 noexcept、std::string 不使用。
#include "gameframework/UiLayer.h"
#include "gameframework/RenderContext.h"

#include "memory/Allocator.h"
#include "platform/Event.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"

#include <cstring>

namespace acs::game {

namespace {

// FindIndex が見つからなかった時に返すセンチネル値。u32 の MAX。
constexpr u32 kInvalidIndex = 0xFFFFFFFFu;

/** NUL 終端文字列を既定アロケータへコピーする。失敗時は nullptr。 */
char* TryCopyWidgetText(const char* text) noexcept {
    const char* const source = text != nullptr ? text : "";
    const usize length = std::strlen(source);
    if (length == static_cast<usize>(-1)) return nullptr;
    char* const copy = static_cast<char*>(
        DefaultAllocator().Alloc(length + 1u));
    if (copy == nullptr) return nullptr;
    std::memcpy(copy, source, length + 1u);
    return copy;
}

/** widget が所有する文字列を解放して空にする。 */
void ReleaseWidgetText(FWidgetEntry& widget) noexcept {
    // text は公開読み取り形式を保つため const char* だが、必ず CUiLayer が
    // char* として確保した領域だけを保持する。所有権境界での解放時だけ const を外す。
    DefaultAllocator().Free(const_cast<char*>(widget.text));
    widget.text = nullptr;
}

} // namespace

CUiLayer::~CUiLayer() noexcept {
    Clear();
}

void CUiLayer::Init() noexcept {
    if (m_Initialized) {
        // 冪等性確保。重複 Init はログのみで何もしない (Shutdown 経由せず再 Init
        // するケースもあり得るが、現状は警告レベルではなく Debug で記録)。
        ACS_LOG_DEBUG("CUiLayer::Init called twice (ignored)");
        return;
    }
    // 自前の軽量 WidgetEntry 配列で Button/Text を直接保持・描画する設計のため、
    // acs::ui::Container のツリーは確保しない (リッチ widget は acs::ui を直接使う)。
    Clear();
    m_NextHandle = 1;
    m_Initialized = true;
    ACS_LOG_DEBUG("CUiLayer::Init: ready");
}

void CUiLayer::Shutdown() noexcept {
    if (!m_Initialized) {
        // 未初期化での Shutdown は no-op (冪等)。
        return;
    }
    Clear();
    m_NextHandle = 1;
    m_Initialized = false;
    ACS_LOG_DEBUG("CUiLayer::Shutdown: state cleared");
}

void CUiLayer::Tick(f32 /*dt*/) noexcept {
    if (!m_Initialized) return;
    // just_pressed は IsButtonPressed の consume-on-read で消費する方式にしたため、
    // Tick で一括クリアはしない (OnEvent→OnUpdate のフレーム順で this-frame の
    // クリックを誤って消さないため)。将来 layout 再計算 / animation 更新をここに。
}

void CUiLayer::HandleInput(const acs::FEvent& event) noexcept {
    if (!m_Initialized) return;
    switch (event.type) {
        case acs::EEventType::MouseMoved: {
            m_MouseX = event.mouse_move.x;
            m_MouseY = event.mouse_move.y;
            // hover 状態を更新 (ボタンのみ)。
            for (u32 i = 0; i < m_Widgets.Num(); ++i) {
                FWidgetEntry& e = m_Widgets[i];
                if (e.kind != EWidgetKind::Button) continue;
                e.hovered = e.visible
                    && m_MouseX >= e.pos.x && m_MouseX < e.pos.x + e.size.x
                    && m_MouseY >= e.pos.y && m_MouseY < e.pos.y + e.size.y;
            }
            break;
        }
        case acs::EEventType::MouseButtonPressed: {
            if (event.mouse_button.button != acs::EMouseButton::Left) break;
            m_PressedHandle = HitTopButton(m_MouseX, m_MouseY);
            if (m_PressedHandle != 0) {
                const u32 idx = FindIndex(m_PressedHandle);
                if (idx != kInvalidIndex) m_Widgets[idx].pressed_down = true;
            }
            break;
        }
        case acs::EEventType::MouseButtonReleased: {
            if (event.mouse_button.button != acs::EMouseButton::Left) break;
            // press 開始ボタンの上で離したらクリック成立。
            if (m_PressedHandle != 0 && HitTopButton(m_MouseX, m_MouseY) == m_PressedHandle) {
                const u32 idx = FindIndex(m_PressedHandle);
                if (idx != kInvalidIndex) m_Widgets[idx].just_pressed = true;
            }
            // 押し込み状態を全解除。
            for (u32 i = 0; i < m_Widgets.Num(); ++i) m_Widgets[i].pressed_down = false;
            m_PressedHandle = 0;
            break;
        }
        default:
            break;
    }
}

// (x,y) を含む最前面 (= 最後に追加) の visible なボタンの handle。無ければ 0。
u32 CUiLayer::HitTopButton(f32 x, f32 y) const noexcept {
    for (u32 i = static_cast<u32>(m_Widgets.Num()); i > 0; --i) {
        const FWidgetEntry& e = m_Widgets[i - 1];
        if (e.kind == EWidgetKind::Button && e.visible
            && x >= e.pos.x && x < e.pos.x + e.size.x
            && y >= e.pos.y && y < e.pos.y + e.size.y) {
            return e.handle;
        }
    }
    return 0;
}

void CUiLayer::Draw(FRenderContext& rc) const noexcept {
    if (!rc.HasSprites()) return;
    CSpriteBatch& sb = rc.Sprites();
    const bool has_font = rc.HasFont();
    for (u32 i = 0; i < m_Widgets.Num(); ++i) {
        const FWidgetEntry& e = m_Widgets[i];
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

u32 CUiLayer::WidgetCount() const noexcept {
    return static_cast<u32>(m_Widgets.Num());
}

u32 CUiLayer::AddButton(const char* label, acs::FVec2 pos, acs::FVec2 size) noexcept {
    if (!m_Initialized) {
        // Init せずに使われたら warn (Scene 側の OnEnter で Init 漏れ検出)。
        ACS_LOG_WARN("CUiLayer::AddButton called before Init (ignored)");
        return 0;
    }
    if (m_NextHandle == 0u) {
        ACS_LOG_WARN("CUiLayer::AddButton: handle space exhausted");
        return 0u;
    }
    char* const owned_text = TryCopyWidgetText(label);
    if (owned_text == nullptr) {
        ACS_LOG_WARN("CUiLayer::AddButton: text allocation failed");
        return 0u;
    }
    FWidgetEntry e{};
    e.handle       = m_NextHandle;
    e.kind         = EWidgetKind::Button;
    e.pos          = pos;
    e.size         = size;
    e.text         = owned_text;
    e.visible      = true;
    e.just_pressed = false;
    if (!m_Widgets.TryAdd(e)) {
        ReleaseWidgetText(e);
        ACS_LOG_WARN("CUiLayer::AddButton: widget allocation failed");
        return 0u;
    }
    ++m_NextHandle;
    return e.handle;
}

u32 CUiLayer::AddText(const char* text, acs::FVec2 pos) noexcept {
    if (!m_Initialized) {
        ACS_LOG_WARN("CUiLayer::AddText called before Init (ignored)");
        return 0;
    }
    if (m_NextHandle == 0u) {
        ACS_LOG_WARN("CUiLayer::AddText: handle space exhausted");
        return 0u;
    }
    char* const owned_text = TryCopyWidgetText(text);
    if (owned_text == nullptr) {
        ACS_LOG_WARN("CUiLayer::AddText: text allocation failed");
        return 0u;
    }
    FWidgetEntry e{};
    e.handle       = m_NextHandle;
    e.kind         = EWidgetKind::Text;
    e.pos          = pos;
    e.size         = acs::FVec2{0.0f, 0.0f};  // フォントメトリックから計算予定
    e.text         = owned_text;
    e.visible      = true;
    e.just_pressed = false;
    if (!m_Widgets.TryAdd(e)) {
        ReleaseWidgetText(e);
        ACS_LOG_WARN("CUiLayer::AddText: widget allocation failed");
        return 0u;
    }
    ++m_NextHandle;
    return e.handle;
}

bool CUiLayer::ConsumeButtonPress(u32 handle) noexcept {
    const u32 idx = FindIndex(handle);
    if (idx == kInvalidIndex) return false;
    FWidgetEntry& e = m_Widgets[idx];
    if (e.kind != EWidgetKind::Button || !e.just_pressed) return false;
    e.just_pressed = false;
    return true;
}

bool CUiLayer::IsButtonPressed(u32 handle) const noexcept {
    const u32 idx = FindIndex(handle);
    if (idx == kInvalidIndex) return false;
    const FWidgetEntry& e = m_Widgets[idx];
    // Text widget には押下概念がない。Button のみが押下対象。
    if (e.kind != EWidgetKind::Button) return false;
    if (!e.just_pressed) return false;
    // mutable な押下フラグだけを消費し、互換 API のワンショット動作を保つ。
    e.just_pressed = false;
    return true;
}

bool CUiLayer::SetText(u32 handle, const char* text) noexcept {
    const u32 idx = FindIndex(handle);
    if (idx == kInvalidIndex) return false;
    char* const replacement = TryCopyWidgetText(text);
    if (replacement == nullptr) return false;
    ReleaseWidgetText(m_Widgets[idx]);
    m_Widgets[idx].text = replacement;
    return true;
}

const char* CUiLayer::Text(u32 handle) const noexcept {
    const u32 idx = FindIndex(handle);
    return idx != kInvalidIndex ? m_Widgets[idx].text : nullptr;
}

void CUiLayer::SetVisible(u32 handle, bool visible) noexcept {
    const u32 idx = FindIndex(handle);
    if (idx == kInvalidIndex) {
        ACS_LOG_WARN("CUiLayer::SetVisible: invalid handle %u", handle);
        return;
    }
    m_Widgets[idx].visible = visible;
}

void CUiLayer::Remove(u32 handle) noexcept {
    const u32 idx = FindIndex(handle);
    if (idx == kInvalidIndex) {
        // 削除で invalid を渡すケースは「既に消されたものを再度消す」など
        // 起こり得るので Debug レベルに留める (Warn だとログが煩い)。
        ACS_LOG_DEBUG("CUiLayer::Remove: invalid handle %u (already removed?)", handle);
        return;
    }
    // hit test は追加順の末尾を最前面とするため、途中削除でも残りの順序を保つ。
    ReleaseWidgetText(m_Widgets[idx]);
    m_Widgets.RemoveAt(idx);
}

void CUiLayer::Clear() noexcept {
    for (u32 i = 0u; i < m_Widgets.Num(); ++i) {
        ReleaseWidgetText(m_Widgets[i]);
    }
    m_Widgets.Reset();
    m_PressedHandle = 0u;
    // m_NextHandle はリセットしない: Clear 後も以前の handle が外部に残っている
    // 可能性があり、再利用すると意図しないヒットが起こり得る。Shutdown まで
    // 単調増加を維持する。
}

u32 CUiLayer::FindIndex(u32 handle) const noexcept {
    if (handle == 0) return kInvalidIndex;
    for (u32 i = 0; i < m_Widgets.Num(); ++i) {
        if (m_Widgets[i].handle == handle) return i;
    }
    return kInvalidIndex;
}

} // namespace acs::game

// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar D — InputMap 実装 (Phase 6)
#include "gameframework/InputMap.h"
#include "platform/Input.h"

namespace acs::game {

void InputMap::BindKey(ActionId action, EKey key) noexcept {
    Binding b;
    b.action = action;
    b.kind   = BindKind::EKey;
    b.code   = static_cast<u32>(key);
    _bindings.PushBack(b);
}

void InputMap::BindMouseButton(ActionId action, EMouseButton mb) noexcept {
    Binding b;
    b.action = action;
    b.kind   = BindKind::EMouseButton;
    b.code   = static_cast<u32>(mb);
    _bindings.PushBack(b);
}

void InputMap::BindGamepad(ActionId action, EGamepadButton gb, u32 player_index) noexcept {
    Binding b;
    b.action = action;
    b.kind   = BindKind::EGamepadButton;
    b.code   = static_cast<u32>(gb);
    b.player = player_index;
    _bindings.PushBack(b);
}

void InputMap::BindAxisKeys(ActionId action, EKey neg, EKey pos) noexcept {
    Binding b;
    b.action   = action;
    b.kind     = BindKind::Axis1D;
    b.code     = static_cast<u32>(neg);
    b.code_pos = static_cast<u32>(pos);
    _bindings.PushBack(b);
}

void InputMap::Unbind(ActionId action) noexcept {
    u32 w = 0;
    for (u32 r = 0; r < _bindings.Size(); ++r) {
        if (_bindings[r].action != action) {
            if (w != r) _bindings[w] = _bindings[r];
            ++w;
        }
    }
    while (_bindings.Size() > w) _bindings.PopBack();
}

void InputMap::ClearAll() noexcept {
    _bindings.Clear();
}

bool InputMap::IsPressed(ActionId action) const noexcept {
    for (u32 i = 0; i < _bindings.Size(); ++i) {
        const Binding& b = _bindings[i];
        if (b.action != action) continue;
        switch (b.kind) {
        case BindKind::EKey:
            if (Input::IsKeyPressed(static_cast<EKey>(b.code))) return true;
            break;
        case BindKind::EMouseButton:
            if (Input::IsMouseButtonPressed(static_cast<EMouseButton>(b.code))) return true;
            break;
        case BindKind::EGamepadButton:
            if (Input::IsGamepadButtonPressed(b.player, static_cast<EGamepadButton>(b.code))) return true;
            break;
        case BindKind::Axis1D:
            // axis は Pressed の概念なし (常に false)
            break;
        }
    }
    return false;
}

bool InputMap::IsHeld(ActionId action) const noexcept {
    for (u32 i = 0; i < _bindings.Size(); ++i) {
        const Binding& b = _bindings[i];
        if (b.action != action) continue;
        switch (b.kind) {
        case BindKind::EKey:
            if (Input::IsKeyDown(static_cast<EKey>(b.code))) return true;
            break;
        case BindKind::EMouseButton:
            if (Input::IsMouseButtonDown(static_cast<EMouseButton>(b.code))) return true;
            break;
        case BindKind::EGamepadButton:
            if (Input::IsGamepadButtonDown(b.player, static_cast<EGamepadButton>(b.code))) return true;
            break;
        case BindKind::Axis1D:
            // axis は |Axis| > 0 で Held とみなす
            if (Input::IsKeyDown(static_cast<EKey>(b.code)) ||
                Input::IsKeyDown(static_cast<EKey>(b.code_pos))) return true;
            break;
        }
    }
    return false;
}

bool InputMap::IsReleased(ActionId action) const noexcept {
    for (u32 i = 0; i < _bindings.Size(); ++i) {
        const Binding& b = _bindings[i];
        if (b.action != action) continue;
        switch (b.kind) {
        case BindKind::EKey:
            if (Input::IsKeyReleased(static_cast<EKey>(b.code))) return true;
            break;
        case BindKind::EMouseButton:
            if (Input::IsMouseButtonReleased(static_cast<EMouseButton>(b.code))) return true;
            break;
        case BindKind::EGamepadButton:
            // GamepadButtonReleased が無いので Released は今フェーズ未対応 (always false)
            break;
        case BindKind::Axis1D:
            break;
        }
    }
    return false;
}

f32 InputMap::Axis(ActionId action) const noexcept {
    f32 acc = 0.0f;
    for (u32 i = 0; i < _bindings.Size(); ++i) {
        const Binding& b = _bindings[i];
        if (b.action != action) continue;
        if (b.kind != BindKind::Axis1D) continue;
        const bool n = Input::IsKeyDown(static_cast<EKey>(b.code));
        const bool p = Input::IsKeyDown(static_cast<EKey>(b.code_pos));
        // 両方押下は 0 (相殺)、片方なら ±1
        if (n && !p) acc -= 1.0f;
        else if (p && !n) acc += 1.0f;
    }
    if (acc >  1.0f) acc =  1.0f;
    if (acc < -1.0f) acc = -1.0f;
    return acc;
}

} // namespace acs::game

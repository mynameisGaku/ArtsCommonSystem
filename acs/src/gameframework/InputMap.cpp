// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar D — FInputMap 実装 (Phase 6)
#include "gameframework/InputMap.h"
#include "platform/Input.h"

namespace acs::game {

void FInputMap::BindKey(FActionId action, EKey key) noexcept {
    FBinding b;
    b.action = action;
    b.kind   = BindKind::EKey;
    b.code   = static_cast<u32>(key);
    _bindings.PushBack(b);
}

void FInputMap::BindMouseButton(FActionId action, EMouseButton mb) noexcept {
    FBinding b;
    b.action = action;
    b.kind   = BindKind::EMouseButton;
    b.code   = static_cast<u32>(mb);
    _bindings.PushBack(b);
}

void FInputMap::BindGamepad(FActionId action, EGamepadButton gb, u32 player_index) noexcept {
    FBinding b;
    b.action = action;
    b.kind   = BindKind::EGamepadButton;
    b.code   = static_cast<u32>(gb);
    b.player = player_index;
    _bindings.PushBack(b);
}

void FInputMap::BindAxisKeys(FActionId action, EKey neg, EKey pos) noexcept {
    FBinding b;
    b.action   = action;
    b.kind     = BindKind::Axis1D;
    b.code     = static_cast<u32>(neg);
    b.code_pos = static_cast<u32>(pos);
    _bindings.PushBack(b);
}

void FInputMap::Unbind(FActionId action) noexcept {
    u32 w = 0;
    for (u32 r = 0; r < _bindings.Size(); ++r) {
        if (_bindings[r].action != action) {
            if (w != r) _bindings[w] = _bindings[r];
            ++w;
        }
    }
    while (_bindings.Size() > w) _bindings.PopBack();
}

void FInputMap::ClearAll() noexcept {
    _bindings.Clear();
}

bool FInputMap::IsPressed(FActionId action) const noexcept {
    for (u32 i = 0; i < _bindings.Size(); ++i) {
        const FBinding& b = _bindings[i];
        if (b.action != action) continue;
        switch (b.kind) {
        case BindKind::EKey:
            if (FInput::IsKeyPressed(static_cast<EKey>(b.code))) return true;
            break;
        case BindKind::EMouseButton:
            if (FInput::IsMouseButtonPressed(static_cast<EMouseButton>(b.code))) return true;
            break;
        case BindKind::EGamepadButton:
            if (FInput::IsGamepadButtonPressed(b.player, static_cast<EGamepadButton>(b.code))) return true;
            break;
        case BindKind::Axis1D:
            // axis は Pressed の概念なし (常に false)
            break;
        }
    }
    return false;
}

bool FInputMap::IsHeld(FActionId action) const noexcept {
    for (u32 i = 0; i < _bindings.Size(); ++i) {
        const FBinding& b = _bindings[i];
        if (b.action != action) continue;
        switch (b.kind) {
        case BindKind::EKey:
            if (FInput::IsKeyDown(static_cast<EKey>(b.code))) return true;
            break;
        case BindKind::EMouseButton:
            if (FInput::IsMouseButtonDown(static_cast<EMouseButton>(b.code))) return true;
            break;
        case BindKind::EGamepadButton:
            if (FInput::IsGamepadButtonDown(b.player, static_cast<EGamepadButton>(b.code))) return true;
            break;
        case BindKind::Axis1D:
            // axis は |Axis| > 0 で Held とみなす
            if (FInput::IsKeyDown(static_cast<EKey>(b.code)) ||
                FInput::IsKeyDown(static_cast<EKey>(b.code_pos))) return true;
            break;
        }
    }
    return false;
}

bool FInputMap::IsReleased(FActionId action) const noexcept {
    for (u32 i = 0; i < _bindings.Size(); ++i) {
        const FBinding& b = _bindings[i];
        if (b.action != action) continue;
        switch (b.kind) {
        case BindKind::EKey:
            if (FInput::IsKeyReleased(static_cast<EKey>(b.code))) return true;
            break;
        case BindKind::EMouseButton:
            if (FInput::IsMouseButtonReleased(static_cast<EMouseButton>(b.code))) return true;
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

f32 FInputMap::Axis(FActionId action) const noexcept {
    f32 acc = 0.0f;
    for (u32 i = 0; i < _bindings.Size(); ++i) {
        const FBinding& b = _bindings[i];
        if (b.action != action) continue;
        if (b.kind != BindKind::Axis1D) continue;
        const bool n = FInput::IsKeyDown(static_cast<EKey>(b.code));
        const bool p = FInput::IsKeyDown(static_cast<EKey>(b.code_pos));
        // 両方押下は 0 (相殺)、片方なら ±1
        if (n && !p) acc -= 1.0f;
        else if (p && !n) acc += 1.0f;
    }
    if (acc >  1.0f) acc =  1.0f;
    if (acc < -1.0f) acc = -1.0f;
    return acc;
}

} // namespace acs::game

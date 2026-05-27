// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar G / Q — FSpritePack 実装
//
// data holder のみ。描画 / texture ロードは責務外。詳細は FSpritePack.h 参照。
#include "gameframework/SpritePack.h"

#include <cstring>   // strcmp

namespace acs::game {

// ============================================================================
// 内部 — name 比較
// ============================================================================

// pointer 同一を最初に試して strcmp に fallback。
// 文字列リテラル運用なら pointer 同一の高速 path で抜ける。
static bool NameEquals(const char* a, const char* b) noexcept {
    if (a == b) return true;
    if (a == nullptr || b == nullptr) return false;
    return ::strcmp(a, b) == 0;
}

// ============================================================================
// 初期化 / セットアップ
// ============================================================================

void FSpritePack::Init(const SpritePackInfo& info) noexcept {
    // 値コピーで取り込む (atlas_texture_path は caller 所有のポインタを保持)。
    // frame 配列はそのまま保持。完全 reset には ClearAll を併用する。
    _info = info;
}

// ============================================================================
// frame 追加 / 削除
// ============================================================================

void FSpritePack::AddFrame(const SpriteFrame& frame) noexcept {
    // name == nullptr は検索不能 frame になるため弾く (debug でも crash させない)。
    if (frame.name == nullptr) return;
    _frames.PushBack(frame);
}

void FSpritePack::RemoveFrame(const char* name) noexcept {
    if (name == nullptr) return;
    // 順序非保持の swap remove。複数一致しても全て削除するため、末尾から走査して
    // 末尾 swap 後に i を据え置きすることで安全にループする。
    usize i = _frames.Size();
    while (i > 0) {
        --i;
        if (NameEquals(_frames[i].name, name)) {
            _frames.RemoveAtSwap(i);
            // i はインデックスとして再使用可能だが、末尾から走査しているので
            // そのまま次のループで i-- が自然に進む。
        }
    }
}

void FSpritePack::ClearAll() noexcept {
    _frames.Clear();
}

// ============================================================================
// frame 検索
// ============================================================================

const SpriteFrame* FSpritePack::FindFrame(const char* name) const noexcept {
    if (name == nullptr) return nullptr;
    for (usize i = 0; i < _frames.Size(); ++i) {
        if (NameEquals(_frames[i].name, name)) {
            return &_frames[i];
        }
    }
    return nullptr;
}

bool FSpritePack::HasFrame(const char* name) const noexcept {
    return FindFrame(name) != nullptr;
}

// ============================================================================
// イテレーション
// ============================================================================

u32 FSpritePack::FrameCount() const noexcept {
    return static_cast<u32>(_frames.Size());
}

const SpriteFrame* FSpritePack::AllFrames(u32& out_count) const noexcept {
    out_count = static_cast<u32>(_frames.Size());
    return _frames.Data();
}

// ============================================================================
// UV 計算
// ============================================================================

acs::FVec4 FSpritePack::ComputeUv(const SpriteFrame& frame) const noexcept {
    // atlas size が未設定 (0) なら 0 除算回避で zero UV。
    if (_info.atlas_width == 0 || _info.atlas_height == 0) {
        return acs::FVec4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    const f32 inv_w = 1.0f / static_cast<f32>(_info.atlas_width);
    const f32 inv_h = 1.0f / static_cast<f32>(_info.atlas_height);
    const f32 u0 = static_cast<f32>(frame.x)            * inv_w;
    const f32 v0 = static_cast<f32>(frame.y)            * inv_h;
    const f32 u1 = static_cast<f32>(frame.x + frame.w)  * inv_w;
    const f32 v1 = static_cast<f32>(frame.y + frame.h)  * inv_h;
    return acs::FVec4(u0, v0, u1, v1);
}

} // namespace acs::game

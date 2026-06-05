// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar G / Q — FSpritePack 実装
//
// data holder のみ。描画 / texture ロードは責務外。詳細は FSpritePack.h 参照。
#include "gameframework/SpritePack.h"
#include "container/Json.h"
#include "foundation/Error.h"
#include "foundation/Move.h"

#include <cstring>   // strcmp

namespace acs::game {

/**
 * frame 名を比較する (pointer 同一 → strcmp の順)。
 *
 * @details 文字列リテラル運用なら pointer 同一の高速 path で抜ける。
 * @param a 比較する名前 A。
 * @param b 比較する名前 B。
 * @return 等しければ true (どちらかが nullptr なら false)。
 */
static bool NameEquals(const char* a, const char* b) noexcept {
    if (a == b) return true;
    if (a == nullptr || b == nullptr) return false;
    return ::strcmp(a, b) == 0;
}

/** atlas メタ情報を値コピーで取り込む (既存 frame は保持)。 */
void FSpritePack::Init(const SpritePackInfo& info) noexcept {
    // 値コピーで取り込む (atlas_texture_path は caller 所有のポインタを保持)。
    // frame 配列はそのまま保持。完全 reset には ClearAll を併用する。
    m_Info = info;
}

/** frame を追加する (name==nullptr は検索不能のため無視)。 */
void FSpritePack::AddFrame(const SpriteFrame& frame) noexcept {
    // name == nullptr は検索不能 frame になるため弾く (debug でも crash させない)。
    if (frame.name == nullptr) return;
    m_Frames.PushBack(frame);
}

/** 指定 name に一致する frame を全て削除する (nullptr は no-op、順序非保持)。 */
void FSpritePack::RemoveFrame(const char* name) noexcept {
    if (name == nullptr) return;
    // 順序非保持の swap remove。複数一致しても全て削除するため、末尾から走査して
    // 末尾 swap 後に i を据え置きすることで安全にループする。
    usize i = m_Frames.Size();
    while (i > 0) {
        --i;
        if (NameEquals(m_Frames[i].name, name)) {
            m_Frames.RemoveAtSwap(i);
            // i はインデックスとして再使用可能だが、末尾から走査しているので
            // そのまま次のループで i-- が自然に進む。
        }
    }
}

/** 全 frame を削除する (atlas メタ情報は保持)。 */
void FSpritePack::ClearAll() noexcept {
    m_Frames.Clear();
}

/** Aseprite "hash" / TexturePacker "array" の atlas JSON を読み込む。 */
TResult<void> FSpritePack::LoadAtlasJson(const char* json_text, usize len) noexcept {
    const auto pr = ParseJson(json_text, len);
    if (pr.IsErr()) return pr.Error();
    const FJsonValue& root = pr.Value();
    if (!root.IsObject()) {
        return ACS_ERR(Generic, 1410, "atlas JSON: root is not an object");
    }

    // frames は Aseprite "hash" (object) か TexturePacker "array" のどちらか。
    const FJsonValue& frames = root.Get("frames");
    u32  count;
    bool hash_form;
    if (frames.IsObject())      { count = frames.MemberCount(); hash_form = true; }
    else if (frames.IsArray())  { count = frames.Size();        hash_form = false; }
    else return ACS_ERR(Generic, 1411, "atlas JSON: 'frames' is neither object nor array");

    // 既存をクリアし名前所有バッファを Reserve (再 alloc を防ぎ SSO name を安定化)。
    m_Frames.Clear();
    m_OwnedNames.Clear();
    m_OwnedNames.Reserve(count);

    for (u32 i = 0; i < count; ++i) {
        const FJsonValue& entry = frames.At(i);   // hash/array とも i 番目の値
        const FStringView name_v = hash_form ? frames.MemberKey(i)
                                             : entry.Get("filename").AsString();
        const FJsonValue& fr = entry.Get("frame");

        FString name;
        name.Append(name_v);
        m_OwnedNames.PushBack(Move(name));         // Reserve 済 → realloc しない

        SpriteFrame sf;
        sf.name = m_OwnedNames[m_OwnedNames.Size() - 1].Data();   // 安定ポインタ
        sf.x = fr.Get("x").AsU32();
        sf.y = fr.Get("y").AsU32();
        sf.w = fr.Get("w").AsU32();
        sf.h = fr.Get("h").AsU32();
        const FJsonValue& piv = entry.Get("pivot");
        if (piv.IsObject()) {
            sf.pivot_x = piv.Get("x").AsF32(0.5f);
            sf.pivot_y = piv.Get("y").AsF32(0.5f);
        }
        m_Frames.PushBack(sf);
    }

    // meta を取り込む (image path は内部所有)。
    const FJsonValue& meta = root.Get("meta");
    m_OwnedImagePath.Clear();
    m_OwnedImagePath.Append(meta.Get("image").AsString());
    m_Info.atlas_texture_path = m_OwnedImagePath.Data();
    m_Info.atlas_width  = meta.Get("size").Get("w").AsU32();
    m_Info.atlas_height = meta.Get("size").Get("h").AsU32();
    return Ok();
}

/** 名前で frame を線形検索する (nullptr / 未検出は nullptr)。 */
const SpriteFrame* FSpritePack::FindFrame(const char* name) const noexcept {
    if (name == nullptr) return nullptr;
    for (usize i = 0; i < m_Frames.Size(); ++i) {
        if (NameEquals(m_Frames[i].name, name)) {
            return &m_Frames[i];
        }
    }
    return nullptr;
}

/** 名前で frame の有無を返す (= FindFrame != nullptr)。 */
bool FSpritePack::HasFrame(const char* name) const noexcept {
    return FindFrame(name) != nullptr;
}

/** 登録 frame 数を返す。 */
u32 FSpritePack::FrameCount() const noexcept {
    return static_cast<u32>(m_Frames.Size());
}

/** frame 配列の先頭を返し、件数を out_count に書き戻す。 */
const SpriteFrame* FSpritePack::AllFrames(u32& out_count) const noexcept {
    out_count = static_cast<u32>(m_Frames.Size());
    return m_Frames.Data();
}

/** frame 矩形を atlas size で割って [0,1] UV {u0,v0,u1,v1} を返す (size 0 は zero UV)。 */
acs::FVec4 FSpritePack::ComputeUv(const SpriteFrame& frame) const noexcept {
    // atlas size が未設定 (0) なら 0 除算回避で zero UV。
    if (m_Info.atlas_width == 0 || m_Info.atlas_height == 0) {
        return acs::FVec4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    const f32 inv_w = 1.0f / static_cast<f32>(m_Info.atlas_width);
    const f32 inv_h = 1.0f / static_cast<f32>(m_Info.atlas_height);
    const f32 u0 = static_cast<f32>(frame.x)            * inv_w;
    const f32 v0 = static_cast<f32>(frame.y)            * inv_h;
    const f32 u1 = static_cast<f32>(frame.x + frame.w)  * inv_w;
    const f32 v1 = static_cast<f32>(frame.y + frame.h)  * inv_h;
    return acs::FVec4(u0, v0, u1, v1);
}

} // namespace acs::game

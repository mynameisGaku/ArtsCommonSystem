// SPDX-License-Identifier: Apache-2.0
// atlas metadataとframeを保持し、失敗時に既存状態を保つ検証付きJSON読込を担う。
#include "gameframework/SpritePack.h"

#include "container/Json.h"
#include "foundation/Error.h"
#include "foundation/Move.h"

#include <cfloat>
#include <cstring>

namespace acs::game {

namespace {

bool NameEquals(const char* a, const char* b) noexcept {
    if (a == b) return true;
    if (a == nullptr || b == nullptr) return false;
    return ::strcmp(a, b) == 0;
}

FSpritePackLoadResult SpriteFailure(
    ESpritePackLoadError error, u32 frame = 0u,
    u16 json_subcode = 0u) noexcept {
    FSpritePackLoadResult result{};
    result.Error = error;
    result.JsonSubcode = json_subcode;
    result.Frame = frame;
    return result;
}

bool HasControlByte(FStringView text) noexcept {
    for (usize i = 0u; i < text.Size(); ++i) {
        if (static_cast<u8>(text.Data()[i]) < 0x20u) return true;
    }
    return false;
}

ESpritePackLoadError ValidateEnvelope(
    const char* text, usize length) noexcept {
    if (text == nullptr) return ESpritePackLoadError::NullInput;
    if (length == 0u) return ESpritePackLoadError::EmptyInput;
    if (length > FSpritePack::kMaxAtlasJsonBytes) {
        return ESpritePackLoadError::InputTooLarge;
    }

    u32 depth = 0u;
    usize string_length = 0u;
    bool in_string = false;
    bool escaped = false;
    for (usize i = 0u; i < length; ++i) {
        const char byte = text[i];
        if (byte == '\0') return ESpritePackLoadError::EmbeddedNul;
        if (in_string) {
            if (escaped) {
                escaped = false;
                ++string_length;
                if (string_length > FSpritePack::kMaxJsonStringBytes) {
                    return ESpritePackLoadError::JsonStringTooLong;
                }
                continue;
            }
            if (byte == '\\') {
                escaped = true;
                ++string_length;
                if (string_length > FSpritePack::kMaxJsonStringBytes) {
                    return ESpritePackLoadError::JsonStringTooLong;
                }
                continue;
            }
            if (byte == '"') {
                in_string = false;
                continue;
            }
            ++string_length;
            if (string_length > FSpritePack::kMaxJsonStringBytes) {
                return ESpritePackLoadError::JsonStringTooLong;
            }
            continue;
        }
        if (byte == '"') {
            in_string = true;
            escaped = false;
            string_length = 0u;
        } else if (byte == '{' || byte == '[') {
            ++depth;
            if (depth > FSpritePack::kMaxJsonDepth) {
                return ESpritePackLoadError::JsonDepthExceeded;
            }
        } else if ((byte == '}' || byte == ']') && depth != 0u) {
            --depth;
        }
    }
    return ESpritePackLoadError::None;
}

ESpritePackLoadError ValidateDom(
    const FJsonValue& value, u32 depth, u32& node_count) noexcept {
    if (depth > FSpritePack::kMaxJsonDepth) {
        return ESpritePackLoadError::JsonDepthExceeded;
    }
    if (node_count == FSpritePack::kMaxJsonNodes) {
        return ESpritePackLoadError::JsonNodeLimitExceeded;
    }
    ++node_count;

    if (value.IsNumber()) {
        const f64 number = value.AsNumber();
        if (!(number == number) || number > DBL_MAX || number < -DBL_MAX) {
            return ESpritePackLoadError::NonFiniteNumber;
        }
    } else if (value.IsString()) {
        const FStringView text = value.AsString();
        if (text.Size() > FSpritePack::kMaxJsonStringBytes) {
            return ESpritePackLoadError::JsonStringTooLong;
        }
        for (usize i = 0u; i < text.Size(); ++i) {
            if (text.Data()[i] == '\0') {
                return ESpritePackLoadError::EmbeddedNul;
            }
        }
    } else if (value.IsObject()) {
        const u32 members = value.MemberCount();
        if (members > FSpritePack::kMaxJsonObjectMembers) {
            return ESpritePackLoadError::JsonNodeLimitExceeded;
        }
        for (u32 i = 0u; i < members; ++i) {
            const FStringView key = value.MemberKey(i);
            if (key.Size() > FSpritePack::kMaxJsonStringBytes) {
                return ESpritePackLoadError::JsonStringTooLong;
            }
            for (usize byte = 0u; byte < key.Size(); ++byte) {
                if (key.Data()[byte] == '\0') {
                    return ESpritePackLoadError::EmbeddedNul;
                }
            }
            for (u32 prior = 0u; prior < i; ++prior) {
                if (key == value.MemberKey(prior)) {
                    return ESpritePackLoadError::DuplicateMember;
                }
            }
            const ESpritePackLoadError child_error =
                ValidateDom(value.At(i), depth + 1u, node_count);
            if (child_error != ESpritePackLoadError::None) return child_error;
        }
    } else if (value.IsArray()) {
        for (u32 i = 0u; i < value.Size(); ++i) {
            const ESpritePackLoadError child_error =
                ValidateDom(value.At(i), depth + 1u, node_count);
            if (child_error != ESpritePackLoadError::None) return child_error;
        }
    }
    return ESpritePackLoadError::None;
}

bool TryReadU32(const FJsonValue& value, u32& output) noexcept {
    if (!value.IsNumber()) return false;
    const f64 number = value.AsNumber();
    if (!(number == number) || number < 0.0 || number > 4294967295.0) {
        return false;
    }
    const u32 converted = static_cast<u32>(number);
    if (static_cast<f64>(converted) != number) return false;
    output = converted;
    return true;
}

bool TryReadPivot(const FJsonValue& value, f32& output) noexcept {
    if (!value.IsNumber()) return false;
    const f64 number = value.AsNumber();
    if (!(number == number) || number < 0.0 || number > 1.0) return false;
    output = static_cast<f32>(number);
    return output == output;
}

FStringView FrameNameAt(
    const FJsonValue& frames, bool hash_form, u32 index) noexcept {
    return hash_form
        ? frames.MemberKey(index)
        : frames.At(index).Get("filename").AsString();
}

} // namespace

const char* SpritePackLoadErrorName(ESpritePackLoadError error) noexcept {
    switch (error) {
        case ESpritePackLoadError::None: return "None";
        case ESpritePackLoadError::NullInput: return "NullInput";
        case ESpritePackLoadError::EmptyInput: return "EmptyInput";
        case ESpritePackLoadError::InputTooLarge: return "InputTooLarge";
        case ESpritePackLoadError::EmbeddedNul: return "EmbeddedNul";
        case ESpritePackLoadError::JsonDepthExceeded: return "JsonDepthExceeded";
        case ESpritePackLoadError::JsonStringTooLong: return "JsonStringTooLong";
        case ESpritePackLoadError::JsonNodeLimitExceeded: return "JsonNodeLimitExceeded";
        case ESpritePackLoadError::JsonSyntaxError: return "JsonSyntaxError";
        case ESpritePackLoadError::RootTypeMismatch: return "RootTypeMismatch";
        case ESpritePackLoadError::DuplicateMember: return "DuplicateMember";
        case ESpritePackLoadError::MissingMember: return "MissingMember";
        case ESpritePackLoadError::MemberTypeMismatch: return "MemberTypeMismatch";
        case ESpritePackLoadError::InvalidInteger: return "InvalidInteger";
        case ESpritePackLoadError::NonFiniteNumber: return "NonFiniteNumber";
        case ESpritePackLoadError::FrameLimitExceeded: return "FrameLimitExceeded";
        case ESpritePackLoadError::NameTooLong: return "NameTooLong";
        case ESpritePackLoadError::DuplicateFrameName: return "DuplicateFrameName";
        case ESpritePackLoadError::InvalidAtlasSize: return "InvalidAtlasSize";
        case ESpritePackLoadError::InvalidFrameRect: return "InvalidFrameRect";
        case ESpritePackLoadError::InvalidPivot: return "InvalidPivot";
        case ESpritePackLoadError::ImagePathTooLong: return "ImagePathTooLong";
        case ESpritePackLoadError::AllocationFailure: return "AllocationFailure";
    }
    return "Unknown";
}

void FSpritePack::Init(const FSpritePackInfo& info) noexcept {
    m_Info = info;
}

void FSpritePack::AddFrame(const FSpriteFrame& frame) noexcept {
    if (frame.name != nullptr) m_Frames.Add(frame);
}

void FSpritePack::RemoveFrame(const char* name) noexcept {
    if (name == nullptr) return;
    usize index = m_Frames.Num();
    while (index != 0u) {
        --index;
        if (NameEquals(m_Frames[index].name, name)) {
            m_Frames.RemoveAtSwap(index);
        }
    }
}

void FSpritePack::ClearAll() noexcept {
    m_Frames.Reset();
    m_OwnedNames.Reset();
}

FSpritePackLoadResult FSpritePack::TryLoadAtlasJson(
    const char* json_text, usize len) noexcept {
    const ESpritePackLoadError envelope_error =
        ValidateEnvelope(json_text, len);
    if (envelope_error != ESpritePackLoadError::None) {
        return SpriteFailure(envelope_error);
    }

    auto parsed = ParseJson(json_text, len);
    if (parsed.IsErr()) {
        return SpriteFailure(
            ESpritePackLoadError::JsonSyntaxError, 0u,
            parsed.Error().subcode);
    }
    const FJsonValue& root = parsed.Value();
    u32 node_count = 0u;
    const ESpritePackLoadError dom_error =
        ValidateDom(root, 0u, node_count);
    if (dom_error != ESpritePackLoadError::None) {
        return SpriteFailure(dom_error);
    }
    if (!root.IsObject()) {
        return SpriteFailure(ESpritePackLoadError::RootTypeMismatch);
    }

    const FJsonValue* frames = root.Find("frames");
    const FJsonValue* meta = root.Find("meta");
    if (frames == nullptr || meta == nullptr) {
        return SpriteFailure(ESpritePackLoadError::MissingMember);
    }
    const bool hash_form = frames->IsObject();
    if (!hash_form && !frames->IsArray()) {
        return SpriteFailure(ESpritePackLoadError::MemberTypeMismatch);
    }
    const u32 frame_count =
        hash_form ? frames->MemberCount() : frames->Size();
    if (frame_count > kMaxFrames) {
        return SpriteFailure(ESpritePackLoadError::FrameLimitExceeded);
    }
    if (!meta->IsObject()) {
        return SpriteFailure(ESpritePackLoadError::MemberTypeMismatch);
    }

    const FJsonValue* image = meta->Find("image");
    const FJsonValue* size = meta->Find("size");
    if (image == nullptr || size == nullptr) {
        return SpriteFailure(ESpritePackLoadError::MissingMember);
    }
    if (!image->IsString() || !size->IsObject()) {
        return SpriteFailure(ESpritePackLoadError::MemberTypeMismatch);
    }
    const FStringView image_path = image->AsString();
    if (image_path.IsEmpty() || image_path.Size() > kMaxImagePathBytes ||
        HasControlByte(image_path)) {
        return SpriteFailure(ESpritePackLoadError::ImagePathTooLong);
    }

    const FJsonValue* atlas_width_value = size->Find("w");
    const FJsonValue* atlas_height_value = size->Find("h");
    if (atlas_width_value == nullptr || atlas_height_value == nullptr) {
        return SpriteFailure(ESpritePackLoadError::MissingMember);
    }
    u32 atlas_width = 0u;
    u32 atlas_height = 0u;
    if (!TryReadU32(*atlas_width_value, atlas_width) ||
        !TryReadU32(*atlas_height_value, atlas_height)) {
        return SpriteFailure(
            atlas_width_value->IsNumber() && atlas_height_value->IsNumber()
                ? ESpritePackLoadError::InvalidInteger
                : ESpritePackLoadError::MemberTypeMismatch);
    }
    if (atlas_width == 0u || atlas_height == 0u ||
        atlas_width > kMaxAtlasDimension ||
        atlas_height > kMaxAtlasDimension) {
        return SpriteFailure(ESpritePackLoadError::InvalidAtlasSize);
    }

    for (u32 index = 0u; index < frame_count; ++index) {
        const FJsonValue& entry = frames->At(index);
        if (!entry.IsObject()) {
            return SpriteFailure(
                ESpritePackLoadError::MemberTypeMismatch, index);
        }
        if (!hash_form) {
            const FJsonValue* filename = entry.Find("filename");
            if (filename == nullptr) {
                return SpriteFailure(
                    ESpritePackLoadError::MissingMember, index);
            }
            if (!filename->IsString()) {
                return SpriteFailure(
                    ESpritePackLoadError::MemberTypeMismatch, index);
            }
        }
        const FStringView name = FrameNameAt(*frames, hash_form, index);
        if (name.IsEmpty() || name.Size() > kMaxFrameNameBytes ||
            HasControlByte(name)) {
            return SpriteFailure(ESpritePackLoadError::NameTooLong, index);
        }
        for (u32 prior = 0u; prior < index; ++prior) {
            if (name == FrameNameAt(*frames, hash_form, prior)) {
                return SpriteFailure(
                    ESpritePackLoadError::DuplicateFrameName, index);
            }
        }

        const FJsonValue* rectangle = entry.Find("frame");
        if (rectangle == nullptr) {
            return SpriteFailure(ESpritePackLoadError::MissingMember, index);
        }
        if (!rectangle->IsObject()) {
            return SpriteFailure(
                ESpritePackLoadError::MemberTypeMismatch, index);
        }
        const FJsonValue* x_value = rectangle->Find("x");
        const FJsonValue* y_value = rectangle->Find("y");
        const FJsonValue* width_value = rectangle->Find("w");
        const FJsonValue* height_value = rectangle->Find("h");
        if (x_value == nullptr || y_value == nullptr ||
            width_value == nullptr || height_value == nullptr) {
            return SpriteFailure(ESpritePackLoadError::MissingMember, index);
        }
        u32 x = 0u;
        u32 y = 0u;
        u32 width = 0u;
        u32 height = 0u;
        if (!x_value->IsNumber() || !y_value->IsNumber() ||
            !width_value->IsNumber() || !height_value->IsNumber()) {
            return SpriteFailure(
                ESpritePackLoadError::MemberTypeMismatch, index);
        }
        if (!TryReadU32(*x_value, x) || !TryReadU32(*y_value, y) ||
            !TryReadU32(*width_value, width) ||
            !TryReadU32(*height_value, height)) {
            return SpriteFailure(
                ESpritePackLoadError::InvalidInteger, index);
        }
        if (width == 0u || height == 0u ||
            x > atlas_width || y > atlas_height ||
            width > atlas_width - x || height > atlas_height - y) {
            return SpriteFailure(
                ESpritePackLoadError::InvalidFrameRect, index);
        }

        const FJsonValue* pivot = entry.Find("pivot");
        if (pivot != nullptr) {
            if (!pivot->IsObject()) {
                return SpriteFailure(
                    ESpritePackLoadError::MemberTypeMismatch, index);
            }
            const FJsonValue* pivot_x = pivot->Find("x");
            const FJsonValue* pivot_y = pivot->Find("y");
            f32 unused_x = 0.0f;
            f32 unused_y = 0.0f;
            if (pivot_x == nullptr || pivot_y == nullptr ||
                !TryReadPivot(*pivot_x, unused_x) ||
                !TryReadPivot(*pivot_y, unused_y)) {
                return SpriteFailure(
                    ESpritePackLoadError::InvalidPivot, index);
            }
        }
    }

    FString staged_image(*m_OwnedImagePath.GetAllocator());
    TArray<FString> staged_names(*m_OwnedNames.GetAllocator());
    TArray<FSpriteFrame> staged_frames(*m_Frames.GetAllocator());
    if (!staged_image.TryAppend(image_path) ||
        !staged_names.TryReserve(frame_count) ||
        !staged_frames.TryReserve(frame_count)) {
        return SpriteFailure(ESpritePackLoadError::AllocationFailure);
    }

    for (u32 index = 0u; index < frame_count; ++index) {
        const FJsonValue& entry = frames->At(index);
        const FStringView name = FrameNameAt(*frames, hash_form, index);
        FString owned_name(*m_OwnedNames.GetAllocator());
        if (!owned_name.TryAppend(name) ||
            !staged_names.TryAdd(Move(owned_name))) {
            return SpriteFailure(
                ESpritePackLoadError::AllocationFailure, index);
        }

        const FJsonValue& rectangle = entry.Get("frame");
        FSpriteFrame frame{};
        frame.name = staged_names.Last().Data();
        (void)TryReadU32(rectangle.Get("x"), frame.x);
        (void)TryReadU32(rectangle.Get("y"), frame.y);
        (void)TryReadU32(rectangle.Get("w"), frame.w);
        (void)TryReadU32(rectangle.Get("h"), frame.h);
        const FJsonValue* pivot = entry.Find("pivot");
        if (pivot != nullptr) {
            (void)TryReadPivot(pivot->Get("x"), frame.pivot_x);
            (void)TryReadPivot(pivot->Get("y"), frame.pivot_y);
        }
        if (!staged_frames.TryAdd(frame)) {
            return SpriteFailure(
                ESpritePackLoadError::AllocationFailure, index);
        }
    }

    m_Frames = Move(staged_frames);
    m_OwnedNames = Move(staged_names);
    m_OwnedImagePath = Move(staged_image);
    m_Info.atlas_texture_path = m_OwnedImagePath.Data();
    m_Info.atlas_width = atlas_width;
    m_Info.atlas_height = atlas_height;
    return FSpritePackLoadResult{};
}

TResult<void> FSpritePack::LoadAtlasJson(
    const char* json_text, usize len) noexcept {
    const FSpritePackLoadResult result = TryLoadAtlasJson(json_text, len);
    if (result) return Ok();
    const EErrCategory category =
        result.Error == ESpritePackLoadError::AllocationFailure
        ? EErrCategory::Memory
        : EErrCategory::Generic;
    return FErrorCode(
        category, static_cast<u16>(result.Error),
        SpritePackLoadErrorName(result.Error), FSourceLoc::Current());
}

const FSpriteFrame* FSpritePack::FindFrame(const char* name) const noexcept {
    if (name == nullptr) return nullptr;
    for (usize i = 0u; i < m_Frames.Num(); ++i) {
        if (NameEquals(m_Frames[i].name, name)) return &m_Frames[i];
    }
    return nullptr;
}

bool FSpritePack::HasFrame(const char* name) const noexcept {
    return FindFrame(name) != nullptr;
}

u32 FSpritePack::FrameCount() const noexcept {
    return m_Frames.Num() > 0xFFFFFFFFu
        ? 0xFFFFFFFFu
        : static_cast<u32>(m_Frames.Num());
}

const FSpriteFrame* FSpritePack::AllFrames(u32& out_count) const noexcept {
    out_count = FrameCount();
    return m_Frames.GetData();
}

acs::FVec4 FSpritePack::ComputeUv(
    const FSpriteFrame& frame) const noexcept {
    if (m_Info.atlas_width == 0u || m_Info.atlas_height == 0u) {
        return acs::FVec4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    const f32 inverse_width =
        1.0f / static_cast<f32>(m_Info.atlas_width);
    const f32 inverse_height =
        1.0f / static_cast<f32>(m_Info.atlas_height);
    const f32 x = static_cast<f32>(frame.x);
    const f32 y = static_cast<f32>(frame.y);
    return acs::FVec4(
        x * inverse_width,
        y * inverse_height,
        (x + static_cast<f32>(frame.w)) * inverse_width,
        (y + static_cast<f32>(frame.h)) * inverse_height);
}

} // namespace acs::game

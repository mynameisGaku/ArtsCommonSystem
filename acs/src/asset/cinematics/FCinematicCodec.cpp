// SPDX-License-Identifier: Apache-2.0
#include "asset/cinematics/FCinematicCodec.h"
#include "asset/cinematics/ECinematicFormatError.h"

#include <cmath>
#include <cstring>

namespace acs::asset {

namespace {

/** .cine v1の固定識別子です。 */
constexpr u8 kMagic[8] = {'A', 'C', 'S', 'C', 'I', 'N', 'E', 0};
/** f32とu32のbit搬送サイズを固定します。 */
static_assert(sizeof(f32) == sizeof(u32));

// little-endian u16値を読み取ります。
u16 ReadU16(const byte* p) noexcept
{
    return static_cast<u16>(static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8u));
}

// little-endian u32値を読み取ります。
u32 ReadU32(const byte* p) noexcept
{
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8u) | (static_cast<u32>(p[2]) << 16u) |
           (static_cast<u32>(p[3]) << 24u);
}

// バイト列からf32値を復元します。
f32 ReadF32(const byte* p) noexcept
{
    const u32 bits = ReadU32(p);
    f32 value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// u16値をlittle-endianで書き込みます。
void WriteU16(byte* p, u16 value) noexcept
{
    p[0] = static_cast<byte>(value & 0xffu);
    p[1] = static_cast<byte>((value >> 8u) & 0xffu);
}

// u32値をlittle-endianで書き込みます。
void WriteU32(byte* p, u32 value) noexcept
{
    p[0] = static_cast<byte>(value & 0xffu);
    p[1] = static_cast<byte>((value >> 8u) & 0xffu);
    p[2] = static_cast<byte>((value >> 16u) & 0xffu);
    p[3] = static_cast<byte>((value >> 24u) & 0xffu);
}

// f32値をバイト列へ書き込みます。
void WriteF32(byte* p, f32 value) noexcept
{
    u32 bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    WriteU32(p, bits);
}

// 期間と時刻が有限で0以上かを確認します。
bool IsFiniteNonNegative(f32 value) noexcept
{
    return std::isfinite(value) && value >= 0.0f;
}

// レコード種別が定義済みかを確認します。
bool IsKind(u8 value) noexcept
{
    return value <= static_cast<u8>(ECinematicEventKind::FireEvent);
}

// payload文字列のUTF-8とNUL禁止を確認します。
bool IsTextValid(const byte* data, u32 size) noexcept
{
    u32 index = 0u;
    while (index < size) {
        const u8 lead = data[index];
        if (lead == 0u) return false;
        u32 width = 1u;
        u32 codepoint = lead;
        if (lead >= 0xc2u && lead <= 0xdfu) {
            width = 2u;
            codepoint = lead & 0x1fu;
        } else if (lead >= 0xe0u && lead <= 0xefu) {
            width = 3u;
            codepoint = lead & 0x0fu;
        } else if (lead >= 0xf0u && lead <= 0xf4u) {
            width = 4u;
            codepoint = lead & 0x07u;
        } else if (lead >= 0x80u)
            return false;
        if (width > size - index) return false;
        for (u32 offset = 1u; offset < width; ++offset) {
            const u8 tail = data[index + offset];
            if ((tail & 0xc0u) != 0x80u) return false;
            codepoint = (codepoint << 6u) | (tail & 0x3fu);
        }
        if ((width == 2u && codepoint < 0x80u) || (width == 3u && codepoint < 0x800u) ||
            (width == 4u && codepoint < 0x10000u) || codepoint > 0x10ffffu ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu))
            return false;
        index += width;
    }
    return true;
}

// 形式エラーをACSのエラーコードへ変換します。
template<typename T>
TResult<T> FormatError(ECinematicFormatError error) noexcept
{
    const u16 subcode = static_cast<u16>(900u + static_cast<u16>(error));
    if (error == ECinematicFormatError::OutOfMemory)
        return ACS_ERR(Memory, subcode, "FCinematicCodec: allocation failed");
    return ACS_ERR(Asset, subcode, "FCinematicCodec: invalid .cine bytes");
}

} // namespace

TResult<TSharedPtr<ACinematicAsset>> FCinematicCodec::Decode(const TArray<byte>& bytes) noexcept
{
    if (bytes.Num() < kHeaderSize) return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::Truncated);
    if (std::memcmp(bytes.GetData(), kMagic, sizeof(kMagic)) != 0)
        return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::InvalidMagic);
    // headerとrecordを読む入力バイト列です。
    const byte* const data = bytes.GetData();
    if (ReadU16(data + 8u) != kVersion)
        return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::UnsupportedVersion);
    if (ReadU16(data + 10u) != kHeaderSize || ReadU32(data + 12u) != 0u || ReadU32(data + 28u) != 0u)
        return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::InvalidHeader);
    // headerに保存されたイベント数、duration、record領域サイズです。
    const u32 count = ReadU32(data + 16u);
    const f32 duration = ReadF32(data + 20u);
    const u32 section_bytes = ReadU32(data + 24u);
    if (count > kMaxEvents) return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::SizeLimit);
    if (section_bytes > bytes.Num() - kHeaderSize)
        return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::Truncated);
    if (!IsFiniteNonNegative(duration))
        return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::InvalidNumber);
    if (section_bytes < bytes.Num() - kHeaderSize)
        return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::TrailingData);

    // Decode結果を成功時だけassetへ渡すための一時イベント列です。
    TArray<FCinematicEvent> events;
    if (!events.TryReserve(count)) return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::OutOfMemory);
    // 現在のrecord位置と時刻順検証用の直前時刻です。
    usize cursor = kHeaderSize;
    f32 previous_time = 0.0f;
    for (u32 index = 0u; index < count; ++index) {
        if (cursor > bytes.Num() || bytes.Num() - cursor < FCinematicCodec::kRecordHeaderSize)
            return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::Truncated);
        // record headerとpayloadを指す入力範囲です。
        const byte* const record = data + cursor;
        const u8 kind_raw = record[0];
        const u8 record_reserved = record[1];
        const u16 reserved = ReadU16(record + 2u);
        const f32 time_sec = ReadF32(record + 4u);
        const u32 payload_size = ReadU32(record + 8u);
        const u32 record_size = ReadU32(record + 12u);
        if (!IsKind(kind_raw)) return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::UnknownKind);
        if (record_reserved != 0u || reserved != 0u || ReadU32(record + 16u) != 0u)
            return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::InvalidHeader);
        if (payload_size > 0xffffffffu - static_cast<u32>(FCinematicCodec::kRecordHeaderSize))
            return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::SizeLimit);
        if (record_size != FCinematicCodec::kRecordHeaderSize + payload_size)
            return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::InvalidHeader);
        if (record_size > bytes.Num() - cursor)
            return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::Truncated);
        // 種別ごとの値を読むpayload範囲です。
        const byte* const payload = record + FCinematicCodec::kRecordHeaderSize;
        if (!IsFiniteNonNegative(time_sec))
            return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::InvalidNumber);
        if (time_sec < previous_time)
            return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::InvalidOrder);
        // 検証済みrecordを組み立てる所有値です。
        FCinematicEvent event{};
        event.time_sec = time_sec;
        event.kind = static_cast<ECinematicEventKind>(kind_raw);
        if (event.kind == ECinematicEventKind::Wait) {
            if (payload_size != 0u)
                return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::InvalidEvent);
        } else if (event.kind == ECinematicEventKind::MoveCamera) {
            if (payload_size != 16u)
                return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::InvalidEvent);
            event.target_pos.x = ReadF32(payload + 0u);
            event.target_pos.y = ReadF32(payload + 4u);
            event.camera_zoom = ReadF32(payload + 8u);
            event.camera_duration = ReadF32(payload + 12u);
            if (!std::isfinite(event.target_pos.x) || !std::isfinite(event.target_pos.y) ||
                !std::isfinite(event.camera_zoom) || event.camera_zoom <= 0.0f ||
                !IsFiniteNonNegative(event.camera_duration))
                return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::InvalidNumber);
        } else if (event.kind == ECinematicEventKind::Dialogue) {
            if (payload_size < 4u) return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::Truncated);
            const u32 text_size = ReadU32(payload);
            if (text_size != payload_size - 4u)
                return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::InvalidHeader);
            if (text_size > ACinematicAsset::kMaxTextBytes)
                return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::SizeLimit);
            if (!IsTextValid(payload + 4u, text_size))
                return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::InvalidText);
            if (!event.text.TryAppend(FStringView(reinterpret_cast<const char*>(payload + 4u), text_size)))
                return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::OutOfMemory);
        } else if (event.kind == ECinematicEventKind::Music) {
            if (payload_size < 8u) return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::Truncated);
            event.music_fade = ReadF32(payload);
            const u32 text_size = ReadU32(payload + 4u);
            if (!IsFiniteNonNegative(event.music_fade))
                return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::InvalidNumber);
            if (text_size != payload_size - 8u)
                return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::InvalidHeader);
            if (text_size > ACinematicAsset::kMaxTextBytes)
                return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::SizeLimit);
            if (!IsTextValid(payload + 8u, text_size))
                return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::InvalidText);
            if (!event.text.TryAppend(FStringView(reinterpret_cast<const char*>(payload + 8u), text_size)))
                return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::OutOfMemory);
        } else {
            if (payload_size != 4u)
                return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::InvalidEvent);
            event.event_id = ReadU32(payload);
        }
        if (!events.TryAdd(Move(event)))
            return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::OutOfMemory);
        previous_time = time_sec;
        cursor += record_size;
    }
    if (cursor != bytes.Num()) return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::TrailingData);
    if (count != 0u && duration < previous_time)
        return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::InvalidOrder);
    // 最終asset生成の結果をcodecのtyped errorへ変換します。
    TResult<TSharedPtr<ACinematicAsset>> result = ACinematicAsset::TryCreate(Move(events), duration);
    if (result.IsOk()) return result;
    if (result.Error().category == EErrCategory::Memory)
        return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::OutOfMemory);
    return FormatError<TSharedPtr<ACinematicAsset>>(ECinematicFormatError::InvalidEvent);
}

TResult<TArray<byte>> FCinematicCodec::Encode(const ACinematicAsset& asset) noexcept
{
    // Encode対象の検証済みイベント列です。
    const TArray<FCinematicEvent>& events = asset.Events();
    if (events.Num() > kMaxEvents) return FormatError<TArray<byte>>(ECinematicFormatError::SizeLimit);
    // record領域の総byte数を先に計算します。
    usize section_size = 0u;
    for (const FCinematicEvent& event : events) {
        usize payload_size = 0u;
        switch (event.kind) {
        case ECinematicEventKind::Wait:
            payload_size = 0u;
            break;
        case ECinematicEventKind::MoveCamera:
            payload_size = 16u;
            break;
        case ECinematicEventKind::Dialogue:
            payload_size = 4u + event.text.Size();
            break;
        case ECinematicEventKind::Music:
            payload_size = 8u + event.text.Size();
            break;
        case ECinematicEventKind::FireEvent:
            payload_size = 4u;
            break;
        default:
            return FormatError<TArray<byte>>(ECinematicFormatError::UnknownKind);
        }
        if (payload_size > 0xffffffffu - FCinematicCodec::kRecordHeaderSize ||
            section_size > static_cast<usize>(-1) - FCinematicCodec::kRecordHeaderSize - payload_size)
            return FormatError<TArray<byte>>(ECinematicFormatError::SizeLimit);
        section_size += FCinematicCodec::kRecordHeaderSize + payload_size;
    }
    if (section_size > 0xffffffffu || section_size > static_cast<usize>(-1) - kHeaderSize)
        return FormatError<TArray<byte>>(ECinematicFormatError::SizeLimit);
    // canonical headerとrecordを格納する出力バイト列です。
    TArray<byte> bytes;
    if (!bytes.TrySetNum(kHeaderSize + section_size))
        return FormatError<TArray<byte>>(ECinematicFormatError::OutOfMemory);
    byte* const data = bytes.GetData();
    std::memcpy(data, kMagic, sizeof(kMagic));
    WriteU16(data + 8u, kVersion);
    WriteU16(data + 10u, kHeaderSize);
    WriteU32(data + 12u, 0u);
    WriteU32(data + 16u, static_cast<u32>(events.Num()));
    WriteF32(data + 20u, asset.DurationSec());
    WriteU32(data + 24u, static_cast<u32>(section_size));
    WriteU32(data + 28u, 0u);
    usize cursor = kHeaderSize;
    for (const FCinematicEvent& event : events) {
        // 現在のrecordへ固定headerを書き込みます。
        byte* const record = data + cursor;
        const u32 payload_size = event.kind == ECinematicEventKind::Wait         ? 0u
                                 : event.kind == ECinematicEventKind::MoveCamera ? 16u
                                 : event.kind == ECinematicEventKind::Dialogue
                                     ? static_cast<u32>(4u + event.text.Size())
                                 : event.kind == ECinematicEventKind::Music ? static_cast<u32>(8u + event.text.Size())
                                                                            : 4u;
        record[0] = static_cast<byte>(event.kind);
        record[1] = 0u;
        WriteU16(record + 2u, 0u);
        WriteF32(record + 4u, event.time_sec);
        WriteU32(record + 8u, payload_size);
        WriteU32(record + 12u, static_cast<u32>(FCinematicCodec::kRecordHeaderSize + payload_size));
        WriteU32(record + 16u, 0u);
        byte* const payload = record + FCinematicCodec::kRecordHeaderSize;
        if (event.kind == ECinematicEventKind::MoveCamera) {
            WriteF32(payload, event.target_pos.x);
            WriteF32(payload + 4u, event.target_pos.y);
            WriteF32(payload + 8u, event.camera_zoom);
            WriteF32(payload + 12u, event.camera_duration);
        } else if (event.kind == ECinematicEventKind::Dialogue) {
            WriteU32(payload, static_cast<u32>(event.text.Size()));
            if (event.text.Size() != 0u) std::memcpy(payload + 4u, event.text.Data(), event.text.Size());
        } else if (event.kind == ECinematicEventKind::Music) {
            WriteF32(payload, event.music_fade);
            WriteU32(payload + 4u, static_cast<u32>(event.text.Size()));
            if (event.text.Size() != 0u) std::memcpy(payload + 8u, event.text.Data(), event.text.Size());
        } else if (event.kind == ECinematicEventKind::FireEvent) {
            WriteU32(payload, event.event_id);
        }
        cursor += FCinematicCodec::kRecordHeaderSize + payload_size;
    }
    return TResult<TArray<byte>>(OkInit, Move(bytes));
}

} // namespace acs::asset

// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "asset/AssetRegistry.h"
#include "asset/cinematics/CCinematicAssetLoader.h"
#include "asset/cinematics/ECinematicFormatError.h"
#include "asset/cinematics/FCinematicCodec.h"
#include "gameframework/cinematics/CCinematicPlayer.h"
#include "platform/FileSystem.h"

#include <cmath>
#include <cstring>

using namespace acs;
using namespace acs::asset;
using namespace acs::game;

namespace {

// 全kindの順序とpayloadを作る入力イベント列を返します。
TArray<FCinematicEvent> MakeEvents()
{
    TArray<FCinematicEvent> events;
    FCinematicEvent wait{};
    wait.time_sec = 0.0f;
    EXPECT_TRUE(events.TryAdd(wait));

    FCinematicEvent camera{};
    camera.time_sec = 1.0f;
    camera.kind = ECinematicEventKind::MoveCamera;
    camera.target_pos = FVec2{2.0f, 3.0f};
    camera.camera_zoom = 1.25f;
    camera.camera_duration = 0.5f;
    EXPECT_TRUE(events.TryAdd(Move(camera)));

    FCinematicEvent dialogue{};
    dialogue.time_sec = 1.0f;
    dialogue.kind = ECinematicEventKind::Dialogue;
    EXPECT_TRUE(dialogue.text.TryAppend(FStringView("hello")));
    EXPECT_TRUE(events.TryAdd(Move(dialogue)));

    FCinematicEvent music{};
    music.time_sec = 1.0f;
    music.kind = ECinematicEventKind::Music;
    music.music_fade = 0.25f;
    EXPECT_TRUE(music.text.TryAppend(FStringView("theme")));
    EXPECT_TRUE(events.TryAdd(Move(music)));

    FCinematicEvent fire{};
    fire.time_sec = 1.0f;
    fire.kind = ECinematicEventKind::FireEvent;
    fire.event_id = 42u;
    EXPECT_TRUE(events.TryAdd(Move(fire)));
    return events;
}

// テスト用little-endian u16を指定位置へ書き込みます。
void PutU16(TArray<byte>& bytes, usize offset, u16 value) noexcept
{
    bytes[offset] = static_cast<byte>(value & 0xffu);
    bytes[offset + 1u] = static_cast<byte>((value >> 8u) & 0xffu);
}

// テスト用little-endian u32を指定位置へ書き込みます。
void PutU32(TArray<byte>& bytes, usize offset, u32 value) noexcept
{
    bytes[offset] = static_cast<byte>(value & 0xffu);
    bytes[offset + 1u] = static_cast<byte>((value >> 8u) & 0xffu);
    bytes[offset + 2u] = static_cast<byte>((value >> 16u) & 0xffu);
    bytes[offset + 3u] = static_cast<byte>((value >> 24u) & 0xffu);
}

// テスト用f32のbit列をlittle-endianで書き込みます。
void PutF32(TArray<byte>& bytes, usize offset, f32 value) noexcept
{
    u32 bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    PutU32(bytes, offset, bits);
}

// 指定byte数のASCII文字列をテスト入力として生成します。
FString MakeSizedText(usize size)
{
    TArray<char> chars;
    if (!chars.TryReserve(size)) return FString();
    for (usize index = 0u; index < size; ++index) {
        if (!chars.TryAdd('x')) return FString();
    }
    return FString(FStringView(chars.GetData(), chars.Num()));
}

/** 再生callbackの順序とpayloadを記録する検証状態です。 */
struct FCallbackState {
    /** 発火したkindの順序です。 */
    u32 order[4] = {};
    /** 発火したcallback数です。 */
    u32 order_count = 0u;
    /** カメラpayloadを受け取った値です。 */
    FVec2 camera_pos{};
    f32 camera_zoom = 0.0f;
    f32 camera_duration = 0.0f;
    /** Dialogue文字列を受け取ったポインターです。 */
    const char* dialogue = nullptr;
    /** Music文字列とfadeを受け取った値です。 */
    const char* music = nullptr;
    f32 music_fade = 0.0f;
    /** FireEvent識別子を受け取った値です。 */
    u32 fire_id = 0u;
};

// Camera callbackのpayloadと発火順を記録します。
void OnCamera(void* user, FVec2 position, f32 zoom, f32 duration) noexcept
{
    FCallbackState& state = *static_cast<FCallbackState*>(user);
    state.order[state.order_count++] = 1u;
    state.camera_pos = position;
    state.camera_zoom = zoom;
    state.camera_duration = duration;
}

// Dialogue callbackの文字列と発火順を記録します。
void OnDialogue(void* user, const char* line_id) noexcept
{
    FCallbackState& state = *static_cast<FCallbackState*>(user);
    state.order[state.order_count++] = 2u;
    state.dialogue = line_id;
}

// Music callbackの文字列・fade・発火順を記録します。
void OnMusic(void* user, const char* music_id, f32 fade) noexcept
{
    FCallbackState& state = *static_cast<FCallbackState*>(user);
    state.order[state.order_count++] = 3u;
    state.music = music_id;
    state.music_fade = fade;
}

// Fire callbackの識別子と発火順を記録します。
void OnFire(void* user, u32 event_id) noexcept
{
    FCallbackState& state = *static_cast<FCallbackState*>(user);
    state.order[state.order_count++] = 4u;
    state.fire_id = event_id;
}

/** 再生中にClearを呼び、後続payloadを検証する状態です。 */
struct FClearDispatchState {
    /** callbackからClearするplayerです。 */
    CCinematicPlayer* player = nullptr;
    /** callback中のアセット寿命を監視します。 */
    TWeakPtr<ACinematicAsset> watched_asset;
    /** 先頭Fire callbackの受信を記録します。 */
    bool fire_seen = false;
    /** Fire callbackの受信回数です。 */
    u32 fire_count = 0u;
    /** Clear直後もアセットが生きていたことを記録します。 */
    bool alive_after_clear = false;
    /** 後続Dialogueの文字列一致を記録します。 */
    bool dialogue_ok = false;
    /** Dialogue callback中のアセット生存を記録します。 */
    bool alive_in_dialogue = false;
    /** Dialogue callbackの受信回数です。 */
    u32 dialogue_count = 0u;
    /** 後続Musicの文字列一致を記録します。 */
    bool music_ok = false;
    /** Music callback中のアセット生存を記録します。 */
    bool alive_in_music = false;
    /** Music callbackの受信回数です。 */
    u32 music_count = 0u;
};

// 先頭Fire callbackでplayerをClearします。
void OnClearDispatchFire(void* user, u32 event_id) noexcept
{
    FClearDispatchState& state = *static_cast<FClearDispatchState*>(user);
    ++state.fire_count;
    state.fire_seen = event_id == 7u;
    state.player->Clear();
    state.alive_after_clear = state.watched_asset.IsValid() && !state.watched_asset.Expired();
}

// Clear後にDialogue文字列を検証します。
void OnClearDispatchDialogue(void* user, const char* line_id) noexcept
{
    FClearDispatchState& state = *static_cast<FClearDispatchState*>(user);
    ++state.dialogue_count;
    state.dialogue_ok = line_id != nullptr && std::strcmp(line_id, "after-clear-dialogue") == 0;
    state.alive_in_dialogue = state.watched_asset.IsValid() && !state.watched_asset.Expired();
}

// Clear後にMusic文字列を検証します。
void OnClearDispatchMusic(void* user, const char* music_id, f32 fade) noexcept
{
    FClearDispatchState& state = *static_cast<FClearDispatchState*>(user);
    ++state.music_count;
    state.music_ok = music_id != nullptr && std::strcmp(music_id, "after-clear-music") == 0 && std::fabs(fade - 0.5f) < 0.0001f;
    state.alive_in_music = state.watched_asset.IsValid() && !state.watched_asset.Expired();
}

} // namespace

ACS_TEST(CCinematicAsset, RoundTripPreservesCanonicalPayloadAndDuration)
{
    // 正常な全kind assetを生成し、失敗なら以降を実行しません。
    TResult<TSharedPtr<ACinematicAsset>> source_result = ACinematicAsset::TryCreate(MakeEvents(), 3.0f);
    EXPECT_TRUE(source_result.IsOk());
    // EncodeとDecodeで所有assetを比較します。
    TSharedPtr<ACinematicAsset> source = Move(source_result.Value());
    // canonical bytesを生成します。
    TResult<TArray<byte>> encoded = FCinematicCodec::Encode(*source);
    EXPECT_TRUE(encoded.IsOk());
    // canonical bytesから再構築したassetです。
    TResult<TSharedPtr<ACinematicAsset>> decoded = FCinematicCodec::Decode(encoded.Value());
    EXPECT_TRUE(decoded.IsOk());
    EXPECT_EQ(decoded.Value()->EventCount(), 5u);
    EXPECT_NEAR(decoded.Value()->DurationSec(), 3.0f, 0.0001f);
    EXPECT_EQ(decoded.Value()->Events()[2u].text.View(), FStringView("hello"));
    EXPECT_EQ(decoded.Value()->Events()[3u].text.View(), FStringView("theme"));
    EXPECT_EQ(decoded.Value()->Events()[4u].event_id, 42u);
    TResult<TArray<byte>> reencoded = FCinematicCodec::Encode(*decoded.Value());
    EXPECT_TRUE(reencoded.IsOk());
    EXPECT_EQ(reencoded.Value().Num(), encoded.Value().Num());
    EXPECT_EQ(std::memcmp(reencoded.Value().GetData(), encoded.Value().GetData(), encoded.Value().Num()), 0);
    EXPECT_EQ(encoded.Value()[FCinematicCodec::kVersionOffset + 2u], static_cast<byte>(32u));
    EXPECT_EQ(encoded.Value()[FCinematicCodec::kVersionOffset + 3u], static_cast<byte>(0u));
    EXPECT_EQ(encoded.Value()[FCinematicCodec::kDurationOffset], static_cast<byte>(0u));
    EXPECT_EQ(encoded.Value()[FCinematicCodec::kDurationOffset + 1u], static_cast<byte>(0u));
    EXPECT_EQ(encoded.Value()[FCinematicCodec::kDurationOffset + 2u], static_cast<byte>(64u));
    EXPECT_EQ(encoded.Value()[FCinematicCodec::kDurationOffset + 3u], static_cast<byte>(64u));
    const usize move_time_offset = FCinematicCodec::kFirstRecordOffset + FCinematicCodec::kRecordHeaderSize +
                                   FCinematicCodec::kRecordTimeOffset;
    EXPECT_EQ(encoded.Value()[move_time_offset], static_cast<byte>(0u));
    EXPECT_EQ(encoded.Value()[move_time_offset + 1u], static_cast<byte>(0u));
    EXPECT_EQ(encoded.Value()[move_time_offset + 2u], static_cast<byte>(128u));
    EXPECT_EQ(encoded.Value()[move_time_offset + 3u], static_cast<byte>(63u));

    // 空文字Dialogue/Musicの入力列を作ります。
    TArray<FCinematicEvent> zero_text_events;
    FCinematicEvent zero_dialogue{};
    zero_dialogue.kind = ECinematicEventKind::Dialogue;
    FCinematicEvent zero_music{};
    zero_music.kind = ECinematicEventKind::Music;
    EXPECT_TRUE(zero_text_events.TryAdd(Move(zero_dialogue)));
    EXPECT_TRUE(zero_text_events.TryAdd(Move(zero_music)));
    TResult<TSharedPtr<ACinematicAsset>> zero_text_asset = ACinematicAsset::TryCreate(Move(zero_text_events), 1.0f);
    EXPECT_TRUE(zero_text_asset.IsOk());
    TResult<TArray<byte>> zero_text_bytes = FCinematicCodec::Encode(*zero_text_asset.Value());
    EXPECT_TRUE(zero_text_bytes.IsOk());
    TResult<TSharedPtr<ACinematicAsset>> zero_text_decoded = FCinematicCodec::Decode(zero_text_bytes.Value());
    EXPECT_TRUE(zero_text_decoded.IsOk());
    EXPECT_TRUE(zero_text_decoded.Value()->Events()[0u].text.IsEmpty());
    EXPECT_TRUE(zero_text_decoded.Value()->Events()[1u].text.IsEmpty());

    // イベントなしで正のdurationを持つassetです。
    TArray<FCinematicEvent> empty_events;
    TResult<TSharedPtr<ACinematicAsset>> empty = ACinematicAsset::TryCreate(Move(empty_events), 2.0f);
    EXPECT_TRUE(empty.IsOk());
    EXPECT_NEAR(empty.Value()->DurationSec(), 2.0f, 0.0001f);

    // 共通上限までのWait列を検証します。
    TArray<FCinematicEvent> max_events;
    bool max_events_added = true;
    for (u32 index = 0u; index < ACinematicAsset::kMaxEvents; ++index)
        max_events_added = max_events_added && max_events.TryAdd(FCinematicEvent{});
    EXPECT_TRUE(max_events_added);
    EXPECT_TRUE(ACinematicAsset::TryCreate(Move(max_events), 0.0f).IsOk());

    // 上限超過はasset生成時に拒否します。
    TArray<FCinematicEvent> oversized_events;
    bool oversized_events_added = true;
    for (u32 index = 0u; index <= ACinematicAsset::kMaxEvents; ++index)
        oversized_events_added = oversized_events_added && oversized_events.TryAdd(FCinematicEvent{});
    EXPECT_TRUE(oversized_events_added);
    EXPECT_TRUE(ACinematicAsset::TryCreate(Move(oversized_events), 0.0f).IsErr());
}

ACS_TEST(CCinematicAsset, RejectsMalformedBytesWithClassifiedErrors)
{
    // malformed入力の基準bytesを生成します。
    TResult<TSharedPtr<ACinematicAsset>> source_result = ACinematicAsset::TryCreate(MakeEvents(), 3.0f);
    EXPECT_TRUE(source_result.IsOk());
    // 各異常ケースを書き換えるcanonical bytesです。
    TResult<TArray<byte>> encoded = FCinematicCodec::Encode(*source_result.Value());
    EXPECT_TRUE(encoded.IsOk());

    TArray<byte> bad_magic = encoded.Value().Clone();
    bad_magic[0] = 'X';
    EXPECT_EQ(FCinematicCodec::Decode(bad_magic).Error().subcode,
              static_cast<u16>(900u + static_cast<u16>(ECinematicFormatError::InvalidMagic)));

    TArray<byte> bad_version = encoded.Value().Clone();
    PutU16(bad_version, FCinematicCodec::kVersionOffset, 2u);
    EXPECT_EQ(FCinematicCodec::Decode(bad_version).Error().subcode,
              static_cast<u16>(900u + static_cast<u16>(ECinematicFormatError::UnsupportedVersion)));

    TArray<byte> oversized_count = encoded.Value().Clone();
    PutU32(oversized_count, FCinematicCodec::kCountOffset, ACinematicAsset::kMaxEvents + 1u);
    EXPECT_EQ(FCinematicCodec::Decode(oversized_count).Error().subcode,
              static_cast<u16>(900u + static_cast<u16>(ECinematicFormatError::SizeLimit)));

    TArray<byte> bad_flags = encoded.Value().Clone();
    PutU32(bad_flags, 12u, 1u);
    EXPECT_EQ(FCinematicCodec::Decode(bad_flags).Error().subcode,
              static_cast<u16>(900u + static_cast<u16>(ECinematicFormatError::InvalidHeader)));

    TArray<byte> bad_top_reserved = encoded.Value().Clone();
    PutU32(bad_top_reserved, 28u, 1u);
    EXPECT_EQ(FCinematicCodec::Decode(bad_top_reserved).Error().subcode,
              static_cast<u16>(900u + static_cast<u16>(ECinematicFormatError::InvalidHeader)));

    TArray<byte> bad_record_reserved = encoded.Value().Clone();
    bad_record_reserved[FCinematicCodec::kFirstRecordOffset + 1u] = 1u;
    EXPECT_EQ(FCinematicCodec::Decode(bad_record_reserved).Error().subcode,
              static_cast<u16>(900u + static_cast<u16>(ECinematicFormatError::InvalidHeader)));

    TArray<byte> bad_payload_size = encoded.Value().Clone();
    PutU32(bad_payload_size, FCinematicCodec::kFirstRecordOffset + FCinematicCodec::kRecordPayloadSizeOffset, 1u);
    EXPECT_EQ(FCinematicCodec::Decode(bad_payload_size).Error().subcode,
              static_cast<u16>(900u + static_cast<u16>(ECinematicFormatError::InvalidHeader)));

    TArray<byte> invalid_event = encoded.Value().Clone();
    const usize invalid_event_insert = FCinematicCodec::kFirstRecordOffset + FCinematicCodec::kRecordHeaderSize;
    EXPECT_TRUE(invalid_event.TryAdd(static_cast<byte>(0u)));
    for (usize index = invalid_event.Num() - 1u; index > invalid_event_insert; --index)
        invalid_event[index] = invalid_event[index - 1u];
    invalid_event[invalid_event_insert] = 0u;
    PutU32(invalid_event, FCinematicCodec::kFirstRecordOffset + FCinematicCodec::kRecordPayloadSizeOffset, 1u);
    PutU32(invalid_event, FCinematicCodec::kFirstRecordOffset + FCinematicCodec::kRecordSizeOffset,
           static_cast<u32>(FCinematicCodec::kRecordHeaderSize + 1u));
    PutU32(invalid_event, 24u, static_cast<u32>(invalid_event.Num() - FCinematicCodec::kHeaderSize));
    EXPECT_EQ(FCinematicCodec::Decode(invalid_event).Error().subcode,
              static_cast<u16>(900u + static_cast<u16>(ECinematicFormatError::InvalidEvent)));

    TArray<byte> oversized_payload = encoded.Value().Clone();
    PutU32(oversized_payload, FCinematicCodec::kFirstRecordOffset + 8u, 0xffffffffu);
    EXPECT_EQ(FCinematicCodec::Decode(oversized_payload).Error().subcode,
              static_cast<u16>(900u + static_cast<u16>(ECinematicFormatError::SizeLimit)));

    TArray<byte> bad_header_size = encoded.Value().Clone();
    PutU16(bad_header_size, FCinematicCodec::kVersionOffset + 2u, 31u);
    EXPECT_EQ(FCinematicCodec::Decode(bad_header_size).Error().subcode,
              static_cast<u16>(900u + static_cast<u16>(ECinematicFormatError::InvalidHeader)));

    TArray<byte> bad_nonfinite = encoded.Value().Clone();
    // MoveCamera recordの固定長を求め、後続Dialogue位置を特定します。
    const usize move_record_size = FCinematicCodec::kRecordHeaderSize + FCinematicCodec::kMoveCameraPayloadSize;
    const usize move_record = FCinematicCodec::kFirstRecordOffset + FCinematicCodec::kRecordHeaderSize;
    PutF32(bad_nonfinite, move_record + FCinematicCodec::kRecordHeaderSize + FCinematicCodec::kMoveCameraZoomOffset, NAN);
    EXPECT_EQ(FCinematicCodec::Decode(bad_nonfinite).Error().subcode,
              static_cast<u16>(900u + static_cast<u16>(ECinematicFormatError::InvalidNumber)));

    TArray<byte> bad_utf8 = encoded.Value().Clone();
    const usize dialogue_record = FCinematicCodec::kFirstRecordOffset + FCinematicCodec::kRecordHeaderSize +
                                  move_record_size;
    bad_utf8[dialogue_record + FCinematicCodec::kRecordHeaderSize + FCinematicCodec::kDialogueTextDataOffset] = 0xffu;
    EXPECT_EQ(FCinematicCodec::Decode(bad_utf8).Error().subcode,
              static_cast<u16>(900u + static_cast<u16>(ECinematicFormatError::InvalidText)));

    TArray<byte> bad_nul = encoded.Value().Clone();
    bad_nul[dialogue_record + FCinematicCodec::kRecordHeaderSize + FCinematicCodec::kDialogueTextDataOffset] = 0u;
    EXPECT_EQ(FCinematicCodec::Decode(bad_nul).Error().subcode,
              static_cast<u16>(900u + static_cast<u16>(ECinematicFormatError::InvalidText)));

    TArray<byte> descending = encoded.Value().Clone();
    const usize descending_time_offset = FCinematicCodec::kFirstRecordOffset + FCinematicCodec::kRecordHeaderSize +
                                         FCinematicCodec::kRecordHeaderSize + FCinematicCodec::kMoveCameraPayloadSize +
                                         FCinematicCodec::kRecordTimeOffset;
    PutF32(descending, descending_time_offset, 0.25f);
    EXPECT_EQ(FCinematicCodec::Decode(descending).Error().subcode,
              static_cast<u16>(900u + static_cast<u16>(ECinematicFormatError::InvalidOrder)));

    TArray<byte> short_duration = encoded.Value().Clone();
    PutF32(short_duration, FCinematicCodec::kDurationOffset, 0.5f);
    EXPECT_EQ(FCinematicCodec::Decode(short_duration).Error().subcode,
              static_cast<u16>(900u + static_cast<u16>(ECinematicFormatError::InvalidOrder)));

    TArray<byte> unknown = encoded.Value().Clone();
    unknown[FCinematicCodec::kFirstRecordOffset] = 255u;
    EXPECT_EQ(FCinematicCodec::Decode(unknown).Error().subcode,
              static_cast<u16>(900u + static_cast<u16>(ECinematicFormatError::UnknownKind)));

    TArray<byte> trailing = encoded.Value().Clone();
    EXPECT_TRUE(trailing.TryAdd(static_cast<byte>(0u)));
    EXPECT_EQ(FCinematicCodec::Decode(trailing).Error().subcode,
              static_cast<u16>(900u + static_cast<u16>(ECinematicFormatError::TrailingData)));

    TArray<byte> truncated_section = encoded.Value().Clone();
    PutU32(truncated_section, 24u, static_cast<u32>(truncated_section.Num() - FCinematicCodec::kHeaderSize + 1u));
    EXPECT_EQ(FCinematicCodec::Decode(truncated_section).Error().subcode,
              static_cast<u16>(900u + static_cast<u16>(ECinematicFormatError::Truncated)));
}

ACS_TEST(CCinematicAsset, EnforcesTextSizeLimit)
{
    // 上限内Dialogueが生成できることを確認します。
    FCinematicEvent dialogue{};
    dialogue.kind = ECinematicEventKind::Dialogue;
    dialogue.text = MakeSizedText(ACinematicAsset::kMaxTextBytes);
    EXPECT_EQ(dialogue.text.Size(), static_cast<usize>(ACinematicAsset::kMaxTextBytes));
    TArray<FCinematicEvent> dialogue_events;
    EXPECT_TRUE(dialogue_events.TryAdd(Move(dialogue)));
    TResult<TSharedPtr<ACinematicAsset>> dialogue_asset = ACinematicAsset::TryCreate(Move(dialogue_events), 0.0f);
    EXPECT_TRUE(dialogue_asset.IsOk());
    if (dialogue_asset.IsErr()) return;
    TResult<TArray<byte>> dialogue_bytes = FCinematicCodec::Encode(*dialogue_asset.Value());
    EXPECT_TRUE(dialogue_bytes.IsOk());
    if (dialogue_bytes.IsErr()) return;
    TResult<TSharedPtr<ACinematicAsset>> decoded_dialogue = FCinematicCodec::Decode(dialogue_bytes.Value());
    EXPECT_TRUE(decoded_dialogue.IsOk());
    if (decoded_dialogue.IsErr()) return;
    EXPECT_EQ(decoded_dialogue.Value()->Events()[0].text.Size(), static_cast<usize>(ACinematicAsset::kMaxTextBytes));

    // Dialogueの宣言長だけを上限超過へ変更します。
    TArray<byte> oversized_dialogue = dialogue_bytes.Value().Clone();
    EXPECT_TRUE(oversized_dialogue.TryAdd(static_cast<byte>(0u)));
    const usize dialogue_record = FCinematicCodec::kFirstRecordOffset;
    const usize dialogue_payload = dialogue_record + FCinematicCodec::kRecordHeaderSize;
    PutU32(oversized_dialogue, dialogue_payload, ACinematicAsset::kMaxTextBytes + 1u);
    PutU32(oversized_dialogue, dialogue_record + FCinematicCodec::kRecordPayloadSizeOffset,
           FCinematicCodec::kDialogueTextDataOffset + ACinematicAsset::kMaxTextBytes + 1u);
    PutU32(oversized_dialogue, dialogue_record + FCinematicCodec::kRecordSizeOffset,
           FCinematicCodec::kRecordHeaderSize + FCinematicCodec::kDialogueTextDataOffset + ACinematicAsset::kMaxTextBytes + 1u);
    PutU32(oversized_dialogue, 24u,
           static_cast<u32>(oversized_dialogue.Num() - FCinematicCodec::kHeaderSize));
    EXPECT_EQ(FCinematicCodec::Decode(oversized_dialogue).Error().subcode,
              static_cast<u16>(900u + static_cast<u16>(ECinematicFormatError::SizeLimit)));

    // 上限超過Dialogueはasset生成を拒否します。
    FCinematicEvent oversized_dialogue_event{};
    oversized_dialogue_event.kind = ECinematicEventKind::Dialogue;
    oversized_dialogue_event.text = MakeSizedText(ACinematicAsset::kMaxTextBytes + 1u);
    TArray<FCinematicEvent> oversized_dialogue_events;
    EXPECT_TRUE(oversized_dialogue_events.TryAdd(Move(oversized_dialogue_event)));
    EXPECT_TRUE(ACinematicAsset::TryCreate(Move(oversized_dialogue_events), 0.0f).IsErr());

    // 上限内Musicが生成できることを確認します。
    FCinematicEvent music{};
    music.kind = ECinematicEventKind::Music;
    music.text = MakeSizedText(ACinematicAsset::kMaxTextBytes);
    EXPECT_EQ(music.text.Size(), static_cast<usize>(ACinematicAsset::kMaxTextBytes));
    TArray<FCinematicEvent> music_events;
    EXPECT_TRUE(music_events.TryAdd(Move(music)));
    TResult<TSharedPtr<ACinematicAsset>> music_asset = ACinematicAsset::TryCreate(Move(music_events), 0.0f);
    EXPECT_TRUE(music_asset.IsOk());
    if (music_asset.IsErr()) return;
    TResult<TArray<byte>> music_bytes = FCinematicCodec::Encode(*music_asset.Value());
    EXPECT_TRUE(music_bytes.IsOk());
    if (music_bytes.IsErr()) return;
    TResult<TSharedPtr<ACinematicAsset>> decoded_music = FCinematicCodec::Decode(music_bytes.Value());
    EXPECT_TRUE(decoded_music.IsOk());
    if (decoded_music.IsErr()) return;
    EXPECT_EQ(decoded_music.Value()->Events()[0].text.Size(), static_cast<usize>(ACinematicAsset::kMaxTextBytes));

    // Musicの宣言長だけを上限超過へ変更します。
    TArray<byte> oversized_music = music_bytes.Value().Clone();
    EXPECT_TRUE(oversized_music.TryAdd(static_cast<byte>(0u)));
    const usize music_record = FCinematicCodec::kFirstRecordOffset;
    const usize music_payload = music_record + FCinematicCodec::kRecordHeaderSize;
    PutU32(oversized_music, music_payload + 4u, ACinematicAsset::kMaxTextBytes + 1u);
    PutU32(oversized_music, music_record + FCinematicCodec::kRecordPayloadSizeOffset,
           8u + ACinematicAsset::kMaxTextBytes + 1u);
    PutU32(oversized_music, music_record + FCinematicCodec::kRecordSizeOffset,
           FCinematicCodec::kRecordHeaderSize + 8u + ACinematicAsset::kMaxTextBytes + 1u);
    PutU32(oversized_music, 24u,
           static_cast<u32>(oversized_music.Num() - FCinematicCodec::kHeaderSize));
    EXPECT_EQ(FCinematicCodec::Decode(oversized_music).Error().subcode,
              static_cast<u16>(900u + static_cast<u16>(ECinematicFormatError::SizeLimit)));

    // 上限超過Musicはasset生成を拒否します。
    FCinematicEvent oversized_music_event{};
    oversized_music_event.kind = ECinematicEventKind::Music;
    oversized_music_event.text = MakeSizedText(ACinematicAsset::kMaxTextBytes + 1u);
    TArray<FCinematicEvent> oversized_music_events;
    EXPECT_TRUE(oversized_music_events.TryAdd(Move(oversized_music_event)));
    EXPECT_TRUE(ACinematicAsset::TryCreate(Move(oversized_music_events), 0.0f).IsErr());
}

ACS_TEST(CCinematicAsset, RegistryAndLegacyBridgePreserveIdentity)
{
    // Registryとplayerへ渡す基準assetを生成します。
    TResult<TSharedPtr<ACinematicAsset>> source_result = ACinematicAsset::TryCreate(MakeEvents(), 3.0f);
    EXPECT_TRUE(source_result.IsOk());
    // 呼び出し元が所有するassetをplayerへ移します。
    TSharedPtr<ACinematicAsset> caller_asset = Move(source_result.Value());
    // Registryとloaderの入力bytesを生成します。
    TResult<TArray<byte>> encoded = FCinematicCodec::Encode(*caller_asset);
    EXPECT_TRUE(encoded.IsOk());
    // 直接loader経路の結果を検証します。
    CCinematicAssetLoader loader;
    TResult<TSharedPtr<AAsset>> direct_loaded = loader.LoadFromBytes(FAssetId{123u}, encoded.Value());
    EXPECT_TRUE(direct_loaded.IsOk());
    EXPECT_EQ(direct_loaded.Value()->Id(), FAssetId{123u});
    EXPECT_EQ(direct_loaded.Value()->Type(), ACinematicAsset::StaticType());
    EXPECT_EQ(direct_loaded.Value()->State(), EAssetState::Ready);

    constexpr const wchar_t* path = L"acs_cinematic_asset_registry_test.cine";
    (void)CFileSystem::Delete(path);
    EXPECT_TRUE(CFileSystem::WriteAllBytesAtomic(path, encoded.Value().GetData(), encoded.Value().Num()).IsOk());
    CAssetRegistry registry;
    EXPECT_TRUE(registry.TryRegisterDefaultLoaders().IsOk());
    TResult<TSharedPtr<AAsset>> loaded = registry.Load(path);
    EXPECT_TRUE(loaded.IsOk());
    EXPECT_TRUE(loaded.Value().Get() != nullptr);
    EXPECT_TRUE(loaded.Value()->Id().IsValid());
    EXPECT_EQ(loaded.Value()->Type(), ACinematicAsset::StaticType());
    EXPECT_EQ(loaded.Value()->State(), EAssetState::Ready);
    EXPECT_TRUE(CFileSystem::Delete(path).IsOk());

    // player callbackの受信状態を保持します。
    FCallbackState callbacks;
    CCinematicPlayer player;
    player.SetCameraCallback(&OnCamera, &callbacks);
    player.SetDialogueCallback(&OnDialogue, &callbacks);
    player.SetMusicCallback(&OnMusic, &callbacks);
    player.SetEventCallback(&OnFire, &callbacks);
    TWeakPtr<ACinematicAsset> watched_asset(caller_asset);
    EXPECT_TRUE(player.TrySetAsset(Move(caller_asset)));
    EXPECT_TRUE(caller_asset.Get() == nullptr);
    EXPECT_TRUE(player.AssetView() != nullptr);
    EXPECT_TRUE(watched_asset.IsValid());
    EXPECT_TRUE(!watched_asset.Expired());
    player.Play();
    player.Tick(1.0f);
    EXPECT_TRUE(!player.IsFinished());
    player.Tick(2.0f);
    EXPECT_NEAR(player.CurrentTime(), 3.0f, 0.0001f);
    EXPECT_TRUE(player.IsFinished());
    EXPECT_EQ(callbacks.order_count, 4u);
    EXPECT_EQ(callbacks.order[0], 1u);
    EXPECT_EQ(callbacks.order[1], 2u);
    EXPECT_EQ(callbacks.order[2], 3u);
    EXPECT_EQ(callbacks.order[3], 4u);
    EXPECT_NEAR(callbacks.camera_pos.x, 2.0f, 0.0001f);
    EXPECT_NEAR(callbacks.camera_pos.y, 3.0f, 0.0001f);
    EXPECT_NEAR(callbacks.camera_zoom, 1.25f, 0.0001f);
    EXPECT_NEAR(callbacks.camera_duration, 0.5f, 0.0001f);
    EXPECT_EQ(std::strcmp(callbacks.dialogue, "hello"), 0);
    EXPECT_EQ(std::strcmp(callbacks.music, "theme"), 0);
    EXPECT_NEAR(callbacks.music_fade, 0.25f, 0.0001f);
    EXPECT_EQ(callbacks.fire_id, 42u);
    EXPECT_TRUE(watched_asset.IsValid());
    player.Clear();
    EXPECT_TRUE(player.AssetView() == nullptr);
    EXPECT_TRUE(watched_asset.Expired());
    EXPECT_TRUE(watched_asset.Lock().Get() == nullptr);

    TArray<FCinematicEvent> empty_events;
    TResult<TSharedPtr<ACinematicAsset>> empty_result = ACinematicAsset::TryCreate(Move(empty_events), 2.0f);
    EXPECT_TRUE(empty_result.IsOk());
    CCinematicPlayer empty_player;
    EXPECT_TRUE(empty_player.TrySetAsset(Move(empty_result.Value())));
    empty_player.Play();
    EXPECT_TRUE(!empty_player.IsFinished());
    empty_player.Tick(2.0f);
    EXPECT_TRUE(empty_player.IsFinished());
}

ACS_TEST(CCinematicAsset, ClearDuringDispatchKeepsPayloadAlive)
{
    // Clear再入を含む同時刻イベント列を生成します。
    TArray<FCinematicEvent> events;
    FCinematicEvent fire{};
    fire.time_sec = 1.0f;
    fire.kind = ECinematicEventKind::FireEvent;
    fire.event_id = 7u;
    EXPECT_TRUE(events.TryAdd(Move(fire)));
    FCinematicEvent dialogue{};
    dialogue.time_sec = 1.0f;
    dialogue.kind = ECinematicEventKind::Dialogue;
    EXPECT_TRUE(dialogue.text.TryAppend(FStringView("after-clear-dialogue")));
    EXPECT_TRUE(events.TryAdd(Move(dialogue)));
    FCinematicEvent music{};
    music.time_sec = 1.0f;
    music.kind = ECinematicEventKind::Music;
    music.music_fade = 0.5f;
    EXPECT_TRUE(music.text.TryAppend(FStringView("after-clear-music")));
    EXPECT_TRUE(events.TryAdd(Move(music)));
    TResult<TSharedPtr<ACinematicAsset>> result = ACinematicAsset::TryCreate(Move(events), 1.0f);
    EXPECT_TRUE(result.IsOk());
    if (result.IsErr()) return;
    TSharedPtr<ACinematicAsset> caller_asset = Move(result.Value());
    TWeakPtr<ACinematicAsset> watched_asset(caller_asset);
    CCinematicPlayer player;
    FClearDispatchState state;
    state.player = &player;
    state.watched_asset = watched_asset;
    player.SetEventCallback(&OnClearDispatchFire, &state);
    player.SetDialogueCallback(&OnClearDispatchDialogue, &state);
    player.SetMusicCallback(&OnClearDispatchMusic, &state);
    EXPECT_TRUE(player.TrySetAsset(Move(caller_asset)));
    EXPECT_TRUE(caller_asset.Get() == nullptr);
    player.Play();
    player.Tick(1.0f);
    EXPECT_TRUE(state.fire_seen);
    EXPECT_EQ(state.fire_count, 1u);
    EXPECT_TRUE(state.dialogue_ok);
    EXPECT_EQ(state.dialogue_count, 1u);
    EXPECT_TRUE(state.music_ok);
    EXPECT_EQ(state.music_count, 1u);
    EXPECT_TRUE(state.alive_after_clear);
    EXPECT_TRUE(state.alive_in_dialogue);
    EXPECT_TRUE(state.alive_in_music);
    EXPECT_TRUE(player.AssetView() == nullptr);
    EXPECT_TRUE(watched_asset.Expired());
    EXPECT_TRUE(watched_asset.Lock().Get() == nullptr);
}

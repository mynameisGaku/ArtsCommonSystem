// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "asset/cinematics/ACinematicAsset.h"
#include "container/Array.h"
#include "foundation/Result.h"

namespace acs::asset {

/** 検証済みアセットと固定バイナリ形式を相互変換します。 */
class FCinematicCodec final {
public:
    /** .cine v1のversion値です。 */
    static constexpr u16 kVersion = 1u;
    /** 固定headerのbyte数です。 */
    static constexpr u16 kHeaderSize = 32u;
    /** codecが受け入れる最大イベント数です。 */
    static constexpr u32 kMaxEvents = ACinematicAsset::kMaxEvents;
    /** record内version位置です。 */
    static constexpr usize kVersionOffset = 8u;
    /** header内イベント数位置です。 */
    static constexpr usize kCountOffset = 16u;
    /** header内duration位置です。 */
    static constexpr usize kDurationOffset = 20u;
    /** 最初のrecord位置です。 */
    static constexpr usize kFirstRecordOffset = kHeaderSize;
    /** record headerのbyte数です。 */
    static constexpr usize kRecordHeaderSize = 20u;
    /** record内時刻位置です。 */
    static constexpr usize kRecordTimeOffset = 4u;
    /** record内payload長位置です。 */
    static constexpr usize kRecordPayloadSizeOffset = 8u;
    /** record内record長位置です。 */
    static constexpr usize kRecordSizeOffset = 12u;
    /** camera payloadのbyte数です。 */
    static constexpr usize kMoveCameraPayloadSize = 16u;
    /** camera payload内zoom位置です。 */
    static constexpr usize kMoveCameraZoomOffset = 8u;
    /** Dialogue payload内の文字列データ位置です。 */
    static constexpr usize kDialogueTextDataOffset = 4u;

    /** 入力bytesを検証してアセットへ変換し、不正形式・サイズ・確保失敗時はtyped errorを返します。 */
    static TResult<TSharedPtr<ACinematicAsset>> Decode(const TArray<byte>& bytes) noexcept;

    /** 入力assetをcanonical bytesへ変換し、値または確保失敗時はtyped errorを返します。 */
    static TResult<TArray<byte>> Encode(const ACinematicAsset& asset) noexcept;
};

} // namespace acs::asset

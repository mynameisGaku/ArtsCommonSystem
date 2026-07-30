// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Span.h"

namespace acs::game {

struct FInputSample;

namespace input_recording_detail {

/** `.acsr` の形式識別値。 */
inline constexpr u32 kMagic = 0x52534341u;
/** 対応する `.acsr` version。 */
inline constexpr u32 kVersion = 1u;
/** header の byte 幅。 */
inline constexpr usize kHeaderBytes = 16u;
/** footer CRC の byte 幅。 */
inline constexpr usize kFooterBytes = 4u;
/** sample 一件の on-disk byte 幅。 */
inline constexpr usize kSampleBytes = 29u;

/** little-endian u32 を aliasing 安全に読む。 */
u32 ReadU32(const u8* source) noexcept;
/** little-endian u32 を aliasing 安全に書く。 */
void WriteU32(u8* destination, u32 value) noexcept;
/** sample 内の mouse float が全て有限か返す。 */
bool HasFiniteMousePosition(const u8* sample) noexcept;
/** in-memory sample 内の mouse float が全て有限か返す。 */
bool HasFiniteMousePosition(const FInputSample& sample) noexcept;
/** byte 列の CRC32 を計算する。 */
u32 ComputeCrc32(TSpan<const u8> bytes) noexcept;
/** sample を ABI 非依存 on-disk layout へ書く。 */
void WriteSample(u8* destination, const FInputSample& sample) noexcept;
/** on-disk sample を ABI 非依存に復元する。 */
void ReadSample(const u8* source, FInputSample& sample) noexcept;

} // namespace input_recording_detail
} // namespace acs::game

// SPDX-License-Identifier: Apache-2.0
#include "gameframework/InputRecordingFormat.h"

#include "foundation/EndianSerialization.h"
#include "gameframework/InputRecorder.h"
#include "memory/Memory.h"

namespace acs::game::input_recording_detail {

namespace {

/** f32 の bit pattern を aliasing 安全に読む。 */
u32 FloatBits(f32 value) noexcept
{
    /** 取り出した bit pattern。 */
    u32 bits = 0u;
    MemCopy(&bits, &value, sizeof(bits));
    return bits;
}

/** u32 bit pattern を f32 へ aliasing 安全に戻す。 */
f32 FloatFromBits(u32 bits) noexcept
{
    /** 復元する float。 */
    f32 value = 0.0f;
    MemCopy(&value, &bits, sizeof(value));
    return value;
}

/** IEEE-754 binary32 bit pattern が有限値か返す。 */
bool IsFiniteBits(u32 bits) noexcept
{
    return (bits & 0x7F800000u) != 0x7F800000u;
}

/** CRC32 lookup table を返す。 */
const u32* Crc32Table() noexcept
{
    /** CRC32 table を初期化する内部型。 */
    struct FCrc32Table {
        /** 256 entry を一度だけ生成する。 */
        FCrc32Table() noexcept
        {
            for (u32 index = 0u; index < 256u; ++index) {
                /** 現在生成している CRC entry。 */
                u32 value = index;
                for (u32 bit = 0u; bit < 8u; ++bit) value = (value & 1u) ? (0xEDB88320u ^ (value >> 1u)) : (value >> 1u);
                values[index] = value;
            }
        }

        /** 生成済み CRC32 table。 */
        u32 values[256] = {};
    };

    /** thread-safe に一度だけ作る table。 */
    static const FCrc32Table table;
    return table.values;
}

} // namespace

/** little-endian u32 を読む。 */
u32 ReadU32(const u8* source) noexcept
{
    return ReadLittleEndian<u32>(source);
}

/** little-endian u32 を書く。 */
void WriteU32(u8* destination, u32 value) noexcept
{
    WriteLittleEndian(destination, value);
}

/** on-disk sample の mouse 値を検査する。 */
bool HasFiniteMousePosition(const u8* sample) noexcept
{
    return IsFiniteBits(ReadU32(sample + 20u)) && IsFiniteBits(ReadU32(sample + 24u));
}

/** in-memory sample の mouse 値を検査する。 */
bool HasFiniteMousePosition(const FInputSample& sample) noexcept
{
    return IsFiniteBits(FloatBits(sample.mouse_pos.x)) && IsFiniteBits(FloatBits(sample.mouse_pos.y));
}

/** byte 列の CRC32 を計算する。 */
u32 ComputeCrc32(TSpan<const u8> bytes) noexcept
{
    /** CRC32 計算用の参照表。 */
    const u32* table = Crc32Table();
    /** 更新中の CRC。 */
    u32 crc = 0xFFFFFFFFu;
    for (usize index = 0u; index < bytes.Size(); ++index) crc = table[(crc ^ bytes[index]) & 0xFFu] ^ (crc >> 8u);
    return crc ^ 0xFFFFFFFFu;
}

/** sample を固定 29 byte layout へ書く。 */
void WriteSample(u8* destination, const FInputSample& sample) noexcept
{
    WriteU32(destination, sample.tick);
    MemCopy(destination + 4u, sample.key_codes_changed, 8u);
    MemCopy(destination + 12u, sample.key_states, 8u);
    WriteU32(destination + 20u, FloatBits(sample.mouse_pos.x));
    WriteU32(destination + 24u, FloatBits(sample.mouse_pos.y));
    destination[28u] = sample.mouse_button_states;
}

/** 固定 29 byte layout から sample を復元する。 */
void ReadSample(const u8* source, FInputSample& sample) noexcept
{
    sample.tick = ReadU32(source);
    MemCopy(sample.key_codes_changed, source + 4u, 8u);
    MemCopy(sample.key_states, source + 12u, 8u);
    sample.mouse_pos.x = FloatFromBits(ReadU32(source + 20u));
    sample.mouse_pos.y = FloatFromBits(ReadU32(source + 24u));
    sample.mouse_button_states = source[28u];
}

} // namespace acs::game::input_recording_detail

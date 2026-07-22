// SPDX-License-Identifier: Apache-2.0

#include "gameframework/AnimationCurveArchive.h"

#include "container/Array.h"
#include "foundation/Error.h"
#include "gameframework/SaveArchive.h"
#include "memory/Memory.h"

namespace acs::game {
namespace {

constexpr u8 kMagic[8] = {'A', 'C', 'S', 'C', 'U', 'R', 'V', '\0'};

void WriteU16LE(u8* destination, u16 value) noexcept {
    destination[0] = static_cast<u8>(value);
    destination[1] = static_cast<u8>(value >> 8u);
}

void WriteU32LE(u8* destination, u32 value) noexcept {
    destination[0] = static_cast<u8>(value);
    destination[1] = static_cast<u8>(value >> 8u);
    destination[2] = static_cast<u8>(value >> 16u);
    destination[3] = static_cast<u8>(value >> 24u);
}

u16 ReadU16LE(const u8* source) noexcept {
    return static_cast<u16>(
        static_cast<u16>(source[0]) |
        static_cast<u16>(static_cast<u16>(source[1]) << 8u));
}

u32 ReadU32LE(const u8* source) noexcept {
    return static_cast<u32>(source[0]) |
           (static_cast<u32>(source[1]) << 8u) |
           (static_cast<u32>(source[2]) << 16u) |
           (static_cast<u32>(source[3]) << 24u);
}

void WriteF32LE(u8* destination, f32 value) noexcept {
    u32 bits = 0u;
    MemCopy(&bits, &value, sizeof(bits));
    WriteU32LE(destination, bits);
}

f32 ReadF32LE(const u8* source) noexcept {
    const u32 bits = ReadU32LE(source);
    f32 value = 0.0f;
    MemCopy(&value, &bits, sizeof(value));
    return value;
}

u32 WireCrc32(const u8* bytes, usize size) noexcept {
    u32 crc = 0xFFFFFFFFu;
    for (usize index = 0u; index < size; ++index) {
        // checksum field 自体は 0 に正準化する。これにより wrap mode を含む意味の
        // ある全 header byte と、すべての key を CRC の検証対象に含める。
        const u8 byte =
            index >= 24u && index < 28u ? 0u : bytes[index];
        crc ^= static_cast<u32>(byte);
        for (u32 bit = 0u; bit < 8u; ++bit) {
            const u32 mask =
                static_cast<u32>(0u - static_cast<u32>(crc & 1u));
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

bool IsValidWrapMode(FAnimationCurve::EWrapMode mode) noexcept {
    return mode == FAnimationCurve::EWrapMode::Clamp ||
           mode == FAnimationCurve::EWrapMode::Loop ||
           mode == FAnimationCurve::EWrapMode::PingPong;
}

bool IsValidInterpolation(ECurveInterpolation interpolation) noexcept {
    return interpolation == ECurveInterpolation::Step ||
           interpolation == ECurveInterpolation::Linear ||
           interpolation == ECurveInterpolation::Hermite;
}

FAnimationCurveArchiveResult MakeCurveFailure(
    const FAnimationCurveResult& curve_result,
    u64 required_size) noexcept {
    FAnimationCurveArchiveResult result{};
    result.error = curve_result.error == EAnimationCurveError::TooManyKeys
        ? EAnimationCurveArchiveError::TooManyKeys
        : curve_result.error == EAnimationCurveError::AllocationFailure
            ? EAnimationCurveArchiveError::AllocationFailure
            : EAnimationCurveArchiveError::InvalidCurveData;
    result.curve_error = curve_result.error;
    result.key_index = curve_result.key_index;
    result.required_size = required_size;
    return result;
}

FAnimationCurveArchiveResult MakePersistenceFailure(
    const FErrorCode& error,
    u64 required_size) noexcept {
    FAnimationCurveArchiveResult result{};
    result.error = EAnimationCurveArchiveError::PersistenceFailure;
    result.required_size = required_size;
    result.persistence_subcode = error.subcode;
    result.os_error = error.os_error;
    return result;
}

FAnimationCurveResult ValidateCurve(
    const FAnimationCurve& curve) noexcept {
    FAnimationCurveResult result{};
    result.key_count = curve.KeyCount();
    if (curve.KeyCount() > FAnimationCurve::kMaxKeys) {
        result.error = EAnimationCurveError::TooManyKeys;
        return result;
    }
    if (!IsValidWrapMode(curve.PreWrap()) ||
        !IsValidWrapMode(curve.PostWrap())) {
        result.error = EAnimationCurveError::InvalidWrapMode;
        return result;
    }

    for (u32 index = 0u; index < curve.KeyCount(); ++index) {
        result.key_index = index;
        const FCurveKey* key = curve.Key(index);
        if (key == nullptr) {
            result.error = EAnimationCurveError::NullKeys;
            return result;
        }
        const f32 values[] = {
            key->time, key->value, key->in_tangent, key->out_tangent};
        for (f32 value : values) {
            // 有限な IEEE-754 値では exponent が全 bit 1 にならない。
            u32 bits = 0u;
            MemCopy(&bits, &value, sizeof(bits));
            if ((bits & 0x7F800000u) == 0x7F800000u) {
                result.error = EAnimationCurveError::NonFiniteValue;
                return result;
            }
        }
        if (!IsValidInterpolation(key->in_interp) ||
            !IsValidInterpolation(key->out_interp)) {
            result.error = EAnimationCurveError::InvalidInterpolation;
            return result;
        }
        if (index > 0u) {
            const FCurveKey* previous = curve.Key(index - 1u);
            if (key->time < previous->time) {
                result.error = EAnimationCurveError::UnsortedKeys;
                return result;
            }
            if (key->time == previous->time) {
                result.error = EAnimationCurveError::DuplicateKeyTime;
                return result;
            }
        }
    }
    return result;
}

} // namespace

const char* FAnimationCurveArchiveResult::ErrorName(
    EAnimationCurveArchiveError error) noexcept {
    switch (error) {
    case EAnimationCurveArchiveError::None: return "None";
    case EAnimationCurveArchiveError::NullArgument: return "NullArgument";
    case EAnimationCurveArchiveError::BufferTooSmall: return "BufferTooSmall";
    case EAnimationCurveArchiveError::InvalidMagic: return "InvalidMagic";
    case EAnimationCurveArchiveError::UnsupportedVersion: return "UnsupportedVersion";
    case EAnimationCurveArchiveError::InvalidHeader: return "InvalidHeader";
    case EAnimationCurveArchiveError::SizeMismatch: return "SizeMismatch";
    case EAnimationCurveArchiveError::ChecksumMismatch: return "ChecksumMismatch";
    case EAnimationCurveArchiveError::TooManyKeys: return "TooManyKeys";
    case EAnimationCurveArchiveError::InvalidCurveData: return "InvalidCurveData";
    case EAnimationCurveArchiveError::AllocationFailure: return "AllocationFailure";
    case EAnimationCurveArchiveError::PersistenceFailure: return "PersistenceFailure";
    }
    return "Unknown";
}

u64 FAnimationCurveArchive::EncodedSize(
    const FAnimationCurve& curve) noexcept {
    return static_cast<u64>(kHeaderSize) +
           static_cast<u64>(kKeyRecordSize) *
               static_cast<u64>(curve.KeyCount());
}

FAnimationCurveArchiveResult FAnimationCurveArchive::Encode(
    const FAnimationCurve& curve,
    void* out_bytes,
    u64 out_capacity,
    u64& out_size) noexcept {
    const u64 required_size = EncodedSize(curve);
    out_size = required_size;

    const FAnimationCurveResult validation = ValidateCurve(curve);
    if (!validation.Succeeded()) {
        return MakeCurveFailure(validation, required_size);
    }
    if (out_capacity < required_size) {
        FAnimationCurveArchiveResult result{};
        result.error = EAnimationCurveArchiveError::BufferTooSmall;
        result.required_size = required_size;
        return result;
    }
    if (out_bytes == nullptr) {
        FAnimationCurveArchiveResult result{};
        result.error = EAnimationCurveArchiveError::NullArgument;
        result.required_size = required_size;
        return result;
    }

    u8* output = static_cast<u8*>(out_bytes);
    MemCopy(output, kMagic, sizeof(kMagic));
    WriteU16LE(output + 8u, kWireVersion);
    WriteU16LE(output + 10u, static_cast<u16>(kHeaderSize));
    WriteU32LE(output + 12u, curve.KeyCount());
    output[16u] = static_cast<u8>(curve.PreWrap());
    output[17u] = static_cast<u8>(curve.PostWrap());
    WriteU16LE(output + 18u, 0u);
    const u32 payload_size = curve.KeyCount() * kKeyRecordSize;
    WriteU32LE(output + 20u, payload_size);
    WriteU32LE(output + 24u, 0u);
    WriteU32LE(output + 28u, 0u);

    u8* record = output + kHeaderSize;
    for (u32 index = 0u; index < curve.KeyCount(); ++index) {
        const FCurveKey& key = *curve.Key(index);
        WriteF32LE(record + 0u, key.time);
        WriteF32LE(record + 4u, key.value);
        WriteF32LE(record + 8u, key.in_tangent);
        WriteF32LE(record + 12u, key.out_tangent);
        record[16u] = static_cast<u8>(key.in_interp);
        record[17u] = static_cast<u8>(key.out_interp);
        WriteU16LE(record + 18u, 0u);
        record += kKeyRecordSize;
    }
    WriteU32LE(output + 24u,
               WireCrc32(output,
                         static_cast<usize>(required_size)));

    FAnimationCurveArchiveResult result{};
    result.required_size = required_size;
    return result;
}

FAnimationCurveArchiveResult FAnimationCurveArchive::Decode(
    const void* bytes,
    u64 size,
    FAnimationCurve& out_curve) noexcept {
    if (bytes == nullptr) {
        FAnimationCurveArchiveResult result{};
        result.error = EAnimationCurveArchiveError::NullArgument;
        result.required_size = kHeaderSize;
        return result;
    }
    if (size < kHeaderSize) {
        FAnimationCurveArchiveResult result{};
        result.error = EAnimationCurveArchiveError::SizeMismatch;
        result.required_size = kHeaderSize;
        return result;
    }
    if (size > kMaxEncodedSize) {
        FAnimationCurveArchiveResult result{};
        result.error = EAnimationCurveArchiveError::TooManyKeys;
        result.required_size = kMaxEncodedSize;
        return result;
    }

    const u8* input = static_cast<const u8*>(bytes);
    for (usize index = 0u; index < sizeof(kMagic); ++index) {
        if (input[index] != kMagic[index]) {
            FAnimationCurveArchiveResult result{};
            result.error = EAnimationCurveArchiveError::InvalidMagic;
            return result;
        }
    }
    if (ReadU16LE(input + 8u) != kWireVersion) {
        FAnimationCurveArchiveResult result{};
        result.error = EAnimationCurveArchiveError::UnsupportedVersion;
        return result;
    }
    if (ReadU16LE(input + 10u) != kHeaderSize ||
        ReadU16LE(input + 18u) != 0u ||
        ReadU32LE(input + 28u) != 0u) {
        FAnimationCurveArchiveResult result{};
        result.error = EAnimationCurveArchiveError::InvalidHeader;
        return result;
    }

    const u32 key_count = ReadU32LE(input + 12u);
    if (key_count > FAnimationCurve::kMaxKeys) {
        FAnimationCurveArchiveResult result{};
        result.error = EAnimationCurveArchiveError::TooManyKeys;
        result.required_size = kMaxEncodedSize;
        return result;
    }
    const u32 payload_size = ReadU32LE(input + 20u);
    const u64 required_size =
        static_cast<u64>(kHeaderSize) +
        static_cast<u64>(key_count) *
            static_cast<u64>(kKeyRecordSize);
    if (payload_size != key_count * kKeyRecordSize ||
        size != required_size) {
        FAnimationCurveArchiveResult result{};
        result.error = EAnimationCurveArchiveError::SizeMismatch;
        result.required_size = required_size;
        return result;
    }
    const auto pre_wrap =
        static_cast<FAnimationCurve::EWrapMode>(input[16u]);
    const auto post_wrap =
        static_cast<FAnimationCurve::EWrapMode>(input[17u]);
    if (!IsValidWrapMode(pre_wrap) || !IsValidWrapMode(post_wrap)) {
        FAnimationCurveArchiveResult result{};
        result.error = EAnimationCurveArchiveError::InvalidCurveData;
        result.curve_error = EAnimationCurveError::InvalidWrapMode;
        result.required_size = required_size;
        return result;
    }
    if (WireCrc32(input, static_cast<usize>(required_size)) !=
        ReadU32LE(input + 24u)) {
        FAnimationCurveArchiveResult result{};
        result.error = EAnimationCurveArchiveError::ChecksumMismatch;
        result.required_size = required_size;
        return result;
    }

    TArray<FCurveKey> staged;
    if (!staged.TryResize(key_count)) {
        FAnimationCurveArchiveResult result{};
        result.error = EAnimationCurveArchiveError::AllocationFailure;
        result.required_size = required_size;
        return result;
    }
    const u8* record = input + kHeaderSize;
    for (u32 index = 0u; index < key_count; ++index) {
        if (ReadU16LE(record + 18u) != 0u) {
            FAnimationCurveArchiveResult result{};
            result.error = EAnimationCurveArchiveError::InvalidHeader;
            result.key_index = index;
            result.required_size = required_size;
            return result;
        }
        FCurveKey& key = staged[index];
        key.time = ReadF32LE(record + 0u);
        key.value = ReadF32LE(record + 4u);
        key.in_tangent = ReadF32LE(record + 8u);
        key.out_tangent = ReadF32LE(record + 12u);
        key.in_interp =
            static_cast<ECurveInterpolation>(record[16u]);
        key.out_interp =
            static_cast<ECurveInterpolation>(record[17u]);
        record += kKeyRecordSize;
    }

    const FAnimationCurveResult curve_result =
        out_curve.TrySetKeys(staged.Data(), key_count,
                             pre_wrap, post_wrap);
    if (!curve_result.Succeeded()) {
        return MakeCurveFailure(curve_result, required_size);
    }

    FAnimationCurveArchiveResult result{};
    result.required_size = required_size;
    return result;
}

FAnimationCurveArchiveResult FAnimationCurveArchive::SaveToFile(
    const wchar_t* file_path,
    const FAnimationCurve& curve) noexcept {
    if (file_path == nullptr || file_path[0] == L'\0') {
        FAnimationCurveArchiveResult result{};
        result.error = EAnimationCurveArchiveError::NullArgument;
        return result;
    }

    const u64 required_size = EncodedSize(curve);
    TArray<u8> encoded;
    if (!encoded.TryResize(static_cast<usize>(required_size))) {
        FAnimationCurveArchiveResult result{};
        result.error = EAnimationCurveArchiveError::AllocationFailure;
        result.required_size = required_size;
        return result;
    }
    u64 encoded_size = 0u;
    FAnimationCurveArchiveResult result =
        Encode(curve, encoded.Data(), required_size, encoded_size);
    if (!result.Succeeded()) return result;

    const TResult<void> persisted = FSaveArchive::WriteToFile(
        file_path, kFileEnvelopeVersion,
        encoded.Data(), encoded_size);
    if (persisted.IsErr()) {
        return MakePersistenceFailure(
            persisted.Error(), required_size);
    }
    return result;
}

FAnimationCurveArchiveResult FAnimationCurveArchive::LoadFromFile(
    const wchar_t* file_path,
    FAnimationCurve& out_curve) noexcept {
    if (file_path == nullptr || file_path[0] == L'\0') {
        FAnimationCurveArchiveResult result{};
        result.error = EAnimationCurveArchiveError::NullArgument;
        return result;
    }

    const TResult<u64> size_result =
        FSaveArchive::PeekPayloadSize(file_path);
    if (size_result.IsErr()) {
        return MakePersistenceFailure(size_result.Error(), 0u);
    }
    const u64 payload_size = size_result.Value();
    if (payload_size < kHeaderSize || payload_size > kMaxEncodedSize) {
        FAnimationCurveArchiveResult result{};
        result.error = payload_size > kMaxEncodedSize
            ? EAnimationCurveArchiveError::TooManyKeys
            : EAnimationCurveArchiveError::SizeMismatch;
        result.required_size = payload_size > kMaxEncodedSize
            ? kMaxEncodedSize : kHeaderSize;
        return result;
    }

    TArray<u8> encoded;
    if (!encoded.TryResize(static_cast<usize>(payload_size))) {
        FAnimationCurveArchiveResult result{};
        result.error = EAnimationCurveArchiveError::AllocationFailure;
        result.required_size = payload_size;
        return result;
    }
    u64 actual_size = 0u;
    const TResult<u32> loaded = FSaveArchive::ReadFromFile(
        file_path, encoded.Data(), payload_size,
        kFileEnvelopeVersion, actual_size);
    if (loaded.IsErr()) {
        return MakePersistenceFailure(
            loaded.Error(), payload_size);
    }
    if (actual_size != payload_size) {
        FAnimationCurveArchiveResult result{};
        result.error = EAnimationCurveArchiveError::SizeMismatch;
        result.required_size = payload_size;
        return result;
    }
    return Decode(encoded.Data(), payload_size, out_curve);
}

} // namespace acs::game

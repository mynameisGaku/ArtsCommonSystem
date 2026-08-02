// SPDX-License-Identifier: Apache-2.0

#include "test/Test.h"
#include "test/Expect.h"
#include "container/Array.h"
#include "foundation/Platform.h"
#include "gameframework/AnimationCurveArchive.h"
#include "gameframework/Easing.h"
#include "gameframework/SaveArchive.h"
#include "memory/Memory.h"
#include "memory/SystemAllocator.h"

using namespace acs;
using namespace acs::game;

namespace {

class FSwitchableArchiveAllocator final : public IAllocator {
public:
    explicit FSwitchableArchiveAllocator(IAllocator& backing) noexcept
        : m_Backing(&backing) {}

    void SetFailing(bool failing) noexcept { m_Failing = failing; }

    void* Alloc(usize size, usize alignment,
                FSourceLoc location) noexcept override {
        return m_Failing
            ? nullptr
            : m_Backing->Alloc(size, alignment, location);
    }

    void Free(void* pointer) noexcept override {
        m_Backing->Free(pointer);
    }

private:
    IAllocator* m_Backing = nullptr;
    bool m_Failing = false;
};

FCurveKey MakeKey(
    f32 time, f32 value,
    ECurveInterpolation interpolation =
        ECurveInterpolation::Linear) noexcept {
    FCurveKey key{};
    key.time = time;
    key.value = value;
    key.in_tangent = value * 0.25f;
    key.out_tangent = value * 0.5f;
    key.in_interp = interpolation;
    key.out_interp = interpolation;
    return key;
}

bool BuildSourceCurve(FAnimationCurve& curve) noexcept {
    const FCurveKey keys[] = {
        MakeKey(-1.0f, 2.0f, ECurveInterpolation::Step),
        MakeKey(0.5f, -4.0f, ECurveInterpolation::Linear),
        MakeKey(3.0f, 7.5f, ECurveInterpolation::Hermite),
    };
    return curve.TrySetKeys(
        keys, 3u, FAnimationCurve::EWrapMode::Loop,
        FAnimationCurve::EWrapMode::PingPong).Succeeded();
}

bool CurvesEqual(
    const FAnimationCurve& left,
    const FAnimationCurve& right) noexcept {
    if (left.KeyCount() != right.KeyCount() ||
        left.PreWrap() != right.PreWrap() ||
        left.PostWrap() != right.PostWrap()) {
        return false;
    }
    for (u32 index = 0u; index < left.KeyCount(); ++index) {
        const FCurveKey& a = *left.Key(index);
        const FCurveKey& b = *right.Key(index);
        if (a.time != b.time || a.value != b.value ||
            a.in_tangent != b.in_tangent ||
            a.out_tangent != b.out_tangent ||
            a.in_interp != b.in_interp ||
            a.out_interp != b.out_interp) {
            return false;
        }
    }
    return true;
}

bool EncodeCurve(
    const FAnimationCurve& curve,
    TArray<u8>& bytes) noexcept {
    const u64 required =
        CAnimationCurveArchive::EncodedSize(curve);
    if (!bytes.TryResize(static_cast<usize>(required))) return false;
    u64 written = 0u;
    return CAnimationCurveArchive::Encode(
        curve, bytes.Data(), required, written).Succeeded() &&
        written == required;
}

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

void WriteF32LE(u8* destination, f32 value) noexcept {
    u32 bits = 0u;
    MemCopy(&bits, &value, sizeof(bits));
    WriteU32LE(destination, bits);
}

u32 WireCrc32(const u8* bytes, usize size) noexcept {
    u32 crc = 0xFFFFFFFFu;
    for (usize index = 0u; index < size; ++index) {
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

void RefreshWireCrc(TArray<u8>& bytes) noexcept {
    WriteU32LE(bytes.Data() + 24u, 0u);
    WriteU32LE(
        bytes.Data() + 24u,
        WireCrc32(bytes.Data(), bytes.Size()));
}

bool BuildDestinationSentinel(FAnimationCurve& curve) noexcept {
    const FCurveKey keys[] = {
        MakeKey(8.0f, 88.0f),
        MakeKey(9.0f, 99.0f),
    };
    return curve.TrySetKeys(
        keys, 2u, FAnimationCurve::EWrapMode::PingPong,
        FAnimationCurve::EWrapMode::Loop).Succeeded();
}

bool IsDestinationSentinel(
    const FAnimationCurve& curve) noexcept {
    return curve.KeyCount() == 2u &&
           curve.PreWrap() ==
               FAnimationCurve::EWrapMode::PingPong &&
           curve.PostWrap() ==
               FAnimationCurve::EWrapMode::Loop &&
           curve.Key(0u) != nullptr &&
           curve.Key(1u) != nullptr &&
           curve.Key(0u)->time == 8.0f &&
           curve.Key(0u)->value == 88.0f &&
           curve.Key(1u)->time == 9.0f &&
           curve.Key(1u)->value == 99.0f;
}

struct FTempCurvePath {
    explicit FTempCurvePath(const wchar_t* tag) noexcept {
        const DWORD temp_length = ::GetTempPathW(
            static_cast<DWORD>(kCapacity), path);
        usize position = 0u;
        if (temp_length > 0u &&
            temp_length < static_cast<DWORD>(kCapacity)) {
            position = static_cast<usize>(temp_length);
        } else {
            // GetTempPathWは失敗時に0、出力先が小さすぎる場合に必要sizeを返す。
            // どちらの値も配列indexとして扱わない。
            path[position++] = L'.';
            path[position++] = L'\\';
            path[position] = L'\0';
        }
        const auto append = [this, &position](wchar_t character) noexcept {
            if (position + 1u >= kCapacity) return false;
            path[position++] = character;
            path[position] = L'\0';
            return true;
        };
        const wchar_t prefix[] = L"acs_curve_archive_";
        for (usize i = 0u; prefix[i] != L'\0'; ++i) {
            if (!append(prefix[i])) break;
        }
        u32 process = static_cast<u32>(::GetCurrentProcessId());
        wchar_t digits[10] = {};
        usize count = 0u;
        do {
            digits[count++] =
                static_cast<wchar_t>(L'0' + process % 10u);
            process /= 10u;
        } while (process != 0u);
        while (count > 0u) {
            if (!append(digits[--count])) break;
        }
        append(L'_');
        if (tag != nullptr) {
            while (*tag != L'\0') {
                if (!append(*tag++)) break;
            }
        }
        const wchar_t suffix[] = L".acssave";
        for (usize i = 0u; suffix[i] != L'\0'; ++i) {
            if (!append(suffix[i])) break;
        }
        ::DeleteFileW(path);
    }

    ~FTempCurvePath() noexcept { ::DeleteFileW(path); }

    static constexpr usize kCapacity = MAX_PATH + 64u;
    wchar_t path[kCapacity] = {};
};

} // namespace

ACS_TEST(AnimationCurvePersistence, CanonicalRoundTripPreservesAllFields) {
    FAnimationCurve source;
    EXPECT_TRUE(BuildSourceCurve(source));

    TArray<u8> bytes;
    EXPECT_TRUE(EncodeCurve(source, bytes));
    EXPECT_EQ(
        bytes.Size(),
        static_cast<usize>(
            CAnimationCurveArchive::kHeaderSize +
            3u * CAnimationCurveArchive::kKeyRecordSize));

    FAnimationCurve restored;
    const FAnimationCurveArchiveResult result =
        CAnimationCurveArchive::Decode(
            bytes.Data(), bytes.Size(), restored);
    EXPECT_TRUE(result.Succeeded());
    EXPECT_TRUE(CurvesEqual(source, restored));
}

ACS_TEST(AnimationCurvePersistence, EmptyCurveEncodingMatchesGoldenBytes) {
    /** 既定 wrap を持つ空の曲線。 */
    FAnimationCurve source;
    /** 既定曲線の固定長 wire 出力。 */
    u8 bytes[CAnimationCurveArchive::kHeaderSize]{};
    /** Encode が報告する書き込み byte 数。 */
    u64 written = 0u;
    /** version、予約領域、CRC を含む既存 wire の正準 byte 列。 */
    constexpr u8 kGoldenBytes[CAnimationCurveArchive::kHeaderSize]{0x41u, 0x43u, 0x53u, 0x43u, 0x55u, 0x52u, 0x56u, 0x00u, 0x01u, 0x00u, 0x20u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xCFu, 0xEFu, 0xD1u, 0x76u, 0x00u, 0x00u, 0x00u, 0x00u};

    EXPECT_TRUE(CAnimationCurveArchive::Encode(source, bytes, sizeof(bytes), written).Succeeded());
    EXPECT_EQ(written, static_cast<u64>(sizeof(kGoldenBytes)));
    /** 正準 wire と比較する byte 位置。 */
    for (usize index = 0u; index < sizeof(kGoldenBytes); ++index) EXPECT_EQ(bytes[index], kGoldenBytes[index]);
}

ACS_TEST(AnimationCurvePersistence, EveryEasingPresetRoundTripsCanonically) {
    for (u32 value = 0u;
         value < static_cast<u32>(Easing::EEasingType::Count);
         ++value) {
        FAnimationCurve source;
        EXPECT_TRUE(source.TrySetEasingPreset(
            static_cast<Easing::EEasingType>(value), 65u).Succeeded());

        TArray<u8> bytes;
        EXPECT_TRUE(EncodeCurve(source, bytes));

        FAnimationCurve decoded;
        const FAnimationCurveArchiveResult result =
            CAnimationCurveArchive::Decode(
                bytes.Data(), bytes.Size(), decoded);
        EXPECT_TRUE(result.Succeeded());
        EXPECT_TRUE(CurvesEqual(source, decoded));

        TArray<u8> reencoded;
        EXPECT_TRUE(EncodeCurve(decoded, reencoded));
        EXPECT_EQ(reencoded.Size(), bytes.Size());
        if (reencoded.Size() == bytes.Size()) {
            bool bytes_equal = true;
            for (usize index = 0u; index < bytes.Size(); ++index) {
                if (bytes[index] != reencoded[index]) {
                    bytes_equal = false;
                    break;
                }
            }
            EXPECT_TRUE(bytes_equal);
        }
    }
}

ACS_TEST(AnimationCurvePersistence, EmptyCurveRoundTripsWithWrapModes) {
    FAnimationCurve source;
    EXPECT_TRUE(source.TrySetWrapModes(
        FAnimationCurve::EWrapMode::PingPong,
        FAnimationCurve::EWrapMode::Loop).Succeeded());
    TArray<u8> bytes;
    EXPECT_TRUE(EncodeCurve(source, bytes));

    FAnimationCurve restored;
    EXPECT_TRUE(CAnimationCurveArchive::Decode(
        bytes.Data(), bytes.Size(), restored).Succeeded());
    EXPECT_EQ(restored.KeyCount(), 0u);
    EXPECT_EQ(restored.PreWrap(),
              FAnimationCurve::EWrapMode::PingPong);
    EXPECT_EQ(restored.PostWrap(),
              FAnimationCurve::EWrapMode::Loop);
}

ACS_TEST(AnimationCurvePersistence, CapacityQueryDoesNotTouchOutput) {
    FAnimationCurve source;
    EXPECT_TRUE(BuildSourceCurve(source));
    u8 sentinel[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    u8 before[8] = {};
    MemCopy(before, sentinel, sizeof(before));
    u64 required = 0u;

    const FAnimationCurveArchiveResult result =
        CAnimationCurveArchive::Encode(
            source, sentinel, sizeof(sentinel), required);
    EXPECT_EQ(result.error,
              EAnimationCurveArchiveError::BufferTooSmall);
    EXPECT_EQ(required,
              CAnimationCurveArchive::EncodedSize(source));
    EXPECT_TRUE(MemCmp(
        sentinel, before, sizeof(sentinel)) == 0);
}

ACS_TEST(AnimationCurvePersistence, HeaderFailuresAreTransactional) {
    FAnimationCurve source;
    EXPECT_TRUE(BuildSourceCurve(source));
    TArray<u8> bytes;
    EXPECT_TRUE(EncodeCurve(source, bytes));

    FAnimationCurve destination;
    EXPECT_TRUE(destination.TryAddKey(10.0f, 99.0f).Succeeded());

    bytes[0u] ^= 0x80u;
    FAnimationCurveArchiveResult result =
        CAnimationCurveArchive::Decode(
            bytes.Data(), bytes.Size(), destination);
    EXPECT_EQ(result.error,
              EAnimationCurveArchiveError::InvalidMagic);
    EXPECT_EQ(destination.KeyCount(), 1u);
    EXPECT_NEAR(destination.Key(0u)->value, 99.0f, 1e-6f);
    bytes[0u] ^= 0x80u;

    WriteU16LE(bytes.Data() + 8u, 77u);
    result = CAnimationCurveArchive::Decode(
        bytes.Data(), bytes.Size(), destination);
    EXPECT_EQ(result.error,
              EAnimationCurveArchiveError::UnsupportedVersion);
    EXPECT_EQ(destination.KeyCount(), 1u);
}

ACS_TEST(AnimationCurvePersistence, ExactSizeAndReservedBytesAreEnforced) {
    FAnimationCurve source;
    EXPECT_TRUE(BuildSourceCurve(source));
    TArray<u8> bytes;
    EXPECT_TRUE(EncodeCurve(source, bytes));
    FAnimationCurve destination;
    EXPECT_TRUE(destination.TryAddKey(9.0f, 11.0f).Succeeded());

    FAnimationCurveArchiveResult result =
        CAnimationCurveArchive::Decode(
            bytes.Data(), bytes.Size() - 1u, destination);
    EXPECT_EQ(result.error,
              EAnimationCurveArchiveError::SizeMismatch);

    bytes[18u] = 1u;
    result = CAnimationCurveArchive::Decode(
        bytes.Data(), bytes.Size(), destination);
    EXPECT_EQ(result.error,
              EAnimationCurveArchiveError::InvalidHeader);
    EXPECT_NEAR(destination.Key(0u)->value, 11.0f, 1e-6f);
}

ACS_TEST(AnimationCurvePersistence, HugeCountIsRejectedBeforeAllocation) {
    u8 header[CAnimationCurveArchive::kHeaderSize] = {};
    const u8 magic[8] = {
        'A', 'C', 'S', 'C', 'U', 'R', 'V', '\0'};
    MemCopy(header, magic, sizeof(magic));
    WriteU16LE(header + 8u,
               CAnimationCurveArchive::kWireVersion);
    WriteU16LE(header + 10u,
               static_cast<u16>(
                   CAnimationCurveArchive::kHeaderSize));
    WriteU32LE(header + 12u,
               FAnimationCurve::kMaxKeys + 1u);

    FAnimationCurve destination;
    const FAnimationCurveArchiveResult result =
        CAnimationCurveArchive::Decode(
            header, sizeof(header), destination);
    EXPECT_EQ(result.error,
              EAnimationCurveArchiveError::TooManyKeys);
    EXPECT_EQ(destination.KeyCount(), 0u);
}

ACS_TEST(AnimationCurvePersistence, PayloadCrcDetectsBitFlip) {
    FAnimationCurve source;
    EXPECT_TRUE(BuildSourceCurve(source));
    TArray<u8> bytes;
    EXPECT_TRUE(EncodeCurve(source, bytes));
    bytes[CAnimationCurveArchive::kHeaderSize + 5u] ^= 0x40u;

    FAnimationCurve destination;
    EXPECT_TRUE(destination.TryAddKey(1.0f, 88.0f).Succeeded());
    const FAnimationCurveArchiveResult result =
        CAnimationCurveArchive::Decode(
            bytes.Data(), bytes.Size(), destination);
    EXPECT_EQ(result.error,
              EAnimationCurveArchiveError::ChecksumMismatch);
    EXPECT_NEAR(destination.Key(0u)->value, 88.0f, 1e-6f);
}

ACS_TEST(AnimationCurvePersistence, HeaderCrcBindsWrapModes) {
    FAnimationCurve source;
    EXPECT_TRUE(BuildSourceCurve(source));
    TArray<u8> bytes;
    EXPECT_TRUE(EncodeCurve(source, bytes));
    // Loop (1)からClamp (0)への変更後もenum自体は有効である。変更後のbyteが
    // 有効という理由だけで意味の変化を見逃してはならない。
    bytes[16u] = static_cast<u8>(
        FAnimationCurve::EWrapMode::Clamp);

    FAnimationCurve destination;
    EXPECT_TRUE(destination.TryAddKey(1.0f, 77.0f).Succeeded());
    const FAnimationCurveArchiveResult result =
        CAnimationCurveArchive::Decode(
            bytes.Data(), bytes.Size(), destination);
    EXPECT_EQ(result.error,
              EAnimationCurveArchiveError::ChecksumMismatch);
    EXPECT_NEAR(destination.Key(0u)->value, 77.0f, 1e-6f);
}

ACS_TEST(AnimationCurvePersistence, ValidCrcDoesNotBypassKeyValidation) {
    FAnimationCurve source;
    EXPECT_TRUE(BuildSourceCurve(source));
    TArray<u8> bytes;
    EXPECT_TRUE(EncodeCurve(source, bytes));

    FAnimationCurve destination;
    EXPECT_TRUE(BuildDestinationSentinel(destination));

    // quiet NaNはIEEE-754表現として構造上有効だが、curve dataとしては無効。
    // semantic検証まで到達させるためwire CRCを再計算する。
    WriteU32LE(
        bytes.Data() + CAnimationCurveArchive::kHeaderSize + 4u,
        0x7FC00000u);
    RefreshWireCrc(bytes);
    FAnimationCurveArchiveResult result =
        CAnimationCurveArchive::Decode(
            bytes.Data(), bytes.Size(), destination);
    EXPECT_EQ(result.error,
              EAnimationCurveArchiveError::InvalidCurveData);
    EXPECT_EQ(result.curve_error,
              EAnimationCurveError::NonFiniteValue);
    EXPECT_EQ(result.key_index, 0u);
    EXPECT_TRUE(IsDestinationSentinel(destination));

    EXPECT_TRUE(EncodeCurve(source, bytes));
    bytes[CAnimationCurveArchive::kHeaderSize + 16u] = 0xFFu;
    RefreshWireCrc(bytes);
    result = CAnimationCurveArchive::Decode(
        bytes.Data(), bytes.Size(), destination);
    EXPECT_EQ(result.error,
              EAnimationCurveArchiveError::InvalidCurveData);
    EXPECT_EQ(result.curve_error,
              EAnimationCurveError::InvalidInterpolation);
    EXPECT_EQ(result.key_index, 0u);
    EXPECT_TRUE(IsDestinationSentinel(destination));

    EXPECT_TRUE(EncodeCurve(source, bytes));
    WriteF32LE(
        bytes.Data() + CAnimationCurveArchive::kHeaderSize +
            CAnimationCurveArchive::kKeyRecordSize,
        -1.0f);
    RefreshWireCrc(bytes);
    result = CAnimationCurveArchive::Decode(
        bytes.Data(), bytes.Size(), destination);
    EXPECT_EQ(result.error,
              EAnimationCurveArchiveError::InvalidCurveData);
    EXPECT_EQ(result.curve_error,
              EAnimationCurveError::DuplicateKeyTime);
    EXPECT_EQ(result.key_index, 1u);
    EXPECT_TRUE(IsDestinationSentinel(destination));

    EXPECT_TRUE(EncodeCurve(source, bytes));
    WriteF32LE(
        bytes.Data() + CAnimationCurveArchive::kHeaderSize +
            CAnimationCurveArchive::kKeyRecordSize,
        -2.0f);
    RefreshWireCrc(bytes);
    result = CAnimationCurveArchive::Decode(
        bytes.Data(), bytes.Size(), destination);
    EXPECT_EQ(result.error,
              EAnimationCurveArchiveError::InvalidCurveData);
    EXPECT_EQ(result.curve_error,
              EAnimationCurveError::UnsortedKeys);
    EXPECT_EQ(result.key_index, 1u);
    EXPECT_TRUE(IsDestinationSentinel(destination));
}

ACS_TEST(AnimationCurvePersistence, ValidCrcRejectsRecordReservedBytes) {
    FAnimationCurve source;
    EXPECT_TRUE(BuildSourceCurve(source));
    TArray<u8> bytes;
    EXPECT_TRUE(EncodeCurve(source, bytes));
    bytes[CAnimationCurveArchive::kHeaderSize +
          2u * CAnimationCurveArchive::kKeyRecordSize + 18u] = 1u;
    RefreshWireCrc(bytes);

    FAnimationCurve destination;
    EXPECT_TRUE(BuildDestinationSentinel(destination));
    const FAnimationCurveArchiveResult result =
        CAnimationCurveArchive::Decode(
            bytes.Data(), bytes.Size(), destination);
    EXPECT_EQ(result.error,
              EAnimationCurveArchiveError::InvalidHeader);
    EXPECT_EQ(result.key_index, 2u);
    EXPECT_TRUE(IsDestinationSentinel(destination));
}

ACS_TEST(AnimationCurvePersistence, DestinationOomPreservesCurve) {
    FAnimationCurve source;
    EXPECT_TRUE(BuildSourceCurve(source));
    TArray<u8> bytes;
    EXPECT_TRUE(EncodeCurve(source, bytes));

    CSystemAllocator backing;
    FSwitchableArchiveAllocator allocator(backing);
    FAnimationCurve destination(allocator);
    EXPECT_TRUE(destination.TryAddKey(1.0f, 55.0f).Succeeded());
    allocator.SetFailing(true);

    const FAnimationCurveArchiveResult result =
        CAnimationCurveArchive::Decode(
            bytes.Data(), bytes.Size(), destination);
    EXPECT_EQ(result.error,
              EAnimationCurveArchiveError::AllocationFailure);
    EXPECT_EQ(result.curve_error,
              EAnimationCurveError::AllocationFailure);
    EXPECT_EQ(destination.KeyCount(), 1u);
    EXPECT_NEAR(destination.Key(0u)->value, 55.0f, 1e-6f);
}

ACS_TEST(AnimationCurvePersistence, FileRoundTripUsesValidatedEnvelope) {
    FTempCurvePath path(L"roundtrip");
    FAnimationCurve source;
    EXPECT_TRUE(BuildSourceCurve(source));
    EXPECT_TRUE(CAnimationCurveArchive::SaveToFile(
        path.path, source).Succeeded());

    FAnimationCurve restored;
    EXPECT_TRUE(CAnimationCurveArchive::LoadFromFile(
        path.path, restored).Succeeded());
    EXPECT_TRUE(CurvesEqual(source, restored));
}

ACS_TEST(AnimationCurvePersistence, MissingFileDoesNotChangeDestination) {
    FTempCurvePath path(L"missing");
    FAnimationCurve destination;
    EXPECT_TRUE(destination.TryAddKey(2.0f, 123.0f).Succeeded());
    const FAnimationCurveArchiveResult result =
        CAnimationCurveArchive::LoadFromFile(
            path.path, destination);
    EXPECT_EQ(result.error,
              EAnimationCurveArchiveError::PersistenceFailure);
    EXPECT_EQ(destination.KeyCount(), 1u);
    EXPECT_NEAR(destination.Key(0u)->value, 123.0f, 1e-6f);
}

ACS_TEST(AnimationCurvePersistence, ValidEnvelopeRejectsInvalidInnerWire) {
    FTempCurvePath path(L"invalid_inner");
    FAnimationCurve source;
    EXPECT_TRUE(BuildSourceCurve(source));
    TArray<u8> bytes;
    EXPECT_TRUE(EncodeCurve(source, bytes));

    bytes[CAnimationCurveArchive::kHeaderSize + 16u] = 0xFFu;
    RefreshWireCrc(bytes);
    EXPECT_TRUE(CSaveArchive::WriteToFile(
        path.path,
        CAnimationCurveArchive::kFileEnvelopeVersion,
        bytes.Data(), bytes.Size()).IsOk());

    FAnimationCurve destination;
    EXPECT_TRUE(BuildDestinationSentinel(destination));
    const FAnimationCurveArchiveResult result =
        CAnimationCurveArchive::LoadFromFile(
            path.path, destination);
    EXPECT_EQ(result.error,
              EAnimationCurveArchiveError::InvalidCurveData);
    EXPECT_EQ(result.curve_error,
              EAnimationCurveError::InvalidInterpolation);
    EXPECT_EQ(result.key_index, 0u);
    EXPECT_TRUE(IsDestinationSentinel(destination));
}

ACS_TEST(AnimationCurvePersistence, ErrorNamesAreStable) {
    EXPECT_TRUE(
        FAnimationCurveArchiveResult::ErrorName(
            EAnimationCurveArchiveError::ChecksumMismatch)[0] == 'C');
    EXPECT_TRUE(
        FAnimationCurveArchiveResult::ErrorName(
            static_cast<EAnimationCurveArchiveError>(255u))[0] == 'U');
}

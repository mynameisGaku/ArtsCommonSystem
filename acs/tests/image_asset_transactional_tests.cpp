// SPDX-License-Identifier: Apache-2.0
#include "asset/ImageAsset.h"
#include "foundation/Limits.h"
#include "memory/Memory.h"
#include "memory/SystemAllocator.h"
#include "test/Expect.h"
#include "test/Test.h"

#include <cstdlib>
#include <type_traits>

namespace acs {

namespace image_asset_test_detail {

/** usizeの入力長をdecoder用intへ変換する製品preflightを呼ぶ。 */
bool TryConvertDecoderInputLengthForTesting(usize input_size, int& decoder_input_length) noexcept;

/** decode済み画像layoutの製品preflightを呼ぶ。 */
bool TryCalculateDecodedByteCountForTesting(int width, int height, int channels, usize bytes_per_pixel, usize& byte_count) noexcept;

} // namespace image_asset_test_detail

namespace {

/** stb_imageが同時に保持できる試験追跡領域数。 */
inline constexpr usize kMaximumTrackedStbiAllocations = 256u;

/** 2x1 RGBA画像を保持する最小PNG fixture。 */
constexpr byte kPngFixture[] = { 0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au, 0x00u, 0x00u, 0x00u, 0x0Du, 0x49u, 0x48u, 0x44u, 0x52u, 0x00u, 0x00u, 0x00u, 0x02u, 0x00u, 0x00u, 0x00u, 0x01u, 0x08u, 0x06u, 0x00u, 0x00u, 0x00u, 0xF4u, 0x22u, 0x7Fu, 0x8Au, 0x00u, 0x00u, 0x00u, 0x11u, 0x49u, 0x44u, 0x41u, 0x54u, 0x78u, 0xDAu, 0x63u, 0xF8u, 0xCFu, 0xC0u, 0xF0u, 0x9Fu, 0xA1u, 0xE1u, 0xBFu, 0x03u, 0x00u, 0x10u, 0xBAu, 0x03u, 0xBEu, 0x3Au, 0x79u, 0xBFu, 0xBEu, 0x00u, 0x00u, 0x00u, 0x00u, 0x49u, 0x45u, 0x4Eu, 0x44u, 0xAEu, 0x42u, 0x60u, 0x82u };

/** 1x1 RGBE画像を保持する最小Radiance HDR fixture。 */
constexpr byte kHdrFixture[] = { 0x23u, 0x3Fu, 0x52u, 0x41u, 0x44u, 0x49u, 0x41u, 0x4Eu, 0x43u, 0x45u, 0x0Au, 0x46u, 0x4Fu, 0x52u, 0x4Du, 0x41u, 0x54u, 0x3Du, 0x33u, 0x32u, 0x2Du, 0x62u, 0x69u, 0x74u, 0x5Fu, 0x72u, 0x6Cu, 0x65u, 0x5Fu, 0x72u, 0x67u, 0x62u, 0x65u, 0x0Au, 0x0Au, 0x2Du, 0x59u, 0x20u, 0x31u, 0x20u, 0x2Bu, 0x58u, 0x20u, 0x31u, 0x0Au, 0x80u, 0x40u, 0x20u, 0x81u };

/** PNG fixtureをdecodeしたRGBA byte列。 */
constexpr byte kExpectedPngPixels[] = { 0xFFu, 0x00u, 0x00u, 0xFFu, 0x00u, 0x80u, 0xFFu, 0x40u };

/** HDR fixtureをdecodeしたlinear RGBA値。 */
constexpr f32 kExpectedHdrPixels[] = { 1.0f, 0.5f, 0.25f, 1.0f };

/** stb_image内部確保の所有数と解放を固定領域だけで追跡する。 */
class CStbiAllocationTracker final {
public:
    /** 未解放領域が無い場合だけ次の試行用に観測値を初期化する。 */
    bool Reset() noexcept
    {
        if (m_OutstandingCount != 0u) return false;
        m_AllocationCallCount = 0u;
        m_ReallocationCallCount = 0u;
        m_FreeCallCount = 0u;
        m_UnexpectedPointer = false;
        return true;
    }

    /** stb_image用領域をC runtimeから確保して追跡する。 */
    void* Allocate(usize size) noexcept
    {
        ++m_AllocationCallCount;

        /** C runtimeが返したstb_image用領域。 */
        void* const pointer = std::malloc(size);
        if (pointer == nullptr) return nullptr;
        if (m_OutstandingCount >= kMaximumTrackedStbiAllocations) {
            m_UnexpectedPointer = true;
            std::free(pointer);
            return nullptr;
        }
        m_Pointers[m_OutstandingCount++] = pointer;
        return pointer;
    }

    /** stb_image用領域をC runtimeで再確保し、同じ追跡slotを更新する。 */
    void* Reallocate(void* pointer, usize size) noexcept
    {
        ++m_ReallocationCallCount;
        if (pointer == nullptr) return Allocate(size);
        if (size == 0u) {
            Release(pointer);
            return nullptr;
        }

        /** 再確保前の領域を保持する追跡slot。 */
        const usize index = Find(pointer);
        if (index == kMaximumTrackedStbiAllocations) {
            m_UnexpectedPointer = true;
            return nullptr;
        }

        /** C runtimeが返した再確保後の領域。 */
        void* const resized = std::realloc(pointer, size);
        if (resized != nullptr) m_Pointers[index] = resized;
        return resized;
    }

    /** stb_image用領域を追跡から外してC runtimeへ返す。 */
    void Release(void* pointer) noexcept
    {
        if (pointer == nullptr) return;
        ++m_FreeCallCount;

        /** 解放領域を保持している追跡slot。 */
        const usize index = Find(pointer);
        if (index == kMaximumTrackedStbiAllocations) {
            m_UnexpectedPointer = true;
            std::free(pointer);
            return;
        }

        --m_OutstandingCount;
        m_Pointers[index] = m_Pointers[m_OutstandingCount];
        m_Pointers[m_OutstandingCount] = nullptr;
        std::free(pointer);
    }

    /** 現在未解放のstb_image領域数を返す。 */
    usize OutstandingCount() const noexcept { return m_OutstandingCount; }

    /** malloc系hookへ到達した回数を返す。 */
    u64 AllocationCallCount() const noexcept { return m_AllocationCallCount; }

    /** free hookへ到達した回数を返す。 */
    u64 FreeCallCount() const noexcept { return m_FreeCallCount; }

    /** 追跡外pointerまたは追跡上限超過を検出したか返す。 */
    bool HadUnexpectedPointer() const noexcept { return m_UnexpectedPointer; }

private:
    /** pointerを保持する追跡slotを検索する。 */
    usize Find(const void* pointer) const noexcept
    {
        for (usize index = 0u; index < m_OutstandingCount; ++index) {
            if (m_Pointers[index] == pointer) return index;
        }
        return kMaximumTrackedStbiAllocations;
    }

    /** 未解放のstb_image領域。 */
    void* m_Pointers[kMaximumTrackedStbiAllocations]{};

    /** 現在使用中の追跡slot数。 */
    usize m_OutstandingCount = 0u;

    /** malloc系hookの呼出回数。 */
    u64 m_AllocationCallCount = 0u;

    /** realloc hookの呼出回数。 */
    u64 m_ReallocationCallCount = 0u;

    /** free hookの呼出回数。 */
    u64 m_FreeCallCount = 0u;

    /** 追跡不能なpointerを検出した状態。 */
    bool m_UnexpectedPointer = false;
};

/** 専用試験から参照するstb_image確保追跡器。 */
CStbiAllocationTracker g_StbiAllocations;

/** 指定要求だけを失敗させ、ACS側の未解放数を観測するallocator。 */
class CFailOnImageRequestAllocator final : public IAllocator {
public:
    /** 次のloadで失敗させる1始まりの要求番号を設定する。0なら失敗させない。 */
    void Begin(u64 failing_request) noexcept
    {
        m_RequestCount = 0u;
        m_FailingRequest = failing_request;
    }

    /** 現在のloadで受けた確保要求数を返す。 */
    u64 RequestCount() const noexcept { return m_RequestCount; }

    /** 現在未解放のACS側確保数を返す。 */
    u64 OutstandingAllocationCount() const noexcept { return m_Backing.AllocationCount(); }

    /** 指定要求だけを拒否し、それ以外をbackingへ渡す。 */
    void* Alloc(usize size, usize alignment, FSourceLoc location) noexcept override
    {
        ++m_RequestCount;
        if (m_FailingRequest != 0u && m_RequestCount == m_FailingRequest) return nullptr;
        return m_Backing.Alloc(size, alignment, location);
    }

    /** ACS側領域をbackingへ返す。 */
    void Free(void* pointer) noexcept override { m_Backing.Free(pointer); }

    /** ACS側で現在保持するbyte数を返す。 */
    u64 BytesAllocated() const noexcept override { return m_Backing.BytesAllocated(); }

    /** ACS側で現在保持する確保数を返す。 */
    u64 AllocationCount() const noexcept override { return m_Backing.AllocationCount(); }

private:
    /** 実際の確保と解放を担当するsystem allocator。 */
    CSystemAllocator m_Backing;

    /** 現在のloadで受けた確保要求数。 */
    u64 m_RequestCount = 0u;

    /** 失敗させる1始まりの確保要求番号。 */
    u64 m_FailingRequest = 0u;
};

/** 試験中だけ既定allocatorを差し替え、終了時に必ず戻す。 */
class CDefaultAllocatorScope final {
public:
    /** 指定allocatorを既定値へ設定する。 */
    explicit CDefaultAllocatorScope(IAllocator& allocator) noexcept : m_Previous(&DefaultAllocator()) { SetDefaultAllocator(&allocator); }

    /** 差し替え前の既定allocatorを復元する。 */
    ~CDefaultAllocatorScope() noexcept { SetDefaultAllocator(m_Previous); }

    CDefaultAllocatorScope(const CDefaultAllocatorScope&) = delete;
    CDefaultAllocatorScope& operator=(const CDefaultAllocatorScope&) = delete;

private:
    /** 差し替え前の既定allocator。 */
    IAllocator* m_Previous = nullptr;
};

/** 固定fixtureをloader入力用TArrayへ複製する。 */
template<usize Count>
TArray<byte> MakeFixtureBytes(const byte (&source)[Count]) noexcept
{
    /** loaderへ渡す所有byte列。 */
    TArray<byte> bytes;
    if (!bytes.TrySetNum(Count)) return TArray<byte>();
    MemCopy(bytes.GetData(), source, Count);
    return bytes;
}

/** 成功assetの共通ID・状態・runtime typeを検査する。 */
void ExpectReadyImage(const TSharedPtr<AAsset>& asset, FAssetId expected_id) noexcept
{
    EXPECT_TRUE(asset.Get() != nullptr);
    if (!asset) return;
    EXPECT_EQ(asset->Id(), expected_id);
    EXPECT_EQ(asset->State(), EAssetState::Ready);
    EXPECT_EQ(asset->Type(), AImageAsset::StaticType());
}

/** 指定fixtureの全ACS allocation要求位置とstb_image解放を検査する。 */
void ExpectAllocationMatrix(const TArray<byte>& fixture, EPixelFormat expected_format) noexcept
{
    /** load対象の固定asset ID。 */
    const FAssetId expected_id{0x123456789ABCDEF0ull};

    /** 画像loader実体。 */
    CImageAssetLoader loader;

    /** ACS側確保を追跡するallocator。 */
    CFailOnImageRequestAllocator allocator;

    /** 成功経路が使うACS側確保要求数。 */
    u64 successful_request_count = 0u;
    allocator.Begin(0u);
    EXPECT_TRUE(g_StbiAllocations.Reset());
    {
        CDefaultAllocatorScope allocator_scope(allocator);
        {
            /** 全確保を許可したload結果。 */
            auto result = loader.LoadFromBytes(expected_id, fixture);
            EXPECT_TRUE(result.IsOk());
            if (result.IsOk()) {
                ExpectReadyImage(result.Value(), expected_id);

                /** format検査対象の具体画像asset。 */
                const AImageAsset* const image = static_cast<const AImageAsset*>(result.Value().Get());
                EXPECT_EQ(image->Format(), expected_format);
            }
            successful_request_count = allocator.RequestCount();
            EXPECT_EQ(allocator.OutstandingAllocationCount(), static_cast<u64>(2u));
            EXPECT_EQ(g_StbiAllocations.OutstandingCount(), static_cast<usize>(0u));
            EXPECT_TRUE(g_StbiAllocations.AllocationCallCount() > 0u);
            EXPECT_TRUE(g_StbiAllocations.FreeCallCount() > 0u);
            EXPECT_FALSE(g_StbiAllocations.HadUnexpectedPointer());
        }
    }
    EXPECT_EQ(successful_request_count, static_cast<u64>(2u));
    EXPECT_EQ(allocator.OutstandingAllocationCount(), static_cast<u64>(0u));

    for (u64 failing_request = 1u; failing_request <= successful_request_count; ++failing_request) {
        allocator.Begin(failing_request);
        EXPECT_TRUE(g_StbiAllocations.Reset());
        {
            CDefaultAllocatorScope allocator_scope(allocator);

            /** 指定ACS確保だけを拒否したload結果。 */
            auto result = loader.LoadFromBytes(expected_id, fixture);
            EXPECT_TRUE(result.IsErr());
            if (result.IsErr()) EXPECT_EQ(result.Error().category, EErrCategory::Memory);
        }
        EXPECT_EQ(allocator.RequestCount(), failing_request);
        EXPECT_EQ(allocator.OutstandingAllocationCount(), static_cast<u64>(0u));
        EXPECT_EQ(g_StbiAllocations.OutstandingCount(), static_cast<usize>(0u));
        EXPECT_TRUE(g_StbiAllocations.AllocationCallCount() > 0u);
        EXPECT_TRUE(g_StbiAllocations.FreeCallCount() > 0u);
        EXPECT_FALSE(g_StbiAllocations.HadUnexpectedPointer());
    }
}

/** CImageAssetLoader::LoadFromBytesの公開member関数型。 */
using FImageLoadSignature = TResult<TSharedPtr<AAsset>> (CImageAssetLoader::*)(FAssetId, const TArray<byte>&) noexcept;

static_assert(std::is_same_v<decltype(&CImageAssetLoader::LoadFromBytes), FImageLoadSignature>);
static_assert(std::is_same_v<FImageAsset, AImageAsset>);
static_assert(std::is_same_v<FImageAssetLoader, CImageAssetLoader>);
static_assert(std::is_polymorphic_v<AImageAsset>);
#if defined(_WIN64)
static_assert(sizeof(AImageAsset) == 72u);
static_assert(alignof(AImageAsset) == 8u);
static_assert(sizeof(CImageAssetLoader) == 8u);
static_assert(alignof(CImageAssetLoader) == 8u);
#endif

} // namespace

namespace image_asset_test_detail {

/** stb_imageのmalloc要求を専用追跡器へ渡す。 */
void* StbiMalloc(usize size) noexcept { return g_StbiAllocations.Allocate(size); }

/** stb_imageのrealloc要求を専用追跡器へ渡す。 */
void* StbiRealloc(void* pointer, usize size) noexcept { return g_StbiAllocations.Reallocate(pointer, size); }

/** stb_imageのfree要求を専用追跡器へ渡す。 */
void StbiFree(void* pointer) noexcept { g_StbiAllocations.Release(pointer); }

} // namespace image_asset_test_detail

ACS_TEST(ImageAssetTransactional, InputAndDecodedCountsAreCheckedBeforeAcsAllocation)
{
    /** decoder用intへ変換した入力byte数。 */
    int decoder_input_length = -1;
    EXPECT_FALSE(image_asset_test_detail::TryConvertDecoderInputLengthForTesting(0u, decoder_input_length));
    EXPECT_EQ(decoder_input_length, -1);
    EXPECT_TRUE(image_asset_test_detail::TryConvertDecoderInputLengthForTesting(static_cast<usize>(TNumLimits<i32>::Max()), decoder_input_length));
    EXPECT_EQ(decoder_input_length, TNumLimits<i32>::Max());
    decoder_input_length = -1;
    EXPECT_FALSE(image_asset_test_detail::TryConvertDecoderInputLengthForTesting(static_cast<usize>(TNumLimits<i32>::Max()) + 1u, decoder_input_length));
    EXPECT_EQ(decoder_input_length, -1);

    /** decode済みpixel配列のbyte数。 */
    usize byte_count = 77u;
    EXPECT_TRUE(image_asset_test_detail::TryCalculateDecodedByteCountForTesting(2, 1, 4, 4u, byte_count));
    EXPECT_EQ(byte_count, static_cast<usize>(8u));

    byte_count = 77u;
    EXPECT_FALSE(image_asset_test_detail::TryCalculateDecodedByteCountForTesting(0, 1, 4, 4u, byte_count));
    EXPECT_EQ(byte_count, static_cast<usize>(77u));
    EXPECT_FALSE(image_asset_test_detail::TryCalculateDecodedByteCountForTesting(1, -1, 4, 4u, byte_count));
    EXPECT_FALSE(image_asset_test_detail::TryCalculateDecodedByteCountForTesting(1, 1, 0, 4u, byte_count));
    EXPECT_FALSE(image_asset_test_detail::TryCalculateDecodedByteCountForTesting(1, 1, 5, 4u, byte_count));
    EXPECT_FALSE(image_asset_test_detail::TryCalculateDecodedByteCountForTesting(1, 1, 4, 0u, byte_count));
    EXPECT_FALSE(image_asset_test_detail::TryCalculateDecodedByteCountForTesting(TNumLimits<i32>::Max(), TNumLimits<i32>::Max(), 4, 16u, byte_count));
    EXPECT_EQ(byte_count, static_cast<usize>(77u));
}

ACS_TEST(ImageAssetTransactional, EmptyInputFailsWithoutDecoderAllocation)
{
    /** 空入力。 */
    const TArray<byte> bytes;

    /** 画像loader実体。 */
    CImageAssetLoader loader;
    EXPECT_TRUE(g_StbiAllocations.Reset());

    /** 空入力のload結果。 */
    auto result = loader.LoadFromBytes(FAssetId{1u}, bytes);
    EXPECT_TRUE(result.IsErr());
    if (result.IsErr()) EXPECT_EQ(result.Error().category, EErrCategory::Asset);
    EXPECT_EQ(g_StbiAllocations.AllocationCallCount(), static_cast<u64>(0u));
    EXPECT_EQ(g_StbiAllocations.OutstandingCount(), static_cast<usize>(0u));
}

ACS_TEST(ImageAssetTransactional, PngSuccessPreservesDecodedBytesAndPublicState)
{
    /** PNG fixtureの所有byte列。 */
    const TArray<byte> bytes = MakeFixtureBytes(kPngFixture);

    /** load結果へ設定するasset ID。 */
    const FAssetId expected_id{0x0102030405060708ull};

    /** 画像loader実体。 */
    CImageAssetLoader loader;
    EXPECT_EQ(loader.TypeId(), AImageAsset::StaticType());
    EXPECT_TRUE(g_StbiAllocations.Reset());

    /** PNGのload結果。 */
    auto result = loader.LoadFromBytes(expected_id, bytes);
    EXPECT_TRUE(result.IsOk());
    if (result.IsErr()) return;
    ExpectReadyImage(result.Value(), expected_id);

    /** decode結果の具体画像asset。 */
    const AImageAsset* const image = static_cast<const AImageAsset*>(result.Value().Get());
    EXPECT_EQ(image->Width(), 2u);
    EXPECT_EQ(image->Height(), 1u);
    EXPECT_EQ(image->Format(), EPixelFormat::R8G8B8A8);
    EXPECT_EQ(image->PixelByteCount(), static_cast<usize>(sizeof(kExpectedPngPixels)));
    for (usize index = 0u; index < sizeof(kExpectedPngPixels); ++index) EXPECT_EQ(image->Pixels()[index], kExpectedPngPixels[index]);
    EXPECT_EQ(g_StbiAllocations.OutstandingCount(), static_cast<usize>(0u));
    EXPECT_TRUE(g_StbiAllocations.FreeCallCount() > 0u);
    EXPECT_FALSE(g_StbiAllocations.HadUnexpectedPointer());
}

ACS_TEST(ImageAssetTransactional, HdrSuccessPreservesDecodedBytesAndPublicState)
{
    /** HDR fixtureの所有byte列。 */
    const TArray<byte> bytes = MakeFixtureBytes(kHdrFixture);

    /** load結果へ設定するasset ID。 */
    const FAssetId expected_id{0x1020304050607080ull};

    /** 画像loader実体。 */
    CImageAssetLoader loader;
    EXPECT_TRUE(g_StbiAllocations.Reset());

    /** HDRのload結果。 */
    auto result = loader.LoadFromBytes(expected_id, bytes);
    EXPECT_TRUE(result.IsOk());
    if (result.IsErr()) return;
    ExpectReadyImage(result.Value(), expected_id);

    /** decode結果の具体画像asset。 */
    const AImageAsset* const image = static_cast<const AImageAsset*>(result.Value().Get());
    EXPECT_EQ(image->Width(), 1u);
    EXPECT_EQ(image->Height(), 1u);
    EXPECT_EQ(image->Format(), EPixelFormat::R32G32B32A32_F);
    EXPECT_EQ(image->PixelByteCount(), static_cast<usize>(sizeof(kExpectedHdrPixels)));

    /** float RGBAとして解釈したdecode結果。 */
    const f32* const decoded = reinterpret_cast<const f32*>(image->Pixels());
    for (usize index = 0u; index < sizeof(kExpectedHdrPixels) / sizeof(kExpectedHdrPixels[0]); ++index) EXPECT_EQ(decoded[index], kExpectedHdrPixels[index]);
    EXPECT_EQ(g_StbiAllocations.OutstandingCount(), static_cast<usize>(0u));
    EXPECT_TRUE(g_StbiAllocations.FreeCallCount() > 0u);
    EXPECT_FALSE(g_StbiAllocations.HadUnexpectedPointer());
}

ACS_TEST(ImageAssetTransactional, AllPngAcsAllocationFailuresReturnMemoryErrorWithoutLeak)
{
    /** PNG fixtureの所有byte列。 */
    const TArray<byte> bytes = MakeFixtureBytes(kPngFixture);
    ExpectAllocationMatrix(bytes, EPixelFormat::R8G8B8A8);
}

ACS_TEST(ImageAssetTransactional, AllHdrAcsAllocationFailuresReturnMemoryErrorWithoutLeak)
{
    /** HDR fixtureの所有byte列。 */
    const TArray<byte> bytes = MakeFixtureBytes(kHdrFixture);
    ExpectAllocationMatrix(bytes, EPixelFormat::R32G32B32A32_F);
}

} // namespace acs

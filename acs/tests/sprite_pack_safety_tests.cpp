// SPDX-License-Identifier: Apache-2.0
// 検査付き sprite atlas JSON 境界テスト。
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/SpritePack.h"
#include "memory/SystemAllocator.h"

#include <cstring>

using namespace acs;
using namespace acs::game;

namespace {

class CSpritePackFailAllocator final : public IAllocator {
public:
    void* Alloc(usize, usize, FSourceLoc) noexcept override { return nullptr; }
    void Free(void*) noexcept override {}
};

class CSpritePackSwitchAllocator final : public IAllocator {
public:
    explicit CSpritePackSwitchAllocator(IAllocator& backing) noexcept
        : m_Backing(&backing) {}

    void SetFailing(bool failing) noexcept { m_Failing = failing; }

    void* Alloc(
        usize size, usize alignment, FSourceLoc location) noexcept override {
        return m_Failing
            ? nullptr
            : m_Backing->Alloc(size, alignment, location);
    }
    void Free(void* pointer) noexcept override { m_Backing->Free(pointer); }

private:
    IAllocator* m_Backing = nullptr;
    bool m_Failing = false;
};

bool TextEquals(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return a == b;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

const char* kValidAtlas =
    R"({"frames":{"idle":{"frame":{"x":0,"y":0,"w":16,"h":16},)"
    R"("pivot":{"x":0.5,"y":1.0}}},)"
    R"("meta":{"image":"hero.png","size":{"w":64,"h":32}}})";

} // namespace

ACS_TEST(SpritePackSafety, CheckedHashLoadOwnsNamesAndMetadata)
{
    FSpritePack pack;
    const FSpritePackLoadResult result =
        pack.TryLoadAtlasJson(kValidAtlas, std::strlen(kValidAtlas));
    EXPECT_TRUE(result.Succeeded());
    EXPECT_EQ(pack.FrameCount(), 1u);
    EXPECT_TRUE(TextEquals(pack.Info().atlas_texture_path, "hero.png"));
    EXPECT_EQ(pack.Info().atlas_width, 64u);
    EXPECT_EQ(pack.Info().atlas_height, 32u);

    const FSpriteFrame* frame = pack.FindFrame("idle");
    EXPECT_TRUE(frame != nullptr);
    if (frame != nullptr) {
        EXPECT_EQ(frame->w, 16u);
        EXPECT_NEAR(frame->pivot_x, 0.5f, 0.000001f);
        EXPECT_NEAR(frame->pivot_y, 1.0f, 0.000001f);
    }
}

ACS_TEST(SpritePackSafety, FailurePreservesFramesNamesImageAndPublishedPointers)
{
    FSpritePack pack;
    EXPECT_TRUE(
        pack.TryLoadAtlasJson(kValidAtlas, std::strlen(kValidAtlas)).Succeeded());
    u32 old_count = 0u;
    const FSpriteFrame* const old_frames = pack.AllFrames(old_count);
    const FSpriteFrame* const old_idle = pack.FindFrame("idle");
    const char* const old_name = old_idle != nullptr ? old_idle->name : nullptr;
    const char* const old_image = pack.Info().atlas_texture_path;

    const char* duplicate =
        R"({"frames":[)"
        R"({"filename":"same","frame":{"x":0,"y":0,"w":8,"h":8}},)"
        R"({"filename":"same","frame":{"x":8,"y":0,"w":8,"h":8}}],)"
        R"("meta":{"image":"replacement.png","size":{"w":16,"h":8}}})";
    const FSpritePackLoadResult result =
        pack.TryLoadAtlasJson(duplicate, std::strlen(duplicate));

    EXPECT_EQ(result.Error, ESpritePackLoadError::DuplicateFrameName);
    EXPECT_EQ(pack.FrameCount(), old_count);
    u32 current_count = 0u;
    EXPECT_TRUE(pack.AllFrames(current_count) == old_frames);
    EXPECT_TRUE(pack.FindFrame("idle") == old_idle);
    EXPECT_TRUE(old_idle != nullptr && old_idle->name == old_name);
    EXPECT_TRUE(pack.Info().atlas_texture_path == old_image);
    EXPECT_TRUE(TextEquals(pack.Info().atlas_texture_path, "hero.png"));
}

ACS_TEST(SpritePackSafety, StrictSchemaRejectsFractionalRectOverflowAndInvalidPivot)
{
    FSpritePack pack;
    const char* fractional =
        R"({"frames":{"bad":{"frame":{"x":0.5,"y":0,"w":8,"h":8}}},)"
        R"("meta":{"image":"a.png","size":{"w":16,"h":16}}})";
    EXPECT_EQ(
        pack.TryLoadAtlasJson(fractional, std::strlen(fractional)).Error,
        ESpritePackLoadError::InvalidInteger);

    const char* outside =
        R"({"frames":{"bad":{"frame":{"x":15,"y":0,"w":2,"h":8}}},)"
        R"("meta":{"image":"a.png","size":{"w":16,"h":16}}})";
    EXPECT_EQ(
        pack.TryLoadAtlasJson(outside, std::strlen(outside)).Error,
        ESpritePackLoadError::InvalidFrameRect);

    const char* pivot =
        R"({"frames":{"bad":{"frame":{"x":0,"y":0,"w":8,"h":8},)"
        R"("pivot":{"x":-0.1,"y":0.5}}},)"
        R"("meta":{"image":"a.png","size":{"w":16,"h":16}}})";
    EXPECT_EQ(
        pack.TryLoadAtlasJson(pivot, std::strlen(pivot)).Error,
        ESpritePackLoadError::InvalidPivot);

    const char* non_finite =
        R"({"frames":{"bad":{"frame":{"x":0,"y":0,"w":8,"h":8},)"
        R"("pivot":{"x":1e999,"y":0.5}}},)"
        R"("meta":{"image":"a.png","size":{"w":16,"h":16}}})";
    EXPECT_EQ(
        pack.TryLoadAtlasJson(non_finite, std::strlen(non_finite)).Error,
        ESpritePackLoadError::NonFiniteNumber);
}

ACS_TEST(SpritePackSafety, DuplicateJsonMembersAndMalformedInputAreRejected)
{
    FSpritePack pack;
    const char* duplicate_member =
        R"({"frames":{},"frames":[],)"
        R"("meta":{"image":"a.png","size":{"w":16,"h":16}}})";
    EXPECT_EQ(
        pack.TryLoadAtlasJson(
            duplicate_member, std::strlen(duplicate_member)).Error,
        ESpritePackLoadError::DuplicateMember);

    const char embedded[] = {
        '{', '"', 'f', 'r', 'a', 'm', 'e', 's', '"', ':', '{', '}',
        ',', '\0', '"', 'm', 'e', 't', 'a', '"', ':', '{', '}', '}'
    };
    EXPECT_EQ(
        pack.TryLoadAtlasJson(embedded, sizeof(embedded)).Error,
        ESpritePackLoadError::EmbeddedNul);

    const char* escaped_nul =
        R"({"frames":{"bad\u0000name":{"frame":{"x":0,"y":0,"w":8,"h":8}}},)"
        R"("meta":{"image":"a.png","size":{"w":8,"h":8}}})";
    EXPECT_EQ(
        pack.TryLoadAtlasJson(
            escaped_nul, std::strlen(escaped_nul)).Error,
        ESpritePackLoadError::EmbeddedNul);

    EXPECT_EQ(
        pack.TryLoadAtlasJson(
            "{}", FSpritePack::kMaxAtlasJsonBytes + 1u).Error,
        ESpritePackLoadError::InputTooLarge);

    const char* truncated = R"({"frames":[)";
    const FSpritePackLoadResult truncated_result =
        pack.TryLoadAtlasJson(truncated, std::strlen(truncated));
    EXPECT_EQ(truncated_result.Error, ESpritePackLoadError::JsonSyntaxError);
    EXPECT_TRUE(truncated_result.JsonSubcode != 0u);
}

ACS_TEST(SpritePackSafety, LimitsAndAllocationFailureAreReportedWithoutCommit)
{
    FSpritePack pack;
    char deep[FSpritePack::kMaxJsonDepth + 3u]{};
    for (u32 i = 0u; i <= FSpritePack::kMaxJsonDepth; ++i) deep[i] = '[';
    deep[FSpritePack::kMaxJsonDepth + 1u] = '0';
    deep[FSpritePack::kMaxJsonDepth + 2u] = ']';
    EXPECT_EQ(
        pack.TryLoadAtlasJson(deep, sizeof(deep)).Error,
        ESpritePackLoadError::JsonDepthExceeded);

    CSpritePackFailAllocator allocator;
    FSpritePack failing_pack(allocator);
    const FSpritePackLoadResult oom =
        failing_pack.TryLoadAtlasJson(kValidAtlas, std::strlen(kValidAtlas));
    EXPECT_EQ(oom.Error, ESpritePackLoadError::AllocationFailure);
    EXPECT_EQ(failing_pack.FrameCount(), 0u);
    EXPECT_TRUE(failing_pack.Info().atlas_texture_path == nullptr);
}

ACS_TEST(SpritePackSafety, AllocationFailurePreservesExistingStorageAndPointers)
{
    CSystemAllocator backing;
    CSpritePackSwitchAllocator allocator(backing);
    FSpritePack pack(allocator);
    EXPECT_TRUE(
        pack.TryLoadAtlasJson(kValidAtlas, std::strlen(kValidAtlas)).Succeeded());

    u32 old_count = 0u;
    const FSpriteFrame* const old_frames = pack.AllFrames(old_count);
    const FSpriteFrame* const old_idle = pack.FindFrame("idle");
    const char* const old_name = old_idle != nullptr ? old_idle->name : nullptr;
    const char* const old_image = pack.Info().atlas_texture_path;

    allocator.SetFailing(true);
    const FSpritePackLoadResult result =
        pack.TryLoadAtlasJson(kValidAtlas, std::strlen(kValidAtlas));
    EXPECT_EQ(result.Error, ESpritePackLoadError::AllocationFailure);
    u32 current_count = 0u;
    EXPECT_TRUE(pack.AllFrames(current_count) == old_frames);
    EXPECT_EQ(current_count, old_count);
    EXPECT_TRUE(pack.FindFrame("idle") == old_idle);
    EXPECT_TRUE(old_idle != nullptr && old_idle->name == old_name);
    EXPECT_TRUE(pack.Info().atlas_texture_path == old_image);
    allocator.SetFailing(false);
}

ACS_TEST(SpritePackSafety, UnknownExporterExtensionsRemainForwardCompatible)
{
    FSpritePack pack;
    const char* extended =
        R"({"futureRoot":true,)"
        R"("frames":[{"filename":"run","rotated":false,)"
        R"("frame":{"x":0,"y":0,"w":8,"h":8,"futureRect":3}}],)"
        R"("meta":{"image":"a.png","size":{"w":8,"h":8},)"
        R"("futureMeta":{"version":2}}})";
    EXPECT_TRUE(
        pack.TryLoadAtlasJson(extended, std::strlen(extended)).Succeeded());
    EXPECT_TRUE(pack.HasFrame("run"));
}

ACS_TEST(SpritePackSafety, LegacyApiCarriesCheckedSubcode)
{
    FSpritePack pack;
    const TResult<void> result = pack.LoadAtlasJson(nullptr, 0u);
    EXPECT_TRUE(result.IsErr());
    EXPECT_EQ(
        result.Error().subcode,
        static_cast<u16>(ESpritePackLoadError::NullInput));
}

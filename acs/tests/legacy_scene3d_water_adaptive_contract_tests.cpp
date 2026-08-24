// SPDX-License-Identifier: Apache-2.0

#include "test/Test.h"
#include "test/Expect.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

/** worktree内のEngine sourceを読み、Legacy統合契約を実装単位で固定する。 */
std::string ReadEngineSource(const char* relative_path)
{
    const std::filesystem::path test_file{__FILE__};
    const std::filesystem::path source_path =
        test_file.parent_path().parent_path() /
        std::filesystem::path{relative_path};
    std::ifstream stream(source_path, std::ios::binary);
    if (!stream) {
        stream.open(
            std::filesystem::path{"acs"} /
                std::filesystem::path{relative_path},
            std::ios::binary);
    }
    return std::string{
        std::istreambuf_iterator<char>{stream},
        std::istreambuf_iterator<char>{}};
}

/** 指定signatureから対応する関数本体だけを取り出す。 */
std::string ExtractFunction(
    const std::string& source, const char* signature)
{
    const std::size_t signature_begin = source.find(signature);
    if (signature_begin == std::string::npos) return {};
    const std::size_t body_begin = source.find('{', signature_begin);
    if (body_begin == std::string::npos) return {};

    unsigned int depth = 0u;
    for (std::size_t index = body_begin; index < source.size(); ++index) {
        if (source[index] == '{') {
            ++depth;
        } else if (source[index] == '}') {
            if (depth == 0u) return {};
            --depth;
            if (depth == 0u) {
                return source.substr(
                    signature_begin, index - signature_begin + 1u);
            }
        }
    }
    return {};
}

/** 対象文字列が現れる回数を返す。 */
std::size_t CountOccurrences(
    const std::string& source, const char* token)
{
    const std::string needle{token};
    if (needle.empty()) return 0u;
    std::size_t count = 0u;
    std::size_t offset = 0u;
    while ((offset = source.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

} // namespace

ACS_TEST(LegacyScene3DWaterAdaptive,
         PlaneUsesAdaptiveGridAndAuthoredMeshKeepsDrawMesh) {
    const std::string source = ReadEngineSource(
        "src/gameframework/LegacyScene3DAdapter.cpp");
    EXPECT_TRUE(!source.empty());
    if (source.empty()) return;

    const std::string collect = ExtractFunction(
        source, "u32 FLegacyScene3DAdapter::CollectWaterDraws(");
    const std::string draw = ExtractFunction(
        source, "void FLegacyScene3DAdapter::DrawWaterScene(");
    EXPECT_TRUE(!collect.empty());
    EXPECT_TRUE(!draw.empty());

    EXPECT_TRUE(collect.find(
        "m_CustomMeshes[index].Component == nullptr") !=
        std::string::npos);
    EXPECT_TRUE(collect.find(
        "mesh->Primitive() == EMeshPrimitive3D::Plane") !=
        std::string::npos);
    EXPECT_TRUE(collect.find(
        "? adaptive_plane : GpuMeshFor(*mesh)") !=
        std::string::npos);

    const std::size_t adaptive_draw = draw.find(
        "m_Water.DrawAdaptivePlane(");
    const std::size_t authored_draw = draw.find(
        "m_Water.DrawMesh(");
    EXPECT_TRUE(adaptive_draw != std::string::npos);
    EXPECT_TRUE(authored_draw != std::string::npos);
    EXPECT_TRUE(adaptive_draw < authored_draw);
    EXPECT_TRUE(draw.find(
        "draw.Mesh->Primitive() == EMeshPrimitive3D::Plane") !=
        std::string::npos);
    EXPECT_TRUE(draw.find("draw.Gpu == adaptive_plane") !=
        std::string::npos);
}

ACS_TEST(LegacyScene3DWaterAdaptive,
         OptionalGridFailureKeepsExistingPlaneFallback) {
    const std::string source = ReadEngineSource(
        "src/gameframework/LegacyScene3DAdapter.cpp");
    EXPECT_TRUE(!source.empty());
    if (source.empty()) return;

    const std::string upload = ExtractFunction(
        source, "bool FLegacyScene3DAdapter::UploadGraphMeshes(");
    const std::string mesh_lookup = ExtractFunction(
        source, "const FGpuMesh* FLegacyScene3DAdapter::GpuMeshFor(");
    EXPECT_TRUE(!upload.empty());
    EXPECT_TRUE(!mesh_lookup.empty());

    const std::size_t create = upload.find(
        "FWaterSurface3D::CreateAdaptivePlaneMesh(");
    const std::size_t traversal = upload.find("TArray<ANode*> stack;");
    EXPECT_TRUE(create != std::string::npos);
    EXPECT_TRUE(traversal != std::string::npos);
    EXPECT_TRUE(traversal < create);
    if (create != std::string::npos
        && traversal != std::string::npos
        && traversal < create) {
        const std::string optional_section =
            upload.substr(create);
        EXPECT_TRUE(optional_section.find("adaptive_upload.IsErr()") !=
            std::string::npos);
        EXPECT_TRUE(optional_section.find(
            "!m_CustomMeshes.TryPushBack(Move(adaptive_plane))") !=
            std::string::npos);
        EXPECT_TRUE(optional_section.find("return false;") ==
            std::string::npos);
    }
    EXPECT_TRUE(mesh_lookup.find(
        "case EMeshPrimitive3D::Plane: return &m_Plane;") !=
        std::string::npos);
}

ACS_TEST(LegacyScene3DWaterAdaptive,
         ReloadAndReleaseOwnExactlyOneGridSentinel) {
    const std::string source = ReadEngineSource(
        "src/gameframework/LegacyScene3DAdapter.cpp");
    const std::string header = ReadEngineSource(
        "src/gameframework/LegacyScene3DAdapter.h");
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!header.empty());
    if (source.empty() || header.empty()) return;

    const std::string upload = ExtractFunction(
        source, "bool FLegacyScene3DAdapter::UploadGraphMeshes(");
    const std::string release = ExtractFunction(
        source, "void FLegacyScene3DAdapter::ReleaseGpu(");
    const std::string load_text = ExtractFunction(
        source, "FScene3DLoadResult FLegacyScene3DAdapter::LoadText(");
    const std::string fallback = ExtractFunction(
        source, "void FLegacyScene3DAdapter::DrawWaterFallback(");
    EXPECT_TRUE(CountOccurrences(
        upload, "FWaterSurface3D::CreateAdaptivePlaneMesh(") == 1u);
    EXPECT_TRUE(upload.find("m_CustomMeshes.Clear();") !=
        std::string::npos);
    EXPECT_TRUE(release.find("m_CustomMeshes.Clear();") !=
        std::string::npos);
    EXPECT_TRUE(load_text.find("DrainAndReleaseGpu();") !=
        std::string::npos);
    EXPECT_TRUE(fallback.find("shader.DrawMesh(") !=
        std::string::npos);
    EXPECT_TRUE(fallback.find("*draw.Gpu") !=
        std::string::npos);
    EXPECT_TRUE(fallback.find("DrawAdaptivePlane(") ==
        std::string::npos);
    EXPECT_TRUE(header.find("m_AdaptiveWater") ==
        std::string::npos);
}

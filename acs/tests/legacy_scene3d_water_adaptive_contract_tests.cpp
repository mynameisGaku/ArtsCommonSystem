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
    /** 実行場所に依存しない基準となるtest sourceの場所。 */
    const std::filesystem::path test_file{__FILE__};
    /** test sourceから解決したEngine sourceの場所。 */
    const std::filesystem::path source_path = test_file.parent_path().parent_path() / std::filesystem::path{relative_path};
    /** 対象sourceをbinaryのまま読むstream。 */
    std::ifstream stream(source_path, std::ios::binary);
    if (!stream) {
        stream.open(std::filesystem::path{"acs"} / std::filesystem::path{relative_path}, std::ios::binary);
    }
    return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

/** 指定signatureから対応する関数本体だけを取り出す。 */
std::string ExtractFunction(const std::string& source, const char* signature)
{
    /** 関数signatureの開始位置。 */
    const std::size_t signature_begin = source.find(signature);
    if (signature_begin == std::string::npos) return {};
    /** 関数本体の開始位置。 */
    const std::size_t body_begin = source.find('{', signature_begin);
    if (body_begin == std::string::npos) return {};

    /** 入れ子になった関数本体の波括弧深度。 */
    unsigned int depth = 0u;
    for (std::size_t index = body_begin; index < source.size(); ++index) {
        if (source[index] == '{') {
            ++depth;
        } else if (source[index] == '}') {
            if (depth == 0u) return {};
            --depth;
            if (depth == 0u) return source.substr(signature_begin, index - signature_begin + 1u);
        }
    }
    return {};
}

/** 対象文字列が現れる回数を返す。 */
std::size_t CountOccurrences(const std::string& source, const char* token)
{
    /** 検索する固定文字列。 */
    const std::string needle{token};
    if (needle.empty()) return 0u;
    /** 検出した出現回数。 */
    std::size_t count = 0u;
    /** 次に検索を始める文字位置。 */
    std::size_t offset = 0u;
    while ((offset = source.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

} // namespace

ACS_TEST(LegacyScene3DWaterAdaptive, PlaneUsesAdaptiveGridAndAuthoredMeshKeepsDrawMesh) {
    /** 契約を検査するLegacy adapter source。 */
    const std::string source = ReadEngineSource("src/gameframework/LegacyScene3DAdapter.cpp");
    EXPECT_TRUE(!source.empty());
    if (source.empty()) return;

    /** 水面draw収集だけを含む関数本体。 */
    const std::string collect = ExtractFunction(source, "u32 ALegacyScene3DAdapter::CollectWaterDraws(");
    /** 水面rendererへのdraw分岐だけを含む関数本体。 */
    const std::string draw = ExtractFunction(source, "void ALegacyScene3DAdapter::DrawWaterScene(");
    EXPECT_TRUE(!collect.empty());
    EXPECT_TRUE(!draw.empty());

    EXPECT_TRUE(collect.find("m_CustomMeshes[index].Component == nullptr") != std::string::npos);
    EXPECT_TRUE(collect.find("mesh->Primitive() == EMeshPrimitive3D::Plane") != std::string::npos);
    EXPECT_TRUE(collect.find("use_adaptive_plane ? adaptive_plane : GpuMeshFor(*mesh)") != std::string::npos);

    /** 標準Planeを適応格子で描く分岐位置。 */
    const std::size_t adaptive_draw = draw.find("m_Water.DrawAdaptivePlane(");
    /** authoring済みMeshを従来経路で描く分岐位置。 */
    const std::size_t authored_draw = draw.find("m_Water.DrawMesh(");
    EXPECT_TRUE(adaptive_draw != std::string::npos);
    EXPECT_TRUE(authored_draw != std::string::npos);
    EXPECT_TRUE(adaptive_draw < authored_draw);
    EXPECT_TRUE(draw.find("draw.Mesh->Primitive() == EMeshPrimitive3D::Plane") != std::string::npos);
    EXPECT_TRUE(draw.find("draw.Gpu == adaptive_plane") != std::string::npos);
}

ACS_TEST(LegacyScene3DWaterAdaptive, OptionalGridFailureKeepsExistingPlaneFallback) {
    /** 契約を検査するLegacy adapter source。 */
    const std::string source = ReadEngineSource("src/gameframework/LegacyScene3DAdapter.cpp");
    EXPECT_TRUE(!source.empty());
    if (source.empty()) return;

    /** GPU meshの作成と保持を行う関数本体。 */
    const std::string upload = ExtractFunction(source, "bool ALegacyScene3DAdapter::UploadGraphMeshes(");
    /** 適応格子がない場合の既存Plane lookupを含む関数本体。 */
    const std::string mesh_lookup = ExtractFunction(source, "const FGpuMesh* ALegacyScene3DAdapter::GpuMeshFor(");
    EXPECT_TRUE(!upload.empty());
    EXPECT_TRUE(!mesh_lookup.empty());

    /** 任意の適応格子を作り始める位置。 */
    const std::size_t create = upload.find("CWaterSurface3D::CreateAdaptivePlaneMesh(");
    /** 必須の任意Mesh traversalを始める位置。 */
    const std::size_t traversal = upload.find("TArray<ANode*> stack;");
    EXPECT_TRUE(create != std::string::npos);
    EXPECT_TRUE(traversal != std::string::npos);
    EXPECT_TRUE(traversal < create);
    if (create != std::string::npos && traversal != std::string::npos && traversal < create) {
        /** 失敗をscene初期化へ伝播させてはいけない任意処理。 */
        const std::string optional_section = upload.substr(create);
        EXPECT_TRUE(optional_section.find("adaptive_upload.IsErr()") != std::string::npos);
        EXPECT_TRUE(optional_section.find("!m_CustomMeshes.TryAdd(Move(adaptive_plane))") != std::string::npos);
        EXPECT_TRUE(optional_section.find("return false;") == std::string::npos);
    }
    EXPECT_TRUE(mesh_lookup.find("case EMeshPrimitive3D::Plane: return &m_Plane;") != std::string::npos);
}

ACS_TEST(LegacyScene3DWaterAdaptive, ReloadAndReleaseOwnExactlyOneGridSentinel) {
    /** 契約を検査するLegacy adapter source。 */
    const std::string source = ReadEngineSource("src/gameframework/LegacyScene3DAdapter.cpp");
    /** class layoutを検査するLegacy adapter header。 */
    const std::string header = ReadEngineSource("src/gameframework/LegacyScene3DAdapter.h");
    EXPECT_TRUE(!source.empty());
    EXPECT_TRUE(!header.empty());
    if (source.empty() || header.empty()) return;

    /** GPU meshの作成と保持を行う関数本体。 */
    const std::string upload = ExtractFunction(source, "bool ALegacyScene3DAdapter::UploadGraphMeshes(");
    /** GPU meshの解放を行う関数本体。 */
    const std::string release = ExtractFunction(source, "void ALegacyScene3DAdapter::ReleaseGpu(");
    /** reload前のGPU解放を行う関数本体。 */
    const std::string load_text = ExtractFunction(source, "FScene3DLoadResult ALegacyScene3DAdapter::LoadText(");
    /** 初期化失敗時の既存PBR描画を行う関数本体。 */
    const std::string fallback = ExtractFunction(source, "void ALegacyScene3DAdapter::DrawWaterFallback(");
    EXPECT_TRUE(CountOccurrences(upload, "CWaterSurface3D::CreateAdaptivePlaneMesh(") == 1u);
    EXPECT_TRUE(upload.find("m_CustomMeshes.Reset();") != std::string::npos);
    EXPECT_TRUE(release.find("m_CustomMeshes.Reset();") != std::string::npos);
    EXPECT_TRUE(load_text.find("DrainAndReleaseGpu();") != std::string::npos);
    EXPECT_TRUE(fallback.find("shader.DrawMesh(") != std::string::npos);
    EXPECT_TRUE(fallback.find("*draw.Gpu") != std::string::npos);
    EXPECT_TRUE(fallback.find("DrawAdaptivePlane(") == std::string::npos);
    EXPECT_TRUE(header.find("m_AdaptiveWater") == std::string::npos);
}

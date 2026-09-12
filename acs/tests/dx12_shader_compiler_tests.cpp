// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"

#if WITH_RENDER_DX12_RAW
#include "render/Dx12/Dx12Shader.h"
#include "threading/Atomic.h"
#include "threading/Thread.h"

using namespace acs;

namespace {

/** 頂点段階の最小入力。外部ファイルや描画デバイスを必要としない。 */
constexpr char kVertexSource[] = "float4 main(uint id : SV_VertexID) : SV_Position { return float4(id,0,0,1); }";
/** 画素段階の最小入力。 */
constexpr char kPixelSource[] = "float4 main() : SV_Target { return float4(1,0,0,1); }";
/** 計算段階の最小入力。 */
constexpr char kComputeSource[] = "[numthreads(1,1,1)] void main() {}";

/** 境界確認済みの四バイトを、格納先の整列に依存せず読み取る。 */
u32 ReadShaderWord_Internal(const byte* bytes) noexcept
{
    return static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8u) | (static_cast<u32>(bytes[2]) << 16u) | (static_cast<u32>(bytes[3]) << 24u);
}

/** 実際に返された命令コンテナーの部品表を検査し、DXIL部品の有無を返す。 */
bool HasDxilPart_Internal(const IRhiShader& shader) noexcept
{
    /** 作成器が所有する命令の先頭と長さ。 */
    const byte* bytes = shader.Bytecode();
    const usize size = shader.BytecodeSize();
    if (!bytes || size < 32u || ReadShaderWord_Internal(bytes) != 0x43425844u) return false;
    if (ReadShaderWord_Internal(bytes + 24u) != size) return false;
    /** コンテナー内の部品数。乗算前に表の残り長さで制限する。 */
    const u32 count = ReadShaderWord_Internal(bytes + 28u);
    if (count > (size - 32u) / 4u) return false;
    for (u32 index = 0u; index < count; ++index) {
        /** 各部品の位置と本文長。 */
        const usize offset = ReadShaderWord_Internal(bytes + 32u + index * 4u);
        if (offset > size - 8u) return false;
        const usize length = ReadShaderWord_Internal(bytes + offset + 4u);
        if (length > size - offset - 8u) return false;
        if (ReadShaderWord_Internal(bytes + offset) == 0x4c495844u && length != 0u) return true;
    }
    return false;
}

/** 開始合図と、担当スレッドだけが書く完了数。全員のJoin後に読み取る。 */
struct FParallelDxcProbeInput {
    /** 全スレッドへ同時に作成開始を伝える、呼出し元所有の合図。 */
    TAtomic<u32>* start = nullptr;
    /** 正しい命令を受け取った回数。書き手は担当一人だけ。 */
    u32 completed = 0u;
};

/** 別々のCOM所有者を同じスレッド内で作成・使用・破棄し、結果を専用領域へ残す。 */
void CompileParallelDxcProbe_Internal(void* user)
{
    /** この担当だけが書き込む入力と出力。 */
    auto& input = *static_cast<FParallelDxcProbeInput*>(user);
    while (input.start->Load() == 0u) Yield();
    for (u32 index = 0u; index < 8u; ++index) {
        /** 同じソースでも作成器のCOMは他スレッドと共有しない。 */
        FShaderDesc desc{};
        desc.stage = EShaderStage::Compute;
        desc.target = "cs_6_0";
        desc.hlsl_source = kComputeSource;
        FDx12Shader shader;
        if (shader.Init(desc).IsOk() && HasDxilPart_Internal(shader)) ++input.completed;
    }
}

} // namespace

ACS_TEST(Render, ShaderModelSixProducesOwnedBytecodeWithoutDevice)
{
    /** 三段階それぞれの入力と、明示したコンパイル形式。 */
    const char* sources[] = {kVertexSource, kPixelSource, kComputeSource};
    const char* targets[] = {"vs_6_0", "ps_6_0", "cs_6_0"};
    const EShaderStage stages[] = {EShaderStage::Vertex, EShaderStage::Pixel, EShaderStage::Compute};
    for (u32 index = 0u; index < 3u; ++index) {
        /** デバイスを作らず、通常C++から直接利用する記述と所有者。 */
        FShaderDesc desc{};
        desc.stage = stages[index];
        desc.hlsl_source = sources[index];
        desc.target = targets[index];
        FDx12Shader shader;
        const FHrResult result = shader.Init(desc);
        EXPECT_TRUE(result.IsOk());
        if (result.IsErr()) continue;
        EXPECT_EQ(shader.Stage(), stages[index]);
        EXPECT_TRUE(HasDxilPart_Internal(shader));
    }
}

ACS_TEST(Render, LegacyShaderTargetsRemainAvailable)
{
    /** 未指定と明示5.0／5.1の互換経路。 */
    const char* targets[] = {nullptr, "ps_5_0", "ps_5_1"};
    for (u32 index = 0u; index < 3u; ++index) {
        FShaderDesc desc{};
        desc.stage = EShaderStage::Pixel;
        desc.hlsl_source = kPixelSource;
        desc.target = targets[index];
        FDx12Shader shader;
        EXPECT_TRUE(shader.Init(desc).IsOk());
        EXPECT_TRUE(shader.BytecodeSize() != 0u);
        EXPECT_FALSE(HasDxilPart_Internal(shader));
    }
}

ACS_TEST(Render, ExplicitShaderTargetMustMatchDeclaredStage)
{
    /** 記述だけ頂点へ偽装した画素ソース。5.1でも不整合を拒否する。 */
    FShaderDesc desc{};
    desc.stage = EShaderStage::Vertex;
    desc.hlsl_source = kPixelSource;
    desc.target = "ps_5_1";
    FDx12Shader shader;
    EXPECT_TRUE(shader.Init(desc).IsErr());
    EXPECT_EQ(shader.BytecodeSize(), 0u);
    desc.target = "ps_6_0";
    EXPECT_TRUE(shader.Init(desc).IsErr());
    EXPECT_EQ(shader.BytecodeSize(), 0u);
}

ACS_TEST(Render, InvalidShaderDescriptionLeavesNoBytecode)
{
    /** 空文字・段階不一致・末尾余分・未対応版を含む不正入力。 */
    const char* targets[] = {"", "cs_6_0x", "cs_7_0", "ps_6_0", "cs_6_"};
    for (u32 index = 0u; index < 5u; ++index) {
        FShaderDesc desc{};
        desc.stage = EShaderStage::Compute;
        desc.hlsl_source = kComputeSource;
        desc.target = targets[index];
        FDx12Shader shader;
        EXPECT_TRUE(shader.Init(desc).IsErr());
        EXPECT_EQ(shader.BytecodeSize(), 0u);
    }
}

ACS_TEST(Render, ShaderCompileFailureClearsPreviousBytecode)
{
    /** 以前の成功を新しい失敗で残さないことを確かめる所有者。 */
    FDx12Shader shader;
    FShaderDesc desc{};
    desc.stage = EShaderStage::Pixel;
    desc.hlsl_source = kPixelSource;
    EXPECT_TRUE(shader.Init(desc).IsOk());
    desc.target = "ps_6_0";
    desc.hlsl_source = "invalid hlsl";
    EXPECT_TRUE(shader.Init(desc).IsErr());
    EXPECT_TRUE(shader.Bytecode() == nullptr);
    EXPECT_EQ(shader.BytecodeSize(), 0u);
}

ACS_TEST(Render, ShaderModelSixRejectsExternalIncludes)
{
    /** 呼出し元が与えていないファイルを暗黙に読み込ませない入力。 */
    FShaderDesc desc{};
    desc.stage = EShaderStage::Compute;
    desc.target = "cs_6_0";
    desc.hlsl_source = "#include \"acs-unprovided-shader-input.hlsl\"\n[numthreads(1,1,1)] void main() {}";
    FDx12Shader shader;
    EXPECT_TRUE(shader.Init(desc).IsErr());
    EXPECT_EQ(shader.BytecodeSize(), 0u);
}

ACS_TEST(Render, LaterShaderCompilationDoesNotInvalidateOwnedBytes)
{
    /** コンパイラーの一時所有者が終了しても残る最初の命令。 */
    FShaderDesc desc{};
    desc.stage = EShaderStage::Compute;
    desc.target = "cs_6_0";
    desc.hlsl_source = kComputeSource;
    FDx12Shader retained;
    const FHrResult result = retained.Init(desc);
    EXPECT_TRUE(result.IsOk());
    if (result.IsErr()) return;
    const byte* original_bytes = retained.Bytecode();
    const usize original_size = retained.BytecodeSize();
    for (u32 index = 0u; index < 8u; ++index) {
        /** 各反復で作成と破棄を完了する別所有者。 */
        FDx12Shader temporary;
        EXPECT_TRUE(temporary.Init(desc).IsOk());
        EXPECT_TRUE(HasDxilPart_Internal(temporary));
    }
    EXPECT_TRUE(retained.Bytecode() == original_bytes);
    EXPECT_EQ(retained.BytecodeSize(), original_size);
    EXPECT_TRUE(HasDxilPart_Internal(retained));
}

ACS_TEST(Render, DxcRuntimeRemainsLoadedBetweenCompilations)
{
    /** 公式の寿命条件を確認する最小の作成。描画デバイスは不要。 */
    FShaderDesc desc{};
    desc.stage = EShaderStage::Compute;
    desc.target = "cs_6_0";
    desc.hlsl_source = kComputeSource;
    {
        /** 命令所有者の寿命もここで終了する。 */
        FDx12Shader shader;
        EXPECT_TRUE(shader.Init(desc).IsOk());
    }
    // DXC内部の状態を作成ごとに破棄しない。参照数を増やさない照会だけで確認する。
    EXPECT_TRUE(::GetModuleHandleW(L"dxcompiler.dll") != nullptr);
    EXPECT_TRUE(::GetModuleHandleW(L"dxil.dll") != nullptr);
}

ACS_TEST(Render, ParallelDxcCompilationsKeepIndependentOwners)
{
    /** 起動失敗があっても全員を解放し、Joinしてから入力領域を破棄する。 */
    TAtomic<u32> start{0u};
    /** 固定八人分のスレッド所有者。STLや新しい実行基盤は使用しない。 */
    FThread threads[8];
    /** 各担当の独立した結果領域。 */
    FParallelDxcProbeInput inputs[8];
    for (u32 index = 0u; index < 8u; ++index) {
        inputs[index].start = &start;
        /** 失敗時に途中でreturnせず、既に起動した全員の終了を保証する。 */
        auto spawned = FThread::Spawn(CompileParallelDxcProbe_Internal, &inputs[index]);
        EXPECT_TRUE(spawned.IsOk());
        if (spawned.IsOk()) threads[index] = Move(spawned.Value());
    }
    start.Store(1u);
    for (u32 index = 0u; index < 8u; ++index) threads[index].Join();
    for (u32 index = 0u; index < 8u; ++index) EXPECT_EQ(inputs[index].completed, 8u);
}
#endif

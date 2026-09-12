// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"

#if WITH_RENDER_DX12_RAW
#include "render/Dx12/Dx12ShaderModelInternal.h"
#include "render/IRhiShader.h"
#include <dxgi.h>

namespace {

using namespace acs;

// GPUで実行しない合成コンテナ。公式のヘッダー配置だけを持ち、LLVM命令や署名は持たない。
// 配置根拠: microsoft/DirectXShaderCompiler の include/dxc/DxilContainer/DxilContainer.h。
class AModelTestShader final : public IRhiShader {
public:
    // 1部品のDXILまたは旧SHEXを作る。shift=1で先頭アドレスの非整列を再現する。
    explicit AModelTestShader(EShaderStage stage = EShaderStage::Compute, u32 model = 0x60u, bool dxil = true, usize shift = 0u) noexcept : m_Data(m_Storage + shift), m_Size(dxil ? 72u : 52u), m_Stage(stage)
    {
        Put(0u, 0x43425844u);
        Put(20u, 1u);
        Put(24u, static_cast<u32>(m_Size));
        Put(28u, 1u);
        Put(32u, 36u);
        Put(36u, dxil ? 0x4c495844u : 0x58454853u);
        Put(40u, dxil ? 28u : 8u);
        Put(44u, (Kind() << 16u) | model);
        Put(48u, dxil ? 7u : 2u);
        if (dxil) {
            Put(52u, 0x4c495844u);
            Put(56u, 0x100u);
            Put(60u, 16u);
            Put(64u, 4u);
            Put(68u, 0xdec04342u);
        }
    }

    // 4バイトを書き換える。試験で指定する位置は所有領域内に限る。
    void Put(usize offset, u32 value) noexcept
    {
        // 下位バイトから格納し、製品の読み取り関数は利用しない。
        for (u32 i = 0u; i < 4u; ++i) {
            m_Data[offset + i] = static_cast<byte>(value >> (8u * i));
        }
    }

    // 本体の前へ補助部品を追加する。長さ1なら後続DXIL部品も非整列になる。
    void PrependPart(u32 fourcc, u32 payload_size) noexcept
    {
        // オフセット表の増加分4バイトと追加部品の大きさ。
        const usize shift = 12u + payload_size;
        // 元の部品を後ろから移動し、重なりによる上書きを避ける。
        for (usize end = m_Size; end > 36u; --end) {
            m_Data[end - 1u + shift] = m_Data[end - 1u];
        }
        m_Size += shift;
        Put(24u, static_cast<u32>(m_Size));
        Put(28u, 2u);
        Put(32u, 40u);
        Put(36u, static_cast<u32>(36u + shift));
        Put(40u, fourcc);
        Put(44u, payload_size);
        // 補助部品の内容は本体と同じ版表現にし、FourCCによる識別を検査する。
        for (u32 i = 0u; i < payload_size; ++i) {
            m_Data[48u + i] = 0u;
        }
        if (payload_size >= 28u) {
            Put(48u, (Kind() << 16u) | 0x66u);
            Put(52u, 7u);
            Put(56u, 0x4c495844u);
            Put(60u, 0x106u);
            Put(64u, 16u);
            Put(68u, 4u);
            Put(72u, 0xdec04342u);
        }
    }

    // 外部に見せる長さだけを変えて切断を再現する。
    void SetSize(usize size) noexcept { m_Size = size; }

    // バイト列の欠落を再現する。以降は書換え関数を呼ばない。
    void ClearData() noexcept { m_Data = nullptr; }

    // 既存インタフェースを通じて、保存された段階とバイト列を返す。
    EShaderStage Stage() const noexcept override { return m_Stage; }
    // 所有領域はヘルパー呼出し中不変とする。
    const byte* Bytecode() const noexcept override { return m_Data; }
    // 範囲検査に使われる長さを返す。
    usize BytecodeSize() const noexcept override { return m_Size; }

private:
    // 公式DxilConstants.hのShaderKind値へ変換する。
    u32 Kind() const noexcept { return m_Stage == EShaderStage::Vertex ? 1u : m_Stage == EShaderStage::Pixel ? 0u : 5u; }

    // 非整列試験の基準になる4バイト整列の固定領域。
    alignas(4) byte m_Storage[256]{};
    // 先頭ずらしまたはnullの試験入力。
    byte* m_Data;
    // IRhiShaderから返す実際の可読長。
    usize m_Size;
    // IRhiShaderが申告する実行段階。
    EShaderStage m_Stage;
};

// 差し替えるのは機能照会のみ。解析と最大要求版の選択は製品ヘルパーを通る。
struct FModelQueryState {
    // 機能照会が返すHRESULT。
    HRESULT result = S_OK;
    // 仮想デバイスが対応する上限。
    D3D_SHADER_MODEL supported = D3D_SHADER_MODEL_6_6;
    // 実際の照会回数。再試行やSM5照会を検出する。
    u32 calls = 0u;
    // 製品が最後に要求した版。
    D3D_SHADER_MODEL requested = static_cast<D3D_SHADER_MODEL>(0);
};

// 実APIと同じく入力以下の対応上限を返し、失敗時のHRESULTも注入する。
HRESULT QueryModel(void* context, D3D12_FEATURE_DATA_SHADER_MODEL& model) noexcept
{
    // 試験中だけ借用する照会状態。
    auto& state = *static_cast<FModelQueryState*>(context);
    ++state.calls;
    state.requested = model.HighestShaderModel;
    if (state.supported < model.HighestShaderModel) model.HighestShaderModel = state.supported;
    return state.result;
}

// 破損は機能照会より先に拒否されることを検査する。
void ExpectMalformed(const IRhiShader& shader) noexcept
{
    // 誤って照会へ進んだ場合も検出する。
    FModelQueryState state{};
    EXPECT_EQ(CheckDx12ShaderModel_Internal(shader, nullptr, QueryModel, &state), E_INVALIDARG);
    EXPECT_EQ(state.calls, 0u);
}

} // namespace

// API自体が成功しても必要版に届かなければ拒否する先行反例。
ACS_TEST(Dx12ShaderModel, SuccessfulQueryWithOnlySM51IsUnsupported)
{
    // 事前コンパイル済みCS6.0。
    AModelTestShader shader;
    // SM5.1までのデバイスを再現する。
    FModelQueryState state{};
    state.supported = D3D_SHADER_MODEL_5_1;
    EXPECT_EQ(CheckDx12ShaderModel_Internal(shader, nullptr, QueryModel, &state), DXGI_ERROR_UNSUPPORTED);
    EXPECT_EQ(state.calls, 1u);
    EXPECT_EQ(state.requested, D3D_SHADER_MODEL_6_0);
}

// 未知の要求版を低い版へ下げて通さず、実APIのエラーを保持する。
ACS_TEST(Dx12ShaderModel, UnknownRuntimePreservesInvalidArgumentWithoutRetry)
{
    // 実行時が解釈できないSM6.6を要求する。
    AModelTestShader shader(EShaderStage::Compute, 0x66u);
    // E_INVALIDARGと低い対応版を同時に返す。
    FModelQueryState state{};
    state.result = E_INVALIDARG;
    state.supported = D3D_SHADER_MODEL_5_1;
    EXPECT_EQ(CheckDx12ShaderModel_Internal(shader, nullptr, QueryModel, &state), E_INVALIDARG);
    EXPECT_EQ(state.calls, 1u);
    EXPECT_EQ(state.requested, D3D_SHADER_MODEL_6_6);
}

// 版不足への置換でデバイス障害を隠さない。
ACS_TEST(Dx12ShaderModel, QueryFailureHresultIsPreserved)
{
    // 通常のSM6入力。
    AModelTestShader shader;
    // 障害と低い対応版を返す照会。
    FModelQueryState state{};
    state.result = DXGI_ERROR_DEVICE_REMOVED;
    state.supported = D3D_SHADER_MODEL_5_1;
    EXPECT_EQ(CheckDx12ShaderModel_Internal(shader, nullptr, QueryModel, &state), DXGI_ERROR_DEVICE_REMOVED);
    EXPECT_EQ(state.calls, 1u);
}

// 成功HRESULTも正規化せず返す。
ACS_TEST(Dx12ShaderModel, SuccessfulHresultIsPreserved)
{
    // 通常のSM6入力。
    AModelTestShader shader;
    // HRESULT保持だけを確かめる合成の成功結果。
    FModelQueryState state{};
    state.result = S_FALSE;
    EXPECT_EQ(CheckDx12ShaderModel_Internal(shader, nullptr, QueryModel, &state), S_FALSE);
    EXPECT_EQ(state.calls, 1u);
}

// VSより高いPSの要求版を採用し、個別照会に分けない。
ACS_TEST(Dx12ShaderModel, Vertex60AndPixel66QueryMaximumOnce)
{
    // 低い要求版のVS。
    AModelTestShader vs(EShaderStage::Vertex, 0x60u);
    // 高い要求版のPS。
    AModelTestShader ps(EShaderStage::Pixel, 0x66u);
    // 最大要求版まで対応する照会。
    FModelQueryState state{};
    EXPECT_EQ(CheckDx12ShaderModel_Internal(vs, &ps, QueryModel, &state), S_OK);
    EXPECT_EQ(state.requested, D3D_SHADER_MODEL_6_6);
    EXPECT_EQ(state.calls, 1u);
    state.calls = 0u;
    state.supported = D3D_SHADER_MODEL_6_0;
    EXPECT_EQ(CheckDx12ShaderModel_Internal(vs, &ps, QueryModel, &state), DXGI_ERROR_UNSUPPORTED);
    EXPECT_EQ(state.calls, 1u);
}

// VS側が高い場合もPSで最大要求版を上書きしない。
ACS_TEST(Dx12ShaderModel, HigherVertexRequirementIsRetained)
{
    // 高い要求版のVS。
    AModelTestShader vs(EShaderStage::Vertex, 0x66u);
    // 低い要求版のPS。
    AModelTestShader ps(EShaderStage::Pixel, 0x60u);
    // 対応版が十分な照会。
    FModelQueryState state{};
    EXPECT_EQ(CheckDx12ShaderModel_Internal(vs, &ps, QueryModel, &state), S_OK);
    EXPECT_EQ(state.requested, D3D_SHADER_MODEL_6_6);
    EXPECT_EQ(state.calls, 1u);
}

// 深度専用描画のPS省略を許し、VSだけで照会する。
ACS_TEST(Dx12ShaderModel, MissingPixelShaderQueriesVertexOnly)
{
    // PSを使わないVS6.0。
    AModelTestShader vs(EShaderStage::Vertex);
    // 必要版ちょうどの対応上限。
    FModelQueryState state{};
    state.supported = D3D_SHADER_MODEL_6_0;
    EXPECT_EQ(CheckDx12ShaderModel_Internal(vs, nullptr, QueryModel, &state), S_OK);
    EXPECT_EQ(state.requested, D3D_SHADER_MODEL_6_0);
    EXPECT_EQ(state.calls, 1u);
}

// 旧バイトコードだけなら照会を追加しない。
ACS_TEST(Dx12ShaderModel, LegacyGraphicsAndComputeDoNotQuery)
{
    // SM5.1のVS。
    AModelTestShader vs(EShaderStage::Vertex, 0x51u, false);
    // SM5.1のPS。
    AModelTestShader ps(EShaderStage::Pixel, 0x51u, false);
    // SM5.1のCS。
    AModelTestShader cs(EShaderStage::Compute, 0x51u, false);
    // 呼ばれれば失敗する照会。
    FModelQueryState state{};
    state.result = E_FAIL;
    EXPECT_EQ(CheckDx12ShaderModel_Internal(vs, &ps, QueryModel, &state), S_OK);
    EXPECT_EQ(CheckDx12ShaderModel_Internal(vs, nullptr, QueryModel, &state), S_OK);
    EXPECT_EQ(CheckDx12ShaderModel_Internal(cs, nullptr, QueryModel, &state), S_OK);
    EXPECT_EQ(state.calls, 0u);
}

// 旧VSとSM6のPSが混在してもPSの照会を省略しない。
ACS_TEST(Dx12ShaderModel, LegacyVertexDoesNotHideDxilPixel)
{
    // 旧形式のVS。
    AModelTestShader vs(EShaderStage::Vertex, 0x51u, false);
    // SM6.6のPS。
    AModelTestShader ps(EShaderStage::Pixel, 0x66u);
    // SM6.6に対応する照会。
    FModelQueryState state{};
    EXPECT_EQ(CheckDx12ShaderModel_Internal(vs, &ps, QueryModel, &state), S_OK);
    EXPECT_EQ(state.requested, D3D_SHADER_MODEL_6_6);
    EXPECT_EQ(state.calls, 1u);
}

// バイト列先頭が4バイト境界外でも安全に読む。
ACS_TEST(Dx12ShaderModel, UnalignedContainerIsReadable)
{
    // 先頭アドレスを1バイトずらす。
    AModelTestShader shader(EShaderStage::Compute, 0x66u, true, 1u);
    // 読み出された要求版を記録する。
    FModelQueryState state{};
    EXPECT_NE(reinterpret_cast<uptr>(shader.Bytecode()) % 4u, 0u);
    EXPECT_EQ(CheckDx12ShaderModel_Internal(shader, nullptr, QueryModel, &state), S_OK);
    EXPECT_EQ(state.requested, D3D_SHADER_MODEL_6_6);
    EXPECT_EQ(state.calls, 1u);
}

// 公式の連続配置を守る非整列部品も、型付きポインターに変換せず読む。
ACS_TEST(Dx12ShaderModel, UnalignedPartAfterOddSizedPartIsReadable)
{
    // 1バイトのPRIV部品を前置する。
    AModelTestShader shader;
    shader.PrependPart(0x56495250u, 1u);
    // DXILの要求を確認する。
    FModelQueryState state{};
    EXPECT_EQ(CheckDx12ShaderModel_Internal(shader, nullptr, QueryModel, &state), S_OK);
    EXPECT_EQ(state.requested, D3D_SHADER_MODEL_6_0);
    EXPECT_EQ(state.calls, 1u);
}

// デバッグと反射部品に含まれるDXIL表現を本体として採用しない。
ACS_TEST(Dx12ShaderModel, DebugAndStatisticsPartsDoNotRaiseRequirement)
{
    // ILDBとSTATの部品識別子。
    const u32 auxiliary_parts[] = {0x42444c49u, 0x54415453u};
    // どちらにもSM6.6を埋め込み、本体のSM6.0を採用することを検査する。
    for (u32 fourcc : auxiliary_parts) {
        // 補助部品より低い要求版の本体。
        AModelTestShader shader;
        shader.PrependPart(fourcc, 28u);
        // 本体の要求版までしか対応しない照会。
        FModelQueryState state{};
        state.supported = D3D_SHADER_MODEL_6_0;
        EXPECT_EQ(CheckDx12ShaderModel_Internal(shader, nullptr, QueryModel, &state), S_OK);
        EXPECT_EQ(state.requested, D3D_SHADER_MODEL_6_0);
        EXPECT_EQ(state.calls, 1u);
    }
}

// DXIL版ではなくProgramVersionから必要SMを読む。
ACS_TEST(Dx12ShaderModel, DxilVersionIsNotShaderModel)
{
    // ProgramVersionは6.6、内部DXIL版は1.0の合成ヘッダー。
    AModelTestShader shader(EShaderStage::Compute, 0x66u);
    // 抽出値を記録する。
    FModelQueryState state{};
    EXPECT_EQ(CheckDx12ShaderModel_Internal(shader, nullptr, QueryModel, &state), S_OK);
    EXPECT_EQ(state.requested, D3D_SHADER_MODEL_6_6);
}

// 必要な照会が無ければ成功させず、SM5では未使用の照会を要求しない。
ACS_TEST(Dx12ShaderModel, MissingQueryRejectedOnlyWhenNeeded)
{
    // 照会が必要なDXIL。
    AModelTestShader dxil;
    // 照会しない旧形式。
    AModelTestShader legacy(EShaderStage::Compute, 0x51u, false);
    EXPECT_EQ(CheckDx12ShaderModel_Internal(dxil, nullptr, nullptr, nullptr), E_INVALIDARG);
    EXPECT_EQ(CheckDx12ShaderModel_Internal(legacy, nullptr, nullptr, nullptr), S_OK);
}

// あらゆる途中切断を宣言長から検出する。
ACS_TEST(Dx12ShaderModel, NullAndEveryTruncatedLengthAreRejected)
{
    // 元の全長72バイト未満を順番に渡す。
    for (usize size = 0u; size < 72u; ++size) {
        // ヘッダーの宣言長は変えない。
        AModelTestShader shader;
        shader.SetSize(size);
        ExpectMalformed(shader);
    }
    // 長さがあってもnullは拒否する。
    AModelTestShader missing;
    missing.ClearData();
    ExpectMalformed(missing);
}

// コンテナの署名・版・長さ・部品数の破損を照会前に拒否する。
ACS_TEST(Dx12ShaderModel, MalformedContainerHeaderIsRejected)
{
    // 書換え位置と不正値。大きな部品数は表の乗算前に拒否する。
    const u32 mutations[][2] = {{0u, 0x4c495844u}, {20u, 2u}, {24u, 31u}, {24u, 73u}, {24u, 0xffffffffu}, {28u, 0xffffffffu}, {28u, 0u}};
    // 各変種を独立に作る。
    for (const auto& mutation : mutations) {
        // 1フィールドだけ壊す元コンテナ。
        AModelTestShader shader;
        shader.Put(mutation[0], mutation[1]);
        ExpectMalformed(shader);
    }
}

// オフセットの巻戻り・隙間・オーバーフローをポインター計算前に拒否する。
ACS_TEST(Dx12ShaderModel, PartOffsetsAreBoundsChecked)
{
    // ヘッダー内・表内・隙間・末尾・桁あふれを起こす位置。
    const u32 offsets[] = {0u, 32u, 35u, 40u, 71u, 0xfffffffcu};
    // 同じ本体へ不正オフセットだけを適用する。
    for (u32 offset : offsets) {
        // 正常な1部品コンテナ。
        AModelTestShader shader;
        shader.Put(32u, offset);
        ExpectMalformed(shader);
    }
}

// 部品長を加算するとあふれる値も、差分による範囲検査で拒否する。
ACS_TEST(Dx12ShaderModel, PartSizeOverflowIsRejected)
{
    // 読み出し領域を越える部品長を注入する。
    AModelTestShader shader;
    shader.Put(40u, 0xfffffffcu);
    ExpectMalformed(shader);
}

// 同じ部品を二重に参照する表を許さない。
ACS_TEST(Dx12ShaderModel, RepeatedPartOffsetIsRejected)
{
    // 2部品の正常な配置から第2位置だけを壊す。
    AModelTestShader shader;
    shader.PrependPart(0x42444c49u, 28u);
    shader.Put(36u, 40u);
    ExpectMalformed(shader);
}

// 物理的には別々のDXIL部品でも要求版を選ばず拒否する。
ACS_TEST(Dx12ShaderModel, DuplicateDxilPartsAreRejected)
{
    // 前側SM6.6、後側SM6.0のDXILを作る。
    AModelTestShader shader;
    shader.PrependPart(0x4c495844u, 28u);
    ExpectMalformed(shader);
}

// ヘッダー長不足・単位換算の桁あふれ・内部署名破損を検査する。
ACS_TEST(Dx12ShaderModel, MalformedProgramHeaderIsRejected)
{
    // 書換え位置と不正値。
    const u32 mutations[][2] = {{40u, 20u}, {48u, 0u}, {48u, 5u}, {48u, 8u}, {48u, 0x40000007u}, {52u, 0u}};
    // ProgramVersionより後ろの構造も照会前に確認する。
    for (const auto& mutation : mutations) {
        // 一度に1フィールドを壊す。
        AModelTestShader shader;
        shader.Put(mutation[0], mutation[1]);
        ExpectMalformed(shader);
    }
}

// ビットコード位置は内部ヘッダー基準であり、その長さを越えないことを検査する。
ACS_TEST(Dx12ShaderModel, BitcodeBoundsAndOffsetOverflowAreRejected)
{
    // 開始位置がヘッダー内・末尾外・桁あふれ、または長さが零・過大の変種。
    const u32 mutations[][2] = {{60u, 0u}, {60u, 15u}, {60u, 17u}, {60u, 0xfffffffcu}, {64u, 0u}, {64u, 5u}, {64u, 0xffffffffu}};
    // 各境界違反で問い合わせを発生させない。
    for (const auto& mutation : mutations) {
        // 正常な内部ヘッダーを1箇所だけ変更する。
        AModelTestShader shader;
        shader.Put(mutation[0], mutation[1]);
        ExpectMalformed(shader);
    }
}

// 本体の宣言段階や予約ビットの不一致をSM5扱いで通さない。
ACS_TEST(Dx12ShaderModel, InvalidProgramVersionIsRejected)
{
    // CSのSM5表記・予約ビット・別段階・未知段階。
    const u32 versions[] = {0x00050051u, 0x00050160u, 0x00010060u, 0xffff0060u};
    // 各不正ProgramVersionを検査する。
    for (u32 version : versions) {
        // IRhiShaderの段階はComputeのまま。
        AModelTestShader shader;
        shader.Put(44u, version);
        ExpectMalformed(shader);
    }
}

// 第1シェーダーが正常でも、第2シェーダーの破損を照会前に拒否する。
ACS_TEST(Dx12ShaderModel, MalformedPixelIsRejectedBeforeQuery)
{
    // 正常なVS6.0。
    AModelTestShader vs(EShaderStage::Vertex);
    // 切断されたPS6.6。
    AModelTestShader ps(EShaderStage::Pixel, 0x66u);
    ps.SetSize(71u);
    // 部分的に解析した要求版で照会しないことを確認する。
    FModelQueryState state{};
    EXPECT_EQ(CheckDx12ShaderModel_Internal(vs, &ps, QueryModel, &state), E_INVALIDARG);
    EXPECT_EQ(state.calls, 0u);
}

#endif

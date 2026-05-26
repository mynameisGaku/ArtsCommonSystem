// SPDX-License-Identifier: Apache-2.0
// FDiligentShader 実装
#include "render/Diligent/DiligentShader.h"

#if WITH_RENDER_DILIGENT

#include "render/Diligent/DiligentCommon.h"
#include "render/Diligent/DiligentDevice.h"
#include "foundation/Log.h"

#include <cctype>
#include <cstring>

namespace acs {

namespace {

// `cbuffer X : register(b3)` or `Texture2D Y : register(t1)` から
// 型 prefix と slot 番号 + 識別子名を抽出する超軽量 HLSL scanner。
// 完全な HLSL parser ではないが、ACS の slim HLSL (前処理 #pragma /
// #define はあるが register 句は plain 数字) を相手にする想定。
// 戻り値: 成功なら true、out_name に identifier、out_slot に数字。
//
// keyword: "cbuffer" / "Texture2D" / "SamplerState" 等
// reg_letter: 'b' / 't' / 's'
void ParseShaderBindings(
    const char* src,
    const char* keyword,
    char        reg_letter,
    char        out_names[][FDiligentShader::kMaxNameLen],
    u32         out_capacity) noexcept
{
    if (!src || !keyword) return;
    const usize klen = std::strlen(keyword);
    auto is_ident_chr = [](char c) noexcept {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
    };
    const char* p = src;
    while (*p) {
        // keyword の境界マッチ (前が ident 文字でない位置のみ)
        bool at_boundary = (p == src) || !is_ident_chr(*(p - 1));
        if (at_boundary && std::strncmp(p, keyword, klen) == 0
            && !is_ident_chr(p[klen])) {
            const char* q = p + klen;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') ++q;
            // identifier 抽出
            const char* name_begin = q;
            while (is_ident_chr(*q)) ++q;
            const usize name_len = static_cast<usize>(q - name_begin);
            if (name_len == 0) { ++p; continue; }
            // ': register ( <letter> <digits> )' を探す
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') ++q;
            if (*q != ':') { p = q; continue; }
            ++q;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') ++q;
            if (std::strncmp(q, "register", 8) != 0) { p = q; continue; }
            q += 8;
            while (*q == ' ' || *q == '\t' || *q == '(' ) ++q;
            if (*q != reg_letter) { p = q; continue; }
            ++q;
            // 数字
            u32 slot = 0; bool got_digit = false;
            while (std::isdigit(static_cast<unsigned char>(*q))) {
                slot = slot * 10 + static_cast<u32>(*q - '0');
                ++q; got_digit = true;
            }
            if (!got_digit) { p = q; continue; }
            if (slot < out_capacity) {
                const usize copy = name_len < (FDiligentShader::kMaxNameLen - 1)
                                    ? name_len : FDiligentShader::kMaxNameLen - 1;
                std::memcpy(out_names[slot], name_begin, copy);
                out_names[slot][copy] = '\0';
            }
            p = q;
        } else {
            ++p;
        }
    }
}

} // namespace

FDiligentShader::~FDiligentShader() noexcept {
    if (_shader) { _shader->Release(); _shader = nullptr; }
}

TResult<void> FDiligentShader::Init(FDiligentDevice& device, const FShaderDesc& desc) noexcept {
    _device = &device;
    _stage  = desc.stage;

    auto* dev = device.RenderDev();
    if (!dev) return ACS_ERR(Render, 140, "FDiligentShader: device not initialized");
    if (!desc.hlsl_source) return ACS_ERR(Render, 141, "FDiligentShader: hlsl_source is null");

    Diligent::ShaderCreateInfo sci;
    sci.SourceLanguage = Diligent::SHADER_SOURCE_LANGUAGE_HLSL;
    // DEFAULT は D3D12 だと FXC (SM 5.1 上限)。DXC に切り替えると SM6.0+
    // 利用可だが、dxcompiler.dll をビルド成果物に同梱する設定が要る。
    // Phase 30 で post-build copy / NuGet 取得を整備したらここを
    // SHADER_COMPILER_DXC に上げる。現状は FXC で十分 (compute / wave 等
    // SM6 専用機能はまだ未使用)。
    sci.ShaderCompiler = Diligent::SHADER_COMPILER_DEFAULT;
    sci.HLSLVersion    = {5, 1};
    sci.Source         = desc.hlsl_source;
    sci.EntryPoint     = desc.entry_point ? desc.entry_point : "main";

    // Diligent 新版で UseCombinedTextureSamplers / CombinedSamplerSuffix は
    // ShaderCreateInfo から FShaderDesc に移動した。
    // true にすると Diligent が <texture>_sampler 名で sampler を自動紐付け
    // するので、PSO 側の ImmutableSamplerDesc::SamplerOrTextureName に
    // テクスチャ名 ("albedo" 等) を渡すだけで sampler binding が成立する。
    // (D3D12 でも HLSL は分離宣言のまま、紐付けの abstraction)
    Diligent::FShaderDesc sd;
    sd.Name                       = desc.debug_name ? desc.debug_name : "ACS_Shader";
    sd.ShaderType                 = diligent_detail::ToDiligent(desc.stage);
    sd.UseCombinedTextureSamplers = true;
    sd.CombinedSamplerSuffix      = "_sampler";
    sci.Desc                      = sd;

    dev->CreateShader(sci, &_shader);
    if (!_shader) {
        ACS_LOG_ERROR("Diligent: CreateShader failed (entry=%s, name=%s)",
                      sci.EntryPoint, sd.Name);
        return ACS_ERR(Render, 142, "CreateShader failed");
    }

    // HLSL source を parse して register(bN/tN) → 名前マッピングを構築。
    // Diligent::ShaderResourceDesc に BindPoint が無いため自前で抽出する。
    ParseShaderBindings(desc.hlsl_source, "cbuffer",      'b', _cb_names,  kMaxSlots);
    ParseShaderBindings(desc.hlsl_source, "Texture2D",    't', _tex_names, kMaxSlots);
    ParseShaderBindings(desc.hlsl_source, "Texture3D",    't', _tex_names, kMaxSlots);
    ParseShaderBindings(desc.hlsl_source, "TextureCube",  't', _tex_names, kMaxSlots);

    return Ok();
}

} // namespace acs

#endif // WITH_RENDER_DILIGENT

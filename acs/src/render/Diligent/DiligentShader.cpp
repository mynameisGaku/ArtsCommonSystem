// SPDX-License-Identifier: Apache-2.0
// DiligentShader 実装
#include "render/Diligent/DiligentShader.h"

#if WITH_RENDER_DILIGENT

#include "render/Diligent/DiligentCommon.h"
#include "render/Diligent/DiligentDevice.h"
#include "foundation/Log.h"

#include <cctype>
#include <cstring>

namespace acs {

namespace {

/**
 * HLSL source から `keyword X : register(<letter>N)` を走査し slot→名前を埋める。
 *
 * @details
 * `cbuffer X : register(b3)` や `Texture2D Y : register(t1)` のような宣言を相手にする
 * 超軽量 scanner。完全な HLSL parser ではなく、register 句が plain 数字である ACS の
 * slim HLSL を前提とする。slot < out_capacity の宣言だけ out_names[slot] に識別子名を
 * 書き込む (名前は kMaxNameLen-1 で切り詰め)。
 * @param src 走査対象の HLSL source 文字列 (null なら何もしない)。
 * @param keyword 探すリソースキーワード ("cbuffer" / "Texture2D" 等、null なら何もしない)。
 * @param reg_letter register レジスタ種別の文字 ('b' / 't' / 's')。
 * @param out_names slot ごとの名前を書き込む二次元バッファ。
 * @param out_capacity 書き込み可能な slot 数 (これ以上の slot は無視)。
 */
void ParseShaderBindings(
    const char* src,
    const char* keyword,
    char        reg_letter,
    char        out_names[][DiligentShader::kMaxNameLen],
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
        const bool at_boundary = (p == src) || !is_ident_chr(*(p - 1));
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
                const usize copy = name_len < (DiligentShader::kMaxNameLen - 1)
                                    ? name_len : DiligentShader::kMaxNameLen - 1;
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

/** Diligent シェーダオブジェクトを解放する。 */
DiligentShader::~DiligentShader() noexcept {
    if (m_Shader) { m_Shader->Release(); m_Shader = nullptr; }
}

/** HLSL source をコンパイルしてシェーダを生成し、register binding を抽出する。 */
TResult<void> DiligentShader::Init(DiligentDevice& device, const FShaderDesc& desc) noexcept {
    m_Device = &device;
    m_Stage  = desc.stage;

    auto* dev = device.RenderDev();
    if (!dev) return ACS_ERR(Render, 140, "DiligentShader: device not initialized");
    if (!desc.hlsl_source) return ACS_ERR(Render, 141, "DiligentShader: hlsl_source is null");

    Diligent::ShaderCreateInfo sci;
    sci.SourceLanguage = Diligent::SHADER_SOURCE_LANGUAGE_HLSL;
    // DEFAULT は D3D12 だと FXC (SM 5.1 上限)。DXC に切り替えると SM6.0+
    // 利用可だが、dxcompiler.dll をビルド成果物に同梱する設定が要る。
    // 現状は FXC で十分 (compute / wave 等 SM6 専用機能はまだ未使用)。
    sci.ShaderCompiler = Diligent::SHADER_COMPILER_DEFAULT;
    sci.HLSLVersion    = {5, 1};
    sci.Source         = desc.hlsl_source;
    sci.EntryPoint     = desc.entry_point ? desc.entry_point : "main";

    // Diligent 新版で UseCombinedTextureSamplers / CombinedSamplerSuffix は
    // ShaderCreateInfo から FShaderDesc に移動した。
    // true にすると Diligent が <texture>m_Sampler 名で sampler を自動紐付け
    // するので、PSO 側の ImmutableSamplerDesc::SamplerOrTextureName に
    // テクスチャ名 ("albedo" 等) を渡すだけで sampler binding が成立する。
    // (D3D12 でも HLSL は分離宣言のまま、紐付けの abstraction)
    Diligent::ShaderDesc sd;
    sd.Name                       = desc.debug_name ? desc.debug_name : "ACS_Shader";
    sd.ShaderType                 = diligent_detail::ToDiligent(desc.stage);
    sd.UseCombinedTextureSamplers = true;
    sd.CombinedSamplerSuffix      = "m_Sampler";
    sci.Desc                      = sd;

    dev->CreateShader(sci, &m_Shader);
    if (!m_Shader) {
        ACS_LOG_ERROR("Diligent: CreateShader failed (entry=%s, name=%s)",
                      sci.EntryPoint, sd.Name);
        return ACS_ERR(Render, 142, "CreateShader failed");
    }

    // HLSL source を parse して register(bN/tN) → 名前マッピングを構築。
    // Diligent::ShaderResourceDesc に BindPoint が無いため自前で抽出する。
    ParseShaderBindings(desc.hlsl_source, "cbuffer",      'b', m_CbNames,  kMaxSlots);
    ParseShaderBindings(desc.hlsl_source, "Texture2D",    't', m_TexNames, kMaxSlots);
    ParseShaderBindings(desc.hlsl_source, "Texture3D",    't', m_TexNames, kMaxSlots);
    ParseShaderBindings(desc.hlsl_source, "TextureCube",  't', m_TexNames, kMaxSlots);

    return Ok();
}

} // namespace acs

#endif // WITH_RENDER_DILIGENT

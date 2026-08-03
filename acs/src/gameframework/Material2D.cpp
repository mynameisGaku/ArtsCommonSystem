// SPDX-License-Identifier: Apache-2.0
// マテリアルアセット (.acsmat) の解析 / 書き出し / 適用。
#include "gameframework/Material2D.h"

#include "container/Array.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace acs::game {

namespace {

// 効果プリセットの一覧 (enum 値と名前)。ドロップダウン順 = この並び。
struct FEffectEntry { ESpriteEffect effect; const char* name; };
const FEffectEntry kEffects[] = {
    { ESpriteEffect::None,       "None"       },
    { ESpriteEffect::Grayscale,  "Grayscale"  },
    { ESpriteEffect::Tint,       "Tint"       },
    { ESpriteEffect::Vignette,   "Vignette"   },
    { ESpriteEffect::Wave,       "Wave"       },
    { ESpriteEffect::Pixelate,   "Pixelate"   },
    { ESpriteEffect::HueShift,   "HueShift"   },
    { ESpriteEffect::Brightness, "Brightness" },
    { ESpriteEffect::Invert,     "Invert"     },
    { ESpriteEffect::Sepia,      "Sepia"      },
    { ESpriteEffect::Posterize,  "Posterize"  },
    { ESpriteEffect::Scanline,   "Scanline"   },
    { ESpriteEffect::Chromatic,  "Chromatic"  },
};
constexpr u32 kEffectCount = static_cast<u32>(sizeof(kEffects) / sizeof(kEffects[0]));

// ASCII の大文字小文字を無視して比較する (STL の strcasecmp は使わない)。
bool IEquals(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

constexpr const char* kMaterialKeys[] = {
    "name", "kind", "effect", "strength", "p0", "p1", "p2", "color", "animated",
    "baseColor", "metallic", "roughness", "emissive", "emissiveStrength",
    "normalStrength", "ao", "shadingMode", "shadow1Color", "shadow1Threshold",
    "shadow2Color", "shadow2Threshold", "rimColor", "rimPower", "specColor",
    "specThreshold", "toonSoftness", "clearcoat", "clearcoatRoughness", "anisotropy",
    "specularLevel", "specularTint", "sheen", "sheenRoughness", "sheenColor",
    "subsurface", "subsurfaceColor", "transmission", "ior", "albedo", "normal",
    "substrateEnabled", "substrateRoot", "substrateNodeCount",
    "substrateExprRoot", "substrateExprCount",
    "substrateExprTexture0", "substrateExprTexture1",
    "substrateExprTexture2", "substrateExprTexture3",
};
constexpr u32 kMaterialStaticKeyCount =
    static_cast<u32>(sizeof(kMaterialKeys) / sizeof(kMaterialKeys[0]));
constexpr u32 kSubstrateNodeKeyBase = kMaterialStaticKeyCount;
constexpr u32 kSubstrateSlabKeyBase = kSubstrateNodeKeyBase + kSubstrateMaxNodes;
constexpr u32 kSubstrateExprBindKeyBase =
    kSubstrateSlabKeyBase + kSubstrateMaxNodes;
constexpr u32 kSubstrateExprNodeKeyBase =
    kSubstrateExprBindKeyBase + kSubstrateMaxNodes;
constexpr u32 kMaterialKeyCount =
    kSubstrateExprNodeKeyBase + kShaderExpressionMaxNodes;
constexpr u32 kMaterialSeenKeyWords = (kMaterialKeyCount + 63u) / 64u;
static_assert(kMaterialKeyCount <= 256u);

int IndexedKeySuffix(const char* key, const char* prefix, u32 max_count) noexcept {
    if (key == nullptr || prefix == nullptr || max_count == 0u) return -1;
    const char* k = key;
    const char* p = prefix;
    while (*p != '\0') {
        char kc = *k, pc = *p;
        if (kc >= 'A' && kc <= 'Z') kc = static_cast<char>(kc - 'A' + 'a');
        if (pc >= 'A' && pc <= 'Z') pc = static_cast<char>(pc - 'A' + 'a');
        if (kc != pc) return -1;
        ++k; ++p;
    }
    if (*k < '0' || *k > '9') return -1;
    u32 value = 0u;
    while (*k >= '0' && *k <= '9') {
        const u32 digit = static_cast<u32>(*k - '0');
        if (value > (max_count - 1u) / 10u ||
            (value == (max_count - 1u) / 10u &&
             digit > (max_count - 1u) % 10u)) {
            return -1;
        }
        value = value * 10u + digit;
        ++k;
    }
    return *k == '\0' ? static_cast<int>(value) : -1;
}

int MaterialKeyIndex(const char* key) noexcept {
    for (u32 i = 0; i < kMaterialStaticKeyCount; ++i) {
        if (IEquals(key, kMaterialKeys[i])) return static_cast<int>(i);
    }
    const int node = IndexedKeySuffix(
        key, "substrateNode", kSubstrateMaxNodes);
    if (node >= 0) return static_cast<int>(kSubstrateNodeKeyBase) + node;
    const int slab = IndexedKeySuffix(
        key, "substrateSlab", kSubstrateMaxNodes);
    if (slab >= 0) return static_cast<int>(kSubstrateSlabKeyBase) + slab;
    const int binding = IndexedKeySuffix(
        key, "substrateExprBind", kSubstrateMaxNodes);
    if (binding >= 0) {
        return static_cast<int>(kSubstrateExprBindKeyBase) + binding;
    }
    const int expression = IndexedKeySuffix(
        key, "substrateExpr", kShaderExpressionMaxNodes);
    if (expression >= 0) {
        return static_cast<int>(kSubstrateExprNodeKeyBase) + expression;
    }
    return -1;
}

enum class ENumberParseStatus : u8 { Ok, Invalid, OutOfRange };

void SkipSpaces(const char*& p) noexcept {
    while (*p == ' ' || *p == '\t') ++p;
}

ENumberParseStatus ParseFloatList(const char* text, f32* out, u32 count) noexcept {
    if (text == nullptr || out == nullptr || count == 0u) return ENumberParseStatus::Invalid;
    const char* p = text;
    for (u32 i = 0; i < count; ++i) {
        SkipSpaces(p);
        if (*p == '\0') return ENumberParseStatus::Invalid;
        errno = 0;
        char* end = nullptr;
        const f32 value = std::strtof(p, &end);
        if (end == p) return ENumberParseStatus::Invalid;
        if (errno == ERANGE || !std::isfinite(value)) return ENumberParseStatus::OutOfRange;
        out[i] = value;
        p = end;
    }
    SkipSpaces(p);
    return *p == '\0' ? ENumberParseStatus::Ok : ENumberParseStatus::Invalid;
}

ENumberParseStatus ParseI32(const char* text, i32& out) noexcept {
    if (text == nullptr) return ENumberParseStatus::Invalid;
    const char* p = text;
    SkipSpaces(p);
    if (*p == '\0') return ENumberParseStatus::Invalid;
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(p, &end, 10);
    if (end == p) return ENumberParseStatus::Invalid;
    const char* tail = end;
    SkipSpaces(tail);
    if (*tail != '\0') return ENumberParseStatus::Invalid;
    if (errno == ERANGE ||
        value < static_cast<long long>(std::numeric_limits<i32>::min()) ||
        value > static_cast<long long>(std::numeric_limits<i32>::max())) {
        return ENumberParseStatus::OutOfRange;
    }
    out = static_cast<i32>(value);
    return ENumberParseStatus::Ok;
}

ENumberParseStatus ParseSubstrateNodeFields(const char* text,
                                            i32& type, i32& input_a, i32& input_b,
                                            f32& factor, u32& flags) noexcept {
    if (text == nullptr) return ENumberParseStatus::Invalid;
    const char* p = text;
    auto signed_token = [&](i32& destination) noexcept -> ENumberParseStatus {
        SkipSpaces(p);
        if (*p == '\0') return ENumberParseStatus::Invalid;
        errno = 0;
        char* end = nullptr;
        const long long v = std::strtoll(p, &end, 10);
        if (end == p) return ENumberParseStatus::Invalid;
        if (errno == ERANGE || v < static_cast<long long>(std::numeric_limits<i32>::min()) ||
            v > static_cast<long long>(std::numeric_limits<i32>::max())) {
            return ENumberParseStatus::OutOfRange;
        }
        destination = static_cast<i32>(v);
        p = end;
        return ENumberParseStatus::Ok;
    };
    ENumberParseStatus status = signed_token(type);
    if (status != ENumberParseStatus::Ok) return status;
    status = signed_token(input_a);
    if (status != ENumberParseStatus::Ok) return status;
    status = signed_token(input_b);
    if (status != ENumberParseStatus::Ok) return status;
    SkipSpaces(p);
    errno = 0;
    char* float_end = nullptr;
    factor = std::strtof(p, &float_end);
    if (float_end == p) return ENumberParseStatus::Invalid;
    if (errno == ERANGE || !std::isfinite(factor)) return ENumberParseStatus::OutOfRange;
    p = float_end;
    SkipSpaces(p);
    errno = 0;
    char* flag_end = nullptr;
    const unsigned long long raw_flags = std::strtoull(p, &flag_end, 10);
    if (flag_end == p) return ENumberParseStatus::Invalid;
    if (errno == ERANGE || raw_flags > static_cast<unsigned long long>(
            std::numeric_limits<u32>::max())) {
        return ENumberParseStatus::OutOfRange;
    }
    p = flag_end;
    SkipSpaces(p);
    if (*p != '\0') return ENumberParseStatus::Invalid;
    flags = static_cast<u32>(raw_flags);
    return ENumberParseStatus::Ok;
}

ENumberParseStatus ParseI16List(const char* text, i16* out, u32 count) noexcept {
    if (text == nullptr || out == nullptr || count == 0u) {
        return ENumberParseStatus::Invalid;
    }
    const char* p = text;
    for (u32 i = 0u; i < count; ++i) {
        SkipSpaces(p);
        if (*p == '\0') return ENumberParseStatus::Invalid;
        errno = 0;
        char* end = nullptr;
        const long long value = std::strtoll(p, &end, 10);
        if (end == p) return ENumberParseStatus::Invalid;
        if (errno == ERANGE ||
            value < static_cast<long long>(std::numeric_limits<i16>::min()) ||
            value > static_cast<long long>(std::numeric_limits<i16>::max())) {
            return ENumberParseStatus::OutOfRange;
        }
        out[i] = static_cast<i16>(value);
        p = end;
    }
    SkipSpaces(p);
    return *p == '\0'
        ? ENumberParseStatus::Ok
        : ENumberParseStatus::Invalid;
}

ENumberParseStatus ParseShaderExpressionNodeFields(
    const char* text, FShaderExpressionNode& out) noexcept {
    if (text == nullptr) return ENumberParseStatus::Invalid;
    const char* p = text;
    auto signed_token = [&](i32& destination) noexcept -> ENumberParseStatus {
        SkipSpaces(p);
        if (*p == '\0') return ENumberParseStatus::Invalid;
        errno = 0;
        char* end = nullptr;
        const long long value = std::strtoll(p, &end, 10);
        if (end == p) return ENumberParseStatus::Invalid;
        if (errno == ERANGE ||
            value < static_cast<long long>(std::numeric_limits<i32>::min()) ||
            value > static_cast<long long>(std::numeric_limits<i32>::max())) {
            return ENumberParseStatus::OutOfRange;
        }
        destination = static_cast<i32>(value);
        p = end;
        return ENumberParseStatus::Ok;
    };
    auto unsigned_token = [&](u32& destination) noexcept -> ENumberParseStatus {
        SkipSpaces(p);
        if (*p == '\0') return ENumberParseStatus::Invalid;
        errno = 0;
        char* end = nullptr;
        const unsigned long long value = std::strtoull(p, &end, 10);
        if (end == p) return ENumberParseStatus::Invalid;
        if (errno == ERANGE ||
            value > static_cast<unsigned long long>(
                std::numeric_limits<u32>::max())) {
            return ENumberParseStatus::OutOfRange;
        }
        destination = static_cast<u32>(value);
        p = end;
        return ENumberParseStatus::Ok;
    };

    i32 op = 0;
    i32 declared_type = 0;
    i32 texture_slot = 0;
    i32 texture_flags = 0;
    i32 component_index = 0;
    i32 inputs[3]{};
    ENumberParseStatus status = signed_token(op);
    if (status != ENumberParseStatus::Ok) return status;
    status = signed_token(declared_type);
    if (status != ENumberParseStatus::Ok) return status;
    status = signed_token(texture_slot);
    if (status != ENumberParseStatus::Ok) return status;
    status = signed_token(texture_flags);
    if (status != ENumberParseStatus::Ok) return status;
    status = signed_token(component_index);
    if (status != ENumberParseStatus::Ok) return status;
    for (u32 i = 0u; i < 3u; ++i) {
        status = signed_token(inputs[i]);
        if (status != ENumberParseStatus::Ok) return status;
    }
    u32 parameter_id = 0u;
    u32 texture_low = 0u;
    u32 texture_high = 0u;
    status = unsigned_token(parameter_id);
    if (status != ENumberParseStatus::Ok) return status;
    status = unsigned_token(texture_low);
    if (status != ENumberParseStatus::Ok) return status;
    status = unsigned_token(texture_high);
    if (status != ENumberParseStatus::Ok) return status;
    f32 values[4]{};
    for (u32 i = 0u; i < 4u; ++i) {
        SkipSpaces(p);
        if (*p == '\0') return ENumberParseStatus::Invalid;
        errno = 0;
        char* end = nullptr;
        values[i] = std::strtof(p, &end);
        if (end == p) return ENumberParseStatus::Invalid;
        if (errno == ERANGE || !std::isfinite(values[i])) {
            return ENumberParseStatus::OutOfRange;
        }
        p = end;
    }
    SkipSpaces(p);
    if (*p != '\0') return ENumberParseStatus::Invalid;
    if (op < 0 || op > static_cast<i32>(std::numeric_limits<u8>::max()) ||
        declared_type < 0 ||
        declared_type > static_cast<i32>(std::numeric_limits<u8>::max()) ||
        texture_slot < 0 ||
        texture_slot > static_cast<i32>(std::numeric_limits<u8>::max()) ||
        texture_flags < 0 ||
        texture_flags > static_cast<i32>(std::numeric_limits<u8>::max()) ||
        component_index < 0 ||
        component_index > static_cast<i32>(std::numeric_limits<u8>::max())) {
        return ENumberParseStatus::OutOfRange;
    }
    for (u32 i = 0u; i < 3u; ++i) {
        if (inputs[i] < static_cast<i32>(std::numeric_limits<i16>::min()) ||
            inputs[i] > static_cast<i32>(std::numeric_limits<i16>::max())) {
            return ENumberParseStatus::OutOfRange;
        }
        out.inputs[i] = static_cast<i16>(inputs[i]);
    }
    out.op = static_cast<EShaderExpressionOp>(op);
    out.declared_type =
        static_cast<EShaderExpressionValueType>(declared_type);
    out.texture_slot = static_cast<u8>(texture_slot);
    out.texture_flags = static_cast<u8>(texture_flags);
    out.component_index = static_cast<u8>(component_index);
    out.parameter_id = parameter_id;
    out.texture_asset_id_low = texture_low;
    out.texture_asset_id_high = texture_high;
    out.value = FShaderExpressionValue{
        values[0], values[1], values[2], values[3]};
    return ENumberParseStatus::Ok;
}

usize BoundedCStringLength(const char* text, usize max_size, bool& terminated) noexcept {
    terminated = false;
    if (text == nullptr) return 0u;
    for (usize i = 0; i <= max_size; ++i) {
        if (text[i] == '\0') {
            terminated = true;
            return i;
        }
    }
    return max_size + 1u;
}

bool SeekFileEnd(std::FILE* file) noexcept {
#if defined(_WIN32)
    return ::_fseeki64(file, 0, SEEK_END) == 0;
#else
    return ::fseeko(file, 0, SEEK_END) == 0;
#endif
}

bool SeekFileBegin(std::FILE* file) noexcept {
#if defined(_WIN32)
    return ::_fseeki64(file, 0, SEEK_SET) == 0;
#else
    return ::fseeko(file, 0, SEEK_SET) == 0;
#endif
}

i64 TellFile(std::FILE* file) noexcept {
#if defined(_WIN32)
    return static_cast<i64>(::_ftelli64(file));
#else
    return static_cast<i64>(::ftello(file));
#endif
}

} // namespace

FLitMaterialParams ToLitParams(const FPbrParams2D& q) noexcept {
    FLitMaterialParams lm;
    lm.baseColor = q.baseColor; lm.metallic = q.metallic; lm.roughness = q.roughness;
    lm.normalStrength = q.normalStrength; lm.ao = q.ao;
    lm.emissive = q.emissive; lm.emissiveStrength = q.emissiveStrength;
    lm.shadingMode = q.shadingMode;
    lm.shadow1Color = q.shadow1Color; lm.shadow1Threshold = q.shadow1Threshold;
    lm.shadow2Color = q.shadow2Color; lm.shadow2Threshold = q.shadow2Threshold;
    lm.rimColor = q.rimColor; lm.rimPower = q.rimPower;
    lm.specColor = q.specColor; lm.specThreshold = q.specThreshold;
    lm.toonSoftness = q.toonSoftness;
    lm.clearcoat = q.clearcoat; lm.clearcoatRoughness = q.clearcoatRoughness;
    lm.anisotropy = q.anisotropy; lm.specularLevel = q.specularLevel; lm.specularTint = q.specularTint;
    lm.sheen = q.sheen; lm.sheenRoughness = q.sheenRoughness; lm.sheenColor = q.sheenColor;
    lm.subsurface = q.subsurface; lm.subsurfaceColor = q.subsurfaceColor;
    return lm;
}

FSubstrateMaterial MakeLegacySubstrateMaterial(const FPbrParams2D& p) noexcept {
    auto sat = [](f32 v) noexcept {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    };
    auto mul = [](FVec3 v, f32 s) noexcept { return FVec3{v.x*s, v.y*s, v.z*s}; };
    auto lerp = [](FVec3 a, FVec3 b, f32 t) noexcept {
        return FVec3{a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t, a.z+(b.z-a.z)*t};
    };

    FSubstrateMaterial material{};
    material.enabled = true;
    material.node_count = 1u;
    material.root = 0;
    FSubstrateNode& base_node = material.nodes[0];
    base_node.type = ESubstrateNodeType::Slab;
    base_node.input_a = base_node.input_b = kSubstrateInvalidNode;
    FSubstrateSlab& s = base_node.slab;
    const f32 metallic = sat(p.metallic);
    const f32 dielectric_f0 = sat(p.specularLevel) * 0.08f;
    const FVec3 dielectric{dielectric_f0, dielectric_f0, dielectric_f0};
    const FVec3 base_color{p.baseColor.x, p.baseColor.y, p.baseColor.z};
    s.diffuse_albedo = mul(base_color, 1.0f - metallic);
    s.f0 = lerp(dielectric, base_color, metallic);
    const f32 tint = sat(p.specularTint);
    s.f90 = lerp(FVec3{1,1,1}, base_color, tint);
    s.roughness = sat(p.roughness);
    s.second_roughness = s.roughness;
    s.second_roughness_weight = 0.0f;
    s.anisotropy = p.anisotropy < -1.0f ? -1.0f :
                   (p.anisotropy > 1.0f ? 1.0f : p.anisotropy);
    s.emissive = mul(p.emissive, p.emissiveStrength < 0.0f ? 0.0f : p.emissiveStrength);
    s.fuzz_color = p.sheenColor;
    s.fuzz_amount = sat(p.sheen);
    s.fuzz_roughness = sat(p.sheenRoughness);
    s.normal_strength = p.normalStrength < 0.0f ? 0.0f :
                        (p.normalStrength > 4.0f ? 4.0f : p.normalStrength);
    if (p.subsurface > 0.0f) {
        const f32 radius_cm = sat(p.subsurface);
        s.mean_free_path_cm = mul(p.subsurfaceColor, radius_cm);
    }
    if (p.transmission > 0.0f) {
        const f32 transmission = sat(p.transmission);
        s.transmittance = FVec3{transmission, transmission, transmission};
        s.thickness_cm = 0.01f;
        s.mean_free_path_cm =
            SubstrateTransmittanceToMeanFreePath(s.transmittance, s.thickness_cm);
        s.f0 = SubstrateIorToF0(p.ior);
    }

    // Clear coat is represented as a real transparent top slab + coverage
    // operator + vertical layer, rather than a special scalar lobe.
    if (p.clearcoat > 0.0f) {
        material.node_count = 4u;
        FSubstrateNode& coat = material.nodes[1];
        coat.type = ESubstrateNodeType::Slab;
        coat.input_a = coat.input_b = kSubstrateInvalidNode;
        coat.flags = static_cast<u32>(ESubstrateBsdfMode::SimpleClearCoat)
                   << SubstrateNodeFlag_BsdfModeShift;
        coat.slab.diffuse_albedo = FVec3{0,0,0};
        coat.slab.f0 = FVec3{0.04f,0.04f,0.04f};
        coat.slab.f90 = FVec3{1,1,1};
        coat.slab.roughness = sat(p.clearcoatRoughness);
        coat.slab.transmittance = FVec3{1,1,1};
        coat.slab.thickness_cm = 0.001f;

        FSubstrateNode& coverage = material.nodes[2];
        coverage.type = ESubstrateNodeType::CoverageWeight;
        coverage.input_a = 1;
        coverage.input_b = kSubstrateInvalidNode;
        coverage.factor = sat(p.clearcoat);

        FSubstrateNode& vertical = material.nodes[3];
        vertical.type = ESubstrateNodeType::VerticalLayer;
        vertical.input_a = 2;
        vertical.input_b = 0;
        vertical.factor = coat.slab.thickness_cm;
        material.root = 3;
    }
    return material;
}

bool ResolveMaterialSubstrate(const FMaterial2D& material,
                              FSubstrateResolvedSurface& out,
                              FSubstrateCompileStats* out_stats) noexcept {
    if (material.substrate.enabled) {
        return ResolveSubstrateMaterial(material.substrate, out, out_stats);
    }
    const FSubstrateMaterial legacy = MakeLegacySubstrateMaterial(material.pbr);
    return ResolveSubstrateMaterial(legacy, out, out_stats);
}

bool SyncLegacyPbrFromSubstrate(FMaterial2D& material) noexcept {
    if (!material.substrate.enabled) return true;
    FSubstrateResolvedSurface s{};
    if (!ResolveSubstrateMaterial(material.substrate, s, nullptr)) return false;
    auto sat = [](f32 v) noexcept {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    };
    auto max3 = [](FVec3 v) noexcept {
        const f32 xy = v.x > v.y ? v.x : v.y;
        return xy > v.z ? xy : v.z;
    };
    const f32 f0_max = max3(s.f0);
    const f32 metallic = sat((f0_max - 0.08f) / 0.92f);
    material.pbr.metallic = metallic;
    material.pbr.baseColor = FVec4{
        metallic > 0.5f ? s.f0.x : s.diffuse_albedo.x,
        metallic > 0.5f ? s.f0.y : s.diffuse_albedo.y,
        metallic > 0.5f ? s.f0.z : s.diffuse_albedo.z,
        s.coverage};
    material.pbr.roughness = sat(s.roughness);
    material.pbr.anisotropy = s.anisotropy;
    material.pbr.normalStrength = s.normal_strength;
    const f32 emissive_max = max3(s.emissive);
    material.pbr.emissiveStrength = emissive_max;
    material.pbr.emissive = emissive_max > 1.0e-6f
        ? FVec3{s.emissive.x/emissive_max, s.emissive.y/emissive_max,
                s.emissive.z/emissive_max}
        : FVec3{0,0,0};
    material.pbr.sheen = sat(s.fuzz_amount);
    material.pbr.sheenColor = s.fuzz_color;
    material.pbr.sheenRoughness = sat(s.fuzz_roughness);
    const f32 mfp_max = max3(s.mean_free_path_cm);
    material.pbr.subsurface = sat(mfp_max);
    material.pbr.subsurfaceColor = mfp_max > 1.0e-6f
        ? FVec3{s.mean_free_path_cm.x/mfp_max, s.mean_free_path_cm.y/mfp_max,
                s.mean_free_path_cm.z/mfp_max}
        : FVec3{1.0f,0.3f,0.2f};
    material.pbr.transmission = sat(max3(s.transmittance));
    if (material.pbr.transmission > 1.0e-6f) {
        // The refraction path consumes baseColor as absorption/tint.  A glass
        // slab has zero diffuse albedo by design, so preserve its medium
        // transmittance instead of turning the refraction shell black.
        material.pbr.baseColor.x = sat(s.transmittance.x);
        material.pbr.baseColor.y = sat(s.transmittance.y);
        material.pbr.baseColor.z = sat(s.transmittance.z);
        material.pbr.baseColor.w = s.coverage;
    }
    const f32 dielectric_f0 = f0_max < 0.999f ? f0_max : 0.999f;
    const f32 root_f0 = std::sqrt(dielectric_f0);
    material.pbr.ior = (1.0f + root_f0) / (1.0f - root_f0);
    return true;
}

u32 SpriteEffectCount() noexcept { return kEffectCount; }

const char* SpriteEffectName(ESpriteEffect e) noexcept {
    for (u32 i = 0; i < kEffectCount; ++i)
        if (kEffects[i].effect == e) return kEffects[i].name;
    return "None";
}

const char* SpriteEffectNameAt(u32 index) noexcept {
    return index < kEffectCount ? kEffects[index].name : "None";
}

ESpriteEffect SpriteEffectAt(u32 index) noexcept {
    return index < kEffectCount ? kEffects[index].effect : ESpriteEffect::None;
}

ESpriteEffect SpriteEffectFromName(const char* name) noexcept {
    for (u32 i = 0; i < kEffectCount; ++i)
        if (IEquals(kEffects[i].name, name)) return kEffects[i].effect;
    return ESpriteEffect::None;
}

FEffectParams DefaultEffectParams(ESpriteEffect e) noexcept {
    FEffectParams p;   // 既定: strength=1, p*=0, color=白
    switch (e) {
        case ESpriteEffect::Grayscale:  p.strength = 1.0f; break;
        case ESpriteEffect::Tint:       p.strength = 1.0f; p.color = FVec4{ 1.0f, 0.5f, 0.3f, 1.0f }; break;
        case ESpriteEffect::Vignette:   p.strength = 0.7f; p.p0 = 0.4f; p.p1 = 1.0f; p.color = FVec4{ 0, 0, 0, 1 }; break;
        case ESpriteEffect::Wave:       p.strength = 0.03f; p.p0 = 12.0f; p.p1 = 3.0f; break;
        case ESpriteEffect::Pixelate:   p.strength = 1.0f; p.p0 = 24.0f; p.p1 = 24.0f; break;
        case ESpriteEffect::HueShift:   p.strength = 0.0f; p.p0 = 90.0f; break;        // p0=90°/s でアニメ
        case ESpriteEffect::Brightness: p.strength = 1.2f; p.p0 = 1.3f; break;          // 明るさ/コントラスト
        case ESpriteEffect::Invert:     p.strength = 1.0f; break;
        case ESpriteEffect::Sepia:      p.strength = 1.0f; break;
        case ESpriteEffect::Posterize:  p.strength = 1.0f; p.p0 = 4.0f; break;          // 4 階調
        case ESpriteEffect::Scanline:   p.strength = 0.4f; p.p0 = 120.0f; break;        // 120 本
        case ESpriteEffect::Chromatic:  p.strength = 1.0f; p.p0 = 0.01f; break;         // UV 0.01 ずらし
        case ESpriteEffect::None:
        default: break;
    }
    return p;
}

bool EffectAnimatedByDefault(ESpriteEffect e) noexcept {
    return e == ESpriteEffect::Wave || e == ESpriteEffect::HueShift;
}

const char* FMaterial2DLoadResult::ErrorName(EMaterial2DLoadError error) noexcept {
    switch (error) {
        case EMaterial2DLoadError::None: return "None";
        case EMaterial2DLoadError::NullArgument: return "NullArgument";
        case EMaterial2DLoadError::InputTooLarge: return "InputTooLarge";
        case EMaterial2DLoadError::EmptyInput: return "EmptyInput";
        case EMaterial2DLoadError::EmbeddedNul: return "EmbeddedNul";
        case EMaterial2DLoadError::TooManyLines: return "TooManyLines";
        case EMaterial2DLoadError::LineTooLong: return "LineTooLong";
        case EMaterial2DLoadError::InvalidHeader: return "InvalidHeader";
        case EMaterial2DLoadError::UnsupportedVersion: return "UnsupportedVersion";
        case EMaterial2DLoadError::InvalidSyntax: return "InvalidSyntax";
        case EMaterial2DLoadError::KeyTooLong: return "KeyTooLong";
        case EMaterial2DLoadError::ValueTooLong: return "ValueTooLong";
        case EMaterial2DLoadError::DuplicateKey: return "DuplicateKey";
        case EMaterial2DLoadError::InvalidValue: return "InvalidValue";
        case EMaterial2DLoadError::ValueOutOfRange: return "ValueOutOfRange";
        case EMaterial2DLoadError::PathTooLong: return "PathTooLong";
        case EMaterial2DLoadError::FileOpenFailed: return "FileOpenFailed";
        case EMaterial2DLoadError::FileSizeFailed: return "FileSizeFailed";
        case EMaterial2DLoadError::FileChanged: return "FileChanged";
        case EMaterial2DLoadError::FileReadFailed: return "FileReadFailed";
        case EMaterial2DLoadError::AllocationFailure: return "AllocationFailure";
    }
    return "Unknown";
}

FMaterial2DLoadResult TryParseAcsmatText(
    const char* text, usize text_size, FMaterial2D& out) noexcept {
    FMaterial2DLoadResult result{};
    result.bytes_read = static_cast<u64>(text_size);
    if (text == nullptr) {
        result.error = EMaterial2DLoadError::NullArgument;
        return result;
    }
    if (text_size > kMaterial2DMaxTextBytes) {
        result.error = EMaterial2DLoadError::InputTooLarge;
        return result;
    }
    if (text_size == 0u) {
        result.error = EMaterial2DLoadError::EmptyInput;
        return result;
    }
    if (std::memchr(text, '\0', text_size) != nullptr) {
        result.error = EMaterial2DLoadError::EmbeddedNul;
        return result;
    }

    usize offset = 0u;
    u32 line_number = 0u;
    char line[kMaterial2DMaxLineBytes + 1u]{};
    auto next_line = [&](char*& out_line) noexcept -> bool {
        if (offset >= text_size || result.error != EMaterial2DLoadError::None) return false;
        if (++line_number > kMaterial2DMaxLines) {
            result.error = EMaterial2DLoadError::TooManyLines;
            result.line = line_number;
            return false;
        }
        const usize begin = offset;
        while (offset < text_size && text[offset] != '\n') ++offset;
        usize length = offset - begin;
        if (offset < text_size) ++offset;
        if (length > kMaterial2DMaxLineBytes) {
            result.error = EMaterial2DLoadError::LineTooLong;
            result.line = line_number;
            return false;
        }
        std::memcpy(line, text + begin, length);
        if (length > 0u && line[length - 1u] == '\r') --length;
        line[length] = '\0';
        char* first = line;
        while (*first == ' ' || *first == '\t') ++first;
        char* last = first + std::strlen(first);
        while (last > first && (last[-1] == ' ' || last[-1] == '\t')) --last;
        *last = '\0';
        out_line = first;
        return true;
    };

    char* current = nullptr;
    if (!next_line(current)) {
        if (result.error == EMaterial2DLoadError::None) {
            result.error = EMaterial2DLoadError::EmptyInput;
        }
        return result;
    }
    if (std::strcmp(current, "ACSMAT 1") != 0) {
        result.error = std::strncmp(current, "ACSMAT ", 7) == 0
            ? EMaterial2DLoadError::UnsupportedVersion
            : EMaterial2DLoadError::InvalidHeader;
        result.line = line_number;
        return result;
    }

    FMaterial2D staged{};
    bool kind_specified = false;
    u64 seen_keys[kMaterialSeenKeyWords]{};
    while (next_line(current)) {
        if (*current == '\0' || *current == ';' || *current == '#') continue;

        char* value = current;
        while (*value != '\0' && *value != ' ' && *value != '\t') ++value;
        const usize key_length = static_cast<usize>(value - current);
        if (key_length == 0u) {
            result.error = EMaterial2DLoadError::InvalidSyntax;
            result.line = line_number;
            return result;
        }
        if (key_length >= 32u) {
            result.error = EMaterial2DLoadError::KeyTooLong;
            result.line = line_number;
            return result;
        }
        char key[32]{};
        std::memcpy(key, current, key_length);
        while (*value == ' ' || *value == '\t') ++value;
        const usize value_length = std::strlen(value);
        if (value_length > kMaterial2DMaxLineBytes) {
            result.error = EMaterial2DLoadError::ValueTooLong;
            result.line = line_number;
            return result;
        }

        const int key_index = MaterialKeyIndex(key);
        if (key_index < 0) continue; // 前方互換: 未知キーは無視する。
        const u32 key_word = static_cast<u32>(key_index) / 64u;
        const u64 key_mask = u64{1} << (static_cast<u32>(key_index) % 64u);
        if ((seen_keys[key_word] & key_mask) != 0u) {
            result.error = EMaterial2DLoadError::DuplicateKey;
            result.line = line_number;
            return result;
        }
        seen_keys[key_word] |= key_mask;

        ENumberParseStatus number_status = ENumberParseStatus::Ok;
        auto parse_f32 = [&](f32& destination) noexcept {
            f32 parsed = 0.0f;
            number_status = ParseFloatList(value, &parsed, 1u);
            if (number_status == ENumberParseStatus::Ok) destination = parsed;
        };
        auto parse_vec3 = [&](FVec3& destination) noexcept {
            f32 parsed[3]{};
            number_status = ParseFloatList(value, parsed, 3u);
            if (number_status == ENumberParseStatus::Ok) {
                destination = FVec3{parsed[0], parsed[1], parsed[2]};
            }
        };
        auto parse_vec4 = [&](FVec4& destination) noexcept {
            f32 parsed[4]{};
            number_status = ParseFloatList(value, parsed, 4u);
            if (number_status == ENumberParseStatus::Ok) {
                destination = FVec4{parsed[0], parsed[1], parsed[2], parsed[3]};
            }
        };

        if (IEquals(key, "name")) {
            if (value_length >= sizeof(staged.name)) {
                result.error = EMaterial2DLoadError::ValueTooLong;
            } else {
                std::memcpy(staged.name, value, value_length + 1u);
            }
        } else if (IEquals(key, "kind")) {
            if (IEquals(value, "pbr") || IEquals(value, "lit")) {
                staged.kind = EMaterialKind::Lit;
            } else if (IEquals(value, "effect")) {
                staged.kind = EMaterialKind::Effect;
            } else {
                result.error = EMaterial2DLoadError::InvalidValue;
            }
            kind_specified = true;
        } else if (IEquals(key, "effect")) {
            staged.effect = SpriteEffectFromName(value);
            if (staged.effect == ESpriteEffect::None && !IEquals(value, "None")) {
                result.error = EMaterial2DLoadError::InvalidValue;
            }
        } else if (IEquals(key, "strength")) {
            parse_f32(staged.params.strength);
        } else if (IEquals(key, "p0")) {
            parse_f32(staged.params.p0);
        } else if (IEquals(key, "p1")) {
            parse_f32(staged.params.p1);
        } else if (IEquals(key, "p2")) {
            parse_f32(staged.params.p2);
        } else if (IEquals(key, "color")) {
            parse_vec4(staged.params.color);
        } else if (IEquals(key, "animated")) {
            i32 parsed = 0;
            number_status = ParseI32(value, parsed);
            if (number_status == ENumberParseStatus::Ok && (parsed < 0 || parsed > 1)) {
                number_status = ENumberParseStatus::OutOfRange;
            }
            if (number_status == ENumberParseStatus::Ok) staged.animated = parsed != 0;
        } else if (IEquals(key, "baseColor")) {
            parse_vec4(staged.pbr.baseColor);
        } else if (IEquals(key, "metallic")) {
            parse_f32(staged.pbr.metallic);
        } else if (IEquals(key, "roughness")) {
            parse_f32(staged.pbr.roughness);
        } else if (IEquals(key, "emissive")) {
            parse_vec3(staged.pbr.emissive);
        } else if (IEquals(key, "emissiveStrength")) {
            parse_f32(staged.pbr.emissiveStrength);
        } else if (IEquals(key, "normalStrength")) {
            parse_f32(staged.pbr.normalStrength);
        } else if (IEquals(key, "ao")) {
            parse_f32(staged.pbr.ao);
        } else if (IEquals(key, "shadingMode")) {
            i32 parsed = 0;
            number_status = ParseI32(value, parsed);
            if (number_status == ENumberParseStatus::Ok && (parsed < 0 || parsed > 1)) {
                number_status = ENumberParseStatus::OutOfRange;
            }
            if (number_status == ENumberParseStatus::Ok) staged.pbr.shadingMode = parsed;
        } else if (IEquals(key, "shadow1Color")) {
            parse_vec3(staged.pbr.shadow1Color);
        } else if (IEquals(key, "shadow1Threshold")) {
            parse_f32(staged.pbr.shadow1Threshold);
        } else if (IEquals(key, "shadow2Color")) {
            parse_vec3(staged.pbr.shadow2Color);
        } else if (IEquals(key, "shadow2Threshold")) {
            parse_f32(staged.pbr.shadow2Threshold);
        } else if (IEquals(key, "rimColor")) {
            parse_vec3(staged.pbr.rimColor);
        } else if (IEquals(key, "rimPower")) {
            parse_f32(staged.pbr.rimPower);
        } else if (IEquals(key, "specColor")) {
            parse_vec3(staged.pbr.specColor);
        } else if (IEquals(key, "specThreshold")) {
            parse_f32(staged.pbr.specThreshold);
        } else if (IEquals(key, "toonSoftness")) {
            parse_f32(staged.pbr.toonSoftness);
        } else if (IEquals(key, "clearcoat")) {
            parse_f32(staged.pbr.clearcoat);
        } else if (IEquals(key, "clearcoatRoughness")) {
            parse_f32(staged.pbr.clearcoatRoughness);
        } else if (IEquals(key, "anisotropy")) {
            parse_f32(staged.pbr.anisotropy);
        } else if (IEquals(key, "specularLevel")) {
            parse_f32(staged.pbr.specularLevel);
        } else if (IEquals(key, "specularTint")) {
            parse_f32(staged.pbr.specularTint);
        } else if (IEquals(key, "sheen")) {
            parse_f32(staged.pbr.sheen);
        } else if (IEquals(key, "sheenRoughness")) {
            parse_f32(staged.pbr.sheenRoughness);
        } else if (IEquals(key, "sheenColor")) {
            parse_vec3(staged.pbr.sheenColor);
        } else if (IEquals(key, "subsurface")) {
            parse_f32(staged.pbr.subsurface);
        } else if (IEquals(key, "subsurfaceColor")) {
            parse_vec3(staged.pbr.subsurfaceColor);
        } else if (IEquals(key, "transmission")) {
            parse_f32(staged.pbr.transmission);
        } else if (IEquals(key, "ior")) {
            parse_f32(staged.pbr.ior);
        } else if (IEquals(key, "albedo")) {
            if (value_length >= sizeof(staged.pbr.albedoPath)) {
                result.error = EMaterial2DLoadError::ValueTooLong;
            } else {
                std::memcpy(staged.pbr.albedoPath, value, value_length + 1u);
            }
        } else if (IEquals(key, "normal")) {
            if (value_length >= sizeof(staged.pbr.normalPath)) {
                result.error = EMaterial2DLoadError::ValueTooLong;
            } else {
                std::memcpy(staged.pbr.normalPath, value, value_length + 1u);
            }
        } else if (IEquals(key, "substrateEnabled")) {
            i32 parsed = 0;
            number_status = ParseI32(value, parsed);
            if (number_status == ENumberParseStatus::Ok && (parsed < 0 || parsed > 1)) {
                number_status = ENumberParseStatus::OutOfRange;
            }
            if (number_status == ENumberParseStatus::Ok) staged.substrate.enabled = parsed != 0;
        } else if (IEquals(key, "substrateRoot")) {
            number_status = ParseI32(value, staged.substrate.root);
        } else if (IEquals(key, "substrateNodeCount")) {
            i32 parsed = 0;
            number_status = ParseI32(value, parsed);
            if (number_status == ENumberParseStatus::Ok &&
                (parsed < 0 || static_cast<u32>(parsed) > kSubstrateMaxNodes)) {
                number_status = ENumberParseStatus::OutOfRange;
            }
            if (number_status == ENumberParseStatus::Ok) {
                staged.substrate.node_count = static_cast<u32>(parsed);
            }
        } else if (IEquals(key, "substrateExprRoot")) {
            i32 parsed = 0;
            number_status = ParseI32(value, parsed);
            if (number_status == ENumberParseStatus::Ok &&
                (parsed < static_cast<i32>(std::numeric_limits<i16>::min()) ||
                 parsed > static_cast<i32>(std::numeric_limits<i16>::max()))) {
                number_status = ENumberParseStatus::OutOfRange;
            }
            if (number_status == ENumberParseStatus::Ok) {
                staged.substrate.expression_graph.root =
                    static_cast<i16>(parsed);
            }
        } else if (IEquals(key, "substrateExprCount")) {
            i32 parsed = 0;
            number_status = ParseI32(value, parsed);
            if (number_status == ENumberParseStatus::Ok &&
                (parsed < 0 ||
                 static_cast<u32>(parsed) > kShaderExpressionMaxNodes)) {
                number_status = ENumberParseStatus::OutOfRange;
            }
            if (number_status == ENumberParseStatus::Ok) {
                staged.substrate.expression_graph.node_count =
                    static_cast<u16>(parsed);
            }
        } else if (IEquals(key, "substrateExprTexture0") ||
                   IEquals(key, "substrateExprTexture1") ||
                   IEquals(key, "substrateExprTexture2") ||
                   IEquals(key, "substrateExprTexture3")) {
            const u32 slot = static_cast<u32>(
                key[std::strlen("substrateExprTexture")] - '0');
            if (slot >= kShaderExpressionMaxTextureSlots ||
                value_length >=
                    sizeof(staged.substrateExpressionTexturePaths[slot])) {
                result.error = EMaterial2DLoadError::ValueTooLong;
            } else {
                std::memcpy(
                    staged.substrateExpressionTexturePaths[slot],
                    value, value_length + 1u);
            }
        } else {
            const int node_index = IndexedKeySuffix(
                key, "substrateNode", kSubstrateMaxNodes);
            const int slab_index = IndexedKeySuffix(
                key, "substrateSlab", kSubstrateMaxNodes);
            const int binding_index = IndexedKeySuffix(
                key, "substrateExprBind", kSubstrateMaxNodes);
            const int expression_index = IndexedKeySuffix(
                key, "substrateExpr", kShaderExpressionMaxNodes);
            if (node_index >= 0) {
                i32 type = 0;
                FSubstrateNode& node = staged.substrate.nodes[static_cast<u32>(node_index)];
                number_status = ParseSubstrateNodeFields(
                    value, type, node.input_a, node.input_b, node.factor, node.flags);
                if (number_status == ENumberParseStatus::Ok &&
                    (type < 0 || type > static_cast<i32>(ESubstrateNodeType::Select))) {
                    number_status = ENumberParseStatus::OutOfRange;
                }
                if (number_status == ENumberParseStatus::Ok) {
                    node.type = static_cast<ESubstrateNodeType>(type);
                }
            } else if (slab_index >= 0) {
                f32 values[kSubstrateSlabScalarCount]{};
                number_status = ParseFloatList(value, values, kSubstrateSlabScalarCount);
                if (number_status == ENumberParseStatus::Ok &&
                    !DecodeSubstrateSlab(
                        values, staged.substrate.nodes[static_cast<u32>(slab_index)].slab)) {
                    number_status = ENumberParseStatus::OutOfRange;
                }
            } else if (binding_index >= 0) {
                number_status = ParseI16List(
                    value,
                    staged.substrate.nodes[static_cast<u32>(binding_index)]
                        .expressions.roots,
                    kSubstrateSlabScalarCount);
            } else if (expression_index >= 0) {
                number_status = ParseShaderExpressionNodeFields(
                    value,
                    staged.substrate.expression_graph
                        .nodes[static_cast<u32>(expression_index)]);
            }
        }

        if (result.error != EMaterial2DLoadError::None) {
            result.line = line_number;
            return result;
        }
        if (number_status != ENumberParseStatus::Ok) {
            result.error = number_status == ENumberParseStatus::OutOfRange
                ? EMaterial2DLoadError::ValueOutOfRange
                : EMaterial2DLoadError::InvalidValue;
            result.line = line_number;
            return result;
        }
    }
    if (result.error != EMaterial2DLoadError::None) return result;
    if (!kind_specified) staged.kind = EMaterialKind::Effect;
    if (staged.substrate.enabled && staged.substrate.node_count == 0u) {
        result.error = EMaterial2DLoadError::InvalidValue;
        result.line = line_number;
        return result;
    }
    if (staged.substrate.node_count > 0u) {
        for (u32 i = 0; i < staged.substrate.node_count; ++i) {
            const u32 node_key = kSubstrateNodeKeyBase + i;
            const bool has_node =
                (seen_keys[node_key / 64u] & (u64{1} << (node_key % 64u))) != 0u;
            if (!has_node) {
                result.error = EMaterial2DLoadError::InvalidValue;
                result.line = line_number;
                return result;
            }
            if (staged.substrate.nodes[i].type == ESubstrateNodeType::Slab) {
                const u32 slab_key = kSubstrateSlabKeyBase + i;
                const bool has_slab =
                    (seen_keys[slab_key / 64u] & (u64{1} << (slab_key % 64u))) != 0u;
                if (!has_slab) {
                    result.error = EMaterial2DLoadError::InvalidValue;
                    result.line = line_number;
                    return result;
                }
            }
        }
        const FSubstrateCompileResult compiled =
            CompileSubstrateMaterial(staged.substrate);
        if (!compiled.Succeeded()) {
            result.error = EMaterial2DLoadError::InvalidValue;
            result.line = line_number;
            return result;
        }
        if (staged.substrate.enabled && !SyncLegacyPbrFromSubstrate(staged)) {
            result.error = EMaterial2DLoadError::InvalidValue;
            result.line = line_number;
            return result;
        }
    }
    for (u32 i = staged.substrate.node_count; i < kSubstrateMaxNodes; ++i) {
        const u32 node_key = kSubstrateNodeKeyBase + i;
        const u32 slab_key = kSubstrateSlabKeyBase + i;
        const u32 binding_key = kSubstrateExprBindKeyBase + i;
        const bool has_extra =
            (seen_keys[node_key / 64u] & (u64{1} << (node_key % 64u))) != 0u ||
            (seen_keys[slab_key / 64u] & (u64{1} << (slab_key % 64u))) != 0u ||
            (seen_keys[binding_key / 64u] &
             (u64{1} << (binding_key % 64u))) != 0u;
        if (has_extra) {
            result.error = EMaterial2DLoadError::InvalidValue;
            result.line = line_number;
            return result;
        }
    }
    const u32 expression_count =
        staged.substrate.expression_graph.node_count;
    for (u32 i = 0u; i < expression_count; ++i) {
        const u32 expression_key = kSubstrateExprNodeKeyBase + i;
        const bool has_expression =
            (seen_keys[expression_key / 64u] &
             (u64{1} << (expression_key % 64u))) != 0u;
        if (!has_expression) {
            result.error = EMaterial2DLoadError::InvalidValue;
            result.line = line_number;
            return result;
        }
    }
    for (u32 i = expression_count; i < kShaderExpressionMaxNodes; ++i) {
        const u32 expression_key = kSubstrateExprNodeKeyBase + i;
        if ((seen_keys[expression_key / 64u] &
             (u64{1} << (expression_key % 64u))) != 0u) {
            result.error = EMaterial2DLoadError::InvalidValue;
            result.line = line_number;
            return result;
        }
    }
    if (expression_count > 0u) {
        const FShaderExpressionCompileResult expressions =
            CompileShaderExpressionGraph(
                staged.substrate.expression_graph);
        if (!expressions.Succeeded()) {
            result.error = EMaterial2DLoadError::InvalidValue;
            result.line = line_number;
            return result;
        }
    }
    if (staged.substrate.node_count > 0u) {
        const FSubstrateExpressionLinkResult links =
            CompileSubstrateExpressionLinks(staged.substrate);
        if (!links.Succeeded()) {
            result.error = EMaterial2DLoadError::InvalidValue;
            result.line = line_number;
            return result;
        }
    }
    out = staged;
    return result;
}

FMaterial2D ParseAcsmatText(const char* text) noexcept {
    FMaterial2D material{};
    bool terminated = false;
    const usize length = BoundedCStringLength(text, kMaterial2DMaxTextBytes, terminated);
    if (!terminated) return material;
    (void)TryParseAcsmatText(text, length, material);
    return material;
}

u32 WriteAcsmatText(const FMaterial2D& mat, char* buf, u32 buf_size) noexcept {
    if (buf == nullptr || buf_size == 0) return 0;
    const auto valid_text_field =
        [](const char* text, usize capacity) noexcept -> bool {
            const char* end = static_cast<const char*>(
                std::memchr(text, '\0', capacity));
            if (end == nullptr) return false;
            for (const char* p = text; p < end; ++p) {
                if (*p == '\r' || *p == '\n') return false;
            }
            return true;
        };
    if (!valid_text_field(mat.name, sizeof(mat.name)) ||
        !valid_text_field(mat.pbr.albedoPath, sizeof(mat.pbr.albedoPath)) ||
        !valid_text_field(mat.pbr.normalPath, sizeof(mat.pbr.normalPath))) {
        buf[0] = '\0';
        return 0;
    }
    for (u32 slot = 0u; slot < kShaderExpressionMaxTextureSlots; ++slot) {
        if (!valid_text_field(
                mat.substrateExpressionTexturePaths[slot],
                sizeof(mat.substrateExpressionTexturePaths[slot]))) {
            buf[0] = '\0';
            return 0;
        }
    }
    // 両ブロック (effect/pbr) を書いてラウンドトリップを無損失にする。kind が使う側を選ぶ。
    int cur = std::snprintf(
        buf, buf_size,
        "ACSMAT 1\n"
        "name %s\n"
        "kind %s\n"
        "effect %s\n"
        "strength %.4f\n"
        "p0 %.4f\n"
        "p1 %.4f\n"
        "p2 %.4f\n"
        "color %.4f %.4f %.4f %.4f\n"
        "animated %d\n"
        "baseColor %.4f %.4f %.4f %.4f\n"
        "metallic %.4f\n"
        "roughness %.4f\n"
        "emissive %.4f %.4f %.4f\n"
        "emissiveStrength %.4f\n"
        "normalStrength %.4f\n"
        "ao %.4f\n"
        "shadingMode %d\n"
        "shadow1Color %.4f %.4f %.4f\n"
        "shadow1Threshold %.4f\n"
        "shadow2Color %.4f %.4f %.4f\n"
        "shadow2Threshold %.4f\n"
        "rimColor %.4f %.4f %.4f\n"
        "rimPower %.4f\n"
        "specColor %.4f %.4f %.4f\n"
        "specThreshold %.4f\n"
        "toonSoftness %.4f\n"
        "clearcoat %.4f\n"
        "clearcoatRoughness %.4f\n"
        "anisotropy %.4f\n"
        "specularLevel %.4f\n"
        "specularTint %.4f\n"
        "sheen %.4f\n"
        "sheenRoughness %.4f\n"
        "sheenColor %.4f %.4f %.4f\n"
        "subsurface %.4f\n"
        "subsurfaceColor %.4f %.4f %.4f\n"
        "transmission %.4f\n"
        "ior %.4f\n",
        mat.name,
        mat.kind == EMaterialKind::Lit ? "pbr" : "effect",
        SpriteEffectName(mat.effect),
        mat.params.strength, mat.params.p0, mat.params.p1, mat.params.p2,
        mat.params.color.x, mat.params.color.y, mat.params.color.z, mat.params.color.w,
        mat.animated ? 1 : 0,
        mat.pbr.baseColor.x, mat.pbr.baseColor.y, mat.pbr.baseColor.z, mat.pbr.baseColor.w,
        mat.pbr.metallic, mat.pbr.roughness,
        mat.pbr.emissive.x, mat.pbr.emissive.y, mat.pbr.emissive.z,
        mat.pbr.emissiveStrength, mat.pbr.normalStrength, mat.pbr.ao,
        mat.pbr.shadingMode,
        mat.pbr.shadow1Color.x, mat.pbr.shadow1Color.y, mat.pbr.shadow1Color.z, mat.pbr.shadow1Threshold,
        mat.pbr.shadow2Color.x, mat.pbr.shadow2Color.y, mat.pbr.shadow2Color.z, mat.pbr.shadow2Threshold,
        mat.pbr.rimColor.x, mat.pbr.rimColor.y, mat.pbr.rimColor.z, mat.pbr.rimPower,
        mat.pbr.specColor.x, mat.pbr.specColor.y, mat.pbr.specColor.z, mat.pbr.specThreshold,
        mat.pbr.toonSoftness,
        mat.pbr.clearcoat, mat.pbr.clearcoatRoughness, mat.pbr.anisotropy,
        mat.pbr.specularLevel, mat.pbr.specularTint,
        mat.pbr.sheen, mat.pbr.sheenRoughness,
        mat.pbr.sheenColor.x, mat.pbr.sheenColor.y, mat.pbr.sheenColor.z,
        mat.pbr.subsurface,
        mat.pbr.subsurfaceColor.x, mat.pbr.subsurfaceColor.y, mat.pbr.subsurfaceColor.z,
        mat.pbr.transmission, mat.pbr.ior);
    if (cur < 0) return 0;
    if (static_cast<u32>(cur) >= buf_size) return buf_size - 1;
    // テクスチャパスは設定済みのときだけ書く (行末まで)。
    if (mat.pbr.albedoPath[0] != '\0') {
        const int w = std::snprintf(buf + cur, buf_size - static_cast<u32>(cur),
                                    "albedo %s\n", mat.pbr.albedoPath);
        if (w > 0 && static_cast<u32>(cur + w) < buf_size) cur += w;
    }
    if (mat.pbr.normalPath[0] != '\0') {
        const int w = std::snprintf(buf + cur, buf_size - static_cast<u32>(cur),
                                    "normal %s\n", mat.pbr.normalPath);
        if (w > 0 && static_cast<u32>(cur + w) < buf_size) cur += w;
    }
    if (mat.substrate.enabled || mat.substrate.node_count > 0u) {
        const FSubstrateCompileResult compiled = CompileSubstrateMaterial(mat.substrate);
        if (!compiled.Succeeded()) {
            buf[0] = '\0';
            return 0u;
        }
        const FSubstrateExpressionLinkResult links =
            CompileSubstrateExpressionLinks(mat.substrate);
        if (!links.Succeeded()) {
            buf[0] = '\0';
            return 0u;
        }
        auto append = [&](const char* text, u32 length) noexcept -> bool {
            if (length >= buf_size - static_cast<u32>(cur)) {
                buf[0] = '\0';
                return false;
            }
            std::memcpy(buf + cur, text, length);
            cur += static_cast<int>(length);
            buf[cur] = '\0';
            return true;
        };
        char line[kMaterial2DMaxLineBytes + 1u]{};
        int w = std::snprintf(line, sizeof(line),
                              "substrateEnabled %d\nsubstrateRoot %d\n"
                              "substrateNodeCount %u\n",
                              mat.substrate.enabled ? 1 : 0,
                              mat.substrate.root, mat.substrate.node_count);
        if (w <= 0 || static_cast<usize>(w) >= sizeof(line) ||
            !append(line, static_cast<u32>(w))) return 0u;
        for (u32 i = 0; i < mat.substrate.node_count; ++i) {
            const FSubstrateNode& node = mat.substrate.nodes[i];
            w = std::snprintf(line, sizeof(line),
                              "substrateNode%u %u %d %d %.9g %u\n",
                              i, static_cast<u32>(node.type), node.input_a,
                              node.input_b, static_cast<double>(node.factor), node.flags);
            if (w <= 0 || static_cast<usize>(w) >= sizeof(line) ||
                !append(line, static_cast<u32>(w))) return 0u;
            if (node.type != ESubstrateNodeType::Slab) continue;

            f32 values[kSubstrateSlabScalarCount]{};
            EncodeSubstrateSlab(node.slab, values);
            w = std::snprintf(line, sizeof(line), "substrateSlab%u", i);
            if (w <= 0 || static_cast<usize>(w) >= sizeof(line)) return 0u;
            u32 line_length = static_cast<u32>(w);
            for (u32 j = 0; j < kSubstrateSlabScalarCount; ++j) {
                const int part = std::snprintf(
                    line + line_length, sizeof(line) - line_length,
                    " %.9g", static_cast<double>(values[j]));
                if (part <= 0 || static_cast<usize>(part) >= sizeof(line) - line_length) {
                    buf[0] = '\0';
                    return 0u;
                }
                line_length += static_cast<u32>(part);
            }
            if (line_length + 1u >= sizeof(line)) return 0u;
            line[line_length++] = '\n';
            line[line_length] = '\0';
            if (!append(line, line_length)) return 0u;

            bool has_expression_binding = false;
            for (u32 j = 0u; j < kSubstrateSlabScalarCount; ++j) {
                if (node.expressions.roots[j] != kShaderExpressionInvalidNode) {
                    has_expression_binding = true;
                    break;
                }
            }
            if (!has_expression_binding) continue;
            w = std::snprintf(line, sizeof(line), "substrateExprBind%u", i);
            if (w <= 0 || static_cast<usize>(w) >= sizeof(line)) return 0u;
            line_length = static_cast<u32>(w);
            for (u32 j = 0u; j < kSubstrateSlabScalarCount; ++j) {
                const int part = std::snprintf(
                    line + line_length, sizeof(line) - line_length,
                    " %d", node.expressions.roots[j]);
                if (part <= 0 ||
                    static_cast<usize>(part) >= sizeof(line) - line_length) {
                    buf[0] = '\0';
                    return 0u;
                }
                line_length += static_cast<u32>(part);
            }
            if (line_length + 1u >= sizeof(line)) return 0u;
            line[line_length++] = '\n';
            line[line_length] = '\0';
            if (!append(line, line_length)) return 0u;
        }

        w = std::snprintf(
            line, sizeof(line),
            "substrateExprRoot %d\nsubstrateExprCount %u\n",
            mat.substrate.expression_graph.root,
            mat.substrate.expression_graph.node_count);
        if (w <= 0 || static_cast<usize>(w) >= sizeof(line) ||
            !append(line, static_cast<u32>(w))) {
            return 0u;
        }
        for (u32 i = 0u;
             i < mat.substrate.expression_graph.node_count;
             ++i) {
            const FShaderExpressionNode& expression =
                mat.substrate.expression_graph.nodes[i];
            w = std::snprintf(
                line, sizeof(line),
                "substrateExpr%u %u %u %u %u %u %d %d %d %u %u %u "
                "%.9g %.9g %.9g %.9g\n",
                i,
                static_cast<u32>(expression.op),
                static_cast<u32>(expression.declared_type),
                static_cast<u32>(expression.texture_slot),
                static_cast<u32>(expression.texture_flags),
                static_cast<u32>(expression.component_index),
                expression.inputs[0],
                expression.inputs[1],
                expression.inputs[2],
                expression.parameter_id,
                expression.texture_asset_id_low,
                expression.texture_asset_id_high,
                static_cast<double>(expression.value.x),
                static_cast<double>(expression.value.y),
                static_cast<double>(expression.value.z),
                static_cast<double>(expression.value.w));
            if (w <= 0 || static_cast<usize>(w) >= sizeof(line) ||
                !append(line, static_cast<u32>(w))) {
                return 0u;
            }
        }
        for (u32 slot = 0u; slot < kShaderExpressionMaxTextureSlots; ++slot) {
            const char* texture_path =
                mat.substrateExpressionTexturePaths[slot];
            if (texture_path[0] == '\0') continue;
            w = std::snprintf(
                line, sizeof(line), "substrateExprTexture%u %s\n",
                slot, texture_path);
            if (w <= 0 || static_cast<usize>(w) >= sizeof(line) ||
                !append(line, static_cast<u32>(w))) {
                return 0u;
            }
        }
    }
    return static_cast<u32>(cur);
}

FMaterial2DLoadResult TryLoadAcsmatFile(const char* path, FMaterial2D& out) noexcept {
    FMaterial2DLoadResult result{};
    bool terminated = false;
    const usize path_length = BoundedCStringLength(path, kMaterial2DMaxPathBytes, terminated);
    if (path == nullptr) {
        result.error = EMaterial2DLoadError::NullArgument;
        return result;
    }
    if (!terminated || path_length == 0u) {
        result.error = EMaterial2DLoadError::PathTooLong;
        return result;
    }

    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        result.error = EMaterial2DLoadError::FileOpenFailed;
        return result;
    }
    if (!SeekFileEnd(file)) {
        std::fclose(file);
        result.error = EMaterial2DLoadError::FileSizeFailed;
        return result;
    }
    const i64 signed_size = TellFile(file);
    if (signed_size < 0 || !SeekFileBegin(file)) {
        std::fclose(file);
        result.error = EMaterial2DLoadError::FileSizeFailed;
        return result;
    }
    const u64 file_size = static_cast<u64>(signed_size);
    if (file_size > static_cast<u64>(kMaterial2DMaxTextBytes)) {
        std::fclose(file);
        result.error = EMaterial2DLoadError::InputTooLarge;
        return result;
    }
    if (file_size == 0u) {
        std::fclose(file);
        result.error = EMaterial2DLoadError::EmptyInput;
        return result;
    }

    TArray<char> buffer;
    if (!buffer.TrySetNum(static_cast<usize>(file_size))) {
        std::fclose(file);
        result.error = EMaterial2DLoadError::AllocationFailure;
        return result;
    }
    usize total = 0u;
    while (total < buffer.Num()) {
        const usize count = std::fread(buffer.GetData() + total, 1u, buffer.Num() - total, file);
        if (count == 0u) {
            const bool io_error = std::ferror(file) != 0;
            std::fclose(file);
            result.error = io_error
                ? EMaterial2DLoadError::FileReadFailed
                : EMaterial2DLoadError::FileChanged;
            result.bytes_read = static_cast<u64>(total);
            return result;
        }
        total += count;
    }
    const int extra = std::fgetc(file);
    const bool read_error = std::ferror(file) != 0;
    const int close_result = std::fclose(file);
    result.bytes_read = static_cast<u64>(total);
    if (read_error || close_result != 0) {
        result.error = EMaterial2DLoadError::FileReadFailed;
        return result;
    }
    if (extra != EOF) {
        result.error = EMaterial2DLoadError::FileChanged;
        return result;
    }
    return TryParseAcsmatText(buffer.GetData(), buffer.Num(), out);
}

bool LoadAcsmatFile(const char* path, FMaterial2D& out) noexcept {
    return TryLoadAcsmatFile(path, out).Succeeded();
}

bool SaveAcsmatFile(const char* path, const FMaterial2D& mat) noexcept {
    bool path_terminated = false;
    const usize path_length = BoundedCStringLength(path, kMaterial2DMaxPathBytes, path_terminated);
    if (path == nullptr || !path_terminated || path_length == 0u) return false;
    // 32 slab closures + 64 expression nodes and texture paths remain bounded.
    char buf[128u * 1024u];
    const u32 n = WriteAcsmatText(mat, buf, sizeof(buf));
    if (n == 0) return false;
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return false;
    const usize wr = std::fwrite(buf, 1, n, f);
    const int flush_result = std::fflush(f);
    const int close_result = std::fclose(f);
    return wr == n && flush_result == 0 && close_result == 0;
}

namespace {
f32 g_MaterialClock = 0.0f;   // アニメ付きマテリアル用の共有経過秒 (スタンドアロン)。
}

void SetMaterialClock(f32 seconds) noexcept { g_MaterialClock = seconds; }
f32  MaterialClock() noexcept { return g_MaterialClock; }

bool ApplyMaterial(CSpriteBatch& sb, const FMaterial2D& mat, f32 time_sec) noexcept {
    if (mat.effect == ESpriteEffect::None) return false;
    FEffectParams p = mat.params;
    if (mat.animated) p.time = time_sec;
    sb.SetEffect(mat.effect, p);
    return true;
}

} // namespace acs::game

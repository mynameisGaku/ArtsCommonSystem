// SPDX-License-Identifier: Apache-2.0
#include "render/SubstrateMaterial.h"

#include <cmath>

namespace acs {
namespace {

constexpr f32 kSubstrateEpsilon = 1.0e-6f;

f32 Saturate01(f32 v) noexcept {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

bool Finite(f32 v) noexcept { return std::isfinite(v); }
bool Finite(FVec3 v) noexcept { return Finite(v.x) && Finite(v.y) && Finite(v.z); }

f32 Luminance(FVec3 v) noexcept {
    return v.x * 0.2126f + v.y * 0.7152f + v.z * 0.0722f;
}

FVec3 Mul(FVec3 a, f32 b) noexcept { return FVec3{a.x * b, a.y * b, a.z * b}; }
FVec3 Add(FVec3 a, FVec3 b) noexcept { return FVec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
FVec3 NormalizeOr(FVec3 v, FVec3 fallback) noexcept {
    const f32 len2 = v.x*v.x + v.y*v.y + v.z*v.z;
    if (!(len2 > kSubstrateEpsilon) || !Finite(len2)) return fallback;
    return Mul(v, 1.0f / std::sqrt(len2));
}

FVec3 SlabNormalTransmittance(const FSubstrateSlab& s, f32 thickness_cm) noexcept {
    auto medium_channel = [&s, thickness_cm](f32 mfp, f32 authored) noexcept {
        if (mfp > kSubstrateEpsilon) {
            return std::exp(-thickness_cm / mfp);
        }
        const f32 reference_thickness = s.thickness_cm > kSubstrateEpsilon
            ? s.thickness_cm : 0.01f;
        const f32 clamped = authored < 1.0e-6f ? 1.0e-6f : Saturate01(authored);
        return std::pow(clamped, thickness_cm / reference_thickness);
    };
    return FVec3{
        medium_channel(s.mean_free_path_cm.x, s.transmittance.x),
        medium_channel(s.mean_free_path_cm.y, s.transmittance.y),
        medium_channel(s.mean_free_path_cm.z, s.transmittance.z),
    };
}

void CollapseLobes(FSubstrateCompiledLobe* lobes, u32& count) noexcept {
    if (count <= 1u) return;
    FSubstrateSlab s{};
    s.diffuse_albedo={0,0,0}; s.f0={0,0,0}; s.f90={0,0,0}; s.tangent={0,0,0};
    s.mean_free_path_cm={0,0,0}; s.emissive={0,0,0}; s.transmittance={0,0,0};
    s.fuzz_color={0,0,0}; s.normal={0,0,0};
    s.roughness=s.second_roughness=s.second_roughness_weight=s.anisotropy=0;
    s.phase_anisotropy=s.thickness_cm=s.fuzz_amount=s.fuzz_roughness=0;
    s.thin_film_weight=s.thin_film_thickness_nm=s.thin_film_ior=s.normal_strength=0;
    f32 total = 0.0f;
    f32 coverage = 0.0f;
    u32 min_depth = lobes[0].vertical_depth;
    for (u32 i = 0; i < count; ++i) {
        const f32 w = lobes[i].weight * lobes[i].coverage;
        total += w;
        coverage += lobes[i].coverage * lobes[i].weight;
        if (lobes[i].vertical_depth < min_depth) min_depth = lobes[i].vertical_depth;
        const FSubstrateSlab& q = lobes[i].slab;
        s.diffuse_albedo=Add(s.diffuse_albedo,Mul(q.diffuse_albedo,w));
        s.f0=Add(s.f0,Mul(q.f0,w)); s.f90=Add(s.f90,Mul(q.f90,w));
        s.roughness+=q.roughness*w; s.second_roughness+=q.second_roughness*w;
        s.second_roughness_weight+=q.second_roughness_weight*w;
        s.anisotropy+=q.anisotropy*w; s.tangent=Add(s.tangent,Mul(q.tangent,w));
        s.mean_free_path_cm=Add(s.mean_free_path_cm,Mul(q.mean_free_path_cm,w));
        s.phase_anisotropy+=q.phase_anisotropy*w; s.emissive=Add(s.emissive,Mul(q.emissive,w));
        s.transmittance=Add(s.transmittance,Mul(q.transmittance,w));
        s.thickness_cm+=q.thickness_cm*w; s.fuzz_color=Add(s.fuzz_color,Mul(q.fuzz_color,w));
        s.fuzz_amount+=q.fuzz_amount*w; s.fuzz_roughness+=q.fuzz_roughness*w;
        s.thin_film_weight+=q.thin_film_weight*w;
        s.thin_film_thickness_nm+=q.thin_film_thickness_nm*w; s.thin_film_ior+=q.thin_film_ior*w;
        s.normal=Add(s.normal,Mul(q.normal,w)); s.normal_strength+=q.normal_strength*w;
    }
    if (total <= kSubstrateEpsilon) {
        count = 0u;
        return;
    }
    const f32 inv=1.0f/total;
    s.diffuse_albedo=Mul(s.diffuse_albedo,inv); s.f0=Mul(s.f0,inv); s.f90=Mul(s.f90,inv);
    s.roughness*=inv; s.second_roughness*=inv; s.second_roughness_weight*=inv;
    s.anisotropy*=inv; s.tangent=Mul(s.tangent,inv); s.mean_free_path_cm=Mul(s.mean_free_path_cm,inv);
    s.phase_anisotropy*=inv; s.emissive=Mul(s.emissive,inv); s.transmittance=Mul(s.transmittance,inv);
    s.thickness_cm*=inv; s.fuzz_color=Mul(s.fuzz_color,inv); s.fuzz_amount*=inv;
    s.fuzz_roughness*=inv; s.thin_film_weight*=inv; s.thin_film_thickness_nm*=inv;
    s.thin_film_ior*=inv; s.normal=Mul(s.normal,inv); s.normal_strength*=inv;
    s.tangent=NormalizeOr(s.tangent,FVec3{1,0,0});
    s.normal=NormalizeOr(s.normal,FVec3{0,0,1});
    lobes[0].slab=s; lobes[0].weight=1.0f; lobes[0].coverage=Saturate01(coverage);
    lobes[0].vertical_depth=min_depth;
    lobes[0].bsdf_mode=ESubstrateBsdfMode::StandardSlab;
    lobes[0].source_node=kShaderExpressionInvalidNode;
    count=1u;
}

bool ValidateUnitColor(FVec3 c) noexcept {
    return Finite(c) && c.x >= 0.0f && c.x <= 1.0f &&
           c.y >= 0.0f && c.y <= 1.0f &&
           c.z >= 0.0f && c.z <= 1.0f;
}

ESubstrateCompileError ValidateSlab(const FSubstrateSlab& s) noexcept {
    if (!Finite(s.diffuse_albedo) || !Finite(s.f0) || !Finite(s.f90) ||
        !Finite(s.tangent) || !Finite(s.mean_free_path_cm) ||
        !Finite(s.emissive) || !Finite(s.transmittance) ||
        !Finite(s.fuzz_color) || !Finite(s.normal) ||
        !Finite(s.roughness) || !Finite(s.second_roughness) ||
        !Finite(s.second_roughness_weight) || !Finite(s.anisotropy) ||
        !Finite(s.phase_anisotropy) || !Finite(s.thickness_cm) ||
        !Finite(s.fuzz_amount) || !Finite(s.fuzz_roughness) ||
        !Finite(s.thin_film_weight) || !Finite(s.thin_film_thickness_nm) ||
        !Finite(s.thin_film_ior) || !Finite(s.normal_strength)) {
        return ESubstrateCompileError::NonFiniteValue;
    }
    if (!ValidateUnitColor(s.diffuse_albedo) || !ValidateUnitColor(s.f0) ||
        !ValidateUnitColor(s.f90) || !ValidateUnitColor(s.transmittance) ||
        !ValidateUnitColor(s.fuzz_color) ||
        s.roughness < 0.0f || s.roughness > 1.0f ||
        s.second_roughness < 0.0f || s.second_roughness > 1.0f ||
        s.second_roughness_weight < 0.0f || s.second_roughness_weight > 1.0f ||
        s.anisotropy < -1.0f || s.anisotropy > 1.0f ||
        s.mean_free_path_cm.x < 0.0f || s.mean_free_path_cm.y < 0.0f ||
        s.mean_free_path_cm.z < 0.0f ||
        s.phase_anisotropy < -0.99f || s.phase_anisotropy > 0.99f ||
        s.emissive.x < 0.0f || s.emissive.y < 0.0f || s.emissive.z < 0.0f ||
        s.thickness_cm < 0.0f || s.fuzz_amount < 0.0f || s.fuzz_amount > 1.0f ||
        s.fuzz_roughness < 0.0f || s.fuzz_roughness > 1.0f ||
        s.thin_film_weight < 0.0f || s.thin_film_weight > 1.0f ||
        s.thin_film_thickness_nm < 0.0f || s.thin_film_ior < 1.0f ||
        s.normal_strength < 0.0f || s.normal_strength > 4.0f) {
        return ESubstrateCompileError::ValueOutOfRange;
    }
    const f32 tangent_len2 = s.tangent.x * s.tangent.x +
                             s.tangent.y * s.tangent.y +
                             s.tangent.z * s.tangent.z;
    const f32 normal_len2 = s.normal.x * s.normal.x +
                            s.normal.y * s.normal.y +
                            s.normal.z * s.normal.z;
    return tangent_len2 > kSubstrateEpsilon && normal_len2 > kSubstrateEpsilon
        ? ESubstrateCompileError::None
        : ESubstrateCompileError::ValueOutOfRange;
}

struct FCompileContext {
    const FSubstrateMaterial& material;
    FSubstrateCompileResult& result;
    u8 state[kSubstrateMaxNodes]{}; // 0=unseen, 1=visiting, 2=done

    bool Fail(ESubstrateCompileError error, i32 node) noexcept {
        if (result.error == ESubstrateCompileError::None) {
            result.error = error;
            result.error_node = node;
        }
        return false;
    }

    bool Append(const FSubstrateCompiledLobe& lobe,
                FSubstrateCompiledLobe* out, u32& count, i32 node) noexcept {
        if (count >= kSubstrateMaxClosures) {
            return Fail(ESubstrateCompileError::ClosureOverflow, node);
        }
        out[count++] = lobe;
        return true;
    }

    bool Visit(i32 index, FSubstrateCompiledLobe* out, u32& out_count) noexcept {
        if (index < 0 || static_cast<u32>(index) >= material.node_count) {
            return Fail(ESubstrateCompileError::InvalidInput, index);
        }
        const u32 uindex = static_cast<u32>(index);
        if (state[uindex] == 1u) return Fail(ESubstrateCompileError::Cycle, index);

        // A DAG node can be referenced more than once.  Re-evaluate it because
        // the fixed graph is tiny and the caller needs a distinct closure copy.
        state[uindex] = 1u;
        const FSubstrateNode& node = material.nodes[uindex];
        const u32 raw_type = static_cast<u32>(node.type);
        if (raw_type > static_cast<u32>(ESubstrateNodeType::Select)) {
            return Fail(ESubstrateCompileError::InvalidNodeType, index);
        }
        if (!Finite(node.factor)) return Fail(ESubstrateCompileError::NonFiniteValue, index);

        if (node.type == ESubstrateNodeType::Slab) {
            if (node.input_a != kSubstrateInvalidNode ||
                node.input_b != kSubstrateInvalidNode) {
                return Fail(ESubstrateCompileError::UnexpectedInput, index);
            }
            const ESubstrateCompileError slab_error = ValidateSlab(node.slab);
            if (slab_error != ESubstrateCompileError::None) return Fail(slab_error, index);
            FSubstrateCompiledLobe lobe{};
            lobe.slab = node.slab;
            const u32 mode = (node.flags & SubstrateNodeFlag_BsdfModeMask)
                           >> SubstrateNodeFlag_BsdfModeShift;
            if (mode > static_cast<u32>(ESubstrateBsdfMode::SingleLayerWater)) {
                return Fail(ESubstrateCompileError::ValueOutOfRange, index);
            }
            lobe.bsdf_mode = static_cast<ESubstrateBsdfMode>(mode);
            lobe.source_node = static_cast<i16>(index);
            const bool appended = Append(lobe, out, out_count, index);
            state[uindex] = 2u;
            return appended;
        }

        const bool unit_factor = node.type == ESubstrateNodeType::CoverageWeight ||
                                 node.type == ESubstrateNodeType::HorizontalBlend ||
                                 node.type == ESubstrateNodeType::Select;
        if (node.factor < 0.0f || (unit_factor && node.factor > 1.0f)) {
            return Fail(ESubstrateCompileError::ValueOutOfRange, index);
        }
        if (node.input_a < 0 || static_cast<u32>(node.input_a) >= material.node_count) {
            return Fail(ESubstrateCompileError::InvalidInput, index);
        }

        FSubstrateCompiledLobe a[kSubstrateMaxClosures]{};
        u32 a_count = 0u;
        if (!Visit(node.input_a, a, a_count)) return false;

        if (node.type == ESubstrateNodeType::CoverageWeight) {
            if (node.input_b != kSubstrateInvalidNode) {
                return Fail(ESubstrateCompileError::UnexpectedInput, index);
            }
            for (u32 i = 0; i < a_count; ++i) {
                a[i].coverage *= node.factor;
                if (!Append(a[i], out, out_count, index)) return false;
            }
            state[uindex] = 2u;
            return true;
        }

        if (node.input_b < 0 || static_cast<u32>(node.input_b) >= material.node_count) {
            return Fail(ESubstrateCompileError::InvalidInput, index);
        }
        FSubstrateCompiledLobe b[kSubstrateMaxClosures]{};
        u32 b_count = 0u;
        if (!Visit(node.input_b, b, b_count)) return false;

        if (node.type == ESubstrateNodeType::Select) {
            const FSubstrateCompiledLobe* selected = node.factor < 0.5f ? a : b;
            const u32 selected_count = node.factor < 0.5f ? a_count : b_count;
            for (u32 i = 0; i < selected_count; ++i)
                if (!Append(selected[i], out, out_count, index)) return false;
        } else if (node.type == ESubstrateNodeType::HorizontalBlend) {
            for (u32 i = 0; i < a_count; ++i) {
                a[i].weight *= 1.0f - node.factor;
                if (a[i].weight > kSubstrateEpsilon && !Append(a[i], out, out_count, index)) return false;
            }
            for (u32 i = 0; i < b_count; ++i) {
                b[i].weight *= node.factor;
                if (b[i].weight > kSubstrateEpsilon && !Append(b[i], out, out_count, index)) return false;
            }
        } else if (node.type == ESubstrateNodeType::VerticalLayer) {
            // A is the top slab, B the bottom.  Coverage controls presence;
            // medium transmittance controls how much bottom energy survives.
            f32 top_coverage = 0.0f;
            f32 top_transmission = 0.0f;
            f32 top_norm = 0.0f;
            for (u32 i = 0; i < a_count; ++i) {
                const f32 w = a[i].weight * a[i].coverage;
                top_coverage += w;
                top_transmission += w * Saturate01(
                    Luminance(SlabNormalTransmittance(a[i].slab, node.factor)));
                top_norm += w;
                if (!Append(a[i], out, out_count, index)) return false;
            }
            top_coverage = Saturate01(top_coverage);
            top_transmission = top_norm > kSubstrateEpsilon ? Saturate01(top_transmission / top_norm) : 1.0f;
            const f32 bottom_throughput =
                (1.0f - top_coverage) + top_coverage * top_transmission;
            for (u32 i = 0; i < b_count; ++i) {
                b[i].weight *= bottom_throughput;
                ++b[i].vertical_depth;
                if (b[i].weight > kSubstrateEpsilon && !Append(b[i], out, out_count, index)) return false;
            }
            result.stats.feature_bits |= SubstrateFeature_VerticalLayer;
        } else { // Add: explicitly non-energy-conserving artistic operator.
            for (u32 i = 0; i < a_count; ++i)
                if (!Append(a[i], out, out_count, index)) return false;
            for (u32 i = 0; i < b_count; ++i)
                if (!Append(b[i], out, out_count, index)) return false;
            result.stats.energy_conserving = false;
            result.stats.feature_bits |= SubstrateFeature_NonPhysicalAdd;
        }

        if ((node.flags & SubstrateNodeFlag_ParameterBlending) != 0u &&
            node.type != ESubstrateNodeType::Select) {
            CollapseLobes(out, out_count);
            result.stats.feature_bits |= SubstrateFeature_ParameterBlend;
        }
        state[uindex] = 2u;
        return true;
    }
};

void AccumulateFeatureBits(const FSubstrateSlab& s, u32& bits) noexcept {
    if (std::fabs(s.f90.x - 1.0f) > 1e-4f ||
        std::fabs(s.f90.y - 1.0f) > 1e-4f ||
        std::fabs(s.f90.z - 1.0f) > 1e-4f) bits |= SubstrateFeature_F90;
    if (s.second_roughness_weight > kSubstrateEpsilon) bits |= SubstrateFeature_SecondRoughness;
    if (std::fabs(s.anisotropy) > kSubstrateEpsilon) bits |= SubstrateFeature_Anisotropy;
    if (s.mean_free_path_cm.x > kSubstrateEpsilon || s.mean_free_path_cm.y > kSubstrateEpsilon ||
        s.mean_free_path_cm.z > kSubstrateEpsilon) bits |= SubstrateFeature_Subsurface;
    if (s.emissive.x > kSubstrateEpsilon || s.emissive.y > kSubstrateEpsilon ||
        s.emissive.z > kSubstrateEpsilon) bits |= SubstrateFeature_Emissive;
    if (s.transmittance.x > kSubstrateEpsilon || s.transmittance.y > kSubstrateEpsilon ||
        s.transmittance.z > kSubstrateEpsilon) bits |= SubstrateFeature_Transmission;
    if (s.fuzz_amount > kSubstrateEpsilon) bits |= SubstrateFeature_Fuzz;
    if (s.thin_film_weight > kSubstrateEpsilon) bits |= SubstrateFeature_ThinFilm;
    if (std::fabs(s.normal.x) > 1e-4f || std::fabs(s.normal.y) > 1e-4f ||
        std::fabs(s.normal.z - 1.0f) > 1e-4f) bits |= SubstrateFeature_CustomNormal;
}

u32 SlabScalarComponent(u32 scalar) noexcept {
    if ((scalar <= 8u) ||
        (scalar >= 13u && scalar <= 18u) ||
        (scalar >= 20u && scalar <= 25u) ||
        (scalar >= 27u && scalar <= 29u) ||
        (scalar >= 35u && scalar <= 37u)) {
        if (scalar <= 8u) return scalar % 3u;
        if (scalar <= 18u) return (scalar - 13u) % 3u;
        if (scalar <= 25u) return (scalar - 20u) % 3u;
        if (scalar <= 29u) return scalar - 27u;
        return scalar - 35u;
    }
    return 0u;
}

u32 ExpressionTypeWidth(EShaderExpressionValueType type) noexcept {
    switch (type) {
        case EShaderExpressionValueType::Float1: return 1u;
        case EShaderExpressionValueType::Float2: return 2u;
        case EShaderExpressionValueType::Float3: return 3u;
        case EShaderExpressionValueType::Float4: return 4u;
        default: return 0u;
    }
}

} // namespace

FSubstrateCompileResult CompileSubstrateMaterial(
    const FSubstrateMaterial& material) noexcept {
    FSubstrateCompileResult result{};
    if (material.node_count == 0u) {
        result.error = ESubstrateCompileError::EmptyGraph;
        return result;
    }
    if (material.node_count > kSubstrateMaxNodes) {
        result.error = ESubstrateCompileError::TooManyNodes;
        return result;
    }
    if (material.root < 0 || static_cast<u32>(material.root) >= material.node_count) {
        result.error = ESubstrateCompileError::InvalidRoot;
        result.error_node = material.root;
        return result;
    }

    FCompileContext context{material, result};
    if (!context.Visit(material.root, result.lobes, result.lobe_count)) return result;

    result.stats.closure_count = result.lobe_count;
    if (result.lobe_count > 1u) result.stats.feature_bits |= SubstrateFeature_MultipleClosure;
    for (u32 i = 0; i < result.lobe_count; ++i) {
        AccumulateFeatureBits(result.lobes[i].slab, result.stats.feature_bits);
        if (result.lobes[i].bsdf_mode == ESubstrateBsdfMode::SimpleClearCoat) {
            result.stats.feature_bits |= SubstrateFeature_ClearCoat;
        } else if (result.lobes[i].bsdf_mode == ESubstrateBsdfMode::SingleLayerWater) {
            result.stats.feature_bits |= SubstrateFeature_SingleLayerWater;
        }
    }

    const u32 single_features = SubstrateFeature_F90 | SubstrateFeature_SecondRoughness |
                                SubstrateFeature_Subsurface | SubstrateFeature_Fuzz |
                                SubstrateFeature_ThinFilm | SubstrateFeature_VerticalLayer |
                                SubstrateFeature_ClearCoat;
    const u32 complex_features = SubstrateFeature_Anisotropy |
                                 SubstrateFeature_MultipleClosure |
                                 SubstrateFeature_NonPhysicalAdd;
    const u32 complex_special_features = SubstrateFeature_SingleLayerWater;
    if ((result.stats.feature_bits & complex_special_features) != 0u) {
        result.stats.complexity = ESubstrateComplexity::ComplexSpecial;
    } else if ((result.stats.feature_bits & complex_features) != 0u) {
        result.stats.complexity = ESubstrateComplexity::Complex;
    } else if ((result.stats.feature_bits & single_features) != 0u) {
        result.stats.complexity = ESubstrateComplexity::Single;
    } else {
        result.stats.complexity = ESubstrateComplexity::Simple;
    }

    // 28-byte compact closure baseline plus feature payload.  This is an
    // estimate for editor budgeting, not an ABI/GBuffer byte guarantee.
    result.stats.estimated_bytes_per_pixel = result.lobe_count * 28u;
    if ((result.stats.feature_bits & (SubstrateFeature_F90 |
                                     SubstrateFeature_SecondRoughness)) != 0u)
        result.stats.estimated_bytes_per_pixel += result.lobe_count * 8u;
    if ((result.stats.feature_bits & (SubstrateFeature_Subsurface |
                                     SubstrateFeature_Transmission)) != 0u)
        result.stats.estimated_bytes_per_pixel += result.lobe_count * 12u;
    if ((result.stats.feature_bits & (SubstrateFeature_Anisotropy |
                                     SubstrateFeature_CustomNormal)) != 0u)
        result.stats.estimated_bytes_per_pixel += result.lobe_count * 8u;
    if ((result.stats.feature_bits & SubstrateFeature_SingleLayerWater) != 0u)
        result.stats.estimated_bytes_per_pixel += result.lobe_count * 16u;
    return result;
}

FSubstrateExpressionLinkResult CompileSubstrateExpressionLinks(
    const FSubstrateMaterial& material) noexcept {
    FSubstrateExpressionLinkResult linked{};
    if (material.node_count > kSubstrateMaxNodes) {
        linked.error =
            ESubstrateExpressionLinkError::SubstrateCompileFailed;
        return linked;
    }

    bool has_binding = false;
    for (u32 node_index = 0u;
         node_index < material.node_count && node_index < kSubstrateMaxNodes;
         ++node_index) {
        const FSubstrateNode& node = material.nodes[node_index];
        for (u32 scalar = 0u; scalar < kSubstrateSlabScalarCount; ++scalar) {
            const i16 root = node.expressions.roots[scalar];
            if (root == kShaderExpressionInvalidNode) continue;
            has_binding = true;
            if (node.type != ESubstrateNodeType::Slab) {
                linked.error = ESubstrateExpressionLinkError::BindingOnNonSlab;
                linked.error_node = static_cast<i16>(node_index);
                linked.error_scalar = static_cast<i16>(scalar);
                return linked;
            }
            if (root < 0 ||
                static_cast<u32>(root) >= material.expression_graph.node_count) {
                linked.error = ESubstrateExpressionLinkError::InvalidBindingRoot;
                linked.error_node = static_cast<i16>(node_index);
                linked.error_scalar = static_cast<i16>(scalar);
                return linked;
            }
        }
    }

    if (material.expression_graph.node_count == 0u) {
        if (material.expression_graph.root !=
            kShaderExpressionInvalidNode) {
            linked.error =
                ESubstrateExpressionLinkError::InvalidBindingRoot;
            linked.error_node = material.expression_graph.root;
            return linked;
        }
        if (has_binding) {
            linked.error = ESubstrateExpressionLinkError::InvalidBindingRoot;
        }
        return linked;
    }

    // The current forward path evaluates all 39 dynamic inputs exactly for a
    // direct Slab. Blends/layers require evaluating the complete closure DAG
    // per pixel (not a linearized resolved surface), so reject them explicitly.
    if (has_binding) {
        if (material.root < 0 ||
            static_cast<u32>(material.root) >= material.node_count ||
            material.nodes[static_cast<u32>(material.root)].type !=
                ESubstrateNodeType::Slab) {
            linked.error =
                ESubstrateExpressionLinkError::UnsupportedDynamicTopology;
            linked.error_node = static_cast<i16>(material.root);
            return linked;
        }
        for (u32 node_index = 0u;
             node_index < material.node_count;
             ++node_index) {
            if (static_cast<i32>(node_index) == material.root) continue;
            for (u32 scalar = 0u;
                 scalar < kSubstrateSlabScalarCount;
                 ++scalar) {
                if (material.nodes[node_index].expressions.roots[scalar] !=
                    kShaderExpressionInvalidNode) {
                    linked.error =
                        ESubstrateExpressionLinkError::
                            UnsupportedDynamicTopology;
                    linked.error_node = static_cast<i16>(node_index);
                    linked.error_scalar = static_cast<i16>(scalar);
                    return linked;
                }
            }
        }
    }

    linked.expression_program =
        CompileShaderExpressionGraph(material.expression_graph);
    if (!linked.expression_program.Succeeded()) {
        linked.error = ESubstrateExpressionLinkError::ExpressionCompileFailed;
        if (linked.expression_program.diagnostic_count > 0u) {
            linked.expression_diagnostic =
                linked.expression_program.diagnostics[0];
            linked.error_node = linked.expression_diagnostic.node;
        }
        return linked;
    }

    // Validate every authored link, including links on currently unreachable
    // Slabs, so later graph rewiring cannot expose a latent type error.
    for (u32 node_index = 0u; node_index < material.node_count; ++node_index) {
        const FSubstrateNode& node = material.nodes[node_index];
        for (u32 scalar = 0u; scalar < kSubstrateSlabScalarCount; ++scalar) {
            const i16 root = node.expressions.roots[scalar];
            if (root == kShaderExpressionInvalidNode) continue;
            if (node.type != ESubstrateNodeType::Slab ||
                root < 0 ||
                static_cast<u32>(root) >= material.expression_graph.node_count) {
                linked.error = node.type != ESubstrateNodeType::Slab
                    ? ESubstrateExpressionLinkError::BindingOnNonSlab
                    : ESubstrateExpressionLinkError::InvalidBindingRoot;
                linked.error_node = static_cast<i16>(node_index);
                linked.error_scalar = static_cast<i16>(scalar);
                return linked;
            }
            const EShaderExpressionValueType type =
                linked.expression_program.original_types[static_cast<u32>(root)];
            const u32 width = ExpressionTypeWidth(type);
            const u32 lane = SlabScalarComponent(scalar);
            if (width == 0u || (width != 1u && lane >= width)) {
                linked.error =
                    ESubstrateExpressionLinkError::BindingTypeMismatch;
                linked.error_node = static_cast<i16>(node_index);
                linked.error_scalar = static_cast<i16>(scalar);
                linked.expression_diagnostic.error =
                    EShaderExpressionError::TypeMismatch;
                linked.expression_diagnostic.node = root;
                linked.expression_diagnostic.expected =
                    lane == 0u ? EShaderExpressionValueType::Float1
                               : EShaderExpressionValueType::Float3;
                linked.expression_diagnostic.actual = type;
                return linked;
            }
        }
    }

    // Parameter blending intentionally collapses source identity.  Recompile
    // a topology-equivalent expanded graph so dynamic inputs retain exact
    // per-lobe coefficients for the forward GPU resolver.
    FSubstrateMaterial expanded = material;
    for (u32 i = 0u; i < expanded.node_count; ++i) {
        expanded.nodes[i].flags &= ~SubstrateNodeFlag_ParameterBlending;
    }
    const FSubstrateCompileResult closures =
        CompileSubstrateMaterial(expanded);
    if (!closures.Succeeded() || closures.lobe_count == 0u) {
        linked.error = ESubstrateExpressionLinkError::SubstrateCompileFailed;
        linked.error_node = static_cast<i16>(closures.error_node);
        return linked;
    }

    f32 base_total = 0.0f;
    f32 coat_total = 0.0f;
    for (u32 i = 0u; i < closures.lobe_count; ++i) {
        const FSubstrateCompiledLobe& lobe = closures.lobes[i];
        const f32 weight = lobe.weight * lobe.coverage;
        if (weight <= kSubstrateEpsilon) continue;
        if (lobe.bsdf_mode == ESubstrateBsdfMode::SimpleClearCoat &&
            closures.lobe_count > 1u) {
            coat_total += weight;
        } else {
            base_total += weight;
        }
    }
    if (base_total <= kSubstrateEpsilon) {
        linked.error = ESubstrateExpressionLinkError::SubstrateCompileFailed;
        return linked;
    }

    for (u32 lobe_index = 0u; lobe_index < closures.lobe_count; ++lobe_index) {
        const FSubstrateCompiledLobe& lobe = closures.lobes[lobe_index];
        if (lobe.source_node < 0 ||
            static_cast<u32>(lobe.source_node) >= material.node_count) {
            continue;
        }
        const f32 weight = lobe.weight * lobe.coverage;
        if (weight <= kSubstrateEpsilon) continue;
        const bool coat =
            lobe.bsdf_mode == ESubstrateBsdfMode::SimpleClearCoat &&
            closures.lobe_count > 1u;
        const f32 denominator = coat ? coat_total : base_total;
        if (denominator <= kSubstrateEpsilon) continue;
        const f32 coefficient = weight / denominator;
        const FSubstrateNode& source =
            material.nodes[static_cast<u32>(lobe.source_node)];
        f32 literals[kSubstrateSlabScalarCount]{};
        EncodeSubstrateSlab(source.slab, literals);

        for (u32 scalar = 0u; scalar < kSubstrateSlabScalarCount; ++scalar) {
            const i16 root = source.expressions.roots[scalar];
            if (root == kShaderExpressionInvalidNode) continue;

            u32 target = scalar;
            if (coat) {
                if (scalar >= 3u && scalar <= 5u) {
                    target = kSubstrateSlabScalarCount + (scalar - 3u);
                } else if (scalar == 9u) {
                    target = static_cast<u32>(
                        ESubstrateResolvedExpressionScalar::CoatRoughness);
                } else {
                    continue;
                }
            }

            const i16 instruction =
                linked.expression_program
                    .original_to_instruction[static_cast<u32>(root)];
            if (instruction < 0 ||
                static_cast<u32>(instruction) >=
                    linked.expression_program.instruction_count) {
                linked.error =
                    ESubstrateExpressionLinkError::InvalidBindingRoot;
                linked.error_node = lobe.source_node;
                linked.error_scalar = static_cast<i16>(scalar);
                return linked;
            }
            const u32 width = ExpressionTypeWidth(
                linked.expression_program
                    .original_types[static_cast<u32>(root)]);
            const u32 scalar_lane = SlabScalarComponent(scalar);
            const u32 component = width == 1u ? 0u : scalar_lane;
            if (linked.binding_count >= kSubstrateMaxExpressionBindings) {
                linked.error = ESubstrateExpressionLinkError::BindingOverflow;
                linked.error_node = lobe.source_node;
                linked.error_scalar = static_cast<i16>(scalar);
                return linked;
            }
            FSubstrateExpressionBinding& binding =
                linked.bindings[linked.binding_count++];
            binding.metadata = PackSubstrateExpressionBindingMetadata(
                target, static_cast<u32>(instruction), component);
            binding.coefficient = coefficient;
            binding.authored_literal = literals[scalar];
        }
    }
    return linked;
}

bool ResolveSubstrateMaterial(const FSubstrateMaterial& material,
                              FSubstrateResolvedSurface& out,
                              FSubstrateCompileStats* out_stats) noexcept {
    const FSubstrateCompileResult compiled = CompileSubstrateMaterial(material);
    if (!compiled.Succeeded() || compiled.lobe_count == 0u) return false;
    if (out_stats) *out_stats = compiled.stats;

    FSubstrateResolvedSurface resolved{};
    resolved.diffuse_albedo = FVec3{0, 0, 0};
    resolved.f0 = FVec3{0, 0, 0};
    resolved.f90 = FVec3{0, 0, 0};
    resolved.tangent = FVec3{0, 0, 0};
    resolved.mean_free_path_cm = FVec3{0, 0, 0};
    resolved.emissive = FVec3{0, 0, 0};
    resolved.transmittance = FVec3{0, 0, 0};
    resolved.fuzz_color = FVec3{0, 0, 0};
    resolved.normal = FVec3{0, 0, 0};
    resolved.coat_f0 = FVec3{0, 0, 0};
    resolved.roughness = resolved.second_roughness =
        resolved.second_roughness_weight = resolved.anisotropy =
        resolved.phase_anisotropy = resolved.thickness_cm =
        resolved.fuzz_amount = resolved.fuzz_roughness =
        resolved.thin_film_weight = resolved.thin_film_thickness_nm =
        resolved.thin_film_ior = resolved.normal_strength = resolved.coverage =
        resolved.coat_weight = resolved.coat_roughness = 0.0f;

    f32 total = 0.0f;
    f32 coat_total = 0.0f;
    for (u32 i = 0; i < compiled.lobe_count; ++i) {
        const FSubstrateCompiledLobe& l = compiled.lobes[i];
        const f32 w = l.weight * l.coverage;
        if (w <= kSubstrateEpsilon) continue;
        if (l.bsdf_mode == ESubstrateBsdfMode::SimpleClearCoat &&
            compiled.lobe_count > 1u) {
            coat_total += w;
            resolved.coat_f0 = Add(resolved.coat_f0, Mul(l.slab.f0, w));
            resolved.coat_roughness += l.slab.roughness * w;
            continue;
        }
        total += w;
        resolved.diffuse_albedo = Add(resolved.diffuse_albedo, Mul(l.slab.diffuse_albedo, w));
        resolved.f0 = Add(resolved.f0, Mul(l.slab.f0, w));
        resolved.f90 = Add(resolved.f90, Mul(l.slab.f90, w));
        resolved.roughness += l.slab.roughness * w;
        resolved.second_roughness += l.slab.second_roughness * w;
        resolved.second_roughness_weight += l.slab.second_roughness_weight * w;
        resolved.anisotropy += l.slab.anisotropy * w;
        resolved.tangent = Add(resolved.tangent, Mul(l.slab.tangent, w));
        resolved.mean_free_path_cm = Add(resolved.mean_free_path_cm,
                                         Mul(l.slab.mean_free_path_cm, w));
        resolved.phase_anisotropy += l.slab.phase_anisotropy * w;
        resolved.emissive = Add(resolved.emissive, Mul(l.slab.emissive, w));
        resolved.transmittance = Add(resolved.transmittance, Mul(l.slab.transmittance, w));
        resolved.thickness_cm += l.slab.thickness_cm * w;
        resolved.fuzz_color = Add(resolved.fuzz_color, Mul(l.slab.fuzz_color, w));
        resolved.fuzz_amount += l.slab.fuzz_amount * w;
        resolved.fuzz_roughness += l.slab.fuzz_roughness * w;
        resolved.thin_film_weight += l.slab.thin_film_weight * w;
        resolved.thin_film_thickness_nm += l.slab.thin_film_thickness_nm * w;
        resolved.thin_film_ior += l.slab.thin_film_ior * w;
        resolved.normal = Add(resolved.normal, Mul(l.slab.normal, w));
        resolved.normal_strength += l.slab.normal_strength * w;
        resolved.coverage += l.coverage * l.weight;
    }
    if (total <= kSubstrateEpsilon) return false;
    const f32 inv = 1.0f / total;
    resolved.diffuse_albedo = Mul(resolved.diffuse_albedo, inv);
    resolved.f0 = Mul(resolved.f0, inv);
    resolved.f90 = Mul(resolved.f90, inv);
    resolved.roughness *= inv;
    resolved.second_roughness *= inv;
    resolved.second_roughness_weight *= inv;
    resolved.anisotropy *= inv;
    resolved.tangent = Mul(resolved.tangent, inv);
    resolved.mean_free_path_cm = Mul(resolved.mean_free_path_cm, inv);
    resolved.phase_anisotropy *= inv;
    resolved.emissive = Mul(resolved.emissive, inv);
    resolved.transmittance = Mul(resolved.transmittance, inv);
    resolved.thickness_cm *= inv;
    resolved.fuzz_color = Mul(resolved.fuzz_color, inv);
    resolved.fuzz_amount *= inv;
    resolved.fuzz_roughness *= inv;
    resolved.thin_film_weight *= inv;
    resolved.thin_film_thickness_nm *= inv;
    resolved.thin_film_ior *= inv;
    resolved.normal = Mul(resolved.normal, inv);
    resolved.normal_strength *= inv;
    resolved.tangent = NormalizeOr(resolved.tangent, FVec3{1,0,0});
    resolved.normal = NormalizeOr(resolved.normal, FVec3{0,0,1});
    resolved.coverage = Saturate01(resolved.coverage);
    if (coat_total > kSubstrateEpsilon) {
        resolved.coat_f0 = Mul(resolved.coat_f0, 1.0f / coat_total);
        resolved.coat_roughness /= coat_total;
        resolved.coat_weight = Saturate01(coat_total);
    } else {
        resolved.coat_f0 = FVec3{0.04f,0.04f,0.04f};
        resolved.coat_roughness = 0.1f;
        resolved.coat_weight = 0.0f;
    }
    out = resolved;
    return true;
}

const char* SubstrateCompileErrorName(ESubstrateCompileError error) noexcept {
    switch (error) {
        case ESubstrateCompileError::None: return "None";
        case ESubstrateCompileError::EmptyGraph: return "EmptyGraph";
        case ESubstrateCompileError::TooManyNodes: return "TooManyNodes";
        case ESubstrateCompileError::InvalidRoot: return "InvalidRoot";
        case ESubstrateCompileError::InvalidNodeType: return "InvalidNodeType";
        case ESubstrateCompileError::InvalidInput: return "InvalidInput";
        case ESubstrateCompileError::UnexpectedInput: return "UnexpectedInput";
        case ESubstrateCompileError::Cycle: return "Cycle";
        case ESubstrateCompileError::NonFiniteValue: return "NonFiniteValue";
        case ESubstrateCompileError::ValueOutOfRange: return "ValueOutOfRange";
        case ESubstrateCompileError::ClosureOverflow: return "ClosureOverflow";
    }
    return "Unknown";
}

const char* SubstrateNodeTypeName(ESubstrateNodeType type) noexcept {
    switch (type) {
        case ESubstrateNodeType::Slab: return "Slab";
        case ESubstrateNodeType::CoverageWeight: return "CoverageWeight";
        case ESubstrateNodeType::HorizontalBlend: return "HorizontalBlend";
        case ESubstrateNodeType::VerticalLayer: return "VerticalLayer";
        case ESubstrateNodeType::Add: return "Add";
        case ESubstrateNodeType::Select: return "Select";
    }
    return "Invalid";
}

const char* SubstrateComplexityName(ESubstrateComplexity complexity) noexcept {
    switch (complexity) {
        case ESubstrateComplexity::Simple: return "Simple";
        case ESubstrateComplexity::Single: return "Single";
        case ESubstrateComplexity::Complex: return "Complex";
        case ESubstrateComplexity::ComplexSpecial: return "ComplexSpecial";
    }
    return "Invalid";
}

const char* SubstrateExpressionLinkErrorName(
    ESubstrateExpressionLinkError error) noexcept {
    switch (error) {
        case ESubstrateExpressionLinkError::None: return "None";
        case ESubstrateExpressionLinkError::ExpressionCompileFailed:
            return "ExpressionCompileFailed";
        case ESubstrateExpressionLinkError::SubstrateCompileFailed:
            return "SubstrateCompileFailed";
        case ESubstrateExpressionLinkError::BindingOnNonSlab:
            return "BindingOnNonSlab";
        case ESubstrateExpressionLinkError::InvalidBindingRoot:
            return "InvalidBindingRoot";
        case ESubstrateExpressionLinkError::BindingTypeMismatch:
            return "BindingTypeMismatch";
        case ESubstrateExpressionLinkError::BindingOverflow:
            return "BindingOverflow";
        case ESubstrateExpressionLinkError::UnsupportedDynamicTopology:
            return "UnsupportedDynamicTopology";
    }
    return "Unknown";
}

void EncodeSubstrateSlab(const FSubstrateSlab& s,
                         f32 out[kSubstrateSlabScalarCount]) noexcept {
    if (!out) return;
    out[0]=s.diffuse_albedo.x; out[1]=s.diffuse_albedo.y; out[2]=s.diffuse_albedo.z;
    out[3]=s.f0.x; out[4]=s.f0.y; out[5]=s.f0.z;
    out[6]=s.f90.x; out[7]=s.f90.y; out[8]=s.f90.z;
    out[9]=s.roughness; out[10]=s.second_roughness; out[11]=s.second_roughness_weight;
    out[12]=s.anisotropy;
    out[13]=s.tangent.x; out[14]=s.tangent.y; out[15]=s.tangent.z;
    out[16]=s.mean_free_path_cm.x; out[17]=s.mean_free_path_cm.y; out[18]=s.mean_free_path_cm.z;
    out[19]=s.phase_anisotropy;
    out[20]=s.emissive.x; out[21]=s.emissive.y; out[22]=s.emissive.z;
    out[23]=s.transmittance.x; out[24]=s.transmittance.y; out[25]=s.transmittance.z;
    out[26]=s.thickness_cm;
    out[27]=s.fuzz_color.x; out[28]=s.fuzz_color.y; out[29]=s.fuzz_color.z;
    out[30]=s.fuzz_amount; out[31]=s.fuzz_roughness;
    out[32]=s.thin_film_weight; out[33]=s.thin_film_thickness_nm; out[34]=s.thin_film_ior;
    out[35]=s.normal.x; out[36]=s.normal.y; out[37]=s.normal.z;
    out[38]=s.normal_strength;
}

bool DecodeSubstrateSlab(const f32 values[kSubstrateSlabScalarCount],
                         FSubstrateSlab& out) noexcept {
    if (!values) return false;
    FSubstrateSlab s{};
    s.diffuse_albedo={values[0],values[1],values[2]};
    s.f0={values[3],values[4],values[5]};
    s.f90={values[6],values[7],values[8]};
    s.roughness=values[9]; s.second_roughness=values[10]; s.second_roughness_weight=values[11];
    s.anisotropy=values[12]; s.tangent={values[13],values[14],values[15]};
    s.mean_free_path_cm={values[16],values[17],values[18]}; s.phase_anisotropy=values[19];
    s.emissive={values[20],values[21],values[22]};
    s.transmittance={values[23],values[24],values[25]}; s.thickness_cm=values[26];
    s.fuzz_color={values[27],values[28],values[29]};
    s.fuzz_amount=values[30]; s.fuzz_roughness=values[31];
    s.thin_film_weight=values[32]; s.thin_film_thickness_nm=values[33]; s.thin_film_ior=values[34];
    s.normal={values[35],values[36],values[37]}; s.normal_strength=values[38];
    if (ValidateSlab(s) != ESubstrateCompileError::None) return false;
    out = s;
    return true;
}

FVec3 SubstrateIorToF0(f32 ior) noexcept {
    if (!Finite(ior) || ior < 1.0f) ior = 1.0f;
    const f32 d = (ior - 1.0f) / (ior + 1.0f);
    const f32 f0 = Saturate01(d * d);
    return FVec3{f0, f0, f0};
}

FVec3 SubstrateTransmittanceToMeanFreePath(FVec3 t, f32 thickness_cm) noexcept {
    if (!Finite(t) || !Finite(thickness_cm) || thickness_cm <= 0.0f) return FVec3{0,0,0};
    auto channel = [thickness_cm](f32 v) noexcept {
        v = v < 1.0e-6f ? 1.0e-6f : (v > 0.999999f ? 0.999999f : v);
        return -thickness_cm / std::log(v);
    };
    return FVec3{channel(t.x), channel(t.y), channel(t.z)};
}

} // namespace acs

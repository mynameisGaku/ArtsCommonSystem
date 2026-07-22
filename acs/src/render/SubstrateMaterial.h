// SPDX-License-Identifier: Apache-2.0
// Principled slab material graph used by ACS runtime and material editor.
//
// This is an independent implementation of the public "slab of matter"
// authoring model.  It deliberately stores a small, bounded DAG so assets are
// deterministic, cheap to validate, and safe to pass across the editor ABI.
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "render/SubstrateExpression.h"

namespace acs {

inline constexpr u32 kSubstrateMaxNodes = 32u;
inline constexpr u32 kSubstrateMaxClosures = 32u;
inline constexpr u32 kSubstrateSlabScalarCount = 39u;
inline constexpr i32 kSubstrateInvalidNode = -1;
inline constexpr u32 kSubstrateResolvedExpressionScalarCount = 43u;
inline constexpr u32 kSubstrateMaxExpressionBindings =
    kSubstrateMaxNodes * kSubstrateSlabScalarCount;

/** A graph node.  Operator inputs always reference an earlier or later node by index. */
enum class ESubstrateNodeType : u8 {
    Slab = 0,
    CoverageWeight,
    HorizontalBlend,
    VerticalLayer,
    Add,
    Select,
};

enum ESubstrateNodeFlag : u32 {
    /** Merge the operator result into one parameter-blended closure. */
    SubstrateNodeFlag_ParameterBlending = 1u << 0u,

    /** Optional editor/building-block classification stored on Slab nodes. */
    SubstrateNodeFlag_BsdfModeShift = 8u,
    SubstrateNodeFlag_BsdfModeMask = 0xFu << SubstrateNodeFlag_BsdfModeShift,
};

enum class ESubstrateBsdfMode : u8 {
    StandardSlab = 0,
    SimpleClearCoat,
    Unlit,
    SingleLayerWater,
};

/** Stable validation/compile diagnostics used by runtime, tests, and editor ABI. */
enum class ESubstrateCompileError : u8 {
    None = 0,
    EmptyGraph,
    TooManyNodes,
    InvalidRoot,
    InvalidNodeType,
    InvalidInput,
    UnexpectedInput,
    Cycle,
    NonFiniteValue,
    ValueOutOfRange,
    ClosureOverflow,
};

/** Evaluation-cost groups mirror the public Substrate complexity vocabulary. */
enum class ESubstrateComplexity : u8 {
    Simple = 0,
    Single,
    Complex,
    ComplexSpecial,
};

enum ESubstrateFeature : u32 {
    SubstrateFeature_None             = 0u,
    SubstrateFeature_F90              = 1u << 0u,
    SubstrateFeature_SecondRoughness  = 1u << 1u,
    SubstrateFeature_Anisotropy       = 1u << 2u,
    SubstrateFeature_Subsurface       = 1u << 3u,
    SubstrateFeature_Emissive         = 1u << 4u,
    SubstrateFeature_Transmission     = 1u << 5u,
    SubstrateFeature_Fuzz             = 1u << 6u,
    SubstrateFeature_ThinFilm         = 1u << 7u,
    SubstrateFeature_CustomNormal     = 1u << 8u,
    SubstrateFeature_MultipleClosure  = 1u << 9u,
    SubstrateFeature_VerticalLayer    = 1u << 10u,
    SubstrateFeature_NonPhysicalAdd   = 1u << 11u,
    SubstrateFeature_ParameterBlend   = 1u << 12u,
    SubstrateFeature_ClearCoat        = 1u << 13u,
    /** Dedicated water BSDF path; reported separately from generic transmission. */
    SubstrateFeature_SingleLayerWater = 1u << 14u,
};

/**
 * Principled BSDF slab: an interface (F0/F90, roughness, anisotropy, normal)
 * over a participating medium (diffuse albedo, MFP, phase and thickness).
 *
 * Units:
 * - mean_free_path_cm and thickness_cm are centimeters.
 * - thin_film_thickness_nm is nanometers.
 * - colors are linear RGB.
 */
struct FSubstrateSlab {
    FVec3 diffuse_albedo{0.8f, 0.8f, 0.8f};
    FVec3 f0{0.04f, 0.04f, 0.04f};
    FVec3 f90{1.0f, 1.0f, 1.0f};

    f32 roughness = 0.5f;
    f32 second_roughness = 0.5f;
    f32 second_roughness_weight = 0.0f;

    f32 anisotropy = 0.0f;
    FVec3 tangent{1.0f, 0.0f, 0.0f};

    FVec3 mean_free_path_cm{0.0f, 0.0f, 0.0f};
    f32 phase_anisotropy = 0.0f;

    FVec3 emissive{0.0f, 0.0f, 0.0f};

    /** Target normal-incidence transmittance through thickness_cm. */
    FVec3 transmittance{0.0f, 0.0f, 0.0f};
    f32 thickness_cm = 0.01f;

    FVec3 fuzz_color{1.0f, 1.0f, 1.0f};
    f32 fuzz_amount = 0.0f;
    f32 fuzz_roughness = 0.5f;

    f32 thin_film_weight = 0.0f;
    f32 thin_film_thickness_nm = 400.0f;
    f32 thin_film_ior = 1.4f;

    /** Tangent-space normal.  {0,0,1} is neutral. */
    FVec3 normal{0.0f, 0.0f, 1.0f};
    f32 normal_strength = 1.0f;
};

/**
 * Optional expression roots for the 39 stable slab scalars.  -1 keeps the
 * authored literal.  A scalar expression is broadcast; vector expressions
 * contribute the lane corresponding to the RGB/vector scalar being bound.
 */
struct FSubstrateSlabExpressionBindings {
    i16 roots[kSubstrateSlabScalarCount];

    FSubstrateSlabExpressionBindings() noexcept {
        for (u32 i = 0u; i < kSubstrateSlabScalarCount; ++i) {
            roots[i] = kShaderExpressionInvalidNode;
        }
    }
};

struct FSubstrateNode {
    ESubstrateNodeType type = ESubstrateNodeType::Slab;
    i32 input_a = kSubstrateInvalidNode;
    i32 input_b = kSubstrateInvalidNode;

    /**
     * CoverageWeight: coverage multiplier.
     * HorizontalBlend: foreground mix (A=background, B=foreground).
     * VerticalLayer: physical top-layer thickness in centimeters.
     * Add: reserved and serialized as 1.
     */
    f32 factor = 1.0f;
    u32 flags = 0u;
    FSubstrateSlab slab{};
    FSubstrateSlabExpressionBindings expressions{};
};

struct FSubstrateMaterial {
    bool enabled = false;
    i32 root = kSubstrateInvalidNode;
    u32 node_count = 0u;
    FSubstrateNode nodes[kSubstrateMaxNodes]{};
    /** Shared typed graph referenced by any number of slab scalar inputs. */
    FShaderExpressionGraph expression_graph{};
};

/** One compiled closure; operators alter weight/coverage without destroying slab data. */
struct FSubstrateCompiledLobe {
    FSubstrateSlab slab{};
    ESubstrateBsdfMode bsdf_mode = ESubstrateBsdfMode::StandardSlab;
    /** Original Slab node, or -1 after a destructive parameter blend. */
    i16 source_node = kShaderExpressionInvalidNode;
    u16 reserved = 0u;
    f32 weight = 1.0f;
    f32 coverage = 1.0f;
    u32 vertical_depth = 0u;
};

struct FSubstrateCompileStats {
    u32 feature_bits = SubstrateFeature_None;
    u32 closure_count = 0u;
    u32 estimated_bytes_per_pixel = 0u;
    ESubstrateComplexity complexity = ESubstrateComplexity::Simple;
    bool energy_conserving = true;
};

struct FSubstrateCompileResult {
    ESubstrateCompileError error = ESubstrateCompileError::None;
    i32 error_node = kSubstrateInvalidNode;
    FSubstrateCompileStats stats{};
    u32 lobe_count = 0u;
    FSubstrateCompiledLobe lobes[kSubstrateMaxClosures]{};

    bool Succeeded() const noexcept { return error == ESubstrateCompileError::None; }
};

/** Runtime targets 0..38 mirror EncodeSubstrateSlab; 39..42 are coat values. */
enum class ESubstrateResolvedExpressionScalar : u8 {
    CoatF0R = kSubstrateSlabScalarCount,
    CoatF0G,
    CoatF0B,
    CoatRoughness,
};

enum class ESubstrateExpressionLinkError : u8 {
    None = 0,
    ExpressionCompileFailed,
    SubstrateCompileFailed,
    BindingOnNonSlab,
    InvalidBindingRoot,
    BindingTypeMismatch,
    BindingOverflow,
    UnsupportedDynamicTopology,
};

/**
 * Padding-free 16-byte GPU binding.
 * metadata: target bits 0..7, instruction bits 8..15, component bits 16..17.
 * Runtime applies target += coefficient * (expression - authored_literal).
 */
struct alignas(16) FSubstrateExpressionBinding {
    u32 metadata = 0u;
    f32 coefficient = 0.0f;
    f32 authored_literal = 0.0f;
    u32 reserved = 0u;
};

struct FSubstrateExpressionLinkResult {
    ESubstrateExpressionLinkError error = ESubstrateExpressionLinkError::None;
    i16 error_node = kShaderExpressionInvalidNode;
    i16 error_scalar = kShaderExpressionInvalidNode;
    FShaderExpressionDiagnostic expression_diagnostic{};
    FShaderExpressionCompileResult expression_program{};
    u32 binding_count = 0u;
    FSubstrateExpressionBinding bindings[kSubstrateMaxExpressionBindings]{};

    bool Succeeded() const noexcept {
        return error == ESubstrateExpressionLinkError::None;
    }
};

/**
 * A single-surface approximation consumed by the existing forward PBR path.
 * The compiled lobes remain available for future adaptive-GBuffer evaluation;
 * this resolved form is deterministic parameter blending, not graph loss.
 */
struct FSubstrateResolvedSurface {
    FVec3 diffuse_albedo{0.8f, 0.8f, 0.8f};
    FVec3 f0{0.04f, 0.04f, 0.04f};
    FVec3 f90{1.0f, 1.0f, 1.0f};
    f32 roughness = 0.5f;
    f32 second_roughness = 0.5f;
    f32 second_roughness_weight = 0.0f;
    f32 anisotropy = 0.0f;
    FVec3 tangent{1.0f, 0.0f, 0.0f};
    FVec3 mean_free_path_cm{0.0f, 0.0f, 0.0f};
    f32 phase_anisotropy = 0.0f;
    FVec3 emissive{0.0f, 0.0f, 0.0f};
    FVec3 transmittance{0.0f, 0.0f, 0.0f};
    f32 thickness_cm = 0.01f;
    FVec3 fuzz_color{1.0f, 1.0f, 1.0f};
    f32 fuzz_amount = 0.0f;
    f32 fuzz_roughness = 0.5f;
    f32 thin_film_weight = 0.0f;
    f32 thin_film_thickness_nm = 400.0f;
    f32 thin_film_ior = 1.4f;
    FVec3 normal{0.0f, 0.0f, 1.0f};
    f32 normal_strength = 1.0f;
    f32 coverage = 1.0f;
    FVec3 coat_f0{0.04f,0.04f,0.04f};
    f32 coat_weight = 0.0f;
    f32 coat_roughness = 0.1f;
};

FSubstrateCompileResult CompileSubstrateMaterial(
    const FSubstrateMaterial& material) noexcept;

/**
 * Compile the shared expression graph and link each reachable Slab input to
 * the resolved forward-surface scalar it affects.
 */
FSubstrateExpressionLinkResult CompileSubstrateExpressionLinks(
    const FSubstrateMaterial& material) noexcept;

bool ResolveSubstrateMaterial(const FSubstrateMaterial& material,
                              FSubstrateResolvedSurface& out,
                              FSubstrateCompileStats* out_stats = nullptr) noexcept;

const char* SubstrateCompileErrorName(ESubstrateCompileError error) noexcept;
const char* SubstrateNodeTypeName(ESubstrateNodeType type) noexcept;
const char* SubstrateComplexityName(ESubstrateComplexity complexity) noexcept;
const char* SubstrateExpressionLinkErrorName(
    ESubstrateExpressionLinkError error) noexcept;

constexpr u32 PackSubstrateExpressionBindingMetadata(
    u32 target, u32 instruction, u32 component) noexcept {
    return (target & 0xFFu) |
           ((instruction & 0xFFu) << 8u) |
           ((component & 0x3u) << 16u);
}

constexpr u32 SubstrateExpressionBindingTarget(
    const FSubstrateExpressionBinding& binding) noexcept {
    return binding.metadata & 0xFFu;
}

constexpr u32 SubstrateExpressionBindingInstruction(
    const FSubstrateExpressionBinding& binding) noexcept {
    return (binding.metadata >> 8u) & 0xFFu;
}

constexpr u32 SubstrateExpressionBindingComponent(
    const FSubstrateExpressionBinding& binding) noexcept {
    return (binding.metadata >> 16u) & 0x3u;
}

/** Stable flat representation used by ACSMAT and the C ABI. */
void EncodeSubstrateSlab(const FSubstrateSlab& slab,
                         f32 out_values[kSubstrateSlabScalarCount]) noexcept;
bool DecodeSubstrateSlab(const f32 values[kSubstrateSlabScalarCount],
                         FSubstrateSlab& out) noexcept;

/** Helpers matching the public building-block parameterization. */
FVec3 SubstrateIorToF0(f32 ior) noexcept;
FVec3 SubstrateTransmittanceToMeanFreePath(FVec3 transmittance,
                                           f32 thickness_cm) noexcept;

static_assert(sizeof(FSubstrateExpressionBinding) == 16u);

} // namespace acs

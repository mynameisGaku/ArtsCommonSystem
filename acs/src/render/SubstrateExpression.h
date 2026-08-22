// SPDX-License-Identifier: Apache-2.0
// Bounded, typed expression graph shared by Substrate runtime and editor preview.
#pragma once

#include "foundation/TypeTraits.h"

#include <cstddef>

namespace acs {

inline constexpr u32 kShaderExpressionMaxNodes = 64u;
inline constexpr u32 kShaderExpressionMaxTextureSlots = 4u;
inline constexpr u32 kShaderExpressionMaxParameters = 32u;
inline constexpr u32 kShaderExpressionMaxDiagnostics = 64u;
inline constexpr i16 kShaderExpressionInvalidNode = -1;

/**
 * Expression value width. Invalid means "infer it" on authoring nodes.
 *
 * Coercion is deliberately small and predictable:
 * - Float1 can be splatted to Float2, Float3, or Float4.
 * - Equal vector widths are compatible.
 * - No implicit truncation or conversion between different vector widths.
 */
enum class EShaderExpressionValueType : u8 {
    Invalid = 0,
    Float1,
    Float2,
    Float3,
    Float4,
};

/**
 * Stable opcode values used by serialized graphs and GPU programs.
 * Do not reorder existing values.
 */
enum class EShaderExpressionOp : u8 {
    Constant = 0,
    ScalarParameter,
    VectorParameter,
    TextureSample2D,
    UV0,
    Time,
    WorldPosition,
    WorldNormal,
    Add,
    Multiply,
    Lerp,
    Clamp,
    Power,
    Dot,
    Normalize,
    Noise,
    /** Extracts one scalar lane selected by component_index. */
    Component,
};

enum EShaderExpressionTextureFlag : u8 {
    ShaderExpressionTextureFlag_None = 0u,
    ShaderExpressionTextureFlag_LinearFilter = 1u << 0u,
    ShaderExpressionTextureFlag_ClampU = 1u << 1u,
    ShaderExpressionTextureFlag_ClampV = 1u << 2u,
    ShaderExpressionTextureFlag_Srgb = 1u << 3u,
};

inline constexpr u8 kShaderExpressionTextureFlagMask =
    ShaderExpressionTextureFlag_LinearFilter |
    ShaderExpressionTextureFlag_ClampU |
    ShaderExpressionTextureFlag_ClampV |
    ShaderExpressionTextureFlag_Srgb;

/** Stable compile/evaluation diagnostics. */
enum class EShaderExpressionError : u8 {
    None = 0,
    EmptyGraph,
    TooManyNodes,
    InvalidRoot,
    InvalidOpcode,
    InvalidDeclaredType,
    MissingInput,
    UnexpectedInput,
    InvalidInputIndex,
    Cycle,
    NonFiniteValue,
    ValueOutOfRange,
    TextureSlotOutOfRange,
    InvalidTextureFlags,
    ParameterTypeConflict,
    TextureSlotConflict,
    TypeMismatch,
    InvalidCompiledProgram,
    EvaluationNodeUnavailable,
    NonFiniteContextValue,
    TooManyParameters,
};

/** ABI-safe float value. Unused lanes are canonicalized to zero by the compiler. */
struct FShaderExpressionValue {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 w = 0.0f;

    constexpr FShaderExpressionValue() noexcept = default;
    constexpr explicit FShaderExpressionValue(f32 x_) noexcept
        : x(x_), y(0.0f), z(0.0f), w(0.0f) {}
    constexpr FShaderExpressionValue(f32 x_, f32 y_) noexcept
        : x(x_), y(y_), z(0.0f), w(0.0f) {}
    constexpr FShaderExpressionValue(f32 x_, f32 y_, f32 z_, f32 w_ = 0.0f) noexcept
        : x(x_), y(y_), z(z_), w(w_) {}
};

/**
 * Authoring node. Parameter names are represented by the stable 32-bit ID from
 * ShaderExpressionParameterId(). Texture IDs are opaque 64-bit asset IDs split
 * into two words to keep the struct padding-free.
 *
 * Source and operator types:
 * - Constant: declared_type, or Float1 when Invalid.
 * - ScalarParameter: Float1.
 * - VectorParameter: declared Float2..Float4, or Float4 when Invalid.
 * - TextureSample2D: Float4, input 0 must be Float2 UV.
 * - UV0/Time/WorldPosition/WorldNormal: Float2/Float1/Float3/Float3.
 * - Add/Multiply/Power: same width or scalar splat.
 * - Lerp/Clamp: all inputs share a width after scalar splat.
 * - Dot: equal widths, returns Float1.
 * - Normalize: preserves its input width.
 * - Noise: accepts Float1..Float4, returns Float1.
 * - Component: extracts x/y/z/w when that lane exists, returns Float1.
 */
struct FShaderExpressionNode {
    EShaderExpressionOp op = EShaderExpressionOp::Constant;
    EShaderExpressionValueType declared_type = EShaderExpressionValueType::Invalid;
    u8 texture_slot = 0u;
    u8 texture_flags = ShaderExpressionTextureFlag_None;
    i16 inputs[3]{
        kShaderExpressionInvalidNode,
        kShaderExpressionInvalidNode,
        kShaderExpressionInvalidNode,
    };
    /** Component: 0=x/r, 1=y/g, 2=z/b, 3=w/a. */
    u8 component_index = 0u;
    u8 reserved = 0u;
    u32 parameter_id = 0u;
    u32 texture_asset_id_low = 0u;
    u32 texture_asset_id_high = 0u;
    /** Constant, parameter default, or texture fallback RGBA. */
    FShaderExpressionValue value{};
};

/**
 * A graph can be compiled with root == -1 when several external Substrate
 * fields independently reference nodes. Otherwise root is the editor preview
 * output. Every node, including disconnected nodes, is validated and compiled.
 */
struct FShaderExpressionGraph {
    i16 root = kShaderExpressionInvalidNode;
    u16 node_count = 0u;
    FShaderExpressionNode nodes[kShaderExpressionMaxNodes]{};
};

struct FShaderExpressionDiagnostic {
    EShaderExpressionError error = EShaderExpressionError::None;
    i16 node = kShaderExpressionInvalidNode;
    i8 input = -1;
    EShaderExpressionValueType expected = EShaderExpressionValueType::Invalid;
    EShaderExpressionValueType actual = EShaderExpressionValueType::Invalid;
    u8 reserved[2]{};
};

/**
 * Compact GPU/C-ABI instruction (48 bytes, 12 32-bit words).
 *
 * Byte/word layout:
 *   word 0: op in bits 0..7, value type in bits 8..15
 *   word 1: signed input 0 in bits 0..15, signed input 1 in bits 16..31
 *   word 2: signed input 2 in bits 0..15, original node in bits 16..31
 *   word 3: parameter ID
 *   word 4: texture slot in bits 0..7, texture flags in bits 8..15;
 *           Component uses bits 0..7 as its source lane
 *   word 5..6: opaque texture asset ID, low then high
 *   word 7..10: literal/default/fallback float4
 *   word 11: zero (reserved)
 *
 * Inputs are instruction indices and always point backward. A GPU interpreter
 * can therefore evaluate the array in one forward pass into float4 registers.
 * Scalar inputs are splatted according to the source instruction's type.
 * The HLSL declarations/semantic helpers returned by
 * ShaderExpressionHlslHelpers() match this exact layout.
 */
struct alignas(16) FShaderExpressionInstruction {
    u32 op_and_type = 0u;
    i16 inputs[3]{
        kShaderExpressionInvalidNode,
        kShaderExpressionInvalidNode,
        kShaderExpressionInvalidNode,
    };
    u16 original_node = 0u;
    u32 parameter_id = 0u;
    u32 texture_metadata = 0u;
    u32 texture_asset_id_low = 0u;
    u32 texture_asset_id_high = 0u;
    FShaderExpressionValue value{};
    u32 reserved = 0u;
};

struct FShaderExpressionCompileResult {
    u16 instruction_count = 0u;
    i16 root_instruction = kShaderExpressionInvalidNode;
    u16 constant_fold_count = 0u;
    u16 diagnostic_count = 0u;
    i16 original_to_instruction[kShaderExpressionMaxNodes]{};
    EShaderExpressionValueType original_types[kShaderExpressionMaxNodes]{};
    FShaderExpressionInstruction instructions[kShaderExpressionMaxNodes]{};
    FShaderExpressionDiagnostic diagnostics[kShaderExpressionMaxDiagnostics]{};

    bool Succeeded() const noexcept { return diagnostic_count == 0u; }
};

struct FShaderExpressionParameter {
    u32 id = 0u;
    EShaderExpressionValueType type = EShaderExpressionValueType::Invalid;
    u8 reserved[3]{};
    FShaderExpressionValue value{};
};

using FShaderExpressionTextureSampler = bool (*)(
    void* user,
    u8 texture_slot,
    u64 texture_asset_id,
    u8 texture_flags,
    FShaderExpressionValue uv,
    FShaderExpressionValue* out_rgba);

struct FShaderExpressionEvaluationContext {
    FShaderExpressionValue uv0{};
    f32 time = 0.0f;
    f32 reserved0[3]{};
    FShaderExpressionValue world_position{};
    FShaderExpressionValue world_normal{0.0f, 0.0f, 1.0f};
    const FShaderExpressionParameter* parameters = nullptr;
    u32 parameter_count = 0u;
    FShaderExpressionTextureSampler texture_sampler = nullptr;
    void* texture_user = nullptr;
};

constexpr u32 PackShaderExpressionOpAndType(
    EShaderExpressionOp op,
    EShaderExpressionValueType type) noexcept {
    return static_cast<u32>(op) | (static_cast<u32>(type) << 8u);
}

constexpr EShaderExpressionOp ShaderExpressionInstructionOp(
    const FShaderExpressionInstruction& instruction) noexcept {
    return static_cast<EShaderExpressionOp>(instruction.op_and_type & 0xFFu);
}

constexpr EShaderExpressionValueType ShaderExpressionInstructionType(
    const FShaderExpressionInstruction& instruction) noexcept {
    return static_cast<EShaderExpressionValueType>(
        (instruction.op_and_type >> 8u) & 0xFFu);
}

constexpr u32 PackShaderExpressionTextureMetadata(u8 slot, u8 flags) noexcept {
    return static_cast<u32>(slot) | (static_cast<u32>(flags) << 8u);
}

constexpr u8 ShaderExpressionInstructionTextureSlot(
    const FShaderExpressionInstruction& instruction) noexcept {
    return static_cast<u8>(instruction.texture_metadata & 0xFFu);
}

constexpr u8 ShaderExpressionInstructionTextureFlags(
    const FShaderExpressionInstruction& instruction) noexcept {
    return static_cast<u8>((instruction.texture_metadata >> 8u) & 0xFFu);
}

constexpr u8 ShaderExpressionInstructionComponent(
    const FShaderExpressionInstruction& instruction) noexcept {
    return static_cast<u8>(instruction.texture_metadata & 0xFFu);
}

constexpr u64 ShaderExpressionTextureAssetId(
    const FShaderExpressionInstruction& instruction) noexcept {
    return static_cast<u64>(instruction.texture_asset_id_low) |
           (static_cast<u64>(instruction.texture_asset_id_high) << 32u);
}

FShaderExpressionCompileResult CompileShaderExpressionGraph(
    const FShaderExpressionGraph& graph) noexcept;

bool EvaluateShaderExpression(
    const FShaderExpressionCompileResult& program,
    i16 original_node,
    const FShaderExpressionEvaluationContext& context,
    FShaderExpressionValue& out_value,
    FShaderExpressionDiagnostic* out_diagnostic = nullptr) noexcept;

bool EvaluateShaderExpressionRoot(
    const FShaderExpressionCompileResult& program,
    const FShaderExpressionEvaluationContext& context,
    FShaderExpressionValue& out_value,
    FShaderExpressionDiagnostic* out_diagnostic = nullptr) noexcept;

/** FNV-1a ID used for parameter names in assets and the editor ABI. */
u32 ShaderExpressionParameterId(const char* utf8_name) noexcept;

/** Hashes the logical program fields, independent of compiler padding. */
u64 HashCompiledShaderExpression(
    const FShaderExpressionCompileResult& program) noexcept;

const char* ShaderExpressionErrorName(EShaderExpressionError error) noexcept;
const char* ShaderExpressionOpName(EShaderExpressionOp op) noexcept;
const char* ShaderExpressionValueTypeName(
    EShaderExpressionValueType type) noexcept;

/**
 * Returns an immutable HLSL source fragment defining AcsExprInstruction,
 * opcode constants, packed-input decoding, scalar broadcast, safe power,
 * normalization, and deterministic noise helpers. Resource/parameter binding
 * remains the caller's responsibility.
 */
const char* ShaderExpressionHlslHelpers() noexcept;

static_assert(sizeof(FShaderExpressionValue) == 16u);
static_assert(sizeof(FShaderExpressionNode) == 40u);
static_assert(offsetof(FShaderExpressionNode, parameter_id) == 12u);
static_assert(offsetof(FShaderExpressionNode, value) == 24u);
static_assert(sizeof(FShaderExpressionGraph) ==
              4u + sizeof(FShaderExpressionNode) *
                       kShaderExpressionMaxNodes);
static_assert(sizeof(FShaderExpressionInstruction) == 48u);
static_assert(offsetof(FShaderExpressionInstruction, op_and_type) == 0u);
static_assert(offsetof(FShaderExpressionInstruction, parameter_id) == 12u);
static_assert(offsetof(FShaderExpressionInstruction, texture_metadata) == 16u);
static_assert(offsetof(FShaderExpressionInstruction, value) == 28u);
static_assert(offsetof(FShaderExpressionInstruction, reserved) == 44u);
static_assert(IsStandardLayoutV<FShaderExpressionInstruction>);
static_assert(IsTriviallyCopyableV<FShaderExpressionInstruction>);
static_assert(IsStandardLayoutV<FShaderExpressionGraph>);
static_assert(IsTriviallyCopyableV<FShaderExpressionGraph>);

} // namespace acs

// SPDX-License-Identifier: Apache-2.0
#include "render/SubstrateExpression.h"

#include <cmath>
#include <cstring>

namespace acs {
namespace {

constexpr u32 kShaderExpressionOpcodeCount =
    static_cast<u32>(EShaderExpressionOp::Component) + 1u;
constexpr f64 kNormalizeEpsilon = 1.0e-20;
constexpr u64 kFnv64Offset = 14695981039346656037ull;
constexpr u64 kFnv64Prime = 1099511628211ull;

bool IsValidType(EShaderExpressionValueType type) noexcept {
    const u32 raw = static_cast<u32>(type);
    return raw >= static_cast<u32>(EShaderExpressionValueType::Float1) &&
           raw <= static_cast<u32>(EShaderExpressionValueType::Float4);
}

u32 ValueWidth(EShaderExpressionValueType type) noexcept {
    return IsValidType(type) ? static_cast<u32>(type) : 0u;
}

f32 ValueComponent(const FShaderExpressionValue& value, u32 index) noexcept {
    switch (index) {
    case 0u: return value.x;
    case 1u: return value.y;
    case 2u: return value.z;
    default: return value.w;
    }
}

void SetValueComponent(FShaderExpressionValue& value,
                       u32 index,
                       f32 component) noexcept {
    switch (index) {
    case 0u: value.x = component; break;
    case 1u: value.y = component; break;
    case 2u: value.z = component; break;
    default: value.w = component; break;
    }
}

bool IsFiniteValue(const FShaderExpressionValue& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}

FShaderExpressionValue CanonicalValue(
    FShaderExpressionValue value,
    EShaderExpressionValueType type) noexcept {
    const u32 width = ValueWidth(type);
    for (u32 lane = width; lane < 4u; ++lane) {
        SetValueComponent(value, lane, 0.0f);
    }
    return value;
}

FShaderExpressionValue BroadcastValue(
    const FShaderExpressionValue& value,
    EShaderExpressionValueType source_type,
    EShaderExpressionValueType target_type) noexcept {
    if (source_type == EShaderExpressionValueType::Float1 &&
        target_type != EShaderExpressionValueType::Float1) {
        return FShaderExpressionValue{value.x, value.x, value.x, value.x};
    }
    return CanonicalValue(value, target_type);
}

EShaderExpressionValueType CombineArithmeticTypes(
    EShaderExpressionValueType a,
    EShaderExpressionValueType b) noexcept {
    if (!IsValidType(a) || !IsValidType(b)) {
        return EShaderExpressionValueType::Invalid;
    }
    if (a == b) {
        return a;
    }
    if (a == EShaderExpressionValueType::Float1) {
        return b;
    }
    if (b == EShaderExpressionValueType::Float1) {
        return a;
    }
    return EShaderExpressionValueType::Invalid;
}

EShaderExpressionValueType CombineArithmeticTypes(
    EShaderExpressionValueType a,
    EShaderExpressionValueType b,
    EShaderExpressionValueType c) noexcept {
    return CombineArithmeticTypes(CombineArithmeticTypes(a, b), c);
}

u32 InputArity(EShaderExpressionOp op) noexcept {
    switch (op) {
    case EShaderExpressionOp::TextureSample2D:
    case EShaderExpressionOp::Normalize:
    case EShaderExpressionOp::Noise:
    case EShaderExpressionOp::Component:
        return 1u;
    case EShaderExpressionOp::Add:
    case EShaderExpressionOp::Multiply:
    case EShaderExpressionOp::Power:
    case EShaderExpressionOp::Dot:
        return 2u;
    case EShaderExpressionOp::Lerp:
    case EShaderExpressionOp::Clamp:
        return 3u;
    default:
        return 0u;
    }
}

void InitializeCompileResult(FShaderExpressionCompileResult& result) noexcept {
    result = {};
    result.root_instruction = kShaderExpressionInvalidNode;
    for (u32 node = 0u; node < kShaderExpressionMaxNodes; ++node) {
        result.original_to_instruction[node] = kShaderExpressionInvalidNode;
        result.original_types[node] = EShaderExpressionValueType::Invalid;
    }
}

void AddDiagnostic(FShaderExpressionCompileResult& result,
                   EShaderExpressionError error,
                   i16 node,
                   i8 input = -1,
                   EShaderExpressionValueType expected =
                       EShaderExpressionValueType::Invalid,
                   EShaderExpressionValueType actual =
                       EShaderExpressionValueType::Invalid) noexcept {
    if (result.diagnostic_count >= kShaderExpressionMaxDiagnostics) {
        return;
    }
    FShaderExpressionDiagnostic& diagnostic =
        result.diagnostics[result.diagnostic_count++];
    diagnostic.error = error;
    diagnostic.node = node;
    diagnostic.input = input;
    diagnostic.expected = expected;
    diagnostic.actual = actual;
}

EShaderExpressionValueType DeclaredSourceType(
    const FShaderExpressionNode& node) noexcept {
    switch (node.op) {
    case EShaderExpressionOp::Constant:
        return node.declared_type == EShaderExpressionValueType::Invalid
            ? EShaderExpressionValueType::Float1 : node.declared_type;
    case EShaderExpressionOp::ScalarParameter:
    case EShaderExpressionOp::Time:
        return EShaderExpressionValueType::Float1;
    case EShaderExpressionOp::VectorParameter:
        return node.declared_type == EShaderExpressionValueType::Invalid
            ? EShaderExpressionValueType::Float4 : node.declared_type;
    case EShaderExpressionOp::TextureSample2D:
        return EShaderExpressionValueType::Float4;
    case EShaderExpressionOp::UV0:
        return EShaderExpressionValueType::Float2;
    case EShaderExpressionOp::WorldPosition:
    case EShaderExpressionOp::WorldNormal:
        return EShaderExpressionValueType::Float3;
    default:
        return EShaderExpressionValueType::Invalid;
    }
}

bool ValidateDeclaredSourceType(const FShaderExpressionNode& node) noexcept {
    const EShaderExpressionValueType type = node.declared_type;
    switch (node.op) {
    case EShaderExpressionOp::Constant:
        return type == EShaderExpressionValueType::Invalid || IsValidType(type);
    case EShaderExpressionOp::ScalarParameter:
        return type == EShaderExpressionValueType::Invalid ||
               type == EShaderExpressionValueType::Float1;
    case EShaderExpressionOp::VectorParameter:
        return type == EShaderExpressionValueType::Invalid ||
               type == EShaderExpressionValueType::Float2 ||
               type == EShaderExpressionValueType::Float3 ||
               type == EShaderExpressionValueType::Float4;
    case EShaderExpressionOp::TextureSample2D:
        return type == EShaderExpressionValueType::Invalid ||
               type == EShaderExpressionValueType::Float4;
    case EShaderExpressionOp::UV0:
        return type == EShaderExpressionValueType::Invalid ||
               type == EShaderExpressionValueType::Float2;
    case EShaderExpressionOp::Time:
        return type == EShaderExpressionValueType::Invalid ||
               type == EShaderExpressionValueType::Float1;
    case EShaderExpressionOp::WorldPosition:
    case EShaderExpressionOp::WorldNormal:
        return type == EShaderExpressionValueType::Invalid ||
               type == EShaderExpressionValueType::Float3;
    default:
        return true;
    }
}

f32 ClampScalar(f32 value, f32 low, f32 high) noexcept {
    return value < low ? low : (value > high ? high : value);
}

f32 SafePower(f32 base, f32 exponent) noexcept {
    if (base == 0.0f) {
        if (exponent > 0.0f) {
            return 0.0f;
        }
        if (exponent == 0.0f) {
            return 1.0f;
        }
    }
    const f64 magnitude =
        std::fmax(std::fabs(static_cast<f64>(base)), 1.0e-6);
    const f64 bounded_exponent = ClampScalar(exponent, -32.0f, 32.0f);
    const f64 log_result = std::log2(magnitude) * bounded_exponent;
    return static_cast<f32>(
        std::exp2(std::fmax(-126.0, std::fmin(126.0, log_result))));
}

f32 ExpressionNoise(const FShaderExpressionValue& value,
                    EShaderExpressionValueType type) noexcept {
    static constexpr f64 kNoiseWeights[4]{
        12.9898, 78.233, 37.719, 19.913,
    };
    f64 projection = 0.0;
    for (u32 lane = 0u; lane < ValueWidth(type); ++lane) {
        projection += static_cast<f64>(ValueComponent(value, lane)) *
                      kNoiseWeights[lane];
    }
    const f64 raw = std::sin(projection) * 43758.5453123;
    return static_cast<f32>(raw - std::floor(raw));
}

bool EvaluateOperator(
    EShaderExpressionOp op,
    EShaderExpressionValueType output_type,
    const FShaderExpressionValue input_values[3],
    const EShaderExpressionValueType input_types[3],
    u8 component_index,
    FShaderExpressionValue& out_value) noexcept {
    const u32 width = ValueWidth(output_type);
    if (width == 0u) {
        return false;
    }

    const FShaderExpressionValue a =
        BroadcastValue(input_values[0], input_types[0], output_type);
    const FShaderExpressionValue b =
        BroadcastValue(input_values[1], input_types[1], output_type);
    const FShaderExpressionValue c =
        BroadcastValue(input_values[2], input_types[2], output_type);
    FShaderExpressionValue result{};

    switch (op) {
    case EShaderExpressionOp::Add:
        for (u32 lane = 0u; lane < width; ++lane) {
            SetValueComponent(result, lane,
                              ValueComponent(a, lane) + ValueComponent(b, lane));
        }
        break;
    case EShaderExpressionOp::Multiply:
        for (u32 lane = 0u; lane < width; ++lane) {
            SetValueComponent(result, lane,
                              ValueComponent(a, lane) * ValueComponent(b, lane));
        }
        break;
    case EShaderExpressionOp::Lerp:
        for (u32 lane = 0u; lane < width; ++lane) {
            const f32 alpha = ValueComponent(c, lane);
            SetValueComponent(
                result, lane,
                ValueComponent(a, lane) +
                    (ValueComponent(b, lane) - ValueComponent(a, lane)) * alpha);
        }
        break;
    case EShaderExpressionOp::Clamp:
        for (u32 lane = 0u; lane < width; ++lane) {
            SetValueComponent(
                result, lane,
                ClampScalar(ValueComponent(a, lane),
                            ValueComponent(b, lane),
                            ValueComponent(c, lane)));
        }
        break;
    case EShaderExpressionOp::Power:
        for (u32 lane = 0u; lane < width; ++lane) {
            SetValueComponent(
                result, lane,
                SafePower(ValueComponent(a, lane), ValueComponent(b, lane)));
        }
        break;
    case EShaderExpressionOp::Dot: {
        f64 dot = 0.0;
        const u32 input_width = ValueWidth(input_types[0]);
        for (u32 lane = 0u; lane < input_width; ++lane) {
            dot += static_cast<f64>(ValueComponent(input_values[0], lane)) *
                   static_cast<f64>(ValueComponent(input_values[1], lane));
        }
        result.x = static_cast<f32>(dot);
        break;
    }
    case EShaderExpressionOp::Normalize: {
        f64 length_squared = 0.0;
        for (u32 lane = 0u; lane < width; ++lane) {
            const f64 component = ValueComponent(input_values[0], lane);
            length_squared += component * component;
        }
        if (length_squared > kNormalizeEpsilon) {
            const f64 reciprocal_length = 1.0 / std::sqrt(length_squared);
            for (u32 lane = 0u; lane < width; ++lane) {
                SetValueComponent(
                    result, lane,
                    static_cast<f32>(
                        ValueComponent(input_values[0], lane) *
                        reciprocal_length));
            }
        }
        break;
    }
    case EShaderExpressionOp::Noise:
        result.x = ExpressionNoise(input_values[0], input_types[0]);
        break;
    case EShaderExpressionOp::Component:
        result.x = ValueComponent(input_values[0], component_index);
        break;
    default:
        return false;
    }

    result = CanonicalValue(result, output_type);
    if (!IsFiniteValue(result)) {
        return false;
    }
    out_value = result;
    return true;
}

struct FTopologicalSort {
    const FShaderExpressionGraph& graph;
    FShaderExpressionCompileResult& result;
    u8 state[kShaderExpressionMaxNodes]{};
    u16 order[kShaderExpressionMaxNodes]{};
    u16 order_count = 0u;

    bool Visit(u16 node_index) noexcept {
        if (state[node_index] == 2u) {
            return true;
        }
        if (state[node_index] == 1u) {
            AddDiagnostic(result, EShaderExpressionError::Cycle,
                          static_cast<i16>(node_index));
            return false;
        }

        state[node_index] = 1u;
        const FShaderExpressionNode& node = graph.nodes[node_index];
        const u32 arity = InputArity(node.op);
        for (u32 input = 0u; input < arity; ++input) {
            const i16 dependency = node.inputs[input];
            if (state[static_cast<u16>(dependency)] == 1u) {
                AddDiagnostic(result, EShaderExpressionError::Cycle,
                              static_cast<i16>(node_index),
                              static_cast<i8>(input));
                return false;
            }
            if (!Visit(static_cast<u16>(dependency))) {
                return false;
            }
        }
        state[node_index] = 2u;
        order[order_count++] = node_index;
        return true;
    }
};

bool ValidateGraphShape(const FShaderExpressionGraph& graph,
                        FShaderExpressionCompileResult& result) noexcept {
    if (graph.node_count == 0u) {
        AddDiagnostic(result, EShaderExpressionError::EmptyGraph,
                      kShaderExpressionInvalidNode);
        return false;
    }
    if (graph.node_count > kShaderExpressionMaxNodes) {
        AddDiagnostic(result, EShaderExpressionError::TooManyNodes,
                      kShaderExpressionInvalidNode);
        return false;
    }
    if (graph.root < kShaderExpressionInvalidNode ||
        graph.root >= static_cast<i16>(graph.node_count)) {
        AddDiagnostic(result, EShaderExpressionError::InvalidRoot, graph.root);
        return false;
    }

    u32 distinct_parameter_count = 0u;
    for (u16 node_index = 0u; node_index < graph.node_count; ++node_index) {
        const FShaderExpressionNode& node = graph.nodes[node_index];
        const u32 raw_op = static_cast<u32>(node.op);
        if (raw_op >= kShaderExpressionOpcodeCount) {
            AddDiagnostic(result, EShaderExpressionError::InvalidOpcode,
                          static_cast<i16>(node_index));
            return false;
        }
        const u32 raw_type = static_cast<u32>(node.declared_type);
        if (raw_type > static_cast<u32>(EShaderExpressionValueType::Float4) ||
            !ValidateDeclaredSourceType(node)) {
            AddDiagnostic(result, EShaderExpressionError::InvalidDeclaredType,
                          static_cast<i16>(node_index), -1,
                          DeclaredSourceType(node), node.declared_type);
            return false;
        }
        if (!IsFiniteValue(node.value)) {
            AddDiagnostic(result, EShaderExpressionError::NonFiniteValue,
                          static_cast<i16>(node_index));
            return false;
        }

        const u32 arity = InputArity(node.op);
        for (u32 input = 0u; input < 3u; ++input) {
            const i16 dependency = node.inputs[input];
            if (input < arity) {
                if (dependency == kShaderExpressionInvalidNode) {
                    AddDiagnostic(result, EShaderExpressionError::MissingInput,
                                  static_cast<i16>(node_index),
                                  static_cast<i8>(input));
                    return false;
                }
                if (dependency < 0 ||
                    dependency >= static_cast<i16>(graph.node_count)) {
                    AddDiagnostic(
                        result, EShaderExpressionError::InvalidInputIndex,
                        static_cast<i16>(node_index), static_cast<i8>(input));
                    return false;
                }
            } else if (dependency != kShaderExpressionInvalidNode) {
                AddDiagnostic(result, EShaderExpressionError::UnexpectedInput,
                              static_cast<i16>(node_index),
                              static_cast<i8>(input));
                return false;
            }
        }

        if (node.op == EShaderExpressionOp::TextureSample2D) {
            if (node.texture_slot >= kShaderExpressionMaxTextureSlots) {
                AddDiagnostic(
                    result, EShaderExpressionError::TextureSlotOutOfRange,
                    static_cast<i16>(node_index));
                return false;
            }
            if ((node.texture_flags &
                 static_cast<u8>(~kShaderExpressionTextureFlagMask)) != 0u) {
                AddDiagnostic(result,
                              EShaderExpressionError::InvalidTextureFlags,
                              static_cast<i16>(node_index));
                return false;
            }
        }
        if (node.op == EShaderExpressionOp::Component &&
            node.component_index >= 4u) {
            AddDiagnostic(
                result, EShaderExpressionError::ValueOutOfRange,
                static_cast<i16>(node_index), 0);
            return false;
        }

        if (node.op == EShaderExpressionOp::ScalarParameter ||
            node.op == EShaderExpressionOp::VectorParameter) {
            const EShaderExpressionValueType parameter_type =
                DeclaredSourceType(node);
            bool first_parameter_with_id = true;
            for (u16 previous = 0u; previous < node_index; ++previous) {
                const FShaderExpressionNode& other = graph.nodes[previous];
                if ((other.op != EShaderExpressionOp::ScalarParameter &&
                     other.op != EShaderExpressionOp::VectorParameter) ||
                    other.parameter_id != node.parameter_id) {
                    continue;
                }
                first_parameter_with_id = false;
                if (DeclaredSourceType(other) != parameter_type) {
                    AddDiagnostic(
                        result,
                        EShaderExpressionError::ParameterTypeConflict,
                        static_cast<i16>(node_index), -1,
                        DeclaredSourceType(other), parameter_type);
                    return false;
                }
            }
            if (first_parameter_with_id &&
                ++distinct_parameter_count >
                    kShaderExpressionMaxParameters) {
                AddDiagnostic(
                    result, EShaderExpressionError::TooManyParameters,
                    static_cast<i16>(node_index));
                return false;
            }
        }

        if (node.op == EShaderExpressionOp::TextureSample2D) {
            for (u16 previous = 0u; previous < node_index; ++previous) {
                const FShaderExpressionNode& other = graph.nodes[previous];
                if (other.op == EShaderExpressionOp::TextureSample2D &&
                    other.texture_slot == node.texture_slot &&
                    (other.texture_asset_id_low != node.texture_asset_id_low ||
                     other.texture_asset_id_high !=
                         node.texture_asset_id_high ||
                     other.texture_flags != node.texture_flags)) {
                    AddDiagnostic(
                        result, EShaderExpressionError::TextureSlotConflict,
                        static_cast<i16>(node_index));
                    return false;
                }
            }
        }
    }
    return true;
}

EShaderExpressionValueType InferNodeType(
    const FShaderExpressionGraph& graph,
    u16 node_index,
    const EShaderExpressionValueType types[kShaderExpressionMaxNodes],
    FShaderExpressionCompileResult& result) noexcept {
    const FShaderExpressionNode& node = graph.nodes[node_index];
    const auto input_type = [&node, &types](u32 input) noexcept {
        return types[static_cast<u16>(node.inputs[input])];
    };

    EShaderExpressionValueType inferred = DeclaredSourceType(node);
    switch (node.op) {
    case EShaderExpressionOp::TextureSample2D:
        if (input_type(0u) != EShaderExpressionValueType::Float2) {
            AddDiagnostic(
                result, EShaderExpressionError::TypeMismatch,
                static_cast<i16>(node_index), 0,
                EShaderExpressionValueType::Float2, input_type(0u));
            return EShaderExpressionValueType::Invalid;
        }
        inferred = EShaderExpressionValueType::Float4;
        break;
    case EShaderExpressionOp::Add:
    case EShaderExpressionOp::Multiply:
    case EShaderExpressionOp::Power:
        inferred =
            CombineArithmeticTypes(input_type(0u), input_type(1u));
        if (!IsValidType(inferred)) {
            AddDiagnostic(
                result, EShaderExpressionError::TypeMismatch,
                static_cast<i16>(node_index), 1, input_type(0u), input_type(1u));
            return EShaderExpressionValueType::Invalid;
        }
        break;
    case EShaderExpressionOp::Lerp:
    case EShaderExpressionOp::Clamp:
        inferred = CombineArithmeticTypes(
            input_type(0u), input_type(1u), input_type(2u));
        if (!IsValidType(inferred)) {
            AddDiagnostic(
                result, EShaderExpressionError::TypeMismatch,
                static_cast<i16>(node_index), 2,
                CombineArithmeticTypes(input_type(0u), input_type(1u)),
                input_type(2u));
            return EShaderExpressionValueType::Invalid;
        }
        break;
    case EShaderExpressionOp::Dot:
        if (input_type(0u) != input_type(1u) ||
            !IsValidType(input_type(0u))) {
            AddDiagnostic(
                result, EShaderExpressionError::TypeMismatch,
                static_cast<i16>(node_index), 1, input_type(0u), input_type(1u));
            return EShaderExpressionValueType::Invalid;
        }
        inferred = EShaderExpressionValueType::Float1;
        break;
    case EShaderExpressionOp::Normalize:
        inferred = input_type(0u);
        break;
    case EShaderExpressionOp::Noise:
        if (!IsValidType(input_type(0u))) {
            AddDiagnostic(
                result, EShaderExpressionError::TypeMismatch,
                static_cast<i16>(node_index), 0,
                EShaderExpressionValueType::Float1, input_type(0u));
            return EShaderExpressionValueType::Invalid;
        }
        inferred = EShaderExpressionValueType::Float1;
        break;
    case EShaderExpressionOp::Component:
        if (node.component_index >= ValueWidth(input_type(0u))) {
            AddDiagnostic(
                result, EShaderExpressionError::TypeMismatch,
                static_cast<i16>(node_index), 0,
                input_type(0u), EShaderExpressionValueType::Invalid);
            return EShaderExpressionValueType::Invalid;
        }
        inferred = EShaderExpressionValueType::Float1;
        break;
    default:
        break;
    }

    if (!IsValidType(inferred)) {
        AddDiagnostic(result, EShaderExpressionError::InvalidDeclaredType,
                      static_cast<i16>(node_index));
        return EShaderExpressionValueType::Invalid;
    }
    if (node.declared_type != EShaderExpressionValueType::Invalid &&
        node.declared_type != inferred) {
        AddDiagnostic(result, EShaderExpressionError::TypeMismatch,
                      static_cast<i16>(node_index), -1,
                      inferred, node.declared_type);
        return EShaderExpressionValueType::Invalid;
    }
    return inferred;
}

bool IsFoldableOperator(EShaderExpressionOp op) noexcept {
    return op == EShaderExpressionOp::Add ||
           op == EShaderExpressionOp::Multiply ||
           op == EShaderExpressionOp::Lerp ||
           op == EShaderExpressionOp::Clamp ||
           op == EShaderExpressionOp::Power ||
           op == EShaderExpressionOp::Dot ||
           op == EShaderExpressionOp::Normalize ||
           op == EShaderExpressionOp::Noise ||
           op == EShaderExpressionOp::Component;
}

void SetRuntimeDiagnostic(FShaderExpressionDiagnostic* diagnostic,
                          EShaderExpressionError error,
                          i16 node,
                          i8 input = -1) noexcept {
    if (diagnostic == nullptr) {
        return;
    }
    *diagnostic = {};
    diagnostic->error = error;
    diagnostic->node = node;
    diagnostic->input = input;
}

const FShaderExpressionParameter* FindParameter(
    const FShaderExpressionEvaluationContext& context,
    u32 id,
    EShaderExpressionValueType type) noexcept {
    if (context.parameters == nullptr ||
        context.parameter_count > kShaderExpressionMaxParameters) {
        return nullptr;
    }
    for (u32 parameter = 0u;
         parameter < context.parameter_count;
         ++parameter) {
        const FShaderExpressionParameter& candidate =
            context.parameters[parameter];
        if (candidate.id == id && candidate.type == type &&
            IsFiniteValue(candidate.value)) {
            return &candidate;
        }
    }
    return nullptr;
}

u64 HashByte(u64 hash, u8 value) noexcept {
    return (hash ^ static_cast<u64>(value)) * kFnv64Prime;
}

u64 HashU16(u64 hash, u16 value) noexcept {
    hash = HashByte(hash, static_cast<u8>(value & 0xFFu));
    return HashByte(hash, static_cast<u8>((value >> 8u) & 0xFFu));
}

u64 HashU32(u64 hash, u32 value) noexcept {
    hash = HashU16(hash, static_cast<u16>(value & 0xFFFFu));
    return HashU16(hash, static_cast<u16>((value >> 16u) & 0xFFFFu));
}

u32 FloatBits(f32 value) noexcept {
    u32 bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

} // namespace

FShaderExpressionCompileResult CompileShaderExpressionGraph(
    const FShaderExpressionGraph& graph) noexcept {
    FShaderExpressionCompileResult result{};
    InitializeCompileResult(result);
    if (!ValidateGraphShape(graph, result)) {
        return result;
    }

    FTopologicalSort sort{graph, result};
    for (u16 node = 0u; node < graph.node_count; ++node) {
        if (!sort.Visit(node)) {
            return result;
        }
    }

    EShaderExpressionValueType types[kShaderExpressionMaxNodes]{};
    for (u16 order_index = 0u;
         order_index < sort.order_count;
         ++order_index) {
        const u16 node_index = sort.order[order_index];
        const EShaderExpressionValueType inferred =
            InferNodeType(graph, node_index, types, result);
        if (!IsValidType(inferred)) {
            return result;
        }
        types[node_index] = inferred;
        result.original_types[node_index] = inferred;
    }

    bool constant[kShaderExpressionMaxNodes]{};
    FShaderExpressionValue constant_value[kShaderExpressionMaxNodes]{};
    for (u16 order_index = 0u;
         order_index < sort.order_count;
         ++order_index) {
        const u16 node_index = sort.order[order_index];
        const FShaderExpressionNode& node = graph.nodes[node_index];
        const EShaderExpressionValueType type = types[node_index];
        const u16 instruction_index = result.instruction_count++;
        result.original_to_instruction[node_index] =
            static_cast<i16>(instruction_index);

        FShaderExpressionInstruction& instruction =
            result.instructions[instruction_index];
        instruction = {};
        instruction.op_and_type =
            PackShaderExpressionOpAndType(node.op, type);
        instruction.inputs[0] = kShaderExpressionInvalidNode;
        instruction.inputs[1] = kShaderExpressionInvalidNode;
        instruction.inputs[2] = kShaderExpressionInvalidNode;
        instruction.original_node = node_index;

        const u32 arity = InputArity(node.op);
        for (u32 input = 0u; input < arity; ++input) {
            instruction.inputs[input] =
                result.original_to_instruction[
                    static_cast<u16>(node.inputs[input])];
        }

        if (node.op == EShaderExpressionOp::Constant ||
            node.op == EShaderExpressionOp::ScalarParameter ||
            node.op == EShaderExpressionOp::VectorParameter ||
            node.op == EShaderExpressionOp::TextureSample2D) {
            instruction.value = CanonicalValue(node.value, type);
        }
        if (node.op == EShaderExpressionOp::ScalarParameter ||
            node.op == EShaderExpressionOp::VectorParameter) {
            instruction.parameter_id = node.parameter_id;
        }
        if (node.op == EShaderExpressionOp::TextureSample2D) {
            instruction.texture_metadata =
                PackShaderExpressionTextureMetadata(
                    node.texture_slot, node.texture_flags);
            instruction.texture_asset_id_low = node.texture_asset_id_low;
            instruction.texture_asset_id_high = node.texture_asset_id_high;
        }
        if (node.op == EShaderExpressionOp::Component) {
            instruction.texture_metadata =
                static_cast<u32>(node.component_index);
        }

        if (node.op == EShaderExpressionOp::Constant) {
            constant[node_index] = true;
            constant_value[node_index] = instruction.value;
            continue;
        }

        bool can_fold = IsFoldableOperator(node.op);
        for (u32 input = 0u; input < arity && can_fold; ++input) {
            can_fold = constant[static_cast<u16>(node.inputs[input])];
        }
        if (!can_fold) {
            continue;
        }

        FShaderExpressionValue input_values[3]{};
        EShaderExpressionValueType input_types[3]{
            EShaderExpressionValueType::Invalid,
            EShaderExpressionValueType::Invalid,
            EShaderExpressionValueType::Invalid,
        };
        for (u32 input = 0u; input < arity; ++input) {
            const u16 dependency = static_cast<u16>(node.inputs[input]);
            input_values[input] = constant_value[dependency];
            input_types[input] = types[dependency];
        }

        FShaderExpressionValue folded{};
        if (!EvaluateOperator(
                node.op, type, input_values, input_types,
                node.component_index, folded)) {
            AddDiagnostic(result, EShaderExpressionError::NonFiniteValue,
                          static_cast<i16>(node_index));
            result.instruction_count = 0u;
            return result;
        }
        instruction.op_and_type = PackShaderExpressionOpAndType(
            EShaderExpressionOp::Constant, type);
        instruction.inputs[0] = kShaderExpressionInvalidNode;
        instruction.inputs[1] = kShaderExpressionInvalidNode;
        instruction.inputs[2] = kShaderExpressionInvalidNode;
        instruction.value = folded;
        constant[node_index] = true;
        constant_value[node_index] = folded;
        ++result.constant_fold_count;
    }

    if (graph.root != kShaderExpressionInvalidNode) {
        result.root_instruction =
            result.original_to_instruction[static_cast<u16>(graph.root)];
    }
    return result;
}

bool EvaluateShaderExpression(
    const FShaderExpressionCompileResult& program,
    i16 original_node,
    const FShaderExpressionEvaluationContext& context,
    FShaderExpressionValue& out_value,
    FShaderExpressionDiagnostic* out_diagnostic) noexcept {
    out_value = {};
    SetRuntimeDiagnostic(out_diagnostic, EShaderExpressionError::None,
                         kShaderExpressionInvalidNode);
    if (!program.Succeeded() ||
        program.instruction_count == 0u ||
        program.instruction_count > kShaderExpressionMaxNodes) {
        SetRuntimeDiagnostic(out_diagnostic,
                             EShaderExpressionError::InvalidCompiledProgram,
                             original_node);
        return false;
    }
    if (original_node < 0 ||
        original_node >= static_cast<i16>(kShaderExpressionMaxNodes)) {
        SetRuntimeDiagnostic(
            out_diagnostic,
            EShaderExpressionError::EvaluationNodeUnavailable,
            original_node);
        return false;
    }
    const i16 target_instruction =
        program.original_to_instruction[static_cast<u16>(original_node)];
    if (target_instruction < 0 ||
        target_instruction >= static_cast<i16>(program.instruction_count)) {
        SetRuntimeDiagnostic(
            out_diagnostic,
            EShaderExpressionError::EvaluationNodeUnavailable,
            original_node);
        return false;
    }

    // A material can carry several disconnected expression roots. Evaluate
    // only the dependency closure of the requested root so an unrelated
    // preview branch cannot affect this result.
    bool required[kShaderExpressionMaxNodes]{};
    required[static_cast<u16>(target_instruction)] = true;
    for (i32 instruction_index = target_instruction;
         instruction_index >= 0;
         --instruction_index) {
        if (!required[static_cast<u16>(instruction_index)]) {
            continue;
        }
        const FShaderExpressionInstruction& instruction =
            program.instructions[static_cast<u16>(instruction_index)];
        const EShaderExpressionOp op =
            ShaderExpressionInstructionOp(instruction);
        const u32 arity = InputArity(op);
        for (u32 input = 0u; input < arity; ++input) {
            const i16 dependency = instruction.inputs[input];
            if (dependency >= 0 && dependency < instruction_index) {
                required[static_cast<u16>(dependency)] = true;
            }
        }
    }

    FShaderExpressionValue registers[kShaderExpressionMaxNodes]{};
    for (u16 instruction_index = 0u;
         instruction_index <= static_cast<u16>(target_instruction);
         ++instruction_index) {
        if (!required[instruction_index]) {
            continue;
        }
        const FShaderExpressionInstruction& instruction =
            program.instructions[instruction_index];
        const EShaderExpressionOp op =
            ShaderExpressionInstructionOp(instruction);
        const EShaderExpressionValueType type =
            ShaderExpressionInstructionType(instruction);
        const u32 raw_op = static_cast<u32>(op);
        if (raw_op >= kShaderExpressionOpcodeCount || !IsValidType(type)) {
            SetRuntimeDiagnostic(
                out_diagnostic,
                EShaderExpressionError::InvalidCompiledProgram,
                static_cast<i16>(instruction.original_node));
            return false;
        }

        const u32 arity = InputArity(op);
        FShaderExpressionValue input_values[3]{};
        EShaderExpressionValueType input_types[3]{
            EShaderExpressionValueType::Invalid,
            EShaderExpressionValueType::Invalid,
            EShaderExpressionValueType::Invalid,
        };
        for (u32 input = 0u; input < arity; ++input) {
            const i16 dependency = instruction.inputs[input];
            if (dependency < 0 ||
                dependency >= static_cast<i16>(instruction_index)) {
                SetRuntimeDiagnostic(
                    out_diagnostic,
                    EShaderExpressionError::InvalidCompiledProgram,
                    static_cast<i16>(instruction.original_node),
                    static_cast<i8>(input));
                return false;
            }
            input_values[input] = registers[static_cast<u16>(dependency)];
            input_types[input] = ShaderExpressionInstructionType(
                program.instructions[static_cast<u16>(dependency)]);
        }

        FShaderExpressionValue value{};
        switch (op) {
        case EShaderExpressionOp::Constant:
            value = instruction.value;
            break;
        case EShaderExpressionOp::ScalarParameter:
        case EShaderExpressionOp::VectorParameter: {
            const FShaderExpressionParameter* parameter =
                FindParameter(context, instruction.parameter_id, type);
            value = parameter != nullptr
                ? CanonicalValue(parameter->value, type)
                : instruction.value;
            break;
        }
        case EShaderExpressionOp::TextureSample2D: {
            value = instruction.value;
            if (context.texture_sampler != nullptr) {
                FShaderExpressionValue sampled{};
                const bool sampled_ok = context.texture_sampler(
                    context.texture_user,
                    ShaderExpressionInstructionTextureSlot(instruction),
                    ShaderExpressionTextureAssetId(instruction),
                    ShaderExpressionInstructionTextureFlags(instruction),
                    input_values[0],
                    &sampled);
                if (sampled_ok && IsFiniteValue(sampled)) {
                    value = sampled;
                }
            }
            break;
        }
        case EShaderExpressionOp::UV0:
            value = CanonicalValue(
                context.uv0, EShaderExpressionValueType::Float2);
            break;
        case EShaderExpressionOp::Time:
            value = FShaderExpressionValue{context.time};
            break;
        case EShaderExpressionOp::WorldPosition:
            value = CanonicalValue(
                context.world_position,
                EShaderExpressionValueType::Float3);
            break;
        case EShaderExpressionOp::WorldNormal:
            value = CanonicalValue(
                context.world_normal,
                EShaderExpressionValueType::Float3);
            break;
        default:
            if (!EvaluateOperator(
                    op, type, input_values, input_types,
                    ShaderExpressionInstructionComponent(instruction),
                    value)) {
                SetRuntimeDiagnostic(
                    out_diagnostic,
                    EShaderExpressionError::NonFiniteContextValue,
                    static_cast<i16>(instruction.original_node));
                return false;
            }
            break;
        }
        value = CanonicalValue(value, type);
        if (!IsFiniteValue(value)) {
            SetRuntimeDiagnostic(
                out_diagnostic,
                EShaderExpressionError::NonFiniteContextValue,
                static_cast<i16>(instruction.original_node));
            return false;
        }
        registers[instruction_index] = value;
    }

    out_value = registers[static_cast<u16>(target_instruction)];
    return true;
}

bool EvaluateShaderExpressionRoot(
    const FShaderExpressionCompileResult& program,
    const FShaderExpressionEvaluationContext& context,
    FShaderExpressionValue& out_value,
    FShaderExpressionDiagnostic* out_diagnostic) noexcept {
    if (program.root_instruction < 0 ||
        program.root_instruction >=
            static_cast<i16>(program.instruction_count)) {
        out_value = {};
        SetRuntimeDiagnostic(
            out_diagnostic,
            EShaderExpressionError::EvaluationNodeUnavailable,
            kShaderExpressionInvalidNode);
        return false;
    }
    const u16 original =
        program.instructions[
            static_cast<u16>(program.root_instruction)].original_node;
    return EvaluateShaderExpression(
        program, static_cast<i16>(original), context,
        out_value, out_diagnostic);
}

u32 ShaderExpressionParameterId(const char* utf8_name) noexcept {
    constexpr u32 kFnv32Offset = 2166136261u;
    constexpr u32 kFnv32Prime = 16777619u;
    u32 hash = kFnv32Offset;
    if (utf8_name == nullptr) {
        return hash;
    }
    for (const char* cursor = utf8_name; *cursor != '\0'; ++cursor) {
        hash ^= static_cast<u8>(*cursor);
        hash *= kFnv32Prime;
    }
    return hash;
}

u64 HashCompiledShaderExpression(
    const FShaderExpressionCompileResult& program) noexcept {
    u64 hash = kFnv64Offset;
    hash = HashU16(hash, program.instruction_count);
    hash = HashU16(hash, static_cast<u16>(program.root_instruction));
    for (u16 index = 0u; index < program.instruction_count; ++index) {
        const FShaderExpressionInstruction& instruction =
            program.instructions[index];
        hash = HashU32(hash, instruction.op_and_type);
        hash = HashU16(hash, static_cast<u16>(instruction.inputs[0]));
        hash = HashU16(hash, static_cast<u16>(instruction.inputs[1]));
        hash = HashU16(hash, static_cast<u16>(instruction.inputs[2]));
        hash = HashU16(hash, instruction.original_node);
        hash = HashU32(hash, instruction.parameter_id);
        hash = HashU32(hash, instruction.texture_metadata);
        hash = HashU32(hash, instruction.texture_asset_id_low);
        hash = HashU32(hash, instruction.texture_asset_id_high);
        hash = HashU32(hash, FloatBits(instruction.value.x));
        hash = HashU32(hash, FloatBits(instruction.value.y));
        hash = HashU32(hash, FloatBits(instruction.value.z));
        hash = HashU32(hash, FloatBits(instruction.value.w));
        hash = HashU32(hash, instruction.reserved);
    }
    return hash;
}

const char* ShaderExpressionErrorName(
    EShaderExpressionError error) noexcept {
    switch (error) {
    case EShaderExpressionError::None: return "None";
    case EShaderExpressionError::EmptyGraph: return "EmptyGraph";
    case EShaderExpressionError::TooManyNodes: return "TooManyNodes";
    case EShaderExpressionError::InvalidRoot: return "InvalidRoot";
    case EShaderExpressionError::InvalidOpcode: return "InvalidOpcode";
    case EShaderExpressionError::InvalidDeclaredType:
        return "InvalidDeclaredType";
    case EShaderExpressionError::MissingInput: return "MissingInput";
    case EShaderExpressionError::UnexpectedInput: return "UnexpectedInput";
    case EShaderExpressionError::InvalidInputIndex: return "InvalidInputIndex";
    case EShaderExpressionError::Cycle: return "Cycle";
    case EShaderExpressionError::NonFiniteValue: return "NonFiniteValue";
    case EShaderExpressionError::ValueOutOfRange: return "ValueOutOfRange";
    case EShaderExpressionError::TextureSlotOutOfRange:
        return "TextureSlotOutOfRange";
    case EShaderExpressionError::InvalidTextureFlags:
        return "InvalidTextureFlags";
    case EShaderExpressionError::ParameterTypeConflict:
        return "ParameterTypeConflict";
    case EShaderExpressionError::TextureSlotConflict:
        return "TextureSlotConflict";
    case EShaderExpressionError::TypeMismatch: return "TypeMismatch";
    case EShaderExpressionError::InvalidCompiledProgram:
        return "InvalidCompiledProgram";
    case EShaderExpressionError::EvaluationNodeUnavailable:
        return "EvaluationNodeUnavailable";
    case EShaderExpressionError::NonFiniteContextValue:
        return "NonFiniteContextValue";
    case EShaderExpressionError::TooManyParameters:
        return "TooManyParameters";
    default: return "Unknown";
    }
}

const char* ShaderExpressionOpName(EShaderExpressionOp op) noexcept {
    switch (op) {
    case EShaderExpressionOp::Constant: return "Constant";
    case EShaderExpressionOp::ScalarParameter: return "ScalarParameter";
    case EShaderExpressionOp::VectorParameter: return "VectorParameter";
    case EShaderExpressionOp::TextureSample2D: return "TextureSample2D";
    case EShaderExpressionOp::UV0: return "UV0";
    case EShaderExpressionOp::Time: return "Time";
    case EShaderExpressionOp::WorldPosition: return "WorldPosition";
    case EShaderExpressionOp::WorldNormal: return "WorldNormal";
    case EShaderExpressionOp::Add: return "Add";
    case EShaderExpressionOp::Multiply: return "Multiply";
    case EShaderExpressionOp::Lerp: return "Lerp";
    case EShaderExpressionOp::Clamp: return "Clamp";
    case EShaderExpressionOp::Power: return "Power";
    case EShaderExpressionOp::Dot: return "Dot";
    case EShaderExpressionOp::Normalize: return "Normalize";
    case EShaderExpressionOp::Noise: return "Noise";
    case EShaderExpressionOp::Component: return "Component";
    default: return "Unknown";
    }
}

const char* ShaderExpressionValueTypeName(
    EShaderExpressionValueType type) noexcept {
    switch (type) {
    case EShaderExpressionValueType::Invalid: return "Invalid";
    case EShaderExpressionValueType::Float1: return "Float1";
    case EShaderExpressionValueType::Float2: return "Float2";
    case EShaderExpressionValueType::Float3: return "Float3";
    case EShaderExpressionValueType::Float4: return "Float4";
    default: return "Unknown";
    }
}

const char* ShaderExpressionHlslHelpers() noexcept {
    return R"HLSL(
// ACS typed expression bytecode contract. Each instruction is 48 bytes.
// uint4 blocks make the layout identical under DXIL and SPIR-V structured
// buffer rules; use the accessors below instead of declaring mixed-width fields.
struct AcsExprInstruction
{
    uint4 Words0_3;
    uint4 Words4_7;
    uint4 Words8_11;
};

static const uint ACS_EXPR_CONSTANT = 0;
static const uint ACS_EXPR_SCALAR_PARAMETER = 1;
static const uint ACS_EXPR_VECTOR_PARAMETER = 2;
static const uint ACS_EXPR_TEXTURE_SAMPLE_2D = 3;
static const uint ACS_EXPR_UV0 = 4;
static const uint ACS_EXPR_TIME = 5;
static const uint ACS_EXPR_WORLD_POSITION = 6;
static const uint ACS_EXPR_WORLD_NORMAL = 7;
static const uint ACS_EXPR_ADD = 8;
static const uint ACS_EXPR_MULTIPLY = 9;
static const uint ACS_EXPR_LERP = 10;
static const uint ACS_EXPR_CLAMP = 11;
static const uint ACS_EXPR_POWER = 12;
static const uint ACS_EXPR_DOT = 13;
static const uint ACS_EXPR_NORMALIZE = 14;
static const uint ACS_EXPR_NOISE = 15;
static const uint ACS_EXPR_COMPONENT = 16;

uint AcsExprOp(AcsExprInstruction i) { return i.Words0_3.x & 255u; }
uint AcsExprType(AcsExprInstruction i)
{
    return (i.Words0_3.x >> 8u) & 255u;
}
uint AcsExprWidth(AcsExprInstruction i) { return AcsExprType(i); }
int AcsExprSigned16(uint x)
{
    return (x & 32768u) != 0u ? int(x | 4294901760u) : int(x);
}
int AcsExprInput(AcsExprInstruction i, uint slot)
{
    if (slot == 0u) return AcsExprSigned16(i.Words0_3.y & 65535u);
    if (slot == 1u) return AcsExprSigned16(i.Words0_3.y >> 16u);
    return AcsExprSigned16(i.Words0_3.z & 65535u);
}
uint AcsExprOriginalNode(AcsExprInstruction i)
{
    return i.Words0_3.z >> 16u;
}
uint AcsExprParameterId(AcsExprInstruction i)
{
    return i.Words0_3.w;
}
uint AcsExprTextureSlot(AcsExprInstruction i)
{
    return i.Words4_7.x & 255u;
}
uint AcsExprTextureFlags(AcsExprInstruction i)
{
    return (i.Words4_7.x >> 8u) & 255u;
}
uint AcsExprComponent(AcsExprInstruction i)
{
    return i.Words4_7.x & 255u;
}
uint2 AcsExprTextureAssetId(AcsExprInstruction i)
{
    return i.Words4_7.yz;
}
float4 AcsExprLiteral(AcsExprInstruction i)
{
    return asfloat(uint4(
        i.Words4_7.w,
        i.Words8_11.x,
        i.Words8_11.y,
        i.Words8_11.z));
}
float4 AcsExprBroadcast(float4 v, uint sourceWidth)
{
    return sourceWidth == 1u ? v.xxxx : v;
}
float AcsExprSafePower(float base, float exponent)
{
    if (base == 0.0 && exponent > 0.0) return 0.0;
    if (base == 0.0 && exponent == 0.0) return 1.0;
    float p = log2(max(abs(base), 1.0e-6)) * clamp(exponent, -32.0, 32.0);
    return exp2(clamp(p, -126.0, 126.0));
}
float4 AcsExprSafePower4(float4 base, float4 exponent)
{
    return float4(
        AcsExprSafePower(base.x, exponent.x),
        AcsExprSafePower(base.y, exponent.y),
        AcsExprSafePower(base.z, exponent.z),
        AcsExprSafePower(base.w, exponent.w));
}
float4 AcsExprNormalize(float4 v, uint width)
{
    float lengthSquared = v.x * v.x;
    if (width > 1u) lengthSquared += v.y * v.y;
    if (width > 2u) lengthSquared += v.z * v.z;
    if (width > 3u) lengthSquared += v.w * v.w;
    return lengthSquared > 1.0e-20 ? v * rsqrt(lengthSquared) : 0.0.xxxx;
}
float AcsExprNoise(float4 p, uint width)
{
    float projection = p.x * 12.9898;
    if (width > 1u) projection += p.y * 78.233;
    if (width > 2u) projection += p.z * 37.719;
    if (width > 3u) projection += p.w * 19.913;
    return frac(sin(projection) * 43758.5453123);
}
)HLSL";
}

} // namespace acs

// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "render/SubstrateExpression.h"

#include <cstring>
#include <limits>

using namespace acs;

namespace {

i16 AddNode(FShaderExpressionGraph& graph,
            EShaderExpressionOp op,
            EShaderExpressionValueType type =
                EShaderExpressionValueType::Invalid) noexcept {
    const u16 index = graph.node_count++;
    graph.nodes[index] = FShaderExpressionNode{};
    graph.nodes[index].op = op;
    graph.nodes[index].declared_type = type;
    return static_cast<i16>(index);
}

i16 AddConstant(FShaderExpressionGraph& graph,
                EShaderExpressionValueType type,
                FShaderExpressionValue value) noexcept {
    const i16 index = AddNode(graph, EShaderExpressionOp::Constant, type);
    graph.nodes[static_cast<u16>(index)].value = value;
    return index;
}

i16 AddUnary(FShaderExpressionGraph& graph,
             EShaderExpressionOp op,
             i16 input) noexcept {
    const i16 index = AddNode(graph, op);
    graph.nodes[static_cast<u16>(index)].inputs[0] = input;
    return index;
}

i16 AddBinary(FShaderExpressionGraph& graph,
              EShaderExpressionOp op,
              i16 a,
              i16 b) noexcept {
    const i16 index = AddNode(graph, op);
    graph.nodes[static_cast<u16>(index)].inputs[0] = a;
    graph.nodes[static_cast<u16>(index)].inputs[1] = b;
    return index;
}

i16 AddTernary(FShaderExpressionGraph& graph,
               EShaderExpressionOp op,
               i16 a,
               i16 b,
               i16 c) noexcept {
    const i16 index = AddNode(graph, op);
    graph.nodes[static_cast<u16>(index)].inputs[0] = a;
    graph.nodes[static_cast<u16>(index)].inputs[1] = b;
    graph.nodes[static_cast<u16>(index)].inputs[2] = c;
    return index;
}

FShaderExpressionValue EvaluateNode(
    const FShaderExpressionCompileResult& program,
    i16 node,
    const FShaderExpressionEvaluationContext& context = {}) noexcept {
    FShaderExpressionValue value{};
    EXPECT_TRUE(EvaluateShaderExpression(
        program, node, context, value, nullptr));
    return value;
}

struct FTextureProbe {
    u32 call_count = 0u;
    u8 slot = 0u;
    u8 flags = 0u;
    u64 asset_id = 0u;
    bool succeed = true;
};

bool ProbeTexture(void* user,
                  u8 slot,
                  u64 asset_id,
                  u8 flags,
                  FShaderExpressionValue uv,
                  FShaderExpressionValue* out_rgba) {
    FTextureProbe& probe = *static_cast<FTextureProbe*>(user);
    ++probe.call_count;
    probe.slot = slot;
    probe.flags = flags;
    probe.asset_id = asset_id;
    if (!probe.succeed) {
        return false;
    }
    *out_rgba = FShaderExpressionValue{
        uv.x, uv.y, static_cast<f32>(slot), 1.0f,
    };
    return true;
}

} // namespace

ACS_TEST(SubstrateExpression, AllMathOpsFoldAndEvaluate) {
    FShaderExpressionGraph graph{};
    const i16 vector = AddConstant(
        graph, EShaderExpressionValueType::Float3,
        FShaderExpressionValue{1.0f, 2.0f, 3.0f});
    const i16 two = AddConstant(
        graph, EShaderExpressionValueType::Float1,
        FShaderExpressionValue{2.0f});
    const i16 add = AddBinary(
        graph, EShaderExpressionOp::Add, vector, two);
    const i16 multiply = AddBinary(
        graph, EShaderExpressionOp::Multiply, add, two);
    const i16 half = AddConstant(
        graph, EShaderExpressionValueType::Float1,
        FShaderExpressionValue{0.5f});
    const i16 lerp = AddTernary(
        graph, EShaderExpressionOp::Lerp, vector, multiply, half);
    const i16 zero = AddConstant(
        graph, EShaderExpressionValueType::Float1,
        FShaderExpressionValue{0.0f});
    const i16 five = AddConstant(
        graph, EShaderExpressionValueType::Float1,
        FShaderExpressionValue{5.0f});
    const i16 clamp = AddTernary(
        graph, EShaderExpressionOp::Clamp, lerp, zero, five);
    const i16 power = AddBinary(
        graph, EShaderExpressionOp::Power, clamp, two);
    const i16 dot = AddBinary(
        graph, EShaderExpressionOp::Dot, vector, vector);
    const i16 normalize = AddUnary(
        graph, EShaderExpressionOp::Normalize, vector);
    const i16 noise = AddUnary(
        graph, EShaderExpressionOp::Noise, vector);
    const i16 component = AddUnary(
        graph, EShaderExpressionOp::Component, power);
    graph.nodes[static_cast<u16>(component)].component_index = 2u;
    graph.root = component;

    const FShaderExpressionCompileResult program =
        CompileShaderExpressionGraph(graph);
    EXPECT_TRUE(program.Succeeded());
    EXPECT_EQ(program.instruction_count, graph.node_count);
    EXPECT_EQ(program.constant_fold_count, 9u);
    EXPECT_EQ(
        ShaderExpressionInstructionOp(
            program.instructions[
                static_cast<u16>(
                    program.original_to_instruction[
                        static_cast<u16>(component)])]),
        EShaderExpressionOp::Constant);

    const FShaderExpressionValue add_value =
        EvaluateNode(program, add);
    EXPECT_NEAR(add_value.x, 3.0f, 1.0e-6f);
    EXPECT_NEAR(add_value.y, 4.0f, 1.0e-6f);
    EXPECT_NEAR(add_value.z, 5.0f, 1.0e-6f);

    const FShaderExpressionValue lerp_value =
        EvaluateNode(program, lerp);
    EXPECT_NEAR(lerp_value.x, 3.5f, 1.0e-6f);
    EXPECT_NEAR(lerp_value.y, 5.0f, 1.0e-6f);
    EXPECT_NEAR(lerp_value.z, 6.5f, 1.0e-6f);

    const FShaderExpressionValue power_value =
        EvaluateNode(program, power);
    EXPECT_NEAR(power_value.x, 12.25f, 1.0e-4f);
    EXPECT_NEAR(power_value.y, 25.0f, 1.0e-4f);
    EXPECT_NEAR(power_value.z, 25.0f, 1.0e-4f);

    const FShaderExpressionValue dot_value =
        EvaluateNode(program, dot);
    EXPECT_NEAR(dot_value.x, 14.0f, 1.0e-5f);

    const FShaderExpressionValue normal_value =
        EvaluateNode(program, normalize);
    EXPECT_NEAR(normal_value.x, 0.2672612f, 1.0e-5f);
    EXPECT_NEAR(normal_value.y, 0.5345225f, 1.0e-5f);
    EXPECT_NEAR(normal_value.z, 0.8017837f, 1.0e-5f);

    const FShaderExpressionValue noise_value =
        EvaluateNode(program, noise);
    EXPECT_TRUE(noise_value.x >= 0.0f);
    EXPECT_TRUE(noise_value.x < 1.0f);

    FShaderExpressionValue root_value{};
    EXPECT_TRUE(EvaluateShaderExpressionRoot(
        program, {}, root_value, nullptr));
    EXPECT_NEAR(root_value.x, 25.0f, 1.0e-4f);
}

ACS_TEST(SubstrateExpression, ContextSourcesAndParametersAreRuntimeFunctional) {
    FShaderExpressionGraph graph{};
    const i16 uv = AddNode(graph, EShaderExpressionOp::UV0);
    const i16 time = AddNode(graph, EShaderExpressionOp::Time);
    const i16 uv_time = AddBinary(
        graph, EShaderExpressionOp::Add, uv, time);
    const i16 world_position =
        AddNode(graph, EShaderExpressionOp::WorldPosition);
    const i16 world_normal =
        AddNode(graph, EShaderExpressionOp::WorldNormal);
    const i16 facing = AddBinary(
        graph, EShaderExpressionOp::Dot,
        world_position, world_normal);

    const i16 speed =
        AddNode(graph, EShaderExpressionOp::ScalarParameter);
    graph.nodes[static_cast<u16>(speed)].parameter_id =
        ShaderExpressionParameterId("Speed");
    graph.nodes[static_cast<u16>(speed)].value =
        FShaderExpressionValue{2.0f};
    const i16 animated = AddBinary(
        graph, EShaderExpressionOp::Multiply, time, speed);

    const i16 tint = AddNode(
        graph, EShaderExpressionOp::VectorParameter,
        EShaderExpressionValueType::Float3);
    graph.nodes[static_cast<u16>(tint)].parameter_id =
        ShaderExpressionParameterId("Tint");
    graph.nodes[static_cast<u16>(tint)].value =
        FShaderExpressionValue{0.1f, 0.2f, 0.3f};
    const i16 tinted_position = AddBinary(
        graph, EShaderExpressionOp::Add, world_position, tint);
    graph.root = tinted_position;

    const FShaderExpressionCompileResult program =
        CompileShaderExpressionGraph(graph);
    EXPECT_TRUE(program.Succeeded());
    EXPECT_EQ(program.constant_fold_count, 0u);

    FShaderExpressionParameter parameters[2]{};
    parameters[0].id = ShaderExpressionParameterId("Speed");
    parameters[0].type = EShaderExpressionValueType::Float1;
    parameters[0].value = FShaderExpressionValue{4.0f};
    parameters[1].id = ShaderExpressionParameterId("Tint");
    parameters[1].type = EShaderExpressionValueType::Float3;
    parameters[1].value =
        FShaderExpressionValue{1.0f, 2.0f, 3.0f};

    FShaderExpressionEvaluationContext context{};
    context.uv0 = FShaderExpressionValue{0.25f, 0.75f};
    context.time = 3.0f;
    context.world_position =
        FShaderExpressionValue{2.0f, 4.0f, 6.0f};
    context.world_normal =
        FShaderExpressionValue{0.0f, 0.0f, 1.0f};
    context.parameters = parameters;
    context.parameter_count = 2u;

    const FShaderExpressionValue uv_time_value =
        EvaluateNode(program, uv_time, context);
    EXPECT_NEAR(uv_time_value.x, 3.25f, 1.0e-6f);
    EXPECT_NEAR(uv_time_value.y, 3.75f, 1.0e-6f);
    EXPECT_NEAR(EvaluateNode(program, facing, context).x, 6.0f, 1.0e-6f);
    EXPECT_NEAR(EvaluateNode(program, animated, context).x, 12.0f, 1.0e-6f);

    const FShaderExpressionValue tinted =
        EvaluateNode(program, tinted_position, context);
    EXPECT_NEAR(tinted.x, 3.0f, 1.0e-6f);
    EXPECT_NEAR(tinted.y, 6.0f, 1.0e-6f);
    EXPECT_NEAR(tinted.z, 9.0f, 1.0e-6f);

    // Independent Slab-field roots evaluate only their own dependency closure.
    context.world_position.x =
        std::numeric_limits<f32>::quiet_NaN();
    EXPECT_NEAR(EvaluateNode(program, animated, context).x, 12.0f, 1.0e-6f);

    context.parameters = nullptr;
    context.parameter_count = 0u;
    EXPECT_NEAR(EvaluateNode(program, animated, context).x, 6.0f, 1.0e-6f);
}

ACS_TEST(SubstrateExpression, TextureFallbackCallbackAndRgbaComponents) {
    FShaderExpressionGraph graph{};
    const i16 uv = AddNode(graph, EShaderExpressionOp::UV0);
    const i16 texture =
        AddUnary(graph, EShaderExpressionOp::TextureSample2D, uv);
    FShaderExpressionNode& texture_node =
        graph.nodes[static_cast<u16>(texture)];
    texture_node.texture_slot = 3u;
    texture_node.texture_flags =
        ShaderExpressionTextureFlag_LinearFilter |
        ShaderExpressionTextureFlag_ClampU |
        ShaderExpressionTextureFlag_Srgb;
    texture_node.texture_asset_id_low = 0x89ABCDEFu;
    texture_node.texture_asset_id_high = 0x01234567u;
    texture_node.value =
        FShaderExpressionValue{0.1f, 0.2f, 0.3f, 0.4f};

    i16 components[4]{};
    for (u8 lane = 0u; lane < 4u; ++lane) {
        components[lane] =
            AddUnary(graph, EShaderExpressionOp::Component, texture);
        graph.nodes[
            static_cast<u16>(components[lane])].component_index = lane;
    }
    graph.root = components[2];

    const FShaderExpressionCompileResult program =
        CompileShaderExpressionGraph(graph);
    EXPECT_TRUE(program.Succeeded());
    EXPECT_NEAR(EvaluateNode(program, components[0]).x, 0.1f, 1.0e-6f);
    EXPECT_NEAR(EvaluateNode(program, components[1]).x, 0.2f, 1.0e-6f);
    EXPECT_NEAR(EvaluateNode(program, components[2]).x, 0.3f, 1.0e-6f);
    EXPECT_NEAR(EvaluateNode(program, components[3]).x, 0.4f, 1.0e-6f);

    FTextureProbe probe{};
    FShaderExpressionEvaluationContext context{};
    context.uv0 = FShaderExpressionValue{0.25f, 0.75f};
    context.texture_sampler = &ProbeTexture;
    context.texture_user = &probe;
    EXPECT_NEAR(
        EvaluateNode(program, components[0], context).x, 0.25f, 1.0e-6f);
    EXPECT_NEAR(
        EvaluateNode(program, components[1], context).x, 0.75f, 1.0e-6f);
    EXPECT_NEAR(
        EvaluateNode(program, components[2], context).x, 3.0f, 1.0e-6f);
    EXPECT_NEAR(
        EvaluateNode(program, components[3], context).x, 1.0f, 1.0e-6f);
    EXPECT_EQ(probe.slot, 3u);
    EXPECT_EQ(
        probe.flags,
        static_cast<u8>(
            ShaderExpressionTextureFlag_LinearFilter |
            ShaderExpressionTextureFlag_ClampU |
            ShaderExpressionTextureFlag_Srgb));
    EXPECT_EQ(probe.asset_id, 0x0123456789ABCDEFull);

    probe.succeed = false;
    EXPECT_NEAR(
        EvaluateNode(program, components[2], context).x, 0.3f, 1.0e-6f);
}

ACS_TEST(SubstrateExpression, ValidationReportsStableNodeAndInput) {
    FShaderExpressionGraph empty{};
    FShaderExpressionCompileResult result =
        CompileShaderExpressionGraph(empty);
    EXPECT_EQ(
        result.diagnostics[0].error,
        EShaderExpressionError::EmptyGraph);

    FShaderExpressionGraph missing{};
    AddNode(missing, EShaderExpressionOp::Add);
    result = CompileShaderExpressionGraph(missing);
    EXPECT_EQ(
        result.diagnostics[0].error,
        EShaderExpressionError::MissingInput);
    EXPECT_EQ(result.diagnostics[0].node, 0);
    EXPECT_EQ(result.diagnostics[0].input, 0);

    FShaderExpressionGraph invalid_index{};
    const i16 invalid_add =
        AddNode(invalid_index, EShaderExpressionOp::Add);
    invalid_index.nodes[static_cast<u16>(invalid_add)].inputs[0] = 9;
    invalid_index.nodes[static_cast<u16>(invalid_add)].inputs[1] = 9;
    result = CompileShaderExpressionGraph(invalid_index);
    EXPECT_EQ(
        result.diagnostics[0].error,
        EShaderExpressionError::InvalidInputIndex);
    EXPECT_EQ(result.diagnostics[0].node, 0);
    EXPECT_EQ(result.diagnostics[0].input, 0);

    FShaderExpressionGraph unexpected{};
    const i16 unexpected_constant = AddConstant(
        unexpected, EShaderExpressionValueType::Float1,
        FShaderExpressionValue{1.0f});
    unexpected.nodes[
        static_cast<u16>(unexpected_constant)].inputs[0] = 0;
    result = CompileShaderExpressionGraph(unexpected);
    EXPECT_EQ(
        result.diagnostics[0].error,
        EShaderExpressionError::UnexpectedInput);

    FShaderExpressionGraph non_finite{};
    AddConstant(
        non_finite, EShaderExpressionValueType::Float1,
        FShaderExpressionValue{
            std::numeric_limits<f32>::quiet_NaN()});
    result = CompileShaderExpressionGraph(non_finite);
    EXPECT_EQ(
        result.diagnostics[0].error,
        EShaderExpressionError::NonFiniteValue);
    EXPECT_EQ(result.diagnostics[0].node, 0);
}

ACS_TEST(SubstrateExpression, RejectsCyclesAndIllegalTypeCoercion) {
    FShaderExpressionGraph cycle{};
    const i16 add = AddNode(cycle, EShaderExpressionOp::Add);
    const i16 normalize =
        AddNode(cycle, EShaderExpressionOp::Normalize);
    const i16 constant = AddConstant(
        cycle, EShaderExpressionValueType::Float1,
        FShaderExpressionValue{1.0f});
    cycle.nodes[static_cast<u16>(add)].inputs[0] = normalize;
    cycle.nodes[static_cast<u16>(add)].inputs[1] = constant;
    cycle.nodes[static_cast<u16>(normalize)].inputs[0] = add;
    FShaderExpressionCompileResult result =
        CompileShaderExpressionGraph(cycle);
    EXPECT_EQ(
        result.diagnostics[0].error,
        EShaderExpressionError::Cycle);
    EXPECT_EQ(result.diagnostics[0].node, normalize);
    EXPECT_EQ(result.diagnostics[0].input, 0);

    FShaderExpressionGraph mismatch{};
    const i16 vector2 = AddConstant(
        mismatch, EShaderExpressionValueType::Float2,
        FShaderExpressionValue{1.0f, 2.0f});
    const i16 vector3 = AddConstant(
        mismatch, EShaderExpressionValueType::Float3,
        FShaderExpressionValue{1.0f, 2.0f, 3.0f});
    const i16 mismatch_add = AddBinary(
        mismatch, EShaderExpressionOp::Add, vector2, vector3);
    result = CompileShaderExpressionGraph(mismatch);
    EXPECT_EQ(
        result.diagnostics[0].error,
        EShaderExpressionError::TypeMismatch);
    EXPECT_EQ(result.diagnostics[0].node, mismatch_add);
    EXPECT_EQ(result.diagnostics[0].input, 1);

    FShaderExpressionGraph bad_component{};
    const i16 source2 = AddConstant(
        bad_component, EShaderExpressionValueType::Float2,
        FShaderExpressionValue{1.0f, 2.0f});
    const i16 z = AddUnary(
        bad_component, EShaderExpressionOp::Component, source2);
    bad_component.nodes[static_cast<u16>(z)].component_index = 2u;
    result = CompileShaderExpressionGraph(bad_component);
    EXPECT_EQ(
        result.diagnostics[0].error,
        EShaderExpressionError::TypeMismatch);
    EXPECT_EQ(result.diagnostics[0].node, z);
}

ACS_TEST(SubstrateExpression, RejectsTextureAndParameterRangeConflicts) {
    FShaderExpressionGraph bad_texture{};
    const i16 uv = AddNode(bad_texture, EShaderExpressionOp::UV0);
    const i16 sample = AddUnary(
        bad_texture, EShaderExpressionOp::TextureSample2D, uv);
    bad_texture.nodes[static_cast<u16>(sample)].texture_slot = 4u;
    FShaderExpressionCompileResult result =
        CompileShaderExpressionGraph(bad_texture);
    EXPECT_EQ(
        result.diagnostics[0].error,
        EShaderExpressionError::TextureSlotOutOfRange);
    EXPECT_EQ(result.diagnostics[0].node, sample);

    bad_texture.nodes[static_cast<u16>(sample)].texture_slot = 0u;
    bad_texture.nodes[static_cast<u16>(sample)].texture_flags = 0x80u;
    result = CompileShaderExpressionGraph(bad_texture);
    EXPECT_EQ(
        result.diagnostics[0].error,
        EShaderExpressionError::InvalidTextureFlags);

    FShaderExpressionGraph texture_conflict{};
    const i16 conflict_uv =
        AddNode(texture_conflict, EShaderExpressionOp::UV0);
    const i16 sample_a = AddUnary(
        texture_conflict, EShaderExpressionOp::TextureSample2D,
        conflict_uv);
    const i16 sample_b = AddUnary(
        texture_conflict, EShaderExpressionOp::TextureSample2D,
        conflict_uv);
    texture_conflict.nodes[
        static_cast<u16>(sample_a)].texture_asset_id_low = 1u;
    texture_conflict.nodes[
        static_cast<u16>(sample_b)].texture_asset_id_low = 2u;
    result = CompileShaderExpressionGraph(texture_conflict);
    EXPECT_EQ(
        result.diagnostics[0].error,
        EShaderExpressionError::TextureSlotConflict);
    EXPECT_EQ(result.diagnostics[0].node, sample_b);

    FShaderExpressionGraph parameter_conflict{};
    const u32 shared = ShaderExpressionParameterId("Shared");
    const i16 scalar = AddNode(
        parameter_conflict, EShaderExpressionOp::ScalarParameter);
    parameter_conflict.nodes[
        static_cast<u16>(scalar)].parameter_id = shared;
    const i16 vector = AddNode(
        parameter_conflict, EShaderExpressionOp::VectorParameter,
        EShaderExpressionValueType::Float3);
    parameter_conflict.nodes[
        static_cast<u16>(vector)].parameter_id = shared;
    result = CompileShaderExpressionGraph(parameter_conflict);
    EXPECT_EQ(
        result.diagnostics[0].error,
        EShaderExpressionError::ParameterTypeConflict);
    EXPECT_EQ(result.diagnostics[0].node, vector);

    FShaderExpressionGraph too_many_parameters{};
    for (u32 index = 0u;
         index <= kShaderExpressionMaxParameters;
         ++index) {
        const i16 parameter = AddNode(
            too_many_parameters,
            EShaderExpressionOp::ScalarParameter);
        too_many_parameters.nodes[
            static_cast<u16>(parameter)].parameter_id = index + 1u;
    }
    result = CompileShaderExpressionGraph(too_many_parameters);
    EXPECT_EQ(
        result.diagnostics[0].error,
        EShaderExpressionError::TooManyParameters);
    EXPECT_EQ(
        result.diagnostics[0].node,
        static_cast<i16>(kShaderExpressionMaxParameters));
}

ACS_TEST(SubstrateExpression, BytecodeOrderAndHashAreDeterministic) {
    FShaderExpressionGraph graph{};
    const i16 add = AddNode(graph, EShaderExpressionOp::Add);
    const i16 scalar =
        AddNode(graph, EShaderExpressionOp::ScalarParameter);
    const i16 vector = AddNode(
        graph, EShaderExpressionOp::VectorParameter,
        EShaderExpressionValueType::Float3);
    graph.nodes[static_cast<u16>(scalar)].parameter_id =
        ShaderExpressionParameterId("Scale");
    graph.nodes[static_cast<u16>(vector)].parameter_id =
        ShaderExpressionParameterId("Color");
    graph.nodes[static_cast<u16>(add)].inputs[0] = vector;
    graph.nodes[static_cast<u16>(add)].inputs[1] = scalar;
    graph.root = add;

    const FShaderExpressionCompileResult first =
        CompileShaderExpressionGraph(graph);
    const FShaderExpressionCompileResult second =
        CompileShaderExpressionGraph(graph);
    EXPECT_TRUE(first.Succeeded());
    EXPECT_TRUE(second.Succeeded());
    EXPECT_EQ(
        HashCompiledShaderExpression(first),
        HashCompiledShaderExpression(second));
    EXPECT_EQ(
        std::memcmp(
            first.instructions, second.instructions,
            first.instruction_count *
                sizeof(FShaderExpressionInstruction)),
        0);

    const i16 add_instruction =
        first.original_to_instruction[static_cast<u16>(add)];
    EXPECT_TRUE(
        first.original_to_instruction[static_cast<u16>(vector)] <
        add_instruction);
    EXPECT_TRUE(
        first.original_to_instruction[static_cast<u16>(scalar)] <
        add_instruction);
    EXPECT_EQ(first.root_instruction, add_instruction);
    EXPECT_TRUE(
        first.instructions[static_cast<u16>(add_instruction)].inputs[0] <
        add_instruction);
    EXPECT_TRUE(
        first.instructions[static_cast<u16>(add_instruction)].inputs[1] <
        add_instruction);

    graph.nodes[static_cast<u16>(scalar)].value =
        FShaderExpressionValue{9.0f};
    const FShaderExpressionCompileResult changed =
        CompileShaderExpressionGraph(graph);
    EXPECT_NE(
        HashCompiledShaderExpression(first),
        HashCompiledShaderExpression(changed));

    const char* helpers = ShaderExpressionHlslHelpers();
    EXPECT_TRUE(std::strstr(helpers, "AcsExprInstruction") != nullptr);
    EXPECT_TRUE(std::strstr(helpers, "ACS_EXPR_COMPONENT") != nullptr);
    EXPECT_TRUE(std::strstr(helpers, "AcsExprSafePower") != nullptr);
}

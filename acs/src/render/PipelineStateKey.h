// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Hash.h"
#include "memory/Memory.h"
#include "render/IRhiPipeline.h"

namespace acs {

/** PSO 記述子を高速検索するための 128 bit キー。 */
struct FPipelineStateKey {
    /** open addressing の開始位置に使う主ハッシュ。 */
    u64 primary = 0u;
    /** 主ハッシュ衝突を識別する検証ハッシュ。 */
    u64 verification = 0u;

    /** 二つのハッシュが一致するか返す。 */
    constexpr bool operator==(const FPipelineStateKey& other) const noexcept {
        return primary == other.primary && verification == other.verification;
    }
};

/** 一つの整数値を PSO キーへ混合する。 */
inline u64 PipelineKeyCombine(u64 hash, u64 value) noexcept {
    return HashMix64(hash ^ HashMix64(value + 0x9E3779B97F4A7C15ull));
}

/** 浮動小数値の bit 表現をキー用整数へ写す。 */
inline u32 PipelineKeyFloatBits(f32 value) noexcept {
    // 浮動小数値の bit 表現。
    u32 bits = 0u;
    MemCopy(&bits, &value, sizeof(bits));
    return bits;
}

/** 生 byte 列を二系統の内容 hash へ変換する。 */
inline FPipelineStateKey PipelineKeyBytes(const void* data, usize size) noexcept {
    if (data == nullptr && size != 0u) return FPipelineStateKey{};
    /** hash 対象 byte 列。 */
    const byte* bytes = static_cast<const byte*>(data);
    /** primary hash の累積値。 */
    u64 primary = 0xCBF29CE484222325ull;
    /** 独立 verification hash の累積値。 */
    u64 verification = 0x84222325CBF29CE4ull;
    for (usize index = 0u; index < size; ++index) {
        primary = (primary ^ static_cast<u64>(bytes[index])) * 0x100000001B3ull;
        verification = (verification ^ (static_cast<u64>(bytes[index]) + 0x9Dull)) * 0x9E3779B185EBCA87ull;
    }
    return FPipelineStateKey{HashMix64(primary ^ static_cast<u64>(size)), HashMix64(verification ^ (static_cast<u64>(size) * 0xD6E8FEB86659FD93ull))};
}

/** null 終端文字列をポインター値に依存しない内容 hash へ変換する。 */
inline FPipelineStateKey PipelineKeyString(const char* value) noexcept {
    if (value == nullptr) return FPipelineStateKey{};
    /** null 終端を除く文字数。 */
    usize length = 0u;
    while (value[length] != '\0') ++length;
    return PipelineKeyBytes(value, length);
}

/** shader の bytecode 内容と stage から意味上の識別値を作る。 */
inline FPipelineStateKey PipelineKeyShader(const IRhiShader* shader) noexcept {
    if (shader == nullptr) return FPipelineStateKey{};
    /** shader が公開する bytecode 先頭。 */
    const byte* bytecode = shader->Bytecode();
    /** shader bytecode の byte 数。 */
    const usize bytecode_size = shader->BytecodeSize();
    /** 内容または pointer fallback から作る識別 key。 */
    FPipelineStateKey content = bytecode != nullptr && bytecode_size > 0u ? PipelineKeyBytes(bytecode, bytecode_size) : FPipelineStateKey{HashMix64(reinterpret_cast<u64>(shader)), HashMix64(reinterpret_cast<u64>(shader) ^ 0xA4093822299F31D0ull)};
    content.primary = PipelineKeyCombine(content.primary, PipelineKeyCombine(static_cast<u64>(bytecode_size), static_cast<u64>(shader->Stage())));
    content.verification = PipelineKeyCombine(content.verification, PipelineKeyCombine(static_cast<u64>(shader->Stage()), static_cast<u64>(bytecode_size)));
    return content;
}

/** サンプラーの全状態を PSO キーへ加える。 */
template<typename FAdd>
inline void AddSamplerToPipelineKey(const FSamplerDesc& sampler, FAdd&& add) noexcept {
    add(static_cast<u64>(sampler.filter));
    add(static_cast<u64>(sampler.address_u));
    add(static_cast<u64>(sampler.address_v));
    add(static_cast<u64>(sampler.address_w));
    add(PipelineKeyFloatBits(sampler.min_lod));
    add(PipelineKeyFloatBits(sampler.max_lod));
    add(sampler.max_anisotropy);
    add(sampler.comparison ? 1u : 0u);
}

/** graphics 記述子の全 PSO 状態からキーを作る。 */
inline FPipelineStateKey MakePipelineStateKey(const FPipelineDesc& desc) noexcept {
    // 構築中の二重ハッシュ。
    FPipelineStateKey key{0x243F6A8885A308D3ull, 0x13198A2E03707344ull};
    // 一つの状態値を二重ハッシュへ加える処理。
    const auto add = [&key](u64 value) noexcept {
        key.primary = PipelineKeyCombine(key.primary, value);
        key.verification = PipelineKeyCombine(key.verification, value ^ 0xA4093822299F31D0ull);
    };
    /** 二系統 hash を独立に key へ合成する関数。 */
    const auto add_pair = [&key](const FPipelineStateKey& value) noexcept {
        key.primary = PipelineKeyCombine(key.primary, value.primary);
        key.verification = PipelineKeyCombine(key.verification, value.verification);
    };
    add_pair(PipelineKeyShader(desc.vs));
    add_pair(PipelineKeyShader(desc.ps));
    add(static_cast<u64>(desc.topology));
    // backend が実際に使う RT 数。
    /** backend が実際に消費する RT 数。 */
    const u32 render_target_count = desc.ps == nullptr ? 0u : desc.rt_count == 0u ? 1u : desc.rt_count;
    add(static_cast<u64>(render_target_count));
    if (render_target_count == 1u && desc.rt_count == 0u) {
        add(static_cast<u64>(desc.rt_format));
    } else {
        for (u32 index = 0u; index < render_target_count && index < 8u; ++index) add(static_cast<u64>(desc.rt_formats[index]));
    }
    add(static_cast<u64>(desc.depth_format));
    add(desc.vertex_stride);
    add(desc.layout_count);
    for (u32 index = 0u; index < desc.layout_count && index < 8u; ++index) {
        add_pair(PipelineKeyString(desc.layout[index].semantic_name));
        add(desc.layout[index].semantic_index);
        add(static_cast<u64>(desc.layout[index].format));
        add(desc.layout[index].offset);
    }
    add(desc.cbuffer_slots);
    add(desc.texture_slots);
    add(desc.static_sampler_count);
    for (u32 index = 0u; index < desc.static_sampler_count && index < 16u; ++index) {
        AddSamplerToPipelineKey(desc.static_samplers[index], add);
    }
    add(static_cast<u64>(desc.cull_mode));
    add(static_cast<u64>(desc.blend_mode));
    add(desc.depth_test ? 1u : 0u);
    add(desc.depth_write ? 1u : 0u);
    add(desc.stencil.enable ? 1u : 0u);
    add(static_cast<u64>(desc.stencil.func));
    add(static_cast<u64>(desc.stencil.pass_op));
    add(static_cast<u64>(desc.stencil.fail_op));
    add(static_cast<u64>(desc.stencil.depth_fail_op));
    add(desc.stencil.read_mask);
    add(desc.stencil.write_mask);
    add(desc.sample_count);
    for (u32 index = 0u; index < desc.cbuffer_slots && index < 16u; ++index) {
        add_pair(PipelineKeyString(desc.cbuffer_names[index]));
    }
    for (u32 index = 0u; index < desc.texture_slots && index < 16u; ++index) {
        add_pair(PipelineKeyString(desc.texture_names[index]));
    }
    return key;
}

/** compute 記述子の全 PSO 状態からキーを作る。 */
inline FPipelineStateKey MakePipelineStateKey(const FComputePipelineDesc& desc) noexcept {
    // 構築中の二重ハッシュ。
    FPipelineStateKey key{0x452821E638D01377ull, 0xBE5466CF34E90C6Cull};
    // 一つの状態値を二重ハッシュへ加える処理。
    const auto add = [&key](u64 value) noexcept {
        key.primary = PipelineKeyCombine(key.primary, value);
        key.verification = PipelineKeyCombine(key.verification, value ^ 0xC0AC29B7C97C50DDull);
    };
    /** 二系統 hash を独立に key へ合成する関数。 */
    const auto add_pair = [&key](const FPipelineStateKey& value) noexcept {
        key.primary = PipelineKeyCombine(key.primary, value.primary);
        key.verification = PipelineKeyCombine(key.verification, value.verification);
    };
    add_pair(PipelineKeyShader(desc.cs));
    add(desc.cbuffer_slots);
    add(desc.srv_slots);
    add(desc.uav_slots);
    add(desc.static_sampler_count);
    for (u32 index = 0u; index < desc.static_sampler_count && index < 16u; ++index) {
        AddSamplerToPipelineKey(desc.static_samplers[index], add);
    }
    for (u32 index = 0u; index < desc.cbuffer_slots && index < 16u; ++index) {
        add_pair(PipelineKeyString(desc.cbuffer_names[index]));
    }
    for (u32 index = 0u; index < desc.srv_slots && index < 16u; ++index) {
        add_pair(PipelineKeyString(desc.srv_names[index]));
    }
    for (u32 index = 0u; index < desc.uav_slots && index < 16u; ++index) {
        add_pair(PipelineKeyString(desc.uav_names[index]));
    }
    return key;
}

} // namespace acs

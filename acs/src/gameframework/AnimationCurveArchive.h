// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/AnimationCurve.h"

namespace acs::game {

/**
 * アニメーションカーブの wire / file codec が返す安定した失敗分類。
 *
 * `FAnimationCurveArchiveResult::error == InvalidCurveData` の場合は、
 * `curve_error` がより具体的な意味検証エラーを保持する。
 */
enum class EAnimationCurveArchiveError : u8 {
    None = 0,
    NullArgument,
    BufferTooSmall,
    InvalidMagic,
    UnsupportedVersion,
    InvalidHeader,
    SizeMismatch,
    ChecksumMismatch,
    TooManyKeys,
    InvalidCurveData,
    AllocationFailure,
    PersistenceFailure,
};

/** buffer / file 永続化操作で共通して返す診断結果。 */
struct FAnimationCurveArchiveResult {
    EAnimationCurveArchiveError error =
        EAnimationCurveArchiveError::None;
    EAnimationCurveError curve_error = EAnimationCurveError::None;
    u32 key_index = 0u;
    u64 required_size = 0u;
    u16 persistence_subcode = 0u;
    u32 os_error = 0u;

    bool Succeeded() const noexcept {
        return error == EAnimationCurveArchiveError::None;
    }

    static const char* ErrorName(
        EAnimationCurveArchiveError error) noexcept;
};

/**
 * FAnimationCurve の正準かつ上限付きの永続化。
 *
 * memory 上の wire format は固定幅 little endian で、native struct の padding は
 * serialize しない。CRC field を 0 として計算する正準な全 wire CRC が header と
 * key record を保護し、file helper はさらに CSaveArchive の検証済み atomic envelope
 * を利用する。
 *
 * decode / load は常に transactional である。完全な header、厳密な size、CRC、
 * 全 key の検証と必要な全確保に成功した後だけ、出力先 curve を commit する。
 */
class CAnimationCurveArchive {
public:
    static constexpr u16 kWireVersion = 1u;
    static constexpr u32 kHeaderSize = 32u;
    static constexpr u32 kKeyRecordSize = 20u;
    static constexpr u32 kFileEnvelopeVersion = 1u;
    static constexpr u64 kMaxEncodedSize =
        static_cast<u64>(kHeaderSize) +
        static_cast<u64>(kKeyRecordSize) *
            static_cast<u64>(FAnimationCurve::kMaxKeys);

    CAnimationCurveArchive() = delete;

    /** Encode に必要な正確な byte 数を返す。 */
    static u64 EncodedSize(const FAnimationCurve& curve) noexcept;

    /**
     * curve を caller 所有 buffer へ encode する。
     *
     * `out_size` には BufferTooSmall の場合も含め、常に必要 size を設定する。
     * curve 全体が有効で buffer 容量が十分な場合を除き、出力 buffer は変更しない。
     */
    static FAnimationCurveArchiveResult Encode(
        const FAnimationCurve& curve,
        void* out_bytes,
        u64 out_capacity,
        u64& out_size) noexcept;

    /**
     * 厳密に 1 件の wire record を decode し、末尾の余剰 byte は拒否する。
     *
     * 失敗時は常に `out_curve` の key と wrap mode を変更しない。
     */
    static FAnimationCurveArchiveResult Decode(
        const void* bytes,
        u64 size,
        FAnimationCurve& out_curve) noexcept;

    /** CSaveArchive の atomic replace を介して正準 wire record を保存する。 */
    static FAnimationCurveArchiveResult SaveToFile(
        const wchar_t* file_path,
        const FAnimationCurve& curve) noexcept;

    /** 正準 curve file 1 件を読み込み、検証後に transactional に commit する。 */
    static FAnimationCurveArchiveResult LoadFromFile(
        const wchar_t* file_path,
        FAnimationCurve& out_curve) noexcept;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FAnimationCurveArchive = CAnimationCurveArchive;

} // namespace acs::game

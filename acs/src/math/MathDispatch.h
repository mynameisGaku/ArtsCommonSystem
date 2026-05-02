// =============================================================================
// ACS Math — バッチ演算の CPU ディスパッチテーブル
// -----------------------------------------------------------------------------
// 1 要素ずつの計算は Vec/Mat/Quat ヘッダにインライン記述（DirectXMath が
// /arch:* に応じて最適 SIMD を選ぶ）。一方、N 要素の一括変換などは関数
// ポインタテーブル経由でディスパッチし、起動時に検出した CPU 機能に応じて
// 最適な実装を選択する。
//
// Phase 1 では SSE2 ベースラインのみ実装。AVX2 専用 TU の追加は v2 課題。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "math/Mat.h"

namespace acs {

// 起動時に CPU 機能から選ばれる関数ポインタ群
struct MathDispatch {
    void (*transform_points) (const Vec3* in, Vec3* out, usize count, const Mat4& m) = nullptr;
    void (*transform_vectors)(const Vec3* in, Vec3* out, usize count, const Mat4& m) = nullptr;
};

// 取得（初回呼び出しで初期化）
const MathDispatch& GetMathDispatch() noexcept;

// 利便関数: ディスパッチ越しに呼ぶ
void TransformPoints (const Vec3* in, Vec3* out, usize count, const Mat4& m) noexcept;
void TransformVectors(const Vec3* in, Vec3* out, usize count, const Mat4& m) noexcept;

} // namespace acs

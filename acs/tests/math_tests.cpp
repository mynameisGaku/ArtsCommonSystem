// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "math/Cpu.h"
#include "math/Vec.h"
#include "math/Mat.h"
#include "math/Quat.h"
#include "math/MathDispatch.h"

using namespace acs;

ACS_TEST(Math, CpuDetectionAlwaysSse2) {
    EXPECT_TRUE(Cpu().sse2);
}

ACS_TEST(Math, Vec3Add) {
    FVec3 a(1, 2, 3);
    FVec3 b(4, 5, 6);
    FVec3 c = a + b;
    EXPECT_NEAR(c.x, 5.0f, 1e-5f);
    EXPECT_NEAR(c.y, 7.0f, 1e-5f);
    EXPECT_NEAR(c.z, 9.0f, 1e-5f);
}

ACS_TEST(Math, Vec3Dot) {
    FVec3 a(1, 0, 0);
    FVec3 b(0, 1, 0);
    EXPECT_NEAR(Dot(a, b), 0.0f, 1e-5f);
    EXPECT_NEAR(Dot(a, a), 1.0f, 1e-5f);
}

ACS_TEST(Math, Vec3Cross) {
    FVec3 c = Cross(FVec3::UnitX(), FVec3::UnitY());
    EXPECT_NEAR(c.x, 0.0f, 1e-5f);
    EXPECT_NEAR(c.y, 0.0f, 1e-5f);
    EXPECT_NEAR(c.z, 1.0f, 1e-5f);
}

ACS_TEST(Math, Vec3Normalize) {
    FVec3 n = Normalize(FVec3(0, 3, 4));
    EXPECT_NEAR(Length(n), 1.0f, 1e-5f);
}

ACS_TEST(Math, Mat4Identity) {
    FMat4 i = FMat4::Identity();
    FVec4 v(1, 2, 3, 1);
    FVec4 r = Transform(v, i);
    EXPECT_NEAR(r.x, 1.0f, 1e-5f);
    EXPECT_NEAR(r.y, 2.0f, 1e-5f);
    EXPECT_NEAR(r.z, 3.0f, 1e-5f);
}

ACS_TEST(Math, Mat4Translation) {
    FMat4 t = FMat4::Translation(FVec3(10, 20, 30));
    FVec3 p = TransformPoint(FVec3(1, 2, 3), t);
    EXPECT_NEAR(p.x, 11.0f, 1e-5f);
    EXPECT_NEAR(p.y, 22.0f, 1e-5f);
    EXPECT_NEAR(p.z, 33.0f, 1e-5f);
}

ACS_TEST(Math, QuatAxisAngleRotates) {
    FQuat q = FQuat::AxisAngle(FVec3::UnitZ(), kHalfPi);
    FVec3 r = Rotate(q, FVec3::UnitX());
    EXPECT_NEAR(r.x, 0.0f, 1e-4f);
    EXPECT_NEAR(r.y, 1.0f, 1e-4f);
    EXPECT_NEAR(r.z, 0.0f, 1e-4f);
}

ACS_TEST(Math, MathDispatchPopulated) {
    const auto& d = GetMathDispatch();
    EXPECT_TRUE(d.transform_points  != nullptr);
    EXPECT_TRUE(d.transform_vectors != nullptr);
}

/** 静的 batch policy と runtime dispatch の点・ベクトル結果を比較する。 */
ACS_TEST(Math, StaticBatchPolicyMatchesRuntimeDispatch)
{
    // 比較する要素数。
    constexpr usize kCount = 5u;
    // 平行移動差を検出できる入力群。
    const FVec3 Input[kCount] = {FVec3(-3.0f, 2.0f, 1.0f), FVec3(0.0f, 0.0f, 0.0f), FVec3(7.0f, -2.0f, 4.0f), FVec3(0.25f, 0.5f, 0.75f), FVec3(100.0f, -100.0f, 2.0f)};
    // runtime 点変換の出力。
    FVec3 RuntimePoints[kCount] = {};
    // static 点変換の出力。
    FVec3 StaticPoints[kCount] = {};
    // runtime ベクトル変換の出力。
    FVec3 RuntimeVectors[kCount] = {};
    // static ベクトル変換の出力。
    FVec3 StaticVectors[kCount] = {};
    // 回転と平行移動を含む比較用行列。
    const FMat4 Matrix = FMat4::RotationY(0.37f) * FMat4::Translation(FVec3(11.0f, -5.0f, 3.0f));

    TransformPoints(Input, RuntimePoints, kCount, Matrix);
    TransformVectors(Input, RuntimeVectors, kCount, Matrix);
    TransformBatchStatic<EBatchTransformPolicy::Point>(Input, StaticPoints, Matrix);
    TransformBatchStatic<EBatchTransformPolicy::Vector>(Input, StaticVectors, Matrix);

    for (usize Index = 0u; Index < kCount; ++Index) {
        EXPECT_NEAR(StaticPoints[Index].x, RuntimePoints[Index].x, 1e-6f);
        EXPECT_NEAR(StaticPoints[Index].y, RuntimePoints[Index].y, 1e-6f);
        EXPECT_NEAR(StaticPoints[Index].z, RuntimePoints[Index].z, 1e-6f);
        EXPECT_NEAR(StaticVectors[Index].x, RuntimeVectors[Index].x, 1e-6f);
        EXPECT_NEAR(StaticVectors[Index].y, RuntimeVectors[Index].y, 1e-6f);
        EXPECT_NEAR(StaticVectors[Index].z, RuntimeVectors[Index].z, 1e-6f);
    }
}

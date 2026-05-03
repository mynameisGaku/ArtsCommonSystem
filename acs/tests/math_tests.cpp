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
    Vec3 a(1, 2, 3);
    Vec3 b(4, 5, 6);
    Vec3 c = a + b;
    EXPECT_NEAR(c.x, 5.0f, 1e-5f);
    EXPECT_NEAR(c.y, 7.0f, 1e-5f);
    EXPECT_NEAR(c.z, 9.0f, 1e-5f);
}

ACS_TEST(Math, Vec3Dot) {
    Vec3 a(1, 0, 0);
    Vec3 b(0, 1, 0);
    EXPECT_NEAR(Dot(a, b), 0.0f, 1e-5f);
    EXPECT_NEAR(Dot(a, a), 1.0f, 1e-5f);
}

ACS_TEST(Math, Vec3Cross) {
    Vec3 c = Cross(Vec3::UnitX(), Vec3::UnitY());
    EXPECT_NEAR(c.x, 0.0f, 1e-5f);
    EXPECT_NEAR(c.y, 0.0f, 1e-5f);
    EXPECT_NEAR(c.z, 1.0f, 1e-5f);
}

ACS_TEST(Math, Vec3Normalize) {
    Vec3 n = Normalize(Vec3(0, 3, 4));
    EXPECT_NEAR(Length(n), 1.0f, 1e-5f);
}

ACS_TEST(Math, Mat4Identity) {
    Mat4 i = Mat4::Identity();
    Vec4 v(1, 2, 3, 1);
    Vec4 r = Transform(v, i);
    EXPECT_NEAR(r.x, 1.0f, 1e-5f);
    EXPECT_NEAR(r.y, 2.0f, 1e-5f);
    EXPECT_NEAR(r.z, 3.0f, 1e-5f);
}

ACS_TEST(Math, Mat4Translation) {
    Mat4 t = Mat4::Translation(Vec3(10, 20, 30));
    Vec3 p = TransformPoint(Vec3(1, 2, 3), t);
    EXPECT_NEAR(p.x, 11.0f, 1e-5f);
    EXPECT_NEAR(p.y, 22.0f, 1e-5f);
    EXPECT_NEAR(p.z, 33.0f, 1e-5f);
}

ACS_TEST(Math, QuatAxisAngleRotates) {
    Quat q = Quat::AxisAngle(Vec3::UnitZ(), kHalfPi);
    Vec3 r = Rotate(q, Vec3::UnitX());
    EXPECT_NEAR(r.x, 0.0f, 1e-4f);
    EXPECT_NEAR(r.y, 1.0f, 1e-4f);
    EXPECT_NEAR(r.z, 0.0f, 1e-4f);
}

ACS_TEST(Math, MathDispatchPopulated) {
    const auto& d = GetMathDispatch();
    EXPECT_TRUE(d.transform_points  != nullptr);
    EXPECT_TRUE(d.transform_vectors != nullptr);
}

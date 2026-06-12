// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework/ReflectSerialize.{h,cpp} の検証:
//   反射メタデータ (FReflectField の name/offset/size/kind) を使った汎用の
//   Serialize / Deserialize が、型を知らずに任意の反射型を往復できることを確認する。
//   これが scene save/load・editor play mode・進行セーブ・prefab 永続化の土台。
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Reflect.h"
#include "gameframework/ReflectSerialize.h"
#include "gameframework/ReflectCatalog.h"   // AcsRegisterEngineTypes (実エンジン型)
#include "math/Vec.h"

#include <cstring>   // std::memcmp

using namespace acs;
using namespace acs::game;

// ===== 反射対象のトイ型 (全 EFieldKind を網羅: スカラ + Vec + Enum) ============

enum class ESaveTeam : u8 { Red, Blue, Green };

struct FSaveStats {
    f32       speed;   // F32
    i32       level;   // I32
    u32       flags;   // U32
    bool      alive;   // Bool
    FVec2     pos;     // FVec2
    FVec4     tint;    // FVec4
    ESaveTeam team;    // Enum (u8)
};

ACS_COMPONENT(FSaveStats,
    ACS_RFIELD(FSaveStats, speed, acs::game::EFieldKind::F32),
    ACS_RFIELD(FSaveStats, level, acs::game::EFieldKind::I32),
    ACS_RFIELD(FSaveStats, flags, acs::game::EFieldKind::U32),
    ACS_RFIELD(FSaveStats, alive, acs::game::EFieldKind::Bool),
    ACS_RFIELD(FSaveStats, pos,   acs::game::EFieldKind::FVec2),
    ACS_RFIELD(FSaveStats, tint,  acs::game::EFieldKind::FVec4),
    ACS_RFIELD(FSaveStats, team,  acs::game::EFieldKind::Enum))

ACS_REFLECT_ENUM(ESaveTeam,
    ACS_EVAL(ESaveTeam, Red), ACS_EVAL(ESaveTeam, Blue), ACS_EVAL(ESaveTeam, Green))

namespace {
FSaveStats MakeSample() noexcept {
    FSaveStats s{};
    s.speed = 12.5f;
    s.level = 7;
    s.flags = 0xCAFEu;
    s.alive = true;
    s.pos   = FVec2{ 3.0f, -4.0f };
    s.tint  = FVec4{ 0.25f, 0.5f, 0.75f, 1.0f };
    s.team  = ESaveTeam::Green;
    return s;
}
} // namespace

// ===== テスト ================================================================

// すべての EFieldKind が往復する: 値→bytes→zero→bytes→値 で完全一致する。
ACS_TEST(ReflectSerialize, RoundTripAllKinds) {
    const FTypeDesc* d = AcsTypeDescOf<FSaveStats>();
    EXPECT_TRUE(d != nullptr);
    if (d == nullptr) return;

    const FSaveStats src = MakeSample();
    u8 buf[256];
    const u32 n = SerializeReflected(d, &src, buf, sizeof(buf));
    EXPECT_TRUE(n > 12u);   // header(12) + 7 fields

    // 別インスタンスを 0 埋めしてから復元する。
    FSaveStats dst{};
    const u32 applied = DeserializeReflected(d, &dst, buf, n);
    EXPECT_EQ(applied, 7u);

    EXPECT_NEAR(dst.speed, 12.5f, 0.0001f);
    EXPECT_EQ(dst.level, 7);
    EXPECT_EQ(dst.flags, 0xCAFEu);
    EXPECT_TRUE(dst.alive);
    EXPECT_NEAR(dst.pos.x, 3.0f, 0.0001f);
    EXPECT_NEAR(dst.pos.y, -4.0f, 0.0001f);
    EXPECT_NEAR(dst.tint.x, 0.25f, 0.0001f);
    EXPECT_NEAR(dst.tint.w, 1.0f, 0.0001f);
    EXPECT_TRUE(dst.team == ESaveTeam::Green);
}

// factory 経由: bytes だけから型を特定して生成・復元できる (scene/prefab load の経路)。
ACS_TEST(ReflectSerialize, CreateFromBytesViaFactory) {
    const FTypeDesc* d = AcsTypeDescOf<FSaveStats>();
    EXPECT_TRUE(d != nullptr);
    if (d == nullptr) return;

    const FSaveStats src = MakeSample();
    u8 buf[256];
    const u32 n = SerializeReflected(d, &src, buf, sizeof(buf));
    EXPECT_TRUE(n > 0u);

    FTypeId tid = 0u;
    void* obj = CreateFromBytes(buf, n, &tid);
    EXPECT_TRUE(obj != nullptr);
    if (obj == nullptr) return;
    EXPECT_EQ(tid, d->id);

    const auto* p = static_cast<const FSaveStats*>(obj);
    EXPECT_EQ(p->level, 7);
    EXPECT_NEAR(p->tint.z, 0.75f, 0.0001f);
    EXPECT_TRUE(p->team == ESaveTeam::Green);

    FTypeRegistry::Get().Destroy(tid, obj);
}

// 型 id が一致しない記述子へ復元しようとすると拒否される (誤った型への書き戻し防止)。
ACS_TEST(ReflectSerialize, TypeMismatchRejected) {
    const FTypeDesc* d = AcsTypeDescOf<FSaveStats>();
    EXPECT_TRUE(d != nullptr);
    if (d == nullptr) return;

    const FSaveStats src = MakeSample();
    u8 buf[256];
    const u32 n = SerializeReflected(d, &src, buf, sizeof(buf));

    // reflect_tests.cpp の FReflTransform (別型) の記述子へ流し込むと 0 (型不一致)。
    const FTypeDesc* other = FTypeRegistry::Get().FindByName("FReflTransform");
    EXPECT_TRUE(other != nullptr);
    if (other != nullptr) {
        u8 dummy[64] = {};
        EXPECT_EQ(DeserializeReflected(other, dummy, buf, n), 0u);
    }
}

// magic 破損は復元拒否 (壊れたデータで実型に書き込まない)。
ACS_TEST(ReflectSerialize, CorruptHeaderRejected) {
    const FTypeDesc* d = AcsTypeDescOf<FSaveStats>();
    if (d == nullptr) { EXPECT_TRUE(false); return; }

    const FSaveStats src = MakeSample();
    u8 buf[256];
    const u32 n = SerializeReflected(d, &src, buf, sizeof(buf));
    EXPECT_TRUE(n > 0u);

    buf[0] ^= 0xFFu;   // magic を壊す
    FSaveStats dst{};
    EXPECT_EQ(DeserializeReflected(d, &dst, buf, n), 0u);

    // 短すぎるデータも安全に 0。
    EXPECT_EQ(DeserializeReflected(d, &dst, buf, 3u), 0u);
}

// cap 不足は 0 を返し、バッファを溢れさせない。
ACS_TEST(ReflectSerialize, RespectsBufferCap) {
    const FTypeDesc* d = AcsTypeDescOf<FSaveStats>();
    if (d == nullptr) { EXPECT_TRUE(false); return; }
    const FSaveStats src = MakeSample();
    u8 small[8];   // header すら入らない
    EXPECT_EQ(SerializeReflected(d, &src, small, sizeof(small)), 0u);
}

// private メンバ (ACS_RPROP の size==0 スキーマ) のコンポーネントは安全にスキップされる。
// → header だけ (12 bytes) を書き、復元しても適用フィールド 0、クラッシュしない。
ACS_TEST(ReflectSerialize, SchemaOnlyFieldsSkipped) {
    AcsRegisterEngineTypes();
    FTypeRegistry& reg = FTypeRegistry::Get();
    const FTypeDesc* sprite = reg.FindByName("FSprite2DComponent");
    EXPECT_TRUE(sprite != nullptr);
    if (sprite == nullptr) return;

    void* obj = reg.Create("FSprite2DComponent");
    EXPECT_TRUE(obj != nullptr);   // 既定構築可能 (INSTANTIABLE)
    if (obj == nullptr) return;

    u8 buf[256];
    const u32 n = SerializeByName("FSprite2DComponent", obj, buf, sizeof(buf));
    EXPECT_EQ(n, 12u);   // magic+type_id+field_count のみ (size==0 の RPROP は除外)

    const u32 applied = DeserializeReflected(sprite, obj, buf, n);
    EXPECT_EQ(applied, 0u);

    reg.Destroy(sprite->id, obj);
}

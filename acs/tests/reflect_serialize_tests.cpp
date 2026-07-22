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

struct FTransientStats {
    i32 persistent;
    i32 scratch;
};

ACS_COMPONENT(FSaveStats,
    ACS_RFIELD(FSaveStats, speed, acs::game::EFieldKind::F32),
    ACS_RFIELD(FSaveStats, level, acs::game::EFieldKind::I32),
    ACS_RFIELD(FSaveStats, flags, acs::game::EFieldKind::U32),
    ACS_RFIELD(FSaveStats, alive, acs::game::EFieldKind::Bool),
    ACS_RFIELD(FSaveStats, pos,   acs::game::EFieldKind::Vec2),
    ACS_RFIELD(FSaveStats, tint,  acs::game::EFieldKind::Vec4),
    ACS_RFIELD(FSaveStats, team,  acs::game::EFieldKind::Enum))

ACS_COMPONENT(FTransientStats,
    ACS_RFIELD(FTransientStats, persistent, acs::game::EFieldKind::I32),
    ACS_RFIELD_F(FTransientStats, scratch, acs::game::EFieldKind::I32,
                 acs::game::FIELD_TRANSIENT))

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
    const FReflectDeserializeResult result = TryDeserializeReflected(d, &dst, buf, n);
    EXPECT_TRUE(result.Succeeded());
    EXPECT_EQ(result.FieldsApplied, 7u);
    EXPECT_EQ(result.BytesRead, n);
    EXPECT_EQ(result.SerializedTypeId, d->id);

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

ACS_TEST(ReflectSerialize, TrySerializeReportsSizeWithoutPartialOutput) {
    const FTypeDesc* d = AcsTypeDescOf<FSaveStats>();
    if (d == nullptr) { EXPECT_TRUE(false); return; }
    const FSaveStats src = MakeSample();

    const FReflectSerializeResult query =
        TrySerializeReflected(d, &src, nullptr, 0u);
    EXPECT_EQ(static_cast<u32>(query.Error),
              static_cast<u32>(EReflectSerializeError::BufferTooSmall));
    EXPECT_EQ(query.BytesWritten, 0u);
    EXPECT_EQ(query.FieldsSerialized, 7u);
    EXPECT_TRUE(query.RequiredBytes > 12u);

    u8 tiny[16];
    std::memset(tiny, 0xA5, sizeof(tiny));
    const FReflectSerializeResult insufficient =
        TrySerializeReflected(d, &src, tiny, sizeof(tiny));
    EXPECT_EQ(static_cast<u32>(insufficient.Error),
              static_cast<u32>(EReflectSerializeError::BufferTooSmall));
    EXPECT_EQ(insufficient.RequiredBytes, query.RequiredBytes);
    for (u32 i = 0u; i < sizeof(tiny); ++i) EXPECT_EQ(tiny[i], 0xA5u);

    u8 exact[256];
    const FReflectSerializeResult saved =
        TrySerializeReflected(d, &src, exact, sizeof(exact));
    EXPECT_TRUE(saved.Succeeded());
    EXPECT_EQ(saved.BytesWritten, query.RequiredBytes);
    EXPECT_EQ(SerializeReflected(d, &src, exact, saved.RequiredBytes),
              saved.BytesWritten);
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

// 後半で切れた入力でも、先頭側のフィールドだけを部分適用してはならない。
ACS_TEST(ReflectSerialize, EveryTruncatedPrefixLeavesDestinationUnchanged) {
    const FTypeDesc* d = AcsTypeDescOf<FSaveStats>();
    if (d == nullptr) { EXPECT_TRUE(false); return; }

    const FSaveStats src = MakeSample();
    u8 buf[256];
    const u32 n = SerializeReflected(d, &src, buf, sizeof(buf));
    EXPECT_TRUE(n > 0u);

    for (u32 prefix_size = 0u; prefix_size < n; ++prefix_size) {
        FSaveStats dst{};
        dst.speed = -100.0f;
        dst.level = -200;
        dst.flags = 0x12345678u;
        dst.alive = false;
        dst.pos = FVec2{-3.0f, 9.0f};
        dst.tint = FVec4{9.0f, 8.0f, 7.0f, 6.0f};
        dst.team = ESaveTeam::Red;
        u8 before[sizeof(dst)];
        std::memcpy(before, &dst, sizeof(dst));

        const FReflectDeserializeResult result =
            TryDeserializeReflected(d, &dst, buf, prefix_size);
        EXPECT_TRUE(!result.Succeeded());
        EXPECT_EQ(static_cast<u32>(result.Error),
                  static_cast<u32>(EReflectSerializeError::TruncatedData));
        EXPECT_TRUE(std::memcmp(&dst, before, sizeof(dst)) == 0);
    }
}

ACS_TEST(ReflectSerialize, RejectsDeclaredFieldLimitAndInvalidRecord) {
    const FTypeDesc* d = AcsTypeDescOf<FSaveStats>();
    if (d == nullptr) { EXPECT_TRUE(false); return; }

    const FSaveStats src = MakeSample();
    u8 buf[256];
    const u32 n = SerializeReflected(d, &src, buf, sizeof(buf));
    EXPECT_TRUE(n > 0u);

    const u32 excessive_count = kReflectSerializeMaxFieldCount + 1u;
    std::memcpy(buf + 8u, &excessive_count, sizeof(excessive_count));
    FSaveStats dst{};
    const FReflectDeserializeResult excessive =
        TryDeserializeReflected(d, &dst, buf, n);
    EXPECT_EQ(static_cast<u32>(excessive.Error),
              static_cast<u32>(EReflectSerializeError::FieldLimitExceeded));

    u8 invalid_record[17]{};
    const u32 one_field = 1u;
    std::memcpy(invalid_record, &kReflectSerializeMagic, sizeof(kReflectSerializeMagic));
    std::memcpy(invalid_record + 4u, &d->id, sizeof(d->id));
    std::memcpy(invalid_record + 8u, &one_field, sizeof(one_field));
    invalid_record[12] = 0u; // 空のフィールド名は禁止
    const FReflectDeserializeResult invalid =
        TryDeserializeReflected(d, &dst, invalid_record, sizeof(invalid_record));
    EXPECT_EQ(static_cast<u32>(invalid.Error),
              static_cast<u32>(EReflectSerializeError::InvalidFieldRecord));
    EXPECT_TRUE(std::strcmp(ReflectSerializeErrorName(invalid.Error), "invalid_field_record") == 0);
}

// 不正なプラグイン/動的登録元のメタデータでも、実体範囲外へアクセスしない。
ACS_TEST(ReflectSerialize, RejectsOutOfBoundsReflectionMetadata) {
    const FTypeDesc* d = AcsTypeDescOf<FSaveStats>();
    if (d == nullptr) { EXPECT_TRUE(false); return; }

    FReflectField invalid_field{
        "level", EFieldKind::I32, static_cast<u32>(sizeof(FSaveStats) - 1u),
        static_cast<u32>(sizeof(i32)), FIELD_NONE, {}
    };
    FTypeDesc invalid_descriptor = *d;
    invalid_descriptor.fields = &invalid_field;
    invalid_descriptor.field_count = 1u;

    const FSaveStats src = MakeSample();
    u8 buf[256];
    EXPECT_EQ(SerializeReflected(&invalid_descriptor, &src, buf, sizeof(buf)), 0u);

    const u32 n = SerializeReflected(d, &src, buf, sizeof(buf));
    EXPECT_TRUE(n > 0u);
    FSaveStats dst{};
    u8 before[sizeof(dst)];
    std::memcpy(before, &dst, sizeof(dst));
    const FReflectDeserializeResult result =
        TryDeserializeReflected(&invalid_descriptor, &dst, buf, n);
    EXPECT_EQ(static_cast<u32>(result.Error),
              static_cast<u32>(EReflectSerializeError::InvalidMetadata));
    EXPECT_TRUE(std::memcmp(&dst, before, sizeof(dst)) == 0);

    char unterminated_name[kReflectSerializeMaxFieldNameBytes + 1u];
    std::memset(unterminated_name, 'x', sizeof(unterminated_name));
    invalid_field.name = unterminated_name;
    invalid_field.offset = static_cast<u32>(offsetof(FSaveStats, level));
    EXPECT_EQ(SerializeReflected(&invalid_descriptor, &src, buf, sizeof(buf)), 0u);
}

// factory は破損 payload の既定構築物を成功として返さず、型ID出力もクリアする。
ACS_TEST(ReflectSerialize, CreateFromBytesRejectsCorruptPayloadTransactionally) {
    const FTypeDesc* d = AcsTypeDescOf<FSaveStats>();
    if (d == nullptr) { EXPECT_TRUE(false); return; }

    const FSaveStats src = MakeSample();
    u8 buf[256];
    const u32 n = SerializeReflected(d, &src, buf, sizeof(buf));
    EXPECT_TRUE(n > 12u);

    FTypeId type_id = 0xFFFFFFFFu;
    void* obj = CreateFromBytes(buf, n - 1u, &type_id);
    EXPECT_TRUE(obj == nullptr);
    EXPECT_EQ(type_id, 0u);
}

ACS_TEST(ReflectSerialize, TransientFieldsAreExcluded) {
    const FTypeDesc* d = AcsTypeDescOf<FTransientStats>();
    if (d == nullptr) { EXPECT_TRUE(false); return; }

    const FTransientStats src{42, 99};
    u8 buf[128];
    const u32 n = SerializeReflected(d, &src, buf, sizeof(buf));
    EXPECT_TRUE(n > 12u);

    FTransientStats dst{-1, 777};
    const FReflectDeserializeResult result = TryDeserializeReflected(d, &dst, buf, n);
    EXPECT_TRUE(result.Succeeded());
    EXPECT_EQ(result.FieldsApplied, 1u);
    EXPECT_EQ(dst.persistent, 42);
    EXPECT_EQ(dst.scratch, 777);
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
    const FTypeDesc* sprite = reg.FindByName("ASprite2DComponent");
    EXPECT_TRUE(sprite != nullptr);
    if (sprite == nullptr) return;

    void* obj = reg.Create("ASprite2DComponent");
    EXPECT_TRUE(obj != nullptr);   // 既定構築可能 (INSTANTIABLE)
    if (obj == nullptr) return;

    u8 buf[256];
    const u32 n = SerializeByName("ASprite2DComponent", obj, buf, sizeof(buf));
    EXPECT_EQ(n, 12u);   // magic+type_id+field_count のみ (size==0 の RPROP は除外)

    const u32 applied = DeserializeReflected(sprite, obj, buf, n);
    EXPECT_EQ(applied, 0u);

    reg.Destroy(sprite->id, obj);
}

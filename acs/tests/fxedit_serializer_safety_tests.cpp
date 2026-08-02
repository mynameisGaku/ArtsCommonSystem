// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/ParticleEffectSystem.h"
#include "gameframework/tools/fxedit/FxeditSerializer.h"
#include "foundation/Platform.h"

#include <cstdio>
#include <cwchar>
#include <cstring>
#include <limits>

using namespace acs;
using namespace acs::game;
using namespace acs::game::fxedit;

namespace {

void MakeFxeditTempPath(wchar_t* out, usize capacity, const wchar_t* suffix) noexcept {
    wchar_t directory[512]{};
    const DWORD directory_length =
        ::GetTempPathW(static_cast<DWORD>(sizeof(directory) / sizeof(directory[0])), directory);
    EXPECT_TRUE(directory_length > 0u);
    static volatile LONG serial = 0;
    const LONG value = ::InterlockedIncrement(&serial);
    const int written = std::swprintf(
        out, capacity, L"%lsacs_fxedit_%lu_%ld_%ls.fxedit",
        directory, static_cast<unsigned long>(::GetCurrentProcessId()),
        static_cast<long>(value), suffix);
    EXPECT_TRUE(written > 0);
}

bool WriteRawFile(const wchar_t* path, const char* bytes, u32 size) noexcept {
    HANDLE file = ::CreateFileW(
        path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0u;
    const BOOL write_ok = ::WriteFile(file, bytes, size, &written, nullptr);
    const BOOL flush_ok = ::FlushFileBuffers(file);
    const BOOL close_ok = ::CloseHandle(file);
    return write_ok && written == size && flush_ok && close_ok;
}

bool FileEquals(const wchar_t* path, const char* expected, u32 size) noexcept {
    HANDLE file = ::CreateFileW(
        path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char buffer[128]{};
    DWORD read = 0u;
    const BOOL read_ok = ::ReadFile(file, buffer, sizeof(buffer), &read, nullptr);
    const BOOL close_ok = ::CloseHandle(file);
    return read_ok && close_ok && read == size &&
           std::memcmp(buffer, expected, size) == 0;
}

} // namespace

ACS_TEST(FxeditSerializerSafety, ExplicitLengthRoundTripAndUnknownKey) {
    constexpr char text[] =
        "ACS_FXEDIT 1\n"
        "EMITTER count 1\n"
        "E0 name \"fire\"\n"
        "E0 future_parameter opaque-v2-payload\n"
        "E0 emit_rate 50\n"
        "E0 lifetime_sec 2\n"
        "E0 burst_count 3\n"
        "E0 speed_min 1\n"
        "E0 speed_max 4\n"
        "E0 scale_start 1\n"
        "E0 scale_end 0.25\n"
        "E0 gravity 0 -9.8 0\n"
        "E0 color_start 1 0.5 0.25 1\n"
        "E0 color_end 0.2 0.1 0 0\n"
        "E0 spread_radians 3.14\n";
    FParticleEmitterDef definitions[2]{};
    char names[64]{};
    const FFxeditSerializeResult result = CFxeditSerializer::TryParseText(
        text, sizeof(text) - 1u, definitions, names, sizeof(names), 2u);
    EXPECT_TRUE(result.Succeeded());
    EXPECT_EQ(result.emitter_count, 1u);
    EXPECT_TRUE(std::strcmp(names, "fire") == 0);
    EXPECT_NEAR(definitions[0].emit_rate_per_sec, 50.0f, 1e-4f);
    EXPECT_NEAR(definitions[0].gravity.y, -9.8f, 1e-4f);
    EXPECT_NEAR(definitions[0].color_start.y, 0.5f, 1e-4f);
}

ACS_TEST(FxeditSerializerSafety, MalformedAndEmbeddedNulLeaveOutputsUnchanged) {
    FParticleEmitterDef definitions[2]{};
    definitions[0].emit_rate_per_sec = 777.0f;
    char names[64]{};
    std::memcpy(names, "keep", 5u);
    constexpr char truncated[] =
        "ACS_FXEDIT 1\nEMITTER count 1\nE0 name \"unterminated\n";
    FFxeditSerializeResult result = CFxeditSerializer::TryParseText(
        truncated, sizeof(truncated) - 1u, definitions, names, sizeof(names), 2u);
    EXPECT_EQ(static_cast<i32>(result.error),
              static_cast<i32>(EFxeditSerializeError::InvalidName));
    EXPECT_NEAR(definitions[0].emit_rate_per_sec, 777.0f, 1e-4f);
    EXPECT_TRUE(std::strcmp(names, "keep") == 0);

    const char embedded[] = {
        'A','C','S','_','F','X','E','D','I','T',' ','1','\n',
        'E','M','I','T','T','E','R',' ','c','o','u','n','t',' ','1','\n',
        'E','0',' ','n','a','m','e',' ','"','x','"','\0','\n'
    };
    result = CFxeditSerializer::TryParseText(
        embedded, sizeof(embedded), definitions, names, sizeof(names), 2u);
    EXPECT_EQ(static_cast<i32>(result.error),
              static_cast<i32>(EFxeditSerializeError::EmbeddedNul));
    EXPECT_NEAR(definitions[0].emit_rate_per_sec, 777.0f, 1e-4f);
    EXPECT_TRUE(std::strcmp(names, "keep") == 0);
}

ACS_TEST(FxeditSerializerSafety, RejectsDuplicateNonFiniteAndCapacityOverflow) {
    FParticleEmitterDef definitions[2]{};
    definitions[0].lifetime_sec = 9.0f;
    char names[64]{};
    std::memcpy(names, "before", 7u);
    constexpr char duplicate[] =
        "ACS_FXEDIT 1\nEMITTER count 1\n"
        "E0 lifetime_sec 2\nE0 lifetime_sec 3\n";
    FFxeditSerializeResult result = CFxeditSerializer::TryParseText(
        duplicate, sizeof(duplicate) - 1u,
        definitions, names, sizeof(names), 2u);
    EXPECT_EQ(static_cast<i32>(result.error),
              static_cast<i32>(EFxeditSerializeError::DuplicateKey));
    EXPECT_NEAR(definitions[0].lifetime_sec, 9.0f, 1e-4f);

    constexpr char non_finite[] =
        "ACS_FXEDIT 1\nEMITTER count 1\nE0 emit_rate nan\n";
    result = CFxeditSerializer::TryParseText(
        non_finite, sizeof(non_finite) - 1u,
        definitions, names, sizeof(names), 2u);
    EXPECT_EQ(static_cast<i32>(result.error),
              static_cast<i32>(EFxeditSerializeError::ValueOutOfRange));
    EXPECT_NEAR(definitions[0].lifetime_sec, 9.0f, 1e-4f);

    constexpr char too_many[] = "ACS_FXEDIT 1\nEMITTER count 2\n";
    result = CFxeditSerializer::TryParseText(
        too_many, sizeof(too_many) - 1u,
        definitions, names, 32u, 2u);
    EXPECT_EQ(static_cast<i32>(result.error),
              static_cast<i32>(EFxeditSerializeError::BufferTooSmall));
    EXPECT_TRUE(std::strcmp(names, "before") == 0);
}

ACS_TEST(FxeditSerializerSafety, RejectsVersionEmitterAndLineLimits) {
    FParticleEmitterDef definition{};
    definition.emit_rate_per_sec = 321.0f;
    char name[32]{};
    std::memcpy(name, "stable", 7u);
    constexpr char version[] = "ACS_FXEDIT 2\nEMITTER count 0\n";
    FFxeditSerializeResult result = CFxeditSerializer::TryParseText(
        version, sizeof(version) - 1u, &definition, name, sizeof(name), 1u);
    EXPECT_EQ(static_cast<i32>(result.error),
              static_cast<i32>(EFxeditSerializeError::UnsupportedVersion));

    constexpr char emitter_overflow[] =
        "ACS_FXEDIT 1\nEMITTER count 1025\n";
    result = CFxeditSerializer::TryParseText(
        emitter_overflow, sizeof(emitter_overflow) - 1u,
        &definition, name, sizeof(name), 1u);
    EXPECT_EQ(static_cast<i32>(result.error),
              static_cast<i32>(EFxeditSerializeError::TooManyEmitters));

    char long_line[32u + CFxeditSerializer::kMaxLineLength + 2u]{};
    const int prefix = std::snprintf(
        long_line, sizeof(long_line), "ACS_FXEDIT 1\nEMITTER count 1\n");
    EXPECT_TRUE(prefix > 0);
    std::memset(
        long_line + prefix, 'x', CFxeditSerializer::kMaxLineLength + 1u);
    result = CFxeditSerializer::TryParseText(
        long_line,
        static_cast<usize>(prefix) + CFxeditSerializer::kMaxLineLength + 1u,
        &definition, name, sizeof(name), 1u);
    EXPECT_EQ(static_cast<i32>(result.error),
              static_cast<i32>(EFxeditSerializeError::LineTooLong));
    EXPECT_NEAR(definition.emit_rate_per_sec, 321.0f, 1e-4f);
    EXPECT_TRUE(std::strcmp(name, "stable") == 0);
}

ACS_TEST(FxeditSerializerSafety, EnforcesCurveAndKeyframeLimits) {
    FParticleEmitterDef definition{};
    char name[32]{};
    constexpr char curve_overflow[] =
        "ACS_FXEDIT 1\nEMITTER count 1\nE0 curve 16\n";
    FFxeditSerializeResult result = CFxeditSerializer::TryParseText(
        curve_overflow, sizeof(curve_overflow) - 1u,
        &definition, name, sizeof(name), 1u);
    EXPECT_EQ(static_cast<i32>(result.error),
              static_cast<i32>(EFxeditSerializeError::TooManyCurves));

    char text[32768]{};
    int used = std::snprintf(
        text, sizeof(text), "ACS_FXEDIT 1\nEMITTER count 1\nE0 curve 0\n");
    EXPECT_TRUE(used > 0);
    for (u32 i = 0u; i <= CFxeditSerializer::kMaxKeyframesPerCurve; ++i) {
        const int added = std::snprintf(
            text + used, sizeof(text) - static_cast<usize>(used),
            "E0 keyframe 0 0.5 %u\n", i);
        EXPECT_TRUE(added > 0);
        used += added;
    }
    result = CFxeditSerializer::TryParseText(
        text, static_cast<usize>(used), &definition, name, sizeof(name), 1u);
    EXPECT_EQ(static_cast<i32>(result.error),
              static_cast<i32>(EFxeditSerializeError::TooManyKeyframes));
}

ACS_TEST(FxeditSerializerSafety, SavePrevalidationPreservesExistingFile) {
    wchar_t path[768]{};
    MakeFxeditTempPath(path, sizeof(path) / sizeof(path[0]), L"prevalidate");
    constexpr char original[] = "ORIGINAL";
    EXPECT_TRUE(WriteRawFile(path, original, sizeof(original) - 1u));

    FParticleEmitterDef definition{};
    definition.emit_rate_per_sec = std::numeric_limits<f32>::quiet_NaN();
    const char* names[] = {"bad"};
    const FFxeditSerializeResult result =
        CFxeditSerializer::TrySave(path, &definition, names, 1u);
    EXPECT_EQ(static_cast<i32>(result.error),
              static_cast<i32>(EFxeditSerializeError::ValueOutOfRange));
    EXPECT_TRUE(FileEquals(path, original, sizeof(original) - 1u));
    (void)::DeleteFileW(path);
}

ACS_TEST(FxeditSerializerSafety, AtomicReplaceKeepsOpenReaderSnapshot) {
    wchar_t path[768]{};
    MakeFxeditTempPath(path, sizeof(path) / sizeof(path[0]), L"snapshot");
    constexpr char original[] = "OLD_SNAPSHOT";
    EXPECT_TRUE(WriteRawFile(path, original, sizeof(original) - 1u));
    HANDLE reader = ::CreateFileW(
        path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_TRUE(reader != INVALID_HANDLE_VALUE);

    FParticleEmitterDef definition{};
    definition.emit_rate_per_sec = 12.0f;
    definition.lifetime_sec = 3.0f;
    const char* names[] = {"new"};
    const FFxeditSerializeResult save =
        CFxeditSerializer::TrySave(path, &definition, names, 1u);
    EXPECT_TRUE(save.Succeeded());

    char snapshot[32]{};
    DWORD read = 0u;
    EXPECT_TRUE(::ReadFile(reader, snapshot, sizeof(snapshot), &read, nullptr) != 0);
    EXPECT_EQ(read, static_cast<DWORD>(sizeof(original) - 1u));
    EXPECT_TRUE(std::memcmp(snapshot, original, sizeof(original) - 1u) == 0);
    EXPECT_TRUE(::CloseHandle(reader) != 0);

    FParticleEmitterDef loaded[1]{};
    char loaded_name[32]{};
    const FFxeditSerializeResult load =
        CFxeditSerializer::TryLoad(path, loaded, loaded_name, sizeof(loaded_name), 1u);
    EXPECT_TRUE(load.Succeeded());
    EXPECT_TRUE(std::strcmp(loaded_name, "new") == 0);
    EXPECT_NEAR(loaded[0].emit_rate_per_sec, 12.0f, 1e-4f);
    (void)::DeleteFileW(path);
}

ACS_TEST(FxeditSerializerSafety, AtomicFailurePreservesDestination) {
    wchar_t path[768]{};
    MakeFxeditTempPath(path, sizeof(path) / sizeof(path[0]), L"locked");
    constexpr char original[] = "LOCKED_ORIGINAL";
    EXPECT_TRUE(WriteRawFile(path, original, sizeof(original) - 1u));
    HANDLE blocker = ::CreateFileW(
        path, GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_TRUE(blocker != INVALID_HANDLE_VALUE);

    FParticleEmitterDef definition{};
    const char* names[] = {"replacement"};
    const FFxeditSerializeResult save =
        CFxeditSerializer::TrySave(path, &definition, names, 1u);
    EXPECT_EQ(static_cast<i32>(save.error),
              static_cast<i32>(EFxeditSerializeError::AtomicReplaceFailed));
    EXPECT_TRUE(::CloseHandle(blocker) != 0);
    EXPECT_TRUE(FileEquals(path, original, sizeof(original) - 1u));
    (void)::DeleteFileW(path);
}

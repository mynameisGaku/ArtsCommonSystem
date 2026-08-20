// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework/Material2D.{h,cpp} の検証:
//   マテリアルアセット (.acsmat) = 効果プリセット + パラメータの束ね。
//   - 効果名 ↔ enum マッピング (ドロップダウン用)
//   - .acsmat テキストの書き出し→解析ラウンドトリップ
//   - 解析の堅牢性 (ヘッダ不正・欠落フィールド)
//   - SceneTextLoader の MAT 行が node にマテリアルを焼き込むこと
//   - ANode::SetMaterial / ClearMaterial / HasMaterial
//   GPU 不要 (純 logic。SetEffect の実描画は対象外)。
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Material2D.h"
#include "gameframework/ANode.h"
#include "gameframework/ProjectSettings.h"
#include "gameframework/SceneTextLoader.h"

#include <cstdio>
#include <cstring>

using namespace acs;
using namespace acs::game;

// --- 効果名 ↔ enum -----------------------------------------------------------
ACS_TEST(Material2D, EffectNameRoundTrip) {
    EXPECT_TRUE(SpriteEffectCount() >= 8u);
    // None は必ず 0 番。
    EXPECT_EQ(static_cast<i32>(SpriteEffectAt(0)), static_cast<i32>(ESpriteEffect::None));
    // index と enum 値が一致する (C# ドロップダウン index == enum 値の前提)。
    for (u32 i = 0; i < SpriteEffectCount(); ++i) {
        ESpriteEffect e = SpriteEffectAt(i);
        EXPECT_EQ(static_cast<i32>(e), static_cast<i32>(i));
        // name → enum も往復する。
        EXPECT_EQ(static_cast<i32>(SpriteEffectFromName(SpriteEffectName(e))), static_cast<i32>(e));
    }
    // 大文字小文字無視。
    EXPECT_EQ(static_cast<i32>(SpriteEffectFromName("grayscale")), static_cast<i32>(ESpriteEffect::Grayscale));
    EXPECT_EQ(static_cast<i32>(SpriteEffectFromName("VIGNETTE")), static_cast<i32>(ESpriteEffect::Vignette));
    // 未知は None。
    EXPECT_EQ(static_cast<i32>(SpriteEffectFromName("nope")), static_cast<i32>(ESpriteEffect::None));
}

// --- 新規プリセット + 既定パラメータ ----------------------------------------
ACS_TEST(Material2D, NewPresetsRegistered) {
    EXPECT_TRUE(SpriteEffectCount() >= 13u);   // None..Chromatic
    EXPECT_EQ(static_cast<i32>(SpriteEffectFromName("Invert")),    static_cast<i32>(ESpriteEffect::Invert));
    EXPECT_EQ(static_cast<i32>(SpriteEffectFromName("Sepia")),     static_cast<i32>(ESpriteEffect::Sepia));
    EXPECT_EQ(static_cast<i32>(SpriteEffectFromName("Posterize")), static_cast<i32>(ESpriteEffect::Posterize));
    EXPECT_EQ(static_cast<i32>(SpriteEffectFromName("Scanline")),  static_cast<i32>(ESpriteEffect::Scanline));
    EXPECT_EQ(static_cast<i32>(SpriteEffectFromName("Chromatic")), static_cast<i32>(ESpriteEffect::Chromatic));
}

ACS_TEST(Material2D, DefaultParamsAreSane) {
    // Vignette: 開始半径 < 終了半径、強さ 0..1。
    FEffectParams v = DefaultEffectParams(ESpriteEffect::Vignette);
    EXPECT_TRUE(v.p0 < v.p1);
    EXPECT_TRUE(v.strength > 0.0f && v.strength <= 1.0f);
    // Pixelate: セル数は 2 以上。
    FEffectParams px = DefaultEffectParams(ESpriteEffect::Pixelate);
    EXPECT_TRUE(px.p0 >= 2.0f && px.p1 >= 2.0f);
    // Posterize: 階調数 2 以上。
    EXPECT_TRUE(DefaultEffectParams(ESpriteEffect::Posterize).p0 >= 2.0f);
    // アニメ既定: Wave/HueShift のみ true。
    EXPECT_TRUE(EffectAnimatedByDefault(ESpriteEffect::Wave));
    EXPECT_TRUE(EffectAnimatedByDefault(ESpriteEffect::HueShift));
    EXPECT_FALSE(EffectAnimatedByDefault(ESpriteEffect::Grayscale));
    EXPECT_FALSE(EffectAnimatedByDefault(ESpriteEffect::None));
}

// --- PBR マテリアル: ラウンドトリップ + kind + 後方互換 ----------------------
ACS_TEST(Material2D, PbrRoundTrip) {
    FMaterial2D src;
    src.kind = EMaterialKind::Lit;
    src.pbr.baseColor = FVec4{ 0.8f, 0.2f, 0.1f, 1.0f };
    src.pbr.metallic = 0.9f;
    src.pbr.roughness = 0.25f;
    src.pbr.emissive = FVec3{ 0.1f, 0.05f, 0.0f };
    src.pbr.emissiveStrength = 2.0f;
    src.pbr.normalStrength = 0.7f;
    src.pbr.ao = 0.8f;
    std::snprintf(src.pbr.albedoPath, sizeof(src.pbr.albedoPath), "C:\\img\\brick.png");
    std::snprintf(src.pbr.normalPath, sizeof(src.pbr.normalPath), "C:\\img\\brick_n.png");
    std::snprintf(src.name, sizeof(src.name), "Brick");

    char buf[1536];
    EXPECT_TRUE(WriteAcsmatText(src, buf, sizeof(buf)) > 0u);
    FMaterial2D got = ParseAcsmatText(buf);

    EXPECT_EQ(static_cast<i32>(got.kind), static_cast<i32>(EMaterialKind::Lit));
    EXPECT_NEAR(got.pbr.baseColor.x, 0.8f, 1e-4f);
    EXPECT_NEAR(got.pbr.metallic, 0.9f, 1e-4f);
    EXPECT_NEAR(got.pbr.roughness, 0.25f, 1e-4f);
    EXPECT_NEAR(got.pbr.emissive.x, 0.1f, 1e-4f);
    EXPECT_NEAR(got.pbr.emissiveStrength, 2.0f, 1e-4f);
    EXPECT_NEAR(got.pbr.normalStrength, 0.7f, 1e-4f);
    EXPECT_NEAR(got.pbr.ao, 0.8f, 1e-4f);
    EXPECT_TRUE(std::strcmp(got.pbr.albedoPath, "C:\\img\\brick.png") == 0);
    EXPECT_TRUE(std::strcmp(got.pbr.normalPath, "C:\\img\\brick_n.png") == 0);
}

ACS_TEST(Material2D, ToonRoundTrip) {
    FMaterial2D src;
    src.kind = EMaterialKind::Lit;
    src.pbr.shadingMode      = 1;   // Toon
    src.pbr.shadow1Color     = FVec3{ 0.6f, 0.38f, 0.55f };
    src.pbr.shadow1Threshold = 0.5f;
    src.pbr.shadow2Color     = FVec3{ 0.35f, 0.2f, 0.4f };
    src.pbr.shadow2Threshold = 0.18f;
    src.pbr.rimColor         = FVec3{ 0.6f, 0.85f, 1.0f };
    src.pbr.rimPower         = 3.0f;
    src.pbr.specThreshold    = 0.92f;
    src.pbr.toonSoftness     = 0.04f;

    char buf[2560];
    EXPECT_TRUE(WriteAcsmatText(src, buf, sizeof(buf)) > 0u);
    FMaterial2D got = ParseAcsmatText(buf);
    EXPECT_EQ(got.pbr.shadingMode, 1);
    EXPECT_NEAR(got.pbr.shadow1Color.x, 0.6f, 1e-4f);
    EXPECT_NEAR(got.pbr.shadow1Threshold, 0.5f, 1e-4f);
    EXPECT_NEAR(got.pbr.shadow2Color.z, 0.4f, 1e-4f);
    EXPECT_NEAR(got.pbr.rimColor.y, 0.85f, 1e-4f);
    EXPECT_NEAR(got.pbr.rimPower, 3.0f, 1e-4f);
    EXPECT_NEAR(got.pbr.toonSoftness, 0.04f, 1e-4f);
    // ToLitParams が toon フィールドを引き継ぐ。
    FLitMaterialParams lm = ToLitParams(got.pbr);
    EXPECT_EQ(lm.shadingMode, 1);
    EXPECT_NEAR(lm.shadow1Threshold, 0.5f, 1e-4f);
}

ACS_TEST(Material2D, SubstrateRoundTrip) {
    FMaterial2D src;
    src.kind = EMaterialKind::Lit;
    src.pbr.clearcoat          = 0.8f;
    src.pbr.clearcoatRoughness = 0.15f;
    src.pbr.anisotropy         = -0.6f;
    src.pbr.specularLevel      = 0.7f;
    src.pbr.specularTint       = 0.4f;
    src.pbr.sheen              = 0.5f;
    src.pbr.sheenRoughness     = 0.25f;
    src.pbr.sheenColor         = FVec3{ 0.9f, 0.7f, 0.8f };
    src.pbr.subsurface         = 0.3f;
    src.pbr.subsurfaceColor    = FVec3{ 0.95f, 0.4f, 0.3f };

    char buf[3072];
    EXPECT_TRUE(WriteAcsmatText(src, buf, sizeof(buf)) > 0u);
    FMaterial2D got = ParseAcsmatText(buf);
    EXPECT_NEAR(got.pbr.clearcoat, 0.8f, 1e-4f);
    EXPECT_NEAR(got.pbr.clearcoatRoughness, 0.15f, 1e-4f);
    EXPECT_NEAR(got.pbr.anisotropy, -0.6f, 1e-4f);
    EXPECT_NEAR(got.pbr.specularLevel, 0.7f, 1e-4f);
    EXPECT_NEAR(got.pbr.specularTint, 0.4f, 1e-4f);
    EXPECT_NEAR(got.pbr.sheen, 0.5f, 1e-4f);
    EXPECT_NEAR(got.pbr.sheenRoughness, 0.25f, 1e-4f);
    EXPECT_NEAR(got.pbr.sheenColor.x, 0.9f, 1e-4f);
    EXPECT_NEAR(got.pbr.subsurface, 0.3f, 1e-4f);
    EXPECT_NEAR(got.pbr.subsurfaceColor.y, 0.4f, 1e-4f);
    // ToLitParams が拡張ロブを引き継ぐ。
    FLitMaterialParams lm = ToLitParams(got.pbr);
    EXPECT_NEAR(lm.clearcoat, 0.8f, 1e-4f);
    EXPECT_NEAR(lm.anisotropy, -0.6f, 1e-4f);
    EXPECT_NEAR(lm.subsurfaceColor.x, 0.95f, 1e-4f);
}

ACS_TEST(Material2D, KindDefaultsAndBackCompat) {
    // 新規 (コード構築) は既定 Lit/PBR。
    FMaterial2D fresh;
    EXPECT_EQ(static_cast<i32>(fresh.kind), static_cast<i32>(EMaterialKind::Lit));
    // kind 行が無い «旧» .acsmat は Effect とみなす (後方互換)。
    FMaterial2D old = ParseAcsmatText("ACSMAT 1\neffect Grayscale\nstrength 1\n");
    EXPECT_EQ(static_cast<i32>(old.kind), static_cast<i32>(EMaterialKind::Effect));
    EXPECT_EQ(static_cast<i32>(old.effect), static_cast<i32>(ESpriteEffect::Grayscale));
    // kind pbr 明記なら Lit。
    FMaterial2D lit = ParseAcsmatText("ACSMAT 1\nkind pbr\nmetallic 1\n");
    EXPECT_EQ(static_cast<i32>(lit.kind), static_cast<i32>(EMaterialKind::Lit));
    EXPECT_NEAR(lit.pbr.metallic, 1.0f, 1e-4f);
}

// --- 書き出し → 解析ラウンドトリップ ----------------------------------------
ACS_TEST(Material2D, WriteParseRoundTrip) {
    FMaterial2D src;
    src.effect          = ESpriteEffect::Vignette;
    src.params.strength = 0.8f;
    src.params.p0       = 0.3f;
    src.params.p1       = 1.0f;
    src.params.p2       = 0.0f;
    src.params.color    = FVec4{ 0.1f, 0.2f, 0.3f, 1.0f };
    src.animated        = true;
    std::snprintf(src.name, sizeof(src.name), "Spooky");

    char buf[512];
    const u32 n = WriteAcsmatText(src, buf, sizeof(buf));
    EXPECT_TRUE(n > 0u);

    FMaterial2D got = ParseAcsmatText(buf);
    EXPECT_EQ(static_cast<i32>(got.effect), static_cast<i32>(ESpriteEffect::Vignette));
    EXPECT_NEAR(got.params.strength, 0.8f, 1e-4f);
    EXPECT_NEAR(got.params.p0, 0.3f, 1e-4f);
    EXPECT_NEAR(got.params.p1, 1.0f, 1e-4f);
    EXPECT_NEAR(got.params.color.x, 0.1f, 1e-4f);
    EXPECT_NEAR(got.params.color.w, 1.0f, 1e-4f);
    EXPECT_TRUE(got.animated);
    EXPECT_TRUE(std::strcmp(got.name, "Spooky") == 0);
}

// --- 解析の堅牢性 ------------------------------------------------------------
ACS_TEST(Material2D, ParseRejectsBadHeader) {
    FMaterial2D got = ParseAcsmatText("NOPE 1\neffect Grayscale\n");
    EXPECT_EQ(static_cast<i32>(got.effect), static_cast<i32>(ESpriteEffect::None));  // 既定のまま
}

ACS_TEST(Material2D, ParseToleratesMissingFields) {
    // effect だけ指定、他は既定。CRLF・コメント・空行も許容。
    const char* text =
        "ACSMAT 1\r\n"
        "; a comment\r\n"
        "\r\n"
        "effect Tint\r\n"
        "color 1 0 0 1\r\n";
    FMaterial2D got = ParseAcsmatText(text);
    EXPECT_EQ(static_cast<i32>(got.effect), static_cast<i32>(ESpriteEffect::Tint));
    EXPECT_NEAR(got.params.color.x, 1.0f, 1e-4f);
    EXPECT_NEAR(got.params.color.y, 0.0f, 1e-4f);
    EXPECT_NEAR(got.params.strength, 1.0f, 1e-4f);   // 既定
}

ACS_TEST(Material2D, ParseNullIsSafe) {
    FMaterial2D got = ParseAcsmatText(nullptr);
    EXPECT_EQ(static_cast<i32>(got.effect), static_cast<i32>(ESpriteEffect::None));
}

ACS_TEST(Material2D, CheckedParserAcceptsExplicitLengthWithoutTerminator) {
    constexpr char text[] = "ACSMAT 1\nkind effect\neffect Vignette\nstrength 0.75\n";
    FMaterial2D material;
    std::snprintf(material.name, sizeof(material.name), "Before");
    const FMaterial2DLoadResult result =
        TryParseAcsmatText(text, sizeof(text) - 1u, material);
    EXPECT_TRUE(result.Succeeded());
    EXPECT_EQ(static_cast<i32>(material.kind), static_cast<i32>(EMaterialKind::Effect));
    EXPECT_EQ(static_cast<i32>(material.effect), static_cast<i32>(ESpriteEffect::Vignette));
    EXPECT_NEAR(material.params.strength, 0.75f, 1e-4f);
}

ACS_TEST(Material2D, CheckedParserRejectsEmbeddedNulTransactionally) {
    const char text[] = {
        'A','C','S','M','A','T',' ','1','\n',
        'n','a','m','e',' ','B','a','d','\0',
        'e','f','f','e','c','t',' ','T','i','n','t','\n'
    };
    FMaterial2D material;
    std::snprintf(material.name, sizeof(material.name), "Unchanged");
    material.params.strength = 42.0f;
    const FMaterial2DLoadResult result =
        TryParseAcsmatText(text, sizeof(text), material);
    EXPECT_EQ(static_cast<i32>(result.error),
              static_cast<i32>(EMaterial2DLoadError::EmbeddedNul));
    EXPECT_TRUE(std::strcmp(material.name, "Unchanged") == 0);
    EXPECT_NEAR(material.params.strength, 42.0f, 1e-4f);
}

ACS_TEST(Material2D, CheckedParserRejectsDuplicateAndNonFiniteValues) {
    FMaterial2D material;
    material.pbr.metallic = 0.25f;
    constexpr char duplicate[] =
        "ACSMAT 1\nkind pbr\nmetallic 0.5\nmetallic 0.8\n";
    FMaterial2DLoadResult result =
        TryParseAcsmatText(duplicate, sizeof(duplicate) - 1u, material);
    EXPECT_EQ(static_cast<i32>(result.error),
              static_cast<i32>(EMaterial2DLoadError::DuplicateKey));
    EXPECT_NEAR(material.pbr.metallic, 0.25f, 1e-4f);

    constexpr char non_finite[] = "ACSMAT 1\nkind pbr\nmetallic nan\n";
    result = TryParseAcsmatText(non_finite, sizeof(non_finite) - 1u, material);
    EXPECT_EQ(static_cast<i32>(result.error),
              static_cast<i32>(EMaterial2DLoadError::ValueOutOfRange));
    EXPECT_NEAR(material.pbr.metallic, 0.25f, 1e-4f);
}

ACS_TEST(Material2D, CheckedParserRejectsOutOfRangeIntegerAndLongLine) {
    FMaterial2D material;
    constexpr char bad_integer[] =
        "ACSMAT 1\nkind pbr\nshadingMode 2147483648\n";
    FMaterial2DLoadResult result =
        TryParseAcsmatText(bad_integer, sizeof(bad_integer) - 1u, material);
    EXPECT_EQ(static_cast<i32>(result.error),
              static_cast<i32>(EMaterial2DLoadError::ValueOutOfRange));

    char long_text[9u + 1u + kMaterial2DMaxLineBytes + 1u]{};
    std::memcpy(long_text, "ACSMAT 1\n", 9u);
    std::memset(long_text + 9u, 'x', kMaterial2DMaxLineBytes + 1u);
    result = TryParseAcsmatText(
        long_text, 9u + kMaterial2DMaxLineBytes + 1u, material);
    EXPECT_EQ(static_cast<i32>(result.error),
              static_cast<i32>(EMaterial2DLoadError::LineTooLong));
    EXPECT_EQ(result.line, 2u);
}

ACS_TEST(ProjectSettingsSafety, CheckedLoadIsTransactionalAndRejectsDuplicates) {
    CProjectSettings settings;
    settings.ResetToDefaults();
    EXPECT_TRUE(settings.Set("Game", "WindowWidth", "777"));

    constexpr char duplicate[] =
        "[Game]\n"
        "WindowWidth=800\n"
        "WindowWidth=900\n";
    const FProjectSettingsLoadResult result =
        settings.TryLoadText(duplicate, sizeof(duplicate) - 1u);
    EXPECT_EQ(static_cast<i32>(result.error),
              static_cast<i32>(EProjectSettingsLoadError::DuplicateKey));
    EXPECT_EQ(settings.GetInt("Game", "WindowWidth", -1), 777);
}

ACS_TEST(ProjectSettingsSafety, CheckedLoadRejectsEmbeddedNulAndInvalidBuiltin) {
    CProjectSettings settings;
    settings.ResetToDefaults();
    EXPECT_TRUE(settings.Set("Game", "WindowWidth", "777"));

    const char embedded[] = {
        '[','G','a','m','e',']','\n',
        'W','i','n','d','o','w','W','i','d','t','h','=','8','0','0','\0','0','\n'
    };
    FProjectSettingsLoadResult result =
        settings.TryLoadText(embedded, sizeof(embedded));
    EXPECT_EQ(static_cast<i32>(result.error),
              static_cast<i32>(EProjectSettingsLoadError::EmbeddedNul));
    EXPECT_EQ(settings.GetInt("Game", "WindowWidth", -1), 777);

    constexpr char overflow[] = "[Game]\nWindowWidth=2147483648\n";
    result = settings.TryLoadText(overflow, sizeof(overflow) - 1u);
    EXPECT_EQ(static_cast<i32>(result.error),
              static_cast<i32>(EProjectSettingsLoadError::ValueOutOfRange));
    EXPECT_EQ(settings.GetInt("Game", "WindowWidth", -1), 777);

    constexpr char non_finite[] = "[Rendering]\nExposure=inf\n";
    result = settings.TryLoadText(non_finite, sizeof(non_finite) - 1u);
    EXPECT_EQ(static_cast<i32>(result.error),
              static_cast<i32>(EProjectSettingsLoadError::ValueOutOfRange));
    EXPECT_EQ(settings.GetInt("Game", "WindowWidth", -1), 777);
}

ACS_TEST(ProjectSettingsSafety, UnknownKeysAreCustomAndEmptyInputUsesDefaults) {
    CProjectSettings settings;
    constexpr char custom[] = "[Plugin]\nMode=Experimental\n";
    FProjectSettingsLoadResult result =
        settings.TryLoadText(custom, sizeof(custom) - 1u);
    EXPECT_TRUE(result.Succeeded());
    const FSettingEntry* entry = settings.Find("Plugin", "Mode");
    EXPECT_TRUE(entry != nullptr);
    EXPECT_FALSE(entry->builtin);
    EXPECT_TRUE(std::strcmp(entry->value, "Experimental") == 0);
    EXPECT_TRUE(settings.Find("Rendering", "ClearColor") != nullptr);

    result = settings.TryLoadText("", 0u);
    EXPECT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.used_defaults);
    EXPECT_TRUE(settings.Find("Plugin", "Mode") == nullptr);
    EXPECT_EQ(settings.GetInt("Game", "WindowWidth", -1), 1280);
}

ACS_TEST(ProjectSettingsSafety, TypedGettersRequireCanonicalFiniteValues) {
    CProjectSettings settings;
    settings.ResetToDefaults();
    EXPECT_TRUE(settings.Set("Game", "WindowWidth", "123junk"));
    EXPECT_EQ(settings.GetInt("Game", "WindowWidth", 55), 55);
    EXPECT_TRUE(settings.Set("Rendering", "Exposure", "nan"));
    EXPECT_NEAR(settings.GetFloat("Rendering", "Exposure", 2.0f), 2.0f, 1e-4f);
    EXPECT_TRUE(settings.Set("Rendering", "ClearColor", "1,2,nan"));
    const FVec3 fallback{0.1f, 0.2f, 0.3f};
    const FVec3 color = settings.GetColor("Rendering", "ClearColor", fallback);
    EXPECT_NEAR(color.x, fallback.x, 1e-4f);
    EXPECT_NEAR(color.y, fallback.y, 1e-4f);
    EXPECT_NEAR(color.z, fallback.z, 1e-4f);
}

ACS_TEST(ProjectSettingsSafety, VolumetricCloudLayerSettingsAreBuiltinAndLoadable) {
    CProjectSettings settings;
    settings.ResetToDefaults();
    const FSettingEntry* base = settings.Find("Rendering", "CloudBaseHeight");
    const FSettingEntry* top = settings.Find("Rendering", "CloudTopHeight");
    const FSettingEntry* scale = settings.Find("Rendering", "CloudNoiseScale");
    const FSettingEntry* render_scale = settings.Find("Rendering", "CloudRenderScale");
    const FSettingEntry* reference_mode = settings.Find("Rendering", "CloudReferenceMode");
    EXPECT_TRUE(base != nullptr && base->builtin);
    EXPECT_TRUE(top != nullptr && top->builtin);
    EXPECT_TRUE(scale != nullptr && scale->builtin);
    EXPECT_TRUE(render_scale != nullptr && render_scale->builtin);
    EXPECT_TRUE(reference_mode != nullptr && reference_mode->builtin);
    EXPECT_TRUE(render_scale != nullptr && render_scale->desc != nullptr &&
                std::strstr(render_scale->desc, "0.5〜4.0") != nullptr);
    EXPECT_NEAR(settings.GetFloat("Rendering", "CloudBaseHeight", 0.0f), 1500.0f, 1e-4f);
    EXPECT_NEAR(settings.GetFloat("Rendering", "CloudTopHeight", 0.0f), 4000.0f, 1e-4f);
    EXPECT_NEAR(settings.GetFloat("Rendering", "CloudNoiseScale", 0.0f), 0.035f, 1e-5f);
    EXPECT_NEAR(settings.GetFloat("Rendering", "CloudRenderScale", 0.0f), -1.0f, 1e-5f);
    EXPECT_FALSE(settings.GetBool("Rendering", "CloudReferenceMode", true));

    constexpr char custom_layer[] =
        "[Rendering]\n"
        "CloudBaseHeight=240\n"
        "CloudTopHeight=315\n"
        "CloudNoiseScale=0.018\n"
        "CloudRenderScale=4.0\n"
        "CloudReferenceMode=true\n";
    const FProjectSettingsLoadResult loaded =
        settings.TryLoadText(custom_layer, sizeof(custom_layer) - 1u);
    EXPECT_TRUE(loaded.Succeeded());
    EXPECT_NEAR(settings.GetFloat("Rendering", "CloudBaseHeight", 0.0f), 240.0f, 1e-4f);
    EXPECT_NEAR(settings.GetFloat("Rendering", "CloudTopHeight", 0.0f), 315.0f, 1e-4f);
    EXPECT_NEAR(settings.GetFloat("Rendering", "CloudNoiseScale", 0.0f), 0.018f, 1e-5f);
    EXPECT_NEAR(settings.GetFloat("Rendering", "CloudRenderScale", 0.0f), 4.0f, 1e-5f);
    EXPECT_TRUE(settings.GetBool("Rendering", "CloudReferenceMode", false));
}

// --- ANode のマテリアル状態 ------------------------------------------------
ACS_TEST(ProjectSettingsSafety, AerialPerspectiveIsBuiltinAndSettable) {
    CProjectSettings settings;
    settings.ResetToDefaults();

    const FSettingEntry* aerial =
        settings.Find("Rendering", "AerialPerspective");
    EXPECT_TRUE(aerial != nullptr);
    EXPECT_TRUE(aerial != nullptr && aerial->builtin);
    EXPECT_TRUE(aerial != nullptr &&
                aerial->type == ESettingType::Int);
    EXPECT_EQ(
        settings.GetInt("Rendering", "AerialPerspective", -1), 0);

    EXPECT_TRUE(
        settings.Set("Rendering", "AerialPerspective", "1"));
    EXPECT_EQ(
        settings.GetInt("Rendering", "AerialPerspective", -1), 1);

    constexpr char enabled[] =
        "[Rendering]\n"
        "AerialPerspective=1\n";
    const FProjectSettingsLoadResult loaded =
        settings.TryLoadText(enabled, sizeof(enabled) - 1u);
    EXPECT_TRUE(loaded.Succeeded());
    EXPECT_EQ(
        settings.GetInt("Rendering", "AerialPerspective", -1), 1);
}

ACS_TEST(Material2D, NodeSetClearMaterial) {
    ANode node;
    EXPECT_FALSE(node.HasMaterial());

    FMaterial2D mat;
    mat.effect = ESpriteEffect::HueShift;
    mat.params.strength = 45.0f;
    node.SetMaterial(mat);
    EXPECT_TRUE(node.HasMaterial());

    node.ClearMaterial();
    EXPECT_FALSE(node.HasMaterial());
}

// --- 共有マテリアルクロック --------------------------------------------------
ACS_TEST(Material2D, MaterialClockGetSet) {
    SetMaterialClock(12.5f);
    EXPECT_NEAR(MaterialClock(), 12.5f, 1e-4f);
    SetMaterialClock(0.0f);
    EXPECT_NEAR(MaterialClock(), 0.0f, 1e-4f);
}

// --- SceneTextLoader の MAT 行 ----------------------------------------------
// MAT 行はファイルパスを指すので、ここでは LoadAcsmatFile を介さない単体検証として
// node に直接 SetMaterial するパス (上の NodeSetClearMaterial) でカバー済み。
// ここでは「MAT 行があってもパースが落ちず、他行が正しく読めること」を確認する。
ACS_TEST(Material2D, SceneWithMatLineParses) {
    const char* scene =
        "ACSCENE v1\n"
        "2\n"
        "1 -1 0 0 0 1 1 48 0.5 0.6 0.7 1 A\n"
        "2 -1 10 0 0 1 1 48 0.2 0.3 0.4 1 B\n"
        "MAT 1 C:\\does\\not\\exist.acsmat\n";   // 存在しないパス → SetMaterial されないが落ちない
    ANode root;
    FSceneBounds b = LoadAcsceneText(scene, root, nullptr, nullptr);
    EXPECT_TRUE(b.valid);
    EXPECT_EQ(root.ChildCount(), 2u);   // MAT 行があってもノードは正しく読める
}

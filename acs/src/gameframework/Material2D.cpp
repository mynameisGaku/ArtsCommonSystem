// SPDX-License-Identifier: Apache-2.0
// マテリアルアセット (.acsmat) の解析 / 書き出し / 適用。
#include "gameframework/Material2D.h"

#include "container/Array.h"

#include <cstdio>
#include <cstring>

namespace acs::game {

namespace {

// 効果プリセットの一覧 (enum 値と名前)。ドロップダウン順 = この並び。
struct EffectEntry { ESpriteEffect effect; const char* name; };
const EffectEntry kEffects[] = {
    { ESpriteEffect::None,       "None"       },
    { ESpriteEffect::Grayscale,  "Grayscale"  },
    { ESpriteEffect::Tint,       "Tint"       },
    { ESpriteEffect::Vignette,   "Vignette"   },
    { ESpriteEffect::Wave,       "Wave"       },
    { ESpriteEffect::Pixelate,   "Pixelate"   },
    { ESpriteEffect::HueShift,   "HueShift"   },
    { ESpriteEffect::Brightness, "Brightness" },
    { ESpriteEffect::Invert,     "Invert"     },
    { ESpriteEffect::Sepia,      "Sepia"      },
    { ESpriteEffect::Posterize,  "Posterize"  },
    { ESpriteEffect::Scanline,   "Scanline"   },
    { ESpriteEffect::Chromatic,  "Chromatic"  },
};
constexpr u32 kEffectCount = static_cast<u32>(sizeof(kEffects) / sizeof(kEffects[0]));

// ASCII の大文字小文字を無視して比較する (STL の strcasecmp は使わない)。
bool IEquals(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

} // namespace

FLitMaterialParams ToLitParams(const FPbrParams2D& q) noexcept {
    FLitMaterialParams lm;
    lm.baseColor = q.baseColor; lm.metallic = q.metallic; lm.roughness = q.roughness;
    lm.normalStrength = q.normalStrength; lm.ao = q.ao;
    lm.emissive = q.emissive; lm.emissiveStrength = q.emissiveStrength;
    lm.shadingMode = q.shadingMode;
    lm.shadow1Color = q.shadow1Color; lm.shadow1Threshold = q.shadow1Threshold;
    lm.shadow2Color = q.shadow2Color; lm.shadow2Threshold = q.shadow2Threshold;
    lm.rimColor = q.rimColor; lm.rimPower = q.rimPower;
    lm.specColor = q.specColor; lm.specThreshold = q.specThreshold;
    lm.toonSoftness = q.toonSoftness;
    lm.clearcoat = q.clearcoat; lm.clearcoatRoughness = q.clearcoatRoughness;
    lm.anisotropy = q.anisotropy; lm.specularLevel = q.specularLevel; lm.specularTint = q.specularTint;
    lm.sheen = q.sheen; lm.sheenRoughness = q.sheenRoughness; lm.sheenColor = q.sheenColor;
    lm.subsurface = q.subsurface; lm.subsurfaceColor = q.subsurfaceColor;
    return lm;
}

u32 SpriteEffectCount() noexcept { return kEffectCount; }

const char* SpriteEffectName(ESpriteEffect e) noexcept {
    for (u32 i = 0; i < kEffectCount; ++i)
        if (kEffects[i].effect == e) return kEffects[i].name;
    return "None";
}

const char* SpriteEffectNameAt(u32 index) noexcept {
    return index < kEffectCount ? kEffects[index].name : "None";
}

ESpriteEffect SpriteEffectAt(u32 index) noexcept {
    return index < kEffectCount ? kEffects[index].effect : ESpriteEffect::None;
}

ESpriteEffect SpriteEffectFromName(const char* name) noexcept {
    for (u32 i = 0; i < kEffectCount; ++i)
        if (IEquals(kEffects[i].name, name)) return kEffects[i].effect;
    return ESpriteEffect::None;
}

FEffectParams DefaultEffectParams(ESpriteEffect e) noexcept {
    FEffectParams p;   // 既定: strength=1, p*=0, color=白
    switch (e) {
        case ESpriteEffect::Grayscale:  p.strength = 1.0f; break;
        case ESpriteEffect::Tint:       p.strength = 1.0f; p.color = FVec4{ 1.0f, 0.5f, 0.3f, 1.0f }; break;
        case ESpriteEffect::Vignette:   p.strength = 0.7f; p.p0 = 0.4f; p.p1 = 1.0f; p.color = FVec4{ 0, 0, 0, 1 }; break;
        case ESpriteEffect::Wave:       p.strength = 0.03f; p.p0 = 12.0f; p.p1 = 3.0f; break;
        case ESpriteEffect::Pixelate:   p.strength = 1.0f; p.p0 = 24.0f; p.p1 = 24.0f; break;
        case ESpriteEffect::HueShift:   p.strength = 0.0f; p.p0 = 90.0f; break;        // p0=90°/s でアニメ
        case ESpriteEffect::Brightness: p.strength = 1.2f; p.p0 = 1.3f; break;          // 明るさ/コントラスト
        case ESpriteEffect::Invert:     p.strength = 1.0f; break;
        case ESpriteEffect::Sepia:      p.strength = 1.0f; break;
        case ESpriteEffect::Posterize:  p.strength = 1.0f; p.p0 = 4.0f; break;          // 4 階調
        case ESpriteEffect::Scanline:   p.strength = 0.4f; p.p0 = 120.0f; break;        // 120 本
        case ESpriteEffect::Chromatic:  p.strength = 1.0f; p.p0 = 0.01f; break;         // UV 0.01 ずらし
        case ESpriteEffect::None:
        default: break;
    }
    return p;
}

bool EffectAnimatedByDefault(ESpriteEffect e) noexcept {
    return e == ESpriteEffect::Wave || e == ESpriteEffect::HueShift;
}

FMaterial2D ParseAcsmatText(const char* text) noexcept {
    FMaterial2D mat;
    if (text == nullptr) return mat;

    const char* p = text;
    char line[512];
    auto read_line = [&](char* out, int outsz) -> bool {
        if (*p == '\0') return false;
        int k = 0;
        while (*p != '\0' && *p != '\n') { if (k < outsz - 1) out[k++] = *p; ++p; }
        // 末尾 CR を落とす (CRLF 対応)
        if (k > 0 && out[k - 1] == '\r') --k;
        out[k] = '\0';
        if (*p == '\n') ++p;
        return true;
    };

    if (!read_line(line, sizeof(line)) || std::strncmp(line, "ACSMAT", 6) != 0) return mat;

    bool kind_specified = false;   // 後方互換: kind 行が無い旧ファイルは Effect とみなす
    while (read_line(line, sizeof(line))) {
        if (line[0] == '\0' || line[0] == ';' || line[0] == '#') continue;
        // "key rest..." に分ける
        char key[32] = {};
        int kn = 0;
        const char* s = line;
        while (*s == ' ' || *s == '\t') ++s;
        while (*s && *s != ' ' && *s != '\t' && kn < 31) key[kn++] = *s++;
        key[kn] = '\0';
        while (*s == ' ' || *s == '\t') ++s;            // s = 値の先頭

        if (IEquals(key, "name")) {
            std::snprintf(mat.name, sizeof(mat.name), "%s", s);
        } else if (IEquals(key, "kind")) {
            mat.kind = (IEquals(s, "pbr") || IEquals(s, "lit")) ? EMaterialKind::Lit
                                                                : EMaterialKind::Effect;
            kind_specified = true;
        // --- Effect ---
        } else if (IEquals(key, "effect")) {
            mat.effect = SpriteEffectFromName(s);
        } else if (IEquals(key, "strength")) {
            std::sscanf(s, "%f", &mat.params.strength);
        } else if (IEquals(key, "p0")) {
            std::sscanf(s, "%f", &mat.params.p0);
        } else if (IEquals(key, "p1")) {
            std::sscanf(s, "%f", &mat.params.p1);
        } else if (IEquals(key, "p2")) {
            std::sscanf(s, "%f", &mat.params.p2);
        } else if (IEquals(key, "color")) {
            // 4 成分そろったときだけ反映する (部分指定で w 等に既定値が残るのを防ぐ)。
            FVec4 c = mat.params.color;
            if (std::sscanf(s, "%f %f %f %f", &c.x, &c.y, &c.z, &c.w) == 4) mat.params.color = c;
        } else if (IEquals(key, "animated")) {
            int v = 0; std::sscanf(s, "%d", &v); mat.animated = (v != 0);
        // --- PBR (Lit) ---
        } else if (IEquals(key, "baseColor")) {
            FVec4 c = mat.pbr.baseColor;
            if (std::sscanf(s, "%f %f %f %f", &c.x, &c.y, &c.z, &c.w) == 4) mat.pbr.baseColor = c;
        } else if (IEquals(key, "metallic")) {
            std::sscanf(s, "%f", &mat.pbr.metallic);
        } else if (IEquals(key, "roughness")) {
            std::sscanf(s, "%f", &mat.pbr.roughness);
        } else if (IEquals(key, "emissive")) {
            FVec3 e = mat.pbr.emissive;
            if (std::sscanf(s, "%f %f %f", &e.x, &e.y, &e.z) == 3) mat.pbr.emissive = e;
        } else if (IEquals(key, "emissiveStrength")) {
            std::sscanf(s, "%f", &mat.pbr.emissiveStrength);
        } else if (IEquals(key, "normalStrength")) {
            std::sscanf(s, "%f", &mat.pbr.normalStrength);
        } else if (IEquals(key, "ao")) {
            std::sscanf(s, "%f", &mat.pbr.ao);
        // --- トゥーン ---
        } else if (IEquals(key, "shadingMode")) {
            int v = 0; std::sscanf(s, "%d", &v); mat.pbr.shadingMode = v;
        } else if (IEquals(key, "shadow1Color")) {
            FVec3 c = mat.pbr.shadow1Color;
            if (std::sscanf(s, "%f %f %f", &c.x, &c.y, &c.z) == 3) mat.pbr.shadow1Color = c;
        } else if (IEquals(key, "shadow1Threshold")) {
            std::sscanf(s, "%f", &mat.pbr.shadow1Threshold);
        } else if (IEquals(key, "shadow2Color")) {
            FVec3 c = mat.pbr.shadow2Color;
            if (std::sscanf(s, "%f %f %f", &c.x, &c.y, &c.z) == 3) mat.pbr.shadow2Color = c;
        } else if (IEquals(key, "shadow2Threshold")) {
            std::sscanf(s, "%f", &mat.pbr.shadow2Threshold);
        } else if (IEquals(key, "rimColor")) {
            FVec3 c = mat.pbr.rimColor;
            if (std::sscanf(s, "%f %f %f", &c.x, &c.y, &c.z) == 3) mat.pbr.rimColor = c;
        } else if (IEquals(key, "rimPower")) {
            std::sscanf(s, "%f", &mat.pbr.rimPower);
        } else if (IEquals(key, "specColor")) {
            FVec3 c = mat.pbr.specColor;
            if (std::sscanf(s, "%f %f %f", &c.x, &c.y, &c.z) == 3) mat.pbr.specColor = c;
        } else if (IEquals(key, "specThreshold")) {
            std::sscanf(s, "%f", &mat.pbr.specThreshold);
        } else if (IEquals(key, "toonSoftness")) {
            std::sscanf(s, "%f", &mat.pbr.toonSoftness);
        // --- Substrate 風 拡張 ---
        } else if (IEquals(key, "clearcoat")) {
            std::sscanf(s, "%f", &mat.pbr.clearcoat);
        } else if (IEquals(key, "clearcoatRoughness")) {
            std::sscanf(s, "%f", &mat.pbr.clearcoatRoughness);
        } else if (IEquals(key, "anisotropy")) {
            std::sscanf(s, "%f", &mat.pbr.anisotropy);
        } else if (IEquals(key, "specularLevel")) {
            std::sscanf(s, "%f", &mat.pbr.specularLevel);
        } else if (IEquals(key, "specularTint")) {
            std::sscanf(s, "%f", &mat.pbr.specularTint);
        } else if (IEquals(key, "sheen")) {
            std::sscanf(s, "%f", &mat.pbr.sheen);
        } else if (IEquals(key, "sheenRoughness")) {
            std::sscanf(s, "%f", &mat.pbr.sheenRoughness);
        } else if (IEquals(key, "sheenColor")) {
            FVec3 c = mat.pbr.sheenColor;
            if (std::sscanf(s, "%f %f %f", &c.x, &c.y, &c.z) == 3) mat.pbr.sheenColor = c;
        } else if (IEquals(key, "subsurface")) {
            std::sscanf(s, "%f", &mat.pbr.subsurface);
        } else if (IEquals(key, "subsurfaceColor")) {
            FVec3 c = mat.pbr.subsurfaceColor;
            if (std::sscanf(s, "%f %f %f", &c.x, &c.y, &c.z) == 3) mat.pbr.subsurfaceColor = c;
        } else if (IEquals(key, "albedo")) {
            std::snprintf(mat.pbr.albedoPath, sizeof(mat.pbr.albedoPath), "%s", s);
        } else if (IEquals(key, "normal")) {
            std::snprintf(mat.pbr.normalPath, sizeof(mat.pbr.normalPath), "%s", s);
        }
    }
    if (!kind_specified) mat.kind = EMaterialKind::Effect;   // kind 行が無い旧 .acsmat
    return mat;
}

u32 WriteAcsmatText(const FMaterial2D& mat, char* buf, u32 buf_size) noexcept {
    if (buf == nullptr || buf_size == 0) return 0;
    // 両ブロック (effect/pbr) を書いてラウンドトリップを無損失にする。kind が使う側を選ぶ。
    int cur = std::snprintf(
        buf, buf_size,
        "ACSMAT 1\n"
        "name %s\n"
        "kind %s\n"
        "effect %s\n"
        "strength %.4f\n"
        "p0 %.4f\n"
        "p1 %.4f\n"
        "p2 %.4f\n"
        "color %.4f %.4f %.4f %.4f\n"
        "animated %d\n"
        "baseColor %.4f %.4f %.4f %.4f\n"
        "metallic %.4f\n"
        "roughness %.4f\n"
        "emissive %.4f %.4f %.4f\n"
        "emissiveStrength %.4f\n"
        "normalStrength %.4f\n"
        "ao %.4f\n"
        "shadingMode %d\n"
        "shadow1Color %.4f %.4f %.4f\n"
        "shadow1Threshold %.4f\n"
        "shadow2Color %.4f %.4f %.4f\n"
        "shadow2Threshold %.4f\n"
        "rimColor %.4f %.4f %.4f\n"
        "rimPower %.4f\n"
        "specColor %.4f %.4f %.4f\n"
        "specThreshold %.4f\n"
        "toonSoftness %.4f\n"
        "clearcoat %.4f\n"
        "clearcoatRoughness %.4f\n"
        "anisotropy %.4f\n"
        "specularLevel %.4f\n"
        "specularTint %.4f\n"
        "sheen %.4f\n"
        "sheenRoughness %.4f\n"
        "sheenColor %.4f %.4f %.4f\n"
        "subsurface %.4f\n"
        "subsurfaceColor %.4f %.4f %.4f\n",
        mat.name,
        mat.kind == EMaterialKind::Lit ? "pbr" : "effect",
        SpriteEffectName(mat.effect),
        mat.params.strength, mat.params.p0, mat.params.p1, mat.params.p2,
        mat.params.color.x, mat.params.color.y, mat.params.color.z, mat.params.color.w,
        mat.animated ? 1 : 0,
        mat.pbr.baseColor.x, mat.pbr.baseColor.y, mat.pbr.baseColor.z, mat.pbr.baseColor.w,
        mat.pbr.metallic, mat.pbr.roughness,
        mat.pbr.emissive.x, mat.pbr.emissive.y, mat.pbr.emissive.z,
        mat.pbr.emissiveStrength, mat.pbr.normalStrength, mat.pbr.ao,
        mat.pbr.shadingMode,
        mat.pbr.shadow1Color.x, mat.pbr.shadow1Color.y, mat.pbr.shadow1Color.z, mat.pbr.shadow1Threshold,
        mat.pbr.shadow2Color.x, mat.pbr.shadow2Color.y, mat.pbr.shadow2Color.z, mat.pbr.shadow2Threshold,
        mat.pbr.rimColor.x, mat.pbr.rimColor.y, mat.pbr.rimColor.z, mat.pbr.rimPower,
        mat.pbr.specColor.x, mat.pbr.specColor.y, mat.pbr.specColor.z, mat.pbr.specThreshold,
        mat.pbr.toonSoftness,
        mat.pbr.clearcoat, mat.pbr.clearcoatRoughness, mat.pbr.anisotropy,
        mat.pbr.specularLevel, mat.pbr.specularTint,
        mat.pbr.sheen, mat.pbr.sheenRoughness,
        mat.pbr.sheenColor.x, mat.pbr.sheenColor.y, mat.pbr.sheenColor.z,
        mat.pbr.subsurface,
        mat.pbr.subsurfaceColor.x, mat.pbr.subsurfaceColor.y, mat.pbr.subsurfaceColor.z);
    if (cur < 0) return 0;
    if (static_cast<u32>(cur) >= buf_size) return buf_size - 1;
    // テクスチャパスは設定済みのときだけ書く (行末まで)。
    if (mat.pbr.albedoPath[0] != '\0') {
        const int w = std::snprintf(buf + cur, buf_size - static_cast<u32>(cur),
                                    "albedo %s\n", mat.pbr.albedoPath);
        if (w > 0 && static_cast<u32>(cur + w) < buf_size) cur += w;
    }
    if (mat.pbr.normalPath[0] != '\0') {
        const int w = std::snprintf(buf + cur, buf_size - static_cast<u32>(cur),
                                    "normal %s\n", mat.pbr.normalPath);
        if (w > 0 && static_cast<u32>(cur + w) < buf_size) cur += w;
    }
    return static_cast<u32>(cur);
}

bool LoadAcsmatFile(const char* path, FMaterial2D& out) noexcept {
    if (path == nullptr) return false;
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (1 << 20)) { std::fclose(f); return false; }
    TArray<char> buf;
    buf.Resize(static_cast<u32>(sz) + 1);
    const usize rd = std::fread(buf.Data(), 1, static_cast<usize>(sz), f);
    std::fclose(f);
    buf[static_cast<u32>(rd)] = '\0';
    out = ParseAcsmatText(buf.Data());
    return true;
}

bool SaveAcsmatFile(const char* path, const FMaterial2D& mat) noexcept {
    if (path == nullptr) return false;
    char buf[3072];   // effect+pbr+toon+Substrate ブロック + テクスチャパス 2 本 (各 ≤256) を収める
    const u32 n = WriteAcsmatText(mat, buf, sizeof(buf));
    if (n == 0) return false;
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return false;
    const usize wr = std::fwrite(buf, 1, n, f);
    std::fclose(f);
    return wr == n;
}

namespace {
f32 g_MaterialClock = 0.0f;   // アニメ付きマテリアル用の共有経過秒 (スタンドアロン)。
}

void SetMaterialClock(f32 seconds) noexcept { g_MaterialClock = seconds; }
f32  MaterialClock() noexcept { return g_MaterialClock; }

bool ApplyMaterial(FSpriteBatch& sb, const FMaterial2D& mat, f32 time_sec) noexcept {
    if (mat.effect == ESpriteEffect::None) return false;
    FEffectParams p = mat.params;
    if (mat.animated) p.time = time_sec;
    sb.SetEffect(mat.effect, p);
    return true;
}

} // namespace acs::game

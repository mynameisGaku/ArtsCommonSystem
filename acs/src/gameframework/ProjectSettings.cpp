// SPDX-License-Identifier: Apache-2.0
// プロジェクト設定 (INI ストア) 実装
#include "gameframework/ProjectSettings.h"
#include "foundation/Log.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace acs::game {

namespace {

/**
 * ビルトイン設定スキーマ。
 *
 * @details カテゴリ/キー/型/既定値/説明。エディタの Project Settings ウィンドウは
 * このテーブル順に表示する。値の実体は FProjectSettings が文字列で保持する。
 * 項目を増やす場合はここに追記するだけでよい (UI / INI は自動追従)。
 */
const FSettingDesc kSchema[] = {
    // --- Rendering ---
    { "Rendering", "QualityLevel",  ESettingType::Enum,  "High", "Ultra|Highest|High|Medium|Low|Lowest",
      "全体グラフィックス品質プリセット。影/SSAO/SSGI/SSR/IBL/ブルーム/シャープ等を一括設定 (個別キーが優先)" },
    { "Rendering", "MsaaSamples",   ESettingType::Enum,  "8",              "1|2|4|8",
      "MSAA サンプル数。1 は FXAA のみ" },
    { "Rendering", "Exposure",      ESettingType::Float, "1.05",           nullptr,
      "3D トーンマップ前の露出 (明るさ)。大きいほど明るい。1.0 で素通し" },
    { "Rendering", "BloomIntensity",ESettingType::Float, "-1",             nullptr,
      "3D ブルーム強度の上書き。-1 = 品質プリセットに従う。0 でブルームオフ" },
    { "Rendering", "ShadowBias",    ESettingType::Float, "-1",             nullptr,
      "3D 影のデプスバイアス上書き (アクネ対策)。-1 = 品質プリセットに従う" },
    { "Rendering", "SunAzimuth",    ESettingType::Float, "-41",            nullptr,
      "3D 太陽 (主光源) の方位角 (度)。影/陰影/空の太陽方向を一括で回す" },
    { "Rendering", "SunElevation",  ESettingType::Float, "58",             nullptr,
      "3D 太陽 (主光源) の仰角 (度、0=地平・90=真上)。低いほど影が長く夕方らしい" },
    { "Rendering", "SunColor",      ESettingType::Color, "1.0,0.95,0.85",  nullptr,
      "3D 太陽 (主光源) の色。暖色=夕日寄り、寒色=曇天寄り" },
    { "Rendering", "SunIntensity",  ESettingType::Float, "2.35",           nullptr,
      "3D 太陽 (主光源) の強度 (明るさ)。実効ライト色 = SunColor × SunIntensity" },
    { "Rendering", "AmbientColor",  ESettingType::Color, "0.10,0.11,0.13", nullptr,
      "2D ライトのアンビエント (環境光) 色" },
    { "Rendering", "LightHeight",   ESettingType::Float, "90",             nullptr,
      "2D ライトの高さ (px)。法線シェーディングの立体感に影響" },
    { "Rendering", "ClearColor",    ESettingType::Color, "0.07,0.08,0.10", nullptr,
      "ビューポート背景 (クリア) 色" },
    // --- Editor ---
    { "Editor",    "SnapMove",      ESettingType::Float, "10",             nullptr,
      "移動スナップの刻み (world 単位)" },
    { "Editor",    "SnapRotateDeg", ESettingType::Float, "15",             nullptr,
      "回転スナップの刻み (度)" },
    { "Editor",    "SnapScale",     ESettingType::Float, "0.25",           nullptr,
      "スケールスナップの刻み" },
    // --- Physics ---
    { "Physics",   "GravityX",      ESettingType::Float, "0",              nullptr,
      "重力 X (px/s^2)。Play モードの剛体に適用" },
    { "Physics",   "GravityY",      ESettingType::Float, "980",            nullptr,
      "重力 Y (px/s^2、下方向が正)。Play モードの剛体に適用" },
    { "Physics",   "FixedTimestep", ESettingType::Float, "0.0166667",      nullptr,
      "物理の固定タイムステップ (秒)" },
    // --- Game ---
    { "Game",      "DefaultScene",  ESettingType::String, "Assets/main.acscene", nullptr,
      "プロジェクトを開いたとき / ゲーム起動時に読み込むシーン" },
    { "Game",      "WindowWidth",   ESettingType::Int,   "1280",           nullptr,
      "スタンドアロン実行のウィンドウ幅" },
    { "Game",      "WindowHeight",  ESettingType::Int,   "720",            nullptr,
      "スタンドアロン実行のウィンドウ高さ" },
    { "Game",      "WindowTitle",   ESettingType::String, "",              nullptr,
      "スタンドアロン実行のウィンドウタイトル (空 = プロジェクト名)" },
};

/** 文字列を cap-1 で切ってコピーし NUL 終端する。 */
void CopyStr(char* dst, usize cap, const char* src) noexcept {
    if (src == nullptr) { dst[0] = '\0'; return; }
    std::snprintf(dst, cap, "%s", src);
}

/** 行末の空白/改行を落とす。 */
void TrimRight(char* s) noexcept {
    usize n = std::strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = '\0';
}

} // namespace

const FSettingDesc* BuiltinSettingSchema(u32& out_count) noexcept {
    out_count = static_cast<u32>(sizeof(kSchema) / sizeof(kSchema[0]));
    return kSchema;
}

void FProjectSettings::AppendBuiltin(const FSettingDesc& d) noexcept {
    FSettingEntry e{};
    CopyStr(e.category, sizeof(e.category), d.category);
    CopyStr(e.key,      sizeof(e.key),      d.key);
    CopyStr(e.value,    sizeof(e.value),    d.def);
    e.type    = d.type;
    e.options = d.options;
    e.desc    = d.desc;
    e.builtin = true;
    m_Entries.PushBack(e);
}

void FProjectSettings::ResetToDefaults() noexcept {
    m_Entries.Clear();
    u32 n = 0;
    const FSettingDesc* s = BuiltinSettingSchema(n);
    for (u32 i = 0; i < n; ++i) AppendBuiltin(s[i]);
}

namespace {
/** text から 1 行を line (cap) へ取り出し、読み進めた位置を返す (終端で nullptr)。 */
const char* ReadLine(const char* text, char* line, usize cap) noexcept {
    if (text == nullptr || *text == '\0') return nullptr;
    usize n = 0;
    while (*text != '\0' && *text != '\n') {
        if (n + 1 < cap) line[n++] = *text;
        ++text;
    }
    line[n] = '\0';
    return (*text == '\n') ? text + 1 : text;
}
} // namespace

bool FProjectSettings::LoadText(const char* text) noexcept {
    ResetToDefaults();
    if (text == nullptr) return false;
    char line[512];
    char section[32] = {};
    const char* cur = text;
    while ((cur = ReadLine(cur, line, sizeof(line))) != nullptr) {
        TrimRight(line);
        const char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '\0' || *p == ';' || *p == '#') continue;       // 空行/コメント
        if (*p == '[') {                                          // [Section]
            const char* q = std::strchr(p, ']');
            if (q != nullptr && q > p + 1) {
                const usize len = static_cast<usize>(q - p - 1);
                const usize cap = sizeof(section) - 1;
                const usize n   = (len < cap) ? len : cap;
                std::memcpy(section, p + 1, n);
                section[n] = '\0';
            }
            continue;
        }
        const char* eq = std::strchr(p, '=');                     // key=value
        if (eq == nullptr || section[0] == '\0') continue;
        char key[64];
        {
            usize len = static_cast<usize>(eq - p);
            while (len > 0 && (p[len-1] == ' ' || p[len-1] == '\t')) --len;   // key 末尾の空白
            const usize cap = sizeof(key) - 1;
            const usize n   = (len < cap) ? len : cap;
            std::memcpy(key, p, n);
            key[n] = '\0';
        }
        const char* val = eq + 1;
        while (*val == ' ' || *val == '\t') ++val;
        if (!Set(section, key, val)) Add(section, key, val);      // 未知キー = ユーザー定義
    }
    return true;
}

bool FProjectSettings::Load(const char* ini_path) noexcept {
    ResetToDefaults();
    if (ini_path == nullptr || ini_path[0] == '\0') return false;
    FILE* f = std::fopen(ini_path, "rb");
    if (f == nullptr) return true;            // ファイル無し = 初回 (既定値のまま)
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); return true; }
    TArray<char> buf;
    buf.Resize(static_cast<usize>(sz) + 1);
    const usize rd = std::fread(buf.Data(), 1, static_cast<usize>(sz), f);
    std::fclose(f);
    buf[rd] = '\0';
    return LoadText(buf.Data());
}

u32 FProjectSettings::SerializeText(char* out, usize cap) const noexcept {
    if (out == nullptr || cap == 0) return 0;
    usize cur = 0;
    auto append = [&](const char* fmt, const char* a, const char* b) noexcept {
        if (cur >= cap) return;
        const int w = std::snprintf(out + cur, cap - cur, fmt, a, b);
        if (w < 0) return;
        cur = (static_cast<usize>(w) < cap - cur) ? cur + static_cast<usize>(w) : cap - 1;
    };
    append("%s%s", "; ACS Project Settings (エディタの Edit > Project Settings から編集できます)\n", "");
    for (u32 i = 0; i < m_Entries.Size(); ++i) {
        bool first_of_cat = true;
        for (u32 j = 0; j < i; ++j)
            if (std::strcmp(m_Entries[j].category, m_Entries[i].category) == 0) { first_of_cat = false; break; }
        if (!first_of_cat) continue;
        append("\n[%s]%s", m_Entries[i].category, "\n");
        for (u32 j = i; j < m_Entries.Size(); ++j)
            if (std::strcmp(m_Entries[j].category, m_Entries[i].category) == 0)
                append("%s=%s\n", m_Entries[j].key, m_Entries[j].value);
    }
    out[cur < cap ? cur : cap - 1] = '\0';
    return static_cast<u32>(cur);
}

bool FProjectSettings::Save(const char* ini_path) const noexcept {
    if (ini_path == nullptr || ini_path[0] == '\0') return false;
    FILE* f = std::fopen(ini_path, "wb");
    if (f == nullptr) {
        ACS_LOG_WARN("FProjectSettings: 保存先を開けません: %s", ini_path);
        return false;
    }
    std::fprintf(f, "; ACS Project Settings (このファイルはエディタの Project Settings から編集できます)\n");
    // カテゴリの出現順を保ったままセクションごとにまとめて出力する
    for (u32 i = 0; i < m_Entries.Size(); ++i) {
        bool first_of_cat = true;                                  // この index がカテゴリ初出か
        for (u32 j = 0; j < i; ++j)
            if (std::strcmp(m_Entries[j].category, m_Entries[i].category) == 0) { first_of_cat = false; break; }
        if (!first_of_cat) continue;
        std::fprintf(f, "\n[%s]\n", m_Entries[i].category);
        for (u32 j = i; j < m_Entries.Size(); ++j)
            if (std::strcmp(m_Entries[j].category, m_Entries[i].category) == 0)
                std::fprintf(f, "%s=%s\n", m_Entries[j].key, m_Entries[j].value);
    }
    std::fclose(f);
    return true;
}

const FSettingEntry* FProjectSettings::Find(const char* cat, const char* key) const noexcept {
    if (cat == nullptr || key == nullptr) return nullptr;
    for (u32 i = 0; i < m_Entries.Size(); ++i)
        if (std::strcmp(m_Entries[i].category, cat) == 0 && std::strcmp(m_Entries[i].key, key) == 0)
            return &m_Entries[i];
    return nullptr;
}

bool FProjectSettings::Set(const char* cat, const char* key, const char* value) noexcept {
    auto* e = const_cast<FSettingEntry*>(Find(cat, key));
    if (e == nullptr) return false;
    CopyStr(e->value, sizeof(e->value), value);
    return true;
}

bool FProjectSettings::Add(const char* cat, const char* key, const char* value) noexcept {
    if (cat == nullptr || cat[0] == '\0' || key == nullptr || key[0] == '\0') return false;
    if (Set(cat, key, value)) return true;     // 既存なら値更新
    FSettingEntry e{};
    CopyStr(e.category, sizeof(e.category), cat);
    CopyStr(e.key,      sizeof(e.key),      key);
    CopyStr(e.value,    sizeof(e.value),    value);
    e.type    = ESettingType::String;
    e.builtin = false;
    m_Entries.PushBack(e);
    return true;
}

bool FProjectSettings::Remove(const char* cat, const char* key) noexcept {
    for (u32 i = 0; i < m_Entries.Size(); ++i) {
        FSettingEntry& e = m_Entries[i];
        if (std::strcmp(e.category, cat) == 0 && std::strcmp(e.key, key) == 0) {
            if (e.builtin) return false;       // ビルトインは削除不可 (値の変更のみ)
            for (u32 j = i; j + 1 < m_Entries.Size(); ++j)   // 表示順を保って詰める
                m_Entries[j] = m_Entries[j + 1];
            m_Entries.PopBack();
            return true;
        }
    }
    return false;
}

f32 FProjectSettings::GetFloat(const char* cat, const char* key, f32 def) const noexcept {
    const FSettingEntry* e = Find(cat, key);
    if (e == nullptr || e->value[0] == '\0') return def;
    char* end = nullptr;
    const f32 v = std::strtof(e->value, &end);
    return (end != e->value) ? v : def;
}

i32 FProjectSettings::GetInt(const char* cat, const char* key, i32 def) const noexcept {
    const FSettingEntry* e = Find(cat, key);
    if (e == nullptr || e->value[0] == '\0') return def;
    char* end = nullptr;
    const long v = std::strtol(e->value, &end, 10);
    return (end != e->value) ? static_cast<i32>(v) : def;
}

bool FProjectSettings::GetBool(const char* cat, const char* key, bool def) const noexcept {
    const FSettingEntry* e = Find(cat, key);
    if (e == nullptr || e->value[0] == '\0') return def;
    return e->value[0] == '1' || e->value[0] == 't' || e->value[0] == 'T';
}

FVec3 FProjectSettings::GetColor(const char* cat, const char* key, FVec3 def) const noexcept {
    const FSettingEntry* e = Find(cat, key);
    if (e == nullptr) return def;
    f32 r = def.x, g = def.y, b = def.z;
    if (std::sscanf(e->value, "%f , %f , %f", &r, &g, &b) >= 3) return FVec3{ r, g, b };
    return def;
}

const char* FProjectSettings::GetString(const char* cat, const char* key, const char* def) const noexcept {
    const FSettingEntry* e = Find(cat, key);
    return (e != nullptr) ? e->value : def;
}

} // namespace acs::game

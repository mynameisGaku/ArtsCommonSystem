// SPDX-License-Identifier: Apache-2.0
// プロジェクト設定 (INI ストア) 実装
#include "gameframework/ProjectSettings.h"
#include "foundation/Log.h"
#include "foundation/Move.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <limits>

namespace acs::game {

namespace {

/**
 * ビルトイン設定スキーマ。
 *
 * @details カテゴリ/キー/型/既定値/説明。エディタの Project Settings ウィンドウは
 * このテーブル順に表示する。値の実体は CProjectSettings が文字列で保持する。
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
    { "Rendering", "SsaoIntensity", ESettingType::Float, "-1",             nullptr,
      "3D SSAO (環境遮蔽) 強度の上書き。-1 = 品質プリセットに従う。0 で SSAO オフ、大きいほど陰が濃い" },
    { "Rendering", "SsrIntensity",  ESettingType::Float, "-1",             nullptr,
      "3D SSR (画面空間反射) 強度の上書き。-1 = 品質プリセットに従う。0 で SSR オフ、大きいほど反射が強い" },
    { "Rendering", "SsgiIntensity", ESettingType::Float, "-1",             nullptr,
      "3D SSGI (画面空間 1 バウンス間接光) 強度の上書き。-1 = 品質プリセットに従う。0 で SSGI オフ (要 Diligent backend)" },
    { "Rendering", "Taa",           ESettingType::Float, "-1",             nullptr,
      "3D TAA (テンポラル AA) の上書き。-1 = 品質プリセットに従う。0 でオフ、1 でオン。Halton ジッタ + history blend" },
    { "Rendering", "Tonemap",       ESettingType::Int,   "0",              nullptr,
      "3D トーンマッパー。0=ACES Filmic / 1=AgX (彩度・ハイライトが自然) / 2=Reinhard 拡張" },
    { "Rendering", "AutoExposure",  ESettingType::Float, "0",              nullptr,
      "3D 自動露出 (eye adaptation)。0=オフ (Exposure を直接使用) / 1=オン (シーン輝度から自動、Exposure は EV 補正)" },
    { "Rendering", "FogDensity",    ESettingType::Float, "0",              nullptr,
      "3D ボリュメトリック指数ハイトフォグの密度。0=オフ / 0.02〜0.05 で自然な大気感。色は空の地平色に追従" },
    { "Rendering", "SkyMode",       ESettingType::Int,   "0",              nullptr,
      "3D 空モデル。0=CSky (グラデ+解析太陽) / 1=CAtmosphere (物理大気散乱 Rayleigh+Mie、背景と IBL が物理ベースに。要 Diligent)。雲は別の CVolumetricClouds を使い、GPU 経路失敗時だけ CSky の低コスト雲へ落ちる" },
    { "Rendering", "AerialPerspective", ESettingType::Int, "0",            nullptr,
      "3D 物理大気の空気遠近法。0=オフ / 1=オン。距離に応じた散乱と透過率をカメラボリュームで合成" },
    { "Rendering", "CloudCoverage", ESettingType::Float, "0.42",           nullptr,
      "3D ボリュメトリック雲の雲量。0=雲オフ、0.35=快晴、0.42=晴天、0.5以上=多雲。ワールド空間の3D密度、自己影、縁の光、時間再構成を使用" },
    { "Rendering", "CloudDensity",  ESettingType::Float, "1.6",            nullptr,
      "雲の濃さ (輪郭の硬さ)。小さいほど薄くふわっと、大きいほど濃くくっきり" },
    { "Rendering", "CloudWind",     ESettingType::Float, "1.0",            nullptr,
      "雲の水平移流倍率。0=水平移流なし（対流変形は継続）、1.0=毎秒12ワールド単位" },
    { "Rendering", "CloudBaseHeight", ESettingType::Float, "1500",         nullptr,
      "ボリュメトリック雲レイヤー下面の world Y。カメラではなくワールドに固定される" },
    { "Rendering", "CloudTopHeight", ESettingType::Float, "4000",          nullptr,
      "ボリュメトリック雲レイヤー上面の world Y。下面以下は Sky 側で安全に補正される" },
    { "Rendering", "CloudNoiseScale", ESettingType::Float, "0.035",        nullptr,
      "ボリュメトリック雲の水平 world-space ノイズ周波数。小さいほど大きな雲塊になる" },
    { "Rendering", "CloudUpperBaseHeight", ESettingType::Float, "0",        nullptr,
      "下層の上へ重ねる高層雲の下面の world Y。0は高層雲を無効にする" },
    { "Rendering", "CloudUpperTopHeight", ESettingType::Float, "0",         nullptr,
      "高層雲の上面の world Y。下面以下なら高層雲を無効にする" },
    { "Rendering", "CloudUpperCoverageScale", ESettingType::Float, "0.55", nullptr,
      "高層雲の被覆割合。下層雲に対する割合で、薄い巻雲は0.2〜0.6が目安" },
    { "Rendering", "CloudUpperDensityScale", ESettingType::Float, "0.30",  nullptr,
      "高層雲の濃さ。下層雲に対する割合で、低いほど光を通す" },
    { "Rendering", "CloudMaxDistance", ESettingType::Float, "60000",       nullptr,
      "雲を追う最大距離 (m)。地上は40000〜80000が目安。飛行場面では必要な範囲へ広げる" },
    { "Rendering", "CloudFadeFraction", ESettingType::Float, "0.35",       nullptr,
      "最大距離の手前で雲密度を薄める区間の割合。0は終端で即時打ち切り、0.35なら末尾35%で減衰" },
    { "Rendering", "CloudStepGrowth", ESettingType::Float, "0",           nullptr,
      "遠方レイの採取間隔を広げる度合い。0は高品質な一定間隔、1前後で遠方負荷を下げる" },
    { "Rendering", "CloudType", ESettingType::Float, "0.78",             nullptr,
      "目標雲種。0は層雲、0.5は層積雲、1は積雲。1に近く降水が強いと積乱雲形状になる。適用率0ではこの値を使わない" },
    { "Rendering", "CloudTypeInfluence", ESettingType::Float, "0",       nullptr,
      "手続き生成した雲種を目標雲種へ寄せる割合。0は元の模様、1は目標雲種へ固定" },
    { "Rendering", "CloudPrecipitation", ESettingType::Float, "0",       nullptr,
      "目標降水成分。0は晴天、1は強い降水域。雲種と組み合わせると積乱雲のかなとこを作る。適用率0ではこの値を使わない" },
    { "Rendering", "CloudPrecipitationInfluence", ESettingType::Float, "0", nullptr,
      "手続き生成した降水成分を目標値へ寄せる割合。晴天では1にして降水域を抑える" },
    { "Rendering", "CloudAmbientAtBase", ESettingType::Float, "0.26", nullptr,
      "雲底が空から受ける光の割合。曇天や厚い降水雲では小さく、明るい環境では大きくする" },
    { "Rendering", "CloudAmbientAtTop", ESettingType::Float, "0.52", nullptr,
      "雲頂が空から受ける光の割合。雲底より大きくして上下の立体感を保つ" },
    { "Rendering", "CloudGroundContribution", ESettingType::Float, "0.15", nullptr,
      "地面から雲底へ届く照り返しの割合。暗い地面では小さく、海や雪原では大きくする" },
    { "Rendering", "CloudRenderScale", ESettingType::Float, "-1",          nullptr,
      "雲の内部描画品質倍率。-1=品質プリセット、0.5〜4.0。1.0は画面寸法の1/4、4.0で等倍" },
    { "Rendering", "CloudReferenceMode", ESettingType::Bool, "false",       nullptr,
      "雲の原因切り分け用。trueで等倍・512刻み・時間再構成なし。通常利用ではfalse" },
    { "Rendering", "DofFocus",      ESettingType::Float, "0",              nullptr,
      "3D 被写界深度の焦点距離 (カメラからの距離)。0=オフ / >0 でその距離に焦点 (前後がぼける)" },
    { "Rendering", "DofRange",      ESettingType::Float, "5",              nullptr,
      "3D 被写界深度の合焦幅 (この距離差で最大ぼけ)。小さいほど浅い被写界深度" },
    { "Rendering", "DofMax",        ESettingType::Float, "0.010",          nullptr,
      "3D 被写界深度の最大ぼけ半径 (画面高さに対する比率)。画素円形なので解像度・アスペクト比に依存しない" },
    { "Rendering", "GodRays",       ESettingType::Float, "0",              nullptr,
      "3D god rays (光芒/crepuscular rays)。0=オフ / >0 で太陽から放射状の光の筋を加算 (0.3〜0.8 が自然)" },
    { "Rendering", "Vignette",      ESettingType::Float, "0",              nullptr,
      "シネマフィルタ: ビネット (四隅の減光)。0=オフ / 0.2〜0.5 が自然。ゲーム出力向け" },
    { "Rendering", "ChromaticAberration", ESettingType::Float, "0",        nullptr,
      "シネマフィルタ: 色収差 (画面端の RGB ずれ)。0=オフ / 0.002 前後が自然" },
    { "Rendering", "FilmGrain",     ESettingType::Float, "0",              nullptr,
      "シネマフィルタ: フィルムグレイン (粒状ノイズ)。0=オフ / 0.01〜0.03 が自然" },
    { "Rendering", "MotionBlur",    ESettingType::Float, "0",              nullptr,
      "3D モーションブラー。0=オフ / >0 で動き/カメラ移動の軌跡方向にぼかす (1.0 が標準)。要 motion vector (Diligent)" },
    { "Rendering", "SunAzimuth",    ESettingType::Float, "-41",            nullptr,
      "3D 太陽 (主光源) の方位角 (度)。影/陰影/空の太陽方向を一括で回す" },
    { "Rendering", "SunElevation",  ESettingType::Float, "58",             nullptr,
      "3D 太陽 (主光源) の仰角 (度、0=地平・90=真上)。低いほど影が長く夕方らしい" },
    { "Rendering", "SunColor",      ESettingType::Color, "1.0,0.95,0.85",  nullptr,
      "3D 太陽 (主光源) の色。暖色=夕日寄り、寒色=曇天寄り" },
    { "Rendering", "SunIntensity",  ESettingType::Float, "4.5",            nullptr,
      "3D 太陽 (主光源) の強度。実効ライト色 = SunColor × SunIntensity。WickedEngine 流に太陽を強いキーライト"
      "にし、IBL 拡散フィル(×0.45)と合わせ «影が見える=接地» する立体的ライティングに" },
    { "Rendering", "SkyZenith",     ESettingType::Color, "0.16,0.33,0.62", nullptr,
      "3D 空グラデの天頂色。空背景 + IBL 環境光の両方を駆動 (夕焼けはここをオレンジに)" },
    { "Rendering", "SkyHorizon",    ESettingType::Color, "0.62,0.70,0.80", nullptr,
      "3D 空グラデの地平線色。空背景 + IBL 環境光を駆動" },
    { "Rendering", "SkyGround",     ESettingType::Color, "0.20,0.19,0.21", nullptr,
      "3D 空グラデの下半球 (地面方向) 色。IBL の下方向環境光を駆動" },
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

usize BoundedCStringLength(const char* text, usize max_size, bool& terminated) noexcept {
    terminated = false;
    if (text == nullptr) return 0u;
    for (usize i = 0; i <= max_size; ++i) {
        if (text[i] == '\0') {
            terminated = true;
            return i;
        }
    }
    return max_size + 1u;
}

void SkipSpaces(const char*& text) noexcept {
    while (*text == ' ' || *text == '\t') ++text;
}

enum class ENumberParseStatus : u8 { Ok, Invalid, OutOfRange };

ENumberParseStatus ParseFiniteFloat(const char* text, f32& out) noexcept {
    if (text == nullptr) return ENumberParseStatus::Invalid;
    const char* p = text;
    SkipSpaces(p);
    if (*p == '\0') return ENumberParseStatus::Invalid;
    errno = 0;
    char* end = nullptr;
    const f32 value = std::strtof(p, &end);
    if (end == p) return ENumberParseStatus::Invalid;
    const char* tail = end;
    SkipSpaces(tail);
    if (*tail != '\0') return ENumberParseStatus::Invalid;
    if (errno == ERANGE || !std::isfinite(value)) return ENumberParseStatus::OutOfRange;
    out = value;
    return ENumberParseStatus::Ok;
}

ENumberParseStatus ParseI32(const char* text, i32& out) noexcept {
    if (text == nullptr) return ENumberParseStatus::Invalid;
    const char* p = text;
    SkipSpaces(p);
    if (*p == '\0') return ENumberParseStatus::Invalid;
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(p, &end, 10);
    if (end == p) return ENumberParseStatus::Invalid;
    const char* tail = end;
    SkipSpaces(tail);
    if (*tail != '\0') return ENumberParseStatus::Invalid;
    if (errno == ERANGE ||
        value < static_cast<long long>(std::numeric_limits<i32>::min()) ||
        value > static_cast<long long>(std::numeric_limits<i32>::max())) {
        return ENumberParseStatus::OutOfRange;
    }
    out = static_cast<i32>(value);
    return ENumberParseStatus::Ok;
}

ENumberParseStatus ParseColor(const char* text, FVec3& out) noexcept {
    if (text == nullptr) return ENumberParseStatus::Invalid;
    const char* p = text;
    f32 values[3]{};
    for (u32 i = 0; i < 3u; ++i) {
        SkipSpaces(p);
        errno = 0;
        char* end = nullptr;
        values[i] = std::strtof(p, &end);
        if (end == p) return ENumberParseStatus::Invalid;
        if (errno == ERANGE || !std::isfinite(values[i])) return ENumberParseStatus::OutOfRange;
        p = end;
        SkipSpaces(p);
        if (i < 2u) {
            if (*p != ',') return ENumberParseStatus::Invalid;
            ++p;
        }
    }
    SkipSpaces(p);
    if (*p != '\0') return ENumberParseStatus::Invalid;
    out = FVec3{values[0], values[1], values[2]};
    return ENumberParseStatus::Ok;
}

bool IEquals(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return *a == '\0' && *b == '\0';
}

bool IsEnumOption(const char* options, const char* value) noexcept {
    if (options == nullptr || value == nullptr) return false;
    const usize value_length = std::strlen(value);
    const char* begin = options;
    while (*begin != '\0') {
        const char* end = begin;
        while (*end != '\0' && *end != '|') ++end;
        if (static_cast<usize>(end - begin) == value_length &&
            std::memcmp(begin, value, value_length) == 0) {
            return true;
        }
        begin = *end == '|' ? end + 1 : end;
    }
    return false;
}

EProjectSettingsLoadError ValidateSettingValue(
    const FSettingEntry& entry, const char* value) noexcept {
    switch (entry.type) {
        case ESettingType::Float: {
            f32 parsed = 0.0f;
            const ENumberParseStatus status = ParseFiniteFloat(value, parsed);
            return status == ENumberParseStatus::Ok
                ? EProjectSettingsLoadError::None
                : (status == ENumberParseStatus::OutOfRange
                    ? EProjectSettingsLoadError::ValueOutOfRange
                    : EProjectSettingsLoadError::InvalidValue);
        }
        case ESettingType::Int: {
            i32 parsed = 0;
            const ENumberParseStatus status = ParseI32(value, parsed);
            return status == ENumberParseStatus::Ok
                ? EProjectSettingsLoadError::None
                : (status == ENumberParseStatus::OutOfRange
                    ? EProjectSettingsLoadError::ValueOutOfRange
                    : EProjectSettingsLoadError::InvalidValue);
        }
        case ESettingType::Bool:
            return IEquals(value, "true") || IEquals(value, "false") ||
                   std::strcmp(value, "1") == 0 || std::strcmp(value, "0") == 0
                ? EProjectSettingsLoadError::None
                : EProjectSettingsLoadError::InvalidValue;
        case ESettingType::Color: {
            FVec3 parsed{};
            const ENumberParseStatus status = ParseColor(value, parsed);
            return status == ENumberParseStatus::Ok
                ? EProjectSettingsLoadError::None
                : (status == ENumberParseStatus::OutOfRange
                    ? EProjectSettingsLoadError::ValueOutOfRange
                    : EProjectSettingsLoadError::InvalidValue);
        }
        case ESettingType::Enum:
            return IsEnumOption(entry.options, value)
                ? EProjectSettingsLoadError::None
                : EProjectSettingsLoadError::InvalidValue;
        case ESettingType::String:
            return EProjectSettingsLoadError::None;
    }
    return EProjectSettingsLoadError::InvalidValue;
}

bool SeekFileEnd(std::FILE* file) noexcept {
#if defined(_WIN32)
    return ::_fseeki64(file, 0, SEEK_END) == 0;
#else
    return ::fseeko(file, 0, SEEK_END) == 0;
#endif
}

bool SeekFileBegin(std::FILE* file) noexcept {
#if defined(_WIN32)
    return ::_fseeki64(file, 0, SEEK_SET) == 0;
#else
    return ::fseeko(file, 0, SEEK_SET) == 0;
#endif
}

i64 TellFile(std::FILE* file) noexcept {
#if defined(_WIN32)
    return static_cast<i64>(::_ftelli64(file));
#else
    return static_cast<i64>(::ftello(file));
#endif
}

} // namespace

const FSettingDesc* BuiltinSettingSchema(u32& out_count) noexcept {
    out_count = static_cast<u32>(sizeof(kSchema) / sizeof(kSchema[0]));
    return kSchema;
}

void CProjectSettings::ResetToDefaults() noexcept {
    (void)TryResetToDefaults();
}

const char* FProjectSettingsLoadResult::ErrorName(EProjectSettingsLoadError error) noexcept {
    switch (error) {
        case EProjectSettingsLoadError::None: return "None";
        case EProjectSettingsLoadError::NullArgument: return "NullArgument";
        case EProjectSettingsLoadError::InputTooLarge: return "InputTooLarge";
        case EProjectSettingsLoadError::EmbeddedNul: return "EmbeddedNul";
        case EProjectSettingsLoadError::TooManyLines: return "TooManyLines";
        case EProjectSettingsLoadError::LineTooLong: return "LineTooLong";
        case EProjectSettingsLoadError::InvalidSection: return "InvalidSection";
        case EProjectSettingsLoadError::SectionTooLong: return "SectionTooLong";
        case EProjectSettingsLoadError::MissingSection: return "MissingSection";
        case EProjectSettingsLoadError::InvalidSyntax: return "InvalidSyntax";
        case EProjectSettingsLoadError::KeyTooLong: return "KeyTooLong";
        case EProjectSettingsLoadError::ValueTooLong: return "ValueTooLong";
        case EProjectSettingsLoadError::DuplicateKey: return "DuplicateKey";
        case EProjectSettingsLoadError::TooManyEntries: return "TooManyEntries";
        case EProjectSettingsLoadError::InvalidValue: return "InvalidValue";
        case EProjectSettingsLoadError::ValueOutOfRange: return "ValueOutOfRange";
        case EProjectSettingsLoadError::PathTooLong: return "PathTooLong";
        case EProjectSettingsLoadError::FileOpenFailed: return "FileOpenFailed";
        case EProjectSettingsLoadError::FileSizeFailed: return "FileSizeFailed";
        case EProjectSettingsLoadError::FileChanged: return "FileChanged";
        case EProjectSettingsLoadError::FileReadFailed: return "FileReadFailed";
        case EProjectSettingsLoadError::AllocationFailure: return "AllocationFailure";
    }
    return "Unknown";
}

FProjectSettingsLoadResult CProjectSettings::TryResetToDefaults() noexcept {
    FProjectSettingsLoadResult result{};
    u32 n = 0;
    const FSettingDesc* s = BuiltinSettingSchema(n);
    if (n > kProjectSettingsMaxEntries) {
        result.error = EProjectSettingsLoadError::TooManyEntries;
        return result;
    }
    TArray<FSettingEntry> staged;
    if (!staged.TryReserve(n)) {
        result.error = EProjectSettingsLoadError::AllocationFailure;
        return result;
    }
    for (u32 i = 0; i < n; ++i) {
        FSettingEntry entry{};
        CopyStr(entry.category, sizeof(entry.category), s[i].category);
        CopyStr(entry.key, sizeof(entry.key), s[i].key);
        CopyStr(entry.value, sizeof(entry.value), s[i].def);
        entry.type = s[i].type;
        entry.options = s[i].options;
        entry.desc = s[i].desc;
        entry.builtin = true;
        if (!staged.TryAdd(entry)) {
            result.error = EProjectSettingsLoadError::AllocationFailure;
            return result;
        }
    }
    m_Entries = Move(staged);
    result.used_defaults = true;
    return result;
}

bool CProjectSettings::LoadText(const char* text) noexcept {
    bool terminated = false;
    const usize length = BoundedCStringLength(text, kProjectSettingsMaxTextBytes, terminated);
    return terminated && TryLoadText(text, length).Succeeded();
}

FProjectSettingsLoadResult CProjectSettings::TryLoadText(
    const char* text, usize text_size) noexcept {
    FProjectSettingsLoadResult result{};
    result.bytes_read = static_cast<u64>(text_size);
    if (text == nullptr) {
        result.error = EProjectSettingsLoadError::NullArgument;
        return result;
    }
    if (text_size > kProjectSettingsMaxTextBytes) {
        result.error = EProjectSettingsLoadError::InputTooLarge;
        return result;
    }
    if (std::memchr(text, '\0', text_size) != nullptr) {
        result.error = EProjectSettingsLoadError::EmbeddedNul;
        return result;
    }

    CProjectSettings staged;
    result = staged.TryResetToDefaults();
    result.bytes_read = static_cast<u64>(text_size);
    if (!result.Succeeded()) return result;
    result.used_defaults = text_size == 0u;

    struct FSeenSetting {
        char section[32]{};
        char key[64]{};
    };
    FSeenSetting seen[kProjectSettingsMaxEntries]{};
    u32 seen_count = 0u;

    usize offset = 0u;
    u32 line_number = 0u;
    char section[32]{};
    char line[kProjectSettingsMaxLineBytes + 1u]{};
    while (offset < text_size) {
        if (++line_number > kProjectSettingsMaxLines) {
            result.error = EProjectSettingsLoadError::TooManyLines;
            result.line = line_number;
            return result;
        }
        const usize begin = offset;
        while (offset < text_size && text[offset] != '\n') ++offset;
        usize length = offset - begin;
        if (offset < text_size) ++offset;
        if (length > kProjectSettingsMaxLineBytes) {
            result.error = EProjectSettingsLoadError::LineTooLong;
            result.line = line_number;
            return result;
        }
        std::memcpy(line, text + begin, length);
        if (length > 0u && line[length - 1u] == '\r') --length;
        line[length] = '\0';
        char* current = line;
        while (*current == ' ' || *current == '\t') ++current;
        char* last = current + std::strlen(current);
        while (last > current && (last[-1] == ' ' || last[-1] == '\t')) --last;
        *last = '\0';
        if (*current == '\0' || *current == ';' || *current == '#') continue;

        if (*current == '[') {
            const usize current_length = std::strlen(current);
            if (current_length < 3u || current[current_length - 1u] != ']') {
                result.error = EProjectSettingsLoadError::InvalidSection;
                result.line = line_number;
                return result;
            }
            const usize section_length = current_length - 2u;
            if (section_length >= sizeof(section)) {
                result.error = EProjectSettingsLoadError::SectionTooLong;
                result.line = line_number;
                return result;
            }
            std::memcpy(section, current + 1, section_length);
            section[section_length] = '\0';
            continue;
        }

        if (section[0] == '\0') {
            result.error = EProjectSettingsLoadError::MissingSection;
            result.line = line_number;
            return result;
        }
        char* equals = std::strchr(current, '=');
        if (equals == nullptr) {
            result.error = EProjectSettingsLoadError::InvalidSyntax;
            result.line = line_number;
            return result;
        }
        char* key_end = equals;
        while (key_end > current && (key_end[-1] == ' ' || key_end[-1] == '\t')) --key_end;
        const usize key_length = static_cast<usize>(key_end - current);
        if (key_length == 0u) {
            result.error = EProjectSettingsLoadError::InvalidSyntax;
            result.line = line_number;
            return result;
        }
        if (key_length >= 64u) {
            result.error = EProjectSettingsLoadError::KeyTooLong;
            result.line = line_number;
            return result;
        }
        char key[64]{};
        std::memcpy(key, current, key_length);

        char* value = equals + 1;
        while (*value == ' ' || *value == '\t') ++value;
        const usize value_length = std::strlen(value);
        if (value_length >= 192u) {
            result.error = EProjectSettingsLoadError::ValueTooLong;
            result.line = line_number;
            return result;
        }
        if (seen_count >= kProjectSettingsMaxEntries) {
            result.error = EProjectSettingsLoadError::TooManyEntries;
            result.line = line_number;
            return result;
        }
        for (u32 i = 0; i < seen_count; ++i) {
            if (std::strcmp(seen[i].section, section) == 0 &&
                std::strcmp(seen[i].key, key) == 0) {
                result.error = EProjectSettingsLoadError::DuplicateKey;
                result.line = line_number;
                return result;
            }
        }
        CopyStr(seen[seen_count].section, sizeof(seen[seen_count].section), section);
        CopyStr(seen[seen_count].key, sizeof(seen[seen_count].key), key);
        ++seen_count;

        if (const FSettingEntry* existing = staged.Find(section, key)) {
            result.error = ValidateSettingValue(*existing, value);
            if (result.error != EProjectSettingsLoadError::None) {
                result.line = line_number;
                return result;
            }
            if (!staged.Set(section, key, value)) {
                result.error = EProjectSettingsLoadError::InvalidSyntax;
                result.line = line_number;
                return result;
            }
        } else {
            if (staged.Count() >= kProjectSettingsMaxEntries) {
                result.error = EProjectSettingsLoadError::TooManyEntries;
                result.line = line_number;
                return result;
            }
            if (!staged.TryAdd(section, key, value)) {
                result.error = EProjectSettingsLoadError::AllocationFailure;
                result.line = line_number;
                return result;
            }
        }
    }
    *this = Move(staged);
    return result;
}

bool CProjectSettings::Load(const char* ini_path) noexcept {
    return TryLoadFile(ini_path).Succeeded();
}

FProjectSettingsLoadResult CProjectSettings::TryLoadFile(const char* ini_path) noexcept {
    FProjectSettingsLoadResult result{};
    bool terminated = false;
    const usize path_length =
        BoundedCStringLength(ini_path, kProjectSettingsMaxPathBytes, terminated);
    if (ini_path == nullptr) {
        result.error = EProjectSettingsLoadError::NullArgument;
        return result;
    }
    if (!terminated || path_length == 0u) {
        result.error = EProjectSettingsLoadError::PathTooLong;
        return result;
    }

    errno = 0;
    std::FILE* file = std::fopen(ini_path, "rb");
    if (file == nullptr) {
        if (errno == ENOENT) {
            result = TryResetToDefaults();
            result.used_defaults = result.Succeeded();
            return result;
        }
        result.error = EProjectSettingsLoadError::FileOpenFailed;
        return result;
    }
    if (!SeekFileEnd(file)) {
        std::fclose(file);
        result.error = EProjectSettingsLoadError::FileSizeFailed;
        return result;
    }
    const i64 signed_size = TellFile(file);
    if (signed_size < 0 || !SeekFileBegin(file)) {
        std::fclose(file);
        result.error = EProjectSettingsLoadError::FileSizeFailed;
        return result;
    }
    const u64 file_size = static_cast<u64>(signed_size);
    if (file_size > static_cast<u64>(kProjectSettingsMaxTextBytes)) {
        std::fclose(file);
        result.error = EProjectSettingsLoadError::InputTooLarge;
        return result;
    }

    TArray<char> buffer;
    if (!buffer.TrySetNum(static_cast<usize>(file_size))) {
        std::fclose(file);
        result.error = EProjectSettingsLoadError::AllocationFailure;
        return result;
    }
    usize total = 0u;
    while (total < buffer.Num()) {
        const usize count = std::fread(buffer.GetData() + total, 1u, buffer.Num() - total, file);
        if (count == 0u) {
            const bool io_error = std::ferror(file) != 0;
            std::fclose(file);
            result.error = io_error
                ? EProjectSettingsLoadError::FileReadFailed
                : EProjectSettingsLoadError::FileChanged;
            result.bytes_read = static_cast<u64>(total);
            return result;
        }
        total += count;
    }
    const int extra = std::fgetc(file);
    const bool read_error = std::ferror(file) != 0;
    const int close_result = std::fclose(file);
    result.bytes_read = static_cast<u64>(total);
    if (read_error || close_result != 0) {
        result.error = EProjectSettingsLoadError::FileReadFailed;
        return result;
    }
    if (extra != EOF) {
        result.error = EProjectSettingsLoadError::FileChanged;
        return result;
    }
    if (buffer.IsEmpty()) return TryLoadText("", 0u);
    return TryLoadText(buffer.GetData(), buffer.Num());
}

u32 CProjectSettings::SerializeText(char* out, usize cap) const noexcept {
    if (out == nullptr || cap == 0) return 0;
    usize cur = 0;
    auto append = [&](const char* fmt, const char* a, const char* b) noexcept {
        if (cur >= cap) return;
        const int w = std::snprintf(out + cur, cap - cur, fmt, a, b);
        if (w < 0) return;
        cur = (static_cast<usize>(w) < cap - cur) ? cur + static_cast<usize>(w) : cap - 1;
    };
    append("%s%s", "; ACS Project Settings (エディタの Edit > Project Settings から編集できます)\n", "");
    for (u32 i = 0; i < m_Entries.Num(); ++i) {
        bool first_of_cat = true;
        for (u32 j = 0; j < i; ++j)
            if (std::strcmp(m_Entries[j].category, m_Entries[i].category) == 0) { first_of_cat = false; break; }
        if (!first_of_cat) continue;
        append("\n[%s]%s", m_Entries[i].category, "\n");
        for (u32 j = i; j < m_Entries.Num(); ++j)
            if (std::strcmp(m_Entries[j].category, m_Entries[i].category) == 0)
                append("%s=%s\n", m_Entries[j].key, m_Entries[j].value);
    }
    out[cur < cap ? cur : cap - 1] = '\0';
    return static_cast<u32>(cur);
}

bool CProjectSettings::Save(const char* ini_path) const noexcept {
    bool path_terminated = false;
    const usize path_length =
        BoundedCStringLength(ini_path, kProjectSettingsMaxPathBytes, path_terminated);
    if (ini_path == nullptr || !path_terminated || path_length == 0u) return false;
    FILE* f = std::fopen(ini_path, "wb");
    if (f == nullptr) {
        ACS_LOG_WARN("CProjectSettings: 保存先を開けません: %s", ini_path);
        return false;
    }
    bool write_ok =
        std::fprintf(f, "; ACS Project Settings (このファイルはエディタの Project Settings から編集できます)\n") >= 0;
    // カテゴリの出現順を保ったままセクションごとにまとめて出力する
    for (u32 i = 0; i < m_Entries.Num(); ++i) {
        bool first_of_cat = true;                                  // この index がカテゴリ初出か
        for (u32 j = 0; j < i; ++j)
            if (std::strcmp(m_Entries[j].category, m_Entries[i].category) == 0) { first_of_cat = false; break; }
        if (!first_of_cat) continue;
        if (std::fprintf(f, "\n[%s]\n", m_Entries[i].category) < 0) write_ok = false;
        for (u32 j = i; j < m_Entries.Num(); ++j)
            if (std::strcmp(m_Entries[j].category, m_Entries[i].category) == 0)
                if (std::fprintf(f, "%s=%s\n", m_Entries[j].key, m_Entries[j].value) < 0)
                    write_ok = false;
    }
    if (std::fflush(f) != 0) write_ok = false;
    if (std::fclose(f) != 0) write_ok = false;
    return write_ok;
}

const FSettingEntry* CProjectSettings::Find(const char* cat, const char* key) const noexcept {
    if (cat == nullptr || key == nullptr) return nullptr;
    bool category_terminated = false;
    bool key_terminated = false;
    const usize category_length = BoundedCStringLength(cat, 31u, category_terminated);
    const usize key_length = BoundedCStringLength(key, 63u, key_terminated);
    if (!category_terminated || !key_terminated ||
        category_length == 0u || key_length == 0u) {
        return nullptr;
    }
    for (u32 i = 0; i < m_Entries.Num(); ++i)
        if (std::strcmp(m_Entries[i].category, cat) == 0 && std::strcmp(m_Entries[i].key, key) == 0)
            return &m_Entries[i];
    return nullptr;
}

bool CProjectSettings::Set(const char* cat, const char* key, const char* value) noexcept {
    bool value_terminated = value == nullptr;
    const usize value_length =
        value == nullptr ? 0u : BoundedCStringLength(value, 191u, value_terminated);
    if (!value_terminated || value_length > 191u) return false;
    auto* e = const_cast<FSettingEntry*>(Find(cat, key));
    if (e == nullptr) return false;
    CopyStr(e->value, sizeof(e->value), value);
    return true;
}

bool CProjectSettings::Add(const char* cat, const char* key, const char* value) noexcept {
    return TryAdd(cat, key, value);
}

bool CProjectSettings::TryAdd(const char* cat, const char* key, const char* value) noexcept {
    if (cat == nullptr || cat[0] == '\0' || key == nullptr || key[0] == '\0') return false;
    bool category_terminated = false;
    bool key_terminated = false;
    bool value_terminated = value == nullptr;
    const usize category_length = BoundedCStringLength(cat, 31u, category_terminated);
    const usize key_length = BoundedCStringLength(key, 63u, key_terminated);
    const usize value_length =
        value == nullptr ? 0u : BoundedCStringLength(value, 191u, value_terminated);
    if (!category_terminated || !key_terminated || !value_terminated ||
        category_length == 0u || key_length == 0u || value_length > 191u) {
        return false;
    }
    if (Set(cat, key, value)) return true;     // 既存なら値更新
    FSettingEntry e{};
    CopyStr(e.category, sizeof(e.category), cat);
    CopyStr(e.key,      sizeof(e.key),      key);
    CopyStr(e.value,    sizeof(e.value),    value);
    e.type    = ESettingType::String;
    e.builtin = false;
    return m_Entries.TryAdd(e);
}

bool CProjectSettings::Remove(const char* cat, const char* key) noexcept {
    if (Find(cat, key) == nullptr) return false;
    for (u32 i = 0; i < m_Entries.Num(); ++i) {
        FSettingEntry& e = m_Entries[i];
        if (std::strcmp(e.category, cat) == 0 && std::strcmp(e.key, key) == 0) {
            if (e.builtin) return false;       // ビルトインは削除不可 (値の変更のみ)
            for (u32 j = i; j + 1 < m_Entries.Num(); ++j)   // 表示順を保って詰める
                m_Entries[j] = m_Entries[j + 1];
            m_Entries.Pop();
            return true;
        }
    }
    return false;
}

f32 CProjectSettings::GetFloat(const char* cat, const char* key, f32 def) const noexcept {
    const FSettingEntry* e = Find(cat, key);
    if (e == nullptr || e->value[0] == '\0') return def;
    f32 value = 0.0f;
    return ParseFiniteFloat(e->value, value) == ENumberParseStatus::Ok ? value : def;
}

i32 CProjectSettings::GetInt(const char* cat, const char* key, i32 def) const noexcept {
    const FSettingEntry* e = Find(cat, key);
    if (e == nullptr || e->value[0] == '\0') return def;
    i32 value = 0;
    return ParseI32(e->value, value) == ENumberParseStatus::Ok ? value : def;
}

bool CProjectSettings::GetBool(const char* cat, const char* key, bool def) const noexcept {
    const FSettingEntry* e = Find(cat, key);
    if (e == nullptr || e->value[0] == '\0') return def;
    if (std::strcmp(e->value, "1") == 0 || IEquals(e->value, "true")) return true;
    if (std::strcmp(e->value, "0") == 0 || IEquals(e->value, "false")) return false;
    return def;
}

FVec3 CProjectSettings::GetColor(const char* cat, const char* key, FVec3 def) const noexcept {
    const FSettingEntry* e = Find(cat, key);
    if (e == nullptr) return def;
    FVec3 value{};
    return ParseColor(e->value, value) == ENumberParseStatus::Ok ? value : def;
}

const char* CProjectSettings::GetString(const char* cat, const char* key, const char* def) const noexcept {
    const FSettingEntry* e = Find(cat, key);
    return (e != nullptr) ? e->value : def;
}

} // namespace acs::game

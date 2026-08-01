// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — AModelViewerPanel 実装
//
// 仕様の意図は AModelViewerPanel.h を参照。本ファイルでは:
//   ・AEditorPanel 基底 hook (OnInit / DrawUI / OnAssetSelected) の override
//   ・LoadModelAsset / ClearModel の asset path コピー保存
//   ・Lighting / Background / Tonemap / Grid / FBone skeleton の getter/setter
//   ・OnAssetSelected で .mdl/.fbx/.gltf/.glb/.obj を識別して LoadModelAsset
//   ・ImGui ベースの viewport プレースホルダ + control bar 描画
// を実装する。すべて noexcept、STL 不使用、ImGui 依存はこの .cpp に閉じる。
#include "gameframework/tools/modelview/ModelViewerPanel.h"

#include "gameframework/tools/editor_core/EditorWorkspace.h"

#include "foundation/Platform.h"  // Windows API (MultiByteToWideChar) で UTF-8 → UTF-16

#include <imgui.h>

#include <cstring>  // std::strlen / std::strncmp (拡張子比較)

namespace acs::game::modelview {

namespace {

/**
 * ASCII 大文字を小文字化する (拡張子比較用、ASCII 範囲のみ対応)。
 *
 * @param c 変換する文字。
 * @return 'A'..'Z' なら対応する小文字、それ以外はそのまま。
 */
inline char ToLowerAscii(char c) noexcept {
    if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
    return c;
}

/**
 * UTF-8 path の末尾が ext に一致するかを返す (大文字小文字無視)。
 *
 * @details
 * CAssetBrowser::ClassifyByExtension の wchar_t 版と同じロジックを char 版で実装する。
 * OnAssetSelected の引数が UTF-8 char* で来るため、ここで UTF-16 化せず直接 ASCII 比較
 * する (拡張子は ASCII 範囲しか出現しない前提)。
 * @param path 検査対象パス (終端 0 付き、nullptr は false)。
 * @param ext 比較する拡張子 (".mdl" 等、小文字 + 先頭 '.')。
 * @return path の末尾が ext に一致すれば true。
 */
bool EndsWithIgnoreCaseAscii(const char* path, const char* ext) noexcept {
    if (path == nullptr || ext == nullptr) return false;
    const usize p_len = std::strlen(path);
    const usize e_len = std::strlen(ext);
    if (e_len > p_len) return false;
    const char* p_tail = path + (p_len - e_len);
    for (usize i = 0; i < e_len; ++i) {
        if (ToLowerAscii(p_tail[i]) != ToLowerAscii(ext[i])) return false;
    }
    return true;
}

/**
 * UTF-8 path が Mesh 拡張子に該当するかを返す。
 *
 * @details CAssetBrowser::ClassifyByExtension の Mesh 分類と一致させること。
 * @param path 検査対象パス (UTF-8、nullptr / 空文字は false)。
 * @return 拡張子が .mdl/.fbx/.gltf/.glb/.obj のいずれかなら true。
 */
bool IsMeshExtensionAscii(const char* path) noexcept {
    if (path == nullptr || path[0] == '\0') return false;
    if (EndsWithIgnoreCaseAscii(path, ".mdl"))  return true;
    if (EndsWithIgnoreCaseAscii(path, ".fbx"))  return true;
    if (EndsWithIgnoreCaseAscii(path, ".gltf")) return true;
    if (EndsWithIgnoreCaseAscii(path, ".glb"))  return true;
    if (EndsWithIgnoreCaseAscii(path, ".obj"))  return true;
    return false;
}

/**
 * UTF-8 src を UTF-16 dst にコピーする (終端 0 保証)。
 *
 * @details
 * CAssetBrowser.cpp の WideToUtf8 と対称な変換。MultiByteToWideChar を使うため
 * foundation/Platform.h 経由で Windows API が引かれる前提。src が nullptr / 空文字の場合や
 * 変換失敗時は dst[0] = L'\0' にする。
 * @param src 変換元 UTF-8 文字列。
 * @param dst 変換先 UTF-16 バッファ。
 * @param dst_cap dst の容量 (wchar_t 単位)。
 */
void Utf8ToWide(const char* src, wchar_t* dst, int dst_cap) noexcept {
    if (dst == nullptr || dst_cap <= 0) return;
    dst[0] = L'\0';
    if (src == nullptr || src[0] == '\0') return;
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, dst_cap);
    if (n <= 0) {
        dst[0] = L'\0';
        return;
    }
    dst[dst_cap - 1] = L'\0';  // 念のため終端保証
}

/**
 * wchar_t 列の長さを返す (終端含まず)。
 *
 * @details CAssetBrowser.cpp の WLen と同等。
 * @param s 長さを測る文字列 (nullptr は 0)。
 * @return 終端を含まない wchar_t 数。
 */
usize WLenLocal(const wchar_t* s) noexcept {
    if (s == nullptr) return 0;
    usize n = 0;
    while (s[n] != L'\0') ++n;
    return n;
}

/**
 * Tonemap mode を ImGui Combo に渡す表示ラベルに変換する。
 *
 * @param mode tonemap mode (0=ACES / 1=Reinhard / 2=Linear)。
 * @return 対応するラベル文字列。範囲外は "(unknown)"。
 */
const char* TonemapLabel(u32 mode) noexcept {
    switch (mode) {
        case 0: return "ACES";
        case 1: return "Reinhard";
        case 2: return "Linear";
        default: return "(unknown)";
    }
}

} // anonymous namespace

/** 内部 state をデフォルトに戻し CEditorCamera を 3D mode で初期化する。 */
void AModelViewerPanel::Init() noexcept {
    // CEditorCamera を 3D mode で完全初期化 (= target=origin, distance=10,
    // pitch=-30° 等のデフォルト姿勢)。
    m_Camera.Init(acs::game::editor_core::EEditorCameraMode::Mode3D);

    // asset path を空文字に。
    m_AssetPath[0] = L'\0';

    // Lighting / Background / 表示 toggle はヘッダ宣言時の初期化子で既に
    // セット済みだが、Init 多重呼び出しでも確定的な値になるよう明示再設定する。
    m_LightDir          = acs::FVec3{0.3f, -0.7f, 0.6f};
    m_LightColor        = acs::FVec3{1.0f, 1.0f, 1.0f};
    m_IblEnabled        = true;
    m_TonemapMode       = 0u;
    m_BgColor           = acs::FVec4{0.15f, 0.15f, 0.18f, 1.0f};
    m_ShowGrid          = true;
    m_ShowBoneSkeleton = false;

    // 表示 / dock ヒントの初期値。viewport 系 panel は dock 中央に置きたい
    // ことが多いので m_DockedTarget = true (= Workspace 側へのヒント)。
    m_Visible       = true;
    m_DockedTarget = true;
}

/** 内部 state を解放し CEditorCamera を Reset、asset path を空にする。 */
void AModelViewerPanel::Shutdown() noexcept {
    // CEditorCamera は POD だが明示 Reset で確定状態にする。
    m_Camera.Reset();
    m_AssetPath[0] = L'\0';
}

/** asset path をコピーして内部バッファに保存する (null / 空文字なら ClearModel)。 */
void AModelViewerPanel::LoadModelAsset(const wchar_t* asset_path) noexcept {
    if (asset_path == nullptr || asset_path[0] == L'\0') {
        // 空 / null は ClearModel と同じ振る舞い (= 内部バッファ空文字化)。
        ClearModel();
        return;
    }
    // 内部バッファに 1 文字ずつコピー (= STL の wcsncpy_s を避ける ACS 流)。
    // 終端 1 文字を必ず L'\0' で埋めるため `kMaxAssetPathChars - 1` で打ち切り。
    usize i = 0;
    for (; asset_path[i] != L'\0' && (i + 1) < kMaxAssetPathChars; ++i) {
        m_AssetPath[i] = asset_path[i];
    }
    m_AssetPath[i] = L'\0';
}

/** 内部 asset path を空文字に戻し HasModel() を false にする。 */
void AModelViewerPanel::ClearModel() noexcept {
    m_AssetPath[0] = L'\0';
}

/** model asset がセットされているか (asset path が空文字でないか) を返す。 */
bool AModelViewerPanel::HasModel() const noexcept {
    return m_AssetPath[0] != L'\0';
}

/** 現在の asset path を返す (未設定 / Clear 後も nullptr ではなく L"")。 */
const wchar_t* AModelViewerPanel::CurrentAssetPath() const noexcept {
    // 常に終端 wchar_t* を返す (空文字でも nullptr ではなく `m_AssetPath` を返す)。
    return m_AssetPath;
}

/** 内部 CEditorCamera への参照を返す。 */
acs::game::editor_core::CEditorCamera& AModelViewerPanel::Camera() noexcept {
    return m_Camera;
}

/** sun light direction を設定する (正規化は呼び出し側任せ)。 */
void AModelViewerPanel::SetLightDirection(acs::FVec3 dir) noexcept {
    // 正規化は呼び出し側 (renderer) 任せ。panel は raw 値で持つ。
    m_LightDir = dir;
}

/** sun light direction を返す。 */
acs::FVec3 AModelViewerPanel::LightDirection() const noexcept {
    return m_LightDir;
}

/** sun light color を設定する。 */
void AModelViewerPanel::SetLightColor(acs::FVec3 color) noexcept {
    m_LightColor = color;
}

/** sun light color を返す。 */
acs::FVec3 AModelViewerPanel::LightColor() const noexcept {
    return m_LightColor;
}

/** IBL の有効/無効を設定する。 */
void AModelViewerPanel::SetIblEnabled(bool b) noexcept {
    m_IblEnabled = b;
}

/** IBL が有効かを返す。 */
bool AModelViewerPanel::IsIblEnabled() const noexcept {
    return m_IblEnabled;
}

/** tonemap mode を設定する (範囲外は無視して既存値を維持)。 */
void AModelViewerPanel::SetToneMappingMode(u32 mode) noexcept {
    // 範囲外は黙って無視 (既存値維持)。理由は header の規約節参照。
    if (mode >= kToneMappingModeCount) return;
    m_TonemapMode = mode;
}

/** 現在の tonemap mode を返す。 */
u32 AModelViewerPanel::ToneMappingMode() const noexcept {
    return m_TonemapMode;
}

/** viewport background color を設定する。 */
void AModelViewerPanel::SetBackgroundColor(acs::FVec4 color) noexcept {
    m_BgColor = color;
}

/** viewport background color を返す。 */
acs::FVec4 AModelViewerPanel::BackgroundColor() const noexcept {
    return m_BgColor;
}

/** grid を描くかを返す。 */
bool AModelViewerPanel::ShowGrid() const noexcept {
    return m_ShowGrid;
}

/** grid を描くかを設定する。 */
void AModelViewerPanel::SetShowGrid(bool b) noexcept {
    m_ShowGrid = b;
}

/** bone skeleton を描くかを返す。 */
bool AModelViewerPanel::ShowBoneSkeleton() const noexcept {
    return m_ShowBoneSkeleton;
}

/** bone skeleton を描くかを設定する。 */
void AModelViewerPanel::SetShowBoneSkeleton(bool b) noexcept {
    m_ShowBoneSkeleton = b;
}

/**
 * Workspace 登録時に基底実装を呼んでから CEditorCamera を 3D mode で再初期化する。
 *
 * @details Init() が呼ばれていなくても Workspace 登録だけでパネルが動くようにする保険。
 */
void AModelViewerPanel::OnInit(acs::game::editor_core::CEditorWorkspace& workspace) noexcept {
    AEditorPanel::OnInit(workspace);
    // CEditorCamera を 3D mode で初期化 (= 既に Init() を呼んでいた場合でも
    // 再初期化される。state は Init/Reset で安定するため副作用は無い)。
    m_Camera.Init(acs::game::editor_core::EEditorCameraMode::Mode3D);
}

/**
 * CAssetBrowser からのファイル選択通知。Mesh 拡張子なら UTF-16 化して LoadModelAsset。
 *
 * @details nullptr / 空文字は「選択解除」とみなし、既存モデルを消さない (ClearModel しない)。
 */
void AModelViewerPanel::OnAssetSelected(const char* asset_path) noexcept {
    if (asset_path == nullptr || asset_path[0] == '\0') {
        // nullptr / 空は「選択解除」(AEditorPanel 規約)。ClearModel しないことで
        // 「他 asset 選択時にビューポート上のモデルが消える」副作用を避ける。
        // モデルを明示的に消したい場合は呼び出し側で ClearModel() を呼ぶ。
        return;
    }
    if (!IsMeshExtensionAscii(asset_path)) {
        // texture / font / particle / scene 等は他 panel の関心事。本 panel は
        // Mesh 拡張子のみ反応する (= 既存モデルを誤って消さない)。
        return;
    }
    // UTF-8 → UTF-16 変換して内部バッファに保存。Utf8ToWide が失敗
    // (= dst[0]=L'\0') した場合は LoadModelAsset(L"") = ClearModel と等価。
    wchar_t wide[kMaxAssetPathChars] = {};
    Utf8ToWide(asset_path, wide, static_cast<int>(kMaxAssetPathChars));
    LoadModelAsset(wide);
}

/**
 * ImGui::Begin("Model Viewport") から始まる 1 window レイアウトを描画する。
 *
 * @details
 * 上部に asset path 表示 + Clear ボタン、中央に viewport プレースホルダ (実 3D は
 * 外部 renderer の責務でダミーテキストのみ)、下部に control bar (Lighting: Light Dir /
 * Light Color / IBL / Tonemap、Background / Display: Background color / Show Grid /
 * Show Bone Skeleton / Reset Camera / Frame Model) を出す。viewport 領域は Drag & Drop
 * target も兼ね、ASSET_PATH payload を accept すると Mesh 拡張子のみ LoadModelAsset する。
 */
void AModelViewerPanel::DrawUI() noexcept {
    if (!IsVisible()) {
        // SetVisible(false) で隠せる (close ボタン or プログラム的 hide)。
        return;
    }

    if (!ImGui::Begin(Title(), &m_Visible)) {
        // 折りたたまれている / minimize されている。
        ImGui::End();
        return;
    }

    // 上部: asset path 表示 + Clear ボタン
    {
        // m_AssetPath (wchar_t) は ImGui に直接渡せないため、UTF-8 に変換して
        // 表示する。CAssetBrowser.cpp の WideToUtf8 と同形のロジックを 1 度だけ
        // 行う (毎フレーム呼ばれるが path 長は短く実用上問題ない)。
        char path_utf8[1024] = {};
        if (m_AssetPath[0] != L'\0') {
            const int n = ::WideCharToMultiByte(
                CP_UTF8, 0,
                m_AssetPath, -1,
                path_utf8, static_cast<int>(sizeof(path_utf8)),
                nullptr, nullptr);
            if (n <= 0) path_utf8[0] = '\0';
            path_utf8[sizeof(path_utf8) - 1] = '\0';
        }
        if (path_utf8[0] == '\0') {
            ImGui::TextDisabled("Asset: (none)");
        } else {
            ImGui::Text("Asset: %s", path_utf8);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            ClearModel();
        }
    }

    ImGui::Separator();

    // viewport プレースホルダ
    // 実 3D 描画は外部 renderer の責務。
    // 本 panel は ImGui::Child で領域だけ確保し、内部に dummy テキストを置く。
    // Drag & Drop target もここに張る。
    {
        // viewport は最低 320x200、上限なしの可変。control bar の高さを差し引く。
        // 残り高さの 60% を viewport に割く (= 40% は下の control area)。
        const float content_h = ImGui::GetContentRegionAvail().y;
        const float viewport_h = (content_h > 100.0f) ? (content_h * 0.60f) : 200.0f;
        ImGui::BeginChild("##modelview_viewport",
                          ImVec2(0, viewport_h),
                          true,
                          ImGuiWindowFlags_NoScrollbar);
        {
            // dummy テキスト (描画 placeholder)。
            const acs::FVec3 eye = m_Camera.ComputeEye();
            const acs::FVec3 tgt = m_Camera.State().target;
            ImGui::TextDisabled("[ 3D Viewport ]  eye=(%.1f %.1f %.1f)  target=(%.1f %.1f %.1f)",
                                eye.x, eye.y, eye.z, tgt.x, tgt.y, tgt.z);
            if (!HasModel()) {
                ImGui::TextDisabled("(No model loaded. Drop an .mdl/.fbx/.gltf/.glb/.obj here)");
            }

            // Drag & Drop target: CAssetBrowser からのドロップを受け取る。
            // payload identifier は CAssetBrowser::kDragDropPayloadId と一致
            // ("ASSET_PATH") する。ImGui の仕様上、target の領域上で発火する。
            if (ImGui::BeginDragDropTarget()) {
                const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH");
                if (payload != nullptr && payload->Data != nullptr) {
                    // payload data は wchar_t* 1 個 (= CAssetBrowser 仕様)。
                    const wchar_t* dropped = *static_cast<const wchar_t* const*>(payload->Data);
                    if (dropped != nullptr) {
                        // Mesh 拡張子のみ accept する (UTF-8 でも UTF-16 でも
                        // 拡張子は ASCII 範囲なので、簡易判定として末尾比較)。
                        const usize len = WLenLocal(dropped);
                        // wchar_t 末尾と ASCII 拡張子の比較ヘルパ。
                        auto WEndsWith = [](const wchar_t* p, usize p_len,
                                            const char* e) noexcept -> bool {
                            const usize e_len = std::strlen(e);
                            if (e_len > p_len) return false;
                            const wchar_t* tail = p + (p_len - e_len);
                            for (usize i = 0; i < e_len; ++i) {
                                wchar_t pc = tail[i];
                                if (pc >= L'A' && pc <= L'Z') pc = static_cast<wchar_t>(pc - L'A' + L'a');
                                char    ec = e[i];
                                if (ec >= 'A' && ec <= 'Z') ec = static_cast<char>(ec - 'A' + 'a');
                                if (pc != static_cast<wchar_t>(ec)) return false;
                            }
                            return true;
                        };
                        const bool is_mesh =
                            WEndsWith(dropped, len, ".mdl")  ||
                            WEndsWith(dropped, len, ".fbx")  ||
                            WEndsWith(dropped, len, ".gltf") ||
                            WEndsWith(dropped, len, ".glb")  ||
                            WEndsWith(dropped, len, ".obj");
                        if (is_mesh) {
                            LoadModelAsset(dropped);
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }
        ImGui::EndChild();
    }

    // 下部: control bar
    {
        ImGui::TextUnformatted("Lighting");
        ImGui::Separator();

        // Light direction (DragFloat3)
        f32 ld[3] = { m_LightDir.x, m_LightDir.y, m_LightDir.z };
        if (ImGui::DragFloat3("Light Dir", ld, 0.01f, -1.0f, 1.0f, "%.3f")) {
            m_LightDir = acs::FVec3{ ld[0], ld[1], ld[2] };
        }

        // Light color (ColorEdit3)
        f32 lc[3] = { m_LightColor.x, m_LightColor.y, m_LightColor.z };
        if (ImGui::ColorEdit3("Light Color", lc)) {
            m_LightColor = acs::FVec3{ lc[0], lc[1], lc[2] };
        }

        // IBL toggle + Tonemap combo (同じ行に並べる)
        if (ImGui::Checkbox("IBL", &m_IblEnabled)) {
            // 値は Checkbox が直接書き換える。
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("Tonemap", TonemapLabel(m_TonemapMode))) {
            for (u32 m = 0; m < kToneMappingModeCount; ++m) {
                const bool selected = (m == m_TonemapMode);
                if (ImGui::Selectable(TonemapLabel(m), selected)) {
                    m_TonemapMode = m;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();
        ImGui::TextUnformatted("Background / Display");
        ImGui::Separator();

        // Background color (ColorEdit4, RGBA)
        f32 bg[4] = { m_BgColor.x, m_BgColor.y, m_BgColor.z, m_BgColor.w };
        if (ImGui::ColorEdit4("Background", bg,
                              ImGuiColorEditFlags_AlphaBar |
                              ImGuiColorEditFlags_AlphaPreview)) {
            m_BgColor = acs::FVec4{ bg[0], bg[1], bg[2], bg[3] };
        }

        // Show grid / Show bone skeleton toggle
        ImGui::Checkbox("Show Grid", &m_ShowGrid);
        ImGui::SameLine();
        ImGui::Checkbox("Show Bone Skeleton", &m_ShowBoneSkeleton);

        // Reset Camera ボタン (小さな utility)
        ImGui::Spacing();
        if (ImGui::Button("Reset Camera")) {
            m_Camera.Reset();
        }
        ImGui::SameLine();
        if (ImGui::Button("Frame Model")) {
            // モデル bounds は本 panel では知らないため、デフォルト bounds
            // (origin / radius=1) で frame する (renderer 側で bounds が
            // 計算できるようになったら実際の bounds で `FrameToBoundingSphere`
            // を呼ぶ)。
            m_Camera.FrameToBoundingSphere(acs::FVec3{0, 0, 0}, 1.0f);
        }
    }

    ImGui::End();
}

} // namespace acs::game::modelview

// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Editor ABI — C# (WPF) エディタが P/Invoke するための C ABI ブリッジ DLL
// -----------------------------------------------------------------------------
// WPF エディタは XAML で「枠」(メニュー/Hierarchy/Inspector/Console) を描き、
// ビューポートだけネイティブ HWND としてホストする。本 DLL はその HWND にエンジンの
// DX12 スワップチェインを作り、エディタ・シーン (ノード階層 + 2D transform) を
// FSpriteBatch で描画し、シーンの列挙 / 選択 / transform 編集を C ABI で公開する。
//
//   create / attach(hwnd) / render(dt) / resize / destroy / version
//   node_count / node_id_at / node_parent / node_name / node_get_transform /
//   node_set_transform / select / selected
//
// シーンモデルは当面 ABI 内の軽量表現 (将来 FNode2D/FScene2D に差し替え可能)。
// =============================================================================
#include "render/Renderer.h"
#include "render/RhiTypes.h"
#include "render/SpriteBatch.h"
#include "render/Fxaa.h"
#include "gameframework/ProjectSettings.h"
#include "math/Vec.h"
#include "foundation/Log.h"
#include "memory/MemorySystem.h"
#include "threading/ThreadPool.h"
#include "gameframework/Reflect.h"          // FTypeRegistry / FTypeDesc / ETypeCategory
#include "gameframework/ReflectCatalog.h"   // AcsRegisterEngineTypes (エンジン型カタログ)
#include "gameframework/Node2D.h"           // 実シーングラフ FNode2D / FTransform2D
#include "gameframework/RigidWorld2D.h"     // FRigidWorld2D (Play モードの物理プレビュー)
#include "gameframework/PrimitiveRenderer2D.h"  // FPrimitiveRenderer2D::DrawShape (形状描画)
#include "gameframework/Material2D.h"       // FMaterial2D (マテリアルアセット = 効果プリセット)
#include "gameframework/Light2DComponent.h" // ComputeLightDirCone (ライト角度→方向/円錐)
#include "gameframework/ComponentFactory.h" // CreateComponentByName (反射名→実コンポーネント)
#include "gameframework/ReflectApply.h"     // ApplyFieldValue (authored 値を実体へ適用)
#include "memory/UniquePtr.h"               // TUniquePtr / MakeUnique
#include "container/Array.h"                // TArray
#include "render/IRhiTexture.h"             // IRhiTexture (スプライト)
#include "render/RenderAssets.h"            // UploadTexture (FImageAsset → GPU)
#include "asset/ImageAsset.h"               // FImageAsset / ImageAssetLoader (stb_image)
#include "asset/AssetId.h"                  // kInvalidAssetId
#include "platform/FileSystem.h"            // FileSystem::ReadAllBytes

#include <cstdint>
#include <cstdio>   // std::snprintf / std::sscanf
#include <cstring>  // std::strncmp (シーン読込のヘッダ判定)
#include <cmath>    // sinf / cosf
#include <new>      // std::nothrow

#define ACS_EDITOR_API extern "C" __declspec(dllexport)

using namespace acs;

// UTF-8 パス → wide 変換のためだけに Win32 を 1 関数だけ前方宣言する (windows.h を引かない)。
extern "C" __declspec(dllimport) int __stdcall MultiByteToWideChar(
    unsigned codepage, unsigned long flags, const char* mb, int mb_len, wchar_t* wc, int wc_len);

// ゲーム DLL の動的ロード (ユーザー定義型の取り込み) 用に Win32 を前方宣言。
extern "C" __declspec(dllimport) void* __stdcall LoadLibraryA(const char* name);
extern "C" __declspec(dllimport) int   __stdcall FreeLibrary(void* module);
extern "C" __declspec(dllimport) void* __stdcall GetProcAddress(void* module, const char* proc);

namespace {

constexpr unsigned kCpUtf8 = 65001u;

/**
 * エディタ・シーンの 1 ノード。エンジンの実 FNode2D を継承し、階層・transform・
 * コンポーネントはエンジン側 (FNode2D) が担う。エディタ表示用メタ (id / 名前 / 色 /
 * ベースサイズ) だけを上乗せする。
 */
struct FEditorNode : public game::FNode2D {
    static constexpr u32 kMaxComponents = 8;
    static constexpr u32 kMaxProps      = 8;   // 1 コンポーネントあたりの編集プロパティ上限

    int   editor_id = 0;                                   // エディタが振る安定 int id
    char  name[64]  = {};                                  // 表示名 (UTF-8)
    f32   base      = 48.0f;                               // 表示矩形のベースサイズ (px)
    FVec4 color     = FVec4{ 0.5f, 0.6f, 0.8f, 1.0f };     // 表示色

    // アタッチされた Component 型 (リフレクション type-id)。エディタ・メタデータとして
    // 保持し (実 FComponent2D は生成しない)、シリアライズ + 将来の play/load で実体化する。
    game::FTypeId components[kMaxComponents] = {};
    u32           component_count            = 0;

    // 各コンポーネントの編集プロパティ値 (リフレクションのスキーマ順、最大 4 成分/プロパティ)。
    // Component は private メンバで offsetof 反射できないため、reflection が「何のフィールドが
    // あるか (名前・種別・既定値)」を与え、ここがその「値」をエディタ側で保持する。
    f32 comp_props[kMaxComponents][kMaxProps][4] = {};

    // スプライト画像。path は UTF-8 (シリアライズ用)、tex は GPU テクスチャ (この場で所有)。
    // 矩形の代わりにこの画像を描く。path が空なら従来どおり色付き矩形。
    char                  sprite_path[256] = {};
    TUniquePtr<IRhiTexture> sprite_tex;

    // 使用マテリアル (.acsmat)。path が設定されていれば、このノードの描画を効果プリセットで
    // 包む (SetEffect→draw→ClearEffect)。material は path から解析した結果のキャッシュ。
    char                  material_path[256] = {};
    game::FMaterial2D     material{};            // material_path を解析した効果プリセット
    bool                  material_loaded = false;
    TUniquePtr<IRhiTexture> mat_albedo_tex;      // PBR マテリアルのアルベド画像 (遅延ロード、任意)
    TUniquePtr<IRhiTexture> mat_normal_tex;      // PBR マテリアルの法線マップ (遅延ロード)
    bool                  mat_tex_loaded = false; // mat 法線/アルベドのロード試行済みか

    // カスタムポリゴン形状 (FPrimitiveRenderer2D shape=Polygon のとき)。ノード原点基準のローカル頂点。
    // poly_verts = コライダー (凸包に間引いた ≤kMaxPolyVerts 頂点、物理に渡す)。
    u32   poly_count = 0;
    FVec2 poly_verts[game::kMaxPolyVerts]{};
    // render_verts = 描画用の滑らかな閉曲線 (ベジエ/Catmull-Rom サンプル)。コライダーより頂点が多い。
    static constexpr u32 kMaxRenderVerts = 64;
    u32   render_count = 0;
    FVec2 render_verts[kMaxRenderVerts]{};
};

/** ゲーム DLL から取り込んだユーザー定義型のスキーマ (ディープコピー。DLL アンロード後も有効)。
 *  自前バッファに名前/フィールドを持ち、安定アドレスの FTypeDesc をエディタの FTypeRegistry へ
 *  登録する → AttachComponent / インスペクタ / シリアライズが engine 型と同じ経路で動く。
 *  ※ TArray 再確保でアドレスが動かないよう、必ず New で個別確保し TArray<FUserType*> で持つ。 */
struct FUserType {
    static constexpr u32 kMaxFields = FEditorNode::kMaxProps;   // ≤8 プロパティ
    char  name[64] = {};
    char  field_names[kMaxFields][48] = {};
    game::FReflectField fields[kMaxFields] = {};
    game::FTypeDesc     desc = {};

    /** 自前バッファへ desc のポインタを張り直す (fill 後・登録前に必ず呼ぶ)。 */
    void Rebuild(int category, u32 field_count) noexcept {
        if (field_count > kMaxFields) field_count = kMaxFields;
        for (u32 i = 0; i < field_count; ++i) fields[i].name = field_names[i];
        desc.name          = name;
        desc.id            = game::AcsTypeHash(name);
        desc.size          = 0;
        desc.align         = 0;
        desc.category      = static_cast<game::ETypeCategory>(category);
        desc.traits        = game::TRAIT_SERIALIZABLE | game::TRAIT_EDITOR_VISIBLE;
        desc.base          = 0;
        desc.fields        = fields;
        desc.field_count   = field_count;
        desc.enum_values   = nullptr;
        desc.enum_count    = 0;
        desc.construct     = nullptr;   // エディタは実体生成しない (DLL アンロード後の dangling 回避)
        desc.destruct      = nullptr;
        desc.category_name = nullptr;
    }
};

/** インプロセス Play 用に、ゲーム DLL のシーン C ABI を解決した関数ポインタ群。 */
struct FGameShim {
    void* (*create)() = nullptr;
    int   (*add_node)(void*, int) = nullptr;
    void  (*set_transform)(void*, int, float, float, float, float, float) = nullptr;
    int   (*add_component)(void*, int, const char*) = nullptr;
    int   (*set_prop)(void*, int, int, const char*, float, float, float, float) = nullptr;
    void  (*tick)(void*, float) = nullptr;
    void  (*get_transform)(void*, int, float*, float*, float*, float*, float*) = nullptr;
    void  (*destroy)(void*) = nullptr;
    // OnDraw 描画 (任意): 古い DLL には無いので Valid() には含めない。あれば Play 中に呼ぶ。
    void  (*draw)(void*, void*, float, float, float, unsigned, unsigned) = nullptr;
    // 入力フィード (任意): editor のキー入力を DLL の acs::Input へ流す + フレーム更新。
    void  (*input_key)(int, int) = nullptr;
    void  (*input_update)()      = nullptr;
    void  (*input_mouse_button)(int, int)  = nullptr;
    void  (*input_mouse_move)(float, float) = nullptr;
    // カメラ連携 (任意): Play 開始時に editor view を渡し、毎フレーム game camera を読み戻して追従。
    void  (*set_camera)(void*, float, float, float)   = nullptr;
    void  (*get_camera)(void*, float*, float*, float*) = nullptr;
    // 親付け替え (任意): ノード列が親より先に子の順でも正しい親子構造を再現する (子の追従)。
    void  (*set_parent)(void*, int, int) = nullptr;
    bool Valid() const noexcept {
        return create && add_node && set_transform && add_component && set_prop && tick && get_transform && destroy;
    }
};

/** 1 つのビューポート + エディタ・シーン (実 FNode2D ツリー) を保持する描画ホスト。 */
struct EditorHost {
    FRenderer    renderer;
    FSpriteBatch sprites;
    bool         attached      = false;
    bool         sprites_ready = false;
    u32          width         = 0;
    u32          height        = 0;
    f32          time          = 0.0f;

    // AA: シーンをオフスクリーン RT に描き、MSAA resolve (既定 8x) または FXAA で backbuffer へ出す。
    FFxaa                       fxaa;                  // FXAA パス (MSAA 無効/失敗時のフォールバック)
    bool                        fxaa_ready = false;
    TUniquePtr<IRhiTexture>     scene_rt;              // シーン用オフスクリーン RT (swapchain と同サイズ)
    u32                         scene_rt_w = 0, scene_rt_h = 0;
    u32                         msaa_samples = 8;      // 現在の MSAA サンプル数 (1=FXAA のみ)
    u32                         msaa_pending = 8;      // 次フレーム適用 (acs_editor_set_msaa)

    // マテリアルプレビュー用の非 MSAA スプライトバッチ (preview_rt は sample_count=1 のため
    // MSAA PSO の本体バッチでは描けない)。EnsurePreviewRt で遅延初期化。
    FSpriteBatch                preview_sprites;
    bool                        preview_sprites_ready = false;

    // プロジェクト設定 (UE の Project Settings 相当)。acs_editor_settings_load で
    // <project>/Config/ProjectSettings.ini を読み、変更のたび保存 + エンジンへ適用する。
    game::FProjectSettings      settings;
    char                        settings_path[512] = {};
    FVec3                       ambient      = FVec3{ 0.10f, 0.11f, 0.13f };   // Rendering.AmbientColor
    f32                         light_height = 90.0f;                          // Rendering.LightHeight
    ClearColor                  clear_color  { 0.07f, 0.08f, 0.10f, 1.0f };    // Rendering.ClearColor

    // マテリアル GPU プレビュー: 実シェーダでサンプルを RT に描き readback する。
    TUniquePtr<IRhiCommandList> preview_cl;            // 専用コマンドリスト (フレーム外で submit)
    TUniquePtr<IRhiTexture>     preview_rt;            // オフスクリーン RT (ColorFormat)
    u32                         preview_rt_size = 0;
    TUniquePtr<IRhiTexture>     preview_sphere_albedo; // 白い円 (alpha 付き、PBR 球の形)
    TUniquePtr<IRhiTexture>     preview_sphere_normal; // 球の法線マップ
    TUniquePtr<IRhiTexture>     preview_scene;         // 効果プレビュー用のミニ風景
    bool                        preview_samples_ready = false;

    // 実シーングラフ: 隠しルート FNode2D がツリー全体を所有する。
    TUniquePtr<game::FNode2D> root;
    // id→ノードの平坦レジストリ (非所有。所有はツリー側)。
    TArray<FEditorNode*>      nodes;
    int          next_id       = 1;
    int          selected      = -1;      // primary (active) ノード。常に selection の一員、空なら -1。
    // 複数選択の集合。selected はこの中の「最後に触れた = primary」。invariant:
    //   selection が空 ⇔ selected == -1。selected >= 0 のとき selected ∈ selection。
    TArray<int>  selection;
    // シーンのシリアライズ文字列バッファ (serialize が書き込み、C# 側が読む)。
    char         scene_text[64 * 1024] = {};

    // 2D ビューカメラ: screen = world * zoom + pan (軸ごと)、size *= zoom。
    // pan=(0,0) zoom=1 で screen==world (= カメラ無しの従来描画と一致)。
    f32          cam_pan_x = 0.0f;
    f32          cam_pan_y = 0.0f;
    f32          cam_zoom  = 1.0f;

    // Undo/Redo: シーンのシリアライズ文字列スナップショットを積む (所有 raw char*)。
    TArray<char*> undo;
    TArray<char*> redo;
    // 連続編集 (ドラッグスクラブ等) の間 PushUndo を抑止し、開始時の 1 スナップショットに束ねる。
    bool suppress_undo = false;

    // ポリゴン描画ツール: クリックで world 点を集め、確定でノード化する。
    bool         poly_drawing = false;
    TArray<FVec2> poly_points;

    // Play モード (物理プレビュー): 0=stopped, 1=playing, 2=paused。
    int                            play_state    = 0;
    char*                          play_snapshot = nullptr;   // play 開始時のシーン (停止で復元)
    TUniquePtr<game::FRigidWorld2D> play_world;
    TArray<u32>                    play_body;                 // world ボディ index (play_node と parallel)
    TArray<int>                    play_node;                 // 対応する編集ノード id (parallel)

    // ユーザー定義型 (ゲーム DLL から取り込んだスキーマ)。Build/HotReload で再読込される。
    // ヒープ実体のアドレスは安定 (TArray 再確保は TUniquePtr を move するだけ) → FTypeRegistry が
    // 保持する desc ポインタが無効化されない。
    TArray<TUniquePtr<FUserType>> user_types;

    // シーン実体化 (authored 値で実コンポーネントを attach し tick する) が有効か。
    bool instances_live = false;

    // ゲームビュー (Game View タブ): true で editor chrome (グリッド/軸/リンク/選択/ギズモ) を描かず
    // ノードの見た目 + Play の OnDraw だけを描く (= スタンドアロンに近い「ゲーム画面」)。
    bool game_view = false;

    // インプロセス Play (ゲーム DLL が実シーンを所有・tick、editor は transform を読み戻して描画)。
    void*     logic_dll   = nullptr;       // LoadLibrary ハンドル (Play 中は載せたまま)
    void*     logic_scene = nullptr;       // DLL 側 Play シーンハンドル
    bool      logic_play  = false;
    FGameShim logic_shim;
    TArray<game::FTransform2D> logic_saved;  // Play 開始時の各ノード transform (停止で復元)
    // Play 開始時の editor カメラ (停止で復元)。Play 中は game camera を読み戻して追従する。
    f32  logic_cam_pan_x = 0.0f;
    f32  logic_cam_pan_y = 0.0f;
    f32  logic_cam_zoom  = 1.0f;
    // Play 開始時に game へ渡したカメラ (center/zoom)。get_camera がこれと «変わったら» 追従を開始する
    // (= ユーザーロジックがカメラを動かしたときだけ。動かさなければ Play 中も手動 pan/zoom を許す)。
    f32  logic_game_cam0_x   = 0.0f;
    f32  logic_game_cam0_y   = 0.0f;
    f32  logic_game_cam0_zoom = 1.0f;
    bool logic_cam_following  = false;   // game がカメラを動かして追従モードに入ったか

    // トランスフォームギズモの状態。
    int  gizmo_mode     = 0;       // 0=move, 1=rotate, 2=scale
    bool gizmo_active   = false;
    bool gizmo_pushed   = false;   // この drag で undo を積んだか (最初の実移動まで遅延)
    int  gizmo_axis     = 0;       // 1=X, 2=Y, 3=free/uniform
    f32  gizmo_off_wx   = 0.0f;    // move: grab 時の (cursor_world - node_world)
    f32  gizmo_off_wy   = 0.0f;
    f32  gizmo_begin_wx = 0.0f;    // move: grab 時の primary ノード world 位置 (軸拘束用)
    f32  gizmo_begin_wy = 0.0f;
    // move 複数選択: grab 時に各選択ノードの開始 world 位置を退避し、primary の delta を全員へ適用する。
    TArray<int> gizmo_move_ids;    // この drag で動かす「選択ルート」ノード id (親が選択外のもの)
    TArray<f32> gizmo_move_bx;     // 各 id の開始 world x (parallel)
    TArray<f32> gizmo_move_by;     // 各 id の開始 world y (parallel)
    f32  gizmo_start_rot   = 0.0f; // rotate: 開始時の local rotation
    f32  gizmo_start_angle = 0.0f; // rotate: 開始時のカーソル角 (screen)
    f32  gizmo_start_sx    = 1.0f; // scale: 開始時の local scale
    f32  gizmo_start_sy    = 1.0f;
    f32  gizmo_start_metric = 1.0f;// scale: 開始時の基準量 (offset / dist)

    // ギズモのスナップ (グリッド吸着) 設定。
    bool snap_enabled = false;
    f32  snap_move    = 10.0f;       // 移動グリッド (world 単位)
    f32  snap_rotate  = 0.2617994f;  // 回転刻み (15° = π/12 rad)
    f32  snap_scale   = 0.25f;       // スケール刻み

    // ラバーバンド (矩形) 選択のオーバーレイ。空 drag 中だけ screen 座標で描く。
    bool marquee_active = false;
    f32  marquee_x0 = 0.0f, marquee_y0 = 0.0f, marquee_x1 = 0.0f, marquee_y1 = 0.0f;
};

/** editor_id からノードを引く (無ければ nullptr)。 */
FEditorNode* FindNode(EditorHost& h, int id) noexcept {
    for (u32 i = 0; i < h.nodes.Size(); ++i) if (h.nodes[i]->editor_id == id) return h.nodes[i];
    return nullptr;
}

// ----- 選択集合の操作 (single/multi 選択。primary = selected) ---------------------

/** id が選択集合に含まれるか。 */
bool SelContains(const EditorHost& h, int id) noexcept {
    for (u32 i = 0; i < h.selection.Size(); ++i) if (h.selection[i] == id) return true;
    return false;
}

/** 選択を空にする。 */
void SelClear(EditorHost& h) noexcept {
    h.selection.Clear();
    h.selected = -1;
}

/** 単一選択にする (集合を {id} に置換、primary=id)。id 不正/未知なら選択解除。 */
void SelSet(EditorHost& h, int id) noexcept {
    h.selection.Clear();
    if (id >= 0 && FindNode(h, id) != nullptr) { h.selection.PushBack(id); h.selected = id; }
    else h.selected = -1;
}

/** id の選択を反転する (Ctrl+click)。追加なら primary になり、primary を外したら別の一員へ移す。 */
void SelToggle(EditorHost& h, int id) noexcept {
    if (id < 0 || FindNode(h, id) == nullptr) return;
    for (u32 i = 0; i < h.selection.Size(); ++i) {
        if (h.selection[i] == id) {                       // 既に選択 → 外す
            h.selection.RemoveAtSwap(i);
            if (h.selected == id)
                h.selected = (h.selection.Size() > 0) ? h.selection[h.selection.Size() - 1] : -1;
            return;
        }
    }
    h.selection.PushBack(id);                              // 未選択 → 追加して primary に
    h.selected = id;
}

/** 構造変更後、選択集合から消えた id を取り除き primary を整える。 */
void SelPrune(EditorHost& h) noexcept {
    for (u32 i = 0; i < h.selection.Size();) {
        if (FindNode(h, h.selection[i]) == nullptr) h.selection.RemoveAtSwap(i);
        else ++i;
    }
    if (h.selected >= 0 && !SelContains(h, h.selected))
        h.selected = (h.selection.Size() > 0) ? h.selection[h.selection.Size() - 1] : -1;
    if (h.selection.Size() == 0) h.selected = -1;
}

/** id がリストに含まれるか (TArray<int> 版の線形探索)。 */
bool IdInList(const TArray<int>& ids, int id) noexcept {
    for (u32 i = 0; i < ids.Size(); ++i) if (ids[i] == id) return true;
    return false;
}

/**
 * n の祖先 (親～ルート手前) のいずれかが ids に含まれるか。
 *
 * @details 複数選択での「選択ルート」判定に使う。祖先が一括操作 (move/duplicate) の対象なら
 * n はそれに従属するので、n 自身を独立に処理すると二重移動 / 二重複製になる。それを防ぐ。
 */
bool AnyAncestorInList(EditorHost& h, const FEditorNode* n, const TArray<int>& ids) noexcept {
    game::FNode2D* p = n->Parent();
    while (p != nullptr && p != h.root.Get()) {
        if (IdInList(ids, static_cast<const FEditorNode*>(p)->editor_id)) return true;
        p = p->Parent();
    }
    return false;
}

/** ノードの「エディタ上の親 id」を返す (隠しルート直下 / 親なしは -1)。 */
int ParentIdOf(const EditorHost& h, const FEditorNode* n) noexcept {
    game::FNode2D* p = n->Parent();
    if (p == nullptr || p == h.root.Get()) return -1;
    return static_cast<const FEditorNode*>(p)->editor_id;
}

/** ノードの sprite_path (UTF-8) を読み込んで GPU テクスチャを (再)生成する。
 *  device 未準備 (attach 前) や path 空なら tex を空にして戻る (描画時に再試行される)。 */
void LoadNodeSprite(EditorHost& h, FEditorNode* n) noexcept {
    if (n == nullptr) return;
    n->sprite_tex.Reset();                                  // 既存を解放 (path 変更/クリア時)
    if (n->sprite_path[0] == '\0') return;
    IRhiDevice* dev = h.renderer.Device();
    if (dev == nullptr) return;                             // attach 前 → DrawScene で再試行
    wchar_t wpath[512];
    if (MultiByteToWideChar(kCpUtf8, 0, n->sprite_path, -1, wpath, 512) <= 0) return;
    auto bytes = FileSystem::ReadAllBytes(wpath);
    if (bytes.IsErr()) return;
    ImageAssetLoader loader;
    auto decoded = loader.LoadFromBytes(kInvalidAssetId, bytes.Value());
    if (decoded.IsErr()) return;
    auto asset = decoded.Value();                           // TSharedPtr を保持 (即解放を防ぐ)
    const FImageAsset* img = static_cast<const FImageAsset*>(asset.Get());
    if (img == nullptr) return;
    auto tex = UploadTexture(*dev, *img);
    if (tex.IsErr()) return;
    n->sprite_tex = Move(tex.Value());
}

/** ノードの material_path (.acsmat) を解析して material キャッシュへ読み込む。
 *  path 空ならマテリアル無し (効果なし) に戻す。マテリアルエディタで再保存されたら
 *  acs_editor_node_reload_material で material_loaded を落として再解析させる。 */
void LoadNodeMaterial(FEditorNode* n) noexcept {
    if (n == nullptr) return;
    n->material = game::FMaterial2D{};                      // 既定 (None) にリセット
    if (n->material_path[0] != '\0')
        game::LoadAcsmatFile(n->material_path, n->material);
    n->material_loaded = true;
    n->mat_tex_loaded  = false;   // マテリアル変更 → テクスチャを再ロード対象に
    n->mat_normal_tex.Reset();
    n->mat_albedo_tex.Reset();
}

/** UTF-8 パスから画像を GPU テクスチャ化して out へ入れる (失敗時は out を空に保つ)。 */
static void LoadTexFromPath(EditorHost& h, const char* utf8_path, TUniquePtr<IRhiTexture>& out) noexcept {
    out.Reset();
    if (utf8_path == nullptr || utf8_path[0] == '\0') return;
    IRhiDevice* dev = h.renderer.Device();
    if (dev == nullptr) return;
    wchar_t wpath[512];
    if (MultiByteToWideChar(kCpUtf8, 0, utf8_path, -1, wpath, 512) <= 0) return;
    auto bytes = FileSystem::ReadAllBytes(wpath);
    if (bytes.IsErr()) return;
    ImageAssetLoader loader;
    auto decoded = loader.LoadFromBytes(kInvalidAssetId, bytes.Value());
    if (decoded.IsErr()) return;
    auto asset = decoded.Value();
    const FImageAsset* img = static_cast<const FImageAsset*>(asset.Get());
    if (img == nullptr) return;
    auto tex = UploadTexture(*dev, *img);
    if (tex.IsErr()) return;
    out = Move(tex.Value());
}

/** PBR マテリアルの法線マップ等を遅延ロードする (device 準備後・1 回)。 */
void LoadNodeMaterialTextures(EditorHost& h, FEditorNode* n) noexcept {
    if (n == nullptr || n->mat_tex_loaded) return;
    if (n->material.kind == game::EMaterialKind::Lit) {
        LoadTexFromPath(h, n->material.pbr.normalPath, n->mat_normal_tex);
        LoadTexFromPath(h, n->material.pbr.albedoPath, n->mat_albedo_tex);   // 任意のアルベド override
    }
    n->mat_tex_loaded = true;
}

/** ノードに付いた FLight2DComponent の component slot を返す (無ければ -1)。 */
static int LightComponentSlot(const FEditorNode* n) noexcept {
    static game::FTypeId s_id = 0;
    if (s_id == 0) {
        const game::FTypeDesc* d = game::FTypeRegistry::Get().FindByName("FLight2DComponent");
        if (d != nullptr) s_id = d->id;
    }
    if (s_id == 0) return -1;
    for (u32 c = 0; c < n->component_count; ++c)
        if (n->components[c] == s_id) return static_cast<int>(c);
    return -1;
}

/** ノードに付いた FShadowCaster2DComponent の component slot を返す (無ければ -1)。 */
static int ShadowCasterSlot(const FEditorNode* n) noexcept {
    static game::FTypeId s_id = 0;
    if (s_id == 0) {
        const game::FTypeDesc* d = game::FTypeRegistry::Get().FindByName("FShadowCaster2DComponent");
        if (d != nullptr) s_id = d->id;
    }
    if (s_id == 0) return -1;
    for (u32 c = 0; c < n->component_count; ++c)
        if (n->components[c] == s_id) return static_cast<int>(c);
    return -1;
}

/** 実 FNode2D ノードを生成してツリー + レジストリに追加する (id / 全 transform 指定)。 */
FEditorNode* CreateNode(EditorHost& h, int id, int parent_id, const char* nm,
                        f32 x, f32 y, f32 rot, f32 sx, f32 sy, f32 base, FVec4 c) noexcept {
    auto child = MakeUnique<FEditorNode>();
    FEditorNode* p = child.Get();
    p->editor_id = id;
    std::snprintf(p->name, sizeof(p->name), "%s", nm);
    p->Local().position = FVec2{ x, y };
    p->Local().rotation = rot;
    p->Local().scale    = FVec2{ sx, sy };
    p->base  = base;
    p->color = c;

    game::FNode2D* parent = (parent_id >= 0) ? static_cast<game::FNode2D*>(FindNode(h, parent_id)) : nullptr;
    if (parent == nullptr) parent = h.root.Get();
    parent->AddChild(TUniquePtr<game::FNode2D>(child.Release(), child.GetAllocator()));
    h.nodes.PushBack(p);
    if (id >= h.next_id) h.next_id = id + 1;   // 明示 id でも採番カウンタを進める
    return p;
}

/** 自動採番でノードを追加する (scale=1)。 */
FEditorNode* AddEditorNode(EditorHost& h, int parent_id, const char* nm,
                           f32 x, f32 y, f32 rot, f32 base, FVec4 c) noexcept {
    return CreateNode(h, h.next_id, parent_id, nm, x, y, rot, 1.0f, 1.0f, base, c);
}

/** コンポーネント slot のプロパティ値を、その型の反射スキーマ既定値で初期化する。 */
void InitCompProps(FEditorNode* n, u32 slot) noexcept {
    for (u32 p = 0; p < FEditorNode::kMaxProps; ++p)
        for (u32 k = 0; k < 4; ++k) n->comp_props[slot][p][k] = 0.0f;
    const game::FTypeDesc* d = game::FTypeRegistry::Get().FindById(n->components[slot]);
    if (d == nullptr) return;
    const u32 nf = d->field_count < FEditorNode::kMaxProps ? d->field_count : FEditorNode::kMaxProps;
    for (u32 p = 0; p < nf; ++p)
        for (u32 k = 0; k < 4; ++k) n->comp_props[slot][p][k] = d->fields[p].defaults[k];
}

/**
 * 登録済み Component 型をノードへアタッチする (記述子のみ。重複/容量超過/非 Component は無視)。
 *
 * @return アタッチ済み or 成功で true、未登録 / 非 Component カテゴリで false。
 */
bool AttachComponent(FEditorNode* n, const char* type_name) noexcept {
    if (n == nullptr || type_name == nullptr) return false;
    game::AcsRegisterEngineTypes();
    const game::FTypeDesc* d = game::FTypeRegistry::Get().FindByName(type_name);
    if (d == nullptr || d->category != game::ETypeCategory::Component) return false;
    for (u32 i = 0; i < n->component_count; ++i) if (n->components[i] == d->id) return true; // 重複
    if (n->component_count >= FEditorNode::kMaxComponents) return false;                      // 容量
    const u32 slot = n->component_count;
    n->components[slot] = d->id;
    InitCompProps(n, slot);                 // スキーマ既定値で値を初期化
    n->component_count = slot + 1;
    return true;
}

/** コンポーネント slot のプロパティ prop に 4 成分値を設定する (範囲外は無視)。 */
void SetCompProp(FEditorNode* n, u32 slot, u32 prop, f32 x, f32 y, f32 z, f32 w) noexcept {
    if (n == nullptr || slot >= n->component_count || prop >= FEditorNode::kMaxProps) return;
    n->comp_props[slot][prop][0] = x; n->comp_props[slot][prop][1] = y;
    n->comp_props[slot][prop][2] = z; n->comp_props[slot][prop][3] = w;
}

/** 型のスキーマで有効なプロパティ数 (= field_count を kMaxProps で頭打ち、未登録は 0)。 */
u32 CompPropCount(const game::FTypeDesc* d) noexcept {
    if (d == nullptr) return 0u;
    return d->field_count < FEditorNode::kMaxProps ? d->field_count : FEditorNode::kMaxProps;
}

/** ノードの全コンポーネントの編集プロパティを CPROP 行として buf へ書き出す。新しい cur を返す。 */
/** 頂点配列を 1 行 "<tag> <id> <count> <x0> <y0> ..." で書く (clamped、新 cur を返す)。 */
int EmitVertLine(char* buf, int cur, int cap, const char* tag, int id,
                 const FVec2* verts, u32 count) noexcept {
    if (count < 3 || cur >= cap) return cur;
    int w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur), "%s %d %u", tag, id, count);
    if (w < 0 || w >= cap - cur) { buf[cap - 1] = '\0'; return cap; }
    cur += w;
    for (u32 k = 0; k < count && cur < cap; ++k) {
        w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur), " %.3f %.3f", verts[k].x, verts[k].y);
        if (w < 0 || w >= cap - cur) { buf[cap - 1] = '\0'; return cap; }
        cur += w;
    }
    if (cur < cap) {
        w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur), "\n");
        if (w > 0 && w < cap - cur) cur += w; else { buf[cap - 1] = '\0'; return cap; }
    }
    return cur;
}

/** ノードのカスタムポリゴンを POLY(コライダー) + RPLY(描画用滑らか頂点) の各行で書く。新 cur を返す。
 *  EmitVertLine は count<3 を内部で弾くので、どちらか一方が退化していても残りは保存される。 */
int EmitNodePoly(char* buf, int cur, int cap, const FEditorNode* n) noexcept {
    if (n->poly_count < 3 && n->render_count < 3) return cur;
    cur = EmitVertLine(buf, cur, cap, "POLY", n->editor_id, n->poly_verts, n->poly_count);
    cur = EmitVertLine(buf, cur, cap, "RPLY", n->editor_id, n->render_verts, n->render_count);
    return cur;
}

int EmitCompProps(char* buf, int cur, int cap, const FEditorNode* n) noexcept {
    for (u32 c = 0; c < n->component_count && cur < cap; ++c) {
        const game::FTypeDesc* d = game::FTypeRegistry::Get().FindById(n->components[c]);
        const u32 nf = CompPropCount(d);
        for (u32 p = 0; p < nf && cur < cap; ++p) {
            const f32* v = n->comp_props[c][p];
            // snprintf は「書き込まれたはず」の長さを返すため、そのまま加算すると切り詰め時に
            // cur が cap を超え (OOB ではないが) 以降の行が静かに欠落する。w を検査して cur を
            // cap で頭打ちにし、溢れたら NUL 終端して打ち切る。
            const int w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur),
                "CPROP %d %u %u %.4f %.4f %.4f %.4f\n", n->editor_id, c, p, v[0], v[1], v[2], v[3]);
            if (w < 0 || w >= cap - cur) { buf[cap - 1] = '\0'; return cap; }
            cur += w;
        }
    }
    return cur;
}

/**
 * src を subtree ごとクローンして parent_id 配下に追加する (再帰)。
 *
 * @details transform / 色 / ベースサイズ / コンポーネント記述子をコピーし、子も再帰複製する。
 * トップは名前末尾に " Copy" を付ける。各クローンは新しい editor_id を採番する。
 * @return クローンしたサブツリーの根。
 */
FEditorNode* CloneSubtree(EditorHost& h, FEditorNode* src, int parent_id, bool top) noexcept {
    const game::FTransform2D t = src->Local();
    char nm[64];
    std::snprintf(nm, sizeof(nm), top ? "%s Copy" : "%s", src->name);
    FEditorNode* clone = CreateNode(h, h.next_id, parent_id, nm,
                                    t.position.x, t.position.y, t.rotation, t.scale.x, t.scale.y,
                                    src->base, src->color);
    clone->SetVisible(src->IsVisible());           // 表示フラグも複製
    clone->SetEnabled(src->IsEnabled());
    clone->SetSortLayer(src->SortLayer());
    std::memcpy(clone->sprite_path, src->sprite_path, sizeof(clone->sprite_path));   // tex は描画時に遅延ロード
    std::memcpy(clone->material_path, src->material_path, sizeof(clone->material_path)); // material は描画時に遅延ロード
    clone->poly_count = src->poly_count;                                             // カスタムポリゴン (コライダー)
    for (u32 pv = 0; pv < src->poly_count; ++pv) clone->poly_verts[pv] = src->poly_verts[pv];
    clone->render_count = src->render_count;                                         // 滑らか描画頂点
    for (u32 rv = 0; rv < src->render_count; ++rv) clone->render_verts[rv] = src->render_verts[rv];
    // コンポーネント記述子 + 編集プロパティ値をコピー。
    for (u32 i = 0; i < src->component_count && clone->component_count < FEditorNode::kMaxComponents; ++i) {
        const u32 slot = clone->component_count;
        clone->components[slot] = src->components[i];
        for (u32 p = 0; p < FEditorNode::kMaxProps; ++p)
            for (u32 k = 0; k < 4; ++k) clone->comp_props[slot][p][k] = src->comp_props[i][p][k];
        ++clone->component_count;
    }
    // 子を再帰複製 (src の子配列は不変なので走査安全)。
    for (u32 i = 0; i < src->ChildCount(); ++i)
        CloneSubtree(h, static_cast<FEditorNode*>(src->Child(i)), clone->editor_id, false);
    return clone;
}

/** シーンを空に戻す (隠しルートだけの状態)。 */
void ClearScene(EditorHost& h) noexcept {
    h.nodes.Clear();                           // 先にレジストリを空に (dangling 回避)
    h.root     = MakeUnique<game::FNode2D>();  // 旧ツリーは再代入で解放
    h.next_id  = 1;
    SelClear(h);
}

/** node 配下を DFS で平坦レジストリへ積む (親が子より先 = save 順を保つ)。 */
void CollectNodes(EditorHost& h, game::FNode2D* node) noexcept {
    for (u32 i = 0; i < node->ChildCount(); ++i) {
        game::FNode2D* c = node->Child(i);
        h.nodes.PushBack(static_cast<FEditorNode*>(c));
        CollectNodes(h, c);
    }
}

/** 構造変更 (削除 / 付け替え) の後にツリーから平坦レジストリを作り直す。 */
void RebuildRegistry(EditorHost& h) noexcept {
    h.nodes.Clear();
    if (h.root.Get() != nullptr) CollectNodes(h, h.root.Get());
    // 構造変更で消えた選択を集合から取り除き、primary を整える。
    SelPrune(h);
}

/** デモ用のエディタ・シーンを構築する (実 FNode2D の親子ツリー)。 */
void InitDemoScene(EditorHost& h) noexcept {
    h.root    = MakeUnique<game::FNode2D>();   // 隠しルート (identity transform)
    h.next_id = 1;
    FEditorNode* player   = AddEditorNode(h, -1, "Player",   320.0f, 230.0f, 0.0f, 56.0f, FVec4{ 0.18f, 0.62f, 0.80f, 1.0f });
    FEditorNode* sprite   = AddEditorNode(h,  1, "Sprite",    52.0f,   0.0f, 0.0f, 30.0f, FVec4{ 0.86f, 0.56f, 0.30f, 1.0f });
    FEditorNode* collider = AddEditorNode(h,  1, "Collider", -38.0f,  24.0f, 0.0f, 26.0f, FVec4{ 0.45f, 0.80f, 0.45f, 1.0f });
    FEditorNode* enemy    = AddEditorNode(h, -1, "Enemy",    520.0f, 150.0f, 0.5f, 48.0f, FVec4{ 0.86f, 0.36f, 0.42f, 1.0f });
    FEditorNode* pickup   = AddEditorNode(h, -1, "Pickup",   180.0f, 330.0f, 0.0f, 24.0f, FVec4{ 0.82f, 0.76f, 0.32f, 1.0f });
    // 描画はコンポーネント駆動。各ノードに FPrimitiveRenderer2D を付ける (Pickup は円)。
    AttachComponent(player,   "FPrimitiveRenderer2D");
    AttachComponent(sprite,   "FPrimitiveRenderer2D");
    AttachComponent(collider, "FPrimitiveRenderer2D");
    AttachComponent(enemy,    "FPrimitiveRenderer2D");
    AttachComponent(pickup,   "FPrimitiveRenderer2D");
    SetCompProp(pickup, 0, 0, 1.0f, 0.0f, 0.0f, 0.0f);   // Pickup の renderer.shape = 1 (Circle)
    SelSet(h, 1);
}

/**
 * シーンを行ベースのテキストへシリアライズする (host 内バッファへ。返り値はそのポインタ)。
 *
 * @details
 * フォーマット (1 ノード 1 行、parent は editor_id、root 直下は -1):
 *   ACSCENE v1
 *   <count>
 *   <id> <parent> <x> <y> <rot> <sx> <sy> <base> <r> <g> <b> <a> <name...>
 * ノード行は «ツリーを DFS» して親が子より先に並ぶよう出力する (host.nodes の平坦順は
 * reparent 後に親子が前後し得るため、それに依存しない)。読込はこの順前提で親を先に解決する。
 */
int EmitNodeLine(char* buf, int cur, int cap, const EditorHost& h, const FEditorNode* n) noexcept {
    if (cur >= cap) return cur;
    const game::FTransform2D& t = n->Local();
    const int pid = ParentIdOf(h, n);
    cur += std::snprintf(buf + cur, static_cast<size_t>(cap - cur),
        "%d %d %.4f %.4f %.4f %.4f %.4f %.2f %.3f %.3f %.3f %.3f %s\n",
        n->editor_id, pid, t.position.x, t.position.y, t.rotation, t.scale.x, t.scale.y,
        n->base, n->color.x, n->color.y, n->color.z, n->color.w, n->name);
    return cur;
}

// ツリーを DFS して «親→子» の順にノード行を出力する (reparent 後でも読込が親を先に解決できる)。
int EmitNodeTreeDFS(char* buf, int cur, int cap, const EditorHost& h, const game::FNode2D* node) noexcept {
    for (u32 i = 0; i < node->ChildCount() && cur < cap; ++i) {
        const game::FNode2D* c = node->Child(i);
        if (c == nullptr) continue;
        cur = EmitNodeLine(buf, cur, cap, h, static_cast<const FEditorNode*>(c));
        cur = EmitNodeTreeDFS(buf, cur, cap, h, c);
    }
    return cur;
}

const char* SerializeScene(EditorHost& h) noexcept {
    char* buf = h.scene_text;
    const int cap = static_cast<int>(sizeof(h.scene_text));
    int cur = std::snprintf(buf, static_cast<size_t>(cap), "ACSCENE v1\n%u\n",
                            static_cast<u32>(h.nodes.Size()));
    cur = EmitNodeTreeDFS(buf, cur, cap, h, h.root.Get());
    // コンポーネント記述子 (ノードごと COMP <editor_id> <type_name>)。
    for (u32 i = 0; i < h.nodes.Size() && cur < cap; ++i) {
        const FEditorNode* n = h.nodes[i];
        for (u32 cmp = 0; cmp < n->component_count && cur < cap; ++cmp) {
            const game::FTypeDesc* d = game::FTypeRegistry::Get().FindById(n->components[cmp]);
            if (d != nullptr && d->name != nullptr)
                cur += std::snprintf(buf + cur, static_cast<size_t>(cap - cur),
                                     "COMP %d %s\n", n->editor_id, d->name);
        }
    }
    // コンポーネントの編集プロパティ (ノードごと CPROP <id> <slot> <prop> <x y z w>)。
    for (u32 i = 0; i < h.nodes.Size() && cur < cap; ++i)
        cur = EmitCompProps(buf, cur, cap, h.nodes[i]);
    // ノードフラグ (非既定のみ): NFLG <id> <visible> <enabled> <sortLayer>。
    // 既定 (visible=1,enabled=1,layer=0) は省略 → 後方互換 (旧ファイルは全既定扱い)。
    for (u32 i = 0; i < h.nodes.Size() && cur < cap; ++i) {
        const FEditorNode* n = h.nodes[i];
        if (!n->IsVisible() || !n->IsEnabled() || n->SortLayer() != 0) {
            const int w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur),
                "NFLG %d %d %d %d\n", n->editor_id,
                n->IsVisible() ? 1 : 0, n->IsEnabled() ? 1 : 0, n->SortLayer());
            if (w < 0 || w >= cap - cur) { buf[cap - 1] = '\0'; return buf; }
            cur += w;
        }
    }
    // スプライト画像パス (設定済みのみ): SPRT <id> <utf8_path> (path は行末まで)。
    for (u32 i = 0; i < h.nodes.Size() && cur < cap; ++i) {
        const FEditorNode* n = h.nodes[i];
        if (n->sprite_path[0] != '\0') {
            const int w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur),
                                        "SPRT %d %s\n", n->editor_id, n->sprite_path);
            if (w < 0 || w >= cap - cur) { buf[cap - 1] = '\0'; return buf; }
            cur += w;
        }
    }
    // 使用マテリアル (.acsmat パス): MAT <id> <utf8_path> (path は行末まで)。
    for (u32 i = 0; i < h.nodes.Size() && cur < cap; ++i) {
        const FEditorNode* n = h.nodes[i];
        if (n->material_path[0] != '\0') {
            const int w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur),
                                        "MAT %d %s\n", n->editor_id, n->material_path);
            if (w < 0 || w >= cap - cur) { buf[cap - 1] = '\0'; return buf; }
            cur += w;
        }
    }
    // カスタムポリゴン: POLY <id> <count> <x0> <y0> ...
    for (u32 i = 0; i < h.nodes.Size() && cur < cap; ++i)
        cur = EmitNodePoly(buf, cur, cap, h.nodes[i]);
    // 選択集合 (undo/redo/open で選択を保つ): SEL <primary> <count> <id...>
    if (cur < cap) {
        int w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur),
                              "SEL %d %u", h.selected, static_cast<u32>(h.selection.Size()));
        if (w > 0 && w < cap - cur) {
            cur += w;
            bool ok = true;
            for (u32 i = 0; i < h.selection.Size() && ok; ++i) {
                w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur), " %d", h.selection[i]);
                if (w > 0 && w < cap - cur) cur += w; else ok = false;
            }
            if (ok) { w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur), "\n");
                      if (w > 0 && w < cap - cur) cur += w; }
        }
    }
    if (cur >= cap) buf[cap - 1] = '\0';   // 溢れた場合は末尾を NUL 終端
    return buf;
}

/** SerializeScene のテキストからシーンを復元する (成功 1 / 失敗 0)。 */
int LoadSceneText(EditorHost& h, const char* text) noexcept {
    if (text == nullptr) return 0;

    const char* p = text;
    char line[2048];   // RPLY (滑らか頂点 最大64) が 1 行に収まる長さ
    // 1 行読み出すローカルヘルパ ('\n' を消費、末尾 NUL 終端、続きがあれば true)。
    auto read_line = [&](char* out, int outsz) -> bool {
        if (*p == '\0') return false;
        int k = 0;
        while (*p != '\0' && *p != '\n') { if (k < outsz - 1) out[k++] = *p; ++p; }
        out[k] = '\0';
        if (*p == '\n') ++p;
        return true;
    };

    // ヘッダ + count を検証してから既存シーンを破棄する (不正入力で現在のシーンを失わない)。
    if (!read_line(line, sizeof(line)) || std::strncmp(line, "ACSCENE", 7) != 0) return 0;
    if (!read_line(line, sizeof(line))) return 0;
    int count = 0;
    std::sscanf(line, "%d", &count);

    ClearScene(h);

    int created = 0;
    // (id, parent) を退避して «順序非依存» に親付けする。子が親より先に並ぶ (旧/手書きファイル)
    // 場合でも、CreateNode 時に親が未生成なら root 直下になるので、全行読了後に付け替えで直す。
    TArray<int> load_id, load_parent;
    for (int i = 0; i < count; ++i) {
        if (!read_line(line, sizeof(line))) break;
        int id = 0, parent = -1, consumed = 0;
        float x = 0, y = 0, rot = 0, sx = 1, sy = 1, base = 48, r = 0.5f, g = 0.6f, b = 0.8f, a = 1.0f;
        const int got = std::sscanf(line, "%d %d %f %f %f %f %f %f %f %f %f %f %n",
            &id, &parent, &x, &y, &rot, &sx, &sy, &base, &r, &g, &b, &a, &consumed);
        if (got < 12) continue;
        const char* nm = line + consumed;
        while (*nm == ' ') ++nm;   // 名前先頭の空白を飛ばす
        CreateNode(h, id, parent, nm, x, y, rot, sx, sy, base, FVec4{ r, g, b, a });
        load_id.PushBack(id); load_parent.PushBack(parent);
        ++created;
    }
    // 親付け fixup: 親が後から来て root 直下になった子を正しい親へ付け替える (Reparent は Local 保持)。
    bool reparented = false;
    for (u32 i = 0; i < load_id.Size(); ++i) {
        if (load_parent[i] < 0) continue;
        FEditorNode* node = FindNode(h, load_id[i]);
        FEditorNode* par  = FindNode(h, load_parent[i]);
        if (node != nullptr && par != nullptr && node->Parent() != static_cast<game::FNode2D*>(par)) {
            node->Reparent(*par); reparented = true;
        }
    }
    if (reparented) h.root->ResolveStructuralChanges();
    // 残り行から COMP / CPROP / SEL を取り込む (COMP→CPROP→SEL の順に並ぶ)。
    bool        sel_present = false;
    int         sel_primary = -1;
    TArray<int> sel_ids;
    while (read_line(line, sizeof(line))) {
        if (std::strncmp(line, "CPROP ", 6) == 0) {       // CPROP <id> <slot> <prop> <x y z w>
            int nid = 0; unsigned slot = 0, prop = 0;
            float a = 0, b = 0, c = 0, e = 0;
            if (std::sscanf(line, "CPROP %d %u %u %f %f %f %f", &nid, &slot, &prop, &a, &b, &c, &e) >= 3)
                SetCompProp(FindNode(h, nid), slot, prop, a, b, c, e);
            continue;
        }
        if (std::strncmp(line, "SEL ", 4) == 0) {         // SEL <primary> <count> <id...>
            int prim = 0, cnt = 0, consumed = 0;
            if (std::sscanf(line, "SEL %d %d %n", &prim, &cnt, &consumed) >= 2) {
                sel_present = true;
                sel_primary = prim;
                const char* q = line + consumed;
                for (int k = 0; k < cnt; ++k) {
                    int sid = 0, c2 = 0;
                    if (std::sscanf(q, "%d%n", &sid, &c2) >= 1) { sel_ids.PushBack(sid); q += c2; }
                    else break;
                }
            }
            continue;
        }
        if (std::strncmp(line, "NFLG ", 5) == 0) {        // NFLG <id> <visible> <enabled> <sortLayer>
            int nid = 0, vis = 1, ena = 1, layer = 0;
            if (std::sscanf(line, "NFLG %d %d %d %d", &nid, &vis, &ena, &layer) >= 2) {
                FEditorNode* n = FindNode(h, nid);
                if (n != nullptr) { n->SetVisible(vis != 0); n->SetEnabled(ena != 0); n->SetSortLayer(layer); }
            }
            continue;
        }
        if (std::strncmp(line, "SPRT ", 5) == 0) {        // SPRT <id> <utf8_path>
            int nid = 0, consumed = 0;
            if (std::sscanf(line, "SPRT %d %n", &nid, &consumed) >= 1) {
                const char* path = line + consumed;
                while (*path == ' ') ++path;
                FEditorNode* n = FindNode(h, nid);
                if (n != nullptr) {
                    std::snprintf(n->sprite_path, sizeof(n->sprite_path), "%s", path);
                    LoadNodeSprite(h, n);                 // device 未準備なら DrawScene で再試行
                }
            }
            continue;
        }
        if (std::strncmp(line, "MAT ", 4) == 0) {         // MAT <id> <utf8_path>
            int nid = 0, consumed = 0;
            if (std::sscanf(line, "MAT %d %n", &nid, &consumed) >= 1) {
                const char* path = line + consumed;
                while (*path == ' ') ++path;
                FEditorNode* n = FindNode(h, nid);
                if (n != nullptr) {
                    std::snprintf(n->material_path, sizeof(n->material_path), "%s", path);
                    LoadNodeMaterial(n);
                }
            }
            continue;
        }
        if (std::strncmp(line, "POLY ", 5) == 0) {        // POLY <id> <count> <x0> <y0> ...
            int nid = 0, pc = 0, consumed = 0;
            if (std::sscanf(line, "POLY %d %d %n", &nid, &pc, &consumed) >= 2) {
                FEditorNode* n = FindNode(h, nid);
                if (n != nullptr && pc >= 3) {
                    if (pc > static_cast<int>(game::kMaxPolyVerts)) pc = static_cast<int>(game::kMaxPolyVerts);
                    const char* q = line + consumed;
                    n->poly_count = 0;
                    for (int k = 0; k < pc; ++k) {
                        float vx = 0, vy = 0; int c2 = 0;
                        if (std::sscanf(q, "%f %f%n", &vx, &vy, &c2) >= 2) { n->poly_verts[n->poly_count++] = FVec2{ vx, vy }; q += c2; }
                        else break;
                    }
                }
            }
            continue;
        }
        if (std::strncmp(line, "RPLY ", 5) == 0) {        // RPLY <id> <count> <x0> <y0> ... (描画用滑らか頂点)
            int nid = 0, pc = 0, consumed = 0;
            if (std::sscanf(line, "RPLY %d %d %n", &nid, &pc, &consumed) >= 2) {
                FEditorNode* n = FindNode(h, nid);
                if (n != nullptr && pc >= 3) {
                    if (pc > static_cast<int>(FEditorNode::kMaxRenderVerts)) pc = static_cast<int>(FEditorNode::kMaxRenderVerts);
                    const char* q = line + consumed;
                    n->render_count = 0;
                    for (int k = 0; k < pc; ++k) {
                        float vx = 0, vy = 0; int c2 = 0;
                        if (std::sscanf(q, "%f %f%n", &vx, &vy, &c2) >= 2) { n->render_verts[n->render_count++] = FVec2{ vx, vy }; q += c2; }
                        else break;
                    }
                }
            }
            continue;
        }
        if (std::strncmp(line, "COMP ", 5) != 0) continue;
        int nid = 0, consumed = 0;
        if (std::sscanf(line, "COMP %d %n", &nid, &consumed) >= 1) {
            const char* tn = line + consumed;
            while (*tn == ' ') ++tn;
            AttachComponent(FindNode(h, nid), tn);
        }
    }
    // 選択を復元する。SEL があれば集合を再構築 (生存 id のみ)、無ければ先頭ノードを単一選択。
    if (sel_present) {
        h.selection.Clear();
        for (u32 i = 0; i < sel_ids.Size(); ++i)
            if (FindNode(h, sel_ids[i]) != nullptr) h.selection.PushBack(sel_ids[i]);
        h.selected = (FindNode(h, sel_primary) != nullptr && SelContains(h, sel_primary))
                     ? sel_primary
                     : (h.selection.Size() > 0 ? h.selection[h.selection.Size() - 1] : -1);
    } else if (created > 0) {
        SelSet(h, h.nodes[0]->editor_id);
    }
    return 1;
}

// ----- Copy / Paste (サブツリーのシリアライズ + id 再マップ) -----

/** node とその子孫を DFS で out へ集める (親が子より先)。 */
void CollectSubtree(FEditorNode* n, TArray<FEditorNode*>& out) noexcept {
    out.PushBack(n);
    for (u32 i = 0; i < n->ChildCount(); ++i)
        CollectSubtree(static_cast<FEditorNode*>(n->Child(i)), out);
}

/** root の subtree を scene_text へシリアライズして返す (root の親は -1、シーン全体と同じ行形式)。 */
const char* SerializeSubtree(EditorHost& h, FEditorNode* root) noexcept {
    TArray<FEditorNode*> sub;
    CollectSubtree(root, sub);
    char* buf = h.scene_text;
    const int cap = static_cast<int>(sizeof(h.scene_text));
    int cur = std::snprintf(buf, static_cast<size_t>(cap), "ACSCENE v1\n%u\n", static_cast<u32>(sub.Size()));
    for (u32 i = 0; i < sub.Size() && cur < cap; ++i) {
        const FEditorNode* n = sub[i];
        const game::FTransform2D& t = n->Local();
        const int pid = (n == root) ? -1 : ParentIdOf(h, n);
        const int w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur),
            "%d %d %.4f %.4f %.4f %.4f %.4f %.2f %.3f %.3f %.3f %.3f %s\n",
            n->editor_id, pid, t.position.x, t.position.y, t.rotation, t.scale.x, t.scale.y,
            n->base, n->color.x, n->color.y, n->color.z, n->color.w, n->name);
        if (w < 0 || w >= cap - cur) { buf[cap - 1] = '\0'; return buf; }
        cur += w;
    }
    for (u32 i = 0; i < sub.Size() && cur < cap; ++i) {
        const FEditorNode* n = sub[i];
        for (u32 c = 0; c < n->component_count && cur < cap; ++c) {
            const game::FTypeDesc* d = game::FTypeRegistry::Get().FindById(n->components[c]);
            if (d != nullptr && d->name != nullptr) {
                const int w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur), "COMP %d %s\n", n->editor_id, d->name);
                if (w < 0 || w >= cap - cur) { buf[cap - 1] = '\0'; return buf; }
                cur += w;
            }
        }
    }
    for (u32 i = 0; i < sub.Size() && cur < cap; ++i)
        cur = EmitCompProps(buf, cur, cap, sub[i]);
    // ノードフラグ (非既定のみ)。
    for (u32 i = 0; i < sub.Size() && cur < cap; ++i) {
        const FEditorNode* n = sub[i];
        if (!n->IsVisible() || !n->IsEnabled() || n->SortLayer() != 0) {
            const int w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur), "NFLG %d %d %d %d\n",
                n->editor_id, n->IsVisible() ? 1 : 0, n->IsEnabled() ? 1 : 0, n->SortLayer());
            if (w < 0 || w >= cap - cur) { buf[cap - 1] = '\0'; return buf; }
            cur += w;
        }
    }
    // スプライト画像パス。
    for (u32 i = 0; i < sub.Size() && cur < cap; ++i) {
        const FEditorNode* n = sub[i];
        if (n->sprite_path[0] != '\0') {
            const int w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur),
                                        "SPRT %d %s\n", n->editor_id, n->sprite_path);
            if (w < 0 || w >= cap - cur) { buf[cap - 1] = '\0'; return buf; }
            cur += w;
        }
    }
    for (u32 i = 0; i < sub.Size() && cur < cap; ++i) {   // 使用マテリアル
        const FEditorNode* n = sub[i];
        if (n->material_path[0] != '\0') {
            const int w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur),
                                        "MAT %d %s\n", n->editor_id, n->material_path);
            if (w < 0 || w >= cap - cur) { buf[cap - 1] = '\0'; return buf; }
            cur += w;
        }
    }
    for (u32 i = 0; i < sub.Size() && cur < cap; ++i)   // カスタムポリゴン
        cur = EmitNodePoly(buf, cur, cap, sub[i]);
    if (cur >= cap) buf[cap - 1] = '\0';
    return buf;
}

/** subtree テキストを target_parent 配下へ貼り付ける (id を新規採番、内部の親子は再マップ)。新根 id / 失敗 -1。 */
int PasteSubtree(EditorHost& h, const char* text, int target_parent) noexcept {
    if (text == nullptr) return -1;
    const char* p = text;
    char line[2048];   // RPLY (滑らか頂点 最大64) が 1 行に収まる長さ
    auto read_line = [&](char* out, int outsz) -> bool {
        if (*p == '\0') return false;
        int k = 0;
        while (*p != '\0' && *p != '\n') { if (k < outsz - 1) out[k++] = *p; ++p; }
        out[k] = '\0';
        if (*p == '\n') ++p;
        return true;
    };
    if (!read_line(line, sizeof(line)) || std::strncmp(line, "ACSCENE", 7) != 0) return -1;
    if (!read_line(line, sizeof(line))) return -1;
    int count = 0;
    std::sscanf(line, "%d", &count);

    TArray<int> oldIds, newIds;
    auto MapId = [&](int o) -> int {
        for (u32 i = 0; i < oldIds.Size(); ++i) if (oldIds[i] == o) return newIds[i];
        return -1;
    };

    int firstNew = -1;
    for (int i = 0; i < count; ++i) {
        if (!read_line(line, sizeof(line))) break;
        int oid = 0, opar = -1, consumed = 0;
        float x = 0, y = 0, rot = 0, sx = 1, sy = 1, base = 48, r = 0.5f, g = 0.6f, b = 0.8f, a = 1.0f;
        const int got = std::sscanf(line, "%d %d %f %f %f %f %f %f %f %f %f %f %n",
            &oid, &opar, &x, &y, &rot, &sx, &sy, &base, &r, &g, &b, &a, &consumed);
        if (got < 12) continue;
        const char* nm = line + consumed;
        while (*nm == ' ') ++nm;
        int np = (opar < 0) ? target_parent : MapId(opar);
        if (np < 0 && opar >= 0) np = target_parent;          // 親が未マップなら target へ
        const int nid = h.next_id;
        CreateNode(h, nid, np, nm, x, y, rot, sx, sy, base, FVec4{ r, g, b, a });
        oldIds.PushBack(oid);
        newIds.PushBack(nid);
        if (firstNew < 0) firstNew = nid;
    }
    while (read_line(line, sizeof(line))) {                    // COMP / CPROP を old_id→new_id で再マップ
        if (std::strncmp(line, "CPROP ", 6) == 0) {
            int onid = 0; unsigned slot = 0, prop = 0;
            float a = 0, b = 0, c = 0, e = 0;
            if (std::sscanf(line, "CPROP %d %u %u %f %f %f %f", &onid, &slot, &prop, &a, &b, &c, &e) >= 3) {
                const int nn = MapId(onid);
                if (nn >= 0) SetCompProp(FindNode(h, nn), slot, prop, a, b, c, e);
            }
            continue;
        }
        if (std::strncmp(line, "NFLG ", 5) == 0) {            // NFLG <old_id> <vis> <ena> <layer>
            int onid = 0, vis = 1, ena = 1, layer = 0;
            if (std::sscanf(line, "NFLG %d %d %d %d", &onid, &vis, &ena, &layer) >= 2) {
                const int nn = MapId(onid);
                FEditorNode* node = (nn >= 0) ? FindNode(h, nn) : nullptr;
                if (node != nullptr) { node->SetVisible(vis != 0); node->SetEnabled(ena != 0); node->SetSortLayer(layer); }
            }
            continue;
        }
        if (std::strncmp(line, "SPRT ", 5) == 0) {            // SPRT <old_id> <utf8_path>
            int onid = 0, consumed = 0;
            if (std::sscanf(line, "SPRT %d %n", &onid, &consumed) >= 1) {
                const char* path = line + consumed;
                while (*path == ' ') ++path;
                const int nn = MapId(onid);
                FEditorNode* node = (nn >= 0) ? FindNode(h, nn) : nullptr;
                if (node != nullptr) {
                    std::snprintf(node->sprite_path, sizeof(node->sprite_path), "%s", path);
                    LoadNodeSprite(h, node);
                }
            }
            continue;
        }
        if (std::strncmp(line, "MAT ", 4) == 0) {             // MAT <old_id> <utf8_path>
            int onid = 0, consumed = 0;
            if (std::sscanf(line, "MAT %d %n", &onid, &consumed) >= 1) {
                const char* path = line + consumed;
                while (*path == ' ') ++path;
                const int nn = MapId(onid);
                FEditorNode* node = (nn >= 0) ? FindNode(h, nn) : nullptr;
                if (node != nullptr) {
                    std::snprintf(node->material_path, sizeof(node->material_path), "%s", path);
                    LoadNodeMaterial(node);
                }
            }
            continue;
        }
        if (std::strncmp(line, "POLY ", 5) == 0) {            // POLY <old_id> <count> <x0> <y0> ...
            int onid = 0, pc = 0, consumed = 0;
            if (std::sscanf(line, "POLY %d %d %n", &onid, &pc, &consumed) >= 2) {
                const int nn = MapId(onid);
                FEditorNode* node = (nn >= 0) ? FindNode(h, nn) : nullptr;
                if (node != nullptr && pc >= 3) {
                    if (pc > static_cast<int>(game::kMaxPolyVerts)) pc = static_cast<int>(game::kMaxPolyVerts);
                    const char* q = line + consumed;
                    node->poly_count = 0;
                    for (int k = 0; k < pc; ++k) {
                        float vx = 0, vy = 0; int c2 = 0;
                        if (std::sscanf(q, "%f %f%n", &vx, &vy, &c2) >= 2) { node->poly_verts[node->poly_count++] = FVec2{ vx, vy }; q += c2; }
                        else break;
                    }
                }
            }
            continue;
        }
        if (std::strncmp(line, "RPLY ", 5) == 0) {            // RPLY <old_id> <count> <x0> <y0> ...
            int onid = 0, pc = 0, consumed = 0;
            if (std::sscanf(line, "RPLY %d %d %n", &onid, &pc, &consumed) >= 2) {
                const int nn = MapId(onid);
                FEditorNode* node = (nn >= 0) ? FindNode(h, nn) : nullptr;
                if (node != nullptr && pc >= 3) {
                    if (pc > static_cast<int>(FEditorNode::kMaxRenderVerts)) pc = static_cast<int>(FEditorNode::kMaxRenderVerts);
                    const char* q = line + consumed;
                    node->render_count = 0;
                    for (int k = 0; k < pc; ++k) {
                        float vx = 0, vy = 0; int c2 = 0;
                        if (std::sscanf(q, "%f %f%n", &vx, &vy, &c2) >= 2) { node->render_verts[node->render_count++] = FVec2{ vx, vy }; q += c2; }
                        else break;
                    }
                }
            }
            continue;
        }
        if (std::strncmp(line, "COMP ", 5) != 0) continue;
        int onid = 0, consumed = 0;
        if (std::sscanf(line, "COMP %d %n", &onid, &consumed) >= 1) {
            const char* tn = line + consumed;
            while (*tn == ' ') ++tn;
            const int nn = MapId(onid);
            if (nn >= 0) AttachComponent(FindNode(h, nn), tn);
        }
    }
    if (firstNew >= 0) SelSet(h, firstNew);
    return firstNew;
}

// ----- Undo/Redo (シーンのスナップショットスタック) -----

/** 現在のシーンをシリアライズして heap コピーを返す (所有は呼び出し側、失敗 nullptr)。 */
char* DupSnapshot(EditorHost& h) noexcept {
    const char* s = SerializeScene(h);
    const size_t len = std::strlen(s) + 1;
    char* copy = new (std::nothrow) char[len];
    if (copy != nullptr) std::memcpy(copy, s, len);
    return copy;
}

/** スナップショットスタックを空にして各 heap バッファを解放する。 */
void ClearStack(TArray<char*>& st) noexcept {
    for (u32 i = 0; i < st.Size(); ++i) delete[] st[i];
    st.Clear();
}

/** 変更操作の直前に呼ぶ: 現在状態を undo に積み、redo を破棄する。
 *  連続編集中 (suppress_undo) は積まない → ドラッグ 1 回が undo 1 ステップになる。 */
void PushUndo(EditorHost& h) noexcept {
    if (h.suppress_undo) return;
    char* snap = DupSnapshot(h);
    if (snap == nullptr) return;
    h.undo.PushBack(snap);
    ClearStack(h.redo);
    constexpr u32 kMaxUndo = 128;
    while (h.undo.Size() > kMaxUndo) {            // 上限超過は古いものから捨てる
        delete[] h.undo[0];
        for (u32 i = 1; i < h.undo.Size(); ++i) h.undo[i - 1] = h.undo[i];
        h.undo.PopBack();
    }
}

// world↔screen 変換 (DrawScene/PickNode/gizmo で共有)。
f32 W2SX(const EditorHost& h, f32 wx) noexcept { return wx * h.cam_zoom + h.cam_pan_x; }
f32 W2SY(const EditorHost& h, f32 wy) noexcept { return wy * h.cam_zoom + h.cam_pan_y; }
f32 S2WX(const EditorHost& h, f32 sx) noexcept { return (h.cam_zoom != 0.0f) ? (sx - h.cam_pan_x) / h.cam_zoom : 0.0f; }
f32 S2WY(const EditorHost& h, f32 sy) noexcept { return (h.cam_zoom != 0.0f) ? (sy - h.cam_pan_y) / h.cam_zoom : 0.0f; }

/** v を step の最も近い倍数に丸める (step<=0 はそのまま)。 */
f32 SnapTo(f32 v, f32 step) noexcept { return (step > 0.0f) ? std::round(v / step) * step : v; }

constexpr f32 kGizmoLen = 64.0f;   // ハンドルの画面長 (固定サイズ)

/** 選択ノードのギズモ中心 (screen)。選択無しは false。 */
bool GizmoCenter(EditorHost& h, f32& cx, f32& cy) noexcept {
    if (h.selected < 0) return false;
    const FEditorNode* n = FindNode(h, h.selected);
    if (n == nullptr) return false;
    const game::FTransform2D w = n->World();
    cx = W2SX(h, w.position.x);
    cy = W2SY(h, w.position.y);
    return true;
}

constexpr f32 kRotR    = 52.0f;   // 回転リング半径
constexpr f32 kScaleC  = 40.0f;   // uniform スケールハンドルのコーナーオフセット

/**
 * クリックがどのギズモハンドルか。モードで意味が変わる:
 *   move : 1=X軸 / 2=Y軸 / 3=自由(中央)
 *   rotate: 1=リング (角度ドラッグ)
 *   scale: 1=X / 2=Y / 3=uniform(コーナー)
 * 0=ハンドル外。
 */
int GizmoHit(EditorHost& h, f32 sx, f32 sy) noexcept {
    f32 cx, cy;
    if (!GizmoCenter(h, cx, cy)) return 0;

    if (h.gizmo_mode == 1) {   // rotate: リング上か
        const f32 d = std::sqrt((sx - cx) * (sx - cx) + (sy - cy) * (sy - cy));
        return (std::fabs(d - kRotR) <= 8.0f) ? 1 : 0;
    }
    if (h.gizmo_mode == 2) {   // scale: uniform コーナー → X → Y
        if (std::fabs(sx - (cx + kScaleC)) <= 8.0f && std::fabs(sy - (cy - kScaleC)) <= 8.0f) return 3;
        if (sx >= cx + 8.0f && sx <= cx + kGizmoLen + 10.0f && std::fabs(sy - cy) <= 7.0f) return 1;
        if (std::fabs(sx - cx) <= 7.0f && sy <= cy - 8.0f && sy >= cy - kGizmoLen - 10.0f) return 2;
        return 0;
    }
    // move: 中央(自由) → X → Y
    if (std::fabs(sx - cx) <= 9.0f && std::fabs(sy - cy) <= 9.0f) return 3;
    if (sx >= cx + 8.0f && sx <= cx + kGizmoLen + 10.0f && std::fabs(sy - cy) <= 7.0f) return 1;
    if (std::fabs(sx - cx) <= 7.0f && sy <= cy - 8.0f && sy >= cy - kGizmoLen - 10.0f) return 2;
    return 0;
}

/** ノードの world 位置を設定する (親の逆変換で local に落とす)。 */
void SetNodeWorldPosition(EditorHost& h, FEditorNode* n, f32 wx, f32 wy) noexcept {
    game::FNode2D* parent = n->Parent();
    if (parent == nullptr || parent == h.root.Get()) {     // 親が root → local==world
        n->Local().position = FVec2{ wx, wy };
        return;
    }
    const game::FTransform2D pw = parent->World();
    const f32 dx = wx - pw.position.x, dy = wy - pw.position.y;
    const f32 c = std::cos(-pw.rotation), s = std::sin(-pw.rotation);   // 親回転を打ち消す
    const f32 rx = dx * c - dy * s;
    const f32 ry = dx * s + dy * c;
    n->Local().position = FVec2{ (pw.scale.x != 0.0f) ? rx / pw.scale.x : rx,
                                 (pw.scale.y != 0.0f) ? ry / pw.scale.y : ry };
}

/** ノードの FPrimitiveRenderer2D の shape (0=Box,1=Circle,2=Triangle) を返す (無ければ -1)。 */
int PrimitiveShape(const FEditorNode* n) noexcept {
    static game::FTypeId s_prId = 0;
    if (s_prId == 0) {
        const game::FTypeDesc* d = game::FTypeRegistry::Get().FindByName("FPrimitiveRenderer2D");
        if (d != nullptr) s_prId = d->id;
    }
    if (s_prId == 0) return -1;
    for (u32 c = 0; c < n->component_count; ++c)
        if (n->components[c] == s_prId) return static_cast<int>(n->comp_props[c][0][0]);  // prop0 = shape
    return -1;
}

/** レンダラー未付与ノードの薄いギズモ (小さな菱形の枠 + 中心点)。選択はできるが「空」だと分かる。 */
void DrawEmptyGizmo(FSpriteBatch& sb, f32 cx, f32 cy, f32 alpha) noexcept {
    const f32 r = 9.0f;
    const FVec4 col{ 0.55f, 0.62f, 0.72f, 0.5f * alpha };
    const FVec2 p[4] = { {cx, cy - r}, {cx + r, cy}, {cx, cy + r}, {cx - r, cy} };
    for (int i = 0; i < 4; ++i) {
        const FVec2 a = p[i], b = p[(i + 1) & 3];
        const f32 dx = b.x - a.x, dy = b.y - a.y;
        const f32 len = std::sqrt(dx * dx + dy * dy);
        sb.DrawRectRotated((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, len, 1.5f, std::atan2(dy, dx), col);
    }
    sb.DrawRect(cx - 1.5f, cy - 1.5f, 3.0f, 3.0f, FVec4{ 0.72f, 0.80f, 0.90f, 0.7f * alpha });
}

// ===== ポリゴン平滑化 (ベジエ風 Catmull-Rom) + 凸包コライダー生成 =====

/** 閉じた一様 Catmull-Rom スプラインで anchors[m] を滑らかにサンプルし out[≤cap] に書く。戻り値は生成頂点数。
 *  各アンカーを必ず通り (張力 0.5)、辺ごとに seg 分割する。m<3 なら anchors をそのまま返す。 */
u32 SmoothClosedSpline(const FVec2* anchors, u32 m, FVec2* out, u32 cap) noexcept {
    if (cap == 0) return 0;
    if (m < 3) { u32 k = (m < cap) ? m : cap; for (u32 i = 0; i < k; ++i) out[i] = anchors[i]; return k; }
    u32 seg = cap / m;                       // 頂点予算を辺数で割る
    if (seg < 1) seg = 1;
    if (seg > 8) seg = 8;                     // 8 分割を超えても見た目は変わらない
    u32 n = 0;
    for (u32 i = 0; i < m && n < cap; ++i) {
        const FVec2 p0 = anchors[(i + m - 1) % m];
        const FVec2 p1 = anchors[i];
        const FVec2 p2 = anchors[(i + 1) % m];
        const FVec2 p3 = anchors[(i + 2) % m];
        for (u32 s = 0; s < seg && n < cap; ++s) {
            const f32 t  = static_cast<f32>(s) / static_cast<f32>(seg);
            const f32 t2 = t * t, t3 = t2 * t;
            FVec2 q;
            q.x = 0.5f * ((2.0f * p1.x) + (-p0.x + p2.x) * t
                        + (2.0f * p0.x - 5.0f * p1.x + 4.0f * p2.x - p3.x) * t2
                        + (-p0.x + 3.0f * p1.x - 3.0f * p2.x + p3.x) * t3);
            q.y = 0.5f * ((2.0f * p1.y) + (-p0.y + p2.y) * t
                        + (2.0f * p0.y - 5.0f * p1.y + 4.0f * p2.y - p3.y) * t2
                        + (-p0.y + 3.0f * p1.y - 3.0f * p2.y + p3.y) * t3);
            out[n++] = q;
        }
    }
    return n;
}

f32 PolyCross2(FVec2 o, FVec2 a, FVec2 b) noexcept {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

/** pts[n] の凸包を Andrew monotone chain で求め、maxOut を超える分は最も平坦な頂点から間引く。
 *  out[≤maxOut] に CCW 順で書き、頂点数を返す。コライダー (poly_verts) 用。 */
u32 ConvexHullDecimated(const FVec2* pts, u32 n, FVec2* out, u32 maxOut) noexcept {
    constexpr u32 kCap = FEditorNode::kMaxRenderVerts;
    if (n > kCap) n = kCap;
    if (n < 3 || maxOut == 0) { u32 k = (n < maxOut) ? n : maxOut; for (u32 i = 0; i < k; ++i) out[i] = pts[i]; return k; }
    FVec2 p[kCap];
    for (u32 i = 0; i < n; ++i) p[i] = pts[i];
    for (u32 i = 1; i < n; ++i) {            // x,y 昇順に挿入ソート
        FVec2 key = p[i]; int j = static_cast<int>(i) - 1;
        while (j >= 0 && (p[j].x > key.x || (p[j].x == key.x && p[j].y > key.y))) { p[j + 1] = p[j]; --j; }
        p[j + 1] = key;
    }
    FVec2 hull[2 * kCap]; u32 k = 0;
    for (u32 i = 0; i < n; ++i) {            // 下側
        while (k >= 2 && PolyCross2(hull[k - 2], hull[k - 1], p[i]) <= 0.0f) --k;
        hull[k++] = p[i];
    }
    const u32 lower = k + 1;
    for (int i = static_cast<int>(n) - 2; i >= 0; --i) {   // 上側
        while (k >= lower && PolyCross2(hull[k - 2], hull[k - 1], p[i]) <= 0.0f) --k;
        hull[k++] = p[i];
    }
    u32 hc = (k > 0) ? k - 1 : 0;            // 末尾は始点の重複なので除く
    while (hc > maxOut) {                     // 三角形面積が最小 = 最も平坦な頂点を除去
        u32 worst = 0; f32 worstA = 3.4e38f;
        for (u32 i = 0; i < hc; ++i) {
            const f32 ar = std::fabs(PolyCross2(hull[(i + hc - 1) % hc], hull[i], hull[(i + 1) % hc]));
            if (ar < worstA) { worstA = ar; worst = i; }
        }
        for (u32 i = worst; i + 1 < hc; ++i) hull[i] = hull[i + 1];
        --hc;
    }
    for (u32 i = 0; i < hc; ++i) out[i] = hull[i];
    return hc;
}

/** ノードのカスタムポリゴンを world→screen 変換して三角ファンで塗る。
 *  滑らかな render_verts があればそれを、無ければコライダー poly_verts を使う。 */
void DrawNodePolygon(EditorHost& h, FSpriteBatch& sb, const FEditorNode* n,
                     const game::FTransform2D& w, FVec4 col) noexcept {
    const FVec2* verts; u32 vc;
    if (n->render_count >= 3)    { verts = n->render_verts; vc = n->render_count; }
    else if (n->poly_count >= 3) { verts = n->poly_verts;   vc = n->poly_count; }
    else return;
    const f32 c = std::cos(w.rotation), s = std::sin(w.rotation);
    FVec2 sv[FEditorNode::kMaxRenderVerts];
    f32 ccx = 0.0f, ccy = 0.0f;
    for (u32 i = 0; i < vc; ++i) {
        const f32 lx = verts[i].x * w.scale.x, ly = verts[i].y * w.scale.y;
        const f32 wx = w.position.x + (lx * c - ly * s), wy = w.position.y + (lx * s + ly * c);
        sv[i] = FVec2{ W2SX(h, wx), W2SY(h, wy) };
        ccx += sv[i].x; ccy += sv[i].y;
    }
    ccx /= static_cast<f32>(vc); ccy /= static_cast<f32>(vc);
    for (u32 i = 0; i < vc; ++i) {
        const FVec2 a = sv[i], b = sv[(i + 1) % vc];
        sb.DrawTriangle(ccx, ccy, a.x, a.y, b.x, b.y, col);
    }
}

/** シーンを 2D で描画する (カメラ適用: screen = world*zoom + pan)。 */
void DrawScene(EditorHost& h, FSpriteBatch& sb, u32 w, u32 hh) noexcept {
    const f32 fw = static_cast<f32>(w);
    const f32 fh = static_cast<f32>(hh);
    const f32 z  = h.cam_zoom;
    const f32 px = h.cam_pan_x;
    const f32 py = h.cam_pan_y;
    auto SX = [&](f32 wx) noexcept { return wx * z + px; };   // world→screen X
    auto SY = [&](f32 wy) noexcept { return wy * z + py; };   // world→screen Y

    // ゲームビューでは editor chrome (グリッド/軸/リンク/選択/ギズモ) を描かない。
    const bool chrome = !h.game_view;

    // 背景グリッド (カメラに合わせて pan / zoom する world グリッド)。
    const FVec4 grid{ 0.15f, 0.17f, 0.22f, 1.0f };
    const f32 spacing = 40.0f * z;
    if (chrome && spacing >= 4.0f) {             // 詰まりすぎる縮小時は省略
        f32 gx = std::fmod(px, spacing); if (gx < 0.0f) gx += spacing;
        for (f32 x = gx; x <= fw; x += spacing) sb.DrawRect(x, 0.0f, 1.0f, fh, grid);
        f32 gy = std::fmod(py, spacing); if (gy < 0.0f) gy += spacing;
        for (f32 y = gy; y <= fh; y += spacing) sb.DrawRect(0.0f, y, fw, 1.0f, grid);
    }
    // world 原点軸 (x=0 / y=0 が画面内にあるときだけ)。
    const FVec4 axis{ 0.25f, 0.29f, 0.37f, 1.0f };
    const f32 axx = SX(0.0f), axy = SY(0.0f);
    if (chrome && axx >= 0.0f && axx <= fw) sb.DrawRect(axx, 0.0f, 1.0f, fh, axis);
    if (chrome && axy >= 0.0f && axy <= fh) sb.DrawRect(0.0f, axy, fw, 1.0f, axis);

    // 親子のリンク線 (world を screen に変換してから結ぶ)。
    for (u32 i = 0; chrome && i < h.nodes.Size(); ++i) {
        FEditorNode* n = h.nodes[i];
        game::FNode2D* parent = n->Parent();
        if (parent == nullptr || parent == h.root.Get()) continue;
        const game::FTransform2D cw = n->World();
        const game::FTransform2D pw = parent->World();
        const f32 cx = SX(cw.position.x), cy = SY(cw.position.y);
        const f32 ppx = SX(pw.position.x), ppy = SY(pw.position.y);
        const f32 dx = ppx - cx, dy = ppy - cy;
        const f32 len = std::sqrt(dx * dx + dy * dy);
        if (len > 1.0f) {
            const f32 ang = std::atan2(dy, dx);
            sb.DrawRectRotated((cx + ppx) * 0.5f, (cy + ppy) * 0.5f, len, 1.5f, ang,
                               FVec4{ 0.35f, 0.40f, 0.50f, 1.0f });
        }
    }

    // 選択ハイライト (選択集合の各ノード背後に枠)。primary は明るい黄、その他は淡い黄。
    for (u32 i = 0; chrome && i < h.selection.Size(); ++i) {
        const FEditorNode* sn = FindNode(h, h.selection[i]);
        if (sn == nullptr) continue;
        const game::FTransform2D w = sn->World();
        const bool primary = (h.selection[i] == h.selected);
        const FVec4 col = primary ? FVec4{ 1.0f, 0.84f, 0.30f, 1.0f }    // primary: 明るい黄
                                  : FVec4{ 0.70f, 0.62f, 0.28f, 1.0f };  // その他: 淡い黄
        // ポリゴンノードは矩形でなく形状に沿ったハイライト (少し拡大したポリゴンを背後に塗る)。
        if (PrimitiveShape(sn) == 3 && (sn->render_count >= 3 || sn->poly_count >= 3)) {
            const f32 ext = sn->base * 0.5f * (w.scale.x + w.scale.y) * 0.5f * z;   // 画面上の半径目安
            const f32 f   = (ext > 0.5f) ? (1.0f + 12.0f / ext) : 1.2f;            // ~12px ぶん外側へ
            game::FTransform2D hw = w; hw.scale = FVec2{ w.scale.x * f, w.scale.y * f };
            DrawNodePolygon(h, sb, sn, hw, col);
        } else {
            sb.DrawRectRotated(SX(w.position.x), SY(w.position.y),
                               sn->base * w.scale.x * z + 12.0f, sn->base * w.scale.y * z + 12.0f,
                               w.rotation, col);
        }
    }

    // 2D 点光源を収集する (FLight2DComponent を持つノード)。lit (PBR) マテリアルの陰影付け用。
    // 位置/半径は screen px (DrawScene はノードを screen 空間に描くため)。
    FSpriteLight lights[16];
    u32 lightCount = 0;
    for (u32 i = 0; i < h.nodes.Size() && lightCount < 16; ++i) {
        const FEditorNode* n = h.nodes[i];
        const int ls = LightComponentSlot(n);
        if (ls < 0 || !n->IsVisible()) continue;
        const game::FTransform2D lw = n->World();
        FSpriteLight& L = lights[lightCount++];
        L.pos       = FVec2{ SX(lw.position.x), SY(lw.position.y) };
        L.radius    = n->comp_props[ls][0][0] * z;                       // radius (world) → screen px
        L.color     = FVec3{ n->comp_props[ls][1][0], n->comp_props[ls][1][1], n->comp_props[ls][1][2] };
        L.intensity = n->comp_props[ls][2][0];
        L.type      = static_cast<i32>(n->comp_props[ls][3][0]);         // 0=Point,1=Spot,2=Directional
        game::ComputeLightDirCone(n->comp_props[ls][4][0], n->comp_props[ls][5][0],
                                  L.dir, L.coneInner, L.coneOuter);       // angle/cone → dir/cos
        if (L.radius < 1.0f) L.radius = 256.0f * z;                      // 未設定ガード
    }
    // 影オクルーダーを収集する (FShadowCaster2DComponent を持つノードのシルエットを円近似)。
    // occNodeIdx[k] = オクルーダー k の元ノードの配列番号 (自己影スキップ用に描画側で逆引き)。
    FSpriteOccluder occluders[16];
    u32 occCount = 0;
    int occNodeIdx[16];
    bool occSelfShadow[16];          // 自己影あり (m_SelfShadow=1) のオクルーダーはスキップ番号を渡さない
    for (u32 i = 0; i < h.nodes.Size() && occCount < 16; ++i) {
        const FEditorNode* n = h.nodes[i];
        const int cs = ShadowCasterSlot(n);
        if (cs < 0 || !n->IsVisible()) continue;
        const game::FTransform2D ow = n->World();
        occNodeIdx[occCount] = static_cast<int>(i);
        occSelfShadow[occCount] = (n->comp_props[cs][2][0] > 0.5f);   // m_SelfShadow (行2)
        FSpriteOccluder& O = occluders[occCount++];
        const f32 scale = (n->comp_props[cs][0][0] > 0.01f) ? n->comp_props[cs][0][0] : 1.0f;  // radiusScale
        const f32 cx = SX(ow.position.x), cy = SY(ow.position.y);
        const f32 dw = n->base * ow.scale.x * z * scale, dh = n->base * ow.scale.y * z * scale;
        const f32 cs_r = std::cos(ow.rotation), sn_r = std::sin(ow.rotation);
        O.center      = FVec2{ cx, cy };
        O.radius      = n->base * 0.5f * (ow.scale.x + ow.scale.y) * 0.5f * z * scale;   // 外接半径
        O.halfExtents = FVec2{ dw * 0.5f, dh * 0.5f };
        O.rotation    = ow.rotation;
        // 影の形状は «見た目の形» に合わせてプリミティブ種別から導出 (三角形/ポリゴンは多角形)。
        const int prs = PrimitiveShape(n);   // 0=Box,1=Circle,2=Triangle,3=Polygon,-1=無し
        if (prs == 2) {                      // 三角形 → 多角形 (3頂点、DrawShape と同式)
            const f32 hx = dw * 0.5f, hy = dh * 0.5f;
            O.shape = 2; O.polyCount = 3;
            O.polyVerts[0] = FVec2{ cx + hy * sn_r,            cy - hy * cs_r };           // 上
            O.polyVerts[1] = FVec2{ cx - hx * cs_r - hy * sn_r, cy - hx * sn_r + hy * cs_r }; // 左下
            O.polyVerts[2] = FVec2{ cx + hx * cs_r - hy * sn_r, cy + hx * sn_r + hy * cs_r }; // 右下
        } else if (prs == 3 && (n->render_count >= 3 || n->poly_count >= 3)) {   // ポリゴン (凹形対応)
            const FVec2* verts = (n->render_count >= 3) ? n->render_verts : n->poly_verts;
            const u32 srcN = (n->render_count >= 3) ? n->render_count : n->poly_count;
            // 滑らか曲線 (最大64頂点) を 16 に「均等間引き」して凹形の周回を保つ (先頭切り詰めは閉曲線を壊す)。
            const u32 vc = (srcN > acs::kMaxOccPolyVerts) ? acs::kMaxOccPolyVerts : srcN;
            O.shape = 2; O.polyCount = static_cast<i32>(vc);
            for (u32 k = 0; k < vc; ++k) {
                const u32 si = (srcN > acs::kMaxOccPolyVerts) ? (k * srcN) / vc : k;   // 等間隔サンプル
                const f32 lx = verts[si].x * ow.scale.x * scale, ly = verts[si].y * ow.scale.y * scale;
                const f32 wx = ow.position.x + (lx * cs_r - ly * sn_r);
                const f32 wy = ow.position.y + (lx * sn_r + ly * cs_r);
                O.polyVerts[k] = FVec2{ SX(wx), SY(wy) };
            }
        } else if (prs == 0) {               // 箱 → 4頂点ポリゴン (SegPolyDist で正確。回転/細長も漏れない)
            const f32 hx = dw * 0.5f, hy = dh * 0.5f;
            O.shape = 2; O.polyCount = 4;
            O.polyVerts[0] = FVec2{ cx - hx*cs_r + hy*sn_r, cy - hx*sn_r - hy*cs_r };
            O.polyVerts[1] = FVec2{ cx + hx*cs_r + hy*sn_r, cy + hx*sn_r - hy*cs_r };
            O.polyVerts[2] = FVec2{ cx + hx*cs_r - hy*sn_r, cy + hx*sn_r + hy*cs_r };
            O.polyVerts[3] = FVec2{ cx - hx*cs_r - hy*sn_r, cy - hx*sn_r + hy*cs_r };
        } else if (prs == 1) {               // 円
            O.shape = 0;
        } else {                             // レンダラー無し → 旧 m_Shape (0=円,1=箱)
            O.shape = static_cast<i32>(n->comp_props[cs][1][0]);
        }
    }
    sb.SetLights(lights, lightCount, h.ambient, h.light_height * z, occluders, occCount);

    // ノード本体。スプライト画像があればそれを、無ければ色付き矩形を描く。
    // 非可視ノードはゴースト表示 (alpha を落として「隠し」を見せつつ選択可能に保つ)。
    for (u32 i = 0; i < h.nodes.Size(); ++i) {
        FEditorNode* n = h.nodes[i];
        if (h.game_view && !n->IsVisible()) continue;   // ゲームでは非可視ノードは出さない (ゴースト無し)
        const game::FTransform2D w = n->World();
        const f32 cx = SX(w.position.x), cy = SY(w.position.y);
        const f32 dw = n->base * w.scale.x * z, dh = n->base * w.scale.y * z;
        const f32 alpha = n->IsVisible() ? 1.0f : 0.25f;
        if (n->sprite_path[0] != '\0' && n->sprite_tex.Get() == nullptr)
            LoadNodeSprite(h, n);              // 遅延ロード (attach 後・シーン読込後・複製後)
        // マテリアルをこのノードの本体描画に適用する。Scene/Game 両ビュー共通。
        if (!n->material_loaded) LoadNodeMaterial(n);
        const int prShape = PrimitiveShape(n);
        // 影の上下関係 = 描画順 (後に描く = 上)。自分より «下» (先描画) のキャスターの影は受けない。
        // 上の物体の面に下からの影が乗る物理は無く、凹形状の相手と重なった際のまだら影も防ぐ。
        // 地面 (先頭に描く受け手) は全キャスターの影を受け、上→下へはシルエットから連続した影が落ちる。
        u32 skipMask = 0;
        for (u32 k = 0; k < occCount; ++k)
            if (occNodeIdx[k] < static_cast<int>(i)) skipMask |= 1u << k;
        bool fx = false, lit = false;
        if (n->material_path[0] != '\0' && n->material.kind == game::EMaterialKind::Lit) {
            // PBR (Lit) マテリアル: 法線マップ + 集めたライトで BRDF 陰影付け。
            if (!n->mat_tex_loaded) LoadNodeMaterialTextures(h, n);   // 法線マップ遅延ロード
            FLitMaterialParams lm = game::ToLitParams(n->material.pbr);   // PBR/トゥーン共通
            lm.occluderSkipMask = skipMask;                              // 自分より下のキャスターを除外
            for (u32 k = 0; k < occCount; ++k)                           // 自分が occluder なら番号を渡す
                if (occNodeIdx[k] == static_cast<int>(i)) {
                    if (occSelfShadow[k]) lm.selfShadowOccluder = static_cast<i32>(k);   // 自己影: 内部=umbra
                    else                  lm.selfOccluder       = static_cast<i32>(k);   // 既定: 自己影スキップ
                    break;
                }
            sb.SetLitMaterial(lm, n->mat_normal_tex.Get());   // normal 無しは平面
            lit = true;
        } else {
            fx = game::ApplyMaterial(sb, n->material, h.time);   // 効果プリセット (None なら false)
            if (!fx && lightCount > 0 && (n->sprite_tex.Get() != nullptr || prShape >= 0)) {
                // ライトのあるシーンではマテリアル無しノードも既定 Lit で陰影付けする
                // (影の中のノードが無灯火のまま明るく浮くのを防ぐ。Node2D::DrawTree と同じ規律)。
                // エディタ用ギズモ (レンダラー無し) は対象外。
                FLitMaterialParams lm;
                lm.roughness = 0.85f;
                lm.occluderSkipMask = skipMask;                          // 自分より下のキャスターを除外
                for (u32 k = 0; k < occCount; ++k)
                    if (occNodeIdx[k] == static_cast<int>(i)) {
                        if (occSelfShadow[k]) lm.selfShadowOccluder = static_cast<i32>(k);
                        else                  lm.selfOccluder       = static_cast<i32>(k);
                        break;
                    }
                sb.SetLitMaterial(lm, nullptr);
                lit = true;
            }
        }
        // アルベド = マテリアルの albedo override (lit のみ) があればそれ、無ければノードのスプライト。
        IRhiTexture* albedoTex = n->sprite_tex.Get();
        if (lit && n->mat_albedo_tex.Get() != nullptr) albedoTex = n->mat_albedo_tex.Get();
        if (albedoTex != nullptr) {                           // スプライト / アルベド画像
            sb.DrawRotated(*albedoTex, cx, cy, dw, dh, w.rotation,
                           0.0f, 0.0f, 1.0f, 1.0f, FVec4{ 1.0f, 1.0f, 1.0f, alpha });
        } else {
            if (prShape == 3 && (n->poly_count >= 3 || n->render_count >= 3)) {   // ポリゴン (頂点はノードが保持)
                FVec4 col = n->color; col.w *= alpha;
                DrawNodePolygon(h, sb, n, w, col);
            } else if (prShape >= 0) {                        // プリミティブレンダラー → 形状を描く
                FVec4 col = n->color; col.w *= alpha;
                game::FPrimitiveRenderer2D::DrawShape(sb, prShape, cx, cy, dw, dh, w.rotation, col);
            } else if (chrome) {                              // レンダラー無し → 薄いエディタ用ギズモ (ゲームでは出さない)
                DrawEmptyGizmo(sb, cx, cy, alpha);
            }
        }
        if (lit)      sb.ClearLit();
        else if (fx)  sb.ClearEffect();                       // 効果を解除して次ノードへ
    }

    // トランスフォームギズモ (選択ノード)。モードでハンドルが変わる。画面固定サイズ。ゲームビューでは出さない。
    f32 gcx, gcy;
    if (chrome && GizmoCenter(h, gcx, gcy)) {
        const FVec4 cR{ 0.92f, 0.33f, 0.33f, 1.0f };   // X 軸
        const FVec4 cG{ 0.33f, 0.85f, 0.42f, 1.0f };   // Y 軸
        const FVec4 cC{ 0.40f, 0.80f, 0.95f, 1.0f };   // rotate/uniform (シアン)
        const FVec4 cY{ 0.95f, 0.92f, 0.38f, 1.0f };   // move free (黄)
        if (h.gizmo_mode == 1) {                        // rotate: リング (N セグメント)
            const int N = 28;
            const f32 segw = 6.2831853f * kRotR / static_cast<f32>(N) * 1.15f;
            for (int i = 0; i < N; ++i) {
                const f32 a = 6.2831853f * static_cast<f32>(i) / static_cast<f32>(N);
                sb.DrawRectRotated(gcx + kRotR * std::cos(a), gcy + kRotR * std::sin(a),
                                   segw, 2.5f, a + 1.5707963f, cC);
            }
        } else if (h.gizmo_mode == 2) {                 // scale: X/Y バー + コーナー(uniform)
            const f32 L = kGizmoLen;
            sb.DrawRect(gcx, gcy - 1.25f, L, 2.5f, cR);
            sb.DrawRect(gcx + L - 6.0f, gcy - 6.0f, 12.0f, 12.0f, cR);            // X 箱
            sb.DrawRect(gcx - 1.25f, gcy - L, 2.5f, L, cG);
            sb.DrawRect(gcx - 6.0f, gcy - L - 6.0f, 12.0f, 12.0f, cG);            // Y 箱
            sb.DrawRect(gcx + kScaleC - 6.0f, gcy - kScaleC - 6.0f, 12.0f, 12.0f, cC); // uniform コーナー
        } else {                                         // move: X/Y 矢印 + 中央(自由)
            const f32 L = kGizmoLen;
            sb.DrawRect(gcx, gcy - 1.25f, L, 2.5f, cR);
            sb.DrawRect(gcx + L - 6.0f, gcy - 5.0f, 11.0f, 11.0f, cR);
            sb.DrawRect(gcx - 1.25f, gcy - L, 2.5f, L, cG);
            sb.DrawRect(gcx - 5.0f, gcy - L - 5.0f, 11.0f, 11.0f, cG);
            sb.DrawRect(gcx - 5.0f, gcy - 5.0f, 10.0f, 10.0f, cY);
        }
    }

    // ラバーバンド選択の矩形 (screen 座標。塗り + 4 辺の枠)。最前面に描く。
    if (h.marquee_active) {
        const f32 minx = (h.marquee_x0 < h.marquee_x1) ? h.marquee_x0 : h.marquee_x1;
        const f32 maxx = (h.marquee_x0 < h.marquee_x1) ? h.marquee_x1 : h.marquee_x0;
        const f32 miny = (h.marquee_y0 < h.marquee_y1) ? h.marquee_y0 : h.marquee_y1;
        const f32 maxy = (h.marquee_y0 < h.marquee_y1) ? h.marquee_y1 : h.marquee_y0;
        const f32 mw = maxx - minx, mh = maxy - miny;
        const FVec4 fill{ 0.24f, 0.60f, 0.91f, 0.15f };
        const FVec4 edge{ 0.40f, 0.72f, 0.96f, 0.90f };
        const f32 t = 1.5f;
        sb.DrawRect(minx, miny, mw, mh, fill);
        sb.DrawRect(minx, miny, mw, t,  edge);          // top
        sb.DrawRect(minx, maxy - t, mw, t, edge);       // bottom
        sb.DrawRect(minx, miny, t, mh, edge);           // left
        sb.DrawRect(maxx - t, miny, t, mh, edge);       // right
    }

    // ポリゴン描画ツール: 集めた点と辺をオーバーレイ表示 (最後→最初は閉じプレビュー)。
    if (h.poly_drawing && h.poly_points.Size() > 0) {
        const FVec4 line{ 0.42f, 0.88f, 0.52f, 0.95f };
        const FVec4 dot{ 0.96f, 0.92f, 0.45f, 1.0f };
        u32 np = h.poly_points.Size();
        constexpr u32 kRV = FEditorNode::kMaxRenderVerts;
        if (np >= 3) {                                            // 確定後と同じ滑らかな閉曲線をプレビュー
            if (np > kRV) np = kRV;
            FVec2 anchors[kRV];
            for (u32 i = 0; i < np; ++i) anchors[i] = h.poly_points[i];
            FVec2 sm[kRV];
            const u32 sc = SmoothClosedSpline(anchors, np, sm, kRV);
            for (u32 i = 0; i < sc; ++i) {
                const f32 px = SX(sm[i].x), py = SY(sm[i].y);
                const FVec2 nx = sm[(i + 1) % sc];
                const f32 px2 = SX(nx.x), py2 = SY(nx.y);
                const f32 dx = px2 - px, dy = py2 - py;
                const f32 len = std::sqrt(dx * dx + dy * dy);
                if (len > 1.0f)
                    sb.DrawRectRotated((px + px2) * 0.5f, (py + py2) * 0.5f, len, 2.0f, std::atan2(dy, dx), line);
            }
        } else if (np == 2) {                                     // 2 点はまだ直線ガイド
            const f32 px = SX(h.poly_points[0].x), py = SY(h.poly_points[0].y);
            const f32 px2 = SX(h.poly_points[1].x), py2 = SY(h.poly_points[1].y);
            const f32 dx = px2 - px, dy = py2 - py;
            const f32 len = std::sqrt(dx * dx + dy * dy);
            if (len > 1.0f)
                sb.DrawRectRotated((px + px2) * 0.5f, (py + py2) * 0.5f, len, 2.0f, std::atan2(dy, dx), line);
        }
        for (u32 i = 0; i < h.poly_points.Size(); ++i) {          // アンカーマーカー
            const f32 px = SX(h.poly_points[i].x), py = SY(h.poly_points[i].y);
            sb.DrawRect(px - 3.0f, py - 3.0f, 6.0f, 6.0f, dot);
        }
    }
}

/**
 * スクリーン座標 (sx, sy) でノードをピックする (上 = 後で描いたものを優先)。
 *
 * @details
 * screen→world に逆変換し、各ノードの OBB (world 中心 + 回転 + base*scale 半幅) に対して
 * 点を逆回転してローカル枠で内外判定する。zoom は screen↔world で打ち消されるので half-extent
 * は world 単位 (base*scale*0.5) を使う。
 * @return ヒットしたノードの editor_id、無ければ -1。
 */
int PickNode(const EditorHost& h, f32 screen_x, f32 screen_y) noexcept {
    if (h.cam_zoom == 0.0f) return -1;
    const f32 wx = (screen_x - h.cam_pan_x) / h.cam_zoom;
    const f32 wy = (screen_y - h.cam_pan_y) / h.cam_zoom;
    for (int i = static_cast<int>(h.nodes.Size()) - 1; i >= 0; --i) {   // topmost first
        const FEditorNode* n = h.nodes[static_cast<u32>(i)];
        const game::FTransform2D w = n->World();
        const f32 hw = n->base * w.scale.x * 0.5f;
        const f32 hgt = n->base * w.scale.y * 0.5f;
        if (hw <= 0.0f || hgt <= 0.0f) continue;
        const f32 lx = wx - w.position.x;
        const f32 ly = wy - w.position.y;
        const f32 c = std::cos(-w.rotation), s = std::sin(-w.rotation);   // 逆回転
        const f32 rx = lx * c - ly * s;
        const f32 ry = lx * s + ly * c;
        if (rx >= -hw && rx <= hw && ry >= -hgt && ry <= hgt) return n->editor_id;
    }
    return -1;
}

/** ロガー/メモリ/スレッドプールをプロセスで 1 度だけ初期化する。 */
bool g_subsystems_ready = false;
void EnsureSubsystems() noexcept {
    if (g_subsystems_ready) return;
    FLogConfig lc{};
    lc.console      = true;
    lc.debug_output = true;
    FLogger::Init(lc);
    (void)FMemorySystem::Init(FMemorySystem::DefaultConfig());
    (void)FThreadPool::Init(0);
    // エンジン同梱型のリフレクション登録を確定する (この呼び出しで ReflectCatalog の
    // TU がリンクへ引き込まれ、全 ACS_REGISTER* 自動登録子が走る)。
    const u32 type_count = acs::game::AcsRegisterEngineTypes();
    g_subsystems_ready = true;
    ACS_LOG_INFO("[acs_editor_abi] subsystems initialized (%u reflected engine types)", type_count);
}

/** ETypeCategory を人間可読なラベルに変換する (editor のカテゴリ見出し用)。 */
const char* CategoryLabel(acs::game::ETypeCategory cat) noexcept {
    using acs::game::ETypeCategory;
    switch (cat) {
        case ETypeCategory::Struct:    return "Struct";
        case ETypeCategory::Enum:      return "Enum";
        case ETypeCategory::Object:    return "Object";
        case ETypeCategory::Component: return "Component";
        case ETypeCategory::Node:      return "Node";
        case ETypeCategory::Scene:     return "Scene";
        case ETypeCategory::Asset:     return "Asset";
        case ETypeCategory::System:    return "System";
        case ETypeCategory::Prefab:    return "Prefab";
        case ETypeCategory::Interface: return "Interface";
        case ETypeCategory::Event:     return "Event";
        case ETypeCategory::Command:   return "Command";
        case ETypeCategory::Resource:  return "Resource";
        case ETypeCategory::Service:   return "Service";
        case ETypeCategory::Custom:    return "Custom";
    }
    return "Unknown";
}

} // namespace

// =============================================================================
// C ABI — ライフサイクル / 描画
// =============================================================================

ACS_EDITOR_API const char* acs_editor_version(void) {
    return "ACS Editor ABI 0.14 (DX12, reflection, scene I/O, camera+pick, undo, gizmo MRS, copy/paste, component props, multi-select, box-select, align/distribute, node props)";
}

ACS_EDITOR_API void* acs_editor_create(void) {
    EnsureSubsystems();
    auto* host = new (std::nothrow) EditorHost();
    if (host != nullptr) InitDemoScene(*host);
    return host;
}

ACS_EDITOR_API int acs_editor_attach(void* handle, void* hwnd, uint32_t width, uint32_t height) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || hwnd == nullptr || width == 0u || height == 0u) return 0;
    const auto r = host->renderer.InitExternal(hwnd, width, height, /*debug=*/false, /*depth=*/true);
    if (r.IsErr()) {
        ACS_LOG_ERROR("[acs_editor_abi] attach failed: %s", r.Error().message);
        return 0;
    }
    host->attached = true;
    host->width    = width;
    host->height   = height;

    // MSAA (既定 8x) でスプライトバッチを初期化。PSO 生成に失敗する環境では非 MSAA へフォールバック。
    auto sr = host->sprites.Init(*host->renderer.Device(), host->renderer.ColorFormat(), 8192,
                                 host->msaa_samples);
    if (sr.IsErr() && host->msaa_samples > 1) {
        ACS_LOG_WARN("[acs_editor_abi] MSAA %ux sprite batch init failed → 非 MSAA で再試行",
                     host->msaa_samples);
        host->msaa_samples = host->msaa_pending = 1;
        host->sprites.Shutdown();
        sr = host->sprites.Init(*host->renderer.Device(), host->renderer.ColorFormat(), 8192, 1);
    }
    host->sprites_ready = sr.IsOk();
    if (!host->sprites_ready) {
        ACS_LOG_ERROR("[acs_editor_abi] sprite batch init failed: %s", sr.Error().message);
    }

    // プロジェクト設定を既定値で初期化 (C# がプロジェクトを開いた後 settings_load_text で上書き)
    host->settings.ResetToDefaults();

    // FXAA (MSAA 無効時のフォールバック。失敗してもエディタは AA 無しで続行できる)
    const auto fr = host->fxaa.Init(*host->renderer.Device(), host->renderer.ColorFormat());
    host->fxaa_ready = fr.IsOk();
    if (!host->fxaa_ready) {
        ACS_LOG_ERROR("[acs_editor_abi] fxaa init failed: %s", fr.Error().message);
    }

    ACS_LOG_INFO("[acs_editor_abi] attached to HWND %p (%ux%u)", hwnd, width, height);
    return 1;
}

static void EditorStepPlay(EditorHost& h, f32 dt) noexcept;   // 前方宣言 (定義は Play モード節)
static void EditorTickLogic(EditorHost& h, f32 dt) noexcept;  // 前方宣言 (インプロセス Play)

/** swapchain と同サイズのシーン用オフスクリーン RT を用意する (リサイズ/サンプル数変更時は作り直し)。 */
static bool EnsureSceneRt(EditorHost& h, u32 w, u32 hgt) noexcept {
    if (w == 0 || hgt == 0) return false;
    if (h.scene_rt && h.scene_rt_w == w && h.scene_rt_h == hgt) return true;
    IRhiDevice* dev = h.renderer.Device();
    if (dev == nullptr) return false;
    dev->WaitIdle();                        // 旧 RT が前フレームでまだ参照されている可能性 (リサイズ時のみ)
    FTextureDesc td{};
    td.width  = w;
    td.height = hgt;
    td.format = h.renderer.ColorFormat();
    td.is_render_target = true;
    td.sample_count = h.msaa_samples;       // MSAA RT (1 なら通常 RT + FXAA)
    auto r = CreateRhiTexture(*dev, td);
    if (r.IsErr()) { h.scene_rt.Reset(); h.scene_rt_w = h.scene_rt_h = 0; return false; }
    h.scene_rt   = Move(r.Value());
    h.scene_rt_w = w;
    h.scene_rt_h = hgt;
    return true;
}

ACS_EDITOR_API void acs_editor_render(void* handle, float dt) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || !host->attached) return;
    host->time += dt;

    if (host->play_state == 1) EditorStepPlay(*host, dt);   // 再生中は物理を進める
    if (host->logic_play)      EditorTickLogic(*host, dt);  // インプロセス Play: ユーザーロジックを進める

    // MSAA サンプル数の変更を適用する (PSO はサンプル数を焼き込むためバッチごと再生成)。
    if (host->msaa_pending != host->msaa_samples && host->renderer.Device() != nullptr) {
        host->renderer.Device()->WaitIdle();
        host->msaa_samples = host->msaa_pending;
        host->scene_rt.Reset();
        host->scene_rt_w = host->scene_rt_h = 0;
        host->sprites.Shutdown();
        auto sr = host->sprites.Init(*host->renderer.Device(), host->renderer.ColorFormat(), 8192,
                                     host->msaa_samples);
        if (sr.IsErr() && host->msaa_samples > 1) {       // 非対応サンプル数 → 非 MSAA へ
            host->msaa_samples = host->msaa_pending = 1;
            host->sprites.Shutdown();
            sr = host->sprites.Init(*host->renderer.Device(), host->renderer.ColorFormat(), 8192, 1);
        }
        host->sprites_ready = sr.IsOk();
        ACS_LOG_INFO("[acs_editor_abi] MSAA = %ux", host->msaa_samples);
    }

    const ClearColor clear = host->clear_color;   // Rendering.ClearColor (プロジェクト設定)
    host->renderer.BeginFrame(clear);

    if (host->sprites_ready) {
        IRhiCommandList* cl = host->renderer.CommandList();
        IRhiSwapchain*   sc = host->renderer.Swapchain();
        if (cl != nullptr && sc != nullptr) {
            const u32 scW = sc->Width(), scH = sc->Height();
            // AA: シーンをオフスクリーン RT に描き、MSAA resolve または FXAA で backbuffer へ。
            // RT 生成失敗時は従来どおり backbuffer へ直接描く (MSAA 時は PSO 不一致のため不可 →
            // attach/切替時のフォールバックで msaa_samples=1 が保証される)。
            const bool useMsaa = host->msaa_samples > 1;
            const bool aa = (useMsaa || host->fxaa_ready) && EnsureSceneRt(*host, scW, scH);
            if (aa) cl->BeginRenderToTexture(*host->scene_rt, clear, nullptr, 1.0f);
            host->sprites.Begin(*cl, scW, scH);
            DrawScene(*host, host->sprites, scW, scH);
            // インプロセス Play 中はユーザーコンポーネントの OnDraw を world view で重ねて描く。
            // SetView でバッチを editor カメラ (screen=world*zoom+pan) に一致する world ビューへ切替え、
            // DLL 側 DrawTree が world 座標で積む OnDraw が正しい画面位置に来るようにする。
            if (host->logic_play && host->logic_scene != nullptr && host->logic_shim.draw != nullptr) {
                const f32 vz  = (host->cam_zoom > 0.0001f) ? host->cam_zoom : 1.0f;
                const f32 vcx = (static_cast<f32>(scW) * 0.5f - host->cam_pan_x) / vz;
                const f32 vcy = (static_cast<f32>(scH) * 0.5f - host->cam_pan_y) / vz;
                host->sprites.SetView(vcx, vcy, vz);   // DrawScene の screen 描画を flush し world view へ
                host->logic_shim.draw(host->logic_scene, &host->sprites, vcx, vcy, vz, scW, scH);
            }
            host->sprites.End();
            if (aa) {
                if (useMsaa) {
                    // MSAA: ResolveSubresource で backbuffer へ解決 (バリアは内部で処理)。
                    cl->ResolveToSwapchain(*host->scene_rt, *sc, host->renderer.CurrentBuffer());
                } else {
                    cl->EndRenderToTexture(*host->scene_rt);            // RT → SRV
                    cl->BeginRenderToSwapchain(*sc, host->renderer.CurrentBuffer(), clear, nullptr, 1.0f);
                    FViewport vp{};                                     // 再バインド後に全画面ビューポートを戻す
                    vp.width  = static_cast<f32>(scW);
                    vp.height = static_cast<f32>(scH);
                    cl->SetViewport(vp);
                    FScissorRect sr{};
                    sr.right  = static_cast<i32>(scW);
                    sr.bottom = static_cast<i32>(scH);
                    cl->SetScissor(sr);
                    host->fxaa.Apply(*cl, *host->scene_rt);             // FXAA 解決 → backbuffer
                }
            }
        }
    }

    host->renderer.EndFrame();
}

ACS_EDITOR_API void acs_editor_resize(void* handle, uint32_t width, uint32_t height) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || !host->attached || width == 0u || height == 0u) return;
    host->renderer.OnResize(width, height);
    host->width  = width;
    host->height = height;
}

// =============================================================================
// C ABI — プロジェクト設定 (UE の Project Settings 相当)
// =============================================================================

/** プロジェクト設定をエンジン状態へ反映する (ロード/変更時に呼ぶ)。 */
static void ApplySettings(EditorHost& h) noexcept {
    const int msaa = h.settings.GetInt("Rendering", "MsaaSamples", 8);
    h.msaa_pending = (msaa >= 8) ? 8u : (msaa >= 4) ? 4u : (msaa >= 2) ? 2u : 1u;
    h.ambient      = h.settings.GetColor("Rendering", "AmbientColor", FVec3{ 0.10f, 0.11f, 0.13f });
    h.light_height = h.settings.GetFloat("Rendering", "LightHeight", 90.0f);
    const FVec3 cc = h.settings.GetColor("Rendering", "ClearColor", FVec3{ 0.07f, 0.08f, 0.10f });
    h.clear_color  = ClearColor{ cc.x, cc.y, cc.z, 1.0f };
    h.snap_move    = h.settings.GetFloat("Editor", "SnapMove", 10.0f);
    h.snap_rotate  = h.settings.GetFloat("Editor", "SnapRotateDeg", 15.0f) * 3.1415926535f / 180.0f;
    h.snap_scale   = h.settings.GetFloat("Editor", "SnapScale", 0.25f);
}

/** INI テキストからプロジェクト設定を読み込み、エンジンへ適用する (C# がファイル I/O 担当)。
 *  ini_text=null/空 は既定値で初期化 (初回起動)。 */
ACS_EDITOR_API void acs_editor_settings_load_text(void* handle, const char* ini_text) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return;
    if (ini_text != nullptr && ini_text[0] != '\0') host->settings.LoadText(ini_text);
    else                                            host->settings.ResetToDefaults();
    ApplySettings(*host);
}

/** プロジェクト設定を INI テキストへシリアライズする (C# が書き込む)。書いた文字数を返す。 */
ACS_EDITOR_API int acs_editor_settings_serialize(void* handle, char* out, int cap) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || out == nullptr || cap <= 0) return 0;
    return static_cast<int>(host->settings.SerializeText(out, static_cast<usize>(cap)));
}

/** 設定エントリ数。 */
ACS_EDITOR_API int acs_editor_settings_count(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    return (host != nullptr) ? static_cast<int>(host->settings.Count()) : 0;
}

/** index 番目のエントリを TSV 1 行 "category\tkey\tvalue\ttype\toptions\tbuiltin\tdesc" で返す。
 *  type は ESettingType の整数。options/desc は無ければ空。 */
ACS_EDITOR_API int acs_editor_settings_entry(void* handle, int index, char* out, int cap) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || out == nullptr || cap <= 0) return 0;
    if (index < 0 || static_cast<u32>(index) >= host->settings.Count()) return 0;
    const game::FSettingEntry& e = host->settings.At(static_cast<u32>(index));
    const int w = std::snprintf(out, static_cast<size_t>(cap), "%s\t%s\t%s\t%d\t%s\t%d\t%s",
                                e.category, e.key, e.value, static_cast<int>(e.type),
                                e.options != nullptr ? e.options : "",
                                e.builtin ? 1 : 0,
                                e.desc != nullptr ? e.desc : "");
    return (w > 0) ? 1 : 0;
}

/** 既存エントリの値を設定し、エンジンへ即適用する。成功 1。 */
ACS_EDITOR_API int acs_editor_settings_set(void* handle, const char* cat, const char* key,
                                           const char* value) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return 0;
    if (!host->settings.Set(cat, key, value)) return 0;
    ApplySettings(*host);
    return 1;
}

/** ユーザー定義エントリを追加する (既存キーは値更新)。成功 1。 */
ACS_EDITOR_API int acs_editor_settings_add(void* handle, const char* cat, const char* key,
                                           const char* value) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return 0;
    const int ok = host->settings.Add(cat, key, value) ? 1 : 0;
    if (ok) ApplySettings(*host);
    return ok;
}

/** ユーザー定義エントリを削除する (ビルトインは不可)。成功 1。 */
ACS_EDITOR_API int acs_editor_settings_remove(void* handle, const char* cat, const char* key) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return 0;
    return host->settings.Remove(cat, key) ? 1 : 0;
}

/** 値を 1 つ取得する (ユーザーコード/エディタの個別参照用)。見つかれば 1。 */
ACS_EDITOR_API int acs_editor_settings_get_value(void* handle, const char* cat, const char* key,
                                                 char* out, int cap) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || out == nullptr || cap <= 0) return 0;
    const game::FSettingEntry* e = host->settings.Find(cat, key);
    if (e == nullptr) return 0;
    std::snprintf(out, static_cast<size_t>(cap), "%s", e->value);
    return 1;
}

/** MSAA サンプル数を設定する (1=FXAA のみ / 2 / 4 / 8)。次フレームの先頭で適用される。 */
ACS_EDITOR_API void acs_editor_set_msaa(void* handle, int samples) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return;
    u32 s = (samples >= 8) ? 8u : (samples >= 4) ? 4u : (samples >= 2) ? 2u : 1u;
    host->msaa_pending = s;
}

/** 現在の MSAA サンプル数を返す (フォールバック後の実効値)。 */
ACS_EDITOR_API int acs_editor_get_msaa(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    return (host != nullptr) ? static_cast<int>(host->msaa_samples) : 1;
}

ACS_EDITOR_API void acs_editor_destroy(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return;
    ClearStack(host->undo);   // undo/redo の heap スナップショットを解放
    ClearStack(host->redo);
    if (host->renderer.Device() != nullptr) host->renderer.Device()->WaitIdle();
    host->sprites.Shutdown();
    host->preview_sprites.Shutdown();
    host->fxaa.Shutdown();
    host->renderer.Shutdown();
    delete host;
}

// =============================================================================
// C ABI — シーン内省 / 編集 (Hierarchy / Inspector 配線用)
// =============================================================================

/** シーンのノード数。 */
ACS_EDITOR_API int acs_editor_node_count(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    return (host != nullptr) ? static_cast<int>(host->nodes.Size()) : 0;
}

/** リスト index 番目のノード id (範囲外は -1)。 */
ACS_EDITOR_API int acs_editor_node_id_at(void* handle, int index) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || index < 0 || index >= static_cast<int>(host->nodes.Size())) return -1;
    return host->nodes[static_cast<u32>(index)]->editor_id;
}

/** ノードの親 id (root 直下は -1、不明も -1)。 */
ACS_EDITOR_API int acs_editor_node_parent(void* handle, int id) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return -1;
    const FEditorNode* n = FindNode(*host, id);
    return (n != nullptr) ? ParentIdOf(*host, n) : -1;
}

/** ノード名 (UTF-8、不明は "")。返り値は内部バッファへのポインタ。 */
ACS_EDITOR_API const char* acs_editor_node_name(void* handle, int id) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return "";
    const FEditorNode* n = FindNode(*host, id);
    return (n != nullptr) ? n->name : "";
}

/** ノードのローカル transform を取得する (out 引数。FNode2D::Local() を読む)。 */
ACS_EDITOR_API void acs_editor_node_get_transform(void* handle, int id,
                                                  float* x, float* y, float* rot, float* sx, float* sy) {
    auto* host = static_cast<EditorHost*>(handle);
    FEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr) return;
    const game::FTransform2D& t = n->Local();
    if (x)   *x   = t.position.x;
    if (y)   *y   = t.position.y;
    if (rot) *rot = t.rotation;
    if (sx)  *sx  = t.scale.x;
    if (sy)  *sy  = t.scale.y;
}

/** ノードのローカル transform を設定する (Inspector の編集を FNode2D::Local() へ反映)。 */
ACS_EDITOR_API void acs_editor_node_set_transform(void* handle, int id,
                                                  float x, float y, float rot, float sx, float sy) {
    auto* host = static_cast<EditorHost*>(handle);
    FEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr) return;
    PushUndo(*host);
    game::FTransform2D& t = n->Local();
    t.position = FVec2{ x, y };
    t.rotation = rot;
    t.scale    = FVec2{ sx, sy };
}

// ----- ノードの表示プロパティ (色 / ベースサイズ / 可視 / 有効 / 描画レイヤ) -----

/** ノードの色 (RGBA) を取得する。 */
ACS_EDITOR_API void acs_editor_node_get_color(void* handle, int id,
                                              float* r, float* g, float* b, float* a) {
    auto* host = static_cast<EditorHost*>(handle);
    const FEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    const FVec4 c = (n != nullptr) ? n->color : FVec4{ 0.5f, 0.6f, 0.8f, 1.0f };
    if (r) *r = c.x; if (g) *g = c.y; if (b) *b = c.z; if (a) *a = c.w;
}

/** ノードの色 (RGBA) を設定する。 */
ACS_EDITOR_API void acs_editor_node_set_color(void* handle, int id, float r, float g, float b, float a) {
    auto* host = static_cast<EditorHost*>(handle);
    FEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr) return;
    PushUndo(*host);
    n->color = FVec4{ r, g, b, a };
}

/** ノードのベース表示サイズ (px)。 */
ACS_EDITOR_API float acs_editor_node_get_base(void* handle, int id) {
    auto* host = static_cast<EditorHost*>(handle);
    const FEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    return (n != nullptr) ? n->base : 0.0f;
}

/** ノードのベース表示サイズを設定する。 */
ACS_EDITOR_API void acs_editor_node_set_base(void* handle, int id, float base) {
    auto* host = static_cast<EditorHost*>(handle);
    FEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr) return;
    PushUndo(*host);
    n->base = base;
}

/** ノードの可視フラグ (1/0)。 */
ACS_EDITOR_API int acs_editor_node_get_visible(void* handle, int id) {
    auto* host = static_cast<EditorHost*>(handle);
    const FEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    return (n != nullptr && n->IsVisible()) ? 1 : 0;
}

/** ノードの可視フラグを設定する。 */
ACS_EDITOR_API void acs_editor_node_set_visible(void* handle, int id, int visible) {
    auto* host = static_cast<EditorHost*>(handle);
    FEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr) return;
    PushUndo(*host);
    n->SetVisible(visible != 0);
}

/** ノードの有効フラグ (1/0)。 */
ACS_EDITOR_API int acs_editor_node_get_enabled(void* handle, int id) {
    auto* host = static_cast<EditorHost*>(handle);
    const FEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    return (n != nullptr && n->IsEnabled()) ? 1 : 0;
}

/** ノードの有効フラグを設定する。 */
ACS_EDITOR_API void acs_editor_node_set_enabled(void* handle, int id, int enabled) {
    auto* host = static_cast<EditorHost*>(handle);
    FEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr) return;
    PushUndo(*host);
    n->SetEnabled(enabled != 0);
}

/** ノードの描画レイヤ (sort layer)。 */
ACS_EDITOR_API int acs_editor_node_get_sortlayer(void* handle, int id) {
    auto* host = static_cast<EditorHost*>(handle);
    const FEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    return (n != nullptr) ? n->SortLayer() : 0;
}

/** ノードの描画レイヤを設定する。 */
ACS_EDITOR_API void acs_editor_node_set_sortlayer(void* handle, int id, int layer) {
    auto* host = static_cast<EditorHost*>(handle);
    FEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr) return;
    PushUndo(*host);
    n->SetSortLayer(layer);
}

/** ノードにスプライト画像を割り当てる (UTF-8 パス)。即ロードを試み、成功 1 / 不明ノード 0。
 *  device 未準備でも path は保存され、描画時に遅延ロードされる (戻り値は 1)。 */
ACS_EDITOR_API int acs_editor_node_set_sprite(void* handle, int id, const char* utf8_path) {
    auto* host = static_cast<EditorHost*>(handle);
    FEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr || utf8_path == nullptr) return 0;
    PushUndo(*host);
    std::snprintf(n->sprite_path, sizeof(n->sprite_path), "%s", utf8_path);
    LoadNodeSprite(*host, n);
    return 1;
}

/** ノードのスプライト画像パス (UTF-8) を返す (未設定/不明は "")。 */
ACS_EDITOR_API const char* acs_editor_node_get_sprite(void* handle, int id) {
    auto* host = static_cast<EditorHost*>(handle);
    const FEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    return (n != nullptr) ? n->sprite_path : "";
}

/** ノードのスプライトを外す (色付き矩形表示に戻す)。成功 1 / 不明 0。 */
ACS_EDITOR_API int acs_editor_node_clear_sprite(void* handle, int id) {
    auto* host = static_cast<EditorHost*>(handle);
    FEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr) return 0;
    PushUndo(*host);
    n->sprite_path[0] = '\0';
    n->sprite_tex.Reset();
    return 1;
}

// ===== マテリアル (.acsmat = 効果プリセット) =====

/** ノードに使用マテリアル (.acsmat パス) を割り当てる。即解析。成功 1 / 不明 0。 */
ACS_EDITOR_API int acs_editor_node_set_material(void* handle, int id, const char* utf8_path) {
    auto* host = static_cast<EditorHost*>(handle);
    FEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr || utf8_path == nullptr) return 0;
    PushUndo(*host);
    std::snprintf(n->material_path, sizeof(n->material_path), "%s", utf8_path);
    LoadNodeMaterial(n);
    return 1;
}

/** ノードの使用マテリアルパス (UTF-8) を返す (未設定/不明は "")。 */
ACS_EDITOR_API const char* acs_editor_node_get_material(void* handle, int id) {
    auto* host = static_cast<EditorHost*>(handle);
    const FEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    return (n != nullptr) ? n->material_path : "";
}

/** ノードのマテリアルを外す (効果なしに戻す)。成功 1 / 不明 0。 */
ACS_EDITOR_API int acs_editor_node_clear_material(void* handle, int id) {
    auto* host = static_cast<EditorHost*>(handle);
    FEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr) return 0;
    PushUndo(*host);
    n->material_path[0] = '\0';
    LoadNodeMaterial(n);
    return 1;
}

/** マテリアルエディタで .acsmat を再保存した後、そのパスを使う全ノードのキャッシュを落として
 *  次フレームで再解析させる (見た目を即反映)。常に 1。 */
ACS_EDITOR_API int acs_editor_reload_material(void* handle, const char* utf8_path) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || utf8_path == nullptr) return 0;
    for (u32 i = 0; i < host->nodes.Size(); ++i) {
        FEditorNode* n = host->nodes[i];
        if (std::strcmp(n->material_path, utf8_path) == 0) LoadNodeMaterial(n);
    }
    return 1;
}

// ----- マテリアルアセット (.acsmat ファイル) の読み書き (マテリアルエディタ用) -----

/** 効果プリセットの総数 (ドロップダウン用)。 */
ACS_EDITOR_API int acs_editor_material_effect_count() {
    return static_cast<int>(game::SpriteEffectCount());
}

/** index 番目の効果プリセット名 (範囲外は "None")。dropdown index == 効果 enum 値。 */
ACS_EDITOR_API const char* acs_editor_material_effect_name(int index) {
    return game::SpriteEffectNameAt(static_cast<u32>(index < 0 ? 0 : index));
}

/** 効果プリセットの「見栄えのする既定パラメータ」を out へ書く。effect を切り替えたとき、
 *  破綻値にならないようマテリアルエディタがこれを使う。color4 は float[4] (RGBA)。 */
ACS_EDITOR_API void acs_editor_material_default_params(int effect,
                                                       float* strength, float* p0, float* p1, float* p2,
                                                       float* color4, int* animated) {
    const ESpriteEffect e = game::SpriteEffectAt(static_cast<u32>(effect < 0 ? 0 : effect));
    const FEffectParams p = game::DefaultEffectParams(e);
    if (strength) *strength = p.strength;
    if (p0)       *p0       = p.p0;
    if (p1)       *p1       = p.p1;
    if (p2)       *p2       = p.p2;
    if (color4) { color4[0] = p.color.x; color4[1] = p.color.y; color4[2] = p.color.z; color4[3] = p.color.w; }
    if (animated) *animated = game::EffectAnimatedByDefault(e) ? 1 : 0;
}

/** 既定 (None) のマテリアル .acsmat を path へ新規作成する。成功 1 / 失敗 0。 */
ACS_EDITOR_API int acs_editor_material_create(const char* path, const char* name) {
    if (path == nullptr) return 0;
    game::FMaterial2D mat;
    if (name != nullptr && name[0] != '\0')
        std::snprintf(mat.name, sizeof(mat.name), "%s", name);
    return game::SaveAcsmatFile(path, mat) ? 1 : 0;
}

/** .acsmat を読み込み、効果 enum / パラメータ / color / animated を out へ書く。成功 1 / 失敗 0。
 *  color4 は float[4] (RGBA)。name_buf に表示名を最大 name_cap-1 文字で書く (任意、null 可)。 */
ACS_EDITOR_API int acs_editor_material_load(const char* path,
                                            int* effect, float* strength,
                                            float* p0, float* p1, float* p2,
                                            float* color4, int* animated,
                                            char* name_buf, int name_cap) {
    if (path == nullptr) return 0;
    game::FMaterial2D mat;
    if (!game::LoadAcsmatFile(path, mat)) return 0;
    if (effect)   *effect   = static_cast<int>(mat.effect);
    if (strength) *strength = mat.params.strength;
    if (p0)       *p0       = mat.params.p0;
    if (p1)       *p1       = mat.params.p1;
    if (p2)       *p2       = mat.params.p2;
    if (color4) {
        color4[0] = mat.params.color.x; color4[1] = mat.params.color.y;
        color4[2] = mat.params.color.z; color4[3] = mat.params.color.w;
    }
    if (animated) *animated = mat.animated ? 1 : 0;
    if (name_buf != nullptr && name_cap > 0)
        std::snprintf(name_buf, static_cast<size_t>(name_cap), "%s", mat.name);
    return 1;
}

/** 効果マテリアルを .acsmat へ書き出す (kind=Effect)。既存ファイルがあれば PBR ブロックを保つ。成功 1。 */
ACS_EDITOR_API int acs_editor_material_save(const char* path, const char* name,
                                            int effect, float strength,
                                            float p0, float p1, float p2,
                                            float r, float g, float b, float a,
                                            int animated) {
    if (path == nullptr) return 0;
    game::FMaterial2D mat;
    game::LoadAcsmatFile(path, mat);   // 既存があれば読み込み (PBR ブロックを保持)。失敗時は既定。
    if (name != nullptr && name[0] != '\0')
        std::snprintf(mat.name, sizeof(mat.name), "%s", name);
    mat.kind            = game::EMaterialKind::Effect;
    mat.effect          = game::SpriteEffectAt(static_cast<u32>(effect < 0 ? 0 : effect));
    mat.params.strength = strength;
    mat.params.p0       = p0;
    mat.params.p1       = p1;
    mat.params.p2       = p2;
    mat.params.color    = FVec4{ r, g, b, a };
    mat.animated        = (animated != 0);
    return game::SaveAcsmatFile(path, mat) ? 1 : 0;
}

/** マテリアルの種別を返す (0=Lit/PBR, 1=Effect)。読めなければ 0 (PBR)。 */
ACS_EDITOR_API int acs_editor_material_kind(const char* path) {
    if (path == nullptr) return 0;
    game::FMaterial2D mat;
    if (!game::LoadAcsmatFile(path, mat)) return 0;
    return static_cast<int>(mat.kind);
}

/** マテリアルの種別を設定する (0=Lit/PBR, 1=Effect)。既存内容は保つ。成功 1 / 失敗 0。 */
ACS_EDITOR_API int acs_editor_material_set_kind(const char* path, int kind) {
    if (path == nullptr) return 0;
    game::FMaterial2D mat;
    if (!game::LoadAcsmatFile(path, mat)) return 0;
    mat.kind = (kind == 1) ? game::EMaterialKind::Effect : game::EMaterialKind::Lit;
    return game::SaveAcsmatFile(path, mat) ? 1 : 0;
}

/** .acsmat の PBR プロパティを out へ読む。base_color4/emissive3 は float 配列。成功 1 / 失敗 0。 */
ACS_EDITOR_API int acs_editor_material_load_pbr(const char* path,
                                                float* base_color4, float* metallic, float* roughness,
                                                float* emissive3, float* emissive_strength,
                                                float* normal_strength, float* ao,
                                                char* albedo_buf, int albedo_cap,
                                                char* normal_buf, int normal_cap) {
    if (path == nullptr) return 0;
    game::FMaterial2D mat;
    if (!game::LoadAcsmatFile(path, mat)) return 0;
    const game::FPbrParams2D& q = mat.pbr;
    if (base_color4) { base_color4[0] = q.baseColor.x; base_color4[1] = q.baseColor.y;
                       base_color4[2] = q.baseColor.z; base_color4[3] = q.baseColor.w; }
    if (metallic)          *metallic          = q.metallic;
    if (roughness)         *roughness         = q.roughness;
    if (emissive3) { emissive3[0] = q.emissive.x; emissive3[1] = q.emissive.y; emissive3[2] = q.emissive.z; }
    if (emissive_strength) *emissive_strength = q.emissiveStrength;
    if (normal_strength)   *normal_strength   = q.normalStrength;
    if (ao)                *ao                = q.ao;
    if (albedo_buf != nullptr && albedo_cap > 0)
        std::snprintf(albedo_buf, static_cast<size_t>(albedo_cap), "%s", q.albedoPath);
    if (normal_buf != nullptr && normal_cap > 0)
        std::snprintf(normal_buf, static_cast<size_t>(normal_cap), "%s", q.normalPath);
    return 1;
}

/** PBR マテリアルを .acsmat へ書き出す (kind=Lit)。既存があれば Effect ブロックを保つ。成功 1。 */
ACS_EDITOR_API int acs_editor_material_save_pbr(const char* path, const char* name,
                                                float br, float bg, float bb, float ba,
                                                float metallic, float roughness,
                                                float er, float eg, float eb,
                                                float emissive_strength, float normal_strength, float ao,
                                                const char* albedo_path, const char* normal_path) {
    if (path == nullptr) return 0;
    game::FMaterial2D mat;
    game::LoadAcsmatFile(path, mat);   // 既存があれば Effect ブロックを保持
    if (name != nullptr && name[0] != '\0')
        std::snprintf(mat.name, sizeof(mat.name), "%s", name);
    mat.kind                 = game::EMaterialKind::Lit;
    mat.pbr.baseColor        = FVec4{ br, bg, bb, ba };
    mat.pbr.metallic         = metallic;
    mat.pbr.roughness        = roughness;
    mat.pbr.emissive         = FVec3{ er, eg, eb };
    mat.pbr.emissiveStrength = emissive_strength;
    mat.pbr.normalStrength   = normal_strength;
    mat.pbr.ao               = ao;
    std::snprintf(mat.pbr.albedoPath, sizeof(mat.pbr.albedoPath), "%s", albedo_path ? albedo_path : "");
    std::snprintf(mat.pbr.normalPath, sizeof(mat.pbr.normalPath), "%s", normal_path ? normal_path : "");
    return game::SaveAcsmatFile(path, mat) ? 1 : 0;
}

/** .acsmat のシェーディングモード + トゥーン項目を out へ読む。成功 1 / 失敗 0。s1/s2/rim/spec は float[3]。 */
ACS_EDITOR_API int acs_editor_material_load_toon(const char* path, int* mode,
        float* s1, float* thr1, float* s2, float* thr2,
        float* rim, float* rim_power, float* spec, float* spec_thr, float* softness) {
    if (path == nullptr) return 0;
    game::FMaterial2D mat;
    if (!game::LoadAcsmatFile(path, mat)) return 0;
    const game::FPbrParams2D& q = mat.pbr;
    if (mode)      *mode      = q.shadingMode;
    if (s1)  { s1[0]=q.shadow1Color.x; s1[1]=q.shadow1Color.y; s1[2]=q.shadow1Color.z; }
    if (thr1)      *thr1      = q.shadow1Threshold;
    if (s2)  { s2[0]=q.shadow2Color.x; s2[1]=q.shadow2Color.y; s2[2]=q.shadow2Color.z; }
    if (thr2)      *thr2      = q.shadow2Threshold;
    if (rim) { rim[0]=q.rimColor.x; rim[1]=q.rimColor.y; rim[2]=q.rimColor.z; }
    if (rim_power) *rim_power = q.rimPower;
    if (spec){ spec[0]=q.specColor.x; spec[1]=q.specColor.y; spec[2]=q.specColor.z; }
    if (spec_thr)  *spec_thr  = q.specThreshold;
    if (softness)  *softness  = q.toonSoftness;
    return 1;
}

/** シェーディングモード + トゥーン項目を .acsmat へ書く (既存内容は保つ、kind=Lit)。成功 1 / 失敗 0。 */
ACS_EDITOR_API int acs_editor_material_save_toon(const char* path, int mode,
        float s1r, float s1g, float s1b, float thr1,
        float s2r, float s2g, float s2b, float thr2,
        float rimr, float rimg, float rimb, float rim_power,
        float specr, float specg, float specb, float spec_thr, float softness) {
    if (path == nullptr) return 0;
    game::FMaterial2D mat;
    game::LoadAcsmatFile(path, mat);   // 既存を保持
    mat.kind = game::EMaterialKind::Lit;
    mat.pbr.shadingMode      = mode;
    mat.pbr.shadow1Color     = FVec3{ s1r, s1g, s1b };
    mat.pbr.shadow1Threshold = thr1;
    mat.pbr.shadow2Color     = FVec3{ s2r, s2g, s2b };
    mat.pbr.shadow2Threshold = thr2;
    mat.pbr.rimColor         = FVec3{ rimr, rimg, rimb };
    mat.pbr.rimPower         = rim_power;
    mat.pbr.specColor        = FVec3{ specr, specg, specb };
    mat.pbr.specThreshold    = spec_thr;
    mat.pbr.toonSoftness     = softness;
    return game::SaveAcsmatFile(path, mat) ? 1 : 0;
}

/** .acsmat の Substrate 拡張 (clearcoat/異方/鏡面/シーン/SSS) を out へ読む。sheen/sss は float[3]。成功 1 / 失敗 0。 */
ACS_EDITOR_API int acs_editor_material_load_pbr_ext(const char* path,
        float* clearcoat, float* clearcoat_roughness, float* anisotropy,
        float* specular_level, float* specular_tint,
        float* sheen, float* sheen_roughness, float* sheen_color3,
        float* subsurface, float* sss_color3) {
    if (path == nullptr) return 0;
    game::FMaterial2D mat;
    if (!game::LoadAcsmatFile(path, mat)) return 0;
    const game::FPbrParams2D& q = mat.pbr;
    if (clearcoat)           *clearcoat           = q.clearcoat;
    if (clearcoat_roughness) *clearcoat_roughness = q.clearcoatRoughness;
    if (anisotropy)          *anisotropy          = q.anisotropy;
    if (specular_level)      *specular_level      = q.specularLevel;
    if (specular_tint)       *specular_tint       = q.specularTint;
    if (sheen)               *sheen               = q.sheen;
    if (sheen_roughness)     *sheen_roughness     = q.sheenRoughness;
    if (sheen_color3) { sheen_color3[0]=q.sheenColor.x; sheen_color3[1]=q.sheenColor.y; sheen_color3[2]=q.sheenColor.z; }
    if (subsurface)          *subsurface          = q.subsurface;
    if (sss_color3) { sss_color3[0]=q.subsurfaceColor.x; sss_color3[1]=q.subsurfaceColor.y; sss_color3[2]=q.subsurfaceColor.z; }
    return 1;
}

/** Substrate 拡張を .acsmat へ書く (既存内容は保つ、kind=Lit)。成功 1 / 失敗 0。 */
ACS_EDITOR_API int acs_editor_material_save_pbr_ext(const char* path,
        float clearcoat, float clearcoat_roughness, float anisotropy,
        float specular_level, float specular_tint,
        float sheen, float sheen_roughness, float sheen_r, float sheen_g, float sheen_b,
        float subsurface, float sss_r, float sss_g, float sss_b) {
    if (path == nullptr) return 0;
    game::FMaterial2D mat;
    game::LoadAcsmatFile(path, mat);   // 既存を保持
    mat.kind = game::EMaterialKind::Lit;
    mat.pbr.clearcoat          = clearcoat;
    mat.pbr.clearcoatRoughness = clearcoat_roughness;
    mat.pbr.anisotropy         = anisotropy;
    mat.pbr.specularLevel      = specular_level;
    mat.pbr.specularTint       = specular_tint;
    mat.pbr.sheen              = sheen;
    mat.pbr.sheenRoughness     = sheen_roughness;
    mat.pbr.sheenColor         = FVec3{ sheen_r, sheen_g, sheen_b };
    mat.pbr.subsurface         = subsurface;
    mat.pbr.subsurfaceColor    = FVec3{ sss_r, sss_g, sss_b };
    return game::SaveAcsmatFile(path, mat) ? 1 : 0;
}

// ===== マテリアル GPU プレビュー (実シェーダでサンプルを RT に描き readback) =====

/** CPU RGBA データから GPU テクスチャを作る。 */
static TUniquePtr<IRhiTexture> MakeTex(IRhiDevice& dev, u32 w, u32 h, const u8* rgba) noexcept {
    FTextureDesc td{};
    td.width = w; td.height = h;
    td.format = EFormat::R8G8B8A8_UNorm;
    td.initial_data = rgba;
    td.initial_data_size = w * h * 4;
    auto r = CreateRhiTexture(dev, td);
    return r.IsErr() ? TUniquePtr<IRhiTexture>{} : Move(r.Value());
}

/** プレビュー用サンプルテクスチャ (球アルベド/球法線/ミニ風景) を 1 度だけ生成する。 */
static void EnsurePreviewSamples(EditorHost& h) noexcept {
    if (h.preview_samples_ready) return;
    IRhiDevice* dev = h.renderer.Device();
    if (dev == nullptr) return;
    constexpr u32 S = 96;
    TArray<u8> alb; alb.Resize(S * S * 4);
    TArray<u8> nrm; nrm.Resize(S * S * 4);
    TArray<u8> scn; scn.Resize(S * S * 4);
    auto B = [](f32 v) -> u8 { return static_cast<u8>(v < 0 ? 0 : (v > 1 ? 255 : v * 255.0f + 0.5f)); };
    for (u32 y = 0; y < S; ++y) for (u32 x = 0; x < S; ++x) {
        const u32 i = (y * S + x) * 4;
        const f32 u = (x + 0.5f) / S * 2.0f - 1.0f;
        const f32 v = (y + 0.5f) / S * 2.0f - 1.0f;
        const f32 r2 = u * u + v * v;
        // 球 (アルベド = 白い円、法線 = 半球)
        if (r2 <= 1.0f) {
            const f32 nz = std::sqrt(1.0f - r2);
            nrm[i+0]=B(u*0.5f+0.5f); nrm[i+1]=B(v*0.5f+0.5f); nrm[i+2]=B(nz*0.5f+0.5f); nrm[i+3]=255;
            alb[i+0]=alb[i+1]=alb[i+2]=255; alb[i+3]=255;
        } else {
            nrm[i+0]=128; nrm[i+1]=128; nrm[i+2]=255; nrm[i+3]=255;
            alb[i+0]=alb[i+1]=alb[i+2]=0; alb[i+3]=0;
        }
        // ミニ風景 (効果プレビュー用): 空グラデ + 太陽 + 丘
        const f32 uu = (x + 0.5f) / S, vv = (y + 0.5f) / S;
        f32 rr, gg, bb;
        if (vv < 0.62f) { f32 t = vv / 0.62f; rr = 0.35f+(0.98f-0.35f)*t; gg = 0.60f+(0.62f-0.60f)*t; bb = 0.95f+(0.40f-0.95f)*t; }
        else            { f32 t = (vv-0.62f)/0.38f; rr = 0.20f+(0.10f-0.20f)*t; gg = 0.65f+(0.40f-0.65f)*t; bb = 0.25f+(0.18f-0.25f)*t; }
        const f32 dx = uu - 0.70f, dy = vv - 0.32f, d = std::sqrt(dx*dx + dy*dy);
        if (d < 0.15f) { f32 t = d/0.15f; rr = 1.0f+(rr-1.0f)*t; gg = 0.85f+(gg-0.85f)*t; bb = 0.30f+(bb-0.30f)*t; }
        scn[i+0]=B(rr); scn[i+1]=B(gg); scn[i+2]=B(bb); scn[i+3]=255;
    }
    h.preview_sphere_albedo = MakeTex(*dev, S, S, alb.Data());
    h.preview_sphere_normal = MakeTex(*dev, S, S, nrm.Data());
    h.preview_scene         = MakeTex(*dev, S, S, scn.Data());
    h.preview_samples_ready = true;
}

/** プレビュー RT (size×size) と専用コマンドリスト・非 MSAA バッチを必要なら (再)生成する。 */
static bool EnsurePreviewRt(EditorHost& h, u32 size) noexcept {
    IRhiDevice* dev = h.renderer.Device();
    if (dev == nullptr) return false;
    if (!h.preview_cl) {
        auto r = CreateRhiCommandList(*dev);
        if (r.IsErr()) return false;
        h.preview_cl = Move(r.Value());
    }
    if (!h.preview_rt || h.preview_rt_size != size) {
        FTextureDesc td{};
        td.width = size; td.height = size;
        td.format = h.renderer.ColorFormat();   // SpriteBatch パイプラインと同フォーマット
        td.is_render_target = true;
        auto r = CreateRhiTexture(*dev, td);
        if (r.IsErr()) return false;
        h.preview_rt = Move(r.Value());
        h.preview_rt_size = size;
    }
    // 本体バッチは MSAA PSO のため非 MSAA の preview_rt には描けない → 専用バッチを遅延初期化
    if (!h.preview_sprites_ready) {
        const auto r = h.preview_sprites.Init(*dev, h.renderer.ColorFormat(), 1024, 1);
        if (r.IsErr()) return false;
        h.preview_sprites_ready = true;
    }
    return true;
}

/** drawFn でサンプルを RT へ描き、CPU へ読み戻して out_rgba (size*size*4) に詰める。成功 1 / 失敗 0。 */
template<typename DrawFn>
static int RenderPreview(EditorHost& h, u32 size, u8* out_rgba, u32 out_size, DrawFn&& drawFn) noexcept {
    if (out_rgba == nullptr || size == 0 || out_size < size * size * 4) return 0;
    if (!EnsurePreviewRt(h, size)) return 0;
    EnsurePreviewSamples(h);
    IRhiDevice* dev = h.renderer.Device();
    IRhiCommandList* cl = h.preview_cl.Get();
    if (dev == nullptr || cl == nullptr || !h.preview_rt) return 0;

    cl->Begin();
    cl->BeginRenderToTexture(*h.preview_rt, ClearColor{ 0.10f, 0.10f, 0.12f, 1.0f }, nullptr, 1.0f);
    h.preview_sprites.Begin(*cl, size, size);
    drawFn(h.preview_sprites, static_cast<f32>(size));
    h.preview_sprites.End();
    cl->EndRenderToTexture(*h.preview_rt);
    cl->End();
    cl->Submit();
    dev->WaitIdle();
    return dev->ReadTexture(*h.preview_rt, out_rgba, out_size) ? 1 : 0;
}

/** PBR マテリアルを «ライト付きの球» として実シェーダで描きプレビューを out へ読み戻す。 */
ACS_EDITOR_API int acs_editor_render_preview_pbr(void* handle,
        float br, float bg, float bb, float ba, float metallic, float roughness,
        float er, float eg, float eb, float em_str, float normal_str, float ao,
        unsigned char* out_rgba, int size) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || !host->sprites_ready || size <= 0) return 0;
    const u32 s = static_cast<u32>(size);
    FLitMaterialParams lm;
    lm.baseColor = FVec4{ br, bg, bb, ba };
    lm.metallic = metallic; lm.roughness = roughness; lm.normalStrength = normal_str; lm.ao = ao;
    lm.emissive = FVec3{ er, eg, eb }; lm.emissiveStrength = em_str;
    return RenderPreview(*host, s, out_rgba, static_cast<u32>(s * s * 4),
        [&](FSpriteBatch& sb, f32 sz) {
            FSpriteLight light;
            light.pos = FVec2{ sz * 0.32f, sz * 0.28f };
            light.radius = sz * 1.6f; light.color = FVec3{ 1.0f, 0.97f, 0.92f }; light.intensity = 2.4f;
            FVec3 amb{ 0.06f, 0.07f, 0.09f };
            sb.SetLights(&light, 1, amb, sz * 0.45f);
            sb.SetLitMaterial(lm, host->preview_sphere_normal.Get());
            if (host->preview_sphere_albedo)
                sb.Draw(*host->preview_sphere_albedo, 0, 0, sz, sz);
            sb.ClearLit();
        });
}

/** 効果プリセットをミニ風景に適用した実シェーダプレビューを out へ読み戻す。 */
ACS_EDITOR_API int acs_editor_render_preview_effect(void* handle,
        int effect, float strength, float p0, float p1, float p2,
        float r, float g, float b, float a, float time,
        unsigned char* out_rgba, int size) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || !host->sprites_ready || size <= 0) return 0;
    const u32 s = static_cast<u32>(size);
    const ESpriteEffect e = game::SpriteEffectAt(static_cast<u32>(effect < 0 ? 0 : effect));
    FEffectParams p;
    p.strength = strength; p.p0 = p0; p.p1 = p1; p.p2 = p2; p.time = time;
    p.color = FVec4{ r, g, b, a };
    return RenderPreview(*host, s, out_rgba, static_cast<u32>(s * s * 4),
        [&](FSpriteBatch& sb, f32 sz) {
            const bool fx = (e != ESpriteEffect::None);
            if (fx) sb.SetEffect(e, p);
            if (host->preview_scene) sb.Draw(*host->preview_scene, 0, 0, sz, sz);
            if (fx) sb.ClearEffect();
        });
}

/** .acsmat を読み込み «実シェーダ» でプレビューを描く (PBR/トゥーン=球、効果=ミニ風景)。
 *  種別/シェーディングモード/トゥーン項目を全てファイルから読むので分岐は engine 側で完結。 */
ACS_EDITOR_API int acs_editor_render_preview_material(void* handle, const char* path,
                                                      unsigned char* out_rgba, int size) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || !host->sprites_ready || size <= 0 || path == nullptr) return 0;
    const u32 s = static_cast<u32>(size);
    game::FMaterial2D mat;
    if (!game::LoadAcsmatFile(path, mat)) return 0;

    if (mat.kind == game::EMaterialKind::Effect) {       // 効果プリセット: ミニ風景に適用
        const bool fx = (mat.effect != ESpriteEffect::None);
        FEffectParams p = mat.params;
        if (mat.animated) p.time = 1.0f;
        return RenderPreview(*host, s, out_rgba, static_cast<u32>(s * s * 4),
            [&](FSpriteBatch& sb, f32 sz) {
                if (fx) sb.SetEffect(mat.effect, p);
                if (host->preview_scene) sb.Draw(*host->preview_scene, 0, 0, sz, sz);
                if (fx) sb.ClearEffect();
            });
    }
    // Lit (PBR or Toon): ライト付きの球。shadingMode はシェーダが見る。
    const FLitMaterialParams lm = game::ToLitParams(mat.pbr);
    return RenderPreview(*host, s, out_rgba, static_cast<u32>(s * s * 4),
        [&](FSpriteBatch& sb, f32 sz) {
            FSpriteLight light;
            light.pos = FVec2{ sz * 0.32f, sz * 0.28f };
            light.radius = sz * 1.6f; light.color = FVec3{ 1.0f, 0.97f, 0.92f }; light.intensity = 2.4f;
            sb.SetLights(&light, 1, FVec3{ 0.06f, 0.07f, 0.09f }, sz * 0.45f);
            sb.SetLitMaterial(lm, host->preview_sphere_normal.Get());
            if (host->preview_sphere_albedo) sb.Draw(*host->preview_sphere_albedo, 0, 0, sz, sz);
            sb.ClearLit();
        });
}

// ===== Play モード (物理プレビュー) =====

/** ノードに付与された FRigidBody2D コンポーネントの slot を返す (無ければ -1)。 */
static int RigidBodySlot(const FEditorNode* n) noexcept {
    static game::FTypeId s_rbId = 0;
    if (s_rbId == 0) {
        const game::FTypeDesc* d = game::FTypeRegistry::Get().FindByName("FRigidBody2D");
        if (d != nullptr) s_rbId = d->id;
    }
    if (s_rbId == 0) return -1;
    for (u32 c = 0; c < n->component_count; ++c)
        if (n->components[c] == s_rbId) return static_cast<int>(c);
    return -1;
}

/** FRigidBody2D を付けたノードから剛体ワールドを組み立てる。
 *  プロパティ (bodyType/shape/restitution/friction/mass/damping) はコンポーネントの
 *  編集値 (comp_props、スキーマ順) から読む。動的のみ書き戻し対象に記録する。 */
static void EditorBuildPlayWorld(EditorHost& h) noexcept {
    h.play_world = MakeUnique<game::FRigidWorld2D>();
    h.play_body.Clear();
    h.play_node.Clear();
    if (h.play_world.Get() == nullptr) return;
    for (u32 i = 0; i < h.nodes.Size(); ++i) {
        FEditorNode* n = h.nodes[i];
        const int slot = RigidBodySlot(n);
        if (slot < 0) continue;                                 // 剛体ボディ未付与 → 物理なし
        const u32 s = static_cast<u32>(slot);
        const int bodyType = static_cast<int>(n->comp_props[s][0][0]);   // 0=Static, 1=Dynamic
        const f32 rest = n->comp_props[s][1][0];
        const f32 fric = n->comp_props[s][2][0];
        const f32 massV = n->comp_props[s][3][0];
        const f32 lind = n->comp_props[s][4][0];
        const f32 angd = n->comp_props[s][5][0];
        const int prShape = PrimitiveShape(n);                           // コライダー形状 = レンダラーの shape
        const bool dynamic = (bodyType == 1);
        const f32 mass = (massV > 0.001f) ? massV : 1.0f;

        const game::FTransform2D w = n->World();
        const f32 sx = (w.scale.x != 0.0f) ? w.scale.x : 1.0f;
        const f32 sy = (w.scale.y != 0.0f) ? w.scale.y : 1.0f;
        const FVec2 pos{ w.position.x, w.position.y };

        u32 bi;
        if (prShape == 3 && n->poly_count >= 3) {               // カスタムポリゴン
            FVec2 lv[game::kMaxPolyVerts];
            for (u32 k = 0; k < n->poly_count; ++k)
                lv[k] = FVec2{ n->poly_verts[k].x * sx, n->poly_verts[k].y * sy };   // scale を焼き込む (rot は SetAngle)
            bi = h.play_world->AddPolygon(pos, lv, n->poly_count, dynamic ? mass : 0.0f, rest, fric);
        } else if (prShape == 1) {                              // 円
            const f32 radius = n->base * 0.5f * ((sx > sy) ? sx : sy);
            bi = h.play_world->AddCircle(pos, radius, dynamic ? mass : 0.0f, rest, fric);
        } else {                                                // 箱 (OBB)
            const FVec2 half{ n->base * 0.5f * sx, n->base * 0.5f * sy };
            bi = dynamic ? h.play_world->AddDynamicAabb(pos, half, mass, rest, fric)
                         : h.play_world->AddStaticAabb(pos, half, rest, fric);
        }
        h.play_world->SetAngle(bi, w.rotation);                 // 向きを衝突に反映 (斜面/初期回転)
        if (dynamic) {
            h.play_world->SetDamping(bi, lind, angd);
            h.play_body.PushBack(bi);                           // 動的のみ書き戻し対象
            h.play_node.PushBack(n->editor_id);
        }
    }
}

/** 1 フレーム物理を進め、ボディ位置/角度を対応ノードへ書き戻す (undo は積まない)。 */
static void EditorStepPlay(EditorHost& h, f32 dt) noexcept {
    if (h.play_world.Get() == nullptr) return;
    if (dt > 0.05f) dt = 0.05f;                          // 大 dt を抑制 (安定性)
    h.play_world->Step(dt, FVec2{ 0.0f, 900.0f });       // +Y = 画面下 → 下向き重力
    for (u32 i = 0; i < h.play_node.Size(); ++i) {
        FEditorNode* n = FindNode(h, h.play_node[i]);
        if (n == nullptr) continue;
        const u32 bi = h.play_body[i];
        const FVec2 p = h.play_world->Position(bi);
        SetNodeWorldPosition(h, n, p.x, p.y);
        n->Local().rotation = h.play_world->Angle(bi);   // 簡易 (親回転は無視)
    }
}

/** 再生を開始する (現在状態をスナップショットし、物理ワールドを構築)。成功 1 / 既に再生中 0。 */
ACS_EDITOR_API int acs_editor_play_start(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || host->play_state != 0) return 0;
    host->play_snapshot = DupSnapshot(*host);            // 停止時の復元用
    EditorBuildPlayWorld(*host);
    host->play_state = 1;
    return 1;
}

/** 再生を停止し、開始時の状態へ復元する。成功 1 / 停止中 0。 */
ACS_EDITOR_API int acs_editor_play_stop(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || host->play_state == 0) return 0;
    host->play_state = 0;
    host->play_world.Reset();
    host->play_body.Clear();
    host->play_node.Clear();
    if (host->play_snapshot != nullptr) {
        LoadSceneText(*host, host->play_snapshot);       // 開始状態へ復元 (undo は積まない)
        delete[] host->play_snapshot;
        host->play_snapshot = nullptr;
    }
    return 1;
}

/** 再生の一時停止/再開を切り替える (paused!=0 で一時停止)。 */
ACS_EDITOR_API void acs_editor_play_set_paused(void* handle, int paused) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return;
    if (paused != 0 && host->play_state == 1)      host->play_state = 2;
    else if (paused == 0 && host->play_state == 2) host->play_state = 1;
}

/** 一時停止中に物理を 1 フレームだけ進める。 */
ACS_EDITOR_API void acs_editor_play_step(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host != nullptr && host->play_state == 2) EditorStepPlay(*host, 1.0f / 60.0f);
}

/** 再生状態を返す (0=stopped, 1=playing, 2=paused)。 */
ACS_EDITOR_API int acs_editor_play_state(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    return (host != nullptr) ? host->play_state : 0;
}

// ===== ポリゴン描画ツール =====

/** ポリゴン描画を開始する (点をクリックで集める)。 */
ACS_EDITOR_API void acs_editor_poly_begin(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return;
    host->poly_drawing = true;
    host->poly_points.Clear();
}

/** 描画中にスクリーン点を 1 つ追加する (world に変換して保持)。 */
ACS_EDITOR_API void acs_editor_poly_add_point(void* handle, float sx, float sy) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || !host->poly_drawing) return;
    host->poly_points.PushBack(FVec2{ S2WX(*host, sx), S2WY(*host, sy) });
}

/** 集めた点を 1 ノード (FPrimitiveRenderer2D=Polygon) として確定する。新 id / 3 点未満は -1。
 *  クリック点をアンカーとして閉じた Catmull-Rom 曲線で滑らかにし (描画 = render_verts)、
 *  その凸包を間引いてコライダー (poly_verts ≤kMaxPolyVerts) を生成する。 */
ACS_EDITOR_API int acs_editor_poly_finalize(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return -1;
    host->poly_drawing = false;
    u32 np = host->poly_points.Size();
    if (np < 3) { host->poly_points.Clear(); return -1; }

    // アンカー (クリック点) を world でコピー (ローカルバッファ上限まで)。
    constexpr u32 kRV = FEditorNode::kMaxRenderVerts;
    if (np > kRV) np = kRV;
    FVec2 anchors[kRV];
    for (u32 i = 0; i < np; ++i) anchors[i] = host->poly_points[i];

    // 閉じた滑らかな曲線をサンプル (world)。
    FVec2 smooth[kRV];
    u32 sc = SmoothClosedSpline(anchors, np, smooth, kRV);
    if (sc < 3) { host->poly_points.Clear(); return -1; }

    // 重心 (ノード原点) を曲線から求める。
    f32 cx = 0.0f, cy = 0.0f;
    for (u32 i = 0; i < sc; ++i) { cx += smooth[i].x; cy += smooth[i].y; }
    cx /= static_cast<f32>(sc); cy /= static_cast<f32>(sc);

    // 描画用頂点 (重心基準) と、ピック枠用の最大半径。
    FVec2 rlocal[kRV]; f32 maxR = 1.0f;
    for (u32 i = 0; i < sc; ++i) {
        rlocal[i] = FVec2{ smooth[i].x - cx, smooth[i].y - cy };
        const f32 r = std::sqrt(rlocal[i].x * rlocal[i].x + rlocal[i].y * rlocal[i].y);
        if (r > maxR) maxR = r;
    }

    // コライダー = 滑らか曲線の凸包を ≤kMaxPolyVerts に間引いたもの。
    FVec2 collider[game::kMaxPolyVerts];
    u32 cc = ConvexHullDecimated(rlocal, sc, collider, game::kMaxPolyVerts);

    PushUndo(*host);
    FEditorNode* n = AddEditorNode(*host, -1, "Polygon", cx, cy, 0.0f, maxR * 2.0f, FVec4{ 0.55f, 0.75f, 0.95f, 1.0f });
    n->render_count = sc;
    for (u32 i = 0; i < sc; ++i) n->render_verts[i] = rlocal[i];
    n->poly_count = cc;
    for (u32 i = 0; i < cc; ++i) n->poly_verts[i] = collider[i];
    AttachComponent(n, "FPrimitiveRenderer2D");
    SetCompProp(n, 0, 0, 3.0f, 0.0f, 0.0f, 0.0f);             // renderer.shape = 3 (Polygon)
    host->poly_points.Clear();
    SelSet(*host, n->editor_id);
    return n->editor_id;
}

/** ポリゴン描画を破棄する。 */
ACS_EDITOR_API void acs_editor_poly_cancel(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return;
    host->poly_drawing = false;
    host->poly_points.Clear();
}

/** ポリゴン描画中か (1/0)。 */
ACS_EDITOR_API int acs_editor_poly_is_drawing(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    return (host != nullptr && host->poly_drawing) ? 1 : 0;
}

/** ノードを単一選択する (集合を {id} に置換、primary=id。ビューポートでハイライト)。 */
ACS_EDITOR_API void acs_editor_select(void* handle, int id) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host != nullptr) SelSet(*host, id);
}

/** 現在の primary (active) ノード id (未選択は -1)。 */
ACS_EDITOR_API int acs_editor_selected(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    return (host != nullptr) ? host->selected : -1;
}

// ----- 複数選択 (Ctrl+click トグル / 全選択 / 解除 / 集合の列挙) -----

/** id の選択を反転する (Ctrl+click)。追加で primary になり、primary を外すと別の一員へ移る。 */
ACS_EDITOR_API void acs_editor_select_toggle(void* handle, int id) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host != nullptr) SelToggle(*host, id);
}

/** 全ノードを選択する (primary は最後のノード)。 */
ACS_EDITOR_API void acs_editor_select_all(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return;
    host->selection.Clear();
    for (u32 i = 0; i < host->nodes.Size(); ++i) host->selection.PushBack(host->nodes[i]->editor_id);
    host->selected = (host->selection.Size() > 0)
                     ? host->selection[host->selection.Size() - 1] : -1;
}

/** 選択を全解除する。 */
ACS_EDITOR_API void acs_editor_select_none(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host != nullptr) SelClear(*host);
}

/** 選択集合の要素数。 */
ACS_EDITOR_API int acs_editor_selection_count(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    return (host != nullptr) ? static_cast<int>(host->selection.Size()) : 0;
}

/** 選択集合の index 番目のノード id (範囲外は -1)。 */
ACS_EDITOR_API int acs_editor_selection_at(void* handle, int index) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || index < 0 || index >= static_cast<int>(host->selection.Size())) return -1;
    return host->selection[static_cast<u32>(index)];
}

/** id が選択集合に含まれるか (1/0)。 */
ACS_EDITOR_API int acs_editor_selection_contains(void* handle, int id) {
    auto* host = static_cast<EditorHost*>(handle);
    return (host != nullptr && SelContains(*host, id)) ? 1 : 0;
}

/**
 * screen 矩形に中心が入るノードを選択する (ラバーバンド選択)。additive=0 で置換、!=0 で追加。
 *
 * @return 矩形内に入った (新規追加 + 既存) ノード総数。
 */
ACS_EDITOR_API int acs_editor_select_box(void* handle, float x0, float y0, float x1, float y1, int additive) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return 0;
    const f32 minx = (x0 < x1) ? x0 : x1, maxx = (x0 < x1) ? x1 : x0;
    const f32 miny = (y0 < y1) ? y0 : y1, maxy = (y0 < y1) ? y1 : y0;
    if (additive == 0) host->selection.Clear();
    int inBox = 0;
    for (u32 i = 0; i < host->nodes.Size(); ++i) {
        FEditorNode* n = host->nodes[i];
        const game::FTransform2D w = n->World();
        const f32 sx = W2SX(*host, w.position.x);
        const f32 sy = W2SY(*host, w.position.y);
        if (sx >= minx && sx <= maxx && sy >= miny && sy <= maxy) {   // 中心が矩形内
            ++inBox;
            if (!SelContains(*host, n->editor_id)) host->selection.PushBack(n->editor_id);
        }
    }
    // primary を整える: 追加で既存 primary が有効ならそのまま、でなければ末尾の一員へ。
    if (host->selection.Size() > 0) {
        if (additive == 0 || host->selected < 0 || !SelContains(*host, host->selected))
            host->selected = host->selection[host->selection.Size() - 1];
    } else {
        host->selected = -1;
    }
    return inBox;
}

/** ラバーバンド矩形のオーバーレイ表示を設定する (active!=0 で screen 座標の矩形を描く)。 */
ACS_EDITOR_API void acs_editor_set_marquee(void* handle, int active, float x0, float y0, float x1, float y1) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return;
    host->marquee_active = (active != 0);
    host->marquee_x0 = x0; host->marquee_y0 = y0;
    host->marquee_x1 = x1; host->marquee_y1 = y1;
}

// =============================================================================
// C ABI — 型レジストリ内省 (エディタが「エンジンの登録型」を列挙するため)
// -----------------------------------------------------------------------------
// エンジンが ACS_REGISTER* で把握している全型 (Component / System / Scene / Asset /
// enum …) を、エディタがハードコードせず実行時に列挙できるようにする。handle 不要
// (プロセス唯一の FTypeRegistry を引く)。AcsRegisterEngineTypes() は create 時に
// 呼ばれ済みだが、念のため各エントリで冪等に確定しておく。
// =============================================================================

/** 登録されているエンジン型の総数。 */
ACS_EDITOR_API int acs_editor_type_count(void) {
    acs::game::AcsRegisterEngineTypes();
    return static_cast<int>(acs::game::FTypeRegistry::Get().Count());
}

/** index 番目の型名 (範囲外は "")。 */
ACS_EDITOR_API const char* acs_editor_type_name_at(int index) {
    if (index < 0) return "";
    const auto* d = acs::game::FTypeRegistry::Get().At(static_cast<u32>(index));
    return (d != nullptr && d->name != nullptr) ? d->name : "";
}

/** index 番目の型のカテゴリ (ETypeCategory の整数値、範囲外は -1)。 */
ACS_EDITOR_API int acs_editor_type_category_at(int index) {
    if (index < 0) return -1;
    const auto* d = acs::game::FTypeRegistry::Get().At(static_cast<u32>(index));
    return (d != nullptr) ? static_cast<int>(d->category) : -1;
}

/** index 番目の型が default 構築可能か (factory で生成できる = 1)。 */
ACS_EDITOR_API int acs_editor_type_instantiable_at(int index) {
    if (index < 0) return 0;
    const auto* d = acs::game::FTypeRegistry::Get().At(static_cast<u32>(index));
    if (d == nullptr) return 0;
    return ((d->traits & acs::game::TRAIT_INSTANTIABLE) != 0u) ? 1 : 0;
}

/** index 番目の型のフィールド数 (Component/Struct)、または列挙値数 (Enum)。 */
ACS_EDITOR_API int acs_editor_type_member_count_at(int index) {
    if (index < 0) return 0;
    const auto* d = acs::game::FTypeRegistry::Get().At(static_cast<u32>(index));
    if (d == nullptr) return 0;
    return static_cast<int>(d->category == acs::game::ETypeCategory::Enum
                                ? d->enum_count : d->field_count);
}

// ===== ユーザー定義型 (ゲーム DLL から取り込んだもの) の列挙 =====

/** ゲーム DLL から取り込んだユーザー定義型の数。 */
ACS_EDITOR_API int acs_editor_user_type_count(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    return (host != nullptr) ? static_cast<int>(host->user_types.Size()) : 0;
}

/** i 番目のユーザー定義型の名前 (UTF-8、範囲外は "")。 */
ACS_EDITOR_API const char* acs_editor_user_type_name_at(void* handle, int index) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || index < 0 || static_cast<u32>(index) >= host->user_types.Size()) return "";
    return host->user_types[static_cast<u32>(index)]->name;
}

// 安全な NUL 終端文字列コピー (cap はバッファ長)。
static void CopyCStr(char* dst, const char* src, u32 cap) noexcept {
    if (dst == nullptr || cap == 0) return;
    u32 i = 0;
    if (src != nullptr) for (; src[i] != '\0' && i + 1 < cap; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

// id で既存ユーザー型を探す (無ければ nullptr)。
static FUserType* FindUserType(EditorHost& h, game::FTypeId id) noexcept {
    for (u32 i = 0; i < h.user_types.Size(); ++i)
        if (h.user_types[i]->desc.id == id) return h.user_types[i].Get();
    return nullptr;
}

/**
 * ゲームのリフレクション DLL をロードし、ユーザー定義の Component 型スキーマを取り込む。
 *
 * @details DLL の C ABI (acs_game_reflect_*) で型を列挙し、エンジンに無い Component をディープ
 * コピーして host->user_types に保持 + エディタの FTypeRegistry へ登録する (→ AttachComponent /
 * インスペクタ / シリアライズが engine 型と同経路で動く)。既存型は in-place 更新。コピー後 DLL は
 * FreeLibrary する (再ビルドで上書き可能に)。
 * @return 取り込んだ/更新した型数、LoadLibrary 失敗 -1、シンボル欠如 -2。
 */
ACS_EDITOR_API int acs_editor_load_game_dll(void* handle, const char* path) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || path == nullptr) return 0;
    void* dll = LoadLibraryA(path);
    if (dll == nullptr) return -1;

    auto init  = reinterpret_cast<void(*)()>(GetProcAddress(dll, "acs_game_reflect_init"));
    auto count = reinterpret_cast<unsigned(*)()>(GetProcAddress(dll, "acs_game_reflect_count"));
    auto nameF = reinterpret_cast<const char*(*)(unsigned)>(GetProcAddress(dll, "acs_game_reflect_name"));
    auto catF  = reinterpret_cast<int(*)(unsigned)>(GetProcAddress(dll, "acs_game_reflect_category"));
    auto fcF   = reinterpret_cast<unsigned(*)(unsigned)>(GetProcAddress(dll, "acs_game_reflect_field_count"));
    auto fnF   = reinterpret_cast<const char*(*)(unsigned, unsigned)>(GetProcAddress(dll, "acs_game_reflect_field_name"));
    auto fkF   = reinterpret_cast<int(*)(unsigned, unsigned)>(GetProcAddress(dll, "acs_game_reflect_field_kind"));
    auto fdF   = reinterpret_cast<void(*)(unsigned, unsigned, float*)>(GetProcAddress(dll, "acs_game_reflect_field_defaults"));
    if (count == nullptr || nameF == nullptr) { FreeLibrary(dll); return -2; }
    if (init != nullptr) init();

    int imported = 0;
    const unsigned n = count();
    for (unsigned i = 0; i < n; ++i) {
        const int category = (catF != nullptr) ? catF(i) : -1;
        if (category != static_cast<int>(game::ETypeCategory::Component)) continue;   // 今は Component のみ取り込む
        const char* tn = nameF(i);
        if (tn == nullptr || tn[0] == '\0') continue;
        const game::FTypeId id = game::AcsTypeHash(tn);
        FUserType* ut = FindUserType(*host, id);
        const game::FTypeDesc* eng = game::FTypeRegistry::Get().FindById(id);
        if (eng != nullptr && ut == nullptr) continue;   // エンジン型 (= editor registry に既存) → skip

        const bool isNew = (ut == nullptr);
        if (isNew) {
            host->user_types.PushBack(MakeUnique<FUserType>());
            ut = host->user_types[host->user_types.Size() - 1].Get();
        }
        CopyCStr(ut->name, tn, sizeof(ut->name));
        unsigned fc = (fcF != nullptr) ? fcF(i) : 0u;
        if (fc > FUserType::kMaxFields) fc = FUserType::kMaxFields;
        for (unsigned j = 0; j < fc; ++j) {
            CopyCStr(ut->field_names[j], (fnF != nullptr) ? fnF(i, j) : "", 48);
            ut->fields[j].kind   = static_cast<game::EFieldKind>((fkF != nullptr) ? fkF(i, j) : 0);
            ut->fields[j].offset = 0; ut->fields[j].size = 0; ut->fields[j].flags = 0;
            float d4[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            if (fdF != nullptr) fdF(i, j, d4);
            for (int k = 0; k < 4; ++k) ut->fields[j].defaults[k] = d4[k];
        }
        ut->Rebuild(category, fc);
        if (isNew) game::FTypeRegistry::Get().Register(&ut->desc);   // 既存は in-place 更新で反映済み
        ++imported;
    }
    FreeLibrary(dll);   // スキーマはディープコピー済 → 解放 (再ビルドで DLL を上書きできる)
    return imported;
}

// ===== シーン実体化 (authored 値で実コンポーネントを attach → tick = 実ロジック実行) =====

/**
 * 各ノードのコンポーネント・メタデータ + comp_props (authored 値) から «実 FComponent2D» を
 * 生成し、反射オフセット経由で値を実体へ適用してノードへ attach する。
 *
 * @details reg.CreateById → ApplyFieldValue(値適用ブリッジ) → AttachComponent。offset 付き
 * フィールド (ACS_RFIELD_D = ユーザー型) は authored 値が乗り、offset 無し (ACS_RPROP) は factory
 * 既定値で動く。tick_instances で UpdateTree すると実 OnUpdate が走る。
 * @return attach した実コンポーネント数。
 */
ACS_EDITOR_API int acs_editor_instantiate_scene(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return 0;
    // 既存の実体を一掃してから作り直す。
    for (u32 i = 0; i < host->nodes.Size(); ++i) host->nodes[i]->RemoveAllComponents();

    int total = 0;
    for (u32 i = 0; i < host->nodes.Size(); ++i) {
        FEditorNode* n = host->nodes[i];
        for (u32 s = 0; s < n->component_count; ++s) {
            const game::FTypeDesc* d = game::FTypeRegistry::Get().FindById(n->components[s]);
            if (d == nullptr || d->name == nullptr) continue;
            TUniquePtr<game::FComponent2D> comp = game::CreateComponentByName(d->name);
            if (!comp) continue;                       // 非 Component / Abstract はスキップ
            void* obj = comp.Get();
            for (u32 j = 0; j < d->field_count && j < FEditorNode::kMaxProps; ++j)
                game::ApplyFieldValue(obj, d->fields[j], n->comp_props[s][j]);   // authored 値を実体へ
            n->AttachComponent(static_cast<TUniquePtr<game::FComponent2D>&&>(comp));
            ++total;
        }
    }
    host->instances_live = true;
    return total;
}

/** 実体化したコンポーネントを 1 フレーム tick する (実 OnUpdate を実行)。 */
ACS_EDITOR_API void acs_editor_tick_instances(void* handle, float dt) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || !host->instances_live || host->root.Get() == nullptr) return;
    host->root->UpdateTree(dt);
    host->root->ResolveStructuralChanges();
}

/** 実体化したコンポーネントを全て除去し、編集 (メタデータのみ) 状態へ戻す。 */
ACS_EDITOR_API void acs_editor_clear_instances(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return;
    for (u32 i = 0; i < host->nodes.Size(); ++i) host->nodes[i]->RemoveAllComponents();
    host->instances_live = false;
}

/** 現在 attach されている «実» コンポーネントの総数 (= 実体化できた数。検証/表示用)。 */
ACS_EDITOR_API int acs_editor_instance_count(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return 0;
    int total = 0;
    for (u32 i = 0; i < host->nodes.Size(); ++i) total += static_cast<int>(host->nodes[i]->ComponentCount());
    return total;
}

// ===== インプロセス Play (ゲーム DLL がユーザーコンポーネントを実行) =====

ACS_EDITOR_API void acs_editor_logic_play_stop(void* handle);   // 前方宣言 (start が呼ぶ)

/** Play 中の 1 フレーム: DLL シーンを tick → 各ノード transform を読み戻して描画へ反映。 */
static void EditorTickLogic(EditorHost& h, f32 dt) noexcept {
    if (!h.logic_play || h.logic_scene == nullptr) return;
    if (dt > 0.05f) dt = 0.05f;
    // フレーム先頭で入力状態を進める (now→prev)。WPF からの key イベントは
    // acs_editor_logic_input_key 経由で DLL の acs::Input へ随時フィードされる。
    if (h.logic_shim.input_update != nullptr) h.logic_shim.input_update();
    // PUSH: editor ノードの現在 transform (= editor 物理 Play が動かした結果) を DLL へ送ってから
    // tick する。これでコンポーネントの OnDraw / OnUpdate が «物理で動いた位置» を基準に走り、
    // editor メタデータ描画 (物理) と DLL の OnDraw が同じ位置に揃う (二重シミュの位置ずれ解消)。
    for (u32 i = 0; i < h.nodes.Size(); ++i) {
        const game::FTransform2D& t = h.nodes[i]->Local();
        h.logic_shim.set_transform(h.logic_scene, static_cast<int>(i),
                                   t.position.x, t.position.y, t.rotation, t.scale.x, t.scale.y);
    }
    h.logic_shim.tick(h.logic_scene, dt);
    for (u32 i = 0; i < h.nodes.Size(); ++i) {   // dll node idx == host->nodes index (順次構築)
        f32 x = 0, y = 0, r = 0, sx = 1, sy = 1;
        h.logic_shim.get_transform(h.logic_scene, static_cast<int>(i), &x, &y, &r, &sx, &sy);
        // DLL が «動かしたノードだけ» 反映する (開始時 transform と同一なら触らない)。
        // → コンポーネントの無いノードは物理 Play 等の結果を保てる (両 Play の共存)。
        const game::FTransform2D& sv = (i < h.logic_saved.Size()) ? h.logic_saved[i] : h.nodes[i]->Local();
        if (x != sv.position.x || y != sv.position.y || r != sv.rotation || sx != sv.scale.x || sy != sv.scale.y) {
            game::FTransform2D& t = h.nodes[i]->Local();
            t.position = FVec2{ x, y }; t.rotation = r; t.scale = FVec2{ sx, sy };
        }
    }

    // game camera を読み戻して editor viewport を追従させる。ただし «ユーザーロジックがカメラを
    // 動かしたときだけ» 追従する。動かしていない (Play 開始時の値のまま) 間は editor cam に触れず、
    // ユーザーが Play 中も自由に pan/zoom できる。一度 game がカメラを動かしたら以後は追従モード。
    if (h.logic_shim.get_camera != nullptr && h.width > 0 && h.height > 0) {
        f32 cx = 0.0f, cy = 0.0f, z = 1.0f;
        h.logic_shim.get_camera(h.logic_scene, &cx, &cy, &z);
        if (z > 0.0001f) {
            if (!h.logic_cam_following) {
                const bool moved = (cx < h.logic_game_cam0_x - 0.01f) || (cx > h.logic_game_cam0_x + 0.01f)
                                || (cy < h.logic_game_cam0_y - 0.01f) || (cy > h.logic_game_cam0_y + 0.01f)
                                || (z  < h.logic_game_cam0_zoom - 0.001f) || (z > h.logic_game_cam0_zoom + 0.001f);
                if (moved) h.logic_cam_following = true;
            }
            if (h.logic_cam_following) {
                const f32 w = static_cast<f32>(h.width), hh = static_cast<f32>(h.height);
                h.cam_zoom  = z;
                h.cam_pan_x = w * 0.5f - cx * z;   // screen=(world-cam)*z+(w/2) と一致 (DrawScene の screen=world*z+pan)
                h.cam_pan_y = hh * 0.5f - cy * z;
            }
        }
    }
}

/**
 * インプロセス Play を開始する。ゲーム DLL をロードし、エディタのノード木 + authored 値から
 * «DLL 側で» 実シーン (real components) を構築する。以降 render 内で tick され、ユーザー
 * コンポーネントの OnUpdate が走る (factory/所有/破棄は全て DLL ローカル = cross-DLL 安全)。
 * @return 1 成功 / -1 LoadLibrary 失敗 / -2 シンボル欠如 / -3 シーン生成失敗。
 */
ACS_EDITOR_API int acs_editor_logic_play_start(void* handle, const char* dll_path) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || dll_path == nullptr) return 0;
    if (host->logic_play) acs_editor_logic_play_stop(handle);

    void* dll = LoadLibraryA(dll_path);
    if (dll == nullptr) return -1;
    FGameShim sh;
    sh.create        = reinterpret_cast<decltype(sh.create)>(GetProcAddress(dll, "acs_game_scene_create"));
    sh.add_node      = reinterpret_cast<decltype(sh.add_node)>(GetProcAddress(dll, "acs_game_scene_add_node"));
    sh.set_transform = reinterpret_cast<decltype(sh.set_transform)>(GetProcAddress(dll, "acs_game_scene_set_transform"));
    sh.add_component = reinterpret_cast<decltype(sh.add_component)>(GetProcAddress(dll, "acs_game_scene_add_component"));
    sh.set_prop      = reinterpret_cast<decltype(sh.set_prop)>(GetProcAddress(dll, "acs_game_scene_set_prop"));
    sh.tick          = reinterpret_cast<decltype(sh.tick)>(GetProcAddress(dll, "acs_game_scene_tick"));
    sh.get_transform = reinterpret_cast<decltype(sh.get_transform)>(GetProcAddress(dll, "acs_game_scene_get_transform"));
    sh.destroy       = reinterpret_cast<decltype(sh.destroy)>(GetProcAddress(dll, "acs_game_scene_destroy"));
    sh.draw          = reinterpret_cast<decltype(sh.draw)>(GetProcAddress(dll, "acs_game_scene_draw"));  // 任意
    sh.input_key     = reinterpret_cast<decltype(sh.input_key)>(GetProcAddress(dll, "acs_game_input_key"));        // 任意
    sh.input_update  = reinterpret_cast<decltype(sh.input_update)>(GetProcAddress(dll, "acs_game_input_update"));  // 任意
    sh.input_mouse_button = reinterpret_cast<decltype(sh.input_mouse_button)>(GetProcAddress(dll, "acs_game_input_mouse_button"));  // 任意
    sh.input_mouse_move   = reinterpret_cast<decltype(sh.input_mouse_move)>(GetProcAddress(dll, "acs_game_input_mouse_move"));      // 任意
    sh.set_camera    = reinterpret_cast<decltype(sh.set_camera)>(GetProcAddress(dll, "acs_game_scene_set_camera"));  // 任意
    sh.get_camera    = reinterpret_cast<decltype(sh.get_camera)>(GetProcAddress(dll, "acs_game_scene_get_camera"));  // 任意
    sh.set_parent    = reinterpret_cast<decltype(sh.set_parent)>(GetProcAddress(dll, "acs_game_scene_set_parent"));  // 任意
    auto init = reinterpret_cast<void(*)()>(GetProcAddress(dll, "acs_game_reflect_init"));
    if (!sh.Valid()) { FreeLibrary(dll); return -2; }
    if (init != nullptr) init();

    void* scene = sh.create();
    if (scene == nullptr) { FreeLibrary(dll); return -3; }

    // dll idx == host->nodes index になるよう、まず全ノードを «root 直下に平坦» 生成し
    // (transform + components 込み)、その後 set_parent で正しい親へ付け替える。これで
    // host->nodes が «親より先に子» の順 (reparent 後にあり得る) でも親子構造が崩れない。
    host->logic_saved.Clear();
    for (u32 i = 0; i < host->nodes.Size(); ++i) {
        FEditorNode* n = host->nodes[i];
        host->logic_saved.PushBack(n->Local());          // 復元用に退避
        // 旧 DLL (set_parent 無し) 互換: best-effort で親-先行ケースだけ即配置。
        int parentIdx = -1;
        if (sh.set_parent == nullptr) {
            const int pid = ParentIdOf(*host, n);
            if (pid >= 0)
                for (u32 k = 0; k < i; ++k) if (host->nodes[k]->editor_id == pid) { parentIdx = static_cast<int>(k); break; }
        }
        const int idx = sh.add_node(scene, parentIdx);
        const game::FTransform2D& t = n->Local();
        sh.set_transform(scene, idx, t.position.x, t.position.y, t.rotation, t.scale.x, t.scale.y);
        for (u32 s = 0; s < n->component_count; ++s) {
            const game::FTypeDesc* d = game::FTypeRegistry::Get().FindById(n->components[s]);
            if (d == nullptr || d->name == nullptr) continue;
            const int dslot = sh.add_component(scene, idx, d->name);
            if (dslot < 0) continue;                       // DLL の registry に無い型はスキップ
            for (u32 j = 0; j < d->field_count && j < FEditorNode::kMaxProps; ++j) {
                if (d->fields[j].name == nullptr) continue;
                const f32* v = n->comp_props[s][j];
                sh.set_prop(scene, idx, dslot, d->fields[j].name, v[0], v[1], v[2], v[3]);
            }
        }
    }
    // 2nd pass: 親子を «順序非依存» に確定する (全ノードを検索して親 index を解決)。
    if (sh.set_parent != nullptr) {
        for (u32 i = 0; i < host->nodes.Size(); ++i) {
            const int pid = ParentIdOf(*host, host->nodes[i]);
            if (pid < 0) continue;                          // root 直下はそのまま
            int parentIdx = -1;
            for (u32 k = 0; k < host->nodes.Size(); ++k)
                if (host->nodes[k]->editor_id == pid) { parentIdx = static_cast<int>(k); break; }
            if (parentIdx >= 0) sh.set_parent(scene, static_cast<int>(i), parentIdx);
        }
    }
    // editor カメラを退避し、play camera を現在の editor view へ合わせる (= 追従の基準点)。
    host->logic_cam_pan_x = host->cam_pan_x;
    host->logic_cam_pan_y = host->cam_pan_y;
    host->logic_cam_zoom  = host->cam_zoom;
    host->logic_cam_following = false;
    if (sh.set_camera != nullptr && host->width > 0 && host->height > 0) {
        const f32 z = (host->cam_zoom > 0.0001f) ? host->cam_zoom : 1.0f;
        const f32 w = static_cast<f32>(host->width), h = static_cast<f32>(host->height);
        const f32 vcx = (w * 0.5f - host->cam_pan_x) / z;
        const f32 vcy = (h * 0.5f - host->cam_pan_y) / z;
        sh.set_camera(scene, vcx, vcy, z);
        host->logic_game_cam0_x = vcx; host->logic_game_cam0_y = vcy; host->logic_game_cam0_zoom = z;
    }

    host->logic_dll = dll; host->logic_scene = scene; host->logic_shim = sh; host->logic_play = true;
    return 1;
}

/** インプロセス Play を停止する。DLL シーンを破棄し、ノード transform を開始時へ復元、DLL を解放。 */
ACS_EDITOR_API void acs_editor_logic_play_stop(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || !host->logic_play) return;
    if (host->logic_scene != nullptr && host->logic_shim.destroy != nullptr)
        host->logic_shim.destroy(host->logic_scene);
    for (u32 i = 0; i < host->nodes.Size() && i < host->logic_saved.Size(); ++i)
        host->nodes[i]->Local() = host->logic_saved[i];   // 編集状態へ復元
    // editor カメラを Play 開始時の view へ復元する。
    host->cam_pan_x = host->logic_cam_pan_x;
    host->cam_pan_y = host->logic_cam_pan_y;
    host->cam_zoom  = host->logic_cam_zoom;
    if (host->logic_dll != nullptr) FreeLibrary(host->logic_dll);
    host->logic_dll = nullptr; host->logic_scene = nullptr; host->logic_play = false;
    host->logic_saved.Clear();
}

/** インプロセス Play 中か (1/0)。 */
ACS_EDITOR_API int acs_editor_logic_play_active(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    return (host != nullptr && host->logic_play) ? 1 : 0;
}

/**
 * Play 中の DLL へキー入力をフィードする (WPF の KeyDown/KeyUp から呼ぶ)。
 *
 * @details
 * ユーザーコンポーネントの OnUpdate が Services().Input() で読む値は «reflect DLL の» acs::Input を
 * poll するため、DLL 内の OnEvent (acs_game_input_key) へ転送する必要がある。Play 中でなければ no-op。
 * @param handle エディタハンドル。
 * @param keycode acs::EKey の整数値 (C# 側が WPF Key からマップ)。
 * @param down 1=押下, 0=解放。
 */
ACS_EDITOR_API void acs_editor_logic_input_key(void* handle, int keycode, int down) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || !host->logic_play || host->logic_shim.input_key == nullptr) return;
    host->logic_shim.input_key(keycode, down);
}

/** Play 中の DLL へマウスボタン入力をフィードする (button: 0=Left,1=Right,2=Middle)。 */
ACS_EDITOR_API void acs_editor_logic_input_mouse_button(void* handle, int button, int down) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || !host->logic_play || host->logic_shim.input_mouse_button == nullptr) return;
    host->logic_shim.input_mouse_button(button, down);
}

/** Play 中の DLL へマウス位置 (viewport クライアント px) をフィードする。 */
ACS_EDITOR_API void acs_editor_logic_input_mouse_move(void* handle, float x, float y) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || !host->logic_play || host->logic_shim.input_mouse_move == nullptr) return;
    host->logic_shim.input_mouse_move(x, y);
}

/**
 * ゲームビュー (Game View タブ) の表示を切り替える。
 *
 * @details on=1 で editor chrome (グリッド/軸/リンク/選択/ギズモ/空ノードギズモ・非可視ノード) を
 * 描かず、ノードの見た目 + Play の OnDraw だけを描く「ゲーム画面」になる。on=0 で通常の編集ビュー。
 * @param handle エディタハンドル。
 * @param on 1=ゲームビュー, 0=シーン(編集)ビュー。
 */
ACS_EDITOR_API void acs_editor_set_game_view(void* handle, int on) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host != nullptr) host->game_view = (on != 0);
}

/** 現在ゲームビューか (1/0)。 */
ACS_EDITOR_API int acs_editor_is_game_view(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    return (host != nullptr && host->game_view) ? 1 : 0;
}

/** カテゴリ整数値の人間可読ラベル ("Component" 等、不正値は "Unknown")。 */
ACS_EDITOR_API const char* acs_editor_category_label(int category) {
    return CategoryLabel(static_cast<acs::game::ETypeCategory>(category));
}

/**
 * 登録型名でシーンにノードを 1 つ追加する (レジストリ駆動のインスタンス化デモ)。
 *
 * @details
 * エディタが「エンジンの登録型」を選んで実 FNode2D ツリーへ実体化する経路。ここでは
 * 型名をラベルにした FEditorNode を生成する (将来 factory による本物のコンポーネント
 * attach に拡張可能)。
 * @return 追加したノードの editor_id、不正引数は -1。
 */
ACS_EDITOR_API int acs_editor_add_node(void* handle, const char* type_name, int parent_id) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || type_name == nullptr || host->root.Get() == nullptr) return -1;
    PushUndo(*host);

    // 親があればその近く、無ければビューポート中央付近に置く。
    const bool has_parent = (FindNode(*host, parent_id) != nullptr);
    const f32  px = has_parent ? 64.0f : static_cast<f32>(host->width)  * 0.5f;
    const f32  py = has_parent ? 0.0f  : static_cast<f32>(host->height) * 0.5f;

    FEditorNode* n = AddEditorNode(*host, has_parent ? parent_id : -1, type_name,
                                   px, py, 0.0f, 40.0f, FVec4{ 0.55f, 0.70f, 0.55f, 1.0f });
    SelSet(*host, n->editor_id);
    return n->editor_id;
}

// =============================================================================
// C ABI — シーンの保存 / 読込 (永続化)
// -----------------------------------------------------------------------------
// ABI はシリアライズ/デシリアライズ (文字列 ⇄ 実 FNode2D ツリー) を担い、ファイル I/O と
// ダイアログ・パス処理は C# (WPF) 側が担当する (パスのエンコーディング問題を回避)。
// =============================================================================

/** 現在のシーンをテキストへシリアライズして返す (内部バッファへのポインタ)。 */
ACS_EDITOR_API const char* acs_editor_scene_serialize(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    return (host != nullptr) ? SerializeScene(*host) : "";
}

/** シリアライズ済みテキストからシーンを復元する (成功 1 / 失敗 0)。 */
ACS_EDITOR_API int acs_editor_scene_load_text(void* handle, const char* text) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return 0;
    PushUndo(*host);
    return LoadSceneText(*host, text);
}

/** シーンを空 (隠しルートのみ) に戻す (New Scene)。 */
ACS_EDITOR_API void acs_editor_scene_new(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return;
    PushUndo(*host);
    ClearScene(*host);
}

// =============================================================================
// C ABI — コピー / ペースト (サブツリーのシリアライズ。クリップボードは C# 側が保持)
// =============================================================================

/** id のノードの subtree をシリアライズして返す (C# がクリップボードに保持)。 */
ACS_EDITOR_API const char* acs_editor_copy_subtree(void* handle, int id) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return "";
    FEditorNode* n = FindNode(*host, id);
    return (n != nullptr) ? SerializeSubtree(*host, n) : "";
}

/** subtree テキストを parent_id 配下へ貼り付ける (新規 id、内部親子は再マップ)。新根 id / 失敗 -1。 */
ACS_EDITOR_API int acs_editor_paste_subtree(void* handle, const char* text, int parent_id) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || text == nullptr) return -1;
    PushUndo(*host);
    return PasteSubtree(*host, text, parent_id);
}

// =============================================================================
// C ABI — Undo / Redo (シーンスナップショット)
// -----------------------------------------------------------------------------
// 各変更操作は実行直前に PushUndo で現在状態を積む。undo は「現在→redo に退避して
// undo から復元」、redo はその逆。復元は LoadSceneText (内部) で行い、スタックは触らない。
// =============================================================================

/** 直前の変更を取り消す (成功 1 / 何もなければ 0)。 */
ACS_EDITOR_API int acs_editor_undo(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || host->undo.Size() == 0) return 0;
    char* cur = DupSnapshot(*host);                       // 現在状態を redo に退避
    if (cur != nullptr) host->redo.PushBack(cur);
    char* prev = host->undo[host->undo.Size() - 1];
    host->undo.PopBack();
    LoadSceneText(*host, prev);                           // 復元 (スタックは不変)
    delete[] prev;
    return 1;
}

/** 取り消した変更をやり直す (成功 1 / 何もなければ 0)。 */
ACS_EDITOR_API int acs_editor_redo(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || host->redo.Size() == 0) return 0;
    char* cur = DupSnapshot(*host);                       // 現在状態を undo に積む
    if (cur != nullptr) host->undo.PushBack(cur);
    char* next = host->redo[host->redo.Size() - 1];
    host->redo.PopBack();
    LoadSceneText(*host, next);
    delete[] next;
    return 1;
}

/** undo 可能か。 */
ACS_EDITOR_API int acs_editor_can_undo(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    return (host != nullptr && host->undo.Size() > 0) ? 1 : 0;
}

/** redo 可能か。 */
ACS_EDITOR_API int acs_editor_can_redo(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    return (host != nullptr && host->redo.Size() > 0) ? 1 : 0;
}

/** 連続編集 (ドラッグスクラブ等) を開始する。開始時点の状態を 1 度だけ undo に積み、
 *  以降の set_* が積む undo を end まで抑止する → ドラッグ全体が undo 1 ステップになる。 */
ACS_EDITOR_API void acs_editor_begin_continuous(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || host->suppress_undo) return;
    PushUndo(*host);                 // 開始状態の 1 スナップショット
    host->suppress_undo = true;
}

/** 連続編集を終了する (以降の set_* は通常どおり undo を積む)。 */
ACS_EDITOR_API void acs_editor_end_continuous(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host != nullptr) host->suppress_undo = false;
}

// =============================================================================
// C ABI — ビューカメラ (パン / ズーム) + ピッキング
// -----------------------------------------------------------------------------
// screen = world * zoom + pan。座標は描画と同じスクリーンピクセル。マウス入力は WPF が
// HWND の WndProc から拾い、これらを呼ぶ (ピックは editor_id を返す)。
// =============================================================================

/** スクリーン座標でノードをピック (上のものを優先、無ければ -1)。 */
ACS_EDITOR_API int acs_editor_pick(void* handle, float screen_x, float screen_y) {
    auto* host = static_cast<EditorHost*>(handle);
    return (host != nullptr) ? PickNode(*host, screen_x, screen_y) : -1;
}

/** カメラをスクリーン量だけ平行移動する (ドラッグパン)。 */
ACS_EDITOR_API void acs_editor_camera_pan(void* handle, float dx, float dy) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host != nullptr) { host->cam_pan_x += dx; host->cam_pan_y += dy; }
}

/** アンカー (スクリーン点) を固定して factor 倍ズームする (ホイール)。 */
ACS_EDITOR_API void acs_editor_camera_zoom(void* handle, float factor, float anchor_x, float anchor_y) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || factor <= 0.0f) return;
    const f32 z0 = host->cam_zoom;
    f32 z1 = z0 * factor;
    if (z1 < 0.05f) z1 = 0.05f;          // クランプ (極端なズームを防ぐ)
    if (z1 > 20.0f) z1 = 20.0f;
    // アンカー直下の world 点を固定: world = (anchor - pan) / z、pan' = anchor - world * z'
    const f32 wx = (anchor_x - host->cam_pan_x) / z0;
    const f32 wy = (anchor_y - host->cam_pan_y) / z0;
    host->cam_zoom  = z1;
    host->cam_pan_x = anchor_x - wx * z1;
    host->cam_pan_y = anchor_y - wy * z1;
}

/** カメラを初期状態 (pan 0 / zoom 1) に戻す。 */
ACS_EDITOR_API void acs_editor_camera_reset(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host != nullptr) { host->cam_pan_x = 0.0f; host->cam_pan_y = 0.0f; host->cam_zoom = 1.0f; }
}

/** 現在のカメラ状態を取得する (UI 表示 / 検証用)。 */
ACS_EDITOR_API void acs_editor_camera_get(void* handle, float* pan_x, float* pan_y, float* zoom) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return;
    if (pan_x) *pan_x = host->cam_pan_x;
    if (pan_y) *pan_y = host->cam_pan_y;
    if (zoom)  *zoom  = host->cam_zoom;
}

// =============================================================================
// C ABI — 移動ギズモ (選択ノードをドラッグで移動)
// -----------------------------------------------------------------------------
// 左クリック時にまず gizmo_begin を試し、ハンドルを掴めたら (戻り値 != 0) 以降の
// マウス移動を gizmo_update に流す。掴めなければ通常のピックに回す。ドラッグ全体で
// 1 つの undo を作る (begin で PushUndo)。
// =============================================================================

/** ギズモモードを設定する (0=move, 1=rotate, 2=scale)。 */
ACS_EDITOR_API void acs_editor_gizmo_set_mode(void* handle, int mode) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host != nullptr && mode >= 0 && mode <= 2) host->gizmo_mode = mode;
}

/** 現在のギズモモード。 */
ACS_EDITOR_API int acs_editor_gizmo_get_mode(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    return (host != nullptr) ? host->gizmo_mode : 0;
}

/** スクリーン点でギズモハンドルを掴む (掴めた handle 種別 1/2/3、掴めなければ 0)。 */
ACS_EDITOR_API int acs_editor_gizmo_begin(void* handle, float sx, float sy) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return 0;
    const int axis = GizmoHit(*host, sx, sy);
    if (axis == 0) return 0;
    FEditorNode* n = FindNode(*host, host->selected);
    if (n == nullptr) return 0;
    // undo は最初の実移動 (gizmo_update) まで遅延する。掴んだだけ (無移動) で離した
    // ときに空の undo を積まない & redo を消さないため。
    host->gizmo_pushed = false;
    host->gizmo_active = true;
    host->gizmo_axis   = axis;
    host->gizmo_move_ids.Clear();          // 前回 drag の残骸を一掃 (rotate/scale では空のまま)
    host->gizmo_move_bx.Clear();
    host->gizmo_move_by.Clear();

    const game::FTransform2D  w   = n->World();
    const game::FTransform2D& loc = n->Local();
    f32 cx, cy; GizmoCenter(*host, cx, cy);

    if (host->gizmo_mode == 1) {            // rotate
        host->gizmo_start_rot   = loc.rotation;
        host->gizmo_start_angle = std::atan2(sy - cy, sx - cx);
    } else if (host->gizmo_mode == 2) {     // scale
        host->gizmo_start_sx = loc.scale.x;
        host->gizmo_start_sy = loc.scale.y;
        f32 m;
        if (axis == 1)      m = sx - cx;                                       // X offset
        else if (axis == 2) m = cy - sy;                                       // Y offset (上が正)
        else                m = std::sqrt((sx - cx) * (sx - cx) + (sy - cy) * (sy - cy)); // uniform dist
        if (std::fabs(m) < 4.0f) m = (m < 0.0f) ? -4.0f : 4.0f;                // 0 除算ガード
        host->gizmo_start_metric = m;
    } else {                                 // move
        host->gizmo_begin_wx = w.position.x;
        host->gizmo_begin_wy = w.position.y;
        host->gizmo_off_wx   = S2WX(*host, sx) - w.position.x;
        host->gizmo_off_wy   = S2WY(*host, sy) - w.position.y;
        // 複数選択の移動: 動かす「選択ルート」(祖先が選択外のもの) を集め、開始 world 位置を退避。
        // 子孫は祖先の移動に従うので含めない (= 二重移動を順序非依存で回避)。
        for (u32 i = 0; i < host->selection.Size(); ++i) {
            FEditorNode* sn = FindNode(*host, host->selection[i]);
            if (sn == nullptr || AnyAncestorInList(*host, sn, host->selection)) continue;
            const game::FTransform2D sw = sn->World();
            host->gizmo_move_ids.PushBack(sn->editor_id);
            host->gizmo_move_bx.PushBack(sw.position.x);
            host->gizmo_move_by.PushBack(sw.position.y);
        }
    }
    return axis;
}

/** ドラッグ中の操作を選択ノードへ適用する (モード別: 移動/回転/スケール)。 */
ACS_EDITOR_API void acs_editor_gizmo_update(void* handle, float sx, float sy) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || !host->gizmo_active) return;
    FEditorNode* n = FindNode(*host, host->selected);
    if (n == nullptr) { host->gizmo_active = false; return; }
    if (!host->gizmo_pushed) { PushUndo(*host); host->gizmo_pushed = true; }   // 最初の移動で 1 undo
    f32 cx, cy; GizmoCenter(*host, cx, cy);

    if (host->gizmo_mode == 1) {            // rotate: カーソル角の差を local rotation へ
        const f32 cur = std::atan2(sy - cy, sx - cx);
        f32 rot = host->gizmo_start_rot + (cur - host->gizmo_start_angle);
        if (host->snap_enabled) rot = SnapTo(rot, host->snap_rotate);
        n->Local().rotation = rot;
    } else if (host->gizmo_mode == 2) {     // scale: 基準量に対する比率
        f32 cur;
        if (host->gizmo_axis == 1)      cur = sx - cx;
        else if (host->gizmo_axis == 2) cur = cy - sy;
        else                            cur = std::sqrt((sx - cx) * (sx - cx) + (sy - cy) * (sy - cy));
        const f32 r = cur / host->gizmo_start_metric;
        f32 nsx = host->gizmo_start_sx * r;
        f32 nsy = host->gizmo_start_sy * r;
        if (host->snap_enabled) {
            const f32 st = host->snap_scale;
            nsx = SnapTo(nsx, st); if (nsx < st) nsx = st;   // 0 に潰さない
            nsy = SnapTo(nsy, st); if (nsy < st) nsy = st;
        }
        if (host->gizmo_axis == 1)      n->Local().scale.x = nsx;
        else if (host->gizmo_axis == 2) n->Local().scale.y = nsy;
        else                            n->Local().scale = FVec2{ nsx, nsy };
    } else {                                 // move
        f32 tw_x = S2WX(*host, sx) - host->gizmo_off_wx;
        f32 tw_y = S2WY(*host, sy) - host->gizmo_off_wy;
        if (host->gizmo_axis == 1) tw_y = host->gizmo_begin_wy;   // X 軸のみ
        if (host->gizmo_axis == 2) tw_x = host->gizmo_begin_wx;   // Y 軸のみ
        if (host->snap_enabled) {
            if (host->gizmo_axis != 2) tw_x = SnapTo(tw_x, host->snap_move);   // X or free
            if (host->gizmo_axis != 1) tw_y = SnapTo(tw_y, host->snap_move);   // Y or free
        }
        if (host->gizmo_move_ids.Size() > 0) {
            // primary の移動量 delta を、退避した各選択ルートの開始位置に絶対適用する
            // (begin+delta の絶対指定なので順序非依存。子孫は祖先に従って動く)。
            const f32 dx = tw_x - host->gizmo_begin_wx;
            const f32 dy = tw_y - host->gizmo_begin_wy;
            for (u32 i = 0; i < host->gizmo_move_ids.Size(); ++i) {
                FEditorNode* mn = FindNode(*host, host->gizmo_move_ids[i]);
                if (mn != nullptr)
                    SetNodeWorldPosition(*host, mn, host->gizmo_move_bx[i] + dx, host->gizmo_move_by[i] + dy);
            }
        } else {
            SetNodeWorldPosition(*host, n, tw_x, tw_y);   // 単一選択フォールバック
        }
    }
}

/** ドラッグを終える。 */
ACS_EDITOR_API void acs_editor_gizmo_end(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host != nullptr) host->gizmo_active = false;
}

/** スナップ設定を更新する (enabled、移動グリッド、回転刻み[度]、スケール刻み。<=0 は据え置き)。 */
ACS_EDITOR_API void acs_editor_set_snap(void* handle, int enabled, float move_grid, float rotate_deg, float scale_step) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return;
    host->snap_enabled = (enabled != 0);
    if (move_grid  > 0.0f) host->snap_move   = move_grid;
    if (rotate_deg > 0.0f) host->snap_rotate = rotate_deg * (3.14159265f / 180.0f);
    if (scale_step > 0.0f) host->snap_scale  = scale_step;
}

/** スナップが有効か。 */
ACS_EDITOR_API int acs_editor_get_snap(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    return (host != nullptr && host->snap_enabled) ? 1 : 0;
}

/** 選択ノードがビューポート中央に来るようカメラを寄せる (ズームは維持)。 */
ACS_EDITOR_API void acs_editor_camera_focus(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || host->selected < 0) return;
    FEditorNode* n = FindNode(*host, host->selected);
    if (n == nullptr) return;
    const game::FTransform2D w = n->World();
    host->cam_pan_x = static_cast<f32>(host->width)  * 0.5f - w.position.x * host->cam_zoom;
    host->cam_pan_y = static_cast<f32>(host->height) * 0.5f - w.position.y * host->cam_zoom;
}

/** 全ノードがビューポートに収まるよう pan/zoom を合わせる (シーン読込直後のフレーミング)。 */
ACS_EDITOR_API void acs_editor_camera_frame_all(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || host->nodes.Size() == 0) return;
    f32 minx = 3.4e38f, miny = 3.4e38f, maxx = -3.4e38f, maxy = -3.4e38f;
    for (u32 i = 0; i < host->nodes.Size(); ++i) {
        const FEditorNode* n = host->nodes[i];
        const game::FTransform2D w = n->World();
        const f32 sx = (w.scale.x < 0.0f) ? -w.scale.x : w.scale.x;
        const f32 sy = (w.scale.y < 0.0f) ? -w.scale.y : w.scale.y;
        f32 ex = 0.5f * n->base * sx; if (ex < 1.0f) ex = 1.0f;   // ノードの半径目安
        f32 ey = 0.5f * n->base * sy; if (ey < 1.0f) ey = 1.0f;
        if (w.position.x - ex < minx) minx = w.position.x - ex;
        if (w.position.y - ey < miny) miny = w.position.y - ey;
        if (w.position.x + ex > maxx) maxx = w.position.x + ex;
        if (w.position.y + ey > maxy) maxy = w.position.y + ey;
    }
    const f32 cx = (minx + maxx) * 0.5f, cy = (miny + maxy) * 0.5f;
    const f32 bw = maxx - minx, bh = maxy - miny;
    const f32 vw = static_cast<f32>(host->width), vh = static_cast<f32>(host->height);
    f32 zoom = host->cam_zoom;
    if (bw > 1.0f && bh > 1.0f && vw > 1.0f && vh > 1.0f) {
        const f32 zx = vw * 0.85f / bw, zy = vh * 0.85f / bh;   // 0.85 = 余白
        zoom = (zx < zy) ? zx : zy;
        if (zoom > 4.0f)  zoom = 4.0f;
        if (zoom < 0.05f) zoom = 0.05f;
    }
    host->cam_zoom  = zoom;
    host->cam_pan_x = vw * 0.5f - cx * zoom;
    host->cam_pan_y = vh * 0.5f - cy * zoom;
}

// =============================================================================
// C ABI — ノード操作 (リネーム / 削除 / 親付け替え)
// -----------------------------------------------------------------------------
// 階層を実 FNode2D の構造変更 API (Destroy / Reparent + ResolveStructuralChanges) で
// 編集する。削除/付け替えの後は平坦レジストリをツリーから作り直して整合を保つ。
// =============================================================================

/** ノードを改名する (成功 1 / 不明 0)。 */
ACS_EDITOR_API int acs_editor_node_rename(void* handle, int id, const char* name) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || name == nullptr) return 0;
    FEditorNode* n = FindNode(*host, id);
    if (n == nullptr) return 0;
    PushUndo(*host);
    std::snprintf(n->name, sizeof(n->name), "%s", name);
    return 1;
}

/** ノードを subtree ごと複製し、元の兄弟として追加する (新しい根の id、不明は -1)。 */
ACS_EDITOR_API int acs_editor_node_duplicate(void* handle, int id) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return -1;
    FEditorNode* src = FindNode(*host, id);
    if (src == nullptr) return -1;
    PushUndo(*host);
    const int parent_id = ParentIdOf(*host, src);   // 元と同じ親 = 兄弟として複製
    FEditorNode* clone = CloneSubtree(*host, src, parent_id, /*top=*/true);
    SelSet(*host, clone->editor_id);
    return clone->editor_id;
}

/** ノード (と subtree) を削除する (成功 1 / 不明 0)。 */
ACS_EDITOR_API int acs_editor_node_delete(void* handle, int id) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || host->root.Get() == nullptr) return 0;
    FEditorNode* n = FindNode(*host, id);
    if (n == nullptr) return 0;
    PushUndo(*host);
    n->Destroy();                              // 遅延破棄をマーク
    host->root->ResolveStructuralChanges();    // subtree ごと reap
    RebuildRegistry(*host);                    // dangling を一掃 + 選択解除
    return 1;
}

/**
 * ノードの親を付け替える (new_parent_id < 0 でルート直下へ)。
 *
 * @details cycle (自分 or 子孫を親に指定) は FNode2D 側で弾かれ無視される。
 * @return 成功 1、不明ノード 0。
 */
ACS_EDITOR_API int acs_editor_node_reparent(void* handle, int id, int new_parent_id) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || host->root.Get() == nullptr) return 0;
    if (new_parent_id == id) return 0;         // 自分自身を親にはできない
    FEditorNode* n = FindNode(*host, id);
    if (n == nullptr) return 0;
    game::FNode2D* np = (new_parent_id >= 0) ? static_cast<game::FNode2D*>(FindNode(*host, new_parent_id))
                                             : host->root.Get();
    if (np == nullptr) np = host->root.Get();
    // no-op: 既に np が現在の親なら何もしない (無駄な undo / 再構築を積まない)。
    if (n->Parent() == np) return 0;
    // cycle 検出: np が n の子孫なら付け替え不可 (engine 側も無視するが、ここで弾いて
    //             spurious undo を防ぐ)。np から親をたどって n に達したら循環。
    for (game::FNode2D* a = np; a != nullptr; a = a->Parent())
        if (a == static_cast<game::FNode2D*>(n)) return 0;
    // ワールド位置を保持して付け替える (ノードが視覚的に飛ばないように)。
    const FVec2 wpos = n->World().position;
    PushUndo(*host);
    n->Reparent(*np);                          // 付け替え予約 (cycle は上で除外済み)
    host->root->ResolveStructuralChanges();    // フレーム境界処理を即時に適用
    RebuildRegistry(*host);                    // 親が子より先になるよう順序を作り直す
    SetNodeWorldPosition(*host, n, wpos.x, wpos.y);   // 新しい親基準で local を再計算しワールド位置を維持
    return 1;
}

/**
 * ノードを兄弟として target の前/後ろへ挿入、または target の子にする (ヒエラルキー D&D)。
 *
 * @param mode 0=target の直前 (兄弟), 1=target の直後 (兄弟), 2=target の子。
 * @return 成功 1、不正/不明 0。
 */
ACS_EDITOR_API int acs_editor_node_move(void* handle, int id, int target_id, int mode) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || host->root.Get() == nullptr) return 0;
    if (id == target_id) return 0;
    if (mode == 2) return acs_editor_node_reparent(handle, id, target_id);   // 子にする = 既存 reparent

    FEditorNode* n = FindNode(*host, id);
    FEditorNode* t = FindNode(*host, target_id);
    if (n == nullptr || t == nullptr) return 0;

    // target の親 P (ルート直下なら root)。n を P の子として並べ替える。
    game::FNode2D* P = t->Parent() ? t->Parent() : host->root.Get();
    // cycle: P (= 挿入先) が n 自身 or その子孫なら不可。
    for (game::FNode2D* a = P; a != nullptr; a = a->Parent())
        if (a == static_cast<game::FNode2D*>(n)) return 0;

    const FVec2 wpos = n->World().position;
    PushUndo(*host);
    if (n->Parent() != P) {                       // まず同じ親へ寄せる (末尾に付く)
        n->Reparent(*P);
        host->root->ResolveStructuralChanges();
    }
    // P の子配列内で n と t のインデックスを取り、mode に応じた最終位置へ動かす。
    u32 c = P->ChildCount(), b = P->ChildCount();
    for (u32 i = 0; i < P->ChildCount(); ++i) {
        game::FNode2D* ch = P->Child(i);
        if (ch == static_cast<game::FNode2D*>(n)) c = i;
        if (ch == static_cast<game::FNode2D*>(t)) b = i;
    }
    if (c < P->ChildCount() && b < P->ChildCount()) {
        u32 to;
        if (mode == 0) to = (c < b) ? (b - 1) : b;        // before
        else           to = (c < b) ? b : (b + 1);        // after
        P->MoveChild(*n, to);
    }
    RebuildRegistry(*host);                       // 兄弟順を平坦レジストリへ反映
    SetNodeWorldPosition(*host, n, wpos.x, wpos.y);
    return 1;
}

/** 選択集合のノードをまとめて削除する (1 undo step。削除数を返す、空/不正は 0)。 */
ACS_EDITOR_API int acs_editor_selection_delete(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || host->root.Get() == nullptr || host->selection.Size() == 0) return 0;
    PushUndo(*host);                           // 一括で 1 undo
    int n_deleted = 0;
    // selection は Destroy では変化しない (RebuildRegistry まで不変) ので直接走査してよい。
    // 祖先と子孫が両方選択されていても Destroy は冪等マークなので二重解放にならない。
    for (u32 i = 0; i < host->selection.Size(); ++i) {
        FEditorNode* node = FindNode(*host, host->selection[i]);
        if (node != nullptr) { node->Destroy(); ++n_deleted; }
    }
    host->root->ResolveStructuralChanges();    // 全 subtree を一括 reap
    RebuildRegistry(*host);                    // SelPrune が消えた選択を一掃 (集合は空に)
    return n_deleted;
}

/** 選択集合のノードをまとめて複製する (1 undo step。複製した根の数を返す、空は 0)。 */
ACS_EDITOR_API int acs_editor_selection_duplicate(void* handle) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr || host->selection.Size() == 0) return 0;
    PushUndo(*host);                           // 一括で 1 undo
    // 複製中に selection を書き換えるので、対象 id を先にコピーしておく。
    TArray<int> sources = host->selection.Clone();
    TArray<int> newIds;
    for (u32 i = 0; i < sources.Size(); ++i) {
        FEditorNode* src = FindNode(*host, sources[i]);
        if (src == nullptr) continue;
        // 祖先も選択集合にあるノードは、その祖先の複製に subtree として含まれる → 単独複製しない
        // (二重複製を防ぐ)。
        if (AnyAncestorInList(*host, src, sources)) continue;
        FEditorNode* clone = CloneSubtree(*host, src, ParentIdOf(*host, src), /*top=*/true);
        newIds.PushBack(clone->editor_id);
    }
    // 新しい複製群を選択する (primary = 最後の複製)。
    host->selection.Clear();
    for (u32 i = 0; i < newIds.Size(); ++i) host->selection.PushBack(newIds[i]);
    host->selected = (host->selection.Size() > 0) ? host->selection[host->selection.Size() - 1] : -1;
    return static_cast<int>(newIds.Size());
}

/**
 * 選択ノードを world 位置で整列する (1 undo step。2 ノード以上で有効)。
 *
 * @param mode 0=左(min x) / 1=右(max x) / 2=上(min y) / 3=下(max y) / 4=水平中央(mid x) / 5=垂直中央(mid y)。
 * @return 整列したノード数 (2 未満は 0)。
 */
ACS_EDITOR_API int acs_editor_align_selection(void* handle, int mode) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return 0;

    // 選択ノードの world 位置の範囲を求める。
    f32 minx = 0, maxx = 0, miny = 0, maxy = 0;
    bool first = true; int cnt = 0;
    for (u32 i = 0; i < host->selection.Size(); ++i) {
        FEditorNode* n = FindNode(*host, host->selection[i]);
        if (n == nullptr) continue;
        const game::FTransform2D w = n->World();
        if (first) { minx = maxx = w.position.x; miny = maxy = w.position.y; first = false; }
        else {
            if (w.position.x < minx) minx = w.position.x;
            if (w.position.x > maxx) maxx = w.position.x;
            if (w.position.y < miny) miny = w.position.y;
            if (w.position.y > maxy) maxy = w.position.y;
        }
        ++cnt;
    }
    if (cnt < 2) return 0;

    PushUndo(*host);
    int applied = 0;
    for (u32 i = 0; i < host->selection.Size(); ++i) {
        FEditorNode* n = FindNode(*host, host->selection[i]);
        if (n == nullptr) continue;
        const game::FTransform2D w = n->World();
        f32 nx = w.position.x, ny = w.position.y;
        switch (mode) {
            case 0: nx = minx; break;
            case 1: nx = maxx; break;
            case 2: ny = miny; break;
            case 3: ny = maxy; break;
            case 4: nx = (minx + maxx) * 0.5f; break;
            case 5: ny = (miny + maxy) * 0.5f; break;
            default: break;
        }
        SetNodeWorldPosition(*host, n, nx, ny);
        ++applied;
    }
    return applied;
}

/**
 * 選択ノードを axis 方向に等間隔で配置する (両端を固定し中間を均等割り。3 ノード以上で有効)。
 *
 * @param axis 0=水平(x) / 1=垂直(y)。
 * @return 配置したノード数 (3 未満は 0)。
 */
ACS_EDITOR_API int acs_editor_distribute_selection(void* handle, int axis) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return 0;

    // (id, axis 位置) を集める。
    TArray<int> ids;
    TArray<f32> pos;
    for (u32 i = 0; i < host->selection.Size(); ++i) {
        FEditorNode* n = FindNode(*host, host->selection[i]);
        if (n == nullptr) continue;
        const game::FTransform2D w = n->World();
        ids.PushBack(host->selection[i]);
        pos.PushBack(axis == 0 ? w.position.x : w.position.y);
    }
    const u32 n = ids.Size();
    if (n < 3) return 0;

    // axis 位置で昇順ソート (挿入ソート、parallel)。
    for (u32 i = 1; i < n; ++i) {
        const f32 kp = pos[i]; const int ki = ids[i];
        u32 j = i;
        while (j > 0 && pos[j - 1] > kp) { pos[j] = pos[j - 1]; ids[j] = ids[j - 1]; --j; }
        pos[j] = kp; ids[j] = ki;
    }

    PushUndo(*host);
    const f32 step = (pos[n - 1] - pos[0]) / static_cast<f32>(n - 1);
    for (u32 i = 1; i + 1 < n; ++i) {   // 両端は固定、中間を均等配置
        FEditorNode* node = FindNode(*host, ids[i]);
        if (node == nullptr) continue;
        const game::FTransform2D w = node->World();
        const f32 target = pos[0] + step * static_cast<f32>(i);
        if (axis == 0) SetNodeWorldPosition(*host, node, target, w.position.y);
        else           SetNodeWorldPosition(*host, node, w.position.x, target);
    }
    return static_cast<int>(n);
}

// =============================================================================
// C ABI — ノードのコンポーネント (リフレクション登録 Component 型のアタッチ記述子)
// -----------------------------------------------------------------------------
// ノードに「どの Component 型が付くか」をエディタ・メタデータとして持たせる。実
// FComponent2D は生成せず、シリアライズして将来 play/load 時に実体化する土台とする。
// =============================================================================

/** ノードに Component 型をアタッチする (型名で。成功/既存 1、未登録や非 Component 0)。 */
ACS_EDITOR_API int acs_editor_node_add_component(void* handle, int id, const char* type_name) {
    auto* host = static_cast<EditorHost*>(handle);
    if (host == nullptr) return 0;
    FEditorNode* n = FindNode(*host, id);
    if (n == nullptr) return 0;
    PushUndo(*host);
    return AttachComponent(n, type_name) ? 1 : 0;
}

/** ノードのコンポーネント数。 */
ACS_EDITOR_API int acs_editor_node_component_count(void* handle, int id) {
    auto* host = static_cast<EditorHost*>(handle);
    const FEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    return (n != nullptr) ? static_cast<int>(n->component_count) : 0;
}

/** ノードの index 番目のコンポーネント型名 (範囲外は "")。 */
ACS_EDITOR_API const char* acs_editor_node_component_name_at(void* handle, int id, int index) {
    auto* host = static_cast<EditorHost*>(handle);
    const FEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr || index < 0 || index >= static_cast<int>(n->component_count)) return "";
    const game::FTypeDesc* d = game::FTypeRegistry::Get().FindById(n->components[static_cast<u32>(index)]);
    return (d != nullptr && d->name != nullptr) ? d->name : "";
}

/** ノードの index 番目のコンポーネントを外す (成功 1)。 */
ACS_EDITOR_API int acs_editor_node_remove_component_at(void* handle, int id, int index) {
    auto* host = static_cast<EditorHost*>(handle);
    FEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr || index < 0 || index >= static_cast<int>(n->component_count)) return 0;
    PushUndo(*host);
    for (u32 i = static_cast<u32>(index); i + 1 < n->component_count; ++i) {
        n->components[i] = n->components[i + 1];
        for (u32 p = 0; p < FEditorNode::kMaxProps; ++p)
            for (u32 k = 0; k < 4; ++k) n->comp_props[i][p][k] = n->comp_props[i + 1][p][k];
    }
    --n->component_count;
    return 1;
}

// =============================================================================
// C ABI — コンポーネントの編集プロパティ (リフレクション・スキーマ駆動)
// -----------------------------------------------------------------------------
// 「型のスキーマ (どの編集可能フィールドがあるか)」は reflection レジストリ (ACS_RPROP*)
// から得る。「インスタンスの値」はノードごと slot ごとにエディタ側 (comp_props) が持つ。
// Component は多態 + private メンバで offsetof 反射できないため、この 2 段構え
// (スキーマ=反射 / 値=エディタ保持) でプロパティ編集を実現する。
// =============================================================================

/** 型名で、その Component 型の編集プロパティ数を返す (未登録 / 非 Component は 0)。 */
ACS_EDITOR_API int acs_editor_component_prop_count(const char* type_name) {
    if (type_name == nullptr) return 0;
    game::AcsRegisterEngineTypes();
    const game::FTypeDesc* d = game::FTypeRegistry::Get().FindByName(type_name);
    return static_cast<int>(CompPropCount(d));
}

/** 型名と index で、編集プロパティ名を返す (範囲外は "")。 */
ACS_EDITOR_API const char* acs_editor_component_prop_name_at(const char* type_name, int index) {
    if (type_name == nullptr || index < 0) return "";
    game::AcsRegisterEngineTypes();
    const game::FTypeDesc* d = game::FTypeRegistry::Get().FindByName(type_name);
    if (d == nullptr || index >= static_cast<int>(CompPropCount(d))) return "";
    const char* nm = d->fields[static_cast<u32>(index)].name;
    return (nm != nullptr) ? nm : "";
}

/** 型名と index で、編集プロパティの種別 (EFieldKind の整数値) を返す (範囲外は -1)。 */
ACS_EDITOR_API int acs_editor_component_prop_kind_at(const char* type_name, int index) {
    if (type_name == nullptr || index < 0) return -1;
    game::AcsRegisterEngineTypes();
    const game::FTypeDesc* d = game::FTypeRegistry::Get().FindByName(type_name);
    if (d == nullptr || index >= static_cast<int>(CompPropCount(d))) return -1;
    return static_cast<int>(d->fields[static_cast<u32>(index)].kind);
}

/** ノードの slot 番コンポーネントの prop 番プロパティ値 (4 成分) を取得する (成功 1)。 */
ACS_EDITOR_API int acs_editor_node_component_prop_get(void* handle, int id, int slot, int prop,
                                                      float* x, float* y, float* z, float* w) {
    auto* host = static_cast<EditorHost*>(handle);
    const FEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr || slot < 0 || slot >= static_cast<int>(n->component_count)
        || prop < 0 || prop >= static_cast<int>(FEditorNode::kMaxProps)) return 0;
    const f32* v = n->comp_props[static_cast<u32>(slot)][static_cast<u32>(prop)];
    if (x != nullptr) *x = v[0];
    if (y != nullptr) *y = v[1];
    if (z != nullptr) *z = v[2];
    if (w != nullptr) *w = v[3];
    return 1;
}

/** ノードの slot 番コンポーネントの prop 番プロパティ値 (4 成分) を設定する (成功 1)。 */
ACS_EDITOR_API int acs_editor_node_component_prop_set(void* handle, int id, int slot, int prop,
                                                      float x, float y, float z, float w) {
    auto* host = static_cast<EditorHost*>(handle);
    FEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr || slot < 0 || slot >= static_cast<int>(n->component_count)
        || prop < 0 || prop >= static_cast<int>(FEditorNode::kMaxProps)) return 0;
    PushUndo(*host);
    SetCompProp(n, static_cast<u32>(slot), static_cast<u32>(prop), x, y, z, w);
    return 1;
}

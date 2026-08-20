// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Editor ABI — C# (WPF) エディタが P/Invoke するための C ABI ブリッジ DLL
// -----------------------------------------------------------------------------
// WPF エディタは XAML で「枠」(メニュー/Hierarchy/Inspector/Console) を描き、
// ビューポートだけネイティブ HWND としてホストする。本 DLL はその HWND にエンジンの
// DX12 スワップチェインを作り、エディタ・シーン (ノード階層 + 2D transform) を
// CSpriteBatch で描画し、シーンの列挙 / 選択 / transform 編集を C ABI で公開する。
//
//   create / attach(hwnd) / render(dt) / resize / destroy / version
//   node_count / node_id_at / node_parent / node_name / node_get_transform /
//   node_set_transform / select / selected
//
// シーンモデルは当面 ABI 内の軽量表現 (将来 ANode/AScene に差し替え可能)。
// =============================================================================
#include "render/Renderer.h"
#include "render/RhiTypes.h"
#include "render/SpriteBatch.h"
#include "render/Fxaa.h"
#include "render/ShadowMap.h"               // 有向光源シャドウマップ (キャスト影)
#include "math/Mat.h"                        // Inverse (sky/影の行列)
#include "gameframework/ProjectSettings.h"
#include "math/Vec.h"
#include "foundation/Log.h"
#include "memory/MemorySystem.h"
#include "threading/Thread.h"
#include "threading/ThreadPool.h"
#include "threading/Mutex.h"
#include "threading/ScopedLock.h"
#include "gameframework/Reflect.h"          // CTypeRegistry / FTypeDesc / ETypeCategory
#include "gameframework/ReflectCatalog.h"   // AcsRegisterEngineTypes (エンジン型カタログ)
#include "gameframework/ANode.h"           // 実シーングラフ ANode / FTransform2D
#include "gameframework/RigidWorld2D.h"     // CRigidWorld2D (Play モードの物理プレビュー)
#include "gameframework/PrimitiveRenderer2D.h"  // APrimitiveRenderer2D::DrawShape (形状描画)
#include "gameframework/Material2D.h"       // FMaterial2D (マテリアルアセット = 効果プリセット)
#include "gameframework/Light2DComponent.h" // ComputeLightDirCone (ライト角度→方向/円錐)
#include "gameframework/ComponentFactory.h" // CreateComponentByName (反射名→実コンポーネント)
#include "gameframework/ReflectApply.h"     // ApplyFieldValue (authored 値を実体へ適用)
#include "gameframework/ReflectMethod.h"    // CMethodRegistry / InvokeMethodByName (関数リフレクション)
#include "gameframework/SceneNodeGraph.h"   // CSceneNodeGraph (3D シーングラフ = 3D ノードの実コンテナ)
#include "gameframework/ANode.h"           // ANode / AComponent
#include "gameframework/MeshComponent3D.h"  // AMeshComponent3D (prim/color/mesh の native 保持)
#include "gameframework/WaterSurface3DComponent.h"
#include "gameframework/HierarchyVisibilityBatch.h"
#include "gameframework/HierarchyWorldTransformBatch.h"
#include "gameframework/Scene3DSerialize.h" // (将来) シリアライザ委譲用
#include "gameframework/SceneTextLoader.h" // strict ACSCENE document preflight
#include "memory/UniquePtr.h"               // TUniquePtr / MakeUnique
#include "memory/SharedPtr.h"               // TSharedPtr / MakeShared (フラットポリゴンメッシュ生成)
#include "container/Array.h"                // TArray
#include "render/IRhiTexture.h"             // IRhiTexture (スプライト)
#include "render/IRhiCommandList.h"
#include "render/RenderAssets.h"            // UploadTexture / UploadMesh / FGpuMesh
// FDebugDraw3D (グリッド/ギズモ/選択 AABB の線)
#include "render/DebugDraw.h"
// エンジン標準の手続きスカイ。
#include "render/Sky.h"
// エンジン標準の PBR 描画。
#include "render/PbrShader.h"
#include "render/StandardShader.h"          // FDirLight (有向光源)
#include "render/PostProcess.h"             // CPostProcess (HDR→ACES トーンマップ)
#include "render/SubsurfaceScattering.h"    // diffuse-only bilateral SSSS
#include "render/Ssao.h"                    // CSsao (GTAO + contact shadow。q_ssao_* を配線)
#include "render/Ssr.h"                     // CSsr (画面空間反射。q_ssr_* を配線)
#include "render/HiZ.h"                     // CHiZ (SSR の full min-depth pyramid)
#include "render/Ssgi.h"                    // CSsgi (画面空間 1 バウンス GI。q_ssgi_* を配線)
#include "render/MotionVector.h"            // CMotionVector (motion+normal G-buffer。TAA/SSR/SSGI の reproject 用)
#include "render/RefractionShader.h"        // CRefractionShader (ガラス/水の屈折。opaque シーンを IOR で曲げて sample)
#include "render/WaterSurface3D.h"
#include "render/Ibl.h"                     // CImageBasedLighting (CSky から env→irradiance/prefilter→SetIbl)
#include "render/Atmosphere.h"              // CAtmosphere (物理大気散乱を equirect に焼く → IBL/背景)
#include "asset/MeshPrimitive.h"            // Primitive::MakeCube/MakeSphere/MakePlane
#include "asset/MeshAsset.h"                // AMeshAsset
#include "math/Camera.h"                    // CCamera (透視 + lookAt)
#include "math/CameraRig.h"                 // ScreenPointToRay (透視/正射 両対応のピックレイ)
#include "math/Mat.h"                       // FMat4 (model 行列の合成)
#include "math/Math.h"                      // kDeg2Rad
#include "math/Collision3D.h"               // Aabb3 (選択ハイライト)
#include "collision/MeshCollider.h"         // exact cached BVH hit testing for custom water
#include "asset/ImageAsset.h"               // AImageAsset / ImageAssetLoader (stb_image)
#include "asset/AssetId.h"                  // kInvalidAssetId
#include "platform/FileSystem.h"            // FileSystem::ReadAllBytes
#include "editor_abi/EditorProfiler.h"       // renderer profiler snapshot ABI
#include "editor_abi/EditorCameraViewRequests.h"
#include "editor_abi/EditorFrustumCulling.h"
#include "editor_abi/EditorSubsurfaceVisibility.h"
#include "editor_abi/EditorCloudWorkload.h"  // optional exact cloud-work snapshot ABI
#include "editor_abi/EditorFrameContract.h"  // busy/fatal/presented frame contract
#include "editor_abi/EditorAbiCapabilities.h" // versioned host capability negotiation
#include "editor_abi/EditorServiceDiagnostics.h"
#include "editor_abi/EditorRenderPolicy.h"   // producer/consumer gates for cached render data

#include <cstdint>
#include <cstdio>   // std::snprintf / std::sscanf
#include <cstring>  // std::strncmp (シーン読込のヘッダ判定)
#include <cmath>    // sinf / cosf
#include <algorithm> // std::max / std::abs (3D ピック)
#include <new>      // std::nothrow
#include <atomic>   // std::atomic (エンジンログ取り込み用 SPSC リング)
#include <array>
#include <limits>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cstdlib>

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
static_assert(
    editor_camera_view::kStableCameraIdBytes ==
    game::kScene3DSerializeMaxCameraIdBytes);
constexpr f32 kCamera3DPitchLimit = 1.5533f;  // 89 degrees: avoid the orbit-camera pole.
constexpr f32 kCamera3DMinDistance = 1.0f;
constexpr f32 kCamera3DMaxDistance = 200.0f;
constexpr f32 kCamera3DFramePitch = 0.55f;    // Stable downward framing view.
std::atomic<u64> g_next_editor_host_generation{1u};
std::atomic<u64> g_next_editor_diagnostic_generation{1u};
struct FEditorHost;
inline constexpr usize kMaxLiveEditorHosts = 4096u;
FMutex g_editor_host_registry_mutex;
std::array<const FEditorHost*, kMaxLiveEditorHosts>
    g_live_editor_hosts{};
usize g_live_editor_host_count = 0u;

bool RegisterEditorHost(const FEditorHost* host) noexcept
{
    if (host == nullptr) return false;
    const FScopedLock lock(g_editor_host_registry_mutex);
    for (usize index = 0u;
         index < g_live_editor_host_count;
         ++index) {
        if (g_live_editor_hosts[index] == host) return false;
    }
    if (g_live_editor_host_count == kMaxLiveEditorHosts) return false;
    g_live_editor_hosts[g_live_editor_host_count++] = host;
    return true;
}

bool UnregisterEditorHost(const FEditorHost* host) noexcept
{
    if (host == nullptr) return false;
    const FScopedLock lock(g_editor_host_registry_mutex);
    for (usize index = 0u;
         index < g_live_editor_host_count;
         ++index) {
        if (g_live_editor_hosts[index] != host) continue;
        --g_live_editor_host_count;
        g_live_editor_hosts[index] =
            g_live_editor_hosts[g_live_editor_host_count];
        g_live_editor_hosts[g_live_editor_host_count] = nullptr;
        return true;
    }
    return false;
}

/**
 * エディタ・シーンの 1 ノード。エンジンの実 ANode を継承し、階層・transform・
 * コンポーネントはエンジン側 (ANode) が担う。エディタ表示用メタ (id / 名前 / 色 /
 * ベースサイズ) だけを上乗せする。
 */
struct AEditorNode : public game::ANode {
    static constexpr u32 kMaxComponents = 8;
    static constexpr u32 kMaxProps      = 24;  // reflection/editor shared authored-field capacity

    int   editor_id = 0;                                   // エディタが振る安定 int id
    char  name[64]  = {};                                  // 表示名 (UTF-8)
    f32   base      = 48.0f;                               // 表示矩形のベースサイズ (px)
    FVec4 color     = FVec4{ 0.5f, 0.6f, 0.8f, 1.0f };     // 表示色

    // アタッチされた Component 型 (リフレクション type-id)。エディタ・メタデータとして
    // 保持し (実 AComponent は生成しない)、シリアライズ + 将来の play/load で実体化する。
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

    // プレハブ・インスタンス: 元 .acsprefab のパス (空 = 通常ノード)。インスペクタの Apply/Revert に使う。
    char                  prefab_src[256] = {};

    // 使用マテリアル (.acsmat)。path が設定されていれば、このノードの描画を効果プリセットで
    // 包む (SetEffect→draw→ClearEffect)。material は path から解析した結果のキャッシュ。
    char                  material_path[256] = {};
    game::FMaterial2D     material{};            // material_path を解析した効果プリセット
    bool                  material_loaded = false;
    TUniquePtr<IRhiTexture> mat_albedo_tex;      // PBR マテリアルのアルベド画像 (遅延ロード、任意)
    TUniquePtr<IRhiTexture> mat_normal_tex;      // PBR マテリアルの法線マップ (遅延ロード)
    bool                  mat_tex_loaded = false; // mat 法線/アルベドのロード試行済みか

    // カスタムポリゴン形状 (APrimitiveRenderer2D shape=Polygon のとき)。ノード原点基準のローカル頂点。
    // poly_verts = コライダー (凸包に間引いた ≤kMaxPolyVerts 頂点、物理に渡す)。
    u32   poly_count = 0;
    FVec2 poly_verts[game::kMaxPolyVerts]{};
    // render_verts = 描画用の滑らかな閉曲線 (ベジエ/Catmull-Rom サンプル)。コライダーより頂点が多い。
    static constexpr u32 kMaxRenderVerts = 64;
    u32   render_count = 0;
    FVec2 render_verts[kMaxRenderVerts]{};
};

/** ゲーム DLL から取り込んだユーザー定義型のスキーマ (ディープコピー。DLL アンロード後も有効)。
 *  自前バッファに名前/フィールドを持ち、安定アドレスの FTypeDesc をエディタの CTypeRegistry へ
 *  登録する → AttachComponent / インスペクタ / シリアライズが engine 型と同じ経路で動く。
 *  ※ TArray 再確保でアドレスが動かないよう、必ず New で個別確保し TArray<FUserType*> で持つ。 */
struct FUserType {
    static constexpr u32 kMaxFields = AEditorNode::kMaxProps;   // ≤8 プロパティ
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

/**
 * editor 3D ノードの «エディタ固有» 状態を ANode に載せるコンポーネント (第2段)。
 *
 * @details
 * 第2段でノードデータを engine native へ移した: transform(pos/scale/回転) は
 * ANode::Local()、メッシュ(prim/color/mesh/mesh_path) は AMeshComponent3D、名前は
 * ANode::Name() が持つ。本コンポーネントには engine に該当の無い «editor 整数 id» と
 * «authored オイラー角(度)» だけを残す (オイラーは quat に焼くと丸まるため編集値を保持)。
 */
struct AEditor3DRecordComponent : public acs::game::AComponent {
    ACS_GAME_COMPONENT_KIND(AEditor3DRecordComponent)
    static constexpr u32 kMaxComponents = 8;
    static constexpr u32 kMaxProps      = 24;
    int   id    = 0;                          ///< editor 整数 id (ABI/C# 境界・シリアライズ用)。
    FVec3 euler = FVec3{ 0, 0, 0 };           ///< authored オイラー角 (度、XYZ。Local().rotation の編集元)。
    // マテリアルは AMeshComponent3D が material_path + FMaterial2D で持つ (.acsmat 参照、2D 鏡映)。
    char  sprite_path[256] = {};              ///< 非空ならスプライト (z=0 テクスチャ付きクアッド)。再読込用の画像パス。
    char  prefab_src[256]  = {};              ///< 非空なら prefab/blueprint インスタンス (.acsprefab/.acsbp パス。2D AEditorNode 鏡映)。
    char  prefab_instance_id[game::kScene3DSerializePrefabInstanceIdBytes + 1u] = {}; ///< Apply/Revert後も維持する32桁小文字hexのinstance ID。
    bool  is_empty         = false;           ///< true なら «空ノード» (描画しないグループ用トランスフォーム。2D の空ノード相当)。
    TArray<FVec2> poly_pts;                   ///< 3点以上なら手続きポリゴン (z=0)。再生成用の元 2D 頂点列。
    /** prim==Mesh を CPbrShader で描画するための GPU メッシュキャッシュ。 */
    FGpuMesh       gm_cache;
    CMeshCollider  water_hit_collider;          ///< CPU BVH, built lazily for exact water pointer hits.
    const void*    water_hit_collider_src = nullptr;
    const void*    water_surface_validation_src = nullptr;
    bool           water_surface_local_xz = false;
    bool           water_surface_fallback_logged = false;
    const void*   gm_cache_src = nullptr;      ///< gm_cache の元 AMeshAsset ポインタ (変化時に再アップロード)。
    bool          material_textures_loaded = false;
    TUniquePtr<IRhiTexture> material_albedo_tex;
    TUniquePtr<IRhiTexture> material_normal_tex;
    TUniquePtr<IRhiTexture>
        material_expression_tex[kShaderExpressionMaxTextureSlots];
    FMat4         prev_world = FMat4::Identity(); ///< 前フレームの world 行列 (CMotionVector のモーションベクタ用、実行時のみ・非シリアライズ)。
    bool          prev_world_valid = false;       ///< prev_world が前フレーム値を持つか (新規ノードは初回 motion=0)。
    bool          has_scene_camera = false;       ///< CAM3D authored camera component is present.
    char          scene_camera_id[
        game::kScene3DSerializeMaxCameraIdBytes + 1u] = {};
    int           scene_camera_projection = 0;    ///< 0=Perspective, 1=Orthographic.
    int           scene_camera_priority = 0;
    bool          scene_camera_active = false;
    f32           scene_camera_fov_deg = 60.0f;
    f32           scene_camera_ortho_height = 10.0f;
    f32           scene_camera_near = 0.05f;
    f32           scene_camera_far = 1000.0f;
    // アタッチされた Component 型 (リフレクション type-id)。2D の AEditorNode と同じ «エディタ・メタデータ» 方式。
    game::FTypeId components[kMaxComponents] = {};
    u32           component_count            = 0;
    f32           comp_props[kMaxComponents][kMaxProps][4] = {};
};

struct FM3DVtx {
    f32 px, py, pz, nx, ny, nz, r, g, b, mt, rg;
};

struct FSprVtx {
    f32 px, py, pz, u, v;
};

/**
 * Exact dependency record for the world-space scene-mesh flattening cache.
 * Array order is DFS order, so hierarchy reordering and the bounded vertex
 * truncation policy are represented without relying on a collision-prone hash.
 */
struct FSceneMeshCacheKey {
    const game::ANode* node = nullptr;
    const game::ANode* parent = nullptr;
    const game::AMeshComponent3D* component = nullptr;
    const void* render_handle = nullptr;
    const AMeshAsset* mesh = nullptr;
    u64 mesh_revision = 0u;
    i32 editor_id = 0;
    i32 primitive = 0;
    i32 water_slot = 0;
    u32 state = 0u;
    u32 material_kind = 0u;
    FMat4 world = FMat4::Identity();
    FVec4 node_color{0, 0, 0, 0};
    FVec4 material_base_color{0, 0, 0, 0};
    f32 metallic = 0.0f;
    f32 roughness = 0.0f;

    bool SameAs(const FSceneMeshCacheKey& other) const noexcept {
        return node == other.node &&
               parent == other.parent &&
               component == other.component &&
               render_handle == other.render_handle &&
               mesh == other.mesh &&
               mesh_revision == other.mesh_revision &&
               editor_id == other.editor_id &&
               primitive == other.primitive &&
               water_slot == other.water_slot &&
               state == other.state &&
               material_kind == other.material_kind &&
               std::memcmp(&world, &other.world, sizeof(world)) == 0 &&
               std::memcmp(
                   &node_color, &other.node_color,
                   sizeof(node_color)) == 0 &&
               std::memcmp(
                   &material_base_color, &other.material_base_color,
                   sizeof(material_base_color)) == 0 &&
               std::memcmp(
                   &metallic, &other.metallic,
                   sizeof(metallic)) == 0 &&
               std::memcmp(
                   &roughness, &other.roughness,
                   sizeof(roughness)) == 0;
    }
};

/** 1 つのビューポート + エディタ・シーン (実 ANode ツリー) を保持する描画ホスト。 */
struct FEditorHost {
    CRenderer    renderer;
    CSpriteBatch sprites;
    // Monotonic process-local identity used to reject results that complete
    // after a managed HwndHost has been destroyed and rebuilt.
    u64          abi_host_generation = 0u;
    bool         attached      = false;
    bool         sprites_ready = false;
    // Startup GPU work is advanced one bounded step per render-pump message.
    // Keeping every step on the HWND/render owner thread preserves the native
    // thread-affinity contract while allowing the WPF dispatcher to process
    // paint/input between shader and PSO creation steps.
    u32          startup_step   = 0;
    std::atomic<bool> startup_ready{false};
    std::atomic<bool> startup_failed{false};
    // The managed editor suppresses scene presentation while a scene document is being
    // loaded.  Renderer warm-up and blank swapchain presentation continue, but no old
    // scene, sky, cloud, gizmo, or simulation frame may leak through the loading boundary.
    bool         scene_presentation_suppressed = false;
    // Full-document replacement can nest through compatibility 2D/3D loaders.
    // Only the outer scope joins scene-dependent startup work and waits for the
    // GPU before either graph releases node-owned resources.
    u32          scene_resource_retirement_depth = 0u;
    editor_profiler::FTimePoint startup_begin{};
    // Slow raw-DX12 startup work is staged here. PBR is the exception to the
    // compile-only workers below: it builds the complete unpublished shader
    // candidate (buffers, fallback textures and PSOs included) before the
    // owner thread joins and publishes pbr3d_ready.
    FThread       startup_worker;
    std::atomic<i32> startup_worker_state{0}; // 0=idle, 1=running, 2=ok, -1=failed
    u32           startup_worker_kind = 0u; // 0=none, 1=PBR, 2=SSGI, 3=CSky, 4=clouds, 6=SSSS, 7=post
    f32           startup_worker_elapsed_ms = 0.0f;
    // Diligent owns the compiler threads; submission, status polling and every
    // PSO/resource operation remain on the HWND/render-owner thread.
    u32           startup_async_shader_kind = 0u; // 0=none, 1=PBR, 3=CSky, 4=clouds, 5=SSAO, 7=post
    editor_profiler::FTimePoint startup_async_shader_begin{};
    CSky::FCompiledShaders startup_sky_shaders{};
    CPbrShader::FCompiledShaders startup_pbr_shaders{};
    IRhiDevice* startup_pbr_candidate_device = nullptr;
    EFormat startup_pbr_candidate_rt_format =
        EFormat::B8G8R8A8_UNorm;
    EFormat startup_pbr_candidate_depth_format =
        EFormat::D32_Float;
    IRhiDevice* startup_ssss_candidate_device = nullptr;
    CSsgi::FCompiledShaders startup_ssgi_shaders{};
    CVolumetricClouds::FCompiledShaders startup_cloud_shaders{};
    CSsao::FCompiledShaders startup_ssao_shaders{};
    CPostProcess::FCompiledShaders startup_post_shaders{};
    bool          startup_phase_pending = false;
    f32           startup_phase_elapsed_override_ms = -1.0f;
    u32          width         = 0;
    u32          height        = 0;
    f32          time          = 0.0f;
    f32          frame_dt      = 1.0f / 60.0f;  // 実測 render dt。フレームレート非依存の motion blur shutter に使用。
    editor_profiler::FAccumulator profiler_work{};
    editor_profiler::FSnapshot    profiler_snapshot{};
    editor_cloud_workload::FSnapshot cloud_workload_snapshot{};
    std::atomic<bool>             cloud_workload_available{false};
    editor_profiler::FRollingPeak profiler_cpu_peak{};
    editor_profiler::FRollingPeak profiler_gpu_peak{};
    editor_profiler::FRollingPeak profiler_active_cpu_peak{};
    editor_profiler::FRollingPeak profiler_present_cpu_peak{};
    editor_profiler::FRollingGpuQueryWindow profiler_gpu_queries{};
    u64                           profiler_last_gpu_peak_frame = 0u;
    u64                           profiler_presented_since_reset = 0u;
    u64                           profiler_reset_serial = 0u;
    f32                           profiler_smoothed_fps = 0.0f;
    editor_profiler::FTimePoint   profiler_last_frame_begin{};
    bool                          profiler_has_previous_frame = false;

    // AA: シーンをオフスクリーン RT に描き、MSAA resolve (既定 8x) または FXAA で backbuffer へ出す。
    CFxaa                       fxaa;                  // FXAA パス (MSAA 無効/失敗時のフォールバック)
    bool                        fxaa_ready = false;
    TUniquePtr<IRhiTexture>     scene_rt;              // シーン用オフスクリーン RT (swapchain と同サイズ)
    u32                         scene_rt_w = 0, scene_rt_h = 0;
    u32                         msaa_samples = 8;      // 現在の MSAA サンプル数 (1=FXAA のみ)
    u32                         msaa_pending = 8;      // 次フレーム適用 (acs_editor_set_msaa)
    // True only while no GPU work has been submitted since the most recent
    // resource-mutation WaitIdle. Resize and an MSAA rebuild in the same owner-
    // thread turn therefore share one queue drain.
    bool                        resource_mutation_idle = false;

    // マテリアルプレビュー用の非 MSAA スプライトバッチ (preview_rt は sample_count=1 のため
    // MSAA PSO の本体バッチでは描けない)。EnsurePreviewRt で遅延初期化。
    CSpriteBatch                preview_sprites;
    bool                        preview_sprites_ready = false;

    // プロジェクト設定。acs_editor_settings_load で
    // <project>/Config/ProjectSettings.ini を読み、変更のたび保存 + エンジンへ適用する。
    game::CProjectSettings      settings;
    char                        settings_path[512] = {};
    FVec3                       ambient      = FVec3{ 0.10f, 0.11f, 0.13f };   // Rendering.AmbientColor
    f32                         light_height = 90.0f;                          // Rendering.LightHeight
    FClearColor                  clear_color  { 0.07f, 0.08f, 0.10f, 1.0f };    // Rendering.FClearColor

    // 品質プリセット (Rendering/QualityLevel) で駆動するノブのキャッシュ。ApplyQualityPreset が埋める。
    // 既定は High 相当。S3 では post-process のみ配線、shadow/SSAO/SSGI/SSR/IBL は後続ステップで配線。
    u32   q_shadow_size      = 2048;  u32  q_shadow_cascades = 2;
    f32   q_shadow_bias      = 0.0015f; f32 q_shadow_filter   = 1.0f;
    bool  q_ssao_on          = true;  f32  q_ssao_intensity  = 1.0f;  f32 q_ssao_radius = 0.8f;
    bool  q_ssgi_on          = false; f32  q_ssgi_intensity  = 1.0f;  f32 q_ssgi_max_dist = 10.0f;
    bool  q_vxgi_on          = false;  // VXGI(voxel GI)。64³ ボクセルの blocky な色のにじみが目立つため既定OFF→滑らかな screen-space SSGI を使用。VxgiOn=1 で実験的に有効化可
    bool  q_ap_on            = false;  // 物理大気の距離霞。ローカル FogDensity は独立に 48x48x96 froxel を起動する
    bool  q_ssr_on           = true;  f32  q_ssr_intensity   = 0.8f;  bool q_ssr_hiz = false;
    i32   q_ibl_mode         = 1;     FVec3 q_ambient        = FVec3{ 0.26f, 0.28f, 0.33f };  // 0=flat 1=sh9 2=cubemap
    bool  q_bloom_on         = true;  f32  q_bloom_intensity = 0.50f; f32 q_bloom_threshold = 0.80f; f32 q_bloom_radius = 1.5f;
    f32   q_exposure         = 1.05f; f32  q_cg_saturation   = 1.10f; f32 q_cg_contrast = 1.12f;
    i32   q_tonemap          = 0;     bool q_auto_exposure   = false;  // 0=ACES 1=AgX 2=Reinhard / 自動露出(eye adaptation)
    // 既定は無効。FogDensity が正なら単一色の高さフォグを有効にする。
    bool  q_fog_on           = false; f32  q_fog_density     = 0.015f; f32 q_fog_height_falloff = 0.10f;
    i32   q_sky_mode         = 0;     // 0=CSky(グラデ+雲) / 1=CAtmosphere(物理大気散乱)。要 Diligent (IBL 経路)
    f32   q_cloud_coverage   = 0.50f; f32 q_cloud_density = 1.6f; f32 q_cloud_wind = 1.0f;
    f32   q_cloud_render_scale = 0.75f;   // quality multiplier for the internal quarter-dimension trace policy
    f32   q_cloud_base       = 1500.0f; f32 q_cloud_top = 4000.0f; f32 q_cloud_noise_scale = 0.035f; // world-space volumetric cloud layer
    f32   q_cas              = 0.3f;  bool q_taa_on          = false; u32 q_msaa_default = 4;
    FVec3 sun_dir            = FVec3{ 0.40f, 0.85f, -0.35f };   // 太陽 (光源) 方向 «光へ向かう» 向き。Rendering/SunAzimuth+Elevation で駆動。
    FVec3 sun_color          = FVec3{ 1.0f, 0.95f, 0.85f };     // 太陽の色 (Rendering/SunColor)。
    f32   sun_intensity      = 2.35f;                           // 太陽の強度 (Rendering/SunIntensity)。光色 = sun_color * sun_intensity。
    FVec3 sky_zenith         = FVec3{ 0.16f, 0.33f, 0.62f };    // 空グラデ天頂/地平/下半球 (Rendering/Sky*)。IBL 環境光源 + 空背景の両方を駆動。
    FVec3 sky_horizon        = FVec3{ 0.62f, 0.70f, 0.80f };
    FVec3 sky_ground         = FVec3{ 0.20f, 0.19f, 0.21f };

    // マテリアル GPU プレビュー: production PBR は線形 HDR に描き、専用
    // ACES + sRGB resolve で表示用 LDR に落とす。描画解像度と readback
    // 解像度を分離し、UI の High/Production 品質では 2x/4x SSAA を行う。
    TUniquePtr<IRhiCommandList> preview_cl;            // 専用コマンドリスト (フレーム外で submit)
    TUniquePtr<IRhiTexture>     preview_rt;            // readback 用 LDR RT (ColorFormat)
    TUniquePtr<IRhiTexture>     preview_work_ldr;      // Toon/Effect の supersample 作業 RT
    TUniquePtr<IRhiTexture>     preview_hdr_rt;        // PBR/Substrate の線形 RGBA16F 作業 RT
    TUniquePtr<IRhiTexture>     preview_depth;         // HDR 作業解像度の depth target
    TUniquePtr<IRhiTexture>     preview_ibl_irradiance;// SH9 使用時の有効な cube binding
    TUniquePtr<IRhiTexture>     preview_ibl_prefilter; // SH9 使用時の有効な cube binding
    TUniquePtr<IRhiTexture>     preview_brdf_lut;      // studio specular split-sum LUT
    u32                         preview_rt_size = 0;
    u32                         preview_ldr_work_size = 0;
    u32                         preview_hdr_size = 0;
    CPbrShader                  preview_pbr3d;         // main viewport state と分離した real PBR/Substrate preview
    bool                        preview_pbr3d_ready = false;
    TUniquePtr<IRhiShader>      preview_post_vs;
    TUniquePtr<IRhiShader>      preview_background_ps;
    TUniquePtr<IRhiShader>      preview_resolve_ps;
    TUniquePtr<IRhiPipeline>    preview_background_pipe;
    TUniquePtr<IRhiPipeline>    preview_resolve_pipe;
    TUniquePtr<IRhiBuffer>      preview_post_cb;
    bool                        preview_post_ready = false;
    u32                         preview_quality = 1;   // 0=1x, 1=2x, 2=4x
    u32                         preview_model = 0;     // 0=sphere, 1=cube, 2=plane
    u32                         preview_background = 0;// 0=studio, 1=checker, 2=black
    f32                         preview_exposure = 1.0f;
    FGpuMesh                    preview_mesh_sphere;
    FGpuMesh                    preview_mesh_cube;
    FGpuMesh                    preview_mesh_plane;
    TUniquePtr<IRhiTexture>     preview_sphere_albedo; // 白い円 (alpha 付き、PBR 球の形)
    TUniquePtr<IRhiTexture>     preview_sphere_normal; // 球の法線マップ
    TUniquePtr<IRhiTexture>     preview_scene;         // 効果プレビュー用のミニ風景
    bool                        preview_samples_ready = false;

    // 実シーングラフ: 隠しルート ANode がツリー全体を所有する。
    TObjectPtr<game::ANode> root;
    // id→ノードの平坦レジストリ (非所有。所有はツリー側)。
    TArray<AEditorNode*>      nodes;
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

    // --- 3D ビューポート: 軌道カメラ + ライト付きプリミティブ + グリッド ---
    // 2D シーン (ANode) とは別系統。view3d=true でレンダ/入力が 3D に切り替わる。
    bool         view3d        = false;
    bool         ortho3d       = false;  // true=正射影 (2D ビュー)。透視⇔正射を切り替え
    f32          cam3d_yaw     = 0.78f;   // 軌道 yaw (rad)
    f32          cam3d_pitch   = 0.55f;  // 軌道 pitch (rad、+で見下ろし)
    f32          cam3d_dist    = 14.0f;   // 注視点からの距離 (ドリー)
    FVec3        cam3d_target  = FVec3{ 0.0f, 1.0f, 0.0f };
    // 3D メッシュの自前ライティングパイプライン (動的 VB + 非インデックス Draw、DebugDraw3D と同方式)。
    TUniquePtr<IRhiShader>   m3d_vs, m3d_ps;
    TUniquePtr<IRhiPipeline> m3d_pipe;                // メッシュ本体 (depth on)
    TUniquePtr<IRhiPipeline> m3d_overlay_pipe;        // ギズモ等のオーバーレイ (depth off, 常に手前)
    TUniquePtr<IRhiBuffer>   m3d_frame_cb;            // b0: view_proj + light + ambient
    TUniquePtr<IRhiBuffer>   m3d_giz_cb;              // ギズモ用 (高アンビエント=フラット、法線非依存)
    TUniquePtr<IRhiBuffer>   m3d_dyn_vb;              // 本体メッシュの動的 VB
    TUniquePtr<IRhiBuffer>   m3d_giz_vb;              // ギズモ用の動的 VB (depth off で描く)
    u32          m3d_dyn_cap  = 0;                    // 動的 VB の頂点容量
    // 2D スプライト (テクスチャ付きクアッド) を 3D シーンに描く (Phase B)。
    TUniquePtr<IRhiShader>   spr_vs, spr_ps;
    TUniquePtr<IRhiPipeline> spr_pipe;                // テクスチャ + アルファブレンド
    TUniquePtr<IRhiBuffer>   spr_vb;                  // スプライト 1 枚 (6 頂点) の動的 VB
    TArray<TUniquePtr<IRhiTexture>> sprite_textures;  // ノードの renderHandle が指す GPU テクスチャ群 (所有)
    // スカイボックス (フルスクリーン三角形で天空グラデーション)。
    TUniquePtr<IRhiShader>   sky_vs, sky_ps;
    TUniquePtr<IRhiPipeline> sky_pipe;
    TUniquePtr<IRhiBuffer>   sky_cb;                  // カメラ基底 (レイ再構成用)
    /** エンジン標準スカイ。 */
    acs::CSky                sky3d;
    bool                     sky3d_ready = false;
    /** エンジン標準の PBR 描画。 */
    acs::CPbrShader          pbr3d;
    bool                     pbr3d_ready = false;
    /** HDR 画像へ ACES トーンマップを適用する。 */
    acs::CPostProcess        post3d;
    bool                     post3d_ready = false;
    u32                      post3d_w = 0, post3d_h = 0;
    acs::CSubsurfaceScattering ssss3d;
    bool                     ssss3d_ready = false;
    bool                     ssss3d_init_failed = false;
    // Pipeline creation and the full-resolution target pair are separate
    // render-pump commits. Raw DX12 builds the unpublished pipeline candidate
    // on startup_worker; state 2 allocates only the internal target pair.
    u32                      ssss3d_init_state = 0u; // 0=idle, 1=compiling, 2=pipeline ready, 3=failed
    acs::CSubsurfaceScattering::FCompiledShaders
                             ssss3d_pending_shaders{};
    TUniquePtr<IRhiTexture>  ssss_diffuse_rt;
    TUniquePtr<IRhiTexture>  ssss_material_rt;
    u32                      ssss_w = 0, ssss_h = 0;
    // The external MRT pair is built one allocation per render-pump call and
    // published only when both candidates match the current viewport.
    TUniquePtr<IRhiTexture>  ssss_pending_diffuse_rt;
    u32                      ssss_pending_w = 0, ssss_pending_h = 0;
    u32                      ssss_frame_resource_state = 0u; // 0=idle, 1=diffuse ready, 2=failed
    u32                      ssss_frame_failed_w = 0, ssss_frame_failed_h = 0;
    // SSAO (GTAO + contact shadow)。法線 G-buffer プリパス → CSsao → CPbrShader.SetSsao で ambient に乗算。
    acs::CSsao               ssao3d;                   // エンジン GTAO。法線 gbuffer + depth を要求
    bool                     ssao_ready = false;       // CSsao Init 成否
    bool                     ssao_pipe_ready = false;  // 法線プリパスのパイプライン成否
    u32                      ssao_w = 0, ssao_h = 0;
    bool                     ssao_computed = false;    // 今フレーム AO を焼いたか (main パスで SetSsao 判定)
    TUniquePtr<IRhiTexture>  normal_rt;                // 法線 G-buffer (RGBA16F world normal)
    u32                      normal_w = 0, normal_h = 0;
    TUniquePtr<IRhiPipeline> normal_pipe;              // M3DVtx → world normal 出力
    TUniquePtr<IRhiShader>   normal_vs, normal_ps;
    TUniquePtr<IRhiBuffer>   normal_cb;                // b0: view_proj (プリパス専用・上書き回避)
    // SSR (画面空間反射)。SSAO と同じ法線+深度 G-buffer + 前フレーム scene color から反射を焼く。
    acs::CSsr                ssr3d;                    // エンジン SSR。出力は CPbrShader.SetSsr で roughness ブレンド
    bool                     ssr_ready = false;
    u32                      ssr_w = 0, ssr_h = 0;
    bool                     ssr_computed = false;     // 今フレーム SSR を焼いたか (次フレームの SetSsr 用)
    acs::CHiZ                hiz3d;                    // 偶奇 texture ping-pong の full mip min-depth pyramid
    bool                     hiz3d_ready = false;
    u32                      hiz3d_w = 0, hiz3d_h = 0;
    acs::CSsgi               ssgi3d;                   // エンジン SSGI (1 バウンス間接光)。出力は CPbrShader.SetSsgi で ambient に加算
    bool                     ssgi_ready = false;
    u32                      ssgi_w = 0, ssgi_h = 0;
    bool                     ssgi_init_tried = false;
    bool                     ssgi_computed = false;    // 今フレーム SSGI を焼いたか (次フレームの SetSsgi 用)
    // --- VXGI (voxel global illumination、色のにじみ): 三角形を radiance volume に voxelize →
    //     画面空間で cone trace して間接光を resolve → SSGI スロット (ssgi_color) に流す。
    TUniquePtr<IRhiTexture>  vxgi_vol;                 // radiance volume (64^3 RGBA16F、UAV+SRV)
    TUniquePtr<IRhiTexture>  vxgi_resolve;             // 画面空間 間接光 (half-res、UAV+SRV) → SetSsgi
    TUniquePtr<IRhiBuffer>   vxgi_tri;  u32 vxgi_tri_cap = 0;   // 三角形 SoA (M3DVtx StructuredBuffer)
    TUniquePtr<IRhiShader>   vxgi_cs_clear, vxgi_cs_vox, vxgi_cs_res;
    TUniquePtr<IRhiPipeline> vxgi_pipe_clear, vxgi_pipe_vox, vxgi_pipe_res;
    TUniquePtr<IRhiBuffer>   vxgi_cb_vox, vxgi_cb_res;
    bool                     vxgi_ready = false, vxgi_tried = false;
    u32                      vxgi_rw = 0, vxgi_rh = 0;
    acs::CMotionVector       mv3d;                     // motion + world normal G-buffer。motion を TAA/SSR/SSGI へ供給 (動く物の ghost 除去)
    bool                     mv_ready = false;
    u32                      mv_w = 0, mv_h = 0;
    bool                     mv_computed = false;      // 今フレーム motion を焼いたか
    // 屈折 (ガラス/水): opaque シーンを複製 (blit) → CRefractionShader が IOR で曲げて sample。要 env cubemap (Diligent)。
    acs::CRefractionShader   refr3d;
    bool                     refr_ready = false;
    acs::CWaterSurface3D     water3d;
    bool                     water3d_ready = false;
    acs::CWaterSurface3D::FCompiledShaders water3d_pending_shaders{};
    u32                      water3d_init_state = 0u; // 0=idle,1=compile,2=ready,3=failed,4=bounded commit
    int                      water3d_draw_ids[CWaterSurface3D::kMaxTrackedSurfaces] = {};
    u32                      water3d_draw_count = 0u;
    TUniquePtr<IRhiTexture>  refr_bg;                  // opaque HDR シーンの複製 (同一 RT read+write 不可のため)
    u32                      refr_bg_w = 0, refr_bg_h = 0;
    bool                     water3d_background_failed = false;
    TUniquePtr<IRhiTexture>  water3d_depth_copy;       // pre-water D32 snapshot sampled while live depth stays bound as DSV
    u32                      water3d_depth_copy_w = 0;
    u32                      water3d_depth_copy_h = 0;
    bool                     water3d_depth_copy_failed = false;
    TUniquePtr<IRhiPipeline> blit_pipe;                // hdrRt → refr_bg のフルスクリーン複製 (屈折/DoF 共用)
    TUniquePtr<IRhiShader>   blit_vs, blit_ps;
    bool                     blit_ready = false;
    // 被写界深度 (DoF): depth から CoC を出し、焦点外をディスクぼかし。scene 複製は refr_bg を共用。
    bool                     q_dof_on = false; f32 q_dof_focus = 4.0f; f32 q_dof_range = 5.0f; f32 q_dof_max = 0.010f;
    TUniquePtr<IRhiPipeline> dof_pipe;
    TUniquePtr<IRhiShader>   dof_vs, dof_ps;
    TUniquePtr<IRhiBuffer>   dof_cb;
    bool                     dof_ready = false;
    // god rays (光芒): 太陽スクリーン位置から bright pass を放射状 march して光の筋を加算。scene 複製は refr_bg 共用。
    bool                     q_godray_on = false; f32 q_godray_intensity = 0.5f; f32 q_godray_decay = 0.96f;
    f32                      q_vignette = 0.0f; f32 q_chromatic = 0.0f; f32 q_grain = 0.0f;  // シネマフィルタ (既定 0=クリーン)
    // モーションブラー: CMotionVector の UV 空間 motion に沿って scene を多タップ平均。scene 複製は refr_bg 共用。
    bool                     q_motionblur_on = false; f32 q_motionblur_intensity = 1.0f;
    TUniquePtr<IRhiPipeline> mblur_pipe;
    TUniquePtr<IRhiShader>   mblur_vs, mblur_ps;
    TUniquePtr<IRhiBuffer>   mblur_cb;
    bool                     mblur_ready = false;
    TUniquePtr<IRhiPipeline> gray_pipe;
    TUniquePtr<IRhiShader>   gray_vs, gray_ps;
    TUniquePtr<IRhiBuffer>   gray_cb;
    bool                     gray_ready = false;
    FMat4                    prev_vp = FMat4::Identity();  // 前フレーム view_proj (SSR/SSGI temporal reproject 共用、jitter 込み)
    FMat4                    prev_vp_nojit = FMat4::Identity();  // 前フレーム view_proj (jitter 無し、TAA history reproject 用)
    FVec3                    prev_temporal_camera_eye{};
    bool                     temporal_camera_pose_valid = false;
    u32                      taa_frame = 0;                // TAA Halton ジッタ列のフレームインデックス
    // IBL (鏡面+拡散 環境光)。CSky を env cubemap 化 → irradiance/prefilter/BRDF-LUT → CPbrShader.SetIbl。
    acs::CImageBasedLighting  ibl3d;                    // Diligent backend 専用 (raw-DX12 は失敗 → SH9 フォールバック)
    /** GPU で物理大気の表を構築し、SkyMode==1 で CPU の CAtmosphere を置き換える。 */
    acs::CSkyAtmosphere      sky_atmo;
    bool                     sky_atmo_tried = false;   // Init を一度試したか
    /** GPU レイマーチで立体雲を描画し、CSky の 2D 雲を置き換える。 */
    acs::CVolumetricClouds   vclouds3d;
    bool                     vclouds_ready = false;    // Init 済み
    bool                     vclouds_tried = false;    // Init を一度試したか
    f32                      vclouds_time  = 0.0f;     // 雲アニメ用時間
    bool                     ibl_ready = false;        // 全 cubemap 生成済み (true なら SetIbl、false なら SH9)
    bool                     ibl_tried = false;        // 一度試して失敗したか (毎フレーム再試行を避ける)
    bool                     ibl_dirty = true;         // 空(太陽/色)が変わった → env を再キャプチャ
    bool                     sh9_dirty = true;         // 空が変わった → SH9 環境光(拡散+鏡面)を再計算
    TUniquePtr<IRhiPipeline> grid_pipe;               // 無限グリッド (y=0 / ortho は z=0)
    TUniquePtr<IRhiShader>   grid_vs, grid_ps;
    TUniquePtr<IRhiBuffer>   grid_cb, grid_vb;        // b0: view_proj + 中心、大クアッド頂点
    bool                     show_grid3d = true;      // 3D ビューポートのグリッド表示 (清書/スクショ時に消せる)
    acs::CShadowMap          shadow;                  // 有向光源シャドウマップ (深度テクスチャ + 光VP)
    TUniquePtr<IRhiPipeline> shadow_caster_pipe;      // M3DVtx 用 depth-only キャスター
    TUniquePtr<IRhiShader>   shadow_caster_vs;
    TUniquePtr<IRhiBuffer>   shadow_lvp_cb;           // b0: 光の view-projection (single cascade)
    TUniquePtr<IRhiBuffer>   shadow_cascade_cb[acs::CShadowMap::kMaxCascades];  // CSM: cascade 毎に別 CB (1フレーム内の上書き回避)
    bool                     shadow_ready = false;
    // グリッド/選択 AABB/ギズモの線。
    FDebugDraw3D dbg3d;
    // post後のdisplay-space camera線。
    FDebugDraw3D camera_frustum_dbg3d;
    bool         r3d_ready     = false;   // 3D リソース初期化済み
    u32          r3d_init_phase = 0;      // incremental startup phase
    bool         r3d_init_failed = false;
    FGpuMesh      gm_cube, gm_sphere, gm_plane;
    FGpuMesh      gm_water_plane;                 // 64x64-cell displacement grid
    TSharedPtr<AMeshAsset> cpu_cube, cpu_sphere, cpu_plane;
    TSharedPtr<AMeshAsset> cpu_water_plane;
    TArray<game::ANode*> scene_mesh_nodes;
    TArray<game::ANode*> camera_resolve_nodes;
    TArray<int> camera_node_ids_scratch;
    TArray<FM3DVtx> scene_mesh_vertices;
    TArray<FSceneMeshCacheKey> scene_mesh_key;
    TArray<FSceneMeshCacheKey> scene_mesh_key_scratch;
    TArray<u32> scene_mesh_vertex_offset;
    TArray<u32> scene_mesh_vertex_count;
    TArray<FVec3> scene_mesh_local_center;
    TArray<f32> scene_mesh_local_radius;
    TArray<u8> scene_mesh_visible;
    game::CHierarchyVisibilityBatch scene_mesh_hierarchy_visibility;
    bool scene_mesh_hierarchy_batch_ready = false;
    game::CHierarchyWorldTransformBatch scene_mesh_world_batch;
    bool scene_mesh_world_batch_ready = false;
    TArray<FVec3> frustum_centers_scratch;
    TArray<f32> frustum_radii_scratch;
    TArray<FVec3> frustum_scales_scratch;
    TArray<f32> frustum_padding_scratch;
    TArray<u32> frustum_node_indices_scratch;
    TArray<editor_frustum_culling::FNodeDecision>
        frustum_decisions_scratch;
    // Hot render-path scratch. Capacity is retained by the host so selecting
    // an object or drawing sprites never allocates after the first warm frame.
    TArray<FM3DVtx> gizmo_vertices;
    TArray<FSprVtx> sprite_vertices;
    TArray<IRhiTexture*> sprite_draw_textures;
    FVec3 scene_mesh_bb_min{1e30f, 1e30f, 1e30f};
    FVec3 scene_mesh_bb_max{-1e30f, -1e30f, -1e30f};
    IRhiBuffer* scene_mesh_uploaded_vb = nullptr;
    u32 scene_mesh_cached_cap = 0u;
    u64 scene_mesh_revision = 0u;
    u64 vxgi_tri_uploaded_revision = 0u;
    bool scene_mesh_cache_valid = false;
    int last_render_camera_node_id = -2; // -2=Scene View, -1=game fallback
    bool last_render_camera_projection_valid = false;
    bool last_render_camera_orthographic = false;
    int game_camera_preview_node_id = -1; // non-persistent Camera View override
    // Bounded logical Camera View requests are separate from the one physical
    // swapchain. Only the explicitly bound presenter may drive that surface;
    // all other requests retain independent identity/extent/generation state
    // for the future async offscreen scheduler.
    editor_camera_view::CRegistry camera_view_requests;
    u64 camera_view_frame_serial = 0u;
    u64 last_render_camera_view_request_id = 0u;
    u32 last_render_camera_view_history_generation = 0u;
    bool show_camera_frustum = true;
    int          sel3d         = -1;      // primary (active) 3D ノード id。常に sel3d_multi の一員、空なら -1。
    TArray<int>  sel3d_multi;             // 3D 選択集合 (multi-select。空 ⇔ sel3d==-1)
    int          next_id3d     = 1;
    int          clip3d        = -1;      // 3D コピー&ペースト用クリップボード (コピー元ノード id)
    bool         scene3d_seeded = false;  // 初回 3D 切替でデフォルトシーンを置いたか
    game::CSceneNodeGraph scene3d;        // 3D シーングラフ (各ノード = root の子 ANode + AEditor3DRecordComponent)
    TArray<FVec2> poly3d_pts;             // Ortho ポリゴン描画中の頂点 (XY, z=0 平面へ逆射影済み)
    // 3D ギズモのドラッグ状態。
    //   ハンドル: 0=非活性, 1=X, 2=Y, 3=Z 軸, 4=XY, 5=YZ, 6=XZ 平面。
    int          giz3d_handle  = 0;
    f32          giz3d_start_mx = 0, giz3d_start_my = 0;   // ドラッグ開始マウス (px)
    f32          giz3d_sdx = 0, giz3d_sdy = 0;             // 軸のスクリーン方向 (単位)、軸移動/回転/拡縮用
    f32          giz3d_wpp = 0;                            // world / px (軸方向、移動の換算)
    // 平面移動: 面内 2 軸のスクリーンベクトル (px / gl-world) と gl。スクリーン空間 2x2 連立で安定移動。
    f32          giz3d_p1x = 0, giz3d_p1y = 0;             // e1 の画面ベクトル
    f32          giz3d_p2x = 0, giz3d_p2y = 0;             // e2 の画面ベクトル
    f32          giz3d_pgl = 1.0f;
    FVec3        giz3d_start_pos{ 0, 0, 0 };
    FVec3        giz3d_start_scale{ 1, 1, 1 };
    FVec3        giz3d_start_rot{ 0, 0, 0 };
    int          water_pointer_node = -1;
    FVec3        water_pointer_world{0, 0, 0};
    bool         water_pointer_valid = false;
    f32          water_pointer_emit_time = -1.0f;

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
    bool                           play_camera_snapshot_valid = false;
    f32                            play_cam_pan_x = 0.0f;
    f32                            play_cam_pan_y = 0.0f;
    f32                            play_cam_zoom = 1.0f;
    bool                           play_view3d = false;
    bool                           play_ortho3d = false;
    f32                            play_cam3d_yaw = 0.0f;
    f32                            play_cam3d_pitch = 0.0f;
    f32                            play_cam3d_dist = 14.0f;
    FVec3                          play_cam3d_target{0.0f, 1.0f, 0.0f};
    TUniquePtr<game::CRigidWorld2D> play_world;
    TArray<u32>                    play_body;                 // world ボディ index (play_node と parallel)
    TArray<int>                    play_node;                 // 対応する編集ノード id (parallel)

    // ユーザー定義型 (ゲーム DLL から取り込んだスキーマ)。Build/HotReload で再読込される。
    // ヒープ実体のアドレスは安定 (TArray 再確保は TUniquePtr を move するだけ) → CTypeRegistry が
    // 保持する desc ポインタが無効化されない。
    TArray<TUniquePtr<FUserType>> user_types;

    // シーン実体化 (authored 値で実コンポーネントを attach し tick する) が有効か。
    bool instances_live = false;

    // Preview (DLL ビルド不要のエンジンコンポーネント実行: instantiate→tick を毎フレーム回す)。
    bool preview_live = false;
    TArray<game::FTransform2D> preview_snap;   // Preview 開始時の各ノード transform (停止で復元)

    // ゲームビュー (Game View タブ): true で editor chrome (グリッド/軸/リンク/選択/ギズモ) を描かず
    // ノードの見た目 + Play の OnDraw だけを描く (= スタンドアロンに近い「ゲーム画面」)。
    bool game_view = false;

    // インプロセス Play (ゲーム DLL が実シーンを所有・tick、editor は transform を読み戻して描画)。
    void*     logic_dll   = nullptr;       // LoadLibrary ハンドル (Play 中は載せたまま)
    void*     logic_scene = nullptr;       // DLL 側 Play シーンハンドル
    bool      logic_play  = false;
    FGameShim logic_shim;
    TArray<game::FTransform2D> logic_saved;  // Play 開始時の各ノード transform (停止で復元)
    // Play 開始時に game へ渡した deterministic camera (center/zoom)。
    // get_camera がこれと変わったら、以後は game camera の値を保持する。
    f32  logic_game_cam0_x   = 0.0f;
    f32  logic_game_cam0_y   = 0.0f;
    f32  logic_game_cam0_zoom = 1.0f;
    f32  logic_game_pan_x = 0.0f;
    f32  logic_game_pan_y = 0.0f;
    f32  logic_game_zoom = 1.0f;
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

/**
 * Drop every history whose samples are tied to the current scene, projection,
 * or logical camera owner.
 *
 * Host publication flags are part of the contract: the opaque pass consumes
 * last frame's SSR/SSGI before those effects render the current frame, so
 * clearing only the effect-internal frame counters would still expose one
 * stale frame.
 */
void InvalidateTemporalRenderHistories(FEditorHost& h) noexcept {
    h.mv_computed = false;
    h.taa_frame = 0u;
    h.post3d.InvalidateTaaHistory();
    h.post3d.InvalidateExposureHistory();
    h.ssr3d.InvalidateHistory();
    h.ssgi3d.InvalidateHistory();
    h.vclouds3d.InvalidateHistory();
    h.ssr_computed = false;
    h.ssgi_computed = false;
    h.temporal_camera_pose_valid = false;
}

/** editor_id からノードを引く (無ければ nullptr)。 */
AEditorNode* FindNode(FEditorHost& h, int id) noexcept {
    for (u32 i = 0; i < h.nodes.Num(); ++i) if (h.nodes[i]->editor_id == id) return h.nodes[i];
    return nullptr;
}

// ----- 選択集合の操作 (single/multi 選択。primary = selected) ---------------------

/** id が選択集合に含まれるか。 */
bool SelContains(const FEditorHost& h, int id) noexcept {
    for (u32 i = 0; i < h.selection.Num(); ++i) if (h.selection[i] == id) return true;
    return false;
}

/** 選択を空にする。 */
void SelClear(FEditorHost& h) noexcept {
    h.selection.Reset();
    h.selected = -1;
}

/** 単一選択にする (集合を {id} に置換、primary=id)。id 不正/未知なら選択解除。 */
void SelSet(FEditorHost& h, int id) noexcept {
    h.selection.Reset();
    if (id >= 0 && FindNode(h, id) != nullptr) { h.selection.Add(id); h.selected = id; }
    else h.selected = -1;
}

/** id の選択を反転する (Ctrl+click)。追加なら primary になり、primary を外したら別の一員へ移す。 */
void SelToggle(FEditorHost& h, int id) noexcept {
    if (id < 0 || FindNode(h, id) == nullptr) return;
    for (u32 i = 0; i < h.selection.Num(); ++i) {
        if (h.selection[i] == id) {                       // 既に選択 → 外す
            h.selection.RemoveAtSwap(i);
            if (h.selected == id)
                h.selected = (h.selection.Num() > 0) ? h.selection[h.selection.Num() - 1] : -1;
            return;
        }
    }
    h.selection.Add(id);                              // 未選択 → 追加して primary に
    h.selected = id;
}

/** 構造変更後、選択集合から消えた id を取り除き primary を整える。 */
void SelPrune(FEditorHost& h) noexcept {
    for (u32 i = 0; i < h.selection.Num();) {
        if (FindNode(h, h.selection[i]) == nullptr) h.selection.RemoveAtSwap(i);
        else ++i;
    }
    if (h.selected >= 0 && !SelContains(h, h.selected))
        h.selected = (h.selection.Num() > 0) ? h.selection[h.selection.Num() - 1] : -1;
    if (h.selection.Num() == 0) h.selected = -1;
}

/** id がリストに含まれるか (TArray<int> 版の線形探索)。 */
bool IdInList(const TArray<int>& ids, int id) noexcept {
    for (u32 i = 0; i < ids.Num(); ++i) if (ids[i] == id) return true;
    return false;
}

/**
 * n の祖先 (親～ルート手前) のいずれかが ids に含まれるか。
 *
 * @details 複数選択での「選択ルート」判定に使う。祖先が一括操作 (move/duplicate) の対象なら
 * n はそれに従属するので、n 自身を独立に処理すると二重移動 / 二重複製になる。それを防ぐ。
 */
bool AnyAncestorInList(FEditorHost& h, const AEditorNode* n, const TArray<int>& ids) noexcept {
    game::ANode* p = n->Parent();
    while (p != nullptr && p != h.root.Get()) {
        if (IdInList(ids, static_cast<const AEditorNode*>(p)->editor_id)) return true;
        p = p->Parent();
    }
    return false;
}

/** ノードの「エディタ上の親 id」を返す (隠しルート直下 / 親なしは -1)。 */
int ParentIdOf(const FEditorHost& h, const AEditorNode* n) noexcept {
    game::ANode* p = n->Parent();
    if (p == nullptr || p == h.root.Get()) return -1;
    return static_cast<const AEditorNode*>(p)->editor_id;
}

/** ノードの sprite_path (UTF-8) を読み込んで GPU テクスチャを (再)生成する。
 *  device 未準備 (attach 前) や path 空なら tex を空にして戻る (描画時に再試行される)。 */
void LoadNodeSprite(FEditorHost& h, AEditorNode* n) noexcept {
    if (n == nullptr) return;
    n->sprite_tex.Reset();                                  // 既存を解放 (path 変更/クリア時)
    if (n->sprite_path[0] == '\0') return;
    IRhiDevice* dev = h.renderer.Device();
    if (dev == nullptr) return;                             // attach 前 → DrawScene で再試行
    wchar_t wpath[512];
    if (MultiByteToWideChar(kCpUtf8, 0, n->sprite_path, -1, wpath, 512) <= 0) return;
    auto bytes = CFileSystem::ReadAllBytes(wpath);
    if (bytes.IsErr()) return;
    CImageAssetLoader loader;
    auto decoded = loader.LoadFromBytes(kInvalidAssetId, bytes.Value());
    if (decoded.IsErr()) return;
    auto asset = decoded.Value();                           // TSharedPtr を保持 (即解放を防ぐ)
    const AImageAsset* img = static_cast<const AImageAsset*>(asset.Get());
    if (img == nullptr) return;
    auto tex = UploadTexture(*dev, *img);
    if (tex.IsErr()) return;
    n->sprite_tex = Move(tex.Value());
}

/** ノードの material_path (.acsmat) を解析して material キャッシュへ読み込む。
 *  path 空ならマテリアル無し (効果なし) に戻す。マテリアルエディタで再保存されたら
 *  acs_editor_node_reload_material で material_loaded を落として再解析させる。 */
void LoadNodeMaterial(AEditorNode* n) noexcept {
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
static void LoadTexFromPath(FEditorHost& h, const char* utf8_path, TUniquePtr<IRhiTexture>& out) noexcept {
    out.Reset();
    if (utf8_path == nullptr || utf8_path[0] == '\0') return;
    IRhiDevice* dev = h.renderer.Device();
    if (dev == nullptr) return;
    wchar_t wpath[512];
    if (MultiByteToWideChar(kCpUtf8, 0, utf8_path, -1, wpath, 512) <= 0) return;
    auto bytes = CFileSystem::ReadAllBytes(wpath);
    if (bytes.IsErr()) return;
    CImageAssetLoader loader;
    auto decoded = loader.LoadFromBytes(kInvalidAssetId, bytes.Value());
    if (decoded.IsErr()) return;
    auto asset = decoded.Value();
    const AImageAsset* img = static_cast<const AImageAsset*>(asset.Get());
    if (img == nullptr) return;
    auto tex = UploadTexture(*dev, *img);
    if (tex.IsErr()) return;
    out = Move(tex.Value());
}

/** path から GPU テクスチャを生成し、画像寸法(px)も返す。失敗で空 + (0,0)。スプライト用。 */
static TUniquePtr<IRhiTexture> LoadTexWithSize(FEditorHost& h, const char* utf8_path, u32& outW, u32& outH) noexcept {
    outW = outH = 0;
    TUniquePtr<IRhiTexture> out;
    if (utf8_path == nullptr || utf8_path[0] == '\0') return out;
    IRhiDevice* dev = h.renderer.Device();
    if (dev == nullptr) return out;
    wchar_t wpath[512];
    if (MultiByteToWideChar(kCpUtf8, 0, utf8_path, -1, wpath, 512) <= 0) return out;
    auto bytes = CFileSystem::ReadAllBytes(wpath);
    if (bytes.IsErr()) return out;
    CImageAssetLoader loader;
    auto decoded = loader.LoadFromBytes(kInvalidAssetId, bytes.Value());
    if (decoded.IsErr()) return out;
    auto asset = decoded.Value();
    const AImageAsset* img = static_cast<const AImageAsset*>(asset.Get());
    if (img == nullptr) return out;
    auto tex = UploadTexture(*dev, *img);
    if (tex.IsErr()) return out;
    outW = img->Width(); outH = img->Height();
    out = Move(tex.Value());
    return out;
}

/** PBR マテリアルの法線マップ等を遅延ロードする (device 準備後・1 回)。 */
void LoadNodeMaterialTextures(FEditorHost& h, AEditorNode* n) noexcept {
    if (n == nullptr || n->mat_tex_loaded) return;
    if (n->material.kind == game::EMaterialKind::Lit) {
        LoadTexFromPath(h, n->material.pbr.normalPath, n->mat_normal_tex);
        LoadTexFromPath(h, n->material.pbr.albedoPath, n->mat_albedo_tex);   // 任意のアルベド override
    }
    n->mat_tex_loaded = true;
}

/** ノードに付いた ALight2DComponent の component slot を返す (無ければ -1)。 */
static int LightComponentSlot(const AEditorNode* n) noexcept {
    static game::FTypeId s_id = 0;
    if (s_id == 0) {
        const game::FTypeDesc* d = game::CTypeRegistry::Get().FindByName("ALight2DComponent");
        if (d != nullptr) s_id = d->id;
    }
    if (s_id == 0) return -1;
    for (u32 c = 0; c < n->component_count; ++c)
        if (n->components[c] == s_id) return static_cast<int>(c);
    return -1;
}

/** ノードに付いた AShadowCaster2DComponent の component slot を返す (無ければ -1)。 */
static int ShadowCasterSlot(const AEditorNode* n) noexcept {
    static game::FTypeId s_id = 0;
    if (s_id == 0) {
        const game::FTypeDesc* d = game::CTypeRegistry::Get().FindByName("AShadowCaster2DComponent");
        if (d != nullptr) s_id = d->id;
    }
    if (s_id == 0) return -1;
    for (u32 c = 0; c < n->component_count; ++c)
        if (n->components[c] == s_id) return static_cast<int>(c);
    return -1;
}

/** 実 ANode ノードを生成してツリー + レジストリに追加する (id / 全 transform 指定)。 */
AEditorNode* CreateNode(FEditorHost& h, int id, int parent_id, const char* nm,
                        f32 x, f32 y, f32 rot, f32 sx, f32 sy, f32 base, FVec4 c) noexcept {
    auto child = NewObject<AEditorNode>();
    AEditorNode* p = child.Get();
    p->editor_id = id;
    p->_SetSerialId(id);   // ANode の SerialId = editor_id → ObjectRef を instantiate/tick で解決可
    std::snprintf(p->name, sizeof(p->name), "%s", nm);
    p->SetLocal2D(game::FTransform2D{ FVec2{ x, y }, rot, FVec2{ sx, sy } });
    p->base  = base;
    p->color = c;

    game::ANode* parent = (parent_id >= 0) ? static_cast<game::ANode*>(FindNode(h, parent_id)) : nullptr;
    if (parent == nullptr) parent = h.root.Get();
    parent->AddChild(Move(child));
    h.nodes.Add(p);
    if (id >= h.next_id) h.next_id = id + 1;   // 明示 id でも採番カウンタを進める
    return p;
}

/** 自動採番でノードを追加する (scale=1)。 */
AEditorNode* AddEditorNode(FEditorHost& h, int parent_id, const char* nm,
                           f32 x, f32 y, f32 rot, f32 base, FVec4 c) noexcept {
    return CreateNode(h, h.next_id, parent_id, nm, x, y, rot, 1.0f, 1.0f, base, c);
}

/** コンポーネント slot のプロパティ値を、その型の反射スキーマ既定値で初期化する。 */
void InitCompProps(AEditorNode* n, u32 slot) noexcept {
    for (u32 p = 0; p < AEditorNode::kMaxProps; ++p)
        for (u32 k = 0; k < 4; ++k) n->comp_props[slot][p][k] = 0.0f;
    const game::FTypeDesc* d = game::CTypeRegistry::Get().FindById(n->components[slot]);
    if (d == nullptr) return;
    const u32 nf = d->field_count < AEditorNode::kMaxProps ? d->field_count : AEditorNode::kMaxProps;
    for (u32 p = 0; p < nf; ++p)
        for (u32 k = 0; k < 4; ++k) n->comp_props[slot][p][k] = d->fields[p].defaults[k];
}

/**
 * 登録済み Component 型をノードへアタッチする (記述子のみ。重複/容量超過/非 Component は無視)。
 *
 * @return アタッチ済み or 成功で true、未登録 / 非 Component カテゴリで false。
 */
bool AttachComponent(AEditorNode* n, const char* type_name) noexcept {
    if (n == nullptr || type_name == nullptr) return false;
    game::AcsRegisterEngineTypes();
    const game::FTypeDesc* d = game::CTypeRegistry::Get().FindByName(type_name);
    if (d == nullptr || d->category != game::ETypeCategory::Component) return false;
    for (u32 i = 0; i < n->component_count; ++i) if (n->components[i] == d->id) return true; // 重複
    if (n->component_count >= AEditorNode::kMaxComponents) return false;                      // 容量
    const u32 slot = n->component_count;
    n->components[slot] = d->id;
    InitCompProps(n, slot);                 // スキーマ既定値で値を初期化
    n->component_count = slot + 1;
    return true;
}

/** コンポーネント slot のプロパティ prop に 4 成分値を設定する (範囲外は無視)。 */
void SetCompProp(AEditorNode* n, u32 slot, u32 prop, f32 x, f32 y, f32 z, f32 w) noexcept {
    if (n == nullptr || slot >= n->component_count || prop >= AEditorNode::kMaxProps) return;
    n->comp_props[slot][prop][0] = x; n->comp_props[slot][prop][1] = y;
    n->comp_props[slot][prop][2] = z; n->comp_props[slot][prop][3] = w;
}

/** 型スキーマの公開プロパティ数。3D レコードの容量まで公開する。 */
u32 CompPropCount(const game::FTypeDesc* d) noexcept {
    if (d == nullptr) return 0u;
    return d->field_count < AEditor3DRecordComponent::kMaxProps
        ? d->field_count
        : AEditor3DRecordComponent::kMaxProps;
}

/** 2D レコードへ実際に格納できるプロパティ数。 */
u32 CompPropCount2D(const game::FTypeDesc* d) noexcept {
    const u32 count = CompPropCount(d);
    return count < AEditorNode::kMaxProps
        ? count
        : AEditorNode::kMaxProps;
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
int EmitNodePoly(char* buf, int cur, int cap, const AEditorNode* n) noexcept {
    if (n->poly_count < 3 && n->render_count < 3) return cur;
    cur = EmitVertLine(buf, cur, cap, "POLY", n->editor_id, n->poly_verts, n->poly_count);
    cur = EmitVertLine(buf, cur, cap, "RPLY", n->editor_id, n->render_verts, n->render_count);
    return cur;
}

int EmitCompProps(char* buf, int cur, int cap, const AEditorNode* n) noexcept {
    for (u32 c = 0; c < n->component_count && cur < cap; ++c) {
        const game::FTypeDesc* d = game::CTypeRegistry::Get().FindById(n->components[c]);
        const u32 nf = CompPropCount2D(d);
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
AEditorNode* CloneSubtree(FEditorHost& h, AEditorNode* src, int parent_id, bool top,
                          TArray<int>* oldIds = nullptr, TArray<int>* newIds = nullptr) noexcept {
    const game::FTransform2D t = src->Local2D();
    char nm[64];
    std::snprintf(nm, sizeof(nm), top ? "%s Copy" : "%s", src->name);
    AEditorNode* clone = CreateNode(h, h.next_id, parent_id, nm,
                                    t.position.x, t.position.y, t.rotation, t.scale.x, t.scale.y,
                                    src->base, src->color);
    if (oldIds != nullptr && newIds != nullptr) { oldIds->Add(src->editor_id); newIds->Add(clone->editor_id); }
    clone->SetVisible(src->IsVisible());           // 表示フラグも複製
    clone->SetEnabled(src->IsEnabled());
    clone->SetDrawLayer(src->DrawLayer());
    std::memcpy(clone->sprite_path, src->sprite_path, sizeof(clone->sprite_path));   // tex は描画時に遅延ロード
    std::memcpy(clone->prefab_src, src->prefab_src, sizeof(clone->prefab_src));      // プレハブリンクも複製
    std::memcpy(clone->material_path, src->material_path, sizeof(clone->material_path)); // material は描画時に遅延ロード
    clone->poly_count = src->poly_count;                                             // カスタムポリゴン (コライダー)
    for (u32 pv = 0; pv < src->poly_count; ++pv) clone->poly_verts[pv] = src->poly_verts[pv];
    clone->render_count = src->render_count;                                         // 滑らか描画頂点
    for (u32 rv = 0; rv < src->render_count; ++rv) clone->render_verts[rv] = src->render_verts[rv];
    // コンポーネント記述子 + 編集プロパティ値をコピー。
    for (u32 i = 0; i < src->component_count && clone->component_count < AEditorNode::kMaxComponents; ++i) {
        const u32 slot = clone->component_count;
        clone->components[slot] = src->components[i];
        for (u32 p = 0; p < AEditorNode::kMaxProps; ++p)
            for (u32 k = 0; k < 4; ++k) clone->comp_props[slot][p][k] = src->comp_props[i][p][k];
        ++clone->component_count;
    }
    // 子を再帰複製 (src の子配列は不変なので走査安全)。
    for (u32 i = 0; i < src->ChildCount(); ++i)
        CloneSubtree(h, static_cast<AEditorNode*>(src->Child(i)), clone->editor_id, false, oldIds, newIds);
    return clone;
}

/** 複製した subtree 内の ObjectRef プロパティ値を old→new id で再マップする (subtree 内参照のみ)。 */
void RemapClonedObjectRefs(FEditorHost& h, const TArray<int>& oldIds, const TArray<int>& newIds) noexcept {
    auto Map = [&](int o) -> int {
        for (u32 i = 0; i < oldIds.Num(); ++i) if (oldIds[i] == o) return newIds[i];
        return -1;
    };
    for (u32 n = 0; n < newIds.Num(); ++n) {
        AEditorNode* node = FindNode(h, newIds[n]);
        if (node == nullptr) continue;
        for (u32 slot = 0; slot < node->component_count; ++slot) {
            const game::FTypeDesc* d = game::CTypeRegistry::Get().FindById(node->components[slot]);
            if (d == nullptr) continue;
            const u32 nf = (d->field_count < AEditorNode::kMaxProps) ? d->field_count : AEditorNode::kMaxProps;
            for (u32 prop = 0; prop < nf; ++prop) {
                if (d->fields[prop].kind != game::EFieldKind::ObjectRef) continue;
                const int rm = Map(static_cast<int>(node->comp_props[slot][prop][0]));
                if (rm >= 0) node->comp_props[slot][prop][0] = static_cast<float>(rm);   // 内部参照のみ
            }
        }
    }
}

// Defined beside the startup-worker state machine. Nested document loaders use
// the same owner-thread retirement transaction, so only the outermost scope
// joins scene-dependent work and waits for submitted GPU references.
void BeginSceneResourceRetirement(FEditorHost& h) noexcept;

class FSceneResourceRetirementScope final {
public:
    explicit FSceneResourceRetirementScope(FEditorHost& host) noexcept
        : m_Host(host) {
        BeginSceneResourceRetirement(m_Host);
    }

    ~FSceneResourceRetirementScope() noexcept {
        ACS_ASSERT(m_Host.scene_resource_retirement_depth > 0u);
        --m_Host.scene_resource_retirement_depth;
    }

    FSceneResourceRetirementScope(
        const FSceneResourceRetirementScope&) = delete;
    FSceneResourceRetirementScope& operator=(
        const FSceneResourceRetirementScope&) = delete;

private:
    FEditorHost& m_Host;
};

/** Release only the 2D graph. The caller must own a retirement scope. */
void ClearScene2DResourcesRetired(FEditorHost& h) noexcept {
    h.nodes.Reset();                           // 先にレジストリを空に (dangling 回避)
    h.root     = NewObject<game::ANode>();  // 旧ツリーは再代入で解放
    h.next_id  = 1;
    SelClear(h);
}

/** シーンを空に戻す (隠しルートだけの状態)。 */
void ClearScene(FEditorHost& h) noexcept {
    FSceneResourceRetirementScope retirement(h);
    ClearScene2DResourcesRetired(h);
}

/** Release only the 3D graph and node-owned GPU resources under retirement. */
void ClearScene3DResourcesRetired(FEditorHost& h) noexcept {
    h.water3d.ClearDisturbances();
    h.water_pointer_valid = false;
    h.water_pointer_node = -1;
    h.water_pointer_emit_time = -1.0f;
    InvalidateTemporalRenderHistories(h);
    h.scene3d.Clear();
    // RenderHandle pointers inside the retired nodes point into this array.
    // Both must be destroyed only after the outer owner-thread WaitIdle.
    h.sprite_textures.Reset();
    h.scene3d.Update(0.0f);
    h.sel3d = -1;
    h.sel3d_multi.Reset();
    h.clip3d = -1;
    h.next_id3d = 1;
    h.scene3d_seeded = true;
    h.poly3d_pts.Reset();
    h.giz3d_handle = 0;
    h.water3d_draw_count = 0u;
    h.scene_mesh_nodes.Reset();
    h.camera_resolve_nodes.Reset();
    h.camera_node_ids_scratch.Reset();
    h.scene_mesh_vertices.Reset();
    h.scene_mesh_key.Reset();
    h.scene_mesh_key_scratch.Reset();
    h.scene_mesh_vertex_offset.Reset();
    h.scene_mesh_vertex_count.Reset();
    h.scene_mesh_local_center.Reset();
    h.scene_mesh_local_radius.Reset();
    h.scene_mesh_visible.Reset();
    h.scene_mesh_hierarchy_visibility.Clear();
    h.scene_mesh_hierarchy_batch_ready = false;
    h.scene_mesh_world_batch.Clear();
    h.scene_mesh_world_batch_ready = false;
    h.frustum_centers_scratch.Reset();
    h.frustum_radii_scratch.Reset();
    h.frustum_scales_scratch.Reset();
    h.frustum_padding_scratch.Reset();
    h.frustum_node_indices_scratch.Reset();
    h.frustum_decisions_scratch.Reset();
    h.scene_mesh_cache_valid = false;
    h.last_render_camera_node_id = -2;
    h.game_camera_preview_node_id = -1;
    // Preserve managed request leases across an atomic scene replacement, but
    // never let an old node id present the replacement graph. The managed
    // stable-id refresh may update and rebind the same opaque request later.
    h.camera_view_requests.MarkAllCamerasStale();
    h.last_render_camera_view_request_id = 0u;
    h.last_render_camera_view_history_generation = 0u;
}

void ClearScene3D(FEditorHost& h) noexcept {
    FSceneResourceRetirementScope retirement(h);
    ClearScene3DResourcesRetired(h);
}

/** node 配下を DFS で平坦レジストリへ積む (親が子より先 = save 順を保つ)。 */
void CollectNodes(FEditorHost& h, game::ANode* node) noexcept {
    for (u32 i = 0; i < node->ChildCount(); ++i) {
        game::ANode* c = node->Child(i);
        h.nodes.Add(static_cast<AEditorNode*>(c));
        CollectNodes(h, c);
    }
}

/** 構造変更 (削除 / 付け替え) の後にツリーから平坦レジストリを作り直す。 */
void RebuildRegistry(FEditorHost& h) noexcept {
    h.nodes.Reset();
    if (h.root.Get() != nullptr) CollectNodes(h, h.root.Get());
    // 構造変更で消えた選択を集合から取り除き、primary を整える。
    SelPrune(h);
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
int EmitNodeLine(char* buf, int cur, int cap, const FEditorHost& h, const AEditorNode* n) noexcept {
    if (cur >= cap) return cur;
    const game::FTransform2D t = n->Local2D();
    const int pid = ParentIdOf(h, n);
    cur += std::snprintf(buf + cur, static_cast<size_t>(cap - cur),
        "%d %d %.4f %.4f %.4f %.4f %.4f %.2f %.3f %.3f %.3f %.3f %s\n",
        n->editor_id, pid, t.position.x, t.position.y, t.rotation, t.scale.x, t.scale.y,
        n->base, n->color.x, n->color.y, n->color.z, n->color.w, n->name);
    return cur;
}

// ツリーを DFS して «親→子» の順にノード行を出力する (reparent 後でも読込が親を先に解決できる)。
int EmitNodeTreeDFS(char* buf, int cur, int cap, const FEditorHost& h, const game::ANode* node) noexcept {
    for (u32 i = 0; i < node->ChildCount() && cur < cap; ++i) {
        const game::ANode* c = node->Child(i);
        if (c == nullptr) continue;
        cur = EmitNodeLine(buf, cur, cap, h, static_cast<const AEditorNode*>(c));
        cur = EmitNodeTreeDFS(buf, cur, cap, h, c);
    }
    return cur;
}

const char* SerializeScene(FEditorHost& h) noexcept {
    char* buf = h.scene_text;
    const int cap = static_cast<int>(sizeof(h.scene_text));
    int cur = std::snprintf(buf, static_cast<size_t>(cap), "ACSCENE v1\n%u\n",
                            static_cast<u32>(h.nodes.Num()));
    cur = EmitNodeTreeDFS(buf, cur, cap, h, h.root.Get());
    // コンポーネント記述子 (ノードごと COMP <editor_id> <type_name>)。
    for (u32 i = 0; i < h.nodes.Num() && cur < cap; ++i) {
        const AEditorNode* n = h.nodes[i];
        for (u32 cmp = 0; cmp < n->component_count && cur < cap; ++cmp) {
            const game::FTypeDesc* d = game::CTypeRegistry::Get().FindById(n->components[cmp]);
            if (d != nullptr && d->name != nullptr)
                cur += std::snprintf(buf + cur, static_cast<size_t>(cap - cur),
                                     "COMP %d %s\n", n->editor_id, d->name);
        }
    }
    // コンポーネントの編集プロパティ (ノードごと CPROP <id> <slot> <prop> <x y z w>)。
    for (u32 i = 0; i < h.nodes.Num() && cur < cap; ++i)
        cur = EmitCompProps(buf, cur, cap, h.nodes[i]);
    // ノードフラグ (非既定のみ): NFLG <id> <visible> <enabled> <sortLayer>。
    // 既定 (visible=1,enabled=1,layer=0) は省略 → 後方互換 (旧ファイルは全既定扱い)。
    for (u32 i = 0; i < h.nodes.Num() && cur < cap; ++i) {
        const AEditorNode* n = h.nodes[i];
        if (!n->IsVisible() || !n->IsEnabled() || n->DrawLayer() != 0) {
            const int w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur),
                "NFLG %d %d %d %d\n", n->editor_id,
                n->IsVisible() ? 1 : 0, n->IsEnabled() ? 1 : 0, n->DrawLayer());
            if (w < 0 || w >= cap - cur) { buf[cap - 1] = '\0'; return buf; }
            cur += w;
        }
    }
    // スプライト画像パス (設定済みのみ): SPRT <id> <utf8_path> (path は行末まで)。
    for (u32 i = 0; i < h.nodes.Num() && cur < cap; ++i) {
        const AEditorNode* n = h.nodes[i];
        if (n->sprite_path[0] != '\0') {
            const int w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur),
                                        "SPRT %d %s\n", n->editor_id, n->sprite_path);
            if (w < 0 || w >= cap - cur) { buf[cap - 1] = '\0'; return buf; }
            cur += w;
        }
    }
    // プレハブリンク (インスタンスのみ): PFAB <id> <utf8_path>。
    for (u32 i = 0; i < h.nodes.Num() && cur < cap; ++i) {
        const AEditorNode* n = h.nodes[i];
        if (n->prefab_src[0] != '\0') {
            const int w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur),
                                        "PFAB %d %s\n", n->editor_id, n->prefab_src);
            if (w < 0 || w >= cap - cur) { buf[cap - 1] = '\0'; return buf; }
            cur += w;
        }
    }
    // 使用マテリアル (.acsmat パス): MAT <id> <utf8_path> (path は行末まで)。
    for (u32 i = 0; i < h.nodes.Num() && cur < cap; ++i) {
        const AEditorNode* n = h.nodes[i];
        if (n->material_path[0] != '\0') {
            const int w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur),
                                        "MAT %d %s\n", n->editor_id, n->material_path);
            if (w < 0 || w >= cap - cur) { buf[cap - 1] = '\0'; return buf; }
            cur += w;
        }
    }
    // カスタムポリゴン: POLY <id> <count> <x0> <y0> ...
    for (u32 i = 0; i < h.nodes.Num() && cur < cap; ++i)
        cur = EmitNodePoly(buf, cur, cap, h.nodes[i]);
    // 選択集合 (undo/redo/open で選択を保つ): SEL <primary> <count> <id...>
    if (cur < cap) {
        int w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur),
                              "SEL %d %u", h.selected, static_cast<u32>(h.selection.Num()));
        if (w > 0 && w < cap - cur) {
            cur += w;
            bool ok = true;
            for (u32 i = 0; i < h.selection.Num() && ok; ++i) {
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

enum class EEditorTextLineResult : u8 {
    End,
    Line,
    TooLong,
};

EEditorTextLineResult ReadEditorTextLine(
    const char*& cursor, char* out, u32 capacity) noexcept {
    if (cursor == nullptr || *cursor == '\0') return EEditorTextLineResult::End;
    u32 length = 0u;
    const char* scan = cursor;
    while (*scan != '\0' && *scan != '\n') {
        if (length + 1u >= capacity) return EEditorTextLineResult::TooLong;
        out[length++] = *scan++;
    }
    if (length > 0u && out[length - 1u] == '\r') --length;
    out[length] = '\0';
    cursor = (*scan == '\n') ? scan + 1 : scan;
    return EEditorTextLineResult::Line;
}

void SkipEditorTextWhitespace(const char*& text) noexcept {
    while (*text != '\0' &&
           std::isspace(static_cast<unsigned char>(*text)) != 0) {
        ++text;
    }
}

bool EditorTextOnlyWhitespace(const char* text) noexcept {
    if (text == nullptr) return false;
    SkipEditorTextWhitespace(text);
    return *text == '\0';
}

bool EditorTextTokenBoundary(char value) noexcept {
    return value == '\0' ||
           std::isspace(static_cast<unsigned char>(value)) != 0;
}

bool ParseEditorTextInt(const char*& text, int& value) noexcept {
    SkipEditorTextWhitespace(text);
    if (*text == '\0') return false;
    errno = 0;
    char* end = nullptr;
    const long long parsed = std::strtoll(text, &end, 10);
    if (end == text || errno == ERANGE || !EditorTextTokenBoundary(*end) ||
        parsed < static_cast<long long>(INT_MIN) ||
        parsed > static_cast<long long>(INT_MAX)) {
        return false;
    }
    value = static_cast<int>(parsed);
    text = end;
    return true;
}

bool ParseEditorTextU32(const char*& text, u32& value) noexcept {
    SkipEditorTextWhitespace(text);
    if (*text == '\0' || *text == '-') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (end == text || errno == ERANGE || !EditorTextTokenBoundary(*end) ||
        parsed > static_cast<unsigned long long>(~u32{0})) {
        return false;
    }
    value = static_cast<u32>(parsed);
    text = end;
    return true;
}

bool ParseEditorTextFloat(const char*& text, f32& value) noexcept {
    SkipEditorTextWhitespace(text);
    if (*text == '\0') return false;
    errno = 0;
    char* end = nullptr;
    const f32 parsed = std::strtof(text, &end);
    if (end == text || errno == ERANGE || !EditorTextTokenBoundary(*end) ||
        !std::isfinite(static_cast<double>(parsed))) {
        return false;
    }
    value = parsed;
    text = end;
    return true;
}

bool ParseEditorTextWord(
    const char*& text, char* output, usize capacity) noexcept {
    SkipEditorTextWhitespace(text);
    const char* begin = text;
    while (*text != '\0' &&
           std::isspace(static_cast<unsigned char>(*text)) == 0) {
        ++text;
    }
    const usize length = static_cast<usize>(text - begin);
    if (length == 0u || length >= capacity) return false;
    std::memcpy(output, begin, length);
    output[length] = '\0';
    return true;
}

bool IsEditorTextDirective(const char* line, const char* name) noexcept {
    const usize length = std::strlen(name);
    return std::strncmp(line, name, length) == 0 &&
           EditorTextTokenBoundary(line[length]);
}

bool ParseEditorTextRemainder(
    const char*& text, usize maximum_length) noexcept {
    SkipEditorTextWhitespace(text);
    const usize length = std::strlen(text);
    return length > 0u && length <= maximum_length;
}

struct FValidatedEditorComponent {
    int node_id = -1;
    game::FTypeId type_id = 0;
};

struct FValidatedEditorProperty {
    int node_id = -1;
    u32 slot = 0u;
    u32 property = 0u;
};

struct FValidatedEditorPrefabLink {
    int node_id = -1;
    bool has_instance_id = false;
    char instance_id[game::kScene3DSerializePrefabInstanceIdBytes + 1u]{};
};

struct FValidatedEditor3DNode {
    int id = -1;
    int parent = -1;
    int parent_index = -1;
    u32 text_offset = 0u;
    u32 text_length = 0u;
    u32 depth = 0u;
    u8 state = 0u;
};

int FindValidatedEditor3DNode(
    const TArray<FValidatedEditor3DNode>& nodes, int id) noexcept {
    u32 first = 0u;
    u32 count = nodes.Num();
    while (count > 0u) {
        const u32 step = count / 2u;
        const u32 index = first + step;
        if (nodes[index].id < id) {
            first = index + 1u;
            count -= step + 1u;
        } else {
            count = step;
        }
    }
    return first < nodes.Num() && nodes[first].id == id
        ? static_cast<int>(first)
        : -1;
}

bool ValidateEditorScene2DText(const char* text) noexcept {
    if (text == nullptr) return false;

    // Reuse the runtime parser as the authoritative structural and known
    // directive preflight. It builds only into this isolated graph.
    game::ANode staging;
    const game::FSceneTextLoadResult parsed =
        game::TryLoadAcsceneText(text, staging);
    if (!parsed.Succeeded()) return false;

    const char* cursor = text;
    char line[game::kSceneTextMaxLineBytes + 1u]{};
    if (ReadEditorTextLine(cursor, line, static_cast<u32>(sizeof(line))) !=
            EEditorTextLineResult::Line ||
        std::strcmp(line, "ACSCENE v1") != 0) {
        return false;
    }
    if (ReadEditorTextLine(cursor, line, static_cast<u32>(sizeof(line))) !=
        EEditorTextLineResult::Line) {
        return false;
    }
    int node_count = 0;
    const char* values = line;
    if (!ParseEditorTextInt(values, node_count) || node_count < 0 ||
        !EditorTextOnlyWhitespace(values)) {
        return false;
    }
    for (int index = 0; index < node_count; ++index) {
        if (ReadEditorTextLine(
                cursor, line, static_cast<u32>(sizeof(line))) !=
            EEditorTextLineResult::Line) {
            return false;
        }
        const char* node_values = line;
        int id = -1;
        int parent = -1;
        f32 numeric[10]{};
        if (!ParseEditorTextInt(node_values, id) ||
            !ParseEditorTextInt(node_values, parent)) {
            return false;
        }
        for (f32& value : numeric) {
            if (!ParseEditorTextFloat(node_values, value)) return false;
        }
        SkipEditorTextWhitespace(node_values);
        if (std::strlen(node_values) >= sizeof(AEditorNode::name)) return false;
    }

    game::AcsRegisterEngineTypes();
    TArray<FValidatedEditorComponent> components;
    while (true) {
        const EEditorTextLineResult read = ReadEditorTextLine(
            cursor, line, static_cast<u32>(sizeof(line)));
        if (read == EEditorTextLineResult::End) break;
        if (read != EEditorTextLineResult::Line) return false;
        if (line[0] == '\0') continue;

        const char* known_directives[] = {
            "COMP", "CPROP", "SEL", "NFLG", "SPRT",
            "PFAB", "MAT", "POLY", "RPLY"
        };
        for (const char* directive : known_directives) {
            if (IsEditorTextDirective(line, directive) &&
                line[std::strlen(directive)] != ' ') {
                return false;
            }
        }

        if (IsEditorTextDirective(line, "COMP")) {
            int id = -1;
            char type_name[128]{};
            values = line + 4;
            if (!ParseEditorTextInt(values, id) ||
                !ParseEditorTextWord(
                    values, type_name, static_cast<usize>(sizeof(type_name))) ||
                !EditorTextOnlyWhitespace(values) ||
                staging.FindBySerialId(id) == nullptr) {
                return false;
            }
            const game::FTypeDesc* descriptor =
                game::CTypeRegistry::Get().FindByName(type_name);
            if (descriptor == nullptr ||
                descriptor->category != game::ETypeCategory::Component) {
                return false;
            }
            u32 count = 0u;
            for (u32 index = 0u; index < components.Num(); ++index) {
                if (components[index].node_id != id) continue;
                if (components[index].type_id == descriptor->id) return false;
                ++count;
            }
            if (count >= AEditorNode::kMaxComponents) return false;
            components.Add(FValidatedEditorComponent{id, descriptor->id});
            continue;
        }
        if (IsEditorTextDirective(line, "CPROP")) {
            int id = -1;
            u32 slot = 0u;
            u32 property = 0u;
            values = line + 5;
            if (!ParseEditorTextInt(values, id) ||
                !ParseEditorTextU32(values, slot) ||
                !ParseEditorTextU32(values, property) ||
                slot >= AEditorNode::kMaxComponents ||
                property >= AEditorNode::kMaxProps) {
                return false;
            }
            u32 component_slot = 0u;
            const FValidatedEditorComponent* target_component = nullptr;
            for (u32 index = 0u; index < components.Num(); ++index) {
                if (components[index].node_id != id) continue;
                if (component_slot == slot) {
                    target_component = &components[index];
                    break;
                }
                ++component_slot;
            }
            if (target_component == nullptr) return false;
            const game::FTypeDesc* descriptor =
                game::CTypeRegistry::Get().FindById(
                    target_component->type_id);
            if (descriptor == nullptr ||
                property >= descriptor->field_count) {
                return false;
            }
            continue; // canonical preflight validates the finite field values
        }

        if (IsEditorTextDirective(line, "SPRT") ||
            IsEditorTextDirective(line, "MAT")) {
            const bool sprite = IsEditorTextDirective(line, "SPRT");
            int id = -1;
            values = line + (sprite ? 4u : 3u);
            if (!ParseEditorTextInt(values, id) ||
                staging.FindBySerialId(id) == nullptr ||
                !ParseEditorTextRemainder(values, 255u)) {
                return false;
            }
            continue;
        }
        if (IsEditorTextDirective(line, "POLY")) {
            int id = -1;
            int count = 0;
            values = line + 4u;
            if (!ParseEditorTextInt(values, id) ||
                !ParseEditorTextInt(values, count) ||
                count < 3 ||
                count > static_cast<int>(game::kMaxPolyVerts)) {
                return false;
            }
            continue; // canonical preflight validates target and vertices
        }

        // SEL and PFAB are editor-only records. The canonical loader keeps
        // unknown records forward-compatible, so validate these known records
        // here rather than letting malformed variants silently degrade.
        if (IsEditorTextDirective(line, "SEL")) {
            int primary = -1;
            int count = 0;
            values = line + 3;
            if (!ParseEditorTextInt(values, primary) ||
                !ParseEditorTextInt(values, count) || count < 0 ||
                count > node_count) {
                return false;
            }
            TArray<int> selected_ids;
            for (int index = 0; index < count; ++index) {
                int selected = -1;
                if (!ParseEditorTextInt(values, selected) ||
                    staging.FindBySerialId(selected) == nullptr) {
                    return false;
                }
                for (u32 prior = 0; prior < selected_ids.Num(); ++prior) {
                    if (selected_ids[prior] == selected) return false;
                }
                selected_ids.Add(selected);
            }
            if (!EditorTextOnlyWhitespace(values)) return false;
            if (primary >= 0) {
                if (staging.FindBySerialId(primary) == nullptr) return false;
                bool primary_selected = false;
                for (u32 index = 0; index < selected_ids.Num(); ++index) {
                    if (selected_ids[index] == primary) {
                        primary_selected = true;
                        break;
                    }
                }
                if (!primary_selected) return false;
            } else if (count != 0) {
                return false;
            }
            continue;
        }
        if (IsEditorTextDirective(line, "PFAB")) {
            int id = -1;
            values = line + 4;
            if (!ParseEditorTextInt(values, id) ||
                staging.FindBySerialId(id) == nullptr ||
                !ParseEditorTextRemainder(
                    values, 255u)) {
                return false;
            }
        }
    }
    return true;
}

bool ValidateEditorScene3DText(const char* text) noexcept;
static int LoadScene3DTextImpl(
    FEditorHost* host, const char* text, bool clear,
    int idOffset, int reparentRootTo, int* out_root,
    bool prevalidated = false) noexcept;

/** SerializeScene のテキストからシーンを復元する (成功 1 / 失敗 0)。 */
int LoadSceneTextValidated(FEditorHost& h, const char* text) noexcept {
    if (text == nullptr) return 0;

    const char* p = text;
    char line[2048];   // RPLY (滑らか頂点 最大64) が 1 行に収まる長さ
    // 1 行読み出すローカルヘルパ ('\n' を消費、末尾 NUL 終端、続きがあれば true)。
    auto read_line = [&](char* out, int outsz) -> bool {
        if (*p == '\0') return false;
        int k = 0;
        while (*p != '\0' && *p != '\n') { if (k < outsz - 1) out[k++] = *p; ++p; }
        if (k > 0 && out[k - 1] == '\r') --k;
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
        load_id.Add(id); load_parent.Add(parent);
        ++created;
    }
    // 親付け fixup: 親が後から来て root 直下になった子を正しい親へ付け替える (Reparent は Local 保持)。
    bool reparented = false;
    for (u32 i = 0; i < load_id.Num(); ++i) {
        if (load_parent[i] < 0) continue;
        AEditorNode* node = FindNode(h, load_id[i]);
        AEditorNode* par  = FindNode(h, load_parent[i]);
        if (node != nullptr && par != nullptr && node->Parent() != static_cast<game::ANode*>(par)) {
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
                    if (std::sscanf(q, "%d%n", &sid, &c2) >= 1) { sel_ids.Add(sid); q += c2; }
                    else break;
                }
            }
            continue;
        }
        if (std::strncmp(line, "NFLG ", 5) == 0) {        // NFLG <id> <visible> <enabled> <sortLayer>
            int nid = 0, vis = 1, ena = 1, layer = 0;
            if (std::sscanf(line, "NFLG %d %d %d %d", &nid, &vis, &ena, &layer) >= 2) {
                AEditorNode* n = FindNode(h, nid);
                if (n != nullptr) { n->SetVisible(vis != 0); n->SetEnabled(ena != 0); n->SetDrawLayer(layer); }
            }
            continue;
        }
        if (std::strncmp(line, "SPRT ", 5) == 0) {        // SPRT <id> <utf8_path>
            int nid = 0, consumed = 0;
            if (std::sscanf(line, "SPRT %d %n", &nid, &consumed) >= 1) {
                const char* path = line + consumed;
                while (*path == ' ') ++path;
                AEditorNode* n = FindNode(h, nid);
                if (n != nullptr) {
                    std::snprintf(n->sprite_path, sizeof(n->sprite_path), "%s", path);
                    LoadNodeSprite(h, n);                 // device 未準備なら DrawScene で再試行
                }
            }
            continue;
        }
        if (std::strncmp(line, "PFAB ", 5) == 0) {        // PFAB <id> <utf8_path> (プレハブリンク)
            int nid = 0, consumed = 0;
            if (std::sscanf(line, "PFAB %d %n", &nid, &consumed) >= 1) {
                const char* path = line + consumed;
                while (*path == ' ') ++path;
                AEditorNode* n = FindNode(h, nid);
                if (n != nullptr) std::snprintf(n->prefab_src, sizeof(n->prefab_src), "%s", path);
            }
            continue;
        }
        if (std::strncmp(line, "MAT ", 4) == 0) {         // MAT <id> <utf8_path>
            int nid = 0, consumed = 0;
            if (std::sscanf(line, "MAT %d %n", &nid, &consumed) >= 1) {
                const char* path = line + consumed;
                while (*path == ' ') ++path;
                AEditorNode* n = FindNode(h, nid);
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
                AEditorNode* n = FindNode(h, nid);
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
                AEditorNode* n = FindNode(h, nid);
                if (n != nullptr && pc >= 3) {
                    if (pc > static_cast<int>(AEditorNode::kMaxRenderVerts)) pc = static_cast<int>(AEditorNode::kMaxRenderVerts);
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
        h.selection.Reset();
        for (u32 i = 0; i < sel_ids.Num(); ++i)
            if (FindNode(h, sel_ids[i]) != nullptr) h.selection.Add(sel_ids[i]);
        h.selected = (FindNode(h, sel_primary) != nullptr && SelContains(h, sel_primary))
                     ? sel_primary
                     : (h.selection.Num() > 0 ? h.selection[h.selection.Num() - 1] : -1);
    } else if (created > 0) {
        SelSet(h, h.nodes[0]->editor_id);
    }
    return 1;
}

int LoadSceneText(FEditorHost& h, const char* text) noexcept {
    return ValidateEditorScene2DText(text)
        ? LoadSceneTextValidated(h, text)
        : 0;
}

// ----- Copy / Paste (サブツリーのシリアライズ + id 再マップ) -----

/** node とその子孫を DFS で out へ集める (親が子より先)。 */
void CollectSubtree(AEditorNode* n, TArray<AEditorNode*>& out) noexcept {
    out.Add(n);
    for (u32 i = 0; i < n->ChildCount(); ++i)
        CollectSubtree(static_cast<AEditorNode*>(n->Child(i)), out);
}

/** root の subtree を scene_text へシリアライズして返す (root の親は -1、シーン全体と同じ行形式)。 */
const char* SerializeSubtree(FEditorHost& h, AEditorNode* root) noexcept {
    TArray<AEditorNode*> sub;
    CollectSubtree(root, sub);
    char* buf = h.scene_text;
    const int cap = static_cast<int>(sizeof(h.scene_text));
    int cur = std::snprintf(buf, static_cast<size_t>(cap), "ACSCENE v1\n%u\n", static_cast<u32>(sub.Num()));
    for (u32 i = 0; i < sub.Num() && cur < cap; ++i) {
        const AEditorNode* n = sub[i];
        const game::FTransform2D t = n->Local2D();
        const int pid = (n == root) ? -1 : ParentIdOf(h, n);
        const int w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur),
            "%d %d %.4f %.4f %.4f %.4f %.4f %.2f %.3f %.3f %.3f %.3f %s\n",
            n->editor_id, pid, t.position.x, t.position.y, t.rotation, t.scale.x, t.scale.y,
            n->base, n->color.x, n->color.y, n->color.z, n->color.w, n->name);
        if (w < 0 || w >= cap - cur) { buf[cap - 1] = '\0'; return buf; }
        cur += w;
    }
    for (u32 i = 0; i < sub.Num() && cur < cap; ++i) {
        const AEditorNode* n = sub[i];
        for (u32 c = 0; c < n->component_count && cur < cap; ++c) {
            const game::FTypeDesc* d = game::CTypeRegistry::Get().FindById(n->components[c]);
            if (d != nullptr && d->name != nullptr) {
                const int w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur), "COMP %d %s\n", n->editor_id, d->name);
                if (w < 0 || w >= cap - cur) { buf[cap - 1] = '\0'; return buf; }
                cur += w;
            }
        }
    }
    for (u32 i = 0; i < sub.Num() && cur < cap; ++i)
        cur = EmitCompProps(buf, cur, cap, sub[i]);
    // ノードフラグ (非既定のみ)。
    for (u32 i = 0; i < sub.Num() && cur < cap; ++i) {
        const AEditorNode* n = sub[i];
        if (!n->IsVisible() || !n->IsEnabled() || n->DrawLayer() != 0) {
            const int w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur), "NFLG %d %d %d %d\n",
                n->editor_id, n->IsVisible() ? 1 : 0, n->IsEnabled() ? 1 : 0, n->DrawLayer());
            if (w < 0 || w >= cap - cur) { buf[cap - 1] = '\0'; return buf; }
            cur += w;
        }
    }
    // スプライト画像パス。
    for (u32 i = 0; i < sub.Num() && cur < cap; ++i) {
        const AEditorNode* n = sub[i];
        if (n->sprite_path[0] != '\0') {
            const int w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur),
                                        "SPRT %d %s\n", n->editor_id, n->sprite_path);
            if (w < 0 || w >= cap - cur) { buf[cap - 1] = '\0'; return buf; }
            cur += w;
        }
    }
    for (u32 i = 0; i < sub.Num() && cur < cap; ++i) {   // プレハブリンク (copy/paste で維持)
        const AEditorNode* n = sub[i];
        if (n->prefab_src[0] != '\0') {
            const int w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur),
                                        "PFAB %d %s\n", n->editor_id, n->prefab_src);
            if (w < 0 || w >= cap - cur) { buf[cap - 1] = '\0'; return buf; }
            cur += w;
        }
    }
    for (u32 i = 0; i < sub.Num() && cur < cap; ++i) {   // 使用マテリアル
        const AEditorNode* n = sub[i];
        if (n->material_path[0] != '\0') {
            const int w = std::snprintf(buf + cur, static_cast<size_t>(cap - cur),
                                        "MAT %d %s\n", n->editor_id, n->material_path);
            if (w < 0 || w >= cap - cur) { buf[cap - 1] = '\0'; return buf; }
            cur += w;
        }
    }
    for (u32 i = 0; i < sub.Num() && cur < cap; ++i)   // カスタムポリゴン
        cur = EmitNodePoly(buf, cur, cap, sub[i]);
    if (cur >= cap) buf[cap - 1] = '\0';
    return buf;
}

/** subtree テキストを target_parent 配下へ貼り付ける (id を新規採番、内部の親子は再マップ)。新根 id / 失敗 -1。 */
int PasteSubtree(FEditorHost& h, const char* text, int target_parent) noexcept {
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
        for (u32 i = 0; i < oldIds.Num(); ++i) if (oldIds[i] == o) return newIds[i];
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
        oldIds.Add(oid);
        newIds.Add(nid);
        if (firstNew < 0) firstNew = nid;
    }
    while (read_line(line, sizeof(line))) {                    // COMP / CPROP を old_id→new_id で再マップ
        if (std::strncmp(line, "CPROP ", 6) == 0) {
            int onid = 0; unsigned slot = 0, prop = 0;
            float a = 0, b = 0, c = 0, e = 0;
            if (std::sscanf(line, "CPROP %d %u %u %f %f %f %f", &onid, &slot, &prop, &a, &b, &c, &e) >= 3) {
                const int nn = MapId(onid);
                if (nn >= 0) {
                    AEditorNode* node = FindNode(h, nn);
                    float v0 = a;
                    // ObjectRef プロパティは subtree 内を指す参照だけ new id へ付け替える
                    // (外部を指す参照は元のまま = prefab 内部リンクが複製後も保たれる)。
                    if (node != nullptr && slot < node->component_count) {
                        const game::FTypeDesc* d = game::CTypeRegistry::Get().FindById(node->components[slot]);
                        if (d != nullptr && prop < d->field_count
                            && d->fields[prop].kind == game::EFieldKind::ObjectRef) {
                            const int rm = MapId(static_cast<int>(a));
                            if (rm >= 0) v0 = static_cast<float>(rm);
                        }
                    }
                    SetCompProp(node, slot, prop, v0, b, c, e);
                }
            }
            continue;
        }
        if (std::strncmp(line, "NFLG ", 5) == 0) {            // NFLG <old_id> <vis> <ena> <layer>
            int onid = 0, vis = 1, ena = 1, layer = 0;
            if (std::sscanf(line, "NFLG %d %d %d %d", &onid, &vis, &ena, &layer) >= 2) {
                const int nn = MapId(onid);
                AEditorNode* node = (nn >= 0) ? FindNode(h, nn) : nullptr;
                if (node != nullptr) { node->SetVisible(vis != 0); node->SetEnabled(ena != 0); node->SetDrawLayer(layer); }
            }
            continue;
        }
        if (std::strncmp(line, "SPRT ", 5) == 0) {            // SPRT <old_id> <utf8_path>
            int onid = 0, consumed = 0;
            if (std::sscanf(line, "SPRT %d %n", &onid, &consumed) >= 1) {
                const char* path = line + consumed;
                while (*path == ' ') ++path;
                const int nn = MapId(onid);
                AEditorNode* node = (nn >= 0) ? FindNode(h, nn) : nullptr;
                if (node != nullptr) {
                    std::snprintf(node->sprite_path, sizeof(node->sprite_path), "%s", path);
                    LoadNodeSprite(h, node);
                }
            }
            continue;
        }
        if (std::strncmp(line, "PFAB ", 5) == 0) {            // PFAB <old_id> <utf8_path> (プレハブリンク)
            int onid = 0, consumed = 0;
            if (std::sscanf(line, "PFAB %d %n", &onid, &consumed) >= 1) {
                const char* path = line + consumed;
                while (*path == ' ') ++path;
                const int nn = MapId(onid);
                AEditorNode* node = (nn >= 0) ? FindNode(h, nn) : nullptr;
                if (node != nullptr) std::snprintf(node->prefab_src, sizeof(node->prefab_src), "%s", path);
            }
            continue;
        }
        if (std::strncmp(line, "MAT ", 4) == 0) {             // MAT <old_id> <utf8_path>
            int onid = 0, consumed = 0;
            if (std::sscanf(line, "MAT %d %n", &onid, &consumed) >= 1) {
                const char* path = line + consumed;
                while (*path == ' ') ++path;
                const int nn = MapId(onid);
                AEditorNode* node = (nn >= 0) ? FindNode(h, nn) : nullptr;
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
                AEditorNode* node = (nn >= 0) ? FindNode(h, nn) : nullptr;
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
                AEditorNode* node = (nn >= 0) ? FindNode(h, nn) : nullptr;
                if (node != nullptr && pc >= 3) {
                    if (pc > static_cast<int>(AEditorNode::kMaxRenderVerts)) pc = static_cast<int>(AEditorNode::kMaxRenderVerts);
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

// 3D シーンの直列化/復元 ABI の前方宣言 (スナップショットが 2D と 3D を一緒に積むため)。
ACS_EDITOR_API int acs_editor_scene3d_serialize(void* handle, char* out, int cap);
ACS_EDITOR_API int acs_editor_scene3d_load_text(void* handle, const char* text);

/** 現在のシーン (2D + 3D) をシリアライズして heap コピーを返す。
 *  形式 = «ACSSNAP3D <2D byte 長>\n» + «2D ACSCENE» + «3D ACS3D»。長さ前置なので 2D 本文に何が入っても誤分割しない
 *  (区切り文字探索だとノード名等に同じ文字列が来ると壊れる)。所有は呼び出し側、失敗 nullptr。 */
char* DupSnapshot(FEditorHost& h) noexcept {
    const char* s2d = SerializeScene(h);                   // h.scene_text (2D, 最大 64KB)
    const size_t l2d = std::strlen(s2d);
    char hdr[48];
    const int lh = std::snprintf(hdr, sizeof(hdr), "ACSSNAP3D %d\n", static_cast<int>(l2d));
    if (lh <= 0) return nullptr;
    constexpr int kInitial3DCapacity = 64 * 1024;
    constexpr int kMaximum3DCapacity = 256 * 1024 * 1024;
    int capacity3d = kInitial3DCapacity;
    int written3d = 0;
    char* tmp3d = nullptr;
    for (;;) {
        tmp3d = new (std::nothrow) char[static_cast<size_t>(capacity3d)];
        if (tmp3d == nullptr) return nullptr;
        written3d = acs_editor_scene3d_serialize(&h, tmp3d, capacity3d);
        if (written3d > 0 && written3d < capacity3d) break;
        delete[] tmp3d;
        tmp3d = nullptr;
        if (written3d <= 0 || capacity3d >= kMaximum3DCapacity) return nullptr;
        const int doubled = capacity3d <= kMaximum3DCapacity / 2
            ? capacity3d * 2
            : kMaximum3DCapacity;
        const int required = written3d < kMaximum3DCapacity ? written3d + 1
                                                            : kMaximum3DCapacity;
        capacity3d = std::max(doubled, required);
    }
    const size_t l3d = static_cast<size_t>(written3d);
    char* copy = new (std::nothrow) char[static_cast<size_t>(lh) + l2d + l3d + 1];   // 実サイズで確保 (固定 64KB の無駄/切詰を回避)
    if (copy == nullptr) { delete[] tmp3d; return nullptr; }
    std::memcpy(copy, hdr, static_cast<size_t>(lh));                                 // ヘッダ (2D 長)
    std::memcpy(copy + lh, s2d, l2d);                                                // 2D 部
    std::memcpy(copy + lh + l2d, tmp3d, l3d + 1);                                    // 3D 部 + null 終端
    delete[] tmp3d;
    return copy;
}

/** DupSnapshot のテキストから 2D + 3D を復元する。ヘッダが無ければ旧形式 (2D のみ) とみなす。
 *  text は破壊的に «2D 部» を終端して分割する (呼び出し側はこの後 text を解放する想定)。 */
void RestoreSnapshot(FEditorHost& h, char* text) noexcept {
    if (text == nullptr) return;
    int l2d = 0, consumed = 0;
    if (std::strncmp(text, "ACSSNAP3D ", 10) != 0) {
        LoadSceneText(h, text); return;                              // 後方互換: 2D のみの旧スナップ
    }
    if (std::sscanf(text, "ACSSNAP3D %d%n", &l2d, &consumed) < 1)
        return;                                                       // 壊れた新形式を legacy と誤認しない
    char* body = text + consumed;
    while (*body == '\n' || *body == '\r') ++body;                   // ヘッダ行の改行をスキップ → 2D 本文の先頭
    const size_t bodyLen = std::strlen(body);
    if (l2d <= 0 || static_cast<size_t>(l2d) > bodyLen) return;
    char* s3d = body + l2d;                                          // 2D は body から l2d バイト、その後が 3D
    const char saved = *s3d; *s3d = '\0';                            // 2D 部を一時終端
    const bool valid2d = ValidateEditorScene2DText(body);
    *s3d = saved;
    if (!valid2d || !ValidateEditorScene3DText(s3d)) return;
    *s3d = '\0';
    FSceneResourceRetirementScope retirement(h);
    LoadSceneTextValidated(h, body);                                 // 2D 復元
    *s3d = saved;
    LoadScene3DTextImpl(
        &h, s3d, /*clear=*/true, /*idOffset=*/0,
        /*reparentRootTo=*/-1, nullptr, /*prevalidated=*/true);
}

/** スナップショットスタックを空にして各 heap バッファを解放する。 */
void ClearStack(TArray<char*>& st) noexcept {
    for (u32 i = 0; i < st.Num(); ++i) delete[] st[i];
    st.Reset();
}

/** 変更前スナップショットを undo 履歴へ公開する。成功時だけ所有権を受け取る。 */
[[nodiscard]] bool CommitUndoSnapshot(FEditorHost& h, char* snapshot) noexcept {
    if (snapshot == nullptr) return false;
    if (h.suppress_undo) {
        delete[] snapshot;
        return true;
    }
    if (!h.undo.TryAdd(snapshot)) return false;
    ClearStack(h.redo);
    constexpr u32 kMaxUndo = 128;
    while (h.undo.Num() > kMaxUndo) {            // 上限超過は古いものから捨てる
        delete[] h.undo[0];
        for (u32 i = 1; i < h.undo.Num(); ++i) h.undo[i - 1] = h.undo[i];
        h.undo.Pop();
    }
    return true;
}

/** 変更操作の直前に呼ぶ: 現在状態を undo に積み、redo を破棄する。
 *  連続編集中 (suppress_undo) は積まない → ドラッグ 1 回が undo 1 ステップになる。 */
void PushUndo(FEditorHost& h) noexcept {
    if (h.suppress_undo) return;
    char* snap = DupSnapshot(h);
    if (snap == nullptr) return;
    if (!CommitUndoSnapshot(h, snap)) delete[] snap;
}

// world↔screen 変換 (DrawScene/PickNode/gizmo で共有)。
f32 W2SX(const FEditorHost& h, f32 wx) noexcept { return wx * h.cam_zoom + h.cam_pan_x; }
f32 W2SY(const FEditorHost& h, f32 wy) noexcept { return wy * h.cam_zoom + h.cam_pan_y; }
f32 S2WX(const FEditorHost& h, f32 sx) noexcept { return (h.cam_zoom != 0.0f) ? (sx - h.cam_pan_x) / h.cam_zoom : 0.0f; }
f32 S2WY(const FEditorHost& h, f32 sy) noexcept { return (h.cam_zoom != 0.0f) ? (sy - h.cam_pan_y) / h.cam_zoom : 0.0f; }

/** v を step の最も近い倍数に丸める (step<=0 はそのまま)。 */
f32 SnapTo(f32 v, f32 step) noexcept { return (step > 0.0f) ? std::round(v / step) * step : v; }

constexpr f32 kGizmoLen = 64.0f;   // ハンドルの画面長 (固定サイズ)

/** 選択ノードのギズモ中心 (screen)。選択無しは false。 */
bool GizmoCenter(FEditorHost& h, f32& cx, f32& cy) noexcept {
    if (h.selected < 0) return false;
    const AEditorNode* n = FindNode(h, h.selected);
    if (n == nullptr) return false;
    const game::FTransform2D w = n->World2D();
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
int GizmoHit(FEditorHost& h, f32 sx, f32 sy) noexcept {
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
void SetNodeWorldPosition(FEditorHost& h, AEditorNode* n, f32 wx, f32 wy) noexcept {
    game::ANode* parent = n->Parent();
    if (parent == nullptr || parent == h.root.Get()) {     // 親が root → local==world
        n->SetPosition2D(FVec2{ wx, wy });
        return;
    }
    const game::FTransform2D pw = parent->World2D();
    const f32 dx = wx - pw.position.x, dy = wy - pw.position.y;
    const f32 c = std::cos(-pw.rotation), s = std::sin(-pw.rotation);   // 親回転を打ち消す
    const f32 rx = dx * c - dy * s;
    const f32 ry = dx * s + dy * c;
    n->SetPosition2D(FVec2{ (pw.scale.x != 0.0f) ? rx / pw.scale.x : rx,
                            (pw.scale.y != 0.0f) ? ry / pw.scale.y : ry });
}

/** ノードの APrimitiveRenderer2D の shape (0=Box,1=Circle,2=Triangle) を返す (無ければ -1)。 */
int PrimitiveShape(const AEditorNode* n) noexcept {
    static game::FTypeId s_prId = 0;
    if (s_prId == 0) {
        const game::FTypeDesc* d = game::CTypeRegistry::Get().FindByName("APrimitiveRenderer2D");
        if (d != nullptr) s_prId = d->id;
    }
    if (s_prId == 0) return -1;
    for (u32 c = 0; c < n->component_count; ++c)
        if (n->components[c] == s_prId) return static_cast<int>(n->comp_props[c][0][0]);  // prop0 = shape
    return -1;
}

/** レンダラー未付与ノードの薄いギズモ (小さな菱形の枠 + 中心点)。選択はできるが「空」だと分かる。 */
void DrawEmptyGizmo(CSpriteBatch& sb, f32 cx, f32 cy, f32 alpha) noexcept {
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
    constexpr u32 kCap = AEditorNode::kMaxRenderVerts;
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
void DrawNodePolygon(FEditorHost& h, CSpriteBatch& sb, const AEditorNode* n,
                     const game::FTransform2D& w, FVec4 col) noexcept {
    const FVec2* verts; u32 vc;
    if (n->render_count >= 3)    { verts = n->render_verts; vc = n->render_count; }
    else if (n->poly_count >= 3) { verts = n->poly_verts;   vc = n->poly_count; }
    else return;
    const f32 c = std::cos(w.rotation), s = std::sin(w.rotation);
    FVec2 sv[AEditorNode::kMaxRenderVerts];
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
void DrawScene(FEditorHost& h, CSpriteBatch& sb, u32 w, u32 hh) noexcept {
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
    for (u32 i = 0; chrome && i < h.nodes.Num(); ++i) {
        AEditorNode* n = h.nodes[i];
        game::ANode* parent = n->Parent();
        if (parent == nullptr || parent == h.root.Get()) continue;
        const game::FTransform2D cw = n->World2D();
        const game::FTransform2D pw = parent->World2D();
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
    for (u32 i = 0; chrome && i < h.selection.Num(); ++i) {
        const AEditorNode* sn = FindNode(h, h.selection[i]);
        if (sn == nullptr) continue;
        const game::FTransform2D w = sn->World2D();
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

    // 2D 点光源を収集する (ALight2DComponent を持つノード)。lit (PBR) マテリアルの陰影付け用。
    // 位置/半径は screen px (DrawScene はノードを screen 空間に描くため)。
    FSpriteLight lights[16];
    u32 lightCount = 0;
    for (u32 i = 0; i < h.nodes.Num() && lightCount < 16; ++i) {
        const AEditorNode* n = h.nodes[i];
        const int ls = LightComponentSlot(n);
        if (ls < 0 || !n->IsVisible()) continue;
        const game::FTransform2D lw = n->World2D();
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
    // 影オクルーダーを収集する (AShadowCaster2DComponent を持つノードのシルエットを円近似)。
    // occNodeIdx[k] = オクルーダー k の元ノードの配列番号 (自己影スキップ用に描画側で逆引き)。
    FSpriteOccluder occluders[16];
    u32 occCount = 0;
    int occNodeIdx[16];
    bool occSelfShadow[16];          // 自己影あり (m_SelfShadow=1) のオクルーダーはスキップ番号を渡さない
    for (u32 i = 0; i < h.nodes.Num() && occCount < 16; ++i) {
        const AEditorNode* n = h.nodes[i];
        const int cs = ShadowCasterSlot(n);
        if (cs < 0 || !n->IsVisible()) continue;
        const game::FTransform2D ow = n->World2D();
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
    for (u32 i = 0; i < h.nodes.Num(); ++i) {
        AEditorNode* n = h.nodes[i];
        if (h.game_view && !n->IsVisible()) continue;   // ゲームでは非可視ノードは出さない (ゴースト無し)
        const game::FTransform2D w = n->World2D();
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
                // (影の中のノードが無灯火のまま明るく浮くのを防ぐ。ANode::DrawTree と同じ規律)。
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
                game::APrimitiveRenderer2D::DrawShape(sb, prShape, cx, cy, dw, dh, w.rotation, col);
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
    if (h.poly_drawing && h.poly_points.Num() > 0) {
        const FVec4 line{ 0.42f, 0.88f, 0.52f, 0.95f };
        const FVec4 dot{ 0.96f, 0.92f, 0.45f, 1.0f };
        u32 np = h.poly_points.Num();
        constexpr u32 kRV = AEditorNode::kMaxRenderVerts;
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
        for (u32 i = 0; i < h.poly_points.Num(); ++i) {          // アンカーマーカー
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
int PickNode(const FEditorHost& h, f32 screen_x, f32 screen_y) noexcept {
    if (h.cam_zoom == 0.0f) return -1;
    const f32 wx = (screen_x - h.cam_pan_x) / h.cam_zoom;
    const f32 wy = (screen_y - h.cam_pan_y) / h.cam_zoom;
    for (int i = static_cast<int>(h.nodes.Num()) - 1; i >= 0; --i) {   // topmost first
        const AEditorNode* n = h.nodes[static_cast<u32>(i)];
        const game::FTransform2D w = n->World2D();
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

// エンジンログをエディタの C# コンソールへ橋渡しする SPSC リング。
// producer = CLogger の writer スレッド (EditorLogSink 経由)、consumer = C# UI の poll。
namespace {
struct FLogRing {
    static constexpr unsigned N   = 512;    // 2 のべき乗
    static constexpr int      MSG = 240;
    struct FItem { int sev; char msg[MSG]; };
    FItem                  items[N];
    std::atomic<unsigned> head{0};   // consumer (poll)
    std::atomic<unsigned> tail{0};   // producer (writer thread)

    void push(int sev, const char* m) noexcept {
        const unsigned t = tail.load(std::memory_order_relaxed);
        const unsigned h = head.load(std::memory_order_acquire);
        if (t - h >= N) return;      // 満杯 → 破棄 (UI が追いつくまで古い順に欠落)
        FItem& it = items[t & (N - 1)];
        it.sev = sev;
        int i = 0;
        for (; m != nullptr && m[i] != '\0' && i < MSG - 1; ++i) it.msg[i] = m[i];
        it.msg[i] = '\0';
        tail.store(t + 1, std::memory_order_release);
    }
    bool pop(int& sev, char* out, int cap) noexcept {
        const unsigned h = head.load(std::memory_order_relaxed);
        const unsigned t = tail.load(std::memory_order_acquire);
        if (h == t) return false;
        FItem& it = items[h & (N - 1)];
        sev = it.sev;
        int i = 0;
        for (; it.msg[i] != '\0' && i < cap - 1; ++i) out[i] = it.msg[i];
        if (cap > 0) out[i] = '\0';
        head.store(h + 1, std::memory_order_release);
        return true;
    }
};
FLogRing g_log_ring;
void EditorLogSink(acs::ELogSeverity sev, const char* msg) noexcept {
    g_log_ring.push(static_cast<int>(sev), msg);
}
u64 NextNonZeroGeneration(std::atomic<u64>& counter) noexcept
{
    u64 value = counter.fetch_add(1u, std::memory_order_relaxed);
    if (value == 0u) {
        value = counter.fetch_add(1u, std::memory_order_relaxed);
    }
    return value;
}

void CopyServiceDiagnosticText(
    char* destination, usize capacity, const char* source) noexcept
{
    if (destination == nullptr || capacity == 0u) return;
    destination[0] = '\0';
    if (source == nullptr) return;
    const usize source_bytes = std::strlen(source);
    usize copy_bytes =
        std::min(source_bytes, capacity - 1u);
    if (copy_bytes < source_bytes) {
        // Do not leave a truncated multi-byte code point in an otherwise
        // strict UTF-8 ABI field. source[copy_bytes] is the first excluded
        // byte, so continuation bytes mean the preceding code point is split.
        while (copy_bytes > 0u &&
               (static_cast<unsigned char>(source[copy_bytes]) &
                0xC0u) == 0x80u) {
            --copy_bytes;
        }
    }
    if (copy_bytes != 0u) {
        std::memcpy(destination, source, copy_bytes);
    }
    destination[copy_bytes] = '\0';
}

void SetServiceDiagnosticStatus(
    editor_service_diagnostics::FDiagnostic& diagnostic,
    editor_service_diagnostics::EState state,
    editor_service_diagnostics::EReason reason,
    u32 flags,
    editor_service_diagnostics::EErrorDomain error_domain,
    editor_service_diagnostics::EErrorCode error_code,
    const char* message,
    const char* stable_code) noexcept
{
    diagnostic.state = static_cast<u32>(state);
    diagnostic.reason = static_cast<u32>(reason);
    diagnostic.flags = flags;
    diagnostic.error_domain = static_cast<u32>(error_domain);
    diagnostic.error_code = static_cast<i32>(error_code);
    CopyServiceDiagnosticText(
        diagnostic.message_utf8,
        editor_service_diagnostics::kMessageBytes,
        message);
    CopyServiceDiagnosticText(
        diagnostic.stable_code_utf8,
        editor_service_diagnostics::kStableCodeBytes,
        stable_code);
}

u64 RequiredCapabilityForService(u32 service) noexcept
{
    using editor_abi::CapabilityBit;
    using editor_abi::ECapability;
    using editor_service_diagnostics::EService;
    switch (static_cast<EService>(service)) {
    case EService::Profiler:
        return CapabilityBit(ECapability::ProfilerV5);
    case EService::VolumetricCloudWorkload:
        return CapabilityBit(
            ECapability::VolumetricCloudWorkloadV1);
    case EService::CameraViewRequests:
        return CapabilityBit(ECapability::CameraViewRequestsV1);
    default:
        return 0u;
    }
}

editor_service_diagnostics::FDiagnostic ResolveServiceDiagnostic(
    const FEditorHost* host,
    u32 service,
    u64 diagnostic_generation) noexcept
{
    using namespace editor_service_diagnostics;
    FDiagnostic diagnostic{};
    diagnostic.service = service;
    diagnostic.host_generation =
        host != nullptr ? host->abi_host_generation : 0u;
    diagnostic.diagnostic_generation = diagnostic_generation;

    const u64 required_capability = RequiredCapabilityForService(service);
    if (required_capability == 0u) {
        SetServiceDiagnosticStatus(
            diagnostic,
            EState::Disabled,
            EReason::UnknownService,
            0u,
            EErrorDomain::EditorAbi,
            EErrorCode::UnknownService,
            "The requested optional editor service is unknown.",
            "ACS.SERVICE.UNKNOWN");
        return diagnostic;
    }
    if ((editor_abi::kCapabilities & required_capability) == 0u) {
        SetServiceDiagnosticStatus(
            diagnostic,
            EState::Disabled,
            EReason::CapabilityNotAdvertised,
            0u,
            EErrorDomain::EditorAbi,
            EErrorCode::CapabilityNotAdvertised,
            "The native provider did not advertise this optional service.",
            "ACS.SERVICE.CAPABILITY_MISSING");
        return diagnostic;
    }
    if (host == nullptr) {
        SetServiceDiagnosticStatus(
            diagnostic,
            EState::Failed,
            EReason::InvalidHost,
            0u,
            EErrorDomain::EditorHost,
            EErrorCode::InvalidHost,
            "The editor host is null or no longer available.",
            "ACS.SERVICE.INVALID_HOST");
        return diagnostic;
    }

    switch (static_cast<EService>(service)) {
    case EService::Profiler:
        SetServiceDiagnosticStatus(
            diagnostic,
            EState::Enabled,
            EReason::None,
            Callable,
            EErrorDomain::None,
            EErrorCode::None,
            "Profiler snapshots are available.",
            "ACS.SERVICE.PROFILER.ENABLED");
        break;
    case EService::VolumetricCloudWorkload:
        if (host->startup_failed) {
            SetServiceDiagnosticStatus(
                diagnostic,
                EState::Failed,
                EReason::StartupFailed,
                0u,
                EErrorDomain::Renderer,
                EErrorCode::StartupFailed,
                "Renderer startup failed before cloud diagnostics became available.",
                "ACS.SERVICE.CLOUD.STARTUP_FAILED");
        } else if (!host->startup_ready) {
            SetServiceDiagnosticStatus(
                diagnostic,
                EState::Pending,
                EReason::StartupPending,
                Retryable,
                EErrorDomain::Renderer,
                EErrorCode::StartupPending,
                "Cloud diagnostics are waiting for incremental renderer startup.",
                "ACS.SERVICE.CLOUD.STARTUP_PENDING");
        } else if (!host->cloud_workload_available) {
            SetServiceDiagnosticStatus(
                diagnostic,
                EState::Inactive,
                EReason::SceneFeatureInactive,
                Callable | Retryable,
                EErrorDomain::Renderer,
                EErrorCode::SceneFeatureInactive,
                "Cloud diagnostics are callable, but no cloud workload is active.",
                "ACS.SERVICE.CLOUD.INACTIVE");
        } else {
            SetServiceDiagnosticStatus(
                diagnostic,
                EState::Enabled,
                EReason::None,
                Callable,
                EErrorDomain::None,
                EErrorCode::None,
                "Cloud workload snapshots are available.",
                "ACS.SERVICE.CLOUD.ENABLED");
        }
        break;
    case EService::CameraViewRequests:
        SetServiceDiagnosticStatus(
            diagnostic,
            EState::Enabled,
            EReason::None,
            Callable,
            EErrorDomain::None,
            EErrorCode::None,
            "Camera-view request scheduling is available.",
            "ACS.SERVICE.CAMERA_VIEWS.ENABLED");
        break;
    default:
        break;
    }
    return diagnostic;
}

} // namespace

/** エディタホスト間で共有する基盤の所有権と参照数。 */
struct FEditorSubsystemState {
    u32 host_count = 0;
    bool ready = false;
    bool closing = false;
    bool logger_owned = false;
    bool logger_sink_owned = false;
    bool memory_owned = false;
    bool thread_pool_owned = false;
};

FEditorSubsystemState g_subsystems;
std::atomic_flag g_subsystems_lock = ATOMIC_FLAG_INIT;

/** create/destroy の基盤遷移を直列化する短時間ロック。 */
struct FEditorSubsystemGuard {
    FEditorSubsystemGuard() noexcept
    {
        while (g_subsystems_lock.test_and_set(std::memory_order_acquire)) {}
    }
    ~FEditorSubsystemGuard() noexcept
    {
        g_subsystems_lock.clear(std::memory_order_release);
    }
};

/** ロガー/メモリ/スレッドプールを初回ホスト用に起動し、参照を 1 件取得する。 */
bool EnsureSubsystems() noexcept
{
    FEditorSubsystemGuard guard;
    if (g_subsystems.closing) return false;
    if (g_subsystems.ready) {
        ++g_subsystems.host_count;
        return true;
    }

    if (!CLogger::IsInitialized()) {
        FLogConfig lc{};
        lc.console = true;
        lc.debug_output = true;
        CLogger::Init(lc);
        g_subsystems.logger_owned = CLogger::IsInitialized();
    }
    if (g_subsystems.logger_owned) {
        CLogger::SetSink(&EditorLogSink);
        g_subsystems.logger_sink_owned = true;
    }

    if (CMemorySystem::Get(ESegment::Default) == nullptr) {
        const auto result = CMemorySystem::Init(CMemorySystem::DefaultConfig());
        if (result.IsErr() && CMemorySystem::Get(ESegment::Default) == nullptr) {
            if (g_subsystems.logger_sink_owned) CLogger::SetSink(nullptr);
            if (g_subsystems.logger_owned) CLogger::Shutdown();
            g_subsystems = FEditorSubsystemState{};
            return false;
        }
        g_subsystems.memory_owned = result.IsOk();
    }

    if (CThreadPool::WorkerCount() == 0) {
        const auto result = CThreadPool::Init(0);
        if (result.IsErr() && CThreadPool::WorkerCount() == 0) {
            if (g_subsystems.memory_owned) CMemorySystem::Shutdown();
            if (g_subsystems.logger_sink_owned) CLogger::SetSink(nullptr);
            if (g_subsystems.logger_owned) CLogger::Shutdown();
            g_subsystems = FEditorSubsystemState{};
            return false;
        }
        g_subsystems.thread_pool_owned = result.IsOk();
    }

    // カタログは固定長の静的レジストリなので、基盤を再起動しても登録内容は有効なまま残る。
    const u32 type_count = acs::game::AcsRegisterEngineTypes();
    g_subsystems.ready = true;
    g_subsystems.host_count = 1;
    ACS_LOG_INFO("[acs_editor_abi] subsystems initialized (%u reflected engine types)", type_count);
    return true;
}

/** 最終ホストの破棄時に、このABIが所有する基盤だけを逆順で停止する。 */
void ReleaseSubsystems() noexcept
{
    bool logger_owned = false;
    bool logger_sink_owned = false;
    bool memory_owned = false;
    bool thread_pool_owned = false;
    {
        FEditorSubsystemGuard guard;
        if (!g_subsystems.ready || g_subsystems.host_count == 0) return;
        if (--g_subsystems.host_count != 0) return;
        // 時間のかかる join/flush 中はロックを保持しない。closing 中の新規 create は失敗させる。
        g_subsystems.ready = false;
        g_subsystems.closing = true;
        logger_owned = g_subsystems.logger_owned;
        logger_sink_owned = g_subsystems.logger_sink_owned;
        memory_owned = g_subsystems.memory_owned;
        thread_pool_owned = g_subsystems.thread_pool_owned;
    }

    ACS_LOG_INFO("[acs_editor_abi] subsystems shutting down");
    if (logger_sink_owned) CLogger::SetSink(nullptr);
    if (thread_pool_owned) CThreadPool::Shutdown();
    if (memory_owned) CMemorySystem::Shutdown();
    if (logger_owned) {
        CLogger::Flush();
        CLogger::Shutdown();
    }
    {
        FEditorSubsystemGuard guard;
        g_subsystems = FEditorSubsystemState{};
    }
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

// =============================================================================
// 3D ビューポート: 軌道カメラ + ライト付きプリミティブ + グリッド
// =============================================================================

/** 3D 描画リソース (シェーダ / デバッグ線 / プリミティブメッシュ) を遅延初期化する。 */
// 3D メッシュの自前ライティング HLSL。FGpuMesh の DrawIndexed はエディタ深度面コンテキストで
// 描画が出ないため、DebugDraw3D と同じ «動的 VB + 非インデックス Draw» 方式を採る。頂点は既に
// ワールド座標 + 法線 + 色 (CPU で展開済み) なので model 行列は不要。
// 物理ベース (Cook-Torrance) BRDF: GGX 法線分布 + Smith 幾何 + Fresnel-Schlick。
// アルベド=頂点色、金属度/粗さは既定 (誘電体 metallic=0, roughness=0.5)。1 方向光 + 簡易拡散環境光。
const char* kMesh3DHLSL = R"(
#pragma pack_matrix(row_major)
cbuffer Frame : register(b0) {
    float4x4 view_proj;
    float4   light_dir;    // xyz=surface→light, w=ベース環境光 (ギズモ用フォールバック)
    float4   light_col;    // rgb=太陽 radiance
    float4   cam_pos;      // xyz=カメラ world 位置, w=影を受けるか(1) / ギズモ等は 0
    float4   sky_zenith;   // IBL: 空のグラデーション (環境光源)。0 ならフラット環境光のみ。
    float4   sky_horizon;
    float4   sky_ground;
    float4x4 light_vp;     // 光源の view-projection (シャドウマップ空間へ)
};
Texture2D    shadow_map   : register(t0);
SamplerState shadow_map_sampler : register(s0);
struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float3 col : COLOR; float2 mat : TEXCOORD; };
struct VSOut { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : NORMAL; float3 col : COLOR; float2 mat : TEXCOORD1; };
VSOut VSMain(VSIn v) {
    VSOut o;
    o.pos  = mul(float4(v.pos, 1.0), view_proj);   // 頂点は既にワールド座標
    o.wpos = v.pos;
    o.nrm  = v.nrm;
    o.col  = v.col;
    o.mat  = v.mat;                                 // x=metallic, y=roughness
    return o;
}
static const float PI = 3.14159265359;
float3 SkyCol(float3 d) {   // 空の放射輝度 (スカイと同じグラデーション)。IBL の環境光源に使う。
    float t = d.y;
    float3 sky_color = lerp(
        sky_horizon.rgb, sky_ground.rgb, saturate(-t * 1.6));
    if (t >= 0.0) {
        sky_color = lerp(
            sky_horizon.rgb, sky_zenith.rgb, pow(saturate(t), 0.55));
    }
    return sky_color;
}
float3 ACESFilm(float3 x) { return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)); } // フィルミック
float3 FresnelSchlick(float cosT, float3 F0) { return F0 + (1.0 - F0) * pow(saturate(1.0 - cosT), 5.0); }
float3 FresnelRough(float cosT, float3 F0, float rough) {   // 環境鏡面用 (粗さで地平 F を抑える)
    float3 m = max((1.0 - rough).xxx, F0);
    return F0 + (m - F0) * pow(saturate(1.0 - cosT), 5.0);
}
float DistGGX(float ndh, float rough) {
    float a = rough * rough; float a2 = a * a;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-5);
}
float GeomSchlickGGX(float nv, float k) { return nv / (nv * (1.0 - k) + k); }
float GeomSmith(float ndv, float ndl, float rough) {
    float k = rough + 1.0; k = (k * k) / 8.0;     // 直接光 (Disney) の k
    return GeomSchlickGGX(ndv, k) * GeomSchlickGGX(ndl, k);
}
float ShadowFactor(float3 wpos, float ndl) {   // 1=lit, 0=影。3x3 PCF。
    float4 lp = mul(float4(wpos, 1.0), light_vp);
    float3 ndc = float3(0.0, 0.0, 0.0);
    float shadow_factor = 1.0;
    bool projection_valid = abs(lp.w) > 1.0e-5;
    if (projection_valid) {
        ndc = lp.xyz / lp.w;
    }
    bool inside_shadow =
        projection_valid &&
        ndc.x >= -1.0 && ndc.x <= 1.0 &&
        ndc.y >= -1.0 && ndc.y <= 1.0 &&
        ndc.z >= 0.0 && ndc.z <= 1.0;
    if (inside_shadow) {
        float2 uv = float2(
            ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);
        float bias = max(
            0.0012 * (1.0 - ndl), 0.0004);   // 法線依存バイアス (シャドウアクネ回避)
        float ts = 1.0 / 2048.0;
        float lit = 0.0;
        [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            float sd = shadow_map.SampleLevel(
                shadow_map_sampler, uv + float2(x, y) * ts, 0).r;
            lit += (sd + bias >= ndc.z) ? 1.0 : 0.0;
        }
        shadow_factor = lit / 9.0;
    }
    return shadow_factor;
}
float4 PSMain(VSOut v) : SV_TARGET {
    float3 N = normalize(v.nrm);
    float3 V = normalize(cam_pos.xyz - v.wpos);
    float3 L = normalize(light_dir.xyz);
    float3 H = normalize(V + L);
    float3 albedo   = v.col;
    float  metallic = saturate(v.mat.x);           // マテリアル: 頂点から (per-node)
    float  rough    = clamp(v.mat.y, 0.04, 1.0);    // マテリアル: 頂点から (per-node)
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float ndl = max(dot(N, L), 0.0);
    float ndv = max(dot(N, V), 0.0);
    float ndh = max(dot(N, H), 0.0);
    float vdh = max(dot(V, H), 0.0);
    // 直接光 (太陽、Cook-Torrance)。
    float  D = DistGGX(ndh, rough);
    float  G = GeomSmith(ndv, ndl, rough);
    float3 F = FresnelSchlick(vdh, F0);
    float3 spec = (D * G) * F / max(4.0 * ndv * ndl, 1e-4);
    float3 kd = (1.0 - F) * (1.0 - metallic);
    float  shadow = (cam_pos.w > 0.5) ? ShadowFactor(v.wpos, ndl) : 1.0;   // 影を受けるメッシュのみ (ギズモは 0)
    float3 Lo = (kd * albedo / PI + spec) * light_col.rgb * ndl * shadow;
    // 環境光 (IBL): 法線方向の空 = 拡散照度、反射方向の空 = 鏡面反射。フラット環境光が «死んだ» 影を解消。
    float3 R   = reflect(-V, N);
    float3 Fr  = FresnelRough(ndv, F0, rough);
    float3 kdA = (1.0 - Fr) * (1.0 - metallic);
    float3 ambient = light_dir.w * albedo                  // ベース環境光 (ギズモ等、sky=0 のとき)
                   + kdA * albedo * SkyCol(N)              // IBL 拡散 (環境照度)
                   + Fr  * SkyCol(R);                       // IBL 鏡面 (環境反射)
    float3 col = (ambient + Lo) * 0.78;                     // 露出 (やや絞ってコントラストを出す)
    return float4(col, 1.0);                                // Phase2: 線形出力 (CPostProcess が一度だけ ACES+ガンマ)
}
)";

struct FM3DFrame { FMat4 view_proj; FVec4 light_dir; FVec4 light_col; FVec4 cam_pos; FVec4 sky_zenith, sky_horizon, sky_ground; FMat4 light_vp; };

// 法線 G-buffer プリパス: M3DVtx (既に world 座標+法線) を world normal として RGBA16F に出す。
// CSsao が GTAO の slice 計算に world normal を要求する (depth 微分法線はブロック状になるため)。
const char* kNormalGBuf3DHLSL = R"(
#pragma pack_matrix(row_major)
cbuffer NFrame : register(b0) { float4x4 view_proj; };
struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float3 col : COLOR; float2 mat : TEXCOORD; };
struct VSOut { float4 pos : SV_POSITION; float3 nrm : NORMAL; };
VSOut VSMain(VSIn v) { VSOut o; o.pos = mul(float4(v.pos, 1.0), view_proj); o.nrm = v.nrm; return o; }
float4 PSMain(VSOut i) : SV_TARGET { return float4(normalize(i.nrm), 1.0); }   // world-space normal
)";

// シャドウ・キャスター: M3DVtx (既に world 座標) を «光の view-projection» でクリップへ。depth-only。
const char* kShadowCaster3DHLSL = R"(
#pragma pack_matrix(row_major)
cbuffer LightFrame : register(b0) { float4x4 light_vp; };
float4 VSMain(float3 pos : POSITION) : SV_POSITION { return mul(float4(pos, 1.0), light_vp); }
)";

// スプライト: テクスチャ付きクアッド (フラット・アルファブレンド)。2D 内容を 3D シーンに描く。
const char* kSprite3DHLSL = R"(
#pragma pack_matrix(row_major)
cbuffer Frame : register(b0) { float4x4 view_proj; float4 light_dir; float4 light_col; };
Texture2D    albedo : register(t0);
SamplerState albedo_sampler : register(s0);
struct VSIn  { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(VSIn v) { VSOut o; o.pos = mul(float4(v.pos, 1.0), view_proj); o.uv = v.uv; return o; }
float4 PSMain(VSOut v) : SV_TARGET {
    float4 c = albedo.Sample(albedo_sampler, v.uv);
    if (c.a < 0.02) discard;                 // 完全透明は捨てる (深度書き込み防止)
    return c;                                // テクスチャは sRGB 値そのまま (UNORM RT へ)
}
)";
// フルスクリーン複製: opaque HDR シーンを refr_bg へコピー (屈折オブジェクトが背景として sample)。
// 頂点バッファ不要 (SV_VertexID の大三角形)、深度オフ。
const char* kBlit3DHLSL = R"(
Texture2D    srcTex         : register(t0);
SamplerState srcTex_sampler : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o; o.uv = uv;
    o.pos = float4(uv.x * 2.0 - 1.0, -(uv.y * 2.0 - 1.0), 0.0, 1.0);
    return o;
}
float4 PSMain(VSOut v) : SV_TARGET { return srcTex.Sample(srcTex_sampler, v.uv); }
)";

// 被写界深度 (DoF): signed CoC と画素単位 Vogel disk を使う gather blur。
// 深度/CoC の不連続を拒否して前景・背景の色漏れを抑え、画面端は反復 sample せず正規化する。
const char* kDof3DHLSL = R"(
Texture2D    sceneTex         : register(t0);
SamplerState sceneTex_sampler : register(s0);
Texture2D    depthTex         : register(t1);
SamplerState depthTex_sampler : register(s1);
cbuffer DOFCB : register(b0) {
    float4 dofp;   // x=focus_dist(view z), y=focus_range, z=max_blur(px), w=near
    float4 dofp2;  // x=far, yz=texel size, w=orthographic
};
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o; o.uv = uv;
    o.pos = float4(uv.x * 2.0 - 1.0, -(uv.y * 2.0 - 1.0), 0.0, 1.0);
    return o;
}
float Linearize(float ndc, float nearZ, float farZ, float ortho) {
    float perspectiveZ = (nearZ * farZ) / max(farZ - ndc * (farZ - nearZ), 1e-4);
    float orthoZ = lerp(nearZ, farZ, ndc);
    return lerp(perspectiveZ, orthoZ, saturate(ortho));
}
float ViewZAt(float2 uv, float nearZ, float farZ) {
    float ndc = depthTex.SampleLevel(depthTex_sampler, uv, 0.0).r;
    return Linearize(ndc, nearZ, farZ, dofp2.w);
}
float SignedCoC(float viewZ) {
    return clamp((viewZ - dofp.x) / max(dofp.y, 1e-3), -1.0, 1.0);
}
float InsideViewport(float2 uv) {
    return step(0.0, uv.x) * step(uv.x, 1.0) * step(0.0, uv.y) * step(uv.y, 1.0);
}
float4 PSMain(VSOut v) : SV_TARGET {
    float nearZ = dofp.w, farZ = dofp2.x;
    float centerZ = ViewZAt(v.uv, nearZ, farZ);
    float centerCoC = SignedCoC(centerZ);
    float radiusPx = abs(centerCoC) * max(dofp.z, 0.0);
    float3 center =
        sceneTex.SampleLevel(sceneTex_sampler, v.uv, 0.0).rgb;
    if (radiusPx < 0.5) return float4(center, 1.0);

    // Vogel disk は «画素» で円形に作り、最後に texel size で UV へ変換する。
    // これにより解像度とアスペクト比が変わっても bokeh が楕円化しない。
    const int N = 32;
    const float GA = 2.39996323;
    float3 sum = center * 1.5;
    float wsum = 1.5;
    float depthTolerance = max(0.035, centerZ * 0.018);
    // Keep the fixed 32-tap quality budget, but leave the loop rolled. Forcing
    // the compiler to clone this depth-aware gather made warm-up block the owner
    // thread without changing the samples or their accumulation order.
    [loop] for (int i = 0; i < N; ++i) {
        float rPx = sqrt((float(i) + 0.5) / float(N)) * radiusPx;
        float a = float(i) * GA;
        float2 sampleUv = v.uv + float2(cos(a), sin(a)) * rPx * dofp2.yz;
        if (InsideViewport(sampleUv) < 0.5) continue;                 // clamp edge の反復による筋を防ぐ

        float sampleZ = ViewZAt(sampleUv, nearZ, farZ);
        float sampleCoC = SignedCoC(sampleZ);
        float sameLayer = 1.0;
        if (centerCoC > 0.0 && sampleZ < centerZ - depthTolerance) sameLayer = 0.0; // 背景へ前景を漏らさない
        if (centerCoC < 0.0 && sampleZ > centerZ + depthTolerance) sameLayer = 0.0; // 前景へ背景を漏らさない
        sameLayer *= step(-0.01, centerCoC * sampleCoC);             // 焦点面を跨ぐ sample を拒否

        // sample 自身の bokeh disk が現在の gather 点まで届く場合だけ採用。
        // sharp sample を大半径へ引き延ばす halo / bleed を抑える。
        float coverage = saturate(abs(sampleCoC) * dofp.z - rPx + 1.25);
        float depthWeight = exp2(-abs(sampleZ - centerZ) / depthTolerance * 2.0);
        float w = sameLayer * coverage * depthWeight;
        if (w > 1e-4) {
            sum += sceneTex.SampleLevel(
                sceneTex_sampler, sampleUv, 0.0).rgb * w;
            wsum += w;
        }
    }
    return float4(sum / wsum, 1.0);
}
)";

// god rays (光芒/crepuscular rays): 太陽スクリーン位置へ向かって bright pass を放射状に march し、
// scene depth の保守的な sky mask で遮蔽物を安定化。範囲外 sample は捨てて edge streak を防ぐ。
const char* kGodRays3DHLSL = R"(
Texture2D    sceneTex         : register(t0);
SamplerState sceneTex_sampler : register(s0);
Texture2D    depthTex         : register(t1);
SamplerState depthTex_sampler : register(s1);
cbuffer GRCB : register(b0) {
    float4 grp;    // xy=sun_uv, z=intensity, w=decay
    float4 grp2;   // x=density(全体の歩幅), y=weight(1タップ重み), z=bright threshold
    float4 grp3;   // xy=texel size, z=sky depth threshold, w=radial extent
};
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o; o.uv = uv;
    o.pos = float4(uv.x * 2.0 - 1.0, -(uv.y * 2.0 - 1.0), 0.0, 1.0);
    return o;
}
float InsideViewport(float2 uv) {
    return step(0.0, uv.x) * step(uv.x, 1.0) * step(0.0, uv.y) * step(uv.y, 1.0);
}
float SkyVisibility(float2 uv) {
    // min-depth 5 tap は geometry を 1px 弱だけ保守的に広げ、細い遮蔽物の点滅と光漏れを抑える。
    float d = depthTex.SampleLevel(depthTex_sampler, uv, 0.0).r;
    d = min(d, depthTex.SampleLevel(depthTex_sampler, uv + float2( grp3.x, 0.0), 0.0).r);
    d = min(d, depthTex.SampleLevel(depthTex_sampler, uv + float2(-grp3.x, 0.0), 0.0).r);
    d = min(d, depthTex.SampleLevel(depthTex_sampler, uv + float2(0.0,  grp3.y), 0.0).r);
    d = min(d, depthTex.SampleLevel(depthTex_sampler, uv + float2(0.0, -grp3.y), 0.0).r);
    return smoothstep(grp3.z, min(1.0, grp3.z + 0.00035), d);
}
float4 PSMain(VSOut v) : SV_TARGET {
    float3 scene = sceneTex.Sample(sceneTex_sampler, v.uv).rgb;
    const int N = 64;
    float2 delta = (v.uv - grp.xy) * (grp2.x / float(N));   // 太陽へ向かう 1 ステップ
    float2 uv = v.uv;
    float  illum = 1.0;
    float3 shaft = float3(0, 0, 0);
    float  wsum  = 0.0;
    [loop] for (int i = 0; i < N; ++i) {
        uv -= delta;
        if (InsideViewport(uv) < 0.5) break;                // clamp edge を繰返し sample して伸びる筋を防ぐ
        float3 s   = sceneTex.SampleLevel(sceneTex_sampler, uv, 0.0).rgb;
        float  lum = dot(s, float3(0.2126, 0.7152, 0.0722));
        float  bright = saturate((lum - grp2.z) / max(lum, 1e-3)); // energy-bounded soft bright pass
        float  source = bright * SkyVisibility(uv);
        shaft += s * source * illum;
        wsum  += illum;
        illum *= saturate(grp.w);
    }
    // decay 合計で正規化し、weight を明示的な散乱エネルギーとして使う。
    shaft *= grp2.y / max(wsum, 1e-3);
    float aspect = grp3.y / max(grp3.x, 1e-6);
    float radial = length((v.uv - grp.xy) * float2(aspect, 1.0));
    float envelope = saturate(1.0 - radial / max(grp3.w, 0.1));
    envelope *= envelope;
    return float4(scene + shaft * grp.z * envelope, 1.0);
}
)";

// モーションブラー: CMotionVector の motion (prev_uv - curr_uv、UV 空間) に沿って scene を多タップ平均。
// 画素単位 cap、深度/速度 bilateral、同一深度 velocity dilation で境界 bleed と大速度の縞を抑える。
const char* kMotionBlur3DHLSL = R"(
Texture2D    sceneTex          : register(t0);
SamplerState sceneTex_sampler  : register(s0);
Texture2D    motionTex         : register(t1);
SamplerState motionTex_sampler : register(s1);
Texture2D    depthTex          : register(t2);
SamplerState depthTex_sampler  : register(s2);
cbuffer MBCB : register(b0) {
    float4 mbp;   // x=frame-normalized intensity, yz=texel size, w=max blur px
    float4 mbp2;  // x=near, y=far, z=relative depth tolerance, w=orthographic
};
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o; o.uv = uv;
    o.pos = float4(uv.x * 2.0 - 1.0, -(uv.y * 2.0 - 1.0), 0.0, 1.0);
    return o;
}
float Linearize(float ndc) {
    float perspectiveZ = (mbp2.x * mbp2.y) / max(mbp2.y - ndc * (mbp2.y - mbp2.x), 1e-4);
    return lerp(perspectiveZ, lerp(mbp2.x, mbp2.y, ndc), saturate(mbp2.w));
}
float ViewZAt(float2 uv) {
    return Linearize(
        depthTex.SampleLevel(depthTex_sampler, uv, 0.0).r);
}
float InsideViewport(float2 uv) {
    return step(0.0, uv.x) * step(uv.x, 1.0) * step(0.0, uv.y) * step(uv.y, 1.0);
}
float2 ClampVelocity(float2 vel) {
    float2 velPx = vel / max(mbp.yz, float2(1e-6, 1e-6));
    float speedPx = length(velPx);
    velPx *= min(1.0, mbp.w / max(speedPx, 1e-4));
    return velPx * mbp.yz;
}
float4 PSMain(VSOut v) : SV_TARGET {
    float3 center =
        sceneTex.SampleLevel(sceneTex_sampler, v.uv, 0.0).rgb;
    float centerZ = ViewZAt(v.uv);
    float depthTolerance = max(0.025, centerZ * mbp2.z);

    // 近傍で同一深度面に属する最大 velocity を選び、細い動体の内部 hole だけを埋める。
    // 深度が違う前景/背景の velocity は採用しないので輪郭を跨ぐ streak を作らない。
    float2 vel =
        motionTex.SampleLevel(motionTex_sampler, v.uv, 0.0).xy;
    float bestSpeed = length(vel / mbp.yz);
    float2 suv = v.uv + float2(mbp.y, 0.0);
    float sz = ViewZAt(suv);
    float2 sv = motionTex.SampleLevel(
        motionTex_sampler, suv, 0.0).xy;
    float ss = length(sv / mbp.yz); if (abs(sz - centerZ) <= depthTolerance && ss > bestSpeed) { vel = sv; bestSpeed = ss; }
    suv = v.uv - float2(mbp.y, 0.0);
    sz = ViewZAt(suv);
    sv = motionTex.SampleLevel(motionTex_sampler, suv, 0.0).xy;
    ss = length(sv / mbp.yz); if (abs(sz - centerZ) <= depthTolerance && ss > bestSpeed) { vel = sv; bestSpeed = ss; }
    suv = v.uv + float2(0.0, mbp.z);
    sz = ViewZAt(suv);
    sv = motionTex.SampleLevel(motionTex_sampler, suv, 0.0).xy;
    ss = length(sv / mbp.yz); if (abs(sz - centerZ) <= depthTolerance && ss > bestSpeed) { vel = sv; bestSpeed = ss; }
    suv = v.uv - float2(0.0, mbp.z);
    sz = ViewZAt(suv);
    sv = motionTex.SampleLevel(motionTex_sampler, suv, 0.0).xy;
    ss = length(sv / mbp.yz); if (abs(sz - centerZ) <= depthTolerance && ss > bestSpeed) { vel = sv; }

    vel = ClampVelocity(vel * mbp.x);
    float2 velPx = vel / mbp.yz;
    float speedPx = length(velPx);
    if (speedPx < 0.5) return float4(center, 1.0);

    // 決定的・対称 tap。フレーム毎の乱数を使わないため TAA 下でも粒状にちらつかない。
    const int N = 17;
    float3 col = center * 1.25;
    float wsum = 1.25;
    // Preserve all 17 symmetric taps and their deterministic order while
    // avoiding owner-thread shader-compiler expansion during editor startup.
    [loop] for (int i = 0; i < N; ++i) {
        float t = float(i) / float(N - 1) - 0.5;
        float2 sampleUv = v.uv + vel * t;
        if (InsideViewport(sampleUv) < 0.5) continue;
        float sampleZ = ViewZAt(sampleUv);
        float depthWeight = exp2(-abs(sampleZ - centerZ) / depthTolerance * 3.0);
        float2 sampleVelPx = motionTex.SampleLevel(
            motionTex_sampler, sampleUv, 0.0).xy / mbp.yz;
        float velocityWeight = exp2(-length(sampleVelPx - velPx / max(mbp.x, 1e-3)) /
                                    max(3.0, speedPx * 0.75));
        float tapWeight = 1.0 - abs(t) * 1.35;
        float w = max(tapWeight, 0.08) * depthWeight * velocityWeight;
        col += sceneTex.SampleLevel(
            sceneTex_sampler, sampleUv, 0.0).rgb * w;
        wsum += w;
    }
    return float4(col / wsum, 1.0);
}
)";

// スカイボックス: フルスクリーン三角形でカメラレイ方向から天空グラデーションを描く。
// 頂点バッファ不要 (SV_VertexID)。深度オフ・最背面 (メッシュが上に描かれる)。
const char* kSky3DHLSL = R"(
#pragma pack_matrix(row_major)
cbuffer Sky : register(b0) {
    float4x4 inv_view_proj;   // クリップ→ワールド (シーンと «同じ» 射影で視線を再構成 → 太陽/空がシーンと一致)
    float4   zenith;          // rgb = 天頂色
    float4   horizon;         // rgb = 地平色
    float4   ground;          // rgb = 地面側色
    float4   sun;             // xyz = 太陽方向 (world)
};
struct VSOut { float4 pos : SV_POSITION; float2 ndc : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);   // (0,0)(2,0)(0,2)
    VSOut o;
    o.ndc = uv * 2.0 - 1.0;                       // -1..3 の三角形
    o.pos = float4(o.ndc, 1.0, 1.0);             // z=1 (最遠)
    return o;
}
float4 PSMain(VSOut v) : SV_TARGET {
    // クリップ空間の near/far をワールドへ逆射影し、その差を視線方向とする (透視/正射の両方で正しい)。
    float4 wn = mul(float4(v.ndc, 0.0, 1.0), inv_view_proj); wn /= wn.w;
    float4 wf = mul(float4(v.ndc, 1.0, 1.0), inv_view_proj); wf /= wf.w;
    float3 dir = normalize(wf.xyz - wn.xyz);
    float  t = dir.y;
    float3 sky;
    if (t >= 0.0) sky = lerp(horizon.rgb, zenith.rgb, pow(saturate(t), 0.55));   // 地平→天頂
    else          sky = lerp(horizon.rgb, ground.rgb,  saturate(-t * 1.6));       // 地平→地面側
    float  sd = max(dot(dir, normalize(sun.xyz)), 0.0);
    sky += float3(1.0, 0.94, 0.80) * (pow(sd, 600.0) * 2.2 + pow(sd, 40.0) * 0.35 + pow(sd, 8.0) * 0.10); // 太陽 (円盤 + 控えめハロ)
    return float4(sky, 1.0);   // Phase2: 線形出力 (CPostProcess が一度だけ tonemap。Reinhard も除去)
}
)";

struct FSkyCb { FMat4 inv_view_proj; FVec4 zenith, horizon, ground, sun; };  // 128 bytes

// 無限グリッド: y=0 (ortho では z=0) の «視点中心の大クアッド» をフラグメントで格子化。
// fwidth でアンチエイリアス線幅を一定に、距離フェードで無限に見せる。minor=1単位 / major=10単位 + 軸色。
const char* kGrid3DHLSL = R"(
#pragma pack_matrix(row_major)
cbuffer Grid : register(b0) {
    float4x4 view_proj;
    float4   gctr;   // xy=フェード中心(グリッド平面の2軸), z=フェード距離, w=ortho(1:z=0平面 / 0:y=0平面)
};
struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float3 col : COLOR; };
struct VSOut { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; };
VSOut VSMain(VSIn v) { VSOut o; o.pos = mul(float4(v.pos, 1.0), view_proj); o.wpos = v.pos; return o; }
float GridAA(float2 c, float scale) {
    float2 cc = c / scale;
    float2 footprint = max(fwidth(cc), 1e-6);
    float2 distanceToLine = abs(frac(cc - 0.5) - 0.5);
    float2 lineCoverage = 1.0 - saturate(distanceToLine / footprint);
    // Once a cell projects below the Nyquist limit, keeping a one-pixel line
    // turns every fragment into grid coverage and produces a dotted horizon.
    // Fade each line family independently; the 10-unit major grid naturally
    // remains visible ten times farther than the minor grid.
    float2 frequencyFade = 1.0 - smoothstep(0.25, 0.50, footprint);
    return max(lineCoverage.x * frequencyFade.x,
               lineCoverage.y * frequencyFade.y);
}
float4 PSMain(VSOut v) : SV_TARGET {
    float2 coord = (gctr.w > 0.5) ? v.wpos.xy : v.wpos.xz;
    float  minor = GridAA(coord, 1.0);
    float  major = GridAA(coord, 10.0);
    float3 col = lerp(float3(0.42,0.44,0.50), float3(0.60,0.62,0.70), major);
    float  a   = max(minor * 0.32, major * 0.70);
    float2 d   = fwidth(coord);
    float  axisV = 1.0 - min(abs(coord.x) / max(d.x, 1e-6), 1.0);  // coord.x=0 の線
    float  axisH = 1.0 - min(abs(coord.y) / max(d.y, 1e-6), 1.0);  // coord.y=0 の線 (X 軸)
    if (axisH > a) { a = axisH; col = float3(0.90, 0.38, 0.40); }                                         // X 軸 = 赤
    if (axisV > a) { a = axisV; col = (gctr.w > 0.5) ? float3(0.42,0.85,0.40) : float3(0.36,0.56,0.95); } // ortho:Y緑 / persp:Z青
    float dist = length(gctr.xy - coord);
    float fade = saturate(1.0 - dist / gctr.z); fade *= fade;
    // Alpha reaches zero continuously. A hard discard threshold converts the
    // projected radial fade boundary into isolated on/off pixels at the horizon.
    return float4(col, saturate(a * fade));
}
)";
struct FGridCb { FMat4 view_proj; FVec4 gctr; };

/** 空グラデーション (CSky と同じ y-up) を SH9 放射輝度係数へ射影する (CPbrShader::SetSh9 用)。
 *  Sh9Irradiance が内部でコサイン畳み込みするので «放射輝度» を渡す。irradiance≈π×平均輝度に
 *  なるため入力放射輝度は控えめ。係数順序 [0]Y00 [1]y [2]z [3]x [4]xy [5]yz [6]Y20 [7]xz [8]x²-y²
 *  はシェーダの Sh9Irradiance に一致させる。 */
// TAA 用 Halton(2,3) low-discrepancy ジッタ列 (16 サンプル)。各成分 [0,1) を返す。
inline float HaltonSeq(u32 i, u32 base) noexcept {
    float f = 1.0f, r = 0.0f;
    while (i > 0) { f /= static_cast<float>(base); r += f * static_cast<float>(i % base); i /= base; }
    return r;
}
// view_proj (DirectXMath 行ベクトル規約: clip = world · VP) に «サブピクセル NDC ジッタ» を加える。
// clip.x += jx*clip.w となるよう column0 に jx*column3 を、column1 に jy*column3 を足す。
inline FMat4 ApplyTaaJitter(FMat4 vp, float jx, float jy) noexcept {
    for (int k = 0; k < 4; ++k) { vp.m[k][0] += jx * vp.m[k][3]; vp.m[k][1] += jy * vp.m[k][3]; }
    return vp;
}

// SH9 環境光の強さ。実際の空色 (CSky と同じ・従来のダミー色より明るい) を使うので控えめに絞り、
// «暖色の直射 vs 青い陰» のコントラスト=立体感を残す (直射は 3 点ライトが確保)。鏡面 fallback も兼ねる。
constexpr float kSh9Ambient = 0.55f;

// 物理大気 LUT は線形 HDR 輝度を返すため、表示輝度は露出と tonemap だけで調整する。
constexpr float kAtmosScale = 1.0f;
constexpr float kDefaultConfiguredSunIntensity = 2.35f;
constexpr float kAtmosphereSunRadianceAtDefault = 22.0f;

float PhysicalAtmosphereSunRadiance(float configured_intensity) noexcept {
    const float intensity = std::fmax(configured_intensity, 0.0f);
    return kAtmosphereSunRadianceAtDefault *
           (intensity / kDefaultConfiguredSunIntensity);
}

// Real solar angular radius (0.2666 degrees).  The atmosphere environment map
// intentionally excludes the analytic disc; it is evaluated in the final
// skybox pass at viewport resolution and remains behind volumetric clouds.
constexpr float kPhysicalSunAngularRadius = 0.004653f;
constexpr float kPhysicalSunDiscRadianceScale = 30.0f;
// Match the 1024 cubemap's angular detail with a 2:1 source.  The former
// 512x256 map had only 0.703 degrees per latitude row; magnifying those rows
// around the equirectangular pole produced visible concentric rings at zenith.
constexpr u32 kPhysicalSkyEquirectWidth = 2048u;
constexpr u32 kPhysicalSkyEquirectHeight = 1024u;
// GPU generation is the production path.  If shader dispatch/readback is not
// available, bound the synchronous CPU fallback so opening a project cannot
// spend hundreds of millions of nested atmosphere samples on the UI thread.
constexpr u32 kPhysicalSkyCpuFallbackWidth = 512u;
constexpr u32 kPhysicalSkyCpuFallbackHeight = 256u;

void ComputeSkySh9(FVec4 out[9], FVec3 zenith, FVec3 horizon, FVec3 ground) noexcept {
    // 空の放射輝度 (linear) を SH9 へ射影。zenith/horizon/ground は «実際に描く CSky と同じ色» を渡し、
    // 環境光(拡散)と鏡面反射(Sh9Radiance)を背景の空と一致させる。時間帯プリセットにも追従する。
    for (int i = 0; i < 9; ++i) out[i] = FVec4{ 0, 0, 0, 0 };
    const int N = 2048; const float PI = 3.14159265f;
    for (int k = 0; k < N; ++k) {
        const float t = (k + 0.5f) / static_cast<float>(N);
        const float z = 1.0f - 2.0f * t;
        const float r = std::sqrt(std::fmax(0.0f, 1.0f - z * z));
        const float phi = 2.0f * PI * static_cast<float>(k) * 0.6180339887f;
        const float x = r * std::cos(phi), y = r * std::sin(phi);   // y = up
        FVec3 c;
        if (y >= 0.0f) c = horizon + (zenith - horizon) * std::pow(std::fmin(1.0f, y), 0.55f);
        else           c = horizon + (ground - horizon) * std::fmin(1.0f, -y * 1.6f);
        const float b[9] = {
            0.282095f, 0.488603f * y, 0.488603f * z, 0.488603f * x,
            1.092548f * x * y, 1.092548f * y * z, 0.315392f * (3.0f * z * z - 1.0f),
            1.092548f * x * z, 0.546274f * (x * x - y * y)
        };
        for (int i = 0; i < 9; ++i) { out[i].x += c.x * b[i]; out[i].y += c.y * b[i]; out[i].z += c.z * b[i]; }
    }
    const float w = 4.0f * PI / static_cast<float>(N);
    for (int i = 0; i < 9; ++i) { out[i].x *= w; out[i].y *= w; out[i].z *= w; }
}

/** Compile only raw-DX12 CSky bytecode away from the window owner thread. */
void SkyCompileWorkerEntry(void* user) noexcept {
    auto& h = *static_cast<FEditorHost*>(user);
    const editor_profiler::FTimePoint begin =
        editor_profiler::CClock::now();
    auto result = CSky::CompileShadersCpu();
    const bool ok = result.IsOk();
    if (ok) {
        h.startup_sky_shaders = Move(result.Value());
    } else {
        ACS_LOG_ERROR(
            "[3D] CSky async CPU compile failed: %s",
            result.Error().message);
    }
    h.startup_worker_elapsed_ms =
        editor_profiler::ElapsedMilliseconds(begin);
    h.startup_worker_state.store(ok ? 2 : -1,
                                 std::memory_order_release);
}

bool BeginSkyCompileWorker(FEditorHost& h) noexcept {
    if (h.startup_worker.Joinable() ||
        h.startup_worker_state.load(std::memory_order_acquire) != 0) {
        return false;
    }

    h.startup_worker_kind = 3u;
    h.startup_worker_elapsed_ms = 0.0f;
    h.startup_worker_state.store(1, std::memory_order_release);
    FThreadConfig config{};
    config.name = L"ACS sky shader compile";
    auto worker_result =
        FThread::Spawn(&SkyCompileWorkerEntry, &h, config);
    if (worker_result.IsErr()) {
        h.startup_worker_kind = 0u;
        h.startup_worker_state.store(0, std::memory_order_release);
        ACS_LOG_ERROR(
            "[acs_editor_abi] failed to create CSky CPU compile worker: %s",
            worker_result.Error().message);
        return false;
    }
    h.startup_worker = Move(worker_result.Value());
    return true;
}

/**
 * Build the complete unpublished raw-DX12 PBR candidate away from the window
 * owner thread. The candidate is never read until PollStartupWorker performs
 * an acquire and joins this thread.
 */
void PbrCompileWorkerEntry(void* user) noexcept {
    auto& h = *static_cast<FEditorHost*>(user);
    const editor_profiler::FTimePoint begin =
        editor_profiler::CClock::now();
    auto shaders = CPbrShader::CompileShadersCpu(true);
    bool ok = shaders.IsOk() &&
              h.startup_pbr_candidate_device != nullptr;
    if (ok) {
        auto result =
            h.pbr3d.BuildInitializedCandidateForRawDx12(
                *h.startup_pbr_candidate_device,
                Move(shaders.Value()),
                h.startup_pbr_candidate_rt_format,
                h.startup_pbr_candidate_depth_format,
                ECullMode::None);
        ok = result.IsOk();
        if (!ok) {
            ACS_LOG_ERROR(
                "[3D] CPbrShader background candidate creation failed: %s",
                result.Error().message);
        }
    } else if (shaders.IsErr()) {
        ACS_LOG_ERROR(
            "[3D] CPbrShader async CPU compile failed: %s",
            shaders.Error().message);
    } else {
        ACS_LOG_ERROR(
            "[3D] CPbrShader background candidate has no device");
    }
    h.startup_worker_elapsed_ms =
        editor_profiler::ElapsedMilliseconds(begin);
    h.startup_worker_state.store(ok ? 2 : -1,
                                 std::memory_order_release);
}

bool BeginPbrCompileWorker(
    FEditorHost& h, IRhiDevice& device,
    EFormat rt_format, EFormat depth_format) noexcept {
    if (h.startup_worker.Joinable() ||
        h.startup_worker_state.load(std::memory_order_acquire) != 0) {
        return false;
    }

    h.startup_worker_kind = 1u;
    h.startup_worker_elapsed_ms = 0.0f;
    h.startup_pbr_candidate_device = &device;
    h.startup_pbr_candidate_rt_format = rt_format;
    h.startup_pbr_candidate_depth_format = depth_format;
    h.startup_worker_state.store(1, std::memory_order_release);
    FThreadConfig config{};
    config.name = L"ACS PBR shader compile";
    auto worker_result =
        FThread::Spawn(&PbrCompileWorkerEntry, &h, config);
    if (worker_result.IsErr()) {
        h.startup_pbr_candidate_device = nullptr;
        h.startup_worker_kind = 0u;
        h.startup_worker_state.store(0, std::memory_order_release);
        ACS_LOG_ERROR(
            "[acs_editor_abi] failed to create PBR CPU compile worker: %s",
            worker_result.Error().message);
        return false;
    }
    h.startup_worker = Move(worker_result.Value());
    return true;
}

/** Compile only raw-DX12 SSGI bytecode away from the window owner thread. */
void SsgiCompileWorkerEntry(void* user) noexcept {
    auto& h = *static_cast<FEditorHost*>(user);
    const editor_profiler::FTimePoint begin =
        editor_profiler::CClock::now();
    auto result = CSsgi::CompileShadersCpu();
    const bool ok = result.IsOk();
    if (ok) {
        h.startup_ssgi_shaders = Move(result.Value());
    } else {
        ACS_LOG_ERROR(
            "[3D] CSsgi async CPU compile failed: %s",
            result.Error().message);
    }
    h.startup_worker_elapsed_ms =
        editor_profiler::ElapsedMilliseconds(begin);
    h.startup_worker_state.store(ok ? 2 : -1,
                                 std::memory_order_release);
}

bool BeginSsgiCompileWorker(FEditorHost& h) noexcept {
    if (h.startup_worker.Joinable() ||
        h.startup_worker_state.load(std::memory_order_acquire) != 0) {
        return false;
    }

    h.startup_worker_kind = 2u;
    h.startup_worker_elapsed_ms = 0.0f;
    h.startup_worker_state.store(1, std::memory_order_release);
    FThreadConfig config{};
    config.name = L"ACS SSGI shader compile";
    auto worker_result =
        FThread::Spawn(&SsgiCompileWorkerEntry, &h, config);
    if (worker_result.IsErr()) {
        h.startup_worker_kind = 0u;
        h.startup_worker_state.store(0, std::memory_order_release);
        ACS_LOG_ERROR(
            "[acs_editor_abi] failed to create SSGI CPU compile worker: %s",
            worker_result.Error().message);
        return false;
    }
    h.startup_worker = Move(worker_result.Value());
    return true;
}

/**
 * Build the complete unpublished raw-DX12 SSSS pipeline candidate away from
 * the window owner thread. Full-resolution targets stay deferred because they
 * depend on the current viewport and are staged by the render pump.
 */
void SsssCompileWorkerEntry(void* user) noexcept {
    auto& h = *static_cast<FEditorHost*>(user);
    const editor_profiler::FTimePoint begin =
        editor_profiler::CClock::now();
    auto shaders = CSubsurfaceScattering::CompileShadersCpu();
    bool ok = shaders.IsOk() &&
              h.startup_ssss_candidate_device != nullptr;
    if (ok) {
        auto result =
            h.ssss3d.BuildPipelineCandidateForRawDx12(
                *h.startup_ssss_candidate_device,
                Move(shaders.Value()));
        ok = result.IsOk() && h.ssss3d.HasPipelineResources();
        if (!ok) {
            ACS_LOG_ERROR(
                "[3D] SSSS background pipeline candidate creation failed: %s",
                result.IsErr() ? result.Error().message :
                                 "incomplete pipeline resources");
        }
    } else if (shaders.IsErr()) {
        ACS_LOG_ERROR(
            "[3D] SSSS async CPU compile failed: %s",
            shaders.Error().message);
    } else {
        ACS_LOG_ERROR(
            "[3D] SSSS background pipeline candidate has no device");
    }
    h.startup_worker_elapsed_ms =
        editor_profiler::ElapsedMilliseconds(begin);
    h.startup_worker_state.store(
        ok ? 2 : -1, std::memory_order_release);
}

bool BeginSsssCompileWorker(
    FEditorHost& h, IRhiDevice& device) noexcept {
    if (h.startup_worker.Joinable() ||
        h.startup_worker_state.load(std::memory_order_acquire) != 0) {
        return false;
    }

    h.startup_worker_kind = 6u;
    h.startup_worker_elapsed_ms = 0.0f;
    h.startup_ssss_candidate_device = &device;
    h.startup_worker_state.store(1, std::memory_order_release);
    FThreadConfig config{};
    config.name = L"ACS SSSS shader compile";
    auto worker_result =
        FThread::Spawn(&SsssCompileWorkerEntry, &h, config);
    if (worker_result.IsErr()) {
        h.startup_ssss_candidate_device = nullptr;
        h.startup_worker_kind = 0u;
        h.startup_worker_state.store(0, std::memory_order_release);
        ACS_LOG_ERROR(
            "[acs_editor_abi] failed to create SSSS CPU compile worker: %s",
            worker_result.Error().message);
        return false;
    }
    h.startup_worker = Move(worker_result.Value());
    return true;
}

/** Compile raw-DX12 post-process bytecode off the window owner thread. */
void PostCompileWorkerEntry(void* user) noexcept {
    auto& h = *static_cast<FEditorHost*>(user);
    const editor_profiler::FTimePoint begin =
        editor_profiler::CClock::now();
    auto result = CPostProcess::CompileShadersCpu();
    const bool ok = result.IsOk();
    if (ok) {
        h.startup_post_shaders = Move(result.Value());
    } else {
        ACS_LOG_ERROR(
            "[3D] CPostProcess async CPU compile failed: %s",
            result.Error().message);
    }
    h.startup_worker_elapsed_ms =
        editor_profiler::ElapsedMilliseconds(begin);
    h.startup_worker_state.store(
        ok ? 2 : -1, std::memory_order_release);
}

bool BeginPostCompileWorker(FEditorHost& h) noexcept {
    if (h.startup_worker.Joinable() ||
        h.startup_worker_state.load(std::memory_order_acquire) != 0) {
        return false;
    }

    h.startup_worker_kind = 7u;
    h.startup_worker_elapsed_ms = 0.0f;
    h.startup_worker_state.store(1, std::memory_order_release);
    FThreadConfig config{};
    config.name = L"ACS post shader compile";
    auto worker_result =
        FThread::Spawn(&PostCompileWorkerEntry, &h, config);
    if (worker_result.IsErr()) {
        h.startup_worker_kind = 0u;
        h.startup_worker_state.store(0, std::memory_order_release);
        ACS_LOG_ERROR(
            "[acs_editor_abi] failed to create post CPU compile worker: %s",
            worker_result.Error().message);
        return false;
    }
    h.startup_worker = Move(worker_result.Value());
    return true;
}

/** Compile only raw-DX12 volumetric-cloud bytecode off the owner thread. */
void CloudCompileWorkerEntry(void* user) noexcept {
    auto& h = *static_cast<FEditorHost*>(user);
    const editor_profiler::FTimePoint begin =
        editor_profiler::CClock::now();
    auto result = CVolumetricClouds::CompileShadersCpu();
    const bool ok = result.IsOk();
    if (ok) {
        h.startup_cloud_shaders = Move(result.Value());
    } else {
        ACS_LOG_ERROR(
            "[3D] CVolumetricClouds async CPU compile failed: %s",
            result.Error().message);
    }
    h.startup_worker_elapsed_ms =
        editor_profiler::ElapsedMilliseconds(begin);
    h.startup_worker_state.store(ok ? 2 : -1,
                                 std::memory_order_release);
}

bool BeginCloudCompileWorker(FEditorHost& h) noexcept {
    if (h.startup_worker.Joinable() ||
        h.startup_worker_state.load(std::memory_order_acquire) != 0) {
        return false;
    }

    h.startup_worker_kind = 4u;
    h.startup_worker_elapsed_ms = 0.0f;
    h.startup_worker_state.store(1, std::memory_order_release);
    FThreadConfig config{};
    config.name = L"ACS cloud shader compile";
    auto worker_result =
        FThread::Spawn(&CloudCompileWorkerEntry, &h, config);
    if (worker_result.IsErr()) {
        h.startup_worker_kind = 0u;
        h.startup_worker_state.store(0, std::memory_order_release);
        ACS_LOG_ERROR(
            "[acs_editor_abi] failed to create cloud CPU compile worker: %s",
            worker_result.Error().message);
        return false;
    }
    h.startup_worker = Move(worker_result.Value());
    return true;
}

/** Return 0 while running, 1 on success, or -1 on failure. */
i32 PollStartupWorker(FEditorHost& h) noexcept {
    const i32 state =
        h.startup_worker_state.load(std::memory_order_acquire);
    if (state == 1) return 0;
    if (state != 2 && state != -1) return -1;
    h.startup_worker.Join();
    h.startup_worker_state.store(0, std::memory_order_release);
    return state == 2 ? 1 : -1;
}

void JoinStartupWorker(FEditorHost& h) noexcept {
    h.startup_worker.Join();
    h.startup_pbr_candidate_device = nullptr;
    h.startup_ssss_candidate_device = nullptr;
    h.startup_worker_kind = 0u;
    h.startup_worker_state.store(0, std::memory_order_release);
}

/**
 * Quiesce raw startup work before scene-owned resources are retired.
 *
 * Core startup products (PBR/sky/cloud/post) are renderer-global, so a worker
 * that completed while joined remains in its terminal state for the normal
 * startup state machine to publish. Runtime SSGI/SSSS jobs are scene-demand
 * dependent and are discarded so the replacement scene can request them
 * afresh without retaining an unpublished candidate.
 */
void JoinSceneReplacementStartupWorker(FEditorHost& h) noexcept {
    const u32 worker_kind = h.startup_worker_kind;
    if (worker_kind == 0u && !h.startup_worker.Joinable()) return;

    h.startup_worker.Join();
    (void)h.startup_worker_state.load(std::memory_order_acquire);
    h.startup_pbr_candidate_device = nullptr;
    h.startup_ssss_candidate_device = nullptr;

    if (worker_kind == 2u) {
        h.startup_ssgi_shaders = {};
        h.ssgi_init_tried = false;
        h.startup_worker_kind = 0u;
        h.startup_worker_state.store(0, std::memory_order_release);
    } else if (worker_kind == 6u) {
        h.ssss3d.Shutdown();
        h.ssss3d_pending_shaders = {};
        h.ssss3d_ready = false;
        h.ssss3d_init_failed = false;
        h.ssss3d_init_state = 0u;
        h.ssss_pending_diffuse_rt.Reset();
        h.ssss_pending_w = 0u;
        h.ssss_pending_h = 0u;
        h.ssss_frame_resource_state = 0u;
        h.ssss_frame_failed_w = 0u;
        h.ssss_frame_failed_h = 0u;
        h.startup_worker_kind = 0u;
        h.startup_worker_state.store(0, std::memory_order_release);
    } else if (worker_kind == 0u) {
        // Defensive recovery for a joinable handle without a registered owner.
        h.startup_worker_state.store(0, std::memory_order_release);
    }
}

void BeginSceneResourceRetirement(FEditorHost& h) noexcept {
    ++h.scene_resource_retirement_depth;
    if (h.scene_resource_retirement_depth != 1u) return;

    // Required lifetime order: no candidate may still be touching the device
    // when WaitIdle establishes the retirement fence, and no scene graph or
    // sprite/material resource is destroyed until after that fence.
    JoinSceneReplacementStartupWorker(h);
    if (IRhiDevice* device = h.renderer.Device(); device != nullptr) {
        device->WaitIdle();
    }
}

void Pass_AtmosphereIbl(FEditorHost& h, IRhiCommandList* cl) noexcept;

TSharedPtr<AMeshAsset> MakeEditorWaterGrid(u32 cells = 64u) noexcept {
    if (cells < 2u) cells = 2u;
    if (cells > 256u) cells = 256u;
    auto mesh = MakeShared<AMeshAsset>();
    if (!mesh) return nullptr;
    auto& vertices = mesh->Vertices();
    auto& indices = mesh->Indices();
    const u32 row = cells + 1u;
    for (u32 z = 0u; z <= cells; ++z) {
        const f32 v = static_cast<f32>(z) / static_cast<f32>(cells);
        for (u32 x = 0u; x <= cells; ++x) {
            const f32 u = static_cast<f32>(x) / static_cast<f32>(cells);
            vertices.Add(FMeshVertex{
                FVec3{u - 0.5f, 0.0f, v - 0.5f},
                FVec3::UnitY(), u, v});
        }
    }
    for (u32 z = 0u; z < cells; ++z) {
        for (u32 x = 0u; x < cells; ++x) {
            const u32 a = z * row + x;
            const u32 b = a + 1u;
            const u32 c = a + row;
            const u32 d = c + 1u;
            // +Y authored normal with the engine's local XZ plane convention.
            indices.Add(a); indices.Add(c); indices.Add(b);
            indices.Add(b); indices.Add(c); indices.Add(d);
        }
    }
    mesh->SubMeshes().Add(
        FSubMesh{0u, static_cast<u32>(indices.Num())});
    return mesh;
}

/** Advance exactly one 3D resource-initialization phase.
 *
 * Owner-thread RHI creation is split across native render-pump messages;
 * expensive raw-DX12 bytecode compilation may run on the CPU-only startup
 * worker. A false result with an incremented r3d_init_phase means "more work
 * remains"; false without progress means the current phase failed.
 */
bool AdvanceEnsure3D(FEditorHost& h) noexcept {
    if (h.r3d_ready) return true;
    if (h.r3d_init_failed) return false;
    h.startup_phase_pending = false;
    IRhiDevice* dev = h.renderer.Device();
    if (dev == nullptr) return false;
    const EFormat cf = h.renderer.ColorFormat();
    const EFormat df = h.renderer.DepthFormat();
    // 3D シーンは «線形 HDR RT» に描いて CPostProcess(ACES) で一度だけ tonemap する。
    // そのため色を出すパイプは全て HDR フォーマットで作る (cf=backbuffer は CPostProcess の出力のみ)。
    const EFormat hdrf = EFormat::R16G16B16A16_Float;
    if (h.r3d_init_phase == 0u) {
    if (h.dbg3d.Init(*dev, hdrf).IsErr()) { ACS_LOG_ERROR("[3D] DebugDraw3D init 失敗"); return false; }
    if (h.camera_frustum_dbg3d.Init(*dev, cf).IsErr()) {
        ACS_LOG_ERROR("[3D] camera frustum DebugDraw3D init 失敗");
        return false;
    }

    FShaderDesc vs{}; vs.stage = EShaderStage::Vertex; vs.hlsl_source = kMesh3DHLSL; vs.entry_point = "VSMain"; vs.debug_name = "Mesh3D.VS";
    FShaderDesc ps{}; ps.stage = EShaderStage::Pixel;  ps.hlsl_source = kMesh3DHLSL; ps.entry_point = "PSMain"; ps.debug_name = "Mesh3D.PS";
    auto vr = CreateRhiShader(*dev, vs); auto pr = CreateRhiShader(*dev, ps);
    if (vr.IsErr() || pr.IsErr()) { ACS_LOG_ERROR("[3D] mesh シェーダ生成失敗"); return false; }
    h.m3d_vs = Move(vr.Value()); h.m3d_ps = Move(pr.Value());

    FPipelineDesc pd{};
    pd.vs = h.m3d_vs.Get(); pd.ps = h.m3d_ps.Get();
    pd.topology     = EPrimitiveTopology::TriangleList;
    pd.rt_format    = hdrf;
    pd.depth_format = df;
    pd.depth_test   = true; pd.depth_write = true;     // 動的 VB 方式なら深度も効く (重なり解決)
    pd.cull_mode    = ECullMode::None;          // 表裏どちらの巻きでも確実に出す
    pd.blend_mode   = EBlendMode::Opaque;
    pd.cbuffer_slots = 1; pd.cbuffer_names[0] = "Frame";
    pd.texture_slots = 1; pd.texture_names[0] = "shadow_map";   // t0 = シャドウ深度 (PCF サンプル)
    pd.static_sampler_count = 1;
    pd.static_samplers[0].filter    = ESamplerFilter::Linear;
    pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
    pd.static_samplers[0].address_w = ESamplerAddress::Clamp;
    pd.vertex_stride = sizeof(FM3DVtx);
    pd.layout[0] = { "POSITION", 0, EFormat::R32G32B32_Float, 0  };
    pd.layout[1] = { "NORMAL",   0, EFormat::R32G32B32_Float, 12 };
    pd.layout[2] = { "COLOR",    0, EFormat::R32G32B32_Float, 24 };
    pd.layout[3] = { "TEXCOORD", 0, EFormat::R32G32_Float,    36 };   // x=metallic, y=roughness
    pd.layout_count = 4;
    auto plr = CreateRhiPipeline(*dev, pd);
    if (plr.IsErr()) { ACS_LOG_ERROR("[3D] mesh パイプライン生成失敗"); return false; }
    h.m3d_pipe = Move(plr.Value());
    h.r3d_init_phase = 1u;
    return false;
    }

    // 法線 G-buffer プリパス用パイプライン (SSAO/CSsao 入力)。M3DVtx → world normal を RGBA16F へ。
    if (h.r3d_init_phase == 1u) {
    {
        FShaderDesc nvs{}; nvs.stage = EShaderStage::Vertex; nvs.hlsl_source = kNormalGBuf3DHLSL; nvs.entry_point = "VSMain"; nvs.debug_name = "NormalGBuf.VS";
        FShaderDesc nps{}; nps.stage = EShaderStage::Pixel;  nps.hlsl_source = kNormalGBuf3DHLSL; nps.entry_point = "PSMain"; nps.debug_name = "NormalGBuf.PS";
        auto nvr = CreateRhiShader(*dev, nvs); auto npr = CreateRhiShader(*dev, nps);
        if (nvr.IsOk() && npr.IsOk()) {
            h.normal_vs = Move(nvr.Value()); h.normal_ps = Move(npr.Value());
            FPipelineDesc np{};
            np.vs = h.normal_vs.Get(); np.ps = h.normal_ps.Get();
            np.topology     = EPrimitiveTopology::TriangleList;
            np.rt_format    = hdrf;                 // RGBA16F (CSsao の normal gbuffer 期待形式)
            np.depth_format = df;
            np.depth_test   = true; np.depth_write = true;
            np.cull_mode    = ECullMode::None;
            np.blend_mode   = EBlendMode::Opaque;
            np.cbuffer_slots = 1; np.cbuffer_names[0] = "NFrame";
            np.vertex_stride = sizeof(FM3DVtx);
            np.layout[0] = { "POSITION", 0, EFormat::R32G32B32_Float, 0  };
            np.layout[1] = { "NORMAL",   0, EFormat::R32G32B32_Float, 12 };
            np.layout[2] = { "COLOR",    0, EFormat::R32G32B32_Float, 24 };
            np.layout[3] = { "TEXCOORD", 0, EFormat::R32G32_Float,    36 };
            np.layout_count = 4;
            auto nplr = CreateRhiPipeline(*dev, np);
            if (nplr.IsOk()) {
                h.normal_pipe = Move(nplr.Value());
                FBufferDesc ncb{}; ncb.size = 256; ncb.usage = EBufferUsage::Uniform; ncb.cpu_writable = true;
                auto ncr = CreateRhiBuffer(*dev, ncb); if (ncr.IsOk()) h.normal_cb = Move(ncr.Value());
                h.ssao_pipe_ready = (h.normal_pipe.Get() != nullptr && h.normal_cb.Get() != nullptr);
            }
        }
        if (!h.ssao_pipe_ready) ACS_LOG_WARN("[3D] 法線 G-buffer パイプライン生成失敗 (SSAO 無効)");
    }
    h.r3d_init_phase = 2u;
    return false;
    }

    // 屈折 (ガラス/水): CRefractionShader + opaque シーン複製 blit パイプライン。要 env cubemap (Diligent IBL)。
    if (h.r3d_init_phase == 2u) {
    { auto rr = h.refr3d.Init(*dev, hdrf, df); h.refr_ready = rr.IsOk();
      if (!h.refr_ready) ACS_LOG_WARN("[3D] CRefractionShader Init 失敗 (屈折無効): %s", rr.Error().message); }
    h.r3d_init_phase = 3u;
    return false;
    }
    if (h.r3d_init_phase == 3u) {
    {
        FShaderDesc bvs{}; bvs.stage = EShaderStage::Vertex; bvs.hlsl_source = kBlit3DHLSL; bvs.entry_point = "VSMain"; bvs.debug_name = "Blit.VS";
        FShaderDesc bps{}; bps.stage = EShaderStage::Pixel;  bps.hlsl_source = kBlit3DHLSL; bps.entry_point = "PSMain"; bps.debug_name = "Blit.PS";
        auto bvr = CreateRhiShader(*dev, bvs); auto bpr = CreateRhiShader(*dev, bps);
        if (bvr.IsOk() && bpr.IsOk()) {
            h.blit_vs = Move(bvr.Value()); h.blit_ps = Move(bpr.Value());
            FPipelineDesc bp{};
            bp.vs = h.blit_vs.Get(); bp.ps = h.blit_ps.Get();
            bp.topology     = EPrimitiveTopology::TriangleList;
            bp.rt_format    = hdrf; bp.depth_format = EFormat::Unknown;
            bp.depth_test   = false; bp.depth_write = false;
            bp.cull_mode    = ECullMode::None; bp.blend_mode = EBlendMode::Opaque;
            bp.texture_slots = 1;
            bp.static_sampler_count = 1;
            bp.static_samplers[0].filter    = ESamplerFilter::Linear;
            bp.static_samplers[0].address_u = ESamplerAddress::Clamp;
            bp.static_samplers[0].address_v = ESamplerAddress::Clamp;
            bp.static_samplers[0].address_w = ESamplerAddress::Clamp;
            auto bplr = CreateRhiPipeline(*dev, bp);
            if (bplr.IsOk()) { h.blit_pipe = Move(bplr.Value()); h.blit_ready = true; }
        }
        if (!h.blit_ready) ACS_LOG_WARN("[3D] blit パイプライン生成失敗 (屈折無効)");
    }
    h.r3d_init_phase = 4u;
    return false;
    }

    // 被写界深度 (DoF): scene 複製 (t0) + depth (t1) を sample してディスクぼかし。
    if (h.r3d_init_phase == 4u) {
    {
        FShaderDesc dvs{}; dvs.stage = EShaderStage::Vertex; dvs.hlsl_source = kDof3DHLSL; dvs.entry_point = "VSMain"; dvs.debug_name = "Dof.VS";
        FShaderDesc dps{}; dps.stage = EShaderStage::Pixel;  dps.hlsl_source = kDof3DHLSL; dps.entry_point = "PSMain"; dps.debug_name = "Dof.PS";
        auto dvr = CreateRhiShader(*dev, dvs); auto dpr = CreateRhiShader(*dev, dps);
        if (dvr.IsOk() && dpr.IsOk()) {
            h.dof_vs = Move(dvr.Value()); h.dof_ps = Move(dpr.Value());
            FPipelineDesc dp{};
            dp.vs = h.dof_vs.Get(); dp.ps = h.dof_ps.Get();
            dp.topology     = EPrimitiveTopology::TriangleList;
            dp.rt_format    = hdrf; dp.depth_format = EFormat::Unknown;
            dp.depth_test   = false; dp.depth_write = false;
            dp.cull_mode    = ECullMode::None; dp.blend_mode = EBlendMode::Opaque;
            dp.texture_slots = 2;
            dp.cbuffer_slots = 1; dp.cbuffer_names[0] = "DOFCB";
            dp.static_sampler_count = 2;
            for (int s = 0; s < 2; ++s) {
                dp.static_samplers[s].filter    = (s == 0) ? ESamplerFilter::Linear : ESamplerFilter::Point;
                dp.static_samplers[s].address_u = ESamplerAddress::Clamp;
                dp.static_samplers[s].address_v = ESamplerAddress::Clamp;
                dp.static_samplers[s].address_w = ESamplerAddress::Clamp;
            }
            auto dplr = CreateRhiPipeline(*dev, dp);
            if (dplr.IsOk()) {
                h.dof_pipe = Move(dplr.Value());
                FBufferDesc dcb{}; dcb.size = 256; dcb.usage = EBufferUsage::Uniform; dcb.cpu_writable = true;
                auto dcr = CreateRhiBuffer(*dev, dcb); if (dcr.IsOk()) h.dof_cb = Move(dcr.Value());
                h.dof_ready = (h.dof_pipe.Get() != nullptr && h.dof_cb.Get() != nullptr);
            }
        }
        if (!h.dof_ready) ACS_LOG_WARN("[3D] DoF パイプライン生成失敗 (被写界深度無効)");
    }
    h.r3d_init_phase = 5u;
    return false;
    }

    // god rays (光芒): scene 複製 (t0) を太陽へ放射状 march。
    if (h.r3d_init_phase == 5u) {
    {
        FShaderDesc gvs{}; gvs.stage = EShaderStage::Vertex; gvs.hlsl_source = kGodRays3DHLSL; gvs.entry_point = "VSMain"; gvs.debug_name = "GodRays.VS";
        FShaderDesc gps{}; gps.stage = EShaderStage::Pixel;  gps.hlsl_source = kGodRays3DHLSL; gps.entry_point = "PSMain"; gps.debug_name = "GodRays.PS";
        auto gvr = CreateRhiShader(*dev, gvs); auto gpr = CreateRhiShader(*dev, gps);
        if (gvr.IsOk() && gpr.IsOk()) {
            h.gray_vs = Move(gvr.Value()); h.gray_ps = Move(gpr.Value());
            FPipelineDesc gp{};
            gp.vs = h.gray_vs.Get(); gp.ps = h.gray_ps.Get();
            gp.topology     = EPrimitiveTopology::TriangleList;
            gp.rt_format    = hdrf; gp.depth_format = EFormat::Unknown;
            gp.depth_test   = false; gp.depth_write = false;
            gp.cull_mode    = ECullMode::None; gp.blend_mode = EBlendMode::Opaque;
            gp.texture_slots = 2;
            gp.cbuffer_slots = 1; gp.cbuffer_names[0] = "GRCB";
            gp.static_sampler_count = 2;
            for (int s = 0; s < 2; ++s) {
                gp.static_samplers[s].filter    = (s == 0) ? ESamplerFilter::Linear : ESamplerFilter::Point;
                gp.static_samplers[s].address_u = ESamplerAddress::Clamp;
                gp.static_samplers[s].address_v = ESamplerAddress::Clamp;
                gp.static_samplers[s].address_w = ESamplerAddress::Clamp;
            }
            auto gplr = CreateRhiPipeline(*dev, gp);
            if (gplr.IsOk()) {
                h.gray_pipe = Move(gplr.Value());
                FBufferDesc gcb{}; gcb.size = 256; gcb.usage = EBufferUsage::Uniform; gcb.cpu_writable = true;
                auto gcr = CreateRhiBuffer(*dev, gcb); if (gcr.IsOk()) h.gray_cb = Move(gcr.Value());
                h.gray_ready = (h.gray_pipe.Get() != nullptr && h.gray_cb.Get() != nullptr);
            }
        }
        if (!h.gray_ready) ACS_LOG_WARN("[3D] god rays パイプライン生成失敗");
    }
    h.r3d_init_phase = 6u;
    return false;
    }

    // モーションブラー: scene 複製 (t0) + motion (t1) を sample。
    if (h.r3d_init_phase == 6u) {
    {
        FShaderDesc mvs{}; mvs.stage = EShaderStage::Vertex; mvs.hlsl_source = kMotionBlur3DHLSL; mvs.entry_point = "VSMain"; mvs.debug_name = "MBlur.VS";
        FShaderDesc mps{}; mps.stage = EShaderStage::Pixel;  mps.hlsl_source = kMotionBlur3DHLSL; mps.entry_point = "PSMain"; mps.debug_name = "MBlur.PS";
        auto mvr = CreateRhiShader(*dev, mvs); auto mpr = CreateRhiShader(*dev, mps);
        if (mvr.IsOk() && mpr.IsOk()) {
            h.mblur_vs = Move(mvr.Value()); h.mblur_ps = Move(mpr.Value());
            FPipelineDesc mp{};
            mp.vs = h.mblur_vs.Get(); mp.ps = h.mblur_ps.Get();
            mp.topology     = EPrimitiveTopology::TriangleList;
            mp.rt_format    = hdrf; mp.depth_format = EFormat::Unknown;
            mp.depth_test   = false; mp.depth_write = false;
            mp.cull_mode    = ECullMode::None; mp.blend_mode = EBlendMode::Opaque;
            mp.texture_slots = 3;
            mp.cbuffer_slots = 1; mp.cbuffer_names[0] = "MBCB";
            mp.static_sampler_count = 3;
            for (int s = 0; s < 3; ++s) {
                mp.static_samplers[s].filter    = (s == 2) ? ESamplerFilter::Point : ESamplerFilter::Linear;
                mp.static_samplers[s].address_u = ESamplerAddress::Clamp;
                mp.static_samplers[s].address_v = ESamplerAddress::Clamp;
                mp.static_samplers[s].address_w = ESamplerAddress::Clamp;
            }
            auto mplr = CreateRhiPipeline(*dev, mp);
            if (mplr.IsOk()) {
                h.mblur_pipe = Move(mplr.Value());
                FBufferDesc mcb{}; mcb.size = 256; mcb.usage = EBufferUsage::Uniform; mcb.cpu_writable = true;
                auto mcr = CreateRhiBuffer(*dev, mcb); if (mcr.IsOk()) h.mblur_cb = Move(mcr.Value());
                h.mblur_ready = (h.mblur_pipe.Get() != nullptr && h.mblur_cb.Get() != nullptr);
            }
        }
        if (!h.mblur_ready) ACS_LOG_WARN("[3D] モーションブラー パイプライン生成失敗");
    }
    h.r3d_init_phase = 7u;
    return false;
    }

    // オーバーレイ用 (同シェーダ・depth off): ギズモを常に手前に出す。
    if (h.r3d_init_phase == 7u) {
    FPipelineDesc pd{};
    pd.vs = h.m3d_vs.Get(); pd.ps = h.m3d_ps.Get();
    pd.topology = EPrimitiveTopology::TriangleList;
    pd.rt_format = hdrf; pd.depth_format = df;
    pd.cull_mode = ECullMode::None; pd.blend_mode = EBlendMode::Opaque;
    pd.cbuffer_slots = 1; pd.cbuffer_names[0] = "Frame";
    pd.texture_slots = 1; pd.texture_names[0] = "shadow_map";
    pd.static_sampler_count = 1;
    pd.static_samplers[0].filter = ESamplerFilter::Linear;
    pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
    pd.static_samplers[0].address_w = ESamplerAddress::Clamp;
    pd.vertex_stride = sizeof(FM3DVtx);
    pd.layout[0] = { "POSITION", 0, EFormat::R32G32B32_Float, 0  };
    pd.layout[1] = { "NORMAL",   0, EFormat::R32G32B32_Float, 12 };
    pd.layout[2] = { "COLOR",    0, EFormat::R32G32B32_Float, 24 };
    pd.layout[3] = { "TEXCOORD", 0, EFormat::R32G32_Float,    36 };
    pd.layout_count = 4;
    pd.depth_test = false; pd.depth_write = false;
    auto plo = CreateRhiPipeline(*dev, pd);
    if (plo.IsErr()) { ACS_LOG_ERROR("[3D] overlay パイプライン生成失敗"); return false; }
    h.m3d_overlay_pipe = Move(plo.Value());

    // スカイボックス (フルスクリーン三角形、depth off、cbuffer Sky 1 本)。
    FShaderDesc svs{}; svs.stage = EShaderStage::Vertex; svs.hlsl_source = kSky3DHLSL; svs.entry_point = "VSMain"; svs.debug_name = "Sky3D.VS";
    FShaderDesc sps{}; sps.stage = EShaderStage::Pixel;  sps.hlsl_source = kSky3DHLSL; sps.entry_point = "PSMain"; sps.debug_name = "Sky3D.PS";
    auto svr = CreateRhiShader(*dev, svs); auto spr = CreateRhiShader(*dev, sps);
    if (svr.IsErr() || spr.IsErr()) { ACS_LOG_ERROR("[3D] sky シェーダ生成失敗"); return false; }
    h.sky_vs = Move(svr.Value()); h.sky_ps = Move(spr.Value());
    FPipelineDesc skd{};
    skd.vs = h.sky_vs.Get(); skd.ps = h.sky_ps.Get();
    skd.topology = EPrimitiveTopology::TriangleList;
    skd.rt_format = hdrf; skd.depth_format = EFormat::Unknown;
    skd.depth_test = false; skd.depth_write = false;
    skd.cull_mode = ECullMode::None; skd.blend_mode = EBlendMode::Opaque;
    skd.cbuffer_slots = 1; skd.cbuffer_names[0] = "Sky";
    auto skr = CreateRhiPipeline(*dev, skd);
    if (skr.IsErr()) { ACS_LOG_ERROR("[3D] sky パイプライン生成失敗"); return false; }
    h.sky_pipe = Move(skr.Value());
    h.r3d_init_phase = 8u;
    return false;
    }

    // エンジン標準スカイ CSky は «depth off の背景フルスクリーン三角» 方式で描く。
    // 2D の CSpriteBatch と同じ depth-off エンジンパスを使い、手続き雲を合成する。
    if (h.r3d_init_phase == 8u) {
    const char* const backend_name = dev->BackendName();
    const bool raw_dx12 = backend_name != nullptr &&
                          std::strcmp(backend_name, "DX12") == 0;
    if (dev->SupportsAsyncShaderCompilation()) {
        if (h.startup_async_shader_kind == 0u) {
            const editor_profiler::FTimePoint submit_begin =
                editor_profiler::CClock::now();
            auto shader_result = CSky::BeginCompileShadersAsync(*dev);
            if (shader_result.IsOk()) {
                h.startup_sky_shaders = Move(shader_result.Value());
                h.startup_async_shader_kind = 3u;
                h.startup_async_shader_begin = submit_begin;
                h.startup_phase_pending = true;
                return false;
            }
            h.sky3d_ready = false;
            ACS_LOG_WARN(
                "[3D] CSky asynchronous shader submission failed: %s",
                shader_result.Error().message);
        } else {
            const EShaderStatus shader_status =
                h.startup_sky_shaders.Status();
            if (shader_status == EShaderStatus::Compiling) {
                h.startup_phase_pending = true;
                return false;
            }
            const f32 compile_ms = editor_profiler::ElapsedMilliseconds(
                h.startup_async_shader_begin);
            h.startup_async_shader_kind = 0u;
            h.startup_phase_elapsed_override_ms = compile_ms;
            if (shader_status == EShaderStatus::Ready) {
                const editor_profiler::FTimePoint commit_begin =
                    editor_profiler::CClock::now();
                const auto sky_result = h.sky3d.InitWithCompiledShaders(
                    *dev, Move(h.startup_sky_shaders), hdrf, df);
                const f32 commit_ms =
                    editor_profiler::ElapsedMilliseconds(commit_begin);
                h.startup_phase_elapsed_override_ms += commit_ms;
                h.sky3d_ready = sky_result.IsOk();
                if (h.sky3d_ready) {
                    ACS_LOG_INFO(
                        "[3D] CSky backend compile %.2f ms; "
                        "owner RHI commit %.2f ms",
                        compile_ms, commit_ms);
                } else {
                    ACS_LOG_WARN(
                        "[3D] CSky owner-thread RHI commit failed: %s",
                        sky_result.Error().message);
                }
            } else {
                h.startup_sky_shaders = {};
                h.sky3d_ready = false;
                ACS_LOG_ERROR("[3D] CSky asynchronous shader compile failed");
            }
        }
    } else if (raw_dx12) {
        if (h.startup_worker_state.load(std::memory_order_acquire) == 0 &&
            !h.startup_worker.Joinable()) {
            if (BeginSkyCompileWorker(h)) {
                h.startup_phase_pending = true;
                return false;
            }
            ACS_LOG_WARN(
                "[3D] CSky CPU compile worker unavailable; using synchronous init");
            const auto sky_result = h.sky3d.Init(*dev, hdrf, df);
            h.sky3d_ready = sky_result.IsOk();
            if (!h.sky3d_ready) {
                ACS_LOG_WARN(
                    "[3D] CSky synchronous init failed: %s",
                    sky_result.Error().message);
            }
        } else {
            const i32 worker_result = PollStartupWorker(h);
            if (worker_result == 0) {
                h.startup_phase_pending = true;
                return false;
            }
            h.startup_worker_kind = 0u;
            h.startup_phase_elapsed_override_ms =
                h.startup_worker_elapsed_ms;
            if (worker_result > 0) {
                const editor_profiler::FTimePoint commit_begin =
                    editor_profiler::CClock::now();
                const auto sky_result = h.sky3d.InitWithCompiledShaders(
                    *dev, Move(h.startup_sky_shaders), hdrf, df);
                const f32 commit_ms =
                    editor_profiler::ElapsedMilliseconds(commit_begin);
                h.startup_phase_elapsed_override_ms += commit_ms;
                h.sky3d_ready = sky_result.IsOk();
                if (!h.sky3d_ready) {
                    ACS_LOG_WARN(
                        "[3D] CSky owner-thread RHI commit failed: %s",
                        sky_result.Error().message);
                } else {
                    ACS_LOG_INFO(
                        "[3D] CSky CPU compile %.2f ms; owner RHI commit %.2f ms",
                        h.startup_worker_elapsed_ms,
                        commit_ms);
                }
            } else {
                h.startup_sky_shaders = {};
                h.sky3d_ready = false;
            }
        }
    } else {
        const auto sky_result = h.sky3d.Init(*dev, hdrf, df);
        h.sky3d_ready = sky_result.IsOk();
        if (!h.sky3d_ready) {
            ACS_LOG_WARN(
                "[3D] CSky init failed on %s: %s",
                backend_name != nullptr ? backend_name : "unknown backend",
                sky_result.Error().message);
        }
    }
    if (h.sky3d_ready) {
          h.sky3d.PresetDay();
          h.sky3d.SetSunDirection(FVec3{ 0.40f, 0.85f, -0.35f });   // シーンのライト方向と一致
          h.sky3d.SetZenithColor (FVec3{ 0.16f, 0.33f, 0.62f });    // メッシュ IBL と同じグラデーション (整合)
          h.sky3d.SetHorizonColor(FVec3{ 0.62f, 0.70f, 0.80f });
          h.sky3d.SetGroundColor (FVec3{ 0.20f, 0.19f, 0.21f });
    } else {
        ACS_LOG_WARN("[3D] CSky unavailable; using fallback sky");
    }
    h.r3d_init_phase = 9u;
    return false;
    }

    // エンジン標準 PBR。HDR フォーマット + cull None (editor の単面 plane/polygon も出す)。
    if (h.r3d_init_phase == 9u) {
    const char* const backend_name = dev->BackendName();
    const bool raw_dx12 = backend_name != nullptr &&
                          std::strcmp(backend_name, "DX12") == 0;
    bool used_async_compile = false;
    if (dev->SupportsAsyncShaderCompilation()) {
        used_async_compile = true;
        if (h.startup_async_shader_kind == 0u) {
            const editor_profiler::FTimePoint submit_begin =
                editor_profiler::CClock::now();
            auto shader_result =
                CPbrShader::BeginCompileShadersAsync(*dev);
            if (shader_result.IsOk()) {
                h.startup_pbr_shaders = Move(shader_result.Value());
                h.startup_async_shader_kind = 1u;
                h.startup_async_shader_begin = submit_begin;
                h.startup_phase_pending = true;
                return false;
            }
            h.pbr3d_ready = false;
            ACS_LOG_ERROR(
                "[3D] CPbrShader asynchronous shader submission failed: %s",
                shader_result.Error().message);
        } else {
            const EShaderStatus shader_status =
                h.startup_pbr_shaders.Status();
            if (shader_status == EShaderStatus::Compiling) {
                h.startup_phase_pending = true;
                return false;
            }
            const f32 compile_ms = editor_profiler::ElapsedMilliseconds(
                h.startup_async_shader_begin);
            h.startup_async_shader_kind = 0u;
            h.startup_phase_elapsed_override_ms = compile_ms;
            if (shader_status == EShaderStatus::Ready) {
                const editor_profiler::FTimePoint commit_begin =
                    editor_profiler::CClock::now();
                const auto pbr_result = h.pbr3d.InitWithCompiledShaders(
                    *dev,
                    Move(h.startup_pbr_shaders),
                    hdrf,
                    df,
                    ECullMode::None);
                const f32 commit_ms =
                    editor_profiler::ElapsedMilliseconds(commit_begin);
                h.startup_phase_elapsed_override_ms += commit_ms;
                h.pbr3d_ready = pbr_result.IsOk();
                if (h.pbr3d_ready) {
                    ACS_LOG_INFO(
                        "[3D] CPbrShader backend compile %.2f ms; "
                        "owner RHI commit %.2f ms",
                        compile_ms, commit_ms);
                } else {
                    ACS_LOG_ERROR(
                        "[3D] CPbrShader owner-thread RHI commit failed: %s",
                        pbr_result.Error().message);
                }
            } else {
                h.startup_pbr_shaders = {};
                h.pbr3d_ready = false;
                ACS_LOG_ERROR(
                    "[3D] CPbrShader asynchronous shader compile failed");
            }
        }
    } else if (raw_dx12) {
        if (h.startup_worker_state.load(std::memory_order_acquire) == 0 &&
            !h.startup_worker.Joinable()) {
            if (BeginPbrCompileWorker(h, *dev, hdrf, df)) {
                h.startup_phase_pending = true;
                return false;
            }
            // Thread creation failure is exceptional, but retaining the
            // owner-thread path preserves rendering quality and correctness.
            ACS_LOG_WARN(
                "[3D] PBR CPU compile worker unavailable; using synchronous init");
            const auto pbr_result =
                h.pbr3d.Init(*dev, hdrf, df, ECullMode::None);
            h.pbr3d_ready = pbr_result.IsOk();
            if (!h.pbr3d_ready) {
                ACS_LOG_ERROR(
                    "[3D] CPbrShader synchronous init failed: %s",
                    pbr_result.Error().message);
            }
        } else {
            const i32 worker_result = PollStartupWorker(h);
            if (worker_result == 0) {
                h.startup_phase_pending = true;
                return false;
            }
            h.startup_worker_kind = 0u;
            used_async_compile = true;
            h.startup_phase_elapsed_override_ms =
                h.startup_worker_elapsed_ms;
            if (worker_result > 0) {
                const editor_profiler::FTimePoint publish_begin =
                    editor_profiler::CClock::now();
                // The acquire + join in PollStartupWorker is the publication
                // boundary for the fully initialized, previously unpublished
                // pbr3d object. No driver work remains on the UI thread.
                h.pbr3d_ready = true;
                h.startup_pbr_candidate_device = nullptr;
                const f32 publish_ms =
                    editor_profiler::ElapsedMilliseconds(publish_begin);
                h.startup_phase_elapsed_override_ms += publish_ms;
                ACS_LOG_INFO(
                    "[3D] CPbrShader background full init %.2f ms; "
                    "owner publication %.3f ms",
                    h.startup_worker_elapsed_ms, publish_ms);
            } else {
                h.startup_pbr_candidate_device = nullptr;
                h.pbr3d_ready = false;
            }
        }
    } else {
        // Backends without an asynchronous compiler retain their established
        // owner-thread creation path.
        const auto pbr_result =
            h.pbr3d.Init(*dev, hdrf, df, ECullMode::None);
        h.pbr3d_ready = pbr_result.IsOk();
        if (!h.pbr3d_ready) {
            ACS_LOG_ERROR(
                "[3D] CPbrShader init failed on %s: %s",
                backend_name != nullptr ? backend_name : "unknown backend",
                pbr_result.Error().message);
        }
    }
    if (h.pbr3d_ready) {
          FVec4 sh9[9]; ComputeSkySh9(sh9, h.sky_zenith, h.sky_horizon, h.sky_ground);  // 実際の空色から SH9 環境光
          for (int si = 0; si < 9; ++si) { sh9[si].x *= kSh9Ambient; sh9[si].y *= kSh9Ambient; sh9[si].z *= kSh9Ambient; }
          h.pbr3d.SetSh9(sh9);                                     // 拡散 ambient + Sh9Radiance(鏡面 fallback) 兼用
          h.sh9_dirty = false;
          if (used_async_compile) {
              ACS_LOG_INFO("[3D] CPbrShader async init OK + SH9 IBL");
          }
    } else {
        ACS_LOG_WARN("[3D] CPbrShader unavailable; using mesh fallback");
    }
    h.r3d_init_phase = 10u;
    return false;
    }

    // スプライト (テクスチャ付きクアッド): t0=albedo + 静的 Linear/Clamp サンプラ、アルファブレンド。
    // 半透明なので depth_test=on / depth_write=off (背後のメッシュには隠れるが互いの深度は描画順)。
    if (h.r3d_init_phase == 10u) {
    {
        FShaderDesc pvs{}; pvs.stage = EShaderStage::Vertex; pvs.hlsl_source = kSprite3DHLSL; pvs.entry_point = "VSMain"; pvs.debug_name = "Sprite3D.VS";
        FShaderDesc pps{}; pps.stage = EShaderStage::Pixel;  pps.hlsl_source = kSprite3DHLSL; pps.entry_point = "PSMain"; pps.debug_name = "Sprite3D.PS";
        auto pvr = CreateRhiShader(*dev, pvs); auto ppr = CreateRhiShader(*dev, pps);
        if (pvr.IsErr() || ppr.IsErr()) { ACS_LOG_ERROR("[3D] sprite シェーダ生成失敗"); return false; }
        h.spr_vs = Move(pvr.Value()); h.spr_ps = Move(ppr.Value());
        FPipelineDesc sd{};
        sd.vs = h.spr_vs.Get(); sd.ps = h.spr_ps.Get();
        sd.topology     = EPrimitiveTopology::TriangleList;
        sd.rt_format    = hdrf; sd.depth_format = df;
        sd.depth_test   = true; sd.depth_write = false;
        sd.cull_mode    = ECullMode::None;
        sd.blend_mode   = EBlendMode::AlphaBlend;
        sd.cbuffer_slots = 1; sd.cbuffer_names[0] = "Frame";
        sd.texture_slots = 1; sd.texture_names[0] = "albedo";
        sd.static_sampler_count    = 1;
        sd.static_samplers[0].filter    = ESamplerFilter::Linear;
        sd.static_samplers[0].address_u = ESamplerAddress::Clamp;
        sd.static_samplers[0].address_v = ESamplerAddress::Clamp;
        sd.static_samplers[0].address_w = ESamplerAddress::Clamp;
        sd.vertex_stride = sizeof(FSprVtx);
        sd.layout[0] = { "POSITION", 0, EFormat::R32G32B32_Float, 0  };
        sd.layout[1] = { "TEXCOORD", 0, EFormat::R32G32_Float,    12 };
        sd.layout_count = 2;
        auto spl = CreateRhiPipeline(*dev, sd);
        if (spl.IsErr()) { ACS_LOG_ERROR("[3D] sprite パイプライン生成失敗"); return false; }
        h.spr_pipe = Move(spl.Value());
        FBufferDesc vb{}; vb.size = sizeof(FSprVtx) * 6 * 1024; vb.usage = EBufferUsage::Vertex; vb.cpu_writable = true;
        auto vr2 = CreateRhiBuffer(*dev, vb); if (vr2.IsOk()) h.spr_vb = Move(vr2.Value());
        if (!h.spr_vb) {
            ACS_LOG_WARN("[3D] sprite vertex-buffer creation failed (3D sprites disabled)");
        }
    }
    h.r3d_init_phase = 11u;
    return false;
    }

    // 無限グリッド: y=0 (ortho は z=0) の大クアッド。アルファブレンド、depth_test on / write off (シーンに隠れる半透明)。
    if (h.r3d_init_phase == 11u) {
    {
        FShaderDesc gvs{}; gvs.stage = EShaderStage::Vertex; gvs.hlsl_source = kGrid3DHLSL; gvs.entry_point = "VSMain"; gvs.debug_name = "Grid3D.VS";
        FShaderDesc gps{}; gps.stage = EShaderStage::Pixel;  gps.hlsl_source = kGrid3DHLSL; gps.entry_point = "PSMain"; gps.debug_name = "Grid3D.PS";
        auto gvr = CreateRhiShader(*dev, gvs); auto gpr = CreateRhiShader(*dev, gps);
        if (gvr.IsErr() || gpr.IsErr()) { ACS_LOG_ERROR("[3D] grid シェーダ生成失敗"); return false; }
        h.grid_vs = Move(gvr.Value()); h.grid_ps = Move(gpr.Value());
        FPipelineDesc gd{};
        gd.vs = h.grid_vs.Get(); gd.ps = h.grid_ps.Get();
        gd.topology     = EPrimitiveTopology::TriangleList;
        gd.rt_format    = hdrf; gd.depth_format = df;
        gd.depth_test   = true; gd.depth_write = false;
        gd.cull_mode    = ECullMode::None; gd.blend_mode = EBlendMode::AlphaBlend;
        gd.cbuffer_slots = 1; gd.cbuffer_names[0] = "Grid";
        gd.vertex_stride = sizeof(FM3DVtx);
        gd.layout[0] = { "POSITION", 0, EFormat::R32G32B32_Float, 0  };
        gd.layout[1] = { "NORMAL",   0, EFormat::R32G32B32_Float, 12 };
        gd.layout[2] = { "COLOR",    0, EFormat::R32G32B32_Float, 24 };
        gd.layout_count = 3;
        auto gpl = CreateRhiPipeline(*dev, gd);
        if (gpl.IsErr()) { ACS_LOG_ERROR("[3D] grid パイプライン生成失敗"); return false; }
        h.grid_pipe = Move(gpl.Value());
        FBufferDesc gvbd{}; gvbd.size = sizeof(FM3DVtx) * 6; gvbd.usage = EBufferUsage::Vertex; gvbd.cpu_writable = true;
        auto gvbr = CreateRhiBuffer(*dev, gvbd); if (gvbr.IsOk()) h.grid_vb = Move(gvbr.Value());
        FBufferDesc gcbd{}; gcbd.size = 256; gcbd.usage = EBufferUsage::Uniform; gcbd.cpu_writable = true;
        auto gcbr = CreateRhiBuffer(*dev, gcbd); if (gcbr.IsOk()) h.grid_cb = Move(gcbr.Value());
        if (!h.grid_vb || !h.grid_cb) {
            ACS_LOG_WARN("[3D] grid buffer creation failed (infinite grid disabled)");
        }
    }
    h.r3d_init_phase = 12u;
    return false;
    }

    // シャドウマップ (有向光源、single cascade 2048)。深度は D32 / shader-visible。キャスターは M3DVtx 専用 (depth-only)。
    if (h.r3d_init_phase == 12u) {
    if (h.shadow.Init(*dev, h.q_shadow_size > 0 ? h.q_shadow_size : 2048u).IsOk()) {
        FShaderDesc cvs{}; cvs.stage = EShaderStage::Vertex; cvs.hlsl_source = kShadowCaster3DHLSL; cvs.entry_point = "VSMain"; cvs.debug_name = "ShadowCasterM3D.VS";
        auto cvr = CreateRhiShader(*dev, cvs);
        if (cvr.IsOk()) {
            h.shadow_caster_vs = Move(cvr.Value());
            FPipelineDesc cpd{};
            cpd.vs = h.shadow_caster_vs.Get(); cpd.ps = nullptr;       // depth-only
            cpd.topology     = EPrimitiveTopology::TriangleList;
            cpd.rt_format    = EFormat::Unknown; cpd.depth_format = EFormat::D32_Float;
            cpd.depth_test   = true; cpd.depth_write = true;
            cpd.cull_mode    = ECullMode::None;        // editor メッシュは片面もあるので cull せず (acne はバイアスで回避)
            cpd.cbuffer_slots = 1; cpd.cbuffer_names[0] = "LightFrame";
            cpd.vertex_stride = sizeof(FM3DVtx);
            cpd.layout[0] = { "POSITION", 0, EFormat::R32G32B32_Float, 0 };
            cpd.layout_count = 1;
            auto cpl = CreateRhiPipeline(*dev, cpd);
            if (cpl.IsOk()) {
                h.shadow_caster_pipe = Move(cpl.Value());
                FBufferDesc lcb{}; lcb.size = 256; lcb.usage = EBufferUsage::Uniform; lcb.cpu_writable = true;
                auto lcr = CreateRhiBuffer(*dev, lcb); if (lcr.IsOk()) h.shadow_lvp_cb = Move(lcr.Value());
                for (u32 c = 0; c < acs::CShadowMap::kMaxCascades; ++c) {   // CSM: cascade 毎の light VP 用 CB
                    FBufferDesc ccb{}; ccb.size = 256; ccb.usage = EBufferUsage::Uniform; ccb.cpu_writable = true;
                    auto ccr = CreateRhiBuffer(*dev, ccb); if (ccr.IsOk()) h.shadow_cascade_cb[c] = Move(ccr.Value());
                }
                h.shadow_ready = (h.shadow_lvp_cb.Get() != nullptr);
            }
        }
    }
    h.r3d_init_phase = 13u;
    return false;
    }

    if (h.r3d_init_phase == 13u) {
    {
        FBufferDesc c{}; c.size = 256; c.usage = EBufferUsage::Uniform; c.cpu_writable = true;
        auto r = CreateRhiBuffer(*dev, c); if (r.IsOk()) h.m3d_frame_cb = Move(r.Value());
    }
    {
        FBufferDesc c{}; c.size = 256; c.usage = EBufferUsage::Uniform; c.cpu_writable = true;
        auto r = CreateRhiBuffer(*dev, c); if (r.IsOk()) h.m3d_giz_cb = Move(r.Value());
    }
    {
        FBufferDesc c{}; c.size = 256; c.usage = EBufferUsage::Uniform; c.cpu_writable = true;
        auto r = CreateRhiBuffer(*dev, c); if (r.IsOk()) h.sky_cb = Move(r.Value());
    }
    {
        h.m3d_dyn_cap = 200000u;                       // 頂点容量 (プリミティブ多数でも十分)
        FBufferDesc c{}; c.size = sizeof(FM3DVtx) * h.m3d_dyn_cap; c.usage = EBufferUsage::Vertex; c.cpu_writable = true;
        auto r = CreateRhiBuffer(*dev, c); if (r.IsOk()) h.m3d_dyn_vb = Move(r.Value());
        FBufferDesc cg{}; cg.size = sizeof(FM3DVtx) * 4096; cg.usage = EBufferUsage::Vertex; cg.cpu_writable = true;
        auto rg = CreateRhiBuffer(*dev, cg); if (rg.IsOk()) h.m3d_giz_vb = Move(rg.Value());
    }
    if (!h.m3d_frame_cb || !h.m3d_dyn_vb ||
        !h.m3d_giz_cb || !h.m3d_giz_vb || !h.sky_cb) {
        ACS_LOG_ERROR("[3D] required frame/gizmo/sky buffer creation failed");
        return false;
    }
    h.r3d_init_phase = 14u;
    return false;
    }

    if (h.r3d_init_phase == 14u) {
    auto cube = Primitive::MakeCube(1.0f);
    auto sph  = Primitive::MakeSphere(0.5f, 32, 16);
    auto pl   = Primitive::MakePlane(1.0f, 1.0f);
    auto water_pl = MakeEditorWaterGrid();
    if (!cube || !sph || !pl) { ACS_LOG_ERROR("[3D] プリミティブ生成失敗"); return false; }
    if (UploadMesh(*dev, *cube, h.gm_cube).IsErr() ||
        UploadMesh(*dev, *sph,  h.gm_sphere).IsErr() ||
        UploadMesh(*dev, *pl,   h.gm_plane).IsErr()) { ACS_LOG_ERROR("[3D] メッシュアップロード失敗"); return false; }
    h.cpu_cube = cube; h.cpu_sphere = sph; h.cpu_plane = pl;
    if (water_pl &&
        UploadMesh(*dev, *water_pl, h.gm_water_plane).IsOk()) {
        h.cpu_water_plane = water_pl;
    } else {
        h.gm_water_plane = FGpuMesh{};
        h.cpu_water_plane.Reset();
        ACS_LOG_WARN(
            "[3D] tessellated water grid unavailable; water nodes "
            "will use the opaque PBR fallback");
    }
    h.r3d_init_phase = 15u;
    return false;
    }

    // Screen-sized post/effect stacks used to compile together on the first
    // real scene frame.  Warm exactly one subsystem per dispatcher pump so a
    // Ready editor never immediately falls back into another long UI stall.
    const u32 startup_w = h.width > 0u ? h.width : 1u;
    const u32 startup_h = h.height > 0u ? h.height : 1u;
    if (h.r3d_init_phase == 15u) {
        const char* const backend_name = dev->BackendName();
        const bool raw_dx12 = backend_name != nullptr &&
                              std::strcmp(backend_name, "DX12") == 0;
        bool use_sync_fallback = false;

        if (dev->SupportsAsyncShaderCompilation()) {
            if (h.startup_async_shader_kind == 7u) {
                const EShaderStatus shader_status =
                    h.startup_post_shaders.Status();
                if (shader_status == EShaderStatus::Compiling) {
                    h.startup_phase_pending = true;
                    return false;
                }

                const f32 compile_ms =
                    editor_profiler::ElapsedMilliseconds(
                        h.startup_async_shader_begin);
                h.startup_async_shader_kind = 0u;
                h.startup_phase_elapsed_override_ms = compile_ms;
                if (shader_status == EShaderStatus::Ready) {
                    const editor_profiler::FTimePoint commit_begin =
                        editor_profiler::CClock::now();
                    const auto result =
                        h.post3d.InitWithCompiledShaders(
                            *dev,
                            Move(h.startup_post_shaders),
                            startup_w,
                            startup_h,
                            h.renderer.ColorFormat());
                    const f32 commit_ms =
                        editor_profiler::ElapsedMilliseconds(commit_begin);
                    h.startup_phase_elapsed_override_ms += commit_ms;
                    h.post3d_ready = result.IsOk();
                    if (h.post3d_ready) {
                        h.post3d_w = startup_w;
                        h.post3d_h = startup_h;
                        ACS_LOG_INFO(
                            "[3D] CPostProcess backend compile %.2f ms; "
                            "owner RHI commit %.2f ms",
                            compile_ms, commit_ms);
                    } else {
                        h.startup_post_shaders = {};
                        ACS_LOG_WARN(
                            "[3D] CPostProcess owner-thread RHI commit "
                            "failed: %s",
                            result.Error().message);
                    }
                } else {
                    h.startup_post_shaders = {};
                    h.post3d_ready = false;
                    ACS_LOG_ERROR(
                        "[3D] CPostProcess asynchronous shader compile "
                        "failed; continuing without the post stack");
                }
            } else if (h.startup_async_shader_kind != 0u) {
                h.startup_phase_pending = true;
                return false;
            } else {
                const editor_profiler::FTimePoint submit_begin =
                    editor_profiler::CClock::now();
                auto shader_result =
                    CPostProcess::BeginCompileShadersAsync(*dev);
                if (shader_result.IsOk()) {
                    h.startup_post_shaders =
                        Move(shader_result.Value());
                    h.startup_async_shader_kind = 7u;
                    h.startup_async_shader_begin = submit_begin;
                    h.startup_phase_pending = true;
                    return false;
                }
                use_sync_fallback = true;
                ACS_LOG_WARN(
                    "[3D] CPostProcess asynchronous shader submission "
                    "failed; using synchronous fallback: %s",
                    shader_result.Error().message);
            }
        } else if (raw_dx12) {
            if (h.startup_worker_kind == 7u) {
                const i32 worker_result = PollStartupWorker(h);
                if (worker_result == 0) {
                    h.startup_phase_pending = true;
                    return false;
                }
                h.startup_worker_kind = 0u;
                h.startup_phase_elapsed_override_ms =
                    h.startup_worker_elapsed_ms;
                if (worker_result > 0 &&
                    h.startup_post_shaders.Status() ==
                        EShaderStatus::Ready) {
                    const editor_profiler::FTimePoint commit_begin =
                        editor_profiler::CClock::now();
                    const auto result =
                        h.post3d.InitWithCompiledShaders(
                            *dev,
                            Move(h.startup_post_shaders),
                            startup_w,
                            startup_h,
                            h.renderer.ColorFormat());
                    const f32 commit_ms =
                        editor_profiler::ElapsedMilliseconds(commit_begin);
                    h.startup_phase_elapsed_override_ms += commit_ms;
                    h.post3d_ready = result.IsOk();
                    if (h.post3d_ready) {
                        h.post3d_w = startup_w;
                        h.post3d_h = startup_h;
                        ACS_LOG_INFO(
                            "[3D] CPostProcess CPU compile %.2f ms; "
                            "owner RHI commit %.2f ms",
                            h.startup_worker_elapsed_ms, commit_ms);
                    } else {
                        h.startup_post_shaders = {};
                        ACS_LOG_WARN(
                            "[3D] CPostProcess owner-thread RHI commit "
                            "failed: %s",
                            result.Error().message);
                    }
                } else {
                    h.startup_post_shaders = {};
                    h.post3d_ready = false;
                    ACS_LOG_ERROR(
                        "[3D] CPostProcess CPU shader compile failed; "
                        "continuing without the post stack");
                }
            } else if (h.startup_worker_kind != 0u ||
                       h.startup_worker.Joinable() ||
                       h.startup_worker_state.load(
                           std::memory_order_acquire) != 0) {
                h.startup_phase_pending = true;
                return false;
            } else if (BeginPostCompileWorker(h)) {
                h.startup_phase_pending = true;
                return false;
            } else {
                use_sync_fallback = true;
                ACS_LOG_WARN(
                    "[3D] CPostProcess CPU compile worker unavailable; "
                    "using synchronous fallback");
            }
        } else {
            // Retain compatibility for a backend without either compilation
            // facility. Production Diligent and raw DX12 never take this path.
            use_sync_fallback = true;
        }

        if (use_sync_fallback) {
            const auto result = h.post3d.Init(
                *dev, startup_w, startup_h,
                h.renderer.ColorFormat());
            h.post3d_ready = result.IsOk();
            if (h.post3d_ready) {
                h.post3d_w = startup_w;
                h.post3d_h = startup_h;
            } else {
                ACS_LOG_WARN(
                    "[3D] CPostProcess synchronous fallback failed: %s",
                    result.Error().message);
            }
        }
        h.r3d_init_phase = 16u;
        return false;
    }

    if (h.r3d_init_phase == 16u) {
        bool compiled_ssao_ready = false;
        if (h.startup_async_shader_kind == 5u) {
            const EShaderStatus shader_status =
                h.startup_ssao_shaders.Status();
            if (shader_status == EShaderStatus::Compiling) {
                h.startup_phase_pending = true;
                return false;
            }

            const f32 compile_ms = editor_profiler::ElapsedMilliseconds(
                h.startup_async_shader_begin);
            h.startup_async_shader_kind = 0u;
            h.startup_phase_elapsed_override_ms = compile_ms;
            compiled_ssao_ready =
                shader_status == EShaderStatus::Ready && h.q_ssao_on;
            if (!compiled_ssao_ready) {
                h.startup_ssao_shaders = {};
                if (shader_status == EShaderStatus::Failed) {
                    ACS_LOG_WARN(
                        "[3D] CSsao asynchronous shader compile failed; "
                        "retrying synchronously");
                }
            }
        } else if (h.q_ssao_on &&
                   dev->SupportsAsyncShaderCompilation()) {
            if (h.startup_async_shader_kind != 0u) {
                // Startup is deliberately serialized, but retain a
                // non-blocking guard if another future phase overlaps.
                h.startup_phase_pending = true;
                return false;
            }
            const editor_profiler::FTimePoint submit_begin =
                editor_profiler::CClock::now();
            auto shader_result =
                CSsao::BeginCompileShadersAsync(*dev);
            if (shader_result.IsOk()) {
                h.startup_ssao_shaders = Move(shader_result.Value());
                h.startup_async_shader_kind = 5u;
                h.startup_async_shader_begin = submit_begin;
                h.startup_phase_pending = true;
                return false;
            }
            ACS_LOG_WARN(
                "[3D] CSsao asynchronous shader submission failed; "
                "retrying synchronously: %s",
                shader_result.Error().message);
        }

        const editor_profiler::FTimePoint owner_commit_begin =
            editor_profiler::CClock::now();
        const bool wants_normal_buffer =
            h.q_ssao_on || h.q_ssr_on || h.q_ssgi_on;
        if (wants_normal_buffer) {
            FTextureDesc normal_desc{};
            normal_desc.width = startup_w;
            normal_desc.height = startup_h;
            normal_desc.format = EFormat::R16G16B16A16_Float;
            normal_desc.is_render_target = true;
            auto normal_result = CreateRhiTexture(*dev, normal_desc);
            if (normal_result.IsOk()) {
                h.normal_rt = Move(normal_result.Value());
                h.normal_w = startup_w;
                h.normal_h = startup_h;
            } else {
                h.normal_w = 0u;
                h.normal_h = 0u;
                ACS_LOG_WARN("[3D] normal-buffer startup init failed: %s",
                             normal_result.Error().message);
            }
        }
        if (h.q_ssao_on) {
            const auto result = compiled_ssao_ready
                ? h.ssao3d.InitWithCompiledShaders(
                      *dev,
                      Move(h.startup_ssao_shaders),
                      startup_w,
                      startup_h)
                : h.ssao3d.Init(*dev, startup_w, startup_h);
            h.ssao_ready = result.IsOk();
            if (h.ssao_ready) {
                h.ssao_w = startup_w;
                h.ssao_h = startup_h;
                if (compiled_ssao_ready) {
                    ACS_LOG_INFO(
                        "[3D] CSsao backend compile completed; "
                        "owner RHI commit %.2f ms",
                        editor_profiler::ElapsedMilliseconds(
                            owner_commit_begin));
                }
            } else {
                h.startup_ssao_shaders = {};
                h.ssao_w = 0u;
                h.ssao_h = 0u;
                ACS_LOG_WARN("[3D] CSsao startup init failed: %s",
                             result.Error().message);
            }
        }
        if (h.startup_phase_elapsed_override_ms >= 0.0f) {
            h.startup_phase_elapsed_override_ms +=
                editor_profiler::ElapsedMilliseconds(owner_commit_begin);
        }
        h.r3d_init_phase = 17u;
        return false;
    }

    if (h.r3d_init_phase == 17u) {
        if (h.q_ssr_on) {
            const auto result = h.ssr3d.Init(
                *dev, EFormat::R16G16B16A16_Float, startup_w, startup_h);
            h.ssr_ready = result.IsOk();
            if (h.ssr_ready) {
                h.ssr_w = startup_w;
                h.ssr_h = startup_h;
            } else {
                h.ssr_w = 0u;
                h.ssr_h = 0u;
                ACS_LOG_WARN("[3D] CSsr startup init failed: %s",
                             result.Error().message);
            }
        }
        h.r3d_init_phase = 18u;
        return false;
    }

    if (h.r3d_init_phase == 18u) {
        if (h.q_ssr_on) {
            const auto result = h.hiz3d.Init(*dev, startup_w, startup_h);
            h.hiz3d_ready = result.IsOk();
            if (h.hiz3d_ready) {
                h.hiz3d_w = startup_w;
                h.hiz3d_h = startup_h;
            } else {
                h.hiz3d_w = 0u;
                h.hiz3d_h = 0u;
                ACS_LOG_WARN("[3D] CHiZ startup init failed: %s",
                             result.Error().message);
            }
        }
        h.r3d_init_phase = 19u;
        return false;
    }

    if (h.r3d_init_phase == 19u) {
        // The setting can be changed while the raw-DX12 compiler is running.
        // Keep this phase pending until the CPU-only job is joined, then either
        // commit on the owner thread or discard its bytecode when SSGI is off.
        if (h.startup_worker_kind == 2u) {
            const i32 worker_result = PollStartupWorker(h);
            if (worker_result == 0) {
                h.startup_phase_pending = true;
                return false;
            }
            h.startup_worker_kind = 0u;
            h.startup_phase_elapsed_override_ms =
                h.startup_worker_elapsed_ms;
            if (worker_result > 0 && h.q_ssgi_on) {
                const editor_profiler::FTimePoint commit_begin =
                    editor_profiler::CClock::now();
                const auto result = h.ssgi3d.InitWithCompiledShaders(
                    *dev,
                    Move(h.startup_ssgi_shaders),
                    startup_w,
                    startup_h);
                const f32 commit_ms =
                    editor_profiler::ElapsedMilliseconds(commit_begin);
                h.startup_phase_elapsed_override_ms += commit_ms;
                h.ssgi_ready = result.IsOk();
                if (h.ssgi_ready) {
                    h.ssgi_w = startup_w;
                    h.ssgi_h = startup_h;
                    ACS_LOG_INFO(
                        "[3D] CSsgi CPU compile %.2f ms; owner RHI commit %.2f ms",
                        h.startup_worker_elapsed_ms,
                        commit_ms);
                } else {
                    h.ssgi_w = 0u;
                    h.ssgi_h = 0u;
                    ACS_LOG_WARN(
                        "[3D] CSsgi owner-thread RHI commit failed: %s",
                        result.Error().message);
                }
            } else {
                h.startup_ssgi_shaders = {};
                h.ssgi_ready = false;
                h.ssgi_w = 0u;
                h.ssgi_h = 0u;
            }
            h.r3d_init_phase = 20u;
            return false;
        }
        if (h.q_ssgi_on) {
            h.ssgi_init_tried = true;
            const char* const backend_name = dev->BackendName();
            const bool raw_dx12 = backend_name != nullptr &&
                                  std::strcmp(backend_name, "DX12") == 0;
            if (raw_dx12) {
                if (h.startup_worker_state.load(std::memory_order_acquire) == 0 &&
                    !h.startup_worker.Joinable()) {
                    if (BeginSsgiCompileWorker(h)) {
                        h.startup_phase_pending = true;
                        return false;
                    }
                    ACS_LOG_WARN(
                        "[3D] SSGI CPU compile worker unavailable; using synchronous init");
                    const auto result =
                        h.ssgi3d.Init(*dev, startup_w, startup_h);
                        h.ssgi_ready = result.IsOk();
                        if (h.ssgi_ready) {
                            h.ssgi_w = startup_w;
                            h.ssgi_h = startup_h;
                        } else {
                            h.ssgi_w = 0u;
                            h.ssgi_h = 0u;
                            ACS_LOG_WARN("[3D] CSsgi startup init failed: %s",
                                         result.Error().message);
                        }
                } else {
                    const i32 worker_result = PollStartupWorker(h);
                    if (worker_result == 0) {
                        h.startup_phase_pending = true;
                        return false;
                    }
                    h.startup_worker_kind = 0u;
                    h.startup_phase_elapsed_override_ms =
                        h.startup_worker_elapsed_ms;
                    if (worker_result > 0) {
                        const editor_profiler::FTimePoint commit_begin =
                            editor_profiler::CClock::now();
                        const auto result =
                            h.ssgi3d.InitWithCompiledShaders(
                                *dev,
                                Move(h.startup_ssgi_shaders),
                                startup_w,
                                startup_h);
                        const f32 commit_ms =
                            editor_profiler::ElapsedMilliseconds(commit_begin);
                        h.startup_phase_elapsed_override_ms += commit_ms;
                        h.ssgi_ready = result.IsOk();
                        if (h.ssgi_ready) {
                            h.ssgi_w = startup_w;
                            h.ssgi_h = startup_h;
                            ACS_LOG_INFO(
                                "[3D] CSsgi CPU compile %.2f ms; owner RHI commit %.2f ms",
                                h.startup_worker_elapsed_ms,
                                commit_ms);
                        } else {
                            h.ssgi_w = 0u;
                            h.ssgi_h = 0u;
                            ACS_LOG_WARN(
                                "[3D] CSsgi owner-thread RHI commit failed: %s",
                                result.Error().message);
                        }
                    } else {
                        h.ssgi_ready = false;
                        h.ssgi_w = 0u;
                        h.ssgi_h = 0u;
                    }
                }
            } else {
                const auto result =
                    h.ssgi3d.Init(*dev, startup_w, startup_h);
                h.ssgi_ready = result.IsOk();
                if (h.ssgi_ready) {
                    h.ssgi_w = startup_w;
                    h.ssgi_h = startup_h;
                } else {
                    h.ssgi_w = 0u;
                    h.ssgi_h = 0u;
                    ACS_LOG_WARN("[3D] CSsgi startup init failed: %s",
                                 result.Error().message);
                }
            }
        }
        h.r3d_init_phase = 20u;
        return false;
    }

    if (h.r3d_init_phase == 20u) {
        const bool wants_motion = h.q_taa_on || h.q_ssr_on ||
                                  h.q_ssgi_on || h.q_motionblur_on;
        if (wants_motion) {
            const auto result = h.mv3d.Init(*dev, startup_w, startup_h);
            h.mv_ready = result.IsOk();
            if (h.mv_ready) {
                h.mv_w = startup_w;
                h.mv_h = startup_h;
            } else {
                h.mv_w = 0u;
                h.mv_h = 0u;
                ACS_LOG_WARN("[3D] CMotionVector startup init failed: %s",
                             result.Error().message);
            }
        }
        h.r3d_init_phase = 21u;
        return false;
    }

    if (h.r3d_init_phase == 21u) {
        if (!h.ortho3d &&
            (h.q_sky_mode == 1 || h.q_ap_on || h.q_fog_on)) {
            h.sky_atmo_tried = true;
            const auto result = h.sky_atmo.Init(*dev);
            if (result.IsErr()) {
                ACS_LOG_WARN("[3D] atmosphere startup init failed: %s",
                             result.Error().message);
            }
        }
        h.r3d_init_phase = 22u;
        return false;
    }

    if (h.r3d_init_phase == 22u) {
        const bool wants_clouds =
            !h.ortho3d && h.q_cloud_coverage > 0.001f;
        if (h.startup_worker_kind == 4u) {
            const i32 worker_result = PollStartupWorker(h);
            if (worker_result == 0) {
                h.startup_phase_pending = true;
                return false;
            }
            h.startup_worker_kind = 0u;
            h.startup_phase_elapsed_override_ms =
                h.startup_worker_elapsed_ms;
            if (worker_result > 0 && wants_clouds) {
                h.vclouds_tried = true;
                const editor_profiler::FTimePoint commit_begin =
                    editor_profiler::CClock::now();
                const auto result =
                    h.vclouds3d.InitWithCompiledShaders(
                        *dev, Move(h.startup_cloud_shaders),
                        EFormat::R16G16B16A16_Float);
                const f32 commit_ms =
                    editor_profiler::ElapsedMilliseconds(commit_begin);
                h.startup_phase_elapsed_override_ms += commit_ms;
                h.vclouds_ready = result.IsOk();
                if (h.vclouds_ready) {
                    ACS_LOG_INFO(
                        "[3D] CVolumetricClouds CPU compile %.2f ms; "
                        "owner RHI commit %.2f ms",
                        h.startup_worker_elapsed_ms, commit_ms);
                } else {
                    ACS_LOG_WARN(
                        "[3D] volumetric-cloud owner-thread RHI commit "
                        "failed: %s",
                        result.Error().message);
                }
            } else {
                h.startup_cloud_shaders = {};
                h.vclouds_ready = false;
                if (!wants_clouds) h.vclouds_tried = false;
            }
        } else if (h.startup_async_shader_kind == 4u) {
            if (!wants_clouds) {
                // Diligent explicitly supports releasing shader objects while
                // they compile. Do not hold an unwanted warm-up until finish.
                h.startup_cloud_shaders = {};
                h.startup_async_shader_kind = 0u;
                h.vclouds_ready = false;
                h.vclouds_tried = false;
            } else {
                const EShaderStatus shader_status =
                    h.startup_cloud_shaders.Status();
                if (shader_status == EShaderStatus::Compiling) {
                    h.startup_phase_pending = true;
                    return false;
                }
                const f32 compile_ms =
                    editor_profiler::ElapsedMilliseconds(
                        h.startup_async_shader_begin);
                h.startup_async_shader_kind = 0u;
                h.startup_phase_elapsed_override_ms = compile_ms;
                if (shader_status == EShaderStatus::Ready) {
                    h.vclouds_tried = true;
                    const editor_profiler::FTimePoint commit_begin =
                        editor_profiler::CClock::now();
                    const auto result =
                        h.vclouds3d.InitWithCompiledShaders(
                            *dev, Move(h.startup_cloud_shaders),
                            EFormat::R16G16B16A16_Float);
                    const f32 commit_ms =
                        editor_profiler::ElapsedMilliseconds(commit_begin);
                    h.startup_phase_elapsed_override_ms += commit_ms;
                    h.vclouds_ready = result.IsOk();
                    if (h.vclouds_ready) {
                        ACS_LOG_INFO(
                            "[3D] CVolumetricClouds backend compile %.2f ms; "
                            "owner RHI commit %.2f ms",
                            compile_ms, commit_ms);
                    } else {
                        ACS_LOG_WARN(
                            "[3D] volumetric-cloud owner-thread RHI commit "
                            "failed: %s",
                            result.Error().message);
                    }
                } else {
                    h.startup_cloud_shaders = {};
                    h.vclouds_ready = false;
                    ACS_LOG_ERROR(
                        "[3D] volumetric-cloud asynchronous shader "
                        "compile failed");
                }
            }
        } else if (wants_clouds &&
                   dev->SupportsAsyncShaderCompilation()) {
            h.vclouds_tried = true;
            const editor_profiler::FTimePoint submit_begin =
                editor_profiler::CClock::now();
            auto shader_result =
                CVolumetricClouds::BeginCompileShadersAsync(*dev);
            if (shader_result.IsOk()) {
                h.startup_cloud_shaders = Move(shader_result.Value());
                h.startup_async_shader_kind = 4u;
                h.startup_async_shader_begin = submit_begin;
                h.startup_phase_pending = true;
                return false;
            }
            h.vclouds_ready = false;
            ACS_LOG_ERROR(
                "[3D] volumetric-cloud asynchronous shader submission "
                "failed: %s",
                shader_result.Error().message);
        } else if (wants_clouds) {
            h.vclouds_tried = true;
            const char* const backend_name = dev->BackendName();
            const bool raw_dx12 = backend_name != nullptr &&
                                  std::strcmp(backend_name, "DX12") == 0;
            if (raw_dx12 &&
                h.startup_worker_state.load(std::memory_order_acquire) == 0 &&
                !h.startup_worker.Joinable()) {
                if (BeginCloudCompileWorker(h)) {
                    h.startup_phase_pending = true;
                    return false;
                }
                ACS_LOG_WARN(
                    "[3D] cloud CPU compile worker unavailable; "
                    "using synchronous init");
            }
            const auto result = h.vclouds3d.Init(
                *dev, EFormat::R16G16B16A16_Float);
            h.vclouds_ready = result.IsOk();
            if (!h.vclouds_ready) {
                ACS_LOG_WARN(
                    "[3D] volumetric-cloud startup init failed: %s",
                    result.Error().message);
            }
        }
        h.r3d_init_phase = 23u;
        return false;
    }

    if (h.r3d_init_phase == 23u) {
        if (h.vclouds_ready &&
            !h.vclouds3d.EnsureSize(
                *dev, startup_w, startup_h, h.q_cloud_render_scale)) {
            ACS_LOG_WARN("[3D] volumetric-cloud startup sizing failed");
        }
        h.r3d_init_phase = 24u;
        return false;
    }

    if (h.r3d_init_phase == 24u) {
        // Water is opt-in. Its shaders and sizeable per-draw constant-buffer
        // ring are initialized lazily only after a scene actually contains a
        // water component. This preserves bounded startup for ordinary scenes.
        h.water3d_ready = false;
        h.water3d_init_state = 0u;
        h.r3d_init_phase = 25u;
        return false;
    }

    if (h.r3d_init_phase == 25u) {
        // IBL construction records GPU work and therefore gets a dedicated
        // startup frame instead of running inside the first authored scene.
        h.renderer.BeginFrame(h.clear_color);
        IRhiCommandList* startup_cl = h.renderer.CommandList();
        if (startup_cl != nullptr) {
            Pass_AtmosphereIbl(h, startup_cl);
        } else {
            h.ibl_tried = true;
            h.ibl_dirty = false;
            ACS_LOG_WARN("[3D] startup IBL skipped: command list unavailable");
        }
        if (!h.renderer.EndFrame()) {
            h.startup_failed = true;
            ACS_LOG_ERROR(
                "[3D] startup IBL submit/present failed");
            return false;
        }
        h.resource_mutation_idle = false;
        h.r3d_init_phase = 26u;
        h.r3d_ready = true;
        return true;
    }
    return h.r3d_ready;
}

/** Rendering code only consumes a fully prepared 3D stack.  During startup,
 * AdvanceEditorStartup performs the bounded work before BeginFrame. */
bool Ensure3D(FEditorHost& h) noexcept {
    return h.r3d_ready;
}

constexpr u32 kEditorStartupStepCount = 28u;

const char* EditorStartupStepName(u32 step) noexcept {
    static constexpr const char* kNames[kEditorStartupStepCount] = {
        "2D sprite pipeline", "FXAA pipeline", "3D base mesh",
        "normal prepass", "refraction", "scene blit", "depth of field",
        "god rays", "motion blur", "editor overlay and fallback sky",
        "physical sky", "PBR", "3D sprite", "infinite grid",
        "shadow map", "frame buffers", "primitive meshes",
        "HDR post process", "SSAO", "screen-space reflections", "Hi-Z",
        "screen-space GI", "motion vectors", "atmosphere",
        "volumetric-cloud pipelines", "volumetric-cloud render targets",
        "interactive water", "initial image-based lighting"
    };
    return step < kEditorStartupStepCount ? kNames[step] : "complete";
}

/** Run one bounded startup phase before BeginFrame.
 *
 * Returning false is expected while more phases remain.  A phase is declared
 * failed only when it returns without advancing its explicit phase counter.
 */
bool AdvanceEditorStartup(FEditorHost& h) noexcept {
    if (h.startup_ready) return true;
    if (h.startup_failed || !h.attached) return false;
    IRhiDevice* dev = h.renderer.Device();
    if (dev == nullptr) {
        h.startup_failed = true;
        return false;
    }

    const u32 step = h.startup_step;
    const editor_profiler::FTimePoint begin = editor_profiler::CClock::now();
    if (step == 0u) {
        // Project settings are loaded after attach and before the first warm-up
        // step. Commit their requested sample count now so startup does not
        // build the default 8x pipeline and immediately rebuild it on frame 1.
        h.msaa_samples = h.msaa_pending;
        auto sr = h.sprites.Init(
            *dev, h.renderer.ColorFormat(), 8192, h.msaa_samples);
        if (sr.IsErr() && h.msaa_samples > 1u) {
            ACS_LOG_WARN(
                "[acs_editor_abi] MSAA %ux sprite batch init failed; retrying without MSAA",
                h.msaa_samples);
            h.msaa_samples = h.msaa_pending = 1u;
            h.sprites.Shutdown();
            sr = h.sprites.Init(
                *dev, h.renderer.ColorFormat(), 8192, 1u);
        }
        h.sprites_ready = sr.IsOk();
        if (!h.sprites_ready) {
            ACS_LOG_ERROR(
                "[acs_editor_abi] sprite batch init failed: %s",
                sr.Error().message);
            // SpriteBatch is the core 2D renderer and has no lower-quality
            // fallback after the 1x retry.  Do not publish Ready for a host
            // that cannot render its canonical scene path.
            h.startup_failed = true;
        } else {
            h.startup_step = 1u;
        }
    } else if (step == 1u) {
        const auto fr = h.fxaa.Init(*dev, h.renderer.ColorFormat());
        h.fxaa_ready = fr.IsOk();
        if (!h.fxaa_ready) {
            ACS_LOG_WARN(
                "[acs_editor_abi] FXAA init failed; continuing without FXAA: %s",
                fr.Error().message);
        }
        h.startup_step = 2u;
    } else {
        const u32 phaseBefore = h.r3d_init_phase;
        const bool ready = AdvanceEnsure3D(h);
        if (ready) {
            h.startup_step = kEditorStartupStepCount;
            h.startup_ready = true;
        } else if (h.r3d_init_phase == phaseBefore &&
                   !h.startup_phase_pending) {
            h.r3d_init_failed = true;
            h.startup_failed = true;
            ACS_LOG_ERROR(
                "[acs_editor_abi] startup phase failed: %s",
                EditorStartupStepName(step));
        } else {
            h.startup_step = 2u + h.r3d_init_phase;
        }
    }

    const bool phase_finished =
        h.startup_step != step || h.startup_failed;
    if (phase_finished) {
        const f32 measured =
            h.startup_phase_elapsed_override_ms >= 0.0f
                ? h.startup_phase_elapsed_override_ms
                : editor_profiler::ElapsedMilliseconds(begin);
        h.startup_phase_elapsed_override_ms = -1.0f;
        ACS_LOG_INFO(
            "[acs_editor_abi] startup %u/%u %s: %.2f ms",
            h.startup_step, kEditorStartupStepCount,
            EditorStartupStepName(step), measured);
    }
    if (h.startup_ready) {
        ACS_LOG_INFO(
            "[acs_editor_abi] renderer startup complete: %.2f ms total",
            editor_profiler::ElapsedMilliseconds(h.startup_begin));
    }
    return h.startup_ready;
}

/** prim 種別に対応する GPU メッシュを返す。 */
const FGpuMesh& Mesh3DFor(const FEditorHost& h, int prim) noexcept {
    if (prim == 1) return h.gm_sphere;
    if (prim == 2) return h.gm_plane;
    return h.gm_cube;
}


/** 軌道カメラの eye 位置を yaw/pitch/dist/target から求める。
 *  eye.y を床上にクランプ → 真上を向いても床下に潜らず «空» を見られる (EditorCam3D が pitch 方向を見る)。 */
FVec3 Cam3DEye(const FEditorHost& h) noexcept {
    const f32 cp = std::cos(h.cam3d_pitch), sp = std::sin(h.cam3d_pitch);
    const f32 cy = std::cos(h.cam3d_yaw),   sy = std::sin(h.cam3d_yaw);
    const FVec3 dir{ cp * sy, sp, cp * cy };          // target→eye 方向
    FVec3 eye{ h.cam3d_target.x + dir.x * h.cam3d_dist,
               h.cam3d_target.y + dir.y * h.cam3d_dist,
               h.cam3d_target.z + dir.z * h.cam3d_dist };
    if (!h.ortho3d) {                                  // 透視のみ: eye が床下に潜らないよう最低高さでクランプ
        const f32 minY = h.cam3d_target.y + 0.30f;
        if (eye.y < minY) eye.y = minY;
    }
    return eye;
}

/** エディタ 3D ビューのカメラを組む (透視 or 正射、軌道 eye + 注視点)。描画/射影/ピックで共通。 */
CCamera EditorCam3D(const FEditorHost& h, f32 aspect) noexcept {
    CCamera cam;
    if (h.ortho3d) {
        const f32 oh = h.cam3d_dist * 0.62f;             // 高さを軌道距離に比例 → ズーム感を保つ
        cam.SetOrthographic(oh * aspect, oh, 0.05f, 500.0f);
    } else {
        cam.SetPerspective(50.0f * 3.14159265f / 180.0f, aspect, 0.05f, 500.0f);
    }
    if (h.ortho3d) {
        cam.SetLookAt(Cam3DEye(h), h.cam3d_target);
    } else {
        // 注視点固定でなく «pitch/yaw 方向» を見る。eye 非クランプ時は look-at-target と完全に同一だが、
        // 真上を向いて eye が床上にクランプされたときは注視点ではなく空を向ける (空/雲/god rays を確認可能に)。
        const f32 cp = std::cos(h.cam3d_pitch), sp = std::sin(h.cam3d_pitch);
        const f32 cy = std::cos(h.cam3d_yaw),   sy = std::sin(h.cam3d_yaw);
        const FVec3 eye = Cam3DEye(h);
        const FVec3 fwd{ -cp * sy, -sp, -cp * cy };    // pitch/yaw 方向 (pitch 負で上向き)
        cam.SetLookAt(eye, FVec3{ eye.x + fwd.x, eye.y + fwd.y, eye.z + fwd.z });
    }
    return cam;
}

/** スクリーン点を z=0 平面のワールド座標(XY)へ逆射影する。視線が平面に平行なら false。 */
bool ScreenToZ0(const FEditorHost& h, f32 sx, f32 sy, f32 W, f32 H, FVec2& outXY) noexcept {
    const f32 aspect = (H > 0) ? W / H : 1.0f;
    const FRay3 ray = acs::ScreenPointToRay(EditorCam3D(h, aspect).ViewProjection(), sx, sy, W, H);
    if (std::abs(ray.direction.z) < 1e-6f) return false;      // 視線が z=0 平面に平行
    const f32 t = -ray.origin.z / ray.direction.z;
    outXY = FVec2{ ray.origin.x + ray.direction.x * t, ray.origin.y + ray.direction.y * t };
    return true;
}

/** スクリーン点 (sx,sy) を通すワールドレイ (origin=eye, dir 正規化) を返す。 */
struct FEGizRay { FVec3 origin; FVec3 dir; };
FEGizRay Cam3DScreenRay(const FEditorHost& h, f32 sx, f32 sy, f32 W, f32 H) noexcept {
    const FVec3 eye = Cam3DEye(h);
    const f32 cpit = std::cos(h.cam3d_pitch), spit = std::sin(h.cam3d_pitch);
    const f32 cyaw = std::cos(h.cam3d_yaw),   syaw = std::sin(h.cam3d_yaw);
    const FVec3 fwd{ -cpit * syaw, -spit, -cpit * cyaw };
    FVec3 right{ cyaw, 0, -syaw };
    FVec3 up{ right.y * fwd.z - right.z * fwd.y, right.z * fwd.x - right.x * fwd.z, right.x * fwd.y - right.y * fwd.x };
    const f32 aspect = (H > 0) ? W / H : 1.0f;
    const f32 tanHalf = std::tan(0.5f * 50.0f * 3.14159265f / 180.0f);
    const f32 ndcx = (2.0f * sx / W - 1.0f) * tanHalf * aspect;
    const f32 ndcy = (1.0f - 2.0f * sy / H) * tanHalf;
    FVec3 d{ fwd.x + right.x * ndcx + up.x * ndcy, fwd.y + right.y * ndcx + up.y * ndcy, fwd.z + right.z * ndcx + up.z * ndcy };
    const f32 dl = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
    if (dl > 1e-6f) d = FVec3{ d.x/dl, d.y/dl, d.z/dl };
    return FEGizRay{ eye, d };
}

/** ワールド点をスクリーン座標へ射影する (画面内かつ前方なら true)。 */
bool WorldToScreen3D(const FEditorHost& h, FVec3 wp, f32 W, f32 H, f32& outSx, f32& outSy) noexcept {
    const f32 aspect = (H > 0) ? W / H : 1.0f;
    const CCamera cam = EditorCam3D(h, aspect);          // 透視 or 正射
    const FMat4 vp = cam.ViewProjection();
    const FVec4 clip = Transform(FVec4{ wp.x, wp.y, wp.z, 1.0f }, vp);
    if (clip.w <= 1e-4f) return false;                  // カメラ後方 (正射では w=1 で常に可視)
    outSx = (clip.x / clip.w * 0.5f + 0.5f) * W;
    outSy = (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * H;
    return true;
}

/** 点 p からレイ [o,d] への «レイに沿った» 最近パラメータ t (= 射影距離)。 */
f32 RayParamForClosest(const FEGizRay& r, FVec3 p) noexcept {
    const FVec3 op{ p.x - r.origin.x, p.y - r.origin.y, p.z - r.origin.z };
    return op.x * r.dir.x + op.y * r.dir.y + op.z * r.dir.z;
}

/** 2 直線 (軸: A+s*ad, レイ: r) の最近接で «軸パラメータ s» を返す (移動ドラッグ用)。 */
f32 ClosestAxisParam(FVec3 A, FVec3 ad, const FEGizRay& r) noexcept {
    // ad, r.dir は正規化。s = ((B-A)·(ad - (ad·rd)rd)) / (1 - (ad·rd)^2)
    const FVec3 B = r.origin;
    const FVec3 BA{ B.x - A.x, B.y - A.y, B.z - A.z };
    const f32 adrd = ad.x*r.dir.x + ad.y*r.dir.y + ad.z*r.dir.z;
    const f32 denom = 1.0f - adrd * adrd;
    if (std::abs(denom) < 1e-5f) return 0.0f;           // 平行
    const f32 baAd = BA.x*ad.x + BA.y*ad.y + BA.z*ad.z;
    const f32 baRd = BA.x*r.dir.x + BA.y*r.dir.y + BA.z*r.dir.z;
    return (baAd - baRd * adrd) / denom;
}

/** 軸番号 (1=X,2=Y,3=Z) → 単位方向。 */
FVec3 AxisDir(int axis) noexcept {
    if (axis == 1) return FVec3{ 1, 0, 0 };
    if (axis == 2) return FVec3{ 0, 1, 0 };
    return FVec3{ 0, 0, 1 };
}

// 3D ノードアクセス補助 (実体は FindNode3D 群の直後)。Seed3DScene が先に使うため前方宣言。
game::ANode& AddNode3D(FEditorHost& h, const char* name) noexcept;

/** 初回 3D 切替でデフォルトの 3D シーン (床 + 立方体 + 球) を置く。 */
void Seed3DScene(FEditorHost& h) noexcept {
    if (h.scene3d_seeded) return;
    h.scene3d_seeded = true;
    {
        game::ANode& g = AddNode3D(h, "Ground");
        g.Local().position = FVec3{ 0, 0, 0 }; g.Local().scale = FVec3{ 16, 1, 16 };
        g.GetComponent<AEditor3DRecordComponent>()->id = h.next_id3d++;
        g.GetComponent<game::AMeshComponent3D>()->SetPrimitive(game::EMeshPrimitive3D::Plane);
        g.GetComponent<game::AMeshComponent3D>()->SetColor(FVec4{ 0.32f, 0.34f, 0.38f, 1 });
    }
    {
        game::ANode& box = AddNode3D(h, "Cube");
        box.Local().position = FVec3{ -1.4f, 0.5f, 0 };
        const int id = h.next_id3d++;
        box.GetComponent<AEditor3DRecordComponent>()->id = id; h.sel3d_multi.Reset(); h.sel3d_multi.Add(id); h.sel3d = id;
        box.GetComponent<game::AMeshComponent3D>()->SetPrimitive(game::EMeshPrimitive3D::Cube);
        box.GetComponent<game::AMeshComponent3D>()->SetColor(FVec4{ 0.85f, 0.45f, 0.35f, 1 });
    }
    {
        game::ANode& ball = AddNode3D(h, "Sphere");
        ball.Local().position = FVec3{ 1.3f, 0.6f, 0.2f }; ball.Local().scale = FVec3{ 1.2f, 1.2f, 1.2f };
        ball.GetComponent<AEditor3DRecordComponent>()->id = h.next_id3d++;
        ball.GetComponent<game::AMeshComponent3D>()->SetPrimitive(game::EMeshPrimitive3D::Sphere);
        ball.GetComponent<game::AMeshComponent3D>()->SetColor(FVec4{ 0.40f, 0.62f, 0.92f, 1 });
    }
}

/** 2D ポリゴン点列 (XY 平面、z=0) からフラットな AMeshAsset を作る (扇状三角形分割、法線+Z)。
 *  «2D は内部的に 3D 空間 (z=0) にある» を体現: 2D ポリゴンを 3D シーンのノードとして持つ。 */
TSharedPtr<AAsset> MakeFlatPolygon3D(const FVec2* pts, u32 n) noexcept {
    TSharedPtr<AMeshAsset> mesh;
    if (!Primitive::TryMakePolygonXY(pts, n, mesh)) return nullptr;
    return TSharedPtr<AAsset>(mesh);
}

// --- 3D ノードアクセス (各 editor ノード = root の子 ANode + AEditor3DRecordComponent + AMeshComponent3D) ---

/** ANode から AEditor3DRecordComponent (editor id + euler) を取り出す (無ければ null)。 */
AEditor3DRecordComponent* Rec3D(game::ANode* n) noexcept {
    return (n != nullptr) ? n->GetComponent<AEditor3DRecordComponent>() : nullptr;
}
const AEditor3DRecordComponent* Rec3D(
    const game::ANode* n) noexcept {
    return n != nullptr
        ? const_cast<game::ANode*>(n)
              ->GetComponent<AEditor3DRecordComponent>()
        : nullptr;
}

/** ANode から AMeshComponent3D (prim/color/mesh) を取り出す (無ければ null)。 */
game::AMeshComponent3D* Mesh3D(game::ANode* n) noexcept {
    return (n != nullptr) ? n->GetComponent<game::AMeshComponent3D>() : nullptr;
}

/** 3D ノードの material_path (.acsmat) を解析して material キャッシュへ読み込む。 */
void LoadNode3DMaterial(game::ANode* n) noexcept {
    game::AMeshComponent3D* mc = Mesh3D(n);
    if (mc == nullptr) return;
    AEditor3DRecordComponent* record = Rec3D(n);
    if (record != nullptr) {
        record->material_textures_loaded = false;
        record->material_albedo_tex.Reset();
        record->material_normal_tex.Reset();
        for (u32 slot = 0u;
             slot < kShaderExpressionMaxTextureSlots;
             ++slot) {
            record->material_expression_tex[slot].Reset();
        }
    }
    mc->MaterialMut() = game::FMaterial2D{};                      // 既定にリセット
    const FStringView mp = mc->MaterialPath();
    if (mp.Size() > 0) {
        char path[260];
        const u32 len = (mp.Size() < 259u) ? mp.Size() : 259u;
        std::memcpy(path, mp.Data(), len); path[len] = '\0';     // FStringView を NUL 終端化
        game::LoadAcsmatFile(path, mc->MaterialMut());           // LoadAcsmatFile は生 fopen → 絶対パス前提
    }
    mc->SetMaterialLoaded(true);
}

/**
 * Load legacy albedo/normal resources plus all four Substrate expression
 * TextureSample2D slots.  UploadTexture creates linear UNORM resources; an
 * expression node's sRGB flag is decoded exactly once by the material VM.
 */
void LoadNode3DMaterialTextures(
    FEditorHost& h, game::ANode* n) noexcept {
    AEditor3DRecordComponent* record = Rec3D(n);
    game::AMeshComponent3D* mc = Mesh3D(n);
    if (record == nullptr || mc == nullptr ||
        record->material_textures_loaded) {
        return;
    }
    const game::FMaterial2D& material = mc->Material();
    if (material.kind == game::EMaterialKind::Lit) {
        LoadTexFromPath(
            h, material.pbr.albedoPath,
            record->material_albedo_tex);
        LoadTexFromPath(
            h, material.pbr.normalPath,
            record->material_normal_tex);
        for (u32 slot = 0u;
             slot < kShaderExpressionMaxTextureSlots;
             ++slot) {
            LoadTexFromPath(
                h,
                material.substrateExpressionTexturePaths[slot],
                record->material_expression_tex[slot]);
        }
    }
    record->material_textures_loaded = true;
}

/** root 配下を DFS pre-order で集める (root 自身は除く)。前方宣言 (FindNode3DNode が使う)。 */
void Dfs3DCollect(game::ANode* n, TArray<game::ANode*>& out) noexcept;

game::ANode* FindNode3DNodeRecursive(
    game::ANode* parent, int id) noexcept {
    if (parent == nullptr) return nullptr;
    for (u32 i = 0; i < parent->ChildCount(); ++i) {
        game::ANode* child = parent->Child(i);
        if (child == nullptr) continue;
        AEditor3DRecordComponent* record = Rec3D(child);
        if (record != nullptr && record->id == id) return child;
        if (game::ANode* nested =
                FindNode3DNodeRecursive(child, id)) {
            return nested;
        }
    }
    return nullptr;
}

/** editor 整数 id で ANode を «木全体» から探索する (階層対応、無ければ null)。 */
game::ANode* FindNode3DNode(FEditorHost& h, int id) noexcept {
    return FindNode3DNodeRecursive(&h.scene3d.Root(), id);
}

// ----- 3D 選択集合の操作 (single/multi。primary = sel3d。2D の Sel* と対称) -----
bool Sel3DContains(const FEditorHost& h, int id) noexcept {
    for (u32 i = 0; i < h.sel3d_multi.Num(); ++i) if (h.sel3d_multi[i] == id) return true;
    return false;
}
/** 単一選択にする (集合を {id} に。id 不正/未知なら解除)。sel3d を直接いじる各所はこれを使う。 */
void SetSel3D(FEditorHost& h, int id) noexcept {
    h.sel3d_multi.Reset();
    if (id >= 0 && FindNode3DNode(h, id) != nullptr) { h.sel3d_multi.Add(id); h.sel3d = id; }
    else h.sel3d = -1;
}
/** id の選択を反転する (Ctrl+click)。追加なら primary、primary を外したら別の一員へ。 */
void ToggleSel3D(FEditorHost& h, int id) noexcept {
    if (id < 0 || FindNode3DNode(h, id) == nullptr) return;
    for (u32 i = 0; i < h.sel3d_multi.Num(); ++i) {
        if (h.sel3d_multi[i] == id) {
            h.sel3d_multi.RemoveAtSwap(i);
            if (h.sel3d == id) h.sel3d = (h.sel3d_multi.Num() > 0) ? h.sel3d_multi[h.sel3d_multi.Num() - 1] : -1;
            return;
        }
    }
    h.sel3d_multi.Add(id); h.sel3d = id;
}
/** 構造変更後、消えた id を除き primary を整える。 */
void PruneSel3D(FEditorHost& h) noexcept {
    for (u32 i = 0; i < h.sel3d_multi.Num();) {
        if (FindNode3DNode(h, h.sel3d_multi[i]) == nullptr) h.sel3d_multi.RemoveAtSwap(i);
        else ++i;
    }
    if (h.sel3d >= 0 && !Sel3DContains(h, h.sel3d))
        h.sel3d = (h.sel3d_multi.Num() > 0) ? h.sel3d_multi[h.sel3d_multi.Num() - 1] : -1;
    if (h.sel3d_multi.Num() == 0) h.sel3d = -1;
}

/** ノードの prim 種別 (AMeshComponent3D 無し or 不明は 0=Cube)。 */
int NPrim(game::ANode* n) noexcept {
    game::AMeshComponent3D* m = Mesh3D(n);
    return (m != nullptr) ? static_cast<int>(m->Primitive()) : 0;
}

/** ノードの色 (AMeshComponent3D 無しは既定灰)。 */
FVec4 NColor(game::ANode* n) noexcept {
    game::AMeshComponent3D* m = Mesh3D(n);
    return (m != nullptr) ? m->Color() : FVec4{ 0.80f, 0.80f, 0.85f, 1.0f };
}

/** ノードのカスタムメッシュ AMeshAsset (prim!=Mesh や未設定は null)。 */
const AMeshAsset* NMesh(game::ANode* n) noexcept {
    game::AMeshComponent3D* m = Mesh3D(n);
    return (m != nullptr) ? m->Mesh() : nullptr;
}

/** ノードを CPbrShader (DrawIndexed) で描くための FGpuMesh を返す。
 *  prim 0/1/2 は共有プリミティブ、prim 3 (Mesh/ポリゴン) はノードの AMeshAsset を on-demand
 *  アップロードして AEditor3DRecordComponent にキャッシュ (元メッシュが変われば再アップロード)。失敗は nullptr。 */
FGpuMesh* GpuMeshForNode3D(FEditorHost& h, game::ANode* nn) noexcept {
    const int prim = NPrim(nn);
    if (prim == 1) return &h.gm_sphere;
    if (prim == 2) return &h.gm_plane;
    if (prim != 3) return &h.gm_cube;                 // 0=Cube (既定)
    const AMeshAsset* cm = NMesh(nn);
    AEditor3DRecordComponent* rec = Rec3D(nn);
    if (cm == nullptr || rec == nullptr) return nullptr;
    if (rec->gm_cache_src != cm || rec->gm_cache.vertex_buffer.Get() == nullptr) {
        IRhiDevice* dev = h.renderer.Device();
        if (dev == nullptr) return nullptr;
        rec->gm_cache = FGpuMesh{};
        if (UploadMesh(*dev, *cm, rec->gm_cache).IsErr()) { rec->gm_cache_src = nullptr; return nullptr; }
        rec->gm_cache_src = cm;
    }
    return &rec->gm_cache;
}

constexpr game::FTypeId kWaterSurface3DTypeId =
    game::AcsTypeHash("AWaterSurface3DComponent");

/**
 * Return whether a node is active in the editor scene hierarchy.
 *
 * Visibility/enabled are inherited contracts: a hidden or disabled group must
 * suppress every descendant from rendering, picking and interaction even when
 * the descendant's local flags remain true.
 */
bool IsEffectivelyVisibleAndEnabled(
    const game::ANode* node) noexcept {
    const game::ANode* current = node;
    while (current != nullptr) {
        if (!current->IsVisible() || !current->IsEnabled() ||
            current->IsPendingDestroy()) {
            return false;
        }
        current = current->Parent();
    }
    return node != nullptr;
}

/** 3D prepass の batch 結果を使い、失敗時だけ scalar 判定へ戻る。 */
bool SceneMeshHierarchyVisible(const FEditorHost& host, u32 index, const game::ANode* node) noexcept {
    return host.scene_mesh_hierarchy_batch_ready ? host.scene_mesh_hierarchy_visibility.IsVisible(index) : IsEffectivelyVisibleAndEnabled(node);
}

/** 3D prepass の batch world を使い、失敗時だけ node の scalar 計算へ戻る。 */
game::FTransform3D SceneMeshWorldTransform(const FEditorHost& host, u32 index, const game::ANode* node) noexcept {
    if (host.scene_mesh_world_batch_ready && index < host.scene_mesh_world_batch.Count()) return host.scene_mesh_world_batch.At(index);
    return node != nullptr ? node->World() : game::FTransform3D{};
}

struct FDeterministicGameCamera2D {
    f32 center_x = 0.0f;
    f32 center_y = 0.0f;
    f32 zoom = 1.0f;
    f32 pan_x = 0.0f;
    f32 pan_y = 0.0f;
};

/**
 * Resolve the legacy 2D Game View camera from authored scene bounds only.
 *
 * Editor pan/zoom is deliberately excluded: changing Scene View navigation
 * must never alter the game camera. Empty scenes use a fixed origin/1x
 * default. The viewport only converts that world camera to screen-space pan.
 */
FDeterministicGameCamera2D ResolveDeterministicGameCamera2D(
    const FEditorHost& host, u32 viewport_width,
    u32 viewport_height) noexcept {
    FDeterministicGameCamera2D result{};
    f32 minimum_x = std::numeric_limits<f32>::max();
    f32 minimum_y = std::numeric_limits<f32>::max();
    f32 maximum_x = -std::numeric_limits<f32>::max();
    f32 maximum_y = -std::numeric_limits<f32>::max();
    bool found = false;
    for (u32 index = 0u; index < host.nodes.Num(); ++index) {
        const AEditorNode* node = host.nodes[index];
        if (!IsEffectivelyVisibleAndEnabled(node)) continue;
        const game::FTransform2D world = node->World2D();
        if (!std::isfinite(world.position.x) ||
            !std::isfinite(world.position.y) ||
            !std::isfinite(world.rotation) ||
            !std::isfinite(world.scale.x) ||
            !std::isfinite(world.scale.y) ||
            !std::isfinite(node->base)) {
            continue;
        }
        const f32 half_width =
            std::max(1.0f, std::fabs(node->base * world.scale.x) * 0.5f);
        const f32 half_height =
            std::max(1.0f, std::fabs(node->base * world.scale.y) * 0.5f);
        const f32 cosine = std::fabs(std::cos(world.rotation));
        const f32 sine = std::fabs(std::sin(world.rotation));
        const f32 extent_x =
            cosine * half_width + sine * half_height;
        const f32 extent_y =
            sine * half_width + cosine * half_height;
        minimum_x = std::min(minimum_x, world.position.x - extent_x);
        minimum_y = std::min(minimum_y, world.position.y - extent_y);
        maximum_x = std::max(maximum_x, world.position.x + extent_x);
        maximum_y = std::max(maximum_y, world.position.y + extent_y);
        found = true;
    }

    const f32 width = viewport_width > 1u
        ? static_cast<f32>(viewport_width) : 1280.0f;
    const f32 height = viewport_height > 1u
        ? static_cast<f32>(viewport_height) : 720.0f;
    if (found) {
        result.center_x = (minimum_x + maximum_x) * 0.5f;
        result.center_y = (minimum_y + maximum_y) * 0.5f;
        const f32 bounds_width = std::max(1.0f, maximum_x - minimum_x);
        const f32 bounds_height = std::max(1.0f, maximum_y - minimum_y);
        result.zoom = std::clamp(
            std::min(
                width * 0.85f / bounds_width,
                height * 0.85f / bounds_height),
            0.05f, 4.0f);
    }
    result.pan_x = width * 0.5f - result.center_x * result.zoom;
    result.pan_y = height * 0.5f - result.center_y * result.zoom;
    return result;
}

int WaterSurface3DSlot(
    const AEditor3DRecordComponent* record) noexcept {
    if (record == nullptr) return -1;
    for (u32 slot = 0u; slot < record->component_count; ++slot) {
        if (record->components[slot] == kWaterSurface3DTypeId) {
            return static_cast<int>(slot);
        }
    }
    return -1;
}

bool IsAuthoredWaterSurface(game::ANode* node) noexcept {
    if (!IsEffectivelyVisibleAndEnabled(node) ||
        WaterSurface3DSlot(Rec3D(node)) < 0) {
        return false;
    }
    const int primitive = NPrim(node);
    return primitive == 2 ||
           (primitive == 3 && NMesh(node) != nullptr);
}

FWaterSurface3DParams WaterSurface3DParamsFor(
    const AEditor3DRecordComponent* record) noexcept {
    FWaterSurface3DParams params{};
    const int slot = WaterSurface3DSlot(record);
    if (slot < 0) return params;
    const f32 (*values)[4] =
        record->comp_props[static_cast<u32>(slot)];
    params.shallow_color =
        FVec3{values[0][0], values[0][1], values[0][2]};
    params.deep_color =
        FVec3{values[1][0], values[1][1], values[1][2]};
    params.absorption =
        FVec3{values[2][0], values[2][1], values[2][2]};
    params.scattering =
        FVec3{values[3][0], values[3][1], values[3][2]};
    params.roughness = values[4][0];
    params.normal_strength = values[5][0];
    params.normal_tiling = values[6][0];
    params.flow_direction =
        FVec2{values[7][0], values[7][1]};
    params.wave_amplitude = values[8][0];
    params.wave_scale = values[9][0];
    params.wave_speed = values[10][0];
    params.ripple_speed = values[11][0];
    params.ripple_wavelength = values[12][0];
    params.ripple_lifetime = values[13][0];
    params.ripple_damping = values[14][0];
    params.refraction_strength = values[15][0];
    params.optical_depth = values[16][0];
    params.foam_intensity = values[17][0];
    params.phase_anisotropy = values[18][0];
    params.foam_color =
        FVec3{values[19][0], values[19][1], values[19][2]};
    return params;
}

bool IsValidCustomWaterSurfaceMesh(
    AEditor3DRecordComponent& record,
    const AMeshAsset* mesh) noexcept {
    if (record.water_surface_validation_src != mesh) {
        record.water_surface_validation_src = mesh;
        record.water_surface_local_xz =
            mesh != nullptr &&
            CWaterSurface3D::IsLocalXzSurfaceMesh(*mesh);
        record.water_surface_fallback_logged = false;
    }
    if (!record.water_surface_local_xz &&
        !record.water_surface_fallback_logged) {
        ACS_LOG_WARN(
            "[3D] water node %d custom mesh is not a valid local-XZ "
            "surface; using the tessellated grid fallback",
            record.id);
        record.water_surface_fallback_logged = true;
    }
    return record.water_surface_local_xz;
}

FGpuMesh* WaterGridFallback(FEditorHost& host) noexcept {
    return host.gm_water_plane.vertex_buffer &&
           host.gm_water_plane.index_buffer
        ? &host.gm_water_plane : nullptr;
}

FGpuMesh* WaterGpuMeshForNode3D(
    FEditorHost& host, game::ANode* node) noexcept {
    if (!IsAuthoredWaterSurface(node)) return nullptr;
    if (NPrim(node) == 2) {
        return WaterGridFallback(host);
    }
    if (NPrim(node) == 3) {
        AEditor3DRecordComponent* record = Rec3D(node);
        const AMeshAsset* source = NMesh(node);
        if (record == nullptr ||
            !IsValidCustomWaterSurfaceMesh(*record, source)) {
            return WaterGridFallback(host);
        }
        FGpuMesh* custom = GpuMeshForNode3D(host, node);
        return custom != nullptr
            ? custom : WaterGridFallback(host);
    }
    return nullptr;
}

const AMeshAsset* WaterCpuMeshForNode3D(
    FEditorHost& host, game::ANode* node) noexcept {
    FGpuMesh* const gpu_mesh =
        WaterGpuMeshForNode3D(host, node);
    if (gpu_mesh == nullptr) return nullptr;
    if (gpu_mesh == &host.gm_water_plane)
        return host.cpu_water_plane.Get();
    return NMesh(node);
}

bool Water3DPassAvailable(const FEditorHost& host) noexcept {
    IRhiTexture* scene_depth = host.renderer.DepthBuffer();
    IRhiTexture* depth_copy = host.water3d_depth_copy.Get();
    return host.water3d_ready && host.pbr3d_ready &&
           host.post3d_ready &&
           host.blit_ready &&
           !host.water3d_depth_copy_failed &&
           scene_depth != nullptr &&
           depth_copy != nullptr &&
           IsDepthTextureCopyCompatible(*scene_depth, *depth_copy) &&
           host.water3d_depth_copy_w == host.width &&
           host.water3d_depth_copy_h == host.height &&
           host.refr_bg.Get() != nullptr &&
           host.refr_bg_w == host.width &&
           host.refr_bg_h == host.height;
}

void PrepareWater3DDrawEligibility(
    FEditorHost& host,
    const TArray<game::ANode*>& nodes) noexcept {
    host.water3d_draw_count = 0u;
    if (!Water3DPassAvailable(host)) return;

    for (u32 i = 0u;
         i < nodes.Num() &&
         host.water3d_draw_count < CWaterSurface3D::kMaxTrackedSurfaces;
         ++i) {
        game::ANode* node = nodes[i];
        if (WaterGpuMeshForNode3D(host, node) == nullptr) continue;
        const AEditor3DRecordComponent* record = Rec3D(node);
        if (record == nullptr) continue;
        host.water3d_draw_ids[host.water3d_draw_count++] = record->id;
    }
}

bool IsRenderedByWater3D(
    FEditorHost& host, game::ANode* node) noexcept {
    if (!Water3DPassAvailable(host) ||
        WaterGpuMeshForNode3D(host, node) == nullptr) {
        return false;
    }
    const AEditor3DRecordComponent* record = Rec3D(node);
    if (record == nullptr) return false;
    for (u32 i = 0u; i < host.water3d_draw_count; ++i) {
        if (host.water3d_draw_ids[i] == record->id) return true;
    }
    return false;
}

/** 新規 3D ノードを生成して ANode への参照を返す (ANode + AEditor3DRecordComponent + AMeshComponent3D)。 */
game::ANode& AddNode3D(FEditorHost& h, const char* name) noexcept {
    game::ANode& n = h.scene3d.Spawn(FStringView((name != nullptr && name[0] != '\0') ? name : "Node"));
    n.AddComponent<AEditor3DRecordComponent>();
    game::AMeshComponent3D& m = n.AddComponent<game::AMeshComponent3D>();
    m.SetColor(FVec4{ 0.80f, 0.80f, 0.85f, 1.0f });   // 旧 ENode3D 既定色を踏襲
    return n;
}

/** root 配下を DFS pre-order で集める (root 自身は除く)。階層対応の列挙/描画/保存に使う。 */
void Dfs3DCollect(game::ANode* n, TArray<game::ANode*>& out) noexcept {
    if (n == nullptr) return;
    for (u32 i = 0; i < n->ChildCount(); ++i) {
        game::ANode* c = n->Child(i);
        if (c != nullptr) { out.Add(c); Dfs3DCollect(c, out); }
    }
}

constexpr int kSceneCameraMinPriority = -1000000;
constexpr int kSceneCameraMaxPriority = 1000000;

bool IsCanonicalSceneCameraId(const char* stable_id) noexcept {
    if (stable_id == nullptr || stable_id[0] == '\0') return false;
    u32 length = 0u;
    for (; stable_id[length] != '\0'; ++length) {
        if (length >= game::kScene3DSerializeMaxCameraIdBytes) return false;
        const char value = stable_id[length];
        const bool alpha = (value >= 'A' && value <= 'Z')
                        || (value >= 'a' && value <= 'z');
        const bool digit = value >= '0' && value <= '9';
        if (!alpha && !digit
            && (length == 0u || (value != '_' && value != '.'
                                 && value != '-'))) {
            return false;
        }
    }
    return length > 0u;
}

bool CopyCanonicalSceneCameraId(
    const char* source, char* destination,
    u32 destination_capacity) noexcept {
    if (source == nullptr || destination == nullptr ||
        destination_capacity <
            game::kScene3DSerializeMaxCameraIdBytes + 1u) {
        return false;
    }
    u32 length = 0u;
    for (; length <= game::kScene3DSerializeMaxCameraIdBytes; ++length) {
        const char value = source[length];
        if (value == '\0') break;
        const bool alpha =
            (value >= 'A' && value <= 'Z') ||
            (value >= 'a' && value <= 'z');
        const bool digit = value >= '0' && value <= '9';
        if (!alpha && !digit &&
            (length == 0u ||
             (value != '_' && value != '.' && value != '-'))) {
            return false;
        }
        destination[length] = value;
    }
    if (length == 0u ||
        length > game::kScene3DSerializeMaxCameraIdBytes) {
        destination[0] = '\0';
        return false;
    }
    destination[length] = '\0';
    return true;
}

bool IsSceneCameraConfigValid(
    int projection, int priority, int active,
    f32 fov_deg, f32 ortho_height,
    f32 near_plane, f32 far_plane) noexcept {
    return (projection == 0 || projection == 1)
        && priority >= kSceneCameraMinPriority
        && priority <= kSceneCameraMaxPriority
        && (active == 0 || active == 1)
        && std::isfinite(fov_deg) && fov_deg >= 1.0f && fov_deg <= 179.0f
        && std::isfinite(ortho_height)
        && ortho_height >= 0.001f && ortho_height <= 1000000.0f
        && std::isfinite(near_plane)
        && near_plane >= 0.0001f && near_plane <= 1000000.0f
        && std::isfinite(far_plane)
        && far_plane > near_plane && far_plane <= 1000000000.0f;
}

bool SceneCameraIdIsUnique(
    FEditorHost& host, int except_node_id, const char* stable_id) noexcept {
    TArray<game::ANode*> nodes;
    Dfs3DCollect(&host.scene3d.Root(), nodes);
    for (u32 index = 0u; index < nodes.Num(); ++index) {
        AEditor3DRecordComponent* record = Rec3D(nodes[index]);
        if (record == nullptr || !record->has_scene_camera
            || record->id == except_node_id) {
            continue;
        }
        if (std::strcmp(record->scene_camera_id, stable_id) == 0)
            return false;
    }
    return true;
}

bool MakeUniqueClonedSceneCameraId(
    FEditorHost& host, const char* source_id, int new_node_id,
    char* output, u32 output_capacity) noexcept {
    if (!IsCanonicalSceneCameraId(source_id) || output == nullptr
        || output_capacity
            < game::kScene3DSerializeMaxCameraIdBytes + 1u) {
        return false;
    }
    for (u32 attempt = 0u; attempt <= game::kScene3DSerializeMaxCameraCount;
         ++attempt) {
        char suffix[32]{};
        const int suffix_length = attempt == 0u
            ? std::snprintf(suffix, sizeof(suffix), "-copy-%d", new_node_id)
            : std::snprintf(
                suffix, sizeof(suffix), "-copy-%d-%u",
                new_node_id, attempt);
        if (suffix_length <= 0
            || static_cast<u32>(suffix_length)
                >= game::kScene3DSerializeMaxCameraIdBytes) {
            return false;
        }
        const u32 prefix_capacity =
            game::kScene3DSerializeMaxCameraIdBytes
            - static_cast<u32>(suffix_length);
        const u32 source_length =
            static_cast<u32>(std::strlen(source_id));
        const u32 prefix_length =
            source_length < prefix_capacity ? source_length : prefix_capacity;
        const int written = std::snprintf(
            output, output_capacity, "%.*s%s",
            static_cast<int>(prefix_length), source_id, suffix);
        if (written <= 0
            || static_cast<u32>(written)
                > game::kScene3DSerializeMaxCameraIdBytes) {
            return false;
        }
        if (SceneCameraIdIsUnique(host, new_node_id, output)) return true;
    }
    return false;
}

bool IsCanonicalPrefabInstanceId(const char* instance_id) noexcept {
    if (instance_id == nullptr) return false;
    for (u32 index = 0u; index < game::kScene3DSerializePrefabInstanceIdBytes; ++index) {
        const char value = instance_id[index];
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) return false;
    }
    return instance_id[game::kScene3DSerializePrefabInstanceIdBytes] == '\0';
}

bool PrefabInstanceIdIsUnique(
    FEditorHost& host, int except_node_id,
    const char* instance_id) noexcept {
    TArray<game::ANode*> nodes;
    Dfs3DCollect(&host.scene3d.Root(), nodes);
    for (u32 index = 0u; index < nodes.Num(); ++index) {
        AEditor3DRecordComponent* record = Rec3D(nodes[index]);
        if (record == nullptr || record->id == except_node_id || record->prefab_instance_id[0] == '\0') continue;
        if (std::strcmp(record->prefab_instance_id, instance_id) == 0) return false;
    }
    return true;
}

u64 HashPrefabInstanceSeed_Internal(const char* source_identity) noexcept {
    u64 hash = 14695981039346656037ull;
    if (source_identity == nullptr) return hash;
    for (u32 index = 0u; source_identity[index] != '\0'; ++index) {
        hash ^= static_cast<u8>(source_identity[index]);
        hash *= 1099511628211ull;
    }
    return hash;
}

bool MakeUniqueClonedPrefabInstanceId(
    FEditorHost& host, const char* source_identity, int new_node_id,
    char* output, u32 output_capacity) noexcept {
    if (source_identity == nullptr || source_identity[0] == '\0' || new_node_id < 0 || output == nullptr || output_capacity < game::kScene3DSerializePrefabInstanceIdBytes + 1u) return false;
    const u64 seed = HashPrefabInstanceSeed_Internal(source_identity);
    for (u32 attempt = 0u; attempt <= game::kScene3DSerializeMaxNodeCount; ++attempt) {
        u64 suffix = seed;
        suffix ^= static_cast<u32>(new_node_id);
        suffix *= 1099511628211ull;
        suffix ^= attempt;
        suffix *= 1099511628211ull;
        const int written = std::snprintf(
            output, output_capacity, "%08x%08x%016llx",
            static_cast<unsigned>(new_node_id), static_cast<unsigned>(attempt),
            static_cast<unsigned long long>(suffix));
        if (written != static_cast<int>(game::kScene3DSerializePrefabInstanceIdBytes)) return false;
        if (PrefabInstanceIdIsUnique(host, new_node_id, output)) return true;
    }
    return false;
}

bool SetPrefabLink3D_Internal(
    FEditorHost& host, int node_id, const char* source,
    const char* instance_id) noexcept {
    AEditor3DRecordComponent* record = Rec3D(FindNode3DNode(host, node_id));
    if (record == nullptr || source == nullptr || source[0] == '\0' || std::strlen(source) >= sizeof(record->prefab_src) || !IsCanonicalPrefabInstanceId(instance_id) || !PrefabInstanceIdIsUnique(host, node_id, instance_id)) return false;
    std::snprintf(record->prefab_src, sizeof(record->prefab_src), "%s", source);
    std::snprintf(record->prefab_instance_id, sizeof(record->prefab_instance_id), "%s", instance_id);
    return true;
}

bool IsEditorCameraNodeEffectivelyEnabled(
    const game::ANode& node) noexcept {
    const game::ANode* current = &node;
    u32 depth = 0u;
    while (current != nullptr) {
        if (current->IsPendingDestroy() || !current->IsEnabled()) return false;
        current = current->Parent();
        if (++depth > game::kNodeMaxTreeDepth) return false;
    }
    return true;
}

struct FResolvedSceneCamera3D {
    game::ANode* node = nullptr;
    AEditor3DRecordComponent* record = nullptr;
    game::FTransform3D world{};
};

bool SceneCameraRecordPrecedes(
    const AEditor3DRecordComponent& left,
    const AEditor3DRecordComponent& right) noexcept {
    if (left.scene_camera_active != right.scene_camera_active)
        return left.scene_camera_active;
    if (left.scene_camera_priority != right.scene_camera_priority)
        return left.scene_camera_priority > right.scene_camera_priority;
    const int identity_order =
        std::strcmp(left.scene_camera_id, right.scene_camera_id);
    if (identity_order != 0) return identity_order < 0;
    return left.id < right.id;
}

bool ResolveActiveCamera3DFromNodes(
    const TArray<game::ANode*>& nodes,
    FResolvedSceneCamera3D& output) noexcept {
    output = FResolvedSceneCamera3D{};
    for (u32 index = 0u; index < nodes.Num(); ++index) {
        game::ANode* node = nodes[index];
        AEditor3DRecordComponent* record = Rec3D(node);
        if (record == nullptr || !record->has_scene_camera
            || !IsEditorCameraNodeEffectivelyEnabled(*node)) {
            continue;
        }
        if (output.record == nullptr
            || SceneCameraRecordPrecedes(*record, *output.record)) {
            output.node = node;
            output.record = record;
        }
    }
    if (output.node == nullptr) return false;
    output.world = output.node->World();
    return std::isfinite(output.world.position.x)
        && std::isfinite(output.world.position.y)
        && std::isfinite(output.world.position.z);
}

bool ResolveActiveCamera3D(
    FEditorHost& host, FResolvedSceneCamera3D& output) noexcept {
    host.camera_resolve_nodes.Reset();
    Dfs3DCollect(
        &host.scene3d.Root(), host.camera_resolve_nodes);
    return ResolveActiveCamera3DFromNodes(
        host.camera_resolve_nodes, output);
}

bool ResolvePreviewCamera3DFromNodes(
    FEditorHost& host,
    const TArray<game::ANode*>& nodes,
    FResolvedSceneCamera3D& output) noexcept {
    output = FResolvedSceneCamera3D{};
    u64 request_id = 0u;
    int preview_node_id = -1;
    const char* expected_stable_id = nullptr;
    u32 request_history_generation = 0u;
    const bool request_presenter =
        host.camera_view_requests.PresenterIdentity(
            request_id,
            preview_node_id,
            expected_stable_id,
            request_history_generation);
    (void)request_id;
    (void)request_history_generation;
    if (!request_presenter)
        preview_node_id = host.game_camera_preview_node_id;
    if (preview_node_id < 0) return false;

    auto invalidate_preview = [&]() noexcept {
        if (request_presenter)
            host.camera_view_requests.MarkPresenterCameraStale();
        else
            host.game_camera_preview_node_id = -1;
    };
    for (u32 index = 0u; index < nodes.Num(); ++index) {
        game::ANode* node = nodes[index];
        AEditor3DRecordComponent* record = Rec3D(node);
        if (record == nullptr ||
            record->id != preview_node_id)
            continue;
        if (!record->has_scene_camera ||
            (request_presenter &&
             (expected_stable_id == nullptr ||
              std::strcmp(
                  record->scene_camera_id,
                  expected_stable_id) != 0)) ||
            !IsEditorCameraNodeEffectivelyEnabled(*node)) {
            invalidate_preview();
            return false;
        }
        output.node = node;
        output.record = record;
        output.world = node->World();
        if (std::isfinite(output.world.position.x) &&
            std::isfinite(output.world.position.y) &&
            std::isfinite(output.world.position.z)) {
            return true;
        }
        break;
    }
    invalidate_preview();
    output = FResolvedSceneCamera3D{};
    return false;
}

struct FRenderCamera3D {
    CCamera camera{};
    FVec3 eye{0.0f, 0.0f, 0.0f};
    FVec3 forward{0.0f, 0.0f, 1.0f};
    FVec3 up{0.0f, 1.0f, 0.0f};
    bool orthographic = false;
    bool authored = false;
    int node_id = -1;
    f32 fov_y_degrees = 50.0f;
    f32 orthographic_height = 10.0f;
    f32 near_plane = 0.05f;
    f32 far_plane = 500.0f;
};

bool NormalizeRenderCameraVector(
    FVec3 value, FVec3& output) noexcept {
    const f32 length_squared =
        value.x * value.x + value.y * value.y + value.z * value.z;
    if (!std::isfinite(length_squared) || length_squared <= 1.0e-12f)
        return false;
    const f32 inverse_length = 1.0f / std::sqrt(length_squared);
    output = value * inverse_length;
    return std::isfinite(output.x) &&
           std::isfinite(output.y) &&
           std::isfinite(output.z);
}

bool BuildResolvedRenderCamera3D(
    const FResolvedSceneCamera3D& resolved,
    f32 aspect, FRenderCamera3D& output) noexcept {
    if (resolved.node == nullptr || resolved.record == nullptr)
        return false;
    const AEditor3DRecordComponent& record = *resolved.record;
    if (!std::isfinite(aspect) || aspect <= 0.0f
        || !IsCanonicalSceneCameraId(record.scene_camera_id)
        || !IsSceneCameraConfigValid(
            record.scene_camera_projection,
            record.scene_camera_priority,
            record.scene_camera_active ? 1 : 0,
            record.scene_camera_fov_deg,
            record.scene_camera_ortho_height,
            record.scene_camera_near,
            record.scene_camera_far)) {
        return false;
    }
    FVec3 forward;
    FVec3 authored_up;
    FVec3 right;
    FVec3 up;
    if (!NormalizeRenderCameraVector(
            Rotate(
                resolved.world.rotation,
                FVec3{0.0f, 0.0f, 1.0f}),
            forward) ||
        !NormalizeRenderCameraVector(
            Rotate(
                resolved.world.rotation,
                FVec3{0.0f, 1.0f, 0.0f}),
            authored_up) ||
        !NormalizeRenderCameraVector(
            Cross(authored_up, forward), right) ||
        !NormalizeRenderCameraVector(
            Cross(forward, right), up)) {
        return false;
    }

    output = FRenderCamera3D{};
    output.eye = resolved.world.position;
    output.forward = forward;
    output.up = up;
    output.orthographic = record.scene_camera_projection == 1;
    output.authored = true;
    output.node_id = record.id;
    output.fov_y_degrees = record.scene_camera_fov_deg;
    output.orthographic_height = record.scene_camera_ortho_height;
    output.near_plane = record.scene_camera_near;
    output.far_plane = record.scene_camera_far;
    if (output.orthographic) {
        output.camera.SetOrthographic(
            output.orthographic_height * aspect,
            output.orthographic_height,
            output.near_plane, output.far_plane);
    } else {
        output.camera.SetPerspective(
            output.fov_y_degrees * 3.14159265f / 180.0f,
            aspect, output.near_plane, output.far_plane);
    }
    output.camera.SetLookAt(
        output.eye, output.eye + output.forward, output.up);
    return true;
}

bool BuildAuthoredRenderCamera3D(
    FEditorHost& host, f32 aspect,
    const TArray<game::ANode*>& nodes,
    bool allow_preview,
    FRenderCamera3D& output) noexcept {
    FResolvedSceneCamera3D resolved;
    if (allow_preview &&
        ResolvePreviewCamera3DFromNodes(
            host, nodes, resolved)) {
        if (BuildResolvedRenderCamera3D(
                resolved, aspect, output)) {
            return true;
        }
        host.game_camera_preview_node_id = -1;
    }
    if (!ResolveActiveCamera3DFromNodes(nodes, resolved))
        return false;
    return BuildResolvedRenderCamera3D(
        resolved, aspect, output);
}

void ComputeDeterministicGameBounds3D(
    FEditorHost& host,
    const TArray<game::ANode*>& nodes,
    FVec3& minimum,
    FVec3& maximum, bool& found) noexcept {
    minimum = FVec3{1.0e30f, 1.0e30f, 1.0e30f};
    maximum = FVec3{-1.0e30f, -1.0e30f, -1.0e30f};
    found = false;
    if (&nodes == &host.scene_mesh_nodes &&
        host.scene_mesh_cache_valid &&
        !host.scene_mesh_vertices.IsEmpty() &&
        std::isfinite(host.scene_mesh_bb_min.x) &&
        std::isfinite(host.scene_mesh_bb_min.y) &&
        std::isfinite(host.scene_mesh_bb_min.z) &&
        std::isfinite(host.scene_mesh_bb_max.x) &&
        std::isfinite(host.scene_mesh_bb_max.y) &&
        std::isfinite(host.scene_mesh_bb_max.z)) {
        minimum = host.scene_mesh_bb_min;
        maximum = host.scene_mesh_bb_max;
        found = true;
        return;
    }
    for (u32 index = 0u; index < nodes.Num(); ++index) {
        game::ANode* node = nodes[index];
        AEditor3DRecordComponent* record = Rec3D(node);
        if (!IsEffectivelyVisibleAndEnabled(node) ||
            Mesh3D(node) == nullptr ||
            (record != nullptr && record->is_empty)) {
            continue;
        }
        const game::FTransform3D world = node->World();
        if (!std::isfinite(world.position.x) ||
            !std::isfinite(world.position.y) ||
            !std::isfinite(world.position.z) ||
            !std::isfinite(world.scale.x) ||
            !std::isfinite(world.scale.y) ||
            !std::isfinite(world.scale.z)) {
            continue;
        }
        const f32 extent = std::max(
            0.5f,
            std::max(
                std::abs(world.scale.x),
                std::max(
                    std::abs(world.scale.y),
                    std::abs(world.scale.z))));
        minimum.x = std::min(minimum.x, world.position.x - extent);
        minimum.y = std::min(minimum.y, world.position.y - extent);
        minimum.z = std::min(minimum.z, world.position.z - extent);
        maximum.x = std::max(maximum.x, world.position.x + extent);
        maximum.y = std::max(maximum.y, world.position.y + extent);
        maximum.z = std::max(maximum.z, world.position.z + extent);
        found = true;
    }
}

void BuildDeterministicFallbackCamera3D(
    FEditorHost& host,
    const TArray<game::ANode*>& nodes,
    f32 aspect,
    FRenderCamera3D& output) noexcept {
    FVec3 minimum;
    FVec3 maximum;
    bool found = false;
    ComputeDeterministicGameBounds3D(
        host, nodes, minimum, maximum, found);
    if (!found) {
        minimum = FVec3{-2.0f, -1.0f, -2.0f};
        maximum = FVec3{2.0f, 3.0f, 2.0f};
    }
    const FVec3 center{
        (minimum.x + maximum.x) * 0.5f,
        (minimum.y + maximum.y) * 0.5f,
        (minimum.z + maximum.z) * 0.5f};
    const FVec3 extent{
        maximum.x - minimum.x,
        maximum.y - minimum.y,
        maximum.z - minimum.z};
    const f32 radius = std::max(
        1.0f,
        0.5f * std::sqrt(
            extent.x * extent.x +
            extent.y * extent.y +
            extent.z * extent.z));
    FVec3 forward;
    (void)NormalizeRenderCameraVector(
        FVec3{-0.55f, -0.35f, -0.76f}, forward);
    const f32 distance = std::max(
        5.0f,
        radius /
            std::tan(25.0f * 3.14159265f / 180.0f) *
            1.18f);

    output = FRenderCamera3D{};
    output.eye = center - forward * distance;
    output.forward = forward;
    output.up = FVec3{0.0f, 1.0f, 0.0f};
    output.near_plane = 0.05f;
    output.far_plane = std::max(
        500.0f, distance + radius * 6.0f);
    output.camera.SetPerspective(
        output.fov_y_degrees * 3.14159265f / 180.0f,
        aspect, output.near_plane, output.far_plane);
    output.camera.SetLookAt(
        output.eye, center, output.up);
}

FRenderCamera3D ResolveGameRenderCamera3D(
    FEditorHost& host, f32 aspect,
    const TArray<game::ANode*>& nodes) noexcept {
    FRenderCamera3D game_camera;
    if (BuildAuthoredRenderCamera3D(
            host, aspect, nodes, true, game_camera)) {
        return game_camera;
    }
    BuildDeterministicFallbackCamera3D(
        host, nodes, aspect, game_camera);
    return game_camera;
}

FRenderCamera3D ResolveRenderCamera3D(
    FEditorHost& host, f32 aspect,
    const TArray<game::ANode*>& scene_nodes) noexcept {
    if (host.game_view) {
        return ResolveGameRenderCamera3D(
            host, aspect, scene_nodes);
    }

    FRenderCamera3D editor_camera;
    editor_camera.camera = EditorCam3D(host, aspect);
    editor_camera.eye = Cam3DEye(host);
    FVec3 forward{
        host.cam3d_target.x - editor_camera.eye.x,
        host.cam3d_target.y - editor_camera.eye.y,
        host.cam3d_target.z - editor_camera.eye.z};
    if (!NormalizeRenderCameraVector(
            forward, editor_camera.forward)) {
        editor_camera.forward = FVec3{0.0f, 0.0f, -1.0f};
    }
    editor_camera.orthographic = host.ortho3d;
    editor_camera.node_id = -2;
    editor_camera.fov_y_degrees = 50.0f;
    editor_camera.orthographic_height =
        host.cam3d_dist * 0.62f;
    editor_camera.near_plane = 0.05f;
    editor_camera.far_plane = 500.0f;
    return editor_camera;
}

bool IsCurrentTemporalRenderCamera3D(
    FEditorHost& host, const game::ANode* node) noexcept {
    if (!host.game_view || node == nullptr ||
        host.last_render_camera_node_id < 0) {
        return false;
    }
    const AEditor3DRecordComponent* record = Rec3D(node);
    return record != nullptr && record->has_scene_camera &&
           record->id == host.last_render_camera_node_id;
}

bool TransformAffectsCurrentTemporalRenderCamera3D(
    FEditorHost& host, const game::ANode* mutated_node) noexcept {
    if (!host.game_view || mutated_node == nullptr ||
        host.last_render_camera_node_id < 0) {
        return false;
    }
    const game::ANode* camera_node =
        FindNode3DNode(host, host.last_render_camera_node_id);
    if (camera_node == nullptr) return false;
    const AEditor3DRecordComponent* camera_record =
        Rec3D(camera_node);
    if (camera_record == nullptr || !camera_record->has_scene_camera) {
        return false;
    }
    // A local transform edit changes the physical camera only when it targets
    // the rendered camera itself or one of its transform ancestors. Runtime
    // camera motion and unrelated mesh edits never pass through this editor
    // mutation helper, so their temporal histories remain warm.
    for (const game::ANode* cursor = camera_node;
         cursor != nullptr; cursor = cursor->Parent()) {
        if (cursor == mutated_node) return true;
    }
    return false;
}

bool AlignSceneCameraNodeToView(
    FEditorHost& host, game::ANode& node) noexcept {
    const CCamera scene_view = EditorCam3D(host, 1.0f);
    const FMat4 camera_world_matrix = Inverse(scene_view.View());
    const FVec3 desired_position = scene_view.Eye();
    const FQuat desired_rotation = FQuat::FromMatrix(camera_world_matrix);
    if (!std::isfinite(desired_position.x)
        || !std::isfinite(desired_position.y)
        || !std::isfinite(desired_position.z)) {
        return false;
    }

    FVec3 local_position = desired_position;
    FQuat local_rotation = desired_rotation;
    if (const game::ANode* parent = node.Parent()) {
        const game::FTransform3D parent_world = parent->World();
        if (!std::isfinite(parent_world.position.x)
            || !std::isfinite(parent_world.position.y)
            || !std::isfinite(parent_world.position.z)
            || !std::isfinite(parent_world.scale.x)
            || !std::isfinite(parent_world.scale.y)
            || !std::isfinite(parent_world.scale.z)
            || std::abs(parent_world.scale.x) < 1.0e-6f
            || std::abs(parent_world.scale.y) < 1.0e-6f
            || std::abs(parent_world.scale.z) < 1.0e-6f) {
            return false;
        }
        const FVec3 delta = desired_position - parent_world.position;
        const FVec3 parent_space =
            Rotate(Inverse(parent_world.rotation), delta);
        local_position = FVec3{
            parent_space.x / parent_world.scale.x,
            parent_space.y / parent_world.scale.y,
            parent_space.z / parent_world.scale.z};
        local_rotation =
            desired_rotation * Inverse(parent_world.rotation);
    }
    game::FTransform3D candidate = node.Local();
    candidate.position = local_position;
    candidate.rotation = local_rotation;
    const FVec3 euler = candidate.EulerDeg();
    if (!std::isfinite(local_position.x)
        || !std::isfinite(local_position.y)
        || !std::isfinite(local_position.z)
        || !std::isfinite(euler.x)
        || !std::isfinite(euler.y)
        || !std::isfinite(euler.z)) {
        return false;
    }
    AEditor3DRecordComponent* record = Rec3D(&node);
    if (record == nullptr) return false;
    const bool resets_temporal_history =
        TransformAffectsCurrentTemporalRenderCamera3D(
            host, &node);
    PushUndo(host);
    node.Local().position = local_position;
    node.Local().rotation = local_rotation;
    record->euler = euler;
    if (resets_temporal_history)
        InvalidateTemporalRenderHistories(host);
    return true;
}

struct FEditorWaterHit {
    int node_id = -1;
    FVec3 world_point{0, 0, 0};
    f32 ray_distance = std::numeric_limits<f32>::max();
};

bool IsFiniteWaterHitValue(FVec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool IsFiniteWaterHitValue(const FMat4& value) noexcept {
    for (u32 row = 0u; row < 4u; ++row) {
        for (u32 column = 0u; column < 4u; ++column) {
            if (!std::isfinite(value.m[row][column])) return false;
        }
    }
    return true;
}

bool IntersectWaterTriangle(
    FVec3 origin, FVec3 direction,
    FVec3 a, FVec3 b, FVec3 c,
    f32& out_t) noexcept {
    const FVec3 edge1 = b - a;
    const FVec3 edge2 = c - a;
    const FVec3 p = Cross(direction, edge2);
    const f32 determinant = Dot(edge1, p);
    if (!std::isfinite(determinant) ||
        std::abs(determinant) <= 1e-8f) {
        return false;
    }
    const f32 inverse = 1.0f / determinant;
    const FVec3 offset = origin - a;
    const f32 u = Dot(offset, p) * inverse;
    if (u < 0.0f || u > 1.0f) return false;
    const FVec3 q = Cross(offset, edge1);
    const f32 v = Dot(direction, q) * inverse;
    if (v < 0.0f || u + v > 1.0f) return false;
    const f32 t = Dot(edge2, q) * inverse;
    if (!std::isfinite(t) || t < 0.0f) return false;
    out_t = t;
    return true;
}

bool EditorOpaqueRayDistance(
    game::ANode* node, const FRay3& ray,
    f32& out_distance) noexcept {
    if (!IsEffectivelyVisibleAndEnabled(node)) {
        return false;
    }
    AEditor3DRecordComponent* record = Rec3D(node);
    if (record == nullptr || record->is_empty ||
        Mesh3D(node) == nullptr) {
        return false;
    }

    const FMat4 model = node->World().ToMat4();
    const FMat4 inverse_model = Inverse(model);
    if (!IsFiniteWaterHitValue(inverse_model)) return false;
    const FRay3 local_ray{
        TransformPoint(ray.origin, inverse_model),
        TransformVector(ray.direction, inverse_model)
    };
    if (!IsFiniteWaterHitValue(local_ray.origin) ||
        !IsFiniteWaterHitValue(local_ray.direction)) {
        return false;
    }

    FRayHit3 hit{};
    switch (NPrim(node)) {
    case 0:
        hit = RaycastAabb(
            local_ray,
            FAabb3{FVec3{0, 0, 0}, FVec3{0.5f, 0.5f, 0.5f}});
        break;
    case 1:
        hit = RaycastSphere(
            local_ray, FSphere{FVec3{0, 0, 0}, 0.5f});
        break;
    case 2:
        hit = RaycastAabb(
            local_ray,
            FAabb3{FVec3{0, 0, 0}, FVec3{0.5f, 0.02f, 0.5f}});
        break;
    case 3: {
        const AMeshAsset* mesh = NMesh(node);
        if (mesh == nullptr || mesh->Vertices().Num() == 0u ||
            mesh->Indices().Num() < 3u) {
            return false;
        }
        if (record->water_hit_collider_src != mesh) {
            record->water_hit_collider.Clear();
            const auto build =
                record->water_hit_collider.BuildFromMesh(*mesh);
            record->water_hit_collider_src =
                build.IsOk() ? mesh : nullptr;
        }
        if (record->water_hit_collider_src != mesh) return false;
        hit = record->water_hit_collider.Raycast(local_ray);
        break;
    }
    default:
        return false;
    }
    if (!hit.hit) return false;

    const FVec3 world_point = TransformPoint(hit.point, model);
    if (!IsFiniteWaterHitValue(world_point)) return false;
    out_distance = Dot(world_point - ray.origin, ray.direction);
    return std::isfinite(out_distance) && out_distance >= 0.0f;
}

bool HitTestEditorWaterSurface(
    FEditorHost& host, f32 screen_x, f32 screen_y,
    f32 viewport_width, f32 viewport_height,
    FEditorWaterHit& out_hit) noexcept {
    if (!std::isfinite(screen_x) || !std::isfinite(screen_y) ||
        !std::isfinite(viewport_width) || !std::isfinite(viewport_height) ||
        viewport_width <= 0.0f || viewport_height <= 0.0f) {
        return false;
    }
    const f32 aspect = viewport_width / viewport_height;
    const FRay3 ray = ScreenPointToRay(
        EditorCam3D(host, aspect).ViewProjection(),
        screen_x, screen_y, viewport_width, viewport_height);
    TArray<game::ANode*> nodes;
    Dfs3DCollect(&host.scene3d.Root(), nodes);
    bool found = false;
    for (u32 i = 0u; i < nodes.Num(); ++i) {
        game::ANode* node = nodes[i];
        AEditor3DRecordComponent* record = Rec3D(node);
        if (!IsAuthoredWaterSurface(node) || record == nullptr) continue;
        const FMat4 model = node->World().ToMat4();
        const FMat4 inverse_model = Inverse(model);
        if (!IsFiniteWaterHitValue(inverse_model)) continue;
        const FVec3 local_origin =
            TransformPoint(ray.origin, inverse_model);
        const FVec3 local_direction =
            TransformVector(ray.direction, inverse_model);
        if (!IsFiniteWaterHitValue(local_origin) ||
            !IsFiniteWaterHitValue(local_direction)) {
            continue;
        }
        FVec3 local_point{};
        const AMeshAsset* mesh =
            NPrim(node) == 3 ? NMesh(node) : nullptr;
        const bool use_custom_mesh =
            NPrim(node) == 3 &&
            IsValidCustomWaterSurfaceMesh(*record, mesh);
        if (use_custom_mesh) {
            if (record->water_hit_collider_src != mesh) {
                record->water_hit_collider.Clear();
                const auto build =
                    record->water_hit_collider.BuildFromMesh(*mesh);
                record->water_hit_collider_src =
                    build.IsOk() ? mesh : nullptr;
            }
            if (record->water_hit_collider_src != mesh) continue;
            const FRayHit3 mesh_hit =
                record->water_hit_collider.Raycast(
                    FRay3{local_origin, local_direction});
            if (!mesh_hit.hit) continue;
            local_point = mesh_hit.point;
        } else {
            if (std::abs(local_direction.y) <= 1e-7f) continue;
            const f32 local_t =
                -local_origin.y / local_direction.y;
            if (!std::isfinite(local_t) || local_t < 0.0f) continue;
            local_point =
                local_origin + local_direction * local_t;
            constexpr f32 kBoundsEpsilon = 1e-4f;
            if (local_point.x < -0.5f - kBoundsEpsilon ||
                local_point.x > 0.5f + kBoundsEpsilon ||
                local_point.z < -0.5f - kBoundsEpsilon ||
                local_point.z > 0.5f + kBoundsEpsilon) {
                continue;
            }
        }
        const FVec3 world_point = TransformPoint(local_point, model);
        if (!IsFiniteWaterHitValue(world_point)) continue;
        const FVec3 offset = world_point - ray.origin;
        const f32 distance = Dot(offset, ray.direction);
        if (!std::isfinite(distance) || distance < 0.0f ||
            distance >= out_hit.ray_distance) {
            continue;
        }
        out_hit.node_id = record->id;
        out_hit.world_point = world_point;
        out_hit.ray_distance = distance;
        found = true;
    }
    if (found) {
        // Pointer interaction follows what the user can actually see. A mesh
        // in front of the selected water hit must consume the pointer rather
        // than allowing a wake to appear through opaque geometry.
        constexpr f32 kOcclusionEpsilon = 1e-4f;
        for (u32 i = 0u; i < nodes.Num(); ++i) {
            game::ANode* node = nodes[i];
            if (IsAuthoredWaterSurface(node)) continue;
            f32 opaque_distance = 0.0f;
            if (EditorOpaqueRayDistance(
                    node, ray, opaque_distance) &&
                opaque_distance + kOcclusionEpsilon <
                    out_hit.ray_distance) {
                return false;
            }
        }
    }
    return found;
}

bool SceneHasAuthoredWater(
    const TArray<game::ANode*>& nodes) noexcept {
    for (u32 i = 0u; i < nodes.Num(); ++i) {
        if (IsAuthoredWaterSurface(nodes[i])) return true;
    }
    return false;
}

bool AdvanceWater3DInitialization(
    FEditorHost& host,
    const TArray<game::ANode*>& nodes) noexcept {
    const bool requested = SceneHasAuthoredWater(nodes);

    // Drain optional work before the feature gate. A hot material/component
    // edit can remove the final water surface while backend shader compilation
    // or the bounded pipeline candidate is still in flight.
    if (!requested) {
        if (host.water3d_init_state == 1u) {
            const EShaderStatus status =
                host.water3d_pending_shaders.Status();
            if (status == EShaderStatus::Compiling) return false;
            host.water3d_pending_shaders = {};
            host.water3d.Shutdown();
            host.water3d_ready = false;
            host.water3d_init_state = 0u;
        } else if (host.water3d_init_state == 4u) {
            // State 4 is unpublished and has never been recorded for drawing;
            // discard its partial constant-buffer ring without advancing it.
            host.water3d_pending_shaders = {};
            host.water3d.Shutdown();
            host.water3d_ready = false;
            host.water3d_init_state = 0u;
        }
        return false;
    }

    if (host.water3d_ready || host.water3d_init_state == 3u) {
        return false;
    }
    IRhiDevice* device = host.renderer.Device();
    if (device == nullptr) return false;
    if (host.water3d_init_state == 0u) {
        if (device->SupportsAsyncShaderCompilation()) {
            auto result =
                CWaterSurface3D::BeginCompileShadersAsync(*device);
            if (result.IsOk()) {
                host.water3d_pending_shaders = Move(result.Value());
                host.water3d_init_state = 1u;
                return false;
            }
            ACS_LOG_WARN(
                "[3D] interactive-water asynchronous submission failed; "
                "using owner-thread fallback: %s",
                result.Error().message);
        }
        const editor_profiler::FTimePoint begin =
            editor_profiler::CClock::now();
        const auto result = host.water3d.Init(
            *device, EFormat::R16G16B16A16_Float,
            host.renderer.DepthFormat(), 1u);
        const f32 elapsed =
            editor_profiler::ElapsedMilliseconds(begin);
        host.water3d_ready = result.IsOk();
        host.water3d_init_state =
            host.water3d_ready ? 2u : 3u;
        if (host.water3d_ready) {
            ACS_LOG_INFO(
                "[3D] interactive-water owner initialization %.2f ms",
                elapsed);
        } else {
            ACS_LOG_WARN(
                "[3D] interactive-water initialization failed; "
                "opaque PBR fallback remains active: %s",
                result.Error().message);
        }
        return true;
    }
    if (host.water3d_init_state == 4u) {
        const editor_profiler::FTimePoint begin =
            editor_profiler::CClock::now();
        auto advance =
            host.water3d.AdvanceInitialization(16u);
        const f32 elapsed =
            editor_profiler::ElapsedMilliseconds(begin);
        if (advance.IsErr()) {
            host.water3d_init_state = 3u;
            host.water3d_ready = false;
            ACS_LOG_WARN(
                "[3D] interactive-water bounded RHI commit failed; "
                "opaque PBR fallback remains active: %s",
                advance.Error().message);
        } else if (advance.Value()) {
            host.water3d_init_state = 2u;
            host.water3d_ready = true;
            ACS_LOG_INFO(
                "[3D] interactive-water bounded RHI commit complete "
                "(last slice %.2f ms)",
                elapsed);
        }
        return true;
    }
    if (host.water3d_init_state != 1u) return false;
    const EShaderStatus status =
        host.water3d_pending_shaders.Status();
    if (status == EShaderStatus::Compiling) return false;
    if (status != EShaderStatus::Ready) {
        host.water3d_pending_shaders = {};
        host.water3d_init_state = 3u;
        ACS_LOG_WARN(
            "[3D] interactive-water asynchronous compile failed; "
            "opaque PBR fallback remains active");
        return false;
    }
    const editor_profiler::FTimePoint commit_begin =
        editor_profiler::CClock::now();
    const auto result = host.water3d.BeginInitWithCompiledShaders(
        *device, Move(host.water3d_pending_shaders),
        EFormat::R16G16B16A16_Float,
        host.renderer.DepthFormat(), 1u);
    const f32 commit_ms =
        editor_profiler::ElapsedMilliseconds(commit_begin);
    host.water3d_ready = false;
    host.water3d_init_state =
        result.IsOk() ? 4u : 3u;
    if (result.IsOk()) {
        ACS_LOG_INFO(
            "[3D] interactive-water shader/PSO owner commit %.2f ms; "
            "constant buffers continue in bounded slices",
            commit_ms);
    } else {
        ACS_LOG_WARN(
            "[3D] interactive-water owner RHI commit failed; "
            "opaque PBR fallback remains active: %s",
            result.Error().message);
    }
    return true;
}

bool EnsureWater3DBackgroundBeforeFrame(
    FEditorHost& host,
    const TArray<game::ANode*>& nodes) noexcept {
    if (!host.water3d_ready || !host.post3d_ready ||
        !host.blit_ready || !SceneHasAuthoredWater(nodes) ||
        host.width == 0u || host.height == 0u ||
        host.renderer.DepthBuffer() == nullptr) {
        return false;
    }
    IRhiTexture* scene_depth = host.renderer.DepthBuffer();
    if (scene_depth->Width() != host.width ||
        scene_depth->Height() != host.height ||
        scene_depth->PixelFormat() != EFormat::D32_Float) {
        return false;
    }
    const bool background_ready =
        host.refr_bg &&
        host.refr_bg_w == host.width &&
        host.refr_bg_h == host.height;
    const bool depth_copy_ready =
        host.water3d_depth_copy &&
        host.water3d_depth_copy_w == host.width &&
        host.water3d_depth_copy_h == host.height &&
        IsDepthTextureCopyCompatible(
            *scene_depth, *host.water3d_depth_copy);
    if (background_ready && depth_copy_ready) return false;
    if (host.water3d_background_failed ||
        host.water3d_depth_copy_failed) {
        return false;
    }

    IRhiDevice* device = host.renderer.Device();
    if (device == nullptr) return false;

    TUniquePtr<IRhiTexture> background_candidate;
    if (!background_ready) {
        FTextureDesc description{};
        description.width = host.width;
        description.height = host.height;
        description.format = EFormat::R16G16B16A16_Float;
        description.is_render_target = true;
        auto texture = CreateRhiTexture(*device, description);
        if (texture.IsErr()) {
            host.water3d_background_failed = true;
            ACS_LOG_WARN(
                "[3D] interactive-water scene background unavailable; "
                "opaque PBR fallback remains active: %s",
                texture.Error().message);
            return true;
        }
        background_candidate = Move(texture.Value());
    }

    TUniquePtr<IRhiTexture> depth_candidate;
    if (!depth_copy_ready) {
        FTextureDesc description{};
        description.width = host.width;
        description.height = host.height;
        description.format = scene_depth->PixelFormat();
        description.is_depth_target = true;
        description.shader_visible_depth = true;
        description.sample_count = 1u;
        auto texture = CreateRhiTexture(*device, description);
        if (texture.IsErr() ||
            !IsDepthTextureCopyCompatible(
                *scene_depth, *texture.Value())) {
            host.water3d_depth_copy_failed = true;
            ACS_LOG_WARN(
                "[3D] interactive-water depth snapshot unavailable; "
                "opaque PBR fallback remains active%s%s",
                texture.IsErr() ? ": " : "",
                texture.IsErr() ? texture.Error().message :
                                  " (incompatible depth allocation)");
            return true;
        }
        depth_candidate = Move(texture.Value());
    }

    if (background_candidate) {
        host.refr_bg = Move(background_candidate);
        host.refr_bg_w = host.width;
        host.refr_bg_h = host.height;
    }
    if (depth_candidate) {
        host.water3d_depth_copy = Move(depth_candidate);
        host.water3d_depth_copy_w = host.width;
        host.water3d_depth_copy_h = host.height;
    }
    host.water3d_background_failed = false;
    host.water3d_depth_copy_failed = false;
    return true;
}

/**
 * Draw specialized water before all scene-space atmosphere composites.
 *
 * The color background and opaque D32 depth are immutable snapshots. The live
 * scene depth is rebound as a writable DSV, so displaced water participates in
 * opaque/water occlusion and becomes visible to later AP/cloud/fog/DoF/TAA.
 */
void DrawInteractiveWater3DPass(
    FEditorHost& host,
    IRhiCommandList& command_list,
    IRhiTexture& hdr_target,
    const TArray<game::ANode*>& nodes,
    editor_frustum_culling::FSubmissionMaskView submission_mask,
    const FMat4& view_projection,
    FVec3 camera_position,
    FVec3 sun_color,
    bool shadow_enabled,
    IRhiTexture* shadow_map,
    const FMat4& light_view_projection,
    f32 shadow_bias,
    f32 shadow_filter,
    u32 width,
    u32 height) noexcept {
    bool any_water = false;
    for (u32 i = 0u; i < nodes.Num(); ++i) {
        if (!submission_mask.ShouldSubmit(i)) continue;
        if (IsRenderedByWater3D(host, nodes[i])) {
            any_water = true;
            break;
        }
    }
    if (!any_water || !host.refr_bg ||
        !host.water3d_depth_copy ||
        !host.blit_pipe) {
        return;
    }

    IRhiTexture* scene_depth = host.renderer.DepthBuffer();
    if (!scene_depth ||
        !IsDepthTextureCopyCompatible(
            *scene_depth, *host.water3d_depth_copy)) {
        return;
    }

    constexpr FClearColor copy_clear{
        0.0f, 0.0f, 0.0f, 1.0f};
    command_list.BeginRenderToTexture(
        *host.refr_bg, copy_clear, nullptr, 1.0f);
    FViewport viewport{};
    viewport.width = static_cast<f32>(width);
    viewport.height = static_cast<f32>(height);
    command_list.SetViewport(viewport);
    FScissorRect scissor{};
    scissor.right = static_cast<i32>(width);
    scissor.bottom = static_cast<i32>(height);
    command_list.SetScissor(scissor);
    command_list.SetPipeline(*host.blit_pipe);
    command_list.SetTexture(0u, hdr_target);
    command_list.Draw(3u, 0u);
    command_list.EndRenderToTexture(*host.refr_bg);

    const bool copied_depth = command_list.CopyDepthTexture(
        *scene_depth, *host.water3d_depth_copy);
    command_list.BeginRenderToTextureLoad(
        hdr_target, scene_depth);
    command_list.SetViewport(viewport);
    command_list.SetScissor(scissor);

    if (!copied_depth) {
        // The specialized nodes were intentionally omitted from the earlier
        // opaque loop. Draw a conservative PBR surface now so a backend copy
        // failure can never make authored water disappear for one frame.
        host.pbr3d.SetNormalMap(nullptr, 1.0f);
        host.pbr3d.ClearSubstrateSurface();
        host.pbr3d.SetExtParams(0.0f, 0.0f, 0.0f);
        host.pbr3d.SetSheen(FVec3::Zero(), 0.0f, 0.3f);
        host.pbr3d.SetSubsurface(FVec3::Zero(), 0.0f);
        host.pbr3d.SetEmissive(FVec3::Zero(), 0.0f);
        for (u32 i = 0u; i < nodes.Num(); ++i) {
            if (!submission_mask.ShouldSubmit(i)) continue;
            game::ANode* node = nodes[i];
            if (!IsRenderedByWater3D(host, node)) continue;
            AEditor3DRecordComponent* record = Rec3D(node);
            FGpuMesh* mesh = WaterGpuMeshForNode3D(host, node);
            if (!record || !mesh) continue;
            const FWaterSurface3DParams params =
                WaterSurface3DParamsFor(record);
            const FVec3 fallback_color{
                params.shallow_color.x * 0.72f +
                    params.deep_color.x * 0.28f,
                params.shallow_color.y * 0.72f +
                    params.deep_color.y * 0.28f,
                params.shallow_color.z * 0.72f +
                    params.deep_color.z * 0.28f,
            };
            host.pbr3d.DrawMesh(
                command_list, *mesh, node->World().ToMat4(),
                fallback_color, 0.0f, params.roughness, 1.0f,
                nullptr);
        }
        command_list.EndRenderToTexture(hdr_target);
        host.water3d_depth_copy_failed = true;
        ACS_LOG_WARN(
            "[3D] interactive-water depth copy failed; "
            "opaque PBR fallback remains active");
        return;
    }

    host.water3d.SetEnvironment(
        host.sky_zenith, host.sky_horizon, host.sky_ground);
    host.water3d.SetFrame(
        view_projection, camera_position,
        width, height, host.sun_dir, sun_color);
    host.water3d.SetShadowMap(
        shadow_enabled ? shadow_map : nullptr,
        light_view_projection, shadow_bias, shadow_filter);
    IRhiTexture* reflection =
        host.ssr_computed
            ? host.ssr3d.OutputTexture() : nullptr;
    for (u32 i = 0u; i < nodes.Num(); ++i) {
        if (!submission_mask.ShouldSubmit(i)) continue;
        game::ANode* node = nodes[i];
        if (!IsRenderedByWater3D(host, node)) continue;
        AEditor3DRecordComponent* record = Rec3D(node);
        FGpuMesh* mesh = WaterGpuMeshForNode3D(host, node);
        if (!record || !mesh) continue;
        IRhiTexture* authored_normal_map = nullptr;
        f32 authored_normal_strength = 1.0f;
        game::AMeshComponent3D* mesh_component = Mesh3D(node);
        if (mesh_component != nullptr &&
            mesh_component->Material().kind ==
                game::EMaterialKind::Lit) {
            LoadNode3DMaterialTextures(host, node);
            authored_normal_map =
                record->material_normal_tex.Get();
            authored_normal_strength =
                mesh_component->Material().pbr.normalStrength;
        }
        host.water3d.SetParams(
            WaterSurface3DParamsFor(record));
        host.water3d.DrawMesh(
            command_list, *mesh, node->World().ToMat4(),
            host.refr_bg.Get(), host.water3d_depth_copy.Get(),
            reflection,
            static_cast<u64>(
                static_cast<u32>(record->id)),
            true, authored_normal_map,
            authored_normal_strength);
    }
    command_list.EndRenderToTexture(hdr_target);
}

/** ノードの «親の editor 整数 id» を返す (親が root or 無しは -1)。 */
int ParentId3D(FEditorHost& h, game::ANode* n) noexcept {
    if (n == nullptr) return -1;
    game::ANode* p = n->Parent();
    if (p == nullptr || p == &h.scene3d.Root()) return -1;
    AEditor3DRecordComponent* r = Rec3D(p);
    return (r != nullptr) ? r->id : -1;
}

/** メッシュファイル (.gltf/.glb/.obj/.fbx、UTF-8 path) を読み込んで AMeshAsset を返す (失敗 null)。 */
TSharedPtr<AAsset> LoadMeshFile(const char* path) noexcept {
    if (path == nullptr || path[0] == '\0') return nullptr;
    wchar_t wpath[512];
    if (MultiByteToWideChar(kCpUtf8, 0, path, -1, wpath, 512) <= 0) return nullptr;
    auto bytes = CFileSystem::ReadAllBytes(wpath);
    if (bytes.IsErr() || bytes.Value().Num() == 0) { ACS_LOG_ERROR("[3D] メッシュ open 失敗: %s", path); return nullptr; }
    // 拡張子で loader を選ぶ。
    const char* ext = std::strrchr(path, '.');
    TResult<TSharedPtr<AAsset>> r = ACS_ERR(Asset, 900, "no loader");
    if (ext != nullptr) {
        if      (_stricmp(ext, ".glb")  == 0) { CGlbAssetLoader  l; r = l.LoadFromBytes(kInvalidAssetId, bytes.Value()); }
        else if (_stricmp(ext, ".gltf") == 0) { CGltfAssetLoader l; r = l.LoadFromBytes(kInvalidAssetId, bytes.Value()); }
        else if (_stricmp(ext, ".obj")  == 0) { CObjAssetLoader  l; r = l.LoadFromBytes(kInvalidAssetId, bytes.Value()); }
        else if (_stricmp(ext, ".fbx")  == 0) { CFbxAssetLoader  l; r = l.LoadFromBytes(kInvalidAssetId, bytes.Value()); }
    }
    if (r.IsErr()) { ACS_LOG_ERROR("[3D] メッシュ parse 失敗: %s", path); return nullptr; }
    TSharedPtr<AAsset> a = r.Value();
    const AMeshAsset* m = static_cast<const AMeshAsset*>(a.Get());
    if (m == nullptr || m->Vertices().Num() == 0) { ACS_LOG_ERROR("[3D] メッシュ空: %s", path); return nullptr; }
    return a;
}

/** CPU メッシュ cm の三角形を model 行列でワールド変換し色を付けて dv へ追加する。 */
void AppendMeshTris(TArray<FM3DVtx>& dv, const AMeshAsset* cm, const FMat4& model, FVec3 col, u32 cap,
                    f32 metallic = 0.0f, f32 roughness = 0.6f) noexcept {
    if (cm == nullptr) return;
    const auto& vtx = cm->Vertices();
    const auto& idx = cm->Indices();
    for (u32 k = 0; k + 2 < idx.Num(); k += 3) {
        for (u32 t = 0; t < 3; ++t) {
            const FMeshVertex& mv = vtx[idx[k + t]];
            const FVec4 wp = Transform(FVec4{ mv.position.x, mv.position.y, mv.position.z, 1.0f }, model);
            const FVec4 wn = Transform(FVec4{ mv.normal.x, mv.normal.y, mv.normal.z, 0.0f }, model);
            FM3DVtx o; o.px = wp.x; o.py = wp.y; o.pz = wp.z;
            o.nx = wn.x; o.ny = wn.y; o.nz = wn.z; o.r = col.x; o.g = col.y; o.b = col.z;
            o.mt = metallic; o.rg = roughness;
            if (dv.Num() < cap) dv.Add(o);
        }
    }
}

/** ギズモのワールド長 (カメラ距離に比例 → 画面上ほぼ一定。視線と軸の角度で破綻しない)。 */
f32 Gizmo3DScale(const FEditorHost& h, FVec3 pos) noexcept {
    const FVec3 eye = Cam3DEye(h);
    const FVec3 d{ eye.x - pos.x, eye.y - pos.y, eye.z - pos.z };
    const f32 dist = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
    return (dist > 0.1f ? dist : 0.1f) * 0.16f;
}

/** 軸方向に伸びる «箱バー» を gv へ追加する (world X/Y/Z 軸のみ、非一様スケールで回転不要)。 */
void AppendBar(TArray<FM3DVtx>& gv, const FEditorHost& h, FVec3 center, int axis, f32 len, f32 th, FVec3 col) noexcept {
    FVec3 s{ th, th, th }; (axis == 1) ? (s.x = len) : (axis == 2) ? (s.y = len) : (s.z = len);
    const FMat4 m = FMat4::Scale(s) * FMat4::Translation(center);
    AppendMeshTris(gv, h.cpu_cube.Get(), m, col, 4096);
}

/** 軸方向の «円錐の矢じり» を gv へ追加する (apex = base + axisDir*len)。 */
static FVec3 GNorm(FVec3 v) noexcept { const f32 l = std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z); return (l>1e-6f)?FVec3{v.x/l,v.y/l,v.z/l}:FVec3{0,1,0}; }
void AppendCone(TArray<FM3DVtx>& gv, FVec3 base, int axis, f32 len, f32 rad, FVec3 col) noexcept {
    const FVec3 ax = AxisDir(axis);
    FVec3 u = (axis == 2) ? FVec3{ 1, 0, 0 } : FVec3{ 0, 1, 0 };
    FVec3 v{ ax.y*u.z - ax.z*u.y, ax.z*u.x - ax.x*u.z, ax.x*u.y - ax.y*u.x };
    const FVec3 apex{ base.x + ax.x*len, base.y + ax.y*len, base.z + ax.z*len };
    const int N = 24;   // 滑らかな矢じり
    auto pushV = [&](FVec3 p, FVec3 n) { if (gv.Num() < 4096) { FM3DVtx o; o.px=p.x;o.py=p.y;o.pz=p.z;o.nx=n.x;o.ny=n.y;o.nz=n.z;o.r=col.x;o.g=col.y;o.b=col.z;o.mt=0;o.rg=0.35f; gv.Add(o);} };
    for (int i = 0; i < N; ++i) {
        const f32 a0 = 6.2831853f * i / N, a1 = 6.2831853f * (i + 1) / N;
        const FVec3 rd0{ u.x*std::cos(a0)+v.x*std::sin(a0), u.y*std::cos(a0)+v.y*std::sin(a0), u.z*std::cos(a0)+v.z*std::sin(a0) };
        const FVec3 rd1{ u.x*std::cos(a1)+v.x*std::sin(a1), u.y*std::cos(a1)+v.y*std::sin(a1), u.z*std::cos(a1)+v.z*std::sin(a1) };
        const FVec3 p0{ base.x + rd0.x*rad, base.y + rd0.y*rad, base.z + rd0.z*rad };
        const FVec3 p1{ base.x + rd1.x*rad, base.y + rd1.y*rad, base.z + rd1.z*rad };
        // 側面の «円錐外向き» 法線 = radial*len + axis*rad (slant に直交、滑らか)。
        const FVec3 n0 = GNorm(FVec3{ rd0.x*len+ax.x*rad, rd0.y*len+ax.y*rad, rd0.z*len+ax.z*rad });
        const FVec3 n1 = GNorm(FVec3{ rd1.x*len+ax.x*rad, rd1.y*len+ax.y*rad, rd1.z*len+ax.z*rad });
        pushV(apex, GNorm(FVec3{n0.x+n1.x,n0.y+n1.y,n0.z+n1.z})); pushV(p0, n0); pushV(p1, n1);   // 側面
        pushV(base, FVec3{-ax.x,-ax.y,-ax.z}); pushV(p1, FVec3{-ax.x,-ax.y,-ax.z}); pushV(p0, FVec3{-ax.x,-ax.y,-ax.z}); // 底
    }
}
/** 軸方向の «円柱» (シャフト)。半径 rad、長さ len、滑らかな外向き法線。 */
void AppendCylinder(TArray<FM3DVtx>& gv, FVec3 base, int axis, f32 len, f32 rad, FVec3 col) noexcept {
    const FVec3 ax = AxisDir(axis);
    FVec3 u = (axis == 2) ? FVec3{ 1, 0, 0 } : FVec3{ 0, 1, 0 };
    FVec3 v{ ax.y*u.z - ax.z*u.y, ax.z*u.x - ax.x*u.z, ax.x*u.y - ax.y*u.x };
    const FVec3 tip{ base.x + ax.x*len, base.y + ax.y*len, base.z + ax.z*len };
    const int N = 20;
    auto pushV = [&](FVec3 p, FVec3 n) { if (gv.Num() < 4096) { FM3DVtx o; o.px=p.x;o.py=p.y;o.pz=p.z;o.nx=n.x;o.ny=n.y;o.nz=n.z;o.r=col.x;o.g=col.y;o.b=col.z;o.mt=0;o.rg=0.35f; gv.Add(o);} };
    for (int i = 0; i < N; ++i) {
        const f32 a0 = 6.2831853f * i / N, a1 = 6.2831853f * (i + 1) / N;
        const FVec3 n0{ u.x*std::cos(a0)+v.x*std::sin(a0), u.y*std::cos(a0)+v.y*std::sin(a0), u.z*std::cos(a0)+v.z*std::sin(a0) };
        const FVec3 n1{ u.x*std::cos(a1)+v.x*std::sin(a1), u.y*std::cos(a1)+v.y*std::sin(a1), u.z*std::cos(a1)+v.z*std::sin(a1) };
        const FVec3 b0{ base.x+n0.x*rad, base.y+n0.y*rad, base.z+n0.z*rad }, b1{ base.x+n1.x*rad, base.y+n1.y*rad, base.z+n1.z*rad };
        const FVec3 t0{ tip.x+n0.x*rad,  tip.y+n0.y*rad,  tip.z+n0.z*rad  }, t1{ tip.x+n1.x*rad,  tip.y+n1.y*rad,  tip.z+n1.z*rad  };
        pushV(b0,n0); pushV(t0,n0); pushV(t1,n1);   // 側面 (smooth)
        pushV(b0,n0); pushV(t1,n1); pushV(b1,n1);
    }
}
/** 軸まわりの «リング» (トーラス)。回転ギズモ用。リング半径 radius、チューブ半径 tube。 */
void AppendRing(TArray<FM3DVtx>& gv, FVec3 center, int axis, f32 radius, f32 tube, FVec3 col) noexcept {
    const FVec3 ax = AxisDir(axis);
    FVec3 u = (axis == 2) ? FVec3{ 1, 0, 0 } : FVec3{ 0, 1, 0 };
    FVec3 v{ ax.y*u.z - ax.z*u.y, ax.z*u.x - ax.x*u.z, ax.x*u.y - ax.y*u.x };   // u,v: リング平面
    const int N = 24, M = 5;
    auto pushV = [&](FVec3 p, FVec3 n) { if (gv.Num() < 4096) { FM3DVtx o; o.px=p.x;o.py=p.y;o.pz=p.z;o.nx=n.x;o.ny=n.y;o.nz=n.z;o.r=col.x;o.g=col.y;o.b=col.z;o.mt=0;o.rg=0.35f; gv.Add(o);} };
    auto ringDir = [&](f32 a){ return FVec3{ u.x*std::cos(a)+v.x*std::sin(a), u.y*std::cos(a)+v.y*std::sin(a), u.z*std::cos(a)+v.z*std::sin(a) }; };
    auto tubeN  = [&](FVec3 rd, f32 b){ return FVec3{ rd.x*std::cos(b)+ax.x*std::sin(b), rd.y*std::cos(b)+ax.y*std::sin(b), rd.z*std::cos(b)+ax.z*std::sin(b) }; };
    for (int i = 0; i < N; ++i) {
        const f32 a0 = 6.2831853f*i/N, a1 = 6.2831853f*(i+1)/N;
        const FVec3 rd0 = ringDir(a0), rd1 = ringDir(a1);
        const FVec3 c0{ center.x+rd0.x*radius, center.y+rd0.y*radius, center.z+rd0.z*radius };
        const FVec3 c1{ center.x+rd1.x*radius, center.y+rd1.y*radius, center.z+rd1.z*radius };
        for (int j = 0; j < M; ++j) {
            const f32 b0 = 6.2831853f*j/M, b1 = 6.2831853f*(j+1)/M;
            const FVec3 n00=tubeN(rd0,b0), n01=tubeN(rd0,b1), n10=tubeN(rd1,b0), n11=tubeN(rd1,b1);
            const FVec3 p00{c0.x+n00.x*tube,c0.y+n00.y*tube,c0.z+n00.z*tube}, p01{c0.x+n01.x*tube,c0.y+n01.y*tube,c0.z+n01.z*tube};
            const FVec3 p10{c1.x+n10.x*tube,c1.y+n10.y*tube,c1.z+n10.z*tube}, p11{c1.x+n11.x*tube,c1.y+n11.y*tube,c1.z+n11.z*tube};
            pushV(p00,n00); pushV(p10,n10); pushV(p11,n11);
            pushV(p00,n00); pushV(p11,n11); pushV(p01,n01);
        }
    }
}

/** 平面ハンドルの «小さな四角» を gv へ追加する (中心 c、面内基底 e1,e2、半サイズ hs)。 */
void AppendQuad(TArray<FM3DVtx>& gv, FVec3 c, FVec3 e1, FVec3 e2, f32 hs, FVec3 col) noexcept {
    const FVec3 n{ e1.y*e2.z - e1.z*e2.y, e1.z*e2.x - e1.x*e2.z, e1.x*e2.y - e1.y*e2.x };
    FVec3 p00{ c.x-(e1.x+e2.x)*hs, c.y-(e1.y+e2.y)*hs, c.z-(e1.z+e2.z)*hs };
    FVec3 p10{ c.x+(e1.x-e2.x)*hs, c.y+(e1.y-e2.y)*hs, c.z+(e1.z-e2.z)*hs };
    FVec3 p11{ c.x+(e1.x+e2.x)*hs, c.y+(e1.y+e2.y)*hs, c.z+(e1.z+e2.z)*hs };
    FVec3 p01{ c.x-(e1.x-e2.x)*hs, c.y-(e1.y-e2.y)*hs, c.z-(e1.z-e2.z)*hs };
    auto pv = [&](FVec3 p){ if (gv.Num()<4096){ FM3DVtx o;o.px=p.x;o.py=p.y;o.pz=p.z;o.nx=n.x;o.ny=n.y;o.nz=n.z;o.r=col.x;o.g=col.y;o.b=col.z;o.mt=0;o.rg=0.35f;gv.Add(o);} };
    pv(p00);pv(p10);pv(p11); pv(p00);pv(p11);pv(p01);
}

/** 平面ハンドル番号 (4=XY,5=YZ,6=XZ) → 面内 2 軸 + 法線。 */
void PlaneAxes(int handle, FVec3& e1, FVec3& e2, FVec3& n) noexcept {
    if (handle == 4)      { e1 = FVec3{1,0,0}; e2 = FVec3{0,1,0}; n = FVec3{0,0,1}; }  // XY
    else if (handle == 5) { e1 = FVec3{0,1,0}; e2 = FVec3{0,0,1}; n = FVec3{1,0,0}; }  // YZ
    else                  { e1 = FVec3{1,0,0}; e2 = FVec3{0,0,1}; n = FVec3{0,1,0}; }  // XZ
}

/**
 * Draw the selected authored-camera frustum into the currently bound target.
 *
 * The caller keeps this editor-only visualization out of Game View.  The HDR
 * path invokes it on the loaded swapchain after post processing, so TAA,
 * bloom, clouds, motion blur, and depth of field cannot attenuate the guide.
 * The direct-to-swapchain compatibility path invokes the same helper without
 * introducing a second color clear.
 */
void DrawSelectedCameraFrustumOverlay(
    FEditorHost& host, IRhiCommandList& command_list,
    const FMat4& view_projection, f32 aspect) noexcept {
    if (!host.show_camera_frustum || host.game_view)
        return;
    game::ANode* node =
        FindNode3DNode(host, host.sel3d);
    AEditor3DRecordComponent* record = Rec3D(node);
    if (node == nullptr || record == nullptr ||
        !record->has_scene_camera) {
        return;
    }
    FResolvedSceneCamera3D resolved{};
    resolved.node = node;
    resolved.record = record;
    resolved.world = node->World();
    FRenderCamera3D camera;
    if (!BuildResolvedRenderCamera3D(
            resolved, aspect, camera)) {
        return;
    }

    FVec3 right;
    if (!NormalizeRenderCameraVector(
            Cross(camera.up, camera.forward), right)) {
        return;
    }
    const f32 near_distance =
        std::max(0.001f, camera.near_plane);
    const f32 far_distance = std::min(
        camera.far_plane,
        std::max(near_distance + 0.5f, 40.0f));
    if (!std::isfinite(far_distance) ||
        far_distance <= near_distance) {
        return;
    }
    f32 near_half_height = camera.orthographic
        ? camera.orthographic_height * 0.5f
        : std::tan(
              camera.fov_y_degrees * 3.14159265f /
              360.0f) * near_distance;
    f32 far_half_height = camera.orthographic
        ? near_half_height
        : std::tan(
              camera.fov_y_degrees * 3.14159265f /
              360.0f) * far_distance;
    const f32 near_half_width =
        near_half_height * aspect;
    const f32 far_half_width =
        far_half_height * aspect;
    const FVec3 near_center =
        camera.eye + camera.forward * near_distance;
    const FVec3 far_center =
        camera.eye + camera.forward * far_distance;
    auto corner = [&](FVec3 center, f32 half_width,
                      f32 half_height, f32 x,
                      f32 y) noexcept {
        return center + right * (half_width * x) +
               camera.up * (half_height * y);
    };
    FVec3 corners[8] = {
        corner(near_center, near_half_width, near_half_height, -1.0f, -1.0f),
        corner(near_center, near_half_width, near_half_height,  1.0f, -1.0f),
        corner(near_center, near_half_width, near_half_height,  1.0f,  1.0f),
        corner(near_center, near_half_width, near_half_height, -1.0f,  1.0f),
        corner(far_center, far_half_width, far_half_height, -1.0f, -1.0f),
        corner(far_center, far_half_width, far_half_height,  1.0f, -1.0f),
        corner(far_center, far_half_width, far_half_height,  1.0f,  1.0f),
        corner(far_center, far_half_width, far_half_height, -1.0f,  1.0f),
    };
    constexpr int edges[12][2] = {
        {0,1}, {1,2}, {2,3}, {3,0},
        {4,5}, {5,6}, {6,7}, {7,4},
        {0,4}, {1,5}, {2,6}, {3,7},
    };
    const FVec4 near_color{1.0f, 0.72f, 0.18f, 0.62f};
    const FVec4 far_color{0.20f, 0.72f, 0.92f, 0.48f};
    host.camera_frustum_dbg3d.Begin();
    for (u32 edge = 0u; edge < 12u; ++edge) {
        host.camera_frustum_dbg3d.Line(
            corners[edges[edge][0]],
            corners[edges[edge][1]],
            edge < 4u ? near_color : far_color);
    }
    host.camera_frustum_dbg3d.Line(
        camera.eye, near_center,
        FVec4{0.30f, 0.82f, 1.0f, 0.55f});
    host.camera_frustum_dbg3d.End(
        command_list, view_projection);
}

void DrawGizmo3DOverlay(FEditorHost& h, IRhiCommandList& cl,
                        const FMat4& view_proj, FVec3 camera_position) noexcept {
    game::ANode* sn = FindNode3DNode(h, h.sel3d);
    if (sn == nullptr) return;

    const FVec3 P = sn->Local().position;
    const f32 gl = Gizmo3DScale(h, P);
    const f32 shaft = gl * 0.020f;
    const f32 head  = gl * 0.085f;
    const f32 headL = gl * 0.26f;
    const f32 axisL = gl * 0.74f;
    const f32 rootO = gl * 0.13f;
    const FVec3 cols[3] = {
        FVec3{ 0.92f, 0.26f, 0.30f },
        FVec3{ 0.40f, 0.86f, 0.34f },
        FVec3{ 0.30f, 0.55f, 0.96f }
    };
    const FVec3 hot{ 1.0f, 0.86f, 0.22f };
    TArray<FM3DVtx>& gv = h.gizmo_vertices;
    gv.Reset();
    if (gv.Max() < 4096u) gv.Reserve(4096u);

    for (int a = 1; a <= 3; ++a) {
        const FVec3 d = AxisDir(a);
        const FVec3 col = (h.giz3d_handle == a) ? hot : cols[a - 1];
        const FVec3 root{
            P.x + d.x * rootO, P.y + d.y * rootO, P.z + d.z * rootO
        };
        const FVec3 end{
            P.x + d.x * axisL, P.y + d.y * axisL, P.z + d.z * axisL
        };
        if (h.gizmo_mode == 1) {
            AppendRing(gv, P, a, axisL, shaft * 1.3f, col);
        } else if (h.gizmo_mode == 2) {
            AppendCylinder(gv, root, a, axisL - rootO, shaft, col);
            AppendMeshTris(
                gv, h.cpu_cube.Get(),
                FMat4::Scale(FVec3{ head*1.6f, head*1.6f, head*1.6f }) *
                    FMat4::Translation(end),
                col, 4096);
        } else {
            AppendCylinder(gv, root, a, axisL - rootO, shaft, col);
            AppendCone(gv, end, a, headL, head, col);
        }
    }

    const f32 po = gl * 0.34f;
    const f32 ph = gl * 0.11f;
    if (h.gizmo_mode != 1) {
        for (int handle = 4; handle <= 6; ++handle) {
            FVec3 e1, e2, normal;
            PlaneAxes(handle, e1, e2, normal);
            const FVec3 center{
                P.x + (e1.x + e2.x) * po,
                P.y + (e1.y + e2.y) * po,
                P.z + (e1.z + e2.z) * po
            };
            FVec3 color{};
            if (handle == 4) {
                color = h.giz3d_handle == 4
                    ? hot : FVec3{0.30f,0.55f,0.96f};
            } else if (handle == 5) {
                color = h.giz3d_handle == 5
                    ? hot : FVec3{0.92f,0.26f,0.30f};
            } else {
                color = h.giz3d_handle == 6
                    ? hot : FVec3{0.40f,0.86f,0.34f};
            }
            AppendQuad(gv, center, e1, e2, ph, color);
        }
    }

    AppendMeshTris(
        gv, h.cpu_sphere.Get(),
        FMat4::Scale(FVec3{gl*0.06f,gl*0.06f,gl*0.06f}) *
            FMat4::Translation(P),
        h.giz3d_handle == 0
            ? hot : FVec3{ 0.88f, 0.88f, 0.92f },
        4096);

    if (gv.Num() == 0 || !h.m3d_overlay_pipe ||
        !h.m3d_giz_vb || !h.m3d_giz_cb) {
        return;
    }

    FM3DFrame cb{};
    cb.view_proj = view_proj;
    cb.light_dir = FVec4{ 0.35f, 0.72f, 0.55f, 0.45f };
    cb.light_col = FVec4{ 1.85f, 1.85f, 1.95f, 0.0f };
    cb.cam_pos = FVec4{
        camera_position.x, camera_position.y, camera_position.z, 0.0f
    };
    h.m3d_giz_cb->Update(&cb, sizeof(cb));
    h.m3d_giz_vb->Update(gv.GetData(), sizeof(FM3DVtx) * gv.Num());
    cl.SetPipeline(*h.m3d_overlay_pipe);
    cl.SetConstantBuffer(0, *h.m3d_giz_cb);
    if (h.shadow.DepthTexture() != nullptr) {
        cl.SetTexture(0, *h.shadow.DepthTexture());
    }
    cl.SetVertexBuffer(*h.m3d_giz_vb, sizeof(FM3DVtx));
    cl.Draw(static_cast<u32>(gv.Num()), 0);
}

// ===== VXGI: ボクセルを使う広域照明 =====================================
// 三角形 (M3DVtx の StructuredBuffer) を 64^3 radiance volume に voxelize (compute)。
// 各 voxel = albedo * 直射太陽 (影なし、色のにじみ proof には十分)。PBR が volume を cone trace。
constexpr u32 kVxgiRes = 64;

const char* kVxgiClearCS = R"(
RWTexture3D<float4> vol : register(u0);
[numthreads(4,4,4)]
void CSMain(uint3 id : SV_DispatchThreadID){ if(id.x>=64u||id.y>=64u||id.z>=64u) return; vol[id]=float4(0,0,0,0); }
)";

const char* kVxgiVoxCS = R"(
struct Vtx { float px,py,pz, nx,ny,nz, r,g,b, mt,rg; };
StructuredBuffer<Vtx> verts : register(t0);
RWTexture3D<float4> vol : register(u0);
cbuffer VoxCB : register(b0) {
    float4 gridMin;   // xyz, w=triCount
    float4 gridExt;   // xyz=extent, w=res
    float4 sunDir;    // xyz
    float4 sunCol;    // rgb
};
[numthreads(64,1,1)]
void CSMain(uint3 tid : SV_DispatchThreadID){
    uint tri=tid.x; if(tri>=(uint)gridMin.w) return;
    Vtx a=verts[tri*3+0], b=verts[tri*3+1], c=verts[tri*3+2];
    float3 N=normalize(float3(a.nx,a.ny,a.nz)+float3(b.nx,b.ny,b.nz)+float3(c.nx,c.ny,c.nz));
    float ndl=max(dot(N, normalize(sunDir.xyz)), 0.0);
    float3 lit=float3(a.r,a.g,a.b) * (sunCol.rgb*(ndl+0.18));   // 直接光 (影なし) + floor
    float res=gridExt.w;
    [unroll] for(int u=0;u<=4;u++){ [unroll] for(int w=0;w+u<=4;w++){
        float bu=u/4.0, bw=w/4.0, bv=1.0-bu-bw;
        float3 p=float3(a.px,a.py,a.pz)*bv + float3(b.px,b.py,b.pz)*bu + float3(c.px,c.py,c.pz)*bw;
        float3 uvw=(p-gridMin.xyz)/max(gridExt.xyz,1e-4);
        if(any(uvw<0.0)||any(uvw>1.0)) continue;
        vol[int3(uvw*res)]=float4(lit,1.0);   // last-write-wins (粗い GI)
    }}
}
)";

// VXGI resolve: 画面ごとに world pos+normal を復元し radiance volume を hemisphere cone trace →
// 間接光 (色のにじみ) を half-res 2D へ。SetSsgi で ambient に加算 (PBR 無改変、既存スロット流用)。
const char* kVxgiResolveCS = R"(
#pragma pack_matrix(row_major)
Texture3D<float4> vol : register(t0);
SamplerState vol_sampler : register(s0);
Texture2D<float> depthTex : register(t1);
SamplerState depthTex_sampler : register(s1);
Texture2D<float4> normalTex : register(t2);
SamplerState normalTex_sampler : register(s2);
RWTexture2D<float4> outI : register(u0);
cbuffer ResCB : register(b0) {
    float4x4 invVP;
    float4 gridMin;   // xyz
    float4 gridExt;   // xyz, w=res
    float4 dims;      // xy=resolve res, z=intensity
};
[numthreads(8,8,1)]
void CSMain(uint3 tid : SV_DispatchThreadID){
    if(tid.x>=(uint)dims.x || tid.y>=(uint)dims.y) return;
    float2 uv=(float2(tid.xy)+0.5)/dims.xy;
    float d=depthTex.SampleLevel(depthTex_sampler, uv, 0);
    if(d>=0.9999){ outI[tid.xy]=float4(0,0,0,0); return; }
    float4 clip=float4(uv.x*2.0-1.0, -(uv.y*2.0-1.0), d, 1.0);
    float4 wp=mul(clip, invVP); float3 P=wp.xyz/wp.w;
    float3 N=normalize(normalTex.SampleLevel(normalTex_sampler, uv, 0).xyz);
    float3 up=abs(N.y)<0.9?float3(0,1,0):float3(1,0,0);
    float3 T=normalize(cross(up,N)), Bt=cross(N,T);
    float voxel=gridExt.x/max(gridExt.w,1.0);
    float3 dirs[5]={ N, normalize(N*0.6+T*0.8), normalize(N*0.6-T*0.8),
                     normalize(N*0.6+Bt*0.8), normalize(N*0.6-Bt*0.8) };
    float3 indirect=float3(0,0,0); float wsum=0.0;
    [unroll] for(int c=0;c<5;c++){
        float3 dir=dirs[c]; float3 sp=P+N*voxel*1.5; float transp=1.0; float3 acc=float3(0,0,0);
        [unroll] for(int s=0;s<8;s++){
            sp += dir*voxel*1.6;
            float3 uvw=(sp-gridMin.xyz)/max(gridExt.xyz,1e-4);
            if(any(uvw<0.0)||any(uvw>1.0)) break;
            float4 v=vol.SampleLevel(vol_sampler, uvw, 0);
            acc += transp*v.rgb*v.a; transp *= (1.0-v.a);
            if(transp<0.05) break;
        }
        float cw=max(dot(dir,N),0.0); indirect += acc*cw; wsum += cw;
    }
    indirect /= max(wsum,1e-3);
    outI[tid.xy]=float4(indirect*dims.z, 1.0);           // 64³ ボクセルの blocky な過剰にじみを避け subtle に
}
)";

// 三角形を SB へ詰めて clear→voxelize。volume を返す (PBR の SetVxgi へ)。失敗時 nullptr。
static_assert(
    !editor_frustum_culling::SceneGeometryPassPolicy(
        editor_frustum_culling::ESceneGeometryPass::VxgiVoxelization)
         .uses_main_view_mask,
    "VXGI is world-space and must not use the active camera visibility mask");
static_assert(
    editor_frustum_culling::SceneGeometryPassPolicy(
        editor_frustum_culling::ESceneGeometryPass::VxgiVoxelization)
        .command_form ==
        editor_frustum_culling::ESubmissionCommandForm::Dispatch);
IRhiTexture* VxgiVoxelize(
    FEditorHost& h, IRhiCommandList* cl,
    const TArray<FM3DVtx>& dv,
    u64 geometry_revision,
    FVec3 bbMin, FVec3 bbMax) noexcept {
    IRhiDevice* dev = h.renderer.Device();
    if (dev == nullptr || cl == nullptr || dv.Num() < 3) return nullptr;
    // raw DX12 compute は texture SRV/UAV workload（clouds/AP）には対応するが、
    // StructuredBuffer SRV binding には未対応。raw buffer descriptor path が完全に
    // 同等になるまでは、実験的 VXGI に既存の graceful fallback を使う。
    const char* backend_name = dev->BackendName();
    if (backend_name != nullptr && std::strcmp(backend_name, "DX12") == 0) {
        if (!h.vxgi_tried) {
            h.vxgi_tried = true;
            ACS_LOG_WARN("[VXGI] raw DX12 has no StructuredBuffer SRV support; using screen-space SSGI fallback");
        }
        return nullptr;
    }
    if (!h.vxgi_tried) {
        h.vxgi_tried = true;
        // volume 64^3 RGBA16F (UAV+SRV)
        FTextureDesc vd{}; vd.width=kVxgiRes; vd.height=kVxgiRes; vd.depth=kVxgiRes;
        vd.format=EFormat::R16G16B16A16_Float; vd.is_uav=true;
        if (auto r=CreateRhiTexture(*dev, vd); r.IsOk()) h.vxgi_vol=Move(r.Value());
        // clear + voxelize compute pipelines
        FShaderDesc cs0{}; cs0.stage=EShaderStage::Compute; cs0.hlsl_source=kVxgiClearCS; cs0.entry_point="CSMain"; cs0.debug_name="Vxgi.Clear";
        FShaderDesc cs1{}; cs1.stage=EShaderStage::Compute; cs1.hlsl_source=kVxgiVoxCS;   cs1.entry_point="CSMain"; cs1.debug_name="Vxgi.Vox";
        if (auto r=CreateRhiShader(*dev, cs0); r.IsOk()) h.vxgi_cs_clear=Move(r.Value());
        if (auto r=CreateRhiShader(*dev, cs1); r.IsOk()) h.vxgi_cs_vox=Move(r.Value());
        if (h.vxgi_cs_clear) { FComputePipelineDesc pd{}; pd.cs=h.vxgi_cs_clear.Get(); pd.uav_slots=1; pd.uav_names[0]="vol";
            if (auto r=CreateRhiComputePipeline(*dev, pd); r.IsOk()) h.vxgi_pipe_clear=Move(r.Value()); }
        if (h.vxgi_cs_vox) { FComputePipelineDesc pd{}; pd.cs=h.vxgi_cs_vox.Get(); pd.cbuffer_slots=1; pd.cbuffer_names[0]="VoxCB";
            pd.srv_slots=1; pd.srv_names[0]="verts"; pd.uav_slots=1; pd.uav_names[0]="vol";
            if (auto r=CreateRhiComputePipeline(*dev, pd); r.IsOk()) h.vxgi_pipe_vox=Move(r.Value()); }
        FBufferDesc cbd{}; cbd.size=64; cbd.usage=EBufferUsage::Uniform; cbd.cpu_writable=true;
        if (auto r=CreateRhiBuffer(*dev, cbd); r.IsOk()) h.vxgi_cb_vox=Move(r.Value());
        h.vxgi_ready = (h.vxgi_vol && h.vxgi_pipe_clear && h.vxgi_pipe_vox && h.vxgi_cb_vox);
        ACS_LOG_INFO("[VXGI] setup vol=%d clear=%d vox=%d", (int)(h.vxgi_vol.Get()!=nullptr),
                     (int)(h.vxgi_pipe_clear.Get()!=nullptr), (int)(h.vxgi_pipe_vox.Get()!=nullptr));
    }
    if (!h.vxgi_ready) return nullptr;
    // 三角形 SB を (再)確保 + upload
    const u32 vcount = dv.Num();
    if (!h.vxgi_tri || h.vxgi_tri_cap < vcount) {
        FBufferDesc bd{}; bd.size=sizeof(FM3DVtx)*vcount; bd.usage=EBufferUsage::Storage;
        bd.cpu_writable=true; bd.struct_stride=sizeof(FM3DVtx);
        if (auto r=CreateRhiBuffer(*dev, bd); r.IsOk()) {
            h.vxgi_tri=Move(r.Value());
            h.vxgi_tri_cap=vcount;
            h.vxgi_tri_uploaded_revision = 0u;
        }
        else return nullptr;
    }
    if (h.vxgi_tri_uploaded_revision != geometry_revision) {
        h.vxgi_tri->Update(dv.GetData(), sizeof(FM3DVtx)*vcount);
        h.vxgi_tri_uploaded_revision = geometry_revision;
    }
    // CB
    const FVec3 ext{ bbMax.x-bbMin.x+0.01f, bbMax.y-bbMin.y+0.01f, bbMax.z-bbMin.z+0.01f };
    struct FVoxCb { FVec4 gmin, gext, sdir, scol; } cb{};
    cb.gmin = FVec4{ bbMin.x, bbMin.y, bbMin.z, static_cast<f32>(vcount/3) };
    cb.gext = FVec4{ ext.x, ext.y, ext.z, static_cast<f32>(kVxgiRes) };
    cb.sdir = FVec4{ h.sun_dir.x, h.sun_dir.y, h.sun_dir.z, 0 };
    cb.scol = FVec4{ h.sun_color.x*h.sun_intensity, h.sun_color.y*h.sun_intensity, h.sun_color.z*h.sun_intensity, 0 };
    h.vxgi_cb_vox->Update(&cb, sizeof(cb));
    // clear → voxelize
    cl->SetComputePipeline(*h.vxgi_pipe_clear);
    cl->BindUav(0, *h.vxgi_vol);
    cl->Dispatch(kVxgiRes/4, kVxgiRes/4, kVxgiRes/4);
    cl->SetComputePipeline(*h.vxgi_pipe_vox);
    cl->SetConstantBuffer(0, *h.vxgi_cb_vox);
    cl->BindStructuredSrv(0, *h.vxgi_tri);
    cl->BindUav(0, *h.vxgi_vol);
    cl->Dispatch((vcount/3 + 63)/64, 1, 1);
    return h.vxgi_vol.Get();
}

// volume を画面空間で cone trace → 間接光 (half-res)。SetSsgi 用テクスチャを返す。失敗時 nullptr。
IRhiTexture* VxgiResolve(FEditorHost& h, IRhiCommandList* cl, IRhiTexture* vol,
                         IRhiTexture* depthTex, IRhiTexture* normalTex,
                         const FMat4& invVP, FVec3 bbMin, FVec3 bbMax,
                         u32 scW, u32 scH, f32 intensity) noexcept {
    IRhiDevice* dev = h.renderer.Device();
    if (dev == nullptr || cl == nullptr || vol == nullptr || depthTex == nullptr || normalTex == nullptr) return nullptr;
    const u32 rw = scW / 2 > 0 ? scW / 2 : 1, rh = scH / 2 > 0 ? scH / 2 : 1;
    if (!h.vxgi_pipe_res) {   // 遅延生成 (pipeline + CB)
        FShaderDesc cs{}; cs.stage=EShaderStage::Compute; cs.hlsl_source=kVxgiResolveCS; cs.entry_point="CSMain"; cs.debug_name="Vxgi.Resolve";
        if (auto r=CreateRhiShader(*dev, cs); r.IsOk()) h.vxgi_cs_res=Move(r.Value()); else return nullptr;
        FComputePipelineDesc pd{}; pd.cs=h.vxgi_cs_res.Get();
        pd.cbuffer_slots=1; pd.cbuffer_names[0]="ResCB";
        pd.srv_slots=3; pd.srv_names[0]="vol"; pd.srv_names[1]="depthTex"; pd.srv_names[2]="normalTex";
        pd.uav_slots=1; pd.uav_names[0]="outI";
        pd.static_sampler_count=3;
        for (u32 s=0;s<3;++s){ pd.static_samplers[s].filter=ESamplerFilter::Linear;
            pd.static_samplers[s].address_u=ESamplerAddress::Clamp; pd.static_samplers[s].address_v=ESamplerAddress::Clamp; }
        if (auto r=CreateRhiComputePipeline(*dev, pd); r.IsOk()) h.vxgi_pipe_res=Move(r.Value()); else return nullptr;
        FBufferDesc cbd{}; cbd.size=sizeof(FMat4)+48; cbd.usage=EBufferUsage::Uniform; cbd.cpu_writable=true;
        if (auto r=CreateRhiBuffer(*dev, cbd); r.IsOk()) h.vxgi_cb_res=Move(r.Value()); else return nullptr;
    }
    if (h.vxgi_rw != rw || h.vxgi_rh != rh) {   // half-res 出力 texture (UAV+SRV)
        FTextureDesc td{}; td.width=rw; td.height=rh; td.format=EFormat::R16G16B16A16_Float; td.is_uav=true;
        if (auto r=CreateRhiTexture(*dev, td); r.IsOk()) { h.vxgi_resolve=Move(r.Value()); h.vxgi_rw=rw; h.vxgi_rh=rh; } else return nullptr;
    }
    if (!h.vxgi_pipe_res || !h.vxgi_resolve || !h.vxgi_cb_res) return nullptr;
    const FVec3 ext{ bbMax.x-bbMin.x+0.01f, bbMax.y-bbMin.y+0.01f, bbMax.z-bbMin.z+0.01f };
    struct FResCb { FMat4 invVP; FVec4 gmin, gext, dims; } cb{};
    cb.invVP = invVP;
    cb.gmin  = FVec4{ bbMin.x, bbMin.y, bbMin.z, 0 };
    cb.gext  = FVec4{ ext.x, ext.y, ext.z, static_cast<f32>(kVxgiRes) };
    cb.dims  = FVec4{ static_cast<f32>(rw), static_cast<f32>(rh), intensity, 0 };
    h.vxgi_cb_res->Update(&cb, sizeof(cb));
    cl->SetComputePipeline(*h.vxgi_pipe_res);
    cl->SetConstantBuffer(0, *h.vxgi_cb_res);
    cl->SetTexture(0, *vol); cl->SetTexture(1, *depthTex); cl->SetTexture(2, *normalTex);
    cl->BindUav(0, *h.vxgi_resolve);
    cl->Dispatch((rw+7)/8, (rh+7)/8, 1);
    return h.vxgi_resolve.Get();
}

/** 3D シーンを描画する (スカイ + ライト付きメッシュ + 選択ハイライト/太いギズモ)。 */
// ===== DrawScene3D の描画工程別関数群 =====

int SceneMeshWaterSlot(
    const FEditorHost& h,
    const AEditor3DRecordComponent* record) noexcept {
    if (record == nullptr) return 0;
    for (u32 index = 0u; index < h.water3d_draw_count; ++index) {
        if (h.water3d_draw_ids[index] == record->id) {
            return static_cast<int>(index + 1u);
        }
    }
    return 0;
}

FSceneMeshCacheKey BuildSceneMeshCacheKey(FEditorHost& h, game::ANode* node, bool active, const game::FTransform3D& world) noexcept {
    FSceneMeshCacheKey key{};
    key.node = node;
    if (node == nullptr) return key;

    key.parent = node->Parent();
    AEditor3DRecordComponent* record = Rec3D(node);
    game::AMeshComponent3D* component = Mesh3D(node);
    key.component = component;
    key.editor_id = record != nullptr ? record->id : 0;
    key.primitive = NPrim(node);
    key.water_slot = SceneMeshWaterSlot(h, record);
    key.world = world.ToMat4();
    key.node_color = NColor(node);

    constexpr u32 kEffectivelyActive = 1u << 0u;
    constexpr u32 kEmptyRecord = 1u << 1u;
    constexpr u32 kHasMeshComponent = 1u << 2u;
    constexpr u32 kHasRenderHandle = 1u << 3u;
    constexpr u32 kRenderedByWater = 1u << 4u;
    constexpr u32 kHasMaterial = 1u << 5u;
    constexpr u32 kMaterialLoaded = 1u << 6u;

    const bool empty = record != nullptr && record->is_empty;
    const bool rendered_by_water = IsRenderedByWater3D(h, node);
    if (active) key.state |= kEffectivelyActive;
    if (empty) key.state |= kEmptyRecord;
    if (component != nullptr) key.state |= kHasMeshComponent;
    if (rendered_by_water) key.state |= kRenderedByWater;

    if (component != nullptr) {
        key.render_handle = component->RenderHandle();
        if (key.render_handle != nullptr) key.state |= kHasRenderHandle;

        // Resolve lazy material state before capturing the exact fields used
        // by BuildSceneMeshVerts. This prevents a guaranteed second rebuild on
        // the frame after first material use.
        const bool contributes =
            active && !empty && key.render_handle == nullptr &&
            !rendered_by_water;
        if (contributes && component->HasMaterial() &&
            !component->MaterialLoaded()) {
            LoadNode3DMaterial(node);
        }
        if (component->HasMaterial()) key.state |= kHasMaterial;
        if (component->MaterialLoaded()) key.state |= kMaterialLoaded;
        const game::FMaterial2D& material = component->Material();
        key.material_kind = static_cast<u32>(material.kind);
        key.material_base_color = material.pbr.baseColor;
        key.metallic = material.pbr.metallic;
        key.roughness = material.pbr.roughness;
    }

    key.mesh =
        key.primitive == 3 ? NMesh(node)
      : key.primitive == 1 ? h.cpu_sphere.Get()
      : key.primitive == 2 ? h.cpu_plane.Get()
                           : h.cpu_cube.Get();
    if (key.mesh != nullptr) {
        key.mesh_revision = key.mesh->GeometryRevision();
    }
    return key;
}

bool SceneMeshCacheKeysMatch(
    const TArray<FSceneMeshCacheKey>& lhs,
    const TArray<FSceneMeshCacheKey>& rhs) noexcept {
    if (lhs.Num() != rhs.Num()) return false;
    for (u32 index = 0u; index < lhs.Num(); ++index) {
        if (!lhs[index].SameAs(rhs[index])) return false;
    }
    return true;
}

// シーンのメッシュ頂点を M3DVtx へ展開 + AABB を求める (シャドウ/本体/VXGI が共有)。
void BuildSceneMeshVerts(FEditorHost& h, const TArray<game::ANode*>& all3d,
                         TArray<FM3DVtx>& dv, FVec3& bbMin, FVec3& bbMax) noexcept {
    h.scene_mesh_vertex_offset.SetNum(all3d.Num());
    h.scene_mesh_vertex_count.SetNum(all3d.Num());
    h.scene_mesh_local_center.SetNum(all3d.Num());
    h.scene_mesh_local_radius.SetNum(all3d.Num());
    for (u32 i = 0; i < all3d.Num(); ++i) {
        h.scene_mesh_vertex_offset[i] = 0u;
        h.scene_mesh_vertex_count[i] = 0u;
        h.scene_mesh_local_center[i] = FVec3{0.0f, 0.0f, 0.0f};
        h.scene_mesh_local_radius[i] = -1.0f;
        game::ANode* nn = all3d[i];
        if (!SceneMeshHierarchyVisible(h, i, nn)) continue;
        { AEditor3DRecordComponent* er = Rec3D(nn); if (er != nullptr && er->is_empty) continue; }       // 空ノードは描画しない
        if (Mesh3D(nn) == nullptr || Mesh3D(nn)->RenderHandle() != nullptr) continue;   // スプライトは別パス
        const bool interactive_water =
            IsRenderedByWater3D(h, nn);
        const int prim = NPrim(nn);
        const AMeshAsset* cm = interactive_water
            ? WaterCpuMeshForNode3D(h, nn)
            : (prim == 3) ? NMesh(nn)
            : (prim == 1) ? h.cpu_sphere.Get()
            : (prim == 2) ? h.cpu_plane.Get()
                          : h.cpu_cube.Get();
        if (cm == nullptr || cm->Vertices().IsEmpty()) continue;
        FVec3 local_minimum{
            std::numeric_limits<f32>::max(),
            std::numeric_limits<f32>::max(),
            std::numeric_limits<f32>::max()};
        FVec3 local_maximum{
            -std::numeric_limits<f32>::max(),
            -std::numeric_limits<f32>::max(),
            -std::numeric_limits<f32>::max()};
        bool has_finite_vertex = false;
        for (u32 vertex_index = 0u;
             vertex_index < cm->Vertices().Num(); ++vertex_index) {
            const FVec3 point = cm->Vertices()[vertex_index].position;
            if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
                !std::isfinite(point.z)) {
                continue;
            }
            local_minimum.x = std::min(local_minimum.x, point.x);
            local_minimum.y = std::min(local_minimum.y, point.y);
            local_minimum.z = std::min(local_minimum.z, point.z);
            local_maximum.x = std::max(local_maximum.x, point.x);
            local_maximum.y = std::max(local_maximum.y, point.y);
            local_maximum.z = std::max(local_maximum.z, point.z);
            has_finite_vertex = true;
        }
        if (!has_finite_vertex) continue;
        const FVec3 local_center =
            (local_minimum + local_maximum) * 0.5f;
        f32 local_radius_squared = 0.0f;
        for (u32 vertex_index = 0u;
             vertex_index < cm->Vertices().Num(); ++vertex_index) {
            const FVec3 point = cm->Vertices()[vertex_index].position;
            if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
                !std::isfinite(point.z)) {
                continue;
            }
            const FVec3 delta = point - local_center;
            local_radius_squared = std::max(
                local_radius_squared,
                delta.x * delta.x + delta.y * delta.y +
                    delta.z * delta.z);
        }
        h.scene_mesh_local_center[i] = local_center;
        h.scene_mesh_local_radius[i] =
            std::sqrt(local_radius_squared);
        // Interactive water owns a separate indexed draw path, but it still
        // needs the exact submitted base-mesh bounds for the shared main-view
        // visibility mask and profiler. Do not duplicate it in the aggregate
        // opaque vertex buffer.
        if (interactive_water) continue;
        const FVec4 col = NColor(nn);
        game::AMeshComponent3D* mc = Mesh3D(nn);
        if (mc != nullptr && !mc->MaterialLoaded() && mc->HasMaterial()) LoadNode3DMaterial(nn);   // 遅延ロード (2D 鏡映)
        f32 mtl = 0.0f, rgh = 0.5f;
        FVec3 albedo{ col.x, col.y, col.z };                                          // 既定はノード色 (NColor)
        if (mc != nullptr) {
            const game::FMaterial2D& mat = mc->Material();
            mtl = mat.pbr.metallic; rgh = mat.pbr.roughness;
            if (mc->HasMaterial() && mat.kind == game::EMaterialKind::Lit)             // material 設定時は baseColor を採用
                albedo = FVec3{ mat.pbr.baseColor.x, mat.pbr.baseColor.y, mat.pbr.baseColor.z };
        }
        const u32 vertex_offset = dv.Num();
        AppendMeshTris(
            dv, cm, SceneMeshWorldTransform(h, i, nn).ToMat4(), albedo,
            h.m3d_dyn_cap, mtl, rgh);
        h.scene_mesh_vertex_offset[i] = vertex_offset;
        h.scene_mesh_vertex_count[i] =
            dv.Num() - vertex_offset;
    }
    for (u32 i = 0; i < dv.Num(); ++i) {
        const FM3DVtx& q = dv[i];
        if (q.px < bbMin.x) bbMin.x = q.px; if (q.py < bbMin.y) bbMin.y = q.py; if (q.pz < bbMin.z) bbMin.z = q.pz;
        if (q.px > bbMax.x) bbMax.x = q.px; if (q.py > bbMax.y) bbMax.y = q.py; if (q.pz > bbMax.z) bbMax.z = q.pz;
    }
}

editor_frustum_culling::FSubmissionMaskView SceneMeshSubmissionMask(
    const FEditorHost& host,
    editor_frustum_culling::ESceneGeometryPass pass) noexcept {
    const editor_frustum_culling::FSubmissionMaskView main_view_mask{
        host.profiler_work.frustum_culling_enabled,
        host.scene_mesh_visible.IsEmpty()
            ? nullptr : host.scene_mesh_visible.GetData(),
        static_cast<u32>(host.scene_mesh_visible.Num())};
    return editor_frustum_culling::SubmissionMaskForPass(
        pass, main_view_mask);
}

void BuildSceneMeshVisibility(
    FEditorHost& host, const TArray<game::ANode*>& nodes,
    const FMat4& view_projection) noexcept {
    host.profiler_work.frustum_culling_enabled = false;
    host.profiler_work.frustum_tested = 0u;
    host.profiler_work.frustum_visible = 0u;
    host.profiler_work.frustum_culled = 0u;
    host.scene_mesh_visible.SetNum(nodes.Num());
    for (u32 index = 0u; index < nodes.Num(); ++index) host.scene_mesh_visible[index] = SceneMeshHierarchyVisible(host, index, nodes[index]) ? 1u : 0u;

    if (!host.pbr3d_ready ||
        host.scene_mesh_local_center.Num() != nodes.Num() ||
        host.scene_mesh_local_radius.Num() != nodes.Num() ||
        host.scene_mesh_vertex_offset.Num() != nodes.Num() ||
        host.scene_mesh_vertex_count.Num() != nodes.Num()) {
        return;
    }
    editor_frustum_culling::FPlane planes[6];
    if (!editor_frustum_culling::ExtractPlanes(
            view_projection, planes))
        return;
    editor_frustum_culling::FFrameDecision frame{};

    host.frustum_centers_scratch.SetNum(nodes.Num());
    host.frustum_radii_scratch.SetNum(nodes.Num());
    host.frustum_scales_scratch.SetNum(nodes.Num());
    host.frustum_padding_scratch.SetNum(nodes.Num());
    host.frustum_node_indices_scratch.SetNum(nodes.Num());
    host.frustum_decisions_scratch.SetNum(nodes.Num());
    u32 candidate_count = 0u;
    for (u32 index = 0u; index < nodes.Num(); ++index) {
        game::ANode* node = nodes[index];
        AEditor3DRecordComponent* record = Rec3D(node);
        game::AMeshComponent3D* mesh = Mesh3D(node);
        if (!SceneMeshHierarchyVisible(host, index, node) || mesh == nullptr || (record != nullptr && record->is_empty) || mesh->RenderHandle() != nullptr) {
            continue;
        }
        const bool interactive_water =
            IsRenderedByWater3D(host, node);
        FGpuMesh* gpu_mesh = interactive_water
            ? WaterGpuMeshForNode3D(host, node)
            : GpuMeshForNode3D(host, node);
        if (gpu_mesh == nullptr || !gpu_mesh->vertex_buffer ||
            !gpu_mesh->index_buffer) {
            continue;
        }
        const game::FTransform3D world = SceneMeshWorldTransform(host, index, node);
        const FVec3 center = TransformPoint(
            host.scene_mesh_local_center[index],
            world.ToMat4());
        const f32 world_radius_padding = interactive_water && record != nullptr ? host.water3d.ConservativeDisplacementBoundForSurface(static_cast<u64>(static_cast<u32>(record->id)), WaterSurface3DParamsFor(record)) : 0.0f;
        host.frustum_centers_scratch[candidate_count] = center;
        host.frustum_radii_scratch[candidate_count] = host.scene_mesh_local_radius[index];
        host.frustum_scales_scratch[candidate_count] = world.scale;
        host.frustum_padding_scratch[candidate_count] = world_radius_padding;
        host.frustum_node_indices_scratch[candidate_count] = index;
        ++candidate_count;
    }
    editor_frustum_culling::EvaluateSpheresBatch(planes, host.frustum_centers_scratch.GetData(), host.frustum_radii_scratch.GetData(), host.frustum_scales_scratch.GetData(), host.frustum_padding_scratch.GetData(), candidate_count, host.frustum_decisions_scratch.GetData());
    for (u32 candidate = 0u; candidate < candidate_count; ++candidate) {
        const editor_frustum_culling::FNodeDecision& decision = host.frustum_decisions_scratch[candidate];
        frame.Apply(decision);
        if (!frame.enabled) {
            // 部分的な可視マスクを公開せず、無効入力では従来どおり fail-open にする。
            for (u32 reset = 0u; reset < nodes.Num(); ++reset)
                host.scene_mesh_visible[reset] = 1u;
            return;
        }
        host.scene_mesh_visible[host.frustum_node_indices_scratch[candidate]] = decision.visible ? 1u : 0u;
    }
    host.profiler_work.frustum_culling_enabled =
        frame.enabled;
    host.profiler_work.frustum_tested = frame.tested;
    host.profiler_work.frustum_visible = frame.visible;
    host.profiler_work.frustum_culled = frame.culled;
}

bool RefreshSceneMeshCache(
    FEditorHost& h,
    const TArray<game::ANode*>& all3d) noexcept {
    h.scene_mesh_key_scratch.Reset();
    h.scene_mesh_key_scratch.Reserve(all3d.Num());
    for (u32 index = 0u; index < all3d.Num(); ++index) {
        h.scene_mesh_key_scratch.Add(BuildSceneMeshCacheKey(h, all3d[index], SceneMeshHierarchyVisible(h, index, all3d[index]), SceneMeshWorldTransform(h, index, all3d[index])));
    }

    const bool cache_matches =
        h.scene_mesh_cache_valid &&
        h.scene_mesh_uploaded_vb == h.m3d_dyn_vb.Get() &&
        h.scene_mesh_cached_cap == h.m3d_dyn_cap &&
        SceneMeshCacheKeysMatch(
            h.scene_mesh_key, h.scene_mesh_key_scratch);
    if (cache_matches) return false;

    h.scene_mesh_vertices.Reset();
    if (h.scene_mesh_vertices.Max() == 0u) {
        h.scene_mesh_vertices.Reserve(8192u);
    }
    h.scene_mesh_bb_min = FVec3{1e30f, 1e30f, 1e30f};
    h.scene_mesh_bb_max = FVec3{-1e30f, -1e30f, -1e30f};
    BuildSceneMeshVerts(
        h, all3d, h.scene_mesh_vertices,
        h.scene_mesh_bb_min, h.scene_mesh_bb_max);
    if (!h.scene_mesh_vertices.IsEmpty() && h.m3d_dyn_vb) {
        h.m3d_dyn_vb->Update(
            h.scene_mesh_vertices.GetData(),
            sizeof(FM3DVtx) * h.scene_mesh_vertices.Num());
    }

    // Rotate the two key arrays so both retain their allocations. The next
    // frame clears only the old cache, avoiding allocator traffic after warmup.
    TArray<FSceneMeshCacheKey> old_key =
        Move(h.scene_mesh_key);
    h.scene_mesh_key = Move(h.scene_mesh_key_scratch);
    h.scene_mesh_key_scratch = Move(old_key);
    h.scene_mesh_key_scratch.Reset();

    h.scene_mesh_uploaded_vb = h.m3d_dyn_vb.Get();
    h.scene_mesh_cached_cap = h.m3d_dyn_cap;
    ++h.scene_mesh_revision;
    if (h.scene_mesh_revision == 0u) h.scene_mesh_revision = 1u;
    h.scene_mesh_cache_valid = h.m3d_dyn_vb.Get() != nullptr;
    h.profiler_work.scene_mesh_cache_rebuilt = true;
    return true;
}

// SH9 環境光を現在の空に追従して再計算 (拡散 ambient + 鏡面 fallback の元データ)。sh9_dirty 時のみ。
void Pass_UpdateSh9(FEditorHost& h) noexcept {
    if (h.pbr3d_ready && h.sh9_dirty) {
        FVec4 sh9[9]; ComputeSkySh9(sh9, h.sky_zenith, h.sky_horizon, h.sky_ground);
        for (int si = 0; si < 9; ++si) { sh9[si].x *= kSh9Ambient; sh9[si].y *= kSh9Ambient; sh9[si].z *= kSh9Ambient; }
        h.pbr3d.SetSh9(sh9);
        h.sh9_dirty = false;
    }
}

// IBL: CSky/物理大気を env cubemap 化 → irradiance + specular prefilter + BRDF-LUT を焼き CPbrShader へ。
// per-slice cubemap 描画なので «描画パスの外» (BeginShadowPass より前) で呼ぶこと。
void Pass_AtmosphereIbl(FEditorHost& h, IRhiCommandList* cl) noexcept {
    if (h.pbr3d_ready && h.sky3d_ready && (h.ibl_ready ? h.ibl_dirty : !h.ibl_tried)) {
        IRhiDevice* idev = h.renderer.Device();
        if (idev != nullptr) {
            h.sky3d.SetSunDirection(h.sun_dir);   // キャプチャ前に空を «現在のライティング» へ合わせる
            h.sky3d.SetSunColor(h.sun_color);
            h.sky3d.SetZenithColor(h.sky_zenith); h.sky3d.SetHorizonColor(h.sky_horizon); h.sky3d.SetGroundColor(h.sky_ground);
            if (h.ibl_ready && h.ibl_dirty) { idev->WaitIdle(); h.ibl3d.ResetEnvCubemap(); }   // 空変更 → 再キャプチャ
            // Procedural CSky still stores its analytic disc in the captured
            // cubemap and must exclude it from indirect convolutions.  The
            // physical-atmosphere bake is scattering-only: its solar disc is
            // rendered analytically at display resolution below, so masking an
            // arbitrary cone here would remove valid sky radiance.
            h.ibl3d.SetDirectLightExclusion(
                h.sun_dir,
                h.q_sky_mode == 1
                    ? 2.0f
                    : 1.0f - h.sky3d.SunRadius());
            bool ok = h.ibl3d.EnsureBrdfLut(*idev, *cl).IsOk();
            if (ok) {
                if (h.q_sky_mode == 1) {
                    acs::FAtmosphereParams ap; ap.sun_dir = h.sun_dir;
                    const f32 atmosphereSunRadiance =
                        PhysicalAtmosphereSunRadiance(h.sun_intensity);
                    ap.sun_intensity = FVec3{
                        atmosphereSunRadiance * h.sun_color.x,
                        atmosphereSunRadiance * h.sun_color.y,
                        atmosphereSunRadiance * h.sun_color.z};
                    if (!h.sky_atmo_tried) { h.sky_atmo_tried = true; (void)h.sky_atmo.Init(*idev); }
                    acs::TArray<f32> sky;
                    u32 skyWidth = kPhysicalSkyEquirectWidth;
                    u32 skyHeight = kPhysicalSkyEquirectHeight;
                    bool gpu = h.sky_atmo.Ready() && h.sky_atmo.BakeEquirect(
                        *idev, *cl, ap,
                        skyWidth,
                        skyHeight,
                        sky);
                    if (!gpu) {
                        skyWidth = kPhysicalSkyCpuFallbackWidth;
                        skyHeight = kPhysicalSkyCpuFallbackHeight;
                        sky = acs::CAtmosphere::BakeEquirect(
                            skyWidth,
                            skyHeight,
                            ap);
                    }
                    for (u32 si = 0; si < sky.Num(); ++si) sky[si] *= kAtmosScale;
                    ok = h.ibl3d.LoadEquirectHdrFromMemory(
                        *idev, *cl, sky.GetData(),
                        skyWidth,
                        skyHeight).IsOk();
                } else {
                    ok = h.ibl3d.EnsureEnvCubemap(*idev, *cl, h.sky3d).IsOk();   // CSky 手続き式を env cubemap に
                }
                ok = ok && h.ibl3d.EnsureIrradiance(*idev, *cl).IsOk()
                        && h.ibl3d.EnsurePrefilter(*idev, *cl).IsOk();
            }
            h.ibl_ready = ok;
            h.ibl_dirty = false;
            if (!ok) { h.ibl_tried = true; ACS_LOG_WARN("[3D] IBL 生成失敗 (Diligent backend 必須?)。SH9 にフォールバック"); }
            else       ACS_LOG_INFO("[3D] IBL 環境光 OK (%s→irradiance+prefilter %u mip+BRDF-LUT)",
                                    h.q_sky_mode == 1 ? "物理大気" : "CSky", h.ibl3d.PrefilterMips());
        }
    }
}

// シャドウパスの出力 (本体パスが PCF/CSM 比較に使う)。
struct FShadowOut {
    FMat4 lightVp{};
    bool  shadowOn = false;
    bool  csmActive = false;
    FMat4 csmVps[acs::CShadowMap::kMaxCascades]    = {};
    f32   csmSplits[acs::CShadowMap::kMaxCascades] = {};
};

static_assert(
    !editor_frustum_culling::SceneGeometryPassPolicy(
        editor_frustum_culling::ESceneGeometryPass::ShadowCaster)
         .uses_main_view_mask,
    "shadow casters are light-space and must not use the camera mask");
static_assert(
    editor_frustum_culling::SceneGeometryPassPolicy(
        editor_frustum_culling::ESceneGeometryPass::ShadowCaster)
        .command_form ==
        editor_frustum_culling::ESubmissionCommandForm::Draw);

// シャドウパス: 光源 (太陽) 視点で深度を焼く → 本体パスで PCF 比較してキャスト影を落とす。
// 品質プリセットの影サイズ/カスケード数に追従 (size 0=影オフ)。CSM は «透視 + cascade>=2» のみ。
FShadowOut Pass_Shadows(FEditorHost& h, IRhiCommandList* cl, u32 dvCount,
                        const FVec3& bbMin, const FVec3& bbMax, const FVec3& eye,
                        const CCamera& cam, bool camera_orthographic) noexcept {
    const bool wantCsm =
        (h.q_shadow_cascades >= 2) &&
        !camera_orthographic &&
        h.q_shadow_size > 0;
    const u32  wantCascades = wantCsm ? (h.q_shadow_cascades <= acs::CShadowMap::kMaxCascades
                                         ? h.q_shadow_cascades : acs::CShadowMap::kMaxCascades) : 1u;
    if (h.shadow_ready && h.q_shadow_size > 0 &&
        (h.q_shadow_size != h.shadow.Size() || wantCascades != h.shadow.CascadeCount())) {
        IRhiDevice* sdev = h.renderer.Device();
        if (sdev != nullptr) { h.shadow.Shutdown(); (void)h.shadow.Init(*sdev, h.q_shadow_size, wantCascades); }
    }
    FShadowOut o;
    if (h.shadow_ready && h.q_shadow_size > 0 && h.shadow.DepthTexture() != nullptr && dvCount > 0) {
        const FVec3 center{ (bbMin.x+bbMax.x)*0.5f, (bbMin.y+bbMax.y)*0.5f, (bbMin.z+bbMax.z)*0.5f };
        const f32 ex = bbMax.x-bbMin.x, ey = bbMax.y-bbMin.y, ez = bbMax.z-bbMin.z;
        const f32 radius = 0.5f * std::sqrt(ex*ex + ey*ey + ez*ez) + 1.0f;
        const FVec3 lightDir = h.sun_dir;   // 太陽方向 (設定駆動)
        if (h.shadow.CascadeCount() >= 2) {
            // CSM: カメラ frustum を距離で分割。far は «視点→シーン» に切り詰め (既定 500 のままだと遠景に解像度浪費)。
            const f32 dx = eye.x-center.x, dy = eye.y-center.y, dz = eye.z-center.z;
            f32 farZ = std::sqrt(dx*dx+dy*dy+dz*dz) + radius * 2.0f;
            if (farZ < 20.0f) farZ = 20.0f; else if (farZ > 300.0f) farZ = 300.0f;
            h.shadow.SetDirectionalLightCascades(lightDir, cam.View(), cam.Projection(), 0.05f, farZ);
            cl->BeginShadowPass(*h.shadow.DepthTexture(), 1.0f);       // atlas 全体 clear
            cl->SetPipeline(*h.shadow_caster_pipe);
            cl->SetVertexBuffer(*h.m3d_dyn_vb, sizeof(FM3DVtx));
            const u32 nc = h.shadow.CascadeCount();
            for (u32 c = 0; c < nc; ++c) {
                o.csmVps[c]    = h.shadow.LightViewProjection(c);
                o.csmSplits[c] = h.shadow.CascadeSplit(c);
                if (!h.shadow_cascade_cb[c]) continue;
                cl->SetViewport(h.shadow.CascadeViewport(c));          // atlas 内の当該 cascade 領域だけに描く
                cl->SetScissor(h.shadow.CascadeScissor(c));
                h.shadow_cascade_cb[c]->Update(&o.csmVps[c], sizeof(FMat4));   // cascade 毎に別 CB (1フレーム上書き回避)
                cl->SetConstantBuffer(0, *h.shadow_cascade_cb[c]);
                cl->Draw(dvCount, 0);
            }
            cl->EndShadowPass(*h.shadow.DepthTexture());
            o.lightVp   = o.csmVps[0];   // fallback(kMesh3DHLSL)用に cascade0 を残す
            o.csmActive = true;
            o.shadowOn  = true;
        } else {
            h.shadow.SetDirectionalLight(lightDir, center, radius);    // single cascade (実績パス)
            o.lightVp = h.shadow.LightViewProjection();
            h.shadow_lvp_cb->Update(&o.lightVp, sizeof(o.lightVp));
            cl->BeginShadowPass(*h.shadow.DepthTexture(), 1.0f);
            cl->SetPipeline(*h.shadow_caster_pipe);
            cl->SetConstantBuffer(0, *h.shadow_lvp_cb);
            cl->SetVertexBuffer(*h.m3d_dyn_vb, sizeof(FM3DVtx));
            cl->Draw(dvCount, 0);
            cl->EndShadowPass(*h.shadow.DepthTexture());
            o.shadowOn = true;   // シャドウマップ有効 (本パスで CPbrShader が PCF サンプル)
        }
    }
    return o;
}

/** Hot-enabling SSGI uses the same CPU-only staging as startup. */
void AdvanceRuntimeSsgi(FEditorHost& h, u32 width, u32 height) noexcept {
    IRhiDevice* const device = h.renderer.Device();
    if (device == nullptr) return;

    // A job may finish after the user turns SSGI off.  Join and discard it
    // without stalling the current draw; Poll is non-blocking while running.
    if (h.startup_worker_kind == 2u) {
        const i32 worker_result = PollStartupWorker(h);
        if (worker_result == 0) return;
        h.startup_worker_kind = 0u;
        if (worker_result > 0 && h.q_ssgi_on) {
            const auto result = h.ssgi3d.InitWithCompiledShaders(
                *device,
                Move(h.startup_ssgi_shaders),
                width > 0u ? width : 1u,
                height > 0u ? height : 1u);
            h.ssgi_ready = result.IsOk();
            if (h.ssgi_ready) {
                h.ssgi_w = width > 0u ? width : 1u;
                h.ssgi_h = height > 0u ? height : 1u;
            } else {
                h.ssgi_w = 0u;
                h.ssgi_h = 0u;
                ACS_LOG_WARN(
                    "[3D] hot-enabled CSsgi owner RHI commit failed: %s",
                    result.Error().message);
            }
        } else {
            h.startup_ssgi_shaders = {};
        }
        return;
    }

    if (!h.q_ssgi_on || h.ssgi_ready || h.ssgi_init_tried) return;

    const char* const backend_name = device->BackendName();
    const bool raw_dx12 = backend_name != nullptr &&
                          std::strcmp(backend_name, "DX12") == 0;
    if (raw_dx12) {
        // SSGI and lazy SSSS share the single raw-DX12 CPU worker. A busy
        // worker is transient: leave init_tried clear so the next render pump
        // retries after the current owner releases the slot.
        if (h.startup_worker_kind != 0u ||
            h.startup_worker.Joinable() ||
            h.startup_worker_state.load(
                std::memory_order_acquire) != 0) {
            return;
        }
        if (!BeginSsgiCompileWorker(h)) {
            // The slot was proven idle above, so failure here is a terminal
            // thread-spawn error rather than shared-worker contention.
            h.ssgi_init_tried = true;
            ACS_LOG_WARN(
                "[3D] hot-enabled SSGI worker unavailable; effect remains disabled");
        } else {
            h.ssgi_init_tried = true;
        }
        return;
    }

    // Other backends preserve their established compiler/resource path.
    h.ssgi_init_tried = true;
    const auto result = h.ssgi3d.Init(
        *device,
        width > 0u ? width : 1u,
        height > 0u ? height : 1u);
    h.ssgi_ready = result.IsOk();
    if (h.ssgi_ready) {
        h.ssgi_w = width > 0u ? width : 1u;
        h.ssgi_h = height > 0u ? height : 1u;
    } else {
        h.ssgi_w = 0u;
        h.ssgi_h = 0u;
        ACS_LOG_WARN("[3D] hot-enabled CSsgi init failed: %s",
                     result.Error().message);
    }
}

editor_subsurface_visibility::FPresence
InspectOpaqueSsssMaterials(
    FEditorHost& host,
    const TArray<game::ANode*>& nodes) noexcept {
    constexpr f32 kSsssEpsilon = 1.0e-4f;
    editor_subsurface_visibility::FPresence presence{};
    const editor_frustum_culling::FSubmissionMaskView
        main_view_mask = SceneMeshSubmissionMask(
            host,
            editor_frustum_culling::ESceneGeometryPass::PbrOpaqueCount);
    for (u32 node_index = 0u; node_index < nodes.Num(); ++node_index) {
        game::ANode* node = nodes[node_index];
        AEditor3DRecordComponent* record = Rec3D(node);
        game::AMeshComponent3D* mesh = Mesh3D(node);
        if (!SceneMeshHierarchyVisible(host, node_index, node) || mesh == nullptr || (record != nullptr && record->is_empty) || mesh->RenderHandle() != nullptr || IsRenderedByWater3D(host, node)) {
            continue;
        }
        if (!mesh->MaterialLoaded() && mesh->HasMaterial()) {
            LoadNode3DMaterial(node);
        }
        const game::FMaterial2D& material = mesh->Material();
        if (!mesh->HasMaterial() ||
            material.kind != game::EMaterialKind::Lit ||
            material.pbr.transmission > kSsssEpsilon) {
            continue;
        }
        bool has_subsurface =
            material.pbr.subsurface > kSsssEpsilon;

        const FSubstrateMaterial& substrate = material.substrate;
        if (!has_subsurface && substrate.enabled) {
            const u32 node_count =
                substrate.node_count < kSubstrateMaxNodes
                    ? substrate.node_count : kSubstrateMaxNodes;
            for (u32 slab_index = 0u;
                 slab_index < node_count; ++slab_index) {
                const FSubstrateNode& substrate_node =
                    substrate.nodes[slab_index];
                if (substrate_node.type != ESubstrateNodeType::Slab)
                    continue;
                const FSubstrateSlab& slab = substrate_node.slab;
                const f32 max_mfp =
                    std::max(slab.mean_free_path_cm.x,
                        std::max(slab.mean_free_path_cm.y,
                                 slab.mean_free_path_cm.z));
                // Stable slab scalar targets:
                // 16..18=MFP RGB, 26=thickness.
                const bool dynamic_mfp =
                    substrate_node.expressions.roots[16] >= 0 ||
                    substrate_node.expressions.roots[17] >= 0 ||
                    substrate_node.expressions.roots[18] >= 0;
                const bool dynamic_thickness =
                    substrate_node.expressions.roots[26] >= 0;
                if ((max_mfp > kSsssEpsilon || dynamic_mfp) &&
                    (slab.thickness_cm > kSsssEpsilon ||
                     dynamic_thickness)) {
                    has_subsurface = true;
                    break;
                }
            }
        }
        FGpuMesh* gpu_mesh =
            has_subsurface ? GpuMeshForNode3D(host, node) : nullptr;
        const bool eligible_for_main_view_draw =
            gpu_mesh != nullptr &&
            gpu_mesh->vertex_buffer.Get() != nullptr &&
            gpu_mesh->index_buffer.Get() != nullptr;
        presence.Observe(
            node_index, has_subsurface,
            eligible_for_main_view_draw, main_view_mask);
        if (presence.Complete()) return presence;
    }
    return presence;
}

struct FPbrFrameDrawCounts {
    u32 opaque = 0u;
    u32 water_fallback = 0u;

    u32 RequiredObjectBuffers() const noexcept {
        const u64 required =
            static_cast<u64>(opaque) +
            static_cast<u64>(water_fallback);
        return required > static_cast<u64>(~u32{0})
            ? ~u32{0} : static_cast<u32>(required);
    }
};

FPbrFrameDrawCounts CountPbrFrameDraws(
    FEditorHost& host,
    const TArray<game::ANode*>& nodes) noexcept {
    FPbrFrameDrawCounts counts{};
    editor_frustum_culling::ForEachSubmittedNode(
        SceneMeshSubmissionMask(
            host,
            editor_frustum_culling::ESceneGeometryPass::PbrOpaqueCount),
        nodes.Num(),
        [&](u32 index) noexcept {
        game::ANode* node = nodes[index];
        AEditor3DRecordComponent* record = Rec3D(node);
        game::AMeshComponent3D* mesh = Mesh3D(node);
        if (!SceneMeshHierarchyVisible(host, index, node) || mesh == nullptr || (record != nullptr && record->is_empty) || mesh->RenderHandle() != nullptr) {
            return;
        }
        if (IsRenderedByWater3D(host, node)) {
            if (WaterGpuMeshForNode3D(host, node) != nullptr &&
                counts.water_fallback != ~u32{0}) {
                ++counts.water_fallback;
            }
            return;
        }
        FGpuMesh* gpu_mesh = GpuMeshForNode3D(host, node);
        if (gpu_mesh == nullptr || !gpu_mesh->vertex_buffer ||
            !gpu_mesh->index_buffer) {
            return;
        }
        if (!mesh->MaterialLoaded() && mesh->HasMaterial())
            LoadNode3DMaterial(node);
        if (mesh->Material().pbr.transmission > 0.0f) return;
        if (counts.opaque != ~u32{0}) ++counts.opaque;
    });
    return counts;
}

bool DrawEditorPbrNode(FEditorHost& host, IRhiCommandList& command_list, game::ANode* node, const game::FTransform3D& world, bool subsurface_mrt) noexcept {
    if (!IsEffectivelyVisibleAndEnabled(node)) return true;
    AEditor3DRecordComponent* record = Rec3D(node);
    if (record != nullptr && record->is_empty) return true;
    game::AMeshComponent3D* mesh = Mesh3D(node);
    if (mesh == nullptr || mesh->RenderHandle() != nullptr ||
        IsRenderedByWater3D(host, node)) {
        return true;
    }
    FGpuMesh* gpu_mesh = GpuMeshForNode3D(host, node);
    if (gpu_mesh == nullptr || !gpu_mesh->vertex_buffer ||
        !gpu_mesh->index_buffer) {
        return true;
    }
    if (!mesh->MaterialLoaded() && mesh->HasMaterial())
        LoadNode3DMaterial(node);
    const FVec4 node_color = NColor(node);
    const game::FPbrParams2D& pbr = mesh->Material().pbr;
    // Transmission is rendered once after the opaque scene has been captured.
    if (pbr.transmission > 0.0f) return true;
    const bool lit =
        mesh->HasMaterial() &&
        mesh->Material().kind == game::EMaterialKind::Lit;
    const FVec3 albedo =
        lit ? FVec3{
                  pbr.baseColor.x, pbr.baseColor.y, pbr.baseColor.z}
            : FVec3{
                  node_color.x, node_color.y, node_color.z};
    if (lit) LoadNode3DMaterialTextures(host, node);
    host.pbr3d.SetNormalMap(
        lit && record != nullptr
            ? record->material_normal_tex.Get() : nullptr,
        lit ? pbr.normalStrength : 1.0f);

    const bool substrate_active =
        lit && mesh->Material().substrate.enabled &&
        host.pbr3d.SetSubstrateMaterial(
            mesh->Material().substrate, host.time);
    if (substrate_active && record != nullptr) {
        for (u32 slot = 0u;
             slot < kShaderExpressionMaxTextureSlots;
             ++slot) {
            host.pbr3d.SetSubstrateExpressionTexture(
                slot, record->material_expression_tex[slot].Get());
        }
    } else {
        host.pbr3d.ClearSubstrateSurface();
        host.pbr3d.SetExtParams(
            pbr.clearcoat, pbr.clearcoatRoughness, pbr.anisotropy);
        host.pbr3d.SetSheen(
            pbr.sheenColor, pbr.sheen, pbr.sheenRoughness);
        host.pbr3d.SetSubsurface(
            pbr.subsurfaceColor, pbr.subsurface);
        host.pbr3d.SetEmissive(
            pbr.emissive, pbr.emissiveStrength);
    }
    if (subsurface_mrt) {
        return host.pbr3d.DrawMeshSubsurfaceMrt(
            command_list, *gpu_mesh, world.ToMat4(), albedo,
            lit ? pbr.metallic : 0.0f,
            lit ? pbr.roughness : 0.5f, pbr.ao,
            lit && record != nullptr
                ? record->material_albedo_tex.Get() : nullptr);
    }
    return host.pbr3d.DrawMesh(
        command_list, *gpu_mesh, world.ToMat4(), albedo,
        lit ? pbr.metallic : 0.0f,
        lit ? pbr.roughness : 0.5f, pbr.ao,
        lit && record != nullptr
            ? record->material_albedo_tex.Get() : nullptr);
}

/**
 * Non-blocking lazy SSSS initialization.
 *
 * Raw DX12 builds the complete pipeline-side candidate on the existing startup
 * worker; Diligent submits to its backend compiler and is polled once per
 * frame. Full-resolution targets are separate later-frame commits. Until the
 * complete target stack is ready the analytic, single-RT PBR path remains live.
 */
bool AdvanceRuntimeSsss(
    FEditorHost& host, u32 width, u32 height,
    bool requested) noexcept {
    IRhiDevice* const device = host.renderer.Device();
    if (device == nullptr) return false;

    const char* const backend_name = device->BackendName();
    const bool raw_dx12 =
        backend_name != nullptr &&
        std::strcmp(backend_name, "DX12") == 0;

    // A material hot edit may remove the final SSS dependency while shader
    // work is still running. Poll and drain that work before the request gate:
    // neither a joinable raw worker nor backend shader handles may be orphaned,
    // and removing SSS must never publish pipelines or allocate targets.
    if (!requested && host.ssss3d_init_state == 1u) {
        if (raw_dx12) {
            if (host.startup_worker_kind != 6u) {
                // kind==0 means there is no SSSS worker that this state owns;
                // another kind belongs to a different startup phase. Never
                // join either one from the hot-remove path.
                return false;
            }
            const i32 worker_result = PollStartupWorker(host);
            if (worker_result == 0) return false;
            host.startup_worker_kind = 0u;
            host.startup_ssss_candidate_device = nullptr;
            // A successful raw worker wrote an unpublished pipeline candidate
            // directly into ssss3d. Discard it on the owner after the acquire
            // poll + join; a failed worker is shut down defensively as well.
            host.ssss3d.Shutdown();
        } else {
            const EShaderStatus shader_status =
                host.ssss3d_pending_shaders.Status();
            if (shader_status == EShaderStatus::Compiling) return false;
        }
        host.ssss3d_pending_shaders = {};
        host.ssss3d_ready = false;
        host.ssss3d_init_state = 0u;
        host.ssss3d_init_failed = false;
        return false;
    }

    if (!requested) {
        // Never retain a half-built external MRT bundle after the last
        // dependent material disappears. Published resources remain cached
        // for a future material edit, matching the other optional effects.
        host.ssss_pending_diffuse_rt.Reset();
        host.ssss_pending_w = 0u;
        host.ssss_pending_h = 0u;
        host.ssss_frame_resource_state = 0u;
        host.ssss_frame_failed_w = 0u;
        host.ssss_frame_failed_h = 0u;
        return false;
    }
    if (width == 0u || height == 0u ||
        host.ssss3d_init_failed) {
        return false;
    }

    if (host.ssss3d_ready) {
        if (host.ssss3d.Width() == width &&
            host.ssss3d.Height() == height) {
            return true;
        }
        const editor_profiler::FTimePoint resize_begin =
            editor_profiler::CClock::now();
        const auto resize_result =
            host.ssss3d.Resize(width, height);
        if (resize_result.IsOk()) {
            ACS_LOG_INFO(
                "[3D] SSSS internal target resize %ux%u: %.3f ms",
                width, height,
                editor_profiler::ElapsedMilliseconds(resize_begin));
            // External MRT candidates begin on the next render-pump call.
            return false;
        }
        ACS_LOG_WARN(
            "[3D] SSSS resize failed; keeping single-RT PBR: %s",
            resize_result.Error().message);
        return false;
    }

    if (host.ssss3d_init_state == 1u) {
        if (raw_dx12) {
            if (host.startup_worker_kind != 6u) return false;
            const i32 worker_result = PollStartupWorker(host);
            if (worker_result == 0) return false;
            host.startup_worker_kind = 0u;
            host.startup_ssss_candidate_device = nullptr;
            if (worker_result > 0 &&
                host.ssss3d.HasPipelineResources()) {
                host.ssss3d_init_state = 2u;
                ACS_LOG_INFO(
                    "[3D] SSSS raw pipeline candidate published "
                    "without owner-thread RHI creation "
                    "(worker %.3f ms)",
                    host.startup_worker_elapsed_ms);
                // The internal target pair is a later render-pump commit.
                return false;
            }
            host.ssss3d.Shutdown();
            host.ssss3d_init_state = 3u;
            host.ssss3d_init_failed = true;
            ACS_LOG_WARN(
                "[3D] SSSS background pipeline candidate failed; "
                "single-RT analytic SSS remains active");
            return false;
        }

        const EShaderStatus shader_status =
            host.ssss3d_pending_shaders.Status();
        if (shader_status == EShaderStatus::Compiling) return false;
        if (shader_status != EShaderStatus::Ready) {
            host.ssss3d_pending_shaders = {};
            host.ssss3d_init_state = 3u;
            host.ssss3d_init_failed = true;
            ACS_LOG_WARN(
                "[3D] SSSS asynchronous compile failed; "
                "single-RT analytic SSS remains active");
            return false;
        }

        const editor_profiler::FTimePoint commit_begin =
            editor_profiler::CClock::now();
        const auto commit_result =
            host.ssss3d.InitPipelineResourcesWithCompiledShaders(
                *device, Move(host.ssss3d_pending_shaders));
        if (commit_result.IsErr()) {
            host.ssss3d_init_state = 3u;
            host.ssss3d_init_failed = true;
            ACS_LOG_WARN(
                "[3D] SSSS owner-thread RHI commit failed; "
                "keeping single-RT PBR: %s",
                commit_result.Error().message);
            return false;
        }
        host.ssss3d_init_state = 2u;
        ACS_LOG_INFO(
            "[3D] SSSS asynchronous pipeline commit complete: %.3f ms; "
            "full-resolution targets remain staged",
            editor_profiler::ElapsedMilliseconds(commit_begin));
        // Keep target allocation out of the pipeline publication frame.
        return false;
    }

    if (host.ssss3d_init_state == 2u) {
        const editor_profiler::FTimePoint targets_begin =
            editor_profiler::CClock::now();
        const auto targets_result = host.ssss3d.Resize(width, height);
        if (targets_result.IsErr()) {
            host.ssss3d_init_state = 3u;
            host.ssss3d_init_failed = true;
            ACS_LOG_WARN(
                "[3D] SSSS internal target allocation failed at %ux%u; "
                "single-RT analytic SSS remains active: %s",
                width, height, targets_result.Error().message);
            host.ssss3d.Shutdown();
            return false;
        }
        host.ssss3d_ready = true;
        ACS_LOG_INFO(
            "[3D] SSSS internal target pair ready at %ux%u: %.3f ms",
            width, height,
            editor_profiler::ElapsedMilliseconds(targets_begin));
        // External MRT candidates begin on the next render-pump call.
        return false;
    }

    if (host.ssss3d_init_state != 0u) return false;
    if (raw_dx12) {
        // Another CPU shader job owns the single startup worker. Retry next
        // frame rather than synchronously compiling or permanently disabling.
        if (host.startup_worker_kind != 0u ||
            !BeginSsssCompileWorker(host, *device)) {
            return false;
        }
        host.ssss3d_init_state = 1u;
        return false;
    }

    if (!device->SupportsAsyncShaderCompilation()) {
        host.ssss3d_init_state = 3u;
        host.ssss3d_init_failed = true;
        ACS_LOG_WARN(
            "[3D] SSSS backend has no non-blocking shader compiler; "
            "single-RT analytic SSS remains active");
        return false;
    }
    auto shader_result =
        CSubsurfaceScattering::BeginCompileShadersAsync(*device);
    if (shader_result.IsErr()) {
        host.ssss3d_init_state = 3u;
        host.ssss3d_init_failed = true;
        ACS_LOG_WARN(
            "[3D] SSSS asynchronous submission failed; "
            "single-RT analytic SSS remains active: %s",
            shader_result.Error().message);
        return false;
    }
    host.ssss3d_pending_shaders =
        Move(shader_result.Value());
    host.ssss3d_init_state = 1u;
    return false;
}

bool EnsureSsssFrameResources(
    FEditorHost& host, u32 width, u32 height) noexcept {
    if (!host.ssss3d_ready || width == 0u || height == 0u) {
        return false;
    }
    IRhiDevice* device = host.renderer.Device();
    if (device == nullptr) return false;

    if (host.ssss_diffuse_rt && host.ssss_material_rt &&
        host.ssss_w == width && host.ssss_h == height &&
        host.ssss_diffuse_rt->Width() == width &&
        host.ssss_diffuse_rt->Height() == height &&
        host.ssss_material_rt->Width() == width &&
        host.ssss_material_rt->Height() == height) {
        return true;
    }

    if (host.ssss_frame_resource_state == 2u &&
        host.ssss_frame_failed_w == width &&
        host.ssss_frame_failed_h == height) {
        return false;
    }

    // A viewport change invalidates only the unpublished candidate. Preserve
    // the last complete active pair until a new complete pair can be swapped.
    if (host.ssss_pending_diffuse_rt &&
        (host.ssss_pending_w != width ||
         host.ssss_pending_h != height)) {
        host.ssss_pending_diffuse_rt.Reset();
        host.ssss_pending_w = 0u;
        host.ssss_pending_h = 0u;
        host.ssss_frame_resource_state = 0u;
    }
    if (host.ssss_frame_resource_state == 2u) {
        host.ssss_frame_resource_state = 0u;
        host.ssss_frame_failed_w = 0u;
        host.ssss_frame_failed_h = 0u;
    }

    FTextureDesc diffuse_desc{};
    diffuse_desc.width = width;
    diffuse_desc.height = height;
    diffuse_desc.format = EFormat::R16G16B16A16_Float;
    diffuse_desc.is_render_target = true;

    if (!host.ssss_pending_diffuse_rt) {
        const editor_profiler::FTimePoint diffuse_begin =
            editor_profiler::CClock::now();
        auto diffuse_result = CreateRhiTexture(*device, diffuse_desc);
        if (diffuse_result.IsErr()) {
            host.ssss_frame_resource_state = 2u;
            host.ssss_frame_failed_w = width;
            host.ssss_frame_failed_h = height;
            ACS_LOG_WARN(
                "[3D] SSSS diffuse RT unavailable at %ux%u; "
                "keeping single-RT PBR: %s",
                width, height, diffuse_result.Error().message);
            return false;
        }
        host.ssss_pending_diffuse_rt = Move(diffuse_result.Value());
        host.ssss_pending_w = width;
        host.ssss_pending_h = height;
        host.ssss_frame_resource_state = 1u;
        ACS_LOG_INFO(
            "[3D] SSSS diffuse MRT candidate ready at %ux%u: %.3f ms",
            width, height,
            editor_profiler::ElapsedMilliseconds(diffuse_begin));
        // Material data is deliberately allocated on the next render pump.
        return false;
    }

    FTextureDesc material_desc = diffuse_desc;
    // RGB stores authored world-space diffusion radii and A stores coverage.
    // Half precision preserves Substrate mean-free-path differences that were
    // previously collapsed into an 8-bit scalar/hash mask.
    material_desc.format = EFormat::R16G16B16A16_Float;
    const editor_profiler::FTimePoint material_begin =
        editor_profiler::CClock::now();
    auto material_result = CreateRhiTexture(*device, material_desc);
    if (material_result.IsErr()) {
        host.ssss_pending_diffuse_rt.Reset();
        host.ssss_pending_w = 0u;
        host.ssss_pending_h = 0u;
        host.ssss_frame_resource_state = 2u;
        host.ssss_frame_failed_w = width;
        host.ssss_frame_failed_h = height;
        ACS_LOG_WARN(
            "[3D] SSSS material RT unavailable at %ux%u; "
            "keeping single-RT PBR: %s",
            width, height, material_result.Error().message);
        return false;
    }

    // Publish only a complete, size-matched pair.
    host.ssss_diffuse_rt = Move(host.ssss_pending_diffuse_rt);
    host.ssss_material_rt = Move(material_result.Value());
    host.ssss_w = width;
    host.ssss_h = height;
    host.ssss_pending_w = 0u;
    host.ssss_pending_h = 0u;
    host.ssss_frame_resource_state = 0u;
    host.ssss_frame_failed_w = 0u;
    host.ssss_frame_failed_h = 0u;
    ACS_LOG_INFO(
        "[3D] SSSS complete external MRT pair published at %ux%u "
        "(material allocation %.3f ms)",
        width, height,
        editor_profiler::ElapsedMilliseconds(material_begin));
    return true;
}

void DrawScene3D(FEditorHost& h, u32 scW, u32 scH) noexcept {
    if (!Ensure3D(h)) {
        InvalidateTemporalRenderHistories(h);
        return;
    }
    IRhiCommandList* cl = h.renderer.CommandList();
    if (cl == nullptr) {
        InvalidateTemporalRenderHistories(h);
        return;
    }

    AdvanceRuntimeSsgi(h, scW, scH);

    // Build retained scene geometry before resolving the Game View fallback:
    // the fallback frames authored scene bounds and never inherits the
    // editor's orbit pose.
    TArray<game::ANode*>& all3d = h.scene_mesh_nodes;
    TArray<FM3DVtx>& dv = h.scene_mesh_vertices;
    {
        editor_profiler::FCpuScope meshPrepassScope(
            h.profiler_work.opaque_cpu_ms);
        all3d.Reset();
        Dfs3DCollect(&h.scene3d.Root(), all3d);
        h.scene_mesh_world_batch_ready = h.scene_mesh_world_batch.Evaluate(&h.scene3d.Root(), all3d.Num());
        h.scene_mesh_hierarchy_batch_ready = h.scene_mesh_hierarchy_visibility.Evaluate(all3d.GetData(), all3d.Num(), &h.scene3d.Root());
        PrepareWater3DDrawEligibility(h, all3d);
        (void)RefreshSceneMeshCache(h, all3d);
    }
    const FVec3& bbMin = h.scene_mesh_bb_min;
    const FVec3& bbMax = h.scene_mesh_bb_max;
    u32 dvCount = static_cast<u32>(dv.Num());

    const f32 aspect = (scH > 0) ? static_cast<f32>(scW) / static_cast<f32>(scH) : 1.0f;
    const FRenderCamera3D renderCamera =
        ResolveRenderCamera3D(h, aspect, all3d);
    u64 camera_view_request_id = 0u;
    int camera_view_node_id = -1;
    const char* camera_view_stable_id = nullptr;
    u32 camera_view_history_generation = 0u;
    const bool rendering_camera_view_request =
        h.game_view &&
        h.camera_view_requests.PresenterIdentity(
            camera_view_request_id,
            camera_view_node_id,
            camera_view_stable_id,
            camera_view_history_generation) &&
        camera_view_node_id == renderCamera.node_id;
    (void)camera_view_stable_id;
    const u64 current_camera_view_request_id =
        rendering_camera_view_request
            ? camera_view_request_id : 0u;
    const bool camera_view_history_reset =
        h.last_render_camera_view_request_id !=
            current_camera_view_request_id ||
        (rendering_camera_view_request &&
         h.last_render_camera_view_history_generation !=
             camera_view_history_generation);
    const CCamera& cam = renderCamera.camera;
    const FVec3 eye = renderCamera.eye;
    const bool renderOrtho = renderCamera.orthographic;
    h.profiler_work.render_orthographic = renderOrtho;
    h.profiler_work.render_camera_resolved = true;
    const FMat4 vp_nojit = cam.ViewProjection();
    const bool render_projection_changed =
        h.last_render_camera_projection_valid &&
        h.last_render_camera_orthographic != renderOrtho;
    h.last_render_camera_projection_valid = true;
    h.last_render_camera_orthographic = renderOrtho;
    const bool render_camera_changed =
        h.last_render_camera_node_id != renderCamera.node_id;
    if (render_camera_changed)
        h.last_render_camera_node_id = renderCamera.node_id;
    const bool render_camera_cut =
        h.temporal_camera_pose_valid &&
        VolumetricCloudViewCutDetected(
            Inverse(h.prev_vp_nojit), h.prev_temporal_camera_eye,
            Inverse(vp_nojit), eye);
    if (render_camera_changed || camera_view_history_reset ||
        render_projection_changed || render_camera_cut) {
        // The physical swapchain is shared, but temporal state must never
        // bleed across logical camera owners. Motion and every screen-space
        // history start cold. Abrupt same-owner teleports, orientation/FOV
        // cuts, and returning the surface to Scene View are covered too.
        InvalidateTemporalRenderHistories(h);
    }
    h.profiler_work.runtime_scene_camera =
        h.game_view && renderCamera.authored;
    h.profiler_work.active_camera_node_id =
        h.game_view && renderCamera.authored
            ? renderCamera.node_id : -1;
    // TAA: 透視時のみ Halton サブピクセルジッタを毎フレーム与える (color+depth+normal+SS が同じ vp で整合)。
    // TAA history reproject だけは «jitter 無し» の vp/prev で行う (jitter が reproject を汚さないように)。
    // 雲は no-jitter ray + 独自 TSR で resolve し、後段で ResolvedDepth の alpha を
    // reactive mask として渡す。したがって global TAA はジオメトリへ常時使いつつ、
    // 雲画素だけ current 100% にできる (二重 temporal history / ghost を回避)。
    FMat4 vp = vp_nojit;
    const bool taaOn =
        h.q_taa_on && !renderOrtho &&
        h.width > 0 && h.height > 0;
    if (taaOn) {
        const u32 fi = (h.taa_frame % 16u) + 1u;         // Halton は i>=1
        const float jx = (HaltonSeq(fi, 2u) - 0.5f) * 2.0f / static_cast<float>(h.width);
        const float jy = (HaltonSeq(fi, 3u) - 0.5f) * 2.0f / static_cast<float>(h.height);
        vp = ApplyTaaJitter(vp_nojit, jx, jy);
    }
    BuildSceneMeshVisibility(h, all3d, vp_nojit);

    const editor_subsurface_visibility::FPresence ssss_presence =
        InspectOpaqueSsssMaterials(h, all3d);
    const bool scene_has_ssss = ssss_presence.scene_has_material;
    const bool ssss_runtime_ready = AdvanceRuntimeSsss(
        h, scW, scH,
        scene_has_ssss && dvCount > 0u &&
        h.pbr3d_ready &&
        h.pbr3d.HasSubsurfaceMrtPipeline());
    // Keep shaders and all full-resolution targets warm scene-wide so an SSSS
    // object entering the camera never falls back for allocation frames.
    // The recurring 4-MRT + two-pass workload is gated independently by the
    // exact main-view mask built above.
    const bool ssss_resources_ready =
        ssss_runtime_ready &&
        h.post3d_ready && h.blit_ready &&
        h.ssao_pipe_ready &&
        EnsureSsssFrameResources(h, scW, scH);
    const bool ssss_frame_resources =
        ssss_presence.main_view_has_material &&
        ssss_resources_ready;
    const FPbrFrameDrawCounts pbr_draw_counts =
        CountPbrFrameDraws(h, all3d);
    // One reset for the complete command-list frame. The reserve includes the
    // opaque pass and the later interactive-water fallback; neither pass may
    // rewind CBs already referenced by recorded draws. A reserve below the
    // complete-frame bound fails before scene RT0 recording begins.
    const bool pbr_pool_reserve_completed =
        !h.pbr3d_ready ||
        h.pbr3d.BeginFrame(
            pbr_draw_counts.RequiredObjectBuffers());
    const bool pbr_object_pool_ready =
        pbr_pool_reserve_completed ||
        (h.pbr3d_ready &&
         h.pbr3d.ObjectBufferCapacity() >=
             pbr_draw_counts.RequiredObjectBuffers());
    if (h.pbr3d_ready && !pbr_object_pool_ready) {
        // The aggregate editor VB is intentionally bounded and is not a
        // completeness fallback for arbitrarily large scenes. Fail before
        // recording scene RT0 rather than publishing an arbitrary mesh suffix.
        // No geometry was submitted, so a mask computed above must not be
        // published as if this were a completed culled frame.
        h.profiler_work.frustum_culling_enabled = false;
        h.profiler_work.frustum_tested = 0u;
        h.profiler_work.frustum_visible = 0u;
        h.profiler_work.frustum_culled = 0u;
        InvalidateTemporalRenderHistories(h);
        return;
    }

    // --- VXGI: 三角形を radiance volume へ voxelize (色のにじみ。Diligent compute、Ultra=q_ssgi_on)。
    //     Raw DX12 は StructuredBuffer SRV 未対応のため VxgiVoxelize 冒頭で明示的に graceful skip。
    //     volume は VxgiResolve で画面空間の間接光へ変換し、SetSsgi へ渡す。
    IRhiTexture* vxgiVol = nullptr;
    IRhiTexture* vxgiResolveTex = nullptr;   // VXGI resolve (cone trace) 結果 → SetSsgi へ
    IRhiTexture* apVol = nullptr;            // aerial-perspective premultiplied in-scatter
    IRhiTexture* apTransVol = nullptr;       // wavelength-dependent transmittance
    IRhiTexture* localFogVol = nullptr;      // this frame's perspective local-fog producer result only
    if (!renderOrtho && dvCount >= 3 && h.q_ssgi_on && h.q_vxgi_on) {
        vxgiVol = VxgiVoxelize(
            h, cl, dv, h.scene_mesh_revision,
            bbMin, bbMax);   // 既定OFF: VXGI(64³)は blocky。OFF時は vxgiVol=null → SetSsgi が滑らかな screen-space SSGI を使う
    }

    // --- SH9 環境光を «現在の空» に追従させて再計算 (時間帯プリセット切替・空色変更に反応) ---
    //     拡散 ambient (Sh9Irradiance) と鏡面 fallback (Sh9Radiance) の両方の元データ。背景 CSky と
    //     同じ色を使うので金属の映り込みが背景と整合する。空が変わらない限り再計算しない (sh9_dirty)。
    Pass_UpdateSh9(h);

    {
        editor_profiler::FCpuScope atmosphereScope(
            h.profiler_work.atmosphere_cpu_ms);
        FScopedRhiGpuTiming atmosphereGpuScope(
            cl, ERhiGpuTimingPass::Atmosphere);
        Pass_AtmosphereIbl(h, cl);
    }

    // --- シャドウパス (光源視点で深度を焼く)。出力 sh を本体パスが PCF/CSM 比較に使う ---
    const FShadowOut sh = Pass_Shadows(
        h, cl, dvCount, bbMin, bbMax,
        eye, cam, renderOrtho);

    // --- G-buffer (法線+深度) プリパス: SSAO / SSR / SSGI の共通入力。いずれか ON で駆動 ---
    //     法線プリパスは depth も焼くが、直後の本パスが depth を clear して描き直すので干渉しない。
    //     SSAO はここで消費、SSR / SSGI は本パス後 (scene color が要るため) に消費する。
    h.ssao_computed = false;
    bool gbufReady = false;
    const bool wantGbuf =
        (h.q_ssao_on || h.q_ssr_on || h.q_ssgi_on ||
         ssss_frame_resources) &&
        h.ssao_pipe_ready && dvCount > 0;
    const bool wantsMotion = taaOn || h.q_ssr_on ||
                             h.q_ssgi_on || h.q_motionblur_on;
    if (wantGbuf || wantsMotion) {
        IRhiDevice* sdev = h.renderer.Device();
        if (sdev != nullptr) {
            const bool normal_size_changed =
                h.normal_w != scW || h.normal_h != scH;
            const bool ssao_size_changed =
                h.ssao_w != scW || h.ssao_h != scH;
            const bool ssr_size_changed =
                h.ssr_w != scW || h.ssr_h != scH;
            const bool hiz_size_changed =
                h.hiz3d_w != scW || h.hiz3d_h != scH;
            const bool ssgi_size_changed =
                h.ssgi_w != scW || h.ssgi_h != scH;
            const bool mv_size_changed =
                h.mv_w != scW || h.mv_h != scH;
            const bool missing_requested_effect =
                (h.q_ssao_on && !h.ssao_ready) ||
                (h.q_ssr_on && (!h.ssr_ready || !h.hiz3d_ready)) ||
                (h.q_ssgi_on && !h.ssgi_ready && !h.ssgi_init_tried) ||
                (wantsMotion && !h.mv_ready) ||
                (wantGbuf && h.normal_rt.Get() == nullptr);
            const bool resize_ready_effect =
                (h.ssao_ready && ssao_size_changed) ||
                (h.ssr_ready && ssr_size_changed) ||
                (h.hiz3d_ready && hiz_size_changed) ||
                (h.ssgi_ready && ssgi_size_changed) ||
                (h.mv_ready && mv_size_changed);
            if (missing_requested_effect || resize_ready_effect ||
                (wantGbuf && normal_size_changed)) {
                // SSR / SSGI の Resize は history RT を再生成する。新しい RT に Render
                // する前の main pass で旧 computed 状態を引き継がない。
                h.ssr_computed = false;
                h.ssgi_computed = false;
                if (mv_size_changed) h.mv_computed = false;
                if (wantGbuf &&
                    (normal_size_changed || h.normal_rt.Get() == nullptr)) {
                    FTextureDesc nd{}; nd.width = scW; nd.height = scH; nd.format = EFormat::R16G16B16A16_Float; nd.is_render_target = true;
                    auto ntr = CreateRhiTexture(*sdev, nd);
                    if (ntr.IsOk()) {
                        h.normal_rt = Move(ntr.Value());
                        h.normal_w = scW;
                        h.normal_h = scH;
                    } else {
                        ACS_LOG_ERROR("[3D] normal-buffer resize failed: %s",
                                      ntr.Error().message);
                        h.normal_rt.Reset();
                        h.normal_w = 0u;
                        h.normal_h = 0u;
                    }
                }
                if (h.ssao_ready && ssao_size_changed) {
                    const auto sr = h.ssao3d.Resize(scW, scH);
                    if (sr.IsOk()) {
                        h.ssao_w = scW;
                        h.ssao_h = scH;
                    } else {
                        ACS_LOG_ERROR("[3D] CSsao resize failed: %s",
                                      sr.Error().message);
                        h.ssao3d.Shutdown();
                        h.ssao_ready = false;
                        h.ssao_w = 0u;
                        h.ssao_h = 0u;
                    }
                }
                if (h.q_ssao_on && !h.ssao_ready) {
                    const auto sr = h.ssao3d.Init(*sdev, scW, scH);
                    h.ssao_ready = sr.IsOk();
                    if (h.ssao_ready) {
                        h.ssao_w = scW;
                        h.ssao_h = scH;
                    } else {
                        h.ssao_w = 0u;
                        h.ssao_h = 0u;
                        ACS_LOG_ERROR("[3D] CSsao Init failed: %s",
                                      sr.Error().message);
                    }
                }
                if (h.ssr_ready && ssr_size_changed) {
                    const auto rr = h.ssr3d.Resize(scW, scH);
                    if (rr.IsOk()) {
                        h.ssr_w = scW;
                        h.ssr_h = scH;
                    } else {
                        ACS_LOG_ERROR("[3D] CSsr resize failed: %s",
                                      rr.Error().message);
                        h.ssr3d.Shutdown();
                        h.ssr_ready = false;
                        h.ssr_w = 0u;
                        h.ssr_h = 0u;
                    }
                }
                if (h.q_ssr_on && !h.ssr_ready) {
                    const auto rr = h.ssr3d.Init(
                        *sdev, EFormat::R16G16B16A16_Float, scW, scH);
                    h.ssr_ready = rr.IsOk();
                    if (h.ssr_ready) {
                        h.ssr_w = scW;
                        h.ssr_h = scH;
                    } else {
                        h.ssr_w = 0u;
                        h.ssr_h = 0u;
                        ACS_LOG_ERROR("[3D] CSsr Init failed: %s",
                                      rr.Error().message);
                    }
                }
                if (h.hiz3d_ready && hiz_size_changed) {
                    const auto hr = h.hiz3d.Resize(scW, scH);
                    if (hr.IsOk()) {
                        h.hiz3d_w = scW;
                        h.hiz3d_h = scH;
                    } else {
                        ACS_LOG_WARN("[3D] CHiZ resize failed: %s",
                                     hr.Error().message);
                        h.hiz3d.Shutdown();
                        h.hiz3d_ready = false;
                        h.hiz3d_w = 0u;
                        h.hiz3d_h = 0u;
                    }
                }
                if (h.q_ssr_on && !h.hiz3d_ready) {
                    const auto hr = h.hiz3d.Init(*sdev, scW, scH);
                    h.hiz3d_ready = hr.IsOk();
                    if (h.hiz3d_ready) {
                        h.hiz3d_w = scW;
                        h.hiz3d_h = scH;
                    } else {
                        h.hiz3d_w = 0u;
                        h.hiz3d_h = 0u;
                        ACS_LOG_WARN(
                            "[3D] CHiZ Init failed (SSR DDA fallback): %s",
                            hr.Error().message);
                    }
                }
                if (h.ssgi_ready && ssgi_size_changed) {
                    const auto gr = h.ssgi3d.Resize(scW, scH);
                    if (gr.IsOk()) {
                        h.ssgi_w = scW;
                        h.ssgi_h = scH;
                    } else {
                        ACS_LOG_ERROR("[3D] CSsgi resize failed: %s",
                                      gr.Error().message);
                        h.ssgi3d.Shutdown();
                        h.ssgi_ready = false;
                        h.ssgi_init_tried = false;
                        h.ssgi_w = 0u;
                        h.ssgi_h = 0u;
                    }
                }
                // A missing raw-DX12 instance is compiled asynchronously by
                // AdvanceRuntimeSsgi on the next frame; never compile it here.
                if (h.mv_ready && mv_size_changed) {
                    const auto mr = h.mv3d.Resize(scW, scH);
                    if (mr.IsOk()) {
                        h.mv_w = scW;
                        h.mv_h = scH;
                    } else {
                        ACS_LOG_ERROR("[3D] CMotionVector resize failed: %s",
                                      mr.Error().message);
                        h.mv3d.Shutdown();
                        h.mv_ready = false;
                        h.mv_w = 0u;
                        h.mv_h = 0u;
                    }
                }
                if (wantsMotion && !h.mv_ready) {
                    const auto mr = h.mv3d.Init(*sdev, scW, scH);
                    h.mv_ready = mr.IsOk();
                    if (h.mv_ready) {
                        h.mv_w = scW;
                        h.mv_h = scH;
                    } else {
                        h.mv_w = 0u;
                        h.mv_h = 0u;
                        ACS_LOG_ERROR("[3D] CMotionVector Init failed: %s",
                                      mr.Error().message);
                    }
                }
            }
            if (wantGbuf && h.normal_rt &&
                h.normal_w == scW && h.normal_h == scH) {
                const FClearColor ncl{ 0.0f, 0.0f, 0.0f, 0.0f };   // 法線+深度プリパス (world normal を RGBA16F へ)
                cl->BeginRenderToTexture(*h.normal_rt, ncl, h.renderer.DepthBuffer(), 1.0f);
                { FViewport nvp{}; nvp.width = static_cast<f32>(scW); nvp.height = static_cast<f32>(scH); cl->SetViewport(nvp);
                  FScissorRect nsr{}; nsr.right = static_cast<i32>(scW); nsr.bottom = static_cast<i32>(scH); cl->SetScissor(nsr); }
                h.normal_cb->Update(&vp, sizeof(vp));   // NFrame.view_proj
                cl->SetPipeline(*h.normal_pipe);
                cl->SetConstantBuffer(0, *h.normal_cb);
                cl->SetVertexBuffer(*h.m3d_dyn_vb, sizeof(FM3DVtx));
                const editor_frustum_culling::FSubmissionMaskView
                    normal_submission_mask = SceneMeshSubmissionMask(
                        h,
                        editor_frustum_culling::ESceneGeometryPass::
                            NormalDepthPrepass);
                if (editor_frustum_culling::ShouldUseAggregateVertexDraw(
                        normal_submission_mask,
                        h.profiler_work.frustum_culled)) {
                    cl->Draw(dvCount, 0);
                } else {
                    editor_frustum_culling::ForEachSubmittedVertexRange(
                        normal_submission_mask, all3d.Num(),
                        [&](u32 node_index) noexcept {
                            return h.scene_mesh_vertex_offset[node_index];
                        },
                        [&](u32 node_index) noexcept {
                            return h.scene_mesh_vertex_count[node_index];
                        },
                        [&](u32 vertex_offset,
                            u32 vertex_count) noexcept {
                            cl->Draw(vertex_count, vertex_offset);
                        });
                }
                cl->EndRenderToTexture(*h.normal_rt);
                gbufReady = true;
            }
        }
    }
    if (gbufReady && h.q_ssao_on && h.ssao_ready) {       // SSAO (GTAO + 接地影) を焼く
        h.ssao3d.Render(*h.renderer.Device(), *cl, *h.renderer.DepthBuffer(), *h.normal_rt,
                        vp, Inverse(vp), cam.View(), eye, h.sun_dir,
                        h.q_ssao_intensity, h.q_ssao_radius);   // light_dir = surface→light = sun_dir
        h.ssao_computed = true;
    }

    // --- VXGI resolve: voxel volume を画面空間 cone trace → 間接光 (色のにじみ)。depth+normal+volume が揃った今 ---
    if (vxgiVol != nullptr && gbufReady && h.normal_rt) {
        vxgiResolveTex = VxgiResolve(h, cl, vxgiVol, h.renderer.DepthBuffer(), h.normal_rt.Get(),
                                     Inverse(vp), bbMin, bbMax, scW, scH, h.q_ssgi_intensity);
    }

    // --- 空気遠近法 + 局所高度フォグ: 48x48x96 のカメラ空間 froxel。
    //     q_fog_on のときは surface 単位の近似ではなく、camera→depth の 24-step 散乱積分を主経路にする。
    //     compute 不可なら下の CPbrShader 解析積分へ自動 fallback。
    // Cloud march と同じ範囲まで積分し、horizon cloud を 300-unit
    // far slice へ誤って clamp しない。Squared Z + 96 slices で近景精度も維持する。
    constexpr f32 kFogVolumeMaxDist = acs::kVolumetricCloudMaxDistance;
    if (!renderOrtho && (h.q_ap_on || h.q_fog_on)) {
        IRhiDevice* adev = h.renderer.Device();
        if (adev != nullptr) {
            if (!h.sky_atmo_tried) { h.sky_atmo_tried = true; (void)h.sky_atmo.Init(*adev); }
            if (h.sky_atmo.Ready()) {
                const f32 apSunRadiance =
                    PhysicalAtmosphereSunRadiance(h.sun_intensity) *
                    kAtmosScale;
                FVolumetricFogParams fog{};
                fog.color = h.sky_horizon;
                fog.density = h.q_fog_on ? h.q_fog_density : 0.0f;
                fog.height_falloff = h.q_fog_height_falloff;
                fog.height_base = 0.0f;
                fog.anisotropy = 0.42f;
                fog.sun_scatter = 0.18f;
                IRhiTexture* builtAp = nullptr;
                {
                    editor_profiler::FCpuScope atmosphereScope(
                        h.profiler_work.atmosphere_cpu_ms);
                    FScopedRhiGpuTiming atmosphereGpuScope(
                        cl, ERhiGpuTimingPass::Atmosphere);
                    builtAp = h.sky_atmo.BuildAerialPerspective(
                                *adev, *cl, Inverse(vp_nojit), eye, h.sun_dir,
                                FVec3{
                                    apSunRadiance * h.sun_color.x,
                                    apSunRadiance * h.sun_color.y,
                                    apSunRadiance * h.sun_color.z},
                                kFogVolumeMaxDist,
                                h.q_ap_on ? 0.001f : 0.0f /*scene metres→km*/,
                                std::fmax(0.0f, eye.y * 0.001f) /*cam_alt_km*/, fog);
                    localFogVol = h.sky_atmo.LocalFogVolume();
                }
                // Local fog owns a separate transfer volume.  Do not route its
                // scene_to_km=0 physical identity volume through either the
                // fullscreen RGB multiply or the cloud atmosphere shader:
                // an unavailable/unwritten identity UAV would otherwise turn
                // both geometry and clouds black instead of failing open.
                if (h.q_ap_on && builtAp != nullptr) {
                    apVol = builtAp;
                    apTransVol = h.sky_atmo.ApTransmittanceVolume();
                }
                if (apTransVol == nullptr) {
                    apVol = nullptr;
                }
            }
        }
    }

    // --- CMotionVector: motion G-buffer を per-node (主パスと同型) で焼き TAA/SSR/SSGI の reproject に供給 ---
    //     no-jitter の vp/prev で «真の幾何モーション» (カメラ + オブジェクト両方) を出す。静止物は motion≈カメラ分、
    //     動く物 (play/ドラッグ) はその差分も入り ghost が消える。TAA/SSR/SSGI が ON のときだけ焼く。
    const bool motionHistoryReady = h.mv_computed;
    h.mv_computed = false;
    auto invalidateMotionHistory = [&]() noexcept {
        for (u32 i = 0; i < all3d.Num(); ++i) {
            if (AEditor3DRecordComponent* record = Rec3D(all3d[i])) {
                record->prev_world_valid = false;
            }
        }
    };
    auto motionMeshForNode = [&](game::ANode* node) noexcept -> FGpuMesh* {
        AEditor3DRecordComponent* record = Rec3D(node);
        if (!IsEffectivelyVisibleAndEnabled(node) ||
            (record != nullptr && record->is_empty)) {
            return nullptr;
        }
        game::AMeshComponent3D* meshComponent = Mesh3D(node);
        if (meshComponent == nullptr ||
            meshComponent->RenderHandle() != nullptr ||
            IsRenderedByWater3D(h, node)) {
            return nullptr;
        }
        FGpuMesh* mesh = GpuMeshForNode3D(h, node);
        if (mesh == nullptr || mesh->vertex_buffer.Get() == nullptr ||
            mesh->index_buffer.Get() == nullptr) {
            return nullptr;
        }
        return mesh;
    };

    // Reserve the exact visible set before opening the render pass. This keeps
    // object-CB growth out of command recording and removes the old silent
    // 256-draw truncation for large scenes.
    u32 motionEligibleCount = 0;
    const bool canDrawMotion = wantsMotion && h.mv_ready && h.pbr3d_ready;
    if (canDrawMotion) {
        const editor_frustum_culling::FSubmissionMaskView submission_mask =
            SceneMeshSubmissionMask(
                h,
                editor_frustum_culling::ESceneGeometryPass::MotionVectors);
        for (u32 i = 0; i < all3d.Num(); ++i) {
            if (!submission_mask.ShouldSubmit(i)) {
                if (AEditor3DRecordComponent* record =
                        Rec3D(all3d[i])) {
                    record->prev_world_valid = false;
                }
                continue;
            }
            if (motionMeshForNode(all3d[i]) != nullptr) {
                ++motionEligibleCount;
            } else if (AEditor3DRecordComponent* record = Rec3D(all3d[i])) {
                record->prev_world_valid = false;
            }
        }
    }
    const bool motionDrawRequested =
        canDrawMotion && motionEligibleCount > 0u;
    if (motionDrawRequested) {
        if (!motionHistoryReady) invalidateMotionHistory();
        const FMat4& previousVp =
            motionHistoryReady ? h.prev_vp_nojit : vp_nojit;
        if (h.mv3d.BeginFrame(motionEligibleCount) &&
            h.mv3d.Begin(*cl, vp_nojit, previousVp)) {
            bool motionComplete = true;
            editor_frustum_culling::ForEachSubmittedNode(
                SceneMeshSubmissionMask(
                    h,
                    editor_frustum_culling::ESceneGeometryPass::MotionVectors),
                all3d.Num(),
                [&](u32 i) noexcept {
                game::ANode* nn = all3d[i];
                FGpuMesh* gm = motionMeshForNode(nn);
                if (gm == nullptr) return;

                AEditor3DRecordComponent* er = Rec3D(nn);
                const FMat4 world = SceneMeshWorldTransform(h, i, nn).ToMat4();
                const FMat4 prevW =
                    (motionHistoryReady && er != nullptr &&
                     er->prev_world_valid)
                        ? er->prev_world
                        : world;
                const bool recorded =
                    h.mv3d.DrawMesh(*cl, *gm, world, prevW);
                motionComplete = motionComplete && recorded;
                if (recorded && er != nullptr) {
                    er->prev_world = world;
                    er->prev_world_valid = true;
                } else if (er != nullptr) {
                    er->prev_world_valid = false;
                }
            });
            h.mv3d.End(*cl);
            h.mv_computed =
                motionComplete &&
                h.mv3d.ObjectDrawCount() == motionEligibleCount;
            if (!h.mv_computed) invalidateMotionHistory();
        } else {
            // A rejected pool reservation or MRT bind must never leave object
            // transforms looking like contiguous history. The next successful
            // frame is a zero-motion cold start for camera and objects.
            invalidateMotionHistory();
        }
    } else if (motionHistoryReady) {
        // Motion was produced last frame but has no consumer/drawable input
        // now. Invalidate once on that transition; remaining disabled frames
        // keep the global h.mv_computed=false gate without an O(N) walk.
        invalidateMotionHistory();
    }

    // 3D シーンを «線形 HDR RT» へ描く → 末尾で CPostProcess(ACES) が一度だけ tonemap。post3d 遅延初期化。
    IRhiDevice* pdev = h.renderer.Device();
    if (pdev != nullptr && h.post3d_ready &&
        (h.post3d_w != scW || h.post3d_h != scH)) {
        const auto resize_result = h.post3d.Resize(scW, scH);
        if (resize_result.IsOk()) {
            h.post3d_w = scW;
            h.post3d_h = scH;
        } else {
            ACS_LOG_ERROR(
                "[3D] CPostProcess resize failed: %s",
                resize_result.Error().message);
        }
    }
    IRhiTexture*   hdrRt  = h.post3d_ready ? h.post3d.HdrRenderTarget() : nullptr;
    IRhiSwapchain* scSwap = h.renderer.Swapchain();
    IRhiTexture* localFogSceneDepth = h.renderer.DepthBuffer();
    const bool canCompositeLocalFog =
        editor_render_policy::ShouldCompositeLocalFog(
            renderOrtho, h.q_fog_on,
            localFogVol != nullptr, localFogSceneDepth != nullptr,
            hdrRt != nullptr && scSwap != nullptr);

    // --- Volumetric clouds: render pass の «外» で雲を compute レイマーチ (UAV へ書く)。
    //     composite は AP 後まで遅延する。CSky の 2D 雲は無効化して二重描画回避。
    bool cloudsActive = false;
    if (h.q_cloud_coverage > 0.001f && !renderOrtho && hdrRt != nullptr) {
        IRhiDevice* cdev = h.renderer.Device();
        if (cdev != nullptr) {
            if (!h.vclouds_tried) { h.vclouds_tried = true;
                h.vclouds_ready = h.vclouds3d.Init(*cdev, EFormat::R16G16B16A16_Float).IsOk();
                if (!h.vclouds_ready) ACS_LOG_WARN("[3D] CVolumetricClouds Init 失敗 (CSky 2D 雲にフォールバック)"); }
            // 雲は «空 pass を End → compute dispatch (pass 外で UAV→SRV 遷移が成立)» する。
            // compute をここ (pass 前) で dispatch すると、後段 composite の SRV 読みへの
            // UAV→SRV 遷移が pass を跨げず書込みが見えない (実測)。よってここでは gate のみ。
            if (h.vclouds_ready &&
                h.vclouds3d.EnsureSize(*cdev, scW, scH, h.q_cloud_render_scale)) {
                cloudsActive = true;
            }
        }
    }
    h.profiler_work.clouds_active = cloudsActive;

    if (hdrRt != nullptr)        cl->BeginRenderToTexture(*hdrRt, h.clear_color, h.renderer.DepthBuffer(), 1.0f);
    else if (scSwap != nullptr)  cl->BeginRenderToSwapchain(*scSwap, h.renderer.CurrentBuffer(), h.clear_color, h.renderer.DepthBuffer(), 1.0f);
    { FViewport rvp{}; rvp.width = static_cast<f32>(scW); rvp.height = static_cast<f32>(scH); cl->SetViewport(rvp);
      FScissorRect rsr{}; rsr.right = static_cast<i32>(scW); rsr.bottom = static_cast<i32>(scH); cl->SetScissor(rsr); }

    // --- (1) スカイ: 物理大気モード(q_sky_mode==1 + IBL 焼成済)は env cubemap を skybox 描画して «背景=IBL=一致»。
    //         それ以外はエンジン標準 CSky (グラデ + 手続き雲)。CSky Init 失敗時のみ自前 kSky3DHLSL。
    {
    editor_profiler::FCpuScope atmosphereScope(
        h.profiler_work.atmosphere_cpu_ms);
    FScopedRhiGpuTiming atmosphereGpuScope(
        cl, ERhiGpuTimingPass::Atmosphere);
    if (h.q_sky_mode == 1 && h.ibl_ready) {
        IRhiDevice* sdev = h.renderer.Device();
        if (sdev != nullptr) {
            const EFormat skyRt = hdrRt ? EFormat::R16G16B16A16_Float : h.renderer.ColorFormat();
            const FVec3 sunDiscRadiance{
                h.sun_color.x * h.sun_intensity *
                    kPhysicalSunDiscRadianceScale,
                h.sun_color.y * h.sun_intensity *
                    kPhysicalSunDiscRadianceScale,
                h.sun_color.z * h.sun_intensity *
                    kPhysicalSunDiscRadianceScale};
            h.ibl3d.DrawEnvSkybox(
                *sdev, *cl, vp, eye, skyRt, h.renderer.DepthFormat(),
                h.sun_dir, sunDiscRadiance, kPhysicalSunAngularRadius);
        }
    } else if (h.sky3d_ready) {
        h.sky3d.SetSunDirection(h.sun_dir);   // 空の太陽もシーンのライト方向に追従 (設定駆動)
        h.sky3d.SetSunColor(h.sun_color);     // 太陽の色も追従
        h.sky3d.SetZenithColor(h.sky_zenith); // 空グラデも設定駆動 → IBL 環境光と背景を一致
        h.sky3d.SetHorizonColor(h.sky_horizon);
        h.sky3d.SetGroundColor(h.sky_ground);
        h.sky3d.SetCloudsEnabled(
            h.q_cloud_coverage > 0.001f &&
            !renderOrtho && !cloudsActive);   // Ortho と volumetric 雲では 48-step CSky fallback を止める
        h.sky3d.SetClouds(h.q_cloud_coverage, h.q_cloud_density);
        h.sky3d.SetCloudWind(h.q_cloud_wind);
        h.sky3d.Render(*cl, cam);
    } else if (h.sky_pipe && h.sky_cb) {
        FSkyCb sk{};
        sk.inv_view_proj = Inverse(vp);
        sk.zenith = FVec4{ h.sky_zenith.x,  h.sky_zenith.y,  h.sky_zenith.z,  0 };   // 天頂 (設定駆動)
        sk.horizon= FVec4{ h.sky_horizon.x, h.sky_horizon.y, h.sky_horizon.z, 0 };   // 地平
        sk.ground = FVec4{ h.sky_ground.x,  h.sky_ground.y,  h.sky_ground.z,  0 };   // 下半球
        sk.sun    = FVec4{ h.sun_dir.x, h.sun_dir.y, h.sun_dir.z, 0 };   // 太陽方向 (world)。シーンのライト方向と一致。
        h.sky_cb->Update(&sk, sizeof(sk));
        cl->SetPipeline(*h.sky_pipe);
        cl->SetConstantBuffer(0, *h.sky_cb);
        cl->Draw(3, 0);
    }
    }

    // --- Volumetric clouds: 空 pass を一旦 End → 雲 compute を pass «外» で dispatch (UAV→SRV
    //     遷移が成立) → depth 付き load pass を再開してシーンメッシュを描く。雲の合成は AP 後まで
    //     遅延し、完成した scene depth で実ジオメトリを除外する。
    if (cloudsActive && hdrRt != nullptr) {
        cl->EndRenderToTexture(*hdrRt);
        // acs_editor_render() が実測 dt を積算した host time を使う。
        // 固定 1/60 加算だと 30/120 Hz で雲の移流速度が半分/2倍になる。
        h.vclouds_time = h.time;
        h.vclouds3d.SetLayer(acs::FVolumetricCloudLayer{
            h.q_cloud_base, h.q_cloud_top, h.q_cloud_noise_scale
        });
        const FVec3 sunC{ h.sun_color.x * h.sun_intensity, h.sun_color.y * h.sun_intensity, h.sun_color.z * h.sun_intensity };
        {
            editor_profiler::FCpuScope cloudScope(
                h.profiler_work.cloud_cpu_ms);
            FScopedRhiGpuTiming cloudGpuScope(
                cl, ERhiGpuTimingPass::Cloud);
            h.vclouds3d.RenderCompute(
                *cl, Inverse(vp_nojit), eye, h.sun_dir, sunC, h.sky_horizon,
                h.q_cloud_coverage, h.q_cloud_density, h.q_cloud_wind,
                h.vclouds_time);
        }
        cl->BeginRenderToTextureLoad(*hdrRt, h.renderer.DepthBuffer());
        { FViewport rvp2{}; rvp2.width = static_cast<f32>(scW); rvp2.height = static_cast<f32>(scH); cl->SetViewport(rvp2);
          FScissorRect rsr2{}; rsr2.right = static_cast<i32>(scW); rsr2.bottom = static_cast<i32>(scH); cl->SetScissor(rsr2); }
    }

    // PBR 視線ベクトル用のカメラ位置。正射(ortho)は視線が平行なので、視軸上の «遠点» を渡して
    // V = normalize(camPos - wpos) を画面全体でほぼ平行にする (透視は実 eye)。
    FVec3 camPos = eye;
    if (renderOrtho) {
        camPos = eye - renderCamera.forward * 1000.0f;
    }
    // フレーム CB (view_proj + 光 + 環境光 + カメラ位置)。
    FM3DFrame fcb{};
    fcb.view_proj = vp;
    const FVec3 sunCol{ h.sun_color.x * h.sun_intensity, h.sun_color.y * h.sun_intensity, h.sun_color.z * h.sun_intensity };
    fcb.light_dir = FVec4{ h.sun_dir.x, h.sun_dir.y, h.sun_dir.z, 0.0f };    // 光方向, w=0 (環境光は IBL から取る)
    fcb.light_col = FVec4{ sunCol.x, sunCol.y, sunCol.z, 0.0f };     // 太陽 = キーライト (色×強度、設定駆動)
    fcb.cam_pos   = FVec4{ camPos.x, camPos.y, camPos.z, sh.shadowOn ? 1.0f : 0.0f };   // w=影を受けるか
    fcb.sky_zenith  = FVec4{ h.sky_zenith.x,  h.sky_zenith.y,  h.sky_zenith.z,  0 };   // IBL 環境光源 (スカイと同じグラデ・設定駆動)
    fcb.sky_horizon = FVec4{ h.sky_horizon.x, h.sky_horizon.y, h.sky_horizon.z, 0 };
    fcb.sky_ground  = FVec4{ h.sky_ground.x,  h.sky_ground.y,  h.sky_ground.z,  0 };
    fcb.light_vp    = sh.lightVp;                           // シャドウマップ空間
    if (h.m3d_frame_cb) h.m3d_frame_cb->Update(&fcb, sizeof(fcb));

    // Preserve the already-rendered sky in RT0 and opaque depth, while
    // clearing only the two dedicated SSSS targets. RT3 loads the geometric
    // normal prepass and replaces covered PBR pixels with the final
    // normal-map/Substrate world normal. Non-SSS frames never bind MRTs and
    // retain the exact established command/resource cost.
    const bool ssss_mrt_active =
        ssss_frame_resources && gbufReady && hdrRt != nullptr &&
        h.ssss_diffuse_rt && h.ssss_material_rt &&
        h.ssss3d_ready && pbr_object_pool_ready;
    IRhiTexture* ssss_targets[4] = {
        hdrRt, h.ssss_diffuse_rt.Get(), h.ssss_material_rt.Get(),
        h.normal_rt.Get()
    };
    bool ssss_mrt_bound = false;
    bool ssss_mrt_draws_valid = true;
    if (ssss_mrt_active) {
        cl->EndRenderToTexture(*hdrRt);
        ssss_mrt_bound = cl->BeginRenderToTextureMrtLoad(
            ssss_targets, 4u, FClearColor{0, 0, 0, 0},
            (1u << 1u) | (1u << 2u),
            h.renderer.DepthBuffer(), false, 1.0f);
        if (!ssss_mrt_bound) {
            // Resume the established single-target PBR path. No MRT draw or
            // EndMrt is legal after a rejected bind.
            cl->BeginRenderToTextureLoad(
                *hdrRt, h.renderer.DepthBuffer());
        }
        FViewport ssss_viewport{};
        ssss_viewport.width = static_cast<f32>(scW);
        ssss_viewport.height = static_cast<f32>(scH);
        cl->SetViewport(ssss_viewport);
        FScissorRect ssss_scissor{};
        ssss_scissor.right = static_cast<i32>(scW);
        ssss_scissor.bottom = static_cast<i32>(scH);
        cl->SetScissor(ssss_scissor);
    }

    // --- (2) ノードのメッシュ本体: エンジン標準 CPbrShader (HDR 線形出力)。per-node DrawMesh で
    //     model + 材質(metallic/roughness/baseColor + Substrate ロブ + emissive)+ キャスト影。
    //     DrawMesh が «object CB リングの現在バッファ» を毎draw bind するので per-object が正しく出る。
    auto draw_aggregate_mesh_fallback = [&]() noexcept {
        // This path draws one aggregate buffer and therefore cannot consume
        // the exact per-node mask. Do not publish misleading culling counts.
        h.profiler_work.frustum_culling_enabled = false;
        h.profiler_work.frustum_tested = 0u;
        h.profiler_work.frustum_visible = 0u;
        h.profiler_work.frustum_culled = 0u;
        if (dvCount == 0u || !h.m3d_dyn_vb || !h.m3d_pipe ||
            !h.m3d_frame_cb) {
            return;
        }
        cl->SetPipeline(*h.m3d_pipe);
        cl->SetConstantBuffer(0, *h.m3d_frame_cb);
        if (h.shadow.DepthTexture() != nullptr)
            cl->SetTexture(0, *h.shadow.DepthTexture());
        cl->SetVertexBuffer(*h.m3d_dyn_vb, sizeof(FM3DVtx));
        cl->Draw(dvCount, 0);
    };
    {
    editor_profiler::FCpuScope opaqueScope(
        h.profiler_work.opaque_cpu_ms);
    FScopedRhiGpuTiming opaqueGpuScope(
        cl, ERhiGpuTimingPass::Opaque);
    if (h.pbr3d_ready && pbr_object_pool_ready) {
        // 3点ライティング: 主光(暖・強)+ 補助光(寒・弱、影側を持ち上げ立体感)+ リム(背面・輪郭)。
        // 1灯+SH9 だと陰が埋まりのっぺりするため、補助/リムで «面の向き» が読めるようにする。
        FDirLight dl[3];
        dl[0].direction = h.sun_dir; dl[0].color = sunCol;  // key (太陽。BRDF/影/空と同じ surface→light 方向)
        dl[1].direction = FVec3{  0.55f, -0.28f, -0.50f }; dl[1].color = FVec3{ 0.40f, 0.48f, 0.62f };  // fill (寒、弱)
        dl[2].direction = FVec3{  0.05f,  0.35f, -0.95f }; dl[2].color = FVec3{ 0.45f, 0.45f, 0.50f };  // rim (背面、輪郭)
        h.pbr3d.SetLights(vp, camPos, dl, 3, FVec3{ 0.22f, 0.24f, 0.30f });   // ambient はやや控えめ(陰のコントラスト確保)
        if (sh.shadowOn) {
            // 品質ノブを配線 (従来は bias 0.0015 固定 + texel_size に 1.0 を渡す «バグ» で PCF が機能不全だった)。
            const u32 ssz = (h.shadow.Size() > 0) ? h.shadow.Size() : 2048u;
            const f32 texel = 1.0f / static_cast<f32>(ssz);
            if (sh.csmActive)   // CSM: 各 cascade の VP + 分割閾値で。HLSL が view-space z で cascade を選択。
                h.pbr3d.SetShadowMapCascades(h.shadow.DepthTexture(), sh.csmVps, sh.csmSplits, h.shadow.CascadeCount(),
                                             h.q_shadow_bias, texel, h.q_shadow_filter);
            else
                h.pbr3d.SetShadowMap(h.shadow.DepthTexture(), sh.lightVp,
                                     h.q_shadow_bias, texel, h.q_shadow_filter);
        }
        else          h.pbr3d.SetShadowMap(nullptr, sh.lightVp);
        // SSAO (GTAO) visibility を ambient へ乗算 (今フレーム焼けていれば)。screen UV でサンプル。
        if (h.ssao_computed) h.pbr3d.SetSsao(h.ssao3d.OutputTexture(), h.q_ssao_intensity, scW, scH);
        else                 h.pbr3d.SetSsao(nullptr, 0.0f, scW, scH);
        // SSR: 前フレームの反射結果を roughness/Fresnel でブレンド (CPbrShader 内)。本フレーム分は本パス後に焼く。
        // Init / resize / re-enable 後の最初の main pass では history RT はまだ未描画。
        // ssr_ready だけで bind すると未初期化 HDR を反射として読むため、直近の
        // Render が完了したことも必須にする。本フレーム分は main pass 後に焼かれ、
        // 次フレームから安全に利用できる。
        if (h.q_ssr_on && h.ssr_ready && h.ssr_computed)
            h.pbr3d.SetSsr(h.ssr3d.OutputTexture(), h.q_ssr_intensity);
        else
            h.pbr3d.SetSsr(nullptr, 0.0f);
        // 間接光を ambient に加算。VXGI (voxel GI、色のにじみ) が焼けていれば «今フレーム» の VXGI を優先、
        // 無ければ前フレームの screen-space SSGI。SetSsgi スロットを共用 (intensity は resolve で適用済み→1.0)。
        if (vxgiResolveTex != nullptr)        h.pbr3d.SetSsgi(vxgiResolveTex, 1.0f);
        else if (h.q_ssgi_on && h.ssgi_ready && h.ssgi_computed)
            h.pbr3d.SetSsgi(h.ssgi3d.OutputTexture(), h.q_ssgi_intensity);
        else                                  h.pbr3d.SetSsgi(nullptr, 0.0f);
        // IBL: env から焼いた irradiance(拡散) + prefilter(鏡面) + BRDF-LUT。成功時は SH9 を切り cubemap 拡散を使う。
        if (h.ibl_ready) {
            h.pbr3d.SetIbl(h.ibl3d.IrradianceMap(), h.ibl3d.PrefilterMap(), h.ibl3d.BrdfLut(), h.ibl3d.PrefilterMips());
            h.pbr3d.SetSh9(nullptr);   // IBL 有効 → irradiance cubemap を拡散に (init の muted SH9 を無効化)
        }   // ibl_ready=false の間は init で焼いた SH9 がそのまま効く (フォールバック)
        // local fog は perspective camera-volume が主経路。compute 不可時だけ
        // 同じ perspective domain で解析積分 height fog へ fallback。
        if (editor_render_policy::ShouldUseAnalyticLocalFog(
                renderOrtho, h.q_fog_on,
                canCompositeLocalFog))
            h.pbr3d.SetFog(h.sky_horizon, h.q_fog_density, h.q_fog_height_falloff,
                           0.0f, 0.42f, 0.18f);
        else            h.pbr3d.SetFog(FVec3{ 0, 0, 0 }, 0.0f, 0.0f, 0.0f);
        // camera-volume は全 opaque/transmission/sky/cloud を含む fullscreen pass で一度だけ適用する。
        // compute 不可時の解析 fog だけは上の SetFog で PBR surface に残す。
        h.pbr3d.SetAerialPerspective(nullptr, kFogVolumeMaxDist);
        editor_frustum_culling::ForEachSubmittedNode(
            SceneMeshSubmissionMask(
                h,
                editor_frustum_culling::ESceneGeometryPass::PbrOpaqueDraw),
            all3d.Num(),
            [&](u32 node_index) noexcept {
                ssss_mrt_draws_valid = DrawEditorPbrNode(h, *cl, all3d[node_index], SceneMeshWorldTransform(h, node_index, all3d[node_index]), ssss_mrt_bound) && ssss_mrt_draws_valid;
            });
    } else {   // フォールバック: 自前 kMesh3DHLSL (CPbrShader 不可時)
        draw_aggregate_mesh_fallback();
    }
    }
    if (!ssss_mrt_bound && !ssss_mrt_draws_valid) {
        // A late base-PBR validation failure must not punch holes into RT0.
        // The aggregate path shares the same depth and fills the complete
        // authored opaque set without consuming additional object CBs.
        draw_aggregate_mesh_fallback();
    }

    // Resolve diffuse-only SSSS immediately after opaque lighting. The
    // resulting complete HDR scene is copied back before grid, sprites,
    // water, transmission, clouds and atmosphere, so those layers stay sharp.
    if (ssss_mrt_bound) {
        // SSSS is part of opaque lighting even though its fullscreen resolve
        // follows the mesh scope. A second same-category segment is accumulated
        // by both profiler backends, and exists only when work is recorded.
        editor_profiler::FCpuScope ssssOpaqueScope(
            h.profiler_work.opaque_cpu_ms);
        FScopedRhiGpuTiming ssssOpaqueGpuScope(
            cl, ERhiGpuTimingPass::Opaque);
        cl->EndRenderToTextureMrt(ssss_targets, 4u);
        if (!ssss_mrt_draws_valid) {
            // MRT auxiliary data is unusable, and the failed object may not
            // have written RT0/depth. Recover the complete scene through the
            // independent aggregate pipeline while preserving sky/background.
            cl->BeginRenderToTextureLoad(
                *hdrRt, h.renderer.DepthBuffer());
            FViewport fallback_viewport{};
            fallback_viewport.width = static_cast<f32>(scW);
            fallback_viewport.height = static_cast<f32>(scH);
            cl->SetViewport(fallback_viewport);
            FScissorRect fallback_scissor{};
            fallback_scissor.right = static_cast<i32>(scW);
            fallback_scissor.bottom = static_cast<i32>(scH);
            cl->SetScissor(fallback_scissor);
            draw_aggregate_mesh_fallback();
            cl->EndRenderToTexture(*hdrRt);
        }

        FSubsurfaceScatteringParams ssss_params{};
        // These values are compatibility fallbacks only. PSMainSsss writes
        // per-material RGB world radii into the RGBA16F material target.
        ssss_params.radius_world = 0.012f;
        ssss_params.channel_radius = FVec3{1.0f, 0.55f, 0.25f};
        ssss_params.strength = 1.0f;
        ssss_params.depth_sigma = 0.001f;
        ssss_params.normal_power = 24.0f;
        ssss_params.max_radius_pixels = 64.0f;
        // Do not consume partially populated auxiliary buffers. The aggregate
        // recovery above has already restored a complete RT0 when needed.
        const bool ssss_rendered =
            ssss_mrt_draws_valid &&
            h.ssss3d.Render(
                *cl, *hdrRt, *h.ssss_diffuse_rt,
                *h.renderer.DepthBuffer(), *h.normal_rt,
                *h.ssss_material_rt, Inverse(vp), ssss_params);
        if (ssss_rendered && h.ssss3d.OutputTexture() != nullptr) {
            cl->BeginRenderToTexture(
                *hdrRt, FClearColor{0, 0, 0, 0}, nullptr, 1.0f);
            FViewport copy_viewport{};
            copy_viewport.width = static_cast<f32>(scW);
            copy_viewport.height = static_cast<f32>(scH);
            cl->SetViewport(copy_viewport);
            FScissorRect copy_scissor{};
            copy_scissor.right = static_cast<i32>(scW);
            copy_scissor.bottom = static_cast<i32>(scH);
            cl->SetScissor(copy_scissor);
            cl->SetPipeline(*h.blit_pipe);
            cl->SetTexture(0, *h.ssss3d.OutputTexture());
            cl->Draw(3, 0);
            cl->EndRenderToTexture(*hdrRt);
        }

        // Even a runtime pass failure leaves RT0 as valid unblurred full-lit
        // HDR, so reopening it is the complete graceful fallback.
        cl->BeginRenderToTextureLoad(
            *hdrRt, h.renderer.DepthBuffer());
        FViewport resume_viewport{};
        resume_viewport.width = static_cast<f32>(scW);
        resume_viewport.height = static_cast<f32>(scH);
        cl->SetViewport(resume_viewport);
        FScissorRect resume_scissor{};
        resume_scissor.right = static_cast<i32>(scW);
        resume_scissor.bottom = static_cast<i32>(scH);
        cl->SetScissor(resume_scissor);
    }
    // --- (2b) 無限グリッド: 視点中心の大クアッド (y=0 / ortho は z=0) を半透明描画。距離フェードで無限に見せる。
    if (!h.game_view && h.show_grid3d && h.grid_pipe && h.grid_vb && h.grid_cb) {
        const f32 S = 800.0f;                    // クアッド半径 (フェード距離より十分大きく)
        const f32 cx = h.cam3d_target.x;
        const f32 cy = h.ortho3d ? h.cam3d_target.y : h.cam3d_target.z;
        auto V = [](f32 x, f32 y, f32 z) { return FM3DVtx{ x, y, z, 0, 1, 0, 0, 0, 0, 0, 0.5f }; };
        FM3DVtx qv[6];
        if (h.ortho3d) {                         // z=0 平面 (XY)
            qv[0]=V(cx-S,cy-S,0); qv[1]=V(cx+S,cy-S,0); qv[2]=V(cx+S,cy+S,0);
            qv[3]=V(cx-S,cy-S,0); qv[4]=V(cx+S,cy+S,0); qv[5]=V(cx-S,cy+S,0);
        } else {                                 // y=0 平面 (XZ)
            qv[0]=V(cx-S,0,cy-S); qv[1]=V(cx+S,0,cy-S); qv[2]=V(cx+S,0,cy+S);
            qv[3]=V(cx-S,0,cy-S); qv[4]=V(cx+S,0,cy+S); qv[5]=V(cx-S,0,cy+S);
        }
        FGridCb gcb{}; gcb.view_proj = vp;
        gcb.gctr = FVec4{ cx, cy, 140.0f, h.ortho3d ? 1.0f : 0.0f };   // 中心 + フェード距離 + 平面フラグ
        h.grid_cb->Update(&gcb, sizeof(gcb));
        h.grid_vb->Update(qv, sizeof(qv));
        cl->SetPipeline(*h.grid_pipe);
        cl->SetConstantBuffer(0, *h.grid_cb);
        cl->SetVertexBuffer(*h.grid_vb, sizeof(FM3DVtx));
        cl->Draw(6, 0);
    }

    // --- (2.5) スプライト (テクスチャ付きクアッド): RenderHandle にテクスチャを持つノードを別パスで。
    //          ローカル XY 単位クアッド (z=0) を World 行列で変換。テクスチャ毎に SetTexture → Draw(6)。
    if (h.spr_pipe && h.spr_vb && h.m3d_frame_cb) {
        TArray<FSprVtx>& sv = h.sprite_vertices;
        TArray<IRhiTexture*>& stex = h.sprite_draw_textures;
        sv.Reset();
        stex.Reset();
        constexpr u32 kMaxSpr = 1024u;
        constexpr u32 kVerticesPerSprite = 6u;
        if (sv.Max() < kMaxSpr * kVerticesPerSprite)
            sv.Reserve(kMaxSpr * kVerticesPerSprite);
        if (stex.Max() < kMaxSpr) stex.Reserve(kMaxSpr);
        // ローカルクアッドの 4 隅 (中心原点・1x1)。Ortho 2D ビュー (カメラ +Z→原点) では
        // world +X が «画面左» に写る (ギズモ赤 X 軸で実測)。画像が元の見た目どおり (左右非反転・
        // 上下正立) になるよう、画像左端 U=0 を world +X 側、V=0 を world +Y 側に割り当てる。
        const FVec3 lTL{ -0.5f,  0.5f, 0 }, lTR{ 0.5f,  0.5f, 0 };
        const FVec3 lBL{ -0.5f, -0.5f, 0 }, lBR{ 0.5f, -0.5f, 0 };
        const FVec2 uTL{ 1, 0 }, uTR{ 0, 0 }, uBL{ 1, 1 }, uBR{ 0, 1 };
        for (u32 i = 0; i < all3d.Num() && stex.Num() < kMaxSpr; ++i) {
            if (!SceneMeshHierarchyVisible(h, i, all3d[i])) continue;
            game::AMeshComponent3D* mc = Mesh3D(all3d[i]);
            if (mc == nullptr || mc->RenderHandle() == nullptr) continue;
            const FMat4 m = SceneMeshWorldTransform(h, i, all3d[i]).ToMat4();
            auto wv = [&](FVec3 lp, FVec2 uv) {
                const FVec4 w = Transform(FVec4{ lp.x, lp.y, lp.z, 1.0f }, m);
                FSprVtx o; o.px = w.x; o.py = w.y; o.pz = w.z; o.u = uv.x; o.v = uv.y; sv.Add(o);
            };
            wv(lTL, uTL); wv(lBL, uBL); wv(lBR, uBR);     // 三角形 1
            wv(lTL, uTL); wv(lBR, uBR); wv(lTR, uTR);     // 三角形 2
            stex.Add(static_cast<IRhiTexture*>(mc->RenderHandle()));
        }
        if (stex.Num() > 0) {
            h.spr_vb->Update(sv.GetData(), sizeof(FSprVtx) * sv.Num());
            cl->SetPipeline(*h.spr_pipe);
            cl->SetConstantBuffer(0, *h.m3d_frame_cb);    // 同じ Frame CB を共有 (sprite は先頭3つの float4 のみ宣言・cam_pos 未使用)
            cl->SetVertexBuffer(*h.spr_vb, sizeof(FSprVtx));
            for (u32 i = 0; i < stex.Num(); ++i) {
                cl->SetTexture(0, *stex[i]);
                cl->Draw(6, i * 6);                       // クアッド i は頂点 [6i, 6i+6)
            }
        }
    }

    // Without the HDR/post chain there is no later load pass, so retain the
    // legacy direct-to-swapchain fallback.  The normal editor path draws this
    // overlay after all scene-space effects below.
    if (hdrRt == nullptr && !h.game_view) {
        DrawSelectedCameraFrustumOverlay(
            h, *cl, vp_nojit, aspect);
        DrawGizmo3DOverlay(h, *cl, vp, camPos);
    }

    // HDR RT → CPostProcess(ACES + 軽 Bloom)で backbuffer へ一度だけ tonemap。
    //         グリッド/ギズモも HDR に線形で乗っているので正しく tonemap される。editor は
    //         ビネット/色収差/グレイン無効でクリーンに。
    if (hdrRt != nullptr && scSwap != nullptr) {
        cl->EndRenderToTexture(*hdrRt);
        // SSR: 本フレームの lit scene color (hdrRt は今 SRV) + 深度 + 法線から反射を焼く。
        // 結果は ssr3d 内部 RT に残り、«次フレーム» の SetSsr が roughness ブレンドで使う (前フレーム反射方式)。
        h.ssr_computed = false;
        if (h.q_ssr_on && h.ssr_ready && gbufReady) {
            editor_profiler::FCpuScope ssrPostScope(
                h.profiler_work.post_cpu_ms);
            FScopedRhiGpuTiming ssrPostGpuScope(
                cl, ERhiGpuTimingPass::Post);
            IRhiTexture* hizEven = nullptr;
            IRhiTexture* hizOdd = nullptr;
            u32 hizMips = 0;
            if (h.q_ssr_hiz && h.hiz3d_ready) {
                h.hiz3d.Build(*h.renderer.Device(), *cl, *h.renderer.DepthBuffer());
                hizEven = h.hiz3d.EvenTexture();
                hizOdd = h.hiz3d.OddTexture();
                hizMips = h.hiz3d.MipCount();
            }
            h.ssr3d.Render(*h.renderer.Device(), *cl, *hdrRt, *h.renderer.DepthBuffer(), *h.normal_rt,
                           vp, Inverse(vp), h.prev_vp, eye, h.q_ssr_intensity,
                           h.mv_computed ? h.mv3d.OutputTexture() : nullptr,
                           hizEven, hizOdd, hizMips);   // full Hi-Z + motion で long-ray cost / ghost を抑制
            h.ssr_computed = h.ssr3d.HasValidOutput();
        } else {
            h.ssr3d.InvalidateHistory();
        }
        // SSGI: 同じ lit scene color + 深度 + 法線から 1 バウンス間接光を焼く (raw→blur→temporal)。
        // 結果は ssgi3d 内部 RT に残り «次フレーム» の SetSsgi が ambient に加算 (前フレーム間接光方式)。
        h.ssgi_computed = false;
        if (h.q_ssgi_on && h.ssgi_ready && gbufReady) {
            editor_profiler::FCpuScope ssgiPostScope(
                h.profiler_work.post_cpu_ms);
            FScopedRhiGpuTiming ssgiPostGpuScope(
                cl, ERhiGpuTimingPass::Post);
            h.ssgi3d.Render(*h.renderer.Device(), *cl, *hdrRt, *h.renderer.DepthBuffer(), *h.normal_rt,
                            vp, Inverse(vp), h.prev_vp, eye, h.q_ssgi_intensity, h.q_ssgi_max_dist,
                            h.mv_computed ? h.mv3d.OutputTexture() : nullptr);   // motion で動く物の間接光 ghost を除去
            h.ssgi_computed = h.ssgi3d.HasValidOutput();
        } else {
            h.ssgi3d.InvalidateHistory();
        }

        // --- モーションブラー: motion (UV 空間) に沿って scene を多タップ平均 (静止物はぼけず、動き/カメラ移動でぼける) ---
        if (h.q_motionblur_on && h.mblur_ready && h.blit_ready && h.mv_computed) {
            IRhiDevice* mdev = h.renderer.Device();
            if (mdev != nullptr) {
                if (h.refr_bg_w != scW || h.refr_bg_h != scH) {   // scene 複製 (refr_bg) 共用・遅延確保
                    FTextureDesc bd{}; bd.width = scW; bd.height = scH; bd.format = EFormat::R16G16B16A16_Float; bd.is_render_target = true;
                    auto btr = CreateRhiTexture(*mdev, bd);
                    if (btr.IsOk()) { h.refr_bg = Move(btr.Value()); h.refr_bg_w = scW; h.refr_bg_h = scH; } else { h.refr_bg_w = 0; }
                }
                if (h.refr_bg) {
                    const FClearColor bc{ 0.0f, 0.0f, 0.0f, 1.0f };
                    cl->BeginRenderToTexture(*h.refr_bg, bc, nullptr, 1.0f);
                    { FViewport bvp{}; bvp.width = static_cast<f32>(scW); bvp.height = static_cast<f32>(scH); cl->SetViewport(bvp);
                      FScissorRect bsr{}; bsr.right = static_cast<i32>(scW); bsr.bottom = static_cast<i32>(scH); cl->SetScissor(bsr); }
                    cl->SetPipeline(*h.blit_pipe); cl->SetTexture(0, *hdrRt); cl->Draw(3, 0);
                    cl->EndRenderToTexture(*h.refr_bg);
                    struct FMbCb { FVec4 mbp; FVec4 mbp2; } mcb{};
                    const f32 frameScale = std::clamp((1.0f / 60.0f) / h.frame_dt, 0.25f, 4.0f);
                    mcb.mbp = FVec4{ h.q_motionblur_intensity * frameScale,
                                    1.0f / static_cast<f32>(scW), 1.0f / static_cast<f32>(scH), 48.0f };
                    mcb.mbp2 = FVec4{
                        renderCamera.near_plane,
                        renderCamera.far_plane,
                        0.018f, renderOrtho ? 1.0f : 0.0f };
                    h.mblur_cb->Update(&mcb, sizeof(mcb));
                    cl->BeginRenderToTextureLoad(*hdrRt, nullptr);
                    { FViewport rvp{}; rvp.width = static_cast<f32>(scW); rvp.height = static_cast<f32>(scH); cl->SetViewport(rvp);
                      FScissorRect rsr{}; rsr.right = static_cast<i32>(scW); rsr.bottom = static_cast<i32>(scH); cl->SetScissor(rsr); }
                    cl->SetPipeline(*h.mblur_pipe);
                    cl->SetConstantBuffer(0, *h.mblur_cb);
                    cl->SetTexture(0, *h.refr_bg);
                    cl->SetTexture(1, *h.mv3d.OutputTexture());
                    cl->SetTexture(2, *h.renderer.DepthBuffer());
                    cl->Draw(3, 0);
                    cl->EndRenderToTexture(*hdrRt);
                }
            }
        }

        // Water writes the live depth before any atmosphere composite. AP,
        // volumetric clouds, and local fog then terminate against the displaced
        // surface, while later DoF/TAA consume the same updated depth.
        DrawInteractiveWater3DPass(
            h, *cl, *hdrRt, all3d,
            SceneMeshSubmissionMask(
                h,
                editor_frustum_culling::ESceneGeometryPass::
                    InteractiveWaterDraw),
            vp, eye, sunCol,
            sh.shadowOn, h.shadow.DepthTexture(), sh.lightVp,
            h.q_shadow_bias, h.q_shadow_filter, scW, scH);

        // --- 空気遠近法 + ボリューメトリックフォグ ---
        // Opaque + water の完成 depth で camera-volume を終端し、一度だけ
        // 合成する。depth は SRV で読むため DSV には同時 bind しない。
        if (apVol != nullptr && apTransVol != nullptr &&
            h.renderer.DepthBuffer() != nullptr) {
            editor_profiler::FCpuScope atmosphereScope(
                h.profiler_work.atmosphere_cpu_ms);
            FScopedRhiGpuTiming atmosphereGpuScope(
                cl, ERhiGpuTimingPass::Atmosphere);
            cl->BeginRenderToTextureLoad(*hdrRt, nullptr);
            h.sky_atmo.CompositeAerialPerspective(*cl, *h.renderer.DepthBuffer(),
                                                  *apVol, *apTransVol,
                                                  Inverse(vp), eye, kFogVolumeMaxDist,
                                                  scW, scH);
            cl->EndRenderToTexture(*hdrRt);
        }

        // --- ボリューメトリック雲の合成 ---
        // AP 後・屈折背景 capture 前に合成する。これによりガラス/水の
        // CRefractionShader は雲を完成済み HDR 背景として正しく屈折 sample する。
        // 完成した scene depth を point sample し、手前の opaque geometry も維持する。
        if (cloudsActive && h.renderer.DepthBuffer() != nullptr) {
            editor_profiler::FCpuScope cloudScope(
                h.profiler_work.cloud_cpu_ms);
            FScopedRhiGpuTiming cloudGpuScope(
                cl, ERhiGpuTimingPass::Cloud);
            cl->BeginRenderToTextureLoad(*hdrRt, nullptr);
            h.vclouds3d.Composite(*cl, *h.renderer.DepthBuffer(), scW, scH,
                                  apVol, apTransVol, kFogVolumeMaxDist);
            cl->EndRenderToTexture(*hdrRt);
        }

        // Long-range Rayleigh/Mie and near-field fog use separate froxel
        // ranges. Apply local fog after clouds so opaque geometry and cloud
        // pixels are terminated at their resolved distances; only clear sky
        // receives the 2.5 km far slice.
        if (canCompositeLocalFog) {
            editor_profiler::FCpuScope fogScope(
                h.profiler_work.fog_cpu_ms);
            FScopedRhiGpuTiming fogGpuScope(
                cl, ERhiGpuTimingPass::Fog);
            cl->BeginRenderToTextureLoad(*hdrRt, nullptr);
            h.sky_atmo.CompositeLocalFog(
                *cl, *localFogSceneDepth, *localFogVol,
                cloudsActive ? h.vclouds3d.ResolvedDepth() : nullptr,
                Inverse(vp), eye, h.sky_atmo.LocalFogMaxDistance(), scW, scH);
            cl->EndRenderToTexture(*hdrRt);
        }

        // --- 屈折 (ガラス/水): transmission>0 のノードを «opaque シーンを IOR で曲げて sample» して描く ---
        //     opaque + AP + cloud HDR を refr_bg へ blit → hdrRt を clear せず load 再オープン (opaque+depth 保持) →
        //     CRefractionShader で描画。env cubemap で Fresnel 反射 (要 Diligent IBL)。
        if (h.refr_ready && h.blit_ready && h.ibl_ready && h.ibl3d.EnvCubemap() != nullptr) {
            const bool anyRefr =
                editor_frustum_culling::AnySubmittedNode(
                    SceneMeshSubmissionMask(
                        h,
                        editor_frustum_culling::ESceneGeometryPass::
                            RefractionPreflight),
                    all3d.Num(),
                    [&](u32 i) noexcept {
                if (!SceneMeshHierarchyVisible(h, i, all3d[i])) return false;
                game::AMeshComponent3D* mc = Mesh3D(all3d[i]);
                return !IsRenderedByWater3D(h, all3d[i]) &&
                       mc != nullptr && mc->MaterialLoaded() &&
                       mc->Material().pbr.transmission > 0.0f;
            });
            IRhiDevice* rdev = h.renderer.Device();
            if (anyRefr && rdev != nullptr) {
                if (h.refr_bg_w != scW || h.refr_bg_h != scH) {   // refr_bg を画面サイズに遅延確保 (HDR)
                    FTextureDesc bd{}; bd.width = scW; bd.height = scH; bd.format = EFormat::R16G16B16A16_Float; bd.is_render_target = true;
                    auto btr = CreateRhiTexture(*rdev, bd);
                    if (btr.IsOk()) { h.refr_bg = Move(btr.Value()); h.refr_bg_w = scW; h.refr_bg_h = scH; } else { h.refr_bg_w = 0; }
                }
                if (h.refr_bg) {
                    const FClearColor bc{ 0.0f, 0.0f, 0.0f, 1.0f };   // 1. opaque HDR → refr_bg へ複製 (fullscreen blit)
                    cl->BeginRenderToTexture(*h.refr_bg, bc, nullptr, 1.0f);
                    { FViewport bvp{}; bvp.width = static_cast<f32>(scW); bvp.height = static_cast<f32>(scH); cl->SetViewport(bvp);
                      FScissorRect bsr{}; bsr.right = static_cast<i32>(scW); bsr.bottom = static_cast<i32>(scH); cl->SetScissor(bsr); }
                    cl->SetPipeline(*h.blit_pipe); cl->SetTexture(0, *hdrRt); cl->Draw(3, 0);
                    cl->EndRenderToTexture(*h.refr_bg);
                    // 2. hdrRt を clear せず load 再オープン (opaque + depth 保持) して屈折オブジェクトを上描き
                    cl->BeginRenderToTextureLoad(*hdrRt, h.renderer.DepthBuffer());
                    { FViewport rvp{}; rvp.width = static_cast<f32>(scW); rvp.height = static_cast<f32>(scH); cl->SetViewport(rvp);
                      FScissorRect rsr{}; rsr.right = static_cast<i32>(scW); rsr.bottom = static_cast<i32>(scH); cl->SetScissor(rsr); }
                    h.refr3d.SetFrame(vp, eye, scW, scH); // opaque と同じ vp/viewport で整合
                    editor_frustum_culling::ForEachSubmittedNode(
                        SceneMeshSubmissionMask(
                            h,
                            editor_frustum_culling::ESceneGeometryPass::
                                RefractionDraw),
                        all3d.Num(),
                        [&](u32 i) noexcept {
                        game::ANode* nn = all3d[i];
                        if (!SceneMeshHierarchyVisible(h, i, nn)) return;
                        game::AMeshComponent3D* mc = Mesh3D(nn);
                        if (mc == nullptr || mc->RenderHandle() != nullptr) return;
                        if (IsRenderedByWater3D(h, nn)) return;
                        if (!mc->MaterialLoaded() ||
                            mc->Material().pbr.transmission <= 0.0f) {
                            return;
                        }
                        FGpuMesh* gm = GpuMeshForNode3D(h, nn);
                        if (gm == nullptr ||
                            gm->vertex_buffer.Get() == nullptr ||
                            gm->index_buffer.Get() == nullptr) {
                            return;
                        }
                        const game::FPbrParams2D& p = mc->Material().pbr;
                        const FVec3 tint{ p.baseColor.x, p.baseColor.y, p.baseColor.z };
                        // thickness は «屈折先までの world 距離» = レンズ歪みの強さ。固定 0.5 だと小さく
                        // 背景がほぼ曲がらず «ガラスに見えない» → オブジェクトサイズ(world scale)に比例させる。
                        const game::FTransform3D world = SceneMeshWorldTransform(h, i, nn);
                        const FVec3 ws = world.scale;
                        const f32 thick = ((ws.x + ws.y + ws.z) / 3.0f) * 1.4f;
                        h.refr3d.DrawMesh(*cl, *gm, world.ToMat4(), *h.refr_bg, *h.ibl3d.EnvCubemap(), p.ior, thick, tint, p.roughness, 0.0f);
                    });
                    cl->EndRenderToTexture(*hdrRt);
                }
            }
        }

        // --- god rays (光芒): 太陽スクリーン位置から bright pass を放射状 march して光の筋を加算 ---
        //     太陽がカメラ前方にあるときのみ。scene を refr_bg へ blit → load 再オープン → 放射状ブラー加算。
        if (h.q_godray_on && h.gray_ready && h.blit_ready) {
            const FVec3 sunPt{ eye.x + h.sun_dir.x * 1000.0f, eye.y + h.sun_dir.y * 1000.0f, eye.z + h.sun_dir.z * 1000.0f };
            const FVec4 sc = Transform(FVec4{ sunPt.x, sunPt.y, sunPt.z, 1.0f }, vp_nojit);
            IRhiDevice* gdev = h.renderer.Device();
            if (sc.w > 1e-3f && gdev != nullptr) {     // 太陽がカメラ前方
                const f32 su = 0.5f + 0.5f * (sc.x / sc.w);
                const f32 sv = 0.5f - 0.5f * (sc.y / sc.w);
                const f32 sunOutside = std::max(std::max(-su, su - 1.0f), std::max(-sv, sv - 1.0f));
                const f32 sunViewportFade = 1.0f - std::clamp(sunOutside / 0.30f, 0.0f, 1.0f);
                if (h.refr_bg_w != scW || h.refr_bg_h != scH) {   // scene 複製 (refr_bg) 共用・遅延確保
                    FTextureDesc bd{}; bd.width = scW; bd.height = scH; bd.format = EFormat::R16G16B16A16_Float; bd.is_render_target = true;
                    auto btr = CreateRhiTexture(*gdev, bd);
                    if (btr.IsOk()) { h.refr_bg = Move(btr.Value()); h.refr_bg_w = scW; h.refr_bg_h = scH; } else { h.refr_bg_w = 0; }
                }
                if (h.refr_bg && sunViewportFade > 0.001f) {
                    const FClearColor bc{ 0.0f, 0.0f, 0.0f, 1.0f };   // 1. 現 HDR → refr_bg へ複製
                    cl->BeginRenderToTexture(*h.refr_bg, bc, nullptr, 1.0f);
                    { FViewport bvp{}; bvp.width = static_cast<f32>(scW); bvp.height = static_cast<f32>(scH); cl->SetViewport(bvp);
                      FScissorRect bsr{}; bsr.right = static_cast<i32>(scW); bsr.bottom = static_cast<i32>(scH); cl->SetScissor(bsr); }
                    cl->SetPipeline(*h.blit_pipe); cl->SetTexture(0, *hdrRt); cl->Draw(3, 0);
                    cl->EndRenderToTexture(*h.refr_bg);
                    // 2. hdrRt を load 再オープン → 放射状ブラーで光の筋を加算
                    struct FGrCb { FVec4 grp; FVec4 grp2; FVec4 grp3; } gcb{};
                    gcb.grp  = FVec4{ su, sv, h.q_godray_intensity * sunViewportFade, h.q_godray_decay };
                    gcb.grp2 = FVec4{ 1.0f, 0.30f, 0.55f, 0.0f };   // density, weight, bright threshold
                    gcb.grp3 = FVec4{ 1.0f / static_cast<f32>(scW), 1.0f / static_cast<f32>(scH),
                                     0.99945f, 1.25f };
                    h.gray_cb->Update(&gcb, sizeof(gcb));
                    cl->BeginRenderToTextureLoad(*hdrRt, nullptr);
                    { FViewport rvp{}; rvp.width = static_cast<f32>(scW); rvp.height = static_cast<f32>(scH); cl->SetViewport(rvp);
                      FScissorRect rsr{}; rsr.right = static_cast<i32>(scW); rsr.bottom = static_cast<i32>(scH); cl->SetScissor(rsr); }
                    cl->SetPipeline(*h.gray_pipe);
                    cl->SetConstantBuffer(0, *h.gray_cb);
                    cl->SetTexture(0, *h.refr_bg);
                    cl->SetTexture(1, *h.renderer.DepthBuffer());
                    cl->Draw(3, 0);
                    cl->EndRenderToTexture(*hdrRt);
                }
            }
        }

        // --- 被写界深度 (DoF): depth から CoC を出して焦点外をディスクぼかし (屈折の後＝ガラスも DoF に乗る) ---
        //     opaque+glass の HDR を refr_bg へ blit → hdrRt を load 再オープン (depth は DSV にせず SRV で sample) →
        //     DoF シェーダで焦点距離に応じてぼかす。
        if (h.q_dof_on && h.dof_ready && h.blit_ready) {
            IRhiDevice* ddev = h.renderer.Device();
            if (ddev != nullptr) {
                if (h.refr_bg_w != scW || h.refr_bg_h != scH) {   // scene 複製 (refr_bg) を共用・遅延確保
                    FTextureDesc bd{}; bd.width = scW; bd.height = scH; bd.format = EFormat::R16G16B16A16_Float; bd.is_render_target = true;
                    auto btr = CreateRhiTexture(*ddev, bd);
                    if (btr.IsOk()) { h.refr_bg = Move(btr.Value()); h.refr_bg_w = scW; h.refr_bg_h = scH; } else { h.refr_bg_w = 0; }
                }
                if (h.refr_bg) {
                    const FClearColor bc{ 0.0f, 0.0f, 0.0f, 1.0f };   // 1. 現 HDR → refr_bg へ複製
                    cl->BeginRenderToTexture(*h.refr_bg, bc, nullptr, 1.0f);
                    { FViewport bvp{}; bvp.width = static_cast<f32>(scW); bvp.height = static_cast<f32>(scH); cl->SetViewport(bvp);
                      FScissorRect bsr{}; bsr.right = static_cast<i32>(scW); bsr.bottom = static_cast<i32>(scH); cl->SetScissor(bsr); }
                    cl->SetPipeline(*h.blit_pipe); cl->SetTexture(0, *hdrRt); cl->Draw(3, 0);
                    cl->EndRenderToTexture(*h.refr_bg);
                    // 2. hdrRt を load 再オープン (depth は DSV にせず) → DoF ぼかしを書き戻す
                    struct FDofCb { FVec4 dofp; FVec4 dofp2; } dcb{};
                    const f32 maxBlurPx = std::clamp(h.q_dof_max * static_cast<f32>(scH), 0.0f, 48.0f);
                    dcb.dofp  = FVec4{
                        h.q_dof_focus, h.q_dof_range,
                        maxBlurPx, renderCamera.near_plane };
                    dcb.dofp2 = FVec4{
                        renderCamera.far_plane,
                        1.0f / static_cast<f32>(scW),
                        1.0f / static_cast<f32>(scH),
                        renderOrtho ? 1.0f : 0.0f };
                    h.dof_cb->Update(&dcb, sizeof(dcb));
                    cl->BeginRenderToTextureLoad(*hdrRt, nullptr);
                    { FViewport rvp{}; rvp.width = static_cast<f32>(scW); rvp.height = static_cast<f32>(scH); cl->SetViewport(rvp);
                      FScissorRect rsr{}; rsr.right = static_cast<i32>(scW); rsr.bottom = static_cast<i32>(scH); cl->SetScissor(rsr); }
                    cl->SetPipeline(*h.dof_pipe);
                    cl->SetConstantBuffer(0, *h.dof_cb);
                    cl->SetTexture(0, *h.refr_bg);
                    cl->SetTexture(1, *h.renderer.DepthBuffer());
                    cl->Draw(3, 0);
                    cl->EndRenderToTexture(*hdrRt);
                }
            }
        }

        // Editor chrome is not scene radiance.  Draw it only after clouds,
        // physical/local atmosphere, refraction, motion blur, god rays and DoF
        // have completed, then let the single viewport tonemap convert it with
        // the rest of HDR.  The depth-off overlay intentionally remains
        // readable even when the selected object sits behind dense cloud/fog.
        if (!h.game_view && FindNode3DNode(h, h.sel3d) != nullptr) {
            cl->BeginRenderToTextureLoad(*hdrRt, nullptr);
            DrawGizmo3DOverlay(h, *cl, vp, camPos);
            cl->EndRenderToTexture(*hdrRt);
        }

        FPostProcessParams pp{};
        // bloom: 6-mip + progressive radius + soft-knee の高品質 bloom。threshold は HDR 1.0 超を抽出、
        // soft-knee でなめらかに立ち上げ。radius は progressive で深い mip ほど広がる(基準 1.0)。
        // 自然な bloom: threshold を «拡散面の明るさ» より上に置き、ハイライト/縁だけ抽出 (オブジェクト
        // 全体が光る «発光感» を回避)。intensity 控えめ + radius 広めで «柔らかく広い» 質感は維持。
        // 品質プリセット (Rendering/QualityLevel) で bloom / 彩度・コントラスト / CAS シャープを駆動。
        pp.bloom_enabled = h.q_bloom_on; pp.bloom_intensity = h.q_bloom_intensity;
        pp.bloom_threshold = h.q_bloom_threshold; pp.bloom_radius = h.q_bloom_radius;
        pp.tonemap_kind = h.q_tonemap;   // 0=ACES Filmic / 1=AgX (Sobotka) / 2=Reinhard 拡張
        pp.exposure = h.q_exposure; pp.gamma = 2.2f;
        if (h.q_auto_exposure) {   // eye adaptation: シーン輝度から露出を自動算出 (q_exposure は EV 補正に)
            pp.auto_exposure_enabled = true; pp.auto_exposure_speed = 2.0f;
            pp.auto_exposure_key = 0.5f; pp.auto_exposure_min = 0.05f; pp.auto_exposure_max = 12.0f;
            pp.delta_time = std::clamp(h.frame_dt, 1.0f / 240.0f, 0.10f);
        }
        pp.cg_saturation = h.q_cg_saturation; pp.cg_contrast = h.q_cg_contrast;
        pp.cas_strength = h.q_cas;   // CAS シャープ (品質連動)
        // シネマフィルタ (既定 0=クリーンなエディタ表示。ゲーム出力プレビュー用に設定で有効化可)。
        pp.vignette_intensity = h.q_vignette; pp.chromatic_aberration = h.q_chromatic;
        pp.grain_intensity = h.q_grain; pp.grain_time = h.time;
        // TAA: Halton ジッタ + history の neighborhood-clamp blend でテンポラル AA。reproject は «jitter 無し»
        // の vp/prev で行う (camera motion 由来。depth から history を offset sample)。motion_texture は未使用。
        if (taaOn) {
            pp.taa_enabled = true;
            pp.taa_blend_factor =
                camera_view_history_reset
                    ? 1.0f   // initialize this request from current color
                    : 0.1f;  // 10% current + 90% history
            pp.taa_depth_texture = h.renderer.DepthBuffer();
            pp.taa_view_proj_no_jitter      = vp_nojit;
            pp.taa_prev_view_proj_no_jitter = h.prev_vp_nojit;
            pp.taa_camera_position          = eye;
            // motion vector があれば depth reproject の代わりに使う (動く mesh も ghost せず追従)。
            if (h.mv_computed) pp.taa_motion_texture = h.mv3d.OutputTexture();
            // 雲の RG32F resolved depth は G=coverage。雲自身の temporal resolve を
            // global history へ再投入せず、周囲のジオメトリだけ TAA する。
            if (cloudsActive) pp.taa_reactive_texture = h.vclouds3d.ResolvedDepth();
        }
        {
            editor_profiler::FCpuScope postScope(
                h.profiler_work.post_cpu_ms);
            FScopedRhiGpuTiming postGpuScope(
                cl, ERhiGpuTimingPass::Post);
            h.post3d.Render(*cl, *scSwap, h.renderer.CurrentBuffer(), pp);
        }
        AEditor3DRecordComponent* selected_camera_record =
            Rec3D(FindNode3DNode(h, h.sel3d));
        if (!h.game_view && h.show_camera_frustum &&
            selected_camera_record != nullptr &&
            selected_camera_record->has_scene_camera) {
            cl->BeginRenderToSwapchainLoad(
                *scSwap, h.renderer.CurrentBuffer());
            DrawSelectedCameraFrustumOverlay(
                h, *cl, vp_nojit, aspect);
            cl->EndRenderToSwapchain(
                *scSwap, h.renderer.CurrentBuffer());
        }
    }
    h.prev_vp = vp;               // SSR/SSGI temporal reproject 用 (jitter 込み)
    h.prev_vp_nojit = vp_nojit;   // TAA history reproject 用 (jitter 無し)
    h.prev_temporal_camera_eye = eye;
    h.temporal_camera_pose_valid = true;
    if (taaOn) ++h.taa_frame;     // ジッタ列を進める (TAA 無効時は固定)
    if (rendering_camera_view_request) {
        ++h.camera_view_frame_serial;
        if (h.camera_view_frame_serial == 0u)
            ++h.camera_view_frame_serial;
        h.camera_view_requests.MarkPresenterRendered(
            h.camera_view_frame_serial,
            scW,
            scH);
        h.last_render_camera_view_request_id =
            camera_view_request_id;
        h.last_render_camera_view_history_generation =
            camera_view_history_generation;
    } else {
        h.last_render_camera_view_request_id = 0u;
        h.last_render_camera_view_history_generation = 0u;
    }
}

} // namespace

// =============================================================================
// C ABI — ライフサイクル / 描画
// =============================================================================

ACS_EDITOR_API const char* acs_editor_version(void) {
    return "ACS Editor 0.15";
}

ACS_EDITOR_API std::uint32_t acs_editor_abi_contract_version(void) {
    return editor_abi::kContractVersion;
}

ACS_EDITOR_API std::uint64_t acs_editor_abi_capabilities(void) {
    return editor_abi::kCapabilities;
}

ACS_EDITOR_API const char* acs_editor_render_backend(void) {
#if WITH_RENDER_DILIGENT
    return "Diligent";
#else
    return "Raw DX12";
#endif
}

ACS_EDITOR_API int acs_editor_abi_query(
    std::uint32_t requested_version,
    std::uint64_t required_capabilities,
    std::uint32_t* out_version,
    std::uint64_t* out_capabilities) {
    if (out_version != nullptr) {
        *out_version = editor_abi::kContractVersion;
    }
    if (out_capabilities != nullptr) {
        *out_capabilities = editor_abi::kCapabilities;
    }
    return editor_abi::IsCompatible(
        requested_version,
        required_capabilities) ? 1 : 0;
}

/**
 * Query the reason an optional native service is enabled, pending, inactive,
 * disabled, or failed.
 *
 * A structurally valid request always returns 1 and receives a typed status,
 * including null-host and unknown-service failures. Malformed version/size
 * headers return 0 without dereferencing a host. Version 1 receives the exact
 * 192-byte prefix; version 2 receives the full 256-byte payload.
 */
ACS_EDITOR_API int acs_editor_optional_service_diagnostic_get(
    void* handle,
    std::uint32_t service,
    editor_service_diagnostics::FDiagnostic* out_diagnostic,
    std::uint32_t out_size) {
    using namespace editor_service_diagnostics;
    if (out_diagnostic == nullptr ||
        out_size < sizeof(u32) * 2u) {
        return 0;
    }

    u32 requested_header[2]{};
    std::memcpy(
        requested_header,
        out_diagnostic,
        sizeof(requested_header));
    const u32 requested_version = requested_header[0];
    const u32 requested_size = requested_header[1];
    u32 copy_size = 0u;
    if (requested_version == kLegacyDiagnosticVersion) {
        if (requested_size < kLegacyDiagnosticSize ||
            requested_size > out_size ||
            out_size < kLegacyDiagnosticSize) {
            return 0;
        }
        copy_size = kLegacyDiagnosticSize;
    } else if (requested_version == kDiagnosticVersion) {
        if (requested_size < kDiagnosticSize ||
            requested_size > out_size ||
            out_size < kDiagnosticSize) {
            return 0;
        }
        copy_size = kDiagnosticSize;
    } else {
        return 0;
    }

    FDiagnostic resolved{};
    {
        // Validate the opaque identity before dereferencing it and keep
        // destroy from reclaiming a live host until the small status snapshot
        // has been copied. This also makes stale non-null and arbitrary handles
        // deterministic InvalidHost results instead of use-after-free reads.
        const auto* candidate =
            static_cast<const FEditorHost*>(handle);
        const FScopedLock lock(g_editor_host_registry_mutex);
        const FEditorHost* host = nullptr;
        for (usize index = 0u;
             index < g_live_editor_host_count;
             ++index) {
            const FEditorHost* live_host =
                g_live_editor_hosts[index];
            if (live_host != candidate) continue;
            host = live_host;
            break;
        }
        resolved = ResolveServiceDiagnostic(
            host,
            service,
            NextNonZeroGeneration(
                g_next_editor_diagnostic_generation));
    }
    if (requested_version == kLegacyDiagnosticVersion) {
        resolved.version = kLegacyDiagnosticVersion;
        resolved.struct_size = kLegacyDiagnosticSize;
    }
    std::memcpy(out_diagnostic, &resolved, copy_size);
    return 1;
}

ACS_EDITOR_API void* acs_editor_create(void) {
    if (!EnsureSubsystems()) return nullptr;
    auto* host = new (std::nothrow) FEditorHost();
    if (host != nullptr) {
        host->abi_host_generation =
            NextNonZeroGeneration(g_next_editor_host_generation);
        // Production editor hosts start with an explicit empty document.  A demo scene here
        // used to be visible for several frames before the managed initial-scene load and
        // also became the accidental fallback after a failed load.
        ClearScene(*host);
        if (!RegisterEditorHost(host)) {
            delete host;
            ReleaseSubsystems();
            return nullptr;
        }
    } else {
        ReleaseSubsystems();
    }
    return host;
}

constexpr FClearColor kEditorNeutralClear{
    0.035f, 0.043f, 0.055f, 1.0f};

int SubmitAndPresentEditorFrame(
    FEditorHost& host, bool avoid_gpu_wait) noexcept {
    const bool presented = avoid_gpu_wait
        ? host.renderer.EndFrameWithoutGpuWait()
        : host.renderer.EndFrame();
    if (!presented)
        return editor_frame::ToAbi(editor_frame::EResult::Fatal);
    host.resource_mutation_idle = false;
    return editor_frame::ToAbi(editor_frame::EResult::Presented);
}

/**
 * Present a deterministic editor-owned frame without entering scene rendering.
 *
 * Every incremental startup phase completes any frame it opens before
 * returning, so this is safe immediately after AdvanceEditorStartup. Startup
 * callers disable GPU timing to keep warm-up and first-frame profiling clean.
 */
int PresentNeutralEditorFrame(
    FEditorHost& host, bool record_gpu_timing,
    bool avoid_gpu_wait) noexcept {
    if (!host.attached || host.renderer.Device() == nullptr ||
        host.renderer.Swapchain() == nullptr) {
        return editor_frame::ToAbi(editor_frame::EResult::Fatal);
    }
    if (!host.renderer.IsOperational())
        return editor_frame::ToAbi(editor_frame::EResult::Fatal);
    IRhiCommandList* command_list = host.renderer.CommandList();
    if (command_list != nullptr) command_list->ResetStatistics();
    if (avoid_gpu_wait) {
        if (!host.renderer.CanBeginFrameWithoutGpuWait()) {
            return editor_frame::ToAbi(editor_frame::EResult::Busy);
        }
        // Both checks execute serially on the HWND/RHI owner thread. No other
        // editor caller can consume this frame slot between the preflight and
        // begin, so a failed begin here is an invariant/backend failure rather
        // than transient GPU backpressure.
        if (!host.renderer.TryBeginFrameWithoutGpuWait(
                kEditorNeutralClear)) {
            return editor_frame::ToAbi(editor_frame::EResult::Fatal);
        }
    } else {
        host.renderer.BeginFrame(kEditorNeutralClear);
    }
    if (record_gpu_timing && command_list != nullptr) {
        command_list->BeginGpuTimingFrame(
            host.profiler_snapshot.frame_index + 1u);
        command_list->EndGpuTimingFrame();
    }
    return SubmitAndPresentEditorFrame(host, avoid_gpu_wait);
}

bool PresentNeutralEditorFrame(
    FEditorHost& host, bool record_gpu_timing) noexcept {
    return PresentNeutralEditorFrame(
        host, record_gpu_timing, false) > 0;
}

ACS_EDITOR_API int acs_editor_attach(void* handle, void* hwnd, uint32_t width, uint32_t height) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || hwnd == nullptr || width == 0u || height == 0u) return 0;
    if (host->attached) {
        ACS_LOG_WARN("[acs_editor_abi] attach is only valid once per editor host");
        return 0;
    }
    const auto r = host->renderer.InitExternal(hwnd, width, height, /*debug=*/false, /*depth=*/true);
    if (r.IsErr()) {
        ACS_LOG_ERROR("[acs_editor_abi] attach failed: %s", r.Error().message);
        return 0;
    }
    host->attached = true;
    host->width    = width;
    host->height   = height;
    host->profiler_work = {};
    host->profiler_snapshot = {};
    host->cloud_workload_snapshot = {};
    host->cloud_workload_available = false;
    host->profiler_cpu_peak.Reset();
    host->profiler_gpu_peak.Reset();
    host->profiler_active_cpu_peak.Reset();
    host->profiler_present_cpu_peak.Reset();
    host->profiler_gpu_queries.Reset();
    host->profiler_last_gpu_peak_frame = 0u;
    host->profiler_presented_since_reset = 0u;
    host->profiler_reset_serial = 0u;
    host->profiler_snapshot.viewport_width = width;
    host->profiler_snapshot.viewport_height = height;
    host->water3d_background_failed = false;
    host->water3d_depth_copy_failed = false;
    host->ssss3d_init_failed = false;
    host->ssss3d_init_state = 0u;
    host->ssss3d_pending_shaders = {};
    host->profiler_smoothed_fps = 0.0f;
    host->profiler_has_previous_frame = false;

    // プロジェクト設定を既定値で初期化 (C# がプロジェクトを開いた後 settings_load_text で上書き)
    host->settings.ResetToDefaults();

    // Shader/PSO creation used to happen synchronously here.  That kept the
    // WPF UI thread inside one native call long enough for Windows to mark the
    // editor as Not Responding.  acs_editor_render advances the same work in
    // bounded steps before BeginFrame, so uploads remain outside an active GPU
    // frame and all native work stays on this owner thread.
    host->startup_step = 0u;
    host->startup_ready = false;
    host->startup_failed = false;
    host->startup_begin = editor_profiler::CClock::now();

    // Own the child HWND's pixels before returning to WPF. Otherwise HwndHost
    // airspace can expose uninitialized/old swapchain contents during warm-up.
    if (!PresentNeutralEditorFrame(*host, false)) {
        ACS_LOG_ERROR(
            "[acs_editor_abi] attach failed during initial neutral present");
        host->attached = false;
        host->renderer.Shutdown();
        return 0;
    }

    ACS_LOG_INFO("[acs_editor_abi] attached to HWND %p (%ux%u)", hwnd, width, height);
    return 1;
}

/** Query non-blocking renderer warm-up state.
 * Returns 1 when ready, 0 while waiting/preparing, and -1 on failure/invalid
 * handle.  Output pointers are optional to keep the C ABI easy to probe. */
ACS_EDITOR_API int acs_editor_startup_status(
    void* handle, uint32_t* completed, uint32_t* total) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (completed != nullptr) {
        *completed = host != nullptr ? host->startup_step : 0u;
    }
    if (total != nullptr) *total = kEditorStartupStepCount;
    if (host == nullptr || host->startup_failed) return -1;
    return host->startup_ready ? 1 : 0;
}

/** Suppress scene presentation without stopping renderer warm-up or swapchain progress. */
ACS_EDITOR_API void acs_editor_set_scene_presentation_suppressed(
    void* handle, int suppressed) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return;
    const bool next = suppressed != 0;
    if (host->scene_presentation_suppressed == next) return;
    host->scene_presentation_suppressed = next;
    // Loading time must not pollute the first visible profiler interval.
    host->profiler_has_previous_frame = false;
    host->profiler_smoothed_fps = 0.0f;
}

static void EditorStepPlay(FEditorHost& h, f32 dt) noexcept;   // 前方宣言 (定義は Play モード節)
static void EditorTickLogic(FEditorHost& h, f32 dt) noexcept;  // 前方宣言 (インプロセス Play)

/** swapchain と同サイズのシーン用オフスクリーン RT を用意する (リサイズ/サンプル数変更時は作り直し)。 */
static bool EnsureSceneRt(FEditorHost& h, u32 w, u32 hgt) noexcept {
    if (w == 0 || hgt == 0) return false;
    if (h.scene_rt && h.scene_rt_w == w && h.scene_rt_h == hgt) return true;
    IRhiDevice* dev = h.renderer.Device();
    if (dev == nullptr) return false;
    if (h.scene_rt) dev->WaitIdle();        // 旧 RT を安全に解放するため (リサイズ/MSAA変更時のみ)。初回生成は
                                            // 旧 RT が無く WaitIdle 不要 — 描画ループ初フレームでの不要な WaitIdle が
                                            // フレームペーシングと競合して間欠クラッシュするのを避ける。
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

static void BeginProfilerFrame(
    FEditorHost& host,
    editor_profiler::FTimePoint frameBegin) noexcept {
    host.profiler_work = {};

    if (host.profiler_has_previous_frame) {
        const f32 intervalMs = static_cast<f32>(
            std::chrono::duration<double, std::milli>(
                frameBegin - host.profiler_last_frame_begin).count());
        if (std::isfinite(intervalMs) && intervalMs > 0.01f) {
            const f32 instantaneousFps = 1000.0f / intervalMs;
            host.profiler_smoothed_fps =
                host.profiler_smoothed_fps > 0.0f
                    ? host.profiler_smoothed_fps * 0.90f +
                          instantaneousFps * 0.10f
                    : instantaneousFps;
        }
    }
    host.profiler_last_frame_begin = frameBegin;
    host.profiler_has_previous_frame = true;
}

static void PublishCloudWorkloadSnapshot(
    FEditorHost& host, u64 profilerFrameIndex) noexcept {
    static_assert(
        static_cast<u32>(EVolumetricCloudFrameSkipReason::None) ==
            static_cast<u32>(editor_cloud_workload::ESkipReason::None) &&
        static_cast<u32>(
            EVolumetricCloudFrameSkipReason::ResourcesNotReady) ==
            static_cast<u32>(
                editor_cloud_workload::ESkipReason::ResourcesNotReady) &&
        static_cast<u32>(
            EVolumetricCloudFrameSkipReason::InvalidCamera) ==
            static_cast<u32>(
                editor_cloud_workload::ESkipReason::InvalidCamera) &&
        static_cast<u32>(
            EVolumetricCloudFrameSkipReason::InvalidProjection) ==
            static_cast<u32>(
                editor_cloud_workload::ESkipReason::InvalidProjection),
        "cloud renderer skip reasons must match the published ABI");
    host.cloud_workload_snapshot = {};
    host.cloud_workload_available = false;
    if (!host.attached || !host.startup_ready || !host.vclouds_ready ||
        !host.profiler_work.clouds_active) {
        return;
    }

    const FVolumetricCloudFrameWorkload& workload =
        host.vclouds3d.LastFrameWorkload();
    if (!workload.attempted) return;

    editor_cloud_workload::FSnapshot& snapshot =
        host.cloud_workload_snapshot;
    snapshot.profiler_frame_index = profilerFrameIndex;
    snapshot.submission_index = workload.submission_index;
    snapshot.trace_width = workload.trace_width;
    snapshot.trace_height = workload.trace_height;
    snapshot.output_width = workload.output_width;
    snapshot.output_height = workload.output_height;
    snapshot.steady_dispatches = workload.steady_dispatches;
    snapshot.one_time_bake_dispatches =
        workload.one_time_bake_dispatches;
    snapshot.shadow_cache_dispatches =
        workload.shadow_cache_dispatches;
    snapshot.total_compute_dispatches =
        workload.total_compute_dispatches;
    snapshot.composite_draws = workload.composite_draws;
    snapshot.trace_logical_invocations =
        workload.trace_logical_invocations;
    snapshot.trace_launched_threads =
        workload.trace_launched_threads;
    snapshot.resolve_logical_invocations =
        workload.resolve_logical_invocations;
    snapshot.resolve_launched_threads =
        workload.resolve_launched_threads;
    snapshot.one_time_bake_logical_invocations =
        workload.one_time_bake_logical_invocations;
    snapshot.one_time_bake_launched_threads =
        workload.one_time_bake_launched_threads;
    snapshot.shadow_cache_logical_invocations =
        workload.shadow_cache_logical_invocations;
    snapshot.shadow_cache_launched_threads =
        workload.shadow_cache_launched_threads;
    snapshot.total_logical_invocations =
        workload.total_logical_invocations;
    snapshot.total_launched_threads =
        workload.total_launched_threads;
    snapshot.maximum_view_samples = workload.maximum_view_samples;
    snapshot.maximum_light_samples = workload.maximum_light_samples;
    snapshot.skip_reason = static_cast<u32>(workload.skip_reason);
    if (workload.attempted) {
        snapshot.flags |= editor_cloud_workload::Attempted;
    }
    if (workload.submitted) {
        snapshot.flags |= editor_cloud_workload::Submitted;
    }
    if (workload.history_was_available) {
        snapshot.flags |= editor_cloud_workload::HistoryWasAvailable;
    }
    if (workload.history_reused) {
        snapshot.flags |= editor_cloud_workload::HistoryReused;
    }
    if (workload.history_invalidated) {
        snapshot.flags |= editor_cloud_workload::HistoryInvalidated;
    }
    if (workload.temporal_super_resolution) {
        snapshot.flags |=
            editor_cloud_workload::TemporalSuperResolution;
    }
    host.cloud_workload_available =
        workload.skip_reason !=
            EVolumetricCloudFrameSkipReason::ResourcesNotReady;
}

static void PublishProfilerFrame(
    FEditorHost& host,
    editor_profiler::FTimePoint frameBegin,
    f32 submitMs,
    f32 nativeRenderActiveMs) noexcept {
    editor_profiler::FSnapshot& snapshot = host.profiler_snapshot;
    snapshot.version = editor_profiler::kSnapshotVersion;
    snapshot.struct_size = editor_profiler::kSnapshotSize;
    snapshot.timing_source = static_cast<u32>(
        editor_profiler::ETimingSource::CpuRecordSubmit);
    snapshot.flags = 0u;
    if (host.scene_presentation_suppressed) {
        snapshot.flags |=
            editor_profiler::ESnapshotFlags::ScenePresentationSuppressed;
    }
    if (host.view3d) {
        snapshot.flags |= editor_profiler::ESnapshotFlags::View3D;
    }
    if (host.profiler_work.clouds_active) {
        snapshot.flags |= editor_profiler::ESnapshotFlags::Clouds;
    }
    if (host.profiler_work.scene_mesh_cache_rebuilt) {
        snapshot.flags |=
            editor_profiler::ESnapshotFlags::SceneMeshCacheRebuilt;
    }
    if (host.game_view) {
        snapshot.flags |= editor_profiler::ESnapshotFlags::GameView;
    }
    if (host.profiler_work.runtime_scene_camera) {
        snapshot.flags |=
            editor_profiler::ESnapshotFlags::RuntimeSceneCamera;
    }
    if (host.profiler_work.frustum_culling_enabled) {
        snapshot.flags |=
            editor_profiler::ESnapshotFlags::FrustumCullingEnabled;
    }
    if (host.view3d &&
        host.profiler_work.render_camera_resolved &&
        !host.profiler_work.render_orthographic &&
        host.q_fog_on) {
        snapshot.flags |= editor_profiler::ESnapshotFlags::Fog;
    }
    if (host.view3d &&
        host.profiler_work.render_camera_resolved &&
        !host.profiler_work.render_orthographic &&
        host.q_ap_on) {
        snapshot.flags |= editor_profiler::ESnapshotFlags::AerialPerspective;
    }

    ++snapshot.frame_index;
    FRhiGpuTimingSnapshot gpuTiming{};
    bool gpuTimingValid = false;
    const auto validGpuMetric = [](f32 value) noexcept {
        return std::isfinite(value) && value >= 0.0f;
    };
    if (IRhiCommandList* commandList = host.renderer.CommandList();
        commandList != nullptr) {
        const FRhiCommandStatistics& statistics =
            commandList->Statistics();
        snapshot.draw_calls = statistics.draw_calls;
        snapshot.dispatch_calls = statistics.dispatch_calls;
        snapshot.triangles = statistics.triangles;
        gpuTimingValid =
            commandList->TryGetGpuTiming(gpuTiming) &&
            gpuTiming.valid &&
            gpuTiming.frame_index != 0u &&
            validGpuMetric(gpuTiming.frame_ms) &&
            validGpuMetric(gpuTiming.opaque_ms) &&
            validGpuMetric(gpuTiming.atmosphere_ms) &&
            validGpuMetric(gpuTiming.cloud_ms) &&
            validGpuMetric(gpuTiming.fog_ms) &&
            validGpuMetric(gpuTiming.post_ms);
    } else {
        snapshot.draw_calls = 0u;
        snapshot.dispatch_calls = 0u;
        snapshot.triangles = 0u;
    }

    snapshot.fps = host.profiler_smoothed_fps;
    snapshot.cpu_frame_ms =
        editor_profiler::ElapsedMilliseconds(frameBegin);
    snapshot.cpu_submit_ms =
        std::isfinite(submitMs) && submitMs >= 0.0f ? submitMs : 0.0f;
    snapshot.native_render_active_cpu_ms =
        std::isfinite(nativeRenderActiveMs) &&
        nativeRenderActiveMs >= 0.0f
            ? nativeRenderActiveMs
            : 0.0f;
    snapshot.native_present_cpu_ms = snapshot.cpu_submit_ms;
    host.profiler_active_cpu_peak.Add(
        snapshot.native_render_active_cpu_ms);
    host.profiler_present_cpu_peak.Add(
        snapshot.native_present_cpu_ms);
    snapshot.native_render_active_cpu_peak_ms =
        host.profiler_active_cpu_peak.Peak();
    snapshot.native_present_cpu_peak_ms =
        host.profiler_present_cpu_peak.Peak();
    snapshot.presented_frame_count_since_reset =
        ++host.profiler_presented_since_reset;
    snapshot.profiler_reset_serial = host.profiler_reset_serial;

    snapshot.opaque_cpu_ms = host.profiler_work.opaque_cpu_ms;
    snapshot.atmosphere_cpu_ms =
        host.profiler_work.atmosphere_cpu_ms;
    snapshot.cloud_cpu_ms = host.profiler_work.cloud_cpu_ms;
    snapshot.fog_cpu_ms = host.profiler_work.fog_cpu_ms;
    snapshot.post_cpu_ms = host.profiler_work.post_cpu_ms;
    if (host.profiler_work.frustum_culling_enabled) {
        snapshot.frustum_tested =
            host.profiler_work.frustum_tested;
        snapshot.frustum_visible =
            host.profiler_work.frustum_visible;
        snapshot.frustum_culled =
            host.profiler_work.frustum_culled;
    } else {
        snapshot.frustum_tested = 0u;
        snapshot.frustum_visible = 0u;
        snapshot.frustum_culled = 0u;
    }
    snapshot.active_camera_node_id =
        host.profiler_work.runtime_scene_camera
            ? host.profiler_work.active_camera_node_id : -1;

    snapshot.gpu_frame_index = 0u;
    snapshot.gpu_latency_frames = 0u;
    snapshot.gpu_frame_ms = -1.0f;
    snapshot.opaque_gpu_ms = -1.0f;
    snapshot.atmosphere_gpu_ms = -1.0f;
    snapshot.cloud_gpu_ms = -1.0f;
    snapshot.fog_gpu_ms = -1.0f;
    snapshot.post_gpu_ms = -1.0f;
    if (gpuTimingValid) {
        snapshot.timing_source = static_cast<u32>(
            editor_profiler::ETimingSource::GpuTimestamp);
        snapshot.flags |= editor_profiler::ESnapshotFlags::GpuTimingsValid;
        snapshot.gpu_frame_index = gpuTiming.frame_index;
        const u64 latency =
            snapshot.frame_index > gpuTiming.frame_index
                ? snapshot.frame_index - gpuTiming.frame_index
                : 0u;
        snapshot.gpu_latency_frames = static_cast<u32>(
            latency > 0xFFFFFFFFull ? 0xFFFFFFFFull : latency);
        snapshot.gpu_frame_ms = gpuTiming.frame_ms;
        snapshot.opaque_gpu_ms = gpuTiming.opaque_ms;
        snapshot.atmosphere_gpu_ms = gpuTiming.atmosphere_ms;
        snapshot.cloud_gpu_ms = gpuTiming.cloud_ms;
        snapshot.fog_gpu_ms = gpuTiming.fog_ms;
        snapshot.post_gpu_ms = gpuTiming.post_ms;
        if (gpuTiming.frame_index !=
            host.profiler_last_gpu_peak_frame) {
            const editor_profiler::FGpuQuerySample querySample{
                gpuTiming.frame_ms,
                gpuTiming.opaque_ms,
                gpuTiming.atmosphere_ms,
                gpuTiming.cloud_ms,
                gpuTiming.fog_ms,
                gpuTiming.post_ms};
            if (host.profiler_gpu_queries.Add(
                    gpuTiming.frame_index, querySample)) {
                host.profiler_gpu_peak.Add(gpuTiming.frame_ms);
            }
            host.profiler_last_gpu_peak_frame =
                gpuTiming.frame_index;
        }
    }

    host.profiler_cpu_peak.Add(snapshot.cpu_frame_ms);
    snapshot.cpu_frame_peak_ms = host.profiler_cpu_peak.Peak();
    snapshot.gpu_frame_peak_ms =
        host.profiler_gpu_peak.HasValues()
            ? host.profiler_gpu_peak.Peak()
            : -1.0f;
    snapshot.peak_window_frames =
        editor_profiler::kPeakWindowFrames;

    const editor_profiler::FGpuQueryWindowStatistics gpuWindow =
        host.profiler_gpu_queries.Statistics();
    snapshot.gpu_query_window_count = gpuWindow.count;
    snapshot.gpu_query_window_capacity =
        editor_profiler::kGpuQueryWindowQueries;
    snapshot.gpu_frame_average_ms = gpuWindow.frame_average_ms;
    snapshot.opaque_gpu_average_ms = gpuWindow.opaque_average_ms;
    snapshot.atmosphere_gpu_average_ms =
        gpuWindow.atmosphere_average_ms;
    snapshot.cloud_gpu_average_ms = gpuWindow.cloud_average_ms;
    snapshot.fog_gpu_average_ms = gpuWindow.fog_average_ms;
    snapshot.post_gpu_average_ms = gpuWindow.post_average_ms;
    snapshot.opaque_gpu_window_peak_ms = gpuWindow.opaque_peak_ms;
    snapshot.atmosphere_gpu_window_peak_ms =
        gpuWindow.atmosphere_peak_ms;
    snapshot.cloud_gpu_window_peak_ms = gpuWindow.cloud_peak_ms;
    snapshot.fog_gpu_window_peak_ms = gpuWindow.fog_peak_ms;
    snapshot.post_gpu_window_peak_ms = gpuWindow.post_peak_ms;

    if (IRhiSwapchain* swapchain = host.renderer.Swapchain();
        swapchain != nullptr) {
        snapshot.viewport_width = swapchain->Width();
        snapshot.viewport_height = swapchain->Height();
    } else {
        snapshot.viewport_width = host.width;
        snapshot.viewport_height = host.height;
    }

    snapshot.cloud_width = 0u;
    snapshot.cloud_height = 0u;
    snapshot.cloud_march_steps = 0u;
    snapshot.cloud_light_steps = 0u;
    snapshot.cloud_render_scale = 0.0f;
    if (host.profiler_work.clouds_active) {
        const FVolumetricCloudTraceResolution trace =
            ResolveVolumetricCloudTraceResolution(
                snapshot.viewport_width,
                snapshot.viewport_height,
                host.q_cloud_render_scale);
        snapshot.cloud_width = trace.width;
        snapshot.cloud_height = trace.height;
        snapshot.cloud_march_steps = 192u;
        snapshot.cloud_light_steps = 8u;
        snapshot.cloud_render_scale = trace.effective_dimension_scale;
    }
    PublishCloudWorkloadSnapshot(host, snapshot.frame_index);
}

static void CommitEditorFrameDelta(
    FEditorHost& host, f32 safe_dt) noexcept {
    host.frame_dt = safe_dt > 1e-4f
        ? std::clamp(safe_dt, 1.0f / 240.0f, 0.10f)
        : (1.0f / 60.0f);
    if (host.water3d_ready) {
        // Advance once per submitted editor frame, independently of 2D/3D
        // view selection or refraction availability. Backpressured attempts
        // must not consume ripple lifetime ahead of the rest of simulation.
        host.water3d.Update(safe_dt);
    }
}

static int RenderEditorFrame(
    void* handle, float dt, bool avoid_gpu_wait) noexcept {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || !host->attached)
        return editor_frame::ToAbi(editor_frame::EResult::Fatal);
    if (!host->renderer.IsOperational())
        return editor_frame::ToAbi(editor_frame::EResult::Fatal);
    const f32 safe_dt = std::isfinite(dt) && dt > 0.0f
        ? std::clamp(dt, 0.0f, 0.10f)
        : 0.0f;
    // Do not enter BeginFrame until all startup upload phases are complete.
    // Each call performs one bounded phase and then returns to the Win32/WPF
    // message pump, keeping the editor responsive throughout shader warm-up.
    if (!host->startup_ready) {
        (void)AdvanceEditorStartup(*host);
        // A phase may be pending, may have failed non-fatally, or may just have
        // resized the swapchain. Presenting here keeps the child HWND at a
        // deterministic color until the complete scene stack is publishable.
        const int present_result = PresentNeutralEditorFrame(
            *host, false, avoid_gpu_wait);
        if (present_result <= 0) return present_result;
        CommitEditorFrameDelta(*host, safe_dt);
        return editor_frame::ToAbi(editor_frame::EResult::Presented);
    }
    // Raw DX12 normally waits for the next allocator slot inside Submit.
    // The editor's cooperative entry point preflights that fence instead, so
    // expensive scene preparation and simulation are not repeated while the
    // GPU owns both frame slots.
    if (avoid_gpu_wait &&
        !host->renderer.CanBeginFrameWithoutGpuWait()) {
        return editor_frame::ToAbi(editor_frame::EResult::Busy);
    }
    const editor_profiler::FTimePoint nativeRenderActiveBegin =
        editor_profiler::CClock::now();
    {
        // Reuse the host-owned DFS scratch retained by DrawScene3D. Clearing a
        // TArray preserves capacity, eliminating the per-frame heap churn that
        // the former local water_nodes array caused in both 2D and 3D views.
        TArray<game::ANode*>& water_nodes = host->scene_mesh_nodes;
        water_nodes.Reset();
        Dfs3DCollect(&host->scene3d.Root(), water_nodes);
        // Resource commits happen before BeginFrame, but the authored scene
        // remains visible through every slice via the opaque PBR fallback.
        // Once the final slice/background succeeds, the same frame may switch
        // to specialized water without an intervening neutral flash.
        (void)AdvanceWater3DInitialization(*host, water_nodes);
        (void)EnsureWater3DBackgroundBeforeFrame(
            *host, water_nodes);
    }
    const editor_profiler::FTimePoint profilerFrameBegin =
        editor_profiler::CClock::now();
    BeginProfilerFrame(*host, profilerFrameBegin);
    if (host->scene_presentation_suppressed) {
        // Keep presenting a deterministic neutral frame so WPF's HwndHost airspace cannot
        // expose the previous/default scene while managed file I/O is in flight.
        const editor_profiler::FTimePoint submitBegin =
            editor_profiler::CClock::now();
        const f32 nativeRenderActiveMs =
            editor_profiler::ElapsedMilliseconds(
                nativeRenderActiveBegin);
        const int present_result = PresentNeutralEditorFrame(
            *host, true, avoid_gpu_wait);
        if (present_result <= 0) return present_result;
        PublishProfilerFrame(
            *host, profilerFrameBegin,
            editor_profiler::ElapsedMilliseconds(submitBegin),
            nativeRenderActiveMs);
        CommitEditorFrameDelta(*host, safe_dt);
        return editor_frame::ToAbi(editor_frame::EResult::Presented);
    }

    // MSAA サンプル数の変更を適用する (PSO はサンプル数を焼き込むためバッチごと再生成)。
    if (host->msaa_pending != host->msaa_samples && host->renderer.Device() != nullptr) {
        if (!host->resource_mutation_idle) {
            host->renderer.Device()->WaitIdle();
            host->resource_mutation_idle = true;
        }
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

    const FClearColor clear = host->clear_color;   // Rendering.FClearColor (プロジェクト設定)
    if (IRhiCommandList* commandList = host->renderer.CommandList();
        commandList != nullptr) {
        commandList->ResetStatistics();
    }
    if (avoid_gpu_wait) {
        // The cooperative preflight above and this begin run on the same
        // HWND/RHI owner thread. A false result cannot be a frame-slot race;
        // classify it as fatal so the managed pump does not spin forever.
        if (!host->renderer.TryBeginFrameWithoutGpuWait(clear))
            return editor_frame::ToAbi(editor_frame::EResult::Fatal);
    } else {
        host->renderer.BeginFrame(clear);
    }
    CommitEditorFrameDelta(*host, safe_dt);
    host->time += safe_dt;

    if (host->play_state == 1) EditorStepPlay(*host, safe_dt);   // 再生中は物理を進める
    if (host->logic_play)      EditorTickLogic(*host, safe_dt);  // インプロセス Play: ユーザーロジックを進める
    if (host->preview_live && host->root.Get() != nullptr) {   // Preview: エンジンコンポーネントの実 OnUpdate
        const f32 cdt = safe_dt > 0.05f ? 0.05f : safe_dt;
        host->root->UpdateTree(cdt);
        host->root->ResolveStructuralChanges();
    }
    if (IRhiCommandList* commandList = host->renderer.CommandList();
        commandList != nullptr) {
        commandList->BeginGpuTimingFrame(
            host->profiler_snapshot.frame_index + 1u);
    }

    // --- 3D ビューポート: backbuffer に直接描く (BeginFrame が深度バッファを bind 済み)。
    //     2D の AA (オフスクリーン RT) 経路は深度を持たないため経由しない。
    if (host->view3d) {
        IRhiCommandList* cl = host->renderer.CommandList();
        IRhiSwapchain*   sc = host->renderer.Swapchain();
        if (cl != nullptr && sc != nullptr) DrawScene3D(*host, sc->Width(), sc->Height());
        if (cl != nullptr) cl->EndGpuTimingFrame();
        const editor_profiler::FTimePoint submitBegin =
            editor_profiler::CClock::now();
        const f32 nativeRenderActiveMs =
            editor_profiler::ElapsedMilliseconds(
                nativeRenderActiveBegin);
        const int present_result =
            SubmitAndPresentEditorFrame(*host, avoid_gpu_wait);
        if (!editor_frame::ShouldPublishProfiler(present_result))
            return present_result;
        PublishProfilerFrame(
            *host, profilerFrameBegin,
            editor_profiler::ElapsedMilliseconds(submitBegin),
            nativeRenderActiveMs);
        return editor_frame::ToAbi(editor_frame::EResult::Presented);
    }

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
            const f32 editorCamPanX = host->cam_pan_x;
            const f32 editorCamPanY = host->cam_pan_y;
            const f32 editorCamZoom = host->cam_zoom;
            const bool useGameCamera = host->game_view;
            if (useGameCamera) {
                if (host->logic_play && host->logic_cam_following) {
                    host->cam_pan_x = host->logic_game_pan_x;
                    host->cam_pan_y = host->logic_game_pan_y;
                    host->cam_zoom = host->logic_game_zoom;
                } else {
                    const FDeterministicGameCamera2D game_camera =
                        ResolveDeterministicGameCamera2D(
                            *host, scW, scH);
                    host->cam_pan_x = game_camera.pan_x;
                    host->cam_pan_y = game_camera.pan_y;
                    host->cam_zoom = game_camera.zoom;
                }
            }
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
            if (useGameCamera) {
                host->cam_pan_x = editorCamPanX;
                host->cam_pan_y = editorCamPanY;
                host->cam_zoom = editorCamZoom;
            }
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

    if (IRhiCommandList* commandList = host->renderer.CommandList();
        commandList != nullptr) {
        commandList->EndGpuTimingFrame();
    }
    const editor_profiler::FTimePoint submitBegin =
        editor_profiler::CClock::now();
    const f32 nativeRenderActiveMs =
        editor_profiler::ElapsedMilliseconds(
            nativeRenderActiveBegin);
    const int present_result =
        SubmitAndPresentEditorFrame(*host, avoid_gpu_wait);
    if (!editor_frame::ShouldPublishProfiler(present_result))
        return present_result;
    PublishProfilerFrame(
        *host, profilerFrameBegin,
        editor_profiler::ElapsedMilliseconds(submitBegin),
        nativeRenderActiveMs);
    return editor_frame::ToAbi(editor_frame::EResult::Presented);
}

ACS_EDITOR_API void acs_editor_render(void* handle, float dt) {
    (void)RenderEditorFrame(handle, dt, false);
}

/**
 * Cooperative editor pump. Returns 1 after a submit/present, 0 when the GPU
 * still owns the current frame slot, and -1 for an invalid/unattached host.
 * RHI calls remain on the caller's existing owner thread.
 */
ACS_EDITOR_API int acs_editor_render_try(void* handle, float dt) {
    return RenderEditorFrame(handle, dt, true);
}

ACS_EDITOR_API int acs_editor_profiler_get(
    void* handle,
    editor_profiler::FSnapshot* outSnapshot,
    uint32_t outSize) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || outSnapshot == nullptr ||
        outSize < sizeof(u32) * 2u) {
        return 0;
    }

    u32 requestedHeader[2]{};
    std::memcpy(
        requestedHeader,
        outSnapshot,
        sizeof(requestedHeader));
    const u32 requestedVersion = requestedHeader[0];
    const u32 requestedSize = requestedHeader[1];
    if (requestedVersion ==
            editor_profiler::kLegacySnapshotVersion) {
        if (outSize < editor_profiler::kLegacySnapshotSize ||
            requestedSize <
                editor_profiler::kLegacySnapshotSize) {
            return 0;
        }
        std::memcpy(
            outSnapshot,
            &host->profiler_snapshot,
            editor_profiler::kLegacySnapshotSize);
        outSnapshot->version =
            editor_profiler::kLegacySnapshotVersion;
        outSnapshot->struct_size =
            editor_profiler::kLegacySnapshotSize;
        return 1;
    }
    if (requestedVersion != editor_profiler::kSnapshotVersion ||
        outSize < editor_profiler::kSnapshotSize ||
        requestedSize < editor_profiler::kSnapshotSize) {
        return 0;
    }
    std::memcpy(
        outSnapshot,
        &host->profiler_snapshot,
        editor_profiler::kSnapshotSize);
    return 1;
}

/**
 * Optional exact cloud-workload snapshot.
 *
 * Returns 1 for a published RenderCompute attempt, 0 while the renderer/cloud
 * runtime is not ready or inactive, and -1 for an invalid ABI call. The
 * unavailable snapshot is still initialized so a negotiated caller can
 * distinguish runtime state from stale payload.
 */
ACS_EDITOR_API int acs_editor_cloud_workload_get(
    void* handle,
    editor_cloud_workload::FSnapshot* outSnapshot,
    uint32_t outSize) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || outSnapshot == nullptr ||
        outSize < editor_cloud_workload::kSnapshotSize ||
        outSnapshot->version !=
            editor_cloud_workload::kSnapshotVersion ||
        outSnapshot->struct_size <
            editor_cloud_workload::kSnapshotSize) {
        return -1;
    }

    const editor_cloud_workload::FSnapshot& snapshot =
        host->cloud_workload_snapshot;
    std::memcpy(
        outSnapshot,
        &snapshot,
        editor_cloud_workload::kSnapshotSize);
    return host->cloud_workload_available ? 1 : 0;
}

ACS_EDITOR_API void acs_editor_profiler_reset_peaks(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return;
    const u64 lastGpuFrameIndex =
        host->profiler_snapshot.gpu_frame_index;
    host->profiler_cpu_peak.Reset();
    host->profiler_gpu_peak.Reset();
    host->profiler_active_cpu_peak.Reset();
    host->profiler_present_cpu_peak.Reset();
    host->profiler_gpu_queries.Reset(
        lastGpuFrameIndex);
    host->profiler_last_gpu_peak_frame =
        lastGpuFrameIndex;
    host->profiler_presented_since_reset = 0u;
    ++host->profiler_reset_serial;
    host->profiler_work = {};
    host->profiler_smoothed_fps = 0.0f;
    host->profiler_last_frame_begin = {};
    host->profiler_has_previous_frame = false;
    host->cloud_workload_snapshot = {};
    host->cloud_workload_available = false;
    host->profiler_snapshot.timing_source = static_cast<u32>(
        editor_profiler::ETimingSource::CpuRecordSubmit);
    host->profiler_snapshot.flags &=
        ~static_cast<u32>(editor_profiler::GpuTimingsValid);
    host->profiler_snapshot.fps = 0.0f;
    host->profiler_snapshot.cpu_frame_ms = 0.0f;
    host->profiler_snapshot.cpu_submit_ms = 0.0f;
    host->profiler_snapshot.gpu_frame_ms = -1.0f;
    host->profiler_snapshot.opaque_cpu_ms = 0.0f;
    host->profiler_snapshot.atmosphere_cpu_ms = 0.0f;
    host->profiler_snapshot.cloud_cpu_ms = 0.0f;
    host->profiler_snapshot.fog_cpu_ms = 0.0f;
    host->profiler_snapshot.post_cpu_ms = 0.0f;
    host->profiler_snapshot.opaque_gpu_ms = -1.0f;
    host->profiler_snapshot.atmosphere_gpu_ms = -1.0f;
    host->profiler_snapshot.cloud_gpu_ms = -1.0f;
    host->profiler_snapshot.fog_gpu_ms = -1.0f;
    host->profiler_snapshot.post_gpu_ms = -1.0f;
    host->profiler_snapshot.gpu_frame_index = 0u;
    host->profiler_snapshot.gpu_latency_frames = 0u;
    host->profiler_snapshot.cpu_frame_peak_ms = 0.0f;
    host->profiler_snapshot.gpu_frame_peak_ms = -1.0f;
    host->profiler_snapshot.gpu_query_window_count = 0u;
    host->profiler_snapshot.gpu_query_window_capacity =
        editor_profiler::kGpuQueryWindowQueries;
    host->profiler_snapshot.gpu_frame_average_ms = -1.0f;
    host->profiler_snapshot.opaque_gpu_average_ms = -1.0f;
    host->profiler_snapshot.atmosphere_gpu_average_ms = -1.0f;
    host->profiler_snapshot.cloud_gpu_average_ms = -1.0f;
    host->profiler_snapshot.fog_gpu_average_ms = -1.0f;
    host->profiler_snapshot.post_gpu_average_ms = -1.0f;
    host->profiler_snapshot.opaque_gpu_window_peak_ms = -1.0f;
    host->profiler_snapshot.atmosphere_gpu_window_peak_ms = -1.0f;
    host->profiler_snapshot.cloud_gpu_window_peak_ms = -1.0f;
    host->profiler_snapshot.fog_gpu_window_peak_ms = -1.0f;
    host->profiler_snapshot.post_gpu_window_peak_ms = -1.0f;
    host->profiler_snapshot.native_render_active_cpu_ms = 0.0f;
    host->profiler_snapshot.native_present_cpu_ms = 0.0f;
    host->profiler_snapshot.native_render_active_cpu_peak_ms = 0.0f;
    host->profiler_snapshot.native_present_cpu_peak_ms = 0.0f;
    host->profiler_snapshot.presented_frame_count_since_reset = 0u;
    host->profiler_snapshot.profiler_reset_serial =
        host->profiler_reset_serial;
}

ACS_EDITOR_API int acs_editor_resize(void* handle, uint32_t width, uint32_t height) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || !host->attached ||
        width == 0u || height == 0u) {
        return 0;
    }
    // The managed host coalesces a Win32 move/size interaction to its stable
    // final dimensions. CRenderer owns exactly one WaitIdle; backend Resize
    // implementations only recreate buffers.
    if (!host->renderer.OnResize(width, height)) return 0;
    host->resource_mutation_idle = true;
    host->width  = width;
    host->height = height;
    host->profiler_snapshot.viewport_width = width;
    host->profiler_snapshot.viewport_height = height;
    host->water3d_background_failed = false;
    host->water3d_depth_copy_failed = false;
    if (!host->startup_ready ||
        host->scene_presentation_suppressed) {
        if (PresentNeutralEditorFrame(*host, false, false) <= 0) return 0;
    }
    return 1;
}

// =============================================================================
// C ABI — プロジェクト設定
// =============================================================================

/** プロジェクト設定をエンジン状態へ反映する (ロード/変更時に呼ぶ)。 */
/** Rendering/QualityLevel プリセット → 各描画ノブ (h.q_*) を埋める。未知文字列は High にフォールバック。
 *  knob 値表は «超最高/最高/高/中/低/最低» を DX12 Samples 流の品質階層で設定。 */
static void ApplyQualityPreset(FEditorHost& h, const char* level) noexcept {
    if (level == nullptr) level = "High";
    auto eq = [&](const char* s){ return std::strcmp(level, s) == 0; };
    h.q_cloud_render_scale = 0.75f;   // High / Highest: 75% of the internal trace policy
    if (eq("Ultra")) {
        h.q_cloud_render_scale=1.0f;  // complete internal trace policy for final/editor review
        h.q_shadow_size=4096; h.q_shadow_cascades=4; h.q_shadow_bias=0.0010f; h.q_shadow_filter=2.0f;
        h.q_ssao_on=true;  h.q_ssao_intensity=1.2f; h.q_ssao_radius=1.5f;
        h.q_ssgi_on=true;  h.q_ssgi_intensity=1.2f; h.q_ssgi_max_dist=15.0f;
        h.q_ssr_on=true;   h.q_ssr_intensity=1.0f;  h.q_ssr_hiz=true;   h.q_ibl_mode=2;
        h.q_bloom_on=true; h.q_bloom_intensity=0.55f; h.q_bloom_threshold=0.70f; h.q_bloom_radius=2.0f;
        h.q_cg_saturation=1.10f; h.q_cg_contrast=1.15f; h.q_cas=0.6f; h.q_taa_on=true;  h.q_msaa_default=4;
    } else if (eq("Highest")) {
        h.q_shadow_size=4096; h.q_shadow_cascades=3; h.q_shadow_bias=0.0012f; h.q_shadow_filter=1.5f;
        h.q_ssao_on=true;  h.q_ssao_intensity=1.0f; h.q_ssao_radius=1.0f;
        h.q_ssgi_on=true;  h.q_ssgi_intensity=1.0f; h.q_ssgi_max_dist=10.0f;
        h.q_ssr_on=true;   h.q_ssr_intensity=1.0f;  h.q_ssr_hiz=true;   h.q_ibl_mode=2;
        h.q_bloom_on=true; h.q_bloom_intensity=0.50f; h.q_bloom_threshold=0.80f; h.q_bloom_radius=1.5f;
        h.q_cg_saturation=1.10f; h.q_cg_contrast=1.12f; h.q_cas=0.4f; h.q_taa_on=true;  h.q_msaa_default=4;
    } else if (eq("Medium")) {
        h.q_cloud_render_scale=0.50f;
        h.q_shadow_size=2048; h.q_shadow_cascades=1; h.q_shadow_bias=0.0015f; h.q_shadow_filter=1.0f;
        h.q_ssao_on=true;  h.q_ssao_intensity=0.9f; h.q_ssao_radius=0.6f;
        h.q_ssgi_on=false; h.q_ssr_on=false; h.q_ibl_mode=1;
        h.q_bloom_on=true; h.q_bloom_intensity=0.40f; h.q_bloom_threshold=0.90f; h.q_bloom_radius=1.2f;
        h.q_cg_saturation=1.05f; h.q_cg_contrast=1.08f; h.q_cas=0.3f; h.q_taa_on=true;  h.q_msaa_default=2;  // Medium も AA 一貫性のため TAA on
    } else if (eq("Low")) {
        h.q_cloud_render_scale=0.50f;
        h.q_shadow_size=1024; h.q_shadow_cascades=1; h.q_shadow_bias=0.0020f; h.q_shadow_filter=0.0f;
        h.q_ssao_on=false; h.q_ssgi_on=false; h.q_ssr_on=false;
        h.q_ibl_mode=0; h.q_ambient=FVec3{0.20f,0.22f,0.26f};
        h.q_bloom_on=true; h.q_bloom_intensity=0.30f; h.q_bloom_threshold=1.00f; h.q_bloom_radius=1.0f;
        h.q_cg_saturation=1.00f; h.q_cg_contrast=1.00f; h.q_cas=0.0f; h.q_taa_on=false; h.q_msaa_default=2;
    } else if (eq("Lowest")) {
        h.q_cloud_render_scale=0.35f;
        h.q_shadow_size=0; h.q_shadow_cascades=0;
        h.q_ssao_on=false; h.q_ssgi_on=false; h.q_ssr_on=false;
        h.q_ibl_mode=0; h.q_ambient=FVec3{0.26f,0.28f,0.33f};
        h.q_bloom_on=false;
        h.q_cg_saturation=1.00f; h.q_cg_contrast=1.00f; h.q_cas=0.0f; h.q_taa_on=false; h.q_msaa_default=1;
    } else {  // High (既定。CSM は Ultra/Highest のみ — 既定は実績ある single cascade を維持)
        h.q_shadow_size=2048; h.q_shadow_cascades=1; h.q_shadow_bias=0.0015f; h.q_shadow_filter=1.0f;
        h.q_ssao_on=true;  h.q_ssao_intensity=1.0f; h.q_ssao_radius=0.8f;
        h.q_ssgi_on=false; h.q_ssr_on=true; h.q_ssr_intensity=0.8f; h.q_ssr_hiz=false; h.q_ibl_mode=2;
        h.q_bloom_on=true; h.q_bloom_intensity=0.50f; h.q_bloom_threshold=0.80f; h.q_bloom_radius=1.5f;
        h.q_cg_saturation=1.10f; h.q_cg_contrast=1.12f; h.q_cas=0.3f; h.q_taa_on=true;  h.q_msaa_default=4;  // 既定で TAA = 3D ビューポートのジャギー解消
    }
}

/** 現在の品質プリセットが要求する影マップ解像度 (0=影オフ)。設定の反映確認/UI 表示用。 */
ACS_EDITOR_API int acs_editor_quality_shadow_size(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr) ? static_cast<int>(host->q_shadow_size) : 0;
}
/** 現在の品質プリセットの bloom 強度 (0=bloom オフ)。100 倍した整数で返す (例 0.55→55)。 */
ACS_EDITOR_API int acs_editor_quality_bloom_x100(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr && host->q_bloom_on) ? static_cast<int>(host->q_bloom_intensity * 100.0f + 0.5f) : 0;
}
/** 現在の露出 (Rendering/Exposure)。100 倍した整数で返す (例 1.05→105)。設定反映の確認用。 */
ACS_EDITOR_API int acs_editor_quality_exposure_x100(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr) ? static_cast<int>(host->q_exposure * 100.0f + 0.5f) : 0;
}
/** 現在の品質プリセットの影カスケード数 (1=single、>=2=CSM、0=影オフ)。設定反映の確認用。 */
ACS_EDITOR_API int acs_editor_quality_shadow_cascades(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr) ? static_cast<int>(host->q_shadow_cascades) : 0;
}
/** 現在の SSAO 強度 ×100 (0=SSAO オフ)。設定反映の確認用。 */
ACS_EDITOR_API int acs_editor_quality_ssao_x100(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr && host->q_ssao_on) ? static_cast<int>(host->q_ssao_intensity * 100.0f + 0.5f) : 0;
}
/** 現在の SSR 強度 ×100 (0=SSR オフ)。設定反映の確認用。 */
ACS_EDITOR_API int acs_editor_quality_ssr_x100(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr && host->q_ssr_on) ? static_cast<int>(host->q_ssr_intensity * 100.0f + 0.5f) : 0;
}
/** 現在の SSGI 強度 ×100 (0=SSGI オフ)。設定反映の確認用。 */
ACS_EDITOR_API int acs_editor_quality_ssgi_x100(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr && host->q_ssgi_on) ? static_cast<int>(host->q_ssgi_intensity * 100.0f + 0.5f) : 0;
}
/** TAA (テンポラル AA) が有効か (1/0)。設定反映の確認用。 */
ACS_EDITOR_API int acs_editor_quality_taa(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr && host->q_taa_on) ? 1 : 0;
}
/** 現在のトーンマッパー (0=ACES 1=AgX 2=Reinhard)。設定反映の確認用。 */
ACS_EDITOR_API int acs_editor_quality_tonemap(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr) ? host->q_tonemap : 0;
}
/** auto-exposure が有効か (1/0)。設定反映の確認用。 */
ACS_EDITOR_API int acs_editor_quality_auto_exposure(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr && host->q_auto_exposure) ? 1 : 0;
}
/** フォグ密度 ×1000 (0=オフ)。設定反映の確認用。 */
ACS_EDITOR_API int acs_editor_quality_fog_x1000(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr && host->q_fog_on) ? static_cast<int>(host->q_fog_density * 1000.0f + 0.5f) : 0;
}
/** 空モード (0=CSky / 1=物理大気)。設定反映の確認用。 */
ACS_EDITOR_API int acs_editor_quality_sky_mode(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr) ? host->q_sky_mode : 0;
}
/** 雲の coverage ×100 (0=雲オフ)。設定反映の確認用。 */
ACS_EDITOR_API int acs_editor_quality_cloud_x100(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr) ? static_cast<int>(host->q_cloud_coverage * 100.0f + 0.5f) : 0;
}
/** DoF (被写界深度) の焦点距離 ×100 (0=オフ)。設定反映の確認用。 */
ACS_EDITOR_API int acs_editor_quality_dof_x100(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr && host->q_dof_on) ? static_cast<int>(host->q_dof_focus * 100.0f + 0.5f) : 0;
}
/** god rays (光芒) の強度 ×100 (0=オフ)。設定反映の確認用。 */
ACS_EDITOR_API int acs_editor_quality_godray_x100(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr && host->q_godray_on) ? static_cast<int>(host->q_godray_intensity * 100.0f + 0.5f) : 0;
}
/** シネマフィルタ ×100 (which: 0=vignette 1=chromatic 2=grain)。設定反映の確認用。 */
ACS_EDITOR_API int acs_editor_quality_cine_x100(void* handle, int which) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return 0;
    const f32 v = (which == 0) ? host->q_vignette : (which == 1) ? host->q_chromatic : host->q_grain;
    return static_cast<int>(v * 100.0f + 0.5f);
}
/** モーションブラー強度 ×100 (0=オフ)。設定反映の確認用。 */
ACS_EDITOR_API int acs_editor_quality_motionblur_x100(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr && host->q_motionblur_on) ? static_cast<int>(host->q_motionblur_intensity * 100.0f + 0.5f) : 0;
}
/** 太陽 (主光源) 方向 «光へ向かう» 単位ベクトルを out3 (x,y,z) へ。設定反映の確認用。 */
ACS_EDITOR_API void acs_editor_sun_direction(void* handle, float* out3) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || out3 == nullptr) return;
    out3[0] = host->sun_dir.x; out3[1] = host->sun_dir.y; out3[2] = host->sun_dir.z;
}
/** 実効ライト色 (sun_color × sun_intensity) を out3 へ。設定反映の確認用。 */
ACS_EDITOR_API void acs_editor_sun_light_color(void* handle, float* out3) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || out3 == nullptr) return;
    out3[0] = host->sun_color.x * host->sun_intensity;
    out3[1] = host->sun_color.y * host->sun_intensity;
    out3[2] = host->sun_color.z * host->sun_intensity;
}
/** 3D ビューポートのグリッド表示を切替える (清書/スクショ用)。 */
ACS_EDITOR_API void acs_editor_set_show_grid3d(void* handle, int on) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host != nullptr) host->show_grid3d = (on != 0);
}
/** 3D グリッド表示状態 (1=表示)。 */
ACS_EDITOR_API int acs_editor_get_show_grid3d(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr && host->show_grid3d) ? 1 : 0;
}

/** 空グラデ色 (zenith3 + horizon3 + ground3) を out9 へ。設定反映の確認用。 */
ACS_EDITOR_API void acs_editor_sky_colors(void* handle, float* out9) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || out9 == nullptr) return;
    out9[0]=host->sky_zenith.x;  out9[1]=host->sky_zenith.y;  out9[2]=host->sky_zenith.z;
    out9[3]=host->sky_horizon.x; out9[4]=host->sky_horizon.y; out9[5]=host->sky_horizon.z;
    out9[6]=host->sky_ground.x;  out9[7]=host->sky_ground.y;  out9[8]=host->sky_ground.z;
}

static void ApplySettings(FEditorHost& h) noexcept {
    const bool taa_was_requested = h.q_taa_on;
    const bool ssr_was_requested = h.q_ssr_on;
    const bool ssgi_was_requested = h.q_ssgi_on;
    const bool auto_exposure_was_requested = h.q_auto_exposure;
    ApplyQualityPreset(h, h.settings.GetString("Rendering", "QualityLevel", "High"));   // 先に品質プリセットを展開
    // 個別キーはプリセットより «優先» (上書き)。露出はプリセット非依存なので常に設定値、
    // bloom/影バイアスは -1 でプリセット追従・>=0 で上書き (ProjectSettings の設計コメント通り)。
    h.q_exposure = h.settings.GetFloat("Rendering", "Exposure", 1.05f);
    const f32 bi = h.settings.GetFloat("Rendering", "BloomIntensity", -1.0f);
    if (bi >= 0.0f) { h.q_bloom_intensity = bi; h.q_bloom_on = (bi > 0.0f); }
    const f32 sb = h.settings.GetFloat("Rendering", "ShadowBias", -1.0f);
    if (sb >= 0.0f) h.q_shadow_bias = sb;
    const f32 si = h.settings.GetFloat("Rendering", "SsaoIntensity", -1.0f);
    if (si >= 0.0f) { h.q_ssao_intensity = si; h.q_ssao_on = (si > 0.0f); }
    const f32 ri = h.settings.GetFloat("Rendering", "SsrIntensity", -1.0f);
    if (ri >= 0.0f) { h.q_ssr_intensity = ri; h.q_ssr_on = (ri > 0.0f); }
    const f32 gi = h.settings.GetFloat("Rendering", "SsgiIntensity", -1.0f);
    if (gi >= 0.0f) { h.q_ssgi_intensity = gi; h.q_ssgi_on = (gi > 0.0f); }
    h.q_vxgi_on = (h.settings.GetInt("Rendering", "VxgiOn", 0) > 0);   // 既定OFF (64³ blocky)。VxgiOn=1 で実験的に voxel GI 有効化
    h.q_ap_on   = (h.settings.GetInt("Rendering", "AerialPerspective", 0) > 0);   // 既定OFF (小シーンで froxel 段)。大スケール屋外シーンで AerialPerspective=1
    const f32 taa = h.settings.GetFloat("Rendering", "Taa", -1.0f);
    if (taa >= 0.0f) h.q_taa_on = (taa > 0.0f);   // -1=プリセット追従 / 0=オフ / >0=オン (テンポラル AA)
    const i32 tm = h.settings.GetInt("Rendering", "Tonemap", 0);
    h.q_tonemap = (tm >= 0 && tm <= 2) ? tm : 0;  // 0=ACES 1=AgX 2=Reinhard
    h.q_auto_exposure = (h.settings.GetFloat("Rendering", "AutoExposure", 0.0f) > 0.0f);
    const f32 fogd = h.settings.GetFloat("Rendering", "FogDensity", 0.0f);   // 0/未指定=オフ / >0=密度 (単一色)
    h.q_fog_on = (fogd > 0.0f); if (h.q_fog_on) h.q_fog_density = fogd;
    const i32 sm = h.settings.GetInt("Rendering", "SkyMode", 0);
    const i32 smc = (sm == 1) ? 1 : 0;
    if (smc != h.q_sky_mode) { h.q_sky_mode = smc; h.ibl_dirty = true; h.ibl_tried = false; }   // モード変更 → env 再焼成
    h.q_cloud_coverage = h.settings.GetFloat("Rendering", "CloudCoverage", 0.50f);   // 0=雲オフ / 0.1〜1.0
    h.q_cloud_density  = h.settings.GetFloat("Rendering", "CloudDensity",  1.6f);
    h.q_cloud_wind     = h.settings.GetFloat("Rendering", "CloudWind",     1.0f);
    h.q_cloud_base     = h.settings.GetFloat("Rendering", "CloudBaseHeight", 1500.0f);
    h.q_cloud_top      = h.settings.GetFloat("Rendering", "CloudTopHeight", 4000.0f);
    h.q_cloud_noise_scale = h.settings.GetFloat("Rendering", "CloudNoiseScale", 0.035f);
    const f32 cloudRenderScale =
        h.settings.GetFloat("Rendering", "CloudRenderScale", -1.0f);
    if (cloudRenderScale >= 0.0f) {
        h.q_cloud_render_scale =
            cloudRenderScale < 0.50f ? 0.50f
          : cloudRenderScale > 1.0f  ? 1.0f
                                     : cloudRenderScale;
    }
    const f32 gray = h.settings.GetFloat("Rendering", "GodRays", 0.0f);
    h.q_godray_on = (gray > 0.0f); if (h.q_godray_on) h.q_godray_intensity = gray;   // 0=オフ / >0=光芒の強度
    h.q_vignette  = h.settings.GetFloat("Rendering", "Vignette", 0.0f);              // シネマフィルタ (0=オフ)
    h.q_chromatic = h.settings.GetFloat("Rendering", "ChromaticAberration", 0.0f);
    h.q_grain     = h.settings.GetFloat("Rendering", "FilmGrain", 0.0f);
    const f32 mbl = h.settings.GetFloat("Rendering", "MotionBlur", 0.0f);
    h.q_motionblur_on = (mbl > 0.0f); if (h.q_motionblur_on) h.q_motionblur_intensity = mbl;   // 0=オフ / >0=強度
    const f32 dofF = h.settings.GetFloat("Rendering", "DofFocus", 0.0f);
    h.q_dof_on = (dofF > 0.0f);                    // 0=オフ / >0=焦点距離 (カメラからの view-space z)
    if (h.q_dof_on) {
        h.q_dof_focus = dofF;
        h.q_dof_range = h.settings.GetFloat("Rendering", "DofRange", 5.0f);
        h.q_dof_max   = h.settings.GetFloat("Rendering", "DofMax",   0.010f);
    }
    // 太陽 (主光源) 方向 = 方位角/仰角 (度) → «光へ向かう» 単位ベクトル。影/陰影/空が一括で追従。
    {
        const f32 az = h.settings.GetFloat("Rendering", "SunAzimuth",   -41.0f) * 3.14159265f / 180.0f;
        const f32 el = h.settings.GetFloat("Rendering", "SunElevation",  58.0f) * 3.14159265f / 180.0f;
        const f32 ce = std::cos(el);
        FVec3 d{ ce * std::cos(az), std::sin(el), ce * std::sin(az) };
        const f32 len = std::sqrt(d.x*d.x + d.y*d.y + d.z*d.z);
        if (len > 1e-4f) { d.x /= len; d.y /= len; d.z /= len; }
        h.sun_dir = d;
    }
    h.sun_color     = h.settings.GetColor("Rendering", "SunColor", FVec3{ 1.0f, 0.95f, 0.85f });
    h.sun_intensity = h.settings.GetFloat("Rendering", "SunIntensity", 2.35f);
    h.sky_zenith    = h.settings.GetColor("Rendering", "SkyZenith",  FVec3{ 0.16f, 0.33f, 0.62f });
    h.sky_horizon   = h.settings.GetColor("Rendering", "SkyHorizon", FVec3{ 0.62f, 0.70f, 0.80f });
    h.sky_ground    = h.settings.GetColor("Rendering", "SkyGround",  FVec3{ 0.20f, 0.19f, 0.21f });
    h.ibl_dirty     = true;   // 太陽/空が変わった → IBL env cubemap を次フレームで再キャプチャ
    h.sh9_dirty     = true;   // 〃 → SH9 環境光(拡散+鏡面 fallback)も再計算
    const int msaa = h.settings.GetInt("Rendering", "MsaaSamples", 8);
    h.msaa_pending = (msaa >= 8) ? 8u : (msaa >= 4) ? 4u : (msaa >= 2) ? 2u : 1u;
    h.ambient      = h.settings.GetColor("Rendering", "AmbientColor", FVec3{ 0.10f, 0.11f, 0.13f });
    h.light_height = h.settings.GetFloat("Rendering", "LightHeight", 90.0f);
    const FVec3 cc = h.settings.GetColor("Rendering", "FClearColor", FVec3{ 0.07f, 0.08f, 0.10f });
    h.clear_color  = FClearColor{ cc.x, cc.y, cc.z, 1.0f };
    h.snap_move    = h.settings.GetFloat("Editor", "SnapMove", 10.0f);
    h.snap_rotate  = h.settings.GetFloat("Editor", "SnapRotateDeg", 15.0f) * 3.1415926535f / 180.0f;
    h.snap_scale   = h.settings.GetFloat("Editor", "SnapScale", 0.25f);
    if (taa_was_requested != h.q_taa_on ||
        ssr_was_requested != h.q_ssr_on ||
        ssgi_was_requested != h.q_ssgi_on ||
        auto_exposure_was_requested != h.q_auto_exposure) {
        // Settings may be toggled off and back on between presented frames.
        // Reset at mutation time so no skipped draw is required to make the
        // next enabled frame cold.
        InvalidateTemporalRenderHistories(h);
    }
    if (!ssgi_was_requested && h.q_ssgi_on && !h.ssgi_ready &&
        h.startup_worker_kind != 2u) {
        h.ssgi_init_tried = false;
    }
}

/** INI テキストからプロジェクト設定を読み込み、エンジンへ適用する (C# がファイル I/O 担当)。
 *  ini_text=null/空 は既定値で初期化 (初回起動)。 */
ACS_EDITOR_API void acs_editor_settings_load_text(void* handle, const char* ini_text) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return;
    if (ini_text != nullptr && ini_text[0] != '\0') host->settings.LoadText(ini_text);
    else                                            host->settings.ResetToDefaults();
    ApplySettings(*host);
}

/** プロジェクト設定を INI テキストへシリアライズする (C# が書き込む)。書いた文字数を返す。 */
ACS_EDITOR_API int acs_editor_settings_serialize(void* handle, char* out, int cap) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || out == nullptr || cap <= 0) return 0;
    return static_cast<int>(host->settings.SerializeText(out, static_cast<usize>(cap)));
}

/** 照明 (太陽 + 空) の «時間帯» プリセットを適用する。Sun/Sky/Exposure 設定を一括設定し即反映。
 *  既存のグラフィックス設定 (検証済み) を束ねた «1 クリック» 機能。既知名で 1、未知で 0。
 *  C# はこの後 INI を保存する。Noon=既定 / Sunset=低い暖色太陽+橙空 / Overcast=高位の寒色弱光+灰空 /
 *  Night=弱い青光+暗青空。値は «良い出発点»、ユーザーは個別設定で微調整できる。 */
ACS_EDITOR_API int acs_editor_apply_lighting_preset(void* handle, const char* name) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || name == nullptr) return 0;
    auto S = [&](const char* k, const char* v) { host->settings.Set("Rendering", k, v); };
    if (std::strcmp(name, "Noon") == 0) {
        S("SunAzimuth","-41"); S("SunElevation","58"); S("SunColor","1.0,0.95,0.85"); S("SunIntensity","2.35");
        S("SkyZenith","0.16,0.33,0.62"); S("SkyHorizon","0.62,0.70,0.80"); S("SkyGround","0.20,0.19,0.21"); S("Exposure","1.05");
    } else if (std::strcmp(name, "Sunset") == 0) {
        S("SunAzimuth","-85"); S("SunElevation","8");  S("SunColor","1.0,0.55,0.28"); S("SunIntensity","2.1");
        S("SkyZenith","0.30,0.22,0.42"); S("SkyHorizon","0.95,0.50,0.28"); S("SkyGround","0.16,0.11,0.10"); S("Exposure","1.15");
    } else if (std::strcmp(name, "Overcast") == 0) {
        S("SunAzimuth","-41"); S("SunElevation","68"); S("SunColor","0.85,0.88,0.92"); S("SunIntensity","1.5");
        S("SkyZenith","0.46,0.49,0.54"); S("SkyHorizon","0.58,0.60,0.63"); S("SkyGround","0.32,0.32,0.34"); S("Exposure","1.0");
    } else if (std::strcmp(name, "Night") == 0) {
        S("SunAzimuth","-50"); S("SunElevation","42"); S("SunColor","0.55,0.68,1.0"); S("SunIntensity","0.7");
        S("SkyZenith","0.03,0.05,0.13"); S("SkyHorizon","0.07,0.10,0.18"); S("SkyGround","0.02,0.02,0.05"); S("Exposure","1.25");
    } else {
        return 0;
    }
    ApplySettings(*host);
    return 1;
}

/** 設定エントリ数。 */
ACS_EDITOR_API int acs_editor_settings_count(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr) ? static_cast<int>(host->settings.Count()) : 0;
}

/** index 番目のエントリを TSV 1 行 "category\tkey\tvalue\ttype\toptions\tbuiltin\tdesc" で返す。
 *  type は ESettingType の整数。options/desc は無ければ空。 */
ACS_EDITOR_API int acs_editor_settings_entry(void* handle, int index, char* out, int cap) {
    auto* host = static_cast<FEditorHost*>(handle);
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
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return 0;
    if (!host->settings.Set(cat, key, value)) return 0;
    ApplySettings(*host);
    return 1;
}

/** ユーザー定義エントリを追加する (既存キーは値更新)。成功 1。 */
ACS_EDITOR_API int acs_editor_settings_add(void* handle, const char* cat, const char* key,
                                           const char* value) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return 0;
    const int ok = host->settings.Add(cat, key, value) ? 1 : 0;
    if (ok) ApplySettings(*host);
    return ok;
}

/** ユーザー定義エントリを削除する (ビルトインは不可)。成功 1。 */
ACS_EDITOR_API int acs_editor_settings_remove(void* handle, const char* cat, const char* key) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return 0;
    return host->settings.Remove(cat, key) ? 1 : 0;
}

/** 値を 1 つ取得する (ユーザーコード/エディタの個別参照用)。見つかれば 1。 */
ACS_EDITOR_API int acs_editor_settings_get_value(void* handle, const char* cat, const char* key,
                                                 char* out, int cap) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || out == nullptr || cap <= 0) return 0;
    const game::FSettingEntry* e = host->settings.Find(cat, key);
    if (e == nullptr) return 0;
    std::snprintf(out, static_cast<size_t>(cap), "%s", e->value);
    return 1;
}

/** MSAA サンプル数を設定する (1=FXAA のみ / 2 / 4 / 8)。次フレームの先頭で適用される。 */
ACS_EDITOR_API void acs_editor_set_msaa(void* handle, int samples) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return;
    u32 s = (samples >= 8) ? 8u : (samples >= 4) ? 4u : (samples >= 2) ? 2u : 1u;
    host->msaa_pending = s;
}

/** 現在の MSAA サンプル数を返す (フォールバック後の実効値)。 */
ACS_EDITOR_API int acs_editor_get_msaa(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr) ? static_cast<int>(host->msaa_samples) : 1;
}

// destroy より後ろで定義される実行状態の停止 API。GPU 解放前の巻き戻しに使う。
ACS_EDITOR_API int acs_editor_play_stop(void* handle);
ACS_EDITOR_API void acs_editor_preview_stop(void* handle);
ACS_EDITOR_API void acs_editor_logic_play_stop(void* handle);
ACS_EDITOR_API void acs_editor_clear_instances(void* handle);

ACS_EDITOR_API void acs_editor_destroy(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (!UnregisterEditorHost(host)) return;
    // The worker owns no RHI resources, but it writes its compiled bytecode
    // result into the host.  Join before any host, renderer, or DLL teardown.
    JoinStartupWorker(*host);
    host->startup_async_shader_kind = 0u;
    host->startup_sky_shaders = {};
    host->startup_pbr_shaders = {};
    host->startup_ssgi_shaders = {};
    host->startup_cloud_shaders = {};
    host->startup_ssao_shaders = {};
    host->startup_post_shaders = {};
    host->ssss3d_pending_shaders = {};
    // ユーザー DLL と実コンポーネントは GPU/基盤より先に破棄する。再生中の直接 destroy でも
    // DLL ハンドル、物理ワールド、開始時スナップショットを残さない。
    acs_editor_logic_play_stop(handle);
    acs_editor_preview_stop(handle);
    acs_editor_clear_instances(handle);
    (void)acs_editor_play_stop(handle);
    ClearStack(host->undo);   // undo/redo の heap スナップショットを解放
    ClearStack(host->redo);
    if (host->renderer.Device() != nullptr) host->renderer.Device()->WaitIdle();
    host->sprites.Shutdown();
    host->preview_sprites.Shutdown();
    host->fxaa.Shutdown();
    host->dbg3d.Shutdown();
    host->camera_frustum_dbg3d.Shutdown();
    // GPU 物理大気を device より先に終了して解放後参照を防ぐ。
    host->sky_atmo.Shutdown();
    host->vclouds3d.Shutdown();  // GPU volumetric clouds (UAF 防止)
    host->m3d_pipe.Reset(); host->m3d_overlay_pipe.Reset(); host->m3d_vs.Reset(); host->m3d_ps.Reset();
    host->sky_pipe.Reset(); host->sky_vs.Reset(); host->sky_ps.Reset(); host->sky_cb.Reset();
    host->grid_pipe.Reset(); host->grid_vs.Reset(); host->grid_ps.Reset(); host->grid_cb.Reset(); host->grid_vb.Reset();
    host->shadow_caster_pipe.Reset(); host->shadow_caster_vs.Reset(); host->shadow_lvp_cb.Reset();
    for (u32 c = 0; c < acs::CShadowMap::kMaxCascades; ++c) host->shadow_cascade_cb[c].Reset();
    host->shadow.Shutdown(); host->shadow_ready = false;
    host->normal_pipe.Reset(); host->normal_vs.Reset(); host->normal_ps.Reset(); host->normal_cb.Reset(); host->normal_rt.Reset(); host->normal_w = 0; host->normal_h = 0;
    host->ssao3d.Shutdown(); host->ssao_ready = false; host->ssao_pipe_ready = false; host->ssao_w = 0; host->ssao_h = 0;
    host->ssr3d.Shutdown(); host->ssr_ready = false; host->ssr_w = 0; host->ssr_h = 0;
    host->hiz3d.Shutdown(); host->hiz3d_ready = false; host->hiz3d_w = 0; host->hiz3d_h = 0;
    host->ssgi3d.Shutdown(); host->ssgi_ready = false; host->ssgi_w = 0; host->ssgi_h = 0;
    host->mv3d.Shutdown(); host->mv_ready = false; host->mv_w = 0; host->mv_h = 0;
    host->refr3d.Shutdown(); host->refr_ready = false;
    host->water3d.Shutdown();
    host->water3d_ready = false;
    host->water3d_pending_shaders = {};
    host->water3d_init_state = 0u;
    host->blit_pipe.Reset(); host->blit_vs.Reset(); host->blit_ps.Reset(); host->blit_ready = false;
    host->refr_bg.Reset(); host->refr_bg_w = 0; host->refr_bg_h = 0;
    host->water3d_depth_copy.Reset();
    host->water3d_depth_copy_w = host->water3d_depth_copy_h = 0u;
    host->water3d_depth_copy_failed = false;
    host->dof_pipe.Reset(); host->dof_vs.Reset(); host->dof_ps.Reset(); host->dof_cb.Reset(); host->dof_ready = false;
    host->gray_pipe.Reset(); host->gray_vs.Reset(); host->gray_ps.Reset(); host->gray_cb.Reset(); host->gray_ready = false;
    host->mblur_pipe.Reset(); host->mblur_vs.Reset(); host->mblur_ps.Reset(); host->mblur_cb.Reset(); host->mblur_ready = false;
    // VXGI の GPU リソースを device 破棄前に解放して UAF を防ぐ。
    host->vxgi_vol.Reset(); host->vxgi_resolve.Reset(); host->vxgi_tri.Reset();
    host->vxgi_pipe_clear.Reset(); host->vxgi_pipe_vox.Reset(); host->vxgi_pipe_res.Reset();
    host->vxgi_cs_clear.Reset(); host->vxgi_cs_vox.Reset(); host->vxgi_cs_res.Reset();
    host->vxgi_cb_vox.Reset(); host->vxgi_cb_res.Reset(); host->vxgi_ready = false;
    host->ibl3d.Shutdown(); host->ibl_ready = false; host->ibl_tried = false; host->ibl_dirty = true;
    host->spr_pipe.Reset(); host->spr_vs.Reset(); host->spr_ps.Reset(); host->spr_vb.Reset();
    host->sprite_textures.Reset();
    // Per-node material and custom-mesh caches are component members; release
    // them explicitly while the RHI device still exists.
    {
        TArray<game::ANode*> all3d;
        Dfs3DCollect(&host->scene3d.Root(), all3d);
        for (u32 i = 0u; i < all3d.Num(); ++i) {
            AEditor3DRecordComponent* record = Rec3D(all3d[i]);
            if (record == nullptr) continue;
            record->gm_cache = FGpuMesh{};
            record->gm_cache_src = nullptr;
            record->material_albedo_tex.Reset();
            record->material_normal_tex.Reset();
            for (u32 slot = 0u;
                 slot < kShaderExpressionMaxTextureSlots;
                 ++slot) {
                record->material_expression_tex[slot].Reset();
            }
            record->material_textures_loaded = false;
        }
    }
    host->m3d_frame_cb.Reset(); host->m3d_giz_cb.Reset(); host->m3d_dyn_vb.Reset(); host->m3d_giz_vb.Reset();
    host->gm_cube = FGpuMesh{}; host->gm_sphere = FGpuMesh{}; host->gm_plane = FGpuMesh{};
    host->gm_water_plane = FGpuMesh{};
    host->cpu_water_plane.Reset();
    // GPU サブシステム/RT は «device 破棄より前» に明示解放する。これを怠ると
    // delete host のデストラクタが renderer.Shutdown() (device 破棄) の «後» に走り、解放済み device
    // 上で GPU リソースを Release して «終了時に間欠 access violation (acs_editor_destroy)» を起こす。
    host->pbr3d.Shutdown();
    host->preview_pbr3d.Shutdown();
    host->preview_pbr3d_ready = false;
    host->ssss3d.Shutdown();
    host->ssss3d_ready = false;
    host->ssss3d_init_failed = false;
    host->ssss3d_init_state = 0u;
    host->ssss3d_pending_shaders = {};
    host->ssss_diffuse_rt.Reset();
    host->ssss_material_rt.Reset();
    host->ssss_pending_diffuse_rt.Reset();
    host->ssss_w = host->ssss_h = 0u;
    host->ssss_pending_w = host->ssss_pending_h = 0u;
    host->ssss_frame_resource_state = 0u;
    host->ssss_frame_failed_w = host->ssss_frame_failed_h = 0u;
    host->post3d.Shutdown();
    host->sky3d.Shutdown();
    host->scene_rt.Reset();
    host->preview_cl.Reset();
    host->preview_rt.Reset();
    host->preview_work_ldr.Reset();
    host->preview_hdr_rt.Reset();
    host->preview_depth.Reset();
    host->preview_ibl_irradiance.Reset();
    host->preview_ibl_prefilter.Reset();
    host->preview_brdf_lut.Reset();
    host->preview_post_cb.Reset();
    host->preview_background_pipe.Reset();
    host->preview_resolve_pipe.Reset();
    host->preview_background_ps.Reset();
    host->preview_resolve_ps.Reset();
    host->preview_post_vs.Reset();
    host->preview_mesh_sphere = FGpuMesh{};
    host->preview_mesh_cube = FGpuMesh{};
    host->preview_mesh_plane = FGpuMesh{};
    host->preview_sphere_albedo.Reset();
    host->preview_sphere_normal.Reset();
    host->preview_scene.Reset();

    // ユーザー型記述子はグローバルレジストリからポインタ参照される。ホスト所有領域を
    // 先に破棄すると次の create/destroy 周回で dangling になるため、登録元だけを外す。
    game::CTypeRegistry& type_registry = game::CTypeRegistry::Get();
    for (u32 i = 0; i < host->user_types.Num(); ++i) {
        (void)type_registry.Unregister(&host->user_types[i]->desc);
    }
    host->user_types.Empty();

    host->renderer.Shutdown();
    delete host;
    ReleaseSubsystems();
}

// =============================================================================
// C ABI — シーン内省 / 編集 (Hierarchy / Inspector 配線用)
// =============================================================================

/** シーンのノード数。 */
ACS_EDITOR_API int acs_editor_node_count(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr) ? static_cast<int>(host->nodes.Num()) : 0;
}

/** リスト index 番目のノード id (範囲外は -1)。 */
ACS_EDITOR_API int acs_editor_node_id_at(void* handle, int index) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || index < 0 || index >= static_cast<int>(host->nodes.Num())) return -1;
    return host->nodes[static_cast<u32>(index)]->editor_id;
}

/** ノードの親 id (root 直下は -1、不明も -1)。 */
ACS_EDITOR_API int acs_editor_node_parent(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return -1;
    const AEditorNode* n = FindNode(*host, id);
    return (n != nullptr) ? ParentIdOf(*host, n) : -1;
}

/** ノード名 (UTF-8、不明は "")。返り値は内部バッファへのポインタ。 */
ACS_EDITOR_API const char* acs_editor_node_name(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return "";
    const AEditorNode* n = FindNode(*host, id);
    return (n != nullptr) ? n->name : "";
}

/** ノードのローカル transform を取得する (out 引数。ANode::Local() を読む)。 */
ACS_EDITOR_API void acs_editor_node_get_transform(void* handle, int id,
                                                  float* x, float* y, float* rot, float* sx, float* sy) {
    auto* host = static_cast<FEditorHost*>(handle);
    AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr) return;
    const game::FTransform2D t = n->Local2D();
    if (x)   *x   = t.position.x;
    if (y)   *y   = t.position.y;
    if (rot) *rot = t.rotation;
    if (sx)  *sx  = t.scale.x;
    if (sy)  *sy  = t.scale.y;
}

/** ノードのローカル transform を設定する (Inspector の編集を ANode::Local() へ反映)。 */
ACS_EDITOR_API void acs_editor_node_set_transform(void* handle, int id,
                                                  float x, float y, float rot, float sx, float sy) {
    auto* host = static_cast<FEditorHost*>(handle);
    AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr) return;
    PushUndo(*host);
    n->SetLocal2D(game::FTransform2D{ FVec2{ x, y }, rot, FVec2{ sx, sy } });
}

// ----- ノードの表示プロパティ (色 / ベースサイズ / 可視 / 有効 / 描画レイヤ) -----

/** ノードの色 (RGBA) を取得する。 */
ACS_EDITOR_API void acs_editor_node_get_color(void* handle, int id,
                                              float* r, float* g, float* b, float* a) {
    auto* host = static_cast<FEditorHost*>(handle);
    const AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    const FVec4 c = (n != nullptr) ? n->color : FVec4{ 0.5f, 0.6f, 0.8f, 1.0f };
    if (r) *r = c.x; if (g) *g = c.y; if (b) *b = c.z; if (a) *a = c.w;
}

/** ノードの色 (RGBA) を設定する。 */
ACS_EDITOR_API void acs_editor_node_set_color(void* handle, int id, float r, float g, float b, float a) {
    auto* host = static_cast<FEditorHost*>(handle);
    AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr) return;
    PushUndo(*host);
    n->color = FVec4{ r, g, b, a };
}

/** ノードのベース表示サイズ (px)。 */
ACS_EDITOR_API float acs_editor_node_get_base(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    const AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    return (n != nullptr) ? n->base : 0.0f;
}

/** ノードのベース表示サイズを設定する。 */
ACS_EDITOR_API void acs_editor_node_set_base(void* handle, int id, float base) {
    auto* host = static_cast<FEditorHost*>(handle);
    AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr) return;
    PushUndo(*host);
    n->base = base;
}

/** ノードの可視フラグ (1/0)。 */
ACS_EDITOR_API int acs_editor_node_get_visible(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    const AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    return (n != nullptr && n->IsVisible()) ? 1 : 0;
}

/** ノードの可視フラグを設定する。 */
ACS_EDITOR_API void acs_editor_node_set_visible(void* handle, int id, int visible) {
    auto* host = static_cast<FEditorHost*>(handle);
    AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr) return;
    PushUndo(*host);
    n->SetVisible(visible != 0);
}

/** ノードの有効フラグ (1/0)。 */
ACS_EDITOR_API int acs_editor_node_get_enabled(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    const AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    return (n != nullptr && n->IsEnabled()) ? 1 : 0;
}

/** ノードの有効フラグを設定する。 */
ACS_EDITOR_API void acs_editor_node_set_enabled(void* handle, int id, int enabled) {
    auto* host = static_cast<FEditorHost*>(handle);
    AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr) return;
    PushUndo(*host);
    n->SetEnabled(enabled != 0);
}

/** ノードの描画レイヤ (sort layer)。 */
ACS_EDITOR_API int acs_editor_node_get_sortlayer(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    const AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    return (n != nullptr) ? n->DrawLayer() : 0;
}

/** ノードの描画レイヤを設定する。 */
ACS_EDITOR_API void acs_editor_node_set_sortlayer(void* handle, int id, int layer) {
    auto* host = static_cast<FEditorHost*>(handle);
    AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr) return;
    PushUndo(*host);
    n->SetDrawLayer(layer);
}

/** ノードにスプライト画像を割り当てる (UTF-8 パス)。即ロードを試み、成功 1 / 不明ノード 0。
 *  device 未準備でも path は保存され、描画時に遅延ロードされる (戻り値は 1)。 */
ACS_EDITOR_API int acs_editor_node_set_sprite(void* handle, int id, const char* utf8_path) {
    auto* host = static_cast<FEditorHost*>(handle);
    AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr || utf8_path == nullptr) return 0;
    PushUndo(*host);
    std::snprintf(n->sprite_path, sizeof(n->sprite_path), "%s", utf8_path);
    LoadNodeSprite(*host, n);
    return 1;
}

/** ノードのスプライト画像パス (UTF-8) を返す (未設定/不明は "")。 */
ACS_EDITOR_API const char* acs_editor_node_get_sprite(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    const AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    return (n != nullptr) ? n->sprite_path : "";
}

/** ノードのスプライトを外す (色付き矩形表示に戻す)。成功 1 / 不明 0。 */
ACS_EDITOR_API int acs_editor_node_clear_sprite(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr) return 0;
    PushUndo(*host);
    n->sprite_path[0] = '\0';
    n->sprite_tex.Reset();
    return 1;
}

// ===== マテリアル (.acsmat = 効果プリセット) =====

/** ノードに使用マテリアル (.acsmat パス) を割り当てる。即解析。成功 1 / 不明 0。 */
ACS_EDITOR_API int acs_editor_node_set_material(void* handle, int id, const char* utf8_path) {
    auto* host = static_cast<FEditorHost*>(handle);
    AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr || utf8_path == nullptr) return 0;
    PushUndo(*host);
    std::snprintf(n->material_path, sizeof(n->material_path), "%s", utf8_path);
    LoadNodeMaterial(n);
    return 1;
}

/** ノードの使用マテリアルパス (UTF-8) を返す (未設定/不明は "")。 */
ACS_EDITOR_API const char* acs_editor_node_get_material(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    const AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    return (n != nullptr) ? n->material_path : "";
}

/** ノードのマテリアルを外す (効果なしに戻す)。成功 1 / 不明 0。 */
ACS_EDITOR_API int acs_editor_node_clear_material(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr) return 0;
    PushUndo(*host);
    n->material_path[0] = '\0';
    LoadNodeMaterial(n);
    return 1;
}

/** マテリアルエディタで .acsmat を再保存した後、そのパスを使う全ノードのキャッシュを落として
 *  次フレームで再解析させる (見た目を即反映)。常に 1。 */
ACS_EDITOR_API int acs_editor_reload_material(void* handle, const char* utf8_path) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || utf8_path == nullptr) return 0;
    for (u32 i = 0; i < host->nodes.Num(); ++i) {
        AEditorNode* n = host->nodes[i];
        if (std::strcmp(n->material_path, utf8_path) == 0) LoadNodeMaterial(n);
    }
    // 3D ノードも同じ .acsmat を参照していれば再ロード (material editor 保存→3D ビューポート即反映)。
    TArray<game::ANode*> all3d; Dfs3DCollect(&host->scene3d.Root(), all3d);
    for (u32 i = 0; i < all3d.Num(); ++i) {
        game::AMeshComponent3D* mc = Mesh3D(all3d[i]);
        if (mc == nullptr) continue;
        const FStringView mp = mc->MaterialPath();
        if (mp.Size() > 0 && std::strncmp(mp.Data(), utf8_path, mp.Size()) == 0 && utf8_path[mp.Size()] == '\0')
            LoadNode3DMaterial(all3d[i]);
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

// ----- Principled slab graph -------------------------------------------------

ACS_EDITOR_API int acs_editor_material_substrate_max_nodes() {
    return static_cast<int>(kSubstrateMaxNodes);
}

ACS_EDITOR_API int acs_editor_material_substrate_slab_scalar_count() {
    return static_cast<int>(kSubstrateSlabScalarCount);
}

/** Legacy ACSMAT assets expose an unsaved generated building-block graph. */
static FSubstrateMaterial MaterialGraphForEditor(
    const game::FMaterial2D& material) noexcept {
    return material.substrate.node_count > 0u
        ? material.substrate
        : game::MakeLegacySubstrateMaterial(material.pbr);
}

ACS_EDITOR_API int acs_editor_material_substrate_get_header(
    const char* path, int* enabled, int* root, int* node_count) {
    if (path == nullptr) return 0;
    game::FMaterial2D material{};
    if (!game::LoadAcsmatFile(path, material)) return 0;
    const FSubstrateMaterial graph = MaterialGraphForEditor(material);
    if (enabled) *enabled = material.substrate.enabled ? 1 : 0;
    if (root) *root = graph.root;
    if (node_count) *node_count = static_cast<int>(graph.node_count);
    return 1;
}

ACS_EDITOR_API int acs_editor_material_substrate_get_node(
    const char* path, int index, int* type, int* input_a, int* input_b,
    float* factor, uint32_t* flags, float* slab39) {
    if (path == nullptr || index < 0) return 0;
    game::FMaterial2D material{};
    if (!game::LoadAcsmatFile(path, material)) return 0;
    const FSubstrateMaterial graph = MaterialGraphForEditor(material);
    if (static_cast<u32>(index) >= graph.node_count) return 0;
    const FSubstrateNode& node = graph.nodes[static_cast<u32>(index)];
    if (type) *type = static_cast<int>(node.type);
    if (input_a) *input_a = node.input_a;
    if (input_b) *input_b = node.input_b;
    if (factor) *factor = node.factor;
    if (flags) *flags = node.flags;
    if (slab39) EncodeSubstrateSlab(node.slab, slab39);
    return 1;
}

static bool SubstrateGraphFromAbi(
    int enabled, int root, int node_count,
    const int* types, const int* input_as, const int* input_bs,
    const float* factors, const uint32_t* flags, const float* slabs,
    FSubstrateMaterial& graph) noexcept {
    if (node_count <= 0 || static_cast<u32>(node_count) > kSubstrateMaxNodes ||
        types == nullptr || input_as == nullptr || input_bs == nullptr ||
        factors == nullptr || flags == nullptr || slabs == nullptr) return false;
    graph = FSubstrateMaterial{};
    graph.enabled = enabled != 0;
    graph.root = root;
    graph.node_count = static_cast<u32>(node_count);
    for (u32 i = 0; i < graph.node_count; ++i) {
        if (types[i] < 0 || types[i] > static_cast<int>(ESubstrateNodeType::Select))
            return false;
        FSubstrateNode& node = graph.nodes[i];
        node.type = static_cast<ESubstrateNodeType>(types[i]);
        node.input_a = input_as[i];
        node.input_b = input_bs[i];
        node.factor = factors[i];
        node.flags = flags[i];
        if (!DecodeSubstrateSlab(slabs + i * kSubstrateSlabScalarCount, node.slab))
            return false;
    }
    return true;
}

static void WriteSubstrateDiagnostics(
    const FSubstrateCompileResult& result,
    int* error_code, int* error_node, uint32_t* feature_bits,
    int* closure_count, int* complexity, int* bytes_per_pixel) noexcept {
    if (error_code) *error_code = static_cast<int>(result.error);
    if (error_node) *error_node = result.error_node;
    if (feature_bits) *feature_bits = result.stats.feature_bits;
    if (closure_count) *closure_count = static_cast<int>(result.stats.closure_count);
    if (complexity) *complexity = static_cast<int>(result.stats.complexity);
    if (bytes_per_pixel)
        *bytes_per_pixel = static_cast<int>(result.stats.estimated_bytes_per_pixel);
}

/** Compile current unsaved editor arrays without touching the asset on disk. */
ACS_EDITOR_API int acs_editor_material_substrate_compile_arrays(
    int enabled, int root, int node_count,
    const int* types, const int* input_as, const int* input_bs,
    const float* factors, const uint32_t* flags, const float* slabs,
    int* error_code, int* error_node, uint32_t* feature_bits,
    int* closure_count, int* complexity, int* bytes_per_pixel) {
    FSubstrateMaterial graph{};
    if (!SubstrateGraphFromAbi(enabled, root, node_count, types, input_as, input_bs,
                               factors, flags, slabs, graph)) {
        FSubstrateCompileResult invalid{};
        invalid.error = ESubstrateCompileError::ValueOutOfRange;
        WriteSubstrateDiagnostics(invalid, error_code, error_node, feature_bits,
                                  closure_count, complexity, bytes_per_pixel);
        return 1;
    }
    const FSubstrateCompileResult result = CompileSubstrateMaterial(graph);
    WriteSubstrateDiagnostics(result, error_code, error_node, feature_bits,
                              closure_count, complexity, bytes_per_pixel);
    return 1;
}

/**
 * Replace the complete graph in one operation.  Arrays contain node_count
 * entries; slabs is a tightly packed node_count*39 float array.
 */
ACS_EDITOR_API int acs_editor_material_substrate_save(
    const char* path, int enabled, int root, int node_count,
    const int* types, const int* input_as, const int* input_bs,
    const float* factors, const uint32_t* flags, const float* slabs) {
    if (path == nullptr) return 0;
    game::FMaterial2D material{};
    if (!game::LoadAcsmatFile(path, material)) return 0;
    FSubstrateMaterial graph{};
    if (!SubstrateGraphFromAbi(enabled, root, node_count, types, input_as, input_bs,
                               factors, flags, slabs, graph)) return 0;
    const FSubstrateCompileResult compiled = CompileSubstrateMaterial(graph);
    if (!compiled.Succeeded()) return 0;
    // Preserve expression authoring when a closure-only editor build saves.
    // Node-index bindings are only safe when topology is unchanged.
    bool has_expression_authoring =
        material.substrate.expression_graph.node_count > 0u ||
        material.substrate.expression_graph.root !=
            kShaderExpressionInvalidNode;
    for (u32 i = 0u;
         !has_expression_authoring &&
         i < material.substrate.node_count;
         ++i) {
        for (u32 scalar = 0u;
             scalar < kSubstrateSlabScalarCount;
             ++scalar) {
            if (material.substrate.nodes[i].expressions.roots[scalar] !=
                kShaderExpressionInvalidNode) {
                has_expression_authoring = true;
                break;
            }
        }
    }
    if (has_expression_authoring) {
        if (graph.node_count != material.substrate.node_count ||
            graph.root != material.substrate.root) {
            return 0;
        }
        for (u32 i = 0u; i < graph.node_count; ++i) {
            const FSubstrateNode& incoming = graph.nodes[i];
            const FSubstrateNode& existing = material.substrate.nodes[i];
            if (incoming.type != existing.type ||
                incoming.input_a != existing.input_a ||
                incoming.input_b != existing.input_b ||
                incoming.flags != existing.flags) {
                return 0;
            }
        }
    }
    graph.expression_graph = material.substrate.expression_graph;
    const u32 preserved_nodes =
        graph.node_count < material.substrate.node_count
            ? graph.node_count
            : material.substrate.node_count;
    for (u32 i = 0u; i < preserved_nodes; ++i) {
        graph.nodes[i].expressions =
            material.substrate.nodes[i].expressions;
    }
    if (!CompileSubstrateExpressionLinks(graph).Succeeded()) return 0;
    material.kind = game::EMaterialKind::Lit;
    material.substrate = graph;
    if (graph.enabled && !game::SyncLegacyPbrFromSubstrate(material)) return 0;
    return game::SaveAcsmatFile(path, material) ? 1 : 0;
}

ACS_EDITOR_API int acs_editor_material_substrate_compile(
    const char* path, int* error_code, int* error_node,
    uint32_t* feature_bits, int* closure_count, int* complexity,
    int* bytes_per_pixel) {
    if (path == nullptr) return 0;
    game::FMaterial2D material{};
    if (!game::LoadAcsmatFile(path, material)) return 0;
    const FSubstrateMaterial graph = MaterialGraphForEditor(material);
    const FSubstrateCompileResult result = CompileSubstrateMaterial(graph);
    WriteSubstrateDiagnostics(result, error_code, error_node, feature_bits,
                              closure_count, complexity, bytes_per_pixel);
    return 1;
}

// ----- Typed shader-expression graph ----------------------------------------

ACS_EDITOR_API int acs_editor_material_expression_max_nodes() {
    return static_cast<int>(kShaderExpressionMaxNodes);
}

ACS_EDITOR_API int acs_editor_material_expression_texture_slots() {
    return static_cast<int>(kShaderExpressionMaxTextureSlots);
}

ACS_EDITOR_API int acs_editor_material_expression_get_header(
    const char* path, int* root, int* node_count) {
    if (path == nullptr) return 0;
    game::FMaterial2D material{};
    if (!game::LoadAcsmatFile(path, material)) return 0;
    if (root) *root = material.substrate.expression_graph.root;
    if (node_count) {
        *node_count =
            static_cast<int>(material.substrate.expression_graph.node_count);
    }
    return 1;
}

ACS_EDITOR_API int acs_editor_material_expression_get_node(
    const char* path, int index,
    int* op, int* declared_type, int* texture_slot, int* texture_flags,
    int* component_index, int* input0, int* input1, int* input2,
    uint32_t* parameter_id, uint32_t* texture_asset_id_low,
    uint32_t* texture_asset_id_high, float* value4) {
    if (path == nullptr || index < 0) return 0;
    game::FMaterial2D material{};
    if (!game::LoadAcsmatFile(path, material)) return 0;
    const FShaderExpressionGraph& graph =
        material.substrate.expression_graph;
    if (static_cast<u32>(index) >= graph.node_count) return 0;
    const FShaderExpressionNode& node =
        graph.nodes[static_cast<u32>(index)];
    if (op) *op = static_cast<int>(node.op);
    if (declared_type) {
        *declared_type = static_cast<int>(node.declared_type);
    }
    if (texture_slot) *texture_slot = static_cast<int>(node.texture_slot);
    if (texture_flags) *texture_flags = static_cast<int>(node.texture_flags);
    if (component_index) {
        *component_index = static_cast<int>(node.component_index);
    }
    if (input0) *input0 = node.inputs[0];
    if (input1) *input1 = node.inputs[1];
    if (input2) *input2 = node.inputs[2];
    if (parameter_id) *parameter_id = node.parameter_id;
    if (texture_asset_id_low) {
        *texture_asset_id_low = node.texture_asset_id_low;
    }
    if (texture_asset_id_high) {
        *texture_asset_id_high = node.texture_asset_id_high;
    }
    if (value4) {
        value4[0] = node.value.x;
        value4[1] = node.value.y;
        value4[2] = node.value.z;
        value4[3] = node.value.w;
    }
    return 1;
}

ACS_EDITOR_API int acs_editor_material_expression_get_bindings(
    const char* path, int slab_node_index, int* roots39) {
    if (path == nullptr || roots39 == nullptr || slab_node_index < 0) {
        return 0;
    }
    game::FMaterial2D material{};
    if (!game::LoadAcsmatFile(path, material)) return 0;
    if (static_cast<u32>(slab_node_index) >=
        material.substrate.node_count) {
        return 0;
    }
    const FSubstrateNode& node =
        material.substrate.nodes[static_cast<u32>(slab_node_index)];
    for (u32 i = 0u; i < kSubstrateSlabScalarCount; ++i) {
        roots39[i] = static_cast<int>(node.expressions.roots[i]);
    }
    return 1;
}

ACS_EDITOR_API int acs_editor_material_expression_get_texture_path(
    const char* path, int slot, char* out_utf8, int out_capacity) {
    if (path == nullptr || out_utf8 == nullptr || out_capacity <= 0 ||
        slot < 0 ||
        static_cast<u32>(slot) >= kShaderExpressionMaxTextureSlots) {
        return 0;
    }
    game::FMaterial2D material{};
    if (!game::LoadAcsmatFile(path, material)) return 0;
    const char* source =
        material.substrateExpressionTexturePaths[static_cast<u32>(slot)];
    const usize length = std::strlen(source);
    if (length + 1u > static_cast<usize>(out_capacity)) return 0;
    std::memcpy(out_utf8, source, length + 1u);
    return 1;
}

static bool ShaderExpressionGraphFromAbi(
    int root, int node_count,
    const int* ops, const int* declared_types,
    const int* texture_slots, const int* texture_flags,
    const int* component_indices,
    const int* input0, const int* input1, const int* input2,
    const uint32_t* parameter_ids,
    const uint32_t* texture_asset_id_lows,
    const uint32_t* texture_asset_id_highs,
    const float* values4,
    FShaderExpressionGraph& graph) noexcept {
    if (node_count < 0 ||
        static_cast<u32>(node_count) > kShaderExpressionMaxNodes) {
        return false;
    }
    graph = FShaderExpressionGraph{};
    if (root < static_cast<int>(std::numeric_limits<i16>::min()) ||
        root > static_cast<int>(std::numeric_limits<i16>::max())) {
        return false;
    }
    graph.root = static_cast<i16>(root);
    graph.node_count = static_cast<u16>(node_count);
    if (node_count == 0) return true;
    if (ops == nullptr || declared_types == nullptr ||
        texture_slots == nullptr || texture_flags == nullptr ||
        component_indices == nullptr || input0 == nullptr ||
        input1 == nullptr || input2 == nullptr || parameter_ids == nullptr ||
        texture_asset_id_lows == nullptr ||
        texture_asset_id_highs == nullptr || values4 == nullptr) {
        return false;
    }
    for (u32 i = 0u; i < graph.node_count; ++i) {
        if (ops[i] < 0 ||
            ops[i] > static_cast<int>(std::numeric_limits<u8>::max()) ||
            declared_types[i] < 0 ||
            declared_types[i] >
                static_cast<int>(std::numeric_limits<u8>::max()) ||
            texture_slots[i] < 0 ||
            texture_slots[i] >
                static_cast<int>(std::numeric_limits<u8>::max()) ||
            texture_flags[i] < 0 ||
            texture_flags[i] >
                static_cast<int>(std::numeric_limits<u8>::max()) ||
            component_indices[i] < 0 ||
            component_indices[i] >
                static_cast<int>(std::numeric_limits<u8>::max()) ||
            input0[i] < static_cast<int>(std::numeric_limits<i16>::min()) ||
            input0[i] > static_cast<int>(std::numeric_limits<i16>::max()) ||
            input1[i] < static_cast<int>(std::numeric_limits<i16>::min()) ||
            input1[i] > static_cast<int>(std::numeric_limits<i16>::max()) ||
            input2[i] < static_cast<int>(std::numeric_limits<i16>::min()) ||
            input2[i] > static_cast<int>(std::numeric_limits<i16>::max())) {
            return false;
        }
        FShaderExpressionNode& node = graph.nodes[i];
        node.op = static_cast<EShaderExpressionOp>(ops[i]);
        node.declared_type =
            static_cast<EShaderExpressionValueType>(declared_types[i]);
        node.texture_slot = static_cast<u8>(texture_slots[i]);
        node.texture_flags = static_cast<u8>(texture_flags[i]);
        node.component_index = static_cast<u8>(component_indices[i]);
        node.inputs[0] = static_cast<i16>(input0[i]);
        node.inputs[1] = static_cast<i16>(input1[i]);
        node.inputs[2] = static_cast<i16>(input2[i]);
        node.parameter_id = parameter_ids[i];
        node.texture_asset_id_low = texture_asset_id_lows[i];
        node.texture_asset_id_high = texture_asset_id_highs[i];
        node.value = FShaderExpressionValue{
            values4[i * 4u + 0u], values4[i * 4u + 1u],
            values4[i * 4u + 2u], values4[i * 4u + 3u]};
    }
    return true;
}

static void WriteShaderExpressionDiagnostics(
    const FShaderExpressionCompileResult& result,
    int* error_code, int* error_node, int* error_input,
    int* expected_type, int* actual_type,
    int* instruction_count, int* constant_fold_count,
    uint32_t* hash_low, uint32_t* hash_high) noexcept {
    FShaderExpressionDiagnostic diagnostic{};
    if (result.diagnostic_count > 0u) {
        diagnostic = result.diagnostics[0];
    }
    if (error_code) *error_code = static_cast<int>(diagnostic.error);
    if (error_node) *error_node = diagnostic.node;
    if (error_input) *error_input = diagnostic.input;
    if (expected_type) {
        *expected_type = static_cast<int>(diagnostic.expected);
    }
    if (actual_type) *actual_type = static_cast<int>(diagnostic.actual);
    if (instruction_count) {
        *instruction_count = static_cast<int>(result.instruction_count);
    }
    if (constant_fold_count) {
        *constant_fold_count =
            static_cast<int>(result.constant_fold_count);
    }
    const u64 hash =
        result.Succeeded() ? HashCompiledShaderExpression(result) : 0u;
    if (hash_low) *hash_low = static_cast<u32>(hash);
    if (hash_high) *hash_high = static_cast<u32>(hash >> 32u);
}

ACS_EDITOR_API int acs_editor_material_expression_compile_arrays(
    int root, int node_count,
    const int* ops, const int* declared_types,
    const int* texture_slots, const int* texture_flags,
    const int* component_indices,
    const int* input0, const int* input1, const int* input2,
    const uint32_t* parameter_ids,
    const uint32_t* texture_asset_id_lows,
    const uint32_t* texture_asset_id_highs,
    const float* values4,
    int* error_code, int* error_node, int* error_input,
    int* expected_type, int* actual_type,
    int* instruction_count, int* constant_fold_count,
    uint32_t* hash_low, uint32_t* hash_high) {
    FShaderExpressionGraph graph{};
    FShaderExpressionCompileResult result{};
    if (!ShaderExpressionGraphFromAbi(
            root, node_count, ops, declared_types, texture_slots,
            texture_flags, component_indices, input0, input1, input2,
            parameter_ids, texture_asset_id_lows, texture_asset_id_highs,
            values4, graph)) {
        result.diagnostic_count = 1u;
        result.diagnostics[0].error =
            EShaderExpressionError::ValueOutOfRange;
    } else {
        result = CompileShaderExpressionGraph(graph);
    }
    WriteShaderExpressionDiagnostics(
        result, error_code, error_node, error_input,
        expected_type, actual_type, instruction_count,
        constant_fold_count, hash_low, hash_high);
    return 1;
}

ACS_EDITOR_API int acs_editor_material_expression_compile(
    const char* path,
    int* link_error, int* link_error_node, int* link_error_scalar,
    int* expression_error, int* expression_error_node,
    int* expression_error_input, int* expected_type, int* actual_type,
    int* instruction_count, int* binding_count) {
    if (path == nullptr) return 0;
    game::FMaterial2D material{};
    if (!game::LoadAcsmatFile(path, material)) return 0;
    const FSubstrateExpressionLinkResult result =
        CompileSubstrateExpressionLinks(material.substrate);
    if (link_error) *link_error = static_cast<int>(result.error);
    if (link_error_node) *link_error_node = result.error_node;
    if (link_error_scalar) *link_error_scalar = result.error_scalar;
    if (expression_error) {
        *expression_error =
            static_cast<int>(result.expression_diagnostic.error);
    }
    if (expression_error_node) {
        *expression_error_node = result.expression_diagnostic.node;
    }
    if (expression_error_input) {
        *expression_error_input = result.expression_diagnostic.input;
    }
    if (expected_type) {
        *expected_type =
            static_cast<int>(result.expression_diagnostic.expected);
    }
    if (actual_type) {
        *actual_type =
            static_cast<int>(result.expression_diagnostic.actual);
    }
    if (instruction_count) {
        *instruction_count =
            static_cast<int>(result.expression_program.instruction_count);
    }
    if (binding_count) {
        *binding_count = static_cast<int>(result.binding_count);
    }
    return 1;
}

/**
 * Atomically replaces the closure graph, expression graph, all Slab bindings,
 * and four expression texture paths. Arrays are tightly packed by node count.
 */
ACS_EDITOR_API int acs_editor_material_substrate_expression_save(
    const char* path,
    int enabled, int substrate_root, int substrate_node_count,
    const int* substrate_types, const int* substrate_input_as,
    const int* substrate_input_bs, const float* substrate_factors,
    const uint32_t* substrate_flags, const float* substrate_slabs39,
    const int* substrate_expression_roots39,
    int expression_root, int expression_node_count,
    const int* expression_ops, const int* expression_declared_types,
    const int* expression_texture_slots,
    const int* expression_texture_flags,
    const int* expression_component_indices,
    const int* expression_input0, const int* expression_input1,
    const int* expression_input2,
    const uint32_t* expression_parameter_ids,
    const uint32_t* expression_texture_asset_id_lows,
    const uint32_t* expression_texture_asset_id_highs,
    const float* expression_values4,
    const char* texture_path0, const char* texture_path1,
    const char* texture_path2, const char* texture_path3) {
    if (path == nullptr || substrate_expression_roots39 == nullptr) {
        return 0;
    }
    game::FMaterial2D material{};
    if (!game::LoadAcsmatFile(path, material)) return 0;
    FSubstrateMaterial graph{};
    if (!SubstrateGraphFromAbi(
            enabled, substrate_root, substrate_node_count,
            substrate_types, substrate_input_as, substrate_input_bs,
            substrate_factors, substrate_flags, substrate_slabs39,
            graph)) {
        return 0;
    }
    if (!ShaderExpressionGraphFromAbi(
            expression_root, expression_node_count,
            expression_ops, expression_declared_types,
            expression_texture_slots, expression_texture_flags,
            expression_component_indices, expression_input0,
            expression_input1, expression_input2,
            expression_parameter_ids,
            expression_texture_asset_id_lows,
            expression_texture_asset_id_highs,
            expression_values4, graph.expression_graph)) {
        return 0;
    }
    for (u32 node_index = 0u;
         node_index < graph.node_count;
         ++node_index) {
        for (u32 scalar = 0u;
             scalar < kSubstrateSlabScalarCount;
             ++scalar) {
            const int root =
                substrate_expression_roots39[
                    node_index * kSubstrateSlabScalarCount + scalar];
            if (root <
                    static_cast<int>(std::numeric_limits<i16>::min()) ||
                root >
                    static_cast<int>(std::numeric_limits<i16>::max())) {
                return 0;
            }
            graph.nodes[node_index].expressions.roots[scalar] =
                static_cast<i16>(root);
        }
    }
    if (!CompileSubstrateMaterial(graph).Succeeded() ||
        !CompileSubstrateExpressionLinks(graph).Succeeded()) {
        return 0;
    }
    const char* paths[kShaderExpressionMaxTextureSlots]{
        texture_path0, texture_path1, texture_path2, texture_path3};
    for (u32 slot = 0u;
         slot < kShaderExpressionMaxTextureSlots;
         ++slot) {
        const char* source = paths[slot] != nullptr ? paths[slot] : "";
        const usize capacity =
            sizeof(material.substrateExpressionTexturePaths[slot]);
        const char* end = static_cast<const char*>(
            std::memchr(source, '\0', capacity));
        if (end == nullptr) {
            return 0;
        }
        const usize length = static_cast<usize>(end - source);
        for (const char* p = source; p < end; ++p) {
            if (*p == '\r' || *p == '\n') return 0;
        }
        std::memcpy(
            material.substrateExpressionTexturePaths[slot],
            source, length + 1u);
    }
    material.kind = game::EMaterialKind::Lit;
    material.substrate = graph;
    if (graph.enabled &&
        !game::SyncLegacyPbrFromSubstrate(material)) {
        return 0;
    }
    return game::SaveAcsmatFile(path, material) ? 1 : 0;
}

// ===== マテリアル GPU プレビュー (実シェーダでサンプルを RT に描き readback) =====

/**
 * Material preview post pass.
 *
 * PBR/Substrate writes scene-linear values to RGBA16F.  PSResolve performs a
 * footprint-aware supersample resolve, ACES fit, exact sRGB OETF and a stable
 * sub-LSB dither before the BGRA8 readback.  Toon/effect previews use the same
 * resolve in pass-through mode because those SpriteBatch shaders already
 * produce display-referred values.
 */
const char* kMaterialPreviewPostHlsl = R"(
cbuffer PreviewConfig : register(b0) {
    float4 params0;     // x=exposure, y=background mode, z=render scale, w=1 for display-referred input
    float4 params1;     // xy=1/source size, zw=1/output size
    float4 background_top;
    float4 background_bottom;
};
Texture2D source_texture : register(t0);
SamplerState source_texture_sampler : register(s0);

struct PreviewVsOut {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

PreviewVsOut VSMain(uint vertex_id : SV_VertexID) {
    float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    PreviewVsOut output;
    output.uv = uv;
    output.pos = float4(
        uv.x * 2.0 - 1.0, -(uv.y * 2.0 - 1.0), 0.0, 1.0);
    return output;
}

float4 PSBackground(PreviewVsOut input) : SV_TARGET {
    int mode = (int)(params0.y + 0.5);
    if (mode == 2) {
        return float4(0.0035, 0.0045, 0.0065, 1.0);
    }
    if (mode == 1) {
        float2 tile = floor(input.uv * 18.0);
        float checker = fmod(tile.x + tile.y, 2.0);
        float3 dark = float3(0.055, 0.060, 0.070);
        float3 light = float3(0.120, 0.128, 0.142);
        return float4(lerp(dark, light, checker), 1.0);
    }

    float vertical = smoothstep(0.0, 1.0, input.uv.y);
    float3 color = lerp(
        background_top.rgb, background_bottom.rgb, vertical);
    float2 centered = input.uv * 2.0 - 1.0;
    float vignette = saturate(1.0 - dot(centered, centered) * 0.23);
    float softbox = exp(
        -dot(input.uv - float2(0.28, 0.22),
             input.uv - float2(0.28, 0.22)) * 7.0);
    color *= lerp(0.78, 1.0, vignette);
    color += softbox * float3(0.050, 0.046, 0.040);
    return float4(color, 1.0);
}

float3 AcsPreviewAces(float3 color) {
    float3 aces;
    aces.r = dot(color, float3(0.59719, 0.35458, 0.04823));
    aces.g = dot(color, float3(0.07600, 0.90834, 0.01566));
    aces.b = dot(color, float3(0.02840, 0.13383, 0.83777));
    float3 numerator =
        aces * (aces + 0.0245786) - 0.000090537;
    float3 denominator =
        aces * (0.983729 * aces + 0.4329510) + 0.238081;
    aces = numerator / max(denominator, 1e-6);
    float3 output;
    output.r = dot(aces, float3( 1.60475, -0.53108, -0.07367));
    output.g = dot(aces, float3(-0.10208,  1.10813, -0.00605));
    output.b = dot(aces, float3(-0.00327, -0.07276,  1.07602));
    return saturate(output);
}

float3 AcsPreviewSrgb(float3 linear_color) {
    float3 linear_value = max(linear_color, 0.0);
    float3 low = linear_value * 12.92;
    float3 high =
        1.055 * pow(linear_value, 1.0 / 2.4) - 0.055;
    return float3(
        linear_value.r <= 0.0031308 ? low.r : high.r,
        linear_value.g <= 0.0031308 ? low.g : high.g,
        linear_value.b <= 0.0031308 ? low.b : high.b);
}

float AcsPreviewNoise(float2 pixel) {
    return frac(52.9829189 *
        frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float4 PSResolve(PreviewVsOut input) : SV_TARGET {
    float3 color;
    float render_scale = max(params0.z, 1.0);
    if (render_scale <= 1.01) {
        color = source_texture.SampleLevel(
            source_texture_sampler, input.uv, 0).rgb;
    } else {
        // Integrate the source footprint of one output pixel.  Four positions
        // per axis are enough for stable 2x/4x preview SSAA while retaining
        // texture/normal detail better than a single bilinear lookup.
        color = 0.0;
        [unroll]
        for (int y = 0; y < 4; ++y) {
            [unroll]
            for (int x = 0; x < 4; ++x) {
                float2 unit_offset =
                    (float2((float)x, (float)y) + 0.5) * 0.25 - 0.5;
                float2 uv_offset =
                    unit_offset * render_scale * params1.xy;
                color += source_texture.SampleLevel(
                    source_texture_sampler, input.uv + uv_offset, 0).rgb;
            }
        }
        color *= 1.0 / 16.0;
    }

    if (params0.w < 0.5) {
        color = AcsPreviewAces(color * params0.x);
        color = AcsPreviewSrgb(color);
        color += (AcsPreviewNoise(input.pos.xy) - 0.5) *
            (0.75 / 255.0);
    }
    return float4(saturate(color), 1.0);
}
)";

struct FMaterialPreviewPostCb {
    FVec4 params0;
    FVec4 params1;
    FVec4 background_top;
    FVec4 background_bottom;
};

constexpr u32 MaterialPreviewScale(u32 quality) noexcept {
    return quality >= 2u ? 4u : (quality == 1u ? 2u : 1u);
}

static u32 MaterialPreviewWorkSize(
    const FEditorHost& h, u32 output_size) noexcept {
    const u64 requested =
        static_cast<u64>(output_size) *
        static_cast<u64>(MaterialPreviewScale(h.preview_quality));
    return static_cast<u32>(
        requested > 2048u ? 2048u : requested);
}

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
static void EnsurePreviewSamples(FEditorHost& h) noexcept {
    if (h.preview_samples_ready) return;
    IRhiDevice* dev = h.renderer.Device();
    if (dev == nullptr) return;
    constexpr u32 S = 96;
    TArray<u8> alb; alb.SetNum(S * S * 4);
    TArray<u8> nrm; nrm.SetNum(S * S * 4);
    TArray<u8> scn; scn.SetNum(S * S * 4);
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
    h.preview_sphere_albedo = MakeTex(*dev, S, S, alb.GetData());
    h.preview_sphere_normal = MakeTex(*dev, S, S, nrm.GetData());
    h.preview_scene         = MakeTex(*dev, S, S, scn.GetData());
    h.preview_samples_ready = true;
}

/** HDR background + ACES/sRGB resolve pipelines shared by all preview modes. */
static bool EnsurePreviewPost(FEditorHost& h) noexcept {
    if (h.preview_post_ready) return true;
    IRhiDevice* dev = h.renderer.Device();
    if (dev == nullptr) return false;

    FShaderDesc vertex_desc{};
    vertex_desc.stage = EShaderStage::Vertex;
    vertex_desc.hlsl_source = kMaterialPreviewPostHlsl;
    vertex_desc.entry_point = "VSMain";
    vertex_desc.debug_name = "MaterialPreview.Post.VS";
    auto vertex_result = CreateRhiShader(*dev, vertex_desc);
    if (vertex_result.IsErr()) return false;

    FShaderDesc background_desc{};
    background_desc.stage = EShaderStage::Pixel;
    background_desc.hlsl_source = kMaterialPreviewPostHlsl;
    background_desc.entry_point = "PSBackground";
    background_desc.debug_name = "MaterialPreview.Background.PS";
    auto background_result =
        CreateRhiShader(*dev, background_desc);
    if (background_result.IsErr()) return false;

    FShaderDesc resolve_desc{};
    resolve_desc.stage = EShaderStage::Pixel;
    resolve_desc.hlsl_source = kMaterialPreviewPostHlsl;
    resolve_desc.entry_point = "PSResolve";
    resolve_desc.debug_name = "MaterialPreview.Resolve.PS";
    auto resolve_result = CreateRhiShader(*dev, resolve_desc);
    if (resolve_result.IsErr()) return false;

    h.preview_post_vs = Move(vertex_result.Value());
    h.preview_background_ps = Move(background_result.Value());
    h.preview_resolve_ps = Move(resolve_result.Value());

    FPipelineDesc background_pipeline{};
    background_pipeline.vs = h.preview_post_vs.Get();
    background_pipeline.ps = h.preview_background_ps.Get();
    background_pipeline.topology = EPrimitiveTopology::TriangleList;
    background_pipeline.rt_format =
        EFormat::R16G16B16A16_Float;
    background_pipeline.depth_format = h.renderer.DepthFormat();
    background_pipeline.vertex_stride = 0u;
    background_pipeline.layout_count = 0u;
    background_pipeline.cull_mode = ECullMode::None;
    background_pipeline.depth_test = false;
    background_pipeline.depth_write = false;
    background_pipeline.cbuffer_slots = 1u;
    background_pipeline.cbuffer_names[0] = "PreviewConfig";
    auto background_pipeline_result =
        CreateRhiPipeline(*dev, background_pipeline);
    if (background_pipeline_result.IsErr()) return false;
    h.preview_background_pipe =
        Move(background_pipeline_result.Value());

    FPipelineDesc resolve_pipeline{};
    resolve_pipeline.vs = h.preview_post_vs.Get();
    resolve_pipeline.ps = h.preview_resolve_ps.Get();
    resolve_pipeline.topology = EPrimitiveTopology::TriangleList;
    resolve_pipeline.rt_format = h.renderer.ColorFormat();
    resolve_pipeline.depth_format = EFormat::Unknown;
    resolve_pipeline.vertex_stride = 0u;
    resolve_pipeline.layout_count = 0u;
    resolve_pipeline.cull_mode = ECullMode::None;
    resolve_pipeline.depth_test = false;
    resolve_pipeline.depth_write = false;
    resolve_pipeline.cbuffer_slots = 1u;
    resolve_pipeline.texture_slots = 1u;
    resolve_pipeline.cbuffer_names[0] = "PreviewConfig";
    resolve_pipeline.texture_names[0] = "source_texture";
    resolve_pipeline.static_sampler_count = 1u;
    resolve_pipeline.static_samplers[0].filter =
        ESamplerFilter::Linear;
    resolve_pipeline.static_samplers[0].address_u =
        ESamplerAddress::Clamp;
    resolve_pipeline.static_samplers[0].address_v =
        ESamplerAddress::Clamp;
    resolve_pipeline.static_samplers[0].address_w =
        ESamplerAddress::Clamp;
    auto resolve_pipeline_result =
        CreateRhiPipeline(*dev, resolve_pipeline);
    if (resolve_pipeline_result.IsErr()) return false;
    h.preview_resolve_pipe =
        Move(resolve_pipeline_result.Value());

    FBufferDesc buffer_desc{};
    buffer_desc.size = 256u;
    buffer_desc.usage = EBufferUsage::Uniform;
    buffer_desc.cpu_writable = true;
    auto buffer_result = CreateRhiBuffer(*dev, buffer_desc);
    if (buffer_result.IsErr()) return false;
    h.preview_post_cb = Move(buffer_result.Value());
    h.preview_post_ready = true;
    return true;
}

/** Display-size LDR RT, dedicated command list and non-MSAA SpriteBatch. */
static bool EnsurePreviewRt(FEditorHost& h, u32 size) noexcept {
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
        td.format = h.renderer.ColorFormat();
        td.is_render_target = true;
        auto color_result = CreateRhiTexture(*dev, td);
        if (color_result.IsErr()) return false;
        h.preview_rt = Move(color_result.Value());
        h.preview_rt_size = size;
    }
    // 本体バッチは MSAA PSO のため preview work RT には描けない。
    if (!h.preview_sprites_ready) {
        const auto r = h.preview_sprites.Init(*dev, h.renderer.ColorFormat(), 1024, 1);
        if (r.IsErr()) return false;
        h.preview_sprites_ready = true;
    }
    return true;
}

static bool EnsurePreviewLdrWork(
    FEditorHost& h, u32 output_size) noexcept {
    if (!EnsurePreviewRt(h, output_size) ||
        !EnsurePreviewPost(h)) {
        return false;
    }
    IRhiDevice* dev = h.renderer.Device();
    if (dev == nullptr) return false;
    const u32 work_size =
        MaterialPreviewWorkSize(h, output_size);
    if (!h.preview_work_ldr ||
        h.preview_ldr_work_size != work_size) {
        FTextureDesc desc{};
        desc.width = work_size;
        desc.height = work_size;
        desc.format = h.renderer.ColorFormat();
        desc.is_render_target = true;
        auto result = CreateRhiTexture(*dev, desc);
        if (result.IsErr()) return false;
        h.preview_work_ldr = Move(result.Value());
        h.preview_ldr_work_size = work_size;
    }
    return true;
}

/** Ensure the preview-only HDR PBR pipeline and selectable studio meshes. */
static bool EnsurePreviewPbr(FEditorHost& h, u32 size) noexcept {
    if (!EnsurePreviewRt(h, size) ||
        !EnsurePreviewPost(h)) {
        return false;
    }
    IRhiDevice* dev = h.renderer.Device();
    if (dev == nullptr) return false;

    // プレビュー環境光は SH9 から供給する。金属・鏡面反射では split-sum BRDF LUT も
    // 参照するため、環境光未設定時の黒出力を避ける解析 LUT と有効な立方体テクスチャを結線する。
    if (!h.preview_ibl_irradiance ||
        !h.preview_ibl_prefilter) {
        FTextureDesc cube_desc{};
        cube_desc.width = 1u;
        cube_desc.height = 1u;
        cube_desc.format = EFormat::R11G11B10_Float;
        cube_desc.array_size = 6u;
        cube_desc.is_cubemap = true;
        auto irradiance_result =
            CreateRhiTexture(*dev, cube_desc);
        if (irradiance_result.IsErr()) return false;
        auto prefilter_result =
            CreateRhiTexture(*dev, cube_desc);
        if (prefilter_result.IsErr()) return false;
        h.preview_ibl_irradiance =
            Move(irradiance_result.Value());
        h.preview_ibl_prefilter =
            Move(prefilter_result.Value());
    }
    if (!h.preview_brdf_lut) {
        constexpr u32 kBrdfSize = 128u;
        TArray<f32> brdf;
        brdf.SetNum(kBrdfSize * kBrdfSize * 2u);
        for (u32 y = 0u; y < kBrdfSize; ++y) {
            const f32 roughness =
                (static_cast<f32>(y) + 0.5f) /
                static_cast<f32>(kBrdfSize);
            const f32 rx = 1.0f - roughness;
            const f32 ry =
                0.0425f - 0.0275f * roughness;
            const f32 rz =
                1.04f - 0.572f * roughness;
            const f32 rw =
                -0.04f + 0.022f * roughness;
            for (u32 x = 0u; x < kBrdfSize; ++x) {
                const f32 no_v =
                    (static_cast<f32>(x) + 0.5f) /
                    static_cast<f32>(kBrdfSize);
                const f32 a004 =
                    std::min(
                        rx * rx,
                        std::exp2(-9.28f * no_v)) *
                        rx + ry;
                const u32 index =
                    (y * kBrdfSize + x) * 2u;
                brdf[index] =
                    std::clamp(
                        -1.04f * a004 + rz,
                        0.0f, 1.0f);
                brdf[index + 1u] =
                    std::clamp(
                        1.04f * a004 + rw,
                        0.0f, 1.0f);
            }
        }
        FTextureDesc brdf_desc{};
        brdf_desc.width = kBrdfSize;
        brdf_desc.height = kBrdfSize;
        brdf_desc.format = EFormat::R32G32_Float;
        brdf_desc.initial_data = brdf.GetData();
        brdf_desc.initial_data_size =
            brdf.Num() * sizeof(f32);
        auto brdf_result =
            CreateRhiTexture(*dev, brdf_desc);
        if (brdf_result.IsErr()) return false;
        h.preview_brdf_lut = Move(brdf_result.Value());
    }

    const u32 work_size = MaterialPreviewWorkSize(h, size);
    if (!h.preview_hdr_rt || !h.preview_depth ||
        h.preview_hdr_size != work_size) {
        FTextureDesc hdr_desc{};
        hdr_desc.width = work_size;
        hdr_desc.height = work_size;
        hdr_desc.format = EFormat::R16G16B16A16_Float;
        hdr_desc.is_render_target = true;
        auto hdr_result = CreateRhiTexture(*dev, hdr_desc);
        if (hdr_result.IsErr()) return false;

        FTextureDesc depth_desc{};
        depth_desc.width = work_size;
        depth_desc.height = work_size;
        depth_desc.format = h.renderer.DepthFormat();
        depth_desc.is_depth_target = true;
        auto depth_result =
            CreateRhiTexture(*dev, depth_desc);
        if (depth_result.IsErr()) return false;
        h.preview_hdr_rt = Move(hdr_result.Value());
        h.preview_depth = Move(depth_result.Value());
        h.preview_hdr_size = work_size;
    }

    if (!h.preview_pbr3d_ready) {
        const auto result = h.preview_pbr3d.Init(
            *dev, EFormat::R16G16B16A16_Float,
            h.renderer.DepthFormat());
        if (result.IsErr()) return false;
        h.preview_pbr3d_ready = true;
    }

    if (!h.preview_mesh_sphere.vertex_buffer ||
        !h.preview_mesh_sphere.index_buffer) {
        auto sphere = Primitive::MakeSphere(0.5f, 128, 64);
        if (!sphere ||
            UploadMesh(
                *dev, *sphere,
                h.preview_mesh_sphere).IsErr()) {
            return false;
        }
    }
    if (!h.preview_mesh_cube.vertex_buffer ||
        !h.preview_mesh_cube.index_buffer) {
        auto cube = Primitive::MakeCube(1.0f);
        if (!cube ||
            UploadMesh(
                *dev, *cube,
                h.preview_mesh_cube).IsErr()) {
            return false;
        }
    }
    if (!h.preview_mesh_plane.vertex_buffer ||
        !h.preview_mesh_plane.index_buffer) {
        auto plane = Primitive::MakePlane(1.35f, 1.35f);
        if (!plane) return false;
        // MakePlane is an XZ ground plane.  A material swatch needs to face
        // the preview camera so UVs, normal maps and anisotropy remain
        // readable instead of being back-face culled into the background.
        // Mapping (x, y=0, z) -> (x, -z, 0) preserves the authored UVs and
        // makes both the geometric and stored normal point toward -Z.
        for (u32 vertex = 0u;
             vertex < plane->Vertices().Num();
             ++vertex) {
            FMeshVertex& value =
                plane->Vertices()[vertex];
            value.position =
                FVec3{
                    value.position.x,
                    -value.position.z,
                    0.0f};
            value.normal = FVec3{0.0f, 0.0f, -1.0f};
        }
        if (UploadMesh(
                *dev, *plane,
                h.preview_mesh_plane).IsErr()) {
            return false;
        }
    }
    return true;
}

static void UpdatePreviewPostCb(
    FEditorHost& h, u32 source_size, u32 output_size,
    bool display_referred) noexcept {
    if (!h.preview_post_cb) return;
    const f32 source = static_cast<f32>(source_size);
    const f32 output = static_cast<f32>(output_size);
    const f32 scale = source / output;
    FMaterialPreviewPostCb cb{};
    cb.params0 = FVec4{
        h.preview_exposure,
        static_cast<f32>(h.preview_background),
        scale,
        display_referred ? 1.0f : 0.0f};
    cb.params1 = FVec4{
        1.0f / source, 1.0f / source,
        1.0f / output, 1.0f / output};
    cb.background_top =
        FVec4{0.115f, 0.135f, 0.180f, 1.0f};
    cb.background_bottom =
        FVec4{0.022f, 0.026f, 0.038f, 1.0f};
    h.preview_post_cb->Update(&cb, sizeof(cb));
}

static void ResolvePreview(
    FEditorHost& h, IRhiCommandList& command_list,
    IRhiTexture& source, u32 output_size) noexcept {
    command_list.BeginRenderToTexture(
        *h.preview_rt,
        FClearColor{0.0f, 0.0f, 0.0f, 1.0f},
        nullptr, 1.0f);
    FViewport viewport{};
    viewport.width = static_cast<f32>(output_size);
    viewport.height = static_cast<f32>(output_size);
    command_list.SetViewport(viewport);
    FScissorRect scissor{};
    scissor.right = static_cast<i32>(output_size);
    scissor.bottom = static_cast<i32>(output_size);
    command_list.SetScissor(scissor);
    command_list.SetPipeline(*h.preview_resolve_pipe);
    command_list.SetConstantBuffer(0u, *h.preview_post_cb);
    command_list.SetTexture(0u, source);
    command_list.Draw(3u, 0u);
    command_list.EndRenderToTexture(*h.preview_rt);
}

/** Supersampled Toon/effect preview, resolved to display-sized BGRA8. */
template<typename DrawFn>
static int RenderPreview(FEditorHost& h, u32 size, u8* out_rgba, u32 out_size, DrawFn&& drawFn) noexcept {
    const u64 required =
        static_cast<u64>(size) * static_cast<u64>(size) * 4u;
    if (out_rgba == nullptr || size == 0u || size > 2048u ||
        required > out_size) {
        return 0;
    }
    if (!EnsurePreviewLdrWork(h, size)) return 0;
    EnsurePreviewSamples(h);
    IRhiDevice* dev = h.renderer.Device();
    IRhiCommandList* cl = h.preview_cl.Get();
    if (dev == nullptr || cl == nullptr ||
        !h.preview_rt || !h.preview_work_ldr ||
        !h.preview_resolve_pipe ||
        !h.preview_post_cb) {
        return 0;
    }
    const u32 work_size =
        MaterialPreviewWorkSize(h, size);
    UpdatePreviewPostCb(
        h, work_size, size, true);

    cl->Begin();
    cl->BeginRenderToTexture(
        *h.preview_work_ldr,
        FClearColor{0.10f, 0.10f, 0.12f, 1.0f},
        nullptr, 1.0f);
    h.preview_sprites.Begin(*cl, work_size, work_size);
    drawFn(
        h.preview_sprites,
        static_cast<f32>(work_size));
    h.preview_sprites.End();
    cl->EndRenderToTexture(*h.preview_work_ldr);
    ResolvePreview(
        h, *cl, *h.preview_work_ldr, size);
    cl->End();
    if (!cl->Submit()) return 0;
    dev->WaitIdle();
    return dev->ReadTexture(*h.preview_rt, out_rgba, out_size) ? 1 : 0;
}

/**
 * Draw a true indexed studio model through CPbrShader.  This is the material
 * editor's authoritative preview path for both legacy PBR and Substrate:
 * WorldPosition/WorldNormal/UV/Time and texture expressions therefore see
 * the same inputs and bytecode interpreter as the 3D viewport.
 */
static int RenderPbrMaterialPreview(
    FEditorHost& h, const game::FMaterial2D& material,
    u32 size, u8* out_rgba, u32 out_size) noexcept {
    const u64 required =
        static_cast<u64>(size) * static_cast<u64>(size) * 4u;
    if (out_rgba == nullptr || size == 0u ||
        size > 2048u || required > out_size ||
        !EnsurePreviewPbr(h, size)) {
        return 0;
    }
    IRhiDevice* dev = h.renderer.Device();
    IRhiCommandList* cl = h.preview_cl.Get();
    if (dev == nullptr || cl == nullptr ||
        !h.preview_rt || !h.preview_hdr_rt ||
        !h.preview_depth || !h.preview_post_cb ||
        !h.preview_background_pipe ||
        !h.preview_resolve_pipe) {
        return 0;
    }

    TUniquePtr<IRhiTexture> albedo_texture;
    TUniquePtr<IRhiTexture> normal_texture;
    TUniquePtr<IRhiTexture>
        expression_textures[kShaderExpressionMaxTextureSlots];
    LoadTexFromPath(
        h, material.pbr.albedoPath, albedo_texture);
    LoadTexFromPath(
        h, material.pbr.normalPath, normal_texture);
    for (u32 slot = 0u;
         slot < kShaderExpressionMaxTextureSlots;
         ++slot) {
        LoadTexFromPath(
            h,
            material.substrateExpressionTexturePaths[slot],
            expression_textures[slot]);
    }

    CCamera camera;
    camera.SetPerspective(
        34.0f * kDeg2Rad, 1.0f, 0.05f, 20.0f);
    const FVec3 eye{1.62f, 0.92f, -2.72f};
    camera.SetLookAt(
        eye, FVec3{0.0f, 0.02f, 0.0f});

    FDirLight lights[1]{};
    lights[0].direction =
        Normalize(FVec3{0.38f, 0.72f, -0.58f});
    lights[0].color = FVec3{1.70f, 1.56f, 1.40f};

    CPbrShader& shader = h.preview_pbr3d;
    if (!shader.BeginFrame(1u)) return 0;
    shader.SetLights(
        camera.ViewProjection(), eye, lights, 1u,
        FVec3{0.018f, 0.021f, 0.028f});
    shader.SetPointLights(nullptr, 0u);
    // CPbrShader's production area-light integrator intentionally exposes
    // its 4x4 samples on mirror-like materials.  That is useful in motion,
    // but a static swatch reveals the individual dots.  The preview instead
    // uses the continuous SH9 studio reflection below plus smooth key/fill
    // directional lobes, keeping glossy and rough materials artifact-free.
    shader.SetAreaLights(nullptr, 0u);
    shader.SetShadowMap(nullptr, FMat4::Identity());
    shader.SetIbl(
        h.preview_ibl_irradiance.Get(),
        h.preview_ibl_prefilter.Get(),
        h.preview_brdf_lut.Get(), 1u);
    // A neutral studio dome remains deterministic on both Raw DX12 and
    // Diligent while providing a readable roughness-dependent reflection.
    FVec4 studio_sh9[9];
    ComputeSkySh9(
        studio_sh9,
        FVec3{0.34f, 0.39f, 0.50f},
        FVec3{0.48f, 0.49f, 0.52f},
        FVec3{0.16f, 0.15f, 0.15f});
    for (u32 coefficient = 0u;
         coefficient < 9u; ++coefficient) {
        studio_sh9[coefficient].x *= 0.72f;
        studio_sh9[coefficient].y *= 0.72f;
        studio_sh9[coefficient].z *= 0.72f;
    }
    shader.SetSh9(studio_sh9);
    const u32 work_size = h.preview_hdr_size;
    shader.SetSsao(
        nullptr, 0.0f, work_size, work_size);
    shader.SetSsgi(nullptr, 0.0f);
    shader.SetSsr(nullptr, 0.0f);
    shader.SetAerialPerspective(nullptr, 1.0f);
    const game::FPbrParams2D& pbr = material.pbr;
    shader.SetNormalMap(normal_texture.Get(), pbr.normalStrength);
    const bool substrate_active =
        material.substrate.enabled &&
        shader.SetSubstrateMaterial(
            material.substrate, h.time);
    if (substrate_active) {
        for (u32 slot = 0u;
             slot < kShaderExpressionMaxTextureSlots;
             ++slot) {
            shader.SetSubstrateExpressionTexture(
                slot, expression_textures[slot].Get());
        }
    } else {
        shader.ClearSubstrateSurface();
        shader.SetExtParams(
            pbr.clearcoat, pbr.clearcoatRoughness,
            pbr.anisotropy, FVec3{1.0f, 0.0f, 0.0f});
        shader.SetSheen(
            pbr.sheenColor, pbr.sheen,
            pbr.sheenRoughness);
        shader.SetSubsurface(
            pbr.subsurfaceColor, pbr.subsurface);
        shader.SetEmissive(
            pbr.emissive, pbr.emissiveStrength);
        shader.SetIridescence(0.0f, 400.0f, 1.4f);
    }

    const FGpuMesh* preview_mesh =
        &h.preview_mesh_sphere;
    FMat4 preview_model =
        FMat4::Scale(FVec3{1.70f, 1.70f, 1.70f});
    if (h.preview_model == 1u) {
        preview_mesh = &h.preview_mesh_cube;
        preview_model =
            FMat4::Scale(FVec3{1.15f, 1.15f, 1.15f}) *
            FMat4::RotationY(0.58f) *
            FMat4::RotationX(-0.24f);
    } else if (h.preview_model == 2u) {
        preview_mesh = &h.preview_mesh_plane;
        preview_model =
            FMat4::Scale(FVec3{1.10f, 1.10f, 1.10f});
    }
    if (!preview_mesh->vertex_buffer ||
        !preview_mesh->index_buffer) {
        shader.SetNormalMap(nullptr);
        shader.ClearSubstrateSurface();
        return 0;
    }

    UpdatePreviewPostCb(
        h, work_size, size, false);
    cl->Begin();
    cl->BeginRenderToTexture(
        *h.preview_hdr_rt,
        FClearColor{0.0f, 0.0f, 0.0f, 1.0f},
        h.preview_depth.Get(), 1.0f);
    FViewport hdr_viewport{};
    hdr_viewport.width = static_cast<f32>(work_size);
    hdr_viewport.height = static_cast<f32>(work_size);
    cl->SetViewport(hdr_viewport);
    FScissorRect hdr_scissor{};
    hdr_scissor.right = static_cast<i32>(work_size);
    hdr_scissor.bottom = static_cast<i32>(work_size);
    cl->SetScissor(hdr_scissor);
    cl->SetPipeline(*h.preview_background_pipe);
    cl->SetConstantBuffer(0u, *h.preview_post_cb);
    cl->Draw(3u, 0u);
    shader.DrawMesh(
        *cl, *preview_mesh, preview_model,
        FVec3{
            pbr.baseColor.x, pbr.baseColor.y,
            pbr.baseColor.z},
        pbr.metallic, pbr.roughness, pbr.ao,
        albedo_texture.Get());
    cl->EndRenderToTexture(*h.preview_hdr_rt);
    ResolvePreview(
        h, *cl, *h.preview_hdr_rt, size);
    cl->End();
    if (!cl->Submit()) {
        shader.SetNormalMap(nullptr);
        shader.ClearSubstrateSurface();
        return 0;
    }
    dev->WaitIdle();
    const int result =
        dev->ReadTexture(
            *h.preview_rt, out_rgba, out_size) ? 1 : 0;

    // Non-owning shader references must never outlive these local textures.
    shader.SetNormalMap(nullptr);
    shader.ClearSubstrateSurface();
    return result;
}

/**
 * Configure the editor-only material preview.  These values are presentation
 * state and are deliberately not serialized into the material asset.
 */
ACS_EDITOR_API void acs_editor_set_material_preview_options(
        void* handle, int quality, int model, int background,
        float exposure) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return;
    host->preview_quality =
        quality <= 0 ? 0u : (quality >= 2 ? 2u : 1u);
    host->preview_model =
        model <= 0 ? 0u : (model >= 2 ? 2u : 1u);
    host->preview_background =
        background <= 0 ? 0u :
        (background >= 2 ? 2u : 1u);
    if (std::isfinite(exposure)) {
        host->preview_exposure =
            std::clamp(exposure, 0.25f, 4.0f);
    }
}

/** PBR マテリアルを «ライト付きの球» として実シェーダで描きプレビューを out へ読み戻す。 */
ACS_EDITOR_API int acs_editor_render_preview_pbr(void* handle,
        float br, float bg, float bb, float ba, float metallic, float roughness,
        float er, float eg, float eb, float em_str, float normal_str, float ao,
        unsigned char* out_rgba, int size) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || !host->sprites_ready || size <= 0) return 0;
    game::FMaterial2D material{};
    material.kind = game::EMaterialKind::Lit;
    material.pbr.baseColor = FVec4{br, bg, bb, ba};
    material.pbr.metallic = metallic;
    material.pbr.roughness = roughness;
    material.pbr.emissive = FVec3{er, eg, eb};
    material.pbr.emissiveStrength = em_str;
    material.pbr.normalStrength = normal_str;
    material.pbr.ao = ao;
    const u32 s = static_cast<u32>(size);
    const u64 bytes =
        static_cast<u64>(s) * static_cast<u64>(s) * 4u;
    if (bytes > static_cast<u64>(
            std::numeric_limits<u32>::max())) {
        return 0;
    }
    return RenderPbrMaterialPreview(
        *host, material, s, out_rgba,
        static_cast<u32>(bytes));
}

/** 効果プリセットをミニ風景に適用した実シェーダプレビューを out へ読み戻す。 */
ACS_EDITOR_API int acs_editor_render_preview_effect(void* handle,
        int effect, float strength, float p0, float p1, float p2,
        float r, float g, float b, float a, float time,
        unsigned char* out_rgba, int size) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || !host->sprites_ready || size <= 0) return 0;
    const u32 s = static_cast<u32>(size);
    const ESpriteEffect e = game::SpriteEffectAt(static_cast<u32>(effect < 0 ? 0 : effect));
    FEffectParams p;
    p.strength = strength; p.p0 = p0; p.p1 = p1; p.p2 = p2; p.time = time;
    p.color = FVec4{ r, g, b, a };
    return RenderPreview(*host, s, out_rgba, static_cast<u32>(s * s * 4),
        [&](CSpriteBatch& sb, f32 sz) {
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
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || !host->sprites_ready || size <= 0 || path == nullptr) return 0;
    const u32 s = static_cast<u32>(size);
    game::FMaterial2D mat;
    if (!game::LoadAcsmatFile(path, mat)) return 0;

    if (mat.kind == game::EMaterialKind::Effect) {       // 効果プリセット: ミニ風景に適用
        const bool fx = (mat.effect != ESpriteEffect::None);
        FEffectParams p = mat.params;
        if (mat.animated) p.time = 1.0f;
        return RenderPreview(*host, s, out_rgba, static_cast<u32>(s * s * 4),
            [&](CSpriteBatch& sb, f32 sz) {
                if (fx) sb.SetEffect(mat.effect, p);
                if (host->preview_scene) sb.Draw(*host->preview_scene, 0, 0, sz, sz);
                if (fx) sb.ClearEffect();
            });
    }
    // Toon keeps its production SpriteBatch toon shader. CPbrShader does not
    // implement the cel ramps/rim/spec-threshold contract.
    if (mat.pbr.shadingMode == 1) {
        const FLitMaterialParams toon =
            game::ToLitParams(mat.pbr);
        const u64 bytes =
            static_cast<u64>(s) * static_cast<u64>(s) * 4u;
        if (bytes > static_cast<u64>(
                std::numeric_limits<u32>::max())) {
            return 0;
        }
        return RenderPreview(
            *host, s, out_rgba, static_cast<u32>(bytes),
            [&](CSpriteBatch& sb, f32 sz) {
                FSpriteLight light;
                light.pos = FVec2{
                    sz * 0.32f, sz * 0.28f};
                light.radius = sz * 1.6f;
                light.color =
                    FVec3{1.0f, 0.97f, 0.92f};
                light.intensity = 2.4f;
                sb.SetLights(
                    &light, 1,
                    FVec3{0.06f, 0.07f, 0.09f},
                    sz * 0.45f);
                sb.SetLitMaterial(
                    toon,
                    host->preview_sphere_normal.Get());
                if (host->preview_sphere_albedo) {
                    sb.Draw(
                        *host->preview_sphere_albedo,
                        0, 0, sz, sz);
                }
                sb.ClearLit();
            });
    }
    // Legacy PBR and Substrate use the real indexed 3D sphere and production VM.
    const u64 bytes =
        static_cast<u64>(s) * static_cast<u64>(s) * 4u;
    if (bytes > static_cast<u64>(
            std::numeric_limits<u32>::max())) {
        return 0;
    }
    return RenderPbrMaterialPreview(
        *host, mat, s, out_rgba,
        static_cast<u32>(bytes));
}

// ===== Play モード (物理プレビュー) =====

/** ノードに付与された ARigidBody2D コンポーネントの slot を返す (無ければ -1)。 */
static int RigidBodySlot(const AEditorNode* n) noexcept {
    static game::FTypeId s_rbId = 0;
    if (s_rbId == 0) {
        const game::FTypeDesc* d = game::CTypeRegistry::Get().FindByName("ARigidBody2D");
        if (d != nullptr) s_rbId = d->id;
    }
    if (s_rbId == 0) return -1;
    for (u32 c = 0; c < n->component_count; ++c)
        if (n->components[c] == s_rbId) return static_cast<int>(c);
    return -1;
}

/** ARigidBody2D を付けたノードから剛体ワールドを組み立てる。
 *  プロパティ (bodyType/shape/restitution/friction/mass/damping) はコンポーネントの
 *  編集値 (comp_props、スキーマ順) から読む。動的のみ書き戻し対象に記録する。 */
static void EditorBuildPlayWorld(FEditorHost& h) noexcept {
    h.play_world = MakeUnique<game::CRigidWorld2D>();
    h.play_body.Reset();
    h.play_node.Reset();
    if (h.play_world.Get() == nullptr) return;
    for (u32 i = 0; i < h.nodes.Num(); ++i) {
        AEditorNode* n = h.nodes[i];
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

        const game::FTransform2D w = n->World2D();
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
            h.play_body.Add(bi);                           // 動的のみ書き戻し対象
            h.play_node.Add(n->editor_id);
        }
    }
}

/** 1 フレーム物理を進め、ボディ位置/角度を対応ノードへ書き戻す (undo は積まない)。 */
static void EditorStepPlay(FEditorHost& h, f32 dt) noexcept {
    if (h.play_world.Get() == nullptr) return;
    if (dt > 0.05f) dt = 0.05f;                          // 大 dt を抑制 (安定性)
    h.play_world->Step(dt, FVec2{ 0.0f, 900.0f });       // +Y = 画面下 → 下向き重力
    for (u32 i = 0; i < h.play_node.Num(); ++i) {
        AEditorNode* n = FindNode(h, h.play_node[i]);
        if (n == nullptr) continue;
        const u32 bi = h.play_body[i];
        const FVec2 p = h.play_world->Position(bi);
        SetNodeWorldPosition(h, n, p.x, p.y);
        n->SetRotation2D(h.play_world->Angle(bi));   // 簡易 (親回転は無視)
    }
}

static void CapturePlayEditorCamera(FEditorHost& h) noexcept {
    h.play_cam_pan_x = h.cam_pan_x;
    h.play_cam_pan_y = h.cam_pan_y;
    h.play_cam_zoom = h.cam_zoom;
    h.play_view3d = h.view3d;
    h.play_ortho3d = h.ortho3d;
    h.play_cam3d_yaw = h.cam3d_yaw;
    h.play_cam3d_pitch = h.cam3d_pitch;
    h.play_cam3d_dist = h.cam3d_dist;
    h.play_cam3d_target = h.cam3d_target;
    h.play_camera_snapshot_valid = true;
}

static void RestorePlayEditorCamera(FEditorHost& h) noexcept {
    if (!h.play_camera_snapshot_valid) return;
    h.cam_pan_x = h.play_cam_pan_x;
    h.cam_pan_y = h.play_cam_pan_y;
    h.cam_zoom = h.play_cam_zoom;
    h.view3d = h.play_view3d;
    h.ortho3d = h.play_ortho3d;
    h.cam3d_yaw = h.play_cam3d_yaw;
    h.cam3d_pitch = h.play_cam3d_pitch;
    h.cam3d_dist = h.play_cam3d_dist;
    h.cam3d_target = h.play_cam3d_target;
    h.play_camera_snapshot_valid = false;
    InvalidateTemporalRenderHistories(h);
}

/** 再生を開始する (現在状態をスナップショットし、物理ワールドを構築)。成功 1 / 既に再生中 0。 */
ACS_EDITOR_API int acs_editor_play_start(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || host->play_state != 0) return 0;
    host->play_snapshot = DupSnapshot(*host);            // 停止時の復元用
    if (host->play_snapshot == nullptr) return 0;
    CapturePlayEditorCamera(*host);
    EditorBuildPlayWorld(*host);
    host->play_state = 1;
    return 1;
}

/** 再生を停止し、開始時の状態へ復元する。成功 1 / 停止中 0。 */
ACS_EDITOR_API int acs_editor_play_stop(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || host->play_state == 0) return 0;
    host->play_state = 0;
    host->play_world.Reset();
    host->play_body.Reset();
    host->play_node.Reset();
    if (host->play_snapshot != nullptr) {
        RestoreSnapshot(*host, host->play_snapshot);     // 開始状態 (2D + 3D) へ復元 (undo は積まない)
        delete[] host->play_snapshot;
        host->play_snapshot = nullptr;
    }
    RestorePlayEditorCamera(*host);
    return 1;
}

/** 再生の一時停止/再開を切り替える (paused!=0 で一時停止)。 */
ACS_EDITOR_API void acs_editor_play_set_paused(void* handle, int paused) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return;
    if (paused != 0 && host->play_state == 1)      host->play_state = 2;
    else if (paused == 0 && host->play_state == 2) host->play_state = 1;
}

/** 一時停止中に物理を 1 フレームだけ進める。 */
ACS_EDITOR_API void acs_editor_play_step(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host != nullptr && host->play_state == 2) EditorStepPlay(*host, 1.0f / 60.0f);
}

/** 再生状態を返す (0=stopped, 1=playing, 2=paused)。 */
ACS_EDITOR_API int acs_editor_play_state(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr) ? host->play_state : 0;
}

// ===== ポリゴン描画ツール =====

/** ポリゴン描画を開始する (点をクリックで集める)。 */
ACS_EDITOR_API void acs_editor_poly_begin(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return;
    host->poly_drawing = true;
    host->poly_points.Reset();
}

/** 描画中にスクリーン点を 1 つ追加する (world に変換して保持)。 */
ACS_EDITOR_API void acs_editor_poly_add_point(void* handle, float sx, float sy) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || !host->poly_drawing) return;
    host->poly_points.Add(FVec2{ S2WX(*host, sx), S2WY(*host, sy) });
}

/** 集めた点を 1 ノード (APrimitiveRenderer2D=Polygon) として確定する。新 id / 3 点未満は -1。
 *  クリック点をアンカーとして閉じた Catmull-Rom 曲線で滑らかにし (描画 = render_verts)、
 *  その凸包を間引いてコライダー (poly_verts ≤kMaxPolyVerts) を生成する。 */
ACS_EDITOR_API int acs_editor_poly_finalize(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return -1;
    host->poly_drawing = false;
    u32 np = host->poly_points.Num();
    if (np < 3) { host->poly_points.Reset(); return -1; }

    // アンカー (クリック点) を world でコピー (ローカルバッファ上限まで)。
    constexpr u32 kRV = AEditorNode::kMaxRenderVerts;
    if (np > kRV) np = kRV;
    FVec2 anchors[kRV];
    for (u32 i = 0; i < np; ++i) anchors[i] = host->poly_points[i];

    // 閉じた滑らかな曲線をサンプル (world)。
    FVec2 smooth[kRV];
    u32 sc = SmoothClosedSpline(anchors, np, smooth, kRV);
    if (sc < 3) { host->poly_points.Reset(); return -1; }

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
    AEditorNode* n = AddEditorNode(*host, -1, "Polygon", cx, cy, 0.0f, maxR * 2.0f, FVec4{ 0.55f, 0.75f, 0.95f, 1.0f });
    n->render_count = sc;
    for (u32 i = 0; i < sc; ++i) n->render_verts[i] = rlocal[i];
    n->poly_count = cc;
    for (u32 i = 0; i < cc; ++i) n->poly_verts[i] = collider[i];
    AttachComponent(n, "APrimitiveRenderer2D");
    SetCompProp(n, 0, 0, 3.0f, 0.0f, 0.0f, 0.0f);             // renderer.shape = 3 (Polygon)
    host->poly_points.Reset();
    SelSet(*host, n->editor_id);
    return n->editor_id;
}

/** ポリゴン描画を破棄する。 */
ACS_EDITOR_API void acs_editor_poly_cancel(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return;
    host->poly_drawing = false;
    host->poly_points.Reset();
}

/** ポリゴン描画中か (1/0)。 */
ACS_EDITOR_API int acs_editor_poly_is_drawing(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr && host->poly_drawing) ? 1 : 0;
}

/** ノードを単一選択する (集合を {id} に置換、primary=id。ビューポートでハイライト)。 */
ACS_EDITOR_API void acs_editor_select(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host != nullptr) SelSet(*host, id);
}

/** 現在の primary (active) ノード id (未選択は -1)。 */
ACS_EDITOR_API int acs_editor_selected(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr) ? host->selected : -1;
}

// ----- 複数選択 (Ctrl+click トグル / 全選択 / 解除 / 集合の列挙) -----

/** id の選択を反転する (Ctrl+click)。追加で primary になり、primary を外すと別の一員へ移る。 */
ACS_EDITOR_API void acs_editor_select_toggle(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host != nullptr) SelToggle(*host, id);
}

/** 全ノードを選択する (primary は最後のノード)。 */
ACS_EDITOR_API void acs_editor_select_all(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return;
    host->selection.Reset();
    for (u32 i = 0; i < host->nodes.Num(); ++i) host->selection.Add(host->nodes[i]->editor_id);
    host->selected = (host->selection.Num() > 0)
                     ? host->selection[host->selection.Num() - 1] : -1;
}

/** 選択を全解除する。 */
ACS_EDITOR_API void acs_editor_select_none(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host != nullptr) SelClear(*host);
}

/** 選択集合の要素数。 */
ACS_EDITOR_API int acs_editor_selection_count(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr) ? static_cast<int>(host->selection.Num()) : 0;
}

/** 選択集合の index 番目のノード id (範囲外は -1)。 */
ACS_EDITOR_API int acs_editor_selection_at(void* handle, int index) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || index < 0 || index >= static_cast<int>(host->selection.Num())) return -1;
    return host->selection[static_cast<u32>(index)];
}

/** id が選択集合に含まれるか (1/0)。 */
ACS_EDITOR_API int acs_editor_selection_contains(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr && SelContains(*host, id)) ? 1 : 0;
}

/**
 * screen 矩形に中心が入るノードを選択する (ラバーバンド選択)。additive=0 で置換、!=0 で追加。
 *
 * @return 矩形内に入った (新規追加 + 既存) ノード総数。
 */
ACS_EDITOR_API int acs_editor_select_box(void* handle, float x0, float y0, float x1, float y1, int additive) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return 0;
    const f32 minx = (x0 < x1) ? x0 : x1, maxx = (x0 < x1) ? x1 : x0;
    const f32 miny = (y0 < y1) ? y0 : y1, maxy = (y0 < y1) ? y1 : y0;
    if (additive == 0) host->selection.Reset();
    int inBox = 0;
    for (u32 i = 0; i < host->nodes.Num(); ++i) {
        AEditorNode* n = host->nodes[i];
        const game::FTransform2D w = n->World2D();
        const f32 sx = W2SX(*host, w.position.x);
        const f32 sy = W2SY(*host, w.position.y);
        if (sx >= minx && sx <= maxx && sy >= miny && sy <= maxy) {   // 中心が矩形内
            ++inBox;
            if (!SelContains(*host, n->editor_id)) host->selection.Add(n->editor_id);
        }
    }
    // primary を整える: 追加で既存 primary が有効ならそのまま、でなければ末尾の一員へ。
    if (host->selection.Num() > 0) {
        if (additive == 0 || host->selected < 0 || !SelContains(*host, host->selected))
            host->selected = host->selection[host->selection.Num() - 1];
    } else {
        host->selected = -1;
    }
    return inBox;
}

/** ラバーバンド矩形のオーバーレイ表示を設定する (active!=0 で screen 座標の矩形を描く)。 */
ACS_EDITOR_API void acs_editor_set_marquee(void* handle, int active, float x0, float y0, float x1, float y1) {
    auto* host = static_cast<FEditorHost*>(handle);
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
// (プロセス唯一の CTypeRegistry を引く)。AcsRegisterEngineTypes() は create 時に
// 呼ばれ済みだが、念のため各エントリで冪等に確定しておく。
// =============================================================================

/** 登録されているエンジン型の総数。 */
ACS_EDITOR_API int acs_editor_type_count(void) {
    acs::game::AcsRegisterEngineTypes();
    return static_cast<int>(acs::game::CTypeRegistry::Get().Count());
}

/** index 番目の型名 (範囲外は "")。 */
ACS_EDITOR_API const char* acs_editor_type_name_at(int index) {
    if (index < 0) return "";
    const auto* d = acs::game::CTypeRegistry::Get().At(static_cast<u32>(index));
    return (d != nullptr && d->name != nullptr) ? d->name : "";
}

/** index 番目の型のカテゴリ (ETypeCategory の整数値、範囲外は -1)。 */
ACS_EDITOR_API int acs_editor_type_category_at(int index) {
    if (index < 0) return -1;
    const auto* d = acs::game::CTypeRegistry::Get().At(static_cast<u32>(index));
    return (d != nullptr) ? static_cast<int>(d->category) : -1;
}

/** index 番目の型が default 構築可能か (factory で生成できる = 1)。 */
ACS_EDITOR_API int acs_editor_type_instantiable_at(int index) {
    if (index < 0) return 0;
    const auto* d = acs::game::CTypeRegistry::Get().At(static_cast<u32>(index));
    if (d == nullptr) return 0;
    return ((d->traits & acs::game::TRAIT_INSTANTIABLE) != 0u) ? 1 : 0;
}

/** index 番目の型のフィールド数 (Component/Struct)、または列挙値数 (Enum)。 */
ACS_EDITOR_API int acs_editor_type_member_count_at(int index) {
    if (index < 0) return 0;
    const auto* d = acs::game::CTypeRegistry::Get().At(static_cast<u32>(index));
    if (d == nullptr) return 0;
    return static_cast<int>(d->category == acs::game::ETypeCategory::Enum
                                ? d->enum_count : d->field_count);
}

// ===== ユーザー定義型 (ゲーム DLL から取り込んだもの) の列挙 =====

/** ゲーム DLL から取り込んだユーザー定義型の数。 */
ACS_EDITOR_API int acs_editor_user_type_count(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr) ? static_cast<int>(host->user_types.Num()) : 0;
}

/** i 番目のユーザー定義型の名前 (UTF-8、範囲外は "")。 */
ACS_EDITOR_API const char* acs_editor_user_type_name_at(void* handle, int index) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || index < 0 || static_cast<u32>(index) >= host->user_types.Num()) return "";
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
static FUserType* FindUserType(FEditorHost& h, game::FTypeId id) noexcept {
    for (u32 i = 0; i < h.user_types.Num(); ++i)
        if (h.user_types[i]->desc.id == id) return h.user_types[i].Get();
    return nullptr;
}

/** DLL から取り込んだメタデータ専用記述子かを判定する。 */
static bool IsImportedUserType(const game::FTypeDesc* descriptor) noexcept
{
    return descriptor != nullptr && descriptor->category == game::ETypeCategory::Component && descriptor->size == 0u &&
           descriptor->align == 0u && descriptor->construct == nullptr && descriptor->destruct == nullptr;
}

/**
 * ゲームのリフレクション DLL をロードし、ユーザー定義の Component 型スキーマを取り込む。
 *
 * @details DLL の C ABI (acs_game_reflect_*) で型を列挙し、エンジンに無い Component をディープ
 * コピーして host->user_types に保持 + エディタの CTypeRegistry へ登録する (→ AttachComponent /
 * インスペクタ / シリアライズが engine 型と同経路で動く)。既存型は in-place 更新。コピー後 DLL は
 * FreeLibrary する (再ビルドで上書き可能に)。
 * @return 取り込んだ/更新した型数、LoadLibrary 失敗 -1、シンボル欠如 -2。
 */
ACS_EDITOR_API int acs_editor_load_game_dll(void* handle, const char* path) {
    auto* host = static_cast<FEditorHost*>(handle);
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
        const game::FTypeDesc* eng = game::CTypeRegistry::Get().FindById(id);
        if (eng != nullptr && ut == nullptr && !IsImportedUserType(eng)) {
            continue; // 実エンジン型 (= editor registry に既存) は上書きしない。
        }

        const bool isNew = (ut == nullptr);
        if (isNew) {
            host->user_types.Add(MakeUnique<FUserType>());
            ut = host->user_types[host->user_types.Num() - 1].Get();
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
        if (isNew && !game::CTypeRegistry::Get().Register(&ut->desc)) {
            // 固定長レジストリが満杯なら、未登録の記述子をホストへ残さない。
            host->user_types.Pop();
            continue;
        }
        ++imported;
    }
    FreeLibrary(dll);   // スキーマはディープコピー済 → 解放 (再ビルドで DLL を上書きできる)
    return imported;
}

// ===== シーン実体化 (authored 値で実コンポーネントを attach → tick = 実ロジック実行) =====

/**
 * 各ノードのコンポーネント・メタデータ + comp_props (authored 値) から «実 AComponent» を
 * 生成し、反射オフセット経由で値を実体へ適用してノードへ attach する。
 *
 * @details reg.CreateById → ApplyFieldValue(値適用ブリッジ) → AttachComponent。offset 付き
 * フィールド (ACS_RFIELD_D = ユーザー型) は authored 値が乗り、offset 無し (ACS_RPROP) は factory
 * 既定値で動く。tick_instances で UpdateTree すると実 OnUpdate が走る。
 * @return attach した実コンポーネント数。
 */
ACS_EDITOR_API int acs_editor_instantiate_scene(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return 0;
    // 既存の実体を一掃してから作り直す。
    for (u32 i = 0; i < host->nodes.Num(); ++i) host->nodes[i]->RemoveAllComponents();

    int total = 0;
    for (u32 i = 0; i < host->nodes.Num(); ++i) {
        AEditorNode* n = host->nodes[i];
        for (u32 s = 0; s < n->component_count; ++s) {
            const game::FTypeDesc* d = game::CTypeRegistry::Get().FindById(n->components[s]);
            if (d == nullptr || d->name == nullptr) continue;
            TUniquePtr<game::AComponent> comp = game::CreateComponentByName(d->name);
            if (!comp) continue;                       // 非 Component / Abstract はスキップ
            void* obj = comp.Get();
            for (u32 j = 0; j < d->field_count && j < AEditorNode::kMaxProps; ++j)
                game::ApplyFieldValue(obj, d->fields[j], n->comp_props[s][j]);   // authored 値を実体へ
            n->AttachComponent(static_cast<TUniquePtr<game::AComponent>&&>(comp));
            ++total;
        }
    }
    host->instances_live = true;
    return total;
}

/** 実体化したコンポーネントを 1 フレーム tick する (実 OnUpdate を実行)。 */
ACS_EDITOR_API void acs_editor_tick_instances(void* handle, float dt) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || !host->instances_live || host->root.Get() == nullptr) return;
    host->root->UpdateTree(dt);
    host->root->ResolveStructuralChanges();
}

/** 実体化したコンポーネントを全て除去し、編集 (メタデータのみ) 状態へ戻す。 */
ACS_EDITOR_API void acs_editor_clear_instances(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return;
    for (u32 i = 0; i < host->nodes.Num(); ++i) host->nodes[i]->RemoveAllComponents();
    host->instances_live = false;
}

// ===== Preview (DLL ビルド不要のライブ実行: エンジンコンポーネントを editor 内で tick) =====

/** Preview を開始する。各ノード transform を退避 → 実体化 → 毎フレーム tick で実 OnUpdate が走る。成功 1。 */
ACS_EDITOR_API int acs_editor_preview_start(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || host->preview_live) return 0;
    host->preview_snap.Reset();                                   // 開始時の transform を退避 (停止で復元)
    for (u32 i = 0; i < host->nodes.Num(); ++i) host->preview_snap.Add(host->nodes[i]->Local2D());
    acs_editor_instantiate_scene(handle);                         // 実コンポーネントを attach + authored 値適用
    host->preview_live = true;
    return 1;
}

/** Preview を停止する。実体を除去し、開始時の transform へ復元する。 */
ACS_EDITOR_API void acs_editor_preview_stop(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || !host->preview_live) return;
    host->preview_live = false;
    acs_editor_clear_instances(handle);
    const u32 n = (host->preview_snap.Num() < host->nodes.Num()) ? host->preview_snap.Num() : host->nodes.Num();
    for (u32 i = 0; i < n; ++i) host->nodes[i]->SetLocal2D(host->preview_snap[i]);   // 非破壊: 位置を戻す
    host->preview_snap.Reset();
}

/** Preview 中なら 1。 */
ACS_EDITOR_API int acs_editor_preview_state(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr && host->preview_live) ? 1 : 0;
}

/** 現在 attach されている «実» コンポーネントの総数 (= 実体化できた数。検証/表示用)。 */
ACS_EDITOR_API int acs_editor_instance_count(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return 0;
    int total = 0;
    for (u32 i = 0; i < host->nodes.Num(); ++i) total += static_cast<int>(host->nodes[i]->ComponentCount());
    return total;
}

// ===== インプロセス Play (ゲーム DLL がユーザーコンポーネントを実行) =====

ACS_EDITOR_API void acs_editor_logic_play_stop(void* handle);   // 前方宣言 (start が呼ぶ)

static void ResetLogicInput(FEditorHost& h) noexcept {
    if (!h.logic_play) return;
    if (h.logic_shim.input_key != nullptr) {
        for (int key = 1; key <= 65; ++key)
            h.logic_shim.input_key(key, 0);
    }
    if (h.logic_shim.input_mouse_button != nullptr) {
        for (int button = 0; button < 3; ++button)
            h.logic_shim.input_mouse_button(button, 0);
    }
}

/** Play 中の 1 フレーム: DLL シーンを tick → 各ノード transform を読み戻して描画へ反映。 */
static void EditorTickLogic(FEditorHost& h, f32 dt) noexcept {
    if (!h.logic_play || h.logic_scene == nullptr) return;
    if (dt > 0.05f) dt = 0.05f;
    // フレーム先頭で入力状態を進める (now→prev)。WPF からの key イベントは
    // acs_editor_logic_input_key 経由で DLL の acs::Input へ随時フィードされる。
    if (h.logic_shim.input_update != nullptr) h.logic_shim.input_update();
    // PUSH: editor ノードの現在 transform (= editor 物理 Play が動かした結果) を DLL へ送ってから
    // tick する。これでコンポーネントの OnDraw / OnUpdate が «物理で動いた位置» を基準に走り、
    // editor メタデータ描画 (物理) と DLL の OnDraw が同じ位置に揃う (二重シミュの位置ずれ解消)。
    for (u32 i = 0; i < h.nodes.Num(); ++i) {
        const game::FTransform2D t = h.nodes[i]->Local2D();
        h.logic_shim.set_transform(h.logic_scene, static_cast<int>(i),
                                   t.position.x, t.position.y, t.rotation, t.scale.x, t.scale.y);
    }
    h.logic_shim.tick(h.logic_scene, dt);
    for (u32 i = 0; i < h.nodes.Num(); ++i) {   // dll node idx == host->nodes index (順次構築)
        f32 x = 0, y = 0, r = 0, sx = 1, sy = 1;
        h.logic_shim.get_transform(h.logic_scene, static_cast<int>(i), &x, &y, &r, &sx, &sy);
        // DLL が «動かしたノードだけ» 反映する (開始時 transform と同一なら触らない)。
        // → コンポーネントの無いノードは物理 Play 等の結果を保てる (両 Play の共存)。
        const game::FTransform2D sv = (i < h.logic_saved.Num()) ? h.logic_saved[i] : h.nodes[i]->Local2D();
        if (x != sv.position.x || y != sv.position.y || r != sv.rotation || sx != sv.scale.x || sy != sv.scale.y) {
            h.nodes[i]->SetLocal2D(game::FTransform2D{ FVec2{ x, y }, r, FVec2{ sx, sy } });
        }
    }

    // game camera is retained independently from the Scene View navigation
    // camera. Game View consumes these values during its render slice; Scene
    // View remains freely navigable throughout Play.
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
                h.logic_game_zoom  = z;
                h.logic_game_pan_x = w * 0.5f - cx * z;
                h.logic_game_pan_y = hh * 0.5f - cy * z;
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
    auto* host = static_cast<FEditorHost*>(handle);
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
    host->logic_saved.Reset();
    for (u32 i = 0; i < host->nodes.Num(); ++i) {
        AEditorNode* n = host->nodes[i];
        host->logic_saved.Add(n->Local2D());          // 復元用に退避
        // 旧 DLL (set_parent 無し) 互換: best-effort で親-先行ケースだけ即配置。
        int parentIdx = -1;
        if (sh.set_parent == nullptr) {
            const int pid = ParentIdOf(*host, n);
            if (pid >= 0)
                for (u32 k = 0; k < i; ++k) if (host->nodes[k]->editor_id == pid) { parentIdx = static_cast<int>(k); break; }
        }
        const int idx = sh.add_node(scene, parentIdx);
        const game::FTransform2D t = n->Local2D();
        sh.set_transform(scene, idx, t.position.x, t.position.y, t.rotation, t.scale.x, t.scale.y);
        for (u32 s = 0; s < n->component_count; ++s) {
            const game::FTypeDesc* d = game::CTypeRegistry::Get().FindById(n->components[s]);
            if (d == nullptr || d->name == nullptr) continue;
            const int dslot = sh.add_component(scene, idx, d->name);
            if (dslot < 0) continue;                       // DLL の registry に無い型はスキップ
            for (u32 j = 0; j < d->field_count && j < AEditorNode::kMaxProps; ++j) {
                if (d->fields[j].name == nullptr) continue;
                const f32* v = n->comp_props[s][j];
                sh.set_prop(scene, idx, dslot, d->fields[j].name, v[0], v[1], v[2], v[3]);
            }
        }
    }
    // 2nd pass: 親子を «順序非依存» に確定する (全ノードを検索して親 index を解決)。
    if (sh.set_parent != nullptr) {
        for (u32 i = 0; i < host->nodes.Num(); ++i) {
            const int pid = ParentIdOf(*host, host->nodes[i]);
            if (pid < 0) continue;                          // root 直下はそのまま
            int parentIdx = -1;
            for (u32 k = 0; k < host->nodes.Num(); ++k)
                if (host->nodes[k]->editor_id == pid) { parentIdx = static_cast<int>(k); break; }
            if (parentIdx >= 0) sh.set_parent(scene, static_cast<int>(i), parentIdx);
        }
    }
    // Initialize the independent legacy 2D game camera from deterministic
    // authored bounds. Scene View navigation is intentionally not consulted.
    const FDeterministicGameCamera2D game_camera =
        ResolveDeterministicGameCamera2D(
            *host, host->width, host->height);
    host->logic_game_pan_x = game_camera.pan_x;
    host->logic_game_pan_y = game_camera.pan_y;
    host->logic_game_zoom = game_camera.zoom;
    host->logic_cam_following = false;
    if (sh.set_camera != nullptr) {
        sh.set_camera(
            scene, game_camera.center_x,
            game_camera.center_y, game_camera.zoom);
    }
    host->logic_game_cam0_x = game_camera.center_x;
    host->logic_game_cam0_y = game_camera.center_y;
    host->logic_game_cam0_zoom = game_camera.zoom;

    host->logic_dll = dll; host->logic_scene = scene; host->logic_shim = sh; host->logic_play = true;
    return 1;
}

/** インプロセス Play を停止する。DLL シーンを破棄し、ノード transform を開始時へ復元、DLL を解放。 */
ACS_EDITOR_API void acs_editor_logic_play_stop(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || !host->logic_play) return;
    ResetLogicInput(*host);
    if (host->logic_scene != nullptr && host->logic_shim.destroy != nullptr)
        host->logic_shim.destroy(host->logic_scene);
    for (u32 i = 0; i < host->nodes.Num() && i < host->logic_saved.Num(); ++i)
        host->nodes[i]->SetLocal2D(host->logic_saved[i]);   // 編集状態へ復元
    if (host->logic_dll != nullptr) FreeLibrary(host->logic_dll);
    host->logic_dll = nullptr; host->logic_scene = nullptr; host->logic_play = false;
    host->logic_saved.Reset();
}

/** インプロセス Play 中か (1/0)。 */
ACS_EDITOR_API int acs_editor_logic_play_active(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
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
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || !host->logic_play || host->logic_shim.input_key == nullptr) return;
    host->logic_shim.input_key(keycode, down);
}

/** Play 中の DLL へマウスボタン入力をフィードする (button: 0=Left,1=Right,2=Middle)。 */
ACS_EDITOR_API void acs_editor_logic_input_mouse_button(void* handle, int button, int down) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || !host->logic_play || host->logic_shim.input_mouse_button == nullptr) return;
    host->logic_shim.input_mouse_button(button, down);
}

/** Play 中の DLL へマウス位置 (viewport クライアント px) をフィードする。 */
ACS_EDITOR_API void acs_editor_logic_input_mouse_move(void* handle, float x, float y) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || !host->logic_play || host->logic_shim.input_mouse_move == nullptr) return;
    host->logic_shim.input_mouse_move(x, y);
}

/** Release every gameplay key/button after Game View focus or routing changes. */
ACS_EDITOR_API void acs_editor_logic_input_reset(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host != nullptr) ResetLogicInput(*host);
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
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return;
    const bool gameView = on != 0;
    if (host->game_view != gameView) {
        ResetLogicInput(*host);
        InvalidateTemporalRenderHistories(*host);
    }
    host->game_view = gameView;
}

/** 現在ゲームビューか (1/0)。 */
ACS_EDITOR_API int acs_editor_is_game_view(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
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
 * エディタが「エンジンの登録型」を選んで実 ANode ツリーへ実体化する経路。ここでは
 * 型名をラベルにした AEditorNode を生成する (将来 factory による本物のコンポーネント
 * attach に拡張可能)。
 * @return 追加したノードの editor_id、不正引数は -1。
 */
ACS_EDITOR_API int acs_editor_add_node(void* handle, const char* type_name, int parent_id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || type_name == nullptr || host->root.Get() == nullptr) return -1;
    PushUndo(*host);

    // 親があればその近く、無ければビューポート中央付近に置く。
    const bool has_parent = (FindNode(*host, parent_id) != nullptr);
    const f32  px = has_parent ? 64.0f : static_cast<f32>(host->width)  * 0.5f;
    const f32  py = has_parent ? 0.0f  : static_cast<f32>(host->height) * 0.5f;

    AEditorNode* n = AddEditorNode(*host, has_parent ? parent_id : -1, type_name,
                                   px, py, 0.0f, 40.0f, FVec4{ 0.55f, 0.70f, 0.55f, 1.0f });
    SelSet(*host, n->editor_id);
    return n->editor_id;
}

// =============================================================================
// C ABI — シーンの保存 / 読込 (永続化)
// -----------------------------------------------------------------------------
// ABI はシリアライズ/デシリアライズ (文字列 ⇄ 実 ANode ツリー) を担い、ファイル I/O と
// ダイアログ・パス処理は C# (WPF) 側が担当する (パスのエンコーディング問題を回避)。
// =============================================================================

/** 現在のシーンをテキストへシリアライズして返す (内部バッファへのポインタ)。 */
ACS_EDITOR_API const char* acs_editor_scene_serialize(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr) ? SerializeScene(*host) : "";
}

/** シリアライズ済みテキストからシーンを復元する (成功 1 / 失敗 0)。 */
ACS_EDITOR_API int acs_editor_scene_load_text(void* handle, const char* text) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || !ValidateEditorScene2DText(text)) {
        return 0;
    }
    PushUndo(*host);
    return LoadSceneTextValidated(*host, text);
}

/** シーンを空 (隠しルートのみ) に戻す (New Scene)。 */
ACS_EDITOR_API void acs_editor_scene_new(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return;
    PushUndo(*host);
    ClearScene(*host);
}

/** 3D シーンを空にする (新規シーン)。選択/クリップボードもリセット。Undo で復元可。 */
ACS_EDITOR_API void acs_editor_scene3d_new(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return;
    PushUndo(*host);
    ClearScene3D(*host);
}

/**
 * Replace the singular editor document with an empty 2D/3D compatibility
 * envelope. The nested clear helpers share one Join -> WaitIdle boundary and
 * the operation contributes exactly one native undo snapshot.
 */
ACS_EDITOR_API void acs_editor_scene_document_new(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return;
    PushUndo(*host);
    FSceneResourceRetirementScope retirement(*host);
    ClearScene(*host);
    ClearScene3D(*host);
}

// =============================================================================
// C ABI — コピー / ペースト (サブツリーのシリアライズ。クリップボードは C# 側が保持)
// =============================================================================

/** id のノードの subtree をシリアライズして返す (C# がクリップボードに保持)。 */
ACS_EDITOR_API const char* acs_editor_copy_subtree(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return "";
    AEditorNode* n = FindNode(*host, id);
    return (n != nullptr) ? SerializeSubtree(*host, n) : "";
}

/** subtree テキストを parent_id 配下へ貼り付ける (新規 id、内部親子は再マップ)。新根 id / 失敗 -1。 */
ACS_EDITOR_API int acs_editor_paste_subtree(void* handle, const char* text, int parent_id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || text == nullptr) return -1;
    PushUndo(*host);
    return PasteSubtree(*host, text, parent_id);
}

/** ノードのプレハブリンク (.acsprefab パス) を設定する (空でクリア)。成功 1。 */
ACS_EDITOR_API int acs_editor_node_set_prefab_src(void* handle, int id, const char* path) {
    auto* host = static_cast<FEditorHost*>(handle);
    AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr) return 0;
    std::snprintf(n->prefab_src, sizeof(n->prefab_src), "%s", (path != nullptr) ? path : "");
    return 1;
}

/** ノードのプレハブリンク (.acsprefab パス) を返す (インスタンスでなければ "")。 */
ACS_EDITOR_API const char* acs_editor_node_get_prefab_src(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    return (n != nullptr) ? n->prefab_src : "";
}

// =============================================================================
// C ABI — Undo / Redo (シーンスナップショット)
// -----------------------------------------------------------------------------
// 各変更操作は実行直前に PushUndo で現在状態を積む。undo は「現在→redo に退避して
// undo から復元」、redo はその逆。復元は LoadSceneText (内部) で行い、スタックは触らない。
// =============================================================================

/** 直前の変更を取り消す (成功 1 / 何もなければ 0)。 */
ACS_EDITOR_API int acs_editor_undo(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || host->undo.Num() == 0) return 0;
    char* cur = DupSnapshot(*host);                       // 現在状態を redo に退避
    if (cur != nullptr) host->redo.Add(cur);
    char* prev = host->undo[host->undo.Num() - 1];
    host->undo.Pop();
    RestoreSnapshot(*host, prev);                         // 2D + 3D を復元 (スタックは不変)
    delete[] prev;
    return 1;
}

/** 取り消した変更をやり直す (成功 1 / 何もなければ 0)。 */
ACS_EDITOR_API int acs_editor_redo(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || host->redo.Num() == 0) return 0;
    char* cur = DupSnapshot(*host);                       // 現在状態を undo に積む
    if (cur != nullptr) host->undo.Add(cur);
    char* next = host->redo[host->redo.Num() - 1];
    host->redo.Pop();
    RestoreSnapshot(*host, next);                         // 2D + 3D を復元
    delete[] next;
    return 1;
}

/** undo 可能か。 */
ACS_EDITOR_API int acs_editor_can_undo(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr && host->undo.Num() > 0) ? 1 : 0;
}

/** redo 可能か。 */
ACS_EDITOR_API int acs_editor_can_redo(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr && host->redo.Num() > 0) ? 1 : 0;
}

/** 連続編集 (ドラッグスクラブ等) を開始する。開始時点の状態を 1 度だけ undo に積み、
 *  以降の set_* が積む undo を end まで抑止する → ドラッグ全体が undo 1 ステップになる。 */
ACS_EDITOR_API void acs_editor_begin_continuous(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || host->suppress_undo) return;
    PushUndo(*host);                 // 開始状態の 1 スナップショット
    host->suppress_undo = true;
}

/** 連続編集を終了する (以降の set_* は通常どおり undo を積む)。 */
ACS_EDITOR_API void acs_editor_end_continuous(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
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
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr) ? PickNode(*host, screen_x, screen_y) : -1;
}

/** カメラをスクリーン量だけ平行移動する (ドラッグパン)。3D モードでは軌道回転。 */
ACS_EDITOR_API void acs_editor_camera_pan(void* handle, float dx, float dy) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return;
    if (host->view3d) {
        if (host->ortho3d) {                            // 2D (正射): 軌道せず注視点を画面平面で平行移動 (パン)
            IRhiSwapchain* sc = host->renderer.Swapchain();
            const f32 H = (sc != nullptr && sc->Height() > 0) ? static_cast<f32>(sc->Height()) : 1080.0f;
            const f32 wpp = (host->cam3d_dist * 0.62f * 2.0f) / H;   // world / px (正射の縦範囲 = dist*0.62*2)
            host->cam3d_target.x += dx * wpp;           // world +X が画面左 → ドラッグで内容が指に追従
            host->cam3d_target.y += dy * wpp;
            return;
        }
        host->cam3d_yaw = std::remainder(
            host->cam3d_yaw - dx * 0.01f, 2.0f * 3.14159265f);
                                                        // 3D 透視: ドラッグで軌道 (yaw/pitch)
        host->cam3d_pitch += dy * 0.01f;
        if (host->cam3d_pitch >  kCamera3DPitchLimit) host->cam3d_pitch =  kCamera3DPitchLimit;
        if (host->cam3d_pitch < -kCamera3DPitchLimit) host->cam3d_pitch = -kCamera3DPitchLimit;
        return;
    }
    host->cam_pan_x += dx; host->cam_pan_y += dy;
}

/** カメラを «真に» 平行移動 (パン)。3D 透視は注視点を camera の right/up 平面で移動 (中ドラッグ用)、
 *  正射は画面平面で移動。camera_pan が透視で «軌道» 回転なのに対し、こちらは平行移動。 */
ACS_EDITOR_API void acs_editor_camera_move(void* handle, float dx, float dy) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || !host->view3d) return;
    IRhiSwapchain* sc = host->renderer.Swapchain();
    const f32 H = (sc != nullptr && sc->Height() > 0) ? static_cast<f32>(sc->Height()) : 1080.0f;
    if (host->ortho3d) {                                // 正射: 画面平面で平行移動
        const f32 wpp = host->cam3d_dist * 0.62f * 2.0f / H;
        host->cam3d_target.x += dx * wpp;
        host->cam3d_target.y += dy * wpp;
        return;
    }
    // 透視: 注視点を camera の right/up 平面で移動 (grab-pan: 内容がカーソルに追従)。
    const FVec3 eye = Cam3DEye(*host);
    auto nrm = [](FVec3 v){ const f32 l = std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z); return l>1e-6f ? FVec3{v.x/l,v.y/l,v.z/l} : FVec3{0,0,1}; };
    auto crs = [](FVec3 a, FVec3 b){ return FVec3{a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; };
    const FVec3 fwd   = nrm(FVec3{ host->cam3d_target.x - eye.x, host->cam3d_target.y - eye.y, host->cam3d_target.z - eye.z });
    const FVec3 right = nrm(crs(fwd, FVec3{ 0, 1, 0 }));
    const FVec3 up    = crs(right, fwd);
    const f32 wpp = host->cam3d_dist * 0.9f / H;        // 距離比例 (注視点深度の world/px ≈ 2*dist*tan(fov/2)/H)
    // grab-pan: 内容がカーソルに追従するよう target を «ドラッグと逆» の screen 方向へ動かす。
    // この scene では cross(fwd, up) が screen-左を指すため +right がドラッグ方向、結果 grab-pan になる。
    host->cam3d_target.x += (right.x * dx + up.x * dy) * wpp;
    host->cam3d_target.y += (right.y * dx + up.y * dy) * wpp;
    host->cam3d_target.z += (right.z * dx + up.z * dy) * wpp;
}

/** アンカー (スクリーン点) を固定して factor 倍ズームする (ホイール)。3D モードではドリー。 */
ACS_EDITOR_API void acs_editor_camera_zoom(void* handle, float factor, float anchor_x, float anchor_y) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || factor <= 0.0f) return;
    if (host->view3d) {                                 // 3D: 注視点との距離を増減
        host->cam3d_dist /= factor;                     // factor>1 (ホイール上) で近づく
        if (host->cam3d_dist < kCamera3DMinDistance) host->cam3d_dist = kCamera3DMinDistance;
        if (host->cam3d_dist > kCamera3DMaxDistance) host->cam3d_dist = kCamera3DMaxDistance;
        return;
    }
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
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return;
    if (host->view3d) {                                  // 3D カメラを既定へ (正射=正面維持、透視=既定の俯瞰)
        host->cam3d_yaw   = host->ortho3d ? 0.0f : 0.78f;
        host->cam3d_pitch = host->ortho3d ? 0.0f : 0.55f;
        host->cam3d_dist  = 14.0f;
        host->cam3d_target = FVec3{ 0.0f, 1.0f, 0.0f };
        InvalidateTemporalRenderHistories(*host);
        return;
    }
    host->cam_pan_x = 0.0f; host->cam_pan_y = 0.0f; host->cam_zoom = 1.0f;
}

/** 現在のカメラ状態を取得する (UI 表示 / 検証用)。 */
ACS_EDITOR_API void acs_editor_camera_get(void* handle, float* pan_x, float* pan_y, float* zoom) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return;
    if (pan_x) *pan_x = host->cam_pan_x;
    if (pan_y) *pan_y = host->cam_pan_y;
    if (zoom)  *zoom  = host->cam_zoom;
}

/**
 * Resolve the legacy 2D Game View camera without mutating editor navigation.
 * The returned values are world center and zoom, not screen-space pan.
 */
ACS_EDITOR_API int acs_editor_game_camera2d_get(
        void* handle, unsigned viewport_width,
        unsigned viewport_height, float* center_x,
        float* center_y, float* zoom) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return 0;
    const FDeterministicGameCamera2D camera =
        ResolveDeterministicGameCamera2D(
            *host, viewport_width, viewport_height);
    if (center_x != nullptr) *center_x = camera.center_x;
    if (center_y != nullptr) *center_y = camera.center_y;
    if (zoom != nullptr) *zoom = camera.zoom;
    return 1;
}

/**
 * 3D camera stateを直接設定する。非有限値は状態を一切変更せず拒否し、
 * pitch / distance は対話操作と同じ安全範囲へ clamp する。
 */
ACS_EDITOR_API int acs_editor_camera3d_set(
        void* handle, float yaw, float pitch, float distance,
        float target_x, float target_y, float target_z) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr ||
        !std::isfinite(yaw) || !std::isfinite(pitch) || !std::isfinite(distance) ||
        !std::isfinite(target_x) || !std::isfinite(target_y) || !std::isfinite(target_z)) {
        return 0;
    }
    if (pitch >  kCamera3DPitchLimit) pitch =  kCamera3DPitchLimit;
    if (pitch < -kCamera3DPitchLimit) pitch = -kCamera3DPitchLimit;
    if (distance < kCamera3DMinDistance) distance = kCamera3DMinDistance;
    if (distance > kCamera3DMaxDistance) distance = kCamera3DMaxDistance;
    host->cam3d_yaw = std::remainder(yaw, 2.0f * 3.14159265f);
    host->cam3d_pitch = pitch;
    host->cam3d_dist = distance;
    host->cam3d_target = FVec3{ target_x, target_y, target_z };
    InvalidateTemporalRenderHistories(*host);
    return 1;
}

/** 3D camera stateを取得する。必要な出力だけを non-null pointer で指定できる。 */
ACS_EDITOR_API int acs_editor_camera3d_get(
        void* handle, float* yaw, float* pitch, float* distance,
        float* target_x, float* target_y, float* target_z) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return 0;
    if (yaw != nullptr) *yaw = host->cam3d_yaw;
    if (pitch != nullptr) *pitch = host->cam3d_pitch;
    if (distance != nullptr) *distance = host->cam3d_dist;
    if (target_x != nullptr) *target_x = host->cam3d_target.x;
    if (target_y != nullptr) *target_y = host->cam3d_target.y;
    if (target_z != nullptr) *target_z = host->cam3d_target.z;
    return 1;
}

/**
 * Resolve the exact camera Game View would consume without mutating editor
 * navigation state. source_node_id is -1 for the deterministic bounds
 * fallback; projection is 0=perspective, 1=orthographic.
 */
ACS_EDITOR_API int acs_editor_game_camera3d_get(
        void* handle, float aspect,
        int* projection, int* source_node_id,
        float* position3, float* forward3, float* up3,
        float* projection4) {
    auto* host = static_cast<FEditorHost*>(handle);
    // Keep public projection math inside a finite, useful domain.
    if (host == nullptr || !std::isfinite(aspect) ||
        aspect < 1.0e-4f || aspect > 1.0e4f)
        return 0;
    host->camera_resolve_nodes.Reset();
    Dfs3DCollect(
        &host->scene3d.Root(),
        host->camera_resolve_nodes);
    const FRenderCamera3D resolved =
        ResolveGameRenderCamera3D(
            *host, aspect,
            host->camera_resolve_nodes);
    if (projection != nullptr)
        *projection = resolved.orthographic ? 1 : 0;
    if (source_node_id != nullptr)
        *source_node_id = resolved.node_id;
    if (position3 != nullptr) {
        position3[0] = resolved.eye.x;
        position3[1] = resolved.eye.y;
        position3[2] = resolved.eye.z;
    }
    if (forward3 != nullptr) {
        forward3[0] = resolved.forward.x;
        forward3[1] = resolved.forward.y;
        forward3[2] = resolved.forward.z;
    }
    if (up3 != nullptr) {
        up3[0] = resolved.up.x;
        up3[1] = resolved.up.y;
        up3[2] = resolved.up.z;
    }
    if (projection4 != nullptr) {
        projection4[0] = resolved.fov_y_degrees;
        projection4[1] = resolved.orthographic_height;
        projection4[2] = resolved.near_plane;
        projection4[3] = resolved.far_plane;
    }
    return 1;
}

/**
 * Create one bounded logical Camera View request.
 *
 * The request owns camera identity, requested extent and temporal/target
 * generations, but no second swapchain. CameraViewRequestsV1 exposes this
 * distinction so managed code never infers a dedicated live renderer from a
 * successfully allocated request.
 */
ACS_EDITOR_API int acs_editor_camera_view_request_create(
        void* handle, int node_id, const char* stable_camera_id,
        std::uint32_t width, std::uint32_t height,
        std::uint64_t* out_request_id) {
    if (out_request_id != nullptr) *out_request_id = 0u;
    char stable_id_copy[
        game::kScene3DSerializeMaxCameraIdBytes + 1u]{};
    auto* host = static_cast<FEditorHost*>(handle);
    game::ANode* node =
        host != nullptr ? FindNode3DNode(*host, node_id) : nullptr;
    AEditor3DRecordComponent* record = Rec3D(node);
    if (host == nullptr || out_request_id == nullptr ||
        node == nullptr || record == nullptr ||
        !record->has_scene_camera ||
        !IsEditorCameraNodeEffectivelyEnabled(*node) ||
        !CopyCanonicalSceneCameraId(
            stable_camera_id,
            stable_id_copy,
            sizeof(stable_id_copy)) ||
        std::strcmp(
            record->scene_camera_id,
            stable_id_copy) != 0) {
        return 0;
    }
    return host->camera_view_requests.Create(
               node_id, stable_id_copy, width, height,
               *out_request_id)
        ? 1 : 0;
}

/** Update camera identity/extent without changing authored camera state. */
ACS_EDITOR_API int acs_editor_camera_view_request_update(
        void* handle, std::uint64_t request_id,
        int node_id, const char* stable_camera_id,
        std::uint32_t width, std::uint32_t height) {
    char stable_id_copy[
        game::kScene3DSerializeMaxCameraIdBytes + 1u]{};
    auto* host = static_cast<FEditorHost*>(handle);
    game::ANode* node =
        host != nullptr ? FindNode3DNode(*host, node_id) : nullptr;
    AEditor3DRecordComponent* record = Rec3D(node);
    if (host == nullptr || node == nullptr || record == nullptr ||
        !record->has_scene_camera ||
        !IsEditorCameraNodeEffectivelyEnabled(*node) ||
        !CopyCanonicalSceneCameraId(
            stable_camera_id,
            stable_id_copy,
            sizeof(stable_id_copy)) ||
        std::strcmp(
            record->scene_camera_id,
            stable_id_copy) != 0) {
        return 0;
    }
    return host->camera_view_requests.Update(
               request_id, node_id, stable_id_copy, width, height)
        ? 1 : 0;
}

/**
 * Bind the one physical presenter.
 *
 * This fails when a different request is already bound. Callers must first
 * complete HWND return and explicitly unbind the previous request.
 */
ACS_EDITOR_API int acs_editor_camera_view_request_bind_presenter(
        void* handle, std::uint64_t request_id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return 0;
    int node_id = -1;
    const char* stable_camera_id = nullptr;
    if (!host->camera_view_requests.RequestIdentity(
            request_id, node_id, stable_camera_id)) {
        return 0;
    }
    game::ANode* node = FindNode3DNode(*host, node_id);
    AEditor3DRecordComponent* record = Rec3D(node);
    if (node == nullptr || record == nullptr ||
        !record->has_scene_camera ||
        !IsEditorCameraNodeEffectivelyEnabled(*node) ||
        stable_camera_id == nullptr ||
        std::strcmp(
            record->scene_camera_id,
            stable_camera_id) != 0) {
        host->camera_view_requests.MarkCameraStale(request_id);
        return 0;
    }
    return host->camera_view_requests.BindPresenter(request_id)
        ? 1 : 0;
}

ACS_EDITOR_API int acs_editor_camera_view_request_unbind_presenter(
        void* handle, std::uint64_t request_id) {
    auto* host = static_cast<FEditorHost*>(handle);
    return host != nullptr &&
           host->camera_view_requests.UnbindPresenter(request_id)
        ? 1 : 0;
}

ACS_EDITOR_API int acs_editor_camera_view_request_destroy(
        void* handle, std::uint64_t request_id) {
    auto* host = static_cast<FEditorHost*>(handle);
    return host != nullptr &&
           host->camera_view_requests.Destroy(request_id)
        ? 1 : 0;
}

/**
 * Read latest request metadata. Deleted/disabled/replaced cameras are marked
 * stale and unbound before the snapshot is returned.
 */
ACS_EDITOR_API int acs_editor_camera_view_request_get(
        void* handle, std::uint64_t request_id,
        editor_camera_view::FSnapshot* out_snapshot,
        std::uint32_t snapshot_size) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (out_snapshot != nullptr &&
        snapshot_size >= sizeof(editor_camera_view::FSnapshot)) {
        *out_snapshot = editor_camera_view::FSnapshot{};
    }
    if (host == nullptr || out_snapshot == nullptr ||
        snapshot_size < sizeof(editor_camera_view::FSnapshot)) {
        return 0;
    }

    int node_id = -1;
    const char* stable_camera_id = nullptr;
    if (host->camera_view_requests.RequestIdentity(
            request_id, node_id, stable_camera_id)) {
        game::ANode* node = FindNode3DNode(*host, node_id);
        AEditor3DRecordComponent* record = Rec3D(node);
        if (node == nullptr || record == nullptr ||
            !record->has_scene_camera ||
            !IsEditorCameraNodeEffectivelyEnabled(*node) ||
            stable_camera_id == nullptr ||
            std::strcmp(
                record->scene_camera_id,
                stable_camera_id) != 0) {
            host->camera_view_requests.MarkCameraStale(request_id);
        }
    }
    return host->camera_view_requests.Snapshot(
               request_id, *out_snapshot)
        ? 1 : 0;
}

/**
 * Set a legacy non-persistent Camera View preview override.
 *
 * This never changes the authored active flag, scene dirty state, or undo
 * history. Inactive camera components may be previewed; disabled/hidden nodes
 * are rejected.
 */
ACS_EDITOR_API int acs_editor_game_camera_preview_set(
        void* handle, int node_id) {
    auto* host = static_cast<FEditorHost*>(handle);
    game::ANode* node =
        host != nullptr
            ? FindNode3DNode(*host, node_id) : nullptr;
    AEditor3DRecordComponent* record = Rec3D(node);
    if (host == nullptr || node == nullptr ||
        record == nullptr || !record->has_scene_camera ||
        !IsEditorCameraNodeEffectivelyEnabled(*node) ||
        host->camera_view_requests.PresenterRequestId() != 0u) {
        return 0;
    }
    host->game_camera_preview_node_id = node_id;
    return 1;
}

/** Clear the non-persistent Camera View preview override. */
ACS_EDITOR_API void acs_editor_game_camera_preview_clear(
        void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host != nullptr)
        host->game_camera_preview_node_id = -1;
}

/**
 * Return the currently valid preview override.
 *
 * Deleted or disabled overrides are cleared automatically and reported as
 * absent.
 */
ACS_EDITOR_API int acs_editor_game_camera_preview_get(
        void* handle, int* node_id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (node_id != nullptr) *node_id = -1;
    if (host == nullptr || node_id == nullptr) return 0;
    host->camera_resolve_nodes.Reset();
    Dfs3DCollect(
        &host->scene3d.Root(),
        host->camera_resolve_nodes);
    FResolvedSceneCamera3D resolved;
    if (!ResolvePreviewCamera3DFromNodes(
            *host, host->camera_resolve_nodes,
            resolved)) {
        return 0;
    }
    *node_id = resolved.record->id;
    return 1;
}

/**
 * Rebuild and return the bounded authored-camera enumeration.
 *
 * Ordering is deterministic scene DFS order. The following node_id_at calls
 * read this retained snapshot and do not rescan the complete scene.
 */
ACS_EDITOR_API int acs_editor_camera3d_count(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return 0;
    constexpr u32 kMaximumEnumeratedCameras = 256u;
    host->camera_resolve_nodes.Reset();
    host->camera_node_ids_scratch.Reset();
    Dfs3DCollect(
        &host->scene3d.Root(),
        host->camera_resolve_nodes);
    if (host->camera_node_ids_scratch.Max() <
        kMaximumEnumeratedCameras) {
        host->camera_node_ids_scratch.Reserve(
            kMaximumEnumeratedCameras);
    }
    for (u32 index = 0u;
         index < host->camera_resolve_nodes.Num() &&
         host->camera_node_ids_scratch.Num() <
             kMaximumEnumeratedCameras;
         ++index) {
        AEditor3DRecordComponent* record =
            Rec3D(host->camera_resolve_nodes[index]);
        if (record != nullptr && record->has_scene_camera)
            host->camera_node_ids_scratch.Add(record->id);
    }
    return static_cast<int>(
        host->camera_node_ids_scratch.Num());
}

ACS_EDITOR_API int acs_editor_camera3d_node_id_at(
        void* handle, int index) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || index < 0 ||
        static_cast<u32>(index) >=
            host->camera_node_ids_scratch.Num()) {
        return -1;
    }
    return host->camera_node_ids_scratch[
        static_cast<u32>(index)];
}

// =============================================================================
// C ABI — 3D ビューポート (モード切替 / ノード / 軌道カメラ / ピック)
// =============================================================================

/** 3D ビューポートの ON/OFF を切り替える。初回 ON で既定の 3D シーンを置く。 */
ACS_EDITOR_API void acs_editor_set_view3d(void* handle, int on) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return;
    const bool view3d = on != 0;
    if (host->view3d != view3d)
        InvalidateTemporalRenderHistories(*host);
    host->view3d = view3d;
    if (host->view3d) Seed3DScene(*host);
}

/** 現在 3D ビューポートか。 */
ACS_EDITOR_API int acs_editor_get_view3d(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr && host->view3d) ? 1 : 0;
}

/** Scene View-only selected camera frustum overlay toggle. */
ACS_EDITOR_API void acs_editor_camera_frustum_set_visible(
        void* handle, int visible) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host != nullptr)
        host->show_camera_frustum = visible != 0;
}

ACS_EDITOR_API int acs_editor_camera_frustum_get_visible(
        void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return host != nullptr && host->show_camera_frustum
        ? 1 : 0;
}

/** 3D ビューの投影を 正射(2D ビュー) / 透視 で切り替える。正射 ON でカメラを正面 (XY 平面直視) へ。 */
ACS_EDITOR_API void acs_editor_set_ortho3d(void* handle, int on) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return;
    const bool ortho3d = on != 0;
    if (host->ortho3d != ortho3d)
        InvalidateTemporalRenderHistories(*host);
    host->ortho3d = ortho3d;
    if (host->ortho3d) {                                 // 正射 = 2D 前面ビュー: XY 平面を正面から直視
        host->cam3d_yaw = 0.0f; host->cam3d_pitch = 0.0f;
        host->cam3d_target = FVec3{ 0, 0, 0 };
    }
}

/** 現在 正射(2D ビュー) か。 */
ACS_EDITOR_API int acs_editor_get_ortho3d(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr && host->ortho3d) ? 1 : 0;
}

/** 3D カメラを既定の俯瞰に戻す。 */
ACS_EDITOR_API void acs_editor_cam3d_reset(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return;
    host->cam3d_yaw = 0.78f; host->cam3d_pitch = 0.55f; host->cam3d_dist = 14.0f;
    host->cam3d_target = FVec3{ 0, 1.0f, 0 };
    InvalidateTemporalRenderHistories(*host);
}

/** 3D ノードを追加する (prim: 0=Cube 1=Sphere 2=Plane)。新ノード id を返す (失敗 -1)。 */
ACS_EDITOR_API int acs_editor_add_node3d(void* handle, int prim, const char* name) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return -1;
    const char* nm = (name != nullptr && name[0] != '\0') ? name
                   : (prim == 1 ? "Sphere" : prim == 2 ? "Plane" : "Cube");
    game::ANode& n = AddNode3D(*host, nm);
    const int id = host->next_id3d++;
    n.GetComponent<AEditor3DRecordComponent>()->id = id;
    game::AMeshComponent3D* m = n.GetComponent<game::AMeshComponent3D>();
    const int p = (prim >= 0 && prim <= 2) ? prim : 0;
    m->SetPrimitive(static_cast<game::EMeshPrimitive3D>(p));
    if (prim == 2) { n.Local().scale = FVec3{ 8, 1, 8 }; m->SetColor(FVec4{ 0.34f, 0.36f, 0.40f, 1 }); }
    else           { n.Local().position = FVec3{ 0, 0.5f, 0 }; }
    SetSel3D(*host, id);
    return id;
}

/** «空ノード» (描画しないグループ用トランスフォーム。2D の空ノード相当) を追加する。新 id (失敗 -1)。
 *  メッシュは描画ループでスキップされる (kind=6)。子をぶら下げる/整理する親として使う。 */
ACS_EDITOR_API int acs_editor_add_empty3d(void* handle, const char* name) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return -1;
    game::ANode& n = AddNode3D(*host, (name != nullptr && name[0] != '\0') ? name : "Empty");
    const int id = host->next_id3d++;
    AEditor3DRecordComponent* r = n.GetComponent<AEditor3DRecordComponent>();
    if (r != nullptr) { r->id = id; r->is_empty = true; }
    SetSel3D(*host, id);
    return id;
}

/** Add an authored camera node aligned to the current Scene View. */
ACS_EDITOR_API int acs_editor_add_camera3d(
    void* handle, const char* name, const char* stable_id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || !IsCanonicalSceneCameraId(stable_id)
        || !SceneCameraIdIsUnique(*host, -1, stable_id)) {
        return -1;
    }
    TArray<game::ANode*> nodes;
    Dfs3DCollect(&host->scene3d.Root(), nodes);
    u32 camera_count = 0u;
    for (u32 index = 0u; index < nodes.Num(); ++index) {
        AEditor3DRecordComponent* record = Rec3D(nodes[index]);
        if (record != nullptr && record->has_scene_camera) ++camera_count;
    }
    if (camera_count >= game::kScene3DSerializeMaxCameraCount) return -1;

    PushUndo(*host);
    game::ANode& node = AddNode3D(
        *host, (name != nullptr && name[0] != '\0') ? name : "Camera");
    AEditor3DRecordComponent* record = Rec3D(&node);
    if (record == nullptr) return -1;
    const int id = host->next_id3d++;
    record->id = id;
    record->is_empty = true;
    record->has_scene_camera = true;
    std::snprintf(
        record->scene_camera_id, sizeof(record->scene_camera_id),
        "%s", stable_id);
    record->scene_camera_projection = host->ortho3d ? 1 : 0;
    record->scene_camera_priority = 0;
    record->scene_camera_active = camera_count == 0u;
    record->scene_camera_fov_deg = 50.0f;
    record->scene_camera_ortho_height =
        host->cam3d_dist * 0.62f;
    record->scene_camera_near = 0.05f;
    record->scene_camera_far = 500.0f;

    const CCamera scene_view = EditorCam3D(*host, 1.0f);
    node.Local().position = scene_view.Eye();
    node.Local().rotation = FQuat::FromMatrix(Inverse(scene_view.View()));
    record->euler = node.Local().EulerDeg();
    SetSel3D(*host, id);
    return id;
}

ACS_EDITOR_API int acs_editor_node3d_camera_get(
    void* handle, int node_id,
    char* stable_id, int stable_cap,
    int* projection, int* priority, int* active,
    float* out4) {
    auto* host = static_cast<FEditorHost*>(handle);
    AEditor3DRecordComponent* record =
        host != nullptr ? Rec3D(FindNode3DNode(*host, node_id)) : nullptr;
    if (record == nullptr || !record->has_scene_camera
        || stable_id == nullptr || stable_cap <= 0
        || projection == nullptr || priority == nullptr
        || active == nullptr || out4 == nullptr) {
        return 0;
    }
    const int required =
        static_cast<int>(std::strlen(record->scene_camera_id)) + 1;
    if (required > stable_cap) {
        stable_id[0] = '\0';
        return 0;
    }
    std::memcpy(stable_id, record->scene_camera_id, required);
    *projection = record->scene_camera_projection;
    *priority = record->scene_camera_priority;
    *active = record->scene_camera_active ? 1 : 0;
    out4[0] = record->scene_camera_fov_deg;
    out4[1] = record->scene_camera_ortho_height;
    out4[2] = record->scene_camera_near;
    out4[3] = record->scene_camera_far;
    return 1;
}

ACS_EDITOR_API int acs_editor_node3d_camera_set(
    void* handle, int node_id, const char* stable_id,
    int projection, int priority, int active,
    float fov_deg, float ortho_height,
    float near_plane, float far_plane) {
    auto* host = static_cast<FEditorHost*>(handle);
    game::ANode* node =
        host != nullptr ? FindNode3DNode(*host, node_id) : nullptr;
    AEditor3DRecordComponent* record = Rec3D(node);
    if (host == nullptr || record == nullptr
        || !IsCanonicalSceneCameraId(stable_id)
        || !SceneCameraIdIsUnique(*host, node_id, stable_id)
        || !IsSceneCameraConfigValid(
            projection, priority, active, fov_deg, ortho_height,
            near_plane, far_plane)) {
        return 0;
    }
    if (!record->has_scene_camera) {
        TArray<game::ANode*> nodes;
        Dfs3DCollect(&host->scene3d.Root(), nodes);
        u32 camera_count = 0u;
        for (u32 index = 0u; index < nodes.Num(); ++index) {
            AEditor3DRecordComponent* candidate = Rec3D(nodes[index]);
            if (candidate != nullptr && candidate->has_scene_camera)
                ++camera_count;
        }
        if (camera_count >= game::kScene3DSerializeMaxCameraCount) return 0;
    }

    const bool was_temporal_owner =
        IsCurrentTemporalRenderCamera3D(*host, node);
    PushUndo(*host);
    if (active != 0) {
        TArray<game::ANode*> nodes;
        Dfs3DCollect(&host->scene3d.Root(), nodes);
        for (u32 index = 0u; index < nodes.Num(); ++index) {
            AEditor3DRecordComponent* candidate = Rec3D(nodes[index]);
            if (candidate != nullptr && candidate->has_scene_camera)
                candidate->scene_camera_active = false;
        }
    }
    record->has_scene_camera = true;
    std::snprintf(
        record->scene_camera_id, sizeof(record->scene_camera_id),
        "%s", stable_id);
    record->scene_camera_projection = projection;
    record->scene_camera_priority = priority;
    record->scene_camera_active = active != 0;
    record->scene_camera_fov_deg = fov_deg;
    record->scene_camera_ortho_height = ortho_height;
    record->scene_camera_near = near_plane;
    record->scene_camera_far = far_plane;
    const bool is_temporal_owner =
        IsCurrentTemporalRenderCamera3D(*host, node);
    if (was_temporal_owner || is_temporal_owner)
        InvalidateTemporalRenderHistories(*host);
    return 1;
}

ACS_EDITOR_API int acs_editor_node3d_camera_clear(
    void* handle, int node_id) {
    auto* host = static_cast<FEditorHost*>(handle);
    game::ANode* node =
        host != nullptr ? FindNode3DNode(*host, node_id) : nullptr;
    AEditor3DRecordComponent* record = Rec3D(node);
    if (host == nullptr || record == nullptr || !record->has_scene_camera)
        return 0;
    const bool was_temporal_owner =
        IsCurrentTemporalRenderCamera3D(*host, node);
    PushUndo(*host);
    record->has_scene_camera = false;
    record->scene_camera_id[0] = '\0';
    record->scene_camera_active = false;
    if (was_temporal_owner)
        InvalidateTemporalRenderHistories(*host);
    return 1;
}

ACS_EDITOR_API int acs_editor_scene3d_active_camera(
    void* handle, int* out_node_id,
    char* stable_id, int stable_cap,
    int* projection, int* priority, float* out4) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || out_node_id == nullptr
        || stable_id == nullptr || stable_cap <= 0
        || projection == nullptr || priority == nullptr
        || out4 == nullptr) {
        return 0;
    }
    FResolvedSceneCamera3D resolved;
    if (!ResolveActiveCamera3D(*host, resolved)
        || resolved.record == nullptr) {
        return 0;
    }
    const int required =
        static_cast<int>(std::strlen(resolved.record->scene_camera_id)) + 1;
    if (required > stable_cap) {
        stable_id[0] = '\0';
        return 0;
    }
    std::memcpy(
        stable_id, resolved.record->scene_camera_id, required);
    *out_node_id = resolved.record->id;
    *projection = resolved.record->scene_camera_projection;
    *priority = resolved.record->scene_camera_priority;
    out4[0] = resolved.record->scene_camera_fov_deg;
    out4[1] = resolved.record->scene_camera_ortho_height;
    out4[2] = resolved.record->scene_camera_near;
    out4[3] = resolved.record->scene_camera_far;
    return 1;
}

ACS_EDITOR_API int acs_editor_node3d_camera_align_to_view(
    void* handle, int node_id) {
    auto* host = static_cast<FEditorHost*>(handle);
    game::ANode* node =
        host != nullptr ? FindNode3DNode(*host, node_id) : nullptr;
    AEditor3DRecordComponent* record = Rec3D(node);
    if (host == nullptr || node == nullptr || record == nullptr
        || !record->has_scene_camera) {
        return 0;
    }
    return AlignSceneCameraNodeToView(*host, *node) ? 1 : 0;
}

/** 3D ノードの subtree を parent の下にクローンする。
 * transform/flags/components/custom mesh/sprite/procedural polygon/material/children を複製する。 */
static int CloneNode3DSubtree(FEditorHost& h, game::ANode* src, game::ANode* parent) noexcept {
    char nm[64];
    const FStringView sn = src->Name();
    const usize ln = (sn.Size() < sizeof(nm) - 1) ? sn.Size() : sizeof(nm) - 1;
    if (ln > 0) std::memcpy(nm, sn.Data(), ln);
    nm[ln] = '\0';
    game::ANode& clone = AddNode3D(h, nm);
    const int newId = h.next_id3d++;
    AEditor3DRecordComponent* sr = Rec3D(src);
    AEditor3DRecordComponent* cr = Rec3D(&clone);
    if (cr != nullptr) {
        cr->id = newId;
        if (sr != nullptr) {
            cr->euler = sr->euler;
            std::memcpy(
                cr->prefab_src, sr->prefab_src,
                sizeof(cr->prefab_src));
            if (cr->prefab_src[0] != '\0') {
                const char* source_identity = sr->prefab_instance_id[0] != '\0' ? sr->prefab_instance_id : sr->prefab_src;
                (void)MakeUniqueClonedPrefabInstanceId(
                    h, source_identity, newId, cr->prefab_instance_id,
                    static_cast<u32>(sizeof(cr->prefab_instance_id)));
            }
            std::memcpy(
                cr->sprite_path, sr->sprite_path,
                sizeof(cr->sprite_path));
            cr->is_empty = sr->is_empty;
            cr->poly_pts = sr->poly_pts.Clone();
            if (sr->has_scene_camera) {
                char cloned_camera_id[
                    game::kScene3DSerializeMaxCameraIdBytes + 1u]{};
                if (MakeUniqueClonedSceneCameraId(
                        h, sr->scene_camera_id, newId,
                        cloned_camera_id, sizeof(cloned_camera_id))) {
                    cr->has_scene_camera = true;
                    std::memcpy(
                        cr->scene_camera_id, cloned_camera_id,
                        sizeof(cr->scene_camera_id));
                    cr->scene_camera_projection =
                        sr->scene_camera_projection;
                    cr->scene_camera_priority = sr->scene_camera_priority;
                    cr->scene_camera_active = sr->scene_camera_active;
                    cr->scene_camera_fov_deg = sr->scene_camera_fov_deg;
                    cr->scene_camera_ortho_height =
                        sr->scene_camera_ortho_height;
                    cr->scene_camera_near = sr->scene_camera_near;
                    cr->scene_camera_far = sr->scene_camera_far;
                }
            }
            cr->component_count = sr->component_count;
            for (u32 slot = 0u;
                 slot < sr->component_count; ++slot) {
                cr->components[slot] = sr->components[slot];
                for (u32 prop = 0u;
                     prop < AEditor3DRecordComponent::kMaxProps;
                     ++prop) {
                    for (u32 lane = 0u; lane < 4u; ++lane) {
                        cr->comp_props[slot][prop][lane] =
                            sr->comp_props[slot][prop][lane];
                    }
                }
            }
        }
    }
    clone.Local() = src->Local();                    // transform をそのまま複製
    clone.SetVisible(src->IsVisible());
    clone.SetEnabled(src->IsEnabled());
    clone.SetDrawLayer(src->DrawLayer());
    game::AMeshComponent3D* sm = Mesh3D(src);
    game::AMeshComponent3D* cm = Mesh3D(&clone);
    if (sm != nullptr && cm != nullptr) {
        cm->SetPrimitive(sm->Primitive());
        cm->SetColor(sm->Color());
        cm->SetCastsShadow(sm->CastsShadow());
        if (sm->HasMeshAsset()) {
            cm->SetMeshAsset(sm->MeshAsset());
        }
        if (sm->MeshPath().Size() > 0u) {
            cm->SetMeshPath(sm->MeshPath());
        }
        if (sm->HasMaterial()) { cm->SetMaterialPath(sm->MaterialPath()); LoadNode3DMaterial(&clone); }
    }
    if (parent != nullptr && parent != &h.scene3d.Root()) clone.Reparent(*parent);   // 元と同じ親へ
    for (u32 i = 0; i < src->ChildCount(); ++i)
        CloneNode3DSubtree(h, src->Child(i), &clone);                                 // 子孫も複製
    return newId;
}

/** 3D ノードの subtree を複製し、トップに重なり回避の小オフセットを付けて選択する。返り値=トップ id (失敗 -1)。 */
static int DuplicateNode3D(FEditorHost& h, game::ANode* src) noexcept {
    const int newId = CloneNode3DSubtree(h, src, src->Parent());
    h.scene3d.Update(0.0f);    // 保留中の reparent を一括解決 (階層を確定)
    if (newId >= 0) {
        if (game::ANode* c = FindNode3DNode(h, newId)) c->Local().position.x += 1.0f;   // 元と重ならないよう +X
        SetSel3D(h, newId);
    }
    return newId;
}

static u32 CountSceneCamerasInSubtree(const game::ANode* node) noexcept {
    if (node == nullptr) return 0u;
    const AEditor3DRecordComponent* record = Rec3D(node);
    u32 count =
        record != nullptr && record->has_scene_camera ? 1u : 0u;
    for (u32 index = 0u; index < node->ChildCount(); ++index) {
        count += CountSceneCamerasInSubtree(node->Child(index));
    }
    return count;
}

static bool CanDuplicateNode3DSubtree(
    const FEditorHost& host, const game::ANode* source) noexcept {
    const u32 existing =
        CountSceneCamerasInSubtree(&host.scene3d.Root());
    const u32 incoming = CountSceneCamerasInSubtree(source);
    return existing <= game::kScene3DSerializeMaxCameraCount
        && incoming <=
            game::kScene3DSerializeMaxCameraCount - existing;
}

/** 3D ノード (とその子孫) を複製する。複製のトップを選択し、その id を返す (失敗 -1)。 */
ACS_EDITOR_API int acs_editor_node3d_duplicate(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return -1;
    game::ANode* src = FindNode3DNode(*host, id);
    if (src == nullptr || !CanDuplicateNode3DSubtree(*host, src)) return -1;
    PushUndo(*host);
    return DuplicateNode3D(*host, src);
}

/** 3D ノードをクリップボードへコピーする (コピー元 id を覚えるだけ)。 */
ACS_EDITOR_API void acs_editor_node3d_copy(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host != nullptr) host->clip3d = (FindNode3DNode(*host, id) != nullptr) ? id : -1;
}

/** クリップボードの 3D ノードを (元と同じ親へ) 貼り付ける。貼り付けたトップ id を返す (失敗 -1)。 */
ACS_EDITOR_API int acs_editor_node3d_paste(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return -1;
    game::ANode* src = FindNode3DNode(*host, host->clip3d);
    if (src == nullptr || !CanDuplicateNode3DSubtree(*host, src)) return -1;
    PushUndo(*host);
    return DuplicateNode3D(*host, src);
}

/** メッシュファイル (.gltf/.glb/.obj/.fbx) を 3D ノードとして読み込む。新ノード id (失敗 -1)。 */
ACS_EDITOR_API int acs_editor_add_mesh3d(void* handle, const char* path, const char* name) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || path == nullptr) return -1;
    TSharedPtr<AAsset> mesh = LoadMeshFile(path);
    if (!mesh) return -1;
    char nmbuf[64];
    if (name != nullptr && name[0] != '\0') std::snprintf(nmbuf, sizeof(nmbuf), "%s", name);
    else {
        const char* base = std::strrchr(path, '\\'); const char* base2 = std::strrchr(path, '/');
        if (base2 != nullptr && base2 > base) base = base2;
        std::snprintf(nmbuf, sizeof(nmbuf), "%s", (base != nullptr) ? base + 1 : path);
    }
    game::ANode& n = AddNode3D(*host, nmbuf);   // 名前は Spawn(nmbuf) で設定済み
    const int id = host->next_id3d++;
    n.GetComponent<AEditor3DRecordComponent>()->id = id;
    n.Local().position = FVec3{ 0, 0.5f, 0 };
    game::AMeshComponent3D* m = n.GetComponent<game::AMeshComponent3D>();
    m->SetMeshAsset(mesh);                          // 種別 Mesh + 所有 (ノード破棄で解放)
    m->SetColor(FVec4{ 0.78f, 0.78f, 0.82f, 1 });
    m->SetMeshPath(FStringView(path));              // 元ファイルパスを記録 (保存/再読込用)
    SetSel3D(*host, id);
    return id;
}

/** 画像ファイルを z=0 のスプライト (テクスチャ付きクアッド) として 3D シーンに追加。新 id (失敗 -1)。 */
ACS_EDITOR_API int acs_editor_add_sprite3d(void* handle, const char* path, const char* name) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || path == nullptr || path[0] == '\0') return -1;
    if (!Ensure3D(*host)) return -1;                // sprite パイプライン / device を準備
    u32 iw = 0, ih = 0;
    TUniquePtr<IRhiTexture> tex = LoadTexWithSize(*host, path, iw, ih);
    if (!tex || iw == 0 || ih == 0) { ACS_LOG_WARN("[3D] スプライト画像の読込に失敗: %s", path); return -1; }
    char nmbuf[64];
    if (name != nullptr && name[0] != '\0') std::snprintf(nmbuf, sizeof(nmbuf), "%s", name);
    else {
        const char* base = std::strrchr(path, '\\'); const char* base2 = std::strrchr(path, '/');
        if (base2 != nullptr && base2 > base) base = base2;
        std::snprintf(nmbuf, sizeof(nmbuf), "%s", (base != nullptr) ? base + 1 : path);
    }
    game::ANode& n = AddNode3D(*host, nmbuf);
    const int id = host->next_id3d++;
    AEditor3DRecordComponent* rec = n.GetComponent<AEditor3DRecordComponent>();
    rec->id = id;
    std::snprintf(rec->sprite_path, sizeof(rec->sprite_path), "%s", path);   // 再読込用に画像パスを保持 (シリアライズ)
    // 画像アスペクト比に合わせて scale (長辺 = 2.0 world)。単位クアッドは local XY ±0.5。
    const f32 aspect = static_cast<f32>(iw) / static_cast<f32>(ih);
    const f32 longSide = 2.0f;
    n.Local().scale    = FVec3{ (aspect >= 1.0f) ? longSide : longSide * aspect,
                                (aspect >= 1.0f) ? longSide / aspect : longSide, 1.0f };
    n.Local().position = FVec3{ 0, 1.0f, 0 };       // 原点より少し上 (床に埋まらない)
    game::AMeshComponent3D* m = n.GetComponent<game::AMeshComponent3D>();
    m->SetColor(FVec4{ 1, 1, 1, 1 });
    IRhiTexture* raw = tex.Get();                   // 所有は host->sprite_textures に移す (heap 上の実体は不動)
    host->sprite_textures.Add(Move(tex));
    m->SetRenderHandle(raw);                        // 描画パスが参照する «非所有» テクスチャポインタ
    SetSel3D(*host, id);
    return id;
}

/** 2D ポリゴン (XY 平面の点列 xy[count*2]) を 3D シーンのフラットノードとして追加する。新 id (失敗 -1)。 */
ACS_EDITOR_API int acs_editor_add_polygon3d(void* handle, const float* xy, int count,
                                            float r, float g, float b, float a, const char* name) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || xy == nullptr || count < 3) return -1;
    TArray<FVec2> pts; pts.Reserve(static_cast<usize>(count));
    for (int i = 0; i < count; ++i) pts.Add(FVec2{ xy[i * 2], xy[i * 2 + 1] });
    TSharedPtr<AAsset> mesh = MakeFlatPolygon3D(pts.GetData(), static_cast<u32>(count));
    if (!mesh) return -1;
    game::ANode& n = AddNode3D(*host, (name != nullptr && name[0] != '\0') ? name : "Polygon2D");
    const int id = host->next_id3d++;
    AEditor3DRecordComponent* rec = n.GetComponent<AEditor3DRecordComponent>();
    rec->id = id;
    rec->poly_pts = Move(pts);                           // 再生成用に元 2D 頂点列を保持 (シリアライズ。pts は以降不要)
    game::AMeshComponent3D* m = n.GetComponent<game::AMeshComponent3D>();
    m->SetMeshAsset(mesh);                               // prim=Mesh + 所有 (z=0 フラットメッシュ)
    m->SetColor(FVec4{ r, g, b, a });
    SetSel3D(*host, id);
    return id;
}

// --- Ortho ビューでのポリゴン «描画ツール» (クリックを z=0 へ逆射影して頂点を貯める) ---

/** Ortho ポリゴン描画を開始する (頂点バッファをクリア)。 */
ACS_EDITOR_API void acs_editor_poly3d_begin(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host != nullptr) host->poly3d_pts.Reset();
}

/** 描画中: スクリーン点を z=0 へ逆射影して頂点を追加する。追加後の頂点数を返す。 */
ACS_EDITOR_API int acs_editor_poly3d_add_point(void* handle, float sx, float sy) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return 0;
    IRhiSwapchain* sc = host->renderer.Swapchain();
    if (sc != nullptr) {
        const f32 W = static_cast<f32>(sc->Width()), H = static_cast<f32>(sc->Height());
        FVec2 xy;
        if (ScreenToZ0(*host, sx, sy, W, H, xy)) host->poly3d_pts.Add(xy);
    }
    return static_cast<int>(host->poly3d_pts.Num());
}

/** 描画を確定し、頂点列からフラットポリゴンノードを作る。新 id (頂点<3 で -1)。 */
ACS_EDITOR_API int acs_editor_poly3d_finalize(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || host->poly3d_pts.Num() < 3) { if (host != nullptr) host->poly3d_pts.Reset(); return -1; }
    TSharedPtr<AAsset> mesh = MakeFlatPolygon3D(host->poly3d_pts.GetData(), static_cast<u32>(host->poly3d_pts.Num()));
    if (!mesh) { host->poly3d_pts.Reset(); return -1; }
    game::ANode& n = AddNode3D(*host, "Polygon2D");
    const int id = host->next_id3d++;
    AEditor3DRecordComponent* rec = n.GetComponent<AEditor3DRecordComponent>();
    rec->id = id;
    rec->poly_pts = Move(host->poly3d_pts);              // 再生成用に元 2D 頂点列を保持 (シリアライズ。mesh は構築済み)
    host->poly3d_pts.Reset();                            // moved-from を明示的に空へ
    game::AMeshComponent3D* m = n.GetComponent<game::AMeshComponent3D>();
    m->SetMeshAsset(mesh);
    m->SetColor(FVec4{ 0.45f, 0.78f, 0.95f, 1 });
    SetSel3D(*host, id);
    return id;
}

/** Ortho ポリゴン描画をキャンセルする。 */
ACS_EDITOR_API void acs_editor_poly3d_cancel(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host != nullptr) host->poly3d_pts.Reset();
}

/** 描画中の頂点数。 */
ACS_EDITOR_API int acs_editor_poly3d_count(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr) ? static_cast<int>(host->poly3d_pts.Num()) : 0;
}

/** 3D ノードを削除する (成功 1)。 */
ACS_EDITOR_API int acs_editor_delete_node3d(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return 0;
    game::ANode* nn = FindNode3DNode(*host, id);
    if (nn == nullptr) return 0;
    TArray<game::ANode*> removed;
    removed.Add(nn);
    Dfs3DCollect(nn, removed);
    for (u32 i = 0u; i < removed.Num(); ++i) {
        AEditor3DRecordComponent* record = Rec3D(removed[i]);
        if (record != nullptr) {
            host->water3d.ClearDisturbancesForSurface(
                static_cast<u64>(
                    static_cast<u32>(record->id)));
            if (host->water_pointer_node == record->id) {
                host->water_pointer_valid = false;
                host->water_pointer_node = -1;
                host->water_pointer_emit_time = -1.0f;
            }
        }
    }
    nn->Destroy();
    host->scene3d.Update(0.0f);          // 破棄予定を purge + 即 reap (構造変更を確定)
    PruneSel3D(*host);                    // 削除されたノードを選択集合から除き primary を整える
    return 1;
}

/** 3D ノード数 (root を除く全ノード、階層含む)。 */
ACS_EDITOR_API int acs_editor_node3d_count(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return 0;
    TArray<game::ANode*> all; Dfs3DCollect(&host->scene3d.Root(), all);
    return static_cast<int>(all.Num());
}

/** DFS pre-order で index 番目の 3D ノード id (範囲外は -1)。階層は親→子の順で並ぶ。 */
ACS_EDITOR_API int acs_editor_node3d_id_at(void* handle, int index) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || index < 0) return -1;
    TArray<game::ANode*> all; Dfs3DCollect(&host->scene3d.Root(), all);
    if (static_cast<u32>(index) >= all.Num()) return -1;
    AEditor3DRecordComponent* r = Rec3D(all[static_cast<u32>(index)]);
    return (r != nullptr) ? r->id : -1;
}

/** ノードの親の editor id を返す (root 直下 / 無効は -1)。Hierarchy パネルの木構築用。 */
ACS_EDITOR_API int acs_editor_node3d_parent(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return -1;
    return ParentId3D(*host, FindNode3DNode(*host, id));
}

/** child を parent(=-1 で root) の子に付け替える。成功 1。 */
ACS_EDITOR_API int acs_editor_reparent3d(void* handle, int child_id, int parent_id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return 0;
    game::ANode* child = FindNode3DNode(*host, child_id);
    if (child == nullptr) return 0;
    game::ANode* parent = (parent_id < 0) ? &host->scene3d.Root() : FindNode3DNode(*host, parent_id);
    if (parent == nullptr) return 0;
    const bool resets_temporal_history =
        TransformAffectsCurrentTemporalRenderCamera3D(
            *host, child);
    child->Reparent(*parent);                 // cycle/自己は engine 側で弾く
    host->scene3d.Update(0.0f);               // 構造変更を即時解決
    if (resets_temporal_history)
        InvalidateTemporalRenderHistories(*host);
    return 1;
}

/** 3D ノードの名前を out (cap) へ書く。成功 1。 */
ACS_EDITOR_API int acs_editor_node3d_name(void* handle, int id, char* out, int cap) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || out == nullptr || cap <= 0) return 0;
    game::ANode* n = FindNode3DNode(*host, id);
    if (n == nullptr) return 0;
    const FStringView nv = n->Name();
    std::snprintf(out, static_cast<size_t>(cap), "%.*s", static_cast<int>(nv.Size()), nv.Data());
    return 1;
}

/** 3D ノードをリネームする。成功 1 / 失敗 0。Undo 可。 */
ACS_EDITOR_API int acs_editor_node3d_set_name(void* handle, int id, const char* name) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || name == nullptr || name[0] == '\0') return 0;
    game::ANode* n = FindNode3DNode(*host, id);
    if (n == nullptr) return 0;
    PushUndo(*host);
    n->SetName(FStringView(name));
    return 1;
}

/** 3D ノードの prim 種別 (0=Cube 1=Sphere 2=Plane 3=Mesh、無効は -1)。 */
ACS_EDITOR_API int acs_editor_node3d_prim(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return -1;
    game::ANode* n = FindNode3DNode(*host, id);
    return (n != nullptr) ? NPrim(n) : -1;
}

/** 3D ノードの «種別» を返す: 0=Cube 1=Sphere 2=Plane 3=Mesh 4=Sprite 5=Polygon (不明 -1)。
 *  prim だけでは sprite/polygon を見分けられない (内部 prim は Cube/Mesh のまま) ため別に公開する。 */
ACS_EDITOR_API int acs_editor_node3d_kind(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return -1;
    game::ANode* n = FindNode3DNode(*host, id);
    if (n == nullptr) return -1;
    AEditor3DRecordComponent* r = Rec3D(n);
    if (r != nullptr && r->is_empty)               return 6;       // Empty (描画しないグループ用トランスフォーム)
    if (r != nullptr && r->sprite_path[0] != '\0') return 4;       // Sprite (テクスチャ付きクアッド)
    if (r != nullptr && r->poly_pts.Num() >= 3)   return 5;       // Polygon (z=0 手続きメッシュ)
    return NPrim(n);                                               // 0..3 (Cube/Sphere/Plane/Mesh)
}

/** スプライトノードの画像パスを返す (スプライトでなければ "")。 */
ACS_EDITOR_API const char* acs_editor_node3d_sprite_get(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return "";
    game::ANode* n = FindNode3DNode(*host, id);
    AEditor3DRecordComponent* r = Rec3D(n);
    return (r != nullptr) ? r->sprite_path : "";
}

/** スプライトノードの画像を差し替える (テクスチャ再ロード)。成功 1。 */
ACS_EDITOR_API int acs_editor_node3d_set_sprite(void* handle, int id, const char* path) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || path == nullptr || path[0] == '\0') return 0;
    if (!Ensure3D(*host)) return 0;
    game::ANode* n = FindNode3DNode(*host, id);
    if (n == nullptr) return 0;
    AEditor3DRecordComponent* r = Rec3D(n);
    game::AMeshComponent3D* m = Mesh3D(n);
    if (r == nullptr || m == nullptr) return 0;
    u32 iw = 0, ih = 0;
    TUniquePtr<IRhiTexture> tex = LoadTexWithSize(*host, path, iw, ih);
    if (!tex || iw == 0 || ih == 0) { ACS_LOG_WARN("[3D] スプライト差替えの画像読込に失敗: %s", path); return 0; }
    std::snprintf(r->sprite_path, sizeof(r->sprite_path), "%s", path);
    IRhiTexture* raw = tex.Get();
    host->sprite_textures.Add(Move(tex));
    m->SetRenderHandle(raw);
    return 1;
}

/** スプライトノードの画像を外し、平面プリミティブへ戻す (2D node_clear_sprite の 3D 版)。成功 1。
 *  スプライト (sprite_path) でなければ何もせず 0。kind は以降 NPrim (= Plane) を返す。 */
ACS_EDITOR_API int acs_editor_node3d_clear_sprite(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    game::ANode* n = (host != nullptr) ? FindNode3DNode(*host, id) : nullptr;
    if (n == nullptr) return 0;
    AEditor3DRecordComponent* r = Rec3D(n);
    game::AMeshComponent3D* m = Mesh3D(n);
    if (r == nullptr || m == nullptr || r->sprite_path[0] == '\0') return 0;
    PushUndo(*host);
    r->sprite_path[0] = '\0';                                       // スプライト解除 (kind が NPrim に戻る)
    m->SetRenderHandle(nullptr);                                    // 描画のスプライトパスを無効化
    m->SetPrimitive(game::EMeshPrimitive3D::Plane);                 // クアッド相当の平面に戻す
    return 1;
}

/** 3D ノードに prefab/blueprint インスタンスリンク (.acsprefab/.acsbp パス) を張る (2D 版の 3D 対応)。成功 1。 */
ACS_EDITOR_API int acs_editor_node3d_set_prefab_src(void* handle, int id, const char* path) {
    auto* host = static_cast<FEditorHost*>(handle);
    AEditor3DRecordComponent* r = (host != nullptr) ? Rec3D(FindNode3DNode(*host, id)) : nullptr;
    if (r == nullptr || (path != nullptr && std::strlen(path) >= sizeof(r->prefab_src))) return 0;
    std::snprintf(r->prefab_src, sizeof(r->prefab_src), "%s", (path != nullptr) ? path : "");
    if (r->prefab_src[0] == '\0') r->prefab_instance_id[0] = '\0';
    return 1;
}

/** 3D ノードの prefab/blueprint リンクパスを返す (インスタンスでなければ "")。 */
ACS_EDITOR_API const char* acs_editor_node3d_get_prefab_src(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    AEditor3DRecordComponent* r = (host != nullptr) ? Rec3D(FindNode3DNode(*host, id)) : nullptr;
    return (r != nullptr) ? r->prefab_src : "";
}

/** 3D Prefab/Blueprintのsourceとstable instance IDを一括設定する。成功1。 */
ACS_EDITOR_API int acs_editor_node3d_set_prefab_link(
    void* handle, int id, const char* source,
    const char* instance_id) {
    auto* host = static_cast<FEditorHost*>(handle);
    return host != nullptr && SetPrefabLink3D_Internal(*host, id, source, instance_id) ? 1 : 0;
}

/** 3D Prefab/Blueprint instanceの32桁stable IDを返す。 */
ACS_EDITOR_API const char* acs_editor_node3d_get_prefab_instance_id(
    void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    AEditor3DRecordComponent* record = host != nullptr ? Rec3D(FindNode3DNode(*host, id)) : nullptr;
    return record != nullptr ? record->prefab_instance_id : "";
}

/** プリミティブノードの形状を切り替える (0=Cube 1=Sphere 2=Plane)。sprite/polygon/mesh は対象外。成功 1。 */
ACS_EDITOR_API int acs_editor_node3d_set_prim(void* handle, int id, int prim) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || prim < 0 || prim > 2) return 0;
    game::ANode* n = FindNode3DNode(*host, id);
    if (n == nullptr) return 0;
    AEditor3DRecordComponent* r = Rec3D(n);
    if (r != nullptr && (r->sprite_path[0] != '\0' || r->poly_pts.Num() >= 3)) return 0;   // sprite/polygon は不可
    game::AMeshComponent3D* m = Mesh3D(n);
    if (m == nullptr || m->Primitive() == game::EMeshPrimitive3D::Mesh) return 0;            // mesh アセットも不可
    m->SetPrimitive(static_cast<game::EMeshPrimitive3D>(prim));
    return 1;
}

/** 3D ノードの transform (pos/rot(度)/scale = 9 float) を取得する。成功 1。 */
ACS_EDITOR_API int acs_editor_node3d_get_transform(void* handle, int id, float* out9) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || out9 == nullptr) return 0;
    game::ANode* n = FindNode3DNode(*host, id);
    if (n == nullptr) return 0;
    const FVec3 p = n->Local().position, s = n->Local().scale;
    AEditor3DRecordComponent* r = Rec3D(n);
    const FVec3 e = (r != nullptr) ? r->euler : FVec3{ 0, 0, 0 };
    out9[0] = p.x; out9[1] = p.y; out9[2] = p.z;
    out9[3] = e.x; out9[4] = e.y; out9[5] = e.z;   // authored オイラー (度)
    out9[6] = s.x; out9[7] = s.y; out9[8] = s.z;
    return 1;
}

namespace {

constexpr u32 kNode3DTransformComponentCount = 9u;
constexpr u32 kNode3DTransformAllComponentsMask =
    (1u << kNode3DTransformComponentCount) - 1u;
constexpr u32 kNode3DRotationComponentsMask =
    (1u << 3u) | (1u << 4u) | (1u << 5u);
constexpr f32 kNode3DMinimumScaleMagnitude = 1.0e-4f;

[[nodiscard]] f32 CanonicalNode3DScale(f32 value) noexcept
{
    if (value >= 0.0f && value < kNode3DMinimumScaleMagnitude) {
        return kNode3DMinimumScaleMagnitude;
    }
    if (value < 0.0f && value > -kNode3DMinimumScaleMagnitude) {
        return -kNode3DMinimumScaleMagnitude;
    }
    return value;
}

[[nodiscard]] int SetNode3DTransformMasked(
    FEditorHost* host,
    int id,
    u32 component_mask,
    const f32* values) noexcept
{
    if (host == nullptr ||
        values == nullptr ||
        component_mask == 0u ||
        (component_mask & ~kNode3DTransformAllComponentsMask) != 0u) {
        return 0;
    }

    game::ANode* const node = FindNode3DNode(*host, id);
    if (node == nullptr) return 0;
    for (u32 index = 0u; index < kNode3DTransformComponentCount; ++index) {
        if ((component_mask & (1u << index)) != 0u &&
            !std::isfinite(values[index])) {
            return 0;
        }
    }

    const bool resets_temporal_history =
        TransformAffectsCurrentTemporalRenderCamera3D(*host, node);
    if ((component_mask & (1u << 0u)) != 0u) {
        node->Local().position.x = values[0];
    }
    if ((component_mask & (1u << 1u)) != 0u) {
        node->Local().position.y = values[1];
    }
    if ((component_mask & (1u << 2u)) != 0u) {
        node->Local().position.z = values[2];
    }

    if ((component_mask & kNode3DRotationComponentsMask) != 0u) {
        AEditor3DRecordComponent* const record = Rec3D(node);
        FVec3 euler =
            (record != nullptr) ? record->euler : FVec3{ 0.0f, 0.0f, 0.0f };
        if ((component_mask & (1u << 3u)) != 0u) euler.x = values[3];
        if ((component_mask & (1u << 4u)) != 0u) euler.y = values[4];
        if ((component_mask & (1u << 5u)) != 0u) euler.z = values[5];
        node->Local().SetEulerDeg(euler);
        if (record != nullptr) record->euler = euler;
    }

    if ((component_mask & (1u << 6u)) != 0u) {
        node->Local().scale.x = CanonicalNode3DScale(values[6]);
    }
    if ((component_mask & (1u << 7u)) != 0u) {
        node->Local().scale.y = CanonicalNode3DScale(values[7]);
    }
    if ((component_mask & (1u << 8u)) != 0u) {
        node->Local().scale.z = CanonicalNode3DScale(values[8]);
    }

    if (resets_temporal_history) {
        InvalidateTemporalRenderHistories(*host);
    }
    return 1;
}

} // namespace

/**
 * 3D ノードの transform の指定成分だけを設定する。
 * component_mask の bit 0..8 は get_transform の float 0..8 に対応する。
 * 未指定成分は読み書きせず、指定された scale 成分だけを invertible に正規化する。
 */
ACS_EDITOR_API int acs_editor_node3d_set_transform_masked(
    void* handle,
    int id,
    std::uint32_t component_mask,
    const float* values9,
    std::uint32_t value_count)
{
    if (value_count != kNode3DTransformComponentCount) return 0;
    return SetNode3DTransformMasked(
        static_cast<FEditorHost*>(handle),
        id,
        static_cast<u32>(component_mask),
        values9);
}

/** 3D ノードの transform を設定する。成功 1。 */
ACS_EDITOR_API int acs_editor_node3d_set_transform(void* handle, int id,
        float px, float py, float pz, float rx, float ry, float rz, float sx, float sy, float sz) {
    const f32 values[kNode3DTransformComponentCount]{
        px, py, pz, rx, ry, rz, sx, sy, sz,
    };
    return SetNode3DTransformMasked(
        static_cast<FEditorHost*>(handle),
        id,
        kNode3DTransformAllComponentsMask,
        values);
}

/** 3D ノードの色を取得する (rgba)。成功 1。 */
ACS_EDITOR_API int acs_editor_node3d_get_color(void* handle, int id, float* out4) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || out4 == nullptr) return 0;
    game::ANode* n = FindNode3DNode(*host, id);
    if (n == nullptr) return 0;
    const FVec4 c = NColor(n);
    out4[0] = c.x; out4[1] = c.y; out4[2] = c.z; out4[3] = c.w;
    return 1;
}

/** 3D ノードの色を設定する。成功 1。 */
ACS_EDITOR_API int acs_editor_node3d_set_color(void* handle, int id, float r, float g, float b, float a) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return 0;
    game::ANode* n = FindNode3DNode(*host, id);
    if (n == nullptr) return 0;
    game::AMeshComponent3D* m = Mesh3D(n);
    if (m != nullptr) m->SetColor(FVec4{ r, g, b, a });
    return 1;
}

// ===== 3D マテリアル (.acsmat アセット参照。2D の node_set/get/clear_material を忠実に鏡映) =====

/** 3D ノードに使用マテリアル (.acsmat パス) を割り当てる。即解析。成功 1 / 不明 0。 */
ACS_EDITOR_API int acs_editor_node3d_set_material(void* handle, int id, const char* utf8_path) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || utf8_path == nullptr) return 0;
    game::ANode* n = FindNode3DNode(*host, id);
    game::AMeshComponent3D* mc = Mesh3D(n);
    if (mc == nullptr) return 0;
    PushUndo(*host);
    mc->SetMaterialPath(FStringView{ utf8_path });   // 2D 同様 set_material に渡された絶対パスをそのまま保持
    LoadNode3DMaterial(n);
    return 1;
}

/** 3D ノードの使用マテリアルパス (UTF-8) を返す (未設定/不明は "")。 */
ACS_EDITOR_API const char* acs_editor_node3d_get_material(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    game::AMeshComponent3D* mc = (host != nullptr) ? Mesh3D(FindNode3DNode(*host, id)) : nullptr;
    if (mc == nullptr || mc->MaterialPath().Size() == 0) return "";
    return mc->MaterialPath().Data();   // FString の NUL 終端バッファ (C# 側で即コピー)
}

/** 3D ノードのマテリアルを外す。成功 1 / 不明 0。 */
ACS_EDITOR_API int acs_editor_node3d_clear_material(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    game::ANode* n = (host != nullptr) ? FindNode3DNode(*host, id) : nullptr;
    game::AMeshComponent3D* mc = Mesh3D(n);
    if (mc == nullptr) return 0;
    PushUndo(*host);
    mc->ClearMaterial();
    LoadNode3DMaterial(n);
    return 1;
}

/** 選択中の 3D ノード id (-1=なし)。 */
ACS_EDITOR_API int acs_editor_selected3d(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr) ? host->sel3d : -1;
}

/** 3D ノードを選択する (id<0 で選択解除)。単一選択 (集合を {id} に)。 */
ACS_EDITOR_API void acs_editor_select3d(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host != nullptr) SetSel3D(*host, id);
}
/** 3D 選択を反転する (Ctrl+click。multi-select)。 */
ACS_EDITOR_API void acs_editor_select3d_toggle(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host != nullptr) ToggleSel3D(*host, id);
}
/** id が 3D 選択集合に含まれるか。 */
ACS_EDITOR_API int acs_editor_node3d_is_selected(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr && Sel3DContains(*host, id)) ? 1 : 0;
}
/** 3D 選択集合の要素数。 */
ACS_EDITOR_API int acs_editor_selected3d_count(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr) ? static_cast<int>(host->sel3d_multi.Num()) : 0;
}
/** 3D 選択集合の index 番目の id (範囲外は -1)。 */
ACS_EDITOR_API int acs_editor_selected3d_at(void* handle, int index) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || index < 0 || index >= static_cast<int>(host->sel3d_multi.Num())) return -1;
    return host->sel3d_multi[static_cast<u32>(index)];
}
/** 3D 選択ノードを整列する (mode: 0=left/1=right/2=top/3=bottom/4=center-h/5=center-v、X=左右/Y=上下)。
 *  整列した数を返す (2 未満は 0)。XY 平面で揃える (2D ビュー編集を想定、z は不変)。 */
ACS_EDITOR_API int acs_editor_align3d_selection(void* handle, int mode) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return 0;
    f32 minx = 0, maxx = 0, miny = 0, maxy = 0; bool first = true; int cnt = 0;
    for (u32 i = 0; i < host->sel3d_multi.Num(); ++i) {
        game::ANode* n = FindNode3DNode(*host, host->sel3d_multi[i]);
        if (n == nullptr) continue;
        const FVec3 p = n->Local().position;
        if (first) { minx = maxx = p.x; miny = maxy = p.y; first = false; }
        else { if (p.x < minx) minx = p.x; if (p.x > maxx) maxx = p.x; if (p.y < miny) miny = p.y; if (p.y > maxy) maxy = p.y; }
        ++cnt;
    }
    if (cnt < 2) return 0;
    PushUndo(*host);
    const f32 cx = (minx + maxx) * 0.5f, cy = (miny + maxy) * 0.5f;
    int applied = 0;
    bool resets_temporal_history = false;
    for (u32 i = 0; i < host->sel3d_multi.Num(); ++i) {
        game::ANode* n = FindNode3DNode(*host, host->sel3d_multi[i]);
        if (n == nullptr) continue;
        resets_temporal_history =
            resets_temporal_history ||
            TransformAffectsCurrentTemporalRenderCamera3D(
                *host, n);
        FVec3 p = n->Local().position;
        switch (mode) {
            case 0: p.x = minx; break; case 1: p.x = maxx; break;     // left / right
            case 2: p.y = maxy; break; case 3: p.y = miny; break;     // top / bottom (Y+ が上)
            case 4: p.x = cx;   break; case 5: p.y = cy;   break;     // center-h / center-v
            default: break;
        }
        n->Local().position = p; ++applied;
    }
    if (resets_temporal_history)
        InvalidateTemporalRenderHistories(*host);
    return applied;
}

/** 3D 選択を axis (0=X, 1=Y) で均等分散する (2D distribute_selection の 3D 版)。
 *  両端は固定し中間ノードを等間隔に。z は不変。3 個未満は何もせず 0。 */
ACS_EDITOR_API int acs_editor_distribute3d_selection(void* handle, int axis) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return 0;
    TArray<int> ids; TArray<f32> pos;
    for (u32 i = 0; i < host->sel3d_multi.Num(); ++i) {
        game::ANode* n = FindNode3DNode(*host, host->sel3d_multi[i]);
        if (n == nullptr) continue;
        const FVec3 p = n->Local().position;
        ids.Add(host->sel3d_multi[i]);
        pos.Add(axis == 0 ? p.x : p.y);
    }
    const u32 n = ids.Num();
    if (n < 3) return 0;
    for (u32 i = 1; i < n; ++i) {                              // axis 位置で昇順 (挿入ソート、parallel)
        const f32 kp = pos[i]; const int ki = ids[i];
        u32 j = i;
        while (j > 0 && pos[j - 1] > kp) { pos[j] = pos[j - 1]; ids[j] = ids[j - 1]; --j; }
        pos[j] = kp; ids[j] = ki;
    }
    PushUndo(*host);
    const f32 step = (pos[n - 1] - pos[0]) / static_cast<f32>(n - 1);
    bool resets_temporal_history = false;
    for (u32 i = 1; i + 1 < n; ++i) {                          // 両端固定・中間を均等配置
        game::ANode* node = FindNode3DNode(*host, ids[i]);
        if (node == nullptr) continue;
        resets_temporal_history =
            resets_temporal_history ||
            TransformAffectsCurrentTemporalRenderCamera3D(
                *host, node);
        const f32 target = pos[0] + step * static_cast<f32>(i);
        if (axis == 0) node->Local().position.x = target;
        else           node->Local().position.y = target;
    }
    if (resets_temporal_history)
        InvalidateTemporalRenderHistories(*host);
    return static_cast<int>(n);
}

/** 3D シーンをテキストへシリアライズする (C# が保存)。
 *  成功時は書いた文字数、容量不足時は cap 以上を返すため、呼び出し側は grow/retry できる。
 *  形式: "N3D <id> <parent> <prim> px py pz rx ry rz sx sy sz r g b a <name>" (DFS、親が先)。 */
static bool AttachComponent3D(AEditor3DRecordComponent* r, const char* type_name) noexcept;   // 前方宣言 (load_text が使う)

/** 1 ノード分のブロック (N3D + MSH3D/SPR3D/PLY3D/CMP3D/CPROP3D/FLG3D/MAT3D) を out[cur..] へ追記する。
 *  parentOverride>=-1 ならその値を親 id として書く (-2 = 実際の親 ParentId3D を使う)。
 *  容量超過時は out を NUL 終端し *overflow=true として打ち切った cur を返す。返り値=追記後の cur。
 *  scene3d_serialize (全体) と copy_subtree3d (部分) の «単一ソース»。 */
static int EmitNode3DBlock(char* out, int cur, int cap, FEditorHost* host,
                           game::ANode* nn, int parentOverride, bool* overflow) noexcept {
    *overflow = false;
    AEditor3DRecordComponent* r = Rec3D(nn);
    if (r == nullptr) return cur;
    const FVec3 p = nn->Local().position, s = nn->Local().scale, e = r->euler;
    const FVec4 col = NColor(nn);
    const int prim = NPrim(nn);
    const int parent = (parentOverride >= -1) ? parentOverride : ParentId3D(*host, nn);
    char nm[128]; { const FStringView nv = nn->Name(); u32 ln = 0; for (; ln < nv.Size() && ln + 1u < sizeof(nm); ++ln) nm[ln] = nv[ln]; nm[ln] = '\0'; }
    const int w = std::snprintf(out + cur, static_cast<size_t>(cap - cur),
        "N3D %d %d %d %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.3f %.3f %.3f %.3f %s\n",
        r->id, parent, prim, p.x, p.y, p.z, e.x, e.y, e.z,
        s.x, s.y, s.z, col.x, col.y, col.z, col.w, nm);
    if (w < 0 || w >= cap - cur) { out[cap - 1] = '\0'; *overflow = true; return cur; }
    cur += w;
    if (r->has_scene_camera && cur < cap) {
        const int camera_written = std::snprintf(
            out + cur, static_cast<size_t>(cap - cur),
            "CAM3D %d %s %d %d %d %.9g %.9g %.9g %.9g\n",
            r->id, r->scene_camera_id,
            r->scene_camera_projection,
            r->scene_camera_priority,
            r->scene_camera_active ? 1 : 0,
            static_cast<double>(r->scene_camera_fov_deg),
            static_cast<double>(r->scene_camera_ortho_height),
            static_cast<double>(r->scene_camera_near),
            static_cast<double>(r->scene_camera_far));
        if (camera_written < 0 || camera_written >= cap - cur) {
            out[cap - 1] = '\0';
            *overflow = true;
            return cur;
        }
        cur += camera_written;
    }
    game::AMeshComponent3D* mc = Mesh3D(nn);
    if (prim == 3 && mc != nullptr && mc->MeshPath().Size() > 0 && cur < cap) {     // カスタムメッシュの元ファイル
        char mp[300]; { const FStringView pv = mc->MeshPath(); u32 ln = 0; for (; ln < pv.Size() && ln + 1u < sizeof(mp); ++ln) mp[ln] = pv[ln]; mp[ln] = '\0'; }
        const int w2 = std::snprintf(out + cur, static_cast<size_t>(cap - cur), "MSH3D %d %s\n", r->id, mp);
        if (w2 < 0 || w2 >= cap - cur) { out[cap - 1] = '\0'; *overflow = true; return cur; }
        cur += w2;
    }
    if (r->sprite_path[0] != '\0' && cur < cap) {                                  // スプライト (z=0 画像) の再読込パス
        const int w2 = std::snprintf(out + cur, static_cast<size_t>(cap - cur), "SPR3D %d %s\n", r->id, r->sprite_path);
        if (w2 < 0 || w2 >= cap - cur) { out[cap - 1] = '\0'; *overflow = true; return cur; }
        cur += w2;
    }
    if (r->poly_pts.Num() >= 3 && cur < cap) {                                    // 手続きポリゴン (z=0) の元 2D 頂点列
        int w2 = std::snprintf(out + cur, static_cast<size_t>(cap - cur), "PLY3D %d %u", r->id, static_cast<unsigned>(r->poly_pts.Num()));
        if (w2 < 0 || w2 >= cap - cur) { out[cap - 1] = '\0'; *overflow = true; return cur; }
        cur += w2;
        for (u32 k = 0; k < r->poly_pts.Num() && cur < cap; ++k) {
            const int wk = std::snprintf(out + cur, static_cast<size_t>(cap - cur), " %.4f %.4f", r->poly_pts[k].x, r->poly_pts[k].y);
            if (wk < 0 || wk >= cap - cur) { out[cap - 1] = '\0'; *overflow = true; return cur; }
            cur += wk;
        }
        if (cur < cap) out[cur++] = '\n';
    }
    for (u32 c = 0; c < r->component_count && cur < cap; ++c) {                     // アタッチ済みコンポーネント
        const game::FTypeDesc* d = game::CTypeRegistry::Get().FindById(r->components[c]);
        if (d == nullptr || d->name == nullptr) continue;
        const int wc = std::snprintf(out + cur, static_cast<size_t>(cap - cur), "CMP3D %d %s\n", r->id, d->name);
        if (wc < 0 || wc >= cap - cur) { out[cap - 1] = '\0'; *overflow = true; return cur; }
        cur += wc;
        const u32 nf = CompPropCount(d);                                            // 編集プロパティ値 (2D の CPROP に対応)
        for (u32 pp = 0; pp < nf && cur < cap; ++pp) {
            const f32* v = r->comp_props[c][pp];
            const int wp = std::snprintf(out + cur, static_cast<size_t>(cap - cur),
                "CPROP3D %d %u %u %.4f %.4f %.4f %.4f\n", r->id, c, pp, v[0], v[1], v[2], v[3]);
            if (wp < 0 || wp >= cap - cur) { out[cap - 1] = '\0'; *overflow = true; return cur; }
            cur += wp;
        }
    }
    if ((!nn->IsVisible() || !nn->IsEnabled()) && cur < cap) {                      // 可視/有効 (非既定のみ。既定=true)
        const int wf = std::snprintf(out + cur, static_cast<size_t>(cap - cur),
            "FLG3D %d %d %d\n", r->id, nn->IsVisible() ? 1 : 0, nn->IsEnabled() ? 1 : 0);
        if (wf < 0 || wf >= cap - cur) { out[cap - 1] = '\0'; *overflow = true; return cur; }
        cur += wf;
    }
    game::AMeshComponent3D* mc3 = Mesh3D(nn);                                       // マテリアル (.acsmat アセット参照)
    if (mc3 != nullptr && mc3->MaterialPath().Size() > 0 && cur < cap) {            // 新形式: «MAT3D id <path>» (SPR3D 同形式)
        char mpath[260]; const u32 ml = (mc3->MaterialPath().Size() < 259u) ? static_cast<u32>(mc3->MaterialPath().Size()) : 259u;
        std::memcpy(mpath, mc3->MaterialPath().Data(), ml); mpath[ml] = '\0';
        const int wm = std::snprintf(out + cur, static_cast<size_t>(cap - cur), "MAT3D %d %s\n", r->id, mpath);
        if (wm < 0 || wm >= cap - cur) { out[cap - 1] = '\0'; *overflow = true; return cur; }
        cur += wm;
    } else if (mc3 != nullptr && cur < cap &&                                       // 旧データ移行: path 無しで pbr 値のみ → 数値で温存
               (mc3->Material().pbr.metallic != 0.0f || mc3->Material().pbr.roughness != 0.5f)) {
        const int wm = std::snprintf(out + cur, static_cast<size_t>(cap - cur),
            "MAT3D %d %.3f %.3f\n", r->id, mc3->Material().pbr.metallic, mc3->Material().pbr.roughness);
        if (wm < 0 || wm >= cap - cur) { out[cap - 1] = '\0'; *overflow = true; return cur; }
        cur += wm;
    }
    if (r->prefab_src[0] != '\0' && cur < cap) {                                    // prefab/blueprint インスタンスリンク
        const int wp = std::snprintf(out + cur, static_cast<size_t>(cap - cur), "PFAB3D %d %s\n", r->id, r->prefab_src);
        if (wp < 0 || wp >= cap - cur) { out[cap - 1] = '\0'; *overflow = true; return cur; }
        cur += wp;
        if (r->prefab_instance_id[0] != '\0' && cur < cap) {
            const int wi = std::snprintf(out + cur, static_cast<size_t>(cap - cur), "PINS3D %d %s\n", r->id, r->prefab_instance_id);
            if (wi < 0 || wi >= cap - cur) { out[cap - 1] = '\0'; *overflow = true; return cur; }
            cur += wi;
        }
    }
    if (r->is_empty && cur < cap) {                                                 // 空ノード (描画しないグループ)
        const int we = std::snprintf(out + cur, static_cast<size_t>(cap - cur), "EMPTY3D %d\n", r->id);
        if (we < 0 || we >= cap - cur) { out[cap - 1] = '\0'; *overflow = true; return cur; }
        cur += we;
    }
    return cur;
}

/** Serializes the complete 3D scene.
 *  Returns the UTF-8 byte count excluding NUL. If the buffer is insufficient, returns cap (or
 *  greater for a header-only overflow), clears out[0], and the caller must grow and retry. */
ACS_EDITOR_API int acs_editor_scene3d_serialize(void* handle, char* out, int cap) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || out == nullptr || cap <= 0) return 0;
    int cur = std::snprintf(out, static_cast<size_t>(cap), "ACS3D v2\n");
    if (cur < 0) { out[0] = '\0'; return 0; }
    if (cur >= cap) { out[0] = '\0'; return cur; }
    if (host->sel3d >= 0) {                             // 選択ノード id (undo/redo/load で復元。2D の SEL 行と同様)
        const int written = std::snprintf(
            out + cur, static_cast<size_t>(cap - cur), "SEL3D %d\n", host->sel3d);
        if (written < 0) { out[0] = '\0'; return 0; }
        if (written >= cap - cur) { out[0] = '\0'; return cap; }
        cur += written;
    }
    TArray<game::ANode*> all; Dfs3DCollect(&host->scene3d.Root(), all);   // 親が子より先 (DFS pre-order)
    for (u32 i = 0; i < all.Num() && cur < cap; ++i) {
        bool ov = false;
        cur = EmitNode3DBlock(out, cur, cap, host, all[i], /*parentOverride=*/-2, &ov);   // -2 = 実際の親を使う
        if (ov) {
            out[0] = '\0';
            return cap;
        }
    }
    if (cur >= cap) {
        out[0] = '\0';
        return cap;
    }
    out[cur] = '\0';
    return cur;
}

/** 3D ノードの subtree を ACS3D テキストへシリアライズして返す (root の親= -1)。失敗/空は ""。
 *  プレハブ/Blueprint 保存・コピー (2D copy_subtree の 3D 版) が使う。バッファは host->scene_text (64KB)。 */
ACS_EDITOR_API const char* acs_editor_copy_subtree3d(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return "";
    game::ANode* root = FindNode3DNode(*host, id);
    if (root == nullptr) return "";
    TArray<game::ANode*> sub; sub.Add(root); Dfs3DCollect(root, sub);   // root + 子孫 (pre-order)
    char* buf = host->scene_text;
    const int cap = static_cast<int>(sizeof(host->scene_text));
    int cur = std::snprintf(buf, static_cast<size_t>(cap), "ACS3D v2\n");
    for (u32 i = 0; i < sub.Num() && cur < cap; ++i) {
        bool ov = false;
        const int parentOverride = (sub[i] == root) ? -1 : -2;   // root は親なし、子孫は実親 (subtree 内)
        cur = EmitNode3DBlock(buf, cur, cap, host, sub[i], parentOverride, &ov);
        if (ov) break;
    }
    buf[cur < cap ? cur : cap - 1] = '\0';
    return buf;
}

namespace {

bool AppendEditorTextLine(TArray<char>& output, const char* line) noexcept {
    const usize length = std::strlen(line);
    if (length > static_cast<usize>(~u32{0}) - output.Num() - 1u ||
        !output.TryReserve(output.Num() + static_cast<u32>(length) + 1u)) {
        return false;
    }
    for (usize index = 0u; index < length; ++index) {
        if (!output.TryAdd(line[index])) return false;
    }
    return output.TryAdd('\n');
}

bool ValidateEditorScene3DText(const char* text) noexcept {
    if (text == nullptr) return false;
    u32 text_size = 0u;
    while (text_size <= game::kScene3DSerializeMaxInputBytes &&
           text[text_size] != '\0') {
        ++text_size;
    }
    if (text_size > game::kScene3DSerializeMaxInputBytes) return false;

    const char* cursor = text;
    char line[game::kScene3DSerializeMaxLineBytes + 1u]{};
    if (ReadEditorTextLine(cursor, line, static_cast<u32>(sizeof(line))) !=
            EEditorTextLineResult::Line ||
        std::strcmp(line, "ACS3D v2") != 0) {
        return false;
    }

    TArray<char> structural;
    TArray<char> auxiliary;
    TArray<char> node_lines;
    TArray<FValidatedEditor3DNode> nodes;
    TArray<int> editor_only_targets;
    TArray<FValidatedEditorComponent> components;
    TArray<FValidatedEditorProperty> properties;
    TArray<FValidatedEditorPrefabLink> prefab_links;
    if (!AppendEditorTextLine(structural, "ACS3D v2")) return false;
    game::AcsRegisterEngineTypes();

    while (true) {
        const EEditorTextLineResult read = ReadEditorTextLine(
            cursor, line, static_cast<u32>(sizeof(line)));
        if (read == EEditorTextLineResult::End) break;
        if (read != EEditorTextLineResult::Line) return false;
        if (line[0] == '\0') continue;

        if (IsEditorTextDirective(line, "N3D")) {
            if (line[3] != ' ') return false;
            const char* values = line + 4u;
            int id = -1;
            int parent = -1;
            int primitive = 0;
            f32 ignored_float = 0.0f;
            if (!ParseEditorTextInt(values, id) ||
                !ParseEditorTextInt(values, parent) ||
                !ParseEditorTextInt(values, primitive) ||
                id < 0 || parent < -1 ||
                primitive < -1 || primitive > 3) {
                return false;
            }
            for (u32 index = 0u; index < 13u; ++index) {
                if (!ParseEditorTextFloat(values, ignored_float)) return false;
            }
            SkipEditorTextWhitespace(values);
            if (std::strlen(values) > 63u) return false;
            if (nodes.Num() >= game::kScene3DSerializeMaxNodeCount) return false;
            const u32 offset = node_lines.Num();
            if (!AppendEditorTextLine(node_lines, line)) return false;
            nodes.Add(FValidatedEditor3DNode{
                id, parent, -1, offset,
                static_cast<u32>(node_lines.Num() - offset), 0u, 0u});
            continue;
        }

        const bool mesh = IsEditorTextDirective(line, "MSH3D");
        const bool material = IsEditorTextDirective(line, "MAT3D");
        const bool component = IsEditorTextDirective(line, "CMP3D");
        const bool property = IsEditorTextDirective(line, "CPROP3D");
        const bool camera = IsEditorTextDirective(line, "CAM3D");
        const bool canonical_auxiliary =
            mesh || material || camera ||
            IsEditorTextDirective(line, "FLG3D") ||
            IsEditorTextDirective(line, "EMPTY3D") ||
            IsEditorTextDirective(line, "SEL3D");
        if (component || property) {
            if (line[component ? 5u : 7u] != ' ') return false;
            const char* values = line + (component ? 5u : 7u);
            int id = -1;
            if (!ParseEditorTextInt(values, id)) return false;
            if (component) {
                char type_name[256]{};
                if (!ParseEditorTextWord(
                        values, type_name,
                        static_cast<usize>(sizeof(type_name))) ||
                    !EditorTextOnlyWhitespace(values)) {
                    return false;
                }
                const game::FTypeDesc* descriptor =
                    game::CTypeRegistry::Get().FindByName(type_name);
                if (descriptor == nullptr ||
                    descriptor->category != game::ETypeCategory::Component) {
                    return false;
                }
                u32 count = 0u;
                for (u32 index = 0u; index < components.Num(); ++index) {
                    if (components[index].node_id != id) continue;
                    if (components[index].type_id == descriptor->id) return false;
                    ++count;
                }
                if (count >= AEditor3DRecordComponent::kMaxComponents) {
                    return false;
                }
                components.Add(
                    FValidatedEditorComponent{id, descriptor->id});
            } else {
                u32 slot = 0u;
                u32 property_index = 0u;
                if (!ParseEditorTextU32(values, slot) ||
                    !ParseEditorTextU32(values, property_index) ||
                    slot >= AEditor3DRecordComponent::kMaxComponents ||
                    property_index >= AEditor3DRecordComponent::kMaxProps) {
                    return false;
                }
                for (u32 index = 0u; index < 4u; ++index) {
                    f32 value = 0.0f;
                    if (!ParseEditorTextFloat(values, value)) return false;
                }
                if (!EditorTextOnlyWhitespace(values)) return false;
                properties.Add(
                    FValidatedEditorProperty{id, slot, property_index});
            }
            editor_only_targets.Add(id);
            continue;
        }
        if (canonical_auxiliary) {
            // Keep editor storage limits in lock-step with the commit parser;
            // the canonical parser supplies the rest of the grammar checks.
            if (mesh || material || component) {
                const char* values =
                    line + (mesh ? 5u : (material ? 5u : 5u));
                int id = -1;
                const usize maximum =
                    mesh ? 259u : (material ? 255u : 255u);
                if (!ParseEditorTextInt(values, id) ||
                    !ParseEditorTextRemainder(values, maximum)) {
                    return false;
                }
                if (material) {
                    const char* numeric = values;
                    f32 metallic = 0.0f;
                    f32 roughness = 0.0f;
                    // The canonical grammar accepts either exactly two
                    // finite legacy PBR values or a non-empty safe material
                    // asset path. The canonical staging parse below performs
                    // the path-byte validation.
                    (void)(ParseEditorTextFloat(numeric, metallic) &&
                           ParseEditorTextFloat(numeric, roughness) &&
                           EditorTextOnlyWhitespace(numeric));
                }
            }
            if (IsEditorTextDirective(line, "SEL3D")) {
                const char* values = line + 5u;
                int selected = -1;
                if (!ParseEditorTextInt(values, selected) ||
                    !EditorTextOnlyWhitespace(values) ||
                    selected < 0) {
                    return false;
                }
                // ACS3D reserves zero for «no selection». Every positive
                // selection is a real scene reference and is resolved after
                // the order-independent node preflight below.
                if (selected > 0) editor_only_targets.Add(selected);
            }
            if (!AppendEditorTextLine(auxiliary, line)) return false;
            continue;
        }

        if (IsEditorTextDirective(line, "SPR3D") ||
            IsEditorTextDirective(line, "PFAB3D")) {
            const bool sprite = IsEditorTextDirective(line, "SPR3D");
            if (line[sprite ? 5u : 6u] != ' ') return false;
            const char* values = line + (sprite ? 5u : 6u);
            int id = -1;
            if (!ParseEditorTextInt(values, id) ||
                !ParseEditorTextRemainder(values, 255u)) {
                return false;
            }
            if (!sprite) {
                for (u32 index = 0u; index < prefab_links.Num(); ++index) {
                    if (prefab_links[index].node_id == id) return false;
                }
                prefab_links.Add(FValidatedEditorPrefabLink{id});
            }
            editor_only_targets.Add(id);
            continue;
        }
        if (IsEditorTextDirective(line, "PINS3D")) {
            if (line[6] != ' ') return false;
            const char* values = line + 7u;
            int id = -1;
            char instance_id[game::kScene3DSerializePrefabInstanceIdBytes + 1u]{};
            if (!ParseEditorTextInt(values, id) || !ParseEditorTextWord(values, instance_id, sizeof(instance_id)) || !EditorTextOnlyWhitespace(values) || !IsCanonicalPrefabInstanceId(instance_id)) return false;
            FValidatedEditorPrefabLink* link = nullptr;
            for (u32 index = 0u; index < prefab_links.Num(); ++index) {
                if (prefab_links[index].node_id == id) link = &prefab_links[index];
                if (prefab_links[index].has_instance_id && std::strcmp(prefab_links[index].instance_id, instance_id) == 0) return false;
            }
            if (link == nullptr || link->has_instance_id) return false;
            link->has_instance_id = true;
            std::snprintf(link->instance_id, sizeof(link->instance_id), "%s", instance_id);
            editor_only_targets.Add(id);
            continue;
        }
        if (IsEditorTextDirective(line, "PLY3D")) {
            if (line[5] != ' ') return false;
            const char* values = line + 5u;
            int id = -1;
            u32 count = 0u;
            if (!ParseEditorTextInt(values, id) ||
                !ParseEditorTextU32(values, count) ||
                count < 3u || count > 4096u) {
                return false;
            }
            for (u32 index = 0u; index < count; ++index) {
                f32 x = 0.0f;
                f32 y = 0.0f;
                if (!ParseEditorTextFloat(values, x) ||
                    !ParseEditorTextFloat(values, y)) {
                    return false;
                }
            }
            if (!EditorTextOnlyWhitespace(values)) return false;
            editor_only_targets.Add(id);
            continue;
        }

        // Unknown directives are intentionally ignored for forward
        // compatibility. A second ACS3D header is never an extension.
        if (IsEditorTextDirective(line, "ACS3D")) return false;
    }

    if (nodes.Num() > 1u) {
        std::sort(
            nodes.GetData(), nodes.GetData() + nodes.Num(),
            [](const FValidatedEditor3DNode& left,
               const FValidatedEditor3DNode& right) noexcept {
                return left.id < right.id;
            });
    }
    for (u32 index = 0u; index < nodes.Num(); ++index) {
        if (index > 0u && nodes[index - 1u].id == nodes[index].id) {
            return false;
        }
        if (nodes[index].parent < 0) continue;
        const int parent_index =
            FindValidatedEditor3DNode(nodes, nodes[index].parent);
        if (parent_index < 0 || parent_index == static_cast<int>(index)) {
            return false;
        }
        nodes[index].parent_index = parent_index;
    }

    TArray<u32> hierarchy_chain;
    for (u32 index = 0u; index < nodes.Num(); ++index) {
        if (nodes[index].state == 2u) continue;
        hierarchy_chain.Reset();
        int current = static_cast<int>(index);
        while (current >= 0 &&
               nodes[static_cast<u32>(current)].state != 2u) {
            FValidatedEditor3DNode& node =
                nodes[static_cast<u32>(current)];
            if (node.state == 1u) return false;
            node.state = 1u;
            hierarchy_chain.Add(static_cast<u32>(current));
            current = node.parent_index;
        }
        u32 depth = current >= 0
            ? nodes[static_cast<u32>(current)].depth + 1u
            : 0u;
        while (!hierarchy_chain.IsEmpty()) {
            const u32 chain_index =
                hierarchy_chain[hierarchy_chain.Num() - 1u];
            hierarchy_chain.Pop();
            nodes[chain_index].depth = depth++;
            nodes[chain_index].state = 2u;
            if (nodes[chain_index].depth >
                game::kScene3DSerializeMaxTreeDepth) {
                return false;
            }
        }
    }

    TArray<u32> topological_order;
    if (!topological_order.TryReserve(nodes.Num())) return false;
    for (u32 index = 0u; index < nodes.Num(); ++index) {
        if (!topological_order.TryAdd(index)) return false;
    }
    if (topological_order.Num() > 1u) {
        std::sort(
            topological_order.GetData(),
            topological_order.GetData() + topological_order.Num(),
            [&](u32 left, u32 right) noexcept {
                if (nodes[left].depth != nodes[right].depth)
                    return nodes[left].depth < nodes[right].depth;
                return nodes[left].id < nodes[right].id;
            });
    }
    for (u32 order_index = 0u;
         order_index < topological_order.Num(); ++order_index) {
        const FValidatedEditor3DNode& node =
            nodes[topological_order[order_index]];
        if (!structural.TryReserve(structural.Num() + node.text_length)) {
            return false;
        }
        for (u32 byte_index = 0u;
             byte_index < node.text_length; ++byte_index) {
            if (!structural.TryAdd(
                    node_lines[node.text_offset + byte_index])) {
                return false;
            }
        }
    }

    for (u32 index = 0u; index < properties.Num(); ++index) {
        const FValidatedEditorProperty& property_record = properties[index];
        u32 slot = 0u;
        const FValidatedEditorComponent* component_record = nullptr;
        for (u32 component_index = 0u;
             component_index < components.Num(); ++component_index) {
            if (components[component_index].node_id !=
                property_record.node_id) {
                continue;
            }
            if (slot == property_record.slot) {
                component_record = &components[component_index];
                break;
            }
            ++slot;
        }
        if (component_record == nullptr) return false;
        const game::FTypeDesc* descriptor =
            game::CTypeRegistry::Get().FindById(component_record->type_id);
        if (descriptor == nullptr ||
            property_record.property >= descriptor->field_count) {
            return false;
        }
    }

    if (!structural.TryReserve(structural.Num() + auxiliary.Num())) {
        return false;
    }
    for (u32 index = 0u; index < auxiliary.Num(); ++index) {
        if (!structural.TryAdd(auxiliary[index])) return false;
    }

    game::CSceneNodeGraph staging;
    const game::FScene3DLoadResult parsed = game::TryLoadScene3DText(
        staging, structural.GetData(), structural.Num());
    if (!parsed.Succeeded()) return false;

    for (u32 index = 0u; index < editor_only_targets.Num(); ++index) {
        if (staging.Root().FindBySerialId(editor_only_targets[index]) ==
            nullptr) {
            return false;
        }
    }
    return true;
}

/** 3D シーンテキストの解析本体。clear=true で全置換 (load_text)、false で追記 (paste_subtree3d)。
 *  idOffset を読み取った全 id に加算 (paste の id 衝突回避)。reparentRootTo>=0 なら «親 -1 の root» を
 *  その id 配下へ繋ぐ (paste のドロップ先)。out_root に最初の root の新 id を返す。成功 1。 */
static int LoadScene3DTextImpl(FEditorHost* host, const char* text, bool clear,
                               int idOffset, int reparentRootTo, int* out_root,
                               bool prevalidated) noexcept {
    if (host == nullptr || text == nullptr) return 0;
    // Validate before the retirement boundary so a rejected source never
    // destroys the currently published world.
    if (!prevalidated && !ValidateEditorScene3DText(text)) return 0;
    if (!clear) {
        TArray<game::ANode*> existing_nodes;
        Dfs3DCollect(&host->scene3d.Root(), existing_nodes);
        u32 existing_camera_count = 0u;
        for (u32 index = 0u; index < existing_nodes.Num(); ++index) {
            const AEditor3DRecordComponent* record =
                Rec3D(existing_nodes[index]);
            if (record != nullptr && record->has_scene_camera)
                ++existing_camera_count;
        }
        if (existing_camera_count > game::kScene3DSerializeMaxCameraCount)
            return 0;
        u32 incoming_camera_count = 0u;
        const char* camera_cursor = text;
        char camera_line[game::kScene3DSerializeMaxLineBytes + 1u]{};
        while (*camera_cursor != '\0') {
            u32 length = 0u;
            while (*camera_cursor != '\0' && *camera_cursor != '\n') {
                if (length + 1u >= sizeof(camera_line)) return 0;
                camera_line[length++] = *camera_cursor++;
            }
            if (length > 0u && camera_line[length - 1u] == '\r') --length;
            camera_line[length] = '\0';
            if (*camera_cursor == '\n') ++camera_cursor;
            if (std::strncmp(camera_line, "CAM3D ", 6u) != 0) continue;
            int ignored_node_id = -1;
            char stable_id[game::kScene3DSerializeMaxCameraIdBytes + 1u]{};
            if (std::sscanf(
                    camera_line, "CAM3D %d %64s",
                    &ignored_node_id, stable_id) != 2
                || !IsCanonicalSceneCameraId(stable_id)
                || ++incoming_camera_count
                    > game::kScene3DSerializeMaxCameraCount
                        - existing_camera_count) {
                return 0;
            }
        }
    }
    if (clear) {
        ClearScene3D(*host);
    }
    host->scene3d_seeded = true;                     // 読み込んだら seed しない
    int maxId = 0;
    int restoredSel = -1;                            // SEL3D 行があれば選択を復元 (無ければ先頭)
    int firstRoot = -1;                              // paste: 最初に作った «親 -1» ノードの新 id (選択用)
    char line[4096];                          // ポリゴンの点列 (PLY3D) も収まる広さ

    // Pass 1 creates every node before any relationship or auxiliary record is
    // applied. Valid documents are therefore independent of declaration order.
    TArray<int> loaded_ids;
    TArray<int> loaded_parents;
    const char* structural_cursor = text;
    while (*structural_cursor != '\0') {
        u32 length = 0u;
        while (*structural_cursor != '\0' && *structural_cursor != '\n') {
            if (length + 1u < sizeof(line)) line[length++] = *structural_cursor;
            ++structural_cursor;
        }
        if (length > 0u && line[length - 1u] == '\r') --length;
        line[length] = '\0';
        if (*structural_cursor == '\n') ++structural_cursor;
        if (std::strncmp(line, "N3D ", 4) != 0) continue;

        int nid = 0;
        int nparent = -1;
        int nprim = 0;
        char name[64] = {};
        FVec3 position{ 0, 0, 0 };
        FVec3 rotation{ 0, 0, 0 };
        FVec3 scale{ 1, 1, 1 };
        FVec4 color{ 0.8f, 0.8f, 0.85f, 1 };
        const int got = std::sscanf(
            line,
            "N3D %d %d %d %f %f %f %f %f %f %f %f %f %f %f %f %f %63[^\n]",
            &nid, &nparent, &nprim,
            &position.x, &position.y, &position.z,
            &rotation.x, &rotation.y, &rotation.z,
            &scale.x, &scale.y, &scale.z,
            &color.x, &color.y, &color.z, &color.w, name);
        if (got < 16) return 0; // unreachable after strict preflight

        const int new_id = nid + idOffset;
        game::ANode& node = AddNode3D(*host, got >= 17 ? name : "Node");
        node.Local().position = position;
        node.Local().scale = scale;
        node.Local().SetEulerDeg(rotation);
        AEditor3DRecordComponent* record =
            node.GetComponent<AEditor3DRecordComponent>();
        if (record != nullptr) {
            record->id = new_id;
            record->euler = rotation;
            record->is_empty = nprim < 0;
        }
        game::AMeshComponent3D* mesh = node.GetComponent<game::AMeshComponent3D>();
        if (mesh != nullptr && nprim >= 0) {
            mesh->SetPrimitive(static_cast<game::EMeshPrimitive3D>(nprim));
            mesh->SetColor(color);
        }

        const int effective_parent =
            nparent >= 0 ? nparent + idOffset : reparentRootTo;
        loaded_ids.Add(new_id);
        loaded_parents.Add(effective_parent);
        if (nparent < 0 && firstRoot < 0) firstRoot = new_id;
        if (new_id > maxId) maxId = new_id;
    }
    for (u32 index = 0u; index < loaded_ids.Num(); ++index) {
        if (loaded_parents[index] < 0) continue;
        game::ANode* node = FindNode3DNode(*host, loaded_ids[index]);
        game::ANode* parent = FindNode3DNode(*host, loaded_parents[index]);
        if (node != nullptr && parent != nullptr && node->Parent() != parent) {
            node->Reparent(*parent);
        }
    }
    host->scene3d.Update(0.0f);

    // Pass 2 applies auxiliary records after all target nodes exist.
    const char* p = text;
    while (*p != '\0') {
        u32 n = 0;
        while (*p != '\0' && *p != '\n') { if (n + 1 < sizeof(line)) line[n++] = *p; ++p; }
        if (n > 0u && line[n - 1u] == '\r') --n;
        line[n] = '\0';
        if (*p == '\n') ++p;
        if (std::strncmp(line, "MSH3D ", 6) == 0) {                  // カスタムメッシュの再読込
            int mid = 0; char mp[260] = {};
            if (std::sscanf(line, "MSH3D %d %259[^\n]", &mid, mp) >= 2) {
                if (game::ANode* en = FindNode3DNode(*host, mid + idOffset)) {
                    if (game::AMeshComponent3D* m = Mesh3D(en)) {
                        m->SetMeshAsset(LoadMeshFile(mp));           // 失敗時 null → 描画スキップ
                        m->SetMeshPath(FStringView(mp));             // パス記録 (種別 Mesh に)
                    }
                }
            }
            continue;
        }
        if (std::strncmp(line, "SPR3D ", 6) == 0) {                  // スプライト (z=0 画像) の再読込
            int sid = 0; char sp[260] = {};
            if (std::sscanf(line, "SPR3D %d %259[^\n]", &sid, sp) >= 2) {
                if (game::ANode* en = FindNode3DNode(*host, sid + idOffset)) {
                    AEditor3DRecordComponent* rec = Rec3D(en);
                    if (rec != nullptr) std::snprintf(rec->sprite_path, sizeof(rec->sprite_path), "%s", sp);
                    u32 iw = 0, ih = 0;
                    TUniquePtr<IRhiTexture> tex = LoadTexWithSize(*host, sp, iw, ih);   // device 準備済み前提
                    if (tex && Mesh3D(en) != nullptr) {
                        IRhiTexture* raw = tex.Get();
                        host->sprite_textures.Add(Move(tex));
                        Mesh3D(en)->SetRenderHandle(raw);            // 描画パスがスプライトとして扱う
                    }
                }
            }
            continue;
        }
        if (std::strncmp(line, "PLY3D ", 6) == 0) {                  // 手続きポリゴン (z=0) の再生成
            int pid = 0; unsigned pc = 0; int off = 0;
            if (std::sscanf(line, "PLY3D %d %u%n", &pid, &pc, &off) >= 2 && pc >= 3 && pc <= 4096) {
                TArray<FVec2> pts; pts.Reserve(pc);
                const char* q = line + off;
                for (unsigned k = 0; k < pc; ++k) {
                    float x = 0, y = 0; int adv = 0;
                    if (std::sscanf(q, " %f %f%n", &x, &y, &adv) < 2) break;
                    pts.Add(FVec2{ x, y }); q += adv;
                }
                if (pts.Num() == pc) {
                    if (game::ANode* en = FindNode3DNode(*host, pid + idOffset)) {
                        if (game::AMeshComponent3D* m = Mesh3D(en)) {
                            TSharedPtr<AAsset> mesh = MakeFlatPolygon3D(pts.GetData(), static_cast<u32>(pts.Num()));
                            if (mesh) m->SetMeshAsset(mesh);         // prim=Mesh + 所有 (z=0 フラット)
                        }
                        AEditor3DRecordComponent* rec = Rec3D(en);
                        if (rec != nullptr) rec->poly_pts = Move(pts);   // mesh 構築後に移譲 (pts は以降不要)
                    }
                }
            }
            continue;
        }
        if (std::strncmp(line, "CMP3D ", 6) == 0) {                  // アタッチ済みコンポーネントの復元 (N3D の後に来る)
            int cid = 0; char tn[256] = {};                          // 型名上限は他の補助行 (SPR3D/MSH3D) と同じ 256
            if (std::sscanf(line, "CMP3D %d %255[^\n]", &cid, tn) >= 2)
                AttachComponent3D(Rec3D(FindNode3DNode(*host, cid + idOffset)), tn);
            continue;
        }
        if (std::strncmp(line, "CAM3D ", 6) == 0) {
            int camera_id = 0;
            char stable_id[game::kScene3DSerializeMaxCameraIdBytes + 1u]{};
            int projection = 0;
            int priority = 0;
            int active = 0;
            float fov = 60.0f;
            float ortho_height = 10.0f;
            float near_plane = 0.05f;
            float far_plane = 1000.0f;
            if (std::sscanf(
                    line,
                    "CAM3D %d %64s %d %d %d %f %f %f %f",
                    &camera_id, stable_id, &projection, &priority,
                    &active, &fov, &ortho_height,
                    &near_plane, &far_plane) == 9) {
                AEditor3DRecordComponent* record = Rec3D(
                    FindNode3DNode(*host, camera_id + idOffset));
                if (record != nullptr) {
                    const int target_node_id = camera_id + idOffset;
                    const char* committed_stable_id = stable_id;
                    char generated_stable_id[
                        game::kScene3DSerializeMaxCameraIdBytes + 1u]{};
                    if (!clear && !SceneCameraIdIsUnique(
                            *host, target_node_id, stable_id)) {
                        if (!MakeUniqueClonedSceneCameraId(
                                *host, stable_id, target_node_id,
                                generated_stable_id,
                                sizeof(generated_stable_id))) {
                            return 0;
                        }
                        committed_stable_id = generated_stable_id;
                    }
                    record->has_scene_camera = true;
                    std::snprintf(
                        record->scene_camera_id,
                        sizeof(record->scene_camera_id),
                        "%s", committed_stable_id);
                    record->scene_camera_projection = projection;
                    record->scene_camera_priority = priority;
                    record->scene_camera_active = active != 0;
                    record->scene_camera_fov_deg = fov;
                    record->scene_camera_ortho_height = ortho_height;
                    record->scene_camera_near = near_plane;
                    record->scene_camera_far = far_plane;
                }
            }
            continue;
        }
        if (std::strncmp(line, "CPROP3D ", 8) == 0) {                // コンポーネント編集プロパティ値の復元 (CMP3D の直後)
            continue; // pass 3, after every CMP3D has been attached
        }
        if (std::strncmp(line, "FLG3D ", 6) == 0) {                  // 可視/有効フラグの復元 (N3D の後)
            int fid = 0, vis = 1, ena = 1;
            if (std::sscanf(line, "FLG3D %d %d %d", &fid, &vis, &ena) >= 3) {
                if (game::ANode* en = FindNode3DNode(*host, fid + idOffset)) { en->SetVisible(vis != 0); en->SetEnabled(ena != 0); }
            }
            continue;
        }
        if (std::strncmp(line, "MAT3D ", 6) == 0) {                  // マテリアル: «MAT3D id <.acsmatパス>» (新) / «MAT3D id m r» (旧)
            int mid = 0; char rest[256] = {};
            if (std::sscanf(line, "MAT3D %d %255[^\n]", &mid, rest) >= 2) {
                game::ANode* mn = FindNode3DNode(*host, mid + idOffset);
                game::AMeshComponent3D* mc = Mesh3D(mn);
                if (mc != nullptr) {
                    const char* numeric = rest;
                    float mm = 0.0f, mr = 0.5f;
                    const bool legacy_values =
                        ParseEditorTextFloat(numeric, mm) &&
                        ParseEditorTextFloat(numeric, mr) &&
                        EditorTextOnlyWhitespace(numeric);
                    if (legacy_values) {                                  // 旧形式 (後方互換): metallic roughness を pbr へ
                        mc->MaterialMut().pbr.metallic = mm;
                        mc->MaterialMut().pbr.roughness = mr;
                        mc->SetMaterialLoaded(true);
                    } else {                                               // 新形式: canonical material asset path
                        mc->SetMaterialPath(FStringView{ rest });
                        LoadNode3DMaterial(mn);
                    }
                }
            }
            continue;
        }
        if (std::strncmp(line, "PFAB3D ", 7) == 0) {                 // prefab/blueprint インスタンスリンクの復元 (N3D の後)
            int fid = 0; char fpath[256] = {};
            if (std::sscanf(line, "PFAB3D %d %255[^\n]", &fid, fpath) >= 2) {
                AEditor3DRecordComponent* rr = Rec3D(FindNode3DNode(*host, fid + idOffset));
                if (rr != nullptr) std::snprintf(rr->prefab_src, sizeof(rr->prefab_src), "%s", fpath);
            }
            continue;
        }
        if (std::strncmp(line, "PINS3D ", 7) == 0) {                 // stable Prefab instance IDの復元 (PFAB3D の後)
            int fid = 0;
            char instance_id[game::kScene3DSerializePrefabInstanceIdBytes + 1u]{};
            if (std::sscanf(line, "PINS3D %d %32s", &fid, instance_id) == 2) {
                AEditor3DRecordComponent* record = Rec3D(FindNode3DNode(*host, fid + idOffset));
                if (record != nullptr) std::snprintf(record->prefab_instance_id, sizeof(record->prefab_instance_id), "%s", instance_id);
            }
            continue;
        }
        if (std::strncmp(line, "EMPTY3D ", 8) == 0) {                // 空ノードフラグの復元 (N3D の後)
            int eid = 0;
            if (std::sscanf(line, "EMPTY3D %d", &eid) >= 1) {
                AEditor3DRecordComponent* rr = Rec3D(FindNode3DNode(*host, eid + idOffset));
                if (rr != nullptr) rr->is_empty = true;
            }
            continue;
        }
        if (std::strncmp(line, "SEL3D ", 6) == 0) { std::sscanf(line, "SEL3D %d", &restoredSel); continue; }   // 選択 id (後で復元)
        // N3D records were committed during pass 1.
    }

    // Pass 3 applies component values after every component declaration,
    // including documents that place CPROP3D before the matching CMP3D.
    p = text;
    while (*p != '\0') {
        u32 n = 0u;
        while (*p != '\0' && *p != '\n') {
            if (n + 1u < sizeof(line)) line[n++] = *p;
            ++p;
        }
        if (n > 0u && line[n - 1u] == '\r') --n;
        line[n] = '\0';
        if (*p == '\n') ++p;
        if (std::strncmp(line, "CPROP3D ", 8) != 0) continue;

        int component_id = 0;
        unsigned slot = 0u;
        unsigned property = 0u;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float w = 0.0f;
        if (std::sscanf(
                line, "CPROP3D %d %u %u %f %f %f %f",
                &component_id, &slot, &property,
                &x, &y, &z, &w) < 7) {
            continue; // unreachable after strict preflight
        }
        AEditor3DRecordComponent* record = Rec3D(
            FindNode3DNode(*host, component_id + idOffset));
        if (record == nullptr || slot >= record->component_count ||
            property >= AEditor3DRecordComponent::kMaxProps) {
            continue; // unreachable after strict preflight
        }
        f32* value = record->comp_props[slot][property];
        value[0] = x;
        value[1] = y;
        value[2] = z;
        value[3] = w;
    }
    if (!clear) {
        for (u32 index = 0u; index < loaded_ids.Num(); ++index) {
            const int target_id = loaded_ids[index];
            AEditor3DRecordComponent* record = Rec3D(FindNode3DNode(*host, target_id));
            if (record == nullptr || record->prefab_src[0] == '\0') continue;
            const char* source_identity = record->prefab_instance_id[0] != '\0' ? record->prefab_instance_id : record->prefab_src;
            char cloned_id[game::kScene3DSerializePrefabInstanceIdBytes + 1u]{};
            if (!MakeUniqueClonedPrefabInstanceId(*host, source_identity, target_id, cloned_id, sizeof(cloned_id))) return 0;
            std::snprintf(record->prefab_instance_id, sizeof(record->prefab_instance_id), "%s", cloned_id);
        }
    }
    host->scene3d.Update(0.0f);         // 保留中の reparent を一括解決 (階層を確定)
    if (maxId + 1 > host->next_id3d) host->next_id3d = maxId + 1;   // paste は既存 next_id3d を後退させない
    if (out_root != nullptr) *out_root = firstRoot;
    if (clear) {
        if (restoredSel >= 0 && FindNode3DNode(*host, restoredSel) != nullptr) {
            SetSel3D(*host, restoredSel);   // SEL3D で保存された選択を復元 (undo/redo で選択維持)
        } else {                            // 無ければ先頭ノード (新規読込のデフォルト)
            TArray<game::ANode*> all; Dfs3DCollect(&host->scene3d.Root(), all);
            AEditor3DRecordComponent* fr = (all.Num() > 0) ? Rec3D(all[0]) : nullptr;
            if (fr != nullptr) SetSel3D(*host, fr->id);
        }
    } else if (firstRoot >= 0) {
        SetSel3D(*host, firstRoot);         // paste: 貼り付けた root を選択
    }
    return 1;
}

} // namespace

/** 3D シーンをテキストから読み込む (既存ノードを置き換える)。成功 1。 */
ACS_EDITOR_API int acs_editor_scene3d_load_text(void* handle, const char* text) {
    return LoadScene3DTextImpl(static_cast<FEditorHost*>(handle), text, /*clear=*/true,
                               /*idOffset=*/0, /*reparentRootTo=*/-1, nullptr);
}

/**
 * Atomically replace both compatibility payloads of the singular scene
 * document. Both formats are validated before the old world is retired.
 */
ACS_EDITOR_API int acs_editor_scene_document_load_text(
    void* handle, const char* scene2d_text,
    const char* scene3d_text) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || scene2d_text == nullptr ||
        scene3d_text == nullptr ||
        !ValidateEditorScene2DText(scene2d_text) ||
        !ValidateEditorScene3DText(scene3d_text)) {
        return 0;
    }

    PushUndo(*host);
    FSceneResourceRetirementScope retirement(*host);
    const int loaded2d = LoadSceneTextValidated(*host, scene2d_text);
    if (loaded2d == 0) return 0;
    return LoadScene3DTextImpl(
        host, scene3d_text, /*clear=*/true,
        /*idOffset=*/0, /*reparentRootTo=*/-1, nullptr,
        /*prevalidated=*/true);
}

/** ACS3D subtree テキストを parent_id 配下へ貼り付ける (id を再採番・親 -1 の root を parent 配下へ。
 *  2D paste_subtree の 3D 版)。貼り付けたトップ root の新 id / 失敗 -1。 */
ACS_EDITOR_API int acs_editor_paste_subtree3d(void* handle, const char* text, int parent_id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || text == nullptr || !ValidateEditorScene3DText(text)) return -1;
    char* const rollback = DupSnapshot(*host);
    if (rollback == nullptr) return -1;
    int root = -1;
    const int ok = LoadScene3DTextImpl(host, text, /*clear=*/false, /*idOffset=*/host->next_id3d,
                                       /*reparentRootTo=*/parent_id, &root, /*prevalidated=*/true);
    if (ok == 0 || !CommitUndoSnapshot(*host, rollback)) {
        RestoreSnapshot(*host, rollback);
        delete[] rollback;
        return -1;
    }
    return root;
}

/** 検証済み3D subtreeをsourceとstable ID付きPrefab instanceとして1 transactionで生成する。 */
ACS_EDITOR_API int acs_editor_prefab_instance3d_instantiate(
    void* handle, const char* source, const char* instance_id,
    const char* text, int parent_id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || source == nullptr || source[0] == '\0' || instance_id == nullptr || text == nullptr || !IsCanonicalPrefabInstanceId(instance_id) || !PrefabInstanceIdIsUnique(*host, -1, instance_id) || !ValidateEditorScene3DText(text)) return -1;
    if (std::strlen(source) >= 256u) return -1;
    char* const rollback = DupSnapshot(*host);
    if (rollback == nullptr) return -1;
    int root = -1;
    const bool instantiated =
        LoadScene3DTextImpl(host, text, /*clear=*/false, /*idOffset=*/host->next_id3d,
                            /*reparentRootTo=*/parent_id, &root, /*prevalidated=*/true) != 0 &&
        SetPrefabLink3D_Internal(*host, root, source, instance_id);
    if (!instantiated || !CommitUndoSnapshot(*host, rollback)) {
        RestoreSnapshot(*host, rollback);
        delete[] rollback;
        return -1;
    }
    return root;
}

/**
 * 既存の3D Prefab/Blueprintインスタンスを検証済みsubtreeから再生成する。
 * sourceまたはpayloadが不正、再生成・設定・履歴公開に失敗した場合は旧sceneを完全に復元する。
 */
ACS_EDITOR_API int acs_editor_prefab_instance3d_refresh(
    void* handle, int id, const char* source, const char* text) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || source == nullptr || source[0] == '\0' ||
        text == nullptr || !ValidateEditorScene3DText(text)) {
        return -1;
    }
    game::ANode* const instance = FindNode3DNode(*host, id);
    AEditor3DRecordComponent* const record = Rec3D(instance);
    if (instance == nullptr || record == nullptr ||
        std::strlen(source) >= sizeof(record->prefab_src)) {
        return -1;
    }
    const int parent = ParentId3D(*host, instance);
    const FVec3 position = instance->Local().position;
    const FVec3 scale = instance->Local().scale;
    const FVec3 euler = record->euler;
    char previous_source[sizeof(record->prefab_src)]{};
    std::snprintf(previous_source, sizeof(previous_source), "%s", record->prefab_src);
    char preserved_instance_id[game::kScene3DSerializePrefabInstanceIdBytes + 1u]{};
    if (record->prefab_instance_id[0] != '\0') std::snprintf(preserved_instance_id, sizeof(preserved_instance_id), "%s", record->prefab_instance_id);

    char* const rollback = DupSnapshot(*host);
    if (rollback == nullptr) return -1;
    int replacement = -1;
    bool refreshed =
        acs_editor_delete_node3d(host, id) != 0 &&
        LoadScene3DTextImpl(host, text, /*clear=*/false, /*idOffset=*/host->next_id3d,
                            /*reparentRootTo=*/parent, &replacement, /*prevalidated=*/true) != 0 &&
        acs_editor_node3d_set_transform(host, replacement, position.x, position.y, position.z,
                                        euler.x, euler.y, euler.z, scale.x, scale.y, scale.z) != 0;
    if (refreshed && preserved_instance_id[0] == '\0') {
        refreshed = MakeUniqueClonedPrefabInstanceId(
            *host, previous_source[0] != '\0' ? previous_source : source,
            replacement, preserved_instance_id,
            static_cast<u32>(sizeof(preserved_instance_id)));
    }
    refreshed = refreshed && SetPrefabLink3D_Internal(*host, replacement, source, preserved_instance_id);
    if (!refreshed || !CommitUndoSnapshot(*host, rollback)) {
        RestoreSnapshot(*host, rollback);
        delete[] rollback;
        return -1;
    }
    return replacement;
}

ACS_EDITOR_API int acs_editor_water3d_hit_test(
    void* handle, float sx, float sy,
    float viewport_width, float viewport_height,
    int* node_id, float* world_x,
    float* world_y, float* world_z) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (node_id != nullptr) *node_id = -1;
    if (host == nullptr) return 0;
    FEditorWaterHit hit{};
    if (!HitTestEditorWaterSurface(
            *host, sx, sy, viewport_width,
            viewport_height, hit)) {
        return 0;
    }
    if (node_id != nullptr) *node_id = hit.node_id;
    if (world_x != nullptr) *world_x = hit.world_point.x;
    if (world_y != nullptr) *world_y = hit.world_point.y;
    if (world_z != nullptr) *world_z = hit.world_point.z;
    return 1;
}

ACS_EDITOR_API int acs_editor_water3d_disturb_world(
    void* handle, int node_id,
    float x, float y, float z,
    float radius, float strength) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || !host->water3d_ready) return 0;
    game::ANode* node = FindNode3DNode(*host, node_id);
    AEditor3DRecordComponent* record = Rec3D(node);
    if (!IsAuthoredWaterSurface(node) || record == nullptr) return 0;
    host->water3d.SetParams(WaterSurface3DParamsFor(record));
    return host->water3d.AddDisturbanceForSurface(
        static_cast<u64>(static_cast<u32>(node_id)),
        FVec3{x, y, z}, radius, strength) ? 1 : 0;
}

ACS_EDITOR_API int acs_editor_water3d_wake_world(
    void* handle, int node_id,
    float x, float y, float z,
    float vx, float vy, float vz,
    float radius, float strength) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || !host->water3d_ready) return 0;
    game::ANode* node = FindNode3DNode(*host, node_id);
    AEditor3DRecordComponent* record = Rec3D(node);
    if (!IsAuthoredWaterSurface(node) || record == nullptr) return 0;
    host->water3d.SetParams(WaterSurface3DParamsFor(record));
    return host->water3d.AddWakeForSurface(
        static_cast<u64>(static_cast<u32>(node_id)),
        FVec3{x, y, z}, FVec3{vx, vy, vz},
        radius, strength) ? 1 : 0;
}

/**
 * Route an existing viewport pointer gesture to world-space water.
 *
 * kind: 0=press/impact, 1=left-button drag/wake, 2=end, 3=hover wake. This
 * function never captures input and never changes camera, selection, or gizmo
 * state. Wake emission is spatially and temporally resampled so high-rate
 * WM_MOUSEMOVE streams cannot exhaust the persistent wake pool.
 */
ACS_EDITOR_API int acs_editor_water3d_pointer_event(
    void* handle, float sx, float sy, int kind) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return -1;
    if (kind == 2) {
        host->water_pointer_valid = false;
        host->water_pointer_node = -1;
        host->water_pointer_emit_time = -1.0f;
        return -1;
    }
    if (kind != 0 && kind != 1 && kind != 3) return -1;
    IRhiSwapchain* swapchain = host->renderer.Swapchain();
    if (swapchain == nullptr) return -1;
    FEditorWaterHit hit{};
    if (!HitTestEditorWaterSurface(
            *host, sx, sy,
            static_cast<f32>(swapchain->Width()),
            static_cast<f32>(swapchain->Height()),
            hit)) {
        host->water_pointer_valid = false;
        host->water_pointer_node = -1;
        host->water_pointer_emit_time = -1.0f;
        return -1;
    }
    game::ANode* node =
        FindNode3DNode(*host, hit.node_id);
    AEditor3DRecordComponent* record = Rec3D(node);
    if (node == nullptr || record == nullptr) return -1;
    const FWaterSurface3DParams params =
        WaterSurface3DParamsFor(record);
    if (host->water3d_ready) {
        host->water3d.SetParams(params);
        const FWaterSurface3DParams& safe_params =
            host->water3d.Params();
        const FVec3 scale = node->World().scale;
        const f32 horizontal_scale = std::max(
            std::abs(scale.x), std::abs(scale.z));
        const f32 radius = std::clamp(
            horizontal_scale * 0.035f, 0.04f, 0.8f);
        const f32 strength = std::clamp(
            std::max(std::abs(safe_params.wave_amplitude) * 0.65f,
                     0.025f),
            0.01f, 0.5f);
        const u64 surface_id =
            static_cast<u64>(
                static_cast<u32>(hit.node_id));
        const bool new_track =
            !host->water_pointer_valid ||
            host->water_pointer_node != hit.node_id;
        if (kind == 0 || (new_track && kind == 1)) {
            (void)host->water3d.AddDisturbanceForSurface(
                surface_id, hit.world_point,
                radius, strength);
            host->water_pointer_emit_time = host->time;
        } else if (!new_track) {
            const FVec3 displacement =
                hit.world_point - host->water_pointer_world;
            const f32 distance_squared =
                Dot(displacement, displacement);
            const f32 min_distance =
                std::max(radius * 0.32f, 0.015f);
            const f32 min_interval = std::max(
                safe_params.ripple_lifetime /
                    static_cast<f32>(
                        CWaterSurface3D::kWakeRippleSlots - 2u),
                0.035f);
            const f32 since_emit =
                host->water_pointer_emit_time < 0.0f
                    ? min_interval
                    : host->time - host->water_pointer_emit_time;
            if (distance_squared < min_distance * min_distance ||
                since_emit < min_interval) {
                return hit.node_id;
            }
            const f32 dt =
                std::max(since_emit, 1.0f / 1000.0f);
            const u32 accepted =
                host->water3d.AddWakeSegmentForSurface(
                    surface_id,
                    host->water_pointer_world, hit.world_point,
                    dt, min_distance,
                    radius, strength * 0.72f);
            if (accepted == 0u) return hit.node_id;
            host->water_pointer_emit_time = host->time;
        }
    }
    host->water_pointer_node = hit.node_id;
    host->water_pointer_world = hit.world_point;
    host->water_pointer_valid = true;
    return hit.node_id;
}

/** スクリーン点から 3D ノードをレイピックする。最も手前の id を返す (外れは -1)。
 *  ノードは «位置中心の球» (半径 = max scale の半分) で近似交差判定する。 */
ACS_EDITOR_API int acs_editor_pick3d(void* handle, float sx, float sy) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return -1;
    IRhiSwapchain* sc = host->renderer.Swapchain();
    if (sc == nullptr) return -1;
    const f32 W = static_cast<f32>(sc->Width()), H = static_cast<f32>(sc->Height());
    if (W <= 0 || H <= 0) return -1;

    // スクリーン → ワールドレイ。行列ベースの ScreenPointToRay は «透視も正射も» 正しく扱う
    // (inverse(view_proj) で near/far を逆射影 → 正射では平行レイ、透視では eye からの放射)。
    const f32 aspect = W / H;
    const FRay3 ray = acs::ScreenPointToRay(EditorCam3D(*host, aspect).ViewProjection(), sx, sy, W, H);

    // ノード交差は engine の CSceneNodeGraph::Raycast に委譲 (回転/スケール/階層を扱う OBB ピック)。
    int best = -1;
    const game::FNodeId hitId = host->scene3d.Raycast(ray);
    if (hitId.IsValid()) {
        AEditor3DRecordComponent* hr = Rec3D(host->scene3d.Get(hitId));
        if (hr != nullptr) best = hr->id;
    }
    if (best >= 0) SetSel3D(*host, best);
    return best;
}

/** 3D 変形ギズモの掴み開始。軸シャフト/平面ハンドルに近ければ掴む。
 *  返り値: 0=外れ, 1-3=軸(X/Y/Z), 4-6=平面(XY/YZ/XZ)。掴めたら以降の move を drag へ。 */
ACS_EDITOR_API int acs_editor_gizmo3d_begin(void* handle, float sx, float sy) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return 0;
    game::ANode* n = FindNode3DNode(*host, host->sel3d);
    IRhiSwapchain* sc = host->renderer.Swapchain();
    if (n == nullptr || sc == nullptr) return 0;
    const f32 W = static_cast<f32>(sc->Width()), H = static_cast<f32>(sc->Height());
    const FVec3 P = n->Local().position;
    const f32 gl = Gizmo3DScale(*host, P);
    f32 csx, csy;
    if (!WorldToScreen3D(*host, P, W, H, csx, csy)) return 0;

    int best = 0; f32 bestScore = 1e9f;
    // --- 平面ハンドル優先 (中心寄りの四角を掴みやすく)。スクリーン上の四角中心への近さ。
    const f32 po = gl * 0.34f;
    for (int hpl = 4; hpl <= 6; ++hpl) {
        FVec3 e1, e2, nrm; PlaneAxes(hpl, e1, e2, nrm);
        const FVec3 c{ P.x + (e1.x+e2.x)*po, P.y + (e1.y+e2.y)*po, P.z + (e1.z+e2.z)*po };
        f32 hx, hy;
        if (!WorldToScreen3D(*host, c, W, H, hx, hy)) continue;
        const f32 dist = std::sqrt((hx-sx)*(hx-sx) + (hy-sy)*(hy-sy));
        if (dist < 16.0f && dist < bestScore) { bestScore = dist; best = hpl; }
    }
    // --- 軸シャフト (線分距離)。平面に掴まれていなければ。
    if (best == 0) {
        f32 bestD = 14.0f;
        for (int a = 1; a <= 3; ++a) {
            const FVec3 d = AxisDir(a);
            f32 tx, ty;
            if (!WorldToScreen3D(*host, FVec3{ P.x + d.x*gl, P.y + d.y*gl, P.z + d.z*gl }, W, H, tx, ty)) continue;
            const f32 vx = tx - csx, vy = ty - csy, wx = sx - csx, wy = sy - csy;
            const f32 len2 = vx*vx + vy*vy;
            f32 t = (len2 > 1e-3f) ? (wx*vx + wy*vy) / len2 : 0.0f;
            t = (t < 0) ? 0 : (t > 1) ? 1 : t;
            const f32 dx = csx + vx*t - sx, dy = csy + vy*t - sy;
            const f32 dist = std::sqrt(dx*dx + dy*dy);
            if (dist < bestD) { bestD = dist; best = a; }
        }
    }
    host->giz3d_handle = best;
    if (best == 0) return 0;

    host->giz3d_start_mx    = sx; host->giz3d_start_my = sy;
    host->giz3d_start_pos   = P;
    host->giz3d_start_scale = n->Local().scale;
    { AEditor3DRecordComponent* r = Rec3D(n); host->giz3d_start_rot = (r != nullptr) ? r->euler : FVec3{ 0, 0, 0 }; }

    if (best <= 3) {
        // 軸: スクリーン上の軸方向 (単位) と world/px を求める (移動を «マウスの軸方向移動» に追従)。
        const FVec3 d = AxisDir(best);
        f32 ex, ey;
        if (WorldToScreen3D(*host, FVec3{ P.x + d.x*gl, P.y + d.y*gl, P.z + d.z*gl }, W, H, ex, ey)) {
            f32 vx = ex - csx, vy = ey - csy;
            const f32 vl = std::sqrt(vx*vx + vy*vy);
            if (vl > 1e-3f) { host->giz3d_sdx = vx/vl; host->giz3d_sdy = vy/vl; host->giz3d_wpp = gl / vl; }
            else            { host->giz3d_sdx = 1; host->giz3d_sdy = 0; host->giz3d_wpp = 0.01f; }
        }
    } else {
        // 平面: 面内 2 軸のスクリーンベクトル (P→P+e*gl) を記録 → drag で 2x2 連立を解く。
        FVec3 e1, e2, nrm; PlaneAxes(best, e1, e2, nrm);
        host->giz3d_pgl = gl;
        f32 a1x, a1y, a2x, a2y;
        if (WorldToScreen3D(*host, FVec3{ P.x+e1.x*gl, P.y+e1.y*gl, P.z+e1.z*gl }, W, H, a1x, a1y) &&
            WorldToScreen3D(*host, FVec3{ P.x+e2.x*gl, P.y+e2.y*gl, P.z+e2.z*gl }, W, H, a2x, a2y)) {
            host->giz3d_p1x = a1x - csx; host->giz3d_p1y = a1y - csy;
            host->giz3d_p2x = a2x - csx; host->giz3d_p2y = a2y - csy;
        } else { host->giz3d_p1x = host->giz3d_p1y = host->giz3d_p2x = host->giz3d_p2y = 0; }
    }
    return best;
}

/** 3D ギズモのドラッグ更新。現在のモード (move/rotate/scale) に従い変形する。 */
ACS_EDITOR_API void acs_editor_gizmo3d_drag(void* handle, float sx, float sy) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || host->giz3d_handle == 0) return;
    game::ANode* n = FindNode3DNode(*host, host->sel3d);
    IRhiSwapchain* sc = host->renderer.Swapchain();
    if (n == nullptr || sc == nullptr) return;
    const bool resets_temporal_history =
        TransformAffectsCurrentTemporalRenderCamera3D(
            *host, n);
    const f32 W = static_cast<f32>(sc->Width()), H = static_cast<f32>(sc->Height());
    const int hnd = host->giz3d_handle;

    if (hnd >= 4) {                                  // 平面移動 (Move 専用): スクリーン空間 2x2 連立
        FVec3 e1, e2, nrm; PlaneAxes(hnd, e1, e2, nrm);
        const f32 mdx = sx - host->giz3d_start_mx, mdy = sy - host->giz3d_start_my;
        // [p1 p2] [u;v] = md  → u,v は «gl-world 単位» の e1,e2 移動量。
        const f32 det = host->giz3d_p1x * host->giz3d_p2y - host->giz3d_p1y * host->giz3d_p2x;
        if (std::abs(det) < 1e-3f) return;            // 平面が視線に平行 (退化) → 無視
        const f32 u = (mdx * host->giz3d_p2y - mdy * host->giz3d_p2x) / det;
        const f32 v = (host->giz3d_p1x * mdy - host->giz3d_p1y * mdx) / det;
        const f32 du = u * host->giz3d_pgl, dv = v * host->giz3d_pgl;
        n->Local().position = FVec3{ host->giz3d_start_pos.x + e1.x*du + e2.x*dv,
                                     host->giz3d_start_pos.y + e1.y*du + e2.y*dv,
                                     host->giz3d_start_pos.z + e1.z*du + e2.z*dv };
        if (host->snap_enabled) { auto& p = n->Local().position;             // グリッドスナップ
            p.x = SnapTo(p.x, host->snap_move); p.y = SnapTo(p.y, host->snap_move); p.z = SnapTo(p.z, host->snap_move); }
        if (host->ortho3d) n->Local().position.z = host->giz3d_start_pos.z;   // 2D (正射): z を固定し XY 平面で編集
        if (resets_temporal_history)
            InvalidateTemporalRenderHistories(*host);
        return;
    }

    // 軸ハンドル: マウスのスクリーン移動量を «軸のスクリーン方向» に射影 → 直感的に追従。
    const f32 mdx = sx - host->giz3d_start_mx, mdy = sy - host->giz3d_start_my;
    const f32 px  = mdx * host->giz3d_sdx + mdy * host->giz3d_sdy;   // 軸方向の px 量 (符号付き)
    const FVec3 ad = AxisDir(hnd);

    if (host->gizmo_mode == 2) {                     // Scale
        FVec3 s0 = host->giz3d_start_scale;
        f32* comp = (hnd == 1) ? &s0.x : (hnd == 2) ? &s0.y : &s0.z;
        *comp += px * host->giz3d_wpp;
        if (host->snap_enabled) { *comp = SnapTo(*comp, host->snap_scale); if (*comp < host->snap_scale) *comp = host->snap_scale; }
        if (*comp < 0.05f) *comp = 0.05f;
        n->Local().scale = s0;
    } else if (host->gizmo_mode == 1) {              // Rotate (px → 度)
        FVec3 r0 = host->giz3d_start_rot;
        const f32 ang = px * 0.5f;
        if (hnd == 1) r0.x += ang; else if (hnd == 2) r0.y += ang; else r0.z += ang;
        if (host->snap_enabled) {                    // 角度スナップ
            if (hnd == 1) r0.x = SnapTo(r0.x, host->snap_rotate);
            else if (hnd == 2) r0.y = SnapTo(r0.y, host->snap_rotate);
            else r0.z = SnapTo(r0.z, host->snap_rotate);
        }
        n->Local().SetEulerDeg(r0);                  // quat に焼く
        AEditor3DRecordComponent* rc = Rec3D(n); if (rc != nullptr) rc->euler = r0;   // authored 値も更新
    } else {                                         // Move (px → world、軸方向)
        const f32 dw = px * host->giz3d_wpp;
        n->Local().position = FVec3{ host->giz3d_start_pos.x + ad.x*dw,
                                     host->giz3d_start_pos.y + ad.y*dw,
                                     host->giz3d_start_pos.z + ad.z*dw };
        if (host->snap_enabled) { auto& p = n->Local().position;             // グリッドスナップ
            p.x = SnapTo(p.x, host->snap_move); p.y = SnapTo(p.y, host->snap_move); p.z = SnapTo(p.z, host->snap_move); }
        if (host->ortho3d) n->Local().position.z = host->giz3d_start_pos.z;   // 2D (正射): z を固定
    }
    if (resets_temporal_history)
        InvalidateTemporalRenderHistories(*host);
}

/** 3D ギズモのドラッグ終了。 */
ACS_EDITOR_API void acs_editor_gizmo3d_end(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host != nullptr) host->giz3d_handle = 0;
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
    auto* host = static_cast<FEditorHost*>(handle);
    if (host != nullptr && mode >= 0 && mode <= 2) host->gizmo_mode = mode;
}

/** 現在のギズモモード。 */
ACS_EDITOR_API int acs_editor_gizmo_get_mode(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr) ? host->gizmo_mode : 0;
}

/** スクリーン点でギズモハンドルを掴む (掴めた handle 種別 1/2/3、掴めなければ 0)。 */
ACS_EDITOR_API int acs_editor_gizmo_begin(void* handle, float sx, float sy) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return 0;
    const int axis = GizmoHit(*host, sx, sy);
    if (axis == 0) return 0;
    AEditorNode* n = FindNode(*host, host->selected);
    if (n == nullptr) return 0;
    // undo は最初の実移動 (gizmo_update) まで遅延する。掴んだだけ (無移動) で離した
    // ときに空の undo を積まない & redo を消さないため。
    host->gizmo_pushed = false;
    host->gizmo_active = true;
    host->gizmo_axis   = axis;
    host->gizmo_move_ids.Reset();          // 前回 drag の残骸を一掃 (rotate/scale では空のまま)
    host->gizmo_move_bx.Reset();
    host->gizmo_move_by.Reset();

    const game::FTransform2D  w   = n->World2D();
    const game::FTransform2D  loc = n->Local2D();
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
        for (u32 i = 0; i < host->selection.Num(); ++i) {
            AEditorNode* sn = FindNode(*host, host->selection[i]);
            if (sn == nullptr || AnyAncestorInList(*host, sn, host->selection)) continue;
            const game::FTransform2D sw = sn->World2D();
            host->gizmo_move_ids.Add(sn->editor_id);
            host->gizmo_move_bx.Add(sw.position.x);
            host->gizmo_move_by.Add(sw.position.y);
        }
    }
    return axis;
}

/** ドラッグ中の操作を選択ノードへ適用する (モード別: 移動/回転/スケール)。 */
ACS_EDITOR_API void acs_editor_gizmo_update(void* handle, float sx, float sy) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || !host->gizmo_active) return;
    AEditorNode* n = FindNode(*host, host->selected);
    if (n == nullptr) { host->gizmo_active = false; return; }
    if (!host->gizmo_pushed) { PushUndo(*host); host->gizmo_pushed = true; }   // 最初の移動で 1 undo
    f32 cx, cy; GizmoCenter(*host, cx, cy);

    if (host->gizmo_mode == 1) {            // rotate: カーソル角の差を local rotation へ
        const f32 cur = std::atan2(sy - cy, sx - cx);
        f32 rot = host->gizmo_start_rot + (cur - host->gizmo_start_angle);
        if (host->snap_enabled) rot = SnapTo(rot, host->snap_rotate);
        n->SetRotation2D(rot);
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
        FVec2 scale = n->Scale2D();
        if (host->gizmo_axis == 1)      scale.x = nsx;
        else if (host->gizmo_axis == 2) scale.y = nsy;
        else                            scale = FVec2{ nsx, nsy };
        n->SetScale2D(scale);
    } else {                                 // move
        f32 tw_x = S2WX(*host, sx) - host->gizmo_off_wx;
        f32 tw_y = S2WY(*host, sy) - host->gizmo_off_wy;
        if (host->gizmo_axis == 1) tw_y = host->gizmo_begin_wy;   // X 軸のみ
        if (host->gizmo_axis == 2) tw_x = host->gizmo_begin_wx;   // Y 軸のみ
        if (host->snap_enabled) {
            if (host->gizmo_axis != 2) tw_x = SnapTo(tw_x, host->snap_move);   // X or free
            if (host->gizmo_axis != 1) tw_y = SnapTo(tw_y, host->snap_move);   // Y or free
        }
        if (host->gizmo_move_ids.Num() > 0) {
            // primary の移動量 delta を、退避した各選択ルートの開始位置に絶対適用する
            // (begin+delta の絶対指定なので順序非依存。子孫は祖先に従って動く)。
            const f32 dx = tw_x - host->gizmo_begin_wx;
            const f32 dy = tw_y - host->gizmo_begin_wy;
            for (u32 i = 0; i < host->gizmo_move_ids.Num(); ++i) {
                AEditorNode* mn = FindNode(*host, host->gizmo_move_ids[i]);
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
    auto* host = static_cast<FEditorHost*>(handle);
    if (host != nullptr) host->gizmo_active = false;
}

/** スナップ設定を更新する (enabled、移動グリッド、回転刻み[度]、スケール刻み。<=0 は据え置き)。 */
ACS_EDITOR_API void acs_editor_set_snap(void* handle, int enabled, float move_grid, float rotate_deg, float scale_step) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return;
    host->snap_enabled = (enabled != 0);
    if (move_grid  > 0.0f) host->snap_move   = move_grid;
    if (rotate_deg > 0.0f) host->snap_rotate = rotate_deg * (3.14159265f / 180.0f);
    if (scale_step > 0.0f) host->snap_scale  = scale_step;
}

/** スナップが有効か。 */
ACS_EDITOR_API int acs_editor_get_snap(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    return (host != nullptr && host->snap_enabled) ? 1 : 0;
}

/** 選択ノードがビューポート中央に来るようカメラを寄せる (ズームは維持)。 */
ACS_EDITOR_API void acs_editor_camera_focus(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return;
    if (host->view3d) {                                  // 3D: 選択ノードへ注視点を寄せる (ギズモと同じ Local 位置基準)
        game::ANode* n = FindNode3DNode(*host, host->sel3d);
        if (n != nullptr) {
            host->cam3d_target = n->Local().position;
            InvalidateTemporalRenderHistories(*host);
        }
        return;
    }
    if (host->selected < 0) return;
    AEditorNode* n = FindNode(*host, host->selected);
    if (n == nullptr) return;
    const game::FTransform2D w = n->World2D();
    host->cam_pan_x = static_cast<f32>(host->width)  * 0.5f - w.position.x * host->cam_zoom;
    host->cam_pan_y = static_cast<f32>(host->height) * 0.5f - w.position.y * host->cam_zoom;
}

/** 全ノードがビューポートに収まるよう pan/zoom を合わせる (シーン読込直後のフレーミング)。 */
ACS_EDITOR_API void acs_editor_camera_frame_all(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return;
    if (host->view3d) {
        const f32 maxf = std::numeric_limits<f32>::max();
        FVec3 minimum{ maxf, maxf, maxf };
        FVec3 maximum{ -maxf, -maxf, -maxf };
        bool found = false;
        auto expand = [&](FVec3 point) noexcept {
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) return;
            if (point.x < minimum.x) minimum.x = point.x;
            if (point.y < minimum.y) minimum.y = point.y;
            if (point.z < minimum.z) minimum.z = point.z;
            if (point.x > maximum.x) maximum.x = point.x;
            if (point.y > maximum.y) maximum.y = point.y;
            if (point.z > maximum.z) maximum.z = point.z;
            found = true;
        };

        TArray<game::ANode*> all3d;
        Dfs3DCollect(&host->scene3d.Root(), all3d);
        for (u32 node_index = 0; node_index < all3d.Num(); ++node_index) {
            game::ANode* node = all3d[node_index];
            const AEditor3DRecordComponent* record = Rec3D(node);
            const game::AMeshComponent3D* mesh_component = Mesh3D(node);
            if (node == nullptr || mesh_component == nullptr ||
                (record != nullptr && record->is_empty)) {
                continue;
            }

            FVec3 local_minimum{ -0.5f, -0.5f, -0.5f };
            FVec3 local_maximum{  0.5f,  0.5f,  0.5f };
            if (mesh_component->Primitive() == game::EMeshPrimitive3D::Plane) {
                local_minimum.y = 0.0f;
                local_maximum.y = 0.0f;
            } else if (mesh_component->Primitive() == game::EMeshPrimitive3D::Mesh) {
                const AMeshAsset* mesh = mesh_component->Mesh();
                if (mesh == nullptr || mesh->Vertices().Num() == 0) continue;
                bool has_local_point = false;
                for (u32 vertex_index = 0; vertex_index < mesh->Vertices().Num(); ++vertex_index) {
                    const FVec3 point = mesh->Vertices()[vertex_index].position;
                    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) continue;
                    if (!has_local_point) {
                        local_minimum = point;
                        local_maximum = point;
                        has_local_point = true;
                    } else {
                        if (point.x < local_minimum.x) local_minimum.x = point.x;
                        if (point.y < local_minimum.y) local_minimum.y = point.y;
                        if (point.z < local_minimum.z) local_minimum.z = point.z;
                        if (point.x > local_maximum.x) local_maximum.x = point.x;
                        if (point.y > local_maximum.y) local_maximum.y = point.y;
                        if (point.z > local_maximum.z) local_maximum.z = point.z;
                    }
                }
                if (!has_local_point) continue;
            }

            const FMat4 world = node->World().ToMat4();
            for (u32 corner = 0; corner < 8; ++corner) {
                const FVec3 local{
                    (corner & 1u) != 0u ? local_maximum.x : local_minimum.x,
                    (corner & 2u) != 0u ? local_maximum.y : local_minimum.y,
                    (corner & 4u) != 0u ? local_maximum.z : local_minimum.z
                };
                expand(TransformPoint(local, world));
            }
        }
        if (!found) return;

        host->cam3d_target = (minimum + maximum) * 0.5f;
        // Frame All must actually look at the bounds it just computed.
        // Preserving an upward-looking pitch leaves the new target behind the
        // camera because the free-look floor clamp intentionally decouples the
        // perspective forward vector from the orbit target.
        host->cam3d_pitch = host->ortho3d ? 0.0f : kCamera3DFramePitch;
        const FVec3 half_extent = (maximum - minimum) * 0.5f;
        const f32 radius = std::sqrt(
            half_extent.x * half_extent.x +
            half_extent.y * half_extent.y +
            half_extent.z * half_extent.z);
        const f32 aspect =
            (host->width > 0 && host->height > 0)
                ? static_cast<f32>(host->width) / static_cast<f32>(host->height)
                : (16.0f / 9.0f);
        const f32 safe_aspect = aspect > 0.01f ? aspect : 1.0f;
        f32 distance = kCamera3DMinDistance;
        if (host->ortho3d) {
            const f32 limiting_axis = safe_aspect < 1.0f ? safe_aspect : 1.0f;
            distance = (2.0f * radius * 1.15f) / (0.62f * limiting_axis);
        } else {
            constexpr f32 half_fov_y = 25.0f * 3.14159265f / 180.0f;
            const f32 half_fov_x = std::atan(std::tan(half_fov_y) * safe_aspect);
            const f32 limiting_half_fov = half_fov_x < half_fov_y ? half_fov_x : half_fov_y;
            distance = radius * 1.15f / std::sin(limiting_half_fov);
        }
        if (!std::isfinite(distance) || distance < kCamera3DMinDistance) {
            distance = kCamera3DMinDistance;
        }
        if (distance > kCamera3DMaxDistance) distance = kCamera3DMaxDistance;
        host->cam3d_dist = distance;
        InvalidateTemporalRenderHistories(*host);
        return;
    }
    if (host->nodes.Num() == 0) return;
    f32 minx = 3.4e38f, miny = 3.4e38f, maxx = -3.4e38f, maxy = -3.4e38f;
    for (u32 i = 0; i < host->nodes.Num(); ++i) {
        const AEditorNode* n = host->nodes[i];
        const game::FTransform2D w = n->World2D();
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
// 階層を実 ANode の構造変更 API (Destroy / Reparent + ResolveStructuralChanges) で
// 編集する。削除/付け替えの後は平坦レジストリをツリーから作り直して整合を保つ。
// =============================================================================

/** ノードを改名する (成功 1 / 不明 0)。 */
ACS_EDITOR_API int acs_editor_node_rename(void* handle, int id, const char* name) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || name == nullptr) return 0;
    AEditorNode* n = FindNode(*host, id);
    if (n == nullptr) return 0;
    PushUndo(*host);
    std::snprintf(n->name, sizeof(n->name), "%s", name);
    return 1;
}

/** ノードを subtree ごと複製し、元の兄弟として追加する (新しい根の id、不明は -1)。 */
ACS_EDITOR_API int acs_editor_node_duplicate(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return -1;
    AEditorNode* src = FindNode(*host, id);
    if (src == nullptr) return -1;
    PushUndo(*host);
    const int parent_id = ParentIdOf(*host, src);   // 元と同じ親 = 兄弟として複製
    TArray<int> oldIds, newIds;
    AEditorNode* clone = CloneSubtree(*host, src, parent_id, /*top=*/true, &oldIds, &newIds);
    RemapClonedObjectRefs(*host, oldIds, newIds);    // subtree 内の参照を新 id へ付け替え
    SelSet(*host, clone->editor_id);
    return clone->editor_id;
}

/** ノード (と subtree) を削除する (成功 1 / 不明 0)。 */
ACS_EDITOR_API int acs_editor_node_delete(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || host->root.Get() == nullptr) return 0;
    AEditorNode* n = FindNode(*host, id);
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
 * @details cycle (自分 or 子孫を親に指定) は ANode 側で弾かれ無視される。
 * @return 成功 1、不明ノード 0。
 */
ACS_EDITOR_API int acs_editor_node_reparent(void* handle, int id, int new_parent_id) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || host->root.Get() == nullptr) return 0;
    if (new_parent_id == id) return 0;         // 自分自身を親にはできない
    AEditorNode* n = FindNode(*host, id);
    if (n == nullptr) return 0;
    game::ANode* np = (new_parent_id >= 0) ? static_cast<game::ANode*>(FindNode(*host, new_parent_id))
                                             : host->root.Get();
    if (np == nullptr) np = host->root.Get();
    // no-op: 既に np が現在の親なら何もしない (無駄な undo / 再構築を積まない)。
    if (n->Parent() == np) return 0;
    // cycle 検出: np が n の子孫なら付け替え不可 (engine 側も無視するが、ここで弾いて
    //             spurious undo を防ぐ)。np から親をたどって n に達したら循環。
    for (game::ANode* a = np; a != nullptr; a = a->Parent())
        if (a == static_cast<game::ANode*>(n)) return 0;
    // ワールド位置を保持して付け替える (ノードが視覚的に飛ばないように)。
    const FVec2 wpos = n->World2D().position;
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
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || host->root.Get() == nullptr) return 0;
    if (id == target_id) return 0;
    if (mode == 2) return acs_editor_node_reparent(handle, id, target_id);   // 子にする = 既存 reparent

    AEditorNode* n = FindNode(*host, id);
    AEditorNode* t = FindNode(*host, target_id);
    if (n == nullptr || t == nullptr) return 0;

    // target の親 P (ルート直下なら root)。n を P の子として並べ替える。
    game::ANode* P = t->Parent() ? t->Parent() : host->root.Get();
    // cycle: P (= 挿入先) が n 自身 or その子孫なら不可。
    for (game::ANode* a = P; a != nullptr; a = a->Parent())
        if (a == static_cast<game::ANode*>(n)) return 0;

    const FVec2 wpos = n->World2D().position;
    PushUndo(*host);
    if (n->Parent() != P) {                       // まず同じ親へ寄せる (末尾に付く)
        n->Reparent(*P);
        host->root->ResolveStructuralChanges();
    }
    // P の子配列内で n と t のインデックスを取り、mode に応じた最終位置へ動かす。
    u32 c = P->ChildCount(), b = P->ChildCount();
    for (u32 i = 0; i < P->ChildCount(); ++i) {
        game::ANode* ch = P->Child(i);
        if (ch == static_cast<game::ANode*>(n)) c = i;
        if (ch == static_cast<game::ANode*>(t)) b = i;
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
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || host->root.Get() == nullptr || host->selection.Num() == 0) return 0;
    PushUndo(*host);                           // 一括で 1 undo
    int n_deleted = 0;
    // selection は Destroy では変化しない (RebuildRegistry まで不変) ので直接走査してよい。
    // 祖先と子孫が両方選択されていても Destroy は冪等マークなので二重解放にならない。
    for (u32 i = 0; i < host->selection.Num(); ++i) {
        AEditorNode* node = FindNode(*host, host->selection[i]);
        if (node != nullptr) { node->Destroy(); ++n_deleted; }
    }
    host->root->ResolveStructuralChanges();    // 全 subtree を一括 reap
    RebuildRegistry(*host);                    // SelPrune が消えた選択を一掃 (集合は空に)
    return n_deleted;
}

/** 選択集合のノードをまとめて複製する (1 undo step。複製した根の数を返す、空は 0)。 */
ACS_EDITOR_API int acs_editor_selection_duplicate(void* handle) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || host->selection.Num() == 0) return 0;
    PushUndo(*host);                           // 一括で 1 undo
    // 複製中に selection を書き換えるので、対象 id を先にコピーしておく。
    TArray<int> sources = host->selection.Clone();
    TArray<int> newIds;
    TArray<int> refOld, refNew;   // 複製した全ノードの old→new (ObjectRef 再マップ用)
    for (u32 i = 0; i < sources.Num(); ++i) {
        AEditorNode* src = FindNode(*host, sources[i]);
        if (src == nullptr) continue;
        // 祖先も選択集合にあるノードは、その祖先の複製に subtree として含まれる → 単独複製しない
        // (二重複製を防ぐ)。
        if (AnyAncestorInList(*host, src, sources)) continue;
        AEditorNode* clone = CloneSubtree(*host, src, ParentIdOf(*host, src), /*top=*/true, &refOld, &refNew);
        newIds.Add(clone->editor_id);
    }
    RemapClonedObjectRefs(*host, refOld, refNew);   // 複製集合内を指す参照を新 id へ付け替え
    // 新しい複製群を選択する (primary = 最後の複製)。
    host->selection.Reset();
    for (u32 i = 0; i < newIds.Num(); ++i) host->selection.Add(newIds[i]);
    host->selected = (host->selection.Num() > 0) ? host->selection[host->selection.Num() - 1] : -1;
    return static_cast<int>(newIds.Num());
}

/**
 * 選択ノードを world 位置で整列する (1 undo step。2 ノード以上で有効)。
 *
 * @param mode 0=左(min x) / 1=右(max x) / 2=上(min y) / 3=下(max y) / 4=水平中央(mid x) / 5=垂直中央(mid y)。
 * @return 整列したノード数 (2 未満は 0)。
 */
ACS_EDITOR_API int acs_editor_align_selection(void* handle, int mode) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return 0;

    // 選択ノードの world 位置の範囲を求める。
    f32 minx = 0, maxx = 0, miny = 0, maxy = 0;
    bool first = true; int cnt = 0;
    for (u32 i = 0; i < host->selection.Num(); ++i) {
        AEditorNode* n = FindNode(*host, host->selection[i]);
        if (n == nullptr) continue;
        const game::FTransform2D w = n->World2D();
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
    for (u32 i = 0; i < host->selection.Num(); ++i) {
        AEditorNode* n = FindNode(*host, host->selection[i]);
        if (n == nullptr) continue;
        const game::FTransform2D w = n->World2D();
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
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return 0;

    // (id, axis 位置) を集める。
    TArray<int> ids;
    TArray<f32> pos;
    for (u32 i = 0; i < host->selection.Num(); ++i) {
        AEditorNode* n = FindNode(*host, host->selection[i]);
        if (n == nullptr) continue;
        const game::FTransform2D w = n->World2D();
        ids.Add(host->selection[i]);
        pos.Add(axis == 0 ? w.position.x : w.position.y);
    }
    const u32 n = ids.Num();
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
        AEditorNode* node = FindNode(*host, ids[i]);
        if (node == nullptr) continue;
        const game::FTransform2D w = node->World2D();
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
// AComponent は生成せず、シリアライズして将来 play/load 時に実体化する土台とする。
// =============================================================================

/** ノードに Component 型をアタッチする (型名で。成功/既存 1、未登録や非 Component 0)。 */
ACS_EDITOR_API int acs_editor_node_add_component(void* handle, int id, const char* type_name) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return 0;
    AEditorNode* n = FindNode(*host, id);
    if (n == nullptr) return 0;
    PushUndo(*host);
    return AttachComponent(n, type_name) ? 1 : 0;
}

/** ノードのコンポーネント数。 */
ACS_EDITOR_API int acs_editor_node_component_count(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    const AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    return (n != nullptr) ? static_cast<int>(n->component_count) : 0;
}

/** ノードの index 番目のコンポーネント型名 (範囲外は "")。 */
ACS_EDITOR_API const char* acs_editor_node_component_name_at(void* handle, int id, int index) {
    auto* host = static_cast<FEditorHost*>(handle);
    const AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr || index < 0 || index >= static_cast<int>(n->component_count)) return "";
    const game::FTypeDesc* d = game::CTypeRegistry::Get().FindById(n->components[static_cast<u32>(index)]);
    return (d != nullptr && d->name != nullptr) ? d->name : "";
}

/** ノードの index 番目のコンポーネントを外す (成功 1)。 */
ACS_EDITOR_API int acs_editor_node_remove_component_at(void* handle, int id, int index) {
    auto* host = static_cast<FEditorHost*>(handle);
    AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr || index < 0 || index >= static_cast<int>(n->component_count)) return 0;
    PushUndo(*host);
    for (u32 i = static_cast<u32>(index); i + 1 < n->component_count; ++i) {
        n->components[i] = n->components[i + 1];
        for (u32 p = 0; p < AEditorNode::kMaxProps; ++p)
            for (u32 k = 0; k < 4; ++k) n->comp_props[i][p][k] = n->comp_props[i + 1][p][k];
    }
    --n->component_count;
    return 1;
}

// --- 3D ノードのコンポーネント (2D と同じ «エディタ・メタデータ» 方式。AEditor3DRecordComponent に保持) ---
static bool AttachComponent3D(AEditor3DRecordComponent* r, const char* type_name) noexcept {
    if (r == nullptr || type_name == nullptr) return false;
    game::AcsRegisterEngineTypes();
    const game::FTypeDesc* d = game::CTypeRegistry::Get().FindByName(type_name);
    if (d == nullptr || d->category != game::ETypeCategory::Component) return false;
    for (u32 i = 0; i < r->component_count; ++i) if (r->components[i] == d->id) return true;  // 重複
    if (r->component_count >= AEditor3DRecordComponent::kMaxComponents) return false;                          // 容量
    const u32 slot = r->component_count;
    r->components[slot] = d->id;
    for (u32 p = 0; p < AEditor3DRecordComponent::kMaxProps; ++p) for (u32 k = 0; k < 4; ++k) r->comp_props[slot][p][k] = 0.0f;
    const u32 nf = d->field_count < AEditor3DRecordComponent::kMaxProps ? d->field_count : AEditor3DRecordComponent::kMaxProps;
    for (u32 p = 0; p < nf; ++p) for (u32 k = 0; k < 4; ++k) r->comp_props[slot][p][k] = d->fields[p].defaults[k];
    r->component_count = slot + 1;
    return true;
}
ACS_EDITOR_API int acs_editor_node3d_add_component(void* handle, int id, const char* type_name) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr) return 0;
    AEditor3DRecordComponent* r = Rec3D(FindNode3DNode(*host, id));
    if (r == nullptr) return 0;
    PushUndo(*host);
    return AttachComponent3D(r, type_name) ? 1 : 0;
}
ACS_EDITOR_API int acs_editor_node3d_component_count(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    AEditor3DRecordComponent* r = (host != nullptr) ? Rec3D(FindNode3DNode(*host, id)) : nullptr;
    return (r != nullptr) ? static_cast<int>(r->component_count) : 0;
}
ACS_EDITOR_API const char* acs_editor_node3d_component_name_at(void* handle, int id, int index) {
    auto* host = static_cast<FEditorHost*>(handle);
    AEditor3DRecordComponent* r = (host != nullptr) ? Rec3D(FindNode3DNode(*host, id)) : nullptr;
    if (r == nullptr || index < 0 || index >= static_cast<int>(r->component_count)) return "";
    const game::FTypeDesc* d = game::CTypeRegistry::Get().FindById(r->components[static_cast<u32>(index)]);
    return (d != nullptr && d->name != nullptr) ? d->name : "";
}
ACS_EDITOR_API int acs_editor_node3d_remove_component_at(void* handle, int id, int index) {
    auto* host = static_cast<FEditorHost*>(handle);
    AEditor3DRecordComponent* r = (host != nullptr) ? Rec3D(FindNode3DNode(*host, id)) : nullptr;
    if (r == nullptr || index < 0 || index >= static_cast<int>(r->component_count)) return 0;
    PushUndo(*host);
    if (r->components[static_cast<u32>(index)] ==
        kWaterSurface3DTypeId) {
        host->water3d.ClearDisturbancesForSurface(
            static_cast<u64>(static_cast<u32>(id)));
        if (host->water_pointer_node == id) {
            host->water_pointer_valid = false;
            host->water_pointer_node = -1;
            host->water_pointer_emit_time = -1.0f;
        }
    }
    for (u32 i = static_cast<u32>(index); i + 1 < r->component_count; ++i) {
        r->components[i] = r->components[i + 1];
        for (u32 p = 0; p < AEditor3DRecordComponent::kMaxProps; ++p)
            for (u32 k = 0; k < 4; ++k) r->comp_props[i][p][k] = r->comp_props[i + 1][p][k];
    }
    --r->component_count;
    return 1;
}

// --- 3D コンポーネントの編集プロパティ値 (2D node_component_prop_get/set の 3D 版) ---
// スキーマ (型名→どの編集フィールドがあるか) は 2D と共有の acs_editor_component_prop_*
// を流用し、«インスタンス値» のみ AEditor3DRecordComponent::comp_props が slot×prop×4 で保持する。
ACS_EDITOR_API int acs_editor_node3d_component_prop_get(void* handle, int id, int slot, int prop,
                                                        float* x, float* y, float* z, float* w) {
    auto* host = static_cast<FEditorHost*>(handle);
    AEditor3DRecordComponent* r = (host != nullptr) ? Rec3D(FindNode3DNode(*host, id)) : nullptr;
    if (r == nullptr || slot < 0 || slot >= static_cast<int>(r->component_count)
        || prop < 0 || prop >= static_cast<int>(AEditor3DRecordComponent::kMaxProps)) return 0;
    const f32* v = r->comp_props[static_cast<u32>(slot)][static_cast<u32>(prop)];
    if (x != nullptr) *x = v[0];
    if (y != nullptr) *y = v[1];
    if (z != nullptr) *z = v[2];
    if (w != nullptr) *w = v[3];
    return 1;
}
ACS_EDITOR_API int acs_editor_node3d_component_prop_set(void* handle, int id, int slot, int prop,
                                                        float x, float y, float z, float w) {
    auto* host = static_cast<FEditorHost*>(handle);
    AEditor3DRecordComponent* r = (host != nullptr) ? Rec3D(FindNode3DNode(*host, id)) : nullptr;
    if (r == nullptr || slot < 0 || slot >= static_cast<int>(r->component_count)
        || prop < 0 || prop >= static_cast<int>(AEditor3DRecordComponent::kMaxProps)) return 0;
    PushUndo(*host);
    f32* v = r->comp_props[static_cast<u32>(slot)][static_cast<u32>(prop)];
    v[0] = x; v[1] = y; v[2] = z; v[3] = w;
    return 1;
}

// 3D ノードの slot 番コンポーネントの反射メソッドを «その場で» 呼ぶ (2D node_invoke_method の 3D 版)。
// 型を一時実体化し editor 値 (comp_props) を適用して起動 → 破棄 (= CallInEditor)。
ACS_EDITOR_API int acs_editor_node3d_invoke_method(void* handle, int id, int slot, const char* method_name) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || method_name == nullptr) return 0;
    AEditor3DRecordComponent* r = Rec3D(FindNode3DNode(*host, id));
    if (r == nullptr || slot < 0 || slot >= static_cast<int>(r->component_count)) return 0;
    const game::FTypeId tid = r->components[static_cast<u32>(slot)];
    const game::FTypeDesc* d = game::CTypeRegistry::Get().FindById(tid);
    if (d == nullptr) return 0;
    void* obj = game::CTypeRegistry::Get().CreateById(tid);   // 一時実体化 (factory)
    if (obj == nullptr) return 0;
    game::ApplyDefaults(obj, *d);                             // C++ 既定値で初期化
    const u32 nf = CompPropCount(d);                          // 編集値 (authored) を実体へ適用
    for (u32 p = 0; p < nf; ++p) {
        const f32* v = r->comp_props[static_cast<u32>(slot)][p];
        game::ApplyFieldValue(obj, d->fields[p], v);
    }
    const bool ok = game::InvokeMethodByName(tid, obj, method_name);   // メソッド起動
    game::CTypeRegistry::Get().Destroy(tid, obj);             // 一時実体を破棄
    return ok ? 1 : 0;
}

// --- 3D ノードの可視/有効フラグ (ANode が m_Visible/m_Enabled を持つ。2D と同じ) ---
ACS_EDITOR_API int acs_editor_node3d_get_visible(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    game::ANode* n = (host != nullptr) ? FindNode3DNode(*host, id) : nullptr;
    return (n != nullptr && n->IsVisible()) ? 1 : 0;
}
ACS_EDITOR_API void acs_editor_node3d_set_visible(void* handle, int id, int visible) {
    auto* host = static_cast<FEditorHost*>(handle);
    game::ANode* n = (host != nullptr) ? FindNode3DNode(*host, id) : nullptr;
    if (n == nullptr) return;
    PushUndo(*host);
    n->SetVisible(visible != 0);
}
ACS_EDITOR_API int acs_editor_node3d_get_enabled(void* handle, int id) {
    auto* host = static_cast<FEditorHost*>(handle);
    game::ANode* n = (host != nullptr) ? FindNode3DNode(*host, id) : nullptr;
    return (n != nullptr && n->IsEnabled()) ? 1 : 0;
}
ACS_EDITOR_API void acs_editor_node3d_set_enabled(void* handle, int id, int enabled) {
    auto* host = static_cast<FEditorHost*>(handle);
    game::ANode* n = (host != nullptr) ? FindNode3DNode(*host, id) : nullptr;
    if (n == nullptr) return;
    PushUndo(*host);
    n->SetEnabled(enabled != 0);
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
    const game::FTypeDesc* d = game::CTypeRegistry::Get().FindByName(type_name);
    return static_cast<int>(CompPropCount(d));
}

/** 型名と index で、編集プロパティ名を返す (範囲外は "")。 */
ACS_EDITOR_API const char* acs_editor_component_prop_name_at(const char* type_name, int index) {
    if (type_name == nullptr || index < 0) return "";
    game::AcsRegisterEngineTypes();
    const game::FTypeDesc* d = game::CTypeRegistry::Get().FindByName(type_name);
    if (d == nullptr || index >= static_cast<int>(CompPropCount(d))) return "";
    const char* nm = d->fields[static_cast<u32>(index)].name;
    return (nm != nullptr) ? nm : "";
}

/** 型名と index で、編集プロパティの種別 (EFieldKind の整数値) を返す (範囲外は -1)。 */
ACS_EDITOR_API int acs_editor_component_prop_kind_at(const char* type_name, int index) {
    if (type_name == nullptr || index < 0) return -1;
    game::AcsRegisterEngineTypes();
    const game::FTypeDesc* d = game::CTypeRegistry::Get().FindByName(type_name);
    if (d == nullptr || index >= static_cast<int>(CompPropCount(d))) return -1;
    return static_cast<int>(d->fields[static_cast<u32>(index)].kind);
}

/** 型名と index で、編集プロパティのフラグ (EFieldFlags の OR) を返す (範囲外は 0)。
 *  bit0=READONLY (表示のみ・編集不可)、bit1=HIDDEN (非表示)、bit2=TRANSIENT。 */
ACS_EDITOR_API int acs_editor_component_prop_flags_at(const char* type_name, int index) {
    if (type_name == nullptr || index < 0) return 0;
    game::AcsRegisterEngineTypes();
    const game::FTypeDesc* d = game::CTypeRegistry::Get().FindByName(type_name);
    if (d == nullptr || index >= static_cast<int>(CompPropCount(d))) return 0;
    return static_cast<int>(d->fields[static_cast<u32>(index)].flags);
}

/**
 * 型名と index で、反射スキーマが公開する既定値を返す。
 *
 * Reset-to-default は現在値から推測してはならないため、managed Details は
 * このスキーマ値だけを使用する。範囲外、未登録型、非有限な壊れたスキーマは
 * 0 を返し、出力を変更しない。
 */
ACS_EDITOR_API int acs_editor_component_prop_default_at(
    const char* type_name,
    int index,
    float* x,
    float* y,
    float* z,
    float* w)
{
    if (type_name == nullptr || index < 0) return 0;
    game::AcsRegisterEngineTypes();
    const game::FTypeDesc* const descriptor =
        game::CTypeRegistry::Get().FindByName(type_name);
    if (descriptor == nullptr ||
        index >= static_cast<int>(CompPropCount(descriptor))) {
        return 0;
    }

    const f32* const defaults =
        descriptor->fields[static_cast<u32>(index)].defaults;
    for (u32 component = 0u; component < 4u; ++component) {
        if (!std::isfinite(defaults[component])) return 0;
    }
    if (x != nullptr) *x = defaults[0];
    if (y != nullptr) *y = defaults[1];
    if (z != nullptr) *z = defaults[2];
    if (w != nullptr) *w = defaults[3];
    return 1;
}

/** 型名と index で、ACS_PROPERTY のカテゴリ名を返す (未指定は "")。 */
ACS_EDITOR_API const char* acs_editor_component_prop_category_at(const char* type_name, int index) {
    if (type_name == nullptr || index < 0) return "";
    game::AcsRegisterEngineTypes();
    const game::FTypeDesc* d = game::CTypeRegistry::Get().FindByName(type_name);
    if (d == nullptr || index >= static_cast<int>(CompPropCount(d))) return "";
    const char* c = d->fields[static_cast<u32>(index)].category;
    return (c != nullptr) ? c : "";
}

// ----- 反射メソッド (ACS_FUNCTION / BlueprintCallable / CallInEditor) -----

/** 型の «反射メソッド» 数を返す (引数なし void メソッド)。 */
ACS_EDITOR_API int acs_editor_component_method_count(const char* type_name) {
    if (type_name == nullptr) return 0;
    game::AcsRegisterEngineTypes();
    return static_cast<int>(game::CMethodRegistry::Get().CountOfOwner(game::AcsTypeHash(type_name)));
}

/** 型の index 番目の反射メソッド名 (範囲外は "")。 */
ACS_EDITOR_API const char* acs_editor_component_method_name_at(const char* type_name, int index) {
    if (type_name == nullptr || index < 0) return "";
    game::AcsRegisterEngineTypes();
    const game::FReflectMethod* m = game::CMethodRegistry::Get().AtOfOwner(game::AcsTypeHash(type_name), static_cast<u32>(index));
    return (m != nullptr && m->name != nullptr) ? m->name : "";
}

/** 型の index 番目の反射メソッドのフラグ (bit0=BlueprintCallable, bit1=CallInEditor)。 */
ACS_EDITOR_API int acs_editor_component_method_flags_at(const char* type_name, int index) {
    if (type_name == nullptr || index < 0) return 0;
    game::AcsRegisterEngineTypes();
    const game::FReflectMethod* m = game::CMethodRegistry::Get().AtOfOwner(game::AcsTypeHash(type_name), static_cast<u32>(index));
    return (m != nullptr) ? static_cast<int>(m->flags) : 0;
}

/** ノードの slot 番コンポーネントの反射メソッドを «その場で» 呼ぶ。成功 1。
 *  コンポーネント型を一時実体化し、ノードの編集値(comp_props)を適用してからメソッドを起動する
 *  (= CallInEditor: 副作用/ログをエディタで観測できる)。実体は呼び出し後に破棄する。 */
ACS_EDITOR_API int acs_editor_node_invoke_method(void* handle, int id, int slot, const char* method_name) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || method_name == nullptr) return 0;
    AEditorNode* n = FindNode(*host, id);
    if (n == nullptr || slot < 0 || slot >= static_cast<int>(n->component_count)) return 0;
    const game::FTypeId tid = n->components[static_cast<u32>(slot)];
    const game::FTypeDesc* d = game::CTypeRegistry::Get().FindById(tid);
    if (d == nullptr) return 0;
    void* obj = game::CTypeRegistry::Get().CreateById(tid);   // 一時実体化 (factory)
    if (obj == nullptr) return 0;
    game::ApplyDefaults(obj, *d);                             // C++ 既定値で初期化
    const u32 nf = CompPropCount2D(d);                        // 編集値 (authored) を実体へ適用
    for (u32 p = 0; p < nf; ++p) {
        const f32* v = n->comp_props[static_cast<u32>(slot)][p];
        game::ApplyFieldValue(obj, d->fields[p], v);
    }
    const bool ok = game::InvokeMethodByName(tid, obj, method_name);   // メソッド起動
    game::CTypeRegistry::Get().Destroy(tid, obj);             // 一時実体を破棄
    return ok ? 1 : 0;
}

/** node_invoke_method の «文字列引数» 版。引数ありメソッドは arg をパースして渡し、
 *  引数なしメソッドは arg を無視する (Blueprint の関数ノードから実引数を渡す経路)。 */
ACS_EDITOR_API int acs_editor_node_invoke_method_arg(void* handle, int id, int slot,
                                                     const char* method_name, const char* arg) {
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || method_name == nullptr) return 0;
    AEditorNode* n = FindNode(*host, id);
    if (n == nullptr || slot < 0 || slot >= static_cast<int>(n->component_count)) return 0;
    const game::FTypeId tid = n->components[static_cast<u32>(slot)];
    const game::FTypeDesc* d = game::CTypeRegistry::Get().FindById(tid);
    if (d == nullptr) return 0;
    void* obj = game::CTypeRegistry::Get().CreateById(tid);
    if (obj == nullptr) return 0;
    game::ApplyDefaults(obj, *d);
    const u32 nf = CompPropCount2D(d);
    for (u32 p = 0; p < nf; ++p) {
        const f32* v = n->comp_props[static_cast<u32>(slot)][p];
        game::ApplyFieldValue(obj, d->fields[p], v);
    }
    const bool ok = game::InvokeMethodByNameArg(tid, obj, method_name, arg != nullptr ? arg : "");
    game::CTypeRegistry::Get().Destroy(tid, obj);
    return ok ? 1 : 0;
}

/** node_invoke_method の «戻り値» 版。arg を渡して起動し、戻り値を out へ文字列化する
 *  (void メソッドは out が空)。Blueprint の関数ノードの data 出力ピンへ結果を流す経路。 */
ACS_EDITOR_API int acs_editor_node_invoke_method_ret(void* handle, int id, int slot,
                                                     const char* method_name, const char* arg,
                                                     char* out, int outcap) {
    if (out != nullptr && outcap > 0) out[0] = '\0';
    auto* host = static_cast<FEditorHost*>(handle);
    if (host == nullptr || method_name == nullptr) return 0;
    AEditorNode* n = FindNode(*host, id);
    if (n == nullptr || slot < 0 || slot >= static_cast<int>(n->component_count)) return 0;
    const game::FTypeId tid = n->components[static_cast<u32>(slot)];
    const game::FTypeDesc* d = game::CTypeRegistry::Get().FindById(tid);
    if (d == nullptr) return 0;
    void* obj = game::CTypeRegistry::Get().CreateById(tid);
    if (obj == nullptr) return 0;
    game::ApplyDefaults(obj, *d);
    const u32 nf = CompPropCount2D(d);
    for (u32 p = 0; p < nf; ++p) {
        const f32* v = n->comp_props[static_cast<u32>(slot)][p];
        game::ApplyFieldValue(obj, d->fields[p], v);
    }
    const bool ok = game::InvokeMethodByNameRet(tid, obj, method_name, arg != nullptr ? arg : "", out, outcap);
    game::CTypeRegistry::Get().Destroy(tid, obj);
    return ok ? 1 : 0;
}

// ----- グローバル反射メソッド列挙 (Blueprint ノードパレット用) -----

/** 登録済み «全» 反射メソッド数 (所有型を問わない)。 */
ACS_EDITOR_API int acs_editor_method_count() {
    game::AcsRegisterEngineTypes();
    return static_cast<int>(game::CMethodRegistry::Get().Count());
}

/** i 番目の反射メソッド名 (範囲外は "")。 */
ACS_EDITOR_API const char* acs_editor_method_name_at(int i) {
    game::AcsRegisterEngineTypes();
    auto& r = game::CMethodRegistry::Get();
    if (i < 0 || static_cast<u32>(i) >= r.Count()) return "";
    const char* nm = r.At(static_cast<u32>(i)).name;
    return (nm != nullptr) ? nm : "";
}

/** i 番目の反射メソッドの所有型名 (ハッシュ→型名を解決。未登録型は "")。 */
ACS_EDITOR_API const char* acs_editor_method_owner_at(int i) {
    game::AcsRegisterEngineTypes();
    auto& r = game::CMethodRegistry::Get();
    if (i < 0 || static_cast<u32>(i) >= r.Count()) return "";
    const game::FTypeDesc* d = game::CTypeRegistry::Get().FindById(r.At(static_cast<u32>(i)).owner);
    return (d != nullptr && d->name != nullptr) ? d->name : "";
}

/** i 番目の反射メソッドのフラグ (bit0=BlueprintCallable, bit1=CallInEditor)。 */
ACS_EDITOR_API int acs_editor_method_flags_at(int i) {
    game::AcsRegisterEngineTypes();
    auto& r = game::CMethodRegistry::Get();
    if (i < 0 || static_cast<u32>(i) >= r.Count()) return 0;
    return static_cast<int>(r.At(static_cast<u32>(i)).flags);
}

/** i 番目の反射メソッドの引数種別 (0=None,1=F32,2=I32,3=Str)。 */
ACS_EDITOR_API int acs_editor_method_argkind_at(int i) {
    game::AcsRegisterEngineTypes();
    auto& r = game::CMethodRegistry::Get();
    if (i < 0 || static_cast<u32>(i) >= r.Count()) return 0;
    return static_cast<int>(r.At(static_cast<u32>(i)).argKind);
}

/** i 番目の反射メソッドの戻り値種別 (0=None/void,1=F32,2=I32,3=Str)。 */
ACS_EDITOR_API int acs_editor_method_retkind_at(int i) {
    game::AcsRegisterEngineTypes();
    auto& r = game::CMethodRegistry::Get();
    if (i < 0 || static_cast<u32>(i) >= r.Count()) return 0;
    return static_cast<int>(r.At(static_cast<u32>(i)).retKind);
}

// ----- エンジンログ取り込み (エディタの C# コンソールへ) -----

/** キューに溜まったエンジンログを 1 件取り出す。成功 1 (severity と message を書く)、空 0。
 *  C# 側はタイマーで 0 が返るまで繰り返し呼ぶ。severity は ELogSeverity (2=Info,3=Warn,4=Error...)。 */
ACS_EDITOR_API int acs_editor_log_poll(int* out_severity, char* buf, int buflen) {
    if (buf == nullptr || buflen <= 0) return 0;
    int s = 0;
    if (!g_log_ring.pop(s, buf, buflen)) return 0;
    if (out_severity != nullptr) *out_severity = s;
    return 1;
}

/** ノードの slot 番コンポーネントの prop 番プロパティ値 (4 成分) を取得する (成功 1)。 */
ACS_EDITOR_API int acs_editor_node_component_prop_get(void* handle, int id, int slot, int prop,
                                                      float* x, float* y, float* z, float* w) {
    auto* host = static_cast<FEditorHost*>(handle);
    const AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr || slot < 0 || slot >= static_cast<int>(n->component_count)
        || prop < 0 || prop >= static_cast<int>(AEditorNode::kMaxProps)) return 0;
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
    auto* host = static_cast<FEditorHost*>(handle);
    AEditorNode* n = (host != nullptr) ? FindNode(*host, id) : nullptr;
    if (n == nullptr || slot < 0 || slot >= static_cast<int>(n->component_count)
        || prop < 0 || prop >= static_cast<int>(AEditorNode::kMaxProps)) return 0;
    PushUndo(*host);
    SetCompProp(n, static_cast<u32>(slot), static_cast<u32>(prop), x, y, z, w);
    return 1;
}

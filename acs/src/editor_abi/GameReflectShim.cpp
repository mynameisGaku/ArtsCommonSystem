// SPDX-License-Identifier: Apache-2.0
// GameReflectShim — ユーザーのゲームプロジェクトを「リフレクション DLL」としてビルドする際に
// 一緒にコンパイルされ、この DLL の FTypeRegistry に登録された型 (= ユーザー定義の
// コンポーネント/オブジェクト + エンジン型) を C ABI で外部 (editor_abi) へ公開する。
//
// editor_abi は LoadLibrary でこの DLL をロードし、これらの関数で型スキーマを列挙して
// ディープコピーし、自分の FTypeRegistry に登録する (DLL ごとに別インスタンスの singleton
// を跨ぐための唯一の手段)。値ベースのスキーマ (ACS_RPROP*) なので名前/種別/既定値だけで足りる。
#include "gameframework/Reflect.h"
#include "gameframework/ReflectCatalog.h"
#include "gameframework/Node2D.h"           // 実シーングラフ
#include "gameframework/Component2D.h"      // FComponent2D::ReflectName
#include "gameframework/ComponentFactory.h" // CreateComponentByName (DLL ローカル factory)
#include "gameframework/ReflectApply.h"     // ApplyValueByName (authored 値を実体へ)
#include "gameframework/RenderContext.h"    // RenderContext (OnDraw に渡す)
#include "gameframework/SceneServices.h"    // FSceneServices (DLL 所有 → cross-DLL 安全)
#include "render/SpriteBatch.h"             // FSpriteBatch (editor 所有を共有)
#include "platform/Input.h"                 // acs::Input (この DLL の g_input を FInputMap が poll)
#include "platform/Event.h"                 // acs::Event (キーイベント合成)
#include "memory/UniquePtr.h"
#include "container/Array.h"

using namespace acs;
using namespace acs::game;

#define GR_API extern "C" __declspec(dllexport)

/** DLL のエンジン型 + (Game.cpp の) ユーザー型を確実に登録させる。LoadLibrary 後に最初に呼ぶ。 */
GR_API void acs_game_reflect_init() noexcept {
    AcsRegisterEngineTypes();   // エンジンカタログを強制リンク (ユーザー型は静的初期化で登録済み)
}

/** この DLL の FTypeRegistry に登録された型の総数。 */
GR_API unsigned acs_game_reflect_count() noexcept {
    return FTypeRegistry::Get().Count();
}

/** i 番目の型名 (範囲外は "")。 */
GR_API const char* acs_game_reflect_name(unsigned i) noexcept {
    const FTypeDesc* d = FTypeRegistry::Get().At(i);
    return (d != nullptr && d->name != nullptr) ? d->name : "";
}

/** i 番目の型カテゴリ (ETypeCategory の整数、範囲外は -1)。 */
GR_API int acs_game_reflect_category(unsigned i) noexcept {
    const FTypeDesc* d = FTypeRegistry::Get().At(i);
    return (d != nullptr) ? static_cast<int>(d->category) : -1;
}

/** i 番目の型のフィールド数。 */
GR_API unsigned acs_game_reflect_field_count(unsigned i) noexcept {
    const FTypeDesc* d = FTypeRegistry::Get().At(i);
    return (d != nullptr) ? d->field_count : 0u;
}

/** i 番目の型の j 番目フィールド名 (範囲外は "")。 */
GR_API const char* acs_game_reflect_field_name(unsigned i, unsigned j) noexcept {
    const FTypeDesc* d = FTypeRegistry::Get().At(i);
    return (d != nullptr && d->fields != nullptr && j < d->field_count) ? d->fields[j].name : "";
}

/** i 番目の型の j 番目フィールド種別 (EFieldKind の整数、範囲外は 0)。 */
GR_API int acs_game_reflect_field_kind(unsigned i, unsigned j) noexcept {
    const FTypeDesc* d = FTypeRegistry::Get().At(i);
    if (d == nullptr || d->fields == nullptr || j >= d->field_count) return 0;
    return static_cast<int>(d->fields[j].kind);
}

/** i 番目の型の j 番目フィールド既定値 (4 成分を out[4] へ)。 */
GR_API void acs_game_reflect_field_defaults(unsigned i, unsigned j, float* out) noexcept {
    if (out == nullptr) return;
    const FTypeDesc* d = FTypeRegistry::Get().At(i);
    if (d != nullptr && d->fields != nullptr && j < d->field_count) {
        for (int k = 0; k < 4; ++k) out[k] = d->fields[j].defaults[k];
    } else {
        for (int k = 0; k < 4; ++k) out[k] = 0.0f;
    }
}

// =============================================================================
// インプロセス Play: «DLL 側» が実シーン (real FNode2D + real components) を所有・tick する。
// factory/allocator は全て DLL ローカルなので、ユーザー定義コンポーネントも安全に生成・破棄・実行
// できる (editor 側で構築/破棄するときの cross-DLL 所有問題を回避)。editor は node を index で
// 構築し、毎フレーム tick → transform を読み戻して描画する。
// =============================================================================

namespace {
/** DLL が所有する Play シーン。root が全ノードを所有、nodes[] は index→node の非所有マップ。 */
struct GamePlayScene {
    TUniquePtr<FNode2D>        root;
    TArray<FNode2D*>          nodes;     // 非所有 (tree が所有)
    // services は root «より後» に宣言 = 破棄は逆順 → tree (services を借用) が先に壊れ、借用先 services が後。
    TUniquePtr<FSceneServices> services;
};
} // namespace

/** Play シーンを作る (空の root + DLL 所有 services)。戻り値は不透明ハンドル。 */
GR_API void* acs_game_scene_create() noexcept {
    // リフレクション共通の生成ヘルパで確保元を記録し、既定アロケータ切替を跨げるようにする。
    auto* s = static_cast<GamePlayScene*>(AcsConstruct<GamePlayScene>());
    if (s != nullptr) {
        s->root = MakeUnique<FNode2D>();
        // Default2D(Clock/Tweens/Sequences/Input) + Camera2D + Physics2D。services は DLL 内で生成・所有
        // → cross-DLL 安全。root に配線しておくと、以降 add_component 時に各コンポーネントの
        //   OnAttachServices がガード付きで即発火する (services は walk-to-root で解決できる)。
        s->services = MakeUnique<FSceneServices>(ESvc::Default2D | ESvc::Camera2D | ESvc::Physics2D);
        if (s->root && s->services) s->root->_SetSceneServices(s->services.Get());
    }
    return s;
}

/** ノードを追加する (parent_idx < 0 で root 直下)。戻り値はノード index。 */
GR_API int acs_game_scene_add_node(void* scene, int parent_idx) noexcept {
    auto* s = static_cast<GamePlayScene*>(scene);
    if (s == nullptr || !s->root) return -1;
    FNode2D* parent = (parent_idx >= 0 && static_cast<unsigned>(parent_idx) < s->nodes.Size())
                          ? s->nodes[static_cast<u32>(parent_idx)] : s->root.Get();
    FNode2D& child = parent->AddChild(MakeUnique<FNode2D>());
    s->nodes.PushBack(&child);
    return static_cast<int>(s->nodes.Size()) - 1;
}

/**
 * 既存ノードの親を付け替える (child_idx を parent_idx 配下へ。parent_idx<0 で root 直下)。
 *
 * @details
 * editor のノード列が «親より先に子» の順 (reparent 後にあり得る) でも、まず全ノードを root 直下に
 * 平坦生成してから本関数で正しい親へ付け替えれば、Play シーンの親子構造が崩れない (= 子の World()
 * が親に追従する)。Reparent は Local を保持するので、set_transform 済みのローカル値はそのまま親基準
 * になる。構築時 (tick 前) なので即 ResolveStructuralChanges して確定する。
 * @param scene Play シーンハンドル。
 * @param child_idx 付け替える子ノード index。
 * @param parent_idx 新しい親ノード index (<0 で root)。
 */
GR_API void acs_game_scene_set_parent(void* scene, int child_idx, int parent_idx) noexcept {
    auto* s = static_cast<GamePlayScene*>(scene);
    if (s == nullptr || !s->root) return;
    if (child_idx < 0 || static_cast<unsigned>(child_idx) >= s->nodes.Size()) return;
    FNode2D* child  = s->nodes[static_cast<u32>(child_idx)];
    FNode2D* parent = (parent_idx >= 0 && static_cast<unsigned>(parent_idx) < s->nodes.Size())
                          ? s->nodes[static_cast<u32>(parent_idx)] : s->root.Get();
    if (child == nullptr || parent == nullptr || child == parent) return;
    child->Reparent(*parent);
    s->root->ResolveStructuralChanges();   // 構築時なので即時適用
}

/** ノードのローカル transform を設定する。 */
GR_API void acs_game_scene_set_transform(void* scene, int idx, float x, float y, float rot, float sx, float sy) noexcept {
    auto* s = static_cast<GamePlayScene*>(scene);
    if (s == nullptr || idx < 0 || static_cast<unsigned>(idx) >= s->nodes.Size()) return;
    FTransform2D& t = s->nodes[static_cast<u32>(idx)]->Local();
    t.position = FVec2{ x, y }; t.rotation = rot; t.scale = FVec2{ sx, sy };
}

/** ノードへ実コンポーネントを attach する (反射名で生成)。戻り値はコンポーネント slot (失敗 -1)。 */
GR_API int acs_game_scene_add_component(void* scene, int idx, const char* type_name) noexcept {
    auto* s = static_cast<GamePlayScene*>(scene);
    if (s == nullptr || idx < 0 || static_cast<unsigned>(idx) >= s->nodes.Size() || type_name == nullptr) return -1;
    TUniquePtr<FComponent2D> comp = CreateComponentByName(type_name);
    if (!comp) return -1;
    FNode2D* n = s->nodes[static_cast<u32>(idx)];
    n->AttachComponent(static_cast<TUniquePtr<FComponent2D>&&>(comp));
    return static_cast<int>(n->ComponentCount()) - 1;
}

/** コンポーネントのプロパティ (authored 値) を実メンバへ適用する。 */
GR_API int acs_game_scene_set_prop(void* scene, int idx, int slot, const char* field, float x, float y, float z, float w) noexcept {
    auto* s = static_cast<GamePlayScene*>(scene);
    if (s == nullptr || idx < 0 || static_cast<unsigned>(idx) >= s->nodes.Size() || field == nullptr) return 0;
    FComponent2D* c = s->nodes[static_cast<u32>(idx)]->ComponentAt(static_cast<u32>(slot));
    if (c == nullptr) return 0;
    const FTypeDesc* d = FTypeRegistry::Get().FindByName(c->ReflectName());
    if (d == nullptr) return 0;
    const float v[4] = { x, y, z, w };
    return ApplyValueByName(static_cast<void*>(c), *d, field, v) ? 1 : 0;
}

/** シーンを 1 フレーム tick する (services 2 段 tick で OnUpdate + Tweens/Sequences/Camera を進める)。 */
GR_API void acs_game_scene_tick(void* scene, float dt) noexcept {
    auto* s = static_cast<GamePlayScene*>(scene);
    if (s == nullptr || !s->root) return;
    if (s->services) {
        // FScene2D と同じ 2 段モデル: PreUpdate(Clock) → OnUpdate → PostUpdate(Tweens/Seq/Camera)。
        s->services->_PreUpdate(dt);
        const float sdt = s->services->_ScaledDt(dt);
        s->root->UpdateTree(sdt);
        s->root->ResolveStructuralChanges();
        s->services->_PostUpdate(sdt);
    } else {
        s->root->UpdateTree(dt);
        s->root->ResolveStructuralChanges();
    }
}

/** ノードのローカル transform を読み戻す (editor が描画へ反映する)。 */
GR_API void acs_game_scene_get_transform(void* scene, int idx, float* x, float* y, float* rot, float* sx, float* sy) noexcept {
    auto* s = static_cast<GamePlayScene*>(scene);
    if (s == nullptr || idx < 0 || static_cast<unsigned>(idx) >= s->nodes.Size()) return;
    const FTransform2D& t = s->nodes[static_cast<u32>(idx)]->Local();
    if (x) *x = t.position.x; if (y) *y = t.position.y; if (rot) *rot = t.rotation;
    if (sx) *sx = t.scale.x;  if (sy) *sy = t.scale.y;
}

/**
 * Play シーンを描画する: 各ノード/コンポーネントの OnDraw を editor の SpriteBatch へ積む。
 *
 * @details
 * sb は editor 所有の FSpriteBatch で、呼び出し側 (editor_abi) が Begin + (world view への)
 * SetView 済みであること。RenderContext に sb と world→screen ビューを配線し DrawTree を呼ぶ。
 * **全 OnDraw の vtable 呼び出しは本 DLL 内で起きる**ため、ユーザー定義コンポーネントの描画も
 * cross-DLL 安全 (factory/所有/tick と同じ「DLL 内で完結」方式)。Cmd() は配線しない
 * (2D コンポーネントは rc.HasSprites()/Sprites() のみ使う) ので、Cmd 依存の描画は対象外。
 * @param scene Play シーンハンドル。
 * @param sprite_batch editor 所有の FSpriteBatch (void* で受けて cast)。
 * @param view_cx world→screen ビュー中心 X (rc.ViewCenter)。
 * @param view_cy world→screen ビュー中心 Y。
 * @param view_scale world→screen スケール (rc.ViewScale)。
 * @param w 画面幅 (px)。
 * @param h 画面高さ (px)。
 */
GR_API void acs_game_scene_draw(void* scene, void* sprite_batch,
                                float view_cx, float view_cy, float view_scale,
                                unsigned w, unsigned h) noexcept {
    auto* s  = static_cast<GamePlayScene*>(scene);
    auto* sb = static_cast<FSpriteBatch*>(sprite_batch);
    if (s == nullptr || !s->root || sb == nullptr) return;
    (void)w; (void)h;   // 予約 (将来 rc.Width/Height を配線する場合に使用)
    RenderContext rc;
    rc._SetSpriteBatch(sb);
    rc._SetView2D(FVec2{ view_cx, view_cy }, view_scale);
    s->root->DrawTree(rc);
}

/**
 * editor から渡されたキーイベントを «この DLL の» acs::Input へ流す。
 *
 * @details
 * FInputMap (FSceneServices::Input) は poll ベースで、query 時に acs::Input::* を呼ぶ。
 * acs::Input の状態はモジュールごとのグローバルなので、ユーザーコンポーネント (この reflect DLL)
 * が読む g_input へ届けるには «この DLL 内» で OnEvent する必要がある。よって editor_abi は本関数を
 * 通して入力をフィードする (editor_abi 自身の g_input では届かない)。
 * @param keycode acs::EKey の整数値。
 * @param down 1=押下, 0=解放。
 */
GR_API void acs_game_input_key(int keycode, int down) noexcept {
    acs::Event e;
    e.type       = (down != 0) ? acs::EventType::KeyPressed : acs::EventType::KeyReleased;
    e.key.key    = static_cast<acs::EKey>(static_cast<acs::u16>(keycode));
    e.key.repeat = false;
    acs::Input::OnEvent(e);
}

/** Play フレーム先頭で呼び、この DLL の入力状態を now→prev に進める (IsPressed/Released エッジ用)。 */
GR_API void acs_game_input_update() noexcept {
    acs::Input::Update();
}

/** マウスボタンイベントをこの DLL の acs::Input へ流す (button: 0=Left,1=Right,2=Middle)。 */
GR_API void acs_game_input_mouse_button(int button, int down) noexcept {
    acs::Event e;
    e.type = (down != 0) ? acs::EventType::MouseButtonPressed : acs::EventType::MouseButtonReleased;
    e.mouse_button.button = static_cast<acs::EMouseButton>(static_cast<acs::u8>(button));
    acs::Input::OnEvent(e);
}

/** マウス移動 (viewport クライアント座標 px) をこの DLL の acs::Input へ流す。 */
GR_API void acs_game_input_mouse_move(float x, float y) noexcept {
    acs::Event e;
    e.type = acs::EventType::MouseMoved;
    e.mouse_move.x = x; e.mouse_move.y = y; e.mouse_move.dx = 0.0f; e.mouse_move.dy = 0.0f;
    acs::Input::OnEvent(e);
}

/** Play シーンのカメラ (Services().Camera()) を設定する (Play 開始時に editor view へ合わせる)。 */
GR_API void acs_game_scene_set_camera(void* scene, float cx, float cy, float zoom) noexcept {
    auto* s = static_cast<GamePlayScene*>(scene);
    if (s == nullptr || !s->services || !s->services->Has(ESvc::Camera2D)) return;
    s->services->Camera().SetPosition(FVec2{ cx, cy });
    s->services->Camera().SetZoom(zoom);
}

/** Play シーンのカメラ位置/zoom を読み戻す (editor viewport を追従させる)。 */
GR_API void acs_game_scene_get_camera(void* scene, float* cx, float* cy, float* zoom) noexcept {
    auto* s = static_cast<GamePlayScene*>(scene);
    if (s == nullptr || !s->services || !s->services->Has(ESvc::Camera2D)) return;
    const FVec2 p = s->services->Camera().Position();
    if (cx)   *cx   = p.x;
    if (cy)   *cy   = p.y;
    if (zoom) *zoom = s->services->Camera().Zoom();
}

/** Play シーンを破棄する (全実コンポーネントも DLL 側で安全に破棄)。 */
GR_API void acs_game_scene_destroy(void* scene) noexcept {
    auto* s = static_cast<GamePlayScene*>(scene);
    // AcsConstruct が記録した生成時アロケータへ返す。メンバは先にデストラクタで破棄される。
    AcsDestruct<GamePlayScene>(s);
}

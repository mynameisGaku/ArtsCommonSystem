// SPDX-License-Identifier: Apache-2.0
// 配布先だけを include/link して header と実装 library の整合を確認する。
#include <acs.h>
#include <cstdio>
#include <type_traits>

static_assert(sizeof(acs::FTimerHandle) == 8u, "event タイマーハンドルは 8byte の独立型です");
static_assert(sizeof(acs::game::FSceneTimerHandle) == 4u, "シーンタイマーハンドルは 4byte の packed 型です");
static_assert(sizeof(acs::FLogSinkHandle) == 8u, "ログ購読ハンドルは枠番号と世代番号の 8byte 型です");
static_assert(acs::IsSameV<acs::FObject, acs::AObject>, "旧オブジェクト基底名が正規型を指していません");
static_assert(acs::IsSameV<acs::FAudioEngine, acs::CAudioEngine>, "旧音声管理器名が正規型を指していません");
static_assert(acs::IsSameV<acs::FMessageBroker, acs::CMessageBroker>, "旧メッセージ仲介器名が正規型を指していません");
static_assert(acs::IsSameV<acs::FTimerManager, acs::CTimerManager>, "旧タイマー管理器名が正規型を指していません");
static_assert(acs::IsSameV<acs::scripting::FLuaVm, acs::scripting::CLuaVm>, "旧Lua実行環境名が正規型を指していません");
static_assert(acs::IsSameV<acs::FEventTypeId, acs::u32> && acs::IsSameV<acs::EventTypeId, acs::FEventTypeId>, "イベント通路番号の正規型と旧名が一致しません");
static_assert(acs::IsSameV<acs::FComponentTypeId, acs::u32> && acs::IsSameV<acs::ComponentTypeId, acs::FComponentTypeId>, "コンポーネント番号の正規型と旧名が一致しません");
static_assert(acs::IsSameV<acs::FComponentSignatureId, acs::u64> && acs::IsSameV<acs::ComponentSignatureId, acs::FComponentSignatureId>, "コンポーネント署名の正規型と旧名が一致しません");
static_assert(std::is_same_v<decltype(&acs::TArray<acs::i32>::Remove), bool (acs::TArray<acs::i32>::*)(const acs::i32&) noexcept>);
static_assert(std::is_same_v<decltype(&acs::TInlineArray<acs::i32, 2u>::Remove), bool (acs::TInlineArray<acs::i32, 2u>::*)(const acs::i32&) noexcept>);
static_assert(std::is_same_v<decltype(&acs::TObservableArray<acs::i32>::Remove), bool (acs::TObservableArray<acs::i32>::*)(const acs::i32&) noexcept>);

/** CDebugDraw の既存・追加公開入口を配布 header と library 間で照合する署名。 */
using FDebugDraw2DLineSignature = void (acs::CDebugDraw::*)(acs::FVec2, acs::FVec2, acs::FVec4) noexcept;
using FDebugDraw2DAabbSignature = void (acs::CDebugDraw::*)(const acs::FAabb2&, acs::FVec4) noexcept;
using FDebugDraw2DCircleSignature = void (acs::CDebugDraw::*)(const acs::FCircle&, acs::FVec4, acs::u32) noexcept;
using FDebugDraw2DCrossSignature = void (acs::CDebugDraw::*)(acs::FVec2, acs::f32, acs::FVec4) noexcept;
using FDebugDraw2DArrowSignature = void (acs::CDebugDraw::*)(acs::FVec2, acs::FVec2, acs::FVec4, acs::f32) noexcept;
using FDebugDraw2DTryLineSignature = bool (acs::CDebugDraw::*)(acs::FVec2, acs::FVec2, acs::FVec4) noexcept;
using FDebugDraw2DTryAabbSignature = bool (acs::CDebugDraw::*)(const acs::FAabb2&, acs::FVec4) noexcept;
using FDebugDraw2DTryCircleSignature = bool (acs::CDebugDraw::*)(const acs::FCircle&, acs::FVec4, acs::u32) noexcept;
using FDebugDraw2DTryCrossSignature = bool (acs::CDebugDraw::*)(acs::FVec2, acs::f32, acs::FVec4) noexcept;
using FDebugDraw2DTryArrowSignature = bool (acs::CDebugDraw::*)(acs::FVec2, acs::FVec2, acs::FVec4, acs::f32) noexcept;

static_assert(acs::IsSameV<acs::FDebugDraw, acs::CDebugDraw>);
static_assert(std::is_same_v<decltype(&acs::CDebugDraw::DrawLine), FDebugDraw2DLineSignature>);
static_assert(std::is_same_v<decltype(&acs::CDebugDraw::DrawAabb), FDebugDraw2DAabbSignature>);
static_assert(std::is_same_v<decltype(&acs::CDebugDraw::DrawCircle), FDebugDraw2DCircleSignature>);
static_assert(std::is_same_v<decltype(&acs::CDebugDraw::DrawCross), FDebugDraw2DCrossSignature>);
static_assert(std::is_same_v<decltype(&acs::CDebugDraw::DrawArrow), FDebugDraw2DArrowSignature>);
static_assert(std::is_same_v<decltype(&acs::CDebugDraw::TryDrawLine), FDebugDraw2DTryLineSignature>);
static_assert(std::is_same_v<decltype(&acs::CDebugDraw::TryDrawAabb), FDebugDraw2DTryAabbSignature>);
static_assert(std::is_same_v<decltype(&acs::CDebugDraw::TryDrawCircle), FDebugDraw2DTryCircleSignature>);
static_assert(std::is_same_v<decltype(&acs::CDebugDraw::TryDrawCross), FDebugDraw2DTryCrossSignature>);
static_assert(std::is_same_v<decltype(&acs::CDebugDraw::TryDrawArrow), FDebugDraw2DTryArrowSignature>);

/** FDebugDraw3Dの既存・追加公開入口を配布headerとlibrary間で照合する署名。 */
using FDebugDrawInitSignature = acs::TResult<void> (acs::FDebugDraw3D::*)(acs::IRhiDevice&, acs::EFormat, acs::u32) noexcept;
using FDebugDrawVoidSignature = void (acs::FDebugDraw3D::*)() noexcept;
using FDebugDrawLineSignature = void (acs::FDebugDraw3D::*)(acs::FVec3, acs::FVec3, acs::FVec4) noexcept;
using FDebugDrawAabbSignature = void (acs::FDebugDraw3D::*)(const acs::FAabb3&, acs::FVec4) noexcept;
using FDebugDrawWireframeSignature = void (acs::FDebugDraw3D::*)(const acs::FVec3*, acs::u32, const acs::u32*, acs::u32, acs::FVec4) noexcept;
using FDebugDrawEndSignature = void (acs::FDebugDraw3D::*)(acs::IRhiCommandList&, const acs::FMat4&) noexcept;
using FDebugDrawTryLineSignature = bool (acs::FDebugDraw3D::*)(acs::FVec3, acs::FVec3, acs::FVec4) noexcept;
using FDebugDrawTryAabbSignature = bool (acs::FDebugDraw3D::*)(const acs::FAabb3&, acs::FVec4) noexcept;
using FDebugDrawTryWireframeSignature = bool (acs::FDebugDraw3D::*)(const acs::FVec3*, acs::u32, const acs::u32*, acs::u32, acs::FVec4) noexcept;

static_assert(std::is_same_v<acs::CDebugDraw3D, acs::FDebugDraw3D>);
static_assert(std::is_same_v<decltype(&acs::FDebugDraw3D::Init), FDebugDrawInitSignature>);
static_assert(std::is_same_v<decltype(&acs::FDebugDraw3D::Shutdown), FDebugDrawVoidSignature>);
static_assert(std::is_same_v<decltype(&acs::FDebugDraw3D::Begin), FDebugDrawVoidSignature>);
static_assert(std::is_same_v<decltype(&acs::FDebugDraw3D::Line), FDebugDrawLineSignature>);
static_assert(std::is_same_v<decltype(&acs::FDebugDraw3D::Aabb), FDebugDrawAabbSignature>);
static_assert(std::is_same_v<decltype(&acs::FDebugDraw3D::Wireframe), FDebugDrawWireframeSignature>);
static_assert(std::is_same_v<decltype(&acs::FDebugDraw3D::End), FDebugDrawEndSignature>);
static_assert(std::is_same_v<decltype(&acs::FDebugDraw3D::TryLine), FDebugDrawTryLineSignature>);
static_assert(std::is_same_v<decltype(&acs::FDebugDraw3D::TryAabb), FDebugDrawTryAabbSignature>);
static_assert(std::is_same_v<decltype(&acs::FDebugDraw3D::TryWireframe), FDebugDrawTryWireframeSignature>);

/** 3D Prefab source node identityの公開getter署名。 */
using FPrefabSourceNodeIdSignature = acs::FStringView (acs::game::APrefabNodeIdentity3DComponent::*)() const noexcept;
/** 3D Prefab source node identityの検証付きsetter署名。 */
using FTrySetPrefabSourceNodeIdSignature = bool (acs::game::APrefabNodeIdentity3DComponent::*)(acs::FStringView) noexcept;
/** 3D Prefab child node property override maskの公開getter署名。 */
using FPrefabNodePropertyOverrideMaskSignature = acs::u32 (acs::game::APrefabNodeIdentity3DComponent::*)() const noexcept;
/** 3D Prefab child node property override maskの検証付きsetter署名。 */
using FTrySetPrefabNodePropertyOverrideMaskSignature = bool (acs::game::APrefabNodeIdentity3DComponent::*)(acs::u32) noexcept;

static_assert(std::is_same_v<decltype(&acs::game::APrefabNodeIdentity3DComponent::SourceNodeId), FPrefabSourceNodeIdSignature>);
static_assert(std::is_same_v<decltype(&acs::game::APrefabNodeIdentity3DComponent::TrySetSourceNodeId), FTrySetPrefabSourceNodeIdSignature>);
static_assert(std::is_same_v<decltype(&acs::game::APrefabNodeIdentity3DComponent::NodePropertyOverrideMask), FPrefabNodePropertyOverrideMaskSignature>);
static_assert(std::is_same_v<decltype(&acs::game::APrefabNodeIdentity3DComponent::TrySetNodePropertyOverrideMask), FTrySetPrefabNodePropertyOverrideMaskSignature>);

/** world-space命中情報を返す3D描画形状raycastの公開署名。 */
using FScene3DRaycastSignature = bool (acs::game::CSceneNodeGraph::*)(const acs::FRay3&, acs::f32, acs::f32, acs::game::FScene3DRaycastHit&) const noexcept;

static_assert(std::is_same_v<decltype(&acs::game::CSceneNodeGraph::TryRaycastGeometryActiveRange), FScene3DRaycastSignature>);

/** Primitive の既存・追加公開入口を配布 header と library 間で照合する署名。 */
using FPrimitiveMakeCubeSignature = acs::TSharedPtr<acs::AMeshAsset> (*)(acs::f32) noexcept;
using FPrimitiveMakeSphereSignature = acs::TSharedPtr<acs::AMeshAsset> (*)(acs::f32, acs::u32, acs::u32) noexcept;
using FPrimitiveMakePlaneSignature = acs::TSharedPtr<acs::AMeshAsset> (*)(acs::f32, acs::f32) noexcept;
using FPrimitiveTryMakeCubeSignature = bool (*)(acs::f32, acs::TSharedPtr<acs::AMeshAsset>&) noexcept;
using FPrimitiveTryMakeSphereSignature = bool (*)(acs::f32, acs::u32, acs::u32, acs::TSharedPtr<acs::AMeshAsset>&) noexcept;
using FPrimitiveTryMakePlaneSignature = bool (*)(acs::f32, acs::f32, acs::TSharedPtr<acs::AMeshAsset>&) noexcept;

static_assert(std::is_same_v<decltype(&acs::Primitive::MakeCube), FPrimitiveMakeCubeSignature>);
static_assert(std::is_same_v<decltype(&acs::Primitive::MakeSphere), FPrimitiveMakeSphereSignature>);
static_assert(std::is_same_v<decltype(&acs::Primitive::MakePlane), FPrimitiveMakePlaneSignature>);
static_assert(std::is_same_v<decltype(&acs::Primitive::TryMakeCube), FPrimitiveTryMakeCubeSignature>);
static_assert(std::is_same_v<decltype(&acs::Primitive::TryMakeSphere), FPrimitiveTryMakeSphereSignature>);
static_assert(std::is_same_v<decltype(&acs::Primitive::TryMakePlane), FPrimitiveTryMakePlaneSignature>);

/** 配布物のイベント通路番号を割り当てる検査型。 */
struct FDistributionEventProbe {};

/** 配布物のコンポーネント番号と署名を割り当てる検査型。 */
struct FDistributionComponentProbe {};

/**
 * 配布ライブラリを経由したシーンタイマー発火を記録する。
 *
 * @param user acs::u32 のアドレス。
 */
void CountSceneTimerFire(void* user) noexcept
{
    /** 呼び出し側が所有する発火回数。 */
    auto& fire_count = *static_cast<acs::u32*>(user);
    ++fire_count;
}

/**
 * 配布ライブラリを経由したログ購読通知を記録する。
 *
 * @param severity 通知されたログ重大度。
 * @param message 通知されたnull終端本文。
 * @param user acs::u32 のアドレス。
 */
void CountLogSink(acs::ELogSeverity severity, const char* message, void* user) noexcept
{
    /** 呼び出し側が所有する通知回数。 */
    auto& notification_count = *static_cast<acs::u32*>(user);
    if (severity == acs::ELogSeverity::Info && message != nullptr) ++notification_count;
}

/** 配布 library に Primitive の既存 3 入口と追加 3 入口が揃うことを link で固定する。 */
void LinkMeshPrimitiveSymbols() noexcept
{
    /** linker が除去できない既存公開 symbol の関数 pointer。 */
    volatile FPrimitiveMakeCubeSignature make_cube = &acs::Primitive::MakeCube;
    volatile FPrimitiveMakeSphereSignature make_sphere = &acs::Primitive::MakeSphere;
    volatile FPrimitiveMakePlaneSignature make_plane = &acs::Primitive::MakePlane;
    /** linker が除去できない追加公開 symbol の関数 pointer。 */
    volatile FPrimitiveTryMakeCubeSignature try_make_cube = &acs::Primitive::TryMakeCube;
    volatile FPrimitiveTryMakeSphereSignature try_make_sphere = &acs::Primitive::TryMakeSphere;
    volatile FPrimitiveTryMakePlaneSignature try_make_plane = &acs::Primitive::TryMakePlane;
    (void)make_cube;
    (void)make_sphere;
    (void)make_plane;
    (void)try_make_cube;
    (void)try_make_sphere;
    (void)try_make_plane;
}

/** 配布libraryに既存7入口と追加3入口の実symbolが揃うことをlinkで固定する。 */
void LinkDebugDraw3DSymbols() noexcept
{
    /** linkerが除去できない既存公開symbolのmember pointer。 */
    volatile FDebugDrawInitSignature init = &acs::FDebugDraw3D::Init;
    volatile FDebugDrawVoidSignature shutdown = &acs::FDebugDraw3D::Shutdown;
    volatile FDebugDrawVoidSignature begin = &acs::FDebugDraw3D::Begin;
    volatile FDebugDrawLineSignature line = &acs::FDebugDraw3D::Line;
    volatile FDebugDrawAabbSignature aabb = &acs::FDebugDraw3D::Aabb;
    volatile FDebugDrawWireframeSignature wireframe = &acs::FDebugDraw3D::Wireframe;
    volatile FDebugDrawEndSignature end = &acs::FDebugDraw3D::End;
    /** linkerが除去できない追加公開symbolのmember pointer。 */
    volatile FDebugDrawTryLineSignature try_line = &acs::FDebugDraw3D::TryLine;
    volatile FDebugDrawTryAabbSignature try_aabb = &acs::FDebugDraw3D::TryAabb;
    volatile FDebugDrawTryWireframeSignature try_wireframe = &acs::FDebugDraw3D::TryWireframe;
    (void)init;
    (void)shutdown;
    (void)begin;
    (void)line;
    (void)aabb;
    (void)wireframe;
    (void)end;
    (void)try_line;
    (void)try_aabb;
    (void)try_wireframe;
}

/** 配布 library に既存 5 入口と追加 5 入口の symbol が揃うことを link で固定する。 */
void LinkDebugDraw2DSymbols() noexcept
{
    /** linker が除去できない既存公開 symbol の member pointer。 */
    volatile FDebugDraw2DLineSignature line = &acs::CDebugDraw::DrawLine;
    volatile FDebugDraw2DAabbSignature aabb = &acs::CDebugDraw::DrawAabb;
    volatile FDebugDraw2DCircleSignature circle = &acs::CDebugDraw::DrawCircle;
    volatile FDebugDraw2DCrossSignature cross = &acs::CDebugDraw::DrawCross;
    volatile FDebugDraw2DArrowSignature arrow = &acs::CDebugDraw::DrawArrow;
    /** linker が除去できない追加公開 symbol の member pointer。 */
    volatile FDebugDraw2DTryLineSignature try_line = &acs::CDebugDraw::TryDrawLine;
    volatile FDebugDraw2DTryAabbSignature try_aabb = &acs::CDebugDraw::TryDrawAabb;
    volatile FDebugDraw2DTryCircleSignature try_circle = &acs::CDebugDraw::TryDrawCircle;
    volatile FDebugDraw2DTryCrossSignature try_cross = &acs::CDebugDraw::TryDrawCross;
    volatile FDebugDraw2DTryArrowSignature try_arrow = &acs::CDebugDraw::TryDrawArrow;
    (void)line;
    (void)aabb;
    (void)circle;
    (void)cross;
    (void)arrow;
    (void)try_line;
    (void)try_aabb;
    (void)try_circle;
    (void)try_cross;
    (void)try_arrow;
}

/** 配布libraryに3D Prefab source node identityの実symbolが揃うことをlinkで固定する。 */
void LinkPrefabSourceNodeIdentitySymbols() noexcept
{
    /** linkerが除去できないgetter symbolのmember pointer。 */
    volatile FPrefabSourceNodeIdSignature source_node_id = &acs::game::APrefabNodeIdentity3DComponent::SourceNodeId;
    /** linkerが除去できない検証付きsetter symbolのmember pointer。 */
    volatile FTrySetPrefabSourceNodeIdSignature try_set_source_node_id = &acs::game::APrefabNodeIdentity3DComponent::TrySetSourceNodeId;
    /** linkerが除去できないnode override getter symbolのmember pointer。 */
    volatile FPrefabNodePropertyOverrideMaskSignature node_override_mask = &acs::game::APrefabNodeIdentity3DComponent::NodePropertyOverrideMask;
    /** linkerが除去できないnode override setter symbolのmember pointer。 */
    volatile FTrySetPrefabNodePropertyOverrideMaskSignature try_set_node_override_mask = &acs::game::APrefabNodeIdentity3DComponent::TrySetNodePropertyOverrideMask;
    (void)source_node_id;
    (void)try_set_source_node_id;
    (void)node_override_mask;
    (void)try_set_node_override_mask;
}

/** 配布SDKのheader、外部symbol、基本計算を検証し、失敗時は1を返す。 */
int main()
{
    using namespace acs;

    LinkMeshPrimitiveSymbols();
    LinkDebugDraw2DSymbols();
    LinkDebugDraw3DSymbols();
    LinkPrefabSourceNodeIdentitySymbols();

    // containerの基本操作を検証する値。
    TArray<i32> v;
    v.Add(10);
    v.Add(32);
    v.Add(10);
    // 最初の一致だけを順序保持で削除できたか。
    const bool array_remove_ok = v.Remove(10) && v.Num() == 2u && v[0] == 32 && v[1] == 10;
    // containerから得た合計値。
    i32 sum = 0;
    for (usize i = 0; i < v.Num(); ++i)
    {
        sum += v[i];
    }

    // 直接領域を越えた配列でも同じ削除契約を検証する値。
    TInlineArray<i32, 2u> inline_values;
    inline_values.Add(7);
    inline_values.Add(8);
    inline_values.Add(7);
    // 動的領域へ移行した配列の削除結果。
    const bool inline_remove_ok = inline_values.Remove(7) && inline_values.Num() == 2u && inline_values[0] == 8 && inline_values[1] == 7;

    // 通知付き配列の値削除を検証する値。
    TObservableArray<i32> observable_values;
    observable_values.Add(3);
    observable_values.Add(4);
    // 値削除後の順序と未一致時の不変結果。
    const bool observable_remove_ok = observable_values.Remove(3) && !observable_values.Remove(9) && observable_values.Num() == 1u && observable_values.At(0) == 4;

    // 距離計算の始点。
    FVec2 a{0.0f, 0.0f};
    // 距離計算の終点。
    FVec2 b{3.0f, 4.0f};
    // 行列APIの生成結果。
    FMat4 m = FMat4::Identity();
    (void)m;

    // windowやGPUを使わない距離結果。
    const f32 dist = easy::Distance(a.x, a.y, b.x, b.y);
    // 範囲制限helperの結果。
    const f32 clamp = easy::Clamp(123.0f, 0.0f, 100.0f);
    // 二次元vector長の結果。
    const f32 len = easy::Length(b.x, b.y);

    // acs.libの非inline実装を必ずlinkさせる入力。
    constexpr char kHashProbe[] = "acs";
    // 現行HashBytes契約で固定した期待値。
    constexpr u64 kExpectedHash = 0x2773fad09b34e937ull;
    // header宣言と配布library実装を跨いだhash結果。
    const u64 linked_hash = HashBytes(kHashProbe, sizeof(kHashProbe) - 1u);

    /** 配布headerから割り当てたイベント通路番号。 */
    const FEventTypeId event_channel = GetEventTypeId<FDistributionEventProbe>();
    /** 初期購読数を検査するメッセージ仲介器。 */
    CMessageBroker event_broker;
    /** イベント通路番号と初期購読数の整合結果。 */
    const bool event_identifier_ok = IsValidEventTypeId(event_channel) && event_broker.SubscriberCount(event_channel) == 0u;

    /** 配布headerから割り当てたコンポーネント番号。 */
    const FComponentTypeId component_id = GetComponentTypeId<FDistributionComponentProbe>();
    /** 型登録で得たコンポーネント操作情報。 */
    const FComponentOps& registered_component = CComponentRegistry::Register<FDistributionComponentProbe>();
    /** 同じ番号から取得したコンポーネント操作情報。 */
    const FComponentOps& fetched_component = CComponentRegistry::Get(component_id);
    /** コンポーネント番号、操作情報、署名の整合結果。 */
    const bool component_identifier_ok = component_id == TComponentTypeTraits<FDistributionComponentProbe>::RuntimeId() && &registered_component == &fetched_component && GetComponentSignatureId<FDistributionComponentProbe>() == TComponentTypeTraits<FDistributionComponentProbe>::Signature;

    /** 配布headerとlibraryを跨いで3D Prefab source node identityを保持するscene。 */
    game::CSceneNodeGraph prefab_identity_scene;
    /** rootへ所有させた3D Prefab source node identity component。 */
    game::APrefabNodeIdentity3DComponent& prefab_identity = prefab_identity_scene.Root().AddComponent<game::APrefabNodeIdentity3DComponent>();
    /** canonical値の設定、非canonical値の拒否、既存値保持をまとめた結果。 */
    const bool prefab_source_identity_ok = prefab_identity.TrySetSourceNodeId(FStringView("0123456789abcdef0123456789abcdef")) && !prefab_identity.TrySetSourceNodeId(FStringView("0123456789ABCDEF0123456789ABCDEF")) && prefab_identity.SourceNodeId() == FStringView("0123456789abcdef0123456789abcdef") && prefab_identity.TrySetNodePropertyOverrideMask(255u) && !prefab_identity.TrySetNodePropertyOverrideMask(256u) && prefab_identity.NodePropertyOverrideMask() == 255u;

    /** 配布headerとlibraryだけで厳密3D raycastを実行するscene。 */
    game::CSceneNodeGraph raycast_scene;
    /** +Z rayの前面に置く単位cube。 */
    game::ANode& raycast_cube = raycast_scene.Spawn(FStringView("RaycastCube"));
    raycast_cube.AddComponent<game::AMeshComponent3D>(game::EMeshPrimitive3D::Cube);
    /** node、parameter、world点、world法線を受け取る結果。 */
    game::FScene3DRaycastHit raycast_hit;
    /** 新しい非inline公開symbolと配布libraryの整合結果。 */
    const bool scene_raycast_ok = raycast_scene.TryRaycastGeometryActiveRange(FRay3{FVec3{0.0f, 0.0f, -2.0f}, FVec3{0.0f, 0.0f, 1.0f}}, 0.0f, 4.0f, raycast_hit) && raycast_hit.Node == raycast_cube.Id() && raycast_hit.T == 1.5f && raycast_hit.Point.x == 0.0f && raycast_hit.Point.y == 0.0f && raycast_hit.Point.z == -0.5f && raycast_hit.Normal.x == 0.0f && raycast_hit.Normal.y == 0.0f && raycast_hit.Normal.z == -1.0f;

    // 呼び出し側が所有するシーンタイマー。
    game::CSceneTimer scene_timer;
    // シーンタイマーの発火回数。
    u32 scene_timer_fire_count = 0u;
    // 配布ライブラリの新しい修飾シンボルを参照する正規ハンドル。
    const game::FSceneTimerHandle scene_timer_handle = scene_timer.SetTimeout(1.0f, &CountSceneTimerFire, &scene_timer_fire_count);
    scene_timer.Tick(1.0f);
    // 登録、発火、完了をまとめて確認する結果。
    const bool scene_timer_ok = scene_timer_handle.IsValid() && scene_timer_fire_count == 1u && !scene_timer.IsActive(scene_timer_handle);

    // 配布ライブラリの複数ログ通知先を画面出力なしで検証する設定。
    FLogConfig log_config{};
    log_config.console = false;
    log_config.debug_output = false;
    CLogger::Init(log_config);
    // ログ購読から受け取った通知回数。
    u32 log_notification_count = 0u;
    FLogSinkSubscription log_subscription = CLogger::SubscribeSinkOwned(&CountLogSink, &log_notification_count);
    ACS_LOG_INFO("distribution log sink");
    CLogger::Flush();
    // 購読の有効性と通知到達をまとめて確認する結果。
    const bool log_sink_ok = log_subscription.IsValid() && log_notification_count == 1u;
    CLogger::Shutdown();

    std::printf("acs.h OK | sum=%d dist=%.1f clamp=%.1f len=%.1f hash=%016llx event=%u component=%u prefab_source_id=%u scene_raycast=%u scene_timer=%u log_sink=%u\n", sum, dist, clamp, len, static_cast<unsigned long long>(linked_hash), event_identifier_ok ? 1u : 0u, component_identifier_ok ? 1u : 0u, prefab_source_identity_ok ? 1u : 0u, scene_raycast_ok ? 1u : 0u, scene_timer_fire_count, log_notification_count);
    return (array_remove_ok && inline_remove_ok && observable_remove_ok && sum == 42 && dist == 5.0f && clamp == 100.0f && len == 5.0f && linked_hash == kExpectedHash && event_identifier_ok && component_identifier_ok && prefab_source_identity_ok && scene_raycast_ok && scene_timer_ok && log_sink_ok) ? 0 : 1;
}

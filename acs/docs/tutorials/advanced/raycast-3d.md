# 3D レイキャスト

[`FRay3`](../../reference/symbols/math/fray3-254bb00c.html) は始点 `origin` と方向 `direction` を持つ3Dレイです。交差関数は [`FRayHit3`](../../reference/symbols/math/frayhit3-f84cdfa2.html) を返し、命中時に `hit`、媒介値 `t`、ワールド座標 `point`、面法線 `normal` を設定します。

## 画面位置から最も近い AABB を選ぶ

[`ScreenPointToRay`](../../reference/symbols/math/screenpointtoray-49ea0dbd.html) は、左上原点の画面座標と `CCamera` から正規化済みのワールドレイを作ります。次の例は三つのAABBのうち、指定距離内で最も近い一つを返します。

```cpp
#include "math/CameraRig.h"
#include "math/Collision3D.h"

struct FRaycastSelection
{
    acs::FRayHit3 hit;
    acs::u32 target_index = ~acs::u32{0};
};

FRaycastSelection PickNearest(const acs::CCamera& camera, acs::f32 screen_x, acs::f32 screen_y, acs::f32 viewport_width, acs::f32 viewport_height, acs::f32 max_distance) noexcept
{
    const acs::FRay3 ray = acs::ScreenPointToRay(camera, screen_x, screen_y, viewport_width, viewport_height);

    const acs::FAabb3 targets[] = {
        acs::FAabb3::FromCenterExtents(acs::FVec3{-2.0f, 0.0f, 4.0f}, acs::FVec3{0.5f, 0.5f, 0.5f}),
        acs::FAabb3::FromCenterExtents(acs::FVec3{0.0f, 0.0f, 6.0f}, acs::FVec3{1.0f, 1.0f, 1.0f}),
        acs::FAabb3::FromCenterExtents(acs::FVec3{2.0f, 0.0f, 8.0f}, acs::FVec3{0.75f, 1.5f, 0.75f}),
    };

    FRaycastSelection nearest{};
    acs::f32 nearest_t = max_distance;
    for (acs::u32 index = 0u; index < 3u; ++index)
    {
        const acs::FRayHit3 candidate =
            acs::RaycastAabb(ray, targets[index], nearest_t);
        if (!candidate.hit || candidate.t >= nearest_t)
            continue;
        nearest.hit = candidate;
        nearest.target_index = index;
        nearest_t = candidate.t;
    }
    return nearest;
}
```

`ScreenPointToRay` が返す `direction` は長さ1なので、`t` と `max_distance` はワールド空間の距離として扱えます。自分で `FRay3` を作る場合も、距離として比較するなら `direction` を `Normalize` します。

## 交差先を選ぶ

| 関数 | 対象 | 命中法線 |
|---|---|---|
| [`RaycastAabb`](../../reference/symbols/math/raycastaabb-152e199e.html) | [`FAabb3`](../../reference/symbols/math/faabb3-ab465d8e.html) | 命中した軸の外向き法線 |
| [`RaycastSphere`](../../reference/symbols/math/raycastsphere-41e6b2d6.html) | [`FSphere`](../../reference/symbols/math/fsphere-e5029ea5.html) | 球中心から命中点へ向かう法線 |
| [`RaycastTriangle`](../../reference/symbols/math/raycasttriangle-34f98036.html) | 三つの `FVec3` | レイと逆向きになる両面法線 |
| [`RaycastPlane`](../../reference/symbols/math/raycastplane-949a94c4.html) | [`FPlane`](../../reference/symbols/math/fplane-c1aacdbc.html) | 平面の `FPlane::normal` |

各関数の `t_max` は探索上限です。現在までの最短 `t` を次の呼び出しへ渡すと、それより遠い候補を除外できます。

## ワールド変換を持つ形状

交差関数は、レイと形状が同じ座標空間にあることを前提とします。ノードのローカルAABBを調べる場合は、ワールドレイの始点と方向を逆モデル行列でローカル空間へ移してから交差判定します。

```cpp
const acs::FMat4 inverse_model = acs::Inverse(model);
const acs::FRay3 local_ray{acs::TransformPoint(world_ray.origin, inverse_model), acs::TransformVector(world_ray.direction, inverse_model)};
const acs::FRayHit3 local_hit = acs::RaycastAabb(local_ray, acs::FAabb3::FromCenterExtents(acs::FVec3{0.0f, 0.0f, 0.0f}, acs::FVec3{0.5f, 0.5f, 0.5f}));
```

拡大率を含む変換でも、例のように変換後の `direction` を再正規化しなければ、`t` はワールドレイと同じ媒介値を保ちます。正規化済みワールドレイから変換した場合は、ローカル交差の `t` をワールド距離として比較できます。ローカルの `direction` を再正規化した場合はこの対応が失われるため、ワールド命中点へ戻して距離を求めます。`point` と `normal` はローカル空間の値なので、利用時はそれぞれワールド空間へ変換します。

## 入力条件と非命中

- `ScreenPointToRay` はビューポート寸法を検証しません。幅と高さが0より大きく、カメラのビュー射影行列が逆行列を持つ場合だけ呼びます。
- 交差関数はエラーを返しません。各形状の交差条件を満たさない場合は、既定値の `FRayHit3` を返します。
- レイと形状の値は事前に検証します。`direction` は有限な非0ベクトル、AABBの半サイズは0以上、球の半径は0より大きい値、平面の法線は正規化済みベクトルを使用します。
- `RaycastTriangle` はレイと三角形がほぼ平行な場合と、`t < 1e-6` の交点を非命中にします。
- `RaycastPlane` は法線と方向の内積の絶対値が `1e-8` 未満なら非命中です。
- `FAabb3` は中心と半サイズで表します。最小・最大座標から作る場合は [`FromMinMax`](../../reference/symbols/math/faabb3-ab465d8e/fromminmax-4e971971.html) を使用します。

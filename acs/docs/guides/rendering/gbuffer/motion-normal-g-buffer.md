# モーション・法線G-buffer

[`CMotionVector`](../../../../src/render/MotionVector.h) は、表示対象の静的メッシュを再描画し、画面上の移動量と世界座標の法線を二つの描画先へ同時に出力します。この同時出力を <abbr title="一回の描画で複数のレンダーターゲットへ書き込む方式">MRT</abbr> と呼びます。

移動量は `R16G16_Float` の `.rg` に `previous_uv - current_uv`、法線は `R16G16B16A16_Float` の `.xyz` に正規化した世界座標法線として格納します。遮蔽判定には内部所有の `D32_Float` 深度画像を使います。

## 初期化とサイズ変更

```cpp
#include "render/MotionVector.h"

acs::CMotionVector motion;
if (motion.Init(device, width, height).IsErr()) {
    return;
}
```

`Init` の幅または高さが `0` の場合は1画素へ置き換えます。`Resize` は `Init` 後だけ呼べます。幅または高さが `0` の場合は成功を返しますが、既存画像の大きさを変更しません。最小化中は出力を使用せず、有効な表示寸法になった時点で再度 `Resize` します。

`Resize` は古い描画先を先に解放します。再作成に失敗した場合、そのフレームでは `OutputTexture` と `OutputNormalTexture` を使用しません。

## フレームの記録

`FVisibleMesh` はこの例だけで使う局所型です。ACS の `FGpuMesh`、現在のモデル行列、前フレームのモデル行列をまとめます。

```cpp
struct FVisibleMesh
{
    const acs::FGpuMesh* mesh;
    acs::FMat4 model;
    acs::FMat4 previous_model;
};

bool gbuffer_valid = false;

if (motion.BeginFrame(visible_count) && motion.Begin(command_list, view_projection_no_jitter, previous_view_projection_no_jitter)) {
    bool complete = true;

    for (acs::u32 i = 0; i < visible_count; ++i) {
        const FVisibleMesh& visible = meshes[i];
        if (!visible.mesh || !motion.DrawMesh(command_list, *visible.mesh, visible.model, visible.previous_model)) {
            complete = false;
        }
    }

    motion.End(command_list);
    gbuffer_valid = complete && motion.ObjectDrawCount() == visible_count && motion.OutputTexture() != nullptr && motion.OutputNormalTexture() != nullptr;
}
```

`BeginFrame` は予定描画数ぶんの物体用定数バッファを準備します。`Begin` が成功した後は、途中の `DrawMesh` が失敗しても `End` を必ず呼びます。静止物体は `previous_model` に現在の `model` と同じ行列を渡します。

現在と前フレームの表示・投影行列には、<abbr title="時間的アンチエイリアスの画素ずらしを適用していない行列">ジッターなし行列</abbr>を渡します。色描画だけに適用する画素ずらしを含めると、静止画面にも誤った移動量が生じます。

## 利用側へ公開する

フレーム全体が完成した場合だけ、移動量と法線を後段へ渡します。

```cpp
post_process_params.taa_motion_texture = gbuffer_valid ? motion.OutputTexture() : nullptr;

if (gbuffer_valid) {
    ssr.Render(device, command_list, scene_color, scene_depth, *motion.OutputNormalTexture(), view_projection, inverse_view_projection, previous_view_projection, camera_position, 0.6f, motion.OutputTexture());
}
```

`CSsr`、`CSsgi`、`CSsao` は法線画像を使用し、`CSsr` と `CSsgi` は移動量も任意で使用します。出力が不完全な場合は法線を使う処理を実行せず、`FPostProcessParams::taa_motion_texture` へ `nullptr` を渡して深度によるカメラ移動の再投影へ戻します。

カメラの切り替え、投影方式の変更、シーンの置き換え、再生状態の切り替え、表示寸法の変更では、前フレームとの連続性がありません。`CPostProcess::InvalidateTaaHistory`、`CSsr::InvalidateHistory`、`CSsgi::InvalidateHistory` を呼び、前フレーム行列と各物体の前回行列も現在値へそろえます。

## 失敗条件と制限

- `BeginFrame` が必要数の定数バッファを確保できない場合、そのフレームのパスを開始しません。
- `Begin` は描画先またはパイプラインがない場合と、MRT を開始できない場合に `false` を返します。
- `DrawMesh` はパス開始前、メッシュの頂点またはインデックスバッファがない場合、定数バッファを確保できない場合に `false` を返します。
- `u32` の最大値は内部の無効値として使うため、予定描画数に指定できません。
- パイプラインは `FMeshVertex` の位置、法線、UVを前提とし、画像を参照しません。アルファ抜きメッシュも面全体を不透明として深度へ描きます。
- `DrawMesh` は物体全体の現在行列と前回行列だけを受け取ります。スキニングによる頂点単位の変形は移動量へ含めません。
- 出力画像は `CMotionVector` が所有します。利用側は非所有参照として扱い、`Resize`、`Shutdown`、デストラクターの後に保持しません。

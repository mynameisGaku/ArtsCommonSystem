# 点光源

[`FPointLight`](../../../../src/render/StandardShader.h) は、世界座標の一点から周囲を照らす光の位置、到達距離、色を保持します。[`CStandardShader`](../../../../src/render/StandardShader.h) と [`CSkinnedShader`](../../../../src/render/SkinnedShader.h) が同じ形式を受け取ります。

## 設定

```cpp
const acs::FPointLight point_lights[] = {
    {acs::FVec3{ 2.0f, 1.5f, 0.0f}, 6.0f, acs::FVec3{1.0f, 0.3f, 0.3f}},
    {acs::FVec3{-2.0f, 1.5f, 0.0f}, 6.0f, acs::FVec3{0.3f, 1.0f, 0.4f}},
    {acs::FVec3{ 0.0f, 1.5f, 3.0f}, 6.0f, acs::FVec3{0.3f, 0.5f, 1.0f}},
};

if (!shader.BeginFrame(visible_count)) {
    return;
}
shader.SetLights(view_projection, camera_position, directional_lights, directional_light_count, ambient_color);
shader.SetPointLights(point_lights, 3u);
```

`SetPointLights` は最大4灯を内部へコピーします。5灯目以降は使いません。点光源を無効にするには `SetPointLights(nullptr, 0u)` を呼びます。`count > 0` では、有効な配列ポインターが必要です。

## 減衰

メッシュ表面から点光源までの距離を `distance` とすると、`distance >= range` では影響がゼロになります。範囲内では次の係数を拡散反射と鏡面反射へ掛けます。

```text
(1 - distance / max(range, 0.0001))²
```

`range` と `color` は CPU 側で有限値へ補正しません。`range > 0` の有限値と、有限な位置・色を渡します。光源位置と表面位置が完全に一致すると方向を定義できないため、点光源を照明対象の表面と同じ位置へ置かないでください。

`FPointLight` に独立した強度値はありません。明るさは `color` の各成分へ含めます。

## 制限

- 点光源は有向光源と環境光へ追加して評価します。
- `SetLights` と `SetPointLights` の呼び順には依存しません。どちらも保持中のフレーム状態を定数バッファへ反映します。
- 現在の `CStandardShader` と `CSkinnedShader` は、点光源へシャドウマップを適用しません。
- 光源配列は呼び出し後に破棄できますが、シェーダ自体が保持する GPU 資源は描画完了まで生存させます。

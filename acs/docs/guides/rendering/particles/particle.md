# 2Dパーティクル

[`CParticleSystem`](../../../../src/render/Particles.h) は、固定容量の粒子プールを CPU で更新し、[`CSpriteBatch`](../../../../src/render/SpriteBatch.h) へ四角形の描画を追加します。粒子の発生条件は [`FEmitterDesc`](../../../../src/render/Particles.h) にまとめます。

## 初期化

`Init` は粒子プールを確保します。既定容量は 4096 個です。`0` を指定した場合は 1024 個へ置き換わります。失敗時は空状態になるため、そのフレーム以降は使用せず、エラーを処理します。

```cpp
#include "render/Particles.h"

acs::CParticleSystem particles;
if (particles.Init(8192u).IsErr()) {
    return;
}

particles.SetEmitter(acs::FEmitterDesc::Fire(acs::FVec2{640.0f, 420.0f}));
particles.SetTexture(glow_texture); // nullptr なら白い四角形を使う。
```

`CParticleSystem` は粒子プールを単独所有し、`Shutdown` またはデストラクターで、確保時と同じ `FAllocator` へ返します。`SetTexture` が受け取る `IRhiTexture*` は非所有参照です。描画命令の実行が完了するまで、呼び出し側で画像を生存させます。

## 更新と描画

`Update` へ非負の経過秒を渡し、`CSpriteBatch::Begin` と `End` の間で `Render` を呼びます。

```cpp
particles.Update(delta_seconds);

sprite_batch.Begin(command_list, screen_width, screen_height);
particles.Render(sprite_batch);
sprite_batch.End();
```

`Render` は生存時間に応じて大きさと色を線形補間します。テクスチャを設定していれば `CSpriteBatch::Draw`、設定していなければ `CSpriteBatch::DrawRect` を使います。

## 連続生成と単発生成

`FEmitterDesc::Fire`、`Sparks`、`Fountain`、`Smoke` は、発生位置を受け取って既定設定を返します。独自設定では位置、初速、重力、寿命、色、生成率を直接指定できます。

```cpp
acs::FEmitterDesc burst = acs::FEmitterDesc::Sparks(impact_position);
burst.active = false;
particles.SetEmitter(burst);
particles.EmitBurst(120u);
```

`active == true` かつ `rate_per_sec > 0` の場合だけ、`Update` が連続生成します。`EmitBurst` は `active` に関係なく即座に生成します。空き容量を超えた粒子は追加しません。

## 失敗条件と制限

- `Init` 前の `Update`、`EmitBurst`、`Render` は何も行いません。
- `Init` は既存プールを先に解放します。再初期化に失敗した場合、以前の粒子は残りません。
- 粒子寿命は生成時に最低 0.05 秒へ補正されます。
- 死亡粒子は末尾との入れ替えで除去されるため、生存粒子の順序は保持しません。
- `Reset` は生存粒子と連続生成の端数だけを消します。プール、エミッタ設定、画像参照は保持します。
- 現在の `CParticleSystem` は `FEmitterDesc::rotation_speed` を更新や描画へ使用しません。回転表現には依存しないでください。
- 粒子の位置と大きさは `CSpriteBatch` と同じ画素座標で扱います。

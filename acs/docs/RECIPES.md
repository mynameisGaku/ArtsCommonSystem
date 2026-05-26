# ACS レシピ集（逆引き）

[`QUICKSTART.md`](QUICKSTART.md) で基本（ウィンドウ・入力・ECS・エラー処理）を
押さえたら、この逆引きレシピ集で各機能の使い方を参照してください。3D 描画・
テクスチャ・アニメーション・シャドウ・パーティクル・多言語対応・セーブデータ
など、ACS の主要 API のコピペ可能なコード例を集めています。

> 以下のコード例の `dev` / `registry` / `renderer` は、`Application` を継承した
> クラスの中で次のように取得した変数です：
> `IRhiDevice* dev = GetRenderer().Device();` ／
> `AssetRegistry& registry = GetAssets();` ／ `Renderer& renderer = GetRenderer();`

関連サンプル: `17_HelloMesh` / `18_HelloTextured`（低レベル RHI）。

---

## キューブを画面に出す最小コード

```cpp
class MyGame : public Application {
    TUniquePtr<IRhiShader>   _vs, _ps;
    TUniquePtr<IRhiBuffer>   _vb, _ib, _cb;
    TUniquePtr<IRhiPipeline> _pipe;
    Camera _cam;
    f32    _angle = 0;

    void OnStart() noexcept override {
        IRhiDevice* dev = GetRenderer().Device();
        // 1. シェーダコンパイル — 17_HelloMesh の HLSL を流用
        // 2. 頂点 / インデックスバッファ作成
        // 3. 定数バッファ（256B 確保しておく）
        // 4. パイプライン: cbuffer_slots=1, depth_test=true, cull=Back
        // 5. カメラ: SetPerspective + SetLookAt
    }

    void OnUpdate(f32 dt) noexcept override {
        _angle += dt;
        FMat4 mvp = FMat4::RotationY(_angle) * _cam.View() * _cam.Projection();
        _cb->Update(&mvp, sizeof(FMat4));
    }

    void OnRender() noexcept override {
        auto* cl = GetRenderer().CommandList();
        cl->SetPipeline(*_pipe);
        cl->SetConstantBuffer(0, *_cb);
        cl->SetVertexBuffer(*_vb, sizeof(Vertex));
        cl->SetIndexBuffer(*_ib);
        cl->DrawIndexed(36);
    }
};
```

## テクスチャを貼る

```cpp
// 手書きピクセルから直接
TextureDesc td{};
td.width = 64; td.height = 64;
td.format = EFormat::R8G8B8A8_UNorm;
td.initial_data = pixels;            // 64*64*4 bytes RGBA
td.initial_data_size = 64*64*4;
// TResult は失敗しうる。Value() を呼ぶ前に必ず IsErr() を確認すること。
auto tex_r = CreateRhiTexture(*dev, td);
if (tex_r.IsErr()) { ACS_LOG_ERROR("テクスチャ作成に失敗"); return; }
auto tex = Move(tex_r.Value());

// またはアセットから（PNG/JPG/BMP/TGA/HDR/...）
auto img_r = registry.Load(L"hero.png");
if (img_r.IsErr()) { ACS_LOG_ERROR("hero.png を読み込めません"); return; }
auto tex2_r = UploadTexture(*dev, *img_r.Value().As<ImageAsset>());
if (tex2_r.IsErr()) { ACS_LOG_ERROR("テクスチャ転送に失敗"); return; }
auto tex2 = Move(tex2_r.Value());

// パイプラインに texture_slots=1 と static_samplers を追加し、
// 描画前に SetTexture(0, *tex)
```

## glTF / OBJ / FBX メッシュをロード

```cpp
auto m_r = registry.Load(L"scene.glb");
if (m_r.IsErr()) { ACS_LOG_ERROR("scene.glb を読み込めません"); return; }
GpuMesh gm;
UploadMesh(*dev, *m_r.Value().As<MeshAsset>(), gm);
// gm.vertex_buffer / gm.index_buffer / gm.index_count を Draw に渡す
```

## 非同期ロード（バックグラウンドスレッドで）

```cpp
AssetFuture fut = registry.LoadAsync(L"big_terrain.glb");
// ...別フレームで...
if (fut.IsReady()) {
    auto r = fut.Get();
    if (r.IsOk()) TRc<Asset> a = r.Value();
}
// または、ブロッキングで完了を待ちつつワーカー支援に参加:
auto r = fut.Wait();
```

## 手続き生成プリミティブ（資源ファイル不要）

```cpp
auto cube_mesh   = Primitive::MakeCube(1.0f);
auto sphere_mesh = Primitive::MakeSphere(0.5f, 32, 16);
auto plane_mesh  = Primitive::MakePlane(10.0f, 10.0f);

GpuMesh gm; UploadMesh(*dev, *cube_mesh, gm);
```

## 2D スプライト描画（SpriteBatch）

ピクセル座標で 2D を描く。複数スプライトを 1 ドローコールにまとめる：

```cpp
SpriteBatch sb;
sb.Init(*renderer.Device(), renderer.ColorFormat());

// 描画フレーム中
sb.Begin(*cl, screen_w, screen_h);
sb.Draw(player_tex, x, y, 32, 32);                       // 32×32 を描く
sb.DrawSub(atlas_tex, 0, 0, 64, 64, 0.0f, 0.0f, 0.5f, 0.5f);  // アトラスの一部
sb.DrawRect(0, 0, screen_w, 32, FVec4{0,0,0,0.5f});       // 半透明バー
sb.End();
```
- 同じテクスチャの連続 Draw は自動でバッチ
- `EBlendMode::AlphaBlend` で透明 PNG が綺麗に出る
- 深度テスト無効（HUD・2D ゲーム想定）

## テキスト描画（TTF + UTF-8 + 漢字）

```cpp
Font font;
// 漢字も使うなら include_cjk=true (アトラスは自動で 2048 へ拡張)
font.LoadFromFile(*renderer.Device(),
                  L"C:\\Windows\\Fonts\\meiryo.ttc",
                  /*pixel_size=*/32.0f,
                  /*atlas_size=*/2048,
                  /*include_cjk=*/true);

sb.Begin(*cl, sw, sh);
sb.DrawString(font, "吾輩は猫である。\n名前はまだ無い。",
            100, 100, FVec4{1, 0.9f, 0.4f, 1});
sb.End();
```
- ASCII / Latin-1 / 平仮名・片仮名 / 全角英数記号 / CJK 統合漢字（U+4E00..U+9FAF）
- `font.MeasureWidth(text)` でピクセル幅を測れる（中央寄せに便利）
- `\n` で改行

## 3D 衝突判定 (`math/Collision3D.h`)

```cpp
Aabb3 box = Aabb3::FromCenterExtents({0, 0, 0}, {1, 1, 1});
FSphere s{ {3, 0, 0}, 0.5f };

if (Intersect(box, s)) { /* 重なってる */ }

Ray3 ray{ camera.Eye(), forward };
RayHit3 h = RaycastAabb(ray, box);
if (h.hit) { /* h.point, h.normal, h.t */ }

FPlane ground = FPlane::FromPointNormal({0,0,0}, {0,1,0});
RayHit3 g = RaycastPlane(ray, ground);
```

## スキンメッシュアニメーション

GPU スキニング（4 ボーン加重）+ ボーンパレット CB:
```cpp
// 1) スキンメッシュアセットを構築（手続きまたは独自パーサ）
auto mesh = MakeRc<SkinnedMeshAsset>();
mesh->Bones().Resize(4);
// ボーン階層を組む（parent / bind_translation / bind_rotation / bind_scale）
mesh->ComputeInverseBindMatrices();
// 頂点は SkinnedVertex（ボーン indices + weights を持つ）
// アニメーションキーを Animation::channels に追加

// 2) GPU アップロード
SkinnedGpuMesh gm;
UploadSkinnedMesh(*device, *mesh, gm);

// 3) シェーダ + プレイヤー
SkinnedShader shd;
shd.Init(*device, color_fmt, depth_fmt);

AnimationPlayer player;
player.SetMesh(mesh.Get());
player.Play(0, /*loop=*/true);

// 毎フレーム
player.Update(dt);
FMat4 palette[64];
u32 nb = player.WritePalette(palette, 64);

shd.SetLights(camera.ViewProjection(), camera.Eye(), lights, count, ambient);
shd.SetObject(model_mat, base_color, specular, shininess);
shd.SetBonePalette(palette, nb);

cl->SetPipeline(*shd.Pipeline());
cl->SetConstantBuffer(0, *shd.PerFrameCB());
cl->SetConstantBuffer(1, *shd.PerObjectCB());
cl->SetConstantBuffer(2, *shd.BonesCB());
cl->SetTexture(0, *shd.DefaultWhiteTexture());
cl->SetVertexBuffer(*gm.vertex_buffer, gm.vertex_stride);
cl->SetIndexBuffer (*gm.index_buffer);
cl->DrawIndexed(gm.index_count);
```
- 最大 64 ボーン、4 ボーン/頂点まで影響
- TRS キーフレームを Slerp/Lerp で時刻補間
- AnimationPlayer がループ再生・一時停止・任意時刻指定
- 15_HelloAnimation サンプルが手続き 4 ボーン円柱でうねうねデモ

## 2D パーティクル

```cpp
ParticleSystem ps;
ps.Init(8192);
ps.SetTexture(my_glow_tex);            // null なら白矩形

// プリセット 4 種
ps.SetEmitter(EmitterDesc::Fire(FVec2{x, y}));
ps.SetEmitter(EmitterDesc::Sparks(FVec2{x, y}));
ps.SetEmitter(EmitterDesc::Fountain(FVec2{x, y}));
ps.SetEmitter(EmitterDesc::Smoke(FVec2{x, y}));

// またはカスタム
EmitterDesc d;
d.position = pos;
d.velocity = FVec2{0, -200};
d.velocity_variance = FVec2{40, 30};
d.gravity = FVec2{0, 200};
d.color_start = FVec4{1, 0.7f, 0.2f, 1};
d.color_end   = FVec4{1, 0.1f, 0,   0};
d.life_seconds = 1.0f;
d.rate_per_sec = 200;
ps.SetEmitter(d);

// 毎フレーム
ps.Update(dt);
sb.Begin(*cl, sw, sh);
ps.Render(sb);                         // バッチに quad を積む
sb.End();

// 単発の爆発
ps.EmitBurst(120);
```
- size / color を寿命 0..1 で線形補間
- gravity 適用、寿命を超えた粒は swap-pop で除去
- 8192 個まで CPU でシミュレーション、GPU 描画は SpriteBatch 経由

## 多言語対応 (i18n)

```cpp
Localization L;
L.LoadFallback(L"lang/en.lang");    // 英語をフォールバック固定
L.LoadActive  (L"lang/ja.lang");    // 起動時の表示言語

sb.DrawString(font, L.Tr("greeting"), x, y, color);
sb.DrawString(font, L.Tr("menu.start"), x, y + 30, color);

// 言語切替
L.LoadActive(L"lang/de.lang");

// 埋め込み版（Bytes）
L.LoadActiveBytes(reinterpret_cast<const u8*>(jp_text), strlen(jp_text));
```
ファイル形式は `Storage` と同じ INI 風 `key=value`：
```ini
greeting=ようこそ、ACS へ
menu.start=ゲーム開始
menu.exit=終了
```
- `Tr(key)` は active → fallback → key 自体 の順で検索
- 部分翻訳 OK（未訳キーは fallback 言語で表示）
- `Localization::Swap()` で active と fallback を入替

## セーブデータ / 設定ファイル

INI 形式の key-value 永続化、AppData 自動解決：
```cpp
Storage cfg;
wchar_t path[260];
// パス取得は失敗しうる（TResult<void>）。エラーなら中断する。
if (Storage::GetAppDataPath(L"MyGame", L"settings.ini", path, 260).IsErr()) return;

cfg.Load(path);                        // 無ければ空のままで成功
cfg.SetInt("high_score", 12345);
cfg.SetFloat("master_volume", 0.8f);
cfg.SetBool("vsync", true);
cfg.SetString("player_name", "タロウ");

i64 score = cfg.GetInt("high_score", 0);
const char* name = cfg.GetString("player_name", "ゲスト");

cfg.Save(path);    // %APPDATA%\MyGame\settings.ini に書き込み
```
- 親ディレクトリは自動作成
- UTF-8 BOM 対応、`#` `;` でコメント
- `Has(key)` / `Remove(key)` / `Clear()` / `Count()` も提供

## 手続き生成スカイ

```cpp
Sky sky;
sky.Init(*renderer.Device(), renderer.ColorFormat(), renderer.DepthFormat());
sky.PresetDay();        // または PresetSunset() / PresetNight()

// シーンの最初に描く
sky.Render(*cl, camera);
// ... StandardShader でメッシュ ...
```
- グラデーション空（地平線→天頂）+ 太陽ディスク + ハロー
- カスタマイズ: `SetSunDirection / SetZenithColor / SetHorizonColor / SetSunRadius`
- StandardShader と整合させるには `sky.SunDirection() / sky.SunColor()` を `DirLight` に渡す

## シャドウマッピング

有向光源の影。depth-only パスを 1 回追加するだけで主シェーダが自動でシャドウサンプル：
```cpp
ShadowMap sm;
sm.Init(*device, /*size=*/2048);

// シェーダにシャドウ機能を有効化（最初に 1 度）
shd.SetShadowMap(sm.DepthTexture(), sm.LightViewProjection(), /*bias=*/0.001f);

// 毎フレーム
sm.SetDirectionalLight(sun_dir, scene_center, scene_radius);

// 1) シャドウパス
cl->BeginShadowPass(*sm.DepthTexture(), 1.0f);
cl->SetPipeline(*sm.CasterPipeline());
cl->SetConstantBuffer(0, *sm.LightCB());
cl->SetConstantBuffer(1, *sm.CasterObjectCB());
for (each caster) {
    sm.SetCaster(model_mat);
    cl->SetVertexBuffer(*gm.vertex_buffer, gm.vertex_stride);
    cl->SetIndexBuffer(*gm.index_buffer);
    cl->DrawIndexed(gm.index_count);
}
cl->EndShadowPass(*sm.DepthTexture());

// 2) 主パス（StandardShader が SetShadowMap で受け取った VP を使う）
shd.SetShadowMap(sm.DepthTexture(), sm.LightViewProjection(), 0.001f);  // 毎フレーム VP 更新
shd.SetLights(...);
cl->SetTexture(0, *albedo);
cl->SetTexture(1, *shd.ShadowTextureOrDefault());
// ... draw scene ...
```
- 4-tap PCF でソフトシャドウ風
- ライト VP 外の領域は自動で「光が当たる」扱い
- ライトが真上のときは UP ベクトル自動切替
- バイアスでシャドウアクネを抑制

## 点光源 (PointLight)

`SetLights` の有向光源と独立に追加できる。減衰は `range` ベースの 2 乗フォールオフ：
```cpp
PointLight pts[3];
pts[0].position = FVec3{ 2, 1.5f, 0};   pts[0].color = FVec3{1.0f, 0.3f, 0.3f}; pts[0].range = 6.0f;
pts[1].position = FVec3{-2, 1.5f, 0};   pts[1].color = FVec3{0.3f, 1.0f, 0.4f}; pts[1].range = 6.0f;
pts[2].position = FVec3{ 0, 1.5f, 3};   pts[2].color = FVec3{0.3f, 0.5f, 1.0f}; pts[2].range = 6.0f;

shd.SetLights(camera.ViewProjection(), camera.Eye(),
              dir_lights, dir_count, ambient);
shd.SetPointLights(pts, 3);     // 最大 4 灯
```
- StandardShader / SkinnedShader 両対応
- range を超えた位置では影響ゼロ（カットオフ）
- range 内は (1 - dist/range)² で滑らかに減衰
- 暗い部屋を 4 色で照らすデモは `samples/12_HelloLights`

## マルチライト + 鏡面反射

```cpp
DirLight lights[2];
lights[0].direction = FVec3{ 0.5f, 0.8f, 0.3f };
lights[0].color     = FVec3{ 1.0f, 0.9f, 0.7f };   // 暖色キーライト
lights[1].direction = FVec3{-0.4f, 0.5f,-0.7f };
lights[1].color     = FVec3{ 0.3f, 0.4f, 0.6f };   // 寒色フィル

shd.SetLights(camera.ViewProjection(), camera.Eye(),
              lights, 2, FVec3{0.08f, 0.10f, 0.14f});

shd.SetObject(model_mat,
              FVec3{1.0f, 0.85f, 0.4f},   // ベース色
              /*specular=*/0.6f,
              /*shininess=*/64.0f);       // 大 = シャープなハイライト
```
- 最大 4 灯
- Blinn-Phong（ハーフベクトル）
- 後方互換: `SetFrame(...)` の単一ライト版もそのまま動作

## 2D 衝突判定 (`math/Collision2D.h`)

```cpp
Aabb2 player = Aabb2::FromTopLeftSize({x, y}, {32, 32});
Aabb2 wall   = Aabb2::FromTopLeftSize({0, 100}, {800, 16});
if (Intersect(player, wall)) { /* 重なってる */ }

FVec2 push;
Circle a{ {ax, ay}, 16 };
Circle b{ {bx, by}, 12 };
if (Resolve(a, b, push)) {
    // a を push 方向へ動かせば衝突解消
    a.center.x += push.x;
    a.center.y += push.y;
}

// レイキャスト
Ray2 ray{ {ox, oy}, {dx, dy} };
RayHit2 h = RaycastAabb(ray, wall);
if (h.hit) { /* h.point, h.normal, h.t */ }
```

## 標準ライティングシェーダで描画

`StandardShader` は Lambert + 環境光 + ベース色 + アルベドテクスチャを自前 HLSL で
セットアップ済み。手書きシェーダ無しに「光が当たった 3D オブジェクト」を出せる。

```cpp
StandardShader shd;
shd.Init(*renderer.Device(), renderer.ColorFormat(), renderer.DepthFormat());

// 毎フレーム
shd.SetFrame(camera.ViewProjection(), camera.Eye(),
             FVec3{-0.5f, 0.8f, 0.3f},   // 光源方向
             FVec3{1, 1, 1},              // 光源色
             FVec3{0.1f, 0.1f, 0.15f});   // 環境光

// 各オブジェクト
shd.SetObject(model_matrix, FVec3{1, 0.85f, 0.4f});  // ベース色

cl->SetPipeline(*shd.Pipeline());
cl->SetConstantBuffer(0, *shd.PerFrameCB());
cl->SetConstantBuffer(1, *shd.PerObjectCB());
cl->SetTexture(0, *shd.DefaultWhiteTexture());  // テクスチャ無しなら白
cl->SetVertexBuffer(*gm.vertex_buffer, gm.vertex_stride);
cl->SetIndexBuffer (*gm.index_buffer);
cl->DrawIndexed(gm.index_count);
```

---

PBR / IBL / Bloom など上級グラフィックスは `samples/23_HelloPbr` 〜
`26_HelloLightmap` を参照してください。

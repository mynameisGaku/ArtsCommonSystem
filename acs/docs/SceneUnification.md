<!-- SPDX-License-Identifier: Apache-2.0 -->
# シーン統一 (AScene) 設計書

Phase 1a / 1b 実装済み、Phase 2 以降は未着手。`AScene2D` / `CScene3D` を単一の `AScene` へ
統一する移行の設計、現状の責務境界、
安全境界、検証手順を記録する。ノード統一の設計と不変条件は
[`NodeUnification.md`](NodeUnification.md) を参照する。本書はその
「シーン自体の統合は次期フェーズ」(NodeUnification.md) を引き取るものである。

**起点 commit**: `d36c7d2` / **Phase 1a**: `1233085` / **Phase 1b**: `bf7ab35`

## 決定事項 (ユーザー確定)

1. **2D/3D シーンを分けない**。単一クラス `AScene` に統一する。ノード統一と同じ方針を
   シーンへ適用する。
2. **2D と 3D の差は投影とサービス構成だけにする**。クラス階層で分けない。
3. 旧 `AScene2D` / `CScene3D` は、再コンパイルする source の互換のために登録済み
   `using` alias を一時的に残す。symbol shim は設けない。
4. 命名規約は [`StyleGuide.md`](StyleGuide.md) §2.1 と [`TypeRoleAudit.md`](TypeRoleAudit.md)
   の役割表に従う。統一シーンは owner に所有され多態的に扱われる object なので `A`。
5. **既存エンジンの構造を参照して設計する**。Unity の `Scene`、Unreal の `UWorld` /
   `ULevel`、Godot の `SceneTree` はいずれもシーン型を 1 本しか持たず、2D/3D の差は
   カメラの投影とコンポーネントで表現する。シーン型を分けない方針はこれに一致する。

## 所有権の分離 (設計判断)

`AScene2D` の中身を丸ごと `AScene` へ移してはならない。`AScene2D` は
**オブジェクト所有と描画リソース所有を 1 クラスに同居させている**ため、そのまま移すと
`Scene.h` が `render/SpriteBatch.h` と RHI へ依存し (現状は foundation / memory /
gameframework のみ)、かつ全シーンが `CSpriteBatch` 3 本と RT 群を抱える。メニューシーンや
headless なテストシーンも同じコストを払い、モーダルを `PushScene` すれば倍加する。

上記 3 エンジンはいずれも、シーンが持つのはオブジェクトツリーだけで、描画リソースは
レンダラ側 (RenderPipeline / FSceneRenderer / RenderingServer) が保持する。これに合わせる。

| 対象 | 移す先 | 根拠 |
|---|---|---|
| root `ANode` ツリー | `AScene` が常に所有 | 全エンジンでシーンの本務。2D/3D で分けない |
| `CNodePool` (`CScene3D` 由来) | `AScene` が常に所有 | 同上。「2D シーンでは確保しない」最適化はしない |
| `CSpriteBatch` × 3 | `CGame` が game 寿命で共有 | 描画リソースはレンダラ側の責務 |
| 反射 RT / 水深度 RT / stencil | レンダラ側で遅延生成 | 使わないシーンがコストを払わない不変条件を維持 |

`CSpriteBatch` の移設先を `CGame` とするのは、ACS 内に同じ前例があるためである。
`FRenderContext::_SetFont` は「`m_Font` は `CGame` が `_BeginFrame` 後に `_SetFont` で
配線する (game 寿命で共有)」と定めており、`_SetSpriteBatch` という seam も既に存在する。
新しい配線経路を追加せず既存の形へ合わせられる。

副次効果として、現行の「`AScene2D` を 1 つ積むごとに `CSpriteBatch` が 3 本増える」構造が
解消される。

## 現状の責務境界 (実測)

統一前の実体は 3 つに割れており、しかも階層が揃っていない。

| 型 | 宣言 | 行数 | 基底 | 役割 |
|---|---|---|---|---|
| `AScene` | `gameframework/Scene.h` | 277 | なし | ライフサイクルフック、`CSceneServices`、World サブシステム束。**描画能力を持たない** |
| `AScene2D` | `gameframework/Scene2D.h` / `.cpp` | 361 / 424 | `AScene` | root `ANode`、`CSpriteBatch`、camera/PPU、反射 RT、水深度 RT、stencil、world/HUD 2 パス描画 |
| `CScene3D` | `gameframework/Scene3D.h` / `.cpp` | 233 / 201 | **なし** | root `ANode`、`CNodePool`、Spawn/Get/Destroy、名前検索、Raycast、Update 伝播。**GPU 非依存** |

### 統合を要する非対称性

- **描画配線が `AScene2D` 専有**。`FRenderContext` へ `CSpriteBatch` を配線するのは
  `AScene2D::OnRender` だけである (`RenderContext.h` の `_SetSpriteBatch` コメントが
  「`AScene2D` または独自ホストが設定する」と明記)。素の `AScene` 派生は
  `rc.HasSprites()` が false のままで、2D 描画が一切できない。
  **これが「`AScene` を直接使えない」実害の本体である。**
- **`CScene3D` は `AScene` を継承していない**。よって `CSceneManager` のスタックに載らず、
  `ChangeScene` / `PushScene` の対象にならない。接頭辞が `C` なのはこの構造の反映であり、
  誤りではない。
- **3D をシーンに載せる経路は既に合成で存在する**。`ALegacyScene3DAdapter : public AScene`
  が `CScene3D m_Graph` を member として持ち、`OnEnter` / `OnExit` / `OnUpdate` /
  `OnFixedUpdate` / `OnRender` から駆動している。統一後の姿はこの合成形を正規化したものになる。
- **root `ANode` を 3 箇所が別々に持つ**。`AScene2D::m_Root`、`CScene3D::m_Root`、
  `ALegacyScene3DAdapter::m_Graph.m_Root`。ノードは既に `ANode` へ統一済みなので、
  ここで分かれている理由は歴史的経緯だけである。

## 目標形

`AScene` が root `ANode` ツリーと描画配線を持ち、2D/3D の違いをカメラ・投影・
`WantedServices()` の差だけにする。

- `AScene2D` の root / `SpriteBatch` / world・HUD パス / `OnDrawWorld` / `OnDrawHud` /
  `OnReady` / `OnTick` / `OnFixedTick` を `AScene` へ引き上げる。
- `CScene3D` の `CNodePool` 管理 (`Spawn` / `Get` / `IsValid` / `IdOf` / `Destroy` /
  `RegisteredCount` / `FindByName` / `Raycast` / `Clear` / `SwapContents`) を `AScene` が
  持つノードグラフとして吸収する。3D レンダラはそのツリーを走査する側に回る。
- 反射 RT / 水深度 RT / stencil の `Ensure*` 群は 2D 専有の重い状態なので、`AScene` 本体
  ではなく遅延生成のままとし、未使用シーンがコストを払わないことを維持する。
- `CScene3D` はノードグラフ型への委譲だけに縮退させる。`AScene2D` の `using` alias 化は
  migration registry の canonical 一意制約に阻まれるため、空派生のまま残す (Phase 3 参照)。

## 影響範囲 (実測)

### `AScene2D` を参照するファイル (25)

engine 内は `Scene2D.cpp` (21件) / `Scene2D.h` (7) / `Forward.h` (4) /
`render/SpriteBatch.h` (4) / `RenderContext.h` (2) / `Sprite2DComponent.h` (2) /
`Spawn2DSubsystem.h` (2、`friend class AScene2D` を含む) / `Scene3D.h` (2) /
`SceneTextLoader.h` (1) / `Scene.h` (1) / `Effects2D.h` (1) /
`ReflectCatalog.cpp` (1) / `render/Dx12/Dx12CommandList.cpp` (1)。

samples は `55_HelloScene2D` / `58_HelloTilemap` / `59_HelloEffects2D` /
`60_HelloStencilMask` / `63_HelloVerticalSlice` (7件)。
tests は `spawn_subsystem_tests` (5) / `reflect_tests` (4) /
`gameframework_forward_header_compile_tests` (3) / `subsystem_tests` (2) /
`subsystem_spawn_header_compile_tests` / `subsystem_canonical_header_tests` /
`component_services_tests`。

### `CScene3D` を参照するファイル (14)

`editor_abi/EditorAbi.cpp`、`CameraComponent3D.h`、`Forward.h`、
`LegacyScene3DAdapter.{h,cpp}`、`Scene3D.{h,cpp}`、`Scene3DSerialize.{h,cpp}`、
`foundation_optimization_wave_k_tests`、`gameframework_forward_header_compile_tests`、
`legacy_scene3d_water_runtime_tests`、`node3d_tests`、`post_effect_quality_tests`。

## 不変条件 (破ってはならない契約)

1. **editor C ABI の export 名を改名しない**。`acs_editor_node3d_*` (30 種以上)、
   `acs_editor_scene_*` は C# editor と native DLL 間の ABI であり、C++ 型名ではない。
   NodeUnification.md が `acs_editor_node_*` について定めた原則をそのまま適用する。
2. **migration registry の契約を維持する**。`scripts/data/cpp_type_role_migrations.json` の
   既存 entry (`acs::game::FScene` → `AScene`、`FScene2D` → `AScene2D`、
   `FScene3D` → `CScene3D`) を書き換える場合は、件数と semantic SHA-256 baseline、
   および `audit_cpp_type_roles.py` の定数を**同じ commit で**更新する。
3. **consumer legacy allowlist を同期する**。`AScene2D` / `CScene3D` を含む行を編集したら
   `scripts/data/cpp_prefix_consumer_legacy_allowlist.json` の file/line identity と
   `audit_cpp_prefix_consumers.py` の `EXPECTED_ALLOWLIST_SHA256` を更新する。
4. **`dist/acs.h` を再生成する**。`scripts/amalgamate.py --write` の後
   `--check` が drift 無しであること。`C:\acs` への配布は
   `scripts/build_single_header.ps1 -Deploy` を使い、手コピーしない。
5. **`Spawn2DSubsystem` の `friend` 関係**を壊さない。生成先ルートの接続は
   シーン初期化成功後にだけ行われる契約である。
6. **遅延生成を維持する**。反射 RT / 水深度 RT / stencil を使わないシーンが GPU リソースを
   確保しないこと。

## 移行フェーズ

各フェーズは isolated clean worktree、別レビュー、Debug/Release 全量ビルド + 全 CTest、
non-force main push を 1 単位とする。

### Phase 1 — 描画配線を `AScene` へ引き上げ

「所有権の分離」に従い、2 段に分ける。

**1a. `CSpriteBatch` を `CGame` へ移す。** `AScene2D` の `m_Sprites` /
`m_SceneSprites` / `m_WaterDepthSprites` と `Ensure*Sprites` を `CGame` へ移し、
`FRenderContext::_SetSpriteBatch` で配線する (`_SetFont` と同じ経路)。RT と stencil も
同時に移す。`Scene.h` へ `render/SpriteBatch.h` を持ち込まないこと。この段だけで
`AScene2D` の描画リソース所有が消え、シーン側は借り物を使う形になる。

**1b. root `ANode` と描画フックを `AScene` へ引き上げる。** `m_Root`、`Root()`、
`OnDrawWorld` / `OnDrawHud`、`OnReady` / `OnTick` / `OnFixedTick`、world/HUD 2 パス、
camera / PPU / `ScreenToWorld` を `AScene` へ移す。`AScene2D` は
`WantedServices()` の既定値だけを変える空派生になる。この時点で
**素の `AScene` 派生から 2D 描画ができる**ようになり、実害が消える。

検証: 既存 samples (`55` / `58` / `59` / `60` / `63`) が無改変で動くこと。
`ACS.UnitTests`、`component_services_tests`、`spawn_subsystem_tests` が緑。
`Scene.h` の include が foundation / memory / gameframework のみであること。

### Phase 1b — 完了 (2026-08-02)

root `ANode` と描画フックを `AScene` へ引き上げ済み。`AScene2D` は `WantedServices()` を
返すだけの空派生になった。`Scene2D.cpp` は `Scene.cpp` へ改名して `AScene` の実装とした。

`WantedServices()` の既定は `ESvc::None` のまま変えず、描画側を service 任意対応にした。
`ViewCenter()` / `ViewZoom()` を追加し、`DrawWorldPass` と `OnRender` の無ガードだった
`Services().Camera()` を `ScreenToWorld` と同じ原点中心・等倍フォールバックへ揃えている。
`OnExit` も `Services().Has(ESvc::Physics2D)` / `Has(ESvc::Tweens)` で個別に確認する。
**`AScene2D` は必ず両方を要求するが `AScene` はそうではない**ため、ここを `HasServices()`
だけで守ると `subsystem_tests` の phase order scene が `_ShutdownAll` 経由で落ちる。

`Spawn2DSubsystem` の `friend` は `AScene` へ移した。`spawn_subsystem_tests` の
`PlainSceneOwnerIsSafe` は分割の帰結を固定していたので `PlainSceneOwnerSpawnsIntoRoot` へ
更新した。未 bind と誤 owner の null 安全は `OrphanOwnerIsSafe` が独立に保つ。

### Phase 2 — ノードグラフを独立型へ切り出す (設計変更あり)

**当初案の「`CScene3D` を `AScene` へ吸収」は不可能であることが実測で判明した。**
`Scene3DSerialize.cpp:1285` (`staged_scene`) と `EditorAbi.cpp:17357` (`staging`) が
**スタック上に一時グラフを構築する**。`CScene3D` を `AScene` 派生または alias にすると、
これらが `CSubsystemCollection` と `TUniquePtr<CSceneServices>` を丸ごと抱えることになる。
該当は `node3d_tests.cpp` 42 箇所、`legacy_scene3d_water_runtime_tests.cpp` 3 箇所、
`foundation_optimization_wave_k_tests.cpp` にも及ぶ。

代わりに **ノードグラフを独立した値型として切り出し、`AScene` がそれを保持する**。

1. `CNodePool` 管理 (`Spawn` / `Get` / `IsValid` / `IdOf` / `Destroy` /
   `RegisteredCount` / `FindByName` / `Raycast` / `Clear` / `SwapContents`) と root 所有を
   `CSceneNodeGraph` (仮称) として切り出す。シーン文脈を一切持たない値型にする。
2. `AScene` は `m_Root` を `CSceneNodeGraph` へ置き換え、既存の `Root()` は graph の root を
   返す薄い委譲にする。
3. `CScene3D` は `CSceneNodeGraph` へ委譲するだけの型に縮退させる。スタック上に置けるという
   性質はここで保たれる。
4. `ALegacyScene3DAdapter` は `m_Graph` を捨て、`AScene` 自身の graph を使う。

注意点。

- **`SwapContents` は root を差し替える**ため、`ANode::_SetSceneServices` /
  `_SetSubsystems` で root に張った配線が落ちる。graph 側に swap 後の再配線 hook を
  持たせるか、`AScene` 側で swap を包む必要がある。
- **tick の意味論が非対称**。`CScene3D::Update` は `UpdateTree` →
  `m_Pool.PurgePendingDestroy()` → `ResolveStructuralChanges` だが、`AScene::OnUpdate` は
  `OnTick` → `UpdateTree` → `ResolveStructuralChanges` で purge が無い。統一するなら
  purge の位置を決めること。
- **root の pool 登録が 2D へ波及する**。`CScene3D` の ctor は root を
  `RegisterExistingNode` するが `AScene` はしない。統一後は全シーンの root が有効な
  `FNodeId` を持つ。`ANode::Id()` の有効性を「pool 登録済みか」の判定に使っている箇所が
  あれば挙動が変わる。
- **`Scene.h` の include 制約**。`Scene3D.h` は `container/StringView.h` と
  `math/Collision3D.h` を直接 include する。`Raycast` の `FRay3` は前方宣言へ落とすこと。
- **C ABI は安全**。`EditorAbi.cpp` の export は全て `void*` handle + プリミティブ引数で、
  `FEditorHost` は当該翻訳単位に閉じている。editor が `scene3d` に対して呼ぶのは
  `Root()` / `Update(f32)` / `Clear()` / `Spawn(FStringView)` / `Raycast(FRay3)` /
  `Get(FNodeId)` の 6 種だけ。内部実装の差し替えは可能。
- editor は独自シリアライザ (`Dfs3DCollect` + `EmitNode3DBlock`) を持つので、
  `Scene3DSerialize` を触っても editor の保存経路は変わらない。

検証: `node3d_tests`、`legacy_scene3d_water_runtime_tests`、`Scene3DSerialize` 系、
`post_effect_quality_tests` が緑。`EditorAbi` の export 一覧を before/after で diff して不変を確認。

### Phase 3 — 旧型の縮退 (alias 化は registry の設計変更が前提)

**`AScene2D` を単純に `using AScene2D = AScene;` にはできない。**
migration registry は canonical 名の一意性を要求する
(`audit_cpp_type_roles.py:385-386`)。`FScene2D` の canonical を `acs::game::AScene` へ
張り替えると、既存の `FScene → acs::game::AScene` と衝突して
`canonical names must be unique` で落ちる。entry を削除して回避すると、

- 互換 alias が `Forward.h` に存在することの要求 (`audit_cpp_type_roles.py:2088-2131`)
- 旧名が製品 source へ再流入していないかの検査 (`_audit_legacy_alias_uses`)
- `EXTERNAL_MANAGED_BASES` の registry からの導出 (同 393-397)

の **3 つが同時に無効化される**。よって Phase 3 は registry schema 側で
「複数 legacy が同一 canonical を指せる」ことを許す変更を先に入れるか、
`AScene2D` を空派生のまま残すかの二択になる。**現状は空派生のまま残す方が安全**であり、
Phase 1b 完了時点で実害 (素の `AScene` で描けない) は既に解消している。

その他の阻害要因。

- `Spawn2DSubsystem.h` の `friend class AScene2D;` は alias 化すると ill-formed
  (elaborated-type-specifier が typedef-name を指す)。Phase 1b で `AScene` へ変更済み。
- allowlist entry は対象 file の**全体 SHA-256** を持つため、
  `gameframework_forward_header_compile_tests.cpp` を 1 byte でも変えるとその file の
  全 entry が無効化される (Scene 関連 11 行ではなく計 96 行)。`--write` モードは無く、
  `_capture_repository_snapshot` から再生成する使い捨てスクリプトが要る。
- `subsystem_canonical_header_tests.cpp` の
  `static_assert(std::is_same_v<acs::FScene2D, acs::AScene2D>)` は恒等 assertion なので
  alias 化しても compile は通るが、canonical が変わると監査の分類器が落とす。
- `acs/docs/**/*.md` と `dist/README.md` はどの監査の走査対象にも入っていない
  (consumer snapshot は `acs/src` / `acs/tests` / `acs/samples` / `acs/scripts` /
  `dist/examples` のみ)。`AScene2D` / `CScene3D` への言及が 65 箇所あり手動更新が要る。

### Phase 4 — 配布と consumer 追随

`dist/acs.h` 再生成、`build_single_header.ps1 -Deploy C:\acs`、
`dist/examples/check.cpp` の smoke。外部 consumer
(`C:\Users\g0190\OneDrive\Desktop\acs_project\cardgame`) の `AScene2D` 参照を `AScene` へ更新。

## 検証コマンド

```powershell
python -B scripts\audit_cpp_type_roles.py --self-test
python -B scripts\audit_cpp_type_roles.py --root src --migration-debt scripts\data\cpp_type_role_migration_debt.json
python -B scripts\audit_cpp_type_roles.py --root tests
python -B scripts\audit_cpp_type_roles.py --root samples
python -B scripts\audit_cpp_prefix_consumers.py --root acs
python -B scripts\audit_cpp_conventions.py --root acs
python -B scripts\audit_reference_type_names.py --root .
python -B scripts\amalgamate.py --check
cmake --build Intermediate\vs --config Debug
cmake --build Intermediate\vs --config Release
ctest --test-dir Intermediate\vs -C Debug   --output-on-failure
ctest --test-dir Intermediate\vs -C Release --output-on-failure
```

## 引き継ぎ時の注意

- **build tree の path 長**。`Intermediate\vs\_deps\acs_diligent_core-build\...\
  Diligent-GraphicsEngineNextGenBase.lastbuildstate` が MAX_PATH (260) を超えると
  MSBuild が `error MSB3501` で落ちる。worktree は短い path (例 `C:\acsw\<name>`) に置く。
- **configure は `acs.ps1` ではなく `generate.ps1` を直接叩くと切り分けやすい**。
  launcher は native command の stderr を失敗と誤判定して `operation failed` を出すことがある。
- **`--scripting` を付けないと `acs_lua_vm_allocation_safety_tests` が生成されない**。
  Lua を含む全量検証では `generate.ps1 -Tests -Diligent -Scripting` を使う。

## Phase 1a 実装後の実測 (2026-08-02)

Phase 1a は実装・検証済み。Debug 65/65、Release 63/63、型役割監査 4 root すべて 0 件、
conventions 1282 file、amalgamation drift なし。新規公開ヘッダを足したので `dist/acs.h` の
再生成が必要だった (469 headers / 108,040 行)。

寿命変更の自己検証結果 (いずれも問題なし)。

- **退場シーンの retire ring と共有バッチ**: バッチがシーンより長生きになったため、ring が
  守っていた「GPU 参照中リソースの破棄」の危険は減る。退場シーンの破棄は共有リソースに触れない。
- **`PushScene` での二重 `Begin`**: `SceneManager.cpp:184-188` の `_Render` は `Top()` の
  1 シーンだけを `OnRender` する。共有バッチへの `Begin` が入れ子になる経路は構造上存在しない。
- **RT の nullptr 参照外し**: `Scene2D.cpp` の 238 / 251 / 279 / 280 は `reflectionReady`
  (= `EnsureSceneRt` 成功) 配下、258 / 270 / 281 は `waterDepthReady` 配下、219 は
  `if (stencil)` で保護されている。
- **破棄順**: `m_SceneRenderResources` は `m_Scenes` より後に宣言されるので先に破棄されるが、
  `~AScene2D()` は `= default` で member は `m_Root` と POD のみ。シーンの破棄が共有リソースへ
  触れる経路は無い。**この順序に依存しているので、`CGame` の member 宣言順を入れ替えないこと。**

## Phase 1b 以降の計画修正 (調査で判明した阻害要因)

以下は engine / samples / tests / gate を全走査して確認した事実であり、上の Phase 1b〜3 の
記述はこれらを反映して読み替えること。**未反映のまま着手すると確実に詰まる。**

### 1. `Scene.cpp` が存在しない

`src/gameframework/Module.cmake` は `Scene.h` だけを列挙する。`OnRender` / `OnEnter` /
`SpriteBatch()` / `ScreenToWorld` / `_OnWorldSubsystemsReady` は `RenderContext.h` /
`Renderer.h` / `Game.h` / `Spawn2DSubsystem.h` を要するため `Scene.h` に書けない。
**新規 `Scene.cpp` の作成と `Module.cmake` への追加が Phase 1b の前提条件。**

### 2. Phase 1b は `spawn_subsystem_tests` を確定的に赤にする

`tests/spawn_subsystem_tests.cpp:65-75` の `ACS_TEST(SpawnSubsystem, PlainSceneOwnerIsSafe)`
は「素の `AScene` を owner にしたら `SpawnPrefabText` が nullptr を返す」を固定している。
`_OnWorldSubsystemsReady` (`Spawner->BindTargetRoot`) を `AScene` へ上げるとこの契約が壊れる。
**Phase 1b の検証項目「`spawn_subsystem_tests` が緑」は成立しない。** テストの意図を
「素の `AScene` でも spawn できる」へ更新するか、bind 条件を `WantedServices()` に紐付けるかを
先に決めること。

### 3. `Services()` が無ガードで、Phase 1b の目的と衝突する

`Scene2D.cpp` の `DrawWorldPass` と `OnRender` は `Services().Camera()` をノーガードで呼ぶ。
`AScene::WantedServices()` の既定は `ESvc::None` (`Scene.h:123`) のままにする方針なので、
そのまま上げると素の `AScene` 派生が `Scene.h:133` の `ACS_ASSERTF` で止まる。
**「素の `AScene` 派生から 2D 描画ができる」という Phase 1b のゴールは、この 2 箇所に
`HasServices()` ガードと camera 既定値を入れない限り達成できない。**

### 4. `CScene3D` を `AScene` の派生にも alias にもできない

`Scene3DSerialize.cpp:1285` と `EditorAbi.cpp:17357` がスタック上に一時グラフを構築する。
`AScene` 化すると `CSubsystemCollection` と `TUniquePtr<CSceneServices>` を丸ごと抱えることに
なり、`node3d_tests.cpp` (42 箇所)、`legacy_scene3d_water_runtime_tests.cpp`、
`foundation_optimization_wave_k_tests.cpp` が該当する。**Phase 2 は「吸収」ではなく、
ノードグラフ部分を独立した型として切り出し `AScene` が保持する形にすること。**
`SwapContents` (`Scene3D.h:83`) は root を差し替えるので、`AScene` 化すると
`_SetSceneServices` / `_SetSubsystems` の root 配線が落ちる点にも注意。

### 5. registry の canonical 一意制約で alias 化が塞がれている

`FScene2D` の canonical を `acs::game::AScene` へ張り替えると、既存の
`FScene → acs::game::AScene` と衝突し `canonical names must be unique`
(`audit_cpp_type_roles.py:385-386`) で落ちる。entry 削除で回避すると、
互換 alias の存在要求 / 旧名再流入の検査 / `EXTERNAL_MANAGED_BASES` の導出という
**3 つの gate が同時に無効化される**。Phase 3 の alias 方針は registry の設計変更込みで
再検討すること。

### 6. `friend class AScene2D;` が alias 化で ill-formed になる

`Spawn2DSubsystem.h:33`。`AScene2D` が `using` alias になると elaborated-type-specifier が
typedef-name を指すことになる。`friend class AScene;` へ変更が必要。

### 7. 影響範囲の追加

上の「影響範囲 (実測)」に **`samples/61_HelloWaterTopDown`** と
**`samples/56_HelloSpriteAnim`** が抜けていた。特に 61 は反射 RT と水深度 RT を実地で踏む
唯一の sample であり、Phase 1a/1b の回帰確認対象に必ず含めること。

### 8. allowlist は file 全体の SHA-256 で固定されている

`audit_cpp_prefix_consumers.py` の allowlist entry は対象 file の**全体ハッシュ**を持つため、
`gameframework_forward_header_compile_tests.cpp` を 1 byte でも変えると、その file の
全 entry が無効化される (Scene 関連 11 行だけでなく計 96 行が該当)。`--write` モードは無いので、
モジュールを import して `_capture_repository_snapshot` から再生成する使い捨てスクリプトが要る。

### 9. docs は完全に無 gate

`acs/docs/**/*.md` と `dist/README.md` はどの監査の走査対象にも入っていない
(consumer snapshot は `acs/src` / `acs/tests` / `acs/samples` / `acs/scripts` / `dist/examples` のみ)。
`AScene2D` / `CScene3D` への言及が 65 箇所あり、放置すると無言で腐る。Phase 3 で手動更新すること。

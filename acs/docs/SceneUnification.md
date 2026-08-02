<!-- SPDX-License-Identifier: Apache-2.0 -->
# シーン統一 (AScene) 設計書

Phase 1〜3 実装済み。**シーン型は `AScene` 一つになった**。2D 専用の `AScene2D` と
3D 専用の `CScene3D` は型ごと削除され、root ANode ツリーと node pool の実体は
シーン文脈を持たない `CSceneNodeGraph` が持つ。2D と 3D の差は投影とサービス構成
(`WantedServices()`) だけである。本書はその移行設計、責務境界、安全境界、検証手順を
記録する。ノード統一の設計と不変条件は
[`NodeUnification.md`](NodeUnification.md) を参照する。本書はその
「シーン自体の統合は次期フェーズ」(NodeUnification.md) を引き取るものである。

**起点 commit**: `d36c7d2` / **Phase 1a**: `1233085` / **Phase 1b**: `bf7ab35` /
**Phase 2**: `ae9468b` / **Phase 3 (型削除)**: 本 commit

## 決定事項 (ユーザー確定)

1. **2D/3D シーンを分けない**。単一クラス `AScene` に統一する。ノード統一と同じ方針を
   シーンへ適用する。
2. **2D と 3D の差は投影とサービス構成だけにする**。クラス階層で分けない。
3. **`AScene2D` / `CScene3D` は残さない (ユーザー指示)**。旧名 `FScene2D` / `FScene3D` の
   互換 alias も含めて削除し、registry からも entry を落とす。`FScene` → `AScene` の
   alias だけは残す。symbol shim は設けない。
4. 命名規約は [`StyleGuide.md`](StyleGuide.md) §2.1 と [`TypeRoleAudit.md`](TypeRoleAudit.md)
   の役割表に従う。統一シーンは owner に所有され多態的に扱われる object なので `A`。
5. **既存エンジンの構造を参照して設計する**。Unity の `Scene`、Unreal の `UWorld` /
   `ULevel`、Godot の `SceneTree` はいずれもシーン型を 1 本しか持たず、2D/3D の差は
   カメラの投影とコンポーネントで表現する。シーン型を分けない方針はこれに一致する。

## 所有権の分離 (設計判断)

Phase 1a 着手前の `AScene2D` の中身を丸ごと `AScene` へ移してはならなかった。当時の
`AScene2D` は **オブジェクト所有と描画リソース所有を 1 クラスに同居させていた**ため、
そのまま移すと
`Scene.h` が `render/SpriteBatch.h` と RHI へ依存し (現状は foundation / memory /
gameframework のみ)、かつ全シーンが `CSpriteBatch` 3 本と RT 群を抱える。メニューシーンや
headless なテストシーンも同じコストを払い、モーダルを `PushScene` すれば倍加する。

上記 3 エンジンはいずれも、シーンが持つのはオブジェクトツリーだけで、描画リソースは
レンダラ側 (RenderPipeline / FSceneRenderer / RenderingServer) が保持する。これに合わせる。

| 対象 | 移す先 | 根拠 |
|---|---|---|
| root `ANode` ツリー | `AScene` が常に所有 | 全エンジンでシーンの本務。2D/3D で分けない |
| `CNodePool` (`CScene3D` 由来) | 文脈非依存の `CSceneNodeGraph` が所有し、`AScene` と `CScene3D` がそれぞれ graph を持つ | 一時 3D graph のスタック所有を保ちつつ、シーン文脈から分離する |
| `CSpriteBatch` × 3 | `CGame` が game 寿命で共有 | 描画リソースはレンダラ側の責務 |
| 反射 RT / 水深度 RT / stencil | レンダラ側で遅延生成 | 使わないシーンがコストを払わない不変条件を維持 |

`CSpriteBatch` の移設先を `CGame` とするのは、ACS 内に同じ前例があるためである。
`FRenderContext::_SetFont` は「`m_Font` は `CGame` が `_BeginFrame` 後に `_SetFont` で
配線する (game 寿命で共有)」と定めており、`_SetSpriteBatch` という seam も既に存在する。
新しい配線経路を追加せず既存の形へ合わせられる。

副次効果として、Phase 1a 前の「`AScene2D` を 1 つ積むごとに `CSpriteBatch` が 3 本増える」
構造は解消された。

## Phase 1 着手前の責務境界 (実測履歴)

Phase 1 着手前の実体は 3 つに割れており、しかも階層が揃っていなかった。以下の表と
非対称性は Phase 1a / 1b の判断根拠を残す履歴であり、現在の class layout を示す表ではない。

| 型 | 宣言 | 行数 | 基底 | 役割 |
|---|---|---|---|---|
| `AScene` | `gameframework/Scene.h` | 277 | なし | ライフサイクルフック、`CSceneServices`、World サブシステム束。**描画能力を持たない** |
| `AScene2D` | `gameframework/Scene2D.h` / `.cpp` | 361 / 424 | `AScene` | root `ANode`、`CSpriteBatch`、camera/PPU、反射 RT、水深度 RT、stencil、world/HUD 2 パス描画 |
| `CScene3D` | `gameframework/Scene3D.h` / `.cpp` | 233 / 201 | **なし** | root `ANode`、`CNodePool`、Spawn/Get/Destroy、名前検索、Raycast、Update 伝播。**GPU 非依存** |

### 統合を要した非対称性 (Phase 1 前の履歴)

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

## 到達形 (実装済み)

`AScene` が root `ANode` ツリーと描画配線を持ち、2D/3D の違いをカメラ・投影・
`WantedServices()` の差だけにする。

- `AScene2D` の root / `SpriteBatch` / world・HUD パス / `OnDrawWorld` / `OnDrawHud` /
  `OnReady` / `OnTick` / `OnFixedTick` を `AScene` へ引き上げる。
- `CScene3D` の `CNodePool` 管理 (`Spawn` / `Get` / `IsValid` / `IdOf` / `Destroy` /
  `RegisteredCount` / `FindByName` / `Raycast` / `Clear` / `SwapContents`) を、シーン文脈を持たず
  スタック所有できる `CSceneNodeGraph` へ切り出す。`AScene` と `CScene3D` はそれぞれ graph を
  保持し、3D レンダラはそのツリーを走査する側に回る。
- 反射 RT / 水深度 RT / stencil の `Ensure*` 群は 2D 専有の重い状態なので、`AScene` 本体
  ではなく遅延生成のままとし、未使用シーンがコストを払わないことを維持する。
- `CScene3D` はノードグラフ型への委譲だけに縮退させる。`AScene2D` の `using` alias 化は
  migration registry の canonical 一意制約に阻まれるため、空派生のまま残す (Phase 3 参照)。

## 削除した型と置き換え先

| 削除した型 | 置き換え | 備考 |
|---|---|---|
| `AScene2D` (`Scene2D.h`) | `AScene` + `WantedServices()` が `kScene2DServices` を返す | 空派生だったので、差はサービス宣言 1 行だけになった |
| `CScene3D` (`Scene3D.h`) | `CSceneNodeGraph` (`SceneNodeGraph.h`) | 委譲 wrapper を畳んだ。スタック所有できる性質は graph が引き継ぐ |
| `FScene2D` / `FScene3D` (互換 alias) | 無し | registry entry ごと削除。旧名が再流入すれば未定義名で compile error になる |

`kScene2DServices` は `Scene.h` の `inline constexpr ESvc`
(`Default2D | Camera2D | Physics2D`)。`AScene::WantedServices()` の既定は `ESvc::None`
のままなので、メニューや headless のシーンは 2D サービスを確保しない。

## 不変条件 (破ってはならない契約)

1. **editor C ABI の export 名を改名しない**。`acs_editor_node3d_*` (30 種以上)、
   `acs_editor_scene_*` は C# editor と native DLL 間の ABI であり、C++ 型名ではない。
   NodeUnification.md が `acs_editor_node_*` について定めた原則をそのまま適用する。
2. **migration registry の契約を維持する**。`scripts/data/cpp_type_role_migrations.json` を
   編集したら、entry 件数 (`DEFAULT_TYPE_ROLE_MIGRATION_ENTRY_COUNT`) と semantic SHA-256
   baseline (`DEFAULT_TYPE_ROLE_MIGRATION_SEMANTIC_SHA256`) を `audit_cpp_type_roles.py` で
   **同じ commit で**更新する。JSON は BOM 無し LF で書く (CRLF だと loader が拒否する)。
3. **consumer legacy allowlist を同期する**。旧名を含む行を編集したら
   `scripts/data/cpp_prefix_consumer_legacy_allowlist.json` の file/line identity と
   `audit_cpp_prefix_consumers.py` の `EXPECTED_ALLOWLIST_SHA256` を更新する。
   allowlist は observation そのものなので、`audit_cpp_prefix_consumers` を import して
   `_capture_repository_snapshot` → `_scan_snapshot` から再生成するのが確実 (`--write` は無い)。
   reason は `(path, legacy, construct)` をキーに旧 entry から引き継げる。
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

### Phase 2 — ノードグラフを独立型へ切り出す — 完了 (`ae9468b`)

**当初案の「`CScene3D` を `AScene` へ吸収」は実測で不可能と判明した。**
`Scene3DSerialize.cpp` (`staged_scene`) と `EditorAbi.cpp` (`staging`) が
**スタック上に一時グラフを構築する**。`CScene3D` を `AScene` 派生または alias にすると、
これらが `CSubsystemCollection` と `TUniquePtr<CSceneServices>` を丸ごと抱えることになる。

代わりに、root ANode ツリーと `CNodePool` を **シーン文脈を持たない `CSceneNodeGraph`**
(`gameframework/SceneNodeGraph.h`) へ切り出し、`AScene` がそれを保持する形にした。
`CScene3D` はこの段では graph への委譲 wrapper に縮退し、Phase 3 で削除した。

実装で確定した設計判断。

- **`SwapContents` は root を差し替える**ので、`_SetSceneServices` / `_SetSubsystems` と
  `ASpawn2DSubsystem::BindTargetRoot` の配線が落ちる。graph に root-swap hook
  (`_SetRootSwapHook`) を持たせ、`AScene::_RewireGraphRoot` が差し替え後の root へ
  配線し直す。graph 自体はシーン文脈を持たないので、hook の中身は owner 側にある。
- **purge の位置を graph 側へ揃えた**。`AScene::_Update` は
  `OnUpdate` → `UpdateTree` → `PurgePendingDestroy` → `ResolveStructuralChanges`。
  旧 `CScene3D::Update` と同じ順序で、reap される前に破棄予定ノードが pool から外れる。
  `_Exit` も `CSceneNodeGraph::ResolveStructuralChanges` を使い同じ順序で後始末する。
- **`ALegacyScene3DAdapter` の `m_Graph` を撤去**し、基底 `AScene` の graph を使う。
  adapter の `OnUpdate` / `OnFixedUpdate` からは手動 tick を外した。基底の
  `_Update` / `_FixedUpdate` が graph を必ず回すので、残すと二重更新になる。
- **root の pool 登録が全シーンへ広がった**。`CSceneNodeGraph` の ctor が root を
  `RegisterExistingNode` するため、2D シーンの root も有効な `FNodeId` を持つ。
  `ANode::Id()` の有効性を「pool 登録済みか」の判定に使う箇所は無く、回帰は出なかった。
- **`Scene.h` の include 制約は維持**。graph の `Raycast` が取る `FRay3` は
  `SceneNodeGraph.h` では前方宣言にとどめ、`math/Collision3D.h` は `.cpp` で include する。
  `Scene3D.h` 経由で `Collision3D.h` が伝播していた consumer
  (`LegacyScene3DAdapter.h` / `node3d_tests`) には明示 include を足した。

### Phase 3 — `AScene2D` / `CScene3D` の削除 — 完了

ユーザー指示 (「Scene2D と Scene3D は無いようにして。消していい」) により、
互換 alias を残さず型ごと削除した。

- **`AScene2D` → `AScene`**。空派生だったので、失われるのは `WantedServices()` の
  既定値だけである。`Scene.h` に `kScene2DServices`
  (`Default2D | Camera2D | Physics2D`) を置き、2D シーンは
  `ESvc WantedServices() const noexcept override { return kScene2DServices; }` を書く。
  **`AScene` の既定は `ESvc::None` のまま変えない**。既定を 2D 側へ寄せると、メニューや
  headless のシーン、3D の adapter シーンまで Camera2D / Physics2D を確保することになる。
- **`CScene3D` → `CSceneNodeGraph`**。Phase 2 の wrapper を畳んだだけで、
  スタック所有・`SwapContents`・`Raycast` などの性質は graph が引き継ぐ。
- **registry から `FScene2D` / `FScene3D` の entry を削除**した。
  当初 Phase 3 の阻害要因だった canonical 一意制約 (`FScene2D` の canonical を
  `AScene` に張り替えると `FScene → AScene` と衝突する) は、**alias を残さない**ことで
  そもそも発生しない。旧名が製品 source へ再流入すれば、alias が無いので未定義名の
  compile error になる。監査の再流入検査より強い保証になっている。
- 併せて `audit_cpp_type_roles.py` の `CANONICAL_EXTERNAL_MANAGED_BASES` から
  `acs::game::AScene2D` を外し、`AScene2D` / `FScene2D` を使っていた self-test fixture を
  `AScene` ベースへ書き換えた。consumer allowlist は observation から再生成し、
  `EXPECTED_ALLOWLIST_SHA256` を更新した。
- reflection 登録は `ACS_REGISTER_SCENE(FScene2D)` → `ACS_REGISTER_SCENE(AScene)`。
  `reflect_tests` の `FindByName` / `Create` も `"AScene"` に合わせた。
- editor の C# テンプレート (`ProjectManager.cs` / `NewClassDialog.xaml.cs` /
  `BlueprintEditor.xaml.cs`) が生成する C++ も `AScene` + `kScene2DServices` にした。
  基底クラス選択肢の `FScene2D` も `AScene` へ置き換えている。

### Phase 4 — 配布と consumer 追随 — 完了

`dist/acs.h` 再生成、`build_single_header.ps1 -Deploy C:\acs`、
`dist/examples/check.cpp` の smoke。外部 consumer
(`C:\Users\g0190\OneDrive\Desktop\acs_project\cardgame`) の `AOutgameScene` は
`AScene2D` 派生だったので、`AScene` 派生 + `WantedServices()` が `kScene2DServices` を
返す形へ更新した。`kScene2DServices` は `Scene.h` で `acs` 名前空間へも再輸出しているので、
`using namespace acs;` だけの consumer でも修飾なしで書ける。

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
- **改行コードを壊さない**。working tree は file ごとに LF / CRLF が混在しており、Python の
  `Path.write_text` は Windows で `\n` を CRLF に変換する。LF の file を round-trip すると
  全行が CRLF になり、**source 文字列を実 file から読んで照合する test**
  (`post_effect_quality_tests` / `water3d_ripple_lifetime_tests` の `ReadWorkspaceSource` 系) が
  「marker が見つからない」で落ちる。git の diff は正規化で小さく見えるため気付きにくい。
  script で書き換えるときは `write_bytes` を使うか、HEAD の blob と CR の有無を突き合わせて戻す。

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
  `AScene2D` は空派生で、`AScene` の破棄経路は root と service だけを解放し、共有 GPU resourceへ
  触れない。将来シーンの destructor から共有描画資源へ触れる場合は、member順とshutdown契約を
  同時に見直すこと。

## Phase 1 着手前の阻害要因 (調査履歴)

以下は engine / samples / tests / gate を全走査して確認した事実である。項目 1〜3 と 6 は
Phase 1b で解消済み、項目 4 と 5 は現在の Phase 2 / 3 設計へ反映済みである。履歴として残すが、
現在の実装状態は上の各フェーズ節を正とする。

### 1. `Scene.cpp` が存在しなかった (Phase 1b で解消済み)

`src/gameframework/Module.cmake` は `Scene.h` だけを列挙する。`OnRender` / `OnEnter` /
`SpriteBatch()` / `ScreenToWorld` / `_OnWorldSubsystemsReady` は `RenderContext.h` /
`Renderer.h` / `Game.h` / `Spawn2DSubsystem.h` を要するため `Scene.h` に書けない。
**新規 `Scene.cpp` の作成と `Module.cmake` への追加が Phase 1b の前提条件。**

### 2. 旧テスト契約では Phase 1b が `spawn_subsystem_tests` を赤にした (解消済み)

`tests/spawn_subsystem_tests.cpp:65-75` の `ACS_TEST(SpawnSubsystem, PlainSceneOwnerIsSafe)`
は「素の `AScene` を owner にしたら `SpawnPrefabText` が nullptr を返す」を固定している。
`_OnWorldSubsystemsReady` (`Spawner->BindTargetRoot`) を `AScene` へ上げるとこの契約が壊れる。
**Phase 1b の検証項目「`spawn_subsystem_tests` が緑」は成立しない。** テストの意図を
「素の `AScene` でも spawn できる」へ更新するか、bind 条件を `WantedServices()` に紐付けるかを
先に決めること。

### 3. `Services()` が無ガードで Phase 1b の目的と衝突した (解消済み)

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

### 6. `friend class AScene2D;` は alias 化で ill-formed になる (Phase 1b で配線解消済み)

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

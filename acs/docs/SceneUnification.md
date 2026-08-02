<!-- SPDX-License-Identifier: Apache-2.0 -->
# シーン統一 (AScene) 設計書

未実装。`AScene2D` / `CScene3D` を単一の `AScene` へ統一する移行の設計、現状の責務境界、
安全境界、検証手順を記録する。ノード統一の設計と不変条件は
[`NodeUnification.md`](NodeUnification.md) を参照する。本書はその
「シーン自体の統合は次期フェーズ」(NodeUnification.md) を引き取るものである。

**起点 commit**: `d36c7d2` (origin/main)

## 決定事項 (ユーザー確定)

1. **2D/3D シーンを分けない**。単一クラス `AScene` に統一する。ノード統一と同じ方針を
   シーンへ適用する。
2. **2D と 3D の差は投影とサービス構成だけにする**。クラス階層で分けない。
3. 旧 `AScene2D` / `CScene3D` は、再コンパイルする source の互換のために登録済み
   `using` alias を一時的に残す。symbol shim は設けない。
4. 命名規約は [`StyleGuide.md`](StyleGuide.md) §2.1 と [`TypeRoleAudit.md`](TypeRoleAudit.md)
   の役割表に従う。統一シーンは owner に所有され多態的に扱われる object なので `A`。

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
- `AScene2D` は `AScene` への `using` alias、`CScene3D` は `AScene` が内包する
  ノードグラフ型へ縮退させる。

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

`AScene` に root `ANode`、`CSpriteBatch`、`OnDrawWorld` / `OnDrawHud`、world/HUD 2 パスを
移す。`AScene2D` は移動後の機能を継承するだけの空派生にする。この時点で
**素の `AScene` 派生から 2D 描画ができる**ようになり、実害が消える。

検証: 既存 samples (`55` / `58` / `59` / `60` / `63`) が無改変で動くこと。
`ACS.UnitTests`、`component_services_tests`、`spawn_subsystem_tests` が緑。

### Phase 2 — ノードグラフを `AScene` へ吸収

`CScene3D` の `CNodePool` 管理 API を `AScene` へ移し、`CScene3D` は移譲のみにする。
`ALegacyScene3DAdapter` は `m_Graph` を捨てて `AScene` 自身のグラフを使う形へ縮退。

検証: `node3d_tests`、`legacy_scene3d_water_runtime_tests`、`Scene3DSerialize` 系、
`post_effect_quality_tests` が緑。`EditorAbi` の export 名が不変であること
(`nm` 相当の比較ではなく、export 一覧を before/after で diff する)。

### Phase 3 — 旧型の縮退と alias 化

`AScene2D` を `AScene` への `using` alias にし、`CScene3D` をノードグラフ型へ縮退。
samples / tests / docs / reference / editor C# の型名を `AScene` へ一括置換する。

検証: `audit_cpp_type_roles.py --root src|samples|tests|tools` が全て 0 件。
`audit_cpp_prefix_consumers.py --root acs` が PASS。`amalgamate.py --check` が
up to date。`audit_reference_type_names.py` が PASS。

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

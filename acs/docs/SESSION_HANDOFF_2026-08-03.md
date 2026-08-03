<!-- SPDX-License-Identifier: Apache-2.0 -->
# ACS — セッション引き継ぎ資料 (2026-08-03)

> 次に入るエージェント (Codex / 別セッションの Claude) が、ゼロから状況を把握して
> 手戻り無く再開するための資料。**§1 と §7 だけは必ず読むこと。**
> 2026-06-03 の [`SESSION_HANDOFF.md`](SESSION_HANDOFF.md) は当時の履歴資料であり、
> ブランチ名も型名も現在とは違う。混同しないこと。

---

## 0. TL;DR

- **origin/main = `aded02c`**。作業ツリーはクリーンで、未 push は無い。
- このセッションの成果は 5 commit。**シーン統合の完了 (Scene2D/Scene3D 削除)**、
  **batch 不要の即時描画**、**シーン遷移の travel context**、
  **コンテナ API の UE 準拠改名**。詳細は §1。
- 全て Debug 65/65 + Release 63/63 の CTest、静的ゲート全数、`C:\acs` への配布、
  外部 consumer (cardgame) のビルドまで通してある。
- **API の破壊的変更が 2 つある** (シーン型の削除、コンテナのメソッド名)。
  古い綴りは残していないので、過去の資料やコード片をそのまま貼ると通らない。§2 と §3 を見ること。
- 次にやる候補は §8。

---

## 1. このセッションの commit (全て push 済み)

| commit | 内容 |
|---|---|
| `ae9468b` | `CSceneNodeGraph` を切り出し、`AScene` が保持。`CScene3D` は委譲 wrapper へ縮退 |
| `83dc9aa` | **`AScene2D` / `CScene3D` を型ごと削除**。`kScene2DServices` を導入 |
| `9bdf9b0` | **batch を持たない即時描画** (`gameframework/Draw.h`) と引数なし描画フック |
| `6e853d7` | **シーン遷移の travel context** (`CSceneTravelContext`) |
| `aded02c` | **コンテナ API を UE 綴りへ改名** (276 file / 3880 箇所) |

起点は codex の `ca139b7` (`codex/prefix-all-integration-20260802` の tip)。
そこから 5 commit を積んで main へ非 force push した。

---

## 2. シーンまわりの現在形 (破壊的変更あり)

**シーン型は `AScene` 一つだけ。** `AScene2D` も `CScene3D` も、互換 alias
`FScene2D` / `FScene3D` も存在しない。旧名を書くと未定義名の compile error になる。

```cpp
class AMyScene final : public AScene {
public:
    // 2D の共通サービス束はこれで要求する (Default2D | Camera2D | Physics2D)。
    ESvc WantedServices() const noexcept override { return kScene2DServices; }

    void OnReady() noexcept override { /* 初期化 */ }

    // 描画は引数なしフック + Draw* で書く (batch を受け取らない)。
    void OnDrawWorld() noexcept override { DrawCircle(0, 0, 0.5f, FVec4{1,1,1,1}); }
    void OnDrawHud()   noexcept override { DrawRect(12, 12, 360, 54, FVec4{0,0,0,0.45f}); }
};
```

- `AScene::WantedServices()` の既定は **`ESvc::None` のまま**。既定を 2D 側へ寄せると、
  メニュー・headless テスト・3D adapter シーンまで Camera2D と Physics2D を確保することになる。
- root `ANode` ツリーと `CNodePool` の実体は `CSceneNodeGraph`
  (`gameframework/SceneNodeGraph.h`)。`AScene::Graph()` で触れる。
  loader / editor はこれをスタック上に構築し `SwapContents` で公開する。
- `SwapContents` は root を差し替えるので、graph に root-swap hook があり
  (`_SetRootSwapHook`)、`AScene::_RewireGraphRoot` が service / subsystem /
  `ASpawn2DSubsystem::BindTargetRoot` を張り直す。**graph を直接差し替える新経路を足すときは
  この hook を通すこと。**
- tick は `AScene::_Update` が `OnUpdate` → `UpdateTree` → `PurgePendingDestroy` →
  `ResolveStructuralChanges`。**シーン側で graph を手動 tick してはいけない** (二重更新になる)。
  `ALegacyScene3DAdapter` は実際にこれを踏んだので手動 tick を外してある。

設計の全体像と、なぜこの形なのかは [`SceneUnification.md`](SceneUnification.md) に全部ある。
Phase 1〜4 の記録と、当初案が実測で潰れた理由もそこ。

---

## 3. コンテナ API の現在形 (破壊的変更あり)

`TArray` / `TInlineArray` / `TSpan` / `THashMap` / `TObservableArray` のメソッド名は
Unreal の綴りに揃えてある。**旧名は残していない。**

| 旧 | 新 | 旧 | 新 |
|---|---|---|---|
| `Size` | `Num` | `Capacity` | `Max` |
| `PushBack` | `Add` | `TryPushBack` | `TryAdd` |
| `EmplaceBack` | `Emplace` | `TryEmplaceBack` | `TryEmplace` |
| `PopBack` | `Pop` | `ShrinkToFit` | `Shrink` |
| `Data` | `GetData` | `Back` | `Last` |
| `Clear` | `Reset` | `ReleaseStorage` | `Empty` |
| `Resize` | `SetNum` | `TryResize` | `TrySetNum` |
| `IndexOf` | `IndexOfByKey` | `IndexOfIf` | `IndexOfByPredicate` |
| `Find` (TArray) | `FindByKey` | `FindIf` | `FindByPredicate` |
| `RemoveFirstSwap` | `RemoveSingleSwap` | `THashMap::Insert` | `Add` |

- `Contains` / `Remove` / `RemoveAt` / `RemoveAtSwap` / `Reserve` は元から UE と同綴りで不変。
- **`THashMap::Find` は残してある** (UE の `TMap::Find` と同義)。`TArray` 側だけ
  `FindByKey` にしたのは、両方に `Find` があると「index が返るのか要素が返るのか」が
  呼び出し側から読めなくなるため。
- **`FString` / `FStringView` は対象外**。`FString::Size()` は今も `Size()` のまま。
  UE の `FString` は `Len` / `Mid` / `Left` など構造ごと違うので、`Size` だけ変えても
  中途半端になると判断した。やるなら §8 の項目として別途。

---

## 4. 今回入った新しい API

### 4.1 即時描画 (`gameframework/Draw.h`)

`AScene::OnRender` がパスの間だけ現在の `FRenderContext` を publish し、free 関数が
そこへ直接積む。`CSpriteBatch` を受け取る必要も `Begin`/`End` も無い。

`DrawRect` / `DrawRectOutline` / `DrawRectRotated` / `DrawCircle` / `DrawCircleOutline` /
`DrawLine` / `DrawTriangle` / `DrawTexture` / `DrawTextureRotated` / `DrawTextureSub` /
`DrawString` / `IsDrawing` / `DrawWidth` / `DrawHeight`。

- 座標単位はパス依存。world パスは world 単位、HUD パスは画面ピクセル。
- **publish が無い / sprite batch 未配線なら全関数 no-op**。3D シーン
  (`ALegacyScene3DAdapter`) は batch を配線しないので、そこで呼んでも落ちない。
  この安全境界は `tests/draw_immediate_tests.cpp` が固定している。
- 円は三角形ファン、線は回転矩形へ落とすので専用テクスチャは要らない。
- `easy` モジュールの同名関数は easy 自身の frame 状態を使う **別実装**。統合していない。

### 4.2 シーン遷移の travel context (`gameframework/SceneTravelContext.h`)

```cpp
class CResultContext final : public CSceneTravelContext {
public:
    ACS_RTTI(CResultContext, CSceneTravelContext)
    i32 score = 0;
};
Scenes().ChangeScene(MakeUnique<AResultScene>(), MakeUnique<CResultContext>());
// 受け側: if (const CResultContext* c = TravelContext<CResultContext>()) { ... }
```

- `ChangeScene` / `PushScene` / `PopScene` / `CGame::TransitionTo` に context 付き overload。
  1 引数版は新 overload へ委譲するだけなので既存コードは無変更で動く。
- 所有権は遷移先のシーンへ移る。**Change/Push は `_Enter` の前、Pop は `OnResume` の前**に
  差し込むので、`OnEnter` / `OnReady` / `OnResume` の時点でもう読める。
- 型違いは `Cast<T>` が nullptr にする。遷移失敗・後勝ち上書き・不成立 pop では破棄される。
- **`CGame::AppState<T>()` との使い分け**: アプリ寿命で持ち続けるものは AppState、
  1 回の遷移で渡すものが travel context。

---

## 5. 検証手順 (この順で全部通ること)

作業 worktree は `C:\acsw\p2` (短い path。理由は §7)。

```powershell
cd C:\acsw\p2\acs
.\generate.ps1 -AllSamples -Tests -Tools -Diligent -Scripting   # test/sample を足したら必須

python -B scripts\amalgamate.py --write
python -B scripts\audit_cpp_conventions.py --root .
python -B scripts\audit_cpp_type_roles.py --self-test
python -B scripts\audit_cpp_type_roles.py --root src --migration-debt scripts\data\cpp_type_role_migration_debt.json
python -B scripts\audit_cpp_type_roles.py --root tests
python -B scripts\audit_cpp_type_roles.py --root samples
python -B scripts\audit_cpp_prefix_consumers.py --root .
python -B scripts\audit_reference_type_names.py --root .

cmake --build Intermediate\vs --config Debug   -- -m -v:quiet -nologo
cmake --build Intermediate\vs --config Release -- -m -v:quiet -nologo
ctest --test-dir Intermediate\vs -C Debug   --output-on-failure   # 65/65
ctest --test-dir Intermediate\vs -C Release --output-on-failure   # 63/63

# 配布と外部 consumer
powershell -ExecutionPolicy Bypass -File acs\scripts\build_single_header.ps1 -Deploy C:\acs
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
  "C:\Users\g0190\OneDrive\Desktop\acs_project\cardgame\cardgame.vcxproj" `
  -p:Configuration=Debug -p:Platform=x64 -m -v:minimal -nologo
```

見た目が変わる変更をしたら、サンプルを起動して**目視**すること
(今回は `Binaries\Debug\hello_scene2d.exe` を撮って grid / 円 / HUD 枠を確認した)。

---

## 6. 便利スクリプト (このセッションで追加)

| script | 用途 |
|---|---|
| `scripts/regen_prefix_consumer_allowlist.py` | consumer allowlist を実測から再生成する。`--dry` あり |
| `scripts/rename_calls_from_msvc_log.py` | MSVC の C2039 から「その型の呼び出しだけ」を改名する |

**allowlist の再生成は今後もほぼ毎回必要になる。** `acs/tests` / `acs/samples` /
`acs/scripts` / `dist/examples` のどれかを 1 byte でも変えると、その file の allowlist entry が
全て無効化されて `ACS.CppPrefixConsumerAudit` が落ちるため。手順は

```powershell
python -B scripts\regen_prefix_consumer_allowlist.py
# 表示された EXPECTED_ALLOWLIST_SHA256 を audit_cpp_prefix_consumers.py の定数へ反映
python -B scripts\audit_cpp_prefix_consumers.py --root .
```

`rename_calls_from_msvc_log.py` は「型定義側だけ先に改名 → build → C2039 が指す
`(file, line, col)` と型名から該当箇所だけ置換 → 再 build」を繰り返す道具。
`Size` / `Data` / `Clear` のように他の型にも同名 API がある場合でも、無関係な型を壊さない。
今回の container 改名はこれで 3880 箇所を移行した。対応表は `--map` で差し替えられる。

---

## 7. 落とし穴 (踏んだものだけ)

1. **改行コードを壊さない (最重要)**。working tree は file ごとに LF と CRLF が混在していて、
   Python の `Path.write_text` は Windows で `\n` を CRLF に変換する。LF の file を
   read → write で round-trip すると全行が CRLF になる。git の diff は正規化されて小さく
   見えるので気付けないが、**source を実 file から読んで文字列照合する test**
   (`post_effect_quality_tests` / `water3d_ripple_lifetime_tests` /
   `atmosphere_composite_tests` の `ReadWorkspaceSource` 系) が「marker が見つからない」で
   落ちる。script で書くときは `write_bytes` を使うか、`git show HEAD:<path>` の blob と
   CR の有無を突き合わせて戻すこと。今回 52 file を戻す羽目になった。
2. **`scripts/data/*.json` は BOM 無し LF 必須**。CRLF だと registry loader が例外を出す。
3. **`Module.cmake` は自動生成物**。`src/*/Module.cmake` を手で編集しないこと。
   file を足したら `dotnet run --project tools/acsbuild -- gen` を回す (再帰収集される)。
4. **test の追加は `tests/CMakeLists.txt` に明示列挙が必要**。glob ではない。
   足したら `generate.ps1` を回さないと CMake に載らない。
5. **baseline 定数が 3 箇所ある**。変更したら同じ commit で更新する。
   - `audit_cpp_type_roles.py`: `DEFAULT_TYPE_ROLE_MIGRATION_ENTRY_COUNT` と
     `DEFAULT_TYPE_ROLE_MIGRATION_SEMANTIC_SHA256` (registry を編集したとき)
   - `audit_cpp_prefix_consumers.py`: `EXPECTED_ALLOWLIST_SHA256`
   - `run_distribution_consumer_smoke.py`: `EXPECTED_EXAMPLE_SHA256`
     (`dist/examples/check.cpp` を変更したとき)
6. **型役割監査の接頭辞**。`A` は `AObject` / `AComponent` / `ANode` / `AScene` から派生する
   管理対象 object だけ。それ以外の具象 class は `C`。今回 `ASceneTravelContext` で弾かれて
   `CSceneTravelContext` に改名した。新しい型を足すときは先に
   [`TypeRoleAudit.md`](TypeRoleAudit.md) を見ること。
7. **PowerShell で native exe に `2>&1` を付けない**。stderr が ErrorRecord に化けて、
   exit 0 でも失敗扱いになる。`generate.ps1` がこれで誤検知した。
   同様に `cmake --build ... -- -v:quiet` は PowerShell だと `-v:quiet` が分断されるので
   `'-v:quiet'` とクォートする。
8. **build tree の path 長**。`Intermediate\vs\_deps\acs_diligent_core-build\...` が
   MAX_PATH (260) を超えると MSBuild が `MSB3501` で落ちる。worktree は `C:\acsw\<短い名前>` に置く。
9. **`acs/bt_serialize_test.btg` は test 実行で書き換わる**。commit に含めないこと。
10. **`acs/docs/REFERENCE.html` は未追跡の生成物**。`generate_reference.py` が吐くが repo には入っていない。

---

## 8. 残作業 / 次の候補

優先度順。上ほど効く。

1. **2D と 3D を 1 シーンで混ぜて描けない**。基底 `AScene::OnRender` (2D パイプライン) と
   `ALegacyScene3DAdapter::OnRender` (3D の PBR 走査) は互いに素で、後者は基底を呼ばない。
   3D シーンにスプライトを付けても `OnDraw` すら呼ばれず、逆に 2D シーンのメッシュは
   `AMeshComponent3D` に `OnDraw` が無いので何も描かれない。**落ちはしないが黙って描かれない。**
   直すなら「3D パスの後に sprite batch を配線して基底の描画パスを走らせる」= 2D オーバーレイ
   の実装で、深度テストの扱いを決める必要がある。これは機能追加であってバグ修正ではない。
2. **`FString` / `FStringView` の UE 準拠**。`Size()` → `Len()` など。§3 の通り今回は見送った。
   やるなら `rename_calls_from_msvc_log.py` に `{"FString": {"Size": "Len"}}` を食わせる。
3. **`easy` モジュールと `Draw.h` の統合**。今は同名関数が 2 実装ある
   (`acs::easy::DrawRect` と `acs::DrawRect`)。easy 側を `Draw.h` の実装へ委譲させれば
   1 本にできるが、easy の `FSprite` ハンドルと frame 状態の扱いを決める必要がある。
4. **`AScene2D` 削除の副産物**: 2D の共通サービスを毎回 `WantedServices()` で書くことになった。
   テンプレートとチュートリアルには入れてあるが、ユーザーが「毎回書くのが面倒」と言い出したら
   `AScene` の既定を変えるのではなく、`kScene2DServices` を返す薄い base を **利用者側に**
   置いてもらう方が安全 (エンジンに空派生を戻すと元の木阿弥)。
5. **`docs/reference/data/*.js` は手書き**。API を足しても自動では載らない。
   `audit_reference_type_names.py` は型名の整合だけ見るので、メソッド名の陳腐化は検出されない。

---

## 9. 環境メモ

- 作業 worktree: `C:\acsw\p2` (branch `claude/phase2-scene-node-graph`、main と同一 commit)。
  用が済んだら `git worktree remove` してよい。**`C:\dev\acs_github` が本体**で、
  そちらは `be3b2b6` の古い状態 + 大量の未コミット変更が残っている (今回は触っていない)。
- 配布先: `C:\acs` (`build_single_header.ps1 -Deploy` で同期。手コピー禁止)。
- 外部 consumer: `C:\Users\g0190\OneDrive\Desktop\acs_project\cardgame`。
  今回 `AScene2D` → `AScene` + `WantedServices()` と、コンテナ API の移行を入れてある。
  **repo 外なので commit には含まれない。** 環境ごと引き継ぐときは注意。
- MSBuild の絶対 path:
  `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe`
  (`msbuild` は PATH に無い)。

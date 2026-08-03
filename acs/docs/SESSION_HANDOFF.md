# ACS — セッション引き継ぎ資料 (2026-06-03)

> **最新の引き継ぎは [`SESSION_HANDOFF_2026-08-03.md`](SESSION_HANDOFF_2026-08-03.md)。**

> **履歴資料:** これは 2026-06-03 時点の作業記録であり、現在のビルド手順・型名・
> テスト件数・ブランチ状態を示す運用資料ではありません。現行手順は
> [`../../README.md`](../../README.md)、[`QUICKSTART.md`](QUICKSTART.md)、
> [`TROUBLESHOOTING.md`](TROUBLESHOOTING.md) を、命名規約とノード統一後の API は
> [`StyleGuide.md`](StyleGuide.md) と [`NodeUnification.md`](NodeUnification.md) を
> 正としてください。以下の commit、件数、旧型名は当時の経緯を保存するために
> 変更せず掲載しています。

> 次の AI セッションがゼロから状況把握し、迷わず作業再開するための超詳細メモ。
> §3〜§8 は実コード(file:line)から複数エージェントで裏取りした検証済みセクション。

---

## 0. 最初に読むこと (TL;DR)

- **ブランチ `claude/phase-33-34-graphics`、HEAD `188aa0e`、全て push 済み**(未push 0、作業ツリーはクリーン)。
- **⚠️ 作業コピーは `C:/dev/acs_github`(OneDrive 外)。** harness の env cwd は死んだ旧 OneDrive パスを指す。**必ず `C:/dev/acs_github` の絶対パスで操作する**(OneDrive 配下は同期で `.git` 破損 → `not a git repository`)。
- 外部未追跡ファイル `AGENTS.md` と `acs/scripts/generate_reference.py` は**コミットに含めない**(env/外部生成物)。コミットはパス指定で。
- このセッションの成果: ① easy ジョブ API ② スマートポインタ改名+拡充 ③ 全機能 HTML リファレンス ④ Pages 準備。詳細は §1。
- **次にやる第一歩は §9。**

---

## 1. このセッションでやったこと(経緯と成果)

新しい順のコミット(全て push 済み、`claude/phase-33-34-graphics`):

| commit | 内容 |
|---|---|
| `188aa0e` | GitHub Pages 用 `.nojekyll`(repo ルート)を追加 |
| `ea22861` | リファレンス foundation/math のセクション題を実体に合わせ修正 |
| `449a0a9` | **ACS 全クラス・全機能の手書き HTML リファレンス `acs/docs/reference/` を追加**(28モジュール/684型/2762メンバ/491用語) |
| `3000c4a` | **スマートポインタ整備: `TRc`→`TSharedPtr` 改名 + `TWeakPtr`/`TSharedFromThis`/`FObject`+`TObjectPtr`/`TWeakObjectPtr` 追加**(tests 10件、acs_unit_tests passed=94) |
| `5e80a6e` | sample 65_HelloJobsVisual: 流れ場モーション + grain 調整でカクつき低減 |
| `1b868d1` | sample 65_HelloJobsVisual(windowed): ジョブ/並列の「見える化」 |
| `87831ca` | **easy 層に初学者向けジョブ/並列 API**(内部は本格 FThreadPool/FJobGraph)。FJobGraph dtor のリーク修正も |

(同一会話の compaction 前に、メモリアロケータ刷新も実施済み: `365e879` シャード TLSF / `a283cb6` Default 結線 / `438cee1` lock-free magazine / `6316648` FRelocatableAllocator。)

### 要点
- **easy ジョブ API**(`87831ca`): `ParallelFor` / `RunAsync`+`WaitJobs`/`JobsDone` / `Job`+`Then`+`RunJobs`。lambda 直渡し・初期化不要・並列不可なら順次フォールバック。sample `64_HelloJobs`(console、`=== ALL PASS ===` 自己検証)で検証済。詳細 §4-b。
- **スマートポインタ**(`3000c4a`): `Rc` が分かりづらいという指摘で `TSharedPtr` に改名し、利用 33 ファイルを新名へ一括更新(`Rc.h` は非推奨互換シムとして残置)。加えて弱参照・オブジェクト系・shared-from-this を新設。`acs_unit_tests` **passed=94 failed=0**、asset/audio/easy/gameframework モジュール + 改名サンプル 6 種のビルド OK。詳細 §4-a。
- **HTML リファレンス**(`449a0a9`): 土台 UI(検索/左ナビ/ホバー用語集/トラブルシュート/ガイド)と書式の手本(`data/memory.js`/`data/_meta.js`)は手書き。残り全モジュールは **workflow で 35 エージェント並列生成**(各自ヘッダを Read → `data/<module>.js` を Write)。私が統合・検証(node 構文 + ランタイムロード)。詳細 §5。
- **GitHub Pages**: `.nojekyll` を準備したが**有効化はユーザー判断で保留**(§2)。

---

## 2. 決定事項・保留

- **GitHub Pages は未有効化(保留)。** この環境に `gh` もトークンも無く、私からトグルを押せない(認証情報を勝手に抜かない方針)。`.nojekyll`(`188aa0e`)で「有効化すれば即動く」状態にはした。有効化手順は §5 末尾。
- **旧 `acs/docs/REFERENCE.html`(`acs/scripts/generate_reference.py` の自動生成物、未追跡)は、手書き `acs/docs/reference/` で置換する方針。** ユーザーは「自動生成じゃない」を明言。旧ファイルは未削除(指示があれば消す)。
- リファレンスの**薄いモジュール(collision/audio/network/event 等)は次セッションで加筆候補**(§5 カバレッジ)。
- 文書ドリフト(次セッションは留意): メモリ記載の `clean.bat` は実体 **`clean-up.bat`** にリネーム済み。`acs/docs/TROUBLESHOOTING.md` の対応表は未だ旧名 `acs::TRc`(更新漏れ)。ビルド経路が 2 系統(VS / Ninja preset)併存(§8 末尾)。

---

## 3. ビルド・実行・テスト手順

### 全体像

ビルドツリーは `C:/dev/acs_github/acs/engine/` に集約され、ソース/アセット(`src` `samples` `tests` `tools`)は 1 つ上の `acs/` 直下にある。`engine/CMakeLists.txt` の `ACS_TREE_ROOT` がその `acs/` を指す。生成物は UE5 風に隔離される(`engine/CMakeLists.txt:37-48`):
- `acs/Binaries/<Config>/` … 実行ファイル + 配布 DLL(`CMAKE_RUNTIME_OUTPUT_DIRECTORY`)
- `acs/Intermediate/vs/` … CMake cache / `.vcxproj` / obj / `.lib` / FetchContent の `_deps`(ビルドツリー `-B`)
- `acs/Saved/` … `generate.log` 等

### ソリューション生成(推奨フロー)

`acs/generate.ps1` を使う。**このファイルは UTF-8 BOM 必須**(先頭バイト `EF BB BF`)。

```powershell
.\generate.ps1                 # 標準: DX12 raw + starter game (55_HelloScene2D)
.\generate.ps1 -Open           # 生成して VS で開く
.\generate.ps1 -Clean          # Intermediate を消してから再生成
.\generate.ps1 -AllSamples     # 全 sample を Game フィルタに追加
.\generate.ps1 -Sample 38_HelloFullGame -Name MyShooter
.\generate.ps1 -Tests -Tools   # tests / tools も Engine 配下に追加
.\generate.ps1 -AllBackends    # Lua/Steamworks/ONNX/OpenXR 等 backend を全 ON
.\generate.ps1 -Diligent       # Diligent RHI backend を追加
```

挙動(`generate.ps1:30-220`):
- 既定: `-Generator "Visual Studio 18 2026"`、`-SolutionName "ACSGame"`、`-Sample "55_HelloScene2D"`、`-StartupProject "hello_scene2d"`。
- 既定では **starter game 1 本のみ**生成(`-DACS_ONLY_SAMPLE=$Sample`)。`-AllSamples` で全サンプル有効化。
- `-Tests`/`-Tools` を付けない限り `ACS_BUILD_TESTS=OFF`/`ACS_BUILD_TOOLS=OFF`。**テストを走らせたいときは `-Tests` 必須。**
- 生成後 `Binaries`/`Intermediate`/`Saved` を hidden 化し、表層に名前付き `.slnx` を 1 つ書く。ログは `acs/Saved/generate.log`。

### 生 CMake コマンド(generate.ps1 を介さない場合)

```powershell
cmake -S acs/engine -B acs/Intermediate/vs -G "Visual Studio 18 2026" `
  -DACS_RENDER_DX12_RAW=ON -DACS_BUILD_SAMPLES=ON `
  -DACS_BUILD_TESTS=ON -DACS_BUILD_TOOLS=ON `
  -DACS_STARTUP_PROJECT=hello_scene2d
cmake --build acs/Intermediate/vs --config Debug   --target hello_scene2d
cmake --build acs/Intermediate/vs --config Release --target acs_unit_tests
```
exe は `acs/Binaries/Debug/` または `acs/Binaries/Release/`。**`-S` は `acs/engine`(`acs/CMakeLists.txt` は無い)。**

主な構成オプション(`engine/CMakeLists.txt:50-103`、既定値): `ACS_BUILD_TESTS=ON` / `ACS_BUILD_SAMPLES=ON` / `ACS_BUILD_TOOLS=ON` / `ACS_ENABLE_ASSERTS=ON` / `ACS_RENDER_DX12_RAW=ON`(既定 backend)/ `ACS_RENDER_DILIGENT=OFF`。backend 群(`ACS_BUILD_STEAMWORKS` `..._SCRIPTING` `..._ML_ONNX` `..._OPENXR` `..._CRASH_REPORTER` `..._TELEMETRY_FILE` `..._LOCAL_MATCHMAKER`)は全て既定 OFF。`ACS_ONLY_SAMPLE`(空=全 sample)。

### MSBuild ターゲット名の規則(snake_case)

- **エンジンモジュール**: target `acs_<module 小文字>`、エイリアス `ACS::<Module>`(`ACSModuleSystem.cmake:136,146-147`)。例 `Foundation`→`acs_foundation`、`Render`→`acs_render`、`GameFramework`→`acs_gameframework`。
- **サンプル**: `samples/<dir>/CMakeLists.txt` の `add_executable(<target> ...)`。例 `55_HelloScene2D`→`hello_scene2d`、`65_HelloJobsVisual`→`hello_jobs_visual`。
- **ユニットテスト**: `acs_unit_tests`(`tests/CMakeLists.txt:1`)。
- **CLI ツール**: `acs_assetpack_cli`(TOOLS=ON + AssetPack 有効時)。**その他**: `acs_lint`(clang-tidy 検出時)、`package`(配布 ZIP)。

### ユニットテスト

自前フレームワーク(GoogleTest 不使用)。`acs/src/test/` の `Test.h`/`Expect.h`/`Test.cpp`、target `ACS::Test`。

```cpp
#include "test/Test.h"
#include "test/Expect.h"
ACS_TEST(SuiteName, CaseName) {
    EXPECT_TRUE(cond); EXPECT_EQ(a, b); EXPECT_NE(a, b);
    EXPECT_NEAR(a, b, eps); EXPECT_FALSE(cond);
}
```
新テスト `.cpp` の登録先: **`acs/tests/CMakeLists.txt`** の `add_executable(acs_unit_tests ...)` ソースリスト(現状 13 本)に追記。新モジュール link は同ファイル `target_link_libraries(acs_unit_tests PRIVATE ...)` に `ACS::<Module>` を追加。

```powershell
cmake --build acs/Intermediate/vs --config Debug --target acs_unit_tests
acs/Binaries/Debug/acs_unit_tests.exe        # 直接実行
ctest --test-dir acs/Intermediate/vs -C Debug
```
実行末尾に `[ACS Test] passed=%u failed=%u`(`Test.cpp:75`)。終了コードは failed==0 で 0。

### 新サンプルの追加手順

1. `acs/samples/<NN>_<Name>/CMakeLists.txt` を作成。
   - **ウィンドウ系(描画する)**は登録を `if(ACS_RENDER_DX12_RAW)` でガード(DX12 raw backend 前提)。書式: `add_executable(<target> [WIN32] <src>)` → `acs_apply_compiler_options(<target>)` → `target_link_libraries(<target> PRIVATE ACS::GameFramework)` 等。
   - **コンソール系(GPU 不要・headless)**はガード不要(例 54/57/62/64)。easy 系は `int main()`(WIN32 なし=別窓コンソールにログ)。
   - ほとんどのゲーム系は `ACS::Game`(10 モジュール集約 INTERFACE)1 行で足りる。`acs_apply_compiler_options(<target>)` は必須。
2. `acs/engine/CMakeLists.txt` の `if(ACS_BUILD_SAMPLES)` ブロック(:174-422)に `acs_add_sample(<NN>_<Name>)` を学習順に追記(ウィンドウ系は `if(ACS_RENDER_DX12_RAW)` で囲む)。再構成して反映。

### generate.ps1 注意点
- **UTF-8 BOM 必須**。既定はテスト/ツール OFF(`-Tests` 必須)。Diligent ON は初回 clone で数分〜十数分。作業コピーは `C:/dev/acs_github`(OneDrive 外)。

---

## 4. 新 API 早見表(スマートポインタ / easy ジョブ)

### a) スマートポインタ群(`acs/src/memory/`、`namespace acs`、原子カウントは `TAtomic`)

**`SharedPtr.h` — TSharedPtr / TWeakPtr / TSharedFromThis(std::shared_ptr 系の代替)**
- `TSharedPtr<T>` 共有所有。コピーで強参照 atomic +1、破棄で -1、0 で対象破棄。`Get/operator*/operator->/bool/IsValid/UseCount/Reset/Swap`、派生→基底アップキャスト変換あり。
- `TWeakPtr<T>` 弱参照(生存を延ばさない)。`TSharedPtr<T>` から暗黙構築。`Expired()/IsValid()/Lock()`(生存時に強参照取得・死亡時空)。
- `TSharedFromThis<T>` enable_shared_from_this 相当。public 継承で `AsShared()/AsWeak()`。**必ず `MakeShared` 経由で生成**(生ポインタ構築では weak-this が仕込まれない)。
- ファクトリ: `MakeShared<T>(args...)`(make_shared 相当の単一確保)/ `MakeSharedIn<T>(alloc, args...)`。
- **循環参照は必ず一方を `TWeakPtr` に**(ControlBlock は strong/weak 2 段、strong=0 で T 破棄・weak=0 でブロック解放)。
```cpp
auto p = MakeShared<Mesh>(args...);
TWeakPtr<Mesh> w = p;
if (auto s = w.Lock()) s->Render();
```

**`ObjectPtr.h` — FObject / TObjectPtr / TWeakObjectPtr / TStrongObjectPtr(UE の UObject 系の代替)**
- `FObject` 参照カウント対象の基底。内部に自分の制御ブロックへの逆ポインタを持ち、**生ポインタ(T*)から強/弱参照を作れる**(TSharedPtr にはできない)。継承は `public FObject`。
- `TObjectPtr<T>` 強参照。`explicit TObjectPtr(T* obj)`(NewObject 由来に限る、`static_assert(IsBaseOfV<FObject,T>)`)。`TStrongObjectPtr<T>` は別名。
- `TWeakObjectPtr<T>` 弱参照。`IsValid()/IsStale()/Get()`(簡易・死亡時 nullptr)/`Pin()`(破棄と競合しても安全に強参照化。**別スレッド破棄あり得る場面は Pin() を使う**)。
- ファクトリ: `NewObject<T>(args...)` / `NewObjectIn<T>(alloc, args...)`。
```cpp
class AEnemy : public FObject { public: void Hit(); };
TObjectPtr<AEnemy> e = NewObject<AEnemy>();
TWeakObjectPtr<AEnemy> w = e;
e.Reset();
if (w.IsValid()) w.Get()->Hit();          // false
TWeakObjectPtr<AEnemy> w2(rawEnemyPtr);    // 生ポインタからの弱参照(FObject 限定の肝)
```

**`UniquePtr.h` — TUniquePtr(std::unique_ptr の代替)** … 単独所有・ムーブのみ。`MakeUnique<T>(args...)`/`MakeUniqueIn`。`Get/operator*/operator->/bool/Release/Reset/GetAllocator`。

**`Rc.h` — 互換シム【非推奨】** … 旧名 `TRc`/`MakeRc`/`MakeRcIn` は `TSharedPtr`/`MakeShared`/`MakeSharedIn` に改名済み。`Rc.h` はエイリアスだけ。**新規コードは新名を直接使う。**

### b) easy ジョブ API(`acs/src/easy/Easy.h`、`namespace acs::easy`)

内部はワークスチール `FThreadPool` + 依存グラフ `FJobGraph`。`JobBatch{u32 id}` / `JobNode{u32 id}` は軽量値型ハンドル。

- **(1) 並列 for(同期):** `ParallelFor(i32 begin, i32 end, [i32 grain,] Fn fn)`。`[begin,end)` を全コアで分担し完了まで待つ。`fn` は i ごとに独立処理のみ。
- **(2) 非同期:** `JobBatch RunAsync(Fn fn)` / `RunAsync(JobBatch, Fn)`(同 batch に追加)/ `WaitJobs(JobBatch)` / `bool JobsDone(JobBatch)`。
- **(3) 依存:** `JobNode Job(Fn fn)` / `Then(JobNode before, JobNode after)` / `RunJobs()`(全ノードを依存順に実行し完了まで待つ)。
- **(4) 情報:** `i32 WorkerCount()` / `bool IsWorker()`。
```cpp
using namespace acs::easy;
ParallelFor(0, n, [&](i32 i){ out[i] = heavy(in[i]); });
auto b = RunAsync([&]{ buildMesh(); }); RunAsync(b, [&]{ loadAudio(); }); WaitJobs(b);
auto a = Job([&]{ stepA(); }); auto c = Job([&]{ stepC(); }); Then(a, c); RunJobs();
```
- **鉄則: ジョブの中身は別スレッド。中で描画系/他の easy 関数を呼ばない(計算だけして結果は捕捉変数へ)。初期化不要**(初回使用で自動起動、並列不可なら順次フォールバック)。

---

## 5. HTML リファレンス(`acs/docs/reference/`)の構造・拡張・カバレッジ

依存ゼロの静的 SPA(ビルド不要、`index.html` をブラウザで開けば動く)。`index.html`(器+配線)/`app.js`(レンダラ)/`styles.css`/`data/*.js`(1ファイル=1モジュール)。

### 仕組み
- `index.html` 冒頭で `window.ACS_REF = { modules:[], glossary:{}, guide:[], troubleshooting:[] }` を宣言 → `data/_meta.js`(ガイド/トラブル/用語集シード、手書き)→ `<!-- DATA-SCRIPTS:START/END -->` 内の各 `data/<module>.js` → 最後に `app.js` の順で読む。
- 各 `data/<module>.js` は `ACS_REF.modules.push({...})` で 1 モジュール追加 + `Object.assign(ACS_REF.glossary, {...})` で用語追加。
- `app.js` の機能: **同 id 統合**(`gameframework_1`〜`_8` は全て `id:"gameframework"` → 1 セクションに統合)/ `order` 昇順並び / 全文検索 / サイドバー絞り込み / ハッシュルーティング `#/<modId>/<typeName>`(`#/guide` `#/trouble` `#/glossary` `#/search/<q>`)/ `<t>用語</t>` ホバー用語集ツールチップ。

### データ形式(手本 `data/memory.js` / `data/_meta.js`)
- モジュール: `{ id, order, title, blurb, types:[...] }`。
- type: `{ name, kind, header, summary, when, sample, members:[...] }`(**type は `name` キー**)。
- member: `{ sig, ret, desc, when, sample }`(**メンバは `sig` キー**)。
- 末尾 `Object.assign(ACS_REF.glossary, { "用語":"定義(HTML可)" })`。
- 記法: 本文の専門用語は `<t>用語</t>`、コード(`sample`)内の `< > &` は `&lt; &gt; &amp;` にエスケープ。`<code>`/`<b>` 可。

### 拡張手順
1. `data/<newmod>.js` を作成(`memory.js` をコピーして雛形に。`id` 一意、`order` は既存の隙間)。
2. `index.html` の `DATA-SCRIPTS:START/END` に `<script src="data/<newmod>.js"></script>` を 1 行追加(物理順は表示順に無関係)。
3. `node --check data/<newmod>.js` で構文確認 → ブラウザで開いてフッタ統計の型数増を確認。
4. 既存加筆は該当 `data/*.js` の `types[]`/`members[]`/glossary を直接編集。

### カバレッジ(全 36 データファイル / 684 型)
gameframework は 8 分割で統合後 **計 346 型**で突出。主要: `render_core`=40, `asset`=30, `easy`=28, `foundation`=22, `threading`=19, `math`=19, `mvvm`=17, `render_backend`=16, `memory`=16, `ui`=15, `platform`/`openxr`=14, `ecs`/`assetpack`=11, `container`/`test`=10。

**薄い/要改善(加筆優先候補)**: `imgui`=1, `audio`=3(サンプル未接続で実発音未検証の caveat), `collision`=3(**実装は 2D/3D コライダー+convex hull+collide-and-slide が在るのに 3 型のみ=最優先**), `mlonnx`/`telemetryfile`=3, `steamworks`/`localmatch`/`crashwin`=4, `network`=5, `event`/`scripting`=8。

### GitHub Pages / .nojekyll
- `.nojekyll` の実配置は **repo ルート `C:/dev/acs_github/.nojekyll`**(commit `188aa0e`)。理由: 既定 Jekyll が `_` 始まり(`data/_meta.js`)を除外 → ガイド/トラブル/用語集シードが配信されなくなる。`.nojekyll` で Jekyll 無効化。
- **Pages は未有効化。** 有効化する場合は Settings → Pages → Source「Deploy from a branch」→ branch `claude/phase-33-34-graphics` / フォルダ `/ (root)`。URL は `https://mynameisgaku.github.io/ArtsCommonSystem/acs/docs/reference/`。private リポジトリなら有料プラン要。

---

## 6. リポジトリ地図 & 規約

### `acs/src` モジュール一覧
- **foundation** — `Result.h`(`TResult<T,E>`/`ACS_TRY`)、`Types.h`(固定幅エイリアス)、`Error`/`Assert`/`Panic`/`Log`/`StackTrace`/`TypeTraits`/`Move`。
- **container** — `TArray`(vector 代替・コピー禁止)、`THashMap`(Robin Hood)、`FString`(UTF-8/SSO22B)、`FStringView`、`TSpan`、`Json`、`Hash`。
- **memory** — `TUniquePtr`/`SharedPtr`/`ObjectPtr`、`FAllocator`/`DefaultAllocator`、TLSF/ShardedTLSF/Arena/Linear/Pool/System/Relocatable、`VirtualMemory`、配置 new。
- **math** — `Vec`/`Mat`/`Quat`/`Camera`、`Collision2D/3D`、SIMD ディスパッチ(`Cpu`/`MathDispatch`)。
- **threading** — `TAtomic`、`Mutex`/`RwLock`/`ScopedLock`/`ConditionVar`、`Thread`、`ThreadPool`、`JobGraph`。
- **platform** — Windows 抽象。`FWindow`/`FEvent`/`FInput`、`FFileSystem`/`FStorage`、`FClock`/`FFrameTimer`、`FLocalization`。
- **event** — `FMessageBroker`(型 pub/sub)、`TMessagePipe`、`FTimerManager`。
- **ecs** — `FWorld`(`Create`/`Add`/`Get`/`Query<...>().Each(...)`)、`FEntityId`/`FComponentRegistry`/`TSparseSet`/`FSystemScheduler`。
- **render** — RHI 抽象 `IRhi*` + backend `Dx12/`(既定)/`Diligent/`、`FRenderer`、各シェーダ/パス(Standard/Pbr/Skinned/Sky/Atmosphere/Ibl/ShadowMap/Ssao/Ssgi/Ssr/HiZ/MotionVector/PostProcess/Particles/SpriteBatch/Font/Light2D/DebugDraw/Blit)。
- **ui** — `FUiRenderer`/`FWidget`/`FStackPanel`/`FContainer`/`FLabel`/`FButton`/`FSlider`/`FCheckbox`/`FTextInput`。**mvvm** — `TObservable`/`TOneWayBinder`/`TTwoWayBinder`/`FCommand`/`FViewModel`(ImGui アダプタは条件付き)。**imgui** — `FImGuiCtx`(DX12 raw 前提)。
- **collision** — `ACS::Collision`。`FSpriteCollider`(α から凸包+輪郭)、`ConvexHull3`。
- **audio** — `FAudioEngine`(XAudio2、最大 64 voice)、`FSoundHandle`。**asset** — `FAsset`/`FAssetId`/`FSkinnedMeshAsset`。**assetpack** — `.acpak`(magic ACPAK、CRC32+任意 AES-256-GCM/LZ4)。
- **easy** — 初学者向け簡単モード(`acs::easy`)。**scripting** — `FLuaVm`(Lua 5.4)。**network** — `FNetwork`/`FTcpConnection`/`FUdpSocket`。**crashwin** — `FWindowsCrashReporter`。**telemetryfile** — `FFileTelemetryBackendClient`。**steamworks**/**mlonnx**/**openxr**/**localmatch** — 各 SDK ブリッジ(Default フォールバック付)。
- **app** — `FApplication`/`FAppConfig`/`EntryPoint`/`Sample`。**gameframework** — `acs::game`(115+ ファイル)。`FScene`/`AScene`/`FGame`/`ANode`/`AComponent`、AI/アニメ/カメラ/ジャンルキット/ランタイム。**test** — `Test`/`Expect`。

### ACS 規約(裏取り済み)
- **エラー処理**: 例外不使用。`TResult<T, E=FErrorCode>`、`IsOk()/IsErr()/Value()/Error()/ValueOr()`、`Ok()/Err()`、`ACS_TRY(expr)`/`ACS_TRY_ASSIGN(name, expr)`。関数は基本 `noexcept`。`Value()`/`Error()` の誤用は `ACS_ASSERT` 停止。
- **STL 不使用 → 対応表**: `vector`→`TArray`(コピー禁止、`Clone` 明示)/ `unordered_map`→`THashMap`/ `string`→`FString`、`string_view`→`FStringView`、`span`→`TSpan`/ `unique_ptr`→`TUniquePtr`、`shared_ptr`→`TSharedPtr`。
- **命名接頭辞**: `T`=テンプレート/値型、`F`=具体クラス/構造体、`A`=`FObject` 所有モデルの node/component、`E`=enum、`I`=インターフェース、`m_`=メンバ、マクロは `ACS_`。
- **ノード/transform 統一**: `ANode` + `AComponent`、親は `TObjectPtr<ANode>` で所有し `NewObject<T>()` で生成、長期参照は `TWeakObjectPtr`。transform は `FTransform3D` 一本で、2D は `Position2D`/`SetRotation2D`/`World2D` helper を使う。
- **2D は Y-down**(左上原点・+X 右・+Y 下・`ANode::Rotation2D` はラジアン。円のみ (x,y)=中心)。
- **名前空間**: `acs` / `acs::easy` / `acs::game`。全ソース先頭 `// SPDX-License-Identifier: Apache-2.0`。

### 重要パス
- 作業コピー `C:/dev/acs_github`(OneDrive 外/同期厳禁)、本体 `C:/dev/acs_github/acs`。
- `acs/Binaries/<Config>`(exe+DLL)、`acs/Intermediate/`(cache/proj/obj/_deps)、`acs/Saved/`(ログ)。全て hidden+gitignore。
- `acs/generate.ps1`(+ `generate.bat`)= ソリューション生成。`clean-up.bat`(+ `.ps1`)= 再生成可能物の掃除。`pack-assets.bat`(+ `.ps1`)= `assets/` → `.acpak`(LZ4、`-Encrypt` で AES-256-GCM)。
- `.slnx`: ルート直下に無し。`acs/ACSGame.slnx` がチェックイン済み代表。実生成物 `acs/Intermediate/vs/ACS.slnx`。

---

## 7. 残作業バックログ(優先度つき)

> 集計元: `acs/src`(TODO/FIXME/HACK/STUB/未実装/未検証/未接続 で 732件/76ファイル)、`acs/samples`(80件/36ファイル)。`ACS_NOT_IMPLEMENTED()` の実発火は 0。`NotImplemented` 系の多くは「設計上の seam が返す安定 subcode」でコード破綻ではない。

### 高優先(機能未接続/見た目未確認、ユーザー価値直結)
- **未スクショの windowed サンプルの実機目視(最重要)。** スクショ証跡は `acs/Saved/` に 55/58/59/63 のみ。WIN32 GUI なのに証跡が無い: **`65_HelloJobsVisual`(今セッション作成、ビルド通るが見た目未確認)**、`56_HelloSpriteAnim`、`60_HelloStencilMask`、`61_HelloWaterTopDown`。alive 止まりの可能性。
- **`FUiLayer` の実ウィジェット root 未確保**(`src/gameframework/UiLayer.cpp:43-56`、`UiLayer.h:37,155` で押下検出未実装の経路)。

### 解消済み/現在の境界
- **`FTextInput` の caret** は `FUiRenderer::MeasureTextBytes` で UTF-8 cursor prefix を測って描画済み。Left/Right/Home/End、Backspace/Delete、checked 挿入、byte 上限も実装済み。現行 pointer API には glyph hit 情報がないため、クリック時は cursor を末尾へ置く。
- **`FHotReloadWatcher` の実 FS 監視**は Windows 開発ビルドで `ReadDirectoryChangesW` + non-blocking `TryTick` として実装済み。bounded queue、debounce、UTF-8 所有、callback 再入防止、overflow/rescan 診断を持つ。アセット再 import・per-type policy・`MessageBroker` publish は watcher 自身では行わず、callback/queue を消費する上位 asset pipeline の接続点として残る。

### 中優先(設計 seam、実 SDK/上位接続待ちの意図的スタブ)
- プラットフォーム SDK 5 ブリッジが seam: `PartySystem.cpp:67-111`、`SocialModeration.cpp:72-115`、`WorkshopBridge.cpp`、`SteamworksBridge.cpp`/`VoiceChat.*`(`kSub*NotImplemented` を返す)。実 SDK 結合は別モジュール委譲方針。
- `FModRegistry` 実 mount/hook 未実装(`ModRegistry.cpp:38-107`)。`FStreamingDirector` registry 未接続時は擬似ロード(`StreamingDirector.cpp:113-190`)。`FAssetBundle` の registry 接続が bridge スケルトン(`AssetBundle.h:33`)。
- OpenXR session/view/action ループ未実装(`src/openxr/KhronosOpenXrBridge.*`、graphics binding 必須のため)。

### 低優先(エディタ将来拡張・最適化・文書 TODO)
- In-game エディタ各パネルの将来拡張 TODO(`src/gameframework/tools/` 多数。`EditorGizmo.cpp:725` rotate trackball、`EditorPanel.h:58` Layout シリアライザ未実装 等)。**エディタ系 windowed サンプル 30〜37 もスクショ証跡なし**。
- `InspectorSeam` の ImGui/UI 接続(`InspectorSeam.cpp:20`)。`FScene`/`FSequence` ライフサイクル細部。`AssetPack` Phase 3+。最適化 TODO(`Pool.h:21` 等)。
- **既知の描画バグ(回避策あり)**: Diligent Tonemap PSO に 4 番目 texture slot を足すと combined sampler が 0(`project_diligent_slot3_issue`)。回避は PbrShader 側 or 専用 composite pass。

### 誤検知(対象外)
- `kSub*NotImplemented` 等 199 ヒットの多くは「安定エラー subcode 定義」。console(headless)サンプル(22/40〜46/48〜51/54/57/62/64)は自己検証ありで目視不要。

---

## 8. 絶対に間違えてはいけない注意点(落とし穴)

- **作業コピーは `C:/dev/acs_github`(OneDrive 外)。** env cwd は死んだ旧 OneDrive パス。tool 呼び出しは必ず `C:/dev/acs_github` 絶対パスで。
- **ビルド検証は「alive」では不可、実機スクショ目視必須。** WIN32 GUI は stdout に何も出さない。alive を通したのに何も描画してなかった事故(sample 59)実在。視覚機能は exe 起動→キャプチャ→PNG を Read して自分の目で確認するまで「完了」と言わない。
- **configure の `-S` は `acs/engine`。`-S acs` は不可**(`acs/CMakeLists.txt` は無い)。
- **新名 `TSharedPtr` を使う。`TRc` は非推奨シム。** (`acs/docs/TROUBLESHOOTING.md:54` の対応表は旧名のまま=更新漏れ注意。)
- **`generate.ps1` は UTF-8 BOM 必須**(PS5.1 が日本語コメントを CP932 誤読 → parse error)。Write/Edit 後は BOM 維持を確認。
- **MSBuild target/exe は snake_case、ディレクトリ名(PascalCase)とずれる。** 例 `65_HelloJobsVisual`→`hello_jobs_visual`。exe は build tree でなく `acs/Binaries/<Config>/<target>.exe`。
- **ジョブの中で描画系/他 easy 関数を呼ばない**(別スレッド、`Easy.h` L330-331)。
- **2D は Y-down。** 「下」(重力/落下/床)は +Y、上昇は -Y。sample 55 が `gravity = FVec2{0,14}`(+Y=画面下)。`{0,-N}` は誤り。シェーダ/レンダラは `-ndc.y` で Y-down 統一済み=触らない。
- **`FSpriteBatch` の view 定数バッファは `kViewRing=32` のリング前提**(view 切替ごとに別スロット)。1 個だと先の DrawIndexed が後で上書きされた view を読んで壊れる(world が HUD view で潰れる)。
- **Diligent tonemap(PostProcess)PSO に 4 番目 texture slot を足さない**(slot3 で Sample が常に 0、Phase 34j SSAO で数時間ハマった既知バグ)。増やすなら PbrShader 側(slot6/7 安定)か専用 composite pass。
- **`Application` フック(`OnStart`/`OnUpdate`/`OnRender`/`OnShutdown`/`OnEvent`)は全て `noexcept`。** override も `noexcept override` 必須(でないと `looser exception specification` でエラー)。
- **STL を入れない(例外無効 `/D_HAS_EXCEPTIONS=0`)。`<vector>` 等は内部 throw で大量エラー。** コンテナはコピー禁止(`b = a;` は deleted、複製は `a.Clone()`)。
- **外部未追跡 `AGENTS.md` / `acs/scripts/generate_reference.py` をコミットに含めない**(パス指定でコミット)。
- **clone は別セッションと共有され予告なく reset/再生成され得る。** 着手前に必ず `git log`/`git status` で HEAD・ブランチ・差分を確認。
- **ビルド経路 2 系統併存**: `generate.ps1`/VS(generator `Visual Studio 18 2026`、出力 `acs/Binaries/<Config>/`)vs `README.md`/`TROUBLESHOOTING.md` の `cmake --preset dx12-debug`(Ninja、出力 `acs/cmake-build-debug/`)。検証時は使った経路の出力先を見る。

---

## 9. 次セッションが最初にやること(推奨順)

1. **状態確認**: `cd C:/dev/acs_github` → `git status` / `git log --oneline -8`。HEAD が `188aa0e`(または以降)・ブランチ `claude/phase-33-34-graphics` を確認。
2. **未スクショ windowed サンプルの実機目視**(§7 高優先)。特に今セッション作成の `65_HelloJobsVisual`、加えて 56/60/61。ユーザーに「VS で起動 or `! acs/Binaries/Release/hello_jobs_visual.exe`」を依頼しスクショ確認 → 崩れていれば修正。
3. **リファレンスの薄いモジュール加筆**(§5 カバレッジ)。最優先は `collision`(実装に対し 3 型のみ)、次に `audio`/`network`/`event`/`imgui`。`data/<module>.js` を直接編集(手本は `data/memory.js`)、または workflow で再生成。
4. (任意・ユーザー判断)**GitHub Pages 有効化**(§5 末尾の手順、`.nojekyll` は準備済)。
5. **ロードマップ継続**はユーザーと方向決め(グラフィック Phase 36-3b+ / GameFramework Phase 2 / スタブ撲滅の続き)。「続けて」で次フェーズ、技術選定は候補+推奨を提示してユーザーに選ばせる流儀。

> メモリ参照: `project_acs_overview` / `project_smart_pointers` / `project_html_reference` / `project_easy_jobs` / `project_phase20_roadmap` / `project_build_workflow` / `project_repo_location` / `feedback_*`。本資料の場所: `acs/docs/SESSION_HANDOFF.md`。

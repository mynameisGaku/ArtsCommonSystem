# シーン遷移・ゲーム構造・セーブ/ロード

`acs::game` でゲーム全体の「画面切替・ポーズ・進行状態・データ保存」を組み立てるための層です。
タイトル→本編→ゲームオーバーのような**画面遷移**、複数理由の**ポーズ管理**、シーンを跨いで生き残る**永続状態**、そして**実ディスクへのセーブ**(`.acssave` ファイル, CRC32 検証付き)を、すべて STL 不使用・例外なし(`TResult`)で扱います。

全体像:

| 層 | クラス | 役割 |
|----|--------|------|
| 画面スタック | `CSceneManager` | `AScene` の push/pop/change。低レベル |
| フェード遷移 | `CGame::TransitionTo` / `CFadeTransition` | 暗転を挟んだ画面切替 |
| 高レベル進行 | `CGameFlow` | Title/Menu/Gameplay… の論理状態マシン |
| ポーズ | `CPauseDirector` | 複数理由の bit mask 管理 |
| 跨ぎ状態 | `CGame::AppState<T>` | シーンを跨いで生きる 1 個の状態 |
| 保存 | `TSaveSlot<T>` / `FSaveArchive` | 実ファイルへの POD 永続化 |

---

## 最小例

`CGame` を継承して最初のシーンを返すだけで動きます。`ACS_GAME_MAIN` がエントリポイントを生成します。

```cpp
#include "gameframework/GameFramework.h"
using namespace acs;
using namespace acs::game;

class ATitleScene final : public AScene {
public:
    void OnUpdate(f32 /*dt*/) noexcept override {
        // Space でゲーム本編へフェード遷移 (暗転中に切替)。
        // ※ 実際の入力取得は CSceneServices::Input() 等で行う (後述)
    }
    void OnRender(FRenderContext& /*rc*/) noexcept override { /* 描画 */ }
};

class CMyGame final : public CGame {
protected:
    TUniquePtr<AScene> InitialScene() noexcept override {
        return MakeUnique<ATitleScene>();   // 最初に push される AScene
    }
};

ACS_GAME_MAIN(CMyGame)
```

シーンの中からは `GetGame()` (`CGame` 参照) と `Scenes()` (`CSceneManager` 参照) に常にアクセスできます。

---

## 主要 API

### CGame (Game.h)

| メンバ | 説明 |
|--------|------|
| `InitialScene()` | **override 必須**。最初に push する `TUniquePtr<AScene>` を返す |
| `Scenes()` | `CSceneManager&` を返す。push/pop/change の入口 |
| `TransitionTo(next, out=0.3f, in=0.3f)` | フェードアウト→暗転中に切替→フェードインを 1 行で |
| `Fade()` | 進行中フェードの `CFadeTransition&`。`IsActive()` で遷移中判定 |
| `EmplaceAppState<T>(args...)` | シーン跨ぎ永続状態を構築(1 個固定) |
| `AppState<T>()` | 取り出し。未設定/型不一致は `nullptr` |
| `SetTimeScale(s)` | `OnUpdate`/`OnFixedUpdate` の dt に乗算(ポーズ=0) |
| `SetFixedTimestep(dt, max=8)` | 固定ステップ長と 1 フレーム最大ステップ数 |

### AScene ライフサイクル (Scene.h) — すべて `noexcept`

| フック | 呼ばれるタイミング |
|--------|------------------|
| `OnEnter()` | 新規 push / change で top に追加された直後。アセット読込はここ |
| `OnExit()` | top から退場する直前(Change/Pop) |
| `OnPause()` / `OnResume()` | 上に Push された / Pop で復帰した時 |
| `OnUpdate(dt)` | 毎フレーム。dt は time_scale 反映済(秒) |
| `OnFixedUpdate(fixed_dt)` | 固定刻み(物理向け) |
| `OnRender(rc)` | 描画。`rc` は `CSpriteBatch` / `FFont` / `IRhiCommandList` を配線する |
| `OnEvent(e)` | 入力/ウィンドウイベント。top のみに届く |

> 注: 上の表は `AScene` のライフサイクル API です。サンプル 58 も同じ `AScene` を使い、2D の共通サービスは `WantedServices()` が `kScene2DServices` を返して要求します。`OnReady()` / `OnTick(dt)` / `OnDrawHud(rc, sb)` も `AScene` 自身が提供します。

### CSceneManager (SceneManager.h)

| メソッド | 説明 |
|----------|------|
| `ChangeScene(next)` | 現 top を pop して `next` を push(= 単純な画面切替) |
| `ChangeScene(next, context)` | 上に加えて、任意データを `next` へ持っていく |
| `PushScene(next)` | 現 top を残して `next` を重ねる(= モーダル/ダイアログ)。旧 top に `OnPause` |
| `PushScene(next, context)` | 上に加えて、任意データを `next` へ持っていく |
| `PopScene()` | top を pop(残り 1 枚以下なら何もせず警告)。新 top に `OnResume` |
| `PopScene(context)` | 上に加えて、戻り先へ任意データ(= モーダルの結果)を渡す |
| `Top()` / `Depth()` / `IsEmpty()` | 状態取得 |

**遷移は即時ではなく「次フレーム頭」で適用**されます。`OnUpdate` の途中でスタックを書き換えても安全。1 フレームに複数要求すると**後勝ち**。退場した `AScene` は GPU が参照中の可能性があるため **3 フレーム保持**してから破棄します。

### CFadeTransition (FadeTransition.h)

`CGame::TransitionTo` を使うなら直接触る必要はありません。状態を読みたい/シーン内で独自フェードしたい時に使います。

| enum / メソッド | 説明 |
|------|------|
| `EFadeKind::{None, FadeIn, FadeOut, FadeInOut, CrossFade}` | フェード種別。FadeInOut が定番の暗転切替 |
| `StartFade(kind, out=0.3f, in=0.3f, mid_pause=0.0f)` | 開始 |
| `Tick(dt)` | 毎フレーム駆動 |
| `IsActive()` / `IsMidPause()` | 遷移中 / 暗転待機中(=切替タイミング) |
| `OverlayAlpha()` / `OverlayColor()` | 描画側が overlay を被せるための alpha[0,1]・色 |

> `CFadeTransition` は **state holder のみ**で自分では描画しません。`mid_pause=0` でも 1 Tick は `IsMidPause()` が true を返すので切替を取りこぼしません。

### CGameFlow (GameFlow.h) — 高レベル進行マシン

`CSceneManager` より 1 段上で「いまゲームのどの段階か」を持ちます。両者は**独立** (`CGameFlow` は `CSceneManager` に依存しない)。

| enum / メソッド | 説明 |
|------|------|
| `EFlowState::{Splash, MainTitle, MainMenu, Settings, Credits, Loading, Gameplay, PauseMenu, GameOver, ExitingGame}` | 10 状態固定 |
| `Init(initial=Splash)` | 10 スロット + 遷移許可テーブル構築。initial の OnEnter 即発火 |
| `RequestTransition(to, fade_sec=0.3f)` | 遷移要求。不正遷移/遷移中の追加要求は no-op |
| `Tick(dt)` | fade timer を進め、enter/exit コールバックを発火 |
| `CurrentState()` / `PendingState()` / `IsTransitioning()` | 状態 query |
| `FadeProgress()` | overlay 不透明度 [0,1] |
| `SetOnEnterCallback(state, cb, user)` / `SetOnExitCallback(...)` | 関数ポインタ + `void* user`(std::function 不使用) |

コールバックは `void(*)(void* user, EFlowState entered_state) noexcept` 型です。

### CPauseDirector (PauseDirector.h) — 複数理由のポーズ

「メニューを閉じたらフォーカス喪失中なのに動き出した」を防ぐため、ポーズ理由を **bit mask** で持ちます。すべての理由が落ちるまでポーズ継続。

| enum / メソッド | 説明 |
|------|------|
| `EPauseReason::{None, UserMenu, SystemMenu, FocusLost, Cinematic, PhotoMode, NetworkSync, Custom1, Custom2}` | bit flag。`\|`/`&` 演算子あり |
| `Pause(reason)` / `Resume(reason)` | bit を立てる / 落とす(複合可) |
| `IsPaused()` | 1 つでも立っていれば true |
| `EffectiveTimeScale()` | ポーズ中=0、非ポーズ=`NormalTimeScale`。**自分で `CGame::SetTimeScale` に渡す** |
| `SetNormalTimeScale(s)` | ポーズ解除時に戻る scale(slow-mo 演出と直交管理) |
| `SetCallback(cb, user)` | bit が立った/落ちた瞬間にだけ発火 |
| `Clear()` | 全 reason 解除(タイトルへ戻る時など) |

列挙子は型名と異なり接頭辞を付けません。設定画面は
`EFlowState::Settings`、フォトモード由来のポーズは
`EPauseReason::PhotoMode` で表します。

### TSaveSlot&lt;T&gt; / FSaveArchive (SaveSlot.h / SaveArchive.h) — **実ファイル保存**

`T` は **trivially-copyable な POD** 限定。`.acssave`(24B ヘッダ + payload + CRC32)を Win32 直叩きで読み書きします。これは stub ではなく**実ディスク I/O**です。

| メソッド | 説明 |
|----------|------|
| `TryInit(const wchar_t* path)` | パスを検証して内部へコピーする推奨 API。`TResult<void>`。失敗時も以前の設定を維持 |
| `Init(const wchar_t* path)` | 互換 API。**ポインタのみ保持・コピーしない**ため、呼び出し側が寿命を保証する |
| `Save(const T& data, version=1)` | `.acssave` 形式で保存。`TResult<void>` |
| `Load(expected_version=1)` | 読み出し。`TResult<T>` |
| `Exists()` | ファイル有無(未初期化なら常に false) |
| `Delete()` | 削除。無ければ成功扱い・べき等。`TResult<void>` |

低レベルが欲しい時は `FSaveArchive::WriteToFile/ReadFromFile/PeekVersion/PeekPayloadSize`(static)を直接使えます。

---

## よく使うパターン

### 1. フェード付きシーン遷移(サンプル 58 そのまま)

`TransitionTo` がフェードアウト→暗転中に切替→フェードインを全部やってくれます。**遷移中は入力を無視**するのが定番です。

```cpp
// AScene の OnTick から (サンプル 58)
void ATitleScene::OnTick(f32 /*dt*/) noexcept {
    if (GetGame().Fade().IsActive()) return;           // 遷移中はガード
    if (Services().Input().IsPressed(kStart)) {
        // 次シーンへ: out 0.3s, in 0.3s
        GetGame().TransitionTo(MakeUnique<ALevelScene>(), 0.3f, 0.3f);
    } else if (Services().Input().IsPressed(kBack)) {
        GetGame().Quit();
    }
}
```

`AScene` 派生なら `OnUpdate` から同じく呼べます。フェード overlay は **CGame が描画する**ので、切替先シーンで重ねてフェードしないこと。

### 2. モーダルを重ねる(ポーズメニュー)

`ChangeScene` ではなく `PushScene` を使うと下のシーンが残り、`PopScene` で戻れます。

```cpp
void AGameplayScene::OnUpdate(f32 dt) noexcept {
    if (Services().Input().IsPressed(kPause)) {
        Scenes().PushScene(MakeUnique<APauseMenuScene>());  // Gameplay は残る → OnPause
    }
}
// APauseMenuScene 側で閉じるとき:
void APauseMenuScene::OnUpdate(f32 dt) noexcept {
    if (Services().Input().IsPressed(kResume)) {
        Scenes().PopScene();                                // Gameplay が OnResume
    }
}
```

### 3. 次のシーンへ任意データを持っていく (travel context)

「リザルト画面へスコアを渡す」「ステージ選択で選んだ番号を渡す」のように、**その遷移でだけ
渡したいデータ**は travel context を使います。`CSceneTravelContext` を継承して
`ACS_RTTI(自分の型, CSceneTravelContext)` を書くだけです。

```cpp
#include "gameframework/SceneTravelContext.h"

class CResultContext final : public acs::CSceneTravelContext {
public:
    ACS_RTTI(CResultContext, acs::CSceneTravelContext)
    acs::i32 score = 0;
    bool cleared = false;
};

// 送る側 (ゲームプレイシーン)
auto ctx = MakeUnique<CResultContext>();
ctx->score   = m_Score;
ctx->cleared = true;
Scenes().ChangeScene(MakeUnique<AResultScene>(), Move(ctx));

// 受け取る側 (リザルトシーン)
void OnReady() noexcept override {
    if (const CResultContext* c = TravelContext<CResultContext>()) {
        m_Score   = c->score;
        m_Cleared = c->cleared;
    }
}
```

- 所有権は遷移先のシーンへ移り、そのシーンが生きている間は何度でも読めます。
- `OnEnter` / `OnReady` の時点で既に読めます(遷移の適用より前に差し込まれます)。
- 型が違えば `TravelContext<T>()` は `nullptr` を返します(`Cast<T>` による安全判定)。
- モーダルの**結果を戻す**場合は `PopScene(context)` を使うと、戻り先が `OnResume` で読めます。
- 遷移が失敗した場合や、適用前に別の遷移要求で上書きされた場合、その context は破棄されます。
- フェード付きの `GetGame().TransitionTo(next, context)` も同じように使えます。

**AppState との使い分け**: ハイスコアやプロファイルのように「アプリが動いている間ずっと
持ち続ける」ものは次の AppState、「この遷移で 1 回だけ渡す」ものは travel context です。

### 4. シーンを跨いで残る状態 (AppState)

ハイスコアやプレイヤープロファイルなど「シーンを切り替えても消えてほしくない」1 個の状態。

```cpp
struct FPlayerProfile { acs::u32 hi_score = 0; };

// 起動時 (CGame::OnStart や最初のシーンの OnEnter で 1 回):
GetGame().EmplaceAppState<FPlayerProfile>();

// 任意のシーンから:
if (auto* prof = GetGame().AppState<FPlayerProfile>()) {
    if (score > prof->hi_score) prof->hi_score = score;
}
```

RTTI 不使用で型 ID を管理するため、`AppState<別の型>()` は安全に `nullptr` を返します。

### 5. ハイスコアを実ファイルに保存・復元(サンプル 38 のラウンドトリップ)

保存対象は POD。`static_assert(__is_trivially_copyable(...))` で型を守ります。

```cpp
// 型 (GameTypes.h)
struct FHighScore { acs::u64 best_score = 0; acs::u64 timestamp = 0; };
static_assert(__is_trivially_copyable(FHighScore), "POD only");

inline constexpr wchar_t kSaveFile[] = L"hello_full_game_highscore.acssave";

// 起動時にロード (FFullGameApp::OnStart)。
// サンプル 38 の Init(kSaveFile) も static 配列なので安全だが、
// 新規コードでは所有コピーする TryInit を基本にする。
auto init = m_HighscoreSlot.TryInit(kSaveFile);
if (init.IsErr()) {
    ACS_LOG_WARN("save slot init failed: %s", init.Error().message);
} else if (m_HighscoreSlot.Exists()) {
    auto r = m_HighscoreSlot.Load();              // TResult<FHighScore>
    if (r.IsOk()) m_Highscore = r.Value();
    else ACS_LOG_WARN("load failed: %s", r.Error().message);
} else {
    ACS_LOG_INFO("first run, no save yet");
}

// ベスト更新時だけ保存 (FFullGameApp::SaveHighScoreIfBetter)
void FFullGameApp::SaveHighScoreIfBetter(u64 final_score) noexcept {
    if (final_score <= m_Highscore.best_score) return;
    m_Highscore.best_score = final_score;
    auto r = m_HighscoreSlot.Save(m_Highscore);   // 実ディスクへ書込
    if (r.IsErr()) ACS_LOG_WARN("save failed: %s", r.Error().message);
}
```

スキーマ(`T` の中身)を変えたら `Save(data, version)` の version を増やすと、旧データ読込時に `Load` が `ESaveArchiveSubCode::kSubMigrationNeeded` を返すので migrate へ分岐できます。

### 6. 高レベル進行を CGameFlow で(サンプル 38)

```cpp
m_Flow.Init(EFlowState::Splash);
m_Flow.RequestTransition(EFlowState::MainTitle, 0.0f);  // fade_sec=0 → 即時
// ... 毎フレーム:
m_Flow.Tick(dt);
if (FInput::IsKeyPressed(EKey::Enter) &&
    m_Flow.CurrentState() == EFlowState::MainTitle) {
    m_Flow.RequestTransition(EFlowState::Gameplay, 0.5f);
}
if (m_Flow.IsTransitioning()) {
    // m_Flow.FadeProgress() を alpha にして overlay を自前で描く
}
```

---

## 注意点 (gotcha)

- **遷移は次フレーム頭で適用**。`Change/Push/Pop` を呼んでも即座には切り替わらない。同フレームに複数要求すると後勝ち。`OnUpdate` 内で安全にスタックを書き換えられるのはこのため。
- **`TransitionTo` の描画は CGame 持ち**。切替先シーンで二重にフェードを描かない。フェードは time_scale の影響を受けず**実時間で進む**(ポーズ中でも遷移は進む)。
- **遷移中は入力をガード**。`if (GetGame().Fade().IsActive()) return;` を入れないと暗転中に二重遷移しがち(サンプル 58 の定番パターン)。
- **`AScene` と `AScene` で利用するフックを選べる**。`AScene` はライフサイクル hook と `OnReady/OnTick/OnDrawHud` の両方を提供し、`AScene` は既定サービス構成だけを追加する。
- **`AScene::OnPause/OnResume` は Push/Pop でのみ呼ばれる**。`ChangeScene` は pause/resume を呼ばない(退場側は `OnExit`、新 top は `OnEnter`)。
- **新規コードは `TSaveSlot::TryInit` を優先**。パスを検証して所有コピーし、空・長すぎるパスや OOM を `TResult<void>` で返します。失敗時は以前のパスを変えません。互換 API の `Init` はコピーしないため、`static` / メンバの `constexpr wchar_t[]` など**寿命がスロット以上の文字列**だけを渡してください。
- **`Save` は atomic replace**。同一ディレクトリの一時ファイルへ完全書込・flush 後に置換するため、途中失敗では既存ファイルを保持する。`Load` 側はサイズ完全一致と CRC32 を検証し、失敗時に呼び出し側の値を変更しない。
- **`TResult` の `Value()`/`Error()` は誤用するとアサートで停止**。必ず `IsOk()`/`IsErr()` で分岐してから読む。
- **`TSaveSlot<T>` の `T` は trivially-copyable 限定**。ポインタは型制約を通っても参照先を永続化できないため、動的配列・文字列・プロセス固有アドレスを含めず、固定レイアウトの値だけを使う。
- **`CPauseDirector` は値を返すだけ**。`EffectiveTimeScale()` を取得して自分で `GetGame().SetTimeScale(...)` を呼ぶ必要がある(モジュールが CGame に依存しない設計)。
- **`CGameFlow` は遷移中の追加要求を無視**(後勝ちしない)。現遷移を完了させてから次を受ける。不正遷移(許可テーブル外)も no-op。
- **`FSettings::Save/Load` は実装済み**(INI 風テキスト、`.tmp`→`MoveFileExW` の atomic write)。`SetI32/GetI32/SetF32/SetBool/SetString/...` の型付き key-value を `Save(L"...ini")` でディスクへ、`Load(L"...ini")` で復元(ファイルが無ければ `Err` → 既定値のまま)。検証 = `62`(round-trip) / `63`(ハイスコアを保存→次回起動でロード)。整数 1〜数個の設定/スコアなら最短、POD をまとめて残すゲームセーブ全般は `TSaveSlot<T>`。

---

## 動くサンプル

- **`acs/samples/58_HelloTilemap/TilemapDemo.cpp`** — `CGame::TransitionTo` による FadeInOut 遷移(Title ⇄ Level)。`GetGame().Fade().IsActive()` での入力ガード。`AScene` 派生・`OnReady/OnTick/OnDrawHud`。
- **`acs/samples/38_HelloFullGame/`** — フルゲーム構成。`TSaveSlot<FHighScore>` の実ファイル・ラウンドトリップ(`FullGameApp.cpp` の `OnStart` でロード / `SaveHighScoreIfBetter` で保存)、`CGameFlow` の進行管理、`AScene` 派生の `ChangeScene`(`GameplayScene.cpp` → `GameOverScene.cpp` → `TitleScene.cpp`)。POD 定義は `GameTypes.h`。
- **`acs/samples/63_HelloVerticalSlice/main.cpp`** — **縦スライスの完結例**。`AScene` 派生の Title/Play/GameOver を `ChangeScene` で遷移、Pause は Play 内 state でゲームを背後に凍結。`FSettings` でハイスコアを INI 保存→次回起動でロード(`OnStart` で `Load`、`AGameOverScene::OnReady` で `SubmitScore`)。UI(`FUiLayer`)・atlas(`FSpritePack`)・tilemap・collide-and-slide(OBB 含む)を 1 本に統合。全 Y-down。
- ヘッダ実物: `acs/src/gameframework/Game.h` / `SceneManager.h` / `Scene.h` /
  `FadeTransition.h` / `GameFlow.h` / `PauseDirector.h` / `AppState.h` /
  `SaveSlot.h` / `SaveArchive.h`

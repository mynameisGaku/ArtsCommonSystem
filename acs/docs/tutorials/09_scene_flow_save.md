# シーン遷移・ゲーム構造・セーブ/ロード

`acs::game` でゲーム全体の「画面切替・ポーズ・進行状態・データ保存」を組み立てるための層です。
タイトル→本編→ゲームオーバーのような**画面遷移**、複数理由の**ポーズ管理**、シーンを跨いで生き残る**永続状態**、そして**実ディスクへのセーブ**(`.acssave` ファイル, CRC32 検証付き)を、すべて STL 不使用・例外なし(`TResult`)で扱います。

全体像:

| 層 | クラス | 役割 |
|----|--------|------|
| 画面スタック | `FSceneManager` | `Scene` の push/pop/change。低レベル |
| フェード遷移 | `FGame::TransitionTo` / `FFadeTransition` | 暗転を挟んだ画面切替 |
| 高レベル進行 | `FGameFlow` | Title/Menu/Gameplay… の論理状態マシン |
| ポーズ | `FPauseDirector` | 複数理由の bit mask 管理 |
| 跨ぎ状態 | `FGame::AppState<T>` | シーンを跨いで生きる 1 個の状態 |
| 保存 | `FSaveSlot<T>` / `FSaveArchive` | 実ファイルへの POD 永続化 |

---

## 最小例

`FGame` を継承して最初のシーンを返すだけで動きます。`ACS_GAME_MAIN` がエントリポイントを生成します。

```cpp
#include "gameframework/GameFramework.h"
using namespace acs;
using namespace acs::game;

class TitleScene final : public Scene {
public:
    void OnUpdate(f32 /*dt*/) noexcept override {
        // Space でゲーム本編へフェード遷移 (暗転中に切替)。
        // ※ 実際の入力取得は FSceneServices::Input() 等で行う (後述)
    }
    void OnRender(RenderContext& /*rc*/) noexcept override { /* 描画 */ }
};

class MyGame final : public FGame {
protected:
    TUniquePtr<Scene> InitialScene() noexcept override {
        return MakeUnique<TitleScene>();   // 最初に push される Scene
    }
};

ACS_GAME_MAIN(MyGame)
```

シーンの中からは `GetGame()`(FGame 参照)と `Scenes()`(FSceneManager 参照)に常にアクセスできます。

---

## 主要 API

### FGame (Game.h)

| メンバ | 説明 |
|--------|------|
| `InitialScene()` | **override 必須**。最初に push する `TUniquePtr<Scene>` を返す |
| `Scenes()` | `FSceneManager&` を返す。push/pop/change の入口 |
| `TransitionTo(next, out=0.3f, in=0.3f)` | フェードアウト→暗転中に切替→フェードインを 1 行で |
| `Fade()` | 進行中フェードの `FFadeTransition&`。`IsActive()` で遷移中判定 |
| `EmplaceAppState<T>(args...)` | シーン跨ぎ永続状態を構築(1 個固定) |
| `AppState<T>()` | 取り出し。未設定/型不一致は `nullptr` |
| `SetTimeScale(s)` | `OnUpdate`/`OnFixedUpdate` の dt に乗算(ポーズ=0) |
| `SetFixedTimestep(dt, max=8)` | 固定ステップ長と 1 フレーム最大ステップ数 |

### Scene ライフサイクル (Scene.h) — すべて `noexcept`

| フック | 呼ばれるタイミング |
|--------|------------------|
| `OnEnter()` | top に来た直後(新規 push / pop 復帰の両方)。アセット読込はここ |
| `OnExit()` | top から退場する直前(Change/Pop) |
| `OnPause()` / `OnResume()` | 上に Push された / Pop で復帰した時 |
| `OnUpdate(dt)` | 毎フレーム。dt は time_scale 反映済(秒) |
| `OnFixedUpdate(fixed_dt)` | 固定刻み(物理向け) |
| `OnRender(rc)` | 描画。`rc` は SpriteBatch/Font/CmdList を持つ |
| `OnEvent(e)` | 入力/ウィンドウイベント。top のみに届く |

> 注: 上の表は素の `Scene` の API です。サンプル 58 が使う `FScene2D` は `Scene` 派生で、フックが `OnReady()`/`OnTick(dt)`/`OnDrawHud(rc, sb)` という別名・別粒度になります。どちらを継承するかでフック名が変わる点に注意。

### FSceneManager (SceneManager.h)

| メソッド | 説明 |
|----------|------|
| `ChangeScene(next)` | 現 top を pop して `next` を push(= 単純な画面切替) |
| `PushScene(next)` | 現 top を残して `next` を重ねる(= モーダル/ダイアログ)。旧 top に `OnPause` |
| `PopScene()` | top を pop(残り 1 枚以下なら何もせず警告)。新 top に `OnResume` |
| `Top()` / `Depth()` / `IsEmpty()` | 状態取得 |

**遷移は即時ではなく「次フレーム頭」で適用**されます。`OnUpdate` の途中でスタックを書き換えても安全。1 フレームに複数要求すると**後勝ち**。退場した Scene は GPU が参照中の可能性があるため **3 フレーム保持**してから破棄します。

### FFadeTransition (FadeTransition.h)

`FGame::TransitionTo` を使うなら直接触る必要はありません。状態を読みたい/シーン内で独自フェードしたい時に使います。

| enum / メソッド | 説明 |
|------|------|
| `EFadeKind::{FadeIn, FadeOut, FadeInOut, CrossFade}` | フェード種別。FadeInOut が定番の暗転切替 |
| `StartFade(kind, out=0.3f, in=0.3f, mid_pause=0.0f)` | 開始 |
| `Tick(dt)` | 毎フレーム駆動 |
| `IsActive()` / `IsMidPause()` | 遷移中 / 暗転待機中(=切替タイミング) |
| `OverlayAlpha()` / `OverlayColor()` | 描画側が overlay を被せるための alpha[0,1]・色 |

> `FFadeTransition` は **state holder のみ**で自分では描画しません。`mid_pause=0` でも 1 Tick は `IsMidPause()` が true を返すので切替を取りこぼしません。

### FGameFlow (GameFlow.h) — 高レベル進行マシン

`FSceneManager` より 1 段上で「いまゲームのどの段階か」を持ちます。両者は**独立**(FGameFlow は SceneManager に依存しない)。

| enum / メソッド | 説明 |
|------|------|
| `EFlowState::{Splash, MainTitle, MainMenu, FSettings, Credits, Loading, Gameplay, PauseMenu, GameOver, ExitingGame}` | 10 状態固定 |
| `Init(initial=Splash)` | 10 スロット + 遷移許可テーブル構築。initial の OnEnter 即発火 |
| `RequestTransition(to, fade_sec=0.3f)` | 遷移要求。不正遷移/遷移中の追加要求は no-op |
| `Tick(dt)` | fade timer を進め、enter/exit コールバックを発火 |
| `CurrentState()` / `PendingState()` / `IsTransitioning()` | 状態 query |
| `FadeProgress()` | overlay 不透明度 [0,1] |
| `SetOnEnterCallback(state, cb, user)` / `SetOnExitCallback(...)` | 関数ポインタ + `void* user`(std::function 不使用) |

コールバックは `void(*)(void* user, EFlowState entered_state) noexcept` 型です。

### FPauseDirector (PauseDirector.h) — 複数理由のポーズ

「メニューを閉じたらフォーカス喪失中なのに動き出した」を防ぐため、ポーズ理由を **bit mask** で持ちます。すべての理由が落ちるまでポーズ継続。

| enum / メソッド | 説明 |
|------|------|
| `EPauseReason::{None, UserMenu, SystemMenu, FocusLost, Cinematic, FPhotoMode, NetworkSync, Custom1, Custom2}` | bit flag。`\|`/`&` 演算子あり |
| `Pause(reason)` / `Resume(reason)` | bit を立てる / 落とす(複合可) |
| `IsPaused()` | 1 つでも立っていれば true |
| `EffectiveTimeScale()` | ポーズ中=0、非ポーズ=`NormalTimeScale`。**自分で `FGame::SetTimeScale` に渡す** |
| `SetNormalTimeScale(s)` | ポーズ解除時に戻る scale(slow-mo 演出と直交管理) |
| `SetCallback(cb, user)` | bit が立った/落ちた瞬間にだけ発火 |
| `Clear()` | 全 reason 解除(タイトルへ戻る時など) |

### FSaveSlot&lt;T&gt; / FSaveArchive (SaveSlot.h / SaveArchive.h) — **実ファイル保存**

`T` は **trivially-copyable な POD** 限定。`.acssave`(24B ヘッダ + payload + CRC32)を Win32 直叩きで読み書きします。これは stub ではなく**実ディスク I/O**です。

| メソッド | 説明 |
|----------|------|
| `Init(const wchar_t* path)` | ファイルパス設定(**ポインタのみ保持・コピーしない** → 寿命に注意) |
| `Save(const T& data, version=1)` | `.acssave` 形式で保存。`TResult<void>` |
| `Load(expected_version=1)` | 読み出し。`TResult<T>` |
| `Exists()` | ファイル有無(未初期化なら常に false) |
| `Delete()` | 削除(無ければ成功扱い・べき等) |

低レベルが欲しい時は `FSaveArchive::WriteToFile/ReadFromFile/PeekVersion/PeekPayloadSize`(static)を直接使えます。

---

## よく使うパターン

### 1. フェード付きシーン遷移(サンプル 58 そのまま)

`TransitionTo` がフェードアウト→暗転中に切替→フェードインを全部やってくれます。**遷移中は入力を無視**するのが定番です。

```cpp
// FScene2D の OnTick から (サンプル 58)
void FTitleScene::OnTick(f32 /*dt*/) noexcept {
    if (GetGame().Fade().IsActive()) return;           // 遷移中はガード
    if (Services().Input().IsPressed(kStart)) {
        // 次シーンへ: out 0.3s, in 0.3s
        GetGame().TransitionTo(MakeUnique<FLevelScene>(), 0.3f, 0.3f);
    } else if (Services().Input().IsPressed(kBack)) {
        GetGame().Quit();
    }
}
```

`Scene` 派生なら `OnUpdate` から同じく呼べます。フェード overlay は **FGame が描画する**ので、切替先シーンで重ねてフェードしないこと。

### 2. モーダルを重ねる(ポーズメニュー)

`ChangeScene` ではなく `PushScene` を使うと下のシーンが残り、`PopScene` で戻れます。

```cpp
void GameplayScene::OnUpdate(f32 dt) noexcept {
    if (Services().Input().IsPressed(kPause)) {
        Scenes().PushScene(MakeUnique<PauseMenuScene>());  // Gameplay は残る → OnPause
    }
}
// PauseMenuScene 側で閉じるとき:
void PauseMenuScene::OnUpdate(f32 dt) noexcept {
    if (Services().Input().IsPressed(kResume)) {
        Scenes().PopScene();                                // Gameplay が OnResume
    }
}
```

### 3. シーンを跨いで残る状態 (AppState)

ハイスコアやプレイヤープロファイルなど「シーンを切り替えても消えてほしくない」1 個の状態。

```cpp
struct PlayerProfile { acs::u32 hi_score = 0; };

// 起動時 (FGame::OnStart や最初のシーンの OnEnter で 1 回):
GetGame().EmplaceAppState<PlayerProfile>();

// 任意のシーンから:
if (auto* prof = GetGame().AppState<PlayerProfile>()) {
    if (score > prof->hi_score) prof->hi_score = score;
}
```

RTTI 不使用で型 ID を管理するため、`AppState<別の型>()` は安全に `nullptr` を返します。

### 4. ハイスコアを実ファイルに保存・復元(サンプル 38 のラウンドトリップ)

保存対象は POD。`static_assert(__is_trivially_copyable(...))` で型を守ります。

```cpp
// 型 (GameTypes.h)
struct HighScore { acs::u64 best_score = 0; acs::u64 timestamp = 0; };
static_assert(__is_trivially_copyable(HighScore), "POD only");

inline constexpr wchar_t kSaveFile[] = L"hello_full_game_highscore.acssave";

// 起動時にロード (FullGameApp::OnStart)
m_HighscoreSlot.Init(kSaveFile);
if (m_HighscoreSlot.Exists()) {
    auto r = m_HighscoreSlot.Load();              // TResult<HighScore>
    if (r.IsOk()) m_Highscore = r.Value();
    else ACS_LOG_WARN("load failed: %s", r.Error().message);
} else {
    ACS_LOG_INFO("first run, no save yet");
}

// ベスト更新時だけ保存 (FullGameApp::SaveHighScoreIfBetter)
void FullGameApp::SaveHighScoreIfBetter(u64 final_score) noexcept {
    if (final_score <= m_Highscore.best_score) return;
    m_Highscore.best_score = final_score;
    auto r = m_HighscoreSlot.Save(m_Highscore);   // 実ディスクへ書込
    if (r.IsErr()) ACS_LOG_WARN("save failed: %s", r.Error().message);
}
```

スキーマ(`T` の中身)を変えたら `Save(data, version)` の version を増やすと、旧データ読込時に `Load` が `ESaveArchiveSubCode::kSubMigrationNeeded` を返すので migrate へ分岐できます。

### 5. 高レベル進行を FGameFlow で(サンプル 38)

```cpp
m_Flow.Init(EFlowState::Splash);
m_Flow.RequestTransition(EFlowState::MainTitle, 0.0f);  // fade_sec=0 → 即時
// ... 毎フレーム:
m_Flow.Tick(dt);
if (input.JustPressed(EKey::Enter) && m_Flow.CurrentState() == EFlowState::MainTitle) {
    m_Flow.RequestTransition(EFlowState::Gameplay, 0.5f);
}
if (m_Flow.IsTransitioning()) {
    // m_Flow.FadeProgress() を alpha にして overlay を自前で描く
}
```

---

## 注意点 (gotcha)

- **遷移は次フレーム頭で適用**。`Change/Push/Pop` を呼んでも即座には切り替わらない。同フレームに複数要求すると後勝ち。`OnUpdate` 内で安全にスタックを書き換えられるのはこのため。
- **`TransitionTo` の描画は FGame 持ち**。切替先シーンで二重にフェードを描かない。フェードは time_scale の影響を受けず**実時間で進む**(ポーズ中でも遷移は進む)。
- **遷移中は入力をガード**。`if (GetGame().Fade().IsActive()) return;` を入れないと暗転中に二重遷移しがち(サンプル 58 の定番パターン)。
- **`Scene` と `FScene2D` でフック名が違う**。素の `Scene` は `OnEnter/OnUpdate/OnRender/OnExit`、`FScene2D` は `OnReady/OnTick/OnDrawHud`。継承元を確認すること。
- **`Scene::OnPause/OnResume` は Push/Pop でのみ呼ばれる**。`ChangeScene` は pause/resume を呼ばない(退場側は `OnExit`、新 top は `OnEnter`)。
- **`FSaveSlot::Init` はパス文字列をコピーしない**(ポインタ保持)。`static` / メンバの `constexpr wchar_t[]` など**寿命がスロット以上の wchar_t 列**を渡すこと。一時バッファを渡すと dangling。
- **`Save` は atomic ではない**。`CREATE_ALWAYS` で truncate 書込なので途中で電源断すると中途半端なファイルが残り得る。電源断耐性が要るなら tmp file → rename を自前で。`Load` 側は CRC32 で破損を検知し `kSubChecksumFail` を返す。
- **`TResult` の `Value()`/`Error()` は誤用するとアサートで停止**。必ず `IsOk()`/`IsErr()` で分岐してから読む。
- **`FSaveSlot<T>` の `T` は POD 限定**。ポインタや非トリビアルなメンバ(動的配列・文字列)を含む構造体を memcpy 保存してはいけない。
- **`FPauseDirector` は値を返すだけ**。`EffectiveTimeScale()` を取得して自分で `GetGame().SetTimeScale(...)` を呼ぶ必要がある(モジュールが FGame に依存しない設計)。
- **`FGameFlow` は遷移中の追加要求を無視**(後勝ちしない)。現遷移を完了させてから次を受ける。不正遷移(許可テーブル外)も no-op。
- **`FSettings::Save/Load` は現状 TODO スタブ**(Phase 2 予定)。`Settings.cpp` の実装は実 I/O を行わないので、設定の永続化は現時点では機能しない。設定を本当にディスクへ残したいなら `FSaveSlot<SettingsPod>` を自前で組むこと。

---

## 動くサンプル

- **`acs/samples/58_HelloTilemap/TilemapDemo.cpp`** — `FGame::TransitionTo` による FadeInOut 遷移(Title ⇄ Level)。`GetGame().Fade().IsActive()` での入力ガード。`FScene2D` 派生・`OnReady/OnTick/OnDrawHud`。
- **`acs/samples/38_HelloFullGame/`** — フルゲーム構成。`FSaveSlot<HighScore>` の実ファイル・ラウンドトリップ(`FullGameApp.cpp` の `OnStart` でロード / `SaveHighScoreIfBetter` で保存)、`FGameFlow` の進行管理、`Scene` 派生の `ChangeScene`(`GameplayScene.cpp` → `GameOverScene.cpp` → `TitleScene`)。POD 定義は `GameTypes.h`。
- ヘッダ実物: `acs/src/gameframework/{Game,SceneManager,Scene,FadeTransition,GameFlow,PauseDirector,AppState,SaveSlot,SaveArchive}.h`

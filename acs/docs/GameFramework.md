# ACS GameFramework モジュール 設計書 v13（包括版）

`acs::Application` の上に、ゲーム制作に必要な要素を**包括的に**載せるフレームワーク
モジュール。本書は実装前の確定設計書。多エージェント技術ディスカッションを 2 巡
（シーン基盤の精査 → 包括サブシステムの設計）行い統合した。

- モジュール: `src/gameframework/` ／ CMake ターゲット `ACS::GameFramework`
- 名前空間: `acs::game`
- 設計方針: ACS 規約準拠（STL 不使用 / 例外不使用 / RTTI 不使用 / `noexcept` 徹底 /
  カスタムコンテナ `TUniquePtr`/`TArray`/`FString`/`THashMap`/`TRc`）
- **基本原則: 既存エンジンを「ラップする・再実装しない」**。ECS・描画・アセット・
  音声・タイマー・スレッド・数学は既存モジュールを使う。

---

## 1. 目的と非目的

### 1.1 解決する課題
`acs::Application` は `OnUpdate`/`OnRender` が単一で、(a) 画面/状態の切替、
(b) シーン内オブジェクトの表現、(c) ゲーム制作の定番部品（補間・カメラ・入力
マップ・当たり判定・セーブ等）を何も持たない。GameFramework はこれらを
**8 つのピラー**として体系的に提供する。

### 1.2 設計の経緯
| 版 | 内容 | 精査で塞いだ点 |
|---|---|---|
| v1 | シーンスタックの素案 | — |
| v2 | + GPU リソース寿命 / 共有描画ヘルパ / 永続状態（3エージェント精査） | 3つの構造的穴 |
| v3 | シーン基盤の完全仕様化 | 命名・入力・非同期・補間・エラー処理を確定 |
| **v4** | **8 ピラーの包括設計（4エージェント設計）** | 「スコープがシーン管理に偏狭」を是正 |
| **v5** | **+ 製品化向けアセット暗号化（`AssetPack` モジュール）** | 出荷時のアセット保護 — 詳細は `docs/AssetPack.md` |
| **v6** | **アセット以外の全ピラー（A〜F・H）を 3 クラスタで多エージェント・レッドチーム精査** | v5 の実バグ・未仕様点を検出し修正・詳細化（§11） |
| **v7** | **肉付け：エフェクト・ピラー(I) + 完成度システム 9 つを追加** | メニュー/会話/適応BGM/実績/アクセシビリティ等（§12） |
| **v8** | **さらに肉付け：必須機能 6 領域（5 新ピラー J/K/L/M/N + 既存ピラー深度拡張）を追加** | シリアライズ/プレハブ・エディタ・AI 深度・ネット・Mod・音響/カメラ/入力/描画/procgen 深度（§13） |
| **v9** | **さらに練り：3D / 物理深度 / アニメ深度 / 出荷パイプライン / スケール / Mod 本設計 / ジャンルキット 7 つ** | 3D・Box2D 級物理・骨格 IK ラグドール・出荷&Live Ops（**新 Pillar O**）・チャンク&メモリ&スレッド（**新 Pillar P**）・Lua 5.4 本設計・VN/Roguelike/Tactical/SHMUP/Rhythm/Cards/Idle キット（§14） |
| **v10** | **広い視野で練り：ライティング & 雰囲気（新 Pillar Q）/ Polish & Game Feel（新 Pillar R）/ メタ層 / 著作ツール深度 / 移植性 seam** | 2D 動的ライティング v1 昇格 + AmbientDirector(時刻/天候) + decals/sprite destruction + water/sky + DoF/lens flare / cinematics + photo mode + tutorial + 世界反応 / TypedHandle 統一 + 決定論 profile + acs_test + Events.h + StyleGuide+lint + Privacy / Particle/BT/Level editor + FMOD/Wwise seam / Linux/Mac/Switch 5 段階 seam（§15） |
| **v11** | **さらに広い視野：Steamworks など Platform Services / Live Ops 深度 / Community 社会層 / Accessibility&Multimedia 深度 / AI&ML 統合 / Backend&Team 開発** | 新 5 ピラー S(Storefront)/T(Community)/U(AI-ML)/V(Backend)/W(Studio) + Pillar O 拡張 + Pillar H/R 拡張（§16） |
| **v12** | **残課題 11 領域全部練り：XR/物理高度化/音響高度化/AI 進化/入力&配信&モバイル/ニッチ&倫理** | 新 Pillar X(XR/AR/MR) + Pillar F3 物理（cloth/hair/destruction/fluid）+ Pillar H 音響深度（HRTF/convolution/FFT/granular）+ Pillar U AI 進化（vision-language/generative agents/AI companion）+ Pillar D/T/S/O 拡張（exotic input + streaming + mobile） + Education/AAC + ACS::Web3Bridge（honest 反対）+ 物販 SKU（§17）|
| **v13** | **全 24 ピラー + メタ層 + 完成度システム 9 + ジャンルキット 7 内部深掘り（20 エージェント総出）** | 各 pillar の data structure / algorithm / edge case / integration / determinism / 性能予算を**実装着手レベル**まで仕様化（§18） |

### 1.3 非目的（v1 では扱わない、§9 に拡張余地を明記）
本格 2D 剛体物理ソルバ / 3D 物理 / ビジュアルシーンエディタ / スクリプト言語
組込み / ネットワーク同期 / アニメーショングラフ。HDR・ポストプロセスのフル
フレーム制御も対象外（必要なら `Application` 直利用）。

---

## 2. アーキテクチャ — 8 ピラー

```
┌─────────────────────────────────────────────────────────────────┐
│ ゲームコード   ユーザーの Scene / Node サブクラス / Component    │
├─────────────────────────────────────────────────────────────────┤
│ acs::game — GameFramework                                       │
│  A. App & Scene ─ Game · Scene · SceneManager · RenderContext   │
│                   AppState · SceneServices(取り付けハブ)         │
│  ┌───────────┬───────────┬──────────┬──────────┬──────────────┐ │
│  │ B.オブジェ│ C.時間・  │ D.入力   │ E.カメラ │ F.物理・衝突 │ │
│  │  クトモデル│  アニメ   │          │          │              │ │
│  │ Node2D    │ Clock     │ InputMap │ Camera2D │ CollisionW.2D│ │
│  │ Transform │ Tween/Ease│          │          │ SpatialGrid  │ │
│  │ Component │ Sequence  │          │          │ PhysicsBody2D│ │
│  │ NodeTree  │ StateMach.│          │          │              │ │
│  │           │ SpriteAnim│          │          │              │ │
│  ├───────────┴───────────┼──────────┴──────────┴──────────────┤ │
│  │ G.リソース・永続化     │ H.UI・オーディオ・ツール           │ │
│  │ AssetBundle/TypedHandle│ UiLayer · AudioDirector · Random   │ │
│  │ SaveArchive · Settings │ TPool<T> · DebugOverlay · DebugDraw │ │
│  └───────────────────────┴────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────────────┤
│ 既存 ACS エンジン（ラップ対象。再実装しない）                    │
│ app·ecs·render·asset·audio·event·threading·math·ui·platform·mem  │
└─────────────────────────────────────────────────────────────────┘
```

**依存方向は一方向**: GameFramework → エンジン（逆は無し）。各ピラーはピラー A と
エンジンに依存してよいが、兄弟ピラーのヘッダは原則 include しない（実行時連携は
コンポーネント・`SceneServices`・`MessageBroker` 経由）。

---

## 3. ピラー A — App & Scene（基盤。v3 で確定済み）

`Game`/`Scene`/`SceneManager`/`RenderContext`/`AppState` と、GPU 安全なシーン破棄。
v3 仕様の要点（詳細は本節末の確定事項）:

- **`Scene`** — `OnEnter/OnUpdate/OnFixedUpdate/OnRender/OnExit/OnPause/OnResume/OnEvent`。
- **`SceneManager`** — `TUniquePtr<Scene>` のスタック。`ChangeScene`/`PushScene`/
  `PopScene`。遷移は遅延適用（1フレーム1遷移・後勝ち）。フェード遷移対応。
- **`Game : Application`** — フレームループから駆動。固定タイムステップ
  （アキュムレータ + 暴走防止クランプ）・タイムスケール。`ACS_GAME_MAIN` でエントリ生成。
- **GPU 遅延削除キュー** — 退場シーンを「フレームインフライト数+1（=3）」フレーム
  保持してから破棄。GPU が直前フレームで参照中のリソースの use-after-free を防ぐ。
- **`AppState`** — ユーザー定義の永続状態。`Game` が型消去 `void*` で保持、
  `Scene::app<T>()` で参照。シーンをまたいで生存。
- **`RenderContext`** — 全シーン共有の `SpriteBatch` + 既定 `Font`。シーン切替で
  パイプライン再構築しない。

### 3.1 `SceneServices` — 取り付けハブ（v4 の中心的追加）
ピラー B〜H の多くは「シーンが 1 つだけ持つ singleton」。これを `SceneServices`
に束ね、シーンが**使うものだけ宣言**して遅延生成する:

```cpp
enum class ESvc : u32 {
    None=0, Tweens=1<<0, Input=1<<1, Camera2D=1<<2, Physics2D=1<<3,
    Ui=1<<4, Audio=1<<5, Events=1<<6, Debug=1<<7, Timers=1<<8,
    Default2D = Tweens|Input|Camera2D|Physics2D|Audio|Events|Debug|Timers,
};
class SceneServices {
public:
    TweenManager&     Tweens()  noexcept;   SequenceRunner& Sequences() noexcept;
    InputMap&         Input()   noexcept;   Camera2D&       Cam()       noexcept;
    CollisionWorld2D& Physics() noexcept;   UiLayer&        Ui()        noexcept;
    AudioDirector&    Audio()   noexcept;   MessageBroker&  Events()    noexcept;
    DebugDraw&        Debug()   noexcept;   TimerManager&   Timers()    noexcept;
    // 要求していないサービスへのアクセスは debug assert
};
```

`Scene` は `virtual ESvc WantedServices() const noexcept` を 1 つ override するだけ。
メニュー画面は `ESvc::Ui|ESvc::Audio` のみ宣言し ECS/物理のコストを払わない。
`Game` が v3 で定めたシーム地点で `SceneServices` を生成・tick する。

### 3.2 取り付け機構（全サブシステムはこの 2 つのどちらか）
1. **`SceneServices` メンバ** — シーン単位の singleton（Tween/Input/Camera/物理/
   UI/Audio/Events/Debug/Timers）。`Game` が自動で `dt` を供給。
2. **`Game` グローバルサービス** — シーンをまたいで生存（`AssetRegistry`・
   `AudioEngine` デバイス・`SaveArchive`・`Settings`・`AppState`・`DebugOverlay`）。

シーン内オブジェクトの表現はピラー B（ノードツリー）。バルク処理が要るシーンは
ECS `World` をシーンのメンバとして持つ（§4.6）。

---

## 4. ピラー B — オブジェクトモデル（`Node2D`）

**「シーンの中身をどう表現するか」の中心設計。** 結論: **ノードツリーが主たる
オブジェクトモデル**。ECS `World` は群体（弾・パーティクル）用の別ツールで、
必要なシーンだけが持つ。両者は一方向ブリッジで繋ぐ（ECS を再実装も強制もしない）。

### 4.1 Node か ECS か — 役割分担
| ノードツリーを使う | ECS `World` を使う |
|---|---|
| 手で配置する階層的・個別スクリプトのオブジェクト（プレイヤー・UI・ボス・扉）。数十〜数百 | systems で一括更新する同種大量オブジェクト（弾・パーティクル・タイル）。数千 |
| 親子トランスフォーム（戦車の上の砲塔、頭上の HP バー） | 親子なしのフラットなもの |

ノードツリーは常にある（最低でもルート）。`World` は使うシーンだけが
`World _world;` をメンバに持つ（フレームワークは ECS を強制しない）。

### 4.2 `Transform2D`・`Node2D`
- **`Transform2D`** — `position`(FVec2) / `rotation`(f32 ラジアン) / `scale`(FVec2) の
  20 バイト値型。`FMat4` ではなく専用型（小さい・合成が速い・分解が非可逆でない）。
  `operator*` で親子合成、`ToMat4()` は `SpriteBatch::SetView` 等の必要時のみ。
- **`Node2D`** — 唯一のノードクラス（2D 専用ゆえ抽象 `Node` 基底は作らない）。
  非コピー・非ムーブ（`TUniquePtr` で所有、`NodeId`/`Node2D*` で参照）。
  ライフサイクルフック（override は `noexcept` 必須）:
  `OnSpawn / OnUpdate(dt) / OnFixedUpdate / OnDraw(RenderContext&) / OnDespawn /
  OnEnabledChanged`。フックは全て「親が先」（spawn・update・draw・despawn とも
  親→子の順）。`_enabled=false` のノードは subtree ごと更新スキップ、
  `_visible=false` は subtree ごと描画スキップ。

### 4.3 トランスフォーム伝播
`_local` が真値、`_world` は派生キャッシュ。`_local` を書き換えると
`MarkWorldDirty()` が dirty フラグを **subtree へ下方伝播**（既に dirty なら早期
脱出 → 計算量は「変化した部分木」に比例）。毎フレーム update と draw の間に
**1 回の解決パス** `ResolveTransforms()` で dirty なノードだけ再合成 → 全 `OnDraw`
が一貫した world transform を見る。`WorldTransform()` はキャッシュを返す
（解決パス後に有効）。update 中の厳密値が要れば `ComputeWorldTransformNow()`。

### 4.4 階層変更の安全性（古典的クラッシュの排除）
ツリー走査中（update/draw 中）の構造変更（spawn/destroy/reparent）は**フレーム
境界まで遅延**。`Scene` が `_spawn_queue`/`_despawn_queue`/`_reparent_queue` と
`_in_traversal` フラグを持つ。`Destroy()` は `_pending_destroy` を立てるだけで
即時解放しない → 自分自身を `OnUpdate` 中に `Destroy()` しても安全。破棄は
フレーム先頭の reap パスで `OnDespawn`（親→子）→ メモリ解放（子→親）。
`NodeId`（index+generation、`EntityId` と同形）で stale 参照を検出。

### 4.5 コンポーネント（合成）
ノードは `Component2D` を attach できる（sprite/animation/collider 等の再利用可能な
振る舞い）。型取得は **RTTI 不使用** — `ComponentKindOf<T>()`（`ecs/ComponentId.h`
と同じ「型ごとの一意 u32」カウンタ）+ virtual `Kind()` + `static_cast`。
`node.GetComponent<SpriteComponent>()` は線形探索（ノードのコンポーネントは数個）。
標準提供: `SpriteComponent` / `AnimatedSpriteComponent` / `TextComponent` /
`ParticleComponent`、サブクラス `CameraNode2D` / `CanvasNode`(画面座標の UI 用)。

### 4.6 ECS ブリッジ（一方向・任意）
ノードが群体（ECS）を併用したい場合のみ、ノードは 1 つの `EntityId` に
**bind** できる（`Node2D::CreateEntityIn(World&)`）。規則: ①一方向（ノードは
entity を知るが entity はノードを知らない）、②ノードが entity の寿命を所有、
③データ二重化なし（同期は明示的 `SyncToEntity()` のみ）。`Scene` のメンバ順は
`_root`（ノードツリー）→ ユーザーの `World _world;` の順（破棄逆順で、ECS-bind
ノードが破棄時に生きた `World` を触れるように）。

---

## 5. ピラー C — 時間・アニメーション

ACS のコールバックは `std::function` 不使用。本ピラーは 3 段のコールバック方式:
**(A) 値書き戻し**（最頻 — tween に変数ポインタを渡し毎 tick 書き込む。コールバック
不要）、**(B) 関数ポインタ + `void*`**（`Timer`/`MessageBroker` と同じ）、
**(C) `Node`/`Component` メソッド**（ノード対象の補間）。

| サブシステム | 役割 |
|---|---|
| `Clock` | シーン単位の scaled/unscaled 時間・`dt`・`fixed_dt`・フレーム数・pause |
| `Tween` + `TweenManager` | 値（f32/FVec2/FVec3/色）を A→B へイージング補間。`TweenManager` が所有・tick・完了処理 |
| `Easing` | 約 30 種のイージング関数（linear/quad/cubic/sine/expo/circ/back/elastic/bounce の in/out/inout）。ヘッダオンリ |
| `Sequence` + `SequenceRunner` | 時間付きアクションの連鎖（wait/call/tween/parallel/loop）。固定容量のアクション配列、関数ポインタ方式。カットシーン・出現ウェーブ |
| `StateMachine<T>` | 小さな汎用 FSM（enter/update/exit）。AI・ゲームフロー。ヘッダオンリテンプレート |
| `SpriteAnimator` | スプライトシートのフレームアニメ（クリップ・fps・loop/pingpong/once・フレームイベント） |

`TweenManager`/`SequenceRunner` は `SceneServices` 経由。`Tween` は `TimerManager`
の上ではなく独立（イージング曲線評価が要るため）。

---

## 6. ピラー D — 入力（`InputMap`）

グローバル `acs::Input`（ポーリング）の上に**アクションマッピング**を載せる。
- 名前付きアクションを物理入力（キー/マウス/ゲームパッド）に束ねる。1 アクションに
  複数バインド可。プレイヤー番号対応。
- デジタルアクション（`Pressed/Held/Released`）とアナログアクション/軸
  （例 "MoveX" を A/D キー or スティックから、`Vector2` 取得）。
- バインドは宣言的に登録。ゲームロジックは物理キーから疎結合になり、キーコンフィグ
  UI も書ける。バインドは `Settings`（§8）に保存可能。
- `OnEvent` ルーティング: ウィンドウ/入力イベントは最上段シーンにのみ配送（v3 確定）。

入力コンテキストスタック（gameplay/menu/dialogue でバインド集を push/pop）は v1.1
（`InputMap` が context id を最初から持つので後付け可能）。

---

## 7. ピラー E — カメラ + ピラー F — 物理・衝突

### 7.1 `Camera2D`
位置・ズーム・回転、ターゲット追従（スムージング・デッドゾーン）、画面シェイク
（trauma 方式）、ワールド↔スクリーン座標変換、ワールド境界クランプ。
`SpriteBatch::SetView(cam, zoom)` に接続。3D 用 `CameraRig`（orbit/FPS/follow）は
`math/Camera` をラップ（v1 任意）。

### 7.2 物理・衝突
- **衝突プリミティブは既存** — `math/Collision2D.h`（`Aabb2`/`Circle`/`Ray2`、
  `Intersect`/`Resolve`/`Raycast*`）を**再エクスポート**。新規プリミティブは作らない。
- **`SpatialGrid`** — 一様グリッドのブロードフェーズ（O(1) 挿入/近傍クエリ、毎フレーム
  clear & rebuild、ツリー再平衡なし）。O(N²) ペアテストを排除。
- **`CollisionWorld2D`** — `SpatialGrid` + 形状登録の薄いラッパ。
  `OverlapCircle`/`OverlapAabb`/`Raycast`（ブロード+ナロー一括）。レイヤ/マスクで
  フィルタ。
- **`PhysicsBody2D`** — 速度/加速度/重力の運動学的積分 + collide-and-slide。
  剛体ソルバではない（プラットフォーマー/トップダウン向けの swept kinematic）。
  トリガーの `OnEnter/Stay/Exit`。

---

## 8. ピラー G — リソース・永続化 ／ ピラー H — UI・オーディオ・ツール

### 8.1 リソース・永続化
- **`AssetPack`（製品化向けアセット暗号化）** — 出荷時、バラのアセットを 1 つの
  暗号化アーカイブ `.acpak` にまとめ、利用者がインストールフォルダからアセットを
  取り出せないようにする。AES-256-GCM（Windows CNG）+ LZ4 圧縮 + 完全性検証
  （`content_hash` + GCM タグ）。`AssetRegistry` の下に VFS（仮想ファイルシステム）を
  挟み、**開発はバラファイル・出荷は暗号化 pak** をマウント 1 行で切替（ゲーム
  コード差分ゼロ）。パッカー CLI `acs_assetpack`。クライアント側暗号化は本質的に
  「難読化」であり、カジュアル流出と汎用吸い出しツールを阻止する（本気の解析者は
  止めない — 設計書で正直に明記）。**これは独立したエンジンモジュール
  `ACS::AssetPack`** — VFS はエンジン基盤であり、アプリ層の GameFramework には
  入れない（エンジンがアプリ層へ逆依存できないため）。Pillar G はこれを利用・統合
  する。**完全仕様は `docs/AssetPack.md`**。
- **型付きハンドル** — `SpriteHandle`/`SoundHandle`/`MeshHandle`（`TRc<Asset>` の
  薄い型安全ラッパ。呼び出し側の `static_cast` を排除）。
- **`AssetBundle`** — 名前付き・シーンスコープのアセット集合を非同期一括ロード
  （既存 `AssetRegistry::LoadAsync`/`AssetFuture` を使用）+ 集約進捗(0..1)。
  ローディング画面シーンが進捗をポーリングできる。VFS 経由なのでバラでも暗号化
  pak でも透過に動く。
- **`SaveArchive`** — 構造化セーブ。**タグ付きバイナリ**（フィールドごとに
  `u16 タグ + 型 + 長さ + 値`）。未知タグは長さでスキップ、欠落タグは既定値 →
  スキーマ進化耐性（旧セーブが新コードで読める）。エンベロープに magic + version
  + crc32、temp ファイルへ書いてアトミック rename。`TResult` でエラー。設定系の
  単純な key-value は既存 `Storage`（INI）をそのまま使う。**任意で HMAC-SHA256 の
  改竄タグ**を付けられる（`AssetPack` の Crypto を再利用。リーダーボード等のチート
  対策。内容暗号化は既定オフ）。
- **`Settings`** — 型付きゲーム設定（音量・解像度・キーバインド）を `Storage` 永続化。

### 8.3 製品化（productization）ワークフロー
- **開発ビルド** — バラ `assets/` を VFS にマウント。変更ファイルは即ホットリロード。
  `acs_assetpack` は不要。`DebugOverlay`/`DebugDraw` 有効。
- **出荷ビルド** — Release 梱包ステップで `acs_assetpack pack` を実行（CI ゲートに
  `acs_assetpack verify`）→ `game.acpak` を実行ファイルに同梱 → `Game` 起動時に
  マウント。`ACS_GAME_SHIPPING` フラグで分岐、`DebugOverlay` 等は `ACS_GAME_DEBUG`
  により shipping ビルドから完全に消える（コスト 0）。
- ゲームコードは dev/ship で完全に同一。違いは起動時のマウント 1 行とビルドフラグのみ。

### 8.2 UI・オーディオ・ツール
- **`UiLayer`** — 既存 `ui/` の `Widget` ツリーを `Scene` のライフサイクル/入力に
  接続するグルー（`ui/` はあるが `Scene` と繋ぐものが無い）。
- **`AudioDirector`** — `AudioEngine` の上のシーン対応層。ボリュームバス
  （Master/Bgm/Sfx）、BGM のシーンまたぎ持続・クロスフェード、名前付き SFX
  ワンショット、ダッキング。`Game` が所有しシーンをまたいで生存。
- **`Random`** — ゲーム用 PRNG クラス（`xoshiro128**` + SplitMix64 シード）。
  インスタンス化・シード可能・決定論的。int/float 範囲・bool・重み付き選択・
  円内/円周上の点・`TArray` シャッフル・`RandomColor`。`Random::Global()` も提供。
  `easy` の private xorshift を公開クラス化したもの。
- **`TPool<T>`** — 弾/エフェクト用オブジェクトプール。世代付きハンドル
  （`EntityId` と同形）、固定/可変容量、live のみ反復、STL 非依存。
- **`DebugOverlay`** — FPS/フレーム時間グラフ/シーンスタック表示/ライブ調整値。
  **`SpriteBatch` で描画**（ImGui は DX12 専用なので不採用、Diligent でも動く・
  追加 GPU リソース 0）。`ACS_GAME_DEBUG` で shipping ビルドから完全に消える。
- **`DebugDraw`** — ワールド空間の即時モード線/矩形/円/テキスト（毎フレーム自動クリア）。

---

## 9. モジュール構成と実装フェーズ

### 9.1 ファイル構成（`src/gameframework/`）
```
Module.cmake   acs_module(NAME GameFramework TYPE Runtime
                 PUBLIC_DEPS App Render Ecs Asset Audio Event Threading
                             Math Ui Platform)
A: Game.h/.cpp  Scene.h  SceneManager.h/.cpp  RenderContext.h/.cpp
   SceneServices.h/.cpp  AppState.h  GameEntry.h
B: Node2D.h/.cpp  Transform2D.h  Component2D.h/.cpp  Components.h/.cpp
C: Clock.h  Easing.h  Tween.h/.cpp  Sequence.h/.cpp  StateMachine.h
   SpriteAnimator.h/.cpp
D: InputMap.h/.cpp
E: Camera2D.h/.cpp
F: Collision.h(再エクスポート)  SpatialGrid.h/.cpp  CollisionWorld2D.h/.cpp
   PhysicsBody2D.h/.cpp
G: AssetHandles.h  AssetBundle.h/.cpp  SaveArchive.h/.cpp  Settings.h/.cpp
H: UiLayer.h/.cpp  AudioDirector.h/.cpp  Random.h/.cpp  Pool.h
   DebugOverlay.h/.cpp  DebugDraw.h/.cpp
GameFrameworkConfig.h  全調整定数を 1 箇所に
```
`modules.cmake` に `acs_enable_module(GameFramework)`、`ACS::Game` 集約にも追加。

### 9.2 実装フェーズ（段階的に実装・各フェーズでビルド検証）
| Ph | 内容 | 成果物 |
|---|---|---|
| 1 | ピラー A（シーン基盤）+ モジュール骨格 | シーン遷移が動く。サンプル `27_HelloScenes` |
| 2 | ピラー B（Node2D・Transform・Component） | ノードツリーで物が描ける。`28_HelloNodes` |
| 3 | ピラー C（Clock/Tween/Easing/Sequence/StateMachine/SpriteAnimator） | アニメ・演出 |
| 4 | ピラー D+E（InputMap・Camera2D） | 操作とスクロール |
| 5 | ピラー F（衝突・物理） | `29_HelloPlatformer`（B〜F 統合） |
| 6 | ピラー G+H（リソース/セーブ/UI/オーディオ/乱数/プール/デバッグ） | `30_HelloUiAudio` |
| 7 | 仕上げ・統合サンプル `31_HelloShowcase` | 小さな完成ゲーム |

各フェーズは独立してビルド検証可能。フェーズ 1 が他の土台。

### 9.3 v1 範囲外（v2、v1 に拡張シーム有り）
2D 剛体ソルバ（`CollisionWorld2D` の隣に `DynamicsSolver` を追加可）/ 3D 物理 /
シーンシリアライズ・エディタ（`Component` 登録は集約済み、`Serialize` を後付け可）/
スクリプト言語（`Component` が C++ で代替）/ ネットワーク同期 / アニメーション
グラフ / 入力コンテキストスタック / dev コンソール / タイルマップ / プロファイラ。

---

## 10. 主要な設計判断（まとめ）

| # | 判断 | 理由 |
|---|---|---|
| 1 | 既存エンジンをラップ、再実装しない | ECS/描画/音声/アセット等は既に高品質。重複は害 |
| 2 | オブジェクトモデルは**ノードツリー主体**、ECS は群体用の併用ツール、ブリッジは一方向 | 手配置の階層オブジェクトにツリーが自然。ECS は既存を活かす。両者の所有権が衝突しない |
| 3 | `SceneServices` + `WantedServices()` で取り付け | 「メニューに ECS/物理を強制しない」を保ちつつ全機能に到達可能 |
| 4 | サブシステム取り付けは `SceneServices`/`Game`グローバルの 2 機構のみ | どのサブシステムも例外なくこの 2 つに収まる＝設計が一貫している証拠 |
| 5 | GPU リソースは遅延削除キュー（3 フレーム）で破棄 | シーン破棄時の use-after-free を構造的に排除（v3 確定） |
| 6 | 階層変更・シーン遷移は遅延適用 | 走査中の自己破棄クラッシュを排除 |
| 7 | RTTI 不使用の型識別（`ComponentKindOf<T>` カウンタ + virtual `Kind()`） | ACS 既存 `ComponentId` と同方式。`dynamic_cast` 不要 |
| 8 | コールバックは値書き戻し / 関数ポインタ+`void*` の 3 段方式 | `std::function` 不使用。既存 `Timer`/`MessageBroker` と同イディオム |
| 9 | デバッグ描画は `SpriteBatch`（ImGui 不採用） | ImGui は DX12 専用。`SpriteBatch` は全バックエンドで動き追加 GPU コスト 0 |
| 10 | 段階的実装（7 フェーズ）。各フェーズでビルド検証 | 巨大モジュールを検証可能な単位に分割 |

---

## 11. v6 — レッドチーム精査による改善

アセット以外の全ピラーを 3 クラスタ（A+B / C+D+E / F+H）に分け、多エージェントで
レッドチーム精査した。v5 が「アーキテクチャ概要」止まりだった各ピラーを実装可能な
詳細仕様へ深め、その過程で検出した v5 の**実バグ・未仕様点**を以下の通り修正した。

### A. App & Scene
- **GPU 遅延削除を観測可能なフェンス基準に**。v5 の「フレームインフライト数+1=3
  フレーム後に破棄」は固定値だが、エンジンはフレームインフライト数を公開して
  いない。固定値 3 は早期解放（GPU use-after-free クラッシュ）にも過剰 VRAM 保持
  にもなり得る。`Renderer` のフェンス完了/フレーム番号を観測して破棄判定する設計に
  変更（必要なら `Renderer` に最小の照会 API を 1 つ追加）。
- **シーン遷移を順序付きキュー方式に**。v5 の「1フレーム1遷移・後勝ち」は意図を
  黙って破棄する（同フレームで `PushScene` 後に `ChangeScene` を呼ぶと `Push` が
  消える）。順序付きキューで適用し、`PopScene`（スタック1枚時）・`ChangeScene`
  （空スタック時）・`OnEnter` からの遷移要求など全エッジケースの挙動を明示。

### B. オブジェクトモデル（Node2D）
- トランスフォーム伝播（dirty フラグ下方伝播 + 早期脱出 + 1フレーム1解決パス）、
  階層変更の遅延適用（spawn/despawn/reparent キュー）、`NodeId` の世代管理、
  RTTI 不使用のコンポーネント型識別を完全アルゴリズム仕様化。

### C/D/E. 時間・入力・カメラ
- **コールバックを単一の正準型 `Callback<Args...>`（関数ポインタ + `void*`）に統一**。
  v5 の「3 段方式」は分類にすぎず仕様ではない。tween/sequence はフレームをまたいで
  生存し、破棄され得るノードを対象にするため、ダングリング `void*` を構造的に
  扱える単一型へ確定。
- **`SpriteBatch::SetView` に回転パラメータが無い**（ビュー変換は画面軸固定）と
  判明。v1 の `Camera2D` は**位置・ズーム対応、回転は非対応**とする（2D の回転
  カメラは用途が限定的。回転を入れるなら `SpriteBatch` の VS 拡張が前提 → v2）。
- tween のキャンセル/完了、sequence のステップ実行、入力アクション解決、カメラ
  追従のスムージング数学を詳細仕様化。

### F. 物理・衝突
- **トンネリングへの対処を明確化**。v5 は `PhysicsBody2D` を「swept kinematic」と
  書くが、離散オーバーラップ + collide-and-slide は swept ではなく、高速な弾は壁を
  すり抜ける。v1 は **(a) 通常ボディは離散 collide-and-slide、(b) 高速移動体には
  スイープ判定（線分 vs コライダ）をオプトイン**の二段構成とする。`SpatialGrid`
  ブロードフェーズの dedup・レイヤ/マスクを詳細仕様化。

### H. UI・音・ツール
- `AudioDirector` のボイス寿命/ポリフォニー上限/バス音量合成、`UiLayer` の既存
  `ui/` ツリーへの入力ルーティングとライフサイクル接続、`TPool<T>` のハンドル
  世代安全性を詳細仕様化。

> 各クラスタの完全な v6 詳細設計（レッドチーム指摘の全項目・全 API・全アルゴリズム）は
> 多エージェント精査の成果として別途保持しており、実装フェーズで各ピラーの確定
> 仕様として参照する。本書はその統合的な目次として機能する。

---

## 12. v7 — 肉付け：エフェクト・ピラーと完成度システム群

「肉付けと品質向上」を目的に、エフェクト系と、フレームワークを「出荷可能な完成品」へ
引き上げる 9 システムを多エージェントで設計し追加した。すべて v6 の不変条件
（既存ピラー内に配置・取り付けは `SceneServices`/`Game` グローバルの 2 機構のみ・
STL/例外/RTTI 不使用）を維持する。

### 12.1 Pillar I — エフェクト / VFX / ゲームフィール（新規・9 番目のピラー）

v6 は `ParticleComponent` とカメラシェイクのみで、「ここで爆発を再生」する手段が
無かった。エフェクトは本質的に横断的（爆発 = パーティクル + 画面フラッシュ +
シェイク + ヒットストップ + 音、を 1 単位で発火）なので独立ピラーとする。
**設計原則: エフェクトは「指揮者」であり自前のシミュレーションを持たない** —
パーティクルは `render/Particles.h`、tween は Pillar C、シェイクは `Camera2D`、音は
オーディオ層から借り、Effects はそれらを構成（compose）するだけ。兄弟ピラーの
ヘッダは include せず、連携は `SceneServices` 経由。

構成:
- `EffectDef` / `EffectAsset` — データ駆動のエフェクト定義（「レシピ」）。
- `EffectRegistry` — id → `EffectDef`。組み込みプリセット（爆発・炎・煙・きらめき・
  マズルフラッシュ・魔法バースト・砂塵 等）を所有。
- `EffectSystem` — スポナー。`PlayEffect(id, position)`、ワンショット自動消滅、
  ループ/ノードアタッチ、プーリング。`EffectHandle`（世代付き）で生存エフェクト参照。
- `EffectComponent` — ノードにアタッチするエフェクト。
- **画面エフェクト** — カラーフラッシュ、**ヒットストップ/時間凍結**（インパクトの
  「重み」を出す数フレーム停止）、シェイク発火、ビネット/色収差パルス、スローモー。
- **スプライトエフェクト** — ヒットフラッシュ（被弾時の白点滅）、モーショントレイル、
  スクワッシュ&ストレッチ、フローティングダメージ数値、ブロブシャドウ、ディゾルブ。
- **エフェクト合成** — 爆発 1 つを「パーティクル+フラッシュ+シェイク+ヒットストップ
  +音」として原子的に発火。Pillar C の Tween/Sequence と連携。

### 12.2 完成度を上げる 9 システム

v6 は「土台」（シーン・ノード・時間・入力・衝突・セーブ）を提供するが、土台と完成
ゲームの間（メニュー・会話・適応 BGM・実績・アクセシビリティ）は開発者が都度作る
必要があった。これを埋める 9 システム:

| システム | 配置 | 内容 |
|---|---|---|
| `LocalizationDirector` | H・Game グローバル | i18n 統合。多書記体系フォントセット、実行時言語切替（UI/会話を再フロー）、ローカライズ素材、複数形・引数整形、疑似ローカライズ |
| `UiKit`（`UiLayer` 昇格） | H・`ESvc::Ui` | **フォーカスナビゲーション**（ゲームパッド/キーボードで UI 操作 — 出荷必須）、メニュープリセット（メイン/ポーズ/設定/確認ダイアログ）、HUD ヘルパ（バー/カウンタ/ゲージ/トースト）、UI アニメーション、UI 効果音 |
| `Dialogue` | H・`ESvc::Dialogue`（新） | 分岐会話/テキストイベント/カットシーン。`.dlg` テキスト形式、タイプライタ表示、選択肢、条件/フラグ（`DialogueVars` をセーブ永続化）、`CutsceneTrack`（Sequence の上の演出層） |
| `MusicDirector`（`AudioDirector` 吸収） | H・Game グローバル | 適応的・レイヤ式 BGM。バスミキサ、ステム層 + intensity カーブ、スティンガー、拍同期の量子化遷移 |
| `Progress` | G・Game グローバル | 実績/統計/アンロック/マイルストーン。宣言的トリガー（統計閾値で自動アンロック）、セーブ永続化、Steam/コンソール連携 seam |
| `Accessibility` | H・Game グローバル | テキスト拡大、色覚モード、字幕、画面シェイク/フラッシュ低減（光過敏対策）、モーション低減、ホールド/タップ変換、ハイコントラスト UI |
| `Starter2D` | B・コンテンツ | 製品品質の 2D キャラコントローラ（コヨーテタイム/ジャンプバッファ/可変ジャンプ/斜面）、トップダウン版、カメラプリセット、共通コンポーネント（Health/Hitbox/Patrol/Pickup 等）、テンプレートシーン |
| `SaveSlots` | G・Game グローバル | 複数スロットセーブ管理。ヘッダ（時刻/プレイ時間/サムネ/進捗）だけ高速読みするセーブ選択 UI、オートセーブ、破損安全 |
| `GameFlow` | A・コンテンツ | boot→スプラッシュ→タイトル→メニュー→ゲーム→クレジットの app フロー雛形。**1〜8 の組み合わせにすぎないため v1.1 送り** |

### 12.3 実装フェーズの拡張
v6 の 7 フェーズに続けて: **Ph8** `LocalizationDirector` + `UiKit`（サンプル
`32_HelloMenus`）→ **Ph9** Pillar I エフェクト + `MusicDirector` + `Accessibility`
（`33_HelloEffects`）→ **Ph10** `Dialogue` + `SaveSlots` + `Progress`
（`34_HelloDialogue`）→ **Ph11** `Starter2D` テンプレート + 全部入り showcase
（`35_HelloAdventure`）。Ph8 は Ph9〜11 の前提。
（v8 で Ph7.5〜Ph27 へさらに拡張 — §13.8）

---

## 13. v8 — さらに肉付け：必須機能 6 領域

ユーザー指摘「シリアライズ機能とか無くない？必須機能まだあるよね。もっと時間かけて
練りまくって」を受け、**6 エージェント並列**で深掘り設計。**5 つの新ピラー J/K/L/M/N と
既存ピラー深度拡張**を確定。各設計の詳細は別途エージェント成果物として保持し、本書は
統合的目次として機能。

### 13.1 Pillar J — シリアライズ / リフレクション / プレハブ（指摘事項）

v6/v7 の `SaveArchive` は**バイト列の envelope のみ**で、ノードツリー保存・プレハブ・
リフレクションが皆無 — **エディタ・ホットリロード・ネットレプリケーション・セーブの
4 つを同時に破綻させていた本質的穴**。これを構造的に解放する基盤として独立ピラー化。

新エンジンモジュール **`acs::serialize`** + GameFramework 側高位 API:
- **RTTI 不使用反射**: `TypeInfo<T>` 特殊化 + `ACS_REFLECT(T, ...)` マクロ + 明示
  `Register()`。リンカセクション autoreg は LTO/SAFESEH 地雷で不採用。型 ID は
  `HashBytes(FQN)` の 64bit ハッシュ。フィールドタグは手動宣言で並べ替え互換を保証。
- **1 反射 + 2 フォーマット**:
  - **`.atxt`** — S 式風・行ベース・人間 diff 可能。設計データ・プレハブ・シーン用。
  - **`.abin`** — タグ付きバイナリ。**未知タグの安全スキップ + フィールド単位 version** で
    スキーマ進化耐性。`SaveArchive` の HMAC/CRC32/atomic rename をそのまま継承。
- **`SceneSerializer`** — Scene → 中間表現 `SceneDocument` → `.scene` で永続化。`Node2D`
  参照は **`SerialNodeId`**（depth-first preorder 採番）で安定化、2nd pass 解決。
- **`PrefabSystem`** — `.prefab` ロード/インスタンス化。**Unity 流のフィールド単位
  override + nested prefab**。プレハブ更新で「override されていない値だけ追従」。
- **`DataAsset<T>`** — 型付き設計データ（`.tdat`）、`AssetRegistry` 経由でホットリロード。
- **`SaveSlot<T>`** — v7 `SaveSlots` をリフレクション駆動で 1 関数化。ヘッダ/ペイロード
  分離でスロット選択 UI 瞬時表示。
- **`MigrationRegistry`** — `Register(type_id, from_ver, fn)` で旧版を新スキーマへ自動変換。
  フィールド追加/削除は無コード、kind 変更は関数 1 個。
- **参照** — `NodeRefT`/`AssetRefT`/`RcRefT`/`WeakRefT` を区別、`TRc` 循環は遅延解決。
- **コールバック保存しない方針**（`Transient` 属性）— 関数ポインタは保存価値<複雑度。
  再 attach は `OnSpawn`/`OnAfterLoad` で。

**1 行サマリ**: 「1 反射 + 2 フォーマット」の上に**シーン・プレハブ・データアセット・
セーブ・ネット差分・エディタの 6 つを全部乗せる**。他の v8 ピラーの構造的前提。

### 13.2 Pillar K — エディタ / 開発ツール

v6 が明示的に v1 範囲外とした「シーンエディタ・dev コンソール・プロファイラ」を完全
設計。**Pillar J の反射を前提**。`ACS_GAME_DEBUG` のみ実体化、shipping ではヘッダのみの
スタブ（ゼロオーバーヘッド）。

- **`Inspector`** — 反射駆動。`Node2D` ツリー + 選択ノードのコンポーネント + 編集可能
  フィールド（slider/dropdown/color picker/asset picker）。`FieldUiHint`（range/step/
  tooltip/category）を読む。ピックは Pillar F の raycast、複数選択、検索/フィルタ。
- **`EditorSeam`** — ギズモ（move/rotate/scale、ワールド/スクリーン）、選択モデル、
  **反射スナップショット駆動 undo/redo**、in-game edit-mode トグル + 外部エディタ用 seam 共通。
- **`DevConsole`** — タブ補完付きコマンドライン、型付き引数の登録コマンド（`spawn`/
  `give`/`tp`/`setvar`）、**CVar**（型付きライブ変数 + cheat/dev/release フラグ）、履歴。
- **`HotReload`** — Win32 `ReadDirectoryChangesW` でファイル監視 → アセット再 import →
  `MessageBroker::Publish(AssetReloaded)`。PNG/フォント/データアセット/スクリプトが
  再起動なしで反映。per-type policy（trivial/scene refresh）。
- **`Profiler`** — スコープ計測（`ACS_GAME_PROFILE_SCOPE("Physics")`、shipping で空展開）+
  フレームタイム/メモリ/ドローカウント HUD、フレームグラフ。
- **`Replay`** — 入力ストリーム + RNG state を記録、決定論的再生。Pillar M (lockstep) と
  Pillar N (modding) も同じ仕組みを共有。
- **`DebugDraw+`** — 永続/一時、ワールド/スクリーン空間、デプステスト切替、チャネル
  トグル（物理/パス/トリガ/音域）。
- **`LiveTune`** — ゲームプレイ定数を CVar にバインド → インスペクタのスライダで実時間
  調整 → dev settings に永続化。

### 13.3 Pillar L — AI / ゲームプレイ深度

v7 までは `StateMachine` と `CollisionWorld2D` のみで、本格 BT・経路探索・タイルマップ・
アニメグラフは皆無。これが「動くデモ」と「実ゲーム」を分ける層。

- **`BehaviorTree`** — Sequence/Selector/Parallel + デコレータ（Inverter/Repeater/
  UntilSuccess/Cooldown）+ リーフ（Action/Condition、Callback ボディ + 反射フィールド）。
  `.bt` テキスト形式 + コード DSL の両対応。`BehaviorTreeComponent` でアタッチ。BT と
  `StateMachine` は併存（BT = 階層化決定木、SM = 順次状態）。
- **`PathfindingSystem`** — A* on 2D nav grid。`ESvc::Pathfind`。重み付きタイル（沼=遅い）、
  動的障害物、非同期パス要求（future-like）。`Tilemap` から自動 grid 構築。
- **`Tilemap`** — `TilemapNode2D`。レイヤ（地面/壁/装飾/空）、per-tile property
  (`THashMap<FString, Value>`)、Tiled `.tmx` 対応、レイヤ × チャンク単位の 1 ドロー。
  **コリジョン形状自動生成 + ナビグリッド自動ビュー**で Pillar F/L へ直結。
- **`AnimationGraph`** — クリップ間のステートマシン + パラメータ駆動遷移（`velocity > 50`
  で idle→run）+ **ブレンドツリー**（8 方向歩行を aim 角でブレンド）+ **イベント
  トラック**（フレーム 5 で足音、フレーム 7 で砂塵パーティクル — **Pillar I エフェクトと
  連動**）。
- **`PerceptionComponent`** — 視界コーン（FOV + range + 遮蔽 raycast）、聴覚半径
  （`SenseGraph` に音源 publish）、line-of-sight。
- **`SpatialQuery`** — `SpatialGrid` 上に radius/cone/box/ray + バッチクエリ + レイヤ
  フィルタ + 距離ソート。AOE/敵探索を手書きしない。
- **`TriggerVolumeComponent`** — enter/stay/exit Callback、レイヤマスク、latched-once。
  Pickup/checkpoint/region/level-transition に直結。
- **`Steering`** — Reynolds（seek/flee/arrival/pursuit/wander/flock/path-follow）、合成可能。

### 13.4 Pillar M — ネットワーク / マルチプレイヤー基盤

v6/v7 では「ネット同期」を v2 送りにしていたが、**SEAM だけは v1 で切らないと後付け
不可能**な構造的問題のため再設計。**正直なスコープ判断**を含む。

**v1 モデル: Deterministic Lockstep (≤4 人) + Async (リプレイ/ゴースト/リーダーボード)**:
- ACS の固定 timestep + 名前付き PRNG チャネル（§13.6）が lockstep に最適。
- Authoritative server + client prediction/reconciliation は **seam
  （`IReplicator`/`IAuthority`/`ScenePeerRole`/同一 delta シリアライザ）のみ v1 で確定、
  実装は v2** — indie 2D の現実スコープ × 実装複雑度 × アンチチート honest 判断。
- **Async = v1 で完全実装** — タイムアタックゴースト・デイリーチャレンジ・リーダー
  ボードは real-time netcode の苦難なしに最大価値を出す。

主要構成:
- **`INetTransport`** 抽象 + Winsock UDP 既定実装（reliable/unreliable/sequenced + 8
  チャネル多重化 + RTO 指数バックオフ + AIMD 輻輳制御）。Steam Datagram/EOS/WebSocket は
  差し替え seam。外部ライブラリ不採用。
- **`LockstepRunner`** — 入力フォワード、入力遅延 4〜8 ticks、**60 tick 毎の state hash
  で desync 検知**、state dump 報告。
- **`NetDeterminism.h`** — 決定論コントラクト明文化（`time()`/`Random::Global()`/
  `THashMap` 順序依存/FPU dispatch 禁止）。`ACS_NET_DETERMINISTIC` ビルドフラグで SSE2 固定。
- **`Replication` 属性** — `[[acs::Replicated(channel, rate, interp, relevance=Distance(R))]]`、
  リフレクション駆動。`[[acs::Rpc(Reliable, ServerToClient)]]` で透過呼び出し（ローカル/
  リモートをユーザコードが区別しない）。手書きマクロ経路（`ACS_REPLICATED_BEGIN/...`）も併設。
- **`RecordingDirector`** — `.acsr` リプレイ（HMAC 付き、入力 + シード + state checkpoint）、
  `GhostPlayback`、`AsyncTurnDirector`。
- **`ILeaderboardClient`** 抽象 + **`acs_replay_verify`** CLI — サーバが受信リプレイを
  ヘッドレス再シミュレートしてスコア検証（**indie で最も低コスト・高効果のアンチチート**）。
- **`Scene::PeerRole`**（`SinglePlayer`/`Host`/`Client`/`DedicatedServer`/`Replay`）—
  1 シーンクラスで全モード対応、`SpawnReplicated` + `NetEntityId` mapping、host migration。
- **honest 限界明示** — NAT 越え未対応（LAN + dedicated host 公開 IP のみ）、暗号化
  トランスポート未実装、本格対戦アクションは v2 待ち、**クライアント側難読化は theatre と認める**。

### 13.5 Pillar N — Mod / スクリプト

> 注: 専任エージェントがタイムアウト。本節は概略のみで、実装フェーズ前に再深掘り予定。

- **Modding via AssetPack VFS overlay** — プレイヤーが `mods/foo.acpak` を置けば
  AssetPack の VFS マウント順で自動上書き。`manifest`（名前/作者/版/依存/ロード順）、
  競合検出、プロファイル毎の有効化。
- **スクリプト言語: Lua 5.4 推奨** — indie ゲーム実績・組込みやすさ（C API がフラット、
  ACS の no-STL/no-exception と整合可能）・コミュニティ規模・LuaJIT で性能必要なら拡張可。
  代案: Wren（小さく OO 純度高い）、AngelScript（C++ 風静的型）。
- **統合** — スクリプトから `Node2D`/`Component2D` 生成、`MessageBroker` 購読、データ
  アセット宣言、`Component2D::OnUpdate` 実装。**Pillar J 反射との連携**で「スクリプト書き
  フィールド」も Inspector に出る。
- **サンドボックス** — 信頼レベル（信頼ローカル / 信頼配信署名済み / 未署名 mod）で
  file/net/process アクセス制限、メモリ/CPU 時間クオータ。
- **Tier 別 mod**:
  - **A. アセット差し替えのみ** — 完全 sandbox、ノーリスク。
  - **B. データ mod** — スクリプト + データアセットで新コンテンツ。指針的主流。
  - **C. トータルコンバージョン** — スクリプトでゲームロジック変更。
  - **D. C++ DLL plugin** — shipping mod では非推奨（ABI 脆性 + サンドボックス困難）。
- **ホットリロード** — `.lua` 編集 → ゲーム即反映（Pillar K の HotReload と統合）。
- **`IModRepository`** 抽象 — Steam Workshop/mod.io/itch.io/ローカルのみプラグイン。
- **`ModManagerScreen`**（UiKit）— インストール mod 一覧、有効化、ドラッグでロード順
  並べ替え、競合警告、依存解決。
- **save 互換** — mod 抜きで modded save をロードしたら graceful degradation か lock-out
  を mod が宣言。

### 13.6 既存ピラー深度拡張

新ピラー化せず、**既存ピラーの中で具体的に肉付け**。

#### Pillar H 音響 — `AudioDirector` → **`AudioSystem`** に拡張・改名
- **2D positional audio** — `Camera2D` をリスナーに、ソース毎の位置/min_distance/
  max_distance/Rolloff（Linear/Log/InverseSquare/Custom curve）、定電力パン。
- **Voice prioritization & stealing** — `priority * 256 - audible_volume * 255 - age` で
  スコア化、最低を奪う。
- **DSP-per-bus** — LPF/HPF/EQ3/Reverb/Limiter（XAudio2 内蔵を活用、各バス最大 4 段）。
- **Reverb zones** — AABB + プリセット（Cave/Hall/SmallRoom/LargeRoom/Outdoors/Underwater/
  Sewer/Chamber）+ 境界フェード + 優先度。
- **Audio snapshots** — バス状態のスナップショット（`"underwater"`/`"menu"`/`"combat"`）+
  クロスフェード（dB 線形）。「水中に潜る」の典型がこれで 1 行。
- **`MusicDirector` は `AudioSystem` の内部モジュール**として残る（v7 設計の継承）。
- v1.1: occlusion（raycast 遮蔽 + ラウンドロビン予算）。

#### Pillar E カメラ — `Camera2D` → **`CameraStack`** で複数カメラ統括
- **複数カメラ** — gameplay + UI + minimap + inset、`priority` 順描画、`LayerMask` で
  カリング、per-camera viewport/scissor、`Camera2D::bypass_post`。
- **Split-screen** — `ConfigureSplit(HorizontalHalf/VerticalHalf/Quad, player_count)` 1 行で
  自動配置。マルチリスナー音響は v1 で「平均位置」（多チャネル分離は v2）。
- **Render-to-texture** — `Camera2D::SetRenderTarget(IRhiTexture*)`。minimap/mirror/
  security-cam/portal/in-world モニタ。GPU リソース寿命は既存 3-frame 遅延削除キュー経由。
- **トランジション** — Cut/Fade/PushLeft/PushRight/WipeIris/WipeDiagonal（6 シェーダ
  ~200 行）+ Pillar I `CutsceneTrack` 連動。
- **`ParallaxLayer`** — `factor` 宣言で `ResolveTransforms` が自動視差解決。`SpriteBatch`
  無変更。
- v1.1: per-camera post-process stack（`RenderPassRegistry` 成熟後）。

#### Pillar D 入力
- **デバイスホットプラグ + active-device tracking** — XInput 接続/切断イベント、
  `LastActiveDevice` で UI が glyph セット自動切替（KB&M ↔ Xbox ↔ PS）。
- **`PlayerSlots`** — 最大 4 スロット、各スロットに `InputMap` インスタンス、auto-join
  ("Press any button to join") — Split-screen の前提。
- **バッファ入力窓** — `SetActionBuffer(action, seconds)` + `ConsumeBufferedPress` で
  Starter2D ジャンプバッファとインフラ共有（DRY）。
- **`InputRecorder`** — 入力ストリームキャプチャ（Pillar K Replay と統合、**Pillar M
  lockstep のリプレイも同じ `.acsr` 形式**）。
- **`InputMap::BeginRebind`** — 次の物理入力を捕獲してアクションへバインド（capture
  プリミティブ）。実 rebind UI は UiKit 待ちで v1.1、ジェスチャ認識も v1.1。

#### Pillar A/H 描画拡張（キーストーン）
- **`RenderPassRegistry`** — `PostProcess` の 5 phase（`BeforeSceneOpaque` /
  `AfterSceneOpaque` / `AfterScene` / `AfterBloom` / `AfterTonemap`）にユーザーが
  フルスクリーン PS を挿入可能。CRT/scanlines/posterize/LUT/heat-haze が**ゲーム側コード**
  で書ける（フレームワーク無変更）。ping-pong RT は遅延確保。
- **`SpriteMaterial`** — スプライト毎の代替 PS。組込み 3 種（Outline/Dissolve/Flash）で
  2D ゲーム素材ニーズの 80%。バッチは material 切替で flush（明示）。
- **`Light2D`** データ構造は v1（point/spot/directional + normal-map slot）、**ソルバ
  実装は v1.1**（G-buffer + 2D shadow polygon、~600 行 HLSL）。これがあるかないかで
  2D ゲームの見た目が桁違い。
- **GPU instancing 検証** — `HelloBullets` で現バッチパスの上限を測る、超えるなら専用
  `DrawInstanced` を追加。50k スプライト/60fps を見込む。

#### Pillar H procgen ツールキット
- **`RandomChannels`**（基盤）— 名前付き PRNG ストリーム（`"world"`/`"visuals"`/`"audio"`/
  `"ui"`）。**`Random::Global()` で gameplay と visuals が混ざる問題を構造的に解消** —
  Pillar M lockstep と Pillar K replay の決定論基盤。`SaveArchive` 経由で snapshot/restore。
- **`Noise`** — Perlin/Simplex/Value/Cellular + Fractal、stateless ~300 行。
- **`Distributions`** — Poisson-disc / Weighted / Gaussian / LogNormal / `WeightTable`
  （O(log n) ピック）。
- **`MarkovNames`** — マルコフ連鎖人名/地名生成、CJK code-point 対応、訓練済みモデル `.mdl`。
- **`Dungeon`** — BSP partition / corridor / drunken walk / A* corridor / connectivity
  verify。5 関数 ~400 行、合成はゲーム側。
- v1.1: Wave-Function-Collapse（~1000 行、`Tilemap` 出力に依存）、Time-of-Day/Weather
  （ジャンル特定）。

### 13.7 全体アーキテクチャ — v8 で 14 ピラー

| 番号 | ピラー | 追加版 |
|---|---|---|
| A | App & Scene | v3〜 |
| B | オブジェクトモデル（Node2D + ECS） | v3〜 |
| C | 時間・アニメ | v4〜 |
| D | 入力（+ホットプラグ/PlayerSlots/buffer/recorder） | v4 + **v8** |
| E | カメラ（`CameraStack` 化、split/RTT/transition/parallax） | v4 + **v8** |
| F | 物理・衝突 | v4〜 |
| G | リソース・永続化（**Pillar J で構造的解放**） | v4 + **v8** |
| H | UI・音・ツール（`AudioSystem` 拡張・`RenderPassRegistry`・procgen） | v4 + **v8** |
| **I** | エフェクト / VFX / ゲームフィール | v7 |
| **J** | シリアライズ / リフレクション / プレハブ | **v8** |
| **K** | エディタ / 開発ツール | **v8** |
| **L** | AI / ゲームプレイ深度 | **v8** |
| **M** | ネットワーク / マルチプレイヤー | **v8** |
| **N** | Mod / スクリプト | **v8** |

加えて v7 §12.2 の完成度システム 9 つ（UiKit/Dialogue/MusicDirector/Progress/
LocalizationDirector/Accessibility/Starter2D/SaveSlots/GameFlow）は維持。

### 13.8 実装フェーズの再構成

v6/v7 の 11 フェーズに以下を挿入・追加:

| Ph | 内容 | サンプル |
|---|---|---|
| **7.5** | `RandomChannels` + Procgen ツールキット（replay/netcode の前提） | `36_HelloProcgen` |
| **9.5** | `AudioSystem` 拡張（positional/DSP/zones/snapshots） | `33_HelloEffects` 拡張 |
| **9.7** | `CameraStack` + RTT + transitions + parallax + split-screen | `37_HelloMultiCam` |
| **10.3** | `InputRecorder` + `PlayerSlots` + buffered input + hot-plug | `34_HelloDialogue` 拡張 |
| **10.5** | `RenderPassRegistry` + `SpriteMaterial`（Outline/Dissolve/Flash） | `38_HelloShaders` |
| **12** | Pillar J 反射基盤（`TypeInfo`/`Register`/walker）+ ラウンドトリップテスト | (テストのみ) |
| **13** | Pillar J `.atxt`/`.abin` + `SaveArchive` 互換 + HMAC | `39_HelloSerialize` |
| **14** | Pillar J `SceneSerializer` + 参照解決 | (29 を `.scene` 起動に移植) |
| **15** | Pillar J `Prefab` + override + nested | `40_HelloPrefab` |
| **16** | Pillar J `DataAsset` + ホットリロード（再起動） | (39 拡張) |
| **17** | Pillar J `SaveSlot<T>` 統合 | `41_HelloSaveSlots` |
| **18** | Pillar K Inspector + DevConsole + Profiler | `42_HelloEditor` |
| **19** | Pillar K HotReload + Replay + LiveTune | (18 拡張) |
| **20** | Pillar L BehaviorTree + Pathfinding + Tilemap | `43_HelloAI` |
| **21** | Pillar L AnimationGraph + Perception + SpatialQuery + Trigger + Steering | (20 拡張) |
| **22** | Pillar M UDP transport + `LanLobby` + HMAC リプレイ | `44_HelloReplay` |
| **23** | Pillar M `LockstepRunner` + 決定論コントラクト + state hash desync | `45_HelloLockstep` |
| **24** | Pillar M Replication 属性 + RPC + `acs_replay_verify` CLI | `46_HelloGhostRace` |
| **25** | Pillar N AssetPack overlay mod + manifest + `ModManagerScreen` | `47_HelloMods` |
| **26** | Pillar N Lua 5.4 組込み + サンドボックス + hot-reload | `48_HelloScripting` |
| **27 (v1.1)** | 2D lighting solver / WFC / ToD / occlusion / rebind UI / per-camera post | — |

Ph7.5 は他の前提（決定論基盤）。Ph12 は Pillar J 全体・Pillar K/M の前提。Ph22〜24 は
Pillar J Ph12〜13 の前提。
（v9 で 3D / 物理深度 / アニメ深度 / 出荷 / スケール / ジャンルキットを追加 — §14）

---

## 14. v9 — さらに練り：7 領域の深掘り

ユーザー指摘「それだけじゃなくもっと練って」を受け **7 エージェント並列**で深掘り。
**2 つの新ピラー O / P と、既存ピラーへの大規模深掘り 5 領域**を確定。
全設計の原典は別途エージェント成果物に保持し、本書は統合的目次。

### 14.1 3D サポート（既存 Pillar B/E/F/H 拡張）

**スコープ判断: (d) 2D primary + opt-in 第一級 3D layer**（Unity 流）。`ESvc::Scene3D|
Camera3D|Physics3D|Lighting3D` を `WantedServices` で課金式 opt-in、メニュー画面に
3D コストを払わせない。

- **Object model**: `Node3D`+`Transform3D{FVec3 pos, FQuat rot, FVec3 scale=44B}`+
  `Component3D`。**`Node2D`/`Node3D` は別ツリー・共通基底なし**（座標規約・dirty
  伝播が別物、抽象は害）。`Scene` が `Root2D()`/`Root3D()` を持つ。
- **Camera3D** — perspective/ortho、4 種 Rig (`Fps`/`Orbit`/`Follow`/`Cinematic`)、
  `ScreenToRay` でピック。**`CameraStack` が 2D/3D 統合の唯一の点** — `Layer{kind=
  World3D/World2D/Ui2D}` を priority 順に描画、3D+HUD/2.5D/billboard 全パターン対応。
- **Collision3D** — `Loose Octree` broadphase + **SAT 直書き 15 ペア** narrowphase
  （FAabb/FSphere/Capsule/Obb/Mesh）。GJK/EPA は v2。`PhysicsBody3D` は swept kinematic
  + collide-and-slide（rigid body solver は v2）。
- **Lighting** — `Directional/Point/Spot/Area/Probe` 5 種 Light Component + `LightManager`
  がフレーム頭で集めて既存 `PbrShader`/`SkinnedShader` に push。IBL は `SkyboxComponent3D`
  経由。Lightmap baker は v1.1。
- **Animation 3D** — `SkinnedMeshComponent` + 既存 `AnimationPlayer` をラップ。Pillar L
  `AnimationGraph` が **2D `SpriteAnimator` も 3D skinned も共通基盤**で扱う。
- **2D/3D 共存**: `BillboardComponent`（Doom 風）、`WorldSpaceCanvas3D`（3D 空間の
  2D UI — HUD/看板/フローティングダメージ数字）、Audio listener も `CameraStack`
  最高 priority に自動追従。
- **v1 = Ph28〜31, ~4300 LOC, サンプル `49_HelloNode3D`〜`52_HelloAnim3D`**。
  v1.1 で `NavMesh3D`/`AnimationGraph3D` blend tree/CSM/Gizmo3D。v2 で rigid body
  solver/GJK/DXR。

### 14.2 物理深度（Pillar F 拡張 — F2 サブピラー）

**判断: Box2D をラップせず自前実装**。Box2D は STL/exception を前提とした
`b2BlockAllocator`/`b2Assert→throw` 設計で、no-STL/no-exception/no-RTTI/`TResult<T,E>`
の ACS に「shim」を被せると実質的に fork になり、4k LOC を節約できない。
4,650 LOC で完全な PGS 系ソルバを書ける（Catto GDC 2006/2014、qu3e/Chipmunk 参考）。

- **Solver**: warm-started **Sequential Impulse PGS** + Island formation + 8/3
  iter（速度/位置）+ split-impulse 位置補正 + Baumgarte。サンプル時点 ~3500 行。
- **Broadphase**: kinematic は既存 `SpatialGrid` 継続、dynamics 用に **Dynamic AABB Tree**
  追加（sleeping body はゼロコスト、可変サイズ shape に強い）。
- **Shape**: Circle/Box/Polygon(凸 8 頂点)/Capsule/Edge/Chain(ghost vertex 付き)/
  Compound。凹は `DecomposeConvex`（Bayazit）で自動分解。
- **Joints**（8 種）: Revolute/Prismatic/Distance/Weld/Rope/Wheel/Pulley/Gear。
  motors+limits+**break impulse threshold**（「ロープが切れる」）+ Soft（freq/damping）。
- **CCD**: TOI + conservative advancement + GJK 距離。**Bullet flag** ＆ 自動
  （fast dynamic-vs-static）。Box2D 互換アルゴ。
- **Queries**: `RaycastFirst/All/Callback`, `ShapecastFirst`, `QueryAabb/Point/Fan`。
  fan/cone は raycast で実装、Pillar L `Perception` 視界コーンと統合。
- **Sensors** + **OneWayPlatform** ヘルパ + **Contact callbacks** (Begin/Persist/End/
  Pre/PostSolve) で Pillar I Effects と接続。
- **決定論コントラクト** — fixed iter 順 + body sort by BodyId + `/fp:precise` +
  ACS 決定論 sin/cos + per-step `StateHash`。Pillar M lockstep が直接利用。
- **Soft body seam** — v1.1 で Verlet cloth/rope (~300 LOC)。SPH 流体は明示的に非採用。
- **v1 = Ph17.5〜17.9 で 3 段階導入**。v8 既存 `PhysicsBody2D`（kinematic）と**共存** —
  Starter2D character controller は kinematic のまま、barrel/crate は dynamics で。

### 14.3 アニメーション深度（Pillar C/L 拡張）

**4 層パイプライン**で統一: `Sampler → Graph → ProceduralLayer → Output`。
`Pose` を中心データ型に。

- **`SkeletonAsset` を `SkinnedMeshAsset` から分離** — 共有スケルトン/ retarget の前提。
  `HumanoidSlot` enum で「人型」を標準化。
- **`ISamplerRuntime` 抽象** + 4 実装: `ClipSampler`（ACS 標準 `.skanim`）、
  **`SpineSampler` / `DragonBonesSampler`**（外部ライセンス回避のため独立モジュール
  `ACS::SpineBridge`/`ACS::DragonBonesBridge` で seam だけ提供、ランタイムは
  ユーザーが import）、`FlipbookSampler`（既存 `SpriteAnimator` ラッパ）。
- **`AnimationGraph` 詳細化**: `ClipNode/Blend1D/Blend2D/BlendAdditive/LayerMix/
  StateMachine/Reference`、ノードを flat 配列 + index 参照 → Pillar J で
  シリアライズ容易・ホットリロード対応。**Inertialization** 遷移を v1 で
  （UE5 流、~200 行で品質激変）。
- **ProceduralLayer**: 順序付き Pose 修正パイプ。`HeadLookComponent`（look-at）、
  **`IkConstraintComponent`**（2-bone analytical/FABRIK/CCD 切替）、`FootIkComponent`
  （raycast で地形対応 + hip adjust）、`BreathingComponent`（additive sin）、
  `LeanIntoTurnComponent`、`HandIkComponent`（インタラクト）。
- **Ragdoll**: `Animated⇄Limp⇄GettingUp` 3 状態。Animated 中は rigid body が
  kinematic で骨に追従（被弾リアクション可）、Limp は **Pillar F joint system**
  （F2 設計の Revolute/Cone joints）で rigid body 駆動、GettingUp は現姿勢を 1 frame
  clip にして getup_anim へ crossfade。骨の breaking impulse で「腕がもげる」演出も。
- **Morph Targets** — 顔表情/衣装。CPU 適用 v1（頂点 ~500 まで実用）、GPU SRV +
  `MorphSkinnedShader` v1.1。**2D pixel art は対象外**（明示）、Spine の mesh
  attachment で同等を達成済。
- **Root Motion** — `bone_index=-1` 特殊 channel から TRS delta 抽出、`PhysicsBody2D`
  経由で collide-and-slide（直接 Transform 書きを禁止）。
- **Animation Event** — clip 内 `(time, kind, hash_or_code, payload)` 配列。
  `AnimationEventDispatcher` が Pillar I `EffectSystem` / Pillar H `AudioSystem` /
  `MessageBroker` に振り分け。OncePerLoop dedup ポリシーで dup 発火を防ぐ。
- **Motion Matching** — `IProceduralAnimNode` seam を v1 で予約、kd-tree 実装は v2。
- **Retargeting** — `RetargetMap` 型のみ v1、HumanoidSlot 経由の bone 対応・骨長
  スケール補正は v1.1。

### 14.4 Pillar O — 出荷 / Live Ops（新規 15 番目のピラー）

v8 までは dev-time 完備だが、**dev→ship→patch→live ops** のループ全体が未設計。
新エンジンモジュール **`ACS::Bake`** + GameFramework 側ピラー O + `tools/` CLI 群 +
CI workflow を追加。

- **Asset bake パイプライン** — `acs_bake` CLI（`acs_assetpack` の sibling）。
  - texture: BC1/BC7/ASTC（mip 自動生成、normal map 専用エンコード）
  - audio: Ogg Vorbis/Opus（loop point 保持）
  - font: TTF→atlas baking、CJK 4096² 範囲指定
  - mesh: glTF/FBX → ACS `.mesh`（meshlet 化 v1.1）
  - data asset: `.atxt` → `.abin`（Pillar J）
  - hash ベースインクリメンタル（変更ファイルのみ再 bake）
  - マルチプラットフォーム output（BC* desktop / ASTC mobile&Switch）
- **Build configurations**: `Debug/Dev/Profile/Ship` プリセット、何が strip されるか
  を構成ごとに明示（Pillar K dev tools / Pillar J `.atxt` reader / 単体テスト 全部
  Ship で空展開）。Ship をうっかり Debug でビルドすると loud fail。
- **Packaging / installers**:
  - Windows: `.zip` portable + MSIX + Inno Setup スクリプト、Authenticode 署名 seam
  - Steam: `steamcmd` upload script + depot config
  - itch.io: `butler` 連携
  - コンソール: SDK 個別 seam（NDA で名前は出さない）
- **CI integration** — GitHub Actions matrix（MSVC + clang-cl × Debug/Ship × x64/ARM64）、
  unit test run（Pillar K Replay 駆動）、artifact upload、git SHA を binary に埋め込み。
- **Telemetry** — `ITelemetryClient` 抽象、game が `Event("level_completed", {id, sec})`
  を発火、バッチ送信、opt-in / GDPR aware、no PII default。Steam/EOS/custom HTTP プラグイン。
- **Crash reporting** — Win32 `SetUnhandledExceptionFilter` → `MiniDumpWriteDump` →
  symbol upload（自前 viewer or Sentry/Bugsplat seam）。Pillar J でゲーム state snapshot
  も同梱可能。
- **Feature flags / Remote config** — `IFeatureFlagClient` 抽象、サーバから push、
  オフラインキャッシュ、A/B テスト seam。
- **Live ops / patching** — **コンテンツパッチは exe 再 download なし**で AssetPack
  overlay として配信、DLC は別 `.acpak`。セーブ互換は Pillar J `MigrationRegistry`。
- **Localization for shipping** — `LocalizationDirector` の runtime 上に翻訳者
  workflow（`.po`/`.csv`/Crowdin seam）、missing-key レポート、pseudoloc 強化。
- **配信アーキタイプ** — 無料デモ+本編 / 無料+DLC / Early Access / 無料+IAP の
  4 つを 1 級サポート。
- **モジュール: `src/gameframework/shipping/`** + **`tools/acs_bake/`**, **`tools/acs_telemetry/`**, **`tools/acs_crashview/`**。

### 14.5 Pillar P — スケール / ストリーミング / メモリ&スレッド深度（新規 16 番目）

v8 までは「**何ができるか**」、v9 Pillar P は「**大きくなっても倒れない**」ための層。

- **World / Chunk** — 2D グリッド上のチャンク（`Tilemap` をラップ）。`ChunkResidency`
  状態機（Unloaded→Loading→Resident→Unloading→Ghost）。**Ghost** = 遠方チャンクを
  低解像度プリベイク画像 1 枚で描く（「山頂から大陸全体が見える」を 50MB GPU で実現）。
  チャンク persistence は Pillar J `SaveArchive` 拡張、**save game = dirty chunks の
  flush**（Skyrim 化を防ぐ）。
- **Async asset streaming** — 既存 `LoadAsync` の上に `AssetStreamer`（5 段優先度キュー
  + per-frame I/O budget + LRU eviction + pin/unpin）。**Mip streaming** v1.1
  （per-mip 部分常駐 → GPU メモリ 4〜10x 削減）。
- **LOD**: sprite mip 自動選択 + sprite swap（near/far）+ **behavior LOD**（far AI は
  tick stride 増）+ animation LOD（off-screen pause / sample stride）。**物理は LOD
  しない**（tunneling 防止）。
- **Large-world coordinates** — **origin rebasing** 採用（`Vec2d` 全置換は f32 shader/
  collision の連鎖変更で v1 不可）。チャンク座標 (cx,cy)+ 局所 FVec2 のハイブリッド
  固定点が副産物。`WorldOriginRebased` event で subscriber が補正。
- **Memory 深度**:
  - **Scene arena** — `Scene` がアリーナ所有、退出時 1 op で reset。trivially destructible
    component が opt-in。
  - **`TPool<T>`** — Pillar H 確定。Pillar I Effects/Pillar M ghosts/Pillar L A\* node
    で利用。
  - **`GpuMemoryBudget`** — texture/mesh GPU bytes 追跡 + evict 候補返却（自動 evict
    せず policy は `AssetStreamer`）。
  - **AllocTracker (debug)** — Pillar K Profiler HUD で per-`EAllocKind` 表示、
    leak 検出。
  - **`FrameAlloc<T>`** — `Temp` segment 簡易 API、フレーム頭で auto-reset。
- **Threading 深度**:
  - ThreadPool は既に Chase-Lev work-stealing。追加: **per-worker hi/lo deque**（30 行）、
    `ParallelForDynamic`、Pinned task、worker stats。
  - **C++20 coroutines `Task<>`** — カスタム allocator が Scene arena から取る。
    `co_await WaitSeconds(0.5f) / WaitForEvent / Sequence / NextFrame / LoadAsset`。
    cutscene/dialogue を「読めるコード」として書ける。**Fiber は不採用**（help-stealing
    で代替可能、debugger/ASAN 互換性のコスト過大）。
  - **`MpmcRing<T>` / `SpscRing<T>`** lock-free（80/40 行）。`MessageBroker` は
    single-threaded 維持、cross-thread は ring 経由で drain。
  - **IO thread** 別建て（Asset 読込/暗号化を long-blocking で workers から隔離）。
  - **決定論コントラクト** — threaded subsystem は固定順 reduction、`ACS_NET_DETERMINISTIC`
    で serial 強制。
- **FrameGovernor** — frame target 超過時、優先度低い順に degradation: DebugDraw →
  DistantBgm/Sfx → Particles → AiPerception → AiBehavior → PostProcess → AssetStreaming
  → UiAnimation。**物理/入力/シーン遷移は絶対 throttle しない**。Pillar K Profiler HUD で
  tier 状態表示。
- **スケール目標**: 9×9 active chunks (~330k tiles), 1000-2000 dyn entities, 50k batched
  sprites, Switch 50MB GPU / PC 1.5GB game-state CPU。

### 14.6 Pillar N 本設計（v8 §13.5 の sketch を本設計に格上げ）

- **Lua 5.4 推奨**（LuaJIT は maintained 終了、5.4 は portable・小型・十分）。
  longjmp は `lua_pcall` ラップで `TResult<T, LuaError>` に変換、no-exception 維持。
  **複数 Lua state**（mod ごと別 state でサンドボックス分離）、カスタム allocator は
  ACS arena。
- **C++↔Lua バインディング自動化** — `AcsLuaBind<&Fn>::Bind(L,"name")` テンプレで
  stub 生成、**Pillar J 反射経由で `TypeInfo<T>` のフィールドが自動公開**。userdata は
  非所有参照、metatable で型分離。
- **`LuaComponent2D`** — Lua 関数で `OnUpdate`/`OnEvent` 実装、per-instance Lua table
  に状態。ホットリロードで `self` table は保持、function ポインタだけ差し替え。
- **サンドボックス深度**:
  - API allowlist（`io`/`os`/`debug.*`/`package.loadlib` 削除、`acs.fs.readSavefile`
    等の curated wrapper のみ提供）
  - メモリ quota（カスタム allocator が bytes track）
  - CPU quota（`lua_sethook(LUA_MASKCOUNT, 10000)` で frame 内 instruction 上限）
  - 信頼レベル: `LocalUntrusted / CommunitySigned / OfficialTrusted` 3 段階
- **Mod システム本設計**:
  - **`manifest.toml`** スキーマ（id/name/version semver/author/description/target framework
    version/depends/signatures/declared APIs）
  - 依存解決アルゴリズム（topological sort + 循環検出 + version range 充足）
  - 衝突検出（同 asset 上書き → error/warn-pick-last/merge policy）
  - 署名検証 — AssetPack `Crypto::HmacSha256` 再利用、未署名は信頼レベル降格
  - ロード順 — 内蔵最低 → 有効化 mod を依存順 → UI 手動再順序付け
- **`ModManagerScreen`** UiKit 構成 — list/detail/conflict/load-order/install 5 screen。
- **`IModRepository`** 抽象 + default `LocalFolderRepository`、Steam Workshop/mod.io/
  itch.io は v1.1 で concrete impl。
- **Tier 別**: A(asset-only) → B(data + script) → C(total-conv) → D(C++ DLL 非推奨)。
- **パフォーマンス規律** — script から C++ への呼び出しは**bulk** で行うイディオム
  （per-entity ループは C++ 側）、~100ns/call のオーバーヘッドを文書化。
- **DAP debugger seam** — v1 で API stub、v1.1 で実装（VSCode/ZeroBrane 接続可能に）。
- **save 互換** — mod 抜きで modded save をロードしたら graceful degradation か
  lock-out を mod が宣言。

### 14.7 ジャンルキット 7 つ（`Starter2D` 拡張・コンテンツ層）

新ピラー化せず、`Starter2D` (v7) と同じ品質バーで「ジャンル形状のレシピ」を 7 つ追加。
すべて既存ピラー A〜N + 完成度システム上に構築、新エンジン primitive ゼロ。

| Kit | namespace | 主特徴 | サンプル |
|---|---|---|---|
| **VN** | `acs::game::vn` | Stage(BG+5 portrait slot)+typewriter+log/auto/skip+**save-per-choice rewind**+read-state tracking+voice routing+CG gallery | `49_HelloVN` |
| **Roguelike** | `acs::game::rogue` | Turn order+AP+grid mvt+**shadowcaster FOV**+map memory+identification(global persist)+auto-explore/attack+meta-progression | `50_HelloRoguelike` |
| **Tactical** | `acs::game::tactical` | Grid board(W×H×Z)+initiative queue+BFS reachability+**AOE preview**+nested ActionMenu+AI ply(BT)+StatusEffectRegistry(Cards と共有)+PartyScreen | `51_HelloTactical` |
| **SHMUP** | `acs::game::shmup` | **Bullet pattern DSL**(Concentric/Spiral/Aimed/Wave/Burst を operator で合成)+ECS(50k bullets)+graze+chain+phased boss+**replay-friendly** | `52_HelloShmup` |
| **Rhythm** | `acs::game::rhythm` | **Audio sample-clock 駆動 BeatClock**+Perfect/Great/Good/Miss+combo+grade+`.chart`+`.osu` import+calibration screen+latency tuner+4/5/7/DJ lane configs | `53_HelloRhythm` |
| **Cards** | `acs::game::cards` | Deck+Hand+stack-based effect resolution+targeting(line+AOE)+StatusEffectRegistry(Tactical と共有)+**seeded shuffle**+intent display+RewardScreen+DeckBuilder | `54_HelloCards` |
| **Idle** | `acs::game::idle` | **BigNumber**(128-bit + 任意精度)+Scientific/Suffix/NamedTier formatter+offline progress(geometric series)+producer/upgrade tree+prestige+**low-CPU on unfocus** | `55_HelloIdle` |

**キットでない明示リスト**: MMO/Souls-like/Survival craft/FPS/MOBA/Racing/RTS/Sports/
Sandbox-sim/Match-3/Fighting/Walking sim — ジャンル形状が bespoke すぎる or 既存組合せ
で表現可能 or 3D/ネット v2 待ち。

**共有不変条件**: 全キットで `RandomChannels` 命名規約・`.acsr` リプレイ・コンテンツ
配置 `assets/<genre>/<category>/*.tdat`・hot-reload・no-STL/exception/RTTI・正準
`Callback`。

### 14.8 全体アーキテクチャ — v9 で 16 ピラー

| 番号 | ピラー | 追加版 |
|---|---|---|
| A | App & Scene | v3〜 |
| B | オブジェクトモデル（Node2D + ECS + **Node3D**） | v3 + **v9** |
| C | 時間・アニメ（**4 層パイプ + IK + Ragdoll + Morph + Root Motion**） | v4 + **v9** |
| D | 入力（hot-plug/PlayerSlots/buffer/recorder） | v4 + v8 |
| E | カメラ（**`CameraStack` 3D 統合 + 4 Camera Rig**） | v4 + v8 + **v9** |
| F | 物理・衝突（**F2: PGS dynamics + 8 joints + CCD**） | v4 + **v9** |
| G | リソース・永続化（Pillar J で構造的解放、**Pillar P で streaming**） | v4 + v8 + v9 |
| H | UI・音・ツール（AudioSystem + RenderPassRegistry + procgen + **5 light + Skybox**） | v4 + v8 + **v9** |
| **I** | エフェクト / VFX / ゲームフィール | v7 |
| **J** | シリアライズ / リフレクション / プレハブ | v8 |
| **K** | エディタ / 開発ツール | v8 |
| **L** | AI / ゲームプレイ深度（**+ NavMesh3D v1.1**） | v8 + **v9** |
| **M** | ネットワーク / マルチプレイヤー | v8 |
| **N** | Mod / スクリプト（**Lua 5.4 本設計**） | v8 + **v9** |
| **O** | **出荷 / Live Ops**（asset bake/CI/telemetry/crash/feature flag/patch） | **v9** |
| **P** | **スケール / ストリーミング / メモリ&スレッド深度** | **v9** |

加えて v7 §12.2 の完成度システム 9 + v9 §14.7 ジャンルキット 7（VN/Roguelike/
Tactical/SHMUP/Rhythm/Cards/Idle）。

### 14.9 実装フェーズの再々構成

v8 までの ~27 フェーズに以下を挿入・追加（v9 で合計 ~50 フェーズ）:

| 区分 | Ph | 内容 |
|---|---|---|
| F2 物理 | **17.5〜17.9** | Shape/Body/Material/Filter/AABB tree → Joints/motors → CCD/sensors/queries |
| 3D | **28〜31** | Node3D/Transform3D → Camera3D/CameraStack 3D/Light 5/MaterialInstance/MeshComponent → CollisionWorld3D/PhysicsBody3D → SkinnedMesh/Billboard/WorldSpaceCanvas3D |
| Pillar O 出荷 | **35〜37** | acs_bake CLI + Build configs → Telemetry + Crash + Feature flags → Packaging + Live Ops patch |
| Pillar P スケール | **38〜40** | World/Chunk/Residency/Ghost → AssetStreamer/LoDController/GpuMemoryBudget → Coroutines/Lock-free/FrameGovernor |
| アニメ深度 | **41〜42** | Sampler/Pose/Graph 詳細化 + Inertialization → ProceduralLayers + Ragdoll + MorphTarget |
| ジャンルキット | **43〜49** | VN → Roguelike → Tactical → Idle → Cards → SHMUP → Rhythm |
| **v1.1 全部** | **50〜** | 2D lighting solver / WFC / NavMesh3D / Mip streaming / Spine bridge concrete / Motion Matching / Steam Workshop / 各種残 |

**前提依存**: Pillar J (Ph12) → 全部の構造的前提。Pillar O Ph35 は Ship 配信の前提。
Pillar P Ph38 は worlds 物の前提。Ph41 (Sampler 統合) は v8 Pillar L AnimationGraph (Ph20)
の前提。各ジャンルキットは独立、ただし Tactical と Cards は StatusEffectRegistry を共有。
（v10 で Pillar Q/R + メタ層 + 著作ツール + 移植性 seam を追加 — §15）

---

## 15. v10 — さらに広い視野で練り：5 領域

ユーザー指摘「ライティングとかは？もっと広い視野で練って」を受け **5 エージェント
並列**で深掘り。**2 つの新ピラー Q / R + メタ層 + 著作ツール深度 + 移植性 seam** を
確定。詳細はエージェント成果物（~330KB）に保持し、本書は統合的目次。

### 15.1 Pillar Q — 視覚世界 / ライティング / 雰囲気（新規 17 番目）

エンジンは PBR/IBL/SSR/SSGI/SSAO/lightmap baker/area light/bloom/tonemap を**既に完備**
だが、GameFramework が「1 級にラップ・公開」する層が未設計だった。**2D lighting
solver を v1.1 → v1 に格上げ**:

- **2D dynamic lighting** — deferred 2D（color + normal + emissive G-buffer → lighting
  pass で N point/spot/area 光源合成）+ **光源 cookies/gobos**（窓ブラインドや葉影の
  texture マスク）+ **2D 影**（collision polygon から visibility polygon ~300 LOC）+
  **sprite normal map**（手描き or alpha からの自動高さマップ）。Defold/Unity 2D
  Renderer/Godot 2D 級。
- **`AmbientDirector`**（v8 v1.1 → v1 昇格） — 時刻 (1 in-game hour = N real sec)、
  天気 (`Rain/Snow/Wind/Storm/Clear` × intensity)、季節 (視覚 + 音響バイアス)、
  雷タイミング。日の出/正午/日没/夜のキーで滑らかブレンド（光色 + 環境光 + 空色 +
  fog density）。
- **Mood snapshots** — 領域ごとの完全ルック（lighting override + audio snapshot + LUT +
  particle ambient）。区域に入るとブレンド。
- **天候エフェクト** — rain (particle + wet shader + audio + 雷光)、snow (particle +
  frost edge)、wind (foliage を vertex shader で `wind*sin(time+pos.x)` 揺らす、
  AI 知覚の音遮蔽にも影響)、parallax cloud layers。
- **Screen effects 深度** — DoF (2D pseudo via depth-layer blur)、motion blur (per-object
  velocity buffer driving directional blur)、lens flares (additive bright-sample +
  lens-dirt texture)、anamorphic streaks (horizontal bloom stretch)、chromatic
  aberration の radial/edge モード、**LUT3D 重ね** (per-area + per-ToD + per-mood)、
  **HDR display 出力** (HDR10/scRGB)、artistic tone curves (Filmic/ACES/AgX 選択)。
- **Decals / sprite destruction / world response** — sticky 投影 decals (blood/scorch/
  paint/footprint in snow/sand) + decal pool + fade、**sprite alpha-mask 削り取り**
  (Metal Slug 風、per-sprite RT に bullet hit で paint)、**foliage 反応** (vertex shader
  push、局所速度フィールド)。
- **Water rendering** — 2D water tile shader (refraction + waves + reflection +
  caustics) + 水中 tint + bubble + reverb zone 連動。
- **Sky / cloud / volumetric** — parallax cloud layers + procedural cloud movement +
  day-color sky gradient + sun/moon/stars/aurora、**godrays** (screen-space radial blur
  from sun pos)。
- **Lighting authoring** — Pillar K Inspector に **light gizmo + shadow viz + lit/unlit
  toggle**、static light bake (既存 `LightmapBaker` 流用)、**light probes** で sprite
  ambient sample。
- **Particle lighting** — particle が scene light を読んで自身を shading（Pillar I
  Effects 統合、足元 dust が太陽光で照明される）。
- **5 ステップ設計者 workflow**: ambient → sun → area moods → per-area lights →
  bake static。
- **モジュール `src/gameframework/vis/`**、namespace `acs::game::vis`。

### 15.2 Pillar R — Polish & Game Feel（新規 18 番目）

「プレイヤーが感じる完成度」の層。**Pillar Q と同じく conductor 役** — 既存 pillar を
借りて作曲する。各サブシステムは `ESvc::*` opt-in（idle ゲームに photo mode コストを
払わせない）。

- **Slow-motion / time-control 深度** — `Node2D::SetTimeScale(0.5f)`（per-node、
  MAX Payne 風 bullet time）、time scale curves（Sequence で 0→1 ramp）、**UI/menu は
  unfrozen のまま世界 freeze**、Replay-aware (time_scale も記録)、multiple time domains
  (gameplay/UI/audio/particle 独立)。
- **Cinematics 深度**（v7 `CutsceneTrack` を拡張） — composition helpers (rule-of-thirds
  / golden ratio / headroom / leading room gridlines)、composition rig templates
  (Two-Shot/Over-the-Shoulder/Close-up/Wide)、**Cinemachine 級 virtual camera blend
  graph**（複数 cam + priority + blend、Pillar E `CameraRig`）、letterbox transitions、
  subtitle channel、universal skip with confirm、cinematic event hooks (HUD off /
  SFX mute 自動)。
- **Photo mode** — pause + free camera (3 axis pan + zoom + rotate)、filters (Pillar O
  color grading 経由)、HUD/entity hide、EXIF metadata 付き screenshot to user pictures、
  **`IPhotoShare` seam** (Steam/social 連携 plug)。
- **Tutorial / onboarding** — `TutorialFlow` data-driven、highlight target (dim + pulse
  outline via Pillar I)、tooltip overlay、completion predicate (`when player_jumped →
  advance`)、contextual hint queue (throttled、dismissible)、re-engage on update。
- **World reactivity** — reactive NPC (`notice player`/`alarmed` anim via Pillar L
  Perception)、environmental (birds fly away/grass bends/water ripples)、**footstep
  variation** (surface detect → SFX + particle/decal)、idle world animation。
- **Settings depth** — graphics presets (Low/Med/High/Ultra/Custom)、GPU auto-detect で
  初回 boot 時 preset 推奨、performance tier auto-detect (first-frame timing 測定)、
  per-feature toggle、brightness/gamma calibration screen、audio bus deep settings、
  mute on focus lost、OS-standard 保存先 (Pillar G `Storage`)。
- **Difficulty / accessibility-as-gameplay** — Story/Normal/Hard/Hardest preset + 
  Celeste 流 Assist Mode の granular slider、permadeath toggle (gameplay 層)、
  mid-run adjust、achievement 取得時 difficulty 記録。
- **Speedrun / streamer support** — in-game timer overlay + split + golden splits、
  **streamer mode** (royalty-free BGM swap + voice mute オプション)、auto-pause on
  focus lost、capture-friendly UI (spoiler-hide for first-time streamers)。
- **Boot / splash / first-run experience** — splash sequence (engine + publisher + game
  logo、初回視聴後 skip 可)、EULA/age gate (locale-aware)、first-run wizard
  (language/graphics/control scheme)、storefront account link seam、settings
  auto-detection (refresh rate/HDR/surround)。
- **Damage feedback** — directional damage indicators (FPS 風 arc overlay)、screen-edge
  tint、low-HP screen vignette pulse、**death cam** (slow-mo + zoom on killer)。
- **Pause 深度** — parallax pause (drifting blurred world、freeze だけでない)、quick-save
  / quick-load shortcut、quit confirm with save、return-to-title with save。
- **モジュール `src/gameframework/polish/`**、namespace `acs::game::polish`。

### 15.3 メタ層（横断的接合・**新ピラーではない・foundation 拡張**）

ピラーが 16 揃った今こそ「全 pillar を 1 プロダクトに見せる接合層」を v1 で確立
しないと遡及修正コストが指数的に増える。

- **`TypedHandle<Tag>` + `HandleRegistry<T,Tag>`** — v9 全体で散在する 9 種の
  `{u32 idx, u32 gen}` ハンドルを 1 つに統一。`EntityId`/`NodeId`/`BodyId` 等は型
  タグで混入時にコンパイル時 fail。`ACS_OPAQUE_ID(Name, Underlying)` で不透明 ID を
  1 行宣言。
- **決定論プロファイル** — `SubsystemTrait<Tag>::profile = Required/BestEffort/
  Unsupported` を全 pillar が宣言、`ACS_DET_REQUIRE`/`ACS_DET_FORBID_CALL` で
  コンパイル時に違反検出。`ACS_NET_DETERMINISTIC` ビルドで `Required` 内の
  `time()`/`Random::Global()`/`THashMap` 反復順依存を runtime assert で禁止。
  **CI ガード**: `acs_det_test record/replay/verify` CLI が `.acsr` を再生し state hash
  完全一致を強制。「決定論で使えない機能」を `docs/Determinism.md` に列挙。
- **テストフレームワーク `acs_test`** — STL/exception/RTTI 不使用 ACS-native。
  `ACS_TEST(category, name)` マクロ、6 種別: **unit / integration**（`HeadlessGame`
  + Pillar K Replay 駆動）**/ property**（`PropertyHarness` で seeded random）**/
  determinism / visual regression**（SSIM golden compare）**/ benchmark**
  （baseline.json で regression detect、CI fail）。JUnit XML 出力で CI 統合。
- **デバッグ体験** — 既存 `Assert`/`Log`/`Panic`/`SourceLoc` を活かし: `PanicAction`
  段階化 (LogOnly/Breakpoint/Dialog/Terminate)、`Logger` channel filter + runtime CVar
  切替 (`DevConsole` で `log_channel physics off`)、標準 channel 名
  `docs/LogChannels.md` 集中管理。**crash dump に Pillar J Scene snapshot 同梱**
  （クラッシュ時点のシーンを Inspector でロード可能）。
- **Performance reproducibility** — `ACS_PROFILE_SCOPE` を `foundation/diag/Profile.h`
  に下ろし weak hook で Pillar K Profiler が install (循環依存回避)、`PerfBudgetGuard`
  (scene 単位 frame budget assert)、`MemBudget` (scene-arena/GPU/audio/net 上限)、
  `.acsperf` dump format、`acs_perfanalyze` で 95p/99p 比較。
- **ドキュメント & メンタルモデル** — `docs/LearningPath.md`（サンプル 00→55 が学習
  順）、`docs/recipes/` 20 個 (platformer/dialogue/new-component/shipping)、
  `docs/Diagrams/` 手書き Mermaid、`docs/migrations/_template.md`。
- **Cross-pillar イベント taxonomy** — `gameframework/meta/Events.h` で v9 全 ~30
  イベントを 1 ヘッダで型 + チャンネル名宣言。`EventSink` RAII で Scene 退出時自動
  unsubscribe。
- **SemVer & deprecation** — `foundation/Version.h` constexpr 版、`ACS_DEPRECATED_SINCE`
  macro、`foundation/compat/v0_X/` で旧名 alias、`acs_migrate` CLI（v1.1）。
- **`docs/StyleGuide.md`** + **`acs_lint`** — clang-tidy custom check AST visitor で
  naming / `noexcept` / no-STL / no-exception / no-RTTI / lifecycle `On` 接頭辞 / event
  中央化 / handle 型安全 / locale 経由 / log channel 既知 等。v1 で 7 rules、CI gate。
- **移植性 seam audit** — `<Windows.h>` 直叩きを `platform/` 外で禁止 (`acs_lint`
  R015)。`PlatDir/PlatProcess/PlatCrash/PlatTime` 新 seam を foundation 公開。
- **License compliance** — `docs/Licenses/THIRD_PARTY.md` + `acs_licenses` ジェネレータ
  + `Game --licenses` + UiKit `LicenseScreen`。
- **Localization 完備監査** — debug ビルドで `Label::SetText` の生 string literal を
  検出、`acs_locale extract/check` CLI で未翻訳キー CI fail。
- **Privacy / GDPR** — `PrivacyDirector` (telemetry/crash 詳細/cloud sync は opt-in)、
  `docs/Privacy.md` 収集内容明示、初回 consent ダイアログ、`ExportUserData` /
  `DeleteUserData`（GDPR 取得権/削除権）。
- **メタ層フェーズ M1〜M7** を v9 phase 表に挿入（Pillar A〜H と並行）。

### 15.4 著作ツール深度 & 外部ミドルウェアシーム（Pillar K 拡張）

**4 不変条件**: (a) Pillar K seam 上に構築・重複実装しない (b) v1 はすべて
**in-game UiKit ベース**、外部 standalone は v1.1 (c) 著作データは `.atxt`/`.abin`/
`.tdat` で git diff 可能 (d) 外部 middleware は抽象 seam のみ、concrete は独立
モジュール `ACS::*Bridge`（ライセンス汚染なし）。

| ツール | 配置 | v1 LOC |
|---|---|---|
| **Particle Editor** — モジュール構成 + curve editor + preview + preset library | `gameframework/tools/fxedit/` (in-game) | ~1800 |
| **Animation Curve Editor** (Tween/AnimGraph blend curves) | `tools/curveedit/` | ~1200 |
| **BT Visual Editor** — drag-drop graph + **live debugger (current active node highlight + blackboard viz)** | `tools/btedit/` | ~2000 |
| **Level Editor** — in-game edit mode + Tilemap painter (palette/paint/fill/layer) + Prefab brush | `tools/leveledit/` | ~2500 |
| **Sprite Atlas / SDF / 9-slice / Outline auto-gen** | `tools/acs_atlas/` CLI + in-game | |
| **Font tools** — codepoint subset (locale-driven) / MSDF / fallback chain (CJK + Latin 共存) | Font 拡張 | |
| **Audio middleware seam** — `IAudioBackend` 抽象 (XAudio2 default)、**FMOD / Wwise** は `ACS::FmodBridge`/`ACS::WwiseBridge` 独立モジュール (free indie tier 内可)、event-driven API (`PlayEvent("explosion_large")`) + bank loading + parameter exposure (RTPC ↔ `MusicDirector` intensity) | `audio/IAudioBackend.h` + Bridge | |
| **Visual scripting** — v1.1+ 送り。当面 Pillar L BT visual editor + Pillar N Lua で代替 | — | — |
| **Docs viewer in-engine** — `DocsViewerScreen` で `docs/*.md` を in-game 閲覧 | UiKit | |
| **Asset diff/inspect CLI** — `acs_diff` (Pillar J reflection 経由 semantic diff)、`acs_assetinspect` (texture/audio/scene contents dump) | `tools/` | |
| **Cinematics editor** — timeline editor (multi-track / ripple edit / time scrubber / preview)、`.cutscene` 保存 | `tools/cineedit/` | ~1500 |
| **ELocale workflow** — `acs_loc_extract/diff/validate` | `tools/` | |

### 15.5 移植性 seam — 5 段階の現実的拡張

**honest 評価**:

| Tier | プラットフォーム | 価値 | コスト | 採否 |
|---|---|---|---|---|
| **Tier 1** | **Linux**（特に Steam Deck Verified）、Mac (Metal backend) | **Steam Deck は indie 売上で巨大** | 数週間〜数ヶ月 (audio/window/threading port) | **v2 推奨** |
| Tier 2 | Switch / PS5 / Xbox | プラットフォーム手数料・proprietary SDK NDA | 数ヶ月〜半年 | v3+ |
| Tier 3 | iOS / Android | touch-first UI 再設計 + app lifecycle + store 統合 | 大きい | v3+ |
| Tier 4 | Web (WebAssembly via Emscripten) | exception-mode WASM が厳しい | 大きい | v4+ |

**v1 で seam だけ確定**:
- 既存 `platform/` を「OS 直叩きを置く唯一の場所」と憲法化。
- **13 seam 確定**: `PlatFile`/`PlatDir`/`PlatWatch`/`PlatMmap`/`PlatAudio`/`PlatInput`/
  `PlatThread`/`PlatTime`/`PlatCrypto`/`PlatLocale`/`PlatProcess`/`PlatCrash`/`PlatWindow`。
- 各 pillar は `<Windows.h>` include 禁止（`acs_lint` R015）。
- future port は `platform/linux/`/`platform/macos/` を実装するだけ。
- **`IStorefront`** 抽象 (Steamworks/EOS/Xbox Live/Switch/iOS GC/Google Play)、Pillar
  Progress (v7) が consumer。
- **`IAudioBackend`** 抽象（FMOD/Wwise も同 seam — §15.4 と統合）。
- **App lifecycle** — `Game::OnSuspend()`/`OnResume()`/`OnLowMemory()` を v1 で hook
  (Windows では no-op、mobile/console で意味を持つ)。
- **Touch-first UI** — `UiInputMode::TouchFirst` enum を v1 で予約、実 touch UI 設計は
  v2+ 送り。

### 15.6 全体アーキテクチャ — v10 で 18 ピラー + メタ層

| 番号 | ピラー | 追加版 |
|---|---|---|
| A | App & Scene | v3〜 |
| B | オブジェクトモデル（Node2D + ECS + Node3D） | v3 + v9 |
| C | 時間・アニメ | v4 + v9 |
| D | 入力 | v4 + v8 |
| E | カメラ（**CameraStack + Cinemachine 級 blend graph**） | v4 + v8 + v9 + **v10** |
| F | 物理・衝突（F2: PGS dynamics） | v4 + v9 |
| G | リソース・永続化 | v4 + v8 + v9 |
| H | UI・音・ツール（**IAudioBackend seam**） | v4 + v8 + v9 + **v10** |
| I | エフェクト | v7 |
| J | シリアライズ/反射/プレハブ | v8 |
| K | エディタ/開発ツール（**著作ツール本体追加**） | v8 + **v10** |
| L | AI/ゲームプレイ深度 | v8 + v9 |
| M | ネット | v8 |
| N | Mod/スクリプト | v8 + v9 |
| O | 出荷/Live Ops | v9 |
| P | スケール/ストリーミング | v9 |
| **Q** | **視覚世界 / ライティング / 雰囲気** | **v10** |
| **R** | **Polish & Game Feel** | **v10** |
| — | **メタ層** (TypedHandle / 決定論 profile / acs_test / Events.h / StyleGuide+lint / 移植性 seam / Privacy 等) | **v10** |

加えて完成度システム 9 + ジャンルキット 7。

### 15.7 実装フェーズ — v10 で ~60 フェーズへ

v9 までの ~50 + 以下挿入:

| Ph | 内容 |
|---|---|
| **M1〜M7** | メタ層（HandleRegistry / Log channel / 決定論 profile / acs_test / Events.h / StyleGuide+lint / Recipes 等）— Pillar A〜H と並行 |
| **51〜53** | Pillar Q 視覚世界（2D lighting solver → AmbientDirector + 天候 → Screen effects + decals + water + sky） |
| **54〜55** | Pillar R Polish（cinematics + photo mode + tutorial → settings deep + difficulty + speedrun + boot） |
| **56〜58** | 著作ツール深度（Particle Editor → BT visual editor + Level editor → FMOD/Wwise seam + Cinematics editor） |
| **59 (v2)** | 移植性 concrete（Linux first、Steam Deck Verified 目標） |
| **60+** | v2 各種（3D physics solver / Web build / mobile / touch UI / 2-way 物理-soft body coupling 等） |
（v11 で 5 新ピラー S/T/U/V/W + 既存ピラー拡張 — §16）

---

## 16. v11 — Steamworks など Platform Services / Live Ops 深度 / Community / Accessibility&Multimedia / AI&ML / Backend&Team

ユーザー指摘「Steamworks SDK の対応とかも追加できる？もっと視野広くして」を受け
**6 エージェント並列**で 6 領域深掘り。**5 つの新ピラー S/T/U/V/W + Pillar O/H/R 拡張**。
詳細はエージェント成果物 (~380KB) に保持。

### 16.1 Pillar S — Storefront / Platform Services（新規 19 番目）

**Steamworks 第一**、indie の現実。`IStorefront`/`IModRepository`/`ILeaderboardClient`/
`IAchievementSink`/`IFeatureFlagClient`/`ITelemetryClient`/`ILobby`/`INetTransport` の
v9/v10 seam を **concrete bridge module** として実装する仕組み:

- **`ACS::SteamworksBridge`** （opt-in CMake、独立モジュール — AssetPack 流儀）:
  - **Achievements** (Pillar Progress sink) / **Stats & Leaderboards** (Pillar M `ILeaderboardClient` concrete、replay HMAC 添付)
  - **Steam Lobbies** (Pillar M `ILobby` concrete) / **Steam Datagram Relay (SDR)** = **Pillar M `INetTransport` 代替実装** (NAT punch + IPv4/6 + 輻輳制御、自前 UDP より優秀)
  - **Steam Workshop** (Pillar N `IModRepository` concrete) / **Steam Cloud** (saves 自動 sync)
  - **Steam Input** (Pillar D `InputMap` アダプタ — 全コントローラ Switch Pro/PS5/exotic 含めサポート)
  - **Steam Rich Presence** / **Big Picture / Steam Deck Verified** / **Steam DLC** / **Family Sharing** / **VAC seam** (real-time 多人数の場合のみ)
  - **Steamworks callback / CallResults を `SteamFuture<T>` で吸収** + `Game::Tick` 内で `SteamAPI_RunCallbacks` 1 回
- **`ACS::EosBridge`** — Epic Online Services (free, cross-platform、特に voice 標準装備が強い)
- **`ACS::XboxBridge` / `ACS::PsnBridge` / `ACS::SwitchBridge`** — NDA SDK、bridge module だけ確保（partner 別 PR で配線）
- **`ACS::GameCenterBridge` / `ACS::PlayGamesBridge`** — iOS / Android
- **`ACS::ItchioBridge`** — JWT 認証、butler upload、leaderboard
- **`ACS::ModIoBridge`** — 非 Steam の Workshop 代替
- **`ACS::GogBridge`** — GOG Galaxy (free SDK)
- **`ACS::DiscordBridge`** — Rich Presence + voice + activities
- **`PlayerIdentity` 統一モデル** — Steam ID + Epic ID + Xbox ID + game-account ID の cross-platform 集約、cross-progress (Steam → Epic で同セーブ続行)
- **Storefront 検出 runtime** — game が Steam/Epic/GOG/itch/raw のどれで起動したか build flag + runtime check で判定、feature set を自動調整
- **DRM** — Steam built-in のみ opt-in、他 DRM は honest に推奨せず

### 16.2 Pillar O 拡張 — Live Ops / GaaS / Monetization 深度

「**倫理デフォルトを最も書きやすい道に**」が設計原則。搾取的パターンは可能だが警告
付き、ヘルシーパターンは 1 行。

- **`IStorefront` を unified entitlement model に格上げ** — DLC/サブスク/ベータブランチ/
  受給を 1 つの `Entitlement` 表へ吸収。各 SDK 差分は bridge に隔離
- **AssetPack を「DLC/パッチ/イベントオーバーレイ/Mod」の共通配信レーンに統一** —
  DLC = `.acpak` + `dlc_manifest.toml` + `EntitlementId`、新メカニズム不要
- **`MonetizationDirector` + `IMtxBackend`** — IAP/プレミアム通貨/ガチャを 1 ハブ化、
  framework 単体ではリアル金額を扱わずレシート検証は server
- **`EventDirector`** — `IFeatureFlagClient` 上の薄層、サーバ駆動シーズン/期間限定 +
  オフラインキャッシュ + grace period
- **Season Pass / Battle Pass framework** — `Season` data asset、`BattlePassDirector` で
  player 進捗追跡、free vs paid track
- **倫理デフォルト**:
  - gacha は **explicit opt-in + 強制 odds 公開** (日中欧加州など)
  - daily login は **escalating-loss 無効**（manipulation patterns 回避）
  - premium currency は **real-money equivalent 表示が既定 ON**
- **subscription support** — Xbox Game Pass / PS Plus / Apple Arcade / Netflix Games 検出
- **DLC pipeline 深度** — variants（story/cosmetic/season pass/character pack）、regional
  pricing、demo distinction（time/content-limited）
- **EOL モード v1 で seam** — EU/UK 消費者保護「killed by publisher」議論を見越し、
  `EolMode` 切替で「servers 落ちてもオフライン継続」を最初から設計
- **規約 honest acknowledgment** — `docs/MonetizationCompliance.md` で各機能の各国
  地雷マップ（中国 license / 日本 odds 表示法 / EU consumer protection / California /
  Belgium gacha ban / 等）

### 16.3 Pillar T — Community / Social / Communication（新規 20 番目）

「ゲームプレイの上の社会層」。**default-off で solo game は LOC ゼロ**。すべて
`ESvc::Social*` opt-in。

- **`IFriendsService`** — Steam / Epic / Xbox / PSN / Switch friends + `PlayerIdentity`
  で cross-platform mapping。online/offline/in-game/away/busy/invisible
- **`PartySystem`** — host invites + party-as-a-unit lobby/match join + party voice 自動
  ルーティング + party leader 制御 + cross-scene 永続
- **`IVoiceChat` seam**:
  - **Steam Voice** (free under Steam) / **EOS Voice** (free, cross-platform) / **Vivox**
    (commercial) / **Discord SDK** voice / 自前 Opus + `INetTransport`
  - **spatial 3D voice** — distance attenuation + `AudioSystem` listener 統合
  - PTT vs voice-activation、voice moderation (STT seam + content filter + report)
- **Text chat** — channel (global/team/party/whisper) + filter + per-user mute +
  slow-mode + 翻訳 seam (v1.1) + spam 検出
- **`IPresenceService`** — Steam/Discord/Xbox Rich Presence + joinable status
- **Activity feed** — server-side log + friend's recent activity UI + opt-out
- **Screenshot/clip sharing** — F12 + 共有 dialog + **photo mode** (Pillar R) export +
  **replay sharing** (`.acsr` → Workshop) + **highlight clips** (auto-capture last 30s
  on achievement)
- **In-game messaging** — async DM + `IMessageStore` (Steam lobby metadata or P2P or EOS) +
  toast notification
- **Group/guild/clan** — server-hosted persistent membership + discovery + chat (v1.1)
- **Blocking/reporting/moderation** — `PlayerBlocklist` 局所 + `Report` flow +
  `IModerationBackend` (Discord T&S / Vivox safety / 自前 manual review) + 自動検出 ML
- **Spectator mode** — live spectator + replay viewer + director cam (Pillar R blend
  graph 連動)
- **Privacy first** — 全機能 opt-in、**under-18 default は最厳** (voice off / anonymous
  handle / DMs friends-only) — COPPA/EU under-16 protection
- **Community hub** — in-game gallery (Workshop curated) + Discord widget + news RSS

### 16.4 Accessibility 深度（Pillar H 拡張）+ Multimedia（Pillar H/R 拡張）

v7 Accessibility baseline を大きく超える深度 + multimedia 統合（TTS/字幕/手話 video が
両者で共有）。**新ピラー化せず、Pillar H に `a11y/`+`media/` 子モジュール**を追加。

#### Accessibility 深度
- **Screen reader / TTS** — `IScreenReader` (Win UIA / Mac VoiceOver / iOS / TalkBack
  バックエンド) + UI accessibility tree (UiKit widget が auto-expose) + 世界内 TTS
  (dialogue/sign 読み上げ) + 方向音響 hint
- **Switch device support** — 単/双ボタンゲーム + hold-cycle remap + auto-aim/auto-combo
- **Eye tracking** — Tobii / PS5 / iOS Face ID seam、gaze-driven UI、gaze aim
- **One-handed mode** — 自動 remap + mobile-style UI re-layout
- **Full subtitle customization** — size/color/per-speaker/font(dyslexia)/position/
  outline/opacity/auto-pause + **speaker identification** color/icon (Hades 流) +
  speech rate
- **Closed captions for SFX** — `[door creaks][footsteps right]` + 方向 indicator +
  important-only filter
- **Sign language avatar** (v1.1) — `ISignLanguageProvider` (pre-recorded clip or ML
  avatar)、locale 別 (ASL/BSL/JSL)
- **Audio accessibility cues** — occlusion-cue / threat radar pulse / loot beep
- **Custom per-disability profile** — Low Vision / Motor Limited / Hearing Impaired /
  Cognitive preset + export/import
- **Cognitive accessibility** — simplified UI / tutorial replay / goal reminder /
  reading help / dyslexia font
- **Photosensitive depth** — per-effect-type toggle + first-time prompt
- **Color contrast checker** — Pillar K Editor designer tool (WCAG AA/AAA)
- **Motor accessibility depth** — configurable QTE/dodge/parry window timing
- **規制対応** — CVAA / EAA 2025 / Game Accessibility Guidelines 採用

#### Multimedia / Content I/O
- **`VideoComponent`** + `VideoComponent3D` — VP9/AV1 (license-free) + H.264/265 hardware
  decode (DXVA) + AssetPack VFS streaming + subtitle/dialogue sync + Cinematic bus 経由
- **Audio recording** (`IAudioRecorder`) — microphone via XAudio2 input、voice memo、
  dictation seam (STT)
- **Webcam integration** (`IWebcam`) — Media Foundation / AVFoundation / Camera2、AR
  filter / head-tracking / streaming overlay、privacy 明示 indicator
- **Microphone gameplay** — voice commands (STT)、singing detection (pitch、rhythm
  game)、voice chat input
- **Image I/O** — PNG/JPG import/export with EXIF metadata、UGC moderation 統合
- **VR-specific media** seam (v1.1+)
- **Streaming/clipping** — Steam Broadcasting / Twitch hint
- **AR/ARKit/ARCore** seam (mobile port 後)

### 16.5 Pillar U — AI / ML 統合（新規 21 番目）

**設計判断**: AI を**新ピラーに隔離**、コア 18 pillar は AI を知らない。`ACS_GAME_AI_ENABLED=OFF`
で完全に消える。**4 大不変条件**: (a) 全 AI 機能 `ESvc::Ai*` opt-in / (b) 全機能 offline
fallback 必須 / (c) **決定論ゾーンへ AI 混入禁止**（コンパイル時 check） / (d) クラウド
送信データは明示同意・破棄可能（`PrivacyDirector` 配下）。

- **v1 = seam 5 個 + AiCostTracker + AiBudget + AiPrivacyHooks**:
  - **`IMlRuntime`** — ONNX Runtime / DirectML / CoreML / NNAPI / Win11 NPU、`MlModel`
    AssetPack 経由 `TRc<MlModel>`、quant tier (fp32/16/int8/4) 自動選択、`Pillar P
    GpuMemoryBudget` 統合
  - **`ILlmBackend`** — OpenAI/Anthropic/Gemini (Remote) + llama.cpp local + **`HybridLlmRouter`**
    (cloud→local→scripted fallback 3 段)、**`LlmSafetyPipeline` v1.1** 必須通過
    (character anchor + jailbreak detect + PII leak + refusal + content rating + token/cost
    budget) — `acs_lint` R042 で直接呼出禁止
  - **`ITtsBackend`** — Platform SAPI/VoiceOver/TalkBack (free) + ElevenLabs/Polly (cloud) +
    Piper (local)、voice cloning は **`consent_token` 強制**
  - **`IContentModerator`** — local cheap → cloud accurate 二段、`SexualMinor` ハードコード
    （studio が OFF 不可）、監査ログ HMAC 署名
  - **`IUpscaler`** — FSR (open) + DLSS Streamline (closed bridge) + XeSS、Pillar Q
    `RenderPassRegistry` 経由
- **v1.1 = 具体実装**:
  - LLM NPC dialogue + `LlmAugmentedNpc` Pillar H Dialogue 統合 + prefetch
  - `IDdaController` — transparent DDA (隠し rubber-banding は推奨せず)、Pillar R Settings に
    toggle、`DebugOverlay` で現値表示 opt
  - `IHintProvider` — scripted hint pool + LLM rephrasing
  - `acs_ai_asset` CLI — dev-time texture/voice 生成 + **provenance tracking** (model/prompt/
    seed/timestamp/license/edit summary を `.aiprov` で記録) + 人手編集 → bake → ship、
    **runtime AI 生成は出さない**（IP/品質/レイテンシ/コスト の honest 判断）
- **v2+ 投機的**: `IMusicGen` (runtime)、RL playtest (`AiPlaytester`)、Motion Matching ML、
  NeRF/Gaussian Splatting、neural texture compression、DLSS Frame Generation
- **規制対応** (`docs/AiCompliance.md`):
  - **EU AI Act** 2026 段階施行 — 生成コンテンツ表示義務 (UiKit `AiContentBadge` 標準)、
    UGC moderation 高リスク扱い → 監査ログ機能
  - California AI 透明性法 (2025-)、日本 AI 推進法、中国生成 AI 弁法、韓国 AI 基本法
  - **opt-out from training** (default OFF) + GDPR `ExportUserData`/`DeleteUserData` AI 拡張
- **正直な評価マップ** (`docs/AiUseCases.md`):
  - LLM NPC 効く: 開放探索の村人 / 商人 / ロア説明 → 効かない: クエスト依頼 / メインクエスト
    critical-path / 戦闘煽り（latency 1.5-4 秒）
  - dev-time AI 効く: prototype art / placeholder VO → runtime AI art は法的/IP/品質で v2 送り
- **コスト**: `AiCostTracker` で per-session/day/month $ 上限、超過時 local fallback → 機能無効

### 16.6 Pillar V — Backend Services + Pillar W — Studio / Team Workflow（新規 22-23）

#### Pillar V — Backend Services（**game-developer 自分のバックエンド側**を扱う層）
- **Dedicated server tooling** — `acs_module(TYPE Server)` で no-render no-audio build、
  `Scene::PeerRole::DedicatedServer` (v9)、サーバ config、persistence、**Docker image +
  Kubernetes template** + AWS GameLift/Azure PlayFab/Google Cloud Game Servers seam、
  auto-scaling、graceful shutdown (drain players)
- **`IMatchmaker` seam** — Steam Lobbies / EOS / 自前。**`RatingService`** (Glicko-2 or
  TrueSkill) + region-based MM + queue UX + fairness (rating widening) + anti-smurf
- **Server browser** — `IServerBrowser` で community-hosted server 一覧 + filter
- **`ICloudGameState`** — server 持ち authoritative state、定期 snapshot → durable storage、
  replay from cloud
- **`IBackendClient`** — game ↔ developer 自前 backend (REST/gRPC) の JWT 認証 + endpoints、
  **`MockBackendClient`** で local dev、複数 backend 並列
- **Anti-cheat 深度** — `IAntiCheat` (VAC / EAC / BattlEye / nProtect / Vanguard / 自前
  server-authoritative) + replay 検証 + shadow-ban + player report queue
- **Player support / customer service backend** — in-game support ticket + auto-classifier
  (small LLM) + `ISupportTicketSink` (Zendesk/Helpdesk/Discord)
- **Admin tooling** — web dashboard for ban/mute/refund/grant/GDPR export
- **Telemetry backend** — `ITelemetryBackend` concrete (PostHog/Amplitude/Mixpanel/
  GameAnalytics/自前) + 反射経由 event schema + funnels + segments + heatmaps + A/B
  test results

#### Pillar W — Studio / Team Workflow（**多人数 dev チーム**支援）
- **Asset locking** (git-LFS pattern) — `AssetLockService` explicit lock、Pillar K Inspector
  に lock status indicator、`acs_assetlock` CLI、pre-commit hook
- **Review workflow** — `acs_review` CLI で **Pillar J 経由 semantic asset diff** +
  Slack/Discord/GitHub PR webhook auto-rendered visual diff
- **Build farm** — `acs_buildfarm` で分散 build + sccache/ccache + 共有 asset bake cache +
  CI matrix (GitHub Actions ↔ Jenkins/Azure DevOps/Buildbot)
- **Multi-user editor** v1.1+ — file-level locking v1、real-time co-edit (OT/CRDT)
  は研究的、v1.1+ で `ICollabBackend` (Liveblocks/Yjs)
- **In-engine code editor** — Pillar N Lua スクリプト用ミニマル editor + **asset browser**
  + **find-references** (Pillar J 反射経由)
- **C++ hot-reload** — `IHotReloadCpp` seam (Live++ 商用 / RCC++ open)。実装は v1.1+、
  current は restart workflow (Lua + DataAsset hot-reload で大半カバー)
- **IDE 拡張** — VSCode language server for `.atxt`/`.tdat`/`.bt`/`.dlg`/`.chart` (Pillar J
  反射駆動 schema validation + autocomplete) + Rider ReSharper plugin (acs_lint linker) +
  VS natvis files
- **In-engine docs search** — `docs/recipes/` 検索 bar + AI-powered RAG search (v1.1) +
  inline help から docs リンク
- **Onboarding** — `acs_setup` CLI 一行で repo clone → build → hello sample 実行
- **Performance regression dashboard** — Pillar K Profiler `.acsperf` → backend dashboard
  for frame-time history over commits + alert
- **Bug repro flow** — player report = replay (.acsr) + state snapshot (Pillar J) + crash
  dump (Pillar O)、`acs_repro` CLI で 1 命令で local 再現、time-travel debug seam
- **Content workflow** — Aseprite/Photoshop/Spine/Blender export auto-watch、Pillar K
  HotReload extends、Jira/Trello/Linear/Notion/GitHub Issues webhook
- **`acs_new` CLI** — `acs_new <kit> <game-name>` で genre kit + sensible defaults + git
  init + first build を 1 行
- **production tracker integration** seam

### 16.7 全体アーキテクチャ — v11 で **23 ピラー A〜W** + メタ層

| 番号 | ピラー | 追加版 |
|---|---|---|
| A〜H | 基礎 8（H 拡張: AudioSystem + IAudioBackend seam + **a11y/media 子モジュール**） | v3 + v10 + **v11** |
| I-P | エフェクト/シリアライズ/エディタ/AI 深度/ネット/Mod/出荷/スケール | v7-v9 |
| O 拡張 | **Live Ops/GaaS** (Entitlement model + MonetizationDirector + EventDirector + EolMode + 倫理デフォルト) | **v11** |
| Q-R | 視覚世界/Polish & Game Feel | v10 |
| — | メタ層 | v10 |
| **S** | **Storefront / Platform Services** (Steamworks 等 bridge module 群) | **v11** |
| **T** | **Community / Social / Communication** | **v11** |
| **U** | **AI / ML 統合** | **v11** |
| **V** | **Backend Services** (dedicated server / matchmaking / IBackendClient / 開発者バックエンド側) | **v11** |
| **W** | **Studio / Team Workflow** (asset lock / build farm / IDE 拡張 / 多人数開発支援) | **v11** |

加えて完成度システム 9 + ジャンルキット 7 + AssetPack + 著作ツール深度。

### 16.8 実装フェーズ — v11 で ~80 フェーズへ

v10 までの ~60 + 以下挿入:

| Ph | 内容 |
|---|---|
| **62〜64** | Pillar S Storefront — `ACS::SteamworksBridge` 完全実装 (achievements/lobbies/SDR/Workshop/Cloud/Input/Rich Presence/DLC) + `PlayerIdentity` 統一 → EOS/itch/GOG/Discord bridge → Xbox/PSN/Switch seam |
| **65〜66** | Pillar O Live Ops 拡張 — Entitlement model + MonetizationDirector + EventDirector + Season/Battle Pass + EolMode |
| **67〜69** | Pillar T Community — Friends/Party/Voice chat (Steam Voice + EOS Voice) → Text chat + Presence + Activity feed → Sharing + Spectator + Moderation |
| **70〜71** | Pillar H/R Accessibility 深度 — ScreenReader/TTS + Switch device + subtitle customization + closed captions for SFX → 規制対応 + Color contrast tool + 残り (eye tracking/sign language v1.1) |
| **72** | Multimedia — VideoComponent + AudioRecorder + Image I/O + Webcam/Mic seam |
| **S1〜S4** | Pillar U AI/ML v1 seams — IMlRuntime / ILlmBackend / ITtsBackend / IContentModerator / IUpscaler + AiCostTracker + PrivacyDirector AI 拡張 |
| **S5〜S11 (v1.1)** | OnnxRuntime + DirectML + LlmSafetyPipeline + Remote/Local LLM + Hybrid Router + Pillar H Dialogue 統合 + Platform/Remote TTS + Content moderation + DDA + Hint + FSR/DLSS + acs_ai_asset + acs_ai_verify CLI |
| **73〜75** | Pillar V Backend — dedicated server build + IMatchmaker (Glicko-2) + IBackendClient + Anti-cheat + Telemetry backend |
| **76〜78** | Pillar W Studio — asset locking + Pillar J semantic diff CLI + build farm + IDE 拡張 (VSCode LSP) + in-engine docs search + bug repro flow + `acs_new` CLI |
| **79 (v1.1)** | C++ hot-reload (Live++/RCC++ seam) + multi-user editor (CRDT) |
| **80+ (v2)** | Console NDA SDK bridges 個別 / Web build / AI playtest RL / AI music gen runtime / NeRF / mobile touch UI |

**前提**: Pillar S Storefront (Ph62) は Pillar M ネット (Ph22-24) 完了後。Pillar T
Community (Ph67) は Pillar S Steamworks Voice 統合後。Pillar U AI (S1) は Pillar P
メモリ予算 (Ph38) 完了後。Pillar V Backend (Ph73) は Pillar O 出荷 (Ph35) 完了後。
Pillar W Studio (Ph76) はメタ層 M1〜M7 完了後 (Pillar J 反射が前提)。
（v12 で 11 領域追加 — §17）

---

## 17. v12 — 残課題 11 領域全部練り

v11 末で「まだ練れる候補」として挙げた 11 領域すべてを **6 エージェント並列**で深掘り。
**新ピラー X 1 つ + 既存ピラー大幅拡張**。詳細はエージェント成果物 (~340KB) に保持。

### 17.1 Pillar X — XR (VR/AR/MR) + 新興プラットフォーム（新規 24 番目）

**設計判断**: 新ピラー X が必須（7 ピラー全部が XR で破綻 — frame loop / late-latch /
action manifold / per-eye view / spatial audio / screen-space PP / camera 奪取）。
Pillar X は **conductor**、実装の 70% は既存 pillar の "XR モード" 拡張。
`ACS_GAME_XR_ENABLED=OFF` で完全消失、indie 2D 開発者はコスト 0。

- **`IXrRuntime` 抽象 + `ACS::OpenXrBridge`** (独立モジュール) — OpenXR を seam として
  Meta Quest / Valve Index / HTC Vive / Pico / Apple Vision Pro / Windows MR をカバー。
  Session lifecycle (`xrWaitFrame`/`xrBeginFrame`/`xrEndFrame`)、reference space、per-eye
  swapchain、foveated rendering hint。
- **6DOF tracking** — HMD pose（**predicted display time** — late-latch）、controllers
  (Oculus Touch / Valve Index Knuckles / Vive Wand / Quest Pro / PSVR2 Sense / Vision Pro
  pinch)、hand tracking (Quest finger + Leap Motion + Vision Pro hand-eye)、**eye tracking**
  (Vision Pro / Quest Pro / Vive Pro Eye、foveated rendering + UI gaze + analytics)、
  body tracking (HTC Trackers / Mocopi)
- **Spatial UI** — UiKit screen-space は破綻。`SpatialUiCanvas` / `WristMenuCanvas` /
  `WatchUiCanvas`、curved canvas、laser pointer vs direct touch vs gaze+pinch、Apple HIG / Meta
  design guidelines に従う最小サイズ・距離
- **Comfort / motion sickness** — locomotion modes (teleport/smooth/dash/blink、player toggle)、
  snap turn vs smooth、vignette during movement、**90Hz minimum / 120Hz target frame rate
  guarantee** (Pillar P FrameGovernor strict)、reprojection awareness (UI no-reproject layer)、
  IPD adjustment、comfort settings UI default-defensive
- **Apple Vision Pro 固有** — passthrough AR (shared space vs full immersion)、Persona seam、
  **eye-tracking primary input** (look + pinch)、90Hz adaptive、SwiftUI 経由 `IXrShellBridge`
- **Meta Quest 固有** — standalone perf budget (Quest 3 ≈ Snapdragon XR2 Gen 2)、PC tethered
  (Link) mode、Mixed Reality (Passthrough API)、hand + controller 共存、App Lab / Quest Store
- **PSVR2 固有** — adaptive triggers + haptics + eye tracking + foveated rendering + PSN
- **Valve Index + SteamVR + Steam Deck** — Knuckles 指トラッキング、SteamVR runtime alternate、
  **Steam Deck は VR でないが gamepad-first UI 必須**、gyro 制御、back paddles L4/L5/R4/R5、
  trackpads、handheld vs docked perf preset
- **AR ARKit/ARCore seam** — world tracking + anchor + plane detection + image tracking + AR
  Cloud (Apple AR Anchors / Google Cloud Anchors) + lighting estimation (Pillar Q
  AmbientDirector 入力) + people occlusion (Pillar Q rendering 統合) + AR Quick Look
- **既存ピラーとの統合**:
  - **Pillar B/E** — `XrCamera : Camera3D` が runtime から late-latch pose
  - **Pillar D** — `XrInputProfile` (action manifest in OpenXR style) が `InputMap` 上層
  - **Pillar H** — **spatial audio mandatory** (Steam Audio bridge、§17.3)、Ambisonics 必須
  - **Pillar I** — screen-space effects (DoF/lens flare) は stereo で破綻、抑制
  - **Pillar P** — strict 90/120Hz、never drop physics
  - **Pillar R** — photo mode = spectator camera (player camera 奪取禁止)
  - **Pillar T** — avatar (Meta Avatars / Vision Pro Personas)、proximity spatial voice
- **Privacy** — eye tracking + body tracking + spatial scanning + Persona すべて biometric、
  `PrivacyDirector` strict opt-in
- **規制** — PEGI / ESRB / CERO の VR rating、Health & Safety startup screen、photosensitive
  enhanced risk
- **honest スコープ**: v1 = seam のみ、v1.1 = OpenXR concrete + Quest standalone full、
  v2 = Vision Pro / PSVR2 / ARKit/ARCore、v3 = body tracking / AR Cloud / in-VR editor

### 17.2 Pillar F3 — 物理シミュ高度化（Pillar F 拡張・サブピラー）

F2 (PGS rigid + 8 joints + CCD) に**上乗せ**、`physics2/` namespace で**独立**。
F2 と F3 の接合は **`ICouplingBridge`** 1 本のみ。**外部 dep ゼロ**
(Bullet/PhysX/Havok/Jolt/FleX 不採用、自前実装)。

- **`SoftBody2D` / `SoftBody3D`** — **XPBD (Extended PBD)** ベース、~600 LOC、cloth/rope/
  jelly を constraint set 切替で扱う。pin (rigid body anchor)、tear (break threshold)、
  one-way coupling v1、two-way v1.1、self-collision v1.1
- **`Cloth2D` / `Cloth3D`** — character clothing + 旗 + 幕、wind force (Pillar Q
  `AmbientDirector` 入力)、ボーン pin (Pillar C 統合)
- **`Hair3D`** — Kelager 風 strand simulation (XPBD with length + bending)、capsule
  collider (head/shoulder)、wind interaction、character LOD、v1.1+
- **Destruction** — **Voronoi shatter** (`acs_fracture` CLI で dev-time pre-fracture)、
  sprite 2D destruction (Pillar Q alpha 削り取りの拡張)、`DestructibleComponent` HP +
  fracture pattern + 動的 rigid body 変換、chunked fracture (大→中→小→塵)
- **Fluids — honest scope**: **2D SPH for puzzle games (Where's My Water 風)** = v1.1、
  ~1000 particles real-time。**3D FLIP fluids = v2+**、**SPH 流体 indie 大半不要**を docs
  で明示
- **`IGpuCompute` seam** — DX12 / Vulkan / Metal / WebGPU 抽象、soft body + fluid +
  particle が利用。Pillar I 既存 particle が GPU 化可能
- **`VehicleComponent`** — F2 wheel joint 拡張 + suspension + Ackermann steering + slip
  friction + differential。2D top-down は bicycle model、3D は 4-wheel slip dynamics
- **`WaterVolume`** + buoyancy + drag + 浮沈 (density)、splash event → Pillar I + Pillar H、
  two-way coupling v1.1
- **決定論コントラクト** — soft body/destruction は `Required`、GPU 流体は `Unsupported`
  (lockstep 不可、CPU fallback あり)
- **honest スコープ**: v1 = XPBD SoftBody + WaterVolume + Destructible (pre-baked shards)、
  v1.1 = 2D SPH + Hair + GPU cloth + self-collision、v2+ = 3D FLIP / 完全 articulated /
  vehicle dynamics 深度 / two-way water surface

### 17.3 Pillar H 音響高度化

**`acs::dsp` 共通プリミティブ**新設 (FFT/IIR/Delay/Osc/Envelope/Resampler/Window) + 上に
6 領域:

- **HRTF binaural** (§spec 1) — `IHrtfRenderer` seam + **`HrtfRendererStub`** (KEMAR 256-tap、
  140KB 同梱、no external dep)、**`ACS::SteamAudioBridge`** v1.1 (Apache-2 free)、
  `SoundSource::use_hrtf` opt-in、VR 必須
- **Convolution reverb** (§spec 2) — UPOLS (uniformly-partitioned overlap-save) CPU、block 128、
  **`ReverbZone::convolution_ir`** で algorithmic と切替、IR pack 同梱 (cave/hall/church
  /parking_garage CC-BY ~20MB)、GPU path v2
- **Vocoder** (§spec 3) — band-bank 16 bands FFT、boss intercom/robot voice、v1.1 niche
- **Real-time FFT analyzer** (§spec 4) — **高優先 v1**。FFT magnitudes + 32 perceptual bands +
  RMS + peak + **LUFS (ITU-R BS.1770) integrated loudness** + dominant Hz + YIN pitch
  (v1.1) + **onset + tempo estimate**。**Rhythm game の `BeatClock` 駆動 / Pillar I
  audio-reactive effects (bass-driven particle, onset-driven flash) / Pillar Q audio-reactive
  lighting / Pillar L AI hearing / Pillar T VAD / 字幕 SFX auto-tagging** — 1 つで多面的に効く
- **Granular synthesis** (§spec 5) — procedural ambient (wind/water/crowd の無限変奏で
  loop perceptibly 消滅) / NPC mumble / slow-mo time-stretch pitch-preserved、
  `RandomChannels::Get("audio")` で**決定論的**(replay/lockstep)、v1.1
- **Ambisonics** (§spec 6) — first-order B-format (4ch W/X/Y/Z) + third-order (16ch)、HRTF
  decode、**VR 必須**、v2 (Pillar X gating)
- **Doppler + air absorption + wind muffling + underwater** (§spec 7-8) — air absorption は
  one biquad per source で **outdoor 距離感劇的向上**、v1。wind muffle / sound speed は v2
- **Occlusion 深度** (§spec 9) — v9 で v1.1 → v1 に昇格。Centerline / Volumetric / **Diffraction
  via NavGrid** (Pillar L pathfinding 経由 shortest detour path → LPF cutoff)、
  **`PhysicsMaterial::audio_attenuation_db` / `audio_lpf_hz`** で素材ごと (wood -6/2000、
  concrete -18/400、glass -3/4000)、AI hearing と共有
- **Per-source DSP chain** (§spec 10) — max 4 nodes/source、~16 instances total、
  `DspChainId` で chain registry、v1.1
- **Voice processing** (§spec 12) — pitch corrector (auto-tune) / PSOLA time-stretch /
  noise gate (`sidechain_voice_chat=true`) / compressor / limiter / parametric EQ、Pillar T
  voice chat 自動配線、v1.1
- **MusicDirector Stinger** (§spec 13) — beat-quantized stinger 発火 + sidechain duck、v1
- **`IAudioBackend` 拡張** — `IHrtfRenderer` / `IConvolutionReverb` / `IAmbisonicsDecoder` /
  `IOcclusionBackend` を seam 分離、`ACS::SteamAudioBridge` は全 4 を 1 ライブラリで提供

### 17.4 Pillar U AI 進化（Pillar U 拡張）

v11 §16.5 Pillar U に **2026-2028 frontier** を上乗せ。`src/game/ai_frontier/` 子モジュール。
**honesty banner**: 2026 shipping game は「**LLM spice 付き scripted**」が現実、「LLM が
ゲームプレイ」ではない。

- **Multimodal LLM (vision-language)** — `IVisionLm` (`ILlmBackend` 拡張)。NPC が player
  avatar / 場面を「見て」反応。GPT-4V / Claude 3 Sonnet vision / Gemini Pro Vision /
  LLaVA local / Idefics local。**`SceneRedactor`** で PII (player name 等) を rendered
  frame から削る。photo mode caption / NPC reactions / **accessibility 視覚説明** (Pillar
  H a11y screen reader 統合) / hint giver "looks at" scene。コスト 2-10x、latency +500ms-2s
- **Generative agents** (Stanford Smallville 2023 風) — `AgentMemoryStream` + `AgentReflection`
  (periodic LLM 反省) + `AgentPlanning` (daily/hourly plan) + `AgentRelationships`。
  **honest cost**: Smallville は 25 agent で $1000/hour。**recommendation**: tier 分け —
  critical NPC のみフル generative agent、background は scripted + 偶発的 LLM rephrase。
  非決定論、`Unsupported` lockstep、save persistence via Pillar J 反射
- **AI companions** — 単一 ML キャラ persistent memory (long-context RAG) + personality
  drift + cross-session memory + emotion expression。**privacy 重大** (intensely personal
  data) → local recommended。sidekick / dungeon master / tutor 用途
- **AI procedural quest generation** — LLM が world state から side quest を生成、
  validation (hallucination 検出して reject)、reward generation。**critical-path/mainline
  quests には使わない**、sandbox busywork のみ
- **AI dynamic narrative** — story tree generation + consistency tracker (別 LLM 呼で矛盾
  validation)、**main story = scripted**、AI は flavor (NPC が過去 reference) 程度
- **AI level generation** — LLM 高位 intent + Pillar H Dungeon primitives 実行、scoring
  で regenerate、**daily challenge** や mod tool assistant 用、main campaign に使わない
- **LLM as behavior tree decision maker** — `LlmBehaviorComponent` — BT (Pillar L) の 1
  リーフが LLM 評価。**興味深い NPC のみ**、大半は通常 BT
- **AI mod tools / authoring assist** — Pillar K Inspector "AI Assist" panel ("suggest
  dialogue for this NPC")、Pillar N Lua code generation from natural language、Pillar W
  Studio bug repro analysis (LLM が crash dump を読む)
- **runtime image gen / text-to-3D** — v2+ 投機的、IP risk
- **AI が NOT 良くないもの** (`docs/AiUseCases.md` 明示): combat AI / physics 推論 /
  tight quest design / replay-determinism / cost-sensitive market / offline market
- **`AiCostTracker` 拡張** — feature category 別 budget、"Reduce AI" mode for fallback

### 17.5 Pillar D/T/S/O 拡張 — 入力 & 配信 & モバイル

#### Part A — 特殊入力 (`IExoticInput` seam、Pillar D 拡張)
- **DDR dance pad** / **Taiko drum** / **plastic guitar/drum (Guitar Hero)** / **racing wheel**
  (Logitech FFB / Thrustmaster FFB SDK) / **flight stick / HOTAS** (8+ 軸) / **trackball
  (a11y)** / **foot pedal (a11y switch)** / **MIDI keyboard input** / **brain-computer
  interface seam** (Emotiv/Muse/OpenBCI — honest: 2024 BCI 研究段階、a11y motor-disabled
  用途のみ) / **haptic suit** (bHaptics seam) / **gyro/accelerometer** (JoyCon/Deck/mobile/
  DualSense) / **Steam Deck back paddles** (Steam Input 経由) / **gaming chair haptics**
  (D-Box / Buttkicker、audio side-channel) / **RGB lighting** (Razer Chroma/Logitech G/
  Corsair iCUE/NZXT CAM)
- **`ExoticDeviceId` opaque ID** + `DeviceCaps` flag (Digital/Analog/Axis2D/3D/Force/Pressure/
  Gyro/Accel/FFB/Haptic/Calibration/**NonDetermin** — lockstep 不可と機械的明示) + `InputMap`
  入力 source 同列扱い

#### Part B — ライブ配信統合 (Pillar T 拡張)
- **OBS plugin seam** (WebSocket via obs-websocket、game → OBS scene switch)
- **Streamlabs alerts** (donation/sub/follower → in-game UI toast)
- **Twitch chat → in-game** (`ITwitchChat` IRC/EventSub、Crowd Control SDK 統合 seam、
  **anti-abuse** rate limit + moderation + `IContentModerator` filter)
- **YouTube Live chat** (YouTube Live Streaming API)
- **Stream overlay templates** (HTML/CSS for OBS browser source、game-bundled)
- **Donation hooks** (Bits/Super Chat/tip → in-game event、donor 名 filter)
- **Spectator camera output** (Pillar E CameraStack 別 cam for streamer)
- **Beam/Trovo/Kick/DLive** seam — `IStreamingPlatform` 抽象
- **`IsBeingStreamed()` detection** → Pillar R streamer mode 自動有効化
- **Stream-replay sharing** (`.acsr` → Twitch Clip / YouTube Short)
- **chat reader TTS** (streamer 用 a11y/multitask)
- **Live captions for streamer's hearing-impaired viewers** (Pillar H §17.3 onset → caption)

#### Part C — モバイル固有 (Pillar S/O 拡張)
- **IAP 深度** — Apple StoreKit 2 (JWT 受領) + Google Play Billing v6、**server-side
  validation** (`IBackendClient` Apple `/verifyReceipt` / Google Play Developer API)、
  family sharing IAP、subscription IAP (auto-renewing receipt refresh)、promo codes、
  refund (App Store Server Notifications V2)
- **Push notifications** — `IPushNotification` (APNs / FCM)、daily login reminder + event
  start + social、quiet hours、token は PII 扱い
- **iOS family sharing for game ownership**
- **Apple Game Center / Google Play Games sign-in** (`IGameCenterAuth` / `IPlayGamesAuth`)
- **Mobile lifecycle 深度** — `OnBackgrounded` / `OnForegrounded` / `OnMemoryWarning` (iOS
  低メモリ通知 → Pillar P eviction) / `OnInterrupt` (電話 → auto-pause)
- **App Clip / Instant App** (~10MB demo launched from QR/NFC、Pillar O 小型 bake target)
- **Deep links** (Universal Links iOS / App Links Android)
- **App Tracking Transparency (ATT)** consent → Pillar V Telemetry 必須遵守
- **Mobile rendering tier detection** (Adreno / Mali / Apple A-series → graphics preset)
- **Battery-saver mode** + **thermal throttling response** (iOS/Android thermal state →
  Pillar P FrameGovernor 拡張)
- **App size optimization** — iOS On-Demand Resources / Google Play Asset Delivery →
  Pillar P streaming 統合
- **Touch UI** — UiKit `TouchFirst` mode (v10 reserved)、tap zone ≥44pt (iOS HIG)、
  gestures (swipe/pinch/tap-hold/long-press)、virtual joystick overlay
- **China region** — 別 distribution (no Google Play in CN)、local review、VPN 検出
- **Compliance** — COPPA mobile stricter、children's category、loot box disclosure (Apple/
  Google policies)

### 17.6 ニッチ・倫理 3 領域

#### Education / 認知支援 / AAC (Pillar H §16.4 a11y 拡張、Pillar H `a11y/edu/` `a11y/aac/`)
- **A11yProfile に preset 追加**: `Dyslexia` / `Adhd` / `Autism` / `Dyscalculia` /
  `Dysgraphia` / `Aphasia` / `Aac`
- **Dyslexia 深度** — OpenDyslexic/Dyslexie/Lexie Readable font fallback chain、character
  spacing 1.2x、line spacing 1.5-2x、color overlay (Pillar Q RenderPassRegistry)、
  syllable highlighting / bigram coloring (libphonet seam)、**LLM auto-rewording for
  clarity** (Pillar U 統合)
- **ADHD 支援** — reduced visual clutter mode / single-task focus / 段階的 animation off /
  Pomodoro break timer
- **Autism / sensory-friendly mode** — sensory profile (flash/sudden audio/shake/movement
  reduction)、**sensory preview** (level 開始前に「containing loud sounds, flashing lights」)、
  **predictability mode** (no jump-scare, no surprise mechanics)
- **AAC (Augmentative and Alternative Communication)** — `IAacBridge` seam (Proloquo2Go /
  TouchChat / LAMP / Tobii Dynavox / CoughDrop)、**ARASAAC** open license symbol library
  同梱、**communication board overlay** (UI で picture board)、**switch-control scanning**
  (Pillar v11 §16.4 拡張、scan speed configurable)、game が **AAC 入力 source + AAC 出力
  format** 両方サポート — **Pillar D `InputMap` "language source" として `framework 全体
  が知る**
- **Educational metadata** — age range / 学習概念 / curriculum alignment (Common Core
  等) + **`IParentDashboard`** seam (heavy children's data privacy COPPA/GDPR — `MinorProfile`
  必須 opt-in、コンパイル時 trait check)
- **Dyscalculia / Dysgraphia / Aphasia** — math 緩和 + 計算機 / 音声入力 + 大 touch / アイコン
  ベース communication
- **Game-as-therapy seam** — EndeavorRx 風 FDA-approved therapeutic、session tracking +
  clinician reporting (HIPAA seam)、niche real
- v1 = profile presets + AAC switch scanning + ARASAAC、v1.1 = `IAacBridge` concrete +
  parent dashboard + educational metadata、v2+ = clinical/therapeutic tooling

#### Web3 / Blockchain / NFT — **honest recommendation against**
- **`ACS::Web3Bridge` 独立モジュール** (default OFF、CMake opt-in、framework 単体では
  1 byte もリンクしない)
- **`docs/Web3Recommendation.md`** で **明示的に反対**:
  - 規制リスク (US SEC / Japan FSA / China total ban / EU MiCA 2024 / South Korea P2E ban)
  - 環境懸念 (PoS で軽減も評判残存) / scam association / 技術複雑度 / 高 churn (2022-2024 で大半失敗)
  - **audience hostility** (Steam が blockchain ban、itch 推奨せず、player default-distrust)
  - 税務 (NFT 売買は most jurisdictions で taxable)
  - AML (FinCEN reporting)
- **Minimal seam if developer insists**: `IBlockchainBacking` 抽象 (wallet 接続 / 所有 query /
  signing)、**cosmetic-only NFTs as receipts** (gameplay-affecting NFTs は絶対避ける)、
  **read-only by default**
- **Decentralized features without crypto**: server-side trade with escrow / server-side
  limited drops / 既存 AssetPack HMAC で user-owned content signing
- v1 = doc only、v1.1 = `IBlockchainBacking` seam、v2+ = concrete (only if community 強要)

#### 物販 / Physical Goods / SKU (Pillar O Entitlement 拡張)
- **`IRedemptionCodeService`** — boxed/cartridge 同梱コード redeem → backend validate →
  Entitlement (Pillar v11 §16.2 Live Ops Entitlement model) → AssetPack overlay 活性化
- **Collector's edition bonus** — soundtrack + art book PDF + in-game cosmetic、AssetPack
  overlay 経由配信、`DigitalArtBookScreen` UiKit bundled
- **Goods QR/NFC** (`IGoodsQrScanner` seam) — phone QR scan → backend → in-game item。
  Sanrio/anime tie-in 日本市場で多い
- **Toys-to-life (NFC figurines)** — Skylanders/Amiibo 風、Switch/3DS/mobile NFC、niche、
  v2+
- **Pre-order DLC** (Steam 自動 + 物理店舗 (GameStop/EBGames) code-based)
- **Charity tie-ins** — `ICharityCampaign` (Humble Bundle 風 — purchase grants in-game items、
  proceeds to charity)
- **Region 別 physical SKU** + 言語 + 規制 metadata
- **Hardware bundles** — pre-installed detection via storefront API
- v1 = `IRedemptionCodeService` + AssetPack overlay unlock、v1.1 = `IGoodsQrScanner` + 
  collector's edition tooling、v2+ = Toys-to-life NFC + Charity campaign UI

### 17.7 全体アーキテクチャ — v12 で **24 ピラー A〜X** + メタ層

| 番号 | ピラー | 追加版 |
|---|---|---|
| A〜H | 基礎 8（H: AudioSystem + **音響高度化深度** + **a11y/edu/aac 拡張**） | v3 + v10 + v11 + **v12** |
| I-W | 既存 15 拡張ピラー（D: **exotic input**, F: **F3 物理高度化**, O: **物理 SKU**, S: **モバイル固有 IAP**, T: **ライブ配信統合**, U: **AI frontier vision-language/generative agents**） | v7-v11 + **v12** |
| **X** | **XR (VR/AR/MR) + 新興プラットフォーム** (Apple Vision Pro / Meta Quest / PSVR2 / SteamVR / Steam Deck 固有 / ARKit/ARCore) | **v12** |
| — | メタ層 | v10 |

加えて完成度システム 9 + ジャンルキット 7 + AssetPack + 著作ツール + 移植性 +
**Web3Bridge 独立モジュール** (honest 反対 docs 付き)。

### 17.8 実装フェーズ — v12 で ~95 フェーズへ

v11 までの ~80 + 以下挿入:

| Ph | 内容 |
|---|---|
| **9.6〜9.13** | Pillar H 音響高度化 v1 — `acs::dsp` プリミティブ + HRTF stub + 占有 (`OcclusionResolver` Centerline+Volumetric) + AudioAnalyzer (LUFS+onset+tempo) + Doppler + air absorption + Stinger + `33_HelloAudio3D` |
| **9.14〜9.19 (v1.1)** | `ACS::SteamAudioBridge` + Convolution reverb + Granular + Vocoder + Voice processing + Diffraction via NavGrid |
| **17.5〜17.9** | (既存 v9) F2 物理 |
| **(新) F3.1〜F3.7** | XPBD SoftBody → Cloth → Hair (v1.1) → Voronoi destruction → WaterVolume buoyancy → IGpuCompute seam → VehicleComponent |
| **X1〜X4 (v1)** | Pillar X XR seam (`IXrRuntime` + `XrCamera` + comfort settings + XR action manifest) |
| **X5〜X8 (v1.1)** | `ACS::OpenXrBridge` concrete + Quest standalone full + Pillar X comfort/locomotion presets |
| **X9〜X12 (v2)** | Vision Pro / PSVR2 / ARKit/ARCore bridges |
| **(AI 拡張) AI1〜AI8** | Multimodal LLM (vision) → Generative agents (memory/reflection/planning) → AI companion → AI quest gen → AI dynamic narrative → LLM behavior leaf → AI mod tools → runtime image gen (v2+) |
| **(入力/配信/モバイル) DTSM1〜DTSM6** | IExoticInput (DDR/wheel/HOTAS/MIDI/haptic suit/RGB) → Live streaming (OBS/Twitch chat/Crowd Control) → Mobile IAP (StoreKit 2/Play Billing 6) → Push notifications (APNs/FCM) → Mobile lifecycle → Touch UI |
| **(ニッチ) ED1〜ED5** | Education/AAC (a11y profile presets + ARASAAC + AAC switch scanning + IAacBridge + IParentDashboard) → Web3Bridge seam (doc + minimal IBlockchainBacking) → 物販 SKU (IRedemptionCodeService + AssetPack overlay unlock) → Goods QR/NFC → Toys-to-life (v2+) |
| **(v2)** | F3 SPH fluids + Pillar X Vision Pro full + AI runtime image gen + Toys-to-life NFC |

**前提**: Pillar X (XR) は v9 3D サポート (Ph28〜31) 完了後。Pillar F3 は F2 (Ph17.5〜17.9)
完了後。音響高度化は Pillar Q (lighting) と並行可。AI 進化は v11 Pillar U (S1〜S11)
完了後。Education/AAC は v11 §16.4 (Pillar H a11y) 完了後。
（v13 で全 pillar 内部深掘り — §18）

---

## 18. v13 — 全 24 ピラー + メタ + 完成度システム + ジャンルキット 内部深掘り

ユーザー指示「全 pillar それぞれ細かく深掘り、20 体エージェントで総出」を受け、**20
エージェント並列**で内部設計を**実装着手レベル**まで仕様化。各 pillar について
data structure / algorithm / edge case / integration / determinism / performance budget を
網羅。完全詳細はエージェント成果物 (~1.5MB) に保持し、本節は確定事項の索引。

### 18.1 Pillar A（App & Scene）内部
- **`Game::Run` フレーム順序確定**（`OnStart` → `OnUpdate` の中で: scene 遷移 → input poll →
  Scene::OnUpdate → Resolve transforms → fixed-step accumulator drain → Scene::OnDraw →
  GPU defer-delete pump → present → OnEvent / OnCustomFrame）
- **`SceneServices`**: `ESvc::* enum class : u64`（拡張余地）+ 固定配列 24 サービススロット
- **Scene 状態機**: Constructed/Spawning/Active/Paused/Suspending/Exiting/Destroyed
- **`SceneManager`**: push/pop/replace/swap、mid-frame 要求は fence point まで deferred
- **GPU 3-frame deferred deletion**: 仕様 v6 §11A 既知 — `IRhiDevice` に
  `CompletedFrameValue()` 追加が必要（v1 で確定）
- **`HeadlessGame`**: no-window swap-chain stub + null audio + null input、unit/integration test 用
- **エラー復旧**: `Scene::OnEnter` 失敗時は前 Scene へ rollback、unhandled panic 時は safe scene 復帰
- 関連: `[email protected]_01WBBvnab8nt9dajpEzaLLTg.json`

### 18.2 Pillar B（Object Model: Node2D + Node3D + ECS）内部
- **`Node2D` メモリ 184B / 3 cache line**（hot/cold 分離）、`Node3D` 224B / 4 cache line
- **`Component2D` ストレージ**: per-node `TArray<TUniquePtr<Component2D>>`（small-vector 不採用）+
  線形クエリ（≤16 component で THashMap より速い）
- **`NodeId` packed 4B**（24bit idx + 8bit gen、wrap で slot retire）
- **Dirty propagation**: `MarkWorldDirty` 下方再帰 + early-exit、`ResolveTransforms` 1 フレーム 1 回
- **Spawn/Destroy/Reparent queue**: fence point で順序適用（destroy→reparent→spawn→component op）
- **同フレーム spawn→destroy = キャンセル**（OnSpawn/OnDespawn 非呼出）
- **Lifecycle precondition matrix**: OnAttach（self 構築のみ）/ OnOwnerSpawned（全 attach 完了、
  他コンポ参照 OK）/ OnDetach（dangling 寸前、self rollback のみ）
- **HotSwapComponent**: 同 kind 同士のみ、`OnHotSwap(old)` で状態移植
- **Multi-tree composition**: Scene member 宣言順は **`_world_ecs` → `_root_3d` → `_root_2d`**
  で破棄逆順 → Node の dtor が ECS world を safely 触れる

### 18.3 Pillar C（時間・アニメ）内部
- **`Clock`**: wall_dt + game_dt + per-domain scale（Gameplay/Ui/Audio/Particle/Cinematic/Photo/Tween）
- **`Tween<T>`**: 型消去 union-based（FVec2/FVec3/FVec4/FQuat/Color/f32 の 7 種、heap-free）
- **Easing**: 30+ 標準カーブ LUT、Bezier custom（Pillar K curve editor 連動）
- **`Sequence` ノード型**: Seq/Parallel/Wait/Call/WaitForEvent/If/Loop、pause/resume mid-sequence
- **`StateMachine`**: HFSM（hierarchical）対応、push-down sub-state
- **`AnimationGraph`**: flat array IR、Pose Arena per frame、Inertialization（200 LOC 品質激変）
- **Retarget**: HumanoidSlot 22 種、骨長スケール + axis flip
- **決定論**: `SubsystemTrait<TweenTag>::profile = Required`、id-order 反復

### 18.4 Pillar D（Input）内部
- **`InputMap` は polling しない** — frame 頭に raw snapshot を受ける
- **`.acsr` 録音は raw を録る**（action ではない）→ binding 変更で recording が壊れない
- **`Binding` 16B tagged union**、`SmallVec<Binding, 4>` で 90% stack
- **集約規則**: Button OR / Axis1D abs-max / Axis2D length-max
- **Context 切替時の sticky 押下対策**: 次の Release まで suppress flag
- **Hot-plug**: XInput polling（接続後 60Hz / 未接続 1Hz）+ WM_DEVICECHANGE
- **`PlayerSlots`** は `Game` グローバル、`InputMap` は slot から借用
- **`IExoticInput::NonDetermin` フラグ**: 立つと lockstep ビルドで `Bind()` が失敗（コンパイル時）
- **Window lost focus** で全 Action 強制 Release（sticky 防止）
- **AZERTY 等ロケール対応**: scan code（位置）/ virtual key（意味）を別管理

### 18.5 Pillar E（Camera）内部
- **`Camera2D`**: position（中心）+ zoom + rotation_rad、回転は SpriteBatch::SetView では消費せず
  Camera2D::View() が直接 view matrix を計算（v6 既知問題対応）
- **4 Rig** (`Fps`/`Orbit`/`Follow`/`Cinematic`): `ICameraRig3D::Update(dt)` 経由切替（instant/blend）
- **`CameraStack`**: priority 順、Layer kind (`World3D`/`World2D`/`Ui2D`)、per-camera viewport+RT+post bypass
- **Split-screen**: `ConfigureSplit(HorizontalHalf/VerticalHalf/Quad, n)` 自動配置
- **RTT 寿命**: GPU 遅延削除キュー経由
- **Transitions 6 種**: Cut/Fade/PushL/PushR/WipeIris/WipeDiagonal、~200 LOC shader + scratch RT
- **`ParallaxLayer`**: factor 宣言 → `ResolveTransforms` が自動視差解決、SpriteBatch 無変更
- **Cinemachine blend graph**: priority + blend curves、pos lerp + rot slerp + FOV lerp
- **Shake trauma**: t² 重み、frequency+amplitude、Pillar I effect 経由発火

### 18.6 Pillar F/F2/F3（物理）内部
- **F kinematic `collide-and-slide`**: residual 移動 → normal plane slide → 4 iter max + step-up + slope
- **F2 PGS Sequential Impulse**: warm-start + Island + sleep + 8 vel iter + 3 pos iter + split-impulse
- **F2 8 joint**: Revolute/Prismatic/Distance/Weld/Rope/Wheel/Pulley/Gear、ソフト式 (freq+damping)、
  break impulse threshold
- **F2 Dynamic AABB Tree**: Catto 2019、loose factor で re-insert 削減
- **F2 CCD**: TOI bisection + conservative advancement + GJK distance + bullet flag
- **F2 Filter**: category_bits/mask_bits + group_index、`(a.cat & b.mask) && (b.cat & a.mask)`
- **F2 Material**: friction static+kinetic、restitution max-of、density auto-mass+inertia
- **F3 XPBD**: cloth/rope/jelly、distance + bending + volume + pin constraint、~600 LOC
- **F3 Voronoi destruction**: `acs_fracture` CLI pre-fracture、chunked (大→中→小→塵)
- **F3 2D SPH** (v1.1): ~1000 particles real-time、neighbor via SpatialGrid
- **F3 `IGpuCompute` seam**: DX12/Vulkan/Metal/WebGPU 抽象
- **決定論**: SSE2 固定、body sort by BodyId、AABB tree refit 同順序

### 18.7 Pillar G + J（Resources + Serialize/Reflect/Prefab）統合内部
- **`AssetRegistry`**: `THashMap<AssetId, TRc<Asset>>`、loader per-extension registry
- **`AssetFuture<T>`**: `TRc<AsyncLoadState>{CompletionCounter, result, error}`
- **`TypeInfo<T>`**: `ACS_REFLECT(T, ...)` macro generates specialization、TypeId = HashBytes(FQN) 64bit
- **`FieldInfo`**: 手動 tag 宣言（u16）+ version_added/version_removed + offset + kind + attr flags
- **`.atxt`**: S-expression line-based、git-diff-friendly、custom parser < 500 LOC
- **`.abin`**: TLV (`tag:u16, kind:u8, size:u32, payload`)、未知タグスキップ
- **`SceneDocument`**: `Scene ↔ SceneDocument ↔ .scene file` 2 段、`SerialNodeId` (depth-first preorder)
- **`PrefabSystem`**: Unity 流 override（per-field delta map）+ nested + cycle 検出
- **`DataAsset<T>`**: 型付き、`AssetRegistry::RegisterLoader<DataAssetLoader<T>>()`
- **`MigrationRegistry`**: chained migrations (v1→v2→v3)
- **参照**: `NodeRefT`/`AssetRefT`/`RcRefT`/`WeakRefT` 区別、cycle 解決パス

### 18.8 Pillar H（UI/Audio/Tools）内部
- **`UiKit Widget` tree**: 既存 ui/Widget 拡張、UiAnchor 9-point、margin/padding、min/max/preferred、focusable
- **`FocusManager`**: spatial nav（angular deviation + nearest center）、Tab cycle
- **`Screen` push/pop**: stack、OnPushed/OnPopped、modal 入力 capture
- **HUD widgets**: `BarWidget`/`CounterWidget`/`GaugeWidget`/`ToastWidget`/`MinimapWidget` + `Observable<T>` MVVM
- **`Random` (xoshiro128**)**: 既存 easy/Easy.cpp の xorshift32 を昇格、`RandomChannels` 命名規約
- **`TPool<T, N>`**: 固定 cap、generational handle、`SparseSet<T*>` live 反復
- **`DebugDraw`**: 即時モード、per-channel toggle（Physics.Shapes / Contacts / Joints / AABB / CCD / Pathfind / Trigger / SoundZones）
- **`RenderPassRegistry`**: 5 phase ping-pong RT lazy alloc
- **`SpriteMaterial`**: 3 built-in (Outline/Dissolve/Flash)、material 切替で batch flush

### 18.9 Pillar I（Effects）内部
- **`EffectDef`**: `.effect.tdat` data asset、reflection-registered、actions = (delay, action) tuples
- **`EffectAction` POD union**: emit_particle/screen_flash/camera_shake/sprite_flash/spawn_sub/play_sound/spawn_decal/trigger_hit_stop
- **`EffectSystem`**: `TPool<EffectInstance, 256>`、`EffectHandle{idx, gen}`、auto-despawn
- **`EffectComponent`**: Node attach、auto-clean on owner destroy
- **Composition**: 爆発 = particles + flash + shake + hit-stop + sound、atomic 発火
- **Determinism**: visuals = `Random::Channel("visuals")`、gameplay-affecting = `Channel("world")`
- **TPool 枯渇**: drop new or steal lowest-priority

### 18.10 Pillar K（Editor / Dev Tools）内部
- **全 `#if ACS_GAME_DEBUG`** — ship では public header の no-op マクロのみ
- **`Inspector`**: reflection-driven field widget dispatch table（per-EFieldKind）
- **Picking**: viewport → Pillar F raycast → topmost node
- **Gizmo**: handle mesh + screen-space ray test pick + snap-to-grid
- **Undo/Redo**: reflection 駆動スナップショット、in-memory binary writer
- **`DevConsole`**: command registry + tab 補完 + history + CVar
- **`HotReload`**: `ReadDirectoryChangesW` async + debounce 50ms + per-type policy
- **`Profiler`**: per-thread scope ring + frame graph + GPU timestamp query
- **`Replay`**: input + RNG state + 5s state checkpoint → `.acsr`
- **`LiveTune`**: `LIVE(name, default)` macro → CVar + Inspector slider + dev settings 永続

### 18.11 Pillar L（AI / Gameplay Depth）内部
- **`BehaviorTree`**: flat node array、`BtNode{kind, flags, first_child, next_sibling}`、DFS tick
- **BT 構成**: Sequence/Selector/Parallel + Inverter/Repeater/UntilSuccess/Cooldown decorator + Action/Condition leaf
- **`Blackboard`**: per-BT instance `THashMap<FString, Variant{i32/f32/FVec2/FVec3/NodeRef/AssetRef}>`
- **`Pathfinding A*`**: heap open list (min-heap by f) + closed bitarray + parent chain、heuristic (Manhattan/Euclidean/Octile)
- **Async path request**: `AsyncPathRequest{start, goal, params, on_complete}` worker pool
- **Tilemap**: chunked (chunk_w × chunk_h)、per-tile property、Tiled `.tmx` 経由、auto collision + nav grid
- **`PerceptionComponent`**: cone (range + half-FOV + raycast occlusion)、`SenseGraph` 共有
- **`SpatialQuery`**: SpatialGrid radius/cone/box/ray batch、layer mask filter + distance sort
- **`Steering`**: Reynolds (seek/flee/arrival/pursuit/wander/flock/path-follow)、sum-of-weighted-forces
- **Behavior LOD**: Full/Half/Quarter/Tenth/Frozen tier（距離基準）

### 18.12 Pillar M（Network）内部
- **`UdpReliableTransport`**: 3-way handshake (CONNECT → CHALLENGE(nonce) → RESPONSE(hmac))、
  MTU 1200B、reliable 16bit seq + 32bit ACK bitmap + RTO 200ms→4s、8 channel multiplex、
  AIMD per-peer congestion control
- **`LockstepRunner`**: input forward、input delay 4-8 ticks、wait for all peers、60-tick state hash
- **State hash desync**: 各 peer の `[[Replicated]]` フィールド hash → mismatch → state dump
- **`NetDeterminism.h`**: `ACS_DET_REQUIRE`/`ACS_DET_FORBID_CALL`、SSE2 固定 (`/fp:precise`)
- **Deterministic sin/cos**: `acs::math::DetSinCos` polynomial approximation（MSVC/Clang 共通）
- **`[[Replicated]]` 属性**: codegen → `ComponentReplicationLayout` table、per-field (channel/rate/interp/relevance/epsilon)
- **`.acsr`**: Header (magic/version/level_hash/seed/total_ticks/HMAC) + delta-encoded input stream + 5s checkpoints

### 18.13 Pillar N（Mod / Lua）内部
- **Lua 5.4 per-mod `lua_State*`**: sandbox isolation、allocator → ACS arena、panic → `TResult` via `lua_pcall`
- **Binding generation**: template `AcsLuaBind<&Fn>` stub、Pillar J `TypeInfo<T>` 経由で field auto-expose
- **Userdata**: non-owning ref + generational handle check
- **`LuaComponent2D`**: per-instance Lua table、`OnUpdate`/`OnEvent` = Lua function
- **Hot-reload**: file change → reload chunk → reattach function、self table 保持
- **Sandbox API allowlist**: `io`/`os`/`debug.*`/`package.loadlib` 除去、curated wrapper
- **Memory quota**: custom Lua allocator が byte tracking、exceed → `LUA_ERRMEM` → TResult
- **CPU quota**: `lua_sethook(LUA_MASKCOUNT, N)` 命令数で time budget check
- **Trust levels**: LocalUntrusted / CommunitySigned / OfficialTrusted、quota スケール
- **Manifest**: `mod.toml` (id/name/version semver/author/depends/signatures/declared APIs)
- **Dependency 解決**: topological sort + cycle 検出 + version range satisfaction
- **`IModRepository`**: `LocalFolderRepository` default、Steam Workshop/mod.io/itch.io plug-in

### 18.14 Pillar O（Shipping / Live Ops）内部
- **`acs_bake` CLI**: 新エンジンモジュール `ACS::Bake`、`tools/acs_bake/` 薄 main
- **Per-asset pipeline**: BC1/BC7/ASTC texture / Ogg Vorbis or Opus audio / TTF→atlas font /
  glTF→ACS mesh / `.atxt`→`.abin` data
- **Hash-based incremental**: SQLite manifest of last-baked hashes
- **Build configs**: Debug/Dev/Profile/Ship、strip matrix per config（Pillar K dev tools等）
- **Git SHA stamp**: `acs_version_stamp.cpp` build-time generated
- **`ITelemetryClient`**: batched send (30s or 50 events)、GDPR opt-in、schema via Pillar J reflection
- **Crash dump**: `SetUnhandledExceptionFilter` → `MiniDumpWriteDump` + Pillar J Scene snapshot 同梱
- **`IFeatureFlagClient`**: server-pushed toggles、cached offline、A/B testing
- **`Entitlement` unified**: DLC/sub/beta access/gacha all share same table
- **`MonetizationDirector`**: IAP/premium currency/gacha 1 ハブ、receipt validation server-side
- **`EventDirector`**: server-driven season events、offline cache + grace period
- **`EolMode` seam**: servers shutdown → offline 継続（EU 消費者保護対応）

### 18.15 Pillar P（Scale / Streaming / Memory / Threading）内部
- **`World` chunk grid**: `THashMap<ChunkCoord, ChunkSlot>`、residency state machine
- **Async chunk load JobGraph**: Read → Deserialize → Register collisions → GPU upload (DeferredUpload)
- **Hysteresis**: 0.25-chunk band around boundary
- **Ghost proxy**: pre-baked 64x64 low-res "skyline" texture for distant chunks
- **`AssetStreamer` 5-tier priority**: bounded `TArray<AssetId>` ring per tier、cheap O(1)
- **LRU eviction**: `THashMap<AssetId, AssetCacheEntry>` walk LRU、evict if pin=0 + TRc::StrongCount==1
- **Mip streaming** v1.1: partial-residency [N..N-2] always、footprint → desired mip
- **`LoDController`**: sprite mip 自動選択 + sprite swap (near/far)、behavior LOD (BT tick stride)、anim LOD
- **物理は LOD しない**（tunneling 防止）
- **Origin rebasing**: > 4096 units で walk all Transform2D、`MessageBroker::Publish(WorldOriginRebased)`
- **`TPool<T, N>`**: fixed-cap、generational handle、sparse-set live iteration
- **`Task<>` C++20 coroutines**: custom allocator (Scene arena)、awaitables (WaitSeconds/Event/Sequence/LoadAsset)
- **`MpmcRing<T>` / `SpscRing<T>`**: Vyukov MPMC bounded、SPSC head/tail、lock-free
- **IO thread**: 別、long-blocking IO 隔離、SpscRing for completion
- **`FrameGovernor`**: target ms、tier (Full/Reduced/Min/Off)、degradation order: DebugDraw → DistantBgm
  → Particles → AiPerception → AiBehavior → PostProcess → AssetStreaming → UiAnimation。**物理/入力/Scene
  transition は絶対 throttle しない**

### 18.16 Pillar Q（Visual World / Lighting）内部
- **Deferred 2D G-buffer**: albedo (RGBA8) + normal (RGB10A2 with .a=depth_layer u8) + emissive (R11G11B10F) + light (R11G11B10F HDR)
- **Light pass**: per-light fullscreen quad with blend、normal sample → Phong/Lambert
- **Light cookies/gobos**: 投影テクスチャ
- **2D 影**: visibility polygon (collision polygon ray cast)、~300 LOC
- **Sprite normal**: 手描き or alpha→height auto-generate
- **`AmbientDirector`**: ToD (1 hour = N sec) + 天気 + 季節 + 雷、key cubic interpolation
- **Mood snapshots**: 領域ごと完全ルック（light + audio + LUT + particle ambient）
- **Decals**: pool + fade-out timer、per-region decal RT
- **Sprite destruction**: per-sprite RT、Pillar F2 contact callback で paint
- **Foliage 反応**: trample velocity field rasterize → foliage shader sample
- **Water**: refraction + waves + reflection + caustics tile shader
- **Tile-based light culling**: 16x16 px tiles、per-tile light list

### 18.17 Pillar R（Polish & Game Feel）内部
- **Per-Node time scale**: lazy resolve (parent_effective × local)、dirty_seq cache、`TimeDomain` enum
  (Gameplay/Ui/Audio/Particle/Cinematic/Photo/Tween)
- **Tween handle scope**: `target_node_handle + domain`、ノード破棄で世代不一致 auto fail-safe
- **Cinematic blend graph**: priority + pos lerp + rot slerp + FOV lerp
- **Photo mode**: pause + free cam (3-axis pan + zoom + rotate) + filter via Pillar O color grading + EXIF
- **`TutorialFlow`**: declarative `(step_id, prompt, predicate)` data asset
- **Highlight target**: Pillar I effect dim + pulse outline
- **Settings depth**: GPU detection → preset 推奨、first-frame perf measure (60 frames) → preset 自動調整
- **Difficulty preset**: Story/Normal/Hard/Hardest、CVar adjustment
- **Assist Mode**: granular slider (invincibility / infinite stamina / dash count / speed_mult)
- **Speedrun timer**: in-game UI + splits + golden splits
- **Streamer mode**: `IsBeingStreamed()` detection (OBS process scan) + 手動 toggle
- **Death cam**: slow-mo Tween (1.0 → 0.2 over 1s) + camera zoom + focus killer

### 18.18 Pillar S + V（Storefront + Backend）統合内部
- **`PlayerIdentity`**: Steam/Epic/Xbox/PSN/Switch/GameCenter/Google/itch/GOG/Discord/GameAccount 統一
  + cross-progress + expiring `session_token`
- **`ACS::SteamworksBridge`**: 独立 CMake module、`SteamAPI_RunCallbacks` per frame、
  `SteamFuture<T>` で CCallResult を TResult-style 化、achievements/lobbies/SDR/Workshop/Cloud/Input/
  Rich Presence/Big Picture/Deck/DLC/Family Sharing/VAC/crash upload 完全実装
- **`Game::Tick` hook**: `SteamAPI_RunCallbacks()` 1 回/frame
- **Dedicated server build**: `acs_module(TYPE Server)` no-render no-audio、`Scene::PeerRole::DedicatedServer`
- **Docker / Kubernetes seam**: AWS GameLift / Azure PlayFab / Google Cloud Game Servers
- **`IMatchmaker`**: Glicko-2 (rating + RD + vol) or TrueSkill、region-based RTT、queue UX、anti-smurf
- **`IBackendClient`**: REST/gRPC + JWT、`MockBackendClient` for local dev、多 backend 並列
- **`IAntiCheat`**: VAC/EAC/BattlEye/nProtect/Vanguard/custom、**honest**: client-side は theatre、
  server-authoritative + replay verify が real
- **Telemetry backend**: `ITelemetryBackend` (PostHog/Amplitude/Mixpanel/GameAnalytics)、funnels/segments/heatmaps

### 18.19 Pillar T + W（Community + Studio Workflow）統合内部
- **`IFriendsService`**: Steam/Epic/Xbox/PSN/Switch friends + `PlayerIdentity` mapping
- **`PartySystem`**: PartyId、leader 制御、auto voice route、cross-scene 永続
- **`IVoiceChat`**: Steam Voice (free) / EOS Voice (free, cross-platform) / Vivox / Discord SDK /
  自前 Opus + INetTransport、**spatial 3D voice** (AudioSystem listener 統合)
- **Text chat**: channel (Global/Team/Party/Whisper) + profanity filter + slow-mode + 翻訳 seam
- **`IPresenceService`**: Steam/Discord/Xbox Rich Presence + joinable status
- **Sharing**: F12 screenshot + photo mode export + replay (.acsr) Workshop upload + highlight clips
  (auto last 30s on achievement)
- **`PlayerBlocklist`** + `IModerationBackend` (Discord T&S / Vivox Safety / 自前)
- **Pillar W**: `AssetLockService` (git-LFS pattern) + `acs_review` semantic asset diff + `acs_buildfarm`
  (sccache + asset bake cache) + VSCode LSP for `.atxt`/`.tdat`/`.bt`/`.dlg`/`.chart` + Rider plugin +
  VS natvis + `acs_repro` (replay + state snapshot + crash dump 1 命令で local 再現) + `acs_new` CLI
- **C++ hot-reload** v1.1+: `IHotReloadCpp` (Live++ commercial / RCC++ open)

### 18.20 Pillar U + X（AI/ML + XR）統合内部
- **`IMlRuntime`**: OnnxRuntime/DirectML/CoreML/NNAPI/WinMlNpu backend、STL/exception は `AcsOrtAdapter`
  pImpl で absorb、quant tier auto-selection (Best: hardware capability で fp32/16/int8/4 切替)
- **GPU memory integration**: `MlModel::MemoryBytes()` → Pillar P `GpuMemoryBudget`
- **`ILlmBackend`**: streaming token callback、`HybridLlmRouter` (cloud→local→scripted fallback 3 段)
- **`LlmSafetyPipeline`** 必須通過: input validation → system prompt + character card → budget check
  → LLM call → output validation (refusal/PII/jailbreak/content rating) → scripted fallback
- **`AiContentBadge`** UiKit widget: EU AI Act 対応
- **`AiCostTracker`**: per-session/day/month budget、feature priority (critical > NPC > hint)
- **`IXrRuntime`**: OpenXR session states、`xrWaitFrame`/`xrBeginFrame`/`xrEndFrame`、late-latch HMD pose
- **`ACS::OpenXrBridge`**: openxr_loader.dll、action manifest (interaction profile)
- **`XrInputProfile`**: layer above Pillar D `InputMap`
- **Spatial UI**: `SpatialUiCanvas`/`WristMenuCanvas`、curved canvas、laser pointer/direct touch/gaze+pinch
- **Comfort**: locomotion modes (Teleport/Smooth/Dash/Blink) + snap turn + vignette + 90Hz min/120Hz target
- **Apple Vision Pro 固有**: passthrough + Persona + eye+pinch primary input + SwiftUI bridge
  (`IXrShellBridge`)
- **Biometric privacy**: eye/body tracking + Persona + spatial scanning は `PrivacyDirector` strict opt-in

### 18.21 メタ層（横断接合）内部
- **`TypedHandle<Tag>`**: 2 flavor — Category A `GenHandle{u32 idx, u32 gen}` 8B for generational
  (Entity/Node/Body/Joint/Timer/Tween/Sequence/Effect/SoundVoice)、Category B opaque enum class via
  `ACS_OPAQUE_ID(Name, u32)` 4B (Asset/CVar/Material/EventChannel)
- **`HandleRegistry<T, Tag>`**: pool + gen counter + free list + retired index list
- **`SubsystemTrait<Tag>::profile`**: Required/BestEffort/Unsupported、compile-time `static_assert`
- **`acs::math::DetSinCos`**: polynomial approximation、MSVC/Clang/Linux fixed
- **`acs_test`** 6 種別: AutoReg via static init、JUnit XML、`HeadlessGame` for integration、
  PropertyHarness, golden SSIM for visual, baseline.json for benchmark
- **`Events.h` central**: ~30 cross-pillar events + `EventSink` RAII
- **SemVer**: `foundation/Version.h` constexpr + git SHA embed + `ACS_DEPRECATED_SINCE`
- **`acs_lint` rules**: R001-R048 clang-tidy custom AST visitor
- **`PrivacyDirector`**: opt-in flags、GDPR `ExportUserData`/`DeleteUserData`
- **`Logger`**: channel filter + runtime CVar toggle + crash dump に Scene snapshot 同梱

### 18.22 完成度システム 9 内部
- **`LocalizationDirector`**: `LanguageDef{id, name, strings_path, fonts, layout}` + `Tr/EFormat/Plural` +
  argument substitution + pseudoloc + locale-specific plural rules
- **`UiKit`**: 既存 widget tree 拡張 + 9-point anchor + min/max/preferred + FocusManager spatial nav +
  Screen push/pop + HUD widgets + Observable<T> MVVM
- **`Dialogue`**: `.dlg` graph (Line/Choice/Jump/Set/Call/Wait/End) + typewriter (per-char timing) +
  CutsceneTrack (Sequence with game-aware steps)
- **`DialogueVars`**: flag map + named ints、SaveArchive 永続化
- **`MusicDirector`**: buses + ducking + layered stems (intensity curve) + stingers (next beat/bar) +
  beat clock (sample-accurate)
- **`Progress`**: Stats (Add/Max/Set/Get) + AchievementDef (Manual/StatThreshold/MilestoneSet trigger) +
  Fraction (progressive) + IAchievementSink seam
- **`Accessibility`**: state struct (text_scale/colorblind/captions/shake/flash/motion/hold/ui_anim/contrast)
  + AccessibilityChangedEvent + profile presets
- **`Starter2D`**: Character2DController (coyote-time + jump buffer + variable jump + slope) + TopDown +
  Camera presets + Health2D/Hurtbox/Hitbox/Patrol/Pickup/Lifetime/Spawner + Template scenes
- **`SaveSlots`**: header (timestamp/playtime/thumbnail/location/progress) + payload split、atomic write
  (.tmp + rename)、CRC32 + HMAC、autosave dedicated slot

### 18.23 ジャンルキット 7 内部
- **VN kit**: PortraitStage 5 slots、BackgroundStage transitions (Cut/CrossFade/Dissolve/Iris/Wipe 8 dirs)、
  AutoSkipController (Manual/Auto/Skip/SkipUnread)、SavePerChoiceTracker (BranchSnapshot push)、
  ReadStateRegistry global persist
- **Roguelike kit**: TurnScheduler (priority queue by next_act_tick)、shadowcasting FOV 8-octant、
  MapMemory bitset + last-seen actors、IdentificationRegistry (per-run cosmetic + global identified persist)、
  AutoNav (BFS to unexplored, halt on FovChangedSignal)
- **Tactical kit**: Board W×H×Z (height)、InitiativeQueue scrolling sidebar、RangeOverlay (BFS flood-fill
  with terrain cost)、AOEPreview (run skill AreaShape over cursor)、AICoach scoring (move-attack pair
  enumeration + threat scoring)、StatusEffectRegistry (Cards 共有)
- **SHMUP kit**: BulletWorld ECS 50k cap、Pattern DSL (Single/Concentric/Spiral/Wave/Aimed/RandomSpread/
  Formation/Burst/Delay/Parallel/Sequence composable)、player hitbox tiny (4x4) + graze ring (~16px)、
  BossDef phased multi-stage、ReplayBinding (InputRecorder + RandomChannels::Snapshot)
- **Rhythm kit**: AudioBeatClock from `MusicDirector` sample position、Judger window (Perfect 20ms / Great
  50ms / Good 100ms / Miss)、Calibration (metronome tap → median error → audio_offset_user)、
  Lane configs (4/5/7/DJ/Taiko)
- **Cards kit**: EffectStack LIFO (Push pops resolves recursively)、Effect tree (Damage/Block/ApplyStatus/
  Draw/Discard/Heal/Sequence/If/ForEachTarget/Callback)、seeded shuffle (Random::Channel("deck"))、
  enemy intent BT
- **Idle kit**: BigNumber 128-bit fixed or (f64 mantissa, i64 exponent) + 1e308 超で arbitrary precision、
  BigNumberFormatter (Scientific/SuffixShort/SuffixLong/NamedTier/Engineering)、OfflineProgress analytic
  (geometric series for constant-rate)、LowCpuTicker (unfocused = 1Hz)
- **クロスキット共有契約**: `RandomChannels` 命名規約 (world/loot/ai/deck/visuals/audio/ui) +
  `.acsr` KitBlob header拡張 + assets/<genre>/<category>/*.tdat 配置規約

### 18.25 AssetPack + CLI ファミリ + 著作ツール + Platform 13-seam 内部
- **`.acpak` 64B ヘッダ bit-level**（magic + format_version + min_reader_version + archive_flags
  6 bit + toc_offset/count/stride/plain_size + blob_offset/size + header_hash）
- **TOC record 64B**（path_hash + blob_offset + stored_size + original_size + content_hash +
  entry_flags + path_offset/length + crypto_nonce[12]）
- **5 ヘッダ不変条件**: magic 一致 / forward+backward 互換 / header_hash 検証先 / offset 境界
  チェック / stride ≥ 64
- **AES-256-GCM via BCrypt**: thread-local 算法ハンドル + `BCRYPT_KEY_HANDLE` per-thread cache、
  per-entry 12B nonce = **CSPRNG 8B + monotonic counter 4B** (extended nonce 構成で nonce reuse
  構造的不可能)、AAD = `path_hash || original_size` (cut-and-paste 防止)
- **TOC nonce derive**: `HashBytes(header_hash, "TOC_NONCE")` 決定論的（archive 一意なので
  collision 64bit hash strength）
- **4-fragment XOR mask key provider** + 4 別 TU 配置（`.rdata$assetpack_a..d`）+ build-time
  mask 自体も XOR 化 + `salt` は `cmake/assetpack_salt.bin` (gitignored、別 build 別 salt) →
  SHA-256(material || salt) KDF → 32B AES key、material 即 SecureZeroMemory
- **代替 key provider**: `FileKeyProvider` (DPAPI 保護 cached file) / `ServerKeyProvider`
  (backend 経由 first-launch fetch) / **`HybridKeyProvider`** (binary obfuscated ⊕ server-provided —
  binary 単独 compromise では復号不能)
- **VFS lookup hot path**: NormalizeLogicalPath (packer/runtime/editor 全て同一 C++ 関数) →
  HashBytes → 優先度降順 stack walk → 各 source の binary search、~10ns + log₂(mounts)
- **ArchiveSource**: `CreateFileMapping` + `MapViewOfFile` (mmap)、TOC 即時復号 + immutable
  `TArray<TocRecord>` (lock-free read)、per-thread BCRYPT_KEY_HANDLE lazy alloc
- **Compress-then-encrypt + 97% 安全則**: compressed_size >= original*0.97 → STORE_RAW 切替、
  archive は loose dir より絶対大きくならない保証
- **LZ4 invocation**: writer = `LZ4_compress_default`、reader = **`LZ4_decompress_safe` 厳守**
  (`_fast` は corrupt input で UB)、>2GB は STORE_RAW 強制
- **3-layer integrity**: header_hash (corruption) / GCM tag (tamper with key) / content_hash
  (bit rot + codec bug)
- **`OBFUSCATED_BLOB` flag**: GCM 後の outermost XOR layer (64B rolling key、`binwalk`/`strings`
  対策、casual extraction floor 上げ)
- **`acs_assetpack` CLI**: pack/list/verify/extract/update、parallel worker (per-entry
  compress+encrypt)、atomic rename (`.tmp` → final)
- **`acs_bake` IBaker registry pattern**: texture (BC1/3/5/6H/7/ASTC/normal encoding
  reconstruct-Z/octahedral) / audio (Vorbis/Opus + loop point sidecar) / font (subset+MSDF+
  fallback chain) / mesh (glTF/FBX + meshlet + LOD) / data (`.atxt`→`.abin`)
- **Hash-based incremental**: 自作 `.bakemanifest` バイナリ（SQLite 不採用 — 600KB + STL +
  exception で ACS 規約と衝突、~200 LOC 自前）、key = (input_content_hash, baker_version,
  settings_hash)
- **`acs_chunkbake`** (Pillar P): 8-direction impostor billboard + simplified collider +
  simplified ambient audio per chunk → `.ghost`
- **`acs_lightmap`** (Pillar Q): xatlas UV2 + 2048² atlas pack + GPU path tracer compute shader
  (64 rays/texel) + Open Image Denoise + BC6H 出力
- **`acs_atlas`**: 自前 MaxRects + 9-slice metadata + SDF/MSDF + outline auto-gen
- **`acs_fracture`**: random/gradient/grid-jittered seed → 自前 Voronoi (Voro++ 流) → mesh clip
  → 隣接グラフ + per-chunk COM/inertia tensor、deterministic seed
- **`acs_test`**: 6 種別ランナー（unit/integration via HeadlessGame/property w/ PropertyHarness/
  determinism/visual w/ SSIM/benchmark w/ baseline.json）+ JUnit XML + `--parallel N`
- **`acs_det_test`**: `record`/`replay`/`verify` 3 段、state hash dump 比較で first-divergence
  frame 特定
- **`acs_replay_verify`** (Pillar M): server-side headless re-sim + HMAC-SHA256 検証 +
  sandbox container + time-limited
- **`acs_lint`** rules R001-R048: clang-tidy custom AST visitor、JUnit XML、`// acs-lint:
  disable R042` per-file disable
- **`acs_licenses`**: `docs/Licenses/THIRD_PARTY.md` 構造化 frontmatter → `Licenses.cpp`
  embed、`Game --licenses` CLI + UiKit `LicenseScreen`
- **`acs_loc_extract/diff/validate`**: libclang AST で `Tr("...")` call extract → `.pot`、
  3-way merge for translator handoff、CI fail on missing keys
- **`acs_diff`** semantic asset diff: Pillar J 反射経由で `.scene/.prefab/.tdat` を normalized
  canonical form 化 → 木構造再帰 diff、unified-diff for `git diff` driver
- **`acs_assetinspect`**: asset 別 dump (texture/audio/mesh/scene/font/lightmap/acpak)、
  human-readable summary
- **`acs_repro`**: zip extract → 検証 → `--repro-mode` 起動 + `--replay` + `--state` + debugger 接続
- **In-game UiKit 著作ツール**: Particle Editor (module composition + curve subcomponent +
  live preview)、Animation Curve Editor (Bezier handle modes mirrored/broken/aligned + preset
  library)、**BT Visual Editor with live debugger** (attach to running entity、現在実行 node
  yellow/success green/fail red、blackboard live table、Step button)、Level Editor (tile painter
  with auto-tile rules + prefab brush stamp mode + gizmo with grid/entity snap + per-scene undo
  stack)、Cinematics Timeline Editor (ripple edit + multi-track)、Docs Viewer (markdown render +
  full-text search index)
- **Bridge module 5 step pattern**: (1) `src/bridges/X/` 配置 (2) `WITH_ACS_X` CMake flag
  (3) `IY : IXSeam` 実装 (4) factory 登録 at init (5) `THIRD_PARTY.md` 追加。**License-tainted
  code stays in bridge**、core にシンボル leak なし
- **Platform 13 seam**: `PlatFile` (sync IO) / `PlatDir` (enum、新規 seam) / `PlatWatch` (file
  changes、Win `ReadDirectoryChangesW` + Linux inotify + Mac FSEvents) / `PlatMmap` (CreateFileMapping
  + MapViewOfFile) / `PlatAudio` (XAudio2/PipeWire/CoreAudio/SDK) / `PlatInput` (XInput/evdev/
  IOKit/SDK) / `PlatThread` (Thread/Mutex/RWLock/CondVar/Semaphore/Atomic/ThreadLocal/SpinLock)
  / `PlatTime` (QueryPerformanceCounter/clock_gettime CLOCK_MONOTONIC) / `PlatCrypto`
  (CNG/libsodium/CommonCrypto/SDK) / `PlatLocale` (MultiByteToWideChar/iconv+ICU) /
  `PlatProcess` (CreateProcessW/fork+exec) / `PlatCrash` (SetUnhandledExceptionFilter +
  MiniDumpWriteDump / breakpad / mach exception ports) / `PlatWindow` (HWND/NSWindow/SDK + Diligent)
- **`acs_lint R015` 憲法的施行**: `<Windows.h>`/`<windows.h>`/`<winsock2.h>`/`<bcrypt.h>` を
  `src/platform/**` 外で禁止
- **Linux/Mac/Switch concrete**: `platform/linux/` (libsodium + PipeWire + libevdev+libudev +
  breakpad + SDL2 minimal for windowing) / `platform/macos/` (CommonCrypto + CoreAudio +
  GameController + Metal layer) / `platform/switch/` (NDA、private repo + patch overlay)
- **`wchar_t` 幅問題**: Windows 16bit / Linux+Mac 32bit → `const wchar_t*` Windows /
  `const char*` Unix overload + `acs::path` namespace で bridge

### 18.26 ACS 規約クロス不変条件（10 項目）

1. **`NormalizeLogicalPath` 単一真実**: packer/runtime/editor/hot-reload watcher 全て同 C++ 関数
2. **`TResult<T,E>` + `ErrCategory::Crypto`** 等のサブカテゴリ、no exception、全 CLI 同パターン
3. **`acs_lint R015` で `<Windows.h>` を `platform/` 外で構造的禁止**、13 seam が唯一エントリ
4. **Bridge module pattern**: 独立 CMake / opt-in flag / no core symbol leak / license 隔離
5. **All asset formats use Pillar J reflection** for diff/inspect/migrate
6. **CLI tools are thin shims over libraries**: 同 lib を editor/CI/server-replay-verifier から呼べる
7. **Hash-incremental bake**: 共通 `.bakemanifest` 自前バイナリ format
8. **In-game UiKit authoring tools** (not external apps): UiKit/reflection を全箇所で共用
9. **Determinism auditable end-to-end**: `acs_det_test` + `acs_replay_verify` + 全 pillar
   `SubsystemTrait<Tag>::profile` 宣言
10. **Honest naming**: AssetPack は「asset obfuscation」（never encryption）、`Web3Bridge` は
    explicit doc-level recommendation against、AI runtime gen は明示的禁止 — docs is the contract

### 18.27 設計書サマリ — 実装着手可能性

全 24 pillar + メタ + 完成度 9 + ジャンルキット 7 + AssetPack + 著作ツール の **内部設計が
data structure / algorithm / edge case / integration / determinism / performance budget 単位で
仕様化済**。エージェント成果物 (`tool-results/toolu_*.json`、合計 ~1.5MB) は実装担当が「設計書なしで C++ を書き始められる粒度」。

実装ロードマップは v12 §17.8 の **~95 フェーズ** + 内部設計詳細の参照で着手可。Pillar A (Ph1)
から開始、依存に従い順次 Pillar B/C/D/E (基礎 8) → Pillar I+H+L (中位) → Pillar J 反射 (中心)
→ Pillar K/M/N → Pillar O/P (出荷インフラ) → Pillar Q/R (visual/polish) → Pillar S/T/V/W
(platform/social/backend/studio) → Pillar U/X (AI/XR) → ジャンルキット 7。

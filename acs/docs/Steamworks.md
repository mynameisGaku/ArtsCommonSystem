<!-- SPDX-License-Identifier: Apache-2.0 -->
# Steamworks SDK 統合

ACS は **Steamworks SDK** を `acs_steamworks` モジュールとして統合する。デフォルトは OFF (= `CSteamworksBridgeStub` no-op)、明示的に ON にしたときだけ real backend がビルドされる。

## アーキテクチャ

```
+-------------------------------------+
|  acs::game::ISteamworksBridge       |  ← 純粋仮想 I/F (interface seam)
|  (src/gameframework/SteamworksBridge.h) |
+-------------------------------------+
            ▲                ▲
            │                │
+-----------+--+    +--------+---------------+
|  CSteamworks |    |  CSteamworksBridgeImpl |  ← real backend
|  BridgeStub  |    |  (src/steamworks/      |
|  (no-op)     |    |   SteamworksBridgeImpl.cpp) |
+--------------+    +------------------------+
   default               ACS_BUILD_STEAMWORKS=ON
```

`ISteamworksBridge`が安定したAPI seamを定義し、CMakeで選択したbackendをその背後へ束縛します。

## API 一覧 (21 メソッド)

bridge APIは初期化とplayer state、非同期service、cloud／workshop／voice入力を分離します。

### 必須機能

| API | 説明 |
|---|---|
| `Init()` | `SteamAPI_Init()` |
| `Shutdown()` | `SteamAPI_Shutdown()` |
| `IsInitialized()` | state flag |
| `GetLocalPlayer()` | SteamID + persona name |
| `UnlockAchievement(name)` | 実績解除 |
| `SetLeaderboardScore(board, score)` | リーダーボード score 投稿 (async) |
| `GetLeaderboardScore(board)` | リーダーボード自分の score 取得 (async) |
| `Tick(dt)` | `SteamAPI_RunCallbacks` 毎フレーム |

### 拡張機能

| API | 説明 |
|---|---|
| `SetStat(name, i64)` / `GetStat(name)` | int32 stat (内部 i32 clamp) |
| `SetFloatStat(name, f32)` / `GetFloatStat(name)` | float stat |
| `IsDlcOwned(app_id)` | DLC 所有チェック |
| `SetRichPresence(key, value)` | rich presence |
| `GetFriendCount()` / `GetFriendByIndex(i)` | フレンドリスト |

### 追加機能

| API | 説明 |
|---|---|
| `CloudWriteFile(path, data, size)` | Steam Cloud 書き込み |
| `CloudReadFile(path, buf, size)` | Steam Cloud 読み込み |
| `CloudFileExists(path)` | Cloud ファイル存在チェック |
| `CloudDeleteFile(path)` | Cloud ファイル削除 |
| `CloudGetQuota(avail, total)` | Cloud quota 取得 |
| `WorkshopGetSubscribedCount()` | Workshop subscribed item 数 |
| `WorkshopGetSubscribedItem(i, item_id, install_path)` | item 情報取得 |
| `VoiceStartRecording()` / `VoiceStopRecording()` | VoIP 録音制御 |
| `VoiceGetCompressed(buf, size)` | 圧縮音声取得 |
| `InputInit()` / `InputGetControllerCount()` | Steam Input |

## SDK 構成契約

CMake構成はlocal SDK、管理済みarchive、stubのいずれかを明示的に選択します。

### ローカル SDK

`ACS_BUILD_STEAMWORKS=ON`では、`ACS_STEAMWORKS_SDK_DIR`が展開済みSDK rootを示す必要が
あります。必須headerまたはlibraryを解決できない場合、configureはreal backendを生成せず
失敗理由を返します。

### 管理済み SDK archive

`ACS_STEAMWORKS_SDK_URL`はbuild環境が管理するSDK archiveを入力として受け取ります。
archiveを取得または展開できない場合はconfigureを失敗させ、stubへ暗黙fallbackしません。

### Stub backend

`ACS_BUILD_STEAMWORKS=OFF`ではSDKを要求せず、`CSteamworksBridgeStub`を選択します。

`CSteamworksBridgeStub` が DI される。実績解除等の呼び出しは `kSubSteamworksNotImplemented` エラーを返すが、ゲーム挙動には影響しない (例外も投げない)。

## ランタイム要件

real backendはAppID、client session、runtime DLLを初期化前に解決します。

### `steam_appid.txt`

real backendはexeと同じディレクトリの`steam_appid.txt`から10進AppIDを読み取ります。
fileがない、内容が10進数でない、または実行環境のAppIDと一致しない場合、初期化を失敗させます。

### Steam クライアント

`SteamAPI_Init()` は **Steam クライアントが起動していないと false を返す**。dev 時は Steam にログイン状態を保つこと。

### `steam_api64.dll`

real backend を使う場合、`steam_api64.dll` (= SDK の `redistributable_bin/win64/steam_api64.dll`) を exe と同じディレクトリにコピーする必要がある。現在は自動コピーしないため、配置漏れを起動前に確認する。

## ゲーム側の backend 契約

`WITH_ACS_STEAMWORKS` 有効時は `CSteamworksBridgeImpl` が実backendを所有し、
初期化失敗時は `CSteamworksBridgeStub` へ切り替えられます。選択したbridgeはframeごとに
`Tick` でcallbackを処理し、application終了前に `Shutdown` でbackend資源を解放します。
achievement、stat、leaderboard、DLCの操作は `ISteamworksBridge` の結果で成否を返します。

## トラブルシューティング

| 症状 | 原因 / 対策 |
|---|---|
| `Init()` が `kSubSteamworksInitFailed` で fail | Steam クライアント未起動 / `steam_appid.txt` 不在 / AppID 不一致 |
| Achievement 解除しても見えない | 選択した AppID に対応するachievement定義がない、または別AppIDで初期化されている |
| `steam_api64.dll not found` ランタイムエラー | DLL を exe ディレクトリにコピー (`<SDK>/redistributable_bin/win64/steam_api64.dll`) |
| Leaderboard が 0 を返す | callback pumpが進むまでasync結果はpendingのまま保持される |
| `IsDlcOwned()` が false | 初期化した AppID に対象DLCのentitlementがない |

## 未対応機能

- マッチメイキング (`ISteamMatchmaking`)
- P2P ネットワーキング (`ISteamNetworking`)
- マイクロトランザクション (`ISteamMicroTxn`)
- Game Server (`ISteamGameServer`)
- HTML サーフェス (`ISteamHTMLSurface`)
- ビデオ (`ISteamVideo`)
- リモートプレイ (`ISteamRemotePlay`)

これらは ACS のターゲット (= 日本インディー向け 2D / 3D ゲーム) では使用頻度が低いため、必要になった時に追加する。

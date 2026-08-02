<!-- SPDX-License-Identifier: Apache-2.0 -->
# Steamworks SDK 統合 (Phase 26)

ACS は **Steamworks SDK** を `acs_steamworks` モジュールとして統合する。デフォルトは OFF (= `FSteamworksBridgeStub` no-op)、明示的に ON にしたときだけ real backend がビルドされる。

## アーキテクチャ

```
+-------------------------------------+
|  acs::game::ISteamworksBridge       |  ← 純粋仮想 I/F (interface seam)
|  (src/gameframework/SteamworksBridge.h) |
+-------------------------------------+
            ▲                ▲
            │                │
+-----------+--+    +--------+---------------+
|  FSteamworks |    |  FSteamworksBridgeImpl |  ← real backend
|  BridgeStub  |    |  (src/steamworks/      |
|  (no-op)     |    |   SteamworksBridgeImpl.cpp) |
+--------------+    +------------------------+
   default               ACS_BUILD_STEAMWORKS=ON
```

利用側は **常に `ISteamworksBridge*` 経由**で API を呼び、実装は CMake オプションで差し替える。

## API 一覧 (21 メソッド)

### Phase 1 — 必須機能

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

### Phase 2 — 拡張機能

| API | 説明 |
|---|---|
| `SetStat(name, i64)` / `GetStat(name)` | int32 stat (内部 i32 clamp) |
| `SetFloatStat(name, f32)` / `GetFloatStat(name)` | float stat |
| `IsDlcOwned(app_id)` | DLC 所有チェック |
| `SetRichPresence(key, value)` | rich presence |
| `GetFriendCount()` / `GetFriendByIndex(i)` | フレンドリスト |

### Phase 3 — フル機能

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

## ビルド方法

### 方式 A (推奨): ローカル SDK ディレクトリ指定

1. [Steamworks Partner](https://partner.steamgames.com/downloads/) から `steamworks_sdk_<version>.zip` を取得 (要パートナー登録)
2. 展開する (例: `C:/lib/steamworks_sdk_162/`)
3. CMake 設定:
   ```
   cmake -B build -DACS_BUILD_STEAMWORKS=ON \
                  -DACS_STEAMWORKS_SDK_DIR=C:/lib/steamworks_sdk_162
   ```
4. ビルド (`cmake --build build`)
5. 実行ディレクトリに **`steam_appid.txt`** (内容: AppID 数字のみ、例 `480` for Spacewar) を置く
6. 実行 (Steam クライアントが起動している必要がある)

### 方式 B (CI 用): FetchContent (URL)

```
cmake -B build -DACS_BUILD_STEAMWORKS=ON \
               -DACS_STEAMWORKS_SDK_URL=file:///path/to/sdk.zip
```

`ACS_STEAMWORKS_SDK_URL` には自前ミラーまたは S3 URL 等を指定。Valve の公式 SDK は公開ミラーが無いため、URL は user 自身で用意する。

### 方式 C (default): Stub のみ (SDK 不要)

```
cmake -B build   # ACS_BUILD_STEAMWORKS=OFF (default)
```

`FSteamworksBridgeStub` が DI される。実績解除等の呼び出しは `kSubSteamworksNotImplemented` エラーを返すが、ゲーム挙動には影響しない (例外も投げない)。

## ランタイム要件

### `steam_appid.txt`

real SDK 実行時には、exe と同じディレクトリに `steam_appid.txt` を配置する必要がある。内容は AppID の 10 進数字のみ:

```
480
```

- **480 = Spacewar** (Valve 提供のテスト AppID、誰でも使える)
- **本番 AppID**: Steam Direct で取得した game の AppID

開発時は 480 で十分。本番リリース時に置換する。**`steam_appid.txt` は `.gitignore` 推奨** (環境ごとに異なる、CI/CD で生成)。

### Steam クライアント

`SteamAPI_Init()` は **Steam クライアントが起動していないと false を返す**。dev 時は Steam にログイン状態を保つこと。

### `steam_api64.dll`

real backend を使う場合、`steam_api64.dll` (= SDK の `redistributable_bin/win64/steam_api64.dll`) を exe と同じディレクトリにコピーする必要がある。CMake の `install(FILES ...)` 経由で自動コピーする予定 (Phase 26-2)。

## ゲーム側での使い方

```cpp
#include "gameframework/SteamworksBridge.h"
// real backend を使うなら:
#ifdef WITH_ACS_STEAMWORKS
#  include "steamworks/SteamworksBridgeImpl.h"
#endif

class CMyGame : public acs::game::CGame {
    acs::game::ISteamworksBridge* m_Social = nullptr;
#ifdef WITH_ACS_STEAMWORKS
    acs::steamworks::FSteamworksBridgeImpl m_RealSocial;
#endif

    void OnStart() noexcept override {
#ifdef WITH_ACS_STEAMWORKS
        // real backend を優先、失敗時は stub にフォールバック
        if (m_RealSocial.Init().IsOk()) {
            m_Social = &m_RealSocial;
        } else {
            m_Social = &acs::game::FSteamworksBridgeStub::GetStub();
        }
#else
        m_Social = &acs::game::FSteamworksBridgeStub::GetStub();
        (void)m_Social->Init();
#endif
    }

    void OnUpdate(f32 dt) noexcept override {
        m_Social->Tick(dt);  // 必須: callback ポンプ
        // ... game logic
    }

    void OnBossKilled() noexcept {
        (void)m_Social->UnlockAchievement("ACH_BOSS_01");
        (void)m_Social->SetStat("bosses_killed", 1);
    }

    void OnShutdown() noexcept override {
        m_Social->Shutdown();
    }
};
```

## トラブルシューティング

| 症状 | 原因 / 対策 |
|---|---|
| `Init()` が `kSubSteamworksInitFailed` で fail | Steam クライアント未起動 / `steam_appid.txt` 不在 / AppID 不一致 |
| Achievement 解除しても見えない | Spacewar 480 で開発中は実際の Steam 実績が無い (本番 AppID で確認) |
| `steam_api64.dll not found` ランタイムエラー | DLL を exe ディレクトリにコピー (`<SDK>/redistributable_bin/win64/steam_api64.dll`) |
| Leaderboard が 0 を返す | async API なので `Tick()` を数フレーム呼ばないと完了通知が来ない |
| `IsDlcOwned()` が false | 開発時は Spacewar で DLC を持たない (本番 AppID で確認) |

## Phase 1/2/3 範囲外 (将来拡張)

- マッチメイキング (`ISteamMatchmaking`)
- P2P ネットワーキング (`ISteamNetworking`)
- マイクロトランザクション (`ISteamMicroTxn`)
- Game Server (`ISteamGameServer`)
- HTML サーフェス (`ISteamHTMLSurface`)
- ビデオ (`ISteamVideo`)
- リモートプレイ (`ISteamRemotePlay`)

これらは ACS のターゲット (= 日本インディー向け 2D / 3D ゲーム) では使用頻度が低いため、必要になった時に追加する。

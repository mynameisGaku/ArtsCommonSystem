// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar S — SteamworksBridge (Achievements / Leaderboards / PlayerIdentity)
//
// Steam / EOS / PS / Xbox / Switch といったプラットフォーム固有 SDK へ橋渡しする
// 「シーム (seam)」インターフェース。ゲーム側コードは ISteamworksBridge 経由でのみ
// 実績解除 / リーダーボード / ローカルプレイヤー情報を取得し、実 SDK (Steamworks
// SDK 等) との結合はビルド時の選択で差し替える。
//
// 使い方:
//   class Game {
//       acs::game::ISteamworksBridge* _social = nullptr;
//
//       void OnStart() noexcept override {
//           // 出荷ビルドでは GoldenSteamworksBridge を DI、開発ビルドでは Stub。
//           _social = &acs::game::SteamworksBridgeStub::GetStub();
//           (void)_social->Init();
//       }
//       void OnTick(f32 dt) noexcept override {
//           _social->Tick(dt);  // callback pump
//       }
//       void OnBossKilled() noexcept {
//           (void)_social->UnlockAchievement("ACH_BOSS_01");
//       }
//   };
//
// 設計選択 (Pillar S Phase 1):
//   ・**シーム (= 純粋仮想 I/F) として抽象化**: Steamworks SDK は static lib 配布で
//     依存追加が重い (~30MB / Win+Linux 別ライブラリ)。本体は SDK 非依存のままで
//     ビルドできるよう、ヘッダだけは常に提供し、実装は別モジュール (将来の
//     `acs_steamworks` 等) で `ISteamworksBridge` を override する形を取る。
//   ・**cross-platform で同じ I/F**: Steam / EOS / PS / Xbox のいずれを後ろで使っても
//     ゲーム側コードを書き換えない。プラットフォーム固有 ID は `const char*
//     platform_id` の opaque な文字列として渡す (例: SteamID64 を "76561198..." の
//     decimal 文字列で)。
//   ・**所有しない const char*** : 文字列は呼び出し側 / プラットフォーム SDK の
//     ライフタイムに従う。Bridge はコピーしない (STL <string> 不使用方針)。
//     利用側は GetLocalPlayer() の戻り値を「ティック内のみ有効」と扱うこと。
//   ・**TResult<T, FErrorCode> で例外なし**: ACS 全体方針に沿う。Stub は Init() のみ
//     成功を返し、他は ACS_ERR(Generic, kSubNotImplemented, "...") を返す。
//   ・**Tick(dt) は必須**: Steamworks の `SteamAPI_RunCallbacks()` 相当を Bridge 側に
//     畳み込む。ゲーム側は dt を毎フレーム渡すだけで、コールバックポンプの存在を
//     意識しなくて良い。
//   ・**Stub は static singleton で取得**: 依存ゼロのデフォルト実装として
//     `SteamworksBridgeStub::GetStub()` を提供。実 SDK 未統合のビルドでも
//     `_social = &SteamworksBridgeStub::GetStub();` だけでコンパイル可能。
//   ・**実 SDK 実装はここでは作らない**: GoldenSteamworksBridge 等は Steamworks SDK
//     ヘッダ / ライブラリへの依存を伴うため、本ファイルでは I/F + Stub のみ。
//
// 範囲外 (Phase 2+ で):
//   ・Stats / リッチプレゼンス / マルチプレイ招待 / クラウドセーブ / DLC owns / ワークショップ。
//   ・非同期 result の Future / コールバック登録。今は同期 API 前提 (Leaderboard
//     get は SDK 側でキャッシュされた値を即返す形を想定)。
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

// ---- FErrorCode subcode 定義 (ErrCategory::Generic 配下) ------------------
// 本来は ErrCategory に NotImplemented を足したい所だが、Foundation の enum 変更は
// 影響が広いため Phase 1 では Generic + 安定 subcode で表現する。
// 利用側は `err.subcode == kSubSteamworksNotImplemented` でフィルタ可能。
inline constexpr u16 kSubSteamworksNotImplemented = 1001;  // Stub による未実装
inline constexpr u16 kSubSteamworksNotInitialized = 1002;  // Init() 前の API 呼び出し

// ---- PlayerIdentity (cross-platform 共通の最小情報) ------------------------
// Bridge は文字列を所有しない。`platform_id` / `display_name` は実 SDK 側 (または
// Stub 内 static literal) のメモリを参照するだけで、呼び出し側でコピーしない。
// 寿命は「次の Tick() を呼ぶまで」を保証する (実装によってはそれより長い)。
struct PlayerIdentity {
    const char* platform_id   = nullptr;  // "76561198..." 等の SDK 固有 ID 文字列
    const char* display_name  = nullptr;  // ユーザー表示名 (UTF-8)
    u64         session_token = 0;        // SDK 発行のセッショントークン (0 = 無効)
};

// ---- 抽象 I/F -------------------------------------------------------------
// Steamworks / EOS / PS / Xbox の差を吸収する純粋仮想インターフェース。
// 実装は本体外モジュール (or テスト) で Override する。
class ISteamworksBridge {
public:
    ISteamworksBridge() noexcept = default;
    virtual ~ISteamworksBridge() noexcept = default;

    ISteamworksBridge(const ISteamworksBridge&)            = delete;
    ISteamworksBridge& operator=(const ISteamworksBridge&) = delete;
    ISteamworksBridge(ISteamworksBridge&&)                 = delete;
    ISteamworksBridge& operator=(ISteamworksBridge&&)      = delete;

    // SDK 初期化。失敗は SDK 固有のエラーを TResult で返す。多重呼び出し可否は
    // 実装依存だが、Stub は何度呼んでも成功を返す。
    virtual TResult<void> Init() noexcept = 0;

    // SDK 終了処理。Init() 前に呼んでも安全 (no-op)。
    virtual void Shutdown() noexcept = 0;

    // Init() 成功後かつ Shutdown() 前なら true。
    virtual bool IsInitialized() const noexcept = 0;

    // ローカルプレイヤー情報。未初期化時は空 PlayerIdentity を返す (異常終了しない)。
    virtual PlayerIdentity GetLocalPlayer() const noexcept = 0;

    // 実績解除。ach_id はプラットフォームの実績 ID 文字列 ("ACH_BOSS_01" 等)。
    // 既に解除済みの場合の振る舞いは SDK 依存 (大抵は no-op で成功)。
    virtual TResult<void> UnlockAchievement(const char* ach_id) noexcept = 0;

    // リーダーボードへスコアを送信。board_id は実績と同じく文字列キー。
    // 既存値より低いかどうかは SDK 側の「上書きポリシー」設定に依存。
    virtual TResult<void> SetLeaderboardScore(const char* board_id, i64 score) noexcept = 0;

    // 自分の現在スコアを取得。未投稿時は 0 / エラーいずれかを返す (実装依存)。
    virtual TResult<i64> GetLeaderboardScore(const char* board_id) noexcept = 0;

    // コールバックポンプ。Steamworks では SteamAPI_RunCallbacks() 相当。
    // ゲームループから毎フレーム呼ぶこと。dt は実時間秒 (実装によっては使わない)。
    virtual void Tick(f32 dt) noexcept = 0;
};

// ---- Stub 実装 ------------------------------------------------------------
// Steamworks SDK 未統合ビルド / ユニットテスト用の no-op 実装。
//   ・Init() のみ常に成功。
//   ・GetLocalPlayer() は固定ダミー ("stub_player" / "Player") を返す。
//   ・Achievement / Leaderboard 系は ACS_ERR(Generic, kSubSteamworksNotImplemented)。
//   ・Shutdown() / Tick() は副作用なし。
class SteamworksBridgeStub final : public ISteamworksBridge {
public:
    SteamworksBridgeStub() noexcept = default;
    ~SteamworksBridgeStub() noexcept override = default;

    TResult<void>    Init() noexcept override;
    void            Shutdown() noexcept override;
    bool            IsInitialized() const noexcept override { return _initialized; }
    PlayerIdentity  GetLocalPlayer() const noexcept override;
    TResult<void>    UnlockAchievement(const char* ach_id) noexcept override;
    TResult<void>    SetLeaderboardScore(const char* board_id, i64 score) noexcept override;
    TResult<i64>     GetLeaderboardScore(const char* board_id) noexcept override;
    void            Tick(f32 dt) noexcept override;

    // 全コードで共有できる static singleton。実 SDK 実装が DI される前のデフォルト。
    static SteamworksBridgeStub& GetStub() noexcept;

private:
    bool _initialized = false;
};

} // namespace acs::game

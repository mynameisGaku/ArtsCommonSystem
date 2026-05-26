// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar T — VoiceChat (Steam Voice / EOS Voice / Vivox / Discord / Opus seam)
//
// 役割:
//   パーティ / チーム / 全体チャンネルに対するボイスチャットの「シーム」インター
//   フェース。実プロバイダは Steam Voice / EOS Voice / Unity Vivox / Discord
//   Game SDK / Opus self-host のいずれでも良く、ゲーム側コードは `IVoiceChatBackend`
//   経由でのみ参加者管理 / ミュート / 音量を扱う。SDK 統合はビルド時の選択で
//   差し替える。
//
// 使い方 (典型例):
//   class Game {
//       acs::game::IVoiceChatBackend* _voice = nullptr;
//
//       void OnStart() noexcept override {
//           // 出荷ビルドでは Vivox/Discord/Steam Voice 実装を DI、開発ビルドは Stub。
//           _voice = &acs::game::GetVoiceStub();
//           (void)_voice->Init(acs::game::EVoiceProvider::None);
//       }
//       void OnPartyJoined() noexcept {
//           (void)_voice->JoinChannel(acs::game::EVoiceChannel::Party, "party-1234");
//       }
//       void OnTick(f32 dt) noexcept override {
//           _voice->Tick(dt);  // audio level 取得 / 発言検出 pump
//       }
//   };
//
// 設計選択 (Pillar T Phase 2):
//   ・**シーム (= 純粋仮想 I/F) として抽象化**: Vivox / Discord / Steam Voice 各 SDK
//     はライセンスもバイナリサイズも大きく、本体に直接リンクできない。ヘッダだけは
//     常に提供し、実装は別モジュール (将来の `acs_voice_vivox` / `acs_voice_discord`
//     等) で `IVoiceChatBackend` を override する形を取る。
//   ・**プロバイダは enum で表現**: 同時に複数バックエンドが共存する稀ケース
//     (Steam Voice + Discord overlay) は本フェーズでは扱わず、`Init(EVoiceProvider)`
//     で 1 個を選んで初期化する。`ActiveProvider()` で取得可能。
//   ・**チャンネルは 4 種類固定**: Party / Team / Global / Custom。最初の 3 つは
//     ゲームロジック共通の意味論を持たせ、Custom は SDK 固有の追加チャンネル用
//     (raid voice / region voice / dev チャット等)。
//   ・**所有しない const char***: 文字列 (user_id / display_name / channel_id) は
//     呼び出し側 / SDK のライフタイムに従い、Bridge はコピーしない (STL <string>
//     不使用方針)。`GetParticipant` の戻り値は「次の Tick まで有効」と扱うこと。
//   ・**TResult<T, FErrorCode> で例外なし**: ACS 全体方針に沿う。Stub は全 API で
//     `ACS_ERR(Generic, kSubVoiceNotImplemented, ...)` を返す。
//   ・**コピー / ムーブ禁止**: backend は通常 1 個の長寿命オブジェクトで運用。
//     誤コピーで SDK ハンドルが二重解放されると詰むため最初から非コピー・非ムーブ。
//   ・**Tick(dt) は必須**: Vivox の `Update3DPosition()` 相当 / Discord の event pump
//     を Backend 側に畳み込む。ゲーム側は dt を毎フレーム渡すだけで、コールバック
//     ポンプの存在を意識しなくて良い。
//   ・**Stub は static singleton**: 依存ゼロのデフォルト実装として `GetVoiceStub()`
//     を提供。実 SDK 未統合のビルドでも `_voice = &GetVoiceStub();` だけでコンパイル可能。
//
// 倫理 / 安全方針:
//   ・**moderation は別モジュール**: 文字起こしによる NG ワード判定 / 通報導線は
//     `SocialModeration` (別 Pillar) が担当。本 system は技術的な mute/volume 管理のみ。
//   ・**under-18 のデフォルト**: 未成年アカウントが既知の場合、呼び出し側で
//     `SetLocalMute(true)` をかぶせる想定。本 system はフラグを持たず強制機構なし
//     (プラットフォームごとに年齢推定 API が異なるため一律ルール化が危険)。
//
// 範囲外 (将来フェーズで):
//   ・push-to-talk / VAD threshold 設定 / 3D ポジショナルボイス座標更新 API。
//   ・録音 / 文字起こし (Pillar U AI 経由 STT)。
//   ・音声エフェクト (ボイスチェンジャー / ノイズリダクション設定)。
//   ・テキストチャット (別モジュール)。
//   ・各 SDK 固有の advanced API (Vivox の transmit policy 等)。
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

// ---- FErrorCode subcode 定義 (ErrCategory::Generic 配下) ------------------
// SteamworksBridge と同様、Foundation の ErrCategory enum を増やさず Generic +
// 安定 subcode で表現する。呼び出し側は `err.subcode == kSubVoiceNotImplemented`
// でフィルタ可能。
inline constexpr u16 kSubVoiceNotImplemented = 99;    // Stub による未実装 (仕様準拠)
inline constexpr u16 kSubVoiceNotInitialized = 1102;  // Init() 前の API 呼び出し

// ---- プロバイダ種別 -------------------------------------------------------
// `Init(EVoiceProvider)` で 1 個選んで初期化する。`OpusSelf` は SDK 非依存の
// 自前 Opus + UDP 実装を意図 (専用サーバ運用ケース)。
enum class EVoiceProvider : u8 {
    None        = 0,  // 未選択 / Stub
    SteamVoice  = 1,  // Steamworks Voice API
    EosVoice    = 2,  // Epic Online Services Voice
    Vivox       = 3,  // Unity Vivox (cross-platform 汎用)
    Discord     = 4,  // Discord Game SDK (Lobby Voice)
    OpusSelf    = 5,  // 自前 Opus codec + 専用サーバ
};

// ---- チャンネル種別 -------------------------------------------------------
// `JoinChannel(EVoiceChannel ch, const char* channel_id)` で参加するチャンネルの
// 意味論を区別する。同時に複数チャンネルへ join 可能 (Party + Global 同居 等)。
enum class EVoiceChannel : u8 {
    Party    = 0,  // パーティ内チャット (PartySystem と紐づく)
    Team     = 1,  // チーム / 隊伍内チャット (試合中の同チーム)
    Global   = 2,  // 全体 / ロビーチャット (試合外の広域)
    Custom   = 3,  // タイトル固有の追加チャンネル (raid / region 等)
};

// ---- 参加者情報 -----------------------------------------------------------
// `GetParticipant` の戻り値。`user_id` / `display_name` は const char* 非所有
// (SDK 側のメモリを参照するだけで、呼び出し側でコピーしない)。寿命は「次の
// Tick() を呼ぶまで」を保証する。
//
//   ・is_speaking      : 現在発言中か (VAD or push-to-talk が active)
//   ・is_muted_local   : ローカル側ミュート (自分から見て聞こえないか)
//   ・audio_level      : 現在の音声レベル (0.0〜1.0、UI のメーター表示用)
struct VoiceParticipant {
    const char* user_id        = nullptr;  // SDK 固有 ID 文字列 (PartySystem の player_id と同形式想定)
    const char* display_name   = nullptr;  // 表示名 (UTF-8、寿命は SDK 側保証)
    bool        is_speaking    = false;    // 現在発言中フラグ
    bool        is_muted_local = false;    // ローカル側ミュート
    f32         audio_level    = 0.0f;     // 0.0〜1.0 の音声レベル
};

// ---- 抽象 I/F -------------------------------------------------------------
// Steam Voice / EOS Voice / Vivox / Discord / OpusSelf の差を吸収する純粋仮想
// インターフェース。実装は本体外モジュール (or テスト) で Override する。
class IVoiceChatBackend {
public:
    IVoiceChatBackend() noexcept = default;
    virtual ~IVoiceChatBackend() noexcept = default;

    IVoiceChatBackend(const IVoiceChatBackend&)            = delete;
    IVoiceChatBackend& operator=(const IVoiceChatBackend&) = delete;
    IVoiceChatBackend(IVoiceChatBackend&&)                 = delete;
    IVoiceChatBackend& operator=(IVoiceChatBackend&&)      = delete;

    // SDK 初期化。`p` で使用するプロバイダを選択。Stub は受け取った p を記録
    // するのみで実 SDK 呼び出しは行わない。多重 Init は実装依存。
    virtual TResult<void> Init(EVoiceProvider p) noexcept = 0;

    // SDK 終了処理。Init() 前に呼んでも安全 (no-op)。
    virtual void Shutdown() noexcept = 0;

    // 利用可能か (Init 済 + SDK 接続可)。Stub は常に false。
    virtual bool IsAvailable() const noexcept = 0;

    // 現在アクティブなプロバイダ。Init 前は EVoiceProvider::None。
    virtual EVoiceProvider ActiveProvider() const noexcept = 0;

    // 指定チャンネルに参加。`channel_id` は SDK 固有のチャンネル ID 文字列
    // (PartySystem の party_id / マッチ ID 等を渡す想定、非所有)。
    virtual TResult<void> JoinChannel(EVoiceChannel ch, const char* channel_id) noexcept = 0;

    // 指定チャンネルから離脱。join 前に呼んでも安全な実装が望ましい。
    virtual TResult<void> LeaveChannel(EVoiceChannel ch) noexcept = 0;

    // 自分のマイクをローカル側でミュート / 解除する。送信を止めるかどうかは
    // 実装依存 (Vivox はミュート時も local capture を続けるが送信を停止)。
    virtual TResult<void> SetLocalMute(bool muted) noexcept = 0;

    // 指定参加者をローカル側でミュート (自分の耳でのみ無音化)。`user_id` は
    // `GetParticipant` で得た値 (非所有、SDK 側で同定可能な ID 文字列)。
    virtual TResult<void> SetParticipantMute(const char* user_id, bool muted) noexcept = 0;

    // 指定参加者のローカル音量を 0.0〜1.0 (もしくは SDK が許す範囲) で設定。
    // 範囲外を渡した場合の振る舞いは実装依存 (clamp 推奨)。
    virtual TResult<void> SetParticipantVolume(const char* user_id, f32 volume) noexcept = 0;

    // 指定チャンネルの参加者数 (自分含む)。未 join 時は 0。
    virtual u32 ParticipantCount(EVoiceChannel ch) noexcept = 0;

    // 指定チャンネルの index 番目の参加者を取得。範囲外 index はエラー。
    // 戻り値の `user_id` / `display_name` 寿命は次 Tick() まで保証。
    virtual TResult<VoiceParticipant> GetParticipant(EVoiceChannel ch, u32 index) noexcept = 0;

    // コールバック / 状態更新ポンプ。Vivox の Update3DPosition / Discord の
    // event pump / VAD threshold 監視を Backend 側に畳み込む。ゲームループから
    // 毎フレーム呼ぶこと。dt は実時間秒 (実装によっては使わない)。
    virtual void Tick(f32 dt) noexcept = 0;
};

// ---- Stub 実装 ------------------------------------------------------------
// SDK 未統合ビルド / ユニットテスト用の no-op 実装。
//   ・Init() は受け取った provider を記録するが TResult は ACS_ERR を返す
//     (SteamworksBridge と違い、こちらは「未実装」を強調するため)。
//   ・IsAvailable() は常に false。
//   ・全 API が ACS_ERR(Generic, kSubVoiceNotImplemented, ...) を返す。
//   ・Shutdown() / Tick() は副作用なし。
class VoiceChatBackendStub final : public IVoiceChatBackend {
public:
    VoiceChatBackendStub() noexcept = default;
    ~VoiceChatBackendStub() noexcept override = default;

    TResult<void>             Init(EVoiceProvider p) noexcept override;
    void                     Shutdown() noexcept override;
    bool                     IsAvailable() const noexcept override { return false; }
    EVoiceProvider            ActiveProvider() const noexcept override { return _provider; }
    TResult<void>             JoinChannel(EVoiceChannel ch, const char* channel_id) noexcept override;
    TResult<void>             LeaveChannel(EVoiceChannel ch) noexcept override;
    TResult<void>             SetLocalMute(bool muted) noexcept override;
    TResult<void>             SetParticipantMute(const char* user_id, bool muted) noexcept override;
    TResult<void>             SetParticipantVolume(const char* user_id, f32 volume) noexcept override;
    u32                      ParticipantCount(EVoiceChannel ch) noexcept override;
    TResult<VoiceParticipant> GetParticipant(EVoiceChannel ch, u32 index) noexcept override;
    void                     Tick(f32 dt) noexcept override;

private:
    EVoiceProvider _provider = EVoiceProvider::None;
    bool          _initialized = false;
};

// 全コードで共有できる static singleton。実 SDK 実装が DI される前のデフォルト。
// SteamworksBridge::GetStub() と同じ Meyer's singleton パターン。
IVoiceChatBackend& GetVoiceStub() noexcept;

} // namespace acs::game

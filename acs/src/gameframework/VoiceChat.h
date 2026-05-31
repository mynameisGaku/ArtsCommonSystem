// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar T — FVoiceChat (Steam Voice / EOS Voice / Vivox / Discord / Opus seam)
//
// 役割:
//   パーティ / チーム / 全体チャンネルに対するボイスチャットの「シーム」インター
//   フェース。実プロバイダは Steam Voice / EOS Voice / Unity Vivox / Discord
//   FGame SDK / Opus self-host のいずれでも良く、ゲーム側コードは `IVoiceChatBackend`
//   経由でのみ参加者管理 / ミュート / 音量を扱う。SDK 統合はビルド時の選択で
//   差し替える。
//
// 使い方 (典型例):
//   class FGame {
//       acs::game::IVoiceChatBackend* m_Voice = nullptr;
//
//       void OnStart() noexcept override {
//           // 出荷ビルドでは Vivox/Discord/Steam Voice 実装を DI、開発ビルドは Stub。
//           m_Voice = &acs::game::GetVoiceStub();
//           (void)m_Voice->Init(acs::game::EVoiceProvider::None);
//       }
//       void OnPartyJoined() noexcept {
//           (void)m_Voice->JoinChannel(acs::game::EVoiceChannel::Party, "party-1234");
//       }
//       void OnTick(f32 dt) noexcept override {
//           m_Voice->Tick(dt);  // audio level 取得 / 発言検出 pump
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
//     を提供。実 SDK 未統合のビルドでも `m_Voice = &GetVoiceStub();` だけでコンパイル可能。
//
// 倫理 / 安全方針:
//   ・**moderation は別モジュール**: 文字起こしによる NG ワード判定 / 通報導線は
//     `FSocialModeration` (別 Pillar) が担当。本 system は技術的な mute/volume 管理のみ。
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
#include "container/Array.h"
#include "container/String.h"

namespace acs::game {

// ---- FErrorCode subcode 定義 (ErrCategory::Generic 配下) ------------------
// FSteamworksBridge と同様、Foundation の ErrCategory enum を増やさず Generic +
// 安定 subcode で表現する。呼び出し側は `err.subcode == kSubVoiceNotImplemented`
// でフィルタ可能。
inline constexpr u16 kSubVoiceNotImplemented = 99;    // Stub による未実装 (仕様準拠)
inline constexpr u16 kSubVoiceNotInitialized = 1102;  // Init() 前の API 呼び出し
inline constexpr u16 kSubVoiceNotJoined      = 1103;  // 未 join のチャンネルへの操作
inline constexpr u16 kSubVoiceUnknownUser    = 1104;  // 未知の user_id を指定
inline constexpr u16 kSubVoiceBadArgument    = 1105;  // 不正引数 (null / 範囲外 / 過大フレーム)
inline constexpr u16 kSubVoiceFrameCorrupt   = 1106;  // 受信フレームのヘッダ / magic 破損
inline constexpr u16 kSubVoiceCapacity       = 1107;  // 参加者 / フレームキュー容量超過

// ---- プロバイダ種別 -------------------------------------------------------
// `Init(EVoiceProvider)` で 1 個選んで初期化する。`OpusSelf` は SDK 非依存の
// 自前 Opus + UDP 実装を意図 (専用サーバ運用ケース)。
enum class EVoiceProvider : u8 {
    None        = 0,  // 未選択 / Stub
    SteamVoice  = 1,  // Steamworks Voice API
    EosVoice    = 2,  // Epic Online Services Voice
    Vivox       = 3,  // Unity Vivox (cross-platform 汎用)
    Discord     = 4,  // Discord FGame SDK (Lobby Voice)
    OpusSelf    = 5,  // 自前 Opus codec + 専用サーバ
};

// ---- チャンネル種別 -------------------------------------------------------
// `JoinChannel(EVoiceChannel ch, const char* channel_id)` で参加するチャンネルの
// 意味論を区別する。同時に複数チャンネルへ join 可能 (Party + Global 同居 等)。
enum class EVoiceChannel : u8 {
    Party    = 0,  // パーティ内チャット (FPartySystem と紐づく)
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
    const char* user_id        = nullptr;  // SDK 固有 ID 文字列 (FPartySystem の player_id と同形式想定)
    const char* display_name   = nullptr;  // 表示名 (UTF-8、寿命は SDK 側保証)
    bool        is_speaking    = false;    // 現在発言中フラグ
    bool        is_muted_local = false;    // ローカル側ミュート
    f32         audio_level    = 0.0f;     // 0.0〜1.0 の音声レベル
};

// ---- 音声フレーム関連 -----------------------------------------------------
// ループバック (= 自前ローカル) backend が実際の PCM を運ぶための型。実 SDK
// backend (Vivox/Discord/Steam) はマイク捕捉〜再生まで SDK 内部で完結するため
// これらの push/pump API を **既定実装 (no-op / NotImplemented)** として持つ。
// 自前 backend (`FVoiceChatLoopbackBackend`) のみがこれらを override し、
//   push PCM → encode(framing) → 参加者ごとの in-process キュー
//     → decode → per-participant gain → N-way mix(sum+clamp) → pump
// の往復を本実装する。

// 1 フレームの想定サンプル数。実 codec (Opus) なら 20ms@48kHz=960 等だが、本
// ループバックは sample-rate 非依存で「呼び出し側が渡した int16 配列」をそのまま
// 1 フレームとして扱う (識別 PCM framing)。上限のみ規定し、超過は分割を呼び出し
// 側に委ねる (over-large frame は kSubVoiceBadArgument)。
inline constexpr u32 kVoiceMaxFrameSamples = 4096;   // 1 push あたりの最大サンプル数
inline constexpr u32 kVoiceFrameMagic      = 0x41435631u; // 'ACV1' = ACS Voice frame v1

// 線形 PCM フレームの固定ヘッダ (16 byte、LE)。encode 時に前置し、decode 時に
// magic / サンプル数を検証してから payload (int16 PCM) を復元する。識別変換だが
// seq + magic + サンプル数を伴う real な framing であり、実 wire codec を後で
// 差し込む際の境界もここで確定する。
struct VoiceFrameHeader {
    u32 magic        = kVoiceFrameMagic;  // 破損検知用マジック
    u32 sequence     = 0;                 // 送信元ごとの単調増加シーケンス番号
    u32 sample_count = 0;                 // payload の int16 サンプル数
    u32 reserved     = 0;                 // 将来の codec id / channel 用 (現状 0)
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
    // (FPartySystem の party_id / マッチ ID 等を渡す想定、非所有)。
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

    // ---- 音声フレーム I/O (自前ループバック backend 用、既定は no-op) -------
    // 実 SDK backend はマイク捕捉/再生を SDK 内部で行うため、これらは override
    // せず既定実装 (NotImplemented / 0 / 無音) のままで良い。
    // `FVoiceChatLoopbackBackend` のみが本実装し、PCM の往復を成立させる。

    // ローカルマイク相当の int16 PCM フレームを 1 つ push する (push 型 capture)。
    // `pcm` は `sample_count` 個の int16 サンプル (非所有、本呼び出し中のみ参照)。
    // ローカルミュート中は送信されない (capture は続くが route しない)。
    // 既定: NotImplemented を返す。
    virtual TResult<void> PushLocalFrame(EVoiceChannel ch, const i16* pcm, u32 sample_count) noexcept {
        (void)ch; (void)pcm; (void)sample_count;
        return ACS_ERR(Generic, kSubVoiceNotImplemented,
                       "IVoiceChatBackend::PushLocalFrame: backend does not support PCM push");
    }

    // ローカル float [-1,1] PCM を push する利便メソッド。内部で int16 に量子化。
    // 既定: NotImplemented を返す。
    virtual TResult<void> PushLocalFrameF32(EVoiceChannel ch, const f32* pcm, u32 sample_count) noexcept {
        (void)ch; (void)pcm; (void)sample_count;
        return ACS_ERR(Generic, kSubVoiceNotImplemented,
                       "IVoiceChatBackend::PushLocalFrameF32: backend does not support PCM push");
    }

    // 指定チャンネルで「自分以外の全参加者から受信したフレーム」を per-participant
    // gain / mute を適用して N-way mix (sum + clamp i16) し、`out` に書き込む。
    // 返り値は実際に書き込んだサンプル数 (`out_capacity` で頭打ち、超過分は捨てる)。
    // mix 後に消費されたフレームは各参加者キューから取り除かれる。
    // 既定: 0 (無音)。
    virtual u32 PumpMixedOutput(EVoiceChannel ch, i16* out, u32 out_capacity) noexcept {
        (void)ch; (void)out; (void)out_capacity;
        return 0;
    }

    // 直近に push したローカルフレームの音声レベル (RMS, 0.0〜1.0)。VAD/メーター用。
    // 既定: 0.0f。
    virtual f32 LocalAudioLevel() const noexcept { return 0.0f; }
};

// ---- Stub 実装 ------------------------------------------------------------
// SDK 未統合ビルド / ユニットテスト用の no-op 実装。
//   ・Init() は受け取った provider を記録するが TResult は ACS_ERR を返す
//     (FSteamworksBridge と違い、こちらは「未実装」を強調するため)。
//   ・IsAvailable() は常に false。
//   ・全 API が ACS_ERR(Generic, kSubVoiceNotImplemented, ...) を返す。
//   ・Shutdown() / Tick() は副作用なし。
class FVoiceChatBackendStub final : public IVoiceChatBackend {
public:
    FVoiceChatBackendStub() noexcept = default;
    ~FVoiceChatBackendStub() noexcept override = default;

    TResult<void>             Init(EVoiceProvider p) noexcept override;
    void                     Shutdown() noexcept override;
    bool                     IsAvailable() const noexcept override { return false; }
    EVoiceProvider            ActiveProvider() const noexcept override { return m_Provider; }
    TResult<void>             JoinChannel(EVoiceChannel ch, const char* channel_id) noexcept override;
    TResult<void>             LeaveChannel(EVoiceChannel ch) noexcept override;
    TResult<void>             SetLocalMute(bool muted) noexcept override;
    TResult<void>             SetParticipantMute(const char* user_id, bool muted) noexcept override;
    TResult<void>             SetParticipantVolume(const char* user_id, f32 volume) noexcept override;
    u32                      ParticipantCount(EVoiceChannel ch) noexcept override;
    TResult<VoiceParticipant> GetParticipant(EVoiceChannel ch, u32 index) noexcept override;
    void                     Tick(f32 dt) noexcept override;

private:
    EVoiceProvider m_Provider = EVoiceProvider::None;
    bool          m_Initialized = false;
};

// 全コードで共有できる static singleton。実 SDK 実装が DI される前のデフォルト。
// FSteamworksBridge::GetStub() と同じ Meyer's singleton パターン。
IVoiceChatBackend& GetVoiceStub() noexcept;

// ---- ループバック (自前ローカル) 実装 -------------------------------------
// SDK 非依存で **実際に音声フレームを往復させる** backend。Vivox 等の外部 SDK を
// リンクできない / したくないビルドでも、同一プロセス内で
//   PushLocalFrame(A) → encode(framing) → A 以外の全参加者の受信キュー
//     → decode → per-participant gain/mute → N-way mix(sum+clamp) → PumpMixedOutput(B)
// を成立させる。ネットワークは使わず純粋に in-process なので決定的でユニット
// テスト可能。実 wire codec / トランスポートを後で差し込む際の境界も本クラスの
// encode/decode と参加者キューが担う。
//
// チャンネルモデル:
//   ・本ループバックは「1 プロセス = 1 ローカルセッション」を表現する。各
//     チャンネル (Party/Team/Global/Custom) は独立した参加者テーブル + per-user
//     受信キューを持つ。
//   ・`PushLocalFrame(ch, ...)` で push したフレームは、その ch に join 済みの
//     **自分以外の全参加者** の受信キューに enqueue される (= 自分の声が他者へ
//     届く loopback)。誰が "自分" かは慣習上 index 0 の participant (= ローカル
//     ユーザ)。AddParticipant で最初に追加した参加者をローカルとして扱う。
//   ・`PumpMixedOutput(ch, ...)` は「ある参加者 (既定: ローカル以外を含む全員) の
//     受信キューに溜まったフレーム」を gain/mute 適用して mix する。テストでは
//     SetPumpTarget で「誰の耳としてミックスするか」を選べる。
class FVoiceChatLoopbackBackend final : public IVoiceChatBackend {
public:
    FVoiceChatLoopbackBackend() noexcept = default;
    ~FVoiceChatLoopbackBackend() noexcept override = default;

    // ---- IVoiceChatBackend (制御系) --------------------------------------
    TResult<void>             Init(EVoiceProvider p) noexcept override;
    void                     Shutdown() noexcept override;
    bool                     IsAvailable() const noexcept override { return m_Initialized; }
    EVoiceProvider            ActiveProvider() const noexcept override { return m_Provider; }
    TResult<void>             JoinChannel(EVoiceChannel ch, const char* channel_id) noexcept override;
    TResult<void>             LeaveChannel(EVoiceChannel ch) noexcept override;
    TResult<void>             SetLocalMute(bool muted) noexcept override;
    TResult<void>             SetParticipantMute(const char* user_id, bool muted) noexcept override;
    TResult<void>             SetParticipantVolume(const char* user_id, f32 volume) noexcept override;
    u32                      ParticipantCount(EVoiceChannel ch) noexcept override;
    TResult<VoiceParticipant> GetParticipant(EVoiceChannel ch, u32 index) noexcept override;
    void                     Tick(f32 dt) noexcept override;

    // ---- IVoiceChatBackend (音声フレーム系、本実装) ----------------------
    TResult<void> PushLocalFrame(EVoiceChannel ch, const i16* pcm, u32 sample_count) noexcept override;
    TResult<void> PushLocalFrameF32(EVoiceChannel ch, const f32* pcm, u32 sample_count) noexcept override;
    u32           PumpMixedOutput(EVoiceChannel ch, i16* out, u32 out_capacity) noexcept override;
    f32           LocalAudioLevel() const noexcept override { return m_LastLocalRms; }

    // ---- ループバック専用 API (テスト / ローカルセッション構築用) --------
    // 参加者を ch に追加する。最初に追加した参加者がローカルユーザ (= 自分。
    // PushLocalFrame の送信元、PumpMixedOutput では自分のフレームは聞かない)。
    // 同一 user_id の二重追加は kSubVoiceUnknownUser ではなく成功 (冪等) 扱い。
    TResult<void> AddParticipant(EVoiceChannel ch, const char* user_id, const char* display_name) noexcept;

    // 参加者を ch から取り除く (受信キューも破棄)。未知 user は kSubVoiceUnknownUser。
    TResult<void> RemoveParticipant(EVoiceChannel ch, const char* user_id) noexcept;

    // PumpMixedOutput が「誰の耳」としてミックスするかを選ぶ。既定はローカル
    // ユーザ (index 0)。`user_id` の受信キューを mix 対象にする。
    TResult<void> SetPumpTarget(EVoiceChannel ch, const char* user_id) noexcept;

    // 直近に push したローカルフレームのピーク絶対値 (0.0〜1.0)。
    f32 LocalAudioPeak() const noexcept { return m_LastLocalPeak; }

    // 指定参加者の現在の受信キューに溜まっているフレーム数 (テスト検証用)。
    u32 PendingFrameCount(EVoiceChannel ch, const char* user_id) noexcept;

    // ---- codec (public static、ユニットテストから直接叩ける) -------------
    // int16 PCM → framed バイト列。out は header(16B) + sample_count*2 byte 必要。
    // 戻り値は書き込んだ総バイト数 (0 = 引数不正)。
    static u32 EncodeFrame(const i16* pcm, u32 sample_count, u32 sequence,
                           u8* out, u32 out_capacity) noexcept;
    // framed バイト列 → int16 PCM。header を検証し payload を out へ復元する。
    // 戻り値の成功時 value は復元したサンプル数。magic 破損等は kSubVoiceFrameCorrupt。
    static TResult<u32> DecodeFrame(const u8* in, u32 in_size,
                                    i16* out, u32 out_capacity) noexcept;

private:
    // 参加者ごとの状態。受信キューは encode 済みバイト列を時系列に連結した frame
    // 群を保持する (各 frame = VoiceFrameHeader + payload)。
    struct FLoopParticipant {
        FString        user_id;                  // 所有コピー (非所有 const char* を受けてコピー)
        FString        display_name;             // 所有コピー
        f32            volume      = 1.0f;        // ローカル音量 (mix 時に適用)
        bool           muted_local = false;       // 自分から見たミュート
        u32            next_seq    = 0;           // この送信元が次に使う sequence
        TArray<u8>     rx_frames;                 // 受信フレーム連結バッファ (encode 済み)
    };

    // 1 チャンネル分の状態。
    struct FChannel {
        bool                      joined       = false;
        FString                   channel_id;
        u32                       pump_target  = 0;     // PumpMixedOutput の対象 index (既定 0=ローカル)
        TArray<FLoopParticipant>  participants;
    };

    // ch に対応する FChannel を返す (常に 4 種固定で確保済み)。
    FChannel&       Chan(EVoiceChannel ch) noexcept { return m_Channels[static_cast<u32>(ch)]; }
    const FChannel& Chan(EVoiceChannel ch) const noexcept { return m_Channels[static_cast<u32>(ch)]; }

    // ch 内で user_id を線形検索 (見つからなければ size を返す)。
    u32 FindParticipant(const FChannel& c, const char* user_id) const noexcept;

    // 1 フレームを participant の rx_frames に enqueue する。
    void EnqueueFrame(FLoopParticipant& p, const i16* pcm, u32 sample_count) noexcept;

    EVoiceProvider m_Provider     = EVoiceProvider::None;
    bool          m_Initialized  = false;
    bool          m_LocalMuted   = false;        // SetLocalMute (送信抑止)
    f32           m_LastLocalRms = 0.0f;         // 直近 push の RMS
    f32           m_LastLocalPeak= 0.0f;         // 直近 push のピーク
    FChannel      m_Channels[4]  = {};           // Party/Team/Global/Custom
};

// ループバック backend の static singleton アクセサ。DI 不要で
// `m_Voice = &GetVoiceLoopback();` だけで本物の音声往復が動く。
FVoiceChatLoopbackBackend& GetVoiceLoopback() noexcept;

} // namespace acs::game

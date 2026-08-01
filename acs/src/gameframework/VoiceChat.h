// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar T — VoiceChat (Steam Voice / EOS Voice / Vivox / Discord / Opus seam)
//
// 役割:
//   パーティ / チーム / 全体チャンネルに対するボイスチャットの「シーム」インター
//   フェース。実プロバイダは Steam Voice / EOS Voice / Unity Vivox / Discord
//   CGame SDK / Opus self-host のいずれでも良く、ゲーム側コードは `IVoiceChatBackend`
//   経由でのみ参加者管理 / ミュート / 音量を扱う。SDK 統合はビルド時の選択で
//   差し替える。
//
// 使い方 (典型例):
//   class CGame {
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
// 設計選択 (Pillar T):
//   ・**シーム (= 純粋仮想 I/F) として抽象化**: Vivox / Discord / Steam Voice 各 SDK
//     はライセンスもバイナリサイズも大きく、本体に直接リンクできない。ヘッダだけは
//     常に提供し、実装は別モジュール (将来の `acs_voice_vivox` / `acs_voice_discord`
//     等) で `IVoiceChatBackend` を override する形を取る。
//   ・**プロバイダは enum で表現**: 同時に複数バックエンドが共存する稀ケース
//     (Steam Voice + Discord overlay) は扱わず、`Init(EVoiceProvider)`
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
//     `CSocialModeration` (別 Pillar) が担当。本 system は技術的な mute/volume 管理のみ。
//   ・**under-18 のデフォルト**: 未成年アカウントが既知の場合、呼び出し側で
//     `SetLocalMute(true)` をかぶせる想定。本 system はフラグを持たず強制機構なし
//     (プラットフォームごとに年齢推定 API が異なるため一律ルール化が危険)。
//
// 範囲外:
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

/**
 * Stub による未実装 (仕様準拠) を表す FErrorCode subcode。
 *
 * @details
 * ISteamworksBridge と同様、Foundation の EErrCategory enum を増やさず Generic +
 * 安定 subcode で表現する。呼び出し側は `err.subcode == kSubVoiceNotImplemented`
 * でフィルタ可能。
 */
inline constexpr u16 kSubVoiceNotImplemented = 99;

/** Init() 前に API を呼び出したことを表す subcode。 */
inline constexpr u16 kSubVoiceNotInitialized = 1102;

/** 未 join のチャンネルへの操作を表す subcode。 */
inline constexpr u16 kSubVoiceNotJoined      = 1103;

/** 未知の user_id を指定したことを表す subcode。 */
inline constexpr u16 kSubVoiceUnknownUser    = 1104;

/** 不正引数 (null / 範囲外 / 過大フレーム) を表す subcode。 */
inline constexpr u16 kSubVoiceBadArgument    = 1105;

/** 受信フレームのヘッダ / magic 破損を表す subcode。 */
inline constexpr u16 kSubVoiceFrameCorrupt   = 1106;

/** 参加者 / フレームキュー容量超過を表す subcode。 */
inline constexpr u16 kSubVoiceCapacity       = 1107;

/**
 * ボイスチャットのプロバイダ種別。
 *
 * @details
 * `Init(EVoiceProvider)` で 1 個選んで初期化する。`OpusSelf` は SDK 非依存の
 * 自前 Opus + UDP 実装を意図 (専用サーバ運用ケース)。
 */
enum class EVoiceProvider : u8 {
    /** 未選択 / Stub。 */
    None        = 0,

    /** Steamworks Voice API。 */
    SteamVoice  = 1,

    /** Epic Online Services Voice。 */
    EosVoice    = 2,

    /** Unity Vivox (cross-platform 汎用)。 */
    Vivox       = 3,

    /** Discord CGame SDK (Lobby Voice)。 */
    Discord     = 4,

    /** 自前 Opus codec + 専用サーバ。 */
    OpusSelf    = 5,
};

/**
 * ボイスチャンネルの種別。
 *
 * @details
 * `JoinChannel(EVoiceChannel ch, const char* channel_id)` で参加するチャンネルの
 * 意味論を区別する。同時に複数チャンネルへ join 可能 (Party + Global 同居 等)。
 */
enum class EVoiceChannel : u8 {
    /** パーティ内チャット (CPartySystem と紐づく)。 */
    Party    = 0,

    /** チーム / 隊伍内チャット (試合中の同チーム)。 */
    Team     = 1,

    /** 全体 / ロビーチャット (試合外の広域)。 */
    Global   = 2,

    /** タイトル固有の追加チャンネル (raid / region 等)。 */
    Custom   = 3,
};

/**
 * `GetParticipant` が返す参加者情報。
 *
 * @details
 * `user_id` / `display_name` は const char* 非所有 (SDK 側のメモリを参照するだけで、
 * 呼び出し側でコピーしない)。寿命は「次の Tick() を呼ぶまで」を保証する。
 */
struct FVoiceParticipant {
    /** SDK 固有 ID 文字列 (CPartySystem の player_id と同形式想定、非所有)。 */
    const char* user_id        = nullptr;

    /** 表示名 (UTF-8、寿命は SDK 側保証、非所有)。 */
    const char* display_name   = nullptr;

    /** 現在発言中か (VAD or push-to-talk が active)。 */
    bool        is_speaking    = false;

    /** ローカル側ミュート (自分から見て聞こえないか)。 */
    bool        is_muted_local = false;

    /** 現在の音声レベル (0.0〜1.0、UI のメーター表示用)。 */
    f32         audio_level    = 0.0f;
};

/**
 * 1 push あたりの最大サンプル数。
 *
 * @details
 * 実 codec (Opus) なら 20ms@48kHz=960 等だが、本ループバックは sample-rate 非依存で
 * 「呼び出し側が渡した int16 配列」をそのまま 1 フレームとして扱う (識別 PCM framing)。
 * 上限のみ規定し、超過は分割を呼び出し側に委ねる (over-large frame は
 * kSubVoiceBadArgument)。
 */
inline constexpr u32 kVoiceMaxFrameSamples = 4096;

/** フレームヘッダの破損検知用マジック ('ACV1' = ACS Voice frame v1)。 */
inline constexpr u32 kVoiceFrameMagic      = 0x41435631u;

/**
 * 線形 PCM フレームの固定ヘッダ (16 byte、LE)。
 *
 * @details
 * encode 時に前置し、decode 時に magic / サンプル数を検証してから payload
 * (int16 PCM) を復元する。識別変換だが seq + magic + サンプル数を伴う real な
 * framing であり、実 wire codec を後で差し込む際の境界もここで確定する。
 */
struct FVoiceFrameHeader {
    /** 破損検知用マジック (kVoiceFrameMagic)。 */
    u32 magic        = kVoiceFrameMagic;

    /** 送信元ごとの単調増加シーケンス番号。 */
    u32 sequence     = 0;

    /** payload の int16 サンプル数。 */
    u32 sample_count = 0;

    /** codec id / channel 用の予約フィールド (現状 0)。 */
    u32 reserved     = 0;
};

/**
 * 各 SDK の差を吸収するボイスチャット backend の純粋仮想インターフェース。
 *
 * @details
 * Steam Voice / EOS Voice / Vivox / Discord / OpusSelf の差を吸収する。実装は
 * 本体外モジュール (or テスト) で override する。
 */
class IVoiceChatBackend {
public:
    /** デフォルト構築する。 */
    IVoiceChatBackend() noexcept = default;

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~IVoiceChatBackend() noexcept = default;

    /** コピー禁止 (backend は 1 個の長寿命オブジェクトとして扱う)。 */
    IVoiceChatBackend(const IVoiceChatBackend&)            = delete;

    /** コピー代入も禁止。 */
    IVoiceChatBackend& operator=(const IVoiceChatBackend&) = delete;

    /** ムーブ禁止 (SDK ハンドルの二重解放を防ぐため)。 */
    IVoiceChatBackend(IVoiceChatBackend&&)                 = delete;

    /** ムーブ代入も禁止。 */
    IVoiceChatBackend& operator=(IVoiceChatBackend&&)      = delete;

    /**
     * SDK を初期化する。
     *
     * @details Stub は受け取った p を記録するのみで実 SDK 呼び出しは行わない。多重 Init は実装依存。
     * @param p 使用するプロバイダ。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> Init(EVoiceProvider p) noexcept = 0;

    /** SDK 終了処理を行う (Init() 前に呼んでも安全な no-op)。 */
    virtual void Shutdown() noexcept = 0;

    /**
     * 利用可能か (Init 済 + SDK 接続可) を返す。
     *
     * @return 利用可能なら true (Stub は常に false)。
     */
    virtual bool IsAvailable() const noexcept = 0;

    /**
     * 現在アクティブなプロバイダを返す。
     *
     * @return アクティブなプロバイダ (Init 前は EVoiceProvider::None)。
     */
    virtual EVoiceProvider ActiveProvider() const noexcept = 0;

    /**
     * 指定チャンネルに参加する。
     *
     * @param ch 参加するチャンネル種別。
     * @param channel_id SDK 固有のチャンネル ID 文字列 (party_id / マッチ ID 等を渡す想定、非所有)。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> JoinChannel(EVoiceChannel ch, const char* channel_id) noexcept = 0;

    /**
     * 指定チャンネルから離脱する。
     *
     * @details join 前に呼んでも安全な実装が望ましい。
     * @param ch 離脱するチャンネル種別。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> LeaveChannel(EVoiceChannel ch) noexcept = 0;

    /**
     * 自分のマイクをローカル側でミュート / 解除する。
     *
     * @details 送信を止めるかどうかは実装依存 (Vivox はミュート時も local capture を続けるが送信を停止)。
     * @param muted true でミュート、false で解除。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> SetLocalMute(bool muted) noexcept = 0;

    /**
     * 指定参加者をローカル側でミュートする (自分の耳でのみ無音化)。
     *
     * @param user_id GetParticipant で得た user_id (非所有、SDK 側で同定可能)。
     * @param muted true でミュート、false で解除。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> SetParticipantMute(const char* user_id, bool muted) noexcept = 0;

    /**
     * 指定参加者のローカル音量を設定する。
     *
     * @details 範囲外を渡した場合の振る舞いは実装依存 (clamp 推奨)。
     * @param user_id GetParticipant で得た user_id (非所有)。
     * @param volume 0.0〜1.0 (もしくは SDK が許す範囲) の音量。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> SetParticipantVolume(const char* user_id, f32 volume) noexcept = 0;

    /**
     * 指定チャンネルの参加者数 (自分含む) を返す。
     *
     * @param ch 対象チャンネル種別。
     * @return 参加者数 (未 join 時は 0)。
     */
    virtual u32 ParticipantCount(EVoiceChannel ch) noexcept = 0;

    /**
     * 指定チャンネルの index 番目の参加者を取得する。
     *
     * @details 戻り値の user_id / display_name 寿命は次 Tick() まで保証される。
     * @param ch 対象チャンネル種別。
     * @param index 参加者インデックス。
     * @return 参加者情報。範囲外 index はエラー。
     */
    virtual TResult<FVoiceParticipant> GetParticipant(EVoiceChannel ch, u32 index) noexcept = 0;

    /**
     * コールバック / 状態更新ポンプ (ゲームループから毎フレーム呼ぶ)。
     *
     * @details Vivox の Update3DPosition / Discord の event pump / VAD threshold 監視を Backend 側に畳み込む。
     * @param dt 実時間秒 (実装によっては使わない)。
     */
    virtual void Tick(f32 dt) noexcept = 0;

    /**
     * ローカルマイク相当の int16 PCM フレームを 1 つ push する (push 型 capture)。
     *
     * @details
     * 実 SDK backend はマイク捕捉/再生を SDK 内部で行うため override せず、既定実装は
     * NotImplemented を返す。`CVoiceChatLoopbackBackend` のみが本実装する。ローカル
     * ミュート中は送信されない (capture は続くが route しない)。
     * @param ch 送信先チャンネル種別。
     * @param pcm sample_count 個の int16 サンプル (非所有、本呼び出し中のみ参照)。
     * @param sample_count PCM のサンプル数。
     * @return 成功なら空の TResult、既定実装は NotImplemented エラー。
     */
    virtual TResult<void> PushLocalFrame(EVoiceChannel ch, const i16* pcm, u32 sample_count) noexcept {
        (void)ch; (void)pcm; (void)sample_count;
        return ACS_ERR(Generic, kSubVoiceNotImplemented,
                       "IVoiceChatBackend::PushLocalFrame: backend does not support PCM push");
    }

    /**
     * ローカル float [-1,1] PCM を push する利便メソッド (内部で int16 に量子化)。
     *
     * @param ch 送信先チャンネル種別。
     * @param pcm sample_count 個の float サンプル (非所有、本呼び出し中のみ参照)。
     * @param sample_count PCM のサンプル数。
     * @return 成功なら空の TResult、既定実装は NotImplemented エラー。
     */
    virtual TResult<void> PushLocalFrameF32(EVoiceChannel ch, const f32* pcm, u32 sample_count) noexcept {
        (void)ch; (void)pcm; (void)sample_count;
        return ACS_ERR(Generic, kSubVoiceNotImplemented,
                       "IVoiceChatBackend::PushLocalFrameF32: backend does not support PCM push");
    }

    /**
     * 自分以外の全参加者から受信したフレームを N-way mix して out に書き込む。
     *
     * @details
     * per-participant gain / mute を適用して sum + clamp i16 する。mix 後に消費された
     * フレームは各参加者キューから取り除かれる。既定実装は 0 (無音) を返す。
     * @param ch 対象チャンネル種別。
     * @param out mix 結果を書き込む int16 PCM バッファ。
     * @param out_capacity out の容量 (サンプル数)。超過分は捨てる。
     * @return 実際に書き込んだサンプル数。
     */
    virtual u32 PumpMixedOutput(EVoiceChannel ch, i16* out, u32 out_capacity) noexcept {
        (void)ch; (void)out; (void)out_capacity;
        return 0;
    }

    /**
     * 直近に push したローカルフレームの音声レベル (RMS) を返す。
     *
     * @return RMS 音声レベル (0.0〜1.0、VAD/メーター用)。既定実装は 0.0f。
     */
    virtual f32 LocalAudioLevel() const noexcept { return 0.0f; }
};

/**
 * SDK 未統合ビルド / ユニットテスト用の no-op backend 実装。
 *
 * @details
 * Init() は受け取った provider を記録するが操作系 API は全て
 * ACS_ERR(Generic, kSubVoiceNotImplemented, ...) を返し、IsAvailable() は常に false。
 * Shutdown() / Tick() は副作用なし。ISteamworksBridge と違い「未実装」を強調する。
 */
class CVoiceChatBackendStub final : public IVoiceChatBackend {
public:
    /** デフォルト構築する。 */
    CVoiceChatBackendStub() noexcept = default;

    /** 破棄する (副作用なし)。 */
    ~CVoiceChatBackendStub() noexcept override = default;

    /**
     * provider を記録するだけの初期化を行う。
     *
     * @param p 記録するプロバイダ。
     * @return 常に成功。
     */
    TResult<void>             Init(EVoiceProvider p) noexcept override;

    /** 状態を初期値に戻す (副作用なしの終了処理)。 */
    void                     Shutdown() noexcept override;

    /**
     * 利用可能かを返す。
     *
     * @return Stub なので常に false。
     */
    bool                     IsAvailable() const noexcept override { return false; }

    /**
     * 記録済みのプロバイダを返す。
     *
     * @return Init で記録したプロバイダ (未 Init は None)。
     */
    EVoiceProvider            ActiveProvider() const noexcept override { return m_Provider; }

    /**
     * チャンネル参加 (未実装)。
     *
     * @param ch 参加するチャンネル種別。
     * @param channel_id チャンネル ID 文字列。
     * @return 未初期化なら NotInitialized、それ以外は NotImplemented エラー。
     */
    TResult<void>             JoinChannel(EVoiceChannel ch, const char* channel_id) noexcept override;

    /**
     * チャンネル離脱 (未実装)。
     *
     * @param ch 離脱するチャンネル種別。
     * @return 未初期化なら NotInitialized、それ以外は NotImplemented エラー。
     */
    TResult<void>             LeaveChannel(EVoiceChannel ch) noexcept override;

    /**
     * ローカルミュート (未実装)。
     *
     * @param muted ミュート指定。
     * @return 未初期化なら NotInitialized、それ以外は NotImplemented エラー。
     */
    TResult<void>             SetLocalMute(bool muted) noexcept override;

    /**
     * 参加者ミュート (未実装)。
     *
     * @param user_id 対象 user_id。
     * @param muted ミュート指定。
     * @return 未初期化なら NotInitialized、それ以外は NotImplemented エラー。
     */
    TResult<void>             SetParticipantMute(const char* user_id, bool muted) noexcept override;

    /**
     * 参加者音量設定 (未実装)。
     *
     * @param user_id 対象 user_id。
     * @param volume 音量。
     * @return 未初期化なら NotInitialized、それ以外は NotImplemented エラー。
     */
    TResult<void>             SetParticipantVolume(const char* user_id, f32 volume) noexcept override;

    /**
     * 参加者数を返す。
     *
     * @param ch 対象チャンネル種別。
     * @return Stub なので常に 0。
     */
    u32                      ParticipantCount(EVoiceChannel ch) noexcept override;

    /**
     * 参加者取得 (未実装)。
     *
     * @param ch 対象チャンネル種別。
     * @param index 参加者インデックス。
     * @return 未初期化なら NotInitialized、それ以外は NotImplemented エラー。
     */
    TResult<FVoiceParticipant> GetParticipant(EVoiceChannel ch, u32 index) noexcept override;

    /**
     * 状態更新ポンプ (何もしない)。
     *
     * @param dt 実時間秒 (未使用)。
     */
    void                     Tick(f32 dt) noexcept override;

private:
    /** Init で記録したプロバイダ。 */
    EVoiceProvider m_Provider = EVoiceProvider::None;

    /** Init 済みフラグ。 */
    bool          m_Initialized = false;
};

/**
 * Stub backend の static singleton アクセサ (Meyer's singleton)。
 *
 * @details
 * 実 SDK 実装が DI される前のデフォルト。ISteamworksBridge::GetStub() と同じパターン。
 * @return 共有 Stub backend への参照。
 */
IVoiceChatBackend& GetVoiceStub() noexcept;

/**
 * SDK 非依存で実際に音声フレームを往復させる in-process ループバック backend。
 *
 * @details
 * Vivox 等の外部 SDK をリンクできない / したくないビルドでも、同一プロセス内で
 * PushLocalFrame(A) → encode(framing) → A 以外の全参加者の受信キュー → decode
 * → per-participant gain/mute → N-way mix(sum+clamp) → PumpMixedOutput(B) を
 * 成立させる。ネットワークは使わず純粋に in-process なので決定的でユニットテスト可能。
 * 各チャンネル (Party/Team/Global/Custom) は独立した参加者テーブル + per-user 受信
 * キューを持ち、index 0 の participant をローカルユーザ (= 自分) として扱う。
 */
class CVoiceChatLoopbackBackend final : public IVoiceChatBackend {
public:
    /** DefaultAllocator を使って構築する。 */
    CVoiceChatLoopbackBackend() noexcept;

    /**
     * 指定 allocator をチャンネル・参加者・受信キューに使って構築する。
     *
     * @param allocator 可変長状態の確保に使う allocator。
     */
    explicit CVoiceChatLoopbackBackend(FAllocator& allocator) noexcept;

    /** 破棄する。 */
    ~CVoiceChatLoopbackBackend() noexcept override = default;

    /**
     * backend を初期化し、全チャンネルの状態をリセットする。
     *
     * @param p 記録するプロバイダ。
     * @return 常に成功。
     */
    TResult<void>             Init(EVoiceProvider p) noexcept override;

    /** 状態を初期値に戻し全チャンネルをクリアする。 */
    void                     Shutdown() noexcept override;

    /**
     * 利用可能かを返す。
     *
     * @return Init 済みなら true。
     */
    bool                     IsAvailable() const noexcept override { return m_Initialized; }

    /**
     * 記録済みのプロバイダを返す。
     *
     * @return Init で記録したプロバイダ。
     */
    EVoiceProvider            ActiveProvider() const noexcept override { return m_Provider; }

    /**
     * 指定チャンネルに参加する (joined フラグを立て channel_id を保持)。
     *
     * @param ch 参加するチャンネル種別。
     * @param channel_id 保持するチャンネル ID (null なら空文字)。
     * @return 成功なら空の TResult、未初期化なら NotInitialized エラー。
     */
    TResult<void>             JoinChannel(EVoiceChannel ch, const char* channel_id) noexcept override;

    /**
     * 指定チャンネルから離脱し参加者・キューを破棄する。
     *
     * @param ch 離脱するチャンネル種別。
     * @return 成功なら空の TResult、未初期化なら NotInitialized エラー。
     */
    TResult<void>             LeaveChannel(EVoiceChannel ch) noexcept override;

    /**
     * ローカルミュートフラグを設定する (送信抑止に使う)。
     *
     * @param muted true でミュート、false で解除。
     * @return 成功なら空の TResult、未初期化なら NotInitialized エラー。
     */
    TResult<void>             SetLocalMute(bool muted) noexcept override;

    /**
     * 指定参加者を全チャンネル横断でミュート / 解除する。
     *
     * @param user_id 対象 user_id (非所有)。
     * @param muted true でミュート、false で解除。
     * @return 成功なら空の TResult、未知 user は UnknownUser エラー。
     */
    TResult<void>             SetParticipantMute(const char* user_id, bool muted) noexcept override;

    /**
     * 指定参加者の音量を全チャンネル横断で設定する (0.0〜2.0 に clamp)。
     *
     * @param user_id 対象 user_id (非所有)。
     * @param volume 設定する音量。
     * @return 成功なら空の TResult、未知 user は UnknownUser エラー。
     */
    TResult<void>             SetParticipantVolume(const char* user_id, f32 volume) noexcept override;

    /**
     * 指定チャンネルの参加者数を返す。
     *
     * @param ch 対象チャンネル種別。
     * @return 参加者数 (未初期化 / 未 join は 0)。
     */
    u32                      ParticipantCount(EVoiceChannel ch) noexcept override;

    /**
     * 指定チャンネルの index 番目の参加者情報を取得する。
     *
     * @param ch 対象チャンネル種別。
     * @param index 参加者インデックス。
     * @return 参加者情報。未 join は NotJoined、範囲外 index は BadArgument エラー。
     */
    TResult<FVoiceParticipant> GetParticipant(EVoiceChannel ch, u32 index) noexcept override;

    /**
     * 状態更新ポンプ (ループバックは何もしない)。
     *
     * @param dt 実時間秒 (未使用)。
     */
    void                     Tick(f32 dt) noexcept override;

    /**
     * ローカル int16 PCM フレームを encode し、自分以外の全参加者キューへ enqueue する。
     *
     * @details RMS / peak を実データから算出する。ローカルミュート中は計測のみ行い route しない。
     * @param ch 送信先チャンネル種別。
     * @param pcm sample_count 個の int16 サンプル (非所有)。
     * @param sample_count PCM のサンプル数 (kVoiceMaxFrameSamples 以下)。
     * @return 成功なら空の TResult、各種前提違反でエラー。
     */
    TResult<void> PushLocalFrame(EVoiceChannel ch, const i16* pcm, u32 sample_count) noexcept override;

    /**
     * ローカル float [-1,1] PCM を int16 に量子化してから PushLocalFrame に合流する。
     *
     * @param ch 送信先チャンネル種別。
     * @param pcm sample_count 個の float サンプル (非所有)。
     * @param sample_count PCM のサンプル数 (kVoiceMaxFrameSamples 以下)。
     * @return 成功なら空の TResult、各種前提違反でエラー。
     */
    TResult<void> PushLocalFrameF32(EVoiceChannel ch, const f32* pcm, u32 sample_count) noexcept override;

    /**
     * pump 対象参加者の受信キューを decode・gain 適用して N-way mix し out に書き込む。
     *
     * @param ch 対象チャンネル種別。
     * @param out mix 結果を書き込む int16 PCM バッファ。
     * @param out_capacity out の容量 (サンプル数)。
     * @return 実際に書き込んだサンプル数。
     */
    u32           PumpMixedOutput(EVoiceChannel ch, i16* out, u32 out_capacity) noexcept override;

    /**
     * 直近 push したローカルフレームの RMS 音声レベルを返す。
     *
     * @return RMS 音声レベル (0.0〜1.0)。
     */
    f32           LocalAudioLevel() const noexcept override { return m_LastLocalRms; }

    /**
     * 参加者を ch に追加する (最初に追加した参加者がローカルユーザ)。
     *
     * @details 同一 user_id の二重追加は表示名のみ更新して成功 (冪等) 扱い。
     * @param ch 追加先チャンネル種別。
     * @param user_id 追加する参加者の user_id (空文字は不可)。
     * @param display_name 表示名 (null なら空文字)。
     * @return 成功なら空の TResult、未 join は NotJoined、空 user_id は BadArgument エラー。
     */
    TResult<void> AddParticipant(EVoiceChannel ch, const char* user_id, const char* display_name) noexcept;

    /**
     * 参加者を ch から取り除く (受信キューも破棄)。
     *
     * @param ch 対象チャンネル種別。
     * @param user_id 取り除く参加者の user_id。
     * @return 成功なら空の TResult、未知 user は UnknownUser エラー。
     */
    TResult<void> RemoveParticipant(EVoiceChannel ch, const char* user_id) noexcept;

    /**
     * PumpMixedOutput が「誰の耳」としてミックスするかを選ぶ。
     *
     * @details 既定はローカルユーザ (index 0)。指定 user の受信キューを mix 対象にする。
     * @param ch 対象チャンネル種別。
     * @param user_id pump 対象とする user_id。
     * @return 成功なら空の TResult、未知 user は UnknownUser エラー。
     */
    TResult<void> SetPumpTarget(EVoiceChannel ch, const char* user_id) noexcept;

    /**
     * 直近 push したローカルフレームのピーク絶対値を返す。
     *
     * @return ピーク値 (0.0〜1.0)。
     */
    f32 LocalAudioPeak() const noexcept { return m_LastLocalPeak; }

    /**
     * 指定参加者の受信キューに溜まっているフレーム数を返す (テスト検証用)。
     *
     * @param ch 対象チャンネル種別。
     * @param user_id 対象 user_id。
     * @return キューに溜まっているフレーム数。
     */
    u32 PendingFrameCount(EVoiceChannel ch, const char* user_id) noexcept;

    /**
     * int16 PCM を framed バイト列に encode する (ユニットテストから直接叩ける)。
     *
     * @details out は header(16B) + sample_count*2 byte 必要。
     * @param pcm 入力 int16 PCM (sample_count 個、非所有)。
     * @param sample_count PCM のサンプル数。
     * @param sequence フレームヘッダに書くシーケンス番号。
     * @param out 書き込み先バッファ。
     * @param out_capacity out の容量 (バイト)。
     * @return 書き込んだ総バイト数 (0 = 引数不正)。
     */
    static u32 EncodeFrame(const i16* pcm, u32 sample_count, u32 sequence,
                           u8* out, u32 out_capacity) noexcept;

    /**
     * framed バイト列を int16 PCM に decode する (header を検証して payload を復元)。
     *
     * @param in 入力 framed バイト列 (非所有)。
     * @param in_size in のバイト数。
     * @param out 復元先 int16 PCM バッファ。
     * @param out_capacity out の容量 (サンプル数)。
     * @return 成功時は復元したサンプル数。magic 破損等は FrameCorrupt エラー。
     */
    static TResult<u32> DecodeFrame(const u8* in, u32 in_size,
                                    i16* out, u32 out_capacity) noexcept;

private:
    /**
     * 参加者ごとの状態。
     *
     * @details
     * 受信キューは encode 済みバイト列を時系列に連結した frame 群を保持する
     * (各 frame = FVoiceFrameHeader + payload)。
     */
    struct FLoopParticipant {
        /** DefaultAllocator を使う空の参加者を構築する。 */
        FLoopParticipant() noexcept = default;

        /** 指定 allocator を文字列と受信キューへ固定する。 */
        explicit FLoopParticipant(FAllocator& allocator) noexcept
            : user_id(allocator), display_name(allocator), rx_frames(allocator)
        {
        }

        /** 参加者 ID (非所有 const char* を受けて所有コピー)。 */
        FString        user_id;

        /** 表示名 (所有コピー)。 */
        FString        display_name;

        /** ローカル音量 (mix 時に適用)。 */
        f32            volume      = 1.0f;

        /** 自分から見たミュート。 */
        bool           muted_local = false;

        /** この送信元が次に使う sequence。 */
        u32            next_seq    = 0;

        /** 受信フレーム連結バッファ (encode 済み)。 */
        TArray<u8>     rx_frames;
    };

    /** 1 チャンネル分の状態 (参加者テーブル + pump 対象)。 */
    struct FChannel {
        /** DefaultAllocator を使う空のチャンネルを構築する。 */
        FChannel() noexcept = default;

        /** 指定 allocator を ID と参加者配列へ固定する。 */
        explicit FChannel(FAllocator& allocator) noexcept : channel_id(allocator), participants(allocator)
        {
        }

        /** チャンネルに join 済みか。 */
        bool                      joined       = false;

        /** join 時に保持したチャンネル ID。 */
        FString                   channel_id;

        /** PumpMixedOutput の対象参加者 index (既定 0=ローカル)。 */
        u32                       pump_target  = 0;

        /** このチャンネルの参加者テーブル。 */
        TArray<FLoopParticipant>  participants;
    };

    /**
     * ch に対応する FChannel を返す (可変参照)。
     *
     * @param ch 対象チャンネル種別。
     * @return 対応する FChannel への参照 (常に 4 種固定で確保済み)。
     */
    FChannel&       Chan(EVoiceChannel ch) noexcept { return m_Channels[static_cast<u32>(ch)]; }

    /**
     * ch に対応する FChannel を返す (const 参照)。
     *
     * @param ch 対象チャンネル種別。
     * @return 対応する FChannel への const 参照。
     */
    const FChannel& Chan(EVoiceChannel ch) const noexcept { return m_Channels[static_cast<u32>(ch)]; }

    /**
     * ch 内で user_id を線形検索する。
     *
     * @param c 検索対象のチャンネル。
     * @param user_id 探す user_id。
     * @return 一致した参加者の index (見つからなければ participants.Size())。
     */
    u32 FindParticipant(const FChannel& c, const char* user_id) const noexcept;

    /**
     * 1 フレームを participant の rx_frames に encode して enqueue する。
     *
     * @param p enqueue 先の参加者。
     * @param pcm int16 PCM (sample_count 個、非所有)。
     * @param sample_count PCM のサンプル数。
     */
    void EnqueueFrame(FLoopParticipant& p, const i16* pcm, u32 sample_count) noexcept;

    /** Init で記録したプロバイダ。 */
    EVoiceProvider m_Provider     = EVoiceProvider::None;

    /** Init 済みフラグ。 */
    bool          m_Initialized  = false;

    /** ローカルミュート (送信抑止)。 */
    bool          m_LocalMuted   = false;

    /** 直近 push の RMS。 */
    f32           m_LastLocalRms = 0.0f;

    /** 直近 push のピーク。 */
    f32           m_LastLocalPeak= 0.0f;

    /** チャンネル内の可変長状態が使う allocator。 */
    FAllocator* m_Allocator = nullptr;

    /** チャンネル状態 (Party/Team/Global/Custom の 4 種固定)。 */
    FChannel m_Channels[4];
};

/**
 * ループバック backend の static singleton アクセサ。
 *
 * @details DI 不要で `m_Voice = &GetVoiceLoopback();` だけで本物の音声往復が動く。
 * @return 共有ループバック backend への参照。
 */
CVoiceChatLoopbackBackend& GetVoiceLoopback() noexcept;

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FVoiceChatBackendStub = CVoiceChatBackendStub;

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FVoiceChatLoopbackBackend = CVoiceChatLoopbackBackend;

} // namespace acs::game

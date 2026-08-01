// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — IAudioBackend (実音声再生 seam)
//
// 役割:
//   `CAudioDirector` から見た「実際に音を出す層」の純粋仮想インターフェース。
//   Windows では `CXAudio2Backend`、それ以外プラットフォーム (将来) では別実装
//   (CoreAudio / ALSA / OpenAL / WebAudio …) で差し替える。
//
// 設計選択:
//   ・**voice handle 方式**: BGM / SFX を統一的に「voice」として扱い、32bit の
//     不透明値で一意化する。0 は無効値として予約する。
//   ・**TResult<void, FErrorCode> for Init**: backend 初期化のみ失敗ありえる
//     (COM init / device 取得失敗等)。Play 系は noexcept で「鳴らせなければ
//     InvalidHandle を返す」設計 (1 フレで何度も呼ぶ hot path なので TResult
//     を回避)。
//   ・**Tick(dt)**: 再生完了済 voice の slot 解放を backend 側に畳み込む。
//     ゲーム側は dt を渡すだけで一発再生の自然回収を任せられる。
//   ・**所有しない pcm_data**: clip データは CAudioDirector / asset layer 側で
//     管理。backend は PlayOneShot 中に内部コピー (XAudio2 はバッファを保持
//     しないと一発再生中に消えると爆ぜる)。
//   ・**コピー / ムーブ禁止**: backend は 1 個の長寿命オブジェクト。誤コピー
//     で COM ハンドル二重解放を避けるため最初から非コピー・非ムーブ。
//   ・**STL 不使用 / 全 noexcept**: ACS 全体方針。
//
// 範囲外:
//   ・3D positional / spatial / HRTF (Pillar CSpatialAudio 担当)
//   ・submix bus / DSP chain / reverb
//   ・wav/ogg/mp3 デコード (今回は Pcm16 raw bytes 入力前提、Wav 形式は
//     別 loader と組合せる)
//   ・streaming (大型 BGM をオンメモリせず逐次デコード)
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

/** Init を 2 回呼んだ (多重初期化)。 */
inline constexpr u16 kSubAudioAlreadyInitialized = 1200;

/** Init() より前に API を呼び出した。 */
inline constexpr u16 kSubAudioNotInitialized     = 1201;

/** COM MTA 利用参照の取得に失敗した。 */
inline constexpr u16 kSubAudioComInitFailed      = 1202;

/** XAudio2Create または同等の生成呼び出しに失敗した。 */
inline constexpr u16 kSubAudioCreateFailed       = 1203;

/** CreateMasteringVoice に失敗した。 */
inline constexpr u16 kSubAudioMasterVoiceFailed  = 1204;

/** 引数が不正 (Init(max_voices=0) など)。 */
inline constexpr u16 kSubAudioInvalidArgs        = 1205;

/** backend 内部状態または再生 pool のメモリ確保に失敗した。 */
inline constexpr u16 kSubAudioOutOfMemory        = 1206;

/**
 * PlayOneShot / PlayLooped に渡す clip の音声フォーマット種別。
 *
 * @details
 * Pcm16 は典型的な WAV PCM の raw bytes、Pcm32Float は DSP-friendly な高品質形式、
 * Wav はファイルからロード済の WAV 形式 (パーサは別途) を表す。
 */
enum class EAudioFormat : u8 {
    /** 16bit signed PCM (典型的な WAV PCM 形式の raw bytes)。 */
    Pcm16      = 0,

    /** 32bit IEEE float PCM (高品質、DSP-friendly)。 */
    Pcm32Float = 1,

    /** ファイルからロード済の WAV 形式 (パーサ別途)。 */
    Wav        = 2,
};

/**
 * 再生する音声バッファのフォーマット + データ参照を表す clip 記述子。
 *
 * @details
 * pcm_data は backend 側で必要な間 (一発再生終了 / Stop まで) 内部コピーされるため、
 * 呼び出し側で寿命を延ばす必要はない。
 */
struct FAudioClipDesc {
    /** raw PCM サンプル列 (Wav 形式の場合は RIFF ヘッダ込み)。 */
    const void*  pcm_data      = nullptr;

    /** pcm_data の有効バイト数。 */
    u64          pcm_size      = 0;

    /** 1 チャネルあたりサンプル/秒 (例: 44100 / 48000)。 */
    u32          sample_rate   = 0;

    /** チャネル数 (1=mono / 2=stereo)。 */
    u32          channel_count = 0;

    /** pcm_data のフォーマット種別。 */
    EAudioFormat format        = EAudioFormat::Pcm16;
};

/**
 * 再生中の voice を一意に指す 32bit ハンドル。
 *
 * @details
 * 全 0 (= m_Packed == 0) は無効ハンドル (kInvalidAudioVoice) を意味する。
 * index + generation 形式を使う独自 backend 向けの互換コンストラクタを残しつつ、
 * CXAudio2Backend は 8bit generation の早期衝突を避けるため 32bit 全体を
 * プロセス通算の不透明チケットとして扱う。呼び出し側は値を分解せず保持して返す。
 */
struct FAudioVoiceHandle {
    /** backend が発行する 32bit 値 (0=無効、互換形式では index + generation)。 */
    u32 m_Packed = 0;

    /** 無効ハンドル (m_Packed=0) を構築する。 */
    constexpr FAudioVoiceHandle() noexcept = default;

    /**
     * index と generation を packed 値に詰めて構築する。
     *
     * @param index voice の slot インデックス (下位 24bit)。
     * @param gen slot の世代カウンタ (上位 8bit、有効ハンドルでは 1 以上)。
     */
    constexpr FAudioVoiceHandle(u32 index, u8 gen) noexcept
        : m_Packed((index & 0x00FFFFFFu) | (static_cast<u32>(gen) << 24)) {}

    /**
     * backend が生成した 32bit の不透明値からハンドルを構築する。
     *
     * @details
     * m_Packed のサイズと配置を変えずに、backend が 32bit 全体を衝突回避用の
     * チケットとして利用できる。0 は常に無効値として予約される。
     * @param packed_value 0 以外の不透明なハンドル値。
     * @return 指定値を保持するハンドル。
     */
    static constexpr FAudioVoiceHandle FromPackedValue(u32 packed_value) noexcept
    {
        FAudioVoiceHandle handle;
        handle.m_Packed = packed_value;
        return handle;
    }

    /**
     * 有効なハンドルかを返す。
     *
     * @return packed 値が非 0 (= 有効) なら true。
     */
    bool IsValid() const noexcept { return m_Packed != 0u; }

    /**
     * 互換形式における下位 24bit を返す。
     *
     * @return packed 値の下位 24bit。
     */
    u32 Index() const noexcept { return m_Packed & 0x00FFFFFFu; }

    /**
     * 互換形式における上位 8bit を返す。
     *
     * @return packed 値の上位 8bit。
     */
    u8  Generation() const noexcept { return static_cast<u8>(m_Packed >> 24); }

    /** backend 間の受け渡しに使う 32bit の不透明値を返す。 */
    u32 PackedValue() const noexcept
    {
        return m_Packed;
    }

    /** 2 つのハンドルが同じ発音を表すか比較する。 */
    constexpr bool operator==(FAudioVoiceHandle other) const noexcept
    {
        return m_Packed == other.m_Packed;
    }
};

static_assert(sizeof(FAudioVoiceHandle) == sizeof(u32), "AudioVoiceHandle must retain its 32-bit ABI");

/** 無効を表す voice ハンドル定数 (m_Packed=0)。 */
inline constexpr FAudioVoiceHandle kInvalidAudioVoice {};

/**
 * CAudioDirector から見た「実際に音を出す層」の純粋仮想インターフェース。
 *
 * @details
 * CXAudio2Backend / 将来の CoreAudioBackend / NullAudioBackend (テスト用) 等の差を
 * 吸収する。`CAudioDirector::SetBackend(IAudioBackend*)` で差し込み、CAudioDirector は
 * backend が nullptr のとき無音 (no-op) で動作する。
 */
class IAudioBackend {
public:
    /** 既定構築する。 */
    IAudioBackend() noexcept = default;

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~IAudioBackend() noexcept = default;

    /** コピー禁止 (backend は 1 個の長寿命オブジェクト、COM ハンドル二重解放を防ぐ)。 */
    IAudioBackend(const IAudioBackend&)            = delete;

    /** コピー代入も禁止。 */
    IAudioBackend& operator=(const IAudioBackend&) = delete;

    /** ムーブ禁止。 */
    IAudioBackend(IAudioBackend&&)                 = delete;

    /** ムーブ代入も禁止。 */
    IAudioBackend& operator=(IAudioBackend&&)      = delete;

    /**
     * backend を初期化する。
     *
     * @details 多重 Init は kSubAudioAlreadyInitialized エラーになる。
     * @param max_voices 同時発音数の上限 (slot 数)。0 は不正。
     * @return 成功なら空の TResult、初期化失敗なら対応する subcode 付きエラー。
     */
    virtual TResult<void> Init(u32 max_voices = 64) noexcept = 0;

    /** 全 voice を停止して資源を解放する (Init 前に呼んでも安全な no-op)。 */
    virtual void Shutdown() noexcept = 0;

    /**
     * Init 済かつ実バックエンドが正常起動した状態かを返す。
     *
     * @return 初期化済みで稼働中なら true。
     */
    virtual bool IsInitialized() const noexcept = 0;

    /**
     * 一発再生する (loop なし、終端で slot を自動回収)。
     *
     * @details 失敗時 (未 init / 空きスロットなし / フォーマット不正) は kInvalidAudioVoice を返す。
     * @param clip 再生する音声バッファの記述子。
     * @param volume 音量 (0.0〜1.0 を想定、実装は clamp 推奨)。
     * @param pitch 再生ピッチ (1.0=等倍、0.5=1 オクターブ低い、2.0=1 オクターブ高い)。
     * @return 再生 voice のハンドル (失敗時は kInvalidAudioVoice)。
     */
    virtual FAudioVoiceHandle PlayOneShot(const FAudioClipDesc& clip,
                                         f32 volume,
                                         f32 pitch) noexcept = 0;

    /**
     * ループ再生する (StopVoice まで鳴り続ける)。
     *
     * @details 失敗時 (未 init / 空きスロットなし / フォーマット不正) は kInvalidAudioVoice を返す。
     * @param clip 再生する音声バッファの記述子。
     * @param volume 音量 (0.0〜1.0 を想定、実装は clamp 推奨)。
     * @param pitch 再生ピッチ (1.0=等倍、0.5=1 オクターブ低い、2.0=1 オクターブ高い)。
     * @return 再生 voice のハンドル (失敗時は kInvalidAudioVoice)。
     */
    virtual FAudioVoiceHandle PlayLooped(const FAudioClipDesc& clip,
                                        f32 volume,
                                        f32 pitch) noexcept = 0;

    /**
     * 指定 voice を停止し slot を解放する。
     *
     * @param voice 停止する voice ハンドル (無効 / 既に解放済なら no-op)。
     */
    virtual void StopVoice(FAudioVoiceHandle voice) noexcept = 0;

    /**
     * 指定 voice の音量を変更する。
     *
     * @param voice 対象の voice ハンドル (無効 / 解放済なら no-op)。
     * @param volume 新しい音量 (範囲外は clamp 推奨)。
     */
    virtual void SetVoiceVolume(FAudioVoiceHandle voice, f32 volume) noexcept = 0;

    /** 全 voice を停止して slot を解放する (Init 前は no-op)。 */
    virtual void StopAllVoices() noexcept = 0;

    /**
     * 現在再生中 (= slot active) の voice 数を返す。
     *
     * @details デバッグ / UI メーター用。
     * @return アクティブな voice 数。
     */
    virtual u32 ActiveVoiceCount() const noexcept = 0;

    /**
     * 内部状態を 1 フレーム進める (完了した一発再生 voice の slot 解放等)。
     *
     * @details ゲームループから毎フレーム呼ぶ。
     * @param dt 実時間の経過秒 (実装によっては使わない)。
     */
    virtual void Tick(f32 dt) noexcept = 0;
};

} // namespace acs::game

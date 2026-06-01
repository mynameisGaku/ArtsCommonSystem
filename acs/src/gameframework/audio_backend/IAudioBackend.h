// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — IAudioBackend (Phase 2: 実音声再生 seam)
//
// 役割:
//   `FAudioDirector` から見た「実際に音を出す層」の純粋仮想インターフェース。
//   Windows では `FXAudio2Backend`、それ以外プラットフォーム (将来) では別実装
//   (CoreAudio / ALSA / OpenAL / WebAudio …) で差し替える。
//
// 設計選択:
//   ・**voice handle 方式**: BGM / SFX を統一的に「voice」として扱い、24bit
//     index + 8bit generation で一意化する。slot 再利用時の use-after-free
//     を generation で検出。
//   ・**TResult<void, FErrorCode> for Init**: backend 初期化のみ失敗ありえる
//     (COM init / device 取得失敗等)。Play 系は noexcept で「鳴らせなければ
//     InvalidHandle を返す」設計 (1 フレで何度も呼ぶ hot path なので TResult
//     を回避)。
//   ・**Tick(dt)**: 再生完了済 voice の slot 解放を backend 側に畳み込む。
//     ゲーム側は dt を渡すだけで一発再生の自然回収を任せられる。
//   ・**所有しない pcm_data**: clip データは FAudioDirector / asset layer 側で
//     管理。backend は PlayOneShot 中に内部コピー (XAudio2 はバッファを保持
//     しないと一発再生中に消えると爆ぜる)。
//   ・**コピー / ムーブ禁止**: backend は 1 個の長寿命オブジェクト。誤コピー
//     で COM ハンドル二重解放を避けるため最初から非コピー・非ムーブ。
//   ・**STL 不使用 / 全 noexcept**: ACS 全体方針。
//
// 範囲外 (将来フェーズで):
//   ・3D positional / spatial / HRTF (Pillar FSpatialAudio 担当)
//   ・submix bus / DSP chain / reverb
//   ・wav/ogg/mp3 デコード (今回は Pcm16 raw bytes 入力前提、Wav 形式は
//     Phase 3 で別 loader と組合せる)
//   ・streaming (大型 BGM をオンメモリせず逐次デコード)
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

// ---- FErrorCode subcode 定義 (ErrCategory::Generic 配下) ------------------
// FSteamworksBridge / FVoiceChat と同様、Generic + 安定 subcode で表現。
// 呼び出し側は `err.subcode == kSubAudioComInitFailed` 等でフィルタ可能。
inline constexpr u16 kSubAudioAlreadyInitialized = 1200;  // Init を 2 回呼んだ
inline constexpr u16 kSubAudioNotInitialized     = 1201;  // Init() 前の API 呼び出し
inline constexpr u16 kSubAudioComInitFailed      = 1202;  // CoInitializeEx 失敗
inline constexpr u16 kSubAudioCreateFailed       = 1203;  // XAudio2Create / 同等失敗
inline constexpr u16 kSubAudioMasterVoiceFailed  = 1204;  // CreateMasteringVoice 失敗
inline constexpr u16 kSubAudioInvalidArgs        = 1205;  // Init(max_voices=0) 等

// ---- フォーマット種別 -----------------------------------------------------
// PlayOneShot / PlayLooped に渡す clip がどの形式かを示す。
//   ・Pcm16     = 16bit signed PCM (典型的な WAV PCM 形式の raw bytes)
//   ・Pcm32Float = 32bit IEEE float PCM (高品質、DSP-friendly)
//   ・Wav       = ファイルからロード済の WAV 形式 (パーサ別途、Phase 3)
enum class EAudioFormat : u8 {
    Pcm16      = 0,
    Pcm32Float = 1,
    Wav        = 2,
};

// ---- clip 記述子 ----------------------------------------------------------
// 再生する音声バッファのフォーマット + データ参照。pcm_data は backend 側で
// 必要な間 (一発再生終了 / Stop まで) 内部コピーされる。
//
//   ・pcm_data     : raw PCM サンプル列 (Wav 形式の場合は RIFF ヘッダ込み)
//   ・pcm_size     : pcm_data の有効バイト数
//   ・sample_rate  : 1 チャネルあたりサンプル/秒 (e.g. 44100 / 48000)
//   ・channel_count: チャネル数 (1=mono / 2=stereo)
//   ・format       : 上記 EAudioFormat
struct AudioClipDesc {
    const void*  pcm_data      = nullptr;
    u64          pcm_size      = 0;
    u32          sample_rate   = 0;
    u32          channel_count = 0;
    EAudioFormat format        = EAudioFormat::Pcm16;
};

// ---- voice handle --------------------------------------------------------
// 内部表現: 下位 24bit = voice index、上位 8bit = generation。
// 再利用された slot を古いハンドルで参照する事故を generation で検出する。
// 全 0 (= m_Packed == 0) は無効ハンドルを意味する (kInvalidAudioVoice)。
//
// **重要な不変条件 (backend 実装側の契約)**:
//   有効ハンドルの packed 値は決して 0 になってはならない。pack は
//   `(index & 0x00FFFFFF) | (gen << 24)` なので、index==0 かつ gen==0 だと
//   packed==0 となり kInvalidAudioVoice と区別不能になる。これを避けるため、
//   **backend は generation を必ず 1 以上で配る** (slot 初期 gen=1、ラップ時は
//   0 をスキップして 1 にする)。これで最初の voice (index=0) でも packed が
//   非 0 になり、確保成功を IsValid()==false と誤判定しない。
//   この規約は FNodeId / FShapeId (24bit index + 8bit gen, gen>=1) と同一。
struct AudioVoiceHandle {
    u32 m_Packed = 0;

    constexpr AudioVoiceHandle() noexcept = default;
    constexpr AudioVoiceHandle(u32 index, u8 gen) noexcept
        : m_Packed((index & 0x00FFFFFFu) | (static_cast<u32>(gen) << 24)) {}

    bool IsValid() const noexcept { return m_Packed != 0u; }

    u32 Index() const noexcept { return m_Packed & 0x00FFFFFFu; }
    u8  Generation() const noexcept { return static_cast<u8>(m_Packed >> 24); }
};

inline constexpr AudioVoiceHandle kInvalidAudioVoice {};

// ---- 抽象 I/F -------------------------------------------------------------
// FXAudio2Backend / 将来の CoreAudioBackend / NullAudioBackend (テスト用) 等の
// 差を吸収する純粋仮想 I/F。`FAudioDirector::SetBackend(IAudioBackend*)` で
// 差し込み、FAudioDirector は backend nullptr のとき無音 (no-op) で動作する。
class IAudioBackend {
public:
    IAudioBackend() noexcept = default;
    virtual ~IAudioBackend() noexcept = default;

    IAudioBackend(const IAudioBackend&)            = delete;
    IAudioBackend& operator=(const IAudioBackend&) = delete;
    IAudioBackend(IAudioBackend&&)                 = delete;
    IAudioBackend& operator=(IAudioBackend&&)      = delete;

    // backend 初期化。max_voices は同時発音数の上限 (slot 数)。0 は不正。
    // 多重 Init は kSubAudioAlreadyInitialized エラー。
    virtual TResult<void> Init(u32 max_voices = 64) noexcept = 0;

    // 全 voice を停止して資源解放。Init 前に呼んでも安全 (no-op)。
    virtual void Shutdown() noexcept = 0;

    // Init 済かつ実バックエンドが正常起動した状態か。
    virtual bool IsInitialized() const noexcept = 0;

    // 一発再生 (loop なし、終端で自動回収)。
    //   volume: 0.0〜1.0 を想定 (実装は clamp 推奨)
    //   pitch : 1.0 = 等倍、0.5 = 1 オクターブ低い、2.0 = 1 オクターブ高い
    // 失敗時 (未 init / 空きスロットなし / fmt 不正) は kInvalidAudioVoice を返す。
    virtual AudioVoiceHandle PlayOneShot(const AudioClipDesc& clip,
                                         f32 volume,
                                         f32 pitch) noexcept = 0;

    // ループ再生 (Stop までずっと鳴り続ける)。引数は PlayOneShot と同様。
    virtual AudioVoiceHandle PlayLooped(const AudioClipDesc& clip,
                                        f32 volume,
                                        f32 pitch) noexcept = 0;

    // 指定 voice を停止し slot を解放。無効 / 既に解放済ハンドルは no-op。
    virtual void StopVoice(AudioVoiceHandle voice) noexcept = 0;

    // 指定 voice の音量を変更。無効 / 解放済は no-op。範囲外は clamp 推奨。
    virtual void SetVoiceVolume(AudioVoiceHandle voice, f32 volume) noexcept = 0;

    // 全 voice を停止して slot 解放。Init 前は no-op。
    virtual void StopAllVoices() noexcept = 0;

    // 現在再生中 (= slot active) の voice 数。デバッグ / UI メーター用。
    virtual u32 ActiveVoiceCount() const noexcept = 0;

    // 完了した一発再生 voice の slot 解放等、内部状態を進める。
    // ゲームループから毎フレーム呼ぶ。dt は実時間秒 (実装によっては使わない)。
    virtual void Tick(f32 dt) noexcept = 0;
};

} // namespace acs::game

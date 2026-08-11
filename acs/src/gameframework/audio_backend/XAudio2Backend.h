// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Array.h"
#include "foundation/Result.h"
#include "foundation/Types.h"
#include "gameframework/audio_backend/IAudioBackend.h"
#include "threading/Atomic.h"
#include "threading/RwLock.h"

namespace acs::game {

/** XAudio2 backend が事前確保を許可する voice slot 数の上限。 */
inline constexpr u32 kXAudio2BackendMaximumVoiceCount = 4096u;

/** 全再生 slot が保持できる PCM buffer 容量の合計上限。 */
inline constexpr u64 kXAudio2BackendResidentBufferBudgetBytes = 512ull * 1024ull * 1024ull;

/**
 * Win32 XAudio2 を叩いて実音声を出す IAudioBackend の Windows 実装。
 *
 * @details
 * `CAudioDirector::SetBackend(&xaudio2)` で差し込んで使う。pimpl で `<xaudio2.h>`
 * (+ COM) の重ヘッダを .cpp に閉じ込め、固定容量 voice pool を `Init(max_voices)`
 * で確保する。voice handle は ABI を 32bit に保ったプロセス通算チケットで一意化し、
 * slot 再利用時の use-after-free をハンドル不一致で no-op 化して防ぐ。一発再生は Tick で
 * `BuffersQueued == 0` を見て自然回収される。COM MTA の寿命は cookie で保持するため、
 * Init と異なるスレッドから Shutdown またはデストラクタを実行できる。
 */
class CXAudio2Backend final : public IAudioBackend {
public:
    /** 空状態で構築する (実リソースは Init で確保)。 */
    CXAudio2Backend() noexcept;

    /** Shutdown を呼んで全 voice・COM・pimpl を解放する。 */
    ~CXAudio2Backend() noexcept override;

    /** コピー禁止 (COM ハンドルの二重解放を避けるため)。 */
    CXAudio2Backend(const CXAudio2Backend&)            = delete;

    /** コピー代入も禁止。 */
    CXAudio2Backend& operator=(const CXAudio2Backend&) = delete;

    /** ムーブ禁止 (長寿命オブジェクトとして単独所有するため)。 */
    CXAudio2Backend(CXAudio2Backend&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CXAudio2Backend& operator=(CXAudio2Backend&&)      = delete;

    /**
     * COM・XAudio2 エンジン・マスタリングボイス・voice pool を確保する。
     *
     * @details
     * 多重 Init は kSubAudioAlreadyInitialized、MaxVoices=0 または
     * kXAudio2BackendMaximumVoiceCount 超過は
     * kSubAudioInvalidArgs を返す。voice pool の確保に失敗した場合は
     * kSubAudioOutOfMemory を返し、途中まで取得した OS 資源をすべて解放する。
     * @param MaxVoices 同時発音数の上限 (= slot 数。0 は不正)。
     * @return 成功なら空の TResult、初期化失敗ならエラー。
     */
    TResult<void> Init(u32 MaxVoices = 64) noexcept override;

    /** 全 voice と MTA cookie と pimpl を解放する。任意スレッドからの多重呼び出しに対応する。 */
    void         Shutdown() noexcept override;

    /**
     * Init 済かつ正常起動した状態かを返す。
     *
     * @return 初期化済みなら true。
     */
    bool         IsInitialized() const noexcept override;

    /**
     * 一発再生する (loop なし、終端で Tick が自然回収)。
     *
     * @details
     * PCM データを slot 内バッファにコピーしてから SourceVoice を再生する。未 init /
     * 空きスロットなし / 未対応フォーマット (Wav 等) の場合は kInvalidAudioVoice を返す。
     * @param Clip 再生する PCM clip 記述子 (Pcm16 / Pcm32Float のみ対応)。
     * @param Volume 音量 (0.0〜1.0、範囲外は clamp)。
     * @param Pitch 周波数比 (1.0=等倍、範囲外は [0.25, 4.0] に clamp)。
     * @return 再生中 voice のハンドル (失敗時は kInvalidAudioVoice)。
     */
    FAudioVoiceHandle PlayOneShot(const FAudioClipDesc& Clip,
                                 f32 Volume,
                                 f32 Pitch) noexcept override;

    /**
     * ループ再生する (StopVoice まで鳴り続ける)。
     *
     * @details 引数・失敗時の挙動は PlayOneShot と同様。ループ再生は Tick では回収されない。
     * @param Clip 再生する PCM clip 記述子 (Pcm16 / Pcm32Float のみ対応)。
     * @param Volume 音量 (0.0〜1.0、範囲外は clamp)。
     * @param Pitch 周波数比 (1.0=等倍、範囲外は [0.25, 4.0] に clamp)。
     * @return 再生中 voice のハンドル (失敗時は kInvalidAudioVoice)。
     */
    FAudioVoiceHandle PlayLooped(const FAudioClipDesc& Clip,
                                f32 Volume,
                                f32 Pitch) noexcept override;

    /**
     * 指定 voice を停止して slot を解放する。
     *
     * @details 無効ハンドル / 既に解放済 / ハンドル不一致 (古いハンドル) は no-op。
     * @param Voice 停止する voice のハンドル。
     */
    void StopVoice(FAudioVoiceHandle Voice) noexcept override;

    /**
     * 指定 voice の音量を変更する。
     *
     * @details 無効ハンドル / 解放済 / ハンドル不一致は no-op。範囲外は clamp。
     * @param Voice 対象 voice のハンドル。
     * @param Volume 新しい音量 (0.0〜1.0、範囲外は clamp)。
     */
    void SetVoiceVolume(FAudioVoiceHandle Voice, f32 Volume) noexcept override;

    /** 全 voice を停止して slot を解放する (Init 前は no-op)。 */
    void StopAllVoices() noexcept override;

    /**
     * 現在再生中 (= slot active) の voice 数を返す。
     *
     * @return アクティブな voice 数 (未 init なら 0)。
     */
    u32  ActiveVoiceCount() const noexcept override;

    /**
     * 内部状態を進める (毎フレーム呼ぶ)。
     *
     * @details 完了した一発再生 voice (BuffersQueued == 0) の slot を回収する。
     * @param DeltaSeconds 実時間の経過秒 (本実装では未使用)。
     */
    void Tick(f32 DeltaSeconds) noexcept override;

    /**
     * マスタリングボイス音量 (= 最終出力の master volume) を設定する。
     *
     * @details 0.0〜1.0 推奨。XAudio2 自体は > 1.0 の over-amplification も許容するが歪むので非推奨。
     * @param Volume master volume (0.0〜1.0、範囲外は clamp)。
     */
    void SetMasterVolume(f32 Volume) noexcept;

#if defined(ACS_XAUDIO2_BACKEND_TEST_HOOKS)
    /** 実デバイスなしで lifecycle と MTA cookie 契約を検証する疑似状態を作る。 */
    TResult<void> InitializeLifecycleTestState() noexcept;

    /** lifecycle 共有操作を決定的に停止させる専用テスト hook。 */
    static void ConfigureLifecycleOperationTestGate(TAtomic<u32>* Entered,
                                                    TAtomic<u32>* Release) noexcept;

    /** Shutdown 要求が新規共有操作を閉じているかを返す。 */
    bool IsShutdownRequestedForTesting() const noexcept;

    /** pimpl が保持されているかを返す。 */
    bool HasLifecycleStateForTesting() const noexcept;
#endif

    /**
     * XAudio2 / COM の重ヘッダを .cpp に閉じ込めるための pimpl。
     *
     * @details
     * public に置くのは .cpp 内の自由関数 (PlayInternal 等) から FImpl のメンバを
     * 直接触るため (acs::CAudioEngine と同じパターン)。定義は .cpp 側にある。
     */
    struct FImpl;

private:
    /** lifecycle 排他ロック取得済みで全資源を解放する。 */
    void ShutdownUnlocked() noexcept;

    /** Shutdown 要求中かを返す。 */
    bool IsShutdownRequested() const noexcept;

    /** pimpl の寿命を Init/Shutdown と共有操作間で同期する。 */
    mutable FRwLock m_LifecycleLock;

    /** 実行開始済み Shutdown 数。0 以外では新規共有操作を拒否する。 */
    TAtomic<u32> m_ShutdownRequests{0u};

    /** pimpl 本体 (Init で確保、Shutdown で解放。未 init は nullptr)。 */
    FImpl* m_Impl = nullptr;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FXAudio2Backend = CXAudio2Backend;

} // namespace acs::game

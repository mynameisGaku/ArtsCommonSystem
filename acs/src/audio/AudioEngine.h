// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/SharedPtr.h"
#include "threading/Atomic.h"
#include "threading/Mutex.h"
#include "threading/RwLock.h"
#include "container/Array.h"
#include "audio/SoundHandle.h"
#include "asset/AudioAsset.h"

namespace acs {

/** 全再生で保持できる未圧縮音声データの合計上限。 */
inline constexpr u64 kAudioEngineResidentBufferBudgetBytes = 512ull * 1024ull * 1024ull;

/**
 * XAudio2を使って音声の再生と停止を管理する。
 * 内部状態の寿命と発音枠を個別に同期し、別スレッドからのShutdownを待ち合わせる。
 */
class CAudioEngine {
public:
    /** 未初期化の音声エンジンを作る。 */
    CAudioEngine() noexcept = default;

    /** 全音声資源を解放して破棄する。 */
    ~CAudioEngine() noexcept;

    /** 音声資源の二重所有を防ぐためコピーを禁止する。 */
    CAudioEngine(const CAudioEngine&) = delete;

    /** 音声資源の二重所有を防ぐためコピー代入を禁止する。 */
    CAudioEngine& operator=(const CAudioEngine&) = delete;

    /**
     * 音声エンジンと最終出力先を初期化する。
     * @return 成功時は空の結果、二重初期化または音声APIを初期化できない場合はエラー。
     */
    TResult<void> Init() noexcept;

    /**
     * 全再生を停止し、音声エンジンが持つ資源を解放する。
     * 多重呼び出しは安全で、実行中の操作を待ってから新しい操作を拒否する。
     */
    void Shutdown() noexcept;

    /**
     * 音声アセットの再生を開始する。
     * 空き枠、音声データ、保持容量、backend操作のいずれかが失敗した場合は状態を残さない。
     * @param Asset 再生する音声データ。
     * @param Volume 開始時の音量。非有限値は0、有限値は0から1の範囲へ収める。
     * @param bLoop 繰り返し再生する場合はtrue。
     * @return 再生の識別値。開始できない場合はkInvalidSound。
     */
    FSoundHandle Play(const FAudioAsset& Asset, f32 Volume = 1.0f, bool bLoop = false) noexcept;

    /**
     * 指定した音声の再生を停止する。
     * 世代が一致しない識別値は状態を変えない。
     * @param Handle 停止する再生の識別値。
     */
    void Stop(FSoundHandle Handle) noexcept;

    /**
     * 指定した再生の音量を変更する。
     * backendが拒否した場合は以前の音量を維持して警告を記録する。
     * @param Handle 音量を変える再生の識別値。
     * @param Volume 新しい音量。非有限値は0、有限値は0から1の範囲へ収める。
     */
    void SetVolume(FSoundHandle Handle, f32 Volume) noexcept;

    /** 再生中の全音声を停止し、保持中の音声データも解放する。 */
    void StopAll() noexcept;

    /**
     * 音声エンジン全体の出力音量を変更する。
     * backendが拒否した場合は以前の音量を維持して警告を記録する。
     * @param Volume 新しい全体音量。非有限値は0、有限値は0から1の範囲へ収める。
     */
    void SetMasterVolume(f32 Volume) noexcept;

    /** 再生中の全音声を現在位置で一時停止する。 */
    void PauseAll() noexcept;

    /** 一時停止中の全音声を保持位置から再開する。 */
    void ResumeAll() noexcept;

    /**
     * 現在使用中の発音枠数を返す。
     * @return 再生に使用している枠の数。
     */
    u32 ActiveCount() const noexcept;

#if defined(ACS_AUDIO_TEST_HOOKS)
    /** OSの音声機器を使わずに状態管理を調べるテスト状態を作る。 */
    TResult<void> InitializeLifecycleTestState() noexcept;

    /**
     * OSの音声機器を使わずに公開音量操作を調べるテスト状態を作る。
     * @return 成功時は空の結果、状態を作れない場合はエラー。
     */
    TResult<void> InitializeVolumeTestState() noexcept;

    /**
     * 状態を読む処理をテスト用の合図で停止させる。
     * @param Entered 処理の開始を通知する値。
     * @param Release 処理の再開を許可する値。
     */
    static void ConfigureLifecycleOperationTestGate(TAtomic<u32>* Entered, TAtomic<u32>* Release) noexcept;

    /**
     * 以後の音量backend操作を失敗させる経路を設定する。
     * @param bPlayVolume 再生開始時の個別音量設定を失敗させる場合はtrue。
     * @param bSetVolume 再生中の個別音量設定を失敗させる場合はtrue。
     * @param bMasterVolume 全体音量設定を失敗させる場合はtrue。
     */
    void ConfigureVolumeFailuresForTesting(bool bPlayVolume, bool bSetVolume, bool bMasterVolume) noexcept;

    /**
     * 音量入力を公開経路と同じ規則で正規化する。
     * @param Volume 確認する音量。
     * @return 非有限値は0、有限値は0から1へ収めた値。
     */
    static f32 NormalizeVolumeForTesting(f32 Volume) noexcept;

    /**
     * 指定した再生のbackend音量を返す。
     * @param Handle 確認する再生の識別値。
     * @return backendが保持する音量。識別値が無効な場合は負数。
     */
    f32 VolumeForTesting(FSoundHandle Handle) const noexcept;

    /** テストbackendが保持する全体音量を返す。 */
    f32 MasterVolumeForTesting() const noexcept;

    /** テストbackendが最後に受け取った正規化済み音量を返す。 */
    f32 LastVolumeAttemptForTesting() const noexcept;

    /** テストbackendが保持している音声データ容量を返す。 */
    u64 ResidentBufferBytesForTesting() const noexcept;

    /** テストbackendが確保している発音ボイス数を返す。 */
    u32 AllocatedVoiceCountForTesting() const noexcept;

    /** 個別音量または全体音量の失敗警告件数を返す。 */
    u32 VolumeFailureWarningCountForTesting() const noexcept;

    /** Shutdown要求中かをテストへ返す。 */
    bool IsShutdownRequestedForTesting() const noexcept;

    /** 内部状態が保持されているかをテストへ返す。 */
    bool HasLifecycleStateForTesting() const noexcept;
#endif

    /** XAudio2の型を公開しないための内部状態。 */
    struct FImpl;

private:
    /** 内部状態の排他ロック取得後に全資源を解放する。 */
    void ShutdownUnlocked() noexcept;

    /** Shutdown要求中で新しい操作を拒否すべきかを返す。 */
    bool IsShutdownRequested() const noexcept;

    /** 初期化と通常操作の間で内部状態の寿命を保護する。 */
    mutable FRwLock m_LifecycleLock;

    /** 実行中のShutdown呼び出し数。 */
    TAtomic<u32> m_ShutdownRequests{0};

    /** 音声エンジンの内部状態。未初期化時はnullptr。 */
    FImpl* m_Impl = nullptr;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FAudioEngine = CAudioEngine;

} // namespace acs

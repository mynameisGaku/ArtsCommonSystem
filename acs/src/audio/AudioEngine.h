// SPDX-License-Identifier: Apache-2.0
// XAudio2 ベースの音声エンジン
//
// 使い方:
//   FAudioEngine engine;
//   engine.Init();
//
//   // FAudioAsset (wav/mp3/flac/ogg) を Asset モジュールから取得
//   TSharedPtr<Asset> asset = registry.Load(L"sound/bgm.ogg").Value();
//   auto* audio = static_cast<FAudioAsset*>(asset.Get());
//
//   // 再生 (volume 0..1, loop は繰り返し)
//   SoundHandle h = engine.Play(*audio, 1.0f, /*loop=*/true);
//
//   // 停止 / 音量変更
//   engine.SetVolume(h, 0.5f);
//   engine.Stop(h);
//
//   engine.Shutdown();
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

/** FAudioEngine が全再生 slot に保持できる PCM buffer 容量の合計上限。 */
inline constexpr u64 kAudioEngineResidentBufferBudgetBytes = 512ull * 1024ull * 1024ull;

/**
 * XAudio2 を裏に持つ音声再生エンジン。
 *
 * @details
 * Init() で COM とマスタリングボイスを立ち上げ、Play() で FAudioAsset ごとに 1 つの
 * ソースボイス (発音スロット) を確保して再生する。同時発音は最大 64 スロットで、
 * 一発再生のボイスはバッファが流れ切ると自動回収され、ループ再生は Stop されるまで残る。
 * 各再生は世代付きの SoundHandle で識別され、スロット再利用後の古いハンドルは無効になる。
 * 内部状態は XAudio2 ヘッダを公開しないよう pimpl で隠蔽する。pimpl の寿命は外側の
 * reader/writer lock、スロットは内側の mutex で保護する。
 * non-copy 型。
 */
class FAudioEngine {
public:
    /** 未初期化状態で構築する (XAudio2 は Init で立ち上げ)。 */
    FAudioEngine() noexcept = default;

    /** 破棄する (Shutdown を呼んで全ボイスと XAudio2 を解放)。 */
    ~FAudioEngine() noexcept;

    /** コピー禁止 (XAudio2 リソースを単独所有するため)。 */
    FAudioEngine(const FAudioEngine&) = delete;

    /** コピー代入も禁止。 */
    FAudioEngine& operator=(const FAudioEngine&) = delete;

    /**
     * COM MTA 利用参照とマスタリングボイスを含む XAudio2 エンジンを初期化する。
     *
     * @details
     * CoIncrementMTAUsage の cookie で MTA の寿命を保持するため、Shutdown と
     * デストラクタは Init と異なるスレッドからでも安全に実行できる。
     * @return 成功なら空の TResult、二重初期化や XAudio2 生成失敗ならエラー。
     */
    TResult<void> Init() noexcept;

    /**
     * 全ボイスを停止・解放し、XAudio2 と COM MTA 利用参照を後始末する。
     *
     * @details 多重呼び出しは安全。実行中の共有操作が完了するまで待ち、新規操作を拒否してから
     * 全状態を解放する。Init と異なるスレッドから呼んでもよい。
     */
    void Shutdown() noexcept;

    /**
     * アセットを 1 つのソースボイスで再生開始する。
     *
     * @details
     * 空きスロットを確保し、サンプルデータを再生中保持用にコピーしてソースボイスを生成・
     * 再生する。空きスロットが無い、アセットが空、または常駐 PCM 予算を超える場合は
     * 無効ハンドルを返す。
     * @param Asset 再生する音声アセット (wav/mp3/flac/ogg)。
     * @param Volume 初期音量 (0.0..1.0、範囲外は内部でクランプ、既定 1.0)。
     * @param bLoop true なら無限ループ再生 (既定 false の一発再生)。
     * @return この再生を指す SoundHandle (失敗時は kInvalidSound)。
     */
    SoundHandle Play(const FAudioAsset& Asset, f32 Volume = 1.0f, bool bLoop = false) noexcept;

    /**
     * 指定ハンドルの再生を停止し、内部スロットを解放する。
     *
     * @details 世代が一致しない (= 既に解放済みの) ハンドルは無視する。
     * @param Handle 停止する再生のハンドル。
     */
    void Stop(SoundHandle Handle) noexcept;

    /**
     * 指定ハンドルの再生音量を変更する。
     *
     * @details 世代不一致のハンドルは無視する。値は 0.0..1.0 に内部クランプする。
     * @param Handle 対象の再生ハンドル。
     * @param Volume 新しい音量 (0.0..1.0)。
     */
    void SetVolume(SoundHandle Handle, f32 Volume) noexcept;

    /** 再生中の全ボイスを停止し、全スロットを解放する。 */
    void StopAll() noexcept;

    /**
     * 最終出力のマスター音量を変更する。
     *
     * @param Volume マスター音量 (0.0..1.0、範囲外は内部でクランプ)。
     */
    void SetMasterVolume(f32 Volume) noexcept;

    /** 再生中の全ボイスを一時停止する (再生位置は保持される)。 */
    void PauseAll() noexcept;

    /** PauseAll で止めた全ボイスを保持位置から再開する。 */
    void ResumeAll() noexcept;

    /**
     * 現在使用中の発音スロット数を返す (デバッグ用)。
     *
     * @return アクティブなボイススロット数。
     */
    u32 ActiveCount() const noexcept;

#if defined(ACS_AUDIO_TEST_HOOKS)
    /** OS の音声デバイスに依存せず lifecycle/MTA cookie 契約を検証するテスト状態を作る。 */
    TResult<void> InitializeLifecycleTestState() noexcept;

    /** lifecycle 共有操作を決定的に停止させるユニットテスト専用 hook。 */
    static void ConfigureLifecycleOperationTestGate(TAtomic<u32>* Entered,
                                                    TAtomic<u32>* Release) noexcept;

    /** Shutdown 要求が公開操作を閉じているかを返すテスト専用 query。 */
    bool IsShutdownRequestedForTesting() const noexcept;

    /** lifecycle 状態が保持されているかを返すテスト専用 query。 */
    bool HasLifecycleStateForTesting() const noexcept;
#endif

    /** XAudio2 ヘッダを公開しないための pimpl 実装型 (前方宣言のみ)。 */
    struct Impl;

private:
    /** lifecycle 排他ロック取得済みで全リソースを解放する。 */
    void ShutdownUnlocked() noexcept;

    /** Shutdown 要求中で新規共有操作を拒否すべきかを返す。 */
    bool IsShutdownRequested() const noexcept;

    /** Init/Shutdown と通常操作間で pimpl の寿命を同期する。 */
    mutable RwLock m_LifecycleLock;

    /** 実行開始済みの Shutdown 呼び出し数。0 以外では新規操作を拒否する。 */
    TAtomic<u32> m_ShutdownRequests{0};

    /** pimpl 実装の所有ポインタ (未初期化時は nullptr)。 */
    Impl* m_Impl = nullptr;
};

} // namespace acs

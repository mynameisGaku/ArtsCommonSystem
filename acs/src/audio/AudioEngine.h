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
#include "threading/Mutex.h"
#include "container/Array.h"
#include "audio/SoundHandle.h"
#include "asset/AudioAsset.h"

namespace acs {

/**
 * XAudio2 を裏に持つ音声再生エンジン。
 *
 * @details
 * Init() で COM とマスタリングボイスを立ち上げ、Play() で FAudioAsset ごとに 1 つの
 * ソースボイス (発音スロット) を確保して再生する。同時発音は最大 64 スロットで、
 * 一発再生のボイスはバッファが流れ切ると自動回収され、ループ再生は Stop されるまで残る。
 * 各再生は世代付きの SoundHandle で識別され、スロット再利用後の古いハンドルは無効になる。
 * 内部状態は XAudio2 ヘッダを公開しないよう pimpl で隠蔽し、スロットは mutex で保護する。
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
     * COM とマスタリングボイスを含む XAudio2 エンジンを初期化する。
     *
     * @details
     * COM の参照数を正しく釣り合わせるため、Shutdown とデストラクタは Init を
     * 呼んだスレッドで実行する。
     * @return 成功なら空の TResult、二重初期化や XAudio2 生成失敗ならエラー。
     */
    TResult<void> Init() noexcept;

    /** 全ボイスを停止・解放し、XAudio2 と COM を後始末する (多重呼び出し安全、Init と同一スレッド必須)。 */
    void Shutdown() noexcept;

    /**
     * アセットを 1 つのソースボイスで再生開始する。
     *
     * @details
     * 空きスロットを確保し、サンプルデータを再生中保持用にコピーしてソースボイスを生成・
     * 再生する。空きスロットが無い・アセットが空の場合は無効ハンドルを返す。
     * @param asset 再生する音声アセット (wav/mp3/flac/ogg)。
     * @param volume 初期音量 (0.0..1.0、範囲外は内部でクランプ、既定 1.0)。
     * @param loop true なら無限ループ再生 (既定 false の一発再生)。
     * @return この再生を指す SoundHandle (失敗時は kInvalidSound)。
     */
    SoundHandle Play(const FAudioAsset& asset, f32 volume = 1.0f, bool loop = false) noexcept;

    /**
     * 指定ハンドルの再生を停止し、内部スロットを解放する。
     *
     * @details 世代が一致しない (= 既に解放済みの) ハンドルは無視する。
     * @param h 停止する再生のハンドル。
     */
    void Stop(SoundHandle h) noexcept;

    /**
     * 指定ハンドルの再生音量を変更する。
     *
     * @details 世代不一致のハンドルは無視する。値は 0.0..1.0 に内部クランプする。
     * @param h 対象の再生ハンドル。
     * @param volume 新しい音量 (0.0..1.0)。
     */
    void SetVolume(SoundHandle h, f32 volume) noexcept;

    /** 再生中の全ボイスを停止し、全スロットを解放する。 */
    void StopAll() noexcept;

    /**
     * 最終出力のマスター音量を変更する。
     *
     * @param volume マスター音量 (0.0..1.0、範囲外は内部でクランプ)。
     */
    void SetMasterVolume(f32 volume) noexcept;

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

    /** XAudio2 ヘッダを公開しないための pimpl 実装型 (前方宣言のみ)。 */
    struct Impl;

private:
    /** pimpl 実装の所有ポインタ (未初期化時は nullptr)。 */
    Impl* m_Impl = nullptr;
};

} // namespace acs

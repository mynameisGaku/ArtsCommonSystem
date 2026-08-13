// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/Forward.h"
#include "gameframework/audio_backend/IAudioBackend.h"

namespace acs { class CAssetRegistry; }

namespace acs::game {

class CSpatialAudio;

/**
 * シーン跨ぎで生存する音声指揮層 (BGM クロスフェード / SFX / ダッキング)。
 *
 * @details
 * 3 段ボリュームバス (Master / Bgm / Sfx)、BGM クロスフェード、SFX one-shot ring、
 * ダッキングを state machine で持つ。SetBackend で IAudioBackend を差すと実音再生に
 * delegate し、backend == nullptr のときは state-only (無音) で動く。name → clip 解決は
 * SetAssetRegistry した registry を通じて行う。CGame (or app) に持たせ、Tick(dt) で
 * 内部タイマを進行させる。
 */
class CAudioDirector {
public:
    /** SFX one-shot の最大同時発音数 (超過時は最古を上書き)。 */
    static constexpr u32 kMaxSfxVoices = 32;

    /** ダッキングの fade-out / fade-in 固定窓 (秒)。 */
    static constexpr f32 kDuckFadeWindow = 0.1f;

    /** SFX ring を固定容量で予約して構築する。 */
    CAudioDirector() noexcept;

    /** 破棄する (backend / registry は非所有なので解放しない)。 */
    ~CAudioDirector() noexcept = default;

    /** コピー禁止 (内部 state を単独所有するため)。 */
    CAudioDirector(const CAudioDirector&)            = delete;

    /** コピー代入も禁止。 */
    CAudioDirector& operator=(const CAudioDirector&) = delete;

    /**
     * Master ボリュームを設定する。
     *
     * @param v 設定する音量 [0, 1] (範囲外は警告 + clamp して受理)。
     */
    void SetMasterVolume(f32 v) noexcept;

    /**
     * BGM バスのボリュームを設定する。
     *
     * @param v 設定する音量 [0, 1] (範囲外は警告 + clamp して受理)。
     */
    void SetBgmVolume(f32 v) noexcept;

    /**
     * SFX バスのボリュームを設定する。
     *
     * @param v 設定する音量 [0, 1] (範囲外は警告 + clamp して受理)。
     */
    void SetSfxVolume(f32 v) noexcept;

    /**
     * Master ボリュームを返す。
     *
     * @return 現在の Master 音量 [0, 1]。
     */
    f32  GetMasterVolume() const noexcept { return m_MasterVolume; }

    /**
     * BGM バスのボリュームを返す。
     *
     * @return 現在の BGM 音量 [0, 1]。
     */
    f32  GetBgmVolume()    const noexcept { return m_BgmVolume; }

    /**
     * SFX バスのボリュームを返す。
     *
     * @return 現在の SFX 音量 [0, 1]。
     */
    f32  GetSfxVolume()    const noexcept { return m_SfxVolume; }

    /**
     * BGM をクロスフェードで再生する。
     *
     * @details
     * name が現行 BGM と同じなら no-op (継続)。既に遷移中なら新規 BGM へ再遷移する
     * (current は強制停止)。backend + registry があれば name を実ロードして実音再生、
     * 無ければ state-only。
     * @param name 再生する BGM 名 (= asset path、所有しない literal 前提)。
     * @param fade_in_sec フェードイン秒 (非有限、<= 0、または変化率を有限にできない値で即時切替)。
     * @param loop ループ再生するか。
     */
    void PlayBgm(const char* name, f32 fade_in_sec = 1.0f, bool loop = true) noexcept;

    /**
     * 再生中の BGM を fade out して停止する。
     *
     * @param fade_out_sec フェードアウト秒 (非有限、<= 0、または変化率を有限にできない値で即時停止)。
     */
    void StopBgm(f32 fade_out_sec = 0.5f) noexcept;

    /**
     * 現在再生中 (current slot) の BGM 名を返す。
     *
     * @return current BGM 名 (再生していなければ nullptr)。
     */
    const char* CurrentBgmName() const noexcept { return m_Bgm[0].name; }

    /**
     * SFX one-shot を再生する。
     *
     * @details ring 満杯なら最古を上書きする (ゲームプレイ妨害なし)。backend + registry が
     * あれば実音再生、無ければ state ring に積む。
     * @param name 再生する SFX 名 (= asset path、所有しない literal 前提)。
     * @param volume_scale この one-shot の追加ゲイン [0, ~] (非有限または 0.0 以下で no-op)。
     */
    void PlaySfx(const char* name, f32 volume_scale = 1.0f) noexcept;

    /**
     * 短期間だけ BGM 音量を一時的に下げる (ダッキング)。
     *
     * @details 前後 kDuckFadeWindow 秒で線形 fade in/out が掛かる。新たな Duck() で既存
     * state を上書きする (スタックしない)。
     * @param duration_sec 谷の幅 (非有限または <= 0 は無視)。
     * @param depth 谷底ゲイン 0.0 (完全消音) ～ 1.0 (抑制なし)。例: 0.3 で 30% に下げる。
     */
    void Duck(f32 duration_sec, f32 depth) noexcept;

    /**
     * 全 BGM / SFX を一時停止する。
     *
     * @details Tick での timer 進行も止まる (復帰時にクロスフェードがそのまま続きから再開)。
     */
    void Pause() noexcept;

    /** 一時停止を解除する。 */
    void Resume() noexcept;

    /** 全 BGM / SFX を停止して state をリセットする (volume バスは保持)。 */
    void StopAll() noexcept;

    /**
     * 一時停止中かを返す。
     *
     * @return Pause 中なら true。
     */
    bool IsPaused() const noexcept { return m_Paused; }

    /**
     * 毎フレーム呼んで内部タイマ (クロスフェード / ダッキング) を進行させる。
     *
     * @details Pause 中は dt を消費しない (state 凍結)。非有限または負の dt は 0 として扱う。
     * @param dt 前フレームからの経過秒。
     */
    void Tick(f32 dt) noexcept;

    /**
     * backend に流す合成済み BGM ボリュームを返す。
     *
     * @return master * bgm * duck_envelope * bgm_mix (Pause 中は 0)。
     */
    f32 EffectiveBgmVolume() const noexcept;

    /**
     * backend に流す合成済み SFX ボリュームを返す。
     *
     * @return master * sfx (Pause 中は 0)。
     */
    f32 EffectiveSfxVolume() const noexcept;

    /**
     * concrete backend (CXAudio2Backend 等) を差し込む。
     *
     * @details
     * nullptr で切断。本層は raw ptr を入れ替えるだけで、既存 BGM/SFX voice を停止しない。
     * backend は本層からの最終利用まで生存させる。切断または破棄時は scene が保持する voice を
     * StopVoice で停止し、残る voice を StopAllVoices で停止してから SetBackend(nullptr) を呼び、
     * その後に backend を Shutdown / 破棄する。破棄済み backend を指す状態にしてはならない。
     * @param backend 差し込む backend (非所有、nullptr で切断)。
     */
    void           SetBackend(IAudioBackend* backend) noexcept { m_Backend = backend; }

    /**
     * 現在の backend を返す。
     *
     * @return 設定済み backend (未設定なら nullptr)。
     */
    IAudioBackend* GetBackend() const noexcept { return m_Backend; }

    /**
     * name → FAudioClipDesc 解決に使う asset registry を差し込む。
     *
     * @details
     * app 所有の registry (CApplication::GetAssets()) を差すと PlayBgm/PlaySfx の name
     * (= asset path) を実ロードして実音再生する。registry または backend 未設定のときは
     * 従来の state-only (無音) で動作する。
     * @param registry 差し込む asset レジストリ (非所有)。
     */
    void            SetAssetRegistry(CAssetRegistry* registry) noexcept { m_Registry = registry; }

    /**
     * 現在の asset registry を返す。
     *
     * @return 設定済み registry (未設定なら nullptr)。
     */
    CAssetRegistry* GetAssetRegistry() const noexcept { return m_Registry; }

    /**
     * raw PCM の FAudioClipDesc を BGM として直接再生する。
     *
     * @details
     * 既存 BGM voice を fade out して停止 → 新 BGM voice を fade in する (本層 state machine
     * が EffectiveBgmVolume を毎フレ backend->SetVoiceVolume に反映するので初期 volume=0)。
     * loop=false の使用は稀 (BGM は基本 loop) で stinger 演出用に許可する。
     * @param clip 再生する PCM クリップ記述子。
     * @param fade_in_sec フェードイン秒 (非有限、<= 0、または変化率を有限にできない値で即時切替)。
     * @param loop ループ再生するか。
     * @return 再生 voice ハンドル (backend 未設定 / 不正 clip は kInvalidAudioVoice)。
     */
    FAudioVoiceHandle PlayBgmClip(const FAudioClipDesc& clip,
                                 f32 fade_in_sec = 1.0f,
                                 bool loop = true) noexcept;

    /**
     * raw PCM の FAudioClipDesc を SFX one-shot として直接再生する。
     *
     * @details volume は EffectiveSfxVolume * volume_scale を [0, 1] に制限して合成する (duck は掛けない)。
     * @param clip 再生する PCM クリップ記述子。
     * @param volume_scale この one-shot の追加ゲイン [0, ~] (非有限または 0.0 以下で no-op)。
     * @param pitch 再生ピッチ。非有限値は 1.0、範囲外は [0.25, 4] に制限する。
     * @return 再生 voice ハンドル (backend 未設定 / 不正 clip は kInvalidAudioVoice)。
     */
    FAudioVoiceHandle PlaySfxClip(const FAudioClipDesc& clip,
                                 f32 volume_scale = 1.0f,
                                 f32 pitch = 1.0f) noexcept;

    /**
     * 再生中 SFX voice へ 3D source の距離減衰、左右 pan、pitch を反映する。
     *
     * @details
     * `EffectiveSfxVolume() * spatial.ComputeAttenuatedVolume(source_id)` を音量として
     * backend へ一回だけ渡す。Pause 中も呼び続けると音量 0 を反映し、Resume 後の
     * 次回呼び出しで復元する。本関数は spatial を保持しない。scene 終了時は backend の
     * StopVoice を先に呼び、その後に
     * RemoveSource / Clear / scene 破棄を行う。backend 未設定、無効 voice、削除済 source は
     * no-op で、source の削除だけでは voice を自動停止しない。特に looped voice は source を
     * 先に削除すると、最後に反映した parameter のまま StopVoice まで再生が続き得る。
     * valid-looking な解放済み voice は backend 側で安全な no-op として扱う。
     * CSpatialAudio の登録・更新・削除と本関数の読出しは scene/update thread で直列化する。
     * backend 呼び出し以降だけを backend 内部で同期し、本関数は thread-safe ではない。
     * @param voice `PlaySfxClip` 等で開始した SFX voice。
     * @param spatial listener と 3D source を保持する scene 局所状態。
     * @param source_id `spatial` が払い出した source ID。
     * @param pitch 再生比率。非有限値は 1、範囲外は [0.25, 4] に制限する。
     */
    void UpdateSpatialSfxVoice(FAudioVoiceHandle voice,
                               const CSpatialAudio& spatial,
                               u32 source_id,
                               f32 pitch = 1.0f) noexcept;

private:
    /** BGM スロット 1 本の state (m_Bgm[0] = current、m_Bgm[1] = 遷移中の new)。 */
    struct FBgmSlot {
        /** BGM 名 (所有しない、literal 前提)。 */
        const char*      name         = nullptr;

        /** 現在の slot ゲイン [0, 1]。 */
        f32              gain         = 0.0f;

        /** 目標ゲイン (= 0 で fade out 中)。 */
        f32              target       = 0.0f;

        /** 1 秒あたりのゲイン変化量 (|target - gain| / fade_duration)。 */
        f32              fade_per_sec = 0.0f;

        /** ループ再生するか。 */
        bool             loop         = true;

        /** スロットが使用中か。 */
        bool             active       = false;

        /** backend voice ハンドル (clip 直接再生時のみ valid、名前ベースは kInvalidAudioVoice)。 */
        FAudioVoiceHandle voice        = {};
    };

    /** SFX one-shot ring の 1 エントリ。 */
    struct FSfxEntry {
        /** SFX 名 (所有しない、literal 前提)。 */
        const char* name         = nullptr;

        /** この one-shot の追加ゲイン。 */
        f32         volume_scale = 1.0f;

        /** エントリが使用中か (false なら slot 空き)。 */
        bool        active       = false;
    };

    /**
     * 値を [0, 1] にクランプする。
     *
     * @param v クランプ対象。
     * @return [0, 1] に収めた値。
     */
    static f32 Clamp01(f32 v) noexcept;

    /**
     * 現在のダッキング timer から duck envelope を計算する。
     *
     * @return BGM に掛ける 0..1 のゲイン係数 (非アクティブ時は 1.0)。
     */
    f32  ComputeDuckEnvelope() const noexcept;

    /** Master ボリューム [0, 1]。 */
    f32 m_MasterVolume = 1.0f;

    /** BGM バスのボリューム [0, 1]。 */
    f32 m_BgmVolume    = 1.0f;

    /** SFX バスのボリューム [0, 1]。 */
    f32 m_SfxVolume    = 1.0f;

    /** BGM クロスフェード state ([0]=current、[1]=遷移中の new)。 */
    FBgmSlot m_Bgm[2] {};

    /** SFX one-shot ring (容量 kMaxSfxVoices で予約)。 */
    TArray<FSfxEntry> m_Sfx;

    /** ring 上書き時の次書込先 (FIFO)。 */
    u32             m_SfxHead = 0;

    /** ダッキングがアクティブか。 */
    bool m_bDuckActive        = false;

    /** ダッキングの谷底ゲイン [0, 1]。 */
    f32  m_DuckDepth         = 1.0f;

    /** ダッキングの残り時間 (fade in + hold + fade out)。 */
    f32  m_DuckRemaining     = 0.0f;

    /** ダッキングの全体長 (= duration + 2 * kDuckFadeWindow)。 */
    f32  m_DuckTotal         = 0.0f;

    /** 一時停止中フラグ。 */
    bool m_Paused = false;

    /**
     * 実音再生先の backend (非所有 raw ptr)。
     *
     * @details
     * nullptr 時は state-only 動作 (無音)。CXAudio2Backend 等を呼び出し側で所有し
     * SetBackend で差し込む。Pause/Resume/StopAll/Tick/SetMasterVolume を本層から forward する。
     */
    IAudioBackend* m_Backend = nullptr;

    /** name → FAudioClipDesc 解決用の asset registry (非所有 raw ptr)。 */
    CAssetRegistry* m_Registry = nullptr;
};

} // namespace acs::game

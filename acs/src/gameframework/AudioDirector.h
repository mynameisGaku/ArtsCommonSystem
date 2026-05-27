// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — FAudioDirector (Phase 2: 実 backend 接続)
//
// シーン跨ぎで生存する「音声指揮層」。FSceneServices ではなく FGame (or app)
// に持たせる前提 (BGM はシーン切替で途切れないため、シーン局所では困る)。
//
// 機能:
//   ・3 段ボリュームバス: Master / Bgm / Sfx (各 f32 [0, 1])
//   ・BGM クロスフェード: `PlayBgm("battle", 2.0f, true)` で 2 秒掛けて遷移
//   ・SFX one-shot: `PlaySfx("hit", 0.8f)` で即時再生 (混ぜて並列発火)
//   ・ダッキング: 短期間だけ BGM 音量を一時的に下げる (ボイス再生中など)
//   ・Pause / Resume / StopAll
//   ・Tick(dt) で内部 timer (クロスフェード / ダッキング) を進行
//   ・**IAudioBackend 接続 (Phase 2 で追加)**: `SetBackend(IAudioBackend*)` で
//     `FXAudio2Backend` 等の concrete backend を差し込むと、実音再生 + master /
//     pause / stop / Tick を backend に delegate する。backend == nullptr のとき
//     は従来通り state-only 動作 (= 無音、ログ警告も出さない)。
//   ・**clip 直接再生 API (Phase 2 で追加)**: `PlayBgmClip(const AudioClipDesc&,
//     fade, loop)` / `PlaySfxClip(const AudioClipDesc&, vol, pitch)` で raw PCM
//     データから直接再生できる。名前ベース API (PlayBgm / PlaySfx) は state 管理
//     のみ (将来 name→clip resolver を導入予定)。
//
// 設計選択:
//   ・**TResult<void> は使わない**: この層は失敗を返さない (ログ警告のみ)。
//     不正引数 (null name / 負の duration 等) は警告ログ + 既定値で続行。
//   ・**SoA cross-fade state**: 同時に鳴る BGM は最大 2 本 (current + next)。
//     `m_Bgm[0]` が現行、`m_Bgm[1]` が遷移中の新 BGM。遷移完了で swap。
//   ・**Ducking は単一 timer**: スタックしない (新 Duck が来たら上書き)。
//     fade-in/fade-out は単純な線形 (depth → 1.0 まで 0.1 秒で復帰の固定窓)。
//   ・**SFX は ring 風に固定容量**: 容量 32 (典型的同時発音数を踏まえた目安)。
//     満杯なら最古を上書き (シューティング的に許容)。
//   ・**name は所有しない**: `const char*` を保持 = ROM の文字列リテラル前提。
//     Phase 3 で FStringView / Asset Handle に置き換える。
//   ・**backend は所有しない**: `IAudioBackend*` は raw ptr。呼び出し側が
//     `FXAudio2Backend` 等を所有し、SetBackend(nullptr) で先に切ってから
//     backend の Shutdown を呼ぶ責任を負う (二重解放回避)。
//   ・**Pause/Resume/StopAll/SetMasterVolume は backend に forward**: backend
//     が存在すれば実音にも反映される。volume バス変更 (SetBgmVolume 等) は
//     Tick() で BGM voice の `SetVoiceVolume` に反映する (EffectiveBgmVolume
//     を毎フレ算出して backend へ流す)。
//
// 範囲外 (Phase 3+ で):
//   ・name → AudioClipDesc resolver (FAssetRegistry 統合)
//   ・3D positional / spatial / submix bus / DSP chain
//   ・スナップショット (mixer state を hot-swap)
//   ・wav/ogg/mp3 decode (本層は raw PCM 前提)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "gameframework/audio_backend/IAudioBackend.h"

namespace acs::game {

class FAudioDirector {
public:
    // SFX one-shot 最大同時発音数。超過時は最古を上書き。
    static constexpr u32 kMaxSfxVoices = 32;
    // ダッキング fade-out / fade-in の固定窓 (秒)。
    static constexpr f32 kDuckFadeWindow = 0.1f;

    FAudioDirector() noexcept;
    ~FAudioDirector() noexcept = default;

    FAudioDirector(const FAudioDirector&)            = delete;
    FAudioDirector& operator=(const FAudioDirector&) = delete;

    // ----- ボリュームバス -----
    // [0, 1] にクランプ。範囲外は警告 + clamp して受理。
    void SetMasterVolume(f32 v) noexcept;
    void SetBgmVolume(f32 v) noexcept;
    void SetSfxVolume(f32 v) noexcept;
    f32  GetMasterVolume() const noexcept { return m_MasterVolume; }
    f32  GetBgmVolume()    const noexcept { return m_BgmVolume; }
    f32  GetSfxVolume()    const noexcept { return m_SfxVolume; }

    // ----- BGM クロスフェード -----
    // name が同じ場合は no-op (現行 BGM 継続)。fade_in_sec <= 0 で即時切替。
    // 既に遷移中なら新規 BGM へ再遷移 (current は強制停止)。
    void PlayBgm(const char* name, f32 fade_in_sec = 1.0f, bool loop = true) noexcept;
    // BGM を fade out して停止。fade_out_sec <= 0 で即時停止。
    void StopBgm(f32 fade_out_sec = 0.5f) noexcept;
    const char* CurrentBgmName() const noexcept { return m_Bgm[0].name; }

    // ----- SFX one-shot -----
    // volume_scale: この one-shot の追加ゲイン [0, ~]。0.0 で no-op。
    // ring 満杯なら最古を上書き (シューティング的にゲームプレイ妨害なし)。
    void PlaySfx(const char* name, f32 volume_scale = 1.0f) noexcept;

    // ----- ダッキング -----
    // 短期間 BGM 音量を一時的に下げる。
    //   duration_sec: 谷の幅 (この時間だけ depth で抑える)
    //   depth: 0.0 (完全消音) ～ 1.0 (抑制なし)。例: 0.3 で 30% に下げる。
    // 前後 kDuckFadeWindow 秒で線形 fade in/out が掛かる (ガラ無し)。
    // 新たな Duck() で既存 state を上書き (スタックしない)。
    void Duck(f32 duration_sec, f32 depth) noexcept;

    // ----- グローバル制御 -----
    // 全 BGM / SFX を一時停止。Tick での timer 進行も止まる (= 復帰時に
    // クロスフェードがそのまま続きから再開)。
    void Pause() noexcept;
    void Resume() noexcept;
    // 全 BGM / SFX を停止して state リセット (volume バスは保持)。
    void StopAll() noexcept;
    bool IsPaused() const noexcept { return m_Paused; }

    // ----- driver (FGame / FSceneManager から毎フレーム呼ぶ) -----
    // Pause 中は dt を消費しない (state 凍結)。
    void Tick(f32 dt) noexcept;

    // ----- 派生情報 (debug / backend に流す合成済みボリューム) -----
    // 実際に backend に流す合成済みボリューム。
    //   master * bgm * duck_envelope  (BGM 系)
    //   master * sfx                  (SFX 系)
    f32 EffectiveBgmVolume() const noexcept;
    f32 EffectiveSfxVolume() const noexcept;

    // ----- backend 接続 (Phase 2 で追加) -----
    // concrete backend (FXAudio2Backend 等) を差し込む。nullptr で切断。
    // 切断時に既存 BGM/SFX voice は backend->StopAllVoices で停止する責任は
    // 呼び出し側に委ねる (本層は raw ptr 入替のみ)。
    void           SetBackend(IAudioBackend* backend) noexcept { m_Backend = backend; }
    IAudioBackend* GetBackend() const noexcept { return m_Backend; }

    // ----- clip 直接再生 (Phase 2 で追加) -----
    // backend が設定されていない / 不正 clip のとき: kInvalidAudioVoice を返す
    // (no-op、警告は backend 側で出す)。
    //
    // BGM clip 再生: 既存 BGM voice を fade out して停止 → 新 BGM voice を
    //                fade in (本層 state machine が EffectiveBgmVolume を毎フレ
    //                backend->SetVoiceVolume に反映するので、初期 volume=0)。
    // loop=false の使用は稀 (BGM は基本 loop)、stinger 演出用に許可。
    AudioVoiceHandle PlayBgmClip(const AudioClipDesc& clip,
                                 f32 fade_in_sec = 1.0f,
                                 bool loop = true) noexcept;
    // SFX one-shot 再生: backend に PlayOneShot を投げる。volume は
    // EffectiveSfxVolume * volume_scale で合成済。pitch=1.0 が等倍。
    AudioVoiceHandle PlaySfxClip(const AudioClipDesc& clip,
                                 f32 volume_scale = 1.0f,
                                 f32 pitch = 1.0f) noexcept;

private:
    // BGM スロット 1 本の state。`m_Bgm[0]` = current、`m_Bgm[1]` = 遷移中の new。
    struct FBgmSlot {
        const char*      name         = nullptr;   // 所有しない (literal 前提)
        f32              gain         = 0.0f;      // 現在の slot ゲイン [0, 1]
        f32              target       = 0.0f;      // 目標ゲイン (= 0 で fade out 中)
        f32              fade_per_sec = 0.0f;      // |target - gain| / fade_duration
        bool             loop         = true;
        bool             active       = false;
        // Phase 2 追加: backend voice handle。clip 直接再生時のみ valid。
        // 名前ベース PlayBgm のみで遷移したスロットは kInvalidAudioVoice のまま。
        AudioVoiceHandle voice        = {};
    };

    // SFX one-shot ring エントリ。
    struct SfxEntry {
        const char* name         = nullptr;
        f32         volume_scale = 1.0f;
        bool        active       = false;  // false なら slot 空き
    };

    // ----- 内部ヘルパ -----
    static f32 Clamp01(f32 v) noexcept;
    static void LogTodoOnce(const char* what) noexcept;
    // duck envelope (= BGM に掛ける 0..1 のゲイン係数) を現在 timer から計算。
    f32  ComputeDuckEnvelope() const noexcept;

    // ----- volume バス -----
    f32 m_MasterVolume = 1.0f;
    f32 m_BgmVolume    = 1.0f;
    f32 m_SfxVolume    = 1.0f;

    // ----- BGM クロスフェード state -----
    FBgmSlot m_Bgm[2] {};

    // ----- SFX ring (固定容量) -----
    TArray<SfxEntry> m_Sfx;   // 容量 kMaxSfxVoices で reserve、サイズも同じく予約
    u32             m_SfxHead = 0;  // 上書き時の次書込先 (FIFO)

    // ----- Duck state -----
    bool m_DuckActive        = false;
    f32  m_DuckDepth         = 1.0f;  // 谷底ゲイン [0, 1]
    f32  m_DuckRemaining     = 0.0f;  // 全体残り (fade in + hold + fade out)
    f32  m_DuckTotal         = 0.0f;  // 全体長 (= duration + 2 * kDuckFadeWindow)

    // ----- 全体制御 -----
    bool m_Paused = false;

    // ----- backend (Phase 2 で追加、非所有 raw ptr) -----
    // nullptr 時は state-only 動作 (無音)。`FXAudio2Backend` 等を呼び出し側で
    // 所有し、SetBackend で差し込む。Pause/Resume/StopAll/Tick/SetMasterVolume
    // を本層から forward する。
    IAudioBackend* m_Backend = nullptr;
};

} // namespace acs::game

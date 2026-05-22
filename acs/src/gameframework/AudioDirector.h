// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — AudioDirector (Phase 1: bridge スケルトン)
//
// シーン跨ぎで生存する「音声指揮層」。SceneServices ではなく Game (or app)
// に持たせる前提 (BGM はシーン切替で途切れないため、シーン局所では困る)。
//
// 機能:
//   ・3 段ボリュームバス: Master / Bgm / Sfx (各 f32 [0, 1])
//   ・BGM クロスフェード: `PlayBgm("battle", 2.0f, true)` で 2 秒掛けて遷移
//   ・SFX one-shot: `PlaySfx("hit", 0.8f)` で即時再生 (混ぜて並列発火)
//   ・ダッキング: 短期間だけ BGM 音量を一時的に下げる (ボイス再生中など)
//   ・Pause / Resume / StopAll
//   ・Tick(dt) で内部 timer (クロスフェード / ダッキング) を進行
//
// 設計選択 (Phase 1):
//   ・**Bridge スケルトン**: 実際に `acs::AudioEngine` を叩く箇所は TODO コメント
//     で Phase 2 に送り、state machine と volume 計算だけ完全に実装する。
//     ACS_LOG_INFO で「Phase 2 で接続」と一度ログを残す。
//   ・**Result<void> は使わない**: この層は失敗を返さない (ログ警告のみ)。
//     不正引数 (null name / 負の duration 等) は警告ログ + 既定値で続行。
//   ・**SoA cross-fade state**: 同時に鳴る BGM は最大 2 本 (current + next)。
//     `_bgm[0]` が現行、`_bgm[1]` が遷移中の新 BGM。遷移完了で swap。
//   ・**Ducking は単一 timer**: スタックしない (新 Duck が来たら上書き)。
//     fade-in/fade-out は単純な線形 (depth → 1.0 まで 0.1 秒で復帰の固定窓)。
//   ・**SFX は ring 風に固定容量**: 容量 32 (典型的同時発音数を踏まえた目安)。
//     満杯なら最古を上書き (シューティング的に許容)。
//   ・**name は所有しない**: `const char*` を保持 = ROM の文字列リテラル前提。
//     Phase 2 で StringView / Asset Handle に置き換える。
//
// 範囲外 (Phase 2+ で):
//   ・実際の AudioEngine 接続 (現在は state のみ)
//   ・3D positional / spatial / submix bus / DSP chain
//   ・スナップショット (mixer state を hot-swap)
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"

namespace acs::game {

class AudioDirector {
public:
    // SFX one-shot 最大同時発音数。超過時は最古を上書き。
    static constexpr u32 kMaxSfxVoices = 32;
    // ダッキング fade-out / fade-in の固定窓 (秒)。
    static constexpr f32 kDuckFadeWindow = 0.1f;

    AudioDirector() noexcept;
    ~AudioDirector() noexcept = default;

    AudioDirector(const AudioDirector&)            = delete;
    AudioDirector& operator=(const AudioDirector&) = delete;

    // ----- ボリュームバス -----
    // [0, 1] にクランプ。範囲外は警告 + clamp して受理。
    void SetMasterVolume(f32 v) noexcept;
    void SetBgmVolume(f32 v) noexcept;
    void SetSfxVolume(f32 v) noexcept;
    f32  GetMasterVolume() const noexcept { return _master_volume; }
    f32  GetBgmVolume()    const noexcept { return _bgm_volume; }
    f32  GetSfxVolume()    const noexcept { return _sfx_volume; }

    // ----- BGM クロスフェード -----
    // name が同じ場合は no-op (現行 BGM 継続)。fade_in_sec <= 0 で即時切替。
    // 既に遷移中なら新規 BGM へ再遷移 (current は強制停止)。
    void PlayBgm(const char* name, f32 fade_in_sec = 1.0f, bool loop = true) noexcept;
    // BGM を fade out して停止。fade_out_sec <= 0 で即時停止。
    void StopBgm(f32 fade_out_sec = 0.5f) noexcept;
    const char* CurrentBgmName() const noexcept { return _bgm[0].name; }

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
    bool IsPaused() const noexcept { return _paused; }

    // ----- driver (Game / SceneManager から毎フレーム呼ぶ) -----
    // Pause 中は dt を消費しない (state 凍結)。
    void Tick(f32 dt) noexcept;

    // ----- 派生情報 (debug / Phase 2 で AudioEngine に渡す予定) -----
    // 実際に AudioEngine に流す合成済みボリューム。
    //   master * bgm * duck_envelope  (BGM 系)
    //   master * sfx                  (SFX 系)
    f32 EffectiveBgmVolume() const noexcept;
    f32 EffectiveSfxVolume() const noexcept;

private:
    // BGM スロット 1 本の state。`_bgm[0]` = current、`_bgm[1]` = 遷移中の new。
    struct BgmSlot {
        const char* name      = nullptr;   // 所有しない (literal 前提)
        f32         gain      = 0.0f;      // 現在の slot ゲイン [0, 1]
        f32         target    = 0.0f;      // 目標ゲイン (= 0 で fade out 中)
        f32         fade_per_sec = 0.0f;   // |target - gain| / fade_duration
        bool        loop      = true;
        bool        active    = false;
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
    f32 _master_volume = 1.0f;
    f32 _bgm_volume    = 1.0f;
    f32 _sfx_volume    = 1.0f;

    // ----- BGM クロスフェード state -----
    BgmSlot _bgm[2] {};

    // ----- SFX ring (固定容量) -----
    Array<SfxEntry> _sfx;   // 容量 kMaxSfxVoices で reserve、サイズも同じく予約
    u32             _sfx_head = 0;  // 上書き時の次書込先 (FIFO)

    // ----- Duck state -----
    bool _duck_active        = false;
    f32  _duck_depth         = 1.0f;  // 谷底ゲイン [0, 1]
    f32  _duck_remaining     = 0.0f;  // 全体残り (fade in + hold + fade out)
    f32  _duck_total         = 0.0f;  // 全体長 (= duration + 2 * kDuckFadeWindow)

    // ----- 全体制御 -----
    bool _paused = false;
};

} // namespace acs::game

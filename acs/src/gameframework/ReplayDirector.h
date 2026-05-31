// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — FReplayDirector (高レベル replay 再生システム)
//
// 役割:
//   1) **ハイレベル replay コントローラ**: 低レベルの `FInputRecorder` (raw 入力)
//      と `FLockstep` (deterministic input frame) を統合し、「録画開始 / 停止 /
//      再生 / 一時停止 / 早送り / 巻き戻し」という FGame UI レベルの粒度で
//      replay を扱う。アプリ側は本 director だけを通じて record/playback の
//      ライフサイクルを管理し、低レベル詳細 (sample / frame の bit-precise
//      layout や cursor 管理) に立ち入らない。
//   2) **メタデータ管理**: ゲームバージョン / レベル ID / RNG seed / 録画日時 /
//      録画時間 / プレイヤー名 / checksum を 1 録画ごとに保持し、replay
//      ファイル (.acsr / .acsl) と並走して保存する。version mismatch 検出や
//      "title-screen の continue で replay を選ぶ" UI でリスト表示する用途。
//   3) **再生速度コントロール**: 0.25x / 0.5x / 1x / 2x / 4x の任意倍速で
//      replay を再生する。アプリ側ループは `Tick(dt)` を毎フレーム呼ぶだけで
//      内部で `dt * speed` ぶんの tick を進める。一時停止は speed = 0 とは
//      別状態として扱い、停止中の Tick は no-op。
//   4) **任意位置への Seek**: スクラブバー UI / バグ再現用ジャンプを想定し、
//      `SeekToTick(t)` で current_tick を任意位置に飛ばす。低レベル側の
//      cursor は Phase 2 (実統合時) に同期させる。
//
// 使い方 (想定):
//   FReplayDirector dir;
//   dir.Init();
//
//   // 録画開始
//   ReplayMetadata meta{
//       .game_version = "1.0.0",
//       .level_id     = "stage_01",
//       .seed         = 0xDEADBEEFu,
//       .timestamp    = TimeNowUnix(),
//       .duration_ticks = 0,                    // Stop 時に更新
//       .player_name  = "Player1",
//       .checksum_hex = "",                     // Stop 時に更新
//   };
//   dir.StartRecording(meta);
//
//   // ゲームループ (録画中):
//   dir.Tick(dt);  // 内部で m_CurrentTick を進めるだけ。
//                  // 入力 capture は FInputRecorder/FLockstep 側で別途行う想定。
//
//   // 録画停止
//   dir.StopRecording();
//   dir.SaveReplay(L"user/replay_01.acsr");
//
//   // 後で再生
//   dir.LoadReplay(L"user/replay_01.acsr");
//   dir.StartPlayback();
//   dir.SetPlaybackSpeed(2.0f);
//   while (running) {
//       dir.Tick(dt);
//       float progress = dir.ProgressNormalized();  // [0, 1]
//       // ... シミュレーションは FLockstep::ConsumeInput 経由で進める ...
//   }
//
// 設計選択 (Phase R-3 スケルトン):
//   ・**FInputRecorder / FLockstep は forward decl**: Phase R-3 は 「ハイレベル
//      API シェイプ + state machine + tick 進行」のみを確定する。実際の
//      sample/frame 連動 (Capture/ConsumeSample の自動呼び出し) は Phase R-4
//      (Pillar M Phase 2) で「FReplayDirector が両者を所有」or「seam 経由で
//      参照する」かを決めた上で接続する。本 header では .cpp 内で完結する
//      forward decl のみ。
//   ・**EReplayMode の 4 状態**: Idle / Recording / Playback / Paused。
//      FInputRecorder の ERecorderMode と FLockstep の ENetMode と異なり、UI が
//      Pause/Resume を必要とするので Paused を専用状態として持つ。Paused
//      からの遷移は Resume (→ Playback) と Stop (→ Idle) のみ。
//   ・**ReplayMetadata は trivially-copyable POD**: `const char*` 文字列は
//      呼び出し側が寿命を保証する static / 長寿命 buffer を渡す規約
//      (FSettings / FAccessibilityProfile と同じ STL 不使用方針)。Phase R-4 で
//      acs::FString への置換は検討するが、現状は raw pointer。
//   ・**SetPlaybackSpeed は範囲 clamp**: 0 < speed <= 16 の範囲外は最寄りの
//      有効値に丸める。負値や 0 は Paused を別 API で扱うため認めない。
//   ・**SeekToTick は duration を超えたら末尾に clamp**: replay 終了後に進む
//      意味がないので duration_ticks を上限とする。0 未満は 0 に clamp。
//   ・**全 noexcept**: ACS 全体方針。エラーは `TResult<T, FErrorCode>` で伝搬する。
//   ・**コピー / ムーブ禁止**: 1 セッション 1 director の長寿命オブジェクト。
//      録画中の state が分裂すると replay のメタデータ整合が崩れるため、
//      FLockstep / FInputRecorder / FSettings と同じ方針で最初から非コピー・
//      非ムーブで固定する。
//
// 範囲外 (Phase R-4 以降):
//   ・実 FInputRecorder / FLockstep の自動連動 (現状は state machine のみ)
//   ・SaveReplay / LoadReplay の実 I/O (現状は stub。.acsr + sidecar
//      metadata .json or .meta file の bit-precise layout は R-4 で確定)
//   ・複数 replay の同時ロード / 並列再生
//   ・スクラブバー UI コンポーネント (本クラスは progress 値を返すのみ)
//   ・録画中のサムネイル自動撮影 (FPhotoMode 経由で別途)
//   ・ネットワーク replay 共有 (StorefrontBridge 経由で別途)
#pragma once

#include "container/String.h"
#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

// ----- forward decl (実 I/O は .cpp で接続) -----
// FReplayDirector は両者を直接所有せず、非所有ポインタ経由で連動させる
// (SetSources で注入する)。header では型の完全定義を必要としないため
// forward decl のみに留め、実 include は ReplayDirector.cpp 側に閉じ込める。
class FInputRecorder;
class FLockstep;

// =============================================================================
// EReplayMode — FReplayDirector の動作モード
// -----------------------------------------------------------------------------
// FInputRecorder の ERecorderMode (Idle/Recording/Replaying) と FLockstep の
// ENetMode (Local/FLockstep/Replay) を統合し、UI が要求する Pause/Resume を
// 加えた 4 状態。状態遷移図:
//
//   Idle ── StartRecording ──> Recording ── StopRecording ──> Idle
//   Idle ── StartPlayback  ──> Playback  ── StopPlayback  ──> Idle
//   Playback ── PausePlayback ──> Paused
//   Paused   ── ResumePlayback ──> Playback
//   Paused   ── StopPlayback   ──> Idle
//
// Recording から Playback への直接遷移は禁止 (一旦 Stop を挟む)。
// =============================================================================
enum class EReplayMode : u8 {
    Idle      = 0,  // 録画も再生もしない (初期状態 / 停止状態)
    Recording = 1,  // 録画中 (Tick で m_CurrentTick を進める)
    Playback  = 2,  // 再生中 (Tick で m_CurrentTick を speed 倍で進める)
    Paused    = 3,  // 再生中だが進行停止 (Tick は no-op)
};

// =============================================================================
// ReplayMetadata — 1 録画分のメタデータ
// -----------------------------------------------------------------------------
// trivially-copyable な POD。`const char*` 文字列は呼び出し側が寿命を保証する
// static / 長寿命 buffer を渡す規約 (FSettings / FAccessibilityProfile と同方針)。
// timestamp は Unix 秒、duration_ticks は録画停止時に確定する。
// checksum_hex は FLockstep::ComputeChecksum を 16 文字 hex 化したもの (将来
// Phase R-4 で自動生成)。
// =============================================================================
struct ReplayMetadata {
    const char* game_version   = nullptr;  // 例 "1.0.0" (NUL 終端の長寿命文字列)
    const char* level_id       = nullptr;  // 例 "stage_01" (録画開始時のシーン ID)
    u64         seed           = 0;        // RNG seed (決定論再生用)
    u64         timestamp      = 0;        // 録画開始時刻 (Unix 秒)
    u32         duration_ticks = 0;        // 録画終了時に確定する総 tick 数
    const char* player_name    = nullptr;  // プレイヤー名 (任意。null 可)
    const char* checksum_hex   = nullptr;  // FLockstep checksum の hex 文字列 (16 文字)
};

// =============================================================================
// FReplayDirector — ハイレベル replay コントローラ
// -----------------------------------------------------------------------------
// 1 セッション 1 オブジェクトの想定。コピー / ムーブ禁止で誤分裂を防ぐ。
// =============================================================================
class FReplayDirector {
public:
    FReplayDirector()  noexcept = default;
    ~FReplayDirector() noexcept = default;

    FReplayDirector(const FReplayDirector&)            = delete;
    FReplayDirector& operator=(const FReplayDirector&) = delete;
    FReplayDirector(FReplayDirector&&)                 = delete;
    FReplayDirector& operator=(FReplayDirector&&)      = delete;

    // 共通エラー subcode (FSaveSlot / FLockstep / FInputRecorder と同 pattern)。
    enum SubCode : u16 {
        kSub_NullPath         = 1,  // SaveReplay / LoadReplay の file_path == nullptr
        kSub_BadMode          = 2,  // Start/Stop が現在 mode と不整合
        kSub_Io               = 3,  // CreateFileW / ReadFile / WriteFile / MoveFileExW 失敗
        kSub_BadMagic         = 4,  // LoadReplay: container magic 'ACRP' 不一致
        kSub_BadVersion       = 5,  // LoadReplay: container version 不一致
        kSub_BadSize          = 6,  // LoadReplay: blob サイズが container と矛盾
        kSub_BadCrc           = 7,  // LoadReplay: CRC32 mismatch (破損 / 改竄)
        kSub_Oom              = 8,  // 読み書きバッファの確保に失敗
        kSub_PathTooLong      = 9,  // SaveReplay: file_path が .tmp suffix を足すと長すぎる
        kSub_NotImplemented   = 99, // (旧 stub 値。後方互換のため残置。現在は未使用)
    };

    // ----- 初期化 -----
    // m_Mode = Idle / m_CurrentTick = 0 / m_PlaybackSpeed = 1.0 にリセット。
    // metadata はデフォルト初期化 (全 null / 0)。
    // 注: SetSources で注入した source ポインタ / owned 文字列バッファは
    //     Init() では触らない (source 結線は director の寿命を通じて維持したい)。
    void Init() noexcept;

    // ----- 低レベル source の注入 (非所有) -----
    // SaveReplay / LoadReplay が直列化対象とする raw 入力 (FInputRecorder) と
    // deterministic input frame (FLockstep) を注入する。両者とも非所有
    // (director は所有権を持たず、ポインタの寿命は呼び出し側が保証する)。
    // nullptr を渡した側は SaveReplay 時に size 0 の blob として扱われ、
    // LoadReplay 時はその blob を復元先なしで読み飛ばす (container は valid)。
    // SaveReplay / LoadReplay より前に 1 度呼んでおく想定。
    void SetSources(FInputRecorder* recorder, FLockstep* lockstep) noexcept;

    // ----- 録画開始 / 停止 -----
    // StartRecording: mode を Recording に切り替え、metadata をコピー保存。
    //   呼び出し前の mode が Idle 以外なら kSub_BadMode (誤遷移防止)。
    TResult<void> StartRecording(const ReplayMetadata& meta) noexcept;

    // StopRecording: Recording から Idle へ。metadata.duration_ticks に
    //   m_CurrentTick を確定書き込みする。Recording 以外で呼ぶと kSub_BadMode。
    TResult<void> StopRecording() noexcept;

    // ----- 再生開始 / 停止 -----
    // StartPlayback: 録画したものを再生する。Idle 以外で呼ぶと kSub_BadMode。
    //   m_CurrentTick = 0 にリセット。metadata は LoadReplay 経由 or 直前の
    //   StartRecording で設定済みである前提。
    TResult<void> StartPlayback() noexcept;

    // PausePlayback: Playback → Paused。Playback 以外では no-op。
    void PausePlayback() noexcept;

    // ResumePlayback: Paused → Playback。Paused 以外では no-op。
    void ResumePlayback() noexcept;

    // StopPlayback: Playback / Paused → Idle。それ以外では no-op。
    void StopPlayback() noexcept;

    // ----- 状態 query -----
    EReplayMode CurrentMode()         const noexcept { return m_Mode; }
    f32        PlaybackSpeed()       const noexcept { return m_PlaybackSpeed; }
    u32        CurrentTick()         const noexcept { return m_CurrentTick; }
    u32        DurationTicks()       const noexcept { return m_Metadata.duration_ticks; }
    f32        ProgressNormalized()  const noexcept;
    const ReplayMetadata& Metadata() const noexcept { return m_Metadata; }

    // SetPlaybackSpeed: 0.25x / 0.5x / 1x / 2x / 4x 等の任意倍速。
    //   0 < speed <= 16 の範囲外は最寄りの有効値に clamp する。
    //   負値は 1.0 にリセット (誤呼び出し防止)。Pause は別 API なので 0 は不可。
    void SetPlaybackSpeed(f32 speed) noexcept;

    // ----- 任意位置への Seek -----
    // m_CurrentTick を tick にジャンプする。duration_ticks を超える値は末尾に
    // clamp。Mode は変更しない (Paused 中も Playback 中も可)。
    void SeekToTick(u32 tick) noexcept;

    // ----- 毎フレーム呼び出し -----
    // Recording 中: m_CurrentTick を 1 tick / Tick(dt) 進める単純実装。
    //   tick 単位の精密制御は呼び出し側 (FLockstep) の責務。
    // Playback 中: dt * speed をアキュムレートし、tick_rate_hz 換算で必要 tick 数を
    //   m_CurrentTick に加算する。duration_ticks を超えたら自動的に Idle へ。
    // Paused / Idle: no-op。
    void Tick(f32 dt) noexcept;

    // ----- 永続化 (本実装。container 1 ファイル + atomic write) -----
    // SaveReplay: 現在の metadata + 注入済み FInputRecorder / FLockstep の
    //   直列化 blob を 1 つの container ファイル (.acsr 拡張子想定) に書き出す。
    //   layout は [magic 'ACRP'][version][metadata][input_blob][lockstep_blob][crc32]。
    //   FSaveArchive / FSettings と同じ atomic write (`.tmp` → MoveFileExW rename)
    //   で書き込み途中のクラッシュでも本ファイルが破損しないことを保証する。
    //   source が未注入 (nullptr) の側は size 0 の blob として書かれる。
    //   file_path == nullptr は kSub_NullPath、I/O 失敗は kSub_Io。
    TResult<void> SaveReplay(const wchar_t* file_path) noexcept;

    // LoadReplay: file_path の container を読み、magic / version / CRC32 を検証して
    //   metadata を復元し、注入済み source があれば input_blob / lockstep_blob を
    //   各 LoadFromBuffer に渡す。復元した metadata の文字列フィールドは director が
    //   所有する内部バッファ (m_*Owned) を指す (ファイル由来の寿命を director に
    //   束ねる)。成功後は m_Mode = Idle / m_CurrentTick = 0 とし StartPlayback 待ち。
    //   file_path == nullptr は kSub_NullPath、magic/version/CRC 不正は対応 subcode。
    TResult<void> LoadReplay(const wchar_t* file_path) noexcept;

private:
    EReplayMode     m_Mode             = EReplayMode::Idle;
    ReplayMetadata m_Metadata         {};         // 現在の録画 / 再生対象の metadata
    u32            m_CurrentTick     = 0;        // 録画中: 次に書き込む tick / 再生中: 次に消費する tick
    f32            m_PlaybackSpeed   = 1.0f;     // 再生倍速 (1.0 = 等倍)
    f32            m_TickAccumulator = 0.0f;     // Tick(dt) の余り (sub-tick の dt を持ち越す)
    u32            m_TickRateHz     = 60;       // 再生時の sample rate

    // ----- 低レベル source (非所有。SetSources で注入) -----
    FInputRecorder* m_Recorder = nullptr;  // raw 入力 (.acsr blob) の供給元 / 復元先
    FLockstep*      m_Lockstep = nullptr;  // deterministic input frame (.acsl blob) の供給元 / 復元先

    // ----- LoadReplay 後の metadata 文字列の所有バッファ -----
    // ReplayMetadata は const char* しか持たないため、LoadReplay でファイルから
    // 復元した文字列の寿命を director に束ねる。m_Metadata.game_version 等は
    // これらの Data() を指す (FString::Data() は常に NUL 終端)。
    FString m_GameVersionOwned;
    FString m_LevelIdOwned;
    FString m_PlayerNameOwned;
    FString m_ChecksumHexOwned;
};

} // namespace acs::game

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/String.h"
#include "foundation/Result.h"
#include "foundation/Types.h"
#include "gameframework/Forward.h"

namespace acs::game {

/**
 * CReplayDirector の動作モード。
 *
 * @details
 * CInputRecorder の ERecorderMode と CLockstep の ENetMode を統合し、UI が要求する
 * Pause/Resume を加えた 4 状態。遷移は Idle→Recording→Idle / Idle→Playback→Idle /
 * Playback↔Paused / Paused→Idle で、Recording から Playback への直接遷移は禁止
 * (一旦 Stop を挟む)。
 */
enum class EReplayMode : u8 {
    /** 録画も再生もしない初期状態 / 停止状態。 */
    Idle      = 0,

    /** 録画中。Tick で m_CurrentTick を進める。 */
    Recording = 1,

    /** 再生中。Tick で m_CurrentTick を speed 倍で進める。 */
    Playback  = 2,

    /** 再生中だが進行停止。Tick は no-op。 */
    Paused    = 3,
};

/**
 * 1 録画分のメタデータ。
 *
 * @details
 * trivially-copyable な POD。`TryStartRecording` は `const char*` 文字列を上限付きで
 * director 所有bufferへ複製し、`TryLoadReplay` も復元文字列をdirectorが所有する。
 */
struct FReplayMetadata {
    /** ゲームバージョン文字列 (例 "1.0.0"、NUL 終端の長寿命文字列)。 */
    const char* game_version   = nullptr;

    /** レベル ID (例 "stage_01"、録画開始時のシーン ID)。 */
    const char* level_id       = nullptr;

    /** RNG seed (決定論再生用)。 */
    u64         seed           = 0;

    /** 録画開始時刻 (Unix 秒)。 */
    u64         timestamp      = 0;

    /** 録画終了時に確定する総 tick 数。 */
    u32         duration_ticks = 0;

    /** プレイヤー名 (任意。null 可)。 */
    const char* player_name    = nullptr;

    /** CLockstep checksum の hex 文字列 (16 文字)。 */
    const char* checksum_hex   = nullptr;
};

/** Replay container 全体の上限 (256 MiB)。 */
inline constexpr u64 kReplayMaximumContainerBytes = 256ull * 1024ull * 1024ull;

/** 内包する recorder / lockstep blob それぞれの上限 (128 MiB)。 */
inline constexpr u32 kReplayMaximumSourceBlobBytes = 128u * 1024u * 1024u;

/** recorder samples / lockstep frames それぞれの件数上限。 */
inline constexpr u32 kReplayMaximumSourceRecords = 1'000'000u;

/** replay path の終端NULを除く最大文字数。 */
inline constexpr usize kReplayMaximumPathChars = 1023u;

/** metadata fieldごとのUTF-8 byte上限。 */
inline constexpr u32 kReplayMaximumGameVersionBytes = 64u;
inline constexpr u32 kReplayMaximumLevelIdBytes = 255u;
inline constexpr u32 kReplayMaximumPlayerNameBytes = 255u;
inline constexpr u32 kReplayChecksumHexBytes = 16u;

/**
 * 低レベルの入力録画と lockstep を統合するハイレベル replay コントローラ。
 *
 * @details
 * CInputRecorder (raw 入力) と CLockstep (deterministic input frame) を非所有
 * ポインタで束ね、録画 / 再生 / 一時停止 / 倍速 / Seek を UI 粒度で扱う。1 セッション
 * 1 オブジェクトの想定で、録画 state の分裂を防ぐためコピー / ムーブ禁止。
 */
class CReplayDirector {
public:
    /** 空状態 (Idle / tick 0 / speed 1.0) で構築する。 */
    CReplayDirector()  noexcept = default;

    /** デストラクタ (非所有 source は解放しない)。 */
    ~CReplayDirector() noexcept = default;

    /** コピー禁止 (1 セッション 1 director の長寿命オブジェクト)。 */
    CReplayDirector(const CReplayDirector&)            = delete;

    /** コピー代入も禁止。 */
    CReplayDirector& operator=(const CReplayDirector&) = delete;

    /** ムーブ禁止。 */
    CReplayDirector(CReplayDirector&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CReplayDirector& operator=(CReplayDirector&&)      = delete;

    /**
     * SaveReplay / LoadReplay / 状態遷移が返すエラー subcode。
     *
     * @details FErrorCode.subcode に格納される (TSaveSlot / CLockstep / CInputRecorder と同 pattern)。
     */
    enum ESubCode : u16 {
        /** SaveReplay / LoadReplay の file_path == nullptr。 */
        kSub_NullPath         = 1,

        /** Start/Stop が現在 mode と不整合 (誤遷移)。 */
        kSub_BadMode          = 2,

        /** CreateFileW / ReadFile / WriteFile / MoveFileExW 失敗。 */
        kSub_Io               = 3,

        /** LoadReplay: container magic 'ACRP' 不一致。 */
        kSub_BadMagic         = 4,

        /** LoadReplay: container version 不一致。 */
        kSub_BadVersion       = 5,

        /** LoadReplay: blob サイズが container と矛盾。 */
        kSub_BadSize          = 6,

        /** LoadReplay: CRC32 mismatch (破損 / 改竄)。 */
        kSub_BadCrc           = 7,

        /** 読み書きバッファの確保に失敗。 */
        kSub_Oom              = 8,

        /** SaveReplay: file_path が .tmp suffix を足すと長すぎる。 */
        kSub_PathTooLong      = 9,

        /** metadata文字列、checksum、path等が非正規。 */
        kSub_BadMetadata      = 10,

        /** container、blob、record件数が製品上限を超える。 */
        kSub_LimitExceeded    = 11,

        /** FlushFileBuffers が失敗し、永続化を確定できない。 */
        kSub_FlushFailed      = 12,

        /** 一時ファイルから保存先へのatomic replaceが失敗。 */
        kSub_AtomicReplaceFailed = 13,

        /** 内包するinput/lockstep blobの構造またはsource契約が不正。 */
        kSub_BadSourceBlob    = 14,

        /** 旧 stub 値 (後方互換のため残置。現在は未使用)。 */
        kSub_NotImplemented   = 99,
    };

    /**
     * 録画 / 再生セッションの state をリセットする。
     *
     * @details
     * m_Mode = Idle / m_CurrentTick = 0 / m_PlaybackSpeed = 1.0 に戻し、metadata を
     * デフォルト初期化する。SetSources で注入した source ポインタと owned 文字列バッファは
     * 触らない (source 結線は director の寿命を通じて維持する)。
     */
    void Init() noexcept;

    /**
     * SaveReplay / LoadReplay が扱う低レベル source を注入する (非所有)。
     *
     * @details
     * director は所有権を持たず、ポインタの寿命は呼び出し側が保証する。nullptr を渡した側は
     * SaveReplay 時に size 0 の blob として扱われ、LoadReplay 時はその blob を読み飛ばす。
     * @param recorder raw 入力の供給元 / 復元先 (null 可)。
     * @param lockstep deterministic input frame の供給元 / 復元先 (null 可)。
     */
    void SetSources(CInputRecorder* recorder, CLockstep* lockstep) noexcept;

    /**
     * 録画を開始する (checked APIへ委譲し、metadata文字列をowned copyする)。
     *
     * @param meta 録画に紐づける metadata。
     * @return Idle から呼ばれれば Ok、それ以外の mode なら kSub_BadMode。
     */
    TResult<void> StartRecording(const FReplayMetadata& meta) noexcept;

    /**
     * metadata文字列を上限付きで複製してから録画を開始する。
     *
     * @details 失敗時はmode、tick、metadata、owned文字列を変更しない。
     */
    TResult<void> TryStartRecording(const FReplayMetadata& meta) noexcept;

    /**
     * 録画を停止する (Recording → Idle)。
     *
     * @details metadata.duration_ticks に m_CurrentTick を確定書き込みする。
     * @return Recording から呼ばれれば Ok、それ以外の mode なら kSub_BadMode。
     */
    TResult<void> StopRecording() noexcept;

    /**
     * 再生を開始する (Idle → Playback、m_CurrentTick を 0 にリセット)。
     *
     * @details metadata は LoadReplay 経由 or 直前の StartRecording で設定済みである前提。
     * @return Idle から呼ばれれば Ok、それ以外の mode なら kSub_BadMode。
     */
    TResult<void> StartPlayback() noexcept;

    /** 再生を一時停止する (Playback → Paused。Playback 以外では no-op)。 */
    void PausePlayback() noexcept;

    /** 一時停止から再開する (Paused → Playback。Paused 以外では no-op)。 */
    void ResumePlayback() noexcept;

    /** 再生を停止する (Playback / Paused → Idle。それ以外では no-op)。 */
    void StopPlayback() noexcept;

    /**
     * 現在の動作モードを返す。
     *
     * @return 現在の EReplayMode。
     */
    EReplayMode CurrentMode()         const noexcept { return m_Mode; }

    /**
     * 現在の再生倍速を返す。
     *
     * @return 再生倍速 (1.0 = 等倍)。
     */
    f32        PlaybackSpeed()       const noexcept { return m_PlaybackSpeed; }

    /**
     * 現在の tick 位置を返す。
     *
     * @return 録画中なら次に書き込む tick、再生中なら次に消費する tick。
     */
    u32        CurrentTick()         const noexcept { return m_CurrentTick; }

    /**
     * 録画全体の総 tick 数を返す。
     *
     * @return metadata に焼かれた総 tick 数 (未設定なら 0)。
     */
    u32        DurationTicks()       const noexcept { return m_Metadata.duration_ticks; }

    /**
     * 再生進捗を [0, 1] で返す。
     *
     * @return current_tick / duration_ticks を 1.0 で clamp した値 (duration 0 なら 0.0)。
     */
    f32        ProgressNormalized()  const noexcept;

    /**
     * 現在の metadata への const 参照を返す。
     *
     * @return 録画 / 再生対象の FReplayMetadata。
     */
    const FReplayMetadata& Metadata() const noexcept { return m_Metadata; }

    /**
     * 再生倍速を設定する。
     *
     * @details 0 < speed <= 16 の範囲外は最寄りの有効値に clamp し、0 / 負値 / NaN は 1.0 に戻す。
     * @param speed 設定する倍速 (例 0.25 / 0.5 / 1 / 2 / 4)。
     */
    void SetPlaybackSpeed(f32 speed) noexcept;

    /**
     * 任意の tick 位置にジャンプする。
     *
     * @details duration_ticks を上限に clamp し、Mode は変更しない (Paused / Playback どちらも可)。
     * @param tick ジャンプ先の tick。
     */
    void SeekToTick(u32 tick) noexcept;

    /**
     * 毎フレーム呼び、現在 mode に応じて tick を進める。
     *
     * @details
     * Recording 中は tick_rate_hz * dt 分、Playback 中は dt * speed * tick_rate_hz 分だけ
     * m_CurrentTick を加算し、duration_ticks に達したら自動的に Idle へ落とす。Paused / Idle は no-op。
     * @param dt 前フレームからの経過秒 (0 以下 / NaN / 60秒超は無視)。
     */
    void Tick(f32 dt) noexcept;

    /**
     * 現在の metadata と source の直列化 blob を 1 つの container に書き出す。
     *
     * @details
     * layout は [magic 'ACRP'][version][metadata][input_blob][lockstep_blob][crc32]。
     * `.tmp` に書いて FlushFileBuffers → MoveFileExW で atomic rename し、書き込み途中の
     * クラッシュでも本ファイルが破損しないことを保証する。未注入 source は size 0 blob となる。
     * @param file_path 出力先パス (.acsr 拡張子想定)。
     * @return 成功なら空の TResult、null パスは kSub_NullPath、I/O 失敗は kSub_Io。
     */
    TResult<void> SaveReplay(const wchar_t* file_path) noexcept;

    /**
     * 上限・allocation・flush・atomic replaceの全失敗を返すchecked保存API。
     *
     * @details `<path>.tmp.<pid>.<tid>.<nonce>` をCREATE_NEWで作成し、本ファイルは
     * flushとcloseが成功するまで変更しない。
     */
    TResult<void> TrySaveReplay(const wchar_t* file_path) noexcept;

    /**
     * container を読み、検証して metadata と source blob を復元する。
     *
     * @details
     * magic / version / CRC32を検証してmetadataと両sourceを一時領域へ復元し、全staging成功後に
     * 一括commitする。復元文字列はdirectorが所有し、成功後はm_Mode = Idle / m_CurrentTick = 0となる。
     * @param file_path 入力ファイルパス。
     * @return 成功なら空の TResult、null パスは kSub_NullPath、検証失敗は対応 subcode。
     */
    TResult<void> LoadReplay(const wchar_t* file_path) noexcept;

    /**
     * containerと内包blobを全検証し、一時文字列の確保成功後にだけstateを置換する。
     *
     * @details 失敗時はdirectorのmetadata、owned文字列、mode、tickと両sourceを変更しない。
     */
    TResult<void> TryLoadReplay(const wchar_t* file_path) noexcept;

private:
    /** 現在の動作モード。 */
    EReplayMode     m_Mode             = EReplayMode::Idle;

    /** 現在の録画 / 再生対象の metadata。 */
    FReplayMetadata m_Metadata         {};

    /** 録画中は次に書き込む tick、再生中は次に消費する tick。 */
    u32            m_CurrentTick     = 0;

    /** 再生倍速 (1.0 = 等倍)。 */
    f32            m_PlaybackSpeed   = 1.0f;

    /** Tick(dt) の余り (sub-tick の dt を持ち越す)。 */
    f32            m_TickAccumulator = 0.0f;

    /** 再生時の tick rate (Hz)。 */
    u32            m_TickRateHz     = 60;

    /** raw 入力 (.acsr blob) の供給元 / 復元先 (非所有)。 */
    CInputRecorder* m_Recorder = nullptr;

    /** deterministic input frame (.acsl blob) の供給元 / 復元先 (非所有)。 */
    CLockstep*      m_Lockstep = nullptr;

    /** LoadReplay で復元した game_version の所有バッファ。 */
    FString m_GameVersionOwned;

    /** LoadReplay で復元した level_id の所有バッファ。 */
    FString m_LevelIdOwned;

    /** LoadReplay で復元した player_name の所有バッファ。 */
    FString m_PlayerNameOwned;

    /** LoadReplay で復元した checksum_hex の所有バッファ。 */
    FString m_ChecksumHexOwned;
};

} // namespace acs::game

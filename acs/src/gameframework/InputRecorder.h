// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Array.h"
#include "foundation/Result.h"
#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

class FReplayDirector;

/** 1 recorderへ読み込めるsample件数上限。 */
inline constexpr u32 kInputRecorderMaximumSamples = 1'000'000u;

/** 永続化headerで許可するtick rate上限。 */
inline constexpr u32 kInputRecorderMaximumTickRateHz = 1000u;

/**
 * 1 tick 分の raw 入力サンプル。
 *
 * @details
 * trivially-copyable な POD。TArray に詰めて bulk memcpy で I/O できる。
 * `key_codes_changed[i]` は今 tick に状態変化があった key code (Win32 VK_* 風 /
 * SDL scancode 風 — 解釈はアプリ層に委ねる)、`key_states[i]` は同 index の
 * press(1) / release(0) を表す。未使用スロットは key_codes_changed = 0 で埋める
 * 規約 (key code 0 は VK_NULL = 「無効キー」相当)。最大 8 key までしか格納
 * できないが、同一 tick 内で 9 個以上の状態変化が同時に起きるケースは事実上
 * 発生しないので十分。
 */
struct FInputSample {
    /** フレーム番号 (0 起点、tick_rate_hz で時刻に変換)。 */
    u32  tick                 = 0;

    /** 状態変化した key code (未使用 slot = 0)。 */
    u8   key_codes_changed[8] = {};

    /** 同 index の press(1) / release(0)。 */
    u8   key_states[8]        = {};

    /** マウス位置 (window-local px。座標系はアプリ側で統一)。 */
    FVec2 mouse_pos            {};

    /** マウスボタン bitmask (L=bit0, R=bit1, M=bit2, X1=bit3, X2=bit4)。 */
    u8   mouse_button_states  = 0;
};

/**
 * FInputRecorder の動作モード。
 *
 * @details
 * FLockstep の ENetMode と異なり、ネットコードは扱わず「録画もしない・録画する・
 * 再生する」の 3 状態のみで完結する。モード切替時は state を Clear せず、
 * cursor だけリセットする (録画した内容をそのまま StartReplay で再生する想定)。
 */
enum class ERecorderMode : u8 {
    /** 録画も再生もしない (初期状態)。 */
    Idle      = 0,

    /** Capture() で sample を蓄積する。 */
    Recording = 1,

    /** ConsumeSample() で蓄積済み sample を取り出す。 */
    Replaying = 2,
};

/**
 * raw 入力の録画 / 再生を担うレコーダ。
 *
 * @details
 * 1 セッション 1 オブジェクトの想定。コピー / ムーブ禁止で誤分裂を防ぐ。
 */
class FInputRecorder {
public:
    /** 空状態 (Idle、samples なし) で構築する。 */
    FInputRecorder()  noexcept = default;

    /** 破棄する (samples は TArray が解放)。 */
    ~FInputRecorder() noexcept = default;

    /** コピー禁止 (録画 state の分裂を防ぐため)。 */
    FInputRecorder(const FInputRecorder&)            = delete;

    /** コピー代入も禁止。 */
    FInputRecorder& operator=(const FInputRecorder&) = delete;

    /** ムーブ禁止。 */
    FInputRecorder(FInputRecorder&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FInputRecorder& operator=(FInputRecorder&&)      = delete;

    /**
     * SaveToBuffer / LoadFromBuffer が返す共通エラー subcode。
     *
     * @details
     * TSaveSlot / FLockstep / IBackendClient と同じ pattern。上位層が switch で
     * 分岐できるよう enum 風に固定値を割り当てる。
     */
    enum ESubCode : u16 {
        /** SaveToBuffer / LoadFromBuffer の buffer == nullptr。 */
        kSub_NullBuffer     = 1,

        /** SaveToBuffer: buffer.size が必要量未満。 */
        kSub_BufferTooSmall = 2,

        /** LoadFromBuffer: magic 不一致。 */
        kSub_BadMagic       = 3,

        /** LoadFromBuffer: version 不一致。 */
        kSub_BadVersion     = 4,

        /** LoadFromBuffer: sample_count とサイズが矛盾。 */
        kSub_BadSize        = 5,

        /** LoadFromBuffer: CRC mismatch。 */
        kSub_BadCrc         = 6,

        /** LoadFromBuffer: staging allocationに失敗。 */
        kSub_Oom            = 7,

        /** sample件数またはtick rateが製品上限外。 */
        kSub_LimitExceeded  = 8,

        /** sampleにNaN/Infinityの非正規floatが含まれる。 */
        kSub_BadValue       = 9,

        /** 未実装。 */
        kSub_NotImplemented = 99,
    };

    /**
     * 録画を開始する。
     *
     * @details
     * mode を Recording に切り替え、cursor / current_tick をリセットする。
     * 既存 samples は破棄しない (続きから録画したい場合は Clear() と併用)。
     * @param tick_rate_hz replay の sample rate 整合検証用 (ファイルに保存される)。
     */
    void StartRecording(u32 tick_rate_hz = 60) noexcept;

    /** 録画を停止し、Idle 状態に戻す (samples / tick_rate_hz は保持)。 */
    void StopRecording() noexcept;

    /**
     * 再生を開始する。
     *
     * @details
     * mode を Replaying に切り替え、cursor / current_tick = 0 にリセットする。
     * 既存 samples は保持し、ConsumeSample で先頭から取り出せる状態にする。
     */
    void StartReplay() noexcept;

    /** 再生を停止し、Idle 状態に戻す (samples / tick_rate_hz は保持)。 */
    void StopReplay() noexcept;

    /**
     * 1 sample を記録する。
     *
     * @details
     * Recording モード以外では no-op (誤呼び出しを許容)。内部 m_CurrentTick は
     * sample.tick + 1 に進める (連続 tick 想定)。
     * @param s 記録する 1 tick 分の入力サンプル。
     */
    void Capture(const FInputSample& s) noexcept;

    /**
     * 指定 tick の sample を取り出す。
     *
     * @details
     * Replaying モード以外では false を返す。該当 sample が見つかれば out に
     * 書き込んで true、なければ out を変更せず false。m_Cursor を前進させ、
     * 線形検索を amortised O(1) にする。
     * @param tick 取り出したい tick 番号。
     * @param out 見つかった sample の書き込み先。
     * @return 該当 sample が見つかれば true。
     */
    bool ConsumeSample(u32 tick, FInputSample& out) noexcept;

    /**
     * 現在の動作モードを返す。
     *
     * @return Idle / Recording / Replaying のいずれか。
     */
    ERecorderMode CurrentMode() const noexcept { return m_Mode; }

    /**
     * 蓄積済み sample 数を返す。
     *
     * @return 録画済み FInputSample の個数。
     */
    u32          SampleCount() const noexcept;

    /**
     * 次に書き込む / 消費する tick を返す。
     *
     * @return 現在の内部 tick カーソル。
     */
    u32          CurrentTick() const noexcept { return m_CurrentTick; }

    /**
     * sample rate を返す。
     *
     * @return StartRecording / Load で設定された tick rate (Hz)。
     */
    u32          TickRateHz()  const noexcept { return m_TickRateHz; }

    /** 全 samples を破棄し、cursor / current_tick をリセットする (Mode は保持)。 */
    void Clear() noexcept;

    /**
     * 現在の samples を `.acsr` layout で buffer に書き出す。
     *
     * @details
     * buffer 不足は kSub_BufferTooSmall。samples 部に CRC32 を付与する。
     * @param buffer 書き込み先バッファ。
     * @param size buffer の容量 (バイト)。
     * @param out_written 実書き込みバイト数の返却先。
     * @return 成功なら空の TResult、buffer 不足等ならエラー。
     */
    TResult<void> SaveToBuffer(u8* buffer, u32 size, u32& out_written) noexcept;

    /**
     * buffer を解釈して samples を「置換」復元する。
     *
     * @details magic 'ACSR' / version / size / CRC32 を検証し、不正なら Err。
     * @param buffer 入力バッファ (`.acsr` layout)。
     * @param size buffer のサイズ (バイト)。
     * @return 成功なら空の TResult、検証失敗ならエラー。
     */
    TResult<void> LoadFromBuffer(const u8* buffer, u32 size) noexcept;

    /**
     * bufferを全検証し、全sampleのstaging成功後にだけstateを置換する。
     *
     * @details 失敗時はsamples、tick rate、cursor、current tickを変更しない。
     */
    TResult<void> TryLoadFromBuffer(const u8* buffer, u32 size) noexcept;

private:
    friend class FReplayDirector;

    /** ReplayDirector staging用にtargetと同じallocatorを注入する。 */
    explicit FInputRecorder(FAllocator& allocator) noexcept : m_Samples(allocator) {}

    /** ReplayDirectorが複数sourceを一括commitするためのno-fail state swap。 */
    void SwapLoadedState(FInputRecorder& other) noexcept;

    /** 現在の動作モード。 */
    ERecorderMode       m_Mode          = ERecorderMode::Idle;

    /** sample rate (Hz)。 */
    u32                m_TickRateHz  = 60;

    /** 次に書き込む / 消費する tick。 */
    u32                m_CurrentTick  = 0;

    /** ConsumeSample の線形検索開始 index。 */
    u32                m_Cursor        = 0;

    /** 録画済み sample の線形ストレージ。 */
    TArray<FInputSample> m_Samples;
};

} // namespace acs::game

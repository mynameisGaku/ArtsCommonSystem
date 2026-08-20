// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Array.h"
#include "foundation/Result.h"
#include "foundation/Types.h"
#include "gameframework/Forward.h"
#include "math/Vec.h"

namespace acs::game {

/** 1 lockstepへ読み込めるframe件数上限。 */
inline constexpr u32 kLockstepMaximumFrames = 1'000'000u;

/** 永続化headerで許可するtick rate上限。 */
inline constexpr u32 kLockstepMaximumTickRateHz = 1000u;

/**
 * 1 tick 分 (1 プレイヤー分) の入力を表す trivially-copyable な POD。
 *
 * @details
 * TArray に詰めて bulk memcpy で I/O できる。buttons は最大 8 ボタンの bitmask
 * (A/B/X/Y/L/R/Start/Select 等)、axis はアナログスティック 1 本分 (-1..1 想定だが
 * 本クラスは範囲チェックを行わない)。
 */
struct FInputFrame {
    /** フレーム番号 (0 起点、tick_rate_hz で時刻に変換)。 */
    u32  tick      = 0;

    /** プレイヤー ID (0..N-1)。 */
    u32  player_id = 0;

    /** ボタン bitmask (8 ボタンまで)。 */
    u8   buttons   = 0;

    /** アナログスティック (-1..1 範囲想定)。 */
    FVec2 axis      {};
};

/**
 * 入力レイヤの動作モード。
 *
 * @details
 * CLockstep は同一クラスで「単独プレイ」「ネット対戦」「リプレイ再生」の 3 モードを
 * 扱う。モード切替時は state を Clear せず cursor だけリセットする (Local 中に記録した
 * frames を StartReplay で再生する用途を想定)。
 */
enum class ENetMode : u8 {
    /** 単独プレイ。入力を記録するが配信はしない。 */
    Local    = 0,

    /** ネット対戦。入力を記録し、リモートから受信した frame を取り込む。 */
    Lockstep = 1,

    /** 過去の入力列を再生する。RecordInput は受け付けない。 */
    Replay   = 2,
};

/**
 * 決定論的な入力レイヤ (lockstep ネットコード / 入力リプレイ)。
 *
 * @details
 * 1 tick = 1 フレーム分の入力を FInputFrame として記録 / 再生し、全クライアントが
 * 同一順序で入力を消費することで決定論的シミュレーションの土台を作る。1 セッション
 * 1 オブジェクトの想定で、誤分裂を防ぐためコピー / ムーブを禁止する。
 */
class CLockstep {
public:
    /** 空の Local モード状態で構築する。 */
    CLockstep()  noexcept = default;

    /** 破棄する (記録した frames は TArray が解放)。 */
    ~CLockstep() noexcept = default;

    /** コピー禁止 (state 分裂による replay 同期ずれを防ぐため)。 */
    CLockstep(const CLockstep&)            = delete;

    /** コピー代入も禁止。 */
    CLockstep& operator=(const CLockstep&) = delete;

    /** ムーブ禁止 (長寿命の単独所有オブジェクトのため)。 */
    CLockstep(CLockstep&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CLockstep& operator=(CLockstep&&)      = delete;

    /**
     * 永続化 API が返すエラー subcode。
     *
     * @details 上位層が switch で分岐できるよう固定値を割り当てる (TSaveSlot と同じ規約)。
     */
    enum ESubCode : u16 {
        /** SaveToBuffer / LoadFromBuffer の buffer が nullptr。 */
        kSub_NullBuffer     = 1,

        /** SaveToBuffer: buffer サイズが必要量未満。 */
        kSub_BufferTooSmall = 2,

        /** LoadFromBuffer: magic 不一致。 */
        kSub_BadMagic       = 3,

        /** LoadFromBuffer: version 不一致。 */
        kSub_BadVersion     = 4,

        /** LoadFromBuffer: frame_count とバッファサイズが矛盾。 */
        kSub_BadSize        = 5,

        /** LoadFromBuffer: CRC mismatch (改竄/破損)。 */
        kSub_BadCrc         = 6,

        /** LoadFromBuffer: staging allocationに失敗。 */
        kSub_Oom            = 7,

        /** frame件数またはtick rateが製品上限外。 */
        kSub_LimitExceeded  = 8,

        /** frameにNaN/Infinityの非正規axisが含まれる。 */
        kSub_BadValue       = 9,

        /** 未実装パス用。 */
        kSub_NotImplemented = 99,
    };

    /**
     * モードを設定し、tick / cursor をリセットする。
     *
     * @details 既存 frames は破棄しない。tick_rate_hz は replay の sample rate 整合検証用 (ファイルに保存される)。
     * @param mode 設定する動作モード。
     * @param tick_rate_hz tick の周波数 (既定 60)。
     */
    void Init(ENetMode mode, u32 tick_rate_hz = 60) noexcept;

    /**
     * 入力 1 件を記録する。
     *
     * @details Replay モード中は no-op。内部 m_CurrentTick を frame.tick + 1 に進める (連続 tick 想定)。
     * @param frame 記録する入力フレーム。
     */
    void RecordInput(const FInputFrame& frame) noexcept;

    /**
     * モードを Replay に切り替え、cursor と current_tick を 0 にリセットする。
     *
     * @details 既存 frames は保持され、ConsumeInput で先頭から取り出せる状態になる。
     */
    void StartReplay() noexcept;

    /**
     * 指定 tick / player_id の入力を取り出す。
     *
     * @details
     * Replay モード以外では false を返す。該当 frame が見つかれば out に書き込み、
     * m_ReplayCursor を前進させて線形検索を amortised O(1) にする。
     * @param tick 取り出したい tick。
     * @param player_id 取り出したいプレイヤー ID。
     * @param out 見つかった入力の書き込み先。
     * @return 見つかれば true (out 更新)、なければ false (out 不変)。
     */
    bool ConsumeInput(u32 tick, u32 player_id, FInputFrame& out) noexcept;

    /**
     * 次に書き込む / 消費する tick を返す。
     *
     * @return 現在の tick カウンタ。
     */
    u32 CurrentTick() const noexcept { return m_CurrentTick; }

    /**
     * 記録済みの入力フレーム数を返す。
     *
     * @return frames の個数。
     */
    u32 InputCount() const noexcept;

    /**
     * 現在の動作モードを返す。
     *
     * @return 設定済みの ENetMode。
     */
    ENetMode Mode() const noexcept { return m_Mode; }

    /**
     * tick の周波数を返す。
     *
     * @return Init で設定した tick_rate_hz。
     */
    u32 TickRateHz() const noexcept { return m_TickRateHz; }

    /**
     * 全入力フレームの FNV-1a-like u64 hash を返す。
     *
     * @details 決定論検証 / replay 同期ずれ検知に使う。
     * @return 全 frame を畳み込んだ checksum。
     */
    u64 ComputeChecksum() const noexcept;

    /**
     * 全 frames を破棄し、cursor / current_tick もリセットする (Mode は保持)。
     */
    void Clear() noexcept;

    /**
     * 現在の frames を file layout で buffer に書き出す。
     *
     * @details frames 部に CRC32 を付加する。buffer 不足は kSub_BufferTooSmall。
     * @param buffer 書き込み先バッファ。
     * @param size buffer の容量 (バイト)。
     * @param out_written 実際に書き込んだバイト数の出力先。
     * @return 成功なら空の TResult、失敗なら subcode 付きエラー。
     */
    TResult<void> SaveToBuffer(u8* buffer, u32 size, u32& out_written) noexcept;

    /**
     * buffer を解釈して frames を置換復元する。
     *
     * @details magic 'ACSL' / version / size / CRC32 を検証し、不正なら Err を返す。
     * @param buffer 読み込み元バッファ。
     * @param size buffer のサイズ (バイト)。
     * @return 成功なら空の TResult、検証失敗なら subcode 付きエラー。
     */
    TResult<void> LoadFromBuffer(const u8* buffer, u32 size) noexcept;

    /**
     * bufferを全検証し、全frameのstaging成功後にだけstateを置換する。
     *
     * @details 失敗時はframes、tick rate、cursor、current tickを変更しない。
     */
    TResult<void> TryLoadFromBuffer(const u8* buffer, u32 size) noexcept;

private:
    /** targetと同じallocatorを使う空のload stagingへ初期化する内部処理。 */
    void PrepareLoadStaging_Internal(CLockstep& staging) const noexcept;

    /** ReplayDirectorが複数sourceを一括commitするためのno-fail state swap。 */
    void SwapLoadedState_Internal(CLockstep& other) noexcept;

public:
    /** ReplayDirectorのtransactional loadへ内部状態操作を限定する非所有アダプター。 */
    class FPersistenceAdapter final {
    public:
        /** 操作対象を保持する。 */
        explicit FPersistenceAdapter(CLockstep& lockstep) noexcept : m_Lockstep(lockstep)
        {
        }

        /** targetと同じallocatorを使う空のstagingを準備する。自己指定は拒否する。 */
        bool PrepareLoadStaging(CLockstep& staging) const noexcept
        {
            if (&staging == &m_Lockstep) return false;
            m_Lockstep.PrepareLoadStaging_Internal(staging);
            return true;
        }

        /** 検証済みstagingの永続状態をno-fail swapで反映する。自己指定は拒否する。 */
        bool CommitLoadedState(CLockstep& staging) noexcept
        {
            if (&staging == &m_Lockstep) return false;
            m_Lockstep.SwapLoadedState_Internal(staging);
            return true;
        }

    private:
        /** 操作対象のlockstep。 */
        CLockstep& m_Lockstep;
    };

    /** transactional load用アダプターを返す。 */
    FPersistenceAdapter PersistenceAccess() noexcept
    {
        return FPersistenceAdapter(*this);
    }

private:
    /** 現在の動作モード。 */
    ENetMode           m_Mode          = ENetMode::Local;

    /** tick の周波数 (Hz)。 */
    u32               m_TickRateHz  = 60;

    /** 次に書き込む / 消費する tick。 */
    u32               m_CurrentTick  = 0;

    /** ConsumeInput の線形検索開始 index。 */
    u32               m_ReplayCursor = 0;

    /** 記録済み入力フレームの線形ストレージ (tick 昇順)。 */
    TArray<FInputFrame> m_Frames;
};

} // namespace acs::game

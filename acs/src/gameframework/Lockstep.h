// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar M — FLockstep (deterministic input replay)
//
// 役割:
//   1) **FLockstep ネットコード** の入力レイヤ: 1 tick = 1 フレーム分の入力を
//      `InputFrame` として記録 / 配信し、全クライアントが同一順序で入力を消費
//      することで決定論的なシミュレーションを実現する土台。
//   2) **入力リプレイ**: Local 中に記録した入力列をそのまま Replay モードで
//      再生し、replay ファイル / TAS / 自動テスト / バグ再現に使う。
//   3) **デターミニズム検証**: `ComputeChecksum()` で全 input frame の FNV-1a-like
//      hash を返し、replay 同期ずれ / 通信改竄 / プラットフォーム間 ABI ズレを
//      bit 単位で検出する。
//
// 使い方 (想定):
//   FLockstep ls;
//   ls.Init(ENetMode::Local, /*tick_rate_hz=*/60);
//
//   // ゲームループ (Local 中):
//   InputFrame f{ m_Tick, /*player_id=*/0, m_Buttons, m_Stick };
//   ls.RecordInput(f);
//   // ... シミュレーションに f を反映 ...
//   ++m_Tick;
//
//   // 後で Replay:
//   ls.StartReplay();
//   while (running) {
//       InputFrame f;
//       if (ls.ConsumeInput(m_Tick, 0, f)) {
//           // f.buttons / f.axis をシミュレーションに反映
//       }
//       ++m_Tick;
//   }
//
// 設計選択:
//   ・**InputFrame は trivially-copyable POD**: `u32 tick + u32 player_id +
//     u8 buttons + FVec2 axis` の小さな struct。padding 込みで 16〜20 B、TArray
//     にぎっしり詰める想定。
//   ・**TArray<InputFrame> による線形ストレージ**: 1 セッション分の入力は
//     60 Hz × 4 players × 30 min = 432,000 件 × ~20 B = ~8.6 MB 程度。
//     大規模 MMO ではなく対戦ゲーム / リプレイ用なのでこの容量で十分。
//   ・**ConsumeInput は線形検索**: `m_ReplayCursor` から
//     順に走査して `tick == requested_tick && player_id == requested_player_id`
//     を探す。記録順 = tick 昇順を仮定し、cursor を前進させることで amortised
//     O(1)。同 tick 内の player 順は問わない。
//   ・**ComputeChecksum は FNV-1a-like u64 hash**: 各 frame の (tick, player_id,
//     buttons, axis.x bits, axis.y bits) を u64 に畳み込む。決定論検証 + replay
//     hash として使う。FNV-1a は STL 不使用で実装が 5 行、衝突は同期ずれ検知用途
//     としては十分。
//   ・**コピー / ムーブ禁止**: FLockstep は通常 1 セッションに 1 個 (グローバル所有)
//     の長寿命オブジェクト。誤って値渡しされて state が分裂すると replay の
//     同期ずれが起きるため、FSettings / FPartySystem と同じく最初から非コピー・
//     非ムーブで固定する。
//   ・**全 noexcept**: ACS 全体方針。エラーは `TResult<T, FErrorCode>` で伝搬する。
//
// File layout (SaveToBuffer / LoadFromBuffer):
//   offset  size  field          説明
//   ------  ----  ------------   ---------------------------------------------
//   0x00    4     magic          'ACSL' = 0x4C534341 (little-endian)
//   0x04    4     version        u32。現状 = 1。
//   0x08    4     tick_rate_hz   u32。再生側との sample rate 整合検証用。
//   0x0C    4     frame_count    u32。続く InputFrame の個数。
//   0x10    N     frames         InputFrame[frame_count] (各 16〜20 B)。
//   0x10+N  4     crc32_footer   u32。frames 部の CRC-32 (改竄/破損検知用)。
//
// 範囲外:
//   ・per-player ring buffer / 差分圧縮 / delta encoding
//   ・rollback netcode (GGPO 風) — 本クラスは「全 input が確定済み」前提
//   ・loss compensation / desync recovery
//   ・複数 replay 同時再生 / 早送り / 巻き戻し
//   ・ネットワーク送受信 (NetTransport seam は別モジュールで切る)
#pragma once

#include "container/Array.h"
#include "foundation/Result.h"
#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

/**
 * 1 tick 分 (1 プレイヤー分) の入力を表す trivially-copyable な POD。
 *
 * @details
 * TArray に詰めて bulk memcpy で I/O できる。buttons は最大 8 ボタンの bitmask
 * (A/B/X/Y/L/R/Start/Select 等)、axis はアナログスティック 1 本分 (-1..1 想定だが
 * 本クラスは範囲チェックを行わない)。
 */
struct InputFrame {
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
 * FLockstep は同一クラスで「単独プレイ」「ネット対戦」「リプレイ再生」の 3 モードを
 * 扱う。モード切替時は state を Clear せず cursor だけリセットする (Local 中に記録した
 * frames を StartReplay で再生する用途を想定)。
 */
enum class ENetMode : u8 {
    /** 単独プレイ。入力を記録するが配信はしない。 */
    Local    = 0,

    /** ネット対戦。入力を記録し、リモートから受信した frame を取り込む。 */
    FLockstep = 1,

    /** 過去の入力列を再生する。RecordInput は受け付けない。 */
    Replay   = 2,
};

/**
 * 決定論的な入力レイヤ (lockstep ネットコード / 入力リプレイ)。
 *
 * @details
 * 1 tick = 1 フレーム分の入力を InputFrame として記録 / 再生し、全クライアントが
 * 同一順序で入力を消費することで決定論的シミュレーションの土台を作る。1 セッション
 * 1 オブジェクトの想定で、誤分裂を防ぐためコピー / ムーブを禁止する。
 */
class FLockstep {
public:
    /** 空の Local モード状態で構築する。 */
    FLockstep()  noexcept = default;

    /** 破棄する (記録した frames は TArray が解放)。 */
    ~FLockstep() noexcept = default;

    /** コピー禁止 (state 分裂による replay 同期ずれを防ぐため)。 */
    FLockstep(const FLockstep&)            = delete;

    /** コピー代入も禁止。 */
    FLockstep& operator=(const FLockstep&) = delete;

    /** ムーブ禁止 (長寿命の単独所有オブジェクトのため)。 */
    FLockstep(FLockstep&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FLockstep& operator=(FLockstep&&)      = delete;

    /**
     * 永続化 API が返すエラー subcode。
     *
     * @details 上位層が switch で分岐できるよう固定値を割り当てる (FSaveSlot と同じ規約)。
     */
    enum SubCode : u16 {
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
    void RecordInput(const InputFrame& frame) noexcept;

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
    bool ConsumeInput(u32 tick, u32 player_id, InputFrame& out) noexcept;

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
    TArray<InputFrame> m_Frames;
};

} // namespace acs::game

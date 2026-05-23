// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar M — Lockstep (deterministic input replay) スケルトン
//
// 役割:
//   1) **Lockstep ネットコード** の入力レイヤ: 1 tick = 1 フレーム分の入力を
//      `InputFrame` として記録 / 配信し、全クライアントが同一順序で入力を消費
//      することで決定論的なシミュレーションを実現する土台。
//   2) **入力リプレイ**: Local 中に記録した入力列をそのまま Replay モードで
//      再生し、replay ファイル / TAS / 自動テスト / バグ再現に使う。
//   3) **デターミニズム検証**: `ComputeChecksum()` で全 input frame の FNV-1a-like
//      hash を返し、replay 同期ずれ / 通信改竄 / プラットフォーム間 ABI ズレを
//      bit 単位で検出する。
//
// 使い方 (想定):
//   Lockstep ls;
//   ls.Init(ENetMode::Local, /*tick_rate_hz=*/60);
//
//   // ゲームループ (Local 中):
//   InputFrame f{ _tick, /*player_id=*/0, _buttons, _stick };
//   ls.RecordInput(f);
//   // ... シミュレーションに f を反映 ...
//   ++_tick;
//
//   // 後で Replay:
//   ls.StartReplay();
//   while (running) {
//       InputFrame f;
//       if (ls.ConsumeInput(_tick, 0, f)) {
//           // f.buttons / f.axis をシミュレーションに反映
//       }
//       ++_tick;
//   }
//
// 設計選択 (Phase M-1 スケルトン):
//   ・**InputFrame は trivially-copyable POD**: `u32 tick + u32 player_id +
//     u8 buttons + Vec2 axis` の小さな struct。padding 込みで 16〜20 B、Array
//     にぎっしり詰める想定。
//   ・**Array<InputFrame> による線形ストレージ**: 1 セッション分の入力は
//     60 Hz × 4 players × 30 min = 432,000 件 × ~20 B = ~8.6 MB 程度。
//     大規模 MMO ではなく対戦ゲーム / リプレイ用なのでこの容量で十分。
//     Phase M-2 で per-player ring buffer / 差分圧縮を検討する。
//   ・**ConsumeInput は線形検索**: Phase 1 スケルトンでは `_replay_cursor` から
//     順に走査して `tick == requested_tick && player_id == requested_player_id`
//     を探す。記録順 = tick 昇順を仮定し、cursor を前進させることで amortised
//     O(1)。同 tick 内の player 順は問わない。
//   ・**ComputeChecksum は FNV-1a-like u64 hash**: 各 frame の (tick, player_id,
//     buttons, axis.x bits, axis.y bits) を u64 に畳み込む。決定論検証 + replay
//     hash として使う。FNV-1a は STL 不使用で実装が 5 行、衝突は同期ずれ検知用途
//     としては十分。CRC-64 / xxHash3 は Phase M-2 で検討。
//   ・**SaveToBuffer / LoadFromBuffer は stub**: Phase 1 では magic + version +
//     count + frames の bit-precise layout 仕様だけ確定し、本体は ACS_ERR(IO,
//     kSub_NotImplemented) で返す (SaveSlot Phase 1 と同じ pattern)。Phase M-2 で
//     `acs::FileSystem::WriteAllBytes` + CRC-32 footer を入れる。
//   ・**コピー / ムーブ禁止**: Lockstep は通常 1 セッションに 1 個 (グローバル所有)
//     の長寿命オブジェクト。誤って値渡しされて state が分裂すると replay の
//     同期ずれが起きるため、Settings / PartySystem と同じく最初から非コピー・
//     非ムーブで固定する。
//   ・**全 noexcept**: ACS 全体方針。エラーは `Result<T, ErrorCode>` で伝搬する。
//
// File layout (SaveToBuffer / LoadFromBuffer の Phase M-2 target):
//   offset  size  field          説明
//   ------  ----  ------------   ---------------------------------------------
//   0x00    4     magic          'ACSL' = 0x4C534341 (little-endian)
//   0x04    4     version        u32。Phase M-2 開始時 = 1。
//   0x08    4     tick_rate_hz   u32。再生側との sample rate 整合検証用。
//   0x0C    4     frame_count    u32。続く InputFrame の個数。
//   0x10    N     frames         InputFrame[frame_count] (各 16〜20 B)。
//   0x10+N  4     crc32_footer   u32。frames 部の CRC-32 (改竄/破損検知用)。
//
// 範囲外 (Phase M-2 以降):
//   ・実 buffer I/O 接続 (現状 stub)
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

// =============================================================================
// InputFrame — 1 tick 分の入力 (1 プレイヤー分)
// -----------------------------------------------------------------------------
// trivially-copyable な POD。Array に詰めて bulk memcpy で I/O できる。
// `buttons` は最大 8 ボタンの bitmask (A/B/X/Y/L/R/Start/Select 等)。9 ボタン
// 以上を扱いたい場合は Phase M-2 で u16 / u32 に拡張する。`axis` はアナログ
// スティック 1 本分 (-1..1 想定だが本クラスは範囲チェックを行わない)。
// =============================================================================
struct InputFrame {
    u32  tick      = 0;      // フレーム番号 (0 起点、tick_rate_hz で時刻に変換)
    u32  player_id = 0;      // プレイヤー ID (0..N-1)
    u8   buttons   = 0;      // ボタン bitmask (8 ボタンまで)
    Vec2 axis      {};       // アナログスティック (-1..1 範囲想定)
};

// =============================================================================
// ENetMode — 入力レイヤの動作モード
// -----------------------------------------------------------------------------
// Lockstep は同一クラスで「単独プレイ」「ネット対戦」「リプレイ再生」の 3 モード
// を扱う。モード切替時は state を Clear せず、cursor だけリセットする (Local 中
// に記録した frames を StartReplay で再生する用途を想定)。
// =============================================================================
enum class ENetMode : u8 {
    Local    = 0,  // 単独プレイ / 入力を記録するが配信はしない
    Lockstep = 1,  // ネット対戦 / 入力を記録 + リモートから受信した frame を取り込む
    Replay   = 2,  // 過去の入力列を再生 / RecordInput は受け付けない
};

// =============================================================================
// Lockstep — 決定論的入力レイヤ
// -----------------------------------------------------------------------------
// 1 セッション 1 オブジェクトの想定。コピー / ムーブ禁止で誤分裂を防ぐ。
// =============================================================================
class Lockstep {
public:
    Lockstep()  noexcept = default;
    ~Lockstep() noexcept = default;

    Lockstep(const Lockstep&)            = delete;
    Lockstep& operator=(const Lockstep&) = delete;
    Lockstep(Lockstep&&)                 = delete;
    Lockstep& operator=(Lockstep&&)      = delete;

    // 共通エラー subcode (SaveSlot / BackendClient と同じ pattern)。
    // 上位層が switch で分岐できるよう enum 風に固定値を割り当てる。
    enum SubCode : u16 {
        kSub_NullBuffer     = 1,  // SaveToBuffer / LoadFromBuffer の buffer == nullptr
        kSub_BufferTooSmall = 2,  // SaveToBuffer: buffer.size が必要量未満
        kSub_BadMagic       = 3,  // LoadFromBuffer: magic 不一致
        kSub_BadVersion     = 4,  // LoadFromBuffer: version 不一致
        kSub_BadSize        = 5,  // LoadFromBuffer: frame_count とサイズが矛盾
        kSub_BadCrc         = 6,  // LoadFromBuffer: CRC mismatch
        kSub_NotImplemented = 99, // Phase 1 stub
    };

    // ----- 初期化 -----
    // mode を設定し、tick / cursor をリセットする。tick_rate_hz は replay の
    // sample rate 整合検証用 (ファイルに保存される)。既存 frames は破棄しない。
    void Init(ENetMode mode, u32 tick_rate_hz = 60) noexcept;

    // ----- 記録 -----
    // 入力 1 件を記録する。Replay モード中は no-op (記録しない)。
    // 内部 _current_tick は frame.tick + 1 に進める (連続 tick 想定)。
    void RecordInput(const InputFrame& frame) noexcept;

    // ----- 再生 -----
    // mode を Replay に切り替え、_replay_cursor = 0 / _current_tick = 0 にリセット。
    // 既存 frames は保持されたまま、ConsumeInput で先頭から取り出せる状態にする。
    void StartReplay() noexcept;

    // 指定 tick / player_id の入力を取り出す。Replay モード以外では false を返す。
    // 該当 frame が見つかれば out に書き込んで true、なければ out を変更せず false。
    // _replay_cursor を前進させ、線形検索を amortised O(1) にする。
    bool ConsumeInput(u32 tick, u32 player_id, InputFrame& out) noexcept;

    // ----- 状態 query -----
    u32 CurrentTick() const noexcept { return _current_tick; }
    u32 InputCount() const noexcept;
    ENetMode Mode() const noexcept { return _mode; }
    u32 TickRateHz() const noexcept { return _tick_rate_hz; }

    // 全 input frame の FNV-1a-like u64 hash。決定論検証 / replay 同期ずれ検知用。
    u64 ComputeChecksum() const noexcept;

    // 全 frames を破棄し、cursor / current_tick もリセットする。Mode は保持。
    void Clear() noexcept;

    // ----- 永続化 (Phase M-2 で実装) -----
    // SaveToBuffer: 現在の frames を上記 file layout で buffer に書き出す。
    //   out_written には実書き込みバイト数を返す (失敗時は 0)。
    //   Phase 1 stub: ACS_ERR(IO, kSub_NotImplemented) を返す。
    Result<void> SaveToBuffer(u8* buffer, u32 size, u32& out_written) noexcept;

    // LoadFromBuffer: buffer の内容を解釈して frames を復元する。
    //   呼び出し前に Clear() を呼ぶことを推奨 (本実装は append でなく置換予定)。
    //   Phase 1 stub: ACS_ERR(IO, kSub_NotImplemented) を返す。
    Result<void> LoadFromBuffer(const u8* buffer, u32 size) noexcept;

private:
    ENetMode           _mode          = ENetMode::Local;
    u32               _tick_rate_hz  = 60;     // sample rate (Hz)
    u32               _current_tick  = 0;      // 次に書き込む / 消費する tick
    u32               _replay_cursor = 0;      // ConsumeInput の線形検索開始 index
    Array<InputFrame> _frames;
};

} // namespace acs::game

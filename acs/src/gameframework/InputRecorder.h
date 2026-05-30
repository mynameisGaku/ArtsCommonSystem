// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar D — FInputRecorder (raw 入力の録画 / 再生) スケルトン
//
// 役割:
//   1) **raw 入力レイヤの録画**: キーボード状態変化 (8 key まで) + マウス座標 +
//      マウスボタン bitmask を 1 tick = 1 `InputSample` として時系列に蓄え、
//      後でそのまま replay 再生する。FLockstep が「ゲーム的に解釈済みの
//      ボタン bitmask + アナログスティック」を扱うのに対し、こちらは
//      **OS 寄りの raw 信号** (key code + key state + mouse pos) を扱う。
//   2) **TAS / 自動テスト / バグ再現用 `.acsr` 形式の入力ファイル**: Phase D-2 で
//      magic + version + tick_rate + count + samples + CRC32 の bit-precise
//      layout を確定し、SaveToBuffer / LoadFromBuffer で永続化する (Phase D-1
//      では stub)。FLockstep `.acsl` と並列の独立フォーマット。
//   3) **FInputMap の後段に置く想定**: アプリ側で `Capture(sample)` を呼ぶのは
//      Win32 raw input / SDL event を `InputSample` に詰めた直後。再生側は
//      `ConsumeSample(tick, out)` で取り出し、ゲームループは録画時と
//      ピクセル単位で同じ入力列を見ることになる。
//
// 使い方 (想定):
//   FInputRecorder rec;
//   rec.StartRecording(/*tick_rate_hz=*/60);
//
//   // ゲームループ (録画中):
//   InputSample s;
//   s.tick                = m_Tick;
//   s.key_codes_changed[] = ...; // 今 tick に押下/解放された key (最大 8 個)
//   s.key_states[]        = ...; // 同 index の press(1)/release(0) 状態
//   s.mouse_pos           = { mx, my };
//   s.mouse_button_states = mouse_bitmask;
//   rec.Capture(s);
//   ++m_Tick;
//
//   // 後で Replay:
//   rec.StartReplay();
//   while (running) {
//       InputSample s;
//       if (rec.ConsumeSample(m_Tick, s)) {
//           // s を OS 入力レイヤの代わりに使う (差し替え)
//       }
//       ++m_Tick;
//   }
//
// 設計選択 (Phase D-1 スケルトン):
//   ・**InputSample は trivially-copyable POD**: `u32 tick + u8[8] codes +
//     u8[8] states + FVec2 mouse_pos + u8 mouse_buttons + padding`。TArray に
//     詰めて bulk memcpy で I/O できるよう sizeof は 28 B 程度に収まる想定。
//     key 変化を 8 個までに絞ったのは fighting game / ARPG / RTS で同 tick に
//     9 個以上の key 変化が起きる頻度が事実上ゼロのため (10 finger 同時入力でも
//     状態変化点に限定すれば 8 で足りる)。9 個以上の場合は呼び出し側で 2 tick に
//     分割するか、Phase D-2 で variable-length encoding を導入する。
//   ・**ERecorderMode の 3 状態**: FLockstep の ENetMode (Local/FLockstep/Replay) と
//     異なり、こちらは「録画もしない・録画する・再生する」の 3 状態のみ。
//     ネットコードは FLockstep 側に分離してあるため、FInputRecorder は録画/再生に
//     特化する。
//   ・**TArray<InputSample> による線形ストレージ**: 60 Hz × 30 min = 108,000 件 ×
//     ~28 B = ~3 MB 程度。`.acsr` ファイルでもこのオーダー。長時間 TAS で
//     1 時間分でも ~6 MB なので Phase D-2 までは無圧縮で OK。
//   ・**ConsumeSample は線形検索 + cursor 前進**: 記録順 = tick 昇順を仮定し、
//     FLockstep と同じ amortised O(1) パターンを採用。
//   ・**SaveToBuffer / LoadFromBuffer は stub**: FLockstep と同じく Phase D-1 では
//     bit-precise file layout 仕様だけ確定し、本体は ACS_ERR(IO,
//     kSub_NotImplemented) を返す。
//   ・**コピー / ムーブ禁止**: FLockstep / FSettings / FPartySystem と同方針。
//     録画中の state が分裂すると再生時にズレるため、誤って値渡しされる経路を
//     最初から塞ぐ。
//   ・**全 noexcept**: ACS 全体方針。エラーは `TResult<T, FErrorCode>` で伝搬する。
//
// File layout (`.acsr` の SaveToBuffer / LoadFromBuffer の Phase D-2 target):
//   offset  size  field          説明
//   ------  ----  ------------   ---------------------------------------------
//   0x00    4     magic          'ACSR' = 0x52534341 (little-endian)
//   0x04    4     version        u32。Phase D-2 開始時 = 1。
//   0x08    4     tick_rate_hz   u32。再生側との sample rate 整合検証用。
//   0x0C    4     sample_count   u32。続く InputSample の個数。
//   0x10    N     samples        InputSample[sample_count] (各 ~28 B)。
//   0x10+N  4     crc32_footer   u32。samples 部の CRC-32 (改竄/破損検知用)。
//
// 範囲外 (Phase D-2 以降):
//   ・実 buffer I/O 接続 (現状 stub)
//   ・variable-length encoding (key 変化 9 個以上の tick への対応)
//   ・mouse wheel / gamepad analog stick の追加 (現状は key + mouse pos/button のみ)
//   ・複数 recorder 同時並行録画 / 早送り / 巻き戻し
//   ・差分圧縮 / RLE / zstd (Phase D-3 で検討)
#pragma once

#include "container/Array.h"
#include "foundation/Result.h"
#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

// =============================================================================
// InputSample — 1 tick 分の raw 入力サンプル
// -----------------------------------------------------------------------------
// trivially-copyable な POD。TArray に詰めて bulk memcpy で I/O できる。
// `key_codes_changed[i]` は今 tick に状態変化があった key code (Win32 VK_* 風 /
// SDL scancode 風 — 解釈はアプリ層に委ねる)、`key_states[i]` は同 index の
// press(1) / release(0) を表す。未使用スロットは key_codes_changed = 0 で埋める
// 規約 (key code 0 は VK_NULL = 「無効キー」相当)。最大 8 key までしか格納
// できないが、同一 tick 内で 9 個以上の状態変化が同時に起きるケースは事実上
// 発生しないので Phase D-1 では十分。
// =============================================================================
struct InputSample {
    u32  tick                 = 0;     // フレーム番号 (0 起点、tick_rate_hz で時刻に変換)
    u8   key_codes_changed[8] = {};    // 状態変化した key code (未使用 slot = 0)
    u8   key_states[8]        = {};    // 同 index の press(1) / release(0)
    FVec2 mouse_pos            {};      // マウス位置 (window-local px。座標系はアプリ側で統一)
    u8   mouse_button_states  = 0;     // マウスボタン bitmask (L=bit0, R=bit1, M=bit2, X1=bit3, X2=bit4)
};

// =============================================================================
// ERecorderMode — FInputRecorder の動作モード
// -----------------------------------------------------------------------------
// FLockstep の ENetMode と異なり、ネットコードは扱わず「録画もしない・録画する・
// 再生する」の 3 状態のみで完結する。モード切替時は state を Clear せず、
// cursor だけリセットする (録画した内容をそのまま StartReplay で再生する想定)。
// =============================================================================
enum class ERecorderMode : u8 {
    Idle      = 0,  // 録画も再生もしない (初期状態)
    Recording = 1,  // Capture() で sample を蓄積する
    Replaying = 2,  // ConsumeSample() で蓄積済み sample を取り出す
};

// =============================================================================
// FInputRecorder — raw 入力の録画 / 再生
// -----------------------------------------------------------------------------
// 1 セッション 1 オブジェクトの想定。コピー / ムーブ禁止で誤分裂を防ぐ。
// =============================================================================
class FInputRecorder {
public:
    FInputRecorder()  noexcept = default;
    ~FInputRecorder() noexcept = default;

    FInputRecorder(const FInputRecorder&)            = delete;
    FInputRecorder& operator=(const FInputRecorder&) = delete;
    FInputRecorder(FInputRecorder&&)                 = delete;
    FInputRecorder& operator=(FInputRecorder&&)      = delete;

    // 共通エラー subcode (FSaveSlot / FLockstep / FBackendClient と同じ pattern)。
    // 上位層が switch で分岐できるよう enum 風に固定値を割り当てる。
    enum SubCode : u16 {
        kSub_NullBuffer     = 1,  // SaveToBuffer / LoadFromBuffer の buffer == nullptr
        kSub_BufferTooSmall = 2,  // SaveToBuffer: buffer.size が必要量未満
        kSub_BadMagic       = 3,  // LoadFromBuffer: magic 不一致
        kSub_BadVersion     = 4,  // LoadFromBuffer: version 不一致
        kSub_BadSize        = 5,  // LoadFromBuffer: sample_count とサイズが矛盾
        kSub_BadCrc         = 6,  // LoadFromBuffer: CRC mismatch
        kSub_NotImplemented = 99, // Phase 1 stub
    };

    // ----- 録画開始 / 停止 -----
    // mode を Recording に切り替え、cursor / current_tick をリセットする。
    // tick_rate_hz は replay の sample rate 整合検証用 (ファイルに保存される)。
    // 既存 samples は破棄しない (続きから録画したい場合は Clear() と併用)。
    void StartRecording(u32 tick_rate_hz = 60) noexcept;

    // 録画を停止し、Idle 状態に戻す。samples / tick_rate_hz は保持。
    void StopRecording() noexcept;

    // ----- 再生開始 / 停止 -----
    // mode を Replaying に切り替え、cursor / current_tick = 0 にリセットする。
    // 既存 samples は保持し、ConsumeSample で先頭から取り出せる状態にする。
    void StartReplay() noexcept;

    // 再生を停止し、Idle 状態に戻す。samples / tick_rate_hz は保持。
    void StopReplay() noexcept;

    // ----- Capture / Consume -----
    // 1 sample を記録する。Recording モード以外では no-op (誤呼び出しを許容)。
    // 内部 m_CurrentTick は sample.tick + 1 に進める (連続 tick 想定)。
    void Capture(const InputSample& s) noexcept;

    // 指定 tick の sample を取り出す。Replaying モード以外では false を返す。
    // 該当 sample が見つかれば out に書き込んで true、なければ out を変更せず false。
    // m_Cursor を前進させ、線形検索を amortised O(1) にする。
    bool ConsumeSample(u32 tick, InputSample& out) noexcept;

    // ----- 状態 query -----
    ERecorderMode CurrentMode() const noexcept { return m_Mode; }
    u32          SampleCount() const noexcept;
    u32          CurrentTick() const noexcept { return m_CurrentTick; }
    u32          TickRateHz()  const noexcept { return m_TickRateHz; }

    // 全 samples を破棄し、cursor / current_tick をリセットする。Mode は保持。
    void Clear() noexcept;

    // ----- 永続化 (`.acsr` file layout で実装済み、round-trip 検証済み) -----
    // SaveToBuffer: 現在の samples を `.acsr` layout で buffer に書き出す。out_written に
    //   実書き込みバイト数を返す。buffer 不足は kSub_BufferTooSmall。samples 部に CRC32。
    TResult<void> SaveToBuffer(u8* buffer, u32 size, u32& out_written) noexcept;

    // LoadFromBuffer: buffer を解釈して samples を「置換」復元する
    //   (magic 'ACSR' / version / size / CRC32 を検証し、不正なら Err)。
    TResult<void> LoadFromBuffer(const u8* buffer, u32 size) noexcept;

private:
    ERecorderMode       m_Mode          = ERecorderMode::Idle;
    u32                m_TickRateHz  = 60;  // sample rate (Hz)
    u32                m_CurrentTick  = 0;   // 次に書き込む / 消費する tick
    u32                m_Cursor        = 0;   // ConsumeSample の線形検索開始 index
    TArray<InputSample> m_Samples;
};

} // namespace acs::game

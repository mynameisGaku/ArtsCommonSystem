// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar D — FInputRecorder 実装 (Phase D-1 スケルトン)
//
// 現フェーズ:
//   ・StartRecording / StopRecording / StartReplay / StopReplay /
//     Capture / ConsumeSample / SampleCount / Clear はすべて本実装。
//   ・SaveToBuffer / LoadFromBuffer は ACS_ERR(IO, kSub_NotImplemented) を返す
//     stub (FLockstep Phase M-1 / FSaveSlot Phase 1 と同じ pattern)。Phase D-2 で
//     `.acsr` の bit-precise file layout に対応する実 I/O を接続予定。
//
// 設計メモ:
//   ・Mode 遷移は明示的: Idle → (StartRecording) → Recording → (StopRecording) → Idle
//                       Idle → (StartReplay)    → Replaying → (StopReplay)    → Idle
//     Recording 中に StartReplay を呼ぶと黙って Replaying に切り替わる (cursor
//     はリセット)。逆も同様。録画と再生が同時並行することはない。
//   ・ConsumeSample は m_Cursor から線形走査。記録順 = tick 昇順を仮定すれば
//     cursor 前進で amortised O(1)。FLockstep::ConsumeInput と同じ pattern。
//   ・key_codes_changed / key_states は struct 定義時にデフォルト初期化済みなので
//     Capture 側は値コピーのみ。InputSample の sizeof は環境依存 (FVec2 = 8 B,
//     padding 込みで 28〜32 B) だが、本クラスは内部で sizeof に依存しないため
//     ABI 変動に耐える。Phase D-2 の file I/O 時は明示的に field を 1 つずつ
//     little-endian で書き出す予定 (`InputSample` 自体の memcpy には頼らない)。
//   ・Phase D-2 で SaveToBuffer / LoadFromBuffer を実装。`.acsr` の on-disk
//     layout は header 16B + samples (1 sample = 29B、field 単位 LE) + footer 4B。
//     1 sample = tick(4) + key_codes_changed(8) + key_states(8) + mouse_pos.x(4) +
//     mouse_pos.y(4) + mouse_button_states(1) = 29B。CRC32 は samples 部のみを
//     対象に計算し footer に置く (FSaveArchive / assetpack と同一の
//     poly 0xEDB88320 実装)。InputSample 自体の memcpy には頼らず、ABI 非依存。
#include "gameframework/InputRecorder.h"

#include "memory/Memory.h"  // MemCopy

namespace acs::game {

namespace {

// -----------------------------------------------------------------------------
// `.acsr` on-disk フォーマット定数
// -----------------------------------------------------------------------------
// magic 'ACSR' = 0x52534341 (little-endian)。header.h の File layout 節と一致。
constexpr u32 kMagic        = 0x52534341u;  // 'A''C''S''R' を LE u32 に詰めた値
constexpr u32 kVersion      = 1u;           // Phase D-2 開始時 = 1
constexpr u32 kHeaderSize   = 16u;          // magic(4)+version(4)+tick_rate(4)+count(4)
constexpr u32 kFooterSize   = 4u;           // crc32(4)
// 1 sample の on-disk バイト数: tick(4)+codes(8)+states(8)+mx(4)+my(4)+buttons(1)。
// InputSample の sizeof (padding 込み) には依存しない明示レイアウト。
constexpr u32 kSampleOnDisk = 29u;

// -----------------------------------------------------------------------------
// little-endian 読み書き helper (strict-aliasing 安全)
// -----------------------------------------------------------------------------
// ホスト側 (Win x64 / ARM64 LE) 前提。FSaveArchive.cpp / FNetSnapshot.cpp と同流儀。
inline void WriteU32LE(u8* dst, u32 v) noexcept { MemCopy(dst, &v, sizeof(u32)); }
inline u32  ReadU32LE (const u8* src) noexcept { u32 v = 0; MemCopy(&v, src, sizeof(u32)); return v; }

// f32 のビットパターンを u32 として取り出す / 書き戻す (memcpy で aliasing 回避)。
inline u32 BitsOfF32(f32 v) noexcept { u32 out = 0; MemCopy(&out, &v, sizeof(u32)); return out; }
inline f32 F32FromBits(u32 v) noexcept { f32 out = 0.0f; MemCopy(&out, &v, sizeof(f32)); return out; }

// -----------------------------------------------------------------------------
// CRC32 (poly 0xEDB88320, init/xorout 0xFFFFFFFF)
// -----------------------------------------------------------------------------
// FSaveArchive.cpp / assetpack と同一実装。あちらの ComputeCrc32 は anonymous
// namespace に閉じていて外から link できないため、ここでも単独に持つ
// (link 単位を独立させ、SaveArchive を依存に持たなくても InputRecorder が
// 単体で使えるようにする)。Meyer's singleton で table を thread-safe に初期化。
const u32* GetCrc32Table() noexcept {
    static u32 table[256] = {};
    static bool initialized = false;
    if (!initialized) {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (u32 k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        initialized = true;
    }
    return table;
}

u32 ComputeCrc32(const void* data, u64 size) noexcept {
    const u32* table = GetCrc32Table();
    const u8*  p     = static_cast<const u8*>(data);
    u32        crc   = 0xFFFFFFFFu;
    for (u64 i = 0; i < size; ++i) {
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

// 1 sample を on-disk LE フォーマット (kSampleOnDisk バイト) に書き出す。
void WriteSample(u8* dst, const InputSample& s) noexcept {
    WriteU32LE(dst + 0, s.tick);
    MemCopy(dst + 4,  s.key_codes_changed, 8);
    MemCopy(dst + 12, s.key_states,        8);
    WriteU32LE(dst + 20, BitsOfF32(s.mouse_pos.x));
    WriteU32LE(dst + 24, BitsOfF32(s.mouse_pos.y));
    dst[28] = s.mouse_button_states;
}

// 1 sample を on-disk LE フォーマット (kSampleOnDisk バイト) から復元する。
void ReadSample(const u8* src, InputSample& out) noexcept {
    out.tick = ReadU32LE(src + 0);
    MemCopy(out.key_codes_changed, src + 4,  8);
    MemCopy(out.key_states,        src + 12, 8);
    out.mouse_pos.x          = F32FromBits(ReadU32LE(src + 20));
    out.mouse_pos.y          = F32FromBits(ReadU32LE(src + 24));
    out.mouse_button_states  = src[28];
}

} // namespace

// -----------------------------------------------------------------------------
// 録画開始 / 停止
// -----------------------------------------------------------------------------

void FInputRecorder::StartRecording(u32 tick_rate_hz) noexcept {
    m_Mode = ERecorderMode::Recording;
    // tick_rate_hz == 0 は意味を成さないので最低 1 に丸める。0 除算防止 +
    // FLockstep::Init と同じ規約。
    m_TickRateHz = (tick_rate_hz == 0) ? 1u : tick_rate_hz;
    m_CurrentTick = 0;
    m_Cursor       = 0;
    // 既存 samples は破棄しない (続きから録画したい場合の許容)。
    // 完全リセットしたい場合は呼び出し側で Clear() を併用する。
}

void FInputRecorder::StopRecording() noexcept {
    // Idle に戻すだけで samples / tick_rate_hz は保持。直後に SaveToBuffer を
    // 呼ぶ想定。Capture/ConsumeSample 双方の no-op 条件を兼ねる。
    m_Mode = ERecorderMode::Idle;
}

// -----------------------------------------------------------------------------
// 再生開始 / 停止
// -----------------------------------------------------------------------------

void FInputRecorder::StartReplay() noexcept {
    m_Mode         = ERecorderMode::Replaying;
    m_Cursor       = 0;
    m_CurrentTick = 0;
    // m_Samples はそのまま。直前に Capture した内容を頭から再生する想定。
}

void FInputRecorder::StopReplay() noexcept {
    // Idle に戻すだけ。再度 StartReplay すれば cursor は 0 から再開する。
    m_Mode = ERecorderMode::Idle;
}

// -----------------------------------------------------------------------------
// Capture (録画中の入力受け取り)
// -----------------------------------------------------------------------------

void FInputRecorder::Capture(const InputSample& s) noexcept {
    // Recording モード以外では記録しない (Idle / Replaying 中の誤呼び出しを許容)。
    // 黙って no-op にする理由: ゲームループから無条件に Capture を呼べる設計に
    // しておくと、上位層の if 分岐が不要になり録画開始/停止だけで切り替えできる。
    if (m_Mode != ERecorderMode::Recording) {
        return;
    }
    m_Samples.PushBack(s);
    // m_CurrentTick は「次に書き込む tick」のヒント。連続 tick 想定で
    // sample.tick + 1 に進める。FLockstep::RecordInput と同じ方針。
    m_CurrentTick = s.tick + 1u;
}

// -----------------------------------------------------------------------------
// ConsumeSample (再生中の入力取り出し)
// -----------------------------------------------------------------------------

bool FInputRecorder::ConsumeSample(u32 tick, InputSample& out) noexcept {
    // Replaying モード以外では取り出しを禁止する (誤用検知)。
    if (m_Mode != ERecorderMode::Replaying) {
        return false;
    }
    const usize n = m_Samples.Size();
    // m_Cursor から線形走査。記録順 = tick 昇順を仮定するため、
    // ヒット後に cursor を前進させて amortised O(1) を狙う。
    for (usize i = m_Cursor; i < n; ++i) {
        const InputSample& s = m_Samples[i];
        if (s.tick == tick) {
            out = s;
            // 次回検索開始位置を更新。FLockstep と異なり同 tick 内に複数 sample が
            // 入る想定はない (1 tick = 1 sample = 1 raw input snapshot) ため、
            // 単純に i + 1 を採用する。
            m_Cursor       = static_cast<u32>(i + 1u);
            m_CurrentTick = tick + 1u;
            return true;
        }
        // 記録順が tick 昇順を破る場合 (rollback / 未来 sample の混入) は素朴に
        // 走査を続ける。Phase D-2 で per-tick index を持つことで高速化予定。
    }
    return false;
}

// -----------------------------------------------------------------------------
// 状態 query / リセット
// -----------------------------------------------------------------------------

u32 FInputRecorder::SampleCount() const noexcept {
    return static_cast<u32>(m_Samples.Size());
}

void FInputRecorder::Clear() noexcept {
    m_Samples.Clear();
    m_CurrentTick = 0;
    m_Cursor       = 0;
    // m_Mode / m_TickRateHz は保持。StartRecording / StartReplay で改めて切り替える設計。
}

// -----------------------------------------------------------------------------
// SaveToBuffer — `.acsr` layout で現在の samples を buffer に書き出す
// -----------------------------------------------------------------------------
// layout: [magic][version][tick_rate_hz][sample_count][samples...][crc32]
//   1. required = 16 (header) + sample_count * 29 (samples) + 4 (footer)
//   2. size < required なら kSub_BufferTooSmall
//   3. write [magic='ACSR'][version=1][tick_rate_hz][sample_count]
//   4. write samples (field 単位で LE エンコード、1 sample = 29B)
//   5. write crc32(samples) footer
//   6. out_written = required
TResult<void> FInputRecorder::SaveToBuffer(u8* buffer, u32 size, u32& out_written) noexcept {
    out_written = 0;
    if (buffer == nullptr) {
        return ACS_ERR(IO, kSub_NullBuffer,
                       "FInputRecorder::SaveToBuffer: buffer is null");
    }

    const u32 sample_count = static_cast<u32>(m_Samples.Size());
    // 必要バイト数 = header + samples + footer。u64 で計算して overflow を避ける。
    const u64 required64 =
        static_cast<u64>(kHeaderSize) +
        static_cast<u64>(sample_count) * static_cast<u64>(kSampleOnDisk) +
        static_cast<u64>(kFooterSize);
    if (required64 > static_cast<u64>(size)) {
        return ACS_ERR(IO, kSub_BufferTooSmall,
                       "FInputRecorder::SaveToBuffer: buffer too small for samples");
    }
    const u32 required = static_cast<u32>(required64);

    // ---- header (16B) -------------------------------------------------------
    WriteU32LE(buffer + 0,  kMagic);
    WriteU32LE(buffer + 4,  kVersion);
    WriteU32LE(buffer + 8,  m_TickRateHz);
    WriteU32LE(buffer + 12, sample_count);

    // ---- samples (field 単位 LE。1 sample = kSampleOnDisk バイト) -----------
    u8* samples_begin = buffer + kHeaderSize;
    for (u32 i = 0; i < sample_count; ++i) {
        WriteSample(samples_begin + static_cast<u64>(i) * kSampleOnDisk, m_Samples[i]);
    }

    // ---- crc32 footer (samples 部のみを対象) --------------------------------
    const u64 samples_bytes = static_cast<u64>(sample_count) * kSampleOnDisk;
    const u32 crc = ComputeCrc32(samples_begin, samples_bytes);
    WriteU32LE(samples_begin + samples_bytes, crc);

    out_written = required;
    return Ok();
}

// -----------------------------------------------------------------------------
// LoadFromBuffer — `.acsr` buffer を検証して samples を復元する
// -----------------------------------------------------------------------------
// layout: [magic][version][tick_rate_hz][sample_count][samples...][crc32]
//   1. size < 20 (header 16 + footer 4 = 最小) なら kSub_BadSize
//   2. magic 検証 → 'ACSR' と不一致なら kSub_BadMagic
//   3. version 検証 → kVersion と不一致なら kSub_BadVersion
//   4. sample_count * 29 + 20 が size と一致しなければ kSub_BadSize
//   5. crc32 計算 → footer と不一致なら kSub_BadCrc
//   6. m_Samples.Clear(); m_Samples.Reserve(sample_count); 順次 PushBack
//   7. m_TickRateHz は header の値で更新、m_CurrentTick / m_Cursor を 0 に
//      リセット (StartReplay 待ち状態)
TResult<void> FInputRecorder::LoadFromBuffer(const u8* buffer, u32 size) noexcept {
    if (buffer == nullptr) {
        return ACS_ERR(IO, kSub_NullBuffer,
                       "FInputRecorder::LoadFromBuffer: buffer is null");
    }
    // header + footer 最小サイズ。これ未満は magic すら読めない。
    if (size < kHeaderSize + kFooterSize) {
        return ACS_ERR(IO, kSub_BadSize,
                       "FInputRecorder::LoadFromBuffer: buffer smaller than header+footer");
    }

    // ---- header parse -------------------------------------------------------
    const u32 magic        = ReadU32LE(buffer + 0);
    if (magic != kMagic) {
        return ACS_ERR(IO, kSub_BadMagic,
                       "FInputRecorder::LoadFromBuffer: magic mismatch (not an .acsr)");
    }
    const u32 version      = ReadU32LE(buffer + 4);
    if (version != kVersion) {
        return ACS_ERR(IO, kSub_BadVersion,
                       "FInputRecorder::LoadFromBuffer: version mismatch");
    }
    const u32 tick_rate_hz = ReadU32LE(buffer + 8);
    const u32 sample_count = ReadU32LE(buffer + 12);

    // ---- sample_count とサイズの整合検証 (overflow 安全に u64 で) ------------
    const u64 expected64 =
        static_cast<u64>(kHeaderSize) +
        static_cast<u64>(sample_count) * static_cast<u64>(kSampleOnDisk) +
        static_cast<u64>(kFooterSize);
    if (expected64 != static_cast<u64>(size)) {
        return ACS_ERR(IO, kSub_BadSize,
                       "FInputRecorder::LoadFromBuffer: sample_count inconsistent with size");
    }

    // ---- crc32 検証 (samples 部のみを対象) ----------------------------------
    const u8* samples_begin = buffer + kHeaderSize;
    const u64 samples_bytes = static_cast<u64>(sample_count) * kSampleOnDisk;
    const u32 expected_crc  = ReadU32LE(samples_begin + samples_bytes);
    const u32 actual_crc    = ComputeCrc32(samples_begin, samples_bytes);
    if (actual_crc != expected_crc) {
        return ACS_ERR(IO, kSub_BadCrc,
                       "FInputRecorder::LoadFromBuffer: CRC32 mismatch (corrupt or tampered)");
    }

    // ---- samples を復元 (append でなく置換) ---------------------------------
    m_Samples.Clear();
    m_Samples.Reserve(static_cast<usize>(sample_count));
    for (u32 i = 0; i < sample_count; ++i) {
        InputSample s;
        ReadSample(samples_begin + static_cast<u64>(i) * kSampleOnDisk, s);
        m_Samples.PushBack(s);
    }

    // ---- 再生待ち状態へ。tick_rate は file 由来の値で上書きする -------------
    m_TickRateHz  = (tick_rate_hz == 0) ? 1u : tick_rate_hz;
    m_CurrentTick = 0;
    m_Cursor      = 0;
    return Ok();
}

} // namespace acs::game

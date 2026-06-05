// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar D — FInputRecorder 実装
//
// StartRecording / StopRecording / StartReplay / StopReplay /
// Capture / ConsumeSample / SampleCount / Clear / SaveToBuffer / LoadFromBuffer
// をすべて本実装する。SaveToBuffer / LoadFromBuffer は `.acsr` の bit-precise
// file layout に対応する実 I/O。
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
//     ABI 変動に耐える。file I/O 時は明示的に field を 1 つずつ
//     little-endian で書き出す (`InputSample` 自体の memcpy には頼らない)。
//   ・SaveToBuffer / LoadFromBuffer の `.acsr` on-disk
//     layout は header 16B + samples (1 sample = 29B、field 単位 LE) + footer 4B。
//     1 sample = tick(4) + key_codes_changed(8) + key_states(8) + mouse_pos.x(4) +
//     mouse_pos.y(4) + mouse_button_states(1) = 29B。CRC32 は samples 部のみを
//     対象に計算し footer に置く (FSaveArchive / assetpack と同一の
//     poly 0xEDB88320 実装)。InputSample 自体の memcpy には頼らず、ABI 非依存。
#include "gameframework/InputRecorder.h"

#include "memory/Memory.h"  // MemCopy

namespace acs::game {

namespace {

/** `.acsr` magic ('ACSR' を little-endian u32 に詰めた値)。 */
constexpr u32 kMagic        = 0x52534341u;

/** `.acsr` フォーマットのバージョン番号。 */
constexpr u32 kVersion      = 1u;

/** header のバイト数 (magic + version + tick_rate + count、各 4B)。 */
constexpr u32 kHeaderSize   = 16u;

/** footer のバイト数 (crc32、4B)。 */
constexpr u32 kFooterSize   = 4u;

/**
 * 1 sample の on-disk バイト数。
 *
 * @details tick(4) + codes(8) + states(8) + mx(4) + my(4) + buttons(1) の明示
 * レイアウト。InputSample の sizeof (padding 込み) には依存しない。
 */
constexpr u32 kSampleOnDisk = 29u;

/**
 * u32 を little-endian で書き出す (strict-aliasing 安全)。
 *
 * @param dst 書き込み先 (4B 以上)。
 * @param v 書き出す値。
 */
inline void WriteU32LE(u8* dst, u32 v) noexcept { MemCopy(dst, &v, sizeof(u32)); }

/**
 * little-endian の u32 を読み出す (strict-aliasing 安全)。
 *
 * @param src 読み込み元 (4B 以上)。
 * @return 読み出した u32 値。
 */
inline u32  ReadU32LE (const u8* src) noexcept { u32 v = 0; MemCopy(&v, src, sizeof(u32)); return v; }

/**
 * f32 のビットパターンを u32 として取り出す (memcpy で aliasing 回避)。
 *
 * @param v ビット取り出し対象の f32。
 * @return v のビットパターンを表す u32。
 */
inline u32 BitsOfF32(f32 v) noexcept { u32 out = 0; MemCopy(&out, &v, sizeof(u32)); return out; }

/**
 * u32 のビットパターンを f32 として復元する (memcpy で aliasing 回避)。
 *
 * @param v ビットパターンを表す u32。
 * @return 復元した f32。
 */
inline f32 F32FromBits(u32 v) noexcept { f32 out = 0.0f; MemCopy(&out, &v, sizeof(f32)); return out; }

/**
 * CRC32 計算テーブルを返す (poly 0xEDB88320、Meyer's singleton で thread-safe 初期化)。
 *
 * @details
 * FSaveArchive.cpp / assetpack と同一実装。あちらの table は anonymous namespace に
 * 閉じていて外から link できないため、InputRecorder が単体で使えるよう独立に持つ。
 * @return 256 要素の CRC32 テーブルへのポインタ。
 */
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

/**
 * バイト列の CRC32 を計算する (poly 0xEDB88320、init/xorout 0xFFFFFFFF)。
 *
 * @param data 対象バイト列の先頭。
 * @param size 対象バイト数。
 * @return 計算した CRC32 値。
 */
u32 ComputeCrc32(const void* data, u64 size) noexcept {
    const u32* table = GetCrc32Table();
    const u8*  p     = static_cast<const u8*>(data);
    u32        crc   = 0xFFFFFFFFu;
    for (u64 i = 0; i < size; ++i) {
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/**
 * 1 sample を on-disk の LE フォーマット (kSampleOnDisk バイト) に書き出す。
 *
 * @param dst 書き込み先 (kSampleOnDisk バイト以上)。
 * @param s 書き出す入力サンプル。
 */
void WriteSample(u8* dst, const InputSample& s) noexcept {
    WriteU32LE(dst + 0, s.tick);
    MemCopy(dst + 4,  s.key_codes_changed, 8);
    MemCopy(dst + 12, s.key_states,        8);
    WriteU32LE(dst + 20, BitsOfF32(s.mouse_pos.x));
    WriteU32LE(dst + 24, BitsOfF32(s.mouse_pos.y));
    dst[28] = s.mouse_button_states;
}

/**
 * on-disk の LE フォーマット (kSampleOnDisk バイト) から 1 sample を復元する。
 *
 * @param src 読み込み元 (kSampleOnDisk バイト以上)。
 * @param out 復元した入力サンプルの書き込み先。
 */
void ReadSample(const u8* src, InputSample& out) noexcept {
    out.tick = ReadU32LE(src + 0);
    MemCopy(out.key_codes_changed, src + 4,  8);
    MemCopy(out.key_states,        src + 12, 8);
    out.mouse_pos.x          = F32FromBits(ReadU32LE(src + 20));
    out.mouse_pos.y          = F32FromBits(ReadU32LE(src + 24));
    out.mouse_button_states  = src[28];
}

} // namespace

/** Recording モードへ切り替え、tick / cursor をリセットする (既存 samples は保持)。 */
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

/** Idle へ戻す (samples / tick_rate_hz は保持)。 */
void FInputRecorder::StopRecording() noexcept {
    // Idle に戻すだけで samples / tick_rate_hz は保持。直後に SaveToBuffer を
    // 呼ぶ想定。Capture/ConsumeSample 双方の no-op 条件を兼ねる。
    m_Mode = ERecorderMode::Idle;
}

/** Replaying モードへ切り替え、cursor / current_tick を 0 にする (samples は保持)。 */
void FInputRecorder::StartReplay() noexcept {
    m_Mode         = ERecorderMode::Replaying;
    m_Cursor       = 0;
    m_CurrentTick = 0;
    // m_Samples はそのまま。直前に Capture した内容を頭から再生する想定。
}

/** Idle へ戻す (再度 StartReplay すれば cursor は 0 から再開)。 */
void FInputRecorder::StopReplay() noexcept {
    // Idle に戻すだけ。再度 StartReplay すれば cursor は 0 から再開する。
    m_Mode = ERecorderMode::Idle;
}

/** Recording 中のみ sample を末尾に蓄積し、current_tick を s.tick + 1 へ進める。 */
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

/** Replaying 中に cursor から線形走査し、指定 tick の sample を取り出す (amortised O(1))。 */
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
        // 走査を続ける。per-tick index を持てば高速化できる。
    }
    return false;
}

/** 蓄積済み sample 数を返す。 */
u32 FInputRecorder::SampleCount() const noexcept {
    return static_cast<u32>(m_Samples.Size());
}

/** 全 sample を破棄し cursor / current_tick を 0 に戻す (Mode / tick_rate は保持)。 */
void FInputRecorder::Clear() noexcept {
    m_Samples.Clear();
    m_CurrentTick = 0;
    m_Cursor       = 0;
    // m_Mode / m_TickRateHz は保持。StartRecording / StartReplay で改めて切り替える設計。
}

/**
 * 現在の samples を `.acsr` layout で buffer に書き出す。
 *
 * @details
 * layout: [magic][version][tick_rate_hz][sample_count][samples...][crc32]。必要量は
 * 16 (header) + sample_count * 29 (samples) + 4 (footer)。header → samples (field 単位
 * LE) → samples 部の crc32 footer の順に書き出す。
 * @param buffer 書き込み先バッファ。
 * @param size buffer の容量 (バイト)。
 * @param out_written 実際に書き込んだバイト数の書き込み先。
 * @return 成功なら Ok。buffer が null なら kSub_NullBuffer、容量不足なら kSub_BufferTooSmall。
 */
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

/**
 * `.acsr` buffer を検証して samples を「置換」復元する。
 *
 * @details
 * layout: [magic][version][tick_rate_hz][sample_count][samples...][crc32]。header の
 * magic / version、sample_count とサイズの整合、samples 部の crc32 を順に検証し、すべて
 * 通れば m_Samples を Clear → Reserve → 復元する。完了後は tick_rate を file の値で更新し
 * current_tick / cursor を 0 にリセット (StartReplay 待ち状態)。
 * @param buffer 読み込み元バッファ。
 * @param size buffer のバイト数。
 * @return 成功なら Ok。null は kSub_NullBuffer、サイズ不整合は kSub_BadSize、magic /
 *         version / crc 不一致はそれぞれ kSub_BadMagic / kSub_BadVersion / kSub_BadCrc。
 */
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

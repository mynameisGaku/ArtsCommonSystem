// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar M — FLockstep 実装
//
// 設計メモ:
//   ・ConsumeInput は m_ReplayCursor から線形走査する。記録順 = tick 昇順を
//     仮定すれば、cursor を前進させることで amortised O(1)。同 tick 内の
//     player_id 順は問わない (4 人対戦なら 4 frame だけ走査する)。
//   ・ComputeChecksum は FNV-1a-like u64。OFFSET_BASIS = 0xCBF29CE484222325,
//     PRIME = 0x100000001B3。各 InputFrame の bit パターンを 8 バイト境界で
//     消費する単純実装。FVec2 の f32 ビットは memcpy 経由で u32 に読み替え
//     (strict aliasing 違反回避)。
//   ・FNV を選んだ理由: STL 不使用 + ヘッダ追加なしで 5 行で書ける。
//     replay 同期ずれ検知 (1〜2 bit 差分が大半) には十分な avalanche を持つ。
#include "gameframework/Lockstep.h"

#include "foundation/Move.h"
#include "memory/Memory.h"   // MemCopy

namespace acs::game {

namespace {

/** FNV-1a 64-bit の offset basis。 */
constexpr u64 kFnvOffsetBasis = 0xCBF29CE484222325ull;

/** FNV-1a 64-bit の prime。 */
constexpr u64 kFnvPrime       = 0x100000001B3ull;

/**
 * SaveToBuffer / LoadFromBuffer のマジック 'ACSL'。
 *
 * @details
 * 永続化レイアウト (すべて little-endian) は
 * [magic][version][tick_rate_hz][frame_count][frames...][crc32]。crc32 は frames 部
 * (header と footer を除いた本体) のみを対象に計算する。InputFrame は padding を含む POD
 * なので生 memcpy はせず固定幅 wire layout で書き出し、ABI / padding 非依存の bit-precise
 * な永続化にする (デターミニズム検証用途で再現性が最重要)。
 */
constexpr u8  kMagic[4]       = { 'A', 'C', 'S', 'L' };

/** wire フォーマットのバージョン。 */
constexpr u32 kFormatVersion  = 1u;

/** ヘッダ部の byte 数 (magic 4 + version 4 + tick_rate_hz 4 + frame_count 4)。 */
constexpr u32 kHeaderSize     = 16u;

/** 1 フレームの wire byte 数 (tick 4 + player_id 4 + buttons 1 + axis.x 4 + axis.y 4)。 */
constexpr u32 kFrameWireSize  = 17u;

/** フッタ部の byte 数 (crc32)。 */
constexpr u32 kFooterSize     = 4u;

/**
 * CRC32 lookup table を遅延初期化して返す (poly 0xEDB88320, init/xorout 0xFFFFFFFF)。
 *
 * @details
 * FSaveArchive / assetpack と同一実装 (link 単位を独立させるためここでも単独に持つ)。
 * Meyer's singleton で thread-safe に table を初期化する。
 * @return 256 要素の CRC32 lookup table。
 */
const u32* GetCrc32Table() noexcept
{
    struct FCrc32Table {
        FCrc32Table() noexcept
        {
            for (u32 Index = 0; Index < 256; ++Index) {
                u32 Value = Index;
                for (u32 Bit = 0; Bit < 8; ++Bit) {
                    Value = (Value & 1u) ? (0xEDB88320u ^ (Value >> 1)) : (Value >> 1);
                }
                Values[Index] = Value;
            }
        }

        u32 Values[256] = {};
    };

    static const FCrc32Table Table;
    return Table.Values;
}

/**
 * バイト列の CRC32 を計算する (Zlib / PNG 規約)。
 *
 * @param data 計算対象の先頭ポインタ。
 * @param size バイト数。
 * @return CRC32 値。
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
 * u32 を little-endian バイト列として書き込む (strict-aliasing 安全)。
 *
 * @details FSaveArchive と同じく MemCopy 経由でバイトコピーする。ホスト側も LE 前提。
 * @param dst 書き込み先 (4 byte)。
 * @param v 書き込む値。
 */
void WriteU32LE(u8* dst, u32 v) noexcept { MemCopy(dst, &v, sizeof(u32)); }

/**
 * little-endian バイト列から u32 を読み出す (strict-aliasing 安全)。
 *
 * @param src 読み込み元 (4 byte)。
 * @return 読み出した u32 値。
 */
u32 ReadU32LE(const u8* src) noexcept {
    u32 v = 0; MemCopy(&v, src, sizeof(u32)); return v;
}

/**
 * 1 バイトを FNV-1a で混ぜる。
 *
 * @param h 現在のハッシュ値。
 * @param byte 混ぜるバイト。
 * @return 更新後のハッシュ値。
 */
inline u64 FnvFold(u64 h, u8 byte) noexcept {
    h ^= static_cast<u64>(byte);
    h *= kFnvPrime;
    return h;
}

/**
 * u32 を little-endian バイト列として FNV-1a で混ぜる。
 *
 * @param h 現在のハッシュ値。
 * @param v 混ぜる u32 値。
 * @return 更新後のハッシュ値。
 */
inline u64 FnvFoldU32(u64 h, u32 v) noexcept {
    h = FnvFold(h, static_cast<u8>(v        & 0xFFu));
    h = FnvFold(h, static_cast<u8>((v >> 8 ) & 0xFFu));
    h = FnvFold(h, static_cast<u8>((v >> 16) & 0xFFu));
    h = FnvFold(h, static_cast<u8>((v >> 24) & 0xFFu));
    return h;
}

/**
 * f32 のビットパターンを u32 として取り出す。
 *
 * @details
 * reinterpret_cast や union 経由は strict aliasing 違反になるため、バイト単位の小ループで
 * 読み出す (STL 不使用方針)。
 * @param v ビットを取り出す f32 値。
 * @return v のビットパターンを表す u32。
 */
inline u32 BitsOf(f32 v) noexcept {
    u32 out = 0;
    const u8* src = reinterpret_cast<const u8*>(&v);
    out |= static_cast<u32>(src[0]);
    out |= static_cast<u32>(src[1]) << 8;
    out |= static_cast<u32>(src[2]) << 16;
    out |= static_cast<u32>(src[3]) << 24;
    return out;
}

/** NaN と正負 infinity を拒否する IEEE-754 binary32 の有限値判定。 */
inline bool IsFiniteF32Bits(u32 bits) noexcept {
    return (bits & 0x7F800000u) != 0x7F800000u;
}

/**
 * u32 ビットパターンを f32 に読み替える (BitsOf の逆、strict-aliasing 安全)。
 *
 * @param bits f32 のビットパターン。
 * @return 読み替えた f32 値。
 */
inline f32 F32FromBits(u32 bits) noexcept {
    f32 out = 0.0f;
    MemCopy(&out, &bits, sizeof(f32));
    return out;
}

} // namespace

void FLockstep::Init(ENetMode mode, u32 tick_rate_hz) noexcept {
    m_Mode          = mode;
    // tick_rate_hz == 0 は意味を成さないので最低 1 に丸める。0 除算防止。
    m_TickRateHz  = (tick_rate_hz == 0) ? 1u : tick_rate_hz;
    m_CurrentTick  = 0;
    m_ReplayCursor = 0;
    // 既存 frames は破棄しない (mode を切り替えるだけの使い方も許容)。
    // 完全リセットしたい場合は呼び出し側で Clear() を併用する。
}

void FLockstep::RecordInput(const FInputFrame& frame) noexcept {
    // Replay モード中は記録しない (上書きを防ぐ)。
    if (m_Mode == ENetMode::Replay) {
        return;
    }
    m_Frames.PushBack(frame);
    // m_CurrentTick は「次に書き込む tick」のヒント。連続 tick 想定で frame.tick+1
    // に進める。非連続な tick (rollback で巻き戻し等) を許容する場合は呼び出し側で
    // 明示的に管理することになる。
    m_CurrentTick = frame.tick + 1u;
}

void FLockstep::StartReplay() noexcept {
    m_Mode          = ENetMode::Replay;
    m_ReplayCursor = 0;
    m_CurrentTick  = 0;
    // m_Frames はそのまま。Local で記録した内容を頭から再生する。
}

bool FLockstep::ConsumeInput(u32 tick, u32 player_id, FInputFrame& out) noexcept {
    // Replay モード以外では取り出しを禁止する (誤用検知)。
    if (m_Mode != ENetMode::Replay) {
        return false;
    }
    const usize n = m_Frames.Size();
    // m_ReplayCursor から線形走査。記録順 = tick 昇順を仮定するため、
    // ヒット後に cursor を前進させて amortised O(1) を狙う。
    for (usize i = m_ReplayCursor; i < n; ++i) {
        const FInputFrame& f = m_Frames[i];
        if (f.tick == tick && f.player_id == player_id) {
            out = f;
            // 次回検索開始位置を更新。同 tick の他プレイヤーも cursor の後ろにある
            // 想定なので、cursor は i+1 ではなく i のままにして次の player を探せる
            // ようにする選択肢もあるが、4 人対戦程度なら全件走査コストが小さいため
            // ここでは「次の tick への前進」を素朴に表現する i+1 を採用。
            m_ReplayCursor = static_cast<u32>(i + 1u);
            m_CurrentTick  = tick + 1u;
            return true;
        }
        // 記録順が tick 昇順を破る場合 (rollback / 未来 frame の混入) は素朴に
        // 走査を続ける。
    }
    return false;
}

u32 FLockstep::InputCount() const noexcept {
    return static_cast<u32>(m_Frames.Size());
}

u64 FLockstep::ComputeChecksum() const noexcept {
    u64 h = kFnvOffsetBasis;
    const usize n = m_Frames.Size();
    for (usize i = 0; i < n; ++i) {
        const FInputFrame& f = m_Frames[i];
        h = FnvFoldU32(h, f.tick);
        h = FnvFoldU32(h, f.player_id);
        h = FnvFold(h, f.buttons);
        // FVec2 axis は f32 のビット表現を u32 として混ぜる。
        // -0.0f と +0.0f を区別したくない場合は呼び出し側で正規化する責任。
        h = FnvFoldU32(h, BitsOf(f.axis.x));
        h = FnvFoldU32(h, BitsOf(f.axis.y));
    }
    return h;
}

void FLockstep::Clear() noexcept {
    m_Frames.Clear();
    m_CurrentTick  = 0;
    m_ReplayCursor = 0;
    // m_Mode / m_TickRateHz は保持。Init で改めて切り替える設計。
}

/** frames を [magic][version][tick_rate_hz][frame_count][frames][crc32] レイアウトで buffer に書き出す。 */
TResult<void> FLockstep::SaveToBuffer(u8* buffer, u32 size, u32& out_written) noexcept {
    out_written = 0;
    if (buffer == nullptr) {
        return ACS_ERR(IO, kSub_NullBuffer,
                       "FLockstep::SaveToBuffer: buffer is null");
    }

    if (m_Frames.Size() > kLockstepMaximumFrames ||
        m_TickRateHz == 0 || m_TickRateHz > kLockstepMaximumTickRateHz) {
        return ACS_ERR(IO, kSub_LimitExceeded,
                       "FLockstep::SaveToBuffer: tick rate or frame count exceeds the limit");
    }
    const u32 frame_count = static_cast<u32>(m_Frames.Size());
    for (u32 i = 0; i < frame_count; ++i) {
        if (!IsFiniteF32Bits(BitsOf(m_Frames[i].axis.x)) ||
            !IsFiniteF32Bits(BitsOf(m_Frames[i].axis.y))) {
            return ACS_ERR(IO, kSub_BadValue,
                           "FLockstep::SaveToBuffer: non-finite axis value");
        }
    }
    // 必要バイト数。frame_count を u64 に広げて 32bit 乗算オーバーフローを避ける。
    const u64 required64 =
        static_cast<u64>(kHeaderSize) +
        static_cast<u64>(frame_count) * static_cast<u64>(kFrameWireSize) +
        static_cast<u64>(kFooterSize);
    if (static_cast<u64>(size) < required64) {
        return ACS_ERR(IO, kSub_BufferTooSmall,
                       "FLockstep::SaveToBuffer: buffer too small for frames");
    }
    const u32 required = static_cast<u32>(required64);

    // ---- header (16 B) ---------------------------------------------------
    MemCopy(buffer, kMagic, sizeof(kMagic));      // 0x00: magic 'ACSL'
    WriteU32LE(buffer + 4,  kFormatVersion);      // 0x04: version
    WriteU32LE(buffer + 8,  m_TickRateHz);        // 0x08: tick_rate_hz
    WriteU32LE(buffer + 12, frame_count);         // 0x0C: frame_count

    // ---- frames (各 17 B、固定 wire layout) ------------------------------
    u8* frames_begin = buffer + kHeaderSize;
    u8* p = frames_begin;
    for (u32 i = 0; i < frame_count; ++i) {
        const FInputFrame& f = m_Frames[i];
        WriteU32LE(p,      f.tick);            // +0:  tick
        WriteU32LE(p + 4,  f.player_id);       // +4:  player_id
        p[8] = f.buttons;                      // +8:  buttons (u8)
        WriteU32LE(p + 9,  BitsOf(f.axis.x));  // +9:  axis.x (f32 bits)
        WriteU32LE(p + 13, BitsOf(f.axis.y));  // +13: axis.y (f32 bits)
        p += kFrameWireSize;
    }

    // ---- crc32 footer (frames 部のみを対象) ------------------------------
    const u32 frames_bytes = frame_count * kFrameWireSize;
    const u32 crc = ComputeCrc32(frames_begin, frames_bytes);
    WriteU32LE(p, crc);

    out_written = required;
    return Ok();
}

/** buffer を解釈・検証 (magic / version / size / crc32) して frames を復元する (置換セマンティクス)。 */
TResult<void> FLockstep::LoadFromBuffer(const u8* buffer, u32 size) noexcept {
    return TryLoadFromBuffer(buffer, size);
}

/** 全検証とstaging成功後にだけframesを置換するchecked load。 */
TResult<void> FLockstep::TryLoadFromBuffer(const u8* buffer, u32 size) noexcept {
    if (buffer == nullptr) {
        return ACS_ERR(IO, kSub_NullBuffer,
                       "FLockstep::LoadFromBuffer: buffer is null");
    }

    // ---- 最小サイズ (header + footer) -----------------------------------
    if (size < kHeaderSize + kFooterSize) {
        return ACS_ERR(IO, kSub_BadSize,
                       "FLockstep::LoadFromBuffer: buffer smaller than header+footer");
    }

    // ---- magic 検証 ------------------------------------------------------
    if (MemCmp(buffer, kMagic, sizeof(kMagic)) != 0) {
        return ACS_ERR(IO, kSub_BadMagic,
                       "FLockstep::LoadFromBuffer: magic mismatch (not an ACSL buffer)");
    }

    // ---- version 検証 ----------------------------------------------------
    const u32 version = ReadU32LE(buffer + 4);
    if (version != kFormatVersion) {
        return ACS_ERR(IO, kSub_BadVersion,
                       "FLockstep::LoadFromBuffer: unsupported format version");
    }

    const u32 tick_rate_hz = ReadU32LE(buffer + 8);
    const u32 frame_count  = ReadU32LE(buffer + 12);
    if (tick_rate_hz == 0 || tick_rate_hz > kLockstepMaximumTickRateHz ||
        frame_count > kLockstepMaximumFrames) {
        return ACS_ERR(IO, kSub_LimitExceeded,
                       "FLockstep::TryLoadFromBuffer: tick rate or frame count exceeds the limit");
    }

    // ---- frame_count とサイズの整合 -------------------------------------
    // header + frames + footer が size と完全一致することを要求する。
    // u64 で計算して 32bit 乗算オーバーフローを避ける。
    const u64 expected64 =
        static_cast<u64>(kHeaderSize) +
        static_cast<u64>(frame_count) * static_cast<u64>(kFrameWireSize) +
        static_cast<u64>(kFooterSize);
    if (expected64 != static_cast<u64>(size)) {
        return ACS_ERR(IO, kSub_BadSize,
                       "FLockstep::LoadFromBuffer: frame_count inconsistent with buffer size");
    }

    // ---- crc32 検証 (frames 部のみ) -------------------------------------
    const u8* frames_begin = buffer + kHeaderSize;
    const u32 frames_bytes = frame_count * kFrameWireSize;
    const u32 actual_crc   = ComputeCrc32(frames_begin, frames_bytes);
    const u32 stored_crc   = ReadU32LE(frames_begin + frames_bytes);  // footer
    if (actual_crc != stored_crc) {
        return ACS_ERR(IO, kSub_BadCrc,
                       "FLockstep::LoadFromBuffer: CRC32 mismatch (corrupt or tampered)");
    }
    const u8* value_cursor = frames_begin;
    for (u32 i = 0; i < frame_count; ++i) {
        if (!IsFiniteF32Bits(ReadU32LE(value_cursor + 9u)) ||
            !IsFiniteF32Bits(ReadU32LE(value_cursor + 13u))) {
            return ACS_ERR(IO, kSub_BadValue,
                           "FLockstep::TryLoadFromBuffer: non-finite axis value");
        }
        value_cursor += kFrameWireSize;
    }

    TArray<FInputFrame> staged(*m_Frames.GetAllocator());
    if (!staged.TryReserve(frame_count)) {
        return ACS_ERR(Memory, kSub_Oom,
                       "FLockstep::TryLoadFromBuffer: frame staging allocation failed");
    }
    const u8* p = frames_begin;
    for (u32 i = 0; i < frame_count; ++i) {
        FInputFrame f;
        f.tick      = ReadU32LE(p);            // +0:  tick
        f.player_id = ReadU32LE(p + 4);        // +4:  player_id
        f.buttons   = p[8];                    // +8:  buttons (u8)
        f.axis.x    = F32FromBits(ReadU32LE(p + 9));   // +9:  axis.x
        f.axis.y    = F32FromBits(ReadU32LE(p + 13));  // +13: axis.y
        if (!staged.TryPushBack(f)) {
            return ACS_ERR(Memory, kSub_Oom,
                           "FLockstep::TryLoadFromBuffer: frame staging append failed");
        }
        p += kFrameWireSize;
    }

    m_Frames = Move(staged);
    m_TickRateHz   = tick_rate_hz;
    m_CurrentTick  = 0;
    m_ReplayCursor = 0;
    return Ok();
}

/** target allocatorを引き継ぐ空のload stagingへ初期化する。 */
void FLockstep::PrepareLoadStaging_Internal(FLockstep& staging) const noexcept
{
    staging.m_Mode = ENetMode::Local;
    staging.m_TickRateHz = 60u;
    staging.m_CurrentTick = 0u;
    staging.m_ReplayCursor = 0u;
    staging.m_Frames = TArray<FInputFrame>(*m_Frames.GetAllocator());
}

/** loaded persistent stateだけをno-fail swapし、modeは各instanceで維持する。 */
void FLockstep::SwapLoadedState_Internal(FLockstep& other) noexcept
{
    u32 value = m_TickRateHz;
    m_TickRateHz = other.m_TickRateHz;
    other.m_TickRateHz = value;
    value = m_CurrentTick;
    m_CurrentTick = other.m_CurrentTick;
    other.m_CurrentTick = value;
    value = m_ReplayCursor;
    m_ReplayCursor = other.m_ReplayCursor;
    other.m_ReplayCursor = value;
    TArray<FInputFrame> frames = Move(m_Frames);
    m_Frames = Move(other.m_Frames);
    other.m_Frames = Move(frames);
}

} // namespace acs::game

// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar M — FLockstep 実装 (Phase M-1 スケルトン)
//
// 現フェーズ:
//   ・Init / RecordInput / StartReplay / ConsumeInput / CurrentTick /
//     InputCount / ComputeChecksum / Clear はすべて本実装。
//   ・SaveToBuffer / LoadFromBuffer は ACS_ERR(IO, kSub_NotImplemented) を返す
//     stub (FSaveSlot Phase 1 と同じ pattern)。Phase M-2 で実 I/O 接続予定。
//
// 設計メモ:
//   ・ConsumeInput は _replay_cursor から線形走査する。記録順 = tick 昇順を
//     仮定すれば、cursor を前進させることで amortised O(1)。同 tick 内の
//     player_id 順は問わない (4 人対戦なら 4 frame だけ走査する)。
//   ・ComputeChecksum は FNV-1a-like u64。OFFSET_BASIS = 0xCBF29CE484222325,
//     PRIME = 0x100000001B3。各 InputFrame の bit パターンを 8 バイト境界で
//     消費する単純実装。FVec2 の f32 ビットは memcpy 経由で u32 に読み替え
//     (strict aliasing 違反回避)。
//   ・FNV を選んだ理由: STL 不使用 + ヘッダ追加なしで 5 行で書ける。
//     replay 同期ずれ検知 (1〜2 bit 差分が大半) には十分な avalanche を持つ。
//     より強い hash (xxHash3 / CRC-64) は Phase M-2 で考慮する。
#include "gameframework/Lockstep.h"

namespace acs::game {

namespace {

// FNV-1a 64-bit 定数 (RFC 風)。
constexpr u64 kFnvOffsetBasis = 0xCBF29CE484222325ull;
constexpr u64 kFnvPrime       = 0x100000001B3ull;

// 1 バイトを FNV-1a で混ぜる。
inline u64 FnvFold(u64 h, u8 byte) noexcept {
    h ^= static_cast<u64>(byte);
    h *= kFnvPrime;
    return h;
}

// u32 を little-endian バイト列として混ぜる。
inline u64 FnvFoldU32(u64 h, u32 v) noexcept {
    h = FnvFold(h, static_cast<u8>(v        & 0xFFu));
    h = FnvFold(h, static_cast<u8>((v >> 8 ) & 0xFFu));
    h = FnvFold(h, static_cast<u8>((v >> 16) & 0xFFu));
    h = FnvFold(h, static_cast<u8>((v >> 24) & 0xFFu));
    return h;
}

// f32 のビットパターンを u32 として取り出して混ぜる。
// reinterpret_cast や union 経由は strict aliasing 違反になるため memcpy で実施。
// memcpy は <cstring> 依存だが、ACS 全体で既に MemCopy 経由 (memory/Memory.h) を
// 採用しているのでここでは __builtin_memcpy 相当のコンパイラ最適化に委ねる小ループで
// 実装する (STL 不使用方針)。
inline u32 BitsOf(f32 v) noexcept {
    u32 out = 0;
    const u8* src = reinterpret_cast<const u8*>(&v);
    out |= static_cast<u32>(src[0]);
    out |= static_cast<u32>(src[1]) << 8;
    out |= static_cast<u32>(src[2]) << 16;
    out |= static_cast<u32>(src[3]) << 24;
    return out;
}

} // namespace

void FLockstep::Init(ENetMode mode, u32 tick_rate_hz) noexcept {
    _mode          = mode;
    // tick_rate_hz == 0 は意味を成さないので最低 1 に丸める。0 除算防止。
    _tick_rate_hz  = (tick_rate_hz == 0) ? 1u : tick_rate_hz;
    _current_tick  = 0;
    _replay_cursor = 0;
    // 既存 frames は破棄しない (mode を切り替えるだけの使い方も許容)。
    // 完全リセットしたい場合は呼び出し側で Clear() を併用する。
}

void FLockstep::RecordInput(const InputFrame& frame) noexcept {
    // Replay モード中は記録しない (上書きを防ぐ)。
    if (_mode == ENetMode::Replay) {
        return;
    }
    _frames.PushBack(frame);
    // _current_tick は「次に書き込む tick」のヒント。連続 tick 想定で frame.tick+1
    // に進める。非連続な tick (rollback で巻き戻し等) を許容する場合は呼び出し側で
    // 明示的に管理することになる (Phase M-2 で rollback 対応する際に再設計)。
    _current_tick = frame.tick + 1u;
}

void FLockstep::StartReplay() noexcept {
    _mode          = ENetMode::Replay;
    _replay_cursor = 0;
    _current_tick  = 0;
    // _frames はそのまま。Local で記録した内容を頭から再生する。
}

bool FLockstep::ConsumeInput(u32 tick, u32 player_id, InputFrame& out) noexcept {
    // Replay モード以外では取り出しを禁止する (誤用検知)。
    if (_mode != ENetMode::Replay) {
        return false;
    }
    const usize n = _frames.Size();
    // _replay_cursor から線形走査。記録順 = tick 昇順を仮定するため、
    // ヒット後に cursor を前進させて amortised O(1) を狙う。
    for (usize i = _replay_cursor; i < n; ++i) {
        const InputFrame& f = _frames[i];
        if (f.tick == tick && f.player_id == player_id) {
            out = f;
            // 次回検索開始位置を更新。同 tick の他プレイヤーも cursor の後ろにある
            // 想定なので、cursor は i+1 ではなく i のままにして次の player を探せる
            // ようにする選択肢もあるが、4 人対戦程度なら全件走査コストが小さいため
            // ここでは「次の tick への前進」を素朴に表現する i+1 を採用。
            _replay_cursor = static_cast<u32>(i + 1u);
            _current_tick  = tick + 1u;
            return true;
        }
        // 記録順が tick 昇順を破る場合 (rollback / 未来 frame の混入) は素朴に
        // 走査を続ける。Phase M-2 で per-tick index を持つことで高速化予定。
    }
    return false;
}

u32 FLockstep::InputCount() const noexcept {
    return static_cast<u32>(_frames.Size());
}

u64 FLockstep::ComputeChecksum() const noexcept {
    u64 h = kFnvOffsetBasis;
    const usize n = _frames.Size();
    for (usize i = 0; i < n; ++i) {
        const InputFrame& f = _frames[i];
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
    _frames.Clear();
    _current_tick  = 0;
    _replay_cursor = 0;
    // _mode / _tick_rate_hz は保持。Init で改めて切り替える設計。
}

// -----------------------------------------------------------------------------
// SaveToBuffer — Phase M-1: stub (NotImplemented を返す)
// -----------------------------------------------------------------------------
// Phase M-2 の擬似コード:
//   1. required = 16 (header) + frame_count * sizeof(InputFrame) + 4 (footer)
//   2. size < required なら kSub_BufferTooSmall
//   3. write [magic][version][tick_rate_hz][frame_count]
//   4. write frames (bulk memcpy)
//   5. write crc32(frames) footer
//   6. out_written = required
TResult<void> FLockstep::SaveToBuffer(u8* buffer, u32 size, u32& out_written) noexcept {
    out_written = 0;
    if (buffer == nullptr) {
        return ACS_ERR(IO, kSub_NullBuffer,
                       "FLockstep::SaveToBuffer: buffer is null");
    }
    (void)size;
    // Phase 1: 実 I/O は未接続。"動くが必ず失敗する" stub にしておき、
    // 呼び出し側 (replay 保存 UI / テスト) が TResult を握りつぶさない設計を強制する。
    return ACS_ERR(IO, kSub_NotImplemented,
                   "FLockstep::SaveToBuffer is not yet implemented (Phase M-1 stub)");
}

// -----------------------------------------------------------------------------
// LoadFromBuffer — Phase M-1: stub (NotImplemented を返す)
// -----------------------------------------------------------------------------
// Phase M-2 の擬似コード:
//   1. size < 20 (header + footer 最小) なら kSub_BadSize
//   2. magic 検証 → 不一致なら kSub_BadMagic
//   3. version 検証 → kVersion と不一致なら kSub_BadVersion
//   4. frame_count * sizeof(InputFrame) + 20 が size と一致しなければ kSub_BadSize
//   5. crc32 計算 → footer と不一致なら kSub_BadCrc
//   6. _frames.Clear(); _frames.Reserve(frame_count); 順次 PushBack
//   7. _tick_rate_hz / _current_tick を 0 にリセット (StartReplay 待ち状態)
TResult<void> FLockstep::LoadFromBuffer(const u8* buffer, u32 size) noexcept {
    if (buffer == nullptr) {
        return ACS_ERR(IO, kSub_NullBuffer,
                       "FLockstep::LoadFromBuffer: buffer is null");
    }
    (void)size;
    return ACS_ERR(IO, kSub_NotImplemented,
                   "FLockstep::LoadFromBuffer is not yet implemented (Phase M-1 stub)");
}

} // namespace acs::game

// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — FReplayDirector 実装 (Phase R-3 スケルトン)
//
// 現フェーズ:
//   ・Init / StartRecording / StopRecording / StartPlayback /
//     PausePlayback / ResumePlayback / StopPlayback /
//     SetPlaybackSpeed / SeekToTick / Tick / ProgressNormalized は本実装。
//   ・SaveReplay / LoadReplay は ACS_ERR(IO, kSub_NotImplemented) を返す stub。
//     Phase R-4 で FInputRecorder / FLockstep の SaveToBuffer / LoadFromBuffer と
//     metadata sidecar (.meta) の bit-precise layout を接続予定。
//
// 設計メモ:
//   ・Mode 遷移は明示的: Idle → (StartRecording) → Recording → (StopRecording) → Idle
//                       Idle → (StartPlayback)  → Playback  → (StopPlayback)  → Idle
//                       Playback ↔ (Pause/Resume) ↔ Paused
//                       Paused → (StopPlayback) → Idle
//     Recording から Playback への直接遷移、および Idle から Pause/Resume への
//     遷移は kSub_BadMode で拒否 (一旦 Stop を挟むことを強制)。
//   ・Tick(dt) は録画中と再生中で意味が変わる:
//     - Recording: m_TickRateHz * dt 分の tick を m_CurrentTick に加算する。
//       実際の入力 capture は FInputRecorder / FLockstep 側で別途行う。
//     - Playback: dt * m_PlaybackSpeed を accumulator に貯め、tick_rate_hz 単位
//       で m_CurrentTick に加算する。duration_ticks に達したら自動 Stop。
//   ・SetPlaybackSpeed は { 0.25, 0.5, 1, 2, 4, 8, 16 } 等を想定するが、
//     UI スクラブで任意 f32 が来ても扱えるよう範囲 clamp する (0.25 未満は
//     0.25、16 超は 16)。0 や負値は 1.0 にリセットして "誤呼び出しからの自動
//     復帰" を担保する。
//   ・SeekToTick は duration_ticks を上限に clamp。Mode は変えない (Paused 中
//     の scrub もできるよう)。accumulator も 0 にリセットする (sub-tick を
//     持ち越さない)。
//   ・ProgressNormalized は duration_ticks == 0 のとき 0.0 を返す (録画開始
//     直後 / metadata 未設定の安全側)。
//
// SaveReplay / LoadReplay (本実装):
//   ・単一 container ファイル (.acsr 拡張子想定) に metadata + FInputRecorder の
//     .acsr blob + FLockstep の .acsl blob をまとめて書き出す。低レベル blob 自体は
//     FInputRecorder::SaveToBuffer / FLockstep::SaveToBuffer の real な直列化を
//     そのまま使い、本クラスは「container header + metadata + 2 blob + CRC32」を
//     被せるだけにする (二重 framing で各 blob は独立に検証可能なまま)。
//   ・Win32 I/O は FSaveArchive / FSettings と同流儀: `.tmp` に書いて
//     FlushFileBuffers → MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH) で atomic
//     rename。途中失敗でも本ファイルは旧内容のまま。
//   ・byte codec は NetSnapshot.cpp / FSaveArchive.cpp と同じ LE MemCopy helper +
//     CRC32 (poly 0xEDB88320) を file-local に複製する (gameframework は assetpack を
//     依存に持たないため link 単位を独立させる方針)。
#include "gameframework/ReplayDirector.h"

#include "gameframework/InputRecorder.h"  // FInputRecorder::SaveToBuffer / LoadFromBuffer / SampleCount
#include "gameframework/Lockstep.h"       // FLockstep::SaveToBuffer / LoadFromBuffer / InputCount

#include "foundation/Platform.h"  // <windows.h> (CreateFileW / WriteFile / ReadFile / MoveFileExW)
#include "memory/Memory.h"        // MemCopy / MemCmp / MemSet / DefaultAllocator
#include "container/Array.h"      // TArray (直列化バッファ)

namespace acs::game {

namespace {

// 再生速度の clamp 範囲。0.25x / 0.5x / 1x / 2x / 4x / 8x / 16x の範囲外を弾く。
constexpr f32 kMinSpeed = 0.25f;
constexpr f32 kMaxSpeed = 16.0f;

// SetPlaybackSpeed の範囲 clamp + 異常値ガード。
inline f32 ClampSpeed(f32 v) noexcept {
    // 0 / 負値 / NaN は誤呼び出しとして 1.0 に強制復帰。Pause は別 API なので
    // ここで 0 を許容すると意味が二重になる。
    if (!(v > 0.0f)) {
        return 1.0f;  // NaN 比較は false になるので NaN もこの枝で 1.0 に。
    }
    if (v < kMinSpeed) return kMinSpeed;
    if (v > kMaxSpeed) return kMaxSpeed;
    return v;
}

// =============================================================================
// container wire format 定数
// -----------------------------------------------------------------------------
// layout:
//   [0]   magic 'ACRP' (4)                            ← 'A' 'C' 'R' 'P'
//   [4]   version u32 (4)
//   ---- metadata ----
//         seed u64 (8)
//         timestamp u64 (8)
//         duration_ticks u32 (4)
//         game_version  : [len u32][bytes]   (null → len 0)
//         level_id      : [len u32][bytes]
//         player_name   : [len u32][bytes]
//         checksum_hex  : [len u32][bytes]
//   ---- blobs ----
//         input_blob_size u32 (4) + input_blob bytes
//         lockstep_blob_size u32 (4) + lockstep_blob bytes
//   ---- footer ----
//         crc32 u32 (4)  ← magic を含む「footer 直前まで」全体の CRC32
// =============================================================================
constexpr u8  kReplayMagic[4] = { 'A', 'C', 'R', 'P' };  // 'ACRP' (ACS Replay)
constexpr u32 kReplayVersion  = 1u;
constexpr u32 kCrcFooterSize  = 4u;

// metadata 文字列の sanity 上限 (破損 container で巨大 len を読まされる事故予防)。
constexpr u32 kMaxStringLen   = 64u * 1024u;          // 64 KiB / 1 文字列
// container 全体の sanity 上限 (FSettings の 16 MiB と整合)。録画は最大でも
// 数十 MB オーダーだが、ここでは header 読み出し時の defensive 上限として使う。
constexpr u64 kMaxContainerBytes = 256ull * 1024ull * 1024ull;  // 256 MiB

// ---- little-endian 読み書き helper (strict-aliasing 安全) ------------------
// FSaveArchive.cpp / NetSnapshot.cpp と同流儀。LE 固定 (Win/x64 / ARM64)。
inline void WriteU32LE(u8* dst, u32 v) noexcept { MemCopy(dst, &v, sizeof(u32)); }
inline void WriteU64LE(u8* dst, u64 v) noexcept { MemCopy(dst, &v, sizeof(u64)); }
inline u32  ReadU32LE (const u8* src) noexcept { u32 v = 0; MemCopy(&v, src, sizeof(u32)); return v; }
inline u64  ReadU64LE (const u8* src) noexcept { u64 v = 0; MemCopy(&v, src, sizeof(u64)); return v; }

// ---- CRC32 (poly 0xEDB88320, init/xorout 0xFFFFFFFF) ----------------------
// FSaveArchive / FLockstep / NetSnapshot と同一実装。Meyer's singleton で
// thread-safe な lookup table 初期化を行う (gameframework 内で重複実装)。
const u32* GetCrc32Table() noexcept {
    static u32 m_Table[256] = {};
    static bool m_Initialized = false;
    if (!m_Initialized) {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (u32 k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            m_Table[i] = c;
        }
        m_Initialized = true;
    }
    return m_Table;
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

// ---- Win32 read/write/path helper (FSaveArchive / FSettings と同流儀) ------

// `<path>` の末尾に L".tmp" を付けた一時パスを out_buf に作る。
// out_cap は out_buf の要素数。成功で true、長さ超過で false。
bool MakeTmpPath(const wchar_t* path, wchar_t* out_buf, usize out_cap) noexcept {
    usize n = 0;
    while (path[n] != L'\0') ++n;
    const wchar_t suffix[] = L".tmp";
    const usize   suffix_n = 4;
    if (n + suffix_n + 1 > out_cap) return false;  // +1 = NUL
    for (usize i = 0; i < n; ++i) out_buf[i] = path[i];
    for (usize i = 0; i < suffix_n; ++i) out_buf[n + i] = suffix[i];
    out_buf[n + suffix_n] = L'\0';
    return true;
}

bool WriteAll(HANDLE h, const void* src, u64 size, DWORD& err) noexcept {
    err = 0;
    if (size == 0) return true;
    const u8* p         = static_cast<const u8*>(src);
    u64       remaining = size;
    while (remaining > 0) {
        const DWORD chunk = (remaining > 0x7FFFFFFFu)
                                ? 0x7FFFFFFFu
                                : static_cast<DWORD>(remaining);
        DWORD wrote = 0;
        if (!::WriteFile(h, p, chunk, &wrote, nullptr) || wrote != chunk) {
            err = ::GetLastError();
            if (err == 0) err = ERROR_WRITE_FAULT;
            return false;
        }
        p         += wrote;
        remaining -= wrote;
    }
    return true;
}

bool ReadAll(HANDLE h, void* dst, u64 size, DWORD& err) noexcept {
    err = 0;
    if (size == 0) return true;
    u8* p         = static_cast<u8*>(dst);
    u64 remaining = size;
    while (remaining > 0) {
        const DWORD chunk = (remaining > 0x7FFFFFFFu)
                                ? 0x7FFFFFFFu
                                : static_cast<DWORD>(remaining);
        DWORD got = 0;
        if (!::ReadFile(h, p, chunk, &got, nullptr) || got == 0) {
            err = ::GetLastError();
            if (err == 0) err = ERROR_HANDLE_EOF;
            return false;
        }
        p         += got;
        remaining -= got;
    }
    return true;
}

// ---- TArray<u8> への length-prefixed 文字列 append ------------------------
// [u32 len][bytes]。s == nullptr は len 0 (= 空文字列) として扱う。
void AppendLenPrefixedString(TArray<u8>& out, const char* s) noexcept {
    u32 len = 0;
    if (s != nullptr) {
        while (s[len] != '\0') ++len;
    }
    const usize off = out.Size();
    out.Resize(off + sizeof(u32) + static_cast<usize>(len));
    WriteU32LE(out.Data() + off, len);
    if (len > 0) {
        MemCopy(out.Data() + off + sizeof(u32), s, len);
    }
}

// ---- TArray<u8> への raw bytes append (LE u32 size prefix 付き) ------------
void AppendU32(TArray<u8>& out, u32 v) noexcept {
    const usize off = out.Size();
    out.Resize(off + sizeof(u32));
    WriteU32LE(out.Data() + off, v);
}

void AppendU64(TArray<u8>& out, u64 v) noexcept {
    const usize off = out.Size();
    out.Resize(off + sizeof(u64));
    WriteU64LE(out.Data() + off, v);
}

void AppendBytes(TArray<u8>& out, const u8* src, u32 size) noexcept {
    if (size == 0) return;
    const usize off = out.Size();
    out.Resize(off + static_cast<usize>(size));
    MemCopy(out.Data() + off, src, size);
}

} // namespace

// -----------------------------------------------------------------------------
// 初期化
// -----------------------------------------------------------------------------

void FReplayDirector::Init() noexcept {
    m_Mode             = EReplayMode::Idle;
    m_Metadata         = ReplayMetadata{};   // 全 field を default に戻す
    m_CurrentTick     = 0;
    m_PlaybackSpeed   = 1.0f;
    m_TickAccumulator = 0.0f;
    m_TickRateHz     = 60;
    // m_Recorder / m_Lockstep (source 結線) と owned 文字列バッファは保持する。
    // Init は「録画 / 再生セッションの state リセット」であり、source の再注入を
    // 強制しない (director の寿命を通じて結線を維持したい)。
}

// -----------------------------------------------------------------------------
// 低レベル source の注入 (非所有)
// -----------------------------------------------------------------------------

void FReplayDirector::SetSources(FInputRecorder* recorder, FLockstep* lockstep) noexcept {
    // 非所有ポインタをそのまま保持する (寿命は呼び出し側責務)。nullptr 許容:
    // 片方だけ注入する / どちらも注入しない構成でも container は valid。
    m_Recorder = recorder;
    m_Lockstep = lockstep;
}

// -----------------------------------------------------------------------------
// 録画開始 / 停止
// -----------------------------------------------------------------------------

TResult<void> FReplayDirector::StartRecording(const ReplayMetadata& meta) noexcept {
    // Idle 以外からの直接遷移は禁止。Recording 中の再開や Playback 中の
    // 切り替えは意図しない上書きが起こりやすいため、明示的な Stop を強制する。
    if (m_Mode != EReplayMode::Idle) {
        return ACS_ERR(Generic, kSub_BadMode,
                       "FReplayDirector::StartRecording: must be Idle");
    }
    m_Mode             = EReplayMode::Recording;
    m_Metadata         = meta;            // POD copy (const char* はポインタコピー)
    m_CurrentTick     = 0;
    m_TickAccumulator = 0.0f;
    // m_PlaybackSpeed は触らない (録画中は無意味だが、StartPlayback 後にも
    // 直前の倍速を引き継ぎたい UX を許容する)。
    return TResult<void>{};
}

TResult<void> FReplayDirector::StopRecording() noexcept {
    // Recording 以外からの呼び出しは誤用扱い。Playback 中に StopRecording を
    // 呼んでしまった場合に黙って Idle にすると metadata が壊れるため明示エラー。
    if (m_Mode != EReplayMode::Recording) {
        return ACS_ERR(Generic, kSub_BadMode,
                       "FReplayDirector::StopRecording: must be Recording");
    }
    // 録画した tick 数を確定。LoadReplay 後の再生で ProgressNormalized が
    // 正しく計算できるよう metadata に焼き込む。
    m_Metadata.duration_ticks = m_CurrentTick;
    m_Mode                    = EReplayMode::Idle;
    m_TickAccumulator        = 0.0f;
    return TResult<void>{};
}

// -----------------------------------------------------------------------------
// 再生開始 / 一時停止 / 停止
// -----------------------------------------------------------------------------

TResult<void> FReplayDirector::StartPlayback() noexcept {
    if (m_Mode != EReplayMode::Idle) {
        return ACS_ERR(Generic, kSub_BadMode,
                       "FReplayDirector::StartPlayback: must be Idle");
    }
    m_Mode             = EReplayMode::Playback;
    m_CurrentTick     = 0;
    m_TickAccumulator = 0.0f;
    // m_Metadata はそのまま (StartRecording 直後の再生、または LoadReplay 後の
    // 再生いずれも metadata が既に正しく設定されている前提)。
    return TResult<void>{};
}

void FReplayDirector::PausePlayback() noexcept {
    // Playback 以外では no-op (Paused 中の再 Pause / Idle 中の誤呼び出しを許容)。
    if (m_Mode == EReplayMode::Playback) {
        m_Mode = EReplayMode::Paused;
    }
}

void FReplayDirector::ResumePlayback() noexcept {
    // Paused 以外では no-op。
    if (m_Mode == EReplayMode::Paused) {
        m_Mode = EReplayMode::Playback;
    }
}

void FReplayDirector::StopPlayback() noexcept {
    // Playback / Paused → Idle。Recording / Idle 中の呼び出しは黙って no-op
    // (Stop は冪等であってほしい UX)。
    if (m_Mode == EReplayMode::Playback || m_Mode == EReplayMode::Paused) {
        m_Mode             = EReplayMode::Idle;
        m_TickAccumulator = 0.0f;
    }
}

// -----------------------------------------------------------------------------
// 再生速度 / Seek / Progress
// -----------------------------------------------------------------------------

void FReplayDirector::SetPlaybackSpeed(f32 speed) noexcept {
    m_PlaybackSpeed = ClampSpeed(speed);
    // accumulator はそのまま。速度変更時に sub-tick の dt を捨てると
    // 再生が微妙にジャンプするのを防ぐ。
}

void FReplayDirector::SeekToTick(u32 tick) noexcept {
    // duration_ticks を上限に clamp。LoadReplay 前 (duration = 0) の seek は
    // 0 に張り付くが、これは UI のスクラブバーで「録画されていない」状態を
    // 明示するため意図通り。
    const u32 limit = m_Metadata.duration_ticks;
    m_CurrentTick     = (tick > limit) ? limit : tick;
    m_TickAccumulator = 0.0f;  // sub-tick を持ち越さない
}

f32 FReplayDirector::ProgressNormalized() const noexcept {
    const u32 d = m_Metadata.duration_ticks;
    if (d == 0) {
        return 0.0f;  // 録画開始直後 / metadata 未設定の安全側
    }
    const f32 p = static_cast<f32>(m_CurrentTick) / static_cast<f32>(d);
    // 上限 1.0 で clamp (Tick で duration を超えた瞬間に 1.0+ になる可能性が
    // あるため)。下限は m_CurrentTick が unsigned なので不要。
    return (p > 1.0f) ? 1.0f : p;
}

// -----------------------------------------------------------------------------
// 毎フレーム呼び出し
// -----------------------------------------------------------------------------

void FReplayDirector::Tick(f32 dt) noexcept {
    // 異常な dt (NaN / 負) はゲームループの早期 frame skip / pause からの復帰時に
    // 紛れ込みやすい。0 でガードして無視する。
    if (!(dt > 0.0f)) {
        return;
    }

    if (m_Mode == EReplayMode::Recording) {
        // 録画中: tick_rate_hz * dt 分だけ m_CurrentTick を進める。
        // 実入力 capture は FInputRecorder / FLockstep 側で別途行う前提なので、
        // ここは単純な tick カウンタの前進のみ。
        m_TickAccumulator += dt * static_cast<f32>(m_TickRateHz);
        if (m_TickAccumulator >= 1.0f) {
            const u32 steps = static_cast<u32>(m_TickAccumulator);
            m_CurrentTick    += steps;
            m_TickAccumulator -= static_cast<f32>(steps);
        }
        return;
    }

    if (m_Mode == EReplayMode::Playback) {
        // 再生中: dt * playback_speed * tick_rate_hz 分だけ m_CurrentTick を進める。
        m_TickAccumulator += dt * m_PlaybackSpeed * static_cast<f32>(m_TickRateHz);
        if (m_TickAccumulator >= 1.0f) {
            const u32 steps = static_cast<u32>(m_TickAccumulator);
            m_CurrentTick    += steps;
            m_TickAccumulator -= static_cast<f32>(steps);
        }
        // duration_ticks に達したら自動的に Idle へ落とす (replay 終了)。
        // 0 のときは metadata 未設定 (load 前) と解釈し、自動停止しない。
        const u32 d = m_Metadata.duration_ticks;
        if (d != 0 && m_CurrentTick >= d) {
            m_CurrentTick     = d;
            m_Mode             = EReplayMode::Idle;
            m_TickAccumulator = 0.0f;
        }
        return;
    }

    // Paused / Idle: no-op (Tick が呼ばれても何もしない)。
}

// -----------------------------------------------------------------------------
// SaveReplay — container 1 ファイル + atomic write の本実装
// -----------------------------------------------------------------------------
// 手順:
//   1. file_path が null なら kSub_NullPath。
//   2. m_Recorder / m_Lockstep の SaveToBuffer 出力を取得 (未注入なら size 0)。
//      SaveToBuffer は buffer 不足を size 報告なしで返すため、上限見積もり
//      (16 + count*32 + 8) で一括確保してから out_written に trim する。
//   3. container body を [magic][version][metadata][input_blob][lockstep_blob]
//      の順で TArray<u8> に組み立て、末尾に body 全体の CRC32 を付ける。
//   4. `<path>.tmp` に書き、FlushFileBuffers → MoveFileExW で atomic rename。
// -----------------------------------------------------------------------------
TResult<void> FReplayDirector::SaveReplay(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) {
        return ACS_ERR(IO, kSub_NullPath,
                       "FReplayDirector::SaveReplay: file_path is null");
    }

    // ---- input_blob を作る (FInputRecorder::SaveToBuffer) ----------------
    TArray<u8> input_blob;
    if (m_Recorder != nullptr) {
        // 上限見積もり: 16 (header) + sample_count*32 + 8 (footer)。SaveToBuffer は
        // buffer 不足時に必要量を報告しないため、生成的に確保してから trim する。
        const u64 cap = 16ull
                      + static_cast<u64>(m_Recorder->SampleCount()) * 32ull
                      + 8ull;
        input_blob.Resize(static_cast<usize>(cap));
        u32 written = 0;
        TResult<void> r = m_Recorder->SaveToBuffer(input_blob.Data(),
                                                   static_cast<u32>(cap),
                                                   written);
        if (r.IsErr()) {
            return r.Error();  // FInputRecorder 側の subcode をそのまま伝搬
        }
        input_blob.Resize(static_cast<usize>(written));  // 実書き込み量に trim
    }

    // ---- lockstep_blob を作る (FLockstep::SaveToBuffer) ------------------
    TArray<u8> lockstep_blob;
    if (m_Lockstep != nullptr) {
        // 上限見積もり: 16 (header) + frame_count*32 + 8 (footer)。InputFrame は
        // ~20B だが 32B 上限で十分な余裕を取る (InputRecorder と同じ流儀)。
        const u64 cap = 16ull
                      + static_cast<u64>(m_Lockstep->InputCount()) * 32ull
                      + 8ull;
        lockstep_blob.Resize(static_cast<usize>(cap));
        u32 written = 0;
        TResult<void> r = m_Lockstep->SaveToBuffer(lockstep_blob.Data(),
                                                   static_cast<u32>(cap),
                                                   written);
        if (r.IsErr()) {
            return r.Error();
        }
        lockstep_blob.Resize(static_cast<usize>(written));
    }

    // ---- container body を組み立てる ------------------------------------
    TArray<u8> body;
    // magic + version
    AppendBytes(body, kReplayMagic, sizeof(kReplayMagic));
    AppendU32(body, kReplayVersion);
    // metadata: 数値 (LE) → 文字列 (len-prefixed)
    AppendU64(body, m_Metadata.seed);
    AppendU64(body, m_Metadata.timestamp);
    AppendU32(body, m_Metadata.duration_ticks);
    AppendLenPrefixedString(body, m_Metadata.game_version);
    AppendLenPrefixedString(body, m_Metadata.level_id);
    AppendLenPrefixedString(body, m_Metadata.player_name);
    AppendLenPrefixedString(body, m_Metadata.checksum_hex);
    // blobs: [size u32][bytes]
    AppendU32(body, static_cast<u32>(input_blob.Size()));
    AppendBytes(body, input_blob.Data(), static_cast<u32>(input_blob.Size()));
    AppendU32(body, static_cast<u32>(lockstep_blob.Size()));
    AppendBytes(body, lockstep_blob.Data(), static_cast<u32>(lockstep_blob.Size()));
    // footer: CRC32 over body so far (magic を含む footer 直前まで全体)
    const u32 crc = ComputeCrc32(body.Data(), static_cast<u64>(body.Size()));
    AppendU32(body, crc);

    // ---- `<path>.tmp` を組む --------------------------------------------
    wchar_t tmp_path[1024];
    if (!MakeTmpPath(file_path, tmp_path, 1024)) {
        return ACS_ERR(IO, kSub_PathTooLong,
                       "FReplayDirector::SaveReplay: file path too long for .tmp suffix");
    }

    // ---- tmp に書き込む (CREATE_ALWAYS = 既存 truncate) -----------------
    HANDLE h = ::CreateFileW(tmp_path,
                             GENERIC_WRITE,
                             0,            // 排他: 書き込み中は他者に開かせない
                             nullptr,
                             CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        return ACS_ERR_OS(IO, kSub_Io,
                          "FReplayDirector::SaveReplay: CreateFileW (.tmp) failed", err);
    }

    DWORD err = 0;
    if (!WriteAll(h, body.Data(), static_cast<u64>(body.Size()), err)) {
        ::CloseHandle(h);
        ::DeleteFileW(tmp_path);  // 失敗した tmp は残さない (best-effort)
        return ACS_ERR_OS(IO, kSub_Io,
                          "FReplayDirector::SaveReplay: WriteFile (.tmp) failed", err);
    }

    ::FlushFileBuffers(h);
    if (!::CloseHandle(h)) {
        const DWORD close_err = ::GetLastError();
        ::DeleteFileW(tmp_path);
        return ACS_ERR_OS(IO, kSub_Io,
                          "FReplayDirector::SaveReplay: CloseHandle (.tmp) failed", close_err);
    }

    // ---- atomic rename: tmp → 本パス ------------------------------------
    if (!::MoveFileExW(tmp_path, file_path,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD move_err = ::GetLastError();
        ::DeleteFileW(tmp_path);
        return ACS_ERR_OS(IO, kSub_Io,
                          "FReplayDirector::SaveReplay: MoveFileExW (rename) failed", move_err);
    }
    return Ok();
}

// -----------------------------------------------------------------------------
// LoadReplay — container 検証 + metadata / blob 復元の本実装
// -----------------------------------------------------------------------------
// 手順:
//   1. file_path が null なら kSub_NullPath。
//   2. ファイルを全読みし、magic / version / CRC32 を検証。
//   3. metadata を復元 (文字列は director 所有の m_*Owned バッファに複製し、
//      m_Metadata.* をその Data() に向ける = 寿命を director に束ねる)。
//   4. 注入済み source があれば input_blob / lockstep_blob を LoadFromBuffer へ。
//   5. m_Mode = Idle / m_CurrentTick = 0 にして StartPlayback 待ち状態にする。
// -----------------------------------------------------------------------------
TResult<void> FReplayDirector::LoadReplay(const wchar_t* file_path) noexcept {
    if (file_path == nullptr) {
        return ACS_ERR(IO, kSub_NullPath,
                       "FReplayDirector::LoadReplay: file_path is null");
    }

    // ---- ファイルを開く (FSaveArchive.cpp と同流儀) ---------------------
    HANDLE h = ::CreateFileW(file_path,
                             GENERIC_READ,
                             FILE_SHARE_READ,
                             nullptr,
                             OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        return ACS_ERR_OS(IO, kSub_Io,
                          "FReplayDirector::LoadReplay: CreateFileW failed", err);
    }

    LARGE_INTEGER size_li{};
    if (!::GetFileSizeEx(h, &size_li)) {
        const DWORD err = ::GetLastError();
        ::CloseHandle(h);
        return ACS_ERR_OS(IO, kSub_Io,
                          "FReplayDirector::LoadReplay: GetFileSizeEx failed", err);
    }
    const u64 size_u64 = static_cast<u64>(size_li.QuadPart);
    // 最小サイズ = magic(4) + version(4) + seed(8) + timestamp(8) + duration(4)
    //            + 4 文字列 len(4*4) + 2 blob size(4*2) + crc(4) = 56 B。
    constexpr u64 kMinContainerBytes = 56ull;
    if (size_u64 < kMinContainerBytes) {
        ::CloseHandle(h);
        return ACS_ERR(IO, kSub_BadSize,
                       "FReplayDirector::LoadReplay: file smaller than minimal container");
    }
    if (size_u64 > kMaxContainerBytes) {
        ::CloseHandle(h);
        return ACS_ERR(IO, kSub_BadSize,
                       "FReplayDirector::LoadReplay: container exceeds 256 MiB sanity limit");
    }

    // ---- 全読み込み -----------------------------------------------------
    const usize buf_size = static_cast<usize>(size_u64);
    FAllocator& alloc    = DefaultAllocator();
    void* raw = alloc.Alloc(buf_size, alignof(u8), FSourceLoc::Current());
    if (raw == nullptr) {
        ::CloseHandle(h);
        return ACS_ERR(Memory, kSub_Oom,
                       "FReplayDirector::LoadReplay: failed to allocate read buffer");
    }
    u8* buf = static_cast<u8*>(raw);

    DWORD err = 0;
    if (!ReadAll(h, buf, size_u64, err)) {
        alloc.Free(raw);
        ::CloseHandle(h);
        return ACS_ERR_OS(IO, kSub_Io,
                          "FReplayDirector::LoadReplay: ReadFile failed", err);
    }
    ::CloseHandle(h);

    // ---- magic 検証 -----------------------------------------------------
    if (MemCmp(buf, kReplayMagic, sizeof(kReplayMagic)) != 0) {
        alloc.Free(raw);
        return ACS_ERR(IO, kSub_BadMagic,
                       "FReplayDirector::LoadReplay: magic mismatch (not an ACRP container)");
    }

    // ---- version 検証 ---------------------------------------------------
    const u32 version = ReadU32LE(buf + 4);
    if (version != kReplayVersion) {
        alloc.Free(raw);
        return ACS_ERR(IO, kSub_BadVersion,
                       "FReplayDirector::LoadReplay: unsupported container version");
    }

    // ---- CRC32 検証 (footer 4B を除いた body 全体) -----------------------
    const u64 body_size = size_u64 - kCrcFooterSize;
    const u32 actual_crc = ComputeCrc32(buf, body_size);
    const u32 stored_crc = ReadU32LE(buf + body_size);
    if (actual_crc != stored_crc) {
        alloc.Free(raw);
        return ACS_ERR(IO, kSub_BadCrc,
                       "FReplayDirector::LoadReplay: CRC32 mismatch (corrupt or tampered)");
    }

    // ---- bounded reader で body を順に parse ----------------------------
    // off は読み出しカーソル。limit = body_size (footer は読まない)。各読み出し前に
    // 残量を検査し、不足なら kSub_BadSize で打ち切る (破損 container 防御)。
    u64 off = sizeof(kReplayMagic) + sizeof(u32);  // magic + version の直後
    const u64 limit = body_size;

    auto need = [&](u64 n) noexcept -> bool { return off + n <= limit; };

    // 数値 metadata
    if (!need(8 + 8 + 4)) {
        alloc.Free(raw);
        return ACS_ERR(IO, kSub_BadSize,
                       "FReplayDirector::LoadReplay: truncated metadata header");
    }
    const u64 seed           = ReadU64LE(buf + off); off += 8;
    const u64 timestamp      = ReadU64LE(buf + off); off += 8;
    const u32 duration_ticks = ReadU32LE(buf + off); off += 4;

    // 文字列 metadata (len-prefixed) を director 所有バッファへ復元する内部 helper。
    // 成功で true、サイズ矛盾 / 上限超過で false。
    auto read_string = [&](FString& owned) noexcept -> bool {
        if (!need(sizeof(u32))) return false;
        const u32 len = ReadU32LE(buf + off);
        off += sizeof(u32);
        if (len > kMaxStringLen) return false;
        if (!need(len)) return false;
        owned.Clear();
        if (len > 0) {
            owned.Append(FStringView(reinterpret_cast<const char*>(buf + off), len));
        }
        off += len;
        return true;
    };

    if (!read_string(m_GameVersionOwned) ||
        !read_string(m_LevelIdOwned) ||
        !read_string(m_PlayerNameOwned) ||
        !read_string(m_ChecksumHexOwned)) {
        alloc.Free(raw);
        return ACS_ERR(IO, kSub_BadSize,
                       "FReplayDirector::LoadReplay: truncated / oversized metadata string");
    }

    // ---- 2 blob を読み出す ----------------------------------------------
    if (!need(sizeof(u32))) {
        alloc.Free(raw);
        return ACS_ERR(IO, kSub_BadSize,
                       "FReplayDirector::LoadReplay: truncated input_blob size");
    }
    const u32 input_blob_size = ReadU32LE(buf + off);
    off += sizeof(u32);
    if (!need(input_blob_size)) {
        alloc.Free(raw);
        return ACS_ERR(IO, kSub_BadSize,
                       "FReplayDirector::LoadReplay: input_blob exceeds container");
    }
    const u8* input_blob_ptr = buf + off;
    off += input_blob_size;

    if (!need(sizeof(u32))) {
        alloc.Free(raw);
        return ACS_ERR(IO, kSub_BadSize,
                       "FReplayDirector::LoadReplay: truncated lockstep_blob size");
    }
    const u32 lockstep_blob_size = ReadU32LE(buf + off);
    off += sizeof(u32);
    if (!need(lockstep_blob_size)) {
        alloc.Free(raw);
        return ACS_ERR(IO, kSub_BadSize,
                       "FReplayDirector::LoadReplay: lockstep_blob exceeds container");
    }
    const u8* lockstep_blob_ptr = buf + off;
    off += lockstep_blob_size;

    // ---- 注入済み source に blob を流す ---------------------------------
    // source 未注入の側は blob を読み飛ばす (container は valid)。LoadFromBuffer の
    // Err はそのまま伝搬する (破損 blob は復元失敗として呼出側に返す)。
    if (m_Recorder != nullptr && input_blob_size > 0) {
        TResult<void> r = m_Recorder->LoadFromBuffer(input_blob_ptr, input_blob_size);
        if (r.IsErr()) {
            alloc.Free(raw);
            return r.Error();
        }
    }
    if (m_Lockstep != nullptr && lockstep_blob_size > 0) {
        TResult<void> r = m_Lockstep->LoadFromBuffer(lockstep_blob_ptr, lockstep_blob_size);
        if (r.IsErr()) {
            alloc.Free(raw);
            return r.Error();
        }
    }

    // ---- metadata を確定 (文字列は m_*Owned の NUL 終端 Data() を指す) ----
    // FString::Data() は常に NUL 終端なので const char* として安全。空文字列も
    // "" を指す有効ポインタを返すため、null ではなく空文字列として復元される。
    m_Metadata.seed           = seed;
    m_Metadata.timestamp      = timestamp;
    m_Metadata.duration_ticks = duration_ticks;
    m_Metadata.game_version   = m_GameVersionOwned.Data();
    m_Metadata.level_id       = m_LevelIdOwned.Data();
    m_Metadata.player_name    = m_PlayerNameOwned.Data();
    m_Metadata.checksum_hex   = m_ChecksumHexOwned.Data();

    alloc.Free(raw);

    // ---- 再生待ち状態へ -------------------------------------------------
    m_Mode             = EReplayMode::Idle;
    m_CurrentTick     = 0;
    m_TickAccumulator = 0.0f;
    return Ok();
}

} // namespace acs::game

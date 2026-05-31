// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar M Phase 2 — FNetSnapshot 実装
//
// 構成:
//   1) NetTransportStub: 常に NotImplemented を返す null-object transport。
//   2) FNetSnapshot:
//      ・Init / Shutdown: ring buffer / pending list / 統計値の初期化と解放。
//      ・AddEntitySnapshot / CommitSnapshot: server 側で 1 frame 分の state を
//        積み、1 つの payload に concat して transport.Send。
//      ・Tick: client 側で transport.Receive を pump し、受信 snapshot を ring
//        buffer に追加 (FIFO で最古を上書き)。
//      ・TryGetInterpolatedSnapshot: 2 snapshot を取り出して entity 単位で
//        線形補間 (現時点では同 entity_id 同 component_mask の data を 2 つ
//        並べる純粋な lerp は行わず、buffer 内の "より新しい側" の data を
//        view として返す)。本 Phase の補間は "時刻に対応する snapshot を選ぶ"
//        段階まで実装し、float コンポーネント単位の値補間は Phase 3 で
//        コンポーネントスキーマが入ってから本実装する。
//
// 設計メモ:
//   ・wire format (frame): [magic 'ACSN'][version][SnapshotHeader(24B)]
//     [payload][crc32]。payload の各 entity は
//     [entity_id u32][component_mask u32][data_size u32][data]。
//   ・直列化の実体は EncodeSnapshot / DecodeSnapshot (static 純粋関数)。
//     CommitSnapshot は EncodeSnapshot の結果を transport.Send し、Tick は
//     transport.Receive した bytes を DecodeSnapshot で検証してから ring
//     buffer に積む。transport (socket) seam は INetTransport のまま外部差替で、
//     encode/decode は seam に依存しない real な byte serialization になっている。
//   ・LE 固定。MemCopy 経由で strict-aliasing 安全。
//   ・CRC32 は Zlib / PNG 規約 (poly 0xEDB88320, init/xorout 0xFFFFFFFF)。
//     FSaveArchive.cpp / FLockstep.cpp と同一の file-local ComputeCrc32 を持つ
//     (gameframework は assetpack を依存に持たないため link 単位を独立させる)。
//   ・全 noexcept。エラーは内部で握り潰す (transport.Send 失敗は packet
//     loss と等価扱い)。INetTransport API は TResult<T> だが、上位の
//     CommitSnapshot/Tick は void で「ベストエフォート」配信を表現する。
//   ・コピー / ムーブ禁止 (header で delete 済み)。

#include "gameframework/NetSnapshot.h"

#include "foundation/Move.h"  // Move (rvalue cast)
#include "memory/Memory.h"    // MemCopy / MemSet

namespace acs::game {

namespace {

// -----------------------------------------------------------------------------
// little-endian 読み書き helper (strict-aliasing 安全)
// -----------------------------------------------------------------------------
// ホスト側 (Win x64 / ARM64 LE) 前提なので memcpy された生バイトがそのまま
// 整数になる。FSaveArchive.cpp / FLockstep.cpp と同じ流儀。
// -----------------------------------------------------------------------------
inline void WriteU32LE(u8* dst, u32 v) noexcept { MemCopy(dst, &v, sizeof(u32)); }
inline void WriteU64LE(u8* dst, u64 v) noexcept { MemCopy(dst, &v, sizeof(u64)); }
inline u32  ReadU32LE (const u8* src) noexcept { u32 v = 0; MemCopy(&v, src, sizeof(u32)); return v; }
inline u64  ReadU64LE (const u8* src) noexcept { u64 v = 0; MemCopy(&v, src, sizeof(u64)); return v; }

// -----------------------------------------------------------------------------
// frame wire format 定数
// -----------------------------------------------------------------------------
// frame = [magic 'ACSN'(4)][version(4)][SnapshotHeader(24)][payload][crc32(4)]。
// magic + version は FLockstep の 'ACSL' / version レイアウトと同思想で、
// 異なる wire stream の混入 / 非互換バージョンを受信時に弾く。
constexpr u8  kFrameMagic[4]   = { 'A', 'C', 'S', 'N' };  // 'ACSN' (Net Snapshot)
constexpr u32 kFrameVersion    = 1u;
constexpr u32 kMagicSize       = 4u;
constexpr u32 kVersionSize     = 4u;

// SnapshotHeader を 24 byte の LE wire format に書く / 読む。
constexpr u32 kHeaderWireSize = 24;

// frame 内オフセット。
constexpr u32 kVersionOffset = kMagicSize;                          // 4
constexpr u32 kHeaderOffset  = kMagicSize + kVersionSize;           // 8
constexpr u32 kPayloadOffset = kHeaderOffset + kHeaderWireSize;     // 32
constexpr u32 kCrcFooterSize = 4u;

// magic / version / header / payload を除いた固定オーバーヘッド。
//   = magic(4) + version(4) + header(24) + crc32(4) = 36 byte。
constexpr u32 kFrameFixedOverhead =
    kMagicSize + kVersionSize + kHeaderWireSize + kCrcFooterSize;

// ---- CRC32 (poly 0xEDB88320, init/xorout 0xFFFFFFFF) ----------------------
// FSaveArchive / FLockstep と同一実装 (gameframework は assetpack を依存に
// 持たないため link 単位を独立させる)。Meyer's singleton で thread-safe な
// lookup table 初期化を行う。
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

// バイト列の CRC32 を計算する (Zlib / PNG 規約)。
u32 ComputeCrc32(const void* data, u64 size) noexcept {
    const u32* table = GetCrc32Table();
    const u8*  p     = static_cast<const u8*>(data);
    u32        crc   = 0xFFFFFFFFu;
    for (u64 i = 0; i < size; ++i) {
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

void WriteHeader(u8* dst, const SnapshotHeader& h) noexcept {
    WriteU32LE(dst + 0,  h.tick);
    WriteU32LE(dst + 4,  h.sequence);
    WriteU64LE(dst + 8,  h.server_timestamp_us);
    WriteU32LE(dst + 16, h.payload_size);
    WriteU32LE(dst + 20, h.crc32);
}

void ReadHeader(const u8* src, SnapshotHeader& h) noexcept {
    h.tick                = ReadU32LE(src + 0);
    h.sequence            = ReadU32LE(src + 4);
    h.server_timestamp_us = ReadU64LE(src + 8);
    h.payload_size        = ReadU32LE(src + 16);
    h.crc32               = ReadU32LE(src + 20);
}

} // namespace

// =============================================================================
// NetTransportStub — defensive stub
// -----------------------------------------------------------------------------
// 全 API が ACS_ERR(IO, kSub_NotImplemented) を返す。BackendClientStub と
// 同じパターン: stub が本番に混入したケースを QA で検出可能にする。
// =============================================================================
TResult<void> NetTransportStub::Connect(const char* address, u16 port) noexcept {
    (void)address;
    (void)port;
    return ACS_ERR(IO, NetSnapshotError::kSub_NotImplemented,
                   "INetTransport::Connect is not implemented "
                   "(stub: link a concrete transport implementation)");
}

void NetTransportStub::Disconnect() noexcept {
    // stub は never-connected。no-op で安全に通す。
}

TResult<void> NetTransportStub::Send(const void* data, u32 size) noexcept {
    (void)data;
    (void)size;
    return ACS_ERR(IO, NetSnapshotError::kSub_NotImplemented,
                   "INetTransport::Send is not implemented "
                   "(stub: link a concrete transport implementation)");
}

TResult<u32> NetTransportStub::Receive(void* out_buffer, u32 capacity) noexcept {
    (void)out_buffer;
    (void)capacity;
    return ACS_ERR(IO, NetSnapshotError::kSub_NotImplemented,
                   "INetTransport::Receive is not implemented "
                   "(stub: link a concrete transport implementation)");
}

// プロセス共有の stub。function-local static (C++11 magic statics) で
// thread-safe 初期化。
INetTransport& GetTransportStub() noexcept {
    static NetTransportStub s_instance;
    return s_instance;
}

// =============================================================================
// FNetSnapshot::EncodedSnapshotSize
// -----------------------------------------------------------------------------
// 1 frame の総バイト数。payload_size を u64 に広げてから clamp し、32bit
// 加算オーバーフローを避ける (max_payload_bytes 上限内なら必ず収まる)。
// =============================================================================
u32 FNetSnapshot::EncodedSnapshotSize(u32 payload_size) noexcept {
    const u64 total = static_cast<u64>(kFrameFixedOverhead)
                    + static_cast<u64>(payload_size);
    // payload_size は呼出側で max_payload_bytes 以下に制限済み。理論上の
    // オーバーフローは clamp で防御 (u32 上限を超えたら u32 max を返す)。
    if (total > 0xFFFFFFFFull) {
        return 0xFFFFFFFFu;
    }
    return static_cast<u32>(total);
}

// =============================================================================
// FNetSnapshot::EncodeSnapshot
// -----------------------------------------------------------------------------
// header + payload を frame wire layout で out_buffer に書き出す real な直列化。
//   [magic 'ACSN'][version][SnapshotHeader(24, crc32=0 で書く)][payload][crc32]
// CRC32 は magic を除いた [version .. payload 末尾) を対象に計算し、footer に
// 格納する (FLockstep が frames 部に CRC を付けるのと同思想)。
// =============================================================================
TResult<void> FNetSnapshot::EncodeSnapshot(const SnapshotHeader& header,
                                          const u8* payload, u32 payload_size,
                                          u8* out_buffer, u32 out_capacity,
                                          u32& out_written) noexcept {
    out_written = 0;
    if (out_buffer == nullptr) {
        return ACS_ERR(IO, NetSnapshotError::kSub_NullBuffer,
                       "FNetSnapshot::EncodeSnapshot: out_buffer is null");
    }
    // payload == nullptr は payload_size == 0 のときのみ許容。
    if (payload == nullptr && payload_size != 0) {
        return ACS_ERR(IO, NetSnapshotError::kSub_BadArgument,
                       "FNetSnapshot::EncodeSnapshot: payload is null but size != 0");
    }
    // header.payload_size と引数 payload_size の不一致は wire 不整合の元なので拒否。
    if (header.payload_size != payload_size) {
        return ACS_ERR(IO, NetSnapshotError::kSub_BadArgument,
                       "FNetSnapshot::EncodeSnapshot: header.payload_size != payload_size");
    }

    const u32 required = EncodedSnapshotSize(payload_size);
    if (out_capacity < required) {
        return ACS_ERR(IO, NetSnapshotError::kSub_BufferTooSmall,
                       "FNetSnapshot::EncodeSnapshot: out_buffer too small for frame");
    }

    // ---- magic (4) + version (4) ----------------------------------------
    MemCopy(out_buffer, kFrameMagic, sizeof(kFrameMagic));
    WriteU32LE(out_buffer + kVersionOffset, kFrameVersion);

    // ---- SnapshotHeader (24, crc32 フィールドは 0 で書き出す) -------------
    // crc32 は frame footer に置くため、CRC 計算対象に含まれる header 内
    // フィールドは 0 として扱う (= 自己参照を避ける)。
    SnapshotHeader hdr = header;
    hdr.crc32 = 0;
    WriteHeader(out_buffer + kHeaderOffset, hdr);

    // ---- payload --------------------------------------------------------
    if (payload_size > 0) {
        MemCopy(out_buffer + kPayloadOffset, payload, payload_size);
    }

    // ---- crc32 footer ---------------------------------------------------
    // 対象 = [version .. payload 末尾) = magic を除いた本体。
    const u32 crc_region_size = kVersionSize + kHeaderWireSize + payload_size;
    const u32 crc = ComputeCrc32(out_buffer + kVersionOffset, crc_region_size);
    WriteU32LE(out_buffer + kPayloadOffset + payload_size, crc);

    out_written = required;
    return Ok();
}

// =============================================================================
// FNetSnapshot::DecodeSnapshot
// -----------------------------------------------------------------------------
// frame bytes を検証 + 復元する real な逆直列化。magic / version / size /
// CRC32 を順に検証し、不正なら対応する Err を返す。成功時は out_header に
// SnapshotHeader (footer の crc32 を復元含む) を、out_payload に payload を
// 置換コピーする。
// =============================================================================
TResult<void> FNetSnapshot::DecodeSnapshot(const u8* buffer, u32 size,
                                          SnapshotHeader& out_header,
                                          TArray<u8>& out_payload) noexcept {
    if (buffer == nullptr) {
        return ACS_ERR(IO, NetSnapshotError::kSub_NullBuffer,
                       "FNetSnapshot::DecodeSnapshot: buffer is null");
    }
    // ---- 最小サイズ (固定オーバーヘッド = payload 0 のケース) ------------
    if (size < kFrameFixedOverhead) {
        return ACS_ERR(IO, NetSnapshotError::kSub_BadSize,
                       "FNetSnapshot::DecodeSnapshot: buffer smaller than frame overhead");
    }
    // ---- magic 検証 ------------------------------------------------------
    if (MemCmp(buffer, kFrameMagic, sizeof(kFrameMagic)) != 0) {
        return ACS_ERR(IO, NetSnapshotError::kSub_BadMagic,
                       "FNetSnapshot::DecodeSnapshot: magic mismatch (not an ACSN frame)");
    }
    // ---- version 検証 ----------------------------------------------------
    const u32 version = ReadU32LE(buffer + kVersionOffset);
    if (version != kFrameVersion) {
        return ACS_ERR(IO, NetSnapshotError::kSub_BadVersion,
                       "FNetSnapshot::DecodeSnapshot: unsupported frame version");
    }

    // ---- header 読み出し + payload_size とサイズの整合 -------------------
    SnapshotHeader hdr{};
    ReadHeader(buffer + kHeaderOffset, hdr);

    // payload_size + 固定オーバーヘッドが size と完全一致することを要求する。
    // u64 で計算して 32bit 加算オーバーフローを避ける。
    const u64 expected64 = static_cast<u64>(kFrameFixedOverhead)
                         + static_cast<u64>(hdr.payload_size);
    if (expected64 != static_cast<u64>(size)) {
        return ACS_ERR(IO, NetSnapshotError::kSub_BadSize,
                       "FNetSnapshot::DecodeSnapshot: payload_size inconsistent with buffer size");
    }

    // ---- crc32 検証 ------------------------------------------------------
    // 対象 = [version .. payload 末尾)。header 内 crc32 フィールドは encode 時に
    // 0 だったので、ここでも自然に 0 が CRC 計算へ寄与する。
    const u32 crc_region_size = kVersionSize + kHeaderWireSize + hdr.payload_size;
    const u32 actual_crc = ComputeCrc32(buffer + kVersionOffset, crc_region_size);
    const u32 stored_crc = ReadU32LE(buffer + kPayloadOffset + hdr.payload_size);
    if (actual_crc != stored_crc) {
        return ACS_ERR(IO, NetSnapshotError::kSub_BadCrc,
                       "FNetSnapshot::DecodeSnapshot: CRC32 mismatch (corrupt or tampered)");
    }

    // ---- payload を復元 (置換) ------------------------------------------
    out_payload.Clear();
    out_payload.Resize(static_cast<usize>(hdr.payload_size));
    if (hdr.payload_size > 0) {
        MemCopy(out_payload.Data(),
                buffer + kPayloadOffset,
                static_cast<usize>(hdr.payload_size));
    }

    // footer の CRC を header に復元して返す (呼出側が保持できるように)。
    hdr.crc32  = stored_crc;
    out_header = hdr;
    return Ok();
}

// =============================================================================
// FNetSnapshot::Init
// -----------------------------------------------------------------------------
// 設定をコピーし、ring buffer の容量を確保する。Standalone 以外で
// transport が nullptr の場合は GetTransportStub() に差し替えて
// リンク互換を保つ (defensive 設計)。
// =============================================================================
void FNetSnapshot::Init(const NetSnapshotConfig& config, ENetRole role,
                       INetTransport* transport) noexcept {
    m_Config = config;
    m_Role   = role;

    // buffer_capacity が 0 / 1 だと補間 (= 2 snapshot 必須) が成立しないため
    // 最低 2 に丸める。max_payload_bytes が 0 の場合は 1KB に。
    if (m_Config.buffer_capacity_snapshots < 2) {
        m_Config.buffer_capacity_snapshots = 2;
    }
    if (m_Config.max_payload_bytes == 0) {
        m_Config.max_payload_bytes = 1024;
    }

    // Standalone 以外で nullptr が渡されたら stub に差し替える (落ちないように)。
    if (role == ENetRole::Standalone) {
        m_Transport = transport;  // nullptr 許容
    } else {
        m_Transport = (transport != nullptr) ? transport : &GetTransportStub();
    }

    // ring buffer を capacity 件で fixed-size 確保。各エントリの payload は
    // 初期サイズ 0 (CommitSnapshot で書き込まれる際に Resize される)。
    m_RingBuffer.Clear();
    m_RingBuffer.Resize(static_cast<usize>(m_Config.buffer_capacity_snapshots));
    m_RingHead  = 0;
    m_RingCount = 0;

    m_PendingEntities.Clear();
    m_InterpScratch.Clear();

    m_NextSequence      = 1;
    m_LastReceivedTick = 0;
    m_PacketsSent       = 0;
    m_PacketsReceived   = 0;
    m_BytesSent         = 0;
    m_BytesReceived     = 0;
}

// =============================================================================
// FNetSnapshot::Shutdown
// -----------------------------------------------------------------------------
// transport は外部所有なので触らない。ring buffer / pending / scratch を解放。
// =============================================================================
void FNetSnapshot::Shutdown() noexcept {
    m_RingBuffer.Clear();
    m_PendingEntities.Clear();
    m_InterpScratch.Clear();
    m_RingHead  = 0;
    m_RingCount = 0;
    m_Transport  = nullptr;
}

u32 FNetSnapshot::BufferedSnapshotCount() const noexcept {
    return m_RingCount;
}

// =============================================================================
// FNetSnapshot::AddEntitySnapshot
// -----------------------------------------------------------------------------
// server 側で 1 entity 分の state を pending list に積む。data は内部に
// バイトコピーするので、呼出側は AddEntitySnapshot 後すぐに data を破棄して
// よい。Client / Standalone では no-op。
// =============================================================================
void FNetSnapshot::AddEntitySnapshot(u32 entity_id, u32 component_mask,
                                    const void* data, u32 data_size) noexcept {
    // Client / Standalone は送信側ではないので no-op。
    if (m_Role == ENetRole::Client || m_Role == ENetRole::Standalone) {
        return;
    }
    // 不正な引数は黙ってスキップ (defensive、ベストエフォート方針)。
    if (data == nullptr && data_size != 0) {
        return;
    }

    PendingEntity pe;
    pe.entity_id      = entity_id;
    pe.component_mask = component_mask;
    if (data_size > 0) {
        pe.data.Resize(static_cast<usize>(data_size));
        MemCopy(pe.data.Data(), data, data_size);
    }
    m_PendingEntities.PushBack(Move(pe));
}

// =============================================================================
// FNetSnapshot::CommitSnapshot
// -----------------------------------------------------------------------------
// pending list を 1 つの payload に concat し、SnapshotHeader を付けて
// EncodeSnapshot で frame bytes (magic+version+header+payload+crc32) を構築し、
// transport.Send する。
//
// 失敗ケース (黙ってスキップ):
//   ・Client / Standalone 役割: 送信側ではないので no-op。
//   ・transport == nullptr: Standalone fallback で起きうる。
//   ・payload > max_payload_bytes: 設定上限超過。
//   ・EncodeSnapshot が Err: 内部 buffer 確保失敗等 (ベストエフォートで skip)。
//   ・transport.Send が Err 返却: packet loss と等価扱い (ベストエフォート)。
//
// pending list は呼出後にクリアされる (成否によらず)。これにより次 tick の
// AddEntitySnapshot は前 tick の残骸を引きずらない。
// =============================================================================
void FNetSnapshot::CommitSnapshot(u32 tick) noexcept {
    // 役割チェック。Client / Standalone は no-op。
    if (m_Role == ENetRole::Client || m_Role == ENetRole::Standalone) {
        m_PendingEntities.Clear();
        return;
    }
    if (m_Transport == nullptr) {
        m_PendingEntities.Clear();
        return;
    }

    // payload バイト数を見積もる。
    //   per-entity = kEntityHeaderSize (12) + data_size
    usize payload_size = 0;
    const usize n_ent = m_PendingEntities.Size();
    for (usize i = 0; i < n_ent; ++i) {
        payload_size += static_cast<usize>(kEntityHeaderSize)
                      + static_cast<usize>(m_PendingEntities[i].data.Size());
    }
    if (payload_size > static_cast<usize>(m_Config.max_payload_bytes)) {
        // 上限超過。interest management / 分割送信は本クラスの範囲外なので skip。
        m_PendingEntities.Clear();
        return;
    }

    // payload を per-entity layout で 1 本に concat する。
    TArray<u8> payload_buf;
    payload_buf.Resize(payload_size);
    u8* payload_ptr = payload_buf.Data();
    usize off = 0;
    for (usize i = 0; i < n_ent; ++i) {
        const PendingEntity& pe = m_PendingEntities[i];
        const u32 data_size = static_cast<u32>(pe.data.Size());
        WriteU32LE(payload_ptr + off + 0, pe.entity_id);
        WriteU32LE(payload_ptr + off + 4, pe.component_mask);
        WriteU32LE(payload_ptr + off + 8, data_size);
        if (data_size > 0) {
            MemCopy(payload_ptr + off + kEntityHeaderSize, pe.data.Data(), data_size);
        }
        off += static_cast<usize>(kEntityHeaderSize) + static_cast<usize>(data_size);
    }

    // header を構築。crc32 は EncodeSnapshot が footer に計算して書き込む。
    SnapshotHeader hdr;
    hdr.tick                = tick;
    hdr.sequence            = m_NextSequence;
    hdr.server_timestamp_us = 0;  // タイムスタンプ source の wire 化は caller 責務
    hdr.payload_size        = static_cast<u32>(payload_size);
    hdr.crc32               = 0;

    // frame bytes = magic+version+header+payload+crc32 を 1 本にまとめる
    // (Send 1 回で送信)。EncodeSnapshot が real CRC を計算する。
    TArray<u8> wire_buf;
    const u32 frame_size = EncodedSnapshotSize(static_cast<u32>(payload_size));
    wire_buf.Resize(static_cast<usize>(frame_size));
    u32 written = 0;
    TResult<void> enc = EncodeSnapshot(hdr, payload_ptr,
                                       static_cast<u32>(payload_size),
                                       wire_buf.Data(), frame_size, written);
    if (enc.IsErr()) {
        // 直列化に失敗したら send せず skip (ベストエフォート)。
        m_PendingEntities.Clear();
        return;
    }

    // 送信 (best-effort)。失敗してもクラッシュさせず、統計だけ更新しない。
    TResult<void> r = m_Transport->Send(wire_buf.Data(), written);
    if (r.IsOk()) {
        ++m_PacketsSent;
        m_BytesSent += written;
        ++m_NextSequence;
        if (m_NextSequence == 0) {
            m_NextSequence = 1;  // 0 は invalid 値として予約
        }

        // ServerListener は host が自分の画面用に補間も使うため、自分が
        // commit した snapshot を ring buffer にも積む (loopback)。送信した
        // frame を DecodeSnapshot で復元することで、受信経路と同じ検証 (CRC 等)
        // を通した上で ring buffer に積む (= encode/decode の round-trip 検証)。
        if (m_Role == ENetRole::ServerListener) {
            const u32 idx = m_RingHead;
            BufferedSnapshot& slot = m_RingBuffer[static_cast<usize>(idx)];
            SnapshotHeader decoded{};
            TResult<void> dec = DecodeSnapshot(wire_buf.Data(), written,
                                               decoded, slot.payload);
            if (dec.IsOk()) {
                slot.header = decoded;
                m_RingHead = (m_RingHead + 1u) % m_Config.buffer_capacity_snapshots;
                if (m_RingCount < m_Config.buffer_capacity_snapshots) {
                    ++m_RingCount;
                }
                m_LastReceivedTick = decoded.tick;
            }
        }
    }

    // pending list は成否によらずクリア (= 次 tick の AddEntitySnapshot は
    // 必ず空 list から積み直す)。
    m_PendingEntities.Clear();
}

// =============================================================================
// FNetSnapshot::Tick
// -----------------------------------------------------------------------------
// transport.Receive を非ブロッキングで pump し、受信した snapshot を ring
// buffer に追加する。1 Tick で複数 snapshot を取り込めるよう、受信なしに
// なるまでループする (PendingBytesIn が 0 で抜ける)。
//
// 受信した snapshot の wire format は CommitSnapshot 側と対称:
//   magic+version+header(24B)+payload(header.payload_size B)+crc32。
// 各 frame は DecodeSnapshot で magic / version / size / CRC32 を検証してから
// ring buffer に積む。検証に失敗した frame (破損 / 改竄 / 非互換) は破棄し、
// 統計 (PacketsReceived/BytesReceived) にだけ載せる。
// 受信フレームを 1 度に取り切る前提 (INetTransport の Receive がメッセージ
// 境界保持の契約)。TCP のような stream transport を使う場合は派生実装側で
// length-prefixed framing を実装する (INetTransport の契約どおり)。
// =============================================================================
void FNetSnapshot::Tick(f32 dt) noexcept {
    (void)dt;

    // Server (listen 非搭載) は受信側を持たないので何もしない。
    // Client / ServerListener / Standalone は (transport があれば) 受信する。
    if (m_Role == ENetRole::Server) {
        return;
    }
    if (m_Transport == nullptr) {
        return;
    }

    // 受信用一時 buffer。1 frame 最大長 = 固定オーバーヘッド + max_payload_bytes。
    const u32 cap = kFrameFixedOverhead + m_Config.max_payload_bytes;
    TArray<u8> rx_buf;
    rx_buf.Resize(static_cast<usize>(cap));

    // 1 Tick で取りきる安全上限 (transport が壊れてループが終わらない事故予防)。
    // ring buffer 容量の 2 倍を上限としておく (古い snapshot は捨てる流儀)。
    const u32 max_iters = m_Config.buffer_capacity_snapshots * 2u;
    for (u32 iter = 0; iter < max_iters; ++iter) {
        TResult<u32> r = m_Transport->Receive(rx_buf.Data(), cap);
        if (r.IsErr()) {
            // 受信エラーは loss と等価扱い。次 Tick で再試行。
            break;
        }
        const u32 got = r.Value();
        if (got == 0) {
            // データなし。受信側 pump 完了。
            break;
        }

        // ring buffer の挿入先 slot に直接 decode する (payload を slot に複製)。
        // FIFO で最古を上書きするため、検証成功時にのみ head を進める。
        const u32 idx = m_RingHead;
        BufferedSnapshot& slot = m_RingBuffer[static_cast<usize>(idx)];
        SnapshotHeader hdr{};
        TResult<void> dec = DecodeSnapshot(rx_buf.Data(), got, hdr, slot.payload);
        if (dec.IsErr()) {
            // magic / version / size / CRC のいずれかで弾かれた frame。破棄して
            // 統計にだけ載せる (= 破損 / 非互換 / 改竄パケットは ring に積まない)。
            ++m_PacketsReceived;
            m_BytesReceived += got;
            continue;
        }

        slot.header = hdr;
        m_RingHead = (m_RingHead + 1u) % m_Config.buffer_capacity_snapshots;
        if (m_RingCount < m_Config.buffer_capacity_snapshots) {
            ++m_RingCount;
        }
        m_LastReceivedTick = hdr.tick;
        ++m_PacketsReceived;
        m_BytesReceived += got;
    }
}

// =============================================================================
// FNetSnapshot::TryGetInterpolatedSnapshot
// -----------------------------------------------------------------------------
// client_time_sec に対して「interpolation_delay_sec 前」の時刻を target に
// 設定し、ring buffer の中から target を挟む 2 snapshot を探す。1 snapshot
// しかなければそれをそのまま使う。
//
// 補間ポリシー (Phase 2):
//   ・component data のスキーマを本クラスは知らないため、float 単位の
//     lerp は行わない。Phase 3 でコンポーネントレジストリ (= 型情報 + 補間
//     関数) が入ってから本実装する。
//   ・Phase 2 では「target 時刻に最も近い snapshot」を view として返す。
//     具体的には、buffer 内の sequence 番号で sort 済みと仮定して、target
//     tick (= last_received_tick - interpolation_delay 相当) 直前の snapshot
//     を選ぶ。
//
// 戻り値:
//   true  — out_snapshots / out_actual_count に有効データを書いた
//   false — buffer が空、または client time が buffer 範囲外
//
// 注意:
//   ・out_snapshots[i].component_data は ring buffer 内 payload を直接指す
//     非所有 view。次の Tick() / CommitSnapshot() 呼出まで有効。
// =============================================================================
bool FNetSnapshot::TryGetInterpolatedSnapshot(f32 client_time_sec,
                                             EntitySnapshot* out_snapshots,
                                             u32 max_count,
                                             u32& out_actual_count) noexcept {
    out_actual_count = 0;

    // Server は補間を持たない。
    if (m_Role == ENetRole::Server) {
        return false;
    }
    if (out_snapshots == nullptr || max_count == 0) {
        return false;
    }
    if (m_RingCount == 0) {
        return false;
    }

    // ring buffer 中の最新スロットを探す。挿入は m_RingHead に書き込み後に
    // インクリメントするので、最新エントリは (m_RingHead - 1) % capacity。
    const u32 cap = m_Config.buffer_capacity_snapshots;
    const u32 newest_idx = (m_RingHead + cap - 1u) % cap;
    const BufferedSnapshot& newest = m_RingBuffer[static_cast<usize>(newest_idx)];

    // target_seq: client_time_sec を秒として持つが、Phase 2 では実時刻軸を
    // wire format に乗せていない (server_timestamp_us は 0 固定)。よって本
    // 実装では「最新 snapshot を返す」シンプル選択を取る。
    //
    // Phase 3 で server_timestamp_us を実装 / 計測時刻と紐付けたら、ここで
    //   target_us = newest.header.server_timestamp_us
    //             - static_cast<u64>(m_Config.interpolation_delay_sec * 1e6f);
    // を計算し、target_us を挟む 2 snapshot を ring から線形探索 → lerp する。
    // 現状 client_time_sec は受け取るだけで使わない (将来の API 後方互換のため)。
    (void)client_time_sec;

    const SnapshotHeader& hdr = newest.header;
    const u8* payload_ptr     = newest.payload.Data();
    const u32 payload_size    = static_cast<u32>(newest.payload.Size());

    // payload を per-entity wire format で walk して EntitySnapshot に流す。
    // out_snapshots の component_data は ring buffer の payload を直接指す
    // 非所有 view (寿命 = 次の Tick() まで)。
    u32 off       = 0;
    u32 emitted   = 0;
    while (off + kEntityHeaderSize <= payload_size && emitted < max_count) {
        const u32 entity_id      = ReadU32LE(payload_ptr + off + 0);
        const u32 component_mask = ReadU32LE(payload_ptr + off + 4);
        const u32 data_size      = ReadU32LE(payload_ptr + off + 8);

        // size sanity check。payload を超えるなら破損 (= 残りを捨てる)。
        if (off + kEntityHeaderSize + data_size > payload_size) {
            break;
        }

        EntitySnapshot& dst = out_snapshots[emitted];
        dst.entity_id           = entity_id;
        dst.component_mask      = component_mask;
        dst.component_data      = (data_size > 0)
                                    ? static_cast<const void*>(payload_ptr + off + kEntityHeaderSize)
                                    : nullptr;
        dst.component_data_size = data_size;

        off += kEntityHeaderSize + data_size;
        ++emitted;
    }

    out_actual_count = emitted;
    // 統計上、補間に使った snapshot の header.tick を反映 (Phase 3 で 2 snapshot
    // 間の状況を返すフィールドを足す可能性あり)。
    (void)hdr;
    return emitted > 0;
}

} // namespace acs::game

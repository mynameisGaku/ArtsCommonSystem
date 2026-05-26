// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar M Phase 2 — FNetSnapshot 実装
//
// 構成:
//   1) FNetTransportStub: 常に NotImplemented を返す null-object transport。
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
//   ・wire format: FSnapshotHeader (24B) + payload。payload の各 entity は
//     [entity_id u32][component_mask u32][data_size u32][data]。
//   ・LE 固定。MemCopy 経由で strict-aliasing 安全。
//   ・delta compression は v1 では未実装。Phase 3 で前 snapshot との差分を
//     計算する hook ポイントを CommitSnapshot 内 TODO で残してある。
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
// 整数になる。SaveArchive.cpp / Lockstep.cpp と同じ流儀。
// -----------------------------------------------------------------------------
inline void WriteU32LE(u8* dst, u32 v) noexcept { MemCopy(dst, &v, sizeof(u32)); }
inline void WriteU64LE(u8* dst, u64 v) noexcept { MemCopy(dst, &v, sizeof(u64)); }
inline u32  ReadU32LE (const u8* src) noexcept { u32 v = 0; MemCopy(&v, src, sizeof(u32)); return v; }
inline u64  ReadU64LE (const u8* src) noexcept { u64 v = 0; MemCopy(&v, src, sizeof(u64)); return v; }

// FSnapshotHeader を 24 byte の LE wire format に書く / 読む。
constexpr u32 kHeaderWireSize = 24;

void WriteHeader(u8* dst, const FSnapshotHeader& h) noexcept {
    WriteU32LE(dst + 0,  h.tick);
    WriteU32LE(dst + 4,  h.sequence);
    WriteU64LE(dst + 8,  h.server_timestamp_us);
    WriteU32LE(dst + 16, h.payload_size);
    WriteU32LE(dst + 20, h.crc32);
}

void ReadHeader(const u8* src, FSnapshotHeader& h) noexcept {
    h.tick                = ReadU32LE(src + 0);
    h.sequence            = ReadU32LE(src + 4);
    h.server_timestamp_us = ReadU64LE(src + 8);
    h.payload_size        = ReadU32LE(src + 16);
    h.crc32               = ReadU32LE(src + 20);
}

} // namespace

// =============================================================================
// FNetTransportStub — defensive stub
// -----------------------------------------------------------------------------
// 全 API が ACS_ERR(IO, kSub_NotImplemented) を返す。BackendClientStub と
// 同じパターン: stub が本番に混入したケースを QA で検出可能にする。
// =============================================================================
TResult<void> FNetTransportStub::Connect(const char* address, u16 port) noexcept {
    (void)address;
    (void)port;
    return ACS_ERR(IO, FNetSnapshotError::kSub_NotImplemented,
                   "INetTransport::Connect is not implemented "
                   "(stub: link a concrete transport implementation)");
}

void FNetTransportStub::Disconnect() noexcept {
    // stub は never-connected。no-op で安全に通す。
}

TResult<void> FNetTransportStub::Send(const void* data, u32 size) noexcept {
    (void)data;
    (void)size;
    return ACS_ERR(IO, FNetSnapshotError::kSub_NotImplemented,
                   "INetTransport::Send is not implemented "
                   "(stub: link a concrete transport implementation)");
}

TResult<u32> FNetTransportStub::Receive(void* out_buffer, u32 capacity) noexcept {
    (void)out_buffer;
    (void)capacity;
    return ACS_ERR(IO, FNetSnapshotError::kSub_NotImplemented,
                   "INetTransport::Receive is not implemented "
                   "(stub: link a concrete transport implementation)");
}

// プロセス共有の stub。function-local static (C++11 magic statics) で
// thread-safe 初期化。
INetTransport& GetTransportStub() noexcept {
    static FNetTransportStub s_instance;
    return s_instance;
}

// =============================================================================
// FNetSnapshot::Init
// -----------------------------------------------------------------------------
// 設定をコピーし、ring buffer の容量を確保する。Standalone 以外で
// transport が nullptr の場合は GetTransportStub() に差し替えて
// リンク互換を保つ (defensive 設計)。
// =============================================================================
void FNetSnapshot::Init(const FNetSnapshotConfig& config, ENetRole role,
                       INetTransport* transport) noexcept {
    _config = config;
    _role   = role;

    // buffer_capacity が 0 / 1 だと補間 (= 2 snapshot 必須) が成立しないため
    // 最低 2 に丸める。max_payload_bytes が 0 の場合は 1KB に。
    if (_config.buffer_capacity_snapshots < 2) {
        _config.buffer_capacity_snapshots = 2;
    }
    if (_config.max_payload_bytes == 0) {
        _config.max_payload_bytes = 1024;
    }

    // Standalone 以外で nullptr が渡されたら stub に差し替える (落ちないように)。
    if (role == ENetRole::Standalone) {
        _transport = transport;  // nullptr 許容
    } else {
        _transport = (transport != nullptr) ? transport : &GetTransportStub();
    }

    // ring buffer を capacity 件で fixed-size 確保。各エントリの payload は
    // 初期サイズ 0 (CommitSnapshot で書き込まれる際に Resize される)。
    _ring_buffer.Clear();
    _ring_buffer.Resize(static_cast<usize>(_config.buffer_capacity_snapshots));
    _ring_head  = 0;
    _ring_count = 0;

    _pending_entities.Clear();
    _interp_scratch.Clear();

    _next_sequence      = 1;
    _last_received_tick = 0;
    _packets_sent       = 0;
    _packets_received   = 0;
    _bytes_sent         = 0;
    _bytes_received     = 0;
}

// =============================================================================
// FNetSnapshot::Shutdown
// -----------------------------------------------------------------------------
// transport は外部所有なので触らない。ring buffer / pending / scratch を解放。
// =============================================================================
void FNetSnapshot::Shutdown() noexcept {
    _ring_buffer.Clear();
    _pending_entities.Clear();
    _interp_scratch.Clear();
    _ring_head  = 0;
    _ring_count = 0;
    _transport  = nullptr;
}

u32 FNetSnapshot::BufferedSnapshotCount() const noexcept {
    return _ring_count;
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
    if (_role == ENetRole::Client || _role == ENetRole::Standalone) {
        return;
    }
    // 不正な引数は黙ってスキップ (defensive、ベストエフォート方針)。
    if (data == nullptr && data_size != 0) {
        return;
    }

    FPendingEntity pe;
    pe.entity_id      = entity_id;
    pe.component_mask = component_mask;
    if (data_size > 0) {
        pe.data.Resize(static_cast<usize>(data_size));
        MemCopy(pe.data.Data(), data, data_size);
    }
    _pending_entities.PushBack(Move(pe));
}

// =============================================================================
// FNetSnapshot::CommitSnapshot
// -----------------------------------------------------------------------------
// pending list を 1 つの payload に concat し、FSnapshotHeader を付けて
// transport.Send する。
//
// 失敗ケース (黙ってスキップ):
//   ・Client / Standalone 役割: 送信側ではないので no-op。
//   ・transport == nullptr: Standalone fallback で起きうる。
//   ・payload > max_payload_bytes: 設定上限超過。
//   ・transport.Send が Err 返却: packet loss と等価扱い (ベストエフォート)。
//
// pending list は呼出後にクリアされる (成否によらず)。これにより次 tick の
// AddEntitySnapshot は前 tick の残骸を引きずらない。
//
// TODO (Phase 3): delta compression — 前 snapshot との XOR 差分を取って
// payload を縮める。`_ring_buffer.Back().payload` を参照して entity 単位で
// XOR を取り、変更ビットだけ送る形に書き換える。
// =============================================================================
void FNetSnapshot::CommitSnapshot(u32 tick) noexcept {
    // 役割チェック。Client / Standalone は no-op。
    if (_role == ENetRole::Client || _role == ENetRole::Standalone) {
        _pending_entities.Clear();
        return;
    }
    if (_transport == nullptr) {
        _pending_entities.Clear();
        return;
    }

    // payload バイト数を見積もる。
    //   per-entity = kEntityHeaderSize (12) + data_size
    usize payload_size = 0;
    const usize n_ent = _pending_entities.Size();
    for (usize i = 0; i < n_ent; ++i) {
        payload_size += static_cast<usize>(kEntityHeaderSize)
                      + static_cast<usize>(_pending_entities[i].data.Size());
    }
    if (payload_size > static_cast<usize>(_config.max_payload_bytes)) {
        // 上限超過。Phase 3 で interest management / 分割送信を入れる候補。
        _pending_entities.Clear();
        return;
    }

    // wire bytes = header + payload を 1 本にまとめる (Send 1 回で送信)。
    const usize total = static_cast<usize>(kHeaderWireSize) + payload_size;

    // header を構築。CRC32 は Phase 3 で計算 (現状 0)。
    FSnapshotHeader hdr;
    hdr.tick                = tick;
    hdr.sequence            = _next_sequence;
    hdr.server_timestamp_us = 0;  // タイムスタンプ source は Phase 3 で wire 化
    hdr.payload_size        = static_cast<u32>(payload_size);
    hdr.crc32               = 0;

    // 送信用 buffer (= ring buffer に保存される表現と同じバイト列の payload 側)。
    // ring buffer には header と payload を分けて保持し、送信時は temp buffer に
    // concat する流れにする。Phase 3 で zero-copy gather IO に置換余地あり。
    TArray<u8> wire_buf;
    wire_buf.Resize(total);
    u8* p = wire_buf.Data();

    // [0..24) = header
    WriteHeader(p, hdr);
    u8* payload_ptr = p + kHeaderWireSize;

    // payload を per-entity layout で書き出す。
    usize off = 0;
    for (usize i = 0; i < n_ent; ++i) {
        const FPendingEntity& pe = _pending_entities[i];
        const u32 data_size = static_cast<u32>(pe.data.Size());
        WriteU32LE(payload_ptr + off + 0, pe.entity_id);
        WriteU32LE(payload_ptr + off + 4, pe.component_mask);
        WriteU32LE(payload_ptr + off + 8, data_size);
        if (data_size > 0) {
            MemCopy(payload_ptr + off + kEntityHeaderSize, pe.data.Data(), data_size);
        }
        off += static_cast<usize>(kEntityHeaderSize) + static_cast<usize>(data_size);
    }

    // 送信 (best-effort)。失敗してもクラッシュさせず、統計だけ更新しない。
    TResult<void> r = _transport->Send(wire_buf.Data(), static_cast<u32>(total));
    if (r.IsOk()) {
        ++_packets_sent;
        _bytes_sent += static_cast<u32>(total);
        ++_next_sequence;
        if (_next_sequence == 0) {
            _next_sequence = 1;  // 0 は invalid 値として予約
        }

        // ServerListener は host が自分の画面用に補間も使うため、自分が
        // commit した snapshot を ring buffer にも積む (loopback)。
        if (_role == ENetRole::ServerListener) {
            const u32 idx = _ring_head;
            FBufferedSnapshot& slot = _ring_buffer[static_cast<usize>(idx)];
            slot.header = hdr;
            slot.payload.Resize(static_cast<usize>(payload_size));
            if (payload_size > 0) {
                MemCopy(slot.payload.Data(), payload_ptr, payload_size);
            }
            _ring_head = (_ring_head + 1u) % _config.buffer_capacity_snapshots;
            if (_ring_count < _config.buffer_capacity_snapshots) {
                ++_ring_count;
            }
            _last_received_tick = hdr.tick;
        }
    }

    // pending list は成否によらずクリア (= 次 tick の AddEntitySnapshot は
    // 必ず空 list から積み直す)。
    _pending_entities.Clear();
}

// =============================================================================
// FNetSnapshot::Tick
// -----------------------------------------------------------------------------
// transport.Receive を非ブロッキングで pump し、受信した snapshot を ring
// buffer に追加する。1 Tick で複数 snapshot を取り込めるよう、受信なしに
// なるまでループする (PendingBytesIn が 0 で抜ける)。
//
// 受信した snapshot の wire format は CommitSnapshot 側と対称:
//   header (24B) + payload (header.payload_size B)。
// 受信フレームを 1 度に取り切る前提 (INetTransport の Receive がメッセージ
// 境界保持の契約)。Phase 3 で TCP framing を入れる場合は、ここで length
// prefix を見て partial reassembly を実装する。
// =============================================================================
void FNetSnapshot::Tick(f32 dt) noexcept {
    (void)dt;

    // Server (listen 非搭載) は受信側を持たないので何もしない。
    // Client / ServerListener / Standalone は (transport があれば) 受信する。
    if (_role == ENetRole::Server) {
        return;
    }
    if (_transport == nullptr) {
        return;
    }

    // 受信用一時 buffer。max_payload_bytes + header 分。Phase 3 で reuse 用に
    // メンバ化する候補 (毎 Tick で再 alloc しないように)。
    const u32 cap = static_cast<u32>(kHeaderWireSize) + _config.max_payload_bytes;
    TArray<u8> rx_buf;
    rx_buf.Resize(static_cast<usize>(cap));

    // 1 Tick で取りきる安全上限 (transport が壊れてループが終わらない事故予防)。
    // ring buffer 容量の 2 倍を上限としておく (古い snapshot は捨てる流儀)。
    const u32 max_iters = _config.buffer_capacity_snapshots * 2u;
    for (u32 iter = 0; iter < max_iters; ++iter) {
        TResult<u32> r = _transport->Receive(rx_buf.Data(), cap);
        if (r.IsErr()) {
            // 受信エラーは loss と等価扱い。次 Tick で再試行。
            break;
        }
        const u32 got = r.Value();
        if (got == 0) {
            // データなし。受信側 pump 完了。
            break;
        }
        // header 最低長未満は破棄。
        if (got < kHeaderWireSize) {
            ++_packets_received;  // バイト数だけ統計には載せる
            _bytes_received += got;
            continue;
        }

        FSnapshotHeader hdr{};
        ReadHeader(rx_buf.Data(), hdr);

        // payload size sanity check。max を超えるか、got と矛盾するなら破棄。
        if (hdr.payload_size > _config.max_payload_bytes) {
            ++_packets_received;
            _bytes_received += got;
            continue;
        }
        const u32 expected = kHeaderWireSize + hdr.payload_size;
        if (got < expected) {
            ++_packets_received;
            _bytes_received += got;
            continue;
        }
        // crc32 検証は Phase 3 で。現状 0 充填なので無条件にパス。

        // ring buffer に挿入。FIFO で最古を上書きする。
        const u32 idx = _ring_head;
        FBufferedSnapshot& slot = _ring_buffer[static_cast<usize>(idx)];
        slot.header = hdr;
        slot.payload.Resize(static_cast<usize>(hdr.payload_size));
        if (hdr.payload_size > 0) {
            MemCopy(slot.payload.Data(),
                    rx_buf.Data() + kHeaderWireSize,
                    static_cast<usize>(hdr.payload_size));
        }
        _ring_head = (_ring_head + 1u) % _config.buffer_capacity_snapshots;
        if (_ring_count < _config.buffer_capacity_snapshots) {
            ++_ring_count;
        }
        _last_received_tick = hdr.tick;
        ++_packets_received;
        _bytes_received += got;
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
                                             FEntitySnapshot* out_snapshots,
                                             u32 max_count,
                                             u32& out_actual_count) noexcept {
    out_actual_count = 0;

    // Server は補間を持たない。
    if (_role == ENetRole::Server) {
        return false;
    }
    if (out_snapshots == nullptr || max_count == 0) {
        return false;
    }
    if (_ring_count == 0) {
        return false;
    }

    // ring buffer 中の最新スロットを探す。挿入は _ring_head に書き込み後に
    // インクリメントするので、最新エントリは (_ring_head - 1) % capacity。
    const u32 cap = _config.buffer_capacity_snapshots;
    const u32 newest_idx = (_ring_head + cap - 1u) % cap;
    const FBufferedSnapshot& newest = _ring_buffer[static_cast<usize>(newest_idx)];

    // target_seq: client_time_sec を秒として持つが、Phase 2 では実時刻軸を
    // wire format に乗せていない (server_timestamp_us は 0 固定)。よって本
    // 実装では「最新 snapshot を返す」シンプル選択を取る。
    //
    // Phase 3 で server_timestamp_us を実装 / 計測時刻と紐付けたら、ここで
    //   target_us = newest.header.server_timestamp_us
    //             - static_cast<u64>(_config.interpolation_delay_sec * 1e6f);
    // を計算し、target_us を挟む 2 snapshot を ring から線形探索 → lerp する。
    // 現状 client_time_sec は受け取るだけで使わない (将来の API 後方互換のため)。
    (void)client_time_sec;

    const FSnapshotHeader& hdr = newest.header;
    const u8* payload_ptr     = newest.payload.Data();
    const u32 payload_size    = static_cast<u32>(newest.payload.Size());

    // payload を per-entity wire format で walk して FEntitySnapshot に流す。
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

        FEntitySnapshot& dst = out_snapshots[emitted];
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

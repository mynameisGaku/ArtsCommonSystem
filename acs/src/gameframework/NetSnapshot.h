// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar M Phase 2 — FNetSnapshot
//   server-authoritative snapshot ベースのネットコード seam
//
// 役割:
//   1) **Server 側 snapshot 書出**: 毎 tick のゲーム状態 (entity ごとの位置 /
//      速度 / 姿勢 / health 等) を 1 つの SnapshotHeader + payload に固めて
//      INetTransport.Send で配信する。1 snapshot = 「ある tick の世界全体の
//      authoritative state」を意味する。
//   2) **Client 側 snapshot 補間**: server から到着した複数 snapshot を ring
//      buffer に貯め、client 時刻に対して「interpolation_delay_sec 前」の
//      snapshot を 2 つ (基準 / 次) 取り出して補間する (= snapshot interpolation,
//      Source / Halo / Valorant 等で採用されている定石)。
//   3) **transport の差し替え**: 実 socket / Steam Datagram Relay / loopback
//      テスト fake のいずれを使うかは INetTransport seam で抽象化し、本クラスは
//      send/recv バイト列だけを扱う。Pillar M Phase 1 (FLockstep = deterministic
//      input replay) と並ぶもう一つの netcode 流儀を提供する。
//
// FLockstep との対比:
//   ・**FLockstep**: 全プレイヤーが同じ入力を受信し、同じ deterministic
//     simulation を回す (=「結果」ではなく「入力」を配信する)。
//     格闘ゲーム / RTS / GGPO 系で標準。
//   ・**FNetSnapshot**: server が 1 体の authoritative simulation を回し、
//     client は受け取った snapshot を補間表示するだけ (=「結果」だけを配信)。
//     FPS / TPS / MMORPG 系で標準。
//   どちらを採用するかはジャンル次第。両方を seam として提供し、タイトル
//   側が用途に応じて選べるようにする。
//
// 使い方 (server 側):
//   NetSnapshotConfig cfg{ /*snapshot_rate_hz=*/30, /*buffer_capacity=*/64,
//                          /*interpolation_delay_sec=*/0.1f,
//                          /*max_payload_bytes=*/8192 };
//   FNetSnapshot snap;
//   snap.Init(cfg, ENetRole::Server, &transport);
//
//   // 毎 simulation tick:
//   for (auto& e : world.Entities()) {
//       snap.AddEntitySnapshot(e.id, e.mask, &e.data, sizeof(e.data));
//   }
//   snap.CommitSnapshot(world.CurrentTick());  // ← Send まで実行
//
// 使い方 (client 側):
//   FNetSnapshot snap;
//   snap.Init(cfg, ENetRole::Client, &transport);
//
//   // 毎フレーム:
//   snap.Tick(dt);  // ← Receive + ring buffer 維持
//   EntitySnapshot view_buf[256];
//   u32 view_n = 0;
//   if (snap.TryGetInterpolatedSnapshot(client_time, view_buf, 256, view_n)) {
//       // view_buf[0..view_n] を描画用 state に流し込む (補間後の世界状態)
//   }
//
// 設計選択 (Phase M-2):
//   ・**INetTransport seam**: 実 socket は持たない。Connect/Send/Receive/
//     PendingBytesIn/PendingBytesOut の最小 API だけを切り、TCP / UDP / Steam
//     Datagram / loopback テスト fake は派生クラスで差し込む。Pillar V
//     IBackendClient の seam パターンを踏襲。
//   ・**ENetRole 4 種**: Standalone (シングル) / Client (受信のみ) / Server
//     (送信のみ) / ServerListener (listen-server 兼任) の 4 状態。listen-server
//     は内部的に Server + Client を同居させる役割で、host プレイヤーが自機の
//     simulation を server として回しつつ自分の画面用に snapshot 補間も使う
//     ケースに使う (HelloMultiplayer サンプル等)。
//   ・**SnapshotHeader fixed 24B**: tick(4) + sequence(4) + timestamp(8) +
//     payload_size(4) + crc32(4) = 24 byte。LE 固定。CRC32 は magic 直後 〜
//     payload 末尾 (= version + SnapshotHeader 本体 + payload) に対する Zlib /
//     PNG 規約 (poly 0xEDB88320, init/xorout 0xFFFFFFFF) で、wire format の
//     改竄 / 破損検知に使う。EncodeSnapshot で計算し DecodeSnapshot で検証する。
//     なお SnapshotHeader::crc32 フィールドは header を CRC 対象に含めるため
//     計算時に 0 として扱い (= header の他フィールドのみが寄与)、実際の値は
//     frame 末尾 4 byte の footer に格納される。
//   ・**EntitySnapshot は非所有 view**: `const void*` + size の pair。Server 側
//     は AddEntitySnapshot で m_PendingEntities にコピーを積み、CommitSnapshot
//     で payload に bulk concat する。Client 側は TryGetInterpolatedSnapshot
//     で ring buffer の中の payload を指す view として返す (寿命は次の Tick
//     呼び出しまで)。
//   ・**delta compression は範囲外**: 前 snapshot との XOR 差分のみを送る最適化は
//     本クラスでは扱わず、full snapshot を毎回 encode/送信する。各 frame は
//     EncodeSnapshot / DecodeSnapshot で magic+version+payload+crc32 を伴う
//     real な byte serialization として完結している (round-trip 検証済み)。
//   ・**ring buffer = TArray<{header, payload}> 固定容量**: snapshot は時系列順
//     に追加され、capacity を超えたら最古を上書きする FIFO。線形検索でも
//     buffer_capacity (= 数十) なので O(N) で十分。Phase 3 で per-sequence
//     index を持つかは検討。
//   ・**全 noexcept / STL 不使用 / TResult<T, FErrorCode>**: ACS 全体方針。
//   ・**コピー / ムーブ禁止**: 1 セッション 1 オブジェクトの長寿命 (transport
//     との結合関係を分裂させないため FLockstep / FBackendClient と同じ方針)。
//
// 範囲外 (Phase 3+):
//   ・delta compression (前 snapshot との差分のみ送る XOR + RLE)
//   ・client-side prediction (input を server に投げる + 自分の predicted
//     state を server snapshot で reconcile)
//   ・lag compensation (server が過去 tick で hit detection をやり直す)
//   ・snapshot encryption (FAssetPack 同居の FAcpakCrypto / AES-256-GCM 流用)
//   ・partial / interest-management (client ごとに見える entity を絞り込む)
//   ・reliable / unreliable channel 分離 (現在は INetTransport が抽象化)
#pragma once

#include "container/Array.h"
#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

// =============================================================================
// ENetRole — FNetSnapshot の動作役割
// -----------------------------------------------------------------------------
// Client : 受信のみ。AddEntitySnapshot / CommitSnapshot は no-op。
// Server : 送信のみ。TryGetInterpolatedSnapshot は false を返す。
// ServerListener : Server + Client 同居 (listen-server)。host が自機画面用に
//   snapshot 補間も使うため、両方の API が有効。
// Standalone : ネット通信なし。Init は transport=nullptr を許容し、全 API は
//   no-op (シングルプレイ用のリンク互換 fallback)。
// =============================================================================
enum class ENetRole : u8 {
    Client         = 0,
    Server         = 1,
    ServerListener = 2,
    Standalone     = 3,
};

// =============================================================================
// SnapshotHeader — wire format の 1 snapshot 内 header 24 byte
// -----------------------------------------------------------------------------
// LE 固定。frame 全体は [magic 'ACSN'(4)][version(4)][SnapshotHeader(24)]
// [payload(payload_size)][crc32(4)] のレイアウトで、client は受信時に magic /
// version / payload_size 上限 / crc32 を検証する (EncodeSnapshot で書き、
// DecodeSnapshot で検証する)。
// =============================================================================
struct SnapshotHeader {
    u32 tick               = 0;  // server tick (1 simulation step = 1 tick)
    u32 sequence           = 0;  // 送信側でモノトニック増加 (loss / 重複検知用)
    u64 server_timestamp_us = 0; // server 計測時の Unix microseconds
    u32 payload_size       = 0;  // 後続 payload のバイト数
    u32 crc32              = 0;  // frame footer に格納される CRC32 (DecodeSnapshot が復元)
};

// =============================================================================
// EntitySnapshot — 1 entity 分の component 状態 view
// -----------------------------------------------------------------------------
// 非所有 view。component_data は呼出側 (server: world / client: snapshot
// payload 内) のメモリを指す。寿命は server 側で「CommitSnapshot まで」、
// client 側で「次の Tick() 呼出まで」を保証する。
// =============================================================================
struct EntitySnapshot {
    u32         entity_id           = 0;       // ゲーム内 entity ID (0 = invalid)
    u32         component_mask      = 0;       // bitmask: どの component が含まれるか
    const void* component_data      = nullptr; // 非所有 view
    u32         component_data_size = 0;       // バイト数
};

// =============================================================================
// NetSnapshotConfig — ランタイム設定
// -----------------------------------------------------------------------------
// snapshot_rate_hz は server 側の Commit 頻度の目安 (本クラスは自動で
// throttle はしない。Caller が dt に応じて Commit を呼ぶ責任を持つ)。
// buffer_capacity_snapshots は ring buffer サイズ。30Hz × 0.5s = 15 が
// 補間 + 多少の loss 吸収には十分。
// =============================================================================
struct NetSnapshotConfig {
    u32 snapshot_rate_hz          = 30;    // server 側の目標 commit Hz (注: 自動 throttle はしない)
    u32 buffer_capacity_snapshots = 64;    // client 側 ring buffer 件数 (>= 2 が必要)
    f32 interpolation_delay_sec   = 0.1f;  // client 補間の遅延 (= jitter buffer の深さ)
    u32 max_payload_bytes         = 8192;  // 1 snapshot あたりの payload 上限
};

// =============================================================================
// INetTransport — 抽象 transport (TCP / UDP / Steam Datagram Relay / fake)
// -----------------------------------------------------------------------------
// 実 socket は本ヘッダに含めない。Pillar V IBackendClient と同じ seam
// パターン: ACS 本体は interface のみ宣言し、具象は別モジュール (or テスト
// 用 fake) で差し替える。
//
// 約束:
//   ・Send/Receive は **非ブロッキング**。Send は buffer 不足なら kSub_BufferFull
//     を返してよい。Receive は受信データなしなら out_size = 0 で成功 TResult。
//   ・Send は **メッセージ境界を保持する** 想定 (datagram / framed stream)。
//     1 回の Receive が 1 snapshot の SnapshotHeader + payload に対応する。
//     TCP のような stream-based transport を採用する場合は派生実装側で
//     length-prefixed framing を実装すること。
// =============================================================================
class INetTransport {
public:
    INetTransport() noexcept = default;
    virtual ~INetTransport() noexcept = default;

    INetTransport(const INetTransport&)            = delete;
    INetTransport& operator=(const INetTransport&) = delete;
    INetTransport(INetTransport&&)                 = delete;
    INetTransport& operator=(INetTransport&&)      = delete;

    // 接続。address は IP/host の static / 長寿命文字列 (例 "127.0.0.1")。
    virtual TResult<void> Connect(const char* address, u16 port) noexcept = 0;

    // 切断。多重呼出 / 未接続呼出は no-op で許容 (べき等)。
    virtual void Disconnect() noexcept = 0;

    // 接続中か。Connect 成功 ～ Disconnect / 通信エラー の間は true。
    virtual bool IsConnected() const noexcept = 0;

    // データ送信。1 呼出で 1 message を atomic に送る (境界保持)。
    virtual TResult<void> Send(const void* data, u32 size) noexcept = 0;

    // 非ブロッキング受信。受信なしなら 0 を返す成功 TResult。
    // out_buffer は capacity バイト確保済みで呼ぶこと。1 呼出で最大 1 message。
    virtual TResult<u32> Receive(void* out_buffer, u32 capacity) noexcept = 0;

    // 受信側にまだ取り出していないバイト数の目安 (実装依存。0 でも可)。
    virtual u32 PendingBytesIn() const noexcept = 0;

    // 送信側にまだ flush されていないバイト数の目安 (実装依存。0 でも可)。
    virtual u32 PendingBytesOut() const noexcept = 0;
};

// =============================================================================
// 共通エラー subcode (FNetSnapshot + NetTransportStub 共通)
// =============================================================================
struct NetSnapshotError {
    enum SubCode : u16 {
        kSub_NotConnected   = 1,  // Send 前に Connect されていない
        kSub_BadArgument    = 2,  // size == 0 / address == nullptr 等
        kSub_BufferFull     = 3,  // Send: 送信 buffer 満杯 / Receive: out_buffer 不足
        kSub_PayloadTooBig  = 4,  // CommitSnapshot: payload が max_payload_bytes 超
        kSub_NullBuffer     = 5,  // Encode/DecodeSnapshot: buffer == nullptr
        kSub_BufferTooSmall = 6,  // EncodeSnapshot: out_buffer が frame 長未満
        kSub_BadSize        = 7,  // DecodeSnapshot: frame 長がフィールドと矛盾
        kSub_BadMagic       = 8,  // DecodeSnapshot: magic 不一致 ('ACSN' でない)
        kSub_BadVersion     = 9,  // DecodeSnapshot: version 不一致
        kSub_BadCrc         = 10, // DecodeSnapshot: CRC32 mismatch (破損 / 改竄)
        kSub_NotImplemented = 99, // stub: 未実装
    };
};

// =============================================================================
// NetTransportStub — INetTransport の null-object 実装
// -----------------------------------------------------------------------------
// 「常に NotImplemented を返す」defensive stub。サンプル / テスト / linker
// 互換用。本番ビルドに混入したケースを QA で必ず検出できるよう、Connect /
// Send / Receive は必ず Err を返す (Pillar V BackendClientStub と同じ pattern)。
// =============================================================================
class NetTransportStub final : public INetTransport {
public:
    NetTransportStub() noexcept = default;
    ~NetTransportStub() noexcept override = default;

    TResult<void> Connect(const char* address, u16 port) noexcept override;
    void Disconnect() noexcept override;
    bool IsConnected() const noexcept override { return false; }
    TResult<void> Send(const void* data, u32 size) noexcept override;
    TResult<u32> Receive(void* out_buffer, u32 capacity) noexcept override;
    u32 PendingBytesIn() const noexcept override { return 0; }
    u32 PendingBytesOut() const noexcept override { return 0; }
};

// プロセス共有の stub INetTransport。常に NotImplemented を返す。
INetTransport& GetTransportStub() noexcept;

// =============================================================================
// FNetSnapshot — server-side 書出 / client-side 補間
// -----------------------------------------------------------------------------
// 1 セッション 1 オブジェクト。コピー / ムーブ禁止。
// =============================================================================
class FNetSnapshot {
public:
    FNetSnapshot() noexcept = default;
    ~FNetSnapshot() noexcept = default;

    FNetSnapshot(const FNetSnapshot&)            = delete;
    FNetSnapshot& operator=(const FNetSnapshot&) = delete;
    FNetSnapshot(FNetSnapshot&&)                 = delete;
    FNetSnapshot& operator=(FNetSnapshot&&)      = delete;

    // ----- 初期化 / 終了 -----
    // role = Standalone の場合、transport は nullptr を許容する。
    // Client / Server / ServerListener では transport は non-null 必須
    // (nullptr の場合は内部的に GetTransportStub() に差し替えてリンク互換を保つ)。
    void Init(const NetSnapshotConfig& config, ENetRole role,
              INetTransport* transport) noexcept;

    // ring buffer / pending entities / 統計値をリセット。transport は触らない。
    void Shutdown() noexcept;

    // ----- 状態 query -----
    ENetRole Role()                       const noexcept { return m_Role; }
    u32      BufferedSnapshotCount()      const noexcept;
    f32      CurrentInterpolationDelay()  const noexcept { return m_Config.interpolation_delay_sec; }
    u32      LastReceivedTick()           const noexcept { return m_LastReceivedTick; }

    // ----- server 側 API -----
    // 1 entity の現在 state を pending list に積む。data は内部にコピーされる
    // (CommitSnapshot まで保持)。Client / Standalone では no-op。
    void AddEntitySnapshot(u32 entity_id, u32 component_mask,
                           const void* data, u32 data_size) noexcept;

    // pending list を 1 つの payload に concat し、SnapshotHeader を付けて
    // transport.Send する。pending list はクリアされる。
    // Client / Standalone では no-op。
    void CommitSnapshot(u32 tick) noexcept;

    // ----- client 側 API -----
    // client_time_sec に対して「interpolation_delay_sec 前」の snapshot を 2 つ
    // 取り出し、線形補間して out_snapshots に書き出す。
    //
    // 戻り値: 補間可能だったら true、buffer に snapshot が 2 つ未満 or 期待
    //   時刻が buffer 外なら false。
    // out_actual_count には書き出した entity 数を返す (max_count で clamp)。
    //
    // 注: out_snapshots の component_data は ring buffer 内 payload を指す
    // 非所有 view。次の Tick() 呼出まで有効。
    bool TryGetInterpolatedSnapshot(f32 client_time_sec,
                                    EntitySnapshot* out_snapshots,
                                    u32 max_count,
                                    u32& out_actual_count) noexcept;

    // ----- 毎フレーム呼出 (Client / ServerListener) -----
    // transport.Receive を pump し、受信した snapshot を ring buffer に積む。
    // dt は将来の jitter buffer / 補間時刻計算で使う想定 (Phase 2 は無視)。
    void Tick(f32 dt) noexcept;

    // ----- 統計 -----
    u32 PacketsSent()     const noexcept { return m_PacketsSent;     }
    u32 PacketsReceived() const noexcept { return m_PacketsReceived; }
    u32 BytesSent()       const noexcept { return m_BytesSent;       }
    u32 BytesReceived()   const noexcept { return m_BytesReceived;   }

    // ----- wire codec (transport 非依存・round-trip 可能) -----
    // EncodeSnapshot / DecodeSnapshot は 1 snapshot を byte buffer に対して
    // 直列化 / 復元する純粋関数 (static)。transport seam とは独立で、
    // CommitSnapshot は EncodeSnapshot の結果を Send し、Tick は Receive した
    // bytes を DecodeSnapshot で検証してから ring buffer に積む。テストや
    // loopback / record-replay でも再利用できる。
    //
    // frame wire layout (すべて little-endian):
    //   [0)   magic 'ACSN' (4)
    //   [4)   version       (4)
    //   [8)   SnapshotHeader (24): tick / sequence / timestamp_us / payload_size / crc32
    //   [32)  payload        (payload_size): entity record の concat
    //   [末尾] crc32 footer   (4): magic を除く [4 .. payload 末尾) に対する CRC32
    //
    // payload 内 1 entity = [entity_id(4)][component_mask(4)][data_size(4)][data]。

    // 1 frame に必要な総バイト数を返す (header.payload_size をそのまま信頼する)。
    // = magic(4) + version(4) + header(24) + payload_size + crc32(4)。
    static u32 EncodedSnapshotSize(u32 payload_size) noexcept;

    // header + payload を wire layout で out_buffer に書き出す。
    //   ・header.payload_size は payload_size 引数と一致している必要がある
    //     (不一致なら kSub_BadArgument)。crc32 フィールドは内部で計算・上書きする。
    //   ・out_capacity が必要量未満なら kSub_BufferTooSmall。
    //   ・成功時 out_written に書き込みバイト数 (= EncodedSnapshotSize) を返す。
    static TResult<void> EncodeSnapshot(const SnapshotHeader& header,
                                        const u8* payload, u32 payload_size,
                                        u8* out_buffer, u32 out_capacity,
                                        u32& out_written) noexcept;

    // wire bytes を解釈し、header を復元 + payload を out_payload に複製する。
    //   ・magic / version / size / CRC32 を検証し、不正なら対応する Err。
    //   ・out_header.crc32 には footer に格納されていた CRC を復元する。
    //   ・out_payload は置換セマンティクス (Clear → payload_size 件 Resize)。
    static TResult<void> DecodeSnapshot(const u8* buffer, u32 size,
                                        SnapshotHeader& out_header,
                                        TArray<u8>& out_payload) noexcept;

private:
    // 1 ring buffer エントリ = SnapshotHeader + payload バイト列。
    // payload は AddEntitySnapshot で積んだ全 entity の concat。
    struct BufferedSnapshot {
        SnapshotHeader header {};
        TArray<u8>      payload;
    };

    // server 側: AddEntitySnapshot で 1 件ずつコピーして積む。CommitSnapshot で
    // payload に concat する。
    struct PendingEntity {
        u32       entity_id      = 0;
        u32       component_mask = 0;
        TArray<u8> data;  // コピー保持
    };

    // payload 内の 1 entity の wire layout (CommitSnapshot で書き出す):
    //   offset  size  field
    //   ------  ----  ---------------------------------------
    //   0       4     entity_id
    //   4       4     component_mask
    //   8       4     data_size
    //   12      N     data (data_size バイト)
    static constexpr u32 kEntityHeaderSize = 12;

    // 補間結果を保持するための temporary 領域 (TryGetInterpolatedSnapshot が
    // 返す EntitySnapshot::component_data が指す先)。client 側のみで使う。
    TArray<u8> m_InterpScratch;

    NetSnapshotConfig          m_Config         {};
    ENetRole                   m_Role           = ENetRole::Standalone;
    INetTransport*             m_Transport      = nullptr;
    TArray<PendingEntity>       m_PendingEntities;
    TArray<BufferedSnapshot>    m_RingBuffer;   // capacity = config.buffer_capacity_snapshots
    u32                        m_RingHead      = 0;  // 次の挿入位置 (FIFO)
    u32                        m_RingCount     = 0;  // 現在の有効件数 (<= capacity)
    u32                        m_NextSequence  = 1;  // server 送信時の sequence 番号 (0 は無効)
    u32                        m_LastReceivedTick = 0;

    // 統計
    u32 m_PacketsSent     = 0;
    u32 m_PacketsReceived = 0;
    u32 m_BytesSent       = 0;
    u32 m_BytesReceived   = 0;
};

} // namespace acs::game

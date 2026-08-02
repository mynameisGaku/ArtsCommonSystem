// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar M — CNetSnapshot
//   server-authoritative snapshot ベースのネットコード seam
//
// 役割:
//   1) **Server 側 snapshot 書出**: 毎 tick のゲーム状態 (entity ごとの位置 /
//      速度 / 姿勢 / health 等) を 1 つの FSnapshotHeader + payload に固めて
//      INetTransport.Send で配信する。1 snapshot = 「ある tick の世界全体の
//      authoritative state」を意味する。
//   2) **Client 側 snapshot 補間**: server から到着した複数 snapshot を ring
//      buffer に貯め、client 時刻に対して「interpolation_delay_sec 前」の
//      snapshot を 2 つ (基準 / 次) 取り出して補間する (= snapshot interpolation,
//      Source / Halo / Valorant 等で採用されている定石)。
//   3) **transport の差し替え**: 実 socket / Steam Datagram Relay / loopback
//      テスト fake のいずれを使うかは INetTransport seam で抽象化し、本クラスは
//      send/recv バイト列だけを扱う。CLockstep (deterministic input replay) と
//      並ぶもう一つの netcode 流儀を提供する。
//
// CLockstep との対比:
//   ・**CLockstep**: 全プレイヤーが同じ入力を受信し、同じ deterministic
//     simulation を回す (=「結果」ではなく「入力」を配信する)。
//     格闘ゲーム / RTS / GGPO 系で標準。
//   ・**CNetSnapshot**: server が 1 体の authoritative simulation を回し、
//     client は受け取った snapshot を補間表示するだけ (=「結果」だけを配信)。
//     FPS / TPS / MMORPG 系で標準。
//   どちらを採用するかはジャンル次第。両方を seam として提供し、タイトル
//   側が用途に応じて選べるようにする。
//
// 使い方 (server 側):
//   FNetSnapshotConfig cfg{ /*snapshot_rate_hz=*/30, /*buffer_capacity=*/64,
//                          /*interpolation_delay_sec=*/0.1f,
//                          /*max_payload_bytes=*/8192 };
//   CNetSnapshot snap;
//   snap.Init(cfg, ENetRole::Server, &transport);
//
//   // 毎 simulation tick:
//   for (auto& e : world.Entities()) {
//       snap.AddEntitySnapshot(e.id, e.mask, &e.data, sizeof(e.data));
//   }
//   snap.CommitSnapshot(world.CurrentTick());  // ← Send まで実行
//
// 使い方 (client 側):
//   CNetSnapshot snap;
//   snap.Init(cfg, ENetRole::Client, &transport);
//
//   // 毎フレーム:
//   snap.Tick(dt);  // ← Receive + ring buffer 維持
//   FEntitySnapshot view_buf[256];
//   u32 view_n = 0;
//   if (snap.TryGetInterpolatedSnapshot(client_time, view_buf, 256, view_n)) {
//       // view_buf[0..view_n] を描画用 state に流し込む (補間後の世界状態)
//   }
//
// 設計選択:
//   ・**INetTransport seam**: 実 socket は持たない。Connect/Send/Receive/
//     PendingBytesIn/PendingBytesOut の最小 API だけを切り、TCP / UDP / Steam
//     Datagram / loopback テスト fake は派生クラスで差し込む。IBackendClient
//     の seam パターンを踏襲。
//   ・**ENetRole 4 種**: Standalone (シングル) / Client (受信のみ) / Server
//     (送信のみ) / ServerListener (listen-server 兼任) の 4 状態。listen-server
//     は内部的に Server + Client を同居させる役割で、host プレイヤーが自機の
//     simulation を server として回しつつ自分の画面用に snapshot 補間も使う
//     ケースに使う (HelloMultiplayer サンプル等)。
//   ・**FSnapshotHeader fixed 24B**: tick(4) + sequence(4) + timestamp(8) +
//     payload_size(4) + crc32(4) = 24 byte。LE 固定。CRC32 は magic 直後 〜
//     payload 末尾 (= version + FSnapshotHeader 本体 + payload) に対する Zlib /
//     PNG 規約 (poly 0xEDB88320, init/xorout 0xFFFFFFFF) で、wire format の
//     改竄 / 破損検知に使う。EncodeSnapshot で計算し DecodeSnapshot で検証する。
//     なお FSnapshotHeader::crc32 フィールドは header を CRC 対象に含めるため
//     計算時に 0 として扱い (= header の他フィールドのみが寄与)、実際の値は
//     frame 末尾 4 byte の footer に格納される。
//   ・**FEntitySnapshot は非所有 view**: `const void*` + size の pair。Server 側
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
//     buffer_capacity (= 数十) なので O(N) で十分。
//   ・**全 noexcept / STL 不使用 / TResult<T, FErrorCode>**: ACS 全体方針。
//   ・**コピー / ムーブ禁止**: 1 セッション 1 オブジェクトの長寿命 (transport
//     との結合関係を分裂させないため CLockstep / IBackendClient と同じ方針)。
#pragma once

#include "container/Array.h"
#include "foundation/Result.h"
#include "foundation/Types.h"
#include "threading/Mutex.h"

namespace acs::game {

/**
 * CNetSnapshot の動作役割。
 *
 * @details
 * role によって有効な API が変わる。送信側 (Server / ServerListener) は
 * AddEntitySnapshot / CommitSnapshot を、受信側 (Client / ServerListener) は
 * Tick / TryGetInterpolatedSnapshot を使う。Standalone は全 API no-op の
 * シングルプレイ用 fallback。
 */
enum class ENetRole : u8 {
    /** 受信のみ。AddEntitySnapshot / CommitSnapshot は no-op。 */
    Client = 0,

    /** 送信のみ。TryGetInterpolatedSnapshot は false を返す。 */
    Server = 1,

    /** Server + Client 同居 (listen-server)。host が自機画面用に補間も使うため両方有効。 */
    ServerListener = 2,

    /** ネット通信なし。transport=nullptr を許容し全 API は no-op (リンク互換 fallback)。 */
    Standalone = 3,
};

/**
 * wire format の 1 snapshot 内 header (24 byte、LE 固定)。
 *
 * @details
 * frame 全体は [magic 'ACSN'(4)][version(4)][FSnapshotHeader(24)]
 * [payload(payload_size)][crc32(4)] のレイアウト。EncodeSnapshot で書き、
 * DecodeSnapshot で magic / version / payload_size 上限 / crc32 を検証する。
 */
struct FSnapshotHeader {
    /** server tick (1 simulation step = 1 tick)。 */
    u32 tick = 0;

    /** 送信側でモノトニック増加する sequence 番号 (loss / 重複検知用)。 */
    u32 sequence = 0;

    /** server 計測時の Unix microseconds。 */
    u64 server_timestamp_us = 0;

    /** 後続 payload のバイト数。 */
    u32 payload_size = 0;

    /** frame footer に格納される CRC32 (DecodeSnapshot が復元)。 */
    u32 crc32 = 0;
};

/**
 * 1 entity 分の component 状態 view (非所有)。
 *
 * @details
 * component_data は呼出側 (server: world / client: snapshot payload 内) の
 * メモリを指す。寿命は server 側で「CommitSnapshot まで」、client 側で
 * 「次の Tick() 呼出まで」を保証する。
 */
struct FEntitySnapshot {
    /** ゲーム内 entity ID (0 = invalid)。 */
    u32 entity_id = 0;

    /** どの component が含まれるかの bitmask。 */
    u32 component_mask = 0;

    /** component データへの非所有 view。 */
    const void* component_data = nullptr;

    /** component データのバイト数。 */
    u32 component_data_size = 0;
};

/**
 * CNetSnapshot のランタイム設定。
 *
 * @details
 * snapshot_rate_hz は server 側の Commit 頻度の目安で、本クラスは自動 throttle
 * しない (Caller が dt に応じて Commit を呼ぶ責任を持つ)。buffer_capacity は
 * ring buffer サイズで、30Hz × 0.5s = 15 程度が補間 + loss 吸収に十分。
 */
struct FNetSnapshotConfig {
    /** server 側の目標 commit Hz (注: 自動 throttle はしない)。 */
    u32 snapshot_rate_hz = 30;

    /** client 側 ring buffer 件数 (>= 2 が必要)。 */
    u32 buffer_capacity_snapshots = 64;

    /** client 補間の遅延秒 (= jitter buffer の深さ)。 */
    f32 interpolation_delay_sec = 0.1f;

    /** 1 snapshot あたりの payload 上限バイト数。 */
    u32 max_payload_bytes = 8192;
};

/** 1 snapshot の payload に許可する製品上限 (4 MiB)。 */
inline constexpr u32 kNetSnapshotMaximumPayloadBytes = 4u * 1024u * 1024u;

/** ring buffer の snapshot 件数上限。 */
inline constexpr u32 kNetSnapshotMaximumRingCapacity = 4096u;

/** 1 snapshot に積める entity record 件数上限。 */
inline constexpr u32 kNetSnapshotMaximumPendingEntities = 65536u;

/** 1 Tick で transport から取り出す message 数上限。 */
inline constexpr u32 kNetSnapshotMaximumReceivesPerTick = 8192u;

/** IPv4 UDP payload の理論上限 (65535 - IPv4 header 20 - UDP header 8)。 */
inline constexpr u32 kNetSnapshotMaximumUdpDatagramBytes = 65507u;

/**
 * 抽象 transport (TCP / UDP / Steam Datagram Relay / fake)。
 *
 * @details
 * 実 socket は本ヘッダに含めず、ACS 本体は interface のみ宣言して具象は別モジュール
 * (or テスト用 fake) で差し替える (IBackendClient と同じ seam パターン)。Send/Receive
 * は非ブロッキングで、Send はメッセージ境界を保持する想定 (1 回の Receive が 1
 * snapshot に対応)。TCP 等の stream transport は派生実装側で length-prefixed framing
 * を実装する。
 */
class INetTransport {
public:
    /** 既定構築。 */
    INetTransport() noexcept = default;

    /** 派生クラスを正しく破棄するための仮想デストラクタ。 */
    virtual ~INetTransport() noexcept = default;

    /** コピー禁止。 */
    INetTransport(const INetTransport&) = delete;

    /** コピー代入も禁止。 */
    INetTransport& operator=(const INetTransport&) = delete;

    /** ムーブ禁止。 */
    INetTransport(INetTransport&&) = delete;

    /** ムーブ代入も禁止。 */
    INetTransport& operator=(INetTransport&&) = delete;

    /**
     * 接続する。
     *
     * @param address IP/host の static / 長寿命文字列 (例 "127.0.0.1")。
     * @param port 接続先ポート。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    virtual TResult<void> Connect(const char* address, u16 port) noexcept = 0;

    /**
     * 切断する (多重呼出 / 未接続呼出は no-op で許容するべき等動作)。
     */
    virtual void Disconnect() noexcept = 0;

    /**
     * 接続中かを返す。
     *
     * @return Connect 成功 ～ Disconnect / 通信エラー の間は true。
     */
    virtual bool IsConnected() const noexcept = 0;

    /**
     * データを送信する (1 呼出で 1 message を atomic に送る、境界保持)。
     *
     * @param data 送信するバイト列。
     * @param size 送信バイト数。
     * @return 成功なら空の TResult、buffer 不足なら kSub_BufferFull 等のエラー。
     */
    virtual TResult<void> Send(const void* data, u32 size) noexcept = 0;

    /**
     * 非ブロッキングで 1 message を受信する。
     *
     * @details out_buffer は capacity バイト確保済みで呼ぶこと。受信なしなら 0 で成功。
     * @param out_buffer 受信データを書き込む先。
     * @param capacity out_buffer の容量。
     * @return 受信バイト数 (受信なしなら 0) を持つ TResult、失敗ならエラー。
     */
    virtual TResult<u32> Receive(void* out_buffer, u32 capacity) noexcept = 0;

    /**
     * 受信側にまだ取り出していないバイト数の目安を返す。
     *
     * @return 受信待ちバイト数の目安 (実装依存。0 でも可)。
     */
    virtual u32 PendingBytesIn() const noexcept = 0;

    /**
     * 送信側にまだ flush されていないバイト数の目安を返す。
     *
     * @return 送信待ちバイト数の目安 (実装依存。0 でも可)。
     */
    virtual u32 PendingBytesOut() const noexcept = 0;
};

/**
 * CNetSnapshot + CNetTransportStub + CUdpTransport 共通のエラー subcode。
 *
 * @details
 * FErrorCode の subcode として使う。kSub_Wsa* 以降の CUdpTransport 固有コードでは
 * os_error に WSAGetLastError() の値をそのまま載せる。
 */
struct FNetSnapshotError {
    /**
     * エラー subcode の値。
     */
    enum ESubCode : u16 {
        /** Send 前に Connect されていない。 */
        kSub_NotConnected = 1,

        /** size == 0 / address == nullptr 等の不正引数。 */
        kSub_BadArgument = 2,

        /** Send: 送信 buffer 満杯 / Receive: out_buffer 不足。 */
        kSub_BufferFull = 3,

        /** CommitSnapshot: payload が max_payload_bytes 超。 */
        kSub_PayloadTooBig = 4,

        /** Encode/DecodeSnapshot: buffer == nullptr。 */
        kSub_NullBuffer = 5,

        /** EncodeSnapshot: out_buffer が frame 長未満。 */
        kSub_BufferTooSmall = 6,

        /** DecodeSnapshot: frame 長がフィールドと矛盾。 */
        kSub_BadSize = 7,

        /** DecodeSnapshot: magic 不一致 ('ACSN' でない)。 */
        kSub_BadMagic = 8,

        /** DecodeSnapshot: version 不一致。 */
        kSub_BadVersion = 9,

        /** DecodeSnapshot: CRC32 mismatch (破損 / 改竄)。 */
        kSub_BadCrc = 10,

        /** Encode/DecodeSnapshot: 製品上限を超える payload / frame。 */
        kSub_FrameTooLarge = 11,

        /** wire header の予約値が非0、または sequence が予約値0。 */
        kSub_NonCanonicalHeader = 12,

        /** checked API の一時領域または出力領域を確保できない。 */
        kSub_AllocationFailed = 13,

        /** Receive が capacity より大きい値を返す等、transport 契約違反。 */
        kSub_TransportContractViolation = 14,

        /** payload の entity record が途中で終わる、または非正規値を含む。 */
        kSub_BadEntityPayload = 15,

        /** ring / payload / role 等の設定値が許容範囲外。 */
        kSub_InvalidConfig = 16,

        /** role に対して許可されない送受信操作。 */
        kSub_WrongRole = 17,

        /** pending entity 件数または合計 payload が上限に達した。 */
        kSub_PendingLimit = 18,

        /** UDP datagram が受信 buffer に収まらず切り詰められた。 */
        kSub_DatagramTruncated = 19,

        /** CUdpTransport: WSAStartup 失敗。 */
        kSub_WsaStartup = 20,

        /** CUdpTransport: socket() 失敗。 */
        kSub_SocketCreate = 21,

        /** CUdpTransport: ioctlsocket(FIONBIO) 失敗。 */
        kSub_SetNonBlocking = 22,

        /** CUdpTransport: bind() 失敗 (local port 衝突等)。 */
        kSub_Bind = 23,

        /** CUdpTransport: address 文字列が IPv4 dotted-quad として不正。 */
        kSub_BadAddress = 24,

        /** CUdpTransport: sendto() 失敗 (WSAEWOULDBLOCK 以外)。 */
        kSub_SendFailed = 25,

        /** CUdpTransport: recvfrom() 失敗 (WSAEWOULDBLOCK 以外)。 */
        kSub_RecvFailed = 26,

        /** CUdpTransport: 既存 socket または WSA 参照の回収に失敗。 */
        kSub_CloseFailed = 27,

        /** stub: 未実装。 */
        kSub_NotImplemented = 99,
    };
};

/**
 * TryTick の受信結果。
 *
 * @details transport は複数 message を順に消費するため、途中エラーより前に受理した
 * snapshot は保持される。`stop_subcode == 0` は正常な「受信なし」で停止したことを表す。
 */
struct FNetSnapshotTickResult {
    /** transport から取り出した非空 message 数。 */
    u32 received_messages = 0;

    /** 検証に成功して ring buffer へ commit した snapshot 数。 */
    u32 accepted_snapshots = 0;

    /** frame / entity payload 検証で棄却した message 数。 */
    u32 rejected_messages = 0;

    /** 正常 message と棄却 messageを合わせた受信バイト数。 */
    u64 received_bytes = 0;

    /** 0 は正常停止。それ以外は FNetSnapshotError::ESubCode。 */
    u16 stop_subcode = 0;

    /** 最後に棄却した message の FNetSnapshotError::ESubCode。棄却なしは0。 */
    u16 last_rejected_subcode = 0;

    /** transport / allocation / contract error で停止しなかったかを返す。 */
    bool Succeeded() const noexcept
    {
        return stop_subcode == 0;
    }
};

/**
 * INetTransport の null-object 実装 (defensive stub)。
 *
 * @details
 * 「常に NotImplemented を返す」stub でサンプル / テスト / linker 互換用。本番
 * ビルドに混入したケースを QA で必ず検出できるよう、Connect / Send / Receive は
 * 必ず Err を返す (GetBackendStub() と同じ pattern)。
 */
class CNetTransportStub final : public INetTransport {
public:
    /** 既定構築。 */
    CNetTransportStub() noexcept = default;

    /** 破棄する。 */
    ~CNetTransportStub() noexcept override = default;

    /**
     * 常に kSub_NotImplemented を返す。
     *
     * @param address 無視される。
     * @param port 無視される。
     * @return 常に NotImplemented エラー。
     */
    TResult<void> Connect(const char* address, u16 port) noexcept override;

    /** never-connected なので no-op。 */
    void Disconnect() noexcept override;

    /**
     * 常に未接続を返す。
     *
     * @return 常に false。
     */
    bool IsConnected() const noexcept override
    {
        return false;
    }

    /**
     * 常に kSub_NotImplemented を返す。
     *
     * @param data 無視される。
     * @param size 無視される。
     * @return 常に NotImplemented エラー。
     */
    TResult<void> Send(const void* data, u32 size) noexcept override;

    /**
     * 常に kSub_NotImplemented を返す。
     *
     * @param out_buffer 無視される。
     * @param capacity 無視される。
     * @return 常に NotImplemented エラー。
     */
    TResult<u32> Receive(void* out_buffer, u32 capacity) noexcept override;

    /**
     * 常に 0 を返す。
     *
     * @return 常に 0。
     */
    u32 PendingBytesIn() const noexcept override
    {
        return 0;
    }

    /**
     * 常に 0 を返す。
     *
     * @return 常に 0。
     */
    u32 PendingBytesOut() const noexcept override
    {
        return 0;
    }
};

/**
 * プロセス共有の stub INetTransport を返す。
 *
 * @return 常に NotImplemented を返す CNetTransportStub への参照。
 */
INetTransport& GetTransportStub() noexcept;

/** CUdpTransport が共有する Winsock 資源の診断スナップショット。 */
struct FUdpTransportDiagnostics {
    /** 現在 CUdpTransport 群が所有する WSAStartup 参照数。 */
    u32 active_winsock_reference_count = 0;

    /** 再試行を待っている WSACleanup エラー。0 は保留なし。 */
    u32 pending_cleanup_error = 0;

    /** pending_cleanup_error の WSA 参照を生存中 instance ではなく共有回収処理が所有するか。 */
    bool cleanup_debt_orphaned = false;

    /** 破棄後も共有回収表が所有し、closesocket の再試行を待っている socket 数。 */
    u32 orphaned_socket_count = 0;

    /** orphaned_socket_count のうち仮想メモリ overflow 表で追跡している socket 数。 */
    u32 overflow_orphaned_socket_count = 0;

    /** プロセス寿命中に観測した資源解放または所有権検証の失敗イベント累積数。 */
    u64 resource_release_failure_count = 0;
};

/**
 * INetTransport の実 Winsock2 UDP 実装。
 *
 * @details
 * 実 socket を用いたコネクションレス UDP datagram transport で、1 Send = 1 sendto()
 * = 1 datagram、1 Receive = 1 recvfrom() = 1 datagram (INetTransport の境界保持契約を
 * そのまま満たす)。Connect は WSAStartup (プロセス内 ref-count) → socket 生成 →
 * 非ブロッキング化 → local port に bind → remote endpoint 保持を行う。Winsock 型を
 * header に漏らさないため socket は uptr、remote endpoint は raw octets + port で保持し
 * .cpp 内でのみ Winsock 型に復元する。1 セッション 1 オブジェクトで non-copy /
 * non-move (INetTransport 由来)。全公開操作はインスタンス単位の排他で直列化され、
 * Connect / Disconnect / Send / Receive を異なるスレッドから呼んでも所有状態を失わない。
 */
class CUdpTransport final : public INetTransport {
public:
    /** 既定構築 (socket は未接続)。 */
    CUdpTransport() noexcept = default;

    /** Disconnect を試み、未回収資源は共有回収処理へ移して再試行可能な状態で破棄する。 */
    ~CUdpTransport() noexcept override;

    /** 全 CUdpTransport が共有する Winsock 参照・解放失敗の現在値を返す。 */
    static FUdpTransportDiagnostics CaptureDiagnostics() noexcept;

    /**
     * 破棄済み transport から共有回収処理へ移された socket と WSA 参照を再回収する。
     *
     * @details 生存中 transport が所有する cleanup debt には触れない。回収完了時も
     * `deferred_cleanup_resolved` の機械可読ログを出し、以前の cleanup_pending が
 * 解決済みであることを明示する。CApplication 終了前の明示 drain にも使用できる。
     * @return 全共有 debt を回収できれば成功。残存時は kSub_CloseFailed。
     */
    static TResult<void> DrainDeferredResources() noexcept;

    /**
     * bind する local port を指定する。
     *
     * @details
     * 0 (既定) なら OS が ephemeral port を割当。受信側は固定 port に bind したいので
     * Connect 前に呼ぶ。Connect 後の変更は次回 Connect まで反映されない。
     * @param local_port bind する local port (0 で ephemeral)。
     */
    void SetLocalPort(u16 local_port) noexcept;

    /**
     * 設定済みの local port を返す。
     *
     * @return SetLocalPort で指定した値 (既定 0)。
     */
    u16 LocalPort() const noexcept;

    /**
     * UDP socket を用意し、送信先 endpoint を確定する。
     *
     * @details
     * WSAStartup (ref-count) → socket 生成 → 非ブロッキング化 → local port に bind →
     * remote endpoint 保持。多重 Connect は前の socket を閉じてから張り直す。
     * @param address 送信先 IPv4 dotted-quad (例 "127.0.0.1")。
     * @param port 送信先ポート。
     * @return 成功なら空の TResult、失敗なら os_error 付きエラー。
     */
    TResult<void> Connect(const char* address, u16 port) noexcept override;

    /** closesocket + WSACleanup (ref を戻す)。多重 / 未接続呼出は no-op。 */
    void Disconnect() noexcept override;

    /**
     * socket が open かを返す。
     *
     * @return open な socket を持つなら true。
     */
    bool IsConnected() const noexcept override;

    /**
     * remote endpoint へ 1 datagram を sendto() する。
     *
     * @details WSAEWOULDBLOCK は kSub_BufferFull、その他失敗は kSub_SendFailed。
     * @param data 送信するバイト列。
     * @param size 送信バイト数。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    TResult<void> Send(const void* data, u32 size) noexcept override;

    /**
     * 非ブロッキング recvfrom() で 1 datagram を取り出す。
     *
     * @details WSAEWOULDBLOCK は「受信なし」として out=0 の成功で返す (Err にしない)。
     * @param out_buffer 受信データを書き込む先。
     * @param capacity out_buffer の容量。
     * @return 受信バイト数 (受信なしなら 0) を持つ TResult、失敗ならエラー。
     */
    TResult<u32> Receive(void* out_buffer, u32 capacity) noexcept override;

    /**
     * recv 待ちバイト数を ioctlsocket(FIONREAD) で問い合わせる。
     *
     * @return 次に取り出せる datagram のバイト数 (失敗 / 未接続なら 0)。
     */
    u32 PendingBytesIn() const noexcept override;

    /**
     * 常に 0 を返す (UDP は OS が即送出し内部バッファを持たない)。
     *
     * @return 常に 0。
     */
    u32 PendingBytesOut() const noexcept override
    {
        return 0;
    }

private:
    /** socket の利用可否と解放保留を区別する内部状態。 */
    enum class EState : u8 {
        Disconnected,
        Configuring,
        Established,
        CleanupPending,
    };

    /** 無効な socket 値 (全 bit 1 = INVALID_SOCKET と同値)。 */
    static constexpr uptr kInvalidSocket = ~uptr{0};

    /** m_StateLock を保持した状態で socket と WSA 参照を回収する。0 は完全解放。 */
    u32 DisconnectLocked() noexcept;

    /** 全公開操作から内部状態を保護する。 */
    mutable FMutex m_StateLock;

    /** 現在の接続・解放状態。 */
    EState m_State = EState::Disconnected;

    /** open な UDP socket を uptr で保持 (未接続なら kInvalidSocket)。 */
    uptr m_Socket = kInvalidSocket;

    /** bind する local port (0 = ephemeral)。 */
    u16 m_LocalPort = 0;

    /** この instance が WSAStartup を 1 回計上したか。 */
    bool m_WsaStarted = false;

    /** remote endpoint の IPv4 dotted-quad (Winsock 型を header に出さない raw 保持)。 */
    u8 m_RemoteOctets[4] = {0, 0, 0, 0};

    /** remote endpoint のポート。 */
    u16 m_RemotePort = 0;
};

/**
 * server-side snapshot 書出 / client-side snapshot 補間を担う netcode seam。
 *
 * @details
 * Server 側は AddEntitySnapshot で entity state を積み CommitSnapshot で 1 payload
 * に concat して transport.Send する。Client 側は Tick で受信 snapshot を ring buffer
 * に貯め、TryGetInterpolatedSnapshot で補間表示用の view を取り出す。wire 直列化は
 * EncodeSnapshot / DecodeSnapshot (transport 非依存の static 純粋関数) が担う。1
 * セッション 1 オブジェクトで non-copy / non-move。
 */
class CNetSnapshot {
public:
    /** 既定構築 (Init まで未初期化)。 */
    CNetSnapshot() noexcept = default;

    /** 破棄する (transport は外部所有なので触らない)。 */
    ~CNetSnapshot() noexcept = default;

    /** コピー禁止 (1 セッション 1 オブジェクト)。 */
    CNetSnapshot(const CNetSnapshot&) = delete;

    /** コピー代入も禁止。 */
    CNetSnapshot& operator=(const CNetSnapshot&) = delete;

    /** ムーブ禁止。 */
    CNetSnapshot(CNetSnapshot&&) = delete;

    /** ムーブ代入も禁止。 */
    CNetSnapshot& operator=(CNetSnapshot&&) = delete;

    /**
     * 設定をコピーし ring buffer を確保して初期化する。
     *
     * @details
     * role = Standalone では transport は nullptr を許容する。それ以外で nullptr が
     * 渡された場合は内部で GetTransportStub() に差し替えてリンク互換を保つ。
     * @param config ランタイム設定 (互換APIでは公開上限の範囲へ正規化する)。
     * @param role 動作役割。
     * @param transport 使用する transport (Standalone なら nullptr 可)。
     */
    void Init(const FNetSnapshotConfig& config, ENetRole role, INetTransport* transport) noexcept;

    /**
     * 設定を検証して初期化を試みる。
     *
     * @details ring 確保を含む全検証が成功するまで既存 state を変更しない。
     * @return 成功、InvalidConfig、AllocationFailed のいずれか。
     */
    TResult<void> TryInit(const FNetSnapshotConfig& config, ENetRole role, INetTransport* transport) noexcept;

    /** ring buffer / pending entities / 統計値をリセットする (transport は触らない)。 */
    void Shutdown() noexcept;

    /**
     * 動作役割を返す。
     *
     * @return Init で設定した ENetRole。
     */
    ENetRole Role() const noexcept
    {
        return m_Role;
    }

    /**
     * ring buffer に貯まっている snapshot 件数を返す。
     *
     * @return 有効な snapshot 件数。
     */
    u32 BufferedSnapshotCount() const noexcept;

    /**
     * 現在の補間遅延秒を返す。
     *
     * @return config.interpolation_delay_sec。
     */
    f32 CurrentInterpolationDelay() const noexcept
    {
        return m_Config.interpolation_delay_sec;
    }

    /**
     * 最後に受信した snapshot の tick を返す。
     *
     * @return 直近受信 snapshot の tick (未受信なら 0)。
     */
    u32 LastReceivedTick() const noexcept
    {
        return m_LastReceivedTick;
    }

    /**
     * 1 entity の現在 state を pending list に積む。
     *
     * @details data は内部にコピーされ CommitSnapshot まで保持される。Client /
     * Standalone では no-op。
     * @param entity_id ゲーム内 entity ID。
     * @param component_mask どの component が含まれるかの bitmask。
     * @param data 積む component データ (コピーされる)。
     * @param data_size data のバイト数。
     */
    void AddEntitySnapshot(u32 entity_id, u32 component_mask, const void* data, u32 data_size) noexcept;

    /**
     * entity state を上限付きで pending list に追加する。
     *
     * @details 失敗時は pending list と合計サイズを変更しない。
     * @return 成功、WrongRole、BadArgument、PendingLimit、AllocationFailed のいずれか。
     */
    TResult<void> TryAddEntitySnapshot(u32 entity_id, u32 component_mask, const void* data,
                                       u32 data_size) noexcept;

    /**
     * pending list を 1 payload に concat し、header を付けて transport.Send する。
     *
     * @details
     * EncodeSnapshot で frame bytes を構築して best-effort 送信する。pending list は
     * 成否によらずクリアされる。Client / Standalone では no-op。
     * @param tick この snapshot の server tick。
     */
    void CommitSnapshot(u32 tick) noexcept;

    /**
     * pending snapshot の直列化と送信を試みる。
     *
     * @details 失敗時は pending entities、sequence、ring、統計値を変更しないため、
     * caller は同じ snapshot を再送できる。成功時だけ pending list を消費する。
     */
    TResult<void> TryCommitSnapshot(u32 tick) noexcept;

    /**
     * client 時刻に対応する snapshot を取り出して補間結果を書き出す。
     *
     * @details
     * out_snapshots の component_data は ring buffer 内 payload を指す非所有 view で
     * 次の Tick() 呼出まで有効。現状は最新 snapshot を view として返す (float 単位の
     * lerp はコンポーネントスキーマ導入後に本実装)。
     * @param client_time_sec client 側の現在時刻秒 (現状は未使用)。
     * @param out_snapshots 書き出し先の FEntitySnapshot 配列。
     * @param max_count out_snapshots の容量。
     * @param out_actual_count 書き出した entity 数を返す (max_count で clamp)。
     * @return 有効データを書いたら true、buffer 空 / 範囲外なら false。
     */
    bool TryGetInterpolatedSnapshot(f32 client_time_sec, FEntitySnapshot* out_snapshots, u32 max_count,
                                    u32& out_actual_count) noexcept;

    /**
     * transport.Receive を pump し、受信 snapshot を ring buffer に積む。
     *
     * @details Server 役割は受信側を持たないので no-op。検証失敗 frame は破棄する。
     * @param dt 前フレームからの経過秒 (jitter buffer 用に予約。現状は無視)。
     */
    void Tick(f32 dt) noexcept;

    /**
     * transport.Receive を上限付きで pump し、詳細結果を返す。
     *
     * @details 各 message は完全検証と allocation 成功後にだけ ring へ commit する。
     * transport が capacity 超の byte 数を返した場合は契約違反として即停止する。
     */
    FNetSnapshotTickResult TryTick(f32 dt) noexcept;

    /**
     * 送信に成功した packet 数を返す。
     *
     * @return 累積 packet 送信数。
     */
    u32 PacketsSent() const noexcept
    {
        return m_PacketsSent;
    }

    /**
     * 受信した packet 数を返す。
     *
     * @return 累積 packet 受信数 (検証失敗 frame も含む)。
     */
    u32 PacketsReceived() const noexcept
    {
        return m_PacketsReceived;
    }

    /**
     * 送信した総バイト数を返す。
     *
     * @return 累積送信バイト数。
     */
    u32 BytesSent() const noexcept
    {
        return m_BytesSent;
    }

    /**
     * 受信した総バイト数を返す。
     *
     * @return 累積受信バイト数。
     */
    u32 BytesReceived() const noexcept
    {
        return m_BytesReceived;
    }

    /** 検証で棄却した frame の飽和累積数。 */
    u32 RejectedPackets() const noexcept
    {
        return m_RejectedPackets;
    }

    /** transport 契約違反の飽和累積数。 */
    u32 TransportContractViolations() const noexcept
    {
        return m_TransportContractViolations;
    }

    /**
     * 1 frame に必要な総バイト数を返す。
     *
     * @details = magic(4) + version(4) + header(24) + payload_size + crc32(4)。
     * @param payload_size payload のバイト数。
     * @return frame 全体のバイト数 (u32 上限を超えたら u32 max に clamp)。
     */
    static u32 EncodedSnapshotSize(u32 payload_size) noexcept;

    /**
     * header + payload を frame wire layout で out_buffer に直列化する。
     *
     * @details
     * frame layout は [magic 'ACSN'][version][FSnapshotHeader(24)][payload][crc32]。
     * crc32 は magic を除いた [version .. payload 末尾) を対象に計算し footer に格納する
     * (header 内 crc32 フィールドは内部で 0 として扱う)。
     * @param header 書き出す FSnapshotHeader。
     * @param payload payload バイト列 (payload_size==0 なら nullptr 可)。
     * @param payload_size payload のバイト数 (header.payload_size と一致必須)。
     * @param out_buffer frame を書き込む先。
     * @param out_capacity out_buffer の容量。
     * @param out_written 書き込みバイト数を返す (= EncodedSnapshotSize)。
     * @return 成功なら空の TResult、引数不正 / 容量不足ならエラー。
     */
    static TResult<void> EncodeSnapshot(const FSnapshotHeader& header, const u8* payload, u32 payload_size,
                                        u8* out_buffer, u32 out_capacity, u32& out_written) noexcept;

    /**
     * frame wire bytes を検証し、header を復元 + payload を複製する。
     *
     * @details
     * magic / version / size / CRC32 を順に検証し、不正なら対応する Err を返す。
     * out_payload は置換セマンティクス。検証またはallocation失敗時は
     * out_header、out_payloadの内容・サイズ・capacity・pointerを変更しない。
     * @param buffer 解釈する frame bytes。
     * @param size buffer のバイト数。
     * @param out_header 復元した header を書き込む先 (crc32 は footer の値を復元)。
     * @param out_payload payload を複製する先。
     * @return 成功なら空の TResult、検証失敗なら対応するエラー。
     */
    static TResult<void> DecodeSnapshot(const u8* buffer, u32 size, FSnapshotHeader& out_header,
                                        TArray<u8>& out_payload) noexcept;

private:
    /**
     * 1 ring buffer エントリ (FSnapshotHeader + payload バイト列)。
     *
     * @details payload は AddEntitySnapshot で積んだ全 entity の concat。
     */
    struct FBufferedSnapshot {
        /** この snapshot の header。 */
        FSnapshotHeader header{};

        /** 全 entity record を concat した payload。 */
        TArray<u8> payload;
    };

    /**
     * server 側で 1 entity 分の state を保持する pending エントリ。
     *
     * @details AddEntitySnapshot で 1 件ずつコピーして積み、CommitSnapshot で payload に concat する。
     */
    struct FPendingEntity {
        /** ゲーム内 entity ID。 */
        u32 entity_id = 0;

        /** どの component が含まれるかの bitmask。 */
        u32 component_mask = 0;

        /** component データのコピー保持。 */
        TArray<u8> data;
    };

    /** payload 内 1 entity の固定ヘッダ長 (entity_id + component_mask + data_size = 12)。 */
    static constexpr u32 kEntityHeaderSize = 12;

    /** 補間結果を保持する temporary 領域 (返す view が指す先。client 側のみ使用)。 */
    TArray<u8> m_InterpScratch;

    /** ランタイム設定 (Init でコピー)。 */
    FNetSnapshotConfig m_Config{};

    /** 動作役割。 */
    ENetRole m_Role = ENetRole::Standalone;

    /** 使用する transport (非所有。Standalone 以外は nullptr なら stub に差し替え)。 */
    INetTransport* m_Transport = nullptr;

    /** server 側で送信待ちの entity list。 */
    TArray<FPendingEntity> m_PendingEntities;

    /** client 側の受信 snapshot ring buffer (capacity = buffer_capacity_snapshots)。 */
    TArray<FBufferedSnapshot> m_RingBuffer;

    /** ring buffer の次の挿入位置 (FIFO)。 */
    u32 m_RingHead = 0;

    /** ring buffer 内の現在の有効件数 (<= capacity)。 */
    u32 m_RingCount = 0;

    /** server 送信時の次 sequence 番号 (0 は無効値として予約)。 */
    u32 m_NextSequence = 1;

    /** 最後に受信した snapshot の tick。 */
    u32 m_LastReceivedTick = 0;

    /** 送信に成功した packet 数。 */
    u32 m_PacketsSent = 0;

    /** 受信した packet 数 (検証失敗 frame も含む)。 */
    u32 m_PacketsReceived = 0;

    /** 送信した総バイト数。 */
    u32 m_BytesSent = 0;

    /** 受信した総バイト数。 */
    u32 m_BytesReceived = 0;

    /** pending entity wire payload の合計バイト数。 */
    u32 m_PendingPayloadBytes = 0;

    /** frame / entity payload 検証で棄却した packet 数。 */
    u32 m_RejectedPackets = 0;

    /** transport が Receive 契約に違反した回数。 */
    u32 m_TransportContractViolations = 0;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FNetSnapshot = CNetSnapshot;

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FNetTransportStub = CNetTransportStub;

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FUdpTransport = CUdpTransport;

} // namespace acs::game

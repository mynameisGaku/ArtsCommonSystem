// SPDX-License-Identifier: Apache-2.0
#include <winsock2.h>

#include "test/Expect.h"
#include "test/Test.h"

#include "foundation/Limits.h"
#include "foundation/Platform.h"
#include "network/Network.h"
#include "network/TcpConnection.h"
#include "network/TcpListener.h"
#include "network/UdpSocket.h"

using namespace acs;

namespace {

/** ノンブロッキング受信を待つ最大時間。 */
constexpr u64 kNetworkOperationTimeoutMs = 3000;

/** WinSock 呼出し有無を見分けるスレッド固有エラー値。 */
constexpr int kSocketErrorSentinel = WSAEPROTONOSUPPORT;

/** WinSock の長さ引数を 1 バイト超える値。 */
constexpr usize kOversizedSocketBuffer = static_cast<usize>(TNumLimits<i32>::Max()) + 1u;

/**
 * 操作が指定値を返し、WinSock の最終エラーを変更しないことを確認する。
 * @param operation 検査する引数なし操作。
 * @param expected_result 期待する戻り値。
 */
template <typename TOperation>
void ExpectSocketCallSkipped(TOperation&& operation, isize expected_result) {
    ::WSASetLastError(kSocketErrorSentinel);
    /** 操作の戻り値。 */
    const isize result = operation();
    /** 操作直後のスレッド固有 WinSock エラー。 */
    const int socket_error = ::WSAGetLastError();
    EXPECT_EQ(result, expected_result);
    EXPECT_EQ(socket_error, kSocketErrorSentinel);
}

/**
 * ノンブロッキングのリスナーから期限内に 1 接続を受理する。
 * @param listener 接続を待つリスナー。
 * @return 受理結果。期限まで接続がなければ最後の WinSock エラー。
 */
TResult<FTcpConnection> AcceptBeforeDeadline(FTcpListener& listener) noexcept {
    /** 待機開始時刻。 */
    const u64 started_at = static_cast<u64>(::GetTickCount64());
    /** 最初の受理結果。 */
    auto accepted = listener.Accept();
    while (accepted.IsErr() && ::WSAGetLastError() == WSAEWOULDBLOCK && static_cast<u64>(::GetTickCount64()) - started_at < kNetworkOperationTimeoutMs) {
        ::Sleep(1u);
        accepted = listener.Accept();
    }
    return accepted;
}

/**
 * ノンブロッキング TCP 接続から期限内にデータを受信する。
 * @param connection 受信側の接続。
 * @param buffer 書き込み先。
 * @param size 書き込み先のバイト数。
 * @return 受信結果。期限までデータがなければ最後の -1。
 */
isize ReceiveTcpBeforeDeadline(FTcpConnection& connection, void* buffer, usize size) noexcept {
    /** 待機開始時刻。 */
    const u64 started_at = static_cast<u64>(::GetTickCount64());
    /** 最初の受信結果。 */
    isize received = connection.Recv(buffer, size);
    while (received < 0 && ::WSAGetLastError() == WSAEWOULDBLOCK && static_cast<u64>(::GetTickCount64()) - started_at < kNetworkOperationTimeoutMs) {
        ::Sleep(1u);
        received = connection.Recv(buffer, size);
    }
    return received;
}

/**
 * ノンブロッキング UDP ソケットから期限内に 1 通受信する。
 * @param socket 受信側のソケット。
 * @param buffer 書き込み先。
 * @param size 書き込み先のバイト数。
 * @param from 送信元の書き込み先。
 * @return 受信結果。期限までデータがなければ最後の -1。
 */
isize ReceiveUdpBeforeDeadline(FUdpSocket& socket, void* buffer, usize size, FIpAddress& from) noexcept {
    /** 待機開始時刻。 */
    const u64 started_at = static_cast<u64>(::GetTickCount64());
    /** 最初の受信結果。 */
    isize received = socket.RecvFrom(buffer, size, from);
    while (received < 0 && ::WSAGetLastError() == WSAEWOULDBLOCK && static_cast<u64>(::GetTickCount64()) - started_at < kNetworkOperationTimeoutMs) {
        ::Sleep(1u);
        received = socket.RecvFrom(buffer, size, from);
    }
    return received;
}

/** IPv4 値が指定した 4 オクテットを持つことを確認する。 */
void ExpectAddress(FIpAddress address, u8 first, u8 second, u8 third, u8 fourth) {
    EXPECT_EQ(address.octets[0], first);
    EXPECT_EQ(address.octets[1], second);
    EXPECT_EQ(address.octets[2], third);
    EXPECT_EQ(address.octets[3], fourth);
}

} // namespace

/** TCP が OS 割当ポートを使い、不正入力を WinSock 前で拒否することを確認する。 */
ACS_TEST(NetworkSocketIo, TcpRejectsInvalidBuffersBeforeCallingOs) {
    /** この動作確認が所有するネットワーク初期化結果。 */
    auto initialized = FNetwork::Init();
    EXPECT_TRUE(initialized.IsOk());
    if (initialized.IsErr()) return;

    {
        /** 無効ソケットでは 0 バイトでも接続エラーを優先する。 */
        FTcpConnection invalid_connection;
        ExpectSocketCallSkipped([&invalid_connection]() { return invalid_connection.Send(nullptr, 0); }, -1);
        ExpectSocketCallSkipped([&invalid_connection]() { return invalid_connection.Recv(nullptr, 0); }, -1);

        /** OS に空きポートを選ばせた待ち受け結果。 */
        auto listener_result = FTcpListener::Listen(FIpAddress::Loopback(), 0);
        EXPECT_TRUE(listener_result.IsOk());
        if (listener_result.IsOk()) {
            /** 接続を受け付ける待ち受け。 */
            FTcpListener& listener = listener_result.Value();
            /** OS が選んだ待ち受け先。 */
            auto listener_address_result = listener.LocalAddress();
            EXPECT_TRUE(listener_address_result.IsOk());
            EXPECT_TRUE(listener.SetNonBlocking(true).IsOk());

            if (listener_address_result.IsOk()) {
                /** 接続に使う実ポート。 */
                const u16 listener_port = listener_address_result.Value().port;
                EXPECT_TRUE(listener_port != 0);
                ExpectAddress(listener_address_result.Value(), 127u, 0u, 0u, 1u);

                /** 待ち受け先へ接続した結果。 */
                auto client_result = FTcpConnection::Connect(FIpAddress::Loopback(), listener_port);
                EXPECT_TRUE(client_result.IsOk());
                if (client_result.IsOk()) {
                    /** 期限付きで接続を受け付けた結果。 */
                    auto server_result = AcceptBeforeDeadline(listener);
                    EXPECT_TRUE(server_result.IsOk());
                    if (server_result.IsOk()) {
                        /** 送信側の TCP 接続。 */
                        FTcpConnection& client = client_result.Value();
                        /** 受信側の TCP 接続。 */
                        FTcpConnection& server = server_result.Value();
                        EXPECT_TRUE(server.SetNonBlocking(true).IsOk());
                        /** 上限超過検査で参照する有効な 1 バイト。 */
                        const char sent_byte = 'A';
                        /** 通常受信で書き換わる 1 バイト。 */
                        char received_byte = '\0';

                        ExpectSocketCallSkipped([&client]() { return client.Send(nullptr, 0); }, 0);
                        ExpectSocketCallSkipped([&server]() { return server.Recv(nullptr, 0); }, 0);
                        ExpectSocketCallSkipped([&client]() { return client.Send(nullptr, 1); }, -1);
                        ExpectSocketCallSkipped([&server]() { return server.Recv(nullptr, 1); }, -1);
                        ExpectSocketCallSkipped([&client, &sent_byte]() { return client.Send(&sent_byte, kOversizedSocketBuffer); }, -1);
                        ExpectSocketCallSkipped([&server, &received_byte]() { return server.Recv(&received_byte, kOversizedSocketBuffer); }, -1);

                        /** 通常の 1 バイト送信結果。 */
                        const isize sent_size = client.Send(&sent_byte, 1);
                        EXPECT_EQ(sent_size, 1);
                        if (sent_size == 1) {
                            EXPECT_EQ(ReceiveTcpBeforeDeadline(server, &received_byte, 1), 1);
                            EXPECT_EQ(received_byte, sent_byte);
                        }
                    }
                }
            }
        }
    }

    FNetwork::Shutdown();
}

/** UDP の空データグラムを維持し、不正入力を WinSock 前で拒否することを確認する。 */
ACS_TEST(NetworkSocketIo, UdpPreservesEmptyDatagramsAndRejectsInvalidBuffers) {
    /** この動作確認が所有するネットワーク初期化結果。 */
    auto initialized = FNetwork::Init();
    EXPECT_TRUE(initialized.IsOk());
    if (initialized.IsErr()) return;

    {
        /** 無効ソケットでは 0 バイトでもソケットエラーを優先する。 */
        FUdpSocket invalid_socket;
        FIpAddress invalid_from = FIpAddress::FromString("9.8.7.6");
        invalid_from.port = 9876;
        ExpectSocketCallSkipped([&invalid_socket]() { return invalid_socket.SendTo(FIpAddress::Loopback(), 1, nullptr, 0); }, -1);
        ExpectSocketCallSkipped([&invalid_socket, &invalid_from]() { return invalid_socket.RecvFrom(nullptr, 0, invalid_from); }, -1);
        ExpectAddress(invalid_from, 9u, 8u, 7u, 6u);
        EXPECT_EQ(invalid_from.port, 9876u);

        /** OS に空きポートを選ばせた受信口。 */
        auto receiver_result = FUdpSocket::Bind(FIpAddress::Loopback(), 0);
        /** OS に空きポートを選ばせた送信口。 */
        auto sender_result = FUdpSocket::Bind(FIpAddress::Loopback(), 0);
        EXPECT_TRUE(receiver_result.IsOk());
        EXPECT_TRUE(sender_result.IsOk());

        if (receiver_result.IsOk() && sender_result.IsOk()) {
            /** 空データグラムを受け取る値。 */
            FUdpSocket& receiver = receiver_result.Value();
            /** 空データグラムを送る値。 */
            FUdpSocket& sender = sender_result.Value();
            /** OS が選んだ受信先。 */
            auto receiver_address_result = receiver.LocalAddress();
            /** OS が選んだ送信元。 */
            auto sender_address_result = sender.LocalAddress();
            EXPECT_TRUE(receiver_address_result.IsOk());
            EXPECT_TRUE(sender_address_result.IsOk());
            EXPECT_TRUE(receiver.SetNonBlocking(true).IsOk());

            if (receiver_address_result.IsOk() && sender_address_result.IsOk()) {
                /** 実際の受信ポート。 */
                const u16 receiver_port = receiver_address_result.Value().port;
                EXPECT_TRUE(receiver_port != 0);
                EXPECT_TRUE(sender_address_result.Value().port != 0);
                /** 上限超過検査で参照する有効な 1 バイト。 */
                const char sent_byte = 'U';
                /** 上限超過検査で参照する書き込み可能な 1 バイト。 */
                char received_byte = '\0';
                /** 拒否時に変更されないことを確かめる送信元。 */
                FIpAddress unchanged_from = FIpAddress::FromString("1.2.3.4");
                unchanged_from.port = 4321;

                ExpectSocketCallSkipped([&sender, receiver_port]() { return sender.SendTo(FIpAddress::Loopback(), receiver_port, nullptr, 1); }, -1);
                ExpectSocketCallSkipped([&sender, receiver_port, &sent_byte]() { return sender.SendTo(FIpAddress::Loopback(), receiver_port, &sent_byte, kOversizedSocketBuffer); }, -1);
                ExpectSocketCallSkipped([&receiver, &unchanged_from]() { return receiver.RecvFrom(nullptr, 1, unchanged_from); }, -1);
                ExpectSocketCallSkipped([&receiver, &received_byte, &unchanged_from]() { return receiver.RecvFrom(&received_byte, kOversizedSocketBuffer, unchanged_from); }, -1);
                ExpectAddress(unchanged_from, 1u, 2u, 3u, 4u);
                EXPECT_EQ(unchanged_from.port, 4321u);

                /** 空データグラムの送信結果。 */
                const isize empty_send_size = sender.SendTo(FIpAddress::Loopback(), receiver_port, nullptr, 0);
                EXPECT_EQ(empty_send_size, 0);
                if (empty_send_size == 0) {
                    /** 空データグラムの送信元。 */
                    FIpAddress empty_from{};
                    EXPECT_EQ(ReceiveUdpBeforeDeadline(receiver, nullptr, 0, empty_from), 0);
                    ExpectAddress(empty_from, 127u, 0u, 0u, 1u);
                    EXPECT_EQ(empty_from.port, sender_address_result.Value().port);
                }

                /** OS の受信失敗でも変更されないことを確かめる送信元。 */
                FIpAddress no_data_from = FIpAddress::FromString("5.6.7.8");
                no_data_from.port = 8765;
                ::WSASetLastError(kSocketErrorSentinel);
                EXPECT_EQ(receiver.RecvFrom(&received_byte, 1, no_data_from), -1);
                EXPECT_EQ(::WSAGetLastError(), WSAEWOULDBLOCK);
                ExpectAddress(no_data_from, 5u, 6u, 7u, 8u);
                EXPECT_EQ(no_data_from.port, 8765u);
            }
        }
    }

    FNetwork::Shutdown();
}

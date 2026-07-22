// SPDX-License-Identifier: Apache-2.0
// NetSnapshot の hostile wire / transport 境界と transaction 契約。
#include "test/Test.h"
#include "test/Expect.h"

#include "gameframework/NetSnapshot.h"
#include "memory/Allocator.h"
#include "memory/Memory.h"

using namespace acs;
using namespace acs::game;

namespace {

/** 通常確保後に OOM を決定論的に注入できる allocator。 */
class FSwitchableFailAllocator final : public FAllocator {
public:
    explicit FSwitchableFailAllocator(FAllocator& backing) noexcept : m_Backing(&backing)
    {
    }

    void SetFailing(bool failing) noexcept
    {
        m_Failing = failing;
    }

    void* Alloc(usize size, usize alignment, FSourceLoc location) noexcept override
    {
        return m_Failing ? nullptr : m_Backing->Alloc(size, alignment, location);
    }

    void Free(void* pointer) noexcept override
    {
        m_Backing->Free(pointer);
    }

private:
    FAllocator* m_Backing = nullptr;
    bool m_Failing = false;
};

/** message 境界と故障を script できる小さな transport fake。 */
class FScriptedTransport final : public INetTransport {
public:
    TResult<void> Connect(const char* address, u16 port) noexcept override
    {
        (void)address;
        (void)port;
        m_Connected = true;
        return Ok();
    }

    void Disconnect() noexcept override
    {
        m_Connected = false;
    }

    bool IsConnected() const noexcept override
    {
        return m_Connected;
    }

    TResult<void> Send(const void* data, u32 size) noexcept override
    {
        if (m_FailSend) {
            return ACS_ERR(IO, FNetSnapshotError::kSub_BufferFull, "FScriptedTransport: injected send failure");
        }
        if (data == nullptr || size > sizeof(m_LastSent)) {
            return ACS_ERR(IO, FNetSnapshotError::kSub_BufferFull, "FScriptedTransport: send capture too small");
        }
        MemCopy(m_LastSent, data, size);
        m_LastSentSize = size;
        return Ok();
    }

    TResult<u32> Receive(void* out_buffer, u32 capacity) noexcept override
    {
        if (m_ReadIndex >= m_MessageCount) return TResult<u32>(OkInit, 0u);
        const FMessage& message = m_Messages[m_ReadIndex++];
        if (message.contract_size_delta != 0) {
            return TResult<u32>(OkInit, capacity + message.contract_size_delta);
        }
        if (out_buffer == nullptr || message.size > capacity) {
            return ACS_ERR(IO, FNetSnapshotError::kSub_DatagramTruncated,
                           "FScriptedTransport: receive capacity too small");
        }
        MemCopy(out_buffer, message.bytes, message.size);
        return TResult<u32>(OkInit, message.size);
    }

    u32 PendingBytesIn() const noexcept override
    {
        return 0;
    }

    u32 PendingBytesOut() const noexcept override
    {
        return 0;
    }

    bool Queue(const u8* bytes, u32 size) noexcept
    {
        if (bytes == nullptr || size > sizeof(m_Messages[0].bytes) || m_MessageCount >= 4u) return false;
        FMessage& message = m_Messages[m_MessageCount++];
        MemCopy(message.bytes, bytes, size);
        message.size = size;
        return true;
    }

    void QueueContractViolation() noexcept
    {
        if (m_MessageCount < 4u) m_Messages[m_MessageCount++].contract_size_delta = 1u;
    }

    struct FMessage {
        u8 bytes[512] = {};
        u32 size = 0;
        u32 contract_size_delta = 0;
    };

    FMessage m_Messages[4] = {};
    u8 m_LastSent[512] = {};
    u32 m_LastSentSize = 0;
    u32 m_MessageCount = 0;
    u32 m_ReadIndex = 0;
    bool m_Connected = true;
    bool m_FailSend = false;
};

/** entity record 1 件を含む正規 frame を作る。 */
u32 MakeEntityFrame(u8* frame, u32 frame_capacity, u32 sequence = 7u) noexcept
{
    const u8 payload[16] = {
        1, 0, 0, 0,       // entity_id
        0x0F, 0, 0, 0,    // component_mask
        4, 0, 0, 0,       // data_size
        9, 8, 7, 6,       // data
    };
    FSnapshotHeader header{};
    header.tick = 42;
    header.sequence = sequence;
    header.server_timestamp_us = 123456;
    header.payload_size = sizeof(payload);
    u32 written = 0;
    const TResult<void> encoded =
        FNetSnapshot::EncodeSnapshot(header, payload, sizeof(payload), frame, frame_capacity, written);
    return encoded.IsOk() ? written : 0u;
}

} // namespace

ACS_TEST(NetSnapshotSafety, CodecRejectsTruncationTrailingCrcAndNoncanonicalHeaderTransactionally)
{
    u8 frame[128] = {};
    const u32 written = MakeEntityFrame(frame, sizeof(frame));
    EXPECT_TRUE(written > 0u);

    FSnapshotHeader output{};
    output.tick = 0xAABBCCDDu;
    TArray<u8> payload;
    payload.Resize(2u);
    payload[0] = 0x55u;
    payload[1] = 0xAAu;

    EXPECT_TRUE(FNetSnapshot::DecodeSnapshot(frame, written - 1u, output, payload).IsErr());
    EXPECT_EQ(output.tick, 0xAABBCCDDu);
    EXPECT_EQ(payload.Size(), static_cast<usize>(2u));

    frame[written] = 0xCCu;
    EXPECT_TRUE(FNetSnapshot::DecodeSnapshot(frame, written + 1u, output, payload).IsErr());
    EXPECT_EQ(output.tick, 0xAABBCCDDu);
    EXPECT_EQ(payload[0], static_cast<u8>(0x55u));

    frame[written - 1u] ^= 0x80u;
    EXPECT_TRUE(FNetSnapshot::DecodeSnapshot(frame, written, output, payload).IsErr());
    EXPECT_EQ(output.tick, 0xAABBCCDDu);
    frame[written - 1u] ^= 0x80u;

    frame[28] = 1u; // header.crc32 は wire 上の予約0。
    const TResult<void> noncanonical = FNetSnapshot::DecodeSnapshot(frame, written, output, payload);
    EXPECT_TRUE(noncanonical.IsErr());
    if (noncanonical.IsErr()) {
        EXPECT_EQ(noncanonical.Error().subcode,
                  static_cast<u16>(FNetSnapshotError::kSub_NonCanonicalHeader));
    }
    EXPECT_EQ(output.tick, 0xAABBCCDDu);
    EXPECT_EQ(payload.Size(), static_cast<usize>(2u));
}

ACS_TEST(NetSnapshotSafety, EncodeRejectsProductLimitWithoutPartialFrame)
{
    FSnapshotHeader header{};
    header.sequence = 1u;
    header.payload_size = kNetSnapshotMaximumPayloadBytes + 1u;
    u8 payload_byte = 1u;
    u8 output[8] = {0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A};
    u32 written = 99u;

    const TResult<void> result =
        FNetSnapshot::EncodeSnapshot(header, &payload_byte, header.payload_size, output, sizeof(output), written);
    EXPECT_TRUE(result.IsErr());
    if (result.IsErr()) {
        EXPECT_EQ(result.Error().subcode, static_cast<u16>(FNetSnapshotError::kSub_FrameTooLarge));
    }
    EXPECT_EQ(written, 0u);
    EXPECT_EQ(output[0], static_cast<u8>(0x5Au));
    EXPECT_EQ(output[7], static_cast<u8>(0x5Au));
}

ACS_TEST(NetSnapshotSafety, DecodeAllocationFailurePreservesBothOutputs)
{
    u8 frame[128] = {};
    const u32 written = MakeEntityFrame(frame, sizeof(frame));
    EXPECT_TRUE(written > 0u);

    FSwitchableFailAllocator allocator(DefaultAllocator());
    TArray<u8> payload(allocator);
    EXPECT_TRUE(payload.TryResize(1u));
    payload[0] = 0xC3u;
    u8* const original_pointer = payload.Data();
    const usize original_capacity = payload.Capacity();
    allocator.SetFailing(true);

    FSnapshotHeader output{};
    output.tick = 0x12345678u;
    const TResult<void> result = FNetSnapshot::DecodeSnapshot(frame, written, output, payload);
    EXPECT_TRUE(result.IsErr());
    if (result.IsErr()) {
        EXPECT_EQ(result.Error().subcode, static_cast<u16>(FNetSnapshotError::kSub_AllocationFailed));
    }
    EXPECT_EQ(output.tick, 0x12345678u);
    EXPECT_EQ(payload.Size(), static_cast<usize>(1u));
    EXPECT_EQ(payload[0], static_cast<u8>(0xC3u));
    EXPECT_TRUE(payload.Data() == original_pointer);
    EXPECT_EQ(payload.Capacity(), original_capacity);
}

ACS_TEST(NetSnapshotSafety, CodecRejectsAliasedInputAndOutputStorage)
{
    u8 frame[128] = {};
    const u32 written = MakeEntityFrame(frame, sizeof(frame));
    EXPECT_TRUE(written > 0u);

    TArray<u8> aliased;
    aliased.Resize(written);
    MemCopy(aliased.Data(), frame, written);
    u8* const original_pointer = aliased.Data();
    const usize original_size = aliased.Size();
    const usize original_capacity = aliased.Capacity();
    FSnapshotHeader decoded{};
    decoded.tick = 0xDEADBEEFu;
    const TResult<void> decode =
        FNetSnapshot::DecodeSnapshot(aliased.Data(), written, decoded, aliased);
    EXPECT_TRUE(decode.IsErr());
    if (decode.IsErr()) {
        EXPECT_EQ(decode.Error().subcode, static_cast<u16>(FNetSnapshotError::kSub_BadArgument));
    }
    EXPECT_TRUE(aliased.Data() == original_pointer);
    EXPECT_EQ(aliased.Size(), original_size);
    EXPECT_EQ(aliased.Capacity(), original_capacity);
    EXPECT_EQ(decoded.tick, 0xDEADBEEFu);

    u8 overlapping[128] = {};
    for (u32 i = 0; i < 16u; ++i) overlapping[40u + i] = static_cast<u8>(i + 1u);
    FSnapshotHeader header{};
    header.sequence = 1u;
    header.payload_size = 16u;
    u32 encoded_size = 88u;
    const TResult<void> encode =
        FNetSnapshot::EncodeSnapshot(header, overlapping + 40u, 16u, overlapping, sizeof(overlapping), encoded_size);
    EXPECT_TRUE(encode.IsErr());
    if (encode.IsErr()) {
        EXPECT_EQ(encode.Error().subcode, static_cast<u16>(FNetSnapshotError::kSub_BadArgument));
    }
    EXPECT_EQ(encoded_size, 0u);
    EXPECT_EQ(overlapping[0], static_cast<u8>(0u));
    EXPECT_EQ(overlapping[40], static_cast<u8>(1u));
}

ACS_TEST(NetSnapshotSafety, CheckedInitRejectsInvalidConfigWithoutReplacingSession)
{
    FScriptedTransport transport;
    FNetSnapshot snapshot;
    FNetSnapshotConfig valid{};
    valid.buffer_capacity_snapshots = 2u;
    EXPECT_TRUE(snapshot.TryInit(valid, ENetRole::Client, &transport).IsOk());

    FNetSnapshotConfig invalid = valid;
    invalid.buffer_capacity_snapshots = kNetSnapshotMaximumRingCapacity + 1u;
    const TResult<void> result = snapshot.TryInit(invalid, ENetRole::Server, &transport);
    EXPECT_TRUE(result.IsErr());
    EXPECT_EQ(static_cast<u32>(snapshot.Role()), static_cast<u32>(ENetRole::Client));
    EXPECT_EQ(snapshot.BufferedSnapshotCount(), 0u);
}

ACS_TEST(NetSnapshotSafety, CheckedCommitRetainsPendingStateAcrossTransportFailure)
{
    FScriptedTransport transport;
    FNetSnapshot snapshot;
    FNetSnapshotConfig config{};
    config.buffer_capacity_snapshots = 2u;
    EXPECT_TRUE(snapshot.TryInit(config, ENetRole::Server, &transport).IsOk());

    const u8 state[4] = {1, 2, 3, 4};
    EXPECT_TRUE(snapshot.TryAddEntitySnapshot(77u, 3u, state, sizeof(state)).IsOk());
    transport.m_FailSend = true;
    EXPECT_TRUE(snapshot.TryCommitSnapshot(99u).IsErr());
    EXPECT_EQ(snapshot.PacketsSent(), 0u);
    EXPECT_EQ(transport.m_LastSentSize, 0u);

    transport.m_FailSend = false;
    EXPECT_TRUE(snapshot.TryCommitSnapshot(99u).IsOk());
    EXPECT_EQ(snapshot.PacketsSent(), 1u);
    EXPECT_TRUE(transport.m_LastSentSize > 0u);

    FSnapshotHeader header{};
    TArray<u8> payload;
    EXPECT_TRUE(FNetSnapshot::DecodeSnapshot(transport.m_LastSent, transport.m_LastSentSize, header, payload).IsOk());
    EXPECT_EQ(header.sequence, 1u);
    EXPECT_EQ(header.tick, 99u);
    EXPECT_EQ(payload.Size(), static_cast<usize>(16u));
}

ACS_TEST(NetSnapshotSafety, TickCommitsOnlyCompleteMessagesAndStopsOnContractViolation)
{
    u8 valid[128] = {};
    const u32 valid_size = MakeEntityFrame(valid, sizeof(valid));
    EXPECT_TRUE(valid_size > 0u);
    u8 corrupt[128] = {};
    MemCopy(corrupt, valid, valid_size);
    corrupt[valid_size - 1u] ^= 0x40u;

    FScriptedTransport transport;
    EXPECT_TRUE(transport.Queue(valid, valid_size));
    EXPECT_TRUE(transport.Queue(corrupt, valid_size));
    transport.QueueContractViolation();

    FNetSnapshot snapshot;
    FNetSnapshotConfig config{};
    config.buffer_capacity_snapshots = 2u;
    EXPECT_TRUE(snapshot.TryInit(config, ENetRole::Client, &transport).IsOk());

    const FNetSnapshotTickResult tick = snapshot.TryTick(0.0f);
    EXPECT_FALSE(tick.Succeeded());
    EXPECT_EQ(tick.stop_subcode, static_cast<u16>(FNetSnapshotError::kSub_TransportContractViolation));
    EXPECT_EQ(tick.received_messages, 2u);
    EXPECT_EQ(tick.accepted_snapshots, 1u);
    EXPECT_EQ(tick.rejected_messages, 2u);
    EXPECT_EQ(snapshot.BufferedSnapshotCount(), 1u);
    EXPECT_EQ(snapshot.PacketsReceived(), 2u);
    EXPECT_EQ(snapshot.RejectedPackets(), 2u);
    EXPECT_EQ(snapshot.TransportContractViolations(), 1u);

    FEntitySnapshot entities[2] = {};
    u32 entity_count = 0;
    EXPECT_TRUE(snapshot.TryGetInterpolatedSnapshot(0.0f, entities, 2u, entity_count));
    EXPECT_EQ(entity_count, 1u);
    EXPECT_EQ(entities[0].entity_id, 1u);
    EXPECT_EQ(entities[0].component_data_size, 4u);
}

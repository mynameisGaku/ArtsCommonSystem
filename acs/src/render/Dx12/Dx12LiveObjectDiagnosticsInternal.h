// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Render - D3D12 / DXGI live-object 診断の内部共通処理
// -----------------------------------------------------------------------------
// ReportLiveObjects の HRESULT はレポート呼び出しの成否だけを表す。
// 残存オブジェクトの有無はデバッグ出力の内容を確認するまで確定しない。
// =============================================================================
#pragma once

#include "foundation/Compiler.h"

#if ACS_BUILD_DEBUG

#    include "foundation/Log.h"
#    include "foundation/Types.h"

#    include <d3d12.h>
#    include <d3d12sdklayers.h>
#    include <dxgi1_3.h>
#    include <dxgidebug.h>

namespace acs::render_internal {

constexpr u32 kMaximumMessageIdentitiesToRecord = 16u;
constexpr usize kRecordedDescriptionBytes = 192u;

struct FGpuDebugMessageIdentity {
    u32 identifier = 0;
    u32 severity = 0;
    bool known_diagnostic_message = false;
    char description[kRecordedDescriptionBytes]{};
};

/** 詳細レポート対応インターフェースを優先して保持する診断専用ハンドル。 */
struct FD3D12DebugDeviceReportHandle {
    ID3D12DebugDevice2* detailed_device = nullptr;
    ID3D12DebugDevice* basic_device = nullptr;

    bool IsAvailable() const noexcept
    {
        return detailed_device != nullptr || basic_device != nullptr;
    }

    void Release() noexcept
    {
        if (detailed_device) {
            detailed_device->Release();
            detailed_device = nullptr;
        }
        if (basic_device) {
            basic_device->Release();
            basic_device = nullptr;
        }
    }
};

/** InfoQueue のレポート前後差分と、取得できたメッセージの severity 集計。 */
struct FGpuDebugMessageSummary {
    bool queue_available = false;
    bool queue_monotonic = true;
    bool inspection_truncated = false;
    bool filter_override_attempted = false;
    bool filter_override_succeeded = false;
    bool filter_restore_succeeded = false;

    u64 stored_message_count_before = 0;
    u64 stored_message_count_after = 0;
    u64 new_stored_message_count = 0;
    u64 retrievable_message_count_before = 0;
    u64 retrievable_message_count_after = 0;
    u64 new_retrievable_message_count = 0;
    u64 discarded_message_count_before = 0;
    u64 discarded_message_count_after = 0;
    u64 newly_discarded_message_count = 0;

    u64 inspected_message_count = 0;
    u64 unreadable_message_count = 0;
    u64 corruption_message_count = 0;
    u64 error_message_count = 0;
    u64 warning_message_count = 0;
    u64 information_message_count = 0;
    u64 ordinary_message_count = 0;
    u64 unknown_severity_message_count = 0;
    u64 known_diagnostic_message_count = 0;
    u64 live_object_message_count = 0;

    u32 recorded_identity_count = 0;
    FGpuDebugMessageIdentity recorded_identities[kMaximumMessageIdentitiesToRecord]{};
};

constexpr u64 kMaximumMessagesToInspect = 65536u;
constexpr usize kMessageBufferBytes = 4096u;

/** D3D12 InfoQueue の既存フィルタを保持したまま、診断中だけ全メッセージを通す。 */
class FD3D12InfoQueueUnfilteredScope final {
public:
    explicit FD3D12InfoQueueUnfilteredScope(ID3D12InfoQueue* information_queue) noexcept
        : m_InformationQueue(information_queue)
    {
        if (!m_InformationQueue) return;

        m_StorageStackSizeBefore = m_InformationQueue->GetStorageFilterStackSize();
        m_RetrievalStackSizeBefore = m_InformationQueue->GetRetrievalFilterStackSize();

        if (FAILED(m_InformationQueue->PushEmptyStorageFilter())) {
            return;
        }
        m_StorageFilterPushed = true;

        if (FAILED(m_InformationQueue->PushEmptyRetrievalFilter())) {
            Restore();
            return;
        }
        m_RetrievalFilterPushed = true;

        m_Active = m_InformationQueue->GetStorageFilterStackSize() == m_StorageStackSizeBefore + 1u &&
                   m_InformationQueue->GetRetrievalFilterStackSize() == m_RetrievalStackSizeBefore + 1u;
        if (!m_Active) Restore();
    }

    ~FD3D12InfoQueueUnfilteredScope() noexcept
    {
        Restore();
    }

    FD3D12InfoQueueUnfilteredScope(const FD3D12InfoQueueUnfilteredScope&) = delete;
    FD3D12InfoQueueUnfilteredScope& operator=(const FD3D12InfoQueueUnfilteredScope&) = delete;

    bool IsActive() const noexcept
    {
        return m_Active;
    }

    bool WasRestored() const noexcept
    {
        return m_RestoreAttempted && m_RestoreSucceeded;
    }

    void Restore() noexcept
    {
        if (!m_InformationQueue || (!m_StorageFilterPushed && !m_RetrievalFilterPushed)) return;

        m_RestoreAttempted = true;
        if (m_RetrievalFilterPushed) {
            m_InformationQueue->PopRetrievalFilter();
            m_RetrievalFilterPushed = false;
        }
        if (m_StorageFilterPushed) {
            m_InformationQueue->PopStorageFilter();
            m_StorageFilterPushed = false;
        }
        m_Active = false;
        m_RestoreSucceeded = m_InformationQueue->GetStorageFilterStackSize() == m_StorageStackSizeBefore &&
                             m_InformationQueue->GetRetrievalFilterStackSize() == m_RetrievalStackSizeBefore;
    }

private:
    ID3D12InfoQueue* m_InformationQueue = nullptr;
    UINT m_StorageStackSizeBefore = 0;
    UINT m_RetrievalStackSizeBefore = 0;
    bool m_StorageFilterPushed = false;
    bool m_RetrievalFilterPushed = false;
    bool m_Active = false;
    bool m_RestoreAttempted = false;
    bool m_RestoreSucceeded = false;
};

/** DXGI InfoQueue の既存フィルタを保持したまま、診断中だけ全メッセージを通す。 */
class FDxgiInfoQueueUnfilteredScope final {
public:
    explicit FDxgiInfoQueueUnfilteredScope(IDXGIInfoQueue* information_queue) noexcept
        : m_InformationQueue(information_queue)
    {
        if (!m_InformationQueue) return;

        m_StorageStackSizeBefore = m_InformationQueue->GetStorageFilterStackSize(DXGI_DEBUG_ALL);
        m_RetrievalStackSizeBefore = m_InformationQueue->GetRetrievalFilterStackSize(DXGI_DEBUG_ALL);

        if (FAILED(m_InformationQueue->PushEmptyStorageFilter(DXGI_DEBUG_ALL))) {
            return;
        }
        m_StorageFilterPushed = true;

        if (FAILED(m_InformationQueue->PushEmptyRetrievalFilter(DXGI_DEBUG_ALL))) {
            Restore();
            return;
        }
        m_RetrievalFilterPushed = true;

        m_Active = m_InformationQueue->GetStorageFilterStackSize(DXGI_DEBUG_ALL) == m_StorageStackSizeBefore + 1u &&
                   m_InformationQueue->GetRetrievalFilterStackSize(DXGI_DEBUG_ALL) == m_RetrievalStackSizeBefore + 1u;
        if (!m_Active) Restore();
    }

    ~FDxgiInfoQueueUnfilteredScope() noexcept
    {
        Restore();
    }

    FDxgiInfoQueueUnfilteredScope(const FDxgiInfoQueueUnfilteredScope&) = delete;
    FDxgiInfoQueueUnfilteredScope& operator=(const FDxgiInfoQueueUnfilteredScope&) = delete;

    bool IsActive() const noexcept
    {
        return m_Active;
    }

    bool WasRestored() const noexcept
    {
        return m_RestoreAttempted && m_RestoreSucceeded;
    }

    void Restore() noexcept
    {
        if (!m_InformationQueue || (!m_StorageFilterPushed && !m_RetrievalFilterPushed)) return;

        m_RestoreAttempted = true;
        if (m_RetrievalFilterPushed) {
            m_InformationQueue->PopRetrievalFilter(DXGI_DEBUG_ALL);
            m_RetrievalFilterPushed = false;
        }
        if (m_StorageFilterPushed) {
            m_InformationQueue->PopStorageFilter(DXGI_DEBUG_ALL);
            m_StorageFilterPushed = false;
        }
        m_Active = false;
        m_RestoreSucceeded = m_InformationQueue->GetStorageFilterStackSize(DXGI_DEBUG_ALL) ==
                                 m_StorageStackSizeBefore &&
                             m_InformationQueue->GetRetrievalFilterStackSize(DXGI_DEBUG_ALL) ==
                                 m_RetrievalStackSizeBefore;
    }

private:
    IDXGIInfoQueue* m_InformationQueue = nullptr;
    UINT m_StorageStackSizeBefore = 0;
    UINT m_RetrievalStackSizeBefore = 0;
    bool m_StorageFilterPushed = false;
    bool m_RetrievalFilterPushed = false;
    bool m_Active = false;
    bool m_RestoreAttempted = false;
    bool m_RestoreSucceeded = false;
};

/** process-global な DXGI InfoQueue の差分採取を直列化する排他ガード。 */
class FDxgiLiveObjectDiagnosticsLockGuard final {
public:
    explicit FDxgiLiveObjectDiagnosticsLockGuard(SRWLOCK& lock) noexcept : m_Lock(lock)
    {
        ::AcquireSRWLockExclusive(&m_Lock);
    }

    ~FDxgiLiveObjectDiagnosticsLockGuard() noexcept
    {
        ::ReleaseSRWLockExclusive(&m_Lock);
    }

    FDxgiLiveObjectDiagnosticsLockGuard(const FDxgiLiveObjectDiagnosticsLockGuard&) = delete;
    FDxgiLiveObjectDiagnosticsLockGuard& operator=(const FDxgiLiveObjectDiagnosticsLockGuard&) = delete;

private:
    SRWLOCK& m_Lock;
};

/** 全バックエンドで共有する DXGI 診断ロックを返す。 */
inline SRWLOCK& DxgiLiveObjectDiagnosticsLock() noexcept
{
    static SRWLOCK lock = SRWLOCK_INIT;
    return lock;
}

/** DXGI 診断ロック保持中に呼び、process-global な診断順序番号を払い出す。 */
inline u64 NextDxgiLiveObjectDiagnosticSequence() noexcept
{
    static u64 sequence = 0;
    ++sequence;
    if (sequence == 0) ++sequence;
    return sequence;
}

inline const char* BooleanText(bool value) noexcept
{
    return value ? "true" : "false";
}

inline FD3D12DebugDeviceReportHandle CaptureD3D12DebugDeviceReportHandle(ID3D12Device* device) noexcept
{
    FD3D12DebugDeviceReportHandle handle{};
    if (!device) {
        return handle;
    }

    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&handle.detailed_device))) && handle.detailed_device) {
        return handle;
    }
    handle.detailed_device = nullptr;
    (void)device->QueryInterface(IID_PPV_ARGS(&handle.basic_device));
    return handle;
}

inline u64 NonNegativeDifference(u64 current, u64 baseline) noexcept
{
    return current >= baseline ? current - baseline : 0u;
}

inline bool BoundedTextStartsWith(const char* text, usize text_bytes, const char* prefix) noexcept
{
    if (!text || text_bytes == 0 || !prefix) return false;
    for (usize index = 0; prefix[index] != '\0'; ++index) {
        if (index >= text_bytes || text[index] == '\0' || text[index] != prefix[index]) return false;
    }
    return true;
}

inline bool BoundedTextEndsWith(const char* text, usize text_bytes, const char* suffix) noexcept
{
    if (!text || text_bytes == 0 || !suffix) return false;

    usize text_length = 0;
    while (text_length < text_bytes && text[text_length] != '\0')
        ++text_length;
    usize suffix_length = 0;
    while (suffix[suffix_length] != '\0')
        ++suffix_length;
    if (suffix_length > text_length) return false;

    const usize suffix_offset = text_length - suffix_length;
    for (usize index = 0; index < suffix_length; ++index) {
        if (text[suffix_offset + index] != suffix[index]) return false;
    }
    return true;
}

inline void RecordMessageIdentity(u32 identifier, u32 severity, bool known_diagnostic_message, const char* description,
                                  usize description_bytes, FGpuDebugMessageSummary& summary) noexcept
{
    if (summary.recorded_identity_count >= kMaximumMessageIdentitiesToRecord) {
        return;
    }

    FGpuDebugMessageIdentity& identity = summary.recorded_identities[summary.recorded_identity_count++];
    identity.identifier = identifier;
    identity.severity = severity;
    identity.known_diagnostic_message = known_diagnostic_message;

    // ログを1行の key-value として扱えるよう、制御文字と引用符を無害化する。
    if (!description || description_bytes == 0) {
        return;
    }
    usize output_index = 0;
    for (usize input_index = 0; input_index < description_bytes && description[input_index] != '\0' &&
                                output_index + 1 < kRecordedDescriptionBytes;
         ++input_index) {
        char character = description[input_index];
        if (character == '\r' || character == '\n' || character == '\t') {
            character = ' ';
        } else if (character == '"') {
            character = '\'';
        }
        identity.description[output_index++] = character;
    }
    identity.description[output_index] = '\0';
}

inline void CaptureD3D12QueueBeforeReport(ID3D12InfoQueue* information_queue, FGpuDebugMessageSummary& summary) noexcept
{
    if (!information_queue) {
        return;
    }

    summary.queue_available = true;
    summary.stored_message_count_before = information_queue->GetNumStoredMessages();
    summary.retrievable_message_count_before = information_queue->GetNumStoredMessagesAllowedByRetrievalFilter();
    summary.discarded_message_count_before = information_queue->GetNumMessagesDiscardedByMessageCountLimit();
}

inline bool IsKnownD3D12LiveObjectDiagnosticMessage(D3D12_MESSAGE_ID identifier, const char* description,
                                                    usize description_bytes) noexcept
{
    if (identifier == D3D12_MESSAGE_ID_LIVE_OBJECT_SUMMARY) {
        constexpr const char* detailed_prefix =
            "Using ID3D12DebugDevice2::ReportLiveDeviceObjects with D3D12_RLDO_DETAIL";
        constexpr const char* basic_prefix = "Using ID3D12DebugDevice::ReportLiveDeviceObjects with D3D12_RLDO_DETAIL";
        return BoundedTextStartsWith(description, description_bytes, detailed_prefix) ||
               BoundedTextStartsWith(description, description_bytes, basic_prefix);
    }

    if (identifier == D3D12_MESSAGE_ID_LIVE_DEVICE) {
        constexpr const char* device_prefix = "Live ID3D12Device at ";
        constexpr const char* expected_reference_count = ", Refcount: 2";
        return BoundedTextStartsWith(description, description_bytes, device_prefix) &&
               BoundedTextEndsWith(description, description_bytes, expected_reference_count);
    }

    return false;
}

inline void CountD3D12MessageSeverity(D3D12_MESSAGE_SEVERITY severity, bool known_diagnostic_message,
                                      FGpuDebugMessageSummary& summary) noexcept
{
    if (known_diagnostic_message) {
        ++summary.known_diagnostic_message_count;
        return;
    }

    ++summary.live_object_message_count;
    switch (severity) {
    case D3D12_MESSAGE_SEVERITY_CORRUPTION:
        ++summary.corruption_message_count;
        break;
    case D3D12_MESSAGE_SEVERITY_ERROR:
        ++summary.error_message_count;
        break;
    case D3D12_MESSAGE_SEVERITY_WARNING:
        ++summary.warning_message_count;
        break;
    case D3D12_MESSAGE_SEVERITY_INFO:
        ++summary.information_message_count;
        break;
    case D3D12_MESSAGE_SEVERITY_MESSAGE:
        ++summary.ordinary_message_count;
        break;
    default:
        ++summary.unknown_severity_message_count;
        break;
    }
}

inline void CaptureD3D12QueueAfterReport(ID3D12InfoQueue* information_queue, FGpuDebugMessageSummary& summary) noexcept
{
    if (!information_queue) {
        return;
    }

    summary.stored_message_count_after = information_queue->GetNumStoredMessages();
    summary.retrievable_message_count_after = information_queue->GetNumStoredMessagesAllowedByRetrievalFilter();
    summary.discarded_message_count_after = information_queue->GetNumMessagesDiscardedByMessageCountLimit();
    summary.queue_monotonic = summary.stored_message_count_after >= summary.stored_message_count_before &&
                              summary.retrievable_message_count_after >= summary.retrievable_message_count_before &&
                              summary.discarded_message_count_after >= summary.discarded_message_count_before;
    summary.new_stored_message_count = NonNegativeDifference(summary.stored_message_count_after,
                                                             summary.stored_message_count_before);
    summary.new_retrievable_message_count = NonNegativeDifference(summary.retrievable_message_count_after,
                                                                  summary.retrievable_message_count_before);
    summary.newly_discarded_message_count = NonNegativeDifference(summary.discarded_message_count_after,
                                                                  summary.discarded_message_count_before);

    const u64 message_count = summary.new_retrievable_message_count > kMaximumMessagesToInspect
                                  ? kMaximumMessagesToInspect
                                  : summary.new_retrievable_message_count;
    summary.inspection_truncated = !summary.queue_monotonic || summary.newly_discarded_message_count != 0 ||
                                   message_count != summary.new_retrievable_message_count;

    for (u64 offset = 0; offset < message_count; ++offset) {
        ++summary.inspected_message_count;
        const UINT64 message_index = summary.retrievable_message_count_before + offset;
        SIZE_T message_bytes = 0;
        const HRESULT size_result = information_queue->GetMessage(message_index, nullptr, &message_bytes);
        if (FAILED(size_result) || message_bytes < sizeof(D3D12_MESSAGE) || message_bytes > kMessageBufferBytes) {
            ++summary.unreadable_message_count;
            continue;
        }

        alignas(D3D12_MESSAGE) byte message_buffer[kMessageBufferBytes]{};
        auto* const message = reinterpret_cast<D3D12_MESSAGE*>(message_buffer);
        const HRESULT message_result = information_queue->GetMessage(message_index, message, &message_bytes);
        if (FAILED(message_result)) {
            ++summary.unreadable_message_count;
            continue;
        }
        const bool known_diagnostic_message = IsKnownD3D12LiveObjectDiagnosticMessage(
            message->ID, message->pDescription, static_cast<usize>(message->DescriptionByteLength));
        RecordMessageIdentity(static_cast<u32>(message->ID), static_cast<u32>(message->Severity),
                              known_diagnostic_message, message->pDescription,
                              static_cast<usize>(message->DescriptionByteLength), summary);
        CountD3D12MessageSeverity(message->Severity, known_diagnostic_message, summary);
    }
}

inline void CaptureDxgiQueueBeforeReport(IDXGIInfoQueue* information_queue, FGpuDebugMessageSummary& summary) noexcept
{
    if (!information_queue) {
        return;
    }

    summary.queue_available = true;
    summary.stored_message_count_before = information_queue->GetNumStoredMessages(DXGI_DEBUG_ALL);
    summary.retrievable_message_count_before = information_queue->GetNumStoredMessagesAllowedByRetrievalFilters(
        DXGI_DEBUG_ALL);
    summary.discarded_message_count_before = information_queue->GetNumMessagesDiscardedByMessageCountLimit(
        DXGI_DEBUG_ALL);
}

inline void CountDxgiMessageSeverity(DXGI_INFO_QUEUE_MESSAGE_SEVERITY severity,
                                     FGpuDebugMessageSummary& summary) noexcept
{
    ++summary.live_object_message_count;
    switch (severity) {
    case DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION:
        ++summary.corruption_message_count;
        break;
    case DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR:
        ++summary.error_message_count;
        break;
    case DXGI_INFO_QUEUE_MESSAGE_SEVERITY_WARNING:
        ++summary.warning_message_count;
        break;
    case DXGI_INFO_QUEUE_MESSAGE_SEVERITY_INFO:
        ++summary.information_message_count;
        break;
    case DXGI_INFO_QUEUE_MESSAGE_SEVERITY_MESSAGE:
        ++summary.ordinary_message_count;
        break;
    default:
        ++summary.unknown_severity_message_count;
        break;
    }
}

inline void CaptureDxgiQueueAfterReport(IDXGIInfoQueue* information_queue, FGpuDebugMessageSummary& summary) noexcept
{
    if (!information_queue) {
        return;
    }

    summary.stored_message_count_after = information_queue->GetNumStoredMessages(DXGI_DEBUG_ALL);
    summary.retrievable_message_count_after = information_queue->GetNumStoredMessagesAllowedByRetrievalFilters(
        DXGI_DEBUG_ALL);
    summary.discarded_message_count_after = information_queue->GetNumMessagesDiscardedByMessageCountLimit(
        DXGI_DEBUG_ALL);
    summary.queue_monotonic = summary.stored_message_count_after >= summary.stored_message_count_before &&
                              summary.retrievable_message_count_after >= summary.retrievable_message_count_before &&
                              summary.discarded_message_count_after >= summary.discarded_message_count_before;
    summary.new_stored_message_count = NonNegativeDifference(summary.stored_message_count_after,
                                                             summary.stored_message_count_before);
    summary.new_retrievable_message_count = NonNegativeDifference(summary.retrievable_message_count_after,
                                                                  summary.retrievable_message_count_before);
    summary.newly_discarded_message_count = NonNegativeDifference(summary.discarded_message_count_after,
                                                                  summary.discarded_message_count_before);

    const u64 message_count = summary.new_retrievable_message_count > kMaximumMessagesToInspect
                                  ? kMaximumMessagesToInspect
                                  : summary.new_retrievable_message_count;
    summary.inspection_truncated = !summary.queue_monotonic || summary.newly_discarded_message_count != 0 ||
                                   message_count != summary.new_retrievable_message_count;

    for (u64 offset = 0; offset < message_count; ++offset) {
        ++summary.inspected_message_count;
        const UINT64 message_index = summary.retrievable_message_count_before + offset;
        SIZE_T message_bytes = 0;
        const HRESULT size_result = information_queue->GetMessage(DXGI_DEBUG_ALL, message_index, nullptr,
                                                                  &message_bytes);
        if (FAILED(size_result) || message_bytes < sizeof(DXGI_INFO_QUEUE_MESSAGE) ||
            message_bytes > kMessageBufferBytes) {
            ++summary.unreadable_message_count;
            continue;
        }

        alignas(DXGI_INFO_QUEUE_MESSAGE) byte message_buffer[kMessageBufferBytes]{};
        auto* const message = reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE*>(message_buffer);
        const HRESULT message_result = information_queue->GetMessage(DXGI_DEBUG_ALL, message_index, message,
                                                                     &message_bytes);
        if (FAILED(message_result)) {
            ++summary.unreadable_message_count;
            continue;
        }
        RecordMessageIdentity(static_cast<u32>(message->ID), static_cast<u32>(message->Severity), false,
                              message->pDescription, static_cast<usize>(message->DescriptionByteLength), summary);
        CountDxgiMessageSeverity(message->Severity, summary);
    }
}

enum class EGpuLiveObjectVerdict : u8 {
    Inconclusive,
    NoLeak,
    Leak,
};

inline bool IsGpuDebugMessageInspectionComplete(const FGpuDebugMessageSummary& summary) noexcept
{
    const bool filter_contract_succeeded = summary.filter_override_attempted && summary.filter_override_succeeded &&
                                           summary.filter_restore_succeeded;
    return summary.queue_available && filter_contract_succeeded && summary.queue_monotonic &&
           !summary.inspection_truncated && summary.unreadable_message_count == 0;
}

inline EGpuLiveObjectVerdict EvaluateGpuLiveObjectVerdict(HRESULT report_result,
                                                          const FGpuDebugMessageSummary& summary) noexcept
{
    if (FAILED(report_result) || !IsGpuDebugMessageInspectionComplete(summary)) {
        return EGpuLiveObjectVerdict::Inconclusive;
    }
    return summary.live_object_message_count == 0 ? EGpuLiveObjectVerdict::NoLeak : EGpuLiveObjectVerdict::Leak;
}

inline void WriteGpuLiveObjectReportMarker(const char* tracker, const char* backend, HRESULT report_result,
                                           const char* report_interface, HRESULT information_queue_result,
                                           const FGpuDebugMessageSummary& summary, u64 diagnostic_sequence) noexcept
{
    ACS_LOG_INFO("[acs][memory] tracker=%s record=report backend=%s report_executed=true "
                 "diagnostic_sequence=%llu report_interface=%s api_call_status=%s result=0x%08lx "
                 "report_result_is_leak_verdict=false "
                 "leak_detected=inconclusive status=inconclusive reason=%s "
                 "requires_debug_output_analysis=true",
                 tracker, backend, static_cast<unsigned long long>(diagnostic_sequence), report_interface,
                 SUCCEEDED(report_result) ? "success" : "failure", static_cast<unsigned long>(report_result),
                 SUCCEEDED(report_result) ? "requires_debug_output_analysis" : "report_api_failed");

    const bool inspection_succeeded = IsGpuDebugMessageInspectionComplete(summary);
    ACS_LOG_INFO("[acs][memory] tracker=%s record=info_queue backend=%s "
                 "diagnostic_sequence=%llu info_queue_available=%s api_call_status=%s result=0x%08lx "
                 "inspection_succeeded=%s filter_override_attempted=%s "
                 "filter_override_succeeded=%s filter_restore_succeeded=%s "
                 "monotonic=%s new_stored_messages=%llu "
                 "new_retrievable_messages=%llu newly_discarded_messages=%llu "
                 "inspected_messages=%llu unreadable_messages=%llu "
                 "inspection_truncated=%s",
                 tracker, backend, static_cast<unsigned long long>(diagnostic_sequence),
                 BooleanText(summary.queue_available),
                 SUCCEEDED(information_queue_result) && summary.queue_available ? "success" : "unavailable",
                 static_cast<unsigned long>(information_queue_result), BooleanText(inspection_succeeded),
                 BooleanText(summary.filter_override_attempted), BooleanText(summary.filter_override_succeeded),
                 BooleanText(summary.filter_restore_succeeded), BooleanText(summary.queue_monotonic),
                 static_cast<unsigned long long>(summary.new_stored_message_count),
                 static_cast<unsigned long long>(summary.new_retrievable_message_count),
                 static_cast<unsigned long long>(summary.newly_discarded_message_count),
                 static_cast<unsigned long long>(summary.inspected_message_count),
                 static_cast<unsigned long long>(summary.unreadable_message_count),
                 BooleanText(summary.inspection_truncated));

    const bool severe_messages_detected = summary.corruption_message_count != 0 || summary.error_message_count != 0 ||
                                          summary.warning_message_count != 0;
    ACS_LOG_INFO("[acs][memory] tracker=%s record=severity backend=%s "
                 "diagnostic_sequence=%llu severe_messages_detected=%s "
                 "corruption_messages=%llu error_messages=%llu "
                 "warning_messages=%llu information_messages=%llu ordinary_messages=%llu "
                 "unknown_severity_messages=%llu known_diagnostic_messages=%llu live_object_messages=%llu",
                 tracker, backend, static_cast<unsigned long long>(diagnostic_sequence),
                 BooleanText(severe_messages_detected),
                 static_cast<unsigned long long>(summary.corruption_message_count),
                 static_cast<unsigned long long>(summary.error_message_count),
                 static_cast<unsigned long long>(summary.warning_message_count),
                 static_cast<unsigned long long>(summary.information_message_count),
                 static_cast<unsigned long long>(summary.ordinary_message_count),
                 static_cast<unsigned long long>(summary.unknown_severity_message_count),
                 static_cast<unsigned long long>(summary.known_diagnostic_message_count),
                 static_cast<unsigned long long>(summary.live_object_message_count));

    for (u32 index = 0; index < summary.recorded_identity_count; ++index) {
        const FGpuDebugMessageIdentity& identity = summary.recorded_identities[index];
        ACS_LOG_INFO("[acs][memory] tracker=%s record=info_queue_message backend=%s "
                     "diagnostic_sequence=%llu ordinal=%u identifier=%u severity=%u "
                     "known_diagnostic_message=%s description=\"%s\"",
                     tracker, backend, static_cast<unsigned long long>(diagnostic_sequence), index, identity.identifier,
                     identity.severity, BooleanText(identity.known_diagnostic_message), identity.description);
    }
    if (summary.inspected_message_count > summary.recorded_identity_count) {
        ACS_LOG_INFO("[acs][memory] tracker=%s record=info_queue_message_limit backend=%s "
                     "diagnostic_sequence=%llu recorded_messages=%u omitted_messages=%llu",
                     tracker, backend, static_cast<unsigned long long>(diagnostic_sequence),
                     summary.recorded_identity_count,
                     static_cast<unsigned long long>(summary.inspected_message_count -
                                                     summary.recorded_identity_count));
    }

    const EGpuLiveObjectVerdict verdict = EvaluateGpuLiveObjectVerdict(report_result, summary);
    if (verdict == EGpuLiveObjectVerdict::Leak) {
        ACS_LOG_ERROR("[acs][memory] tracker=%s record=verdict backend=%s diagnostic_sequence=%llu "
                      "leak_detected=true status=leak_detected live_object_messages=%llu "
                      "known_diagnostic_messages=%llu inspection_succeeded=true",
                      tracker, backend, static_cast<unsigned long long>(diagnostic_sequence),
                      static_cast<unsigned long long>(summary.live_object_message_count),
                      static_cast<unsigned long long>(summary.known_diagnostic_message_count));
    } else if (verdict == EGpuLiveObjectVerdict::NoLeak) {
        ACS_LOG_INFO("[acs][memory] tracker=%s record=verdict backend=%s diagnostic_sequence=%llu "
                     "leak_detected=false status=ok live_object_messages=0 known_diagnostic_messages=%llu "
                     "inspection_succeeded=true",
                     tracker, backend, static_cast<unsigned long long>(diagnostic_sequence),
                     static_cast<unsigned long long>(summary.known_diagnostic_message_count));
    } else {
        ACS_LOG_INFO("[acs][memory] tracker=%s record=verdict backend=%s diagnostic_sequence=%llu "
                     "leak_detected=inconclusive status=inconclusive live_object_messages=%llu "
                     "known_diagnostic_messages=%llu inspection_succeeded=false",
                     tracker, backend, static_cast<unsigned long long>(diagnostic_sequence),
                     static_cast<unsigned long long>(summary.live_object_message_count),
                     static_cast<unsigned long long>(summary.known_diagnostic_message_count));
    }
}

/** D3D12 live-object reportを実行する。handle の所有権は移動しない。 */
inline void ReportD3D12LiveObjects(const FD3D12DebugDeviceReportHandle& handle, const char* backend) noexcept
{
    if (!handle.IsAvailable()) {
        ACS_LOG_INFO("[acs][memory] tracker=d3d12_live_objects record=report backend=%s "
                     "report_executed=false api_call_status=not_executed "
                     "report_result_is_leak_verdict=false leak_detected=inconclusive "
                     "status=unavailable reason=debug_interface_unavailable "
                     "requires_debug_output_analysis=false report_interface=none "
                     "info_queue_available=false",
                     backend);
        return;
    }

    ID3D12InfoQueue* information_queue = nullptr;
    HRESULT information_queue_result = E_NOINTERFACE;
    FGpuDebugMessageSummary summary{};
    HRESULT report_result = E_NOINTERFACE;
    const char* report_interface = "ID3D12DebugDevice";

    if (handle.detailed_device) {
        report_interface = "ID3D12DebugDevice2";
        information_queue_result = handle.detailed_device->QueryInterface(IID_PPV_ARGS(&information_queue));
        FD3D12InfoQueueUnfilteredScope filter_scope(information_queue);
        summary.filter_override_attempted = information_queue != nullptr;
        summary.filter_override_succeeded = filter_scope.IsActive();
        CaptureD3D12QueueBeforeReport(information_queue, summary);
        report_result = handle.detailed_device->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_SUMMARY |
                                                                        D3D12_RLDO_IGNORE_INTERNAL);
        CaptureD3D12QueueAfterReport(information_queue, summary);
        filter_scope.Restore();
        summary.filter_restore_succeeded = filter_scope.WasRestored();
    } else {
        information_queue_result = handle.basic_device->QueryInterface(IID_PPV_ARGS(&information_queue));
        FD3D12InfoQueueUnfilteredScope filter_scope(information_queue);
        summary.filter_override_attempted = information_queue != nullptr;
        summary.filter_override_succeeded = filter_scope.IsActive();
        CaptureD3D12QueueBeforeReport(information_queue, summary);
        report_result = handle.basic_device->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_SUMMARY |
                                                                     D3D12_RLDO_IGNORE_INTERNAL);
        CaptureD3D12QueueAfterReport(information_queue, summary);
        filter_scope.Restore();
        summary.filter_restore_succeeded = filter_scope.WasRestored();
    }

    if (!summary.filter_override_succeeded || !summary.filter_restore_succeeded) {
        summary.inspection_truncated = true;
    }
    WriteGpuLiveObjectReportMarker("d3d12_live_objects", backend, report_result, report_interface,
                                   information_queue_result, summary, 0);
    if (information_queue) {
        information_queue->Release();
    }
}

/** DXGI live-object reportを実行し、取得できたInfoQueue差分を集計する。 */
inline void ReportDxgiLiveObjects(const char* backend) noexcept
{
    SRWLOCK& diagnostic_lock = DxgiLiveObjectDiagnosticsLock();
    FDxgiLiveObjectDiagnosticsLockGuard diagnostic_guard(diagnostic_lock);
    const u64 diagnostic_sequence = NextDxgiLiveObjectDiagnosticSequence();

    IDXGIDebug1* debug = nullptr;
    const HRESULT interface_result = ::DXGIGetDebugInterface1(0, IID_PPV_ARGS(&debug));
    if (FAILED(interface_result) || !debug) {
        ACS_LOG_INFO("[acs][memory] tracker=dxgi_live_objects record=report backend=%s diagnostic_sequence=%llu "
                     "report_executed=false api_call_status=not_executed result=0x%08lx "
                     "report_result_is_leak_verdict=false leak_detected=inconclusive "
                     "status=unavailable reason=debug_interface_unavailable "
                     "requires_debug_output_analysis=false info_queue_available=false",
                     backend, static_cast<unsigned long long>(diagnostic_sequence),
                     static_cast<unsigned long>(interface_result));
        if (debug) {
            debug->Release();
        }
        return;
    }

    IDXGIInfoQueue* information_queue = nullptr;
    const HRESULT information_queue_result = ::DXGIGetDebugInterface1(0, IID_PPV_ARGS(&information_queue));
    FGpuDebugMessageSummary summary{};
    FDxgiInfoQueueUnfilteredScope filter_scope(information_queue);
    summary.filter_override_attempted = information_queue != nullptr;
    summary.filter_override_succeeded = filter_scope.IsActive();
    CaptureDxgiQueueBeforeReport(information_queue, summary);

    const auto report_flags = static_cast<DXGI_DEBUG_RLO_FLAGS>(DXGI_DEBUG_RLO_DETAIL | DXGI_DEBUG_RLO_SUMMARY |
                                                                DXGI_DEBUG_RLO_IGNORE_INTERNAL);
    const HRESULT report_result = debug->ReportLiveObjects(DXGI_DEBUG_ALL, report_flags);

    CaptureDxgiQueueAfterReport(information_queue, summary);
    filter_scope.Restore();
    summary.filter_restore_succeeded = filter_scope.WasRestored();
    if (!summary.filter_override_succeeded || !summary.filter_restore_succeeded) {
        summary.inspection_truncated = true;
    }
    WriteGpuLiveObjectReportMarker("dxgi_live_objects", backend, report_result, "IDXGIDebug1", information_queue_result,
                                   summary, diagnostic_sequence);
    if (information_queue) {
        information_queue->Release();
    }
    debug->Release();
}

} // namespace acs::render_internal

#endif // ACS_BUILD_DEBUG

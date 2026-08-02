// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — btedit / ABehaviorTreeEditorPanel 実装
//
// 仕様の意図は ABehaviorTreeEditorPanel.h を参照。本ファイルでは:
//   ・Init / Shutdown / Reset / autorun & step 制御
//   ・メタミラー (AddNode / SetNodeStatus / ClearNodes / NodeStatus)
//   ・履歴 ring buffer 操作 (kHistorySize 固定)
//   ・DrawUI: toolbar + history graph + 左 tree view + 右 node inspector
//   ・DrawTreeRecursive: parent_id → 子線形走査の再帰展開
// を実装する。すべて noexcept、STL 不使用、ImGui 依存はこの .cpp に閉じる。
#include "gameframework/tools/btedit/BehaviorTreeEditorPanel.h"
#include "gameframework/tools/btedit/BtGuardNodes.h"   // ABtConditionNode / ABtCompareNode (bake)
#include "foundation/Move.h"
#include "foundation/Platform.h"

#include <imgui.h>
#include <charconv>
#include <limits>
#include <system_error>

#include <cstdio>   // std::snprintf / fopen / fgets / sscanf (label 整形・保存読込)
#include <cmath>    // floorf (グリッド描画)
#include <cstring>  // strncmp (保存ヘッダ判定)

namespace acs::game::btedit {

/**
 * tree 再帰描画の深度上限 (= 不正な parent_id による循環参照ガード)。
 *
 * @details 実 BT で 32 階層を超えることはまず無いが、64 まで余裕を取る。
 */
static constexpr u32 kTreeRecursionLimit = 64u;

/**
 * EBtKind を表示用リテラルに変換する。
 *
 * @param k 変換元の node 種別。
 * @return "Selector" / "Sequence" / "Action" (到達不能時は "Unknown")。
 */
static const char* KindLabel(EBtKind k) noexcept {
    switch (k) {
        case EBtKind::Selector:  return "Selector";
        case EBtKind::Sequence:  return "Sequence";
        case EBtKind::Action:    return "Action";
        case EBtKind::Decorator: return "Decorator";
        case EBtKind::Task:      return "Task";
    }
    return "Unknown"; // 到達不能 (enum を u8 で広げない限り)
}

/**
 * EBtDecoratorOp を表示用リテラルに変換する。
 *
 * @param op 変換元の decorator op。
 * @return "Inverter" / "ForceSuccess" / "ForceFailure" / "Repeat"。
 */
static const char* DecoLabel(EBtDecoratorOp op) noexcept {
    switch (op) {
        case EBtDecoratorOp::Inverter:     return "Inverter";
        case EBtDecoratorOp::ForceSuccess: return "ForceSuccess";
        case EBtDecoratorOp::ForceFailure: return "ForceFailure";
        case EBtDecoratorOp::Repeat:       return "Repeat";
    }
    return "Inverter";
}

/**
 * EBtCompareOp を表示用の記号に変換する。
 *
 * @param op 変換元の比較演算子。
 * @return "<" / "<=" / "==" / "!=" / ">=" / ">"。
 */
static const char* CmpOpLabel(EBtCompareOp op) noexcept {
    switch (op) {
        case EBtCompareOp::Less:      return "<";
        case EBtCompareOp::LessEq:    return "<=";
        case EBtCompareOp::Equal:     return "==";
        case EBtCompareOp::NotEqual:  return "!=";
        case EBtCompareOp::GreaterEq: return ">=";
        case EBtCompareOp::Greater:   return ">";
    }
    return "<";
}

/**
 * EBtStatus を表示用リテラルに変換する。
 *
 * @param s 変換元の status。
 * @return "Success" / "Failure" / "Running" (到達不能時は "Unknown")。
 */
static const char* StatusLabel(EBtStatus s) noexcept {
    switch (s) {
        case EBtStatus::Success: return "Success";
        case EBtStatus::Failure: return "Failure";
        case EBtStatus::Running: return "Running";
    }
    return "Unknown";
}

/**
 * EBtStatus を PlotLines 用の float 値に変換する。
 *
 * @details Success=1.0 / Running=0.5 / Failure=0.0。視覚的に「上ほど成功・中段で実行中・下が失敗」と読める並びにする。
 * @param s 変換元の status。
 * @return プロット用の y 値 (到達不能時は 0.0)。
 */
static f32 StatusToPlotValue(EBtStatus s) noexcept {
    switch (s) {
        case EBtStatus::Success: return 1.0f;
        case EBtStatus::Running: return 0.5f;
        case EBtStatus::Failure: return 0.0f;
    }
    return 0.0f;
}

/**
 * name が null なら代替ラベルを返す。
 *
 * @param name 表示名 (nullptr 可)。
 * @return name が非 null ならそのまま、null なら "(unnamed)"。
 */
static const char* SafeName(const char* name) noexcept {
    return (name != nullptr) ? name : "(unnamed)";
}

/**
 * id が m_Nodes の有効範囲内かをチェックする。
 *
 * @param id 検査する node id。
 * @param node_count 現在の node 数 (m_Nodes.Size())。
 * @return kInvalidId でなく id < node_count なら true。
 */
static bool IsValidId(u32 id, usize node_count) noexcept {
    if (id == ABehaviorTreeEditorPanel::kInvalidId) return false;
    return static_cast<usize>(id) < node_count;
}

static FBtGraphPersistenceResult GraphFailure(
    EBtGraphPersistenceError error, u32 line = 0u, u64 bytes = 0u,
    u32 os_error = 0u) noexcept {
    FBtGraphPersistenceResult result{};
    result.error = error;
    result.line = line;
    result.bytes = bytes;
    result.os_error = os_error;
    return result;
}

static bool TryBoundedCStringLength(
    const char* text, usize limit, usize& out_length) noexcept {
    out_length = 0u;
    if (text == nullptr) return false;
    while (out_length <= limit && text[out_length] != '\0') ++out_length;
    return out_length <= limit;
}

static bool IsSafeToken(const char* text, usize length) noexcept {
    if (text == nullptr || length == 0u) return false;
    for (usize i = 0u; i < length; ++i) {
        const u8 byte = static_cast<u8>(text[i]);
        if (byte <= 0x20u || byte == 0x7Fu) return false;
    }
    return true;
}

static bool IsSafeDisplayName(const char* text, usize length) noexcept {
    if (text == nullptr) return false;
    for (usize i = 0u; i < length; ++i) {
        const u8 byte = static_cast<u8>(text[i]);
        if (byte < 0x20u || byte == 0x7Fu) return false;
    }
    return true;
}

struct FGraphTokenCursor {
    const char* Current = nullptr;
    const char* End = nullptr;

    FGraphTokenCursor(const char* text, usize length) noexcept
        : Current(text), End(text + length) {}

    void SkipSpace() noexcept {
        while (Current < End && (*Current == ' ' || *Current == '\t')) ++Current;
    }

    bool Next(const char*& begin, const char*& end) noexcept {
        SkipSpace();
        if (Current == End) return false;
        begin = Current;
        while (Current < End && *Current != ' ' && *Current != '\t') ++Current;
        end = Current;
        return true;
    }

    const char* Remainder() noexcept {
        SkipSpace();
        return Current;
    }

    bool Empty() noexcept {
        SkipSpace();
        return Current == End;
    }
};

static EBtGraphPersistenceError ParseU32Token(
    const char* begin, const char* end, u32& out) noexcept {
    if (begin == end) return EBtGraphPersistenceError::InvalidNumber;
    const char* conversion_begin = (*begin == '+') ? begin + 1 : begin;
    if (conversion_begin == end) return EBtGraphPersistenceError::InvalidNumber;
    u32 value = 0u;
    const std::from_chars_result parsed =
        std::from_chars(conversion_begin, end, value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return EBtGraphPersistenceError::InvalidNumber;
    }
    out = value;
    return EBtGraphPersistenceError::None;
}

static EBtGraphPersistenceError ParseI32Token(
    const char* begin, const char* end, i32& out) noexcept {
    if (begin == end) return EBtGraphPersistenceError::InvalidNumber;
    const char* conversion_begin = (*begin == '+') ? begin + 1 : begin;
    if (conversion_begin == end) return EBtGraphPersistenceError::InvalidNumber;
    i32 value = 0;
    const std::from_chars_result parsed =
        std::from_chars(conversion_begin, end, value, 10);
    if (parsed.ec == std::errc::result_out_of_range) {
        return EBtGraphPersistenceError::IntegerOutOfRange;
    }
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return EBtGraphPersistenceError::InvalidNumber;
    }
    out = value;
    return EBtGraphPersistenceError::None;
}

static EBtGraphPersistenceError ParseF32Token(
    const char* begin, const char* end, f32& out) noexcept {
    if (begin == end) return EBtGraphPersistenceError::InvalidNumber;
    const char* conversion_begin = (*begin == '+') ? begin + 1 : begin;
    if (conversion_begin == end) return EBtGraphPersistenceError::InvalidNumber;
    f32 value = 0.0f;
    const std::from_chars_result parsed = std::from_chars(
        conversion_begin, end, value, std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return EBtGraphPersistenceError::InvalidNumber;
    }
    if (!std::isfinite(value)) {
        return EBtGraphPersistenceError::NonFiniteNumber;
    }
    out = value;
    return EBtGraphPersistenceError::None;
}

struct FGraphTextBuilder {
    TArray<char> Text;
    EBtGraphPersistenceError Error = EBtGraphPersistenceError::None;

    bool Append(const char* bytes, usize length) noexcept {
        if (Error != EBtGraphPersistenceError::None) return false;
        if (length > ABehaviorTreeEditorPanel::kMaxGraphTextBytes - Text.Size()) {
            Error = EBtGraphPersistenceError::InputTooLarge;
            return false;
        }
        const usize old_size = Text.Size();
        if (!Text.TryResize(old_size + length)) {
            Error = EBtGraphPersistenceError::AllocationFailure;
            return false;
        }
        if (length != 0u) std::memcpy(Text.Data() + old_size, bytes, length);
        return true;
    }

    bool AppendChar(char value) noexcept { return Append(&value, 1u); }

    bool AppendCString(const char* value) noexcept {
        return Append(value, std::strlen(value));
    }

    bool AppendU32(u32 value) noexcept {
        char buffer[16]{};
        const std::to_chars_result converted =
            std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (converted.ec != std::errc{}) {
            Error = EBtGraphPersistenceError::InvalidNumber;
            return false;
        }
        return Append(buffer, static_cast<usize>(converted.ptr - buffer));
    }

    bool AppendI32(i32 value) noexcept {
        char buffer[16]{};
        const std::to_chars_result converted =
            std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (converted.ec != std::errc{}) {
            Error = EBtGraphPersistenceError::InvalidNumber;
            return false;
        }
        return Append(buffer, static_cast<usize>(converted.ptr - buffer));
    }

    bool AppendF32(f32 value) noexcept {
        if (!std::isfinite(value)) {
            Error = EBtGraphPersistenceError::NonFiniteNumber;
            return false;
        }
        char buffer[64]{};
        const std::to_chars_result converted = std::to_chars(
            buffer, buffer + sizeof(buffer), value, std::chars_format::general,
            std::numeric_limits<f32>::max_digits10);
        if (converted.ec != std::errc{}) {
            Error = EBtGraphPersistenceError::InvalidNumber;
            return false;
        }
        return Append(buffer, static_cast<usize>(converted.ptr - buffer));
    }
};

static bool SeekGraphFileEnd(std::FILE* file) noexcept {
#if ACS_PLATFORM_WINDOWS
    return ::_fseeki64(file, 0, SEEK_END) == 0;
#else
    return std::fseek(file, 0, SEEK_END) == 0;
#endif
}

static i64 TellGraphFile(std::FILE* file) noexcept {
#if ACS_PLATFORM_WINDOWS
    return static_cast<i64>(::_ftelli64(file));
#else
    return static_cast<i64>(std::ftell(file));
#endif
}

static bool SeekGraphFileBegin(std::FILE* file) noexcept {
#if ACS_PLATFORM_WINDOWS
    return ::_fseeki64(file, 0, SEEK_SET) == 0;
#else
    return std::fseek(file, 0, SEEK_SET) == 0;
#endif
}

#if ACS_PLATFORM_WINDOWS
static volatile LONG g_BtGraphTemporarySerial = 0;
#endif

static FBtGraphPersistenceResult WriteGraphAtomically(
    const char* path, const char* bytes, usize byte_count) noexcept {
    constexpr u32 kOpenAttempts = 32u;
    constexpr usize kTemporaryPathBytes =
        ABehaviorTreeEditorPanel::kMaxGraphPathBytes + 96u;
    char temporary_path[kTemporaryPathBytes]{};

#if ACS_PLATFORM_WINDOWS
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD last_error = ERROR_FILE_EXISTS;
    for (u32 attempt = 0u; attempt < kOpenAttempts; ++attempt) {
        const u32 serial = static_cast<u32>(
            ::InterlockedIncrement(&g_BtGraphTemporarySerial));
        const int length = std::snprintf(
            temporary_path, sizeof(temporary_path), "%s.tmp.%08lX.%08lX.%08X",
            path, static_cast<unsigned long>(::GetCurrentProcessId()),
            static_cast<unsigned long>(::GetCurrentThreadId()),
            static_cast<unsigned int>(serial));
        if (length <= 0 || static_cast<usize>(length) >= sizeof(temporary_path)) {
            return GraphFailure(EBtGraphPersistenceError::PathTooLong);
        }
        file = ::CreateFileA(
            temporary_path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        last_error = ::GetLastError();
        if (last_error != ERROR_FILE_EXISTS &&
            last_error != ERROR_ALREADY_EXISTS) {
            return GraphFailure(
                EBtGraphPersistenceError::FileOpenFailed, 0u, 0u,
                static_cast<u32>(last_error));
        }
    }
    if (file == INVALID_HANDLE_VALUE) {
        return GraphFailure(
            EBtGraphPersistenceError::TemporaryFileExhausted, 0u, 0u,
            static_cast<u32>(last_error));
    }

    usize offset = 0u;
    while (offset < byte_count) {
        const usize remaining = byte_count - offset;
        const DWORD chunk = remaining > 0x7FFFFFFFu
            ? 0x7FFFFFFFu
            : static_cast<DWORD>(remaining);
        DWORD written = 0u;
        if (!::WriteFile(file, bytes + offset, chunk, &written, nullptr) ||
            written != chunk) {
            const DWORD error = ::GetLastError();
            ::CloseHandle(file);
            ::DeleteFileA(temporary_path);
            return GraphFailure(
                EBtGraphPersistenceError::FileWriteFailed, 0u,
                static_cast<u64>(offset), static_cast<u32>(error));
        }
        offset += written;
    }
    if (!::FlushFileBuffers(file)) {
        const DWORD error = ::GetLastError();
        ::CloseHandle(file);
        ::DeleteFileA(temporary_path);
        return GraphFailure(
            EBtGraphPersistenceError::FileFlushFailed, 0u,
            static_cast<u64>(offset), static_cast<u32>(error));
    }
    if (!::CloseHandle(file)) {
        const DWORD error = ::GetLastError();
        ::DeleteFileA(temporary_path);
        return GraphFailure(
            EBtGraphPersistenceError::FileCloseFailed, 0u,
            static_cast<u64>(offset), static_cast<u32>(error));
    }
    if (!::MoveFileExA(
            temporary_path, path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = ::GetLastError();
        ::DeleteFileA(temporary_path);
        return GraphFailure(
            EBtGraphPersistenceError::AtomicReplaceFailed, 0u,
            static_cast<u64>(offset), static_cast<u32>(error));
    }
#else
    const int length = std::snprintf(
        temporary_path, sizeof(temporary_path), "%s.tmp.%p",
        path, static_cast<const void*>(bytes));
    if (length <= 0 || static_cast<usize>(length) >= sizeof(temporary_path)) {
        return GraphFailure(EBtGraphPersistenceError::PathTooLong);
    }
    std::FILE* file = std::fopen(temporary_path, "wbx");
    if (file == nullptr) {
        return GraphFailure(EBtGraphPersistenceError::FileOpenFailed);
    }
    const usize written = std::fwrite(bytes, 1u, byte_count, file);
    if (written != byte_count) {
        std::fclose(file);
        std::remove(temporary_path);
        return GraphFailure(
            EBtGraphPersistenceError::FileWriteFailed, 0u,
            static_cast<u64>(written));
    }
    if (std::fflush(file) != 0) {
        std::fclose(file);
        std::remove(temporary_path);
        return GraphFailure(
            EBtGraphPersistenceError::FileFlushFailed, 0u,
            static_cast<u64>(written));
    }
    if (std::fclose(file) != 0) {
        std::remove(temporary_path);
        return GraphFailure(
            EBtGraphPersistenceError::FileCloseFailed, 0u,
            static_cast<u64>(written));
    }
    if (std::rename(temporary_path, path) != 0) {
        std::remove(temporary_path);
        return GraphFailure(
            EBtGraphPersistenceError::AtomicReplaceFailed, 0u,
            static_cast<u64>(written));
    }
#endif

    FBtGraphPersistenceResult result{};
    result.bytes = static_cast<u64>(byte_count);
    return result;
}

const char* FBtGraphPersistenceResult::ErrorName(
    EBtGraphPersistenceError value) noexcept {
    switch (value) {
        case EBtGraphPersistenceError::None: return "None";
        case EBtGraphPersistenceError::NullArgument: return "NullArgument";
        case EBtGraphPersistenceError::EmptyPath: return "EmptyPath";
        case EBtGraphPersistenceError::PathTooLong: return "PathTooLong";
        case EBtGraphPersistenceError::EmptyInput: return "EmptyInput";
        case EBtGraphPersistenceError::InputTooLarge: return "InputTooLarge";
        case EBtGraphPersistenceError::EmbeddedNul: return "EmbeddedNul";
        case EBtGraphPersistenceError::TooManyLines: return "TooManyLines";
        case EBtGraphPersistenceError::LineTooLong: return "LineTooLong";
        case EBtGraphPersistenceError::InvalidMagic: return "InvalidMagic";
        case EBtGraphPersistenceError::UnsupportedVersion: return "UnsupportedVersion";
        case EBtGraphPersistenceError::InvalidCount: return "InvalidCount";
        case EBtGraphPersistenceError::NodeCountLimit: return "NodeCountLimit";
        case EBtGraphPersistenceError::InvalidNodeRecord: return "InvalidNodeRecord";
        case EBtGraphPersistenceError::DuplicateNodeId: return "DuplicateNodeId";
        case EBtGraphPersistenceError::InvalidNodeId: return "InvalidNodeId";
        case EBtGraphPersistenceError::InvalidParentReference: return "InvalidParentReference";
        case EBtGraphPersistenceError::InvalidStructure: return "InvalidStructure";
        case EBtGraphPersistenceError::CycleDetected: return "CycleDetected";
        case EBtGraphPersistenceError::DepthLimitExceeded: return "DepthLimitExceeded";
        case EBtGraphPersistenceError::InvalidKind: return "InvalidKind";
        case EBtGraphPersistenceError::InvalidDecorator: return "InvalidDecorator";
        case EBtGraphPersistenceError::InvalidDecoratorMode: return "InvalidDecoratorMode";
        case EBtGraphPersistenceError::InvalidCompareOp: return "InvalidCompareOp";
        case EBtGraphPersistenceError::InvalidNumber: return "InvalidNumber";
        case EBtGraphPersistenceError::NonFiniteNumber: return "NonFiniteNumber";
        case EBtGraphPersistenceError::NameTooLong: return "NameTooLong";
        case EBtGraphPersistenceError::InvalidName: return "InvalidName";
        case EBtGraphPersistenceError::BlackboardCountLimit: return "BlackboardCountLimit";
        case EBtGraphPersistenceError::InvalidBlackboardRecord: return "InvalidBlackboardRecord";
        case EBtGraphPersistenceError::DuplicateBlackboardName: return "DuplicateBlackboardName";
        case EBtGraphPersistenceError::IntegerOutOfRange: return "IntegerOutOfRange";
        case EBtGraphPersistenceError::AllocationFailure: return "AllocationFailure";
        case EBtGraphPersistenceError::FileOpenFailed: return "FileOpenFailed";
        case EBtGraphPersistenceError::FileSizeFailed: return "FileSizeFailed";
        case EBtGraphPersistenceError::FileChanged: return "FileChanged";
        case EBtGraphPersistenceError::FileReadFailed: return "FileReadFailed";
        case EBtGraphPersistenceError::FileWriteFailed: return "FileWriteFailed";
        case EBtGraphPersistenceError::FileFlushFailed: return "FileFlushFailed";
        case EBtGraphPersistenceError::FileCloseFailed: return "FileCloseFailed";
        case EBtGraphPersistenceError::TemporaryFileExhausted: return "TemporaryFileExhausted";
        case EBtGraphPersistenceError::AtomicReplaceFailed: return "AtomicReplaceFailed";
    }
    return "Unknown";
}

/**
 * EBtStatus を RGBA float に変換する (header から ImVec4 を隠す目的)。
 *
 * @details
 * Success=緑 (0.2,1.0,0.3)、Failure=赤 (1.0,0.3,0.3)、Running=黄 (1.0,1.0,0.3) に固定。
 * 完全飽和ではなく落ち着いた色味にする (ダーク背景でも明るすぎず、ライト背景でも沈まないトーン)。
 */
void ABehaviorTreeEditorPanel::StatusColor(EBtStatus s, f32 out_rgba[4]) noexcept {
    if (out_rgba == nullptr) return;
    switch (s) {
        case EBtStatus::Success:
            out_rgba[0] = 0.2f; out_rgba[1] = 1.0f; out_rgba[2] = 0.3f; out_rgba[3] = 1.0f; return;
        case EBtStatus::Failure:
            out_rgba[0] = 1.0f; out_rgba[1] = 0.3f; out_rgba[2] = 0.3f; out_rgba[3] = 1.0f; return;
        case EBtStatus::Running:
            out_rgba[0] = 1.0f; out_rgba[1] = 1.0f; out_rgba[2] = 0.3f; out_rgba[3] = 1.0f; return;
    }
    out_rgba[0] = 0.7f; out_rgba[1] = 0.7f; out_rgba[2] = 0.7f; out_rgba[3] = 1.0f;
}

/** メタミラー / 履歴 / selection / step counter を初期化する (callback は維持)。 */
void ABehaviorTreeEditorPanel::Init() noexcept {
    // メタミラー / 履歴 / selection / step counter を全 reset。
    m_Nodes.Clear();
    m_History.Clear();
    m_History.Resize(static_cast<usize>(kHistorySize)); // 60 frame 分を確保
    for (usize i = 0; i < m_History.Size(); ++i) {
        m_History[i] = static_cast<u8>(EBtStatus::Failure); // 初期は Failure
    }
    m_HistoryHead = 0;

    m_Tree       = nullptr; // BT は SetTree で後付け
    m_Autorun    = false;
    m_StepCount = 0;
    m_Selected   = kInvalidId;
    // callback はリセットしない (Init は state の reset、callback は別操作)。
}

/** メタミラー / 履歴 / callback を全てクリアする (多重 Shutdown 可)。 */
void ABehaviorTreeEditorPanel::Shutdown() noexcept {
    // 多重 Shutdown 可。TArray は Clear() で要素破棄 + 容量保持。
    m_Nodes.Clear();
    m_History.Clear();
    m_HistoryHead = 0;

    m_Tree       = nullptr;
    m_Autorun    = false;
    m_StepCount = 0;
    m_Selected   = kInvalidId;

    // callback も解除する (Shutdown は完全リセットの意味合い)。
    m_StepCb    = nullptr;
    m_StepUser  = nullptr;
}

/** 観察対象の BT を差し替え、Reset() で実行状態を初期化する (メタミラーは維持)。 */
void ABehaviorTreeEditorPanel::SetTree(CBehaviorTree* tree) noexcept {
    m_Tree = tree;
    // 観察対象が変わったので step counter / 履歴 / 全 status を初期化する。
    // メタミラー (m_Nodes) は触らない (= 同構造で別インスタンスを観察する用途に
    // 対応するため、ユーザが ClearNodes を別途呼ばない限り維持する)。
    Reset();
}

/** step counter / history / 全 node の last_status を初期状態に戻す。 */
void ABehaviorTreeEditorPanel::Reset() noexcept {
    m_StepCount   = 0;
    m_HistoryHead = 0;
    for (usize i = 0; i < m_History.Size(); ++i) {
        m_History[i] = static_cast<u8>(EBtStatus::Failure);
    }
    for (usize i = 0; i < m_Nodes.Size(); ++i) {
        m_Nodes[i].last_status = EBtStatus::Failure;
    }
    // autorun / selection / メタミラーは触らない (= ユーザ操作で変える物)。
}

/** kStepDt (0.016 秒) 固定で 1 tick 進める。 */
void ABehaviorTreeEditorPanel::StepOnce() noexcept {
    // 0.016 sec (= 60 fps の 1 frame) を仕様で固定。Step は常に同じ dt で進めることで
    // ユーザが "今のステップ数 * 1/60 秒進んだ" と即座に解釈できるようにする。
    TickInternal(kStepDt);
}

/** 実 BT を dt で 1 tick 進め、root status を履歴に push、step counter を +1 する。 */
void ABehaviorTreeEditorPanel::TickInternal(f32 dt) noexcept {
    // レジストリ設定済みなら「グラフを直接インタプリト」して実行 (no-code 実行)。
    if (m_Registry != nullptr) { TickGraph(dt); return; }
    if (m_Tree == nullptr) return;

    // (1) 実 BT を 1 tick 進める
    EBtStatus root_status = EBtStatus::Failure;
    if (m_StepCb != nullptr) {
        // callback に委譲 (ユーザが任意 blackboard を渡せる)。
        // callback 側で `tree->Tick(my_bb, dt)` を呼ぶ規約。戻り値は取れないので、
        // root status は callback が SetNodeStatus(root_id=0, ...) で push する想定。
        // ここでは m_Nodes[0].last_status を採用する fallback (= 履歴 graph 用)。
        m_StepCb(m_StepUser, m_Tree, dt);
        if (!m_Nodes.IsEmpty()) {
            root_status = m_Nodes[0].last_status;
        }
    } else {
        // fallback: blackboard = nullptr で直接呼ぶ。
        // 戻り値を取れるのでそのまま履歴に push する。
        root_status = m_Tree->Tick(nullptr, dt);
        if (!m_Nodes.IsEmpty()) {
            // root の last_status も同期的に更新 (Inspector / TreeView 表示と
            // PlotLines の値を一致させるため)。
            m_Nodes[0].last_status = root_status;
        }
    }

    // (2) 履歴 ring buffer に push
    if (!m_History.IsEmpty()) {
        m_History[m_HistoryHead] = static_cast<u8>(root_status);
        m_HistoryHead = (m_HistoryHead + 1u) % kHistorySize;
    }

    // (3) step counter
    ++m_StepCount;
}

/** autorun が ON のときだけ実 dt で 1 tick 進める (0 dt は無視)。 */
void ABehaviorTreeEditorPanel::OnFrameBegin(f32 dt) noexcept {
    // autorun ON のときだけ毎フレーム 1 tick 進める。実 dt を渡すことで
    // ゲームの fps に追従させる (Step は 0.016 固定だが autorun は別)。
    if (!m_Autorun) return;
    if (m_Tree == nullptr && m_Registry == nullptr) return;  // 実行対象なし
    if (dt <= 0.0f) return; // 0 dt スパイクで何もしない (= 一時停止と同じ)
    TickInternal(dt);
}

/** 選択 node を設定する (範囲外 / kInvalidId は「未選択」に正規化)。 */
void ABehaviorTreeEditorPanel::SelectNode(u32 node_id) noexcept {
    if (!IsValidId(node_id, m_Nodes.Size())) {
        m_Selected = kInvalidId;
        return;
    }
    m_Selected = node_id;
}

/** 新規 node をメタミラーに追加し、払い出した id を返す (上限到達は kInvalidId)。 */
u32 ABehaviorTreeEditorPanel::AddNode(EBtKind kind, const char* name, u32 parent_id) noexcept {
    if (m_Nodes.Size() >= static_cast<usize>(kMaxNodes)) {
        // 上限到達は silent fail (= kInvalidId 返却で通知)。
        return kInvalidId;
    }

    // parent_id バリデーション: kInvalidId 以外で範囲外なら root 扱いに倒す。
    // (= 不正な parent でも panel が落ちないようにする防衛策)
    if (parent_id != kInvalidId && !IsValidId(parent_id, m_Nodes.Size())) {
        parent_id = kInvalidId;
    }

    FNodeMeta n;
    n.id          = static_cast<u32>(m_Nodes.Size()); // 払い出し = 現在の末尾 index
    n.parent_id   = parent_id;
    n.kind        = kind;
    n.name        = name;
    n.last_status = EBtStatus::Failure; // 初期は Failure

    const u32 new_id = n.id;
    m_Nodes.PushBack(n);
    m_DidLayout = false;   // 構成が変わったので次フレームで再レイアウト
    return new_id;
}

/** 指定 node の last_status を更新する (範囲外は no-op)。 */
void ABehaviorTreeEditorPanel::SetNodeStatus(u32 node_id, EBtStatus status) noexcept {
    if (!IsValidId(node_id, m_Nodes.Size())) return;
    m_Nodes[static_cast<usize>(node_id)].last_status = status;
}

/** Decorator node の変換 op を設定する (範囲外 / Decorator 以外は no-op で false)。 */
bool ABehaviorTreeEditorPanel::SetNodeDecoratorOp(u32 node_id, EBtDecoratorOp op) noexcept {
    if (!IsValidId(node_id, m_Nodes.Size())) return false;
    FNodeMeta& n = m_Nodes[static_cast<usize>(node_id)];
    if (n.kind != EBtKind::Decorator) return false;
    n.deco = op;
    return true;
}

/** Decorator node の動作モードを設定する (範囲外 / Decorator 以外は no-op で false)。 */
bool ABehaviorTreeEditorPanel::SetNodeDecoratorMode(u32 node_id, EBtDecoMode mode) noexcept {
    if (!IsValidId(node_id, m_Nodes.Size())) return false;
    FNodeMeta& n = m_Nodes[static_cast<usize>(node_id)];
    if (n.kind != EBtKind::Decorator) return false;
    n.decoMode = mode;
    return true;
}

/** Decorator node を Compare モードにし、比較条件 (var op rhs) を設定する。 */
bool ABehaviorTreeEditorPanel::SetNodeCompare(u32 node_id, const char* var, EBtCompareOp op, f32 rhs) noexcept {
    if (!IsValidId(node_id, m_Nodes.Size())) return false;
    FNodeMeta& n = m_Nodes[static_cast<usize>(node_id)];
    if (n.kind != EBtKind::Decorator) return false;
    n.decoMode = EBtDecoMode::Compare;
    std::snprintf(n.var, sizeof(n.var), "%s", (var != nullptr) ? var : "");
    n.cmpOp  = op;
    n.cmpRhs = rhs;
    return true;
}

/** メタミラーを全削除し selection を解除する (history / step counter / autorun は維持)。 */
void ABehaviorTreeEditorPanel::ClearNodes() noexcept {
    m_Nodes.Clear();
    m_Selected = kInvalidId;
    m_DragNode = kInvalidId;
    m_LinkSrc  = kInvalidId;
    m_DidLayout = false;
    // history / step counter / autorun は触らない (= ユーザの明示操作で変える物)。
}

/** 指定 id の node の last_status を返す (範囲外は Failure)。 */
EBtStatus ABehaviorTreeEditorPanel::NodeStatus(u32 node_id) const noexcept {
    if (!IsValidId(node_id, m_Nodes.Size())) return EBtStatus::Failure;
    return m_Nodes[static_cast<usize>(node_id)].last_status;
}

/** step tick 用の callback とユーザポインタを登録する (nullptr で解除)。 */
void ABehaviorTreeEditorPanel::SetOnStepCallback(StepCallback cb, void* user) noexcept {
    m_StepCb   = cb;
    m_StepUser = user;
}

/**
 * 1 node を TreeNode で描画し、子を線形走査して再帰展開する。
 *
 * @details
 * parent_id が node_id を指す子 node を m_Nodes 内で線形に探して再帰描画する。
 * Action (= leaf) は ImGuiTreeNodeFlags_Leaf を付ける。ImGuiTreeNodeFlags_OpenOnArrow で
 * 「矢印クリックで展開、ラベルクリックで選択」にする (Unity Hierarchy と同形)。
 */
void ABehaviorTreeEditorPanel::DrawTreeRecursive(u32 node_id, u32 depth) noexcept {
    if (depth >= kTreeRecursionLimit) {
        ImGui::TextDisabled("  (tree depth limit reached)");
        return;
    }
    if (!IsValidId(node_id, m_Nodes.Size())) return;

    const FNodeMeta& node = m_Nodes[static_cast<usize>(node_id)];
    if (!node.alive) return;

    // 子検索 (= 線形走査で parent_id == node_id の生存要素があるか)。
    bool has_child = false;
    if (!BtKindIsLeaf(node.kind)) {
        // Action / Task は leaf。Selector / Sequence / Decorator のみ子を持ち得る。
        for (usize i = 0; i < m_Nodes.Size(); ++i) {
            if (m_Nodes[i].alive && m_Nodes[i].parent_id == node_id) {
                has_child = true;
                break;
            }
        }
    }

    // TreeNode flag 設定。
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                             | ImGuiTreeNodeFlags_OpenOnDoubleClick
                             | ImGuiTreeNodeFlags_SpanAvailWidth
                             | ImGuiTreeNodeFlags_DefaultOpen;
    if (!has_child) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (m_Selected == node_id) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    // ラベル整形: "[Kind] name  ● Status" (kind と status を視認しやすく)。
    char label[160];
    std::snprintf(label, sizeof(label),
                  "[%s] %s",
                  KindLabel(node.kind),
                  DisplayName(node));

    // ID 衝突回避のため node_id で PushID。
    ImGui::PushID(static_cast<int>(node_id));

    // ラベル全体を status color で描画する (緑/赤/黄)。
    f32 col[4];
    StatusColor(node.last_status, col);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(col[0], col[1], col[2], col[3]));

    const bool open = ImGui::TreeNodeEx("##bt_tn", flags, "%s", label);

    ImGui::PopStyleColor();

    // ラベルクリックで選択 (TreeNode の click 領域に対する反応を捕捉)。
    // arrow click は TreeNode が消費するので、それ以外の click を selection に。
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        m_Selected = node_id;
    }

    // 同行右側に status バッジを SameLine 表示 ([Success/Failure/Running])。
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(col[0], col[1], col[2], col[3]),
                       "[%s]", StatusLabel(node.last_status));

    if (open && has_child) {
        for (usize i = 0; i < m_Nodes.Size(); ++i) {
            if (m_Nodes[i].alive && m_Nodes[i].parent_id == node_id) {
                DrawTreeRecursive(static_cast<u32>(i), depth + 1u);
            }
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

/** node の表示名 (エディタ名 ename を優先、無ければ name)。 */
const char* ABehaviorTreeEditorPanel::DisplayName(const FNodeMeta& n) const noexcept {
    if (n.ename[0] != '\0') return n.ename;
    return SafeName(n.name);
}

/** maybe_ancestor が node の祖先 (自分自身含む) かを返す。 */
bool ABehaviorTreeEditorPanel::IsAncestor(u32 maybe_ancestor, u32 node) const noexcept {
    u32 cur = node;
    for (u32 guard = 0; guard < kTreeRecursionLimit; ++guard) {
        if (cur == maybe_ancestor) return true;
        if (!IsValidId(cur, m_Nodes.Size())) break;
        cur = m_Nodes[static_cast<usize>(cur)].parent_id;
        if (cur == kInvalidId) break;
    }
    return false;
}

namespace {
constexpr f32 kColW = 172.0f;   // 兄弟間の横間隔 (world)
constexpr f32 kRowH = 98.0f;    // 階層間の縦間隔 (world)
}

/** tree 構造から x,y を自動配置する (Reingold-Tilford 風の簡易 tidy layout)。 */
void ABehaviorTreeEditorPanel::AutoLayout() noexcept {
    // 再帰は関数ポインタにできないので明示スタックではなく素朴な再帰ヘルパをラムダ無しで。
    // m_Nodes を直接触る member 再帰を below の static 関数に委譲する代わり、ここで完結させる。
    f32 next_leaf = 0.0f;
    // 再帰ヘルパ (自己呼び出しのため struct local function)
    struct FRec {
        TArray<FNodeMeta>& nodes;
        f32& next_leaf;
        f32 run(u32 id, u32 depth, u32 guard) noexcept {
            if (guard >= kTreeRecursionLimit || !(static_cast<usize>(id) < nodes.Size())) {
                const f32 gx = next_leaf; next_leaf += 1.0f; return gx;
            }
            f32 sum = 0.0f; u32 cnt = 0;
            for (usize i = 0; i < nodes.Size(); ++i) {
                if (nodes[i].alive && nodes[i].parent_id == id) {
                    sum += run(static_cast<u32>(i), depth + 1u, guard + 1u);
                    ++cnt;
                }
            }
            f32 gx;
            if (cnt == 0) { gx = next_leaf; next_leaf += 1.0f; }
            else          { gx = sum / static_cast<f32>(cnt); }
            nodes[static_cast<usize>(id)].x = gx * kColW;
            nodes[static_cast<usize>(id)].y = static_cast<f32>(depth) * kRowH;
            return gx;
        }
    } rec{ m_Nodes, next_leaf };

    for (usize i = 0; i < m_Nodes.Size(); ++i) {
        if (m_Nodes[i].alive && m_Nodes[i].parent_id == kInvalidId) {
            rec.run(static_cast<u32>(i), 0u, 0u);
        }
    }
}

/** エディタからノードを追加する (グラフ上に配置、再レイアウトはしない)。 */
u32 ABehaviorTreeEditorPanel::AddNodeGraph(EBtKind kind, u32 parent_id, f32 wx, f32 wy) noexcept {
    if (m_Nodes.Size() >= static_cast<usize>(kMaxNodes)) return kInvalidId;
    if (parent_id != kInvalidId && !IsValidId(parent_id, m_Nodes.Size())) parent_id = kInvalidId;

    // Decorator は単子: 既存の子があれば root へ外してから新しい子を付ける (= 置換)。
    // これで「Add child を Decorator に複数回 → 余分な子が黙って無視される」を防ぐ。
    if (parent_id != kInvalidId && m_Nodes[static_cast<usize>(parent_id)].kind == EBtKind::Decorator) {
        for (usize i = 0; i < m_Nodes.Size(); ++i) {
            if (m_Nodes[i].alive && m_Nodes[i].parent_id == parent_id) {
                m_Nodes[i].parent_id = kInvalidId;
            }
        }
    }

    FNodeMeta n;
    n.id          = static_cast<u32>(m_Nodes.Size());
    n.parent_id   = parent_id;
    n.kind        = kind;
    n.name        = nullptr;
    n.x = wx; n.y = wy;
    n.alive = true;
    n.last_status = EBtStatus::Failure;
    std::snprintf(n.ename, sizeof(n.ename), "%s %u", KindLabel(kind), static_cast<unsigned>(n.id));

    const u32 new_id = n.id;
    m_Nodes.PushBack(n);
    m_Selected = new_id;
    // 明示配置なので m_DidLayout は変えない (= 既存ノードの位置を保つ)。
    return new_id;
}

/** ノードを tombstone 削除し、子を祖父へ付け替える。 */
void ABehaviorTreeEditorPanel::DeleteNodeGraph(u32 id) noexcept {
    if (!IsValidId(id, m_Nodes.Size())) return;
    const u32 gp = m_Nodes[static_cast<usize>(id)].parent_id;
    for (usize i = 0; i < m_Nodes.Size(); ++i) {
        if (m_Nodes[i].parent_id == id) m_Nodes[i].parent_id = gp;
    }
    m_Nodes[static_cast<usize>(id)].alive     = false;
    m_Nodes[static_cast<usize>(id)].parent_id = kInvalidId;
    if (m_Selected == id) m_Selected = kInvalidId;
    if (m_DragNode == id) m_DragNode = kInvalidId;
    if (m_CtxNode  == id) m_CtxNode  = kInvalidId;
}

/** id の生存子を x 座標 (左→右) 順に集める。 */
u32 ABehaviorTreeEditorPanel::CollectChildrenSorted(u32 id, u32* out, u32 cap) const noexcept {
    u32 cnt = 0;
    for (usize i = 0; i < m_Nodes.Size(); ++i) {
        if (!m_Nodes[i].alive || m_Nodes[i].parent_id != id) continue;
        if (cnt >= cap) break;
        // x 昇順に挿入 (見た目の左→右をそのまま評価順にする)
        u32 pos = cnt;
        while (pos > 0 && m_Nodes[static_cast<usize>(out[pos - 1])].x > m_Nodes[i].x) {
            out[pos] = out[pos - 1]; --pos;
        }
        out[pos] = static_cast<u32>(i);
        ++cnt;
    }
    return cnt;
}

/** グラフ 1 ノードを再帰インタプリト (selector/sequence/action 意味論)。 */
EBtStatus ABehaviorTreeEditorPanel::TickGraphNode(u32 id, f32 dt, u32 guard) noexcept {
    if (guard >= kTreeRecursionLimit || !IsValidId(id, m_Nodes.Size())) return EBtStatus::Failure;
    if (!m_Nodes[static_cast<usize>(id)].alive) return EBtStatus::Failure;

    m_Nodes[static_cast<usize>(id)].visit_order = ++m_VisitSeq;   // 実行フロー記録
    const EBtKind kind = m_Nodes[static_cast<usize>(id)].kind;

    EBtStatus result;
    if (BtKindIsLeaf(kind)) {
        // Action / Task はともにレジストリ解決した関数を呼ぶ末端 leaf (種別は表示上の区別)。
        CBtActionRegistry::Fn fn = (m_Registry != nullptr)
            ? m_Registry->Find(DisplayName(m_Nodes[static_cast<usize>(id)])) : nullptr;
        result = (fn != nullptr) ? fn(m_GraphBb, dt) : EBtStatus::Failure;  // 未解決は Failure
    } else if (kind == EBtKind::Decorator) {
        // Decorator は子 1 つ (左端 = 最初の子)。モードで Transform / 条件ガードを切替。
        FNodeMeta& dn = m_Nodes[static_cast<usize>(id)];
        u32 kids[kMaxNodes];
        const u32 ck = CollectChildrenSorted(id, kids, kMaxNodes);

        if (dn.decoMode == EBtDecoMode::Transform) {
            // 子を評価し、deco op で変換 (runtime ApplyDecorator と同一実装)。
            if (ck == 0) result = EBtStatus::Failure;          // 装飾対象なし → ソフトフェイル
            else         result = ApplyDecorator(dn.deco, TickGraphNode(kids[0], dt, guard + 1u));
        } else {
            // 条件ガード: 条件 true のときだけ子を実行し、その結果を返す。false は子をスキップして Failure。
            bool pass = false;
            if (dn.decoMode == EBtDecoMode::Condition) {
                CBtConditionRegistry::Fn cf =
                    (m_CondReg != nullptr) ? m_CondReg->Find(DisplayName(dn)) : nullptr;
                pass = (cf != nullptr) ? cf(m_GraphBb) : false; // 未解決は false (ガードで止まる)
            } else { // EBtDecoMode::Compare
                // 変数解決は動的ブラックボード (名前) を優先し、無ければ offset スキーマ。
                if (m_DynBb != nullptr && m_DynBb->Has(dn.var)) {
                    pass = BtCompareF32(m_DynBb->GetAsF32(dn.var), dn.cmpOp, dn.cmpRhs);
                } else if (m_Schema != nullptr) {
                    const u32 vi = m_Schema->IndexOf(dn.var);
                    if (vi != FBtBlackboardSchema::kInvalid) {
                        pass = BtCompareVar(m_GraphBb, m_Schema->OffsetAt(vi),
                                            m_Schema->TypeAt(vi), dn.cmpOp, dn.cmpRhs);
                    }
                }
            }
            result = (pass && ck > 0) ? TickGraphNode(kids[0], dt, guard + 1u) : EBtStatus::Failure;
        }
    } else {
        u32 kids[kMaxNodes];
        const u32 ck = CollectChildrenSorted(id, kids, kMaxNodes);
        // 子なし: Selector=Failure / Sequence=Success (ランタイム FBt* と同規約)
        result = (kind == EBtKind::Selector) ? EBtStatus::Failure : EBtStatus::Success;
        for (u32 k = 0; k < ck; ++k) {
            const EBtStatus s = TickGraphNode(kids[k], dt, guard + 1u);
            if (kind == EBtKind::Selector) {
                if (s == EBtStatus::Running) { result = EBtStatus::Running; break; }
                if (s == EBtStatus::Success) { result = EBtStatus::Success; break; }
                // Failure → 次の子へ
            } else { // Sequence
                if (s == EBtStatus::Running) { result = EBtStatus::Running; break; }
                if (s == EBtStatus::Failure) { result = EBtStatus::Failure; break; }
                // Success → 次の子へ
            }
        }
    }
    m_Nodes[static_cast<usize>(id)].last_status = result;
    return result;
}

/** メタミラーのグラフを 1 tick 直接インタプリトする。 */
EBtStatus ABehaviorTreeEditorPanel::TickGraph(f32 dt) noexcept {
    m_VisitSeq = 0u;
    for (usize i = 0; i < m_Nodes.Size(); ++i) m_Nodes[i].visit_order = 0u;  // フロークリア

    EBtStatus root_status = EBtStatus::Failure;
    for (usize i = 0; i < m_Nodes.Size(); ++i) {
        if (m_Nodes[i].alive && m_Nodes[i].parent_id == kInvalidId) {
            root_status = TickGraphNode(static_cast<u32>(i), dt, 0u);
            break;   // root は 1 つ想定 (forest の場合は先頭のみ駆動)
        }
    }
    if (!m_History.IsEmpty()) {
        m_History[m_HistoryHead] = static_cast<u8>(root_status);
        m_HistoryHead = (m_HistoryHead + 1u) % kHistorySize;
    }
    ++m_StepCount;
    return root_status;
}

/** グラフをテキストファイルへ保存する (生存ノードを id 圧縮して書く)。 */
bool ABehaviorTreeEditorPanel::SaveGraph(const char* path) const noexcept {
    return TrySaveGraph(path).Succeeded();
}

FBtGraphPersistenceResult ABehaviorTreeEditorPanel::TrySaveGraph(
    const char* path) const noexcept {
    if (path == nullptr) {
        return GraphFailure(EBtGraphPersistenceError::NullArgument);
    }
    usize path_length = 0u;
    if (!TryBoundedCStringLength(path, kMaxGraphPathBytes, path_length)) {
        return GraphFailure(EBtGraphPersistenceError::PathTooLong);
    }
    if (path_length == 0u) {
        return GraphFailure(EBtGraphPersistenceError::EmptyPath);
    }
    if (m_Nodes.Size() > static_cast<usize>(kMaxNodes)) {
        return GraphFailure(EBtGraphPersistenceError::NodeCountLimit);
    }

    u32 remap[kMaxNodes]{};
    for (u32 i = 0u; i < kMaxNodes; ++i) remap[i] = kInvalidId;
    u32 node_count = 0u;
    for (usize i = 0u; i < m_Nodes.Size(); ++i) {
        if (m_Nodes[i].alive) remap[i] = node_count++;
    }

    for (usize i = 0u; i < m_Nodes.Size(); ++i) {
        const FNodeMeta& node = m_Nodes[i];
        if (!node.alive) continue;

        const u32 kind = static_cast<u32>(node.kind);
        const u32 decorator = static_cast<u32>(node.deco);
        const u32 mode = static_cast<u32>(node.decoMode);
        const u32 compare = static_cast<u32>(node.cmpOp);
        if (kind > static_cast<u32>(EBtKind::Task)) {
            return GraphFailure(EBtGraphPersistenceError::InvalidKind);
        }
        if (decorator > static_cast<u32>(EBtDecoratorOp::Repeat)) {
            return GraphFailure(EBtGraphPersistenceError::InvalidDecorator);
        }
        if (mode > static_cast<u32>(EBtDecoMode::Compare)) {
            return GraphFailure(EBtGraphPersistenceError::InvalidDecoratorMode);
        }
        if (compare > static_cast<u32>(EBtCompareOp::Greater)) {
            return GraphFailure(EBtGraphPersistenceError::InvalidCompareOp);
        }
        if (!std::isfinite(node.cmpRhs) ||
            !std::isfinite(node.x) || !std::isfinite(node.y)) {
            return GraphFailure(EBtGraphPersistenceError::NonFiniteNumber);
        }

        usize variable_length = 0u;
        if (!TryBoundedCStringLength(
                node.var, sizeof(node.var) - 1u, variable_length)) {
            return GraphFailure(EBtGraphPersistenceError::NameTooLong);
        }
        if (variable_length != 0u &&
            !IsSafeToken(node.var, variable_length)) {
            return GraphFailure(EBtGraphPersistenceError::InvalidName);
        }

        const char* display_name = DisplayName(node);
        usize name_length = 0u;
        if (!TryBoundedCStringLength(
                display_name, sizeof(node.ename) - 1u, name_length)) {
            return GraphFailure(EBtGraphPersistenceError::NameTooLong);
        }
        if (!IsSafeDisplayName(display_name, name_length)) {
            return GraphFailure(EBtGraphPersistenceError::InvalidName);
        }

        if (node.parent_id != kInvalidId) {
            if (!IsValidId(node.parent_id, m_Nodes.Size()) ||
                !m_Nodes[static_cast<usize>(node.parent_id)].alive) {
                return GraphFailure(
                    EBtGraphPersistenceError::InvalidParentReference);
            }
        }

        u32 child_count = 0u;
        for (usize child = 0u; child < m_Nodes.Size(); ++child) {
            if (m_Nodes[child].alive &&
                m_Nodes[child].parent_id == static_cast<u32>(i)) {
                ++child_count;
            }
        }
        if ((BtKindIsLeaf(node.kind) && child_count != 0u) ||
            (node.kind == EBtKind::Decorator && child_count > 1u)) {
            return GraphFailure(EBtGraphPersistenceError::InvalidStructure);
        }

        bool visited[kMaxNodes]{};
        u32 current = static_cast<u32>(i);
        u32 depth = 0u;
        while (current != kInvalidId) {
            if (!IsValidId(current, m_Nodes.Size()) ||
                !m_Nodes[static_cast<usize>(current)].alive) {
                return GraphFailure(
                    EBtGraphPersistenceError::InvalidParentReference);
            }
            if (visited[current]) {
                return GraphFailure(EBtGraphPersistenceError::CycleDetected);
            }
            if (depth >= kMaxGraphDepth) {
                return GraphFailure(
                    EBtGraphPersistenceError::DepthLimitExceeded);
            }
            visited[current] = true;
            ++depth;
            current = m_Nodes[static_cast<usize>(current)].parent_id;
        }
    }

    const u32 blackboard_count =
        m_DynBb != nullptr ? m_DynBb->Count() : 0u;
    if (blackboard_count > FBtBlackboard::kMax) {
        return GraphFailure(
            EBtGraphPersistenceError::BlackboardCountLimit);
    }
    for (u32 i = 0u; i < blackboard_count; ++i) {
        const char* name = m_DynBb->NameAt(i);
        const u32 type_value = static_cast<u32>(m_DynBb->TypeAt(i));
        if (type_value > static_cast<u32>(EBtVarType::F32)) {
            return GraphFailure(
                EBtGraphPersistenceError::InvalidBlackboardRecord);
        }
        usize name_length = 0u;
        if (!TryBoundedCStringLength(
                name, FBtBlackboard::kNameLen - 1u, name_length)) {
            return GraphFailure(EBtGraphPersistenceError::NameTooLong);
        }
        if (!IsSafeToken(name, name_length)) {
            return GraphFailure(EBtGraphPersistenceError::InvalidName);
        }
        for (u32 previous = 0u; previous < i; ++previous) {
            if (std::strcmp(name, m_DynBb->NameAt(previous)) == 0) {
                return GraphFailure(
                    EBtGraphPersistenceError::DuplicateBlackboardName);
            }
        }
        if (m_DynBb->TypeAt(i) == EBtVarType::F32 &&
            !std::isfinite(m_DynBb->GetF32(name))) {
            return GraphFailure(EBtGraphPersistenceError::NonFiniteNumber);
        }
    }

    FGraphTextBuilder builder;
    if (!builder.Text.TryReserve(4096u)) {
        return GraphFailure(EBtGraphPersistenceError::AllocationFailure);
    }
    builder.AppendCString("ACSBT 4\n");
    builder.AppendU32(node_count);
    builder.AppendChar('\n');
    for (usize i = 0u; i < m_Nodes.Size(); ++i) {
        const FNodeMeta& node = m_Nodes[i];
        if (!node.alive) continue;
        builder.AppendU32(remap[i]);
        builder.AppendChar(' ');
        builder.AppendI32(
            node.parent_id == kInvalidId
                ? -1
                : static_cast<i32>(remap[node.parent_id]));
        builder.AppendChar(' ');
        builder.AppendU32(static_cast<u32>(node.kind));
        builder.AppendChar(' ');
        builder.AppendU32(static_cast<u32>(node.deco));
        builder.AppendChar(' ');
        builder.AppendU32(static_cast<u32>(node.decoMode));
        builder.AppendChar(' ');
        builder.AppendU32(static_cast<u32>(node.cmpOp));
        builder.AppendChar(' ');
        builder.AppendF32(node.cmpRhs);
        builder.AppendChar(' ');
        builder.AppendF32(node.x);
        builder.AppendChar(' ');
        builder.AppendF32(node.y);
        builder.AppendChar(' ');
        builder.AppendCString(node.var[0] == '\0' ? "-" : node.var);
        builder.AppendChar(' ');
        builder.AppendCString(DisplayName(node));
        builder.AppendChar('\n');
    }
    builder.AppendCString("BB ");
    builder.AppendU32(blackboard_count);
    builder.AppendChar('\n');
    for (u32 i = 0u; i < blackboard_count; ++i) {
        const char* name = m_DynBb->NameAt(i);
        const EBtVarType type = m_DynBb->TypeAt(i);
        builder.AppendCString(name);
        builder.AppendChar(' ');
        builder.AppendU32(static_cast<u32>(type));
        builder.AppendChar(' ');
        switch (type) {
            case EBtVarType::Bool:
                builder.AppendU32(m_DynBb->GetBool(name) ? 1u : 0u);
                break;
            case EBtVarType::I32:
                builder.AppendI32(m_DynBb->GetI32(name));
                break;
            case EBtVarType::F32:
                builder.AppendF32(m_DynBb->GetF32(name));
                break;
        }
        builder.AppendChar('\n');
    }
    if (builder.Error != EBtGraphPersistenceError::None) {
        return GraphFailure(builder.Error);
    }
    return WriteGraphAtomically(
        path, builder.Text.Data(), builder.Text.Size());
}

/** メタミラー 1 ノードを実行可能な ABtNode へ再帰変換する。 */
TUniquePtr<ABtNode> ABehaviorTreeEditorPanel::BuildRuntimeNode(u32 id, u32 guard) const noexcept {
    if (guard >= kTreeRecursionLimit || !IsValidId(id, m_Nodes.Size())) return TUniquePtr<ABtNode>();
    const FNodeMeta& n = m_Nodes[static_cast<usize>(id)];
    if (!n.alive) return TUniquePtr<ABtNode>();

    u32 kids[kMaxNodes];
    const u32 ck = CollectChildrenSorted(id, kids, kMaxNodes);

    switch (n.kind) {
        case EBtKind::Selector: {
            auto s = MakeUnique<ABtSelector>();
            for (u32 k = 0; k < ck; ++k) s->AddChild(BuildRuntimeNode(kids[k], guard + 1u));
            return s;   // TUniquePtr<ABtSelector> → TUniquePtr<ABtNode> (upcast)
        }
        case EBtKind::Sequence: {
            auto s = MakeUnique<ABtSequence>();
            for (u32 k = 0; k < ck; ++k) s->AddChild(BuildRuntimeNode(kids[k], guard + 1u));
            return s;
        }
        case EBtKind::Decorator: {
            TUniquePtr<ABtNode> child = (ck > 0) ? BuildRuntimeNode(kids[0], guard + 1u)
                                                 : TUniquePtr<ABtNode>();
            if (n.decoMode == EBtDecoMode::Transform) {
                auto d = MakeUnique<ABtDecorator>(n.deco);
                d->SetChild(Move(child));
                return d;
            } else if (n.decoMode == EBtDecoMode::Condition) {
                ABtConditionNode::Fn cf = (m_CondReg != nullptr) ? m_CondReg->Find(DisplayName(n)) : nullptr;
                auto d = MakeUnique<ABtConditionNode>(cf);
                d->SetChild(Move(child));
                return d;
            } else { // EBtDecoMode::Compare
                // 変数解決はインタプリタと同順: 動的 BB (名前) を優先、無ければ offset スキーマ。
                TUniquePtr<ABtCompareNode> d;
                if (m_DynBb != nullptr && m_DynBb->Has(n.var)) {
                    d = MakeUnique<ABtCompareNode>(n.var, n.cmpOp, n.cmpRhs);          // dynamic
                } else if (m_Schema != nullptr) {
                    const u32 vi = m_Schema->IndexOf(n.var);
                    if (vi != FBtBlackboardSchema::kInvalid) {
                        d = MakeUnique<ABtCompareNode>(m_Schema->OffsetAt(vi), m_Schema->TypeAt(vi),
                                                       n.cmpOp, n.cmpRhs);             // schema
                    }
                }
                if (!d) d = MakeUnique<ABtCompareNode>(n.var, n.cmpOp, n.cmpRhs);      // 未解決→dynamic名 (Failure)
                d->SetChild(Move(child));
                return d;
            }
        }
        case EBtKind::Action:
        case EBtKind::Task: {
            CBtActionRegistry::Fn fn = (m_Registry != nullptr) ? m_Registry->Find(DisplayName(n)) : nullptr;
            return MakeUnique<ABtAction>(fn);   // fn 未解決でも ABtAction が Failure を返す
        }
    }
    return TUniquePtr<ABtNode>(); // 到達不能
}

/** 現在のグラフを実行可能な CBehaviorTree ノードツリーへ bake する。 */
TUniquePtr<ABtNode> ABehaviorTreeEditorPanel::BuildRuntimeTree() const noexcept {
    for (usize i = 0; i < m_Nodes.Size(); ++i) {
        if (m_Nodes[i].alive && m_Nodes[i].parent_id == kInvalidId) {
            return BuildRuntimeNode(static_cast<u32>(i), 0u);   // root は先頭の親なしノード
        }
    }
    return TUniquePtr<ABtNode>();
}

/** テキストファイルからグラフを読み込む (既存はクリア)。 */
bool ABehaviorTreeEditorPanel::LoadGraph(const char* path) noexcept {
    return TryLoadGraph(path).Succeeded();
}

#if 0
bool ABehaviorTreeEditorPanel::LoadGraphLegacy(const char* path) noexcept {
    if (path == nullptr) return false;
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;

    char line[256];
    int ver = 1;
    if (std::fgets(line, sizeof(line), f) == nullptr || std::strncmp(line, "ACSBT", 5) != 0) {
        std::fclose(f); return false;
    }
    std::sscanf(line, "ACSBT %d", &ver);   // version (>=2 なら deco 列あり)
    int count = 0;
    if (std::fgets(line, sizeof(line), f) == nullptr || std::sscanf(line, "%d", &count) != 1) {
        std::fclose(f); return false;
    }

    ClearNodes();
    for (int k = 0; k < count; ++k) {
        if (std::fgets(line, sizeof(line), f) == nullptr) break;
        int id = 0, parent = -1, kind = 2, deco = 0, decoMode = 0, cmpOp = 0, off = 0;
        float x = 0.0f, y = 0.0f, cmpRhs = 0.0f;
        char varbuf[48] = "-";
        if (ver >= 3) {
            if (std::sscanf(line, "%d %d %d %d %d %d %f %f %f %47s %n",
                            &id, &parent, &kind, &deco, &decoMode, &cmpOp,
                            &cmpRhs, &x, &y, varbuf, &off) < 10) continue;
        } else if (ver == 2) {
            if (std::sscanf(line, "%d %d %d %d %f %f %n", &id, &parent, &kind, &deco, &x, &y, &off) < 6) continue;
        } else {
            if (std::sscanf(line, "%d %d %d %f %f %n", &id, &parent, &kind, &x, &y, &off) < 5) continue;
        }
        // 名前は残り全部 (空白を含み得る)。改行を除去。
        const char* nm = line + off;
        char namebuf[48];
        std::snprintf(namebuf, sizeof(namebuf), "%s", nm);
        for (u32 c = 0; c < sizeof(namebuf); ++c) {
            if (namebuf[c] == '\n' || namebuf[c] == '\r') { namebuf[c] = '\0'; break; }
        }
        if (m_Nodes.Size() >= static_cast<usize>(kMaxNodes)) break;
        FNodeMeta n;
        n.id          = static_cast<u32>(m_Nodes.Size());
        n.parent_id   = (parent < 0) ? kInvalidId : static_cast<u32>(parent);
        n.kind        = (kind == 0) ? EBtKind::Selector
                      : (kind == 1) ? EBtKind::Sequence
                      : (kind == 3) ? EBtKind::Decorator
                      : (kind == 4) ? EBtKind::Task
                      :               EBtKind::Action;
        n.deco        = (deco == 1) ? EBtDecoratorOp::ForceSuccess
                      : (deco == 2) ? EBtDecoratorOp::ForceFailure
                      : (deco == 3) ? EBtDecoratorOp::Repeat
                      :               EBtDecoratorOp::Inverter;
        n.decoMode    = (decoMode == 1) ? EBtDecoMode::Condition
                      : (decoMode == 2) ? EBtDecoMode::Compare
                      :                   EBtDecoMode::Transform;
        n.cmpOp       = (cmpOp >= 0 && cmpOp <= 5) ? static_cast<EBtCompareOp>(cmpOp) : EBtCompareOp::Less;
        n.cmpRhs      = cmpRhs;
        if (std::strcmp(varbuf, "-") != 0) std::snprintf(n.var, sizeof(n.var), "%s", varbuf); // "-" は空
        n.x = x; n.y = y;
        n.alive = true;
        n.last_status = EBtStatus::Failure;
        std::snprintf(n.ename, sizeof(n.ename), "%s", namebuf);
        m_Nodes.PushBack(n);
    }

    // 動的ブラックボード変数 (v4)。"BB <count>" + 各変数行を m_DynBb へ復元する。
    if (ver >= 4 && m_DynBb != nullptr) {
        int vcount = 0;
        if (std::fgets(line, sizeof(line), f) != nullptr && std::sscanf(line, "BB %d", &vcount) == 1) {
            m_DynBb->Clear();
            for (int v = 0; v < vcount; ++v) {
                if (std::fgets(line, sizeof(line), f) == nullptr) break;
                char   vname[48] = {};
                int    vty       = 2;
                double vval      = 0.0;
                if (std::sscanf(line, "%47s %d %lf", vname, &vty, &vval) < 3) continue;
                const EBtVarType t = (vty == 0) ? EBtVarType::Bool
                                   : (vty == 1) ? EBtVarType::I32
                                   :              EBtVarType::F32;
                m_DynBb->Add(vname, t);
                switch (t) {
                    case EBtVarType::Bool: m_DynBb->SetBool(vname, vval != 0.0); break;
                    case EBtVarType::I32:  m_DynBb->SetI32(vname, static_cast<acs::i32>(vval)); break;
                    case EBtVarType::F32:  m_DynBb->SetF32(vname, static_cast<f32>(vval)); break;
                }
            }
        }
    }

    std::fclose(f);
    m_DidLayout = true;          // 保存座標を尊重 (auto-layout で潰さない)
    m_Selected  = kInvalidId;
    return true;
}
#endif

FBtGraphPersistenceResult ABehaviorTreeEditorPanel::TryParseGraphText(
    const char* text, usize text_size) noexcept {
    if (text == nullptr) {
        return GraphFailure(EBtGraphPersistenceError::NullArgument);
    }
    if (text_size == 0u) {
        return GraphFailure(EBtGraphPersistenceError::EmptyInput);
    }
    if (text_size > kMaxGraphTextBytes) {
        return GraphFailure(
            EBtGraphPersistenceError::InputTooLarge, 0u,
            static_cast<u64>(text_size));
    }
    if (std::memchr(text, '\0', text_size) != nullptr) {
        return GraphFailure(
            EBtGraphPersistenceError::EmbeddedNul, 0u,
            static_cast<u64>(text_size));
    }

    FBtGraphPersistenceResult result{};
    result.bytes = static_cast<u64>(text_size);
    usize offset = 0u;
    u32 line_number = 0u;
    char line[kMaxGraphLineBytes + 1u]{};
    usize line_length = 0u;
    auto next_line = [&]() noexcept -> bool {
        if (offset >= text_size ||
            result.error != EBtGraphPersistenceError::None) {
            return false;
        }
        ++line_number;
        if (line_number > kMaxGraphLines) {
            result.error = EBtGraphPersistenceError::TooManyLines;
            result.line = line_number;
            return false;
        }
        const usize begin = offset;
        while (offset < text_size && text[offset] != '\n') ++offset;
        usize length = offset - begin;
        if (offset < text_size) ++offset;
        if (length != 0u && text[begin + length - 1u] == '\r') --length;
        if (length > kMaxGraphLineBytes) {
            result.error = EBtGraphPersistenceError::LineTooLong;
            result.line = line_number;
            return false;
        }
        if (length != 0u) std::memcpy(line, text + begin, length);
        line[length] = '\0';
        line_length = length;
        return true;
    };

    if (!next_line()) {
        if (result.error == EBtGraphPersistenceError::None) {
            result.error = EBtGraphPersistenceError::EmptyInput;
        }
        return result;
    }

    FGraphTokenCursor header(line, line_length);
    const char* begin = nullptr;
    const char* end = nullptr;
    if (!header.Next(begin, end) ||
        static_cast<usize>(end - begin) != 5u ||
        std::memcmp(begin, "ACSBT", 5u) != 0) {
        result.error = EBtGraphPersistenceError::InvalidMagic;
        result.line = line_number;
        return result;
    }
    u32 version = 0u;
    if (!header.Next(begin, end)) {
        result.error = EBtGraphPersistenceError::UnsupportedVersion;
        result.line = line_number;
        return result;
    }
    EBtGraphPersistenceError parse_error =
        ParseU32Token(begin, end, version);
    if (parse_error != EBtGraphPersistenceError::None ||
        !header.Empty() || version < 1u || version > 4u) {
        result.error = EBtGraphPersistenceError::UnsupportedVersion;
        result.line = line_number;
        return result;
    }

    if (!next_line()) {
        if (result.error == EBtGraphPersistenceError::None) {
            result.error = EBtGraphPersistenceError::InvalidCount;
            result.line = line_number + 1u;
        }
        return result;
    }
    FGraphTokenCursor count_cursor(line, line_length);
    u32 node_count = 0u;
    if (!count_cursor.Next(begin, end)) {
        result.error = EBtGraphPersistenceError::InvalidCount;
        result.line = line_number;
        return result;
    }
    parse_error = ParseU32Token(begin, end, node_count);
    if (parse_error != EBtGraphPersistenceError::None ||
        !count_cursor.Empty()) {
        result.error = EBtGraphPersistenceError::InvalidCount;
        result.line = line_number;
        return result;
    }
    if (node_count > kMaxNodes) {
        result.error = EBtGraphPersistenceError::NodeCountLimit;
        result.line = line_number;
        return result;
    }

    FNodeMeta parsed_nodes[kMaxNodes]{};
    bool seen_ids[kMaxNodes]{};
    u32 source_lines[kMaxNodes]{};
    for (u32 record = 0u; record < node_count; ++record) {
        if (!next_line()) {
            if (result.error == EBtGraphPersistenceError::None) {
                result.error = EBtGraphPersistenceError::InvalidNodeRecord;
                result.line = line_number + 1u;
            }
            return result;
        }
        FGraphTokenCursor cursor(line, line_length);
        const char* token_begin = nullptr;
        const char* token_end = nullptr;
        auto required_token = [&]() noexcept -> bool {
            return cursor.Next(token_begin, token_end);
        };

        u32 id = 0u;
        i32 parent = -1;
        u32 kind = static_cast<u32>(EBtKind::Action);
        u32 decorator = static_cast<u32>(EBtDecoratorOp::Inverter);
        u32 decorator_mode = static_cast<u32>(EBtDecoMode::Transform);
        u32 compare_op = static_cast<u32>(EBtCompareOp::Less);
        f32 compare_rhs = 0.0f;
        f32 x = 0.0f;
        f32 y = 0.0f;
        const char* variable_begin = nullptr;
        const char* variable_end = nullptr;

        if (!required_token() ||
            (parse_error = ParseU32Token(
                 token_begin, token_end, id)) !=
                EBtGraphPersistenceError::None ||
            !required_token() ||
            (parse_error = ParseI32Token(
                 token_begin, token_end, parent)) !=
                EBtGraphPersistenceError::None ||
            !required_token() ||
            (parse_error = ParseU32Token(
                 token_begin, token_end, kind)) !=
                EBtGraphPersistenceError::None) {
            result.error = parse_error == EBtGraphPersistenceError::None
                ? EBtGraphPersistenceError::InvalidNodeRecord
                : parse_error;
            result.line = line_number;
            return result;
        }

        if (version >= 2u) {
            if (!required_token() ||
                (parse_error = ParseU32Token(
                     token_begin, token_end, decorator)) !=
                    EBtGraphPersistenceError::None) {
                result.error = parse_error == EBtGraphPersistenceError::None
                    ? EBtGraphPersistenceError::InvalidNodeRecord
                    : parse_error;
                result.line = line_number;
                return result;
            }
        }
        if (version >= 3u) {
            if (!required_token() ||
                (parse_error = ParseU32Token(
                     token_begin, token_end, decorator_mode)) !=
                    EBtGraphPersistenceError::None ||
                !required_token() ||
                (parse_error = ParseU32Token(
                     token_begin, token_end, compare_op)) !=
                    EBtGraphPersistenceError::None ||
                !required_token() ||
                (parse_error = ParseF32Token(
                     token_begin, token_end, compare_rhs)) !=
                    EBtGraphPersistenceError::None) {
                result.error = parse_error == EBtGraphPersistenceError::None
                    ? EBtGraphPersistenceError::InvalidNodeRecord
                    : parse_error;
                result.line = line_number;
                return result;
            }
        }
        if (!required_token() ||
            (parse_error = ParseF32Token(
                 token_begin, token_end, x)) !=
                EBtGraphPersistenceError::None ||
            !required_token() ||
            (parse_error = ParseF32Token(
                 token_begin, token_end, y)) !=
                EBtGraphPersistenceError::None) {
            result.error = parse_error == EBtGraphPersistenceError::None
                ? EBtGraphPersistenceError::InvalidNodeRecord
                : parse_error;
            result.line = line_number;
            return result;
        }
        if (version >= 3u) {
            if (!required_token()) {
                result.error = EBtGraphPersistenceError::InvalidNodeRecord;
                result.line = line_number;
                return result;
            }
            variable_begin = token_begin;
            variable_end = token_end;
        }

        if (id >= node_count) {
            result.error = EBtGraphPersistenceError::InvalidNodeId;
            result.line = line_number;
            return result;
        }
        if (seen_ids[id]) {
            result.error = EBtGraphPersistenceError::DuplicateNodeId;
            result.line = line_number;
            return result;
        }
        if (parent < -1 ||
            (parent >= 0 && static_cast<u32>(parent) >= node_count)) {
            result.error = EBtGraphPersistenceError::InvalidParentReference;
            result.line = line_number;
            return result;
        }
        if (kind > static_cast<u32>(EBtKind::Task)) {
            result.error = EBtGraphPersistenceError::InvalidKind;
            result.line = line_number;
            return result;
        }
        if (decorator > static_cast<u32>(EBtDecoratorOp::Repeat)) {
            result.error = EBtGraphPersistenceError::InvalidDecorator;
            result.line = line_number;
            return result;
        }
        if (decorator_mode > static_cast<u32>(EBtDecoMode::Compare)) {
            result.error = EBtGraphPersistenceError::InvalidDecoratorMode;
            result.line = line_number;
            return result;
        }
        if (compare_op > static_cast<u32>(EBtCompareOp::Greater)) {
            result.error = EBtGraphPersistenceError::InvalidCompareOp;
            result.line = line_number;
            return result;
        }

        FNodeMeta& node = parsed_nodes[id];
        node.id = id;
        node.parent_id =
            parent < 0 ? kInvalidId : static_cast<u32>(parent);
        node.kind = static_cast<EBtKind>(kind);
        node.deco = static_cast<EBtDecoratorOp>(decorator);
        node.decoMode = static_cast<EBtDecoMode>(decorator_mode);
        node.cmpOp = static_cast<EBtCompareOp>(compare_op);
        node.cmpRhs = compare_rhs;
        node.x = x;
        node.y = y;
        node.alive = true;
        node.last_status = EBtStatus::Failure;
        node.visit_order = 0u;
        node.name = nullptr;

        if (version >= 3u &&
            !(static_cast<usize>(variable_end - variable_begin) == 1u &&
              variable_begin[0] == '-')) {
            const usize variable_length =
                static_cast<usize>(variable_end - variable_begin);
            if (variable_length >= sizeof(node.var)) {
                result.error = EBtGraphPersistenceError::NameTooLong;
                result.line = line_number;
                return result;
            }
            if (!IsSafeToken(variable_begin, variable_length)) {
                result.error = EBtGraphPersistenceError::InvalidName;
                result.line = line_number;
                return result;
            }
            std::memcpy(node.var, variable_begin, variable_length);
            node.var[variable_length] = '\0';
        }

        const char* name = cursor.Remainder();
        const usize name_length =
            static_cast<usize>((line + line_length) - name);
        if (name_length >= sizeof(node.ename)) {
            result.error = EBtGraphPersistenceError::NameTooLong;
            result.line = line_number;
            return result;
        }
        if (!IsSafeDisplayName(name, name_length)) {
            result.error = EBtGraphPersistenceError::InvalidName;
            result.line = line_number;
            return result;
        }
        if (name_length != 0u) {
            std::memcpy(node.ename, name, name_length);
        }
        node.ename[name_length] = '\0';
        seen_ids[id] = true;
        source_lines[id] = line_number;
    }

    for (u32 id = 0u; id < node_count; ++id) {
        if (!seen_ids[id]) {
            result.error = EBtGraphPersistenceError::InvalidNodeId;
            return result;
        }
        const FNodeMeta& node = parsed_nodes[id];
        u32 child_count = 0u;
        for (u32 child = 0u; child < node_count; ++child) {
            if (parsed_nodes[child].parent_id == id) ++child_count;
        }
        if ((BtKindIsLeaf(node.kind) && child_count != 0u) ||
            (node.kind == EBtKind::Decorator && child_count > 1u)) {
            result.error = EBtGraphPersistenceError::InvalidStructure;
            result.line = source_lines[id];
            return result;
        }

        bool visited[kMaxNodes]{};
        u32 current = id;
        u32 depth = 0u;
        while (current != kInvalidId) {
            if (current >= node_count || !seen_ids[current]) {
                result.error =
                    EBtGraphPersistenceError::InvalidParentReference;
                result.line = source_lines[id];
                return result;
            }
            if (visited[current]) {
                result.error = EBtGraphPersistenceError::CycleDetected;
                result.line = source_lines[id];
                return result;
            }
            if (depth >= kMaxGraphDepth) {
                result.error =
                    EBtGraphPersistenceError::DepthLimitExceeded;
                result.line = source_lines[id];
                return result;
            }
            visited[current] = true;
            ++depth;
            current = parsed_nodes[current].parent_id;
        }
    }

    FBtBlackboard staged_blackboard;
    bool commit_blackboard = false;
    if (version >= 4u) {
        if (!next_line()) {
            if (result.error == EBtGraphPersistenceError::None) {
                result.error =
                    EBtGraphPersistenceError::InvalidBlackboardRecord;
                result.line = line_number + 1u;
            }
            return result;
        }
        FGraphTokenCursor bb_header(line, line_length);
        if (!bb_header.Next(begin, end) ||
            static_cast<usize>(end - begin) != 2u ||
            std::memcmp(begin, "BB", 2u) != 0 ||
            !bb_header.Next(begin, end)) {
            result.error =
                EBtGraphPersistenceError::InvalidBlackboardRecord;
            result.line = line_number;
            return result;
        }
        u32 blackboard_count = 0u;
        parse_error = ParseU32Token(begin, end, blackboard_count);
        if (parse_error != EBtGraphPersistenceError::None ||
            !bb_header.Empty()) {
            result.error =
                EBtGraphPersistenceError::InvalidBlackboardRecord;
            result.line = line_number;
            return result;
        }
        if (blackboard_count > FBtBlackboard::kMax) {
            result.error =
                EBtGraphPersistenceError::BlackboardCountLimit;
            result.line = line_number;
            return result;
        }

        for (u32 record = 0u; record < blackboard_count; ++record) {
            if (!next_line()) {
                if (result.error == EBtGraphPersistenceError::None) {
                    result.error =
                        EBtGraphPersistenceError::InvalidBlackboardRecord;
                    result.line = line_number + 1u;
                }
                return result;
            }
            FGraphTokenCursor cursor(line, line_length);
            const char* name_begin = nullptr;
            const char* name_end = nullptr;
            const char* type_begin = nullptr;
            const char* type_end = nullptr;
            const char* value_begin = nullptr;
            const char* value_end = nullptr;
            if (!cursor.Next(name_begin, name_end) ||
                !cursor.Next(type_begin, type_end) ||
                !cursor.Next(value_begin, value_end) ||
                !cursor.Empty()) {
                result.error =
                    EBtGraphPersistenceError::InvalidBlackboardRecord;
                result.line = line_number;
                return result;
            }
            const usize name_length =
                static_cast<usize>(name_end - name_begin);
            if (name_length >= FBtBlackboard::kNameLen) {
                result.error = EBtGraphPersistenceError::NameTooLong;
                result.line = line_number;
                return result;
            }
            if (!IsSafeToken(name_begin, name_length)) {
                result.error = EBtGraphPersistenceError::InvalidName;
                result.line = line_number;
                return result;
            }
            char name[FBtBlackboard::kNameLen]{};
            std::memcpy(name, name_begin, name_length);
            name[name_length] = '\0';
            if (staged_blackboard.Has(name)) {
                result.error =
                    EBtGraphPersistenceError::DuplicateBlackboardName;
                result.line = line_number;
                return result;
            }

            u32 type_value = 0u;
            parse_error = ParseU32Token(type_begin, type_end, type_value);
            if (parse_error != EBtGraphPersistenceError::None ||
                type_value > static_cast<u32>(EBtVarType::F32)) {
                result.error =
                    EBtGraphPersistenceError::InvalidBlackboardRecord;
                result.line = line_number;
                return result;
            }
            const EBtVarType type = static_cast<EBtVarType>(type_value);
            if (staged_blackboard.Add(name, type) ==
                FBtBlackboard::kInvalid) {
                result.error =
                    EBtGraphPersistenceError::BlackboardCountLimit;
                result.line = line_number;
                return result;
            }
            switch (type) {
                case EBtVarType::Bool: {
                    u32 value = 0u;
                    parse_error =
                        ParseU32Token(value_begin, value_end, value);
                    if (parse_error != EBtGraphPersistenceError::None ||
                        value > 1u) {
                        result.error =
                            EBtGraphPersistenceError::InvalidBlackboardRecord;
                        result.line = line_number;
                        return result;
                    }
                    staged_blackboard.SetBool(name, value != 0u);
                    break;
                }
                case EBtVarType::I32: {
                    i32 value = 0;
                    parse_error =
                        ParseI32Token(value_begin, value_end, value);
                    if (parse_error != EBtGraphPersistenceError::None) {
                        result.error = parse_error;
                        result.line = line_number;
                        return result;
                    }
                    staged_blackboard.SetI32(name, value);
                    break;
                }
                case EBtVarType::F32: {
                    f32 value = 0.0f;
                    parse_error =
                        ParseF32Token(value_begin, value_end, value);
                    if (parse_error != EBtGraphPersistenceError::None) {
                        result.error = parse_error;
                        result.line = line_number;
                        return result;
                    }
                    staged_blackboard.SetF32(name, value);
                    break;
                }
            }
        }
        commit_blackboard = true;
    }

    while (next_line()) {
        FGraphTokenCursor trailing(line, line_length);
        if (!trailing.Empty()) {
            result.error = version >= 4u
                ? EBtGraphPersistenceError::InvalidBlackboardRecord
                : EBtGraphPersistenceError::InvalidNodeRecord;
            result.line = line_number;
            return result;
        }
    }
    if (result.error != EBtGraphPersistenceError::None) return result;

    TArray<FNodeMeta> committed_nodes;
    if (!committed_nodes.TryReserve(static_cast<usize>(node_count))) {
        result.error = EBtGraphPersistenceError::AllocationFailure;
        return result;
    }
    for (u32 id = 0u; id < node_count; ++id) {
        if (!committed_nodes.TryPushBack(parsed_nodes[id])) {
            result.error = EBtGraphPersistenceError::AllocationFailure;
            return result;
        }
    }

    m_Nodes = Move(committed_nodes);
    if (commit_blackboard && m_DynBb != nullptr) {
        *m_DynBb = staged_blackboard;
    }
    m_DidLayout = true;
    m_Selected = kInvalidId;
    m_DragNode = kInvalidId;
    m_LinkSrc = kInvalidId;
    m_CtxNode = kInvalidId;
    return result;
}

FBtGraphPersistenceResult ABehaviorTreeEditorPanel::TryLoadGraph(
    const char* path) noexcept {
    if (path == nullptr) {
        return GraphFailure(EBtGraphPersistenceError::NullArgument);
    }
    usize path_length = 0u;
    if (!TryBoundedCStringLength(path, kMaxGraphPathBytes, path_length)) {
        return GraphFailure(EBtGraphPersistenceError::PathTooLong);
    }
    if (path_length == 0u) {
        return GraphFailure(EBtGraphPersistenceError::EmptyPath);
    }

    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return GraphFailure(EBtGraphPersistenceError::FileOpenFailed);
    }
    if (!SeekGraphFileEnd(file)) {
        std::fclose(file);
        return GraphFailure(EBtGraphPersistenceError::FileSizeFailed);
    }
    const i64 signed_size = TellGraphFile(file);
    if (signed_size < 0 || !SeekGraphFileBegin(file)) {
        std::fclose(file);
        return GraphFailure(EBtGraphPersistenceError::FileSizeFailed);
    }
    const u64 file_size = static_cast<u64>(signed_size);
    if (file_size == 0u) {
        std::fclose(file);
        return GraphFailure(EBtGraphPersistenceError::EmptyInput);
    }
    if (file_size > static_cast<u64>(kMaxGraphTextBytes)) {
        std::fclose(file);
        return GraphFailure(
            EBtGraphPersistenceError::InputTooLarge, 0u, file_size);
    }

    TArray<char> buffer;
    if (!buffer.TryResize(static_cast<usize>(file_size))) {
        std::fclose(file);
        return GraphFailure(EBtGraphPersistenceError::AllocationFailure);
    }
    usize total = 0u;
    while (total < buffer.Size()) {
        const usize read = std::fread(
            buffer.Data() + total, 1u, buffer.Size() - total, file);
        if (read == 0u) {
            const bool read_error = std::ferror(file) != 0;
            std::fclose(file);
            return GraphFailure(
                read_error
                    ? EBtGraphPersistenceError::FileReadFailed
                    : EBtGraphPersistenceError::FileChanged,
                0u, static_cast<u64>(total));
        }
        total += read;
    }
    const int extra = std::fgetc(file);
    const bool read_error = std::ferror(file) != 0;
    const int close_result = std::fclose(file);
    if (read_error) {
        return GraphFailure(
            EBtGraphPersistenceError::FileReadFailed, 0u,
            static_cast<u64>(total));
    }
    if (close_result != 0) {
        return GraphFailure(
            EBtGraphPersistenceError::FileCloseFailed, 0u,
            static_cast<u64>(total));
    }
    if (extra != EOF) {
        return GraphFailure(
            EBtGraphPersistenceError::FileChanged, 0u,
            static_cast<u64>(total));
    }
    return TryParseGraphText(buffer.Data(), buffer.Size());
}

/** 「ノード追加」メニュー項目群を描画する (popup / submenu から共通利用)。 */
void ABehaviorTreeEditorPanel::DrawAddMenu(u32 parent, f32 wx, f32 wy) noexcept {
    if (ImGui::MenuItem("Selector")) AddNodeGraph(EBtKind::Selector, parent, wx, wy);
    if (ImGui::MenuItem("Sequence")) AddNodeGraph(EBtKind::Sequence, parent, wx, wy);

    // Decorator (結果変換 op)。
    if (ImGui::BeginMenu("Decorator (op)")) {
        for (int d = 0; d < 4; ++d) {
            const EBtDecoratorOp op = static_cast<EBtDecoratorOp>(d);
            if (ImGui::MenuItem(DecoLabel(op)))
                SetNodeDecoratorOp(AddNodeGraph(EBtKind::Decorator, parent, wx, wy), op);
        }
        ImGui::EndMenu();
    }

    // Condition デコレーター (catalog の bool 関数で子をガード)。
    if (ImGui::BeginMenu("Condition (fn)")) {
        if (m_CondReg != nullptr && m_CondReg->Count() > 0) {
            for (u32 c = 0; c < m_CondReg->Count(); ++c) {
                const char* cn = m_CondReg->NameAt(c);
                if (ImGui::MenuItem(cn)) {
                    const u32 nid = AddNodeGraph(EBtKind::Decorator, parent, wx, wy);
                    if (IsValidId(nid, m_Nodes.Size())) {
                        std::snprintf(m_Nodes[static_cast<usize>(nid)].ename,
                                      sizeof(m_Nodes[static_cast<usize>(nid)].ename), "%s", cn);
                        m_Nodes[static_cast<usize>(nid)].decoMode = EBtDecoMode::Condition;
                    }
                }
            }
        } else {
            ImGui::TextDisabled("(no conditions registered)");
        }
        ImGui::EndMenu();
    }

    // Compare デコレーター (変数と定数の比較で子をガード。var/op/const は Inspector で設定)。
    if (ImGui::MenuItem("Compare (var)")) {
        const u32 nid = AddNodeGraph(EBtKind::Decorator, parent, wx, wy);
        if (IsValidId(nid, m_Nodes.Size())) m_Nodes[static_cast<usize>(nid)].decoMode = EBtDecoMode::Compare;
    }

    ImGui::Separator();

    // Task (catalog の Action 関数にバインド)。
    if (ImGui::BeginMenu("Task (catalog)")) {
        if (m_Registry != nullptr && m_Registry->Count() > 0) {
            for (u32 t = 0; t < m_Registry->Count(); ++t) {
                const char* tn = m_Registry->NameAt(t);
                if (ImGui::MenuItem(tn)) {
                    const u32 nid = AddNodeGraph(EBtKind::Task, parent, wx, wy);
                    if (IsValidId(nid, m_Nodes.Size()))
                        std::snprintf(m_Nodes[static_cast<usize>(nid)].ename,
                                      sizeof(m_Nodes[static_cast<usize>(nid)].ename), "%s", tn);
                }
            }
        } else {
            ImGui::TextDisabled("(no actions registered)");
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Task (blank)"))   AddNodeGraph(EBtKind::Task,   parent, wx, wy);
    if (ImGui::MenuItem("Action (blank)")) AddNodeGraph(EBtKind::Action, parent, wx, wy);
}

/** ノードグラフ canvas を描画 + 操作する。 */
void ABehaviorTreeEditorPanel::DrawGraph() noexcept {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 80.0f) size.x = 80.0f;
    if (size.y < 80.0f) size.y = 80.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (!m_DidLayout && !m_Nodes.IsEmpty()) { AutoLayout(); m_DidLayout = true; }

    const ImVec2 br = ImVec2(origin.x + size.x, origin.y + size.y);
    const float  z  = m_Zoom;

    // 背景 + グリッド
    dl->AddRectFilled(origin, br, IM_COL32(22, 24, 30, 255));
    const float grid = 32.0f * z;
    if (grid > 5.0f) {
        for (float gx = m_PanX - floorf(m_PanX / grid) * grid; gx < size.x; gx += grid)
            dl->AddLine(ImVec2(origin.x + gx, origin.y), ImVec2(origin.x + gx, br.y), IM_COL32(38, 42, 50, 255));
        for (float gy = m_PanY - floorf(m_PanY / grid) * grid; gy < size.y; gy += grid)
            dl->AddLine(ImVec2(origin.x, origin.y + gy), ImVec2(br.x, origin.y + gy), IM_COL32(38, 42, 50, 255));
    }
    dl->PushClipRect(origin, br, true);

    const float NW = 156.0f, NH = 52.0f;
#define BT_S(wx, wy) ImVec2(origin.x + m_PanX + (wx) * z, origin.y + m_PanY + (wy) * z)

    // エッジ (親→子)。直近 tick で訪問された子へのエッジは status 色で太く光らせ、
    // 「今どの経路を処理が流れたか」を可視化する。
    for (usize i = 0; i < m_Nodes.Size(); ++i) {
        const FNodeMeta& n = m_Nodes[i];
        if (!n.alive || !IsValidId(n.parent_id, m_Nodes.Size())) continue;
        const FNodeMeta& p = m_Nodes[static_cast<usize>(n.parent_id)];
        if (!p.alive) continue;
        const ImVec2 a = BT_S(p.x + NW * 0.5f, p.y + NH);
        const ImVec2 b = BT_S(n.x + NW * 0.5f, n.y);
        const float dyy = (b.y - a.y) * 0.5f;
        ImU32 ecol; float ew;
        if (n.visit_order > 0u) {
            f32 c[4]; StatusColor(n.last_status, c);
            ecol = IM_COL32(static_cast<int>(c[0]*255), static_cast<int>(c[1]*255), static_cast<int>(c[2]*255), 255);
            ew   = 3.5f;
        } else {
            ecol = IM_COL32(78, 86, 100, 255);
            ew   = 2.0f;
        }
        dl->AddBezierCubic(a, ImVec2(a.x, a.y + dyy), ImVec2(b.x, b.y - dyy), b, ecol, ew);
    }

    // リンクドラッグのプレビュー
    if (IsValidId(m_LinkSrc, m_Nodes.Size())) {
        const FNodeMeta& s = m_Nodes[static_cast<usize>(m_LinkSrc)];
        const ImVec2 a = BT_S(s.x + NW * 0.5f, s.y + NH);
        const ImVec2 b = io.MousePos;
        const float dyy = (b.y - a.y) * 0.5f;
        dl->AddBezierCubic(a, ImVec2(a.x, a.y + dyy), ImVec2(b.x, b.y - dyy), b, IM_COL32(255, 215, 90, 230), 2.5f);
    }

    // ノード
    for (usize i = 0; i < m_Nodes.Size(); ++i) {
        const FNodeMeta& n = m_Nodes[i];
        if (!n.alive) continue;
        const ImVec2 p0 = BT_S(n.x, n.y);
        const ImVec2 p1 = BT_S(n.x + NW, n.y + NH);
        ImU32 kindcol;
        switch (n.kind) {
            case EBtKind::Selector:  kindcol = IM_COL32(60, 110, 200, 255); break; // 青
            case EBtKind::Sequence:  kindcol = IM_COL32(210, 130, 50, 255); break; // 橙
            case EBtKind::Decorator: kindcol = IM_COL32(150, 90, 200, 255); break; // 紫
            case EBtKind::Task:      kindcol = IM_COL32(40, 160, 170, 255); break; // 青緑
            default:                 kindcol = IM_COL32(70, 160, 90, 255);  break; // Action 緑
        }
        const float th = 18.0f * z;
        dl->AddRectFilled(p0, p1, IM_COL32(44, 48, 60, 255), 6.0f);
        dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + th), kindcol, 6.0f, ImDrawFlags_RoundCornersTop);
        const bool sel = (m_Selected == static_cast<u32>(i));
        dl->AddRect(p0, p1, sel ? IM_COL32(255, 215, 90, 255) : IM_COL32(90, 100, 120, 255), 6.0f, 0, sel ? 2.5f : 1.0f);

        // 実行フロー: 直近 tick で訪問されたノードを status 色で発光 + 訪問順バッジ。
        if (n.visit_order > 0u) {
            f32 gc[4]; StatusColor(n.last_status, gc);
            const ImU32 glow = IM_COL32(static_cast<int>(gc[0]*255), static_cast<int>(gc[1]*255), static_cast<int>(gc[2]*255), 200);
            dl->AddRect(ImVec2(p0.x - 2.0f, p0.y - 2.0f), ImVec2(p1.x + 2.0f, p1.y + 2.0f), glow, 7.0f, 0, 2.0f);
            char ob[8]; std::snprintf(ob, sizeof(ob), "%u", static_cast<unsigned>(n.visit_order));
            const ImVec2 bp = ImVec2(p0.x - 5.0f * z, p0.y - 5.0f * z);
            dl->AddCircleFilled(bp, 8.0f * z, IM_COL32(20, 22, 28, 255));
            dl->AddCircle(bp, 8.0f * z, glow, 0, 1.5f);
            dl->AddText(ImGui::GetFont(), 11.0f * z, ImVec2(bp.x - 3.5f * z, bp.y - 6.5f * z), IM_COL32(238, 238, 242, 255), ob);
        }
        // 未解決ノードを赤 "?" で警告: leaf(registry未登録) / Condition(条件未登録) / Compare(変数未定義)。
        bool unbound = false;
        if (BtKindIsLeaf(n.kind)) {
            unbound = (m_Registry != nullptr) && (m_Registry->Find(DisplayName(n)) == nullptr);
        } else if (n.kind == EBtKind::Decorator && n.decoMode == EBtDecoMode::Condition) {
            unbound = (m_CondReg != nullptr) && (m_CondReg->Find(DisplayName(n)) == nullptr);
        } else if (n.kind == EBtKind::Decorator && n.decoMode == EBtDecoMode::Compare) {
            const bool known = (m_DynBb  != nullptr && m_DynBb->Has(n.var))
                            || (m_Schema != nullptr && m_Schema->IndexOf(n.var) != FBtBlackboardSchema::kInvalid);
            unbound = (m_DynBb != nullptr || m_Schema != nullptr) && (n.var[0] != '\0') && !known;
        }
        if (unbound) {
            dl->AddText(ImGui::GetFont(), 13.0f * z, ImVec2(p1.x - 13.0f * z, p1.y - 16.0f * z), IM_COL32(255, 90, 90, 255), "?");
        }

        f32 col[4]; StatusColor(n.last_status, col);
        dl->AddCircleFilled(ImVec2(p1.x - 9.0f * z, p0.y + th * 0.5f), 4.0f * z,
                            IM_COL32(static_cast<int>(col[0]*255), static_cast<int>(col[1]*255), static_cast<int>(col[2]*255), 255));
        if (z > 0.45f) {
            // ヘッダ: 種別。Decorator は mode に応じて変換 op / 条件 / 比較式を併記。
            char head[48];
            if (n.kind == EBtKind::Decorator) {
                switch (n.decoMode) {
                    case EBtDecoMode::Transform:
                        std::snprintf(head, sizeof(head), "Decorator: %s", DecoLabel(n.deco)); break;
                    case EBtDecoMode::Condition:
                        std::snprintf(head, sizeof(head), "Condition"); break;
                    case EBtDecoMode::Compare:
                        std::snprintf(head, sizeof(head), "If %s %s %g",
                                      (n.var[0] != '\0' ? n.var : "?"), CmpOpLabel(n.cmpOp),
                                      static_cast<double>(n.cmpRhs)); break;
                }
            } else {
                std::snprintf(head, sizeof(head), "%s", KindLabel(n.kind));
            }
            dl->AddText(ImGui::GetFont(), 13.0f * z, ImVec2(p0.x + 8.0f * z, p0.y + 2.0f * z),  IM_COL32(248, 248, 250, 255), head);
            dl->AddText(ImGui::GetFont(), 12.0f * z, ImVec2(p0.x + 8.0f * z, p0.y + th + 5.0f * z), IM_COL32(206, 212, 224, 255), DisplayName(n));
        }
        dl->AddCircleFilled(BT_S(n.x + NW * 0.5f, n.y), 4.0f * z, IM_COL32(180, 190, 205, 255));   // 入力(上)
        if (!BtKindIsLeaf(n.kind))
            dl->AddCircleFilled(BT_S(n.x + NW * 0.5f, n.y + NH), 4.5f * z, IM_COL32(255, 215, 90, 255)); // 出力(下: composite/decorator)
    }
    dl->PopClipRect();

    // ===== 操作 =====
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##bt_canvas", size,
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 mp = io.MousePos;

    u32 hitN = kInvalidId, hitOut = kInvalidId;
    for (int i = static_cast<int>(m_Nodes.Size()) - 1; i >= 0; --i) {
        const FNodeMeta& n = m_Nodes[static_cast<usize>(i)];
        if (!n.alive) continue;
        if (!BtKindIsLeaf(n.kind) && hitOut == kInvalidId) {
            const ImVec2 pc = BT_S(n.x + NW * 0.5f, n.y + NH);
            const float dx = mp.x - pc.x, dy = mp.y - pc.y;
            if (dx * dx + dy * dy <= (10.0f * z) * (10.0f * z)) hitOut = static_cast<u32>(i);
        }
        if (hitN == kInvalidId) {
            const ImVec2 p0 = BT_S(n.x, n.y), p1 = BT_S(n.x + NW, n.y + NH);
            if (mp.x >= p0.x && mp.x <= p1.x && mp.y >= p0.y && mp.y <= p1.y) hitN = static_cast<u32>(i);
        }
    }

    if (hov && io.MouseWheel != 0.0f) {
        const float oldZ = m_Zoom;
        float nz = m_Zoom * (1.0f + io.MouseWheel * 0.12f);
        nz = (nz < 0.3f) ? 0.3f : (nz > 2.5f ? 2.5f : nz);
        m_Zoom = nz;
        const float relx = mp.x - origin.x, rely = mp.y - origin.y;
        m_PanX = relx - (relx - m_PanX) * (nz / oldZ);
        m_PanY = rely - (rely - m_PanY) * (nz / oldZ);
    }

    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hitOut != kInvalidId)      { m_LinkSrc = hitOut; }
        else if (hitN != kInvalidId)   { m_Selected = hitN; m_DragNode = hitN; }
        else                           { m_Selected = kInvalidId; m_PanningBg = true; }
    }
    if (IsValidId(m_DragNode, m_Nodes.Size()) && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        m_Nodes[static_cast<usize>(m_DragNode)].x += io.MouseDelta.x / z;
        m_Nodes[static_cast<usize>(m_DragNode)].y += io.MouseDelta.y / z;
    }
    if (m_PanningBg && ImGui::IsMouseDown(ImGuiMouseButton_Left)) { m_PanX += io.MouseDelta.x; m_PanY += io.MouseDelta.y; }
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))         { m_PanX += io.MouseDelta.x; m_PanY += io.MouseDelta.y; }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (m_LinkSrc != kInvalidId) {
            if (hitN != kInvalidId && hitN != m_LinkSrc && !IsAncestor(hitN, m_LinkSrc)) {
                // Decorator は単子: 既存の子 (hitN 以外) を root へ外してから付け替える。
                if (m_Nodes[static_cast<usize>(m_LinkSrc)].kind == EBtKind::Decorator) {
                    for (usize i = 0; i < m_Nodes.Size(); ++i) {
                        if (m_Nodes[i].alive && m_Nodes[i].parent_id == m_LinkSrc
                            && static_cast<u32>(i) != hitN) {
                            m_Nodes[i].parent_id = kInvalidId;
                        }
                    }
                }
                m_Nodes[static_cast<usize>(hitN)].parent_id = m_LinkSrc;  // 接続 (子の親を付け替え)
            }
            m_LinkSrc = kInvalidId;
        }
        m_DragNode  = kInvalidId;
        m_PanningBg = false;
    }

    // 右クリックメニュー
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (hitN != kInvalidId) { m_CtxNode = hitN; m_Selected = hitN; ImGui::OpenPopup("##bt_node_ctx"); }
        else {
            m_AddX = (mp.x - origin.x - m_PanX) / z;
            m_AddY = (mp.y - origin.y - m_PanY) / z;
            ImGui::OpenPopup("##bt_add");
        }
    }
    if (ImGui::BeginPopup("##bt_add")) {
        ImGui::TextDisabled("Add node"); ImGui::Separator();
        DrawAddMenu(kInvalidId, m_AddX, m_AddY);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##bt_node_ctx")) {
        if (IsValidId(m_CtxNode, m_Nodes.Size())) {
            const f32 ax = m_Nodes[static_cast<usize>(m_CtxNode)].x + 40.0f;
            const f32 ay = m_Nodes[static_cast<usize>(m_CtxNode)].y + kRowH;
            ImGui::SetNextItemWidth(170.0f);
            ImGui::InputText("name", m_Nodes[static_cast<usize>(m_CtxNode)].ename,
                             sizeof(m_Nodes[static_cast<usize>(m_CtxNode)].ename));
            // Decorator が既に子を持つ場合、Add child は単子制約で既存子を置換する旨を明示。
            if (m_Nodes[static_cast<usize>(m_CtxNode)].kind == EBtKind::Decorator) {
                u32 ctx_kids = 0;
                for (usize i = 0; i < m_Nodes.Size(); ++i)
                    if (m_Nodes[i].alive && m_Nodes[i].parent_id == m_CtxNode) ++ctx_kids;
                if (ctx_kids >= 1) ImGui::TextDisabled("(Add child replaces current child)");
            }
            // leaf (Action/Task) は子を持てないので Add child を出さない。
            if (!BtKindIsLeaf(m_Nodes[static_cast<usize>(m_CtxNode)].kind) && ImGui::BeginMenu("Add child")) {
                DrawAddMenu(m_CtxNode, ax, ay);
                ImGui::EndMenu();
            }
            // Decorator は op をその場で切替できると便利。
            if (m_Nodes[static_cast<usize>(m_CtxNode)].kind == EBtKind::Decorator
                && ImGui::BeginMenu("Decorator op")) {
                for (int d = 0; d < 4; ++d) {
                    const EBtDecoratorOp op = static_cast<EBtDecoratorOp>(d);
                    const bool cur = (m_Nodes[static_cast<usize>(m_CtxNode)].deco == op);
                    if (ImGui::MenuItem(DecoLabel(op), nullptr, cur)) SetNodeDecoratorOp(m_CtxNode, op);
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Detach (root)")) m_Nodes[static_cast<usize>(m_CtxNode)].parent_id = kInvalidId;
            ImGui::Separator();
            if (ImGui::MenuItem("Delete")) DeleteNodeGraph(m_CtxNode);
        }
        ImGui::EndPopup();
    }
#undef BT_S
}

namespace {
/** FNV-1a 風の畳み込みヘルパ (undo の変更検出シグネチャ用)。 */
inline u64 SigMix(u64 h, u64 v) noexcept { return (h ^ v) * 1099511628211ull; }
inline u64 SigF32(u64 h, f32 f) noexcept { u32 b = 0; std::memcpy(&b, &f, sizeof(b)); return SigMix(h, b); }
inline u64 SigStr(u64 h, const char* s) noexcept {
    if (s != nullptr) for (; *s != '\0'; ++s) h = SigMix(h, static_cast<u64>(static_cast<unsigned char>(*s)));
    return h;
}
/** undo 履歴の上限 (memory ガード)。 */
constexpr u32 kMaxUndo = 64u;
} // namespace

/** 現在のグラフ状態 (ノード + 動的 BB) を out へコピーする。 */
void ABehaviorTreeEditorPanel::CaptureSnapshot(FGraphSnapshot& out) const noexcept {
    out.nodes.Clear();
    out.nodes.Reserve(m_Nodes.Size());
    for (usize i = 0; i < m_Nodes.Size(); ++i) out.nodes.PushBack(m_Nodes[i]);   // NodeMeta 値コピー
    out.hasBb = (m_DynBb != nullptr);
    if (out.hasBb) out.bb = *m_DynBb;                                            // FBtBlackboard コピー
}

/** スナップショットから現在のグラフ状態を復元する。 */
void ABehaviorTreeEditorPanel::RestoreSnapshot(const FGraphSnapshot& s) noexcept {
    m_Nodes.Clear();
    m_Nodes.Reserve(s.nodes.Size());
    for (usize i = 0; i < s.nodes.Size(); ++i) m_Nodes.PushBack(s.nodes[i]);
    if (s.hasBb && m_DynBb != nullptr) {
        // bb は「構造 (名前+型)」だけ snapshot に合わせ、生存変数の現在値は保持する。
        // signature は bb 値を追跡しない (graph-run の値変動でフラッドしないため) ので、
        // 構造編集の undo で間に挟まった値 poke を stale 値へ巻き戻さないよう値を引き継ぐ。
        FBtBlackboard rebuilt;
        for (u32 i = 0; i < s.bb.Count(); ++i) {
            const char*      nm = s.bb.NameAt(i);
            const EBtVarType ty = s.bb.TypeAt(i);
            rebuilt.Add(nm, ty);
            if (m_DynBb->Has(nm)) {                       // 現在値を引き継ぐ (新規復活変数は 0)
                const f32 cur = m_DynBb->GetAsF32(nm);
                switch (ty) {
                    case EBtVarType::Bool: rebuilt.SetBool(nm, cur != 0.0f); break;
                    case EBtVarType::I32:  rebuilt.SetI32(nm, static_cast<acs::i32>(cur)); break;
                    case EBtVarType::F32:  rebuilt.SetF32(nm, cur); break;
                }
            }
        }
        *m_DynBb = rebuilt;
    }
    // 進行中の操作状態はリセット (復元したノードに対して無効なため)。
    m_Selected = kInvalidId; m_DragNode = kInvalidId; m_LinkSrc = kInvalidId;
    m_CtxNode  = kInvalidId; m_PanningBg = false;
    m_DidLayout = true;   // 復元座標を尊重 (auto-layout で潰さない)
}

/** 現在のグラフ状態の変更検出用シグネチャ。 */
u64 ABehaviorTreeEditorPanel::GraphSignature() const noexcept {
    u64 h = 1469598103934665603ull;
    h = SigMix(h, static_cast<u64>(m_Nodes.Size()));
    for (usize i = 0; i < m_Nodes.Size(); ++i) {
        const FNodeMeta& n = m_Nodes[i];
        h = SigMix(h, n.alive ? 1u : 0u);
        if (!n.alive) continue;
        h = SigMix(h, n.parent_id);
        h = SigMix(h, static_cast<u64>(n.kind));
        h = SigMix(h, static_cast<u64>(n.deco));
        h = SigMix(h, static_cast<u64>(n.decoMode));
        h = SigMix(h, static_cast<u64>(n.cmpOp));
        h = SigF32(h, n.cmpRhs);
        h = SigF32(h, n.x);
        h = SigF32(h, n.y);
        h = SigStr(h, n.ename);
        h = SigStr(h, n.name);
        h = SigStr(h, n.var);
    }
    // 動的 BB は「変数の構成 (名前・型・個数)」だけを追跡し、値は含めない。
    // 値を含めると graph-run 中に条件関数が変数を書き換えるたびに「変更」と誤検知し、
    // undo 履歴が毎フレーム溢れてしまうため。値の poke は undo 対象外 (transient なデバッグ操作)。
    if (m_DynBb != nullptr) {
        h = SigMix(h, static_cast<u64>(m_DynBb->Count()));
        for (u32 v = 0; v < m_DynBb->Count(); ++v) {
            h = SigStr(h, m_DynBb->NameAt(v));
            h = SigMix(h, static_cast<u64>(m_DynBb->TypeAt(v)));
        }
    }
    return h;
}

/** 現在の baseline を undo stack へ積み、baseline を現在状態へ更新する。 */
void ABehaviorTreeEditorPanel::CommitBaseline() noexcept {
    m_UndoStack.PushBack(Move(m_UndoBaseline));   // 直前の確定状態を保存
    CaptureSnapshot(m_UndoBaseline);              // 新しい baseline = 現在
    m_BaselineSig = GraphSignature();
    m_RedoStack.Clear();                          // 新しい分岐 → redo 無効化

    // 上限超過分は古い物 (front) から落とす。
    while (m_UndoStack.Size() > static_cast<usize>(kMaxUndo)) {
        for (usize i = 0; i + 1 < m_UndoStack.Size(); ++i) m_UndoStack[i] = Move(m_UndoStack[i + 1]);
        m_UndoStack.PopBack();
    }
}

/** 毎フレーム呼び、編集が確定したら baseline を undo stack へ積む。 */
void ABehaviorTreeEditorPanel::UpdateUndoTracking() noexcept {
    if (!m_UndoInit) {
        CaptureSnapshot(m_UndoBaseline);
        m_BaselineSig = GraphSignature();
        m_UndoInit = true;
        return;
    }
    // 編集中 (マウス押下 / テキスト入力中) はコミットしない → drag/typing を 1 手にまとめる。
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsAnyItemActive()) return;
    if (GraphSignature() == m_BaselineSig) return;   // 変更なし
    CommitBaseline();
}

/** 現在のグラフ状態を undo 履歴へ明示的にコミットする (チェックポイント)。 */
void ABehaviorTreeEditorPanel::PushUndoCheckpoint() noexcept {
    if (!m_UndoInit) {   // 初回は baseline 初期化のみ
        CaptureSnapshot(m_UndoBaseline);
        m_BaselineSig = GraphSignature();
        m_UndoInit = true;
        return;
    }
    CommitBaseline();
}

/** 1 手戻す。 */
void ABehaviorTreeEditorPanel::Undo() noexcept {
    if (m_UndoStack.IsEmpty()) return;
    FGraphSnapshot cur; CaptureSnapshot(cur);
    m_RedoStack.PushBack(Move(cur));                                 // 現在 → redo
    FGraphSnapshot prev = Move(m_UndoStack[m_UndoStack.Size() - 1]);
    m_UndoStack.PopBack();
    RestoreSnapshot(prev);
    m_BaselineSig  = GraphSignature();
    m_UndoBaseline = Move(prev);
}

/** 1 手やり直す。 */
void ABehaviorTreeEditorPanel::Redo() noexcept {
    if (m_RedoStack.IsEmpty()) return;
    FGraphSnapshot cur; CaptureSnapshot(cur);
    m_UndoStack.PushBack(Move(cur));                                 // 現在 → undo
    FGraphSnapshot next = Move(m_RedoStack[m_RedoStack.Size() - 1]);
    m_RedoStack.PopBack();
    RestoreSnapshot(next);
    m_BaselineSig  = GraphSignature();
    m_UndoBaseline = Move(next);
}

/** toolbar + history graph + 左 tree view + 右 node inspector を描画する。 */
void ABehaviorTreeEditorPanel::DrawUI() noexcept {
    if (!IsVisible()) return;

    // ノードグラフが見えるよう十分な初期サイズを与える (ユーザが変えたら保存値を尊重)。
    ImGui::SetNextWindowSize(ImVec2(960.0f, 640.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(Title(), &m_Visible)) {
        ImGui::End();
        return;
    }

    // キーボードショートカット: Ctrl+Z = undo / Ctrl+Shift+Z or Ctrl+Y = redo。
    // テキスト入力中は item 側の undo を優先するため無効化する。
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive()) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (io.KeyShift) Redo(); else Undo(); }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) Redo();
    }

    // (1) Toolbar 行: Reset / Undo / Redo / Step / Continuous(autorun) | Active / Step counter
    if (ImGui::Button("Reset")) {
        Reset();
    }
    ImGui::SameLine();
    {
        const bool can_undo = !m_UndoStack.IsEmpty();
        if (!can_undo) ImGui::BeginDisabled();
        if (ImGui::Button("Undo")) Undo();
        if (!can_undo) ImGui::EndDisabled();
        ImGui::SameLine();
        const bool can_redo = !m_RedoStack.IsEmpty();
        if (!can_redo) ImGui::BeginDisabled();
        if (ImGui::Button("Redo")) Redo();
        if (!can_redo) ImGui::EndDisabled();
    }
    ImGui::SameLine();
    // Step は実行対象 (tree もしくは graph レジストリ) が無ければ disable。
    const bool step_enabled = (m_Tree != nullptr) || (m_Registry != nullptr);
    if (!step_enabled) ImGui::BeginDisabled();
    if (ImGui::Button("Step")) {
        StepOnce();
    }
    if (!step_enabled) ImGui::EndDisabled();

    ImGui::SameLine();
    // Continuous: autorun toggle。ON/OFF をラベルで明示。
    {
        bool autorun = m_Autorun;
        if (ImGui::Checkbox("Continuous", &autorun)) {
            m_Autorun = autorun;
        }
    }

    ImGui::SameLine();
    // 表示モード切替: ノードグラフ / 従来のツリーリスト。
    if (ImGui::Button(m_GraphMode ? "View: Graph" : "View: Tree")) {
        m_GraphMode = !m_GraphMode;
    }
    ImGui::SameLine();
    // ルートノードを追加 (空のツリーから組み始めるとき用)。
    if (ImGui::Button("+ Root")) {
        AddNodeGraph(EBtKind::Selector, kInvalidId, 60.0f, 40.0f);
        m_GraphMode = true;
    }
    ImGui::SameLine();
    // グラフの保存 / 読み込み (固定ファイル)。
    if (ImGui::Button("Save")) SaveGraph("behavior_tree.btg");
    ImGui::SameLine();
    if (ImGui::Button("Load")) { LoadGraph("behavior_tree.btg"); m_GraphMode = true; }
    if (m_Registry != nullptr) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "[graph-run]");  // no-code 実行モード
    }

    ImGui::SameLine();
    // Active node count: status != Failure な node 数を毎フレーム集計。
    // (= Inspector で「今いくつの node が Success/Running か」を一目で見る指標)
    u32 active = 0;
    for (usize i = 0; i < m_Nodes.Size(); ++i) {
        if (m_Nodes[i].last_status != EBtStatus::Failure) ++active;
    }
    ImGui::Text("| Active: %u  Step: %u",
                static_cast<unsigned>(active),
                static_cast<unsigned>(m_StepCount));

    ImGui::Separator();

    // (2) History graph: 60 frame の root status を PlotLines で表示
    // ring buffer を新→旧の時系列順に float へ展開する。
    // m_HistoryHead は「次に書き込む位置」 = "ちょうど 1 frame 前 + 1" なので、
    // PlotLines に対して `(head, head+1, ..., head+kHistorySize-1) mod size` の
    // 順に並べると左 → 右 = 古い → 新しい時系列になる。
    if (!m_History.IsEmpty()) {
        f32 plot[kHistorySize];
        for (u32 i = 0; i < kHistorySize; ++i) {
            const u32 idx = (m_HistoryHead + i) % kHistorySize;
            const EBtStatus s = static_cast<EBtStatus>(m_History[idx]);
            plot[i] = StatusToPlotValue(s);
        }
        // overlay 表示: 最新値の文字列ラベル。
        char overlay[32];
        const u32 last_idx = (m_HistoryHead + kHistorySize - 1u) % kHistorySize;
        std::snprintf(overlay, sizeof(overlay), "Root: %s",
                      StatusLabel(static_cast<EBtStatus>(m_History[last_idx])));
        ImGui::PlotLines("##bt_history", plot, static_cast<int>(kHistorySize),
                         0, overlay,
                         0.0f, 1.0f, // y range [Failure=0 .. Success=1]
                         ImVec2(0.0f, 60.0f));
    }

    ImGui::Separator();

    // (3) 2 カラム: 左 Tree View / 右 Node Inspector
    const float content_w = ImGui::GetContentRegionAvail().x;
    const float left_w    = m_GraphMode ? content_w * 0.74f
                          : ((content_w > 540.0f) ? content_w * 0.55f : content_w * 0.50f);

    // 左カラム: ノードグラフ (m_GraphMode) または従来のツリーリスト
    ImGui::BeginChild("##bt_tree_left", ImVec2(left_w, 0),
                      m_GraphMode ? false : true,
                      m_GraphMode ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                                  : 0);
    {
        if (m_GraphMode) {
            if (m_Nodes.IsEmpty()) {
                ImGui::TextDisabled("(No nodes) — right-click to add, or press [+ Root].");
            }
            DrawGraph();
        } else {
            ImGui::TextUnformatted("Behavior Tree");
            ImGui::Separator();
            if (m_Nodes.IsEmpty()) {
                ImGui::TextDisabled("(No nodes registered)");
                ImGui::TextDisabled("Call panel.AddNode(kind, name, parent_id) from your sample.");
            } else {
                // root (= parent_id == kInvalidId) を全て描画。複数 root も許容 (forest)。
                bool drew_any = false;
                for (u32 i = 0; i < m_Nodes.Size(); ++i) {
                    if (m_Nodes[i].alive && m_Nodes[i].parent_id == kInvalidId) {
                        DrawTreeRecursive(i, 0u);
                        drew_any = true;
                    }
                }
                if (!drew_any) {
                    ImGui::TextDisabled("(No root node — possible cycle)");
                }
            }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // 右カラム: Node Inspector
    ImGui::BeginChild("##bt_inspector_right", ImVec2(0, 0), true);
    {
        ImGui::TextUnformatted("Node Inspector");
        ImGui::Separator();

        if (!IsValidId(m_Selected, m_Nodes.Size()) || !m_Nodes[static_cast<usize>(m_Selected)].alive) {
            ImGui::TextDisabled("(No node selected)");
            ImGui::TextDisabled(m_GraphMode ? "Click a node. Right-click: add/delete."
                                            : "Click a node in the tree view.");
        } else {
            FNodeMeta& n = m_Nodes[static_cast<usize>(m_Selected)];

            // child count を線形走査でカウント (leaf なら 0、生存のみ)。
            u32 child_count = 0;
            if (!BtKindIsLeaf(n.kind)) {
                for (usize i = 0; i < m_Nodes.Size(); ++i) {
                    if (m_Nodes[i].alive && m_Nodes[i].parent_id == m_Selected) ++child_count;
                }
            }

            // 名前はインスペクタから直接編集可能。
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##bt_name", n.ename, sizeof(n.ename));
            ImGui::Text("Name     : %s", DisplayName(n));
            ImGui::Text("Kind     : %s", KindLabel(n.kind));
            ImGui::Text("Id       : %u", static_cast<unsigned>(n.id));
            if (n.parent_id == kInvalidId) {
                ImGui::Text("Parent   : (root)");
            } else {
                ImGui::Text("Parent   : %u", static_cast<unsigned>(n.parent_id));
            }
            ImGui::Text("Children : %u", static_cast<unsigned>(child_count));

            // Decorator: モード (Transform / Condition / Compare) と各モードの設定を編集。
            if (n.kind == EBtKind::Decorator) {
                int modeI = static_cast<int>(n.decoMode);
                const char* modes[] = { "Transform (op)", "Condition (fn)", "Compare (var)" };
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::Combo("##bt_decomode", &modeI, modes, 3)) {
                    n.decoMode = static_cast<EBtDecoMode>(modeI);
                }

                if (n.decoMode == EBtDecoMode::Transform) {
                    // 結果変換 op。
                    int cur = static_cast<int>(n.deco);
                    const char* items[] = { "Inverter", "ForceSuccess", "ForceFailure", "Repeat" };
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::Combo("##bt_deco", &cur, items, 4)) n.deco = static_cast<EBtDecoratorOp>(cur);
                } else if (n.decoMode == EBtDecoMode::Condition) {
                    // 条件関数を catalog から選ぶ (ノード名 = 関数名)。
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::BeginCombo("##bt_condpick", DisplayName(n))) {
                        if (m_CondReg != nullptr) {
                            for (u32 c = 0; c < m_CondReg->Count(); ++c) {
                                const char* cn = m_CondReg->NameAt(c);
                                if (ImGui::Selectable(cn)) std::snprintf(n.ename, sizeof(n.ename), "%s", cn);
                            }
                        } else {
                            ImGui::TextDisabled("(no condition registry set)");
                        }
                        ImGui::EndCombo();
                    }
                    if (m_CondReg != nullptr) {
                        const bool bound = (m_CondReg->Find(DisplayName(n)) != nullptr);
                        ImGui::TextColored(bound ? ImVec4(0.4f, 0.9f, 0.5f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                           "Condition : %s", bound ? "bound" : "UNBOUND");
                    }
                } else { // EBtDecoMode::Compare
                    // 変数を schema から選び、op と定数を指定 (var <op> const)。
                    ImGui::SetNextItemWidth(130.0f);
                    if (ImGui::BeginCombo("var", (n.var[0] != '\0' ? n.var : "(pick)"))) {
                        bool any = false;
                        if (m_DynBb != nullptr) {                       // 動的 BB の変数
                            for (u32 v = 0; v < m_DynBb->Count(); ++v) {
                                const char* vn = m_DynBb->NameAt(v);
                                if (ImGui::Selectable(vn)) std::snprintf(n.var, sizeof(n.var), "%s", vn);
                                any = true;
                            }
                        }
                        if (m_Schema != nullptr) {                      // offset スキーマの変数
                            for (u32 v = 0; v < m_Schema->Count(); ++v) {
                                const char* vn = m_Schema->NameAt(v);
                                if (ImGui::Selectable(vn)) std::snprintf(n.var, sizeof(n.var), "%s", vn);
                                any = true;
                            }
                        }
                        if (!any) ImGui::TextDisabled("(no blackboard variables)");
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    int opI = static_cast<int>(n.cmpOp);
                    const char* ops[] = { "<", "<=", "==", "!=", ">=", ">" };
                    ImGui::SetNextItemWidth(56.0f);
                    if (ImGui::Combo("##bt_cmpop", &opI, ops, 6)) n.cmpOp = static_cast<EBtCompareOp>(opI);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::InputFloat("##bt_cmprhs", &n.cmpRhs, 0.0f, 0.0f, "%.2f");
                    // 動的 BB と offset スキーマの「両方」に無いときだけ未解決警告
                    // (canvas の "?" バッジ / インタプリタの解決順と一致させる)。
                    if (n.var[0] != '\0' && (m_DynBb != nullptr || m_Schema != nullptr)) {
                        const bool known = (m_DynBb  != nullptr && m_DynBb->Has(n.var))
                                        || (m_Schema != nullptr && m_Schema->IndexOf(n.var) != FBtBlackboardSchema::kInvalid);
                        if (!known) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "var not bound");
                    }
                }

                if (n.decoMode != EBtDecoMode::Transform) {
                    ImGui::TextDisabled("true => run child, false => Failure");
                }
                if (child_count > 1) {
                    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                                       "Note: decorator uses 1 child (leftmost).");
                }
            }
            // leaf は registry 解決状況を表示 (graph-run 時)。
            if (BtKindIsLeaf(n.kind) && m_Registry != nullptr) {
                const bool bound = (m_Registry->Find(DisplayName(n)) != nullptr);
                ImGui::TextColored(bound ? ImVec4(0.4f, 0.9f, 0.5f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                   "Action fn : %s", bound ? "bound" : "UNBOUND (rename to match registry)");
            }

            ImGui::Separator();

            // Last Status (status color text)。
            f32 col[4];
            StatusColor(n.last_status, col);
            ImGui::TextUnformatted("Last Status : ");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(col[0], col[1], col[2], col[3]),
                               "%s", StatusLabel(n.last_status));
        }

        // ===== Blackboard =====
        // 動的ブラックボード (エディタ所有): 変数の 追加 / リネーム / 型変更 / 値編集 / 削除。
        // ここで足した変数はそのまま Compare デコレーターの変数候補になり、値を poke すると
        // 実行中のグラフへ即反映される (= コードに無い変数をエディタだけで足して条件に使える)。
        if (m_DynBb != nullptr) {
            ImGui::Separator();
            ImGui::TextUnformatted("Blackboard (editable)");
            if (ImGui::SmallButton("+ Add var")) m_DynBb->Add(nullptr, EBtVarType::F32);
            u32 removeIdx = FBtBlackboard::kInvalid;
            // 幅に依存しない 2 行レイアウト: 行1 = [x][name]、行2 = [type][value]。
            for (u32 v = 0; v < m_DynBb->Count(); ++v) {
                ImGui::PushID(static_cast<int>(1000 + v));
                // 行1: 削除ボタン + 名前 (rename はバッファ直接編集、残り幅いっぱい)。
                if (ImGui::SmallButton("x")) removeIdx = v;
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1.0f);
                // 変数名は空白禁止 (シリアライズが 1 トークン %s で読むため、空白だと load 時に消える)。
                ImGui::InputText("##nm", m_DynBb->NameBufAt(v), FBtBlackboard::kNameLen,
                                 ImGuiInputTextFlags_CharsNoBlank);
                // 行2: 型コンボ + 値エディタ。
                int ti = static_cast<int>(m_DynBb->TypeAt(v));
                const char* types[] = { "Bool", "I32", "F32" };
                ImGui::SetNextItemWidth(64.0f);
                if (ImGui::Combo("##ty", &ti, types, 3)) m_DynBb->SetType(v, static_cast<EBtVarType>(ti));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1.0f);
                switch (m_DynBb->TypeAt(v)) {
                    case EBtVarType::Bool: { bool* p = m_DynBb->BoolPtrAt(v); if (p) ImGui::Checkbox("##vl", p); break; }
                    case EBtVarType::I32:  { i32*  p = m_DynBb->I32PtrAt(v);  if (p) ImGui::InputInt("##vl", reinterpret_cast<int*>(p), 0, 0); break; }
                    case EBtVarType::F32:  { f32*  p = m_DynBb->F32PtrAt(v);  if (p) ImGui::InputFloat("##vl", p); break; }
                }
                ImGui::Separator();
                ImGui::PopID();
            }
            if (removeIdx != FBtBlackboard::kInvalid) m_DynBb->Remove(removeIdx);
            ImGui::TextDisabled("Add/rename vars; edit a value to poke the tree.");
        }
        // offset スキーマ (コード所有、読み取り専用 schema): 値の poke のみ。
        if (m_Schema != nullptr && m_GraphBb != nullptr && m_Schema->Count() > 0) {
            ImGui::Separator();
            ImGui::TextUnformatted("Blackboard (schema, poke)");
            for (u32 v = 0; v < m_Schema->Count(); ++v) {
                const char* vn   = m_Schema->NameAt(v);
                char*       base = static_cast<char*>(m_GraphBb) + m_Schema->OffsetAt(v);
                ImGui::PushID(static_cast<int>(2000 + v));
                ImGui::SetNextItemWidth(130.0f);
                switch (m_Schema->TypeAt(v)) {
                    case EBtVarType::Bool: ImGui::Checkbox(vn,   reinterpret_cast<bool*>(base)); break;
                    case EBtVarType::I32:  ImGui::InputInt(vn,   reinterpret_cast<int*>(base));  break;
                    case EBtVarType::F32:  ImGui::InputFloat(vn, reinterpret_cast<f32*>(base));  break;
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();

    // この frame の編集が確定したら undo 履歴へコミットする (drag/typing は settle 後に 1 手)。
    UpdateUndoTracking();

    ImGui::End();
}

} // namespace acs::game::btedit

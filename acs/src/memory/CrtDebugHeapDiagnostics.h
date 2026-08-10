// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory - MSVC CRT デバッグヒープ診断
// -----------------------------------------------------------------------------
// Debug/MSVC では CRT デバッグヒープのチェックポイント、差分、終了時リーク検査を
// 扱う。Release または非 MSVC では同じ API を安全な no-op として提供する。
// =============================================================================
#pragma once

#include "foundation/Compiler.h"
#include "foundation/Types.h"

namespace acs {

/** CRT デバッグヒープ診断スコープの動作設定。 */
struct FCrtDebugHeapScopeConfiguration {
    /** 機械可読ログに載せるスコープ名。Begin 時に内部へコピーされる。 */
    const char* ScopeName = "unnamed";

    /** Begin と End で CRT ヒープの整合性を検査する。 */
    bool bCheckHeapIntegrity = true;

    /** リーク検出時にチェックポイント以後の全オブジェクトを CRT へダンプする。 */
    bool bDumpObjectsOnLeak = false;

    /** CRT 自身の内部ブロックもリーク判定へ含める。通常は false を推奨する。 */
    bool bIncludeCrtBlocksInLeakResult = false;

    /** End 時に成否に応じた機械可読ログを標準出力または標準エラーへ出力する。 */
    bool bWriteMachineReadableLog = true;
};

/** CRT デバッグヒープのチェックポイント差分。 */
struct FCrtDebugHeapScopeReport {
    /** このビルドで MSVC CRT デバッグヒープが利用可能か。 */
    bool bSupported = false;

    /** 有効な Begin に対応する End だったか。 */
    bool bWasActive = false;

    /** Begin と End の両方で CRT の割り当て追跡が有効だったか。 */
    bool bAllocationTrackingEnabled = false;

    /** Begin/End のヒープ整合性検査がすべて成功したか。 */
    bool bHeapValid = true;

    /** CRT がチェックポイント間に何らかの差分を検出したか。 */
    bool bDifferenceDetected = false;

    /** 判定対象ブロックの正味増加が残っているか。 */
    bool bLeakDetected = false;

    /** 判定対象となった未解放ブロック数。 */
    u64 OutstandingAllocationCount = 0;

    /** 判定対象となった未解放バイト数。 */
    u64 OutstandingBytes = 0;

    /** 通常ブロックの正味増加件数。 */
    u64 NormalAllocationCount = 0;

    /** 通常ブロックの正味増加バイト数。 */
    u64 NormalBytes = 0;

    /** クライアントブロックの正味増加件数。 */
    u64 ClientAllocationCount = 0;

    /** クライアントブロックの正味増加バイト数。 */
    u64 ClientBytes = 0;

    /** CRT 内部ブロックの正味増加件数。 */
    u64 CrtAllocationCount = 0;

    /** CRT 内部ブロックの正味増加バイト数。 */
    u64 CrtBytes = 0;

    /** Begin から End まで追跡設定とヒープが有効で、リーク判定を信頼できるか。 */
    bool bMeasurementConclusive = false;

    /** Begin から End まで CRT デバッグヒープ設定が変化しなかったか。 */
    bool bConfigurationStable = false;
};

/** `_CrtDumpMemoryLeaks` によるプロセス全体の直接検査結果。 */
struct FCrtDebugHeapProcessLeakReport {
    /** 現在のビルドで MSVC Debug CRT 診断を利用できるか。 */
    bool bSupported = false;

    /** 再入拒否を受けず、直接検査を最後まで実行できたか。 */
    bool bInspectionSucceeded = false;

    /** `_CrtDumpMemoryLeaks` が未解放 Debug CRT ブロックを検出したか。 */
    bool bLeakDetected = false;
};

/** CRT のヒープ検査頻度。高頻度ほど診断精度と実行コストが上がる。 */
enum class ECrtDebugHeapCheckFrequency : u8 {
    Default,
    Every16Operations,
    Every128Operations,
    Every1024Operations,
    EveryOperation,
};

/** プロセス全体へ一時適用する CRT デバッグヒープ設定。 */
struct FCrtDebugHeapProcessConfiguration {
    /** デバッグヒープによる割り当て追跡を有効にする。 */
    bool bEnableAllocationTracking = true;

    /** プロセス正常終了時に CRT のリークダンプを実行する。 */
    bool bEnableProcessExitLeakCheck = true;

    /** 解放済みブロックを保持し、書き込み破壊の検出に利用する。 */
    bool bRetainFreedBlocks = false;

    /** CRT 自身の内部ブロックをリーク検査へ含める。 */
    bool bIncludeCrtBlocks = false;

    /** ヒープ検査の頻度。 */
    ECrtDebugHeapCheckFrequency CheckFrequency = ECrtDebugHeapCheckFrequency::Default;

    /** CRT の警告、エラー、アサートをデバッガへ送る。 */
    bool bReportToDebugger = true;

    /** CRT の警告、エラー、アサートを標準エラーへ送る。 */
    bool bReportToStandardError = true;

    /** BreakOnAllocationSequence をこの設定スコープで適用する。 */
    bool bConfigureBreakOnAllocationSequence = false;

    /** ブレークする CRT 割り当て通し番号。-1 はブレークを無効にする。 */
    i64 BreakOnAllocationSequence = -1;
};

/**
 * CRT ヒープのチェックポイント間に残る正味増加を検出する再利用可能スコープ。
 *
 * @details Begin は多重呼び出しと、別の診断スコープとの重複を拒否する。End 後は同じインスタンスを
 * 再利用できる。プロセス設定スコープ内で開始した場合、その設定は End のチェックポイントまで固定される。
 * デストラクタは有効なスコープを自動的に End する。
 */
class FCrtDebugHeapScope final {
public:
    FCrtDebugHeapScope() noexcept = default;
    ~FCrtDebugHeapScope() noexcept;

    FCrtDebugHeapScope(const FCrtDebugHeapScope&) = delete;
    FCrtDebugHeapScope& operator=(const FCrtDebugHeapScope&) = delete;
    FCrtDebugHeapScope(FCrtDebugHeapScope&&) = delete;
    FCrtDebugHeapScope& operator=(FCrtDebugHeapScope&&) = delete;

    /** 現在位置を基準チェックポイントとして診断を開始する。追跡無効時は false。 */
    bool Begin(const FCrtDebugHeapScopeConfiguration& Configuration = {}) noexcept;

    /** 現在位置との差分を収集し、必要ならダンプと機械可読ログを出力する。 */
    FCrtDebugHeapScopeReport End() noexcept;

    /** Begin 済みか。 */
    bool IsActive() const noexcept
    {
        return m_Active;
    }

private:
    /** CRT 型を公開ヘッダへ漏らさないための Win32 ヒープ上の内部状態。 */
    void* m_Implementation = nullptr;

    bool m_Active = false;
};

/**
 * プロセス全体の CRT デバッグヒープ設定を一時変更するスコープ。
 *
 * @details 対応ビルドでは同時に有効化できるインスタンスは一つだけ。診断スコープが先に有効な場合は
 * Begin を拒否する。設定スコープ内で始めた診断が残る間に End した場合は、そのチェックポイントが完了するまで
 * debug flags、break allocation、全 report mode/file の復元を遅延する。
 */
class FCrtDebugHeapProcessConfigurationScope final {
public:
    FCrtDebugHeapProcessConfigurationScope() noexcept = default;
    ~FCrtDebugHeapProcessConfigurationScope() noexcept;

    FCrtDebugHeapProcessConfigurationScope(const FCrtDebugHeapProcessConfigurationScope&) = delete;
    FCrtDebugHeapProcessConfigurationScope& operator=(const FCrtDebugHeapProcessConfigurationScope&) = delete;
    FCrtDebugHeapProcessConfigurationScope(FCrtDebugHeapProcessConfigurationScope&&) = delete;
    FCrtDebugHeapProcessConfigurationScope& operator=(FCrtDebugHeapProcessConfigurationScope&&) = delete;

    /** 設定を退避して一時設定を適用する。 */
    bool Begin(const FCrtDebugHeapProcessConfiguration& Configuration = {}) noexcept;

    /** Begin 前の設定を復元する。診断中の場合は、その診断の End まで復元を遅延する。 */
    void End() noexcept;

    /** 設定が現在適用中か。 */
    bool IsActive() const noexcept
    {
        return m_Active;
    }

private:
    /** CRT 型と report file 型を公開ヘッダへ漏らさない内部状態。 */
    void* m_Implementation = nullptr;

    bool m_Active = false;
};

/** MSVC CRT デバッグヒープの単発診断 API。 */
class CCrtDebugHeapDiagnostics final {
public:
    CCrtDebugHeapDiagnostics() = delete;

    /** MSVC の Debug CRT 診断が利用可能か。 */
    static bool IsSupported() noexcept;

    /** CRT ヒープを検査する。report hook からの再入時は false。非対応ビルドでは true。 */
    static bool CheckHeapIntegrity() noexcept;

    /** 現在生存する全 CRT ヒープオブジェクトを出力する。report hook からの再入は何もせず戻る。 */
    static void DumpAllLiveObjects() noexcept;

    /**
     * `_CrtDumpMemoryLeaks` を実行し、結果を機械可読な1行として出力します。
     * 成功は標準出力、リーク・検査不能・非対応・標準出力の書込み失敗は標準エラーへ送ります。
     * 標準エラーの書込み失敗はデバッガだけへ送ります。
     * @param bWriteMachineReadableLog 機械可読な判定を出力するか。
     * @return 対応状況、検査完了、リーク検出の結果。
     */
    static FCrtDebugHeapProcessLeakReport DumpProcessMemoryLeaks(
        bool bWriteMachineReadableLog = true) noexcept;

    /**
     * 指定した CRT 割り当て通し番号でブレークする。
     * @return 変更前の通し番号。非対応ビルドでは -1。
     */
    static i64 SetBreakOnAllocationSequence(i64 AllocationSequence) noexcept;

    /**
     * プロセス正常終了時の CRT リーク検査を切り替える。
     * @return 変更前に有効だった場合 true。非対応ビルドでは false。
     */
    static bool SetProcessExitLeakCheckEnabled(bool bEnabled) noexcept;
};

/** 移行期間中に旧名を受け付ける互換別名。 */
using FCrtDebugHeapDiagnostics = CCrtDebugHeapDiagnostics;

} // namespace acs

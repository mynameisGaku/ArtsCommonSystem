// SPDX-License-Identifier: Apache-2.0

using System;
using System.IO;

namespace AcsEditor;

internal static class EditorAbiContractSelfTest
{
    internal static int Run(TextWriter output)
    {
        ArgumentNullException.ThrowIfNull(output);
        int failures = 0;

        void Check(bool condition, string description)
        {
            if (condition)
            {
                output.WriteLine($"[PASS] {description}");
                return;
            }
            output.WriteLine($"[FAIL] {description}");
            failures++;
        }

        Check(
            sizeof(int) == 4 &&
            sizeof(uint) == 4 &&
            sizeof(ulong) == 8,
            "managed P/Invoke scalar widths match the native ABI");

        EditorAbiCapability complete =
            EditorAbiContract.RequiredCapabilities |
            EditorAbiCapability.ProfilerV3 |
            EditorAbiCapability.UnifiedSceneDocument |
            EditorAbiCapability.VolumetricCloudWorkloadV1;
        EditorAbiSnapshot compatible = EditorAbiContract.Evaluate(
            queryAvailable: true,
            queryResult: 1,
            providerVersion: EditorAbiContract.RequestedVersion,
            capabilityBits: (ulong)complete,
            productVersion: "ACS Editor test",
            renderBackend: "Test RHI");
        Check(compatible.Compatible && compatible.MissingRequired == 0,
            "current provider with every required capability is compatible");
        Check(compatible.ToDisplayText().Contains(
                  "profiler-v3",
                  StringComparison.Ordinal) &&
              compatible.ToDisplayText().Contains(
                  "cloud-workload-v1",
                  StringComparison.Ordinal) &&
              !EditorAbiContract.RequiredCapabilities.HasFlag(
                  EditorAbiCapability.VolumetricCloudWorkloadV1),
            "diagnostics list negotiated optional capabilities");

        EditorAbiSnapshot missing = EditorAbiContract.Evaluate(
            queryAvailable: true,
            queryResult: 0,
            providerVersion: EditorAbiContract.RequestedVersion,
            capabilityBits:
                (ulong)(complete &
                    ~EditorAbiCapability.ResizeResultContract),
            productVersion: "ACS Editor test",
            renderBackend: "Test RHI");
        Check(!missing.Compatible &&
              missing.MissingRequired.HasFlag(
                  EditorAbiCapability.ResizeResultContract),
            "missing required capability fails closed with an exact reason");

        EditorAbiSnapshot falseSuccess = EditorAbiContract.Evaluate(
            queryAvailable: true,
            queryResult: 1,
            providerVersion: EditorAbiContract.RequestedVersion,
            capabilityBits:
                (ulong)(complete &
                    ~EditorAbiCapability.FrameResultContract),
            productVersion: "false-success",
            renderBackend: "test");
        Check(!falseSuccess.Compatible &&
              falseSuccess.MissingRequired.HasFlag(
                  EditorAbiCapability.FrameResultContract),
            "provider success cannot mask a missing required capability");

        EditorAbiSnapshot legacy = EditorAbiContract.Evaluate(
            queryAvailable: false,
            queryResult: 0,
            providerVersion: 0,
            capabilityBits: 0,
            productVersion: null,
            renderBackend: null);
        Check(!legacy.Compatible &&
              legacy.Diagnostic.Contains(
                  "does not expose",
                  StringComparison.Ordinal),
            "legacy DLL without the query export is reported, not guessed");

        EditorAbiSnapshot disguisedLegacy = EditorAbiContract.Evaluate(
            queryAvailable: false,
            queryResult: 1,
            providerVersion: EditorAbiContract.RequestedVersion,
            capabilityBits: (ulong)complete,
            productVersion: "legacy-with-current-label",
            renderBackend: "test");
        Check(!disguisedLegacy.Compatible,
            "matching metadata cannot mask a missing legacy query export");

        EditorAbiSnapshot future = EditorAbiContract.Evaluate(
            queryAvailable: true,
            queryResult: 1,
            providerVersion: EditorAbiContract.RequestedVersion + 1,
            capabilityBits: (ulong)complete | (1UL << 63),
            productVersion: "future",
            renderBackend: "future");
        Check(future.Compatible &&
              future.UnknownCapabilityBits == (1UL << 63) &&
              future.ToDisplayText().Contains(
                  "0x8000000000000000",
                  StringComparison.Ordinal),
            "additive provider versions and unknown capability bits are preserved");

        EditorAbiSnapshot rejected = EditorAbiContract.Evaluate(
            queryAvailable: true,
            queryResult: 0,
            providerVersion: EditorAbiContract.RequestedVersion,
            capabilityBits: (ulong)complete,
            productVersion: "rejected",
            renderBackend: "test");
        Check(!rejected.Compatible &&
              rejected.Diagnostic.Contains(
                  "rejected",
                  StringComparison.Ordinal),
            "provider rejection cannot be masked by matching local data");

        output.WriteLine(
            failures == 0
                ? "Editor ABI contract self-test PASS."
                : $"Editor ABI contract self-test FAIL ({failures}).");
        return failures;
    }
}

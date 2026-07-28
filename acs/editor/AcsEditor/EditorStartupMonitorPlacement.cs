// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;

namespace AcsEditor;

internal readonly record struct EditorStartupMonitorSelector(
    bool PreferSecondary,
    int MonitorIndex);

internal readonly record struct EditorMonitorWorkArea(
    string DeviceName,
    int Left,
    int Top,
    int Right,
    int Bottom,
    bool IsPrimary)
{
    internal int Width => Math.Max(0, Right - Left);
    internal int Height => Math.Max(0, Bottom - Top);
}

/// <summary>
/// Resolves and applies an explicit editor startup monitor without activating
/// the window. Monitor selection is intentionally opt-in so the normal saved
/// editor layout remains authoritative for ordinary launches.
/// </summary>
internal static class EditorStartupMonitorPlacement
{
    private const uint MonitorInfoPrimary = 0x00000001;
    private const uint SwpNoZOrder = 0x0004;
    private const uint SwpNoActivate = 0x0010;
    private const uint SwpNoOwnerZOrder = 0x0200;

    private delegate bool MonitorEnumProcedure(
        nint monitor,
        nint deviceContext,
        ref NativeRect monitorRectangle,
        nint applicationData);

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeRect
    {
        internal int Left;
        internal int Top;
        internal int Right;
        internal int Bottom;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct NativeMonitorInfo
    {
        internal int Size;
        internal NativeRect Monitor;
        internal NativeRect WorkArea;
        internal uint Flags;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        internal string DeviceName;
    }

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnumDisplayMonitors(
        nint deviceContext,
        nint clippingRectangle,
        MonitorEnumProcedure callback,
        nint applicationData);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetMonitorInfo(
        nint monitor,
        ref NativeMonitorInfo monitorInfo);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetWindowRect(
        nint window,
        out NativeRect rectangle);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetWindowPos(
        nint window,
        nint insertAfter,
        int x,
        int y,
        int width,
        int height,
        uint flags);

    internal static bool TryParse(
        IReadOnlyList<string> arguments,
        out EditorStartupMonitorSelector? selector,
        out string? error)
    {
        selector = null;
        error = null;
        bool sawSecondary = false;
        int? explicitIndex = null;

        for (int index = 0; index < arguments.Count; index++)
        {
            string argument = arguments[index];
            if (string.Equals(
                    argument,
                    "--secondary-monitor",
                    StringComparison.OrdinalIgnoreCase))
            {
                if (sawSecondary)
                {
                    error = "--secondary-monitor may be specified only once.";
                    return false;
                }

                sawSecondary = true;
                continue;
            }

            if (!string.Equals(
                    argument,
                    "--monitor",
                    StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            if (explicitIndex.HasValue)
            {
                error = "--monitor may be specified only once.";
                return false;
            }
            if (++index >= arguments.Count ||
                !int.TryParse(
                    arguments[index],
                    NumberStyles.None,
                    CultureInfo.InvariantCulture,
                    out int parsedIndex) ||
                parsedIndex < 0)
            {
                error = "--monitor requires a non-negative integer index.";
                return false;
            }

            explicitIndex = parsedIndex;
        }

        if (sawSecondary && explicitIndex.HasValue)
        {
            error = "--secondary-monitor and --monitor cannot be combined.";
            return false;
        }
        if (sawSecondary)
        {
            selector = new EditorStartupMonitorSelector(
                PreferSecondary: true,
                MonitorIndex: 0);
        }
        else if (explicitIndex.HasValue)
        {
            selector = new EditorStartupMonitorSelector(
                PreferSecondary: false,
                MonitorIndex: explicitIndex.Value);
        }

        return true;
    }

    internal static bool TryResolve(
        IReadOnlyList<EditorMonitorWorkArea> availableMonitors,
        EditorStartupMonitorSelector selector,
        out EditorMonitorWorkArea target,
        out string? error)
    {
        target = default;
        error = null;

        EditorMonitorWorkArea[] monitors = availableMonitors
            .Where(monitor => monitor.Width > 0 && monitor.Height > 0)
            .OrderByDescending(monitor => monitor.IsPrimary)
            .ThenBy(monitor => monitor.DeviceName, StringComparer.OrdinalIgnoreCase)
            .ThenBy(monitor => monitor.Left)
            .ThenBy(monitor => monitor.Top)
            .ToArray();
        if (monitors.Length == 0)
        {
            error = "Windows did not report an available monitor work area.";
            return false;
        }

        if (selector.PreferSecondary)
        {
            EditorMonitorWorkArea? secondary = monitors
                .Cast<EditorMonitorWorkArea?>()
                .FirstOrDefault(monitor => monitor is { IsPrimary: false });
            if (!secondary.HasValue)
            {
                error = "--secondary-monitor requires at least two active monitors.";
                return false;
            }

            target = secondary.Value;
            return true;
        }

        if (selector.MonitorIndex >= monitors.Length)
        {
            error =
                $"--monitor {selector.MonitorIndex} is unavailable; " +
                $"Windows reported {monitors.Length} active monitor(s).";
            return false;
        }

        target = monitors[selector.MonitorIndex];
        return true;
    }

    internal static bool TryApply(
        Window window,
        EditorStartupMonitorSelector selector,
        out string? error)
    {
        ArgumentNullException.ThrowIfNull(window);
        error = null;

        if (!TryEnumerate(out IReadOnlyList<EditorMonitorWorkArea> monitors, out error) ||
            !TryResolve(monitors, selector, out EditorMonitorWorkArea target, out error))
        {
            return false;
        }

        nint handle = new WindowInteropHelper(window).Handle;
        if (handle == 0)
        {
            error = "The editor window handle is not initialized.";
            return false;
        }
        if (!GetWindowRect(handle, out NativeRect current))
        {
            error = "Windows could not read the editor window bounds.";
            return false;
        }

        int width = Math.Clamp(
            Math.Max(1, current.Right - current.Left),
            1,
            target.Width);
        int height = Math.Clamp(
            Math.Max(1, current.Bottom - current.Top),
            1,
            target.Height);
        int x = target.Left + Math.Max(0, (target.Width - width) / 2);
        int y = target.Top + Math.Max(0, (target.Height - height) / 2);

        if (!SetWindowPos(
                handle,
                0,
                x,
                y,
                width,
                height,
                SwpNoZOrder | SwpNoActivate | SwpNoOwnerZOrder))
        {
            error =
                $"Windows could not place the editor on {target.DeviceName}.";
            return false;
        }

        return true;
    }

    private static bool TryEnumerate(
        out IReadOnlyList<EditorMonitorWorkArea> monitors,
        out string? error)
    {
        var result = new List<EditorMonitorWorkArea>();
        MonitorEnumProcedure callback = (
            nint monitor,
            nint _,
            ref NativeRect __,
            nint ___) =>
        {
            var info = new NativeMonitorInfo
            {
                Size = Marshal.SizeOf<NativeMonitorInfo>(),
                DeviceName = string.Empty
            };
            if (GetMonitorInfo(monitor, ref info))
            {
                result.Add(new EditorMonitorWorkArea(
                    string.IsNullOrWhiteSpace(info.DeviceName)
                        ? $"Monitor {result.Count}"
                        : info.DeviceName,
                    info.WorkArea.Left,
                    info.WorkArea.Top,
                    info.WorkArea.Right,
                    info.WorkArea.Bottom,
                    (info.Flags & MonitorInfoPrimary) != 0));
            }
            return true;
        };

        if (!EnumDisplayMonitors(0, 0, callback, 0))
        {
            monitors = Array.Empty<EditorMonitorWorkArea>();
            error = "Windows could not enumerate active monitors.";
            return false;
        }

        monitors = result;
        error = null;
        return true;
    }
}

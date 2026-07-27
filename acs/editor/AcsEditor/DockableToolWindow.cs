// SPDX-License-Identifier: Apache-2.0

using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Interop;
using System.Windows.Media;

namespace AcsEditor;

/// <summary>
/// Reusable owner-edge snap behavior for editor-owned floating tool windows.
/// Geometry is evaluated in Win32 physical pixels; WPF continues to own the
/// window's per-monitor-DPI layout.
/// </summary>
internal sealed class OwnedToolWindowSnapBehavior : IDisposable
{
    private const int WmMoving = 0x0216;
    private const uint MonitorDefaultToNearest = 0x00000002;
    private const uint SwpNoZOrder = 0x0004;
    private const uint SwpNoActivate = 0x0010;
    private const uint SwpNoOwnerZOrder = 0x0200;

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeRect
    {
        internal int Left;
        internal int Top;
        internal int Right;
        internal int Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeMonitorInfo
    {
        internal uint Size;
        internal NativeRect Monitor;
        internal NativeRect WorkArea;
        internal uint Flags;
    }

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetWindowRect(
        IntPtr hwnd,
        out NativeRect rect);

    [DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(IntPtr hwnd);

    [DllImport("user32.dll")]
    private static extern IntPtr MonitorFromRect(
        ref NativeRect rect,
        uint flags);

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetMonitorInfo(
        IntPtr monitor,
        ref NativeMonitorInfo info);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetWindowPos(
        IntPtr hwnd,
        IntPtr insertAfter,
        int x,
        int y,
        int width,
        int height,
        uint flags);

    private readonly Window _owner;
    private readonly Window _toolWindow;
    private readonly Action<string> _logWarning;
    private HwndSource? _source;
    private IntPtr _windowHandle;
    private bool _disposed;

    internal OwnedToolWindowSnapBehavior(
        Window owner,
        Window toolWindow,
        Action<string> logWarning)
    {
        ArgumentNullException.ThrowIfNull(owner);
        ArgumentNullException.ThrowIfNull(toolWindow);
        ArgumentNullException.ThrowIfNull(logWarning);
        _owner = owner;
        _toolWindow = toolWindow;
        _logWarning = logWarning;
    }

    internal void Attach()
    {
        if (_disposed || _source != null)
            return;
        _windowHandle = new WindowInteropHelper(_toolWindow).Handle;
        if (_windowHandle == IntPtr.Zero)
            return;
        _source = HwndSource.FromHwnd(_windowHandle);
        _source?.AddHook(WindowMessageHook);
        EnsureRestoredPlacementIsReachable();
    }

    public void Dispose()
    {
        if (_disposed)
            return;
        _disposed = true;
        _source?.RemoveHook(WindowMessageHook);
        _source = null;
        _windowHandle = IntPtr.Zero;
    }

    private IntPtr WindowMessageHook(
        IntPtr hwnd,
        int message,
        IntPtr wParam,
        IntPtr lParam,
        ref bool handled)
    {
        if (message == WmMoving)
            ApplyDpiAwareOwnerSnap(hwnd, lParam);
        return IntPtr.Zero;
    }

    private void ApplyDpiAwareOwnerSnap(
        IntPtr hwnd,
        IntPtr movingRectPointer)
    {
        if (movingRectPointer == IntPtr.Zero)
            return;
        IntPtr ownerHandle = new WindowInteropHelper(_owner).Handle;
        if (ownerHandle == IntPtr.Zero ||
            !GetWindowRect(ownerHandle, out NativeRect ownerRect))
        {
            return;
        }

        NativeRect movingRect =
            Marshal.PtrToStructure<NativeRect>(movingRectPointer);
        if (!TryPixelBounds(movingRect, out ToolWindowPixelBounds moving) ||
            !TryPixelBounds(ownerRect, out ToolWindowPixelBounds owner))
        {
            return;
        }

        uint dpi = Math.Max(96u, GetDpiForWindow(hwnd));
        ToolWindowPixelBounds workArea = default;
        if (TryGetNearestWorkArea(
                ref movingRect,
                out ToolWindowPixelBounds nearestWorkArea))
            workArea = nearestWorkArea;
        ToolWindowPixelBounds snapped = ToolPanelSnapPolicy.Snap(
            moving,
            owner,
            workArea,
            ToolPanelSnapPolicy.ThresholdPixels(dpi));
        if (workArea.IsValid)
            snapped = ToolPanelPlacementPolicy.ClampRestoredToWorkArea(
                snapped, workArea, dpi);
        if (snapped.Left == moving.Left && snapped.Top == moving.Top)
            return;
        if (snapped.Right is < int.MinValue or > int.MaxValue ||
            snapped.Bottom is < int.MinValue or > int.MaxValue)
        {
            return;
        }

        movingRect.Left = snapped.Left;
        movingRect.Top = snapped.Top;
        movingRect.Right = (int)snapped.Right;
        movingRect.Bottom = (int)snapped.Bottom;
        Marshal.StructureToPtr(movingRect, movingRectPointer, false);
    }

    private void EnsureRestoredPlacementIsReachable()
    {
        if (_windowHandle == IntPtr.Zero ||
            !GetWindowRect(_windowHandle, out NativeRect restoredRect) ||
            !TryPixelBounds(
                restoredRect,
                out ToolWindowPixelBounds restored) ||
            !TryGetNearestWorkArea(
                ref restoredRect,
                out ToolWindowPixelBounds workArea))
        {
            return;
        }

        ToolWindowPixelBounds clamped =
            ToolPanelPlacementPolicy.ClampRestoredToWorkArea(
                restored,
                workArea,
                Math.Max(96u, GetDpiForWindow(_windowHandle)));
        if (clamped.Left == restored.Left &&
            clamped.Top == restored.Top)
        {
            return;
        }
        if (clamped.Right is < int.MinValue or > int.MaxValue ||
            clamped.Bottom is < int.MinValue or > int.MaxValue)
        {
            _logWarning(
                "Tool window placement could not be represented safely in " +
                "the nearest monitor work area.");
            return;
        }
        if (!SetWindowPos(
                _windowHandle,
                IntPtr.Zero,
                clamped.Left,
                clamped.Top,
                clamped.Width,
                clamped.Height,
                SwpNoZOrder | SwpNoActivate | SwpNoOwnerZOrder))
        {
            _logWarning(
                "Tool window placement could not be clamped to the nearest " +
                "monitor work area.");
        }
    }

    private static bool TryGetNearestWorkArea(
        ref NativeRect source,
        out ToolWindowPixelBounds workArea)
    {
        IntPtr monitor = MonitorFromRect(
            ref source,
            MonitorDefaultToNearest);
        var info = new NativeMonitorInfo
        {
            Size = checked((uint)Marshal.SizeOf<NativeMonitorInfo>()),
        };
        if (monitor == IntPtr.Zero ||
            !GetMonitorInfo(monitor, ref info))
        {
            workArea = default;
            return false;
        }
        return TryPixelBounds(info.WorkArea, out workArea);
    }

    private static bool TryPixelBounds(
        NativeRect value,
        out ToolWindowPixelBounds bounds)
    {
        long width = (long)value.Right - value.Left;
        long height = (long)value.Bottom - value.Top;
        if (width is <= 0 or > int.MaxValue ||
            height is <= 0 or > int.MaxValue)
        {
            bounds = default;
            return false;
        }
        bounds = new ToolWindowPixelBounds(
            value.Left,
            value.Top,
            (int)width,
            (int)height);
        return true;
    }
}

/// <summary>
/// Owner-chromed window for one existing WPF tool panel. It never clones the
/// panel: the same FrameworkElement is transferred between its original dock
/// parent and this window's content host.
/// </summary>
internal sealed class DockableToolWindow : Window
{
    private readonly ContentControl _contentHost;
    private readonly Func<bool> _requestRedock;
    private readonly Action<Rect, bool> _savePlacement;
    private readonly Action<string> _logWarning;
    private readonly OwnedToolWindowSnapBehavior _snapBehavior;
    private bool _abandoned;

    internal DockableToolWindow(
        MainWindow owner,
        string title,
        FrameworkElement enabledSource,
        Rect placement,
        Func<bool> requestRedock,
        Action<Rect, bool> savePlacement,
        Action<string> logWarning)
    {
        ArgumentNullException.ThrowIfNull(owner);
        ArgumentException.ThrowIfNullOrWhiteSpace(title);
        ArgumentNullException.ThrowIfNull(enabledSource);
        ArgumentNullException.ThrowIfNull(requestRedock);
        ArgumentNullException.ThrowIfNull(savePlacement);
        ArgumentNullException.ThrowIfNull(logWarning);

        Owner = owner;
        Title = title;
        Width = placement.Width;
        Height = placement.Height;
        Left = placement.Left;
        Top = placement.Top;
        MinWidth = 360.0;
        MinHeight = 240.0;
        WindowStartupLocation = WindowStartupLocation.Manual;
        WindowStyle = WindowStyle.ToolWindow;
        ResizeMode = ResizeMode.CanResize;
        ShowInTaskbar = false;
        ShowActivated = ToolPanelWindowPolicy.ShowActivatedOnFloat;
        Topmost = ToolPanelWindowPolicy.IsTopmost;
        Background = new SolidColorBrush(Color.FromRgb(10, 14, 19));
        UseLayoutRounding = true;
        SnapsToDevicePixels = true;

        _requestRedock = requestRedock;
        _savePlacement = savePlacement;
        _logWarning = logWarning;

        var dockButton = new Button
        {
            Content = "Re-dock",
            MinWidth = 78.0,
            Height = 27.0,
            Margin = new Thickness(8.0, 5.0, 8.0, 5.0),
            Padding = new Thickness(10.0, 2.0, 10.0, 2.0),
            HorizontalAlignment = HorizontalAlignment.Right,
            ToolTip = "Return this tool to its original editor slot.",
            Visibility = ToolPanelWindowPolicy.RequiresExplicitDockAction
                ? Visibility.Visible
                : Visibility.Collapsed,
        };
        dockButton.Click += (_, _) => Close();

        var chrome = new DockPanel
        {
            Height = 37.0,
            LastChildFill = true,
            Background = new SolidColorBrush(Color.FromRgb(28, 34, 42)),
        };
        DockPanel.SetDock(dockButton, Dock.Right);
        chrome.Children.Add(dockButton);
        chrome.Children.Add(new TextBlock
        {
            Text = title,
            Margin = new Thickness(10.0, 0.0, 4.0, 0.0),
            VerticalAlignment = VerticalAlignment.Center,
            Foreground = new SolidColorBrush(Color.FromRgb(173, 190, 206)),
            FontSize = 11.0,
            FontWeight = FontWeights.SemiBold,
        });

        _contentHost = new ContentControl
        {
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            VerticalContentAlignment = VerticalAlignment.Stretch,
        };
        BindingOperations.SetBinding(
            _contentHost,
            IsEnabledProperty,
            new Binding(nameof(IsEnabled))
            {
                Source = enabledSource,
                Mode = BindingMode.OneWay,
            });

        var root = new Grid();
        root.RowDefinitions.Add(
            new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(
            new RowDefinition
            {
                Height = new GridLength(1.0, GridUnitType.Star),
            });
        Grid.SetRow(chrome, 0);
        Grid.SetRow(_contentHost, 1);
        root.Children.Add(chrome);
        root.Children.Add(_contentHost);
        Content = root;

        _snapBehavior = new OwnedToolWindowSnapBehavior(
            owner,
            this,
            logWarning);
        SourceInitialized += OnSourceInitialized;
        Closing += OnClosing;
        Closed += OnClosed;
    }

    internal Rect LastNormalBounds { get; private set; }

    internal Rect CaptureNormalBounds() => CurrentNormalBounds();

    internal bool TryReleaseToolContent(out FrameworkElement? content)
    {
        if (_contentHost.Content is not FrameworkElement current)
        {
            content = null;
            return false;
        }
        _contentHost.Content = null;
        content = current;
        return true;
    }

    internal bool OwnsToolContent(FrameworkElement content) =>
        ReferenceEquals(_contentHost.Content, content);

    internal bool TryRestoreToolContent(FrameworkElement content)
    {
        ArgumentNullException.ThrowIfNull(content);
        if (_contentHost.Content != null ||
            LogicalTreeHelper.GetParent(content) != null)
        {
            return false;
        }
        _contentHost.Content = content;
        return true;
    }

    internal bool CloseForOwner()
    {
        Close();
        return ToolPanelWindowPolicy.MayCompleteOwnerClose(!IsVisible);
    }

    internal void AbandonBeforeShow()
    {
        if (IsVisible)
            throw new InvalidOperationException(
                "A visible tool window cannot be abandoned.");
        _abandoned = true;
        DetachHandlers();
        BindingOperations.ClearBinding(
            _contentHost,
            IsEnabledProperty);
        Owner = null;
    }

    private void OnSourceInitialized(object? sender, EventArgs e) =>
        _snapBehavior.Attach();

    private void OnClosing(object? sender, CancelEventArgs e)
    {
        if (_abandoned)
            return;
        LastNormalBounds = CurrentNormalBounds();
        try
        {
            _savePlacement(LastNormalBounds, true);
        }
        catch (Exception error)
        {
            _logWarning(
                $"{Title} placement could not be saved: {error.Message}");
        }

        try
        {
            if (_requestRedock())
                return;
            e.Cancel = true;
            _logWarning(
                $"{Title} close was cancelled because its panel could " +
                "not be safely returned to the editor.");
        }
        catch (Exception error)
        {
            e.Cancel = true;
            _logWarning(
                $"{Title} close was cancelled: {error.Message}");
        }
    }

    private void OnClosed(object? sender, EventArgs e)
    {
        if (LastNormalBounds.IsEmpty)
            LastNormalBounds = CurrentNormalBounds();
        DetachHandlers();
        BindingOperations.ClearBinding(
            _contentHost,
            IsEnabledProperty);
    }

    private Rect CurrentNormalBounds()
    {
        Rect bounds = WindowState == WindowState.Normal
            ? new Rect(Left, Top, ActualWidth, ActualHeight)
            : RestoreBounds;
        return ToolPanelDockingContract.NormalizePlacementRect(
            bounds,
            new Rect(96.0, 72.0, 720.0, 480.0));
    }

    private void DetachHandlers()
    {
        _snapBehavior.Dispose();
        SourceInitialized -= OnSourceInitialized;
        Closing -= OnClosing;
        Closed -= OnClosed;
    }
}

/// <summary>
/// Single-owner transfer coordinator for a docked WPF panel.
/// </summary>
internal readonly record struct DockableToolHostSnapshot(
    string PanelId,
    ToolPanelDockState State,
    Rect Placement);

internal sealed class DockableToolHost
{
    private readonly MainWindow _owner;
    private readonly string _panelId;
    private readonly string _title;
    private readonly FrameworkElement _content;
    private readonly FrameworkElement _enabledSource;
    private readonly Panel _dockParent;
    private readonly int _dockIndex;
    private readonly Func<bool> _dockVisibility;
    private readonly Action<bool> _applyDockVisibility;
    private readonly Action<ToolPanelDockState> _stateChanged;
    private readonly Func<Rect> _loadPlacement;
    private readonly Action<Rect, bool> _savePlacement;
    private readonly Action<string> _logWarning;
    private DockableToolWindow? _window;
    private bool _dockVisibilityAfterRedock = true;
    private bool _preserveFloatingRestore;

    internal DockableToolHost(
        MainWindow owner,
        string panelId,
        string title,
        FrameworkElement content,
        FrameworkElement enabledSource,
        Func<bool> dockVisibility,
        Action<bool> applyDockVisibility,
        Action<ToolPanelDockState> stateChanged,
        Func<Rect> loadPlacement,
        Action<Rect, bool> savePlacement,
        Action<string> logWarning)
    {
        ArgumentNullException.ThrowIfNull(owner);
        if (!ToolPanelDockingContract.IsKnownPanelId(panelId))
            throw new ArgumentOutOfRangeException(nameof(panelId));
        ArgumentException.ThrowIfNullOrWhiteSpace(title);
        ArgumentNullException.ThrowIfNull(content);
        ArgumentNullException.ThrowIfNull(enabledSource);
        ArgumentNullException.ThrowIfNull(dockVisibility);
        ArgumentNullException.ThrowIfNull(applyDockVisibility);
        ArgumentNullException.ThrowIfNull(stateChanged);
        ArgumentNullException.ThrowIfNull(loadPlacement);
        ArgumentNullException.ThrowIfNull(savePlacement);
        ArgumentNullException.ThrowIfNull(logWarning);

        _owner = owner;
        _panelId = panelId;
        _title = title;
        _content = content;
        _enabledSource = enabledSource;
        _dockVisibility = dockVisibility;
        _applyDockVisibility = applyDockVisibility;
        _stateChanged = stateChanged;
        _loadPlacement = loadPlacement;
        _savePlacement = savePlacement;
        _logWarning = logWarning;
        _dockParent =
            LogicalTreeHelper.GetParent(content) as Panel ??
            throw new InvalidOperationException(
                $"{panelId} must start in a WPF Panel.");
        _dockIndex = _dockParent.Children.IndexOf(content);
        if (_dockIndex < 0)
            throw new InvalidOperationException(
                $"{panelId} was not found in its dock parent.");
    }

    internal string PanelId => _panelId;

    internal ToolPanelDockState State =>
        ToolPanelDockingContract.ResolveState(
            _window != null,
            _dockVisibility());

    internal bool IsFloating => _window != null;

    internal DockableToolHostSnapshot CaptureSnapshot() =>
        new(
            _panelId,
            State,
            _window?.CaptureNormalBounds() ?? _loadPlacement());

    internal bool TryToggleFloating() =>
        _window == null
            ? TryFloat()
            : RequestRedock(visibleAfterDock: true);

    internal bool TryFloat()
    {
        if (_window != null)
            return true;
        if (!ReferenceEquals(
                LogicalTreeHelper.GetParent(_content),
                _dockParent))
        {
            _logWarning(
                $"{_title} cannot float because another visual host owns it.");
            return false;
        }

        int currentIndex = _dockParent.Children.IndexOf(_content);
        if (currentIndex < 0)
            return false;
        bool wasVisible = _dockVisibility();
        DockableToolWindow? window = null;
        Rect placement = default;
        try
        {
            placement = _loadPlacement();
            window = new DockableToolWindow(
                _owner,
                _title,
                _enabledSource,
                placement,
                TryRedockFromWindow,
                _savePlacement,
                _logWarning);
            _applyDockVisibility(false);
            _dockParent.Children.RemoveAt(currentIndex);
            _content.Visibility = Visibility.Visible;
            if (!window.TryRestoreToolContent(_content))
            {
                throw new InvalidOperationException(
                    "The tool panel could not be attached to its floating host.");
            }
            window.Closed += OnWindowClosed;
            _window = window;
            _dockVisibilityAfterRedock = true;
            _preserveFloatingRestore = false;
        }
        catch (Exception error)
        {
            RollBackUnshownFloat(window, wasVisible, error);
            return false;
        }

        try
        {
            window.Show();
        }
        catch (Exception error)
        {
            if (!window.IsVisible)
            {
                RollBackUnshownFloat(window, wasVisible, error);
                return false;
            }

            PublishState(ToolPanelDockState.Floating);
            _logWarning(
                $"{_title} reported an error while opening but remains " +
                $"safely floating: {error.Message}");
            TrySavePlacement(placement, floating: true);
            return true;
        }

        if (!HasSingleVisualOwner(window))
        {
            _logWarning(
                $"{_title} entered an invalid visual ownership state after " +
                "being floated.");
        }
        PublishState(ToolPanelDockState.Floating);
        TrySavePlacement(placement, floating: true);
        return true;
    }

    internal bool HandleVisibilityRequest(bool visible)
    {
        if (_window == null)
            return false;
        if (visible)
        {
            PublishState(ToolPanelDockState.Floating);
            return true;
        }
        return RequestRedock(visibleAfterDock: false);
    }

    internal bool CloseForOwner()
    {
        if (_window == null)
            return true;
        _dockVisibilityAfterRedock = true;
        _preserveFloatingRestore = true;
        bool closed;
        try
        {
            closed = _window.CloseForOwner();
        }
        catch (Exception error)
        {
            closed = false;
            _logWarning(
                $"{_title} could not close with its owner: {error.Message}");
        }
        if (!closed)
        {
            _preserveFloatingRestore = false;
            PublishState(ToolPanelDockState.Floating);
        }
        return ToolPanelWindowPolicy.MayCompleteOwnerClose(closed);
    }

    internal bool ResetToDock()
        => TryRestoreState(ToolPanelDockState.Docked);

    internal bool TryRestoreState(ToolPanelDockState state)
    {
        if (!Enum.IsDefined(state))
            return false;
        if (state == ToolPanelDockState.Floating)
            return TryFloat();
        if (_window != null)
        {
            return RequestRedock(
                visibleAfterDock: state == ToolPanelDockState.Docked);
        }
        try
        {
            bool visible = state == ToolPanelDockState.Docked;
            _applyDockVisibility(visible);
            PublishState(state);
            return true;
        }
        catch (Exception error)
        {
            _logWarning(
                $"{_title} could not restore its {state} state: " +
                error.Message);
            return false;
        }
    }

    private bool RequestRedock(bool visibleAfterDock)
    {
        DockableToolWindow? window = _window;
        if (window == null)
            return true;
        _dockVisibilityAfterRedock = visibleAfterDock;
        try
        {
            window.Close();
        }
        catch (Exception error)
        {
            _dockVisibilityAfterRedock = true;
            PublishState(ToolPanelDockState.Floating);
            _logWarning(
                $"{_title} could not be re-docked: {error.Message}");
            return false;
        }
        bool closed = _window == null;
        if (!closed)
        {
            _dockVisibilityAfterRedock = true;
            PublishState(ToolPanelDockState.Floating);
        }
        return closed;
    }

    private bool TryRedockFromWindow()
    {
        DockableToolWindow? window = _window;
        if (window == null)
            return true;
        if (!window.TryReleaseToolContent(out FrameworkElement? content) ||
            !ReferenceEquals(content, _content))
        {
            PublishState(ToolPanelDockState.Floating);
            _logWarning(
                $"{_title} could not be detached from its floating host.");
            return false;
        }

        try
        {
            if (LogicalTreeHelper.GetParent(_content) != null)
                throw new InvalidOperationException(
                    "The tool panel already has a visual owner.");
            _dockParent.Children.Insert(
                Math.Min(_dockIndex, _dockParent.Children.Count),
                _content);
            _applyDockVisibility(_dockVisibilityAfterRedock);
            PublishState(
                _dockVisibilityAfterRedock
                    ? ToolPanelDockState.Docked
                    : ToolPanelDockState.Hidden);
            return true;
        }
        catch (Exception error)
        {
            if (ReferenceEquals(
                    LogicalTreeHelper.GetParent(_content),
                    _dockParent))
            {
                _dockParent.Children.Remove(_content);
            }
            _content.Visibility = Visibility.Visible;
            if (window.TryRestoreToolContent(_content) &&
                HasSingleVisualOwner(window))
            {
                PublishState(ToolPanelDockState.Floating);
                _logWarning(
                    $"{_title} could not be re-docked and remains floating: " +
                    error.Message);
                return false;
            }

            try
            {
                if (LogicalTreeHelper.GetParent(_content) == null)
                {
                    _dockParent.Children.Insert(
                        Math.Min(_dockIndex, _dockParent.Children.Count),
                        _content);
                }
                if (!ReferenceEquals(
                        LogicalTreeHelper.GetParent(_content),
                        _dockParent))
                {
                    throw new InvalidOperationException(
                        "The panel could not be recovered by either visual host.");
                }
                _applyDockVisibility(_dockVisibilityAfterRedock);
                PublishState(
                    _dockVisibilityAfterRedock
                        ? ToolPanelDockState.Docked
                        : ToolPanelDockState.Hidden);
                _logWarning(
                    $"{_title} recovered in its dock after a floating-host " +
                    $"rollback failure: {error.Message}");
                return true;
            }
            catch (Exception fallbackError)
            {
                bool inDock = ReferenceEquals(
                    LogicalTreeHelper.GetParent(_content),
                    _dockParent);
                bool inFloat = window.OwnsToolContent(_content);
                if (ToolPanelDockingContract.CanOwnSingleVisual(
                        inDock,
                        inFloat))
                {
                    PublishState(
                        inFloat
                            ? ToolPanelDockState.Floating
                            : ToolPanelDockingContract.ResolveState(
                                floating: false,
                                _dockVisibilityAfterRedock));
                }
                _logWarning(
                    $"{_title} visual-owner recovery failed after re-dock " +
                    $"error '{error.Message}': {fallbackError.Message}");
                return inDock;
            }
        }
    }

    private void OnWindowClosed(object? sender, EventArgs e)
    {
        if (sender is not DockableToolWindow window ||
            !ReferenceEquals(window, _window))
        {
            return;
        }
        window.Closed -= OnWindowClosed;
        _window = null;
        if (!_preserveFloatingRestore)
        {
            TrySavePlacement(
                window.LastNormalBounds,
                floating: false);
        }
        _preserveFloatingRestore = false;
        PublishState(
            _dockVisibility()
                ? ToolPanelDockState.Docked
                : ToolPanelDockState.Hidden);
    }

    private void RollBackUnshownFloat(
        DockableToolWindow? window,
        bool wasVisible,
        Exception error)
    {
        if (window?.IsVisible == true)
        {
            PublishState(ToolPanelDockState.Floating);
            _logWarning(
                $"{_title} could not complete its float transition but " +
                $"remains visible: {error.Message}");
            return;
        }

        bool releasedFromWindow = window == null;
        if (window != null)
        {
            if (window.OwnsToolContent(_content))
            {
                releasedFromWindow =
                    window.TryReleaseToolContent(out FrameworkElement? content) &&
                    ReferenceEquals(content, _content);
                if (!releasedFromWindow)
                {
                    _window = window;
                    PublishState(ToolPanelDockState.Floating);
                    _logWarning(
                        $"{_title} float rollback could not release its " +
                        $"visual content: {error.Message}");
                    return;
                }
            }
            else
            {
                releasedFromWindow = true;
            }
        }

        bool recovered = false;
        try
        {
            if (LogicalTreeHelper.GetParent(_content) == null)
            {
                _dockParent.Children.Insert(
                    Math.Min(_dockIndex, _dockParent.Children.Count),
                    _content);
            }
            recovered = ReferenceEquals(
                LogicalTreeHelper.GetParent(_content),
                _dockParent);
            if (recovered)
                _applyDockVisibility(wasVisible);
        }
        catch (Exception rollbackError)
        {
            _logWarning(
                $"{_title} dock rollback also failed: {rollbackError.Message}");
        }

        if (recovered)
        {
            _window = null;
            if (window != null)
            {
                window.Closed -= OnWindowClosed;
                try
                {
                    window.AbandonBeforeShow();
                }
                catch (Exception abandonError)
                {
                    _logWarning(
                        $"{_title} floating host cleanup failed: " +
                        abandonError.Message);
                }
            }
            PublishState(
                ToolPanelDockingContract.ResolveState(
                    floating: false,
                    wasVisible));
            _logWarning(
                $"{_title} could not be floated: {error.Message}");
            return;
        }

        if (window != null &&
            releasedFromWindow &&
            LogicalTreeHelper.GetParent(_content) == null &&
            window.TryRestoreToolContent(_content))
        {
            _window = window;
            window.Closed -= OnWindowClosed;
            window.Closed += OnWindowClosed;
            try
            {
                window.Show();
                PublishState(ToolPanelDockState.Floating);
                _logWarning(
                    $"{_title} dock rollback failed, so the panel remains " +
                    $"floating: {error.Message}");
                return;
            }
            catch (Exception showError)
            {
                _logWarning(
                    $"{_title} emergency floating recovery also failed: " +
                    showError.Message);
            }
        }

        _window = window?.OwnsToolContent(_content) == true
            ? window
            : null;
        _logWarning(
            $"{_title} could not be floated and its visual owner could not " +
            $"be recovered: {error.Message}");
    }

    private bool HasSingleVisualOwner(DockableToolWindow window) =>
        ToolPanelDockingContract.CanOwnSingleVisual(
            ReferenceEquals(
                LogicalTreeHelper.GetParent(_content),
                _dockParent),
            window.OwnsToolContent(_content));

    private void TrySavePlacement(Rect placement, bool floating)
    {
        try
        {
            _savePlacement(placement, floating);
        }
        catch (Exception error)
        {
            _logWarning(
                $"{_title} placement could not be saved: {error.Message}");
        }
    }

    private void PublishState(ToolPanelDockState state)
    {
        try
        {
            _stateChanged(state);
        }
        catch (Exception error)
        {
            _logWarning(
                $"{_title} state UI could not be refreshed: {error.Message}");
        }
    }
}

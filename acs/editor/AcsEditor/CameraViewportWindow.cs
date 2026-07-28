// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;

namespace AcsEditor;

internal sealed record CameraViewChoice(
    int NodeId,
    string NodeName,
    string StableCameraId,
    bool IsEnabled,
    bool IsAuthoredActive,
    bool IsPreview,
    int Priority)
{
    public string DisplayName =>
        $"{NodeName}  ·  {StableCameraId}" +
        (IsEnabled ? "" : "  (Disabled)") +
        (IsPreview ? "  ·  Preview" :
         IsAuthoredActive ? "  ·  Active" : "");
}

internal sealed record CameraViewSlotView(
    ulong SlotId,
    ulong NativeRequestId,
    int NodeId,
    string NodeName,
    string StableCameraId,
    bool IsCameraAvailable,
    bool IsEnabled,
    bool IsSelected,
    bool IsPresenter,
    uint RequestedWidth,
    uint RequestedHeight,
    uint TargetGeneration,
    uint HistoryGeneration)
{
    public string Header =>
        NodeName +
        (IsCameraAvailable
            ? ""
            : IsEnabled ? " (Stale)" : " (Unavailable)");

    public string Diagnostic =>
        $"{RequestedWidth}x{RequestedHeight}" +
        (NativeRequestId == 0
            ? " · legacy override"
            : $" · target {TargetGeneration}, history {HistoryGeneration}");
}

/// <summary>
/// Owns chrome for the one live native Camera/Game View. The renderer child is
/// reparented into this HWND; this class never creates another EngineViewport.
/// </summary>
internal sealed class CameraViewportWindow : Window
{
    private const int WmMoving = 0x0216;
    private const int WmEnterSizeMove = 0x0231;
    private const int WmExitSizeMove = 0x0232;
    private const int WmDpiChanged = 0x02E0;
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
    private static extern bool GetClientRect(IntPtr hwnd, out NativeRect rect);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetWindowRect(IntPtr hwnd, out NativeRect rect);

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

    private readonly EngineViewport _viewport;
    private readonly Func<IReadOnlyList<CameraViewChoice>> _cameraProvider;
    private readonly Func<IReadOnlyList<CameraViewSlotView>> _slotProvider;
    private readonly Func<int, string, bool> _pinCamera;
    private readonly Func<ulong, bool> _activateSlot;
    private readonly Func<ulong, bool> _closeSlot;
    private readonly Func<bool> _refreshSlotsFromScene;
    private readonly Func<int?> _previewNodeProvider;
    private readonly Action _clearPreview;
    private readonly Func<uint, uint, bool> _resizeRequest;
    private readonly Action<string> _logWarning;
    private readonly ComboBox _cameraSelector;
    private readonly TabControl _slotTabs;
    private readonly TextBlock _status;
    private readonly FrameworkElement _toolbar;
    private readonly DispatcherTimer _healthTimer;
    private HwndSource? _source;
    private IntPtr _windowHandle;
    private bool _updatingSelector;
    private bool _updatingTabs;
    private bool _surfaceAttached;
    private bool _suppressPlacementSave;
    private string _stableCameraId;

    internal CameraViewportWindow(
        MainWindow owner,
        EngineViewport viewport,
        string stableCameraId,
        CameraViewPlacementState placement,
        Func<IReadOnlyList<CameraViewChoice>> cameraProvider,
        Func<IReadOnlyList<CameraViewSlotView>> slotProvider,
        Func<int, string, bool> pinCamera,
        Func<ulong, bool> activateSlot,
        Func<ulong, bool> closeSlot,
        Func<bool> refreshSlotsFromScene,
        Func<int?> previewNodeProvider,
        Action clearPreview,
        Func<uint, uint, bool> resizeRequest,
        Action<string> logWarning)
    {
        ArgumentNullException.ThrowIfNull(owner);
        ArgumentNullException.ThrowIfNull(viewport);
        ArgumentNullException.ThrowIfNull(placement);
        ArgumentNullException.ThrowIfNull(cameraProvider);
        ArgumentNullException.ThrowIfNull(slotProvider);
        ArgumentNullException.ThrowIfNull(pinCamera);
        ArgumentNullException.ThrowIfNull(activateSlot);
        ArgumentNullException.ThrowIfNull(closeSlot);
        ArgumentNullException.ThrowIfNull(refreshSlotsFromScene);
        ArgumentNullException.ThrowIfNull(previewNodeProvider);
        ArgumentNullException.ThrowIfNull(clearPreview);
        ArgumentNullException.ThrowIfNull(resizeRequest);
        ArgumentNullException.ThrowIfNull(logWarning);

        _viewport = viewport;
        _cameraProvider = cameraProvider;
        _slotProvider = slotProvider;
        _pinCamera = pinCamera;
        _activateSlot = activateSlot;
        _closeSlot = closeSlot;
        _refreshSlotsFromScene = refreshSlotsFromScene;
        _previewNodeProvider = previewNodeProvider;
        _clearPreview = clearPreview;
        _resizeRequest = resizeRequest;
        _logWarning = logWarning;
        _stableCameraId = stableCameraId;

        Title = "Camera View";
        Width = placement.Width;
        Height = placement.Height;
        Left = placement.Left;
        Top = placement.Top;
        MinWidth = 420.0;
        MinHeight = 280.0;
        WindowStartupLocation = WindowStartupLocation.Manual;
        WindowStyle = WindowStyle.ToolWindow;
        ResizeMode = ResizeMode.CanResize;
        ShowInTaskbar = false;
        ShowActivated = false;
        Background = new SolidColorBrush(Color.FromRgb(10, 14, 19));
        UseLayoutRounding = true;
        SnapsToDevicePixels = true;

        _cameraSelector = new ComboBox
        {
            Width = 300.0,
            Height = 27.0,
            Margin = new Thickness(6.0, 5.0, 8.0, 6.0),
            DisplayMemberPath = nameof(CameraViewChoice.DisplayName),
            VerticalContentAlignment = VerticalAlignment.Center,
            ToolTip =
                "Add or focus a logical Camera View slot. Up to eight native " +
                "request leases can be retained, while exactly one shared " +
                "presenter remains live.",
        };
        _cameraSelector.SelectionChanged += OnCameraSelectionChanged;
        _cameraSelector.DropDownOpened += OnCameraDropDownOpened;

        _slotTabs = new TabControl
        {
            Height = 34.0,
            Background = Brushes.Transparent,
            BorderThickness = new Thickness(0.0),
            Padding = new Thickness(0.0),
            ToolTip =
                "Logical camera leases retain independent requested extent " +
                "and temporal-history generations. Selecting a tab transfers " +
                "the one shared presenter.",
        };
        _slotTabs.SelectionChanged += OnCameraSlotSelectionChanged;
        var slotScroller = new ScrollViewer
        {
            Content = _slotTabs,
            Margin = new Thickness(8.0, 4.0, 8.0, 0.0),
            HorizontalScrollBarVisibility =
                ScrollBarVisibility.Auto,
            VerticalScrollBarVisibility =
                ScrollBarVisibility.Disabled,
            CanContentScroll = true,
        };

        var dockButton = new Button
        {
            Content = "Re-dock",
            MinWidth = 78.0,
            Height = 27.0,
            Margin = new Thickness(0.0, 5.0, 8.0, 6.0),
            Padding = new Thickness(10.0, 2.0, 10.0, 2.0),
            ToolTip = "Return the same live renderer surface to the main editor.",
        };
        dockButton.Click += (_, _) => Close();

        _status = new TextBlock
        {
            Margin = new Thickness(8.0, 0.0, 10.0, 1.0),
            VerticalAlignment = VerticalAlignment.Center,
            Foreground = new SolidColorBrush(Color.FromRgb(143, 166, 187)),
            Text = "Preparing live camera surface…",
            TextTrimming = TextTrimming.CharacterEllipsis,
        };

        var addCameraLabel = new TextBlock
        {
            Text = "Add camera",
            Margin = new Thickness(10.0, 0.0, 0.0, 1.0),
            VerticalAlignment = VerticalAlignment.Center,
            Foreground = new SolidColorBrush(Color.FromRgb(183, 197, 211)),
            FontWeight = FontWeights.SemiBold,
        };
        var addCameraPanel = new StackPanel
        {
            Orientation = Orientation.Horizontal,
        };
        addCameraPanel.Children.Add(addCameraLabel);
        addCameraPanel.Children.Add(_cameraSelector);

        var actionRow = new DockPanel
        {
            LastChildFill = true,
        };
        DockPanel.SetDock(addCameraPanel, Dock.Left);
        actionRow.Children.Add(addCameraPanel);
        DockPanel.SetDock(dockButton, Dock.Right);
        actionRow.Children.Add(dockButton);
        actionRow.Children.Add(_status);

        var toolbar = new Grid
        {
            Height = 76.0,
            Background = new SolidColorBrush(Color.FromRgb(28, 34, 42)),
        };
        toolbar.RowDefinitions.Add(new RowDefinition
        {
            Height = new GridLength(38.0),
        });
        toolbar.RowDefinitions.Add(new RowDefinition
        {
            Height = new GridLength(38.0),
        });
        Grid.SetRow(slotScroller, 0);
        Grid.SetRow(actionRow, 1);
        toolbar.Children.Add(slotScroller);
        toolbar.Children.Add(actionRow);
        _toolbar = toolbar;

        var surfacePlaceholder = new Border
        {
            Background = new SolidColorBrush(Color.FromRgb(7, 10, 14)),
            Child = new TextBlock
            {
                Text = "LIVE CAMERA VIEW",
                Foreground = new SolidColorBrush(Color.FromRgb(62, 76, 90)),
                FontSize = 13.0,
                FontWeight = FontWeights.SemiBold,
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
            },
        };
        var root = new Grid();
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        Grid.SetRow(toolbar, 0);
        Grid.SetRow(surfacePlaceholder, 1);
        root.Children.Add(toolbar);
        root.Children.Add(surfacePlaceholder);
        Content = root;

        _healthTimer = new DispatcherTimer(
            TimeSpan.FromSeconds(1.0),
            DispatcherPriority.Background,
            (_, _) => ValidatePreviewHealth(),
            Dispatcher);

        SourceInitialized += OnSourceInitialized;
        Loaded += OnLoaded;
        SizeChanged += OnWindowSizeChanged;
        Closing += OnClosing;
        Closed += OnClosed;
        // Owner is the only external lifetime edge created by construction.
        // Assign it last so an earlier constructor failure leaves the partial
        // window collectible and cannot register it in the editor's OwnedWindows.
        Owner = owner;
    }

    internal bool HasLiveSurface => _surfaceAttached;

    internal string StableCameraId => _stableCameraId;

    internal event EventHandler? LiveSurfaceAttached;

    internal event EventHandler? LiveSurfaceDocked;

    internal event EventHandler? LiveSurfaceAttachFailed;

    internal bool PinCamera(
        int cameraNodeId,
        string stableCameraId)
    {
        if (cameraNodeId < 0 ||
            !CameraAuthoringContract.IsValidStableCameraId(stableCameraId))
            return false;
        if (!_pinCamera(cameraNodeId, stableCameraId))
            return false;
        _stableCameraId = stableCameraId;
        RefreshCameraUi();
        return true;
    }

    internal void RefreshFromScene()
    {
        if (!IsLoaded)
            return;
        if (!_refreshSlotsFromScene())
        {
            _status.Text =
                "Camera slots could not be rebound after scene replacement.";
        }
        RefreshCameraUi();
    }

    internal bool CloseForOwner()
    {
        Close();
        return !IsVisible &&
               !_surfaceAttached &&
               !_viewport.IsRenderSurfaceFloating;
    }

    internal bool TryRollbackFailedOpen()
    {
        _suppressPlacementSave = true;
        try
        {
            Close();
        }
        finally
        {
            _suppressPlacementSave = false;
        }
        return !IsVisible &&
               !_surfaceAttached &&
               !_viewport.IsRenderSurfaceFloating;
    }

    private void OnSourceInitialized(object? sender, EventArgs e)
    {
        _windowHandle = new WindowInteropHelper(this).Handle;
        _source = HwndSource.FromHwnd(_windowHandle);
        _source?.AddHook(WindowMessageHook);
        EnsureRestoredPlacementIsReachable();
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        RefreshCameraUi();
        _ = Dispatcher.BeginInvoke(
            DispatcherPriority.Loaded,
            new Action(() =>
            {
                if (!IsVisible || _windowHandle == IntPtr.Zero)
                    return;
                if (!TryUpdateSurfaceBounds(attachIfNeeded: true))
                {
                    _status.Text = "Could not attach the live renderer surface.";
                    _logWarning(
                        "Camera View could not reparent the existing renderer surface; " +
                        "no secondary renderer was created.");
                    LiveSurfaceAttachFailed?.Invoke(this, EventArgs.Empty);
                    Close();
                    return;
                }
                UpdateLiveStatus();
                LiveSurfaceAttached?.Invoke(this, EventArgs.Empty);
                _healthTimer.Start();
            }));
    }

    private void OnWindowSizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (_surfaceAttached)
            _ = TryUpdateSurfaceBounds(attachIfNeeded: false);
    }

    private void OnCameraSelectionChanged(
        object sender,
        SelectionChangedEventArgs e)
    {
        if (_updatingSelector ||
            _cameraSelector.SelectedItem is not CameraViewChoice choice)
        {
            return;
        }
        if (!choice.IsEnabled)
        {
            _status.Text = "Disabled cameras cannot drive Game View.";
            RefreshCameraUi();
            return;
        }
        if (!_pinCamera(choice.NodeId, choice.StableCameraId))
        {
            _status.Text =
                "Camera changed, is unavailable, or the logical slot limit was reached.";
            RefreshCameraUi();
            return;
        }

        _stableCameraId = choice.StableCameraId;
        RefreshCameraUi();
    }

    private void OnCameraDropDownOpened(object? sender, EventArgs e) =>
        RefreshCameraUi();

    private void OnCameraSlotSelectionChanged(
        object sender,
        SelectionChangedEventArgs e)
    {
        if (_updatingTabs ||
            _slotTabs.SelectedItem is not TabItem tab ||
            tab.Tag is not ulong slotId)
        {
            return;
        }
        CameraViewSlotView? slot = _slotProvider().FirstOrDefault(
            candidate => candidate.SlotId == slotId);
        if (slot == null || !slot.IsCameraAvailable)
        {
            _status.Text =
                "This logical camera slot is stale or unavailable.";
            RefreshCameraUi();
            return;
        }
        if (!_activateSlot(slotId))
        {
            _status.Text =
                "The shared presenter could not switch to this camera slot.";
            RefreshCameraUi();
            return;
        }
        _stableCameraId = slot.StableCameraId;
        RefreshCameraUi();
    }

    private void OnCameraSlotClose(
        object sender,
        RoutedEventArgs e)
    {
        e.Handled = true;
        if (sender is not Button { Tag: ulong slotId })
            return;
        if (!_closeSlot(slotId))
        {
            _status.Text =
                "The logical camera slot could not be released safely.";
            RefreshCameraUi();
            return;
        }
        IReadOnlyList<CameraViewSlotView> remaining = _slotProvider();
        if (remaining.Count == 0)
        {
            Close();
            return;
        }
        RefreshCameraUi();
    }

    private void RefreshCameraUi()
    {
        IReadOnlyList<CameraViewChoice> choices;
        IReadOnlyList<CameraViewSlotView> slots;
        try
        {
            choices = _cameraProvider();
            slots = _slotProvider();
        }
        catch (Exception error)
        {
            _status.Text = "Camera enumeration failed.";
            _logWarning("Camera View enumeration failed: " + error.Message);
            return;
        }

        _updatingSelector = true;
        try
        {
            _cameraSelector.ItemsSource = choices;
            _cameraSelector.SelectedItem = null;
            _cameraSelector.IsEnabled =
                choices.Any(static camera => camera.IsEnabled);
        }
        finally
        {
            _updatingSelector = false;
        }

        _updatingTabs = true;
        try
        {
            _slotTabs.Items.Clear();
            TabItem? selectedTab = null;
            foreach (CameraViewSlotView slot in slots)
            {
                var label = new TextBlock
                {
                    Text = slot.Header,
                    MaxWidth = 180.0,
                    VerticalAlignment = VerticalAlignment.Center,
                    TextTrimming = TextTrimming.CharacterEllipsis,
                };
                var close = new Button
                {
                    Content = "×",
                    Tag = slot.SlotId,
                    Width = 20.0,
                    Height = 20.0,
                    Padding = new Thickness(0.0),
                    Margin = new Thickness(7.0, 0.0, 0.0, 0.0),
                    ToolTip = "Close this logical camera slot.",
                    Focusable = false,
                };
                close.Click += OnCameraSlotClose;
                var header = new StackPanel
                {
                    Orientation = Orientation.Horizontal,
                };
                header.Children.Add(label);
                header.Children.Add(close);
                var tab = new TabItem
                {
                    Tag = slot.SlotId,
                    Header = header,
                    ToolTip =
                        $"{slot.StableCameraId}\n{slot.Diagnostic}\n" +
                        "One shared presenter; no dedicated offscreen target.",
                    Padding = new Thickness(9.0, 3.0, 5.0, 3.0),
                    MinWidth = 92.0,
                };
                _slotTabs.Items.Add(tab);
                if (slot.IsSelected)
                    selectedTab = tab;
            }
            _slotTabs.SelectedItem = selectedTab;
        }
        finally
        {
            _updatingTabs = false;
        }

        CameraViewSlotView? selected =
            slots.FirstOrDefault(static slot => slot.IsSelected);
        if (selected == null)
        {
            Title = "Camera View";
            _status.Text =
                choices.Count == 0
                    ? "No authored cameras in this scene."
                    : "No logical camera slot is selected.";
            return;
        }
        _stableCameraId = selected.StableCameraId;
        Title = $"Camera View — {selected.NodeName}";
        UpdateLiveStatus(slots, selected);
    }

    private void UpdateLiveStatus()
    {
        IReadOnlyList<CameraViewSlotView> slots = _slotProvider();
        CameraViewSlotView? selected =
            slots.FirstOrDefault(static slot => slot.IsSelected);
        UpdateLiveStatus(slots, selected);
    }

    private void UpdateLiveStatus(
        IReadOnlyList<CameraViewSlotView> slots,
        CameraViewSlotView? selected)
    {
        if (selected == null)
        {
            _status.Text = "No logical camera slot is selected.";
            return;
        }
        if (!selected.IsCameraAvailable)
        {
            _status.Text =
                "Selected slot is unavailable; its stable identity is retained.";
            return;
        }
        if (!selected.IsPresenter)
        {
            _status.Text =
                "Selected slot is waiting for the one shared presenter.";
            return;
        }
        int capacity = slots.Any(
            static slot => slot.NativeRequestId != 0)
                ? CameraViewSlotPolicy.MaximumLogicalSlots
                : 1;
        _status.Text =
            $"Live · presenter 1/1 · slots {slots.Count}/" +
            $"{capacity} · " +
            selected.Diagnostic;
    }

    private void ValidatePreviewHealth()
    {
        IReadOnlyList<CameraViewSlotView> slots = _slotProvider();
        CameraViewSlotView? selected =
            slots.FirstOrDefault(static slot => slot.IsSelected);
        if (selected == null)
            return;
        int? previewNode = _previewNodeProvider();
        if (selected.IsPresenter &&
            selected.IsCameraAvailable &&
            previewNode == selected.NodeId)
        {
            UpdateLiveStatus(slots, selected);
            return;
        }
        _status.Text =
            selected.IsCameraAvailable
                ? "Shared presenter health check failed; switch tabs to retry."
                : "Selected slot is unavailable; stable identity is retained.";
    }

    private bool TryUpdateSurfaceBounds(bool attachIfNeeded)
    {
        if (_windowHandle == IntPtr.Zero ||
            !GetClientRect(_windowHandle, out NativeRect client))
        {
            return false;
        }

        uint dpi = Math.Max(96u, GetDpiForWindow(_windowHandle));
        int toolbarPixels = Math.Max(
            1,
            checked((int)Math.Ceiling(
                Math.Max(1.0, _toolbar.ActualHeight) * dpi / 96.0)));
        int width = Math.Max(1, client.Right - client.Left);
        int height = Math.Max(1, client.Bottom - client.Top - toolbarPixels);
        if (!_resizeRequest(
                checked((uint)width),
                checked((uint)height)))
        {
            _status.Text = "Camera render request rejected this extent.";
            return false;
        }
        var bounds = new Int32Rect(0, toolbarPixels, width, height);

        bool succeeded = !_surfaceAttached && attachIfNeeded
            ? _viewport.TryFloatRenderSurface(_windowHandle, bounds)
            : _viewport.UpdateFloatingRenderSurfaceBounds(bounds);
        if (succeeded || _viewport.IsRenderSurfaceFloating)
            _surfaceAttached = true;
        return succeeded;
    }

    private IntPtr WindowMessageHook(
        IntPtr hwnd,
        int message,
        IntPtr wParam,
        IntPtr lParam,
        ref bool handled)
    {
        switch (message)
        {
        case WmEnterSizeMove:
            _viewport.PauseRenderPumpForWindowInteraction();
            break;
        case WmExitSizeMove:
            _viewport.ResumeRenderPumpAfterWindowInteraction();
            _ = Dispatcher.BeginInvoke(
                DispatcherPriority.Loaded,
                new Action(() => _ = TryUpdateSurfaceBounds(attachIfNeeded: false)));
            break;
        case WmDpiChanged:
            _ = Dispatcher.BeginInvoke(
                DispatcherPriority.Loaded,
                new Action(() => _ = TryUpdateSurfaceBounds(attachIfNeeded: false)));
            break;
        case WmMoving:
            ApplyDpiAwareOwnerSnap(hwnd, lParam);
            break;
        }
        return IntPtr.Zero;
    }

    private void ApplyDpiAwareOwnerSnap(IntPtr hwnd, IntPtr movingRectPointer)
    {
        if (movingRectPointer == IntPtr.Zero)
            return;
        IntPtr ownerHandle = new WindowInteropHelper(Owner).Handle;
        if (ownerHandle == IntPtr.Zero ||
            !GetWindowRect(ownerHandle, out NativeRect ownerRect))
        {
            return;
        }

        NativeRect movingRect =
            Marshal.PtrToStructure<NativeRect>(movingRectPointer);
        if (!TryPixelBounds(movingRect, out CameraViewPixelBounds moving) ||
            !TryPixelBounds(ownerRect, out CameraViewPixelBounds owner))
        {
            return;
        }
        uint dpi = Math.Max(96u, GetDpiForWindow(hwnd));
        CameraViewPixelBounds snapped = CameraViewSnapPolicy.Snap(
            moving,
            owner,
            CameraViewSnapPolicy.ThresholdPixels(dpi));
        IntPtr monitor = MonitorFromRect(
            ref movingRect,
            MonitorDefaultToNearest);
        var monitorInfo = new NativeMonitorInfo
        {
            Size = checked((uint)Marshal.SizeOf<NativeMonitorInfo>()),
        };
        if (monitor != IntPtr.Zero &&
            GetMonitorInfo(monitor, ref monitorInfo))
        {
            if (!TryPixelBounds(
                    monitorInfo.WorkArea,
                    out CameraViewPixelBounds workArea))
            {
                return;
            }
            snapped = CameraViewPlacementPolicy.ClampRestoredToWorkArea(
                snapped,
                workArea,
                dpi);
        }
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
                out CameraViewPixelBounds restored))
        {
            return;
        }

        IntPtr monitor = MonitorFromRect(
            ref restoredRect,
            MonitorDefaultToNearest);
        var monitorInfo = new NativeMonitorInfo
        {
            Size = checked((uint)Marshal.SizeOf<NativeMonitorInfo>()),
        };
        if (monitor == IntPtr.Zero ||
            !GetMonitorInfo(monitor, ref monitorInfo) ||
            !TryPixelBounds(
                monitorInfo.WorkArea,
                out CameraViewPixelBounds workArea))
        {
            return;
        }

        CameraViewPixelBounds clamped =
            CameraViewPlacementPolicy.ClampRestoredToWorkArea(
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
                "Camera View placement could not be represented safely in " +
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
                "Camera View placement could not be clamped to the nearest " +
                "monitor work area.");
        }
    }

    private static bool TryPixelBounds(
        NativeRect value,
        out CameraViewPixelBounds bounds)
    {
        long width = (long)value.Right - value.Left;
        long height = (long)value.Bottom - value.Top;
        if (width is <= 0 or > int.MaxValue ||
            height is <= 0 or > int.MaxValue)
        {
            bounds = default;
            return false;
        }
        bounds = new CameraViewPixelBounds(
            value.Left,
            value.Top,
            (int)width,
            (int)height);
        return true;
    }

    private void OnClosing(object? sender, CancelEventArgs e)
    {
        if (!_suppressPlacementSave)
            SavePlacement();
        _healthTimer.Stop();
        if (_surfaceAttached && !_viewport.TryDockRenderSurface())
        {
            e.Cancel = true;
            _healthTimer.Start();
            _status.Text = "Re-dock failed; Camera View remains open.";
            _logWarning(
                "Camera View close was cancelled because the live renderer " +
                "surface could not be safely returned to the editor.");
            return;
        }
        _surfaceAttached = false;
        LiveSurfaceDocked?.Invoke(this, EventArgs.Empty);
    }

    private void OnClosed(object? sender, EventArgs e)
    {
        _healthTimer.Stop();
        _source?.RemoveHook(WindowMessageHook);
        _source = null;
        _windowHandle = IntPtr.Zero;
        _cameraSelector.SelectionChanged -= OnCameraSelectionChanged;
        _cameraSelector.DropDownOpened -= OnCameraDropDownOpened;
        _slotTabs.SelectionChanged -= OnCameraSlotSelectionChanged;
        foreach (TabItem tab in _slotTabs.Items.OfType<TabItem>())
        {
            if (tab.Header is StackPanel header)
            {
                foreach (Button close in header.Children.OfType<Button>())
                    close.Click -= OnCameraSlotClose;
            }
        }
        _slotTabs.Items.Clear();
        SourceInitialized -= OnSourceInitialized;
        Loaded -= OnLoaded;
        SizeChanged -= OnWindowSizeChanged;
        Closing -= OnClosing;
        Closed -= OnClosed;
        LiveSurfaceAttached = null;
        LiveSurfaceDocked = null;
        LiveSurfaceAttachFailed = null;
    }

    private void SavePlacement()
    {
        try
        {
            Rect bounds = WindowState == WindowState.Normal
                ? new Rect(Left, Top, ActualWidth, ActualHeight)
                : RestoreBounds;
            if (!CameraViewPlacementStore.TrySave(new CameraViewPlacementState
            {
                Left = bounds.Left,
                Top = bounds.Top,
                Width = bounds.Width,
                Height = bounds.Height,
                StableCameraId = _stableCameraId,
            }, out string warning))
            {
                _logWarning(
                    "Camera View placement could not be saved: " + warning);
            }
        }
        catch (Exception error)
        {
            _logWarning("Camera View placement could not be saved: " + error.Message);
        }
    }
}

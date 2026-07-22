// SPDX-License-Identifier: Apache-2.0

using System;
using System.Windows.Interop;

namespace AcsEditor;

/// <summary>
/// Coordinates the top-level Win32 move/size modal loop with the native child
/// viewport. WindowChrome owns caption hit testing; this layer only prevents
/// rendering and swapchain resize work from competing with that modal loop.
/// </summary>
public partial class MainWindow
{
    private const int WmCancelMode = 0x001F;
    private const int WmActivate = 0x0006;
    private const int WmNcLButtonDown = 0x00A1;
    private const int WmEnterSizeMove = 0x0231;
    private const int WmExitSizeMove = 0x0232;
    private const int HtCaption = 2;

    private HwndSource? _editorWindowMessageSource;
    private bool _windowMoveSizeActive;

    /// <summary>
    /// Returns +1 on entry, -1 on exit, and zero for unrelated top-level
    /// messages. Kept pure so the modal-loop contract is covered by self-test.
    /// </summary>
    internal static int WindowMoveSizeTransition(int message) =>
        message == WmEnterSizeMove ? 1 :
        message == WmExitSizeMove ? -1 :
        0;

    private void InitializeWindowInteraction()
    {
        SourceInitialized += OnEditorWindowSourceInitialized;
        Closed += OnEditorWindowInteractionClosed;
    }

    private void OnEditorWindowInteractionClosed(object? sender, EventArgs e)
    {
        Closed -= OnEditorWindowInteractionClosed;
        DetachEditorWindowMessageHook();
    }

    private void OnEditorWindowSourceInitialized(object? sender, EventArgs e)
    {
        if (_editorWindowMessageSource != null) return;
        nint handle = new WindowInteropHelper(this).Handle;
        if (handle == 0) return;

        _editorWindowMessageSource = HwndSource.FromHwnd(handle);
        _editorWindowMessageSource?.AddHook(EditorWindowMessageHook);
    }

    private void DetachEditorWindowMessageHook()
    {
        HwndSource? source = _editorWindowMessageSource;
        _editorWindowMessageSource = null;
        _windowMoveSizeActive = false;
        if (source == null) return;
        try { source.RemoveHook(EditorWindowMessageHook); }
        catch (InvalidOperationException) { }
    }

    private IntPtr EditorWindowMessageHook(
        IntPtr hwnd,
        int message,
        IntPtr wParam,
        IntPtr lParam,
        ref bool handled)
    {
        // `ShowActivated=false` is only a Show() hint. During a --no-activate
        // launch the top-level HWND also carries WS_EX_NOACTIVATE, preventing
        // delayed renderer/startup work from taking the foreground. A real
        // mouse click is the sole transition back to normal editor behavior.
        // Removing the style before returning MA_ACTIVATE lets the same click
        // activate and interact with the editor; --unattended never satisfies
        // this policy and therefore remains permanently noninteractive.
        if (App.ShouldReleaseInitialActivationGuard(
                App.IsInitialActivationSuppressed,
                App.IsNonInteractiveLaunch,
                message,
                unchecked((int)((lParam.ToInt64() >> 16) & 0xFFFFL))) &&
            App.ReleaseInitialEditorActivation(this))
        {
            handled = true;
            return (IntPtr)App.MaActivate;
        }

        // Keep the WPF tree and HwndHost hit-test-visible so the native
        // swapchain remains composited, but consume every unattended input
        // message before WPF controls or WindowChrome can react to it.
        if (EditorInputGate.ShouldSuppressNativeMessage(
                App.IsNonInteractiveLaunch, message))
        {
            handled = true;
            return IntPtr.Zero;
        }

        int transition = WindowMoveSizeTransition(message);
        if (transition > 0)
        {
            if (!_windowMoveSizeActive)
            {
                _windowMoveSizeActive = true;
                _viewport?.PauseRenderPumpForWindowInteraction();
            }
        }
        else if (transition < 0)
        {
            if (_windowMoveSizeActive)
            {
                _windowMoveSizeActive = false;
                QueueViewportResumeAfterMoveSize();
            }
        }

        // Caption drag, modal cancellation, or deactivation must not inherit a
        // native child capture from a viewport gizmo/pan gesture.
        bool deactivating = message == WmActivate &&
                            (wParam.ToInt64() & 0xFFFFL) == 0;
        bool captionPress = message == WmNcLButtonDown &&
                            wParam.ToInt64() == HtCaption;
        if (message == WmCancelMode || deactivating || captionPress)
            _viewport?.CancelPointerInteraction();

        return IntPtr.Zero;
    }

    private void QueueViewportResumeAfterMoveSize()
    {
        if (Dispatcher.HasShutdownStarted || Dispatcher.HasShutdownFinished)
            return;

        _ = Dispatcher.BeginInvoke(
            System.Windows.Threading.DispatcherPriority.Loaded,
            new Action(() =>
            {
                // A new modal interaction may have started before this layout
                // turn. Keep rendering paused until its matching exit.
                if (!_windowMoveSizeActive)
                    _viewport?.ResumeRenderPumpAfterWindowInteraction();
            }));
    }

    private void SynchronizeWindowInteractionWithViewport()
    {
        if (_windowMoveSizeActive)
            _viewport?.PauseRenderPumpForWindowInteraction();
    }
}

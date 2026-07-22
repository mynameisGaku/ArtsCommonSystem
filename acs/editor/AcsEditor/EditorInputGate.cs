// SPDX-License-Identifier: Apache-2.0

namespace AcsEditor;

/// <summary>Pure input-suppression contract shared by runtime guards and tests.</summary>
internal static class EditorInputGate
{
    private const int WmKeyFirst = 0x0100;
    private const int WmKeyLast = 0x0109;
    private const int WmGesture = 0x0119;
    private const int WmGestureNotify = 0x011A;
    private const int WmNcMouseFirst = 0x00A0;
    private const int WmNcMouseLast = 0x00AD;
    private const int WmMouseMove = 0x0200;
    private const int WmMouseHWheel = 0x020E;
    private const int WmDropFiles = 0x0233;
    private const int WmTouch = 0x0240;
    private const int WmPointerHWheel = 0x024F;

    internal static bool ShouldSuppressShortcuts(
        bool nonInteractiveLaunch,
        bool isActive,
        bool keyboardFocusWithin) =>
        nonInteractiveLaunch || !isActive || !keyboardFocusWithin;

    internal static bool ShouldSuppressNativeMessage(
        bool nonInteractiveLaunch,
        int message) =>
        nonInteractiveLaunch &&
        ((message >= WmKeyFirst && message <= WmKeyLast) ||
         message == WmGesture ||
         message == WmGestureNotify ||
         (message >= WmNcMouseFirst && message <= WmNcMouseLast) ||
         message == WmDropFiles ||
         (message >= WmMouseMove && message <= WmMouseHWheel) ||
         (message >= WmTouch && message <= WmPointerHWheel));
}

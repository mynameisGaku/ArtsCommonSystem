namespace AcsEditor;

/// <summary>
/// Native interactive-water pointer actions. Values intentionally match the
/// editor ABI contract.
/// </summary>
internal enum WaterPointerAction
{
    None = -1,
    Press = 0,
    Drag = 1,
    End = 2,
    Hover = 3,
}

/// <summary>
/// Pure viewport state consumed by the water pointer routing policy.
/// </summary>
internal readonly record struct WaterPointerRoutingState(
    bool EngineReady,
    bool View3D,
    bool PolygonMode,
    bool GizmoDragging,
    bool Gizmo3DDragging,
    bool Panning,
    bool MarqueeDragging)
{
    public bool BlocksWater =>
        !EngineReady ||
        !View3D ||
        PolygonMode ||
        GizmoDragging ||
        Gizmo3DDragging ||
        Panning ||
        MarqueeDragging;
}

/// <summary>
/// A routing decision deliberately carries the capture contract. Interactive
/// water observes existing viewport gestures and must never own mouse capture.
/// </summary>
internal readonly record struct WaterPointerRoutingDecision(
    WaterPointerAction Action,
    bool CapturePointer)
{
    public bool ShouldRoute => Action != WaterPointerAction.None;
}

internal static class WaterPointerRoutingPolicy
{
    internal const int WmCancelMode = 0x001F;
    internal const int WmLeftButtonUp = 0x0202;
    internal const int WmMouseWheel = 0x020A;
    internal const int WmMouseHWheel = 0x020E;
    internal const int WmCaptureChanged = 0x0215;
    internal const int WmMouseLeave = 0x02A3;

    private static WaterPointerRoutingDecision Route(
        WaterPointerAction action) =>
        new(action, CapturePointer: false);

    public static WaterPointerRoutingDecision ForPress(
        WaterPointerRoutingState state,
        bool gizmoAccepted)
    {
        if (state.BlocksWater || gizmoAccepted)
            return Route(WaterPointerAction.None);
        return Route(WaterPointerAction.Press);
    }

    public static WaterPointerRoutingDecision ForMove(
        WaterPointerRoutingState state,
        bool leftButtonDown)
    {
        if (state.BlocksWater)
            return Route(WaterPointerAction.None);
        return Route(
            leftButtonDown
                ? WaterPointerAction.Drag
                : WaterPointerAction.Hover);
    }

    public static WaterPointerRoutingDecision ForEnd(
        bool engineReady) =>
        Route(
            engineReady
                ? WaterPointerAction.End
                : WaterPointerAction.None);

    public static bool EndsTrackingForWindowMessage(int message) =>
        message is
            WmCancelMode or
            WmLeftButtonUp or
            WmMouseWheel or
            WmMouseHWheel or
            WmCaptureChanged or
            WmMouseLeave;

    public static WaterPointerRoutingDecision ForWindowMessage(
        int message,
        bool engineReady) =>
        EndsTrackingForWindowMessage(message)
            ? ForEnd(engineReady)
            : Route(WaterPointerAction.None);
}

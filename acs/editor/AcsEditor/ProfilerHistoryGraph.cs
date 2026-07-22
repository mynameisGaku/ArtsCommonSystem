// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Windows;
using System.Windows.Media;

namespace AcsEditor;

/// <summary>Allocation-light WPF history graph for sampled profiler data.</summary>
internal sealed class ProfilerHistoryGraph : FrameworkElement
{
    private static readonly Brush GraphBackground = FrozenBrush("#FF101419");
    private static readonly Brush GridBrush = FrozenBrush("#FF2B333D");
    private static readonly Brush TextBrush = FrozenBrush("#FF7E8996");
    private static readonly Pen CpuPen = FrozenPen("#FF61C7F2", 1.6);
    private static readonly Pen GpuPen = FrozenPen("#FFF3B65B", 1.6);
    private static readonly Pen CpuPeakPen = FrozenDashedPen("#9961C7F2");
    private static readonly Pen GpuPeakPen = FrozenDashedPen("#99F3B65B");
    private static readonly Pen OpaquePen = FrozenPen("#FF97D67C", 1.0);
    private static readonly Pen AtmospherePen = FrozenPen("#FF8BA7FF", 1.0);
    private static readonly Pen CloudPen = FrozenPen("#FFF08CC0", 1.0);
    private static readonly Pen FogPen = FrozenPen("#FFB59AF2", 1.0);
    private static readonly Pen PostPen = FrozenPen("#FFE28B63", 1.0);

    private IReadOnlyList<EditorProfilerPoint> _points =
        Array.Empty<EditorProfilerPoint>();
    private float _cpuPeakMs = -1;
    private float _gpuPeakMs = -1;

    internal bool ShowCpu { get; set; } = true;
    internal bool ShowGpu { get; set; } = true;
    internal bool ShowPasses { get; set; } = true;

    internal void SetHistory(IReadOnlyList<EditorProfilerPoint> points)
    {
        _points = points ?? Array.Empty<EditorProfilerPoint>();
        InvalidateVisual();
    }

    internal void SetPeaks(float cpuPeakMs, float gpuPeakMs)
    {
        _cpuPeakMs = float.IsFinite(cpuPeakMs) && cpuPeakMs >= 0
            ? cpuPeakMs : -1;
        _gpuPeakMs = float.IsFinite(gpuPeakMs) && gpuPeakMs >= 0
            ? gpuPeakMs : -1;
        InvalidateVisual();
    }

    protected override void OnRender(DrawingContext drawingContext)
    {
        base.OnRender(drawingContext);
        Rect bounds = new(0, 0, Math.Max(0, ActualWidth), Math.Max(0, ActualHeight));
        drawingContext.DrawRectangle(GraphBackground, null, bounds);
        if (bounds.Width < 40 || bounds.Height < 30)
            return;

        const double labelWidth = 42;
        const double padding = 8;
        Rect plot = new(
            labelWidth,
            padding,
            Math.Max(1, bounds.Width - labelWidth - padding),
            Math.Max(1, bounds.Height - padding * 2));

        double maxMs = 33.34;
        foreach (EditorProfilerPoint point in _points)
        {
            maxMs = Math.Max(maxMs, point.CpuFrameMs);
            if (point.GpuFrameMs >= 0)
                maxMs = Math.Max(maxMs, point.GpuFrameMs);
        }
        if (_cpuPeakMs >= 0)
            maxMs = Math.Max(maxMs, _cpuPeakMs);
        if (_gpuPeakMs >= 0)
            maxMs = Math.Max(maxMs, _gpuPeakMs);
        maxMs = NiceMaximum(Math.Min(250, maxMs * 1.15));

        DrawGrid(drawingContext, plot, maxMs);
        if (ShowCpu && _cpuPeakMs >= 0)
            DrawPeak(drawingContext, plot, maxMs, _cpuPeakMs,
                CpuPeakPen, "CPU peak", -12);
        if (ShowGpu && _gpuPeakMs >= 0)
            DrawPeak(drawingContext, plot, maxMs, _gpuPeakMs,
                GpuPeakPen, "GPU peak", 1);
        if (_points.Count < 2)
        {
            DrawText(drawingContext, "Waiting for frame samples…",
                new Point(plot.Left + 10, plot.Top + 10), TextBrush);
            return;
        }

        if (ShowPasses)
        {
            DrawSeries(drawingContext, plot, maxMs, OpaquePen, p => p.OpaqueCpuMs);
            DrawSeries(drawingContext, plot, maxMs, AtmospherePen, p => p.AtmosphereCpuMs);
            DrawSeries(drawingContext, plot, maxMs, CloudPen, p => p.CloudCpuMs);
            DrawSeries(drawingContext, plot, maxMs, FogPen, p => p.FogCpuMs);
            DrawSeries(drawingContext, plot, maxMs, PostPen, p => p.PostCpuMs);
        }
        if (ShowCpu)
            DrawSeries(drawingContext, plot, maxMs, CpuPen, p => p.CpuFrameMs);
        if (ShowGpu && HasGpuSamples())
            DrawSeries(drawingContext, plot, maxMs, GpuPen, p => p.GpuFrameMs);
    }

    private void DrawPeak(
        DrawingContext dc,
        Rect plot,
        double maxMs,
        float peakMs,
        Pen pen,
        string label,
        double labelOffset)
    {
        double y = plot.Bottom -
            plot.Height * Math.Clamp(peakMs / maxMs, 0.0, 1.0);
        dc.DrawLine(
            pen,
            new Point(plot.Left, y),
            new Point(plot.Right, y));
        DrawText(
            dc,
            $"{label} {peakMs:0.0}",
            new Point(Math.Max(plot.Left + 4, plot.Right - 94),
                      Math.Clamp(y + labelOffset, plot.Top, plot.Bottom - 12)),
            pen.Brush);
    }

    private void DrawGrid(DrawingContext dc, Rect plot, double maxMs)
    {
        Pen gridPen = new(GridBrush, 1);
        gridPen.Freeze();
        for (int i = 0; i <= 4; i++)
        {
            double value = maxMs * i / 4.0;
            double y = plot.Bottom - plot.Height * i / 4.0;
            dc.DrawLine(gridPen, new Point(plot.Left, y), new Point(plot.Right, y));
            DrawText(dc, $"{value:0.#}", new Point(4, y - 7), TextBrush);
        }

        DrawText(dc, "ms", new Point(6, plot.Top), TextBrush);
        DrawText(dc, "CPU frame", new Point(plot.Left + 8, plot.Top + 4),
            CpuPen.Brush);
        if (HasGpuSamples())
            DrawText(dc, "GPU", new Point(plot.Left + 78, plot.Top + 4),
                GpuPen.Brush);
    }

    private void DrawSeries(
        DrawingContext dc,
        Rect plot,
        double maxMs,
        Pen pen,
        Func<EditorProfilerPoint, float> selector)
    {
        var geometry = new StreamGeometry();
        using (StreamGeometryContext context = geometry.Open())
        {
            bool begun = false;
            int denominator = Math.Max(1, _points.Count - 1);
            for (int i = 0; i < _points.Count; i++)
            {
                float raw = selector(_points[i]);
                if (!float.IsFinite(raw) || raw < 0)
                {
                    begun = false;
                    continue;
                }

                double x = plot.Left + plot.Width * i / denominator;
                double y = plot.Bottom -
                    plot.Height * Math.Clamp(raw / maxMs, 0.0, 1.0);
                Point point = new(x, y);
                if (!begun)
                {
                    context.BeginFigure(point, false, false);
                    begun = true;
                }
                else
                {
                    context.LineTo(point, true, false);
                }
            }
        }
        geometry.Freeze();
        dc.DrawGeometry(null, pen, geometry);
    }

    private bool HasGpuSamples()
    {
        foreach (EditorProfilerPoint point in _points)
            if (point.GpuFrameMs >= 0)
                return true;
        return false;
    }

    private void DrawText(
        DrawingContext dc,
        string text,
        Point origin,
        Brush brush)
    {
        var formatted = new FormattedText(
            text,
            CultureInfo.InvariantCulture,
            FlowDirection.LeftToRight,
            new Typeface("Segoe UI"),
            10,
            brush,
            VisualTreeHelper.GetDpi(this).PixelsPerDip);
        dc.DrawText(formatted, origin);
    }

    private static double NiceMaximum(double value)
    {
        if (value <= 16.67) return 16.67;
        if (value <= 33.34) return 33.34;
        if (value <= 50) return 50;
        if (value <= 100) return 100;
        if (value <= 166.7) return 166.7;
        return 250;
    }

    private static Brush FrozenBrush(string color)
    {
        var brush = new SolidColorBrush((Color)ColorConverter.ConvertFromString(color));
        brush.Freeze();
        return brush;
    }

    private static Pen FrozenPen(string color, double thickness)
    {
        var pen = new Pen(FrozenBrush(color), thickness);
        pen.Freeze();
        return pen;
    }

    private static Pen FrozenDashedPen(string color)
    {
        var pen = new Pen(FrozenBrush(color), 1)
        {
            DashStyle = DashStyles.Dash,
        };
        pen.Freeze();
        return pen;
    }
}

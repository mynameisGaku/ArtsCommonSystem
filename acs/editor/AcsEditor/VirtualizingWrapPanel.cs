// SPDX-License-Identifier: Apache-2.0

using System;
using System.Collections.Specialized;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Media;

namespace AcsEditor;

/// <summary>
/// A uniform, vertically scrolling wrap panel that realizes only the rows around the viewport.
/// The panel is intended for tile views whose content size is controlled by <see cref="TileSize"/>.
/// </summary>
public sealed class VirtualizingWrapPanel : VirtualizingPanel, IScrollInfo
{
    private const double DefaultTileSize = 64d;
    private const double DefaultHorizontalChrome = 12d;
    private const double DefaultVerticalChrome = 30d;
    private const double ScrollEpsilon = 0.01d;

    public static readonly DependencyProperty TileSizeProperty =
        DependencyProperty.Register(
            nameof(TileSize),
            typeof(double),
            typeof(VirtualizingWrapPanel),
            new FrameworkPropertyMetadata(
                DefaultTileSize,
                FrameworkPropertyMetadataOptions.AffectsMeasure,
                OnLayoutPropertyChanged,
                CoercePositiveFiniteDouble));

    public static readonly DependencyProperty HorizontalChromeProperty =
        DependencyProperty.Register(
            nameof(HorizontalChrome),
            typeof(double),
            typeof(VirtualizingWrapPanel),
            new FrameworkPropertyMetadata(
                DefaultHorizontalChrome,
                FrameworkPropertyMetadataOptions.AffectsMeasure,
                OnLayoutPropertyChanged,
                CoerceNonNegativeFiniteDouble));

    public static readonly DependencyProperty VerticalChromeProperty =
        DependencyProperty.Register(
            nameof(VerticalChrome),
            typeof(double),
            typeof(VirtualizingWrapPanel),
            new FrameworkPropertyMetadata(
                DefaultVerticalChrome,
                FrameworkPropertyMetadataOptions.AffectsMeasure,
                OnLayoutPropertyChanged,
                CoerceNonNegativeFiniteDouble));

    public static readonly DependencyProperty CacheRowsProperty =
        DependencyProperty.Register(
            nameof(CacheRows),
            typeof(int),
            typeof(VirtualizingWrapPanel),
            new FrameworkPropertyMetadata(
                1,
                FrameworkPropertyMetadataOptions.AffectsMeasure,
                OnLayoutPropertyChanged,
                CoerceCacheRows));

    private Size _extent;
    private Size _viewport;
    private Point _offset;
    private double _cellWidth = DefaultTileSize + DefaultHorizontalChrome;
    private double _cellHeight = DefaultTileSize + DefaultVerticalChrome;
    private int _columns = 1;
    private int _itemCount;
    private bool _canHorizontallyScroll;
    private bool _canVerticallyScroll = true;
    private ScrollViewer? _scrollOwner;

    public double TileSize
    {
        get => (double)GetValue(TileSizeProperty);
        set => SetValue(TileSizeProperty, value);
    }

    /// <summary>
    /// Width added by the item container (margin, border, and padding) around the square tile.
    /// </summary>
    public double HorizontalChrome
    {
        get => (double)GetValue(HorizontalChromeProperty);
        set => SetValue(HorizontalChromeProperty, value);
    }

    /// <summary>
    /// Height added by the item container and the single-line asset name below the square tile.
    /// </summary>
    public double VerticalChrome
    {
        get => (double)GetValue(VerticalChromeProperty);
        set => SetValue(VerticalChromeProperty, value);
    }

    /// <summary>Rows realized before and after the visible viewport.</summary>
    public int CacheRows
    {
        get => (int)GetValue(CacheRowsProperty);
        set => SetValue(CacheRowsProperty, value);
    }

    public bool CanHorizontallyScroll
    {
        get => _canHorizontallyScroll;
        set
        {
            if (_canHorizontallyScroll == value) return;
            _canHorizontallyScroll = value;
            if (!value) _offset.X = 0d;
            InvalidateMeasure();
        }
    }

    public bool CanVerticallyScroll
    {
        get => _canVerticallyScroll;
        set
        {
            if (_canVerticallyScroll == value) return;
            _canVerticallyScroll = value;
            if (!value) _offset.Y = 0d;
            InvalidateMeasure();
        }
    }

    public double ExtentWidth => _extent.Width;
    public double ExtentHeight => _extent.Height;
    public double ViewportWidth => _viewport.Width;
    public double ViewportHeight => _viewport.Height;
    public double HorizontalOffset => _offset.X;
    public double VerticalOffset => _offset.Y;

    public ScrollViewer? ScrollOwner
    {
        get => _scrollOwner;
        set
        {
            if (ReferenceEquals(_scrollOwner, value)) return;
            _scrollOwner = value;
            _scrollOwner?.InvalidateScrollInfo();
        }
    }

    protected override Size MeasureOverride(Size availableSize)
    {
        ItemsControl? owner = ItemsControl.GetItemsOwner(this);
        if (owner == null)
        {
            ClearRealizedChildren();
            UpdateScrollInfo(new Size(), NormalizeViewport(availableSize, null));
            return NormalizeDesiredSize(availableSize);
        }

        _itemCount = owner.Items.Count;
        Size viewport = NormalizeViewport(availableSize, owner);
        EstablishCellMetrics();

        if (_itemCount == 0)
        {
            ClearRealizedChildren();
            _columns = 1;
            _offset = new Point();
            UpdateScrollInfo(new Size(viewport.Width, 0d), viewport);
            return NormalizeDesiredSize(availableSize);
        }

        MeasureRealizedRange(owner, viewport, allowMetricCorrection: true);
        return NormalizeDesiredSize(availableSize);
    }

    protected override Size ArrangeOverride(Size finalSize)
    {
        double width = NormalizeFiniteLength(finalSize.Width, _viewport.Width);
        double height = NormalizeFiniteLength(finalSize.Height, _viewport.Height);
        if (width > 0d && Math.Abs(width - _viewport.Width) > ScrollEpsilon)
        {
            // A resize can change the wrap column count after Measure. Request a fresh
            // realization while still arranging the currently valid children safely.
            InvalidateMeasure();
        }

        ItemsControl? owner = ItemsControl.GetItemsOwner(this);
        IItemContainerGenerator? generator =
            owner == null ? null : GetGenerator(owner);
        for (int childIndex = 0; childIndex < InternalChildren.Count; childIndex++)
        {
            UIElement child = InternalChildren[childIndex];
            int itemIndex = generator?.IndexFromGeneratorPosition(
                new GeneratorPosition(childIndex, 0)) ?? -1;
            if (itemIndex < 0 || itemIndex >= _itemCount)
            {
                child.Arrange(new Rect(0d, 0d, 0d, 0d));
                continue;
            }

            int row = itemIndex / Math.Max(1, _columns);
            int column = itemIndex % Math.Max(1, _columns);
            double x = column * _cellWidth - _offset.X;
            double y = row * _cellHeight - _offset.Y;
            child.Arrange(new Rect(x, y, _cellWidth, _cellHeight));
        }

        return new Size(width, height);
    }

    protected override void OnItemsChanged(object sender, ItemsChangedEventArgs args)
    {
        base.OnItemsChanged(sender, args);
        switch (args.Action)
        {
            case NotifyCollectionChangedAction.Reset:
            case NotifyCollectionChangedAction.Move:
                ClearRealizedChildren();
                break;
            case NotifyCollectionChangedAction.Remove:
            case NotifyCollectionChangedAction.Replace:
                if (args.ItemUICount > 0 &&
                    args.Position.Index >= 0 &&
                    args.Position.Index < InternalChildren.Count)
                {
                    int count = Math.Min(
                        args.ItemUICount,
                        InternalChildren.Count - args.Position.Index);
                    RemoveInternalChildRange(args.Position.Index, count);
                }
                break;
        }

        InvalidateMeasure();
    }

    protected override void BringIndexIntoView(int index)
    {
        ItemsControl? owner = ItemsControl.GetItemsOwner(this);
        int count = owner?.Items.Count ?? 0;
        if (index < 0 || index >= count) return;

        int columns = Math.Max(1, _columns);
        double rowTop = (index / columns) * _cellHeight;
        double rowBottom = rowTop + _cellHeight;
        double target = _offset.Y;
        if (rowTop < _offset.Y)
            target = rowTop;
        else if (rowBottom > _offset.Y + _viewport.Height)
            target = rowBottom - _viewport.Height;

        SetVerticalOffset(target);
    }

    public void LineUp() => SetVerticalOffset(VerticalOffset - _cellHeight);
    public void LineDown() => SetVerticalOffset(VerticalOffset + _cellHeight);
    public void LineLeft() => SetHorizontalOffset(HorizontalOffset - _cellWidth);
    public void LineRight() => SetHorizontalOffset(HorizontalOffset + _cellWidth);

    public void MouseWheelUp() =>
        SetVerticalOffset(VerticalOffset - Math.Max(_cellHeight, _cellHeight * 3d));

    public void MouseWheelDown() =>
        SetVerticalOffset(VerticalOffset + Math.Max(_cellHeight, _cellHeight * 3d));

    public void MouseWheelLeft() => LineLeft();
    public void MouseWheelRight() => LineRight();
    public void PageUp() => SetVerticalOffset(VerticalOffset - _viewport.Height);
    public void PageDown() => SetVerticalOffset(VerticalOffset + _viewport.Height);
    public void PageLeft() => SetHorizontalOffset(HorizontalOffset - _viewport.Width);
    public void PageRight() => SetHorizontalOffset(HorizontalOffset + _viewport.Width);

    public void SetHorizontalOffset(double offset)
    {
        double coerced = _canHorizontallyScroll
            ? CoerceOffset(offset, _extent.Width, _viewport.Width)
            : 0d;
        if (Math.Abs(coerced - _offset.X) <= ScrollEpsilon) return;
        _offset.X = coerced;
        InvalidateMeasure();
        _scrollOwner?.InvalidateScrollInfo();
    }

    public void SetVerticalOffset(double offset)
    {
        double coerced = _canVerticallyScroll
            ? CoerceOffset(offset, _extent.Height, _viewport.Height)
            : 0d;
        if (Math.Abs(coerced - _offset.Y) <= ScrollEpsilon) return;
        _offset.Y = coerced;
        InvalidateMeasure();
        _scrollOwner?.InvalidateScrollInfo();
    }

    public Rect MakeVisible(Visual visual, Rect rectangle)
    {
        if (visual == null || rectangle.IsEmpty) return Rect.Empty;
        UIElement? container = FindDirectChild(visual);
        if (container == null) return Rect.Empty;

        int childIndex = InternalChildren.IndexOf(container);
        if (childIndex < 0) return Rect.Empty;
        ItemsControl? owner = ItemsControl.GetItemsOwner(this);
        IItemContainerGenerator? generator =
            owner == null ? null : GetGenerator(owner);
        int itemIndex = generator?.IndexFromGeneratorPosition(
            new GeneratorPosition(childIndex, 0)) ?? -1;
        if (itemIndex < 0) return Rect.Empty;

        int row = itemIndex / Math.Max(1, _columns);
        int column = itemIndex % Math.Max(1, _columns);
        Rect itemBounds = new(
            column * _cellWidth,
            row * _cellHeight,
            _cellWidth,
            _cellHeight);
        Rect target = rectangle;
        target.Offset(itemBounds.X, itemBounds.Y);

        double vertical = _offset.Y;
        if (target.Top < vertical)
            vertical = target.Top;
        else if (target.Bottom > vertical + _viewport.Height)
            vertical = target.Bottom - _viewport.Height;
        SetVerticalOffset(vertical);

        double horizontal = _offset.X;
        if (_canHorizontallyScroll)
        {
            if (target.Left < horizontal)
                horizontal = target.Left;
            else if (target.Right > horizontal + _viewport.Width)
                horizontal = target.Right - _viewport.Width;
            SetHorizontalOffset(horizontal);
        }

        target.Offset(-_offset.X, -_offset.Y);
        return target;
    }

    private void MeasureRealizedRange(
        ItemsControl owner,
        Size viewport,
        bool allowMetricCorrection)
    {
        CalculateLayout(viewport, out int firstIndex, out int lastIndex, out Size extent);
        CoerceOffsets(extent, viewport);
        CalculateLayout(viewport, out firstIndex, out lastIndex, out extent);

        if (!VirtualizingPanel.GetIsVirtualizing(owner))
        {
            firstIndex = 0;
            lastIndex = _itemCount - 1;
        }

        RealizeRange(owner, firstIndex, lastIndex);
        CleanupRange(owner, firstIndex, lastIndex);

        double measuredWidth = _cellWidth;
        double measuredHeight = _cellHeight;
        var childConstraint = new Size(_cellWidth, double.PositiveInfinity);
        foreach (UIElement child in InternalChildren)
        {
            child.Measure(childConstraint);
            measuredWidth = Math.Max(measuredWidth, child.DesiredSize.Width);
            measuredHeight = Math.Max(measuredHeight, child.DesiredSize.Height);
        }

        bool metricsChanged =
            Math.Abs(measuredWidth - _cellWidth) > ScrollEpsilon ||
            Math.Abs(measuredHeight - _cellHeight) > ScrollEpsilon;
        if (allowMetricCorrection && metricsChanged)
        {
            _cellWidth = measuredWidth;
            _cellHeight = measuredHeight;
            MeasureRealizedRange(owner, viewport, allowMetricCorrection: false);
            return;
        }

        CalculateLayout(viewport, out _, out _, out extent);
        CoerceOffsets(extent, viewport);
        UpdateScrollInfo(extent, viewport);
    }

    private void CalculateLayout(
        Size viewport,
        out int firstIndex,
        out int lastIndex,
        out Size extent)
    {
        double usableWidth = Math.Max(1d, viewport.Width);
        _columns = Math.Max(1, (int)Math.Floor(usableWidth / Math.Max(1d, _cellWidth)));
        int rows = (_itemCount + _columns - 1) / _columns;
        double extentHeight = rows * _cellHeight;
        double extentWidth = Math.Max(viewport.Width, _columns * _cellWidth);
        extent = new Size(extentWidth, extentHeight);

        if (_itemCount == 0)
        {
            firstIndex = 0;
            lastIndex = -1;
            return;
        }

        int firstVisibleRow = Math.Max(
            0,
            (int)Math.Floor(_offset.Y / Math.Max(1d, _cellHeight)));
        int visibleRows = Math.Max(
            1,
            (int)Math.Ceiling(viewport.Height / Math.Max(1d, _cellHeight)) + 1);
        int firstRow = Math.Max(0, firstVisibleRow - CacheRows);
        int lastRow = Math.Min(
            rows - 1,
            firstVisibleRow + visibleRows - 1 + CacheRows);
        firstIndex = Math.Min(_itemCount - 1, firstRow * _columns);
        lastIndex = Math.Min(
            _itemCount - 1,
            ((lastRow + 1) * _columns) - 1);
    }

    private void RealizeRange(ItemsControl owner, int firstIndex, int lastIndex)
    {
        if (lastIndex < firstIndex) return;
        IItemContainerGenerator generator = GetGenerator(owner);
        GeneratorPosition start = generator.GeneratorPositionFromIndex(firstIndex);
        int childIndex = start.Offset == 0 ? start.Index : start.Index + 1;

        using (generator.StartAt(
                   start,
                   GeneratorDirection.Forward,
                   allowStartAtRealizedItem: true))
        {
            for (int itemIndex = firstIndex;
                 itemIndex <= lastIndex;
                 itemIndex++, childIndex++)
            {
                bool newlyRealized;
                if (generator.GenerateNext(out newlyRealized) is not UIElement child)
                    break;
                if (newlyRealized)
                {
                    if (childIndex >= InternalChildren.Count)
                        AddInternalChild(child);
                    else
                        InsertInternalChild(childIndex, child);
                    generator.PrepareItemContainer(child);
                }
            }
        }
    }

    private void CleanupRange(ItemsControl owner, int firstIndex, int lastIndex)
    {
        IItemContainerGenerator generator = GetGenerator(owner);
        bool recycle =
            VirtualizingPanel.GetVirtualizationMode(owner) ==
                VirtualizationMode.Recycling &&
            generator is IRecyclingItemContainerGenerator;

        for (int childIndex = InternalChildren.Count - 1; childIndex >= 0; childIndex--)
        {
            var position = new GeneratorPosition(childIndex, 0);
            int itemIndex = generator.IndexFromGeneratorPosition(position);
            // During the generator's initial attachment WPF may temporarily expose a
            // child that has not received an item index yet. It is not legal to recycle
            // that position; a subsequent layout pass will either map or discard it.
            if (itemIndex < 0) continue;
            if (itemIndex >= firstIndex && itemIndex <= lastIndex) continue;

            if (recycle)
                ((IRecyclingItemContainerGenerator)generator).Recycle(position, 1);
            else
                generator.Remove(position, 1);
            RemoveInternalChildRange(childIndex, 1);
        }
    }

    private void ClearRealizedChildren()
    {
        if (InternalChildren.Count != 0)
            RemoveInternalChildRange(0, InternalChildren.Count);
    }

    private IItemContainerGenerator GetGenerator(ItemsControl owner) =>
        ((IItemContainerGenerator)owner.ItemContainerGenerator)
            .GetItemContainerGeneratorForPanel(this);

    private void EstablishCellMetrics()
    {
        _cellWidth = Math.Max(1d, TileSize + HorizontalChrome);
        _cellHeight = Math.Max(1d, TileSize + VerticalChrome);
    }

    private void CoerceOffsets(Size extent, Size viewport)
    {
        _offset.X = _canHorizontallyScroll
            ? CoerceOffset(_offset.X, extent.Width, viewport.Width)
            : 0d;
        _offset.Y = _canVerticallyScroll
            ? CoerceOffset(_offset.Y, extent.Height, viewport.Height)
            : 0d;
    }

    private void UpdateScrollInfo(Size extent, Size viewport)
    {
        bool changed =
            !AreClose(_extent, extent) ||
            !AreClose(_viewport, viewport);
        _extent = extent;
        _viewport = viewport;
        if (changed) _scrollOwner?.InvalidateScrollInfo();
    }

    private static Size NormalizeViewport(Size availableSize, ItemsControl? owner)
    {
        double width = NormalizeFiniteLength(
            availableSize.Width,
            owner?.ActualWidth ?? 0d);
        double height = NormalizeFiniteLength(
            availableSize.Height,
            owner?.ActualHeight ?? 0d);
        if (width <= 0d) width = DefaultTileSize + DefaultHorizontalChrome;
        if (height <= 0d) height = DefaultTileSize + DefaultVerticalChrome;
        return new Size(width, height);
    }

    private static Size NormalizeDesiredSize(Size availableSize) =>
        new(
            NormalizeFiniteLength(availableSize.Width, 0d),
            NormalizeFiniteLength(availableSize.Height, 0d));

    private static double NormalizeFiniteLength(double value, double fallback)
    {
        if (!double.IsNaN(value) && !double.IsInfinity(value) && value >= 0d)
            return value;
        if (!double.IsNaN(fallback) && !double.IsInfinity(fallback) && fallback >= 0d)
            return fallback;
        return 0d;
    }

    private static double CoerceOffset(double value, double extent, double viewport)
    {
        if (double.IsNaN(value) || double.IsInfinity(value)) value = 0d;
        return Math.Clamp(value, 0d, Math.Max(0d, extent - viewport));
    }

    private UIElement? FindDirectChild(DependencyObject visual)
    {
        DependencyObject current = visual;
        while (current != null && !ReferenceEquals(VisualTreeHelper.GetParent(current), this))
        {
            DependencyObject? parent = VisualTreeHelper.GetParent(current);
            if (parent == null) return null;
            current = parent;
        }
        return current as UIElement;
    }

    private static bool AreClose(Size left, Size right) =>
        Math.Abs(left.Width - right.Width) <= ScrollEpsilon &&
        Math.Abs(left.Height - right.Height) <= ScrollEpsilon;

    private static void OnLayoutPropertyChanged(
        DependencyObject dependencyObject,
        DependencyPropertyChangedEventArgs e)
    {
        var panel = (VirtualizingWrapPanel)dependencyObject;
        panel.InvalidateMeasure();
        panel._scrollOwner?.InvalidateScrollInfo();
    }

    private static object CoercePositiveFiniteDouble(
        DependencyObject dependencyObject,
        object baseValue)
    {
        double value = (double)baseValue;
        return !double.IsNaN(value) && !double.IsInfinity(value) && value > 0d
            ? value
            : DefaultTileSize;
    }

    private static object CoerceNonNegativeFiniteDouble(
        DependencyObject dependencyObject,
        object baseValue)
    {
        double value = (double)baseValue;
        return !double.IsNaN(value) && !double.IsInfinity(value) && value >= 0d
            ? value
            : 0d;
    }

    private static object CoerceCacheRows(
        DependencyObject dependencyObject,
        object baseValue) =>
        Math.Clamp((int)baseValue, 0, 32);
}

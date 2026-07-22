// SPDX-License-Identifier: Apache-2.0
// Shared Details-panel presentation helpers. The native engine remains the source of truth;
// this file only provides UE-style filtering, category accordions, and component cards.
using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace AcsEditor;

public partial class MainWindow
{
    private string _detailsFilter = "";

    private void OnDetailsSearchChanged(object sender, TextChangedEventArgs e)
    {
        _detailsFilter = DetailsSearchBox.Text?.Trim() ?? "";
        DetailsSearchHint.Visibility = _detailsFilter.Length == 0
            ? Visibility.Visible : Visibility.Collapsed;

        if (Engine == IntPtr.Zero) return;
        if (_view3d)
        {
            int id = EngineInterop.acs_editor_selected3d(Engine);
            if (id >= 0) Populate3DInspector(id);
        }
        else if (_selectedId >= 0)
        {
            ApplyStatic2DDetailsFilter();
            PopulateComponents(_selectedId);
        }
    }

    private void OnClearDetailsSearch(object sender, RoutedEventArgs e)
    {
        DetailsSearchBox.Clear();
        DetailsSearchBox.Focus();
    }

    private bool DetailsMatches(params string?[] searchableText)
    {
        if (_detailsFilter.Length == 0) return true;
        foreach (string? text in searchableText)
            if (!string.IsNullOrWhiteSpace(text)
                && text.Contains(_detailsFilter, StringComparison.OrdinalIgnoreCase))
                return true;
        return false;
    }

    private bool DetailsComponentMatches(string typeName)
    {
        if (DetailsMatches("component", "script", typeName, Friendly(typeName))) return true;
        int propertyCount = EngineInterop.acs_editor_component_prop_count(typeName);
        for (int property = 0; property < propertyCount; property++)
        {
            if (DetailsMatches(
                    EngineInterop.ComponentPropName(typeName, property),
                    EngineInterop.ComponentPropCategory(typeName, property)))
                return true;
        }

        int methodCount = EngineInterop.acs_editor_component_method_count(typeName);
        for (int method = 0; method < methodCount; method++)
            if (DetailsMatches(EngineInterop.ComponentMethodName(typeName, method))) return true;
        return false;
    }

    private static TextBlock DetailsTitle(string title) => new()
    {
        Text = title,
        FontWeight = FontWeights.SemiBold,
        FontSize = 11.5,
        VerticalAlignment = VerticalAlignment.Center,
    };

    private Expander DetailsCategory(string title, UIElement body, bool expanded = true)
    {
        return new Expander
        {
            Header = DetailsTitle(title),
            Content = body,
            IsExpanded = expanded || _detailsFilter.Length > 0,
            Style = (Style)FindResource("DetailsExpander"),
        };
    }

    private Expander ComponentCard(
        string typeName,
        UIElement body,
        bool native,
        Action? remove = null)
    {
        var header = new DockPanel { LastChildFill = true };

        if (remove != null)
        {
            var removeButton = new Button
            {
                Content = "×",
                Width = 22,
                Height = 20,
                Padding = new Thickness(0),
                Margin = new Thickness(6, 0, 0, 0),
                Foreground = (Brush)FindResource("WarnFg"),
                Background = Brushes.Transparent,
                BorderThickness = new Thickness(0),
                Cursor = Cursors.Hand,
                ToolTip = $"Remove {Friendly(typeName)}",
            };
            removeButton.Click += (_, e) =>
            {
                e.Handled = true;
                remove();
            };
            DockPanel.SetDock(removeButton, Dock.Right);
            header.Children.Add(removeButton);
        }

        var badge = new Border { Style = (Style)FindResource("ComponentBadge") };
        badge.Child = new TextBlock
        {
            Text = native ? "NATIVE" : "SCRIPT",
            FontSize = 8.5,
            FontWeight = FontWeights.SemiBold,
            Foreground = native ? (Brush)FindResource("InfoFg") : (Brush)FindResource("TextDim"),
        };
        DockPanel.SetDock(badge, Dock.Right);
        header.Children.Add(badge);

        header.Children.Add(new TextBlock
        {
            Text = native ? "▣" : "◇",
            Margin = new Thickness(0, 0, 6, 0),
            VerticalAlignment = VerticalAlignment.Center,
            Foreground = native ? (Brush)FindResource("InfoFg") : (Brush)FindResource("TextDim"),
        });
        header.Children.Add(DetailsTitle(Friendly(typeName)));

        return new Expander
        {
            Header = header,
            Content = body,
            IsExpanded = true,
            Style = (Style)FindResource("ComponentExpander"),
        };
    }

    private TextBlock EmptyDetailsResult(string text = "No properties match this filter.") => new()
    {
        Text = text,
        Foreground = (Brush)FindResource("TextDim"),
        FontSize = 11,
        TextWrapping = TextWrapping.Wrap,
        Margin = new Thickness(8, 12, 8, 8),
        HorizontalAlignment = HorizontalAlignment.Center,
    };

    // The legacy 2D editor is retained for project compatibility even though the main editor
    // boots in 3D mode. Its fixed sections still obey Details search at category granularity.
    private void ApplyStatic2DDetailsFilter()
    {
        if (_view3d) return;
        bool transform = DetailsMatches("transform", "name", "position", "rotation", "scale");
        bool display = DetailsMatches(
            "display", "visible", "enabled", "sort layer", "base", "color", "sprite", "material");
        bool components = DetailsMatches("component", "script", "property");
        InspFields.Visibility = transform || display || components
            ? Visibility.Visible : Visibility.Collapsed;
    }
}

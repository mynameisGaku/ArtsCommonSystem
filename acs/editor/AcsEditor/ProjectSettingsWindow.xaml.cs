// SPDX-License-Identifier: Apache-2.0
// Project Settings is a UI over the existing FProjectSettings editor ABI.
// Values are still applied immediately and persisted through the MainWindow callback.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace AcsEditor;

public partial class ProjectSettingsWindow : Window
{
    private readonly IntPtr _engine;
    private readonly Action _onChanged;
    private readonly List<Entry> _entries = new();
    private string _selectedCat = "";
    private bool _uiReady;

    private sealed record Entry(
        string Cat,
        string Key,
        string Value,
        int Type,
        string Options,
        bool Builtin,
        string Desc);

    private sealed record CategoryItem(string Name, int Count, bool IsAll = false);

    public ProjectSettingsWindow(Window owner, IntPtr engine, Action onChanged)
    {
        InitializeComponent();
        Owner = owner;
        _engine = engine;
        _onChanged = onChanged;
        _uiReady = true;
        Reload(keepSelection: false);
        UpdateSearchChrome();
        UpdateAddButton();
    }

    /// <summary>Reloads the complete setting catalog from the editor ABI.</summary>
    private void Reload(bool keepSelection)
    {
        string previousCategory = keepSelection ? _selectedCat : "";
        _entries.Clear();

        int count = EngineInterop.acs_editor_settings_count(_engine);
        var buffer = new byte[1024];
        for (int index = 0; index < count; index++)
        {
            Array.Clear(buffer, 0, buffer.Length);
            if (EngineInterop.acs_editor_settings_entry(_engine, index, buffer, buffer.Length) == 0)
                continue;

            string[] fields = EngineInterop.Utf8Z(buffer).Split('\t');
            if (fields.Length < 7)
                continue;

            _entries.Add(new Entry(
                fields[0],
                fields[1],
                fields[2],
                int.TryParse(fields[3], out int type) ? type : 4,
                fields[4],
                fields[5] == "1",
                fields[6]));
        }

        _selectedCat = previousCategory;
        RefreshCategoryList(keepSelection);
    }

    private void RefreshCategoryList(bool keepSelection)
    {
        string previousCategory = keepSelection ? _selectedCat : "";
        List<Entry> searchMatches = GetSearchMatches().ToList();
        List<CategoryItem> categories = searchMatches
            .GroupBy(entry => entry.Cat)
            .OrderBy(group => group.Key, StringComparer.OrdinalIgnoreCase)
            .Select(group => new CategoryItem(group.Key, group.Count()))
            .ToList();

        categories.Insert(0, new CategoryItem("All settings", searchMatches.Count, IsAll: true));
        CatList.ItemsSource = categories;
        CategoryCountText.Text = categories.Count == 1
            ? "0"
            : (categories.Count - 1).ToString();

        CategoryItem? selection = null;
        if (!string.IsNullOrEmpty(previousCategory))
        {
            selection = categories.FirstOrDefault(item =>
                !item.IsAll &&
                string.Equals(item.Name, previousCategory, StringComparison.Ordinal));
        }

        CatList.SelectedItem = selection ?? categories[0];
    }

    private IEnumerable<Entry> GetSearchMatches()
    {
        string query = SearchBox.Text.Trim();
        if (query.Length == 0)
            return _entries;

        return _entries.Where(entry =>
            ContainsInvariant(entry.Key, query) ||
            ContainsInvariant(entry.Desc, query) ||
            ContainsInvariant(entry.Cat, query));
    }

    private static bool ContainsInvariant(string value, string query) =>
        value.IndexOf(query, StringComparison.OrdinalIgnoreCase) >= 0;

    private void OnCatSelected(object sender, SelectionChangedEventArgs e)
    {
        if (CatList.SelectedItem is not CategoryItem selected)
            return;

        _selectedCat = selected.IsAll ? "" : selected.Name;
        if (!selected.IsAll && NewCat != null && !NewCat.IsKeyboardFocusWithin)
            NewCat.Text = selected.Name;

        BuildRows();
    }

    private void OnSearchChanged(object sender, TextChangedEventArgs e)
    {
        if (!_uiReady)
            return;

        // Search is global by default. Users can then narrow the result to a category.
        _selectedCat = "";
        RefreshCategoryList(keepSelection: false);
        UpdateSearchChrome();
    }

    private void OnClearSearch(object sender, RoutedEventArgs e)
    {
        SearchBox.Clear();
        SearchBox.Focus();
    }

    private void UpdateSearchChrome()
    {
        bool hasQuery = SearchBox.Text.Length > 0;
        SearchHint.Visibility = hasQuery ? Visibility.Collapsed : Visibility.Visible;
        SearchClearButton.Visibility = hasQuery ? Visibility.Visible : Visibility.Collapsed;
    }

    /// <summary>Builds the current category/search result as readable setting rows.</summary>
    private void BuildRows()
    {
        Rows.Children.Clear();

        List<Entry> visibleEntries = GetSearchMatches()
            .Where(entry => string.IsNullOrEmpty(_selectedCat) || entry.Cat == _selectedCat)
            .OrderBy(entry => entry.Cat, StringComparer.OrdinalIgnoreCase)
            .ThenBy(entry => entry.Key, StringComparer.OrdinalIgnoreCase)
            .ToList();

        SectionTitleText.Text = string.IsNullOrEmpty(_selectedCat)
            ? "All settings"
            : _selectedCat;

        string query = SearchBox.Text.Trim();
        ResultSummaryText.Text = query.Length == 0
            ? $"{visibleEntries.Count} setting{PluralSuffix(visibleEntries.Count)}"
            : $"{visibleEntries.Count} match{PluralSuffix(visibleEntries.Count)} for “{query}”";
        VisibleCountText.Text = $"{visibleEntries.Count} SHOWN";

        foreach (Entry entry in visibleEntries)
            Rows.Children.Add(BuildSettingRow(entry));

        if (visibleEntries.Count == 0)
            Rows.Children.Add(BuildEmptyState(query.Length > 0));
    }

    private FrameworkElement BuildSettingRow(Entry entry)
    {
        var row = new Border
        {
            BorderBrush = ResourceBrush("Hairline"),
            BorderThickness = new Thickness(0, 0, 0, 1),
            Padding = new Thickness(14, 11, 14, 11),
        };

        var layout = new Grid();
        layout.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        layout.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(300) });

        var identity = new StackPanel
        {
            Margin = new Thickness(0, 0, 24, 0),
            VerticalAlignment = VerticalAlignment.Center,
        };

        var metadata = new StackPanel { Orientation = Orientation.Horizontal };
        metadata.Children.Add(new TextBlock
        {
            Text = entry.Cat.ToUpperInvariant(),
            Foreground = ResourceBrush("SectionFg"),
            FontSize = 9.5,
            FontWeight = FontWeights.SemiBold,
        });
        metadata.Children.Add(new TextBlock
        {
            Text = $"  /  {TypeLabel(entry.Type)}",
            Foreground = ResourceBrush("TextDim"),
            FontSize = 9.5,
        });
        if (!entry.Builtin)
        {
            metadata.Children.Add(new TextBlock
            {
                Text = "  /  CUSTOM",
                Foreground = ResourceBrush("Accent"),
                FontSize = 9.5,
                FontWeight = FontWeights.SemiBold,
            });
        }
        identity.Children.Add(metadata);

        identity.Children.Add(new TextBlock
        {
            Text = entry.Key,
            Foreground = ResourceBrush("Text"),
            FontSize = 13,
            FontWeight = FontWeights.SemiBold,
            Margin = new Thickness(0, 3, 0, 0),
            TextWrapping = TextWrapping.Wrap,
        });

        if (!string.IsNullOrWhiteSpace(entry.Desc))
        {
            identity.Children.Add(new TextBlock
            {
                Text = entry.Desc,
                Foreground = ResourceBrush("TextDim"),
                FontSize = 11.5,
                Margin = new Thickness(0, 4, 0, 0),
                TextWrapping = TextWrapping.Wrap,
                LineHeight = 17,
            });
        }

        Grid.SetColumn(identity, 0);
        layout.Children.Add(identity);

        var editorArea = new DockPanel
        {
            LastChildFill = true,
            VerticalAlignment = VerticalAlignment.Center,
        };

        if (!entry.Builtin)
        {
            var removeButton = new Button
            {
                Content = "Remove",
                Padding = new Thickness(9, 3, 9, 3),
                Margin = new Thickness(8, 0, 0, 0),
                ToolTip = $"Remove {entry.Cat}.{entry.Key}",
            };
            string category = entry.Cat;
            string key = entry.Key;
            removeButton.Click += (_, _) =>
            {
                EngineInterop.acs_editor_settings_remove(_engine, category, key);
                _onChanged();
                Reload(keepSelection: true);
            };
            DockPanel.SetDock(removeButton, Dock.Right);
            editorArea.Children.Add(removeButton);
        }

        editorArea.Children.Add(BuildValueEditor(entry));
        Grid.SetColumn(editorArea, 1);
        layout.Children.Add(editorArea);

        row.Child = layout;
        return row;
    }

    private FrameworkElement BuildEmptyState(bool searched)
    {
        var panel = new StackPanel
        {
            Margin = new Thickness(22, 32, 22, 32),
            HorizontalAlignment = HorizontalAlignment.Center,
        };
        panel.Children.Add(new TextBlock
        {
            Text = searched ? "No settings match this search." : "This category has no settings.",
            FontSize = 13,
            FontWeight = FontWeights.SemiBold,
            HorizontalAlignment = HorizontalAlignment.Center,
        });
        panel.Children.Add(new TextBlock
        {
            Text = searched
                ? "Try a key, description, or category name."
                : "Add a custom setting below when the project needs one.",
            Foreground = ResourceBrush("TextDim"),
            Margin = new Thickness(0, 5, 0, 0),
            HorizontalAlignment = HorizontalAlignment.Center,
        });
        return panel;
    }

    /// <summary>Creates the value control used by the existing setting type ABI.</summary>
    private FrameworkElement BuildValueEditor(Entry entry)
    {
        string category = entry.Cat;
        string key = entry.Key;

        switch (entry.Type)
        {
            case 2:
            {
                var checkBox = new CheckBox
                {
                    Content = "Enabled",
                    IsChecked = entry.Value == "1" ||
                                entry.Value.StartsWith("t", StringComparison.OrdinalIgnoreCase),
                    VerticalAlignment = VerticalAlignment.Center,
                };
                checkBox.Checked += (_, _) => Commit(category, key, "1");
                checkBox.Unchecked += (_, _) => Commit(category, key, "0");
                return checkBox;
            }
            case 5:
            {
                var comboBox = new ComboBox
                {
                    MinWidth = 160,
                    VerticalAlignment = VerticalAlignment.Center,
                };

                foreach (string option in entry.Options.Split('|', StringSplitOptions.RemoveEmptyEntries))
                    comboBox.Items.Add(option);

                if (comboBox.Items.Count > 0)
                {
                    comboBox.SelectedItem = comboBox.Items
                        .Cast<string>()
                        .FirstOrDefault(option => option == entry.Value) ?? comboBox.Items[0];
                }

                comboBox.SelectionChanged += (_, _) =>
                {
                    if (comboBox.SelectedItem is not string value)
                        return;
                    if (TryValidateSettingValue(entry, value, out string normalized, out _))
                        Commit(category, key, normalized);
                };
                return comboBox;
            }
            default:
            {
                var textBox = new TextBox
                {
                    Text = entry.Value,
                    MinWidth = 180,
                    VerticalAlignment = VerticalAlignment.Center,
                };

                // String (including every custom setting) deliberately remains unrestricted.
                // Typed built-ins are validated here before the native ABI or persistence callback.
                var validationText = new TextBlock
                {
                    Foreground = ResourceBrush("WarnFg"),
                    FontSize = 10.5,
                    Margin = new Thickness(1, 4, 0, 0),
                    TextWrapping = TextWrapping.Wrap,
                    Visibility = Visibility.Collapsed,
                };
                string lastCommittedValue = entry.Value;

                void ClearValidation()
                {
                    validationText.Text = "";
                    validationText.Visibility = Visibility.Collapsed;
                    textBox.ClearValue(Control.BorderBrushProperty);
                    textBox.ClearValue(ToolTipProperty);
                }

                void ShowValidation(string message)
                {
                    validationText.Text = message;
                    validationText.Visibility = Visibility.Visible;
                    textBox.BorderBrush = ResourceBrush("WarnFg");
                    textBox.ToolTip = message;
                }

                bool Apply()
                {
                    if (!TryValidateSettingValue(entry, textBox.Text, out string normalized,
                                                 out string validationError))
                    {
                        ShowValidation(validationError);
                        return false;
                    }
                    if (!Commit(category, key, normalized))
                    {
                        ShowValidation("The engine rejected this value; the saved value was not changed.");
                        return false;
                    }

                    lastCommittedValue = normalized;
                    ClearValidation();
                    return true;
                }

                textBox.TextChanged += (_, _) =>
                {
                    if (validationText.Visibility != Visibility.Visible)
                        return;
                    if (TryValidateSettingValue(entry, textBox.Text, out _, out string error))
                        ClearValidation();
                    else
                        ShowValidation(error);
                };
                textBox.LostKeyboardFocus += (_, _) => Apply();
                textBox.KeyDown += (_, args) =>
                {
                    if (args.Key == Key.Enter)
                    {
                        if (Apply())
                            Keyboard.ClearFocus();
                        args.Handled = true;
                    }
                    else if (args.Key == Key.Escape)
                    {
                        textBox.Text = lastCommittedValue;
                        ClearValidation();
                        textBox.SelectAll();
                        args.Handled = true;
                    }
                };

                var editor = new StackPanel();
                editor.Children.Add(textBox);
                editor.Children.Add(validationText);
                return editor;
            }
        }
    }

    private static bool TryValidateSettingValue(
        Entry entry, string input, out string normalized, out string error)
    {
        normalized = input;
        error = "";

        switch (entry.Type)
        {
            case 0:
            {
                normalized = input.Trim();
                if (!float.TryParse(normalized, NumberStyles.Float, CultureInfo.InvariantCulture,
                                    out float value) || !float.IsFinite(value))
                {
                    error = "Enter one finite number, using a period as the decimal separator.";
                    return false;
                }
                return true;
            }
            case 1:
            {
                normalized = input.Trim();
                if (!int.TryParse(normalized, NumberStyles.Integer, CultureInfo.InvariantCulture,
                                  out _))
                {
                    error = "Enter a whole number from -2,147,483,648 to 2,147,483,647.";
                    return false;
                }
                return true;
            }
            case 3:
            {
                string[] components = input.Split(',');
                if (components.Length != 3)
                {
                    error = "Enter exactly three color components: red, green, blue.";
                    return false;
                }

                for (int index = 0; index < components.Length; index++)
                {
                    components[index] = components[index].Trim();
                    if (!float.TryParse(components[index], NumberStyles.Float,
                                        CultureInfo.InvariantCulture, out float value) ||
                        !float.IsFinite(value))
                    {
                        error = "Each color component must be a finite number (for example 0.2,0.5,1.0).";
                        return false;
                    }
                }
                normalized = string.Join(',', components);
                return true;
            }
            case 5:
            {
                bool isOption = entry.Options
                    .Split('|', StringSplitOptions.RemoveEmptyEntries)
                    .Contains(input, StringComparer.Ordinal);
                if (!isOption)
                {
                    error = "Choose one of the available options.";
                    return false;
                }
                return true;
            }
            default:
                // Bool uses a checkbox. String/custom values intentionally accept any text.
                return true;
        }
    }

    private bool Commit(string category, string key, string value)
    {
        int index = _entries.FindIndex(entry => entry.Cat == category && entry.Key == key);
        if (index >= 0 && string.Equals(_entries[index].Value, value, StringComparison.Ordinal))
            return true;

        if (EngineInterop.acs_editor_settings_set(_engine, category, key, value) == 0)
            return false;

        if (index >= 0)
            _entries[index] = _entries[index] with { Value = value };
        _onChanged();
        return true;
    }

    private void OnAddCustom(object sender, RoutedEventArgs e)
    {
        string category = NewCat.Text.Trim();
        string key = NewKey.Text.Trim();
        if (category.Length == 0 || key.Length == 0)
        {
            ShowAddValidation("Category and key are required.");
            return;
        }

        if (EngineInterop.acs_editor_settings_add(_engine, category, key, NewValue.Text) == 0)
        {
            ShowAddValidation("The setting could not be added. The key may already exist in this category.");
            return;
        }

        _onChanged();
        _selectedCat = category;
        NewKey.Clear();
        NewValue.Clear();
        HideAddValidation();
        Reload(keepSelection: true);
        NewKey.Focus();
    }

    private void OnCustomFieldChanged(object sender, TextChangedEventArgs e)
    {
        if (!_uiReady)
            return;

        UpdateAddButton();
        HideAddValidation();
    }

    private void OnCustomInputKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key != Key.Enter || AddCustomButton.IsEnabled == false)
            return;

        OnAddCustom(AddCustomButton, new RoutedEventArgs());
        e.Handled = true;
    }

    private void UpdateAddButton()
    {
        AddCustomButton.IsEnabled =
            !string.IsNullOrWhiteSpace(NewCat.Text) &&
            !string.IsNullOrWhiteSpace(NewKey.Text);
    }

    private void ShowAddValidation(string message)
    {
        AddValidationText.Text = message;
        AddValidationText.Visibility = Visibility.Visible;
    }

    private void HideAddValidation()
    {
        AddValidationText.Visibility = Visibility.Collapsed;
        AddValidationText.Text = "";
    }

    private void OnWindowPreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.F && Keyboard.Modifiers.HasFlag(ModifierKeys.Control))
        {
            SearchBox.Focus();
            SearchBox.SelectAll();
            e.Handled = true;
            return;
        }

        if (e.Key == Key.Escape && SearchBox.IsKeyboardFocusWithin && SearchBox.Text.Length > 0)
        {
            SearchBox.Clear();
            e.Handled = true;
        }
    }

    private void OnTitleBarMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ClickCount == 2)
        {
            ToggleMaximizeRestore();
            return;
        }

        if (e.ButtonState == MouseButtonState.Pressed)
            DragMove();
    }

    private void OnMinimizeWindow(object sender, RoutedEventArgs e) =>
        WindowState = WindowState.Minimized;

    private void OnMaximizeRestoreWindow(object sender, RoutedEventArgs e) =>
        ToggleMaximizeRestore();

    private void OnCloseWindow(object sender, RoutedEventArgs e) =>
        Close();

    private void ToggleMaximizeRestore() =>
        WindowState = WindowState == WindowState.Maximized
            ? WindowState.Normal
            : WindowState.Maximized;

    private Brush ResourceBrush(string key) => (Brush)FindResource(key);

    private static string PluralSuffix(int count) => count == 1 ? "" : "s";

    private static string TypeLabel(int type) => type switch
    {
        0 => "FLOAT",
        1 => "INTEGER",
        2 => "BOOLEAN",
        3 => "COLOR",
        5 => "ENUM",
        _ => "STRING",
    };
}

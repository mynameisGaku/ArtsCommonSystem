// SPDX-License-Identifier: Apache-2.0
// Project Settings ウィンドウ (UE の Project Settings 相当)。
// 左 = カテゴリ一覧、右 = 選択カテゴリの設定項目。値の実体と INI シリアライズは
// ABI (FProjectSettings) が持ち、このウィンドウは UI のみを担う。変更は即時に
// エンジンへ適用され、onChanged コールバック (MainWindow) が INI へ保存する。
using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;

namespace AcsEditor;

public partial class ProjectSettingsWindow : Window
{
    private readonly IntPtr _engine;
    private readonly Action _onChanged;          // 変更後の保存 + 反映 (MainWindow 側)
    private List<Entry> _entries = new();
    private string _selectedCat = "";

    private sealed record Entry(string Cat, string Key, string Value, int Type,
                                string Options, bool Builtin, string Desc);

    public ProjectSettingsWindow(Window owner, IntPtr engine, Action onChanged)
    {
        InitializeComponent();
        Owner      = owner;
        _engine    = engine;
        _onChanged = onChanged;
        Reload(keepSelection: false);
    }

    /// <summary>ABI から全エントリを取り直し、カテゴリ一覧と行を再構築する。</summary>
    private void Reload(bool keepSelection)
    {
        string prev = keepSelection ? _selectedCat : "";
        _entries.Clear();
        int n = EngineInterop.acs_editor_settings_count(_engine);
        var buf = new byte[1024];
        for (int i = 0; i < n; i++)
        {
            Array.Clear(buf, 0, buf.Length);
            if (EngineInterop.acs_editor_settings_entry(_engine, i, buf, buf.Length) == 0) continue;
            string[] f = EngineInterop.Utf8Z(buf).Split('\t');
            if (f.Length < 7) continue;
            _entries.Add(new Entry(f[0], f[1], f[2], int.TryParse(f[3], out var t) ? t : 4,
                                   f[4], f[5] == "1", f[6]));
        }
        var cats = _entries.Select(e => e.Cat).Distinct().ToList();
        CatList.ItemsSource = cats;
        int sel = cats.IndexOf(prev);
        CatList.SelectedIndex = (sel >= 0) ? sel : (cats.Count > 0 ? 0 : -1);
    }

    private void OnCatSelected(object sender, SelectionChangedEventArgs e)
    {
        if (CatList.SelectedItem is not string cat) return;
        _selectedCat = cat;
        if (NewCat != null) NewCat.Text = cat;     // 追加行のカテゴリを選択中に追従
        BuildRows();
    }

    /// <summary>選択カテゴリの設定行を生成する。型ごとに編集 UI を変える。</summary>
    private void BuildRows()
    {
        Rows.Children.Clear();
        foreach (var en in _entries.Where(x => x.Cat == _selectedCat))
        {
            var panel = new DockPanel { Margin = new Thickness(0, 3, 0, 3) };
            var label = new TextBlock
            {
                Text = en.Key, Width = 170, VerticalAlignment = VerticalAlignment.Center,
                FontFamily = new System.Windows.Media.FontFamily("Consolas"),
                ToolTip = string.IsNullOrEmpty(en.Desc) ? null : en.Desc,
            };
            DockPanel.SetDock(label, Dock.Left);
            panel.Children.Add(label);

            if (!en.Builtin)                        // ユーザー定義は削除可
            {
                var del = new Button { Content = "✕", Padding = new Thickness(7, 2, 7, 2), Margin = new Thickness(6, 0, 0, 0) };
                var (c, k) = (en.Cat, en.Key);
                del.Click += (_, __) =>
                {
                    EngineInterop.acs_editor_settings_remove(_engine, c, k);
                    _onChanged();
                    Reload(keepSelection: true);
                };
                DockPanel.SetDock(del, Dock.Right);
                panel.Children.Add(del);
            }

            panel.Children.Add(BuildValueEditor(en));
            Rows.Children.Add(panel);
        }
        if (Rows.Children.Count == 0)
            Rows.Children.Add(new TextBlock { Text = "(項目なし)", Foreground = (System.Windows.Media.Brush)FindResource("TextDim") });
    }

    /// <summary>型 (ESettingType: 0=Float 1=Int 2=Bool 3=Color 4=String 5=Enum) に応じた値エディタ。</summary>
    private FrameworkElement BuildValueEditor(Entry en)
    {
        var (cat, key) = (en.Cat, en.Key);
        switch (en.Type)
        {
            case 2:   // Bool → チェックボックス
            {
                var cb = new CheckBox { IsChecked = en.Value == "1" || en.Value.StartsWith("t"), VerticalAlignment = VerticalAlignment.Center };
                cb.Checked   += (_, __) => Commit(cat, key, "1");
                cb.Unchecked += (_, __) => Commit(cat, key, "0");
                return cb;
            }
            case 5:   // Enum → ドロップダウン
            {
                var combo = new ComboBox { MinWidth = 130, VerticalAlignment = VerticalAlignment.Center };
                foreach (var op in en.Options.Split('|')) combo.Items.Add(op);
                combo.SelectedItem = combo.Items.Cast<string>().FirstOrDefault(s => s == en.Value) ?? combo.Items[0];
                combo.SelectionChanged += (_, __) => { if (combo.SelectedItem is string s) Commit(cat, key, s); };
                return combo;
            }
            default:  // Float / Int / Color / String → テキストボックス (Color は "r,g,b")
            {
                var tb = new TextBox { Text = en.Value, MinWidth = 200, VerticalAlignment = VerticalAlignment.Center };
                void Apply() { Commit(cat, key, tb.Text); }
                tb.LostKeyboardFocus += (_, __) => Apply();
                tb.KeyDown += (_, ev) => { if (ev.Key == Key.Enter) { Apply(); Keyboard.ClearFocus(); } };
                return tb;
            }
        }
    }

    private void Commit(string cat, string key, string value)
    {
        if (EngineInterop.acs_editor_settings_set(_engine, cat, key, value) != 0)
        {
            var i = _entries.FindIndex(x => x.Cat == cat && x.Key == key);
            if (i >= 0) _entries[i] = _entries[i] with { Value = value };
            _onChanged();
        }
    }

    private void OnAddCustom(object sender, RoutedEventArgs e)
    {
        string cat = NewCat.Text.Trim();
        string key = NewKey.Text.Trim();
        if (cat.Length == 0 || key.Length == 0) return;
        if (EngineInterop.acs_editor_settings_add(_engine, cat, key, NewValue.Text) != 0)
        {
            _onChanged();
            _selectedCat = cat;
            NewKey.Text = ""; NewValue.Text = "";
            Reload(keepSelection: true);
        }
    }
}

// ログのタグ付け / フィルタ / 文字列検索 / ソート。
// Log(msg) は内容からタグ + 重大度を自動分類。Log(msg, tag, level) で明示も可能。
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Media;

namespace AcsEditor
{
    /// <summary>ログ行の重大度。チップ色 / メッセージ色 / フィルタに使う。</summary>
    public enum LogLevel { Info, Warn, Error, Success }

    /// <summary>1 行のログ (時刻 + タグ + 重大度 + 本文)。色は重大度/タグから算出。</summary>
    public sealed class LogEntry
    {
        public long     Seq     { get; init; }
        public DateTime Time    { get; init; }
        public string   Tag     { get; init; } = "General";
        public LogLevel Level   { get; init; }
        public string   Message { get; init; } = "";

        public string TimeText => Time.ToString("HH:mm:ss");

        public Brush MsgBrush => Level switch
        {
            LogLevel.Error   => LogTheme.ErrorText,
            LogLevel.Warn    => LogTheme.WarnText,
            LogLevel.Success => LogTheme.SuccessText,
            _                => LogTheme.InfoText,
        };
        public Brush ChipBrush => LogTheme.ChipBg(Tag);
        public Brush ChipFg    => LogTheme.ChipFg;
    }

    /// <summary>ログの配色テーブル (凍結ブラシ。タグごとに識別色)。</summary>
    internal static class LogTheme
    {
        private static SolidColorBrush B(uint argb)
        {
            var b = new SolidColorBrush(Color.FromArgb((byte)(argb >> 24), (byte)(argb >> 16), (byte)(argb >> 8), (byte)argb));
            b.Freeze();
            return b;
        }

        public static readonly Brush InfoText    = B(0xFFB7BEC8);
        public static readonly Brush WarnText    = B(0xFFE8C56A);
        public static readonly Brush ErrorText   = B(0xFFEC6A66);
        public static readonly Brush SuccessText = B(0xFF7FCF96);
        public static readonly Brush ChipFg      = B(0xFF0E1116);   // チップ上の暗いテキスト

        private static readonly Dictionary<string, Brush> _tagBg = new()
        {
            ["3D"]       = B(0xFF5BBFD0),
            ["Build"]    = B(0xFFE0A152),
            ["Reload"]   = B(0xFFB58BD8),
            ["Play"]     = B(0xFF7FCF96),
            ["Scene"]    = B(0xFF6FA8E8),
            ["Material"] = B(0xFFD98BB0),
            ["Asset"]    = B(0xFF67C7B8),
            ["Settings"] = B(0xFFC2A86A),
            ["General"]  = B(0xFF8B93A0),
        };
        public static Brush ChipBg(string tag) => _tagBg.TryGetValue(tag, out var b) ? b : _tagBg["General"];
    }

    public partial class MainWindow
    {
        private readonly ObservableCollection<LogEntry> _logAll = new();
        private ICollectionView? _logView;
        private long _logSeq;

        /// <summary>コンストラクタから呼ぶ: ビューを作り ConsoleList に束縛する。</summary>
        private void InitLog()
        {
            _logView = CollectionViewSource.GetDefaultView(_logAll);
            _logView.Filter = LogFilterPredicate;
            _logView.SortDescriptions.Add(new SortDescription(nameof(LogEntry.Seq), ListSortDirection.Ascending));
            ConsoleList.ItemsSource = _logView;
        }

        /// <summary>内容からタグ + 重大度を推定する。</summary>
        private static (string tag, LogLevel level) ClassifyLog(string msg)
        {
            string m = msg.ToLowerInvariant();
            LogLevel level = LogLevel.Info;
            if (m.Contains("error") || m.Contains("fail") || m.Contains("失敗") || m.Contains("エラー") || m.Contains("不正"))
                level = LogLevel.Error;
            else if (m.Contains("warn") || m.Contains("警告"))
                level = LogLevel.Warn;
            else if (m.Contains("✓") || m.Contains("成功") || m.Contains("saved") || m.Contains("完了") || m.Contains(" done") || m.Contains("reparented"))
                level = LogLevel.Success;

            string tag = "General";
            if (msg.StartsWith("3D:") || m.Contains("3d ") || m.Contains("ビューポート")) tag = "3D";
            else if (m.Contains("hot reload") || m.Contains("reload") || m.Contains("リロード")) tag = "Reload";
            else if (m.Contains("build") || m.Contains("ビルド")) tag = "Build";
            else if (m.Contains("play") || m.Contains("run ") || m.Contains("実行")) tag = "Play";
            // Scene を先に判定 (メッセージにシーン関連語があればパス中の "Assets" より優先)。
            else if (m.Contains("scene") || m.Contains("node") || m.Contains("hierarchy")
                  || m.Contains("reparent") || m.Contains("moved") || m.Contains("spawn") || m.Contains("シーン") || m.Contains("ノード")) tag = "Scene";
            else if (m.Contains("material") || m.Contains("マテリアル")) tag = "Material";
            else if (m.Contains("import") || m.Contains("アセット") || m.Contains("asset browser")) tag = "Asset";
            else if (m.Contains("settings") || m.Contains("設定")) tag = "Settings";
            return (tag, level);
        }

        /// <summary>タグ/重大度を明示してログを追加する。</summary>
        private void Log(string msg, string tag, LogLevel level)
        {
            if (!Dispatcher.CheckAccess()) { Dispatcher.BeginInvoke(() => Log(msg, tag, level)); return; }
            _logAll.Add(new LogEntry { Seq = ++_logSeq, Time = DateTime.Now, Tag = tag, Level = level, Message = msg });
            // 自動スクロール (表示順の末尾へ)。新しい順表示なら先頭、古い順なら末尾。
            if (_logView != null && ConsoleList.Items.Count > 0)
            {
                var last = ConsoleList.Items[_logNewestFirst ? 0 : ConsoleList.Items.Count - 1];
                if (last != null) ConsoleList.ScrollIntoView(last);
            }
        }

        private bool LogFilterPredicate(object o)
        {
            if (o is not LogEntry e) return false;
            bool levelOk = e.Level switch
            {
                LogLevel.Error => FltError.IsChecked == true,
                LogLevel.Warn  => FltWarn.IsChecked == true,
                _              => FltInfo.IsChecked == true,   // Info + Success は Info トグル
            };
            if (!levelOk) return false;
            string q = LogSearch.Text?.Trim() ?? "";
            if (q.Length == 0) return true;
            return e.Message.Contains(q, StringComparison.OrdinalIgnoreCase)
                || e.Tag.Contains(q, StringComparison.OrdinalIgnoreCase);
        }

        // TextBox.TextChanged と ToggleButton.Click の両方から呼ぶ (RoutedEventArgs は両者の基底)。
        private void OnLogFilterChanged(object sender, RoutedEventArgs e) => _logView?.Refresh();

        private bool _logNewestFirst;
        private void OnLogSortToggle(object sender, RoutedEventArgs e)
        {
            _logNewestFirst = SortNewest.IsChecked == true;
            SortNewest.Content = _logNewestFirst ? "↑ Old" : "↓ New";
            if (_logView == null) return;
            _logView.SortDescriptions.Clear();
            _logView.SortDescriptions.Add(new SortDescription(nameof(LogEntry.Seq),
                _logNewestFirst ? ListSortDirection.Descending : ListSortDirection.Ascending));
        }

        private void OnClearLog(object sender, RoutedEventArgs e) => _logAll.Clear();
    }
}

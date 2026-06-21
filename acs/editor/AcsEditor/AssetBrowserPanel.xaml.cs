using System;
using System.Collections.ObjectModel;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace AcsEditor;

/// <summary>アセットがダブルクリック/ドラッグで起動されたとき。</summary>
public sealed class AssetActivatedEventArgs : EventArgs
{
    public string FullPath { get; }
    public string Kind { get; }   // "image" | "audio" | "mesh" | "text" | "scene" | "file"
    public AssetActivatedEventArgs(string path, string kind) { FullPath = path; Kind = kind; }
}

/// <summary>1 アセット (ファイル or フォルダ) のタイル表示用データ。</summary>
public sealed class AssetItem
{
    public string FullPath { get; init; } = "";
    public string Name { get; init; } = "";
    public string Kind { get; init; } = "file";
    public bool IsDirectory { get; init; }
    public string Glyph { get; init; } = "";
    public Brush GlyphBrush { get; init; } = Brushes.Gray;
    public ImageSource? Thumb { get; init; }
}

/// <summary>プロジェクトの Assets フォルダを走査・分類して表示し、ノードへの割当 / インポートを行う。</summary>
public partial class AssetBrowserPanel : UserControl
{
    public ObservableCollection<AssetItem> Items { get; } = new();

    /// <summary>エンジンハンドル (.acsmat サムネイルを実シェーダ GPU プレビューで描く。0=CPU フォールバック)。</summary>
    public IntPtr Engine { get; set; } = IntPtr.Zero;

    private Project? _project;
    private string _currentDir = "";
    private string _filter = "";   // 検索フィルタ (名前部分一致)
    private FileSystemWatcher? _watcher;
    private readonly DispatcherTimer _debounce;
    private Point _dragStart;
    private bool _maybeDrag;

    /// <summary>画像/音声などのアセットがアクティブ化されたとき (MainWindow が割当を担う)。</summary>
    public event EventHandler<AssetActivatedEventArgs>? AssetActivated;
    /// <summary>右クリック「シーンに配置」。Blueprint/Prefab をシーンへ実体化する要求。</summary>
    public event EventHandler<AssetActivatedEventArgs>? AssetPlace;
    /// <summary>右クリック「Blueprint に変換」。旧 Prefab を .acsbp へ移行する要求。</summary>
    public event EventHandler<AssetActivatedEventArgs>? AssetConvert;
    /// <summary>コンソールへのログ出力。</summary>
    public event Action<string>? Log;

    public AssetBrowserPanel()
    {
        InitializeComponent();
        Tiles.ItemsSource = Items;
        _debounce = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(250) };
        _debounce.Tick += (_, _) => { _debounce.Stop(); Refresh(); };
        ClearPreview();
    }

    /// <summary>表示対象のプロジェクトを設定し、Assets フォルダの監視と一覧表示を開始する。</summary>
    public void SetProject(Project? project)
    {
        _project = project;
        DisposeWatcher();
        if (project == null) { _currentDir = ""; Items.Clear(); PathText.Text = ""; return; }

        Directory.CreateDirectory(project.AssetsDir);   // 念のため
        _currentDir = project.AssetsDir;

        try
        {
            _watcher = new FileSystemWatcher(project.AssetsDir)
            {
                IncludeSubdirectories = true,
                NotifyFilter = NotifyFilters.FileName | NotifyFilters.DirectoryName | NotifyFilters.LastWrite,
                EnableRaisingEvents = true,
            };
            _watcher.Created += OnFsEvent; _watcher.Deleted += OnFsEvent;
            _watcher.Renamed += OnFsEvent; _watcher.Changed += OnFsEvent;
        }
        catch { /* 監視できなくても手動更新で動く */ }

        Refresh();
    }

    private void OnFsEvent(object sender, FileSystemEventArgs e) =>
        Dispatcher.BeginInvoke(() => { _debounce.Stop(); _debounce.Start(); });

    private void DisposeWatcher()
    {
        if (_watcher != null) { _watcher.EnableRaisingEvents = false; _watcher.Dispose(); _watcher = null; }
    }

    public void Refresh()
    {
        Items.Clear();
        if (_project == null || string.IsNullOrEmpty(_currentDir) || !Directory.Exists(_currentDir)) return;

        PathText.Text = RelDisplay(_currentDir);
        UpBtn.IsEnabled = !PathEquals(_currentDir, _project.AssetsDir);

        try
        {
            foreach (string dir in Directory.GetDirectories(_currentDir))
            {
                var di = new DirectoryInfo(dir);
                if (!PassFilter(di.Name)) continue;
                Items.Add(new AssetItem
                {
                    FullPath = dir, Name = di.Name, Kind = "dir", IsDirectory = true,
                    Glyph = "📁", GlyphBrush = MakeBrush(0xC9, 0xA2, 0x5A),
                });
            }
            foreach (string file in Directory.GetFiles(_currentDir))
            {
                if (!PassFilter(Path.GetFileName(file))) continue;
                string ext = Path.GetExtension(file).ToLowerInvariant();
                string kind = ClassifyExt(ext);
                Items.Add(new AssetItem
                {
                    FullPath = file, Name = Path.GetFileName(file), Kind = kind, IsDirectory = false,
                    Glyph = GlyphFor(kind), GlyphBrush = BrushFor(kind),
                    Thumb = kind == "image" ? TryThumb(file)
                          : kind == "material" ? TryMaterialThumb(file) : null,
                });
            }
        }
        catch (Exception ex) { Log?.Invoke("Asset enumerate error: " + ex.Message); }
    }

    // ===== ナビゲーション =====
    private void OnUp(object sender, RoutedEventArgs e)
    {
        if (_project == null || PathEquals(_currentDir, _project.AssetsDir)) return;
        string? parent = Path.GetDirectoryName(_currentDir);
        if (parent != null && parent.Length >= _project.AssetsDir.Length) { _currentDir = parent; Refresh(); }
    }

    private void OnRefresh(object sender, RoutedEventArgs e) => Refresh();

    private bool PassFilter(string name) =>
        _filter.Length == 0 || name.Contains(_filter, StringComparison.OrdinalIgnoreCase);

    private void OnSearchChanged(object sender, TextChangedEventArgs e)
    {
        _filter = SearchBox.Text?.Trim() ?? "";
        Refresh();
    }

    // タイル選択でプレビュー + 情報を更新 (参考エンジン風)。
    private void OnTileSelected(object sender, SelectionChangedEventArgs e)
    {
        if (Tiles.SelectedItem is not AssetItem item) { ClearPreview(); return; }

        PreviewNameText.Text = item.Name;
        string meta = item.IsDirectory ? "フォルダ" : KindLabel(item.Kind);
        if (!item.IsDirectory)
        {
            try { meta += "   " + FormatSize(new FileInfo(item.FullPath).Length); } catch { }
        }
        meta += "\n" + RelDisplay(item.FullPath);
        PreviewMetaText.Text = meta;

        // プレビュー画像: マテリアル=球 / 画像=その絵 / それ以外=大きいグリフ。
        ImageSource? img = null;
        if (!item.IsDirectory)
        {
            if (item.Kind == "material") img = TryMaterialPreview(item.FullPath, 256);
            else if (item.Kind == "image") img = TryThumb(item.FullPath, 320);
        }
        if (img != null)
        {
            PreviewImage.Source = img;
            PreviewImage.Visibility = Visibility.Visible;
            PreviewGlyphBox.Visibility = Visibility.Collapsed;
        }
        else
        {
            PreviewImage.Source = null;
            PreviewImage.Visibility = Visibility.Collapsed;
            PreviewGlyphBox.Visibility = Visibility.Visible;
            PreviewGlyph.Text = item.IsDirectory ? "📁" : item.Glyph;
        }
    }

    private void ClearPreview()
    {
        PreviewNameText.Text = "";
        PreviewMetaText.Text = "アセットを選択";
        PreviewImage.Source = null;
        PreviewImage.Visibility = Visibility.Collapsed;
        PreviewGlyphBox.Visibility = Visibility.Visible;
        PreviewGlyph.Text = "";
    }

    private static string KindLabel(string kind) => kind switch
    {
        "image" => "画像", "audio" => "音声", "mesh" => "メッシュ", "text" => "テキスト",
        "scene" => "シーン", "project" => "プロジェクト", "material" => "マテリアル",
        "prefab" => "プレハブ", "blueprint" => "Blueprint", "dir" => "フォルダ", _ => "ファイル",
    };

    private static string FormatSize(long bytes)
    {
        if (bytes < 1024) return $"{bytes} B";
        double kb = bytes / 1024.0;
        if (kb < 1024) return $"{kb:0.#} KB";
        double mb = kb / 1024.0;
        return mb < 1024 ? $"{mb:0.#} MB" : $"{mb / 1024.0:0.#} GB";
    }

    private void OnTileDoubleClick(object sender, MouseButtonEventArgs e)
    {
        if (Tiles.SelectedItem is not AssetItem item) return;
        if (item.IsDirectory) { _currentDir = item.FullPath; Refresh(); return; }
        AssetActivated?.Invoke(this, new AssetActivatedEventArgs(item.FullPath, item.Kind));
    }

    // 右クリックで対象タイルを選択する (コンテキストメニューが選択アイテムに作用するように)。
    private void OnTileRightDown(object sender, MouseButtonEventArgs e)
    {
        if ((e.OriginalSource as FrameworkElement)?.DataContext is AssetItem it) Tiles.SelectedItem = it;
    }

    private void OnCtxOpen(object sender, RoutedEventArgs e)
    {
        if (Tiles.SelectedItem is AssetItem item && !item.IsDirectory)
            AssetActivated?.Invoke(this, new AssetActivatedEventArgs(item.FullPath, item.Kind));
    }

    private void OnCtxPlace(object sender, RoutedEventArgs e)
    {
        if (Tiles.SelectedItem is AssetItem item && !item.IsDirectory)
            AssetPlace?.Invoke(this, new AssetActivatedEventArgs(item.FullPath, item.Kind));
    }

    private void OnCtxConvert(object sender, RoutedEventArgs e)
    {
        if (Tiles.SelectedItem is AssetItem item && !item.IsDirectory)
            AssetConvert?.Invoke(this, new AssetActivatedEventArgs(item.FullPath, item.Kind));
    }

    // ===== インポート (Assets/現在フォルダへコピー) =====
    private void OnImport(object sender, RoutedEventArgs e)
    {
        if (_project == null) { Log?.Invoke("インポートにはプロジェクトが必要です。"); return; }
        var dlg = new Microsoft.Win32.OpenFileDialog
        {
            Title = "アセットをインポート",
            Multiselect = true,
            Filter = "All assets|*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif;*.wav;*.ogg;*.mp3;*.flac;*.fbx;*.gltf;*.glb;*.obj;*.txt;*.json|All files (*.*)|*.*",
        };
        if (dlg.ShowDialog() != true) return;
        int n = 0;
        foreach (string src in dlg.FileNames)
        {
            try
            {
                string dst = Path.Combine(_currentDir, Path.GetFileName(src));
                dst = UniquePath(dst);
                File.Copy(src, dst);
                n++;
            }
            catch (Exception ex) { Log?.Invoke("Import failed: " + ex.Message); }
        }
        Log?.Invoke($"{n} 個のアセットをインポートしました → {RelDisplay(_currentDir)}");
        Refresh();
    }

    // ===== ドラッグ (ノード/インスペクタへ ASSET_PATH を渡す) =====
    private void OnTileMouseDown(object sender, MouseButtonEventArgs e)
    {
        _dragStart = e.GetPosition(this);
        _maybeDrag = Tiles.SelectedItem is AssetItem || (e.OriginalSource as FrameworkElement)?.DataContext is AssetItem;
    }

    private void OnTilePreviewMouseMove(object sender, MouseEventArgs e)
    {
        if (!_maybeDrag || e.LeftButton != MouseButtonState.Pressed) return;
        Point p = e.GetPosition(this);
        if (Math.Abs(p.X - _dragStart.X) < 5 && Math.Abs(p.Y - _dragStart.Y) < 5) return;
        if (Tiles.SelectedItem is not AssetItem item || item.IsDirectory) { _maybeDrag = false; return; }
        _maybeDrag = false;
        var data = new DataObject();
        data.SetData("ASSET_PATH", item.FullPath);
        data.SetData(DataFormats.FileDrop, new[] { item.FullPath });
        try { DragDrop.DoDragDrop(Tiles, data, DragDropEffects.Copy); } catch { }
    }

    // ===== 分類・グリフ・サムネイル =====
    private static string ClassifyExt(string ext) => ext switch
    {
        ".png" or ".jpg" or ".jpeg" or ".bmp" or ".tga" or ".dds" or ".ktx" or ".hdr" or ".gif" => "image",
        ".wav" or ".ogg" or ".mp3" or ".flac" => "audio",
        ".fbx" or ".gltf" or ".glb" or ".obj" or ".mdl" => "mesh",
        ".txt" or ".json" or ".xml" or ".yaml" or ".yml" or ".toml" or ".ini" or ".csv" or ".md"
            or ".log" or ".hlsl" or ".glsl" or ".lua" => "text",
        ".acscene" => "scene",
        ".acsproject" => "project",
        ".acsmat" => "material",
        ".acsprefab" => "prefab",
        ".acsbp" => "blueprint",
        _ => "file",
    };

    private static string GlyphFor(string kind) => kind switch
    {
        "image" => "IMG", "audio" => "AUD", "mesh" => "MESH", "text" => "TXT",
        "scene" => "SCN", "project" => "PROJ", "material" => "MAT", "prefab" => "PREF",
        "blueprint" => "BP", _ => "FILE",
    };

    private static Brush BrushFor(string kind) => kind switch
    {
        "image" => MakeBrush(0x3C, 0x9A, 0xE8),
        "audio" => MakeBrush(0x4C, 0xB0, 0x6B),
        "mesh"  => MakeBrush(0xC2, 0x6A, 0xD6),
        "text"  => MakeBrush(0x8A, 0x93, 0xA0),
        "scene" => MakeBrush(0xD8, 0x8A, 0x3C),
        "project" => MakeBrush(0xC9, 0x55, 0x55),
        "material" => MakeBrush(0x5C, 0xC2, 0xC9),
        "blueprint" => MakeBrush(0x6B, 0xD0, 0x8A),
        "prefab" => MakeBrush(0xC2, 0x9A, 0x5A),
        _ => MakeBrush(0x60, 0x68, 0x74),
    };

    private static SolidColorBrush MakeBrush(byte r, byte g, byte b)
    {
        var br = new SolidColorBrush(Color.FromRgb(r, g, b));
        br.Freeze();
        return br;
    }

    // .acsmat のサムネイル (一覧=56)。
    private ImageSource? TryMaterialThumb(string path) => TryMaterialPreview(path, 56);

    // .acsmat を N×N にレンダリング (一覧サムネ=56 / プレビュー=大)。エンジンがあれば実シェーダ GPU、無ければ CPU。
    private ImageSource? TryMaterialPreview(string path, int N)
    {
        try
        {
            // 重要(安定性): GPU プレビュー (acs_editor_render_preview_material = preview_cl の Submit+WaitIdle) が
            // メインレンダーループと競合し、«操作不要で起動後数秒に約70%» エディタを間欠クラッシュさせていた
            // (実測: GPU 経路あり 0〜1/3 安定 → 切ると 4/4 安定)。これがメッシュプレビュー/SSAO/アセット選択の
            // 不安定の正体。よってサムネ/プレビューは «CPU 数式 (MaterialPreview)» で生成する (十分な品質・完全安定)。
            // GPU プレビュー再導入は «preview の submit をメインフレームと安全に直列化» してから (描画オーバーホール)。
            // CPU で生成:
            int kind = EngineInterop.acs_editor_material_kind(path);
            if (kind == 0)
            {
                var bc = new float[4] { 1, 1, 1, 1 };
                var em = new float[3] { 0, 0, 0 };
                var ab = new byte[260]; var nb = new byte[260];
                if (EngineInterop.acs_editor_material_load_pbr(path, bc, out float metallic, out float roughness,
                        em, out float emStr, out float normalStr, out float ao, ab, ab.Length, nb, nb.Length) == 0)
                    return null;
                return MaterialPreview.RenderPbr(bc, metallic, roughness, em, emStr, normalStr, ao, N);
            }
            var color = new float[4] { 1, 1, 1, 1 };
            var nameBuf = new byte[64];
            if (EngineInterop.acs_editor_material_load(path, out int effect, out float strength,
                    out float p0, out float p1, out float p2, color, out int animated, nameBuf, nameBuf.Length) == 0)
                return null;
            return MaterialPreview.Render(effect, strength, p0, p1, p2, color, animated != 0, N);
        }
        catch { return null; }   // DLL 未ロード等は glyph 表示にフォールバック
    }

    private static ImageSource GpuBmp(byte[] bgra, int n)
    {
        var b = BitmapSource.Create(n, n, 96, 96, PixelFormats.Bgra32, null, bgra, n * 4);
        b.Freeze();
        return b;
    }

    private static ImageSource? TryThumb(string path, int decodeW = 64)
    {
        try
        {
            byte[] bytes = File.ReadAllBytes(path);   // ストリームを残さずロード (ファイルロック回避)
            var bmp = new BitmapImage();
            bmp.BeginInit();
            bmp.CacheOption = BitmapCacheOption.OnLoad;   // 即デコードしてストリーム依存を断つ
            bmp.DecodePixelWidth = decodeW;               // サムネ=64 / プレビュー=大
            bmp.StreamSource = new MemoryStream(bytes);
            bmp.EndInit();
            bmp.Freeze();
            return bmp;
        }
        catch { return null; }
    }

    // ===== パスユーティリティ =====
    private string RelDisplay(string fullDir)
    {
        if (_project == null) return fullDir;
        string root = _project.AssetsDir;
        if (PathEquals(fullDir, root)) return "Assets";
        if (fullDir.StartsWith(root, StringComparison.OrdinalIgnoreCase))
            return "Assets" + fullDir.Substring(root.Length).Replace('\\', '/');
        return fullDir;
    }

    private static bool PathEquals(string a, string b) =>
        string.Equals(Path.TrimEndingDirectorySeparator(Path.GetFullPath(a)),
                      Path.TrimEndingDirectorySeparator(Path.GetFullPath(b)),
                      StringComparison.OrdinalIgnoreCase);

    private static string UniquePath(string dst)
    {
        if (!File.Exists(dst)) return dst;
        string dir = Path.GetDirectoryName(dst)!;
        string name = Path.GetFileNameWithoutExtension(dst);
        string ext = Path.GetExtension(dst);
        for (int i = 1; i < 1000; ++i)
        {
            string cand = Path.Combine(dir, $"{name} ({i}){ext}");
            if (!File.Exists(cand)) return cand;
        }
        return dst;
    }
}
